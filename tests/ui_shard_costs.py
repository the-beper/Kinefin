#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes tests/ui_shard_costs.txt from the XML logs of a run of test_ui_pages.

CMake deals the functions of test_ui_pages out to the shards `ui_pages_N` by these costs, the longest first to the lightest shard. Measure after the tests change a lot:

    build/build-env.sh python3 tests/ui_shard_costs.py <build directory> [--write]

It runs the shards side by side with `-o <file>,xml` and prints the cost of the busiest shard and the idle one that the current table gives. With `--write` it replaces tests/ui_shard_costs.txt.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

COSTS = Path(__file__).resolve().parent / "ui_shard_costs.txt"


def run_shards(build: str, out: Path) -> None:
    listing = subprocess.run(
        ["ctest", "--test-dir", build, "--show-only=json-v1", "-R", r"^ui_pages_[0-9]+$"],
        capture_output=True, text=True, check=True).stdout
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", KINEFIN_TEST_TIMEOUT_SCALE="3")
    procs = [
        subprocess.Popen(test["command"] + ["-o", f"{out / test['name']}.xml,xml"], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for test in json.loads(listing)["tests"]
    ]
    for proc in procs:
        proc.wait()


def durations(out: Path) -> dict[str, int]:
    found: dict[str, int] = {}
    for path in sorted(out.glob("*.xml")):
        for function in ET.parse(path).getroot().iter("TestFunction"):
            name = function.get("name", "")
            duration = function.find("Duration")
            if name in ("initTestCase", "cleanupTestCase") or duration is None:
                continue
            found[name] = round(float(duration.get("msecs", "0")))
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build", help="the build directory of test_ui_pages")
    parser.add_argument("--write", action="store_true", help="replace tests/ui_shard_costs.txt")
    parser.add_argument("--from", dest="logs", help="read the XML logs of an earlier run in this directory instead of running")
    args = parser.parse_args()
    if args.logs:
        costs = durations(Path(args.logs))
    else:
        with tempfile.TemporaryDirectory() as tmp:
            run_shards(args.build, Path(tmp))
            costs = durations(Path(tmp))
    if not costs:
        print("no test function was measured", file=sys.stderr)
        return 1
    median = round(statistics.median(costs.values()))
    lines = [
        "# The measured milliseconds of each function of test_ui_pages: CMake deals the functions out to the shards by them (see tests/ui_shard_costs.py)",
        f"default_cost {median}",
    ]
    lines += [f"{name} {ms}" for name, ms in sorted(costs.items(), key=lambda item: (-item[1], item[0]))]
    text = "\n".join(lines) + "\n"
    print(f"{len(costs)} functions, {sum(costs.values()) / 1000:.0f} s in all, median {median} ms")
    if args.write:
        COSTS.write_text(text, encoding="utf-8")
        print(f"wrote {COSTS}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
