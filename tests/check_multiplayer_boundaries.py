#!/usr/bin/env python3
"""Enforce the launcher/session/game multiplayer ownership boundary."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

GAME_FORBIDDEN = (
    re.compile(r"\broom_code\b", re.IGNORECASE),
    re.compile(r"\bmatchmak(?:e|er|ing)\b", re.IGNORECASE),
    re.compile(r"\bwebsocket(?:\b|_)", re.IGNORECASE),
    re.compile(r"\bwebrtc(?:\b|_)", re.IGNORECASE),
    re.compile(r"\bcloudflare\b", re.IGNORECASE),
    re.compile(r"\bdurable[_ ]?object\b", re.IGNORECASE),
    re.compile(r"\bparty_protocol\b", re.IGNORECASE),
    re.compile(r"[\"']/(?:controller|room)/", re.IGNORECASE),
)

PURE_FORBIDDEN = (
    re.compile(r"\b(?:malloc|calloc|realloc|free)\s*\("),
    re.compile(r"\b(?:getenv|setenv|time|clock|gettimeofday)\s*\("),
    re.compile(r"\b(?:socket|connect|send|recv|poll)\s*\("),
    re.compile(r"#\s*include\s*[<\"](?:SDL|emscripten|webgpu|imgui)", re.I),
)


def violations(text: str, patterns: tuple[re.Pattern[str], ...]) -> list[str]:
    found: list[str] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        for pattern in patterns:
            if pattern.search(line):
                found.append(f"line {line_number}: {pattern.pattern}: {line.strip()}")
    return found


def main() -> int:
    failures: list[str] = []
    for path in sorted((ROOT / "game" / "src").glob("*.c")):
        for problem in violations(path.read_text(errors="replace"), GAME_FORBIDDEN):
            failures.append(f"{path.relative_to(ROOT)}: {problem}")

    pure_path = ROOT / "platform" / "session" / "session_core.c"
    for problem in violations(pure_path.read_text(errors="replace"), PURE_FORBIDDEN):
        failures.append(f"{pure_path.relative_to(ROOT)}: {problem}")

    # Built-in broken-direction controls prove this check is not a vacuous scan.
    synthetic_game = '#include "webrtc_transport.h"\nconst char *room_code = "AAAAAA";'
    synthetic_core = '#include <SDL.h>\nvoid f(void) { (void)malloc(4); }'
    if len(violations(synthetic_game, GAME_FORBIDDEN)) < 2:
        failures.append("negative control: game/service boundary mutation escaped")
    if len(violations(synthetic_core, PURE_FORBIDDEN)) < 2:
        failures.append("negative control: pure-core dependency mutation escaped")

    if failures:
        print("FAIL multiplayer source boundaries", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("PASS: game is service-blind and SessionCore remains platform-pure")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
