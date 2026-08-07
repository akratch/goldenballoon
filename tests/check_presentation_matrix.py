#!/usr/bin/env python3
"""Presentation rate must not move the authoritative tick (spec 12.2.2).

The presentation scheduler decouples host image opportunities from authoritative
simulation ticks. Production can publish immutable, presentation-only images at
arbitrary rates while the game continues to receive the original fixed tickets.

Arms
----

* **A — fixed-ticket authority.** The promoted ``HostFrameDriver`` clock must
  conserve every two-field ticket on the floor-paced original schedule. Clock
  ticks, issued-plus-pending tickets, completed simulation ticks, state rows,
  and event rows must agree with zero live debt or update-rate violations. The
  authority arm is repeated on WebGPU and compared byte-for-byte with GL.

* **B — rate matrix (slice 2).** The per-tick ``[SIMHASH]`` state stream,
  ordered ``[EVENTHASH]`` gameplay-event stream, consumed input, and temporary
  PCM capture must all be byte-identical
  with ``MDKR_PRESENT_RATE`` unset, ``=30``, and ``=60`` over the same route.
  ``=60`` presents twice per tick, so it is given twice the headless frame
  budget to reach the same 3600 TICKS — authority is counted in ticks, not
  images.

  The unset and ``=60`` endpoints are also run on WebGPU. Both 60 Hz arms use
  the public ``MotionSmoothing=interpolate`` request and must produce real
  intermediate replay walks while preserving every authority stream.

* **C — production mechanism diagnostics.** ``=60`` plus the public
  ``MotionSmoothing=interpolate`` setting alternates a
  completed real endpoint and a production interpolated midpoint. Every
  midpoint must differ from BOTH of its neighbours: same retained display
  list, moved camera. The positive
  control is production ``MDKR_PRESENT_SMOOTHING=off``, whose extra host
  opportunities do not swap or submit a new surface image. Its backend dump
  remains the prior completed authored image.

  A second positive control, ``MDKR_TEST_OBJECT_INTERPOLATION=off``, keeps the
  same interpolated camera but disables generation-keyed matrix rebuilding.
  At least one intermediate frame must then differ from the fully smoothed
  arm, proving the new object path changes backend pixels rather than merely
  incrementing telemetry.

  A third control, ``MDKR_TEST_DEFORMATION_INTERPOLATION=off``, proves the true
  forward-pair path. Enabled replay must consume adjacent ``{T,T+1}`` authored
  vertex streams and visibly differ from an exact-task hold. A semantic observer
  at ``G_VTX`` separately requires byte-identical alpha-zero endpoints.

  A fourth control, ``MDKR_TEST_PARTICLE_INTERPOLATION=off``, runs the battle
  challenge whose continuous point trails change their world-space mesh from
  tick to tick. It requires adjacent retained capture and a visible geometry
  difference against the particle-only hold control.

  A fifth control, ``MDKR_TEST_VERTEX_COLOR_INTERPOLATION=off``, independently
  proves retained shade RGBA is blended only from the same adjacent pair. It
  must visibly differ from the color-hold control without changing authority.

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
  free and the authoritative stream to remain identical. Compatible forward
  recipes must visibly differ from the authored-matrix hold control.

  An eighth control, ``MDKR_TEST_UV_SCROLL_INTERPOLATION=off``, is the
  midpoint-sensitivity arm for authored UV scroll. It runs Jungle Falls, whose
  waterfall sheet and wave-driven water advance their texture phase every
  authored tick. At an interpolated midpoint the scrolling phase must differ
  from the phase held at BOTH bracketing endpoints, which is exactly what the
  control measures: with the seam off the midpoint reproduces the endpoint
  phase, so a pixel difference between the two arms at ODD present indices is
  the interpolation, and byte-identity at EVEN indices proves the authored
  endpoints did not move. It is non-vacuous by construction — the off arm is
  the pre-M4 renderer's behaviour.

What arm C's pixels are, and are not
------------------------------------

Arm C compares dumped PPM frames, and those come from ``platform_dump_frame``
(platform_sdl_min.c), which reads the BACKEND's captured frame —
``gfx_read_framebuffer_rgb`` / the GL readback of the render target — *before*
the swap. They are not a capture of the swapchain, and nothing in this suite
observes the swapchain at all.

The ship review found that an opportunity with nothing newly drawn used to call
``platform_frame_sync``
and therefore ``SDL_GL_SwapWindow`` on a back buffer whose contents are undefined
after the previous swap. On screen that is flicker at
``FrameLimit=60``/``MotionSmoothing=off``; in this gate it was invisible, because
the backend's captured frame still held the tick's image and arm C's positive
control read exactly that and passed. The engine now skips the swap on the
no-draw paths (``platform_frame_sync_no_swap``), which is what makes the "repeat
the authored image remain visible; arm C keeps working because the dump is
unaffected. The separate ``surfaceupdates`` counter now pins the number of
successful GL swaps/WebGPU surface presents, while these PPMs remain a backend
content oracle rather than an out-of-process display capture.

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

from harness_utils import (completed_tick_conservation, DEFAULT_BUILD_DIR,
                           parse_last, resolve_binary, tear_free_presentation)

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
TICKS = 3600
# Presentation invariance is asserted against the current authority contract.
# Archived v1/v2 streams remain selectable only in check_state_hash.py.
HASH_VERSION = "3"

PERF_RE = re.compile(r"\[PRESENTPERF\] (.*)")
ENDPOINT_RE = re.compile(
    r"\[PRESENT-ENDPOINT\] frame=(\d+) authored=(\d+) source=(real|replay)")
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
# Jungle Falls (level 29) is the authored-UV-scroll witness: its waterfall sheet
# is a level-model triangle batch scrolling V by 16 S10.5 units (half a texel) a
# tick, and its wave-driven water is a ping-pong surface scrolling in both axes.
# Measured override density over the route is one confirmed site on every tick
# from 3000 to 3300; the final 60 presents of that band are the sampled window.
UV_SCROLL_TICKS = 3230
UV_SCROLL_DUMP_FROM = 6400
UV_SCROLL_TRACK = "29"
PRESENT_WORK_BUDGET_NS = 16_666_667
TEST_REPLAY_ENV = {
    "MDKR_PRESENT_SMOOTHING": "interpolate",
}


def parse_summary(text: str) -> dict[str, int]:
    return parse_last(text, "PRESENTSCHED-SUMMARY")


def parse_audio_summary(text: str) -> dict[str, int]:
    return parse_last(text, "AUDIO-SERVICE")


def parse_perf_fields(text: str) -> dict[str, dict[str, int]]:
    sections: dict[str, dict[str, int]] = {}
    for match in PERF_RE.finditer(text):
        fields: dict[str, int] = {}
        name = ""
        for token in match.group(1).split():
            key, separator, value = token.partition("=")
            if not separator:
                continue
            if key == "section":
                name = value
            else:
                fields[key] = int(value)
        if name:
            sections[name] = fields
    return sections


def run(binary: Path, rom: Path, label: str, root: Path,
        extra_env: dict[str, str], frames: int, timeout: int,
        verbose: bool, dump_from: int | None = None,
        audio_capture: bool = False, renderer: str = "gl") -> str:
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
        MDKR_RENDERER=renderer,
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
    )
    if renderer == "webgpu":
        # Native hidden WebGPU windows have no drawable by design. The GL/WebGPU
        # production arm asserts physical surface commits, so keep this finite
        # automation window compositor-visible.
        env["MDKR_TEST_VISIBLE_HEADLESS"] = "1"
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


def endpoint_rows(output: str) -> dict[int, tuple[int, str]]:
    rows: dict[int, tuple[int, str]] = {}
    for match in ENDPOINT_RE.finditer(output):
        frame = int(match.group(1))
        if frame in rows:
            raise RuntimeError(f"duplicate endpoint token for frame {frame}")
        rows[frame] = (int(match.group(2)), match.group(3))
    return rows


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


def compare_frame_dirs(left: Path, right: Path,
                       parity: int | None = None) -> tuple[int, int]:
    left_frames = {path.name: path.read_bytes() for path in left.glob("*.ppm")}
    right_frames = {path.name: path.read_bytes() for path in right.glob("*.ppm")}
    compared = different = 0
    for name in sorted(left_frames.keys() & right_frames.keys()):
        if (parity is not None and
                int(Path(name).stem.split("_")[1]) % 2 != parity):
            continue
        compared += 1
        if left_frames[name] != right_frames[name]:
            different += 1
    return compared, different


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

        conservation_error = completed_tick_conservation(summary, TICKS,
                                                          "arm A GL")
        if conservation_error:
            failures.append(conservation_error)
        failures.extend(tear_free_presentation(baseline, "arm A GL unset"))
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
        for key in ("lag", "zerodue", "multidue", "rebases",
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
                          "MDKR_PRESENT_SMOOTHING": "interpolate",
                          "MDKR_PRESENT_SCHED_TRACE": "1"},
                         TICKS * RATE60_PRESENTS_PER_TICK,
                         args.timeout, args.verbose, audio_capture=True)
            webgpu_unset = run(
                binary, rom, "webgpu-clock-agreement", root,
                {"MDKR_PRESENT_SCHED_TRACE": "1"}, TICKS,
                args.timeout, args.verbose, audio_capture=True,
                renderer="webgpu")
            webgpu_rate60 = run(
                binary, rom, "webgpu-rate-60", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SMOOTHING": "interpolate",
                 "MDKR_PRESENT_SCHED_TRACE": "1"},
                TICKS * RATE60_PRESENTS_PER_TICK,
                args.timeout, args.verbose, audio_capture=True,
                renderer="webgpu")
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
        for label, other_output, capture_label in (
                ("GL =30", rate30, "rate-30"),
                ("GL =60", rate60, "rate-60"),
                ("WebGPU unset", webgpu_unset, "webgpu-clock-agreement"),
                ("WebGPU =60", webgpu_rate60, "webgpu-rate-60")):
            failures.extend(
                tear_free_presentation(other_output, f"arm B: {label}"))
            other = sim_hash_rows(other_output)
            if len(other) != TICKS:
                failures.append(
                    f"arm B: {label} produced {len(other)} "
                    f"[SIMHASH] rows, expected {TICKS}. The presentation rate "
                    "changed how many authoritative ticks ran, which is the "
                    "one thing it must never do")
                continue
            index = first_difference(unset_rows, other)
            if index is not None:
                failures.append(
                    f"arm B: {label} diverges from the GL unset "
                    f"stream at tick {index} ({unset_rows[index]} vs "
                    f"{other[index]}) — presentation rate reached authoritative "
                    "state")
            other_events = event_hash_rows(other_output)
            if len(other_events) != TICKS:
                failures.append(
                    f"arm B: {label} produced "
                    f"{len(other_events)} [EVENTHASH] rows, expected {TICKS}")
            else:
                event_index = first_difference(unset_events, other_events)
                if event_index is not None:
                    failures.append(
                        f"arm B: {label} event stream "
                        f"diverges at tick {event_index} — presentation rate "
                        "changed an ordered gameplay event")
            other_inputs = input_hash_rows(other_output)
            if len(other_inputs) != TICKS:
                failures.append(
                    f"arm B: {label} produced "
                    f"{len(other_inputs)} [INPUTHASH] rows, expected {TICKS}")
            else:
                input_index = first_difference(unset_inputs, other_inputs)
                if input_index is not None:
                    failures.append(
                        f"arm B: {label} consumed-input "
                        f"stream diverges at tick {input_index}")
            try:
                other_audio = parse_audio_summary(other_output)
                other_pcm = audio_digest(root, capture_label)
            except RuntimeError as error:
                failures.append(f"arm B: {label}: {error}")
                continue
            for key in ("fields", "due", "serviced", "pending", "samples",
                        "retired", "notready", "dropped", "quantumfields"):
                if other_audio.get(key) != unset_audio.get(key):
                    failures.append(
                        f"arm B: {label} audio {key}="
                        f"{other_audio.get(key)} differs from unset "
                        f"{unset_audio.get(key)}")
            if other_pcm != unset_pcm:
                failures.append(
                    f"arm B: {label} synthesized PCM differs "
                    "from the unset schedule")
        try:
            rate60_summary = parse_summary(rate60)
            replay_summary = parse_last(rate60, "REPLAY-SUMMARY")
            retained_summary = parse_last(rate60, "RETAINED-TASK")
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm B: {error}")
            return 1
        if rate60_summary["presents"] != TICKS * RATE60_PRESENTS_PER_TICK:
            failures.append(
                f"arm B: =60 presented {rate60_summary['presents']} frames for "
                f"{TICKS} ticks; expected {TICKS * RATE60_PRESENTS_PER_TICK}, "
                "so the subloop is not actually running at the requested rate")
        if rate60_summary["interp"] <= 0:
            failures.append(
                "arm B: public production =60 smoothing produced no "
                "interpolated images")
        if rate60_summary["simticks"] != TICKS:
            failures.append(
                f"arm B: =60 advanced {rate60_summary['simticks']} "
                f"authoritative ticks, expected {TICKS}")
        conservation_error = completed_tick_conservation(rate60_summary, TICKS,
                                                          "arm B GL =60")
        if conservation_error:
            failures.append(conservation_error)
        try:
            webgpu_unset_summary = parse_summary(webgpu_unset)
            webgpu_rate60_summary = parse_summary(webgpu_rate60)
            webgpu_replay_summary = parse_last(webgpu_rate60, "REPLAY-SUMMARY")
            webgpu_retained_summary = parse_last(webgpu_rate60, "RETAINED-TASK")
            webgpu_backpressure = parse_last(webgpu_rate60, "WGPU-BACKPRESSURE")
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm B WebGPU: {error}")
            return 1
        for backend_label, backend_sched in (
                ("arm A WebGPU", webgpu_unset_summary),
                ("arm B WebGPU =60", webgpu_rate60_summary)):
            conservation_error = completed_tick_conservation(backend_sched,
                                                              TICKS,
                                                              backend_label)
            if conservation_error:
                failures.append(conservation_error)
        for key in ("ticks", "presents", "simticks", "tickfields",
                    "blocked", "maxpending", "updatebad", "updatemin",
                    "updatemax", "lead", "lag", "zerodue", "multidue",
                    "rebases", "catchup", "skips"):
            if webgpu_unset_summary.get(key) != summary.get(key):
                failures.append(
                    f"arm A: WebGPU {key}="
                    f"{webgpu_unset_summary.get(key)} differs from GL "
                    f"{summary.get(key)}")
        if webgpu_rate60_summary.get("presents") != \
                TICKS * RATE60_PRESENTS_PER_TICK:
            failures.append(
                f"arm B: WebGPU =60 presented "
                f"{webgpu_rate60_summary.get('presents')} frames for {TICKS} "
                f"ticks; expected {TICKS * RATE60_PRESENTS_PER_TICK}")
        if webgpu_rate60_summary.get("interp", 0) <= 0:
            failures.append(
                "arm B: public WebGPU =60 smoothing produced no interpolated image")
        if webgpu_rate60_summary.get("simticks") != TICKS:
            failures.append(
                f"arm B: WebGPU =60 advanced "
                f"{webgpu_rate60_summary.get('simticks')} authoritative "
                f"ticks, expected {TICKS}")
        for backend_label, backend_sched, backend_replay, backend_retained in (
                ("GL", rate60_summary, replay_summary, retained_summary),
                ("WebGPU", webgpu_rate60_summary, webgpu_replay_summary,
                 webgpu_retained_summary)):
            if (backend_replay.get("walks", 0) <= 0 or
                    backend_replay.get("walks") != backend_sched.get("interp")):
                failures.append(
                    f"arm B: production {backend_label} replay walks="
                    f"{backend_replay.get('walks')} do not match successful "
                    f"interpolated images={backend_sched.get('interp')}")
            if backend_sched.get("replayendpoints", -1) != 0:
                failures.append(
                    f"arm B: production {backend_label} redrew "
                    f"{backend_sched.get('replayendpoints')} endpoints")
            expected_real_endpoints = TICKS - 3
            if (backend_label == "GL" and
                    backend_sched.get("realendpoints") !=
                    expected_real_endpoints):
                failures.append(
                    f"arm B: production {backend_label} exposed "
                    f"{backend_sched.get('realendpoints')} completed "
                    f"real-walk endpoints, expected {expected_real_endpoints}; "
                    "the three-opportunity bootstrap has no graphics task")
            intended_updates = (backend_sched.get("realendpoints", 0) +
                                backend_sched.get("interp", 0))
            surface_updates = backend_sched.get("surfaceupdates", -1)
            if backend_label == "GL" and surface_updates != intended_updates:
                failures.append(
                    f"arm B: production {backend_label} committed "
                    f"{surface_updates} surface images for "
                    f"{intended_updates} real+interpolated images")
            if (backend_retained.get("captures", 0) <= 0 or
                    backend_retained.get("captures") !=
                    backend_retained.get("begins") or
                    backend_retained.get("failures", -1) != 0 or
                    backend_retained.get("rejects", -1) != 0 or
                    backend_retained.get("arenaResolve", 0) <= 0 or
                    backend_retained.get("externalResolve", 0) <= 0):
                failures.append(
                    f"arm B: production {backend_label} retained-task "
                    f"publication was incomplete: {backend_retained}")
        webgpu_endpoint_skips = webgpu_backpressure.get("endpointSkips", -1)
        webgpu_replay_skips = webgpu_backpressure.get("replaySkips", -1)
        webgpu_admission_skips = webgpu_backpressure.get("skips", -1)
        if (webgpu_rate60_summary.get("realendpoints", -1) +
                webgpu_endpoint_skips != TICKS - 3):
            failures.append(
                "arm B: WebGPU real endpoints plus endpoint admission holds "
                f"do not account for {TICKS - 3} authored tasks")
        if webgpu_admission_skips != (
                webgpu_endpoint_skips + webgpu_replay_skips):
            failures.append(
                "arm B: WebGPU admission skips do not reconcile by endpoint "
                "and replay class")
        if (webgpu_backpressure.get("submitted", -1) !=
                webgpu_rate60_summary.get("realendpoints", 0) +
                webgpu_rate60_summary.get("interp", 0)):
            failures.append(
                "arm B: WebGPU submissions do not match successful real and "
                "interpolated images")
        if (webgpu_rate60_summary.get("realendpoints", 0) +
                webgpu_rate60_summary.get("interp", 0) +
                webgpu_rate60_summary.get("stale", 0) !=
                webgpu_rate60_summary.get("presents", -1)):
            failures.append(
                "arm B: WebGPU real/interpolated/held images do not account "
                "for every host opportunity")
        for key in ("failures", "abandoned", "inflight", "runtimewaits",
                    "runtimewaitns"):
            if webgpu_backpressure.get(key, 0) != 0:
                failures.append(
                    f"arm B: WebGPU {key}="
                    f"{webgpu_backpressure.get(key)}, expected 0")
        if not (1 <= webgpu_backpressure.get("highwater", 0) <= 2):
            failures.append(
                "arm B: WebGPU in-flight high-water escaped the two-frame "
                f"bound: {webgpu_backpressure.get('highwater')}")
        webgpu_surface = webgpu_rate60_summary.get("surfaceupdates", -1)
        webgpu_intended = (webgpu_rate60_summary.get("realendpoints", 0) +
                           webgpu_rate60_summary.get("interp", 0))
        webgpu_deficit = webgpu_intended - webgpu_surface
        accounted_holds = (webgpu_backpressure.get("held", 0) +
                           webgpu_backpressure.get("unavailable", 0))
        if webgpu_surface != webgpu_backpressure.get("presented"):
            failures.append(
                "arm B: WebGPU surface-update counter differs from backend "
                "presented telemetry")
        if (webgpu_deficit < 0 or webgpu_deficit > accounted_holds or
                webgpu_backpressure.get("failures", -1) != 0):
            failures.append(
                f"arm B: WebGPU image-to-surface deficit {webgpu_deficit} "
                f"is not bounded by held+unavailable={accounted_holds}, or "
                "the backend reported a completion failure")
        notes.append(
            f"arm B: state, ordered-event, consumed-input, and PCM streams "
            f"byte-identical over "
            f"{TICKS} ticks for "
            f"MDKR_PRESENT_RATE unset, =30 and =60; =60 issued "
            f"{rate60_summary['presents']} opportunities, exposed "
            f"{rate60_summary['realendpoints']} completed real-walk endpoints "
            f"plus {rate60_summary['interp']} immutable interpolated images "
            "through the public MotionSmoothing=interpolate setting")
        notes.append(
            f"arm A/B WebGPU: {webgpu_unset_summary.get('ticks')} debt-free "
            "fixed tickets match GL authority; =60 issued "
            f"{webgpu_rate60_summary.get('presents')} host opportunities with "
            f"{webgpu_rate60_summary.get('realendpoints')} real endpoints and "
            f"{webgpu_rate60_summary.get('interp')} interpolated images, while "
            "state/event/input/PCM remained byte-identical to GL unset")

        # ---- arm C: the smoothness witness ----------------------------
        try:
            smooth_on = run(binary, rom, "smooth-on", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_PERF": "1",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_TEST_RETAINED_DEPENDENCY_REWRITE": "1",
                 "MDKR_INTERNAL_TEST_TOKEN":
                     "mdkr64-presentation-replay-v1",
                 "MDKR_TEST_RETAINED_ARENA_POISON": "1"},
                SMOOTH_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=SMOOTH_DUMP_FROM)
            smooth_off = run(binary, rom, "smooth-off", root,
                {"MDKR_PRESENT_RATE": "60", "MDKR_PRESENT_SMOOTHING": "off",
                 "MDKR_PRESENT_SCHED_TRACE": "1"},
                SMOOTH_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=SMOOTH_DUMP_FROM)
            delayed_endpoint = run(
                binary, rom, "delayed-endpoint-control", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_INTERNAL_TEST_TOKEN":
                     "mdkr64-presentation-replay-v1",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_TEST_DELAYED_ENDPOINT_REPLAY": "1",
                 "MDKR_TEST_ENDPOINT_VERTEX_BYTES": "1",
                 "MDKR_TEST_RETAINED_DEPENDENCY_REWRITE": "1",
                 "MDKR_TEST_RETAINED_ARENA_POISON": "1"},
                SMOOTH_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=SMOOTH_DUMP_FROM)
            webgpu_mechanism = run(
                binary, rom, "webgpu-mechanism", root,
                {"MDKR_PRESENT_RATE": "60",
                 "MDKR_PACE_REALTIME": "1",
                 "MDKR_INTERNAL_TEST_TOKEN":
                     "mdkr64-presentation-replay-v1",
                 "MDKR_PRESENT_SMOOTHING": "interpolate",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_TEST_RETAINED_ARENA_POISON": "1"},
                SMOOTH_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, renderer="webgpu")
            object_off = run(binary, rom, "object-off", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_TEST_OBJECT_INTERPOLATION": "off"},
                SMOOTH_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=SMOOTH_DUMP_FROM)
            deformation_on = run(binary, rom, "deformation-on", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_TEST_ENDPOINT_VERTEX_BYTES": "1"},
                DEFORMATION_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=DEFORMATION_DUMP_FROM)
            deformation_off = run(binary, rom, "deformation-off", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_TEST_DEFORMATION_INTERPOLATION": "off"},
                DEFORMATION_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=DEFORMATION_DUMP_FROM)
            particle_on = run(binary, rom, "particle-on", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_LOAD_TRACK": "26"},
                PARTICLE_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=PARTICLE_DUMP_FROM)
            particle_off = run(binary, rom, "particle-off", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_LOAD_TRACK": "26",
                 "MDKR_TEST_PARTICLE_INTERPOLATION": "off"},
                PARTICLE_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=PARTICLE_DUMP_FROM)
            particle_color_off = run(binary, rom, "particle-color-off", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_LOAD_TRACK": "26",
                 "MDKR_TEST_VERTEX_COLOR_INTERPOLATION": "off"},
                PARTICLE_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=PARTICLE_DUMP_FROM)
            particle_alpha_off = run(binary, rom, "particle-alpha-off", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_LOAD_TRACK": "26",
                 "MDKR_TEST_PRIMITIVE_ALPHA_INTERPOLATION": "off"},
                PARTICLE_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=PARTICLE_DUMP_FROM)
            effect_on = run(binary, rom, "effect-on", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_FORCE_SHIELD": EFFECT_FORCE_WINDOW},
                EFFECT_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=EFFECT_DUMP_FROM)
            effect_off = run(binary, rom, "effect-off", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_FORCE_SHIELD": EFFECT_FORCE_WINDOW,
                 "MDKR_TEST_EFFECT_INTERPOLATION": "off"},
                EFFECT_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=EFFECT_DUMP_FROM)
            uv_scroll_on = run(binary, rom, "uv-scroll-on", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_LOAD_TRACK": UV_SCROLL_TRACK},
                UV_SCROLL_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=UV_SCROLL_DUMP_FROM)
            uv_scroll_off = run(binary, rom, "uv-scroll-off", root,
                {**TEST_REPLAY_ENV, "MDKR_PRESENT_RATE": "60",
                 "MDKR_PRESENT_SCHED_TRACE": "1",
                 "MDKR_LOAD_TRACK": UV_SCROLL_TRACK,
                 "MDKR_TEST_UV_SCROLL_INTERPOLATION": "off"},
                UV_SCROLL_TICKS * RATE60_PRESENTS_PER_TICK, args.timeout,
                args.verbose, dump_from=UV_SCROLL_DUMP_FROM)
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm C: {error}")
            return 1

        moved_on, still_on = intermediate_frame_verdict(
            root / "smooth-on" / "frames")
        moved_off, still_off = intermediate_frame_verdict(
            root / "smooth-off" / "frames")
        object_compared, object_changed = compare_frame_dirs(
            root / "smooth-on" / "frames", root / "object-off" / "frames",
            parity=1)
        endpoint_compared, endpoint_changed = compare_frame_dirs(
            root / "smooth-on" / "frames", root / "smooth-off" / "frames",
            parity=0)
        delayed_compared, delayed_changed = compare_frame_dirs(
            root / "delayed-endpoint-control" / "frames",
            root / "smooth-off" / "frames", parity=0)
        deformation_compared, deformation_changed = compare_frame_dirs(
            root / "deformation-on" / "frames",
            root / "deformation-off" / "frames")
        particle_compared, particle_changed = compare_frame_dirs(
            root / "particle-on" / "frames",
            root / "particle-off" / "frames")
        particle_color_compared, particle_color_changed = compare_frame_dirs(
            root / "particle-on" / "frames",
            root / "particle-color-off" / "frames")
        particle_alpha_compared, particle_alpha_changed = compare_frame_dirs(
            root / "particle-on" / "frames",
            root / "particle-alpha-off" / "frames", parity=1)
        effect_compared, effect_changed = compare_frame_dirs(
            root / "effect-on" / "frames",
            root / "effect-off" / "frames")
        # Odd indices are the interpolated midpoints, even indices the authored
        # endpoints. The scroll seam must move the first set and no other.
        uv_mid_compared, uv_mid_changed = compare_frame_dirs(
            root / "uv-scroll-on" / "frames",
            root / "uv-scroll-off" / "frames", parity=1)
        uv_end_compared, uv_end_changed = compare_frame_dirs(
            root / "uv-scroll-on" / "frames",
            root / "uv-scroll-off" / "frames", parity=0)
        uv_moved, uv_still = intermediate_frame_verdict(
            root / "uv-scroll-on" / "frames")
        try:
            smooth_replay = parse_last(smooth_on, "REPLAY-SUMMARY")
            smooth_retained = parse_last(smooth_on, "RETAINED-TASK")
            smooth_summary = parse_summary(smooth_on)
            smooth_off_summary = parse_summary(smooth_off)
            delayed_summary = parse_summary(delayed_endpoint)
            delayed_packet = parse_last(delayed_endpoint, "PRESENT-PACKET")
            delayed_retained = parse_last(delayed_endpoint, "RETAINED-TASK")
            smooth_perf = parse_perf_fields(smooth_on)
            smooth_endpoints = endpoint_rows(smooth_on)
            smooth_off_endpoints = endpoint_rows(smooth_off)
            delayed_endpoints = endpoint_rows(delayed_endpoint)
            webgpu_mechanism_summary = parse_summary(webgpu_mechanism)
            webgpu_mechanism_replay = parse_last(webgpu_mechanism, "REPLAY-SUMMARY")
            webgpu_mechanism_retained = parse_last(webgpu_mechanism, "RETAINED-TASK")
            webgpu_mechanism_backpressure = parse_last(webgpu_mechanism, "WGPU-BACKPRESSURE")
            object_off_replay = parse_last(object_off, "REPLAY-SUMMARY")
            object_off_packet = parse_last(object_off, "PRESENT-PACKET")
            deformation_off_packet = parse_last(deformation_off, "PRESENT-PACKET")
            deformation_on_packet = parse_last(deformation_on, "PRESENT-PACKET")
            particle_on_packet = parse_last(particle_on, "PRESENT-PACKET")
            particle_off_packet = parse_last(particle_off, "PRESENT-PACKET")
            particle_color_off_packet = parse_last(particle_color_off, "PRESENT-PACKET")
            particle_alpha_off_packet = parse_last(particle_alpha_off, "PRESENT-PACKET")
            effect_on_packet = parse_last(effect_on, "PRESENT-PACKET")
            effect_off_packet = parse_last(effect_off, "PRESENT-PACKET")
            uv_scroll_on_packet = parse_last(uv_scroll_on, "PRESENT-PACKET")
            uv_scroll_off_packet = parse_last(uv_scroll_off, "PRESENT-PACKET")
            uv_scroll_on_retained = parse_last(uv_scroll_on, "RETAINED-TASK")
        except RuntimeError as error:
            print(f"check_presentation_matrix: FAIL\n  - arm C: {error}")
            return 1
        for section in ("freeze", "replay"):
            perf = smooth_perf.get(section, {})
            if perf.get("hits", 0) <= 0:
                failures.append(
                    f"arm C: present performance census has no {section} "
                    "samples")
            elif perf.get("meanns", PRESENT_WORK_BUDGET_NS) >= \
                    PRESENT_WORK_BUDGET_NS:
                failures.append(
                    f"arm C: mean {section} work is "
                    f"{perf.get('meanns')} ns, which consumes an entire "
                    "60 Hz presentation interval")
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
        if endpoint_compared == 0:
            failures.append(
                "arm C: alpha-zero endpoint witness found no comparable "
                "backend frames")
        elif endpoint_changed != 0:
            failures.append(
                f"arm C: {endpoint_changed}/{endpoint_compared} alpha-zero "
                "backend frames differ from smoothing-off authored endpoints")
        if delayed_compared == 0:
            failures.append(
                "arm C: delayed endpoint control found no comparable frames")
        elif delayed_changed != 0:
            failures.append(
                f"arm C: {delayed_changed}/{delayed_compared} delayed alpha-zero "
                "frames differ from ordinary authored endpoints")
        for frame in range(SMOOTH_DUMP_FROM,
                           SMOOTH_TICKS * RATE60_PRESENTS_PER_TICK, 2):
            expected = smooth_off_endpoints.get(frame)
            real = smooth_endpoints.get(frame)
            delayed = delayed_endpoints.get(frame)
            if expected is None or real is None or delayed is None:
                failures.append(
                    f"arm C: missing endpoint task token for frame {frame}")
                break
            if real != expected or real[1] != "real":
                failures.append(
                    f"arm C: completed endpoint frame {frame} does not map to "
                    f"the production authored task ({real} vs {expected})")
                break
            if delayed[0] != expected[0] or delayed[1] != "replay":
                failures.append(
                    f"arm C: delayed negative-control frame {frame} did not "
                    f"replay the same authored task ({delayed} vs {expected})")
                break
        for label, sched in (("production smoothing-off", smooth_off_summary),
                             ("production interpolated", smooth_summary)):
            expected_replays = 0
            if sched.get("replayendpoints", -1) != expected_replays:
                failures.append(
                    f"arm C: {label} replayendpoints="
                    f"{sched.get('replayendpoints')}, expected 0")
        if delayed_summary.get("realendpoints", -1) != 0 or \
                delayed_summary.get("replayendpoints", 0) <= 0 or \
                delayed_summary.get("interp", -1) != 0:
            failures.append(
                "arm C: delayed endpoint control was not isolated to endpoint "
                "replays only")
        for label, retained, expected_poison in (
                ("production midpoint", smooth_retained,
                 smooth_replay.get("walks", 0)),
                ("delayed endpoint", delayed_retained,
                 delayed_summary.get("replayendpoints", 0))):
            if (retained.get("captures", 0) <= 0 or
                    retained.get("failures", -1) != 0 or
                    retained.get("rejects", -1) != 0 or
                    retained.get("arenaResolve", 0) <= 0 or
                    retained.get("externalResolve", 0) <= 0 or
                    retained.get("livePoison", -1) != expected_poison):
                failures.append(
                    f"arm C: {label} retained-task poison contract failed: "
                    f"{retained}, expected livePoison={expected_poison}")
        for key in ("forcedmatrixrewrite", "forcedvertexrewrite",
                    "dependencychecks"):
            if delayed_packet.get(key, 0) <= 0:
                failures.append(
                    f"arm C: delayed retained-dependency control {key}="
                    f"{delayed_packet.get(key)}")
        if (delayed_packet.get("unsafestalefallback", -1) != 0 or
                delayed_packet.get("dependencymismatch", -1) != 0 or
                delayed_packet.get("dependencyexpected", 0) == 0 or
                delayed_packet.get("dependencyexpected") !=
                delayed_packet.get("dependencyactual")):
            failures.append(
                "arm C: delayed retained-dependency mechanism did not hold its "
                "forced rewritten matrix/vertex bytes exactly")
        webgpu_walks = webgpu_mechanism_replay.get("walks", 0)
        if (webgpu_mechanism_summary.get("interp", 0) <= 0 or
                webgpu_walks != webgpu_mechanism_summary.get("interp")):
            failures.append(
                "arm C: paced production WebGPU completed no accountable "
                "interpolated replay walks")
        if (webgpu_mechanism_summary.get("replayendpoints", -1) != 0 or
                webgpu_mechanism_summary.get("realendpoints", 0) <= 0):
            failures.append(
                "arm C: paced WebGPU mechanism did not use real authored "
                "endpoints plus production-only midpoints")
        if (webgpu_mechanism_retained.get("captures", 0) <= 0 or
                webgpu_mechanism_retained.get("failures", -1) != 0 or
                webgpu_mechanism_retained.get("rejects", -1) != 0 or
                webgpu_mechanism_retained.get("arenaResolve", 0) <= 0 or
                webgpu_mechanism_retained.get("externalResolve", 0) <= 0 or
                webgpu_mechanism_retained.get("livePoison", -1) !=
                    webgpu_walks):
            failures.append(
                "arm C: paced WebGPU did not complete every admitted replay "
                "from poisoned-live private task ownership")
        webgpu_mechanism_skips = webgpu_mechanism_backpressure.get("skips", -1)
        if webgpu_mechanism_skips != (
                webgpu_mechanism_backpressure.get("endpointSkips", -2) +
                webgpu_mechanism_backpressure.get("replaySkips", -2)):
            failures.append(
                "arm C: paced WebGPU admission skips do not reconcile by "
                "endpoint and replay class")
        if (webgpu_mechanism_backpressure.get("submitted", -1) !=
                webgpu_mechanism_summary.get("realendpoints", 0) +
                webgpu_mechanism_summary.get("interp", 0)):
            failures.append(
                "arm C: paced WebGPU submissions do not match completed real "
                "and interpolated images")
        for key in ("failures", "abandoned", "inflight", "runtimewaits",
                    "runtimewaitns"):
            if webgpu_mechanism_backpressure.get(key, 0) != 0:
                failures.append(
                    f"arm C: paced WebGPU {key}="
                    f"{webgpu_mechanism_backpressure.get(key)}, expected 0")
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
        for key in ("deformreg", "deformphasehold", "deformpeak",
                    "futurecaptures", "deformhit", "deformoverride",
                    "colorhit", "coloroverride"):
            if deformation_on_packet.get(key, 0) <= 0:
                failures.append(
                    f"arm C: deformation witness {key}="
                    f"{deformation_on_packet.get(key)}; forward authored "
                    "deformation interpolation is not live")
        if deformation_on_packet.get("futurefailures", -1) != 0:
            failures.append(
                "arm C: future deformation census reported failures")
        for key in ("projectedshadowvertexreg",
                    "projectedshadowdeformhit",
                    "projectedshadowdeformoverride"):
            if deformation_on_packet.get(key, 0) <= 0:
                failures.append(
                    f"arm C: projected-shadow smoothing {key}="
                    f"{deformation_on_packet.get(key)}; the generation-keyed "
                    "rigid decal replay path is not live")
        if deformation_on_packet.get("deformcollision", -1) != 0:
            failures.append(
                "arm C: projected/model deformation ownership collided "
                f"(deformcollision="
                f"{deformation_on_packet.get('deformcollision')})")
        if deformation_off_packet.get("deformoverride", -1) != 0:
            failures.append(
                "arm C: deformation-off control unexpectedly changed retained "
                f"batches (deformoverride="
                f"{deformation_off_packet.get('deformoverride')})")
        if (deformation_off_packet.get("projectedshadowdeformhit", -1) != 0 or
                deformation_off_packet.get(
                    "projectedshadowdeformoverride", -1) != 0):
            failures.append(
                "arm C: deformation-off control left projected-shadow "
                "smoothing enabled "
                f"(hits={deformation_off_packet.get('projectedshadowdeformhit')}, "
                "overrides="
                f"{deformation_off_packet.get('projectedshadowdeformoverride')})")
        if (delayed_packet.get("endpointchecks", 0) <= 0 or
                delayed_packet.get("endpointmismatch", -1) != 0 or
                delayed_packet.get("endpointexpected", 0) == 0 or
                delayed_packet.get("endpointexpected") !=
                delayed_packet.get("endpointactual")):
            failures.append(
                "arm C: delayed alpha-zero control did not reproduce retained "
                "task-endpoint vertex bytes exactly")
        if deformation_compared == 0:
            failures.append(
                "arm C: deformation pixel witness found no comparable "
                "intermediate frames")
        elif deformation_changed == 0:
            failures.append(
                "arm C: forward deformation interpolation is pixel-identical "
                "to its exact-task hold control")
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
        if particle_on_packet.get("particlevertexreg", 0) <= 0:
            failures.append(
                "arm C: battle point-trail witness captured no retained "
                "particle vertices")
        for key in ("particledeformhit", "particledeformoverride",
                    "particlecolorhit", "particlecoloroverride"):
            if particle_on_packet.get(key, 0) <= 0:
                failures.append(
                    f"arm C: particle witness {key}="
                    f"{particle_on_packet.get(key)}; forward point-trail "
                    "interpolation is not live")
        if (particle_on_packet.get("futurecaptures", 0) <= 0 or
                particle_on_packet.get("futurefailures", -1) != 0):
            failures.append(
                "arm C: particle witness did not publish its forward tasks")
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
                "arm C: battle point-trail geometry is pixel-identical to its "
                "exact-task hold control")
        if particle_color_compared == 0:
            failures.append(
                "arm C: particle color witness found no comparable "
                "intermediate frames")
        elif particle_color_changed == 0:
            failures.append(
                "arm C: point-trail RGBA interpolation is pixel-identical to "
                "its color-hold control")
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
        if effect_on_packet.get("effectreg", 0) <= 0:
            failures.append(
                "arm C: shield effect witness captured no retained semantic "
                "matrix recipes")
        if effect_on_packet.get("effectphasehold", 0) <= 0:
            failures.append(
                "arm C: forced effects recorded no phase-safe authored holds")
        for key in ("effecthit", "effectoverride"):
            if effect_on_packet.get(key, 0) <= 0:
                failures.append(
                    "arm C: forward authored effect recipes did not reach "
                    f"replay ({key}={effect_on_packet.get(key)})")
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
                "arm C: forced-shield forward recipes are pixel-identical to "
                "the authored-matrix hold control")
        notes.append(
            f"arm C: with smoothing on, {moved_on}/{moved_on + still_on} "
            f"production interpolated backend frames differ from both neighbours; with "
            f"MDKR_PRESENT_SMOOTHING=off, {moved_off}/{moved_off + still_off} "
            "do (every no-swap backend dump retains the authored image); "
            f"object interpolation changes {object_changed}/{object_compared} "
            "intermediate backend frames against the camera-identical control; "
            f"all {endpoint_compared} alpha-zero frames reproduce the authored "
            "endpoint")
        notes.append(
            f"arm C: {deformation_on_packet.get('deformoverride', 0)} retained "
            "deformation batches blend true forward task bytes; enabled/control "
            f"differ on {deformation_changed}/{deformation_compared} backend frames and "
            f"{delayed_packet.get('endpointchecks', 0)} delayed alpha-zero "
            "G_VTX semantic controls match")
        notes.append(
            f"arm C: battle point trails use adjacent task-authored vertices "
            f"and differ from the hold control on {particle_changed}/"
            f"{particle_compared} frames; "
            "authoritative state is byte-identical")
        notes.append(
            f"arm C: retained point-trail RGBA differs from its exact-hold "
            f"control on {particle_color_changed}/{particle_color_compared} frames")
        notes.append(
            f"arm C: particle primitive alpha applies "
            f"{particle_on_packet.get('particleprimalphaoverride', 0)} changed "
            f"draws from "
            f"{particle_on_packet.get('particleprimalphahit', 0)} compatible "
            f"pairs and independently changes {particle_alpha_changed}/"
            f"{particle_alpha_compared} intermediate backend frames; point "
            "trails remain single-scaled and the alpha-only control is "
            "authoritatively byte-identical")
        uv_on_rows = sim_hash_rows(uv_scroll_on)
        uv_off_rows = sim_hash_rows(uv_scroll_off)
        if (len(uv_on_rows) != UV_SCROLL_TICKS or
                len(uv_off_rows) != UV_SCROLL_TICKS):
            failures.append(
                "arm C: UV-scroll witness did not complete exactly "
                f"{UV_SCROLL_TICKS} authoritative ticks "
                f"(on={len(uv_on_rows)}, off={len(uv_off_rows)})")
        elif uv_on_rows != uv_off_rows:
            index = first_difference(uv_on_rows, uv_off_rows)
            failures.append(
                "arm C: UV-scroll interpolation changed authoritative state "
                f"at tick {index}")
        if event_hash_rows(uv_scroll_on) != event_hash_rows(uv_scroll_off):
            failures.append(
                "arm C: UV-scroll interpolation changed the ordered "
                "gameplay-event stream")
        if input_hash_rows(uv_scroll_on) != input_hash_rows(uv_scroll_off):
            failures.append(
                "arm C: UV-scroll interpolation changed consumed input")
        if uv_scroll_on_packet.get("uvscrollreg", 0) <= 0:
            failures.append(
                "arm C: the UV-scroll route registered no scrolling triangle "
                "batches at all - the census found no {T,T+1} endpoint pair")
        if uv_scroll_on_packet.get("uvscrollcollision") != 0:
            failures.append(
                "arm C: UV-scroll census found ambiguous batch keys "
                f"(uvscrollcollision="
                f"{uv_scroll_on_packet.get('uvscrollcollision')})")
        for key in ("uvscrollconfirm", "uvscrollhit", "uvscrolloverride"):
            if uv_scroll_on_packet.get(key, 0) <= 0:
                failures.append(
                    "arm C: confirmed UV-scroll displacements did not reach "
                    f"replay ({key}={uv_scroll_on_packet.get(key)})")
        if (uv_scroll_off_packet.get("uvscrollhit", -1) != 0 or
                uv_scroll_off_packet.get("uvscrolloverride", -1) != 0):
            failures.append(
                "arm C: UV-scroll positive control left the scroll "
                "interpolation path enabled")
        if uv_mid_compared == 0:
            failures.append(
                "arm C: UV-scroll witness found no comparable interpolated "
                "midpoint frames")
        elif uv_mid_changed == 0:
            failures.append(
                "arm C: every interpolated midpoint is byte-identical with "
                "UV-scroll interpolation enabled and disabled - the scrolling "
                "phase is still held at its authored endpoint value")
        if uv_end_compared == 0:
            failures.append(
                "arm C: UV-scroll witness found no comparable authored "
                "endpoint frames")
        elif uv_end_changed != 0:
            failures.append(
                f"arm C: {uv_end_changed}/{uv_end_compared} authored endpoint "
                "frames moved when UV-scroll interpolation was enabled; only "
                "midpoints may differ")
        if uv_moved == 0:
            failures.append(
                "arm C: none of the UV-scroll route's "
                f"{uv_moved + uv_still} midpoints differed from both of its "
                "neighbours")
        notes.append(
            f"arm C: forced shield/magnet-class recipes hold the display "
            f"list's authored matrix on {effect_on_packet.get('effectphasehold', 0)} "
            f"phase gaps and differs on {effect_changed}/{effect_compared} frames "
            "and remain collision-free")
        notes.append(
            f"arm C: authored UV scroll confirms "
            f"{uv_scroll_on_packet.get('uvscrollconfirm', 0)} adjacent "
            f"{{T,T+1}} displacements from a peak of "
            f"{uv_scroll_on_packet.get('uvscrollpeak', 0)} live scroll sites "
            f"({uv_scroll_on_packet.get('uvscrollbytespeak', 0)} bytes of "
            "retained displacement, held outside the arena copy budget of "
            f"{uv_scroll_on_retained.get('arenaBudget', 0)}), holds "
            f"{uv_scroll_on_packet.get('uvscrollhold', 0)} unconfirmed ticks, "
            f"and moves {uv_mid_changed}/{uv_mid_compared} interpolated "
            f"midpoints while all {uv_end_compared} authored endpoints stay "
            "byte-identical")
        notes.append(
            "arm C: retained-task publication and poison-hardened replay "
            f"average {smooth_perf.get('freeze', {}).get('meanns', 0) / 1e6:.3f} "
            "ms and "
            f"{smooth_perf.get('replay', {}).get('meanns', 0) / 1e6:.3f} ms; "
            "each remains below one 60 Hz presentation interval")
        notes.append(
            "arm C WebGPU: paced production completed "
            f"{webgpu_mechanism_summary.get('interp', 0)} immutable midpoints "
            "from a fully poisoned live arena with "
            f"{webgpu_mechanism_backpressure.get('replaySkips', 0)} replay "
            "admission holds and zero runtime waits")

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
