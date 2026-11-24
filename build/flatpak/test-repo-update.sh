#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Drives the release scripts with a real flatpak and ostree, throw-away keys and a local web server, to prove that installed clients still update and that the signed bundle installs
usage() {
    cat <<'EOF'
usage: test-repo-update.sh [<built repository>]
  <built repository> is the OSTree repository build-flatpak.sh leaves (default dist/flatpak-repo), read only
  1. release 1 is made as the first release is, on an empty pages directory: export-flatpak-commits.sh, prepare-flatpak-repo.sh, sign-flatpak-repo.sh (--check, --unpack, --sign), finish-flatpak-repo.sh; it is pushed as gh-pages and a client installs it from its .flatpakrepo
  2. release 2 goes through the same scripts on top of release 1 (a changed tree and changed appstream data); a gh-pages with a ref moved under its signed summary is refused
  3. the same URL serves a release 2 whose commit is signed by another key: the update is refused on the signature; then the real one: the update reaches the new commit, whose parent is the installed one, without static deltas
  4. the signed bundle of finish-flatpak-repo.sh goes through check-flatpak-bundle.sh, as the check-bundle job runs it: it installs into a user installation that knows no remote, at the listed commit, signed by the pinned key; a bundle with the right key embedded passes, one with another key embedded and one whose commit is signed by another key (with that key embedded) are refused
  5. ostree pull-local --gpg-verify, the check of check-flatpak-bundle.sh, with a remote that knows only the pinned key, refuses an unsigned commit and a commit signed by another key on the signature, and accepts the listed one
  6. the served .flatpakref installs with a real client (its Homepage, Icon and RuntimeRepo fields are accepted)
  the built repository must have no object over 100 MiB, GitHub's limit for a push
  needs flatpak, ostree, gpg, git, python3, curl and diffutils (cmp, diff), no D-Bus, no bwrap, no network and no real key; CI runs it on ubuntu-24.04
exit: 0 every assertion passed, 1 an assertion failed or a tool is missing, 2 bad arguments
EOF
}
set -eu
case ${1:-} in -h | --help) usage; exit 0 ;; esac
[ $# -le 1 ] || { usage >&2; exit 2; }
for tool in flatpak ostree gpg git python3 curl cmp diff; do
    command -v "$tool" > /dev/null || { echo "FAIL: $tool is needed" >&2; exit 1; }
done
# the scripts under test append to these in a job, and this test must not write into the step outputs of the job that runs it
unset GITHUB_OUTPUT GITHUB_ENV GITHUB_PATH
root=$(cd "$(dirname "$0")/../.." && pwd)
built=$(readlink -f "${1:-$root/dist/flatpak-repo}")
id=io.github.the_beper.Kinefin
ref=app/$id/x86_64/stable
fail() { echo "FAIL: $*" >&2; exit 1; }
[ -d "$built/objects" ] || fail "$built is not an OSTree repository (build with build/flatpak/build-flatpak.sh first)"
ostree --repo="$built" rev-parse "$ref" > /dev/null || fail "$built has no $ref"
largest=$(find "$built/objects" -type f -printf '%s %p\n' | sort -n | tail -n 1)
echo "largest object of $built: ${largest%% *} bytes (${largest#* })"
# GitHub refuses a pushed file over 100 MiB: gh-pages could not take the repository
[ -z "$(find "$built/objects" -type f -size +100M)" ] || fail "$built has an object over 100 MiB, which GitHub does not accept in a push"

work=$(mktemp -d)
server=
cleanup() {
    set +e
    [ -z "$server" ] || kill "$server" 2> /dev/null
    for home in "$work"/key-*; do
        [ -d "$home" ] && gpgconf --homedir "$home" --kill gpg-agent 2> /dev/null
    done
    chmod -R u+w "$work" 2> /dev/null
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
HOME=$work/home
export HOME
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null
export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.invalid GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.invalid
mkdir -p "$HOME"

# the saved log of a refused command must carry the signature error, as ostree 2024.5 and 2026.4 and flatpak 1.14.6 and 1.18.4 word it
refused_on_signature() { # refused_on_signature LOG WHAT WORDING
    grep -qF "$3" "$1" || { cat "$1" >&2; fail "$2 was refused, but not with \"$3\""; }
}

# two keys: the repository's (A) and somebody else's (B)
for k in a b; do
    mkdir -m 700 "$work/key-$k"
    GNUPGHOME=$work/key-$k gpg --batch --quiet --pinentry-mode loopback --passphrase '' --quick-gen-key "Kinefin test $k" ed25519 sign never
    GNUPGHOME=$work/key-$k gpg --batch --list-keys --with-colons | awk -F: '$1 == "fpr" { print $10; exit }' > "$work/key-$k/fpr"
    GNUPGHOME=$work/key-$k gpg --batch --armor --export "$(cat "$work/key-$k/fpr")" > "$work/key-$k/public.asc"
    gpg --batch --dearmor < "$work/key-$k/public.asc" > "$work/key-$k/public.gpg"
done
fpr_a=$(cat "$work/key-a/fpr")
fpr_b=$(cat "$work/key-b/fpr")

port=$(python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
url=http://127.0.0.1:$port/current
mkdir "$work/www"

# gh-pages as a git remote with no branch yet, and a one-commit repository that stands in for the checkout the scripts look at
git init -q --bare --initial-branch=gh-pages "$work/origin.git"
git init -q --initial-branch=main "$work/checkout"
git -C "$work/checkout" commit -q --allow-empty -m "Release"
sha=$(git -C "$work/checkout" rev-parse HEAD)
export GITHUB_SHA="$sha" KINEFIN_ROOT="$work/checkout" PAGES_REMOTE="file://$work/origin.git" SITE_URL="$url" RUNNER_TEMP="$work" SIGNING_PUBKEY="$work/key-a/public.asc"
export GITHUB_REPOSITORY=example/Kinefin PATH="$work/bin:$PATH"
mkdir "$work/bin"
# prepare lists the published releases with gh, which has no repository here: the answer is a file, rewritten before release 2
printf '#!/bin/sh\ncat "%s/gh.json"\n' "$work" > "$work/bin/gh"
chmod +x "$work/bin/gh"
echo '[]' > "$work/gh.json"

# one release the way the workflow makes it: the export of the build job, then prepare, check, sign (the key only in its own step), finish and the check of the bundle
release() { # release N PAGES-DIR: the commits of PAGES-DIR/repo end up as $work/rN/site, the unpacked site, and $work/rN/signed/Kinefin-x86_64.flatpak, which passed check-flatpak-bundle.sh
    r=$work/r$1
    mkdir "$r" "$r/ci"
    "$root/build/release/export-flatpak-commits.sh" "$2" "$r/commits"
    base=$(sed -n 's/^base\t//p' "$r/commits/refs.txt")
    refs_sha=$(sha256sum "$r/commits/refs.txt" | cut -d' ' -f1)
    EXPECT_COMMITS_SHA256=$(sha256sum "$r/commits/flatpak-commits.tar" | cut -d' ' -f1) EXPECT_REFS_SHA256=$refs_sha \
        GITHUB_OUTPUT=$r/ci/prepare.out "$root/build/release/prepare-flatpak-repo.sh" "$r/commits/flatpak-commits.tar" "$r/prepared"
    prepared_sha=$(sed -n 's/^prepared_sha256=//p' "$r/ci/prepare.out")
    signlist_sha=$(sed -n 's/^signlist_sha256=//p' "$r/ci/prepare.out")
    heads_sha=$(sed -n 's/^heads_sha256=//p' "$r/ci/prepare.out")
    EXPECT_PREPARED_SHA256=$prepared_sha EXPECT_SIGNLIST_SHA256=$signlist_sha EXPECT_BASE_SHA=$base EXPECT_REFS_SHA256=$refs_sha EXPECT_HEADS_SHA256=$heads_sha \
        GITHUB_OUTPUT=$r/ci/check.out "$root/build/release/sign-flatpak-repo.sh" --check "$r/prepared/prepared.tar" "$r/prepared/sign-list.txt" "$r/commits/refs.txt" "$r/checked"
    EXPECT_CHECKED_SHA256=$(sed -n 's/^checked_sha256=//p' "$r/ci/check.out") EXPECT_SIGNLIST_SHA256=$signlist_sha EXPECT_REFS_SHA256=$refs_sha EXPECT_HEADS_SHA256=$heads_sha \
        "$root/build/release/sign-flatpak-repo.sh" --unpack "$r/checked/checked-repo.tar" "$r/checked/sign-list.txt" "$r/commits/refs.txt" "$r/signwork"
    FLATPAK_GPG_PRIVATE_KEY=$(GNUPGHOME=$work/key-a gpg --batch --pinentry-mode loopback --passphrase '' --armor --export-secret-keys "$fpr_a") \
        GITHUB_OUTPUT=$r/ci/sign.out "$root/build/release/sign-flatpak-repo.sh" --sign "$r/signwork" "$r/signed-repo"
    EXPECT_PREPARED_SHA256=$prepared_sha EXPECT_SIGNLIST_SHA256=$signlist_sha EXPECT_BASE_SHA=$base EXPECT_REFS_SHA256=$refs_sha \
        EXPECT_SIGNED_SHA256=$(sed -n 's/^signed_sha256=//p' "$r/ci/sign.out") GITHUB_OUTPUT=$r/ci/finish.out \
        "$root/build/release/finish-flatpak-repo.sh" "$r/prepared/prepared.tar" "$r/prepared/sign-list.txt" "$r/signed-repo/signed-repo.tar" "$r/commits/refs.txt" "$r/signed"
    EXPECT_FLATPAK_SHA256=$(sed -n 's/^flatpak_sha256=//p' "$r/ci/finish.out") EXPECT_REFS_SHA256=$refs_sha \
        "$root/build/release/check-flatpak-bundle.sh" "$r/signed/Kinefin-x86_64.flatpak" "$r/commits/refs.txt"
    mkdir "$r/site"
    tar -xf "$r/signed/site.tar" -C "$r/site"
}

# release 1: the build exports its commit into an empty pages directory, as for the first release
mkdir "$work/pages1"
ostree --repo="$work/pages1/repo" init --mode=archive-z2
flatpak build-commit-from --src-repo="$built" --src-ref="$ref" --no-update-summary --subject="update test 1" "$work/pages1/repo" "$ref"
release 1 "$work/pages1"
s1=$work/r1/site
c1=$(ostree --repo="$s1/repo" rev-parse "$ref")
echo "ok: release 1 went through the pipeline on an empty pages directory, commit $c1, and its bundle passed check-flatpak-bundle.sh"

# publishing: gh-pages gets the site, and the build job of release 2 clones it
git -C "$s1" init -q --initial-branch=gh-pages
git -C "$s1" add --all
git -C "$s1" commit -q -m "Release 1"
git -C "$s1" push -q "$work/origin.git" gh-pages
git clone -q --depth 1 --branch gh-pages "file://$work/origin.git" "$work/pages"
# a clone lacks the empty directories of the repository
mkdir -p "$work/pages/repo/refs/heads" "$work/pages/repo/refs/mirrors" "$work/pages/repo/refs/remotes" "$work/pages/repo/tmp"

# release 2: the build job exports a commit with another tree on top of gh-pages
ostree --repo="$built" checkout --user-mode "$ref" "$work/tree"
chmod -R u+w "$work/tree"
mkdir -p "$work/tree/files/share/kinefin"
echo "repository update test" > "$work/tree/files/share/kinefin/update-test"
# every release changes the appstream data, so build-update-repo makes new unsigned appstream commits that the sign list must carry
for xml in "$work"/tree/files/share/app-info/xmls/*.xml.gz; do
    [ -f "$xml" ] || continue
    gzip -dc "$xml" | sed 's|<summary>[^<]*</summary>|<summary>update test 2</summary>|' | gzip -n > "$xml.new"
    mv "$xml.new" "$xml"
done
# without the sandbox for the icon check: it needs user namespaces, which CI runners may restrict
flatpak build-export --disable-sandbox "$work/changed" "$work/tree" stable
flatpak build-commit-from --src-repo="$work/changed" --src-ref="$ref" --no-update-summary --subject="update test 2" "$work/pages/repo" "$ref"
# the rollback anchor: when the built app has a release in its metainfo, release 1 is a published release with the bundle, else there is none
version=$(ostree --repo="$built" cat "$ref" "/files/share/metainfo/$id.metainfo.xml" | awk '/<release / && !d { v = $0; sub(/.*version="/, "", v); sub(/".*/, "", v); print v; d = 1 }') || true
if [ -n "$version" ]; then
    printf '[{"tag_name":"v%s","draft":false,"prerelease":false,"assets":[{"name":"Kinefin-x86_64.flatpak","state":"uploaded"}]}]\n' "$version" > "$work/gh.json"
    echo "note: the metainfo has a release, so the fake gh lists v$version with the bundle for release 2 (anchor: gh-pages must not be older)"
else
    echo "note: the metainfo has no release, so the fake gh lists none for release 2 (no anchor)"
fi
release 2 "$work/pages"
s2=$work/r2/site
c2=$(ostree --repo="$s2/repo" rev-parse "$ref")
[ "$c2" != "$c1" ] || fail "the new commit is the old one"
[ "$(ostree --repo="$s2/repo" rev-parse "$c2^")" = "$c1" ] || fail "the new commit does not have the old one as its parent"
grep -q '^appstream' "$work/r2/prepared/sign-list.txt" || fail "the sign list of release 2 has no appstream line"
echo "ok: release 2 went through the pipeline, commit $c2 on top of $c1, with appstream commits to sign, and its bundle passed check-flatpak-bundle.sh"

# a gh-pages as an attacker could write it, refs/heads/X moved to another commit and the signed summary left as it was
# ostree takes the commit of a ref from the verified summary, so a pull alone would not notice: prepare compares the refs
mkdir -p "$work/evil-tree/files"
echo evil > "$work/evil-tree/files/evil"
for variant in unsigned other-key; do
    git clone -q --bare "file://$work/origin.git" "$work/origin-$variant.git"
    git clone -q --depth 1 --branch gh-pages "file://$work/origin-$variant.git" "$work/bad-$variant"
    mkdir -p "$work/bad-$variant/repo/refs/heads" "$work/bad-$variant/repo/refs/mirrors" "$work/bad-$variant/repo/refs/remotes" "$work/bad-$variant/repo/tmp"
    if [ "$variant" = unsigned ]; then signing=; else signing="--gpg-sign=$fpr_b --gpg-homedir=$work/key-b"; fi
    # shellcheck disable=SC2086 # the options have no spaces
    ostree --repo="$work/bad-$variant/repo" commit --branch="$ref" --tree=dir="$work/evil-tree" --no-xattrs --subject="moved ref ($variant)" $signing > /dev/null
    git -C "$work/bad-$variant" add --all
    git -C "$work/bad-$variant" commit -q -m "ref moved, summary unchanged"
    git -C "$work/bad-$variant" push -q "file://$work/origin-$variant.git" HEAD:gh-pages
    # the build job exports on top of that gh-pages, as it would
    git clone -q --depth 1 --branch gh-pages "file://$work/origin-$variant.git" "$work/pages-$variant"
    mkdir -p "$work/pages-$variant/repo/refs/heads" "$work/pages-$variant/repo/refs/mirrors" "$work/pages-$variant/repo/refs/remotes" "$work/pages-$variant/repo/tmp"
    flatpak build-commit-from --src-repo="$work/changed" --src-ref="$ref" --no-update-summary --subject="update test 3" "$work/pages-$variant/repo" "$ref"
    "$root/build/release/export-flatpak-commits.sh" "$work/pages-$variant" "$work/commits-$variant" > /dev/null
    if EXPECT_COMMITS_SHA256=$(sha256sum "$work/commits-$variant/flatpak-commits.tar" | cut -d' ' -f1) EXPECT_REFS_SHA256=$(sha256sum "$work/commits-$variant/refs.txt" | cut -d' ' -f1) \
        PAGES_REMOTE=file://$work/origin-$variant.git "$root/build/release/prepare-flatpak-repo.sh" "$work/commits-$variant/flatpak-commits.tar" "$work/prepared-$variant" > "$work/prepare-$variant.log" 2>&1; then
        cat "$work/prepare-$variant.log"
        fail "prepare accepted a gh-pages whose ref was moved under its signed summary ($variant)"
    fi
    grep -q "not the refs and commits of its signed summary" "$work/prepare-$variant.log" || { cat "$work/prepare-$variant.log"; fail "prepare refused ($variant) for another reason"; }
    [ ! -e "$work/prepared-$variant/prepared.tar" ] || fail "prepare wrote its output after refusing ($variant)"
done
echo "ok: a gh-pages with a ref moved under its signed summary is refused (commit unsigned, or signed by another key)"

# release 2 whose new commit is signed by somebody else and whose summary is signed by the published key, so that only the signature of the commit is wrong
# the signature file of the published key is removed, not gpg-sign --delete, which does not match a fingerprint in every ostree
cp -a "$s2" "$work/s3"
rm "$work/s3/repo/objects/$(printf '%s' "$c2" | cut -c1-2)/$(printf '%s' "$c2" | cut -c3-).commitmeta"
ostree --repo="$work/s3/repo" gpg-sign --gpg-homedir="$work/key-b" "$c2" "$fpr_b"
flatpak build-update-repo --gpg-sign="$fpr_a" --gpg-homedir="$work/key-a" "$work/s3/repo" > /dev/null

# the server and the client
python3 -I -m http.server "$port" --bind 127.0.0.1 --directory "$work/www" > "$work/http.log" 2>&1 &
server=$!
serve() { ln -sfn "$1" "$work/www/current"; }
serve "$s1"
for _ in $(seq 20); do curl -fsS -o /dev/null "$url/kinefin.flatpakrepo" 2> /dev/null && break; sleep 0.5; done
curl -fsS -o /dev/null "$url/kinefin.flatpakrepo" || fail "the test server does not answer"

FLATPAK_USER_DIR=$work/client
export FLATPAK_USER_DIR
flatpak remote-add --user kinefin "$url/kinefin.flatpakrepo"
flatpak install --user --noninteractive -y --no-deps --no-related kinefin "$ref"
installed() { flatpak info --user --show-commit "$id"; }
[ "$(installed)" = "$c1" ] || fail "the installed commit is $(installed), not $c1"
echo "ok: a client installed release 1 from its .flatpakrepo"

update() { flatpak update --user --noninteractive -y --no-deps --no-related "$id"; }

serve "$work/s3"
if update > "$work/update-s3.log" 2>&1; then cat "$work/update-s3.log"; fail "the update to a commit signed by another key succeeded"; fi
[ "$(installed)" = "$c1" ] || { cat "$work/update-s3.log"; fail "a commit signed by another key was installed"; }
refused_on_signature "$work/update-s3.log" "the update to a commit signed by another key" "Can't check signature: public key not found"
echo "ok: a commit signed by another key is refused, on its signature"

serve "$s2"
before=$(wc -l < "$work/http.log")
update || fail "the update to the repository signed the new way failed"
[ "$(installed)" = "$c2" ] || fail "the installed commit is $(installed), not $c2"
if tail -n +"$((before + 1))" "$work/http.log" | grep -q '/repo/deltas/'; then fail "the client asked for a static delta, and the site has none"; fi
echo "ok: the client updated from $c1 to $c2 (child of $c1), without static deltas"

# bundles made by hand, each through the script of the check-bundle job (a user installation of its own that knows no remote), with the hash of the bundle and the refs.txt of release 2
bundle=$work/r2/signed/Kinefin-x86_64.flatpak
[ -f "$bundle" ] || fail "finish-flatpak-repo.sh wrote no bundle"
bundle_check() { # bundle_check BUNDLE
    EXPECT_FLATPAK_SHA256=$(sha256sum "$1" | cut -d' ' -f1) EXPECT_REFS_SHA256=$(sha256sum "$work/r2/commits/refs.txt" | cut -d' ' -f1) \
        "$root/build/release/check-flatpak-bundle.sh" "$1" "$work/r2/commits/refs.txt"
}
# flatpak deploys a bundle even with --no-deploy, which is why check-flatpak-bundle.sh has a job of its own
FLATPAK_USER_DIR=$work/bundle-nodeploy flatpak install --user --noninteractive -y --no-deps --no-related --no-deploy --bundle "$bundle" > "$work/bundle-nodeploy.log" 2>&1 || { cat "$work/bundle-nodeploy.log"; fail "the signed bundle does not install with --no-deploy and only the key embedded in it"; }
if [ -d "$work/bundle-nodeploy/app/$id" ]; then deployed=deployed; else deployed="not deployed"; fi
echo "note: --no-deploy --bundle leaves the app $deployed (flatpak $(flatpak --version | cut -d' ' -f2)); check-flatpak-bundle.sh runs in a job that holds no write, signing or attestation permission either way"

# the same command with key A embedded passes, with key B it is refused by flatpak on the signature
flatpak build-bundle --arch=x86_64 --gpg-keys="$work/key-a/public.gpg" "$s2/repo" "$work/bundle-a.flatpak" "$id" stable
bundle_check "$work/bundle-a.flatpak" > "$work/bundle-a-check.log" 2>&1 || { cat "$work/bundle-a-check.log"; fail "a bundle with the right key embedded does not pass check-flatpak-bundle.sh"; }
flatpak build-bundle --arch=x86_64 --gpg-keys="$work/key-b/public.gpg" "$s2/repo" "$work/bundle-b.flatpak" "$id" stable
if bundle_check "$work/bundle-b.flatpak" > "$work/bundle-b-check.log" 2>&1; then
    cat "$work/bundle-b-check.log"
    fail "a bundle with another key embedded passed check-flatpak-bundle.sh"
fi
grep -qF "does not install with only the key embedded in it" "$work/bundle-b-check.log" || { cat "$work/bundle-b-check.log"; fail "check-flatpak-bundle.sh refused a bundle with another key embedded, but not on the install"; }
refused_on_signature "$work/bundle-b-check.log" "a bundle with another key embedded" "GPG signatures found, but none are in trusted keyring"
# a commit signed by key B with key B embedded installs (the bundle is consistent), and only the pinned key refuses it
flatpak build-bundle --arch=x86_64 --gpg-keys="$work/key-b/public.gpg" "$work/s3/repo" "$work/bundle-s3.flatpak" "$id" stable
if bundle_check "$work/bundle-s3.flatpak" > "$work/bundle-s3-check.log" 2>&1; then
    cat "$work/bundle-s3-check.log"
    fail "a bundle whose commit is signed by another key passed check-flatpak-bundle.sh"
fi
grep -qF "is not signed by the pinned key" "$work/bundle-s3-check.log" || { cat "$work/bundle-s3-check.log"; fail "check-flatpak-bundle.sh refused a commit signed by another key, but not as unsigned by the pinned key"; }
echo "ok: check-flatpak-bundle.sh passes a bundle with the signer's key embedded, refuses one with another key embedded on its signature, and one whose commit the pinned key did not sign"

# the check check-flatpak-bundle.sh relies on: pull-local with the signature checks on, into a repository whose only remote knows the pinned key
pinned_repo() {
    ostree --repo="$1" init --mode=archive
    ostree --repo="$1" remote add --set=gpg-verify=true --gpg-import="$work/key-a/public.gpg" pinned "file://$work/nowhere"
}
c0=$(ostree --repo="$built" rev-parse "$ref")
c3=$(ostree --repo="$work/s3/repo" rev-parse "$ref")
[ "$c3" = "$c2" ] || fail "the signed-by-B release should carry the commit of release 2"
pinned_repo "$work/chk-unsigned"
if ostree --repo="$work/chk-unsigned" pull-local --remote=pinned --gpg-verify "$built" "$c0" > "$work/pull-unsigned.log" 2>&1; then
    cat "$work/pull-unsigned.log"
    fail "pull-local --gpg-verify accepted an unsigned commit"
fi
refused_on_signature "$work/pull-unsigned.log" "pull-local of an unsigned commit" "GPG verification enabled, but no signatures found"
if ostree --repo="$work/chk-unsigned" show "$c0" > /dev/null 2>&1; then fail "the refused unsigned commit was stored"; fi
# without --gpg-verify the same pull works, so the refusal above is the signature check
ostree --repo="$work/chk-unsigned" pull-local "$built" "$c0" > "$work/pull-control.log" 2>&1 || { cat "$work/pull-control.log"; fail "the control pull of the unsigned commit without --gpg-verify failed"; }
pinned_repo "$work/chk-other"
if ostree --repo="$work/chk-other" pull-local --remote=pinned --gpg-verify "$work/s3/repo" "$c3" > "$work/pull-other.log" 2>&1; then
    cat "$work/pull-other.log"
    fail "pull-local --gpg-verify accepted a commit signed by another key"
fi
refused_on_signature "$work/pull-other.log" "pull-local of a commit signed by another key" "Can't check signature: public key not found"
if ostree --repo="$work/chk-other" show "$c3" > /dev/null 2>&1; then fail "the refused commit of another key was stored"; fi
pinned_repo "$work/chk-ok"
ostree --repo="$work/chk-ok" pull-local --remote=pinned --gpg-verify "$s2/repo" "$c2" > "$work/pull-ok.log" 2>&1 || { cat "$work/pull-ok.log"; fail "pull-local --gpg-verify refused the listed signed commit"; }
ostree --repo="$work/chk-ok" show "$c2" > /dev/null || fail "the accepted commit was not stored"
echo "ok: pull-local --gpg-verify refuses an unsigned commit and a commit of another key, on the signature, and accepts the signed one"

# the .flatpakref the site serves, with a real client: it names the Homepage, the Icon, the RuntimeRepo and the key
FLATPAK_USER_DIR=$work/ref-client
export FLATPAK_USER_DIR
flatpak install --user --noninteractive -y --no-deps --no-related --from "$url/kinefin.flatpakref" > "$work/flatpakref.log" 2>&1 || { cat "$work/flatpakref.log"; fail "the served kinefin.flatpakref does not install"; }
[ "$(installed)" = "$c2" ] || fail "the .flatpakref installed $(installed), not $c2"
echo "ok: the served .flatpakref installs with a real client, at $c2"
echo "done"
