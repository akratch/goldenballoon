#!/usr/bin/env python3
"""Presentation-mode gate for the videoconf wave.

ROM-backed, so like check_widescreen_shadow.py it is not registered with CTest.
It complements the millisecond ROM-free unit test (tests/test_video_config.c)
with real-game runs asserting four things:

  * --pure, --restored and --remastered emit IDENTICAL normalized [PACE] state
    streams. For a title whose renderer feeds simulation this is a far stronger
    statement than any pixel comparison: it proves the presentation modes differ
    only in presentation. If a mode ever moves race state, it is not a
    presentation mode and the wave that introduced it has a bug.

  * Pure reproduces the authored 4:3 framing. On a 16:9 drawable it must
    pillarbox -- an undistorted, centered presentation narrower than the
    drawable -- while Restored fills the window. This is the assertion that
    stops someone "simplifying" Pure to Video.Widescreen=0, which does NOT give
    a 4:3 image: it engages the pre-widescreen compatibility path where 4:3
    content is STRETCHED to fill the window.

  * The precedence ladder resolves as specified:
    defaults < mdkr64.ini < preset < MDKR_* env < --video-set.

  * POSITIVE CONTROL. The [PACE] comparator must be able to fail. A run with
    MDKR_RNGSEED=legacy perturbs the boot seeds and therefore the simulation, so
    its stream MUST differ from the baseline. Without this arm a green result
    would be indistinguishable from a comparator that always says "identical" --
    for instance if the rows stopped being emitted at all.

Usage:
    python3 tests/check_video_presets.py --build build --rom baserom.us.v80.z64 -v
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness_utils import resolve_binary  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
FRAMES = 3400
TIMEOUT = 300

MODES = ("--pure", "--restored", "--remastered")

DISPLAY_RE = re.compile(
    r"^\[DISPLAY\] drawable=(\d+)x(\d+) presentation=(\d+)x(\d+)\+(-?\d+)\+(-?\d+).*?aspect=([\d.]+)",
    re.MULTILINE,
)


def clean_environment(extra: dict[str, str] | None = None) -> dict[str, str]:
    """A run environment with every MDKR_* inherited from the caller stripped.

    Without this a developer's exported MDKR_RENDER_SCALE would silently become
    an env-precedence override in every arm and the gate would test nothing.
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("MDKR_")}
    env.update(
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_TRACE="1",
        MDKR_NO_CRASH_HANDLER="1",
    )
    if extra:
        env.update(extra)
    return env


def normalized_pace(output: str) -> list[str]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker < 0:
            continue
        row = line[marker:]
        # Wall-clock measurement is intentionally non-deterministic; every
        # simulation field to either side must remain byte-identical.
        row = re.sub(r" dtms=\S+", " dtms=<wall>", row)
        rows.append(row)
    return rows


def run(binary: Path, rom: Path, mode: str, size: str,
        extra_env: dict[str, str] | None = None,
        extra_args: list[str] | None = None,
        verbose: bool = False) -> tuple[int, str]:
    command = [
        str(binary),
        "--rom", str(rom),
        "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT),
        "--window-size", size,
        mode,
    ]
    if extra_args:
        command += extra_args
    if verbose:
        print(f"$ ({mode} {size}) " + shlex.join(command), flush=True)
    # A fresh cwd gives each arm an independent save/ directory AND guarantees
    # no stray mdkr64.ini from the repo root leaks in as a file-precedence layer.
    with tempfile.TemporaryDirectory(prefix="mdkr_video_") as run_dir:
        try:
            proc = subprocess.run(
                command, cwd=run_dir, env=clean_environment(extra_env),
                text=True, capture_output=True, timeout=TIMEOUT, check=False,
            )
        except subprocess.TimeoutExpired as exc:
            return 124, (exc.stdout or "") + (exc.stderr or "")
    return proc.returncode, proc.stdout + proc.stderr


def first_diff(a: list[str], b: list[str]) -> str:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return f"row {i}\n    baseline: {x}\n    other:    {y}"
    return f"row count {len(a)} vs {len(b)}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    # Shared contract: --build accepts either a build directory or the
    # executable itself. run_checks.py passes the executable.
    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom)
    if not rom.is_absolute():
        rom = (REPO / rom).resolve()
    if not binary.exists():
        print(f"FAIL: binary not found: {binary}")
        return 1
    if not rom.exists():
        print(f"FAIL: ROM not found: {rom}")
        return 1

    failures: list[str] = []

    # ---- 1. Simulation invariance across the three modes -------------------
    streams: dict[str, list[str]] = {}
    for mode in MODES:
        rc, output = run(binary, rom, mode, "1280x720", verbose=args.verbose)
        if rc != 0:
            failures.append(f"{mode}: exit {rc}\n{output[-3000:]}")
            continue
        rows = normalized_pace(output)
        if not rows:
            failures.append(f"{mode}: no [PACE] rows -- the route never drove a racer")
            continue
        streams[mode] = rows
        if args.verbose:
            print(f"  {mode}: {len(rows)} [PACE] rows")

    baseline = streams.get("--pure")
    if baseline:
        for mode in ("--restored", "--remastered"):
            other = streams.get(mode)
            if other is None:
                continue
            if other != baseline:
                failures.append(
                    f"{mode}: [PACE] diverges from --pure at {first_diff(baseline, other)}"
                )

    # ---- 2. Pure reproduces the authored 4:3 framing ------------------------
    for mode, expect_pillarbox in (("--pure", True), ("--restored", False)):
        rc, output = run(binary, rom, mode, "1280x720", verbose=args.verbose)
        matches = DISPLAY_RE.findall(output)
        if not matches:
            failures.append(f"{mode}: no [DISPLAY] layout line")
            continue
        dw, dh, pw, ph, px, _py, aspect = matches[-1]
        dw, pw, px, aspect = int(dw), int(pw), int(px), float(aspect)
        pillarboxed = pw < dw and px > 0
        if expect_pillarbox:
            if not pillarboxed:
                failures.append(
                    f"--pure did not pillarbox on a 16:9 drawable: "
                    f"drawable={dw}x{dh} presentation={pw}x{ph}+{px}. "
                    f"Pure must pin Video.Aspect=4:3 (NOT Video.Widescreen=0, "
                    f"which stretches)."
                )
            if abs(aspect - 4.0 / 3.0) > 0.001:
                failures.append(f"--pure presentation aspect {aspect} is not 4:3")
        else:
            if pillarboxed:
                failures.append(
                    f"{mode} pillarboxed but should fill the window: "
                    f"drawable={dw}x{dh} presentation={pw}x{ph}+{px}"
                )
        if args.verbose:
            print(f"  {mode}: drawable={dw}x{dh} presentation={pw}x{ph}+{px} aspect={aspect}")

    # ---- 3. Precedence ladder (cheap: --video-list exits before ROM load) ---
    ladder = [
        # No mode flag at all: the schema default, unattributed to any layer.
        ({}, [], "Video.RenderScale", "2", "default"),
        # Passing the mode explicitly IS a preset application, even when it
        # resolves to the same value the default already held.
        ({}, ["--remastered"], "Video.RenderScale", "2", "preset"),
        ({}, ["--pure"], "Video.RenderScale", "1", "preset"),
        ({"MDKR_RENDER_SCALE": "3"}, ["--pure"], "Video.RenderScale", "3", "env"),
        ({"MDKR_RENDER_SCALE": "3"},
         ["--pure", "--video-set", "Video.RenderScale=4"],
         "Video.RenderScale", "4", "CLI"),
        ({}, ["--pure"], "Video.Aspect", "4:3", "preset"),
    ]
    for extra_env, argv, key, want_value, want_source in ladder:
        command = [str(binary), "--video-list"] + argv
        proc = subprocess.run(command, env=clean_environment(extra_env),
                              text=True, capture_output=True, check=False)
        line = next((l for l in proc.stdout.splitlines() if key in l), None)
        if line is None:
            failures.append(f"--video-list {' '.join(argv)}: no {key} row")
            continue
        fields = line.split()
        if len(fields) < 3 or fields[1] != want_value or fields[2] != f"[{want_source}]":
            failures.append(
                f"--video-list {' '.join(argv)} (env={extra_env}): expected "
                f"{key}={want_value} [{want_source}], got: {line.strip()}"
            )
        elif args.verbose:
            print(f"  ladder ok: {' '.join(argv)} env={extra_env} -> {line.strip()}")

    # ---- 3b. Supersampling ---------------------------------------------------
    # Restored and Remastered ship at 2x, but this direct 1x-vs-4x check proves
    # the full supported path rather than relying on the preset default. Two
    # properties matter and they pull in opposite directions:
    # the scaled frame must actually DIFFER (or the setting is inert, which it
    # silently was on both backends until the resolve landed), and the readback
    # must stay OUTPUT-sized (or every pixel gate in the project starts scoring
    # supersampled images without saying so).
    ss = {}
    for scale in ("1", "4"):
        with tempfile.TemporaryDirectory(prefix="mdkr_ss_") as run_dir:
            frames = Path(run_dir) / "frames"
            frames.mkdir()
            command = [
                str(binary), "--rom", str(rom),
                "--headless-frames", "1200",
                "--input-script", str(SCRIPT),
                "--window-size", "640x480",
                "--dump-frames", str(frames),
                "--restored", "--video-set", f"Video.RenderScale={scale}",
            ]
            env = clean_environment({"MDKR_DUMP_FROM": "1100", "MDKR_DUMP_EVERY": "999"})
            proc = subprocess.run(command, cwd=run_dir, env=env, text=True,
                                  capture_output=True, timeout=TIMEOUT, check=False)
            dumps = sorted(frames.glob("*.ppm"))
            if proc.returncode != 0 or not dumps:
                failures.append(f"supersampling x{scale}: exit {proc.returncode}, "
                                f"{len(dumps)} frames")
                continue
            ss[scale] = dumps[0].read_bytes()

    if len(ss) == 2:
        def ppm_dims(blob):
            parts = blob.split(maxsplit=4)
            return int(parts[1]), int(parts[2])
        d1, d4 = ppm_dims(ss["1"]), ppm_dims(ss["4"])
        if d1 != d4:
            failures.append(
                f"readback size changed with supersampling: {d1} at 1x vs {d4} at 4x. "
                f"Every pixel gate would silently start scoring scaled frames."
            )
        if ss["1"] == ss["4"]:
            failures.append(
                "Video.RenderScale=4 produced a byte-identical frame to 1x -- "
                "supersampling is inert (it silently was, on both backends, "
                "before the resolve landed)."
            )
        elif args.verbose:
            print(f"  supersampling ok: {d1} at both scales, images differ")

    # ---- 4. POSITIVE CONTROL: the comparator must be able to fail -----------
    if baseline:
        rc, output = run(binary, rom, "--pure", "1280x720",
                         extra_env={"MDKR_RNGSEED": "legacy"}, verbose=args.verbose)
        control = normalized_pace(output)
        if rc != 0:
            failures.append(f"positive control: exit {rc}")
        elif not control:
            failures.append("positive control: no [PACE] rows")
        elif control == baseline:
            failures.append(
                "POSITIVE CONTROL FAILED: MDKR_RNGSEED=legacy produced an "
                "identical [PACE] stream. The comparator cannot detect "
                "simulation divergence, so the passes above prove nothing."
            )
        elif args.verbose:
            print(f"  positive control ok: diverges at {first_diff(baseline, control)}")

    if failures:
        print("FAIL video presets:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(
        f"PASS video presets: {len(baseline or [])} [PACE] rows identical across "
        f"pure/restored/remastered; Pure pillarboxes 4:3; precedence ladder "
        f"resolves; positive control diverges"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
