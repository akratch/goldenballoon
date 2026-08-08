#!/usr/bin/env python3
"""Attribute presentation smoothing to its individual stages at 120 Hz.

Motion smoothing is not one mechanism. It is seven independently gated stages
that all run inside the same interpolated replay walk, and until now the only
question any gate asked about them was "does this stage change pixels at all"
(``check_presentation_matrix.py``'s arm C controls). That answer is a yes/no
per stage against a *different* route each. It cannot say which stage carries
most of the visible motion on one route, and it therefore cannot tell an
artifact hunt where to look.

This gate answers the ranking question. It renders the same scripted route at
``MDKR_PRESENT_RATE=120`` against the 30 Hz authoritative tick — three
intermediate presents between every pair of authored images, which is the
densest alpha grid production can request on a 120 Hz display — once per stage
configuration:

* **all-on** — production behaviour, the reference every other arm is
  differenced against;
* **all-off** — every stage's opt-out set, so only the interpolated camera
  moves. This is the attribution ceiling and the harness's non-vacuity control;
* **leave-one-out**, once per stage — exactly one opt-out set. The difference
  from all-on is that stage's contribution *in the presence of all the others*,
  which is the number an artifact hunt needs; a stage measured alone would be
  measured in a scene the other six never composed.

Two routes, because no single one exercises all seven stages:

* **Route A — Jungle Falls (level 29), in-race, shields forced.** Carries
  object roots, model deformation, vertex shade, primitive alpha, shield shear
  and authored UV scroll. It ranks the stages.
* **Route B — battle challenge (level 26).** The only witness in the tree for
  world-space point-trail meshes: route A registers particle vertices but never
  interpolates one (``particledeformoverride=0``), so route A's particle arm
  would report "no contribution" for a stage that simply never fired. Route B
  measures it where it does.

What each arm asserts
---------------------

* **Authority is untouched.** Every arm's ``[SIMHASH]``/``[EVENTHASH]``/
  ``[INPUTHASH]`` stream must be byte-identical to its route's all-on arm.
  These envs are presentation-only by construction; a stream that moves is a
  real defect in the stage, not a harness artifact, and is reported as one.
* **Authored endpoints are exact.** At 120 Hz every fourth present is the real
  walk's own image. Those frames must be byte-identical across every arm —
  a stage that changes an endpoint has escaped the replay.
* **The opt-out actually reached the binary.** Each leave-one-out arm must
  drive the left-out stage's ``[PRESENT-PACKET]`` reach counter to exactly
  zero. Without this the harness's worst failure is silent: a mistyped env
  name produces an arm identical to all-on, and its zero pixel difference is
  indistinguishable from a stage that ran and changed nothing. Real zeros of
  the second kind exist in this table, so "zero difference" cannot validate
  itself.
* **The instrument is not vacuous.** all-off must differ from all-on on at
  least one intermediate frame, and every ranked stage must both be *reached*
  and *fire* — a non-zero override — on its route. A stage that is reached but
  never substitutes anything is reported as ``reached-never-interpolated``, one
  that is never reached at all as ``absent``; neither is ever silently read as
  innocence. All seven stages get an explicit ``[STAGE-DISPOSITION]`` row on
  both routes, ranked or not.
* **Every replayed recipe is anchored to its own tick.** A reconstruction
  measures its capture-time residual against the alpha-zero pose, and that
  cancels only if the two describe the same authored tick. Every production arm
  asserts ``ownertickmismatch=0`` over a non-zero ``ownertickcheck``, for all
  four recipe classes, and the assertion first requires both fields to be
  *present*. This is the exact witness for the shield/magnet anchoring defect
  that shipped in v1.0.1-v1.0.3 and read 708 of 708 violations on route A; the
  pixel-level backstop is ``check_effect_shell_envelope.py``.
* **Every UV-scroll hold is attributed.** A held scroll batch steps its texture
  phase at the authored tick rate while the surface around it glides, so the
  hold rate is a visible quantity and its *clauses* are what disposition it.
  Each route's all-on arm asserts the four clause counters account for the
  aggregate exactly, and prints the split as ``[UV-SCROLL-HOLDS]``.
* **The primitive-alpha magnitude census is arithmetically alive.** An
  override is a changed alpha byte, so ``primalphadeltasum`` can never be
  below ``primalphaoverride`` and the peak can never be zero while overrides
  fire. That identity is what stops the counter whose numbers disposition
  artifact class C9 from silently accumulating nothing. Printed as
  ``[PRIM-ALPHA-MAGNITUDE]``.

The uncaptured-external arm
---------------------------

The same run shape carries the evidence for the retained-pointer fail-closed
path. ``dkr_retain_resolved_pointer`` refuses an interpolated walk that
resolves a non-arena dependency with no retained copy. On a correct tree that
branch is unreachable, so **every** production arm — all-on, all-off and each
leave-one-out, on both routes — asserts ``uncapturedext=0``, and the assertion
first requires the stat field to be *present*: a lookup with a default would
pass silently the day the counter is renamed. The covered arm and replay counts
are printed as ``[UNCAPTURED-OWNERSHIP]`` rather than summed by hand.
``MDKR_TEST_UNCAPTURED_EXTERNAL`` (token-gated) then forces every external
lookup to miss and asserts the walks refuse — holding the authored image —
rather than reading live memory.

Always muted + headless. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import operator
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, parse_last, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
HASH_VERSION = "3"

# The 120 Hz-equivalent alpha grid. The authoritative tick is two 60 Hz fields,
# so 120 presents per second is exactly four presents per tick: the authored
# endpoint plus three interpolated images at alpha 1/4, 2/4, 3/4.
PRESENT_RATE = "120"
PRESENTS_PER_TICK = 4

# Frames captured per arm, ending at the last present of the run. 120 presents
# is 30 authoritative ticks and 90 interpolated images -- enough that a stage
# firing a few times a second is still sampled dozens of times, and small
# enough that eleven arms of 640x480 PPM stay inside a temporary directory.
DUMP_FRAMES = 120

# The versioned token every adversarial replay seam requires (present_sched.c).
INTERNAL_TEST_TOKEN = "mdkr64-presentation-replay-v1"

# Route A. The navigation route's countdown clears around authored tick 3120
# (see check_presentation_matrix.py's deformation witness), so the sampled
# window is deliberately after it: racers under power, camera swinging, the
# waterfall sheet and wave-driven water scrolling their authored UV phase.
# MDKR_FORCE_SHIELD is in PRESENT indices, and its window brackets the dump so
# the shield shear stage is live for every sampled frame.
# Route C. The fractional-scroller witness, and the reason it is not route A.
#
# obj_loop_texscroll advances a level texture through a TWO-BIT accumulator:
# it adds the authored rate (quarter-units a tick) to a residue, emits the
# whole units that completes and keeps the rest. An authored rate that is a
# multiple of four therefore emits the same whole-unit step every tick, and a
# rate that is not alternates its step by one, forever.
#
# Jungle Falls' waterfall is the first kind -- 32 quarter-units a tick at
# updateRate 2, so 16 whole units every tick, and it confirms on every
# lookup. Both existing arms that exercise UV scroll run Jungle Falls ONLY,
# so neither has ever driven the second kind. Level 19 is the second kind
# twice over (authored V rates 127 and 85, both odd), which is what makes it
# the arm that can fail: with the authored rate absent, no two of its ticks
# can agree, the confirm-or-hold rule refuses every one of them, and its
# water holds its texture phase on every interpolated present while the world
# glides past it. That is the shimmer, and it is measured here red and green.
ROUTE_C_TICKS = 3400
ROUTE_C_TRACK = "19"

# The counters the arm reads. Registrations and confirmations that carried an
# authored rate; a lookup with a default would pass silently the day either is
# renamed, so presence is asserted before value.
UV_AUTHORED_CONTRACT = ("uvscrollauthored", "uvscrollauthoredconfirm")

ROUTE_A_TICKS = 3230
ROUTE_A_TRACK = "29"
ROUTE_A_SHIELD = "12680:600"

# Route B. The battle challenge's continuous point trails are the only content
# in the tree that moves a world-space particle mesh between adjacent authored
# ticks; check_presentation_matrix.py's particle arm uses the same level for
# the same reason.
ROUTE_B_TICKS = 4200
ROUTE_B_TRACK = "26"

# The uncaptured-external arm needs no pixels and no route depth: it only has
# to reach armed interpolated replay, which happens within the first hundred
# ticks. Kept short so the adversarial seam costs seconds, not half a minute.
FAILCLOSED_TICKS = 600

# (name, opt-out env, disarm counter, fire counter) — both counters are
# [PRESENT-PACKET] fields and they answer two different questions that a single
# number cannot separate.
#
# `disarm` must be non-zero in the all-on arm and EXACTLY ZERO in that stage's
# leave-one-out arm. That is the only thing standing between this harness and
# its worst failure mode: a mistyped env name produces an arm identical to
# all-on, whose zero pixel difference is indistinguishable from a stage that
# ran and changed nothing. Measured 30-tick windows already produce genuine
# zeros here (primitive_alpha), so "zero difference" cannot be self-validating.
#
# `fire` is the override count — the replay actually substituting an
# interpolated value. A stage can be reached without ever firing, and route A's
# particle stage is exactly that (particledeformhit=20166,
# particledeformoverride=0), so ranking needs the override and the env check
# needs the reach.
#
# deformation is the one stage whose reach counter is not usable as its disarm
# witness: `deformhit` is shared with the particle lookup and only falls from
# 767,829 to 20,166 when the seam is off. Its override is the clean witness.
STAGES = (
    ("object", "MDKR_TEST_OBJECT_INTERPOLATION",
     "matrixhit", "matrixoverride"),
    ("deformation", "MDKR_TEST_DEFORMATION_INTERPOLATION",
     "deformoverride", "deformoverride"),
    ("vertex_color", "MDKR_TEST_VERTEX_COLOR_INTERPOLATION",
     "colorhit", "coloroverride"),
    ("particle", "MDKR_TEST_PARTICLE_INTERPOLATION",
     "particledeformhit", "particledeformoverride"),
    ("primitive_alpha", "MDKR_TEST_PRIMITIVE_ALPHA_INTERPOLATION",
     "primalphahit", "primalphaoverride"),
    ("effect", "MDKR_TEST_EFFECT_INTERPOLATION",
     "effecthit", "effectoverride"),
    ("uv_scroll", "MDKR_TEST_UV_SCROLL_INTERPOLATION",
     "uvscrollhit", "uvscrolloverride"),
)

# Fields [REPLAY-SUMMARY] must carry for the ownership assertion below to mean
# anything. A dict lookup with a default silently passes when a stat is renamed
# or dropped, which turns the strongest claim this gate makes into a no-op.
REPLAY_STAT_CONTRACT = ("uncapturedext", "uncapturedrefusals")

# The same idea for [PRESENT-PACKET], guarding a different claim.
#
# Every replayed recipe -- object root, articulated child, billboard anchor and
# shield/magnet effect alike -- reconstructs its pose as
# ``interpolated_pose + (captured_source - alpha_zero_pose)``. That residual is
# meant to carry the render-only adjustments the snapshot has already restored,
# and it carries only those when the capture and the alpha-zero pose describe
# the SAME authored tick. Hand it a capture from the next tick and it silently
# becomes a constant one-tick offset: the object is coherently, consistently in
# the wrong place, on every interpolated present, at every alpha.
#
# That is not hypothetical. It is what ``mdkr_camera_replay_effect_world``
# shipped with in v1.0.1-v1.0.3 (708 of 708 effect overrides on route A), and a
# second instance reached the same wrong answer through abandoned billboard
# registrations surviving a pass that never submitted its list. Neither was
# visible to any pixel-difference control, because both arms of such a control
# put the object somewhere and a wrong somewhere is as coherent as a right one.
# See docs/evidence/smoothing-artifact-repro-2026-08.md section 2.
#
# So it is asserted structurally, on every production arm, for the whole
# family -- including the classes that obey the rule today and had nothing
# saying so.
PACKET_STAT_CONTRACT = ("ownertickcheck", "ownertickmismatch")

# The UV-scroll hold clauses, and the aggregate they must account for.
#
# A held scroll batch is a surface stepping its texture phase at the authored
# tick rate while everything around it glides, so the hold RATE is a visible
# quantity -- 14.56% of lookups on both routes. The rate alone cannot be
# dispositioned, because the clauses mean opposite things: a batch with no
# published {T-1} record is structural and self-clearing, while two published
# ticks that disagree about the displacement is the wrap the resolver could not
# undo. What is asserted here is the accounting, not the content: every hold
# lands in exactly one bucket. A future clause added without a counter would
# otherwise vanish into the aggregate, and the disposition in
# docs/evidence/smoothing-artifact-repro-2026-08.md section 5.2 would quietly
# stop describing the build. The per-clause VALUES are content -- a poisoned
# key is a deliberate outcome, not a defect -- so they are printed, not bounded.
UV_HOLD_CLAUSES = ("uvscrollholdunpub", "uvscrollholdambig",
                   "uvscrollholdshape", "uvscrollholdphase")

# The primitive-alpha substitution magnitude, and the arithmetic that makes it
# a witness rather than a decoration.
#
# `primalphadeltasum` is the quantitative basis for reading route A's 119,129
# overrides as sub-visible rather than as wasted work, and a counter nothing
# constrains is a counter that can be accumulating zero. Every override moves
# the alpha byte by at least one step, by the definition of "override", so the
# sum can never be smaller than the count and the peak can never be zero while
# the count is not. That identity is what a `+= 0` regression breaks.
PRIM_ALPHA_STAT_CONTRACT = ("primalphaoverride", "primalphadeltasum",
                            "primalphadeltapeak")

# A stage is ranked only from a route that actually fires it. Route A never
# interpolates a world-space particle mesh -- it registers particle vertices
# and then holds every one of them -- so ranking `particle` there would report
# an absent stage as an innocent one, which is the single most misleading thing
# an attribution table can do.
ROUTE_A_STAGES = tuple(
    name for name, _, _, _ in STAGES if name != "particle")
ROUTE_B_STAGES = ("particle",)

STAGE_OFF = "off"


def stage_env(disabled: tuple[str, ...]) -> dict[str, str]:
    return {env: STAGE_OFF for name, env, _, _ in STAGES if name in disabled}


def replay_ownership(output: str, label: str) -> tuple[list[str], int]:
    """Assert one arm resolved no uncaptured external. Returns (problems, ext).

    Applied to EVERY arm that runs production interpolated replays, not only
    the all-on reference: a leave-one-out arm replays just as many times, and
    an ownership hole that only appears with one stage disabled is exactly the
    kind a reference-arm-only assertion would miss.
    """

    problems: list[str] = []
    replay = parse_last(output, "REPLAY-SUMMARY")
    missing = [key for key in REPLAY_STAT_CONTRACT if key not in replay]
    if missing:
        problems.append(
            f"{label}: [REPLAY-SUMMARY] carries no {'/'.join(missing)} field. "
            "That is the uncaptured-external stat contract "
            "(gfx_dkr_replay_get_uncaptured_stats -> present_sched.c); without "
            "it this gate's ownership assertion silently passes on every arm")
        return problems, 0
    if replay["uncapturedext"] != 0:
        problems.append(
            f"{label}: uncapturedext={replay['uncapturedext']} — an "
            "interpolated walk resolved a non-arena dependency with no "
            "retained copy. Before the fail-closed change it would have read "
            "that pointer live")
    return problems, replay["uncapturedext"]


def replay_tick_agreement(output: str, label: str,
                          expect_checks: bool) -> tuple[list[str], int]:
    """Assert one arm anchored every replayed recipe to its own capture tick.

    Applied to every production arm for the same reason ``replay_ownership``
    is: a leave-one-out arm replays just as often, and an anchoring error that
    only appears with one stage disabled is exactly what a reference-arm-only
    assertion would miss.
    """

    problems: list[str] = []
    packet = parse_last(output, "PRESENT-PACKET")
    missing = [key for key in PACKET_STAT_CONTRACT if key not in packet]
    if missing:
        problems.append(
            f"{label}: [PRESENT-PACKET] carries no {'/'.join(missing)} field. "
            "That is the tick-agreement stat contract "
            "(gfx_presentation_packet_note_owner_tick -> present_sched.c); "
            "without it this gate's anchoring assertion silently passes on "
            "every arm")
        return problems, 0
    if packet["ownertickcheck"] == 0:
        # An arm with MDKR_TEST_OBJECT_INTERPOLATION=off calls none of the
        # replay transforms, so it has no residual to anchor and a zero here
        # is the arm working as configured. Every other arm's zero would mean
        # the stamp itself had gone missing, which reads as green while
        # asserting nothing -- the exact failure PACKET_STAT_CONTRACT exists
        # to prevent one level up.
        if expect_checks:
            problems.append(
                f"{label}: ownertickcheck=0 -- no replayed recipe was checked "
                "at all, so ownertickmismatch=0 says nothing. Either the "
                "replay did not run or the recipes lost their capture stamp")
        return problems, 0
    if packet["ownertickmismatch"] != 0:
        problems.append(
            f"{label}: ownertickmismatch={packet['ownertickmismatch']} of "
            f"{packet['ownertickcheck']} -- a replayed recipe's residual was "
            "measured against a pose from a different authored tick than the "
            "recipe itself describes, which adds a whole tick of the owner's "
            "travel to its reconstructed position AND heading at every alpha. "
            "See docs/evidence/smoothing-artifact-repro-2026-08.md section "
            "2.3")
    return problems, packet["ownertickcheck"]


def uv_scroll_hold_accounting(output: str, label: str) -> tuple[list[str], str]:
    """Assert the UV-scroll hold clauses account for every hold on one arm."""

    problems: list[str] = []
    packet = parse_last(output, "PRESENT-PACKET")
    missing = [key for key in (*UV_HOLD_CLAUSES, "uvscrollhold")
               if key not in packet]
    if missing:
        problems.append(
            f"{label}: [PRESENT-PACKET] carries no {'/'.join(missing)} field. "
            "That is the UV-scroll hold-clause contract "
            "(gfx_presentation_packet_lookup_uv_scroll -> present_sched.c); "
            "without it a hold rate cannot be attributed to the refusal that "
            "produced it")
        return problems, ""
    total = sum(packet[key] for key in UV_HOLD_CLAUSES)
    if total != packet["uvscrollhold"]:
        problems.append(
            f"{label}: the UV-scroll hold clauses sum to {total} against "
            f"uvscrollhold={packet['uvscrollhold']}. A refusal reached the "
            "aggregate without landing in a bucket, so the hold rate can no "
            "longer be attributed. See "
            "docs/evidence/smoothing-artifact-repro-2026-08.md section 5.2")
    return problems, " ".join(
        f"{key}={packet[key]}"
        for key in ("uvscrollhit", "uvscrollhold", *UV_HOLD_CLAUSES))


def primitive_alpha_magnitude(output: str, label: str) -> tuple[list[str], str]:
    """Assert the primitive-alpha magnitude census is arithmetically alive."""

    problems: list[str] = []
    packet = parse_last(output, "PRESENT-PACKET")
    missing = [key for key in PRIM_ALPHA_STAT_CONTRACT if key not in packet]
    if missing:
        problems.append(
            f"{label}: [PRESENT-PACKET] carries no {'/'.join(missing)} field. "
            "That is the primitive-alpha magnitude contract "
            "(gfx_presentation_packet_note_primitive_alpha -> "
            "present_sched.c); without it the substitutions' size cannot be "
            "read and docs/evidence/smoothing-artifact-repro-2026-08.md "
            "section 5.3 stops describing this build")
        return problems, ""
    overrides = packet["primalphaoverride"]
    total = packet["primalphadeltasum"]
    peak = packet["primalphadeltapeak"]
    if overrides == 0:
        # Nothing fired here, so nothing is claimed. Route B always fires; a
        # route that does not is reported by [STAGE-DISPOSITION] already.
        return problems, f"primalphaoverride=0 primalphadeltasum={total}"
    if total < overrides or peak == 0:
        problems.append(
            f"{label}: primalphaoverride={overrides} but "
            f"primalphadeltasum={total} and primalphadeltapeak={peak}. An "
            "override is by definition a changed alpha byte, so the sum "
            "cannot be below the count and the peak cannot be zero. The "
            "magnitude census is accumulating nothing")
    return problems, (f"primalphaoverride={overrides} "
                      f"primalphadeltasum={total} primalphadeltapeak={peak} "
                      f"meandelta={total / overrides:.2f}")


def run(binary: Path, rom: Path, label: str, root: Path, track: str,
        ticks: int, extra_env: dict[str, str], timeout: int, verbose: bool,
        dump: bool) -> tuple[str, Path | None]:
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_EVENT_HASH="1",
        MDKR_INPUT_HASH="1",
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=track,
        # GL, deliberately, and NOT because GL is what matters.
        #
        # This gate drives 120 Hz to get several interpolated presents per
        # authoritative tick. On WebGPU that is exactly the window where a
        # replay is refused admission (it needs zero frames in flight and the
        # tick's own frame has not retired at 8.3 ms), so ~80% of presents come
        # back stale and the rows below measure a stream that barely
        # interpolates -- R4 saw displayed=4868 of 24000 presents. The gate
        # would be failing on the renderer's admission policy rather than on
        # anything it exists to judge.
        #
        # Pass --renderer webgpu to run it there anyway; it is one command, and
        # it is how the starvation was characterised. Making WebGPU the default
        # is blocked on the open item in docs/open-items/renderer.md
        # ("interpolated presents are starved on WebGPU above the display
        # refresh"), not on anything in this file.
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
        MDKR_PRESENT_RATE=PRESENT_RATE,
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_PRESENT_SCHED_TRACE="1",
    )
    env.update(extra_env)
    command = [
        str(binary), "--headless-ticks", str(ticks),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", "320x240",
    ]
    frame_dir: Path | None = None
    if dump:
        frame_dir = run_dir / "frames"
        frame_dir.mkdir(parents=True)
        env["MDKR_DUMP_FROM"] = str(
            ticks * PRESENTS_PER_TICK - DUMP_FRAMES)
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
    return output, frame_dir


def hash_rows(output: str) -> tuple[list[str], list[str], list[str]]:
    sim, event, inp = [], [], []
    for line in output.splitlines():
        if line.startswith("[SIMHASH]"):
            sim.append(line)
        elif line.startswith("[EVENTHASH]"):
            event.append(line)
        elif line.startswith("[INPUTHASH]"):
            inp.append(line)
    return sim, event, inp


def load_frames(frame_dir: Path) -> dict[int, bytes]:
    frames: dict[int, bytes] = {}
    for path in sorted(frame_dir.glob("*.ppm")):
        frames[int(path.stem.split("_")[1])] = path.read_bytes()
    return frames


def frame_delta(left: bytes, right: bytes) -> tuple[float, float]:
    """(changed byte fraction, mean absolute difference), both whole-frame.

    Both denominators are the whole PPM payload, header included; the header is
    a short fixed prefix identical between two dumps of the same geometry, so
    it dilutes both numbers by the same constant and cannot manufacture a
    difference. Reported together because neither alone is readable: the
    fraction says how much of the screen a stage touched, the mean absolute
    difference weights that by how far the touched bytes moved, and their
    ratio is the mean magnitude among the bytes that actually changed. A wide
    faint change and a narrow violent one are the same number in one metric
    and opposite in the other.
    """

    if left == right:
        return 0.0, 0.0
    if len(left) != len(right):
        raise RuntimeError("frame size differs between arms")
    total = 0
    changed = 0
    for delta in map(operator.sub, left, right):
        if delta:
            changed += 1
            total += delta if delta > 0 else -delta
    return changed / len(left), total / len(left)


def attribute(baseline: dict[int, bytes], arm: dict[int, bytes],
              label: str) -> tuple[dict[str, float], list[str]]:
    """Difference an arm against its route's all-on reference."""

    problems: list[str] = []
    shared = sorted(set(baseline) & set(arm))
    if len(shared) != DUMP_FRAMES:
        problems.append(
            f"{label}: {len(shared)} frames shared with the all-on arm, "
            f"expected {DUMP_FRAMES} — the arms did not sample the same "
            "presents, so no difference between them is attributable")
    endpoint_moved = 0
    intermediates = 0
    differing = 0
    changed_sum = 0.0
    abs_sum = 0.0
    for index in shared:
        if index % PRESENTS_PER_TICK == 0:
            if baseline[index] != arm[index]:
                endpoint_moved += 1
            continue
        intermediates += 1
        changed, mean_abs = frame_delta(baseline[index], arm[index])
        if changed:
            differing += 1
        changed_sum += changed
        abs_sum += mean_abs
    if endpoint_moved:
        problems.append(
            f"{label}: {endpoint_moved} authored endpoint frames differ from "
            "the all-on arm. Endpoints are the real walk's own images; a "
            "presentation stage that moves one has escaped the replay")
    return ({
        "intermediates": intermediates,
        "differing": differing,
        "changedfrac": changed_sum / intermediates if intermediates else 0.0,
        "meanabs": abs_sum / intermediates if intermediates else 0.0,
    }, problems)


def fractional_scroller(green: str, red: str) -> tuple[list[str], str]:
    """Assert the authored-rate path is what carries a fractional scroller.

    Two arms of one route: production, and the same route with the authored
    rate opted out so the batch has to be recovered by differencing its own
    result. The comparison is the whole point -- a green arm alone cannot
    distinguish "this content interpolates" from "this content was never
    hard", and the content that was never hard is exactly what the two
    existing UV-scroll arms run.
    """

    problems: list[str] = []
    on = parse_last(green, "PRESENT-PACKET")
    off = parse_last(red, "PRESENT-PACKET")
    missing = [key for key in (*UV_AUTHORED_CONTRACT, *UV_HOLD_CLAUSES)
               if key not in on or key not in off]
    if missing:
        problems.append(
            f"route C: [PRESENT-PACKET] carries no {'/'.join(missing)} field. "
            "That is the authored-UV-scroll contract "
            "(gfx_presentation_packet_lookup_uv_scroll -> present_sched.c); "
            "without it this arm cannot tell an authored confirmation from a "
            "measured one and every assertion below reads a default")
        return problems, ""

    # The seam actually reached the binary. Without this the arm's worst
    # failure is silent: a mistyped env name produces a red arm identical to
    # the green one, and "the numbers match" would read as a pass.
    if off["uvscrollauthored"] != 0:
        problems.append(
            "route C red arm: MDKR_TEST_UV_SCROLL_AUTHORED_RATE=off still "
            f"registered {off['uvscrollauthored']} authored rates, so the "
            "opt-out never reached the binary and the two arms below are the "
            "same arm")
    if on["uvscrollauthored"] == 0 or on["uvscrollauthoredconfirm"] == 0:
        problems.append(
            "route C green arm: the authored-rate path registered "
            f"{on['uvscrollauthored']} rates and confirmed "
            f"{on['uvscrollauthoredconfirm']} of them. This route carries two "
            "fractional scrollers; a zero here means the path is not reached "
            "at all and the level's water is interpolating by accident or "
            "not at all")

    # An authored record can only be refused by the two clauses that say it
    # does not describe this batch -- never by the confirmation rule, which it
    # does not go through. Both are zero on this route in both arms, so every
    # authored lookup confirmed: the fractional scrollers' hold rate is zero.
    for label, row in (("green", on), ("red", off)):
        for clause in ("uvscrollholdambig", "uvscrollholdshape"):
            if row[clause] != 0:
                problems.append(
                    f"route C {label} arm: {clause}={row[clause]}. An authored "
                    "record is refused by nothing else, so a non-zero here "
                    "means this arm can no longer claim the fractional "
                    "scrollers never hold")

    # The defect, red. Removing the authored rate must push exactly the batches
    # it was carrying back into the phase clause -- the fail-closed refusal of a
    # displacement that alternates by one and therefore never repeats. The
    # bound is 90% rather than an identity because a batch's first published
    # tick lands in the unpublished clause in both arms.
    recovered = off["uvscrollholdphase"] - on["uvscrollholdphase"]
    if recovered < 0.9 * on["uvscrollauthoredconfirm"]:
        problems.append(
            f"route C: opting the authored rate out moved {recovered} lookups "
            f"into the phase clause against {on['uvscrollauthoredconfirm']} "
            "authored confirmations. The two should track each other: if they "
            "do not, the authored path is not what is carrying this route's "
            "fractional scrollers and this arm is measuring something else. "
            "See docs/evidence/smoothing-artifact-repro-2026-08.md section 5.2")

    # And it costs nothing elsewhere. The unpublished clause is a batch that
    # was not drawn on the previous tick, which the authored rate does not
    # speak to either way, so it must be untouched between the arms.
    if on["uvscrollholdunpub"] != off["uvscrollholdunpub"]:
        problems.append(
            "route C: the unpublished clause moved from "
            f"{off['uvscrollholdunpub']} to {on['uvscrollholdunpub']} between "
            "the arms. The authored rate says nothing about whether a batch "
            "was drawn last tick, so a change here is a side effect nothing "
            "asked for")

    return problems, (
        f"authored={on['uvscrollauthored']} "
        f"authoredconfirm={on['uvscrollauthoredconfirm']} "
        f"phasehold_on={on['uvscrollholdphase']} "
        f"phasehold_off={off['uvscrollholdphase']} "
        f"recovered={recovered} "
        f"unpub_on={on['uvscrollholdunpub']} "
        f"unpub_off={off['uvscrollholdunpub']}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=1200)
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
    rows: list[str] = []
    # Counted rather than asserted from a hand-summed figure: the published
    # claim about how much production this gate covers has to come from the
    # run that made it.
    owned_arms = 0
    owned_replays = 0
    anchored_arms = 0
    anchored_checks = 0

    routes = (
        ("A", ROUTE_A_TRACK, ROUTE_A_TICKS,
         {"MDKR_FORCE_SHIELD": ROUTE_A_SHIELD}, ROUTE_A_STAGES),
        ("B", ROUTE_B_TRACK, ROUTE_B_TICKS, {}, ROUTE_B_STAGES),
    )

    with tempfile.TemporaryDirectory(prefix="mdkr-stage-bisect-") as tmp:
        root = Path(tmp)
        try:
            for route, track, ticks, route_env, ranked in routes:
                base_out, base_dir = run(
                    binary, rom, f"route{route}-all-on", root, track, ticks,
                    route_env, args.timeout, args.verbose, dump=True)
                base_sim, base_event, base_input = hash_rows(base_out)
                if len(base_sim) != ticks:
                    failures.append(
                        f"route {route}: {len(base_sim)} [SIMHASH] rows over "
                        f"{ticks} ticks")
                summary = parse_last(base_out, "PRESENTSCHED-SUMMARY")
                replay = parse_last(base_out, "REPLAY-SUMMARY")
                packet = parse_last(base_out, "PRESENT-PACKET")
                expected_presents = ticks * PRESENTS_PER_TICK
                if summary["presents"] != expected_presents:
                    failures.append(
                        f"route {route}: {summary['presents']} presents over "
                        f"{ticks} ticks, expected {expected_presents} — the "
                        "120 Hz alpha grid is not three intermediates a tick")
                if summary["interp"] == 0:
                    failures.append(
                        f"route {route}: no interpolated replay ran, so every "
                        "arm below differences an image nothing produced")
                # Production ownership: the fail-closed branch must be
                # unreachable on a correct tree, in every stage configuration.
                problems, _ = replay_ownership(
                    base_out, f"route {route} arm all-on")
                failures.extend(problems)
                # Production anchoring: every replayed recipe measures its
                # residual against its own tick, in every stage configuration.
                problems, base_tick_checks = replay_tick_agreement(
                    base_out, f"route {route} arm all-on", True)
                failures.extend(problems)
                anchored_arms += 1
                anchored_checks += base_tick_checks
                owned_arms += 1
                owned_replays += summary["interp"]
                problems, uv_row = uv_scroll_hold_accounting(
                    base_out, f"route {route} arm all-on")
                failures.extend(problems)
                if uv_row:
                    rows.append(f"[UV-SCROLL-HOLDS] route={route} {uv_row}")
                problems, alpha_row = primitive_alpha_magnitude(
                    base_out, f"route {route} arm all-on")
                failures.extend(problems)
                if alpha_row:
                    rows.append(
                        f"[PRIM-ALPHA-MAGNITUDE] route={route} {alpha_row}")
                notes.append(
                    f"route {route} all-on: {summary['interp']} interpolated "
                    f"replays over {summary['presents']} presents, "
                    f"{len(base_sim)} authoritative ticks, uncapturedext="
                    f"{replay.get('uncapturedext')}")

                # Per-stage disposition on THIS route, stated for all seven so
                # a stage that never fires here is recorded rather than absent
                # from the output.
                for stage, _, disarm, fire in STAGES:
                    reach = packet.get(disarm, 0)
                    fired = packet.get(fire, 0)
                    if reach == 0:
                        disposition = "absent"
                    elif fired == 0:
                        disposition = "reached-never-interpolated"
                    else:
                        disposition = "fires"
                    rows.append(
                        f"[STAGE-DISPOSITION] route={route} stage={stage} "
                        f"{disarm}={reach} {fire}={fired} "
                        f"disposition={disposition} "
                        f"ranked={int(stage in ranked)}")
                    if stage in ranked and disposition != "fires":
                        failures.append(
                            f"route {route}: stage {stage} is {disposition} "
                            f"({disarm}={reach}, {fire}={fired}), so its "
                            "leave-one-out arm measures a stage that never "
                            "substituted anything rather than one that "
                            "substituted and did not show. Rank it from a "
                            "route that fires it")

                base_frames = load_frames(base_dir)
                if len(base_frames) != DUMP_FRAMES:
                    failures.append(
                        f"route {route}: all-on dumped {len(base_frames)} "
                        f"frames, expected {DUMP_FRAMES}")

                arms: list[tuple[str, tuple[str, ...]]] = [
                    ("all-off", tuple(name for name, _, _, _ in STAGES))]
                arms += [(stage, (stage,)) for stage in ranked]
                measured: dict[str, dict[str, float]] = {}
                # Kept so every leave-one-out arm can be compared against the
                # floor as well as the ceiling: a stage whose arm reproduces
                # all-off byte-for-byte is not one contributor among seven, it
                # is the master gate the other six hang off.
                alloff_frames: dict[int, bytes] = {}
                for label, disabled in arms:
                    out, frame_dir = run(
                        binary, rom, f"route{route}-{label}-off", root, track,
                        ticks, {**route_env, **stage_env(disabled)},
                        args.timeout, args.verbose, dump=True)
                    problems, _ = replay_ownership(
                        out, f"route {route} arm {label}")
                    failures.extend(problems)
                    problems, arm_tick_checks = replay_tick_agreement(
                        out, f"route {route} arm {label}",
                        "object" not in disabled)
                    failures.extend(problems)
                    anchored_arms += 1
                    anchored_checks += arm_tick_checks
                    owned_arms += 1
                    owned_replays += parse_last(
                        out, "PRESENTSCHED-SUMMARY")["interp"]
                    # THE ENV-REACHED-THE-BINARY GUARD. Every stage this arm
                    # switched off must have stopped being reached. A mistyped
                    # variable, a renamed env, or an opt-out that stopped
                    # disarming all land here instead of masquerading as a
                    # stage that ran and changed nothing.
                    arm_packet = parse_last(out, "PRESENT-PACKET")
                    for stage, env_name, disarm, _ in STAGES:
                        if stage not in disabled:
                            continue
                        if disarm not in packet or disarm not in arm_packet:
                            failures.append(
                                f"route {route} arm {label}: [PRESENT-PACKET] "
                                f"carries no {disarm} field. That is this "
                                f"stage's disarm-reach counter; without it "
                                "this guard silently passes on every arm "
                                "instead of asserting the opt-out disarmed "
                                "the stage. Check the counter name in STAGES "
                                "against present_sched.c")
                            continue
                        all_on_reach = packet[disarm]
                        if all_on_reach == 0:
                            if stage in ranked:
                                # This route's STAGE-DISPOSITION check (above)
                                # already requires ranked stages to fire, so a
                                # ranked stage reading 0 here is the real
                                # vacuity the .get(,0) shortcut used to hide:
                                # a mistyped/renamed counter that happens to
                                # collide with the legitimate "route doesn't
                                # exercise this stage" zero. Fail it by name
                                # rather than silently skipping the disarm
                                # check underneath it.
                                failures.append(
                                    f"route {route} arm {label}: all-on "
                                    f"{disarm}=0 for ranked stage {stage} -- "
                                    "the stage was never reached with "
                                    "everything armed, so this arm's zero "
                                    "proves nothing about the opt-out. "
                                    "Either the counter name is wrong or the "
                                    "recipe stopped exercising this stage")
                            # else: legitimately absent on this route (see
                            # STAGE-DISPOSITION above) -- nothing to disarm.
                            continue
                        reach = arm_packet[disarm]
                        if reach != 0:
                            failures.append(
                                f"route {route} arm {label}: {env_name}=off "
                                f"left {disarm}={reach} (all-on: "
                                f"{all_on_reach}). The opt-out did "
                                "not disarm the stage, so this arm is not a "
                                "leave-one-out of it and its frame difference "
                                "attributes nothing. Check the env name "
                                "reaches the binary")
                    sim, event, inp = hash_rows(out)
                    for name, left, right in (("[SIMHASH]", base_sim, sim),
                                              ("[EVENTHASH]", base_event,
                                               event),
                                              ("[INPUTHASH]", base_input,
                                               inp)):
                        if left != right:
                            failures.append(
                                f"route {route} arm {label}: the {name} "
                                "stream moved. These envs are presentation-"
                                "only; a stage that changes the "
                                "authoritative stream is a real defect, not "
                                "a harness artifact")
                    arm_frames = load_frames(frame_dir)
                    stats, problems = attribute(
                        base_frames, arm_frames,
                        f"route {route} arm {label}")
                    failures.extend(problems)
                    measured[label] = stats
                    if label == "all-off":
                        alloff_frames = arm_frames
                        subsumes = 1
                    else:
                        subsumes = int(all(
                            arm_frames.get(i) == alloff_frames.get(i)
                            for i in base_frames))
                    rows.append(
                        f"[STAGE-ATTRIBUTION] route={route} arm={label} "
                        f"intermediates={stats['intermediates']:.0f} "
                        f"differing={stats['differing']:.0f} "
                        f"changedfrac={stats['changedfrac']:.6f} "
                        f"meanabs={stats['meanabs']:.6f} "
                        f"reproducesalloff={subsumes}")
                    if subsumes and label != "all-off":
                        notes.append(
                            f"route {route}: stage {label} alone reproduces "
                            "the all-off arm byte-for-byte — it is not a peer "
                            "of the other six seams but the gate they hang "
                            "off, so its leave-one-out number is the whole "
                            "ceiling and cannot be added to theirs")

                if measured["all-off"]["differing"] == 0:
                    failures.append(
                        f"route {route}: the all-off arm reproduces all-on on "
                        "every intermediate frame, so this route measures "
                        "nothing and every zero below is vacuous")
                ceiling = measured["all-off"]["changedfrac"]
                ranking = sorted(
                    ranked, key=lambda name: measured[name]["changedfrac"],
                    reverse=True)
                counters = {name: fire for name, _, _, fire in STAGES}
                for position, stage in enumerate(ranking, start=1):
                    share = (measured[stage]["changedfrac"] / ceiling
                             if ceiling else 0.0)
                    rows.append(
                        f"[STAGE-RANK] route={route} rank={position} "
                        f"stage={stage} "
                        f"changedfrac={measured[stage]['changedfrac']:.6f} "
                        f"ceilingshare={share:.4f}")
                    if measured[stage]["differing"] == 0:
                        notes.append(
                            f"route {route}: stage {stage} FIRED "
                            f"({counters[stage]}="
                            f"{packet.get(counters[stage], 0)}) but changed "
                            "no intermediate pixel — a candidate for artifact "
                            "innocence, not for removal")

            # ---- the uncaptured-external fail-closed arm ------------------
            control_out, _ = run(
                binary, rom, "failclosed-control", root, ROUTE_A_TRACK,
                FAILCLOSED_TICKS, {}, args.timeout, args.verbose, dump=False)
            refuse_out, _ = run(
                binary, rom, "failclosed-refuse", root, ROUTE_A_TRACK,
                FAILCLOSED_TICKS,
                {"MDKR_TEST_UNCAPTURED_EXTERNAL": "1",
                 "MDKR_INTERNAL_TEST_TOKEN": INTERNAL_TEST_TOKEN},
                args.timeout, args.verbose, dump=False)
            untokened_out, _ = run(
                binary, rom, "failclosed-untokened", root, ROUTE_A_TRACK,
                FAILCLOSED_TICKS,
                {"MDKR_TEST_UNCAPTURED_EXTERNAL": "1"},
                args.timeout, args.verbose, dump=False)
            # ---- the fractional-scroller arm ------------------------------
            fractional_on, _ = run(
                binary, rom, "routeC-authored-on", root, ROUTE_C_TRACK,
                ROUTE_C_TICKS, {}, args.timeout, args.verbose, dump=False)
            fractional_off, _ = run(
                binary, rom, "routeC-authored-off", root, ROUTE_C_TRACK,
                ROUTE_C_TICKS,
                {"MDKR_TEST_UV_SCROLL_AUTHORED_RATE": "off"},
                args.timeout, args.verbose, dump=False)
        except RuntimeError as error:
            print(f"check_smoothing_stage_bisection: FAIL\n  - {error}")
            return 1

        control = parse_last(control_out, "REPLAY-SUMMARY")
        control_sched = parse_last(control_out, "PRESENTSCHED-SUMMARY")
        refuse = parse_last(refuse_out, "REPLAY-SUMMARY")
        refuse_sched = parse_last(refuse_out, "PRESENTSCHED-SUMMARY")
        untokened = parse_last(untokened_out, "REPLAY-SUMMARY")
        for label, row in (("control", control), ("forced-miss", refuse),
                           ("untokened", untokened)):
            missing = [key for key in REPLAY_STAT_CONTRACT if key not in row]
            if missing:
                print("check_smoothing_stage_bisection: FAIL\n  - "
                      f"fail-closed {label}: [REPLAY-SUMMARY] carries no "
                      f"{'/'.join(missing)} field — the uncaptured-external "
                      "stat contract is gone and every assertion below it "
                      "would read a default")
                return 1

        if control["uncapturedrefusals"] != 0 or control["uncapturedext"] != 0:
            failures.append(
                "fail-closed control: production resolved "
                f"{control['uncapturedext']} uncaptured externals")
        if refuse["uncapturedrefusals"] == 0:
            failures.append(
                "fail-closed arm: the forced-miss seam refused no walk, so "
                "the refusal branch is unproven")
        if refuse_sched["interp"] >= control_sched["interp"] // 4:
            failures.append(
                "fail-closed arm: interpolated presents held at "
                f"{refuse_sched['interp']} against a control of "
                f"{control_sched['interp']} — the walks kept drawing over an "
                "external they could not prove they owned")
        if refuse_sched["stale"] <= control_sched["stale"]:
            failures.append(
                "fail-closed arm: refused walks did not become held authored "
                f"images (stale={refuse_sched['stale']} vs control "
                f"{control_sched['stale']}); a refusal that does not hold the "
                "front image is a dropped frame, not a safe one")
        if untokened["uncapturedrefusals"] != 0:
            failures.append(
                "fail-closed arm: MDKR_TEST_UNCAPTURED_EXTERNAL armed without "
                "the versioned token — an adversarial seam must not be "
                "reachable from a bare environment variable")
        problems, fractional_row = fractional_scroller(
            fractional_on, fractional_off)
        failures.extend(problems)
        if fractional_row:
            rows.append(f"[UV-SCROLL-AUTHORED] route=C {fractional_row}")
        rows.append(
            f"[UNCAPTURED-OWNERSHIP] arms={owned_arms} "
            f"interpolated_replays={owned_replays} uncapturedext=0")
        rows.append(
            f"[TICK-ANCHORING] arms={anchored_arms} "
            f"recipes_checked={anchored_checks} ownertickmismatch=0")
        rows.append(
            f"[UNCAPTURED-EXTERNAL] control_ext={control['uncapturedext']} "
            f"control_interp={control_sched['interp']} "
            f"forced_ext={refuse['uncapturedext']} "
            f"forced_refusals={refuse['uncapturedrefusals']} "
            f"forced_interp={refuse_sched['interp']} "
            f"forced_stale={refuse_sched['stale']} "
            f"untokened_refusals={untokened['uncapturedrefusals']}")

    for row in rows:
        print(row)
    for note in notes:
        print(f"  . {note}")
    if failures:
        print("check_smoothing_stage_bisection: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_smoothing_stage_bisection: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
