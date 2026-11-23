#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Reads a running Plasma session and says whether Kinefin's desktop integrations show up (see --help)
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)

usage() {
    cat <<'USAGE'
Reads a running Plasma session and says whether Kinefin's desktop integrations show up: MPRIS, the screen and suspend inhibitions, KWallet and KRunner. It runs in the build image with no network and the session bus and the system bus mounted, so the host gets nothing installed and the container is trusted as much as the user's session. Start Kinefin first (the installed Flatpak or a native build), signed in, with a video playing for the player checks.

Usage: build/desktop-check.sh [--act] [--cast] [--term WORD] [--profile-dir DIR]
  --act              also control the player (Pause, Play, Seek) and run the first KRunner match: it changes what plays and opens an item in the app
  --cast             a cast is playing: also check the suspend inhibition
  --term WORD        what to ask KRunner for (default "the", at least three letters)
  --profile-dir DIR  the profiles folder to look for servers.json in: an absolute path, mounted read-only (default: the Flatpak's and the native one)

One line per check (PASS, FAIL, SKIP, or LOOK for what only eyes can judge) is printed and written to dist/desktop-check.txt; the exit status is 1 when a check failed.
USAGE
}
case ${1:-} in -h | --help) usage; exit 0 ;; esac

# On the host: run again in the build image with the buses mounted
if [ -z "${KINEFIN_DESKTOP_CHECK_INNER:-}" ] && [ ! -e /run/.containerenv ]; then
    image=${KINEFIN_BUILD_IMAGE:-localhost/kinefin-build:fedora45}
    runtime=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
    [ -S "$runtime/bus" ] || { echo "no session bus at $runtime/bus: run this inside the Plasma session" >&2; exit 2; }
    podman image exists "$image" || { echo "build image $image is missing: run build/build-env.sh once" >&2; exit 2; }
    given=
    prev=
    for arg in "$@"; do
        if [ "$prev" = --profile-dir ]; then given=$arg; fi
        prev=$arg
    done
    case $given in
    '' | /*) ;;
    *) echo "--profile-dir needs an absolute path: the container does not start in your working directory" >&2; exit 2 ;;
    esac
    set -- "$image" "$here/build/desktop-check.sh" "$@"
    # The system bus tells the suspend inhibitors; the profile folders tell whether a token sits on disk
    if [ -S /run/dbus/system_bus_socket ]; then set -- -v /run/dbus/system_bus_socket:/run/dbus/system_bus_socket "$@"; fi
    flatpak_profiles=$HOME/.var/app/io.github.the_beper.Kinefin/data/kinefin/profiles
    native_profiles=$HOME/.local/share/kinefin/profiles
    for dir in "$flatpak_profiles" "$native_profiles"; do
        if [ -d "$dir" ]; then set -- -v "$dir:$dir:ro" "$@"; fi
    done
    if [ -d "$given" ] && [ "$given" != "$flatpak_profiles" ] && [ "$given" != "$native_profiles" ]; then set -- -v "$given:$given:ro" "$@"; fi
    exec podman run --rm --userns=keep-id --network=none --security-opt label=disable \
        -e "DBUS_SESSION_BUS_ADDRESS=unix:path=$runtime/bus" -e "XDG_RUNTIME_DIR=$runtime" -e "HOME=$HOME" \
        -v "$runtime/bus:$runtime/bus" -v "$here:$here" -w "$here" "$@"
fi

act=0
cast=0
term=the
profile_dir=
while [ $# -gt 0 ]; do
    case $1 in
    --act) act=1 ;;
    --cast) cast=1 ;;
    --term) shift; term=${1:?--term needs a word} ;;
    --profile-dir) shift; profile_dir=${1:?--profile-dir needs a folder} ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
if [ -n "$profile_dir" ]; then
    set -- "$profile_dir"
else
    set -- "${HOME:-/nonexistent}/.var/app/io.github.the_beper.Kinefin/data/kinefin/profiles" "${HOME:-/nonexistent}/.local/share/kinefin/profiles"
fi

out=${KINEFIN_DESKTOP_CHECK_OUT:-$here/dist/desktop-check.txt}
mkdir -p "$(dirname "$out")"
: > "$out"
failed=0
say() {
    printf '%s\n' "$1"
    printf '%s\n' "$1" >> "$out"
}
pass() { say "PASS  $1${2:+: $2}"; }
fail() { say "FAIL  $1${2:+: $2}"; failed=$((failed + 1)); }
skip() { say "SKIP  $1${2:+: $2}"; }
look() { say "LOOK  $1"; }
finish() {
    say ""
    say "$failed check(s) failed; the report is in $out"
    [ "$failed" -eq 0 ]
}

if ! command -v busctl >/dev/null 2>&1; then
    fail "busctl" "busctl is missing: it comes with systemd, and every check below needs it"
    finish || exit 1
fi

# The text of a busctl property or reply after its type letter, without the quotes of a plain string
value() {
    sed -e 's/^[a-z]\{1,3\} //' -e 's/^"\(.*\)"$/\1/'
}

# --- MPRIS ---
mpris_name=$(busctl --user list --no-legend 2>/dev/null | awk '{print $1}' | grep '^org\.mpris\.MediaPlayer2\.Kinefin' | head -n 1 || true)
status=
if [ -z "$mpris_name" ]; then
    fail "MPRIS name" "no org.mpris.MediaPlayer2.Kinefin* on the session bus: is Kinefin running in this session?"
else
    pass "MPRIS name" "$mpris_name"
    mprop() { busctl --user get-property "$mpris_name" /org/mpris/MediaPlayer2 "$1" "$2" 2>/dev/null; }
    desktop=$(mprop org.mpris.MediaPlayer2 DesktopEntry | value || true)
    if [ "$desktop" = io.github.the_beper.Kinefin ]; then pass "MPRIS DesktopEntry" "$desktop"; else fail "MPRIS DesktopEntry" "'$desktop', expected io.github.the_beper.Kinefin"; fi
    identity=$(mprop org.mpris.MediaPlayer2 Identity | value || true)
    if [ -n "$identity" ]; then pass "MPRIS Identity" "$identity"; else fail "MPRIS Identity" "empty"; fi
    control=$(mprop org.mpris.MediaPlayer2.Player CanControl | value || true)
    if [ "$control" = true ]; then pass "MPRIS CanControl" true; else fail "MPRIS CanControl" "'$control'"; fi
    status=$(mprop org.mpris.MediaPlayer2.Player PlaybackStatus | value || true)
    case $status in
    Playing | Paused | Stopped) pass "MPRIS PlaybackStatus" "$status" ;;
    *) fail "MPRIS PlaybackStatus" "'$status'" ;;
    esac
    if [ "$status" = Stopped ] || [ -z "$status" ]; then
        skip "MPRIS Metadata" "nothing plays: start a video"
    else
        metadata=$(mprop org.mpris.MediaPlayer2.Player Metadata || true)
        if printf '%s' "$metadata" | grep -q 'xesam:title'; then pass "MPRIS Metadata title" "present"; else fail "MPRIS Metadata title" "no xesam:title"; fi
        # A token or a server address in what other programs read would leak the session
        if printf '%s' "$metadata" | grep -Eqi 'api_?key|access_?token|token='; then fail "MPRIS Metadata secrets" "a token-like text is in the metadata"; else pass "MPRIS Metadata secrets" "none"; fi
        art=$(printf '%s' "$metadata" | grep -o '"mpris:artUrl" s "[^"]*"' | sed 's/.* s "\(.*\)"/\1/' || true)
        case $art in
        '') skip "MPRIS artUrl" "none set" ;;
        file:*) pass "MPRIS artUrl" "a local file" ;;
        http:* | https:*) fail "MPRIS artUrl" "not a local file, scheme ${art%%:*} (a server address would carry the session)" ;;
        *) fail "MPRIS artUrl" "not a local file (a server address would carry the session)" ;;
        esac
    fi
    if [ "$act" = 1 ] && [ "$status" != Stopped ] && [ -n "$status" ]; then
        busctl --user call "$mpris_name" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player Pause >/dev/null 2>&1 || true
        sleep 1
        after=$(mprop org.mpris.MediaPlayer2.Player PlaybackStatus | value || true)
        if [ "$after" = Paused ]; then pass "MPRIS Pause" "Paused"; else fail "MPRIS Pause" "status is '$after'"; fi
        before=$(mprop org.mpris.MediaPlayer2.Player Position | value || echo 0)
        busctl --user call "$mpris_name" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player Seek x 5000000 >/dev/null 2>&1 || true
        sleep 1
        later=$(mprop org.mpris.MediaPlayer2.Player Position | value || echo 0)
        if [ "${later:-0}" -gt "${before:-0}" ] 2>/dev/null; then pass "MPRIS Seek" "$before to $later"; else fail "MPRIS Seek" "position $before then $later"; fi
        busctl --user call "$mpris_name" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player Play >/dev/null 2>&1 || true
        sleep 1
        after=$(mprop org.mpris.MediaPlayer2.Player PlaybackStatus | value || true)
        if [ "$after" = Playing ]; then pass "MPRIS Play" "Playing"; else fail "MPRIS Play" "status is '$after'"; fi
        status=$after
    elif [ "$act" = 1 ]; then
        skip "MPRIS controls" "nothing plays"
    fi
fi
look "The media applet and the lock screen show the title and the cover of what plays, and the media keys act on it"

# --- the screen stays on while a video plays ---
policy() {
    busctl --user call org.kde.Solid.PowerManagement.PolicyAgent /org/kde/Solid/PowerManagement/PolicyAgent \
        org.kde.Solid.PowerManagement.PolicyAgent ListInhibitions 2>/dev/null
}
# The listing is (application, reason) pairs: only how many belong to Kinefin is reported, never the reasons of other programs
inhibitions() { printf '%s' "$1" | grep -o '"[^"]*" "[^"]*"' | grep -ci "$2" || true; }
if ! listing=$(policy); then
    skip "Screen inhibition" "the Plasma power policy agent does not answer on this bus (not a Plasma session?)"
elif [ "$status" = Playing ]; then
    held=$(inhibitions "$listing" kinefin)
    if [ "$held" -gt 0 ]; then pass "Screen inhibition while playing" "Kinefin holds $held"; else fail "Screen inhibition while playing" "no inhibition of Kinefin among the $(inhibitions "$listing" .) held"; fi
    if [ "$act" = 1 ]; then
        busctl --user call "$mpris_name" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player Pause >/dev/null 2>&1 || true
        gone=0
        unanswered=0
        for _ in 1 2 3 4 5 6; do
            sleep 1
            if ! listing=$(policy); then unanswered=1; break; fi
            if [ "$(inhibitions "$listing" kinefin)" -eq 0 ]; then gone=1; break; fi
        done
        if [ "$gone" = 1 ]; then
            pass "Screen inhibition after Pause" "released"
        elif [ "$unanswered" = 1 ]; then
            skip "Screen inhibition after Pause" "the Plasma power policy agent stopped answering"
        else
            fail "Screen inhibition after Pause" "still held after 6 s"
        fi
        busctl --user call "$mpris_name" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player Play >/dev/null 2>&1 || true
    fi
elif [ "$status" = Paused ]; then
    held=$(inhibitions "$listing" kinefin)
    if [ "$held" -gt 0 ]; then fail "Screen inhibition while paused" "Kinefin still holds $held"; else pass "Screen inhibition while paused" "none held"; fi
else
    skip "Screen inhibition" "nothing plays: start a video"
fi

# --- the computer does not suspend while a cast plays ---
if [ "$cast" = 1 ]; then
    if inhibitors=$(systemd-inhibit --list --no-pager 2>/dev/null); then
        if printf '%s' "$inhibitors" | grep -i kinefin | grep -qi sleep; then pass "Suspend inhibition during a cast" "a sleep inhibitor of Kinefin"; else fail "Suspend inhibition during a cast" "no sleep inhibitor of Kinefin (is a cast playing?)"; fi
        if printf '%s' "$inhibitors" | grep -i kinefin | grep -qi idle; then fail "Suspend inhibition during a cast" "Kinefin also holds an idle inhibitor: the screen would stay on"; fi
    else
        skip "Suspend inhibition" "the system bus is not reachable"
    fi
else
    skip "Suspend inhibition" "pass --cast while a cast plays"
fi

# --- KWallet ---
if entries=$(kwallet-query -l kdewallet -f Kinefin 2>/dev/null); then
    count=0
    bad=0
    legacy=0
    id='[0-9A-Za-z-]{1,64}'
    for key in $entries; do
        count=$((count + 1))
        # The key is the profile, the server and the user (the ids are what isValidJellyfinId accepts): the values are never read
        if printf '%s' "$key" | LC_ALL=C grep -Eq "^[^/]+/$id/$id\$"; then
            :
        # Without a user it is the older per-server key, which stays until that user signs in again
        elif printf '%s' "$key" | LC_ALL=C grep -Eq "^[^/]+/$id\$"; then
            legacy=$((legacy + 1))
        else
            bad=$((bad + 1))
        fi
    done
    if [ "$count" -eq 0 ]; then
        fail "KWallet entries" "the folder Kinefin of kdewallet is empty: is a user signed in, with the wallet open?"
    elif [ "$bad" -gt 0 ]; then
        fail "KWallet entries" "$bad of $count keys are not <profile>/<serverId>/<userId>"
    else
        note=
        if [ "$legacy" -gt 0 ]; then note=" ($legacy of them <profile>/<serverId>, the older per-server key)"; fi
        pass "KWallet entries" "$count, named <profile>/<serverId>/<userId>$note"
    fi
else
    skip "KWallet entries" "kwallet-query could not read the folder Kinefin of kdewallet (wallet closed, disabled or absent)"
fi
found=0
leaked=0
unreadable=0
for dir in "$@"; do
    [ -d "$dir" ] || continue
    if ! list=$(find "$dir" -name servers.json -type f 2>/dev/null); then unreadable=$((unreadable + 1)); fi
    while IFS= read -r file; do
        [ -n "$file" ] || continue
        found=$((found + 1))
        # grep exits 1 when the pattern is absent and 2 when the file cannot be read: only the first one is a clean answer
        grep_status=0
        grep -Eqi '"(access_?)?token"' "$file" 2>/dev/null || grep_status=$?
        case $grep_status in
        0) leaked=$((leaked + 1)) ;;
        1) ;;
        *) unreadable=$((unreadable + 1)) ;;
        esac
    done <<LIST
$list
LIST
done
if [ "$leaked" -gt 0 ]; then
    fail "servers.json holds no token" "a token key is in $leaked servers.json file(s)"
elif [ "$unreadable" -gt 0 ]; then
    fail "servers.json holds no token" "$unreadable file(s) or folder(s) could not be read"
elif [ "$found" -eq 0 ]; then
    skip "servers.json holds no token" "no servers.json found in $* (the container sees only the folders mounted by the host run)"
else
    pass "servers.json holds no token" "$found file(s) checked"
fi

# --- KRunner ---
runner=$(busctl --user list --no-legend 2>/dev/null | awk '{print $1}' | grep '^io\.github\.the_beper\.Kinefin\.Runner' | head -n 1 || true)
if [ -z "$runner" ]; then
    fail "KRunner name" "no io.github.the_beper.Kinefin.Runner* on the session bus: is Kinefin running?"
else
    pass "KRunner name" "$runner"
    if reply=$(busctl --user call "$runner" /runner org.kde.krunner1 Match s "$term" 2>/dev/null); then
        n=$(printf '%s' "$reply" | awk '{print $2}')
        if [ "${n:-0}" -gt 0 ] 2>/dev/null; then
            pass "KRunner Match '$term'" "$n match(es)"
            if printf '%s' "$reply" | grep -Eqi 'api_?key|access_?token|token=|<'; then fail "KRunner Match text" "a token-like text or a markup character is in the matches"; else pass "KRunner Match text" "no token, no markup"; fi
            if [ "$act" = 1 ]; then
                id=$(printf '%s' "$reply" | sed -n 's/^a(sssida{sv}) [0-9]* "\([^"]*\)".*/\1/p')
                if [ -n "$id" ] && busctl --user call "$runner" /runner org.kde.krunner1 Run ss "$id" "" >/dev/null 2>&1; then pass "KRunner Run" "the first match was run"; else fail "KRunner Run" "the call failed"; fi
            fi
        else
            skip "KRunner Match '$term'" "no match: is the library searchable, or pass --term"
        fi
    else
        fail "KRunner Match" "the call failed"
    fi
    if busctl --user call "$runner" /runner org.kde.krunner1 Actions >/dev/null 2>&1; then pass "KRunner Actions" "answers"; else fail "KRunner Actions" "the call failed"; fi
fi
look "KRunner lists the results in a sensible order, and a result opens its item in Kinefin"

finish
