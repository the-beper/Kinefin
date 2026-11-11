#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run source-only release checks together; never publish or modify sources.

This is not a substitute for the GitHub CI gate, real-tag release, Flatpak smoke
or Jellyfin/Plasma host acceptance. It can be run before release credentials
are available. Optional --tag verifies release metadata and a clean tagged
checkout suitable for a Flathub manifest.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
COMMANDS = (
    "verify_action_pins.py",
    "verify_runtime_pins.py",
    "verify_flatpak_sources.py",
    "verify_flatpak_permissions.py",
    "verify_release_branding.py",
)
TAG = re.compile(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")
DIGEST = re.compile(r"sha256:[0-9a-f]{64}\Z")


class PreflightError(Exception):
    """A source-only release preparation gate failed."""


def verify(tag: str | None = None, *, require_digest: bool = False,
           image_digest: str = "", runner=subprocess.run) -> None:
    if tag is not None and not TAG.fullmatch(tag):
        raise PreflightError("tag must be stable vX.Y.Z")
    if require_digest and (not DIGEST.fullmatch(image_digest) or image_digest == "sha256:" + "0" * 64):
        raise PreflightError("set KINEFIN_RELEASE_FLATPAK_DIGEST to an approved nonzero sha256: digest")
    commands = [[sys.executable, "-I", str(ROOT / "build/release" / script)] for script in COMMANDS]
    if tag:
        commands.extend([
            [sys.executable, "-I", str(ROOT / "build/release/verify_release.py"), tag],
            [sys.executable, "-I", str(ROOT / "build/release/prepare_flathub.py"), "--dry-run", tag],
        ])
    for command in commands:
        try:
            result = runner(command, cwd=ROOT, check=False, stdout=subprocess.DEVNULL, timeout=120)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise PreflightError(f"cannot finish {Path(command[2]).name}: {error}") from error
        if result.returncode != 0:
            raise PreflightError(f"{Path(command[2]).name} failed (exit {result.returncode})")


def main() -> int:
    parser = argparse.ArgumentParser(description="Check Kinefin release sources without modifying or publishing")
    parser.add_argument("--tag", help="existing, checked-out stable Git release tag")
    parser.add_argument("--require-image-digest", action="store_true", help="also require the approved build image digest")
    args = parser.parse_args()
    try:
        verify(args.tag, require_digest=args.require_image_digest,
               image_digest=os.environ.get("KINEFIN_RELEASE_FLATPAK_DIGEST", ""))
    except PreflightError as error:
        print(f"release-preflight: {error}", file=sys.stderr)
        return 1
    print("Offline release-source checks passed; this does not verify a build, live server or desktop session.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
