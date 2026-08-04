#!/usr/bin/env python3
"""Drive the real launcher widgets through SDL keyboard/gamepad input.

Every process gets private app preferences, video config, and save roots. The
test proves first-run selection, cross-process reload, gamepad parity, visible
save failure with unchanged desired state, and successful recovery once the
configuration path becomes writable.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from harness_utils import resolve_binary


def run(executable: Path, environment: dict[str, str], expected: tuple[str, ...]) -> str:
    merged = os.environ.copy()
    merged.update(environment)
    completed = subprocess.run(
        [str(executable)], env=merged, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    if completed.returncode != 0:
        raise RuntimeError(
            f"app exited {completed.returncode}\n{completed.stdout[-6000:]}")
    for marker in expected:
        if marker not in completed.stdout:
            raise RuntimeError(
                f"missing marker {marker!r}\n{completed.stdout[-6000:]}")
    return completed.stdout


def common(root: Path, video_path: Path) -> dict[str, str]:
    prefs = root / "prefs"
    saves = root / "saves"
    prefs.mkdir(parents=True, exist_ok=True)
    saves.mkdir(parents=True, exist_ok=True)
    return {
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_VIDEO_CONFIG_PATH": str(video_path),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
        "MDKR_APP_PANEL": "Settings",
    }


def select_240(executable: Path, root: Path, input_mode: str,
               expect_failure: bool = False) -> str:
    video = root / "video.ini"
    env = common(root, video)
    env.update({
        "MDKR_APP_SMOKE_FRAMES": "24" if input_mode == "gamepad" else "18",
        "MDKR_APP_SMOKE_SELECT_FRAME_LIMIT": "240",
        "MDKR_APP_SMOKE_INPUT": input_mode,
        "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
    })
    if expect_failure:
        env["MDKR_APP_SMOKE_EXPECT_SAVE_FAILURE"] = "1"
    actual = "original" if expect_failure else "240"
    expected = [
        f"input={input_mode} frame-limit requested=240 actual={actual}",
        "surface presents=",
    ]
    if expect_failure:
        expected.append("visible settings error:")
    return run(executable, env, tuple(expected))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path, nargs="?")
    parser.add_argument("--build")
    parser.add_argument("--rom", help="accepted for tools/run_checks.py compatibility")
    args = parser.parse_args()
    if args.executable is not None:
        executable = args.executable
    elif args.build:
        executable = Path(resolve_binary(args.build))
    else:
        parser.error("provide an executable or --build")

    temporary = Path(tempfile.mkdtemp(prefix="mdkr-app-ui-"))
    try:
        keyboard = temporary / "keyboard"
        keyboard.mkdir()
        select_240(executable, keyboard, "keyboard")
        config_text = (keyboard / "video.ini").read_text(encoding="utf-8")
        if "FrameLimit=240" not in config_text:
            raise RuntimeError("keyboard widget did not persist FrameLimit=240")

        reload_env = common(keyboard, keyboard / "video.ini")
        reload_env.update({
            "MDKR_APP_SMOKE_FRAMES": "4",
            "MDKR_APP_UI_TRACE": "1",
        })
        reload_output = run(
            executable, reload_env,
            ("frame-limit value=240", "restartPending=0",
             "frame-rate-controls visible=1 gameplay-accuracy-separated=1"))
        if "frame-limit value=240" not in reload_output:
            raise RuntimeError("second process did not reload persisted 240")

        gamepad = temporary / "gamepad"
        gamepad.mkdir()
        select_240(executable, gamepad, "gamepad")

        # Persisted-scale reload matrix. Each value is read by two independent
        # processes; malformed parsing or cumulative style state cannot hide
        # behind process lifetime.
        for scale in ("0.75", "1.00", "1.50", "2.00"):
            scale_root = temporary / f"scale-{scale}"
            scale_root.mkdir()
            scale_env = common(scale_root, scale_root / "video.ini")
            (scale_root / "prefs" / "mdkr64_app.ini").write_text(
                f"# scale matrix\nui_scale={scale}\n", encoding="utf-8")
            scale_env.update({
                "MDKR_APP_SMOKE_FRAMES": "4",
                "MDKR_APP_UI_TRACE": "1",
                "MDKR_APP_SMOKE_WINDOW_SIZE": "640x480",
            })
            marker = f"ui-scale loaded={float(scale):.2f}"
            run(executable, scale_env, (marker, "surface presents="))
            run(executable, scale_env, (marker, "surface presents="))

        # Hold and move the real ImGui slider through five SDL motion frames.
        # The launcher records its applied theme scale and widget rectangle on
        # every frame: both must remain fixed until button-up, followed by one
        # application and one durable preference write.
        drag = temporary / "scale-drag"
        drag.mkdir()
        drag_env = common(drag, drag / "video.ini")
        drag_capture = drag / "scale-drag-final.bmp"
        drag_env.update({
            "MDKR_APP_SMOKE_FRAMES": "11",
            "MDKR_APP_SMOKE_UI_SCALE_DRAG": "2.00",
            "MDKR_APP_SMOKE_SHOT": str(drag_capture),
            "MDKR_APP_SMOKE_WINDOW_SIZE": "900x700",
        })
        run(executable, drag_env, (
            "ui-scale drag requested=2.00 actual=2.00 applications=1 "
            "stableWhileHeld=1 persisted=1",
            "surface presents=",
        ))
        prefs_text = (drag / "prefs" / "mdkr64_app.ini").read_text(
            encoding="utf-8")
        if "ui_scale=2.00" not in prefs_text:
            raise RuntimeError("UI-scale drag did not persist ui_scale=2.00")
        capture_bytes = drag_capture.read_bytes()
        if len(capture_bytes) < 54 or capture_bytes[:2] != b"BM":
            raise RuntimeError("UI-scale drag did not produce a valid final BMP")

        failure = temporary / "failure"
        failure.mkdir()
        missing_parent = failure / "unwritable"
        failure_env = common(failure, missing_parent / "video.ini")
        failure_env.update({
            "MDKR_APP_SMOKE_FRAMES": "22",
            "MDKR_APP_SMOKE_SELECT_FRAME_LIMIT": "240",
            "MDKR_APP_SMOKE_INPUT": "keyboard",
            "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
            "MDKR_APP_SMOKE_EXPECT_SAVE_FAILURE": "1",
            "MDKR_APP_SMOKE_RESTORE_CONFIG_DIR": str(missing_parent),
        })
        run(executable, failure_env, (
            "visible settings error:",
            "same-process Retry save click queued",
            "Retry save widget activated",
            "frame-limit requested=240 actual=240 saveFailureExpected=1",
        ))
        if "FrameLimit=240" not in (
                missing_parent / "video.ini").read_text(encoding="utf-8"):
            raise RuntimeError("same-process Retry did not persist FrameLimit=240")

        # A fresh process now checks that the same-process Retry persisted a
        # clean value and left no poisoned transient state behind.
        recovery_env = common(failure, missing_parent / "video.ini")
        recovery_env.update({
            "MDKR_APP_SMOKE_FRAMES": "4",
            "MDKR_APP_UI_TRACE": "1",
        })
        run(executable, recovery_env, (
            "frame-limit value=240", "restartPending=0",
            "frame-rate-controls visible=1 gameplay-accuracy-separated=1",
        ))
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"app UI input validation failed: {error}")
        return 1
    finally:
        shutil.rmtree(temporary, ignore_errors=True)

    print("app UI input valid: keyboard, gamepad, stable UI-scale drag, reload, same-process Retry, and recovery passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
