#!/usr/bin/env python3
"""Check playable Wizpig's real race voice dispatch; not an audio-quality oracle.

Hot Top Volcano's plane route exercises both zipper celebrations and negative
reactions. Rendering and synthesis run normally, with device output muted.
The expected IDs are pinned to the retail Wizpig voice bank as literals
(they prove routing to his bank, not which clip suits which reaction).

Two field arms add the racers the time trial does not have: a two-player
Ancient Lake race with four CPU racers. With player one as Wizpig, every
other racer (the second human and the CPU racers) must keep its retail voice:
retail identity, never a Wizpig or Taj clip, and only player one may resolve
to Wizpig. The same race with no test player is the retail control: no
bonus-identity dispatch at all. Both arms require reactions from the other
human, and the two together at least MIN_CPU_REACTIONS from unfinished CPU
racers, so neither check can pass for want of voices. The CPU floor is pooled
because a CPU reaction is rare on this route (2 in the retail arm, 4 with
Wizpig, measured on build-rel and build-asan alike): per arm, one benign shift
in race timing would read as lost coverage.

Evidence (run.log and the save of every arm) goes to a private temporary
directory that is removed on PASS and kept, and named, on FAIL; --evidence-dir
keeps it in a directory of your choosing either way.
"""
from __future__ import annotations

import argparse
from collections import Counter
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tests/input_scripts/race_full_3lap_tt.txt"
FIELD_SCRIPT = ROOT / "tests/input_scripts/wizpig_voice_2p_field.txt"
VOICES = {
    0x162: {0xB3, 0xB4, 0xB5, 0xB6, 0x35, 0x48, 0x51, 0x52, 0x57},
    0x1C2: {0x6D, 0x6E, 0x72, 0x75, 0x6C},
}
# Taj's race reactions: WAHEY, HOHO, WOAH, ALAKAZAM, ALAKAZOOM2.
TAJ_VOICES = {0x25, 0x26, 0x28, 0x236, 0x237}
BONUS_VOICES = set().union(*VOICES.values()) | TAJ_VOICES
WIZPIG_PLAYER = 0
WIZPIG = 2
VOICE = re.compile(
    r"mod_racer_voice: player=0 identity=2 event=(\d+) sound=(\d+)")
MOD_VOICE = re.compile(
    r"mod_racer_voice: player=(-?\d+) identity=(\d+) event=\d+ sound=\d+")
RACER_VOICE = re.compile(
    r"racer_voice: racer=(-?\d+) player=(-?\d+) finished=([01]) identity=(\d+) "
    r"event=(\d+) sound=(\d+)")
FATAL = ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:")
# Reactions from unfinished CPU racers, both field arms together.
MIN_CPU_REACTIONS = 2


def check_fatal(output: str) -> None:
    for marker in FATAL:
        if marker in output:
            raise ValueError(f"fatal marker: {marker}")


def check_output(output: str) -> int:
    check_fatal(output)
    if "level_load: levelId=7 numPlayers=0 entrance=0 vehicle=2" not in output:
        raise ValueError("the intended player plane race did not load")
    seen: set[int] = set()
    previous = None
    events = VOICE.findall(output)
    for event_text, sound_text in events:
        event, sound = int(event_text), int(sound_text)
        if sound not in VOICES.get(event, set()):
            raise ValueError(f"wrong voice for event {event}: {sound}")
        if sound == previous:
            raise ValueError(f"consecutive repeated voice: {sound}")
        previous = sound
        seen.add(event)
    if seen != set(VOICES):
        raise ValueError(f"missing positive/negative coverage: {sorted(seen)}")
    check_field(output, WIZPIG_PLAYER)
    return len(events)


def check_field(output: str, wizpig_player: int | None) -> Counter:
    """Only the Wizpig player's racer may dispatch a bonus clip.

    ``wizpig_player`` None is the retail control. Returns dispatch counts per
    source: 'wizpig', 'human' (another port) and 'cpu' (an unfinished CPU
    racer)."""
    for player, identity in MOD_VOICE.findall(output):
        if int(player) != wizpig_player or int(identity) != WIZPIG:
            raise ValueError(f"bonus voice table used by player {player} "
                             f"identity {identity}")
    counts: Counter = Counter()
    for row in RACER_VOICE.findall(output):
        racer, player, finished, identity, event, sound = map(int, row)
        wizpig_racer = wizpig_player is not None and (
            player == wizpig_player or
            (player == -1 and finished and racer == wizpig_player))
        if identity != 0:
            if not wizpig_racer or identity != WIZPIG:
                raise ValueError(f"racer {racer} player {player} finished={finished} "
                                 f"resolved to identity {identity}")
            if sound not in VOICES.get(event, set()):
                raise ValueError(f"Wizpig dispatched {sound} for event {event}")
            counts["wizpig"] += 1
            continue
        if sound in BONUS_VOICES:
            raise ValueError(f"retail racer {racer} player {player} was handed "
                             f"bonus clip {sound}")
        if wizpig_racer and player == wizpig_player:
            raise ValueError(f"Wizpig player dispatched retail voice {sound}")
        if player >= 0:
            counts["human"] += 1
        elif not finished:
            counts["cpu"] += 1
    return counts


def check_field_arm(output: str, wizpig_player: int | None) -> Counter:
    check_fatal(output)
    if "level_load: levelId=5 numPlayers=1 " not in output:
        raise ValueError("the two-player Ancient Lake race did not load")
    counts = check_field(output, wizpig_player)
    expected = {"human"} | ({"wizpig"} if wizpig_player is not None else set())
    missing = expected - {key for key, value in counts.items() if value}
    if missing:
        raise ValueError(f"no reactions observed from {sorted(missing)}: {dict(counts)}")
    return counts


def check_cpu_coverage(*arms: Counter) -> int:
    """The CPU racers' retail voice was exercised, across the field arms."""
    total = sum(arm["cpu"] for arm in arms)
    if total < MIN_CPU_REACTIONS:
        raise ValueError(f"only {total} CPU racer reaction(s) across the field "
                         f"arms (need {MIN_CPU_REACTIONS}): "
                         f"{[dict(arm) for arm in arms]}")
    return total


def self_test() -> None:
    """The field oracle must reject a bonus identity or clip on another racer."""
    good = ("level_load: levelId=5 numPlayers=1 \n"
            "racer_voice: racer=0 player=0 finished=0 identity=2 event=354 sound=179\n"
            "mod_racer_voice: player=0 identity=2 event=354 sound=179\n"
            "racer_voice: racer=1 player=1 finished=0 identity=0 event=450 sound=486\n"
            "racer_voice: racer=2 player=-1 finished=0 identity=0 event=354 sound=402\n")
    retail = ("level_load: levelId=5 numPlayers=1 \n"
              "racer_voice: racer=0 player=0 finished=0 identity=0 event=354 sound=402\n"
              "racer_voice: racer=1 player=1 finished=0 identity=0 event=450 sound=486\n"
              "racer_voice: racer=2 player=-1 finished=0 identity=0 event=354 sound=402\n")
    check_cpu_coverage(check_field_arm(good, WIZPIG_PLAYER),
                       check_field_arm(retail, None))
    # The pooled floor still rejects arms whose CPU racers never reacted.
    no_cpu = good.replace("player=-1 finished=0", "player=-1 finished=1")
    try:
        check_cpu_coverage(check_field_arm(no_cpu, WIZPIG_PLAYER),
                           check_field_arm(retail.replace("finished=0 identity=0 event=354 sound=402\n",
                                                          "finished=1 identity=0 event=354 sound=402\n"),
                                           None))
    except ValueError:
        pass
    else:
        raise AssertionError("field runs with no CPU reactions were accepted")
    for bad, player in (
            (good + "racer_voice: racer=3 player=-1 finished=0 identity=2 event=354 sound=180\n",
             WIZPIG_PLAYER),
            (good + "mod_racer_voice: player=-1 identity=2 event=354 sound=180\n",
             WIZPIG_PLAYER),
            (good + "racer_voice: racer=1 player=1 finished=0 identity=0 event=354 sound=181\n",
             WIZPIG_PLAYER),
            (good + "racer_voice: racer=0 player=0 finished=0 identity=0 event=354 sound=354\n",
             WIZPIG_PLAYER),
            (good, None)):
        try:
            check_field_arm(bad, player)
        except ValueError:
            continue
        raise AssertionError("invalid field voice evidence was accepted")


def run(binary: Path, rom: Path, root: Path, env: dict, script: Path, frames: int) -> str:
    root.mkdir(parents=True, exist_ok=True)
    env = dict(env, MDKR_SAVE_DIR=str(root / "save"),
               MDKR_VIDEO_CONFIG_PATH=str(root / "video.ini"))
    result = subprocess.run(
        [str(binary), "--headless-frames", str(frames), "--input-script",
         str(script), "--rom", str(rom)], cwd=root, env=env,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=240, check=False)
    (root / "run.log").write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise ValueError(f"runner exit {result.returncode}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.resolve()
    if not binary.is_file() or not rom.is_file():
        parser.error("binary and supported ROM must exist")
    self_test()
    # Private run logs and saves: kept for investigation on FAIL, removed on
    # PASS unless the caller named a directory to keep them in.
    keep = args.evidence_dir is not None
    root = (args.evidence_dir.resolve() if keep else
            Path(tempfile.mkdtemp(prefix="mdkr-wizpig-race-voice-")))
    root.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_AUTOPILOT="1",
               MDKR_TRACE="1", MDKR_RENDERER="gl")
    try:
        count = check_output(run(
            binary, rom, root, dict(env, MDKR_LOAD_TRACK="7:2",
                                    MDKR_WIZPIG_TEST_PLAYER=str(WIZPIG_PLAYER)),
            SCRIPT, 8000))
        field = check_field_arm(run(
            binary, rom, root / "field", dict(env, MDKR_WIZPIG_TEST_PLAYER=str(WIZPIG_PLAYER)),
            FIELD_SCRIPT, 7600), WIZPIG_PLAYER)
        retail = check_field_arm(run(
            binary, rom, root / "retail", env, FIELD_SCRIPT, 7600), None)
        cpu = check_cpu_coverage(field, retail)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"check_wizpig_race_voice: FAIL -- {error}; evidence kept: {root}")
        return 1
    if not keep:
        shutil.rmtree(root, ignore_errors=True)
    print(f"check_wizpig_race_voice: PASS -- {count} Wizpig-owned positive/negative "
          f"race voice dispatches, no consecutive repeats; two-player field "
          f"{dict(field)} and retail control {dict(retail)} kept bonus clips to "
          f"Wizpig alone ({cpu} CPU reactions across both)"
          + (f"; evidence: {root}" if keep else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
