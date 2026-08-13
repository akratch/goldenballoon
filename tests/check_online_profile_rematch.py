#!/usr/bin/env python3
"""Prove impaired carrier state and authentication retire at each rematch epoch."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/race_4p_split.txt"
ROUNDS = 3
TICKS = 2800
ROSTER_RE = re.compile(r"^\[NET-ROSTER\] epoch=(\d+) manifest=([0-9a-f]{16}) ", re.M)
ROUND_RE = re.compile(
    rf"^\[session-test\] engine round=(\d+) epoch=(\d+) id=(\d+) "
    rf"ticks={TICKS} complete$", re.M
)
PROFILE_RE = re.compile(
    r"^\[NET-PROFILE\] name=regional-variable sent=(\d+) dropped=(\d+) "
    r"duplicate=(\d+) reordered=(\d+) corrupted=(\d+) outage=(\d+) "
    r"throttled=(\d+) overflow=(\d+) decoded=(\d+) rejected=(\d+) "
    r"offers=(\d+) skipped=(\d+) long=(\d+) sleep=(\d+)$", re.M
)
TRANSPORT_RE = re.compile(
    r"^\[NET-TRANSPORT\] epoch=(\d+) accepted=(\d+) corrected=(\d+) "
    r"duplicate=(\d+) invalid=(\d+) stale=(\d+) unauthorized=(\d+) "
    r"conflict=(\d+) outWindow=(\d+) drained=(\d+) drainRejected=(\d+) "
    r"recovery=(\d+) takeoverStarted=(\d+) takeoverIgnored=(\d+)$", re.M
)


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online profile rematch: {message}", file=sys.stderr)
    if output:
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
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM"),
                        (SCRIPT, "input script")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    with tempfile.TemporaryDirectory(prefix="mdkr64-online-rematch-") as temp:
        root = Path(temp)
        (root / "preferences").mkdir()
        (root / "saves").mkdir()
        environment = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_INPUT_SCRIPT=str(SCRIPT),
            MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
            MDKR_APP_PREFS_DIR=str(root / "preferences"),
            MDKR_APP_TEST_NET_PROFILE="regional-variable",
            MDKR_APP_TEST_NET_PROFILE_START_TICK="30",
            MDKR_APP_TEST_ONLINE_LOCAL_MASK="0x1",
            MDKR_APP_TEST_ONLINE_LOOPBACK_INPUTS="1",
            MDKR_APP_TEST_ONLINE_VIEWPORT_MASK="0x1",
            MDKR_APP_TEST_SESSION_ROUNDTRIPS=str(ROUNDS),
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_EVENT_HASH="1",
            MDKR_INPUT_HASH="1",
            MDKR_NO_CRASH_HANDLER="1",
            MDKR_PRESENT_RATE="original",
            MDKR_RENDERER="gl",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(root / "saves"),
            MDKR_STATE_HASH="3",
            MDKR_TEST_SCRIPT_ONLY_INPUT="1",
            MDKR_VIDEO_CONFIG_PATH=str(root / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        if args.verbose:
            print(f"$ {binary}  # three impaired online epochs", flush=True)
        try:
            process = subprocess.run(
                [str(binary)], cwd=root, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return fail(f"could not complete rematches: {error}")

    output = process.stdout or ""
    if process.returncode != 0:
        return fail(f"application exited {process.returncode}", output)
    forbidden = (
        "[FATAL]", "[CRASH]", "[NET-RECOVERY]", "overflow=1",
        "overflows=1", "forbidden_io=1", "simulation witness mismatch",
        "persistent identity/epoch mismatch",
    )
    for marker in forbidden:
        if marker in output:
            return fail(f"observed forbidden marker {marker!r}", output)

    rosters = ROSTER_RE.findall(output)
    if [epoch for epoch, _ in rosters] != ["1", "2", "3"] or \
            len({digest for _, digest in rosters}) != ROUNDS:
        return fail(f"roster epochs/digests were {rosters!r}", output)
    rounds = ROUND_RE.findall(output)
    if [(number, epoch) for number, epoch, _ in rounds] != [
            ("1", "1"), ("2", "2"), ("3", "3")]:
        return fail(f"round/epoch witnesses were {rounds!r}", output)
    identities = {identity for _, _, identity in rounds}
    if len(identities) != 1 or "0" in identities:
        return fail(f"launcher identity changed: {identities!r}", output)

    profiles = [tuple(map(int, row)) for row in PROFILE_RE.findall(output)]
    if len(profiles) != ROUNDS:
        return fail(f"profile witnesses were {profiles!r}", output)
    for index, stats in enumerate(profiles, 1):
        # Per-round witnesses only for the fields the profile makes near-certain
        # per round: sent/decoded/offer/skip/long are structural, and reorder at
        # 75/1000 over a few hundred packets misses a round with probability
        # ~1e-12. Loss (5/1000) and duplication (10/1000) are NOT per-round
        # certainties: at ~344 sends a round has no drop about one time in six,
        # and the impairment RNG is deterministically seeded, so any upstream
        # change to packet flow re-rolls a fixed die. Asserting them per round
        # made the gate fail on trees whose netcode was healthy. They are
        # witnessed across the whole session below instead.
        # Corruption/outage/rejection belong to harsher named profiles.
        if any(stats[field] == 0 for field in (0, 3, 8, 10, 11, 12)) \
                or stats[7] != 0:
            return fail(f"round {index} profile was vacuous: {stats!r}", output)
    for field, name in ((1, "loss"), (2, "duplication")):
        if sum(stats[field] for stats in profiles) == 0:
            return fail(
                f"profile applied no {name} across any round: {profiles!r}",
                output)

    transports = [tuple(map(int, row)) for row in TRANSPORT_RE.findall(output)]
    if len(transports) != ROUNDS:
        return fail(f"transport witnesses were {transports!r}", output)
    drain_counts: set[int] = set()
    for index, stats in enumerate(transports, 1):
        epoch = stats[0]
        accepted, corrected, duplicate = stats[1:4]
        invalid, stale, unauthorized, conflict, out_window = stats[4:9]
        drained, drain_rejected, recovery = stats[9:12]
        takeover_started, takeover_ignored = stats[12:14]
        if epoch != index or accepted == 0 or corrected == 0 or duplicate == 0 \
                or drained < 300 or any((invalid, stale, unauthorized,
                                         conflict, out_window,
                                         drain_rejected, recovery,
                                         takeover_started,
                                         takeover_ignored)):
            return fail(f"round {index} transport was not isolated: {stats!r}", output)
        drain_counts.add(drained)
    if len(drain_counts) != 1:
        return fail(f"online authored durations differed: {drain_counts!r}", output)
    if output.count("[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0") != ROUNDS:
        return fail("an engine epoch retained host state", output)
    verdict = (
        "[session-test] persistent native lifecycle passed rounds=3 id="
        + next(iter(identities))
    )
    if output.count(verdict) != 1:
        return fail("missing persistent launcher verdict", output)

    print(
        "PASS online profile rematch: epochs=3 rematches=2 "
        f"session={next(iter(identities))} carrier=three-frame-bundle "
        "profile=regional-variable stale=0 recovery=0 teardown=clean"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
