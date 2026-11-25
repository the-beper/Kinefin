# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail-closed, network-free regression tests for Flatpak source pinning."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "build/release"))
from prepare_flathub import render  # noqa: E402

MANIFEST = "build/flatpak/io.github.the_beper.Kinefin.yml"
VERIFIER = "build/release/verify_flatpak_sources.py"
TAG = "v0.0.2"
COMMIT = "a" * 40


class FlatpakSourcesTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "checkout with spaces"
        for file in (MANIFEST, VERIFIER):
            dest = self.root / file
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(SOURCE / file, dest)
        self.manifest = self.root / MANIFEST

    def run_verify(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-I", str(self.root / VERIFIER), *args],
            cwd=self.root.parent, capture_output=True, text=True,
            check=False, timeout=10,
        )

    def replace(self, old: str, new: str) -> None:
        contents = self.manifest.read_text(encoding="utf-8")
        self.assertIn(old, contents)
        self.manifest.write_text(contents.replace(old, new, 1), encoding="utf-8")

    def fails(self, message: str, *args: str) -> None:
        process = self.run_verify(*args)
        self.assertNotEqual(process.returncode, 0, process.stdout)
        self.assertIn(message, process.stderr)

    def generated(self) -> Path:
        output = self.root / "generated.yml"
        output.write_text(render(self.manifest.read_text(encoding="utf-8"), TAG, COMMIT), encoding="utf-8")
        return output

    def test_local_sources_valid_from_any_directory_and_read_only(self) -> None:
        original = self.manifest.read_bytes()
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("10 immutable or approved local sources", result.stdout)
        self.assertEqual(self.manifest.read_bytes(), original)

    def test_real_flathub_generator_output_accepted_with_matching_tag_and_commit(self) -> None:
        generated = self.generated()
        result = self.run_verify(
            "--manifest", str(generated), "--flathub-tag", TAG,
            "--flathub-commit", COMMIT,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("10 immutable", result.stdout)

    def test_flathub_fails_without_both_release_identifiers(self) -> None:
        generated = self.generated()
        self.fails("requires a valid release tag and full commit", "--manifest", str(generated), "--flathub-tag", TAG)
        self.fails("requires a valid release tag and full commit", "--manifest", str(generated), "--flathub-commit", COMMIT)
        self.fails("requires a valid release tag and full commit", "--manifest", str(generated), "--flathub-tag", "v1.2.3-rc1", "--flathub-commit", COMMIT)

    def test_flathub_rejects_wrong_tag_commit_and_git_url(self) -> None:
        generated = self.generated()
        args = ("--manifest", str(generated), "--flathub-tag", TAG, "--flathub-commit", COMMIT)
        self.fails("must match the release tag", "--manifest", str(generated), "--flathub-tag", "v1.0.0", "--flathub-commit", COMMIT)
        self.fails("must match the release tag", "--manifest", str(generated), "--flathub-tag", TAG, "--flathub-commit", "b" * 40)
        contents = generated.read_text(encoding="utf-8")
        generated.write_text(contents.replace("https://github.com/the-beper/Kinefin.git", "https://example.com/Kinefin.git"), encoding="utf-8")
        self.fails("must match the release tag", *args)

    def test_local_directory_source_is_not_allowed_for_flathub(self) -> None:
        self.fails(
            "local directory sources are permitted only",
            "--manifest", str(self.manifest), "--flathub-tag", TAG, "--flathub-commit", COMMIT,
        )

    def test_local_directory_source_is_not_allowed_in_dependencies(self) -> None:
        self.replace("          - type: git\n            url: https://github.com/haasn/libplacebo.git", "          - type: dir\n            path: ../..\n            skip: [.git, dist]\n          - type: git\n            url: https://github.com/haasn/libplacebo.git")
        self.fails("local directory sources are permitted only")

    def test_directory_path_and_skip_must_match_exact_local_checkout(self) -> None:
        self.replace("        path: ../..", "        path: ../../..")
        self.fails("local directory sources are permitted only")

    def test_missing_and_mutable_git_commits_rejected_even_with_tag(self) -> None:
        original = self.manifest.read_text(encoding="utf-8")
        old = "commit: 275f7dea27924cdd34ab2e6c20e448b6bc4fab65"
        self.assertIn(old, original)
        for bad in ("", "main", "a" * 39, "A" * 40, "0" * 40):
            with self.subTest(commit=bad):
                self.manifest.write_text(original.replace(old, "commit: " + bad), encoding="utf-8")
                self.fails("missing value for commit" if not bad else "Git sources need a full, nonzero lowercase commit ID")

    def test_nested_modules_are_audited(self) -> None:
        self.replace("commit: 275f7dea27924cdd34ab2e6c20e448b6bc4fab65", "commit: master")
        self.fails("Git sources need a full, nonzero lowercase commit ID")

    def test_missing_invalid_or_uppercase_sha256_rejected(self) -> None:
        original = self.manifest.read_text(encoding="utf-8")
        old = "sha256: bcfebbfa2b65a6d88dc97ea18117831072373e38b5785a2afb874ed4df5070dd"
        self.assertIn(old, original)
        for bad in ("", "latest", "a" * 63, "A" * 64, "0" * 64):
            with self.subTest(sha=bad):
                self.manifest.write_text(original.replace(old, "sha256: " + bad), encoding="utf-8")
                self.fails("missing value for sha256" if not bad else "downloaded sources require a nonzero lowercase SHA-256")

    def test_file_sources_are_audited_not_just_archives(self) -> None:
        self.replace("sha256: 6ff5f56a0edd8248073446e5cc038f47d4b17c46b6af43cf438123d0ac911189", "sha256: absent")
        self.fails("downloaded sources require a nonzero lowercase SHA-256")

    def test_non_https_or_ambiguous_source_url_rejected(self) -> None:
        original = self.manifest.read_text(encoding="utf-8")
        old = "https://github.com/haasn/libplacebo.git"
        for new in (
            "http://github.com/haasn/libplacebo.git", "https://user:password@github.com/haasn/libplacebo.git",
            "https://github.com/haasn/libplacebo.git?branch=main", "https://github.com:443/haasn/libplacebo.git",
            "file:///tmp/checkout",
        ):
            with self.subTest(url=new):
                self.manifest.write_text(original.replace(old, new), encoding="utf-8")
                self.fails("source URL must be an unambiguous HTTPS URL")

    def test_mutable_branch_and_unknown_options_fail_closed(self) -> None:
        self.replace("        commit: 275f7dea27924cdd34ab2e6c20e448b6bc4fab65", "        branch: main")
        self.fails("unreviewed source property branch")

    def test_unknown_source_type_fails_closed(self) -> None:
        self.replace("      - type: archive", "      - type: extra-data")
        self.fails("unsupported source entry")

    def test_unsupported_sources_mapping_fails_closed(self) -> None:
        self.replace("    sources:\n", "    sources: *remote\n")
        self.fails("YAML anchors, aliases and merges are not supported")

    def test_yaml_merge_anchors_outside_sources_are_rejected(self) -> None:
        self.replace("  - name: qtwebsockets\n", "  - name: qtwebsockets\n    <<: *unknown")
        self.fails("YAML anchors, aliases and merges are not supported")

    def test_duplicate_source_keys_fail_closed(self) -> None:
        self.replace("            commit: 275f7dea27924cdd34ab2e6c20e448b6bc4fab65", "            commit: 275f7dea27924cdd34ab2e6c20e448b6bc4fab65\n            commit: " + "b" * 40)
        self.fails("duplicate commit")

    def test_missing_application_source_is_rejected(self) -> None:
        self.replace("      - type: dir\n        path: ../..\n        skip: [.git, dist]", "      # application source missing")
        self.fails("expected exactly one approved Kinefin application source")

    def test_workflows_check_both_local_and_flathub_before_artifact_upload(self) -> None:
        release = (SOURCE / ".github/workflows/release.yml").read_text(encoding="utf-8")
        ci = (SOURCE / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        command = "python3 -I build/release/verify_flatpak_sources.py"
        self.assertIn(command, ci)
        self.assertLess(ci.index(command), ci.index("  check:"))
        self.assertEqual(release.count(command), 2)
        self.assertLess(release.index(command), release.index("Prepare commit-pinned Flathub manifest"))
        self.assertLess(release.index("Verify generated Flathub sources"), release.index("          name: flathub-manifest"))
        self.assertLess(release.index("Verify generated Flathub sources"), release.index("  build-flatpak:"))


if __name__ == "__main__":
    unittest.main()
