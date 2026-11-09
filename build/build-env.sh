#!/bin/sh
# Usage: build/build-env.sh [command...] (default: interactive shell)
# The build image is fedora45 (build/Containerfile); CI runs it from ghcr (images.yml).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
image=${KINEFIN_BUILD_IMAGE:-localhost/kinefin-build:fedora45}
containerfile=${KINEFIN_BUILD_CONTAINERFILE:-$root/build/Containerfile}

podman image exists "$image" || podman build -t "$image" -f "$containerfile" "$(dirname "$containerfile")"

tty=; [ -t 0 ] && tty=-t

exec podman run --rm -i $tty --userns=keep-id --network=host --security-opt label=disable \
    -e QT_QPA_PLATFORM=offscreen \
    -v "$root:$root" -w "$root" \
    -v kinefin-buildtmp:/buildtmp \
    "$image" "${@:-bash}"
# The home directory inside is the container user's own (keep-id): nothing of the host's user data is touched, and no cache needs to be mounted for a C++/QML build.
