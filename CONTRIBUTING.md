# Contributing to Kinefin

This page covers building, checking, packaging and the working rules. The [architecture overview](docs/architecture.md) maps the source code.

## Repository layout

| Path | Content |
| --- | --- |
| `build/Containerfile` | The build/check image (Fedora 45, Qt 6/KF6/ECM, mpv-devel, gettext) |
| `build/build-env.sh` | Runs any command in that image: the only supported way to build and check locally |
| `build/check.sh` | Every automated check (clang-format, cppcheck, build, ctest, qmllint, metainfo, desktop file, translation consistency, Python contract tests, release simulation, `reuse lint`, catalogs): the one list |
| `build/desktop-check.sh` | Reads a running Plasma session and reports on MPRIS, the screen and suspend inhibitions, KWallet and KRunner (`--act` also controls the player; run it with Kinefin open and a video playing) |
| `build/screenshots.sh` | Renders the screenshots of `docs/images`, light, and `docs/images/dark` (README and user guide) from a library of the CC BY open films of Blender (`tests/DocScene.h`, artwork in `tests/assets/doc`), in a build-image layer with the Breeze Qt style and a virtual X server (`build/screenshots/Containerfile`); run it after a change of the look and commit the new PNGs |
| `build/flatpak/` | The Flatpak image (`Containerfile`), the manifest (`io.github.the_beper.Kinefin.yml`), and its build/test scripts |
| `build/release/` | Version bump, Flatpak source/runtime/permission/Action gates, Flathub manifest generator, artifact verification, attestation checks, release publisher and first-release checklist |
| `data/` | Desktop file, AppStream metainfo and icons (`icons/hicolor`: the scalable SVG source and the 128/256 px PNG renders) |
| `po/` | Gettext: `LINGUAS`, `POTFILES.in`, `extract.sh`, per-language catalogs |
| `docs/` | The user guide, the settings reference and the architecture overview |
| `src/`, `tests/` | The application and its tests, mapped in the [architecture overview](docs/architecture.md); `tests/fixtures/` is the server-response corpus the fake server replays |
| `third_party/` | The vendored mpvqt renderer, KWin's picture-in-picture protocol and the Google Cast root certificates; [THIRD_PARTY.md](THIRD_PARTY.md) lists them with every other component and its license |
| `scripts/testing/` | The portable checks and the read-only API probe |
| `scripts/po.py` | Status, listing and writing of the gettext catalogs (`po/*.po`) |

## Building

The build image exists because a bare host `cmake` misses ECM and friends; Fedora's own packages don't give a reproducible environment. Inside, `QT_QPA_PLATFORM=offscreen` is set for you.

```sh
build/build-env.sh cmake -S . -B "${KINEFIN_BUILD_DIR:-/buildtmp/check}" -G Ninja -DBUILD_TESTING=ON
build/build-env.sh cmake --build "${KINEFIN_BUILD_DIR:-/buildtmp/check}"
build/build-env.sh ctest --test-dir "${KINEFIN_BUILD_DIR:-/buildtmp/check}" --output-on-failure -j 4
```

The UI tests (`test_ui_pages`) run as the shards `ui_pages_0` to `ui_pages_5`, which CMake builds from the test class, so they run side by side under `-j` (each counts as two processors, so `-j4` runs two at a time); `ui_pages_shards` fails when a test function is in no shard. `KINEFIN_TEST_TIMEOUT_SCALE` (integer 1 to 10, default 1) multiplies every wait of `test_ui_pages`; CI sets it to 3. One UI test alone: `build/build-env.sh /buildtmp/check/bin/test_ui_pages <function>`. Screenshots for a UI review: `KINEFIN_SHOTS=<directory inside the checkout> build/build-env.sh /buildtmp/check/bin/test_ui_pages visualTourWhenRequested` walks Home, a library, the details page (wide and narrow), the track list and the edit dialogs and saves one PNG per step; without the variable the test skips. The directory must be inside the checkout because the build container mounts only that.

`build/check.sh` uses ccache automatically when the image has it (cache in `/buildtmp/ccache`, the same build volume); CI restores the newest cache of that directory with `actions/cache`; only a push to main saves a new one, and `build/ci/prune_caches.py` keeps the two newest.

The build tree defaults to `/buildtmp/check` (a named podman volume that build-env.sh mounts, incremental between runs). Keep CMake build trees outside the checkout: the `dist/` tree is scanned by the REUSE gate, which would otherwise have to cover the fetched third-party mpvqt sources. `dist/` is not in git. To run the built app in the container you are on your own: playback needs a real session; do that on the host once it can be host-built, or inside the container for offscreen-only experiments.

mpvqt 1.2.1 is fetched by the root `CMakeLists.txt` (`FetchContent`, SHA256 pinned); the Flatpak manifest pins the same tarball and points `KINEFIN_MPVQT_URL` at it, so the two pins must move together. Its renderer is replaced by the copy in `third_party/mpvqt/` (the patch step of `FetchContent`); a new mpvqt version needs that copy diffed against its own renderer first.

The picture-in-picture window's KWin surface (`src/app/PipSurface.cpp`, the `xx_pip_v1` protocol vendored in `third_party/wayland/`) uses QtWaylandClient's private headers, which only the Flatpak SDK ships: it compiles in the Flatpak build alone (CMake prints `KWin picture-in-picture surface (xx_pip_v1): ON`), while the native build and the offscreen tests get the ordinary-window fallback.

After changing `build/Containerfile`, rebuild the local image before checking:

```sh
podman build -t localhost/kinefin-build:fedora45 -f build/Containerfile build
```

## Checks

Run the whole suite:

```sh
build/build-env.sh build/check.sh
```

`python3 scripts/po.py status` alone shows the pending translations per language (zero is required).

The gates that need no build (metainfo, catalogs, Python tests, REUSE) run before the build, and every test has a 15-minute timeout. With `CI` set, `build/check.sh` reruns the failed tests once and prints a warning, so that a timing flake of the UI tests costs one rerun instead of the whole job; locally a failure fails at once. In CI the `changes` job also runs the static checks (shellcheck, actionlint, zizmor, the release verifiers) before the long jobs start.

`tests/shell_smoke.py` starts the binary offscreen five times (Servers, then Settings, About, the sign-in page and Home through the `KINEFIN_START_PAGE` start-up hook; the sign-in page and Home open without a session) and fails on any QML problem. Run it before proposing a commit. Shell scripts (`build/`, `scripts/`, `po/`) are shellcheck-clean (`--severity=warning`); C++ code follows the Qt-like style of `.clang-format` (LLVM base with explicit knobs: the image's clang-format has no `Qt` preset). Run `clang-format -i <files>` before proposing, because the image gate fails on drift. `.editorconfig` governs the non-C++ text files.

`tests/test_kwallet_host.cpp` runs against the session's real KWallet and is skipped unless `KINEFIN_TEST_KWALLET=1`. Run it against the session bus from the build container, after a build in `/buildtmp/dev`:

```sh
podman run --rm --userns=keep-id --network=host --security-opt label=disable -e QT_QPA_PLATFORM=offscreen -e KINEFIN_TEST_KWALLET=1 -e DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus" -v "$XDG_RUNTIME_DIR/bus:$XDG_RUNTIME_DIR/bus" -v "$PWD:$PWD" -w "$PWD" -v kinefin-buildtmp:/buildtmp localhost/kinefin-build:fedora45 /buildtmp/dev/bin/test_kwallet_host
```

## Flatpak

```sh
build/flatpak/build-flatpak.sh                 # dist/Kinefin-x86_64.flatpak
KINEFIN_EXPECT_VERSION=X.Y.Z build/flatpak/test-flatpak.sh dist/Kinefin-x86_64.flatpak
```

The build script installs the org.kde runtime at commits recorded in `build/flatpak/runtime-pins.conf` and refuses to run outside rootless podman. The runtimes live in the podman volume `kinefin-flatpak`; the flatpak-builder state (module cache, sources, ccache) lives in the volume `kinefin-flatpak-state`, or in the host directory `KINEFIN_FLATPAK_STATE_DIR`, which CI restores on every run and saves after a push to main, with a key per month and without the build trees (the release job has none and builds clean). The first local build after the state moved to its own volume starts cold. Until the first release, the About dialog of a local bundle shows its build time (`YYYY.MM.DD.HHMM`, the host's local time, or `KINEFIN_BUILD_ID` when set) in place of the version; a build with `CI` set (every workflow) keeps the version, and `--version` and the server always get the version of `CMakeLists.txt`. `test-flatpak.sh` installs the bundle into a throwaway container `HOME`, checks the version, installed sandbox permissions and raster icons, D-Bus single-instance behaviour, the MPRIS name, the remote-control socket (private, with its welcome, taken back after a killed instance), private profile storage and QML start-up. A real Plasma/Wayland host is still needed for playback, KWallet, portals and GUI acceptance.

The `repo-update` CI job (after `flatpak`, on a plain `ubuntu-24.04` runner, so with Ubuntu's own flatpak and ostree) runs `build/flatpak/test-repo-update.sh` against the repository the `flatpak` job built (`dist/flatpak-repo`). With a real flatpak and ostree, throwaway keys and a local web server, it makes a first and a second release through the release scripts and checks that an installed client updates, that a commit signed by another key is refused, that the signed bundle installs with only its embedded key while a bundle with another key is refused, and that the served `.flatpakref` installs. Locally, after a Flatpak build, run it in the Flatpak image: `podman run --rm --security-opt label=disable -v "$PWD:$PWD" -w "$PWD" localhost/kinefin-flatpak:fedora45 build/flatpak/test-repo-update.sh dist/flatpak-repo` (rebuild the image first when its `Containerfile` changed).

## Translations

Strings are `i18n()` calls (QML: plain `i18n()` injected with `KLocalization::setupLocalizedContext()` from `KF6::I18nQml`). The project requires KF6 >= 6.24, the first Kirigami with `Kirigami.StyleHints` (the KDE 6.11 Flatpak runtime ships Kirigami 6.32). When a file gains its first translatable string, add it to `po/POTFILES.in` in the same change (the extraction script fails otherwise), then regenerate `po/kinefin.pot` with `build/build-env.sh po/extract.sh` and fill `it.po` (`python3 scripts/po.py todo` lists the pending entries). `en.po` repeats the msgid. Catalogs are compiled by the CMake install (ECM PO); `po/extract.sh --check` must verify real source extraction, and `po.py status --strict` must report zero pending per language or the check fails.

`po/kinefin.pot` is generated: never edit it by hand.

Action labels follow the KDE HIG ellipsis rule ([Text and labels](https://develop.kde.org/hig/text_and_labels/#ellipses)):

- End a label with `…` (the U+2026 character, never three periods) when the action always requires additional user input before it completes, such as a dialog, a picker or a confirmation.
- Leave it off when the action completes at once or only toggles state (`Mark as Watched`, `Download`, `Ask for Password`).
- An entry that opens a popup submenu shows the menu arrow, not an ellipsis.
- A dialog title repeats the action's wording without the ellipsis (`Delete Media…` opens `Delete Media`).

## Releasing

Source preparation is implemented; first-release acceptance is not closed. Do not tag or publish before the required v1 features, real Jellyfin/Plasma rounds and packaging gates pass. The authoritative instructions are [release tooling](build/release/README.md) and the [first-release checklist](build/release/FIRST_RELEASE.md).

1. Run `python3 -I build/release/preflight.py` for the offline source gates, then `build/build-env.sh build/check.sh` for the native/container checks.
2. When v1 acceptance is complete, prepare the version with `build/release/version-bump.sh --dry-run --date YYYY-MM-DD X.Y.Z` and `build/release/version-bump.sh --date YYYY-MM-DD X.Y.Z`; review and commit CMake and AppStream together.
3. Review the rebuilt Flatpak image from the Images workflow and set the repository variable `KINEFIN_RELEASE_FLATPAK_DIGEST` to its approved `sha256:` digest. Verify KDE SDK/Platform commits against Flathub availability.
4. Only after green CI on the intended `main` commit, create the protected `vX.Y.Z` tag. The release workflow validates the tag, builds and smokes the bundle, verifies signed attestations and provenance, and publishes the three checked release assets. This workflow has **not yet been verified end to end from a real tag**.
5. Review the generated commit-pinned Flathub manifest separately. Submission to Flathub is a manual, independent review; the GitHub release workflow does not submit it.

Source checks and the 128px/256px icon are present, but they do not replace a build on the pinned image, a real host review or a maintainer decision to release.

## How to work here

- Follow the [code of conduct](CODE_OF_CONDUCT.md).
- Follow the [KDE HIG](https://develop.kde.org/hig/); a deliberate exception states its reason in the change.
- Client settings keep Jellyfin Desktop's names, defaults and values wherever they map to mpv. A setting added, renamed or dropped updates the [settings reference](docs/settings.md) in the same change; `test_settings_parity` fails otherwise.
- A change users or contributors can see updates the [user guide](docs/user-guide.md) or the [architecture overview](docs/architecture.md) in the same change.
- Every new source file starts with its SPDX header (the KDE REUSE convention): `SPDX-FileCopyrightText: 2026 <author>` + `SPDX-License-Identifier: GPL-3.0-or-later`; the metainfo metadata keeps CC0 and the docs carry no header: `REUSE.toml` covers them, `LICENSES/` provides license texts, and `reuse lint` is mandatory in the build image. Verify exceptions and file-level headers when adding third-party files.
- Keep shell scripts POSIX sh (the build runs plain `/bin/sh` inside Fedora images) and oksh/fish out of CI.
- Treat remote metadata (item names, cast text, images) as untrusted input everywhere; see the invariants in the [architecture overview](docs/architecture.md#invariants). Never log or leak the access token.

## Validation scope of this snapshot

Portable checks (including a real C++20 state-machine compile) run with `scripts/testing/run_portable_checks.sh`; the native Qt/KF6 tests require the build container. The `build/release/` unit tests exercise release logic with local fixtures; signed attestation retrieval, a real tag build and host/Flatpak tests still require external systems. A fake Jellyfin server can be exercised with `python3 scripts/testing/probe_jellyfin.py --fixture`; live-server read-only API probes require explicitly supplied environment variables (see the script). Playback and session reporting still require dedicated host tests.
