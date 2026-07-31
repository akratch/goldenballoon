#!/usr/bin/env python3
"""Check reachable git history for launch-blocking public path provenance."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


# Paths that must never appear anywhere in reachable git history for a public
# launch. None of them has an allowed exception: they are either internal-only
# working documentation (docs/superpowers/** is the internal planning
# workspace, docs/archive/** the dated working documents; both trees exist in
# this repository) or assets that were deleted and must not resurface
# (brand/gb1.png, brand/gb2.png).
INTERNAL_ONLY_PATH_PREFIXES = (
    "docs/superpowers/",
    "docs/archive/",
)
INTERNAL_ONLY_EXACT_PATHS = {
    "PUBLIC_LAUNCH_BACKLOG.md",
    "brand/gb1.png",
    "brand/gb2.png",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check public git history path provenance.")
    parser.add_argument("--repo-root", default=".", help="repository root")
    return parser.parse_args()


def run_text(root: Path, args: list[str]) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *args],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result.stdout


def history_paths(root: Path) -> set[str]:
    output = run_text(root, ["log", "--all", "--name-only", "--format="])
    return {line.strip() for line in output.splitlines() if line.strip()}


def newest_touch(root: Path, path: str) -> str:
    output = run_text(root, ["log", "--all", "--format=%H", "--", path])
    return (output.splitlines() or ["unknown"])[0]


def is_launch_blocking(path: str) -> str | None:
    if path in INTERNAL_ONLY_EXACT_PATHS:
        return "internal-only public-launch path"
    for prefix in INTERNAL_ONLY_PATH_PREFIXES:
        if path.startswith(prefix):
            return "internal-only working documentation path"
    return None


def main() -> int:
    args = parse_args()
    root = Path(args.repo_root).resolve()

    if not (root / ".git").exists():
        print("SKIP: not a git checkout; no reachable history to scan")
        return 0

    problems: list[tuple[str, str]] = []
    for path in sorted(history_paths(root)):
        reason = is_launch_blocking(path)
        if reason is not None:
            problems.append((path, reason))

    if problems:
        print("FAIL: reachable git history contains launch-blocking public path(s).", file=sys.stderr)
        for path, reason in problems:
            commit = newest_touch(root, path)
            print(f"  - {path}: {reason}; touched by {commit[:12]}", file=sys.stderr)
        print(
            "The public repository is cut as a fresh single-root launch repository by "
            "tools/create_public_launch_repo.sh; run this guard inside that repository, "
            "which excludes these paths, rather than against a source checkout.",
            file=sys.stderr,
        )
        return 1

    print("PASS: no launch-blocking public paths found in reachable git history")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
