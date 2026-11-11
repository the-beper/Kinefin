# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Release build image pin tests; no GHCR credentials, Podman or network needed."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "build/release/pull-pinned-image.sh"
DIGEST = "sha256:" + "a1" * 32
IMAGE = "ghcr.io/the-beper/kinefin-flatpak@" + DIGEST


class ReleaseImagePinTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.dir = Path(temp.name)
        self.log = self.dir / "podman.log"
        self.summary = self.dir / "summary.md"
        bin_dir = self.dir / "bin"
        bin_dir.mkdir()
        fake = bin_dir / "podman"
        fake.write_text(
            "#!/bin/sh\n"
            'printf "%s\\n" "$*" >> "$PODMAN_TEST_LOG"\n'
            'if [ "$1" = login ]; then\n'
            '    IFS= read -r token || exit 11\n'
            '    [ "$token" = "$PODMAN_TEST_TOKEN" ] || exit 12\n'
            'fi\n'
            '[ "${PODMAN_TEST_FAIL:-}" != "$1" ] || exit 45\n',
            encoding="ascii",
        )
        fake.chmod(0o755)
        self.env = {
            **os.environ,
            "PATH": str(bin_dir) + os.pathsep + os.environ.get("PATH", ""),
            "KINEFIN_RELEASE_FLATPAK_DIGEST": DIGEST,
            "GHCR_USER": "kinefin-test",
            "GHCR_TOKEN": "private-test-token",
            "PODMAN_TEST_TOKEN": "private-test-token",
            "PODMAN_TEST_LOG": str(self.log),
            "GITHUB_STEP_SUMMARY": str(self.summary),
        }
        self.env.pop("PODMAN_TEST_FAIL", None)

    def run_pull(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(SCRIPT)], env=self.env, cwd=self.dir,
            text=True, capture_output=True, timeout=10, check=False,
        )

    def calls(self) -> list[str]:
        if not self.log.exists():
            return []
        return self.log.read_text(encoding="utf-8").splitlines()

    def test_uses_digest_only_and_logs_auditable_source(self) -> None:
        result = self.run_pull()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [
            "login ghcr.io -u kinefin-test --password-stdin",
            f"pull {IMAGE}",
            f"tag {IMAGE} localhost/kinefin-flatpak:fedora45",
        ])
        self.assertEqual(self.summary.read_text(encoding="utf-8"),
                         f"Flatpak release build image: `{IMAGE}`\n")
        self.assertIn(IMAGE, result.stdout)
        self.assertNotIn("private-test-token", result.stdout + result.stderr)

    def test_missing_digest_is_rejected_before_podman(self) -> None:
        self.env.pop("KINEFIN_RELEASE_FLATPAK_DIGEST")
        result = self.run_pull()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("KINEFIN_RELEASE_FLATPAK_DIGEST", result.stderr)
        self.assertEqual(self.calls(), [])
        self.assertFalse(self.summary.exists())

    def test_malformed_and_untrusted_refs_never_reach_podman(self) -> None:
        for invalid in (
            "", "fedora45", IMAGE, "sha256:" + "0" * 63,
            "sha256:" + "A" * 64, "sha256:" + "g" * 64,
            DIGEST + "\n", DIGEST + " --tls-verify=false",
            "sha512:" + "1" * 64,
        ):
            with self.subTest(invalid=invalid):
                self.env["KINEFIN_RELEASE_FLATPAK_DIGEST"] = invalid
                self.assertNotEqual(self.run_pull().returncode, 0)
                self.assertEqual(self.calls(), [])
                self.assertFalse(self.summary.exists())

    def test_missing_credentials_abort_before_podman(self) -> None:
        for missing in ("GHCR_TOKEN", "GHCR_USER"):
            with self.subTest(missing=missing):
                saved = self.env.pop(missing)
                result = self.run_pull()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("are required", result.stderr)
                self.assertEqual(self.calls(), [])
                self.env[missing] = saved

    def test_login_failure_prevents_pull_and_tag(self) -> None:
        self.env["PODMAN_TEST_FAIL"] = "login"
        self.assertNotEqual(self.run_pull().returncode, 0)
        self.assertEqual(self.calls(), ["login ghcr.io -u kinefin-test --password-stdin"])
        self.assertFalse(self.summary.exists())

    def test_pull_failure_prevents_tag(self) -> None:
        self.env["PODMAN_TEST_FAIL"] = "pull"
        self.assertNotEqual(self.run_pull().returncode, 0)
        self.assertEqual(len(self.calls()), 2)
        self.assertEqual(self.calls()[1], f"pull {IMAGE}")
        self.assertFalse(self.summary.exists())

    def test_tag_failure_prevents_success_report(self) -> None:
        self.env["PODMAN_TEST_FAIL"] = "tag"
        self.assertNotEqual(self.run_pull().returncode, 0)
        self.assertEqual(len(self.calls()), 3)
        self.assertFalse(self.summary.exists())

    def test_workflow_requires_pin_before_build(self) -> None:
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        build = workflow.split("  build-flatpak:\n", 1)[1].split("  verify:\n", 1)[0]
        self.assertIn("vars.KINEFIN_RELEASE_FLATPAK_DIGEST", build)
        self.assertIn("run: build/release/pull-pinned-image.sh", build)
        self.assertLess(build.index("run: build/release/pull-pinned-image.sh"),
                        build.index("run: build/flatpak/build-flatpak.sh"))
        self.assertNotIn("podman pull ghcr.io/the-beper/kinefin-flatpak:fedora45", build)


if __name__ == "__main__":
    unittest.main()
