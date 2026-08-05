#!/usr/bin/env python3
"""Structural guards for cached-model material ownership.

ObjectModel instances are cached by model ID and may be referenced by many
world objects. Per-object material choices must therefore stay draw-local in
the native renderer. This test complements the real-ROM door oracle by making
the ownership boundary fail closed at source level.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OBJECTS = ROOT / "game" / "src" / "objects.c"
OBJECTS_HEADER = ROOT / "game" / "src" / "objects.h"
OBJECT_FUNCTIONS = ROOT / "game" / "src" / "object_functions.c"
GFX_PC = ROOT / "platform" / "fast3d" / "gfx_pc_dkr.c"

# The cache-owned batch array is spelled either through a local pointer
# (`batch[i]`) or inline through the relocation macro
# (`DKR_PTR(TriangleBatchInfo, objModel->batches)[batchNum]`); both index the
# same shared allocation, so both spellings are shared-state writes.
BATCH_ELEMENT = (
    r"(?:\w+|DKR_PTR\s*\(\s*TriangleBatchInfo\s*,[^)]*\))\s*\[[^\]]+\]"
)
ASSIGNMENT = r"(?:\+=|-=|\|=|&=|(?<![=!<>])=(?!=))"
BATCH_MATERIAL_WRITE = re.compile(
    rf"{BATCH_ELEMENT}\s*\.\s*(?:texOffset|textureIndex)\s*{ASSIGNMENT}"
)
# The only admissible writer of the shared array: the model-global animated
# texture cadence, which advances one offset for every object that shares the
# cached model. Per-object (door, racer) choices must stay draw-local.
BATCH_MATERIAL_WRITERS = ("obj_tex_animate_model",)
DEFINITION = re.compile(
    r"^[A-Za-z_][\w \t*]*?\b(?P<name>[A-Za-z_]\w*)\s*\([^;{]*?\)\s*\{",
    re.M | re.S,
)
# Draw-local fallback: the shared batch offset is read and shifted into the
# display-list material, never written back.
SHARED_FALLBACK_READ = re.compile(
    r"DKR_PTR\s*\(\s*TriangleBatchInfo\s*,\s*objModel->batches\s*\)"
    r"\s*\[[^\]]+\]\s*\.\s*texOffset\s*<<\s*14"
)


def native_port_projection(source: str, enabled: bool) -> str:
    """Project simple NATIVE_PORT conditionals without expanding C macros."""

    output: list[str] = []
    active = True
    stack: list[tuple[bool, bool | None, bool]] = []
    directive = re.compile(r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$")

    for line in source.splitlines(keepends=True):
        match = directive.match(line)
        if match is None:
            if active:
                output.append(line)
            continue

        command, expression = match.group(1), match.group(2)
        mentions_native = "NATIVE_PORT" in expression
        if command in ("ifdef", "ifndef", "if"):
            parent = active
            condition: bool | None = None
            if mentions_native:
                condition = enabled
                if command == "ifndef" or (
                    command == "if" and expression.lstrip().startswith("!")
                ):
                    condition = not condition
                active = parent and condition
            stack.append((parent, condition, active))
        elif command == "elif":
            if not stack:
                raise AssertionError("unmatched #elif")
            parent, condition, _ = stack[-1]
            if condition is not None:
                condition = enabled if mentions_native else False
                active = parent and condition
                stack[-1] = (parent, condition, active)
        elif command == "else":
            if not stack:
                raise AssertionError("unmatched #else")
            parent, condition, branch_active = stack[-1]
            if condition is not None:
                active = parent and not branch_active
                stack[-1] = (parent, not condition, active)
        elif command == "endif":
            if not stack:
                raise AssertionError("unmatched #endif")
            parent, _, _ = stack.pop()
            active = parent

    if stack:
        raise AssertionError("unterminated preprocessor conditional")
    return "".join(output)


def enclosing_function(source: str, offset: int) -> str | None:
    """Name of the column-zero function definition preceding `offset`."""

    name: str | None = None
    for match in DEFINITION.finditer(source):
        if match.start() > offset:
            break
        name = match.group("name")
    return name


def batch_material_writers(source: str) -> set[str | None]:
    return {
        enclosing_function(source, match.start())
        for match in BATCH_MATERIAL_WRITE.finditer(source)
    }


def function_body(source: str, name: str) -> str:
    """Return a C function body while ignoring braces in comments/strings."""

    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.S)
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


class ObjectMaterialOwnershipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        objects = OBJECTS.read_text(encoding="utf-8")
        header = OBJECTS_HEADER.read_text(encoding="utf-8")
        cls.native_objects = native_port_projection(objects, enabled=True)
        cls.legacy_objects = native_port_projection(objects, enabled=False)
        cls.native_header = native_port_projection(header, enabled=True)
        functions = OBJECT_FUNCTIONS.read_text(encoding="utf-8")
        cls.native_functions = native_port_projection(functions, enabled=True)
        cls.legacy_functions = native_port_projection(functions, enabled=False)
        cls.gfx_pc = GFX_PC.read_text(encoding="utf-8")

    def test_mutating_door_api_is_absent_from_native_build(self) -> None:
        self.assertNotIn("obj_door_number(", self.native_objects)
        self.assertNotIn("obj_door_number(", self.native_header)
        self.assertIn("obj_door_number(", self.legacy_objects)

    def test_authoritative_tick_cannot_publish_per_door_material(self) -> None:
        body = function_body(
            self.native_objects, "obj_authoritative_texture_tick"
        )
        self.assertNotIn("obj_door_number", body)
        self.assertNotRegex(
            body,
            r"\.(?:texOffset|textureIndex)\s*"
            r"(?:\+=|-=|\|=|&=|(?<![=!<>])=(?!=))",
        )

    def test_door_and_racer_selection_are_pure_draw_local_lookups(self) -> None:
        door = function_body(
            self.native_objects, "obj_door_batch_texture_offset"
        )
        self.assertNotRegex(
            door,
            r"batch\s*\[[^]]+\]\.(?:texOffset|textureIndex|flags)\s*"
            r"(?:\+=|-=|\|=|&=|(?<![=!<>])=(?!=))",
        )
        racer = function_body(self.native_objects, "racer_light_tex_offset")
        self.assertNotIn("TriangleBatchInfo", racer)

        render = function_body(self.native_objects, "render_mesh")
        per_door = render.find("obj_door_batch_texture_offset(")
        per_racer = render.find("gObjectRenderRacerTexOffset")
        fallback = SHARED_FALLBACK_READ.search(render)
        self.assertGreaterEqual(per_door, 0)
        self.assertGreater(per_racer, per_door)
        self.assertIsNotNone(fallback)
        self.assertGreater(fallback.start(), per_racer)
        self.assertNotRegex(render, BATCH_MATERIAL_WRITE)

    def test_native_world_path_has_no_legacy_batch_offset_writes(self) -> None:
        native = batch_material_writers(self.native_objects)
        self.assertTrue(native)
        self.assertEqual(native - set(BATCH_MATERIAL_WRITERS), set())
        # Positive control: the same guard still finds the legacy per-object
        # publishers, so its silence on the native build is a real result.
        legacy = batch_material_writers(self.legacy_objects)
        self.assertTrue(legacy - set(BATCH_MATERIAL_WRITERS))

    def test_char_select_numeral_is_not_written_into_the_shared_model(
        self,
    ) -> None:
        # The behaviour loop shares its ObjectModel with every other actor that
        # uses the same model id, so the placard numeral must not be stored.
        native = function_body(self.native_functions, "obj_loop_char_select")
        self.assertNotRegex(native, BATCH_MATERIAL_WRITE)
        # Positive control: the same guard finds the legacy publisher, so its
        # silence on the native projection is a real result.
        legacy = function_body(self.legacy_functions, "obj_loop_char_select")
        self.assertRegex(legacy, BATCH_MATERIAL_WRITE)

    def test_char_select_selection_is_a_pure_draw_local_lookup(self) -> None:
        resolver = function_body(
            self.native_functions, "obj_char_select_batch_texture_index"
        )
        self.assertNotRegex(resolver, BATCH_MATERIAL_WRITE)
        self.assertNotIn("obj_char_select_batch_texture_index",
                         self.legacy_functions)

        render = function_body(self.native_objects, "render_mesh")
        per_char = render.find("obj_char_select_batch_texture_index(")
        fallback = SHARED_FALLBACK_READ.search(render)
        self.assertGreaterEqual(per_char, 0)
        self.assertIsNotNone(fallback)
        self.assertNotRegex(render, BATCH_MATERIAL_WRITE)

    def test_native_object_behaviours_have_no_batch_material_writes(
        self,
    ) -> None:
        native = batch_material_writers(self.native_functions)
        self.assertEqual(native - set(BATCH_MATERIAL_WRITERS), set())
        legacy = batch_material_writers(self.legacy_functions)
        self.assertTrue(legacy - set(BATCH_MATERIAL_WRITERS))

    def test_gl_sampler_memo_key_contains_texture_identity(self) -> None:
        bind = function_body(self.gfx_pc, "dkr_bind_tile")
        identity = bind.find("const bool texture_changed")
        decision = bind.find("if (texture_changed ||")
        publish = bind.find("bound_texture_id[unit] = texture_id")
        self.assertGreaterEqual(identity, 0)
        self.assertGreater(decision, identity)
        self.assertGreater(publish, decision)


if __name__ == "__main__":
    unittest.main()
