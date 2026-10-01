#!/usr/bin/env python3
"""Back out of a screen, press Tab: focus must reach a control that speaks.

Dear ImGui remembers keyboard focus as the id of the focused control. When the
shell stops drawing that control while its window stays open, the id names a
control that no longer exists. With the highlight still showing, a forward
Tab looks for that control to move past, never finds it, and does nothing.
Shift+Tab, the arrows and the D-pad still move, so a sighted tester rarely
notices; a keyboard or screen-reader player is left on a menu that will not
Tab forward.

The in-game menu had three such screens, all exercised here through its real
SDL input path (MDKR_TEST_OVERLAY_NAV_SCRIPT in ui_overlay.cpp):

  * a confirmation (Return to Launcher, Quit to Desktop): Escape, controller B
    or Cancel closes it with focus on one of its buttons, and opening one from
    Quit to Desktop leaves focus on the button that opened it;
  * the Settings list: Escape or B with focus inside it closes the list, and
    Dear ImGui's own "leave the child window" step points focus at the list
    itself;
  * the Settings button, whose id came from its label, so pressing it ("Settings"
    becomes "Hide Settings", and back) left focus on the label it no longer had.

Each overlay arm enters one of them the way a player would, backs out, then
presses Tab and requires focus to land on a control drawn that frame, with the
highlight showing, and the shell's voice to announce it. Every arm also checks
that its earlier steps reached the screen they claim to, so a layout change
fails here by name instead of quietly testing something else.

The launcher arms walk the Settings and About panels by Tab, press an Escape
that nothing on those panels claims, and keep tabbing. Tab must carry on from
the control that had focus: neither stop nor start over at the first stop.
Reverting AppNavigation_configure's Escape setting fails them either way, with
or without the end-of-frame focus recovery.

Audio safety: every run sets MDKR_AUDIO=0 and is bounded by a frame or tick
budget. Speech is traced as text; nothing is routed to a device.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from check_a11y_shell import GateFailure, run, session, utterances
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

INPUT_TOKEN = "mdkr64-app-ui-input-v1"
ESCAPE_TOKEN = "mdkr64-a11y-escape-v1"

# The menu opens on an Escape at this authoritative tick; the script then runs
# in rendered frames and closes it. The budget leaves ample room after that.
OVERLAY_OPEN_TICK = 120
OVERLAY_TICKS = 600

STEP_RE = re.compile(r"^\[overlay-nav\] step=(\d+) key=(\S+)$")
SETTLED_RE = re.compile(
    r"^\[overlay-nav\] settled step=(\d+) focus=(\w+) cursor=([01]) "
    r"open=([01]) confirm=([01]) settings=([01]) input=(\w+)$")
SPEAK_LINE_RE = re.compile(r"^\[SPEAK\] cat=(\S+) pri=\S+ text=(.*)$")
ESCAPE_REACHED_RE = re.compile(
    r"^\[app-a11y-walk\] escape reached imgui frame=\d+$", re.MULTILINE)


@dataclass
class Step:
    key: str
    focus: list[str] = field(default_factory=list)    # cat=focus texts
    spoken: list[str] = field(default_factory=list)   # every [SPEAK] text
    settled: dict[str, str] | None = None


def overlay_steps(output: str) -> tuple[list[Step], bool]:
    """Each scripted step with what was said during it and where it settled."""
    steps: list[Step] = []
    current: Step | None = None
    complete = False
    for line in output.splitlines():
        if (match := STEP_RE.match(line)):
            if int(match.group(1)) != len(steps):
                raise GateFailure(f"overlay steps out of order at {line!r}")
            current = Step(match.group(2))
            steps.append(current)
        elif (match := SETTLED_RE.match(line)):
            if current is None or int(match.group(1)) != len(steps) - 1:
                raise GateFailure(f"settled line without its step: {line!r}")
            current.settled = dict(zip(
                ("focus", "cursor", "open", "confirm", "settings", "input"),
                match.groups()[1:]))
        elif (match := SPEAK_LINE_RE.match(line)) and current is not None \
                and current.settled is None:
            current.spoken.append(match.group(2))
            if match.group(1) == "focus":
                current.focus.append(match.group(2))
        elif line.startswith("[overlay-nav] script complete"):
            complete = True
    return steps, complete


# What an arm expects of the steps before its final Tab: ("heard", step,
# prefix) -- a focus announcement starting with prefix; ("spoke", step, "") --
# any announcement at all; ("state", step, "key=value ...") -- settled values.
Expectation = tuple[str, int, str]


@dataclass(frozen=True)
class OverlayArm:
    name: str
    script: tuple[str, ...]
    expect: tuple[Expectation, ...]


# The menu opens with focus on Resume and the highlight hidden, so the first
# Tab only shows the highlight there; Shift+Tab from Resume wraps to the last
# button. Both are Dear ImGui's behaviour, and the expectations pin them.
_RETURN = ("shift-tab", "shift-tab", "enter", "tab")
_RETURN_EXPECT = (("heard", 1, "Return to Launcher."),
                  ("state", 2, "confirm=1"),
                  ("heard", 3, "Cancel."))
_LIST = ("tab", "tab", "enter", "down", "enter")
_LIST_EXPECT = (("heard", 1, "Settings."),
                ("state", 2, "settings=1"),
                ("heard", 3, "Settings list."),
                ("spoke", 4, ""),
                ("state", 4, "focus=drawn settings=1"))

OVERLAY_ARMS = (
    OverlayArm("confirmation closed by Escape", _RETURN + ("escape", "tab"),
               _RETURN_EXPECT + (("state", 4, "confirm=0 open=1"),)),
    OverlayArm("confirmation closed by controller B",
               _RETURN + ("pad-b", "tab"),
               _RETURN_EXPECT + (("state", 4, "confirm=0 open=1 input=gamepad"),)),
    OverlayArm("confirmation closed by Enter on Cancel",
               _RETURN + ("enter", "tab"),
               _RETURN_EXPECT + (("state", 4, "confirm=0 open=1"),)),
    OverlayArm("confirmation opened from Quit to Desktop",
               ("shift-tab", "enter", "tab"),
               (("heard", 0, "Quit to Desktop."),
                ("state", 1, "confirm=1"),
                ("state", 2, "confirm=1"))),
    OverlayArm("Settings list closed by Escape", _LIST + ("escape", "tab"),
               _LIST_EXPECT + (("state", 5, "settings=0 open=1"),)),
    OverlayArm("Settings list closed by controller B",
               _LIST + ("pad-b", "tab"),
               _LIST_EXPECT + (("state", 5, "settings=0 open=1 input=gamepad"),)),
    OverlayArm("Settings button opening the list", ("tab", "tab", "enter", "tab"),
               (("heard", 1, "Settings."), ("state", 2, "settings=1"))),
    OverlayArm("Settings button closing the list",
               ("tab", "tab", "enter", "left", "right", "enter", "tab"),
               (("heard", 1, "Settings."),
                ("state", 2, "settings=1"),
                ("heard", 4, "Hide Settings."),
                ("state", 5, "settings=0"))),
)


def walk_overlay(executable: Path, root: Path, rom: Path, arm: OverlayArm,
                 index: int, timeout: int) -> str:
    _home, env = session(root, f"overlay-{index}")
    env.update({
        "MDKR_APP_AUTOPLAY": "1",
        "MDKR_APP_AUTOPLAY_TICKS": str(OVERLAY_TICKS),
        "MDKR_ROM": str(rom),
        "MDKR_TEST_OVERLAY_ESCAPE_OPEN_FRAME": str(OVERLAY_OPEN_TICK),
        "MDKR_TEST_OVERLAY_NAV_SCRIPT": ",".join(arm.script),
    })
    return run(executable, env, f"overlay arm '{arm.name}'", timeout)


def describe(steps: list[Step]) -> str:
    lines = []
    for index, step in enumerate(steps):
        said = "; ".join(text[:50] for text in step.spoken) or "(silent)"
        lines.append(f"  {index} {step.key:<9} {step.settled} {said}")
    return "\n".join(lines)


def check_overlay_arm(arm: OverlayArm, output: str) -> str:
    steps, complete = overlay_steps(output)
    if "[overlay-nav] script armed" not in output:
        raise GateFailure(
            f"{arm.name}: the overlay never armed its navigation script\n"
            + output[-2000:])
    if not complete or len(steps) != len(arm.script) or any(
            step.settled is None for step in steps):
        raise GateFailure(
            f"{arm.name}: the script did not run to the end "
            f"({len(steps)} of {len(arm.script)} steps)\n{describe(steps)}")
    for kind, index, detail in arm.expect:
        step = steps[index]
        assert step.settled is not None
        if kind == "heard":
            ok = any(text.startswith(detail) for text in step.focus)
            what = f"announce {detail!r}"
        elif kind == "spoke":
            ok = bool(step.spoken)
            what = "announce anything"
        else:
            wanted = dict(pair.split("=") for pair in detail.split())
            ok = all(step.settled[key] == value for key, value in wanted.items())
            what = f"settle at {detail}"
        if not ok:
            raise GateFailure(
                f"{arm.name}: step {index} ({step.key}) did not {what}, so the "
                f"arm never reached the screen it tests\n{describe(steps)}")

    probe = steps[-1]
    assert probe.settled is not None
    if probe.settled["focus"] != "drawn" or probe.settled["cursor"] != "1":
        raise GateFailure(
            f"{arm.name}: the final Tab left focus "
            f"focus={probe.settled['focus']} cursor={probe.settled['cursor']} "
            "-- not on a control that is on screen, so Tab cannot move on from "
            f"it\n{describe(steps)}")
    if not probe.focus:
        raise GateFailure(
            f"{arm.name}: the final Tab reached a control that said "
            f"nothing\n{describe(steps)}")
    return probe.focus[0].split(".")[0]


@dataclass(frozen=True)
class LauncherArm:
    panel: str
    tabs_before_escape: int
    frames: int


# Few enough Tabs that the Escape lands before the walk has visited every stop
# once, so "carried on" and "started over" announce different controls.
LAUNCHER_ARMS = (
    LauncherArm("About", 5, 40),
    LauncherArm("Settings", 12, 60),
)


def walk_launcher(executable: Path, root: Path, arm: LauncherArm,
                  timeout: int) -> str:
    _home, env = session(root, f"launcher-{arm.panel}")
    env.update({
        "MDKR_APP_PANEL": arm.panel,
        "MDKR_APP_SMOKE_FRAMES": str(arm.frames),
        "MDKR_APP_SMOKE_A11Y_WALK": "1",
        "MDKR_APP_SMOKE_INPUT": "keyboard",
        "MDKR_APP_SMOKE_INPUT_TOKEN": INPUT_TOKEN,
        "MDKR_APP_SMOKE_A11Y_ESCAPE": str(arm.tabs_before_escape),
        "MDKR_APP_SMOKE_A11Y_ESCAPE_TOKEN": ESCAPE_TOKEN,
    })
    return run(executable, env, f"{arm.panel} Escape walk", timeout)


def check_launcher_arm(arm: LauncherArm, output: str) -> str:
    reached = ESCAPE_REACHED_RE.search(output)
    if reached is None:
        raise GateFailure(
            f"{arm.panel}: the walk never reported its Escape reaching the "
            f"launcher\n{output[-3000:]}")
    before = [text for category, _p, text in utterances(output[:reached.start()])
              if category == "focus"]
    after = [text for category, _p, text in utterances(output[reached.end():])
             if category == "focus"]
    if not before:
        raise GateFailure(
            f"{arm.panel}: nothing had focus when the Escape arrived, so the "
            "arm proves nothing")
    if len(set(after)) < 3:
        raise GateFailure(
            f"{arm.panel}: Tab stopped moving after an Escape nothing claimed: "
            f"{len(set(after))} controls announced after it (last before it: "
            f"{before[-1][:60]!r})")
    if after[0] in before:
        raise GateFailure(
            f"{arm.panel}: the first Tab after Escape announced "
            f"{after[0][:60]!r}, a control the walk had already passed. Tab "
            f"started over instead of carrying on from {before[-1][:60]!r}")
    return (f"{arm.panel}: {before[-1].split('.')[0]} -> Escape -> "
            f"{after[0].split('.')[0]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64", type=Path)
    parser.add_argument("--launcher-only", action="store_true",
                        help="run only the ROM-free launcher arms")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()

    executable = Path(resolve_binary(args.build))
    if not executable.is_file():
        parser.error(f"missing executable: {executable}")
    rom = args.rom.expanduser()
    if not args.launcher_only and not rom.is_file():
        parser.error(f"missing ROM: {rom} (or pass --launcher-only)")

    results: list[str] = []
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr64_dead_ends_") as temporary:
        root = Path(temporary)
        arms: list[tuple[str, object]] = [
            ("launcher", arm) for arm in LAUNCHER_ARMS]
        if not args.launcher_only:
            arms += [("overlay", arm) for arm in OVERLAY_ARMS]
        for index, (kind, arm) in enumerate(arms):
            try:
                if kind == "launcher":
                    assert isinstance(arm, LauncherArm)
                    results.append(check_launcher_arm(
                        arm, walk_launcher(executable, root, arm,
                                           args.timeout)))
                else:
                    assert isinstance(arm, OverlayArm)
                    landed = check_overlay_arm(arm, walk_overlay(
                        executable, root, rom, arm, index, args.timeout))
                    results.append(f"{arm.name}: Tab -> {landed}")
            except GateFailure as failure:
                failures.append(str(failure))
            except subprocess.TimeoutExpired as expired:
                failures.append(f"{kind} arm timed out: {expired}")

    for line in results:
        print(f"  ok  {line}")
    if failures:
        for failure in failures:
            print(f"FAIL keyboard dead ends: {failure}", file=sys.stderr)
        return 1
    print(f"PASS keyboard dead ends: {len(results)} arms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
