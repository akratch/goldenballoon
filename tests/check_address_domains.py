#!/usr/bin/env python3
"""Keep native pointer-to-32-bit crossings inside named domain boundaries.

The compiler already rejects direct pointer/integer casts. This source gate owns
the easy-to-hide two-cast spelling, such as ``(u32)(uintptr_t)pointer``, which
otherwise suppresses that diagnostic while still discarding the high half.
Matching-only ``!NATIVE_PORT`` branches are excluded; unknown preprocessor
conditions are scanned conservatively in both directions.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOTS = (ROOT / "game", ROOT / "platform")
ALLOWED = {
    Path("game/include/dkr_native_ptr.h"): 1,
    Path("platform/address_domains.h"): 2,
    # The clean-room audio engine's AL param helpers pack exactly a u32 into a
    # void* (alParamFromS32/alParamFromF32Bits widen (u32)->(uintptr_t)), so
    # the two narrowing reads recover precisely the stored 32-bit value —
    # round-trip exact by construction, per libaudio's N64 32-bit param ABI.
    Path("platform/synthInternals.h"): 2,
}
NARROW_RE = re.compile(
    r"\(\s*(u32|s32|uint32_t|int32_t|dkrptr32|"
    r"MdkrArenaToken|MdkrRomOffset)\s*\)\s*"
    r"\(\s*(u?intptr_t)\s*\)"
)
DIRECTIVE_RE = re.compile(
    r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$"
)


@dataclass
class Conditional:
    parent_active: bool
    known: bool
    condition: bool
    branch_taken: bool
    else_seen: bool = False


def native_condition(kind: str, expression: str) -> tuple[bool, bool]:
    expression = re.sub(r"\s+", "", expression)
    if kind == "ifdef" and expression == "NATIVE_PORT":
        return True, True
    if kind == "ifndef" and expression == "NATIVE_PORT":
        return True, False
    if kind in {"if", "elif"}:
        if expression in {"defined(NATIVE_PORT)", "definedNATIVE_PORT"}:
            return True, True
        if expression in {
            "!defined(NATIVE_PORT)",
            "!definedNATIVE_PORT",
        }:
            return True, False
    # An unrelated build condition may be true or false. Scan both its primary
    # and alternate branches by treating each as active under the parent.
    return False, True


def active_lines(source: str) -> list[tuple[int, str]]:
    active = True
    stack: list[Conditional] = []
    result: list[tuple[int, str]] = []
    in_block_comment = False

    for number, raw in enumerate(source.splitlines(), 1):
        directive = DIRECTIVE_RE.match(raw)
        if directive:
            kind, expression = directive.groups()
            if kind in {"ifdef", "ifndef", "if"}:
                known, condition = native_condition(kind, expression)
                stack.append(
                    Conditional(
                        active, known, condition,
                        branch_taken=known and condition,
                    )
                )
                active = active and (condition if known else True)
            elif kind == "elif" and stack:
                frame = stack[-1]
                known, condition = native_condition(kind, expression)
                frame.else_seen = True
                if frame.known and frame.branch_taken:
                    active = False
                else:
                    frame.known = frame.known and known
                    frame.condition = condition
                    frame.branch_taken = (
                        frame.branch_taken or (known and condition)
                    )
                    active = frame.parent_active and (
                        condition if known else True
                    )
            elif kind == "else" and stack:
                frame = stack[-1]
                frame.else_seen = True
                active = frame.parent_active and (
                    not frame.branch_taken if frame.known else True
                )
            elif kind == "endif" and stack:
                active = stack.pop().parent_active
            continue
        if not active:
            continue

        # Remove comments without joining lines, preserving useful diagnostics.
        line = raw
        cleaned: list[str] = []
        index = 0
        while index < len(line):
            if in_block_comment:
                end = line.find("*/", index)
                if end < 0:
                    index = len(line)
                    continue
                in_block_comment = False
                index = end + 2
                continue
            block = line.find("/*", index)
            single = line.find("//", index)
            if single >= 0 and (block < 0 or single < block):
                cleaned.append(line[index:single])
                break
            if block < 0:
                cleaned.append(line[index:])
                break
            cleaned.append(line[index:block])
            in_block_comment = True
            index = block + 2
        result.append((number, "".join(cleaned)))
    return result


def violations(path: Path, source: str) -> list[tuple[int, str]]:
    return [
        (number, line.strip())
        for number, line in active_lines(source)
        if NARROW_RE.search(line)
    ]


def main() -> int:
    found: dict[Path, list[tuple[int, str]]] = {}
    for source_root in SOURCE_ROOTS:
        for path in sorted(source_root.rglob("*")):
            if path.suffix not in {".c", ".h"}:
                continue
            relative = path.relative_to(ROOT)
            matches = violations(
                relative, path.read_text(encoding="utf-8", errors="replace")
            )
            if matches:
                found[relative] = matches

    failures: list[str] = []
    unexpected = sorted(set(found) - set(ALLOWED))
    if unexpected:
        failures.extend(
            f"raw native narrowing {path}:{number}: {line}"
            for path in unexpected
            for number, line in found[path]
        )
    for path, expected_count in ALLOWED.items():
        actual = len(found.get(path, []))
        if actual != expected_count:
            failures.append(
                f"{path}: expected {expected_count} named boundary "
                f"conversion(s), found {actual}"
            )

    # Both directions: reject a native narrowing, but do not accuse the
    # matching-only N64 branch that is intentionally outside the host contract.
    bad = "u32 f(void *p) { return (u32)(uintptr_t)p; }\n"
    native_only = (
        "#ifdef NATIVE_PORT\n" + bad + "#else\nu32 f(void *p) { return 0; }\n#endif\n"
    )
    matching_only = "#ifndef NATIVE_PORT\n" + bad + "#endif\n"
    if not violations(Path("control.c"), bad):
        failures.append("unguarded positive control escaped")
    if not violations(Path("control.c"), native_only):
        failures.append("NATIVE_PORT positive control escaped")
    if violations(Path("control.c"), matching_only):
        failures.append("matching-only negative control was misclassified")

    if failures:
        raise AssertionError(
            "address-domain source contract failed:\n  "
            + "\n  ".join(failures)
        )

    total = sum(len(items) for items in found.values())
    print(
        "check_address_domains: PASS — "
        f"{total} unavoidable native narrowing conversions confined to "
        f"{len(ALLOWED)} named boundary headers; raw and conditional controls pass"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
