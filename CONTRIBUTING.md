# Contributing to Kinefin

A scaffold-forward project of one developer. These are the real rules; the layout table is the map.

## Repository layout

| Path | Content |
| --- | --- |
| `build/Containerfile` | The build/check image (Fedora 45, Qt 6/KF6/ECM, mpv-devel, gettext) |
| `build/build-env.sh` | Runs any command in that image: the only supported way to build and check locally |
| `build/check.sh` | Every automated check (build, ctest, qmllint, metainfo, desktop file, catalogs): the one list |
| `build/flatpak/` | The Flatpak image (`Containerfile`), the manifest (`io.github.the_beper.Kinefin.yml`), and its build/test scripts |
| `build/release/` | Release tooling (empty until the version-bump script is written) |
| `data/` | Desktop file, AppStream metainfo and icons (`icons/hicolor`: the scalable SVG source and the 128/256 px PNG renders) |
| `po/` | Gettext: `LINGUAS`, `POTFILES.in`, `extract.sh`, per-language catalogs |
| `src/api/` | Jellyfin REST client (auth, discovery, device profile, media sources, reporting) |
| `src/downloads/` | Not started: download queue, tracked-series auto rules, portal export (feature doc: parity §10) |
| `src/app/` | `ConnectionController`: the one object the QML pages talk to (servers, sign-in, Quick Connect, session restore); profile commands and logging; `ClientSettingsQml.h`, which exposes the settings object to QML |
| `src/player/` | Playback session state machine, server reporting (`PlaybackReporter`) and the mpv engine (`PlaybackEngine`); property mapping and subtitles come later |
| `src/integration/` | Safe `jellyfin://` link parser; MPRIS, screensaver/power, Wayland/libkscreen display switching, SDL3/optional CEC input |
| `src/settings/` | Profile index and identity, server credentials, KConfig client settings (`SettingDefinitions`, `ClientSettings`, `SettingsSectionModel`) |
| `src/models/` | list models without QtQml or QtGui: `ItemSummary`, the windowing base `PagedItemsModel` with `LibraryItemsModel` and `SeasonEpisodesModel`, `ItemRowModel`, `HomeRowsModel`, the item details pair `ItemDetails` (parser) and `ItemDetailsModel`, and `ItemChildrenModel` (seasons, Next Up, episodes, collection children) |
| `src/imagecache/` | Image origin lock and the restricted network access (allowed image URLs only, disk cache with a cap); the subtitle cache is not started |
| `src/qml/` | All Kirigami pages and their components, built as the static QML module `kinefin_ui` (linked by the `kinefin` executable and by `test_ui_pages`) |
| `tests/`, `scripts/testing/` | Unit tests, test scripts, and `tests/fixtures/` (the realistic server-response corpus the P2 fake server replays; its README rules shape changes) |
| `scripts/po.py` | Status, listing and writing of the gettext catalogs (`po/*.po`) |

## Building

The build image exists because a bare host `cmake` misses ECM and friends; Fedora's own packages don't give a reproducible environment. Inside, `QT_QPA_PLATFORM=offscreen` is set for you.

```sh
build/build-env.sh cmake -S . -B "${KINEFIN_BUILD_DIR:-/buildtmp/check}" -G Ninja -DBUILD_TESTING=ON
build/build-env.sh cmake --build "${KINEFIN_BUILD_DIR:-/buildtmp/check}"
build/build-env.sh ctest --test-dir "${KINEFIN_BUILD_DIR:-/buildtmp/check}" --output-on-failure
```

The build tree defaults to `/buildtmp/check` (a named podman volume build-env.sh mounts — incremental between runs). Keep CMake build trees outside the checkout: the `dist/` tree is scanned by the REUSE gate, which would otherwise have to cover the fetched third-party mpvqt sources. `dist/` is not in git. To run the built app in the container you are on your own — playback needs a real session; do that on the host once it can be host-built, or inside the container for offscreen-only experiments.

mpvqt 1.2.1 is fetched by the root `CMakeLists.txt` (`FetchContent`, SHA256 pinned); the Flatpak manifest pins the same tarball and points `KINEFIN_MPVQT_URL` at it, so the two pins must move together.

After changing `build/Containerfile`, rebuild the local image before checking:

```sh
podman build -t localhost/kinefin-build:fedora45 -f build/Containerfile build
```

## Checks

Run the whole suite:

```sh
build/build-env.sh build/check.sh
python3 scripts/po.py status        # 0 pending per language
```

`tests/shell_smoke.py` starts the binary offscreen three times (Servers, then Settings and About through the `KINEFIN_START_PAGE` start-up hook) and fails on any QML problem. `tests/test_kwallet_host.cpp` runs against the session's real KWallet and is skipped unless `KINEFIN_TEST_KWALLET=1`; the command is in the plan's "KWallet host checks". Run it before proposing a commit. Shell scripts (`build/`, `scripts/`, `po/`) are shellcheck-clean (`--severity=warning`); C++ code follows the Qt-like style of `.clang-format` (LLVM base with explicit knobs: the image's clang-format has no `Qt` preset) (`clang-format -i <files>` before proposing — the image gate fails on drift; `.editorconfig` governs the non-C++ text files).

## Flatpak

```sh
build/flatpak/build-flatpak.sh                 # dist/Kinefin-x86_64.flatpak
KINEFIN_EXPECT_VERSION=$(…) build/flatpak/test-flatpak.sh dist/Kinefin-x86_64.flatpak
```

The build script installs the org.kde runtime at pinned commits (see the constants inside) and refuses to run outside rootless podman. `test-flatpak.sh` installs the bundle into a throwaway container `HOME`, checks the version, that the app owns its D-Bus name, that the profile data lands under the redirected XDG, and that a second launch hands over — then kills the app.

## Translations

Strings are `i18n()` calls (QML: plain `i18n()` injected with `KLocalization::setupLocalizedContext()` from `KF6::I18nQml`, requiring KF6 >= 6.8). When a file gains its first translatable string, add it to `po/POTFILES.in` in the same change (the extraction script fails otherwise), then regenerate `po/kinefin.pot` with `build/build-env.sh po/extract.sh` and fill `it.po` (`python3 scripts/po.py todo` lists the pending entries). `en.po` repeats the msgid. Catalogs are compiled by the CMake install (ECM PO); `po/extract.sh --check` must verify real source extraction, and `po.py status --strict` must report zero pending per language or the check fails.

`po/kinefin.pot` is generated: never edit it by hand.

## Releasing

1. Bump the version with `build/release/version-bump.sh X.Y.0 NOTES` (single sources: root `CMakeLists.txt` and one metainfo `<release>` entry). Until that script exists, the bump is hand-written and both places must move together.
2. Run `build/build-env.sh build/check.sh` on the commit and tag it `vX.Y.0`.

Before the first release:

- re-pin the org.kde Platform/Sdk commits in `build/flatpak/build-flatpak.sh` (the pins date from 2026-11-07 and were copied over as working values);
- decide self-hosting: Flathub avoids it (submissions need `type: git` sources in the manifest — the switch is marked in `io.github.the_beper.Kinefin.yml`); a signed OSTree repository needs KuickShare's pipeline ported; a bundle attached to a GitHub release needs nothing.

## How to work here

- Follow the [KDE HIG](https://develop.kde.org/hig/); deliberate exceptions are recorded in the project notes in the same change.
- Anything that touches behaviour against Jellyfin Desktop updates the parity notes in the same change — including a setting dropped or renamed (it gets a row saying so; silent omissions are bugs).
- Every new source file starts with its SPDX header (the KDE REUSE convention): `SPDX-FileCopyrightText: 2026 <author>` + `SPDX-License-Identifier: GPL-3.0-or-later`; the metainfo metadata keeps CC0 and the docs stay plain for now — `REUSE.toml` covers the snapshot, `LICENSES/` provides license texts, and `reuse lint` is mandatory in the build image. Verify exceptions and file-level headers when adding third-party files.
- Keep shell scripts POSIX sh (the build runs plain `/bin/sh` inside Fedora images) and oksh/fish out of CI.
- Treat remote metadata (item names, cast text, images) as untrusted input everywhere; see the architecture's "Remote data" rule. Never log or leak the access token.

## Validation scope of this snapshot

P0 portable checks (including a real C++20 state-machine compile) run with `scripts/testing/run_portable_checks.sh`; the native Qt/KF6 tests require the build container. A fake Jellyfin server can be exercised with `python3 scripts/testing/probe_jellyfin.py --fixture`; live-server read-only API probes require explicitly supplied environment variables (see the script). Playback and session reporting still require dedicated host tests.
