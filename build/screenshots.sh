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

Builds the UI test of the project in the build container, then runs its `documentationScreenshotsWhenRequested` function in a second image (build/screenshots/Containerfile) that adds the Breeze Qt style and a virtual X server with Mesa's software OpenGL, against the fake server and the library of open films of tests/DocScene.h (posters and frames in tests/assets/doc, CC BY, credited in the README). It writes one PNG per page to docs/images: home, library, filters, details, series, season, player and player-subtitles. The window is 1280 by 960 (720 for the player), light theme, English, no user data: nothing of a real server or account is in them. With KINEFIN_BUILD_DIR the test is built in another directory of the build volume.

The pictures have no window title bar, and the picture of the player holds a frame of Tears of Steel in place of the frame mpv draws; the controls around it are the real ones. The time after Ends at is the time of the run, so a rerun changes those PNGs.
USAGE
}
case ${1:-} in -h | --help) usage; exit 0 ;; esac

build="$root/build/build-env.sh"
"$build" true
podman image exists "$image" || podman build -t "$image" -f "$root/build/screenshots/Containerfile" "$root/build/screenshots"
jobs=$(($(nproc) / 2))
"$build" env CMAKE_BUILD_PARALLEL_LEVEL="$jobs" KINEFIN_BUILD_DIR="$bdir" sh -c 'cmake -S . -B "$KINEFIN_BUILD_DIR" -G Ninja -DBUILD_TESTING=ON >/dev/null && cmake --build "$KINEFIN_BUILD_DIR" --target test_ui_pages'
mkdir -p "$root/docs/images"
KINEFIN_BUILD_IMAGE=$image "$build" env KINEFIN_DOC_SHOTS="$root/docs/images" QT_STYLE_OVERRIDE=Breeze QT_QPA_PLATFORM=xcb xvfb-run -a -s "-screen 0 1920x1200x24" "$bdir/bin/test_ui_pages" documentationScreenshotsWhenRequested
ls -l "$root/docs/images"
