#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Finishes a Flatpak repository for static hosting: prune, summary, appstream data and the files users add it with, without static deltas (a delta carries the signature of its commit, which the split signing cannot produce)
usage() {
    cat >&2 <<'EOF'
usage: [GPG_KEY_ID=<key> | GPG_KEY_B64=<key>] publish-repo.sh <site-dir> <base-url>
  <site-dir>/repo  the OSTree repository, <base-url> where <site-dir> is served
  GPG_KEY_ID       signs the new commits and the summary with that key of $GNUPGHOME and writes its public key into the site files
  GPG_KEY_B64      without a key to sign with: the base64 of the binary public key for the site files, nothing is signed
EOF
    exit 2
}
set -eu
[ $# -eq 2 ] || usage
site=$(readlink -f "$1")
url=${2%/}
if [ -n "${GPG_KEY_ID:-}" ]; then
    sign=--gpg-sign=$GPG_KEY_ID
    key=$(gpg --export "$GPG_KEY_ID" | base64 -w0)
else
    sign=
    key=${GPG_KEY_B64:?GPG_KEY_B64 is empty or not set: set it to the base64 of the binary public key (nothing is signed), or set GPG_KEY_ID to sign}
fi

mkdir -p "$site/repo/refs/heads" "$site/repo/refs/mirrors" "$site/repo/refs/remotes" "$site/repo/tmp"
# The deltas of earlier releases would be listed in the new summary: clients fetch objects instead
rm -rf "$site/repo/deltas" "$site/repo/delta-indexes"
flatpak build-update-repo ${sign:+"$sign"} --prune --prune-depth=3 "$site/repo"
GPG_KEY_B64=$key "$(dirname "$0")/write-site-files.sh" "$site" "$url"
