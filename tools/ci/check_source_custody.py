#!/usr/bin/env python3
"""Require source-area inputs to exist in HEAD, not merely in a local index.

This inventory complements (does not replace) configure/build and archive smoke.
It does not infer transitive CMake dependencies or audit content licensing.
Private planning documents outside source areas are not publication inputs.
"""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys

SOURCE_AREAS = {"game", "platform", "tests", "tools", "cmake", ".github"}
ROOT_INPUTS = {"CMakeLists.txt"}


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          stdout=subprocess.PIPE).stdout


def paths(data):
    if data and not data.endswith(b"\0"):
        raise ValueError("unterminated Git filename inventory")
    return {value.decode("utf-8", "surrogateescape") for value in
            (data[:-1].split(b"\0") if data else [])}


def source_path(path):
    return path in ROOT_INPUTS or PurePosixPath(path).parts[0] in SOURCE_AREAS


def inventory(root):
    committed = paths(git(root, "ls-tree", "-r", "--name-only", "-z", "HEAD"))
    indexed = paths(git(root, "ls-files", "-z", "--cached"))
    untracked = paths(git(root, "ls-files", "-z", "--others", "--exclude-standard"))
    findings = []
    for path in sorted(committed | indexed | untracked):
        if not source_path(path):
            continue
        if path not in committed:
            findings.append({"path": path, "status": "index-only" if path in indexed else "untracked"})
        elif not (root / path).exists():
            findings.append({"path": path, "status": "missing-worktree"})
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--json", action="store_true", help="emit inventory for local release planning")
    args = parser.parse_args()
    try:
        # Git -C must name the repository root: path inventories are root-relative.
        raw_root = git(args.repo_root, "rev-parse", "--show-toplevel")
        if not raw_root.endswith(b"\n"):
            raise ValueError("invalid Git root")
        root = Path(os.fsdecode(raw_root[:-1]))
        findings = inventory(root)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"source custody: scan failed: {error}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(findings, indent=2, ensure_ascii=True))
    elif findings:
        for finding in findings:
            print(f"source custody: {finding['status']}: {finding['path']!r}")
    else:
        print("source custody: PASS -- source-area paths exist in HEAD and working tree")
    if findings:
        print("source custody: FAIL -- review and commit intended inputs before archive qualification; "
              "do not bulk-add private artifacts", file=sys.stderr)
    return int(bool(findings))


if __name__ == "__main__":
    sys.exit(main())
