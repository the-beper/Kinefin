#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Delete all but the newest GitHub Actions caches whose key starts with a prefix.

A cache entry is immutable, so every save under a new key adds one and the oldest are only evicted when the repository passes its size limit, which pushes out the other caches too. Best effort: a failed request is reported and never fails the job.

Usage: prune_caches.py KEY_PREFIX KEEP (needs GH_TOKEN with actions: write and GITHUB_REPOSITORY)
"""

from __future__ import annotations

import http.client
import json
import os
import sys
import urllib.parse
import urllib.request

API = "https://api.github.com"


def stale(caches: list[dict], keep: int) -> list[dict]:
    """The caches to delete: everything but the `keep` newest."""
    newest_first = sorted(caches, key=lambda cache: cache["created_at"], reverse=True)
    return newest_first[max(keep, 0):]


def request(method: str, path: str, token: str) -> bytes:
    req = urllib.request.Request(
        API + path,
        method=method,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    with urllib.request.urlopen(req, timeout=30) as response:
        return response.read()


def main(argv: list[str]) -> int:
    if len(argv) != 3 or not argv[2].isdigit():
        print(__doc__, file=sys.stderr)
        return 2
    prefix, keep = argv[1], int(argv[2])
    repo, token = os.environ.get("GITHUB_REPOSITORY", ""), os.environ.get("GH_TOKEN", "")
    if not repo or not token:
        print("prune_caches: GITHUB_REPOSITORY and GH_TOKEN are required", file=sys.stderr)
        return 2
    try:
        query = urllib.parse.urlencode({"key": prefix, "per_page": 100, "sort": "created_at", "direction": "desc"})
        caches = json.loads(request("GET", f"/repos/{repo}/actions/caches?{query}", token))["actions_caches"]
        # The key filter is a prefix match, but keep the check local so that a change on the API side cannot widen it
        caches = [cache for cache in caches if cache["key"].startswith(prefix)]
        for cache in stale(caches, keep):
            request("DELETE", f"/repos/{repo}/actions/caches/{cache['id']}", token)
            print(f"deleted cache {cache['key']} ({cache['size_in_bytes'] // 1_000_000} MB)")
    except (OSError, http.client.HTTPException, KeyError, ValueError) as error:
        print(f"::warning::prune_caches: {error}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
