#!/usr/bin/env python3
"""Drive the real launcher's scroll paths through SDL wheel and touch input.

The launcher renders one long, scrolling panel per page. Two ways a player
reaches the bottom of one have to work and, until now, neither was exercised by
this suite:

* Mouse wheel / MacBook trackpad. A two-finger trackpad scroll on macOS arrives
  as SDL_MOUSEWHEEL events whose *integer* ``y`` is zero and whose motion lives
  entirely in the fractional ``preciseY`` field. A wheel bridge that reads the
  integer field scrolls nothing. This check drives exactly that event shape
  (``preciseY`` fractional, integer ``y`` truncated to zero) through the
  production ``AppHost::processEvent`` -> ``ImGui_ImplSDL2_ProcessEvent`` adapter
  and asserts the panel's ``ScrollY`` actually advances.

* macOS natural-scroll DIRECTION. SDL marks the event ``SDL_MOUSEWHEEL_FLIPPED``
  after macOS has applied the user's preference. The bridge must not reverse
  that already-adjusted delta again.

* Touchscreen drag. A finger drag that begins on non-interactive content pans
  the same panel through the custom ``TouchScrollCurrentWindow`` gesture.

Both travel the real SDL event loop the interactive app uses, not a standalone
ImGui harness, so a regression in the app's own wheel/touch integration -- the
only place these bugs can live -- fails the check. Each run owns private app
preferences, video config, and save roots so a repo-root config can never reach
it (the harness-isolation contract).
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
    save_directory = environment.get("MDKR_SAVE_DIR")
    if not save_directory or not Path(save_directory).is_absolute():
        raise ValueError("scroll fixture requires an explicit absolute MDKR_SAVE_DIR")
    merged = {key: value for key, value in os.environ.items()
              if not key.startswith(("MDKR", "GE007_"))}
    merged.update(environment)
    # Make the launch boundary fail closed independently of its caller and
    # keep the pin visible to the intraprocedural hermeticity source gate.
    merged["MDKR_SAVE_DIR"] = save_directory
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
        # Pinned per the harness-isolation contract: the scroll run writes its
        # own settings here, never a repo-root mdkr64.ini.
        "MDKR_VIDEO_CONFIG_PATH": str(video_path),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
        # Settings is the page that reliably overflows at 1280x720, so both
        # gestures have somewhere to scroll.
        "MDKR_APP_PANEL": "Settings",
    }


def check_wheel(executable: Path, root: Path) -> None:
    """A fractional-preciseY (trackpad-shaped) wheel advances the panel."""
    env = common(root, root / "video.ini")
    env.update({
        "MDKR_APP_SMOKE_FRAMES": "12",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "1280x720",
        "MDKR_APP_SMOKE_WHEEL_SCROLL": "1",
        "MDKR_APP_SMOKE_WHEEL_TOKEN": "mdkr64-app-wheel-v1",
    })
    # scroll=1 is only printed when the panel's ScrollY advanced past the
    # threshold under wheel events whose integer y is zero -- i.e. the bridge
    # read preciseY, not the rounded field.
    run(executable, env, ("[app-ui-test] wheel handheld scroll=1",))


def check_wheel_flipped(executable: Path, root: Path) -> None:
    """A macOS natural-scroll (FLIPPED) wheel scrolls the RIGHT way.

    The flag is INFORMATION, not an instruction to negate. SDL2's
    src/video/cocoa/SDL_cocoamouse.m sets SDL_MOUSEWHEEL_FLIPPED from
    -[NSEvent isDirectionInvertedFromDevice] and passes `[event deltaY]`
    through untouched, because macOS has already applied the player's
    natural-scrolling preference to that delta. Dear ImGui's SDL2 backend
    ignores `direction` for the same reason. So this run injects the SAME sign
    as the normal-wheel run and requires the SAME downward result: a bridge that
    negates on the flag scrolls up, clamps at the top, and stays red.

    """
    env = common(root, root / "video.ini")
    env.update({
        "MDKR_APP_SMOKE_FRAMES": "12",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "1280x720",
        "MDKR_APP_SMOKE_WHEEL_SCROLL": "1",
        "MDKR_APP_SMOKE_WHEEL_TOKEN": "mdkr64-app-wheel-v1",
        "MDKR_APP_SMOKE_WHEEL_FLIPPED": "1",
    })
    run(executable, env, ("[app-ui-test] wheel handheld scroll=1",))


def check_touch(executable: Path, root: Path) -> None:
    """A touchscreen drag on empty content advances the same panel."""
    env = common(root, root / "video.ini")
    env.update({
        "MDKR_APP_SMOKE_FRAMES": "10",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "1280x720",
        "MDKR_APP_SMOKE_TOUCH_SCROLL": "1",
        "MDKR_APP_SMOKE_TOUCH_TOKEN": "mdkr64-app-touch-v1",
    })
    run(executable, env, ("[app-ui-test] touch handheld targets=1 scroll=1",))


def check_phone_panel(executable: Path, root: Path, touch: bool) -> None:
    """Exercise the real phone surface's parent scroll region at minimum size.

    The existing party trace relaxes only ROM readiness; it does not fabricate
    transport availability or open a network room. This does not cover a full
    connected-phone roster or keyboard/gamepad focus scrolling.
    """
    env = common(root, root / "video.ini")
    env.update(MDKR_APP_PANEL="Play", MDKR_APP_PARTY_TRACE="1",
               MDKR_APP_SMOKE_WINDOW_SIZE="640x480", MDKR_APP_SMOKE_FRAMES="12")
    if touch:
        env.update(MDKR_APP_SMOKE_TOUCH_SCROLL="1",
                   MDKR_APP_SMOKE_TOUCH_TOKEN="mdkr64-app-touch-v1")
        marker = "[app-ui-test] touch handheld targets=1 scroll=1"
    else:
        env.update(MDKR_APP_SMOKE_WHEEL_SCROLL="1",
                   MDKR_APP_SMOKE_WHEEL_TOKEN="mdkr64-app-wheel-v1")
        marker = "[app-ui-test] wheel handheld scroll=1"
    run(executable, env, ("[app] party:", "phase=closed", marker))


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

    temporary = Path(tempfile.mkdtemp(prefix="mdkr-app-scroll-"))
    try:
        wheel = temporary / "wheel"
        wheel.mkdir()
        check_wheel(executable, wheel)

        wheel_flipped = temporary / "wheel-flipped"
        wheel_flipped.mkdir()
        check_wheel_flipped(executable, wheel_flipped)

        touch = temporary / "touch"
        touch.mkdir()
        check_touch(executable, touch)

        for touch_input in (False, True):
            phone = temporary / ("phone-touch" if touch_input else "phone-wheel")
            phone.mkdir()
            check_phone_panel(executable, phone, touch_input)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"app scroll validation failed: {error}")
        return 1
    finally:
        shutil.rmtree(temporary, ignore_errors=True)

    print("app scroll valid: trackpad-shaped fractional wheel (normal and "
          "macOS natural-scroll FLIPPED) and touchscreen drag all advance the "
          "launcher panel's ScrollY in the right direction; closed phone panel "
          "also scrolls with wheel and touch at 640x480")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
