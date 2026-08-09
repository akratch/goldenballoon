#!/usr/bin/env python3
"""Controller hotplug gate: what happens to a DKR channel after boot.

platform_input_init() opens every joystick it can see at startup, and that
path runs on every launch.  What happens AFTER startup was unmeasured, and the
failure that actually loses races lives there: a pad that disconnects mid-race
while a direction is held, leaving a channel steering into a wall.

WHY VIRTUAL JOYSTICKS.  SDL_JoystickAttachVirtual() creates a device the game
controller layer enumerates, maps, opens, reads and removes exactly like
hardware -- same SDL_CONTROLLERDEVICEADDED/_REMOVED events, same instance ids,
same force-recentering on removal.  So the whole binding path is exercised with
no pad plugged in and no hardware-specific behaviour assumed.  The engine side
of the arm is MDKR_TEST_PAD_HOTPLUG (platform/platform_sdl_min.c), which is
inert unless set.

WHAT IS OBSERVED.  Not SDL: the ``[PAD-CHANNEL]`` rows are emitted from
platform_input_commit_tick() through platform_pad_present/buttons/stick, the
same three accessors osContGetReadData() reads in platform/stubs_dkr.c to fill
its MAXCONTROLLERS entries.  A trace that agreed with SDL while disagreeing
with the game would prove nothing.

THE SIX ASSERTIONS, each independently scored:

  1. join        a pad added mid-run is opened, and into the next free channel
  2. neutral     a pad removed mid-run leaves EXACT neutral, not its last value
  3. numbering   removing player 1's pad does not renumber players 2-4
  4. rebind      re-adding returns to the freed channel, not a new one
  5. overflow    a fifth pad is ignored, without an error and without a crash
  6. rumble      a motor request on a channel with no pad is a no-op

Assertion 2 is scored twice on purpose.  Ports 1-3 disconnect through the input
queue's presence fail-safe; PORT 0 CANNOT, because the keyboard is a complete
P1 controller and that port is present whether or not a pad is attached.  Its
neutralization has to come from the device-removed handler instead, so the two
are separate code paths and get separate arms.

LIMIT OF ASSERTION 4.  Channels are claimed lowest-free-first, so "the original
channel" is honoured whenever a single pad drops out.  Two pads dropping out at
once and reconnecting in the other order would swap them; recognising an
individual unit across a reconnect needs a stable identity SDL does not offer
(instance ids are per-connection, and GUIDs name the model, not the unit), so
that case is deliberately out of scope rather than silently covered.

Usage:
    tests/check_input_hotplug.py [--build build] [--rom baserom.us.v80.z64]
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass

from harness_utils import DEFAULT_BUILD_DIR, find_fatal, resolve_binary, ABORT_MARKERS


# The engine converts an SDL axis to the N64 range as (value * 80) / 32767, so
# the fixture's full deflection of 32000 lands here.  Stated as the arithmetic
# rather than as a magic 78 so a change to either constant is visible.
FULL_DEFLECTION = 32000
STICK_FULL = (FULL_DEFLECTION * 80) // 32767

# Pose -> the (stick_x, stick_y) a correctly routed channel must publish.
# N64 stick_y is positive up, which is why "down" is negative here.
POSE_STICK = {
    "left": (-STICK_FULL, 0),
    "right": (STICK_FULL, 0),
    "up": (0, STICK_FULL),
    "down": (0, -STICK_FULL),
}

# Ticks allowed between a scheduled op and the first published tick that must
# already show its effect.  The engine applies both within the same tick in
# practice; the margin exists so a scheduler retiming cannot turn a phase shift
# into a false defect report.
SETTLE = 4

CHANNEL_RE = re.compile(
    r"\[PAD-CHANNEL\] tick=(-?\d+) port=(\d+) present=(\d+) "
    r"buttons=0x([0-9a-fA-F]+) sx=(-?\d+) sy=(-?\d+) instance=(-?\d+) "
    r"attached=(\d+)")
HOTPLUG_RE = re.compile(
    r"\[PAD-HOTPLUG\] tick=(\d+) op=(\w+) id=(-?\d+) port=(-?\d+) "
    r"instance=(-?\d+) result=(-?\d+)")
OVERFLOW_NOTICE = "[SDL] gamepad ignored: all 4 controller channels are in use"


@dataclass(frozen=True)
class Channel:
    tick: int
    port: int
    present: int
    buttons: int
    sx: int
    sy: int
    instance: int
    attached: int

    def neutral(self) -> bool:
        return self.buttons == 0 and self.sx == 0 and self.sy == 0


@dataclass(frozen=True)
class Op:
    tick: int
    op: str
    id: int
    port: int
    instance: int
    result: int


class Run:
    """One engine run under the hotplug arm, parsed into rows."""

    def __init__(self, name: str, schedule: str, frames: int,
                 proc: "subprocess.CompletedProcess[str]") -> None:
        self.name = name
        self.schedule = schedule
        self.frames = frames
        self.returncode = proc.returncode
        self.stdout = proc.stdout
        self.output = proc.stdout + proc.stderr
        self.channels = [
            Channel(int(m[0]), int(m[1]), int(m[2]), int(m[3], 16), int(m[4]),
                    int(m[5]), int(m[6]), int(m[7]))
            for m in CHANNEL_RE.findall(proc.stderr)
        ]
        self.ops = [
            Op(int(m[0]), m[1], int(m[2]), int(m[3]), int(m[4]), int(m[5]))
            for m in HOTPLUG_RE.findall(proc.stderr)
        ]

    def op_at(self, op: str, handle: int) -> Op:
        for entry in self.ops:
            if entry.op == op and entry.id == handle:
                return entry
        raise LookupError(f"{self.name}: no [PAD-HOTPLUG] {op} row for id={handle}")

    def rumbles(self) -> list[Op]:
        return [entry for entry in self.ops if entry.op == "rumble"]

    def window(self, port: int, first: int, last: int) -> list[Channel]:
        return [row for row in self.channels
                if row.port == port and first <= row.tick <= last]

    def last_tick(self) -> int:
        return max((row.tick for row in self.channels), default=-1)


def run_engine(binary: str, rom: str, name: str, schedule: str, frames: int,
               verbose: bool) -> Run:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    with tempfile.TemporaryDirectory(prefix="mdkr_hotplug_") as scratch:
        env.update(
            # --headless-frames is the guarantee; the digit 0 is what silences
            # the audio device, and only the digit ("off" is not a value).
            MDKR_AUDIO="0",
            MDKR_TEST_PAD_HOTPLUG=schedule,
            # Never inherit a repo-local mdkr64.ini: a developer's chosen
            # presentation pace changes how many game ticks a frame budget
            # buys, which would move every scheduled op.
            MDKR_VIDEO_CONFIG_PATH=os.path.join(scratch, "mdkr64.ini"),
            MDKR_SAVE_DIR=os.path.join(scratch, "save"),
        )
        cmd = [binary, "--headless-frames", str(frames), "--rom", rom]
        if verbose:
            print(f"$ MDKR_AUDIO=0 MDKR_TEST_PAD_HOTPLUG={schedule} "
                  + " ".join(cmd))
        proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return Run(name, schedule, frames, proc)


# --------------------------------------------------------------------------- #
#  Shared row predicates
# --------------------------------------------------------------------------- #


def require_rows(run: Run, port: int, first: int, last: int,
                 label: str) -> tuple[list[Channel], list[str]]:
    rows = run.window(port, first, last)
    if not rows:
        return [], [f"{label}: no [PAD-CHANNEL] rows for port {port} in "
                    f"ticks {first}..{last} (run reached tick {run.last_tick()})"]
    return rows, []


def expect_bound(run: Run, port: int, instance: int, first: int, last: int,
                 label: str) -> list[str]:
    """``port`` is owned by ``instance``, present and attached, all window."""
    rows, problems = require_rows(run, port, first, last, label)
    for row in rows:
        if row.instance != instance or row.present != 1 or row.attached != 1:
            problems.append(
                f"{label}: tick {row.tick} port {port} is instance="
                f"{row.instance} present={row.present} attached={row.attached}, "
                f"expected instance={instance} present=1 attached=1")
            break
    return problems


def expect_stick(run: Run, port: int, pose: str, first: int, last: int,
                 label: str) -> list[str]:
    want = POSE_STICK[pose]
    rows, problems = require_rows(run, port, first, last, label)
    for row in rows:
        if (row.sx, row.sy) != want:
            problems.append(
                f"{label}: tick {row.tick} port {port} published "
                f"sx={row.sx} sy={row.sy}, expected {want} for a held "
                f"'{pose}'")
            break
    return problems


def expect_neutral(run: Run, port: int, first: int, last: int, label: str, *,
                   present: int | None = None,
                   unbound: bool = True) -> list[str]:
    """Exact neutral -- zero buttons and a centred stick -- across a window."""
    rows, problems = require_rows(run, port, first, last, label)
    for row in rows:
        if not row.neutral():
            problems.append(
                f"{label}: tick {row.tick} port {port} still published "
                f"buttons=0x{row.buttons:04x} sx={row.sx} sy={row.sy}; a "
                f"channel with no pad must be EXACT neutral")
            break
    for row in rows:
        if unbound and (row.instance != -1 or row.attached != 0):
            problems.append(
                f"{label}: tick {row.tick} port {port} still owns instance="
                f"{row.instance} attached={row.attached} after its pad was "
                f"removed")
            break
    if present is not None:
        for row in rows:
            if row.present != present:
                problems.append(
                    f"{label}: tick {row.tick} port {port} present="
                    f"{row.present}, expected {present}")
                break
    return problems


def survived(run: Run) -> list[str]:
    problems: list[str] = []
    if run.returncode != 0:
        problems.append(f"{run.name}: exit code {run.returncode}")
    marker = find_fatal(run.output, *ABORT_MARKERS)
    if marker is not None:
        line = next((l for l in run.output.splitlines() if marker in l), marker)
        problems.append(f"{run.name}: fatal marker {marker!r}: {line.strip()}")
    return problems


# --------------------------------------------------------------------------- #
#  Arm A -- a pad at boot, a second pad joining, leaving and coming back
# --------------------------------------------------------------------------- #

# Tick 0 fires from platform_input_init() BEFORE its startup enumeration, which
# is the ordering a pad plugged in before launch produces: SDL_Init has already
# queued the device-added event that the first input pump will deliver.
ARM_JOIN = ("attach=1@0,hold=1:right@40,"
            "attach=2@120,hold=2:left@160,detach=2@220,"
            "rumble=1:1@260,rumble=3:1@262,"
            "attach=3@300,hold=3:up@340")
ARM_JOIN_FRAMES = 420

ARM_OVERFLOW = ("attach=1@60,attach=2@62,attach=3@64,attach=4@66,attach=5@68,"
                "hold=1:left@110,hold=3:right@112,hold=4:down@114,"
                "detach=1@200,rumble=0:1@240,rumble=0:0@242")
ARM_OVERFLOW_FRAMES = 320


def assert_join(run: Run) -> dict[str, list[str]]:
    """Assertions 1, 2, 4 and 6, from the join/leave/rejoin arm."""
    boot = run.op_at("attach", 1)
    join = run.op_at("attach", 2)
    leave = run.op_at("detach", 2)
    rejoin = run.op_at("attach", 3)
    hold_join = run.op_at("hold", 2)
    hold_rejoin = run.op_at("hold", 3)

    scored: dict[str, list[str]] = {}

    # 1. The boot pad owns exactly ONE channel, and the pad that joins later
    #    takes the next one.  Both halves matter: a boot pad bound twice makes
    #    DKR believe a controller is plugged into P2, and pushes the first real
    #    joiner to P3.
    problems: list[str] = []
    owners = {row.port for row in run.channels if row.instance == boot.instance}
    if owners != {0}:
        problems.append(
            f"join: the boot pad (instance {boot.instance}) occupied channels "
            f"{sorted(owners)}, expected exactly [0]")
    problems += expect_bound(run, 0, boot.instance, hold_join.tick + SETTLE,
                             leave.tick - 1, "join")
    problems += expect_bound(run, 1, join.instance, hold_join.tick + SETTLE,
                             leave.tick - 1, "join")
    problems += expect_stick(run, 1, "left", hold_join.tick + SETTLE,
                             leave.tick - 1, "join")
    # The joiner's input must reach ITS channel and not leak into the other.
    problems += expect_stick(run, 0, "right", hold_join.tick + SETTLE,
                             leave.tick - 1, "join")
    for port in (2, 3):
        problems += expect_neutral(run, port, hold_join.tick + SETTLE,
                                   leave.tick - 1, "join", present=0)
    scored["1 join"] = problems

    # 2. Unplugged while holding left.  Everything about that channel goes to
    #    exact neutral and STAYS there until a pad comes back -- while the
    #    other channel keeps steering, so a global input wipe cannot pass.
    problems = expect_neutral(run, 1, leave.tick + SETTLE, rejoin.tick - 1,
                              "neutral", present=0)
    problems += expect_stick(run, 0, "right", leave.tick + SETTLE,
                             rejoin.tick - 1, "neutral")
    scored["2 neutral (ports 1-3)"] = problems

    # 4. The freed channel is the one reused, rather than the pad landing on a
    #    new channel beside a slot nobody can reach.
    problems = expect_bound(run, 1, rejoin.instance, rejoin.tick + SETTLE,
                            run.last_tick(), "rebind")
    problems += expect_stick(run, 1, "up", hold_rejoin.tick + SETTLE,
                             run.last_tick(), "rebind")
    for port in (2, 3):
        problems += expect_neutral(run, port, rejoin.tick + SETTLE,
                                   run.last_tick(), "rebind", present=0)
    scored["4 rebind"] = problems

    # 6a. A motor request aimed at a channel whose pad has just gone, and at
    #     one that never had a pad.  Neither may fault, and neither may leave
    #     anything behind on the channel.
    problems = []
    for entry in run.rumbles():
        if entry.result != 0:
            problems.append(
                f"rumble: platform_pad_rumble(port={entry.port}) on a channel "
                f"with no pad returned {entry.result}, expected the 0 no-op")
    if len(run.rumbles()) != 2:
        problems.append(
            f"rumble: expected 2 [PAD-HOTPLUG] rumble rows, got "
            f"{len(run.rumbles())}")
    settled = max(entry.tick for entry in run.rumbles()) + SETTLE
    problems += expect_neutral(run, 1, settled, rejoin.tick - 1, "rumble",
                               present=0)
    problems += expect_neutral(run, 3, settled, rejoin.tick - 1, "rumble",
                               present=0)
    problems += survived(run)
    scored["6 rumble (join arm)"] = problems
    return scored


def assert_overflow(run: Run) -> dict[str, list[str]]:
    """Assertions 3, 5 and the port-0 half of 2, from the four-pad arm."""
    pads = [run.op_at("attach", handle) for handle in (1, 2, 3, 4, 5)]
    leave = run.op_at("detach", 1)
    hold_last = run.op_at("hold", 4)
    before = (hold_last.tick + SETTLE, leave.tick - 1)
    after = (leave.tick + SETTLE, run.last_tick())

    scored: dict[str, list[str]] = {}

    # 3. Player 1 unplugs.  Players 2-4 keep their channels, their pads and
    #    their held controls; nothing slides down to fill the gap.
    problems: list[str] = []
    for port, pad in enumerate(pads[:4]):
        problems += expect_bound(run, port, pad.instance, *before, "numbering")
    problems += expect_stick(run, 2, "right", *before, "numbering")
    problems += expect_stick(run, 3, "down", *before, "numbering")
    for port in (1, 2, 3):
        problems += expect_bound(run, port, pads[port].instance, *after,
                                 "numbering")
    problems += expect_stick(run, 2, "right", *after, "numbering")
    problems += expect_stick(run, 3, "down", *after, "numbering")
    scored["3 numbering"] = problems

    # 2b. Port 0 again, because it is the one port the input queue's presence
    #     fail-safe can never neutralize: the keyboard keeps P1 present, so the
    #     device-removed handler is solely responsible.  Presence is asserted
    #     to STAY 1 for the same reason -- a keyboard player must not lose
    #     their controller because a pad was unplugged.
    problems = expect_neutral(run, 0, *after, "neutral", present=1)
    scored["2 neutral (port 0)"] = problems

    # 5. Five pads, four channels.  The fifth is ignored: never bound, and said
    #    out loud once rather than silently doing nothing.
    problems = []
    fifth = [row for row in run.channels if row.instance == pads[4].instance]
    if fifth:
        problems.append(
            f"overflow: the fifth pad (instance {pads[4].instance}) reached "
            f"channel {fifth[0].port} at tick {fifth[0].tick}")
    notices = run.stdout.count(OVERFLOW_NOTICE)
    if notices != 1:
        problems.append(
            f"overflow: the run logged the ignored-pad notice {notices} times, "
            f"expected exactly 1")
    problems += survived(run)
    scored["5 overflow"] = problems

    # 6b. The motor request lands on channel 0 after its pad is gone.
    problems = []
    for entry in run.rumbles():
        if entry.result != 0:
            problems.append(
                f"rumble: platform_pad_rumble(port={entry.port}) on a channel "
                f"with no pad returned {entry.result}, expected the 0 no-op")
    if len(run.rumbles()) != 2:
        problems.append(
            f"rumble: expected 2 [PAD-HOTPLUG] rumble rows, got "
            f"{len(run.rumbles())}")
    settled = max(entry.tick for entry in run.rumbles()) + SETTLE
    problems += expect_neutral(run, 0, settled, run.last_tick(), "rumble",
                               present=1)
    problems += survived(run)
    scored["6 rumble (overflow arm)"] = problems
    return scored


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    scored: dict[str, list[str]] = {}
    for name, schedule, frames, judge in (
            ("join", ARM_JOIN, ARM_JOIN_FRAMES, assert_join),
            ("overflow", ARM_OVERFLOW, ARM_OVERFLOW_FRAMES, assert_overflow)):
        run = run_engine(binary, args.rom, name, schedule, frames,
                         args.verbose)
        if not run.channels:
            # No trace at all means the arm never armed (or the binary died
            # before the first tick).  Report that once, rather than as six
            # identical "no rows" assertion failures.
            print(f"FAIL: {name} arm produced no [PAD-CHANNEL] rows; "
                  f"exit={run.returncode}", file=sys.stderr)
            for line in run.output.splitlines()[-40:]:
                print(f"    {line}", file=sys.stderr)
            return 1
        unsupported = [op for op in run.ops if op.result == -2]
        if unsupported:
            print("FAIL: this SDL has no virtual joystick support, so the "
                  "hotplug arm cannot drive a device add or remove; the gate "
                  "asserts nothing here rather than passing vacuously",
                  file=sys.stderr)
            return 1
        try:
            scored.update(judge(run))
        except LookupError as error:
            print(f"FAIL: {error}", file=sys.stderr)
            return 1

    failed = 0
    for name in sorted(scored):
        problems = scored[name]
        print(f"{'PASS' if not problems else 'FAIL'}  assertion {name}")
        for problem in problems:
            print(f"        {problem}")
        failed += 1 if problems else 0

    if failed:
        print(f"FAIL: {failed} of {len(scored)} hotplug assertions failed",
              file=sys.stderr)
        return 1
    print(f"controller hotplug gate passed ({len(scored)} assertions)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
