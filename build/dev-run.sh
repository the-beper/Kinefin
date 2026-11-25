#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: build/dev-run.sh [--shots=step,step,...] [kinefin options...]
# --shots signs in, walks the steps (home, movies, shows, music, audiobooks, favorites, playlists, downloads, search, item:<id>) offscreen, saves one PNG of the window per step in tmp/shots (KINEFIN_DEV_SIZE=WxH, default 1280x800) and exits: nothing appears on the desktop.
# Builds the dev tree (/buildtmp/dev, the volume build-env.sh uses) and runs it from the build container on the desktop's Wayland session, with its data on a tmpfs: nothing outlives the run and the Flatpak's ~/.var/app is never read or written. KINEFIN_DEV_NO_BUILD=1 skips the build step.
# The server under test comes from `.env` in the repository root (git-ignored, mode 600, `KEY=value` lines read by podman; KINEFIN_DEV_ENV names another file): KINEFIN_DEV_SERVER, KINEFIN_DEV_USER and KINEFIN_DEV_PASSWORD sign the dev build in at every start. Without them, sign in from the window.
# Not representative of the Flatpak: the container has its own Mesa, FFmpeg and Qt, no VA-API decoders (hardware decoding stays off), no portals and no session bus of the desktop (a private one: no MPRIS, no notifications).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
image=${KINEFIN_DEV_IMAGE:-localhost/kinefin-dev:fedora45}
runtime=${XDG_RUNTIME_DIR:-}
display=${WAYLAND_DISPLAY:-}
shots=
case "${1:-}" in
    --shots=*) shots=${1#--shots=}; shift ;;
esac
[ -n "$shots" ] || [ -n "$runtime" ] && [ -n "$display" ] && [ -S "$runtime/$display" ] || { echo "dev-run.sh needs a Wayland session (WAYLAND_DISPLAY and XDG_RUNTIME_DIR)" >&2; exit 1; }

if [ -z "${KINEFIN_DEV_NO_BUILD:-}" ]; then
    # shellcheck disable=SC2016
    "$root/build/build-env.sh" sh -c 'cmake -S "$0" -B /buildtmp/dev -G Ninja -DBUILD_TESTING=ON -DKINEFIN_DEV_AUTOLOGIN=ON >/dev/null && nice -n 19 ninja -C /buildtmp/dev -j4 kinefin' "$root"
fi

podman image exists "$image" || podman build -t "$image" -f "$root/build/dev.Containerfile" "$root/build"

# The desktop's sockets are mounted under /host-run and named by environment variables: the runtime directory of the app is its own, owned by the user, on the tmpfs
set -- "$image" sh -c 'mkdir -p -m 700 /tmp/kinefin-dev/run /tmp/kinefin-dev/home && exec dbus-run-session -- /buildtmp/dev/bin/kinefin --config-dir /tmp/kinefin-dev/data "$@"' kinefin-dev "$@"
if [ -e "$runtime/pipewire-0" ]; then
    set -- -v "$runtime/pipewire-0:/host-run/pipewire-0" -e PIPEWIRE_RUNTIME_DIR=/host-run "$@"
fi
if [ -e "$runtime/pulse/native" ]; then
    set -- -v "$runtime/pulse:/host-run/pulse" -e PULSE_SERVER=unix:/host-run/pulse/native "$@"
fi
# The colour scheme follows the desktop
if [ -f "$HOME/.config/kdeglobals" ]; then
    set -- -v "$HOME/.config/kdeglobals:/host-config/kdeglobals:ro" -e XDG_CONFIG_DIRS=/host-config:/etc/xdg "$@"
fi
# The app's own icons (the cast action) are installed by the packages, not by the dev tree
set -- -v "$root/data/icons:/host-data/icons:ro" -e XDG_DATA_DIRS=/host-data:/usr/local/share:/usr/share "$@"
envfile=${KINEFIN_DEV_ENV:-$root/.env}
if [ -f "$envfile" ]; then
    [ -z "$(find "$envfile" -perm /077)" ] || { echo "$envfile holds a password: run chmod 600 on it" >&2; exit 1; }
    set -- --env-file "$envfile" "$@"
fi

if [ -n "$shots" ]; then
    mkdir -p "$root/tmp/shots"
    exec podman run --rm -i --userns=keep-id --network=host --security-opt label=disable \
        --tmpfs /tmp/kinefin-dev:rw,mode=1777 \
        -v "$root/tmp/shots:/shots" \
        -v kinefin-buildtmp:/buildtmp \
        -e XDG_RUNTIME_DIR=/tmp/kinefin-dev/run -e QT_QPA_PLATFORM=offscreen \
        -e HOME=/tmp/kinefin-dev/home -e LANG=C.UTF-8 -e QT_STYLE_OVERRIDE=Breeze -e QT_QPA_PLATFORMTHEME=kde -e QT_FONT_DPI -e QT_SCALE_FACTOR \
        -e KINEFIN_DEV_SHOTS=/shots -e "KINEFIN_DEV_STEPS=$shots" -e "KINEFIN_DEV_SIZE=${KINEFIN_DEV_SIZE:-1280x800}" \
        "$@"
fi
exec podman run --rm -i --userns=keep-id --network=host --security-opt label=disable \
    --device /dev/dri \
    --tmpfs /tmp/kinefin-dev:rw,mode=1777 \
    -v "$runtime/$display:/host-run/$display" \
    -v kinefin-buildtmp:/buildtmp \
    -e XDG_RUNTIME_DIR=/tmp/kinefin-dev/run -e WAYLAND_DISPLAY="/host-run/$display" -e QT_QPA_PLATFORM=wayland \
    -e HOME=/tmp/kinefin-dev/home -e LANG=C.UTF-8 -e QT_STYLE_OVERRIDE=Breeze -e QT_QPA_PLATFORMTHEME=kde -e QT_FONT_DPI -e QT_SCALE_FACTOR \
    "$@"
