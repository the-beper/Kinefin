#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail closed when the Flatpak KDE runtime pins drift from the manifest.

This is an offline consistency gate, not an online check that the pinned OSTree
commits still exist on Flathub. The build performs that check by installing them.
"""

from __future__ import annotations

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
PIN_FILE = ROOT / "build/flatpak/runtime-pins.conf"
MANIFEST = ROOT / "build/flatpak/io.github.the_beper.Kinefin.yml"
BUILD = ROOT / "build/flatpak/build-flatpak.sh"
SMOKE = ROOT / "build/flatpak/test-flatpak.sh"
PIN_KEYS = frozenset({"sdk", "platform", "sdk_commit", "platform_commit"})
PIN_LINE = re.compile(r"([a-z_]+)=([A-Za-z0-9./]+)\Z")
SHA256 = re.compile(r"[a-f0-9]{64}\Z")
VERSION = re.compile(r"[1-9][0-9]*(?:\.[0-9]+)+\Z")


class PinError(ValueError):
    """Invalid, inconsistent or unconsumed runtime pins."""


def read_pins(contents: str) -> dict[str, str]:
    """Read only the four permitted plain shell assignments; no shell execution."""
    values: dict[str, str] = {}
    for line_number, line in enumerate(contents.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        match = PIN_LINE.fullmatch(line)
        if not match:
            raise PinError(f"invalid runtime-pins.conf assignment at line {line_number}")
        key, value = match.groups()
        if key not in PIN_KEYS:
            raise PinError(f"unknown runtime-pins.conf key: {key}")
        if key in values:
            raise PinError(f"duplicate runtime-pins.conf key: {key}")
        values[key] = value
    if values.keys() != PIN_KEYS:
        raise PinError(f"runtime-pins.conf is missing keys: {', '.join(sorted(PIN_KEYS - values.keys()))}")
    for key in ("sdk_commit", "platform_commit"):
        if not SHA256.fullmatch(values[key]) or values[key] == "0" * 64:
            raise PinError(f"{key} must be a nonzero lowercase SHA-256 OSTree commit")
    return values


def read_top_level(manifest: str, key: str) -> str:
    """Require exactly one unambiguous top-level scalar in this YAML manifest."""
    matches = re.findall(rf"^{re.escape(key)}:[ \t]*([^\n#]+?)[ \t]*(?:#.*)?$", manifest, re.MULTILINE)
    if len(matches) != 1:
        raise PinError(f"Flatpak manifest requires exactly one top-level {key}")
    value = matches[0].strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        value = value[1:-1]
    return value


def check(pins: str, manifest: str, build_script: str, smoke_script: str) -> dict[str, str]:
    """Validate the shared pins and confirm both scripts consume the same file."""
    values = read_pins(pins)
    runtime = read_top_level(manifest, "runtime")
    sdk = read_top_level(manifest, "sdk")
    version = read_top_level(manifest, "runtime-version")
    if runtime != "org.kde.Platform" or sdk != "org.kde.Sdk":
        raise PinError("Flatpak manifest must use the KDE Platform and SDK")
    if not VERSION.fullmatch(version):
        raise PinError("invalid Flatpak runtime-version")
    for key, name in (("sdk", sdk), ("platform", runtime)):
        expected = f"{name}//{version}"
        if values[key] != expected:
            raise PinError(f"{key} pin {values[key]!r} does not match manifest {expected!r}")
    source = '. "$(dirname "$0")/runtime-pins.conf"'
    for script, label in ((build_script, "build-flatpak.sh"), (smoke_script, "test-flatpak.sh")):
        if script.count(source) != 1:
            raise PinError(f"{label} must source the shared runtime-pins.conf exactly once")
        if re.search(r"^(?:sdk|platform|sdk_commit|platform_commit)=", script, re.MULTILINE):
            raise PinError(f"{label} contains duplicate hard-coded runtime pins")
    mount = '-v "$root/build/flatpak/runtime-pins.conf:/runtime-pins.conf:ro"'
    if smoke_script.count(mount) != 1:
        raise PinError("test-flatpak.sh must mount runtime-pins.conf read-only into the smoke container")
    return values


def main() -> int:
    try:
        pins = check(
            PIN_FILE.read_text(encoding="utf-8"),
            MANIFEST.read_text(encoding="utf-8"),
            BUILD.read_text(encoding="utf-8"),
            SMOKE.read_text(encoding="utf-8"),
        )
    except (OSError, UnicodeError, PinError) as error:
        print(f"runtime-pin-verify: {error}", file=sys.stderr)
        return 1
    print(f"KDE runtime pins verified: {pins['sdk']} and {pins['platform']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
