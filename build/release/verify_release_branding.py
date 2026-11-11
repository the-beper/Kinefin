#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check first-release icon assets, desktop entry and AppStream identity.

Pure Python: the same checks run on a host and on GitHub's Ubuntu runners.
They intentionally do not claim that a real Flatpak build or a desktop review
has taken place. SVG and raster image content is checked without fetching URLs.
"""

from __future__ import annotations

import argparse
import re
from configparser import ConfigParser, Error as ConfigError
from pathlib import Path
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

ROOT = Path(__file__).resolve().parents[2]
APP_ID = "io.github.the_beper.Kinefin"
SVG_NS = "{http://www.w3.org/2000/svg}"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
ICON = Path("data/icons/hicolor")
DESKTOP = Path(f"data/{APP_ID}.desktop")
METAINFO = Path(f"data/{APP_ID}.metainfo.xml")
ALLOWED_SVG_TAGS = {"svg", "defs", "linearGradient", "stop", "clipPath", "rect", "path", "g"}


class BrandingError(ValueError):
    """Release artwork or desktop metadata is missing or inconsistent."""


def verify_svg(data: bytes) -> None:
    if b"<!DOCTYPE" in data.upper() or b"<!ENTITY" in data.upper():
        raise BrandingError("SVG DTD/entities are not permitted")
    try:
        root = ET.fromstring(data)
    except ET.ParseError as error:
        raise BrandingError(f"malformed SVG: {error}") from error
    if (root.tag != SVG_NS + "svg" or root.get("width") != "48" or
            root.get("height") != "48" or root.get("viewBox") != "0 0 48 48"):
        raise BrandingError("SVG must use the reviewed 48px application-icon grid")
    paths = 0
    for node in root.iter():
        if not node.tag.startswith(SVG_NS) or node.tag[len(SVG_NS):] not in ALLOWED_SVG_TAGS:
            raise BrandingError(f"unsupported SVG element {node.tag!r}")
        paths += node.tag == SVG_NS + "path"
        for name, value in node.attrib.items():
            if name.startswith("on") or name.endswith("}href") or name == "href":
                raise BrandingError("SVG contains an event handler or external reference")
            if "url(" in value.lower() and not re.fullmatch(r"url\(#[A-Za-z0-9-]+\)", value):
                raise BrandingError("SVG has a non-local paint reference")
            if re.search(r"(?i)https?://|javascript:|data:", value):
                raise BrandingError("SVG contains an external resource")
    if paths < 2:
        raise BrandingError("SVG is missing the fin and playback artwork")


def png_pixels(data: bytes, expected: int) -> tuple[int, int, int]:
    """Validate PNG chunks and RGBA rows, returning alpha at key locations."""
    if len(data) > 1_000_000 or not data.startswith(PNG_MAGIC):
        raise BrandingError("PNG is missing, oversized or has an invalid signature")
    offset = len(PNG_MAGIC)
    chunks: list[tuple[bytes, bytes]] = []
    while offset + 12 <= len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        end = offset + 12 + size
        if size > 1_000_000 or end > len(data):
            raise BrandingError("truncated or oversized PNG chunk")
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + size]
        checksum = struct.unpack_from(">I", data, offset + 8 + size)[0]
        if zlib.crc32(kind + payload) != checksum:
            raise BrandingError(f"PNG {kind!r} CRC mismatch")
        chunks.append((kind, payload))
        offset = end
        if kind == b"IEND":
            break
    if not chunks or offset != len(data) or chunks[0][0] != b"IHDR" or chunks[-1] != (b"IEND", b""):
        raise BrandingError("PNG chunks or end marker are invalid")
    headers = [value for kind, value in chunks if kind == b"IHDR"]
    if len(headers) != 1 or len(headers[0]) != 13:
        raise BrandingError("PNG must contain one IHDR")
    width, height, depth, mode, compression, filtering, interlacing = struct.unpack(">IIBBBBB", headers[0])
    if (width, height, depth, mode, compression, filtering, interlacing) != (expected, expected, 8, 6, 0, 0, 0):
        raise BrandingError(f"PNG must be non-interlaced {expected}x{expected} RGBA")
    if any(kind[0] < 97 and kind not in (b"IHDR", b"IDAT", b"IEND") for kind, _ in chunks):
        raise BrandingError("unsupported critical PNG chunk")
    idats = [payload for kind, payload in chunks if kind == b"IDAT"]
    if not idats or len(idats) > 20:
        raise BrandingError("PNG has no pixel data or too many IDAT chunks")
    expected_bytes = expected * (1 + expected * 4)
    decoder = zlib.decompressobj()
    try:
        raw = decoder.decompress(b"".join(idats), expected_bytes + 1)
    except zlib.error as error:
        raise BrandingError(f"invalid PNG compressed image data: {error}") from error
    if len(raw) != expected_bytes or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise BrandingError("PNG image data length is not valid")
    previous = bytearray(expected * 4)
    alpha_samples: dict[tuple[int, int], int] = {}
    colored = 0
    for y in range(expected):
        index = y * (1 + expected * 4)
        filter_kind = raw[index]
        if filter_kind > 4:
            raise BrandingError("unsupported PNG row filter")
        row = bytearray(raw[index + 1:index + 1 + expected * 4])
        for i in range(len(row)):
            left = row[i - 4] if i >= 4 else 0
            up = previous[i]
            upper_left = previous[i - 4] if i >= 4 else 0
            if filter_kind == 1:
                prediction = left
            elif filter_kind == 2:
                prediction = up
            elif filter_kind == 3:
                prediction = (left + up) // 2
            elif filter_kind == 4:
                p = left + up - upper_left
                a, b, c = abs(p-left), abs(p-up), abs(p-upper_left)
                prediction = left if a <= b and a <= c else up if b <= c else upper_left
            else:
                prediction = 0
            row[i] = (row[i] + prediction) & 0xff
        colored += sum(row[i] > 0 for i in range(3, len(row), 4))
        for x in (0, expected // 2, expected - 1):
            alpha_samples[(x, y)] = row[x * 4 + 3]
        previous = row
    if colored < expected * expected // 3 or alpha_samples[(0, 0)] != 0 or alpha_samples[(expected - 1, expected - 1)] != 0:
        raise BrandingError("PNG icon must have a visible subject and transparent corners")
    if alpha_samples[(expected // 2, expected // 2)] < 240:
        raise BrandingError("PNG icon centre must be opaque")
    return width, height, colored


def verify(root: Path = ROOT) -> None:
    svg_path = root / ICON / "scalable/apps" / f"{APP_ID}.svg"
    verify_svg(svg_path.read_bytes())
    for size in (128, 256):
        bitmap = root / ICON / f"{size}x{size}/apps" / f"{APP_ID}.png"
        png_pixels(bitmap.read_bytes(), size)

    desktop = ConfigParser(interpolation=None, strict=True)
    desktop.optionxform = str
    try:
        desktop.read_string((root / DESKTOP).read_text(encoding="utf-8"))
    except ConfigError as error:
        raise BrandingError(f"malformed desktop entry: {error}") from error
    if not desktop.has_section("Desktop Entry"):
        raise BrandingError("missing [Desktop Entry]")
    entry = desktop["Desktop Entry"]
    for key, wanted in {
        "Type": "Application", "Exec": "kinefin %u", "Icon": APP_ID,
        "MimeType": "x-scheme-handler/jellyfin;", "Terminal": "false",
    }.items():
        if entry.get(key) != wanted:
            raise BrandingError(f"desktop entry {key} must be {wanted!r}")
    categories = set(filter(None, entry.get("Categories", "").split(";")))
    if not {"AudioVideo", "Video"}.issubset(categories) or "Music" in categories:
        raise BrandingError("desktop categories must describe a movie/TV application, not music")

    try:
        meta = ET.fromstring((root / METAINFO).read_bytes())
    except ET.ParseError as error:
        raise BrandingError(f"malformed AppStream XML: {error}") from error
    if (meta.tag != "component" or meta.get("type") != "desktop-application" or
            meta.findtext("id") != APP_ID or
            meta.findtext("launchable[@type='desktop-id']") != f"{APP_ID}.desktop" or
            meta.findtext("provides/binary") != "kinefin" or
            meta.findtext("provides/mediatype") != "x-scheme-handler/jellyfin"):
        raise BrandingError("AppStream identity does not match the installed desktop entry")
    if not meta.findtext("summary") or meta.find("description/p") is None:
        raise BrandingError("AppStream summary/description is missing")


def main() -> int:
    parser = argparse.ArgumentParser(description="Check Kinefin release artwork and desktop/AppStream identity")
    parser.parse_args()
    try:
        verify()
    except (BrandingError, OSError, UnicodeError) as error:
        print(f"release-branding: {error}", file=sys.stderr)
        return 1
    print("Release artwork and desktop/AppStream identity verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
