#!/usr/bin/env python3
"""Record a Time Trial ghost and read it back in a FRESH process, per legal pair.

Why this exists
---------------
`tests/check_race_finish_time.py` is the only gate that has ever exercised
`timetrial_ghost_read()`, and it drives exactly ONE of the 47 legal
(track, vehicle) combinations: Ancient Lake in the car. That single row is how
the 3-vs-4 Catmull-Rom control-point overflow was found at all, and it was found
by accident -- the fixture happened to re-enter the level after the finish, which
is the only thing that plays a recorded ghost back. The abort it produced was
SIGABRT out of `__stack_chk_fail` with **nothing at all on stderr**: no
`[CRASH]`, no `[FATAL]`, no sanitizer line. A check that greps for crash markers
cannot see that class of failure; only the exit code and the presence of the
markers a *successful* run must emit can.

Ghosts are also the one piece of player data that persists keyed by the PAIR
rather than by the track. `func_80074B34()` / `func_80075000()` (game/src/save_data.c)
both match a Controller Pak ghost slot on `levelId` **and** `vehicleId`:

    if ((levelId == ghostDataBody[i].unk0) && (vehicleId == ghostDataBody[i].unk1)) {

so "Ancient Lake in a car round-trips" says nothing about the other 46 rows. This
check closes that gap: every legal pair records a ghost, saves it, and a
**separate process** loads it back and plays it.

The three runs per pair
-----------------------
Each pair gets its own `MDKR_SAVE_DIR`, so no pair can see another's EEPROM or
Controller Pak, and the 6-slot pak ghost directory (`DKR_GHOST_SLOT_COUNT`,
save_data.c) is never contended -- 47 ghosts do not fit on one pak. The
contended case is owned by tests/check_ghost_bank_capacity.py, which drives
more pairs than the window holds against ONE shared save directory and gates
platform/ghost_bank.c's window swapping (issue #46); this check stays about
per-pair round-tripping.

  1. **measure** -- drive the Time Trial to the finish with plain A taps and read
     the frame at which the post-race OPTIONS stage opens offering SAVE GHOST.
     A three-lap finish lands anywhere from ~7500 (Ancient Lake, car) to ~12500
     (DarkMoon Caverns, car) frames depending on the pair, so a fixed input
     script cannot aim at a menu row across 47 rows; the `[TTGHOST] event=options`
     probe (game/src/menu.c) is what makes the aim a measurement. This run also
     carries the **in-process** coverage: its trailing A taps pick TRY AGAIN,
     re-entering the level and playing back the ghost just recorded, which is the
     original overflow route.
  2. **write** -- from a fresh save directory, re-drive the identical
     (deterministic) race and steer the measured menu onto SAVE GHOST, which is
     the only route to `timetrial_save_player_ghost()`. Asserts
     `[TTGHOST] event=save ... status=0` with a non-empty node count, and that
     the Controller Pak image appeared on disk.
  3. **read** -- a FRESH process against that same save directory. On level entry
     `timetrial_init_player_ghost()` pulls the ghost back off the pak; the check
     asserts `[TTGHOST] event=load` reports the SAME pair, the SAME node count,
     character and time that were written, and that ghost playback actually ran
     from a player bank.

What "silent abort" means for the assertions
--------------------------------------------
Every run asserts the exit code AND that the markers a healthy run emits are
present -- the traced finish, the options probe, the save/load rows. "No crash
marker appeared" is never sufficient, because the defect class this exists for
produces no marker.

Pacing, and one documented non-producer
---------------------------------------
Whether the autopilot completes a lap at all is pacing-dependent in BOTH
directions -- neither simulation cadence laps the whole matrix (see CADENCE_ARMS
for the four measured rows). So each pair is driven on the first cadence that
gets it round, and the run reports which one; pinning a single pacing would
silently cost coverage, and on Enhanced alone Frosty Village -- a
hovercraft-only track -- has nothing that can lap it.

Spaceport Alpha (15) **in the car** completes no lap on either cadence: its
autopilot racing line dead-ends at `courseCheckpoint` 10, and DKR only records a
ghost for a course time under 10800 frames (`objects.c`
race_finish_time_trial). The pair is legal -- the ROM's `available_vehicles`
mask offers the car -- so it is not skipped: it is asserted to behave exactly
that way, and the check fails if it ever starts finishing (meaning it can be
promoted) or if it starts aborting.

Usage:
    tests/check_ghost_matrix.py                  # all 47 legal pairs
    tests/check_ghost_matrix.py --subset         # one row per track and vehicle
    tests/check_ghost_matrix.py --pairs 5:0,15:2
    tests/check_ghost_matrix.py --matrix         # print the matrix and exit

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md. Exit 0 = every pair round-tripped its ghost.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from typing import NamedTuple

from check_vehicle_sweep import MAIN_TRACK_IDS, VEHICLE_NAMES, rom_vehicle_matrix
from harness_utils import DEFAULT_BUILD_DIR, find_fatal, resolve_binary, save_env

# The post-race navigation prefix of tests/input_scripts/race_full_3lap_tt.txt:
# reach track select, then push DOWN at the Time-Trial stage so
# set_time_trial_enabled(1) runs. Without that DOWN the race is an ordinary
# Tracks race, gIsTimeTrial is false, and nothing records a ghost at all.
# Kept here rather than read from the fixture because the trailing A taps have to
# be generated per pair (see the module docstring), and a half-file-half-generated
# script would hide which half aimed the menu.
NAV_PREFIX = """\
# Generated by tests/check_ghost_matrix.py -- navigation to a solo TIME TRIAL,
# identical to the prefix of tests/input_scripts/race_full_3lap_tt.txt.
1250 START 4
1330 START 4
1450 A 4
1540 A 4
1750 A 4
1900 DOWN 4
2000 A 4
2150 A 4
2320 A 4
2400 DOWN 6
2460 A 4
2600 A 4
"""

# The earliest three-lap finish over the whole matrix is ~5600 frames, and the
# navigation prefix ends at 2600, so taps from here can neither land inside the
# navigation nor be exhausted before the slowest pair (~12500) finishes. The
# autopilot drives regardless of a tapped A, which is why one cadence of taps
# serves every pair.
TAP_START = 5000
TAP_STEP = 24
BUDGET = 20000          # frames: covers the latest measured finish (~12500) plus
                        # the post-race walk, the ghost save, and a re-entry
READ_BUDGET = 9000      # the read run only has to load the level and play back
TIMEOUT = 420

PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+).*clock=(\d+) cp=(-?\d+) lap=(-?\d+) rlap=(-?\d+) fin=(-?\d+)"
    r" ghost=(\d+) gbank=(-?\d+)")
OPTIONS_RE = re.compile(
    r"\[TTGHOST\] frame=(\d+) event=options option=(\d+) count=(\d+) hasghost=(\d+)")
GHOST_IO_RE = re.compile(
    r"\[TTGHOST\] frame=(\d+) event=(save|load) level=(\d+) vehicle=(\d+) status=(-?\d+)"
    r" nodes=(-?\d+) character=(-?\d+) time=(-?\d+)")

CONTROLLER_PAK_GOOD = 0          # game/src/save_data.h SIDeviceStatus
PAK_IMAGE = "controller-pak-1.mdp"
MIN_GHOST_FRAMES = 100           # as tests/check_race_finish_time.py


class CadenceArm(NamedTuple):
    """A pacing the race can be driven under."""

    cadence: str
    synth_fields: str

    def __str__(self) -> str:
        return self.cadence


# Whether the autopilot can complete a lap at all is pacing-dependent, and it is
# dependent in BOTH directions -- neither cadence laps the whole matrix:
#
#   track 4 hovercraft    Original finishes frame 5614; Enhanced stalls at
#                         rlap 1 / cp 25 over a 40000-frame budget
#   track 33 car          Original finishes frame 6837; Enhanced stalls at
#                         rlap 1 / cp 52
#   track 19 hovercraft   Enhanced finishes; Original stalls at rlap 0
#   track 30 plane        Enhanced finishes; Original stalls at rlap 0
#   track 33 hovercraft   Enhanced finishes; Original stalls at rlap 0
#
# Track 33 appears on both sides, which is the point: this splits per PAIR, not
# per track. Track 4 offers only the hovercraft, so on Enhanced alone that is a
# whole track nothing can lap. So the pair is driven on the first arm that gets
# it round and
# the arm used is reported: pinning one pacing would silently cost coverage, and
# picking per pair keeps this check about ghosts rather than about pacing. The
# order puts check_vehicle_sweep.py's pacing -- the sweep that owns this matrix --
# first, so the common case matches the sibling check.
CADENCE_ARMS = (CadenceArm("original", "2"), CadenceArm("enhanced", "1"))

# Pairs no arm can complete; see the module docstring. Asserted, never skipped.
NO_GHOST_PAIRS = {
    (15, 0): "Spaceport Alpha in the car: the autopilot racing line dead-ends at "
             "courseCheckpoint 10 and completes no lap in 40000 frames, on BOTH "
             "simulation cadences",
}


def find_ghost_header(image: bytes, character: int, time_: int, nodes: int) -> int:
    """Where the serialised GhostHeader for this ghost sits in a pak image, or -1.

    GhostHeader (game/include/structs.h) is 8 bytes: `s16 checksum`, then
    `u8 characterID` + `u8 unk3`, then `s16 time`, then `s16 nodeCount`. The
    checksum is not searched for -- it is derived from the payload, so requiring
    it would only restate what the engine computed.

    The pak image holds the struct exactly as the machine laid it out, which is
    what an N64 Controller Pak holds too (raw big-endian N64 structs); on a
    little-endian host the same design yields little-endian fields. Hence
    ``sys.byteorder`` rather than a fixed order: the assertion is "the struct
    round-tripped verbatim", not "the port picked an endianness".
    """

    pattern = (bytes((character & 0xFF, 0))
               + (time_ & 0xFFFF).to_bytes(2, sys.byteorder)
               + (nodes & 0xFFFF).to_bytes(2, sys.byteorder))
    return image.find(pattern)


class Run:
    """One child process, parsed."""

    def __init__(self, proc: subprocess.CompletedProcess, seconds: float):
        self.rc = proc.returncode
        self.out = (proc.stdout or "") + (proc.stderr or "")
        self.seconds = seconds
        self.pace = [tuple(int(g) for g in m.groups())
                     for m in PACE_RE.finditer(self.out)]
        self.options = [tuple(int(g) for g in m.groups())
                        for m in OPTIONS_RE.finditer(self.out)]
        self.io = [(int(f), kind, int(lvl), int(veh), int(st), int(n), int(c), int(t))
                   for f, kind, lvl, veh, st, n, c, t
                   in (m.groups() for m in GHOST_IO_RE.finditer(self.out))]

    @property
    def finished(self) -> bool:
        return any(s[5] == 1 for s in self.pace)

    @property
    def max_rlap(self) -> int:
        return max((s[4] for s in self.pace), default=-1)

    @property
    def ghost_frames(self) -> int:
        return max((s[6] for s in self.pace), default=0)

    @property
    def ghost_banks(self) -> set[int]:
        """Every bank ghost playback was seen coming from.

        A set, not "the last one": several tracks ship a staff ghost, and on
        those the player's own ghost and the staff ghost are both live objects
        being interpolated. Whichever `[PACE]` row happens to be last then
        reports bank 2, which would read as "the player ghost never played" when
        it plainly did. What this check needs is that a PLAYER bank was reached
        at some point, so collect them all and test membership.
        """
        return {s[7] for s in self.pace if s[6] > 0}

    def events(self, kind: str) -> list[tuple]:
        return [e for e in self.io if e[1] == kind]

    def fatal(self, label: str) -> list[str]:
        """Everything that means this run died rather than disagreed.

        The exit code is checked FIRST and unconditionally: the defect this file
        exists for prints nothing, so a log-only verdict would pass it.
        """
        problems = []
        if self.rc != 0:
            problems.append(
                f"{label}: exited {self.rc}"
                + (f" (killed by signal {-self.rc})" if self.rc < 0 else "")
                + " -- a -fstack-protector abort prints NOTHING to stderr, so this "
                  "is the whole evidence for that failure class")
        marker = find_fatal(self.out)
        if marker:
            problems.append(f"{label}: log contains {marker!r}")
        if not self.pace:
            problems.append(f"{label}: no [PACE] samples -- did the race load at all?")
        return problems


def run_child(binary: str, rom: str, script: str, save_dir: str, level: int,
              vehicle: int, frames: int, verbose: bool, arm: "CadenceArm") -> Run:
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"          # belt-and-braces; --headless-frames already
    env["MDKR_PRESENT_RATE"] = "original"
    env["MDKR_SIMULATION_CADENCE"] = arm.cadence
    env["MDKR_SYNTH_FIELDS"] = arm.synth_fields
    env["MDKR_AUTOPILOT"] = "1"      # drive with DKR's own AI
    # The autopilot inherits the AI's stuck-recovery cooldown deadlock
    # (racer.c unk215; see mdkr_autopilot_unstick). 19:hovercraft wedges
    # deterministically at checkpoint 26 on both pre- and post-campaign
    # trees -- pre-campaign only escaped by cadence luck on the retry arm.
    # Unscoped opt-in, same as check_race_multiplayer: any cell of a
    # twenty-track matrix may wedge, and this gate's subject is ghosts,
    # not wedge geography.
    env["MDKR_AUTOPILOT_UNSTICK"] = "1"
    env["MDKR_TRACE"] = "1"          # emit [PACE] and [TTGHOST]
    env["MDKR_LOAD_TRACK"] = f"{level}:{vehicle}"
    save_env(env, save_dir)
    cmd = [binary, "--headless-frames", str(frames),
           "--input-script", script, "--rom", rom]
    if verbose:
        print("    $ MDKR_LOAD_TRACK=%d:%d MDKR_SIMULATION_CADENCE=%s "
              "MDKR_SYNTH_FIELDS=%s %s"
              % (level, vehicle, arm.cadence, arm.synth_fields, " ".join(cmd)))
    started = time.time()
    try:
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True,
                              timeout=TIMEOUT)
    except subprocess.TimeoutExpired as expired:
        proc = subprocess.CompletedProcess(
            cmd, -1, (expired.stdout or b"").decode("utf-8", "replace")
            if isinstance(expired.stdout, bytes) else (expired.stdout or ""), "")
    return Run(proc, time.time() - started)


def generate_script(path: str, budget: int, options_frame: int | None = None,
                    option_index: int = 0) -> None:
    """The navigation prefix plus the post-race taps this run needs.

    With ``options_frame`` the taps stop short of the OPTIONS stage and the menu
    is steered onto SAVE GHOST instead of confirming its first row (TRY AGAIN).
    After a successful save the game resets the highlight to row 0 and drops the
    SAVE GHOST row (menu.c), so plain taps resume and re-enter the level -- the
    write run therefore covers playback too.
    """
    lines = [NAV_PREFIX]
    limit = budget if options_frame is None else options_frame - 8
    frame = TAP_START
    while frame < limit:
        lines.append(f"{frame} A 5\n")
        frame += TAP_STEP
    if options_frame is not None:
        frame = options_frame + 12
        for _ in range(option_index):
            lines.append(f"{frame} DOWN 4\n")   # one edge per row moved down
            frame += 12
        frame += 12
        lines.append(f"{frame} A 5\n")          # confirm SAVE GHOST
        frame += 90                             # the save runs 5 frames later
        while frame < budget:
            lines.append(f"{frame} A 5\n")
            frame += TAP_STEP
    with open(path, "w") as handle:
        handle.write("".join(lines))


def check_pair(binary: str, rom: str, level: int, vehicle: int, workdir: str,
               verbose: bool) -> tuple[list[str], str, float]:
    """Returns (failures, one-line detail, seconds)."""
    failures: list[str] = []
    tag = f"{level}_{vehicle}"
    elapsed = 0.0
    save_measure = os.path.join(workdir, f"save_m_{tag}")
    save_pair = os.path.join(workdir, f"save_p_{tag}")

    # --- 1. measure -----------------------------------------------------------
    # Try each pacing until one gets this pair round; see CADENCE_ARMS.
    script = os.path.join(workdir, f"measure_{tag}.txt")
    generate_script(script, BUDGET)
    attempts: list[tuple[CadenceArm, Run]] = []
    measure = None
    arm = CADENCE_ARMS[0]
    for candidate in CADENCE_ARMS:
        shutil.rmtree(save_measure, ignore_errors=True)
        attempt = run_child(binary, rom, script, save_measure, level, vehicle,
                            BUDGET, verbose, candidate)
        elapsed += attempt.seconds
        attempts.append((candidate, attempt))
        # A run that DIED is a failure of this check whichever arm it was on --
        # never fall through to the next pacing and let a crash look like "that
        # cadence just could not lap it".
        died = attempt.fatal(f"measure[{candidate}]")
        if died:
            return failures + died, f"died on {candidate}", elapsed
        if attempt.finished:
            arm, measure = candidate, attempt
            break

    if (level, vehicle) in NO_GHOST_PAIRS:
        # A documented non-producer. Assert exactly that, so the row stays
        # falsifiable in both directions instead of being skipped.
        reason = NO_GHOST_PAIRS[(level, vehicle)]
        if measure is not None:
            failures.append(
                f"{reason} -- but this run FINISHED on {arm} (rlap={measure.max_rlap}). "
                "The pair can now produce a ghost: remove it from NO_GHOST_PAIRS so it "
                "is covered like the rest of the matrix")
        laps = ", ".join(f"{a}: rlap={r.max_rlap}" for a, r in attempts)
        return failures, f"documented non-producer ({laps})", elapsed

    if measure is None:
        laps = ", ".join(f"{a}: rlap={r.max_rlap}" for a, r in attempts)
        failures.append(
            f"measure: the Time Trial never finished on any pacing ({laps}); no ghost "
            "can be recorded from an unfinished run")
        return failures, "did not finish", elapsed
    # The in-process half of the coverage: the trailing taps pick TRY AGAIN,
    # which re-enters the level and plays back what was just recorded. This is
    # the exact route the control-point overflow aborted on.
    if measure.ghost_frames < MIN_GHOST_FRAMES:
        failures.append(
            f"measure: only {measure.ghost_frames} ghost-playback frames "
            f"(need >= {MIN_GHOST_FRAMES}): the run never re-entered the level, so "
            "timetrial_ghost_read() was not exercised for this pair at all")
    elif not (measure.ghost_banks & {0, 1}):
        failures.append(
            f"measure: ghost playback only ever came from bank(s) "
            f"{sorted(measure.ghost_banks)}; the player's own recorded ghost (bank 0 or "
            "1) never played, so only the authored staff ghost was exercised")

    offered = [o for o in measure.options if o[3] == 1]
    if not offered:
        failures.append(
            "measure: the post-race OPTIONS stage never offered SAVE GHOST "
            "(no [TTGHOST] event=options with hasghost=1), so this pair's ghost "
            "cannot be persisted")
        return failures, "no SAVE GHOST offered", elapsed
    options_frame, _option, option_count, _has = offered[0]
    # menu.c inserts SAVE GHOST at the old last row and pushes QUIT after it, so
    # it is always the penultimate row of the widened list.
    save_index = option_count - 2
    if save_index < 1:
        failures.append(f"measure: option count {option_count} leaves no SAVE GHOST row")
        return failures, "bad option list", elapsed

    # --- 2. write -------------------------------------------------------------
    # A fresh save directory: the measure run already banked this time, and
    # race_finish_time_trial() only offers a ghost for a NEW best.
    script = os.path.join(workdir, f"write_{tag}.txt")
    generate_script(script, BUDGET, options_frame=options_frame,
                    option_index=save_index)
    write = run_child(binary, rom, script, save_pair, level, vehicle, BUDGET,
                      verbose, arm)
    elapsed += write.seconds
    failures += write.fatal("write")

    saves = write.events("save")
    if not saves:
        failures.append(
            "write: timetrial_save_player_ghost() never ran -- no [TTGHOST] event=save. "
            f"The menu was steered to row {save_index} of {option_count} at frame "
            f"{options_frame}; the option list must have moved")
        return failures, "ghost never saved", elapsed
    _f, _k, s_level, s_vehicle, s_status, s_nodes, s_char, s_time = saves[-1]
    if s_status != CONTROLLER_PAK_GOOD:
        failures.append(
            f"write: ghost save returned status {s_status}, expected "
            f"{CONTROLLER_PAK_GOOD} (CONTROLLER_PAK_GOOD)")
    if (s_level, s_vehicle) != (level, vehicle):
        failures.append(
            f"write: ghost was saved under pair ({s_level}, {s_vehicle}), not the "
            f"({level}, {vehicle}) that was raced -- the pak slot key is wrong")
    if s_nodes <= 0:
        failures.append(
            f"write: ghost payload is {s_nodes} nodes -- an empty ghost round-trips "
            "trivially and proves nothing")
    pak = os.path.join(save_pair, PAK_IMAGE)
    pak_after_write = None
    if not os.path.exists(pak):
        failures.append(f"write: {PAK_IMAGE} was never created in {save_pair}")
    else:
        image = open(pak, "rb").read()
        pak_after_write = hashlib.md5(image).hexdigest()
        # The serialised GhostHeader, located by its MEASURED contents rather
        # than a hardcoded offset -- the same discipline check_race_finish_time.py
        # uses for the EEPROM course record, and for the same reason: a layout or
        # byte-order regression in the ghost serialiser moves these bytes and
        # fails here, where an offset-keyed probe would keep passing.
        if find_ghost_header(image, s_char, s_time, s_nodes) < 0:
            failures.append(
                f"write: the serialised GhostHeader for character={s_char} time={s_time} "
                f"nodes={s_nodes} is not present in {PAK_IMAGE} -- the record the trace "
                "reported was not the record written to disk")

    # --- 3. read, in a FRESH process ------------------------------------------
    script = os.path.join(workdir, f"read_{tag}.txt")
    generate_script(script, READ_BUDGET)
    read = run_child(binary, rom, script, save_pair, level, vehicle,
                     READ_BUDGET, verbose, arm)
    elapsed += read.seconds
    failures += read.fatal("read")

    # The load that matters is the one at level entry that reports the payload
    # (the query form; the re-entry form passes NULL and traces nodes=-1).
    loads = [e for e in read.events("load") if e[5] >= 0]
    if not loads:
        failures.append(
            "read: no [TTGHOST] event=load reported a payload in the fresh process -- "
            "timetrial_init_player_ghost() never pulled the ghost off the pak")
        return failures, "ghost never loaded back", elapsed
    _f, _k, l_level, l_vehicle, l_status, l_nodes, l_char, l_time = loads[0]
    if l_status != CONTROLLER_PAK_GOOD:
        failures.append(
            f"read: fresh-process ghost load returned status {l_status}, expected "
            f"{CONTROLLER_PAK_GOOD} -- the ghost written last run did not come back")
    if (l_level, l_vehicle) != (level, vehicle):
        failures.append(
            f"read: loaded a ghost keyed ({l_level}, {l_vehicle}) for a "
            f"({level}, {vehicle}) race")
    # The payload comparison. nodeCount/character/time are the whole of
    # GhostHeader apart from its checksum, so equality here is the serialised
    # record surviving a process boundary byte-for-byte in every field the
    # format carries.
    if (l_nodes, l_char, l_time) != (s_nodes, s_char, s_time):
        failures.append(
            f"read: ghost came back as nodes={l_nodes} character={l_char} time={l_time}, "
            f"but nodes={s_nodes} character={s_char} time={s_time} was written -- the "
            "serialised record did not survive the process boundary intact")
    if read.ghost_frames < MIN_GHOST_FRAMES:
        failures.append(
            f"read: the reloaded ghost produced only {read.ghost_frames} playback "
            f"frames (need >= {MIN_GHOST_FRAMES}) -- it loaded but never played")
    elif not (read.ghost_banks & {0, 1}):
        failures.append(
            f"read: playback only ever came from bank(s) {sorted(read.ghost_banks)}; the "
            "ghost reloaded from the pak never played")
    # Reading a ghost back must not rewrite it. The read run re-drives the same
    # deterministic race, which cannot beat the stored time, so the pak has to
    # come out byte-identical -- the same argument check_race_finish_time.py
    # makes about the EEPROM image on its second run.
    if pak_after_write is not None and os.path.exists(pak):
        pak_after_read = hashlib.md5(open(pak, "rb").read()).hexdigest()
        if pak_after_read != pak_after_write:
            failures.append(
                f"read: {PAK_IMAGE} changed on the reload run ({pak_after_write} -> "
                f"{pak_after_read}) -- an identical re-drive rewrote stored player data")

    detail = (f"nodes={s_nodes} time={s_time} char={s_char} "
              f"playback={measure.ghost_frames}/{read.ghost_frames} frames")
    if arm != CADENCE_ARMS[0]:
        # Name the fallback explicitly: a pair that only laps on the second
        # pacing is a standing observation about the AI, not a detail to bury.
        detail += f"  [{arm} cadence]"
    return failures, detail, elapsed


def re_entry_case(binary: str, rom: str, level: int, vehicle: int, workdir: str,
                  verbose: bool) -> list[str]:
    """write -> reload level -> read -> re-enter -> read again, on one pair.

    The original overflow was only ever observed because a fixture re-entered a
    level a second time and read a ghost it had already read once. The per-pair
    protocol above reads once per process; this drives the repeat explicitly, so
    a defect that needs the SECOND read to show up cannot hide behind it.
    """
    failures: list[str] = []
    tag = f"reentry_{level}_{vehicle}"
    save_dir = os.path.join(workdir, f"save_{tag}")

    script = os.path.join(workdir, f"{tag}_measure.txt")
    generate_script(script, BUDGET)
    # Same first-cadence-that-laps-it search as check_pair, so naming any pair
    # here works rather than only the ones Original happens to get round.
    measure = None
    arm = CADENCE_ARMS[0]
    for candidate in CADENCE_ARMS:
        shutil.rmtree(save_dir, ignore_errors=True)
        attempt = run_child(binary, rom, script, save_dir, level, vehicle, BUDGET,
                            verbose, candidate)
        died = attempt.fatal(f"re-entry measure[{candidate}]")
        if died:
            return failures + died
        if attempt.finished:
            arm, measure = candidate, attempt
            break
    if measure is None:
        return failures + ["re-entry: the named pair finished on no cadence"]
    offered = [o for o in measure.options if o[3] == 1]
    if not offered:
        return failures + ["re-entry: SAVE GHOST was never offered"]

    shutil.rmtree(save_dir, ignore_errors=True)
    script = os.path.join(workdir, f"{tag}_write.txt")
    generate_script(script, BUDGET, options_frame=offered[0][0],
                    option_index=offered[0][2] - 2)
    write = run_child(binary, rom, script, save_dir, level, vehicle, BUDGET,
                      verbose, arm)
    failures += write.fatal("re-entry write")
    if not write.events("save"):
        return failures + ["re-entry: the ghost was never saved"]

    # A long fresh run against the saved pak: it loads the ghost, plays it, and
    # the trailing taps pick TRY AGAIN so the level is entered a second and third
    # time, each of which reads the same ghost again.
    script = os.path.join(workdir, f"{tag}_read.txt")
    generate_script(script, BUDGET)
    read = run_child(binary, rom, script, save_dir, level, vehicle, BUDGET,
                     verbose, arm)
    failures += read.fatal("re-entry read")
    entries = len([e for e in read.events("load")])
    if entries < 2:
        failures.append(
            f"re-entry: the level was entered {entries} time(s) in the reload run; the "
            "repeat read that originally exposed the control-point overflow did not "
            "happen")
    if read.ghost_frames < MIN_GHOST_FRAMES:
        failures.append(
            f"re-entry: only {read.ghost_frames} playback frames across {entries} entries")
    return failures


def subset_pairs(combos: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Every track at least once and every vehicle at least once, cheaply.

    Greedy: walk the matrix and keep one pair per track, preferring a vehicle
    that can actually produce a ghost on it -- picking a NO_GHOST_PAIRS row where
    the track has a working alternative would cost the subset a whole track's
    real coverage. That covers all 20 tracks; then add the cheapest extra rows
    needed so each of the three vehicles appears. Documented because a subset
    that silently stopped covering a track or a vehicle would be worse than no
    subset.
    """
    picked: list[tuple[int, int]] = []
    seen_tracks: set[int] = set()
    for level, vehicle in combos:
        if level in seen_tracks:
            continue
        if (level, vehicle) in NO_GHOST_PAIRS:
            alternative = next(
                (c for c in combos
                 if c[0] == level and c not in NO_GHOST_PAIRS), None)
            if alternative is not None:
                seen_tracks.add(level)
                picked.append(alternative)
                continue
        seen_tracks.add(level)
        picked.append((level, vehicle))
    for vehicle in sorted(VEHICLE_NAMES):
        if any(v == vehicle for _l, v in picked):
            continue
        extra = next((c for c in combos if c[1] == vehicle and c not in picked), None)
        if extra:
            picked.append(extra)
    return sorted(picked)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--pairs", default=None,
                        help="comma-separated level:vehicle to run instead of the matrix")
    parser.add_argument("--subset", action="store_true",
                        help="one row per track plus whatever it takes to cover every vehicle")
    parser.add_argument("--matrix", action="store_true",
                        help="print the pairs this would run and exit 0")
    parser.add_argument("--re-entry-pair", default="5:0",
                        help="the pair that additionally runs the repeat-read case")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument(
        "--jobs", type=int, default=4,
        help="pairs to run concurrently (1 = sequential). Each pair already "
             "owns its save directory, its input scripts and its child "
             "environment, so pairs do not share state; --jobs 1 is kept for "
             "reproducing a pooled result sequentially.")
    args = parser.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom):
        if not os.path.exists(path):
            print(f"FAIL: {path} not found -- build first")
            return 1

    rom_matrix = rom_vehicle_matrix(args.rom)
    combos = [(level, vehicle) for level in MAIN_TRACK_IDS
              for vehicle in sorted(VEHICLE_NAMES)
              if (rom_matrix[level][1] >> vehicle) & 1]

    if args.pairs:
        wanted = {tuple(int(x) for x in token.split(":"))
                  for token in args.pairs.split(",") if token.strip()}
        illegal = wanted - set(combos)
        if illegal:
            print(f"FAIL: not legal (track, vehicle) pairs: {sorted(illegal)}")
            return 1
        combos = [c for c in combos if c in wanted]
    elif args.subset:
        combos = subset_pairs(combos)

    if args.matrix:
        for level, vehicle in combos:
            note = NO_GHOST_PAIRS.get((level, vehicle), "")
            print("  level %-3d %-11s %s" % (level, VEHICLE_NAMES[vehicle], note))
        print(f"{len(combos)} pair(s)")
        return 0

    re_entry = tuple(int(x) for x in args.re_entry_pair.split(":"))
    print(f"check_ghost_matrix: {len(combos)} (track, vehicle) pair(s); each records a "
          f"ghost, saves it, and reloads it in a fresh process")

    failures: list[str] = []
    workdir = tempfile.mkdtemp(prefix="mdkr64-ghost-matrix-")
    started = time.time()
    try:
        # Pairs are independent: each owns its save directory (save_env per
        # child), its generated input scripts (named by tag inside workdir) and
        # its own child environment, and none of them writes anything shared.
        # Running them concurrently therefore changes wall-clock time and
        # nothing else -- results are collected and REPORTED IN MATRIX ORDER
        # below, so the output is byte-identical to a sequential run and a
        # reviewer can diff the two. `--jobs 1` reproduces that sequentially.
        jobs = max(1, args.jobs)
        if jobs == 1:
            outcomes = [
                check_pair(binary, args.rom, level, vehicle, workdir,
                           args.verbose)
                for level, vehicle in combos
            ]
        else:
            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
                outcomes = list(pool.map(
                    lambda combo: check_pair(binary, args.rom, combo[0],
                                             combo[1], workdir, args.verbose),
                    combos))

        for (level, vehicle), (problems, detail, seconds) in zip(combos,
                                                                 outcomes):
            tag = "ok" if not problems else "FAIL"
            print("  level %-3d %-11s %-6s %5.1fs  %s"
                  % (level, VEHICLE_NAMES[vehicle], tag, seconds,
                     detail if not problems else problems[0]))
            if problems:
                for problem in problems:
                    failures.append(f"[{level}:{VEHICLE_NAMES[vehicle]}] {problem}")
                # A failed child is evidence, not verbosity.
                for line in problems[1:]:
                    print(f"      | {line}")

        print(f"\ncheck_ghost_matrix: repeat-read case on "
              f"{re_entry[0]}:{VEHICLE_NAMES[re_entry[1]]} "
              f"(write -> reload -> read -> re-enter -> read again)")
        problems = re_entry_case(binary, args.rom, re_entry[0], re_entry[1],
                                 workdir, args.verbose)
        print("  %s" % ("ok" if not problems else "FAIL"))
        for problem in problems:
            print(f"      | {problem}")
            failures.append(f"[re-entry {re_entry[0]}:{re_entry[1]}] {problem}")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    total = time.time() - started
    if failures:
        print()
        for failure in failures:
            print(f"FAIL: {failure}")
        print(f"\ncheck_ghost_matrix: FAIL ({len(failures)} problem(s), {total / 60:.1f} min)")
        return 1
    covered = len([c for c in combos if c not in NO_GHOST_PAIRS])
    print(f"\ncheck_ghost_matrix: PASS -- {covered} pair(s) round-tripped a ghost through "
          f"a fresh process, {len(combos) - covered} documented non-producer(s), "
          f"{total / 60:.1f} min")
    return 0


if __name__ == "__main__":
    sys.exit(main())
