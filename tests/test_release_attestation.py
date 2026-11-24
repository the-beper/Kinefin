# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline tests for the signed release attestation gate and CLI contract."""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build/release"))
import verify_attestation as sut  # noqa: E402

REPO = "the-beper/Kinefin"
COMMIT = "a" * 40
TAG = "v1.2.3"


class SignedReleaseAttestationTest(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.bundle = self.root / "Kinefin-x86_64.flatpak"
        self.bundle.write_bytes(b"fake Flatpak payload")
        self.calls: list[list[str]] = []

    def fake_runner(self, args: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
        self.calls.append(args)
        self.assertFalse(kwargs["check"])
        self.assertEqual(kwargs["timeout"], 120)
        self.assertTrue(kwargs["capture_output"])
        self.assertTrue(kwargs["text"])
        return subprocess.CompletedProcess(args, 0, stdout="verified\n", stderr="")

    def test_verifies_exact_bundle_and_all_trusted_identities(self) -> None:
        sut.verify(self.bundle, REPO, COMMIT, TAG, runner=self.fake_runner)
        self.assertEqual(self.calls, [[
            "gh", "attestation", "verify", str(self.bundle.resolve()),
            "--repo", REPO,
            "--signer-workflow", f"{REPO}/.github/workflows/release.yml",
            "--source-digest", COMMIT,
            "--source-ref", f"refs/tags/{TAG}",
        ]])

    def test_invalid_untrusted_repository_tag_or_commit_never_calls_gh(self) -> None:
        invalid = (
            ("./repo", COMMIT, TAG),
            ("a/../repo", COMMIT, TAG),
            ("owner/repo.", COMMIT, TAG),
            (REPO, "0" * 40, TAG),
            (REPO, COMMIT.upper(), TAG),
            (REPO, COMMIT[:39], TAG),
            (REPO, COMMIT, "v1.2"),
            (REPO, COMMIT, "v01.2.3"),
            (REPO, COMMIT, "main"),
            (REPO, COMMIT, "v1.2.3; rm -rf ."),
        )
        for repo, commit, tag in invalid:
            with self.subTest(repo=repo, commit=commit, tag=tag):
                with self.assertRaises(sut.AttestationError):
                    sut.verify(self.bundle, repo, commit, tag, runner=self.fake_runner)
        self.assertFalse(self.calls)

    def test_missing_symlinked_and_empty_bundle_never_calls_gh(self) -> None:
        self.bundle.unlink()
        with self.assertRaisesRegex(sut.AttestationError, "missing or symlinked"):
            sut.verify(self.bundle, REPO, COMMIT, TAG, runner=self.fake_runner)
        outside = self.root / "outside"
        outside.write_bytes(b"bundle")
        self.bundle.symlink_to(outside)
        with self.assertRaisesRegex(sut.AttestationError, "missing or symlinked"):
            sut.verify(self.bundle, REPO, COMMIT, TAG, runner=self.fake_runner)
        self.bundle.unlink()
        self.bundle.write_bytes(b"")
        with self.assertRaisesRegex(sut.AttestationError, "empty Flatpak"):
            sut.verify(self.bundle, REPO, COMMIT, TAG, runner=self.fake_runner)
        self.assertFalse(self.calls)

    def test_gh_failure_is_never_treated_as_success(self) -> None:
        def failure(args: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(args, 1, stdout="", stderr="signature mismatch")

        with self.assertRaisesRegex(sut.AttestationError, "signature mismatch"):
            sut.verify(self.bundle, REPO, COMMIT, TAG, runner=failure)

    def test_gh_unavailable_or_timed_out_fails_closed(self) -> None:
        def unavailable(args: list[str], **kwargs):
            raise FileNotFoundError("gh is not installed")

        def timeout(args: list[str], **kwargs):
            raise subprocess.TimeoutExpired(args, 120)

        for runner in (unavailable, timeout):
            with self.subTest(runner=runner.__name__):
                with self.assertRaisesRegex(sut.AttestationError, "unable to verify"):
                    sut.verify(self.bundle, REPO, COMMIT, TAG, runner=runner)

    def test_gh_success_with_blank_output_is_allowed(self) -> None:
        def silent(args: list[str], **kwargs):
            return subprocess.CompletedProcess(args, 0, stdout="", stderr="")

        sut.verify(self.bundle, REPO, COMMIT, TAG, runner=silent)

    def cli(self, *, result_code: int = 0) -> tuple[subprocess.CompletedProcess[str], list[str]]:
        # Fake 'gh' executable: exercise the real isolated-mode Python CLI
        # without network and capture precisely what it asked GitHub to verify.
        directory = self.root / "bin"
        directory.mkdir(exist_ok=True)
        command = directory / "gh"
        command.write_text(
            "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$GH_ARGS_FILE\"\n"
            "if [ \"$GH_FAIL\" = 1 ]; then echo 'untrusted signer' >&2; exit 3; fi\n"
            "exit 0\n", encoding="ascii",
        )
        command.chmod(0o755)
        captured = self.root / "args.txt"
        env = dict(os.environ, PATH=f"{directory}:{os.environ.get('PATH', '')}",
                   GH_ARGS_FILE=str(captured), GH_FAIL=str(result_code))
        result = subprocess.run(
            [sys.executable, "-I", str(ROOT / "build/release/verify_attestation.py"),
             str(self.bundle), "--repository", REPO, "--commit", COMMIT, "--tag", TAG],
            capture_output=True, text=True, cwd=self.root, env=env, timeout=10,
        )
        return result, captured.read_text(encoding="ascii").splitlines()

    def test_cli_in_isolated_mode_invokes_gh_with_exact_source_policy(self) -> None:
        result, args = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Verified signed Flatpak attestation", result.stdout)
        self.assertEqual(args[0:3], ["attestation", "verify", str(self.bundle.resolve())])
        self.assertEqual(args[-4:], ["--source-digest", COMMIT, "--source-ref", f"refs/tags/{TAG}"])
        self.assertIn(f"{REPO}/.github/workflows/release.yml", args)

    def test_cli_exits_nonzero_when_signature_is_invalid(self) -> None:
        result, _ = self.cli(result_code=1)
        self.assertEqual(result.returncode, 1)
        self.assertIn("untrusted signer", result.stderr)
        self.assertNotIn("Verified signed", result.stdout)

    def test_cli_requires_expected_source_sha_and_tag(self) -> None:
        command = [sys.executable, "-I", str(ROOT / "build/release/verify_attestation.py"), str(self.bundle)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--commit", result.stderr)
        self.assertIn("--tag", result.stderr)

    def test_release_workflow_attests_and_verifies_before_publication(self) -> None:
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        attest = workflow.split("  attest-flatpak:\n", 1)[1].split("  verify:\n", 1)[0]
        verify = workflow.split("  verify:\n", 1)[1].split("  publish:\n", 1)[0]
        publish = workflow.split("  publish:\n", 1)[1]
        pin = re.search(r"uses: actions/attest@([0-9a-f]{40}) # v4\.2\.2", attest)
        self.assertIsNotNone(pin)
        self.assertIn("subject-path: asset-flatpak/Kinefin-x86_64.flatpak", attest)
        self.assertLess(attest.index("sha256sum -c -"), attest.index("uses: actions/attest@"))
        for permission in ("id-token: write", "attestations: write", "artifact-metadata: write"):
            self.assertIn(permission, attest)
        for job, later in ((verify, "Record build provenance"), (publish, "Publish only after")):
            self.assertIn("attestations: read", job)
            self.assertIn("GH_TOKEN: ${{ github.token }}", job)
            self.assertIn("--repository \"$RELEASE_REPOSITORY\"", job)
            self.assertIn("--commit \"$RELEASE_COMMIT\" --tag \"$RELEASE_TAG\"", job)
            self.assertLess(job.index("build/release/verify_attestation.py"), job.index(later))
            self.assertNotIn("continue-on-error: true", job)


if __name__ == "__main__":
    unittest.main()
