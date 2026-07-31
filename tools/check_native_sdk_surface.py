#!/usr/bin/env python3
"""Guard the native build against re-admitting SGI libultra SDK source.

Ported from mgb64's tools/check_native_sdk_surface.py and adapted to mdkr64's
post-audio-swap reality: `platform/audio_compat.c`, `audio_event_queue.c` and
`audio_fx_transfer.c` (the clean-room engine ported from mgb64) replaced the 49
decompiled SGI-legend synthesiser sources that used to live under
`game/libultra/src/audio/` (commit 2a180eb). `game/libultra/` still supplies
compat headers and non-audio OS/IO/libc shims used by the decompiled game code,
and `game/include/PR/` still carries the original SGI-authored header
declarations required to compile against — those are a *documented*, expected
exception (see NOTICE.md), not covered here.

This guard asserts three independent things about the CURRENT tree:

  1. The native CMake target does not reference any source file (.c/.cc/.s)
     rooted under game/libultra/ — i.e. the audio glob is gone and nobody
     re-added an explicit source reference into that tree.
  2. No file(GLOB ...) in CMakeLists.txt reaches into game/libultra/ — a glob
     would silently re-admit any file dropped back into that tree, bypassing
     assertion 1's explicit-reference check entirely.
  3. No SGI/proprietary legend text, and no vendor RCS/copyright legend
     (a second, wider pattern — see below), appears anywhere under game/.

Each assertion is independent and reports its own PASS/FAIL, and every hit is
a failure: game/include/ now carries first-party clean-room declaration text,
so there is no allowlist and no expected-red assertion. Any legend hit is a
real regression.

Assertion 3 uses TWO patterns. The primary pattern (Silicon Graphics /
UNPUBLISHED PROPRIETARY) catches the SGI-style boilerplate most PR/ headers
carry. A second, wider "vendor RCS/copyright legend" pattern catches headers
that carry a Nintendo copyright block and/or a CVS/RCS keyword-expansion tag
($Header/$Revision/$Date/$Source/$Author/$Id) WITHOUT the SGI boilerplate —
verified present in game/include/PR/{gs2dex,os_flash,os_gbpak,os_motor,
os_version,os_voice,rdb}.h, game/include/sys/{asm,regdef}.h and
game/include/ultrahost.h. This second pattern is deliberately scoped to
game/include/ only (never game/libultra/src/**), because plain RCS tags alone
also appear throughout the genuinely-decompiled, already-documented
game/libultra/src/{io,libc}/*.c implementation (see NOTICE.md's "Decompiled
game code" section) — those are a different, already-accepted provenance
class, not the "declarations only" SDK-header claim this guard polices, and
would be a flood of false positives if the wider pattern applied there.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


# A source-file reference: something ending in a compiled-source extension.
# Directory includes (target_include_directories) never carry these suffixes,
# so this pattern only ever matches an actual build-source reference.
LIBULTRA_SOURCE_REF_RE = re.compile(r"game/libultra/\S*\.(?:c|cc|cpp|s)\b")

# Matches a whole file(GLOB ...) invocation, including CONFIGURE_DEPENDS and
# multi-line argument lists, so a glob whose path arguments span lines is
# still caught.
GLOB_CALL_RE = re.compile(r"file\(\s*GLOB[^)]*\)", re.IGNORECASE | re.DOTALL)

# Task-specified legend pattern: either phrase anywhere in a game/ file is a
# proprietary-notice hit. Applied everywhere under game/.
SGI_LEGEND_RE = re.compile(r"Silicon Graphics|UNPUBLISHED PROPRIETARY")

# Wider "vendor RCS/copyright legend" pattern: a Nintendo copyright block (not
# always accompanied by the SGI boilerplate above -- several PR/ headers carry
# only a short "Copyright (C) 19xx Nintendo." credit line), or a bare CVS/RCS
# keyword-expansion tag. Scoped to game/include/ only -- see the module
# docstring for why it must not apply under game/libultra/src/.
VENDOR_RCS_LEGEND_RE = re.compile(
    r"Copyright\s*\([Cc]\)[^\n]{0,60}NINTENDO|\$(?:Header|Revision|Date|Source|Author|Id|RCSfile):",
    re.IGNORECASE,
)


def check_no_cmake_source_into_libultra(cmake_text: str) -> list[str]:
    """Assertion 1: no explicit CMake source reference under game/libultra/."""
    failures = []
    for match in LIBULTRA_SOURCE_REF_RE.finditer(cmake_text):
        failures.append(f"CMake references a build source under game/libultra/: {match.group(0)}")
    return failures


def check_no_glob_into_libultra(cmake_text: str) -> list[str]:
    """Assertion 2: no file(GLOB ...) call's path arguments reach game/libultra/."""
    failures = []
    for match in GLOB_CALL_RE.finditer(cmake_text):
        block = match.group(0)
        if "game/libultra" in block:
            first_line = block.splitlines()[0]
            failures.append(f"file(GLOB ...) call reaches into game/libultra/: {first_line} ...")
    return failures


def check_no_sgi_legend_under_game(root: Path) -> list[str]:
    """Assertion 3: no SGI/proprietary legend, and no vendor RCS/copyright
    legend, anywhere under game/ (the second pattern scoped to game/include/;
    see the module docstring)."""
    failures = []
    game_dir = root / "game"
    include_dir = root / "game" / "include"
    if not game_dir.is_dir():
        return [f"expected directory missing: {game_dir}"]
    for path in sorted(game_dir.rglob("*")):
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            failures.append(f"could not read {path}: {exc}")
            continue
        rel = path.relative_to(root).as_posix()
        if SGI_LEGEND_RE.search(text):
            failures.append(f"SGI/proprietary legend text found in {rel}")
        elif include_dir in path.parents and VENDOR_RCS_LEGEND_RE.search(text):
            # elif: a file already reported by the primary pattern is not
            # double-reported under the secondary one.
            failures.append(f"vendor RCS/copyright legend text found in {rel}")
    return failures


def run_assertion(label: str, failures: list[str]) -> bool:
    """Print one assertion's report and return whether it passed.

    Every failure is a real one: there is no allowlist, so a hit here is
    always a regression.
    """
    # Every line goes to the SAME stream (stdout), flushed immediately, so
    # interleaving with other assertions can never scramble one assertion's
    # own report.
    print(f"== {label} ==", flush=True)
    if not failures:
        print("PASS", flush=True)
        return True

    for failure in failures:
        print(f"FAIL: {failure}", flush=True)
    print(f"FAIL ({len(failures)} issue(s))", flush=True)
    return False


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=".", help="repository root")
    args = parser.parse_args(argv)

    root = Path(args.repo_root).resolve()
    cmake_path = root / "CMakeLists.txt"
    if not cmake_path.is_file():
        print(f"FAIL: missing {cmake_path}", file=sys.stderr)
        return 1
    cmake_text = cmake_path.read_text(encoding="utf-8", errors="replace")

    all_ok = True
    all_ok &= run_assertion(
        "1. no native CMake source reference into game/libultra/",
        check_no_cmake_source_into_libultra(cmake_text),
    )
    all_ok &= run_assertion(
        "2. no file(GLOB ...) reaching into game/libultra/",
        check_no_glob_into_libultra(cmake_text),
    )
    all_ok &= run_assertion(
        "3. no SGI/proprietary legend text under game/",
        check_no_sgi_legend_under_game(root),
    )

    print()
    if not all_ok:
        print(
            "check_native_sdk_surface: FAIL -- see above. Every hit is a real "
            "regression; game/include/ carries first-party clean-room text and "
            "no exception list exists.",
            file=sys.stderr,
        )
        return 1

    print("check_native_sdk_surface: PASS -- native SDK surface guard passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
