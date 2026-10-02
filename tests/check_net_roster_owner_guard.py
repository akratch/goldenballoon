#!/usr/bin/env python3
"""Compile + run the net_roster ownership-guard proof (local-Play beach-ball fix).

Standalone: it builds tests/test_net_roster_owner_guard.c against the roster
sources with the host C compiler and runs it, so it needs no CMake target and no
game ROM. The guard is what stops a local-Play boot from inheriting an online
session's process-global roster and stalling on network input that never comes.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

import harness_utils  # noqa: F401,E402 -- a direct run gets private saves, prefs and mods

ROOT = Path(__file__).resolve().parent.parent
SOURCES = [
    ROOT / "tests/test_net_roster_owner_guard.c",
    ROOT / "platform/net/net_roster_runtime.c",
    ROOT / "platform/net/net_roster.c",
    ROOT / "platform/net/match_manifest.c",
    ROOT / "platform/net/match_launch_descriptor.c",
]


def main() -> int:
    cc = os.environ.get("CC", "cc")
    for src in SOURCES:
        if not src.is_file():
            print(f"FAIL net_roster owner guard: missing source {src}",
                  file=sys.stderr)
            return 1
    with tempfile.TemporaryDirectory(prefix="mdkr64-roster-guard-") as tmp:
        binary = Path(tmp) / "net_roster_owner_guard"
        compile_cmd = [
            cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT),
            *[str(s) for s in SOURCES],
            "-o", str(binary),
        ]
        compiled = subprocess.run(compile_cmd, text=True,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, check=False)
        if compiled.returncode != 0:
            print("FAIL net_roster owner guard: compile failed", file=sys.stderr)
            print(compiled.stdout, file=sys.stderr)
            return 1
        run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=60, check=False)
        sys.stdout.write(run.stdout)
        if run.returncode != 0 or "test_net_roster_owner_guard: PASS" not in run.stdout:
            print(f"FAIL net_roster owner guard: exit {run.returncode}",
                  file=sys.stderr)
            return 1
    print("PASS net_roster owner guard: local-Play boot cannot inherit a "
          "foreign online roster; ownership is explicit; guard is idempotent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
