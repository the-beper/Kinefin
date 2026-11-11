# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline checks for the local all-in-one release preflight."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build/release"))
import preflight  # noqa: E402


class PreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self.calls: list[list[str]] = []

    def runner(self, command, **kwargs):
        self.calls.append(command)
        self.assertEqual(kwargs["cwd"], ROOT)
        self.assertEqual(kwargs["timeout"], 120)
        self.assertFalse(kwargs["check"])
        return subprocess.CompletedProcess(command, 0)

    def test_source_checks_run_without_tag_or_credentials(self):
        preflight.verify(runner=self.runner)
        self.assertEqual([Path(call[2]).name for call in self.calls], list(preflight.COMMANDS))

    def test_tagged_checkout_adds_metadata_and_flathub_dry_run(self):
        preflight.verify("v1.2.3", runner=self.runner)
        self.assertEqual(len(self.calls), len(preflight.COMMANDS) + 2)
        self.assertEqual(self.calls[-2][-1], "v1.2.3")
        self.assertEqual(self.calls[-1][-2:], ["--dry-run", "v1.2.3"])

    def test_invalid_tags_rejected_before_starting_commands(self):
        for tag in ("v01.2.3", "1.2.3", "v1.2", "v1.2.3; rm -rf /", "v1.2.3\n"):
            with self.subTest(tag=tag), self.assertRaisesRegex(preflight.PreflightError, "tag"):
                preflight.verify(tag, runner=self.runner)
        self.assertEqual(self.calls, [])

    def test_requires_approved_image_digest_when_requested(self):
        for digest in ("", "sha256:" + "0" * 64, "sha256:" + "a" * 63,
                       "sha256:" + "A" * 64, "ghcr.io/test@sha256:" + "a" * 64):
            with self.subTest(digest=digest), self.assertRaisesRegex(preflight.PreflightError, "digest"):
                preflight.verify(require_digest=True, image_digest=digest, runner=self.runner)
        self.assertEqual(self.calls, [])
        preflight.verify(require_digest=True, image_digest="sha256:" + "a" * 64,
                         runner=self.runner)
        self.assertEqual(len(self.calls), len(preflight.COMMANDS))

    def test_failed_subprocess_stops_remaining_checks(self):
        def failing(command, **kwargs):
            self.calls.append(command)
            return subprocess.CompletedProcess(command, 42)
        with self.assertRaisesRegex(preflight.PreflightError, "failed.*42"):
            preflight.verify(runner=failing)
        self.assertEqual(len(self.calls), 1)

    def test_missing_subprocess_is_rejected(self):
        def missing(command, **kwargs):
            raise FileNotFoundError("python is not available")
        with self.assertRaisesRegex(preflight.PreflightError, "cannot finish"):
            preflight.verify(runner=missing)

    def test_cli_without_tag_works_on_checked_in_skeleton(self):
        process = subprocess.run([sys.executable, "-I", str(ROOT / "build/release/preflight.py")],
                                 cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertIn("Offline release-source checks passed", process.stdout)

    def test_cli_fails_on_unbumped_release_metadata(self):
        process = subprocess.run([sys.executable, "-I", str(ROOT / "build/release/preflight.py"),
                                  "--tag", "v0.0.1"],
                                 cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("release-preflight", process.stderr)

    def test_packaged_icon_checks_are_in_flatpak_smoke(self):
        smoke = (ROOT / "build/flatpak/test-flatpak.sh").read_text()
        self.assertIn('flatpak info --system --show-location "$id"', smoke)
        self.assertIn('"$install_root/files/share/icons/hicolor/${size}x${size}/apps/$id.png"', smoke)
        self.assertIn('"$install_root/files/share/icons/hicolor/scalable/apps/$id.svg"', smoke)


if __name__ == "__main__":
    unittest.main()
