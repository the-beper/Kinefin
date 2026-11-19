#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs the real kinefin binary with the profile flags, with no session bus: the commands must work before the single-instance service and exit with the documented codes.

Usage: cli_profiles.py <kinefin binary>.
"""
import os
import subprocess
import sys
import tempfile
import unittest

BINARY = ""


class ProfileFlags(unittest.TestCase):
    def setUp(self) -> None:
        self.home = tempfile.TemporaryDirectory()
        self.addCleanup(self.home.cleanup)
        self.env = dict(
            os.environ,
            QT_QPA_PLATFORM="offscreen",
            XDG_DATA_HOME=f"{self.home.name}/data",
            XDG_CONFIG_HOME=f"{self.home.name}/config",
            XDG_CACHE_HOME=f"{self.home.name}/cache",
            # No bus on purpose: a command that needed D-Bus would hang or fail here.
            DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent",
        )
        self.env.pop("XDG_RUNTIME_DIR", None)

    def run_kinefin(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([BINARY, *arguments], env=self.env, capture_output=True, text=True, timeout=30)

    def test_list_runs_without_a_bus(self) -> None:
        result = self.run_kinefin("--list-profiles")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("* default\tDefault", result.stdout)

    def test_create_default_and_delete(self) -> None:
        self.assertEqual(self.run_kinefin("--create-profile", "work").returncode, 0)
        self.assertEqual(self.run_kinefin("--set-default-profile", "work").returncode, 0)
        self.assertIn("* work\twork", self.run_kinefin("--list-profiles").stdout)
        self.assertEqual(self.run_kinefin("--delete-profile", "work").returncode, 1)  # the default
        self.assertEqual(self.run_kinefin("--set-default-profile", "default").returncode, 0)
        self.assertEqual(self.run_kinefin("--delete-profile", "work").returncode, 0)
        self.assertNotIn("work", self.run_kinefin("--list-profiles").stdout)

    def test_unknown_profile_exits_2(self) -> None:
        result = self.run_kinefin("--profile", "nope")
        self.assertEqual(result.returncode, 2)
        self.assertIn("nope", result.stderr)

    def test_commands_are_exclusive(self) -> None:
        self.assertEqual(self.run_kinefin("--list-profiles", "--create-profile", "x").returncode, 2)
        self.assertEqual(self.run_kinefin("--list-profiles", "--profile", "default").returncode, 2)

    def test_bad_log_level_exits_2(self) -> None:
        self.assertEqual(self.run_kinefin("--log-level", "loud").returncode, 2)

    def test_config_dir_moves_everything_under_one_root(self) -> None:
        root = f"{self.home.name}/root"
        result = self.run_kinefin("--config-dir", root, "--list-profiles")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("* default\tDefault", result.stdout)
        self.assertTrue(os.path.exists(f"{root}/profiles.json"))
        self.assertFalse(os.path.exists(f"{self.home.name}/config/kinefin/profiles.json"))
        self.assertEqual(self.run_kinefin("--config-dir", root, "--create-profile", "work").returncode, 0)
        self.assertIn("work", self.run_kinefin("--config-dir", root, "--list-profiles").stdout)
        self.assertFalse(os.path.exists(f"{self.home.name}/config/kinefin"))

    def test_config_dir_needs_an_existing_parent(self) -> None:
        result = self.run_kinefin("--config-dir", f"{self.home.name}/missing/root", "--list-profiles")
        self.assertEqual(result.returncode, 1)
        self.assertIn("parent", result.stderr)

    def test_config_dir_refuses_a_folder_of_other_files(self) -> None:
        root = f"{self.home.name}/documents"
        os.mkdir(root, 0o755)
        with open(f"{root}/notes.txt", "w", encoding="utf-8") as notes:
            notes.write("x")
        result = self.run_kinefin("--config-dir", root, "--list-profiles")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(os.stat(root).st_mode & 0o777, 0o755)
        self.assertFalse(os.path.exists(f"{root}/profiles.json"))

    def test_config_dir_refuses_a_file(self) -> None:
        root = f"{self.home.name}/file"
        with open(root, "w", encoding="utf-8") as file:
            file.write("x")
        self.assertEqual(self.run_kinefin("--config-dir", root, "--list-profiles").returncode, 1)

    def test_config_dir_accepts_an_empty_folder_and_a_relative_path(self) -> None:
        os.mkdir(f"{self.home.name}/empty")
        result = subprocess.run([BINARY, "--config-dir", "empty", "--list-profiles"], env=self.env,
                                capture_output=True, text=True, timeout=30, cwd=self.home.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(os.path.exists(f"{self.home.name}/empty/profiles.json"))

    def test_config_dir_needs_a_value(self) -> None:
        self.assertEqual(self.run_kinefin("--config-dir=", "--list-profiles").returncode, 2)

    def test_licenses_prints_the_gpl(self) -> None:
        result = self.run_kinefin("--licenses")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("GNU GENERAL PUBLIC LICENSE", result.stdout)

    def test_platform_equals_form_is_accepted(self) -> None:
        # No inherited platform: only the pre-scan can make this run headless
        self.env.pop("QT_QPA_PLATFORM")
        result = self.run_kinefin("--platform=offscreen", "--list-profiles")
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    BINARY = os.path.abspath(sys.argv.pop(1))
    unittest.main()
