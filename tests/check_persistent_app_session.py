#!/usr/bin/env python3
"""Prove two rematches return through one live launcher process.

This is deliberately ROM-optional in CI: the repository cannot distribute the
cartridge image needed to boot the translated game. Release and local evidence
runs pass ``--rom`` and exercise the production executable without replacing
the engine entry point or teardown path.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


TICKS = 5
ROUND_RE = re.compile(
    rf"\[session-test\] engine round=(1|2|3) epoch=(1|2|3) "
    rf"id=(\d+) ticks={TICKS} complete"
)


def fail(message: str, output: str) -> int:
    print(f"FAIL persistent app session: {message}", file=sys.stderr)
    print(output[-9000:], file=sys.stderr)
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
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    if not binary.is_file():
        parser.error(f"missing native app binary: {binary}")
    if not rom.is_file():
        parser.error(f"missing ROM: {rom}")

    with tempfile.TemporaryDirectory(prefix="mdkr64-persistent-session-") as temp:
        root = Path(temp)
        prefs = root / "preferences"
        saves = root / "saves"
        prefs.mkdir()
        saves.mkdir()
        config = root / "video.ini"
        environment = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
            MDKR_APP_PREFS_DIR=str(prefs),
            MDKR_APP_TEST_SESSION_ROUNDTRIPS="3",
            MDKR_AUDIO="0",
            MDKR_RENDERER="gl",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(saves),
            MDKR_VIDEO_CONFIG_PATH=str(config),
            MDKR64_HIDDEN="1",
        )
        if args.verbose:
            print(f"$ {binary}  # three epochs / two in-process rematches", flush=True)
        try:
            process = subprocess.run(
                [str(binary)], cwd=root, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return fail(f"could not complete lifecycle run: {error}", "")

    output = process.stdout or ""
    if process.returncode != 0:
        return fail(f"application exited {process.returncode}", output)

    rounds = ROUND_RE.findall(output)
    if [(round_no, epoch) for round_no, epoch, _ in rounds] != [
            ("1", "1"), ("2", "2"), ("3", "3")]:
        return fail(f"round/epoch witnesses were {rounds!r}", output)
    identities = {identity for _, _, identity in rounds}
    if len(identities) != 1 or "0" in identities:
        return fail(f"session identity changed or was invalid: {identities!r}", output)

    identity = next(iter(identities))
    expected_pass = (
        f"[session-test] persistent native lifecycle passed rounds=3 id={identity}"
    )
    if output.count(expected_pass) != 1:
        return fail("missing the final persistent-lifecycle verdict", output)
    if output.count(
            f"[SDL] headless: reached {TICKS} simulation ticks, exiting cleanly."
    ) != 3:
        return fail("all three epochs did not complete the requested simulation work", output)
    if output.count("[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0") != 3:
        return fail("one or more engine teardowns retained host-owned state", output)
    forbidden = (
        "[MEM] free rejected",
        "simulation witness mismatch",
        "persistent identity/epoch mismatch",
    )
    for marker in forbidden:
        if marker in output:
            return fail(f"observed forbidden lifecycle diagnostic {marker!r}", output)

    if args.verbose:
        print(output, end="" if output.endswith("\n") else "\n")
    print(
        f"PASS persistent app session: id={identity}, epochs=3, "
        f"rematches=2, ticks={TICKS} each"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
