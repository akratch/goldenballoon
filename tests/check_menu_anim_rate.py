#!/usr/bin/env python3
"""The character-select dance must not oscillate several times a second.

The bug this locks down (T1)
---------------------------
The dancing characters on PLAYER SELECT ran about 8x too fast -- 4.7 frames per
cycle against 36-38 on the real ROM. It was reported from the browser build, but
it reproduced headlessly on the native one, and the cause was in neither: it was
`osGetCount()`.

`audio.c music_animation_fraction()` integrates COUNTER deltas and drives the
characters' `animFrame` from the result (`object_functions.c`, the charselect
branch). It cannot survive a delta of zero:

    if ((u32) audioPrevCount < cnt) { tick += (cnt - audioPrevCount) / 46875.0f; }
    else                           { tick += ((cnt - audioPrevCount) - 1) / 46875.0f; }

Equal values take the else branch, where `0u - 1` is 0xFFFFFFFF -- a +91626 ms
jump, which modulo the 659 ms beat cycle lands 22.4 ms BEHIND. Eight character
models call that per frame, so the phase moved -140 ms per frame instead of
+16.7 ms and swept backwards through the cycle every 4.7 frames.

On hardware the COUNTER advances every second CPU cycle, so two reads in one frame
always differ. Our sources did not: the synthetic (headless) clock is constant
within a frame by construction, and host clocks are quantised -- browsers clamp
performance.now() to ~100us. `osGetCount()` now guarantees strict increase.

What is asserted
----------------
Not the exact period -- the visible motion also includes butterflies drifting
across the screen, so the autocorrelation of a short window is smeared and a
tight period band would be flaky. What IS unambiguous is the *shape*: a healthy
capture decays monotonically at short lags, and the broken one has a hard peak
every ~5 frames at r>0.9. So:

  1. something is actually moving        (motion RMS above a floor -- otherwise a
                                          black screen would pass vacuously)
  2. no local correlation maximum below lag 15 with r >= 0.60
                                         (pre-fix: lag 5, r=0.92 -> FAIL)

Verified in both directions: with the fix reverted this check fails on the lag-5
peak; with it in place the correlogram decays monotonically to lag 15.

Usage:
    tests/check_menu_anim_rate.py
    tests/check_menu_anim_rate.py --keep-frames /tmp/anim   # inspect the capture

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.
"""
import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

from harness_utils import resolve_binary

# Frames of the char-select screen to capture. The route parks on PLAYER SELECT at
# native frame ~1394; 1400 is safely past the fade-in.
DUMP_FROM = 1400
FRAMES = 1500
REGION = (0.1, 0.2, 0.95, 0.65)   # the two character rows, excluding title/ground
MOTION_FLOOR = 5.0
MAX_SHORT_LAG = 15
SHORT_LAG_R = 0.60


def load_anim_period():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "tools", "anim_period.py")
    spec = importlib.util.spec_from_file_location("anim_period", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--script", default="tests/input_scripts/nav_to_character_select.txt")
    ap.add_argument("--keep-frames", default=None,
                    help="write the capture here and keep it (default: temp dir, deleted)")
    args = ap.parse_args()
    args.build = resolve_binary(args.build)

    for path in (args.build, args.rom, args.script):
        if not os.path.exists(path):
            sys.exit("missing: %s" % path)

    tmp = args.keep_frames or tempfile.mkdtemp(prefix="mdkr64-anim-")
    os.makedirs(tmp, exist_ok=True)
    try:
        env = dict(os.environ)
        env["MDKR_AUDIO"] = "0"          # belt-and-braces; --headless-frames is the guarantee
        env["MDKR_DUMP_FROM"] = str(DUMP_FROM)
        env["MDKR_DUMP_EVERY"] = "1"
        cmd = [args.build, "--headless-frames", str(FRAMES),
               "--input-script", args.script, "--rom", args.rom, "--dump-frames", tmp]
        print("check_menu_anim_rate: capturing frames %d..%d of the char-select screen"
              % (DUMP_FROM, FRAMES))
        proc = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                              stderr=subprocess.STDOUT, timeout=600)
        if proc.returncode != 0:
            print("check_menu_anim_rate: FAIL — runner exited %d" % proc.returncode)
            return 1

        mod = load_anim_period()
        frames, first, last = mod.load_capture(tmp, 24, 18, None, None, REGION)
        corr, motion = mod.autocorrelation(frames, 3, 40)
        print("   %d frames (%d..%d), motion RMS %.1f" % (len(frames), first, last, motion))

        failures = []
        if motion < MOTION_FLOOR:
            failures.append("nothing is moving (motion RMS %.2f < %.2f) — the screen is "
                            "static or black, so this check proved nothing"
                            % (motion, MOTION_FLOOR))

        short = [(lag, r) for lag, r in corr if lag <= MAX_SHORT_LAG]
        bad = []
        for i in range(1, len(short) - 1):
            lag, r = short[i]
            if r > short[i - 1][1] and r >= short[i + 1][1] and r >= SHORT_LAG_R:
                bad.append((lag, r))
        if bad:
            failures.append("fast oscillation: correlation peak(s) at lag %s — the "
                            "animation is cycling several times a second"
                            % ", ".join("%d frames (r=%.2f)" % (l, r) for l, r in bad))

        print("   correlogram lag 3..%d: %s" % (MAX_SHORT_LAG,
              " ".join("%d:%.2f" % (l, r) for l, r in short)))
        if failures:
            for f in failures:
                print("check_menu_anim_rate: FAIL — %s" % f)
            return 1
        print("check_menu_anim_rate: PASS — no sub-15-frame oscillation; "
              "correlogram decays monotonically")
        return 0
    finally:
        if not args.keep_frames:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
