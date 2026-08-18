#!/usr/bin/env python3
"""Prove the [SMOOTH-VERDICT] per-surface-class census is real, not vacuous.

Task 2 of the presentation-safety plan adds a census that translates existing
replay clause outcomes (uv_scroll_hold_*, MDKR_REPLAY_HOLD_*) into one
[SMOOTH-VERDICT] row per graded MdkrSurfaceClass. This gate proves three
things about that translation on one Remastered scripted race:

* it actually fires: at least one row for WATER_WAVE, OBJECT_ROOT and
  WORLD_SCROLL;
* it is not vacuous: every row's blend+snap equals its own total, and
  WATER_WAVE graded at least one draw;
* it pins the wave surfaces' ownership. Task 7 gave wave geometry a real
  presentation owner (``game/src/waves.c``: one registered transform per
  visible tile, topology-keyed on the grid LOD variant), so WATER_WAVE must
  now BLEND at least once and must file NO_OWNER exactly zero times -- a
  minority of unowned wave draws would hide behind ``top_reason`` otherwise.
  ``--no-expect-water-owned`` restores the pre-Task-7 assertion (all-snap,
  top_reason=NO_OWNER) and is how the red state is reproduced on an older
  tree; the default is now the owned one, so this gate fails loudly the day
  ownership regresses instead of quietly passing.

Route: Jungle Falls (level 29), MDKR_LOAD_TRACK-retargeted so the race loads
without menu navigation. Jungle Falls is the tree's own established
UV-scroll/wave witness (see check_presentation_matrix.py's
``UV_SCROLL_TRACK``): its waterfall sheet is an ordinary BHV_TEXTURE_SCROLL
level-model batch (WORLD_SCROLL) and its ping-pong water surface is the
wave-driven, still-unowned content (WATER_WAVE) in the same view. Level 5
(Ancient Lake, check_track_sweep's first track) has water but no distinct
scroller in view during an early lap and was measured to produce zero
WORLD_SCROLL rows; Jungle Falls produces all three classes reliably.

Smoothing is armed with the public ``MDKR_PRESENT_RATE=60`` /
``MDKR_PRESENT_SMOOTHING=interpolate`` pair (present rate above the tick
rate, motion smoothing on) — the same production arm condition
``present_sched_replay_armed()`` requires. The census counters accumulate
unconditionally; only the stderr rows are gated, and only on
``MDKR_SMOOTH_VERDICT=1`` on top of the usual ``MDKR_PRESENT_SCHED_TRACE=1``
that gates the whole trace dump.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, fatal_re, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "race_full_3lap_tt.txt"
PAN_DEMOTE_SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
TRACK = "29"                # Jungle Falls: waterfall scroll + ping-pong water
# Terminate on authoritative ticks, not host presentation calls. The old 20,000
# frame budget completed after a host-speed-dependent number of simulation
# ticks, so Darwin background scheduling changed the topology census on the
# identical binary. 10,000 fixed ticks retain the old budget's intended 2:1
# 60 Hz presentation / 30 Hz authority window and reliably reach the ordinary
# waterfall scroller as well as the wave surface.
TICKS = 10000
# The pan-demote arm uses the presentation matrix's measured UV-scroll window:
# Jungle Falls' waterfall has one confirmed scroll site per authored tick from
# tick 3000 through 3230. It then token-gates a 45 degree presentation-yaw
# input, so the production 22.5 degree threshold and the class filter are
# exercised deterministically instead of waiting for a real hard pan and a
# confirmed scroller replay to coincide by chance.
PAN_DEMOTE_TICKS = 3230
PAN_DEMOTE_FORCED_YAW_DEG = "45.0"
# The shipped production constant (MDKR_PAN_DEMOTE_YAW_DEG_PER_TICK in
# gfx_pc_dkr.c), duplicated here ONLY as the value this gate expects
# [PAN-DEMOTE]'s report to equal -- the report itself is read from the
# binary's own stderr, not assumed, so a source change to the compiled
# constant that this literal is not also updated to match fails the
# production sub-arm below rather than passing silently.
EXPECTED_PRODUCTION_THRESHOLD_DEG = 22.5
FATAL_RE = fatal_re(r"\[FX BUG\]", "Assertion", "Validation Error")

# The row grew six reason counters (uvhold, tickmismatch, generation,
# discontinuity, camcut, depmiss) between topomismatch= and heldpermille=
# when the 2026-08-17 interpolation stack widened the census. The old exact
# adjacency match then found ZERO rows on a binary that was printing them on
# every run ("no [SMOOTH-VERDICT] rows at all", 2026-08-18) — so the fields
# this gate does not consume are bridged instead of enumerated, and the next
# added counter cannot silently blind the gate again.
ROW_RE = re.compile(
    r"\[SMOOTH-VERDICT\] class=(\S+) blend=(\d+) snap=(\d+) "
    r"top_reason=(\S+) pandemoted=(\d+) noowner=(\d+) "
    r"topomismatch=(\d+) (?:\w+=\d+ )*heldpermille=(\d+)"
)
PAN_THRESHOLD_RE = re.compile(r"\[PAN-DEMOTE\] armed threshold=([0-9.]+)")
PAN_FORCED_YAW_RE = re.compile(r"\[PAN-DEMOTE-TEST\] forced_yaw=([0-9.]+)")
# Task 7's topology-key pair comparison, from [PRESENT-PACKET]. The CHECK
# count is what makes the guard observable. A continuously rebuilt route
# produces 282 checks / 0 mismatches, while skipped draws can retain an older
# list across a real grid-LOD boundary and correctly produce a bounded number
# of TOPOLOGY_MISMATCH snaps. The forced-flip arm below remains the proof that
# an incompatible pair cannot blend at all.
TOPOLOGY_RE = re.compile(r"topocheck=(\d+) topomismatch=(\d+)")
# Task 5: VRR-honest alpha quantization. This fixture drives synthetic
# (non-realtime) pacing via --headless-ticks, which
# platform_present_display_quantum_units() declines to quantize onto a grid
# unconditionally (platform_sdl_min.c:4083-4102, the very first branch) --
# long before the new variance check ever runs. So this gate only proves the
# line fires and reports what synthetic pacing has always done: mode=free,
# units=0. It is not a witness for the variance-driven decline itself; that
# needs a real, jittery display (tests/check_pacing_quality.py's realtime
# arm).
ALPHA_QUANTUM_RE = re.compile(
    r"\[ALPHA-QUANTUM\] units=(\d+) variance_ppm=(\d+) mode=(grid|free)"
)

REQUIRED_CLASSES = ("WATER_WAVE", "OBJECT_ROOT", "WORLD_SCROLL")


def environment(save_dir: Path, pan_demote: bool,
                 topology_flip: bool = False,
                 authorize_forced_yaw: bool = True) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=TRACK,
        # Smoothing is armed the same way a player reaches it: a present policy
        # above the tick rate plus the public Motion smoothing = Interpolated
        # setting. Pin one host field per headless frame as well. Without this,
        # the real clock decides how many authored ticks fit inside 20,000 fast
        # host calls: the same binary measured 282 topology checks standalone
        # and 41,248 in Darwin's background band, turning scheduling priority
        # into a different wave-ownership verdict.
        MDKR_SYNTH_FIELDS="1",
        MDKR_PRESENT_RATE="60",
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_SMOOTH_VERDICT="1",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
    )
    if pan_demote:
        # Task 4's optional pan-rate demotion (default OFF). The forced yaw is
        # a token-gated presentation-only negative control; the route and
        # frame budget are the presentation matrix's measured scroll window.
        env["MDKR_SMOOTH_PAN_DEMOTE"] = "1"
        env["MDKR_TEST_PAN_DEMOTE_FORCE_YAW_DEG_PER_TICK"] = (
            PAN_DEMOTE_FORCED_YAW_DEG
        )
        if authorize_forced_yaw:
            env["MDKR_INTERNAL_TEST_TOKEN"] = "mdkr64-presentation-replay-v1"
    if topology_flip:
        # Task 7's negative control: fold the per-tick vertex-buffer flip into
        # the wave topology key, which declares every authored pair a topology
        # change. See waves.c waves_owner_topology_flip_seam.
        env["MDKR_TEST_WAVE_TOPOLOGY_FLIP"] = "1"
        env["MDKR_INTERNAL_TEST_TOKEN"] = "mdkr64-presentation-replay-v1"
    return env


def run(binary: Path, rom: Path, work: Path, timeout: int, verbose: bool,
        pan_demote: bool, ticks: int,
        topology_flip: bool = False,
        authorize_forced_yaw: bool = True) -> str:
    import subprocess

    save_dir = work / "save"
    save_dir.mkdir(parents=True)
    command = [
        str(binary),
        "--headless-ticks", str(ticks),
        "--input-script", str(PAN_DEMOTE_SCRIPT if pan_demote else SCRIPT),
        "--rom", str(rom),
    ]
    if verbose:
        print(f"$ {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=work,
            env=environment(save_dir, pan_demote, topology_flip,
                            authorize_forced_yaw),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"timed out\n{(exc.stdout or '')[-4000:]}"
        ) from exc

    output = proc.stdout or ""
    fatal = FATAL_RE.search(output)
    if proc.returncode != 0 or fatal is not None:
        raise RuntimeError(
            f"exit={proc.returncode}, "
            f"fatal={fatal.group(0) if fatal else 'none'}\n{output[-5000:]}"
        )
    return output


def parse_rows(
        output: str,
        failures: list[str]) -> dict[str, tuple[int, int, str, int, int, int, int]]:
    rows: dict[str, tuple[int, int, str, int, int, int, int]] = {}
    for match in ROW_RE.finditer(output):
        (cls, blend_s, snap_s, top_reason, pandemoted_s, noowner_s,
         topomismatch_s, held_permille_s) = match.groups()
        blend, snap, pandemoted = int(blend_s), int(snap_s), int(pandemoted_s)
        noowner, topomismatch = int(noowner_s), int(topomismatch_s)
        held_permille = int(held_permille_s)
        # Multiple rows for the same class would mean [SMOOTH-VERDICT] fired
        # more than once per process, which the flush contract (once, at
        # present_sched_trace_summary) forbids.
        if cls in rows:
            failures.append(f"class={cls} printed more than one row")
            continue
        expected_held_permille = snap * 1000 // (blend + snap)
        if held_permille != expected_held_permille:
            failures.append(
                f"class={cls}: heldpermille={held_permille}, want exact "
                f"snap/(blend+snap)={expected_held_permille}"
            )
        rows[cls] = (blend, snap, top_reason, pandemoted, noowner,
                     topomismatch, held_permille)
    return rows


#
# --sky-midpoint (Task 8): the skydome's midpoint-sensitivity witness.
#
# check_smooth_verdict.py's other arms all read the [SMOOTH-VERDICT] census,
# which the skydome does not (and should not: residual obligation 0 notes it
# has no spawned-Object identity to publish one from -- see
# docs/architecture/presentation-interpolation.md). This arm is pixel-based
# instead, on the same two-run-diff method as check_wave_midpoint_envelope.py:
# production against MDKR_TEST_SKYDOME_CAMERA_LOCK_DISABLE=1, the negative
# control that makes gfx_pc_dkr.c's G_MTX replay branch recompose a
# camera_locked entry exactly like any other -- captured tick-T translation
# held frozen against an interpolated view-projection, i.e. this fix's own
# defect reproduced on demand. The pixels that differ between the two runs
# ARE the fix's footprint; measuring "does the sky move at all" on a single
# run cannot distinguish this fix from the camera-ROTATION panning that
# already worked before it (mtx_cam_push's matrices were already recomposed
# against the interpolated camera on vp_overridden -- see gfx_pc_dkr.c:6559
# and this file's own WATER_WAVE note above). Only the frozen-translation
# defect this task closes is invisible to that broader check.
#
# Route: the title screen's own scripted camera fly-around (levelId 23,
# reached by simply booting -- the same "title" fixture
# check_overlay_pause_cutscene.py uses), which pans and translates hard
# enough to move the dome. MDKR_PRESENT_RATE=120 over the 30 Hz tick gives 4
# presents per tick; measured empirically that the exact-tick present lands
# at (index - SKY_DUMP_FROM) % 4 == 2 in this fixture's cadence, not 0 --
# read from the dump, not assumed, by the endpoint-identity check below.
SKY_SCRIPT = ROOT / "tests" / "input_scripts" / "adventure_hub_drive.txt"
SKY_TICKS = 1090
SKY_DUMP_FROM = 690
# Measured: 429 of 3670 presents in this window differ between the two runs,
# the dome's own footprint, concentrated in the top ~24% of the frame (bbox
# y max 229 of 960) -- consistent with "sky band, top 20% of frame" once HUD
# rows and window chrome are accounted for. 20 keeps ample margin against
# host-scheduling changing which presents land as an interpolated replay.
SKY_MIN_ACTED = 20
SKY_WINDOW = "640x480"
# Top 20% of the (possibly HiDPI-doubled) frame, then trimmed another 4
# rows off the top: this fixture has no HUD, but the title logo/press-start
# prompt can start as low as the sky band's own top edge on some window
# scales, and trimming a slim margin costs nothing the dome needs.
SKY_BAND_FRACTION = 0.20
SKY_BAND_TRIM_ROWS = 4


def _read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    fields: list[bytes] = []
    idx = 0
    while len(fields) < 4:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx] != 0x0A:
                idx += 1
            continue
        start = idx
        while not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(data[start:idx])
    idx += 1
    width, height = int(fields[1]), int(fields[2])
    return width, height, data[idx:idx + width * height * 3]


def _sky_band(width: int, height: int, pixels: bytes) -> bytes:
    band_h = max(1, int(height * SKY_BAND_FRACTION) - SKY_BAND_TRIM_ROWS)
    return pixels[:band_h * width * 3]


def _run_sky_arm(binary: Path, rom: Path, work: Path, timeout: int,
                  verbose: bool, disable_lock: bool) -> dict[int, bytes]:
    import subprocess

    frames_dir = work / "frames"
    frames_dir.mkdir(parents=True)
    (work / "save").mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_APP_AUTOPLAY="1",
        MDKR_APP_AUTOPLAY_TICKS=str(SKY_TICKS),
        MDKR_APP_AUTOPLAY_INPUT_SCRIPT=str(SKY_SCRIPT),
        MDKR_APP_AUTOPLAY_DUMP_FRAMES=str(frames_dir),
        MDKR_APP_PREFS_DIR=str(work / "prefs"),
        MDKR_APP_SMOKE_WINDOW_SIZE=SKY_WINDOW,
        MDKR_AUDIO="0",
        MDKR_DUMP_FROM=str(SKY_DUMP_FROM),
        MDKR_DUMP_EVERY="1",
        MDKR_PRESENT_RATE="120",
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_ROM=str(rom),
        MDKR_SAVE_DIR=str(work / "save"),
        MDKR_NO_CRASH_HANDLER="1",
        MDKR_VIDEO_CONFIG_PATH=str(work / "video.ini"),
        MDKR64_HIDDEN="1",
    )
    (work / "prefs").mkdir(parents=True, exist_ok=True)
    if disable_lock:
        env["MDKR_TEST_SKYDOME_CAMERA_LOCK_DISABLE"] = "1"
        env["MDKR_INTERNAL_TEST_TOKEN"] = "mdkr64-presentation-replay-v1"
    if verbose:
        print(f"$ [sky-midpoint disable_lock={disable_lock}] {binary}",
              flush=True)
    try:
        proc = subprocess.run(
            [str(binary)], cwd=str(work), env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"sky-midpoint timed out\n{(exc.stdout or '')[-4000:]}"
        ) from exc
    if proc.returncode != 0:
        raise RuntimeError(
            f"sky-midpoint: exit={proc.returncode}\n"
            f"{(proc.stdout or '')[-4000:]}")

    out: dict[int, bytes] = {}
    for path in frames_dir.glob("frame_*.ppm"):
        match = re.search(r"frame_(\d+)\.ppm$", path.name)
        if not match:
            continue
        width, height, pixels = _read_ppm(path)
        out[int(match.group(1))] = _sky_band(width, height, pixels)
    return out


def run_sky_midpoint_arm(binary: Path, rom: Path, timeout: int,
                          verbose: bool) -> int:
    failures: list[str] = []
    try:
        with tempfile.TemporaryDirectory(
                prefix="mdkr_sky_midpoint_prod_") as prod_dir, \
             tempfile.TemporaryDirectory(
                prefix="mdkr_sky_midpoint_ctrl_") as ctrl_dir:
            production = _run_sky_arm(binary, rom, Path(prod_dir), timeout,
                                      verbose, disable_lock=False)
            control = _run_sky_arm(binary, rom, Path(ctrl_dir), timeout,
                                   verbose, disable_lock=True)
    except (OSError, RuntimeError) as exc:
        print(f"check_smooth_verdict --sky-midpoint: FAIL — {exc}",
              file=sys.stderr)
        return 1

    shared = sorted(set(production) & set(control))
    if len(shared) < 100:
        failures.append(
            f"only {len(shared)} presents captured in both runs; nothing "
            "to judge")
        print("check_smooth_verdict --sky-midpoint: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    # The exact-tick present's phase (index - SKY_DUMP_FROM) % 4 is a
    # property of when MDKR_DUMP_FROM happens to land inside the present
    # cadence, not a constant this file can assume -- so it is FOUND from the
    # data: the phase with the fewest disagreements between the two runs is
    # the authored endpoint, because a snap and a blend agree exactly at
    # their shared alpha-0/alpha-1 endpoint (the same identity the wave
    # envelope gate's claim 1 checks), while every other phase is a genuine
    # interpolated replay the fix can (and, per claim below, does) touch.
    phase_diffs = [0, 0, 0, 0]
    phase_totals = [0, 0, 0, 0]
    for index in shared:
        phase = (index - SKY_DUMP_FROM) % 4
        phase_totals[phase] += 1
        if production[index] != control[index]:
            phase_diffs[phase] += 1
    endpoint_phase = min(range(4), key=lambda p: phase_diffs[p])
    endpoint_mismatches = phase_diffs[endpoint_phase]
    endpoint_checks = phase_totals[endpoint_phase]
    acted = sum(phase_diffs) - endpoint_mismatches

    if endpoint_checks < 20:
        failures.append(
            f"endpoint phase {endpoint_phase} only had {endpoint_checks} "
            "presents -- not enough to trust as the authored-tick phase")
    if endpoint_mismatches != 0:
        failures.append(
            f"endpoint phase {endpoint_phase}: {endpoint_mismatches} of "
            f"{endpoint_checks} presents differ between production and the "
            "MDKR_TEST_SKYDOME_CAMERA_LOCK_DISABLE=1 control -- an authored "
            "tick frame must be identical regardless of the fix, since "
            "vp_overridden is never set at alpha 0/1. If this phase is "
            "wrong (see the comment above), the real endpoint phase has a "
            "worse mismatch count than this one and something else is "
            "moving too")
    if acted < SKY_MIN_ACTED:
        failures.append(
            f"acted={acted} of {len(shared) - endpoint_checks} interpolated "
            f"presents (need {SKY_MIN_ACTED}) -- the fix's own footprint "
            "barely or never reached the screen. Either the camera-lock "
            "substitution stopped firing, or the title route no longer "
            "pans/translates hard enough for this fixture to witness it")

    if failures:
        print("check_smooth_verdict --sky-midpoint: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(f"phase_diffs={phase_diffs} phase_totals={phase_totals}",
              file=sys.stderr)
        return 1

    print(
        "check_smooth_verdict --sky-midpoint: PASS — "
        f"acted={acted} of {len(shared) - endpoint_checks} interpolated "
        f"presents, endpoint_phase={endpoint_phase} "
        f"endpoint_checks={endpoint_checks} mismatches=0")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument(
        "--expect-water-owned", action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "assert wave geometry carries a real presentation owner "
            "(Task 7): WATER_WAVE blends and never files NO_OWNER. "
            "--no-expect-water-owned restores the pre-Task-7 pin "
            "(all-snap, top_reason=NO_OWNER) for reproducing the red state "
            "on an older tree"
        ),
    )
    parser.add_argument(
        "--topology-negative-control", action="store_true",
        help=(
            "Task 7: arm MDKR_TEST_WAVE_TOPOLOGY_FLIP=1 so every authored "
            "wave pair reads as a topology change, and prove the guard "
            "actually refuses -- WATER_WAVE all-snap with "
            "top_reason=TOPOLOGY_MISMATCH. Without this the route's own "
            "zero mismatches cannot distinguish a working guard from a "
            "dead one."
        ),
    )
    parser.add_argument(
        "--sky-midpoint", action="store_true",
        help=(
            "Task 8: the skydome's midpoint-sensitivity witness. Runs the "
            "title screen's fly-around twice (production vs "
            "MDKR_TEST_SKYDOME_CAMERA_LOCK_DISABLE=1) and diffs the sky "
            "band. Standalone -- does not read [SMOOTH-VERDICT] and "
            "ignores every other flag."
        ),
    )
    parser.add_argument(
        "--pan-demote", action="store_true",
        help=(
            "Task 4: arm MDKR_SMOOTH_PAN_DEMOTE=1 over the same Jungle Falls "
            "route with a token-gated forced yaw and prove WATER_WAVE is "
            "demoted while OBJECT_ROOT still blends (objects are never in "
            "the demotable set). Any confirmed WORLD_SCROLL draw must also "
            "demote, but a run containing only UV_HOLD scroll rows is not "
            "failed. With this flag omitted, every observed class must show "
            "pandemoted=0 -- the default-off proof."
        ),
    )
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()

    if args.sky_midpoint:
        return run_sky_midpoint_arm(binary, rom, args.timeout, args.verbose)

    failures: list[str] = []

    try:
        with tempfile.TemporaryDirectory(prefix="mdkr_smooth_verdict_") as temp:
            ticks = PAN_DEMOTE_TICKS if args.pan_demote else TICKS
            output = run(binary, rom, Path(temp), args.timeout, args.verbose,
                         args.pan_demote, ticks,
                         args.topology_negative_control)
    except (OSError, RuntimeError) as exc:
        print(f"check_smooth_verdict: FAIL — {exc}", file=sys.stderr)
        return 1

    rows = parse_rows(output, failures)

    if not rows:
        failures.append("no [SMOOTH-VERDICT] rows at all")

    # Task 7 non-vacuity: the topology guard must have been ASKED.
    topology_match = None
    for topology_match in TOPOLOGY_RE.finditer(output):
        pass
    if topology_match is None:
        failures.append(
            "[PRESENT-PACKET] carries no topocheck/topomismatch fields — "
            "Task 7's topology-key comparison has no witness at all")
    else:
        topo_checks = int(topology_match.group(1))
        topo_mismatches = int(topology_match.group(2))
        if args.expect_water_owned and topo_checks <= 0:
            failures.append(
                "topocheck=0 — no topology-keyed owner ever asked the "
                "published pair whether its two ticks describe the same "
                "mesh. Either wave ownership stopped stamping "
                "topology_keyed, or the guard is no longer reached; every "
                "mismatch assertion below would be vacuous")
        if args.topology_negative_control:
            if topo_mismatches <= 0:
                failures.append(
                    f"topomismatch={topo_mismatches} of {topo_checks} "
                    "checks with MDKR_TEST_WAVE_TOPOLOGY_FLIP=1 — the seam "
                    "makes every authored pair a topology change, so a "
                    "guard that compares keys at all cannot agree even "
                    "once. A constant topology key reads exactly like this")
        else:
            wave = rows.get("WATER_WAVE")
            if topo_mismatches > topo_checks:
                failures.append(
                    f"topomismatch={topo_mismatches} exceeds "
                    f"topocheck={topo_checks}")
            if wave is not None:
                wave_snap = wave[1]
                wave_topology_mismatches = wave[5]
                if wave_topology_mismatches > wave_snap:
                    failures.append(
                        f"class=WATER_WAVE: topomismatch="
                        f"{wave_topology_mismatches} exceeds snap="
                        f"{wave_snap} — an incompatible pair was not "
                        "accounted as a refusal")
                if (topo_mismatches == 0) != (
                        wave_topology_mismatches == 0):
                    failures.append(
                        f"topology census disagrees: packet mismatches="
                        f"{topo_mismatches}, WATER_WAVE mismatches="
                        f"{wave_topology_mismatches}")

    # Task 5: the [ALPHA-QUANTUM] line must fire at the same flush window as
    # [SMOOTH-VERDICT], and this fixture's synthetic (--headless-ticks)
    # pacing must report the harness's known-today value: mode=free, units=0
    # (see ALPHA_QUANTUM_RE's comment above for why this is not yet a witness
    # for the variance threshold itself).
    alpha_quantum_match = ALPHA_QUANTUM_RE.search(output)
    if alpha_quantum_match is None:
        failures.append("no [ALPHA-QUANTUM] line at all")
    else:
        aq_units, aq_variance_ppm, aq_mode = alpha_quantum_match.groups()
        if aq_units != "0" or aq_mode != "free":
            failures.append(
                f"[ALPHA-QUANTUM]: expected units=0 mode=free under "
                f"synthetic pacing (declined long before the variance "
                f"check runs), got units={aq_units} "
                f"variance_ppm={aq_variance_ppm} mode={aq_mode}"
            )

    # Under the topology negative control the subject is the wave guard, and
    # WORLD_SCROLL is an unrelated class whose ROW is itself statistically
    # flaky at this tick budget (see TICKS' note above). Requiring it here
    # would import that flake into an assertion it has nothing to do with; the
    # default arm still requires all three.
    required = (tuple(c for c in REQUIRED_CLASSES if c != "WORLD_SCROLL")
                if args.topology_negative_control or args.pan_demote
                else REQUIRED_CLASSES)
    for cls in required:
        if cls not in rows:
            failures.append(f"no [SMOOTH-VERDICT] row for class={cls}")
            continue
        blend, snap, top_reason, pandemoted, noowner, _, _ = rows[cls]
        n = blend + snap
        if n <= 0:
            failures.append(f"class={cls}: blend+snap={n} is not positive")
        if cls == "WATER_WAVE":
            if n <= 0:
                failures.append("class=WATER_WAVE: non-vacuity requires n>0")
            if args.topology_negative_control:
                # Every pair is a declared topology change, so nothing about
                # the surface may blend and the refusal must be attributed to
                # the clause that caused it -- not folded into a generic hold.
                if blend != 0:
                    failures.append(
                        f"class=WATER_WAVE: blend={blend} with "
                        "MDKR_TEST_WAVE_TOPOLOGY_FLIP=1 — a pair the guard "
                        "declared incompatible was interpolated anyway")
                if top_reason != "TOPOLOGY_MISMATCH":
                    failures.append(
                        f"class=WATER_WAVE: top_reason={top_reason} with "
                        "MDKR_TEST_WAVE_TOPOLOGY_FLIP=1 — the surface "
                        "snapped, but not for the reason the seam forced")
            elif args.pan_demote:
                if noowner != 0:
                    failures.append(
                        f"class=WATER_WAVE: noowner={noowner} under forced "
                        "pan — ownership must remain intact while the choke "
                        "point demotes the draw"
                    )
                if pandemoted <= 0 or top_reason != "PAN_RATE_DEMOTED":
                    failures.append(
                        f"class=WATER_WAVE: pandemoted={pandemoted} "
                        f"top_reason={top_reason} under forced pan"
                    )
            elif args.expect_water_owned:
                # Two separate claims, because either alone is satisfiable by
                # a broken tree: blend>0 without noowner==0 is "some tiles
                # got owners", and noowner==0 without blend>0 is "nothing
                # graded this class at all in a way that could fail".
                if blend <= 0:
                    failures.append(
                        f"class=WATER_WAVE: blend={blend} over n={n} — wave "
                        "geometry is supposed to carry a presentation owner "
                        "(Task 7) and nothing about the surface interpolated"
                    )
                if noowner != 0:
                    failures.append(
                        f"class=WATER_WAVE: noowner={noowner} of n={n} — "
                        "some wave draws still reach the census with no "
                        "presentation owner at all. top_reason cannot see "
                        "this: a minority of unowned tiles hides behind a "
                        "modal BLEND"
                    )
            else:
                if snap != n or top_reason != "NO_OWNER":
                    failures.append(
                        f"class=WATER_WAVE: expected snap=={n} "
                        f"top_reason=NO_OWNER (today's pinned truth — wave "
                        f"geometry has no presentation owner yet), got "
                        f"blend={blend} snap={snap} "
                        f"top_reason={top_reason}. This is the "
                        f"--no-expect-water-owned arm: it only holds on a "
                        f"tree from before wave ownership landed."
                    )

    if args.pan_demote:
        # WATER_WAVE is the deterministic positive witness. WORLD_SCROLL's
        # lookup can legitimately produce only UV_HOLD rows on a host-scheduled
        # run; if it does confirm a draw, forced yaw must prevent BLEND.
        if "WORLD_SCROLL" in rows and rows["WORLD_SCROLL"][0] != 0:
            failures.append(
                f"class=WORLD_SCROLL: blend={rows['WORLD_SCROLL'][0]} under "
                "forced pan — a confirmed scroller escaped demotion"
            )
        if "OBJECT_ROOT" in rows and rows["OBJECT_ROOT"][3] != 0:
            failures.append(
                f"class=OBJECT_ROOT: pandemoted="
                f"{rows['OBJECT_ROOT'][3]} but OBJECT_ROOT is not in the "
                "demotable surface-class set and must never be demoted"
            )
        threshold_match = PAN_THRESHOLD_RE.search(output)
        if threshold_match is None:
            failures.append("no [PAN-DEMOTE] production-threshold row")
        elif abs(float(threshold_match.group(1)) -
                 EXPECTED_PRODUCTION_THRESHOLD_DEG) > 1e-6:
            failures.append(
                f"[PAN-DEMOTE] threshold={threshold_match.group(1)}, "
                f"expected {EXPECTED_PRODUCTION_THRESHOLD_DEG}"
            )
        forced_match = PAN_FORCED_YAW_RE.search(output)
        if forced_match is None:
            failures.append("token-gated forced-yaw seam did not report")
        elif abs(float(forced_match.group(1)) -
                 float(PAN_DEMOTE_FORCED_YAW_DEG)) > 1e-6:
            failures.append(
                f"forced yaw={forced_match.group(1)}, expected "
                f"{PAN_DEMOTE_FORCED_YAW_DEG}"
            )
        try:
            with tempfile.TemporaryDirectory(
                    prefix="mdkr_smooth_verdict_bare_pan_") as bare_temp:
                bare_output = run(
                    binary, rom, Path(bare_temp), args.timeout, args.verbose,
                    True, PAN_DEMOTE_TICKS, authorize_forced_yaw=False,
                )
        except (OSError, RuntimeError) as exc:
            failures.append(f"bare-token forced-yaw control: {exc}")
            bare_output = ""
        if PAN_FORCED_YAW_RE.search(bare_output) is not None:
            failures.append(
                "bare-token forced-yaw control activated without "
                "MDKR_INTERNAL_TEST_TOKEN"
            )
    else:
        # Default-off proof (Step 3): with the env unset, the choke point's
        # demotion clause can never fire, for ANY class.
        for cls, (blend, snap, top_reason, pandemoted, _,
                  _topo, _held_permille) in rows.items():
            if pandemoted != 0:
                failures.append(
                    f"class={cls}: MDKR_SMOOTH_PAN_DEMOTE is unset but "
                    f"pandemoted={pandemoted} -- default-off is violated"
                )

    if failures:
        print("check_smooth_verdict: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(f"rows: {rows}", file=sys.stderr)
        return 1

    summary = "; ".join(
        f"{cls} blend={rows[cls][0]} snap={rows[cls][1]} "
        f"top_reason={rows[cls][2]} pandemoted={rows[cls][3]} "
        f"noowner={rows[cls][4]} topomismatch={rows[cls][5]} "
        f"heldpermille={rows[cls][6]}"
        for cls in required if cls in rows
    )
    print(f"check_smooth_verdict: PASS — {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
