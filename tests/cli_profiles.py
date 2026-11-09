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


if __name__ == "__main__":
    BINARY = os.path.abspath(sys.argv.pop(1))
    unittest.main()
