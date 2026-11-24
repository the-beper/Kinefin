# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Flathub manifest generation with a local Git tag and no GitHub or Flatpak."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "tests"))
from release_skeleton import reset_to_skeleton  # noqa: E402

APP_ID = "io.github.the_beper.Kinefin"
MANIFEST = f"build/flatpak/{APP_ID}.yml"
FILES = (
    ".gitignore", "CMakeLists.txt", f"data/{APP_ID}.metainfo.xml", MANIFEST,
    "build/release/verify_release.py", "build/release/version-bump.sh",
    "build/release/version_bump.py", "build/release/prepare_flathub.py",
)


class FlathubManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "checkout"
        self.root.mkdir()
        for file in FILES:
            path = self.root / file
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(SOURCE / file, path)
        reset_to_skeleton(self.root)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.name", "Kinefin release test")
        self.git("config", "user.email", "release-test@example.invalid")
        result = subprocess.run(
            [str(self.root / "build/release/version-bump.sh"), "--date", "2026-11-11", "0.0.2"],
            cwd=self.root, text=True, capture_output=True, timeout=10, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.git("add", "-A")
        self.git("commit", "-qm", "Release 0.0.2")
        self.git("tag", "v0.0.2")
        self.manifest = self.root / MANIFEST
        self.output = self.root / "dist/flathub/v0.0.2" / f"{APP_ID}.yml"

    def git(self, *args: str) -> str:
        process = subprocess.run(
            ["git", *args], cwd=self.root, text=True, capture_output=True,
            timeout=10, check=False,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        return process.stdout.strip()

    def run_prepare(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-I", str(self.root / "build/release/prepare_flathub.py"), *args],
            cwd=self.root.parent, text=True, capture_output=True, timeout=10, check=False,
        )

    def test_generates_pinned_git_source_without_changing_local_manifest(self) -> None:
        original = self.manifest.read_bytes()
        digest = self.git("rev-parse", "HEAD")
        result = self.run_prepare("v0.0.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = self.output.read_text(encoding="utf-8")
        self.assertIn("      - type: git\n", manifest)
        self.assertIn("        url: https://github.com/the-beper/Kinefin.git\n", manifest)
        self.assertIn("        tag: v0.0.2\n", manifest)
        self.assertIn(f"        commit: {digest}\n", manifest)
        self.assertNotIn("      - type: dir\n", manifest)
        self.assertNotIn("        path: ../..\n", manifest)
        self.assertNotIn("skip: [.git, dist]", manifest)
        self.assertIn("      - type: file\n", manifest)
        self.assertIn("sha256: 6ff5f56a0edd8248073446e5cc038f47d4b17c46b6af43cf438123d0ac911189", manifest)
        self.assertIn('runtime-version: "6.11"', manifest)
        self.assertEqual(self.manifest.read_bytes(), original)
        self.assertEqual(self.run_prepare("v0.0.2").returncode, 0)

    def test_dry_run_prints_manifest_and_leaves_no_file(self) -> None:
        result = self.run_prepare("--dry-run", "v0.0.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith("app-id:"))
        self.assertIn("tag: v0.0.2", result.stdout)
        self.assertFalse(self.output.exists())

    def test_missing_or_invalid_tags_are_rejected(self) -> None:
        for tag in ("v0.0.3", "v0.0.2-rc1", "v0.00.2", "0.0.2", "v0.0.2\n"):
            with self.subTest(tag=tag):
                result = self.run_prepare(tag)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())

    def test_tag_must_point_to_head(self) -> None:
        self.git("commit", "--allow-empty", "-qm", "Unrelated later commit")
        result = self.run_prepare("v0.0.2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("HEAD", result.stderr)
        self.assertFalse(self.output.exists())

    def test_cmake_and_appstream_must_match_tag(self) -> None:
        self.git("tag", "v0.0.3")
        result = self.run_prepare("v0.0.3")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CMake version", result.stderr)
        self.assertFalse(self.output.exists())

    def test_staged_and_unstaged_changes_are_rejected(self) -> None:
        cmake = self.root / "CMakeLists.txt"
        original = cmake.read_bytes()
        cmake.write_bytes(original + b"\n# changed without a release tag\n")
        for staged in (False, True):
            with self.subTest(staged=staged):
                if staged:
                    self.git("add", "CMakeLists.txt")
                result = self.run_prepare("v0.0.2")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("not clean", result.stderr)
                self.assertFalse(self.output.exists())

    def test_untracked_sources_are_rejected(self) -> None:
        (self.root / "untracked.txt").write_text("new file", encoding="ascii")
        result = self.run_prepare("v0.0.2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not clean", result.stderr)

    def test_unknown_local_source_layout_fails_closed(self) -> None:
        text = self.manifest.read_text(encoding="utf-8")
        self.assertIn("skip: [.git, dist]", text)
        self.manifest.write_text(text.replace("skip: [.git, dist]", "skip: [.git]"), encoding="utf-8")
        self.git("add", MANIFEST)
        self.git("commit", "-qm", "Change manifest layout")
        self.git("tag", "-f", "v0.0.2")
        result = self.run_prepare("v0.0.2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source layout changed", result.stderr)
        self.assertFalse(self.output.exists())

    def test_refuses_to_destroy_a_different_generated_manifest(self) -> None:
        self.assertEqual(self.run_prepare("v0.0.2").returncode, 0)
        self.output.write_text("manually reviewed changes\n", encoding="ascii")
        result = self.run_prepare("v0.0.2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("already exists", result.stderr)
        self.assertEqual(self.output.read_text(encoding="ascii"), "manually reviewed changes\n")

    def test_release_workflow_produces_a_reviewable_artifact(self) -> None:
        workflow = (SOURCE / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn('python3 -I build/release/prepare_flathub.py "$RELEASE_TAG"', workflow)
        self.assertIn("name: flathub-manifest", workflow)
        self.assertIn("path: dist/flathub/${{ github.ref_name }}/io.github.the_beper.Kinefin.yml", workflow)
        self.assertLess(workflow.index("Prepare commit-pinned Flathub manifest"), workflow.index("  ci-gate:"))


if __name__ == "__main__":
    unittest.main()
