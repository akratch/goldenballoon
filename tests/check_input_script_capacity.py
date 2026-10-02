#!/usr/bin/env python3
"""Native boundary proof: CAPACITY input entries accepted, one more rejected,
never truncated.

The loader admitted 512 entries until 1.8, and the longest generated route
(check_resource_plateau.py's retry pulses) reached 481 of them, close enough
that one more navigation step would turn a passing gate into an overflow
abort. CAPACITY is the engine's MAX_SCRIPT (platform/platform_sdl_min.c); this
check is what holds the two together, and test_input_script_census.py holds
every committed script to it.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, VALIDATION_MARKERS,
                           fatal_re, resolve_binary)

CAPACITY = 4096


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--binary", type=Path,
                        help="explicit executable override for direct diagnostics")
    parser.add_argument("--rom", type=Path, required=True)
    args = parser.parse_args()
    binary = (args.binary or Path(resolve_binary(args.build))).resolve()
    rom = args.rom.resolve()
    with tempfile.TemporaryDirectory(prefix="mdkr-input-capacity-") as tmp:
        root = Path(tmp)
        for count in (CAPACITY, CAPACITY + 1):
            case = root / str(count)
            case.mkdir()
            script = case / "input.txt"
            script.write_text("".join(f"{1000 + i * 10} A 1\n" for i in range(count))
                              + "# trailing comments are not entries\n\n", encoding="utf-8")
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(("MDKR", "GE007_"))}
            env.update(MDKR_AUDIO="0", MDKR64_HIDDEN="1", MDKR_RENDERER="webgpu",
                       MDKR_APP_PREFS_DIR=str(case), MDKR_SAVE_DIR=str(case / "save"),
                       MDKR_VIDEO_CONFIG_PATH=str(case / "video.ini"))
            result = subprocess.run(
                [str(binary), "--rom", str(rom), "--headless-frames", "1",
                 "--window-size", "320x240", "--input-script", str(script)],
                env=env, cwd=case, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=60)
            expected = 0 if count == CAPACITY else 1
            # Exit 1 is expected for overflow, but an unrelated sanitizer or
            # renderer error must never be mistaken for a valid rejection.
            fatal = fatal_re(*ASSERT_MARKERS, *VALIDATION_MARKERS).search(result.stdout)
            if fatal is not None:
                raise RuntimeError(f"unexpected native diagnostic: {fatal.group(0)}\n"
                                   + result.stdout[-5000:])
            marker = (f"[input-script] loaded {CAPACITY} entries" if count == CAPACITY else
                      f"[input-script] too many entries (maximum {CAPACITY})")
            if result.returncode != expected or marker not in result.stdout:
                raise RuntimeError(f"{count} entries: exit={result.returncode}\n"
                                   + result.stdout[-5000:])
            if count > CAPACITY and "input-script] loaded" in result.stdout:
                raise RuntimeError("overflow was reported as a successful partial load")
    print(f"check_input_script_capacity: PASS ({CAPACITY} accepted, "
          f"{CAPACITY + 1} rejected)")


if __name__ == "__main__":
    main()
