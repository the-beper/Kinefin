#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the Flatpak repository scripts of the release against fixtures, with fake gh, flatpak and ostree and ephemeral GnuPG keys
usage() {
    cat >&2 <<'EOF'
usage: simulate.sh
  runs every case in a temporary directory and prints TAP ("ok N - ..." and "# N/N passed"), exit 0 when all pass
  needs sh, git, gpg 2.2+, python3, tar, sha256sum and base64, and no network, no real key and no container
  the real tools are not modelled beyond the fakes in build/release/testdata: flatpak, ostree, gh and GitHub Pages are checked by a release
EOF
    exit 2
}
set -eu
[ $# -eq 0 ] || usage
here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/../.." && pwd)
td=$here/testdata

for tool in git gpg python3 tar sha256sum base64; do
    command -v "$tool" > /dev/null || { echo "Bail out! $tool is needed"; exit 2; }
done

work=$(mktemp -d)
keys=$work/keys
mkdir -p "$keys" "$work/bin" "$work/home"
cleanup() {
    set +e
    for home in "$keys"/*; do
        [ -d "$home" ] && gpgconf --homedir "$home" --kill gpg-agent 2> /dev/null
    done
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# The fakes, first in PATH; git and gpg stay the real ones. No configuration of the host leaks in
for fake in gh flatpak ostree; do ln -s "$td/fake-$fake" "$work/bin/$fake"; done
PATH=$work/bin:$PATH
HOME=$work/home
GNUPGHOME=$work/gnupg-unused
GIT_CONFIG_GLOBAL=/dev/null
GIT_CONFIG_SYSTEM=/dev/null
GIT_AUTHOR_NAME=simulate GIT_AUTHOR_EMAIL=simulate@example.invalid
GIT_COMMITTER_NAME=simulate GIT_COMMITTER_EMAIL=simulate@example.invalid
export PATH HOME GNUPGHOME GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL
# A CI job that runs this may have these set, and the scripts append to them
unset GITHUB_OUTPUT GITHUB_ENV
mkdir -m 700 "$GNUPGHOME"

rel=$repo_root/build/release
app_id=io.github.the_beper.Kinefin
total=23
n=0
passed=0
failed=0
case_failed=0
diag=
rc=0

# test harness
run() { # run a command, keep its output in $work/last.out and in the log of everything, its status in $rc
    rc=0
    "$@" > "$work/last.out" 2>&1 || rc=$?
    cat "$work/last.out" >> "$work/all.log"
}
check() { # check "what" command args...: records a failure when the command fails
    what=$1
    shift
    if "$@"; then :; else
        case_failed=1
        diag="$diag# FAILED: $what
"
    fi
}
end_case() {
    n=$((n + 1))
    if [ "$case_failed" = 0 ]; then
        passed=$((passed + 1))
        echo "ok $n - $1"
    else
        failed=$((failed + 1))
        echo "not ok $n - $1"
        printf '%s' "$diag"
        echo "# last output:"
        sed 's/^/#   /' "$work/last.out" | tail -n 12
    fi
    case_failed=0
    diag=
}
not() { ! "$@"; }
rc_is() { [ "$rc" -eq "$1" ]; }
rc_fails() { [ "$rc" -ne 0 ]; }
out_has() { grep -q -e "$1" "$work/last.out"; }
file_has() { grep -q -e "$2" "$1"; }
same() { [ "$1" = "$2" ]; }
sha256_of() { sha256sum "$1" 2> /dev/null | cut -d' ' -f1; }
no_leftovers() { [ -z "$(find "$RUNNER_TEMP" -mindepth 1 -maxdepth 1)" ] && [ -z "$(find /dev/shm -maxdepth 1 -name 'kinefin-gnupg.*' 2> /dev/null)" ]; }

# ephemeral keys: a real GnuPG key pair each, made in a keyring that exists only in this run
gen_key() {
    d=$keys/$1
    mkdir -m 700 "$d"
    GNUPGHOME=$d gpg --batch --pinentry-mode loopback --passphrase '' --quick-gen-key "Kinefin ephemeral $1" ed25519 sign never > "$d/gen.log" 2>&1 \
        || { cat "$d/gen.log"; echo "Bail out! cannot generate a key"; exit 2; }
    GNUPGHOME=$d gpg --batch --list-keys --with-colons 2>> "$d/gen.log" | awk -F: '$1 == "fpr" { print $10; exit }' > "$d/fpr"
    GNUPGHOME=$d gpg --batch --pinentry-mode loopback --passphrase '' --armor --export-secret-keys "$(cat "$d/fpr")" 2>> "$d/gen.log" > "$d/secret.asc"
    GNUPGHOME=$d gpg --batch --armor --export "$(cat "$d/fpr")" 2>> "$d/gen.log" > "$d/pub.asc"
    GNUPGHOME=$d gpg --batch --list-secret-keys --with-keygrip --with-colons 2>> "$d/gen.log" | awk -F: '$1 == "grp" { print $10 }' > "$d/grips"
}
use_secret() { FLATPAK_GPG_PRIVATE_KEY=$(cat "$keys/$1/secret.asc"); export FLATPAK_GPG_PRIVATE_KEY; }
drop_secret() { unset FLATPAK_GPG_PRIVATE_KEY; }


make_world() { # <number>: a fresh project, origin and published repository, and the environment the scripts read
    w=$work/w$1
    origin=$w/origin.git
    mkdir -p "$w/runner-temp"
    git init -q --bare --initial-branch=main "$origin"
    seed=$w/seed
    git init -q --initial-branch=main "$seed"
    echo "world $1" > "$seed/README"
    git -C "$seed" add -A
    git -C "$seed" commit -q -m "Release 0.2.0"
    git -C "$seed" push -q "$origin" main
    git clone -q "$origin" "$w/checkout"

    GITHUB_SHA=$(git -C "$w/checkout" rev-parse HEAD)
    GITHUB_REPOSITORY=example/Kinefin
    KINEFIN_ROOT=$w/checkout
    SITE_URL=https://pages.example/Kinefin
    PAGES_REMOTE=file://$origin
    RUNNER_TEMP=$w/runner-temp
    SIGNING_PUBKEY=$keys/a/pub.asc
    GH_TOKEN=ephemeral-test-token
    FAKE_GH_STATE=$w/gh-state.json
    FAKE_OSTREE_LOG=$w/ostree.log
    FAKE_OSTREE_ORPHAN=
    FAKE_FLATPAK_LOG=$w/flatpak.log
    FAKE_FLATPAK_BUNDLE_COMMIT=
    FAKE_FLATPAK_BUNDLE_KEY=
    FAKE_FLATPAK_BUNDLE_NOSIG=
    FAKE_FLATPAK_INSTALL_NO_VERIFY=
    export GITHUB_SHA GITHUB_REPOSITORY KINEFIN_ROOT SITE_URL PAGES_REMOTE RUNNER_TEMP SIGNING_PUBKEY GH_TOKEN
    export FAKE_GH_STATE FAKE_OSTREE_LOG FAKE_OSTREE_ORPHAN FAKE_FLATPAK_LOG FAKE_FLATPAK_BUNDLE_COMMIT FAKE_FLATPAK_BUNDLE_KEY FAKE_FLATPAK_BUNDLE_NOSIG FAKE_FLATPAK_INSTALL_NO_VERIFY
    echo '{"releases": []}' > "$FAKE_GH_STATE"
    drop_secret

    # The published repository (gh-pages): version 0.1.0, signed with key A, as the live one is
    site=$w/base-site
    ostree --repo="$site/repo" fake-commit "app/$app_id/x86_64/stable" "published 0.1.0" > /dev/null
    flatpak build-sign --gpg-sign="$(cat "$keys/a/fpr")" --gpg-homedir="$keys/a" "$site/repo" "$app_id" stable
    GNUPGHOME=$keys/a GPG_KEY_ID=$(cat "$keys/a/fpr") "$repo_root/build/flatpak/publish-repo.sh" "$site" "$SITE_URL"
    git -C "$site" init -q --initial-branch=gh-pages
    git -C "$site" add -A
    git -C "$site" commit -q -m "Flatpak repository for v0.1.0"
    git -C "$site" push -q "$origin" HEAD:refs/heads/gh-pages
    base_sha=$(git -C "$site" rev-parse HEAD)
    published_commit=$(ostree --repo="$site/repo" rev-parse "app/$app_id/x86_64/stable")
}

# What the build jobs do: export a build onto a clone of gh-pages, take the commit out, prepare the unsigned site
build_jobs() {
    rm -rf "$w/pages" "$w/commits"
    git clone -q --depth 1 --branch gh-pages "file://$origin" "$w/pages"
    ostree --repo="$w/pages/repo" fake-commit "app/$app_id/x86_64/stable" "built 0.2.0" > /dev/null
    run "$rel/export-flatpak-commits.sh" "$w/pages" "$w/commits"
    job_prepare
}

# The jobs after the build, each leaves its status in $rc
job_prepare() { # the keyless job that finishes the repository unsigned
    rm -rf "$w/prepared" "$w/prepare.gho"
    EXPECT_COMMITS_SHA256=$(sha256_of "$w/commits/flatpak-commits.tar")
    EXPECT_REFS_SHA256=$(sha256_of "$w/commits/refs.txt")
    export EXPECT_COMMITS_SHA256 EXPECT_REFS_SHA256
    run env GITHUB_OUTPUT="$w/prepare.gho" "$rel/prepare-flatpak-repo.sh" "$w/commits/flatpak-commits.tar" "$w/prepared"
    cp "$work/last.out" "$w/prepare.out"
}
# The jobs check-flatpak (--check, no secret, from the files of prepare and the hashes it reported), sign-flatpak (--unpack without the secret, then --sign with it, from the files of the check and the hashes it reported, nothing else) and finish-flatpak. job_sign runs them in a row and stops at the first that fails, with its status in $rc
out_of() { sed -n "s/^$2=//p" "$1"; } # a value a job wrote to its GITHUB_OUTPUT
job_check() {
    rm -rf "$w/checked" "$w/signwork" "$w/signed-repo" "$w/signed" "$w/check.gho" "$w/sign.gho"
    EXPECT_PREPARED_SHA256=$(out_of "$w/prepare.gho" prepared_sha256)
    EXPECT_SIGNLIST_SHA256=$(out_of "$w/prepare.gho" signlist_sha256)
    EXPECT_REFS_SHA256=$(sha256_of "$w/commits/refs.txt")
    EXPECT_BASE_SHA=$(out_of "$w/prepare.gho" base_sha)
    EXPECT_HEADS_SHA256=$(out_of "$w/prepare.gho" heads_sha256)
    export EXPECT_PREPARED_SHA256 EXPECT_SIGNLIST_SHA256 EXPECT_REFS_SHA256 EXPECT_BASE_SHA EXPECT_HEADS_SHA256
    run env GITHUB_OUTPUT="$w/check.gho" "$rel/sign-flatpak-repo.sh" --check "$w/prepared/prepared.tar" "$w/prepared/sign-list.txt" "$w/commits/refs.txt" "$w/checked"
}
# What the sign job has: the artifact of the check job with the hash of its output, the build's refs.txt, and the hashes that prepare-flatpak and build-flatpak reported for the sign list and refs.txt; not the prepared site, not the environment of the check
sign_side() { run env -u EXPECT_PREPARED_SHA256 -u EXPECT_BASE_SHA "$@"; }
unpack_pinned() { # <checked-repo.tar hash> <sign list hash> [<refs.txt hash> [<heads hash>]]: the keyless step of the sign job, with the pins it was given
    rm -rf "$w/signwork"
    sign_side EXPECT_CHECKED_SHA256="$1" EXPECT_SIGNLIST_SHA256="$2" EXPECT_REFS_SHA256="${3-$(sha256_of "$w/commits/refs.txt")}" \
        EXPECT_HEADS_SHA256="${4-$(out_of "$w/prepare.gho" heads_sha256)}" \
        "$rel/sign-flatpak-repo.sh" --unpack "$w/checked/checked-repo.tar" "$w/checked/sign-list.txt" "$w/commits/refs.txt" "$w/signwork"
}
job_unpack() { unpack_pinned "$(out_of "$w/check.gho" checked_sha256)" "$(out_of "$w/prepare.gho" signlist_sha256)"; }
job_sign_step() { # <key name>: the step with the secret
    use_secret "${1:-a}"
    sign_side GITHUB_OUTPUT="$w/sign.gho" "$rel/sign-flatpak-repo.sh" --sign "$w/signwork" "$w/signed-repo"
    drop_secret
}
job_sign_key() { # <key name>: unpack, then the step with the secret
    job_unpack
    if [ "$rc" -eq 0 ]; then job_sign_step "${1:-a}"; fi
}
job_finish() {
    rm -f "$w/finish.gho"
    EXPECT_PREPARED_SHA256=$(out_of "$w/prepare.gho" prepared_sha256)
    EXPECT_SIGNLIST_SHA256=$(out_of "$w/prepare.gho" signlist_sha256)
    EXPECT_SIGNED_SHA256=$(out_of "$w/sign.gho" signed_sha256)
    EXPECT_REFS_SHA256=$(sha256_of "$w/commits/refs.txt")
    EXPECT_BASE_SHA=$(out_of "$w/prepare.gho" base_sha)
    export EXPECT_PREPARED_SHA256 EXPECT_SIGNLIST_SHA256 EXPECT_SIGNED_SHA256 EXPECT_REFS_SHA256 EXPECT_BASE_SHA
    run env GITHUB_OUTPUT="$w/finish.gho" "$rel/finish-flatpak-repo.sh" "$w/prepared/prepared.tar" "$w/prepared/sign-list.txt" "$w/signed-repo/signed-repo.tar" "$w/commits/refs.txt" "$w/signed"
}
job_bundle() { # the check-bundle job: the bundle with the hash finish reported, the build's refs.txt with the hash build-flatpak reported, no other output of the jobs before
    rm -f "$w/bundle.gho"
    : > "$FAKE_FLATPAK_LOG"
    run env -u EXPECT_PREPARED_SHA256 -u EXPECT_SIGNLIST_SHA256 -u EXPECT_SIGNED_SHA256 -u EXPECT_BASE_SHA GITHUB_OUTPUT="$w/bundle.gho" \
        EXPECT_FLATPAK_SHA256="${1-$(out_of "$w/finish.gho" flatpak_sha256)}" EXPECT_REFS_SHA256="${2-$(sha256_of "$w/commits/refs.txt")}" \
        "$rel/check-flatpak-bundle.sh" "${3-$w/signed/Kinefin-x86_64.flatpak}" "$w/commits/refs.txt"
}
job_sign() { # <key name>
    job_check
    if [ "$rc" -eq 0 ]; then job_sign_key "${1:-a}"; fi
    if [ "$rc" -eq 0 ]; then job_finish; fi
    cp "$work/last.out" "$w/sign.out"
}
tar_has() { tar -tf "$1" | grep -qx -e "$2"; } # a member of a tar (a basic regular expression)
prepared_has() { tar -tf "$w/prepared/prepared.tar" | grep -qx -e "$1"; }
prepared_file() { tar -xOf "$w/prepared/prepared.tar" "$1"; }
# The same job with another tag, commit or public key (the caller's own values are restored)
with_sha() { saved=$GITHUB_SHA; GITHUB_SHA=$1; export GITHUB_SHA; "$2"; GITHUB_SHA=$saved; export GITHUB_SHA; }
sign_with_pubkey() { saved=$SIGNING_PUBKEY; SIGNING_PUBKEY=$keys/$1/pub.asc; export SIGNING_PUBKEY; job_sign "$2"; SIGNING_PUBKEY=$saved; export SIGNING_PUBKEY; }

gen_key a
gen_key b
echo "TAP version 13"
echo "1..$total"

# the happy path, up to finish
make_world 1
build_jobs
job_sign
site_field() { tar -xOf "$w/signed/site.tar" "./$1" | sed -n "s/^$2=//p"; }
gpgkey=$(gpg --batch --dearmor < "$keys/a/pub.asc" | base64 -w0)
check "finish exits 0" rc_is 0
check "the signed site has the signature of the summary" tar -tf "$w/signed/site.tar" ./repo/summary.sig > /dev/null
check "(and the files users add the repository with)" tar -tf "$w/signed/site.tar" ./kinefin.flatpakref ./kinefin.flatpakrepo ./icon.svg ./.nojekyll > /dev/null
check "the .flatpakrepo has the title, the url, the icon, the homepage and the key" same "$(site_field kinefin.flatpakrepo Title) $(site_field kinefin.flatpakrepo Url) $(site_field kinefin.flatpakrepo Icon) $(site_field kinefin.flatpakrepo Homepage) $(site_field kinefin.flatpakrepo GPGKey)" "Kinefin https://pages.example/Kinefin/repo/ https://pages.example/Kinefin/icon.svg https://github.com/the-beper/Kinefin $gpgkey"
check "the .flatpakref has the app, the branch and the suggested remote name" same "$(site_field kinefin.flatpakref Name) $(site_field kinefin.flatpakref Branch) $(site_field kinefin.flatpakref SuggestRemoteName) $(site_field kinefin.flatpakref IsRuntime)" "io.github.the_beper.Kinefin stable kinefin false"
check "(its url, icon, homepage and key)" same "$(site_field kinefin.flatpakref Url) $(site_field kinefin.flatpakref Icon) $(site_field kinefin.flatpakref Homepage) $(site_field kinefin.flatpakref GPGKey)" "https://pages.example/Kinefin/repo/ https://pages.example/Kinefin/icon.svg https://github.com/the-beper/Kinefin $gpgkey"
check "(and the runtime repository)" same "$(site_field kinefin.flatpakref RuntimeRepo)" "https://dl.flathub.org/repo/flathub.flatpakrepo"
check "prepare kept the GPGKey of the published repository" file_has "$w/prepare.out" "the GPGKey is the published one"
check "the new app commit sits on the published one" same "$(awk -F'\t' '$1 ~ /^app\// { print $3 }' "$w/commits/refs.txt")" "$published_commit"
check "the sign list has the app ref and the two appstream refs" same "$(cut -f1 "$w/prepared/sign-list.txt" | tr '\n' ' ')" "app/$app_id/x86_64/stable appstream/x86_64 appstream2/x86_64 "
check "finish reports the hash of the site, the hash of the bundle and the base" same "$(out_of "$w/finish.gho" site_sha256) $(out_of "$w/finish.gho" flatpak_sha256) $(out_of "$w/finish.gho" base_sha)" "$(sha256_of "$w/signed/site.tar") $(sha256_of "$w/signed/Kinefin-x86_64.flatpak") $base_sha"
check "(and leaves no keyring or scratch directory behind)" no_leftovers
end_case "happy path: prepare, check, sign, finish; the new app commit sits on the published one, the site files have the fields of the repository"

# failed build and failed sign
make_world 7
git clone -q --depth 1 --branch gh-pages "file://$origin" "$w/pages-empty"
run "$rel/export-flatpak-commits.sh" "$w/pages-empty" "$w/commits-empty"
check "a build that exported nothing fails" rc_is 1
check "(the message says so)" out_has "added no commit"
git clone -q --depth 1 --branch gh-pages "file://$origin" "$w/pages-orphan"
FAKE_OSTREE_ORPHAN=1 ostree --repo="$w/pages-orphan/repo" fake-commit "app/$app_id/x86_64/stable" "orphan build" > /dev/null
run "$rel/export-flatpak-commits.sh" "$w/pages-orphan" "$w/commits-orphan"
check "a commit that does not follow the published one exits 3" rc_is 3
git clone -q --depth 1 --branch gh-pages "file://$origin" "$w/pages-debug"
ostree --repo="$w/pages-debug/repo" fake-commit "app/$app_id/x86_64/stable" "built 0.2.0" > /dev/null
ostree --repo="$w/pages-debug/repo" fake-commit "runtime/$app_id.Debug/x86_64/stable" "built debug 0.2.0" > /dev/null
run "$rel/export-flatpak-commits.sh" "$w/pages-debug" "$w/commits-debug"
check "the export of a build with a debug extension succeeds" rc_is 0
check "(it lists only the app ref)" same "$(cut -f1 "$w/commits-debug/refs.txt" | tr '\n' ' ')" "base app/$app_id/x86_64/stable "
check "(and the extension is not in the exported repository)" not test -e "$w/commits-debug/repo/refs/heads/runtime/$app_id.Debug/x86_64/stable"
build_jobs
job_sign
check "sign with the right key works" rc_is 0
check "(and leaves no keyring or scratch directory behind)" no_leftovers
check "(a client with only the public key accepted it in finish)" out_has "a client with only the public key accepts"
check "(the signed site has the signature of the summary)" tar -tf "$w/signed/site.tar" ./repo/summary.sig > /dev/null
job_sign b
check "sign with a key that is not the committed one is refused" rc_is 2
check "(the message names the problem)" out_has "refusing to sign"
check "(no keyring is left)" no_leftovers
check "(nothing was signed)" not test -e "$w/signed-repo/signed-repo.tar"
sign_with_pubkey b a
check "sign with a public key that is not the secret's is refused" rc_is 2
check "(no keyring is left)" no_leftovers
sign_with_pubkey b b
check "a new key, not the published one, signs, and finish refuses the site" rc_is 1
check "(the message says the GPGKey is not the key)" out_has "byte for byte"
check "(no keyring is left)" no_leftovers
check "(no site was written)" not test -e "$w/signed/site.tar"
FAKE_FLATPAK_LEAK=summary
export FAKE_FLATPAK_LEAK
job_sign
unset FAKE_FLATPAK_LEAK
check "sign refuses to archive a repository whose summary holds key material" rc_is 1
check "(the message says so)" out_has "key material found"
check "(nothing was signed)" not test -e "$w/signed-repo/signed-repo.tar"
check "(no keyring is left)" no_leftovers
FAKE_FLATPAK_LEAK=stray
export FAKE_FLATPAK_LEAK
job_sign
unset FAKE_FLATPAK_LEAK
check "sign refuses to archive a repository with a file it did not have" rc_is 1
check "(the message names the file)" out_has "leak.txt"
check "(nothing was signed)" not test -e "$w/signed-repo/signed-repo.tar"
check "(no keyring is left)" no_leftovers
FAKE_FLATPAK_LEAK="link"
export FAKE_FLATPAK_LEAK
job_sign
unset FAKE_FLATPAK_LEAK
check "sign refuses to archive a repository with a link it did not have" rc_is 1
check "(the message names the link)" out_has "leak-link"
check "(nothing was signed)" not test -e "$w/signed-repo/signed-repo.tar"
FAKE_FLATPAK_LEAK=inplace
export FAKE_FLATPAK_LEAK
job_sign
unset FAKE_FLATPAK_LEAK
check "sign refuses a file overwritten with key material, whose name and size did not change" rc_is 1
check "(the message says so)" out_has "key material found"
check "(nothing was signed)" not test -e "$w/signed-repo/signed-repo.tar"
check "(no keyring is left)" no_leftovers
job_check
job_unpack
run "$rel/sign-flatpak-repo.sh" --sign "$w/signwork" "$w/signed-repo"
check "sign without the secret exits 2" rc_is 2
check "(the message names the secret)" out_has "FLATPAK_GPG_PRIVATE_KEY"
check "(no keyring is left)" no_leftovers
git clone -q "file://$origin" "$w/other"
git -C "$w/other" checkout -q gh-pages
echo other > "$w/other/other.txt"
git -C "$w/other" add -A
git -C "$w/other" commit -q -m "Another release"
git -C "$w/other" push -q origin gh-pages
job_sign
check "sign refuses when gh-pages moved since the build (stale base)" rc_is 3
check "(no keyring is left)" no_leftovers
end_case "failed build and failed sign: a debug extension ref is not exported, nothing exported, wrong parent, wrong key, wrong public key, new key, key material, no secret, stale base; keyring always gone"

# review findings: config, bindings, appstream continuity, deltas, files outside repo
rehash_prepared() { sed -i '/^prepared_sha256=/d; /^signlist_sha256=/d' "$w/prepare.gho"; { echo "prepared_sha256=$(sha256_of "$w/prepared/prepared.tar")"; echo "signlist_sha256=$(sha256_of "$w/prepared/sign-list.txt")"; } >> "$w/prepare.gho"; }
tamper_prepared() { # <shell command run in the unpacked site>: a compromised prepare job
    rm -rf "$w/x"
    mkdir "$w/x"
    tar -xf "$w/prepared.orig/prepared.tar" -C "$w/x"
    (cd "$w/x/site/repo" && eval "$1")
    tar -C "$w/x" -cf "$w/prepared/prepared.tar" site
    rehash_prepared
}
make_world 24
build_jobs
check "prepare verified the published repository with the pinned key" file_has "$w/prepare.out" "verify with the pinned key"
cp -a "$w/prepared" "$w/prepared.orig"
cp "$w/prepare.gho" "$w/prepare.gho.orig"
app_ref=app/$app_id/x86_64/stable
app_commit=$(awk -F'\t' -v r="$app_ref" '$1 == r { print $2 }' "$w/commits/refs.txt")
restore_prepared() { rm -rf "$w/prepared"; cp -a "$w/prepared.orig" "$w/prepared"; cp "$w/prepare.gho.orig" "$w/prepare.gho"; }
objfile() { printf 'objects/%s/%s.commit' "$(printf '%s' "$1" | cut -c1-2)" "${1#??}"; }

job_sign
check "an honest run goes through" rc_is 0
# config
tamper_prepared 'printf "[remote \"evil\"]\nurl=file:///etc\ngpg-verify=false\n" >> config'
job_check
check "--check refuses a config with a remote" rc_is 2
check "(the message says so)" out_has "config of the repository"
restore_prepared
tamper_prepared 'echo "gpg-verify=false" >> config'
job_check
check "--check refuses a config with another key" rc_is 2
restore_prepared
# the old summary is gone before the key is used
job_check
check "the old summary files are not in the checked repository" not tar_has "$w/checked/checked-repo.tar" 'repo/summary[^/]*\|repo/summaries.*'
check "(and no scratch directory content)" not tar_has "$w/checked/checked-repo.tar" 'repo/tmp/.'
check "the checked repository holds repo/ and nothing else" same "$(tar -tf "$w/checked/checked-repo.tar" | grep -vc '^repo/')" 0
# binding of a listed commit
tamper_prepared "sed -i 's|^binding=.*|binding=app/other.app/x86_64/stable|' $(objfile "$app_commit")"
job_check
check "--check refuses a listed commit bound to another ref" rc_is 1
check "(the message says so)" out_has "not bound to its own ref"
restore_prepared
tamper_prepared "sed -i '/^binding=/d' $(objfile "$app_commit")"
job_check
check "--check refuses a listed commit with no binding" rc_is 1
restore_prepared
# a signature already on a listed commit
tamper_prepared "touch $(objfile "$app_commit" | sed 's/\.commit$/.commitmeta/')"
job_check
check "--check refuses a listed commit that has a signature" rc_is 1
restore_prepared
# the appstream commit continues the published one, or the key does not sign
tamper_prepared "FAKE_OSTREE_ORPHAN=1 ostree --repo=. fake-commit appstream/x86_64 evil > ../../newappstream"
sed -i "s/^\(appstream\/x86_64\t\).*/\1$(cat "$w/x/newappstream")/" "$w/prepared/sign-list.txt"
rehash_prepared
job_sign
check "--check refuses an appstream commit that does not continue the published one" rc_is 1
check "(the message says so)" out_has "does not continue the published commit"
restore_prepared
tamper_prepared "mkdir deltas; echo x > deltas/x"
job_sign
check "--check refuses static deltas" rc_is 1
check "(the message says so)" out_has "static deltas"
restore_prepared
tamper_prepared "touch 'we\\ird'"
job_sign
check "--check refuses a member name with a backslash" rc_is 2
check "(the message says so)" out_has "characters outside"
restore_prepared
tamper_prepared "echo '# changed' >> ../kinefin.flatpakref"
job_sign
check "finish refuses a changed file outside repo/" rc_is 1
check "(the message says so)" out_has "outside repo/"
restore_prepared
tamper_prepared "echo x > ../extra.html"
job_sign
check "finish refuses an extra file outside repo/" rc_is 1
restore_prepared
# refs that `ostree refs` does not list
tamper_prepared "mkdir -p refs/mirrors/org.example; printf '%s\n' $published_commit > refs/mirrors/org.example/x"
job_sign
check "--check refuses a ref under refs/mirrors" rc_is 1
check "(the message says so)" out_has "refs/mirrors"
restore_prepared
# a listed commit with another binding, after the signature step
job_check
job_sign_key
job_finish
check "finish accepts the honest signed repository" rc_is 0
rm -rf "$w/y"
mkdir "$w/y"
tar -xf "$w/signed-repo/signed-repo.tar" -C "$w/y"
sed -i 's|^binding=.*|binding=app/other.app/x86_64/stable|' "$w/y/repo/$(objfile "$app_commit")"
tar -C "$w/y" -cf "$w/signed-repo/signed-repo.tar" repo
sed -i '/^signed_sha256=/d' "$w/sign.gho"
echo "signed_sha256=$(sha256_of "$w/signed-repo/signed-repo.tar")" >> "$w/sign.gho"
rm -rf "$w/signed"
job_finish
check "finish refuses a listed commit bound to another ref" rc_is 1
check "(the message says so)" out_has "not bound to its own ref"
check "(no site was written)" not test -e "$w/signed/site.tar"
end_case "review findings: config, bindings, a signature already there, appstream continuity, deltas, files outside repo/, refs under refs/mirrors"

make_world 25
git clone -q "file://$origin" "$w/pubx"
git -C "$w/pubx" checkout -q gh-pages
echo tampered >> "$w/pubx/repo/summary"
git -C "$w/pubx" commit -q -a -m "Tampered"
git -C "$w/pubx" push -q origin gh-pages
base_sha=$(git -C "$w/pubx" rev-parse HEAD)
build_jobs
check "prepare refuses a published summary that the pinned key did not sign" rc_is 2
check "(the message says so)" out_has "does not verify with the pinned key"
check "(nothing was written)" not test -e "$w/prepared/prepared.tar"
end_case "tampered gh-pages: the base is verified with the pinned key before anything is built on it"

# gh-pages with refs/heads moved and the old signed summary kept
make_world 26
git clone -q "file://$origin" "$w/pubx"
git -C "$w/pubx" checkout -q gh-pages
mkdir -p "$w/pubx/repo/refs/heads" "$w/pubx/repo/refs/mirrors" "$w/pubx/repo/refs/remotes" "$w/pubx/repo/tmp"
evil=$(ostree --repo="$w/pubx/repo" fake-commit "app/$app_id/x86_64/stable" "evil")
ostree --repo="$w/pubx/repo" gpg-sign --gpg-homedir="$keys/a" "$evil" "$(cat "$keys/a/fpr")"
git -C "$w/pubx" add -A
git -C "$w/pubx" commit -q -m "Moved a ref, summary unchanged"
git -C "$w/pubx" push -q origin gh-pages
base_sha=$(git -C "$w/pubx" rev-parse HEAD)
build_jobs
check "prepare refuses a gh-pages whose refs are not those of its signed summary" rc_is 2
check "(the message says so)" out_has "not the refs and commits of its signed summary"
check "(nothing was written)" not test -e "$w/prepared/prepared.tar"
end_case "gh-pages with a ref moved under a valid signed summary: refused"

# gh-pages with a link under refs/mirrors: git keeps symbolic links; `ostree refs` and a search for plain files do not see one. (In a tar the link is refused by tar_safe, so only prepare, which works on the clone itself, depends on the check of ref_map.)
make_world 27
git clone -q "file://$origin" "$w/pubx"
git -C "$w/pubx" checkout -q gh-pages
mkdir -p "$w/pubx/repo/refs/mirrors/org.example"
ln -s "../../heads/app/$app_id/x86_64/stable" "$w/pubx/repo/refs/mirrors/org.example/x"
git -C "$w/pubx" add -A
git -C "$w/pubx" commit -q -m "A link under refs/mirrors"
git -C "$w/pubx" push -q origin gh-pages
base_sha=$(git -C "$w/pubx" rev-parse HEAD)
build_jobs
check "prepare refuses a gh-pages with a link under refs/mirrors" rc_is 1
check "(the message says so)" out_has "refs/mirrors"
check "(nothing was written)" not test -e "$w/prepared/prepared.tar"
end_case "gh-pages with a symbolic link under refs/mirrors: refused by the check itself, not by a later tar check"

# the same key (fingerprint) with other bytes: A renewed expiry or an added user id: the fingerprint is the same, the bytes clients imported are not
make_world 28
mkdir -m 700 "$keys/a2"
cp "$keys/a/pubring.kbx" "$keys/a2/"
cp -r "$keys/a/private-keys-v1.d" "$keys/a2/"
GNUPGHOME=$keys/a2 gpg --batch --pinentry-mode loopback --passphrase '' --quick-add-uid "$(cat "$keys/a/fpr")" "Kinefin second user id" > "$keys/a2/uid.log" 2>&1
GNUPGHOME=$keys/a2 gpg --batch --armor --export "$(cat "$keys/a/fpr")" > "$keys/a2/pub.asc" 2>> "$keys/a2/uid.log"
check "the changed key has the fingerprint of key A" same "$(GNUPGHOME=$keys/a2 gpg --batch --show-keys --with-colons "$keys/a2/pub.asc" 2> /dev/null | awk -F: '$1 == "fpr" { print $10; exit }')" "$(cat "$keys/a/fpr")"
check "(and other bytes)" not cmp -s "$keys/a/pub.asc" "$keys/a2/pub.asc"
build_jobs
SIGNING_PUBKEY=$keys/a2/pub.asc
job_prepare
SIGNING_PUBKEY=$keys/a/pub.asc
check "prepare refuses a public key file with the published fingerprint and other bytes" rc_is 2
check "(the message says byte for byte)" out_has "byte for byte"
check "(nothing was written)" not test -e "$w/prepared/prepared.tar"
# the same, one job later: finish compares the site files with the bytes of its own public key file
job_prepare
job_check
job_sign_key a
SIGNING_PUBKEY=$keys/a2/pub.asc
job_finish
SIGNING_PUBKEY=$keys/a/pub.asc
check "finish refuses a public key file with the same fingerprint and other bytes than the site" rc_is 1
check "(the message says byte for byte)" out_has "byte for byte"
check "(no site was written)" not test -e "$w/signed/site.tar"
# what CONTRIBUTING.md (Releases) tells to do: put the new bytes on gh-pages by hand, then the release goes through
git clone -q "file://$origin" "$w/pubx"
git -C "$w/pubx" checkout -q gh-pages
sed -i "s|^GPGKey=.*|GPGKey=$(gpg --batch --dearmor < "$keys/a2/pub.asc" | base64 -w0)|" "$w/pubx/kinefin.flatpakrepo" "$w/pubx/kinefin.flatpakref"
git -C "$w/pubx" commit -q -a -m "The renewed key"
git -C "$w/pubx" push -q origin gh-pages
base_sha=$(git -C "$w/pubx" rev-parse HEAD)
SIGNING_PUBKEY=$keys/a2/pub.asc
build_jobs
job_sign a
SIGNING_PUBKEY=$keys/a/pub.asc
check "with the new bytes on gh-pages, prepare, sign and finish go through" rc_is 0
check "(and the site has the new bytes)" same "$(tar -xOf "$w/signed/site.tar" ./kinefin.flatpakrepo | sed -n 's/^GPGKey=//p')" "$(gpg --batch --dearmor < "$keys/a2/pub.asc" | base64 -w0)"
end_case "same fingerprint, other key bytes: prepare and finish refuse until gh-pages carries the new bytes"

# an empty variable is said to be empty
make_world 29
mkdir "$w/s"
run env -u GPG_KEY_ID GPG_KEY_B64= "$repo_root/build/flatpak/publish-repo.sh" "$w/s" "$SITE_URL"
check "publish-repo.sh refuses an empty GPG_KEY_B64" rc_fails
check "(and says it is empty or not set)" out_has "GPG_KEY_B64 is empty or not set"
run env -u GPG_KEY_B64 "$repo_root/build/flatpak/write-site-files.sh" "$w/s" "$SITE_URL"
check "write-site-files.sh refuses a GPG_KEY_B64 that is not set" rc_fails
check "(and says it is empty or not set)" out_has "GPG_KEY_B64 is empty or not set"
check "(nothing was written)" test -z "$(find "$w/s" -mindepth 1)"
build_jobs
job_check
job_unpack
run env FLATPAK_GPG_PRIVATE_KEY= "$rel/sign-flatpak-repo.sh" --sign "$w/signwork" "$w/signed-repo"
check "--sign refuses an empty secret" rc_is 2
check "(and says it is empty or not available)" out_has "is empty or not available"
check "(no scratch directory is left)" no_leftovers
end_case "empty GPG_KEY_B64 and empty secret: refused, and the message says empty"

# dates of the new commits
snapshot_prepared() { rm -rf "$w/prepared.orig"; cp -a "$w/prepared" "$w/prepared.orig"; cp "$w/prepare.gho" "$w/prepare.gho.orig"; }
tamper_signed() { # <shell command run in the unpacked signed repository>: a compromised sign job
    rm -rf "$w/y"
    mkdir "$w/y"
    tar -xf "$w/signed-repo/signed-repo.tar" -C "$w/y"
    (cd "$w/y/repo" && eval "$1")
    tar -C "$w/y" -cf "$w/signed-repo/signed-repo.tar" repo
    sed -i '/^signed_sha256=/d' "$w/sign.gho"
    echo "signed_sha256=$(sha256_of "$w/signed-repo/signed-repo.tar")" >> "$w/sign.gho"
    rm -rf "$w/signed"
}
list_commit() { awk -F'\t' -v r="$1" '$1 == r { print $2 }' "$w/prepared.orig/sign-list.txt"; }

make_world 30
build_jobs
snapshot_prepared
app_ref=app/$app_id/x86_64/stable
app_commit=$(list_commit "$app_ref")
now=$(date +%s)
tamper_prepared "sed -i 's|^date=.*|date=$((now + 7200))|' $(objfile "$app_commit")"
job_check
check "--check refuses a new commit dated two hours ahead" rc_is 1
check "(the message says so)" out_has "is dated more than 3600s after now"
restore_prepared
tamper_prepared "sed -i 's|^date=.*|date=1000|' $(objfile "$app_commit")"
job_check
check "--check refuses a new commit older than the published head" rc_is 1
check "(the message says so)" out_has "is older than the published head"
restore_prepared
tamper_prepared "sed -i 's|^date=.*|date=$((now + 7200))|' $(objfile "$(list_commit appstream/x86_64)")"
job_check
check "--check refuses a new appstream commit dated two hours ahead" rc_is 1
check "(the message says so)" out_has "is dated more than"
restore_prepared
job_check
job_sign_key a
tamper_signed "sed -i 's|^date=.*|date=$((now + 7200))|' $(objfile "$app_commit")"
job_finish
check "finish refuses a signed commit dated two hours ahead" rc_is 1
check "(the message says so)" out_has "is dated more than 3600s after now"
# a build whose new commits are dated half an hour ahead (a commit object is its checksum, so it is built that way)
make_world 40
FAKE_OSTREE_DATE=$((now + 1800))
export FAKE_OSTREE_DATE
build_jobs
unset FAKE_OSTREE_DATE
job_check
check "--check accepts a new commit dated half an hour ahead (clock skew)" rc_is 0
end_case "commit dates: a new commit is not dated far ahead of now or before the head it continues, in --check and finish"

# the appstream commits that prepare makes
make_world 31
build_jobs
snapshot_prepared
as1=$(list_commit appstream/x86_64)
as2=$(list_commit appstream2/x86_64)
job_sign
check "an honest run goes through, with appstream commits of the expected shape" rc_is 0
tamper_prepared "echo 'keys=ostree.collection-binding ostree.ref-binding xa.ref' >> $(objfile "$as1")"
job_check
check "--check refuses an appstream commit with one more metadata key" rc_is 1
check "(the message says so)" out_has "other metadata keys"
restore_prepared
tamper_prepared "echo 'keys=ostree.ref-binding ostree.ref-binding ostree.collection-binding' >> $(objfile "$as1")"
job_check
check "--check refuses an appstream commit with a key twice" rc_is 1
check "(the message says so)" out_has "other metadata keys"
restore_prepared
tamper_prepared "echo 'keys=ostree.ref-binding' >> $(objfile "$as2")"
job_check
check "--check refuses an appstream2 commit without the collection binding" rc_is 1
check "(the message says so)" out_has "other metadata keys"
restore_prepared
tamper_prepared "echo 'subject=Something else' >> $(objfile "$as2")"
job_check
check "--check refuses an appstream commit with another subject" rc_is 1
check "(the message says so)" out_has "another subject or a body"
restore_prepared
tamper_prepared "echo 'message=a body' >> $(objfile "$as1")"
job_check
check "--check refuses an appstream commit with a body" rc_is 1
check "(the message says so)" out_has "another subject or a body"
restore_prepared
job_check
job_sign_key a
tamper_signed "echo 'subject=Something else' >> $(objfile "$as1")"
job_finish
check "finish refuses a signed appstream commit with another subject" rc_is 1
check "(the message says so)" out_has "another subject or a body"
end_case "appstream commits: exact metadata keys (sorted, duplicates count), subject Update, no body, in --check and finish"

# the top of repo/
make_world 33
build_jobs
snapshot_prepared
tamper_prepared "echo x > stray.txt"
job_check
check "--check refuses a file at the top of repo/ that ostree ignores" rc_is 1
check "(the message names it)" out_has "stray.txt"
restore_prepared
tamper_prepared "mkdir old-site"
job_check
check "--check refuses a directory at the top of repo/ that ostree does not know" rc_is 1
check "(the message says so)" out_has "entries at its top"
restore_prepared
job_sign
check "the honest run goes through (config objects refs summary... are allowed)" rc_is 0
end_case "repo/ has only the entries of an ostree repository at its top"

# a gh-pages older than the newest published release, and the first publication
fake_release() { # <tag> <draft true|false> <pre-release true|false> <name of the one asset, or - for none>: a release in the fake gh
    python3 -I - "$FAKE_GH_STATE" "$1" "$2" "$3" "$4" << 'PY'
import json
import sys

path, tag, draft, prerelease, asset = sys.argv[1:6]
with open(path) as f:
    state = json.load(f)
state["releases"] = [r for r in state["releases"] if r["tag_name"] != tag]
state["releases"].append({"id": 900 + len(state["releases"]), "tag_name": tag, "name": tag, "draft": draft == "true", "prerelease": prerelease == "true",
                          "body": "", "assets": [] if asset == "-" else [{"id": 800, "name": asset, "size": 1, "state": "uploaded"}]})
with open(path, "w") as f:
    json.dump(state, f)
PY
}
make_world 34
build_jobs
check "prepare accepts a gh-pages when no release is published" rc_is 0
fake_release v0.1.0 false true Kinefin-x86_64.flatpak
job_prepare
check "prepare accepts a gh-pages at the newest published release, a pre-release" rc_is 0
check "(and says which)" out_has "the newest published release is v0.1.0"
fake_release v0.2.1 false true Kinefin-x86_64.flatpak
job_prepare
check "a published pre-release with the bundle is an anchor: a gh-pages older than it is refused" rc_is 1
check "(the message says it was rolled back)" out_has "rolled back or never updated"
check "(nothing was written)" not test -e "$w/prepared/prepared.tar"
fake_release v0.2.1 false false Kinefin-x86_64.flatpak
job_prepare
check "a published release that is not a pre-release is an anchor too" rc_is 1
check "(the message says it was rolled back)" out_has "rolled back or never updated"
fake_release v0.2.1 false true -
job_prepare
check "prepare ignores a published release without files (made by hand, never on Pages)" rc_is 0
fake_release v0.2.1 true true Kinefin-x86_64.flatpak
job_prepare
check "prepare ignores a draft" rc_is 0
fake_release v0.2.1 false true Kinefin-x86_64.flatpak
# the old record of gh-pages is what a pusher writes: the subject of the head commit says it is current
git clone -q "file://$origin" "$w/pubx"
git -C "$w/pubx" checkout -q gh-pages
git -C "$w/pubx" commit -q --allow-empty -m "Flatpak repository for v0.2.1"
git -C "$w/pubx" push -q origin gh-pages
base_sha=$(git -C "$w/pubx" rev-parse HEAD)
build_jobs
check "prepare refuses a rolled-back gh-pages whose head commit says it is current" rc_is 1
check "(the version comes from the signed app: the message names it)" out_has "the app of gh-pages is at 0.1.0, but v0.2.1 is published"
fake_release v0.2.1 false true -
job_prepare
check "(without the bundle on the release it is accepted again)" rc_is 0
# GH_TOKEN is for gh alone
: > "$FAKE_OSTREE_LOG"
: > "$FAKE_FLATPAK_LOG"
fake_release v0.1.0 false true Kinefin-x86_64.flatpak
job_prepare
check "prepare lists the releases with the token" rc_is 0
check "(ostree never saw the token)" not file_has "$FAKE_OSTREE_LOG" "SAW GH_TOKEN"
check "(flatpak never saw it)" not file_has "$FAKE_FLATPAK_LOG" "SAW GH_TOKEN"
# the first publication: no gh-pages, so no release may be on Pages already
make_world 36
git -C "$origin" update-ref -d refs/heads/gh-pages
rm -rf "$w/pages" "$w/commits" "$w/dist"
mkdir "$w/pages"
ostree --repo="$w/pages/repo" fake-commit "app/$app_id/x86_64/stable" "first 0.2.0" > /dev/null
run "$rel/export-flatpak-commits.sh" "$w/pages" "$w/commits"
fake_release v0.1.0 false true Kinefin-x86_64.flatpak
job_prepare
check "prepare refuses a first publication when a release with the bundle exists, a pre-release too" rc_is 1
check "(the message says gh-pages is gone)" out_has "there is no gh-pages, but v0.1.0 is published"
fake_release v0.1.0 false true -
job_prepare
check "(a release without files does not stop the first publication)" rc_is 0
end_case "a gh-pages older than the newest published release (with the bundle, a pre-release too) is refused, a draft or a release without files is no anchor"

# a published repository from the time of static deltas: The real gh-pages has deltas/ and delta-indexes/ (names with a plus sign, as base64 names have). The release has none
make_world 35
git clone -q "file://$origin" "$w/pubx"
git -C "$w/pubx" checkout -q gh-pages
mkdir -p "$w/pubx/repo/deltas/ab/cd+ef-gh_ij/0" "$w/pubx/repo/delta-indexes/ab"
echo x > "$w/pubx/repo/deltas/ab/cd+ef-gh_ij/superblock"
echo x > "$w/pubx/repo/deltas/ab/cd+ef-gh_ij/0/part"
echo x > "$w/pubx/repo/delta-indexes/ab/cd+ef.index"
git -C "$w/pubx" add -A
git -C "$w/pubx" commit -q -m "Static deltas of an earlier release"
git -C "$w/pubx" push -q origin gh-pages
base_sha=$(git -C "$w/pubx" rev-parse HEAD)
build_jobs
job_sign
check "a release goes through on a gh-pages that has static deltas and delta indexes" rc_is 0
check "(the site has neither)" test -z "$(tar -tf "$w/signed/site.tar" | grep -i delta)"
end_case "a base with static deltas and delta indexes (names with a plus sign): both are removed before anything checks the repository"

# every file of the repository is an ostree file; the collection binding is empty
make_world 37
build_jobs
snapshot_prepared
as1=$(list_commit appstream/x86_64)
for stray in "mkdir -p state && echo x > state/stray" "mkdir -p extensions && echo x > extensions/stray" "mkdir -p objects/ab && echo x > objects/ab/stray.txt" "mkdir -p summaries && echo x > summaries/stray"; do
    tamper_prepared "$stray"
    job_check
    check "--check refuses a file that is no ostree file: $stray" rc_is 1
    check "(the message says so)" out_has "not the files of an ostree repository"
    restore_prepared
done
tamper_prepared "echo 'collection=org.example.Collection' >> $(objfile "$as1")"
job_check
check "--check refuses an appstream commit bound to a collection" rc_is 1
check "(the message says so)" out_has "bound to a collection"
restore_prepared
job_sign
check "the honest run goes through the same checks, in --check and in finish" rc_is 0
end_case "files of the repository: only ostree objects, refs, summary files and the config; the collection binding of an appstream commit is empty"

# prepare: the keyless job that finishes the repository unsigned
make_world 20
build_jobs
job_prepare
check "prepare exits 0" rc_is 0
check "(and leaves no scratch directory behind)" no_leftovers
check "it writes the prepared site and the list" test -s "$w/prepared/prepared.tar" -a -s "$w/prepared/sign-list.txt"
check "it reports the hashes and the base" file_has "$w/prepare.gho" "^prepared_sha256=$(sha256_of "$w/prepared/prepared.tar")$"
check "(the hash of the list too)" file_has "$w/prepare.gho" "^signlist_sha256=$(sha256_of "$w/prepared/sign-list.txt")$"
check "(and the base)" file_has "$w/prepare.gho" "^base_sha=$base_sha$"
check "the list has the ref of the build and the two appstream refs" same "$(cut -f1 "$w/prepared/sign-list.txt" | tr '\n' ' ')" "app/$app_id/x86_64/stable appstream/x86_64 appstream2/x86_64 "
check "the commits of the build are the listed ones" same "$(grep -c "^app/.*	$(awk -F'\t' '$1 ~ /^app\// { print $2 }' "$w/commits/refs.txt")$" "$w/prepared/sign-list.txt")" 1
check "the prepared site has the repository and the files users add it with" prepared_has "site/repo/summary"
check "(the .flatpakref too)" prepared_has "site/kinefin.flatpakref"
check "the git metadata of the clone is not in it" not prepared_has "site/.git/HEAD"
check "the summary is not signed yet" not prepared_has "site/repo/summary.sig"
check "the new commits are not signed" not prepared_has "site/repo/objects/$(awk -F'\t' '$1 ~ /^app\// { print $2 }' "$w/commits/refs.txt").commitmeta"
check "the GPGKey is byte-identical to the published one" same "$(prepared_file site/kinefin.flatpakrepo | sed -n 's/^GPGKey=//p')" "$(sed -n 's/^GPGKey=//p' "$site/kinefin.flatpakrepo")"
check "(the log says so)" out_has "the GPGKey is the published one"
# a secret in the environment: refused before anything is read
use_secret a
rm -rf "$w/prepared"
run "$rel/prepare-flatpak-repo.sh" "$w/commits/flatpak-commits.tar" "$w/prepared"
drop_secret
check "prepare refuses to run with the secret in its environment" rc_is 2
check "(the message says so)" out_has "must run without the secret"
check "(nothing was written)" not test -e "$w/prepared/prepared.tar"
check "(no scratch directory is left)" no_leftovers
# changed inputs
job_prepare
saved=$EXPECT_COMMITS_SHA256
EXPECT_COMMITS_SHA256=0
run env GITHUB_OUTPUT="$w/prepare.gho" "$rel/prepare-flatpak-repo.sh" "$w/commits/flatpak-commits.tar" "$w/prepared2"
check "prepare refuses commits that are not the ones the build job reported" rc_is 1
EXPECT_COMMITS_SHA256=$saved
EXPECT_REFS_SHA256=0
run env GITHUB_OUTPUT="$w/prepare.gho" "$rel/prepare-flatpak-repo.sh" "$w/commits/flatpak-commits.tar" "$w/prepared2"
check "prepare refuses a refs.txt that is not the one the build job reported" rc_is 1
check "(the message names it)" out_has "refs.txt of the commits archive"
EXPECT_REFS_SHA256=$(sha256_of "$w/commits/refs.txt")
mkdir "$w/evil"
tar -xf "$w/commits/flatpak-commits.tar" -C "$w/evil"
ln -s /etc/passwd "$w/evil/repo/objects/link"
tar -C "$w/evil" -cf "$w/commits-evil.tar" repo refs.txt
EXPECT_COMMITS_SHA256=$(sha256_of "$w/commits-evil.tar")
run env GITHUB_OUTPUT="$w/prepare.gho" "$rel/prepare-flatpak-repo.sh" "$w/commits-evil.tar" "$w/prepared2"
check "prepare refuses a commits archive with a symlink" rc_is 2
check "(the message says plain files)" out_has "not a plain file"
rm -f "$w/evil/repo/objects/link"
echo surprise > "$w/evil/notes.txt"
tar -C "$w/evil" -cf "$w/commits-evil.tar" repo refs.txt notes.txt
EXPECT_COMMITS_SHA256=$(sha256_of "$w/commits-evil.tar")
run env GITHUB_OUTPUT="$w/prepare.gho" "$rel/prepare-flatpak-repo.sh" "$w/commits-evil.tar" "$w/prepared2"
check "prepare refuses a commits archive with other files" rc_is 2
check "(no scratch directory is left)" no_leftovers
EXPECT_COMMITS_SHA256=$saved
export EXPECT_COMMITS_SHA256 EXPECT_REFS_SHA256
SIGNING_PUBKEY=$keys/b/pub.asc
job_prepare
SIGNING_PUBKEY=$keys/a/pub.asc
check "prepare refuses a published GPGKey that is not the key of the public key file" rc_is 2
check "(the message says clients would stop updating)" out_has "installed clients would stop updating"
check "(nothing was written)" not test -e "$w/prepared/prepared.tar"
with_sha 0000000000000000000000000000000000000001 job_prepare
check "prepare refuses a checkout that is not the commit of the run" rc_is 1
git clone -q "file://$origin" "$w/other"
git -C "$w/other" checkout -q gh-pages
echo other > "$w/other/other.txt"
git -C "$w/other" add -A
git -C "$w/other" commit -q -m "Another release"
git -C "$w/other" push -q origin gh-pages
job_prepare
check "prepare refuses when gh-pages moved after the build (stale base)" rc_is 3
check "(no scratch directory is left)" no_leftovers
# the first release: no gh-pages yet, so the GPGKey is made from the public key file
make_world 21
git -C "$origin" update-ref -d refs/heads/gh-pages
rm -rf "$w/pages" "$w/commits" "$w/dist"
mkdir "$w/pages"
ostree --repo="$w/pages/repo" fake-commit "app/$app_id/x86_64/stable" "first 0.2.0" > /dev/null
run "$rel/export-flatpak-commits.sh" "$w/pages" "$w/commits"
job_prepare
check "prepare accepts the first release" rc_is 0
check "(and reports no base)" file_has "$w/prepare.gho" "^base_sha=-$"
check "(it says the GPGKey is made from the public key file)" out_has "there was no published .flatpakrepo"
check "the GPGKey is the key of the public key file" same "$(prepared_file site/kinefin.flatpakrepo | sed -n 's/^GPGKey=//p' | base64 -d | gpg --batch --show-keys --with-colons 2> /dev/null | awk -F: '$1 == "fpr" { print $10; exit }')" "$(cat "$keys/a/fpr")"
check "(and the .flatpakref has it too)" same "$(prepared_file site/kinefin.flatpakref | sed -n 's/^GPGKey=//p')" "$(prepared_file site/kinefin.flatpakrepo | sed -n 's/^GPGKey=//p')"
end_case "prepare: unsigned site and list, hashes, GPGKey kept; refuses the secret, wrong hashes, links, other files, another key, stale base"

# the secret reaches nothing but the sign step; tampered inputs are refused
make_world 22
build_jobs
cp -a "$w/prepared" "$w/prepared.orig"
cp "$w/prepare.gho" "$w/prepare.gho.orig"
unpack_prepared() { rm -rf "$w/x"; mkdir "$w/x"; tar -xf "$w/prepared.orig/prepared.tar" -C "$w/x"; }
pack_prepared() { tar -C "$w/x" -cf "$w/prepared/prepared.tar" site; }
app_ref=app/$app_id/x86_64/stable
app_commit=$(awk -F'\t' -v r="$app_ref" '$1 == r { print $2 }' "$w/commits/refs.txt")

# the secret in the environment of the steps that read data of other jobs
use_secret a
job_check
drop_secret
check "--check refuses to run with the secret in its environment" rc_is 2
check "(the message says so)" out_has "must run without the secret"
check "(nothing was written)" not test -e "$w/checked/checked-repo.tar"

# no child process of the sign step sees the secret, and the step does what it should and no more
rt=$work/shim
mkdir -p "$rt/bin"
for t in flatpak gpg ostree git; do
    real=$(command -v "$t")
    # shellcheck disable=SC2016 # the shim expands its own variables
    printf '#!/bin/sh\necho "%s $*" >> "%s/calls"\nprintenv FLATPAK_GPG_PRIVATE_KEY >> "%s/seen" || true\nexec %s "$@"\n' "$t" "$rt" "$rt" "$real" > "$rt/bin/$t"
    chmod +x "$rt/bin/$t"
done
with_shims() { saved_path=$PATH; PATH=$rt/bin:$PATH; export PATH; "$@"; PATH=$saved_path; export PATH; }
: > "$rt/seen"
: > "$rt/calls"
with_shims job_check
check "--check passes" rc_is 0
: > "$rt/seen"
: > "$rt/calls"
with_shims job_sign_key
check "--sign works" rc_is 0
check "the secret was seen by no child process" same "$(wc -c < "$rt/seen")" 0
for t in flatpak gpg ostree; do
    check "(the shim of $t was called: the check proves something)" test "$(grep -c "^$t " "$rt/calls")" -gt 0
done
check "the key was imported" file_has "$rt/calls" "^gpg .*--import"
check "the commits were signed with ostree gpg-sign" file_has "$rt/calls" "^ostree .*gpg-sign"
check "the summary was signed without appstream data" file_has "$rt/calls" "^flatpak build-update-repo --no-update-appstream --gpg-sign="
check "no static delta" not file_has "$rt/calls" "generate-static-deltas"
check "no prune" not file_has "$rt/calls" "prune"
check "no commit was made or pulled from the build" not file_has "$rt/calls" "build-commit-from\\|pull-local\\|fake-commit"
check "no git, no network in the step with the key" not file_has "$rt/calls" "^git "
check "(no keyring is left)" no_leftovers
use_secret a
job_finish
drop_secret
check "finish refuses to run with the secret in its environment" rc_is 2
check "(the message says so)" out_has "must run without the secret"
job_finish
check "finish accepts what sign made" rc_is 0
cp -a "$w/signed-repo" "$w/signed-repo.orig"
cp "$w/sign.gho" "$w/sign.gho.orig"

# a tampered prepared site or sign list
job_check
check "the untouched files pass the checks" rc_is 0
unpack_prepared
echo surprise > "$w/x/site/evil.txt"
pack_prepared
job_check
check "sign refuses a prepared site that is not the one prepare reported" rc_is 1
check "(the message says so)" out_has "not the one the earlier job produced"
check "(nothing was written)" not test -e "$w/checked/checked-repo.tar"
unpack_prepared
ln -s /etc/passwd "$w/x/site/repo/objects/link"
pack_prepared
rehash_prepared
job_check
check "sign refuses a prepared site with a symlink, whatever hash prepare reported" rc_is 2
check "(the message says plain files)" out_has "not a plain file"
restore_prepared
unpack_prepared
printf '%s\n' "$published_commit" > "$w/x/site/repo/refs/heads/$app_ref"
pack_prepared
rehash_prepared
job_check
check "sign refuses a repository whose listed ref is not at the listed commit" rc_is 1
check "(the message names the ref)" out_has "is not at the listed commit"
restore_prepared
echo "app/other.app/x86_64/stable	$app_commit" >> "$w/prepared/sign-list.txt"
job_check
check "sign refuses a sign list that is not the one prepare reported" rc_is 1
rehash_prepared
job_check
check "sign refuses a sign list with a ref that is not in refs.txt" rc_is 2
check "(the message says so)" out_has "bad ref"
restore_prepared
awk -F'\t' -v r="$app_ref" -v c="$published_commit" 'BEGIN { OFS = "\t" } $1 == r { $2 = c } { print }' "$w/prepared.orig/sign-list.txt" > "$w/prepared/sign-list.txt"
rehash_prepared
job_check
check "sign refuses a sign list with another commit than the build made" rc_is 2
check "(the message says so)" out_has "another commit than the build made"
restore_prepared
grep -v "^$app_ref" "$w/prepared.orig/sign-list.txt" > "$w/prepared/sign-list.txt"
rehash_prepared
job_check
check "sign refuses a sign list that lacks a ref of the build" rc_is 2
check "(the message says so)" out_has "of the build is not on it"
restore_prepared
sed -i 's/^base_sha=.*/base_sha=0000000000000000000000000000000000000001/' "$w/prepare.gho"
job_check
check "sign refuses a base that is not the one refs.txt records" rc_is 2
restore_prepared
job_check
EXPECT_REFS_SHA256=0
run "$rel/sign-flatpak-repo.sh" --check "$w/prepared/prepared.tar" "$w/prepared/sign-list.txt" "$w/commits/refs.txt" "$w/checked"
check "sign refuses a refs.txt that is not the one the build job reported" rc_is 1

# a signed repository that is not the prepared one plus signatures
job_check
job_sign_key
restore_signed() { rm -rf "$w/signed-repo"; cp -a "$w/signed-repo.orig" "$w/signed-repo"; cp "$w/sign.gho.orig" "$w/sign.gho"; }
rehash_signed() { sed -i '/^signed_sha256=/d' "$w/sign.gho"; echo "signed_sha256=$(sha256_of "$w/signed-repo/signed-repo.tar")" >> "$w/sign.gho"; }
unpack_signed() { rm -rf "$w/y"; mkdir "$w/y"; tar -xf "$w/signed-repo.orig/signed-repo.tar" -C "$w/y"; }
pack_signed() { tar -C "$w/y" -cf "$w/signed-repo/signed-repo.tar" repo; rehash_signed; }
restore_signed
job_finish
check "finish accepts the signed repository" rc_is 0
unpack_signed
echo surprise >> "$w/y/repo/refs/heads/$app_ref"
tar -C "$w/y" -cf "$w/signed-repo/signed-repo.tar" repo
job_finish
check "finish refuses a signed repository that is not the one sign reported" rc_is 1
check "(the message says so)" out_has "not the one the earlier job produced"
restore_signed
unpack_signed
mkdir -p "$w/y/repo/refs/heads/app/evil.app/x86_64"
printf '%s\n' "$app_commit" > "$w/y/repo/refs/heads/app/evil.app/x86_64/stable"
pack_signed
job_finish
check "finish refuses an extra ref" rc_is 1
check "(the message says so)" out_has "other refs or commits"
restore_signed
unpack_signed
printf '%s\n' "$published_commit" > "$w/y/repo/refs/heads/$app_ref"
pack_signed
job_finish
check "finish refuses a ref moved to another commit" rc_is 1
check "(the message says so)" out_has "other refs or commits"
restore_signed
unpack_signed
echo "<script>alert(1)</script>" > "$w/y/repo/evil.html"
pack_signed
job_finish
check "finish refuses a stray file in the repository" rc_is 1
check "(the message names the file)" out_has "evil.html"
restore_signed
unpack_signed
obj=$(find "$w/y/repo/objects" -name '*.commit' | head -n 1)
echo changed >> "$obj"
pack_signed
job_finish
check "finish refuses a changed object" rc_is 1
check "(the message says so)" out_has "differs from the prepared one"
restore_signed
unpack_signed
rm "$w/y/repo/objects/$(printf '%s' "$app_commit" | cut -c1-2)/${app_commit#??}.commitmeta"
pack_signed
job_finish
check "finish refuses a listed commit that is not signed" rc_fails
check "(the client check says so)" out_has "not signed by a key"
restore_signed
unpack_signed
rm "$w/y/repo/summary.sig"
pack_signed
job_finish
check "finish refuses an unsigned summary" rc_fails
restore_signed
unpack_signed
astream=$(cat "$w/y/repo/refs/heads/appstream/x86_64")
rm "$w/y/repo/objects/$(printf '%s' "$astream" | cut -c1-2)/${astream#??}.commitmeta"
pack_signed
job_finish
check "finish refuses an appstream commit that is not signed (Flatpak itself does not notice)" rc_fails
check "(the pull with signature checks says so)" out_has "not signed by a key"
restore_signed
unpack_signed
ln -s /etc/passwd "$w/y/repo/link"
pack_signed
job_finish
check "finish refuses a symlink in the signed repository" rc_is 2
check "(the message says plain files)" out_has "not a plain file"
restore_signed
unpack_prepared
printf '[Flatpak Repo]\nGPGKey=%s\n' "$(gpg --batch --dearmor < "$keys/b/pub.asc" | base64 -w0)" > "$w/x/site/kinefin.flatpakrepo"
pack_prepared
rehash_prepared
job_finish
check "finish refuses a GPGKey that is not the key of the public key file" rc_is 1
check "(the message says so)" out_has "byte for byte"
restore_prepared
unpack_prepared
k=$({ gpg --batch --dearmor < "$keys/a/pub.asc"; printf '\250\003PGP'; } | base64 -w0)
sed -i "s|^GPGKey=.*|GPGKey=$k|" "$w/x/site/kinefin.flatpakrepo" "$w/x/site/kinefin.flatpakref"
pack_prepared
rehash_prepared
job_finish
check "finish refuses the same key with other bytes (an extra marker packet)" rc_is 1
check "(the message says so)" out_has "byte for byte"
restore_prepared
job_finish
check "and accepts everything again once the files are the real ones" rc_is 0
end_case "split signing: the secret reaches only the sign step; tampered prepared site, sign list, refs, signed repository, stray file, object, signature and key are refused"

# check-flatpak and sign-flatpak share only the check's output
make_world 38
build_jobs
job_check
check "the check passes" rc_is 0
check "(it reports the hash of the checked repository)" test -n "$(out_of "$w/check.gho" checked_sha256)"
check "(its copy of the sign list is the one prepare made)" same "$(sha256_of "$w/checked/sign-list.txt")" "$(out_of "$w/prepare.gho" signlist_sha256)"
cp -a "$w/checked" "$w/checked.orig"
cp "$w/check.gho" "$w/check.gho.orig"
orig_pin=$(out_of "$w/check.gho" checked_sha256)
list_pin=$(out_of "$w/prepare.gho" signlist_sha256)
refs_pin=$(sha256_of "$w/commits/refs.txt")
heads_pin=$(out_of "$w/prepare.gho" heads_sha256)
rt=$work/shim3
mkdir -p "$rt/bin"
for t in flatpak gpg ostree git curl tar; do
    real=$(command -v "$t" || true)
    if [ -n "$real" ]; then
        # shellcheck disable=SC2016 # the shim expands its own variables
        printf '#!/bin/sh\necho "%s $*" >> "%s/calls"\nexec %s "$@"\n' "$t" "$rt" "$real" > "$rt/bin/$t"
    else
        # shellcheck disable=SC2016 # the shim expands its own variables
        printf '#!/bin/sh\necho "%s $*" >> "%s/calls"\necho "%s is not installed here" >&2\nexit 127\n' "$t" "$rt" "$t" > "$rt/bin/$t"
    fi
    chmod +x "$rt/bin/$t"
done
# The sign job has the files and hashes named above and nothing else: no prepared site, no commits archive, no gh-pages
mv "$w/prepared" "$w/prepared.away"
mv "$w/commits/flatpak-commits.tar" "$w/flatpak-commits.tar.away"
saved_remote=$PAGES_REMOTE
PAGES_REMOTE=file://$w/no-such-remote.git
export PAGES_REMOTE
: > "$rt/calls"
with_shims job_unpack
check "unpack works from the check's archive, refs.txt and the three hashes, with no remote and no prepared site" rc_is 0
check "(the repository is unpacked)" test -d "$w/signwork/repo/objects"
check "(and the sign list)" test -s "$w/signwork/sign-list.txt"
check "(the shim of tar was called: the check proves something)" file_has "$rt/calls" "^tar "
check "unpack ran no ostree, flatpak, gpg, git or curl" not file_has "$rt/calls" "^\\(ostree\\|flatpak\\|gpg\\|git\\|curl\\) "
check "(no keyring is left)" no_leftovers
: > "$rt/calls"
with_shims job_sign_step
check "the step with the secret works on the same files and the same absence" rc_is 0
check "it called ostree rev-parse and gpg-sign, once per listed ref each" same "$(grep -c '^ostree .* \(rev-parse\|gpg-sign\) ' "$rt/calls")" "$(($(wc -l < "$w/checked/sign-list.txt") * 2))"
check "(and the summary signing)" file_has "$rt/calls" "^flatpak build-update-repo --no-update-appstream --gpg-sign="
check "(and the key import)" file_has "$rt/calls" "^gpg .*--import"
check "and nothing else: no git, no network, no check of the repository" not grep -Evq '^(ostree --repo=[^ ]+ (rev-parse|gpg-sign) |flatpak build-update-repo --no-update-appstream --gpg-sign=|gpg |tar )' "$rt/calls"
check "(no keyring is left)" no_leftovers
PAGES_REMOTE=$saved_remote
export PAGES_REMOTE
mv "$w/prepared.away" "$w/prepared"
mv "$w/flatpak-commits.tar.away" "$w/commits/flatpak-commits.tar"
job_finish
check "finish, pinned to prepare and sign as before, accepts what the sign job made" rc_is 0

# a checked repository or sign list replaced after the check: the pins are the outputs of the check job
rm -rf "$w/y"
mkdir "$w/y"
tar -xf "$w/checked.orig/checked-repo.tar" -C "$w/y"
echo evil > "$w/y/repo/evil.txt"
tar -C "$w/y" -cf "$w/checked/checked-repo.tar" repo
: > "$rt/calls"
rm -rf "$w/signwork"
with_shims unpack_pinned "$orig_pin" "$list_pin"
check "unpack refuses a checked repository that is not the one the check reported (the record re-hashed, the pin not)" rc_is 1
check "(the message says so)" out_has "not the one the earlier job produced"
check "(it failed before tar read the archive)" not file_has "$rt/calls" "^tar "
check "(nothing was extracted)" not test -e "$w/signwork"
cp "$w/checked.orig/checked-repo.tar" "$w/checked/checked-repo.tar"
echo "app/other.app/x86_64/stable	$(awk -F'\t' 'NR == 1 { print $2 }' "$w/checked/sign-list.txt")" >> "$w/checked/sign-list.txt"
unpack_pinned "$orig_pin" "$list_pin"
check "unpack refuses a sign list that is not the one the check reported" rc_is 1
check "(nothing was extracted)" not test -e "$w/signwork"
cp "$w/checked.orig/sign-list.txt" "$w/checked/sign-list.txt"

# the artifact of prepare given to unpack
: > "$rt/calls"
with_shims sign_side EXPECT_CHECKED_SHA256="$orig_pin" EXPECT_SIGNLIST_SHA256="$list_pin" EXPECT_REFS_SHA256="$refs_pin" EXPECT_HEADS_SHA256="$heads_pin" "$rel/sign-flatpak-repo.sh" --unpack "$w/prepared/prepared.tar" "$w/prepared/sign-list.txt" "$w/commits/refs.txt" "$w/signwork"
check "unpack refuses the prepared site in place of the checked repository" rc_is 1
check "(the message says so)" out_has "not the one the earlier job produced"
check "(nothing was extracted)" not test -e "$w/signwork"
sign_side EXPECT_CHECKED_SHA256="$(sha256_of "$w/prepared/prepared.tar")" EXPECT_SIGNLIST_SHA256="$list_pin" EXPECT_REFS_SHA256="$refs_pin" EXPECT_HEADS_SHA256="$heads_pin" "$rel/sign-flatpak-repo.sh" --unpack "$w/prepared/prepared.tar" "$w/prepared/sign-list.txt" "$w/commits/refs.txt" "$w/signwork"
check "unpack refuses the prepared site even under its own hash (it holds site/, not repo/)" rc_is 2
check "(the message names the folder)" out_has "other than repo/"
check "(nothing was extracted)" not test -e "$w/signwork"

# a pin that is empty, not set or not a hash
unpack_pinned "" "$list_pin"
check "unpack refuses an empty pin of the checked repository" rc_is 2
check "(nothing was extracted)" not test -e "$w/signwork"
unpack_pinned "$orig_pin" ""
check "unpack refuses an empty pin of the sign list" rc_is 2
check "(nothing was extracted)" not test -e "$w/signwork"
sign_side -u EXPECT_CHECKED_SHA256 -u EXPECT_SIGNLIST_SHA256 -u EXPECT_REFS_SHA256 -u EXPECT_HEADS_SHA256 "$rel/sign-flatpak-repo.sh" --unpack "$w/checked/checked-repo.tar" "$w/checked/sign-list.txt" "$w/commits/refs.txt" "$w/signwork"
check "unpack refuses pins that are not set" rc_is 2
check "(nothing was extracted)" not test -e "$w/signwork"
unpack_pinned "$orig_pin" "$list_pin" ""
check "unpack refuses an empty pin of refs.txt" rc_is 2
check "(nothing was extracted)" not test -e "$w/signwork"
unpack_pinned "$orig_pin" "$list_pin" "$refs_pin" ""
check "unpack refuses an empty pin of the heads" rc_is 2
check "(nothing was extracted)" not test -e "$w/signwork"
unpack_pinned "not-a-hash" "$list_pin"
check "unpack refuses a pin that is not a hash" rc_is 2
check "(nothing was extracted)" not test -e "$w/signwork"
unpack_pinned "$orig_pin" "$list_pin" "$orig_pin"
check "unpack refuses a refs.txt that is not the one the build reported" rc_is 1
check "(the message says so)" out_has "not the one the earlier job produced"
check "(nothing was extracted)" not test -e "$w/signwork"

# a compromised check job: it writes what it likes and reports the hash of it, but the pins of the sign list and refs.txt are those of prepare and build, and the key never signs a commit that is not the build's
refused_unpack() { # <what> <exit code> <message>: the tree in $w/y goes back as the archive of the check, with its new hash
    tar -C "$w/y" -cf "$w/checked/checked-repo.tar" repo
    unpack_pinned "$(sha256_of "$w/checked/checked-repo.tar")" "$list_pin"
    check "unpack refuses $1" rc_is "$2"
    check "(the message says so)" out_has "$3"
    check "(nothing was extracted)" not test -e "$w/signwork"
}
fresh_y() { rm -rf "$w/y"; mkdir "$w/y"; tar -xf "$w/checked.orig/checked-repo.tar" -C "$w/y"; }
app_ref=app/$app_id/x86_64/stable
app_c=$(awk -F'\t' -v r="$app_ref" '$1 == r { print $2 }' "$w/commits/refs.txt")
evil_c=$(printf evil | sha256sum | cut -d' ' -f1)
fresh_y
awk -F'\t' -v c="$app_c" -v e="$evil_c" 'BEGIN { OFS = "\t" } $2 == c { $2 = e } { print }' "$w/checked.orig/sign-list.txt" > "$w/checked/sign-list.txt"
refused_unpack "a sign list of its own, with the hash of its archive (the pin of the list is prepare's)" 1 "not the one the earlier job produced"
cp "$w/checked.orig/sign-list.txt" "$w/checked/sign-list.txt"
fresh_y
printf '%s\n' "$published_commit" > "$w/y/repo/refs/heads/$app_ref"
refused_unpack "a ref that points to another commit than the listed one" 1 "not the ones prepare-flatpak made"
fresh_y
echo '# changed' >> "$w/y/repo/$(objfile "$app_c")"
refused_unpack "a commit object that does not hash to its id" 1 "do not hash to their names"
fresh_y
rm "$w/y/repo/$(objfile "$app_c")"
refused_unpack "an archive without a listed commit object" 1 "is not in the checked repository"
fresh_y
echo surprise > "$w/y/repo/summary"
refused_unpack "a repository that already has a summary" 1 "summary is made again"
fresh_y
printf '[remote "evil"]\nurl=file:///x\n' >> "$w/y/repo/config"
refused_unpack "a config with a remote" 2 "not the expected ones"
fresh_y
mkdir "$w/y/repo/deltas"
refused_unpack "static deltas" 1 "static deltas"
fresh_y
echo "<script></script>" > "$w/y/repo/evil.html"
refused_unpack "a stray file at the top of repo/" 1 "entries at its top"
fresh_y
mkdir -p "$w/y/repo/refs/heads/app/evil.app/x86_64"
printf '%s\n' "$app_c" > "$w/y/repo/refs/heads/app/evil.app/x86_64/stable"
refused_unpack "a ref of another app, which is no file of this repository" 1 "not the files of an ostree repository"
fresh_y
app_parent=$(awk -F'\t' -v r="$app_ref" '$1 == r { print $3 }' "$w/commits/refs.txt")
echo '# changed' >> "$w/y/repo/$(objfile "$app_parent")"
refused_unpack "an unlisted commit object that does not hash to its name" 1 "do not hash to their names"
fresh_y
touch "$w/y/repo/objects/$(printf '%s' "$app_c" | cut -c1-2)/${app_c#??}.commitmeta"
refused_unpack "a signature file of a listed commit" 1 "signature file already"
fresh_y
check "the honest archive has .commitmeta2 files, as flatpak build-update-repo writes them (xa.reachable)" tar_has "$w/checked.orig/checked-repo.tar" 'repo/objects/.*commitmeta2'
printf 'crafted\n' >> "$w/y/repo/objects/$(printf '%s' "$app_c" | cut -c1-2)/${app_c#??}.commitmeta2"
refused_unpack "a .commitmeta2 that was changed (flatpak parses it with the key at hand)" 1 "not the ones prepare-flatpak made"
fresh_y
mkdir -p "$w/y/repo/objects/00"
cp "$w/y/repo/objects/$(printf '%s' "$app_c" | cut -c1-2)/${app_c#??}.commitmeta2" "$w/y/repo/objects/00/$(printf 'a%.0s' $(seq 62)).commitmeta2"
refused_unpack "a .commitmeta2 that was added" 1 "not the ones prepare-flatpak made"
fresh_y
rm "$w/y/repo/objects/$(printf '%s' "$app_c" | cut -c1-2)/${app_c#??}.commitmeta2"
refused_unpack "a .commitmeta2 that was dropped" 1 "not the ones prepare-flatpak made"
fresh_y
printf '%s\t%s\n' "$app_ref" "$app_c" >> "$w/y/repo/refs/heads/$app_ref"
refused_unpack "a ref file that holds more than a commit id" 1 "cannot list the refs"
cp "$w/checked.orig/checked-repo.tar" "$w/checked/checked-repo.tar"

# a symlink member under a matching pin
rm -rf "$w/y"
mkdir "$w/y"
tar -xf "$w/checked.orig/checked-repo.tar" -C "$w/y"
ln -s /etc/passwd "$w/y/repo/objects/link"
tar -C "$w/y" -cf "$w/checked/checked-repo.tar" repo
unpack_pinned "$(sha256_of "$w/checked/checked-repo.tar")" "$list_pin"
check "unpack refuses a symlink member under a matching pin" rc_is 2
check "(the message says plain files)" out_has "not a plain file"
check "(nothing was extracted)" not test -e "$w/signwork"
cp "$w/checked.orig/checked-repo.tar" "$w/checked/checked-repo.tar"
use_secret a
unpack_pinned "$orig_pin" "$list_pin"
drop_secret
check "unpack refuses to run with the secret in its environment" rc_is 2
check "(nothing was extracted)" not test -e "$w/signwork"
unpack_pinned "$orig_pin" "$list_pin"
check "and unpacks the real files again" rc_is 0
SIGNING_PUBKEY=/dev/null
job_check
SIGNING_PUBKEY=$keys/a/pub.asc
check "--check refuses a public key file it cannot read, before anyone is asked to approve" rc_is 2
check "(the message says so)" out_has "cannot read the fingerprint"
check "(nothing was written)" not test -e "$w/checked/checked-repo.tar"
end_case "check and sign share only the check's output: unpack runs no ostree, flatpak, gpg, git or network; a replaced archive or list, prepare's artifact, empty pins and a symlink are refused before tar"

# gh-pages moved since the build: the check job refuses, long before the key is imported
make_world 23
build_jobs
git clone -q "file://$origin" "$w/other"
git -C "$w/other" checkout -q gh-pages
echo other > "$w/other/other.txt"
git -C "$w/other" add -A
git -C "$w/other" commit -q -m "Another release"
git -C "$w/other" push -q origin gh-pages
rt=$work/shim2
mkdir -p "$rt/bin"
for t in flatpak gpg ostree git; do
    real=$(command -v "$t")
    # shellcheck disable=SC2016 # the shim expands its own variables
    printf '#!/bin/sh\necho "%s $*" >> "%s/calls"\nexec %s "$@"\n' "$t" "$rt" "$real" > "$rt/bin/$t"
    chmod +x "$rt/bin/$t"
done
: > "$rt/calls"
with_shims job_sign
check "the check exits 3 when gh-pages is not where the build left it" rc_is 3
check "(the message says stale base)" out_has "stale base"
check "the key was never imported" not file_has "$rt/calls" "--import"
check "the repository was not touched" not file_has "$rt/calls" "gpg-sign\\|build-update-repo"
check "gh-pages did ask: git ls-remote ran" file_has "$rt/calls" "^git ls-remote"
check "(no keyring is left)" no_leftovers
end_case "stale base: the check job refuses, so the sign job is never approved; the key was never imported, nothing was signed"

# a build that adds another ref than the app ref (a runtime, another app, another arch or branch) is refused by prepare and by both sign steps
make_world 42
build_jobs
job_check
cp -a "$w/commits" "$w/commits.honest"
app_c=$(awk -F'\t' -v r="app/$app_id/x86_64/stable" '$1 == r { print $2 }' "$w/commits/refs.txt")
for foreign in runtime/org.example.Other/x86_64/stable app/org.example.Other/x86_64/stable "app/$app_id/x86_64/beta" "app/$app_id/aarch64/stable"; do
    rm -rf "$w/fx" "$w/prepared-foreign"
    mkdir "$w/fx"
    tar -xf "$w/commits.honest/flatpak-commits.tar" -C "$w/fx"
    printf '%s\t%s\t-\n' "$foreign" "$app_c" >> "$w/fx/refs.txt"
    tar -C "$w/fx" -cf "$w/commits-foreign.tar" repo refs.txt
    run env EXPECT_COMMITS_SHA256="$(sha256_of "$w/commits-foreign.tar")" EXPECT_REFS_SHA256="$(sha256_of "$w/fx/refs.txt")" "$rel/prepare-flatpak-repo.sh" "$w/commits-foreign.tar" "$w/prepared-foreign"
    check "prepare refuses a build that adds $foreign" rc_is 2
    check "(the message says so)" out_has "bad ref"
    check "(nothing was written)" not test -e "$w/prepared-foreign/prepared.tar"
    # the sign steps get that refs.txt in place of the honest one, with its hash as the pin
    cp "$w/fx/refs.txt" "$w/commits/refs.txt"
    job_check
    check "--check refuses a refs.txt with $foreign" rc_is 2
    check "(the message says so)" out_has "bad ref"
    cp "$w/commits.honest/refs.txt" "$w/commits/refs.txt"
    job_check
    cp "$w/fx/refs.txt" "$w/commits/refs.txt"
    unpack_pinned "$(out_of "$w/check.gho" checked_sha256)" "$(out_of "$w/prepare.gho" signlist_sha256)"
    check "--unpack refuses a refs.txt with $foreign" rc_is 2
    check "(the message says so)" out_has "bad ref"
    check "(nothing was extracted)" not test -e "$w/signwork"
    cp "$w/commits.honest/refs.txt" "$w/commits/refs.txt"
done
end_case "foreign refs: a runtime, another app, another branch or arch in refs.txt is refused by prepare, --check and --unpack, and the sign list and the repository accept the app ref only"

# finish builds the signed bundle without installing it; check-flatpak-bundle.sh installs it as a user would, and a key or a commit that is not the pinned one is refused
make_world 41
build_jobs
job_sign
check "finish exits 0" rc_is 0
bundle=$w/signed/Kinefin-x86_64.flatpak
app_ref=app/$app_id/x86_64/stable
listed_commit=$(awk -F'\t' -v r="$app_ref" '$1 == r { print $2 }' "$w/prepared/sign-list.txt")
check "finish wrote the bundle" test -s "$bundle"
check "it was built for x86_64 with the key and the Flathub runtime repository, into scratch space" file_has "$FAKE_FLATPAK_LOG" "^build-bundle --arch=x86_64 --gpg-keys=[^ ]* --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo [^ ]* $RUNNER_TEMP/finish\.[^/]*/bundle-out/Kinefin-x86_64.flatpak $app_id stable \["
check "finish did not install it" not file_has "$FAKE_FLATPAK_LOG" "^install .*--bundle"
job_bundle
check "check-flatpak-bundle accepts the honest bundle" rc_is 0
check "(and says what it checked)" out_has "the bundle installs with only its embedded key, at the listed app commit $listed_commit"
check "it installed the bundle into a scratch user installation, deploying it" file_has "$FAKE_FLATPAK_LOG" "^install --user --noninteractive -y --no-deps --no-related --bundle $bundle \[FLATPAK_USER_DIR=$RUNNER_TEMP/bundle\.[^/]*/client\]$"
check "(and asked flatpak for the installed commit)" file_has "$FAKE_FLATPAK_LOG" "^info --user --show-commit $app_id \[FLATPAK_USER_DIR=$RUNNER_TEMP/bundle\.[^/]*/client\]$"
check "(and pulled the installed commit with signature checks into a scratch repository)" file_has "$FAKE_OSTREE_LOG" "^pull-local --remote=pinned --gpg-verify $RUNNER_TEMP/bundle\.[^/]*/client/repo $listed_commit$"
check "it writes no GITHUB_OUTPUT" not test -e "$w/bundle.gho"
check "(and leaves the output of finish as it was, and no scratch directory)" same "$(find "$w/signed" -type f -printf '%f\n' | LC_ALL=C sort | tr '\n' ' ') $(sha256_of "$bundle") $(no_leftovers && echo clean)" "Kinefin-x86_64.flatpak site.tar  $(out_of "$w/finish.gho" flatpak_sha256) clean"
rm -rf "$w/user"
mkdir "$w/user"
run env FLATPAK_USER_DIR="$w/user" flatpak install --user -y --no-deploy --no-deps --bundle "$bundle"
user_remote=$(ostree --repo="$w/user/repo" remote list)
check "the bundle carries the listed app commit, the one the build made" same "$(ostree --repo="$w/user/repo" rev-parse "$user_remote:$app_ref") $listed_commit" "$(awk -F'\t' -v r="$app_ref" '$1 == r { print $2 }' "$w/commits/refs.txt") $listed_commit"
bundle_refused() { # <what> <message>: the check ran with a bundle or a knob that is not the honest one; the caller resets the knobs
    check "check-flatpak-bundle refuses $1" rc_is 1
    check "(the message says so)" out_has "$2"
    check "(it writes no GITHUB_OUTPUT)" not test -e "$w/bundle.gho"
    check "(and leaves no scratch directory)" no_leftovers
}
FAKE_FLATPAK_BUNDLE_KEY=$keys/b/pub.asc
job_finish
FAKE_FLATPAK_BUNDLE_KEY=
check "finish does not install the bundle, so it writes one with another key" rc_is 0
job_bundle
bundle_refused "a bundle with another key than the one that signed the commit (flatpak refuses it)" "does not install with only the key embedded in it"
FAKE_FLATPAK_BUNDLE_NOSIG=1
job_finish
FAKE_FLATPAK_BUNDLE_NOSIG=
FAKE_FLATPAK_INSTALL_NO_VERIFY=1
job_bundle
FAKE_FLATPAK_INSTALL_NO_VERIFY=
bundle_refused "a bundle whose commit carries no signature, though the client accepted it" "is not signed by the pinned key"
FAKE_FLATPAK_BUNDLE_COMMIT=$published_commit
job_finish
FAKE_FLATPAK_BUNDLE_COMMIT=
job_bundle
bundle_refused "a bundle with a signed commit other than the listed one" "installs the commit $published_commit, not the listed app commit $listed_commit"
job_finish
job_bundle
check "check-flatpak-bundle accepts the honest run again" rc_is 0
end_case "signed bundle: finish does not install it, check-flatpak-bundle does and writes nothing; it carries the listed commit; another key, no signature and another commit are refused"

# check-flatpak-bundle trusts neither the bundle nor refs.txt until their hashes match what finish and build-flatpak reported
bundle=$w/signed/Kinefin-x86_64.flatpak
cp "$bundle" "$w/tampered.flatpak"
printf '\0' >> "$w/tampered.flatpak"
job_bundle "$(out_of "$w/finish.gho" flatpak_sha256)" "$(sha256_of "$w/commits/refs.txt")" "$w/tampered.flatpak"
check "check-flatpak-bundle refuses a bundle whose hash differs from EXPECT_FLATPAK_SHA256, though flatpak would install it" rc_is 1
check "(the message says so)" out_has "the bundle is not the one the earlier job produced"
check "(flatpak was never called)" test ! -s "$FAKE_FLATPAK_LOG"
check "(and nothing was written)" not test -e "$w/bundle.gho"
job_bundle "$(sha256_of "$w/tampered.flatpak")" "$(sha256_of "$w/commits/refs.txt")" "$w/tampered.flatpak"
check "(the tampered bundle does install, so the hash was the only thing against it)" rc_is 0
job_bundle 0
check "check-flatpak-bundle refuses an EXPECT_FLATPAK_SHA256 that is not the hash of the bundle" rc_is 1
check "(flatpak was never called)" test ! -s "$FAKE_FLATPAK_LOG"
job_bundle "$(out_of "$w/finish.gho" flatpak_sha256)" 0
check "check-flatpak-bundle refuses an EXPECT_REFS_SHA256 that is not the hash of refs.txt" rc_is 1
check "(the message names refs.txt)" out_has "refs.txt is not the one the earlier job produced"
check "(flatpak was never called)" test ! -s "$FAKE_FLATPAK_LOG"
run env -u EXPECT_FLATPAK_SHA256 "$rel/check-flatpak-bundle.sh" "$bundle" "$w/commits/refs.txt"
check "check-flatpak-bundle refuses to run without EXPECT_FLATPAK_SHA256" rc_fails
run env EXPECT_FLATPAK_SHA256="$(sha256_of "$bundle")" EXPECT_REFS_SHA256="$(sha256_of "$w/commits/refs.txt")" "$rel/check-flatpak-bundle.sh" "$bundle"
check "check-flatpak-bundle refuses a call with one argument" rc_is 2
use_secret a
job_bundle
drop_secret
check "check-flatpak-bundle refuses to run with the secret in its environment" rc_is 2
check "(the message says so)" out_has "must run without the secret"
check "(flatpak was never called)" test ! -s "$FAKE_FLATPAK_LOG"
check "(and no scratch directory is left)" no_leftovers
end_case "check-flatpak-bundle: a bundle that is not the one finish hashed, a refs.txt that is not the build's, a missing hash, a missing argument and the secret are refused before flatpak runs"

# the first release: there is no gh-pages yet, up to finish
make_world 18
git -C "$origin" update-ref -d refs/heads/gh-pages
base_sha=-
# What the build job does without a gh-pages: an empty directory instead of the clone
rm -rf "$w/pages" "$w/commits"
mkdir "$w/pages"
ostree --repo="$w/pages/repo" fake-commit "app/$app_id/x86_64/stable" "first 0.2.0" > /dev/null
run "$rel/export-flatpak-commits.sh" "$w/pages" "$w/commits"
check "the export records no base" file_has "$w/commits/refs.txt" "^base	-$"
job_prepare
job_sign
check "sign accepts the first release" rc_is 0
check "(and reports no base)" file_has "$w/sign.out" "built on gh-pages -"
check "finish wrote the site and the bundle" test -s "$w/signed/site.tar" -a -s "$w/signed/Kinefin-x86_64.flatpak"
job_bundle
check "check-flatpak-bundle accepts the bundle of the first release" rc_is 0
check "the site has the summary and its signature" tar -tf "$w/signed/site.tar" ./repo/summary ./repo/summary.sig > /dev/null
end_case "first release: no gh-pages, base -: prepare, check, sign and finish accept it"

# no secret anywhere
leaks=$keys/leaks
: > "$leaks"
for k in a b; do
    cat "$keys/$k/grips" >> "$leaks"
    sed -n '3p' "$keys/$k/secret.asc" >> "$leaks"
done
sed -i '/^$/d' "$leaks"
echo "-----BEGIN PGP PRIVATE KEY BLOCK" >> "$leaks"
check "the key material to look for exists" test "$(wc -l < "$leaks")" -ge 5
check "no private key block, keygrip or key material in any output, log or artifact" not grep -rqaF -f "$leaks" --exclude-dir=keys "$work"
check "no keyring or scratch directory is left in any RUNNER_TEMP" test -z "$(find "$work"/w*/runner-temp -mindepth 1 -maxdepth 1)"
check "the log of everything is not empty" test -s "$work/all.log"
end_case "no private key block, keygrip or key material in outputs, logs, artifacts; no keyring left"

echo "# $passed/$total passed"
[ "$failed" -eq 0 ] && [ "$n" -eq "$total" ]
