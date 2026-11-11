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


## Pin the release build image

Before a tag build, open the successful **Images** workflow run on `main` in
GitHub Actions. Its **Digest** step prints a line resembling
`ghcr.io/the-beper/kinefin-flatpak:fedora45@sha256:<64 hex digits>`. Review
that build, then set the GitHub Actions **repository variable**
`KINEFIN_RELEASE_FLATPAK_DIGEST` to the `sha256:<64 lowercase hex digits>`
portion, without the image name or tag. This is a non-secret repository
variable, not an Actions secret. An absent, malformed, or uppercase value
blocks the release build before any network or Podman operation.

`release.yml` pulls `ghcr.io/the-beper/kinefin-flatpak@sha256:...` and gives
the verified image the local name expected by `build-flatpak.sh`. It never
pulls `:fedora45` during a release. CI may continue testing the moving tag,
but that tag cannot silently change the image used by an individual release
build. The actual digest is recorded in the job summary for review. Keep the
repository variable unchanged while retrying a release to reproduce its
build environment; updating it for the next release requires maintainer
review. **The digest cannot be inferred from this repository** because the
GHCR image is private; it must come from the trusted Images workflow.

Portable tests, using a fake `podman` command rather than a network pull:

```sh
python3 -m unittest discover -s tests -p 'test_release_image_pin.py' -v
```

This guard is independent of the KDE runtime commit pins in
`build/flatpak/build-flatpak.sh`; those still need their own release review.

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

## Prepare a Flathub submission manifest

The manifest in `build/flatpak/` uses a local directory source so CI and local
builds compile exactly the checked-out tree. Do not change that source to a
remote Git reference in the checked-in manifest. Once a stable release tag
exists and its commit is checked out, generate the Flathub variant instead:

```sh
python3 -I build/release/prepare_flathub.py v0.0.2
# or inspect the transformed YAML without writing a file:
python3 -I build/release/prepare_flathub.py --dry-run v0.0.2
```

The helper requires a **clean checkout at the exact tag commit**, matching
CMake and AppStream release metadata. It replaces the application's `type: dir`
source with a public `type: git` source containing both `tag: vX.Y.Z` and the
full commit ID. All other dependencies and their hashes remain unchanged. The
result lives under `dist/flathub/vX.Y.Z/io.github.the_beper.Kinefin.yml` and
is never submitted or published automatically. It refuses to overwrite a
previously generated file if its contents differ.

A successful release **validate** job also uploads the same candidate YAML as
a separate `flathub-manifest` Actions artifact for review. This is not a
Flathub submission or a replacement for Flathub's build and review gates.
Before submitting, review the app metadata, runtime pins, application icon,
permissions, and the completed first-release acceptance checks. The CLI needs
no GitHub credentials, network access, Flatpak or Qt; Git and Python 3 suffice.

```sh
python3 -m unittest discover -s tests -p 'test_flathub_manifest.py' -v
```
