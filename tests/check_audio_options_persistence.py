#!/usr/bin/env python3
"""Exercise Audio Options save success, visible retry, and session-only exit."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import resolve_binary


MENU_RE = re.compile(r"menu_init: menuId=(\d+)")
PERSIST_RE = re.compile(
    r"audio_options: persist action=(\w+) result=(\d+) "
    r"music=(\d+) effects=(\d+)"
)
BAD_RE = re.compile(
    r"\[FATAL\]|\[CRASH\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Segmentation fault"
)


def run_route(
    binary: Path,
    rom: Path,
    script: Path,
    root: Path,
    config: Path,
    ticks: int,
) -> str:
    save = root / "save"
    save.mkdir()
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_ROM=str(rom),
        MDKR_SAVE_DIR=str(save),
        MDKR_TRACE="1",
        MDKR_VIDEO_CONFIG_PATH=str(config),
    )
    process = subprocess.run(
        [
            str(binary), "--headless-frames", str(ticks),
            "--input-script", str(script), "--rom", str(rom),
        ],
        cwd=root,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=240,
        check=False,
    )
    output = process.stdout.decode("utf-8", "replace")
    if process.returncode != 0:
        raise AssertionError(
            f"{script.stem} exited {process.returncode}\n{output[-5000:]}"
        )
    fatal = BAD_RE.search(output)
    if fatal:
        raise AssertionError(
            f"{script.stem} emitted {fatal.group(0)!r}\n{output[-5000:]}"
        )
    menus = [int(value) for value in MENU_RE.findall(output)]
    if 13 not in menus or not menus or menus[-1] != 12:
        raise AssertionError(
            f"{script.stem} did not enter Audio Options and return: {menus}"
        )
    return output


def assert_changed_levels(rows: list[tuple[str, str, str, str]]) -> None:
    if not rows:
        raise AssertionError("Audio Options emitted no persistence verdict")
    for _action, _result, music, effects in rows:
        if not (0 < int(music) < 256 and 0 < int(effects) < 256):
            raise AssertionError(f"sliders did not change from defaults: {rows}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64", type=Path)
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.expanduser().resolve()
    scripts = Path(__file__).resolve().parent / "input_scripts"
    success_script = scripts / "audio_options_persist_success.txt"
    failure_script = scripts / "audio_options_persist_failure.txt"
    for path in (binary, rom, success_script, failure_script):
        if not path.is_file():
            parser.error(f"missing required file: {path}")

    try:
        with tempfile.TemporaryDirectory(
            prefix="mdkr_audio_options_success_"
        ) as raw:
            root = Path(raw)
            config = root / "mdkr64.ini"
            output = run_route(
                binary, rom, success_script, root, config, 2160
            )
            rows = PERSIST_RE.findall(output)
            assert_changed_levels(rows)
            if [row[:2] for row in rows] != [("exit", "1")]:
                raise AssertionError(f"unexpected successful verdicts: {rows}")
            text = config.read_text(encoding="utf-8")
            for key in ("MusicVolume=", "EffectsVolume="):
                match = re.search(rf"^{key}(\d+)$", text, re.MULTILINE)
                if not match or not (0 < int(match.group(1)) < 100):
                    raise AssertionError(
                        f"successful transaction omitted changed {key!r}\n{text}"
                    )

        with tempfile.TemporaryDirectory(
            prefix="mdkr_audio_options_failure_"
        ) as raw:
            root = Path(raw)
            config = root / "missing-parent" / "mdkr64.ini"
            output = run_route(
                binary, rom, failure_script, root, config, 2360
            )
            rows = PERSIST_RE.findall(output)
            assert_changed_levels(rows)
            if [row[:2] for row in rows] != [
                ("exit", "4"), ("retry", "4"), ("session", "4")
            ]:
                raise AssertionError(f"unexpected failure workflow: {rows}")
            if "audio_options: persistence prompt visible result=4" not in output:
                raise AssertionError("save failure was not rendered to the player")
            if config.exists():
                raise AssertionError("failed transaction unexpectedly wrote config")
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"check_audio_options_persistence: FAIL\n  {error}", file=sys.stderr)
        return 1

    print(
        "check_audio_options_persistence: PASS (durable exit, visible failure, "
        "retry, and explicit session-only acceptance)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
