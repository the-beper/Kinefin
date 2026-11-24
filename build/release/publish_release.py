#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Publish only a fully verified Kinefin GitHub release, safely on retries.

GitHub's release creation, asset uploads and publication are separate requests.
Keep the release as a draft until all three assets match local SHA256 digests
and Pages serves the new Flatpak repository (publish_site.py pushes it to
gh-pages first). A 0.x release is a pre-release and never the latest release;
from 1.0.0 on it is the latest. This script can resume a draft after an
interrupted upload or Pages wait, and finishes the site and the flags of a
release that was published by hand, but never changes the assets of a published
release. A tag older than a published release is refused before anything is
written, so a re-run of an old run cannot roll the repository back. Check the
remote tag target against the exact workflow commit before any write and again
immediately before publication.

Exit status: 0 published; 1 failure (the release stays a draft, unless it was
already published or the failure came after the publication edit); 3 gh-pages
moved since the site was built (re-run all jobs).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from typing import Callable, Mapping

# Python -I does not include the script's directory on sys.path
sys.path.append(str(Path(__file__).resolve().parent))
import publish_site  # noqa: E402


BUNDLE = "Kinefin-x86_64.flatpak"
PROVENANCE = "PROVENANCE.json"
SUMS = "SHA256SUMS"
ASSET_NAMES = (BUNDLE, PROVENANCE, SUMS)
TAG_RE = re.compile(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")
REPO_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*\Z")
SUMS_RE = re.compile(
    rf"([0-9a-f]{{64}})  {re.escape(BUNDLE)}\n"
    rf"([0-9a-f]{{64}})  {re.escape(PROVENANCE)}\n\Z"
)
IMAGE_RE = re.compile(r"ghcr\.io/the-beper/kinefin-flatpak@sha256:[0-9a-f]{64}\Z")
RUN_RE = re.compile(r"[1-9][0-9]*\Z")
COMMIT_RE = re.compile(r"[0-9a-f]{40}\Z")
SITE_URL_RE = re.compile(r"https://[A-Za-z0-9][A-Za-z0-9.-]*(?::[0-9]+)?(?:/[A-Za-z0-9._~-]+)*\Z")
LIST_WAIT_SECONDS = 60
LIST_POLL_SECONDS = 3


class ReleasePublishError(Exception):
    """The release must remain unpublished."""


class GitHubNotFound(ReleasePublishError):
    """GitHub answered HTTP 404."""


@dataclass(frozen=True)
class Asset:
    path: Path
    digest: str
    size: int


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked_object(value: object, keys: set[str], description: str) -> dict:
    if not isinstance(value, dict) or set(value) != keys:
        raise ReleasePublishError(f"invalid {description} fields")
    return value


def strict_json(path: Path) -> dict:
    def no_duplicates(pairs: list[tuple[str, object]]) -> dict:
        result: dict = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key!r}")
            result[key] = value
        return result

    try:
        if path.stat().st_size > 16 * 1024:
            raise ReleasePublishError("PROVENANCE.json is unexpectedly large")
        data = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=no_duplicates)
    except (ValueError, UnicodeError, OSError) as error:
        raise ReleasePublishError(f"invalid PROVENANCE.json: {error}") from error
    return checked_object(
        data, {"schema_version", "repository", "source", "build", "artifact"}, "provenance",
    )


def validate_provenance(
    path: Path, bundle: Asset, tag: str, repo: str, commit: str, run_id: str, image_digest: str,
) -> None:
    metadata = strict_json(path)
    if type(metadata["schema_version"]) is not int or metadata["schema_version"] != 1:
        raise ReleasePublishError("unsupported provenance schema version")
    if metadata["repository"] != repo:
        raise ReleasePublishError("provenance repository does not match release repository")
    source = checked_object(metadata["source"], {"tag", "commit"}, "provenance source")
    if source != {"tag": tag, "commit": commit}:
        raise ReleasePublishError("provenance tag or commit does not match release workflow")
    builder = checked_object(metadata["build"], {"github_run_id", "image"}, "provenance build")
    if builder["github_run_id"] != run_id or builder["image"] != f"ghcr.io/the-beper/kinefin-flatpak@{image_digest}":
        raise ReleasePublishError("provenance run ID or image differs from the build job")
    if (not isinstance(builder["github_run_id"], str)
            or not RUN_RE.fullmatch(builder["github_run_id"])
            or not isinstance(builder["image"], str)
            or not IMAGE_RE.fullmatch(builder["image"])
            or builder["image"].endswith("0" * 64)):
        raise ReleasePublishError("invalid build provenance run ID or pinned image")
    artifact = checked_object(metadata["artifact"], {"name", "sha256", "size"}, "provenance artifact")
    if (artifact["name"] != BUNDLE or artifact["sha256"] != bundle.digest
            or type(artifact["size"]) is not int or artifact["size"] != bundle.size):
        raise ReleasePublishError("provenance bundle identity, hash or size differs from uploaded bundle")


def local_assets(
    directory: Path, tag: str, repo: str, commit: str, run_id: str, image_digest: str,
) -> dict[str, Asset]:
    assets: dict[str, Asset] = {}
    for name in ASSET_NAMES:
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ReleasePublishError(f"missing or symlinked release asset: {path}")
        size = path.stat().st_size
        if not size:
            raise ReleasePublishError(f"empty release asset: {path}")
        assets[name] = Asset(path=path, digest=hash_file(path), size=size)

    if assets[SUMS].size > 1024:
        raise ReleasePublishError("SHA256SUMS is unexpectedly large")
    try:
        text = assets[SUMS].path.read_text(encoding="ascii")
    except (UnicodeError, OSError) as error:
        raise ReleasePublishError(f"invalid SHA256SUMS: {error}") from error
    checksum = SUMS_RE.fullmatch(text)
    if checksum is None:
        raise ReleasePublishError("SHA256SUMS must contain the two canonical artifact checksums")
    if checksum.group(1) != assets[BUNDLE].digest or checksum.group(2) != assets[PROVENANCE].digest:
        raise ReleasePublishError("Flatpak bundle or provenance differs from SHA256SUMS")
    validate_provenance(assets[PROVENANCE].path, assets[BUNDLE], tag, repo, commit, run_id, image_digest)
    return assets


def gh(*args: str) -> str:
    try:
        process = subprocess.run(
            ["gh", *args], check=False, capture_output=True, text=True, timeout=180,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ReleasePublishError(f"could not run GitHub CLI: {error}") from error
    if process.returncode:
        message = process.stderr.strip() or f"exit status {process.returncode}"
        # gh prints "gh: Not Found (HTTP 404)" and exits 1
        error = GitHubNotFound if "(HTTP 404)" in message else ReleasePublishError
        raise error(f"gh {' '.join(args[:2])} failed: {message}")
    return process.stdout


GhRunner = Callable[..., str]


def github_json(runner: GhRunner, endpoint: str, description: str) -> dict:
    try:
        data = json.loads(runner("api", endpoint))
    except (ValueError, TypeError) as error:
        raise ReleasePublishError(f"GitHub returned invalid {description} JSON") from error
    if not isinstance(data, dict):
        raise ReleasePublishError(f"GitHub returned an invalid {description}")
    return data


def remote_tag_commit(tag: str, repo: str, runner: GhRunner) -> str:
    """Resolve lightweight and annotated GitHub tag refs, never branch names.

    git/ref/tags/<tag> returns the ref object; annotated tags have an object
    of type 'tag', which must be dereferenced via git/tags/<sha> until a
    commit is reached. GitHub release target_commitish is not authoritative
    for an existing tag, so it is intentionally not used here.
    """
    ref = github_json(runner, f"repos/{repo}/git/ref/tags/{tag}", "tag reference")
    if ref.get("ref") != f"refs/tags/{tag}":
        raise ReleasePublishError("GitHub tag reference does not match the release tag")

    target = ref.get("object")
    visited: set[str] = set()
    for _ in range(8):
        if not isinstance(target, dict):
            raise ReleasePublishError("GitHub tag reference is missing its object")
        kind = target.get("type")
        sha = target.get("sha")
        if not isinstance(sha, str) or not COMMIT_RE.fullmatch(sha) or sha == "0" * 40:
            raise ReleasePublishError("GitHub tag reference has an invalid object SHA")
        if kind == "commit":
            return sha
        if kind != "tag":
            raise ReleasePublishError(f"GitHub tag points to unsupported object type: {kind!r}")
        if sha in visited:
            raise ReleasePublishError("GitHub annotated tag contains a reference cycle")
        visited.add(sha)
        annotation = github_json(runner, f"repos/{repo}/git/tags/{sha}", "annotated tag")
        if annotation.get("sha") != sha:
            raise ReleasePublishError("GitHub annotated tag object SHA does not match the request")
        target = annotation.get("object")
    raise ReleasePublishError("GitHub annotated tag chain is too deep")


def require_release_commit(tag: str, repo: str, expected_commit: str, runner: GhRunner) -> None:
    actual = remote_tag_commit(tag, repo, runner)
    if actual != expected_commit:
        raise ReleasePublishError(
            f"remote release tag {tag} points to {actual}, "
            f"not the workflow commit {expected_commit}; refusing publication"
        )


def is_prerelease(tag: str) -> bool:
    return int(tag[1:].split(".")[0]) == 0


def list_releases(repo: str, runner: GhRunner) -> list:
    """The paginated release list: the by-tag endpoint does not show drafts."""
    output = runner("api", "--paginate", f"repos/{repo}/releases?per_page=100")
    # gh prints one JSON array per page, back to back
    decoder = json.JSONDecoder()
    releases: list = []
    position = 0
    try:
        while True:
            while position < len(output) and output[position].isspace():
                position += 1
            if position >= len(output):
                break
            page, position = decoder.raw_decode(output, position)
            if not isinstance(page, list):
                raise ValueError("page is not a list")
            releases.extend(page)
    except (ValueError, TypeError) as error:
        raise ReleasePublishError("GitHub returned invalid release list JSON") from error
    return releases


def pick_release(tag: str, releases: list) -> dict | None:
    found = [release for release in releases if isinstance(release, dict) and release.get("tag_name") == tag]
    if len(found) > 1:
        raise ReleasePublishError(f"{len(found)} releases have the tag {tag}; manual review required")
    if not found:
        return None
    release = found[0]
    if type(release.get("draft")) is not bool or type(release.get("prerelease")) is not bool:
        raise ReleasePublishError("GitHub returned a release without a draft or pre-release state")
    if not isinstance(release.get("assets"), list):
        raise ReleasePublishError("GitHub release asset list is missing")
    return release


def find_release(tag: str, repo: str, runner: GhRunner) -> dict | None:
    return pick_release(tag, list_releases(repo, runner))


def tag_version(name: object) -> tuple[int, ...] | None:
    if not isinstance(name, str) or not TAG_RE.fullmatch(name):
        return None
    return tuple(int(part) for part in name[1:].split("."))


def require_newest(tag: str, releases: list) -> None:
    """Refuse a tag older than a published release: a re-run of the failed jobs of an old run would put its version on top of the current repository and offer it to every client."""
    mine = tag_version(tag)
    published = [
        release["tag_name"] for release in releases
        if isinstance(release, dict) and release.get("draft") is not True and tag_version(release.get("tag_name")) is not None
    ]
    newer = [name for name in published if tag_version(name) > mine]
    if newer:
        raise ReleasePublishError(
            f"{tag} is older than the published release {max(newer, key=tag_version)}: publishing it would roll the repository back; cut the next version instead"
        )


def get_release(tag: str, repo: str, runner: GhRunner) -> dict:
    release = find_release(tag, repo, runner)
    if release is None:
        raise ReleasePublishError(f"release {tag} is not in the release list")
    return release


def wait_for_release(tag: str, repo: str, runner: GhRunner, sleep: Callable[[float], None]) -> dict:
    """A draft that was just created can take a moment to show up in the list."""
    polls = LIST_WAIT_SECONDS // LIST_POLL_SECONDS
    for poll in range(polls + 1):
        release = find_release(tag, repo, runner)
        if release is not None:
            return release
        if poll < polls:
            sleep(LIST_POLL_SECONDS)
    raise ReleasePublishError(f"the new draft {tag} is not in the release list after {LIST_WAIT_SECONDS}s")


def require_latest_state(tag: str, repo: str, prerelease: bool, runner: GhRunner) -> None:
    """A 0.x release must not be the latest release (none at all is fine); from 1.0.0 on it must be."""
    try:
        latest = github_json(runner, f"repos/{repo}/releases/latest", "latest release").get("tag_name")
    except GitHubNotFound:
        latest = None
    if prerelease and latest == tag:
        raise ReleasePublishError(f"pre-release {tag} is the latest release")
    if not prerelease and latest != tag:
        raise ReleasePublishError(f"the latest release is {latest}, not {tag}")


def missing_or_mismatched(release: dict, assets: dict[str, Asset]) -> list[str]:
    found: dict[str, dict] = {}
    for asset in release["assets"]:
        if not isinstance(asset, dict) or not isinstance(asset.get("name"), str):
            raise ReleasePublishError("GitHub returned an invalid asset entry")
        name = asset["name"]
        if name not in assets:
            raise ReleasePublishError(f"unexpected release asset {name!r}; manual review required")
        if name in found:
            raise ReleasePublishError(f"duplicate remote release asset {name!r}")
        found[name] = asset
    return [
        name for name, expected in assets.items()
        if name not in found
        or found[name].get("digest") != f"sha256:{expected.digest}"
        or type(found[name].get("size")) is not int
        or found[name]["size"] != expected.size
        or found[name].get("state") != "uploaded"
    ]


def publish(
    tag: str, directory: Path, repo: str, expected_commit: str,
    expected_run_id: str, expected_image_digest: str, runner: GhRunner = gh,
    *, site: Callable[[], None], sleep: Callable[[float], None] = time.sleep,
) -> str:
    """Verify the assets, run site() (push gh-pages, wait for Pages), then publish; until then the release is a draft."""
    if not TAG_RE.fullmatch(tag):
        raise ReleasePublishError("release tag must be stable vX.Y.Z")
    if not REPO_RE.fullmatch(repo) or ".." in repo or repo.endswith("."):
        raise ReleasePublishError("GITHUB_REPOSITORY must be a valid owner/repository")
    if not isinstance(expected_commit, str) or not COMMIT_RE.fullmatch(expected_commit) or expected_commit == "0" * 40:
        raise ReleasePublishError("RELEASE_COMMIT must be a full, nonzero lowercase Git commit SHA")
    if not isinstance(expected_run_id, str) or not RUN_RE.fullmatch(expected_run_id):
        raise ReleasePublishError("GITHUB_RUN_ID must be a positive decimal run ID")
    if (not isinstance(expected_image_digest, str)
            or not re.fullmatch(r"sha256:[0-9a-f]{64}", expected_image_digest)
            or expected_image_digest == "sha256:" + "0" * 64):
        raise ReleasePublishError("RELEASE_IMAGE_DIGEST must be an immutable SHA256 digest")
    assets = local_assets(directory, tag, repo, expected_commit, expected_run_id, expected_image_digest)
    # The server's tag ref may have changed since the workflow was triggered.
    # Refuse even creating a draft if the released tag is not the built commit.
    require_release_commit(tag, repo, expected_commit, runner)

    prerelease = is_prerelease(tag)
    # A re-run resumes the release it finds; GitHub may accept a second draft for the same tag, so create only when there is none
    releases = list_releases(repo, runner)
    require_newest(tag, releases)
    release = pick_release(tag, releases)
    if release is None:
        try:
            runner(
                "release", "create", tag, "--draft", "--verify-tag", "--generate-notes",
                *(["--prerelease"] if prerelease else []),
                "--title", f"Kinefin {tag[1:]}", "--repo", repo,
            )
        except ReleasePublishError as error:
            raise ReleasePublishError(f"could not create draft: {error}") from error
        release = wait_for_release(tag, repo, runner, sleep)

    differences = missing_or_mismatched(release, assets)
    if release["draft"] is False:
        if differences:
            raise ReleasePublishError(
                "published release assets differ from local artifacts; refusing to change published assets"
            )
        outcome = f"Release {tag} was already published; all three assets verified"
    else:
        if differences:
            runner(
                "release", "upload", tag,
                *(str(assets[name].path) for name in differences),
                "--clobber", "--repo", repo,
            )

        # GitHub computes immutable SHA256 digests for uploaded release assets.
        # Do not publish if any digest is missing, differs or is still processing.
        checked = get_release(tag, repo, runner)
        if checked["draft"] is not True:
            raise ReleasePublishError("release became published before asset verification")
        unverified = missing_or_mismatched(checked, assets)
        if unverified:
            raise ReleasePublishError(f"release remains draft: unverified assets: {', '.join(unverified)}")
        outcome = f"Published release {tag}; all three assets match local SHA256 digests"

    # Nothing goes to gh-pages for a tag that moved during the uploads
    require_release_commit(tag, repo, expected_commit, runner)
    # A release that was published by hand gets its site too
    site()

    # Re-check after uploads and the Pages wait: a force-pushed tag must not cause publishing a valid bundle against a different revision; tag protection is still required to avoid a race between this query and the release edit
    require_release_commit(tag, repo, expected_commit, runner)
    # The wait is long: an asset replaced meanwhile must not go public
    before_edit = get_release(tag, repo, runner)
    if before_edit["draft"] is not release["draft"] or missing_or_mismatched(before_edit, assets):
        raise ReleasePublishError("the release changed while the repository was being published; refusing publication")
    runner(
        "release", "edit", tag, "--draft=false",
        f"--prerelease={str(prerelease).lower()}", f"--latest={str(not prerelease).lower()}", "--repo", repo,
    )
    final = get_release(tag, repo, runner)
    if final["draft"] is not False or final["prerelease"] is not prerelease or missing_or_mismatched(final, assets):
        raise ReleasePublishError("release publication state or asset digests could not be confirmed")
    require_latest_state(tag, repo, prerelease, runner)
    kind = "a pre-release, not the latest release" if prerelease else "the latest release"
    return f"{outcome}; Pages serves the repository; {tag} is {kind}"


def site_step(tag: str, environ: Mapping[str, str], repo: str, scratch: Path) -> Callable[[], None]:
    """Validate the site inputs now, before any request, and return the step that pushes gh-pages and waits for Pages."""
    archive = Path(environ.get("SITE_TAR", ""))
    expected = environ.get("EXPECT_SITE_SHA256", "")
    base = environ.get("SITE_BASE_SHA", "")
    url = environ.get("SITE_URL", "")
    remote = environ.get("REPO_REMOTE") or f"https://github.com/{repo}.git"
    if not environ.get("SITE_TAR") or archive.is_symlink() or not archive.is_file():
        raise ReleasePublishError("SITE_TAR must be the site archive of the finish job")
    if not publish_site.DIGEST_RE.fullmatch(expected):
        raise ReleasePublishError("EXPECT_SITE_SHA256 must be a lowercase SHA-256 digest")
    if hash_file(archive) != expected:
        raise ReleasePublishError("the site archive is not the one the finish job produced")
    if base != "-" and not COMMIT_RE.fullmatch(base):
        raise ReleasePublishError("SITE_BASE_SHA must be - or a full commit id")
    if not SITE_URL_RE.fullmatch(url):
        raise ReleasePublishError("SITE_URL must be an https URL without a trailing slash")
    if not publish_site.REMOTE_RE.fullmatch(remote):
        raise ReleasePublishError("REPO_REMOTE must be an https or file URL")
    site_dir = scratch / "site"

    def step() -> None:
        publish_site.publish(archive, expected, None if base == "-" else base, remote, tag, site_dir)
        publish_site.wait_served(site_dir, url)

    return step


def main() -> int:
    parser = argparse.ArgumentParser(description="Safely publish a verified GitHub release")
    parser.add_argument("tag", help="tag vX.Y.Z, already pushed and validated by the release workflow")
    parser.add_argument("assets_dir", type=Path, help="directory containing bundle, PROVENANCE.json and SHA256SUMS")
    args = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix="kinefin-release.") as scratch:
            repo = os.environ.get("GITHUB_REPOSITORY", "")
            message = publish(
                args.tag, args.assets_dir, repo,
                os.environ.get("RELEASE_COMMIT", ""),
                os.environ.get("GITHUB_RUN_ID", ""),
                os.environ.get("RELEASE_IMAGE_DIGEST", ""),
                site=site_step(args.tag, os.environ, repo, Path(scratch)),
            )
    except publish_site.StaleBase as error:
        print(f"release-publish: {error}", file=sys.stderr)
        return 3
    except (ReleasePublishError, publish_site.SitePublishError, OSError) as error:
        print(f"release-publish: {error}", file=sys.stderr)
        return 1
    print(message)
    return 0


if __name__ == "__main__":
    sys.exit(main())
