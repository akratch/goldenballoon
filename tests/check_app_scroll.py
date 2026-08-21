#!/usr/bin/env python3
"""Drive the real launcher's scroll paths through SDL wheel and touch input.

The launcher renders one long, scrolling panel per page. Two ways a player
reaches the bottom of one have to work and, until now, neither was exercised by
this suite:

* Mouse wheel / MacBook trackpad. A two-finger trackpad scroll on macOS arrives
  as SDL_MOUSEWHEEL events whose *integer* ``y`` is zero and whose motion lives
  entirely in the fractional ``preciseY`` field. A wheel bridge that reads the
  integer field scrolls nothing -- "scrolling breaks completely" on a trackpad.
  This check drives exactly that event shape (``preciseY`` fractional, integer
  ``y`` truncated to zero) through the production
  ``AppHost::processEvent`` -> ``ImGui_ImplSDL2_ProcessEvent`` adapter and
  asserts the panel's ``ScrollY`` actually advances.

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

        touch = temporary / "touch"
        touch.mkdir()
        check_touch(executable, touch)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"app scroll validation failed: {error}")
        return 1
    finally:
        shutil.rmtree(temporary, ignore_errors=True)

    print("app scroll valid: trackpad-shaped fractional wheel and touchscreen "
          "drag both advance the launcher panel's ScrollY")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
