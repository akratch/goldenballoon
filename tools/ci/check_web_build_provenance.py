#!/usr/bin/env python3
"""Fail closed when a staged web build is not from the candidate source tree."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def fail(message: str) -> int:
    print(f"web build provenance: FAIL -- {message}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate staged web build provenance against an exact commit.")
    parser.add_argument("--web-dir", required=True, type=Path)
    parser.add_argument("--commit", required=True,
                        help="full candidate commit resolved by the caller")
    parser.add_argument("--version", required=True,
                        help="candidate version resolved from the source tree")
    args = parser.parse_args()

    commit = args.commit
    if len(commit) != 40 or any(char not in "0123456789abcdef" for char in commit):
        return fail("candidate commit must be a full lowercase SHA-1")
    if not args.version:
        return fail("candidate version must not be empty")

    info_path = args.web_dir / "build-info.json"
    try:
        with info_path.open(encoding="utf-8") as stream:
            record = json.load(stream)
    except FileNotFoundError:
        return fail(f"missing {info_path}")
    except (OSError, json.JSONDecodeError) as error:
        return fail(f"cannot parse {info_path}: {error}")
    if not isinstance(record, dict):
        return fail(f"{info_path} must contain a JSON object")

    source_commit = record.get("source_commit")
    if not isinstance(source_commit, str):
        return fail("build-info source_commit is missing or not a string")
    if source_commit != commit:
        return fail(
            f"build-info source_commit {source_commit!r} does not equal HEAD {commit}")
    if record.get("version") != args.version:
        return fail(
            f"build-info version {record.get('version')!r} does not equal "
            f"candidate version {args.version!r}")
    # `is False` deliberately rejects 0, null, strings, and omitted values.
    # A candidate cannot make a release claim from ambiguous provenance.
    if record.get("source_dirty") is not False:
        return fail(
            f"build-info source_dirty={record.get('source_dirty')!r}; "
            "rebuild from a clean source tree")

    print(
        f"web build provenance: PASS -- {info_path} is clean at {commit} "
        f"for {args.version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
