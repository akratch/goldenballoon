#!/usr/bin/env python3
"""Reject `updateRate` used as a stand-in for launch-time simulation cadence.

The rule (`docs/CAMPAIGN_FAITHFUL_ENHANCED.md` section 3.1): code that
behaves differently under the Enhanced simulation cadence must gate on
`platform_sim_cadence_is_enhanced()` (declared in `platform/platform_os.h`,
defined in `platform/platform_sdl_min.c`) -- the launch-time cadence -- and
never on the per-tick `updateRate` parameter threaded through gameplay code.

Why this is load-bearing, not style: under Original, a lagging host tick
legitimately arrives with `updateRate` 3 or more (see the "mega-step"
accounting in `platform/stubs_dkr.c`), and the authored code must handle that
byte-identically to a normal tick, because the determinism contract cares
most about exactly those frames. `updateRate == 1` happens to be true on
almost every Original tick and `updateRate == 2` happens to be true on almost
every Enhanced tick, which makes them tempting as a free mode test -- but a
lag tick breaks that correlation, so keying a mode branch off either value
would change Original's own output on exactly the tick a real player's
machine actually hitches. Two correct examples already exist in this tree:
`mdkr_boss_cadence_clamp` in `game/src/racer.c` and `menu_stick_delay_amount`
in `game/src/menu.c`, both of which call
`platform_sim_cadence_is_enhanced()` directly instead of inspecting
`updateRate`.

What this gate does and does not flag
--------------------------------------
`updateRate` is threaded through an enormous amount of gameplay code as an
ordinary scale factor (`racer->throttle += updateRateF * 0.016`, `armedFor +=
updateRate`, `while (updateRate > 0) { ... }`) -- that is correct and must
not be flagged. The gate matches only an equality/inequality comparison
between `updateRate` and the literal `1` or `2`, in either operand order:
`updateRate == 1`, `updateRate != 2`, `1 == updateRate`, `2 != updateRate`,
and so on. Those four forms are exactly the ones that quietly reconstruct
"is this Original / is this Enhanced" from a value that is not the mode.
Ordinary threshold/lag arithmetic (`updateRate > 0`, `updateRate >= 3`) is
untouched: those don't collapse to a two-value mode test the way `==1`/`==2`/
`!=1`/`!=2` do, and rejecting them would make this gate the kind that gets
disabled within a week.

Scope: `game/src/**/*.c`, `game/src/**/*.h`, `platform/**/*.c`,
`platform/**/*.h`, excluding `platform/platform_sdl_min.c`'s own definition
of `platform_sim_cadence_is_enhanced()` (which legitimately does not exist,
i.e. there is nothing there to false-positive on) and this test file itself.

Positive control: `_self_test()` feeds the matcher a seeded
`if (updateRate == 2) { /* Enhanced-only behaviour */ }` snippet and asserts
it is rejected, alongside snippets drawn from the two real correct call
sites (by name, not full source, since they are gameplay files this gate
also scans) that must NOT be rejected.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOTS = (ROOT / "game" / "src", ROOT / "platform")
SELF = Path(__file__).resolve()

# updateRate == 1 | updateRate == 2 | updateRate != 1 | updateRate != 2,
# and the same four with operands swapped (1 == updateRate, etc).
_FORWARD = re.compile(r"\bupdateRate\s*(==|!=)\s*[12]\b")
_REVERSE = re.compile(r"(?<![\w.])[12]\s*(==|!=)\s*updateRate\b")


def find_mode_tests(text: str) -> list[tuple[int, str]]:
    """Return (line_number, matched_snippet) for each disallowed comparison."""
    hits: list[tuple[int, str]] = []
    for line_no, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("//") or stripped.startswith("*"):
            continue
        for pattern in (_FORWARD, _REVERSE):
            for match in pattern.finditer(line):
                hits.append((line_no, match.group(0)))
    return hits


def source_files() -> list[Path]:
    files: list[Path] = []
    for root in SOURCE_ROOTS:
        for ext in ("*.c", "*.h"):
            files.extend(sorted(root.rglob(ext)))
    return [f for f in files if f.resolve() != SELF]


def scan() -> list[str]:
    failures: list[str] = []
    for path in source_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        for line_no, snippet in find_mode_tests(text):
            failures.append(
                f"{path.relative_to(ROOT)}:{line_no}: `{snippet}` tests "
                "updateRate as a cadence mode -- gate on "
                "platform_sim_cadence_is_enhanced() instead"
            )
    return failures


def _self_test() -> None:
    """Positive control, plus the two real correct patterns as negatives."""
    seeded_bad = "if (updateRate == 2) { /* Enhanced-only behaviour */ }"
    assert find_mode_tests(seeded_bad), (
        "positive control failed: `updateRate == 2` used as a mode test was "
        "not detected"
    )

    for bad in (
        "if (updateRate != 1) enhanced_only();",
        "if (1 == updateRate) original_only();",
        "if (2 != updateRate) return;",
    ):
        assert find_mode_tests(bad), f"positive control failed for: {bad}"

    # The two correct call sites in this tree gate on the launch-time
    # cadence function, never on updateRate's value, and must not trip.
    good = (
        "if (!platform_sim_cadence_is_enhanced()) {\n"
        "    return;\n"
        "}\n"
        "static s32 menu_stick_delay_amount(void) {\n"
        "    return platform_sim_cadence_is_enhanced() ? (STICK_DELAY_AMOUNT * 2)\n"
        "                                              : STICK_DELAY_AMOUNT;\n"
        "}\n"
    )
    assert not find_mode_tests(good), "false positive on the cadence function itself"

    # Ordinary arithmetic/threshold uses of updateRate must never trip.
    arithmetic = "\n".join([
        "racer->throttle += updateRateF * 0.016;",
        "while (updateRate > 0) { step(); updateRate--; }",
        "if (gRacerFXData[temp].unk3 - updateRate > 0) { keep(); }",
        "armedFor += updateRate;",
        "if (updateRate >= 3) { consolidate_lag(); }",
    ])
    assert not find_mode_tests(arithmetic), (
        "false positive on ordinary updateRate arithmetic"
    )

    # A comment mentioning the pattern must not trip -- only live code does.
    commented = "// historically this used to check updateRate == 2 here"
    assert not find_mode_tests(commented), "comment line was scanned as code"


def main() -> int:
    _self_test()
    if "--self-test-only" in sys.argv:
        print("check_cadence_gating: positive control passed")
        return 0

    failures = scan()
    if failures:
        print("check_cadence_gating: FAIL")
        for failure in failures:
            print(f"  {failure}")
        print(
            f"\n{len(failures)} cadence mode test(s) keyed on updateRate. "
            "Under Original a lag tick legitimately arrives with updateRate "
            "3 or more, so this reconstructs the mode from a value that "
            "isn't it -- gate on platform_sim_cadence_is_enhanced() instead. "
            "See docs/CAMPAIGN_FAITHFUL_ENHANCED.md section 3.1."
        )
        return 1

    print(
        f"check_cadence_gating: PASS ({len(source_files())} source files "
        "clean; positive control confirmed the scanner is not vacuous)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
