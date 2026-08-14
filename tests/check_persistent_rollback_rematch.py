#!/usr/bin/env python3
"""Run three complete 4P rollback routes through one native launcher."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


FRAMES = 8200
ROUNDS = 3
ROUND_RE = re.compile(
    r"\[session-test\] engine round=(\d+) epoch=(\d+) "
    r"id=(\d+) ticks=(\d+) complete"
)
BREADTH_RE = re.compile(r"\[ROLLBACK\] gameplay breadth: .* result=(\d+)")
READY_RE = re.compile(
    r"\[ROLLBACK\] lab ready: ranges=(\d+) snapshot=(\d+) bytes "
    r"ring=(\d+) bytes target=(\d+) epoch=(\d+)"
)


def fail(message: str, output: str) -> int:
    print(f"FAIL persistent rollback rematch: {message}", file=sys.stderr)
    print(output[-16000:], file=sys.stderr)
    return 1


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    # 240s was calibrated for a Release engine; the suite hands this gate the
    # AddressSanitizer build, whose 2,800-tick rounds run past four minutes on
    # a 14-core machine even serial. Budget the sanitizer tax honestly.
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    script = (Path(__file__).parent / "input_scripts/race_4p_split.txt").resolve()
    for path, label in ((binary, "binary"), (rom, "ROM"), (script, "input script")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    with tempfile.TemporaryDirectory(prefix="mdkr64-rollback-rematch-") as temp:
        root = Path(temp)
        (root / "preferences").mkdir()
        (root / "saves").mkdir()
        environment = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_FRAMES=str(FRAMES),
            MDKR_APP_AUTOPLAY_INPUT_SCRIPT=str(script),
            MDKR_APP_PREFS_DIR=str(root / "preferences"),
            MDKR_APP_TEST_SESSION_ROUNDTRIPS=str(ROUNDS),
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_PRESENT_RATE="original",
            MDKR_RENDERER="gl",
            MDKR_ROLLBACK_LAB="1",
            MDKR_ROLLBACK_LAB_RESIM="1",
            MDKR_ROLLBACK_LAB_ROUNDTRIP="1",
            MDKR_ROLLBACK_LAB_TARGET_TICK="4800",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(root / "saves"),
            MDKR_TEST_SCRIPT_ONLY_INPUT="1",
            MDKR_VIDEO_CONFIG_PATH=str(root / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        if args.verbose:
            print(
                f"$ {binary}  # {ROUNDS} complete 4P rollback epochs",
                flush=True,
            )
        try:
            process = subprocess.run(
                [str(binary)], cwd=root, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return fail(f"could not complete full rematch run: {error}", "")

    output = process.stdout or ""
    if process.returncode != 0:
        return fail(f"application exited {process.returncode}", output)
    rounds = ROUND_RE.findall(output)
    if [(row[0], row[1]) for row in rounds] != [
        ("1", "1"), ("2", "2"), ("3", "3")
    ]:
        return fail(f"round/epoch witnesses were {rounds!r}", output)
    identities = {row[2] for row in rounds}
    if len(identities) != 1 or "0" in identities:
        return fail(f"session identity changed: {identities!r}", output)
    required_counts = {
        f"[SDL] headless: reached {FRAMES} frames, exiting cleanly.": ROUNDS,
        "[ROLLBACK] first-boundary restore roundtrip passed tick=1": ROUNDS,
        "[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0": ROUNDS,
    }
    for marker, expected in required_counts.items():
        actual = output.count(marker)
        if actual != expected:
            return fail(
                f"expected {expected} occurrences of {marker!r}, got {actual}",
                output,
            )
    ready = [tuple(map(int, row)) for row in READY_RE.findall(output)]
    if len(ready) != ROUNDS or any(
            ranges < 130 or snapshot <= 0 or ring != snapshot * 32 or
            ring > 16 * 1024 * 1024 or target != 4800 or epoch != 0
            for index, (ranges, snapshot, ring, target, epoch)
            in enumerate(ready, start=1)):
        return fail(f"rollback authority witnesses were {ready!r}", output)
    breadth = [int(value) for value in BREADTH_RE.findall(output)]
    if len(breadth) != ROUNDS or any(value < 4 for value in breadth):
        return fail(f"full-race result counts were {breadth!r}", output)
    verdict = (
        "[session-test] persistent native lifecycle passed rounds=3 id="
        + next(iter(identities))
    )
    if output.count(verdict) != 1:
        return fail("missing persistent two-rematch verdict", output)
    forbidden = (
        "[FATAL]", "overflow=1", "overflows=1", "forbidden_io=1",
        "simulation witness mismatch", "presentation witness mismatch",
        "persistent identity/epoch mismatch",
    )
    for marker in forbidden:
        if marker in output:
            return fail(f"observed forbidden diagnostic {marker!r}", output)
    if args.verbose:
        print(output, end="" if output.endswith("\n") else "\n")
    print(
        "PASS persistent rollback rematch: "
        f"epochs={ROUNDS}, rematches=2, frames={FRAMES} each, "
        f"results={breadth}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
