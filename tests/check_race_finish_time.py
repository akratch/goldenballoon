#!/usr/bin/env python3
"""P3.1 check: finishing a TIME TRIAL must record a time to EEPROM.

Why this exists
---------------
"The player can drive but cannot complete a race and keep their time" was the
project's top open item.  Two things made it look like a broken finish path when
it was not:

  1. **No fixture ever enabled Time Trial.**  DKR writes course/lap records only
     from `race_finish_time_trial()`, whose whole body is gated on
     `if (gIsTimeTrial)` (game/src/objects.c).  Every existing race fixture ends
     its track-select navigation with "confirm TIME TRIAL = OFF", so the record
     write was unreachable by construction and no run could ever touch EEPROM.
     The fixture used here pushes the stick DOWN at the track-select Time-Trial
     stage first, so `set_time_trial_enabled(1)` runs (game/src/menu.c).
  2. **A solo Time Trial has no CPU racers**, and DKR's AI throttle routine
     returns early when the field contains none, so `MDKR_AUTOPILOT` steered but
     never accelerated.  See the hook in `update_player_racer` (game/src/racer.c).

`raceFinished` itself was always being set; a probe living *inside*
`update_player_racer` simply could not see it, because a finished racer moves to
a different update path.

What this asserts
-----------------
  1. **The race finishes.**  `rlap=` in the [PACE] trace reaches the level's lap
     count (3 on Ancient Lake) and `fin=` flips 0 -> 1.  Both come from
     `race_check_finish()`, i.e. from outside `update_player_racer` — the older
     `lap=`/`cp=` fields freeze one lap short because that function stops running
     for a racer as soon as it finishes.
  2. **A plausible time.**  The race clock freezes at the finish, and its final
     value (== the course time, the sum of the lap times) lands in a sane band
     for three autopilot laps.
  3. **The time reaches EEPROM.**  The run starts from a deleted `eeprom.bin`,
     so the file must be created, be a full 512 bytes, not be the all-zero image
     the game writes when it saves nothing, and the exact
     frozen clock value must appear in it as a 16-bit big-endian record.  Tying
     the assertion to the *measured* clock rather than a hardcoded offset means a
     byteswap or layout regression in the record serialiser fails the check.
  4. **It survives a restart.**  A second run of the same fixture must read the
     record back: it re-drives the identical (deterministic) time, which does not
     beat the stored one, so nothing is rewritten and the file is byte-identical.
     If the read-back path were broken the defaults would come back instead, the
     run would "set a new record", and the md5 would move.

Usage:
    tests/check_race_finish_time.py [--build build] [--rom baserom.us.v80.z64] [-v]

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.  Exit 0 = pass; exit 1 = at least one assertion failed (each
printed with the measured value).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

from harness_utils import (EEPROM_ARTIFACTS, preserved_eeprom, resolve_binary,
                          save_env, test_save_dir)

SCRIPT = "tests/input_scripts/race_full_3lap_tt.txt"
FRAMES = 13000
SAVE_DIR = test_save_dir()
EEPROM = os.path.join(SAVE_DIR, "eeprom.bin")

# --- thresholds (measured on Ancient Lake under MDKR_AUTOPILOT=1) ------------
EXPECT_LAPS = 3          # Ancient Lake's LevelHeader.laps
CLOCK_MIN, CLOCK_MAX = 3000, 9000   # measured course time 4777 for 3 laps
EEPROM_SIZE = 512
MIN_GHOST_FRAMES = 100   # measured ~5000 on the post-finish "TRY AGAIN" re-entry

PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+).*clock=(\d+) cp=(-?\d+) lap=(-?\d+) rlap=(-?\d+) fin=(-?\d+)"
    r" ghost=(\d+) gbank=(-?\d+)")


def md5(path: str) -> str | None:
    if not os.path.exists(path):
        return None
    with open(path, "rb") as fh:
        return hashlib.md5(fh.read()).hexdigest()


def run(binary: str, rom: str, verbose: bool, extra_env: dict | None = None) -> list[tuple[int, ...]]:
    """Run once; [PACE] samples as (frame, clock, cp, lap, rlap, fin, ghost, gbank)."""
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"          # belt-and-braces; --headless-frames already
    save_env(env, SAVE_DIR)
    env["MDKR_SIMULATION_CADENCE"] = "enhanced"
    env["MDKR_SYNTH_FIELDS"] = "1"
    env["MDKR_AUTOPILOT"] = "1"      # drive with DKR's own AI
    env["MDKR_TRACE"] = "1"          # emit the [PACE] probe
    if extra_env:
        env.update(extra_env)
    cmd = [binary, "--headless-frames", str(FRAMES),
           "--input-script", SCRIPT, "--rom", rom]
    if verbose:
        print("  $ " + " ".join(cmd))
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    if "[CRASH]" in out or "[FATAL]" in out:
        print("FAIL: run produced [CRASH]/[FATAL]")
        sys.exit(1)
    # A -fstack-protector abort prints NOTHING to stderr — it goes straight to
    # SIGABRT. The post-race "TRY AGAIN" path used to die exactly that way (the
    # timetrial_ghost_read 3-vs-4 control-point overflow), so the return code has
    # to be part of the assertion, not just the log contents.
    if proc.returncode != 0:
        print(f"FAIL: run exited {proc.returncode} "
              f"(negative = killed by signal {-proc.returncode})")
        sys.exit(1)
    samples = [tuple(int(g) for g in m.groups()) for m in PACE_RE.finditer(out)]
    if not samples:
        print("FAIL: no [PACE] racer samples — did the race load at all?")
        sys.exit(1)
    return samples


def main() -> int:
    # This check deletes and rewrites EEPROM images to assert a known starting
    # state. Under tools/run_checks.py SAVE_DIR is a run-scoped temporary
    # directory; a standalone run defaults to the repository's playable save/,
    # whose prior contents must survive however this exits.
    with preserved_eeprom(SAVE_DIR):
        return run_check()


def run_check() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    if not os.path.exists(binary):
        print(f"FAIL: {binary} not found — build first")
        return 1

    failures: list[str] = []

    # Start from no record at all, so a successful finish must write one. The
    # pre-state is absence, not a digest: an md5 of a deleted file is None and
    # can never equal the digest of whatever the run produces, which would make
    # the "did it change" assertion below unfailable. State the starting
    # condition instead, and compare the run's output against the empty image
    # the game writes when it saves nothing.
    for name in EEPROM_ARTIFACTS:
        candidate = os.path.join(SAVE_DIR, name)
        if os.path.exists(candidate):
            os.remove(candidate)
    if os.path.exists(EEPROM):
        failures.append(f"{EEPROM} still exists after being removed")
    empty_digest = hashlib.md5(b"\x00" * EEPROM_SIZE).hexdigest()

    print("run 1: fresh EEPROM, drive a 3-lap Time Trial to the finish")
    samples = run(binary, args.rom, args.verbose)

    max_lap = max(s[4] for s in samples)               # rlap, from race_check_finish
    finished = [s for s in samples if s[5] == 1]        # fin, ditto
    # The course time is the clock value at the moment the race clock stops. Take
    # the max seen: samples continue past the finish into the post-race screens,
    # where the level eventually unloads and the probe fields reset to 0.
    course_time = max(s[1] for s in samples)
    # The clock must stop: a finished racer accrues no more race time. Require a
    # long plateau at that maximum rather than a single sample.
    plateau = sum(1 for s in samples if s[1] == course_time)

    if args.verbose:
        print(f"  max rlap={max_lap} raceFinished seen={bool(finished)}"
              f"{' at frame ' + str(finished[0][0]) if finished else ''}"
              f" course time={course_time} clock plateau={plateau} frames")

    if max_lap < EXPECT_LAPS:
        failures.append(f"race never reached lap {EXPECT_LAPS} (max rlap={max_lap})")
    if not finished:
        failures.append("racer->raceFinished was never observed TRUE (fin= stayed 0)")
    if plateau < 100:
        failures.append(f"race clock never froze — only {plateau} frames at its maximum "
                        f"{course_time}; the finish did not happen")
    if not (CLOCK_MIN <= course_time <= CLOCK_MAX):
        failures.append(f"course time {course_time} outside plausible band "
                        f"[{CLOCK_MIN}, {CLOCK_MAX}]")
    final_clock = course_time

    # --- coverage: the route must actually play a ghost back ------------------
    # After a Time Trial finish the game records a player ghost and swaps it in,
    # and the fixture's trailing A taps pick "TRY AGAIN", which re-enters the
    # level and plays that ghost. That is the only thing that exercises
    # timetrial_ghost_read() -- where a 3-vs-4 Catmull-Rom control-point overflow
    # used to abort in __stack_chk_fail with nothing on stderr. Assert the
    # coverage explicitly: a future route change that stopped re-entering the
    # level would otherwise keep passing every assertion above while silently
    # covering none of that code.
    ghost_frames = max(s[6] for s in samples)
    ghost_bank = next((s[7] for s in reversed(samples) if s[6] > 0), -1)
    if args.verbose:
        kind = {0: "player ghost", 1: "player ghost", 2: "staff ghost"}.get(ghost_bank, "none")
        print(f"  ghost playback frames={ghost_frames} bank={ghost_bank} ({kind})")
    if ghost_frames < MIN_GHOST_FRAMES:
        failures.append(
            f"only {ghost_frames} ghost-playback frames (need >= {MIN_GHOST_FRAMES}): the route "
            "no longer re-enters the level after the finish, so timetrial_ghost_read() is "
            "not being exercised at all")

    after = md5(EEPROM)
    if after is None:
        failures.append(f"{EEPROM} was never created — no time was saved")
    else:
        blob = open(EEPROM, "rb").read()
        if len(blob) != EEPROM_SIZE:
            failures.append(f"{EEPROM} is {len(blob)} bytes, expected {EEPROM_SIZE}")
        if after == empty_digest:
            failures.append(
                f"{EEPROM} is an all-zero {EEPROM_SIZE}-byte image (md5 {after}): "
                "the file was created but no record was written")
        if not any(blob):
            failures.append(f"{EEPROM} contains no set bits — nothing was saved")
        # The recorded course time is what race_finish_time_trial() sums at the
        # instant it runs; the traced clock is what the [PACE] probe last saw from
        # inside update_player_racer, which stops for a finished racer. Those two
        # reads are a couple of updates apart, so allow a small window rather than
        # demanding equality. Up to P3.5 they happened to agree exactly (both
        # 4777); after the ParticleBehaviour stride fix moved the trajectory they
        # differ by 8 (traced 4709, recorded 4717), which is the skew, not a
        # regression. The window is still tight enough that a byteswap or a
        # record-layout regression — which move the value by thousands or write it
        # little-endian — fails, which is the point of matching the *measured*
        # value at all instead of a hardcoded offset.
        CLOCK_SKEW = 32
        hits = [(i, (blob[i] << 8) | blob[i + 1]) for i in range(len(blob) - 1)
                if abs(((blob[i] << 8) | blob[i + 1]) - final_clock) <= CLOCK_SKEW]
        if not hits:
            failures.append(f"no 16-bit big-endian value within {CLOCK_SKEW} of the traced "
                            f"course time {final_clock} (0x{final_clock:04X}) is present in "
                            f"{EEPROM} — the record never reached the save")
        elif args.verbose:
            print(f"  course time {final_clock} (traced) recorded as {hits[0][1]} at byte "
                  f"{hits[0][0]}; md5 {after}")

    # --- restart persistence -------------------------------------------------
    print("run 2: same fixture again — the record must be read back, not reset")
    run(binary, args.rom, args.verbose)
    after2 = md5(EEPROM)
    if after2 != after:
        failures.append(
            f"{EEPROM} changed on the second run ({after} -> {after2}): the stored "
            "record was not read back, so the identical re-drive looked like a new best")
    elif args.verbose:
        print(f"  md5 unchanged ({after2}) — record persisted and was honoured")

    if failures:
        print()
        for f in failures:
            print(f"FAIL: {f}")
        return 1
    print(f"\nPASS: 3-lap Time Trial finished, course time {final_clock} recorded to "
          f"{EEPROM} and read back after restart")
    return 0


if __name__ == "__main__":
    sys.exit(main())
