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
Releases below 1.0.0 are pre-releases: they need the checks under "Before
every tag", and 1.0.0 also needs the gates under "Before 1.0.0". Do not tag
1.0.0 or submit to Flathub until those gates have evidence. A green offline
preflight does not compensate for missing host acceptance or unverified
runtime commits.

## Before every tag (local + GitHub)

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

Pre-releases use 0.x versions; choose 1.0.0 only after the gates under
"Before 1.0.0".
Version preparation changes CMake and AppStream together; never leave a
half-bumped commit:

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

## Before 1.0.0

- [ ] Implement and validate all v1 movie/TV features, including the player
  wired to the UI. Every feature described in `docs/user-guide.md` must work
  and have test evidence; edit the guide rather than ship a description of
  something that does not work yet.
- [ ] Run one manual acceptance round on a KDE Plasma Wayland desktop with the installed
  Flatpak, against a real Jellyfin 12.4 server. Prepare a series with at least
  three episodes, a film that needs a transcode, and a second Jellyfin client
  (jellyfin-web in a browser) for the remote-control and SyncPlay steps. A
  missing prerequisite is recorded as blocked, never as passed. Go in this
  order, sign-in first and the destructive keyring cases last (on Plasma and
  on GNOME):
  1. Launch: `--version`; start twice; open a `jellyfin://item/<id>` link while
     the app runs. One instance stays, the second start and the link raise it,
     and the About page shows the bundle version.
  2. Sign-in: add the server, sign in, quit, start again. No password prompt
     appears, `servers.json` holds no token and
     `~/.var/app/io.github.the_beper.Kinefin/data/keyrings/default.keyring`
     exists. After an update from 0.x every remembered user signs in once more.
  3. Browsing: Home, Movies, TV Shows, a series, an item and Search (Ctrl+F)
     with the keyboard only and then with Orca, in the light and dark theme and
     at a window width of 560 px. The Tab order follows the layout, every
     control has a spoken name and no label is clipped.
  4. Player: direct play a film and the transcoded film; switch audio and
     subtitles; seek on the trickplay preview; pause; stop. Playback Info names
     the method, the stop updates Continue Watching in jellyfin-web and the
     transcode job ends on the server.
  5. The player on real hardware: mpv's rendering, trickplay previews in
     order, true full screen under KWin, touch input and the Breeze look.
  6. Series: play episode 1 to near its end and let Up Next start episode 2,
     picking another audio or subtitle language there. The Up Next card and
     countdown appear, chapters show on the timeline and episode 3 keeps the
     choice made for the series.
  7. Mini player: go back while a film plays, use its controls, open the player
     again. Playback continues throughout.
  8. Picture-in-picture: open it, drag to move, drag an edge to resize, close it
     with KWin's close and with the X, and repeat after moving between screens.
     The window stays above others, closing returns to the player at the same
     position, Playback Info still shows hardware decoding and the focus goes
     back to the main window.
  9. Plasma integration: the media applet, lock screen controls, media keys,
     KDE Connect from a phone, and Raise from the applet; play for 15 minutes
     with the screen idle; send a command through the remote-control socket from
     a host shell. Every control acts on the player, the screen stays on only
     while playing, and the socket command works.
  10. Downloads and offline: download an episode with an external text subtitle
     and a film with chapters; pause, quit and resume a large one; switch to
     Work Offline and play both; watch one past 90 %; go online. The byte count
     resumes, offline the subtitle track and chapters are there and nothing
     reaches the server, and online the outbox marks the item watched.
  11. Export: export a completed download to a folder through the portal,
     restart, export again. The files land in `Movies/` or `Series/Season XX/`,
     and the grant survives the restart or the control says it does not.
  12. Restart while opening: start a film and kill the app
     (`flatpak kill io.github.the_beper.Kinefin`) while it opens, then start
     again. After the server's timeout no phantom session remains in its
     dashboard, and a new play works.
  13. Remote control and SyncPlay: from jellyfin-web, list sessions, pause,
     resume and stop; create a SyncPlay group with the app and the browser,
     play, seek and leave. Commands act within a second and the group stays in
     step.
  14. Playlists: create, reorder, rename and delete one; open one shared by
     another user. Edits persist in jellyfin-web and a non-owner's edit is
     refused with a message.
  15. Italian: start with `LANGUAGE=it`. Main pages, player and dialogs are in
     Italian, with no English fallback in new strings.
  16. Keyring: on Plasma and on GNOME, with the keyring locked, check that
     Kinefin asks for the unlock or for the password again instead of failing;
     on a session without a Secret portal, check that sign-in works and the
     next start asks for the password. Then sign out and run
     `--delete-profile` on a second profile. `flatpak info --show-permissions`
     lists no `kwalletd6`, and `flatpak uninstall --delete-data` removes
     `keyrings/`.

  Not in this round: other distributions, GPUs and compositors, OpenSubtitles
  (needs an OpenSubtitles.com account), Cast to a DLNA renderer and to a Google
  Cast device, and server-driven token expiry (needs an administrator). Save
  redacted logs and screenshots; never upload tokens.

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
- [ ] After the first pre-release, with a real client: install from
  `https://the-beper.github.io/Kinefin/kinefin.flatpakref` and check the
  `GPGKey` bytes of the site files against `build/flatpak/signing-key.asc`.
- [ ] After the second pre-release:
  `flatpak remote-info --log kinefin io.github.the_beper.Kinefin` shows the
  commit of the first one as the parent.
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
