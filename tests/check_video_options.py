#!/usr/bin/env python3
"""End-to-end gate for the native in-game Video Options screen.

The three arms run in private working/save directories:

* success: navigates to menu 29, mutates every control, verifies live/restart
  result traces, the atomic INI, and a fresh-process reload;
* precedence: proves an environment-owned aspect is visibly locked and is not
  baked into the user's file while unrelated menu settings still work;
* storage fault: blocks the transaction lock and proves every video mutation
  fails closed while the accessibility toggle remains usable.
* malformed launcher: proves an invalid internal seed exits 2 without changing
  an existing config even though persistence was requested before main parsing.

No arm reads or writes the repository's mdkr64.ini or the player's EEPROM.
"""
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
CHANGE_RE = re.compile(
    r"video_options: item=(\d+) value=(.*?) result=(\d+) pending=(\d+)"
)
DISPLAY_RE = re.compile(
    r"\[DISPLAY\] output=(\d+)x(\d+) render=(\d+)x(\d+) "
    r"scale=([0-9.]+) effectiveScale=([0-9.]+)"
)
BAD_RE = re.compile(r"\[FATAL\]|\[CRASH\]|AddressSanitizer|runtime error:")


def run_route(
    binary: Path,
    rom: Path,
    script: Path,
    cwd: Path,
    extra_env: dict[str, str] | None = None,
) -> str:
    save = cwd / "save"
    save.mkdir(exist_ok=True)
    env = dict(
        os.environ,
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_SAVE_DIR=str(save),
        # Product settings now live below the platform preference root. Keep
        # this gate hermetic and make its atomic-file assertions target the
        # exact path exercised by the child process.
        MDKR_VIDEO_CONFIG_PATH=str(cwd / "mdkr64.ini"),
        MDKR_SYNTH_FIELDS="2",
    )
    if extra_env:
        env.update(extra_env)
    proc = subprocess.run(
        [
            str(binary),
            "--headless-frames",
            # Raised from 2900 with the WORLD SHADOWS row: the fixture gained a
            # DOWN/RIGHT pair, so RETURN now fires at 2820 and the route still
            # needs room to land back on menu 12.
            "3000",
            "--input-script",
            str(script),
            "--rom",
            str(rom),
        ],
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=300,
        check=False,
    )
    output = proc.stdout.decode("utf-8", "replace")
    if proc.returncode != 0:
        raise AssertionError(f"route exited {proc.returncode}\n{output[-5000:]}")
    bad = BAD_RE.search(output)
    if bad:
        raise AssertionError(f"route reported {bad.group(0)}\n{output[-5000:]}")
    menus = [int(match.group(1)) for match in MENU_RE.finditer(output)]
    if 29 not in menus or not menus or menus[-1] != 12:
        raise AssertionError(
            f"route did not enter Video Options and return to Options: {menus}"
        )
    return output


def renderer_env(renderer: str, extra: dict[str, str] | None = None) -> dict[str, str]:
    values = {"MDKR_RENDERER": renderer}
    if extra:
        values.update(extra)
    return values


def parse_changes(output: str) -> list[tuple[int, str, int, int]]:
    return [
        (int(item), value, int(result), int(pending))
        for item, value, result, pending in CHANGE_RE.findall(output)
    ]


def require_change(
    changes: list[tuple[int, str, int, int]],
    item: int,
    result: int,
    value: str | None = None,
) -> None:
    matches = [
        change
        for change in changes
        if change[0] == item
        and change[2] == result
        and (value is None or change[1] == value)
    ]
    if not matches:
        raise AssertionError(
            f"missing item={item} value={value!r} result={result}; got {changes}"
        )


def run_success(binary: Path, rom: Path, script: Path, renderer: str) -> None:
    with tempfile.TemporaryDirectory(prefix="mdkr-video-options-success-") as raw:
        root = Path(raw)
        output = run_route(
            binary, rom, script, root, renderer_env(renderer)
        )
        changes = parse_changes(output)
        # Landing values re-derived 2026-07-30 for the Restored native default
        # (the same owner call the web launcher made at v0.7.2): the input
        # script is unchanged, so each cycle lands one step earlier in the
        # mode/preset wheels than it did from the old Remastered start.
        require_change(changes, 0, 2, "REMASTERED")
        require_change(changes, 1, 1, "3X")
        require_change(changes, 2, 1, "4:3")
        require_change(changes, 3, 1, "50 DEG")
        require_change(changes, 4, 2, "ORIGINAL")
        require_change(changes, 5, 2, "OFF")
        # WORLD SHADOWS, added at R2 between REMASTER EFFECTS and SUBTITLES.
        # Result 1 (LIVE), not 2: the world-shadow pass is re-read per frame and
        # its depth resources are re-acquired from the cascade plan every frame,
        # so unlike RemasterFX it does not need a relaunch. The wheel starts at
        # FULL because the Presentation row above landed on REMASTERED, whose
        # preset pins it there.
        require_change(changes, 6, 1, "SOFT")
        require_change(changes, 7, 1)

        config_path = root / "mdkr64.ini"
        if not config_path.is_file():
            raise AssertionError("successful route did not create mdkr64.ini")
        text = config_path.read_text(encoding="utf-8")
        # Persisted values re-derived for the Restored default start (same
        # deterministic input script; every cycled wheel lands one step
        # earlier). AnisotropicFiltering no longer changes on this route —
        # its wheel starts at a different position — so the route persists
        # no line for it and the assertion follows the ini's actual truth.
        expected = {
            "Mode=custom",
            "RemasterFX=0",
            "Aspect=4:3",
            "RenderScale=4",
            "Mipmaps=0",
            "GameplayFOV=50",
            # The one LIVE row added at R2: it applies immediately AND persists,
            # so both halves of that contract are asserted here and above.
            "WorldShadows=soft",
        }
        missing = sorted(entry for entry in expected if entry not in text)
        if missing:
            raise AssertionError(f"persisted config missing {missing}\n{text}")
        if (root / "mdkr64.ini.tmp").exists():
            raise AssertionError("atomic config temporary survived successful replace")
        displays = [
            (
                int(output_width),
                int(output_height),
                int(render_width),
                int(render_height),
                float(scale),
                float(effective_scale),
            )
            for (
                output_width,
                output_height,
                render_width,
                render_height,
                scale,
                effective_scale,
            ) in DISPLAY_RE.findall(output)
        ]
        if len(displays) < 2:
            raise AssertionError(
                f"supersampling route omitted live display transitions: {displays}"
            )
        initial = displays[0]
        resized = [
            display
            for display in displays[1:]
            if display[0:2] == initial[0:2]
            and display[4] == 3.0
            and display[2] > initial[2]
            and display[3] > initial[3]
            and display[5] > initial[5]
        ]
        if not resized:
            raise AssertionError(
                "supersampling menu change did not enlarge the live renderer "
                f"within its GPU limit: {displays}"
            )
        if not (root / "save" / "eeprom.bin").is_file():
            raise AssertionError("subtitle mutation did not reach isolated EEPROM")

        reload_proc = subprocess.run(
            [str(binary), "--video-list"],
            cwd=root,
            env=dict(
                os.environ,
                MDKR_AUDIO="0",
                MDKR_VIDEO_CONFIG_PATH=str(config_path),
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=30,
            check=False,
        )
        reload_output = reload_proc.stdout.decode("utf-8", "replace")
        if reload_proc.returncode != 0:
            raise AssertionError(
                f"fresh-process config reload failed\n{reload_output}"
            )
        for token in (
            "[video] mode=custom",
            "Video.RenderScale",
            "Video.GameplayFOV",
            "50",
            "Video.Aspect",
            "4:3",
        ):
            if token not in reload_output:
                raise AssertionError(
                    f"fresh-process reload omitted {token!r}\n{reload_output}"
                )


def run_precedence(binary: Path, rom: Path, script: Path, renderer: str) -> None:
    with tempfile.TemporaryDirectory(prefix="mdkr-video-options-lock-") as raw:
        root = Path(raw)
        output = run_route(
            binary, rom, script, root,
            renderer_env(renderer, {"MDKR_ASPECT": "4:3"}),
        )
        changes = parse_changes(output)
        require_change(changes, 0, 3)
        require_change(changes, 2, 3, "4:3")
        require_change(changes, 1, 1)
        text = (root / "mdkr64.ini").read_text(encoding="utf-8")
        if "Aspect=" in text:
            raise AssertionError(
                "temporary environment aspect was baked into mdkr64.ini\n" + text
            )


def run_storage_fault(binary: Path, rom: Path, script: Path, renderer: str) -> None:
    with tempfile.TemporaryDirectory(prefix="mdkr-video-options-fault-") as raw:
        root = Path(raw)
        # Persistence now uses exclusive PID/serial staging names, so a fixed
        # .tmp directory no longer intersects the production transaction. A
        # directory at the exact lock path deterministically rejects the
        # transaction before any staging file can be published.
        (root / "mdkr64.ini.lock").mkdir()
        output = run_route(
            binary, rom, script, root, renderer_env(renderer)
        )
        changes = parse_changes(output)
        failed = [change for change in changes if change[2] == 4]
        # Eight since the WORLD SHADOWS row: six video rows, SUPERSAMPLING
        # twice, and now shadows. Subtitles is the only mutation that does not
        # go through the ini, so it still succeeds.
        if len(failed) != 8:
            raise AssertionError(
                f"storage fault did not fail all eight video transactions: {changes}"
            )
        require_change(changes, 7, 1)
        if (root / "mdkr64.ini").exists():
            raise AssertionError("storage-fault arm published a partial config")


def run_malformed_launcher(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="mdkr-video-launcher-invalid-") as raw:
        root = Path(raw)
        config = root / "mdkr64.ini"
        original = (
            "[Future]\n"
            "Mode=must-not-shadow-video\n"
            "\n"
            "[Video]\n"
            "Mode=restored\n"
            "RenderScale=2\n"
        )
        config.write_text(original, encoding="utf-8")
        proc = subprocess.run(
            [
                str(binary),
                "--video-launch-set",
                "Video.RenderScale=99",
                "--video-launch-persist",
                "--video-list",
            ],
            cwd=root,
            env=dict(
                os.environ,
                MDKR_AUDIO="0",
                MDKR_VIDEO_CONFIG_PATH=str(config),
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=30,
            check=False,
        )
        output = proc.stdout.decode("utf-8", "replace")
        if proc.returncode != 2:
            raise AssertionError(
                f"malformed launcher returned {proc.returncode}, expected 2\n{output}"
            )
        if "existing config left unchanged" not in output:
            raise AssertionError(
                "malformed launcher did not report fail-closed persistence\n"
                + output
            )
        if config.read_text(encoding="utf-8") != original:
            raise AssertionError("malformed launcher rewrote the existing config")
        if (root / "mdkr64.ini.tmp").exists():
            raise AssertionError("malformed launcher left an atomic temporary")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    script = (
        Path(__file__).resolve().parent
        / "input_scripts"
        / "nav_video_options_mutate.txt"
    )
    if not binary.is_file() or not rom.is_file() or not script.is_file():
        print(
            f"FAIL: missing binary={binary}, ROM={rom}, or script={script}",
            file=sys.stderr,
        )
        return 1

    try:
        requested_renderer = os.environ.get("MDKR_RENDERER", "").lower()
        renderers = (
            [requested_renderer]
            if requested_renderer in {"webgpu", "gl"}
            else ["webgpu", "gl"]
        )
        for renderer in renderers:
            run_success(binary, rom, script, renderer)
            run_precedence(binary, rom, script, renderer)
            run_storage_fault(binary, rom, script, renderer)
        run_malformed_launcher(binary)
    except (AssertionError, subprocess.TimeoutExpired) as error:
        print(f"check_video_options: FAIL\n  {error}", file=sys.stderr)
        return 1

    print(
        "check_video_options: PASS  "
        f"(menu 29, {','.join(renderers)}, all controls, atomic reload, "
        "override lock, storage fault, malformed launcher)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
