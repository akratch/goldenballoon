#!/usr/bin/env python3
"""Keep WebGPU cache eviction coherent with redundant-bind tracking.

The scene pass remembers the last pipeline and bind group it published. Native
WebGPU implementations may recycle a released C handle, so cache-owned objects
must clear those trackers before release. Otherwise a later object at the same
address can false-match and inherit the previous material or pipeline.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
BACKEND = ROOT / "platform" / "fast3d" / "gfx_webgpu.c"

HELPER = "wgpu_release_cached_bind_group"
RAW_RELEASE = re.compile(r"wgpuBindGroupRelease\s*\(\s*(?P<handle>[^)]*)\)")
# Only these two tables can hold a handle the scene pass memoizes.
SCENE_CACHE = re.compile(r"s_bg_cache_tab|s_modern_cache")

# A raw release outside the helper is admissible only where the released handle
# cannot be the one sitting in s_bg_applied. Every entry names the invariant that
# guarantees it, and each invariant is asserted below against the real code.
#   private  — the handle belongs to a private pass (post/resolve/shadow/minimap)
#              and never enters a scene bind-group cache.
#   modern   — modern-mesh handles bind the scene pass directly, so that draw
#              nulls s_bg_applied instead of memoizing them.
#   teardown — pass tracking is reset before any cache member is released.
RAW_RELEASE_ALLOWLIST = {
    "wgpu_run_resolve_to": "private",
    "wgpu_run_postfx": "private",
    "wgpu_release_shadow_resources": "private",
    "wgpu_shadow_ensure_pipeline_resources": "private",
    "wgpu_ensure_minimap": "private",
    "wgpu_modern_resources": "modern",
    "wgpu_draw_modern_mesh": "modern",
    "wgpu_release_device_objects": "teardown",
}


def code_only(source: str) -> str:
    """Blank comments and literal contents, preserving every byte offset.

    Embedded shader text contains braces and would otherwise be parsed as code.
    """

    output = list(source)
    index = 0
    state = "code"
    while index < len(source):
        char = source[index]
        pair = source[index:index + 2]
        if state == "code":
            if pair == "/*":
                state = "block-comment"
                output[index] = output[index + 1] = " "
                index += 2
                continue
            if pair == "//":
                state = "line-comment"
                output[index] = output[index + 1] = " "
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
        elif state == "block-comment":
            if pair == "*/":
                state = "code"
                output[index] = output[index + 1] = " "
                index += 2
                continue
            if char != "\n":
                output[index] = " "
        elif state == "line-comment":
            if char == "\n":
                state = "code"
            else:
                output[index] = " "
        else:
            if char == "\\":
                output[index] = output[index + 1] = " "
                index += 2
                continue
            if (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
            elif char != "\n":
                output[index] = " "
        index += 1
    return "".join(output)


def function_spans(source: str) -> list[tuple[str, int, int]]:
    """Return (name, start, end) for every column-zero function definition."""

    definition = re.compile(
        r"^[A-Za-z_][\w \t*]*?\b(?P<name>[A-Za-z_]\w*)\s*\([^;{]*?\)\s*\{",
        re.M | re.S,
    )
    spans: list[tuple[str, int, int]] = []
    for match in definition.finditer(source):
        start = source.index("{", match.end() - 1)
        depth = 0
        index = start
        while index < len(source):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        spans.append((match.group("name"), start, index))
    return spans


def enclosing_function(
    spans: list[tuple[str, int, int]], offset: int
) -> str | None:
    """Owning function of an offset, or None for file scope (declarations)."""

    for name, start, end in spans:
        if start <= offset <= end:
            return name
    return None


def function_body(source: str, name: str) -> str:
    """Return a C function body, ignoring braces in comments and literals."""

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


class WebGpuCachedBindOwnershipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = BACKEND.read_text(encoding="utf-8")
        cls.code = code_only(cls.source)
        cls.spans = function_spans(cls.code)
        cls.releases = [
            (enclosing_function(cls.spans, match.start()),
             match.group("handle").strip(), match.start())
            for match in RAW_RELEASE.finditer(cls.code)
        ]

    def assert_release_clears_tracker(
        self, helper: str, tracker: str, release_call: str
    ) -> None:
        body = function_body(self.source, helper)
        compare = body.find(f"{tracker} ==")
        clear = body.find(f"{tracker} = NULL")
        release = body.find(release_call)
        self.assertGreaterEqual(compare, 0)
        self.assertGreater(clear, compare)
        self.assertGreater(release, clear)

    def test_cache_release_helpers_clear_applied_handles_first(self) -> None:
        self.assert_release_clears_tracker(
            "wgpu_release_cached_bind_group",
            "s_bg_applied",
            "wgpuBindGroupRelease",
        )
        self.assert_release_clears_tracker(
            "wgpu_release_cached_pipeline",
            "s_pipe_applied",
            "wgpuRenderPipelineRelease",
        )

    def test_every_raw_bind_group_release_is_the_helper_or_allowlisted(self) -> None:
        self.assertGreater(len(self.releases), 1)
        for owner, handle, offset in self.releases:
            self.assertIsNotNone(owner, f"release at offset {offset}")
            if owner == HELPER:
                continue
            self.assertIn(
                owner, RAW_RELEASE_ALLOWLIST,
                f"raw wgpuBindGroupRelease({handle}) in {owner} at offset "
                f"{offset} must call {HELPER}()",
            )
        owners = {owner for owner, _, _ in self.releases}
        self.assertIn(HELPER, owners)
        for name in RAW_RELEASE_ALLOWLIST:
            self.assertIn(name, owners, f"stale allowlist entry {name}")

    def test_the_helper_owns_the_scene_bind_group_caches(self) -> None:
        helper_calls = {
            enclosing_function(self.spans, match.start())
            for match in re.finditer(rf"\b{HELPER}\s*\(", self.code)
        } - {HELPER, None}
        self.assertGreaterEqual(len(helper_calls), 3)

    def test_private_pass_releases_never_touch_a_scene_cache(self) -> None:
        for owner, handle, _ in self.releases:
            if RAW_RELEASE_ALLOWLIST.get(owner) != "private":
                continue
            self.assertNotRegex(handle, SCENE_CACHE, owner)

    def test_modern_mesh_draw_refuses_to_memoize_its_bind_group(self) -> None:
        body = function_body(self.source, "wgpu_draw_modern_mesh")
        bind = body.find("wgpuRenderPassEncoderSetBindGroup(s_pass, 0, res->bg")
        self.assertGreaterEqual(bind, 0)
        cleared = re.search(r"s_bg_applied\s*=\s*NULL", body[bind:])
        self.assertIsNotNone(cleared)
        for owner, handle, _ in self.releases:
            if RAW_RELEASE_ALLOWLIST.get(owner) != "modern":
                continue
            self.assertRegex(handle, r"s_modern_cache|res->bg", owner)

    def test_teardown_resets_pass_tracking_before_every_raw_release(self) -> None:
        name = "wgpu_release_device_objects"
        start = next(s for n, s, _ in self.spans if n == name)
        reset = self.code.index("wgpu_reset_pass_dynamic_state()", start)
        released = [
            offset for owner, _, offset in self.releases if owner == name
        ]
        self.assertTrue(released)
        for offset in released:
            self.assertGreater(offset, reset)

    def test_pipeline_cache_eviction_uses_the_helper(self) -> None:
        body = function_body(self.source, "wgpu_pipe_reserve_slot")
        self.assertIn("wgpu_release_cached_pipeline(", body)
        self.assertNotIn("wgpuRenderPipelineRelease(", body)

    def test_device_teardown_resets_pass_tracking_before_raw_releases(self) -> None:
        body = function_body(self.source, "wgpu_release_device_objects")
        reset = body.find("wgpu_reset_pass_dynamic_state()")
        bind_groups = body.find("wgpuBindGroupRelease(s_bg_cache_tab[i].bg)")
        pipelines = body.find("wgpuRenderPipelineRelease(prg->pipes[j].pipe)")
        self.assertGreaterEqual(reset, 0)
        self.assertGreater(bind_groups, reset)
        self.assertGreater(pipelines, reset)


if __name__ == "__main__":
    unittest.main()
