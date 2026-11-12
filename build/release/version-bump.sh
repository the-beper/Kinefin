#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Update the CMake version and AppStream releases together. Run from any directory.
set -eu
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
exec python3 "$root/build/release/version_bump.py" "$@"
