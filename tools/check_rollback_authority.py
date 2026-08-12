#!/usr/bin/env python3
"""Fail closed when the race-authority declaration census changes.

This is intentionally a review gate, not a C parser. It fingerprints mutable
file-scope declarations in the translated game sources after stripping line
numbers and whitespace. A changed declaration must therefore update the
authority inventory in the same reviewed change. The runtime proof remains the
v3 hash and restore matrix; this gate prevents silent scope growth before that
proof runs.
"""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOTS = (ROOT / "game" / "src",)
DECLARATION = re.compile(
    r"^(?:static\s+)?(?:signed\s+|unsigned\s+)?"
    r"(?:char|short|int|long|float|double|[us](?:8|16|32|64)|"
    r"[A-Z][A-Za-z0-9_]*)\s+[\s*]*(?:g|s)[A-Za-z0-9_]+"
    r"(?:\s*\[[^;=]*\])?\s*(?:=|;)")
EXPECTED_COUNT = 1619
EXPECTED_DIGEST = "ecf1f6332b08b50c3ac6454bcb144632ec9c3375e0701d2575f0f4787c8d0dbe"


def _code_without_comments_or_literals(
    line: str, in_block_comment: bool
) -> tuple[str, bool]:
    """Return lexical code suitable for declaration and brace accounting."""
    output: list[str] = []
    index = 0
    quote = ""
    escaped = False
    while index < len(line):
        char = line[index]
        following = line[index + 1] if index + 1 < len(line) else ""
        if in_block_comment:
            if char == "*" and following == "/":
                in_block_comment = False
                output.extend((" ", " "))
                index += 2
            else:
                output.append(" ")
                index += 1
            continue
        if quote:
            output.append(" ")
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
            index += 1
            continue
        if char == "/" and following == "*":
            in_block_comment = True
            output.extend((" ", " "))
            index += 2
        elif char == "/" and following == "/":
            output.extend(" " for _ in line[index:])
            break
        elif char in {'"', "'"}:
            quote = char
            output.append(" ")
            index += 1
        else:
            output.append(char)
            index += 1
    return "".join(output), in_block_comment


def scan_source(relative: str, source: str) -> list[str]:
    """Fingerprint zero-indented declarations (this tree's file-scope style)."""
    rows: list[str] = []
    in_block_comment = False
    for raw in source.splitlines():
        code, in_block_comment = _code_without_comments_or_literals(
            raw, in_block_comment
        )
        stripped = code.strip()
        if code == code.lstrip() and not stripped.startswith("#"):
            declaration = " ".join(stripped.split())
            if DECLARATION.match(declaration) and not declaration.startswith("const "):
                rows.append(f"{relative}|{declaration}")
    if in_block_comment:
        raise ValueError(f"unterminated block comment in {relative}")
    return rows


def census(extra: str = "") -> list[str]:
    rows: list[str] = []
    for root in SOURCE_ROOTS:
        for path in sorted(root.rglob("*")):
            if path.suffix not in {".c", ".cpp"}:
                continue
            relative = path.relative_to(ROOT).as_posix()
            rows.extend(scan_source(
                relative, path.read_text(encoding="utf-8", errors="strict")
            ))
    if extra:
        rows.append(extra)
    return sorted(rows)


def fingerprint(rows: list[str]) -> str:
    return hashlib.sha256(("\n".join(rows) + "\n").encode()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--print-digest", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    rows = census()
    digest = fingerprint(rows)
    if args.print_digest:
        print(len(rows), digest)
        return 0
    failures: list[str] = []
    if len(rows) != EXPECTED_COUNT or digest != EXPECTED_DIGEST:
        failures.append(
            "mutable file-scope declaration census changed "
            f"(expected {EXPECTED_COUNT}/{EXPECTED_DIGEST}, got {len(rows)}/{digest}); "
            "classify the delta in docs/ref/rollback-authority-v1.md before "
            "updating this reviewed fingerprint")
    if args.self_test:
        injected = fingerprint(census("game/src/negative_control.c|s32 gOmittedAuthority = 1;"))
        if injected == EXPECTED_DIGEST:
            failures.append("negative control did not change the authority census")
        scope_probe = scan_source(
            "game/src/scope_probe.c",
            "s32 gReviewedGlobal;\nvoid probe(void) {\n  s32 gLocalDecoy;\n}\n",
        )
        if scope_probe != ["game/src/scope_probe.c|s32 gReviewedGlobal;"]:
            failures.append("lexical scope control counted a function-local decoy")
    required = {
        ROOT / "platform" / "sim_hash.c": (
            "v3 retains v2 and adds", "Deliberate v3 exclusions"),
        ROOT / "platform" / "rollback" / "rollback_snapshot.h": (
            "MDKR_ROLLBACK_RANGE_HOST_HANDLE",),
        ROOT / "platform" / "rollback" / "rollback_events.c": (
            "progression_write", "resimulating"),
    }
    for path, markers in required.items():
        text = path.read_text(encoding="utf-8")
        for marker in markers:
            if marker not in text:
                failures.append(f"{path.relative_to(ROOT)} lost authority marker {marker!r}")
    if failures:
        print("check_rollback_authority: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(f"check_rollback_authority: PASS ({len(rows)} declarations, {digest[:12]})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
