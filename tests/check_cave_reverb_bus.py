#!/usr/bin/env python3
"""Prove cave sound effects reach the big-room reverb bus (aux bus 1).

Why this exists
---------------
Diddy Kong Racing runs TWO reverb buses (game/src/audio.c):

  * bus 0 = AL_FX_CUSTOM  — the ROM music reverb; every CSP (music) voice stays
    on it, and it is the only bus the port ever built.
  * bus 1 = AL_FX_BIGROOM — the fixed libaudio big-room reverb. Every SFX voice
    is re-parented onto it at note-on (audiosfx.c, func_80065A80), which is what
    gives engine/environment sounds their echo inside caves and tunnels.

The port hardcoded ``maxAuxBusses = 1`` and built only bus 0, so the SFX
re-parent to bus 1 was silently rejected (audio_compat.c) and cave SFX were sent
to the music reverb — or, deep in a cave where the reverb-line strength drives
the dry signal down, effectively muted. That is issue #49: "the echo effect in
Treasure Caves isn't handled correctly; instead of an echo, sounds are simply
turned down, muted around mid cave."

Every other audio gate misses it. ``check_audio_level_reference.py`` drives the
outdoor race_state_oracle route on Ancient Lake, which carries NO reverb lines,
so SFX wet ~= 0 there and the bus SFX land on is irrelevant. The bug only shows
where SFX fxMix > 0 — a reverb-line level. This check is that missing oracle.

What it does
------------
Loads Treasure Caves directly (``MDKR_LOAD_TRACK=30``, the same retarget hook
``check_track_sweep.py`` uses) and drives it with DKR's own AI
(``MDKR_AUTOPILOT=1``) so the racer moves through the cave and under its ceiling,
where ``audspat_calculate_echo`` ramps the SFX reverb send up. The clean-room
synth writes a per-voice reverb-routing line to ``MDKR_AUDIO_VOICE_TRACE_JSONL``
each time a voice's FX mix is set: the aux bus its envmixer is currently a source
of, the wet-send amount (fxMix, 0..127), and how many aux buses the synth built.

What it asserts
---------------
 1. The synth built TWO aux buses (``max_aux == 2`` on every trace line). Before
    the fix this is 1 and this assertion fails first.
 2. Treasure Caves actually loaded as a race and the racer made forward progress,
    so the trace is of a car driving through the cave and not a menu (anti-vacuity
    for "mid-cave").
 3. SFX voices reach the big-room bus WITH wet send: at least ``MIN_BUS1_WET``
    trace lines have ``bus == 1`` and ``fxmix > 0``. Before the fix every
    fxmix>0 line is on ``bus == 0`` (the music reverb) and there are zero bus-1
    lines — RED. After the fix the cave SFX land on bus 1 — GREEN.

Self-validation — this check is proven to be able to fail
---------------------------------------------------------
The pre-fix binary (single aux bus) produces a trace whose every line is
``bus:0 max_aux:1`` including the fxmix>0 cave SFX, so assertions 1 and 3 both
fail. That is the RED state; the fix turns it GREEN. Only SFX voices appear here
(music sets its FX mix through the note-on start param, not ``alSynSetFXMix``),
which sharpens the bus-0/bus-1 discriminator. This oracle proves the ROUTING —
which bus the cave SFX reach — not that the reverb DSP carries real parameters;
the level/PCM gates cover the DSP itself.

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), one authored
Original-cadence ticket per host opportunity (MDKR_SYNTH_FIELDS=2), per
tests/README.md.

Usage:
    tests/check_cave_reverb_bus.py --build build --rom baserom.us.v80.z64
    tests/check_cave_reverb_bus.py --track 30 --frames 6000   # a deeper drive
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "race_drive_time_trial.txt"

# ASSET_LEVEL_TREASURECAVES (game/include/asset_enums.h). A Sherbet Island cave
# track that carries reverb lines; index 30 in the level-header enum and in the
# menu-order track table check_track_sweep.py sweeps.
TREASURE_CAVES = 30

# The reverb send ramps 0 -> magnitude over ~300 units of depth, so a handful of
# frames near a reverb line is enough; require a clear margin above noise.
MIN_BUS1_WET = 8

PACE_RE = re.compile(r"racer x=(\S+) y=(\S+) z=(\S+) clock=(\d+) cp=(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"MemorySanitizer|runtime error:|Assertion|\[FX BUG\]"
)


def run_cave(build, rom, script, track, frames, trace_path, timeout):
    """One headless Treasure-Caves drive with the voice reverb trace on."""
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR_", "GE007_"))}
    env["MDKR_AUDIO"] = "0"                 # --headless-frames is the guarantee
    env["MDKR_AUDIO_REVERB"] = "1"          # the wet path must be live
    env["MDKR_PRESENT_RATE"] = "original"
    env["MDKR_SIMULATION_CADENCE"] = "original"
    env["MDKR_SYNTH_FIELDS"] = "2"
    env["MDKR_AUTOPILOT"] = "1"             # drive with DKR's own AI
    env["MDKR_TRACE"] = "1"                 # emit the [PACE] racer probe
    env["MDKR_LOAD_TRACK"] = str(track)
    env["MDKR_AUDIO_VOICE_TRACE_JSONL"] = str(trace_path)
    cmd = [build, "--headless-frames", str(frames), "--input-script",
           str(script), "--rom", rom]
    with tempfile.TemporaryDirectory(prefix="mdkr_cave_reverb_") as run_dir:
        env["MDKR_VIDEO_CONFIG_PATH"] = os.path.join(run_dir, "video.ini")
        env["MDKR_SAVE_DIR"] = os.path.join(run_dir, "save")
        proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def load_trace(path):
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return rows


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--script", default=str(SCRIPT))
    ap.add_argument("--track", type=int, default=TREASURE_CAVES)
    ap.add_argument("--frames", type=int, default=5600)
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()
    args.build = resolve_binary(args.build)

    for path in (args.build, args.rom, args.script):
        if not os.path.exists(path):
            sys.exit("missing: %s" % path)

    failures = []
    notes = []

    def fail(msg):
        failures.append(msg)

    def note(msg):
        notes.append(msg)

    with tempfile.TemporaryDirectory(prefix="mdkr64-cave-reverb-") as tmp:
        trace_path = os.path.join(tmp, "voices.jsonl")
        rc, out = run_cave(args.build, args.rom, args.script, args.track,
                           args.frames, trace_path, args.timeout)
        if rc != 0:
            print(out[-4000:])
            sys.exit("cave drive exited %d" % rc)
        bad = BAD_RE.findall(out)
        if bad:
            fail("%d bad-log line(s); first: %s" % (len(bad), bad[0]))

        # Assertion 2a: the intended cave actually loaded as a race.
        loaded = ("level_load: levelId=%d numPlayers=0" % args.track) in out
        if not loaded:
            fail("Treasure Caves (levelId=%d) never loaded as a race" % args.track)

        # Assertion 2b: the racer drove into the cave (forward progress).
        pace = PACE_RE.findall(out)
        maxcp = max((int(s[4]) for s in pace), default=0)
        note("racer samples %d, max courseCheckpoint %d" % (len(pace), maxcp))
        if maxcp < 3:
            fail("no forward progress (max courseCheckpoint %d); the racer never "
                 "drove into the cave, so 'mid-cave' cannot be asserted" % maxcp)

        if not os.path.exists(trace_path):
            sys.exit("no voice trace at %s (the MDKR_AUDIO_VOICE_TRACE_JSONL "
                     "seam is not in this binary)" % trace_path)
        rows = [r for r in load_trace(trace_path) if r.get("event") == "fxmix"]
        if not rows:
            fail("voice reverb trace is empty; no SFX/music voice set an FX mix, "
                 "so this run measured nothing")

        # Assertion 1: the synth built two aux buses.
        max_aux = {int(r.get("max_aux", -1)) for r in rows}
        note("aux-bus counts seen in trace: %s" % sorted(max_aux))
        if rows and max_aux != {2}:
            fail("synth reports max_aux=%s; DKR configures two FX buses "
                 "(CUSTOM + BIGROOM) and each must exist for SFX to reach the "
                 "big-room reverb" % sorted(max_aux))

        # Tallies per bus.
        bus_wet = {}
        bus_any = {}
        for r in rows:
            bus = int(r.get("bus", -1))
            fx = int(r.get("fxmix", 0))
            bus_any[bus] = bus_any.get(bus, 0) + 1
            if fx > 0:
                bus_wet[bus] = bus_wet.get(bus, 0) + 1
        note("fxmix lines per bus (all): %s"
             % {b: bus_any[b] for b in sorted(bus_any)})
        note("fxmix>0 lines per bus (wet): %s"
             % {b: bus_wet[b] for b in sorted(bus_wet)})

        # Assertion 3: SFX reach bus 1 with wet send.
        bus1_wet = bus_wet.get(1, 0)
        note("big-room bus (1) wet lines: %d (need >= %d)"
             % (bus1_wet, MIN_BUS1_WET))
        if bus1_wet < MIN_BUS1_WET:
            fail("only %d SFX voice(s) reached the big-room reverb bus (1) with "
                 "a wet send >= 1; need >= %d. Before the fix this is 0 because "
                 "the port built one aux bus and the SFX re-parent to bus 1 was "
                 "rejected, leaving cave SFX on the music reverb (bus 0)."
                 % (bus1_wet, MIN_BUS1_WET))

    for line in notes:
        print(line)
    print("")
    if failures:
        for line in failures:
            print("FAIL: %s" % line)
        print("\ncheck_cave_reverb_bus: FAIL (%d)" % len(failures))
        return 1
    print("check_cave_reverb_bus: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
