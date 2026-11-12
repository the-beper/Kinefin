# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable release-branding tests: do not require Qt, Inkscape or Flatpak."""

from __future__ import annotations

from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build/release"))
import verify_release_branding as branding  # noqa: E402


class BrandingTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.svg = self.root / branding.ICON / "scalable/apps" / (branding.APP_ID + ".svg")
        self.desktop = self.root / branding.DESKTOP
        self.metainfo = self.root / branding.METAINFO
        self.rasters = [
            self.root / branding.ICON / f"{size}x{size}/apps" / (branding.APP_ID + ".png")
            for size in (128, 256)
        ]
        for path in [self.svg, self.desktop, self.metainfo, *self.rasters]:
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path.relative_to(self.root), path)

    def test_valid_release_assets(self) -> None:
        branding.verify(self.root)
        for size, path in zip((128, 256), self.rasters):
            self.assertEqual(branding.png_pixels(path.read_bytes(), size)[:2], (size, size))

    def test_icon_is_missing(self) -> None:
        self.svg.unlink()
        with self.assertRaises(FileNotFoundError):
            branding.verify(self.root)

    def test_external_resource_in_svg_is_rejected(self) -> None:
        svg = self.svg.read_text()
        self.svg.write_text(svg.replace('fill="url(#fin)"', 'fill="url(https://evil.example/paint)"'))
        with self.assertRaisesRegex(branding.BrandingError, "non-local paint"):
            branding.verify(self.root)

    def test_svg_scripts_and_event_handlers_are_rejected(self) -> None:
        original = self.svg.read_text()
        for xml in (
            original.replace("<defs>", "<script>bad()</script><defs>"),
            original.replace('<svg xmlns=', '<svg onload="bad()" xmlns='),
            original.replace("<defs>", '<!DOCTYPE svg [<!ENTITY foo SYSTEM "file:///etc/passwd">]><defs>'),
        ):
            with self.subTest(xml=xml[:60]):
                self.svg.write_text(xml)
                with self.assertRaises(branding.BrandingError):
                    branding.verify(self.root)
        self.svg.write_text(original)

    def test_wrong_svg_geometry_is_rejected(self) -> None:
        self.svg.write_text(self.svg.read_text().replace('viewBox="0 0 48 48"', 'viewBox="0 0 64 64"'))
        with self.assertRaisesRegex(branding.BrandingError, "48px"):
            branding.verify(self.root)

    def test_png_corruption_is_rejected(self) -> None:
        contents = bytearray(self.rasters[0].read_bytes())
        contents[44] ^= 0xff
        self.rasters[0].write_bytes(contents)
        with self.assertRaisesRegex(branding.BrandingError, "CRC"):
            branding.verify(self.root)

    def test_png_trailing_data_is_rejected(self) -> None:
        self.rasters[1].write_bytes(self.rasters[1].read_bytes() + b"unexpected")
        with self.assertRaisesRegex(branding.BrandingError, "end marker"):
            branding.verify(self.root)

    def test_png_dimensions_are_checked(self) -> None:
        self.rasters[1].write_bytes(self.rasters[0].read_bytes())
        with self.assertRaisesRegex(branding.BrandingError, "256x256"):
            branding.verify(self.root)

    def test_png_must_be_rgba_and_visible(self) -> None:
        self.rasters[0].write_bytes(b"not a PNG")
        with self.assertRaisesRegex(branding.BrandingError, "signature"):
            branding.verify(self.root)

    def test_wrong_desktop_icon_and_link_handler_are_rejected(self) -> None:
        original = self.desktop.read_text()
        for before, after in (
            (f"Icon={branding.APP_ID}", "Icon=wrong"),
            ("Exec=kinefin %u", "Exec=kinefin"),
            ("MimeType=x-scheme-handler/jellyfin;", "MimeType=text/plain;"),
        ):
            with self.subTest(before=before):
                self.desktop.write_text(original.replace(before, after))
                with self.assertRaisesRegex(branding.BrandingError, "desktop entry"):
                    branding.verify(self.root)
        self.desktop.write_text(original)

    def test_scope_is_movie_and_tv_only(self) -> None:
        self.desktop.write_text(self.desktop.read_text().replace("Video;Qt;", "Video;Music;Qt;"))
        with self.assertRaisesRegex(branding.BrandingError, "not music"):
            branding.verify(self.root)

    def test_appstream_identity_must_match_desktop_entry(self) -> None:
        self.metainfo.write_text(self.metainfo.read_text().replace(
            "<binary>kinefin</binary>", "<binary>other</binary>"))
        with self.assertRaisesRegex(branding.BrandingError, "AppStream identity"):
            branding.verify(self.root)

    def test_cli_and_release_ci_gates(self) -> None:
        result = subprocess.run(
            [sys.executable, "-I", str(ROOT / "build/release/verify_release_branding.py")],
            cwd=self.root, capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("verified", result.stdout)
        ci = (ROOT / ".github/workflows/verify.yml").read_text()
        release = (ROOT / ".github/workflows/release.yml").read_text()
        self.assertIn("python3 -I build/release/verify_release_branding.py", ci)
        self.assertIn("python3 -I build/release/verify_release_branding.py", release)
        self.assertLess(release.index("python3 -I build/release/verify_release_branding.py"),
                        release.index("  build-flatpak:"))


if __name__ == "__main__":
    unittest.main()
