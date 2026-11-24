# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline regression tests for the self-reported release build record."""

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
import create_provenance as sut  # noqa: E402


class ReleaseProvenanceTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.bundle = self.root / sut.BUNDLE
        self.bundle.write_bytes(b"test flatpak\n" * 100)
        self.output = self.root / "PROVENANCE.json"
        self.sha = hashlib.sha256(self.bundle.read_bytes()).hexdigest()
        self.kwargs = {
            "tag": "v1.2.3",
            "commit": "a" * 40,
            "repository": "the-beper/Kinefin",
            "run_id": "12345",
            "image_digest": "sha256:" + "b" * 64,
            "bundle": self.bundle,
            "output": self.output,
            "expected_sha256": self.sha,
        }

    def create(self, **overrides: object) -> dict:
        return sut.create(**(self.kwargs | overrides))

    def test_generates_deterministic_and_reproducible_record(self) -> None:
        result = self.create()
        self.assertEqual(result["source"], {"tag": "v1.2.3", "commit": "a" * 40})
        self.assertEqual(result["artifact"], {
            "name": sut.BUNDLE, "sha256": self.sha, "size": self.bundle.stat().st_size,
        })
        self.assertEqual(result["build"]["image"], sut.IMAGE + "@sha256:" + "b" * 64)
        self.assertEqual(result["build"]["github_run_id"], "12345")
        self.assertEqual(result["repository"], "the-beper/Kinefin")
        self.assertEqual(result["schema_version"], 1)
        contents = self.output.read_bytes()
        self.assertTrue(contents.endswith(b"\n"))
        self.assertEqual(json.loads(contents), result)
        self.assertEqual(self.create(), result)
        self.assertEqual(contents, self.output.read_bytes())

    def test_rejects_invalid_source_metadata(self) -> None:
        invalid = (
            ("tag", "v1.2.03"), ("tag", "v1.2.3-rc1"),
            ("commit", "abc"), ("commit", "0" * 40),
            ("repository", "a/b/other"), ("repository", "../other"),
            ("run_id", "0"), ("run_id", "../123"),
        )
        for key, value in invalid:
            with self.subTest(key=key, value=value):
                with self.assertRaises(sut.ProvenanceError):
                    self.create(**{key: value})
                self.assertFalse(self.output.exists())

    def test_rejects_unpinned_images_and_incorrect_bundle_hash(self) -> None:
        for value in ("", "sha256:" + "a" * 63, "sha256:" + "0" * 64,
                      "sha256:" + "A" * 64, "fedora45"):
            with self.subTest(image_digest=value):
                with self.assertRaisesRegex(sut.ProvenanceError, "image digest"):
                    self.create(image_digest=value)
        for value in ("", "0" * 64, "f" * 64, "not-a-sha"):
            with self.subTest(expected_sha256=value):
                with self.assertRaises(sut.ProvenanceError):
                    self.create(expected_sha256=value)
        self.assertFalse(self.output.exists())

    def test_rejects_missing_empty_and_symlink_bundle(self) -> None:
        self.bundle.unlink()
        with self.assertRaisesRegex(sut.ProvenanceError, "bundle"):
            self.create()
        self.bundle.write_bytes(b"")
        with self.assertRaisesRegex(sut.ProvenanceError, "bundle"):
            self.create()
        self.bundle.unlink()
        outside = self.root / "outside.flatpak"
        outside.write_bytes(b"new bundle")
        self.bundle.symlink_to(outside)
        with self.assertRaisesRegex(sut.ProvenanceError, "bundle"):
            self.create()
        self.assertFalse(self.output.exists())

    def test_existing_record_may_only_be_reused_verbatim(self) -> None:
        self.create()
        original = self.output.read_bytes()
        with self.assertRaisesRegex(sut.ProvenanceError, "refusing to replace"):
            self.create(run_id="54321")
        self.assertEqual(self.output.read_bytes(), original)
        self.output.unlink()
        self.output.symlink_to(self.bundle)
        with self.assertRaisesRegex(sut.ProvenanceError, "regular file"):
            self.create()

    def test_cli_isolated_from_cwd_and_rejects_corrupt_sha(self) -> None:
        args = [
            sys.executable, "-I", str(ROOT / "build" / "release" / "create_provenance.py"),
            "--tag", "v1.2.3", "--commit", "a" * 40,
            "--repository", "the-beper/Kinefin", "--run-id", "12345",
            "--image-digest", "sha256:" + "b" * 64,
            "--bundle", str(self.bundle), "--expected-sha256", self.sha,
            "--output", str(self.output),
        ]
        completed = subprocess.run(args, capture_output=True, text=True, cwd=self.root,
                                   check=False, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(self.output.exists())
        bad = args.copy()
        bad[bad.index(self.sha)] = "c" * 64
        completed = subprocess.run(bad, capture_output=True, text=True, cwd=self.root,
                                   check=False, timeout=10)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("differs from the build job SHA256", completed.stderr)

    def test_workflow_passes_exact_build_image_and_hash_to_verify(self) -> None:
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        build = workflow.split("  build-flatpak:\n", 1)[1].split("  prepare-flatpak:\n", 1)[0]
        finish = workflow.split("  finish-flatpak:\n", 1)[1].split("  attest-flatpak:\n", 1)[0]
        verify = workflow.split("  verify:\n", 1)[1].split("  publish:\n", 1)[0]
        self.assertIn("image_digest: ${{ steps.pull.outputs.digest }}", build)
        self.assertIn('printf \'digest=%s\\n\' "$KINEFIN_RELEASE_FLATPAK_DIGEST" >> "$GITHUB_OUTPUT"', build)
        self.assertIn("RELEASE_IMAGE_DIGEST: ${{ needs.build-flatpak.outputs.image_digest }}", verify)
        self.assertIn("flatpak_sha256: ${{ steps.finish.outputs.flatpak_sha256 }}", finish)
        self.assertIn("EXPECT_SHA256_FLATPAK: ${{ needs.finish-flatpak.outputs.flatpak_sha256 }}", verify)
        self.assertNotIn("flatpak_sha256", build)
        self.assertIn("actions/checkout@", verify)
        self.assertIn("--commit \"$RELEASE_COMMIT\"", verify)
        self.assertIn("--run-id \"$RELEASE_RUN_ID\"", verify)
        self.assertIn("--output release-assets/PROVENANCE.json", verify)
        self.assertIn("sha256sum Kinefin-x86_64.flatpak PROVENANCE.json > SHA256SUMS", verify)
        self.assertIn("sha256sum -c SHA256SUMS", verify)


if __name__ == "__main__":
    unittest.main()
