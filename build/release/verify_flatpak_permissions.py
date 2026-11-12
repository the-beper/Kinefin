#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit the reviewed Flatpak sandbox policy in source and installed metadata.

The source parser intentionally understands only the top-level scalars and the
block-style finish-args list used by this project. A new YAML representation or
an additional sandbox grant needs a deliberate review, not an implicit allow.
Use --metadata with the output of ``flatpak info --show-metadata APP_ID`` to
verify the actual installed bundle in addition to the checked-in manifest.
"""

from __future__ import annotations

import argparse
from configparser import ConfigParser, Error as ConfigError
from pathlib import Path
import re
import sys

APP_ID = "io.github.the_beper.Kinefin"
DEFAULT_MANIFEST = Path(__file__).resolve().parent.parent / "flatpak" / f"{APP_ID}.yml"
IDENTITY = {
    "app-id": APP_ID,
    "runtime": "org.kde.Platform",
    "runtime-version": "6.11",
    "sdk": "org.kde.Sdk",
    "command": "kinefin",
}

# Granted by the org.kde.Platform runtime's own [Context], not by the manifest: read-only KDE colour and font settings
RUNTIME_CONTEXT: dict[str, set[str]] = {"filesystems": {"xdg-config/kdeglobals:ro"}}
# Session bus names the same runtime talks to for every KDE app
RUNTIME_SESSION_BUS = {
    "com.canonical.AppMenu.Registrar": "talk",
    "org.kde.KGlobalSettings": "talk",
    "org.kde.kconfig.notify": "talk",
}

# Explicitly reviewed grants. New permissions must be audited before this list
# changes; do not derive approval from the manifest being checked.
ALLOWED_ARGS = frozenset({
    "--socket=wayland",
    "--socket=pulseaudio",
    "--share=network",
    "--device=dri",
    "--device=input",
    "--own-name=io.github.the_beper.Kinefin.*",
    "--own-name=io.github.the_beper.Kinefin",
    "--own-name=org.mpris.MediaPlayer2.Kinefin.*",
    "--talk-name=org.freedesktop.ScreenSaver",
    "--system-talk-name=org.freedesktop.NetworkManager",
    "--talk-name=org.kde.kwalletd6",
    "--system-talk-name=org.freedesktop.login1",
    "--talk-name=org.freedesktop.portal.Desktop",
})

ROOT_KEY = re.compile(r"([a-z][a-z0-9-]*):(?:\s+(.*))?\Z")
ARG_LINE = re.compile(r"  - (--[a-z][a-z-]*=[A-Za-z0-9_.*-]+)(?:\s+#.*)?\Z")
YAML_REFERENCE = re.compile(r"(?:^|\s)(?:<<:|[&*][A-Za-z_][A-Za-z0-9_-]*)")


class SandboxError(ValueError):
    """The package identity or sandbox permissions are not approved."""


def scalar(text: str) -> str:
    text = text.split(" #", 1)[0].strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        text = text[1:-1]
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", text):
        raise SandboxError(f"unsupported top-level YAML scalar {text!r}")
    return text


def check_manifest(contents: str) -> None:
    """Reject unknown, duplicate or missing permissions, and identity drift."""
    scalars: dict[str, str] = {}
    roots: set[str] = set()
    permissions: list[str] = []
    in_permissions = False
    for number, original in enumerate(contents.splitlines(), 1):
        line = original.rstrip()
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.strip() in ("---", "...") or YAML_REFERENCE.search(line.split(" #", 1)[0]):
            raise SandboxError(f"YAML documents, anchors and aliases are not supported (line {number})")
        if "\t" in line[: len(line) - len(line.lstrip())]:
            raise SandboxError(f"YAML tabs are not supported (line {number})")
        if not line.startswith(" "):
            match = ROOT_KEY.fullmatch(line)
            if match is None:
                raise SandboxError(f"unsupported root YAML mapping (line {number})")
            key, raw = match.groups()
            if key in roots:
                raise SandboxError(f"duplicate top-level {key} (line {number})")
            roots.add(key)
            in_permissions = key == "finish-args"
            if in_permissions and raw is not None and not raw.lstrip().startswith("#"):
                raise SandboxError("finish-args must be a plain YAML block list")
            if key in IDENTITY:
                if raw is None:
                    raise SandboxError(f"missing top-level {key}")
                scalars[key] = scalar(raw)
            continue
        if in_permissions:
            match = ARG_LINE.fullmatch(line)
            if match is None:
                raise SandboxError(f"unsupported finish-args entry (line {number})")
            permissions.append(match.group(1))

    for key, expected in IDENTITY.items():
        if scalars.get(key) != expected:
            raise SandboxError(f"{key} must be {expected!r}, got {scalars.get(key)!r}")
    if "finish-args" not in roots:
        raise SandboxError("missing finish-args")
    if len(permissions) != len(set(permissions)):
        raise SandboxError("duplicate finish-args permission")
    actual = set(permissions)
    if actual != ALLOWED_ARGS:
        extra, missing = sorted(actual - ALLOWED_ARGS), sorted(ALLOWED_ARGS - actual)
        raise SandboxError(f"unapproved finish-args (extra={extra}, missing={missing})")


def expected_metadata() -> tuple[dict[str, set[str]], dict[str, str], dict[str, str]]:
    contexts: dict[str, set[str]] = {"shared": set(), "sockets": set(), "devices": set()}
    session: dict[str, str] = {}
    system: dict[str, str] = {}
    for arg in ALLOWED_ARGS:
        key, value = arg.split("=", 1)
        if key in ("--share", "--socket", "--device"):
            contexts[{"--share": "shared", "--socket": "sockets", "--device": "devices"}[key]].add(value)
        elif key in ("--talk-name", "--own-name"):
            session[value] = "own" if key == "--own-name" else "talk"
        elif key in ("--system-talk-name", "--system-own-name"):
            system[value] = "own" if key == "--system-own-name" else "talk"
        else:
            raise SandboxError(f"unhandled policy permission {key}")
    return contexts, {**session, **RUNTIME_SESSION_BUS}, system


def permission_list(value: str, label: str) -> set[str]:
    parts = [part for part in value.split(";") if part]
    if len(parts) != len(set(parts)):
        raise SandboxError(f"duplicate entries in installed metadata {label}")
    return set(parts)


def check_metadata(contents: str) -> None:
    """Check the installed package, not a system/user override or desired YAML."""
    parser = ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str  # D-Bus names and key case must not be normalized.
    try:
        parser.read_string(contents)
    except ConfigError as error:
        raise SandboxError(f"invalid installed Flatpak metadata: {error}") from error

    if not parser.has_section("Application"):
        raise SandboxError("installed metadata is missing [Application]")
    application = {
        "name": APP_ID,
        "runtime": f"{IDENTITY['runtime']}/x86_64/{IDENTITY['runtime-version']}",
        "sdk": f"{IDENTITY['sdk']}/x86_64/{IDENTITY['runtime-version']}",
        "command": IDENTITY["command"],
    }
    for key, expected in application.items():
        actual = parser.get("Application", key, fallback=None)
        if actual != expected:
            raise SandboxError(f"installed [Application] {key}: expected {expected!r}, got {actual!r}")

    if not parser.has_section("Context"):
        raise SandboxError("installed metadata is missing [Context]")
    contexts, session, system = expected_metadata()
    for key, wanted in contexts.items():
        actual = permission_list(parser.get("Context", key, fallback=""), f"Context/{key}")
        if actual != wanted:
            raise SandboxError(f"installed [Context] {key}: expected {sorted(wanted)}, got {sorted(actual)}")
    for key, value in parser.items("Context", raw=True):
        if key in contexts or not value.strip():
            continue
        if permission_list(value, f"Context/{key}") != RUNTIME_CONTEXT.get(key, set()):
            raise SandboxError(f"unexpected installed [Context] permission: {key}={value}")
    for section, expected in (("Session Bus Policy", session), ("System Bus Policy", system)):
        actual = dict(parser.items(section, raw=True)) if parser.has_section(section) else {}
        if actual != expected:
            raise SandboxError(f"installed [{section}] differs from approved policy")
    for section in ("Environment", "USB Devices", "A11y Bus Policy"):
        if parser.has_section(section) and parser.items(section, raw=True):
            raise SandboxError(f"unexpected installed [{section}] permissions")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify reviewed Flatpak sandbox permissions")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--metadata", type=Path, help="flatpak info --show-metadata output")
    args = parser.parse_args()
    try:
        check_manifest(args.manifest.read_text(encoding="utf-8"))
        if args.metadata is not None:
            check_metadata(args.metadata.read_text(encoding="utf-8"))
    except (SandboxError, OSError, UnicodeError) as error:
        print(f"flatpak-sandbox: {error}", file=sys.stderr)
        return 1
    print("Flatpak sandbox permissions and application identity verified" + (" (installed bundle)" if args.metadata else " (manifest)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
