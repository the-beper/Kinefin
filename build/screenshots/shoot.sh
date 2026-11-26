#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Inside the screenshots image (build/screenshots.sh runs it with the PNG folder, light or dark, and the test_ui_pages binary): starts a nested KWin on a virtual output at scale 2 and runs the documentation shots of the UI test in it
set -eu
out=$1
scheme=$2
binary=$3
case $scheme in
light | dark) ;;
*)
    echo "unknown colour scheme: $scheme (light or dark)" >&2
    exit 2
    ;;
esac
export XDG_RUNTIME_DIR=/tmp/xdg HOME=/tmp/home
export XDG_CONFIG_HOME="$HOME/.config" XDG_DATA_HOME="$HOME/.local/share"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
mkdir -p "$out" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME/applications" "$HOME/share"
# KWin takes the icon of the title bar from the desktop file the window's app id names, and the buttons take the icons of the application (the Cast one) from a data directory, as an install lays them out
cp data/io.github.the_beper.Kinefin.desktop "$XDG_DATA_HOME/applications/"
cp -r data/icons "$HOME/share/"
export XDG_DATA_DIRS="$HOME/share:/usr/local/share:/usr/share"
# Dark is the scheme file written to kdeglobals, as Plasma writes it when the scheme is chosen: Kirigami, the Breeze style and the window decoration read their colours from it
if [ "$scheme" = dark ]; then
    cp /usr/share/color-schemes/BreezeDark.colors "$XDG_CONFIG_HOME/kdeglobals"
    printf '\n[General]\nColorScheme=BreezeDark\n\n[KDE]\nwidgetStyle=Breeze\n' >>"$XDG_CONFIG_HOME/kdeglobals"
fi
printf '[Common]\nShadowSize=ShadowSmall\n' >"$XDG_CONFIG_HOME/breezerc"
# kwin_wayland carries a file capability, which a rootless container refuses to execute; a copy has none
cp /usr/bin/kwin_wayland "$XDG_RUNTIME_DIR/kwin_wayland"
# shellcheck disable=SC2016
exec dbus-run-session -- sh -c '
    set -eu
    log="$XDG_RUNTIME_DIR/kwin.log"
    "$XDG_RUNTIME_DIR/kwin_wayland" --virtual --no-lockscreen --width=2560 --height=1600 --socket=kinefin-shots >"$log" 2>&1 &
    kwin=$!
    # Whatever ends the run stops KWin, and a failure shows its log
    trap '\''status=$?; kill "$kwin" 2>/dev/null || true; [ "$status" = 0 ] || cat "$log" >&2; exit "$status"'\'' EXIT
    for _ in $(seq 50); do
        [ -S "$XDG_RUNTIME_DIR/kinefin-shots" ] && break
        sleep 0.2
    done
    [ -S "$XDG_RUNTIME_DIR/kinefin-shots" ] || { echo "KWin did not start" >&2; exit 1; }
    export WAYLAND_DISPLAY=kinefin-shots
    # The --scale option of kwin_wayland does not reach the virtual output, so the scale is set once KWin runs, and checked: the metainfo states the size of the pictures
    kscreen-doctor output.Virtual-0.scale.2 >/dev/null
    scaled=no
    for _ in $(seq 25); do
        kscreen-doctor -o | sed "s/\x1b\[[0-9;]*m//g" | grep -q "Scale: 2" && scaled=yes && break
        sleep 0.2
    done
    [ "$scaled" = yes ] || { echo "the virtual output did not take scale 2" >&2; exit 1; }
    env KINEFIN_DOC_SHOTS="$1" QT_STYLE_OVERRIDE=Breeze QT_QPA_PLATFORM=wayland "$2" documentationScreenshotsWhenRequested
' sh "$out" "$binary"
