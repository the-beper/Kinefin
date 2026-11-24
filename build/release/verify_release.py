#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail closed if a release tag, CMake version and AppStream history disagree.

The release workflow calls this before pulling a build image or uploading an
artifact. The check also runs locally with no GitHub API, Flatpak or Qt.
"""

from __future__ import annotations

import argparse
from datetime import date
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
APP_ID = "io.github.the_beper.Kinefin"
CMAKE = ROOT / "CMakeLists.txt"
METAINFO = ROOT / "data" / f"{APP_ID}.metainfo.xml"
STABLE_VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")
PROJECT = re.compile(r"^project\(kinefin VERSION ([^\s)]+) LANGUAGES CXX\)$", re.MULTILINE)
RELEASE_DATE = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}\Z")


class ReleaseError(Exception):
    """Invalid or incomplete release metadata."""


def version_tuple(raw: str) -> tuple[int, int, int]:
    if not STABLE_VERSION.fullmatch(raw):
        raise ReleaseError(f"invalid stable release version {raw!r}; expected X.Y.Z without leading zeroes")
    return tuple(int(part) for part in raw.split("."))


def require_newest(tag: str, root: Path = ROOT) -> None:
    """Fail unless tag is among the checkout's local vX.Y.Z tags and is the highest of them."""
    try:
        listing = subprocess.run(
            ["git", "-C", str(root), "tag", "-l", "v*"],
            capture_output=True, text=True, timeout=30, check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ReleaseError(f"could not list the Git tags: {error}") from error
    if listing.returncode:
        raise ReleaseError(f"could not list the Git tags: {listing.stderr.strip() or listing.returncode}")
    versions = {
        name: version_tuple(name[1:]) for name in listing.stdout.splitlines()
        if name.startswith("v") and STABLE_VERSION.fullmatch(name[1:])
    }
    if tag not in versions:
        raise ReleaseError(f"tag {tag} is not among the local tags; the checkout must fetch the tags")
    newest = max(versions, key=versions.__getitem__)
    if versions[tag] != versions[newest]:
        raise ReleaseError(
            f"the newest release tag is {newest}, not {tag}: a release is never made below an existing one "
            "(cut the next version instead)"
        )


def validate(tag: str, cmake: str, metainfo: str) -> tuple[str, str]:
    """Validate source contents; return (version, release date) on success."""
    if not tag.startswith("v"):
        raise ReleaseError("tag must be vX.Y.Z")
    version = tag[1:]
    version_tuple(version)

    matches = PROJECT.findall(cmake)
    if len(matches) != 1:
        raise ReleaseError("expected exactly one project(kinefin VERSION X.Y.Z LANGUAGES CXX) in CMakeLists.txt")
    cmake_version = matches[0]
    version_tuple(cmake_version)
    if cmake_version != version:
        raise ReleaseError(f"tag {tag} does not match CMake version {cmake_version}")

    try:
        component = ET.fromstring(metainfo)
    except ET.ParseError as error:
        raise ReleaseError(f"invalid AppStream XML: {error}") from error
    if (component.tag != "component" or component.get("type") != "desktop-application"
            or len(component.findall("id")) != 1 or component.findtext("id") != APP_ID):
        raise ReleaseError("AppStream XML is not the Kinefin desktop component")
    blocks = component.findall("releases")
    if len(blocks) != 1:
        raise ReleaseError("AppStream must have exactly one releases block")
    releases = blocks[0].findall("release")
    if not releases:
        raise ReleaseError("AppStream has no release entries; run version-bump.sh first")

    seen: set[str] = set()
    previous: tuple[int, int, int] | None = None
    for release in releases:
        entry_version = release.get("version", "")
        number = version_tuple(entry_version)
        if entry_version in seen:
            raise ReleaseError(f"duplicate AppStream release {entry_version}")
        seen.add(entry_version)
        if previous is not None and number >= previous:
            raise ReleaseError("AppStream release history must be in descending version order")
        previous = number
        released = release.get("date", "")
        if not RELEASE_DATE.fullmatch(released):
            raise ReleaseError(f"AppStream release {entry_version} has an invalid date: {released!r}")
        try:
            date.fromisoformat(released)
        except ValueError as error:
            raise ReleaseError(f"AppStream release {entry_version} has an invalid calendar date") from error

    newest = releases[0]
    if newest.get("version") != version:
        raise ReleaseError(
            f"newest AppStream release {newest.get('version')} does not match tag {tag}; "
            "run version-bump.sh and commit both files"
        )
    return version, newest.attrib["date"]


def main() -> int:
    parser = argparse.ArgumentParser(description="Check release tag against CMake and AppStream metadata")
    parser.add_argument("tag", help="release Git tag, e.g. v1.2.3")
    parser.add_argument("--require-newest", action="store_true", help="also require the tag to be the highest local vX.Y.Z tag")
    args = parser.parse_args()
    try:
        version, released = validate(
            args.tag,
            CMAKE.read_text(encoding="utf-8"),
            METAINFO.read_text(encoding="utf-8"),
        )
        if args.require_newest:
            require_newest(args.tag)
    except (ReleaseError, OSError, UnicodeError) as error:
        print(f"release-verify: {error}", file=sys.stderr)
        return 1
    print(f"Release v{version} verified: CMake and AppStream match (date {released})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
