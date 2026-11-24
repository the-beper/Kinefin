# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline tests for the gh-pages step of the release publisher, against a bare origin over file://."""

from __future__ import annotations

import base64
import contextlib
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build" / "release"))
import publish_site as sut  # noqa: E402


IDENTITY = {
    "GIT_AUTHOR_NAME": "other", "GIT_AUTHOR_EMAIL": "other@example.com",
    "GIT_COMMITTER_NAME": "other", "GIT_COMMITTER_EMAIL": "other@example.com",
}


def git(*args: str, input: str | None = None) -> str:
    process = subprocess.run(
        ["git", *args], check=True, capture_output=True, text=True, timeout=60, input=input,
        env={**os.environ, **IDENTITY},
    )
    return process.stdout.strip()


class RecordingGit:
    """sut.run_git with a record of every call and optional hooks around the push."""

    def __init__(self) -> None:
        self.calls: list[tuple[tuple[str, ...], dict]] = []
        self.before_push = None
        self.after_push = None

    def commands(self) -> list[str]:
        return [sut._subcommand(args) for args, _ in self.calls]

    def __call__(self, *args: str, env=None) -> str:
        self.calls.append((args, dict(env or {})))
        is_push = sut._subcommand(args) == "push"
        if is_push and self.before_push:
            self.before_push()
        output = sut.run_git(*args, env=env)
        if is_push and self.after_push:
            self.after_push()
        return output


class SitePublishTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.tmp = Path(temp.name)
        self.origin = self.tmp / "origin.git"
        git("init", "--quiet", "--bare", str(self.origin))
        self.remote = f"file://{self.origin}"
        self.git = RecordingGit()
        self.count = 0

    def make_site(self, summary: bytes = b"summary one", extra: dict[str, bytes] | None = None) -> tuple[Path, str]:
        self.count += 1
        source = self.tmp / f"site-source-{self.count}"
        files = {"repo/summary": summary, "repo/summary.sig": b"sig of " + summary, ".nojekyll": b"", **(extra or {})}
        for name, data in files.items():
            (source / name).parent.mkdir(parents=True, exist_ok=True)
            (source / name).write_bytes(data)
        archive = self.tmp / f"site-{self.count}.tar"
        with tarfile.open(archive, "w") as tar:
            tar.add(source, arcname=".")
        return archive, hashlib.sha256(archive.read_bytes()).hexdigest()

    def make_tar(self, members: list[tuple[tarfile.TarInfo, bytes | None]]) -> tuple[Path, str]:
        self.count += 1
        archive = self.tmp / f"site-{self.count}.tar"
        with tarfile.open(archive, "w") as tar:
            for info, data in members:
                tar.addfile(info, io.BytesIO(data) if data is not None else None)
        return archive, hashlib.sha256(archive.read_bytes()).hexdigest()

    def publish(self, archive: tuple[Path, str], base: str | None = None, tag: str = "v0.1.0") -> str:
        self.count += 1
        return sut.publish(
            archive[0], archive[1], base, self.remote, tag, self.tmp / f"unpacked-{self.count}", self.git,
        )

    def branch(self, name: str) -> str:
        return git("--git-dir", str(self.origin), "rev-parse", "--verify", "--quiet", f"refs/heads/{name}^{{commit}}") \
            if self.has_branch(name) else ""

    def has_branch(self, name: str) -> bool:
        return subprocess.run(
            ["git", "--git-dir", str(self.origin), "show-ref", "--verify", "--quiet", f"refs/heads/{name}"],
            check=False,
        ).returncode == 0

    def foreign_commit(self) -> str:
        tree = git("--git-dir", str(self.origin), "mktree", input="")
        commit = git("--git-dir", str(self.origin), "commit-tree", tree, "-m", "somebody else")
        git("--git-dir", str(self.origin), "update-ref", "refs/heads/gh-pages", commit)
        return commit

    def test_first_publication_has_no_previous(self) -> None:
        commit = self.publish(self.make_site())
        self.assertEqual(self.branch("gh-pages"), commit)
        self.assertFalse(self.has_branch("gh-pages-previous"))
        listing = git("--git-dir", str(self.origin), "ls-tree", "-r", "--name-only", commit).split()
        self.assertEqual(set(listing), {".nojekyll", "repo/summary", "repo/summary.sig"})
        self.assertEqual(git("--git-dir", str(self.origin), "rev-list", "--parents", "-n1", commit), commit)
        self.assertEqual(
            git("--git-dir", str(self.origin), "log", "-1", "--format=%an <%ae>|%cn <%ce>|%s", commit),
            "github-actions <github-actions@users.noreply.github.com>|"
            "github-actions <github-actions@users.noreply.github.com>|Flatpak repository for v0.1.0",
        )
        push, = [args for args, _ in self.git.calls if sut._subcommand(args) == "push"]
        self.assertNotIn("--atomic", push)
        self.assertIn("--force-with-lease=refs/heads/gh-pages:", push)

    def test_old_head_kept_as_previous(self) -> None:
        first = self.publish(self.make_site(b"one"))
        second = self.publish(self.make_site(b"two"), base=first, tag="v0.2.0")
        self.assertNotEqual(first, second)
        self.assertEqual(self.branch("gh-pages"), second)
        self.assertEqual(self.branch("gh-pages-previous"), first)
        self.assertEqual(git("--git-dir", str(self.origin), "rev-list", "--parents", "-n1", second), second)
        third = self.publish(self.make_site(b"three"), base=second, tag="v0.3.0")
        self.assertEqual(self.branch("gh-pages-previous"), second)
        self.assertEqual(self.branch("gh-pages"), third)
        pushes = [args for args, _ in self.git.calls if sut._subcommand(args) == "push"]
        self.assertIn("--atomic", pushes[1])
        self.assertIn(f"--force-with-lease=refs/heads/gh-pages:{first}", pushes[1])
        self.assertIn("--force-with-lease=refs/heads/gh-pages-previous:", pushes[1])

    def test_same_tree_not_pushed(self) -> None:
        archive = self.make_site()
        first = self.publish(archive)
        self.git.calls.clear()
        with contextlib.redirect_stdout(io.StringIO()) as out:
            again = self.publish(archive)
            after_other_release = self.publish(archive, base="b" * 40)
        self.assertEqual((again, after_other_release), (first, first))
        self.assertNotIn("push", self.git.commands())
        self.assertIn("no push", out.getvalue())
        self.assertEqual(self.branch("gh-pages"), first)
        self.assertFalse(self.has_branch("gh-pages-previous"))

    def test_moved_base_moves_nothing(self) -> None:
        first = self.publish(self.make_site(b"one"))
        other = self.foreign_commit()
        pushes = self.git.commands().count("push")
        for base in (first, None, "-", "c" * 40):
            with self.subTest(base=base):
                with self.assertRaises(sut.StaleBase):
                    self.publish(self.make_site(b"two"), base=base)
        self.assertEqual(self.branch("gh-pages"), other)
        self.assertFalse(self.has_branch("gh-pages-previous"))
        self.assertEqual(self.git.commands().count("push"), pushes)

    def test_missing_gh_pages_with_a_base_is_stale(self) -> None:
        with self.assertRaises(sut.StaleBase):
            self.publish(self.make_site(), base="a" * 40)
        self.assertFalse(self.has_branch("gh-pages"))

    def test_lost_ack_counts_as_pushed(self) -> None:
        def lose_ack() -> None:
            raise sut.SitePublishError("git push failed: connection reset")

        for base_first in (False, True):
            with self.subTest(base_first=base_first):
                self.git = RecordingGit()
                head = self.branch("gh-pages")
                self.git.after_push = lose_ack
                with contextlib.redirect_stdout(io.StringIO()) as out:
                    commit = self.publish(self.make_site(b"s" + bytes([base_first])), base=head or None)
                self.assertIn("acknowledgement was lost", out.getvalue())
                self.assertEqual(self.branch("gh-pages"), commit)
                self.assertEqual(self.branch("gh-pages-previous"), head)

    def test_refused_push_moves_neither_branch(self) -> None:
        first = self.publish(self.make_site(b"one"))
        hook = self.origin / "hooks" / "pre-receive"
        hook.write_text("#!/bin/sh\necho refused >&2\nexit 1\n", encoding="utf-8")
        hook.chmod(0o755)
        with self.assertRaises(sut.SitePublishError) as caught:
            self.publish(self.make_site(b"two"), base=first, tag="v0.2.0")
        self.assertNotIsInstance(caught.exception, sut.StaleBase)
        self.assertIn("neither branch has moved", str(caught.exception))
        self.assertEqual(self.branch("gh-pages"), first)
        self.assertFalse(self.has_branch("gh-pages-previous"))

    def test_writer_during_push_is_stale(self) -> None:
        first = self.publish(self.make_site(b"one"))
        self.git.before_push = self.foreign_commit
        with self.assertRaises(sut.StaleBase):
            self.publish(self.make_site(b"two"), base=first, tag="v0.2.0")
        self.assertNotEqual(self.branch("gh-pages"), first)
        self.assertFalse(self.has_branch("gh-pages-previous"))

    def test_lost_ack_of_a_first_publication_with_an_old_previous_branch_counts_as_pushed(self) -> None:
        orphan = self.foreign_commit()
        git("--git-dir", str(self.origin), "update-ref", "refs/heads/gh-pages-previous", orphan)
        git("--git-dir", str(self.origin), "update-ref", "-d", "refs/heads/gh-pages")

        def lose_ack() -> None:
            raise sut.SitePublishError("git push failed: connection reset")

        self.git.after_push = lose_ack
        commit = self.publish(self.make_site())
        self.assertEqual(self.branch("gh-pages"), commit)
        self.assertEqual(self.branch("gh-pages-previous"), orphan)

    def test_committed_bytes_are_the_archives_whatever_the_global_git_config(self) -> None:
        config = self.tmp / "gitconfig"
        attributes = self.tmp / "attributes"
        attributes.write_text("* text eol=lf\n", encoding="utf-8")
        config.write_text(f"[core]\n\tautocrlf = true\n\tattributesFile = {attributes}\n", encoding="utf-8")
        data = b"line one\r\nline two\r\n"
        with mock.patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": str(config)}):
            commit = self.publish(self.make_site(data))
        blob = subprocess.run(
            ["git", "--git-dir", str(self.origin), "cat-file", "blob", f"{commit}:repo/summary"],
            check=True, capture_output=True, timeout=60,
        ).stdout
        self.assertEqual(blob, data)

    def test_remote_must_be_an_https_or_file_url(self) -> None:
        archive = self.make_site()
        for remote in ("", "-oProxyCommand=x", "ext::sh -c id", "git@github.com:the-beper/Kinefin.git",
                       "http://github.com/the-beper/Kinefin.git", "https://github.com/a b", "/srv/origin.git",
                       "file://" + str(self.origin) + "\n"):
            with self.subTest(remote=remote):
                with self.assertRaisesRegex(sut.SitePublishError, "remote must be"):
                    sut.publish(*archive, None, remote, "v0.1.0", self.tmp / "r", self.git)
        self.assertEqual(self.git.calls, [])

    def test_unreadable_remote_after_failed_push_fails_closed(self) -> None:
        def lose_remote() -> None:
            raise sut.SitePublishError("git push failed")

        self.git.after_push = lose_remote
        real = self.git.__call__
        reads = 0

        def runner(*args: str, env=None) -> str:
            nonlocal reads
            if sut._subcommand(args) == "ls-remote":
                reads += 1
                if reads == 2:
                    raise sut.SitePublishError("git ls-remote failed")
            return real(*args, env=env)

        with self.assertRaisesRegex(sut.SitePublishError, "cannot be read again"):
            sut.publish(*self.make_site(), None, self.remote, "v0.1.0", self.tmp / "unpacked", runner)

    def test_site_tar_hash_and_unsafe_members_refused(self) -> None:
        archive, digest = self.make_site()
        for wrong in ("0" * 64, "short", digest.upper()):
            with self.subTest(digest=wrong[:8]):
                with self.assertRaises(sut.SitePublishError):
                    sut.publish(archive, wrong, None, self.remote, "v0.1.0", self.tmp / "u", self.git)
        with self.assertRaisesRegex(sut.SitePublishError, "not the one the finish job produced"):
            sut.publish(archive, "0" * 64, None, self.remote, "v0.1.0", self.tmp / "u", self.git)
        self.assertFalse((self.tmp / "u").exists())

        def entry(name: str, kind: bytes = tarfile.REGTYPE, link: str = "") -> tuple[tarfile.TarInfo, bytes | None]:
            info = tarfile.TarInfo(name)
            info.type = kind
            info.linkname = link
            data = b"x" if kind == tarfile.REGTYPE else None
            info.size = 1 if data else 0
            return info, data

        good = [entry("./repo/", tarfile.DIRTYPE), entry("./repo/summary"), entry("./repo/summary.sig")]
        cases = {
            "parent": [entry("../escape")],
            "nested parent": [entry("./repo/../../escape")],
            "absolute": [entry("/etc/passwd")],
            "git": [entry("./.git/config")],
            "nested git": [entry("./repo/.git/hooks/pre-commit")],
            "git upper case": [entry("./.GIT/config")],
            "git directory": [entry("./.git/", tarfile.DIRTYPE)],
            "attributes": [entry("./.gitattributes")],
            "nested attributes upper case": [entry("./repo/.GitAttributes")],
            "modules": [entry("./.gitmodules")],
            "nested modules": [entry("./repo/sub/.GITMODULES")],
            "backslash": [entry("./repo\\x")],
            "symlink": [entry("./link", tarfile.SYMTYPE, "/etc")],
            "hardlink": [entry("./hard", tarfile.LNKTYPE, "repo/summary")],
            "device": [entry("./dev", tarfile.CHRTYPE)],
            "fifo": [entry("./fifo", tarfile.FIFOTYPE)],
            "duplicate": [entry("./repo/summary")],
        }
        for label, bad in cases.items():
            with self.subTest(label):
                members = [*good, *bad]
                archive, digest = self.make_tar(members)
                target = self.tmp / f"unsafe-{label.replace(' ', '-')}"
                with self.assertRaises(sut.SitePublishError):
                    sut.publish(archive, digest, None, self.remote, "v0.1.0", target, self.git)
                self.assertEqual(list(target.rglob("*")) if target.exists() else [], [])
        for label, members in {"no signature": good[:2], "no summary": [good[0], good[2]]}.items():
            with self.subTest(label):
                archive, digest = self.make_tar(members)
                with self.assertRaisesRegex(sut.SitePublishError, "the site has no repo/summary"):
                    sut.publish(archive, digest, None, self.remote, "v0.1.0", self.tmp / f"u-{label}", self.git)
        self.assertEqual(self.git.calls, [])
        self.assertFalse(self.has_branch("gh-pages"))

    def test_occupied_site_directory_refused(self) -> None:
        archive = self.make_site()
        target = self.tmp / "occupied"
        target.mkdir()
        (target / "stale").write_text("x", encoding="utf-8")
        with self.assertRaisesRegex(sut.SitePublishError, "empty directory"):
            sut.publish(*archive[:2], None, self.remote, "v0.1.0", target, self.git)

    def test_pages_timeout_raises(self) -> None:
        archive = self.make_site()
        site_dir = self.tmp / "served"
        sut.unpack_site(archive[0], site_dir)
        now = 0.0
        sleeps: list[float] = []

        def sleep(seconds: float) -> None:
            nonlocal now
            sleeps.append(seconds)
            now += seconds

        urls: list[str] = []

        def stale(url: str) -> bytes:
            urls.append(url)
            return b"old repository"

        with self.assertRaisesRegex(sut.SitePublishError, "Pages does not serve the new repo/summary, repo/summary.sig"):
            sut.wait_served(site_dir, "https://pages.example/Kinefin", 60, 15, stale, sleep, lambda: now)
        self.assertEqual(sleeps, [15, 15, 15, 15])
        self.assertTrue(all(url.startswith("https://pages.example/Kinefin/repo/summary") and "?nocache=" in url for url in urls))

    def test_pages_wait_ends_when_both_files_are_served(self) -> None:
        archive = self.make_site(b"fresh")
        site_dir = self.tmp / "served"
        sut.unpack_site(archive[0], site_dir)
        now = 0.0
        sleeps: list[float] = []
        attempts = 0

        def sleep(seconds: float) -> None:
            nonlocal now
            sleeps.append(seconds)
            now += seconds

        def fetch(url: str) -> bytes:
            nonlocal attempts
            attempts += 1
            if attempts == 1:
                raise OSError("connection refused")
            if attempts <= 4:
                return b"HTTP 404 page"
            return (site_dir / "repo" / url.split("/repo/")[1].split("?")[0]).read_bytes()

        with contextlib.redirect_stdout(io.StringIO()):
            sut.wait_served(site_dir, "https://pages.example/Kinefin/", 600, 15, fetch, sleep, lambda: now)
        self.assertEqual(sleeps, [15, 15])

    def test_pages_serving_the_summary_but_not_the_signature_is_not_served(self) -> None:
        archive = self.make_site(b"fresh")
        site_dir = self.tmp / "served"
        sut.unpack_site(archive[0], site_dir)
        now = 0.0

        def fetch(url: str) -> bytes:
            return (site_dir / "repo/summary").read_bytes()

        def sleep(seconds: float) -> None:
            nonlocal now
            now += seconds

        with self.assertRaisesRegex(sut.SitePublishError, "new repo/summary.sig at"):
            sut.wait_served(site_dir, "https://pages.example/Kinefin", 30, 15, fetch, sleep, lambda: now)

    def fake_urlopen(self, final_url: str, body: bytes = b"data") -> tuple[mock.Mock, mock.MagicMock]:
        response = mock.MagicMock()
        response.geturl.return_value = final_url
        response.read.side_effect = lambda size: body[:size]
        opened = mock.MagicMock()
        opened.__enter__.return_value = response
        return mock.Mock(return_value=opened), response

    def test_fetch_https_refuses_other_schemes_and_redirects_to_them(self) -> None:
        for url in ("http://pages.example/x", "file:///etc/passwd", "ftp://pages.example/x"):
            with self.subTest(url=url):
                opener, _ = self.fake_urlopen("https://pages.example/x")
                with mock.patch.object(sut.urllib.request, "urlopen", opener):
                    with self.assertRaisesRegex(sut.SitePublishError, "must be https"):
                        sut.fetch_https(url)
                opener.assert_not_called()
        opener, _ = self.fake_urlopen("http://pages.example/elsewhere")
        with mock.patch.object(sut.urllib.request, "urlopen", opener):
            with self.assertRaisesRegex(sut.SitePublishError, "redirected to a URL that is not https"):
                sut.fetch_https("https://pages.example/x")
        opener, _ = self.fake_urlopen("https://cdn.example/x", b"served bytes")
        with mock.patch.object(sut.urllib.request, "urlopen", opener):
            self.assertEqual(sut.fetch_https("https://pages.example/x"), b"served bytes")
        request = opener.call_args.args[0]
        self.assertEqual(request.get_header("Cache-control"), "no-cache")
        self.assertEqual(opener.call_args.kwargs["timeout"], 30)

    def test_fetch_https_reads_at_most_one_byte_over_the_cap(self) -> None:
        opener, response = self.fake_urlopen("https://pages.example/x", b"x" * (sut.MAX_SERVED_BYTES + 100))
        with mock.patch.object(sut.urllib.request, "urlopen", opener):
            data = sut.fetch_https("https://pages.example/x")
        response.read.assert_called_once_with(sut.MAX_SERVED_BYTES + 1)
        self.assertEqual(len(data), sut.MAX_SERVED_BYTES + 1)
        # Over the cap counts as not served
        site_dir = self.tmp / "served"
        sut.unpack_site(self.make_site()[0], site_dir)
        now = 0.0

        def sleep(seconds: float) -> None:
            nonlocal now
            now += seconds

        with self.assertRaisesRegex(sut.SitePublishError, "Pages does not serve"):
            sut.wait_served(site_dir, "https://pages.example", 15, 15, lambda url: data, sleep, lambda: now)

    def test_token_only_in_git_environment(self) -> None:
        token = "ghs_secret_token_value"
        auth = base64.b64encode(f"x-access-token:{token}".encode()).decode()
        archive = self.make_site()
        https_remote = "https://github.com/the-beper/Kinefin.git"
        seen: list[tuple[tuple[str, ...], dict]] = []

        def runner(*args: str, env=None) -> str:
            seen.append((args, dict(env or {})))
            if sut._subcommand(args) in ("ls-remote", "push"):
                return ""
            return sut.run_git(*args, env=env)

        before = dict(os.environ)
        with mock.patch.dict(os.environ, {"GH_TOKEN": token, "GITHUB_ACTIONS": "true"}):
            with contextlib.redirect_stdout(io.StringIO()) as out:
                sut.publish(*archive, None, https_remote, "v0.1.0", self.tmp / "tok", runner)
            self.assertNotIn("GIT_CONFIG_VALUE_0", os.environ)
        self.assertEqual(dict(os.environ), before)
        self.assertEqual(out.getvalue().count(f"::add-mask::{auth}"), 1)
        self.assertNotIn(token, out.getvalue())
        network = [(args, env) for args, env in seen if sut._subcommand(args) in ("ls-remote", "push")]
        self.assertEqual(len(network), 2)
        for args, env in seen:
            self.assertNotIn(token, " ".join(args))
            self.assertNotIn(auth, " ".join(args))
            self.assertNotIn(token, " ".join(env.values()))
        for args, env in network:
            self.assertEqual(env["GIT_CONFIG_COUNT"], "1")
            self.assertEqual(env["GIT_CONFIG_KEY_0"], "http.https://github.com/.extraheader")
            self.assertEqual(env["GIT_CONFIG_VALUE_0"], f"AUTHORIZATION: basic {auth}")
        for args, env in seen:
            if (args, env) not in network:
                self.assertNotIn("GIT_CONFIG_VALUE_0", env)

    def test_token_not_sent_to_other_remotes_or_logged_outside_actions(self) -> None:
        token = "ghs_secret_token_value"
        with mock.patch.dict(os.environ, {"GH_TOKEN": token}):
            os.environ.pop("GITHUB_ACTIONS", None)
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertNotIn("GIT_CONFIG_VALUE_0", sut.git_environment(self.remote))
                self.assertNotIn("GIT_CONFIG_VALUE_0", sut.git_environment("http://github.com/x/y.git"))
                self.assertIn("GIT_CONFIG_VALUE_0", sut.git_environment("https://github.com/x/y.git"))
        self.assertNotIn("::add-mask::", out.getvalue())


if __name__ == "__main__":
    unittest.main()
