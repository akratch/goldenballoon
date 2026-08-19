#!/usr/bin/env python3
"""Prove finite runs unwind through every native runtime owner.

Historically ``platform_frame_sync`` called ``exit()`` at the headless boundary.
That made a successful process status indistinguishable from leaked renderer
children, an unclosed browser audio sink, and cleanup code in ``main`` that was
literally unreachable. This gate requires the dependency-ordered terminal
evidence from both native renderers:

    headless boundary -> audio -> backend -> frontend -> SDL host

Every run uses a private save directory and a finite frame count.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
FATAL_MARKERS = (
    "[CRASH]",
    "[FATAL]",
    "AddressSanitizer",
    "runtime error:",
    "memory access out of bounds",
)
GL_RE = re.compile(
    r"\[GL-SHUTDOWN\] context=1 programs=(\d+) livePrograms=0 "
    r"liveCoreBuffers=0 liveTargets=0 cpuCapture=0"
)
WGPU_RE = re.compile(
    r"\[WGPU-SHUTDOWN\] roots=owned shaders=(\d+) textures=(\d+) "
    r"pendingPipelines=0 liveChildren=0 cpuArrays=0"
)
GFX_RE = re.compile(
    r"\[GFX-SHUTDOWN\] texturesCreated=(\d+) texturesDeleted=(\d+) "
    r"live=(\d+)->0 shaders=(\d+) backendReleased=1 cpuScratch=0"
)


def validate_output(output: str, renderer: str, frames: int) -> list[str]:
    errors: list[str] = []
    backend_marker = (
        "[GL-SHUTDOWN]" if renderer == "gl" else "[WGPU-SHUTDOWN]"
    )
    required = (
        f"[mdkr64] renderer backend: {renderer}",
        f"[SDL] headless: reached {frames} frames, exiting cleanly.",
        "[AUDIO-SHUTDOWN]",
        backend_marker,
        "[GFX-SHUTDOWN]",
        "[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0",
        "[SDL-SHUTDOWN] window=0 glContext=0 controllers=0 sdl=0",
    )
    for marker in required:
        if output.count(marker) != 1:
            errors.append(
                f"{renderer}: expected exactly one {marker!r}, "
                f"saw {output.count(marker)}"
            )
    for marker in FATAL_MARKERS:
        if marker in output:
            errors.append(f"{renderer}: fatal marker {marker!r}")

    backend_match = GL_RE.search(output) if renderer == "gl" else WGPU_RE.search(output)
    if backend_match is None:
        errors.append(f"{renderer}: backend terminal counters are not zero")
    elif int(backend_match.group(1)) <= 0:
        errors.append(f"{renderer}: route created no shader/program to release")

    gfx = GFX_RE.search(output)
    if gfx is None:
        errors.append(f"{renderer}: frontend terminal counters are malformed")
    else:
        created, deleted, live_before, shaders = map(int, gfx.groups())
        if deleted > created or live_before > created or shaders <= 0:
            errors.append(
                f"{renderer}: incoherent frontend ownership "
                f"created={created} deleted={deleted} live={live_before} "
                f"shaders={shaders}"
            )

    positions = [
        output.find(f"[SDL] headless: reached {frames} frames"),
        output.find("[AUDIO-SHUTDOWN]"),
        output.find(backend_marker),
        output.find("[GFX-SHUTDOWN]"),
        output.find("[HOST-SHUTDOWN]"),
        output.find("[SDL-SHUTDOWN]"),
    ]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        errors.append(
            f"{renderer}: teardown order is not "
            "boundary->audio->backend->frontend->SDL"
        )
    return errors


def run_backend(
    binary: Path, rom: Path, renderer: str, frames: int, timeout: float
) -> list[str]:
    with tempfile.TemporaryDirectory(
        prefix=f"mdkr_final_shutdown_{renderer}_"
    ) as temporary:
        root = Path(temporary)
        env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith("MDKR_")
        }
        env.update(
            {
                "MDKR_AUDIO": "0",
                "MDKR_RENDERER": renderer,
                "MDKR_SAVE_DIR": str(root / "save"),
                # Isolate the video config with the save (see check_door_blocks.py).
                "MDKR_VIDEO_CONFIG_PATH": str(root / "save" / "video.ini"),
            }
        )
        if renderer == "webgpu":
            env["MDKR_TEST_VISIBLE_HEADLESS"] = "1"
        command = [
            str(binary),
            "--headless-frames",
            str(frames),
            "--rom",
            str(rom),
        ]
        try:
            process = subprocess.run(
                command,
                cwd=root,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return [f"{renderer}: timed out after {timeout:.0f}s"]
        errors = validate_output(process.stdout, renderer, frames)
        if process.returncode != 0:
            errors.append(f"{renderer}: exited {process.returncode}")
        if errors:
            errors.append(
                f"{renderer}: output tail:\n"
                + "\n".join(process.stdout.splitlines()[-80:])
            )
        return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=120)
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    if not binary.is_file() or not rom.is_file():
        print("check_final_shutdown: FAIL — binary or ROM is missing", file=sys.stderr)
        return 1
    if args.frames < 10:
        print("check_final_shutdown: FAIL — --frames must be at least 10", file=sys.stderr)
        return 1

    # Parser/checker positive controls: missing teardown, wrong order, and a
    # nonzero live backend class must each be rejected.
    synthetic = "\n".join(
        (
            "[mdkr64] renderer backend: gl",
            f"[SDL] headless: reached {args.frames} frames, exiting cleanly.",
            "[AUDIO-SHUTDOWN] device=0 web=0 complete=1",
            "[GL-SHUTDOWN] context=1 programs=1 livePrograms=1 "
            "liveCoreBuffers=0 liveTargets=0 cpuCapture=0",
            "[GFX-SHUTDOWN] texturesCreated=0 texturesDeleted=0 "
            "live=0->0 shaders=1 backendReleased=1 cpuScratch=0",
            "[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0",
            "[SDL-SHUTDOWN] window=0 glContext=0 controllers=0 sdl=0",
        )
    )
    if not validate_output(synthetic, "gl", args.frames):
        print(
            "check_final_shutdown: FAIL — nonzero-live positive control passed",
            file=sys.stderr,
        )
        return 1
    reordered = synthetic.replace(
        "[AUDIO-SHUTDOWN] device=0 web=0 complete=1\n", ""
    ) + "\n[AUDIO-SHUTDOWN] device=0 web=0 complete=1"
    if not validate_output(reordered, "gl", args.frames):
        print(
            "check_final_shutdown: FAIL — ordering positive control passed",
            file=sys.stderr,
        )
        return 1

    failures: list[str] = []
    for renderer in ("gl", "webgpu"):
        errors = run_backend(
            binary, rom, renderer, args.frames, args.timeout
        )
        if errors:
            failures.extend(errors)
        else:
            print(f"  {renderer}: dependency-ordered final shutdown PASS")
    if failures:
        print(
            "check_final_shutdown: FAIL\n" + "\n".join(failures),
            file=sys.stderr,
        )
        return 1
    print(
        "check_final_shutdown: PASS — GL/WebGPU finite runs released audio, "
        "backend children, frontend caches, and SDL in dependency order"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
