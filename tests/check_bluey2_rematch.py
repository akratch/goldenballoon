#!/usr/bin/env python3
"""Regression for Bluey's second-boss race and its authored audio transition.

The primary arm resumes a checksum-valid Adventure One checkpoint in which
Bluey 1 and all four Snowflake Mountain silver-coin races are complete and
Bluey 2 has already been visited. It reaches a normal race load through the
production hub/lobby route, retargets that one load to course 52, and observes
Bluey's real vehicle update until the natural loss verdict.

Original two-field simulation is the product/default contract. The Enhanced
one-field option is a deliberate positive control: it must reproduce the much
faster boss or this check has lost sensitivity to the reported failure mode.
The audio assertion records the Original arm headlessly. It freezes the
original game's intro-cue stop and delayed race-cue start while requiring the
intervening mix to remain alive; the reported "dip" is a cue transition, not a
digital dropout.

All EEPROM and PCM data are created in temporary directories and deleted.
Nothing ROM-derived is committed by this check.
"""

from __future__ import annotations

import argparse
import array
from dataclasses import dataclass
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import wave

from check_first_boss_progression import (
    RECORD_BYTES,
    RECORD_OFFSETS,
    config_block,
    put_bits,
    read_bits,
    save_order,
    sum_block,
)
from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/adventure_resume_race.txt"

EEPROM_BYTES = 512
SLOT_BYTES = 40
CONFIG_OFFSET = 120
BLUEY_ONE = 1
BLUEY_TWO = 52
SNOWFLAKE_RACES = (8, 4, 10, 30)
BLUEY_ONE_BOSS_BIT = 0x008
SNOWFLAKE_BOSS_SCENE = 0x20
RACE_CLEARED = 2
SILVER_CLEARED = 3
RACE_VISITED = 1

ORIGINAL_FRAMES = 4100
ENHANCED_FRAMES = 5650
DRIVE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12;12:E5"
)

ORACLE_RE = re.compile(
    r"\[ORACLE\] frame=(?P<frame>\d+) map=-?\d+ slot=(?P<slot>\d+) "
    r".*?xv=(?P<xv>\S+) yv=(?P<yv>\S+) zv=(?P<zv>\S+) "
    r".*?cp=(?P<checkpoint>-?\d+) next=-?\d+ "
    r"lap=(?P<lap>-?\d+) countlap=-?\d+ fin=(?P<finished>-?\d+) "
    r"fpos=(?P<finish_position>-?\d+) ridx=(?P<racer_index>\d+) "
    r"pidx=(?P<player_index>-?\d+) vehicle=(?P<vehicle>-?\d+) "
    r".*?start=(?P<start_timer>-?\d+) delta=\d+ rate=(?P<update_rate>-?\d+)"
)
LEVEL_RE = re.compile(
    r"level_load: levelId=52 numPlayers=0 entrance=0 vehicle=-?\d+ cutscene=0"
)
BOSS_FINISH_RE = re.compile(
    r"bossfinish: finishPos=(?P<position>-?\d+) playerIndex=0 courseId=52 "
    r"worldId=3 bosses=0x(?P<bosses>[0-9a-f]+) "
    r"courseFlags=0x(?P<flags>[0-9a-f]+).*raceFinished=(?P<finished>-?\d+)"
)
MUSIC_RE = re.compile(
    r"\[AUDIO\] music seq=(-?\d+) tempo=-?\d+ uspt=-?\d+ "
    r"playing=(\d+) at sample=(\d+)"
)
TOTAL_RE = re.compile(r"\[AUDIO\] TOTAL frames=(\d+) samples=(\d+)")
GUARD_RE = re.compile(r"\[AUDIO\] fx-guard trips=(\d+)")
CLIP_RE = re.compile(r"\[AUDIO\] mainbus clip: (\d+)/(\d+) samples")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)


@dataclass(frozen=True)
class Row:
    frame: int
    slot: int
    xv: float
    yv: float
    zv: float
    checkpoint: int
    lap: int
    finished: int
    finish_position: int
    racer_index: int
    player_index: int
    vehicle: int
    start_timer: int
    update_rate: int


@dataclass(frozen=True)
class RaceResult:
    label: str
    finish_clock: int
    finish_frame: int
    finish_position: int
    max_checkpoint: int
    max_lap: int
    mean_speed: float
    countdown_max: int
    go_frame: int
    update_rates: frozenset[int]
    output: str
    audio: bytes | None


def checkpoint_image(eligible: list[int]) -> bytes:
    status = {
        BLUEY_ONE: RACE_CLEARED,
        BLUEY_TWO: RACE_VISITED,
        **{course: SILVER_CLEARED for course in SNOWFLAKE_RACES},
    }
    bits: list[int] = []
    put_bits(bits, 16, 0)  # checksum, filled below
    for ordinal in range(34):
        course = eligible[ordinal] if ordinal < len(eligible) else -1
        put_bits(bits, 2, status.get(course, 0))
    put_bits(bits, 6, 0)  # Taj flags
    put_bits(bits, 10, 0)  # trophies
    put_bits(bits, 12, BLUEY_ONE_BOSS_BIT)
    # Nine Snowflake balloons is the minimal completed first-boss + silver set.
    for value in (9, 0, 0, 9, 0, 0):
        put_bits(bits, 7, value)
    put_bits(bits, 3, 0)  # TT amulet
    put_bits(bits, 3, 0)  # Wizpig amulet
    for _ in range(6):
        put_bits(bits, 16, 0)  # world object flags
    put_bits(bits, 8, 0)  # keys
    put_bits(bits, 32, SNOWFLAKE_BOSS_SCENE)
    put_bits(bits, 16, 0x1234)  # named/started file
    put_bits(bits, 8, 0)
    if len(bits) != SLOT_BYTES * 8:
        raise ValueError(f"checkpoint encoded {len(bits)} bits")

    slot = bytearray(
        sum(bits[i + j] << (7 - j) for j in range(8))
        for i in range(0, len(bits), 8)
    )
    slot[:2] = ((5 + sum(slot[2:])) & 0xFFFF).to_bytes(2, "big")
    image = bytearray(EEPROM_BYTES)
    image[:SLOT_BYTES] = slot
    image[SLOT_BYTES:CONFIG_OFFSET] = b"\xFF" * (CONFIG_OFFSET - SLOT_BYTES)
    image[CONFIG_OFFSET:CONFIG_OFFSET + 8] = config_block()
    for offset in RECORD_OFFSETS:
        image[offset:offset + RECORD_BYTES] = sum_block(RECORD_BYTES)

    if read_bits(image, 0, 16) != ((5 + sum(image[2:SLOT_BYTES])) & 0xFFFF):
        raise ValueError("canonical checkpoint checksum is invalid")
    for course, wanted in status.items():
        actual = read_bits(image, 16 + eligible.index(course) * 2, 2)
        if actual != wanted:
            raise ValueError(
                f"canonical checkpoint course {course}={actual}, expected {wanted}"
            )
    if read_bits(image, 100, 12) != BLUEY_ONE_BOSS_BIT:
        raise ValueError("canonical checkpoint does not carry the Bluey 1 boss bit")
    return bytes(image)


def parse_rows(output: str) -> list[Row]:
    rows: list[Row] = []
    for match in ORACLE_RE.finditer(output):
        values = match.groupdict()
        rows.append(
            Row(
                frame=int(values["frame"]),
                slot=int(values["slot"]),
                xv=float(values["xv"]),
                yv=float(values["yv"]),
                zv=float(values["zv"]),
                checkpoint=int(values["checkpoint"]),
                lap=int(values["lap"]),
                finished=int(values["finished"]),
                finish_position=int(values["finish_position"]),
                racer_index=int(values["racer_index"]),
                player_index=int(values["player_index"]),
                vehicle=int(values["vehicle"]),
                start_timer=int(values["start_timer"]),
                update_rate=int(values["update_rate"]),
            )
        )
    return rows


def analyse_race(label: str, output: str, audio: bytes | None) -> RaceResult:
    bluey = [
        row for row in parse_rows(output)
        if row.slot == 1 and row.racer_index == 1
        and row.player_index == -1 and row.vehicle == 6
    ]
    if not bluey:
        raise ValueError(f"{label}: no Bluey (slot/racer 1, vehicle 6) state")

    saw_countdown = False
    tick = 0
    go_frame = -1
    race_rows: list[Row] = []
    for row in bluey:
        if row.start_timer > 0:
            saw_countdown = True
            continue
        if not saw_countdown:
            continue
        if go_frame < 0:
            go_frame = row.frame
        tick += max(1, row.update_rate)
        race_rows.append(row)
        if row.finished:
            break
    if not race_rows or not race_rows[-1].finished:
        raise ValueError(f"{label}: Bluey did not finish")

    moving = [row for row in race_rows if not row.finished]
    speeds = [
        math.sqrt(row.xv * row.xv + row.yv * row.yv + row.zv * row.zv)
        for row in moving
    ]
    return RaceResult(
        label=label,
        finish_clock=tick,
        finish_frame=race_rows[-1].frame,
        finish_position=race_rows[-1].finish_position,
        max_checkpoint=max(row.checkpoint for row in race_rows),
        max_lap=max(row.lap for row in race_rows),
        mean_speed=sum(speeds) / len(speeds),
        countdown_max=max(row.start_timer for row in bluey),
        go_frame=go_frame,
        update_rates=frozenset(row.update_rate for row in race_rows),
        output=output,
        audio=audio,
    )


def invoke(
    binary: Path,
    rom: Path,
    checkpoint: bytes,
    label: str,
    cadence: str,
    frames: int,
    timeout: int,
) -> RaceResult:
    with tempfile.TemporaryDirectory(prefix=f"mdkr_bluey2_{label}_") as temp:
        run_dir = Path(temp)
        save_dir = run_dir / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(checkpoint)
        wav_path = run_dir / "bluey2.wav"

        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("MDKR", "GE007_"))
        }
        env.update(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
            MDKR_LOAD_TRACK=str(BLUEY_TWO),
            MDKR_ORACLE_STATE="1",
            MDKR_RENDERER="gl",
            MDKR_SAVE_DIR=str(save_dir),
            MDKR_SIMULATION_CADENCE=cadence,
            MDKR_SYNTH_FIELDS="2" if cadence == "original" else "1",
            MDKR_TRACE="1",
            MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        if cadence == "original":
            env.update(
                MDKR_AUDIO_DUMP=str(wav_path),
                MDKR_AUDIO_RMS="1",
            )
        command = [
            str(binary), "--headless-frames", str(frames),
            "--input-script", str(SCRIPT), "--rom", str(rom),
        ]
        process = subprocess.run(
            command,
            cwd=run_dir,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        output = process.stdout or ""
        if process.returncode != 0:
            raise RuntimeError(
                f"{label}: mdkr64 exited {process.returncode}:\n{output[-4000:]}"
            )
        if BAD_RE.search(output):
            raise RuntimeError(f"{label}: runtime failure marker in output")
        if not LEVEL_RE.search(output):
            raise RuntimeError(f"{label}: production route never loaded Bluey 2")
        if "bossredirect:" in output:
            raise RuntimeError(
                f"{label}: checkpoint was not a visited, progression-valid rematch"
            )
        verdicts = list(BOSS_FINISH_RE.finditer(output))
        if not verdicts:
            raise RuntimeError(f"{label}: no production boss verdict")
        verdict = verdicts[0]
        if (
            int(verdict["position"]) != 2
            or int(verdict["bosses"], 16) & BLUEY_ONE_BOSS_BIT == 0
            or int(verdict["flags"], 16) & RACE_VISITED == 0
            or int(verdict["finished"]) == 0
        ):
            raise RuntimeError(f"{label}: invalid natural loss verdict {verdict.group(0)}")

        saved = (save_dir / "eeprom.bin").read_bytes()
        if len(saved) != EEPROM_BYTES or read_bits(saved, 0, 16) != (
            (5 + sum(saved[2:SLOT_BYTES])) & 0xFFFF
        ):
            raise RuntimeError(f"{label}: persisted EEPROM checksum is invalid")
        audio = wav_path.read_bytes() if wav_path.is_file() else None
        return analyse_race(label, output, audio)


def cadence_failures(original: RaceResult, enhanced: RaceResult) -> list[str]:
    failures: list[str] = []
    for result, expected_rate in ((original, 2), (enhanced, 1)):
        if result.finish_position != 1:
            failures.append(
                f"{result.label}: Bluey finish position {result.finish_position}, want 1"
            )
        if result.max_checkpoint < 24 or result.max_lap < 2:
            failures.append(
                f"{result.label}: incomplete course progress "
                f"cp={result.max_checkpoint} lap={result.max_lap}"
            )
        if result.countdown_max != 80:
            failures.append(
                f"{result.label}: countdown max {result.countdown_max}, want 80"
            )
        if result.update_rates != frozenset((expected_rate,)):
            failures.append(
                f"{result.label}: update rates {sorted(result.update_rates)}, "
                f"want only {expected_rate}"
            )
    if not 3350 <= original.finish_clock <= 3650:
        failures.append(
            f"Original: finish clock {original.finish_clock} outside 3350..3650"
        )
    if not 2920 <= enhanced.finish_clock <= 3120:
        failures.append(
            f"Enhanced control: finish clock {enhanced.finish_clock} outside 2920..3120"
        )
    if original.finish_clock - enhanced.finish_clock < 350:
        failures.append(
            "Enhanced control no longer reproduces a materially faster boss "
            f"({original.finish_clock} vs {enhanced.finish_clock})"
        )
    speed_ratio = enhanced.mean_speed / original.mean_speed
    if speed_ratio < 1.10:
        failures.append(
            f"Enhanced control speed ratio {speed_ratio:.4f} is below 1.10"
        )
    return failures


def pcm_rms(values: array.array) -> float:
    return (
        math.sqrt(sum(value * value for value in values) / len(values))
        if values else 0.0
    )


def longest_zero_run(values: array.array) -> int:
    longest = current = 0
    for value in values:
        if value == 0:
            current += 1
            longest = max(longest, current)
        else:
            current = 0
    return longest


def validate_audio(result: RaceResult) -> tuple[list[str], dict[str, float]]:
    failures: list[str] = []
    if result.audio is None:
        return ["Original: no PCM capture"], {}
    with tempfile.NamedTemporaryFile(suffix=".wav") as handle:
        handle.write(result.audio)
        handle.flush()
        with wave.open(handle.name, "rb") as wav:
            fmt = (wav.getframerate(), wav.getnchannels(), wav.getsampwidth())
            frame_count = wav.getnframes()
            samples = array.array("h")
            samples.frombytes(wav.readframes(frame_count))
    if sys.byteorder == "big":
        samples.byteswap()
    if fmt != (22050, 2, 2):
        failures.append(f"Original audio format {fmt}, want (22050, 2, 2)")
        return failures, {}

    total = TOTAL_RE.search(result.output)
    guard = GUARD_RE.search(result.output)
    clip = CLIP_RE.search(result.output)
    if total is None or int(total[1]) <= 0:
        failures.append("Original audio has no synthesis accounting")
        return failures, {}
    pumps = int(total[1])
    sample_frames_per_present = frame_count / pumps
    if not 735.0 <= sample_frames_per_present <= 737.0:
        failures.append(
            "Original audio produced "
            f"{sample_frames_per_present:.2f} sample-frames/present, want about 736"
        )
    if guard is None or int(guard[1]) != 0:
        failures.append("Original audio reverb guard tripped or was not reported")
    if clip is None or int(clip[1]) / max(1, int(clip[2])) > 0.005:
        failures.append("Original audio main-bus clip rate exceeds 0.5%")

    rate = fmt[0]
    go_sample = int(round(result.go_frame * sample_frames_per_present))
    six_seconds = min(frame_count, go_sample + int(6.0 * rate))
    race_mix = samples[2 * go_sample:2 * six_seconds]
    window = int(0.25 * rate) * 2
    window_rms = [
        pcm_rms(race_mix[offset:offset + window])
        for offset in range(0, len(race_mix) - window + 1, window)
    ]
    if len(window_rms) < 20 or min(window_rms, default=0.0) < 1500.0:
        failures.append(
            "Original mix drops out around GO "
            f"(windows={len(window_rms)}, min RMS={min(window_rms, default=0):.1f})"
        )
    zero_run = longest_zero_run(race_mix)
    if zero_run > int(0.02 * rate * 2):
        failures.append(
            f"Original mix contains a zero run of {zero_run} samples around GO"
        )

    events = [tuple(map(int, match.groups())) for match in MUSIC_RE.finditer(result.output)]
    stop_index = max(
        (i for i, event in enumerate(events) if event[1] == 0 and event[2] < go_sample),
        default=-1,
    )
    race_index = next(
        (i for i, event in enumerate(events) if i > stop_index and event[:2] == (57, 1)),
        -1,
    )
    intro_ok = any(event[:2] == (30, 1) for event in events[:stop_index])
    if stop_index < 0 or race_index < 0 or not intro_ok:
        failures.append("Original Bluey intro-stop/race-cue transition was not observed")
        return failures, {}
    stop_sample = events[stop_index][2]
    race_sample = events[race_index][2]
    gap_seconds = (race_sample - stop_sample) / rate
    race_cue_after_go = (race_sample - go_sample) / rate
    if not 2.8 <= gap_seconds <= 3.6:
        failures.append(f"Original music transition gap is {gap_seconds:.3f}s")
    if not 0.5 <= race_cue_after_go <= 1.4:
        failures.append(
            f"Original race cue starts {race_cue_after_go:.3f}s after GO"
        )
    gap_pcm = samples[2 * stop_sample:2 * race_sample]
    gap_rms = pcm_rms(gap_pcm)
    gap_zero_fraction = gap_pcm.count(0) / len(gap_pcm) if gap_pcm else 1.0
    if gap_rms < 1500.0 or gap_zero_fraction > 0.05:
        failures.append(
            "Authored music transition became an audio dropout "
            f"(RMS={gap_rms:.1f}, zeros={gap_zero_fraction:.2%})"
        )
    return failures, {
        "min_window_rms": min(window_rms, default=0.0),
        "gap_seconds": gap_seconds,
        "gap_rms": gap_rms,
        "race_cue_after_go": race_cue_after_go,
        "zero_run": float(zero_run),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1
    try:
        eligible = save_order(str(rom))
        for course in (BLUEY_ONE, BLUEY_TWO, *SNOWFLAKE_RACES):
            if course not in eligible:
                raise ValueError(f"course {course} absent from save ordering")
        checkpoint = checkpoint_image(eligible)
        original = invoke(
            binary, rom, checkpoint, "Original", "original",
            ORIGINAL_FRAMES, args.timeout,
        )
        enhanced = invoke(
            binary, rom, checkpoint, "Enhanced control", "enhanced",
            ENHANCED_FRAMES, args.timeout,
        )
        failures = cadence_failures(original, enhanced)
        audio_failures, audio = validate_audio(original)
        failures.extend(audio_failures)

        # Self-validation without another run: substituting the healthy arm for
        # the positive control must be rejected by the relationship assertions.
        if not cadence_failures(original, original):
            failures.append("same-as-Original sensitivity control was not rejected")
    except (OSError, RuntimeError, subprocess.TimeoutExpired, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    speed_ratio = enhanced.mean_speed / original.mean_speed
    print(
        "Bluey 2 progression-valid rematch: "
        f"Original finish={original.finish_clock} ticks speed={original.mean_speed:.3f}; "
        f"Enhanced control finish={enhanced.finish_clock} ticks "
        f"speed={enhanced.mean_speed:.3f} ({speed_ratio:.4f}x)"
    )
    if audio:
        print(
            "  authored cue transition: "
            f"gap={audio['gap_seconds']:.3f}s, "
            f"race cue={audio['race_cue_after_go']:.3f}s after GO, "
            f"gap RMS={audio['gap_rms']:.1f}, "
            f"minimum 250ms RMS={audio['min_window_rms']:.1f}"
        )
    if failures:
        print(f"check_bluey2_rematch: FAIL ({len(failures)} issue(s))")
        for failure in failures:
            print(f"  - {failure}")
        if args.verbose:
            print("\nOriginal tail:\n" + original.output[-4000:])
            print("\nEnhanced tail:\n" + enhanced.output[-4000:])
        return 1
    print("check_bluey2_rematch: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
