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

    def selection(self, event: str = "pull_request", base: str | None = None,
                  before: str | None = None) -> str:
        env = os.environ.copy()
        env.pop("GITHUB_OUTPUT", None)
        env.pop("BEFORE_SHA", None)
        env.update(EVENT_NAME=event, BASE_SHA=self.base if base is None else base)
        if before is not None:
            env.update(BEFORE_SHA=before)
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

    def test_settings_reference_change_requires_checks(self) -> None:
        self.write("docs/settings.md", "| Setting | Key |\n")
        self.git("add", ".")
        self.git("commit", "-qm", "settings reference")
        self.assertIn("code=true packaging=false", self.selection())

    def test_new_python_test_requires_only_checks(self) -> None:
        self.write("tests/test_new.py", "# test\n")
        self.git("add", ".")
        self.git("commit", "-qm", "test")
        self.assertIn("code=true packaging=false", self.selection())

    def test_unknown_base_runs_all_jobs(self) -> None:
        self.assertIn("code=true packaging=true", self.selection(base="unknown"))

    def test_main_push_runs_all_jobs(self) -> None:
        self.assertIn("code=true packaging=true", self.selection(event="push"))

    def head(self) -> str:
        return self.git("rev-parse", "HEAD").stdout.strip()

    def commit_file(self, path: str) -> None:
        self.write(path, "changed\n")
        self.git("add", ".")
        self.git("commit", "-qm", path)

    def test_push_changing_an_image_input_waits_for_images(self) -> None:
        for path in ("build/Containerfile", "build/flatpak/Containerfile",
                     ".github/workflows/publish-images.yml"):
            with self.subTest(path=path):
                before = self.head()
                self.commit_file(path)
                self.assertIn("code=true packaging=true images=true",
                              self.selection(event="push", before=before))

    def test_image_input_changed_in_an_earlier_pushed_commit_waits(self) -> None:
        before = self.head()
        self.commit_file("build/Containerfile")
        self.commit_file("src/player/PlaybackSession.cpp")
        self.assertIn("images=true", self.selection(event="push", before=before))

    def test_ordinary_push_does_not_wait_for_images(self) -> None:
        before = self.head()
        self.commit_file("build/check.sh")
        self.assertIn("code=true packaging=true images=false",
                      self.selection(event="push", before=before))

    def test_push_with_unknown_previous_commit_may_wait_for_images(self) -> None:
        self.commit_file("build/Containerfile")
        for before in ("0" * 40, "unknown", "1" * 40):
            with self.subTest(before=before):
                self.assertIn("code=true packaging=true images=maybe",
                              self.selection(event="push", before=before))
        self.assertIn("images=maybe", self.selection(event="push"))

    def test_unknown_previous_commit_warns_on_stderr(self) -> None:
        env = os.environ.copy()
        env.pop("GITHUB_OUTPUT", None)
        env.update(EVENT_NAME="push", BEFORE_SHA="1" * 40)
        result = subprocess.run(["sh", str(SCRIPT)], cwd=self.repo, env=env, text=True,
                                capture_output=True, check=True, timeout=15)
        self.assertIn("images=maybe", result.stderr)

    def test_pull_request_and_manual_runs_never_wait_for_images(self) -> None:
        self.commit_file("build/Containerfile")
        self.assertIn("images=false", self.selection())
        self.assertIn("images=false", self.selection(event="workflow_dispatch", before=self.base))
        self.assertIn("images=false", self.selection(base="unknown"))

    def test_outputs_file_gets_maybe_for_an_unknown_previous_commit(self) -> None:
        out = self.repo / "github-output"
        env = os.environ.copy()
        env.update(EVENT_NAME="push", BEFORE_SHA="1" * 40, GITHUB_OUTPUT=str(out))
        subprocess.run(["sh", str(SCRIPT)], cwd=self.repo, env=env, text=True,
                       capture_output=True, check=True, timeout=15)
        self.assertEqual(out.read_text().splitlines(),
                         ["code=true", "packaging=true", "images=maybe"])

    def test_outputs_file_gets_the_images_flag(self) -> None:
        before = self.head()
        self.commit_file("build/Containerfile")
        out = self.repo / "github-output"
        env = os.environ.copy()
        env.update(EVENT_NAME="push", BEFORE_SHA=before, GITHUB_OUTPUT=str(out))
        subprocess.run(["sh", str(SCRIPT)], cwd=self.repo, env=env, text=True,
                       capture_output=True, check=True, timeout=15)
        self.assertEqual(out.read_text().splitlines(),
                         ["code=true", "packaging=true", "images=true"])


if __name__ == "__main__":
    unittest.main()
