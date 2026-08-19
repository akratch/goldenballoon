#!/usr/bin/env python3
"""Verify that Taj's entrance theme starts with every authored layer audible."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "taj_theme.txt"
EXPECTED_CHANNELS = {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13}
FATAL_MARKERS = ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:")
DRIVE_ROUTE = "0:-197,193"


def entrance_segment(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    """Return the final sequence, identified by the sequencer tick reset."""
    start = 0
    last_tick = -1
    for index, row in enumerate(rows):
        tick = row.get("seq_ticks")
        if isinstance(tick, int):
            if tick < last_tick:
                start = index
            last_tick = tick
    return rows[start:]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--frames", type=int, default=7200)
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build))
    rom = Path(args.rom).expanduser()
    if not binary.is_file() or not rom.is_file():
        print("check_taj_theme: FAIL -- executable or ROM is missing", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="mdkr-taj-theme-") as temp:
        temp_path = Path(temp)
        note_path = temp_path / "notes.jsonl"
        save_path = temp_path / "save"
        save_path.mkdir()
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("MDKR_", "GE007_"))
        }
        env.update({
            "LC_ALL": "C",
            "MDKR_AUDIO": "0",
            "MDKR_AUDIO_RMS": "1",
            "MDKR_HIDDEN": "1",
            "MDKR_TRACE": "1",
            "MDKR_RENDERER": "gl",
            "MDKR_SAVE_DIR": str(save_path),
            # Isolate the video config for the same reason the save dir is
            # isolated (see the check_door_blocks note): a repo-root
            # mdkr64.ini with display-paced smoothing turns --headless-frames
            # into a presents budget and the drive never reaches Taj.
            "MDKR_VIDEO_CONFIG_PATH": str(temp_path / "video.ini"),
            "MDKR_MUSIC_MIDI_TRACE_JSONL": str(note_path),
            "MDKR_DRIVE_ROUTE": DRIVE_ROUTE,
        })
        result = subprocess.run(
            [str(binary), "--headless-frames", str(args.frames),
             "--input-script", str(SCRIPT), "--rom", str(rom)],
            cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=args.timeout, check=False,
        )
        if result.returncode != 0:
            print(f"check_taj_theme: FAIL -- runner exited {result.returncode}",
                  file=sys.stderr)
            print(result.stdout[-4000:], file=sys.stderr)
            return 1
        bad = next((marker for marker in FATAL_MARKERS if marker in result.stdout), None)
        if bad is not None or "music seq=32 " not in result.stdout:
            reason = bad or "Taj entrance sequence 32 never started"
            print(f"check_taj_theme: FAIL -- {reason}", file=sys.stderr)
            return 1
        try:
            rows = [json.loads(line) for line in note_path.read_text().splitlines()]
        except (OSError, json.JSONDecodeError) as error:
            print(f"check_taj_theme: FAIL -- invalid MIDI trace: {error}",
                  file=sys.stderr)
            return 1

    segment = entrance_segment(rows)
    heard = {
        row.get("chan") for row in segment if row.get("event") == "note_on"
    }
    missing = EXPECTED_CHANNELS - heard
    disabled = {
        row.get("chan") for row in segment
        if row.get("event") == "note_reject"
        and row.get("reason") == "channel-disabled"
    }
    if missing or disabled:
        print(
            "check_taj_theme: FAIL -- entrance theme channel mask is stale; "
            f"missing={sorted(missing)}, disabled={sorted(disabled)}",
            file=sys.stderr,
        )
        return 1

    print(
        "check_taj_theme: PASS -- sequence 32 played all 13 authored channels "
        "without a channel-disabled rejection"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
