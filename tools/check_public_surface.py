#!/usr/bin/env python3
"""Fail closed when new public Git content carries private working material.

The repository is public and keeps ordinary Git history.  This guard therefore
scans the exact tree or commits that will be published; it does not rely on
``export-ignore`` or on rebuilding a separate squashed repository.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from collections.abc import Collection, Iterable
from pathlib import Path, PurePosixPath


DENYLIST_PATH = "tools/public_text_denylist.txt"
MAX_HITS = 80
NUL_ALLOWED_BINARY_SUFFIXES = frozenset(
    {
        ".gif",
        ".icns",
        ".ico",
        ".jpeg",
        ".jpg",
        ".otf",
        ".png",
        ".ttf",
        ".webp",
        ".woff",
        ".woff2",
    }
)

# Minimum run length for treating bytes in a binary blob as readable text.
PRINTABLE_RUN_RE = re.compile(rb"[\t\x20-\x7e]{4,}")

# The initial public commit contains detector and migration-tool blobs which
# necessarily quote the vocabulary they remove. Grandfather only these reviewed
# byte sequences. Never exempt an entire path: a changed version must be scanned
# and rejected until its exact digest is deliberately reviewed and added here.
REVIEWED_CONTENT_EXEMPT_BLOBS = frozenset(
    {
        (
            ".gitattributes",
            "3848f041a246cb422967535f80194f4b9df5480794dd9770d3c11c6063319a8a",
        ),
        (
            ".gitignore",
            "885c6a13099b9b43c67fb848edbb9298fefb8bd3e0212cf2adf837c37cddc6c9",
        ),
        (
            "tools/check_native_sdk_surface.py",
            "642c257cc22a15f1c58455c3c0738527a52bd2f90c744ff3bf960999a53c1961",
        ),
        (
            "tools/check_public_history_paths.py",
            "77e67b28a806b00c8ee5559cd4d823207b7ece5a14e757e88339a2aa4d66859d",
        ),
        (
            "tools/ci/check_public_history_text.sh",
            "f17de0cfa0661d5dab6c16e7e6b82b2010191f5cff7adafafbc780a8165a79b9",
        ),
        (
            "tools/ci/check_release_ready.sh",
            "7c63a1586a9bfb35c3b49e7b8a04826dfba13a45474946041664c3de20ad2b0a",
        ),
        (
            "tools/create_public_launch_repo.sh",
            "6caa794b87cf251ae23edb408029745002100af6a72bcc0274938a5b33ae2785",
        ),
        # Reviewed detector/test blobs on the assembly branch immediately
        # before exact-blob enforcement replaced its path exemptions.
        (
            "tools/check_public_surface.py",
            "c441bbc559e35ad8c75052d7c2bfa46fdc4741e1565cc9e1601b714d4d5f5e36",
        ),
        (
            "tests/test_public_surface.py",
            "820fc1803bf43f2674e94cf6bcffa3675907a413b48fe09b55417f5b94d4fca5",
        ),
        (
            DENYLIST_PATH,
            "d81b17320576fd6c9f81c3cd896714ba565b1aa604b99fa8861083538516adc7",
        ),
        # Current detector vocabulary, reviewed independently of this guard.
        (
            DENYLIST_PATH,
            "7094aafed749072619d8124310a74675e6fc5d73ce1a30fb5d3d774e38592e51",
        ),
        # Same vocabulary, after the header comment followed
        # check_github_launch_ready.sh into tools/manual/. Reviewed: the only
        # difference from the digest above is that one path in one comment.
        (
            DENYLIST_PATH,
            "3466b7e8b574c3789f6e39f26d9fd64655d456cca70718c1cea74aea941f5b04",
        ),
    }
)

FORBIDDEN_EXACT_PATHS = {
    "PUBLIC_LAUNCH_" "BACKLOG.md",
    "docs/SDK_INVENTORY.md",
}
FORBIDDEN_PREFIXES = (
    ".private/",
    "docs/archive/",
    "docs/" "super" "powers/",
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


def printable_runs(data: bytes) -> list[str]:
    """Extract the printable ASCII runs of a binary blob, `strings`-style."""
    return [
        run.decode("ascii")
        for run in PRINTABLE_RUN_RE.findall(data)
    ]


def blob_hits(
    path: str,
    data: bytes,
    patterns: Iterable[re.Pattern[str]],
) -> list[str]:
    hits: list[str] = []
    if b"\0" in data:
        suffix = PurePosixPath(path).suffix.lower()
        if suffix not in NUL_ALLOWED_BINARY_SUFFIXES:
            return [f"{path}: NUL byte in text/source-like public blob"]
        # The suffix exempts the blob from the NUL-in-text rule only. Its bytes
        # are still read: a file named .png that actually carries text or a
        # credential must not reach the public tree unscanned. Match the
        # denylist against the blob's printable runs, which is what a reader
        # (or a leak scanner) would recover from it either way.
        for run in printable_runs(data):
            if any(pattern.search(run) for pattern in patterns):
                excerpt = run.strip()
                if len(excerpt) > 180:
                    excerpt = excerpt[:177] + "..."
                hits.append(f"{path}: high-risk text in binary blob: {excerpt}")
        return hits
    text = data.decode("utf-8", errors="replace")
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
    *,
    exempt_blobs: Collection[tuple[str, str]] = REVIEWED_CONTENT_EXEMPT_BLOBS,
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
        digest = hashlib.sha256(data).hexdigest()
        if (path, digest) in exempt_blobs:
            continue
        problems.extend(blob_hits(path, data, patterns))
        if len(problems) >= MAX_HITS:
            break
    return problems


def scan_tree(
    root: Path, patterns: list[re.Pattern[str]], revision: str = "HEAD"
) -> list[str]:
    paths = nul_paths(
        run_git(root, ["ls-tree", "-r", "-z", "--name-only", revision])
    )
    return scan_paths(
        root,
        paths,
        patterns,
        lambda path: run_git(root, ["show", f"{revision}:{path}"]),
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
    root: Path,
    commit: str,
    patterns: list[re.Pattern[str]],
    *,
    metadata: bool = True,
    exempt_blobs: Collection[tuple[str, str]] = REVIEWED_CONTENT_EXEMPT_BLOBS,
) -> list[str]:
    problems: list[str] = []
    short = commit[:12]
    if metadata:
        author_email, committer_email, message = commit_metadata(root, commit)
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
                "-m",
                "--first-parent",
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
            exempt_blobs=exempt_blobs,
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


def release_history_revision(root: Path) -> str:
    """Return the public history ref for this checkout.

    The private assembly checkout has a dedicated ``public`` remote whose main
    branch is the published history. A normal public clone has no such remote,
    so its release candidate is HEAD. If the assembly remote exists but has not
    been fetched, fail instead of silently auditing the private lineage.
    """
    remotes = set(run_git(root, ["remote"]).decode().splitlines())
    if "public" not in remotes:
        return "HEAD"

    revision = "refs/remotes/public/main"
    try:
        run_git(root, ["show-ref", "--verify", "--quiet", revision])
    except GuardError:
        raise GuardError(
            "public remote is configured but public/main is unavailable; "
            "run 'git fetch public main' before release readiness"
        ) from None
    return revision


def scan_history(
    root: Path,
    revision: str,
    patterns: list[re.Pattern[str]],
    *,
    exempt_blobs: Collection[tuple[str, str]] = REVIEWED_CONTENT_EXEMPT_BLOBS,
) -> list[str]:
    """Scan every reachable blob version, including files later deleted.

    Historical commit identities/messages are intentionally outside this mode:
    the new-commit boundary scans those before publication.  Release readiness
    uses this mode to prove that forbidden text never entered reachable file
    content, even transiently.
    """
    problems: list[str] = []
    for commit in commits_for_range(root, revision):
        problems.extend(
            scan_commit(
                root,
                commit,
                patterns,
                metadata=False,
                exempt_blobs=exempt_blobs,
            )
        )
        if len(problems) >= MAX_HITS:
            break
    return problems[:MAX_HITS]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="scan public Git trees/commits for private planning material"
    )
    parser.add_argument("--repo-root", default=".")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument(
        "--tree",
        dest="tree_revision",
        nargs="?",
        const="HEAD",
        metavar="REVISION",
        help="scan REVISION's complete tree (default: HEAD)",
    )
    modes.add_argument(
        "--staged", action="store_true", help="scan staged additions/modifications"
    )
    modes.add_argument(
        "--range", dest="revision_range", help="scan every commit in REVISION_RANGE"
    )
    modes.add_argument(
        "--tag", dest="tag_object", help="scan one annotated tag object"
    )
    modes.add_argument(
        "--commits-stdin",
        action="store_true",
        help="scan whitespace-separated commit ids read from stdin",
    )
    modes.add_argument(
        "--history",
        dest="history_revision",
        metavar="REVISION",
        help="scan file content in every commit reachable from REVISION",
    )
    modes.add_argument(
        "--release-history",
        action="store_true",
        help="scan public/main in an assembly checkout, otherwise HEAD",
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
        elif args.tree_revision:
            problems = scan_tree(root, patterns, args.tree_revision)
            label = f"committed public tree {args.tree_revision}"
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
        elif args.history_revision:
            problems = scan_history(root, args.history_revision, patterns)
            label = f"reachable public history {args.history_revision}"
        elif args.release_history:
            revision = release_history_revision(root)
            problems = scan_history(root, revision, patterns)
            label = f"reachable public release history {revision}"
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
