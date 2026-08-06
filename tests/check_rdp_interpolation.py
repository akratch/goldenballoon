#!/usr/bin/env python3
"""Prove screen-linear RDP shade/fog gradients on both shipped native backends.

Each backend drives the same deterministic Time Trial route twice:

* production: screen-linear shade/fog plus clipped-vertex fog recomputation;
* positive control: ``MDKR_RDP_GRADIENTS=legacy`` restores perspective GPU
  varyings and endpoint-byte fog interpolation.

The gameplay-state stream must remain byte-identical across every arm, both
production frames must contain a real scene, and the legacy control must cause a
material pixel change on both GL and WebGPU. The C unit test separately proves a
synthetic clipped edge where old fog interpolation yields 60 but recomputation
yields 28.
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

from harness_utils import read_ppm as read_ppm_bytes, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
RENDERER_SOURCE = ROOT / "platform" / "fast3d" / "gfx_pc_dkr.c"
REQUIRED_SOURCE_COUNTS = {
    "gfx_rdp_interpolation_options(": 1,
    "gfx_rdp_fog_from_clip(": 2,
    'getenv("MDKR_RDP_GRADIENTS")': 1,
}
FATAL_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion|\[FX BUG\]"
)


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
        MDKR_DUMP_FROM="2800",
        MDKR_DUMP_EVERY="999",
        MDKR64_HIDDEN="1",
        LC_ALL="C",
    )
    if legacy:
        env["MDKR_RDP_GRADIENTS"] = "legacy"
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
        raise RuntimeError(f"{label}: no [PACE] rows; route did not reach gameplay")
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


def difference(fixed: Image, legacy: Image) -> tuple[int, float]:
    if (fixed.width, fixed.height) != (legacy.width, legacy.height):
        raise ValueError(
            f"dimension mismatch {fixed.width}x{fixed.height} vs "
            f"{legacy.width}x{legacy.height}"
        )
    changed = sum(
        fixed.pixels[i:i + 3] != legacy.pixels[i:i + 3]
        for i in range(0, len(fixed.pixels), 3)
    )
    mad = sum(
        abs(a - b) for a, b in zip(fixed.pixels, legacy.pixels)
    ) / len(fixed.pixels)
    return changed, mad


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=2900)
    parser.add_argument("--timeout", type=int, default=120)
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
                "renderer interpolation source contract failed: "
                + "; ".join(source_failures)
            )
        with tempfile.TemporaryDirectory(prefix="mdkr_rdp_interp_") as temp:
            work = Path(temp)
            for backend in backends:
                for legacy in (False, True):
                    results[(backend, legacy)] = run_arm(
                        binary, rom, backend, legacy, work,
                        args.frames, args.timeout, args.verbose,
                    )
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"check_rdp_interpolation: FAIL — {exc}", file=sys.stderr)
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
            changed, mad = difference(
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
        # The production route changes shade across most of the road/terrain.
        # Keep the threshold deliberately loose enough for driver rasterization
        # differences while still rejecting an inert shader qualifier.
        if changed < pixels // 100 or mad < 0.1:
            failures.append(
                f"{backend}: legacy positive control was inert "
                f"({changed}/{pixels} pixels, MAD {mad:.3f})"
            )
        summaries.append(
            f"{backend} {changed}/{pixels} pixels (MAD {mad:.3f})"
        )

    if failures:
        print("check_rdp_interpolation: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        "check_rdp_interpolation: PASS — screen-linear fixed/legacy A/B "
        + "; ".join(summaries)
        + f"; {len(baseline)} [PACE] rows identical"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
