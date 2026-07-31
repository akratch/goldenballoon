#!/usr/bin/env python3
"""vec3f_rotate_py() must pair pitch with yaw, not each angle with itself.

The bug this locks down
-----------------------
`vec3f_rotate_py()` (game/src/hasm/math_util.c) turns a pitch/yaw pair into a
direction vector.  The upstream NON_MATCHING C body had the two angles
**transposed** in the x and y results:

    vec->x = z * cosY * sinX;      /* WRONG */
    vec->y = -z * sinY;            /* WRONG */
    vec->z = z * cosX * cosY;      /* right */

The ROM computes `x = z*cos(pitch)*sin(yaw)`, `y = -z*sin(pitch)`.
`LEAF(vec3f_rotate_py)` in game/src/hasm/ido/math_util.s (ROM 0x800706D0) loads
the pitch first and the yaw second:

    lh    a0, 0x2(a2)     # pitch
    jal   sins_f
    mul.s ft1, ft2, fv0   # ft1 = z * sin(pitch)
    lh    a0, 0x2(a2)     # pitch
    jal   coss_f
    neg.s ft1             # y   = -z * sin(pitch)
    mul.s ft2, fv0        # z1  =  z * cos(pitch)
    lh    a0, 0x0(a2)     # yaw
    jal   sins_f
    mul.s ft0, ft2, fv0   # x   = z1 * sin(yaw)
    lh    a0, 0x0(a2)     # yaw
    jal   coss_f
    mul.s ft2, fv0        # z   = z1 * cos(yaw)

The offsets are what settle it, not the .s comments: `Vec3s`
(game/include/structs.h:51) is a union whose rotation view puts `y_rotation` at
0x0 and `x_rotation` at 0x2.  So the pair loaded FIRST (0x2) is the pitch.  The
.s labels 0x0 "roll", which is what the transposition was copied from.

The matching N64 build takes `GLOBAL_ASM("asm/math_util/vec3f_rotate_py.s")`, so
upstream never compiles that C body.  This port builds with NON_MATCHING=1, which
makes us the first to run it.

Why it is silent
----------------
Every call site feeds the result straight into a position or a velocity, so a
wrong direction still moves something plausibly -- nothing crashes and nothing
logs.  The magnitude is not subtle, though: with pitch 0 and yaw 90 degrees the
ROM returns (z, 0, 0) and the transposed version returns (0, -z, 0), i.e. a
horizontal direction becomes a vertical one.  Call sites are particle emission
(particles.c:1163/1165/1214/2393), the lens flare (weather.c:631), spotlight
direction (lights.c:413) and object sprite placement (objects.c:493).

Why the check works this way
----------------------------
Both arms come from the SAME binary: `MDKR_ROTPY=legacy` restores the
transposition verbatim (platform/math_stubs_temp.c), so the broken arm is
re-produced on every run and has to actually *exhibit* the defect before the
fixed arm is credited.  Three independent things are asserted:

1. **An oracle that needs no golden numbers.**  `vec3f_rotate_py` is by its own
   docstring `vec3f_rotate` specialised to (0, 0, z) with zero roll, and
   `vec3f_rotate` matches its own assembly (audited separately).  The self-test
   runs both on the same inputs and prints both.  They agree only with the fix.
   This assertion does not depend on anyone's reading of the assembly.
2. **The ROM's formula**, evaluated here in Python from the instruction sequence
   quoted above.
3. **Reachability**, because the fix is behaviour-neutral for the racer
   simulation -- measured 0 of 359 [PACE] rows changed, since no call site feeds
   physics -- so a [PACE] diff could never show the code runs.  The probe counts
   calls and folds every result into a hash instead; the arms must differ.

    MDKR_AUDIO=0 python3 tests/check_math_rotpy.py -v        # ~20 s

Always muted + headless, per tests/README.md.  Exit 0 = pass.
"""
import argparse
import importlib.util
import os
import re
import subprocess
import sys

from harness_utils import resolve_binary

# Must match the cases in platform/math_stubs_temp.c's self-test.
CASES = {"A": (0x0000, 0x4000), "B": (0x2000, 0x1000)}
Z_IN = 100.0

# float32 arithmetic printed at %.5f -- 1e-2 is far tighter than the gap being
# measured (the two arms differ by tens of units) and far looser than float noise.
#
# This is NOT a trig tolerance, and deliberately was not widened into one.  The
# port used to evaluate sins_f/coss_f with libm; since the "closedloop" wave it
# walks the ROM's gSineTable and lerps, exactly as LEAF(sins_f) does, which
# deviates from a true sine by up to 7/65536 of amplitude -- 0.011 world units at
# z=100, i.e. just over this tolerance, and case B duly started failing on
# (65.3172 vs 65.3281).  The fix is to make the reference use the ROM's sine
# rather than to allow the reference to be wrong: rom_formula() below evaluates
# the same table walk, read out of the same .s, so the comparison is exact again
# and the tolerance still only has to absorb float32 noise.
TOL = 1e-2
# The two functions are the same formula on the same inputs, so they agree to
# float noise, not merely approximately.
ORACLE_TOL = 1e-3

SELFTEST_RE = re.compile(
    r"\[ROTPY\] selftest case=(\w) pitch=0x([0-9A-Fa-f]+) yaw=0x([0-9A-Fa-f]+) "
    r"z=100 -> x=(\S+) y=(\S+) z=(\S+) \| rotate3=(\S+),(\S+),(\S+)")
CALLS_RE = re.compile(r"\[ROTPY\] calls=(\d+) hash=0x([0-9a-f]+) legacy=(\d)")
BAD_RE = re.compile(r"\[FATAL\]|\[CRASH\]|AddressSanitizer|Assertion")


def _load_sine_oracle():
    """The ROM's own sin/cos: gSineTable out of the .s, walked by XLEAF(sins_s16).

    Reuses tests/check_math_tables.py so there is exactly ONE Python
    transcription of that assembly in the tree; that file asserts it reproduces
    the binary's sins_s16 on all 65536 angles, so importing it here means this
    check and that one cannot drift apart.
    """
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "check_math_tables.py")
    spec = importlib.util.spec_from_file_location("mdkr_check_math_tables", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    table = mod.parse_asm_half_table(open(mod.ASM).read(), "gSineTable")

    def sins_f(angle):
        """LEAF(sins_f): sins_s16(angle) * (1.0f / 0x10000)."""
        v = mod.sins_s16_from_asm(table, angle & 0xFFFF)
        if v >= 0x80000000:
            v -= 0x100000000
        return v / 65536.0

    def coss_f(angle):
        """LEAF(coss_f): addiu a0, 0x4000 and fall through."""
        return sins_f(angle + 0x4000)

    return sins_f, coss_f


SINS_F, COSS_F = _load_sine_oracle()


def rom_formula(pitch, yaw, z):
    """x = z*cos(pitch)*sin(yaw), y = -z*sin(pitch), z = z*cos(pitch)*cos(yaw).

    Read straight off LEAF(vec3f_rotate_py) -- see the module docstring -- and
    evaluated with the ROM's own sine rather than libm's, because that is what
    the function it is checking calls.
    """
    return (z * COSS_F(pitch) * SINS_F(yaw),
            -z * SINS_F(pitch),
            z * COSS_F(pitch) * COSS_F(yaw))


def transposed_formula(pitch, yaw, z):
    """The pre-fix body, for the positive control."""
    return (z * COSS_F(yaw) * SINS_F(pitch),
            -z * SINS_F(yaw),
            z * COSS_F(pitch) * COSS_F(yaw))


def run(binary, rom, frames, legacy, verbose):
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"       # belt-and-braces; --headless-frames is the guarantee
    env["MDKR_TRACE"] = "1"       # arms the [ROTPY] probe and the self-test
    if legacy:
        env["MDKR_ROTPY"] = "legacy"
    else:
        env.pop("MDKR_ROTPY", None)
    # This check has exactly ONE variable, the pitch/yaw pairing, and rom_formula()
    # below evaluates the ROM's own sine to compare against. An inherited
    # MDKR_TRIG=libm would put the binary on a different sine from the reference
    # and fail both arms for a reason that has nothing to do with vec3f_rotate_py
    # -- measured: (27.0598, -70.7107, 65.3281) against a reference of
    # (27.0568, -70.7092, 65.3172). The other two A/B toggles are cleared for the
    # same hygiene, though neither feeds this function.
    for k in ("MDKR_TRIG", "MDKR_ARCTAN", "MDKR_RNGSEED"):
        env.pop(k, None)
    cmd = [binary, "--headless-frames", str(frames), "--rom", rom]
    if verbose:
        print("  $ MDKR_AUDIO=0 MDKR_TRACE=1 MDKR_ROTPY=%s %s"
              % ("legacy" if legacy else "(unset)", " ".join(cmd)))
    proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=600)
    return proc.stdout.decode("utf-8", "replace"), proc.returncode


def parse(out):
    cases = {}
    for m in SELFTEST_RE.finditer(out):
        cases[m.group(1)] = {
            "pitch": int(m.group(2), 16),
            "yaw": int(m.group(3), 16),
            "v": tuple(float(m.group(i)) for i in (4, 5, 6)),
            "oracle": tuple(float(m.group(i)) for i in (7, 8, 9)),
        }
    c = CALLS_RE.search(out)
    return {
        "cases": cases,
        "calls": int(c.group(1)) if c else None,
        "hash": c.group(2) if c else None,
        "legacyFlag": int(c.group(3)) if c else None,
        "bad": BAD_RE.findall(out),
    }


def close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def fmt(v):
    return "(%.4f, %.4f, %.4f)" % v


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--frames", type=int, default=1500)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom):
        if not os.path.exists(path):
            print("FAIL: missing %s" % path, file=sys.stderr)
            return 1

    failures: list[str] = []
    arms = {}
    for name, legacy in (("fixed", False), ("legacy", True)):
        out, rc = run(binary, args.rom, args.frames, legacy, args.verbose)
        r = parse(out)
        arms[name] = r
        if rc != 0:
            failures.append("%s arm: exit code %d" % (name, rc))
        if r["bad"]:
            failures.append("%s arm: %s in output" % (name, r["bad"][0]))
        # Fail closed: without the probes every assertion below is vacuous.
        if r["calls"] is None:
            failures.append("%s arm: no '[ROTPY] calls=' line -- the reachability "
                            "probe is gone, so this check cannot see whether the "
                            "function runs at all" % name)
        missing = sorted(set(CASES) - set(r["cases"]))
        if missing:
            failures.append("%s arm: no '[ROTPY] selftest' line for case(s) %s -- "
                            "the self-test is gone, so the formula is unasserted"
                            % (name, ",".join(missing)))
        if r["legacyFlag"] is not None and r["legacyFlag"] != int(legacy):
            failures.append("%s arm: probe reports legacy=%d, expected %d -- the "
                            "MDKR_ROTPY toggle is not taking effect, so the two "
                            "arms are not actually different"
                            % (name, r["legacyFlag"], int(legacy)))
    if failures:
        for f in failures:
            print("  - %s" % f, file=sys.stderr)
        print("check_math_rotpy: FAIL")
        return 1

    fixed, legacy = arms["fixed"], arms["legacy"]

    for tag, (pitch, yaw) in sorted(CASES.items()):
        want = rom_formula(pitch, yaw, Z_IN)
        got = fixed["cases"][tag]["v"]
        oracle = fixed["cases"][tag]["oracle"]
        broke = legacy["cases"][tag]["v"]
        want_broke = transposed_formula(pitch, yaw, Z_IN)

        if fixed["cases"][tag]["pitch"] != pitch or fixed["cases"][tag]["yaw"] != yaw:
            failures.append("case %s: binary self-tested pitch=0x%04X yaw=0x%04X but "
                            "this check expects 0x%04X/0x%04X -- they have drifted "
                            "apart" % (tag, fixed["cases"][tag]["pitch"],
                                       fixed["cases"][tag]["yaw"], pitch, yaw))
            continue

        # (1) the oracle that needs no golden numbers
        if not close(got, oracle, ORACLE_TOL):
            failures.append("case %s: vec3f_rotate_py returned %s but vec3f_rotate "
                            "on the same angles applied to (0,0,%g) returned %s. "
                            "vec3f_rotate_py IS that specialisation, so they must "
                            "agree; the pitch/yaw pairing is wrong"
                            % (tag, fmt(got), Z_IN, fmt(oracle)))
        # (2) the ROM's formula
        if not close(got, want, TOL):
            failures.append("case %s (pitch=0x%04X yaw=0x%04X): fixed arm returned %s, "
                            "want %s from the ROM's x=z*cosP*sinY, y=-z*sinP, "
                            "z=z*cosP*cosY (LEAF(vec3f_rotate_py), ROM 0x800706D0)"
                            % (tag, pitch, yaw, fmt(got), fmt(want)))
        # (3) the positive control must actually be broken
        if not close(broke, want_broke, TOL):
            failures.append("case %s: MDKR_ROTPY=legacy returned %s, but the "
                            "transposed formula gives %s -- the positive control no "
                            "longer reproduces the defect, so this check proves "
                            "nothing" % (tag, fmt(broke), fmt(want_broke)))
        if close(broke, got, TOL):
            failures.append("case %s: both arms returned %s -- MDKR_ROTPY=legacy did "
                            "not change the result, so the A/B is not wired up"
                            % (tag, fmt(got)))
        # and the oracle must DISCRIMINATE, or assertion (1) is unfalsifiable
        if close(broke, legacy["cases"][tag]["oracle"], ORACLE_TOL):
            failures.append("case %s: in the legacy arm vec3f_rotate_py (%s) still "
                            "agrees with vec3f_rotate (%s). The oracle cannot tell "
                            "the two apart on this input, so assertion (1) is "
                            "vacuous -- pick angles where pitch != yaw"
                            % (tag, fmt(broke), fmt(legacy["cases"][tag]["oracle"])))
        if args.verbose:
            print("  case %s pitch=0x%04X yaw=0x%04X: fixed %s | legacy %s | oracle %s"
                  % (tag, pitch, yaw, fmt(got), fmt(broke), fmt(oracle)))

    # (4) reachability -- the fix is invisible in a [PACE] stream, so this is the
    # only thing that shows the corrected code actually runs on live data.
    if not fixed["calls"]:
        failures.append("fixed arm: vec3f_rotate_py was called %s times outside the "
                        "self-test -- if it is never reached, correcting it is churn "
                        "and this check is not measuring anything"
                        % fixed["calls"])
    if fixed["hash"] == legacy["hash"]:
        failures.append("both arms produced the same output hash (0x%s) over %d/%d "
                        "live calls -- the transposition made no difference to any "
                        "vector the game actually asked for, so the fix is not "
                        "demonstrably reached"
                        % (fixed["hash"], fixed["calls"], legacy["calls"]))
    if args.verbose:
        print("  live calls: fixed=%d hash=0x%s | legacy=%d hash=0x%s"
              % (fixed["calls"], fixed["hash"], legacy["calls"], legacy["hash"]))

    if failures:
        for msg in failures:
            print("  - %s" % msg, file=sys.stderr)
        print("check_math_rotpy: FAIL")
        return 1
    print("check_math_rotpy: PASS  (case A fixed=%s legacy=%s; %d live calls, "
          "hashes differ)"
          % (fmt(fixed["cases"]["A"]["v"]), fmt(legacy["cases"]["A"]["v"]),
             fixed["calls"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
