#!/usr/bin/env python3
"""Replay gate for projected kart shadows at 60 Hz presentation.

This is deliberately a two-arm, self-contained visual check rather than a
golden-image test.  It drives the Ancient Lake race (track 5, Diddy car) in
Restored presentation with world shadows off, captures the last 200 presents,
and compares normal projected-shadow replay against the token-gated vertex-lerp
regression.

The control may only change presentation.  Therefore all authoritative
SIMHASH, EVENTHASH and INPUTHASH rows, and each captured authored endpoint,
must be exact.  The odd presents are true midpoints.  The A/B difference
localizes the kart-shadow footprint without assuming a track texture or a
specific GPU's RGB rounding.  Within that footprint we measure the temporal
second difference of each arm.  The bad vertex lerp creates a substantially
larger residual; the healthy alpha-coherent projection stays below conservative
bounds measured from the final Ancient Lake evidence.

Usage:
    python3 tests/check_presentation_shadows.py --build build-rel \
        --rom /path/to/us-rev1.z64 -v
"""

from __future__ import annotations

import argparse
from collections import OrderedDict
import os
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (DEFAULT_BUILD_DIR, DEVICE_MARKERS, fatal_re,
                           read_ppm, resolve_binary)


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
PRESENTS = 6600
CAPTURE_FROM = 6400
CAPTURE_TO = 6599
EXPECTED_TICKS = 3300
TOKEN = "mdkr64-presentation-replay-v1"
ENDPOINT_RE = re.compile(
    r"^\[PRESENT-ENDPOINT\] frame=(\d+) authored=(\d+) source=(real|replay)$",
    re.MULTILINE,
)
PACKET_RE = re.compile(r"^\[PRESENT-PACKET\] (.*)$", re.MULTILINE)
FATAL_RE = fatal_re(*DEVICE_MARKERS, ignore_case=True)

# A midpoint is usable once the A/B shadow-support mask has at least this many
# pixels.  The final evidence has 97 such triples; the two tiny, nearly static
# support masks are intentionally excluded instead of letting quantization
# dominate a geometric measurement.
MIN_SHADOW_SUPPORT = 250
ROI_MARGIN = 3

# Conservative bounds are calibrated from the exact Restored, world-shadow-off
# evidence.  They deliberately leave room for backend RGB rounding.
PRODUCTION_RESIDUAL_MEDIAN_MAX = 75.0
PRODUCTION_RESIDUAL_P95_MAX = 125.0
CONTROL_RATIO_MEDIAN_MIN = 1.20
CONTROL_RATIO_P95_MIN = 2.50
CONTROL_SEVERE_RATIO = 2.0
CONTROL_SEVERE_MIN = 30


@dataclass(frozen=True)
class Arm:
    label: str
    output: str
    frame_dir: Path


@dataclass(frozen=True)
class VisualMetrics:
    triples: int
    support_median: float
    production_median: float
    production_p95: float
    control_median: float
    control_p95: float
    ratio_median: float
    ratio_p95: float
    severe_control: int


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def percentile(values: list[float], fraction: float) -> float:
    """Linear percentile with no NumPy/Pillow dependency."""
    require(values, "cannot calculate a percentile of no values")
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def clean_environment() -> dict[str, str]:
    return {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }


def run_arm(binary: Path, rom: Path, root: Path, backend: str, label: str,
            mode: str, timeout: int, verbose: bool, traced: bool = True) -> Arm:
    run_dir = root / label
    frame_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frame_dir.mkdir(parents=True)
    save_dir.mkdir()
    env = clean_environment()
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_EVENT_HASH="1",
        MDKR_INPUT_HASH="1",
        MDKR_STATE_HASH="3",
        MDKR_LOAD_TRACK="5",
        MDKR_PRESENT_RATE="60",
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_RENDERER=backend,
        MDKR_SAVE_DIR=str(save_dir),
        # Isolate the video config with the save (see check_door_blocks.py).
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
        MDKR_SYNTH_FIELDS="1",
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
        MDKR_WORLD_SHADOW="off",
        MDKR_DUMP_FROM=str(CAPTURE_FROM),
        MDKR_DUMP_EVERY="1",
        MDKR64_HIDDEN="1",
    )
    # Every authority stream this check reads ([SIMHASH]/[EVENTHASH]/
    # [INPUTHASH]/[PRESENT-SCHED]) has its own variable, so MDKR_TRACE is not
    # load-bearing here -- and it arms engine behaviour of its own
    # (mdkr_resource_trace_enabled(); wave "shadowdeep" R1 in
    # docs/open-items/renderer.md). The shipping arm in main() runs the
    # production mode without it and requires identical frames.
    if traced:
        env["MDKR_TRACE"] = "1"
    if mode != "production":
        env.update(
            MDKR_INTERNAL_TEST_TOKEN=TOKEN,
        )
    if mode == "vertex-lerp":
        env["MDKR_TEST_PROJECTED_SHADOW_VERTEX_LERP"] = "1"
    elif mode == "hold":
        env["MDKR_TEST_PROJECTED_SHADOW_INTERPOLATION"] = "off"
    command = [
        str(binary), "--headless-frames", str(PRESENTS),
        "--input-script", str(SCRIPT),
        "--dump-frames", str(frame_dir),
        "--rom", str(rom), "--window-size", "640x480", "--restored",
    ]
    if verbose:
        print(f"$ ({label}) {shlex.join(command)}", flush=True)
    try:
        process = subprocess.run(
            command, cwd=run_dir, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        raise RuntimeError(
            f"{label}: timed out after {timeout}s\n{output[-5000:]}"
        ) from error
    output = process.stdout or ""
    fatal = FATAL_RE.search(output)
    require(
        process.returncode == 0 and fatal is None,
        f"{label}: exit={process.returncode}, fatal="
        f"{fatal.group(0) if fatal else 'none'}\n{output[-5000:]}",
    )
    return Arm(label, output, frame_dir)


def authority_stream(output: str, marker: str) -> list[str]:
    rows = [line for line in output.splitlines() if line.startswith(marker)]
    require(
        len(rows) == EXPECTED_TICKS,
        f"{marker}: got {len(rows)} rows, expected {EXPECTED_TICKS}",
    )
    return rows


def packet_fields(output: str, label: str) -> dict[str, int]:
    matches = list(PACKET_RE.finditer(output))
    require(matches, f"{label}: missing [PRESENT-PACKET] telemetry")
    fields: dict[str, int] = {}
    for token in matches[-1].group(1).split():
        key, separator, value = token.partition("=")
        require(separator == "=" and value.isdigit(),
                f"{label}: malformed packet token {token!r}")
        fields[key] = int(value)
    return fields


def endpoint_rows(output: str, label: str) -> dict[int, tuple[int, str]]:
    rows: dict[int, tuple[int, str]] = {}
    for match in ENDPOINT_RE.finditer(output):
        frame = int(match.group(1))
        require(frame not in rows, f"{label}: duplicate endpoint frame {frame}")
        rows[frame] = (int(match.group(2)), match.group(3))
    return rows


def first_difference(left: list[str], right: list[str]) -> int | str:
    for index, (left_row, right_row) in enumerate(zip(left, right)):
        if left_row != right_row:
            return index
    return "stream length"


def frames(arm: Arm) -> dict[int, Path]:
    result: dict[int, Path] = {}
    for path in arm.frame_dir.glob("frame_*.ppm"):
        try:
            frame = int(path.stem.split("_", 1)[1])
        except ValueError as error:
            raise RuntimeError(f"{arm.label}: unexpected frame name {path.name}") from error
        require(frame not in result, f"{arm.label}: duplicate dumped frame {frame}")
        result[frame] = path
    expected = set(range(CAPTURE_FROM, CAPTURE_TO + 1))
    require(set(result) == expected,
            f"{arm.label}: dumped frames differ from {CAPTURE_FROM}..{CAPTURE_TO} "
            f"(got {len(result)})")
    return result


def support_bbox(left: bytes, right: bytes, width: int, height: int) -> tuple[int, int, int, int, int]:
    """Count and bound RGB-different pixels in an A/B midpoint pair."""
    count = 0
    min_x, min_y, max_x, max_y = width, height, -1, -1
    for byte_index in range(0, len(left), 3):
        if left[byte_index:byte_index + 3] == right[byte_index:byte_index + 3]:
            continue
        pixel = byte_index // 3
        x, y = pixel % width, pixel // width
        count += 1
        min_x, min_y = min(min_x, x), min(min_y, y)
        max_x, max_y = max(max_x, x), max(max_y, y)
    return count, min_x, min_y, max_x, max_y


def temporal_residual(previous: bytes, midpoint: bytes, following: bytes,
                      width: int, box: tuple[int, int, int, int]) -> float:
    """Mean RGB L1 second-difference in a localized shadow footprint."""
    left, top, right, bottom = box
    total = 0
    pixels = 0
    for y in range(top, bottom + 1):
        start = (y * width + left) * 3
        stop = (y * width + right + 1) * 3
        for offset in range(start, stop):
            total += abs(2 * midpoint[offset] - previous[offset] - following[offset])
        pixels += right - left + 1
    return total / pixels


def visual_metrics(production: Arm, control: Arm,
                   production_frames: dict[int, Path],
                   control_frames: dict[int, Path]) -> VisualMetrics:
    # The retained window spans only one midpoint and its two endpoints per
    # arm. Do not retain the full high-DPI capture: 200 640x480 logical PPMs
    # can exceed a GiB on a Retina drawable and turn a release gate into a
    # memory-pressure test instead of a visual regression.
    cache: OrderedDict[tuple[str, int], tuple[int, int, bytes]] = OrderedDict()
    cache_limit = 12

    def image(arm: Arm, number: int) -> tuple[int, int, bytes]:
        key = (arm.label, number)
        cached = cache.pop(key, None)
        if cached is not None:
            cache[key] = cached
            return cached
        loaded = read_ppm(
            (production_frames if arm is production else control_frames)[number])
        cache[key] = loaded
        if len(cache) > cache_limit:
            cache.popitem(last=False)
        return loaded

    supports: list[float] = []
    production_values: list[float] = []
    control_values: list[float] = []
    ratios: list[float] = []
    for midpoint_number in range(CAPTURE_FROM + 1, CAPTURE_TO, 2):
        width, height, prod_midpoint = image(production, midpoint_number)
        control_width, control_height, control_midpoint = image(control, midpoint_number)
        require((width, height) == (control_width, control_height),
                f"frame {midpoint_number}: A/B dimensions differ")
        support, min_x, min_y, max_x, max_y = support_bbox(
            prod_midpoint, control_midpoint, width, height)
        if support < MIN_SHADOW_SUPPORT:
            continue
        box = (
            max(0, min_x - ROI_MARGIN), max(0, min_y - ROI_MARGIN),
            min(width - 1, max_x + ROI_MARGIN), min(height - 1, max_y + ROI_MARGIN),
        )
        _, _, prod_previous = image(production, midpoint_number - 1)
        _, _, prod_following = image(production, midpoint_number + 1)
        _, _, control_previous = image(control, midpoint_number - 1)
        _, _, control_following = image(control, midpoint_number + 1)
        production_value = temporal_residual(
            prod_previous, prod_midpoint, prod_following, width, box)
        control_value = temporal_residual(
            control_previous, control_midpoint, control_following, width, box)
        supports.append(float(support))
        production_values.append(production_value)
        control_values.append(control_value)
        ratios.append(control_value / max(production_value, 0.001))

    require(len(production_values) >= 95,
            f"only {len(production_values)} valid midpoint triples; expected at least 95")
    return VisualMetrics(
        triples=len(production_values),
        support_median=percentile(supports, 0.50),
        production_median=percentile(production_values, 0.50),
        production_p95=percentile(production_values, 0.95),
        control_median=percentile(control_values, 0.50),
        control_p95=percentile(control_values, 0.95),
        ratio_median=percentile(ratios, 0.50),
        ratio_p95=percentile(ratios, 0.95),
        severe_control=sum(value > CONTROL_SEVERE_RATIO for value in ratios),
    )


def hold_motion_witness(production: Arm, hold: Arm,
                        production_frames: dict[int, Path],
                        hold_frames: dict[int, Path]) -> tuple[int, float]:
    """Prove the rigid production footprint reaches rasterization at midpoints.

    A held authored decal can be quieter than a moving one, so this is not an
    image-quality metric. It is a direct, localized witness that production
    differs from the token-gated hold only on true interpolated presents.
    """
    supports: list[float] = []
    for number in range(CAPTURE_FROM + 1, CAPTURE_TO, 2):
        width, height, production_pixels = read_ppm(production_frames[number])
        hold_width, hold_height, hold_pixels = read_ppm(hold_frames[number])
        require((width, height) == (hold_width, hold_height),
                f"frame {number}: production/hold dimensions differ")
        support, _, _, _, _ = support_bbox(
            production_pixels, hold_pixels, width, height)
        if support >= MIN_SHADOW_SUPPORT:
            supports.append(float(support))
    require(len(supports) >= 95,
            f"only {len(supports)} production/hold midpoint footprints; "
            "projected shadow did not visibly advance at presentation rate")
    return len(supports), percentile(supports, 0.50)


def verify(production: Arm, control: Arm, hold: Arm) -> tuple[VisualMetrics, int, float]:
    for marker in ("[SIMHASH]", "[EVENTHASH]", "[INPUTHASH]"):
        left = authority_stream(production.output, marker)
        right = authority_stream(control.output, marker)
        require(left == right,
                f"authority changed in {marker}; first A/B difference is "
                f"{first_difference(left, right)}")
        require(left == authority_stream(hold.output, marker),
                f"hold changed authority in {marker}; first difference is "
                f"{first_difference(left, authority_stream(hold.output, marker))}")

    packet = packet_fields(production.output, production.label)
    require(packet.get("projectedshadowvertexreg", 0) > 0,
            "production: no projected-shadow vertex registrations")
    require(packet.get("projectedshadowdeformhit", 0) > 0,
            "production: no projected-shadow deformation hits")
    require(packet.get("projectedshadowdeformoverride", 0) > 0,
            "production: no projected-shadow deformation overrides")
    require(packet.get("projectedshadowprimalphahit", 0) > 0,
            "production: projected-shadow primitive-alpha owner was never "
            "bound during replay")
    require(packet.get("deformcollision", -1) == 0,
            f"production: deformation collisions={packet.get('deformcollision')}")

    # Task 9: the X/Z nearest-slot reorder guard for regenerated shadow
    # decals (gfx_presentation_packet_deformation_reordered) only has
    # anything to check when the per-vertex lerp path is armed -- the
    # shipped rigid-translation default (exercised by this file's
    # "production" arm) never reads a previous tick's vertex bytes at all,
    # so index correspondence cannot matter to it.
    #
    # "production" must show shadowreordercheck=0: not because the route
    # lacks shadows (projectedshadow* above already proves it does not) but
    # because the guard itself never runs outside the token-gated lerp mode.
    require(packet.get("shadowreordercheck", -1) == 0,
            "production: the shadow reorder guard ran outside its "
            f"token-gated vertex-lerp mode (shadowreordercheck="
            f"{packet.get('shadowreordercheck')}); it must stay fully inert "
            "for the shipped rigid-translation default")

    # An initial version of this fixture asserted shadowreordermismatch=0 on
    # this route, expecting a well-behaved TT lap to never trip the guard.
    # Measured instead: 617 of 22456 checks (~2.7%) DO reorder on real
    # Ancient Lake gameplay -- the assertion, not the guard, was wrong. A
    # kart's shadow decal is rebuilt from whichever ground triangles it is
    # currently touching, and which triangles those are (and the order
    # shadow_generate() walks them in) can legitimately change as the kart
    # crosses a terrain triangle boundary between two adjacent ticks, even
    # though the decal's vertex/triangle COUNT stays the same -- exactly the
    # "same topology, reordered authoring" case this guard exists for. The
    # nearest-neighbour heuristic cannot distinguish that from a decal that
    # simply moved farther than half its own inter-vertex spacing in one
    # tick; both are graded the same way here, and both get the same
    # response: hold on the authored current mesh rather than guess a
    # correspondence, which is the safe side of the ambiguity regardless of
    # which case actually happened. Requiring zero would only prove this one
    # route's frame count never touched a boundary tick, not that the guard
    # is correct -- exactly the kind of measurement Task 9's own brief warns
    # against trusting without checking it against the tree.
    #
    # What IS provable here: the guard actually ran (checks>0) and it
    # discriminates rather than firing on everything (0 < mismatches <
    # checks) -- a detector stubbed to always answer "reordered" would fail
    # the upper bound, and one stubbed to always answer "not reordered"
    # (silently inert) would fail the lower bound and packet_fields'
    # ">0" floor above. The exact catch (a synthetic, known two-slot swap)
    # is proven precisely, and mutation-tested, at the packet layer in
    # test_presentation_packet.c's check_shadow_reorder_detection.
    control_packet = packet_fields(control.output, control.label)
    control_checks = control_packet.get("shadowreordercheck", 0)
    control_mismatches = control_packet.get("shadowreordermismatch", -1)
    require(control_checks > 0,
            "vertex-lerp control: the shadow reorder guard never ran "
            "(shadowreordercheck=0) -- Task 9's wiring is not reaching the "
            "choke point in this mode")
    require(0 < control_mismatches < control_checks,
            "vertex-lerp control: shadow reorder mismatches "
            f"({control_mismatches}) are not a proper, nonempty subset of "
            f"checks ({control_checks}) -- either the guard never actually "
            "caught a reorder on this route (inert/stubbed), is firing on "
            "every graded tick (indiscriminate), or the counters disagree")

    production_frames = frames(production)
    control_frames = frames(control)
    hold_frames = frames(hold)
    production_endpoints = endpoint_rows(production.output, production.label)
    control_endpoints = endpoint_rows(control.output, control.label)
    hold_endpoints = endpoint_rows(hold.output, hold.label)
    exact_endpoints = 0
    for number in range(CAPTURE_FROM, CAPTURE_TO + 1, 2):
        require(number in production_endpoints and number in control_endpoints,
                f"frame {number}: missing authored endpoint telemetry")
        require(production_endpoints[number][1] == "real" and
                control_endpoints[number][1] == "real",
                f"frame {number}: endpoint was not authored real data")
        require(production_endpoints[number] == control_endpoints[number],
                f"frame {number}: A/B endpoint metadata differs "
                f"({production_endpoints[number]} != {control_endpoints[number]})")
        require(production_endpoints[number] == hold_endpoints.get(number),
                f"frame {number}: hold endpoint metadata differs")
        require(production_frames[number].read_bytes() == control_frames[number].read_bytes(),
                f"frame {number}: authored endpoint pixels differ between arms")
        require(production_frames[number].read_bytes() == hold_frames[number].read_bytes(),
                f"frame {number}: hold changed authored endpoint pixels")
        exact_endpoints += 1
    require(exact_endpoints == 100,
            f"only {exact_endpoints}/100 authored endpoints were exact")

    metrics = visual_metrics(production, control, production_frames, control_frames)
    require(metrics.production_median < PRODUCTION_RESIDUAL_MEDIAN_MAX and
            metrics.production_p95 < PRODUCTION_RESIDUAL_P95_MAX,
            "production: localized temporal shadow residual escaped the "
            f"conservative bounds (median/p95 {metrics.production_median:.2f}/"
            f"{metrics.production_p95:.2f}, caps "
            f"{PRODUCTION_RESIDUAL_MEDIAN_MAX:.2f}/"
            f"{PRODUCTION_RESIDUAL_P95_MAX:.2f})")
    require(metrics.ratio_median > CONTROL_RATIO_MEDIAN_MIN and
            metrics.ratio_p95 > CONTROL_RATIO_P95_MIN and
            metrics.severe_control >= CONTROL_SEVERE_MIN,
            "negative control did not materially regress the localized shadow "
            f"walk (control/production median/p95 "
            f"{metrics.ratio_median:.2f}/{metrics.ratio_p95:.2f}, severe "
            f"{metrics.severe_control}; minima "
            f"{CONTROL_RATIO_MEDIAN_MIN:.2f}/{CONTROL_RATIO_P95_MIN:.2f}/"
            f"{CONTROL_SEVERE_MIN})")
    motion_triples, motion_support = hold_motion_witness(
        production, hold, production_frames, hold_frames)
    return metrics, motion_triples, motion_support


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR,
                        help="build directory or mdkr64 executable")
    parser.add_argument("--rom", required=True,
                        help="US Rev 1 ROM (us.v80)")
    parser.add_argument("--timeout", type=int, default=480,
                        help="seconds allowed for each arm (default: 480)")
    parser.add_argument("--backends", nargs="+", choices=("gl", "webgpu"),
                        default=("gl", "webgpu"),
                        help="renderer backends to qualify (default: gl webgpu)")
    parser.add_argument("--keep-artifacts", metavar="DIR",
                        help="write run logs and PPMs to an empty directory")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    missing = [path for path in (binary, rom, SCRIPT) if not path.is_file()]
    if missing:
        for path in missing:
            print(f"check_presentation_shadows: FAIL: missing {path}", file=sys.stderr)
        return 1
    if args.timeout <= 0:
        print("check_presentation_shadows: FAIL: --timeout must be positive", file=sys.stderr)
        return 1

    temporary: tempfile.TemporaryDirectory[str] | None = None
    if args.keep_artifacts:
        root = Path(args.keep_artifacts).expanduser().resolve()
        if root.exists():
            if not root.is_dir() or any(root.iterdir()):
                print("check_presentation_shadows: FAIL: --keep-artifacts must "
                      "name an empty directory", file=sys.stderr)
                return 1
        else:
            root.mkdir(parents=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="mdkr-presentation-shadows-")
        root = Path(temporary.name)

    try:
        notes: list[str] = []
        for backend in args.backends:
            production = run_arm(
                binary, rom, root, backend, f"{backend}-production", "production",
                args.timeout, args.verbose)
            control = run_arm(
                binary, rom, root, backend, f"{backend}-vertex-lerp-control",
                "vertex-lerp", args.timeout, args.verbose)
            hold = run_arm(
                binary, rom, root, backend, f"{backend}-hold-control", "hold",
                args.timeout, args.verbose)
            # Shipping configuration: the production arm again with MDKR_TRACE
            # absent, which is what the shipped launchers and the web shell
            # actually run. The assertion is on pixels -- if a diagnostic
            # variable reaches the frame buffer, every visual verdict above is
            # about a program nobody ships. This is what wave "shadowdeep" R1
            # needed and did not have.
            shipping = run_arm(
                binary, rom, root, backend, f"{backend}-production-shipping",
                "production", args.timeout, args.verbose, traced=False)
            require("[PACE]" not in shipping.output,
                    f"{shipping.label}: [PACE] rows present with MDKR_TRACE "
                    f"unset -- this arm is not in shipping configuration")
            traced_frames = sorted(production.frame_dir.glob("*.ppm"))
            ship_frames = sorted(shipping.frame_dir.glob("*.ppm"))
            require(
                bool(ship_frames) and len(ship_frames) == len(traced_frames),
                f"{shipping.label}: dumped {len(ship_frames)} frames against "
                f"{len(traced_frames)} traced")
            differing = [
                left.name for left, right in zip(traced_frames, ship_frames)
                if left.read_bytes() != right.read_bytes()]
            require(
                not differing,
                f"{backend}: MDKR_TRACE changes the rendered image -- "
                f"{len(differing)} of {len(traced_frames)} frames differ from "
                f"the same route in shipping configuration ({differing[0] if differing else ''}). "
                f"Find what the trace variable arms besides printing "
                f"(mdkr_resource_trace_enabled() is the known precedent)")
            (root / shipping.label / "run.log").write_text(shipping.output)
            (root / production.label / "run.log").write_text(production.output)
            (root / control.label / "run.log").write_text(control.output)
            (root / hold.label / "run.log").write_text(hold.output)
            metrics, motion_triples, motion_support = verify(production, control, hold)
            packet = packet_fields(production.output, production.label)
            control_packet = packet_fields(control.output, control.label)
            notes.append(
                f"{backend}: projected hits/overrides "
                f"{packet['projectedshadowdeformhit']}/"
                f"{packet['projectedshadowdeformoverride']}; "
                f"projected alpha hits "
                f"{packet['projectedshadowprimalphahit']}; "
                f"{metrics.triples} midpoint triples, residual median/p95 "
                f"{metrics.production_median:.2f}/{metrics.production_p95:.2f}, "
                f"control/production {metrics.ratio_median:.2f}/"
                f"{metrics.ratio_p95:.2f} (severe {metrics.severe_control}); "
                f"motion/hold {motion_triples} triples, support median "
                f"{motion_support:.0f}px; "
                f"shadow reorder checks (vertex-lerp control) "
                f"{control_packet.get('shadowreordercheck')}, mismatches "
                f"{control_packet.get('shadowreordermismatch')} "
                f"(production, unarmed: "
                f"{packet.get('shadowreordercheck')})")
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"check_presentation_shadows: FAIL\n  - {error}", file=sys.stderr)
        if args.keep_artifacts:
            print(f"  artifacts retained at {root}", file=sys.stderr)
        return 1
    finally:
        if temporary is not None:
            temporary.cleanup()

    print(
        "check_presentation_shadows: PASS -- authority streams exact "
        f"({EXPECTED_TICKS} SIM/EVENT/INPUT rows), 100/100 authored endpoints "
        "exact per backend"
    )
    for note in notes:
        print(f"  {note}")
    if args.keep_artifacts:
        print(f"artifacts retained at {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
