#!/usr/bin/env python3
"""Prove the RDP cutout classifier reads the blender bits the assets were authored around.

DKR forked a render mode, ``G_RM_AA_ZB_XLU_LINE_MOD`` (game/include/f3ddkr.h),
specifically to switch ``ALPHA_CVG_SEL`` off while keeping ``CVG_X_ALPHA`` and
``FORCE_BL`` on -- the in-source comment says the removal is the point. That word
is an ordinary alpha blend, and the game places it in the anti-aliased +
Z-compare slot of every ``dRenderSettingsCutout`` group and of the
``dRenderSettingsBlinkingLights`` cutout groups (game/src/textures_sprites.c).
Classifying a cutout on ``CVG_X_ALPHA`` alone collapses that authored blend into
a hard 0.19 alpha test, which also bakes into the cached mip chain.

Each backend renders the same deterministic character-select route twice:

* production: ``CVG_X_ALPHA | ALPHA_CVG_SEL`` together and ``FORCE_BL`` clear;
* positive control: ``MDKR_TEXEDGE=legacy`` restores the CVG_X_ALPHA-only test.

The gameplay-state stream must stay identical across every arm, the production
frame must contain a real scene, and the legacy control must move pixels on both
GL and WebGPU -- otherwise the classifier change is inert. The measured footprint
is small and localized on purpose: the reclassified draws are the soft alpha
edges of character-model overlay materials, so the assertion is bounded from both
sides. Too few pixels means the correction never reached the screen; a
whole-screen delta means it reached far more than the authored blend.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, fatal_re,
                           FX_MARKERS, read_ppm as read_ppm_bytes,
                           resolve_binary)


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_character_select.txt"
RENDERER_SOURCE = ROOT / "platform" / "fast3d" / "gfx_pc_dkr.c"
REQUIRED_SOURCE_COUNTS = {
    'getenv("MDKR_TEXEDGE")': 1,
    "dkr_other_mode_l_is_cutout(": 3,
    "(other_mode_l & (CVG_X_ALPHA | ALPHA_CVG_SEL)) ==": 1,
    "(other_mode_l & FORCE_BL) == 0": 1,
}
# Measured on the pinned route/frame: 131 of 1228800 pixels change, and the
# largest single-channel delta is 203. The window is wide enough for driver
# rasterization differences and narrow enough to fail an over-broad classifier.
MIN_CHANGED = 40
MAX_CHANGED_FRACTION = 0.01
MIN_MAX_DELTA = 24
FATAL_RE = fatal_re(*ASSERT_MARKERS, *FX_MARKERS)


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True)
class Arm:
    backend: str
    legacy: bool
    pace: tuple[str, ...]
    image: Image


def prove_source_contract(source: str) -> list[str]:
    failures: list[str] = []
    for fragment, expected in REQUIRED_SOURCE_COUNTS.items():
        actual = source.count(fragment)
        if actual != expected:
            failures.append(
                f"{fragment!r}: found {actual}, expected {expected}"
            )
    if failures:
        return failures
    for fragment in REQUIRED_SOURCE_COUNTS:
        mutated = source.replace(fragment, "", 1)
        if all(
            mutated.count(required) == expected
            for required, expected in REQUIRED_SOURCE_COUNTS.items()
        ):
            failures.append(
                f"source assertion is inert after removing {fragment!r}"
            )
    return failures


def normalized_pace(output: str) -> tuple[str, ...]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker >= 0:
            rows.append(re.sub(r" dtms=\S+", " dtms=<wall>", line[marker:]))
    return tuple(rows)


def read_ppm(path: Path) -> Image:
    return Image(*read_ppm_bytes(path))


def clean_environment(backend: str, legacy: bool) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_RENDERER=backend,
        MDKR_DUMP_FROM="1500",
        MDKR_DUMP_EVERY="999",
        MDKR64_HIDDEN="1",
        LC_ALL="C",
    )
    if legacy:
        env["MDKR_TEXEDGE"] = "legacy"
    return env


def run_arm(
    binary: Path,
    rom: Path,
    backend: str,
    legacy: bool,
    work: Path,
    frames: int,
    timeout: int,
    verbose: bool,
) -> Arm:
    label = f"{backend}-{'legacy' if legacy else 'fixed'}"
    run_dir = work / label
    frame_dir = run_dir / "frames"
    frame_dir.mkdir(parents=True)
    command = [
        str(binary),
        "--headless-frames",
        str(frames),
        "--input-script",
        str(SCRIPT),
        "--dump-frames",
        str(frame_dir),
        "--rom",
        str(rom),
        "--pure",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=clean_environment(backend, legacy),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        raise RuntimeError(f"{label}: timed out\n{output[-4000:]}") from exc

    output = proc.stdout
    fatal = FATAL_RE.search(output)
    dumps = sorted(frame_dir.glob("*.ppm"))
    if proc.returncode != 0 or fatal is not None or len(dumps) != 1:
        reason = (
            f"exit={proc.returncode}, fatal="
            f"{fatal.group(0) if fatal else 'none'}, dumps={len(dumps)}"
        )
        raise RuntimeError(f"{label}: {reason}\n{output[-4000:]}")
    pace = normalized_pace(output)
    if not pace:
        raise RuntimeError(f"{label}: no [PACE] rows; route did not run")
    return Arm(backend, legacy, pace, read_ppm(dumps[0]))


def scene_colours(image: Image) -> int:
    return len(
        {
            (image.pixels[i] >> 3,
             image.pixels[i + 1] >> 3,
             image.pixels[i + 2] >> 3)
            for i in range(0, len(image.pixels), 3 * 8)
        }
    )


def difference(fixed: Image, legacy: Image) -> tuple[int, int]:
    if (fixed.width, fixed.height) != (legacy.width, legacy.height):
        raise ValueError(
            f"dimension mismatch {fixed.width}x{fixed.height} vs "
            f"{legacy.width}x{legacy.height}"
        )
    changed = 0
    peak = 0
    for i in range(0, len(fixed.pixels), 3):
        a = fixed.pixels[i:i + 3]
        b = legacy.pixels[i:i + 3]
        if a != b:
            changed += 1
            delta = max(abs(x - y) for x, y in zip(a, b))
            if delta > peak:
                peak = delta
    return changed, peak


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=1600)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument(
        "--renderer",
        choices=("gl", "webgpu"),
        action="append",
        help="limit iteration to one backend; default proves both",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    backends = tuple(dict.fromkeys(args.renderer or ("gl", "webgpu")))
    failures: list[str] = []
    results: dict[tuple[str, bool], Arm] = {}

    try:
        source_failures = prove_source_contract(RENDERER_SOURCE.read_text())
        if source_failures:
            raise ValueError(
                "renderer cutout classifier source contract failed: "
                + "; ".join(source_failures)
            )
        with tempfile.TemporaryDirectory(prefix="mdkr_texedge_") as temp:
            work = Path(temp)
            for backend in backends:
                for legacy in (False, True):
                    results[(backend, legacy)] = run_arm(
                        binary, rom, backend, legacy, work,
                        args.frames, args.timeout, args.verbose,
                    )
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"check_texture_edge_classification: FAIL — {exc}", file=sys.stderr)
        return 1

    baseline = next(iter(results.values())).pace
    for (backend, legacy), arm in results.items():
        label = f"{backend}-{'legacy' if legacy else 'fixed'}"
        if arm.pace != baseline:
            failures.append(f"{label}: [PACE] stream diverged")
        if not legacy and scene_colours(arm.image) < 200:
            failures.append(f"{label}: captured frame is flat/incomplete")

    summaries: list[str] = []
    for backend in backends:
        try:
            changed, peak = difference(
                results[(backend, False)].image,
                results[(backend, True)].image,
            )
        except ValueError as exc:
            failures.append(f"{backend}: {exc}")
            continue
        pixels = (
            results[(backend, False)].image.width *
            results[(backend, False)].image.height
        )
        if changed < MIN_CHANGED or peak < MIN_MAX_DELTA:
            failures.append(
                f"{backend}: legacy positive control was inert "
                f"({changed}/{pixels} pixels, peak delta {peak})"
            )
        elif changed > pixels * MAX_CHANGED_FRACTION:
            failures.append(
                f"{backend}: reclassification is over-broad "
                f"({changed}/{pixels} pixels); only the authored "
                f"XLU_LINE_MOD blend should move"
            )
        summaries.append(
            f"{backend} {changed}/{pixels} pixels (peak delta {peak})"
        )

    if failures:
        print("check_texture_edge_classification: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        "check_texture_edge_classification: PASS — cutout/blend A/B "
        + "; ".join(summaries)
        + f"; {len(baseline)} [PACE] rows identical"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
