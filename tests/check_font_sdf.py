#!/usr/bin/env python3
"""Gate UI-1/UI-2 runtime-derived font coverage across modes and backends.

For GL and WebGPU this check captures one stable copyright-text frame in six
arms: Pure/Restored/Remastered, each with the production path and with
``MDKR_FONT_SDF=0``. It proves:

* Pure and Restored perform zero derivation and are pixel-identical to control;
* Remastered uploads derived atlases and differs materially from control;
* every changed pixel stays inside the known text rectangle (atlas-bleed guard);
* font registry operations and texture-cache verification report zero failures;
* all twelve normalized [PACE] streams are identical.
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
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_game_select.txt"
FONT_RE = re.compile(r"\[FONT\] sdfUploads=(\d+) registryFailures=(\d+)")
CACHE_RE = re.compile(r"\[TEXCACHE\] staleHits=(\d+)")
FATAL_RE = fatal_re(*ASSERT_MARKERS, *FX_MARKERS)

# Pure/Restored pass on pixel EQUALITY, which a frame that drew nothing also
# satisfies. The fixture is one dim copyright screen rather than a lit race, so
# the floor is set on distinct RGB triples and luma spread instead of
# check_renderer_backends' scene thresholds: every arm measured here carries at
# least 207 sampled colours and sigma 7.7.
MIN_FRAME_COLOURS = 64
MIN_FRAME_SIGMA = 3.0


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True)
class Arm:
    backend: str
    mode: str
    disabled: bool
    uploads: int
    registry_failures: int
    stale_hits: int
    pace: tuple[str, ...]
    image: Image


def read_ppm(path: Path) -> Image:
    return Image(*read_ppm_bytes(path))


def normalized_pace(output: str) -> tuple[str, ...]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker >= 0:
            rows.append(re.sub(r" dtms=\S+", " dtms=<wall>", line[marker:]))
    return tuple(rows)


def environment(backend: str, disabled: bool) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_TRACE="1",
        MDKR_RENDERER=backend,
        MDKR_TEXCACHE_VERIFY="1",
        MDKR_DUMP_FROM="1100",
        MDKR_DUMP_EVERY="999",
        MDKR64_HIDDEN="1",
        LC_ALL="C",
    )
    if disabled:
        env["MDKR_FONT_SDF"] = "0"
    return env


def run_arm(
    binary: Path, rom: Path, backend: str, mode: str, disabled: bool,
    work: Path, frames: int, timeout: int, verbose: bool,
) -> Arm:
    label = f"{backend}-{mode}-{'off' if disabled else 'on'}"
    run_dir = work / label
    dump_dir = run_dir / "frames"
    dump_dir.mkdir(parents=True)
    command = [
        str(binary),
        "--headless-frames",
        str(frames),
        "--input-script",
        str(SCRIPT),
        "--dump-frames",
        str(dump_dir),
        "--rom",
        str(rom),
        f"--{mode}",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=environment(backend, disabled),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"{label}: timed out\n{(exc.stdout or '')[-4000:]}") from exc

    output = proc.stdout
    fatal = FATAL_RE.search(output)
    font_matches = list(FONT_RE.finditer(output))
    cache_matches = list(CACHE_RE.finditer(output))
    dumps = sorted(dump_dir.glob("*.ppm"))
    if (proc.returncode != 0 or fatal is not None or len(font_matches) != 1 or
            len(cache_matches) != 1 or len(dumps) != 1):
        raise RuntimeError(
            f"{label}: exit={proc.returncode}, "
            f"fatal={fatal.group(0) if fatal else 'none'}, "
            f"fontTelemetry={len(font_matches)}, "
            f"cacheTelemetry={len(cache_matches)}, dumps={len(dumps)}\n"
            f"{output[-4000:]}"
        )
    pace = normalized_pace(output)
    if not pace:
        raise RuntimeError(f"{label}: no [PACE] stream")
    font = font_matches[0]
    return Arm(
        backend=backend,
        mode=mode,
        disabled=disabled,
        uploads=int(font.group(1)),
        registry_failures=int(font.group(2)),
        stale_hits=int(cache_matches[0].group(1)),
        pace=pace,
        image=read_ppm(dumps[0]),
    )


def frame_metrics(image: Image) -> tuple[int, float]:
    """Return distinct sampled colour count and luma sigma for a capture."""
    colours: set[bytes] = set()
    count = total = total_sq = 0
    for y in range(0, image.height, 3):
        row = y * image.width * 3
        for x in range(0, image.width, 3):
            offset = row + x * 3
            pixel = image.pixels[offset:offset + 3]
            colours.add(pixel)
            luma = (pixel[0] * 299 + pixel[1] * 587 + pixel[2] * 114) // 1000
            count += 1
            total += luma
            total_sq += luma * luma
    if count == 0:
        return 0, 0.0
    mean = total / count
    return len(colours), max(0.0, total_sq / count - mean * mean) ** 0.5


def changed_pixels(left: Image, right: Image) -> list[tuple[int, int]]:
    if (left.width, left.height) != (right.width, right.height):
        raise ValueError("A/B dimensions differ")
    changed: list[tuple[int, int]] = []
    for pixel in range(left.width * left.height):
        offset = pixel * 3
        if left.pixels[offset:offset + 3] != right.pixels[offset:offset + 3]:
            changed.append((pixel % left.width, pixel // left.width))
    return changed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=1200)
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument(
        "--renderer",
        choices=("gl", "webgpu"),
        action="append",
        help="limit iteration; default proves both shipped native backends",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    backends = tuple(dict.fromkeys(args.renderer or ("gl", "webgpu")))
    results: dict[tuple[str, str, bool], Arm] = {}
    failures: list[str] = []

    try:
        with tempfile.TemporaryDirectory(prefix="mdkr_font_sdf_") as temp:
            work = Path(temp)
            for backend in backends:
                for mode in ("pure", "restored", "remastered"):
                    for disabled in (False, True):
                        arm = run_arm(
                            binary, rom, backend, mode, disabled, work,
                            args.frames, args.timeout, args.verbose,
                        )
                        results[(backend, mode, disabled)] = arm
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"check_font_sdf: FAIL — {exc}", file=sys.stderr)
        return 1

    pace_baseline = next(iter(results.values())).pace
    summaries: list[str] = []
    for key, arm in results.items():
        label = f"{arm.backend}-{arm.mode}-{'off' if arm.disabled else 'on'}"
        if arm.registry_failures != 0:
            failures.append(
                f"{label}: {arm.registry_failures} font registry failure(s)"
            )
        if arm.stale_hits != 0:
            failures.append(
                f"{label}: {arm.stale_hits} stale texture-cache hit(s)"
            )
        if arm.pace != pace_baseline:
            failures.append(f"{label}: [PACE] diverged")
        colours, sigma = frame_metrics(arm.image)
        if colours < MIN_FRAME_COLOURS or sigma < MIN_FRAME_SIGMA:
            failures.append(
                f"{label}: capture is blank or near-uniform "
                f"(colours={colours}, luma sigma={sigma:.1f})"
            )

    for backend in backends:
        for mode in ("pure", "restored"):
            enabled = results[(backend, mode, False)]
            disabled = results[(backend, mode, True)]
            if enabled.uploads != 0 or disabled.uploads != 0:
                failures.append(
                    f"{backend}-{mode}: derivation reached protected mode"
                )
            if enabled.image.pixels != disabled.image.pixels:
                failures.append(
                    f"{backend}-{mode}: SDF toggle changed protected pixels"
                )

        enabled = results[(backend, "remastered", False)]
        disabled = results[(backend, "remastered", True)]
        if enabled.uploads <= 0:
            failures.append(f"{backend}-remastered: no derived atlas upload")
        if disabled.uploads != 0:
            failures.append(f"{backend}-remastered-off: derivation still ran")
        try:
            changed = changed_pixels(enabled.image, disabled.image)
        except ValueError as exc:
            failures.append(f"{backend}-remastered: {exc}")
            continue
        if len(changed) < 500:
            failures.append(
                f"{backend}-remastered: positive control nearly inert "
                f"({len(changed)} changed pixels)"
            )
        else:
            # This fixture has exactly one text pass: the copyright line.
            # A change elsewhere means the derived atlas escaped its glyph draw
            # or sampled a neighbouring atlas cell.
            x0 = int(enabled.image.width * 0.25)
            x1 = int(enabled.image.width * 0.75)
            y0 = int(enabled.image.height * 0.80)
            y1 = int(enabled.image.height * 0.93)
            outside = sum(
                not (x0 <= x < x1 and y0 <= y < y1)
                for x, y in changed
            )
            if outside != 0:
                failures.append(
                    f"{backend}-remastered: {outside} changed pixel(s) "
                    "outside the text rectangle (atlas bleed)"
                )
            summaries.append(
                f"{backend} uploads={enabled.uploads}, "
                f"textPixels={len(changed)}"
            )

    if failures:
        print("check_font_sdf: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        "check_font_sdf: PASS — "
        + "; ".join(summaries)
        + "; Pure/Restored pixel-identical; "
        + f"{len(pace_baseline)} [PACE] rows identical"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
