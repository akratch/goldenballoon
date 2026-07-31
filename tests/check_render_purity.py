#!/usr/bin/env python3
"""Render purity: skipping renders must not change authoritative state.

The fidelity spec's §12.2.1 gate (docs/
FIXED_SIMULATION_HIGH_RATE_PRESENTATION_SPEC_2026-07-28.md). After the Phase 2
migrations, rendering is required to be pure: a schedule that skips half of
all scene renders (`MDKR_TEST_SKIP_RENDER=odd`) must produce a byte-identical
per-tick `[SIMHASH]` stream.

Arms:

* **A — purity (the assertion):** with `MDKR_TEST_PURE_RENDER=1` (a test-only
  seed bracket around the whole render, subtracting the two documented
  blockers: the obj_tex_animate dice and the HUD voice-line dice — see the
  census's Phase 2 exit state), normal vs skip-odd streams must be
  BYTE-IDENTICAL over the full route.
* **B — positive control:** the same pair WITHOUT the bracket must DIVERGE:
  the residual render-path RNG fires only on rendered ticks, proving this
  gate can see impurity at all.
* **C — zero-delta replay, authoritative (Phase 3 Wave B slice 1):** with
  `MDKR_TEST_REPLAY_WALK=1` every tick's display list is walked a SECOND time
  through the HLE, with the frozen matrix registry restored and the same
  view-projection. The `[SIMHASH]` stream must stay byte-identical to arm A's:
  drawing the frame twice is not allowed to move one authoritative bit.
* **D — zero-delta replay, pixels:** the same replay must also overdraw the
  frame IDENTICALLY. Dumped frames are compared byte-for-byte against a
  non-replay run of the same route. Arm D is the one that can see a replay
  which renders *nearly* the same thing; arm C cannot.
* **E — recomposition control (the path slice 2 actually uses):** with
  `MDKR_TEST_REPLAY_WALK=recompose` every gameplay-camera matrix is forced
  through the `world x view_projection` recomposition, still with the camera
  unchanged. Authoritative state and pixels must again be untouched. This arm
  exists because arms C/D alone never exercise the recomposition — a replay
  that overrides nothing has nothing to rebuild — so without it slice 2 would
  be building on an unproven path.

Arms C/D history: their first runs failed at ~8k of 307k pixels, over 7k of
them off by exactly 1. Root cause was not the matrix path but the caster frame:
gfx_end_frame commits this tick's shadow casters and swaps the read index
before the replay runs, so the replay was lighting the scene with a shadow map
one tick ahead of the geometry casting it.

Arm E's first run found the second real defect: 5178 of 35119 gameplay matrices
did not recompose back to the display list they came from, the worst by ~2320
world units, because not every registration site's decomposition is the exact
association the list was built with. The replay now verifies each decomposition
against the list before trusting it with a different camera, and falls back to
the list's own matrix when it does not hold.

That verification is bit-exact HERE and a geometric distance on the interpolated
path, and arm E is what keeps the two apart. Arm E forces recomposition with the
camera UNCHANGED, so the display list already holds the exact matrix and the
only thing a tolerance could buy is a precision loss against the identical-
overdraw assertion below. The `mtxtol == 0` check states that separation as an
assertion instead of leaving it to the pixels to notice.

History note: arm A's first ever runs failed and root-caused three real
defects — the skydome object's camera-follow write (hash-visible live
object), and two 1-ULP add/subtract restore pairs on racers (tumble y,
carBob xyz). This gate exists precisely to keep finding that class.

Known blind spots, stated rather than discovered later
------------------------------------------------------

* **The purity hash is v2** (raised from v1 on this branch). `[SIMHASH]`
  covers the fields `mdkr_sim_hash_frame()` hashes, so arm A can only prove
  purity *as far as the hash sees*. v1 saw RNG plus position, y_rotation and
  scale; v2 adds full rotation, transform flags, velocity, animFrame,
  header/segment ids and the whole particle integrator — see the field table
  in `platform/sim_hash.c`. That raises the ceiling but does not remove it.
  Still outside: `unk201`, the held-object lerp, and the census's other
  non-transform findings, all of which were found by reading rather than by
  this gate.
* **Three render-owned object fields are excluded from v2 ON PURPOSE**, each
  an open census item with a named migration: `distanceToCamera` (finding 3),
  `opacity` (item 5) and `modelIndex` (item 6). They are not authoritative
  state today — render writes them — so hashing them into the authoritative
  stream would assert something false. `MDKR_STATE_HASH=2x` hashes them anyway
  and is the measurement that keeps this honest: under `2x` arm A goes RED,
  under `2` it is green, so the gap between this gate's ceiling and true
  purity is exactly the census's own open list. Re-measure with `2x` after
  each migration lands; when the list empties, fold them in as v3.
* **Arm E's ceiling.** `mtxhit != 0` proves the recomposition ran, not that it
  is still working; see the reject-ratio assertion below for why that needed a
  second bound and what it is set to.

Always muted + headless. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
FRAMES = 3600
# The authoritative field set every arm asserts over. Promoting this from "1"
# to "2" strengthens every "byte-identical" claim below; see the blind-spot
# notes in the module docstring for what v2 does and does not reach.
HASH_VERSION = "2"
# Arm D dumps real images, so it runs a short prefix of the same route. The
# frames sampled are late enough to be in-race with a moving camera and a
# populated scene, which is where a not-quite-identical redraw shows up.
PIXEL_FRAMES = 200
PIXEL_DUMP_FROM = 190


def run_arm(binary: Path, rom: Path, label: str, root: Path,
            extra_env: dict[str, str], timeout: int,
            verbose: bool, frames: int = FRAMES,
            dump_frames: bool = False) -> list[str]:
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_STATE_HASH=HASH_VERSION,
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
    if dump_frames:
        frame_dir = run_dir / "frames"
        frame_dir.mkdir(parents=True)
        env["MDKR_DUMP_FROM"] = str(PIXEL_DUMP_FROM)
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
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    rows = [line for line in output.splitlines()
            if line.startswith("[SIMHASH]")]
    if len(rows) != frames:
        raise RuntimeError(
            f"{label}: expected {frames} [SIMHASH] rows, got {len(rows)}")
    run_arm.last_output = output
    return rows


def replay_summary(output: str) -> dict[str, int]:
    """Parse the last [REPLAY-SUMMARY] telemetry row."""
    row = None
    for line in output.splitlines():
        if line.startswith("[REPLAY-SUMMARY]"):
            row = line
    if row is None:
        raise RuntimeError("no [REPLAY-SUMMARY] row was emitted")
    fields: dict[str, int] = {}
    for token in row.split()[1:]:
        key, _, value = token.partition("=")
        fields[key] = int(value)
    return fields


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build-rel")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=900)
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
    with tempfile.TemporaryDirectory(prefix="mdkr-render-purity-") as tmp:
        root = Path(tmp)
        try:
            pure = {"MDKR_TEST_PURE_RENDER": "1"}
            pure_skip = {"MDKR_TEST_PURE_RENDER": "1",
                         "MDKR_TEST_SKIP_RENDER": "odd"}
            raw_skip = {"MDKR_TEST_SKIP_RENDER": "odd"}
            replay = {"MDKR_TEST_PURE_RENDER": "1",
                      "MDKR_TEST_REPLAY_WALK": "1",
                      "MDKR_PRESENT_SCHED_TRACE": "1"}
            recompose = {"MDKR_TEST_PURE_RENDER": "1",
                         "MDKR_TEST_REPLAY_WALK": "recompose",
                         "MDKR_PRESENT_SCHED_TRACE": "1"}
            arm_a_normal = run_arm(binary, rom, "pure-normal", root,
                                   pure, args.timeout, args.verbose)
            arm_a_skip = run_arm(binary, rom, "pure-skip", root,
                                 pure_skip, args.timeout, args.verbose)
            arm_b_normal = run_arm(binary, rom, "raw-normal", root,
                                   {}, args.timeout, args.verbose)
            arm_b_skip = run_arm(binary, rom, "raw-skip", root,
                                 raw_skip, args.timeout, args.verbose)
            arm_c_replay = run_arm(binary, rom, "pure-replay", root,
                                   replay, args.timeout, args.verbose)
            summary = replay_summary(run_arm.last_output)
            arm_e_replay = run_arm(binary, rom, "pure-recompose", root,
                                   recompose, args.timeout, args.verbose)
            recompose_summary = replay_summary(run_arm.last_output)
            run_arm(binary, rom, "pixel-normal", root, {}, args.timeout,
                    args.verbose, frames=PIXEL_FRAMES, dump_frames=True)
            run_arm(binary, rom, "pixel-replay", root,
                    {"MDKR_TEST_REPLAY_WALK": "1"}, args.timeout,
                    args.verbose, frames=PIXEL_FRAMES, dump_frames=True)
            run_arm(binary, rom, "pixel-recompose", root,
                    {"MDKR_TEST_REPLAY_WALK": "recompose"}, args.timeout,
                    args.verbose, frames=PIXEL_FRAMES, dump_frames=True)
        except RuntimeError as error:
            print(f"check_render_purity: FAIL\n  - {error}")
            return 1

        if arm_a_normal != arm_c_replay:
            first = next(i for i, (a, b) in
                         enumerate(zip(arm_a_normal, arm_c_replay)) if a != b)
            failures.append(
                f"arm C: the zero-delta replay moved authoritative state at "
                f"tick {first} ({arm_a_normal[first]} vs "
                f"{arm_c_replay[first]}) — a second walk of the same display "
                "list is reaching something it must not")
        # DKR does not submit a graphics task on every frame, so the replay
        # count must equal the REAL walk count, not the tick count.
        if summary["walks"] != summary["realwalks"]:
            failures.append(
                f"arm C: {summary['walks']} replay walks against "
                f"{summary['realwalks']} real display-list walks — the replay "
                "skipped a walk, so the arm proves less than it claims")
        if summary["realwalks"] == 0:
            failures.append("arm C: no display lists were walked at all")
        if summary["freezefail"] != 0:
            failures.append(
                f"arm C: {summary['freezefail']} registry freezes failed whole")
        if summary["restores"] != summary["walks"]:
            failures.append(
                f"arm C: {summary['restores']} registry restores for "
                f"{summary['walks']} replays — the frozen registry is not "
                "reaching every replay")
        # Ship review (A3). gfx_shadow_replay_restore takes custody of the live
        # registry before dropping the frozen copy on it, and every failure
        # return between those two points used to leave the custody flag set —
        # which permanently refuses every later restore AND leaves the frozen
        # set masquerading as live. The unwind paths are allocation failures,
        # so they are not reachable from a fixture; this counter is how the
        # gate states that none of them fired. restores == walks above would
        # also catch a wedged module, but only after the fact and without
        # naming the cause.
        if summary["restorefail"] != 0:
            failures.append(
                f"arm C: {summary['restorefail']} replay restore/release "
                "unwinds — the registry lost a frame of registrations")
        notes.append(
            f"arm C: {summary['walks']}/{summary['realwalks']} display lists "
            f"re-walked, {summary['freezes']} registry freezes, 0 failures, "
            "[SIMHASH] byte-identical to the single-walk run")

        if arm_a_normal != arm_e_replay:
            first = next(i for i, (a, b) in
                         enumerate(zip(arm_a_normal, arm_e_replay)) if a != b)
            failures.append(
                f"arm E: forced recomposition moved authoritative state at "
                f"tick {first}")
        if recompose_summary["mtxhit"] == 0:
            failures.append(
                "arm E: zero matrices were recomposed — the arm did not "
                "exercise the path slice 2 depends on, so it proves nothing")
        # Ship review (A5): mtxhit != 0 alone leaves a ceiling the arm cannot
        # see. A recomposition that regressed until it rejected nearly
        # everything would keep mtxhit non-zero (one accepted matrix is
        # enough), silently fall back to the display list's own matrix for the
        # rest, and still pass every other assertion here — because falling
        # back is bit-exact by construction, so neither the hash nor the
        # pixels move. The arm would be defanged without going red. Measured on
        # this branch: 3413 rejects against 28173 hits, a 10.8% reject ratio.
        # The bound is set at 50%, comfortably clear of that but far below the
        # ~100% a real regression produces.
        recomposed = (recompose_summary["mtxhit"] +
                      recompose_summary["mtxreject"])
        if recomposed and recompose_summary["mtxreject"] / recomposed > 0.50:
            failures.append(
                f"arm E: {recompose_summary['mtxreject']}/{recomposed} "
                f"({recompose_summary['mtxreject'] / recomposed:.1%}) of "
                "gameplay matrices failed to recompose and kept the list's "
                "own matrix — the arm still 'passes' because a fallback is "
                "bit-exact, but the interpolation path it exists to prove is "
                "no longer being exercised")
        # The vp_overridden GATING of the geometric recomposition tolerance,
        # asserted from the one arm that can see it.
        #
        # On an interpolated present the verification is a distance
        # (DKR_RECOMPOSE_TOLERANCE_LSB, gfx_pc_dkr.c) rather than an identity,
        # because rejecting a correct-but-re-associated decomposition draws that
        # geometry with the TICK's camera while the rest of the frame gets the
        # interpolated one. Arm E is the other path: it forces recomposition
        # with the camera UNCHANGED, where the display list already holds the
        # exact matrix and a tolerance could only license a needless precision
        # loss -- which arm E's own pixel comparison above would then have to
        # catch, byte for byte, on every dumped frame.
        #
        # So the tolerance must never fire here, and this asserts it directly
        # rather than inferring it from the pixels staying identical. If the
        # gating is ever widened to the non-overridden path, this goes red on
        # the FIRST tolerated matrix instead of waiting for one to move a pixel.
        if recompose_summary.get("mtxtol", 0) != 0:
            failures.append(
                f"arm E: {recompose_summary['mtxtol']} matrices were accepted "
                "by the geometric tolerance on the camera-UNCHANGED path. That "
                "path must stay bit-exact: the display list already holds the "
                "exact matrix, so a tolerated recomposition here is a pure "
                "precision loss, and arm E's identical-overdraw assertion is "
                "what it would be traded against")
        notes.append(
            "arm E: 0 matrices accepted by the geometric tolerance — it stays "
            "gated on vp_overridden, so this arm's bit-exact semantics hold")
        notes.append(
            f"arm E: {recompose_summary['mtxhit']} gameplay matrices "
            f"recomposed and verified against their display list, "
            f"{recompose_summary['mtxreject']} rejected as unfaithful "
            "decompositions and kept the list's matrix, "
            f"{recompose_summary['mtxmiss']} unregistered")

        normal_frames = sorted((root / "pixel-normal" / "frames").glob("*.ppm"))
        if not normal_frames:
            failures.append("arm D: no frames were dumped")
        for arm, label in (("pixel-replay", "D"),
                           ("pixel-recompose", "E")):
            other_dir = root / arm / "frames"
            differing = []
            for frame in normal_frames:
                other = other_dir / frame.name
                if not other.exists():
                    differing.append(f"{frame.name} (missing)")
                elif frame.read_bytes() != other.read_bytes():
                    differing.append(frame.name)
            if differing:
                failures.append(
                    f"arm {label}: the replay did NOT overdraw identically — "
                    f"{len(differing)}/{len(normal_frames)} dumped frames "
                    f"differ ({', '.join(differing[:4])}). A zero-delta replay "
                    "that changes pixels means the second walk is consuming "
                    "different renderer state than the first")
            else:
                notes.append(
                    f"arm {label} pixels: {len(normal_frames)} dumped frames "
                    "byte-identical to the single-walk build")

    if arm_a_normal != arm_a_skip:
        first = next(i for i, (a, b) in
                     enumerate(zip(arm_a_normal, arm_a_skip)) if a != b)
        failures.append(
            f"render is IMPURE: skip-render stream diverges at tick {first} "
            f"({arm_a_normal[first]} vs {arm_a_skip[first]}) — a render-path "
            "write to authoritative state has been (re)introduced")
    if arm_b_normal == arm_b_skip:
        failures.append(
            "positive control failed: without the diagnostic bracket the "
            "streams are identical — either the residual render RNG stopped "
            "firing on this route (find a new control route) or the gate "
            "lost its sensitivity")

    if failures:
        print("check_render_purity: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    control_tick = next((i for i, (a, b) in
                         enumerate(zip(arm_b_normal, arm_b_skip))
                         if a != b), None)
    print(
        "check_render_purity: PASS — skip-render stream byte-identical over "
        f"{FRAMES} ticks (render is pure modulo the two documented RNG "
        f"blockers); positive control diverges at tick {control_tick}")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
