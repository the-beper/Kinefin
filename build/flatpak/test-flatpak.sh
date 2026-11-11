#!/bin/sh
# Smoke test of the Flatpak bundle in a throw-away container (no desktop: offscreen Qt, a private session bus): install with non-default XDG folders, check the version, start the app offscreen, check it owns its D-Bus name, check a second launch hands over and exits, and check that the profile data lands under ~/.var/app. Playback, portal dialogs and theming need a real Plasma session and are untested here.
# Usage: [KINEFIN_EXPECT_VERSION=X.Y.Z] build/flatpak/test-flatpak.sh dist/Kinefin-x86_64.flatpak
# KINEFIN_EXPECT_VERSION (optional): main.cpp uses QCommandLineParser::addVersionOption(); KAboutData provides QCoreApplication::applicationVersion. Upper/lower-case app labels and optional colon are accepted.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
image=localhost/kinefin-flatpak:fedora45

if [ ! -e /run/.containerenv ]; then
    bundle=$(readlink -f "$1")
    [ "$(podman info --format '{{.Host.Security.Rootless}}')" = true ] || { echo "test-flatpak.sh needs rootless podman" >&2; exit 1; }
    # The runtime and the pinned commits live in the build volume (kinefin-flatpak), so the
    # smoke mounts it and installs the bundle into the system installation: a --user install
    # would have to pull the runtime from a remote, which a throw-away HOME cannot offer.
    # The inner script pins before and after, so a smoke can never push the pins around.
    set --
    if [ -n "${KINEFIN_FLATPAK_REPO:-}" ]; then
        set -- -e "KINEFIN_FLATPAK_REPO=$KINEFIN_FLATPAK_REPO"
    fi
    exec podman run --rm --security-opt unmask=ALL --cap-add SYS_ADMIN --cap-add NET_ADMIN --security-opt label=disable \
        --device /dev/fuse -e KINEFIN_EXPECT_VERSION -v kinefin-flatpak:/var/lib/flatpak \
        -v "$bundle:/bundle.flatpak:ro" -v "$root/build/flatpak/test-flatpak.sh:/test.sh:ro" \
        -v "$root/build/flatpak/runtime-pins.conf:/runtime-pins.conf:ro" \
        -v "$root/build/flatpak/io.github.the_beper.Kinefin.yml:/manifest.yml:ro" \
        -v "$root/build/release/verify_flatpak_permissions.py:/verify-permissions.py:ro" \
        "$@" "$image" sh -c 'mkdir -p -m 700 /run/user/0 && exec dbus-run-session -- /test.sh /bundle.flatpak'
fi

# The smoke test consumes the same pinned commits as the build. In the
# container the file is mounted read-only next to /test.sh.
# shellcheck source=build/flatpak/runtime-pins.conf
. "$(dirname "$0")/runtime-pins.conf"
assert_pin() { [ "$(flatpak info --system --show-commit "$1")" = "$2" ] || { echo "$1 is not at the pinned commit $2" >&2; exit 1; }; }
assert_pin "$sdk" "$sdk_commit"
assert_pin "$platform" "$platform_commit"

id=io.github.the_beper.Kinefin
fail() { echo "FAIL: $*" >&2; exit 1; }
HOME=$(mktemp -d)
export HOME
trap 'flatpak kill "$id" 2>/dev/null; kill %1 2>/dev/null; rm -rf "$HOME"' EXIT
export XDG_DATA_HOME=$HOME/xdg-data XDG_CONFIG_HOME=$HOME/xdg-config
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"

# --no-deps: the runtime and its extension are already at the pinned commits; pulling
# anything here would move the pins the build script then refuses. --reinstall keeps the
# smoke idempotent against leftovers of a previous run in the shared volume.
flatpak install --system -y --noninteractive --reinstall --no-deps --bundle "$1" >/dev/null 2>&1 || fail "install"
echo "ok: installed"
assert_pin "$sdk" "$sdk_commit"
assert_pin "$platform" "$platform_commit"
# Check the actual Flatpak metadata from the installed bundle before launching it.
# The source-only release gate cannot detect extra permissions in a built bundle.
flatpak info --system --show-metadata "$id" >"$HOME/bundle-metadata" || fail "read bundle metadata"
python3 -I /verify-permissions.py --manifest /manifest.yml \
    --metadata "$HOME/bundle-metadata" || fail "installed Flatpak sandbox differs from approved permissions"
echo "ok: installed sandbox permissions match reviewed policy"

if [ -n "${KINEFIN_EXPECT_VERSION:-}" ]; then
    out=$(flatpak run --env=QT_QPA_PLATFORM=offscreen "$id" --version 2>&1) || fail "--version failed: $out"
    printf '%s\n' "$out" | grep -qxF -e "Kinefin $KINEFIN_EXPECT_VERSION" -e "Kinefin: $KINEFIN_EXPECT_VERSION" \
        -e "kinefin $KINEFIN_EXPECT_VERSION" -e "kinefin: $KINEFIN_EXPECT_VERSION" \
        || fail "--version is not $KINEFIN_EXPECT_VERSION: $out"
    echo "ok: version $KINEFIN_EXPECT_VERSION"
fi

# Start the app offscreen (a real Plasma session would raise a window; here KDBusService only has to own the name) and check the bus, the profile bootstrap and the log for fatal QML errors.
flatpak run --env=QT_QPA_PLATFORM=offscreen "$id" >"$HOME/app.log" 2>&1 &
owned() {
    gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.NameHasOwner io.github.the_beper.Kinefin 2>/dev/null | grep -q true
}
for _ in $(seq 20); do owned && break; sleep 1; done
owned || { cat "$HOME/app.log"; fail "the app did not own io.github.the_beper.Kinefin"; }
echo "ok: single instance owns io.github.the_beper.Kinefin"

# A Flatpak app uses its own per-app XDG config root (independent of the
# host's XDG_CONFIG_HOME). The profile index is configuration, not media data.
profile_index="$HOME/.var/app/$id/config/kinefin/profiles.json"
[ -f "$profile_index" ] || fail "no profiles.json in Flatpak's per-app config directory"
echo "ok: profile bootstrap inside Flatpak's per-app configuration"

grep -q "QQmlApplicationEngine failed to load component" "$HOME/app.log" && {
    cat "$HOME/app.log"; fail "a QML file failed to load"; }
echo "ok: no fatal QML errors"

# A second launch hands over to the running instance and exits within a timeout.
timeout 30 flatpak run --env=QT_QPA_PLATFORM=offscreen "$id" >/dev/null 2>&1 || fail "the second launch did not hand over"
echo "ok: second launch hands over"
flatpak kill "$id"
assert_pin "$sdk" "$sdk_commit"
assert_pin "$platform" "$platform_commit"
echo "done"
