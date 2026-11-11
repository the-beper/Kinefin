#!/bin/sh
# Builds an OSTree repository (dist/flatpak-repo, or $KINEFIN_FLATPAK_REPO) and dist/Kinefin-x86_64.flatpak inside a throw-away rootless podman container. Runtimes and the Flatpak build cache live in the podman volume "kinefin-flatpak", not on the host.
# Usage: build/flatpak/build-flatpak.sh [flatpak-builder options...]
# TODO (first release): port KuickShare's prepare/sign/finish pipeline (a signed OSTree repository on GitHub Pages) if the Flatpak is ever self-hosted; until then Flathub is the only repository and the bundle goes to the GitHub release as an asset.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
image=localhost/kinefin-flatpak:fedora45

# P0 requires a real CMake project: missing source must fail rather than silently
# reporting success and skipping the artifact/smoke-test gates.
[ -f "$root/CMakeLists.txt" ] || { echo "missing required CMakeLists.txt" >&2; exit 1; }

if [ ! -e /run/.containerenv ]; then
    # Rootless podman only: root in the container is then the calling user on the host. No --privileged: bubblewrap, the sandbox of flatpak-builder (and of its icon validation), needs /proc unmasked to mount its own (unmask=ALL) and SYS_ADMIN and NET_ADMIN to create namespaces as container root. Without NET_ADMIN, "loopback: Failed RTM_NEWADDR". The capabilities hold inside the container's user namespace only.
    [ "$(podman info --format '{{.Host.Security.Rootless}}')" = true ] || { echo "build-flatpak.sh needs rootless podman" >&2; exit 1; }
    podman image exists "$image" || podman build -t "$image" -f "$root/build/flatpak/Containerfile" "$root/build/flatpak"
    exec podman run --rm --security-opt unmask=ALL --cap-add SYS_ADMIN --cap-add NET_ADMIN --security-opt label=disable --network=host -e KINEFIN_FLATPAK_REPO \
        -v kinefin-flatpak:/var/lib/flatpak -v "$root:$root" -w "$root" \
        "$image" "$root/build/flatpak/build-flatpak.sh" "$@"
fi

cd "$root"
# The version is the single source in the root CMakeLists.txt (project(kinefin VERSION X.Y.Z)).
version=$(sed -n 's/^project(kinefin VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$version" ] || { echo "version not found in CMakeLists.txt" >&2; exit 1; }
id=io.github.the_beper.Kinefin
# The build tree holds symlinks to /run, which on the host leads into the document portal and makes file scanners (VS Code, clangd) walk the whole home through it: keep it in the volume, not in the workspace.
builder=/var/lib/flatpak/builder
repo=$(readlink -m "${KINEFIN_FLATPAK_REPO:-dist/flatpak-repo}")
# A repository cloned from gh-pages lacks its empty directories (git keeps none).
if [ -d "$repo" ]; then mkdir -p "$repo/refs/heads" "$repo/refs/mirrors" "$repo/refs/remotes" "$repo/tmp"; fi

flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
# Runtime refs and commits are shared with the smoke test. A release gate
# checks them against the manifest before the Flatpak build starts.
# shellcheck source=build/flatpak/runtime-pins.conf
. "$(dirname "$0")/runtime-pins.conf"
pin() { # pin REF COMMIT: installs nothing, moves the installed ref to the commit and checks it
    flatpak update -y --noninteractive --commit="$2" "$1"
    pinned "$1" "$2"
}
pinned() { [ "$(flatpak info --show-commit "$1")" = "$2" ] || { echo "$1 is not at the pinned commit $2" >&2; exit 1; }; }
flatpak install -y --noninteractive flathub "$sdk" "$platform"
pin "$sdk" "$sdk_commit"
pin "$platform" "$platform_commit"
mkdir -p dist/tmp

cd build/flatpak
flatpak-builder --force-clean --disable-rofiles-fuse --default-branch=stable \
    --state-dir="$builder/state" --repo="$repo" "$@" \
    "$builder/build" "$id.yml"
cd "$root"
# No --install-deps-from: flatpak-builder would update every installed dependency to the Flathub head.
pinned "$sdk" "$sdk_commit"
pinned "$platform" "$platform_commit"
flatpak build-bundle "$repo" dist/Kinefin-x86_64.flatpak "$id" stable
echo "Built dist/Kinefin-x86_64.flatpak"
