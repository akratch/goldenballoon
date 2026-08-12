#!/usr/bin/env python3
"""Activate every Online Room action through native keyboard and gamepad UI."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

from harness_utils import resolve_binary


ACTION_PREFIX = "online-gallery-actions-v1|"
SPEC_RE = re.compile(
    r"^online-gallery-v1 slug=([a-z0-9-]+).* admission=([01]) "
    r"timeout-visible=[01]$"
)
RESULT_RE = re.compile(
    r"\[app-ui-test\] online action=(\d+) input=(keyboard|gamepad) "
    r"queued=1 witnessed=1 accepted=1"
)
INPUT_TOKEN = "mdkr64-app-ui-input-v1"
ACTION_TOKEN = "mdkr64-online-action-v1"
LAST_ACTION = 25


class ActionError(RuntimeError):
    pass


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def run(binary: str, environment: dict[str, str], label: str,
        timeout: int) -> str:
    completed = subprocess.run(
        [binary], env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
    )
    if completed.returncode != 0:
        raise ActionError(
            f"{label} exited {completed.returncode}\n{completed.stdout[-5000:]}"
        )
    return completed.stdout


def discover(binary: str, root: Path, timeout: int) -> tuple[dict[int, str], set[str]]:
    prefs = root / "inventory-prefs"
    saves = root / "inventory-saves"
    prefs.mkdir()
    saves.mkdir()
    output = run(binary, clean_environment(
        LC_ALL="C",
        MDKR_APP_DUMP_ONLINE_GALLERY="1",
        MDKR_APP_PREFS_DIR=str(prefs),
        MDKR_VIDEO_CONFIG_PATH=str(root / "inventory-video.ini"),
        MDKR_SAVE_DIR=str(saves),
        MDKR_AUDIO="0",
    ), "action inventory", timeout)
    admission = {
        match.group(1) for match in map(SPEC_RE.match, output.splitlines())
        if match and match.group(2) == "1"
    }
    cases: dict[int, str] = {}
    for line in output.splitlines():
        if not line.startswith(ACTION_PREFIX):
            continue
        fields = line.split("|")
        if len(fields) != 6:
            raise ActionError(f"malformed action inventory row: {line}")
        slug = fields[1]
        for value in fields[2:]:
            action = int(value)
            if action:
                cases.setdefault(action, slug)
    expected = set(range(1, LAST_ACTION + 1))
    missing = sorted(expected - cases.keys())
    unexpected = sorted(cases.keys() - expected)
    if missing or unexpected:
        raise ActionError(
            f"action inventory mismatch: missing={missing}, unexpected={unexpected}"
        )
    return cases, admission


def activate(binary: str, root: Path, action: int, slug: str,
             input_mode: str, admission: set[str], timeout: int) -> None:
    case_root = root / f"{action:02d}-{input_mode}"
    prefs = case_root / "prefs"
    saves = case_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    environment = clean_environment(
        LC_ALL="C",
        MDKR_APP_PREFS_DIR=str(prefs),
        MDKR_VIDEO_CONFIG_PATH=str(case_root / "video.ini"),
        MDKR_SAVE_DIR=str(saves),
        MDKR_AUDIO="0",
        MDKR64_HIDDEN="1",
        MDKR_APP_PANEL="Online Room",
        MDKR_APP_ONLINE_FAKE="1",
        MDKR_APP_ONLINE_GALLERY=slug,
        MDKR_APP_SMOKE_FRAMES="18",
        MDKR_ONLINE_ROOM_PREVIEW="1",
        MDKR_APP_SMOKE_INPUT=input_mode,
        MDKR_APP_SMOKE_INPUT_TOKEN=INPUT_TOKEN,
        MDKR_APP_SMOKE_ONLINE_ACTION=str(action),
        MDKR_APP_ONLINE_FOCUS_ACTION=str(action),
        MDKR_APP_ONLINE_ACTION_TOKEN=ACTION_TOKEN,
    )
    if slug in admission:
        environment["MDKR_APP_ONLINE_FAKE_ALLOW_START"] = "1"
    output = run(binary, environment, f"action {action} {input_mode} ({slug})",
                 timeout)
    results = RESULT_RE.findall(output)
    if results != [(str(action), input_mode)]:
        raise ActionError(
            f"action {action} {input_mode} ({slug}) has invalid witness "
            f"{results}\n{output[-3000:]}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True)
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-online-actions-") as temporary:
            root = Path(temporary)
            cases, admission = discover(binary, root, args.timeout)
            for action, slug in sorted(cases.items()):
                for input_mode in ("keyboard", "gamepad"):
                    activate(binary, root, action, slug, input_mode,
                             admission, args.timeout)
    except (ActionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"FAIL online room actions: {error}")
        return 1
    print(
        "PASS online room actions: "
        f"actions={len(cases)} keyboard={len(cases)} gamepad={len(cases)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
