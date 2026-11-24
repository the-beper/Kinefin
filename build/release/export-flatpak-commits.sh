#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Takes the unsigned commit of the app ref out of a Flatpak build into a fresh repository for the signing jobs, which hold no build output
usage() {
    cat >&2 <<'EOF'
usage: export-flatpak-commits.sh <pages-dir> <out-dir>
  <pages-dir>  a clone of gh-pages (or an empty directory for the first release) that build-flatpak.sh exported its build into (KINEFIN_FLATPAK_REPO=<pages-dir>/repo)
  <out-dir>    receives repo/ (only app/io.github.the_beper.Kinefin/x86_64/stable, copied with its checksum unchanged), refs.txt and flatpak-commits.tar
  refs.txt has "base TAB <commit of gh-pages the build started from, or - when there was none>" and "<ref> TAB <new commit> TAB <its parent>"
  any other ref of the build (a debug extension) stays off the site, and the parent of the new commit must be the published commit of the ref
exit: 0 ok, 1 nothing to export or a failed tool, 2 bad input, 3 broken parent chain
EOF
    exit 2
}
set -eu
[ $# -eq 2 ] || usage
[ -d "$1/repo" ] || { echo "export-flatpak-commits: $1/repo does not exist" >&2; exit 2; }
pages=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
repo=$pages/repo
# A clone of gh-pages lacks the empty directories of the repository (git keeps none)
mkdir -p "$repo/refs/heads" "$repo/refs/mirrors" "$repo/refs/remotes" "$repo/tmp"
fail() { echo "export-flatpak-commits: $*" >&2; exit 1; }

app_ref=app/io.github.the_beper.Kinefin/x86_64/stable
base=-
if [ -d "$pages/.git" ]; then base=$(git -C "$pages" rev-parse HEAD); fi
rm -rf "$out/repo" "$out/refs.txt" "$out/flatpak-commits.tar"

new=$(ostree --repo="$repo" rev-parse "$app_ref") || fail "the build has no $app_ref"
old=-
if [ "$base" != - ] && git -C "$pages" cat-file -e "HEAD:repo/refs/heads/$app_ref" 2> /dev/null; then
    old=$(git -C "$pages" show "HEAD:repo/refs/heads/$app_ref" | tr -d '\n')
fi
[ "$new" != "$old" ] || fail "the build added no commit to $repo"
if parent=$(ostree --repo="$repo" rev-parse "$new^" 2> /dev/null); then :; else parent=-; fi
if [ "$parent" != "$old" ]; then
    echo "export-flatpak-commits: $app_ref: the new commit $new has parent $parent, the published one is $old" >&2
    exit 3
fi

ostree --repo="$out/repo" init --mode=archive-z2
printf 'base\t%s\n%s\t%s\t%s\n' "$base" "$app_ref" "$new" "$parent" > "$out/refs.txt"
# By checksum, not by name: pull-local resolves a name through the summary file when the repository has one, and the published summary still names the old commit
ostree --repo="$out/repo" pull-local "$repo" "$new"
ostree --repo="$out/repo" refs --create="$app_ref" "$new"
[ "$(ostree --repo="$out/repo" rev-parse "$app_ref")" = "$new" ] || fail "$app_ref was not copied unchanged"
tar --sort=name --owner=0 --group=0 --numeric-owner -C "$out" -cf "$out/flatpak-commits.tar" repo refs.txt
echo "export-flatpak-commits: $app_ref exported on top of $base:"
cat "$out/refs.txt"
