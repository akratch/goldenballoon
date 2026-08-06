#!/usr/bin/env python3
"""Pixel-level widescreen/FOV proportion gate for HUD and world billboards.

The ordinary projection tests prove that perspective geometry is isotropic, but
DKR's regular world sprites use a separate F3DDKR billboard path: the anchor is
projected normally and the sprite-local vertices are then added directly in clip
space. A fixed 4:3 compensation therefore stretched collectible balloons by
1.33x at 16:9 and 1.75x at 21:9 even while karts and track geometry were correct.

This check drives to two deterministic approach frames on Timber's Island that
contain the same golden balloon in two independent rendering paths:

  * the authored HUD glyph in the centered SAFE_2D region at frame 6300;
  * a collectible world billboard, closer and large enough for an honest
    pixel-proportion measurement, at frame 6410.

It captures seven arms at equal requested height:

  * stock 4:3;
  * production 16:10;
  * production 16:9;
  * production 21:9 with the default 104-degree horizontal cap;
  * a forced 4:3 presentation centered inside a 21:9 drawable;
  * production 16:9 with a 75-degree gameplay FOV and no cap;
  * 21:9 exact legacy stretching as the deliberate failing positive control.

The blue zigzag inside each balloon is segmented without Pillow or NumPy. Its
bounding-box aspect must remain stable in every production arm. Its size must
also stay authored at ordinary Hor+, change by the analytically expected focal
scale for the cap/FOV arms, and stretch by about 1.75x only in the legacy arm.
That last arm proves the image detector rejects the pre-fix behavior.

Usage:
    python3 tests/check_widescreen_proportions.py \
        --build build --rom baserom.us.v80.z64 -v

Use ``--renderer gl`` or ``--renderer webgpu`` to select a backend explicitly,
and ``--keep-frames DIR`` to retain the fourteen PPMs and seven logs.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import shlex
import subprocess
import sys
import tempfile
from contextlib import nullcontext
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (DEFAULT_BUILD_DIR, read_ppm as read_ppm_bytes,
                           resolve_binary)


REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "tests" / "input_scripts" / "adventure_hub_drive.txt"
HUD_CAPTURE_FRAME = 6300
WORLD_CAPTURE_FRAME = 6410
# Keep running after both sampled approach frames until the fixture physically
# collects balloon 10. This proves the world motif belongs to the intended
# object rather than similarly coloured scenery.
FRAMES = 6500
LOGICAL_ASPECT = 4.0 / 3.0
MAX_PROPORTION_ERROR = 0.08
MAX_SCALE_ERROR = 0.10
MIN_LEGACY_STRETCH = 1.55
MAX_LEGACY_STRETCH = 1.95

# Spawn -> a safe north-east waypoint -> two approach points -> balloon 10.
# The longer tail is unreachable in this short fixture but keeps this route
# useful for manual continuation through the Dino Domain lobby.
DRIVE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
    ":-3381,1946:-2519,1516:-1858,1099;12:E5:E0"
)

PACE_RE = re.compile(rf"\[PACE\] frame={WORLD_CAPTURE_FRAME} .*")
BALLOON_RE = re.compile(
    r"drive: level=0 step 3: balloon 10 COLLECTED \(total=(\d+)\) @frame~(\d+)"
)
FATAL_MARKERS = ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:")


@dataclass(frozen=True)
class Case:
    label: str
    size: str
    aspect: float
    arguments: tuple[str, ...]
    expected_world_scale: float
    legacy: bool = False


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True)
class Component:
    area: int
    x0: int
    y0: int
    x1: int
    y1: int

    @property
    def width(self) -> int:
        return self.x1 - self.x0 + 1

    @property
    def height(self) -> int:
        return self.y1 - self.y0 + 1

    @property
    def aspect(self) -> float:
        return self.width / self.height


@dataclass
class Result:
    case: Case
    output: str
    hud_image: Image | None
    world_image: Image | None
    hud: Component | None = None
    world: Component | None = None


CAP_21X9_SCALE = (
    math.tan(math.radians(30.0))
    * (21.0 / 9.0)
    / math.tan(math.radians(104.0 / 2.0))
)
FOV_75_SCALE = math.tan(math.radians(30.0)) / math.tan(math.radians(75.0 / 2.0))

CASES = (
    Case("4x3", "720x540", 4.0 / 3.0, (), 1.0),
    Case("16x10", "864x540", 16.0 / 10.0, (), 1.0),
    Case("16x9", "960x540", 16.0 / 9.0, (), 1.0),
    Case("21x9-cap104", "1260x540", 21.0 / 9.0, (), CAP_21X9_SCALE),
    Case(
        "21x9-forced4x3",
        "1260x540",
        21.0 / 9.0,
        ("--aspect", "4:3"),
        1.0,
    ),
    Case(
        "16x9-fov75",
        "960x540",
        16.0 / 9.0,
        ("--fov", "75", "--max-hfov", "off"),
        FOV_75_SCALE,
    ),
    Case(
        "21x9-legacy",
        "1260x540",
        21.0 / 9.0,
        ("--legacy-stretch",),
        1.0,
        legacy=True,
    ),
)


def billboard_source_contract() -> list[str]:
    """Reject a new post-projection billboard producer that bypasses correction.

    F3DDKR billboard mode is the unusual path that caused the asset stretch:
    local sprite offsets are added after the anchor projection. Every audited
    caller must continue to converge on camera.c's world or ortho builder. A new
    direct producer would need its own aspect/FOV analysis and pixel fixture.
    """
    failures: list[str] = []
    producers: list[tuple[Path, int]] = []
    for path in (REPO / "game" / "src").rglob("*.c"):
        text = path.read_text(encoding="utf-8", errors="replace")
        producers.extend(
            (path, match.start())
            for match in re.finditer(r"\bgDkrEnableBillboard\s*\(", text)
        )
    if len(producers) != 2 or any(path.name != "camera.c" for path, _ in producers):
        locations = [path.relative_to(REPO).as_posix() for path, _ in producers]
        failures.append(
            "billboard source census changed: expected exactly the audited world "
            f"and ortho producers in camera.c, found {locations}"
        )

    camera_source = (REPO / "game" / "src" / "camera.c").read_text(
        encoding="utf-8", errors="replace")
    world_start = camera_source.find("static s32 render_sprite_billboard_transform_impl")
    world_end = camera_source.find("static void render_ortho_triangle_image_transform_impl")
    world_source = camera_source[world_start:world_end]
    if (
        world_start < 0
        or world_end < 0
        or "mdkr_display_calculate_billboard_correction" not in world_source
        or "mdkr_display_apply_billboard_correction" not in world_source
    ):
        failures.append(
            "world billboard builder no longer applies the shared aspect/FOV correction"
        )

    renderer_source = (REPO / "platform" / "fast3d" / "gfx_pc_dkr.c").read_text(
        encoding="utf-8", errors="replace")
    if len(re.findall(r"\bif\s*\(\s*rsp\.billboard\s*\)", renderer_source)) != 1:
        failures.append(
            "F3DDKR billboard decode census changed; audit every post-projection offset path"
        )
    return failures


def clean_environment(renderer: str | None) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("MDKR")
    }
    env.update(
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_AUTOPILOT="1",
        MDKR_TRACE="1",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        MDKR_DUMP_FROM=str(HUD_CAPTURE_FRAME),
        MDKR_DUMP_EVERY=str(WORLD_CAPTURE_FRAME - HUD_CAPTURE_FRAME),
        MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
        LC_ALL="C",
    )
    if renderer:
        env["MDKR_RENDERER"] = renderer
    return env


def read_ppm(path: Path) -> Image:
    return Image(*read_ppm_bytes(path))


def run_case(
    binary: Path,
    rom: Path,
    case: Case,
    root: Path,
    renderer: str | None,
    timeout: int,
    verbose: bool,
) -> tuple[Result, list[str]]:
    failures: list[str] = []
    case_root = root / case.label
    frame_dir = case_root / "frames"
    run_dir = case_root / "run"
    frame_dir.mkdir(parents=True)
    run_dir.mkdir()
    command = [
        str(binary),
        "--headless-frames",
        str(FRAMES),
        "--window-size",
        case.size,
        *case.arguments,
        "--input-script",
        str(SCRIPT),
        "--dump-frames",
        str(frame_dir),
        "--rom",
        str(rom),
    ]
    if verbose:
        print(f"$ ({case.label}) " + shlex.join(command), flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=clean_environment(renderer),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        output = proc.stdout
        returncode = proc.returncode
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        returncode = 124
    (case_root / "run.log").write_text(output, encoding="utf-8")

    if returncode != 0:
        failures.append(f"{case.label}: exited {returncode}")
    for marker in FATAL_MARKERS:
        if marker in output:
            failures.append(f"{case.label}: output contains {marker!r}")
    if renderer and f"[mdkr64] renderer backend: {renderer}" not in output:
        failures.append(
            f"{case.label}: did not confirm requested {renderer} renderer"
        )

    frames = sorted(frame_dir.glob("frame_*.ppm"))
    expected_names = [
        f"frame_{HUD_CAPTURE_FRAME:04d}.ppm",
        f"frame_{WORLD_CAPTURE_FRAME:04d}.ppm",
    ]
    if [path.name for path in frames] != expected_names:
        failures.append(
            f"{case.label}: dumped {[path.name for path in frames]}, "
            f"expected exactly {expected_names}"
        )
        hud_image = None
        world_image = None
    else:
        try:
            hud_image = read_ppm(frames[0])
            world_image = read_ppm(frames[1])
        except ValueError as exc:
            failures.append(str(exc))
            hud_image = None
            world_image = None
    return Result(case, output, hud_image, world_image), failures


def blue_components(
    image: Image, bounds: tuple[int, int, int, int]
) -> list[Component]:
    x0 = max(0, min(image.width, bounds[0]))
    y0 = max(0, min(image.height, bounds[1]))
    x1 = max(x0, min(image.width, bounds[2]))
    y1 = max(y0, min(image.height, bounds[3]))
    blue: set[tuple[int, int]] = set()

    for y in range(y0, y1):
        row = y * image.width * 3
        for x in range(x0, x1):
            index = row + x * 3
            red, green, value = image.pixels[index:index + 3]
            if (
                value >= 90
                and value * 100 >= red * 150
                and value * 100 >= green * 150
            ):
                blue.add((x, y))

    components: list[Component] = []
    while blue:
        point = blue.pop()
        stack = [point]
        points = [point]
        while stack:
            x, y = stack.pop()
            for neighbour in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if neighbour in blue:
                    blue.remove(neighbour)
                    stack.append(neighbour)
                    points.append(neighbour)
        if len(points) >= 10:
            xs = [item[0] for item in points]
            ys = [item[1] for item in points]
            components.append(
                Component(len(points), min(xs), min(ys), max(xs), max(ys))
            )
    return components


def safe_rect(image: Image, legacy: bool) -> tuple[float, float, float, float]:
    if legacy:
        return 0.0, 0.0, float(image.width), float(image.height)
    if image.width / image.height > LOGICAL_ASPECT:
        height = float(image.height)
        width = height * LOGICAL_ASPECT
        return (image.width - width) * 0.5, 0.0, width, height
    width = float(image.width)
    height = width / LOGICAL_ASPECT
    return 0.0, (image.height - height) * 0.5, width, height


def hud_balloon_component(image: Image, legacy: bool) -> Component | None:
    safe_x, safe_y, safe_width, safe_height = safe_rect(image, legacy)
    bounds = (
        round(safe_x + (12.0 / 320.0) * safe_width),
        round(safe_y + (5.0 / 240.0) * safe_height),
        round(safe_x + (65.0 / 320.0) * safe_width),
        round(safe_y + (55.0 / 240.0) * safe_height),
    )
    # The Remastered native-resolution text path can place a larger, narrow
    # blue glyph component in this deliberately broad HUD crop.  The balloon's
    # authored blue zigzag is always wider than it is tall, including the
    # legacy-stretch positive control.  Select by that invariant before area
    # so sharper unrelated text cannot masquerade as the balloon.
    components = [
        component
        for component in blue_components(image, bounds)
        if component.aspect >= 1.5
    ]
    return max(components, key=lambda item: item.area, default=None)


def world_balloon_component(image: Image) -> Component | None:
    components = blue_components(
        image,
        (
            round(image.width * 0.30),
            round(image.height * 0.20),
            round(image.width * 0.70),
            round(image.height * 0.45),
        ),
    )
    candidates = []
    for component in components:
        centre_x = (component.x0 + component.x1) * 0.5 / image.width
        centre_y = (component.y0 + component.y1) * 0.5 / image.height
        if (
            # The restored retail vehicle-audio RNG path moves the approach
            # camera slightly; the intended motif now centres at x=0.524 in
            # the 4:3 and legacy arms. Keep a tight route-local window without
            # clipping that deterministic camera variation.
            0.45 <= centre_x <= 0.55
            and 0.23 <= centre_y <= 0.42
            and component.aspect >= 2.0
        ):
            candidates.append(component)
    return max(candidates, key=lambda item: item.area, default=None)


def normalized_pace(output: str) -> str | None:
    matches = PACE_RE.findall(output)
    if not matches:
        return None
    return re.sub(r" dtms=\S+", " dtms=<wall>", matches[-1])


def relative_error(actual: float, expected: float) -> float:
    return abs(actual / expected - 1.0)


def analyze_results(results: list[Result], verbose: bool) -> list[str]:
    failures: list[str] = []
    reference = next((item for item in results if item.case.label == "4x3"), None)

    for result in results:
        if result.hud_image is None or result.world_image is None:
            continue
        for sample_name, image in (
            ("HUD", result.hud_image),
            ("world", result.world_image),
        ):
            actual_aspect = image.width / image.height
            if abs(actual_aspect - result.case.aspect) > 0.005:
                failures.append(
                    f"{result.case.label}: {sample_name} image aspect "
                    f"{actual_aspect:.5f}, expected {result.case.aspect:.5f}"
                )
        result.hud = hud_balloon_component(
            result.hud_image, result.case.legacy
        )
        result.world = world_balloon_component(result.world_image)
        if result.hud is None:
            failures.append(f"{result.case.label}: HUD balloon motif not found")
        if result.world is None:
            failures.append(f"{result.case.label}: world balloon motif not found")

    if (
        reference is None
        or reference.hud_image is None
        or reference.world_image is None
        or reference.hud is None
        or reference.world is None
    ):
        failures.append("4x3 reference image or balloon metrics unavailable")
        return failures

    reference_pace = normalized_pace(reference.output)
    reference_balloon = BALLOON_RE.findall(reference.output)

    for result in results:
        pace = normalized_pace(result.output)
        balloon = BALLOON_RE.findall(result.output)
        if pace is None:
            failures.append(
                f"{result.case.label}: no frame-{WORLD_CAPTURE_FRAME} "
                "[PACE] state"
            )
        elif reference_pace is not None and pace != reference_pace:
            failures.append(
                f"{result.case.label}: capture state differs from 4x3\n"
                f"    4x3: {reference_pace}\n"
                f"    {result.case.label}: {pace}"
            )
        if not balloon:
            failures.append(
                f"{result.case.label}: route did not collect balloon 10"
            )
        elif reference_balloon and balloon[-1] != reference_balloon[-1]:
            failures.append(
                f"{result.case.label}: balloon collection {balloon[-1]} "
                f"differs from 4x3 {reference_balloon[-1]}"
            )

        if (
            result.hud_image is None
            or result.world_image is None
            or result.hud is None
            or result.world is None
        ):
            continue
        if result.hud_image.height != reference.hud_image.height:
            failures.append(
                f"{result.case.label}: HUD dump height "
                f"{result.hud_image.height}, 4x3 reference is "
                f"{reference.hud_image.height}"
            )
        if result.world_image.height != reference.world_image.height:
            failures.append(
                f"{result.case.label}: world dump height "
                f"{result.world_image.height}, 4x3 reference is "
                f"{reference.world_image.height}"
            )

        hud_aspect_scale = result.hud.aspect / reference.hud.aspect
        world_aspect_scale = result.world.aspect / reference.world.aspect
        hud_width_scale = result.hud.width / reference.hud.width
        hud_height_scale = result.hud.height / reference.hud.height
        world_width_scale = result.world.width / reference.world.width
        world_height_scale = result.world.height / reference.world.height

        if result.case.legacy:
            for name, value in (
                ("HUD motif aspect", hud_aspect_scale),
                ("world motif aspect", world_aspect_scale),
                ("HUD width", hud_width_scale),
                ("world width", world_width_scale),
            ):
                if not MIN_LEGACY_STRETCH <= value <= MAX_LEGACY_STRETCH:
                    failures.append(
                        f"{result.case.label}: {name} scale {value:.3f} "
                        f"does not demonstrate the expected 1.75x stretch"
                    )
            for name, value in (
                ("HUD height", hud_height_scale),
                ("world height", world_height_scale),
            ):
                if relative_error(value, 1.0) > MAX_SCALE_ERROR:
                    failures.append(
                        f"{result.case.label}: {name} scale {value:.3f}, "
                        "expected the authored vertical size"
                    )
            # Both-direction guard: the known-bad control must fail the same
            # production proportion criterion used below.
            if (
                relative_error(hud_aspect_scale, 1.0) <= MAX_PROPORTION_ERROR
                or relative_error(world_aspect_scale, 1.0)
                <= MAX_PROPORTION_ERROR
            ):
                failures.append(
                    f"{result.case.label}: legacy positive control would pass "
                    "the production proportion threshold"
                )
        else:
            for name, value in (
                ("HUD motif aspect", hud_aspect_scale),
                ("world motif aspect", world_aspect_scale),
            ):
                if relative_error(value, 1.0) > MAX_PROPORTION_ERROR:
                    failures.append(
                        f"{result.case.label}: {name} scale {value:.3f} "
                        f"exceeds {MAX_PROPORTION_ERROR:.0%} proportion tolerance"
                    )
            for name, value in (
                ("HUD width", hud_width_scale),
                ("HUD height", hud_height_scale),
            ):
                if relative_error(value, 1.0) > MAX_SCALE_ERROR:
                    failures.append(
                        f"{result.case.label}: {name} scale {value:.3f}, "
                        "expected authored SAFE_2D size"
                    )
            for name, value in (
                ("world width", world_width_scale),
                ("world height", world_height_scale),
            ):
                if (
                    relative_error(value, result.case.expected_world_scale)
                    > MAX_SCALE_ERROR
                ):
                    failures.append(
                        f"{result.case.label}: {name} scale {value:.3f}, "
                        f"expected focal scale "
                        f"{result.case.expected_world_scale:.3f}"
                    )

        if verbose:
            print(
                f"  {result.case.label:14s} "
                f"HUD={result.hud.width}x{result.hud.height} "
                f"aspectScale={hud_aspect_scale:.3f}; "
                f"world={result.world.width}x{result.world.height} "
                f"aspectScale={world_aspect_scale:.3f} "
                f"sizeScale={world_width_scale:.3f}x{world_height_scale:.3f} "
                f"expected={result.case.expected_world_scale:.3f}"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--renderer", choices=("gl", "webgpu"), default=None)
    parser.add_argument("--timeout", type=int, default=120, help="seconds per arm")
    parser.add_argument("--keep-frames", type=Path)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    if not binary.is_file():
        print(f"FAIL: binary not found: {binary}", file=sys.stderr)
        return 2
    if not rom.is_file():
        print(f"FAIL: ROM not found: {rom}", file=sys.stderr)
        return 2
    if args.timeout <= 0:
        print("FAIL: --timeout must be positive", file=sys.stderr)
        return 2

    if args.keep_frames:
        root = args.keep_frames.expanduser().resolve()
        try:
            root.mkdir(parents=True)
        except FileExistsError:
            print(f"FAIL: --keep-frames directory already exists: {root}", file=sys.stderr)
            return 2
        context = nullcontext(str(root))
    else:
        context = tempfile.TemporaryDirectory(prefix="mdkr_widescreen_proportions_")

    failures: list[str] = []
    results: list[Result] = []
    failures.extend(billboard_source_contract())
    with context as root_text:
        root = Path(root_text)
        for case in CASES:
            try:
                result, case_failures = run_case(
                    binary,
                    rom,
                    case,
                    root,
                    args.renderer,
                    args.timeout,
                    args.verbose,
                )
                results.append(result)
                failures.extend(case_failures)
            except (OSError, ValueError) as exc:
                failures.append(f"{case.label}: {exc}")
        failures.extend(analyze_results(results, args.verbose))
        if args.keep_frames:
            print(f"  artifacts: {root}")

    if failures:
        print("FAIL: widescreen/FOV proportion check")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(
        "PASS: HUD and world balloons preserve proportions at 4:3/16:10/16:9/"
        "21:9, forced 4:3, and 75-degree FOV; billboard census fixed; "
        "legacy stretch rejected"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
