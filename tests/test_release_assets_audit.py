# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline tests for independent Kinefin release asset verification."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build" / "release"))
import verify_release_assets as sut  # noqa: E402

TAG = "v1.2.3"
COMMIT = "a" * 40
REPO = "the-beper/Kinefin"
RUN = "123456"
DIGEST = "sha256:" + "b" * 64


class OfflineReleaseAuditTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.bundle = self.root / "Kinefin-x86_64.flatpak"
        self.bundle.write_bytes(b"pretend Flatpak data" * 100)
        self.provenance = self.root / "PROVENANCE.json"
        self.sums = self.root / "SHA256SUMS"
        self.write_metadata()

    @staticmethod
    def sha(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def write_sums(self) -> None:
        self.sums.write_text(
            f"{self.sha(self.bundle)}  {self.bundle.name}\n"
            f"{self.sha(self.provenance)}  {self.provenance.name}\n",
            encoding="ascii",
        )

    def write_metadata(self, *, artifact_sha: str | None = None) -> None:
        record = {
            "schema_version": 1,
            "repository": REPO,
            "source": {"tag": TAG, "commit": COMMIT},
            "build": {"github_run_id": RUN, "image": "ghcr.io/the-beper/kinefin-flatpak@" + DIGEST},
            "artifact": {
                "name": self.bundle.name,
                "sha256": artifact_sha or self.sha(self.bundle),
                "size": self.bundle.stat().st_size,
            },
        }
        self.provenance.write_text(json.dumps(record, sort_keys=True) + "\n", encoding="utf-8")
        self.write_sums()

    def audit(self, **kwargs: str) -> dict:
        return sut.verify(self.root, TAG, COMMIT, REPO, **kwargs)

    def command(self, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-I", str(ROOT / "build/release/verify_release_assets.py"),
             TAG, str(self.root), "--commit", COMMIT, *extra],
            capture_output=True, text=True, cwd=self.root, timeout=10,
        )

    def test_valid_release_is_verified_without_network(self) -> None:
        result = self.audit()
        self.assertEqual(result["commit"], COMMIT)
        self.assertEqual(result["sha256"], self.sha(self.bundle))
        self.assertEqual(result["run_id"], RUN)
        self.assertEqual(result["image_digest"], DIGEST)

    def test_strong_expectations_can_be_supplied_independently(self) -> None:
        self.assertEqual(
            self.audit(run_id=RUN, image_digest=DIGEST, expected_sha256=self.sha(self.bundle))["tag"], TAG,
        )
        for values in (
            {"run_id": "123457"},
            {"image_digest": "sha256:" + "c" * 64},
            {"expected_sha256": "c" * 64},
            {"expected_sha256": "XYZ"},
        ):
            with self.subTest(values=values), self.assertRaises(sut.publisher.ReleasePublishError):
                self.audit(**values)

    def test_expected_source_identity_is_never_taken_from_assets(self) -> None:
        for tag, commit, repository in (
            ("v1.2.4", COMMIT, REPO),
            (TAG, "c" * 40, REPO),
            (TAG, COMMIT, "elsewhere/Kinefin"),
        ):
            with self.subTest(tag=tag, commit=commit, repo=repository):
                with self.assertRaises(sut.publisher.ReleasePublishError):
                    sut.verify(self.root, tag, commit, repository)

    def test_invalid_identity_arguments_are_rejected(self) -> None:
        for tag, commit, repo in (
            ("v01.2.3", COMMIT, REPO),
            (TAG, "0" * 40, REPO),
            (TAG, COMMIT.upper(), REPO),
            (TAG, COMMIT, "../other"),
        ):
            with self.subTest(tag=tag, commit=commit, repo=repo):
                with self.assertRaises(sut.publisher.ReleasePublishError):
                    sut.verify(self.root, tag, commit, repo)

    def test_bundle_corruption_is_detected(self) -> None:
        self.bundle.write_bytes(b"modified bundle")
        with self.assertRaisesRegex(sut.publisher.ReleasePublishError, "SHA256SUMS"):
            self.audit()

    def test_self_consistent_modified_files_need_a_trusted_bundle_hash(self) -> None:
        original_sha = self.sha(self.bundle)
        self.bundle.write_bytes(b"repacked, untrusted Flatpak" * 200)
        self.write_metadata()  # A malicious mirror can rewrite all three files.
        self.assertEqual(self.audit()["sha256"], self.sha(self.bundle))
        with self.assertRaisesRegex(sut.publisher.ReleasePublishError, "independently supplied digest"):
            self.audit(expected_sha256=original_sha)

    def test_symlinked_bundle_and_checksums_are_rejected(self) -> None:
        for name in ("Kinefin-x86_64.flatpak", "SHA256SUMS"):
            with self.subTest(asset=name):
                path = self.root / name
                original = path.read_bytes()
                external = self.root / (name + ".external")
                external.write_bytes(original)
                path.unlink()
                path.symlink_to(external)
                with self.assertRaisesRegex(sut.publisher.ReleasePublishError, "missing or symlinked"):
                    self.audit()
                path.unlink()
                path.write_bytes(original)

    def test_corrupted_sums_is_detected(self) -> None:
        self.sums.write_text("not a checksum\n", encoding="utf-8")
        with self.assertRaisesRegex(sut.publisher.ReleasePublishError, "SHA256SUMS"):
            self.audit()

    def test_changed_metadata_with_recalculated_checksums_still_fails(self) -> None:
        original = json.loads(self.provenance.read_text(encoding="utf-8"))
        for key, value in (("schema_version", 2), ("repository", "someone/Kinefin")):
            with self.subTest(key=key):
                record = {**original, key: value}
                self.provenance.write_text(json.dumps(record) + "\n", encoding="utf-8")
                self.write_sums()
                with self.assertRaises(sut.publisher.ReleasePublishError):
                    self.audit()
        self.write_metadata()

    def test_wrong_bundle_digest_inside_metadata_fails(self) -> None:
        self.write_metadata(artifact_sha="c" * 64)
        with self.assertRaisesRegex(sut.publisher.ReleasePublishError, "provenance bundle identity"):
            self.audit()

    def test_missing_symlink_or_duplicate_key_provenance_fails(self) -> None:
        original = self.provenance.read_bytes()
        self.provenance.unlink()
        with self.assertRaisesRegex(sut.publisher.ReleasePublishError, "missing or symlinked"):
            self.audit()
        outside = self.root / "outside.json"
        outside.write_bytes(original)
        self.provenance.symlink_to(outside)
        with self.assertRaisesRegex(sut.publisher.ReleasePublishError, "missing or symlinked"):
            self.audit()
        self.provenance.unlink()
        self.provenance.write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")
        self.write_sums()
        with self.assertRaisesRegex(sut.publisher.ReleasePublishError, "duplicate JSON key"):
            self.audit()

    def test_invalid_optional_claim_fields_fail(self) -> None:
        original = json.loads(self.provenance.read_text(encoding="utf-8"))
        for data in (None, True, "", "ghcr.io/the-beper/kinefin-flatpak:fedora45"):
            with self.subTest(value=data):
                original["build"]["image"] = data
                self.provenance.write_text(json.dumps(original) + "\n", encoding="utf-8")
                self.write_sums()
                with self.assertRaises(sut.publisher.ReleasePublishError):
                    self.audit()
        original["build"]["image"] = "ghcr.io/the-beper/kinefin-flatpak@" + DIGEST
        original["build"]["github_run_id"] = 0
        self.provenance.write_text(json.dumps(original) + "\n", encoding="utf-8")
        self.write_sums()
        with self.assertRaises(sut.publisher.ReleasePublishError):
            self.audit()

    def test_cli_in_isolated_mode_from_any_working_directory(self) -> None:
        result = self.command("--expected-sha256", self.sha(self.bundle), "--run-id", RUN,
                              "--image-digest", DIGEST)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Verified three local release assets", result.stdout)
        self.assertIn("unsigned", result.stdout)
        self.assertNotIn("Traceback", result.stderr)

    def test_cli_requires_trusted_commit_and_fails_closed(self) -> None:
        result = subprocess.run(
            [sys.executable, "-I", str(ROOT / "build/release/verify_release_assets.py"), TAG, str(self.root)],
            capture_output=True, text=True, cwd=self.root, timeout=10,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--commit", result.stderr)
        corrupted = self.command("--expected-sha256", "c" * 64)
        self.assertEqual(corrupted.returncode, 1)
        self.assertIn("independently supplied digest", corrupted.stderr)

    def test_workflow_audits_before_upload(self) -> None:
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        verification = workflow.split("  verify:\n", 1)[1].split("  publish:\n", 1)[0]
        self.assertIn("python3 -I build/release/verify_release_assets.py", verification)
        self.assertLess(verification.index("verify_release_assets.py"), verification.index("name: release-assets"))


if __name__ == "__main__":
    unittest.main()
