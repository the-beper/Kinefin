<!-- SPDX-FileCopyrightText: 2026 Kinefin contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# First release: final acceptance checklist

The release channel, the job table and the recovery procedures are in the
[release tooling README](README.md#signed-flatpak-repository). Flathub follows
after 1.0.0. The checked-in Flatpak manifest retains its local `type: dir`
source for CI; the Flathub candidate is generated from the exact tag and
commit by `prepare_flathub.py`.

Check these prerequisites before the first tag:

- [ ] The signing key exists and its public part is committed as
  `build/flatpak/signing-key.asc`.
- [ ] The environment `release-signing` has a required reviewer and a
  deployment tag policy `v*`.
- [ ] The secret `FLATPAK_GPG_PRIVATE_KEY` exists only in that environment.
- [ ] The tag ruleset and the `main` ruleset are active.
- [ ] The approved image digest is set (see below).
- [ ] The repository is public.

This file is an **operational checklist**, not a claim of release verification.
Do not tag a stable release or submit to Flathub until the live gates below
have evidence. In particular, a green offline preflight does not compensate
for unfinished native playback, missing host acceptance, or unverified runtime
commits.

## Before tagging (local + GitHub)

- [ ] Implement and validate all v1 movie/TV features, including the player
  wired to the UI. Every feature described in `docs/user-guide.md` must work
  and have test evidence; edit the guide rather than ship a description of
  something that does not work yet.
- [ ] Perform the real Jellyfin 12.4 and KDE Plasma Wayland host acceptance
  rounds, including KWallet, resume, remote control, playback reporting,
  subtitles, online/offline transitions and actual screen-reader/keyboard
  behavior. Save redacted logs and screenshots; never upload tokens.
- [ ] Review the final icon at small and large sizes and confirm that the
  AppStream/desktop description matches what actually works. The source icon
  is `data/icons/hicolor/scalable/apps/io.github.the_beper.Kinefin.svg`;
  128px/256px PNGs are committed, not generated at build time.
- [ ] Recheck the exact `org.kde.Sdk` and `org.kde.Platform` 6.11 commits on
  Flathub and run an actual Flatpak build with those pins. The offline runtime
  gate only checks consistency, not remote availability.
- [ ] Ensure the **Images** workflow succeeded on `main` after the latest
  changes to `build/flatpak/Containerfile`; review the image digest and set
  `KINEFIN_RELEASE_FLATPAK_DIGEST=sha256:<64 lowercase hex characters>` in
  GitHub Actions repository variables. The release never falls back to a tag.
- [ ] Ensure CI succeeded for the exact commit on `main`, including
  `build/check.sh` and the Flatpak build and smoke test. Review its logs,
  especially desktop/AppStream validation and the icon-install smoke gate.

The source-only checks can be run *before* a tag exists:

```sh
python3 -I build/release/preflight.py
```

Choose a stable version only after acceptance. Version preparation changes
CMake and AppStream together; never leave a half-bumped commit:

```sh
build/release/version-bump.sh [--dry-run] [--date YYYY-MM-DD] X.Y.Z
# Run it with --dry-run first, review the two changed files, commit, and
# get CI green on main.
```

When that commit is clean, tagged and checked out, run the stricter checks:

```sh
# Set KINEFIN_RELEASE_FLATPAK_DIGEST to the reviewed sha256: digest first.
test -n "${KINEFIN_RELEASE_FLATPAK_DIGEST:-}" || { echo "Missing approved digest" >&2; exit 1; }
python3 -I build/release/preflight.py --tag vX.Y.Z --require-image-digest
```

Both commands are **read-only**; the tagged preflight dry-runs the generated
Flathub manifest and fails if the tag does not point to the current HEAD, the
working tree is dirty, or the release metadata is inconsistent.

## Release and installation review

- [ ] Create/push a protected `vX.Y.Z` tag pointing at the accepted `main`
  commit. Do not move an existing release tag.
- [ ] Confirm `release.yml` is **green from that real tag**: immutable image
  pull, build, installed permission/icon checks, smoke test with
  `KINEFIN_EXPECT_VERSION=X.Y.Z`, the approved `sign-flatpak` deployment,
  the `check-bundle` install check, signed attestation, provenance and
  checksums, GitHub draft publication and final tag recheck.
- [ ] Enable Pages after the first publication, as described in
  [First publication](README.md#first-publication).
- [ ] After v0.1.0, with a real client: install from
  `https://the-beper.github.io/Kinefin/kinefin.flatpakref` and check the
  `GPGKey` bytes of the site files against `build/flatpak/signing-key.asc`.
- [ ] After v0.2.0: `flatpak remote-info --log kinefin io.github.the_beper.Kinefin`
  shows the commit of v0.1.0 as the parent.
- [ ] Download all three public assets (`Kinefin-x86_64.flatpak`,
  `PROVENANCE.json`, `SHA256SUMS`) on a separate machine. Verify the source
  commit and independent digest with `verify_release_assets.py`, and run
  `verify_attestation.py` to authenticate the build signer. The bundle must
  also install on its own, without the remote.
- [ ] Install and launch the downloaded Flatpak on a KDE Plasma Wayland host.
  Confirm icon, desktop protocol, localization, first-run login, library
  browsing, video playback and quit/reopen; preserve anonymized evidence.

## Flathub submission (separate manual approval)

- [ ] Retrieve the generated `flathub-manifest` artifact from the validated
  release run; inspect the exact `type: git` tag/commit and upstream source
  digests against the released source.
- [ ] Provide **real** application screenshots, final AppStream content and
  verified permissions. Do not submit mockups as application screenshots.
- [ ] Validate/build/install the generated manifest with Flatpak Builder in a
  clean environment, resolve review findings, and submit it through Flathub's
  normal review flow. The release workflow does **not** publish to Flathub.

The icon, source audits and publication tooling are automated. All checkboxes
above require actual evidence and a maintainer decision; none are automatically
marked completed by this repository or by a passing unit test.
