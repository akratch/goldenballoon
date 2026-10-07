#!/usr/bin/env python3
"""Controller focus and visible auto-scroll at the launcher's minimum size.

Asset-free; opens a real local controller room, but does not pair a phone.
Keyboard traversal supplies an independent control inventory for each closed
panel. Every one of those controls must also be visibly reached by SDL gamepad
navigation. The open room additionally requires all unpaired-room actions.
"""
import argparse
from pathlib import Path
import re
import tempfile
import zipfile

from check_a11y_shell import session, run
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

GEOMETRY = re.compile(r"\[app-focus-geometry\] visible=([01]) scroll=([\d.]+) name=(.*)")


def visible_controls(output):
    return {name for visible, _, name in GEOMETRY.findall(output) if visible == "1"}


def walk(binary, root, panel, mode, size, opened=False):
    home, env = session(root, panel + mode + ("-open" if opened else ""))
    if panel == "Content":
        mods = home / "mods"
        mods.mkdir()
        for index in range(24):
            with zipfile.ZipFile(mods / f"fixture-{index:02d}.zip", "w") as archive:
                archive.writestr("pack.ini", f"[pack]\nname=Scroll fixture {index:02d}\n")
    env.update(MDKR_APP_PANEL=panel, MDKR_APP_PARTY_TRACE="1",
               MDKR_APP_SMOKE_WINDOW_SIZE=size, MDKR_APP_SMOKE_FRAMES="800",
               MDKR_APP_SMOKE_A11Y_WALK="1", MDKR_APP_SMOKE_INPUT=mode,
               MDKR_APP_SMOKE_INPUT_TOKEN="mdkr64-app-ui-input-v1",
               MDKR_MODS_DIR=str(home / "mods"),
               MDKR_CHARACTERS_DIR=str(home / "characters"))
    if opened:
        env["MDKR_APP_SMOKE_OPEN_LOCAL"] = "mdkr64-open-local-v1"
    return run(binary, env, f"{panel} {mode} open={opened}", 180)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", help="accepted for suite compatibility; unused")
    parser.add_argument("--size", default="640x480")
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    failures = []
    required = {
        "Play": {"Start Local Play", "Choose your game file"},
        "Settings": {"Presentation", "Speech volume", "Check for updates", "D-pad down"},
        "Content": {"Install pack", "Custom content", "Open Character Workshop"},
    }
    with tempfile.TemporaryDirectory(prefix="mdkr-controller-scroll-") as tmp:
        root = Path(tmp)
        for panel in ("Play", "Settings", "Content"):
            keyboard = walk(binary, root, panel, "keyboard", args.size)
            gamepad = walk(binary, root, panel, "gamepad", args.size)
            expected = visible_controls(keyboard) | required[panel]
            actual = visible_controls(gamepad)
            missing = expected - actual
            if missing:
                failures.append(f"{panel}: controller never visibly reached {sorted(missing)}")
            if not any(float(scroll) > 0 for visible, scroll, _ in GEOMETRY.findall(gamepad)
                       if visible == "1"):
                failures.append(f"{panel}: no visible focused control after scrolling")
            if not missing:
                print(f"PASS: {panel} controller visibly reaches {len(expected)} controls at {args.size}", flush=True)
        opened = walk(binary, root, "Play", "gamepad", args.size, opened=True)
        missing = {"Copy Invite Link", "Close Invite", "Stop Local Play"} - visible_controls(opened)
        if missing:
            failures.append(f"open Local Play: controller never visibly reached {sorted(missing)}")
        else:
            print(f"PASS: open Local Play invite and stop controls visible at {args.size}", flush=True)
    if failures:
        raise SystemExit("\n".join(failures))


if __name__ == "__main__":
    main()
