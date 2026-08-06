#!/usr/bin/env python3
"""Prove that the final-lap request changes the live sequencer and PCM tempo.

The gameplay-side BPM is not sufficient evidence: ``music_tempo_set()`` updates
that value before the queued MIDI meta event reaches the native sequence player.
This check finishes a real three-lap Time Trial, captures the produced PCM, and
requires all three layers to agree:

* the racer reaches lap three;
* the same music sequence reports a 12% BPM increase and a matching decrease in
  its live microseconds-per-quarter-note value; and
* the final-lap PCM beat grid is centred on that new live tempo.

It also substitutes the old tempo into the final-lap PCM window and requires
the beat-grid assertion to fail, proving the check detects the original defect.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from check_audio_output import (
    TEMPO_ARGMAX_TOL,
    TEMPO_DETECT_MIN,
    TEMPO_MARGIN,
    frames_of,
    load_wav,
    music_windows,
    parse_engine,
    tempo_report,
)
from harness_utils import (fatal_re, FX_MARKERS, resolve_binary,
                           VALIDATION_MARKERS)


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
FRAMES = 9000
PACE_RE = re.compile(r"\[PACE\] frame=(\d+).* lap=(-?\d+) rlap=(-?\d+)")
FATAL_RE = fatal_re(*FX_MARKERS, *VALIDATION_MARKERS)


def beat_grid_ok(row: dict[str, float | int]) -> bool:
    return (
        float(row["best"]) >= TEMPO_DETECT_MIN
        and abs(float(row["argmax"]) - 1.0) <= TEMPO_ARGMAX_TOL
        and float(row["margin"]) >= TEMPO_MARGIN
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if not binary.is_file() or not rom.is_file() or not SCRIPT.is_file():
        print("check_final_lap_music: FAIL -- missing binary, ROM, or route",
              file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="mdkr-final-lap-music-") as temp:
        work = Path(temp)
        save = work / "save"
        save.mkdir()
        wav = work / "final-lap.wav"
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("MDKR_", "GE007_"))
        }
        env.update(
            MDKR_AUDIO="0",
            MDKR_AUDIO_RMS="1",
            MDKR_AUDIO_DUMP=str(wav),
            MDKR_RENDERER="webgpu",
            MDKR_PRESENT_RATE="original",
            MDKR_PRESENT_SMOOTHING="off",
            MDKR_SIMULATION_CADENCE="enhanced",
            MDKR_SYNTH_FIELDS="1",
            MDKR_TRACE="1",
            MDKR_AUTOPILOT="1",
            MDKR_SAVE_DIR=str(save),
        )
        command = [
            str(binary), "--headless-frames", str(FRAMES),
            "--input-script", str(SCRIPT), "--rom", str(rom), "--restored",
        ]
        try:
            proc = subprocess.run(
                command, cwd=work, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except subprocess.TimeoutExpired:
            print(f"check_final_lap_music: FAIL -- timed out after "
                  f"{args.timeout}s", file=sys.stderr)
            return 1

        output = proc.stdout
        fatal = FATAL_RE.search(output)
        if proc.returncode != 0 or fatal is not None or not wav.is_file():
            print(
                "check_final_lap_music: FAIL -- "
                f"exit={proc.returncode}, fatal="
                f"{fatal.group(0) if fatal else 'none'}, wav={wav.is_file()}",
                file=sys.stderr,
            )
            return 1

        progress = [
            (int(match.group(1)), int(match.group(2)), int(match.group(3)))
            for line in output.splitlines()
            if (match := PACE_RE.search(line))
        ]
        if not progress or max(row[2] for row in progress) < 3:
            reached = max((row[2] for row in progress), default=-1)
            print(f"check_final_lap_music: FAIL -- route reached rlap={reached}, "
                  "not lap three", file=sys.stderr)
            return 1

        info = parse_engine(output)
        rate, channels, width, samples = load_wav(str(wav))
        windows = music_windows(info, frames_of(samples))
        changes = [
            (before, after)
            for before, after in zip(windows, windows[1:])
            if before[0] == after[0] and before[1] > 0
            and after[1] == int(before[1] * 1.12)
        ]
        if len(changes) != 1:
            summary = [(row[0], row[1], row[2]) for row in windows]
            print(f"check_final_lap_music: FAIL -- expected one in-sequence "
                  f"12% tempo request, found {len(changes)}: {summary}",
                  file=sys.stderr)
            return 1

        before, after = changes[0]
        expected_ratio = after[1] / before[1]
        live_ratio = before[2] / after[2] if after[2] > 0 else 0.0
        if after[2] >= before[2] or abs(live_ratio - expected_ratio) > 0.005:
            print(
                "check_final_lap_music: FAIL -- gameplay requested "
                f"{before[1]}->{after[1]} BPM but live tempo stayed "
                f"{before[2]}->{after[2]} us/qn "
                f"(ratio {live_ratio:.5f}, want {expected_ratio:.5f})",
                file=sys.stderr,
            )
            return 1

        rows = tempo_report(samples, (after,), rate)
        if len(rows) != 1 or not beat_grid_ok(rows[0]):
            print(f"check_final_lap_music: FAIL -- final-lap PCM does not "
                  f"match its live tempo: {rows}", file=sys.stderr)
            return 1

        # Broken-direction control: describe the same final-lap PCM using the
        # pre-change live tempo. It must no longer satisfy the beat-grid oracle.
        stale = (after[0], after[1], before[2], after[3], after[4])
        stale_rows = tempo_report(samples, (stale,), rate)
        if len(stale_rows) != 1 or beat_grid_ok(stale_rows[0]):
            print("check_final_lap_music: FAIL -- stale-tempo control did not "
                  "reproduce the original defect", file=sys.stderr)
            return 1

        row = rows[0]
        print(
            "check_final_lap_music: PASS -- lap three reached; "
            f"sequence {after[0]} changed {before[1]}->{after[1]} BPM, "
            f"live tempo {before[2]}->{after[2]} us/qn; PCM beat grid "
            f"argmax={float(row['argmax']):.4f}x, "
            f"margin={float(row['margin']):+.4f}; stale-tempo control rejected; "
            f"format={rate}Hz/{channels}ch/{width * 8}-bit"
        )
        return 0


if __name__ == "__main__":
    sys.exit(main())
