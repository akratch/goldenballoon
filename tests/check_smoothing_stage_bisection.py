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
* **The instrument is not vacuous.** all-off must differ from all-on on at
  least one intermediate frame, and every stage must show a non-zero
  ``[PRESENT-PACKET]`` override counter on the route it is ranked from.
  A stage that never fired is reported as UNEXERCISED, never as innocent.

The uncaptured-external arm
---------------------------

The same run shape carries the evidence for the retained-pointer fail-closed
path. ``dkr_retain_resolved_pointer`` refuses an interpolated walk that
resolves a non-arena dependency with no retained copy. On a correct tree that
branch is unreachable, so the production arms assert ``uncapturedext=0`` and
``MDKR_TEST_UNCAPTURED_EXTERNAL`` (token-gated) forces every external lookup to
miss and asserts the walks refuse — holding the authored image — rather than
reading live memory.

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

# (name, opt-out env, the [PRESENT-PACKET] counter that proves the stage
# actually fired on a route). The counters are all *override* counts, not
# registration counts: a registration only proves the walk saw the data, while
# an override proves the replay substituted an interpolated value for it.
STAGES = (
    ("object", "MDKR_TEST_OBJECT_INTERPOLATION", "matrixoverride"),
    ("deformation", "MDKR_TEST_DEFORMATION_INTERPOLATION", "deformoverride"),
    ("vertex_color", "MDKR_TEST_VERTEX_COLOR_INTERPOLATION", "coloroverride"),
    ("particle", "MDKR_TEST_PARTICLE_INTERPOLATION",
     "particledeformoverride"),
    ("primitive_alpha", "MDKR_TEST_PRIMITIVE_ALPHA_INTERPOLATION",
     "primalphaoverride"),
    ("effect", "MDKR_TEST_EFFECT_INTERPOLATION", "effectoverride"),
    ("uv_scroll", "MDKR_TEST_UV_SCROLL_INTERPOLATION", "uvscrolloverride"),
)

# A stage is ranked only from a route that actually fires it. Route A never
# interpolates a world-space particle mesh -- it registers particle vertices
# and then holds every one of them -- so ranking `particle` there would report
# an absent stage as an innocent one, which is the single most misleading thing
# an attribution table can do.
ROUTE_A_STAGES = tuple(name for name, _, _ in STAGES if name != "particle")
ROUTE_B_STAGES = ("particle",)

STAGE_OFF = "off"


def stage_env(disabled: tuple[str, ...]) -> dict[str, str]:
    return {env: STAGE_OFF for name, env, _ in STAGES if name in disabled}


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
                if replay.get("uncapturedext", 0) != 0:
                    failures.append(
                        f"route {route}: uncapturedext="
                        f"{replay['uncapturedext']} — an interpolated walk "
                        "resolved a non-arena dependency with no retained "
                        "copy. That is an uncaptured external, and before the "
                        "fail-closed change it would have been read live")
                notes.append(
                    f"route {route} all-on: {summary['interp']} interpolated "
                    f"replays over {summary['presents']} presents, "
                    f"{len(base_sim)} authoritative ticks, uncapturedext="
                    f"{replay.get('uncapturedext')}")

                for stage, _, counter in STAGES:
                    value = packet.get(counter, 0)
                    rows.append(
                        f"[STAGE-COVERAGE] route={route} stage={stage} "
                        f"counter={counter} value={value} "
                        f"ranked={int(stage in ranked)}")
                    if stage in ranked and value == 0:
                        failures.append(
                            f"route {route}: stage {stage} is UNEXERCISED — "
                            f"{counter}=0, so its leave-one-out arm measures "
                            "an absent stage rather than an innocent one. "
                            "Rank it from a route that fires it")

                base_frames = load_frames(base_dir)
                if len(base_frames) != DUMP_FRAMES:
                    failures.append(
                        f"route {route}: all-on dumped {len(base_frames)} "
                        f"frames, expected {DUMP_FRAMES}")

                arms: list[tuple[str, tuple[str, ...]]] = [
                    ("all-off", tuple(name for name, _, _ in STAGES))]
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
                counters = {name: counter for name, _, counter in STAGES}
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
        except RuntimeError as error:
            print(f"check_smoothing_stage_bisection: FAIL\n  - {error}")
            return 1

        control = parse_last(control_out, "REPLAY-SUMMARY")
        control_sched = parse_last(control_out, "PRESENTSCHED-SUMMARY")
        refuse = parse_last(refuse_out, "REPLAY-SUMMARY")
        refuse_sched = parse_last(refuse_out, "PRESENTSCHED-SUMMARY")
        untokened = parse_last(untokened_out, "REPLAY-SUMMARY")

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
