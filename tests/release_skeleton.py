# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Put a copied checkout back to the unreleased skeleton, so the release fixtures do not depend on the version of the real one."""

from __future__ import annotations

from pathlib import Path
import re


SKELETON_VERSION = "0.0.1"
PROJECT = re.compile(r"^project\(kinefin VERSION [0-9.]+ LANGUAGES CXX\)$", re.MULTILINE)
RELEASES = re.compile(r"^  <releases>\n.*?^  </releases>\n", re.MULTILINE | re.DOTALL)


def reset_to_skeleton(root: Path) -> None:
    cmake = root / "CMakeLists.txt"
    text, count = PROJECT.subn(f"project(kinefin VERSION {SKELETON_VERSION} LANGUAGES CXX)", cmake.read_text(encoding="utf-8"))
    if count != 1:
        raise AssertionError("CMakeLists.txt has no single project(kinefin VERSION ...) line")
    cmake.write_text(text, encoding="utf-8")
    meta = root / "data/io.github.the_beper.Kinefin.metainfo.xml"
    text = RELEASES.sub("", meta.read_text(encoding="utf-8"), count=1)
    if "<releases" in text:
        raise AssertionError("the metainfo has a <releases> layout the skeleton reset does not know")
    meta.write_text(text, encoding="utf-8")
