<!-- SPDX-FileCopyrightText: 2026 Kinefin contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Release version preparation

For the complete P9 release-channel decision and the remaining manual release
gates, see [FIRST_RELEASE.md](FIRST_RELEASE.md). The checked-in preflight
script runs all source-only checks together without publishing:

```sh
python3 -I build/release/preflight.py
```

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

The release now contains three assets: `Kinefin-x86_64.flatpak`,
`PROVENANCE.json` and `SHA256SUMS`. The build job provides the **actual**
image digest it pulled and the bundle's SHA-256 as job outputs. The downstream
`verify` job compares the downloaded bundle with that SHA-256 before writing
a deterministic `PROVENANCE.json`. This record names the release tag, exact
source commit, GitHub repository and Actions run ID, pinned GHCR image digest,
and the resulting bundle's SHA-256 and byte size. `SHA256SUMS` includes the
Flatpak and the provenance file, in that order.

The publisher validates both local checksums and the provenance schema against
`RELEASE_COMMIT`, `GITHUB_RUN_ID` and the digest output of the original build
job (`RELEASE_IMAGE_DIGEST`) **before** contacting GitHub. It then creates a
**draft** release or resumes an existing one, uploading only missing or
mismatched draft assets. All three remote asset SHA-256 digests and sizes must
match before publication. A missing remote digest fails closed.

`PROVENANCE.json` is **self-reported release metadata, not a cryptographically
signed attestation**. Separately, the build job signs a SLSA provenance
attestation for the exact Flatpak bytes with GitHub Actions' OIDC identity and
stores it in GitHub's attestation service. Both `verify` and `publish` check
that signature and the trusted repository, tag, source commit and signer
workflow. A missing or invalid attestation blocks publication. This does
not substitute for CI branch protection or an independent reproducible build.

Publication also verifies the **current remote Git tag** resolves to the
exact `RELEASE_COMMIT` from the workflow (`github.sha`). This supports both
lightweight and annotated tags. It checks before creating a draft and again
after uploading, immediately before publishing. A tag that has been moved,
deleted, or points to a different object **blocks publication**; a draft
remains unpublished on failure. GitHub release `target_commitish` is not
reliable evidence of a pre-existing tag's target and is not used. Protect
release tags against force-pushes: no sequence of API queries can prevent a
tag from changing in the instant after the final check.

An interrupted upload or publication can be retried by rerunning the job with
its original artifacts. A release that is already published is treated as
successful **only** when all three assets still match; the helper never replaces a
published asset. Extra or duplicated release assets require manual review.

The workflow **does not** create or push tags outside the prevalidated tag
trigger. This helper does not replace the main-ancestry, build-image pin,
Flatpak smoke-test or manual release gates.

Portable tests (including a fake GitHub release lifecycle):

```sh
python3 -m unittest discover -s tests -p 'test_release_publish.py' -v
python3 -m unittest discover -s tests -p 'test_release_provenance.py' -v
```

## Verify downloaded release assets offline

The published Flatpak release contains three files: `Kinefin-x86_64.flatpak`,
`SHA256SUMS` and `PROVENANCE.json`. To check a downloaded set **without GitHub
credentials or network access**, use a trusted copy of this repository's
verifier and supply the expected full Git commit SHA independently (not taken
from the downloaded `PROVENANCE.json`):

```sh
python3 -I build/release/verify_release_assets.py v1.2.3 /path/to/downloads \
  --commit <trusted-40-character-git-commit> \
  --expected-sha256 <independently-trusted-flatpak-sha256>
```

The optional `--expected-sha256` provides an external reference for the bundle
hash. If supplied, it must be obtained from a trusted, independent channel.
`--run-id` and `--image-digest` can also be provided to compare against known
workflow values; otherwise they are checked only as claims in the manifest.
The repository defaults to `the-beper/Kinefin`; pass `--repository owner/name`
when auditing a different publisher. The tool rejects missing, symlinked,
changed or inconsistent assets, malformed JSON, incorrect checksums and a
mismatched tag, repository or commit. It only reads the release files.

**This is an integrity check, not authentication.** Both `SHA256SUMS` and
`PROVENANCE.json` are **unsigned**. Anyone able to replace the release files
could rewrite all of them consistently. A matching commit identifier in the
manifest is not cryptographic evidence that the commit built the Flatpak.
Compare the bundle's SHA-256 against an independently trusted checksum and
verify the origin of the release before installation. The check does not inspect
Flatpak contents or perform reproducible-build verification.

The release workflow now runs this same offline audit with its independently
known build inputs before uploading the three assets. Offline tests:

```sh
python3 -m unittest discover -s tests -p 'test_release_assets_audit.py' -v
```

## Verify the GitHub-signed Flatpak attestation

The release **build-flatpak** job uses the SHA-pinned `actions/attest` action to
create a Sigstore-signed SLSA provenance attestation for the Flatpak bundle.
GitHub stores this attestation **separately** from the three files on the
release page; it is not included in `SHA256SUMS`. Both **verify** and **publish**
require successful cryptographic verification before uploading or publishing.
On retries, the publisher checks the attestation again.

To verify a downloaded release using the trusted Kinefin source commit and tag,
install GitHub CLI (`gh`) and use a network connection:

```sh
python3 -I build/release/verify_attestation.py \
  /path/to/Kinefin-x86_64.flatpak \
  --repository the-beper/Kinefin \
  --commit <trusted-40-character-git-commit> \
  --tag v1.2.3
```

The command checks the signature, bundle digest, repository identity, **only**
`.github/workflows/release.yml` as signer, the full source commit and the
exact `refs/tags/vX.Y.Z` source ref. Obtain the commit/tag from a trusted source,
not solely from the unsigned downloaded `PROVENANCE.json`. The verifier requires
GitHub attestation API access (`GH_TOKEN` may be needed for private repos) and
will fail if `gh` is missing, GitHub is unreachable, or a signed attestation is
missing or inconsistent. The existing `verify_release_assets.py` remains a
separate offline integrity check; it **does not** verify signatures.

A valid signed workflow attestation proves the signed build's recorded GitHub
identity and the artifact's SHA-256, **not** that the workflow was uncompromised,
that all dependencies were independently reproduced, or that the GitHub token
and branch/tag protection are correctly configured. The attestation action and
CLI must also be available to test the complete release on GitHub Actions.

Offline regression tests (the `gh` call is replaced with a fake executable):

```sh
python3 -m unittest discover -s tests -p 'test_release_attestation.py' -v
```

## Audit immutable Flatpak source pins

Before submitting a release, validate **all** downloadable `sources:` entries in
`build/flatpak/io.github.the_beper.Kinefin.yml` and the generated Flathub
manifest. Git sources must include an immutable full commit ID; downloaded
archives and files must have a nonzero SHA-256 checksum and use HTTPS.
Mutable branches, missing hashes, additional local directory sources and
unreviewed YAML source layouts are rejected. The single `type: dir` source for
Kinefin's checkout is allowed **only** in the local/CI manifest.

```sh
python3 -I build/release/verify_flatpak_sources.py
python3 -I build/release/verify_flatpak_sources.py \
  --manifest dist/flathub/v0.0.2/io.github.the_beper.Kinefin.yml \
  --flathub-tag v0.0.2 --flathub-commit "$(git rev-parse HEAD)"
python3 -m unittest discover -s tests -p 'test_flatpak_sources.py' -v
```

CI runs the local-source audit, while the release validation job checks both
the local source tree and the generated Flathub artifact before uploading it.
This is an **offline source pinning audit**, not a network or archive-content
verification. The Flatpak builder still downloads and verifies the referenced
commits/checksums. Any new source format must be reviewed and explicitly
supported by the audit, rather than silently skipped.

## Keep KDE runtime pins consistent

The Flatpak builder and the offscreen smoke test both load the **same**
`build/flatpak/runtime-pins.conf` file. It contains the KDE SDK/Platform refs
and their immutable OSTree SHA-256 commits; the smoke container receives this
file as a **read-only mount**. Update the pins only after checking the matching
refs and commits on Flathub, and review the change to the four assignments.

The offline gate prevents an accidental mismatch with the manifest's
`runtime`, `sdk` or `runtime-version` fields and refuses malformed commits,
duplicate assignments or scripts that stop sourcing the shared pins. It runs
in both CI and release validation, before attempting a Flatpak build:

```sh
python3 -I build/release/verify_runtime_pins.py
python3 -m unittest discover -s tests -p 'test_runtime_pins.py' -v
```

This check does **not** fetch Flathub metadata or prove the pinned commits are
still downloadable. The Flatpak build still installs and verifies each pinned
commit before bundling; the smoke test checks the same commits before and after
running the app. Re-pin and run a real Flatpak build for the first release.

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

## Audit Flatpak sandbox permissions

The release manifest uses an explicitly reviewed set of sandbox grants. Run the
fail-closed, **offline** audit before building or submitting a Flathub manifest:

```sh
python3 -I build/release/verify_flatpak_permissions.py
python3 -I build/release/verify_flatpak_permissions.py \
  --manifest dist/flathub/v0.0.2/io.github.the_beper.Kinefin.yml
python3 -m unittest discover -s tests -p 'test_flatpak_sandbox_permissions.py' -v
```

The validator checks the app ID, KDE runtime and SDK, launch command and all
`finish-args`. It rejects duplicate or unreviewed grants rather than accepting
new filesystem, broad D-Bus, X11 or host-device access silently. **Intended**
permission changes need a security review and an update to the allowlist in
`verify_flatpak_permissions.py`; the gate is not a substitute for that review.

The Flatpak smoke test also runs the verifier against the **installed bundle's**
`flatpak info --show-metadata` output, before starting the app. This detects a
packaging result that differs from the reviewed manifest, including new host
filesystem grants, bus names and runtime drift. A real Flatpak build is still
required to exercise this test; the offline tests use representative keyfiles.

The smoke-test image now includes Python 3. After merging this change, verify
that the **Images** workflow rebuilt `kinefin-flatpak:fedora45`, and set the
release digest variable to the new approved image digest before tagging a
release. Reusing the old image will fail the smoke test because Python is
missing. No auto-update or publication is performed by this gate.

## Verify immutable GitHub Actions dependencies

Before merging workflow edits or preparing a release, run:

```sh
python3 -I build/release/verify_action_pins.py
python3 -m unittest discover -s tests -p 'test_action_pins.py' -v
```

The CI lint job and release validation job both reject external `uses:` entries
that point to mutable tags, branches, short SHAs or expressions. Ordinary GitHub
Actions must use a full, nonzero, lowercase 40-character Git commit hash.
`docker://` actions must use an explicit nonzero SHA-256 image digest. Local
composite actions and reusable workflows must resolve to files inside
`.github/`. The audit also checks composite action definitions under
`.github/actions/` and rejects YAML aliases and ambiguous `uses:` syntax.

This offline audit is a guard against accidental dependency drift, **not**
a signature or provenance verification. `actionlint` still validates workflow
syntax separately. When changing an action, review the upstream release and
resolve its published tag to an actual commit before updating the pinned SHA.

## Release artwork and application identity

Kinefin's final P9 icon is maintained as a 48px-grid scalable SVG source,
with committed 128px and 256px PNG exports for Flatpak/AppStream. Re-export
both rasters after any SVG edit (Inkscape command-line example):

```sh
for size in 128 256; do
  inkscape data/icons/hicolor/scalable/apps/io.github.the_beper.Kinefin.svg \
    --export-area-page --export-width="$size" --export-height="$size" \
    --export-filename="data/icons/hicolor/${size}x${size}/apps/io.github.the_beper.Kinefin.png"
done
python3 -I build/release/verify_release_branding.py
```

The branding gate runs during CI lint and release validation. It verifies the
source SVG, both PNG pixel streams, file dimensions and alpha margins, desktop
protocol/identity and AppStream identity. The installed Flatpak smoke test also
requires both raster icons and the absence of the scalable SVG (removed in the
Flatpak manifest for compatibility with AppStream composition). This is not a
substitute for a human visual review of the artwork on KDE Plasma.

```sh
python3 -m unittest discover -s tests -p 'test_release_branding.py' -v
python3 -m unittest discover -s tests -p 'test_release_preflight.py' -v
```
