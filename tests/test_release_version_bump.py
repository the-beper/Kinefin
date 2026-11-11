# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable release-script tests: no Qt, network, git tags or Flatpak required."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
import xml.etree.ElementTree as ET


SOURCE = Path(__file__).resolve().parents[1]


class VersionBumpTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "checkout"
        for name in (
            "CMakeLists.txt",
            "data/io.github.the_beper.Kinefin.metainfo.xml",
            "build/release/version-bump.sh",
            "build/release/version_bump.py",
        ):
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(SOURCE / name, destination)
        self.cmake = self.root / "CMakeLists.txt"
        self.meta = self.root / "data/io.github.the_beper.Kinefin.metainfo.xml"
        self.script = self.root / "build/release/version-bump.sh"

    def run_bump(self, *args: str) -> subprocess.CompletedProcess[str]:
        # Relative paths must not depend on the caller's working directory.
        return subprocess.run(
            [str(self.script), *args],
            cwd=self.tmp.name,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def contents(self) -> tuple[bytes, bytes]:
        return self.cmake.read_bytes(), self.meta.read_bytes()

    def releases(self) -> list[ET.Element]:
        root = ET.fromstring(self.meta.read_bytes())
        return root.findall("releases/release")

    def test_bump_changes_only_project_version_and_release_metadata(self) -> None:
        old_cmake, old_meta = self.contents()
        result = self.run_bump("--date", "2026-11-11", "0.0.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("v0.0.2", result.stdout)
        self.assertEqual(
            self.cmake.read_bytes(),
            old_cmake.replace(b"project(kinefin VERSION 0.0.1", b"project(kinefin VERSION 0.0.2"),
        )
        new_meta = self.meta.read_bytes()
        self.assertIn(b'<release version="0.0.2" date="2026-11-11" />', new_meta)
        self.assertIn(b"<content_rating", new_meta)
        self.assertIn(b"<project_license>GPL-3.0-or-later</project_license>", new_meta)
        self.assertEqual(len(self.releases()), 1)
        self.assertNotEqual(old_meta, new_meta)

    def test_second_bump_preserves_existing_releases_in_newest_first_order(self) -> None:
        self.assertEqual(self.run_bump("0.0.2", "--date", "2026-11-11").returncode, 0)
        first = self.releases()[0]
        self.assertEqual(first.attrib, {"version": "0.0.2", "date": "2026-11-11"})
        result = self.run_bump("0.1.0", "--date", "2026-11-18")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([r.get("version") for r in self.releases()], ["0.1.0", "0.0.2"])
        self.assertEqual(self.releases()[1].get("date"), "2026-11-11")
        self.assertIn(b"project(kinefin VERSION 0.1.0", self.cmake.read_bytes())

    def test_dry_run_prints_both_diffs_without_editing_files(self) -> None:
        original = self.contents()
        result = self.run_bump("--dry-run", "--date", "2026-11-11", "0.0.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.contents(), original)
        self.assertIn("--- a/CMakeLists.txt", result.stdout)
        self.assertIn("+++ b/data/io.github.the_beper.Kinefin.metainfo.xml", result.stdout)
        self.assertIn('version="0.0.2"', result.stdout)

    def test_rejects_invalid_and_non_increasing_versions_without_writing(self) -> None:
        original = self.contents()
        for version in (
            "0.0.1", "0.0.0", "v0.0.2", "0.0.2-rc1", "0.00.2", "0.0.2.1",
            "0.0", "-1.0.0", "0.0.2\nproject(other VERSION 1.0.0)",
        ):
            with self.subTest(version=version):
                result = self.run_bump(version, "--date", "2026-11-11")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.contents(), original)

    def test_rejects_bad_date_before_editing(self) -> None:
        original = self.contents()
        for bad in ("2026-02-30", "2026-2-1", "2026-11-11T12:00", "not-a-date"):
            with self.subTest(date=bad):
                result = self.run_bump("0.0.2", "--date", bad)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.contents(), original)

    def test_invalid_xml_never_partially_updates_cmake(self) -> None:
        self.meta.write_text("<component><invalid>", encoding="utf-8")
        original = self.contents()
        result = self.run_bump("0.0.2", "--date", "2026-11-11")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid AppStream XML", result.stderr)
        self.assertEqual(self.contents(), original)

    def test_rejects_unsupported_layout_and_newer_prior_release(self) -> None:
        content = self.meta.read_text(encoding="utf-8")
        content = content.replace(
            '  <content_rating type="oars-1.1"/>',
            '  <releases><release version="0.0.9" date="2026-11-11" /></releases>\n'
            '  <content_rating type="oars-1.1"/>',
        )
        self.meta.write_text(content, encoding="utf-8")
        original = self.contents()
        self.assertNotEqual(self.run_bump("0.0.8", "--date", "2026-11-12").returncode, 0)
        self.assertNotEqual(self.run_bump("0.0.10", "--date", "2026-11-12").returncode, 0)
        self.assertEqual(self.contents(), original)

    def test_refuses_missing_file_and_malformed_project_line(self) -> None:
        original_meta = self.meta.read_bytes()
        self.cmake.write_text("project(kinefin VERSION broken LANGUAGES CXX)\n", encoding="utf-8")
        result = self.run_bump("0.0.2", "--date", "2026-11-11")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.meta.read_bytes(), original_meta)
        self.cmake.unlink()
        self.assertNotEqual(self.run_bump("0.0.2", "--date", "2026-11-11").returncode, 0)


if __name__ == "__main__":
    unittest.main()
