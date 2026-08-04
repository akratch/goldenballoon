#!/usr/bin/env python3
"""Keep browser pipeline-cache completion on the renderer event boundary."""

from pathlib import Path
import sys


source = (Path(__file__).resolve().parents[1] / "platform" / "fast3d" /
          "gfx_webgpu.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)


require("cb.mode = WGPUCallbackMode_AllowProcessEvents;" in source,
        "async pipeline callback must require ProcessEvents")
require("cb.mode = WGPUCallbackMode_AllowSpontaneous;" not in source,
        "async pipeline callback must not mutate cache spontaneously")
callback = source[source.index("static void on_pipeline_ready"):source.index(
    "static bool wgpu_kick_async_pipeline")]
require("if (!s_pipeline_callback_owner_drain)" in callback,
        "pipeline callback must reject a non-owner dispatch")
require(callback.index("if (!s_pipeline_callback_owner_drain)") <
        callback.index("if (s_pending_pipelines > 0)"),
        "owner assertion must precede pipeline-cache mutation")
require("abort();" in callback,
        "an impossible non-owner callback must terminate without racing cache state")
require("ctx->generation != s_active_work_generation" in callback and
        "program is deliberately never" in callback,
        "late callback must be generation-guarded before using its program")
require("host_frame - ctx->kick_host_frame" in callback and
        "host_frame - ctx->kick_host_frame + 1" not in callback and
        "opportunities that were actually incomplete" in callback and
        "ctx->kick_host_frame = (uint64_t)g_frameCounter" in source and
        "s_perf_frame_serial" not in source,
        "pipeline latency must count incomplete host opportunities without "
        "including the ready endpoint or internal replay passes")
require(source.count("wgpu_pipeline_callback_owner_poll();") >= 2,
        "all queue event polls must enter the owner boundary")
require(source.count("wgpu_pipeline_callback_owner_drain();") >= 2,
        "frame and shutdown drains must enter the owner boundary")
require(source.count("WGPU_PIPELINE_CALLBACK_OWNER_WAIT(") == 5,
        "all four yielding waits must use the owner-boundary wrapper")
require("WGPU_COMPAT_WAIT(" not in source,
        "raw compatibility waits must not bypass pipeline callback ownership")
wrapper_end = source.index("/* PERF-005b:")
for raw_pump in ("WGPU_COMPAT_PUMP(", "WGPU_COMPAT_DRAIN(",
                 "WGPU_COMPAT_QUEUE_POLL(", "WGPU_COMPAT_QUEUE_BLOCK("):
    positions = [index for index in range(len(source))
                 if source.startswith(raw_pump, index)]
    require(positions and all(index < wrapper_end for index in positions),
            f"{raw_pump} must only occur inside an owner wrapper")
require("ownerEventPumps=%llu" in source,
        "event-pump telemetry must not be mislabeled as drains")
require("shutdown guarded %d late pipeline completion(s)" in source,
        "shutdown must report generation-guarded late completions")

print("webgpu pipeline callback ownership: PASS")
