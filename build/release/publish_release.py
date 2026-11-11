#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Publish only a fully verified Kinefin GitHub release, safely on retries.

GitHub's release creation, asset uploads and publication are separate requests.
Keep the release as a draft until all three assets match local SHA256 digests.
This script can resume a draft after an interrupted upload, but never changes
an already-published release. Check the remote tag target against the exact
workflow commit before any write and again immediately before publication.
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
from typing import Callable


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


class ReleasePublishError(Exception):
    """The release must remain unpublished."""


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
        raise ReleasePublishError(f"gh {' '.join(args[:2])} failed: {message}")
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


def get_release(tag: str, repo: str, runner: GhRunner) -> dict:
    output = runner("api", f"repos/{repo}/releases/tags/{tag}")
    try:
        data = json.loads(output)
    except (ValueError, TypeError) as error:
        raise ReleasePublishError("GitHub returned invalid release JSON") from error
    if not isinstance(data, dict) or data.get("tag_name") != tag or type(data.get("draft")) is not bool:
        raise ReleasePublishError("GitHub returned a release with the wrong tag or draft state")
    if not isinstance(data.get("assets"), list):
        raise ReleasePublishError("GitHub release asset list is missing")
    return data


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
) -> str:
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

    # A re-run cannot create the same release twice. Attempt an unpublished
    # draft first; if it exists already, fetch its state and resume safely.
    create_error: ReleasePublishError | None = None
    try:
        runner(
            "release", "create", tag, "--draft", "--verify-tag", "--generate-notes",
            "--title", f"Kinefin {tag[1:]}", "--repo", repo,
        )
    except ReleasePublishError as error:
        create_error = error
    try:
        release = get_release(tag, repo, runner)
    except ReleasePublishError as error:
        if create_error is not None:
            raise ReleasePublishError(
                f"could not create draft ({create_error}) or find existing release ({error})"
            ) from error
        raise

    differences = missing_or_mismatched(release, assets)
    if release["draft"] is False:
        if differences:
            raise ReleasePublishError(
                "published release assets differ from local artifacts; refusing to change published assets"
            )
        return f"Release {tag} already published; all three assets verified"

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

    # Re-check after uploads: a force-pushed tag must not cause publishing a
    # valid bundle against a different revision. Tag protection is still
    # required to avoid a race between this query and the release edit.
    require_release_commit(tag, repo, expected_commit, runner)
    runner("release", "edit", tag, "--draft=false", "--repo", repo)
    final = get_release(tag, repo, runner)
    if final["draft"] is not False or missing_or_mismatched(final, assets):
        raise ReleasePublishError("release publication state or asset digests could not be confirmed")
    return f"Published release {tag}; all three assets match local SHA256 digests"


def main() -> int:
    parser = argparse.ArgumentParser(description="Safely publish a verified GitHub release")
    parser.add_argument("tag", help="tag vX.Y.Z, already pushed and validated by the release workflow")
    parser.add_argument("assets_dir", type=Path, help="directory containing bundle, PROVENANCE.json and SHA256SUMS")
    args = parser.parse_args()
    try:
        message = publish(
            args.tag, args.assets_dir, os.environ.get("GITHUB_REPOSITORY", ""),
            os.environ.get("RELEASE_COMMIT", ""),
            os.environ.get("GITHUB_RUN_ID", ""),
            os.environ.get("RELEASE_IMAGE_DIGEST", ""),
        )
    except (ReleasePublishError, OSError) as error:
        print(f"release-publish: {error}", file=sys.stderr)
        return 1
    print(message)
    return 0


if __name__ == "__main__":
    sys.exit(main())
