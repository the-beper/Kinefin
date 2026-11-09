#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Portable subset: does not pretend to replace a Qt/KF6/Flatpak/live-server run.
set -eu
cd "$(dirname "$0")/../.."
python3 -m unittest discover -s tests -p 'test_*.py' -v
python3 -m unittest discover -s scripts -p 'test_po.py' -v
python3 scripts/po.py status --strict
python3 scripts/testing/probe_jellyfin.py --fixture
find build scripts po -type f -name '*.sh' -exec sh -n {} \;
compiler=${CXX:-g++}
mkdir -p dist/tmp/portable
"$compiler" -std=c++20 -Wall -Wextra -Werror -pedantic -I src \
    src/player/PlaybackSession.cpp tests/test_playback_session_portable.cpp \
    -o dist/tmp/portable/playback_session_test
./dist/tmp/portable/playback_session_test
