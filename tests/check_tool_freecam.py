#!/usr/bin/env python3
"""Detaching the free camera must change the picture and nothing else.

WHAT THIS GATE IS FOR. `platform/app/tool_freecam.cpp` substitutes the
projection record the fixed-tick camera finalizer latched, inside the
presentation scope, and re-attaches by CEASING TO SUBSTITUTE rather than by
saving a pose and putting it back. Those are two separate claims and this
script tests both:

  inert      the v3 `[SIMHASH]` stream over the whole race is byte-identical to
             an un-detached run. Substituting at presentation depth cannot move
             authoritative state, and this is what says so.
  exact      every presented frame from the re-attachment onward is
             BYTE-IDENTICAL to the un-detached run's frame at the same ordinal.
             Not close: identical. A tool that saved the authored record at
             detach and restored it at re-attach would put back a record built
             from a stale generation and a stale lens, and this assertion is
             what refuses it. The correct implementation restores nothing --
             the finalizer relatches the authored record from authored inputs
             on the next tick, so the frame is built from bytes the tool never
             touched.

WHY IT IS NOT VACUOUS. Two of the three frame bands exist only to close that:

  * the frames DURING detachment must DIFFER. If they did not, the tool never
    substituted anything and "identical afterwards" would be a comparison of a
    run against itself;
  * the frames BEFORE detachment must be identical, so the difference is
    attributable to the detachment rather than to the tool being open at all;
  * both `[FREECAM] event=` transitions must actually appear in the detached
    arm and must NOT appear in the attached arm.

GUARD BANDS. The frame the substitution starts on and the frame the dump
captures are one pipeline apart -- render_scene authors the display list, the
task is walked, and the backbuffer is read at the following present. Frames
within `--guard` of an announced transition are therefore classified as neither
band and skipped, rather than being asserted into whichever band an off-by-one
would put them in.

WHY THE ARMS RUN THROUGH THE APP SHELL. Same reason check_dev_tools_purity.py's
per-tool arms do: the free camera is a registered tool, its substitution runs
only while the tool is open, and the in-game tool surface exists only on the
shell's unattended path. `MDKR_APP_AUTOPLAY_DUMP_FRAMES` is the shell's own
frame-capture seam, so the frames compared here are the frames the app presents.
The capture is taken before the overlay is drawn, so the ImGui window itself is
not in the comparison -- what differs is the rendered scene.

Always muted + headless. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import (ABORT_MARKERS, ASSERT_MARKERS, DEFAULT_BUILD_DIR,
                           find_fatal, resolve_binary)

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"

# Matches check_dev_tools_purity's route and length: long enough that the run is
# genuinely driving, so the detached window lands on a moving scene rather than
# on a menu that would look the same through any lens.
TICKS = 3600
TRACK = "5"
HASH_VERSION = "3"

# The detach window, in authoritative ticks. 300 ticks is the sprint's own
# figure and is ~10 seconds of race at 30 Hz.
DETACH_TICK = 2000
REATTACH_TICK = DETACH_TICK + 300
# A wide lens: the substitution has to be large enough that no reasonable
# encoder or filter could round the two images back together.
ZOOM = "2.50"

FREECAM_RE = re.compile(
    r"\[FREECAM\] event=(\S+) tick=(\d+) frame=(\d+)")
CHECKPOINT_RE = re.compile(r"\bcp=(-?\d+)")
RACER_RE = re.compile(r"\[PACE\] frame=(\d+) .*\| racer (.*)$")
FRAME_RE = re.compile(r"frame_(\d+)\.ppm$")


class ArmError(RuntimeError):
    """A run that could not produce evidence, as opposed to failing evidence."""


class Arm:
    def __init__(self, label: str, output: str, dump: Path):
        self.label = label
        self.output = output
        self.dump = dump
        self.state = [line for line in output.splitlines()
                      if line.startswith("[SIMHASH]")]
        self.outcome: list[tuple[str, str]] = []
        for line in output.splitlines():
            racer = RACER_RE.search(line)
            if racer is not None:
                self.outcome.append((racer.group(1), racer.group(2)))
        self.events = [(kind, int(tick), int(frame))
                       for kind, tick, frame in FREECAM_RE.findall(output)]
        self.frames: dict[int, str] = {}
        for entry in sorted(os.listdir(dump)) if dump.is_dir() else []:
            match = FRAME_RE.search(entry)
            if match is None:
                continue
            with open(dump / entry, "rb") as handle:
                self.frames[int(match.group(1))] = hashlib.sha1(
                    handle.read()).hexdigest()

    @property
    def final_checkpoint(self) -> int:
        if not self.outcome:
            return -1
        match = CHECKPOINT_RE.search(self.outcome[-1][1])
        return int(match.group(1)) if match else -1

    def event_frame(self, kind: str) -> int | None:
        for event, _tick, frame in self.events:
            if event == kind:
                return frame
        return None


def clean_env(**overrides: str) -> dict[str, str]:
    """A run's environment, with the developer's own MDKR_* preferences dropped.

    Identical in intent to check_dev_tools_purity's: an inherited MDKR_TOOLS,
    MDKR_FREECAM_* or MDKR_RENDERER would change what the baseline is a
    baseline OF.
    """
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(LC_ALL="C", MDKR_AUDIO="0")
    env.update(overrides)
    return env


def run_arm(binary: Path, rom: Path, root: Path, label: str, detach: bool,
            dump_from: int, dump_every: int, timeout: int,
            verbose: bool) -> Arm:
    run_dir = root / label
    dump = run_dir / "frames"
    (run_dir / "prefs").mkdir(parents=True)
    (run_dir / "save").mkdir(parents=True)
    dump.mkdir(parents=True)
    env = clean_env(
        MDKR_APP_AUTOPLAY="1",
        MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
        MDKR_APP_AUTOPLAY_INPUT_SCRIPT=str(SCRIPT),
        MDKR_APP_AUTOPLAY_DUMP_FRAMES=str(dump),
        MDKR_APP_PREFS_DIR=str(run_dir / "prefs"),
        MDKR_AUTOPILOT="1",
        MDKR_DUMP_FROM=str(dump_from),
        MDKR_DUMP_EVERY=str(dump_every),
        MDKR_LOAD_TRACK=TRACK,
        MDKR_RENDERER="gl",
        MDKR_ROM=str(rom),
        MDKR_SAVE_DIR=str(run_dir / "save"),
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_TOOLS="1",
        # The tool is OPEN in both arms. Only the schedule differs, so a frame
        # that differs cannot be blamed on the window merely existing.
        MDKR_TOOLS_OPEN="freecam",
        MDKR_TRACE="1",
        MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
        MDKR64_HIDDEN="1",
    )
    if detach:
        env["MDKR_FREECAM_DETACH"] = str(DETACH_TICK)
        env["MDKR_FREECAM_REATTACH"] = str(REATTACH_TICK)
        env["MDKR_FREECAM_ZOOM"] = ZOOM
    if verbose:
        print(f"$ ({label}) detach={detach} {binary}", flush=True)
    process = subprocess.run([str(binary)], cwd=run_dir, env=env, text=True,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=timeout,
                             check=False)
    output = process.stdout or ""
    if process.returncode != 0:
        raise ArmError(f"{label}: exit {process.returncode}\n{output[-3000:]}")
    marker = find_fatal(output, *ABORT_MARKERS, *ASSERT_MARKERS)
    if marker:
        raise ArmError(f"{label}: fatal marker {marker}\n{output[-3000:]}")
    return Arm(label, output, dump)


def check_reached_race(arm: Arm) -> list[str]:
    if not arm.state:
        return [f"{arm.label}: no [SIMHASH] rows — the run produced no "
                "authoritative state to compare"]
    if arm.final_checkpoint <= 0:
        return [f"{arm.label}: finished at checkpoint {arm.final_checkpoint} — "
                "the racer is not driving, so the frames being compared are "
                "not a moving scene"]
    return []


def compare_state(detached: Arm, attached: Arm) -> list[str]:
    if detached.state == attached.state:
        return []
    if len(detached.state) != len(attached.state):
        return [f"v3 state stream has {len(detached.state)} rows against the "
                f"attached run's {len(attached.state)} — detaching the free "
                "camera moved authoritative state"]
    index = next(i for i, (a, b) in enumerate(zip(attached.state,
                                                  detached.state)) if a != b)
    return [f"v3 state stream diverged at row {index} — detaching the free "
            "camera moved authoritative state\n"
            f"    attached: {attached.state[index]}\n"
            f"    detached: {detached.state[index]}"]


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--dump-from", type=int, default=DETACH_TICK - 250)
    parser.add_argument("--dump-every", type=int, default=50)
    parser.add_argument(
        "--guard", type=int, default=60,
        help="frames either side of a transition that are classified as "
             "neither band (the render/present pipeline offset)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT):
        if not os.path.exists(path):
            print(f"check_tool_freecam: FAIL — missing {path}", file=sys.stderr)
            return 1

    print(f"check_tool_freecam: {TICKS} ticks per arm, detaching at tick "
          f"{DETACH_TICK} and re-attaching at {REATTACH_TICK} with a "
          f"{ZOOM}x lens")

    failures: list[str] = []
    notes: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-tool-freecam-") as tmp:
        root = Path(tmp)
        try:
            attached = run_arm(binary, rom, root, "attached", False,
                               args.dump_from, args.dump_every, args.timeout,
                               args.verbose)
            detached = run_arm(binary, rom, root, "detached", True,
                               args.dump_from, args.dump_every, args.timeout,
                               args.verbose)
        except (ArmError, subprocess.TimeoutExpired) as error:
            print(f"check_tool_freecam: FAIL\n  - {error}")
            return 1

        failures += check_reached_race(attached)
        failures += check_reached_race(detached)

        # --- the transitions actually happened --------------------------------
        if attached.events:
            failures.append(
                "attached: the free camera announced "
                f"{[event for event, _t, _f in attached.events]} without a "
                "schedule — the arm that is supposed to be the reference "
                "detached, so every comparison below is against a moved "
                "reference")
        detach_frame = detached.event_frame("detach")
        reattach_frame = detached.event_frame("reattach")
        if detach_frame is None:
            failures.append(
                "detached: no [FREECAM] event=detach row — the camera never "
                "detached, so this gate would be comparing a run against an "
                "identical one")
        if reattach_frame is None:
            failures.append(
                "detached: no [FREECAM] event=reattach row — the camera never "
                "re-attached, so the exactness assertion has nothing to test")

        common = sorted(set(attached.frames) & set(detached.frames))
        if not common:
            failures.append(
                "no frames were dumped by both arms — the comparison has no "
                "subject. --dump-from/--dump-every may be outside the run.")

        if failures:
            print("check_tool_freecam: FAIL")
            for failure in failures:
                print(f"  - {failure}")
            return 1

        assert detach_frame is not None and reattach_frame is not None

        # --- classify every dumped frame --------------------------------------
        before: list[int] = []
        during: list[int] = []
        after: list[int] = []
        skipped = 0
        for ordinal in common:
            if ordinal < detach_frame - args.guard:
                before.append(ordinal)
            elif (detach_frame + args.guard <= ordinal <
                  reattach_frame - args.guard):
                during.append(ordinal)
            elif ordinal >= reattach_frame + args.guard:
                after.append(ordinal)
            else:
                skipped += 1

        # Fail closed on an empty band. "All frames in an empty set are
        # identical" is exactly how this gate would pass while testing nothing.
        if len(before) < 1:
            failures.append(
                f"no dumped frame lands before the detachment (frame "
                f"{detach_frame}); the attached-and-open control is missing")
        if len(during) < 2:
            failures.append(
                f"only {len(during)} dumped frames land inside the detached "
                f"window (frames {detach_frame}..{reattach_frame}); the "
                "not-vacuous assertion needs at least two")
        if len(after) < 2:
            failures.append(
                f"only {len(after)} dumped frames land after the "
                f"re-attachment (frame {reattach_frame}); the exactness "
                "assertion needs at least two")

        # --- inert -------------------------------------------------------------
        failures += compare_state(detached, attached)

        # --- identical before, different during, identical after ---------------
        for ordinal in before:
            if attached.frames[ordinal] != detached.frames[ordinal]:
                failures.append(
                    f"frame {ordinal} differs BEFORE the camera detached — the "
                    "tool is changing the picture merely by being open, so the "
                    "difference during detachment proves nothing")
        same_during = [o for o in during
                       if attached.frames[o] == detached.frames[o]]
        if same_during:
            failures.append(
                f"{len(same_during)} of {len(during)} frames inside the "
                f"detached window are IDENTICAL to the attached run "
                f"(first: frame {same_during[0]}) — the substitution did not "
                "reach the image, so the exactness assertion below is vacuous")
        differ_after = [o for o in after
                        if attached.frames[o] != detached.frames[o]]
        if differ_after:
            failures.append(
                f"{len(differ_after)} of {len(after)} frames after "
                f"re-attachment differ from the attached run (first: frame "
                f"{differ_after[0]}) — the authored lens was RESTORED rather "
                "than resumed. Re-attaching must cease substituting, not put "
                "a saved record back: a record put back carries the rounding "
                "and the staleness of whatever produced it.")

        if not failures:
            notes.append(
                f"attached arm reached checkpoint {attached.final_checkpoint} "
                f"over {len(attached.state)} authoritative ticks")
            notes.append(
                f"{len(before)} frames before, {len(during)} during and "
                f"{len(after)} after the detachment were compared; {skipped} "
                f"inside the +/-{args.guard} frame guard bands were skipped")
            notes.append(
                "every frame from the re-attachment onward is byte-identical: "
                "the finalizer's own relatched record is what render read, "
                "and the tool put nothing back")

    if failures:
        print("check_tool_freecam: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("check_tool_freecam: PASS — the v3 [SIMHASH] stream is unchanged by "
          "detaching, the detached frames differ, and every frame after "
          "re-attaching is byte-identical to the un-detached run")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
