<!-- SPDX-FileCopyrightText: 2026 Kinefin contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Release version preparation

`build/release/version-bump.sh` prepares the two versioned source files for a
stable Kinefin release. It does **not** create a tag, publish, push, or install
anything. Run it from any directory in a local checkout:

```sh
build/release/version-bump.sh --dry-run --date 2026-11-11 0.0.2
build/release/version-bump.sh --date 2026-11-11 0.0.2
```

Replace `0.0.2` and the sample date with the intended version and release date.
The date option is optional (it defaults to today's UTC date). Versions are
strict, stable `X.Y.Z` numbers without a `v` prefix or leading zeroes; each bump
must increase the version in `CMakeLists.txt`. A new, dated `<release>` entry is
prepended to the AppStream release history. Invalid XML, a duplicate or newer
AppStream release, and malformed input abort without modifying either file.

Review and commit both `CMakeLists.txt` and
`data/io.github.the_beper.Kinefin.metainfo.xml` together. After the release
gates pass and the commit is on `main`, tag that exact commit as `vX.Y.Z`; the
release workflow checks the tag against the CMake version. Other P9 gates
(Flatpak build-image digest, runtime pins, artwork, manual checks) remain
separate and are **not** handled by this script.

Portable regression tests:

```sh
python3 -m unittest discover -s tests -p 'test_release_version_bump.py' -v
```
