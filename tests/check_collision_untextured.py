#!/usr/bin/env python3
"""An untextured terrain batch is collidable with SURFACE_DEFAULT, not skipped.

The divergence this locks down
------------------------------
`generate_collision_candidates()` (game/src/hasm/collision.c) decides which
terrain triangles `resolve_collisions()` will test.  For a batch whose
`textureIndex` is 255 -- no texture, and the surface type is normally read out of
the texture -- the upstream NON_MATCHING C body did

    if (batches[batchIndex].textureIndex == 255) { continue; }   /* WRONG */

which drops the whole batch, making untextured level geometry non-solid.  The ROM
instead zeroes the surface and falls straight into the triangle loop.
`generate_collision_candidates`, game/src/hasm/collision.s:

    800313D0  addiu $at, $zero, 0xFF
    800313D4  or    $t2, $zero, $zero   # surface = SURFACE_DEFAULT
    800313D8  beql  $v1, $at, .L8003141C
    800313DC   lh   $t0, 0x4($t5)       # (branch-likely delay slot)
    800313E0  sll   $v1, $v1, 3         # else &textures[textureIndex]
    800313E4  add   $v1, $v1, $a0
    800313E8  lb    $t2, 0x7($v1)       #      surface = surfaceType

`.L8003141C` (collision.s:234) is the triangle-loop setup, *not* the batch-skip
labels `.L80031488` / `.L8003148C` that this loop's four genuine `continue`s
branch to.  The decisive evidence needs no reading of the branch target at all:
`or $t2, $zero, $zero` at 800313D4 would be dead code if 255 meant "skip", since
every falling-through path overwrites `$t2` at 800313E8.  It exists only to give
the untextured case SURFACE_DEFAULT.

The matching N64 build links the assembly, so upstream never compiles that C.
This port builds with NON_MATCHING=1 and does.  Same file, same class and same
shape as the grid-mask defect fixed in the "gridmask" wave.

**It is NOT reached by any shipped level.**  Measured `untexturedBatches=0` across
all 20 main tracks and all 10 boss tracks, and again on four tracks with a full
9400-frame race budget.  So the correction is behaviour-neutral today, and this
check says so in both of the ways that matter.

Why the check works this way
----------------------------
Four arms, all from the SAME binary:

* **default vs `MDKR_COLLTEX=legacy`** -- must be byte-identical, and the counter
  must read 0.  That is the *safety* assertion: on shipped data the correction
  changes nothing, which is the strongest available statement that it cannot have
  broken anything.  It is also the tripwire: if some level, ROM revision or
  custom track ever does contain an untextured collidable batch, the count stops
  being 0 and this check reports it instead of it being discovered as a fall.

* **`MDKR_COLLTEX_FORCE=1` vs that plus `=legacy`** -- because the real case is
  unreached, comparing the first pair alone could never distinguish a working fix
  from no fix at all.  Forcing every batch untextured makes the mechanism
  observable, and it reproduces exactly the harm the divergence would cause:
  measured on Ancient Lake, the forced+legacy arm's candidate list collapses to
  **1** entry (the segment pointer, with no facets behind it) and the racer drops
  to **y = -380**, i.e. straight through the world, while the forced+fixed arm
  keeps its candidates and stays on the ground at y = 128.

    MDKR_AUDIO=0 python3 tests/check_collision_untextured.py -v     # ~2 min

Always muted + headless, per tests/README.md.  Exit 0 = pass.
"""
import argparse
import os
import re
import subprocess
import sys

from harness_utils import (DEFAULT_BUILD_DIR, preserved_eeprom, resolve_binary,
                           save_env, test_save_dir)

TRACK = 5              # Ancient Lake -- fast to reach, plenty of terrain
FRAMES = 3400
SCRIPT = "tests/input_scripts/race_full_3lap_tt.txt"
SAVE_DIR = test_save_dir()
SAVE = os.path.join(SAVE_DIR, "eeprom.bin")

# Forced+legacy measured 1 candidate and y=-380; forced+fixed measured 26 and y=128.
MIN_FORCED_FIXED_CANDIDATES = 10
MAX_FORCED_LEGACY_CANDIDATES = 5
MIN_GROUND_Y = -100.0   # anything below this on Ancient Lake is out of the world

PACE_RE = re.compile(r"racer x=(\S+) y=(\S+) z=(\S+)")
COLL_RE = re.compile(r"\[COLL\] maxCandidates=(\d+) truncated=(\d+) cap=(\d+)")
CT_RE = re.compile(r"\[COLLTEX\] untexturedBatches=(\d+) legacy=(\d)")
BAD_RE = re.compile(r"\[FATAL\]|\[CRASH\]|AddressSanitizer|Assertion")


def run(binary, rom, frames, legacy, force, verbose):
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"       # belt-and-braces; --headless-frames is the guarantee
    save_env(env, SAVE_DIR)
    env["MDKR_TRACE"] = "1"
    env["MDKR_AUTOPILOT"] = "1"
    env["MDKR_LOAD_TRACK"] = str(TRACK)
    for k in ("MDKR_COLLTEX", "MDKR_COLLTEX_FORCE"):
        env.pop(k, None)
    if legacy:
        env["MDKR_COLLTEX"] = "legacy"
    if force:
        env["MDKR_COLLTEX_FORCE"] = "1"
    cmd = [binary, "--headless-frames", str(frames),
           "--input-script", SCRIPT, "--rom", rom]
    if verbose:
        print("  $ MDKR_COLLTEX=%s MDKR_COLLTEX_FORCE=%s %s"
              % ("legacy" if legacy else "(unset)", "1" if force else "(unset)",
                 " ".join(cmd)))
    p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=900)
    out = p.stdout.decode("utf-8", "replace")
    coll = COLL_RE.search(out)
    ct = CT_RE.search(out)
    pace = [(float(m.group(1)), float(m.group(2)), float(m.group(3)))
            for m in PACE_RE.finditer(out)]
    return {
        "rc": p.returncode,
        "stream": pace,
        "minY": min((p[1] for p in pace), default=None),
        "maxCandidates": int(coll.group(1)) if coll else None,
        "untextured": int(ct.group(1)) if ct else None,
        "legacyFlag": int(ct.group(2)) if ct else None,
        "bad": BAD_RE.findall(out),
    }


def main() -> int:
    # This check deletes and rewrites EEPROM images to assert a known starting
    # state. Under tools/run_checks.py SAVE_DIR is a run-scoped temporary
    # directory; a standalone run defaults to the repository's playable save/,
    # whose prior contents must survive however this exits.
    with preserved_eeprom(SAVE_DIR):
        return run_check()


def run_check() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--frames", type=int, default=FRAMES)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not os.path.exists(path):
            print("FAIL: missing %s" % path, file=sys.stderr)
            return 1

    failures: list[str] = []
    arms = {}
    for name, legacy, force in (("fixed", False, False), ("legacy", True, False),
                                ("forced", False, True), ("forcedLegacy", True, True)):
        if os.path.exists(SAVE):
            os.remove(SAVE)
        r = run(binary, args.rom, args.frames, legacy, force, args.verbose)
        arms[name] = r
        if r["rc"] != 0:
            failures.append("%s arm: exit code %d" % (name, r["rc"]))
        if r["bad"]:
            failures.append("%s arm: %s in output" % (name, r["bad"][0]))
        # Fail closed: without the probes nothing below means anything.
        if r["untextured"] is None:
            failures.append("%s arm: no '[COLLTEX] untexturedBatches=' line -- the "
                            "probe is gone, so this check cannot tell whether the "
                            "untextured case is reached" % name)
        if r["maxCandidates"] is None:
            failures.append("%s arm: no '[COLL] maxCandidates=' line" % name)
        if not r["stream"]:
            failures.append("%s arm: no racer rows -- the route never reached a race"
                            % name)
        if r["legacyFlag"] is not None and r["legacyFlag"] != int(legacy):
            failures.append("%s arm: probe reports legacy=%d, expected %d -- the "
                            "MDKR_COLLTEX toggle is not taking effect"
                            % (name, r["legacyFlag"], int(legacy)))
        if args.verbose and r["untextured"] is not None:
            print("  arm %-12s untextured=%-6d maxCandidates=%-4s minY=%s"
                  % (name, r["untextured"], r["maxCandidates"],
                     None if r["minY"] is None else round(r["minY"], 1)))
    if failures:
        for f in failures:
            print("  - %s" % f, file=sys.stderr)
        print("check_collision_untextured: FAIL")
        return 1

    fx, lg = arms["fixed"], arms["legacy"]
    ff, fl = arms["forced"], arms["forcedLegacy"]

    # --- safety / tripwire: unreached on shipped data, so nothing may move ---
    if fx["untextured"] != 0:
        failures.append("the untextured-batch case is now REACHED on track %d "
                        "(%d batches). That is new: it measured 0 across all 20 main "
                        "and all 10 boss tracks. The correction in collision.c is "
                        "now load-bearing rather than latent -- re-read "
                        "docs/OPEN_ITEMS.md wave \"hasmaudit\" and re-do the "
                        "reachability sweep before trusting any of it"
                        % (TRACK, fx["untextured"]))
    elif fx["stream"] != lg["stream"]:
        n = sum(1 for a, b in zip(fx["stream"], lg["stream"]) if a != b)
        failures.append("with the untextured case unreached (0 batches) the two arms "
                        "must be bit-identical, but they differ in %d of %d racer "
                        "rows. The correction is changing something it cannot be "
                        "responsible for" % (n, len(fx["stream"])))

    # --- the forced arms: does the fixed path actually collide? --------------
    if not ff["untextured"]:
        failures.append("MDKR_COLLTEX_FORCE=1 produced untexturedBatches=%s -- the "
                        "forced injection is not working, so the two assertions "
                        "below are vacuous and this check cannot distinguish a "
                        "working fix from no fix at all" % ff["untextured"])
    else:
        if ff["maxCandidates"] < MIN_FORCED_FIXED_CANDIDATES:
            failures.append("forced+fixed peaked at %d collision candidates (want "
                            ">= %d) -- treating a batch as untextured should still "
                            "collide with it, so the ROM's SURFACE_DEFAULT path is "
                            "not feeding the list"
                            % (ff["maxCandidates"], MIN_FORCED_FIXED_CANDIDATES))
        if ff["minY"] < MIN_GROUND_Y:
            failures.append("forced+fixed dropped to y=%.0f (want >= %.0f) -- the "
                            "racer fell out of the world even with the fix, so "
                            "untextured batches are not actually being collided with"
                            % (ff["minY"], MIN_GROUND_Y))
        # the positive control: the legacy `continue` must visibly break it
        if fl["maxCandidates"] > MAX_FORCED_LEGACY_CANDIDATES:
            failures.append("forced+legacy still peaked at %d candidates (want <= %d) "
                            "-- skipping every batch should collapse the list to just "
                            "the segment pointer. The positive control is not "
                            "reproducing the defect, so this check proves nothing"
                            % (fl["maxCandidates"], MAX_FORCED_LEGACY_CANDIDATES))
        if fl["minY"] >= MIN_GROUND_Y:
            failures.append("forced+legacy kept the racer at y >= %.0f (min %.0f). "
                            "With every collidable batch skipped it should fall "
                            "through the world; the control is not reproducing the "
                            "harm" % (MIN_GROUND_Y, fl["minY"]))
        if fl["maxCandidates"] == ff["maxCandidates"] and fl["minY"] == ff["minY"]:
            failures.append("forced+fixed and forced+legacy are indistinguishable "
                            "(%d candidates, minY %.0f in both) -- the A/B is not "
                            "wired up, or the fix is not in the build"
                            % (ff["maxCandidates"], ff["minY"]))

    if os.path.exists(SAVE):
        os.remove(SAVE)
    if failures:
        for msg in failures:
            print("  - %s" % msg, file=sys.stderr)
        print("check_collision_untextured: FAIL")
        return 1
    print("check_collision_untextured: PASS  (shipped data: %d untextured batches, "
          "%d racer rows identical across both arms | forced: fixed %d cand/minY %.0f "
          "vs legacy %d cand/minY %.0f)"
          % (fx["untextured"], len(fx["stream"]), ff["maxCandidates"], ff["minY"],
             fl["maxCandidates"], fl["minY"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
