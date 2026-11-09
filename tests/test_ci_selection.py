#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""PR job selection must build both C++ and packaging when sources change."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "build/ci/changes.sh"


class ChangeSelection(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.repo = Path(self.dir.name)
        self.git("init", "-q")
        self.git("config", "user.name", "CI Test")
        self.git("config", "user.email", "ci@example.invalid")
        self.write("README.md", "readme\n")
        self.write("src/player/PlaybackSession.cpp", "// baseline\n")
        self.git("add", ".")
        self.git("commit", "-qm", "baseline")
        self.base = self.git("rev-parse", "HEAD").stdout.strip()

    def git(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["git", *args], cwd=self.repo, text=True, capture_output=True,
                              check=True, timeout=15)

    def write(self, path: str, value: str) -> None:
        p = self.repo / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(value, encoding="utf-8")

    def selection(self, event: str = "pull_request", base: str | None = None) -> str:
        env = os.environ.copy()
        env.pop("GITHUB_OUTPUT", None)
        env.update(EVENT_NAME=event, BASE_SHA=self.base if base is None else base)
        result = subprocess.run(["sh", str(SCRIPT)], cwd=self.repo, env=env,
                                text=True, capture_output=True, check=True, timeout=15)
        return result.stdout

    def test_cpp_change_requires_both_jobs(self) -> None:
        self.write("src/player/PlaybackSession.cpp", "// changed\n")
        self.git("add", ".")
        self.git("commit", "-qm", "cpp")
        self.assertIn("code=true packaging=true", self.selection())

    def test_readme_only_change_can_skip_both(self) -> None:
        self.write("README.md", "new readme\n")
        self.git("add", ".")
        self.git("commit", "-qm", "docs")
        self.assertIn("code=false packaging=false", self.selection())

    def test_new_python_test_requires_only_checks(self) -> None:
        self.write("tests/test_new.py", "# test\n")
        self.git("add", ".")
        self.git("commit", "-qm", "test")
        self.assertIn("code=true packaging=false", self.selection())

    def test_unknown_base_runs_all_jobs(self) -> None:
        self.assertIn("code=true packaging=true", self.selection(base="unknown"))

    def test_main_push_runs_all_jobs(self) -> None:
        self.assertIn("code=true packaging=true", self.selection(event="push"))


if __name__ == "__main__":
    unittest.main()
