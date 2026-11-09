#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Starts the real kinefin binary offscreen and fails on any QML load problem.

Usage: shell_smoke.py <kinefin binary>. The app must still be running after the grace period (killed by this script, which is the pass), with a private HOME-like tree and its own session bus; any QML warning, a crash or an early exit fails.
"""
import os
import subprocess
import sys
import tempfile

GRACE_SECONDS = 6
BAD_MARKERS = ("qml:", ".qml:", "is not a type", "failed to load", "Cannot assign", "ReferenceError", "TypeError", "Unable to", "Could not load")


# Kirigami's PageRow creates every pushed page without a parent and places it right after; Qt 6.11.2 with Kirigami 6.31 warns about that for any pageStack.push, even of an empty Kirigami.Page (observed: initialPage is silent, every push warns). Inferred harmless; everything else still fails the test.
KNOWN_NOISE = ("Created graphical object was not placed in the graphics scene",)

# Kirigami.AboutPage (Kirigami 6.31) reports binding loops in the OverlaySheet it builds for the license text, and a plain QML line follows each report; inferred harmless, they come from Kirigami's own files.
KNOWN_NOISE += ("templates/OverlaySheet.qml", "AboutItem.qml:377:9: QML OverlaySheet: Binding loop")


# KINEFIN_START_PAGE opens one more page at start; None is the plain Servers page.
PAGES = (None, "settings", "about")


def run(binary: str, page: str | None) -> int:
    with tempfile.TemporaryDirectory() as home:
        env = dict(os.environ)
        env.update(
            QT_QPA_PLATFORM="offscreen",
            # Without these the app's messages never reach stderr under a bare session bus.
            QT_LOGGING_TO_CONSOLE="1",
            QT_FORCE_STDERR_LOGGING="1",
            XDG_DATA_HOME=f"{home}/data",
            XDG_CONFIG_HOME=f"{home}/config",
            XDG_CACHE_HOME=f"{home}/cache",
            XDG_RUNTIME_DIR=f"{home}/run",
        )
        if page:
            env["KINEFIN_START_PAGE"] = page
        os.makedirs(env["XDG_RUNTIME_DIR"], mode=0o700)
        command = ["dbus-run-session", "--", "timeout", "--signal=TERM", str(GRACE_SECONDS), binary]
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        output = result.stdout + result.stderr
        # timeout(1) answers 124 when it had to stop a still-running app: the pass.
        if result.returncode != 124:
            print(f"kinefin ({page or 'servers'}) exited early with {result.returncode}:\n{output}")
            return 1
        bad = [
            line
            for line in output.splitlines()
            if any(marker in line for marker in BAD_MARKERS) and not any(noise in line for noise in KNOWN_NOISE)
        ]
        if bad:
            print(f"QML problems ({page or 'servers'}):\n" + "\n".join(bad))
            return 1
    return 0


def main() -> int:
    binary = os.path.abspath(sys.argv[1])
    if any(run(binary, page) for page in PAGES):
        return 1
    print("shell loaded without QML problems")
    return 0


if __name__ == "__main__":
    sys.exit(main())
