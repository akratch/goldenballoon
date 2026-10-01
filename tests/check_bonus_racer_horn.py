#!/usr/bin/env python3
"""Check which clip the horn button dispatches for each racer identity.

A player-one Tracks race on Ancient Lake (the ordinary nav_to_time_trial_race
route, Time Trial off) presses Z three times once the race is live. Every
press emits ``racer_horn: player=0 identity=<id> sound=<id>``; ``sound=-1``
means the horn cheat handed the press to the character-voice path. Arms run
as the retail racer, Wizpig, Terry and Taj, without and with the horn cheat
(seeded through the persisted magic-code state, the same file the Magic Codes
menu writes).

Expected clips are pinned to retail sound IDs independently of the
production table:

* retail: its own character's horn (Diddy, the default pick), or the voice
  path under the cheat;
* Wizpig: his shortest authored laugh, with or without the cheat;
* Terry: his donor kart's horn, with or without the cheat. No authored Terry
  clip exists (no voice bank; retail's two spare horn IDs carry no sound bite),
  and his voice path is deliberately mute, so the cheat must not silence him;
* Taj: his magic call, with or without the cheat.

This proves dispatch identity, not perceived sound. Audio output is muted.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, fatal_re, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
ROUTE = ROOT / "tests/input_scripts/nav_to_time_trial_race.txt"
# The race loads at frame ~2620 and its countdown ignores Z; these land live.
PRESSES = (2900, 3100, 3300)
FRAMES = 3400
HORN = re.compile(r"racer_horn: player=(-?\d+) identity=(\d+) sound=(-?\d+)")
RACE = "level_load: levelId=5 numPlayers=0"
MAGIC = "magic_codes_restore: unlocked=00000100 active=00000100"

SOUND_HORN_KRUNCH = 0x156
SOUND_HORN_DIDDY = 0x15F
SOUND_VOICE_WIZPIG_LAUGH_SHORT2 = 0xB3
SOUND_VOICE_TAJ_ALAKAZAM = 0x236
VOICE_PATH = -1

# (label, test-player variable or None, identity, horn cheat, expected clip)
ARMS = (
    ("retail", None, 0, False, SOUND_HORN_DIDDY),
    ("retail-cheat", None, 0, True, VOICE_PATH),
    ("wizpig", "MDKR_WIZPIG_TEST_PLAYER", 2, False, SOUND_VOICE_WIZPIG_LAUGH_SHORT2),
    ("wizpig-cheat", "MDKR_WIZPIG_TEST_PLAYER", 2, True, SOUND_VOICE_WIZPIG_LAUGH_SHORT2),
    ("terry", "MDKR_TERRY_TEST_PLAYER", 3, False, SOUND_HORN_KRUNCH),
    ("terry-cheat", "MDKR_TERRY_TEST_PLAYER", 3, True, SOUND_HORN_KRUNCH),
    ("taj-cheat", "MDKR_TAJ_TEST_PLAYER", 1, True, SOUND_VOICE_TAJ_ALAKAZAM),
)


def magic_codes_state(active: int) -> str:
    """Canonical magic-code sidecar (platform/magic_codes_state.c)."""
    prefix = ("magic_codes_version=1\n"
              f"unlocked={active:08X}\nactive={active:08X}\n")
    digest = 2166136261
    for byte in prefix.encode("ascii"):
        digest = ((digest ^ byte) * 16777619) & 0xFFFFFFFF
    return f"{prefix}checksum={digest:08X}"


def check_output(output: str, identity: int, cheat: bool, expected: int) -> int:
    fatal = fatal_re().search(output)
    if fatal:
        raise ValueError(f"fatal marker: {fatal.group(0)}")
    if RACE not in output:
        raise ValueError("the Ancient Lake race did not load")
    if cheat != (MAGIC in output):
        raise ValueError(f"horn cheat state is not {cheat}")
    horns = [tuple(map(int, row)) for row in HORN.findall(output)]
    mine = [(who, sound) for player, who, sound in horns if player == 0]
    if len(mine) != len(PRESSES):
        raise ValueError(f"expected {len(PRESSES)} player-one horns, got {mine}")
    for who, sound in mine:
        if who != identity:
            raise ValueError(f"player one honked as identity {who}, not {identity}")
        if sound != expected:
            raise ValueError(f"player one horn dispatched {sound}, expected {expected}")
    return len(mine)


def run_arm(binary: Path, rom: Path, root: Path, arm) -> int:
    label, variable, identity, cheat, expected = arm
    save = root / label / "save"
    save.mkdir(parents=True)
    if cheat:
        (save / "magic_codes_state.ini").write_text(magic_codes_state(1 << 8))
    script = root / label / "input.txt"
    script.write_text(ROUTE.read_text() + "".join(f"{frame} Z 4\n" for frame in PRESSES))
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MDKR", "GE007_"))}
    env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1", MDKR_RENDERER="gl",
               MDKR_SAVE_DIR=str(save), MDKR_VIDEO_CONFIG_PATH=str(save / "video.ini"))
    if variable:
        env[variable] = "0"
    result = subprocess.run(
        [str(binary), "--headless-frames", str(FRAMES), "--input-script", str(script),
         "--rom", str(rom)], cwd=root / label, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240, check=False)
    (root / label / "run.log").write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise ValueError(f"runner exit {result.returncode}")
    return check_output(result.stdout, identity, cheat, expected)


def self_test() -> None:
    """The oracle must reject a donor horn, a wrong identity and a lost cheat."""
    good = (RACE + "\n" + "".join(
        f"racer_horn: player=0 identity=2 sound={SOUND_VOICE_WIZPIG_LAUGH_SHORT2}\n"
        for _ in PRESSES))
    check_output(good, 2, False, SOUND_VOICE_WIZPIG_LAUGH_SHORT2)
    for output, identity, cheat in (
            (good.replace(f"sound={SOUND_VOICE_WIZPIG_LAUGH_SHORT2}",
                          f"sound={SOUND_HORN_KRUNCH}"), 2, False),
            (good.replace("identity=2", "identity=0"), 2, False),
            (good, 2, True),
            (good.replace("racer_horn", "racer_hum", 1), 2, False)):
        try:
            check_output(output, identity, cheat, SOUND_VOICE_WIZPIG_LAUGH_SHORT2)
        except ValueError:
            continue
        raise AssertionError("invalid horn evidence was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--arm", choices=("all",) + tuple(a[0] for a in ARMS), default="all")
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.resolve()
    if not binary.is_file() or not rom.is_file():
        parser.error("binary and supported ROM must exist")
    self_test()
    # Keep failures for investigation; evidence stays outside source by default.
    root = (args.evidence_dir.resolve() if args.evidence_dir else
            Path(tempfile.mkdtemp(prefix="mdkr-bonus-racer-horn-")))
    root.mkdir(parents=True, exist_ok=True)
    failed = 0
    for arm in ARMS:
        if args.arm not in ("all", arm[0]):
            continue
        try:
            count = run_arm(binary, rom, root, arm)
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            print(f"check_bonus_racer_horn: FAIL -- {arm[0]}: {error}", flush=True)
            failed += 1
            continue
        print(f"check_bonus_racer_horn: PASS -- {arm[0]}: {count} presses dispatched "
              f"{arm[4]}", flush=True)
    print(f"check_bonus_racer_horn: {'FAIL' if failed else 'PASS'} -- evidence: {root}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
