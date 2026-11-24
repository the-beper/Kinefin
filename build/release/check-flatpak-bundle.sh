#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs the signed bundle as a user would and checks what it installed: pass or fail, nothing is written for another job. It runs in a job of its own because flatpak deploys a bundle even with --no-deploy (flatpak 1.14.6 and 1.18.4, tried), so that job holds no write, signing or attestation permission, no token and no secret, and hands nothing on: whatever the bundle does when it is deployed cannot choose what a later job trusts
# shellcheck disable=SC2154 # $app_ref comes from flatpak-lib.sh
set -eu
tools=$(cd "$(dirname "$0")/../.." && pwd)
root=${KINEFIN_ROOT:-$tools}
pubkey=${SIGNING_PUBKEY:-$tools/build/flatpak/signing-key.asc}
me=check-flatpak-bundle
scratch=${RUNNER_TEMP:-${TMPDIR:-/tmp}}
tmp=$(mktemp -d "$scratch/bundle.XXXXXX")
cleanup() {
    set +e
    gpgconf --homedir "$tmp/gnupg" --kill all 2> /dev/null
    chmod -R u+w "$tmp" 2> /dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
# shellcheck source=/dev/null
. "$tools/build/release/flatpak-lib.sh"

usage() {
    cat >&2 <<'EOF'
usage: check-flatpak-bundle.sh <Kinefin-x86_64.flatpak> <refs.txt>
  installs the bundle into a scratch user installation that has only the key embedded in it, and requires the installed app commit to be the one refs.txt lists for the app ref, signed by the pinned key
  writes nothing but its scratch space: no GITHUB_OUTPUT, no file for another job
environment: EXPECT_FLATPAK_SHA256 EXPECT_REFS_SHA256 GITHUB_SHA
  optional: RUNNER_TEMP, SIGNING_PUBKEY (default build/flatpak/signing-key.asc), KINEFIN_ROOT
  refuses to run with FLATPAK_GPG_PRIVATE_KEY set
exit: 0 ok, 1 a check failed, 2 bad input or the secret is set
EOF
    exit 2
}

no_secret
[ $# -eq 2 ] || usage
: "${GITHUB_SHA:?}" "${EXPECT_FLATPAK_SHA256:?}" "${EXPECT_REFS_SHA256:?}"
for f in "$1" "$2"; do [ -f "$f" ] || bad_input "$f does not exist"; done
bundle=$(readlink -f "$1")
refs=$(readlink -f "$2")
# the files, before anything reads them
check_sha256 "$bundle" "$EXPECT_FLATPAK_SHA256" "the bundle"
check_sha256 "$refs" "$EXPECT_REFS_SHA256" "refs.txt"
[ "$(git -C "$root" rev-parse HEAD)" = "$GITHUB_SHA" ] || fail "HEAD is not $GITHUB_SHA"
read_refs "$refs"
app_commit=$(awk -F'\t' -v r="$app_ref" '$1 == r { print $2 }' "$refs")
printf '%s\n' "$app_commit" | grep -Eq '^[0-9a-f]{64}$' || bad_input "refs.txt has no commit for $app_ref"
mkdir -m 700 "$tmp/gnupg"
GNUPGHOME=$tmp/gnupg
export GNUPGHOME
want_fpr=$(key_fpr "$pubkey")
printf '%s\n' "$want_fpr" | grep -Eq '^[0-9A-F]{40}$' || bad_input "cannot read the fingerprint of $pubkey"
gpg --batch --dearmor < "$pubkey" > "$tmp/public.gpg"

# the bundle as a user gets it, in a user installation of its own with no key but the one inside it
FLATPAK_USER_DIR=$tmp/client
export FLATPAK_USER_DIR
mkdir "$FLATPAK_USER_DIR"
flatpak install --user --noninteractive -y --no-deps --no-related --bundle "$bundle" > "$tmp/install.log" 2>&1 || { sed 's/^/    /' "$tmp/install.log" >&2; fail "the bundle does not install with only the key embedded in it"; }
installed=$(flatpak info --user --show-commit "$app_id") || fail "the bundle installed no $app_id"
[ "$installed" = "$app_commit" ] || fail "the bundle installs the commit $installed, not the listed app commit $app_commit"
# The commit of the installation must carry a signature of the pinned key: it is pulled into a scratch repository whose only remote knows that key alone, with signature checks on, so the exit status decides and no output (which the build's commit message could imitate) is parsed
ostree --repo="$tmp/chk" init --mode=archive
ostree --repo="$tmp/chk" remote add --set=gpg-verify=true --gpg-import="$tmp/public.gpg" pinned "file://$tmp/nowhere"
ostree --repo="$tmp/chk" pull-local --remote=pinned --gpg-verify "$FLATPAK_USER_DIR/repo" "$installed" > "$tmp/verify.log" 2>&1 || { sed 's/^/    /' "$tmp/verify.log" >&2; fail "the commit of the bundle is not signed by the pinned key $want_fpr"; }
echo "$me: the bundle installs with only its embedded key, at the listed app commit $app_commit, signed by $want_fpr"
