#!/usr/bin/env python3
"""A racer must never stay embedded in a collision-meshed object.

Why this exists
---------------
`func_80017A18()` -- the per-facet object-model collision test, and the only
thing that makes a collision-meshed object solid -- is ONE-SIDED by
construction.  It fires only on `sum1 >= -0.1 && sum2 < -0.1`: the point started
outside the facet's plane and ended inside it.  A point that is ALREADY inside
when the tick begins satisfies neither half, so every facet rejects it and the
object stops pushing entirely.  Nothing else ejects it either -- `resolve_collisions()`
only ever sees terrain -- so the point stays inside the mesh for as long as the
object exists.

`resolve_collisions()` (game/src/hasm/collision.c, "Step 2: ensure object is
fully pushed out from underneath") has solved exactly this for terrain since the
ROM.  The object path never had the equivalent, and could not have needed it
before wave "objcoll": until that wave un-stubbed `func_80017A18`, every
collision-meshed object was intangible.  `docs/open-items/collision.md` recorded
the exposure at the time -- *"Wedging is bounded but not impossible: the matched
body's own `counter > 10` bail resets the racer to `x2/y2/z2`"* -- and this is
the pass that closes it, plus the instrument that keeps it closed.

Reported by JappaWakka (goldenballoon #32) as two symptoms: a kart left "45
degrees into the floor unable to move properly" after an angled hit on a locked
balloon door, and a kart stuck inside a palm tree.

What this asserts
-----------------
  1. **MECHANISM, fixed arm** -- with the recovery pass live, a seeded embedded
     point is ejected: `[OBJRECOVER] points >= 1`, and the independent arrival
     probe never reports an embedded tick (`embedded == 0`).
  2. **MECHANISM, control arm (positive control)** -- with `MDKR_OBJCOLL=norecover`
     the SAME seed on the SAME route is NOT ejected: `points == 0` and
     `embedded >= 1`.  This is what makes assertion 1 falsifiable.  The fix's
     failure mode is silence -- a point that is never ejected is simply never
     mentioned again by the one-sided walk -- so the fixed arm's `embedded == 0`
     proves nothing without the control arm's `embedded >= 1` beside it, from the
     same binary, same route, same seed frame.
  3. **ESCAPE** -- the fixed arm must complete its post-impact escape: brake,
     reverse, and drive back out to both return waypoints.  A kart standing
     still with the throttle held into a shut door is CORRECT behaviour, so only
     a commanded escape attempt (the `H`/`R` route steps) can distinguish that
     from a wedge.
  4. **NO FREEZE** -- while a forward waypoint step is steering, no racer may
     hold a bit-exact position for more than `FREEZE_LIMIT` ticks.  Bit-exact is
     the point: `func_80017A18`'s `counter > 10` bail resets a point to its
     ORIGIN, so a pinned kart repeats a position exactly rather than drifting.
     The window deliberately excludes the `H`/`R` steps, which stand still on
     purpose.
  5. **PITCH** -- `[GRND] xrot` must return below `PITCH_LIMIT` within
     `PITCH_WINDOW` ticks of the last `[OBJCOLL]` hit.  This is the direct
     observable for the reporter's first symptom; `func_80054FD0()` levels pitch
     from wheel contact only on ticks where no terrain wall was touched, so a
     kart that never separates never levels.

What this does NOT assert
-------------------------
It does not claim the reported wedge is reproduced.  It is not: four measured
approach angles into the hub's Dino Domain door leaf -- head-on plus three
glancing -- all separated correctly and all escaped when steered away.  The
embedded state is reachable in play (the reporter reached it) but not on a
schedule a gate can assert on, which is why assertions 1-2 seed it.  Assertions
3-5 are the route-driven half and would catch a regression that made the ordinary
case wedge; they are not evidence that the original report is cured.

The seed is a TEST MUTATION and gated as one, behind the versioned capability
`MDKR_INTERNAL_TEST_TOKEN=mdkr64-objcoll-wedge-v1` -- the same pattern
`present_sched.c` uses for its replay arms.  It moves wheel points, which no
production path does.

Usage:
    tests/check_object_wedge.py [--build build] [--rom baserom.us.v80.z64] [-v]

Always runs muted + headless (`MDKR_AUDIO=0` and `--headless-frames`), per
CONTRIBUTING.md's audio-safety rule.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, save_env

# Timber's Island, loaded directly. The hub is where the collision-meshed
# progression gates are: MDKR_OBJDUMP reports 8 doors here against 0 on a race
# track, and the door leaf below is the 1-balloon Dino Domain gate at
# (-4105, 260, 2435), the same leaf tests/check_door_blocks.py drives.
HUB_LEVEL = 0
SCRIPT = "tests/input_scripts/race_full_3lap_tt.txt"
FRAMES = 5000

# Drive at the leaf, brake, reverse off it, then leave by two waypoints. The two
# return waypoints are what make "escaped" mean travelled, not merely twitched:
# the first is 800 units away and the second 2400.
#
# H and R are frame-counted, not position-counted. Reverse steering is mirrored
# so the nose swings toward the next point, which means the kart travels AWAY
# from it -- a "reverse until you arrive" step is unsatisfiable by construction
# (measured: stalled at 764 units and growing).
ROUTE = "0:-1004,946:-3336,2111:-4105,2435:H60:R240:-3336,2111:-1004,946"
WP_RADIUS = 90

# The route's own step indices, so a route edit cannot silently un-assert this.
ESCAPE_STEPS = (3, 4, 5, 6)   # H60, R240, and the two return waypoints
FINAL_STEP = 6

# The seed frame. The kart first contacts the leaf around tick 2908 on this
# route; arming a few ticks early lets the seed fire on the first real contact
# rather than requiring a frame-perfect guess, since the hook waits for one.
SEED_FRAME = 2905

# A bit-exact repeat this long, while a forward step is steering, is a wedge.
# 60 ticks is two authored seconds -- far longer than the ~18-tick exponential
# settle measured on a legitimate head-on door stop.
FREEZE_LIMIT = 60

# Pitch. 0x2000 is 45 degrees, the reporter's figure; the clamp in
# func_80054FD0() is 0x3400. 0x0800 (about 11 degrees) is the "levelled again"
# threshold, and 300 ticks the window to get there.
PITCH_LIMIT = 0x0800
PITCH_WINDOW = 300

PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+).*racer x=(-?[\d.]+) y=(-?[\d.]+) z=(-?[\d.]+)"
)
GRND_RE = re.compile(
    r"\[GRND\] frame=(\d+) pi=(-?\d+) ri=(-?\d+) gw=(\d+) surf=\S+ "
    r"xrot=(-?\d+) zrot=(-?\d+) blk=(\d+)"
)
HIT_RE = re.compile(r"\[OBJCOLL\] hit #\d+ frame=(\d+)")
STEP_RE = re.compile(r"drive: level=(\d+) step (\d+): (.+?) @frame~(\d+)")
RECOVER_RE = re.compile(
    r"\[OBJRECOVER\] points=(\d+) iters=(\d+) embedded=(-?\d+)"
)
EMBED_RE = re.compile(r"\[OBJEMB\] frame=(\d+) embedded=(\d+) total=(\d+)")
LEVEL_RE = re.compile(r"level_load: levelId=(\d+) .*@frame~(\d+)")


def run(binary, rom, save_dir, norecover, verbose):
    """One arm. Both arms come from ONE binary; only MDKR_OBJCOLL differs."""
    env = save_env(
        dict(
            os.environ,
            MDKR_AUDIO="0",       # belt-and-braces; --headless-frames is the guarantee
            MDKR_PRESENT_RATE="original",
            MDKR_SIMULATION_CADENCE="original",
            MDKR_SYNTH_FIELDS="2",  # one authored gameplay ticket per opportunity
            MDKR_TRACE="1",         # emit [PACE], [GRND], drive: and [OBJEMB]
            MDKR_LOAD_TRACK=str(HUB_LEVEL),
            MDKR_DRIVE_ROUTE=ROUTE,
            MDKR_DRIVE_WPR=str(WP_RADIUS),
            # The seed and the arrival probe are both inert without this exact
            # versioned capability.
            MDKR_INTERNAL_TEST_TOKEN="mdkr64-objcoll-wedge-v1",
            MDKR_TEST_OBJCOLL_EMBED=str(SEED_FRAME),
        ),
        save_dir,
    )
    # `trace` is needed in BOTH arms (the pitch assertion anchors on the last hit
    # frame), and the seam composes, so the control arm asks for both.
    env["MDKR_OBJCOLL"] = "norecover+trace" if norecover else "trace"

    cmd = [binary, "--headless-frames", str(FRAMES),
           "--input-script", SCRIPT, "--rom", rom]
    if verbose:
        prefix = "MDKR_OBJCOLL=norecover " if norecover else ""
        print(f"  $ {prefix}" + " ".join(cmd))

    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return proc.returncode, proc.stdout + proc.stderr


def parse(out):
    """Everything the assertions read, in one pass."""
    pos, grnd, steps = {}, {}, {}
    hits, embeds = [], []
    for line in out.splitlines():
        m = PACE_RE.search(line)
        if m:
            pos[int(m.group(1))] = (m.group(2), m.group(3), m.group(4))
        m = GRND_RE.search(line)
        if m:
            grnd.setdefault(int(m.group(1)), []).append(
                (int(m.group(2)), int(m.group(3)), int(m.group(5)), int(m.group(7)))
            )
        m = HIT_RE.search(line)
        if m:
            hits.append(int(m.group(1)))
        m = EMBED_RE.search(line)
        if m:
            embeds.append(int(m.group(1)))
        m = STEP_RE.search(line)
        if m and int(m.group(1)) == HUB_LEVEL:
            steps[int(m.group(2))] = (m.group(3), int(m.group(4)))
    recover = None
    m = RECOVER_RE.search(out)
    if m:
        recover = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    levels = {int(m.group(1)): int(m.group(2)) for m in LEVEL_RE.finditer(out)}
    return {
        "pos": pos, "grnd": grnd, "steps": steps, "hits": hits,
        "embeds": embeds, "recover": recover, "levels": levels,
    }


def longest_freeze(pos, grnd, lo, hi):
    """Longest run of BIT-EXACT repeated position inside [lo, hi).

    Exact rather than epsilon-based on purpose: `func_80017A18`'s bail resets a
    point to its origin, so a pinned kart repeats a position identically. A
    legitimate slow crawl drifts in the low bits and is not this.

    Ticks where `gRacerInputBlocked` is set are skipped, and that exclusion is
    load-bearing rather than a convenience. `process_onscreen_textbox()` calls
    `disable_racer_input()` every tick the "you need N balloons" message is up,
    which zeroes the stick and buttons and decays velocity by 0.65 per tick to a
    hard stop -- so the kart sits at ONE bit-exact position for the whole message
    even while the route commands a reverse. Measured: 94 such ticks on this
    route. That is the game holding the player still, not geometry trapping them,
    and a detector that cannot tell those apart fires on correct behaviour.
    """
    blocked = {
        f for f, rows in grnd.items()
        if any(b for _pi, _ri, _x, b in rows)
    }
    frames = [f for f in sorted(pos) if lo <= f < hi and f not in blocked]
    best = (0, None)
    run_len, start = 0, None
    for i in range(1, len(frames)):
        if pos[frames[i]] == pos[frames[i - 1]]:
            if start is None:
                start = frames[i - 1]
            run_len += 1
            if run_len > best[0]:
                best = (run_len, start)
        else:
            run_len, start = 0, None
    return best


def check_crash(tag, rc, out, failures):
    if rc != 0:
        failures.append(f"[{tag}] exit code {rc}")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in out:
            line = next((l for l in out.splitlines() if marker in l), marker)
            failures.append(f"[{tag}] {marker}: {line.strip()}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures = []
    summary = []

    # ---- arm 1: the fix ---------------------------------------------------
    with tempfile.TemporaryDirectory(prefix="mdkr-wedge-fixed-") as save_dir:
        rc, out = run(binary, args.rom, save_dir, False, args.verbose)
    fixed = parse(out)
    check_crash("fixed", rc, out, failures)

    # The route has to have actually run, or every assertion below is vacuous.
    if HUB_LEVEL not in fixed["levels"]:
        failures.append(
            f"[fixed] never loaded the hub (levelId {HUB_LEVEL}); nothing below "
            f"means anything. levels seen: {sorted(fixed['levels'])}"
        )
    if not fixed["hits"]:
        failures.append(
            "[fixed] zero [OBJCOLL] hits -- the kart never touched a "
            "collision-meshed object, so the route missed the door leaf"
        )

    rec = fixed["recover"]
    if rec is None:
        failures.append("[fixed] no [OBJRECOVER] summary line")
    else:
        points, iters, embedded = rec
        summary.append(
            f"  fixed     : recovered points={points} iters={iters} "
            f"embedded_ticks={embedded} objcoll_hits={len(fixed['hits'])}"
        )
        # 1. MECHANISM.
        if points < 1:
            failures.append(
                f"[fixed] the recovery pass never fired (points={points}) -- "
                f"the seed at tick {SEED_FRAME} did not produce an embedded "
                f"point, so this arm proves nothing. Check that the seed depth "
                f"stays inside the recovery band (one collision radius + 3)."
            )
        if embedded != 0:
            failures.append(
                f"[fixed] {embedded} tick(s) began with a wheel point already "
                f"inside a collision mesh and unrecoverable by the one-sided "
                f"walk. With the recovery pass live this must be 0."
            )

    # 3. ESCAPE.
    for step in ESCAPE_STEPS:
        if step not in fixed["steps"]:
            failures.append(
                f"[fixed] escape step {step} never completed -- the kart could "
                f"not brake, reverse and drive back out after the door impact. "
                f"Steps completed: {sorted(fixed['steps'])}"
            )
    if FINAL_STEP in fixed["steps"]:
        summary.append(
            f"  fixed     : escaped -- final waypoint reached @tick "
            f"{fixed['steps'][FINAL_STEP][1]}"
        )

    # 4. NO FREEZE, over the forward-driving window only.
    if FINAL_STEP in fixed["steps"] and 4 in fixed["steps"]:
        lo = fixed["steps"][4][1]          # end of the commanded reverse
        hi = fixed["steps"][FINAL_STEP][1] # arrival at the last waypoint
        run_len, start = longest_freeze(
            fixed["pos"], fixed["grnd"], lo, hi)
        summary.append(
            f"  fixed     : longest bit-exact freeze while steering = "
            f"{run_len} ticks (limit {FREEZE_LIMIT}), window {lo}..{hi}"
        )
        if run_len > FREEZE_LIMIT:
            failures.append(
                f"[fixed] held a bit-exact position for {run_len} ticks from "
                f"tick {start} while a forward waypoint step was steering "
                f"elsewhere (limit {FREEZE_LIMIT}). That is a wedge: a kart "
                f"blocked by geometry drifts, a pinned one repeats exactly."
            )

    # 5. PITCH.
    if fixed["hits"] and fixed["grnd"]:
        last_hit = max(fixed["hits"])
        levelled = [
            f for f in sorted(fixed["grnd"])
            if f >= last_hit
            and all(abs(x) < PITCH_LIMIT for _p, _r, x, _b in fixed["grnd"][f])
        ]
        if not levelled or levelled[0] > last_hit + PITCH_WINDOW:
            where = levelled[0] if levelled else "never"
            failures.append(
                f"[fixed] pitch never returned below {PITCH_LIMIT:#x} within "
                f"{PITCH_WINDOW} ticks of the last object collision (tick "
                f"{last_hit}); first levelled tick: {where}. A racer that "
                f"cannot level its pitch is the reporter's '45 degrees into "
                f"the floor'."
            )
        else:
            summary.append(
                f"  fixed     : pitch levelled {levelled[0] - last_hit} ticks "
                f"after the last object collision (window {PITCH_WINDOW})"
            )

    # ---- arm 2: the positive control -------------------------------------
    with tempfile.TemporaryDirectory(prefix="mdkr-wedge-ctl-") as save_dir:
        rc, out = run(binary, args.rom, save_dir, True, args.verbose)
    ctl = parse(out)
    check_crash("norecover", rc, out, failures)

    rec = ctl["recover"]
    if rec is None:
        failures.append("[norecover] no [OBJRECOVER] summary line")
    else:
        points, iters, embedded = rec
        summary.append(
            f"  norecover : recovered points={points} iters={iters} "
            f"embedded_ticks={embedded} objcoll_hits={len(ctl['hits'])}"
        )
        # 2. POSITIVE CONTROL.
        if points != 0:
            failures.append(
                f"[norecover] the recovery pass ran anyway (points={points}) -- "
                f"MDKR_OBJCOLL=norecover no longer disables it, so the control "
                f"arm is not a control"
            )
        if embedded < 1:
            failures.append(
                f"[norecover] POSITIVE CONTROL BROKEN: embedded={embedded}. "
                f"Without the recovery pass the seeded point must stay inside "
                f"the mesh and the arrival probe must see it. Until this fires, "
                f"the fixed arm's embedded==0 is unfalsifiable -- it would also "
                f"be satisfied by a seed that never embedded anything."
            )

    # ---- verdict ----------------------------------------------------------
    if failures:
        print("FAIL: object wedge check")
        for line in summary:
            print(line)
        for f in failures:
            print(f"  - {f}")
        return 1
    print("PASS: object wedge check")
    for line in summary:
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
