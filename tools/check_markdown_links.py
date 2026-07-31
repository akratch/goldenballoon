#!/usr/bin/env python3
"""Check local Markdown links and anchors in tracked public docs."""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
import urllib.parse
from pathlib import Path


INLINE_LINK_RE = re.compile(r"(?<!!)\[[^\]\n]+\]\(([^)\n]+)\)")
IMAGE_LINK_RE = re.compile(r"!\[[^\]\n]*\]\(([^)\n]+)\)")
REFERENCE_LINK_RE = re.compile(r"^\s*\[[^\]]+\]:\s*(\S+)", re.MULTILINE)
HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*#*\s*$")
HTML_TAG_RE = re.compile(r"<[^>]+>")
MARKDOWN_DECORATION_RE = re.compile(r"[`*_~]")
LINK_TEXT_RE = re.compile(r"\[([^\]]+)\]\([^)]+\)")


KNOWN_FAILURES_PATH = "tools/known_markdown_link_failures.txt"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate local Markdown links and GitHub-style heading anchors."
    )
    parser.add_argument(
        "--repo-root",
        default=None,
        help="repository root; defaults to git top-level or current directory",
    )
    return parser.parse_args()


def load_known_failures(root: Path) -> list[str]:
    """Pre-existing breakage this check must not fail the build over.

    Each non-comment line of KNOWN_FAILURES_PATH is one allowlist entry in this
    script's own problem-line format. An entry containing a glob metacharacter
    (`*` or `?`) is matched with fnmatch and may cover several problem lines at
    once; any other entry must match a problem line exactly. That keeps the file
    able to describe a whole class of breakage without enumerating paths it must
    not carry.

    An entry that matches nothing is stale, not an error: it is reported so it
    can be deleted, and never changes the exit status. Anything NOT covered by
    an entry is a NEW failure and still fails the check.
    """
    path = root / KNOWN_FAILURES_PATH
    if not path.exists():
        return []
    known: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        known.append(line)
    return known


def known_failure_matches(entry: str, problem: str) -> bool:
    if "*" in entry or "?" in entry:
        return fnmatch.fnmatchcase(problem, entry)
    return problem == entry


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


def tracked_markdown_files(root: Path) -> list[Path]:
    try:
        out = subprocess.check_output(
            ["git", "ls-files", "*.md", "*.markdown"],
            cwd=root,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        files = [root / line for line in out.splitlines() if line]
        if files:
            return sorted(files)
    except (OSError, subprocess.CalledProcessError):
        pass

    skipped_dirs = {".git", "build", "dist", "__pycache__"}
    return sorted(
        path
        for path in root.rglob("*")
        if path.suffix.lower() in {".md", ".markdown"}
        and not any(part in skipped_dirs for part in path.parts)
    )


def strip_fenced_code(text: str) -> str:
    lines: list[str] = []
    in_fence = False
    fence_marker = ""
    for line in text.splitlines():
        stripped = line.lstrip()
        if not in_fence and (stripped.startswith("```") or stripped.startswith("~~~")):
            in_fence = True
            fence_marker = stripped[:3]
            lines.append("")
            continue
        if in_fence and stripped.startswith(fence_marker):
            in_fence = False
            fence_marker = ""
            lines.append("")
            continue
        lines.append("" if in_fence else line)
    return "\n".join(lines)


def slug_heading(text: str) -> str:
    text = HTML_TAG_RE.sub("", text)
    text = LINK_TEXT_RE.sub(r"\1", text)
    text = MARKDOWN_DECORATION_RE.sub("", text)
    text = text.strip().lower()

    chars: list[str] = []
    for char in text:
        if char.isalnum() or char in {"_", "-", " "}:
            chars.append(char)
    return "".join(chars).replace(" ", "-")


def anchors_for(path: Path) -> set[str]:
    text = strip_fenced_code(path.read_text(encoding="utf-8", errors="replace"))
    anchors: set[str] = set()
    seen: dict[str, int] = {}
    for line in text.splitlines():
        match = HEADING_RE.match(line)
        if not match:
            continue
        base = slug_heading(match.group(2))
        count = seen.get(base, 0)
        seen[base] = count + 1
        anchors.add(base if count == 0 else f"{base}-{count}")
    return anchors


def extract_target(raw: str) -> str:
    raw = raw.strip()
    if raw.startswith("<") and ">" in raw:
        return raw[1 : raw.index(">")]
    return raw.split()[0]


def is_external(target: str) -> bool:
    parsed = urllib.parse.urlparse(target)
    return bool(parsed.scheme) or target.startswith("//")


def display_path(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def validate_link(
    root: Path,
    source: Path,
    target: str,
    file_anchors: dict[Path, set[str]],
) -> str | None:
    target = extract_target(target)
    if not target or is_external(target):
        return None

    path_text, has_fragment, fragment = target.partition("#")
    fragment = urllib.parse.unquote(fragment)

    if not path_text:
        resolved = source
    else:
        path_text = urllib.parse.unquote(path_text)
        candidate = (source.parent / path_text).resolve()
        try:
            resolved = candidate.relative_to(root)
        except ValueError:
            return f"points outside repository: {target}"
        resolved = root / resolved

    if path_text and not resolved.exists():
        return f"missing target: {target}"

    if has_fragment and resolved.is_file() and resolved.suffix.lower() in {".md", ".markdown"}:
        if fragment and fragment not in file_anchors.get(resolved, set()):
            return f"missing anchor '#{fragment}' in {display_path(root, resolved)}"

    return None


def main() -> int:
    args = parse_args()
    root = Path(args.repo_root).resolve() if args.repo_root else git_root()
    files = tracked_markdown_files(root)
    file_anchors = {path: anchors_for(path) for path in files}
    problems: list[str] = []

    for path in files:
        text = strip_fenced_code(path.read_text(encoding="utf-8", errors="replace"))
        for regex in (INLINE_LINK_RE, IMAGE_LINK_RE, REFERENCE_LINK_RE):
            for match in regex.finditer(text):
                issue = validate_link(root, path, match.group(1), file_anchors)
                if issue:
                    problems.append(f"{display_path(root, path)}: {issue}")

    known_failures = load_known_failures(root)
    unique_problems = sorted(set(problems))

    matched_entries: set[str] = set()
    still_failing_known: set[str] = set()
    new_problems: list[str] = []
    for problem in unique_problems:
        covering = [e for e in known_failures if known_failure_matches(e, problem)]
        if covering:
            matched_entries.update(covering)
            still_failing_known.add(problem)
        else:
            new_problems.append(problem)
    resolved_known = sorted(e for e in known_failures if e not in matched_entries)

    if new_problems:
        print("Markdown link check failed:", file=sys.stderr)
        for problem in new_problems:
            print(f"  {problem}", file=sys.stderr)
        if still_failing_known:
            print(
                f"  ({len(still_failing_known)} additional known failure(s) allowlisted"
                f" in {KNOWN_FAILURES_PATH}, not shown)",
                file=sys.stderr,
            )
        return 1

    if resolved_known:
        print(
            f"NOTE: {len(resolved_known)} entr{'y' if len(resolved_known) == 1 else 'ies'} "
            f"in {KNOWN_FAILURES_PATH} no longer fail(s) — remove to burn down the allowlist:",
        )
        for problem in resolved_known:
            print(f"  {problem}")

    if still_failing_known:
        print(
            f"PASS: checked {len(files)} Markdown files "
            f"({len(still_failing_known)} pre-existing known failure(s) allowlisted, no NEW breakage)"
        )
    else:
        print(f"PASS: checked {len(files)} Markdown files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
