#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kinefin contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Require immutable refs for GitHub Actions used by Kinefin workflows.

This is a deliberately narrow, dependency-free YAML style check, not a YAML
parser. Actionlint separately validates the full workflow syntax. Nonstandard
forms that could hide a `uses` reference are rejected instead of ignored.
"""

from __future__ import annotations

import argparse
from pathlib import Path, PurePosixPath
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
EXTERNAL = re.compile(
    r"([A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*"
    r"(?:/[A-Za-z0-9_.-]+)*)@([0-9a-f]{40})\Z"
)
CONTAINER = re.compile(r"docker://[a-z0-9][a-z0-9._:/-]*@sha256:([0-9a-f]{64})\Z")
USES = re.compile(r"^ *-? *uses:[ ]*(.*?) *$")
SUSPECT_USES = re.compile(r"""(?<![A-Za-z0-9_])['"]?uses['"]?\s*:""")
BLOCK_START = re.compile(r"^ *(?:- +)?[A-Za-z][A-Za-z0-9_-]*: *[>|][1-9]?[+-]?(?: +#.*)?$")
YAML_MERGE_OR_ALIAS = re.compile(r"(?:^|\s)<<\s*:|:\s*[&*][A-Za-z_][A-Za-z0-9_-]*")


class ActionPinError(ValueError):
    """An action reference is mutable or cannot be audited safely."""


def validate_reference(raw: str, root: Path) -> bool:
    """Validate a `uses` value. Return True for a local reference."""
    value = raw.split(" #", 1)[0].strip()
    if value.startswith(("'", '"')):
        if len(value) < 2 or value[-1] != value[0]:
            raise ActionPinError("invalid quoted uses reference")
        value = value[1:-1]
    if not value or any(character.isspace() for character in value):
        raise ActionPinError("empty or multiline uses reference")

    if value.startswith("./"):
        parts = PurePosixPath(value).parts
        if ".." in parts or "\\" in value:
            raise ActionPinError("local uses path must not traverse directories")
        if value.startswith("./.github/actions/"):
            target = root / value
            if (target / "action.yml").is_file() or (target / "action.yaml").is_file():
                allowed = target.is_dir()
            else:
                allowed = False
        elif value.startswith("./.github/workflows/") and value.endswith((".yml", ".yaml")):
            target = root / value
            allowed = target.is_file()
        else:
            raise ActionPinError("local uses must point to an action or reusable workflow under .github")
        if not allowed or not target.resolve().is_relative_to(root.resolve()):
            raise ActionPinError("local uses target is missing or outside the checkout")
        return True

    match = EXTERNAL.fullmatch(value)
    if match:
        path, commit = match.groups()
        if any(part in (".", "..") for part in path.split("/")) or set(commit) == {"0"}:
            raise ActionPinError("invalid repository path or zero commit")
        return False

    match = CONTAINER.fullmatch(value)
    if match:
        if set(match.group(1)) == {"0"}:
            raise ActionPinError("docker uses requires a nonzero SHA256 digest")
        return False

    raise ActionPinError("external uses requires a full lowercase 40-character commit SHA (or docker SHA256 digest)")


def scan_file(path: Path, root: Path) -> tuple[int, int]:
    if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root.resolve()):
        raise ActionPinError(f"workflow/action file is missing, a symlink or outside the checkout: {path}")
    try:
        contents = path.read_text(encoding="utf-8")
    except (UnicodeError, OSError) as error:
        raise ActionPinError(f"cannot read {path}: {error}") from error
    if contents.startswith("\ufeff"):
        raise ActionPinError(f"{path}: YAML with BOM is not supported")

    external, local = 0, 0
    block_indent: int | None = None
    for line_number, line in enumerate(contents.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        spaces = len(line) - len(line.lstrip(" "))
        if block_indent is not None:
            if spaces > block_indent:
                continue
            block_indent = None
        if "\t" in line[: len(line) - len(line.lstrip())]:
            raise ActionPinError(f"{path}:{line_number}: tabs in YAML indentation are not supported")

        # YAML anchors, aliases and merges could smuggle an action reference
        # outside a direct `uses:` field; require an explicit, reviewable form.
        if YAML_MERGE_OR_ALIAS.search(line):
            raise ActionPinError(f"{path}:{line_number}: YAML anchors, aliases or merges are not supported")
        matched = USES.fullmatch(line)
        if matched:
            try:
                is_local = validate_reference(matched.group(1), root)
            except ActionPinError as error:
                raise ActionPinError(f"{path}:{line_number}: {error}") from error
            local += int(is_local)
            external += int(not is_local)
            continue
        if SUSPECT_USES.search(line):
            raise ActionPinError(f"{path}:{line_number}: unsupported uses syntax; write `- uses: owner/repo@SHA`")
        if BLOCK_START.fullmatch(line):
            block_indent = spaces
    return external, local


def scan(root: Path) -> tuple[int, int, int]:
    workflow_dir = root / ".github/workflows"
    if not workflow_dir.is_dir() or workflow_dir.is_symlink():
        raise ActionPinError("missing or symlinked .github/workflows directory")
    workflows = sorted(workflow_dir.glob("*.yml")) + sorted(workflow_dir.glob("*.yaml"))
    if not workflows:
        raise ActionPinError("no GitHub Actions workflow files found")

    action_dir = root / ".github/actions"
    actions: list[Path] = []
    if action_dir.is_dir():
        if action_dir.is_symlink():
            raise ActionPinError("symlinked .github/actions directory")
        if any(entry.is_symlink() for entry in action_dir.rglob("*")):
            raise ActionPinError("symlinks under .github/actions are not supported")
        actions = sorted(action_dir.rglob("action.yml")) + sorted(action_dir.rglob("action.yaml"))
    external = local = 0
    for path in workflows + actions:
        pinned, relative = scan_file(path, root)
        external += pinned
        local += relative
    return external, local, len(workflows) + len(actions)


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit immutable GitHub Actions references")
    parser.add_argument("--root", type=Path, default=ROOT, help="checkout root (for offline tests)")
    args = parser.parse_args()
    try:
        external, local, files = scan(args.root)
    except ActionPinError as error:
        print(f"action-pins: {error}", file=sys.stderr)
        return 1
    print(f"action-pins: {external} pinned external uses, {local} local uses across {files} workflows/actions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
