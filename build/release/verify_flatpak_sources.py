#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline, fail-closed pin audit for the Kinefin Flatpak manifest.

The checked-in manifest uses a single local directory source for Kinefin. The
Flathub submission replaces it with an immutable Git commit. Both manifests
must pin every downloaded source; this deliberately accepts only the small
YAML source-list layout used by this project, rather than pretending that a
regular expression is a general-purpose YAML parser.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parents[2]
APP_ID = "io.github.the_beper.Kinefin"
DEFAULT_MANIFEST = ROOT / "build/flatpak" / f"{APP_ID}.yml"
APP_GIT_URL = "https://github.com/the-beper/Kinefin.git"
GIT_COMMIT = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
TAG = re.compile(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")
SAFE_REF = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/+\-]*\Z")
SOURCE_HEADER = re.compile(r"( *)sources:(?: *#.*)?\Z")
SOURCE_LINE = re.compile(r"- type: ([a-z]+)(?: *#.*)?\Z")
PROPERTY = re.compile(r"([a-z][a-z0-9-]*):(?: +(.*))?\Z")
MODULE_LINE = re.compile(r"  - name: ([a-z0-9-]+)(?: *#.*)?\Z")
KEYS = {
    "git": frozenset({"url", "commit", "tag", "x-checker-data"}),
    "archive": frozenset({"url", "sha256", "x-checker-data"}),
    "file": frozenset({"url", "sha256", "dest-filename", "x-checker-data"}),
    "dir": frozenset({"path", "skip"}),
}


class SourceError(ValueError):
    """A source might be mutable or the manifest needs human review."""


@dataclass(frozen=True)
class Source:
    kind: str
    properties: dict[str, str]
    line: int
    app_module: bool

    def fail(self, message: str) -> None:
        raise SourceError(f"source at line {self.line} ({self.kind}): {message}")


def scalar(value: str) -> str:
    """Accept only plain/quoted, single-line scalars (no YAML aliases/merges)."""
    if value.startswith(("'", '"')):
        if len(value) < 2 or value[-1] != value[0]:
            raise SourceError("invalid or multiline YAML scalar")
        # YAML escape sequences are intentionally not interpreted here.
        value = value[1:-1]
    else:
        value = value.split(" #", 1)[0].rstrip()
    if not value or any(char in value for char in "\r\n\t"):
        raise SourceError("empty or invalid source scalar")
    if value != "[.git, dist]" and (value.startswith(("*", "&", "!", "|", ">", "{", "[")) or " #" in value):
        raise SourceError("YAML references and complex source scalars are not supported")
    return value


def source_lists(manifest: str) -> list[Source]:
    """Parse the project's block-style sources lists, including nested modules.

    Unsupported YAML forms fail closed: if a new source layout is introduced,
    this gate must be reviewed and updated alongside it.
    """
    lines = manifest.splitlines()
    sections: list[tuple[int, int, bool]] = []
    top_module = ""
    for i, line in enumerate(lines):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        # YAML merge keys and aliases could add source lists that the strict
        # block parser cannot inspect. Reject them even outside sources:.
        uncommented = line.split(" #", 1)[0]
        if re.search(r"(?:^|\s)(?:<<:|[&*][A-Za-z_][A-Za-z0-9_-]*)", uncommented):
            raise SourceError(f"YAML anchors, aliases and merges are not supported (line {i + 1})")
        if line.strip() in ("---", "..."):
            raise SourceError(f"multiple YAML documents are not supported (line {i + 1})")
        whitespace = line[: len(line) - len(line.lstrip())]
        if "\t" in whitespace:
            raise SourceError(f"tabs in YAML indentation at line {i + 1}")
        match = MODULE_LINE.fullmatch(line)
        if match:
            top_module = match.group(1)
        if re.match(r" *sources\s*:", line):
            match = SOURCE_HEADER.fullmatch(line)
            if match is None:
                raise SourceError(f"unsupported sources mapping at line {i + 1}")
            depth = len(match.group(1))
            if depth % 2:
                raise SourceError(f"unexpected sources indentation at line {i + 1}")
            sections.append((i, depth, top_module == "kinefin" and depth == 4))

    if not sections:
        raise SourceError("no Flatpak source lists found")

    result: list[Source] = []
    for start, depth, app_module in sections:
        entries: list[tuple[int, int]] = []
        end = start + 1
        for j in range(start + 1, len(lines)):
            line = lines[j]
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            indent = len(line) - len(line.lstrip(" "))
            if indent <= depth:
                break
            if indent == depth + 2:
                if not SOURCE_LINE.fullmatch(line.strip()):
                    raise SourceError(f"unsupported source entry at line {j + 1}")
                entries.append((j, indent))
            elif not entries:
                raise SourceError(f"source list has no first item at line {j + 1}")
            end = j + 1
        if not entries:
            raise SourceError(f"empty sources list at line {start + 1}")
        for number, (index, indent) in enumerate(entries):
            boundary = entries[number + 1][0] if number + 1 < len(entries) else end
            kind = SOURCE_LINE.fullmatch(lines[index].strip()).group(1)
            if kind not in KEYS:
                raise SourceError(f"unsupported source type {kind!r} at line {index + 1}")
            props: dict[str, str] = {}
            parent_key = ""
            for k in range(index + 1, boundary):
                line = lines[k]
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                offset = len(line) - len(line.lstrip(" "))
                if offset == indent + 2:
                    found = PROPERTY.fullmatch(line.strip())
                    if found is None:
                        raise SourceError(f"unsupported source property at line {k + 1}")
                    key, raw = found.groups()
                    if key in props:
                        raise SourceError(f"duplicate {key} at line {k + 1}")
                    if key not in KEYS[kind]:
                        raise SourceError(f"unreviewed source property {key} at line {k + 1}")
                    if raw is None:
                        if key != "x-checker-data":
                            raise SourceError(f"missing value for {key} at line {k + 1}")
                        props[key] = "<metadata>"
                    else:
                        props[key] = scalar(raw)
                    parent_key = key
                elif offset > indent + 2 and parent_key == "x-checker-data":
                    # The update-checker is non-operative metadata, not a source.
                    continue
                else:
                    raise SourceError(f"unexpected YAML nesting at line {k + 1}")
            result.append(Source(kind, props, index + 1, app_module))
    return result


def check_https(url: str, source: Source) -> None:
    try:
        parsed = urlsplit(url)
        port = parsed.port
    except ValueError as error:
        source.fail(f"invalid URL: {error}")
    if (
        parsed.scheme != "https" or not parsed.hostname or parsed.username is not None
        or parsed.password is not None or port is not None or parsed.query or parsed.fragment
        or not parsed.path.startswith("/") or any(c.isspace() for c in url)
    ):
        source.fail("source URL must be an unambiguous HTTPS URL without credentials or query")


def check(manifest: str, *, flathub_tag: str | None = None, flathub_commit: str | None = None) -> int:
    """Verify immutable source identifiers and the intended Kinefin source."""
    flathub = flathub_tag is not None or flathub_commit is not None
    if flathub:
        if not flathub_tag or not flathub_commit or not TAG.fullmatch(flathub_tag) or not GIT_COMMIT.fullmatch(flathub_commit):
            raise SourceError("Flathub validation requires a valid release tag and full commit")

    sources = source_lists(manifest)
    app_sources = 0
    local_directories = 0
    for source in sources:
        props = source.properties
        if source.kind == "dir":
            if flathub or not source.app_module or props != {"path": "../..", "skip": "[.git, dist]"}:
                source.fail("local directory sources are permitted only for the checked-in Kinefin module")
            local_directories += 1
            app_sources += 1
            continue
        check_https(props.get("url", ""), source)
        if source.kind == "git":
            commit = props.get("commit", "")
            if not GIT_COMMIT.fullmatch(commit) or set(commit) == {"0"}:
                source.fail("Git sources need a full, nonzero lowercase commit ID")
            if "tag" in props and not SAFE_REF.fullmatch(props["tag"]):
                source.fail("invalid pinned Git tag")
            if source.app_module:
                if not flathub or props.get("url") != APP_GIT_URL or props.get("tag") != flathub_tag or commit != flathub_commit:
                    source.fail("Kinefin Flathub source must match the release tag and checked-out commit")
                app_sources += 1
        else:
            checksum = props.get("sha256", "")
            if not SHA256.fullmatch(checksum) or set(checksum) == {"0"}:
                source.fail("downloaded sources require a nonzero lowercase SHA-256")
            if source.app_module:
                # The Kinefin module also contains a pinned mpvqt archive.
                if source.kind != "file":
                    source.fail("unexpected source in the application module")
    if local_directories != (0 if flathub else 1) or app_sources != 1:
        raise SourceError("expected exactly one approved Kinefin application source")
    return len(sources)


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit immutable Flatpak source pins")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--flathub-tag", help="release tag for the generated Flathub manifest")
    parser.add_argument("--flathub-commit", help="checked-out Git commit for the generated manifest")
    args = parser.parse_args()
    try:
        count = check(
            args.manifest.read_text(encoding="utf-8"),
            flathub_tag=args.flathub_tag,
            flathub_commit=args.flathub_commit,
        )
    except (OSError, UnicodeError, SourceError) as error:
        print(f"flatpak-source-verify: {error}", file=sys.stderr)
        return 1
    print(f"Flatpak sources verified: {count} immutable or approved local sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
