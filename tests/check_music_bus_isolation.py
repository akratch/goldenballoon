#!/usr/bin/env python3
"""Prove music (CSP) voices stay on the music reverb bus, never the SFX bus.

Why this exists
---------------
Diddy Kong Racing runs TWO reverb buses (game/src/audio.c):

  * bus 0 = AL_FX_CUSTOM  — the ROM music reverb. Every CSP (music) voice must
    stay on it.
  * bus 1 = AL_FX_BIGROOM — the fixed libaudio big-room reverb. Every SFX voice
    is re-parented onto it at note-on (audiosfx.c, func_80065A80) for the
    cave/tunnel echo.

Physical voices are a SHARED pool (alSynAllocVoice, platform/audio_synth.c). The
allocator sets only the LOGICAL voice's fxBus; it never moves the PHYSICAL
voice's aux bus. SFX re-parent their physical voice to bus 1 and never move it
back, so once a slot has been used by an SFX it is pinned to the big-room bus.
When a later MUSIC note reuses that physical voice, its reverb send follows the
voice onto bus 1 instead of the CUSTOM music reverb — the music leaks into the
SFX echo. Stock DKR guards against exactly this: csplayer.c re-parents every
music voice to bus 0 on note-on, right after a successful alSynAllocVoice. The
port dropped that step; ``maxAuxBusses == 1`` masked it until issue #49 built the
real second bus, which unmasked the leak.

Every other audio gate misses it. check_cave_reverb_bus.py proves SFX REACH bus
1 but says nothing about where the music lands; the level/PCM gates measure the
mix, not per-voice routing. This is the missing music-side oracle.

What it does
------------
Loads Treasure Caves (``MDKR_LOAD_TRACK=30``) and drives it with DKR's own AI
(``MDKR_AUTOPILOT=1``) — a race that plays music AND a steady stream of engine
SFX, so the 40-voice physical pool is recycled between the two owners and music
note-ons routinely land on slots an SFX just vacated on bus 1. The clean-room
synth writes a per-voice routing line to ``MDKR_AUDIO_VOICE_TRACE_JSONL``: an
``"fxmix"`` line whenever the SFX player sets a voice's FX mix, and a ``"music"``
line at every CSP note-on start (alSynStartVoiceParams, a path the SFX player
never uses). Each line carries the aux bus the voice's envmixer is currently a
source of and how many aux buses the synth built.

What it asserts
---------------
 1. The synth built TWO aux buses (``max_aux == 2`` on every line) — the post-#49
    configuration this regression lives in.
 2. Treasure Caves loaded as a race and the racer made forward progress
    (anti-vacuity: a real drive, not a menu).
 3. SFX actually reached the big-room bus (``fxmix`` lines on bus 1) — the
    precondition for the leak. Without SFX parking voices on bus 1 there is
    nothing for music to inherit and the oracle would pass vacuously.
 4. Music voices actually played (a healthy count of ``music`` lines).
 5. THE ORACLE: ZERO ``music`` lines are on bus 1. Every music voice is on the
    CUSTOM music reverb (bus 0).

Self-validation — this check is proven to be able to fail
---------------------------------------------------------
With the stock re-parent dropped, a Treasure Caves drive puts the majority of
music note-ons (measured 5209 of 6252) on bus 1 once SFX have moved pool voices
there, so assertion 5 fails hard — the RED state. Restoring the note-on
re-parent to bus 0 turns it GREEN (every music line on bus 0). Assertions 3 and
4 keep GREEN from being vacuous: the run must contain both SFX-on-bus-1 lines and
music lines for the leak to have had an opportunity to happen.

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), one authored
Original-cadence ticket per host opportunity (MDKR_SYNTH_FIELDS=2), per
tests/README.md.

Usage:
    tests/check_music_bus_isolation.py --build build --rom baserom.us.v80.z64
    tests/check_music_bus_isolation.py --track 30 --frames 6000   # a deeper drive
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

# ASSET_LEVEL_TREASURECAVES (game/include/asset_enums.h): a Sherbet Island cave
# track that plays music over dense engine SFX and carries reverb lines, so both
# reverb buses are live and the shared voice pool is heavily recycled.
TREASURE_CAVES = 30

# Anti-vacuity floors. A full Treasure Caves drive measured ~6252 music note-ons
# and ~11180 SFX FX-mix lines; require a clear margin so a run that measured
# almost nothing (no music, or no SFX on bus 1) fails loudly instead of passing.
MIN_MUSIC_LINES = 100
MIN_SFX_BUS1 = 8

PACE_RE = re.compile(r"racer x=(\S+) y=(\S+) z=(\S+) clock=(\d+) cp=(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"MemorySanitizer|runtime error:|Assertion|\[FX BUG\]"
)


def run_cave(build, rom, script, track, frames, trace_path, timeout):
    """One headless Treasure-Caves drive with the voice routing trace on."""
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
    with tempfile.TemporaryDirectory(prefix="mdkr_music_bus_") as run_dir:
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

    with tempfile.TemporaryDirectory(prefix="mdkr64-music-bus-") as tmp:
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
                 "drove, so voice reuse between music and SFX cannot be asserted"
                 % maxcp)

        if not os.path.exists(trace_path):
            sys.exit("no voice trace at %s (the MDKR_AUDIO_VOICE_TRACE_JSONL "
                     "seam is not in this binary)" % trace_path)
        rows = load_trace(trace_path)
        music = [r for r in rows if r.get("event") == "music"]
        sfx = [r for r in rows if r.get("event") == "fxmix"]
        if not music and not sfx:
            fail("voice routing trace is empty; no music or SFX voice started, "
                 "so this run measured nothing")

        # Assertion 1: the synth built two aux buses.
        max_aux = {int(r.get("max_aux", -1)) for r in (music + sfx)}
        note("aux-bus counts seen in trace: %s" % sorted(max_aux))
        if (music or sfx) and max_aux != {2}:
            fail("synth reports max_aux=%s; DKR configures two FX buses "
                 "(CUSTOM + BIGROOM). This oracle only means something in the "
                 "post-#49 two-bus configuration" % sorted(max_aux))

        # Assertion 3: SFX actually reached the big-room bus (the leak's
        # precondition — voices parked on bus 1 for music to inherit).
        sfx_bus1 = sum(1 for r in sfx if int(r.get("bus", -1)) == 1)
        note("SFX (fxmix) lines on bus 1: %d (need >= %d)"
             % (sfx_bus1, MIN_SFX_BUS1))
        if sfx_bus1 < MIN_SFX_BUS1:
            fail("only %d SFX voice(s) reached the big-room bus (1); need >= %d. "
                 "Without SFX parking pool voices on bus 1 there is nothing for "
                 "music to inherit and this oracle would pass vacuously."
                 % (sfx_bus1, MIN_SFX_BUS1))

        # Assertion 4: music actually played.
        note("music note-on lines: %d (need >= %d)"
             % (len(music), MIN_MUSIC_LINES))
        if len(music) < MIN_MUSIC_LINES:
            fail("only %d music voice(s) started; need >= %d. Without music "
                 "note-ons the routing oracle has nothing to check."
                 % (len(music), MIN_MUSIC_LINES))

        # Assertion 5 (THE ORACLE): no music voice is on the big-room bus.
        music_bus = {}
        for r in music:
            b = int(r.get("bus", -1))
            music_bus[b] = music_bus.get(b, 0) + 1
        note("music note-on lines per bus: %s"
             % {b: music_bus[b] for b in sorted(music_bus)})
        music_bus1 = music_bus.get(1, 0)
        if music_bus1 > 0:
            fail("%d of %d music voice(s) sounded on the big-room SFX bus (1) "
                 "instead of the CUSTOM music reverb (bus 0). A recycled SFX "
                 "physical voice keeps its bus-1 parenting, and the port dropped "
                 "stock DKR's per-note re-parent to bus 0, so the music's reverb "
                 "send leaks into the SFX echo." % (music_bus1, len(music)))

    for line in notes:
        print(line)
    print("")
    if failures:
        for line in failures:
            print("FAIL: %s" % line)
        print("\ncheck_music_bus_isolation: FAIL (%d)" % len(failures))
        return 1
    print("check_music_bus_isolation: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
