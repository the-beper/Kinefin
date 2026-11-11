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

## Release metadata gate

Before creating a `vX.Y.Z` tag, run the same check as the release workflow:

```sh
python3 -I build/release/verify_release.py v0.0.2
```

Replace the example tag with the version being released. The check requires an
exact match between the tag, `project(kinefin VERSION ...)` and the **newest**
AppStream `<release>` entry, validates its date and rejects duplicate or
out-of-order release history. It does not change files or require GitHub access.
The workflow also checks that the tagged commit is on `main`. The current
`0.0.1` skeleton has no AppStream release entry and therefore is **not**
release-ready until `version-bump.sh` has prepared the first release.

Run its portable regression tests with:

```sh
python3 -m unittest discover -s tests -p 'test_release_verify.py' -v
```

## Restartable GitHub publication

The `publish` job in `.github/workflows/release.yml` runs
`build/release/publish_release.py` **only after** the CI and Flatpak artifact
verification gates pass. The job checks out the tagged source commit because
this helper is part of the repository; `GITHUB_REPOSITORY` and `GH_TOKEN` come
from GitHub Actions.

The helper requires a non-empty `Kinefin-x86_64.flatpak` and a canonical,
single-entry `SHA256SUMS` in the supplied assets directory. It checks the local
checksum before contacting GitHub, then creates a **draft** release or resumes
an existing one. Only missing or mismatched draft assets are uploaded. After
upload, both remote asset digests and sizes must match the local artifacts
before the draft is published. GitHub supplies the digest of each uploaded
asset through its release API; a missing digest fails closed.

An interrupted upload or publication can be retried by rerunning the job with
its original artifacts. A release that is already published is treated as
successful **only** when both assets still match; the helper never replaces a
published asset. Extra or duplicated release assets require manual review.

The workflow **does not** create or push tags outside the prevalidated tag
trigger. This helper does not replace the main-ancestry, build-image pin,
Flatpak smoke-test or manual release gates.

Portable tests (including a fake GitHub release lifecycle):

```sh
python3 -m unittest discover -s tests -p 'test_release_publish.py' -v
```
