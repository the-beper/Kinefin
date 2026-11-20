#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixture conformance and in-process REST preflight checks."""
from __future__ import annotations
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests" / "fixtures"


class FixtureContracts(unittest.TestCase):
    def fixture(self, name: str) -> dict:
        document = json.loads((FIXTURES / f"{name}.json").read_text(encoding="utf-8"))
        self.assertIsInstance(document, dict, name)
        return document

    def test_all_json_syntax(self):
        paths = list(FIXTURES.glob("*.json"))
        self.assertGreaterEqual(len(paths), 9)
        for path in paths:
            with self.subTest(path=path.name):
                json.loads(path.read_text(encoding="utf-8"))

    def test_paging_envelopes(self):
        for name in ("user_views", "movie_item", "episodes_page"):
            with self.subTest(name=name):
                payload = self.fixture(name)
                self.assertIsInstance(payload.get("Items"), list)
                self.assertIsInstance(payload.get("TotalRecordCount"), int)
                self.assertIsInstance(payload.get("StartIndex"), int)
                self.assertGreaterEqual(payload["TotalRecordCount"], len(payload["Items"]))

    def test_user_views_types(self):
        payload = self.fixture("user_views")
        self.assertGreater(len(payload["Items"]), 0)
        self.assertTrue(all("Id" in i and "Name" in i for i in payload["Items"]))

    def test_login_shape(self):
        payload = self.fixture("authenticate_success")
        self.assertTrue(payload.get("AccessToken"))
        self.assertTrue(payload.get("User", {}).get("Id"))
        self.assertTrue(payload.get("ServerId"))

    def test_playbackinfo_direct_and_transcoded(self):
        direct = self.fixture("playbackinfo_direct")
        trans = self.fixture("playbackinfo_transcode")
        self.assertTrue(any(s.get("SupportsDirectPlay") for s in direct["MediaSources"]))
        self.assertTrue(any(s.get("SupportsTranscoding") and not s.get("SupportsDirectPlay")
                            and s.get("TranscodingUrl") for s in trans["MediaSources"]))
        self.assertTrue(direct.get("PlaySessionId"))
        self.assertTrue(trans.get("PlaySessionId"))

    def test_playback_report_goldens(self):
        for name in ("playback_start_request", "playback_progress_request", "playback_stopped_request"):
            with self.subTest(name=name):
                payload = self.fixture(name)
                self.assertGreaterEqual(payload["PositionTicks"], 0)
                self.assertNotIn("Item", payload)
                self.assertNotIn("SessionId", payload)
        stopped = self.fixture("playback_stopped_request")
        self.assertIn("PositionTicks", stopped)  # a Stopped without a position marks the item played
        self.assertIn("Failed", stopped)
        for name in ("playback_start_request", "playback_progress_request"):
            self.assertIn(self.fixture(name)["PlayMethod"], ("DirectPlay", "Transcode"))

    def test_quick_connect_pairing_fixtures(self):
        initiated = self.fixture("quick_connect_initiate")
        pending = self.fixture("quick_connect_pending")
        approved = self.fixture("quick_connect_approved")
        self.assertIs(initiated.get("Authenticated"), False)
        self.assertIs(pending.get("Authenticated"), False)
        self.assertIs(approved.get("Authenticated"), True)
        self.assertTrue(initiated.get("Code"))
        self.assertTrue(initiated.get("Secret"))
        self.assertEqual(initiated["Secret"], pending["Secret"])
        self.assertEqual(initiated["Secret"], approved["Secret"])
        for response in (initiated, pending, approved):
            self.assertNotIn("AccessToken", response)
            self.assertNotIn("Pw", response)

    def test_fake_server_negotiation_probe(self):
        command = [sys.executable, str(ROOT / "scripts/testing/probe_jellyfin.py"), "--fixture"]
        result = subprocess.run(command, capture_output=True, text=True, timeout=15, check=True)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["server_identity_present"])
        self.assertGreater(payload["library_count"], 0)
        self.assertTrue(payload["negotiation_checked"])
        self.assertTrue(payload["direct_play_available"])
        self.assertFalse(payload["session_reporting_checked"])
        self.assertFalse(payload["mpv_playback_checked"])
        self.assertNotIn("AccessToken", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
