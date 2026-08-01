#!/usr/bin/env python3
"""Presentation rate must not move the authoritative tick (spec 12.2.2).

Phase 3 Wave B decouples "how often the host presents an image" from "how often
the authoritative simulation steps". This gate is the machine-checkable half of
that claim, built up one arm per landed slice.

Arms
----

* **A — fixed-ticket authority.** The promoted ``HostFrameDriver`` clock must
  issue exactly one two-field ticket per game pass on the floor-paced original
  schedule. Clock ticks, issued tickets, completed simulation ticks, state rows,
  and event rows must agree with zero debt or update-rate violations.

* **B — rate matrix (slice 2).** The per-tick ``[SIMHASH]`` state stream,
  ordered ``[EVENTHASH]`` gameplay-event stream, consumed input, and temporary
  PCM capture must all be byte-identical
  with ``MDKR_PRESENT_RATE`` unset, ``=30``, and ``=60`` over the same route.
  ``=60`` presents twice per tick, so it is given twice the headless frame
  budget to reach the same 3600 TICKS — authority is counted in ticks, not
  images.

  The same arm also requires generation-keyed object ownership to be live on
  the replay path: root and composed-child registrations must both be observed,
  their census must reconcile exactly, and at least one owned matrix must be
  rebuilt at an intermediate alpha. This prevents a camera-only replay from
  satisfying the rate/determinism half of the gate while every object remains
  parked on its tick pose.

* **C — smoothness witness (slice 2).** At ``=60`` the presents alternate
  retained tick endpoint, interpolated midpoint, endpoint, midpoint. Every
  midpoint must differ from BOTH of its neighbours: same retained display
  list, moved camera. The positive
  control is ``MDKR_PRESENT_SMOOTHING=off`` (spec §11's
  ``Video.MotionSmoothing=off``), which presents at the same rate but repeats
  the tick's image — there, every intermediate frame must be byte-IDENTICAL to
  its neighbour. Without that control, arm C could not tell interpolation from
  any other source of frame-to-frame difference.

  A second positive control, ``MDKR_TEST_OBJECT_INTERPOLATION=off``, keeps the
  same interpolated camera but disables generation-keyed matrix rebuilding.
  At least one intermediate frame must then differ from the fully smoothed
  arm, proving the new object path changes backend pixels rather than merely
  incrementing telemetry.

  A third control, ``MDKR_TEST_DEFORMATION_INTERPOLATION=off``, leaves camera,
  root, child, and billboard interpolation intact while holding retained model
  vertices at the tick pose. A pixel difference proves animated deformation is
  reaching the backend independently of the already-proven root motion.

  A fourth control, ``MDKR_TEST_PARTICLE_INTERPOLATION=off``, runs the battle
  challenge whose continuous point trails actually change their world-space
  mesh from tick to tick. The ordinary time-trial route mostly exercises line
  particles: their authored chevrons are built, then intentionally remain
  stationary, so a non-zero lookup count there cannot prove visible particle
  motion. The battle arm requires changed retained XYZ, identical authoritative
  hashes, and a backend-pixel difference against the particle-only control.

  A fifth control, ``MDKR_TEST_VERTEX_COLOR_INTERPOLATION=off``, leaves the
  point-trail XYZ replay enabled but holds its retained shade RGBA at the
  authored tick. A pixel difference against the normal particle arm proves
  continuous color/opacity scalars reach the backend independently of motion.

  A sixth control, ``MDKR_TEST_PRIMITIVE_ALPHA_INTERPOLATION=off``, leaves
  geometry and retained vertex RGBA replay enabled while holding draw-local
  primitive alpha at its authored tick. A pixel difference against the normal
  particle arm proves sprite/model/line-particle fades reach the backend
  without double-scaling point trails, whose opacity is already in vertex
  alpha.

  A seventh control, ``MDKR_TEST_EFFECT_INTERPOLATION=off``, forces the authored
  level-3 shield path around each racer in both arms. Shield matrices combine a
  shared render object's local rotation/scale/shear recipe with a generation-
  keyed racer root, so this arm requires both identities to remain collision-
  free, the authoritative stream to remain identical, and the semantic matrix
  reconstruction to change backend pixels.

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
import hashlib
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
# Presentation invariance is asserted against the current authority contract.
# Archived v1/v2 streams remain selectable only in check_state_hash.py.
HASH_VERSION = "3"

SUMMARY_RE = re.compile(r"\[PRESENTSCHED-SUMMARY\] (.*)")
AUDIO_SUMMARY_RE = re.compile(r"\[AUDIO-SERVICE\] (.*)")
REPLAY_SUMMARY_RE = re.compile(r"\[REPLAY-SUMMARY\] (.*)")
OWNER_SUMMARY_RE = re.compile(r"\[PRESENT-OWNERS\] (.*)")
PACKET_SUMMARY_RE = re.compile(r"\[PRESENT-PACKET\] (.*)")
# Presents per tick at MDKR_PRESENT_RATE=60 under the 60 Hz field clock: the
# tick floor is two source fields and a present is one.
RATE60_PRESENTS_PER_TICK = 2
SMOOTH_TICKS = 400          # arm C runs a short in-race prefix
SMOOTH_DUMP_FROM = 780      # present index, i.e. tick 390
# The navigation route's countdown clears around authored tick 3120. Racer
# deformation is visible there; the earlier menu witness has compatible but
# screen-static model batches and cannot prove this slice with pixels.
DEFORMATION_TICKS = 3140
DEFORMATION_DUMP_FROM = 6240
# Battle challenge level 26 exercises moving point-trail meshes. Its retargeted
# race loads at tick 2621; the final 50 intermediate frames are a measured
# particle-positive window (10,997 changed batches over the whole arm).
PARTICLE_TICKS = 4200
PARTICLE_DUMP_FROM = 8300
# g_frameCounter counts presents in the =60 arm. Force shields after the race
# loads and sample the final 60 presents of the deterministic window.
EFFECT_TICKS = 4120
EFFECT_DUMP_FROM = 8180
EFFECT_FORCE_WINDOW = "6240:2000"


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


def parse_audio_summary(text: str) -> dict[str, int]:
    match = None
    for match in AUDIO_SUMMARY_RE.finditer(text):
        pass
    if match is None:
        raise RuntimeError("no [AUDIO-SERVICE] row was emitted")
    fields: dict[str, int] = {}
    for token in match.group(1).split():
        key, _, value = token.partition("=")
        fields[key] = int(value)
    return fields


def parse_last_fields(text: str, pattern: re.Pattern[str], name: str) -> dict[str, int]:
    match = None
    for match in pattern.finditer(text):
        pass
    if match is None:
        raise RuntimeError(f"no [{name}] row was emitted")
    fields: dict[str, int] = {}
    for token in match.group(1).split():
        key, separator, value = token.partition("=")
        if separator:
            fields[key] = int(value)
    return fields


def run(binary: Path, rom: Path, label: str, root: Path,
        extra_env: dict[str, str], frames: int, timeout: int,
        verbose: bool, dump_from: int | None = None,
        audio_capture: bool = False) -> str:
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUDIO_SERVICE_TRACE="1",
        MDKR_EVENT_HASH="1",
        MDKR_INPUT_HASH="1",
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK="5",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
    )
    env.update(extra_env)
    if audio_capture:
        env["MDKR_AUDIO_DUMP"] = str(run_dir / "schedule-audio.wav")
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


def audio_digest(root: Path, label: str) -> str:
    path = root / label / "schedule-audio.wav"
    if not path.is_file() or path.stat().st_size <= 44:
        raise RuntimeError(f"{label}: audio service produced no PCM capture")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sim_hash_rows(output: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith("[SIMHASH]")]


def event_hash_rows(output: str) -> list[str]:
    return [line for line in output.splitlines()
            if line.startswith("[EVENTHASH]")]


def input_hash_rows(output: str) -> list[str]:
    return [line for line in output.splitlines()
            if line.startswith("[INPUTHASH]")]


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


def compare_frame_dirs(left: Path, right: Path, odd_only: bool = False) -> tuple[int, int]:
    left_frames = {path.name: path.read_bytes() for path in left.glob("*.ppm")}
    right_frames = {path.name: path.read_bytes() for path in right.glob("*.ppm")}
    compared = different = 0
    for name in sorted(left_frames.keys() & right_frames.keys()):
        if odd_only and int(Path(name).stem.split("_")[1]) % 2 != 1:
            continue
        compared += 1
        if left_frames[name] != right_frames[name]:
            different += 1
    return compared, different


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
                           TICKS, args.timeout, args.verbose,
                           audio_capture=True)
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - {error}")
            return 1

        rows = sim_hash_rows(baseline)
        event_rows = event_hash_rows(baseline)
        input_rows = input_hash_rows(baseline)
        if len(rows) != TICKS:
            failures.append(
                f"arm A: expected {TICKS} [SIMHASH] rows, got {len(rows)}")
        if len(event_rows) != TICKS:
            failures.append(
                f"arm A: expected {TICKS} [EVENTHASH] rows, "
                f"got {len(event_rows)}")
        if len(input_rows) != TICKS:
            failures.append(
                f"arm A: expected {TICKS} [INPUTHASH] rows, "
                f"got {len(input_rows)}")
        try:
            summary = parse_summary(baseline)
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm A: {error}")
            return 1

        if summary["ticks"] != summary["presents"]:
            failures.append(
                "arm A: the authoritative clock and fixed-ticket adapter "
                f"disagree — sched ticks={summary['ticks']} vs "
                f"presents={summary['presents']}. The accumulator is fed the "
                "pacer's committed field count, so this is a real "
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
        for key, expected in (("tickfields", 2), ("blocked", 0),
                              ("maxpending", 1), ("updatebad", 0),
                              ("updatemin", 2), ("updatemax", 2)):
            if summary.get(key) != expected:
                failures.append(
                    f"arm A: {key}={summary.get(key)}, expected {expected}")
        for key in ("lead", "lag", "zerodue", "multidue", "rebases",
                    "catchup", "skips"):
            if summary[key] != 0:
                failures.append(
                    f"arm A: {key}={summary[key]} — the accumulator diverged "
                    "from 1:1 at least once during the route, which slice 0 "
                    "forbids under the floor-paced synthetic clock")
        notes.append(
            f"arm A: {summary['ticks']} sched ticks == {summary['presents']} "
            f"presents == {len(rows)} state/event rows, fieldHz="
            f"{summary['fieldhz']}, zero lead/lag over the whole route")

        # ---- arm B: the rate matrix -----------------------------------
        try:
            rate30 = run(binary, rom, "rate-30", root,
                         {"MDKR_PRESENT_RATE": "30"}, TICKS,
                         args.timeout, args.verbose, audio_capture=True)
            rate60 = run(binary, rom, "rate-60", root,
                         {"MDKR_PRESENT_RATE": "60",
                         "MDKR_PRESENT_SCHED_TRACE": "1"},
                         TICKS * RATE60_PRESENTS_PER_TICK,
                         args.timeout, args.verbose, audio_capture=True)
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm B: {error}")
            return 1

        unset_rows = rows
        unset_events = event_rows
        unset_inputs = input_rows
        try:
            unset_audio = parse_audio_summary(baseline)
            unset_pcm = audio_digest(root, "clock-agreement")
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm B: {error}")
            return 1
        for label, other_output in (("=30", rate30), ("=60", rate60)):
            other = sim_hash_rows(other_output)
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
            other_events = event_hash_rows(other_output)
            if len(other_events) != TICKS:
                failures.append(
                    f"arm B: MDKR_PRESENT_RATE{label} produced "
                    f"{len(other_events)} [EVENTHASH] rows, expected {TICKS}")
            else:
                event_index = first_difference(unset_events, other_events)
                if event_index is not None:
                    failures.append(
                        f"arm B: MDKR_PRESENT_RATE{label} event stream "
                        f"diverges at tick {event_index} — presentation rate "
                        "changed an ordered gameplay event")
            other_inputs = input_hash_rows(other_output)
            if len(other_inputs) != TICKS:
                failures.append(
                    f"arm B: MDKR_PRESENT_RATE{label} produced "
                    f"{len(other_inputs)} [INPUTHASH] rows, expected {TICKS}")
            else:
                input_index = first_difference(unset_inputs, other_inputs)
                if input_index is not None:
                    failures.append(
                        f"arm B: MDKR_PRESENT_RATE{label} consumed-input "
                        f"stream diverges at tick {input_index}")
            try:
                other_audio = parse_audio_summary(other_output)
                other_pcm = audio_digest(
                    root, "rate-30" if label == "=30" else "rate-60")
            except RuntimeError as error:
                failures.append(f"arm B: MDKR_PRESENT_RATE{label}: {error}")
                continue
            for key in ("fields", "due", "serviced", "pending", "samples",
                        "retired", "notready", "dropped", "quantumfields"):
                if other_audio.get(key) != unset_audio.get(key):
                    failures.append(
                        f"arm B: MDKR_PRESENT_RATE{label} audio {key}="
                        f"{other_audio.get(key)} differs from unset "
                        f"{unset_audio.get(key)}")
            if other_pcm != unset_pcm:
                failures.append(
                    f"arm B: MDKR_PRESENT_RATE{label} synthesized PCM differs "
                    "from the unset schedule")
        try:
            rate60_summary = parse_summary(rate60)
            replay_summary = parse_last_fields(
                rate60, REPLAY_SUMMARY_RE, "REPLAY-SUMMARY")
            owner_summary = parse_last_fields(
                rate60, OWNER_SUMMARY_RE, "PRESENT-OWNERS")
            packet_summary = parse_last_fields(
                rate60, PACKET_SUMMARY_RE, "PRESENT-PACKET")
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
        if replay_summary.get("objhit", 0) <= 0:
            failures.append(
                "arm B: =60 rebuilt no generation-keyed object matrices — "
                "presentation is still camera-only")
        if owner_summary.get("roots", 0) <= 0:
            failures.append(
                "arm B: =60 classified no object root matrices")
        if owner_summary.get("children", 0) <= 0:
            failures.append(
                "arm B: =60 classified no composed child matrices")
        owner_parts = sum(owner_summary.get(key, 0) for key in
                          ("roots", "children", "effects", "unowned"))
        if owner_summary.get("registrations") != owner_parts:
            failures.append(
                "arm B: matrix-owner census does not reconcile — "
                f"registrations={owner_summary.get('registrations')} vs "
                f"classified={owner_parts}")
        for key in ("matrixreg", "vertexreg", "matrixoverride",
                    "vertexoverride", "matrixpeak", "vertexpeak"):
            if packet_summary.get(key, 0) <= 0:
                failures.append(
                    f"arm B: retained billboard packet {key}="
                    f"{packet_summary.get(key)}; sprite ownership/interpolation "
                    "is not live")
        for key in ("deformreg", "deformhit", "deformoverride", "deformpeak"):
            if packet_summary.get(key, 0) <= 0:
                failures.append(
                    f"arm B: retained deformation packet {key}="
                    f"{packet_summary.get(key)}; animated model vertices are "
                    "still tick-held")
        if packet_summary.get("freezefail") != 0:
            failures.append(
                "arm B: retained billboard packet freezefail="
                f"{packet_summary.get('freezefail')}, expected 0")
        if packet_summary.get("deformcollision") != 0:
            failures.append(
                "arm B: retained deformation packet found "
                f"{packet_summary.get('deformcollision')} ambiguous "
                "owner/viewport/batch keys; those model streams need a more "
                "specific stable recipe")
        # A transient arena address can be rewritten after its last owned
        # billboard use by an unrelated command. The packet compares the live
        # bytes and refuses that dead tenant; a small nonzero census is evidence
        # the guard fired, not a reason to trust stale ownership. Bound the
        # measured tail so a lifecycle regression cannot grow unnoticed.
        if packet_summary.get("stale", 0) > 32:
            failures.append(
                "arm B: retained billboard packet refused "
                f"{packet_summary.get('stale')} stale keys, above the route "
                "budget of 32")
        notes.append(
            f"arm B: state, ordered-event, consumed-input, and PCM streams "
            f"byte-identical over "
            f"{TICKS} ticks for "
            f"MDKR_PRESENT_RATE unset, =30 and =60; =60 issued "
            f"{rate60_summary['presents']} presents "
            f"({rate60_summary['interp']} interpolated, "
            f"{rate60_summary['stale']} skipped on a stale display list); "
            f"{replay_summary.get('objhit', 0)} object-owned matrices rebuilt "
            f"across {owner_summary.get('roots', 0)} root and "
            f"{owner_summary.get('children', 0)} child registrations; "
            f"{packet_summary.get('matrixoverride', 0)} billboard matrices and "
            f"{packet_summary.get('vertexoverride', 0)} world-space anchors "
            f"rebuilt, plus {packet_summary.get('deformoverride', 0)} changed "
            f"model vertex batches blended from "
            f"{packet_summary.get('deformhit', 0)} compatible retained pairs "
            f"({packet_summary.get('stale', 0)} rewritten keys refused)")

        # ---- arm C: the smoothness witness ----------------------------
        try:
            smooth_on = run(binary, rom, "smooth-on", root,
                {"MDKR_PRESENT_RATE": "60", "MDKR_PRESENT_SCHED_TRACE": "1"},
                SMOOTH_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=SMOOTH_DUMP_FROM)
            run(binary, rom, "smooth-off", root,
                {"MDKR_PRESENT_RATE": "60", "MDKR_PRESENT_SMOOTHING": "off"},
                SMOOTH_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=SMOOTH_DUMP_FROM)
            object_off = run(binary, rom, "object-off", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_TEST_OBJECT_INTERPOLATION": "off"},
                SMOOTH_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=SMOOTH_DUMP_FROM)
            deformation_on = run(binary, rom, "deformation-on", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1"},
                DEFORMATION_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=DEFORMATION_DUMP_FROM)
            deformation_off = run(binary, rom, "deformation-off", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_TEST_DEFORMATION_INTERPOLATION": "off"},
                DEFORMATION_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=DEFORMATION_DUMP_FROM)
            particle_on = run(binary, rom, "particle-on", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_LOAD_TRACK": "26"},
                PARTICLE_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=PARTICLE_DUMP_FROM)
            particle_off = run(binary, rom, "particle-off", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_LOAD_TRACK": "26",
                 "MDKR_TEST_PARTICLE_INTERPOLATION": "off"},
                PARTICLE_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=PARTICLE_DUMP_FROM)
            particle_color_off = run(binary, rom, "particle-color-off", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_LOAD_TRACK": "26",
                 "MDKR_TEST_VERTEX_COLOR_INTERPOLATION": "off"},
                PARTICLE_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=PARTICLE_DUMP_FROM)
            particle_alpha_off = run(binary, rom, "particle-alpha-off", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_LOAD_TRACK": "26",
                 "MDKR_TEST_PRIMITIVE_ALPHA_INTERPOLATION": "off"},
                PARTICLE_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=PARTICLE_DUMP_FROM)
            effect_on = run(binary, rom, "effect-on", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_FORCE_SHIELD": EFFECT_FORCE_WINDOW},
                EFFECT_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=EFFECT_DUMP_FROM)
            effect_off = run(binary, rom, "effect-off", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_FORCE_SHIELD": EFFECT_FORCE_WINDOW,
                 "MDKR_TEST_EFFECT_INTERPOLATION": "off"},
                EFFECT_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=EFFECT_DUMP_FROM)
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm C: {error}")
            return 1

        moved_on, still_on = intermediate_frame_verdict(
            root / "smooth-on" / "frames")
        moved_off, still_off = intermediate_frame_verdict(
            root / "smooth-off" / "frames")
        object_compared, object_changed = compare_frame_dirs(
            root / "smooth-on" / "frames", root / "object-off" / "frames",
            odd_only=True)
        deformation_compared, deformation_changed = compare_frame_dirs(
            root / "deformation-on" / "frames",
            root / "deformation-off" / "frames", odd_only=True)
        particle_compared, particle_changed = compare_frame_dirs(
            root / "particle-on" / "frames",
            root / "particle-off" / "frames", odd_only=True)
        particle_color_compared, particle_color_changed = compare_frame_dirs(
            root / "particle-on" / "frames",
            root / "particle-color-off" / "frames", odd_only=True)
        particle_alpha_compared, particle_alpha_changed = compare_frame_dirs(
            root / "particle-on" / "frames",
            root / "particle-alpha-off" / "frames", odd_only=True)
        effect_compared, effect_changed = compare_frame_dirs(
            root / "effect-on" / "frames",
            root / "effect-off" / "frames", odd_only=True)
        try:
            smooth_replay = parse_last_fields(
                smooth_on, REPLAY_SUMMARY_RE, "REPLAY-SUMMARY")
            object_off_replay = parse_last_fields(
                object_off, REPLAY_SUMMARY_RE, "REPLAY-SUMMARY")
            object_off_packet = parse_last_fields(
                object_off, PACKET_SUMMARY_RE, "PRESENT-PACKET")
            deformation_off_packet = parse_last_fields(
                deformation_off, PACKET_SUMMARY_RE, "PRESENT-PACKET")
            deformation_on_packet = parse_last_fields(
                deformation_on, PACKET_SUMMARY_RE, "PRESENT-PACKET")
            particle_on_packet = parse_last_fields(
                particle_on, PACKET_SUMMARY_RE, "PRESENT-PACKET")
            particle_off_packet = parse_last_fields(
                particle_off, PACKET_SUMMARY_RE, "PRESENT-PACKET")
            particle_color_off_packet = parse_last_fields(
                particle_color_off, PACKET_SUMMARY_RE, "PRESENT-PACKET")
            particle_alpha_off_packet = parse_last_fields(
                particle_alpha_off, PACKET_SUMMARY_RE, "PRESENT-PACKET")
            effect_on_packet = parse_last_fields(
                effect_on, PACKET_SUMMARY_RE, "PRESENT-PACKET")
            effect_off_packet = parse_last_fields(
                effect_off, PACKET_SUMMARY_RE, "PRESENT-PACKET")
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm C: {error}")
            return 1
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
        if smooth_replay.get("objhit", 0) <= 0:
            failures.append(
                "arm C: full smoothing rebuilt no object matrices")
        if object_off_replay.get("objhit", -1) != 0:
            failures.append(
                "arm C: object-interpolation positive control did not "
                f"disable rebuilding (objhit={object_off_replay.get('objhit')})")
        if (object_off_packet.get("matrixoverride", -1) != 0 or
                object_off_packet.get("vertexoverride", -1) != 0):
            failures.append(
                "arm C: object-interpolation positive control left retained "
                "billboard rebuilding enabled")
        if object_compared == 0:
            failures.append(
                "arm C: object pixel witness found no comparable intermediate "
                "frames")
        elif object_changed == 0:
            failures.append(
                "arm C: every intermediate frame is byte-identical with object "
                "interpolation enabled and disabled — the object path has no "
                "visible effect")
        if deformation_off_packet.get("deformoverride", -1) != 0:
            failures.append(
                "arm C: deformation-interpolation positive control still "
                "blended retained batches "
                "(deformoverride="
                f"{deformation_off_packet.get('deformoverride')})")
        if deformation_on_packet.get("deformoverride", 0) <= 0:
            failures.append(
                "arm C: race witness applied no changed deformation batches")
        if deformation_compared == 0:
            failures.append(
                "arm C: deformation pixel witness found no comparable "
                "intermediate frames")
        elif deformation_changed == 0:
            failures.append(
                "arm C: every intermediate frame is byte-identical with "
                "deformation interpolation enabled and disabled — retained "
                "model vertices have no visible effect")
        particle_on_rows = sim_hash_rows(particle_on)
        particle_off_rows = sim_hash_rows(particle_off)
        particle_color_off_rows = sim_hash_rows(particle_color_off)
        particle_alpha_off_rows = sim_hash_rows(particle_alpha_off)
        if (len(particle_on_rows) != PARTICLE_TICKS or
                len(particle_off_rows) != PARTICLE_TICKS or
                len(particle_color_off_rows) != PARTICLE_TICKS or
                len(particle_alpha_off_rows) != PARTICLE_TICKS):
            failures.append(
                "arm C: particle witness did not complete exactly "
                f"{PARTICLE_TICKS} authoritative ticks "
                f"(on={len(particle_on_rows)}, off={len(particle_off_rows)}, "
                f"color-off={len(particle_color_off_rows)}, "
                f"alpha-off={len(particle_alpha_off_rows)})")
        elif (particle_on_rows != particle_off_rows or
                particle_on_rows != particle_color_off_rows or
                particle_on_rows != particle_alpha_off_rows):
            comparison = next(
                rows for rows in (
                    particle_off_rows,
                    particle_color_off_rows,
                    particle_alpha_off_rows,
                )
                if rows != particle_on_rows
            )
            index = first_difference(particle_on_rows, comparison)
            failures.append(
                "arm C: particle/scalar interpolation changed authoritative "
                "state at "
                f"tick {index}")
        for key in ("particlevertexreg", "particledeformhit",
                    "particledeformoverride", "particlecolorhit",
                    "particlecoloroverride"):
            if particle_on_packet.get(key, 0) <= 0:
                failures.append(
                    f"arm C: particle witness {key}="
                    f"{particle_on_packet.get(key)}; the retained point-trail "
                    "path is not live")
        if particle_on_packet.get("deformcollision") != 0:
            failures.append(
                "arm C: particle witness found ambiguous retained keys "
                f"(deformcollision="
                f"{particle_on_packet.get('deformcollision')})")
        if particle_off_packet.get("particledeformhit", -1) != 0 or \
                particle_off_packet.get("particledeformoverride", -1) != 0:
            failures.append(
                "arm C: particle-interpolation positive control left the "
                "particle replay path enabled")
        if (particle_color_off_packet.get("colorhit", -1) != 0 or
                particle_color_off_packet.get("coloroverride", -1) != 0 or
                particle_color_off_packet.get("particlecolorhit", -1) != 0 or
                particle_color_off_packet.get(
                    "particlecoloroverride", -1) != 0):
            failures.append(
                "arm C: vertex-color positive control left retained RGBA "
                "replay enabled")
        if (particle_on_packet.get("particleprimalphahit", 0) <= 0 or
                particle_on_packet.get(
                    "particleprimalphaoverride", 0) <= 0):
            failures.append(
                "arm C: particle witness applied no changed primitive-alpha "
                "overrides")
        if (particle_alpha_off_packet.get("primalphahit", -1) != 0 or
                particle_alpha_off_packet.get("primalphaoverride", -1) != 0 or
                particle_alpha_off_packet.get(
                    "particleprimalphahit", -1) != 0 or
                particle_alpha_off_packet.get(
                    "particleprimalphaoverride", -1) != 0):
            failures.append(
                "arm C: primitive-alpha positive control left scalar replay "
                "enabled")
        if particle_compared == 0:
            failures.append(
                "arm C: particle pixel witness found no comparable "
                "intermediate frames")
        elif particle_changed == 0:
            failures.append(
                "arm C: every battle-challenge intermediate frame is "
                "byte-identical with particle interpolation enabled and "
                "disabled — retained point-trail vertices have no visible "
                "effect")
        if particle_color_compared == 0:
            failures.append(
                "arm C: particle color witness found no comparable "
                "intermediate frames")
        elif particle_color_changed == 0:
            failures.append(
                "arm C: every battle-challenge intermediate frame is "
                "byte-identical with vertex-color interpolation enabled and "
                "disabled — retained RGBA has no visible effect")
        if particle_alpha_compared == 0:
            failures.append(
                "arm C: particle primitive-alpha witness found no comparable "
                "intermediate frames")
        elif particle_alpha_changed == 0:
            failures.append(
                "arm C: every battle-challenge intermediate frame is "
                "byte-identical with primitive-alpha interpolation enabled "
                "and disabled — interpolated fades have no visible effect")
        effect_on_rows = sim_hash_rows(effect_on)
        effect_off_rows = sim_hash_rows(effect_off)
        if (len(effect_on_rows) != EFFECT_TICKS or
                len(effect_off_rows) != EFFECT_TICKS):
            failures.append(
                "arm C: effect witness did not complete exactly "
                f"{EFFECT_TICKS} authoritative ticks "
                f"(on={len(effect_on_rows)}, off={len(effect_off_rows)})")
        elif effect_on_rows != effect_off_rows:
            index = first_difference(effect_on_rows, effect_off_rows)
            failures.append(
                "arm C: effect interpolation changed authoritative state at "
                f"tick {index}")
        for key in ("effectreg", "effecthit", "effectoverride"):
            if effect_on_packet.get(key, 0) <= 0:
                failures.append(
                    f"arm C: shield effect witness {key}="
                    f"{effect_on_packet.get(key)}; the retained semantic "
                    "matrix path is not live")
        if effect_on_packet.get("effectcollision") != 0:
            failures.append(
                "arm C: shield effect witness found ambiguous two-identity "
                f"keys (effectcollision="
                f"{effect_on_packet.get('effectcollision')})")
        if (effect_off_packet.get("effecthit", -1) != 0 or
                effect_off_packet.get("effectoverride", -1) != 0):
            failures.append(
                "arm C: effect-interpolation positive control left the "
                "semantic matrix replay path enabled")
        if effect_compared == 0:
            failures.append(
                "arm C: shield effect pixel witness found no comparable "
                "intermediate frames")
        elif effect_changed == 0:
            failures.append(
                "arm C: every forced-shield intermediate frame is "
                "byte-identical with effect interpolation enabled and "
                "disabled — the reconstructed shear matrix has no visible "
                "effect")
        notes.append(
            f"arm C: with smoothing on, {moved_on}/{moved_on + still_on} "
            f"interpolated frames differ from both neighbours; with "
            f"MDKR_PRESENT_SMOOTHING=off, {moved_off}/{moved_off + still_off} "
            "do (every intermediate frame is a byte-identical repeat); "
            f"object interpolation changes {object_changed}/{object_compared} "
            "intermediate backend frames against the camera-identical control")
        notes.append(
            f"arm C: retained deformation changes {deformation_changed}/"
            f"{deformation_compared} intermediate backend frames while its "
            "control leaves camera/root/billboard smoothing enabled")
        notes.append(
            f"arm C: battle point trails blend "
            f"{particle_on_packet.get('particledeformoverride', 0)} changed "
            f"batches from "
            f"{particle_on_packet.get('particledeformhit', 0)} compatible "
            f"pairs and change {particle_changed}/{particle_compared} "
            "intermediate backend frames; the particle-only control is "
            "authoritatively byte-identical")
        notes.append(
            f"arm C: retained point-trail RGBA applies "
            f"{particle_on_packet.get('particlecoloroverride', 0)} changed "
            f"batches from "
            f"{particle_on_packet.get('particlecolorhit', 0)} compatible "
            f"pairs and independently changes {particle_color_changed}/"
            f"{particle_color_compared} intermediate backend frames; its "
            "color-only control preserves XYZ replay and authoritative state")
        notes.append(
            f"arm C: particle primitive alpha applies "
            f"{particle_on_packet.get('particleprimalphaoverride', 0)} changed "
            f"draws from "
            f"{particle_on_packet.get('particleprimalphahit', 0)} compatible "
            f"pairs and independently changes {particle_alpha_changed}/"
            f"{particle_alpha_compared} intermediate backend frames; point "
            "trails remain single-scaled and the alpha-only control is "
            "authoritatively byte-identical")
        notes.append(
            f"arm C: forced shield/magnet-class recipes apply "
            f"{effect_on_packet.get('effectoverride', 0)} semantic matrix "
            f"overrides and change {effect_changed}/{effect_compared} "
            "intermediate backend frames; the effect-only control is "
            "authoritatively byte-identical and collision-free")

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
