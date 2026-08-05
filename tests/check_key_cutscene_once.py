#!/usr/bin/env python3
"""A one-shot cutscene must play ONCE: the world-key "challenge door unlocked"
sequence, and the latch that is supposed to suppress it.

Why this exists
---------------
Reported from the published browser build: *"I collected the Key on the first
race, and now the key animation is playing after EVERY race."*

`level_load()` (game/src/game.c) is where a world hub decides to redirect into the
"your key just unlocked the Challenge door" cutscene.  The gate is

    if (settings->keys & (1 << world) &&
        !(settings->cutsceneFlags & (CUTSCENE_DINO_DOMAIN_KEY << (world + 31))))

and the body ORs the same expression into `cutsceneFlags` as the "already shown"
latch.  `world` is 1..4 here, so that shift count is **32..35**.  The ROM means it:
MIPS `sllv` takes the count from the low five bits of the register, so `<< 32` is
`<< 0` and the four worlds land on 0x4000/0x8000/0x10000/0x20000 exactly as
`structs.h` documents them.  In C a shift count >= the operand width is
**undefined behaviour**, and this is the shape an optimiser can see straight
through: the enclosing `world > WORLD_CENTRAL_AREA && world < WORLD_FUTURE_FUN_LAND`
pins the count to a range that is *entirely* >= 32, so clang concludes the value is
poison and folds it to zero.

A mask of zero breaks the latch in both directions at once: `!(flags & 0)` is
always true, so the gate always fires, and `flags |= 0` is a no-op, so nothing is
ever recorded — not in RAM, not in the save.  The cutscene therefore replays on
every single entry to that world's hub, which is to say after every race in it,
for ever.

The port's **native build defaults to Debug and the web build to Release**
(CMakeLists.txt:6-14).  At `-O0` clang emits a real register shift and arm64 masks
the count modulo 32 just like MIPS, so the same source is correct — which is why
this was invisible to every native check and visible immediately to the owner in
the browser.  Measured on this route, Release vs Debug, same save and same ROM:

    -O0   keycutscene: world=1 keys=0x2 cutsceneFlags=0x0     mask=0x4000  -> fires
          keycutscene: world=1 keys=0x2 cutsceneFlags=0x44000 mask=0x4000  -> latched, silent
    -O2   keycutscene: world=1 keys=0x2 cutsceneFlags=0x0     mask=0x0     -> fires
          keycutscene: world=1 keys=0x2 cutsceneFlags=0x40000 mask=0x0     -> FIRES AGAIN

and the user-visible difference is one extra load of the world hub on the way back
from the race, which is the cutscene playing:

    fixed  : level 12 @10709 -> level 0 @11057          (one hub load)
    broken : level 12 @10709 -> level 12 @11435 -> 0 @11783   (cutscene, then hub)

The fix is `DKR_SHL32()` (game/include/macros.h), which makes the MIPS masking
explicit under `NATIVE_PORT` and leaves the expression untouched for a matching
ROM build.  Three more instances of the same idiom were found and fixed with it —
see docs/OPEN_ITEMS.md wave "keyshift".

What this asserts
-----------------
  1. **The route really ran.**  levelId 0 (Timber's Island) -> 12 (Dino Domain
     lobby) -> 5 (Ancient Lake) -> 12 -> 0, and real in-race motion on Ancient
     Lake.  This is coverage, not decoration: every assertion below is about what
     happens on the *second* entry to level 12, so a fixture change that stopped
     entering the hub twice would leave them all passing while testing nothing.
  2. **The world hub's key gate is evaluated exactly twice** — once on the way in,
     once returning from the race.  That is what makes assertion 4 meaningful.
  3. **The latch mask is non-zero.**  This is the root cause stated directly: a
     mask of 0x0 in the trace *is* the bug, whatever else happens to be true.
  4. **The cutscene is not replayed.**  The first evaluation sees the latch clear
     (the key was just collected, so the cutscene is due) and the second must see
     it SET.  Additionally the return leg must contain exactly ONE load of
     levelId 12 — the count that distinguishes "returned to the lobby" from
     "returned to the lobby via the cutscene again".
  5. **The latch reaches the save.**  Slot 0 of `save/eeprom.bin` must have bit
     0x4000 in `cutsceneFlags` afterwards.  In the browser the save lives in
     IndexedDB, so a latch that is only ever set in RAM comes back cleared on the
     next reload and the player sees the cutscene again for a second reason.
  6. **The process exits 0** with no `[CRASH]`/`[FATAL]`.

Proven in both directions
-------------------------
Run against a build configured with `-DMDKR_EXTRA_C_FLAGS=-DMDKR_SHL32_CONTROL`,
which restores the plain C shift and nothing else, at `-DCMAKE_BUILD_TYPE=Release`:

    key: the key-cutscene latch mask is 0x0, so the latch can never be set or
         tested (game.c's `CUTSCENE_DINO_DOMAIN_KEY << (world + 31)` folded to 0)
    key: the cutscene fired again on the return to the world hub -- cutsceneFlags
         0x40000 still lacks 0x4000 after it had already played once
    key: 2 loads of levelId 12 on the return leg, expected 1 (the extra load is
         the replayed cutscene)
    key: save slot 0 cutsceneFlags 0x40000 lacks bit 0x4000 after the run

The same build at `-DCMAKE_BUILD_TYPE=Debug` PASSES with the control define set,
which is the point: the defect is a property of the optimiser, so a Debug-only
check cannot see it.  Pass `--expect-fail` to assert that the run fails, which is
how the control is exercised without hand-reading the output.

Usage:
    tests/check_key_cutscene_once.py [--build build-rel] [--rom baserom.us.v80.z64]
                                     [--expect-fail] [-v]

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.  Exit 0 = pass; exit 1 = at least one assertion failed (each
printed with the measured value).

The save directory is wiped before AND after the run.  That is load-bearing, not
tidiness: this check *injects* a save, and a leftover one sends FILE SELECT down
the resume path in check_adventure_hub.py / check_adventure_race_loop.py, both of
which assert the new-game route.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

from harness_utils import preserved_eeprom, resolve_binary, save_env, test_save_dir

SCRIPT = "tests/input_scripts/adventure_race_loop.txt"
FRAMES = 17000
SAVE_DIR = test_save_dir()
EEPROM = os.path.join(SAVE_DIR, "eeprom.bin")
EEPROM_BAD = os.path.join(SAVE_DIR, "eeprom.bin.bad")
EEPROM_TMP = os.path.join(SAVE_DIR, "eeprom.bin.tmp")

HUB = 0        # ASSET_LEVEL_CENTRALAREAHUB   (Timber's Island)
LOBBY = 12     # ASSET_LEVEL_DINODOMAINHUB    (the Dino Domain world hub)
RACE = 5       # ASSET_LEVEL_ANCIENTLAKE

# The world under test and its latch bit.  world 1 == WORLD_DINO_DOMAIN
# (enums.h), and CUTSCENE_DINO_DOMAIN_KEY << (1 - 1) == 0x4000 (structs.h).
WORLD = 1
KEY_BIT = 1 << WORLD          # settings->keys bit for Dino Domain
LATCH = 0x4000                # cutsceneFlags bit for "the world 1 key cutscene has played"

# The same closed-loop steering check_adventure_race_loop.py uses, so the two
# checks cross the same ground and a route regression shows up in both.  The
# waypoints are sampled from adventure_hub_drive.txt's own [PACE] trace; the
# balloon and the exits are named objects.  See platform/mdkr_adventure.c.
DRIVE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
    ":-3381,1946:-2519,1516:-1858,1099"
    ";12:E5:E0"
)

# --- thresholds (measured on us.v80, this route, fixed Release build) -------- #
# The route is deterministic (tests/check_determinism.py), so these are stable.
MIN_RACE_ROWS = 3000          # measured ~6500 in-race racer rows over 3 laps
MIN_RACE_PATH = 20000.0       # measured 64153 world units
RETURN_LOBBY_LOADS = 1        # fixed 1 (@10709); broken 2 (@10709 and @11435)

PACE_RE = re.compile(r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+)")
LEVEL_RE = re.compile(r"level_load: levelId=(\d+) numPlayers=(-?\d+) entrance=(-?\d+)"
                      r" vehicle=(-?\d+) cutscene=(-?\d+) @frame~(\d+)")
KEY_RE = re.compile(r"keycutscene: world=(\d+) keys=0x([0-9a-f]+)"
                    r" cutsceneFlags=0x([0-9a-f]+) mask=0x([0-9a-f]+)")

# --------------------------------------------------------------------------- #
#  Synthetic EEPROM.  Layout from game/include/save_layout.h, and the bit order
#  is exactly the order populate_settings_from_save_data() reads it in with
#  func_80072C54() (game/src/save_data.c:398-413).  No ROM bytes are involved:
#  every byte here is generated from those field widths.  Deliberately
#  self-contained rather than shared with check_save_failsafe.py's builder, so
#  the two checks cannot break each other.
# --------------------------------------------------------------------------- #
SLOT_BYTES = 40
EEPROM_SIZE = 512
CONFIG_OFF, CONFIG_BYTES = 120, 8
FASTLAPS_OFF, TIMES_OFF, RECORD_BYTES = 128, 320, 192
# bit offset of cutsceneFlags inside a slot: checksum + courses + taj + trophies
# + bosses + 6 balloon counts + 2 amulets + 6 door-flag words + keys
CUTSCENE_BIT_OFF = 16 + 68 + 6 + 10 + 12 + (6 * 7) + 3 + 3 + (6 * 16) + 8


def slot(keys: int, balloons: tuple, filename: int = 0x1234) -> bytes:
    """A started, named save slot with `keys` and `balloons` set and everything
    else zero.  Named so FILE SELECT resumes it instead of asking for initials,
    which is what makes the route match ordinary play."""
    bits: list[int] = []

    def put(n: int, v: int) -> None:
        for i in range(n - 1, -1, -1):
            bits.append((v >> i) & 1)

    put(16, 0)              # checksum, filled in below
    put(68, 0)              # per-course status, 2 bits each + pad
    put(6, 0)               # tajFlags
    put(10, 0)              # trophies
    put(12, 0)              # bosses
    for b in balloons:      # balloonsPtr[0] is the TOTAL, [1..5] per world
        put(7, b)
    put(3, 0)               # ttAmulet
    put(3, 0)               # wizpigAmulet
    for _ in range(6):
        put(16, 0)          # per-world door flags
    put(8, keys)
    put(32, 0)              # cutsceneFlags -- nothing has been shown yet
    put(16, filename)
    put(8, 0)               # trailing pad
    assert len(bits) == SLOT_BYTES * 8, len(bits)
    out = bytearray()
    for i in range(0, len(bits), 8):
        b = 0
        for j in range(8):
            b = (b << 1) | bits[i + j]
        out.append(b)
    s = (5 + sum(out[2:])) & 0xFFFF     # populate_settings_from_save_data()'s sum
    out[0], out[1] = (s >> 8) & 0xFF, s & 0xFF
    return bytes(out)


def sum_block(n: int) -> bytes:
    d = bytearray(n)
    s = (5 + sum(d[2:])) & 0xFFFF
    d[0], d[1] = (s >> 8) & 0xFF, s & 0xFF
    return bytes(d)


def config_block() -> bytes:
    """A checksum-valid SaveConfig word (read_eeprom_settings sums nibbles 0..13)."""
    v = 1 << 25                          # the subtitles bit; any payload will do
    ck = 5 + sum((v >> (i * 4)) & 0xF for i in range(14))
    v |= (ck & 0xFF) << 56
    return v.to_bytes(8, "big")


def image(slot0: bytes) -> bytes:
    out = bytearray(EEPROM_SIZE)
    out[0:SLOT_BYTES] = slot0
    for i in (1, 2):                     # the other two slots stay erased
        out[i * SLOT_BYTES:(i + 1) * SLOT_BYTES] = b"\xff" * SLOT_BYTES
    out[CONFIG_OFF:CONFIG_OFF + CONFIG_BYTES] = config_block()
    out[FASTLAPS_OFF:FASTLAPS_OFF + RECORD_BYTES] = sum_block(RECORD_BYTES)
    out[TIMES_OFF:TIMES_OFF + RECORD_BYTES] = sum_block(RECORD_BYTES)
    return bytes(out)


def read_cutscene_flags(path: str) -> int | None:
    """cutsceneFlags out of slot 0 of an on-disk eeprom image."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return None
    if len(data) < SLOT_BYTES:
        return None
    bits = "".join(format(b, "08b") for b in data[:SLOT_BYTES])
    return int(bits[CUTSCENE_BIT_OFF:CUTSCENE_BIT_OFF + 32], 2)


def wipe_save() -> None:
    for p in (EEPROM, EEPROM_BAD, EEPROM_TMP):
        if os.path.exists(p):
            os.remove(p)


def is_finite(v: float) -> bool:
    return v == v and -1e30 < v < 1e30


def main() -> int:
    # This check deletes and rewrites EEPROM images to assert a known starting
    # state. Under tools/run_checks.py SAVE_DIR is a run-scoped temporary
    # directory; a standalone run defaults to the repository's playable save/,
    # whose prior contents must survive however this exits.
    with preserved_eeprom(SAVE_DIR):
        return run_check()


def run_check() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build-rel",
                    help="build dir; MUST be an optimised build -- see the module "
                         "docstring, a Debug build cannot observe this defect")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--expect-fail", action="store_true",
                    help="invert the exit code, for running the reverted control")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not os.path.exists(path):
            print("FAIL: missing %s" % path, file=sys.stderr)
            return 1

    fails: list[str] = []

    def bad(msg: str) -> None:
        fails.append(msg)
        print("  FAIL key: %s" % msg)

    # The key is already collected; nothing has been shown yet.  One total balloon
    # so the Dino Domain doors open, and ZERO Dino Domain balloons so the
    # boss-approach cutscene block (game.c, same idiom, own latch) stays out of it.
    wipe_save()
    os.makedirs(SAVE_DIR, exist_ok=True)
    with open(EEPROM, "wb") as fh:
        fh.write(image(slot(keys=KEY_BIT, balloons=(1, 0, 0, 0, 0, 0))))

    env = save_env(dict(os.environ,
                        MDKR_AUDIO="0",     # belt and braces; --headless-frames is the guarantee
                        MDKR_TRACE="1",
                        MDKR_AUTOPILOT="1",  # DKR's own AI drives the race
                        MDKR_DRIVE_ROUTE=DRIVE_ROUTE),
                   SAVE_DIR)
    cmd = [binary, "--headless-frames", str(FRAMES),
           "--input-script", SCRIPT, "--rom", args.rom]
    if args.verbose:
        print("$ MDKR_DRIVE_ROUTE='%s' MDKR_AUTOPILOT=1 %s" % (DRIVE_ROUTE, " ".join(cmd)))

    proc = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=3600)
    out = proc.stdout + proc.stderr
    try:
        wipe_save_flags = read_cutscene_flags(EEPROM)
    finally:
        pass

    # --- 6. the process survived ------------------------------------------- #
    if proc.returncode != 0:
        bad("run exited %d (negative = killed by signal)" % proc.returncode)
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in out:
            bad("run produced %s" % marker)

    # --- 1. the route really ran ------------------------------------------- #
    loads = [(int(m.group(1)), int(m.group(3)), int(m.group(6)))
             for m in LEVEL_RE.finditer(out)]           # (levelId, entrance, frame)
    seq = [lv for lv, _, _ in loads]
    for want, name in ((HUB, "Timber's Island"), (LOBBY, "the Dino Domain lobby"),
                       (RACE, "Ancient Lake")):
        if want not in seq:
            bad("levelId %d (%s) was never loaded -- the route did not run, so "
                "nothing below was actually exercised" % (want, name))
    race_at = next((f for lv, _, f in loads if lv == RACE), None)

    rows = [(int(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)))
            for m in PACE_RE.finditer(out)]
    if race_at is None:
        bad("Ancient Lake never loaded, so the race leg is unmeasurable")
        race_rows = []
    else:
        # Rows between the race load and the return to the lobby.
        back_at = next((f for lv, _, f in loads if lv == LOBBY and f > race_at), FRAMES)
        race_rows = [r for r in rows if race_at + 5 < r[0] < back_at]
    if len(race_rows) < MIN_RACE_ROWS:
        bad("only %d in-race racer rows on Ancient Lake (need >= %d) -- the race "
            "did not really run" % (len(race_rows), MIN_RACE_ROWS))
    path = 0.0
    for a, b in zip(race_rows, race_rows[1:]):
        if b[0] != a[0] + 1 or not all(is_finite(v) for v in a[1:] + b[1:]):
            continue
        path += ((b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2 + (b[3] - a[3]) ** 2) ** 0.5
    if path < MIN_RACE_PATH:
        bad("the racer only covered %.1f units on Ancient Lake (need >= %.1f)"
            % (path, MIN_RACE_PATH))

    # --- 2. the key gate was evaluated on both entries to the world hub ----- #
    evals = [(int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16))
             for m in KEY_RE.finditer(out)]
    evals = [e for e in evals if e[0] == WORLD]
    if len(evals) != 2:
        bad("the world-%d key gate was evaluated %d times, expected 2 (once "
            "entering the hub, once returning from the race). Without both "
            "evaluations the replay assertion below is vacuous."
            % (WORLD, len(evals)))

    # --- 3. the latch mask is non-zero: the root cause, stated directly ----- #
    for i, (_, _, _, mask) in enumerate(evals):
        if mask != LATCH:
            bad("the key-cutscene latch mask is 0x%x at evaluation %d, expected "
                "0x%x -- `CUTSCENE_DINO_DOMAIN_KEY << (world + 31)` folded away, "
                "so the latch can never be set or tested" % (mask, i + 1, LATCH))
            break

    # --- 4. the cutscene played once ---------------------------------------- #
    if len(evals) >= 1:
        _, keys, flags, _ = evals[0]
        if not keys & KEY_BIT:
            bad("the injected save did not arrive: keys=0x%x lacks bit 0x%x, so "
                "the gate was never eligible to fire" % (keys, KEY_BIT))
        if flags & LATCH:
            bad("the latch was already set (cutsceneFlags=0x%x) on the FIRST entry "
                "to the world hub, so this run never exercised the cutscene at all"
                % flags)
    if len(evals) >= 2:
        _, _, flags, _ = evals[1]
        if not flags & LATCH:
            bad("the cutscene fired again on the return to the world hub -- "
                "cutsceneFlags 0x%x still lacks 0x%x after it had already played "
                "once" % (flags, LATCH))
    if race_at is not None:
        after = [f for lv, _, f in loads if lv == LOBBY and f > race_at]
        if len(after) != RETURN_LOBBY_LOADS:
            bad("%d loads of levelId %d on the return leg, expected %d (the extra "
                "load is the replayed cutscene) -- frames %s"
                % (len(after), LOBBY, RETURN_LOBBY_LOADS, after))

    # --- 5. the latch reached the save -------------------------------------- #
    if wipe_save_flags is None:
        bad("save/eeprom.bin was unreadable after the run, so the latch cannot be "
            "shown to survive a reload")
    elif not wipe_save_flags & LATCH:
        bad("save slot 0 cutsceneFlags 0x%x lacks bit 0x%x after the run -- the "
            "latch never reached the save, so a browser reload replays the "
            "cutscene even if RAM had it right" % (wipe_save_flags, LATCH))

    wipe_save()

    if args.verbose:
        for e in evals:
            print("  keycutscene world=%d keys=0x%x cutsceneFlags=0x%x mask=0x%x" % e)
        print("  level loads: %s" % [(lv, f) for lv, _, f in loads])
        print("  race rows=%d path=%.1f saved cutsceneFlags=%s"
              % (len(race_rows), path,
                 "None" if wipe_save_flags is None else "0x%x" % wipe_save_flags))

    if args.expect_fail:
        if fails:
            print("check_key_cutscene_once: PASS (control) -- %d assertion(s) failed "
                  "as expected with the fix reverted" % len(fails))
            return 0
        print("check_key_cutscene_once: FAIL (control) -- the reverted build passed, "
              "so this check does not actually measure the defect")
        return 1
    if fails:
        print("check_key_cutscene_once: FAIL -- %d assertion(s)" % len(fails))
        return 1
    print("check_key_cutscene_once: PASS -- the world-%d key cutscene played once, "
          "latched 0x%x in RAM and in the save, and did not replay after the race"
          % (WORLD, LATCH))
    return 0


if __name__ == "__main__":
    sys.exit(main())
