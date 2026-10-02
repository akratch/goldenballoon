#!/usr/bin/env python3
"""All-cells proof of the online-catalog-id -> engine Character mapping.

Standalone: it builds tests/test_online_character_map.c against the single-source
map TU (game/src/online/online_character_map.c) with the host C compiler and runs
it, so it needs no CMake target and no game ROM. The map is the seam the online
direct-boot racer spawn rides (menu.c get_character_id_from_slot ->
mdkr_online_character_to_engine); the online lobby numbers racers in its OWN
catalog order, which differs from the engine Character enum, so a missing
translation raced the WRONG character (Tiptup->Conker, T.T.->Diddy). This asserts
every one of the ten grid cells spawns its own racer. Both endpoints agree by
construction: the map is a pure function over the shared consensus descriptor.
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
    ROOT / "tests/test_online_character_map.c",
    ROOT / "game/src/online/online_character_map.c",
]


def main() -> int:
    cc = os.environ.get("CC", "cc")
    for src in SOURCES:
        if not src.is_file():
            print(f"FAIL online character map: missing source {src}",
                  file=sys.stderr)
            return 1
    with tempfile.TemporaryDirectory(prefix="mdkr64-char-map-") as tmp:
        binary = Path(tmp) / "online_character_map"
        compile_cmd = [
            cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
            "-DMDKR_ENABLE_ONLINE_BETA=1",
            "-I", str(ROOT / "game/src"),
            "-I", str(ROOT / "game/include"),
            *[str(s) for s in SOURCES],
            "-o", str(binary),
        ]
        compiled = subprocess.run(compile_cmd, text=True,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, check=False)
        if compiled.returncode != 0:
            print("FAIL online character map: compile failed", file=sys.stderr)
            print(compiled.stdout, file=sys.stderr)
            return 1
        run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=60, check=False)
        sys.stdout.write(run.stdout)
        if run.returncode != 0 or \
                "test_online_character_map: PASS" not in run.stdout:
            print(f"FAIL online character map: exit {run.returncode}",
                  file=sys.stderr)
            return 1
    print("PASS online character map: all 10 grid cells spawn their own racer; "
          "online-id -> engine Character map is a bijection; the raw-id defect "
          "(Tiptup->Conker, T.T.->Diddy) is pinned")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
