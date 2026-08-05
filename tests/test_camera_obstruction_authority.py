#!/usr/bin/env python3
"""Fail closed if camera correction crosses fixed-tick authority boundaries.

The obstruction runtime is live and this source-level CTest pins its only
valid placement: both fixed-tick routes must resolve exactly once each, after
the TT-camera author and before simulation-owned sort/LOD/visibility.
It also prevents those simulation-owned passes from consuming a resolved,
display, projection, effective, or presentation camera API.  Such state may be
used only by private rendering work; their basis must remain explicitly logical.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OBJECTS = ROOT / "game" / "src" / "objects.c"
THREAD3 = ROOT / "game" / "src" / "thread3_main.c"

AUTHORITY_FUNCTIONS = {
    "obj_sort_tick": "scene_build_last_viewport_basis",
    "obj_lod_tick": "scene_build_last_viewport_basis",
    "obj_visibility_tick": "scene_visibility_prepare_viewport",
}
RESOLVED_WORDS = re.compile(
    r"\b(?:mdkr_)?(?:camera_obstruction|camera|cam)_"
    r"[A-Za-z0-9_]*(?:resolved|display|projection|effective|present)"
    r"[A-Za-z0-9_]*\s*\("
)
OBSTRUCTION_API_CALL = re.compile(
    r"\b(?:mdkr_)?camera_obstruction_[A-Za-z0-9_]*\s*\("
)
LOGICAL_BASIS_CALL = re.compile(
    r"\b[A-Za-z_][A-Za-z0-9_]*logical[A-Za-z0-9_]*"
    r"(?:basis|view)[A-Za-z0-9_]*\s*\("
)


def function_body(source: str, name: str) -> str:
    """Return one C function body, ignoring braces in comments and strings."""

    # All guarded functions are void. Anchoring at the declaration's source line
    # prevents prose such as "by obj_lod_tick(). Keeping ..." from becoming a
    # false function header when the expression spans into the next definition.
    match = re.search(
        rf"^[ \t]*(?:static\s+)?void\s+{re.escape(name)}\s*"
        rf"\([^;]*?\)\s*\{{",
        source,
        re.M | re.S,
    )
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = source.find("{", match.start())
    depth = 0
    index = start
    state = "code"
    while index < len(source):
        char = source[index]
        pair = source[index:index + 2]
        if state == "code":
            if pair == "/*":
                state = "block-comment"
                index += 2
                continue
            if pair == "//":
                state = "line-comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start:index + 1]
        elif state == "block-comment" and pair == "*/":
            state = "code"
            index += 2
            continue
        elif state == "line-comment" and char == "\n":
            state = "code"
        elif state in ("string", "character"):
            if char == "\\":
                index += 2
                continue
            if (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        index += 1
    raise AssertionError(f"unterminated function {name}")


def code_only(source: str) -> str:
    """C source with comment and string-literal text blanked out.

    Every needle below is an assertion about what the code does. Matching a
    needle in a comment or a string is how a gate quietly passes on prose, so
    the order-blind regex searches run against this view.
    """

    result: list[str] = []
    index = 0
    state = "code"
    while index < len(source):
        char = source[index]
        pair = source[index:index + 2]
        if state == "code":
            if pair == "/*":
                state = "block-comment"
                index += 2
                continue
            if pair == "//":
                state = "line-comment"
                index += 2
                continue
            if char in ('"', "'"):
                state = "string" if char == '"' else "character"
            result.append(char)
        elif state == "block-comment":
            if pair == "*/":
                state = "code"
                index += 2
                continue
            result.append("\n" if char == "\n" else " ")
        elif state == "line-comment":
            if char == "\n":
                state = "code"
            result.append("\n" if char == "\n" else " ")
        else:
            if char == "\\":
                result.append("  ")
                index += 2
                continue
            if (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
                result.append(char)
            else:
                result.append(" ")
        index += 1
    return "".join(result)


def calls_named(source: str, name: str) -> list[int]:
    """Find actual call expressions, excluding comments and declarations."""

    result: list[int] = []
    index = 0
    state = "code"
    pattern = re.compile(rf"\b{re.escape(name)}\s*\(")
    while index < len(source):
        char = source[index]
        pair = source[index:index + 2]
        if state == "code":
            if pair == "/*":
                state = "block-comment"
                index += 2
                continue
            if pair == "//":
                state = "line-comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            else:
                match = pattern.match(source, index)
                if match is not None:
                    result.append(index)
                    index = match.end()
                    continue
        elif state == "block-comment" and pair == "*/":
            state = "code"
            index += 2
            continue
        elif state == "line-comment" and char == "\n":
            state = "code"
        elif state in ("string", "character"):
            if char == "\\":
                index += 2
                continue
            if (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        index += 1
    return result


class CameraObstructionAuthorityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.objects = OBJECTS.read_text(encoding="utf-8")
        cls.thread3 = THREAD3.read_text(encoding="utf-8")

    def test_authoritative_camera_consumers_stay_logical(self) -> None:
        for function, legacy_logical_builder in AUTHORITY_FUNCTIONS.items():
            with self.subTest(function=function):
                body = code_only(function_body(self.objects, function))
                self.assertIsNone(
                    RESOLVED_WORDS.search(body),
                    f"{function} must not consume resolved/display camera state",
                )
                obstruction_calls = OBSTRUCTION_API_CALL.findall(body)
                self.assertTrue(
                    all("logical" in call.lower() for call in obstruction_calls),
                    f"{function} may use camera_obstruction APIs only through "
                    "an explicitly named logical basis/view API",
                )
                self.assertTrue(
                    calls_named(body, legacy_logical_builder)
                    or LOGICAL_BASIS_CALL.search(body),
                    f"{function} must retain its legacy logical builder or switch "
                    "to an explicitly named logical basis/view API",
                )

    def test_resolver_has_exactly_two_fixed_tick_sites(self) -> None:
        # The finalizer is integrated. "Absent" was a pre-integration allowance,
        # and while it stood, deleting both call sites passed this gate.
        calls = calls_named(self.thread3, "camera_obstruction_tick")
        self.assertEqual(
            len(calls),
            2,
            "camera_obstruction_tick must appear exactly once in mode_game and "
            "once in update_menu_scene",
        )

        mode_game = function_body(self.thread3, "mode_game")
        menu_scene = function_body(self.thread3, "update_menu_scene")
        self.assertEqual(len(calls_named(mode_game, "camera_obstruction_tick")), 1)
        self.assertEqual(len(calls_named(menu_scene, "camera_obstruction_tick")), 1)

        for route_name, body in (("mode_game", mode_game), ("update_menu_scene", menu_scene)):
            with self.subTest(route=route_name):
                tt = calls_named(body, "scene_tt_camera_tick")
                resolver = calls_named(body, "camera_obstruction_tick")
                sort = calls_named(body, "obj_sort_tick")
                lod = calls_named(body, "obj_lod_tick")
                visibility = calls_named(body, "obj_visibility_tick")
                self.assertEqual(len(tt), 1, f"{route_name} must author TT camera once")
                self.assertEqual(len(resolver), 1, f"{route_name} must resolve once")
                self.assertEqual(len(sort), 1, f"{route_name} must sort once")
                self.assertEqual(len(lod), 1, f"{route_name} must commit LOD once")
                self.assertEqual(len(visibility), 1, f"{route_name} must cull once")
                self.assertLess(tt[0], resolver[0])
                self.assertLess(resolver[0], sort[0])
                self.assertLess(resolver[0], lod[0])
                self.assertLess(resolver[0], visibility[0])


if __name__ == "__main__":
    unittest.main()
