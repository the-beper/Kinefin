# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline tests for immutable GitHub Actions workflow and composite-action refs."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]
SCRIPT = SOURCE / "build/release/verify_action_pins.py"
sys.path.insert(0, str(SCRIPT.parent))
import verify_action_pins as sut  # noqa: E402
SHA = "a" * 40
DIGEST = "b" * 64


class ActionPinTests(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "repo with spaces"
        self.workflows = self.root / ".github/workflows"
        self.workflows.mkdir(parents=True)
        self.workflow = self.workflows / "ci.yml"
        self.write("actions/checkout@" + SHA)

    def write(self, ref: str) -> None:
        self.workflow.write_text(
            "name: Test\non: [push]\njobs:\n  test:\n    runs-on: ubuntu-latest\n"
            f"    steps:\n      - uses: {ref}\n",
            encoding="utf-8",
        )

    def scan(self) -> tuple[int, int, int]:
        return sut.scan(self.root)

    def rejects(self, expected: str) -> None:
        with self.assertRaises(sut.ActionPinError) as caught:
            self.scan()
        self.assertIn(expected, str(caught.exception))

    def test_real_workflows_are_pinned_without_mutation(self) -> None:
        before = {
            p: p.read_bytes() for p in (SOURCE / ".github/workflows").glob("*.yml")
        }
        result = subprocess.run(
            [sys.executable, "-I", str(SCRIPT)],
            capture_output=True, text=True, timeout=10, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout, r"action-pins: [1-9][0-9]* pinned external uses")
        self.assertEqual({p: p.read_bytes() for p in before}, before)

    def test_normal_and_nested_git_actions_are_accepted(self) -> None:
        self.write("owner/repo/subdir/action@" + SHA)
        self.assertEqual(self.scan(), (1, 0, 1))

    def test_quotes_and_version_comments_are_supported(self) -> None:
        self.write("'actions/checkout@" + SHA + "' # v4.2.0")
        self.assertEqual(self.scan(), (1, 0, 1))
        self.write('"actions/checkout@' + SHA + '" # pinned')
        self.assertEqual(self.scan(), (1, 0, 1))

    def test_tags_branches_and_short_or_uppercase_hashes_are_rejected(self) -> None:
        for ref in (
            "actions/checkout@v4", "actions/checkout@main", "actions/checkout@deadbeef",
            "actions/checkout@" + "A" * 40, "actions/checkout@" + "0" * 40,
            "actions/checkout@" + "a" * 39, "actions/checkout",
            "${{ github.event.inputs.action }}", "actions/checkout@" + SHA + "/unexpected",
        ):
            with self.subTest(ref=ref):
                self.write(ref)
                with self.assertRaises(sut.ActionPinError):
                    self.scan()

    def test_rejects_invalid_repositories_and_malformed_values(self) -> None:
        for ref in (
            "owner/../repo@" + SHA, "owner//repo@" + SHA,
            "owner/repo/subdir/../next@" + SHA,
            "", "'actions/checkout@" + SHA, "actions/checkout@" + SHA + " extra",
        ):
            with self.subTest(ref=ref):
                self.write(ref)
                with self.assertRaises(sut.ActionPinError):
                    self.scan()

    def test_only_digest_pinned_docker_actions_are_allowed(self) -> None:
        self.write("docker://ghcr.io/example/my-action@sha256:" + DIGEST)
        self.assertEqual(self.scan(), (1, 0, 1))
        self.write("docker://ghcr.io/example/my-action:latest")
        self.rejects("requires")
        self.write("docker://ghcr.io/example/my-action@sha256:" + "0" * 64)
        self.rejects("nonzero SHA256")

    def test_local_composite_action_and_reusable_workflow_are_allowed(self) -> None:
        action = self.root / ".github/actions/verify"
        action.mkdir(parents=True)
        (action / "action.yml").write_text(
            "name: Composite\nruns:\n  using: composite\n  steps:\n"
            f"    - uses: owner/other@{SHA}\n",
            encoding="utf-8",
        )
        reusable = self.workflows / "reuse.yaml"
        reusable.write_text("name: Reusable\non: workflow_call\njobs: {}\n", encoding="utf-8")
        self.workflow.write_text(
            "name: Test\non: push\njobs:\n  build:\n    steps:\n"
            "      - uses: ./.github/actions/verify\n"
            "  downstream:\n    uses: ./.github/workflows/reuse.yaml\n",
            encoding="utf-8",
        )
        self.assertEqual(self.scan(), (1, 2, 3))

    def test_local_refs_must_exist_and_remain_in_checkout(self) -> None:
        for ref in (
            "./.github/actions/missing", "./.github/workflows/missing.yml",
            "./.github/actions/../../outside", "./.github/actions/verify/../missing",
            "./scripts/run.sh", "../outside/action", "./.github/workflows/../actions/a.yml",
        ):
            with self.subTest(ref=ref):
                self.write(ref)
                with self.assertRaises(sut.ActionPinError):
                    self.scan()

    def test_symlinked_workflow_is_rejected(self) -> None:
        other = self.workflows / "other.yml"
        other.write_text("name: Other\non: push\njobs: {}\n", encoding="utf-8")
        self.workflow.unlink()
        self.workflow.symlink_to(other)
        self.rejects("symlink")

    def test_symlinked_composite_directory_is_rejected(self) -> None:
        action_root = self.root / ".github/actions"
        action_root.mkdir(parents=True)
        outside = self.root.parent / "other"
        outside.mkdir()
        (outside / "action.yml").write_text("name: Other\n", encoding="utf-8")
        (action_root / "external").symlink_to(outside, target_is_directory=True)
        self.rejects("symlinks under .github/actions")

    def test_composite_actions_cannot_hide_mutable_references(self) -> None:
        action = self.root / ".github/actions/verify"
        action.mkdir(parents=True)
        (action / "action.yaml").write_text(
            "runs:\n  using: composite\n  steps:\n    - uses: actions/checkout@v4\n",
            encoding="utf-8",
        )
        self.rejects("requires")

    def test_script_blocks_are_ignored_but_real_uses_are_checked(self) -> None:
        self.workflow.write_text(
            "name: Test\non: push\njobs:\n  test:\n    steps:\n"
            "      - run: |\n        echo 'uses: actions/checkout@main'\n"
            "        echo '<<: *alias'\n"
            "      - uses: actions/checkout@" + SHA + "\n",
            encoding="utf-8",
        )
        self.assertEqual(self.scan(), (1, 0, 1))

    def test_nonstandard_uses_forms_are_rejected(self) -> None:
        for line in (
            "      - {uses: actions/checkout@" + SHA + "}",
            '      - "uses": actions/checkout@' + SHA,
            "      - uses : actions/checkout@" + SHA,
            "      - uses: *shared",
            "      - uses: >\n        actions/checkout@" + SHA,
        ):
            with self.subTest(line=line):
                self.workflow.write_text("name: Test\non: push\njobs:\n  test:\n    steps:\n" + line + "\n", encoding="utf-8")
                with self.assertRaises(sut.ActionPinError):
                    self.scan()

    def test_yaml_aliases_and_merges_are_rejected(self) -> None:
        self.write("actions/checkout@" + SHA)
        with self.workflow.open("a", encoding="utf-8") as handle:
            handle.write("      - name: Other\n        with: &defaults\n          foo: bar\n")
        self.rejects("anchors, aliases or merges")
        self.write("actions/checkout@" + SHA)
        with self.workflow.open("a", encoding="utf-8") as handle:
            handle.write("      - <<: *defaults\n")
        self.rejects("anchors, aliases or merges")

    def test_no_workflow_fails_closed(self) -> None:
        self.workflow.unlink()
        self.rejects("no GitHub Actions workflow files")

    def test_release_and_ci_validate_before_build_jobs(self) -> None:
        command = "python3 -I build/release/verify_action_pins.py"
        ci = (SOURCE / ".github/workflows/verify.yml").read_text(encoding="utf-8")
        release = (SOURCE / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertEqual(ci.count(command), 1)
        self.assertEqual(release.count(command), 1)
        self.assertLess(ci.index(command), ci.index("  check:"))
        self.assertLess(release.index(command), release.index("  build-flatpak:"))
        self.assertLess(release.index(command), release.index("Prepare commit-pinned Flathub manifest"))


if __name__ == "__main__":
    unittest.main()
