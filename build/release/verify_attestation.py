#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify GitHub's signed SLSA attestation for an exact Kinefin Flatpak.

Unlike SHA256SUMS or PROVENANCE.json, this validates the attestation's
signature, subject digest and signer/source identity through GitHub CLI.
GitHub CLI fetches attestations over the network unless configured otherwise.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
from typing import Callable

# Python -I does not include the script's directory on sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import publish_release as publisher  # noqa: E402

WORKFLOW = ".github/workflows/release.yml"
DEFAULT_REPOSITORY = "the-beper/Kinefin"


class AttestationError(ValueError):
    """The signed attestation could not be trusted or verified."""


def verify(
    bundle: Path, repository: str, commit: str, tag: str,
    *, runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> None:
    """Fail closed unless a signed attestation matches independently known inputs."""
    if (not publisher.REPO_RE.fullmatch(repository) or ".." in repository
            or repository.endswith(".")):
        raise AttestationError("repository must be a valid trusted owner/name")
    if not publisher.COMMIT_RE.fullmatch(commit) or commit == "0" * 40:
        raise AttestationError("commit must be an independently trusted full lowercase Git SHA")
    if not publisher.TAG_RE.fullmatch(tag):
        raise AttestationError("tag must be stable vX.Y.Z")
    if bundle.is_symlink() or not bundle.is_file():
        raise AttestationError(f"missing or symlinked Flatpak: {bundle}")
    if bundle.stat().st_size <= 0:
        raise AttestationError(f"empty Flatpak: {bundle}")

    # All three expectations must come from the caller, never the downloaded
    # unsigned PROVENANCE.json. The CLI checks the subject bytes, Sigstore
    # certificate, source SHA, release ref and signer workflow together.
    args = [
        "gh", "attestation", "verify", str(bundle.resolve()),
        "--repo", repository,
        "--signer-workflow", f"{repository}/{WORKFLOW}",
        "--source-digest", commit,
        "--source-ref", f"refs/tags/{tag}",
    ]
    try:
        process = runner(args, capture_output=True, text=True, timeout=120, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise AttestationError(f"unable to verify GitHub attestation: {error}") from error
    if process.returncode:
        detail = (process.stderr or process.stdout or "verification failed").strip()
        raise AttestationError(f"GitHub attestation rejected: {detail[:1000]}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify the signed GitHub Actions attestation of a Kinefin Flatpak (requires gh and network)",
    )
    parser.add_argument("bundle", type=Path, help="downloaded Kinefin-x86_64.flatpak")
    parser.add_argument("--repository", default=DEFAULT_REPOSITORY, help="trusted GitHub owner/repo")
    parser.add_argument("--commit", required=True, help="trusted full source commit SHA")
    parser.add_argument("--tag", required=True, help="trusted stable release tag, e.g. v1.2.3")
    args = parser.parse_args()
    try:
        verify(args.bundle, args.repository, args.commit, args.tag)
    except (AttestationError, OSError) as error:
        print(f"release-attestation: {error}", file=sys.stderr)
        return 1
    print(f"Verified signed Flatpak attestation: {args.repository} {args.tag} ({args.commit})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
