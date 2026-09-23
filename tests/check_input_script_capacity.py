#!/usr/bin/env python3
"""Native boundary proof: 512 input entries accepted, 513 rejected, not truncated."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, VALIDATION_MARKERS,
                           fatal_re, resolve_binary)


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
        for count in (512, 513):
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
            expected = 0 if count == 512 else 1
            # Exit 1 is expected for overflow, but an unrelated sanitizer or
            # renderer error must never be mistaken for a valid rejection.
            fatal = fatal_re(*ASSERT_MARKERS, *VALIDATION_MARKERS).search(result.stdout)
            if fatal is not None:
                raise RuntimeError(f"unexpected native diagnostic: {fatal.group(0)}\n"
                                   + result.stdout[-5000:])
            marker = ("[input-script] loaded 512 entries" if count == 512 else
                      "[input-script] too many entries (maximum 512)")
            if result.returncode != expected or marker not in result.stdout:
                raise RuntimeError(f"{count} entries: exit={result.returncode}\n"
                                   + result.stdout[-5000:])
            if count == 513 and "input-script] loaded" in result.stdout:
                raise RuntimeError("overflow was reported as a successful partial load")
    print("check_input_script_capacity: PASS (512 accepted, 513 rejected)")


if __name__ == "__main__":
    main()
