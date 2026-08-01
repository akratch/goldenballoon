#!/usr/bin/env python3
"""Fail closed when new public Git content carries private working material.

The repository is public and keeps ordinary Git history.  This guard therefore
scans the exact tree or commits that will be published; it does not rely on
``export-ignore`` or on rebuilding a separate squashed repository.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Iterable


DENYLIST_PATH = "tools/public_text_denylist.txt"
MAX_HITS = 80

# These files necessarily describe the detection rules.  Keeping the exemption
# small and exact prevents a documentation directory from becoming a blind spot.
CONTENT_EXEMPT_PATHS = {
    DENYLIST_PATH,
    "tools/check_public_surface.py",
    "tools/check_native_sdk_surface.py",
    "tools/ci/check_public_history_text.sh",
    "tools/ci/check_public_push.sh",
    "tests/test_public_surface.py",
}

FORBIDDEN_EXACT_PATHS = {
    "PUBLIC_LAUNCH_BACKLOG.md",
    "docs/SDK_INVENTORY.md",
}
FORBIDDEN_PREFIXES = (
    ".private/",
    "docs/archive/",
    "docs/superpowers/",
    "docs/plans/",
)
PRIVATE_DOC_NAME_RE = re.compile(
    r"(?:^|[-_.])(BACKLOG|HANDOFF|SESSION|WORKING[-_]?PLAN|ENGINEERING[-_]?PLAN)(?:[-_.]|$)",
    re.IGNORECASE,
)
ALLOWED_EMAIL_RE = re.compile(
    r"^(?:[^@]+@users\.noreply\.github\.com|noreply@github\.com|[^@]+@example\.invalid)$",
    re.IGNORECASE,
)


class GuardError(RuntimeError):
    """A malformed guard configuration, not a detected publication leak."""


def run_git(root: Path, args: list[str], *, input_bytes: bytes | None = None) -> bytes:
    process = subprocess.run(
        ["git", "-C", str(root), *args],
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode != 0:
        detail = process.stderr.decode("utf-8", errors="replace").strip()
        raise GuardError(f"git {' '.join(args)} failed: {detail}")
    return process.stdout


def python_pattern(posix_ere: str) -> str:
    """Translate the deliberately-small shared POSIX/Python regex subset."""
    return posix_ere.replace("[[:space:]]", r"\s")


def load_patterns(root: Path) -> list[re.Pattern[str]]:
    path = root / DENYLIST_PATH
    if not path.is_file():
        raise GuardError(f"missing shared public-text denylist: {DENYLIST_PATH}")
    patterns: list[re.Pattern[str]] = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        value = raw.strip()
        if not value or value.startswith("#"):
            continue
        try:
            patterns.append(re.compile(python_pattern(value), re.IGNORECASE))
        except re.error as error:
            raise GuardError(
                f"{DENYLIST_PATH}:{number}: pattern is not portable: {error}"
            ) from error
    if not patterns:
        raise GuardError(f"{DENYLIST_PATH} defines no patterns")
    return patterns


def forbidden_path_reason(path: str) -> str | None:
    normalized = PurePosixPath(path).as_posix()
    while normalized.startswith("./"):
        normalized = normalized[2:]
    if normalized in FORBIDDEN_EXACT_PATHS:
        return "private working-document path"
    for prefix in FORBIDDEN_PREFIXES:
        if normalized.startswith(prefix):
            return "private working-document directory"
    if normalized.lower().endswith((".md", ".markdown")):
        name = PurePosixPath(normalized).name
        if PRIVATE_DOC_NAME_RE.search(name):
            return "private planning/handoff filename"
    return None


def email_is_public(email: str) -> bool:
    return ALLOWED_EMAIL_RE.fullmatch(email.strip()) is not None


def blob_hits(
    path: str, data: bytes, patterns: Iterable[re.Pattern[str]]
) -> list[str]:
    if path in CONTENT_EXEMPT_PATHS or b"\0" in data:
        return []
    text = data.decode("utf-8", errors="replace")
    hits: list[str] = []
    for number, line in enumerate(text.splitlines(), 1):
        if any(pattern.search(line) for pattern in patterns):
            excerpt = line.strip()
            if len(excerpt) > 180:
                excerpt = excerpt[:177] + "..."
            hits.append(f"{path}:{number}: high-risk text: {excerpt}")
    return hits


def nul_paths(data: bytes) -> list[str]:
    return [
        item.decode("utf-8", errors="surrogateescape")
        for item in data.split(b"\0")
        if item
    ]


def scan_paths(
    root: Path,
    paths: Iterable[str],
    patterns: list[re.Pattern[str]],
    read_blob,
) -> list[str]:
    problems: list[str] = []
    for path in paths:
        reason = forbidden_path_reason(path)
        if reason is not None:
            problems.append(f"{path}: {reason}")
            continue
        try:
            data = read_blob(path)
        except GuardError as error:
            problems.append(str(error))
            continue
        problems.extend(blob_hits(path, data, patterns))
        if len(problems) >= MAX_HITS:
            break
    return problems


def scan_tree(root: Path, patterns: list[re.Pattern[str]]) -> list[str]:
    paths = nul_paths(run_git(root, ["ls-tree", "-r", "-z", "--name-only", "HEAD"]))
    return scan_paths(
        root,
        paths,
        patterns,
        lambda path: run_git(root, ["show", f"HEAD:{path}"]),
    )


def scan_staged(root: Path, patterns: list[re.Pattern[str]]) -> list[str]:
    paths = nul_paths(
        run_git(
            root,
            ["diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"],
        )
    )
    return scan_paths(
        root,
        paths,
        patterns,
        lambda path: run_git(root, ["show", f":{path}"]),
    )


def commit_metadata(root: Path, commit: str) -> tuple[str, str, str]:
    fields = run_git(
        root,
        ["show", "-s", "--format=%ae%x00%ce%x00%B", commit],
    ).decode("utf-8", errors="replace").split("\0", 2)
    if len(fields) != 3:
        raise GuardError(f"could not parse metadata for commit {commit}")
    return fields[0].strip(), fields[1].strip(), fields[2]


def scan_commit(
    root: Path, commit: str, patterns: list[re.Pattern[str]]
) -> list[str]:
    problems: list[str] = []
    author_email, committer_email, message = commit_metadata(root, commit)
    short = commit[:12]
    for role, email in (("author", author_email), ("committer", committer_email)):
        if not email_is_public(email):
            problems.append(
                f"commit {short}: {role} email is not an approved public identity: {email}"
            )
    problems.extend(
        f"commit {short}: {hit}"
        for hit in blob_hits("<commit-message>", message.encode(), patterns)
    )
    paths = nul_paths(
        run_git(
            root,
            [
                "diff-tree",
                "--root",
                "--no-commit-id",
                "-r",
                "--name-only",
                "--diff-filter=ACMR",
                "-z",
                commit,
            ],
        )
    )
    problems.extend(
        scan_paths(
            root,
            paths,
            patterns,
            lambda path: run_git(root, ["show", f"{commit}:{path}"]),
        )
    )
    return problems[:MAX_HITS]


def scan_annotated_tag(
    root: Path, tag_object: str, patterns: list[re.Pattern[str]]
) -> list[str]:
    """Scan the identity and message stored in an annotated tag object."""
    raw = run_git(root, ["cat-file", "tag", tag_object]).decode(
        "utf-8", errors="replace"
    )
    headers, separator, message = raw.partition("\n\n")
    if not separator:
        raise GuardError(f"could not parse annotated tag {tag_object}")
    tagger_lines = [line for line in headers.splitlines() if line.startswith("tagger ")]
    if len(tagger_lines) != 1:
        raise GuardError(f"annotated tag {tag_object} has no unique tagger identity")
    match = re.search(r"<([^<>]+)>\s+[0-9]+\s+[+-][0-9]{4}$", tagger_lines[0])
    if match is None:
        raise GuardError(f"could not parse tagger email for annotated tag {tag_object}")

    short = tag_object[:12]
    problems: list[str] = []
    email = match.group(1)
    if not email_is_public(email):
        problems.append(
            f"annotated tag {short}: tagger email is not an approved public identity: {email}"
        )
    problems.extend(
        f"annotated tag {short}: {hit}"
        for hit in blob_hits("<tag-message>", message.encode(), patterns)
    )
    return problems


def commits_for_range(root: Path, revision_range: str) -> list[str]:
    return run_git(root, ["rev-list", "--reverse", revision_range]).decode().split()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="scan public Git trees/commits for private planning material"
    )
    parser.add_argument("--repo-root", default=".")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--tree", action="store_true", help="scan the committed HEAD tree")
    modes.add_argument("--staged", action="store_true", help="scan staged additions/modifications")
    modes.add_argument("--range", dest="revision_range", help="scan every commit in REVISION_RANGE")
    modes.add_argument("--tag", dest="tag_object", help="scan one annotated tag object")
    modes.add_argument(
        "--commits-stdin",
        action="store_true",
        help="scan whitespace-separated commit ids read from stdin",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(args.repo_root).resolve()
    try:
        patterns = load_patterns(root)
        if args.staged:
            problems = scan_staged(root, patterns)
            label = "staged public changes"
        elif args.revision_range:
            commits = commits_for_range(root, args.revision_range)
            problems = []
            for commit in commits:
                problems.extend(scan_commit(root, commit, patterns))
                if len(problems) >= MAX_HITS:
                    break
            label = f"public commit range {args.revision_range}"
        elif args.tag_object:
            problems = scan_annotated_tag(root, args.tag_object, patterns)
            label = f"annotated tag {args.tag_object}"
        elif args.commits_stdin:
            commits = sys.stdin.read().split()
            problems = []
            for commit in commits:
                problems.extend(scan_commit(root, commit, patterns))
                if len(problems) >= MAX_HITS:
                    break
            label = "outgoing public commits"
        else:
            problems = scan_tree(root, patterns)
            label = "committed public tree"
    except GuardError as error:
        print(f"FAIL: public-surface guard error: {error}", file=sys.stderr)
        return 2

    if problems:
        print(f"FAIL: {label} contains private/high-risk material:", file=sys.stderr)
        for problem in problems[:MAX_HITS]:
            print(f"  - {problem}", file=sys.stderr)
        if len(problems) >= MAX_HITS:
            print(f"  - output stopped at {MAX_HITS} findings", file=sys.stderr)
        return 1
    print(f"PASS: {label} contains no private planning paths or high-risk text")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
