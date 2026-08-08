#!/usr/bin/env python3
"""Gate the outline redraw of DKR's two plain lettering faces.

For GL and WebGPU this check captures one stable copyright-text frame in six
arms: Pure/Restored/Remastered, each with the production path and with
``MDKR_HIRES_TEXT=0``. It proves:

* Pure performs zero outline work and is pixel-identical to control **even
  with MDKR_HIRES_TEXT=1 forced on** -- a preset default is outranked by an
  ini value, this variable, or a CLI pair, so byte-exactness has to come
  from a mode guard and this arm is what proves it;
* Restored AND Remastered both redraw, which is the point of the feature: this
  is not a look-changing effect, it is the same lettering at a resolution the
  11-pixel source could not carry;
* every changed pixel stays inside the known text rectangle, which is the
  atlas-bleed guard -- a texrect samples exactly one glyph cell, so a write
  outside a registered cell would show up as a neighbouring glyph changing;
* font registry operations and texture-cache verification report zero failures,
  so the added face classification and character-keyed regions did not break
  registration or let a stale atlas survive a mode change;
* no glyph is clipped by the per-cell backstop on any arm, which proves the
  bounded-stretch fit holds for all 94 authored glyphs at their real sizes --
  the synthetic CTest can only reach a handful;
* all twelve normalized [PACE] streams are identical, so redrawing glyphs costs
  no timing.

check_font_sdf.py owns the SDF contour pass and pins this path off.
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
FONT_RE = re.compile(
    r"\[FONT\] sdfUploads=(\d+) outlineUploads=(\d+) registryFailures=(\d+) "
    r"clippedTexels=(\d+)")
CACHE_RE = re.compile(r"\[TEXCACHE\] staleHits=(\d+)")
FATAL_RE = fatal_re(*ASSERT_MARKERS, *FX_MARKERS)

# Pure passes on pixel EQUALITY, which a frame that drew nothing also satisfies.
# Same floor and rationale as check_font_sdf.py.
MIN_FRAME_COLOURS = 64
MIN_FRAME_SIGMA = 3.0

# The redraw must actually move a visible amount of the text.
MIN_CHANGED_PIXELS = 500


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
    outline_uploads: int
    registry_failures: int
    clipped_texels: int
    stale_hits: int
    pace: tuple[str, ...]
    image: Image


def read_ppm(path: Path) -> Image:
    return Image(*read_ppm_bytes(path))


def normalized_pace(output: str) -> tuple[str, ...]:
    """[PACE] rows with the wall-clock field blanked, as check_font_sdf does.

    The rows carry a log prefix, so match on the marker rather than the start
    of the line, and normalise dtms out before comparing arms.
    """
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
    # Always set the key explicitly, in both directions. Pure's preset is
    # already 0, so only setting it when disabled would make the two Pure arms
    # byte-identical configurations and the Pure comparison a run against
    # itself. Forcing it to 1 in Pure is the case that matters: an ini value,
    # this variable, or a CLI pair all outrank a preset default, so Pure's
    # byte-exactness has to come from a mode guard, not from the preset.
    env["MDKR_HIRES_TEXT"] = "0" if disabled else "1"
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
        "--headless-frames", str(frames),
        "--input-script", str(SCRIPT),
        "--dump-frames", str(dump_dir),
        "--rom", str(rom),
        f"--{mode}",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command, cwd=run_dir, env=environment(backend, disabled),
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"{label}: timed out\n{(exc.stdout or '')[-4000:]}") from exc

    output = proc.stdout
    fatal = FATAL_RE.search(output)
    font_matches = list(FONT_RE.finditer(output))
    cache_matches = list(CACHE_RE.finditer(output))
    dumps = sorted(dump_dir.glob("*.ppm"))

    if (proc.returncode != 0 or fatal is not None or len(font_matches) != 1 or
            len(cache_matches) != 1 or not dumps):
        raise RuntimeError(
            f"{label}: rc={proc.returncode}, "
            f"fatal={fatal.group(0) if fatal else None}, "
            f"fontTelemetry={len(font_matches)}, "
            f"cacheTelemetry={len(cache_matches)}, frames={len(dumps)}\n"
            f"{output[-4000:]}"
        )

    font = font_matches[0]
    pace = normalized_pace(output)
    if not pace:
        raise RuntimeError(f"{label}: no [PACE] stream")
    return Arm(
        backend=backend,
        mode=mode,
        disabled=disabled,
        outline_uploads=int(font.group(2)),
        registry_failures=int(font.group(3)),
        clipped_texels=int(font.group(4)),
        stale_hits=int(cache_matches[0].group(1)),
        pace=pace,
        image=read_ppm(dumps[0]),
    )


def frame_metrics(image: Image) -> tuple[int, float]:
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
        "--renderer", choices=("gl", "webgpu"), action="append",
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
        with tempfile.TemporaryDirectory(prefix="mdkr_font_outline_") as temp:
            work = Path(temp)
            for backend in backends:
                for mode in ("pure", "restored", "remastered"):
                    for disabled in (False, True):
                        results[(backend, mode, disabled)] = run_arm(
                            binary, rom, backend, mode, disabled, work,
                            args.frames, args.timeout, args.verbose,
                        )
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"check_font_outline: FAIL — {exc}", file=sys.stderr)
        return 1

    pace_baseline = next(iter(results.values())).pace
    summaries: list[str] = []
    for arm in results.values():
        label = f"{arm.backend}-{arm.mode}-{'off' if arm.disabled else 'on'}"
        if arm.registry_failures != 0:
            failures.append(
                f"{label}: {arm.registry_failures} font registry failure(s)")
        if arm.stale_hits != 0:
            failures.append(
                f"{label}: {arm.stale_hits} stale texture-cache hit(s)")
        if arm.pace != pace_baseline:
            failures.append(f"{label}: [PACE] diverged")
        # The per-cell backstop is a safety net, not a policy: the fit is meant
        # to keep every glyph inside its own cell. Anything it trimmed is a
        # clipped letter. Asserting it here proves the fit holds across all 94
        # authored glyphs at their real sizes, which the synthetic unit test
        # cannot reach.
        if arm.clipped_texels != 0:
            failures.append(
                f"{label}: {arm.clipped_texels} glyph texel(s) clipped by the "
                "per-cell backstop")
        colours, sigma = frame_metrics(arm.image)
        if colours < MIN_FRAME_COLOURS or sigma < MIN_FRAME_SIGMA:
            failures.append(
                f"{label}: capture is blank or near-uniform "
                f"(colours={colours}, luma sigma={sigma:.1f})")

    for backend in backends:
        # Pure must refuse the redraw even when the key is forced on, and the
        # forced-on capture must be pixel-identical to the forced-off one.
        enabled = results[(backend, "pure", False)]
        disabled = results[(backend, "pure", True)]
        if enabled.outline_uploads != 0 or disabled.outline_uploads != 0:
            failures.append(
                f"{backend}-pure: outline redraw reached Pure "
                f"(on={enabled.outline_uploads}, off={disabled.outline_uploads})")
        if enabled.image.pixels != disabled.image.pixels:
            failures.append(
                f"{backend}-pure: MDKR_HIRES_TEXT=1 changed protected pixels")

        for mode in ("restored", "remastered"):
            enabled = results[(backend, mode, False)]
            disabled = results[(backend, mode, True)]
            if enabled.outline_uploads <= 0:
                failures.append(f"{backend}-{mode}: no outline atlas upload")
            if disabled.outline_uploads != 0:
                failures.append(f"{backend}-{mode}-off: redraw still ran")
            try:
                changed = changed_pixels(enabled.image, disabled.image)
            except ValueError as exc:
                failures.append(f"{backend}-{mode}: {exc}")
                continue
            if len(changed) < MIN_CHANGED_PIXELS:
                failures.append(
                    f"{backend}-{mode}: positive control nearly inert "
                    f"({len(changed)} changed pixels)")
                continue
            # This fixture has exactly one text pass: the copyright line. A
            # change anywhere else means a glyph escaped its own cell.
            x0 = int(enabled.image.width * 0.25)
            x1 = int(enabled.image.width * 0.75)
            y0 = int(enabled.image.height * 0.80)
            y1 = int(enabled.image.height * 0.93)
            outside = sum(
                not (x0 <= x < x1 and y0 <= y < y1) for x, y in changed)
            if outside != 0:
                failures.append(
                    f"{backend}-{mode}: {outside} changed pixel(s) outside "
                    "the text rectangle (atlas bleed)")
            summaries.append(
                f"{backend}-{mode} uploads={enabled.outline_uploads}, "
                f"textPixels={len(changed)}")

    if failures:
        print("check_font_outline: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        "check_font_outline: PASS — "
        + "; ".join(summaries)
        + "; Pure pixel-identical; "
        + f"{len(pace_baseline)} [PACE] rows identical"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
