#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Pull the exact GHCR Flatpak build image selected for this release.
# The source image is immutable by SHA-256 digest; never fall back to :fedora45.
set -euo pipefail

digest=${KINEFIN_RELEASE_FLATPAK_DIGEST:-}
if [[ ! "$digest" =~ ^sha256:[0-9a-f]{64}$ ]]; then
    echo 'release-image: set repository variable KINEFIN_RELEASE_FLATPAK_DIGEST to sha256:<64 lowercase hex digits> from a trusted Images workflow' >&2
    exit 1
fi

if [[ -z "${GHCR_TOKEN:-}" || -z "${GHCR_USER:-}" ]]; then
    echo 'release-image: GHCR_TOKEN and GHCR_USER are required' >&2
    exit 1
fi

image="ghcr.io/the-beper/kinefin-flatpak@${digest}"
# The release build script expects this local image name. Only the source
# reference is pulled: no lookup of the mutable fedora45 tag takes place.
printf '%s\n' "$GHCR_TOKEN" | podman login ghcr.io -u "$GHCR_USER" --password-stdin >/dev/null
podman pull "$image"
podman tag "$image" localhost/kinefin-flatpak:fedora45

if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    # shellcheck disable=SC2016 # the backticks are Markdown code in the step summary, not a command substitution
    printf 'Flatpak release build image: `%s`\n' "$image" >> "$GITHUB_STEP_SUMMARY"
fi
printf 'Using Flatpak build image %s\n' "$image"
