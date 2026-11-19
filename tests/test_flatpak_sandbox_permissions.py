# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable regression tests for reviewed Flatpak grants and built keyfiles."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]
MANIFEST = "build/flatpak/io.github.the_beper.Kinefin.yml"
VERIFIER = "build/release/verify_flatpak_permissions.py"
sys.path.insert(0, str(SOURCE / "build/release"))
from verify_flatpak_permissions import SandboxError, check_manifest, check_metadata  # noqa: E402

# Representative ``flatpak info --show-metadata`` for the reviewed manifest, including the grants the org.kde.Platform runtime adds.
INSTALLED = """[Application]
name=io.github.the_beper.Kinefin
runtime=org.kde.Platform/x86_64/6.11
sdk=org.kde.Sdk/x86_64/6.11
command=kinefin

[Context]
shared=network;
sockets=pulseaudio;wayland;
devices=input;dri;
filesystems=xdg-config/kdeglobals:ro;

[Session Bus Policy]
org.mpris.MediaPlayer2.Kinefin.*=own
org.freedesktop.ScreenSaver=talk
org.kde.kwalletd6=talk
org.freedesktop.portal.Desktop=talk
com.canonical.AppMenu.Registrar=talk
org.kde.KGlobalSettings=talk
org.kde.kconfig.notify=talk

[System Bus Policy]
org.freedesktop.NetworkManager=talk
"""


class FlatpakSandboxPermissionsTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "checkout with spaces"
        for name in (MANIFEST, VERIFIER):
            output = self.root / name
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE / name, output)
        self.manifest = self.root / MANIFEST
        self.metadata = self.root / "metadata.ini"
        self.metadata.write_text(INSTALLED, encoding="utf-8")

    def check(self, *, with_metadata: bool = False, manifest: Path | None = None) -> subprocess.CompletedProcess[str]:
        cmd = [sys.executable, "-I", str(self.root / VERIFIER), "--manifest", str(manifest or self.manifest)]
        if with_metadata:
            cmd += ["--metadata", str(self.metadata)]
        return subprocess.run(cmd, cwd=self.root.parent, capture_output=True, text=True, timeout=10, check=False)

    def reject(self, message: str, *, with_metadata: bool = False) -> None:
        # Fast in-process negative tests; the positive and missing-file tests
        # also exercise the actual isolated command-line interface.
        with self.assertRaises(SandboxError) as raised:
            check_manifest(self.manifest.read_text(encoding="utf-8"))
            if with_metadata:
                check_metadata(self.metadata.read_text(encoding="utf-8"))
        self.assertIn(message, str(raised.exception))

    def replace(self, old: str, new: str, *, metadata: bool = False) -> None:
        target = self.metadata if metadata else self.manifest
        value = target.read_text(encoding="utf-8")
        self.assertIn(old, value)
        target.write_text(value.replace(old, new, 1), encoding="utf-8")

    def test_checked_in_manifest_is_approved_and_unchanged(self) -> None:
        contents = self.manifest.read_bytes()
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("permissions and application identity verified", result.stdout)
        self.assertEqual(self.manifest.read_bytes(), contents)

    def test_approved_installed_bundle_is_accepted_without_mutating_files(self) -> None:
        contents = self.metadata.read_bytes()
        result = self.check(with_metadata=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("installed bundle", result.stdout)
        self.assertEqual(self.metadata.read_bytes(), contents)

    def test_generated_flathub_manifest_preserves_permissions(self) -> None:
        sys.path.insert(0, str(SOURCE / "build/release"))
        from prepare_flathub import render
        output = self.root / "generated.yml"
        output.write_text(render(self.manifest.read_text(encoding="utf-8"), "v0.0.2", "f" * 40), encoding="utf-8")
        result = self.check(manifest=output)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_broad_grants_in_manifest_are_rejected(self) -> None:
        original = self.manifest.read_text(encoding="utf-8")
        for arg in ("--filesystem=home", "--filesystem=host", "--socket=x11", "--socket=session-bus",
                    "--socket=system-bus", "--device=all", "--share=ipc", "--talk-name=org.kde.*"):
            with self.subTest(arg=arg):
                self.manifest.write_text(original.replace("  - --share=network", "  - --share=network\n  - " + arg), encoding="utf-8")
                self.reject("unapproved finish-args")

    def test_missing_grant_is_rejected(self) -> None:
        self.replace("  - --socket=wayland\n", "")
        self.reject("unapproved finish-args")

    def test_duplicate_grant_is_rejected(self) -> None:
        self.replace("  - --socket=wayland\n", "  - --socket=wayland\n  - --socket=wayland\n")
        self.reject("duplicate finish-args")

    def test_identity_changes_are_rejected(self) -> None:
        original = self.manifest.read_text(encoding="utf-8")
        for old, new, error in (
            ("app-id: io.github.the_beper.Kinefin", "app-id: org.example.Other", "app-id must be"),
            ('runtime-version: "6.11"', 'runtime-version: "6.12"', "runtime-version must be"),
            ("sdk: org.kde.Sdk", "sdk: org.gnome.Sdk", "sdk must be"),
            ("command: kinefin", "command: sh", "command must be"),
        ):
            with self.subTest(old=old):
                self.manifest.write_text(original.replace(old, new), encoding="utf-8")
                self.reject(error)

    def test_missing_duplicate_and_complex_yaml_list_are_rejected(self) -> None:
        original = self.manifest.read_text(encoding="utf-8")
        variants = (
            ("finish-args:", "other-args:", "missing finish-args"),
            ("finish-args:", "finish-args: [--socket=wayland]", "plain YAML block list"),
            ("finish-args:", "finish-args: *shared", "YAML documents, anchors and aliases"),
            ("finish-args:", "finish-args:\nfinish-args:", "duplicate top-level finish-args"),
            ("  - --socket=wayland", "    - --socket=wayland", "unsupported finish-args entry"),
            ("  - --socket=wayland", "  - !include wayland", "unsupported finish-args entry"),
        )
        for old, new, error in variants:
            with self.subTest(new=new):
                self.manifest.write_text(original.replace(old, new, 1), encoding="utf-8")
                self.reject(error)

    def test_cli_rejects_broadened_manifest(self) -> None:
        self.replace("  - --share=network", "  - --share=network\n  - --filesystem=home")
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unapproved finish-args", result.stderr)

    def test_non_block_yaml_and_references_are_rejected(self) -> None:
        self.replace("  - name: qtwebsockets", "  - name: qtwebsockets\n    <<: *extra")
        self.reject("YAML documents, anchors and aliases")

    def test_installed_app_identity_must_match(self) -> None:
        original = self.metadata.read_text(encoding="utf-8")
        for old, new in (
            ("name=io.github.the_beper.Kinefin", "name=org.example.Other"),
            ("runtime=org.kde.Platform/x86_64/6.11", "runtime=org.kde.Platform/x86_64/6.12"),
            ("sdk=org.kde.Sdk/x86_64/6.11", "sdk=org.gnome.Sdk/x86_64/6.11"),
            ("command=kinefin", "command=sh"),
        ):
            with self.subTest(old=old):
                self.metadata.write_text(original.replace(old, new), encoding="utf-8")
                self.reject("installed [Application]", with_metadata=True)

    def test_installed_context_grants_must_match_exactly(self) -> None:
        original = self.metadata.read_text(encoding="utf-8")
        for old, new, name in (
            ("shared=network;", "shared=network;ipc;", "shared"),
            ("shared=network;", "shared=", "shared"),
            ("sockets=pulseaudio;wayland;", "sockets=pulseaudio;wayland;x11;", "sockets"),
            ("sockets=pulseaudio;wayland;", "sockets=pulseaudio;", "sockets"),
            ("devices=input;dri;", "devices=all;", "devices"),
            ("devices=input;dri;", "devices=input;dri;input;", "duplicate entries"),
        ):
            with self.subTest(new=new):
                self.metadata.write_text(original.replace(old, new), encoding="utf-8")
                self.reject(name, with_metadata=True)

    def test_installed_filesystem_persistent_and_extra_context_grants_are_rejected(self) -> None:
        original = self.metadata.read_text(encoding="utf-8")
        for field in ("persistent=.config;", "features=devel;", "usb=all;", "other-context-permission=unsafe"):
            with self.subTest(field=field):
                self.metadata.write_text(original.replace("[Context]\n", "[Context]\n" + field + "\n"), encoding="utf-8")
                self.reject("unexpected installed [Context] permission", with_metadata=True)
        for filesystems in ("filesystems=home;", "filesystems=xdg-config/kdeglobals:rw;"):
            with self.subTest(filesystems=filesystems):
                self.metadata.write_text(original.replace("filesystems=xdg-config/kdeglobals:ro;", filesystems), encoding="utf-8")
                self.reject("unexpected installed [Context] permission", with_metadata=True)

    def test_installed_bus_policy_expansion_or_missing_grant_fails(self) -> None:
        original = self.metadata.read_text(encoding="utf-8")
        for old, new, error in (
            ("org.kde.kwalletd6=talk", "org.kde.kwalletd6=own", "[Session Bus Policy]"),
            ("org.kde.kwalletd6=talk", "", "[Session Bus Policy]"),
            ("org.freedesktop.NetworkManager=talk", "org.freedesktop.NetworkManager=own", "[System Bus Policy]"),
            ("org.freedesktop.NetworkManager=talk", "org.freedesktop.NetworkManager=talk\norg.freedesktop.*=talk", "[System Bus Policy]"),
        ):
            with self.subTest(new=new):
                self.metadata.write_text(original.replace(old, new), encoding="utf-8")
                self.reject(error, with_metadata=True)

    def test_installed_unreviewed_environment_and_usb_permissions_rejected(self) -> None:
        original = self.metadata.read_text(encoding="utf-8")
        for block, error in (
            ("[Environment]\nLD_PRELOAD=/app/payload.so", "[Environment]"),
            ("[USB Devices]\nenumerable-devices=all", "[USB Devices]"),
            ("[A11y Bus Policy]\norg.example.Other=own", "[A11y Bus Policy]"),
        ):
            with self.subTest(block=block):
                self.metadata.write_text(original + "\n" + block + "\n", encoding="utf-8")
                self.reject(error, with_metadata=True)

    def test_corrupted_or_duplicated_installed_metadata_rejected(self) -> None:
        original = self.metadata.read_text(encoding="utf-8")
        for corruption in (original + "\n[Application]\nname=other", original.replace("name=io.github.the_beper.Kinefin", "name=io.github.the_beper.Kinefin\nname=other"), "not-ini"):
            with self.subTest(corruption=corruption[-30:]):
                self.metadata.write_text(corruption, encoding="utf-8")
                self.reject("invalid installed Flatpak metadata", with_metadata=True)

    def test_missing_manifest_and_metadata_files_fail_closed(self) -> None:
        self.manifest.unlink()
        result = self.check(with_metadata=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No such file", result.stderr)

    def test_ci_and_release_gate_before_build_and_flathub_upload(self) -> None:
        ci = (SOURCE / ".github/workflows/verify.yml").read_text(encoding="utf-8")
        release = (SOURCE / ".github/workflows/release.yml").read_text(encoding="utf-8")
        command = "python3 -I build/release/verify_flatpak_permissions.py"
        self.assertIn(command, ci)
        self.assertLess(ci.index(command), ci.index("  check:"))
        self.assertEqual(release.count(command), 2)
        self.assertLess(release.index(command), release.index("  build-flatpak:"))
        self.assertLess(release.index("Verify generated Flathub permissions"), release.index("          name: flathub-manifest"))

    def test_smoke_checks_installed_metadata_before_launch_and_mounts_read_only(self) -> None:
        smoke = (SOURCE / "build/flatpak/test-flatpak.sh").read_text(encoding="utf-8")
        container = (SOURCE / "build/flatpak/Containerfile").read_text(encoding="utf-8")
        self.assertIn("dbus-daemon python3", container)
        for path in ("/manifest.yml:ro", "/verify-permissions.py:ro"):
            self.assertIn(path, smoke)
        command = 'flatpak info --system --show-metadata "$id"'
        self.assertIn(command, smoke)
        self.assertIn('python3 -I /verify-permissions.py --manifest /manifest.yml', smoke)
        self.assertLess(smoke.index(command), smoke.index("flatpak run --env=QT_QPA_PLATFORM=offscreen"))
        self.assertEqual(smoke.count('HOME=$(mktemp -d)'), 1)


if __name__ == "__main__":
    unittest.main()
