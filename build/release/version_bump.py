#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare a stable release version without changing unrelated build pins.

Usage: build/release/version-bump.sh [--date YYYY-MM-DD] [--dry-run] X.Y.Z

CMake's project version is the single source for the binary, Flatpak bundle and
release.yml tag check. AppStream keeps the release history for software centers.
"""

from __future__ import annotations

import argparse
from datetime import date, datetime, timezone
import difflib
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
CMAKE = ROOT / "CMakeLists.txt"
METAINFO = ROOT / "data/io.github.the_beper.Kinefin.metainfo.xml"
VERSION_RE = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")
PROJECT_RE = re.compile(r"^project\(kinefin VERSION ([0-9.]+) LANGUAGES CXX\)$", re.MULTILINE)
RELEASE_OPEN = "  <releases>\n"


class BumpError(Exception):
    """Invalid release request or unsupported source file layout."""


def parse_version(version: str) -> tuple[int, int, int]:
    if not VERSION_RE.fullmatch(version):
        raise BumpError("version must be X.Y.Z (three non-negative integers, no leading zeros or v prefix)")
    return tuple(int(piece) for piece in version.split("."))


def parse_date(value: str) -> str:
    if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", value):
        raise BumpError("date must be YYYY-MM-DD")
    try:
        date.fromisoformat(value)
    except ValueError as error:
        raise BumpError("date is not a valid calendar date") from error
    return value


def updated_contents(cmake: str, metainfo: str, version: str, released: str) -> tuple[str, str, str]:
    new_number = parse_version(version)
    parse_date(released)

    project_lines = list(PROJECT_RE.finditer(cmake))
    if len(project_lines) != 1:
        raise BumpError("expected exactly one project(kinefin VERSION X.Y.Z LANGUAGES CXX) line")
    current = project_lines[0].group(1)
    if new_number <= parse_version(current):
        raise BumpError(f"new version {version} must be greater than current version {current}")
    new_cmake = cmake[: project_lines[0].start(1)] + version + cmake[project_lines[0].end(1) :]

    try:
        root = ET.fromstring(metainfo)
    except ET.ParseError as error:
        raise BumpError(f"invalid AppStream XML: {error}") from error
    if root.tag != "component" or root.findtext("id") != "io.github.the_beper.Kinefin":
        raise BumpError("AppStream XML is not the Kinefin component")
    sections = root.findall("releases")
    if len(sections) > 1:
        raise BumpError("AppStream XML has more than one releases section")
    if sections:
        known = set()
        for entry in sections[0].findall("release"):
            existing = entry.get("version", "")
            known_number = parse_version(existing)
            if existing in known:
                raise BumpError(f"duplicate AppStream release {existing}")
            known.add(existing)
            if known_number >= new_number:
                raise BumpError(f"AppStream already has release {existing} newer than or equal to {version}")

    entry = f'    <release version="{version}" date="{released}" />\n'
    if sections:
        if metainfo.count(RELEASE_OPEN) != 1:
            raise BumpError("unsupported AppStream releases layout; expected a two-space indented <releases> line")
        new_meta = metainfo.replace(RELEASE_OPEN, RELEASE_OPEN + entry, 1)
    else:
        anchor = "  <content_rating type=\"oars-1.1\"/>\n"
        if metainfo.count(anchor) != 1:
            raise BumpError("cannot insert AppStream releases: content_rating anchor not found")
        block = RELEASE_OPEN + entry + "  </releases>\n"
        new_meta = metainfo.replace(anchor, block + anchor, 1)
    try:
        verify = ET.fromstring(new_meta)
    except ET.ParseError as error:
        raise BumpError(f"generated AppStream XML is invalid: {error}") from error
    added = [element for element in verify.findall("releases/release") if element.get("version") == version]
    if len(added) != 1 or added[0].get("date") != released:
        raise BumpError("generated AppStream release entry was not preserved")
    return current, new_cmake, new_meta


def stage_file(path: Path, data: bytes) -> Path:
    """Write a complete replacement beside the original, keeping its permissions."""
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f".{path.name}.", delete=False) as output:
        staged = Path(output.name)
        os.fchmod(output.fileno(), stat.S_IMODE(path.stat().st_mode))
        output.write(data)
    return staged


def replace_files(updates: list[tuple[Path, str]]) -> None:
    """Validate/stage both files before replacing either; restore on an I/O error."""
    original = {path: path.read_bytes() for path, _ in updates}
    staged: list[tuple[Path, Path]] = []
    replaced: list[Path] = []
    try:
        for path, content in updates:
            staged.append((path, stage_file(path, content.encode("utf-8"))))
        for path, replacement in staged:
            os.replace(replacement, path)
            replaced.append(path)
    except OSError:
        for path in reversed(replaced):
            backup = stage_file(path, original[path])
            try:
                os.replace(backup, path)
            finally:
                backup.unlink(missing_ok=True)
        raise
    finally:
        for _, replacement in staged:
            replacement.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Bump Kinefin's CMake version and AppStream release metadata")
    parser.add_argument("version", help="new stable version, X.Y.Z (tag it as vX.Y.Z)")
    parser.add_argument("--date", default=datetime.now(timezone.utc).date().isoformat(), help="release date YYYY-MM-DD (default: today UTC)")
    parser.add_argument("--dry-run", action="store_true", help="print a unified diff without writing any file")
    args = parser.parse_args()
    try:
        # Read everything before computing or writing updates. Invalid XML or an
        # existing release never leaves CMake half-bumped.
        old_cmake = CMAKE.read_text(encoding="utf-8")
        old_meta = METAINFO.read_text(encoding="utf-8")
        current, new_cmake, new_meta = updated_contents(old_cmake, old_meta, args.version, args.date)
        updates = [(CMAKE, new_cmake), (METAINFO, new_meta)]
        if args.dry_run:
            for path, content in updates:
                source = old_cmake if path == CMAKE else old_meta
                diff = difflib.unified_diff(
                    source.splitlines(keepends=True), content.splitlines(keepends=True),
                    fromfile=f"a/{path.relative_to(ROOT)}", tofile=f"b/{path.relative_to(ROOT)}",
                )
                sys.stdout.writelines(diff)
        else:
            replace_files(updates)
            print(f"Kinefin {current} -> {args.version} ({args.date}); release tag: v{args.version}")
    except (BumpError, OSError, UnicodeError) as error:
        print(f"version-bump: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
