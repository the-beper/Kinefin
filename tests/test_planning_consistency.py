#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable regression checks for release gates and design/code alignment."""
from __future__ import annotations

import importlib.util
import urllib.parse
from pathlib import Path
import re
import stat
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


def content(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class PlanningConsistency(unittest.TestCase):
    def test_mpris_and_sandbox_scope(self) -> None:
        manifest = content("build/flatpak/io.github.the_beper.Kinefin.yml")
        self.assertIn("--system-talk-name=org.freedesktop.NetworkManager", manifest)
        self.assertIn("--own-name=org.mpris.MediaPlayer2.Kinefin.*", manifest)
        self.assertIn("--talk-name=org.kde.kwalletd6", manifest)
        self.assertNotIn("  - --filesystem=home", manifest)
        self.assertNotIn("  - --system-talk-name=org.freedesktop.Avahi", manifest)

    def test_jellyfin_uri_handler_is_declared_consistently(self) -> None:
        desktop = content("data/io.github.the_beper.Kinefin.desktop")
        metainfo = content("data/io.github.the_beper.Kinefin.metainfo.xml")
        self.assertIn("MimeType=x-scheme-handler/jellyfin;", desktop)
        self.assertIn("Exec=kinefin %u", desktop)
        self.assertIn("<mediatype>x-scheme-handler/jellyfin</mediatype>", metainfo)
        self.assertNotIn("x-scheme-handler/http;", desktop)
        self.assertNotIn("x-scheme-handler/https;", desktop)

    def test_all_qml_messages_are_in_committed_catalogs(self) -> None:
        potfiles = set(content("po/POTFILES.in").splitlines())
        self.assertIn("src/qml/Main.qml", potfiles)
        messages: set[tuple[str, str | None]] = set()
        for path in sorted((ROOT / "src/qml").glob("*.qml")):
            relative = path.relative_to(ROOT).as_posix()
            qml = content(relative)
            # Context is part of the gettext identity: a translation without the
            # matching i18nc context is not used at runtime.
            found = {(msgid, None) for msgid in re.findall(r'\bi18n\("([^"\\]+)"', qml)}
            found |= {(msgid, context) for context, msgid in
                      re.findall(r'\bi18nc\("([^"\\]+)"\s*,\s*"([^"\\]+)"', qml)}
            found |= {(msgid, None) for msgid in
                      re.findall(r'\bi18np\("([^"\\]+)"\s*,\s*"[^"\\]+"', qml)}
            found |= {(msgid, context) for context, msgid in
                      re.findall(r'\bi18ncp\("([^"\\]+)"\s*,\s*"([^"\\]+)"\s*,\s*"[^"\\]+"', qml)}
            # A page with translatable strings must be extracted, or its strings never reach a catalog.
            self.assertTrue(not found or relative in potfiles, relative)
            messages |= found
        self.assertGreaterEqual(len(messages), 6)
        spec = importlib.util.spec_from_file_location("kinefin_po", ROOT / "scripts/po.py")
        assert spec and spec.loader
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for lang in ("en", "it"):
            with self.subTest(lang=lang):
                keys = module.message_keys(ROOT / "po" / f"{lang}.po")
                self.assertTrue(messages <= keys, messages - keys)
                self.assertEqual(module.pending(lang), [])

    def test_d_f_checkboxes_do_not_mutate_drafts_on_programmatic_sync(self) -> None:
        # AbstractButton.toggled fires on any checked change, including a
        # property binding refresh. Only an actual user click may mutate the
        # filter draft or toggle the wrapping preference delegate.
        for file, start in (
            ("src/qml/FilterDialog.qml", "id: optionComponent"),
            ("src/qml/UserPreferencesPage.qml", "component LibraryCheckRow:"),
        ):
            with self.subTest(file=file):
                body = content(file).split(start, 1)[1]
                checkbox = body.split("Controls.CheckBox {", 1)[1].split("Controls.Label {", 1)[0]
                self.assertIn("onClicked:", checkbox)
                self.assertNotIn("onToggled:", checkbox)

    def test_reuse_configuration_and_license_texts(self) -> None:
        settings = tomllib.loads(content("REUSE.toml"))
        self.assertEqual(settings["version"], 1)
        self.assertTrue((ROOT / "LICENSES/GPL-3.0-or-later.txt").is_file())
        self.assertTrue((ROOT / "LICENSES/CC0-1.0.txt").is_file())
        self.assertTrue(any(a.get("SPDX-License-Identifier") == "CC0-1.0" and
                            "metainfo.xml" in a.get("path", "") for a in settings["annotations"]))

    def test_directly_invoked_shell_scripts_are_executable(self) -> None:
        scripts = (
            "build/build-env.sh", "build/check.sh", "build/ci/changes.sh",
            "build/flatpak/build-flatpak.sh", "build/flatpak/test-flatpak.sh", "po/extract.sh",
            "scripts/testing/run_portable_checks.sh",
        )
        for name in scripts:
            with self.subTest(script=name):
                self.assertTrue((ROOT / name).stat().st_mode & stat.S_IXUSR,
                                f"{name} must be executable in a checkout; CI calls it directly")

    def test_documentation_local_links_are_resolvable(self) -> None:
        """Regression against missing references in the root documentation."""
        broken = []
        for document in ROOT.glob("*.md"):
            for target in re.findall(r"(?<!!)\[[^\]]*\]\(([^)]+)\)", document.read_text(encoding="utf-8")):
                relative = target.split("#", 1)[0]
                if not relative or relative.startswith(("http:", "https:", "mailto:")):
                    continue
                decoded = urllib.parse.unquote(relative)
                if not (document.parent / decoded).exists():
                    broken.append(f"{document.relative_to(ROOT)} -> {target}")
        self.assertEqual(broken, [])

    def test_package_checks_and_snapshot_facts(self) -> None:
        self.assertTrue((ROOT / ".github/workflows/ci.yml").exists())
        self.assertTrue((ROOT / "scripts/po.py").exists())
        script = content("build/check.sh")
        self.assertIn("po/extract.sh --check", script)
        self.assertIn("po.py status --strict", script)
        self.assertIn("reuse lint", script)


if __name__ == "__main__":
    unittest.main()
