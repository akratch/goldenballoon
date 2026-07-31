#!/usr/bin/env python3
"""Reach new-save filename entry and require a sanitizer-clean render cycle.

This route exists for MEM-01. The ROM reserves four bytes for
`gCurFilenameCharBeingDrawn`, but the decomp used to declare one byte and pass
its address to `draw_text()` as a NUL-terminated string. AddressSanitizer then
aborted deterministically as soon as FILE_SELECT rendered the character grid.

The check deliberately runs in an isolated working directory: filename entry
creates save state, and a regression check must never remove or overwrite a
developer's normal `save/eeprom.bin`.

Normal verification:

    python3 tests/check_filename_entry.py --build build-asan

Positive control, against a binary with the scalar declaration restored:

    python3 tests/check_filename_entry.py \
        --build /path/to/broken/mdkr64 --expect-asan

Always muted and headless. Exit zero means the selected arm behaved exactly as
expected.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import resolve_binary


DEFAULT_SCRIPT = Path("tests/input_scripts/adventure_hub_drive.txt")
DEFAULT_FRAMES = 2300
FILE_SELECT_MENU_ID = 6

MENU_RE = re.compile(r"menu_init: menuId=(\d+)")
SANITIZER_RE = re.compile(
    r"AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"global-buffer-overflow|runtime error:"
)
FATAL_RE = re.compile(r"\[CRASH\]|\[FATAL\]|Assertion")
MEM01_RE = re.compile(r"gCurFilenameCharBeingDrawn|font\.c:408")


def output_tail(output: str, lines: int = 80) -> str:
    return "\n".join(output.splitlines()[-lines:])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", default="build-asan",
                    help="build directory or mdkr64 binary (default: build-asan)")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--script", default=str(DEFAULT_SCRIPT))
    ap.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    ap.add_argument("--renderer", choices=("gl", "webgpu"), default="gl")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--expect-asan", action="store_true",
                    help="positive-control mode: require the historical MEM-01 ASan abort")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    script = Path(args.script).expanduser().resolve()
    for label, path in (("binary", binary), ("ROM", rom), ("input script", script)):
        if not path.is_file():
            print(f"check_filename_entry: FAIL — missing {label}: {path}",
                  file=sys.stderr)
            return 1

    # Do not inherit a gameplay/debug hook that silently changes the route.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MDKR_")}
    env.update({
        "MDKR_AUDIO": "0",
        "MDKR_TRACE": "1",
        "MDKR_RENDERER": args.renderer,
    })

    cmd = [
        str(binary),
        "--headless-frames", str(args.frames),
        "--input-script", str(script),
        "--rom", str(rom),
    ]
    if args.verbose:
        print("$ " + " ".join(cmd))

    with tempfile.TemporaryDirectory(prefix="mdkr_filename_") as run_dir:
        try:
            proc = subprocess.run(
                cmd,
                cwd=run_dir,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=args.timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            partial = (exc.stdout or b"").decode("utf-8", "replace")
            print(f"check_filename_entry: FAIL — timed out after {args.timeout}s",
                  file=sys.stderr)
            if partial:
                print(output_tail(partial), file=sys.stderr)
            return 1

    output = proc.stdout.decode("utf-8", "replace")
    menus = [int(match.group(1)) for match in MENU_RE.finditer(output)]
    reached_file_select = FILE_SELECT_MENU_ID in menus
    sanitizer_hit = SANITIZER_RE.search(output) is not None
    fatal_hit = FATAL_RE.search(output) is not None
    mem01_hit = MEM01_RE.search(output) is not None

    failures: list[str] = []
    if not reached_file_select:
        failures.append(
            f"route never reached FILE_SELECT menuId={FILE_SELECT_MENU_ID}; saw {menus}"
        )

    if args.expect_asan:
        if proc.returncode == 0:
            failures.append("broken control exited zero instead of aborting")
        if not sanitizer_hit:
            failures.append("broken control emitted no sanitizer diagnostic")
        if not mem01_hit:
            failures.append(
                "sanitizer output did not identify gCurFilenameCharBeingDrawn/font.c:408"
            )
    else:
        if proc.returncode != 0:
            failures.append(f"runner exited {proc.returncode}")
        if sanitizer_hit:
            failures.append("sanitizer diagnostic appeared in the fixed arm")
        if fatal_hit:
            failures.append("fatal/crash/assertion marker appeared in the fixed arm")

    if failures:
        print("check_filename_entry: FAIL", file=sys.stderr)
        for failure in failures:
            print("  - " + failure, file=sys.stderr)
        if args.verbose or proc.returncode != 0 or sanitizer_hit or fatal_hit:
            print("--- output tail ---", file=sys.stderr)
            print(output_tail(output), file=sys.stderr)
        return 1

    if args.expect_asan:
        print("check_filename_entry: PASS (control) — FILE_SELECT reproduced "
              "the historical MEM-01 ASan abort")
    else:
        print("check_filename_entry: PASS — FILE_SELECT rendered through frame "
              f"{args.frames} with no sanitizer/fatal output")
    return 0


if __name__ == "__main__":
    sys.exit(main())
