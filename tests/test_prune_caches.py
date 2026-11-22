# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline tests for the cache pruning script of CI."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import unittest
from unittest import mock


SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prune_caches", SOURCE / "build/ci/prune_caches.py")
prune_caches = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prune_caches)


def cache(name: str, created: str) -> dict:
    return {"id": hash(name) % 1000, "key": name, "created_at": created, "size_in_bytes": 1}


class StaleCaches(unittest.TestCase):
    CACHES = [
        cache("a", "2026-11-01T10:00:00Z"),
        cache("c", "2026-11-03T10:00:00Z"),
        cache("b", "2026-11-02T10:00:00Z"),
    ]

    def test_keeps_the_newest_whatever_the_order(self) -> None:
        self.assertEqual([c["key"] for c in prune_caches.stale(self.CACHES, 2)], ["a"])

    def test_keep_zero_deletes_everything(self) -> None:
        self.assertEqual(len(prune_caches.stale(self.CACHES, 0)), 3)

    def test_keeping_more_than_there_are_deletes_nothing(self) -> None:
        self.assertEqual(prune_caches.stale(self.CACHES, 5), [])

    def test_missing_arguments_are_refused(self) -> None:
        self.assertEqual(prune_caches.main(["prune_caches.py"]), 2)
        self.assertEqual(prune_caches.main(["prune_caches.py", "key", "two"]), 2)


class MainWithAFakeApi(unittest.TestCase):
    ENV = {"GITHUB_REPOSITORY": "owner/repo", "GH_TOKEN": "token"}

    def run_main(self, listing: list[dict] | Exception, keep: str = "2") -> tuple[int, list[str], str]:
        calls: list[str] = []

        def fake_request(method: str, path: str, token: str) -> bytes:
            calls.append(f"{method} {path.split('?')[0]}")
            if method == "GET":
                if isinstance(listing, Exception):
                    raise listing
                return json.dumps({"actions_caches": listing}).encode()
            return b""

        out = io.StringIO()
        with mock.patch.dict(os.environ, self.ENV), mock.patch.object(prune_caches, "request", fake_request), \
                contextlib.redirect_stdout(out):
            code = prune_caches.main(["prune_caches.py", "state-", keep])
        return code, calls, out.getvalue()

    def test_deletes_the_older_caches_of_the_prefix_only(self) -> None:
        listing = [
            {**cache("state-a", "2026-11-01T10:00:00Z"), "id": 1},
            {**cache("state-b", "2026-11-02T10:00:00Z"), "id": 2},
            {**cache("state-c", "2026-11-03T10:00:00Z"), "id": 3},
            {**cache("other-z", "2026-10-01T10:00:00Z"), "id": 9},
        ]
        code, calls, _ = self.run_main(listing)
        self.assertEqual(code, 0)
        self.assertEqual(calls, ["GET /repos/owner/repo/actions/caches", "DELETE /repos/owner/repo/actions/caches/1"])

    def test_a_failing_api_is_a_warning_not_a_failure(self) -> None:
        for error in (TimeoutError("timed out"), OSError("connection reset")):
            with self.subTest(error=error):
                code, calls, out = self.run_main(error)
                self.assertEqual(code, 0)
                self.assertEqual(calls, ["GET /repos/owner/repo/actions/caches"])
                self.assertIn("::warning::prune_caches", out)

    def test_an_unexpected_answer_is_a_warning_too(self) -> None:
        code, _, out = self.run_main([{"id": 1}])
        self.assertEqual(code, 0)
        self.assertIn("::warning::prune_caches", out)


if __name__ == "__main__":
    unittest.main()
