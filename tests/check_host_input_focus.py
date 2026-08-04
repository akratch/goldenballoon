#!/usr/bin/env python3
"""Prevent latched host controls when the native window loses focus."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / "platform" / "platform_sdl_min.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_body(name: str) -> str:
    match = re.search(rf"static void {name}\(void\)\s*\{{", SOURCE)
    require(match is not None, f"missing {name} helper")
    start = match.end()
    depth = 1
    for index in range(start, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:index]
    raise AssertionError(f"unterminated {name} helper")


def main() -> int:
    clear_body = function_body("input_clear_game_sources")
    require("s_keyboardDown" in clear_body,
            "focus retirement must release keyboard state")
    require("s_controllerSource" in clear_body,
            "focus retirement must release controller buttons and axes")

    focus = re.search(
        r"SDL_WINDOWEVENT_FOCUS_LOST\)\s*\{(?P<body>.*?)\n\s*\}",
        SOURCE,
        re.DOTALL,
    )
    require(focus is not None, "missing native focus-loss branch")
    body = focus.group("body")
    require("input_clear_game_sources();" in body,
            "focus loss must retire every host input source")
    require("input_changed = 1;" in body,
            "focus loss must publish the neutral state immediately")
    print("host input focus contract passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
