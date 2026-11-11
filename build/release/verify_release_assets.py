#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline integrity audit of the three files in a Kinefin GitHub release.

Reuse the publisher's strict local checks without invoking the GitHub CLI.
These files are unsigned: the result is NOT proof of who built the bundle.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

# Python -I excludes the script directory from the import path. Add only the
# trusted directory containing this script, not the caller's working directory.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import publish_release as publisher  # noqa: E402

IMAGE_PREFIX = "ghcr.io/the-beper/kinefin-flatpak@"
SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
DEFAULT_REPOSITORY = "the-beper/Kinefin"


def verify(
    directory: Path, tag: str, commit: str, repository: str,
    run_id: str | None = None, image_digest: str | None = None,
    expected_sha256: str | None = None,
) -> dict:
    """Verify local files and optional independently supplied expectations.

    The tag, repository and exact commit are mandatory reference values, not
    values learned from the potentially modified release assets themselves.
    """
    if not publisher.TAG_RE.fullmatch(tag):
        raise publisher.ReleasePublishError("expected tag must be stable vX.Y.Z")
    if not publisher.REPO_RE.fullmatch(repository) or ".." in repository or repository.endswith("."):
        raise publisher.ReleasePublishError("invalid expected GitHub repository")
    if not publisher.COMMIT_RE.fullmatch(commit) or commit == "0" * 40:
        raise publisher.ReleasePublishError("expected commit must be a full lowercase Git SHA")
    if expected_sha256 is not None and (
        not SHA256_RE.fullmatch(expected_sha256) or expected_sha256 == "0" * 64
    ):
        raise publisher.ReleasePublishError("expected Flatpak SHA256 must be 64 lowercase hex digits")

    provenance = directory / publisher.PROVENANCE
    if provenance.is_symlink() or not provenance.is_file():
        raise publisher.ReleasePublishError(f"missing or symlinked release asset: {provenance}")
    metadata = publisher.strict_json(provenance)
    builder = publisher.checked_object(
        metadata.get("build"), {"github_run_id", "image"}, "provenance build",
    )
    image = builder["image"]
    if not isinstance(image, str) or not image.startswith(IMAGE_PREFIX):
        raise publisher.ReleasePublishError("invalid provenance build image")

    # The caller may also pin these two fields to known values (e.g. from the
    # trusted workflow run); otherwise they are checked for syntax and internal
    # consistency, but remain claims made by the unsigned manifest.
    expected_run_id = builder["github_run_id"] if run_id is None else run_id
    expected_image_digest = image[len(IMAGE_PREFIX):] if image_digest is None else image_digest
    assets = publisher.local_assets(
        directory, tag, repository, commit, expected_run_id, expected_image_digest,
    )
    # Reuse the already computed hash; re-hashing a large Flatpak is expensive.
    actual_sha256 = assets[publisher.BUNDLE].digest
    if expected_sha256 is not None and actual_sha256 != expected_sha256:
        raise publisher.ReleasePublishError("Flatpak SHA256 differs from the independently supplied digest")

    return {
        "tag": tag,
        "repository": repository,
        "commit": commit,
        "sha256": actual_sha256,
        "run_id": expected_run_id,
        "image_digest": expected_image_digest,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify Kinefin Flatpak, SHA256SUMS and unsigned PROVENANCE.json offline",
    )
    parser.add_argument("tag", help="trusted release tag, e.g. v1.2.3")
    parser.add_argument("directory", type=Path, help="directory containing the three release files")
    parser.add_argument("--commit", required=True, help="expected full Git commit SHA, from a trusted source")
    parser.add_argument("--repository", default=DEFAULT_REPOSITORY, help="expected GitHub owner/repo")
    parser.add_argument("--run-id", help="optional independently known GitHub Actions run ID")
    parser.add_argument("--image-digest", help="optional independently known sha256:... build-image digest")
    parser.add_argument("--expected-sha256", help="optional trusted SHA256 of the Flatpak bundle")
    args = parser.parse_args()
    try:
        result = verify(
            args.directory, args.tag, args.commit, args.repository,
            args.run_id, args.image_digest, args.expected_sha256,
        )
    except (publisher.ReleasePublishError, OSError, TypeError, ValueError) as error:
        print(f"release-audit: {error}", file=sys.stderr)
        return 1
    print(f"Verified three local release assets for {result['repository']} {result['tag']}")
    print(f"Source commit: {result['commit']}")
    print(f"Flatpak SHA256: {result['sha256']}")
    print(f"Run ID (self-reported unless pinned): {result['run_id']}")
    print(f"Build image (self-reported unless pinned): {result['image_digest']}")
    print("Integrity check only: PROVENANCE.json and SHA256SUMS are unsigned; "
          "this does not authenticate the publisher or the build.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
