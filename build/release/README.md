<!-- SPDX-FileCopyrightText: 2026 Kinefin contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Release version preparation

For the complete release-channel decision of the first release and the remaining manual release
gates, see [FIRST_RELEASE.md](FIRST_RELEASE.md). The checked-in preflight
script runs all source-only checks together without publishing:

```sh
python3 -I build/release/preflight.py
```

`build/release/version-bump.sh` prepares the two versioned source files for a
Kinefin release. It does **not** create a tag, publish, push, or install
anything. Run it from any directory in a local checkout:

```sh
build/release/version-bump.sh [--dry-run] [--date YYYY-MM-DD] X.Y.Z
```

`--date` is optional (it defaults to today's UTC date). Versions are strict,
stable `X.Y.Z` numbers without a `v` prefix or leading zeroes; each bump must
increase the version in `CMakeLists.txt`. A new, dated `<release>` entry is
prepended to the AppStream release history. Invalid XML, a duplicate or newer
AppStream release, and malformed input abort without modifying either file.

Review and commit both `CMakeLists.txt` and
`data/io.github.the_beper.Kinefin.metainfo.xml` together. After the release
gates pass and the commit is on `main`, tag that exact commit as `vX.Y.Z`; the
release workflow checks the tag against the CMake version. Other first-release
gates (Flatpak build-image digest, runtime pins, artwork, manual checks) remain
separate and are **not** handled by this script.

Portable regression tests:

```sh
python3 -m unittest discover -s tests -p 'test_release_version_bump.py' -v
```

## Release metadata gate

Before creating a `vX.Y.Z` tag, run the same check as the release workflow:

```sh
python3 -I build/release/verify_release.py vX.Y.Z
```

`vX.Y.Z` is the version being released. The check requires an
exact match between the tag, `project(kinefin VERSION ...)` and the **newest**
AppStream `<release>` entry, validates its date and rejects duplicate or
out-of-order release history. It does not change files or require GitHub access.
The workflow also checks that the tagged commit is on `main` and that the tag
is the newest `vX.Y.Z` tag (`--require-newest`, which reads `git tag -l 'v*'`
and ignores other names).

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

## Signed Flatpak repository

A release publishes the app commit to an OSTree repository and attaches a
signed bundle to the GitHub release. The channel is:

- URL: `https://the-beper.github.io/Kinefin`, served by GitHub Pages from the
  `gh-pages` branch.
- Flatpak remote: `kinefin`; Flatpak branch: `stable`.
- Site files: `kinefin.flatpakref`, `kinefin.flatpakrepo`, `icon.svg` and the
  repository under `repo/`.

Releases below 1.0.0 are pre-releases that are never "latest"; from 1.0.0 a
release is the latest. Do not link `releases/latest` anywhere: it does not
resolve while only pre-releases exist.

The commits are signed with an ed25519 key without expiry, user id "Kinefin
Flatpak repository"; the fingerprint is in the
[user guide](../../docs/user-guide.md#check-the-signature). The public part is
`build/flatpak/signing-key.asc`. The private key exists only as the secret
`FLATPAK_GPG_PRIVATE_KEY` of the environment `release-signing` and in the
maintainer's offline copy.

| Job | Does | Permissions |
| --- | --- | --- |
| `validate` | tag, CMake, AppStream, main ancestry, newest tag | contents: read |
| `ci-gate` | waits for the green CI run of the commit on main | contents, actions: read |
| `build-flatpak` | pinned image (by digest), builds on an anonymous clone of gh-pages, smoke test, exports the unsigned commits | contents, packages: read |
| `prepare-flatpak` | unsigned repository on top of gh-pages, summary, appstream, list of refs to sign; no secret | contents: read |
| `check-flatpak` | a second keyless job checks what prepare made | contents: read |
| `sign-flatpak` | the only job with the signing key: environment `release-signing` (a maintainer approves it), secret `FLATPAK_GPG_PRIVATE_KEY` | contents: read |
| `finish-flatpak` | checks the signed repository with a client (no deploy), writes the signed bundle `Kinefin-x86_64.flatpak` (public key embedded) and the site archive | contents: read |
| `check-bundle` | the only job that installs (deploys) the signed bundle, in a scratch user installation with only the key embedded in the bundle; checks that the installed commit is the listed app commit and is signed by the pinned key; passes nothing on, it only passes or fails (`check-flatpak-bundle.sh`) | contents: read |
| `attest-flatpak` | needs `check-bundle`; checks the bundle hash and attests it (Sigstore, `actions/attest`) | contents: read, id-token: write, attestations: write, artifact-metadata: write |
| `verify` | attestation, `PROVENANCE.json`, `SHA256SUMS` | contents, attestations: read |
| `publish` | the only job that writes: draft release, three assets, pushes the site to gh-pages (the old head is kept as `gh-pages-previous`), waits until Pages serves the new summary, then publishes the release; refuses when a published release with a higher `vX.Y.Z` version exists | contents: write, attestations: read |

Every signing step accepts only the app ref
`app/io.github.the_beper.Kinefin/x86_64/stable` and the appstream refs, so a
compromised build job cannot get a runtime or another app signed. The signed
commit is the smoke-tested one: signatures are detached, so signing does not
change the commit id.

The local simulation and the real-client test of these scripts are described in
[CONTRIBUTING.md](../../CONTRIBUTING.md#flatpak).

### One-time setup

A repository admin does this once, in this order, before the first tag. The
`gh api` payloads follow GitHub's REST documentation and have not been run.

1. Generate the signing key in a throwaway `GNUPGHOME`, with no expiry (an
   installed client does not refresh the key), and commit the public part as
   `build/flatpak/signing-key.asc`:

   ```sh
   export GNUPGHOME=$(mktemp -d)
   gpg --batch --pinentry-mode loopback --passphrase '' --quick-gen-key 'Kinefin Flatpak repository' ed25519 sign never
   fpr=$(gpg --list-keys --with-colons | awk -F: '$1=="fpr"{print $10; exit}')
   gpg --armor --export "$fpr" > build/flatpak/signing-key.asc
   ```

   Keep an offline copy of the private key (`gpg --armor --export-secret-keys`).
2. Create the environment `release-signing` before anything refers to it: a
   job that names a missing environment makes GitHub create it without
   protection. It needs you as its required reviewer and a deployment policy
   limited to the tag pattern `v*`:

   ```sh
   gh api -X PUT repos/the-beper/Kinefin/environments/release-signing --input - <<<"{\"prevent_self_review\":false,\"reviewers\":[{\"type\":\"User\",\"id\":$(gh api user --jq .id)}],\"deployment_branch_policy\":{\"protected_branches\":false,\"custom_branch_policies\":true}}"
   gh api -X POST repos/the-beper/Kinefin/environments/release-signing/deployment-branch-policies -f name='v*' -f type=tag
   ```
3. Store the private key as the secret `FLATPAK_GPG_PRIVATE_KEY` of that
   environment only, then destroy the temporary keyring.
   `gh secret list` must show no repository-level secret of that name:

   ```sh
   gpg --armor --export-secret-keys "$fpr" | gh secret set FLATPAK_GPG_PRIVATE_KEY --env release-signing
   gpgconf --kill all; rm -rf "$GNUPGHOME"
   ```
4. Create a ruleset for `refs/tags/v*` that blocks update, deletion and
   non-fast-forward and lets only admins create tags (a moved tag races the
   publisher's final tag check), and a ruleset for `main` that requires the
   `required` check of GitHub Actions and blocks force-push and deletion. Keep
   the default `GITHUB_TOKEN` read-only.
5. Enable Pages from `gh-pages` only after the first `publish` job has pushed
   the branch, as described in [First publication](#first-publication).

GitHub Pages sites are limited to 1 GB and pushed files to 100 MB (GitHub's
documentation; not checked here). The size of Kinefin's repository objects has
not been measured against that limit. Flathub, when it comes, is a second
remote for the same app id `io.github.the_beper.Kinefin`, and moving users
from one remote to the other is a separate decision.

### First publication

The prerequisites are listed in [FIRST_RELEASE.md](FIRST_RELEASE.md). Pages is
not enabled before the first publication, because there is no `gh-pages` branch
yet. The first `publish` job pushes it, its Pages wait fails and the release
stays a draft. Then enable Pages from `gh-pages` (root) and re-run the failed
job:

```sh
gh api -X POST repos/the-beper/Kinefin/pages -f 'source[branch]=gh-pages' -f 'source[path]=/'
```

Never create `gh-pages` by hand: a branch without a signed repository makes
`prepare-flatpak` fail. The checks to run with a real client after the first
two releases are in [FIRST_RELEASE.md](FIRST_RELEASE.md).

### Maintainer procedures

Short cases:

| Situation | Action |
| --- | --- |
| Approve a release | Approve the `sign-flatpak` deployment of `release-signing` in the run |
| Interrupted publish | "Re-run failed jobs": every step resumes (draft, assets, gh-pages push, Pages wait) |
| Exit 3 in prepare, check, finish or publish | gh-pages moved since the build: "Re-run all jobs". This relies on GitHub replacing the artifacts of a re-run job's previous attempt; that has not been tested |
| Three tags pushed quickly | The newest-tag rule (the tag must be the newest `vX.Y.Z` tag) refuses the older ones: cut the next version |
| Old run re-run after a newer release | `publish` refuses (a published release with a higher version exists), so an old run cannot downgrade clients: cut the next version |
| Lost key | No automated path |
| New build image | After reviewing the Images run on main: `gh variable set KINEFIN_RELEASE_FLATPAK_DIGEST --body sha256:<digest>` |

Bad release. Clients that already updated keep the bad commit, so the fix is a
new version:

1. `git fetch origin gh-pages-previous`
2. `git push --force origin origin/gh-pages-previous:gh-pages`
3. `gh release edit vX.Y.Z --draft=true`
4. Fix forward and cut the next version.

Never re-run the bad run's publish job afterwards: it would push the bad site
again.

The `gh-pages` branch deleted. `prepare-flatpak` refuses to run. Either:

1. Take the commit from the `gh-pages is now <commit>` log line.
2. `git fetch origin <commit>`
3. `git push --force origin <commit>:gh-pages`

or re-run the publish job of the newest release's run only, while its
artifacts exist (14 days).

Leaked key:

1. Delete the environment secret at once.
2. Make a new key and re-sign gh-pages.
3. Update the `GPGKey` of both site files and `signing-key.asc` in one change.
4. Every installed client then runs
   `flatpak remote-modify --gpg-import=<new key> kinefin`.

## Restartable GitHub publication

The `publish` job in `.github/workflows/release.yml` runs
`build/release/publish_release.py` **only after** the CI and Flatpak artifact
verification gates pass. The job checks out the tagged source commit because
this helper is part of the repository; `GITHUB_REPOSITORY` and `GH_TOKEN` come
from GitHub Actions.

The release contains three assets: `Kinefin-x86_64.flatpak` (signed),
`PROVENANCE.json` and `SHA256SUMS`. The build job provides the **actual**
image digest it pulled; `finish-flatpak` provides the SHA-256 of the bundle it
built. The downstream `verify` job compares the downloaded bundle with that
SHA-256 before writing
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
signed attestation**. Separately, the `attest-flatpak` job signs a SLSA
provenance attestation for the exact Flatpak bytes with GitHub Actions' OIDC
identity and stores it in GitHub's attestation service. Both `verify` and `publish` check
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

The site comes from the workflow through `SITE_TAR`, `EXPECT_SITE_SHA256`,
`SITE_BASE_SHA` and `SITE_URL`; `REPO_REMOTE` is optional. After the assets
match, the publisher pushes the site to `gh-pages` (the old
head stays as `gh-pages-previous`; a push is skipped when the tree is
unchanged, and a moved base exits with 3), waits until Pages serves the new
summary, and only then publishes the release: as a pre-release, never "latest",
for a 0.x version, as the latest from 1.0.0. The draft lookup goes through the
release list, because the by-tag endpoint does not show drafts.

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

The release workflow runs this same offline audit with its independently
known build inputs before uploading the three assets. Offline tests:

```sh
python3 -m unittest discover -s tests -p 'test_release_assets_audit.py' -v
```

## Verify the GitHub-signed Flatpak attestation

The release **attest-flatpak** job uses the SHA-pinned `actions/attest` action to
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
  --manifest dist/flathub/vX.Y.Z/io.github.the_beper.Kinefin.yml \
  --flathub-tag vX.Y.Z --flathub-commit "$(git rev-parse HEAD)"
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
python3 -I build/release/prepare_flathub.py vX.Y.Z
# or inspect the transformed YAML without writing a file:
python3 -I build/release/prepare_flathub.py --dry-run vX.Y.Z
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
  --manifest dist/flathub/vX.Y.Z/io.github.the_beper.Kinefin.yml
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

Kinefin's final icon is maintained as a 48px-grid scalable SVG source,
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
