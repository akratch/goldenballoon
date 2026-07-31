#!/usr/bin/env python3
"""Headless determinism check: the same fixture must render byte-identical frames.

Why this exists
---------------
Every frame-comparison check in this project -- the oracle scoring in
docs/ORACLE.md, the GL-vs-WebGPU parity scoring, the native-vs-wasm comparison,
and any "dump a PNG and look at it" verification -- silently assumes that a
headless run is REPRODUCIBLE.  It was not.

`osGetCount()` (platform/stubs_dkr.c) used to return the host monotonic clock.
On the N64 the COUNTER and the VI retrace rate are both real time and therefore
locked to each other; a headless run breaks that, because the frame loop advances
as fast as the machine allows while the pacer synthesises a FIXED field count per
frame.  So the COUNTER advanced by a machine-load-dependent amount per simulated
frame.  audio.c `music_animation_fraction()` integrates COUNTER deltas, and
object_functions.c `obj_loop_charselect` feeds that straight into the
character-select models' `animFrame` -- so the characters' animation phase, and
thus the rendered image, differed on every run.

Measured before the fix, nav_to_character_select frame 1450 over 10 runs:
10 distinct images, 45/45 pairs differing, median 18.9% of pixels, minimum 4.7%.
The per-frame draw telemetry was identical for frames 0..1298 and diverged from
frame 1299 -- immediately after the menu transition that starts a music-synced
animation.  Crucially this reproduced across 5 different arena base addresses
after the fix, which rules out the ASLR/pointer-value hypothesis this codebase's
history makes tempting: it was the wall clock, not the allocator.

A non-reproducible renderer does not fail any existing test -- every fixture
still exits 0 -- so nothing caught it.  Hence this check.

What it asserts
---------------
For each fixture: run it N times (default 2) and require every dumped frame to be
byte-identical across runs.  A single differing byte fails.

Usage:
    tests/check_determinism.py [--build build] [--rom baserom.us.v80.z64]
                               [--runs N] [--frames N] [--every N]
                               [--fixture NAME ...] [-v]

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.  Exit 0 = pass; exit 1 = at least one fixture diverged.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

from harness_utils import resolve_binary

DEFAULT_FIXTURES = [
    "nav_to_character_select",  # music-synced character animation: the original repro
    "nav_to_options",
    "race_drive_time_trial",
]


def frame_hashes(build, rom, script, outdir, frames, every, verbose):
    """Run one headless pass and return {frame_name: sha1} for every dumped frame."""
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"          # belt-and-braces; --headless-frames is the guarantee
    env["MDKR_DUMP_EVERY"] = str(every)
    cmd = [build, "--headless-frames", str(frames), "--rom", rom,
           "--dump-frames", outdir]
    if script:
        cmd += ["--input-script", script]
    if verbose:
        print("    $ " + " ".join(cmd))
    proc = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise RuntimeError("run failed rc=%d: %s"
                           % (proc.returncode, proc.stderr.decode("utf-8", "replace")[-400:]))
    out = {}
    for name in sorted(os.listdir(outdir)):
        if not name.endswith(".ppm"):
            continue
        with open(os.path.join(outdir, name), "rb") as f:
            out[name] = hashlib.sha1(f.read()).hexdigest()
    if not out:
        raise RuntimeError("no frames were dumped")
    return out


def first_difference(a_dir, b_dir, name):
    """Byte offset and pixel index of the first difference, for the report."""
    with open(os.path.join(a_dir, name), "rb") as f:
        a = f.read()
    with open(os.path.join(b_dir, name), "rb") as f:
        b = f.read()
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return n if len(a) != len(b) else -1


def check_fixture(fixture, args):
    script = None
    if fixture != "__boot__":
        script = os.path.join("tests", "input_scripts", fixture + ".txt")
        # Fail closed: a fixture that cannot be found proves nothing about
        # determinism, so a missing input script is a failure, not a skip.
        if not os.path.exists(script):
            print("  %-28s FAIL: missing input script %s" % (fixture, script))
            return False

    dirs = []
    try:
        runs = []
        for r in range(args.runs):
            d = tempfile.mkdtemp(prefix="mdkr_det_")
            dirs.append(d)
            runs.append(frame_hashes(args.build, args.rom, script, d,
                                     args.frames, args.every, args.verbose))

        names = sorted(runs[0])
        for r in range(1, args.runs):
            if sorted(runs[r]) != names:
                print("  %-28s FAIL: run %d dumped a different set of frames "
                      "(%d vs %d)" % (fixture, r + 1, len(runs[r]), len(names)))
                return False

        bad = []
        for name in names:
            hashes = {runs[r][name] for r in range(args.runs)}
            if len(hashes) != 1:
                off = first_difference(dirs[0], dirs[1], name)
                bad.append((name, off))

        if bad:
            print("  %-28s FAIL: %d of %d frames differ across %d runs"
                  % (fixture, len(bad), len(names), args.runs))
            for name, off in bad[:5]:
                print("      %s: first differing byte at offset %d" % (name, off))
            if len(bad) > 5:
                print("      ... and %d more" % (len(bad) - 5))
            return False

        # Fail closed: "identical" over an empty frame set is vacuous. Comparing
        # zero frames means the fixture produced no evidence, so it cannot pass.
        if not names:
            print("  %-28s FAIL: no frames to compare" % fixture)
            return False

        print("  %-28s ok (%d frames identical across %d runs)"
              % (fixture, len(names), args.runs))
        return True
    finally:
        for d in dirs:
            shutil.rmtree(d, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--frames", type=int, default=1500)
    ap.add_argument("--every", type=int, default=350)
    ap.add_argument("--fixture", action="append", dest="fixtures")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    args.build = resolve_binary(args.build)

    if not os.path.exists(args.build):
        sys.exit("no build at %s" % args.build)
    if not os.path.exists(args.rom):
        sys.exit("no ROM at %s" % args.rom)

    fixtures = args.fixtures or DEFAULT_FIXTURES
    print("check_determinism: %d run(s) per fixture, %d frames, dumping every %d"
          % (args.runs, args.frames, args.every))
    ok = True
    for fx in fixtures:
        try:
            ok &= check_fixture(fx, args)
        except RuntimeError as e:
            print("  %-28s FAIL: %s" % (fx, e))
            ok = False

    print("check_determinism: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
