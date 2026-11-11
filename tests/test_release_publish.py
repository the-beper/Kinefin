# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline tests for the release publisher; no network or GitHub account needed."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build" / "release"))
import publish_release as sut  # noqa: E402


class FakeGitHub:
    def __init__(self) -> None:
        self.release: dict | None = None
        self.calls: list[tuple[str, ...]] = []
        self.fail_upload_once = False
        self.fail_publish = False
        self.omit_digest = False
        self.fail_create = False
        self.fail_api = False

    @staticmethod
    def asset(path: Path) -> dict:
        data = path.read_bytes()
        return {
            "name": path.name,
            "digest": f"sha256:{hashlib.sha256(data).hexdigest()}",
            "size": len(data),
            "state": "uploaded",
        }

    def __call__(self, *args: str) -> str:
        self.calls.append(args)
        if args[:2] == ("release", "create"):
            if self.fail_create:
                raise sut.ReleasePublishError("not authorized to create a release")
            if self.release is not None:
                raise sut.ReleasePublishError("release already exists")
            self.release = {"tag_name": args[2], "draft": True, "assets": []}
            return ""
        if args[0] == "api":
            if self.fail_api or self.release is None:
                raise sut.ReleasePublishError("release not found")
            state = json.loads(json.dumps(self.release))
            if self.omit_digest:
                for asset in state["assets"]:
                    asset.pop("digest", None)
            return json.dumps(state)
        if args[:2] == ("release", "upload"):
            if self.fail_upload_once:
                self.fail_upload_once = False
                raise sut.ReleasePublishError("upload interrupted")
            assert self.release is not None and self.release["draft"]
            cutoff = args.index("--clobber")
            for filename in args[3:cutoff]:
                asset = self.asset(Path(filename))
                self.release["assets"] = [
                    existing for existing in self.release["assets"]
                    if existing["name"] != asset["name"]
                ]
                self.release["assets"].append(asset)
            return ""
        if args[:2] == ("release", "edit"):
            if self.fail_publish:
                raise sut.ReleasePublishError("publish failed")
            assert self.release is not None and self.release["draft"]
            self.release["draft"] = False
            return ""
        raise AssertionError(f"unexpected command: {args}")


class ReleasePublishTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.bundle = self.root / sut.BUNDLE
        self.sums = self.root / sut.SUMS
        self.bundle.write_bytes(b"fake Flatpak bundle\n" * 200)
        self.write_sums()
        self.gh = FakeGitHub()

    def write_sums(self) -> None:
        digest = hashlib.sha256(self.bundle.read_bytes()).hexdigest()
        self.sums.write_text(f"{digest}  {sut.BUNDLE}\n", encoding="ascii")

    def publish(self, tag: str = "v1.2.3", repo: str = "the-beper/Kinefin") -> str:
        return sut.publish(tag, self.root, repo, self.gh)

    def asset_calls(self, action: str) -> list[tuple[str, ...]]:
        return [call for call in self.gh.calls if call[:2] == ("release", action)]

    def test_new_release_stays_draft_until_both_asset_digests_match(self) -> None:
        result = self.publish()
        self.assertIn("Published release v1.2.3", result)
        self.assertFalse(self.gh.release["draft"])
        self.assertEqual({a["name"] for a in self.gh.release["assets"]}, set(sut.ASSET_NAMES))
        create, = self.asset_calls("create")
        self.assertIn("--draft", create)
        self.assertIn("--verify-tag", create)
        self.assertIn("--repo", create)
        self.assertEqual(create[-1], "the-beper/Kinefin")
        upload, = self.asset_calls("upload")
        self.assertEqual(set(upload[3:upload.index("--clobber")]), {str(self.bundle), str(self.sums)})
        self.assertEqual(len(self.asset_calls("edit")), 1)
        self.assertLess(self.gh.calls.index(upload), self.gh.calls.index(self.asset_calls("edit")[0]))
        self.assertEqual(sum(1 for call in self.gh.calls if call[0] == "api"), 3)

    def test_retry_after_interrupted_upload(self) -> None:
        self.gh.fail_upload_once = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "upload interrupted"):
            self.publish()
        self.assertTrue(self.gh.release["draft"])
        result = self.publish()
        self.assertIn("Published release", result)
        self.assertFalse(self.gh.release["draft"])

    def test_retry_uploads_only_the_missing_asset(self) -> None:
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": True,
            "assets": [self.gh.asset(self.sums)],
        }
        self.publish()
        upload, = self.asset_calls("upload")
        self.assertEqual(upload[3:upload.index("--clobber")], (str(self.bundle),))
        self.assertEqual(len(self.asset_calls("edit")), 1)

    def test_matching_complete_draft_publishes_without_reupload(self) -> None:
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": True,
            "assets": [self.gh.asset(self.sums), self.gh.asset(self.bundle)],
        }
        self.publish()
        self.assertFalse(self.asset_calls("upload"))
        self.assertEqual(len(self.asset_calls("edit")), 1)

    def test_retry_replaces_only_mismatched_draft_asset(self) -> None:
        bad_bundle = self.gh.asset(self.bundle)
        bad_bundle["digest"] = "sha256:" + "0" * 64
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": True,
            "assets": [self.gh.asset(self.sums), bad_bundle],
        }
        self.publish()
        upload, = self.asset_calls("upload")
        self.assertEqual(upload[3:upload.index("--clobber")], (str(self.bundle),))
        self.assertFalse(self.gh.release["draft"])

    def test_matching_published_release_is_a_noop(self) -> None:
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": False,
            "assets": [self.gh.asset(self.bundle), self.gh.asset(self.sums)],
        }
        message = self.publish()
        self.assertIn("already published", message)
        self.assertFalse(self.asset_calls("upload"))
        self.assertFalse(self.asset_calls("edit"))

    def test_mismatched_published_release_never_changes(self) -> None:
        bad_bundle = self.gh.asset(self.bundle)
        bad_bundle["digest"] = "sha256:" + "f" * 64
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": False,
            "assets": [bad_bundle, self.gh.asset(self.sums)],
        }
        with self.assertRaisesRegex(sut.ReleasePublishError, "refusing to change published assets"):
            self.publish()
        self.assertFalse(self.asset_calls("upload"))
        self.assertFalse(self.asset_calls("edit"))

    def test_unknown_or_duplicate_assets_need_manual_review(self) -> None:
        for extra in ({"name": "unexpected.exe"}, self.gh.asset(self.bundle)):
            with self.subTest(extra=extra):
                self.gh = FakeGitHub()
                self.gh.release = {
                    "tag_name": "v1.2.3", "draft": True,
                    "assets": [self.gh.asset(self.bundle), self.gh.asset(self.sums), extra],
                }
                with self.assertRaises(sut.ReleasePublishError):
                    self.publish()
                self.assertFalse(self.asset_calls("upload"))
                self.assertFalse(self.asset_calls("edit"))

    def test_missing_remote_digest_keeps_release_as_draft(self) -> None:
        self.gh.omit_digest = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "unverified assets"):
            self.publish()
        self.assertTrue(self.gh.release["draft"])
        self.assertFalse(self.asset_calls("edit"))

    def test_publish_error_leaves_verified_draft_for_next_attempt(self) -> None:
        self.gh.fail_publish = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "publish failed"):
            self.publish()
        self.assertTrue(self.gh.release["draft"])
        self.gh.fail_publish = False
        self.publish()
        self.assertFalse(self.gh.release["draft"])
        self.assertEqual(len(self.asset_calls("upload")), 1)

    def test_tag_repo_validation_precedes_any_github_call(self) -> None:
        for tag in ("1.2.3", "v1.02.3", "v1.2.3-beta", "v1.2.3\n", "v1.2.3/other"):
            with self.subTest(tag=tag):
                with self.assertRaisesRegex(sut.ReleasePublishError, "release tag"):
                    self.publish(tag)
        for repo in ("", "owner", "../bad", "a/b/third", "owner/..", "owner/bad name"):
            with self.subTest(repo=repo):
                with self.assertRaisesRegex(sut.ReleasePublishError, "GITHUB_REPOSITORY"):
                    self.publish(repo=repo)
        self.assertEqual(self.gh.calls, [])

    def test_missing_and_empty_assets_abort_before_network(self) -> None:
        self.bundle.unlink()
        with self.assertRaisesRegex(sut.ReleasePublishError, "missing or symlinked"):
            self.publish()
        self.bundle.write_bytes(b"")
        with self.assertRaisesRegex(sut.ReleasePublishError, "empty release asset"):
            self.publish()
        self.assertEqual(self.gh.calls, [])

    def test_symlink_asset_is_rejected(self) -> None:
        outside = self.root / "outside"
        outside.write_bytes(self.bundle.read_bytes())
        self.bundle.unlink()
        self.bundle.symlink_to(outside)
        with self.assertRaisesRegex(sut.ReleasePublishError, "symlinked"):
            self.publish()
        self.assertEqual(self.gh.calls, [])

    def test_invalid_checksum_file_aborts_before_network(self) -> None:
        original = self.sums.read_text(encoding="ascii")
        for corrupted in (
            "0" * 64 + f"  {sut.BUNDLE}\n",
            original + original,
            original.replace("  ", " "),
            original.replace(sut.BUNDLE, "other.flatpak"),
            original.rstrip("\n"),
            "not a checksum\n",
        ):
            with self.subTest(corrupted=corrupted[:60]):
                self.sums.write_text(corrupted, encoding="ascii")
                with self.assertRaises(sut.ReleasePublishError):
                    self.publish()
                self.assertEqual(self.gh.calls, [])

    def test_cannot_read_existing_release_after_create_failure(self) -> None:
        self.gh.fail_create = True
        self.gh.fail_api = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "could not create draft"):
            self.publish()
        self.assertFalse(self.asset_calls("upload"))
        self.assertFalse(self.asset_calls("edit"))

    def test_workflow_does_not_publish_before_remote_sha_check(self) -> None:
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn('python3 -I build/release/publish_release.py "$RELEASE_TAG" release-assets', workflow)
        publish_job = workflow.split("  publish:\n", 1)[1]
        self.assertIn("actions/checkout@", publish_job)
        self.assertIn("persist-credentials: false", publish_job)
        self.assertNotIn("gh release create", publish_job)
        self.assertNotIn("gh release edit", publish_job)


if __name__ == "__main__":
    unittest.main()
