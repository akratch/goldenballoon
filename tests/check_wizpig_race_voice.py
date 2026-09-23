#!/usr/bin/env python3
"""Check playable Wizpig's real race voice dispatch; not an audio-quality oracle.

Hot Top Volcano's plane route exercises both zipper celebrations and negative
reactions. Rendering and synthesis run normally, with device output muted.
The expected IDs are independently pinned to the retail Wizpig voice bank.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tests/input_scripts/race_full_3lap_tt.txt"
VOICES = {
    0x162: {0xB3, 0xB4, 0xB5, 0xB6, 0x35, 0x48, 0x51, 0x52, 0x57},
    0x1C2: {0x6D, 0x6E, 0x72, 0x75, 0x6C},
}
VOICE = re.compile(
    r"mod_racer_voice: player=0 identity=2 event=(\d+) sound=(\d+)")


def check_output(output: str) -> int:
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise ValueError(f"fatal marker: {marker}")
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
    return len(events)


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
    # Keep failures for investigation; evidence stays outside source by default.
    root = (args.evidence_dir.resolve() if args.evidence_dir else
            Path(tempfile.mkdtemp(prefix="mdkr-wizpig-race-voice-")))
    root.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_AUTOPILOT="1",
               MDKR_TRACE="1", MDKR_LOAD_TRACK="7:2",
               MDKR_WIZPIG_TEST_PLAYER="0", MDKR_SAVE_DIR=str(root / "save"),
               MDKR_VIDEO_CONFIG_PATH=str(root / "video.ini"), MDKR_RENDERER="gl")
    try:
        result = subprocess.run(
            [str(binary), "--headless-frames", "8000", "--input-script",
             str(SCRIPT), "--rom", str(rom)], cwd=root, env=env,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=240, check=False)
        (root / "run.log").write_text(result.stdout, encoding="utf-8")
        if result.returncode:
            raise ValueError(f"runner exit {result.returncode}")
        count = check_output(result.stdout)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"check_wizpig_race_voice: FAIL -- {error}; evidence: {root}")
        return 1
    print(f"check_wizpig_race_voice: PASS -- {count} Wizpig-owned positive/negative "
          f"race voice dispatches, no consecutive repeats; evidence: {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
