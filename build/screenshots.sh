#!/bin/sh
# Renders the screenshots of the documentation into docs/images from a library of open films (tests/DocScene.h): see --help
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
base=${KINEFIN_BUILD_IMAGE:-localhost/kinefin-build:fedora45}
image=${KINEFIN_SHOTS_IMAGE:-localhost/kinefin-shots:fedora45}
bdir=${KINEFIN_BUILD_DIR:-/buildtmp/dev}

usage() {
    cat <<'USAGE'
Usage: build/screenshots.sh

Builds the UI test of the project in the build container, then runs its `documentationScreenshotsWhenRequested` function in a second image (build/screenshots/Containerfile) that adds the Breeze Qt style and a virtual X server with Mesa's software OpenGL, against the fake server and the library of open films of tests/DocScene.h (posters and frames in tests/assets/doc, CC BY, credited in the README). It writes one PNG per page to docs/images: home, library, filters, details, series, season, player and player-subtitles, and the same pages in the Breeze Dark colour scheme to docs/images/dark. The window is 1280 by 960 (720 for the player), English, no user data: nothing of a real server or account is in them. The dark run points XDG_CONFIG_HOME at a kdeglobals made of BreezeDark.colors, which is what Plasma writes when the scheme is chosen: Kirigami and the Breeze style read their colours from it. With KINEFIN_BUILD_DIR the test is built in another directory of the build volume.

The pictures have no window title bar, and the picture of the player holds a frame of Tears of Steel in place of the frame mpv draws; the controls around it are the real ones. The time after Ends at is the time of the run, so a rerun changes those PNGs.
USAGE
}
case ${1:-} in -h | --help) usage; exit 0 ;; esac

build="$root/build/build-env.sh"
"$build" true
podman image exists "$image" || podman build -t "$image" -f "$root/build/screenshots/Containerfile" "$root/build/screenshots"
jobs=$(($(nproc) / 2))
"$build" env CMAKE_BUILD_PARALLEL_LEVEL="$jobs" KINEFIN_BUILD_DIR="$bdir" sh -c 'cmake -S . -B "$KINEFIN_BUILD_DIR" -G Ninja -DBUILD_TESTING=ON >/dev/null && cmake --build "$KINEFIN_BUILD_DIR" --target test_ui_pages'
# One run of the test with the pictures written to $1, in the colour scheme named by $2 (light is the default of the style, dark has the scheme file written to kdeglobals first)
shoot() {
    KINEFIN_BUILD_IMAGE=$image "$build" sh -c '
        mkdir -p "$1"
        if [ "$2" = dark ]; then
            mkdir -p /tmp/colors
            cp /usr/share/color-schemes/BreezeDark.colors /tmp/colors/kdeglobals
            printf "\n[General]\nColorScheme=BreezeDark\n\n[KDE]\nwidgetStyle=Breeze\n" >> /tmp/colors/kdeglobals
            export XDG_CONFIG_HOME=/tmp/colors
        fi
        env KINEFIN_DOC_SHOTS="$1" QT_STYLE_OVERRIDE=Breeze QT_QPA_PLATFORM=xcb xvfb-run -a -s "-screen 0 1920x1200x24" "$3" documentationScreenshotsWhenRequested
    ' sh "$1" "$2" "$bdir/bin/test_ui_pages"
}
shoot "$root/docs/images" light
shoot "$root/docs/images/dark" dark
ls -l "$root/docs/images" "$root/docs/images/dark"
