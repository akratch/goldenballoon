#!/usr/bin/env python3
"""Browser-arm unit tests: tools/web/run_emscripten_unit_tests.sh, in the suite.

CTest builds natively only. The code that behaves differently in the browser
-- the no-replace move that sets a damaged controller pak or ghost aside
(Emscripten's file system has no hard links, so that move has an arm of its
own) and the ghost bank that uses it, including its request to commit browser
storage after a set-aside -- is built with the pinned emcc and run under node,
on the in-memory file system the browser's save mount sits on. Build output
goes to a private temporary directory.

Without the pinned Emscripten or node this exits 77: SKIPPED, named by
tools/run_checks.py, and a failure in a release run, which must cover the
browser lane. ROM-free; opens no window.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import harness_utils  # noqa: F401 -- a direct run gets private saves, prefs and mods

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "web" / "run_emscripten_unit_tests.sh"
SKIP_EXIT = 77
UNITS = ("fs_set_aside", "ghost_bank")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="mdkr-emscripten-units-") as out:
        try:
            result = subprocess.run(
                ["bash", str(SCRIPT), out], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=1200, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"check_emscripten_units: FAIL -- {error}")
            return 1
    print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.returncode == SKIP_EXIT:
        rule = "=" * 72
        print(f"{rule}\ncheck_emscripten_units: SKIPPED -- the pinned Emscripten "
              f"and node are not available here, so the browser arms of "
              f"{', '.join(UNITS)} did not run\n{rule}")
        return SKIP_EXIT
    missing = [unit for unit in UNITS if f"PASS: {unit}" not in result.stdout]
    if result.returncode or missing:
        print(f"check_emscripten_units: FAIL -- exit {result.returncode}"
              + (f"; no PASS from {', '.join(missing)}" if missing else ""))
        return 1
    print(f"check_emscripten_units: PASS -- {', '.join(UNITS)} under node")
    return 0


if __name__ == "__main__":
    sys.exit(main())
