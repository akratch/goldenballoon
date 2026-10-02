#!/usr/bin/env python3
"""Validate an ares audio capture against its own completion witness.

tools/run_oracle.sh runs this after an ``--audio-dump`` route. The capture is
accepted only when ares logged that it reached the expected frame limit, the
stored PCM is whole s16 stereo frames whose count equals the frame count ares
reported, the ``.rate`` sidecar names a plausible rate, the capture is at least
one second long, and (for a forced boss route) ares logged the forced retail
race load it was asked for. The capture stays outside the repository; only its
length, rate and digest are printed.

Usage: validate_ares_audio_capture.py AUDIO ARES_LOG EXPECTED_FRAMES SOURCE TARGET
(TARGET < 0 when the route forces no race load.)
"""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

COMPLETION_RE = re.compile(
    r"reached frame limit (\d+) \(dumped \d+ frame\(s\), (\d+) audio "
    r"sample-frame\(s\)\), exiting")


def validate(audio: Path, log: str, expected_frames: int, source: int,
             target: int) -> str:
    """The PASS line for a capture, or SystemExit naming why it is refused."""

    completed = COMPLETION_RE.search(log)
    if completed is None or int(completed.group(1)) != expected_frames:
        raise SystemExit("FAIL: missing expected ares audio completion witness")
    if target >= 0 and not re.search(
            rf"(?:^|\n)(?:mdkr64 oracle: )?forced retail race load {source} -> "
            rf"{target}[ \t]*(?:\n|$)", log):
        raise SystemExit("FAIL: ares did not load the requested boss race")
    rate_path = Path(str(audio) + ".rate")
    if not audio.is_file() or audio.stat().st_size < 4:
        raise SystemExit(f"FAIL: ares produced no audio capture at {audio}")
    size = audio.stat().st_size
    if size % 4:
        raise SystemExit(
            f"FAIL: ares audio capture is not whole s16 stereo frames: {size} bytes")
    try:
        rate = int(rate_path.read_text(encoding="ascii").strip())
    except (OSError, ValueError) as error:
        raise SystemExit(f"FAIL: invalid ares audio rate sidecar {rate_path}: {error}")
    if not 8000 <= rate <= 96000:
        raise SystemExit(f"FAIL: implausible ares audio rate {rate}")
    frames = size // 4
    reported_frames = int(completed.group(2))
    if frames != reported_frames:
        raise SystemExit(
            f"FAIL: ares audio capture length disagrees with completion witness: "
            f"{frames} stored frames, {reported_frames} reported")
    if frames < rate:
        raise SystemExit(
            f"FAIL: ares audio capture is shorter than one second: {frames} frames")
    hasher = hashlib.sha256()
    with audio.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return (f"ares audio capture: PASS -- {frames} frames at {rate} Hz "
            f"({frames / rate:.2f}s), sha256={hasher.hexdigest()}")


def main(argv: list[str]) -> int:
    if len(argv) != 5:
        raise SystemExit("usage: validate_ares_audio_capture.py AUDIO ARES_LOG "
                         "EXPECTED_FRAMES SOURCE TARGET")
    audio, log, expected, source, target = argv
    text = Path(log).read_text(encoding="utf-8", errors="replace")
    print(validate(Path(audio), text, int(expected), int(source), int(target)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
