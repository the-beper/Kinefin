#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create a deterministic, self-reported build record for a Flatpak release.

This is an auditable manifest, NOT a signed provenance attestation. It records
inputs selected by the release workflow and the hash of the finished artifact.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


BUNDLE = "Kinefin-x86_64.flatpak"
IMAGE = "ghcr.io/the-beper/kinefin-flatpak"
TAG_RE = re.compile(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")
COMMIT_RE = re.compile(r"[0-9a-f]{40}\Z")
SHA_RE = re.compile(r"[0-9a-f]{64}\Z")
RUN_RE = re.compile(r"[1-9][0-9]*\Z")
REPO_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*\Z")


class ProvenanceError(Exception):
    """The metadata should not be created."""


def create(
    tag: str, commit: str, repository: str, run_id: str,
    image_digest: str, bundle: Path, output: Path, expected_sha256: str,
) -> dict:
    if not TAG_RE.fullmatch(tag):
        raise ProvenanceError("invalid release tag (expected vX.Y.Z)")
    if not COMMIT_RE.fullmatch(commit) or commit == "0" * 40:
        raise ProvenanceError("invalid release commit")
    if not REPO_RE.fullmatch(repository) or ".." in repository or repository.endswith("."):
        raise ProvenanceError("invalid GitHub repository")
    if not RUN_RE.fullmatch(run_id):
        raise ProvenanceError("invalid GitHub run ID")
    if not image_digest.startswith("sha256:") or not SHA_RE.fullmatch(image_digest[7:]) or image_digest[7:] == "0" * 64:
        raise ProvenanceError("invalid pinned build image digest")
    if not SHA_RE.fullmatch(expected_sha256) or expected_sha256 == "0" * 64:
        raise ProvenanceError("invalid expected bundle SHA256")
    if bundle.name != BUNDLE or bundle.is_symlink() or not bundle.is_file():
        raise ProvenanceError(f"missing, symlinked or wrongly named bundle: {bundle}")
    if output.is_symlink() or output.exists() and not output.is_file():
        raise ProvenanceError("provenance output must be a regular file")

    size = 0
    digest = hashlib.sha256()
    with bundle.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            size += len(chunk)
            digest.update(chunk)
    if size == 0 or digest.hexdigest() != expected_sha256:
        raise ProvenanceError("bundle is empty or differs from the build job SHA256")

    record = {
        "schema_version": 1,
        "repository": repository,
        "source": {"tag": tag, "commit": commit},
        "build": {
            "github_run_id": run_id,
            "image": f"{IMAGE}@{image_digest}",
        },
        "artifact": {
            "name": BUNDLE,
            "sha256": expected_sha256,
            "size": size,
        },
    }
    encoded = (json.dumps(record, sort_keys=True, indent=2) + "\n").encode("ascii")
    if output.exists():
        if output.read_bytes() != encoded:
            raise ProvenanceError("refusing to replace different provenance output")
    else:
        output.write_bytes(encoded)
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description="Write verified, self-reported Flatpak release metadata")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--image-digest", required=True)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        record = create(
            args.tag, args.commit, args.repository, args.run_id,
            args.image_digest, args.bundle, args.output, args.expected_sha256,
        )
    except (ProvenanceError, OSError, UnicodeError) as error:
        print(f"release-provenance: {error}", file=sys.stderr)
        return 1
    print(f"Release {record['source']['tag']} provenance written: {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
