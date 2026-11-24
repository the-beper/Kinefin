# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""The KRunner plugin file against the constants it must match.

KRunner reads a fixed key set from a Flatpak-exported runner description, and the description and
the runner must agree on the service name, the object path and the minimum length.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLUGIN = ROOT / "data" / "krunner" / "io.github.the_beper.Kinefin.desktop"
SOURCE = ROOT / "src" / "integration" / "KRunnerProvider.h"

# Flatpak exports a runner description only from 1.16, and that version already forces EnabledByDefault off; since 1.18.4 an exported file keeps only the allowlisted keys of flatpak-dir.c, which are listed here plus EnabledByDefault, which Flatpak drops on purpose and this file still carries so a native install starts off like the Flatpak
ALLOWED_KEYS = {
    "Type",
    "Name",
    "Comment",
    "Icon",
    "X-KDE-PluginInfo-Name",
    "X-KDE-PluginInfo-Version",
    "X-KDE-PluginInfo-License",
    "X-KDE-PluginInfo-Author",
    "X-KDE-PluginInfo-Email",
    "X-KDE-PluginInfo-EnabledByDefault",
    "X-Plasma-API",
    "X-Plasma-DBusRunner-Service",
    "X-Plasma-DBusRunner-Path",
    "X-Plasma-Request-Actions-Once",
    "X-Plasma-Match-Regex",
    "X-Plasma-Runner-Min-Letter-Count",
    "X-Plasma-Runner-Syntaxes",
    "X-Plasma-Runner-Syntax-Descriptions",
    "X-Plasma-Runner-Unique-Results",
    "X-Plasma-Runner-Weak-Results",
}


def read_desktop(path: Path) -> dict[str, str]:
    entries = {}
    group = None
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            group = line[1:-1]
            continue
        if group != "Desktop Entry" or "=" not in line:
            continue
        key, value = line.split("=", 1)
        entries[key.strip()] = value.strip()
    return entries


def constant(name: str) -> str:
    source = SOURCE.read_text(encoding="utf-8")
    match = re.search(rf"\b{name}\s*=\s*(\"([^\"]*)\"|\d+)", source)
    if not match:
        raise AssertionError(f"{name} not found in {SOURCE}")
    return match.group(2) if match.group(2) is not None else match.group(1)


class KRunnerPluginTest(unittest.TestCase):
    def setUp(self):
        self.plugin = read_desktop(PLUGIN)

    def test_the_file_is_named_after_the_app_id(self):
        # Flatpak exports only <app-id>.desktop and <app-id>.<sub>.desktop
        self.assertEqual(PLUGIN.name, "io.github.the_beper.Kinefin.desktop")

    def test_the_runner_is_off_by_default_and_uses_the_session_bus(self):
        self.assertEqual(self.plugin["Type"], "Service")
        self.assertEqual(self.plugin["X-Plasma-API"], "DBus")
        self.assertEqual(self.plugin["X-KDE-PluginInfo-EnabledByDefault"], "false")

    def test_the_service_path_and_minimum_length_match_the_runner(self):
        self.assertEqual(self.plugin["X-Plasma-DBusRunner-Service"],
                         constant("ServiceName") + "*")
        self.assertEqual(self.plugin["X-Plasma-DBusRunner-Path"], constant("ObjectPath"))
        self.assertEqual(int(self.plugin["X-Plasma-Runner-Min-Letter-Count"]),
                         int(constant("MinTermLength")))

    def test_the_comment_says_what_is_sent(self):
        comment = self.plugin["Comment"].lower()
        self.assertIn("jellyfin server", comment)
        self.assertIn("sent to that server", comment)

    def test_every_key_survives_flatpaks_export_or_is_meant_to_be_dropped(self):
        for key in self.plugin:
            base = key.split("[", 1)[0]
            self.assertIn(base, ALLOWED_KEYS, f"{key} would be dropped by Flatpak")

    def test_the_plugin_has_no_exec_or_session_bus_activation(self):
        # KRunner calls the running app, never starts it: no Exec, no D-Bus service file
        self.assertNotIn("Exec", self.plugin)
        self.assertNotIn("DBusActivatable", self.plugin)


if __name__ == "__main__":
    unittest.main()
