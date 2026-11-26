#!/bin/sh
# Smoke test of the Flatpak bundle in a throw-away container (no desktop: offscreen Qt, a private session bus): install with non-default XDG folders, check the version, start the app offscreen, check it owns its D-Bus name and its MPRIS name, check the private remote-control socket and its welcome, check a second launch hands over and exits, check a restart takes back the socket a killed instance left, check that the profile data lands under ~/.var/app, check that the token store's library has the libsecret backend, and check that deleting a profile with a saved session fails fast and names the keyring when there is no Secret portal. Playback, portal dialogs and theming need a real Plasma session and are untested here.
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
export XDG_DATA_HOME="$HOME"/xdg-data XDG_CONFIG_HOME="$HOME"/xdg-config
# Without it Flatpak keeps the app's runtime folder under ~/.cache instead of the /run/user one the socket check reads
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
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

# Artwork gate: the installed Flatpak must ship the raster icons AppStream can compose. The scalable source is installed for native packages only and removed by the Flatpak manifest's post-install step
install_root=$(flatpak info --system --show-location "$id") || fail "read installed Flatpak location"
for size in 128 256; do
    [ -s "$install_root/files/share/icons/hicolor/${size}x${size}/apps/$id.png" ] ||
        fail "missing installed ${size}x${size} application icon"
done
[ ! -e "$install_root/files/share/icons/hicolor/scalable/apps/$id.svg" ] ||
    fail "the Flatpak must not contain the scalable SVG icon"
echo "ok: installed raster icon sizes and Flatpak SVG policy"

# Hardware decoding gate: the default copy mode needs a VA-API device that mpv creates itself, and a build without x11 gets one only through the DRM backend
libmpv=$(find "$install_root/files/lib" -name 'libmpv.so.*' -type f | head -n 1)
[ -n "$libmpv" ] || fail "libmpv is not in the installed Flatpak"
grep -ao 'List of enabled features:[ -~]*' "$libmpv" | grep -qw vaapi-drm ||
    fail "libmpv has no vaapi-drm: hardware decoding in copy mode cannot start"
echo "ok: libmpv has the VA-API DRM backend"

# Token store gate: QtKeychain is built here against the runtime's libsecret, and the libsecret backend is the only one the sandbox allows (QTKEYCHAIN_BACKEND is pinned in the manifest). A build without it would keep every session in memory
keychain=$(find "$install_root/files/lib" -name 'libqt6keychain.so.*' -type f | head -n 1)
[ -n "$keychain" ] || fail "libqt6keychain is not in the installed Flatpak"
grep -aq secret_password_lookup "$keychain" || fail "libqt6keychain has no libsecret backend: no token could be stored"
echo "ok: QtKeychain has the libsecret backend"

# HDR output gate: CMake builds the path only where it finds QtWaylandClient's private headers, and a bundle without it would show HDR video as SDR without a word. Only the HDR code imports Qt's wrappers of wp_color_manager_v1, whose names stay in the stripped binary's dynamic symbols
binary="$install_root/files/bin/kinefin"
[ -f "$binary" ] || fail "the kinefin binary is not in the installed Flatpak"
grep -aq wp_color_manager_v1 "$binary" || fail "kinefin was built without the HDR output path"
echo "ok: kinefin has the HDR output path"

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

# MPRIS takes the default profile's name once the window is up, through the sandbox's wildcard own-name grant
mpris_owned() {
    gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.NameHasOwner org.mpris.MediaPlayer2.Kinefin.profile_default 2>/dev/null | grep -q true
}
for _ in $(seq 20); do mpris_owned && break; sleep 1; done
mpris_owned || { cat "$HOME/app.log"; fail "the app did not own org.mpris.MediaPlayer2.Kinefin.profile_default"; }
echo "ok: MPRIS name org.mpris.MediaPlayer2.Kinefin.profile_default"

# The remote-control socket sits in the app's runtime folder, which Flatpak shares with the host: private to the user, and it greets a client with the version
socket="$XDG_RUNTIME_DIR/app/$id/kinefin-default"
welcome_of() {
    python3 -I -c 'import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sys.argv[1])
data = b""
while b"\n" not in data:
    chunk = s.recv(4096)
    if not chunk:
        break
    data += chunk
print(data.split(b"\n")[0].decode())' "$1"
}
for _ in $(seq 20); do [ -S "$socket" ] && break; sleep 1; done
[ -S "$socket" ] || { cat "$HOME/app.log"; fail "no remote-control socket at $socket"; }
[ "$(stat -c %a "$socket")" = 600 ] || fail "the remote-control socket is not private: $(stat -c %a "$socket")"
welcome_of "$socket" | grep -q '"version"' || fail "the remote-control socket sent no welcome"
echo "ok: remote-control socket, private, with its welcome"

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

# A killed instance leaves its socket behind: the next start replaces it. The old one must be gone first, or its own welcome would pass for the new one's
for _ in $(seq 20); do welcome_of "$socket" >/dev/null 2>&1 || break; sleep 1; done
flatpak run --env=QT_QPA_PLATFORM=offscreen "$id" >"$HOME/app2.log" 2>&1 &
for _ in $(seq 20); do welcome_of "$socket" >/dev/null 2>&1 && break; sleep 1; done
welcome_of "$socket" 2>/dev/null | grep -q '"version"' || { cat "$HOME/app2.log"; fail "a restart did not take the socket back"; }
echo "ok: a restart takes the socket back"
flatpak kill "$id"

# The container has no Secret portal and no secret service, so the keyring cannot be reached: deleting a profile that holds a saved session must still finish (the token removal fails, it never waits for the 30 s limit), say so naming the keyring, and must not create a plaintext or empty keyring folder as a side effect
flatpak run --env=QT_QPA_PLATFORM=offscreen "$id" --create-profile keyringgate >/dev/null 2>&1 || fail "could not create the keyring gate profile"
gate_data="$HOME/.var/app/$id/data/kinefin/profiles/keyringgate"
mkdir -p "$gate_data"
printf '%s\n' '{"servers": [{"serverId": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "url": "https://jellyfin.invalid", "userId": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "deviceId": "gate", "deviceName": "gate", "username": "gate", "remembered": true}]}' >"$gate_data/servers.json"
started=$(date +%s)
status=0
out=$(timeout 40 flatpak run --env=QT_QPA_PLATFORM=offscreen "$id" --delete-profile keyringgate 2>&1) || status=$?
elapsed=$(($(date +%s) - started))
[ "$status" -eq 1 ] || fail "deleting a profile with a saved session exited $status, not 1: $out"
[ "$elapsed" -lt 20 ] || fail "deleting a profile with a saved session took $elapsed s"
printf '%s\n' "$out" | grep -qi keyring || fail "the deletion failure does not name the keyring: $out"
[ ! -e "$gate_data" ] || fail "the profile files of the keyring gate are still there"
[ ! -e "$HOME/.var/app/$id/data/keyrings" ] || fail "a keyrings folder was created without a Secret portal"
echo "ok: deleting a profile with a saved session fails within $elapsed s, names the keyring and creates no keyring"
assert_pin "$sdk" "$sdk_commit"
assert_pin "$platform" "$platform_commit"
echo "done"
