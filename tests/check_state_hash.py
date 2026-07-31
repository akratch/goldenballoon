#!/usr/bin/env python3
"""Authoritative-state hash determinism and first presentation-invariance arms.

Phase 1 of the fidelity architecture (docs/
FIXED_SIMULATION_HIGH_RATE_PRESENTATION_SPEC_2026-07-28.md §12) rests on one
instrument: a per-tick versioned hash of authoritative state (`[SIMHASH]`
rows, MDKR_STATE_HASH=1). Every later gate — render purity, the
presentation-rate matrix, catch-up equivalence — is "this stream is
identical" between two schedules. This check anchors the instrument:

1. **Determinism:** two identical runs produce byte-identical streams.
2. **Presentation invariance (first slice):** a different window size and the
   other renderer backend must not change one bit of authoritative state.
3. **Positive control:** the `MDKR_RNGSEED=legacy` arm must DIVERGE — the
   hash provably sees the RNG, or the identity assertions above are
   vacuous.
4. **Field-set control (v2):** the hash must provably read the fields v1
   could not. See `HASH_VERSION` below.

This gate runs the **v2** field set (`MDKR_STATE_HASH=2`). v1 covered
y_rotation ONLY per object, which was a measured blind spot rather than a
theoretical one: on level 37 a line particle's x/z rotation diverged
between two runs of the same binary at tick 3378 and v1 could not see it
until the drift reached y_rotation at 3381 (2a4f281). v2 adds full
rotation, transform flags, velocity, animFrame, segment/header ids, and
the whole particle integrator including angularVelocity — see the field
table in platform/sim_hash.c. v1 stays selectable (`MDKR_STATE_HASH=1`)
so an archived stream can still be compared against, and it is
byte-for-byte what it always was.

Always muted + headless per tests/README.md. Exit 0 = pass.
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
# The field set every arm below runs. Promoting this is deliberate: a change
# here changes what every "byte-identical" claim in this file MEANS.
HASH_VERSION = "2"
# Where the field-set control flips a bit. Any in-race tick works; this one
# is late enough that the object list is populated and moving.
PERTURB_TICK = 2000


def run_arm(binary: Path, rom: Path, label: str, root: Path,
            renderer: str, window: str, extra_env: dict[str, str],
            timeout: int, verbose: bool,
            version: str = HASH_VERSION) -> list[str]:
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_STATE_HASH=version,
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK="5",
        MDKR_RENDERER=renderer,
        MDKR_SAVE_DIR=str(save_dir),
    )
    env.update(extra_env)
    command = [
        str(binary), "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", window,
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    if process.returncode != 0:
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n"
            f"{(process.stdout or '')[-3000:]}")
    rows = [line for line in (process.stdout or "").splitlines()
            if line.startswith("[SIMHASH]")]
    if len(rows) != FRAMES:
        raise RuntimeError(
            f"{label}: expected {FRAMES} [SIMHASH] rows, got {len(rows)}")
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build-rel")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-state-hash-") as tmp:
        root = Path(tmp)
        try:
            base = run_arm(binary, rom, "base", root, "gl", "320x240",
                           {}, args.timeout, args.verbose)
            rerun = run_arm(binary, rom, "rerun", root, "gl", "320x240",
                            {}, args.timeout, args.verbose)
            bigwin = run_arm(binary, rom, "bigwin", root, "gl", "640x480",
                             {}, args.timeout, args.verbose)
            webgpu = run_arm(binary, rom, "webgpu", root, "webgpu",
                             "320x240", {}, args.timeout, args.verbose)
            control = run_arm(binary, rom, "rng-control", root, "gl",
                              "320x240", {"MDKR_RNGSEED": "legacy"},
                              args.timeout, args.verbose)
            # Field-set control. MDKR_TEST_HASH_PERTURB flips one bit of one
            # object's x_rotation at the named tick, hashes, and reverts it
            # before anything else runs, so the SIMULATION is untouched and
            # only the hash's view of it changes. Run under both versions.
            perturb = {"MDKR_TEST_HASH_PERTURB": str(PERTURB_TICK)}
            v2_perturbed = run_arm(binary, rom, "v2-perturb", root, "gl",
                                   "320x240", perturb, args.timeout,
                                   args.verbose)
            v1_base = run_arm(binary, rom, "v1-base", root, "gl", "320x240",
                              {}, args.timeout, args.verbose, version="1")
            v1_perturbed = run_arm(binary, rom, "v1-perturb", root, "gl",
                                   "320x240", perturb, args.timeout,
                                   args.verbose, version="1")
        except RuntimeError as error:
            print(f"check_state_hash: FAIL\n  - {error}")
            return 1

    if base != rerun:
        first = next(i for i, (a, b) in enumerate(zip(base, rerun)) if a != b)
        failures.append(
            f"identical runs diverged at tick {first}: "
            f"{base[first]} vs {rerun[first]}")
    if base != bigwin:
        first = next(i for i, (a, b) in enumerate(zip(base, bigwin)) if a != b)
        failures.append(
            "window size changed authoritative state at tick "
            f"{first}: {base[first]} vs {bigwin[first]}")
    if base != webgpu:
        first = next(i for i, (a, b) in enumerate(zip(base, webgpu)) if a != b)
        failures.append(
            "renderer backend changed authoritative state at tick "
            f"{first}: {base[first]} vs {webgpu[first]}")
    if base == control:
        failures.append(
            "positive control failed: the legacy-RNG arm produced an "
            "identical stream — the hash cannot see the RNG")

    # Field-set control: v2 must SEE x_rotation and v1 must NOT. Both halves
    # matter. The v2 half is what stops this gate silently degrading to a
    # narrower field set than it advertises; the v1 half is the demonstration
    # that the widening was necessary, kept executable rather than described.
    v2_diffs = [i for i, (a, b) in enumerate(zip(base, v2_perturbed))
                if a != b]
    if v2_diffs != [PERTURB_TICK]:
        failures.append(
            f"field-set control failed: flipping one bit of x_rotation at "
            f"tick {PERTURB_TICK} changed the v2 stream at ticks "
            f"{v2_diffs[:8] or 'NONE'} — expected exactly [{PERTURB_TICK}]. "
            "No difference at all means v2 is not reading x_rotation (the "
            "v1 blind spot has been reintroduced); a difference at any OTHER "
            "tick means the control did not revert cleanly and every other "
            "arm here is comparing perturbed state")
    if v1_base != v1_perturbed:
        first = next(i for i, (a, b) in enumerate(zip(v1_base, v1_perturbed))
                     if a != b)
        failures.append(
            f"v1 reference arm is not v1: the x_rotation perturbation moved "
            f"the v1 stream at tick {first}, but v1 does not hash x_rotation. "
            "Either MDKR_STATE_HASH=1 no longer selects the archived field "
            "set, or the perturbation is leaking into the simulation")

    # Process-nondeterminism arms. Arm 1 above proves determinism on ONE route
    # (level 5). That is not the same claim as "this binary is deterministic",
    # and the gap was load-bearing: an all-levels sweep found three levels where
    # the same binary disagreed with ITSELF, each because an authoritative field
    # was seeded from host memory that moves between processes.
    #
    # Both mechanisms are level-shaped, so a single route cannot see either one:
    #   41 — Smokey 1 has no BHV_SETUP_POINT matching the entrance the route
    #        uses, so track_setup_racers() read the racer's spawn heading out of
    #        an uninitialised stack slot (spawnAngle[], the one member of its
    #        quartet the zeroing loop skipped).
    #   37 — Wizpig 1 is the only level in 0..65 that rains, so it is the only
    #        one that emits PARTICLE_KIND_LINE from WEATHER, and
    #        create_line_particle() is the only particle constructor that left
    #        trans.rotation and angularVelocity holding recycled pool bytes —
    #        host pointer fragments on LP64.
    #   11 — added when the hash was widened to v2. v1 could only see
    #        y_rotation, and the SAME constructor also skips
    #        setup_particle_position/setup_particle_velocity, so `localPos` and
    #        `velocity` were recycled bytes too. Under v2 that failed its own
    #        control on TEN levels (11, 23, 25, 26, 27, 35, 37, 38, 53, 55) —
    #        line particles are emitted by vehicles, not just by rain, so the
    #        blast radius was far wider than 37. Level 11 is kept as the arm
    #        because it diverges earliest (tick 2729) and is not level 37, so
    #        the two arms cannot both go green for the same reason.
    # These are re-run rather than compared to a golden hash on purpose: the
    # assertion is self-consistency, so it stays valid as the simulation
    # legitimately evolves and cannot rot into a stale-constant check.
    with tempfile.TemporaryDirectory(prefix="mdkr-state-hash-levels-") as tmp:
        levels_root = Path(tmp)
        for level, why in ((41, "no matching setup point"),
                           (37, "rain/line particles"),
                           (11, "vehicle line particles, v2 fields")):
            env = {"MDKR_LOAD_TRACK": str(level)}
            try:
                first = run_arm(binary, rom, f"lvl{level}-a", levels_root,
                                "gl", "320x240", env, args.timeout,
                                args.verbose)
                second = run_arm(binary, rom, f"lvl{level}-b", levels_root,
                                 "gl", "320x240", env, args.timeout,
                                 args.verbose)
            except RuntimeError as error:
                failures.append(f"level {level} ({why}): {error}")
                continue
            if first != second:
                tick = next(i for i, (a, b) in enumerate(zip(first, second))
                            if a != b)
                failures.append(
                    f"level {level} ({why}) is not process-deterministic: two "
                    f"runs of the same binary diverged at tick {tick}: "
                    f"{first[tick]} vs {second[tick]}")

    if failures:
        print("check_state_hash: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    diverge = next((i for i, (a, b) in enumerate(zip(base, control))
                    if a != b), None)
    print(
        f"check_state_hash: PASS (v{HASH_VERSION} field set) — "
        f"{FRAMES} ticks byte-identical across rerun, 640x480, and WebGPU; "
        f"levels 41, 37 and 11 self-identical across two processes each; "
        f"legacy-RNG control diverges at tick {diverge}; field-set control "
        f"moves v2 at tick {PERTURB_TICK} only and leaves v1 untouched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
