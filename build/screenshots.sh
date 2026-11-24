#!/bin/sh
# Renders the screenshots of the documentation into docs/images from a library of open films (tests/DocScene.h): see --help
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
image=${KINEFIN_SHOTS_IMAGE:-localhost/kinefin-shots:fedora45}
bdir=${KINEFIN_BUILD_DIR:-/buildtmp/dev}

usage() {
    cat <<'USAGE'
Usage: build/screenshots.sh

Builds the UI test of the project in the build container, then runs its `documentationScreenshotsWhenRequested` function in a second image (build/screenshots/Containerfile) inside a nested KWin on a virtual output at scale 2 (build/screenshots/shoot.sh), against the fake server and the library of open films of tests/DocScene.h (posters and frames in tests/assets/doc, CC BY, credited in THIRD_PARTY.md). KWin draws the Breeze title bar, the shadow and the rounded corners, which Flathub asks the pictures of a software centre to show. KWin composites with OpenGL, so the run needs the host's GPU (/dev/dri).

It writes one PNG per page to docs/images: home, library, filters, details, series, season, player and player-subtitles, and the same pages in the Breeze Dark colour scheme to docs/images/dark, then shrinks them with oxipng. The window is 1000 by 625 (Flathub asks for at most 1000 by 700), English, no user data: nothing of a real server or account is in them. The dark run writes BreezeDark.colors to kdeglobals, as Plasma does when the scheme is chosen. With KINEFIN_BUILD_DIR the test is built in another directory of the build volume.

The picture of the player holds a frame of Tears of Steel in place of the frame mpv draws; the controls around it are the real ones. The time after Ends at is the time of the run, so a rerun changes those PNGs.
USAGE
}
case ${1:-} in -h | --help) usage; exit 0 ;; esac

[ -e /dev/dri ] || { echo "build/screenshots.sh needs the host GPU (/dev/dri) for KWin's compositing" >&2; exit 1; }
build="$root/build/build-env.sh"
"$build" true
# Built on every run: the layer cache makes an unchanged Containerfile cheap, and a changed one or a new build image is never missed
podman build -t "$image" -f "$root/build/screenshots/Containerfile" "$root/build/screenshots"
jobs=$(($(nproc) / 2))
"$build" env CMAKE_BUILD_PARALLEL_LEVEL="$jobs" KINEFIN_BUILD_DIR="$bdir" sh -c 'cmake -S . -B "$KINEFIN_BUILD_DIR" -G Ninja -DBUILD_TESTING=ON >/dev/null && cmake --build "$KINEFIN_BUILD_DIR" --target test_ui_pages'
# podman directly rather than build-env.sh: the run takes the host's GPU, which KWin's OpenGL compositing needs (without it KWin crashes at the first screenshot)
in_shots_image() {
    podman run --rm --userns=keep-id --group-add keep-groups --device /dev/dri --security-opt label=disable \
        -v "$root:$root" -w "$root" -v kinefin-buildtmp:/buildtmp "$image" "$@"
}
in_shots_image sh build/screenshots/shoot.sh "$root/docs/images" light "$bdir/bin/test_ui_pages"
in_shots_image sh build/screenshots/shoot.sh "$root/docs/images/dark" dark "$bdir/bin/test_ui_pages"
in_shots_image sh -c 'oxipng -q -o 4 --strip safe "$1"/*.png "$1"/dark/*.png' sh "$root/docs/images"
ls -l "$root/docs/images" "$root/docs/images/dark"
