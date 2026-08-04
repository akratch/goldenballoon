#!/usr/bin/env python3
"""Exercise real-ROM PCM through the native SDL dummy queue evidence seam.

This is deliberately not a listening or DAC qualification. It proves that a
short engine route produces a valid, non-silent WAV containing only blocks the
native SDL queue accepted after sink recovery. SDL's dummy driver is used so
the test is safe and deterministic on automation hosts.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import wave

from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tests" / "input_scripts" / "race_drive_time_trial.txt"
SUMMARY_RE = re.compile(
    r"\[AUDIO-SINK-EVIDENCE\] accepted_blocks=(\d+) accepted_bytes=(\d+) "
    r"repaired_blocks=(\d+) rejected_blocks=(\d+) dropped_blocks=(\d+) "
    r"capture_bytes=(\d+) capture_failed=(\d+)"
)
BAD_RE = re.compile(
    r"\[FATAL\]|\[CRASH\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Segmentation fault"
)


def run_engine(binary: Path, rom: Path, frames: int, capture: Path,
               enable_sink: bool) -> tuple[int, str]:
    state_root = capture.parent / f"{capture.stem}-state"
    save_dir = state_root / "save"
    save_dir.mkdir(parents=True, exist_ok=True)
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        SDL_AUDIODRIVER="dummy",
        MDKR_AUDIO_SINK_DUMP=str(capture),
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_VIDEO_CONFIG_PATH=str(state_root / "video.ini"),
    )
    if enable_sink:
        env["MDKR_TEST_HEADLESS_AUDIO"] = "1"
    command = [
        str(binary), "--headless-frames", str(frames),
        "--input-script", str(SCRIPT), "--rom", str(rom),
    ]
    process = subprocess.run(
        command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=180, check=False,
    )
    return process.returncode, process.stdout.decode("utf-8", "replace")


def assert_clean_exit(returncode: int, output: str, label: str) -> None:
    if returncode != 0:
        raise AssertionError(f"{label} exited {returncode}\n{output[-5000:]}")
    bad = BAD_RE.search(output)
    if bad:
        raise AssertionError(f"{label} emitted {bad.group(0)!r}\n{output[-5000:]}")


def assert_accepted_capture(capture: Path, output: str) -> None:
    summaries = SUMMARY_RE.findall(output)
    if len(summaries) != 1:
        raise AssertionError(
            "expected one accepted-SDL summary\n" + output[-5000:]
        )
    accepted_blocks, accepted_bytes, repaired_blocks, rejected_blocks, \
        dropped_blocks, capture_bytes, capture_failed = map(int, summaries[0])
    if accepted_blocks <= 0 or accepted_bytes <= 0:
        raise AssertionError(f"no PCM reached SDL: {summaries[0]}")
    if rejected_blocks != 0 or dropped_blocks != 0:
        raise AssertionError(
            "short dummy route must not lose PCM: " + str(summaries[0])
        )
    if repaired_blocks != 0:
        raise AssertionError(
            "clean dummy route unexpectedly required recovery: " + str(summaries[0])
        )
    if capture_failed != 0 or capture_bytes != accepted_bytes:
        raise AssertionError("capture accounting disagrees: " + str(summaries[0]))
    if not capture.is_file():
        raise AssertionError("SDL accepted PCM but no capture was written")

    with wave.open(str(capture), "rb") as wav:
        if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth(),
            wav.getcomptype()) != (22050, 2, 2, "NONE"):
            raise AssertionError(f"unexpected WAV contract: {wav.getparams()}")
        payload = wav.readframes(wav.getnframes())
    if len(payload) != accepted_bytes or capture.stat().st_size != 44 + len(payload):
        raise AssertionError(
            f"WAV length is not exact accepted PCM: file={capture.stat().st_size} "
            f"payload={len(payload)} accepted={accepted_bytes}"
        )
    if not any(payload):
        raise AssertionError("accepted SDL capture is unexpectedly all silence")


def assert_default_headless_stays_sinkless(binary: Path, rom: Path,
                                           frames: int, capture: Path) -> None:
    returncode, output = run_engine(binary, rom, frames, capture, False)
    assert_clean_exit(returncode, output, "headless opt-out control")
    if capture.exists() or "[AUDIO-SINK-EVIDENCE]" in output:
        raise AssertionError(
            "headless mode opened an audio sink without explicit test opt-in"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64", type=Path)
    parser.add_argument("--frames", default=1200, type=int)
    parser.add_argument(
        "--control-no-opt-in", action="store_true",
        help="run only the control proving ordinary headless mode remains sinkless",
    )
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.expanduser().resolve()
    if args.frames <= 0:
        parser.error("--frames must be positive")
    for path in (binary, rom, SCRIPT):
        if not path.is_file():
            parser.error(f"missing required file: {path}")

    try:
        with tempfile.TemporaryDirectory(prefix="mdkr_audio_sink_evidence_") as raw:
            root = Path(raw)
            if args.control_no_opt_in:
                assert_default_headless_stays_sinkless(
                    binary, rom, args.frames, root / "blocked.wav"
                )
                print("check_audio_sink_evidence: PASS (headless opt-out control)")
                return 0

            capture = root / "accepted.wav"
            returncode, output = run_engine(
                binary, rom, args.frames, capture, True
            )
            assert_clean_exit(returncode, output, "accepted-SDL route")
            assert_accepted_capture(capture, output)
            assert_default_headless_stays_sinkless(
                binary, rom, args.frames, root / "blocked.wav"
            )
    except (AssertionError, OSError, subprocess.TimeoutExpired, wave.Error) as error:
        print(f"check_audio_sink_evidence: FAIL\n  {error}", file=sys.stderr)
        return 1

    print(
        "check_audio_sink_evidence: PASS (real-ROM accepted SDL PCM, valid WAV, "
        "no loss on dummy, and ordinary headless opt-out)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
