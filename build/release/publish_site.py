# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Push the signed Flatpak repository site to gh-pages and wait until Pages serves it.

publish_release.py calls this between the verified assets and the end of the
draft state. The site archive is checked against the digest the finish job
produced and unpacked with an allowlist of member types. The new tree goes to
gh-pages as one orphan commit; the old head is kept as gh-pages-previous in the
same atomic push, each branch behind a lease on the value read just before.
"""

from __future__ import annotations

import base64
from collections.abc import Callable, Mapping, Sequence
import hashlib
import http.client
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile
import time
import urllib.request


COMMIT_RE = re.compile(r"[0-9a-f]{40}\Z")
DIGEST_RE = re.compile(r"[0-9a-f]{64}\Z")
TAG_RE = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+\Z")
REMOTE_RE = re.compile(r"(?:https|file)://[^\s?#]+\Z")
# Names git itself interprets in a work tree
GIT_NAMES = {".git", ".gitattributes", ".gitmodules"}
SERVED_FILES = ("repo/summary", "repo/summary.sig")
BRANCH = "gh-pages"
PREVIOUS_BRANCH = "gh-pages-previous"
BOT = {"NAME": "github-actions", "EMAIL": "github-actions@users.noreply.github.com"}
MAX_SERVED_BYTES = 16 * 1024 * 1024


class SitePublishError(Exception):
    """The site was not published or is not served; the release must stay a draft."""


class StaleBase(SitePublishError):
    """gh-pages moved since the site was built: nothing was pushed, build again."""


GitRunner = Callable[..., str]
Fetch = Callable[[str], bytes]


def _subcommand(args: Sequence[str]) -> str:
    skip = False
    for arg in args:
        if skip:
            skip = False
        elif arg == "-c":
            skip = True
        elif not arg.startswith("-"):
            return arg
    return "?"


def run_git(*args: str, env: Mapping[str, str] | None = None) -> str:
    """Run git with extra environment variables; stdout on success."""
    try:
        process = subprocess.run(
            ["git", *args], check=False, capture_output=True, text=True, timeout=600,
            env={**os.environ, **env} if env else None,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SitePublishError(f"could not run git {_subcommand(args)}: {error}") from error
    if process.returncode:
        message = process.stderr.strip() or f"exit status {process.returncode}"
        raise SitePublishError(f"git {_subcommand(args)} failed: {message}")
    return process.stdout


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def unpack_site(site_tar: Path, site_dir: Path) -> None:
    """Extract regular files and directories only, with no path that could leave site_dir or reach .git."""
    if site_dir.exists() and (not site_dir.is_dir() or any(site_dir.iterdir())):
        raise SitePublishError(f"{site_dir} must be an empty directory")
    site_dir.mkdir(parents=True, exist_ok=True)
    try:
        with tarfile.open(site_tar, "r:") as archive:
            members = archive.getmembers()
            seen: set[PurePosixPath] = set()
            for member in members:
                path = PurePosixPath(member.name)
                if (path.is_absolute() or "\\" in member.name
                        or any(part == ".." or part.lower() in GIT_NAMES for part in path.parts)):
                    raise SitePublishError(f"the site archive has an unsafe path: {member.name!r}")
                if not (member.isreg() or member.isdir()):
                    raise SitePublishError(f"the site archive has a member that is neither file nor directory: {member.name!r}")
                if path in seen:
                    raise SitePublishError(f"the site archive lists {member.name!r} twice")
                seen.add(path)
            archive.extractall(site_dir, members=members, filter="data")
    except (tarfile.TarError, OSError) as error:
        raise SitePublishError(f"could not unpack the site archive: {error}") from error
    for name in SERVED_FILES:
        path = site_dir / name
        if path.is_symlink() or not path.is_file():
            raise SitePublishError(f"the site has no {name}")


def git_environment(remote: str) -> dict[str, str]:
    """Environment of the git commands that talk to the remote: credentials never reach argv, a file or a log."""
    env = {"GIT_TERMINAL_PROMPT": "0"}
    token = os.environ.get("GH_TOKEN", "")
    if token and remote.startswith("https://"):
        auth = base64.b64encode(f"x-access-token:{token}".encode()).decode()
        if os.environ.get("GITHUB_ACTIONS") == "true":
            print(f"::add-mask::{auth}")
        env.update({
            "GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_KEY_0": "http.https://github.com/.extraheader",
            "GIT_CONFIG_VALUE_0": f"AUTHORIZATION: basic {auth}",
        })
    return env


def read_branches(git: GitRunner, repo: str, remote: str, env: Mapping[str, str]) -> tuple[str, str]:
    """Heads of gh-pages and gh-pages-previous on the remote, empty when absent."""
    heads = {BRANCH: "", PREVIOUS_BRANCH: ""}
    output = git(repo, "ls-remote", remote, f"refs/heads/{BRANCH}", f"refs/heads/{PREVIOUS_BRANCH}", env=env)
    for line in output.splitlines():
        sha, _, ref = line.partition("\t")
        name = ref.removeprefix("refs/heads/")
        if ref.startswith("refs/heads/") and name in heads:
            if not COMMIT_RE.fullmatch(sha):
                raise SitePublishError(f"the remote reported an invalid commit for {ref}")
            heads[name] = sha
    return heads[BRANCH], heads[PREVIOUS_BRANCH]


def publish(
    site_tar: Path, expected_sha256: str, base: str | None, remote: str, tag: str, site_dir: Path,
    git: GitRunner = run_git,
) -> str:
    """Put the site on gh-pages unless it is already there and return the gh-pages commit; base is the gh-pages commit the site was built on (None or "-" for none)"""
    if not DIGEST_RE.fullmatch(expected_sha256):
        raise SitePublishError("EXPECT_SITE_SHA256 must be a lowercase SHA-256 digest")
    if not REMOTE_RE.fullmatch(remote):
        raise SitePublishError("the remote must be an https or file URL")
    if base == "-":
        base = None
    if base is not None and not COMMIT_RE.fullmatch(base):
        raise SitePublishError("the site base must be - or a full commit id")
    if not TAG_RE.fullmatch(tag):
        raise SitePublishError("release tag must be vX.Y.Z")
    if hash_file(site_tar) != expected_sha256:
        raise SitePublishError("the site archive is not the one the finish job produced")
    unpack_site(site_tar, site_dir)

    env = git_environment(remote)
    with tempfile.TemporaryDirectory(prefix="kinefin-pages.") as scratch:
        repo = f"--git-dir={scratch}/pages.git"
        git("init", "--quiet", "--bare", f"{scratch}/pages.git")
        # No global setting may change the committed bytes
        git(
            repo, f"--work-tree={site_dir}", "-c", "core.excludesFile=/dev/null", "-c", "core.attributesFile=/dev/null",
            "-c", "core.autocrlf=false", "add", "--force", "--all",
        )
        site_tree = git(repo, "write-tree").strip()
        remote_head, remote_previous = read_branches(git, repo, remote, env)
        remote_tree = ""
        if remote_head:
            git(repo, "fetch", "--quiet", remote, f"refs/heads/{BRANCH}", env=env)
            if git(repo, "rev-parse", "FETCH_HEAD").strip() != remote_head:
                raise StaleBase(f"{BRANCH} moved while it was being read; nothing was pushed, build again")
            remote_tree = git(repo, "rev-parse", "FETCH_HEAD^{tree}").strip()

        if remote_tree == site_tree:
            print(f"publish-site: {BRANCH} already has this site (tree {site_tree}), no push")
            return remote_head
        if remote_head != (base or ""):
            raise StaleBase(
                f"{BRANCH} is at {remote_head or 'nothing'}, not at the base {base or 'nothing'} the site was built on, "
                "and it is not the new site: another release got there first; nothing was pushed, build again"
            )

        identity = {f"GIT_{role}_{field}": value for role in ("AUTHOR", "COMMITTER") for field, value in BOT.items()}
        commit = git(repo, "commit-tree", site_tree, "-m", f"Flatpak repository for {tag}", env=identity).strip()
        if remote_head:
            print(f"publish-site: keeping the old {BRANCH} ({remote_head}) as {PREVIOUS_BRANCH}")
            # One atomic push, both branches or neither, each behind a lease on the value read above
            push = [
                "push", "--quiet", "--atomic",
                f"--force-with-lease=refs/heads/{BRANCH}:{remote_head}",
                f"--force-with-lease=refs/heads/{PREVIOUS_BRANCH}:{remote_previous}",
                remote, f"{commit}:refs/heads/{BRANCH}", f"{remote_head}:refs/heads/{PREVIOUS_BRANCH}",
            ]
        else:
            push = ["push", "--quiet", f"--force-with-lease=refs/heads/{BRANCH}:", remote, f"{commit}:refs/heads/{BRANCH}"]
        try:
            git(repo, *push, env=env)
        except SitePublishError as error:
            settle_failed_push(git, repo, remote, env, commit, remote_head, remote_previous, error)
    print(f"publish-site: {BRANCH} is now {commit}")
    return commit


def settle_failed_push(
    git: GitRunner, repo: str, remote: str, env: Mapping[str, str], commit: str,
    old_head: str, old_previous: str, error: SitePublishError,
) -> None:
    """Return when the push had landed and only its acknowledgement was lost; raise otherwise."""
    try:
        head, previous = read_branches(git, repo, remote, env)
    except SitePublishError as reread:
        raise SitePublishError(f"the push to {BRANCH} failed ({error}) and the branches cannot be read again: {reread}") from error
    # A first publication leaves whatever gh-pages-previous was there
    if head == commit and previous == (old_head or old_previous):
        print(f"publish-site: the push had landed ({BRANCH} is {commit}), only its acknowledgement was lost")
        return
    if head != old_head or previous != old_previous:
        raise StaleBase(
            f"the push to {BRANCH} failed because another writer changed {BRANCH} or {PREVIOUS_BRANCH}; "
            "nothing was pushed, build again"
        ) from error
    raise SitePublishError(
        f"the push to {BRANCH} failed and neither branch has moved ({error}): nothing was pushed, run this job again"
    ) from error


def fetch_https(url: str) -> bytes:
    if not url.startswith("https://"):
        raise SitePublishError("the site URL must be https")
    request = urllib.request.Request(url, headers={"Cache-Control": "no-cache"})
    with urllib.request.urlopen(request, timeout=30) as response:  # noqa: S310 (https only, checked above)
        if not response.geturl().startswith("https://"):
            raise SitePublishError("the site URL redirected to a URL that is not https")
        return response.read(MAX_SERVED_BYTES + 1)


def wait_served(
    site_dir: Path, site_url: str, timeout: float = 600, interval: float = 15,
    fetch: Fetch = fetch_https, sleep: Callable[[float], None] = time.sleep,
    clock: Callable[[], float] = time.monotonic,
) -> None:
    """Poll until Pages serves the local summary and its signature byte for byte."""
    expected = {}
    for name in SERVED_FILES:
        path = site_dir / name
        if not path.is_file():
            raise SitePublishError(f"the site has no {name}")
        expected[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    deadline = clock() + timeout
    while True:
        stale = []
        for name, digest in expected.items():
            try:
                data = fetch(f"{site_url.rstrip('/')}/{name}?nocache={int(time.time())}")
            except (OSError, http.client.HTTPException):
                data = b""
            if len(data) > MAX_SERVED_BYTES or hashlib.sha256(data).hexdigest() != digest:
                stale.append(name)
        if not stale:
            print("publish-site: Pages serves the new repository")
            return
        if clock() >= deadline:
            raise SitePublishError(
                f"Pages does not serve the new {', '.join(stale)} at {site_url} after {timeout:g}s: "
                "the release stays a draft; run this job again when Pages is up"
            )
        sleep(interval)
