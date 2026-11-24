#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks, unpacks and signs the prepared Flatpak repository, only --sign touches the signing key
# shellcheck disable=SC2154,SC2034 # $base and $tab come from flatpak-lib.sh
# The first commands are the secret's: it leaves the environment before any other command can run
secret=${FLATPAK_GPG_PRIVATE_KEY:-}
unset FLATPAK_GPG_PRIVATE_KEY
# Nothing an earlier step put into the environment of this one may load code into the tools that run with the key
unset BASH_ENV ENV LD_PRELOAD LD_LIBRARY_PATH
set -eu
umask 077
tools=$(cd "$(dirname "$0")/../.." && pwd)
root=${KINEFIN_ROOT:-$tools}
pubkey=${SIGNING_PUBKEY:-$tools/build/flatpak/signing-key.asc}
me=sign-flatpak-repo
scratch=${RUNNER_TEMP:-${TMPDIR:-/tmp}}
tmp=$(mktemp -d "$scratch/sign.XXXXXX")
gnupg=
cleanup() {
    set +e
    if [ -n "$gnupg" ]; then
        gpgconf --homedir "$gnupg" --kill all 2> /dev/null
        rm -rf "$gnupg"
    fi
    chmod -R u+w "$tmp" 2> /dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
# shellcheck source=/dev/null
. "$tools/build/release/flatpak-lib.sh"

usage() {
    cat >&2 <<'EOF'
usage: sign-flatpak-repo.sh --check <prepared.tar> <sign-list.txt> <refs.txt> <out-dir>
       sign-flatpak-repo.sh --unpack <checked-repo.tar> <sign-list.txt> <refs.txt> <work-dir>
       sign-flatpak-repo.sh --sign <work-dir> <out-dir>
       sign-flatpak-repo.sh --cleanup [<work-dir>]
  --check and --unpack refuse to run with FLATPAK_GPG_PRIVATE_KEY set, --sign needs it (the armored secret key, no passphrase)
environment of --check: EXPECT_PREPARED_SHA256 EXPECT_SIGNLIST_SHA256 EXPECT_HEADS_SHA256 EXPECT_REFS_SHA256 EXPECT_BASE_SHA GITHUB_SHA GITHUB_REPOSITORY (or PAGES_REMOTE)
environment of --unpack: EXPECT_CHECKED_SHA256 EXPECT_SIGNLIST_SHA256 EXPECT_REFS_SHA256 EXPECT_HEADS_SHA256
  optional: RUNNER_TEMP, SIGNING_PUBKEY (default build/flatpak/signing-key.asc), KINEFIN_ROOT
  GITHUB_OUTPUT receives checked_sha256 (--check) or signed_sha256 (--sign)
exit: 0 ok, 1 a check failed, 2 key or input problem, 3 stale base
EOF
    exit 2
}

[ $# -ge 1 ] || usage
mode=$1
shift

# --cleanup: whatever an earlier step left
if [ "$mode" = --cleanup ]; then
    unset secret
    for d in /dev/shm/kinefin-gnupg.* "$scratch"/gnupg.*; do
        [ -d "$d" ] || continue
        gpgconf --homedir "$d" --kill all > /dev/null 2>&1 || true
        rm -rf "$d"
    done
    [ $# -eq 0 ] || rm -rf "$1"
    exit 0
fi

# --check: before the key exists, on a machine without it
if [ "$mode" = --check ]; then
    [ -z "$secret" ] || bad_input "FLATPAK_GPG_PRIVATE_KEY is set: --check reads data from other jobs and must run without the secret"
    unset secret
    [ $# -eq 4 ] || usage
    : "${GITHUB_SHA:?}" "${EXPECT_PREPARED_SHA256:?}" "${EXPECT_SIGNLIST_SHA256:?}" "${EXPECT_REFS_SHA256:?}" "${EXPECT_BASE_SHA:?}" "${EXPECT_HEADS_SHA256:?}"
    remote=${PAGES_REMOTE:-https://github.com/${GITHUB_REPOSITORY:?}.git}
    [ "$(git -C "$root" rev-parse HEAD)" = "$GITHUB_SHA" ] || fail "HEAD is not $GITHUB_SHA"
    for f in "$1" "$2" "$3"; do [ -f "$f" ] || bad_input "$f does not exist"; done
    prepared=$(readlink -f "$1")
    signlist=$(readlink -f "$2")
    refs=$(readlink -f "$3")
    check_sha256 "$prepared" "$EXPECT_PREPARED_SHA256" "the prepared site"
    check_sha256 "$signlist" "$EXPECT_SIGNLIST_SHA256" "the sign list"
    check_sha256 "$refs" "$EXPECT_REFS_SHA256" "refs.txt"
    read_refs "$refs"
    [ "$base" = "$EXPECT_BASE_SHA" ] || bad_input "refs.txt says the build started from $base, not from $EXPECT_BASE_SHA"

    # gh-pages must be where the build left it, before the key is touched (the publish job checks again before it pushes)
    heads=$(git ls-remote --heads "$remote" gh-pages | cut -f1)
    if [ "$base" = - ]; then
        [ -z "$heads" ] || stale "the build saw no gh-pages, but it exists now ($heads)"
    else
        [ "$heads" = "$base" ] || stale "gh-pages is $heads, the build started from $base"
    fi

    tar_safe "$prepared" site
    work=$tmp/check
    mkdir "$work"
    tar -xf "$prepared" -C "$work" --no-same-owner
    repo=$work/site/repo
    [ -d "$repo/objects" ] || bad_input "the prepared site has no ostree repository"
    # git keeps no empty directory, and ostree (Ubuntu's at least) fails without these
    mkdir -p "$repo/refs/heads" "$repo/refs/mirrors" "$repo/refs/remotes" "$repo/tmp"
    read_signlist "$signlist" "$refs"
    while IFS=$tab read -r ref commit; do
        [ "$(ostree --repo="$repo" rev-parse "$ref")" = "$commit" ] || fail "$ref is not at the listed commit in the prepared repository"
    done < "$signlist"
    check_config "$repo/config"
    no_deltas "$repo"
    check_repo_entries "$repo"
    check_pages_base "$repo"
    check_repo_paths "$repo"
    ostree --repo="$repo" fsck --quiet > "$tmp/fsck.log" 2>&1 || { sed 's/^/    /' "$tmp/fsck.log" >&2; fail "ostree fsck of the prepared repository failed"; }
    while IFS=$tab read -r ref commit; do
        check_binding "$repo" "$ref" "$commit"
        check_appstream_commit "$repo" "$ref" "$commit"
        no_commitmeta "$repo" "$commit"
        sigs=$(ostree --repo="$repo" show --print-detached-metadata-key=ostree.gpgsigs "$commit" 2> /dev/null || true)
        [ -z "$sigs" ] || fail "$ref $commit has a signature already: prepare lists only new commits"
    done < "$signlist"
    # The summary is made again from the refs; whatever the old files hold is not read with the key at hand
    rm -rf "$repo/summary" "$repo/summary.sig" "$repo/summary.idx" "$repo/summary.idx.sig" "$repo/summaries"
    # Clean what the checks left (ostree writes into tmp/) and look at the tree that is archived, not at the one that was checked
    rm -rf "$repo/tmp"
    mkdir "$repo/tmp"
    check_tree "$repo" "$EXPECT_HEADS_SHA256"
    mkdir -m 700 "$tmp/gnupg"
    GNUPGHOME=$tmp/gnupg
    export GNUPGHOME
    want_fpr=$(key_fpr "$pubkey")
    printf '%s\n' "$want_fpr" | grep -Eq '^[0-9A-F]{40}$' || bad_input "cannot read the fingerprint of $pubkey"
    # This machine's own tar, from the plain tree: the sign job takes this archive and nothing else of the run but its pin
    mkdir -p "$4"
    out=$(cd "$4" && pwd)
    tar --sort=name --owner=0 --group=0 --numeric-owner -C "$work/site" -cf "$out/checked-repo.tar" repo
    cp "$signlist" "$out/sign-list.txt"
    checked_sha=$(sha256sum "$out/checked-repo.tar" | cut -d' ' -f1)
    echo "$me: checked: $(wc -l < "$signlist") refs to sign, key $want_fpr; checked-repo.tar $checked_sha"
    if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "checked_sha256=$checked_sha" >> "$GITHUB_OUTPUT"; fi
    exit 0
fi

# --unpack: what the check job wrote, on the machine that holds the key, in a step that does not
if [ "$mode" = --unpack ]; then
    [ -z "$secret" ] || bad_input "FLATPAK_GPG_PRIVATE_KEY is set: --unpack reads data from another job and must run without the secret"
    unset secret
    [ $# -eq 4 ] || usage
    pin_checked=${EXPECT_CHECKED_SHA256:-}
    pin_signlist=${EXPECT_SIGNLIST_SHA256:-}
    pin_refs=${EXPECT_REFS_SHA256:-}
    pin_heads=${EXPECT_HEADS_SHA256:-}
    for pin in "$pin_checked" "$pin_signlist" "$pin_refs" "$pin_heads"; do
        printf '%s\n' "$pin" | grep -Eq '^[0-9a-f]{64}$' || bad_input "EXPECT_CHECKED_SHA256, EXPECT_SIGNLIST_SHA256, EXPECT_REFS_SHA256 and EXPECT_HEADS_SHA256 must be the hashes the earlier jobs reported (64 hex digits, not empty)"
    done
    for f in "$1" "$2" "$3"; do [ -f "$f" ] || bad_input "$f does not exist"; done
    checked=$(readlink -f "$1")
    signlist=$(readlink -f "$2")
    refs=$(readlink -f "$3")
    check_sha256 "$checked" "$pin_checked" "the checked repository"
    check_sha256 "$signlist" "$pin_signlist" "the sign list"
    check_sha256 "$refs" "$pin_refs" "refs.txt"
    tar_safe "$checked" repo
    read_refs "$refs"
    read_signlist "$signlist" "$refs"
    [ ! -e "$4" ] || [ -z "$(ls -A "$4")" ] || bad_input "$4 is not empty"
    mkdir "$tmp/unpack"
    tar -xf "$checked" -C "$tmp/unpack" --no-same-owner
    repo=$tmp/unpack/repo
    [ -d "$repo/objects" ] || bad_input "the checked archive has no ostree repository"
    # What --check made sure of, again: the archive is not trusted for it. The summary the key signs lists every head of the repository, the unlisted ones too, with its xa.metadata, and flatpak reads the .commitmeta2 files for it
    check_tree "$repo" "$pin_heads"
    while IFS=$tab read -r ref commit; do
        no_commitmeta "$repo" "$commit"
        obj=$repo/objects/$(printf '%s' "$commit" | cut -c1-2)/${commit#??}.commit
        [ -f "$obj" ] || fail "$ref: the commit object $commit is not in the checked repository"
        [ "$(sha256sum "$obj" | cut -d' ' -f1)" = "$commit" ] || fail "$ref: the commit object does not hash to its id $commit"
        [ -f "$repo/refs/heads/$ref" ] && [ "$(cat "$repo/refs/heads/$ref")" = "$commit" ] || fail "$ref does not point to the listed commit $commit in the checked repository"
    done < "$signlist"
    mkdir -p "$4"
    work=$(cd "$4" && pwd)
    mv "$repo" "$work/repo"
    cp "$signlist" "$work/sign-list.txt"
    echo "$me: unpacked: $(wc -l < "$signlist") refs to sign"
    exit 0
fi

# --sign: the secret is in $secret, and only here
[ "$mode" = --sign ] || bad_input "unknown mode $mode (--check, --unpack, --sign, --cleanup)"
[ $# -eq 2 ] || usage
work=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
repo=$work/repo
[ -f "$work/sign-list.txt" ] && [ -d "$repo/objects" ] || bad_input "$work is not the output of --unpack"
[ -n "$secret" ] || bad_input "the secret FLATPAK_GPG_PRIVATE_KEY is empty or not available to this job (see CONTRIBUTING.md, Releasing)"

# The keyring: in memory when there is a tmpfs, so that nothing of the key is left on a disk when the job ends
if [ -d /dev/shm ] && [ -w /dev/shm ]; then
    gnupg=$(mktemp -d /dev/shm/kinefin-gnupg.XXXXXX)
else
    gnupg=$(mktemp -d "$scratch/gnupg.XXXXXX")
fi
GNUPGHOME=$gnupg
export GNUPGHOME
# The key the checkout pins; the secret is in a variable of this shell only, no child has it
want_fpr=$(key_fpr "$pubkey")
printf '%s\n' "$want_fpr" | grep -Eq '^[0-9A-F]{40}$' || bad_input "cannot read the fingerprint of $pubkey"
if printf '%s\n' "$secret" | gpg --batch --quiet --import 2> "$gnupg/import.log"; then :; else
    unset secret
    sed 's/^/gpg: /' "$gnupg/import.log" >&2
    bad_input "the secret FLATPAK_GPG_PRIVATE_KEY cannot be imported"
fi
# Something to look for in the repository before it is archived: every line of the secret key text (the base64 body) and the keygrips of its keys. (They stay in the keyring directory, which is removed at the end.)
printf '%s\n' "$secret" | awk 'length($0) >= 40 && $0 !~ /^-/' > "$gnupg/leaks"
unset secret
secret_count=$(gpg --batch --list-secret-keys --with-colons | awk -F: '$1 == "sec" { n++ } END { print n + 0 }')
[ "$secret_count" -eq 1 ] || bad_input "the secret holds $secret_count keys, expected exactly one"
fpr=$(gpg --batch --list-secret-keys --with-colons | awk -F: '$1 == "fpr" { print $10; exit }')
[ "$fpr" = "$want_fpr" ] || bad_input "the secret key is $fpr, but $(basename "$pubkey") is $want_fpr: refusing to sign (that would make every installed client reject the repository)"
gpg --batch --list-secret-keys --with-keygrip --with-colons | awk -F: '$1 == "grp" { print $10 }' >> "$gnupg/leaks"
echo "PRIVATE KEY BLOCK" >> "$gnupg/leaks"
sed -i '/^$/d' "$gnupg/leaks"
echo probe > "$gnupg/probe"
gpg --batch --yes --quiet --pinentry-mode error --local-user "$fpr" --detach-sign -o "$gnupg/probe.sig" "$gnupg/probe" \
    || bad_input "the key cannot sign without a passphrase"
echo "$me: signing key $fpr matches $(basename "$pubkey")"

# sign the listed commits, then the summary that lists them Names and sizes of the files before: only the summary files and the signatures of the listed commits may come out different
repo_files() { (cd "$repo" && find . -path ./tmp -prune -o ! -type d -printf '%p %s\n' | LC_ALL=C sort); }
repo_files > "$gnupg/files.before"
while IFS=$tab read -r ref commit; do
    [ "$(ostree --repo="$repo" rev-parse "$ref")" = "$commit" ] || bad_input "$ref is not the commit that was checked"
    ostree --repo="$repo" gpg-sign --gpg-homedir="$gnupg" "$commit" "$fpr" > "$gnupg/gpg-sign.log" 2>&1 || {
        sed 's/^/    /' "$gnupg/gpg-sign.log" >&2
        fail "ostree gpg-sign failed for $ref"
    }
done < "$work/sign-list.txt"
# Only the summary and its signature: the appstream data is not regenerated, nothing is pruned, there are no static deltas (a delta carries the signature of its commit, and the signed summary has to list it)
flatpak build-update-repo --no-update-appstream --gpg-sign="$fpr" --gpg-homedir="$gnupg" "$repo" > "$gnupg/update.log" 2>&1 || {
    sed 's/^/    /' "$gnupg/update.log" >&2
    fail "flatpak build-update-repo (summary signature) failed"
}
rm -rf "$repo/tmp"

# The key lives in $gnupg, outside the repository; make sure nothing of it was copied in: no new or resized file but the allowed ones, no link, and not a trace of the key in any file (a write of the same size changes no name or size)
repo_files > "$gnupg/files.after"
allowed='^\./(summary|summary\.sig|summary\.idx|summary\.idx\.sig)$|^\./summaries/[^/]+$'
while IFS=$tab read -r ref commit; do printf './objects/%s/%s.commitmeta\n' "$(printf '%s' "$commit" | cut -c1-2)" "${commit#??}"; done < "$work/sign-list.txt" > "$gnupg/files.signatures"
diff "$gnupg/files.before" "$gnupg/files.after" | sed -n 's/^[<>] \(.*\) [0-9]*$/\1/p' | LC_ALL=C sort -u > "$gnupg/files.changed"
if bad=$(grep -Ev "$allowed" "$gnupg/files.changed" | grep -vxF -f "$gnupg/files.signatures"); then
    printf '%s\n' "$bad" | sed 's/^/    /' >&2
    fail "the signing tools changed files other than the summary files and the signatures of the listed commits (see above)"
fi
if grep -rqaF -f "$gnupg/leaks" "$repo"; then
    fail "key material found in the repository"
fi
tar --sort=name --owner=0 --group=0 --numeric-owner -C "$work" -cf "$out/signed-repo.tar" repo
signed_sha=$(sha256sum "$out/signed-repo.tar" | cut -d' ' -f1)
echo "$me: signed-repo.tar $signed_sha"
if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "signed_sha256=$signed_sha" >> "$GITHUB_OUTPUT"; fi
