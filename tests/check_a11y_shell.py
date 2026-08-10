#!/usr/bin/env python3
"""Every control the shell offers must say what it is and what it is set to.

ImGui draws to a canvas and publishes no accessibility tree, so the app speaks
for itself: platform/app/ui_common.cpp announces the focused row, and
platform/a11y_model.c turns that into one `[SPEAK]` line per utterance under
MDKR_A11Y_TRACE=1. This gate walks the real UI by keyboard and reads them.

What makes it worth having is where the expectation comes from. The list of
controls is NOT written here -- it is enumerated from the app's own schema dump
(`Settings_dumpSchemaContract`), which reports one row per setting with the
label and value the shell would speak. A setting added tomorrow therefore joins
this gate's requirements the moment it has a schema entry, and stays failing
until something voices it. Keeping the list in the test instead would mean the
gate could only ever catch controls somebody had already thought about.

Three surfaces, because the announcement has to survive all three:

  * the launcher's Settings panel, tabbed from top to bottom, which carries the
    coverage claim -- every visible setting, its name and its current value;
  * each of the other launcher panels, which must announce themselves on
    arrival;
  * the in-game overlay over a running engine session, which draws the same
    rows through the same helper and must speak them there too.

Audio safety: every run sets MDKR_AUDIO=0 and is bounded by an explicit frame
or tick budget. Speech is never routed to a device here; the gate reads text.
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

# One row per setting, produced by Settings_dumpSchemaContract().
CONTROL_RE = re.compile(
    r'^\[app-a11y\] control key=(\S+) visible=([01]) label="([^"]*)" '
    r'value="([^"]*)"$', re.MULTILINE)
SPEAK_RE = re.compile(r"^\[SPEAK\] cat=(\S+) pri=(\S+) text=(.*)$", re.MULTILINE)
TAB_PHASE_RE = re.compile(r"^\[app-a11y-walk\] tab phase complete frame=(\d+)$",
                          re.MULTILINE)

# One Tab per rendered frame. A complete pass over the settings panel takes
# about 140; the budget leaves room for the panel to grow before the walk stops
# reaching the bottom of it, which would otherwise report new controls as silent
# when they are merely unreached.
WALK_FRAMES = 220
PANEL_FRAMES = 6
# The overlay arm needs a live engine session. It opens early and closes well
# before the budget so the run still proves the game came back.
OVERLAY_TICKS = 700
OVERLAY_OPEN_TICK = 120
OVERLAY_CLOSE_TICK = 620

INPUT_TOKEN = "mdkr64-app-ui-input-v1"


class GateFailure(RuntimeError):
    """A failure whose message is the finding, not a stack trace."""


def clean_environment(**updates: str) -> dict[str, str]:
    """A process that inherits none of the caller's MDKR configuration."""
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(updates)
    return env


def session(root: Path, name: str) -> tuple[Path, dict[str, str]]:
    """A private prefs/config/save root, with speech switched on in the file.

    Speech is enabled through the settings FILE rather than the environment on
    purpose: an environment variable pins the key above runtime precedence, and
    the settings panel correctly draws a pinned key disabled -- which would take
    Accessibility.Speech itself out of the keyboard walk and make the gate
    silently stop checking the one control the sprint is about.
    """
    home = root / name
    (home / "prefs").mkdir(parents=True, exist_ok=True)
    (home / "saves").mkdir(parents=True, exist_ok=True)
    config = home / "video.ini"
    config.write_text("[Accessibility]\nSpeech=1\n", encoding="utf-8")
    return home, clean_environment(
        LC_ALL="C",
        MDKR_APP_PREFS_DIR=str(home / "prefs"),
        MDKR_VIDEO_CONFIG_PATH=str(config),
        MDKR_SAVE_DIR=str(home / "saves"),
        MDKR_AUDIO="0",
        MDKR_A11Y_TRACE="1",
        MDKR64_HIDDEN="1",
    )


def run(executable: Path, env: dict[str, str], label: str,
        timeout: int) -> str:
    completed = subprocess.run(
        [str(executable)], env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False)
    output = completed.stdout or ""
    if completed.returncode != 0:
        raise GateFailure(
            f"{label} exited {completed.returncode}\n{output[-4000:]}")
    return output


def utterances(output: str) -> list[tuple[str, str, str]]:
    return [(category, priority, text)
            for category, priority, text in SPEAK_RE.findall(output)]


def inventory(executable: Path, root: Path,
              timeout: int) -> list[tuple[str, str, str]]:
    """(key, label, value) for every control the product offers.

    Read from the app, not from this file. The dump runs windowless and
    non-interactively against the same settings file the walk will use, so the
    values it reports are the values the walk will be looking at.
    """
    _home, env = session(root, "schema")
    env["MDKR_APP_DUMP_SCHEMA"] = "1"
    output = run(executable, env, "schema dump", timeout)
    rows = CONTROL_RE.findall(output)
    if not rows:
        raise GateFailure(
            "the app printed no control inventory; "
            "Settings_dumpSchemaContract() no longer enumerates the schema\n"
            + output[-2000:])
    visible = [(key, label, value)
               for key, flag, label, value in rows if flag == "1"]
    if not visible:
        raise GateFailure(
            f"the app reports {len(rows)} settings and not one of them "
            "visible; there would be nothing for a player to reach")
    return visible


def walk_launcher(executable: Path, root: Path, timeout: int) -> str:
    _home, env = session(root, "walk")
    env.update({
        "MDKR_APP_PANEL": "Settings",
        "MDKR_APP_SMOKE_FRAMES": str(WALK_FRAMES),
        "MDKR_APP_SMOKE_A11Y_WALK": "1",
        "MDKR_APP_SMOKE_INPUT": "keyboard",
        "MDKR_APP_SMOKE_INPUT_TOKEN": INPUT_TOKEN,
    })
    return run(executable, env, "launcher keyboard walk", timeout)


def walk_panel(executable: Path, root: Path, panel: str, timeout: int) -> str:
    _home, env = session(root, f"panel-{panel}")
    env.update({
        "MDKR_APP_PANEL": panel,
        "MDKR_APP_SMOKE_FRAMES": str(PANEL_FRAMES),
        "MDKR_APP_SMOKE_A11Y_WALK": "1",
        "MDKR_APP_SMOKE_INPUT": "keyboard",
        "MDKR_APP_SMOKE_INPUT_TOKEN": INPUT_TOKEN,
    })
    return run(executable, env, f"{panel} panel walk", timeout)


def walk_overlay(executable: Path, root: Path, rom: Path, timeout: int) -> str:
    _home, env = session(root, "overlay")
    env.update({
        "MDKR_APP_AUTOPLAY": "1",
        "MDKR_APP_AUTOPLAY_TICKS": str(OVERLAY_TICKS),
        "MDKR_ROM": str(rom),
        "MDKR_TEST_OVERLAY_ESCAPE_OPEN_FRAME": str(OVERLAY_OPEN_TICK),
        "MDKR_TEST_OVERLAY_ESCAPE_CLOSE_FRAME": str(OVERLAY_CLOSE_TICK),
        "MDKR_TEST_OVERLAY_A11Y_WALK": "1",
    })
    return run(executable, env, "in-game overlay walk", timeout)


def check_launcher(output: str, controls: list[tuple[str, str, str]]) -> str:
    spoken = utterances(output)
    if not spoken:
        raise GateFailure(
            "the launcher walk collected zero [SPEAK] lines. Either nothing "
            "announces the focused control, or the trace is off -- both mean "
            "a blind player is looking at a silent app.")

    focus = [text for category, _priority, text in spoken if category == "focus"]
    sections = [text for category, _priority, text in spoken
                if category == "section"]

    # Every control, by name AND by the value it is currently on. A row that
    # announces its name but not its setting tells a player where they are and
    # not what they would be changing.
    silent: list[str] = []
    valueless: list[str] = []
    for key, label, value in controls:
        named = [text for text in focus if text.startswith(label)]
        if not named:
            silent.append(f"{key} (\"{label}\")")
        elif value and not any(value in text for text in named):
            valueless.append(f'{key} ("{label}") never said its value "{value}"')
    if silent:
        raise GateFailure(
            f"{len(silent)} of {len(controls)} controls produced no "
            "utterance -- a player tabbing onto them hears nothing:\n  "
            + "\n  ".join(silent))
    if valueless:
        raise GateFailure(
            "controls announced their name without their current setting:\n  "
            + "\n  ".join(valueless))

    if not sections:
        raise GateFailure(
            "moving between sections produced no cat=section utterance, so "
            "there is no way to tell where in the menu you have arrived")

    # The arrow keys are the other way a player moves. The walk prints the
    # boundary; utterances have to keep coming after it.
    boundary = TAB_PHASE_RE.search(output)
    if boundary is None:
        raise GateFailure(
            "the walk never reported finishing its Tab phase, so the arrow-key "
            "half of it did not run")
    after = utterances(output[boundary.end():])
    if not after:
        raise GateFailure(
            "the arrow-key phase produced no utterance: the rows answer to Tab "
            "and not to the arrow keys")

    return (f"{len(controls)} controls, {len(focus)} focus utterances, "
            f"{len(sections)} section utterances, "
            f"{len(after)} of them after the arrow-key phase began")


def check_panels(executable: Path, root: Path, timeout: int) -> str:
    """Each launcher panel announces itself on arrival."""
    announced = []
    for panel in ("Game ROM", "Diagnostics", "About"):
        output = walk_panel(executable, root, panel, timeout)
        sections = [text for category, _priority, text in utterances(output)
                    if category == "section"]
        if not any(text.startswith(panel) for text in sections):
            raise GateFailure(
                f"opening the {panel!r} panel produced no cat=section "
                f"utterance naming it; got {sections}")
        announced.append(panel)
    return ", ".join(announced)


def check_overlay(output: str, controls: list[tuple[str, str, str]]) -> str:
    for marker in ("[overlay-test] Escape handled; overlay=open",
                   "[overlay-test] Escape handled; overlay=closed"):
        if marker not in output:
            raise GateFailure(
                f"the overlay run never reported {marker!r}\n{output[-3000:]}")
    spoken = utterances(output)
    if not spoken:
        raise GateFailure(
            "the in-game overlay collected zero [SPEAK] lines: the shell goes "
            "quiet the moment a race starts, which is where a player needs it")
    focus = [text for category, _priority, text in spoken if category == "focus"]
    reached = [key for key, label, _value in controls
               if any(text.startswith(label) for text in focus)]
    if not reached:
        raise GateFailure(
            "the overlay spoke, but never announced a settings row. Its rows "
            "come from the same drawKey() the launcher uses, so a silent "
            "overlay means the announcement is not where it claims to be.\n"
            f"utterances: {[text[:60] for _c, _p, text in spoken][:10]}")
    return f"{len(focus)} focus utterances over {len(reached)} settings rows"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64", type=Path)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    executable = Path(resolve_binary(args.build))
    if not executable.is_file():
        parser.error(f"missing executable: {executable}")
    rom = args.rom.expanduser()
    if not rom.is_file():
        parser.error(f"missing ROM: {rom}")

    with tempfile.TemporaryDirectory(prefix="mdkr64_a11y_shell_") as temporary:
        root = Path(temporary)
        try:
            controls = inventory(executable, root, args.timeout)
            launcher = check_launcher(
                walk_launcher(executable, root, args.timeout), controls)
            panels = check_panels(executable, root, args.timeout)
            overlay = check_overlay(
                walk_overlay(executable, root, rom, args.timeout), controls)
        except GateFailure as failure:
            print(f"FAIL a11y shell: {failure}", file=sys.stderr)
            return 1
        except subprocess.TimeoutExpired as expired:
            print(f"FAIL a11y shell: {expired}", file=sys.stderr)
            return 1

    print(f"PASS a11y shell: {launcher}; panels announced: {panels}; "
          f"in-game overlay: {overlay}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
