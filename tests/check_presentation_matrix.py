#!/usr/bin/env python3
"""Presentation rate must not move the authoritative tick (spec 12.2.2).

Phase 3 Wave B decouples "how often the host presents an image" from "how often
the authoritative simulation steps". This gate is the machine-checkable half of
that claim, built up one arm per landed slice.

Arms
----

* **A — clock agreement (slice 0).** The parallel ``SimSched`` accumulator wired
  into the video-queue retrace branch must issue exactly as many authoritative
  ticks as the containment loop performed presents, over the full route. It is
  fed the pacer's own committed field count, so a non-zero delta is a real
  modelling disagreement, not clock skew. This is the precondition for ever
  letting the accumulator gate anything: prove the new opinion matches the old
  pacer *before* it is allowed to diverge.

* **B — rate matrix (slice 2).** The per-tick ``[SIMHASH]`` stream must be
  byte-identical with ``MDKR_PRESENT_RATE`` unset, ``=30``, and ``=60`` over the
  same route. ``=60`` presents twice per tick, so it is given twice the headless
  frame budget to reach the same 3600 TICKS — the gate compares simulation, and
  the simulation is counted in ticks, not images.

* **C — smoothness witness (slice 2).** At ``=60`` the presents alternate
  tick, interpolated, tick, interpolated. Every interpolated frame must differ
  from BOTH of its neighbours: same display list, moved camera. The positive
  control is ``MDKR_PRESENT_SMOOTHING=off`` (spec §11's
  ``Video.MotionSmoothing=off``), which presents at the same rate but repeats
  the tick's image — there, every intermediate frame must be byte-IDENTICAL to
  its neighbour. Without that control, arm C could not tell interpolation from
  any other source of frame-to-frame difference.

What arm C's pixels are, and are not
------------------------------------

Arm C compares dumped PPM frames, and those come from ``platform_dump_frame``
(platform_sdl_min.c), which reads the BACKEND's captured frame —
``gfx_read_framebuffer_rgb`` / the GL readback of the render target — *before*
the swap. They are not a capture of the swapchain, and nothing in this suite
observes the swapchain at all.

That is a real blind spot, recorded rather than papered over: the ship review
found that a present with nothing newly drawn used to call ``platform_frame_sync``
and therefore ``SDL_GL_SwapWindow`` on a back buffer whose contents are undefined
after the previous swap. On screen that is flicker at
``FrameLimit=60``/``MotionSmoothing=off``; in this gate it was invisible, because
the backend's captured frame still held the tick's image and arm C's positive
control read exactly that and passed. The engine now skips the swap on the
no-draw paths (``platform_frame_sync_no_swap``), which is what makes the "repeat
the tick's own image" behaviour real; arm C keeps working because the dump is
unaffected. Proving what reaches the display needs a capture outside the process
and is not in this gate's scope.

Arm B history: its first run diverged from tick 1345, at the first level
transition on the route. Two real defects, in order. (1) The deterministic
COUNTER (``osGetCount`` in synthetic pacing) was being advanced once per
PRESENT; its monotonic clamp fabricates a tick whenever it is read without the
clock having moved, so the phase of the advance is observable and not just its
total. (2) The authoritative tick index was bumped BEFORE the interpolated
presents, and since every present pumps input, the last pump before a tick
applied the NEXT tick's scripted input — the simulation consumed every input one
tick early.

Always muted + headless. Exit 0 = pass.
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

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
TICKS = 3600

SUMMARY_RE = re.compile(r"\[PRESENTSCHED-SUMMARY\] (.*)")
# Presents per tick at MDKR_PRESENT_RATE=60 under the 60 Hz field clock: the
# tick floor is two source fields and a present is one.
RATE60_PRESENTS_PER_TICK = 2
SMOOTH_TICKS = 400          # arm C runs a short in-race prefix
SMOOTH_DUMP_FROM = 780      # present index, i.e. tick 390


def parse_summary(text: str) -> dict[str, int]:
    match = None
    for match in SUMMARY_RE.finditer(text):
        pass
    if match is None:
        raise RuntimeError("no [PRESENTSCHED-SUMMARY] row was emitted")
    fields: dict[str, int] = {}
    for token in match.group(1).split():
        key, _, value = token.partition("=")
        fields[key] = int(value)
    return fields


def run(binary: Path, rom: Path, label: str, root: Path,
        extra_env: dict[str, str], frames: int, timeout: int,
        verbose: bool, dump_from: int | None = None) -> str:
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_STATE_HASH="1",
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK="5",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
    )
    env.update(extra_env)
    command = [
        str(binary), "--headless-frames", str(frames),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", "320x240",
    ]
    if dump_from is not None:
        frame_dir = run_dir / "frames"
        frame_dir.mkdir(parents=True)
        env["MDKR_DUMP_FROM"] = str(dump_from)
        command += ["--dump-frames", str(frame_dir)]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(f"{label}: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    return output


def sim_hash_rows(output: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith("[SIMHASH]")]


def first_difference(left: list[str], right: list[str]) -> int | None:
    for index, (a, b) in enumerate(zip(left, right)):
        if a != b:
            return index
    return None if len(left) == len(right) else min(len(left), len(right))


def intermediate_frame_verdict(frame_dir: Path) -> tuple[int, int]:
    """(intermediates differing from both neighbours, intermediates not)."""
    frames: dict[int, bytes] = {}
    for path in sorted(frame_dir.glob("*.ppm")):
        frames[int(path.stem.split("_")[1])] = path.read_bytes()
    moved = still = 0
    for index in sorted(frames):
        # Odd present indices are the interpolated ones: each branch entry puts
        # out the tick's own image first, then the interpolated frames.
        if index % 2 != 1 or index - 1 not in frames or index + 1 not in frames:
            continue
        if (frames[index] != frames[index - 1] and
                frames[index] != frames[index + 1]):
            moved += 1
        else:
            still += 1
    return moved, still


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build-rel")
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
    notes: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-present-matrix-") as tmp:
        root = Path(tmp)
        try:
            baseline = run(binary, rom, "clock-agreement", root,
                           {"MDKR_PRESENT_SCHED_TRACE": "1"},
                           TICKS, args.timeout, args.verbose)
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - {error}")
            return 1

        rows = sim_hash_rows(baseline)
        if len(rows) != TICKS:
            failures.append(
                f"arm A: expected {TICKS} [SIMHASH] rows, got {len(rows)}")
        try:
            summary = parse_summary(baseline)
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm A: {error}")
            return 1

        if summary["ticks"] != summary["presents"]:
            failures.append(
                "arm A: the parallel accumulator and the containment loop "
                f"disagree — sched ticks={summary['ticks']} vs "
                f"presents={summary['presents']}. The accumulator is fed the "
                "pacer's own committed field count, so this is a real "
                "modelling difference (unit conversion, tick size, or field "
                "clock), not clock skew")
        if summary["simticks"] != summary["presents"]:
            failures.append(
                f"arm A: g_simTickCounter={summary['simticks']} != "
                f"presents={summary['presents']}; the tick index has drifted "
                "off the present that carried it")
        if summary["ticks"] != TICKS:
            failures.append(
                f"arm A: sched issued {summary['ticks']} ticks over {TICKS} "
                "presents")
        for key in ("lead", "lag", "zerodue", "multidue", "rebases",
                    "catchup", "skips"):
            if summary[key] != 0:
                failures.append(
                    f"arm A: {key}={summary[key]} — the accumulator diverged "
                    "from 1:1 at least once during the route, which slice 0 "
                    "forbids under the floor-paced synthetic clock")
        notes.append(
            f"arm A: {summary['ticks']} sched ticks == {summary['presents']} "
            f"presents == {len(rows)} [SIMHASH] rows, fieldHz="
            f"{summary['fieldhz']}, zero lead/lag over the whole route")

        # ---- arm B: the rate matrix -----------------------------------
        try:
            rate30 = run(binary, rom, "rate-30", root,
                         {"MDKR_PRESENT_RATE": "30"}, TICKS,
                         args.timeout, args.verbose)
            rate60 = run(binary, rom, "rate-60", root,
                         {"MDKR_PRESENT_RATE": "60",
                          "MDKR_PRESENT_SCHED_TRACE": "1"},
                         TICKS * RATE60_PRESENTS_PER_TICK,
                         args.timeout, args.verbose)
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm B: {error}")
            return 1

        unset_rows = rows
        for label, other in (("=30", sim_hash_rows(rate30)),
                             ("=60", sim_hash_rows(rate60))):
            if len(other) != TICKS:
                failures.append(
                    f"arm B: MDKR_PRESENT_RATE{label} produced {len(other)} "
                    f"[SIMHASH] rows, expected {TICKS}. The presentation rate "
                    "changed how many authoritative ticks ran, which is the "
                    "one thing it must never do")
                continue
            index = first_difference(unset_rows, other)
            if index is not None:
                failures.append(
                    f"arm B: MDKR_PRESENT_RATE{label} diverges from the unset "
                    f"stream at tick {index} ({unset_rows[index]} vs "
                    f"{other[index]}) — presentation rate reached authoritative "
                    "state")
        try:
            rate60_summary = parse_summary(rate60)
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm B: {error}")
            return 1
        if rate60_summary["presents"] != TICKS * RATE60_PRESENTS_PER_TICK:
            failures.append(
                f"arm B: =60 presented {rate60_summary['presents']} frames for "
                f"{TICKS} ticks; expected {TICKS * RATE60_PRESENTS_PER_TICK}, "
                "so the subloop is not actually running at the requested rate")
        if rate60_summary["interp"] == 0:
            failures.append(
                "arm B: =60 issued no interpolated presents at all — the "
                "subloop engaged but never drew an intermediate frame")
        if rate60_summary["simticks"] != TICKS:
            failures.append(
                f"arm B: =60 advanced {rate60_summary['simticks']} "
                f"authoritative ticks, expected {TICKS}")
        notes.append(
            f"arm B: [SIMHASH] byte-identical over {TICKS} ticks for "
            f"MDKR_PRESENT_RATE unset, =30 and =60; =60 issued "
            f"{rate60_summary['presents']} presents "
            f"({rate60_summary['interp']} interpolated, "
            f"{rate60_summary['stale']} skipped on a stale display list)")

        # ---- arm C: the smoothness witness ----------------------------
        try:
            run(binary, rom, "smooth-on", root,
                {"MDKR_PRESENT_RATE": "60"},
                SMOOTH_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=SMOOTH_DUMP_FROM)
            run(binary, rom, "smooth-off", root,
                {"MDKR_PRESENT_RATE": "60", "MDKR_PRESENT_SMOOTHING": "off"},
                SMOOTH_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=SMOOTH_DUMP_FROM)
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm C: {error}")
            return 1

        moved_on, still_on = intermediate_frame_verdict(
            root / "smooth-on" / "frames")
        moved_off, still_off = intermediate_frame_verdict(
            root / "smooth-off" / "frames")
        if moved_on == 0:
            failures.append(
                "arm C: no interpolated frame differed from both of its "
                "neighbours — the intermediate presents are not showing a "
                "moved camera, so the smoothing is doing nothing visible")
        if still_on != 0:
            failures.append(
                f"arm C: {still_on} interpolated frames matched a neighbour "
                "byte-for-byte while smoothing was ON")
        if moved_off != 0:
            failures.append(
                f"arm C: positive control failed — {moved_off} intermediate "
                "frames still differ from their neighbours with "
                "MDKR_PRESENT_SMOOTHING=off, so arm C cannot tell "
                "interpolation from some other frame-to-frame difference")
        if still_off == 0:
            failures.append(
                "arm C: positive control produced no comparable intermediate "
                "frames at all")
        notes.append(
            f"arm C: with smoothing on, {moved_on}/{moved_on + still_on} "
            f"interpolated frames differ from both neighbours; with "
            f"MDKR_PRESENT_SMOOTHING=off, {moved_off}/{moved_off + still_off} "
            "do (every intermediate frame is a byte-identical repeat)")

    if failures:
        print("check_presentation_matrix: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_presentation_matrix: PASS")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
