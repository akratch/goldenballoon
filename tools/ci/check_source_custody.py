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

# What the build, the test suite and the release packages read, by consumer.
# Ignored files (.gitignore) are never inventoried, which is what keeps the
# generated web outputs, build trees and packaged zips under dist/ out.
SOURCE_AREAS = {
    "game", "platform", "lib",       # compiled into the game and its tools
    "third_party",                   # vendored code (qrcodegen) and the licences
                                     # every package copies
    "cmake", "tests", "tools",       # build recipes, gates and package scripts
    ".github", ".githooks",          # CI and the publication hooks
    "services",                      # the party service and its lockfile
    "windows",                       # resource/manifest templates and the icon
    "brand",                         # launcher art embedded at build time
    "macos",                         # bundle scripts, Info.plist, resources
    "dist",                          # the web shell the web build publishes
}
ROOT_INPUTS = {
    "CMakeLists.txt", "CMakePresets.json",
    "LICENSE", "README.md",          # shipped in the packages
    ".gitignore", ".gitattributes",  # clean-room rules; git archive attributes
}
# Tracked top-level entries nothing builds or packages, and why. Every tracked
# top-level entry must be in exactly one of these three sets
# (tests/test_source_custody.py), so a new input cannot go unwatched.
NOT_INPUTS = {
    "docs": "documentation and private planning; read by people, not the build",
    "AGENTS.md": "workspace instructions",
    "CHANGELOG.md": "release history for readers",
    "CODE_OF_CONDUCT.md": "project policy",
    "CONTRIBUTING.md": "project policy",
    "DISCLAIMER.md": "repository notice for readers",
    "NOTICE.md": "repository notice for readers; packages ship per-library licences",
    "RELEASE_NOTES.md": "published as release text, not built",
    "ROADMAP.md": "planning",
    "SECURITY.md": "project policy",
    "THIRD_PARTY.md": "inventory for readers; packages ship third_party/ licences",
    "boost-magnitude-report.md": "measurement report",
    ".clangd": "editor configuration",
    ".clang-format": "formatter configuration",
    ".editorconfig": "editor configuration",
    ".decomp-baseline": "decomp sync bookkeeping for tools/sync_decomp.sh, not a build input",
}


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
