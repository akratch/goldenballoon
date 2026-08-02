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

    def test_every_live_bind_group_cache_release_uses_the_helper(self) -> None:
        for name in (
            "wgpu_bg_cache_invalidate_view",
            "wgpu_bg_cache_invalidate_view_indexed",
            "wgpu_draw_triangles",
        ):
            body = function_body(self.source, name)
            self.assertIn("wgpu_release_cached_bind_group(", body, name)
            self.assertNotIn("wgpuBindGroupRelease(", body, name)

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
