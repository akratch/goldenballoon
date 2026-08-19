#!/usr/bin/env python3
"""Prove the shadow static caster cache resets at level load in SHIPPING builds.

Why this exists
---------------
Wave "shadowplay"'s follow-up review found that `gfx_shadow_stage_begin()` was
only reachable through `level_load()`'s *diagnostic* path: the call sat inside
`if (mdkr_resource_trace_enabled())`, so a shipping build (no `MDKR_TRACE`,
no `MDKR_RESOURCE_STATS`) never reset the stage cache. Every previously loaded
level's static geometry kept casting into the current world, and recycled
arena addresses false-dedup'ed the *new* level's casters out of the map.

Nothing caught it because every world-shadow check exports `MDKR_TRACE=1`,
which enabled the reset path as a side effect. This gate runs the exact same
route with and without `MDKR_TRACE` and requires the terminal
`[WORLD-FX] static=` census to be identical: the boot/menu levels that precede
the race must not leak casters into the race's cache in either arm.

Positive control: `MDKR_TEST_SHADOW_STAGE_RESET_SKIP=1` suppresses only the
stage reset (restoring the historical defect); that arm must report strictly
more static triangles than the fixed arms, or the equality assertion above
could never fail.

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, fatal_re, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
FRAMES = 3600
STATIC_RE = re.compile(r"\[WORLD-FX\] mode=neutral .*?static=(\d+) ")
FATAL_RE = fatal_re(ignore_case=True)


def run_arm(binary: Path, rom: Path, label: str, root: Path,
            trace: bool, skip_reset: bool, timeout: int,
            verbose: bool) -> int:
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_AUTOPILOT="1",
        MDKR_RENDERER="gl",
        MDKR_LOAD_TRACK="5",
        MDKR_WORLD_FX_TRACE="1",
        MDKR_SAVE_DIR=str(save_dir),
        # Isolate the video config with the save (see check_door_blocks.py).
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    if trace:
        env["MDKR_TRACE"] = "1"
    if skip_reset:
        env["MDKR_TEST_SHADOW_STAGE_RESET_SKIP"] = "1"
    command = [
        str(binary),
        "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT),
        "--rom", str(rom),
        "--window-size", "320x240",
        "--remastered",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n{output[-4000:]}")
    fatal = FATAL_RE.search(output)
    if fatal:
        raise RuntimeError(f"{label}: fatal marker {fatal.group(0)!r}")
    matches = STATIC_RE.findall(output)
    if not matches:
        raise RuntimeError(
            f"{label}: no [WORLD-FX] static census in output\n"
            f"{output[-2000:]}")
    return int(matches[-1])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    with tempfile.TemporaryDirectory(prefix="mdkr-stage-reset-") as tmp:
        root = Path(tmp)
        try:
            shipping = run_arm(binary, rom, "shipping", root,
                               trace=False, skip_reset=False,
                               timeout=args.timeout, verbose=args.verbose)
            traced = run_arm(binary, rom, "traced", root,
                             trace=True, skip_reset=False,
                             timeout=args.timeout, verbose=args.verbose)
            control = run_arm(binary, rom, "reset-skipped", root,
                              trace=False, skip_reset=True,
                              timeout=args.timeout, verbose=args.verbose)
        except RuntimeError as error:
            print(f"check_shadow_stage_reset: FAIL\n  - {error}")
            return 1

    failures: list[str] = []
    if shipping <= 0:
        failures.append(f"shipping arm captured no static casters ({shipping})")
    if shipping != traced:
        failures.append(
            "shipping and traced arms disagree on the terminal static census "
            f"({shipping} vs {traced}) — the stage reset is diagnostic-gated "
            "again")
    if control <= shipping:
        failures.append(
            "positive control failed: with the reset suppressed the census "
            f"must grow ({control} vs {shipping}) or this check cannot detect "
            "the defect it exists for")
    if failures:
        print("check_shadow_stage_reset: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(
        "check_shadow_stage_reset: PASS — shipping and traced arms agree "
        f"(static={shipping}); reset-suppressed control grows to {control}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
