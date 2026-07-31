#!/usr/bin/env python3
"""Run syntax checks for tracked shell scripts."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check tracked shell script syntax.")
    parser.add_argument(
        "--repo-root",
        default=None,
        help="repository root; defaults to git top-level or current directory",
    )
    return parser.parse_args()


def git_root() -> Path:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            stderr=subprocess.DEVNULL,
            text=True,
        )
        return Path(out.strip()).resolve()
    except (OSError, subprocess.CalledProcessError):
        return Path.cwd().resolve()


def is_shell_script(root: Path, path: str) -> bool:
    if path.endswith(".sh"):
        return True
    try:
        first_line = (root / path).read_text(encoding="utf-8", errors="replace").splitlines()[0]
    except (IndexError, OSError):
        return False
    return first_line.startswith("#!") and any(
        shell in first_line for shell in ("/sh", "/bash", "env sh", "env bash")
    )


def tracked_shell_scripts(root: Path) -> list[str]:
    try:
        out = subprocess.check_output(
            ["git", "ls-files"],
            cwd=root,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        return sorted(line for line in out.splitlines() if line and is_shell_script(root, line))
    except (OSError, subprocess.CalledProcessError):
        skipped_dirs = {".git", "build", "dist", "__pycache__"}
        return sorted(
            str(path.relative_to(root))
            for path in root.rglob("*")
            if path.is_file()
            if is_shell_script(root, str(path.relative_to(root)))
            if not any(part in skipped_dirs or part.startswith("build-") for part in path.parts)
        )


def script_shell(path: Path) -> str:
    try:
        first_line = path.read_text(encoding="utf-8", errors="replace").splitlines()[0]
    except IndexError:
        return "bash"
    return "sh" if first_line.strip() == "#!/bin/sh" else "bash"


def main() -> int:
    args = parse_args()
    root = Path(args.repo_root).resolve() if args.repo_root else git_root()
    scripts = tracked_shell_scripts(root)
    failures: list[str] = []

    for script in scripts:
        shell = script_shell(root / script)
        result = subprocess.run([shell, "-n", script], cwd=root)
        if result.returncode != 0:
            failures.append(script)

    if failures:
        print("Shell syntax check failed:", file=sys.stderr)
        for script in failures:
            print(f"  {script}", file=sys.stderr)
        return 1

    print(f"PASS: checked {len(scripts)} shell scripts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
