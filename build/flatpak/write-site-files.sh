#!/bin/sh
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Writes the .flatpakrepo, .flatpakref, icon and .nojekyll of the site, publish-repo.sh calls it and finish-flatpak-repo.sh calls it again to compare
usage() {
    cat >&2 <<'EOF'
usage: GPG_KEY_B64=<base64 of the binary public key> write-site-files.sh <site-dir> <base-url>
EOF
    exit 2
}
set -eu
[ $# -eq 2 ] || usage
site=$(readlink -f "$1")
url=${2%/}
id=io.github.the_beper.Kinefin
key=${GPG_KEY_B64:?GPG_KEY_B64 is empty or not set: it must be the base64 of the binary public key}

cat > "$site/kinefin.flatpakrepo" <<REPO
[Flatpak Repo]
Title=Kinefin
Url=$url/repo/
Homepage=https://github.com/the-beper/Kinefin
Comment=Browse Jellyfin movies and TV series on KDE Plasma
Description=Kinefin is a native KDE Plasma client for browsing movie and TV libraries on Jellyfin servers.
Icon=$url/icon.svg
GPGKey=$key
REPO

cat > "$site/kinefin.flatpakref" <<REF
[Flatpak Ref]
Title=Kinefin
Name=$id
Branch=stable
Url=$url/repo/
Homepage=https://github.com/the-beper/Kinefin
Icon=$url/icon.svg
SuggestRemoteName=kinefin
IsRuntime=false
RuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo
GPGKey=$key
REF

cp "$(dirname "$0")/../../data/icons/hicolor/scalable/apps/$id.svg" "$site/icon.svg"
# GitHub Pages runs Jekyll unless told not to, and Jekyll skips the dot-files OSTree uses
touch "$site/.nojekyll"
