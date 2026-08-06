#!/usr/bin/env python3
"""Save-file fail-safe check: a corrupt save must never strand the player.

Why this exists
---------------
The published browser build keeps `save/eeprom.bin` in IndexedDB, so whatever is
in it comes back on every reload.  That makes the save file the one piece of
*persistent untrusted input* in the whole port: if a bad image can send the boot
somewhere the player cannot get out of, a visitor is stuck for good — the page
had no control that clears saved progress, and "clear site data" is not something
a visitor thinks to do.  A report of exactly that ("every time I boot I get
dropped into a race" -- later corrected by the owner to "after navigating all
screens it boots into a genie test which we already completed") is what prompted
this file.

Three real defects came out of the investigation, and this check covers all of
them.  The corrected symptom is defect 3.

**1. The image was accepted at any length, and interpreted whatever it held.**
`eeprom_load()` (platform/stubs_dkr.c) `fread` the file and ignored the count, so
a short file was interpreted with its missing tail read as zeros, and the next
`eeprom_store()` promoted that half-image to a full, canonical 512 bytes — the
original bytes gone, silently.  A short file is not hypothetical: the old store
did `fopen(path, "wb")`, which truncates *before* anything is written, so a crash
mid-write left 0..511 bytes on disk.  The store is now atomic (write `.tmp`,
`rename()` over the real path) and the load validates: exactly 512 bytes, and
every block either erased (all 0xFF), blank (all 0x00), or checksum-valid, per
save_layout.h.  Anything else is copied to `save/eeprom.bin.bad` and replaced with
the state the game itself defines for "empty".  Nothing is destroyed silently and
the rejection is loud.

**2. A checksum-VALID slot with an impossible field could SIGSEGV, every boot.**
The slot checksum is a plain 16-bit byte sum, which corrupt bytes satisfy by
chance: 4 of 40 randomly-corrupted-but-checksum-valid images crashed the engine.
All four narrowed to one field.  `settings->wizpigAmulet` is 3 bits on disk (so
0..7) but the amulet has FOUR pieces — the game's own tests are `>= 4` and
vehicle_tricky.c uses `wizpigAmulet - 1` as a 0..3 index — and objects.c's
BHV_DYNAMIC_LIGHT_OBJECT_2 spawn does `assetCount = wizpigAmulet + 1;
modelIndex = wizpigAmulet` with no clamp against the object's numberOfModelIds.
Values 5 and 7 therefore load out-of-range model ids on Timber's Island and then
SIGSEGV inside the racer collision path (`func_80054FD0`, from
`update_player_racer`) about 750 frames after the hub appears.  In the browser
that is a permanent boot loop: crash, reload, same save from IndexedDB, crash.
`populate_settings_from_save_data()` now rejects such a slot under NATIVE_PORT
using the function's own idiom for "this is not a save" (leave `newGame` TRUE),
so the game erases the slot and FILE SELECT offers an unstarted file.

**3. A save whose two halves of Taj progress disagree wedges Timber's Island.**
Reported from the published build as "after navigating all the menus it puts me
into a genie test we already completed".  `tajFlags` holds car/hover/plane
challenge OFFERED (0x01/0x02/0x04) and the same three BEATEN (0x08/0x10/0x20),
written at two different moments by two different functions.  The offer gate
(`objects.c:1794`) consults only the OFFERED half, and the auto-offer dialogue
chain never asks — `DIALOGUEPAGE_TAJ_CHALLENGE_CAR` falls through to
`DIALOGUEPAGE_TAJ_4`, which returns `(gNextTajChallengeMenu - 1) | 0x40` and
starts the race.  So "beaten but never offered" replays a finished challenge on
every entry to the hub, and Taj's dialogue calls `disable_racer_input()` every
frame it is open: measured 76% of the run stationary, 14439 units driven against
37768 healthy.  `populate_settings_from_save_data()` now ORs OFFERED in from
BEATEN, which is sound in that direction only (a challenge cannot be beaten
without having been offered) and is a no-op on every consistent save.

What this asserts
-----------------
  1. **A torn save boots to the TITLE SCREEN.**  menuId 0 must be reached, and the
     only levels loaded before it may be the two the healthy boot loads
     (OPTIONSBACKGROUND behind the boot logo, FRONTEND behind the logos).  A race
     level_load before the title fails.
  2. **A full-length garbage save does the same.**
  3. **Both were really consumed and really rejected**, not merely absent:
     `save/eeprom.bin.bad` must exist afterwards and be byte-identical to the
     bytes fed in, and `save/eeprom.bin` must be a clean 512-byte image.  This is
     what stops case 1/2 passing vacuously — an absent save produces no `.bad`
     file, which case 4 pins down explicitly.
  4. **The poison save runs the Adventure resume route without crashing**, and
     *reaches Timber's Island* while doing it.  The reach assertion is coverage,
     not decoration: without it, a future fixture change that stopped entering
     the hub would leave every other assertion passing while exercising none of
     the code that crashed.
  5. **The control (no save at all) boots clean and produces no quarantine.**
  6. **A save whose Taj progress contradicts itself does not wedge the hub.**
     `tajFlags` records "car challenge beaten" without "car challenge offered";
     the kart must still drive Timber's Island normally. Measured by path length
     and stalled-row fraction, because the damage is that Taj comes over and holds
     the racer's input down, not a crash.

Proven in both directions
-------------------------
With the fixes reverted, measured on us.v80 (both controls were run, not reasoned
about):
  * revert eeprom_load()'s validation in platform/stubs_dkr.c -> 3 failures:
        torn: save/eeprom.bin.bad was not created …
        torn: save/eeprom.bin is 100 bytes after the run, expected 512
        garbage: save/eeprom.bin.bad was not created …
  * revert the amulet rejection in game/src/save_data.c -> 3 failures:
        poison: run produced [CRASH]/[FATAL] …
        poison: run exited -11 (negative = killed by signal 11) …
        poison: slot 0 was not erased (first bytes 002c00000000) …
  * revert the BEATEN-implies-OFFERED repair in game/src/save_data.c -> 2 failures:
        taj: the kart only drove 14439.4 units (need >= 25000.0) …
        taj: the kart was stationary for 76% of the run (limit 45%) …

Usage:
    tests/check_save_failsafe.py [--build build] [--rom baserom.us.v80.z64] [-v]

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.  Exit 0 = pass; exit 1 = at least one assertion failed (each
printed with the measured value).

The save directory is wiped before AND after every case.  That is load-bearing,
not tidiness: a leftover save sends FILE SELECT down the resume path and would
break check_adventure_hub.py, which asserts the new-game cutscene.
"""

from __future__ import annotations

import argparse
import importlib.util
import hashlib
import math
import os
import random
import re
import subprocess
import sys

from harness_utils import (config_block as save_config_block, EEPROM_ARTIFACTS,
                           preserved_eeprom, resolve_binary, save_env,
                           seal_slot, SLOT_BYTES, test_save_dir)

SAVE_DIR = test_save_dir()
EEPROM = os.path.join(SAVE_DIR, "eeprom.bin")
EEPROM_BAD = os.path.join(SAVE_DIR, "eeprom.bin.bad")
EEPROM_TMP = os.path.join(SAVE_DIR, "eeprom.bin.tmp")
EEPROM_AUTOSAVES = tuple(
    os.path.join(SAVE_DIR, f"eeprom.bin.autosave.{index}")
    for index in range(1, 4)
)
EEPROM_SIZE = 512

ADVENTURE_SCRIPT = "tests/input_scripts/adventure_hub_drive.txt"

# Measured on us.v80: MENU_BOOT(26) @14, MENU_LOGOS(1) @173, MENU_TITLE(0) @1134.
BOOT_FRAMES = 1400
MENU_TITLE = 0
TITLE_FRAME_MAX = 1400
# The only levels a healthy boot loads up to and including the title, and what
# they are for. Anything else appearing in that window is the reported symptom.
BOOT_LEVELS = {39: "OPTIONSBACKGROUND (behind MENU_BOOT)",
               21: "FRONTEND (behind MENU_LOGOS)",
               23: "TITLESCREENSEQUENCE (the title's own backdrop, same frame)"}

# The poison case must outlive the crash point. Measured pre-fix: the poison slot
# reads as a started file, FILE SELECT resumes it, the hub loads at ~2086,
# reloads at ~2844 (the Wizpig-amulet cutscene bounce an out-of-range amulet
# triggers) and the SIGSEGV lands shortly after. 5000 leaves margin.
POISON_FRAMES = 5000
MENU_CHARACTER_SELECT = 3
MENU_FILE_SELECT = 6

# --- case 5: the Taj-challenge trap ----------------------------------------- #
# tajFlags packs car/hover/plane challenge OFFERED (0x01/0x02/0x04) and the same
# three BEATEN (0x08/0x10/0x20). They are two separate writes minutes apart, each
# with its own save flush; if only the later lands, the save reads "beaten but
# never offered". The offer gate (objects.c:1794) tests ONLY the OFFERED bits, so
# Taj teleports over on every entry to Timber's Island and the auto-offer chain
# starts the race without asking — a challenge you have already beaten, for ever.
#
# It is measured by DRIVING, because the visible damage is that Taj's dialogue
# calls disable_racer_input() every frame it is open and cannot be advanced from
# the pad (docs/OPEN_ITEMS.md "P3.5 Part C"): the kart simply stops.
# Measured on us.v80 over 12000 frames, 9909 post-hub [PACE] rows:
#     repair in place  : path 37767.6, 2406 stalled rows (24%)  -- and BYTE-EQUAL
#                        to tajFlags 0x00 and 0x09, i.e. no offer at all
#     repair reverted  : path 14439.4, 7525 stalled rows (76%)  -- wedged
TAJ_FRAMES = 12000
TAJ_BEATEN_NOT_OFFERED = 0x08   # car challenge beaten, car challenge never offered
TAJ_BALLOONS = 5                # == ASSET_MISC_16[0], the car-challenge threshold
TAJ_MIN_PATH = 25000.0          # healthy 31699.8 on the closed-loop hub tour
                                # (37767.6 on the old open-loop route) vs wedged 14439.4
TAJ_MAX_STALLED_FRACTION = 0.45  # healthy 0.10 closed-loop (0.24 open-loop) vs wedged 0.76
TAJ_MIN_ROWS = 4000             # measured 9909; a route that never drives fails

PACE_RE = re.compile(r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+)")

EVENT_RE = re.compile(
    r"\[TRACE\] (?:menu_init: menuId=(?P<menu>\d+)|level_load: levelId=(?P<level>\d+))"
    r".*@frame~(?P<frame>\d+)")


# --------------------------------------------------------------------------- #
#  Synthetic EEPROM images.  Layout from game/include/save_layout.h:
#    3 x SaveFile(40) | SaveConfig(8) | CourseRecords(192) x 2  == 512
#  Slot bit stream MSB-first from byte 0, exactly as func_80072C54 reads it in
#  populate_settings_from_save_data() (game/src/save_data.c).  No ROM bytes are
#  involved: every image here is generated from these field widths.
# --------------------------------------------------------------------------- #
CONFIG_OFF, CONFIG_BYTES = 120, 8
FASTLAPS_OFF, TIMES_OFF, RECORD_BYTES = 128, 320, 192


def _slot_bits(wizpig_amulet: int, taj_flags: int = 0,
               balloons_total: int = 0) -> bytes:
    """A slot that decodes as a started file, with the fields under test set."""
    bits: list[int] = []

    def put(n: int, v: int) -> None:
        for i in range(n - 1, -1, -1):
            bits.append((v >> i) & 1)

    put(16, 0)          # checksum, filled in below
    put(68, 0)          # per-course status (2 bits each) + pad
    put(6, taj_flags)   # tajFlags
    put(10, 0)          # trophies
    put(12, 0)          # bosses
    put(7, balloons_total)      # balloons[0] == the TOTAL balloon count
    for _ in range(5):
        put(7, 0)       # balloons per world
    put(3, 0)           # ttAmulet
    put(3, wizpig_amulet)
    for _ in range(6):
        put(16, 0)      # per-world door flags
    put(8, 0)           # keys
    put(32, 0)          # cutsceneFlags
    put(16, 0)          # filename
    put(8, 0)           # trailing pad
    assert len(bits) == SLOT_BYTES * 8, len(bits)
    out = bytearray()
    for i in range(0, len(bits), 8):
        b = 0
        for j in range(8):
            b = (b << 1) | bits[i + j]
        out.append(b)
    return bytes(seal_slot(out))


def _sum_block(n: int) -> bytes:
    """A checksum-valid, otherwise-empty CourseRecords block."""
    return bytes(seal_slot(bytearray(n)))


def _config_block() -> bytes:
    """A checksum-valid SaveConfig word."""
    return save_config_block(1 << 25)  # the subtitles bit; any payload will do


def named_slot(taj_flags: int, balloons_total: int) -> bytes:
    """A started file with a name, so FILE SELECT resumes it instead of asking for
    initials. filename 0 decompresses to an empty name but still counts as
    started; give it a real one so the route matches ordinary play."""
    body = bytearray(_slot_bits(0, taj_flags, balloons_total))
    # filename occupies the 16 bits at bit offset 16+68+6+10+12+42+3+3+96+8+32.
    off = 16 + 68 + 6 + 10 + 12 + 42 + 3 + 3 + 96 + 8 + 32
    for i, bit in enumerate(f"{0x1234:016b}"):
        byte, shift = (off + i) // 8, 7 - ((off + i) % 8)
        if bit == "1":
            body[byte] |= 1 << shift
        else:
            body[byte] &= ~(1 << shift) & 0xFF
    return bytes(seal_slot(body))


def image(slots: list[bytes]) -> bytes:
    out = bytearray(EEPROM_SIZE)
    for i, s in enumerate(slots):
        out[i * SLOT_BYTES:(i + 1) * SLOT_BYTES] = s
    out[CONFIG_OFF:CONFIG_OFF + CONFIG_BYTES] = _config_block()
    out[FASTLAPS_OFF:FASTLAPS_OFF + RECORD_BYTES] = _sum_block(RECORD_BYTES)
    out[TIMES_OFF:TIMES_OFF + RECORD_BYTES] = _sum_block(RECORD_BYTES)
    return bytes(out)


def erased_image() -> bytes:
    """What the game itself leaves behind: three erased (0xFF) slots."""
    return image([b"\xff" * SLOT_BYTES] * 3)


# --------------------------------------------------------------------------- #
#  Harness
# --------------------------------------------------------------------------- #
def wipe_save() -> None:
    # harness_utils.EEPROM_ARTIFACTS is the single list of everything
    # eeprom_store()/eeprom_load() can leave behind; a local copy here would
    # silently stop clearing whatever the platform layer learns to write next.
    for name in EEPROM_ARTIFACTS:
        p = os.path.join(SAVE_DIR, name)
        if os.path.exists(p):
            os.remove(p)


def hub_motion(out: str, hub_frame: int) -> tuple[float, int, int]:
    """(path length, stalled rows, rows) from the [PACE] racer rows after the hub
    load. Same idea as check_adventure_hub.py's path metric."""
    pts = [(int(m.group(1)), float(m.group(2)), float(m.group(4)))
           for m in PACE_RE.finditer(out)]
    pts = [q for q in pts if q[0] > hub_frame + 5]
    path, stalled = 0.0, 0
    for a, b in zip(pts, pts[1:]):
        step = math.hypot(b[1] - a[1], b[2] - a[2])
        path += step
        if step < 0.01:
            stalled += 1
    return path, stalled, len(pts)


def hub_tour_route() -> str:
    """The closed-loop Timber's Island tour, imported from check_adventure_hub.py.

    ONE definition of that route in the tree, deliberately. Case 5 below measures
    "did the kart drive the hub or was it wedged by a Taj dialogue", and until the
    "closedloop" wave it did so with the input script's blind RIGHT-pulse train --
    which stopped touring the island at all once the ROM-faithful RNG seed /
    arctan table / trig became the defaults (it drove into a hub exit and left for
    the Dino Domain lobby). A wedge and a route that wandered off look identical
    in path length, so the drive has to be closed-loop for this case to mean
    anything.
    """
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "check_adventure_hub.py")
    spec = importlib.util.spec_from_file_location("mdkr_check_adventure_hub", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.HUB_TOUR_ROUTE


def run(binary: str, rom: str, frames: int, script: str | None,
        verbose: bool) -> tuple[int, list[tuple[str, int, int]], str]:
    """Returns (returncode, [(kind, id, frame)], combined output)."""
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"      # belt-and-braces; --headless-frames already
    env["MDKR_TRACE"] = "1"      # emit menu_init / level_load
    save_env(env, SAVE_DIR)
    # This is a save oracle, so a developer's CWD mdkr64.ini must not change
    # its authored-tick frame budget (for example by selecting Uncapped).
    # /dev/null or NUL is an explicit, read-only empty config on each host.
    env["MDKR_VIDEO_CONFIG_PATH"] = os.devnull
    cmd = [binary, "--headless-frames", str(frames)]
    if script:
        cmd += ["--input-script", script]
        # The hub cases drive; give them the closed loop. The MDKR_DRIVE_ROUTE
        # hook is a no-op on every level the route does not name, and the Taj
        # dialogue still wedges the kart through it exactly as it wedges a human:
        # the hook writes gCurrent*, and update_player_racer zeroes those whenever
        # gRacerInputBlocked is set. So this makes the healthy arm reproducible
        # WITHOUT making the wedged arm recoverable, which is the property case 5
        # depends on.
        env["MDKR_DRIVE_ROUTE"] = hub_tour_route()
        env["MDKR_AUTOPILOT"] = "1"
    cmd += ["--rom", rom]
    if verbose:
        print("  $ " + " ".join(cmd))
    # A save-derived frontend invariant once exposed an unbounded random retry
    # loop. A gate for fail-safe boot must itself fail in bounded time rather
    # than wedging the whole release runner.
    timeout = max(60.0, frames / 20.0)
    try:
        proc = subprocess.run(
            cmd, env=env, capture_output=True, text=True, timeout=timeout
        )
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        partial = stdout + stderr
        return 124, [], partial + (
            f"\n[TIMEOUT] save fail-safe route exceeded {timeout:.1f}s\n")
    out = proc.stdout + proc.stderr
    events: list[tuple[str, int, int]] = []
    for m in EVENT_RE.finditer(out):
        if m.group("menu") is not None:
            events.append(("menu", int(m.group("menu")), int(m.group("frame"))))
        else:
            events.append(("level", int(m.group("level")), int(m.group("frame"))))
    return proc.returncode, events, out


def md5(path: str) -> str | None:
    if not os.path.exists(path):
        return None
    with open(path, "rb") as fh:
        return hashlib.md5(fh.read()).hexdigest()


def check_clean_boot(label: str, rc: int, events: list[tuple[str, int, int]],
                     out: str, failures: list[str], verbose: bool) -> None:
    """Shared assertions: no crash, the title is reached, and nothing that looks
    like a race was loaded on the way there."""
    if "[CRASH]" in out or "[FATAL]" in out:
        failures.append(f"{label}: run produced [CRASH]/[FATAL]")
    # A -fstack-protector abort prints nothing at all and goes straight to
    # SIGABRT, so the return code is part of the assertion, not just the log.
    if rc != 0:
        failures.append(f"{label}: run exited {rc} "
                        f"(negative = killed by signal {-rc})")
    title = next((f for kind, i, f in events if kind == "menu" and i == MENU_TITLE), None)
    if title is None:
        failures.append(f"{label}: MENU_TITLE (menuId {MENU_TITLE}) never reached — "
                        "boot did not get to the title screen")
    elif title > TITLE_FRAME_MAX:
        failures.append(f"{label}: MENU_TITLE only at frame {title} "
                        f"(expected by {TITLE_FRAME_MAX})")
    stray = [(i, f) for kind, i, f in events
             if kind == "level" and i not in BOOT_LEVELS
             and (title is None or f <= title)]
    if stray:
        failures.append(f"{label}: level(s) {stray} loaded before the title screen — "
                        "a corrupt save must not launch anything")
    if verbose:
        print(f"  {label}: rc={rc} title@{title} "
              f"levels={[ (i, f) for k, i, f in events if k == 'level' ][:4]}")


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
    for path in (binary, args.rom, ADVENTURE_SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: {path} not found — build first / check the ROM path")
            return 1

    os.makedirs(SAVE_DIR, exist_ok=True)
    failures: list[str] = []

    # ---- case 1: torn write -------------------------------------------------
    # 100 bytes of a valid image: what the old truncate-then-write store left on
    # disk when it was interrupted.
    print("case 1: torn save (100 of 512 bytes)")
    torn = image([_slot_bits(0)] * 3)[:100]
    wipe_save()
    with open(EEPROM, "wb") as fh:
        fh.write(torn)
    rc, events, out = run(binary, args.rom, BOOT_FRAMES, None, args.verbose)
    check_clean_boot("torn", rc, events, out, failures, args.verbose)
    bad = md5(EEPROM_BAD)
    if bad is None:
        failures.append(f"torn: {EEPROM_BAD} was not created — the corrupt image was "
                        "accepted instead of quarantined (this is also what keeps "
                        "this case from passing vacuously)")
    elif bad != hashlib.md5(torn).hexdigest():
        failures.append(f"torn: {EEPROM_BAD} does not hold the bytes that were fed in "
                        f"(md5 {bad})")
    elif args.verbose:
        print(f"  torn: quarantined {os.path.getsize(EEPROM_BAD)} bytes verbatim")
    if os.path.exists(EEPROM) and os.path.getsize(EEPROM) != EEPROM_SIZE:
        failures.append(f"torn: {EEPROM} is {os.path.getsize(EEPROM)} bytes after the "
                        f"run, expected {EEPROM_SIZE}")

    # ---- case 2: full-length garbage ---------------------------------------
    print("case 2: full-length garbage save (512 random bytes)")
    rng = random.Random(20260725)
    garbage = bytes(rng.randrange(256) for _ in range(EEPROM_SIZE))
    wipe_save()
    with open(EEPROM, "wb") as fh:
        fh.write(garbage)
    rc, events, out = run(binary, args.rom, BOOT_FRAMES, None, args.verbose)
    check_clean_boot("garbage", rc, events, out, failures, args.verbose)
    bad = md5(EEPROM_BAD)
    if bad is None:
        failures.append(f"garbage: {EEPROM_BAD} was not created — every block of a "
                        "random image fails its own checksum, so all of them should "
                        "have been rejected")
    elif bad != hashlib.md5(garbage).hexdigest():
        failures.append(f"garbage: {EEPROM_BAD} does not hold the bytes that were fed "
                        f"in (md5 {bad})")

    # ---- case 2b: block-granular automatic snapshot recovery --------------
    # Damage only slot 0 in the newest image. Recovery must take slot 0 from the
    # newest valid autosave while retaining the newer, still-valid slot 1 from
    # the live image. Restoring the whole older image would silently roll back
    # unrelated progress and therefore fails this case.
    print("case 2b: corrupt slot restored from autosave without rolling back "
          "other valid blocks")
    recovered_slot = named_slot(0x01, 5)
    current_slot = named_slot(0x03, 12)
    older_slot = named_slot(0x01, 2)
    broken_slot = bytearray(recovered_slot)
    broken_slot[7] ^= 0x40
    broken_current = image([
        bytes(broken_slot), current_slot, b"\xff" * SLOT_BYTES
    ])
    automatic = image([
        recovered_slot, older_slot, b"\xff" * SLOT_BYTES
    ])
    wipe_save()
    with open(EEPROM, "wb") as fh:
        fh.write(broken_current)
    with open(EEPROM_AUTOSAVES[0], "wb") as fh:
        fh.write(automatic)
    rc, events, out = run(
        binary, args.rom, BOOT_FRAMES, None, args.verbose
    )
    check_clean_boot("autosave", rc, events, out, failures, args.verbose)
    after = open(EEPROM, "rb").read() if os.path.exists(EEPROM) else b""
    if len(after) != EEPROM_SIZE:
        failures.append(
            f"autosave: restored EEPROM is {len(after)} bytes, expected 512"
        )
    else:
        if after[:SLOT_BYTES] != recovered_slot:
            failures.append(
                "autosave: corrupt slot 0 was not restored from snapshot 1"
            )
        if after[SLOT_BYTES:2 * SLOT_BYTES] != current_slot:
            failures.append(
                "autosave: valid newer slot 1 was rolled back with the whole "
                "older snapshot"
            )
    if md5(EEPROM_BAD) != hashlib.md5(broken_current).hexdigest():
        failures.append(
            "autosave: the damaged live generation was not quarantined verbatim"
        )

    # ---- case 3: checksum-valid, impossible field --------------------------
    # This is the strand: the image passes every checksum the game checks, so
    # nothing upstream rejects it, and pre-fix it took the engine down ~750 frames
    # into Timber's Island -- every boot, for ever, in the browser.
    #
    # Slot 0 is the poison one; slots 1 and 2 are legitimate started files with a
    # COMPLETE amulet (4), the largest value real play can produce. That mix is
    # what makes this case non-vacuous and keeps the fix honest: afterwards slot 0
    # must be erased and slots 1 and 2 must be byte-for-byte untouched. An absent
    # save cannot satisfy the second half, and a fix that rejected the whole image
    # instead of the one bad slot cannot satisfy it either.
    print("case 3: checksum-valid save with wizpigAmulet=7 in slot 0, "
          "valid slots 1-2, Adventure route")
    good_slot = _slot_bits(4)
    poison = image([_slot_bits(7), good_slot, good_slot])
    wipe_save()
    with open(EEPROM, "wb") as fh:
        fh.write(poison)
    rc, events, out = run(binary, args.rom, POISON_FRAMES, ADVENTURE_SCRIPT, args.verbose)
    if "[CRASH]" in out or "[FATAL]" in out:
        failures.append("poison: run produced [CRASH]/[FATAL] — an out-of-range "
                        "amulet still reaches the object spawner")
    if rc != 0:
        failures.append(f"poison: run exited {rc} "
                        f"(negative = killed by signal {-rc}) — the corrupt slot is "
                        "still being accepted as a real save")
    # charselect_assign_ai() completes either through the original retry loop or
    # through the bounded completion path, depending on where the ROM-faithful
    # generator's stream happens to be. Which one ran is not the contract; the
    # contract is that assignment terminates. Pinning the fallback's own trace
    # line makes an unrelated RNG-consumption change look like a regression, and
    # says nothing when the loop hangs before ever emitting it. Assert instead
    # that character select was entered and left.
    charselect_frames = [f for kind, i, f in events
                         if kind == "menu" and i == MENU_CHARACTER_SELECT]
    if not charselect_frames:
        failures.append(
            f"poison: character select (menuId {MENU_CHARACTER_SELECT}) was never "
            "entered, so charselect_assign_ai() never ran and the RNG "
            "termination control covered nothing"
        )
    else:
        left = [f for kind, i, f in events
                if kind != "menu" or i != MENU_CHARACTER_SELECT]
        if not any(f > charselect_frames[0] for f in left):
            failures.append(
                f"poison: nothing follows character select at frame "
                f"{charselect_frames[0]} — charselect_assign_ai() never returned"
            )
        elif args.verbose:
            taken = ("bounded fallback"
                     if "charselect_assign_ai: bounded RNG fallback" in out
                     else "original retry loop")
            print(f"  poison: character select @{charselect_frames[0]} "
                  f"completed via the {taken}")
    fsel = next((f for kind, i, f in events
                 if kind == "menu" and i == MENU_FILE_SELECT), None)
    if fsel is None:
        failures.append(f"poison: FILE SELECT (menuId {MENU_FILE_SELECT}) was never "
                        "reached, so the save file was never read — this case would be "
                        "passing without covering anything")
    after = open(EEPROM, "rb").read() if os.path.exists(EEPROM) else b""
    if len(after) != EEPROM_SIZE:
        failures.append(f"poison: {EEPROM} is {len(after)} bytes after the run, "
                        f"expected {EEPROM_SIZE}")
    else:
        slot0 = after[0:SLOT_BYTES]
        if slot0 != b"\xff" * SLOT_BYTES:
            failures.append("poison: slot 0 was not erased (first bytes "
                            f"{slot0[:6].hex()}) — the impossible amulet was accepted "
                            "as real progress")
        for idx in (1, 2):
            got = after[idx * SLOT_BYTES:(idx + 1) * SLOT_BYTES]
            if got != good_slot:
                failures.append(f"poison: valid slot {idx} was modified "
                                f"({got[:6].hex()} vs {good_slot[:6].hex()}) — the "
                                "fail-safe must reject the bad slot only")
    if args.verbose:
        print(f"  poison: rc={rc} FILE SELECT @{fsel} slot0 erased="
              f"{after[0:SLOT_BYTES] == b'\xff' * SLOT_BYTES}")

    # ---- case 5: a consistent-checksum save whose Taj progress disagrees ----
    print("case 5: tajFlags says the car challenge is BEATEN but was never OFFERED")
    taj = image([named_slot(TAJ_BEATEN_NOT_OFFERED, TAJ_BALLOONS),
                 b"\xff" * SLOT_BYTES, b"\xff" * SLOT_BYTES])
    wipe_save()
    with open(EEPROM, "wb") as fh:
        fh.write(taj)
    rc, events, out = run(binary, args.rom, TAJ_FRAMES, ADVENTURE_SCRIPT, args.verbose)
    if rc != 0 or "[CRASH]" in out or "[FATAL]" in out:
        failures.append(f"taj: run exited {rc} / produced [CRASH]")
    hub = next((f for kind, i, f in events if kind == "level" and i == 0), None)
    if hub is None:
        failures.append("taj: Timber's Island (levelId 0) never loaded — the route "
                        "did not resume the file, so this case covers nothing")
    else:
        path, stalled, rows = hub_motion(out, hub)
        frac = stalled / rows if rows else 1.0
        if args.verbose:
            print(f"  taj: hub@{hub} rows={rows} path={path:.1f} "
                  f"stalled={stalled} ({frac:.0%})")
        if rows < TAJ_MIN_ROWS:
            failures.append(f"taj: only {rows} post-hub [PACE] rows "
                            f"(need >= {TAJ_MIN_ROWS})")
        if path < TAJ_MIN_PATH:
            failures.append(f"taj: the kart only drove {path:.1f} units "
                            f"(need >= {TAJ_MIN_PATH}) — Taj re-offered a challenge "
                            "that the save records as already beaten, and his "
                            "dialogue holds the racer's input down")
        if frac > TAJ_MAX_STALLED_FRACTION:
            failures.append(f"taj: the kart was stationary for {frac:.0%} of the run "
                            f"(limit {TAJ_MAX_STALLED_FRACTION:.0%}) — wedged in "
                            "Taj's challenge dialogue")

    # ---- case 4: control, no save at all -----------------------------------
    # Pins down that the quarantine assertions above mean something: with nothing
    # on disk there is nothing to reject, so no .bad file may appear.
    print("case 4: control — no save file at all")
    wipe_save()
    rc, events, out = run(binary, args.rom, BOOT_FRAMES, None, args.verbose)
    check_clean_boot("absent", rc, events, out, failures, args.verbose)
    if os.path.exists(EEPROM_BAD):
        failures.append(f"absent: {EEPROM_BAD} exists after a run with no save file — "
                        "the quarantine is firing on nothing, which would make cases "
                        "1 and 2 pass vacuously")
    if os.path.exists(EEPROM_TMP):
        failures.append(f"absent: {EEPROM_TMP} left behind — the atomic store did not "
                        "rename or clean up its temporary file")

    wipe_save()

    if failures:
        print(f"\nFAIL ({len(failures)}):")
        for f in failures:
            print("  - " + f)
        return 1
    print("\nPASS: a torn, a garbage and a checksum-valid-but-impossible save all "
          "boot to the title screen; the corrupt bytes are quarantined, not adopted.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
