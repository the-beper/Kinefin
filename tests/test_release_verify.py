# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Release gate regression tests without GitHub, Qt, Git tags or a Flatpak build."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1]
METAINFO = "data/io.github.the_beper.Kinefin.metainfo.xml"


class ReleaseVerifyTest(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / "checkout"
        for name in (
            "CMakeLists.txt",
            METAINFO,
            "build/release/version-bump.sh",
            "build/release/version_bump.py",
            "build/release/verify_release.py",
        ):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(SOURCE / name, target)
        self.cmake = self.root / "CMakeLists.txt"
        self.meta = self.root / METAINFO

    def run_verify(self, tag: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-I", str(self.root / "build/release/verify_release.py"), tag],
            cwd=self.root.parent, capture_output=True, text=True, timeout=10, check=False,
        )

    def bump(self, version: str) -> None:
        result = subprocess.run(
            [str(self.root / "build/release/version-bump.sh"), "--date", "2026-11-11", version],
            cwd=self.root.parent, capture_output=True, text=True, timeout=10, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def replace_meta(self, old: str, new: str) -> None:
        data = self.meta.read_text(encoding="utf-8")
        self.assertIn(old, data)
        self.meta.write_text(data.replace(old, new), encoding="utf-8")

    def assert_rejected(self, tag: str, message: str) -> None:
        original = (self.cmake.read_bytes(), self.meta.read_bytes())
        result = self.run_verify(tag)
        self.assertNotEqual(result.returncode, 0, (tag, result.stdout, result.stderr))
        self.assertIn(message, result.stderr)
        self.assertEqual((self.cmake.read_bytes(), self.meta.read_bytes()), original)

    def test_skeleton_without_release_history_is_not_releasable(self) -> None:
        self.assert_rejected("v0.0.1", "exactly one releases block")

    def test_valid_bumped_release_from_a_different_directory(self) -> None:
        self.bump("0.0.2")
        original = (self.cmake.read_bytes(), self.meta.read_bytes())
        result = self.run_verify("v0.0.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Release v0.0.2 verified", result.stdout)
        self.assertIn("date 2026-11-11", result.stdout)
        self.assertEqual((self.cmake.read_bytes(), self.meta.read_bytes()), original)

    def test_multiple_release_entries_in_descending_order_are_supported(self) -> None:
        self.bump("0.0.2")
        self.bump("0.0.3")
        result = self.run_verify("v0.0.3")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_bad_tags_are_rejected(self) -> None:
        self.bump("0.0.2")
        for tag in ("0.0.2", "v0.0.02", "v01.0.0", "v0.0", "v0.0.2.1", "v0.0.2-rc1", "v0.0.2\n", "v0.0.a"):
            with self.subTest(tag=tag):
                self.assert_rejected(tag, "tag must be" if not tag.startswith("v") else "invalid stable release version")

    def test_tag_different_from_cmake_is_rejected(self) -> None:
        self.bump("0.0.2")
        self.assert_rejected("v0.0.3", "does not match CMake version")

    def test_wrong_appstream_version_is_rejected(self) -> None:
        self.bump("0.0.2")
        self.replace_meta('release version="0.0.2"', 'release version="0.0.1"')
        self.assert_rejected("v0.0.2", "newest AppStream release")

    def test_duplicate_appstream_versions_are_rejected(self) -> None:
        self.bump("0.0.2")
        self.replace_meta(
            '    <release version="0.0.2" date="2026-11-11" />',
            '    <release version="0.0.2" date="2026-11-11" />\n'
            '    <release version="0.0.2" date="2026-11-10" />',
        )
        self.assert_rejected("v0.0.2", "duplicate AppStream release")

    def test_out_of_order_appstream_history_is_rejected(self) -> None:
        self.bump("0.0.2")
        self.replace_meta(
            '    <release version="0.0.2" date="2026-11-11" />',
            '    <release version="0.0.2" date="2026-11-11" />\n'
            '    <release version="0.0.3" date="2026-11-10" />',
        )
        self.assert_rejected("v0.0.2", "descending version order")

    def test_missing_or_invalid_appstream_dates_are_rejected(self) -> None:
        self.bump("0.0.2")
        for invalid in ("", "2026-11-99", "2026-2-1", "tomorrow"):
            with self.subTest(invalid=invalid):
                saved = self.meta.read_text(encoding="utf-8")
                self.replace_meta('date="2026-11-11"', f'date="{invalid}"')
                self.assert_rejected("v0.0.2", "invalid")
                self.meta.write_text(saved, encoding="utf-8")

    def test_malformed_or_wrong_component_is_rejected(self) -> None:
        self.bump("0.0.2")
        saved = self.meta.read_text(encoding="utf-8")
        self.replace_meta("</component>", "</wrong-component>")
        self.assert_rejected("v0.0.2", "invalid AppStream XML")
        self.meta.write_text(saved, encoding="utf-8")
        self.replace_meta("<id>io.github.the_beper.Kinefin</id>", "<id>com.example.Other</id>")
        self.assert_rejected("v0.0.2", "not the Kinefin desktop component")

    def test_duplicate_release_blocks_are_rejected(self) -> None:
        self.bump("0.0.2")
        self.replace_meta("  </releases>", "  </releases>\n  <releases></releases>")
        self.assert_rejected("v0.0.2", "exactly one releases block")

    def test_invalid_or_duplicate_cmake_project_line_is_rejected(self) -> None:
        self.bump("0.0.2")
        saved = self.cmake.read_text(encoding="utf-8")
        self.cmake.write_text(saved.replace("VERSION 0.0.2", "VERSION 0.00.2"), encoding="utf-8")
        self.assert_rejected("v0.0.2", "invalid stable release version")
        self.cmake.write_text(saved + "\nproject(kinefin VERSION 0.0.2 LANGUAGES CXX)\n", encoding="utf-8")
        self.assert_rejected("v0.0.2", "expected exactly one project")

    def test_github_workflow_runs_verifier_before_any_release_build(self) -> None:
        workflow = (SOURCE / ".github/workflows/release.yml").read_text(encoding="utf-8")
        check = workflow.index('python3 -I build/release/verify_release.py "$RELEASE_TAG"')
        build = workflow.index("  build-flatpak:")
        self.assertLess(check, build)
        self.assertIn('git merge-base --is-ancestor "$GITHUB_SHA" origin/main', workflow)


if __name__ == "__main__":
    unittest.main()
