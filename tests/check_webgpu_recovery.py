#!/usr/bin/env python3
"""Exercise WebGPU bring-up, frame-failure, surface, and device recovery.

The lifecycle transition table itself is exhaustively covered by the ROM-free
C test. This integration check proves that the real native backend wires each
class of transition to a survivable action at a complete-frame boundary. Every
injection uses the public MDKR_WEBGPU_FAULT vocabulary; the one explicit
recovery-failure control exists to prove the terminal WebGPU→GL arm.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

from harness_utils import resolve_binary


class CheckFailure(RuntimeError):
    pass


def run_case(
    binary: Path,
    rom: Path,
    name: str,
    injected: dict[str, str],
    required: tuple[str, ...],
    forbidden: tuple[str, ...] = (),
    frames: int = 5,
    mode: str = "pure",
    dump_frames: bool = False,
    hidden: bool = True,
) -> str:
    always_forbidden = (
        "Validation Error",
        "validation error",
        "Assertion failed",
        "assertion failed",
        "SIGABRT",
        "Abort trap",
        "[webgpu] device error",
    )
    env = os.environ.copy()
    env.update({"MDKR_RENDERER": "webgpu", "MDKR_AUDIO": "0"})
    if hidden:
        env["MDKR64_HIDDEN"] = "1"
        env.pop("MDKR_TEST_VISIBLE_HEADLESS", None)
    else:
        env.pop("MDKR64_HIDDEN", None)
        env["MDKR_TEST_VISIBLE_HEADLESS"] = "1"
        # A bounded headless loop otherwise runs as fast as the CPU and can
        # finish before the native compositor grants the newly shown window
        # its first drawable. Surface-resource fault arms need a real drawable,
        # so pace them at the historical one-field realtime cadence.
        env["MDKR_PACE_REALTIME"] = "1"
        env["MDKR_SIMULATION_CADENCE"] = "enhanced"
        env["MDKR_SYNTH_FIELDS"] = "1"
    env.update(injected)
    with tempfile.TemporaryDirectory(prefix="mdkr-wgpu-fault-") as temporary:
        case_dir = Path(temporary)
        env["MDKR_SAVE_DIR"] = str(case_dir / "save")
        command = [
            str(binary),
            "--rom",
            str(rom),
            "--headless-frames",
            str(frames),
            f"--{mode}",
        ]
        if dump_frames:
            frame_dir = case_dir / "frames"
            frame_dir.mkdir()
            command.extend(("--dump-frames", str(frame_dir)))
        proc = subprocess.run(
            command,
            cwd=Path(__file__).resolve().parent.parent,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=90,
            check=False,
        )
    output = proc.stdout
    if proc.returncode != 0:
        raise CheckFailure(
            f"{name}: exited {proc.returncode}\n{output[-6000:]}"
        )
    missing = [text for text in required if text not in output]
    escaped = [
        text for text in (*always_forbidden, *forbidden) if text in output
    ]
    if missing or escaped:
        raise CheckFailure(
            f"{name}: missing={missing} forbidden={escaped}\n{output[-6000:]}"
        )
    print(f"  PASS {name}")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True)
    parser.add_argument("--rom", required=True)
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if not binary.is_file():
        raise CheckFailure(f"binary not found: {binary}")
    if not rom.is_file():
        raise CheckFailure(f"ROM not found: {rom}")

    def completion(frames: int) -> str:
        return f"[SDL] headless: reached {frames} frames, exiting cleanly."

    startup_fallback_points = (
        "bringup.instance",
        "bringup.surface",
        "bringup.adapter",
        "bringup.device-defaults",
        "bringup.queue",
        "capabilities.format",
    )
    for point in startup_fallback_points:
        run_case(
            binary,
            rom,
            f"{point} -> startup GL",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                "[SDL] GL ready:",
                completion(5),
            ),
        )

    run_case(
        binary,
        rom,
        "raised device limits -> default-limit retry",
        {"MDKR_WEBGPU_FAULT": "bringup.device-limits"},
        (
            "[webgpu-fault] injected bringup.device-limits@1",
            "retrying with default limits",
            "[webgpu] backend initialized",
            completion(5),
        ),
        ("[SDL] GL ready:",),
    )

    run_case(
        binary,
        rom,
        "feature-limited depth adapter",
        {"MDKR_WEBGPU_FAULT": "capabilities.depth-clip-absent"},
        (
            "[webgpu-fault] injected capabilities.depth-clip-absent@1",
            "far-plane-crossing geometry will use the frontend's "
            "homogeneous-z clamp",
            completion(20),
        ),
        ("[SDL] GL ready:",),
        frames=20,
    )

    for point in (
        "capabilities.configure",
        "surface.configure",
        "frame.encoder",
        "frame.pass",
        "frame.finish",
        "queue.submit",
        "surface.error",
    ):
        run_case(
            binary,
            rom,
            f"{point} -> one native reinit",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                "attempting one native device reinitialization",
                "native device recovery succeeded; gameplay continues",
                completion(20),
            ),
            ("switched to OpenGL",),
            frames=20,
        )

    run_case(
        binary,
        rom,
        "device loss -> WebGPU reinit",
        {"MDKR_WEBGPU_FAULT": "device.lost"},
        (
            "[webgpu-fault] injected device.lost@1",
            "attempting one native device reinitialization",
            "native device recovery succeeded; gameplay continues",
            completion(20),
        ),
        ("switched to OpenGL",),
        frames=20,
    )

    run_case(
        binary,
        rom,
        "attachment-only surface blit -> device rebuild",
        {
            "MDKR_TEST_WEBGPU_SURFACE_USAGES": "attachment",
            "MDKR_TEST_OCCLUDED_SURFACE_FAULTS": "1",
            "MDKR_WEBGPU_FAULT": "surface.blit-view",
        },
        (
            "[webgpu-fault] injected surface.blit-view@1",
            "native device recovery succeeded; gameplay continues",
            completion(120),
        ),
        ("switched to OpenGL",),
        frames=120,
        hidden=False,
    )

    run_case(
        binary,
        rom,
        "direct-present view failure -> complete offscreen fallback",
        {
            "MDKR_TEST_WEBGPU_DIRECT_VIEW": "1",
            "MDKR_TEST_OCCLUDED_SURFACE_FAULTS": "1",
            "MDKR_RENDER_SCALE": "1",
            "MDKR_REMASTER_FX": "1",
            "MDKR_WEBGPU_FAULT": "surface.direct-view",
        },
        (
            "[webgpu-fault] injected surface.direct-view@1",
            completion(120),
        ),
        (
            "attempting one native device reinitialization",
            "switched to OpenGL",
        ),
        frames=120,
        mode="remastered",
        hidden=False,
    )

    run_case(
        binary,
        rom,
        "device loss -> GL",
        {
            "MDKR_WEBGPU_FAULT": "device.lost",
            "MDKR_TEST_WEBGPU_RECOVERY_FAIL": "1",
        },
        (
            "[webgpu-fault] injected device.lost@1",
            "injected native recovery failure",
            "recovery failed; switched to OpenGL without restarting",
            "[SDL] GL ready:",
            completion(20),
        ),
        frames=20,
    )
    run_case(
        binary,
        rom,
        "presented-surface capture allocation -> local diagnostic failure",
        {
            "MDKR_WEBGPU_FAULT": "capture.surface-buffer",
            "GE007_WEBGPU_DUMP_SURFACE": "1",
        },
        (
            "[webgpu-fault] injected capture.surface-buffer@1",
            completion(20),
        ),
        (
            "attempting one native device reinitialization",
            "switched to OpenGL",
        ),
        frames=20,
        hidden=False,
    )

    run_case(
        binary,
        rom,
        "second device loss -> bounded GL switch",
        {"MDKR_WEBGPU_FAULT": "device.lost@all"},
        (
            "[webgpu-fault] injected device.lost@1",
            "native device recovery succeeded; gameplay continues",
            "[webgpu-fault] injected device.lost@2",
            "recovery failed; switched to OpenGL without restarting",
            completion(20),
        ),
        frames=20,
    )

    run_case(
        binary,
        rom,
        "persistent surface timeout is bounded",
        {"MDKR_WEBGPU_FAULT": "surface.timeout@all"},
        (
            "[webgpu-fault] injected surface.timeout@1",
            "surface recovery failed for 120 consecutive frames",
            "native device recovery succeeded; gameplay continues",
            completion(300),
        ),
        frames=300,
    )

    for point in (
        "surface.suboptimal",
        "surface.timeout",
        "surface.outdated",
        "surface.lost",
    ):
        run_case(
            binary,
            rom,
            f"{point} -> bounded in-place repair",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                completion(20),
            ),
            (
                "attempting one native device reinitialization",
                "switched to OpenGL",
            ),
            frames=20,
        )

    run_case(
        binary,
        rom,
        "alpha capability fallback",
        {"MDKR_WEBGPU_FAULT": "capabilities.alpha"},
        (
            "[webgpu-fault] injected capabilities.alpha@1",
            completion(20),
        ),
        ("attempting one native device reinitialization",),
        frames=20,
    )
    run_case(
        binary,
        rom,
        "present-mode capability fallback",
        {
            "MDKR_WEBGPU_FAULT": "capabilities.present",
            "GE007_WEBGPU_PRESENT": "mailbox",
        },
        (
            "[webgpu-fault] injected capabilities.present@1",
            "using fifo",
            completion(20),
        ),
        ("attempting one native device reinitialization",),
        frames=20,
    )

    pure_fatal_points = (
        "frame.noise-buffer",
        "target.scene-texture",
        "target.scene-view",
        "target.depth-texture",
        "target.depth-view",
        "target.post-texture",
        "target.post-view",
        "target.resolve-texture",
        "target.resolve-view",
        "target.output-depth-texture",
        "target.output-depth-view",
        "shader.bind-group-layout",
        "shader.module",
        "shader.pipeline-layout",
        "shader.pipeline-sync",
        "texture.texture",
        "texture.view",
        "texture.sampler",
        "draw.white-texture",
        "draw.white-view",
        "draw.default-sampler",
        "draw.vertex-buffer",
        "draw.bind-group",
    )
    for point in pure_fatal_points:
        run_case(
            binary,
            rom,
            f"{point} -> device rebuild",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                "native device recovery succeeded; gameplay continues",
                completion(120),
            ),
            ("switched to OpenGL",),
            frames=120,
        )

    remastered_fatal_points = (
        "resolve.module",
        "resolve.bind-group-layout",
        "resolve.uniform",
        "resolve.sampler",
        "resolve.pipeline-layout",
        "resolve.pipeline",
        "resolve.bind-group",
        "resolve.pass",
        "post.module",
        "post.bind-group-layout",
        "post.uniform",
        "post.sampler-nearest",
        "post.sampler-linear",
        "post.pipeline-layout",
        "post.pipeline",
        "post.bind-group",
        "post.pass",
        "overlay.output-pass",
    )
    for point in remastered_fatal_points:
        run_case(
            binary,
            rom,
            f"{point} -> device rebuild",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                "native device recovery succeeded; gameplay continues",
                completion(120),
            ),
            ("switched to OpenGL",),
            frames=120,
            mode="remastered",
        )

    for point in ("texture.mip-texture", "texture.mip-view"):
        run_case(
            binary,
            rom,
            f"{point} -> faithful single-level fallback",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                completion(120),
            ),
            (
                "attempting one native device reinitialization",
                "switched to OpenGL",
            ),
            frames=120,
            mode="remastered",
        )

    run_case(
        binary,
        rom,
        "internal frame-capture allocation -> local diagnostic failure",
        {
            "MDKR_WEBGPU_FAULT": "capture.frame-buffer",
            "GE007_WEBGPU_DUMP_FRAME": "1",
        },
        (
            "[webgpu-fault] injected capture.frame-buffer@1",
            completion(20),
        ),
        (
            "attempting one native device reinitialization",
            "switched to OpenGL",
        ),
        frames=20,
    )
    for point in (
        "readback.buffer",
        "readback.encoder",
        "readback.finish",
        "readback.map",
    ):
        run_case(
            binary,
            rom,
            f"{point} -> local diagnostic failure",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                completion(20),
            ),
            (
                "attempting one native device reinitialization",
                "switched to OpenGL",
            ),
            frames=20,
            dump_frames=True,
        )

    segmented = run_case(
        binary,
        rom,
        "forced-small vertex stream -> bounded segmentation",
        {"MDKR_TEST_WEBGPU_VERTEX_SEGMENT_BYTES": "4096"},
        (completion(120),),
        (
            "vertex-buffer segment allocation failed",
            "attempting one native device reinitialization",
            "switched to OpenGL",
        ),
        frames=120,
        mode="remastered",
    )
    segment_match = re.search(
        r"\[WGPU-LIMITS\].*vertexBytes=(\d+)\s+"
        r"vertexSegments=(\d+)\s+vertexSegmentCap=(\d+)",
        segmented,
    )
    if (
        segment_match is None
        or int(segment_match.group(1)) <= int(segment_match.group(3))
        or int(segment_match.group(2)) < 2
    ):
        raise CheckFailure(
            "forced-small vertex stream did not prove segmentation\n"
            + segmented[-6000:]
        )

    shader_guard = run_case(
        binary,
        rom,
        "forced shader-index exhaustion -> no-reuse guard and bounded GL",
        {"MDKR_TEST_WEBGPU_SHADER_LIMIT": "1"},
        (
            "shader hard limit reached (effective=1 hard=4096); "
            "refusing unsafe pointer reuse",
            "native device recovery succeeded; gameplay continues",
            "recovery failed; switched to OpenGL without restarting",
            completion(120),
        ),
        frames=120,
    )
    guard_match = re.search(
        r"\[WGPU-LIMITS\]\s+shaders=(\d+)/4096\s+pipelines=(\d+).*"
        r"tableOverflow=(\d+)\s+pipelineFailures=(\d+)",
        shader_guard,
    )
    if (
        guard_match is None
        or int(guard_match.group(1)) != 1
        or int(guard_match.group(3)) < 2
        or int(guard_match.group(4)) != 0
    ):
        raise CheckFailure(
            "forced shader-index exhaustion did not prove bounded refusal "
            "without pipeline corruption\n"
            + shader_guard[-6000:]
        )

    case_count = (
        len(startup_fallback_points)
        + 2
        + 7
        + 4
        + 4
        + 2
        + len(pure_fatal_points)
        + len(remastered_fatal_points)
        + 2
        + 4
        + 4
    )
    print(
        "PASS: WebGPU recovery integration "
        f"({case_count} injected cases + forced vertex segmentation and "
        "shader-index exhaustion)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CheckFailure, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}")
        raise SystemExit(1)
