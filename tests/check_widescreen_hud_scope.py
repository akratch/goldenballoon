#!/usr/bin/env python3
"""Confine the opt-in widescreen HUD offset to the HUD (issue #50).

The widescreen HUD (Video.WidescreenHUD, off by default) edge-anchors the race
counters and minimap by swapping in a left-anchored, presentation-wide ortho
(WIDE_HUD draw space) and by shifting each opted-in HUD element.  Two ways that
scope leaked into elements that must stay centered:

  * the race banana counter's "x" glyph (HUD_BANANA_COUNT_X) was omitted from
    the anchor group that carries the rest of the counter, so it drifted out of
    its own group and collided with the lap counter; and

  * the WIDE_HUD ortho persists in the game display list, so the dialogue
    boxes, pause menu and Taj subtitles drawn after the HUD inherited it and
    slid off-centre (box background left of its centered text).

This is a regex-over-source gate (the check_ci_contract.py / party origin-gate
house style), so it needs neither the ROM nor the GPU lane and stays green in
the unit-test tier.  It asserts:

  1. every member of the race banana counter shares the lap counter's anchor
     bucket in hud_widescreen_anchor(); and
  2. the standard centered ortho is restored at the HUD -> dialogue-box
     boundary, gated so the widescreen-HUD-off default path is untouched.

A self-test replays both assertions against the pre-fix source to prove they
actually fail red, matching the origin-gate's self-test discipline.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME_UI = ROOT / "game" / "src" / "game_ui.c"
THREAD3 = ROOT / "game" / "src" / "thread3_main.c"

# The race banana counter and the lap counter sit stacked at the top-right and
# must ride the same right edge anchor; if any counter glyph is left behind it
# separates from its group and overlaps the neighbour.
BANANA_COUNTER = (
    "HUD_BANANA_COUNT_ICON_SPIN",
    "HUD_BANANA_COUNT_X",
    "HUD_BANANA_COUNT_NUMBER_1",
    "HUD_BANANA_COUNT_NUMBER_2",
    "HUD_BANANA_COUNT_ICON_STATIC",
    "HUD_BANANA_COUNT_SPARKLE",
)
LAP_COUNTER = (
    "HUD_LAP_COUNT_LABEL",
    "HUD_LAP_COUNT_CURRENT",
    "HUD_LAP_COUNT_SEPERATOR",
    "HUD_LAP_COUNT_TOTAL",
)

ANCHOR_FN_RE = re.compile(
    r"hud_widescreen_anchor\s*\([^)]*\)\s*\{(?P<body>.*?)\n\}",
    re.DOTALL,
)
CASE_RE = re.compile(r"case\s+(HUD_[A-Z0-9_]+)\s*:")
RETURN_RE = re.compile(r"return\s+(MDKR_HUD_ANCHOR_[A-Z]+)\s*;")


def anchor_of(body: str) -> dict[str, str]:
    """Map each cased HUD element to the anchor its fallthrough returns."""
    anchors: dict[str, str] = {}
    pending: list[str] = []
    for line in body.splitlines():
        case = CASE_RE.search(line)
        if case:
            pending.append(case.group(1))
            continue
        ret = RETURN_RE.search(line)
        if ret:
            for element in pending:
                anchors[element] = ret.group(1)
            pending = []
    return anchors


def check_banana_anchor(game_ui: str) -> list[str]:
    problems: list[str] = []
    match = ANCHOR_FN_RE.search(game_ui)
    if not match:
        return ["hud_widescreen_anchor() not found in game_ui.c"]
    anchors = anchor_of(match.group("body"))

    lap_anchors = {anchors.get(name, "MDKR_HUD_ANCHOR_CENTER")
                   for name in LAP_COUNTER}
    if lap_anchors != {"MDKR_HUD_ANCHOR_RIGHT"}:
        problems.append(
            f"lap counter must be a single right anchor, got {sorted(lap_anchors)}")

    for name in BANANA_COUNTER:
        got = anchors.get(name, "MDKR_HUD_ANCHOR_CENTER")
        if got != "MDKR_HUD_ANCHOR_RIGHT":
            problems.append(
                f"{name} must ride the right anchor with its counter group, "
                f"got {got}")
    return problems


def check_ortho_restore(game_ui: str, thread3: str) -> list[str]:
    problems: list[str] = []

    # The restore helper must exist and must gate on the widescreen HUD being
    # active, so the default (off) path emits nothing new.
    helper = re.search(
        r"void\s+hud_widescreen_restore_screen_ortho\s*\([^)]*\)\s*\{"
        r"(?P<body>.*?)\n\}",
        game_ui, re.DOTALL)
    if not helper:
        return ["hud_widescreen_restore_screen_ortho() not defined in game_ui.c"]
    body = helper.group("body")
    if "hud_widescreen_enabled()" not in body:
        problems.append(
            "hud_widescreen_restore_screen_ortho() must gate on "
            "hud_widescreen_enabled() so the widescreen-HUD-off path is untouched")
    if "mtx_ortho(" not in body:
        problems.append(
            "hud_widescreen_restore_screen_ortho() must re-establish the "
            "standard mtx_ortho() screen space")

    # It must run at the HUD -> dialogue-box boundary, before the boxes draw.
    restore = thread3.find("hud_widescreen_restore_screen_ortho")
    boxes = thread3.find("render_dialogue_boxes")
    if restore < 0:
        problems.append(
            "thread3_main.c must call hud_widescreen_restore_screen_ortho() "
            "before render_dialogue_boxes()")
    elif boxes < 0 or restore > boxes:
        problems.append(
            "hud_widescreen_restore_screen_ortho() must be called before "
            "render_dialogue_boxes() in thread3_main.c")
    return problems


def run(game_ui: str, thread3: str) -> list[str]:
    return check_banana_anchor(game_ui) + check_ortho_restore(game_ui, thread3)


def self_test() -> int:
    """Prove both assertions reject the pre-fix source."""
    game_ui = GAME_UI.read_text()
    thread3 = THREAD3.read_text()

    # Regression 1: banana "x" dropped back to the default (center) anchor.
    broken_ui = re.sub(
        r"\n\s*case HUD_BANANA_COUNT_X:(?=\s*\n\s*case HUD_BANANA_COUNT_NUMBER_1:)",
        "", game_ui, count=1)
    if broken_ui == game_ui:
        print("self-test: FAIL -- could not synthesise the banana-x regression",
              file=sys.stderr)
        return 1
    if not check_banana_anchor(broken_ui):
        print("self-test: FAIL -- banana anchor check passed a broken source",
              file=sys.stderr)
        return 1

    # Regression 2: the ortho restore boundary removed.
    broken_thread3 = thread3.replace(
        "hud_widescreen_restore_screen_ortho(&gCurrDisplayList, &gGameCurrMatrix);",
        "")
    if broken_thread3 == thread3:
        print("self-test: FAIL -- could not synthesise the ortho-restore regression",
              file=sys.stderr)
        return 1
    if not check_ortho_restore(game_ui, broken_thread3):
        print("self-test: FAIL -- ortho-restore check passed a broken source",
              file=sys.stderr)
        return 1

    print("check_widescreen_hud_scope: self-test OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="only prove the assertions fail on the pre-fix source")
    args = parser.parse_args()

    # The self-test (both assertions must reject the pre-fix source) runs on
    # every invocation, so a weakened gate is a suite failure -- the party
    # origin-gate discipline.
    if self_test() != 0:
        return 1
    if args.self_test:
        return 0

    for path in (GAME_UI, THREAD3):
        if not path.is_file():
            print(f"check_widescreen_hud_scope: FAIL -- missing {path}",
                  file=sys.stderr)
            return 1

    problems = run(GAME_UI.read_text(), THREAD3.read_text())
    if problems:
        for problem in problems:
            print(f"check_widescreen_hud_scope: FAIL -- {problem}",
                  file=sys.stderr)
        return 1

    print("check_widescreen_hud_scope: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
