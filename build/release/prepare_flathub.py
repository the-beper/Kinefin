#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render a Flathub submission manifest from a verified, clean release tag.

The local/CI manifest intentionally uses a directory source. A Flathub build
needs the public Git source instead, pinned to the exact tagged commit. Neither
this helper nor the release workflow modifies the checked-in local manifest.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
APP_ID = "io.github.the_beper.Kinefin"
MANIFEST = ROOT / "build/flatpak" / f"{APP_ID}.yml"
REPO_URL = "https://github.com/the-beper/Kinefin.git"
TAG_PATTERN = re.compile(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")
COMMIT_PATTERN = re.compile(r"[0-9a-f]{40,64}\Z")
APP_MODULE = "  - name: kinefin\n"
LOCAL_SOURCE_COMMENT = (
    "      # CI and local builds build this checkout (dev scaffold; switched to type: git once tagged).\n"
)

# -I does not put this script's directory on sys.path: use the explicit trusted
# sibling to share the release metadata validator rather than duplicate it.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_release import ReleaseError, validate  # noqa: E402


class FlathubError(Exception):
    """The submission manifest cannot be generated safely."""


def git(*args: str) -> str:
    try:
        process = subprocess.run(
            ["git", *args], cwd=ROOT, capture_output=True, text=True,
            timeout=15, check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise FlathubError(f"could not run git: {error}") from error
    if process.returncode:
        raise FlathubError(f"git {' '.join(args[:2])} failed: {process.stderr.strip()}")
    return process.stdout.strip()


def verify_tag(tag: str) -> str:
    if not TAG_PATTERN.fullmatch(tag):
        raise FlathubError("release tag must be stable vX.Y.Z")
    # Check the real tag object, not a similarly named branch or an implicit
    # expression. An annotated tag resolves to the commit it names.
    tagged_commit = git("rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}")
    head_commit = git("rev-parse", "--verify", "HEAD^{commit}")
    if not COMMIT_PATTERN.fullmatch(tagged_commit) or tagged_commit != head_commit:
        raise FlathubError("release tag must point to the checked-out HEAD commit")
    # This is a source-release tool: no unstaged, staged or untracked source
    # changes may be silently omitted from the public Git source.
    if git("status", "--porcelain", "--untracked-files=normal"):
        raise FlathubError("working tree is not clean; use the tagged release checkout")
    try:
        validate(
            tag,
            (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
            (ROOT / "data" / f"{APP_ID}.metainfo.xml").read_text(encoding="utf-8"),
        )
    except (ReleaseError, OSError, UnicodeError) as error:
        raise FlathubError(f"invalid release metadata: {error}") from error
    return tagged_commit


def render(manifest: str, tag: str, commit: str) -> str:
    """Replace just the Kinefin local source with an immutable tagged Git source.

    Deliberately reject unknown manifest layouts instead of converting a random
    directory source from an embedded dependency or silently dropping new keys.
    """
    if manifest.count(APP_MODULE) != 1:
        raise FlathubError("expected exactly one Kinefin module in the Flatpak manifest")
    before, kinefin = manifest.split(APP_MODULE, 1)
    if kinefin.count(LOCAL_SOURCE_COMMENT) != 1:
        raise FlathubError("local Kinefin source marker is missing or duplicated")
    source_start = kinefin.index(LOCAL_SOURCE_COMMENT)
    next_source = kinefin.find("      - type: file\n", source_start)
    if next_source < 0:
        raise FlathubError("expected the pinned mpvqt file source after the local source")
    local_block = kinefin[source_start:next_source]
    meaningful = [
        line.strip() for line in local_block.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if meaningful != ["- type: dir", "path: ../..", "skip: [.git, dist]"]:
        raise FlathubError("the local app source layout changed; review the conversion")
    if "      - type: dir\n" in before or "      - type: dir\n" in kinefin[next_source:]:
        raise FlathubError("unexpected additional directory source in the manifest")
    git_block = (
        "      # Flathub builds the reviewed tag, never a mutable branch or local files.\n"
        "      - type: git\n"
        f"        url: {REPO_URL}\n"
        f"        tag: {tag}\n"
        f"        commit: {commit}\n"
    )
    return before + APP_MODULE + kinefin[:source_start] + git_block + kinefin[next_source:]


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare a commit-pinned Kinefin Flathub manifest")
    parser.add_argument("tag", help="existing stable release tag, e.g. v1.2.3")
    parser.add_argument("--dry-run", action="store_true", help="print the generated manifest without writing")
    args = parser.parse_args()
    try:
        commit = verify_tag(args.tag)
        converted = render(MANIFEST.read_text(encoding="utf-8"), args.tag, commit)
        if args.dry_run:
            sys.stdout.write(converted)
            return 0
        output = ROOT / "dist/flathub" / args.tag / f"{APP_ID}.yml"
        # Repeatable, but never destroy a manually reviewed/edited artifact.
        if output.exists() and output.read_text(encoding="utf-8") != converted:
            raise FlathubError(f"{output} already exists with different contents; review it before replacing")
        output.parent.mkdir(parents=True, exist_ok=True)
        if not output.exists():
            output.write_text(converted, encoding="utf-8")
    except (FlathubError, OSError, UnicodeError) as error:
        print(f"flathub-manifest: {error}", file=sys.stderr)
        return 1
    print(f"Flathub manifest: {output.relative_to(ROOT)} ({commit})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
