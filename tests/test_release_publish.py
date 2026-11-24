# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline tests for the release publisher; no network or GitHub account needed."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build" / "release"))
import publish_release as sut  # noqa: E402
import publish_site  # noqa: E402


EXPECTED_COMMIT = "a" * 40
MOVED_COMMIT = "b" * 40
ANNOTATED_SHA = "c" * 40


class FakeGitHub:
    def __init__(self) -> None:
        self.release: dict | None = None
        self.calls: list[tuple[str, ...]] = []
        self.fail_upload_once = False
        self.fail_publish = False
        self.omit_digest = False
        self.fail_create = False
        self.fail_api = False
        self.fail_tag_api = False
        self.change_tag_after_upload = False
        self.ref_object: dict = {"type": "commit", "sha": EXPECTED_COMMIT}
        self.ref_name = "refs/tags/v1.2.3"
        self.tag_objects: dict[str, dict] = {}
        self.ref_override: object | None = None
        self.by_tag_calls = 0
        # Releases of other tags in the list, and how many list requests do not show the new draft yet
        self.others: list[dict] = []
        self.list_lag = 0
        self.list_calls = 0
        self.latest_tag: str | None = None
        self.stuck_latest: str | None = None
        self.fail_latest = False
        self.latest_hidden = False

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
                # GitHub may accept a second draft with the same tag
                self.others.append({"tag_name": args[2], "draft": True, "prerelease": False, "assets": []})
                return ""
            self.release = {"tag_name": args[2], "draft": True, "prerelease": "--prerelease" in args, "assets": []}
            return ""
        if args[0] == "api":
            endpoint = args[-1]
            if "/git/ref/tags/" in endpoint:
                if self.fail_tag_api:
                    raise sut.ReleasePublishError("tag ref missing")
                if self.ref_override is not None:
                    return json.dumps(self.ref_override)
                return json.dumps({"ref": self.ref_name, "object": self.ref_object})
            if "/git/tags/" in endpoint:
                tag_sha = endpoint.rsplit("/", 1)[-1]
                if tag_sha not in self.tag_objects:
                    raise sut.ReleasePublishError("annotated tag not found")
                return json.dumps(self.tag_objects[tag_sha])
            if endpoint.endswith("/releases/latest"):
                if self.fail_latest:
                    raise sut.ReleasePublishError("gh api failed: server error (HTTP 500)")
                latest = self.stuck_latest or self.latest_tag
                if latest is None or self.latest_hidden:
                    raise sut.GitHubNotFound("gh api failed: gh: Not Found (HTTP 404)")
                return json.dumps({"tag_name": latest})
            if "/releases/tags/" in endpoint:
                # Real GitHub does not show a draft here; the publisher must not depend on this endpoint
                self.by_tag_calls += 1
                if self.release is None or self.release["draft"]:
                    raise sut.GitHubNotFound("gh api failed: gh: Not Found (HTTP 404)")
                return json.dumps(self.release)
            if "/releases?" in endpoint:
                assert args[1] == "--paginate"
                if self.fail_api:
                    raise sut.ReleasePublishError("release list not available")
                visible = self.release is not None and self.list_calls >= self.list_lag
                self.list_calls += 1
                releases = [*self.others]
                if visible:
                    state = json.loads(json.dumps(self.release))
                    state.setdefault("prerelease", False)
                    if self.omit_digest:
                        for asset in state["assets"]:
                            asset.pop("digest", None)
                    releases.append(state)
                # gh prints one array per page, back to back
                return "\n".join(json.dumps([release]) for release in releases) or "[]"
            raise AssertionError(f"unexpected endpoint: {endpoint}")
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
            if self.change_tag_after_upload:
                self.ref_object = {"type": "commit", "sha": MOVED_COMMIT}
            return ""
        if args[:2] == ("release", "edit"):
            if self.fail_publish:
                raise sut.ReleasePublishError("publish failed")
            assert self.release is not None and "--draft=false" in args
            flags = dict(arg.split("=", 1) for arg in args if arg.startswith("--") and "=" in arg)
            self.release["draft"] = False
            self.release["prerelease"] = flags["--prerelease"] == "true"
            if flags["--latest"] == "true":
                self.latest_tag = args[2]
            elif self.latest_tag == args[2]:
                self.latest_tag = None
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
        self.provenance = self.root / sut.PROVENANCE
        self.tag = "v1.2.3"
        self.write_provenance()
        self.write_sums()
        self.gh = FakeGitHub()
        self.sleeps: list[float] = []
        self.site_error: Exception | None = None
        self.site_pushed = False
        self.during_site = None
        self.wait_error: Exception | None = None
        self.site_tar = self.root / "site.tar"
        self.site_tar.write_bytes(b"site archive")

    def site(self) -> None:
        # The site step is a fake here: the order of the calls is what these tests look at
        self.gh.calls.append(("site",))
        if self.during_site is not None:
            self.during_site()
        if self.site_error is not None:
            raise self.site_error
        self.site_pushed = True

    def use_tag(self, tag: str) -> None:
        self.tag = tag
        self.gh.ref_name = f"refs/tags/{tag}"
        self.write_provenance()
        self.write_sums()

    def write_provenance(self) -> None:
        self.provenance.write_text(json.dumps({
            "schema_version": 1,
            "repository": "the-beper/Kinefin",
            "source": {"tag": self.tag, "commit": EXPECTED_COMMIT},
            "build": {
                "github_run_id": "12345",
                "image": "ghcr.io/the-beper/kinefin-flatpak@sha256:" + "b" * 64,
            },
            "artifact": {
                "name": sut.BUNDLE,
                "sha256": hashlib.sha256(self.bundle.read_bytes()).hexdigest(),
                "size": self.bundle.stat().st_size,
            },
        }, sort_keys=True) + "\n", encoding="utf-8")

    def write_sums(self) -> None:
        bundle_sha = hashlib.sha256(self.bundle.read_bytes()).hexdigest()
        provenance_sha = hashlib.sha256(self.provenance.read_bytes()).hexdigest()
        self.sums.write_text(
            f"{bundle_sha}  {sut.BUNDLE}\n{provenance_sha}  {sut.PROVENANCE}\n",
            encoding="ascii",
        )

    def publish(self, tag: str | None = None, repo: str = "the-beper/Kinefin") -> str:
        return sut.publish(
            tag or self.tag, self.root, repo, EXPECTED_COMMIT, "12345", "sha256:" + "b" * 64, self.gh,
            site=self.site, sleep=self.sleeps.append,
        )

    def asset_calls(self, action: str) -> list[tuple[str, ...]]:
        return [call for call in self.gh.calls if call[:2] == ("release", action)]

    def test_lightweight_tag_must_match_workflow_commit(self) -> None:
        self.gh.ref_object = {"type": "commit", "sha": MOVED_COMMIT}
        with self.assertRaisesRegex(sut.ReleasePublishError, "not the workflow commit"):
            self.publish()
        self.assertIsNone(self.gh.release)
        self.assertFalse(self.asset_calls("create"))
        self.assertFalse(self.asset_calls("upload"))

    def test_annotated_tag_is_dereferenced_to_commit(self) -> None:
        self.gh.ref_object = {"type": "tag", "sha": ANNOTATED_SHA}
        self.gh.tag_objects[ANNOTATED_SHA] = {
            "sha": ANNOTATED_SHA,
            "object": {"type": "commit", "sha": EXPECTED_COMMIT},
        }
        self.assertIn("Published release", self.publish())
        self.assertEqual(len([call for call in self.gh.calls if "/git/tags/" in call[-1]]), 3)

    def test_nested_annotated_tags_are_dereferenced(self) -> None:
        nested = "d" * 40
        self.gh.ref_object = {"type": "tag", "sha": ANNOTATED_SHA}
        self.gh.tag_objects[ANNOTATED_SHA] = {
            "sha": ANNOTATED_SHA, "object": {"type": "tag", "sha": nested},
        }
        self.gh.tag_objects[nested] = {
            "sha": nested, "object": {"type": "commit", "sha": EXPECTED_COMMIT},
        }
        self.assertIn("Published release", self.publish())

    def test_deleted_remote_tag_ref_fails_closed(self) -> None:
        self.gh.fail_tag_api = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "tag ref missing"):
            self.publish()
        self.assertIsNone(self.gh.release)

    def test_changed_tag_during_upload_leaves_draft_unpublished(self) -> None:
        self.gh.change_tag_after_upload = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "not the workflow commit"):
            self.publish()
        self.assertIsNotNone(self.gh.release)
        self.assertTrue(self.gh.release["draft"])
        self.assertFalse(self.asset_calls("edit"))
        self.assertFalse(self.site_pushed)

    def test_matching_existing_draft_rechecks_tag_before_site_and_publication(self) -> None:
        # The tag is looked up before the draft, before the site and before the publication edit
        for moved_at in (2, 3):
            with self.subTest(moved_at=moved_at):
                self.gh = FakeGitHub()
                self.site_pushed = False
                self.gh.release = {
                    "tag_name": "v1.2.3", "draft": True,
                    "assets": [self.gh.asset(self.bundle), self.gh.asset(self.provenance), self.gh.asset(self.sums)],
                }
                lookups = 0

                def moving_runner(*args: str) -> str:
                    nonlocal lookups
                    if args[0] == "api" and "/git/ref/tags/" in args[-1]:
                        lookups += 1
                        if lookups == moved_at:
                            self.gh.ref_object = {"type": "commit", "sha": MOVED_COMMIT}
                    return self.gh(*args)

                with self.assertRaisesRegex(sut.ReleasePublishError, "not the workflow commit"):
                    sut.publish(
                        "v1.2.3", self.root, "the-beper/Kinefin", EXPECTED_COMMIT, "12345", "sha256:" + "b" * 64,
                        moving_runner, site=self.site, sleep=self.sleeps.append,
                    )
                self.assertTrue(self.gh.release["draft"])
                self.assertFalse(self.asset_calls("edit"))
                self.assertEqual(self.site_pushed, moved_at == 3)

    def test_malformed_tag_json_and_cycles_are_rejected(self) -> None:
        for invalid in (
            {"ref": "refs/tags/else", "object": {"type": "commit", "sha": EXPECTED_COMMIT}},
            {"ref": "refs/tags/v1.2.3", "object": {"type": "tree", "sha": EXPECTED_COMMIT}},
            {"ref": "refs/tags/v1.2.3", "object": {"type": "commit", "sha": "no"}},
            [1, 2],
        ):
            with self.subTest(invalid=invalid):
                self.gh = FakeGitHub()
                self.gh.ref_override = invalid
                with self.assertRaises(sut.ReleasePublishError):
                    self.publish()
                self.assertFalse(self.asset_calls("create"))
        self.gh = FakeGitHub()
        self.gh.ref_object = {"type": "tag", "sha": ANNOTATED_SHA}
        self.gh.tag_objects[ANNOTATED_SHA] = {
            "sha": ANNOTATED_SHA, "object": {"type": "tag", "sha": ANNOTATED_SHA},
        }
        with self.assertRaisesRegex(sut.ReleasePublishError, "cycle"):
            self.publish()
        self.assertFalse(self.asset_calls("create"))

    def test_commit_validation_precedes_network(self) -> None:
        for invalid in ("", "HEAD", "0" * 40, "A" * 40, "f" * 39, "a" * 41):
            with self.subTest(invalid=invalid):
                with self.assertRaisesRegex(sut.ReleasePublishError, "RELEASE_COMMIT"):
                    sut.publish(
                        "v1.2.3", self.root, "the-beper/Kinefin", invalid, "12345", "sha256:" + "b" * 64, self.gh,
                        site=self.site, sleep=self.sleeps.append,
                    )
        self.assertEqual(self.gh.calls, [])

    def test_new_release_stays_draft_until_all_asset_digests_match(self) -> None:
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
        self.assertEqual(
            set(upload[3:upload.index("--clobber")]),
            {str(self.bundle), str(self.provenance), str(self.sums)},
        )
        self.assertEqual(len(self.asset_calls("edit")), 1)
        self.assertLess(self.gh.calls.index(upload), self.gh.calls.index(("site",)))
        self.assertLess(self.gh.calls.index(("site",)), self.gh.calls.index(self.asset_calls("edit")[0]))
        # Three tag lookups, five release list lookups (find, wait, verify, before the edit, confirm) and the latest release
        self.assertEqual(sum(1 for call in self.gh.calls if call[0] == "api"), 9)
        self.assertEqual(len(self.asset_calls("create")), 1)

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
            "assets": [self.gh.asset(self.provenance), self.gh.asset(self.sums)],
        }
        self.publish()
        upload, = self.asset_calls("upload")
        self.assertEqual(upload[3:upload.index("--clobber")], (str(self.bundle),))
        self.assertEqual(len(self.asset_calls("edit")), 1)

    def test_retry_uploads_only_missing_provenance(self) -> None:
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": True,
            "assets": [self.gh.asset(self.bundle), self.gh.asset(self.sums)],
        }
        self.publish()
        upload, = self.asset_calls("upload")
        self.assertEqual(upload[3:upload.index("--clobber")], (str(self.provenance),))
        self.assertEqual(len(self.asset_calls("edit")), 1)

    def test_matching_complete_draft_publishes_without_reupload(self) -> None:
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": True,
            "assets": [self.gh.asset(self.sums), self.gh.asset(self.bundle), self.gh.asset(self.provenance)],
        }
        self.publish()
        self.assertFalse(self.asset_calls("upload"))
        self.assertEqual(len(self.asset_calls("edit")), 1)

    def test_retry_replaces_only_mismatched_draft_asset(self) -> None:
        bad_bundle = self.gh.asset(self.bundle)
        bad_bundle["digest"] = "sha256:" + "0" * 64
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": True,
            "assets": [self.gh.asset(self.sums), self.gh.asset(self.provenance), bad_bundle],
        }
        self.publish()
        upload, = self.asset_calls("upload")
        self.assertEqual(upload[3:upload.index("--clobber")], (str(self.bundle),))
        self.assertFalse(self.gh.release["draft"])

    def test_matching_published_release_keeps_its_assets(self) -> None:
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": False,
            "assets": [self.gh.asset(self.bundle), self.gh.asset(self.provenance), self.gh.asset(self.sums)],
        }
        message = self.publish()
        self.assertIn("already published", message)
        self.assertFalse(self.asset_calls("upload"))
        self.assertEqual(len(self.asset_calls("edit")), 1)
        self.assertTrue(self.site_pushed)

    def test_mismatched_published_release_never_changes(self) -> None:
        bad_bundle = self.gh.asset(self.bundle)
        bad_bundle["digest"] = "sha256:" + "f" * 64
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": False,
            "assets": [bad_bundle, self.gh.asset(self.provenance), self.gh.asset(self.sums)],
        }
        with self.assertRaisesRegex(sut.ReleasePublishError, "refusing to change published assets"):
            self.publish()
        self.assertFalse(self.asset_calls("upload"))
        self.assertFalse(self.asset_calls("edit"))
        self.assertFalse(self.site_pushed)

    def test_unknown_or_duplicate_assets_need_manual_review(self) -> None:
        for extra in ({"name": "unexpected.exe"}, self.gh.asset(self.bundle)):
            with self.subTest(extra=extra):
                self.gh = FakeGitHub()
                self.gh.release = {
                    "tag_name": "v1.2.3", "draft": True,
                    "assets": [self.gh.asset(self.bundle), self.gh.asset(self.provenance), self.gh.asset(self.sums), extra],
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

    def test_cannot_create_draft(self) -> None:
        self.gh.fail_create = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "could not create draft"):
            self.publish()
        self.assertFalse(self.asset_calls("upload"))
        self.assertFalse(self.asset_calls("edit"))
        self.assertFalse(self.site_pushed)

    def test_unreadable_release_list_creates_nothing(self) -> None:
        self.gh.fail_api = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "release list not available"):
            self.publish()
        self.assertFalse(self.asset_calls("create"))
        self.assertFalse(self.asset_calls("edit"))

    def test_rerun_resumes_the_draft_without_creating_another(self) -> None:
        self.gh.fail_upload_once = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "upload interrupted"):
            self.publish()
        self.assertEqual(len(self.asset_calls("create")), 1)
        self.publish()
        self.assertEqual(len(self.asset_calls("create")), 1)
        self.assertEqual(self.gh.others, [])
        self.assertFalse(self.gh.release["draft"])
        # Interrupted during the Pages wait: the same
        self.gh = FakeGitHub()
        self.site_error = publish_site.SitePublishError("Pages is not up")
        with self.assertRaises(publish_site.SitePublishError):
            self.publish()
        self.site_error = None
        self.publish()
        self.assertEqual(len(self.asset_calls("create")), 1)
        self.assertEqual(self.gh.others, [])

    def test_asset_or_state_changed_during_the_pages_wait_is_not_published(self) -> None:
        def clobber() -> None:
            self.gh.release["assets"][0]["digest"] = "sha256:" + "e" * 64

        def publish_meanwhile() -> None:
            self.gh.release["draft"] = False

        for label, hook, published in (
            ("draft, asset clobbered", clobber, False),
            ("draft, published meanwhile", publish_meanwhile, False),
            ("published, asset clobbered", clobber, True),
        ):
            with self.subTest(label):
                self.gh = FakeGitHub()
                self.gh.release = {
                    "tag_name": "v1.2.3", "draft": not published,
                    "assets": [self.gh.asset(self.bundle), self.gh.asset(self.provenance), self.gh.asset(self.sums)],
                }
                self.during_site = hook
                with self.assertRaisesRegex(sut.ReleasePublishError, "changed while the repository was being published"):
                    self.publish()
                self.assertFalse(self.asset_calls("edit"))

    def test_tag_moved_during_the_uploads_pushes_no_site(self) -> None:
        self.gh.change_tag_after_upload = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "not the workflow commit"):
            self.publish()
        self.assertFalse(self.site_pushed)
        self.assertEqual(self.gh.calls.count(("site",)), 0)

    def test_build_identity_validation_precedes_network(self) -> None:
        for run_id, digest, message in (
            ("0", "sha256:" + "b" * 64, "GITHUB_RUN_ID"),
            ("12346", "sha256:" + "b" * 64, "differs from the build job"),
            ("12345", "sha256:" + "c" * 64, "differs from the build job"),
            ("12345", "sha256:" + "0" * 64, "RELEASE_IMAGE_DIGEST"),
        ):
            with self.subTest(run_id=run_id, digest=digest[:15]):
                with self.assertRaisesRegex(sut.ReleasePublishError, message):
                    sut.publish("v1.2.3", self.root, "the-beper/Kinefin", EXPECTED_COMMIT,
                                run_id, digest, self.gh, site=self.site, sleep=self.sleeps.append)
                self.assertEqual(self.gh.calls, [])

    def test_missing_provenance_aborts_before_network(self) -> None:
        self.provenance.unlink()
        with self.assertRaisesRegex(sut.ReleasePublishError, "missing or symlinked"):
            self.publish()
        self.assertEqual(self.gh.calls, [])

    def test_symlinked_provenance_aborts_before_network(self) -> None:
        original = self.provenance.read_bytes()
        outside = self.root / "external-metadata.json"
        outside.write_bytes(original)
        self.provenance.unlink()
        self.provenance.symlink_to(outside)
        with self.assertRaisesRegex(sut.ReleasePublishError, "symlinked"):
            self.publish()
        self.assertEqual(self.gh.calls, [])

    def test_provenance_drift_rejected_even_with_matching_checksums(self) -> None:
        cases = (
            ("repository", None, "someone-else/Kinefin", "provenance repository"),
            ("schema_version", None, 2, "schema version"),
            ("source", "commit", MOVED_COMMIT, "tag or commit"),
            ("source", "tag", "v1.2.4", "tag or commit"),
            ("build", "image", "ghcr.io/the-beper/kinefin-flatpak:fedora45", "differs from the build job"),
            ("build", "github_run_id", "", "run ID"),
            ("artifact", "sha256", "f" * 64, "bundle identity"),
            ("artifact", "size", 1, "bundle identity"),
            ("artifact", "size", True, "bundle identity"),
            ("artifact", "name", "other.flatpak", "bundle identity"),
        )
        original = self.provenance.read_text(encoding="utf-8")
        for section, field, altered, error in cases:
            with self.subTest(section=section, field=field, altered=altered):
                data = json.loads(original)
                if field is None:
                    data[section] = altered
                else:
                    data[section][field] = altered
                self.provenance.write_text(json.dumps(data) + "\n", encoding="utf-8")
                self.write_sums()
                with self.assertRaisesRegex(sut.ReleasePublishError, error):
                    self.publish()
                self.assertEqual(self.gh.calls, [])
        self.provenance.write_text(original, encoding="utf-8")
        self.write_sums()

    def test_invalid_provenance_json_aborts_before_network(self) -> None:
        original = self.provenance.read_text(encoding="utf-8")
        for corrupted in ("{broken", "{}", '{"schema_version": 1, "schema_version": 1}',
                          original.replace('"schema_version": 1', '"schema_version": true'),
                          original + " " * 20000):
            with self.subTest(corrupted=corrupted[:40]):
                self.provenance.write_text(corrupted, encoding="utf-8")
                self.write_sums()
                with self.assertRaises(sut.ReleasePublishError):
                    self.publish()
                self.assertEqual(self.gh.calls, [])

    def test_provenance_missing_from_published_release_is_not_modified(self) -> None:
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": False,
            "assets": [self.gh.asset(self.bundle), self.gh.asset(self.sums)],
        }
        with self.assertRaisesRegex(sut.ReleasePublishError, "refusing to change published assets"):
            self.publish()
        self.assertFalse(self.asset_calls("upload"))
        self.assertFalse(self.asset_calls("edit"))

    def test_unverified_provenance_asset_keeps_draft(self) -> None:
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": True,
            "assets": [self.gh.asset(self.bundle), self.gh.asset(self.sums)],
        }
        self.gh.omit_digest = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "unverified assets"):
            self.publish()
        self.assertTrue(self.gh.release["draft"])
        self.assertFalse(self.asset_calls("edit"))

    def test_draft_found_through_list_endpoint(self) -> None:
        self.gh.others = [
            {"tag_name": "v1.2.2", "draft": False, "prerelease": False, "assets": []},
            {"tag_name": "v1.2.4", "draft": True, "prerelease": False, "assets": []},
        ]
        self.publish()
        self.assertEqual(self.gh.by_tag_calls, 0)
        self.assertFalse(any("/releases/tags/" in call[-1] for call in self.gh.calls if call[0] == "api"))
        self.assertEqual(self.sleeps, [])
        # An existing draft is found the same way, though the by-tag endpoint answers 404 for it
        self.gh = FakeGitHub()
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": True,
            "assets": [self.gh.asset(self.bundle), self.gh.asset(self.provenance), self.gh.asset(self.sums)],
        }
        with self.assertRaises(sut.GitHubNotFound):
            self.gh("api", "repos/the-beper/Kinefin/releases/tags/v1.2.3")
        self.publish()
        self.assertFalse(self.gh.release["draft"])

    def test_tag_older_than_a_published_release_is_refused_before_any_write(self) -> None:
        for newer in ("v1.2.4", "v1.3.0", "v2.0.0", "v1.10.0"):
            self.gh = FakeGitHub()
            self.gh.others = [{"tag_name": newer, "draft": False, "prerelease": False, "assets": []}]
            with self.assertRaisesRegex(sut.ReleasePublishError, f"older than the published release {newer}.*next version"):
                self.publish()
            self.assertIsNone(self.gh.release)
            self.assertFalse(self.asset_calls("create"))
            self.assertFalse(self.asset_calls("upload"))
            self.assertFalse(self.asset_calls("edit"))
            self.assertNotIn(("site",), self.gh.calls)
            self.assertFalse(self.site_pushed)

    def test_refusal_to_roll_back_exits_1_from_main(self) -> None:
        self.gh.others = [{"tag_name": "v1.2.4", "draft": False, "prerelease": False, "assets": []}]
        code, message = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("older than the published release v1.2.4", message)
        self.assertFalse(self.asset_calls("create"))

    def test_other_releases_do_not_stop_a_publication(self) -> None:
        self.gh.others = [
            {"tag_name": "v1.2.4", "draft": True, "prerelease": False, "assets": []},
            {"tag_name": "v1.2.2", "draft": False, "prerelease": False, "assets": []},
            {"tag_name": "v1.2.3-rc1", "draft": False, "prerelease": True, "assets": []},
            {"tag_name": "nightly", "draft": False, "prerelease": True, "assets": []},
            {"tag_name": "v9.9", "draft": False, "prerelease": False, "assets": []},
            {"tag_name": "v9.9.9.9", "draft": False, "prerelease": False, "assets": []},
            {"tag_name": "9.9.9", "draft": False, "prerelease": False, "assets": []},
        ]
        self.publish()
        self.assertFalse(self.gh.release["draft"])
        self.assertTrue(self.site_pushed)

    def test_republishing_the_newest_tag_is_not_a_rollback(self) -> None:
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": False,
            "assets": [self.gh.asset(self.bundle), self.gh.asset(self.provenance), self.gh.asset(self.sums)],
        }
        self.gh.others = [{"tag_name": "v1.2.2", "draft": False, "prerelease": False, "assets": []}]
        self.assertIn("already published", self.publish())

    def test_release_lists_with_several_pages_and_duplicates(self) -> None:
        self.gh.others = [{"tag_name": f"v0.0.{n}", "draft": False, "prerelease": True, "assets": []} for n in range(5)]
        self.publish()
        self.assertFalse(self.gh.release["draft"])
        self.site_pushed = False
        self.gh = FakeGitHub()
        self.gh.others = [{"tag_name": "v1.2.3", "draft": True, "prerelease": False, "assets": []}]
        self.gh.release = {"tag_name": "v1.2.3", "draft": True, "assets": []}
        with self.assertRaisesRegex(sut.ReleasePublishError, "2 releases have the tag v1.2.3"):
            self.publish()
        self.assertFalse(self.asset_calls("upload"))
        self.assertFalse(self.site_pushed)

    def test_list_lag_is_waited_for(self) -> None:
        # The first lookup finds nothing before the draft is created, then three more come back empty
        self.gh.list_lag = 4
        self.publish()
        self.assertEqual(self.sleeps, [3, 3, 3])
        self.assertFalse(self.gh.release["draft"])

    def test_list_that_never_shows_the_draft_fails_after_a_minute(self) -> None:
        self.gh.list_lag = 10_000
        with self.assertRaisesRegex(sut.ReleasePublishError, "not in the release list after 60s"):
            self.publish()
        self.assertEqual(self.sleeps, [3] * 20)
        self.assertTrue(self.gh.release["draft"])
        self.assertFalse(self.asset_calls("upload"))
        self.assertFalse(self.site_pushed)

    def test_zero_major_is_prerelease_not_latest(self) -> None:
        self.use_tag("v0.1.0")
        message = self.publish()
        create, = self.asset_calls("create")
        self.assertIn("--prerelease", create)
        self.assertEqual(create[-1], "the-beper/Kinefin")
        edit, = self.asset_calls("edit")
        self.assertEqual(edit[3:7], ("--draft=false", "--prerelease=true", "--latest=false", "--repo"))
        self.assertTrue(self.gh.release["prerelease"])
        self.assertIsNone(self.gh.latest_tag)
        self.assertIn("pre-release, not the latest release", message)

    def test_one_zero_zero_is_latest(self) -> None:
        self.use_tag("v1.0.0")
        message = self.publish()
        create, = self.asset_calls("create")
        self.assertNotIn("--prerelease", create)
        edit, = self.asset_calls("edit")
        self.assertEqual(edit[3:7], ("--draft=false", "--prerelease=false", "--latest=true", "--repo"))
        self.assertFalse(self.gh.release["prerelease"])
        self.assertEqual(self.gh.latest_tag, "v1.0.0")
        self.assertIn("is the latest release", message)

    def test_is_prerelease_follows_the_major_version(self) -> None:
        self.assertEqual(
            [sut.is_prerelease(tag) for tag in ("v0.0.1", "v0.10.0", "v1.0.0", "v10.0.0", "v2.0.0")],
            [True, True, False, False, False],
        )

    def test_resumed_draft_gets_prerelease_flag(self) -> None:
        self.use_tag("v0.1.0")
        # A draft that an earlier run created without --prerelease
        self.gh.release = {
            "tag_name": "v0.1.0", "draft": True, "prerelease": False,
            "assets": [self.gh.asset(self.bundle), self.gh.asset(self.provenance), self.gh.asset(self.sums)],
        }
        self.publish()
        edit, = self.asset_calls("edit")
        self.assertIn("--prerelease=true", edit)
        self.assertTrue(self.gh.release["prerelease"])
        self.assertFalse(self.asset_calls("upload"))

    def test_published_by_hand_still_pushes_site(self) -> None:
        self.use_tag("v0.1.0")
        # Published by hand as a normal release, and so the latest one
        self.gh.release = {
            "tag_name": "v0.1.0", "draft": False, "prerelease": False,
            "assets": [self.gh.asset(self.bundle), self.gh.asset(self.provenance), self.gh.asset(self.sums)],
        }
        self.gh.latest_tag = "v0.1.0"
        message = self.publish()
        self.assertIn("already published", message)
        self.assertTrue(self.site_pushed)
        self.assertFalse(self.asset_calls("upload"))
        edit, = self.asset_calls("edit")
        self.assertLess(self.gh.calls.index(("site",)), self.gh.calls.index(edit))
        self.assertTrue(self.gh.release["prerelease"])
        self.assertIsNone(self.gh.latest_tag)

    def test_published_release_with_other_assets_gets_no_site(self) -> None:
        self.gh.release = {
            "tag_name": "v1.2.3", "draft": False,
            "assets": [self.gh.asset(self.bundle), self.gh.asset(self.sums)],
        }
        with self.assertRaises(sut.ReleasePublishError):
            self.publish()
        self.assertFalse(self.site_pushed)

    def test_release_stays_draft_when_the_site_step_fails(self) -> None:
        for error in (
            publish_site.SitePublishError("Pages does not serve the new repo/summary"),
            publish_site.StaleBase("gh-pages moved"),
        ):
            with self.subTest(error=type(error).__name__):
                self.gh = FakeGitHub()
                self.site_error = error
                with self.assertRaises(type(error)):
                    self.publish()
                self.assertTrue(self.gh.release["draft"])
                self.assertEqual(len(self.gh.release["assets"]), 3)
                self.assertFalse(self.asset_calls("edit"))
                self.site_error = None
                self.publish()
                self.assertFalse(self.gh.release["draft"])
                self.assertEqual(len(self.asset_calls("upload")), 1)

    def test_tag_moved_during_the_pages_wait_leaves_draft(self) -> None:
        def move_tag() -> None:
            self.gh.ref_object = {"type": "commit", "sha": MOVED_COMMIT}

        self.during_site = move_tag
        with self.assertRaisesRegex(sut.ReleasePublishError, "not the workflow commit"):
            self.publish()
        self.assertTrue(self.gh.release["draft"])
        self.assertFalse(self.asset_calls("edit"))

    def test_latest_state_is_confirmed_after_publication(self) -> None:
        self.use_tag("v0.1.0")
        self.gh.stuck_latest = "v0.1.0"
        with self.assertRaisesRegex(sut.ReleasePublishError, "pre-release v0.1.0 is the latest release"):
            self.publish()
        self.gh = FakeGitHub()
        self.use_tag("v1.0.0")
        self.gh.stuck_latest = "v0.9.0"
        with self.assertRaisesRegex(sut.ReleasePublishError, "the latest release is v0.9.0, not v1.0.0"):
            self.publish()
        self.gh = FakeGitHub()
        self.use_tag("v1.0.0")
        self.gh.latest_hidden = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "the latest release is None, not v1.0.0"):
            self.publish()
        self.gh = FakeGitHub()
        self.use_tag("v0.1.0")
        self.gh.latest_hidden = True
        self.publish()

    def test_latest_lookup_error_other_than_404_fails_closed(self) -> None:
        self.use_tag("v0.1.0")
        self.gh.fail_latest = True
        with self.assertRaisesRegex(sut.ReleasePublishError, "HTTP 500"):
            self.publish()

    def test_prerelease_flag_must_be_confirmed(self) -> None:
        self.use_tag("v0.1.0")
        fake = self.gh

        def ignoring_flags(*args: str) -> str:
            if args[:2] == ("release", "edit"):
                fake.calls.append(args)
                fake.release["draft"] = False
                fake.release["prerelease"] = False
                return ""
            return fake(*args)

        with self.assertRaisesRegex(sut.ReleasePublishError, "could not be confirmed"):
            sut.publish(
                "v0.1.0", self.root, "the-beper/Kinefin", EXPECTED_COMMIT, "12345", "sha256:" + "b" * 64,
                ignoring_flags, site=self.site, sleep=self.sleeps.append,
            )

    def test_gh_runner_reports_404_apart_from_other_errors(self) -> None:
        for stderr, expected in (
            ("gh: Not Found (HTTP 404)", sut.GitHubNotFound),
            ("gh: Server Error (HTTP 500)", sut.ReleasePublishError),
            ("gh: Not Found", sut.ReleasePublishError),
        ):
            with self.subTest(stderr=stderr):
                done = mock.Mock(returncode=1, stderr=stderr, stdout="")
                with mock.patch.object(sut.subprocess, "run", return_value=done):
                    with self.assertRaises(sut.ReleasePublishError) as caught:
                        sut.gh("api", "repos/x/y/releases/latest")
                self.assertIs(type(caught.exception), expected)

    def run_main(self, **overrides: str | None) -> tuple[int, str]:
        env = {
            "GITHUB_REPOSITORY": "the-beper/Kinefin",
            "RELEASE_COMMIT": EXPECTED_COMMIT,
            "GITHUB_RUN_ID": "12345",
            "RELEASE_IMAGE_DIGEST": "sha256:" + "b" * 64,
            "SITE_TAR": str(self.site_tar),
            "EXPECT_SITE_SHA256": hashlib.sha256(b"site archive").hexdigest(),
            "SITE_BASE_SHA": "-",
            "SITE_URL": "https://the-beper.github.io/Kinefin",
            **overrides,
        }
        env = {key: value for key, value in env.items() if value is not None}
        real_publish = sut.publish
        self.served: list[tuple] = []
        self.pushed: list[tuple] = []

        def site_publish(*args) -> str:
            self.pushed.append(args)
            if self.site_error is not None:
                raise self.site_error
            return "c" * 40

        def site_wait(*args) -> None:
            self.served.append(args)
            if self.wait_error is not None:
                raise self.wait_error

        stderr = io.StringIO()
        with (mock.patch.dict(os.environ, env, clear=True),
              mock.patch.object(sys, "argv", ["publish_release.py", self.tag, str(self.root)]),
              mock.patch.object(sut, "publish", lambda *a, **k: real_publish(*a, runner=self.gh, sleep=self.sleeps.append, **k)),
              mock.patch.object(publish_site, "publish", site_publish),
              mock.patch.object(publish_site, "wait_served", site_wait),
              contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO())):
            return sut.main(), stderr.getvalue()

    def test_site_inputs_are_validated_before_any_request(self) -> None:
        for name, bad, fragment in (
            ("SITE_TAR", None, "SITE_TAR"), ("SITE_TAR", str(self.root / "missing.tar"), "SITE_TAR"),
            ("SITE_TAR", str(self.root), "SITE_TAR"),
            ("EXPECT_SITE_SHA256", None, "EXPECT_SITE_SHA256"), ("EXPECT_SITE_SHA256", "A" * 64, "EXPECT_SITE_SHA256"),
            ("EXPECT_SITE_SHA256", "0" * 64, "not the one the finish job produced"),
            ("SITE_BASE_SHA", None, "SITE_BASE_SHA"), ("SITE_BASE_SHA", "", "SITE_BASE_SHA"),
            ("SITE_BASE_SHA", "abc", "SITE_BASE_SHA"), ("SITE_BASE_SHA", "G" * 40, "SITE_BASE_SHA"),
            ("SITE_URL", None, "SITE_URL"), ("SITE_URL", "http://the-beper.github.io/Kinefin", "SITE_URL"),
            ("SITE_URL", "https://the-beper.github.io/Kinefin/", "SITE_URL"), ("SITE_URL", "https://x.example/a b", "SITE_URL"),
            ("SITE_URL", "file:///etc", "SITE_URL"),
            ("REPO_REMOTE", "git@github.com:the-beper/Kinefin.git", "REPO_REMOTE"),
            ("REPO_REMOTE", "https://github.com/a b", "REPO_REMOTE"),
        ):
            with self.subTest(name=name, bad=bad):
                code, message = self.run_main(**{name: bad})
                self.assertEqual(code, 1)
                self.assertIn(fragment, message)
                self.assertEqual(self.gh.calls, [])
                self.assertEqual(self.pushed, [])

    def test_site_inputs_reach_the_site_step(self) -> None:
        code, _ = self.run_main()
        self.assertEqual(code, 0)
        (archive, digest, base, remote, tag, site_dir), = self.pushed
        self.assertEqual(archive, self.site_tar)
        self.assertEqual((digest, base, remote, tag), (hashlib.sha256(b"site archive").hexdigest(), None, "https://github.com/the-beper/Kinefin.git", "v1.2.3"))
        self.assertEqual(self.served, [(site_dir, "https://the-beper.github.io/Kinefin")])
        self.assertFalse(self.gh.release["draft"])
        self.gh = FakeGitHub()
        code, _ = self.run_main(SITE_BASE_SHA="d" * 40, REPO_REMOTE="file:///tmp/origin.git")
        self.assertEqual(code, 0)
        self.assertEqual(self.pushed[0][2:4], ("d" * 40, "file:///tmp/origin.git"))

    def test_stale_site_base_exits_3(self) -> None:
        self.site_error = publish_site.StaleBase("gh-pages moved: build again")
        code, message = self.run_main()
        self.assertEqual(code, 3)
        self.assertIn("gh-pages moved", message)
        self.assertTrue(self.gh.release["draft"])
        self.assertFalse(self.asset_calls("edit"))

    def test_site_push_and_pages_failures_exit_1_and_keep_the_draft(self) -> None:
        for attribute in ("site_error", "wait_error"):
            with self.subTest(attribute=attribute):
                self.gh = FakeGitHub()
                self.site_error = self.wait_error = None
                error = publish_site.SitePublishError(f"{attribute} failed")
                setattr(self, attribute, error)
                code, message = self.run_main()
                self.assertEqual(code, 1)
                self.assertIn(str(error), message)
                self.assertTrue(self.gh.release["draft"])
                self.assertFalse(self.asset_calls("edit"))

    def test_workflow_does_not_publish_before_remote_sha_check(self) -> None:
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn('python3 -I build/release/publish_release.py "$RELEASE_TAG" release-assets', workflow)
        self.assertIn('RELEASE_COMMIT: ${{ github.sha }}', workflow)
        publish_job = workflow.split("  publish:\n", 1)[1]
        self.assertIn("actions/checkout@", publish_job)
        self.assertIn("persist-credentials: false", publish_job)
        self.assertNotIn("gh release create", publish_job)
        self.assertNotIn("gh release edit", publish_job)


if __name__ == "__main__":
    unittest.main()
