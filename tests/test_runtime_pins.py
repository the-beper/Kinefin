# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline regression tests for shared KDE Flatpak runtime pins."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]
FILES = (
    "build/flatpak/runtime-pins.conf",
    "build/flatpak/build-flatpak.sh",
    "build/flatpak/test-flatpak.sh",
    "build/flatpak/io.github.the_beper.Kinefin.yml",
    "build/release/verify_runtime_pins.py",
)
PIN_FILE, BUILD, SMOKE, MANIFEST, VERIFIER = FILES


class RuntimePinsTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "test checkout"
        for file in FILES:
            output = self.root / file
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(SOURCE / file, output)

    def run_verify(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-I", str(self.root / VERIFIER)],
            cwd=self.root.parent, capture_output=True, text=True, timeout=10, check=False,
        )

    def change(self, file: str, old: str, new: str) -> None:
        target = self.root / file
        text = target.read_text(encoding="utf-8")
        self.assertIn(old, text)
        target.write_text(text.replace(old, new), encoding="utf-8")

    def assert_fails(self, message: str) -> None:
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(message, result.stderr)

    def test_shared_pins_validate_from_another_directory(self) -> None:
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("org.kde.Sdk//6.11", result.stdout)
        self.assertIn("org.kde.Platform//6.11", result.stdout)

    def test_shell_loads_the_shared_immutable_pins(self) -> None:
        script = '. "$1"; printf "%s\\n" "$sdk" "$platform" "$sdk_commit" "$platform_commit"'
        result = subprocess.run(
            ["sh", "-eu", "-c", script, "sh", str(self.root / PIN_FILE)],
            capture_output=True, text=True, timeout=10, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        refs = result.stdout.splitlines()
        self.assertEqual(refs[:2], ["org.kde.Sdk//6.11", "org.kde.Platform//6.11"])
        for commit in refs[2:]:
            self.assertEqual(len(commit), 64)
            self.assertEqual(commit, commit.lower())

    def test_invalid_sha256s_fail_closed(self) -> None:
        original = (self.root / PIN_FILE).read_text(encoding="utf-8")
        old = "sdk_commit=" + original.split("sdk_commit=", 1)[1].splitlines()[0]
        for bad in ("fedora45", "a" * 63, "A" * 64, "0" * 64, "z" * 64):
            with self.subTest(bad=bad):
                (self.root / PIN_FILE).write_text(original.replace(old, "sdk_commit=" + bad), encoding="utf-8")
                self.assert_fails("sdk_commit must be a nonzero lowercase SHA-256")

    def test_duplicate_unknown_and_unsafe_shell_assignments_are_rejected(self) -> None:
        original = (self.root / PIN_FILE).read_text(encoding="utf-8")
        for extra, message in (
            ("sdk=org.kde.Sdk//6.11\n", "duplicate"),
            ("other=anything\n", "unknown"),
            ("sdk=$(rm -rf anything)\n", "invalid"),
            ("export sdk=org.kde.Sdk//6.11\n", "invalid"),
            ("# extra harmless comment\n; echo dangerous\n", "invalid"),
        ):
            with self.subTest(extra=extra):
                (self.root / PIN_FILE).write_text(original + extra, encoding="utf-8")
                self.assert_fails(message)

    def test_missing_pin_fails_closed(self) -> None:
        original = (self.root / PIN_FILE).read_text(encoding="utf-8")
        removed = next(line for line in original.splitlines(keepends=True) if line.startswith("platform_commit="))
        (self.root / PIN_FILE).write_text(original.replace(removed, ""), encoding="utf-8")
        self.assert_fails("missing keys: platform_commit")

    def test_sdk_and_platform_refs_must_match_manifest(self) -> None:
        for key in ("sdk", "platform"):
            with self.subTest(key=key):
                original = (self.root / PIN_FILE).read_text(encoding="utf-8")
                (self.root / PIN_FILE).write_text(original.replace(f"{key}=org.kde.", f"{key}=org.gnome."), encoding="utf-8")
                self.assert_fails(f"{key} pin")
                (self.root / PIN_FILE).write_text(original, encoding="utf-8")

    def test_manifest_runtime_upgrade_requires_corresponding_pin_upgrade(self) -> None:
        self.change(MANIFEST, 'runtime-version: "6.11"', 'runtime-version: "6.12"')
        self.assert_fails("does not match manifest")

    def test_missing_duplicate_or_wrong_manifest_fields_are_rejected(self) -> None:
        original = (self.root / MANIFEST).read_text(encoding="utf-8")
        for old, new, message in (
            ("runtime: org.kde.Platform\n", "", "exactly one top-level runtime"),
            ("sdk: org.kde.Sdk\n", "sdk: org.kde.Sdk\nsdk: org.kde.Sdk\n", "exactly one top-level sdk"),
            ("sdk: org.kde.Sdk\n", "sdk: org.gnome.Sdk\n", "must use the KDE Platform and SDK"),
            ('runtime-version: "6.11"\n', 'runtime-version: "latest"\n', "invalid Flatpak runtime-version"),
        ):
            with self.subTest(old=old, new=new):
                (self.root / MANIFEST).write_text(original.replace(old, new), encoding="utf-8")
                self.assert_fails(message)

    def test_build_and_smoke_must_source_shared_pins(self) -> None:
        source = '. "$(dirname "$0")/runtime-pins.conf"'
        for file, name in ((BUILD, "build-flatpak.sh"), (SMOKE, "test-flatpak.sh")):
            with self.subTest(file=file):
                original = (self.root / file).read_text(encoding="utf-8")
                self.change(file, source, "# runtime pins accidentally removed")
                self.assert_fails(f"{name} must source")
                (self.root / file).write_text(original, encoding="utf-8")
                self.change(file, source, source + "\n" + source)
                self.assert_fails(f"{name} must source")
                (self.root / file).write_text(original, encoding="utf-8")

    def test_smoke_must_read_only_mount_the_pins(self) -> None:
        mount = '-v "$root/build/flatpak/runtime-pins.conf:/runtime-pins.conf:ro"'
        self.change(SMOKE, mount, '-v "$root/build/flatpak/runtime-pins.conf:/runtime-pins.conf"')
        self.assert_fails("read-only")

    def test_no_script_may_shadow_the_shared_pins(self) -> None:
        self.change(BUILD, "pin() {", "sdk_commit=" + "1" * 64 + "\npin() {")
        self.assert_fails("duplicate hard-coded")

    def test_all_shell_files_are_valid_posix_syntax(self) -> None:
        result = subprocess.run(
            ["sh", "-n", *(str(self.root / path) for path in (PIN_FILE, BUILD, SMOKE))],
            capture_output=True, text=True, timeout=10, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_both_ci_and_release_gate_run_before_flatpak_build(self) -> None:
        ci = (SOURCE / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        release = (SOURCE / ".github/workflows/release.yml").read_text(encoding="utf-8")
        command = "python3 -I build/release/verify_runtime_pins.py"
        self.assertIn(command, ci)
        self.assertLess(ci.index(command), ci.index("  flatpak:"))
        self.assertIn(command, release)
        self.assertLess(release.index(command), release.index("  build-flatpak:"))
        self.assertIn('pinned "$sdk" "$sdk_commit"', (SOURCE / BUILD).read_text(encoding="utf-8"))
        self.assertIn('pinned "$platform" "$platform_commit"', (SOURCE / BUILD).read_text(encoding="utf-8"))
        self.assertIn('assert_pin "$sdk" "$sdk_commit"', (SOURCE / SMOKE).read_text(encoding="utf-8"))
        self.assertIn('assert_pin "$platform" "$platform_commit"', (SOURCE / SMOKE).read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
