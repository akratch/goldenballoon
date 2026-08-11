#!/usr/bin/env python3
"""Hold the wave surfaces to the midpoint-sensitivity contract.

Task 7 gave the wave surfaces a presentation owner. ``check_smooth_verdict.py``
is the structural witness for that (WATER_WAVE blends, files no NO_OWNER, and
its topology guard both runs and refuses). This is the pixel witness, and it
asserts the one property no counter can: the **midpoint-sensitivity contract**
(``docs/UNCAPPED_PRESENTATION.md:198-200``) -- an image reconstructed between
two authored ticks must differ from BOTH of them, in the region the
reconstruction is responsible for. Content that is replayed rather than
reconstructed passes every endpoint check ever written, because reproducing an
endpoint is exactly what it does; that is the whole failure mode.

**The region is measured, not cropped.** A hand-picked water rectangle is a
number about the crop as much as about the water, and it goes stale the moment
the route moves. So the route is run twice from identical deterministic inputs:

* production;
* with ``MDKR_TEST_WAVE_TOPOLOGY_FLIP=1``, the negative control in
  ``game/src/waves.c``, which folds the per-tick vertex-buffer flip into the
  wave topology key. Every authored pair then reads as a topology change, the
  choke point refuses every wave blend, and the surface is replayed instead of
  reconstructed.

The pixels that differ between the two runs at a given present ARE the wave
stage's own footprint at that present. Everything else -- camera, racers,
scenery -- is identical by construction, and the gate CHECKS that rather than
assuming it: on authored frames (alpha 0) the two runs must be byte-identical,
because a snap and a blend agree exactly at their shared endpoint. If they are
not, the seam is moving something other than wave interpolation and every
figure below is attributing someone else's pixels to the wave stage.

Three claims, in the order that makes each one meaningful:

1. **endpoint identity** -- every authored frame agrees between the runs;
2. **the stage reaches the screen** -- at least ``MIN_ACTED_TICKS`` midpoints
   differ between the runs at all. This is the non-vacuity floor and the
   mutation catch in one: a build whose topology key is constant refuses
   nothing, the control becomes byte-identical to production, and this count
   collapses to zero;
3. **midpoint sensitivity** -- at every one of those midpoints, production's
   image differs from BOTH bracketing authored images over that midpoint's own
   footprint. Measured 95 of 95 on this window.

Always muted + headless. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"

# WHALE BAY (level 8), not Jungle Falls. check_smooth_verdict.py's fixture is
# chosen for carrying a scroller and a wave surface in one view; this gate
# needs the wave surface to be most of what the camera is looking at. Measured
# 777 topology-key comparisons over Whale Bay against 282 over Jungle Falls --
# and, decisively, an entire Jungle Falls race produced ZERO presents at which
# wave interpolation changed a pixel (3200 frame pairs, two renderers). A gate
# cannot be built on a route where the stage under test never reaches the
# screen; it would go green on a build that had deleted the stage.
ROUTE_TRACK = "8"
# 120 Hz over a 30 Hz tick: four presents per authored tick, so index%4==0 is
# the real walk's own image and index%4==2 is the midpoint this gate is named
# for. Same rate and dump mechanism as check_effect_shell_envelope.py.
PRESENT_RATE = "120"
PRESENTS_PER_TICK = 4
ROUTE_TICKS = 3230
# Present indices: mid-race, water in frame. 400 presents is ~99 authored
# ticks, of which ~95 were measured to have wave content on screen.
DUMP_FROM = 10480
DUMP_FRAMES = 400
# 160x120 requested, i.e. a 320x240 backend dump. This gate compares whole
# frames rather than segmenting an object out of one, so it needs no
# resolution beyond enough water pixels to be sure a difference is the
# surface and not one stray edge -- and two 400-frame dumps have to fit in a
# temp dir.
WINDOW_SIZE = "160x120"

# See claim 2 in the module docstring. Measured 95 of 99 candidate midpoints
# on this window; 20 keeps four-fifths of that as margin against host
# scheduling changing which presents get an interpolated walk.
MIN_ACTED_TICKS = 20


def read_ppm(path: Path) -> bytes:
    """The pixel payload of a binary P6 dump."""

    data = path.read_bytes()
    fields: list[bytes] = []
    offset = 0
    while len(fields) < 4:
        while offset < len(data) and data[offset:offset + 1].isspace():
            offset += 1
        if data[offset:offset + 1] == b"#":
            while offset < len(data) and data[offset] != 0x0A:
                offset += 1
            continue
        start = offset
        while offset < len(data) and not data[offset:offset + 1].isspace():
            offset += 1
        fields.append(data[start:offset])
    if fields[0] != b"P6" or fields[3] != b"255":
        raise RuntimeError(f"{path}: not an 8-bit binary PPM")
    return bytes(data[offset + 1:])


def run(binary: Path, rom: Path, root: Path, label: str, timeout: int,
        verbose: bool, topology_flip: bool) -> dict[int, bytes]:
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    frame_dir = run_dir / "frames"
    frame_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=ROUTE_TRACK,
        # WebGPU is what players run; a reconstruction claim proven only on
        # the diagnostic backend proves nothing about the shipped picture.
        MDKR_RENDERER="webgpu",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
        MDKR_PRESENT_RATE=PRESENT_RATE,
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        MDKR_DUMP_FROM=str(DUMP_FROM),
    )
    if topology_flip:
        env["MDKR_TEST_WAVE_TOPOLOGY_FLIP"] = "1"
    command = [
        str(binary), "--headless-ticks", str(ROUTE_TICKS),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", WINDOW_SIZE, "--dump-frames", str(frame_dir),
    ]
    if verbose:
        print(f"$ [{label}] {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False)
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(f"{label}: exit {process.returncode}\n"
                           f"{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")

    frames: dict[int, bytes] = {}
    for path in frame_dir.glob("*.ppm"):
        index = int(path.stem.split("_")[1])
        if DUMP_FROM <= index < DUMP_FROM + DUMP_FRAMES:
            frames[index] = read_ppm(path)
    if len(frames) != DUMP_FRAMES:
        raise RuntimeError(
            f"{label}: {len(frames)} frames in the measurement window, "
            f"expected {DUMP_FRAMES}")
    return frames


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []
    rows: list[str] = []

    with tempfile.TemporaryDirectory(prefix="mdkr-wave-midpoint-") as tmp:
        try:
            prod = run(binary, rom, Path(tmp), "prod", args.timeout,
                       args.verbose, False)
            control = run(binary, rom, Path(tmp), "control", args.timeout,
                          args.verbose, True)
        except Exception as error:  # noqa: BLE001 - reported, not swallowed
            print(f"FAIL: {error}", file=sys.stderr)
            return 1

        indices = sorted(set(prod) & set(control))
        authored = [i for i in indices if i % PRESENTS_PER_TICK == 0]

        # Claim 1. Before anything is attributed to the wave stage, prove the
        # seam touches nothing else: alpha 0 is a byte reproduction of the
        # authored endpoint in both runs, so they must agree there exactly.
        disagreeing = [i for i in authored if prod[i] != control[i]]
        rows.append(f"[WAVE-ENDPOINTS] authored={len(authored)} "
                    f"disagreeing={len(disagreeing)}")
        if disagreeing:
            print(rows[0])
            print(
                f"FAIL: {len(disagreeing)} of {len(authored)} AUTHORED frames "
                f"differ between production and control (first "
                f"{disagreeing[0]}). MDKR_TEST_WAVE_TOPOLOGY_FLIP may only "
                "change reconstructed images; that it changed an authored one "
                "means either the endpoint contract is broken or the seam "
                "reaches further than the wave topology key. Every figure "
                "this gate would report below is measuring the wrong thing",
                file=sys.stderr)
            return 1

        # Claims 2 and 3, per midpoint, over that midpoint's OWN footprint.
        # A window-wide footprint would drag pixels that only some other tick's
        # water covered into every tick's verdict.
        acted = 0
        locked: list[int] = []
        for opener in authored:
            midpoint = opener + PRESENTS_PER_TICK // 2
            closer = opener + PRESENTS_PER_TICK
            if midpoint not in prod or closer not in prod:
                continue
            if prod[midpoint] == control[midpoint]:
                # The wave stage changed nothing here: no interpolated walk
                # landed on this present, or no wave surface was in frame.
                # There is no reconstruction to hold to a contract.
                continue
            acted += 1
            mask = (int.from_bytes(prod[midpoint], "big") ^
                    int.from_bytes(control[midpoint], "big"))
            mid = int.from_bytes(prod[midpoint], "big") & mask
            low = int.from_bytes(prod[opener], "big") & mask
            high = int.from_bytes(prod[closer], "big") & mask
            if mid == low or mid == high:
                locked.append(opener)

    rows.append(f"[WAVE-MIDPOINT] acted={acted} locked={len(locked)} "
                f"floor={MIN_ACTED_TICKS}")

    if acted < MIN_ACTED_TICKS:
        failures.append(
            f"only {acted} midpoints (floor {MIN_ACTED_TICKS}) differ between "
            "production and the topology-flip control. The control refuses "
            "every wave pair, so any wave surface being reconstructed on "
            "screen must show up here. Zero or near-zero means the wave stage "
            "is not reaching the picture at all -- which is what a constant "
            "topology key, a lost owner, or a deleted guard all look like -- "
            "and claim 3 below would then be quantified over nothing")
    if locked:
        failures.append(
            f"{len(locked)} of {acted} reconstructed midpoints reproduce a "
            f"bracketing authored image EXACTLY over their own footprint "
            f"(first at authored present {locked[0]}). A reconstruction that "
            "equals an endpoint is not a reconstruction: the surface is being "
            "replayed at one tick's bytes while the frame around it moves. "
            "See docs/UNCAPPED_PRESENTATION.md:198-200")

    for row in rows:
        print(row)
    if failures:
        print("", file=sys.stderr)
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: every reconstructed wave midpoint differs from both of its "
          "authored endpoints")
    return 0


if __name__ == "__main__":
    sys.exit(main())
