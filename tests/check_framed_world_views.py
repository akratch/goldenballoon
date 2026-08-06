#!/usr/bin/env python3
"""Apply widescreen deliberately to cinematic and framed menu world views.

``VIEWPORT_EXTRA_BG`` is a rendering mechanism, not an aspect-ratio policy.
Opening logos, credits, the Track Select setup page, and the initial post-race
footage are unframed presentations and should use the wider region. Track
Select's browser/zoom phases and the later post-race pages place live world
rendering behind a wooden aperture and must contain it in the safe 4:3 region.
Decorative menu backgrounds should fill the side gutters in either case.

This gate captures both policies at 4:3, 16:9, and 21:9 on WebGPU, plus their
21:9 witnesses on OpenGL. ``MDKR_TEST_FRAMED_WORLD_UNSAFE=1`` is a deliberate
renderer fault: it must be inert for presentation scenes and at 4:3, but expose
large side-band bleed for safe-aperture scenes at wider ratios. Frontend route
and gameplay pacing must remain unchanged in every pair. Focused 60 Hz
interpolated captures land on the midpoint that crosses each changed policy;
the camera snapshot must hold the new projection instead of blending safe and
wide aspect recipes for one frame.
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, fatal_re,
                           read_ppm as read_ppm_bytes, resolve_binary,
                           VALIDATION_MARKERS)


ROOT = Path(__file__).resolve().parent.parent
ROM_SCRIPT = ROOT / "tests" / "input_scripts" / "adventure_race_loop.txt"
TRACK_SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_track_select.txt"
TRACK_SCROLL_SCRIPT = (
    ROOT / "tests" / "input_scripts" / "nav_track_select_scroll.txt"
)
TRACK_RACE_SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
TRACK_BACK_SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_track_setup_back.txt"
CHARSELECT_LEAK_SCRIPT = (
    ROOT / "tests" / "input_scripts" / "nav_charselect_after_track_select.txt"
)
CHARSELECT_CONTROL_SCRIPT = (
    ROOT / "tests" / "input_scripts" / "nav_charselect_late.txt"
)
CREDITS_SCRIPT = ROOT / "tests" / "input_scripts" / "credits_via_cheat.txt"
FATAL_RE = fatal_re(*ASSERT_MARKERS, *VALIDATION_MARKERS)


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True)
class Scene:
    name: str
    frames: int
    capture: int
    script: Path
    policy: str
    extra_env: tuple[tuple[str, str], ...] = ()
    # Does this safe-aperture scene's live view reach the decorative gutters
    # once the deliberate fault releases it from the 4:3 region? A full-frame
    # view (zoom, post-race) does and must; Track Select's browse carousel
    # draws through an inset sub-rectangle that rescales well inside the safe
    # area, so its gutters must stay inert in BOTH arms. Ignored for
    # presentation scenes, which have no aperture to contain.
    gutter_bleed: bool = True


@dataclass(frozen=True)
class Arm:
    scene: str
    backend: str
    size: str
    unsafe: bool
    image: Image
    pace: tuple[str, ...]
    route: tuple[str, ...]


POST_RACE_ENV = (
    ("MDKR_AUTOPILOT", "1"),
    ("MDKR_ADVENTURE_LOSS", "1"),
    (
        "MDKR_DRIVE_ROUTE",
        "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,"
        "2180:E12:-3381,1946:-2519,1516:-1858,1099;12:E5:E0",
    ),
)

SCENES = (
    Scene("opening-logos", 701, 700, TRACK_SCRIPT, "presentation"),
    Scene("track-select", 2300, 2299, TRACK_SCRIPT, "safe-aperture",
          gutter_bleed=False),
    Scene("track-select-zoom", 2171, 2170, TRACK_RACE_SCRIPT, "safe-aperture"),
    Scene("track-setup", 2211, 2210, TRACK_RACE_SCRIPT, "presentation"),
    Scene("track-setup-back", 2331, 2330, TRACK_BACK_SCRIPT, "safe-aperture"),
    Scene("credits", 4001, 4000, CREDITS_SCRIPT, "presentation"),
    Scene(
        "post-race-unframed",
        12741,
        12740,
        ROM_SCRIPT,
        "presentation",
        POST_RACE_ENV,
    ),
    Scene(
        "post-race-transition",
        12761,
        12760,
        ROM_SCRIPT,
        "safe-aperture",
        POST_RACE_ENV,
    ),
    Scene(
        "post-race-framed",
        12811,
        12810,
        ROM_SCRIPT,
        "safe-aperture",
        POST_RACE_ENV,
    ),
)

INTERPOLATED_BOUNDARY_SCENES = (
    Scene(
        "track-setup-interpolated-boundary",
        2211 * 2,
        2210 * 2 - 1,
        TRACK_RACE_SCRIPT,
        "presentation",
        (("MDKR_PRESENT_RATE", "60"),
         ("MDKR_PRESENT_SMOOTHING", "interpolate"),
         ("MDKR_SIMULATION_CADENCE", "original")),
    ),
    Scene(
        "track-setup-back-interpolated-boundary",
        2331 * 2,
        2330 * 2 - 1,
        TRACK_BACK_SCRIPT,
        "safe-aperture",
        (("MDKR_PRESENT_RATE", "60"),
         ("MDKR_PRESENT_SMOOTHING", "interpolate"),
         ("MDKR_SIMULATION_CADENCE", "original")),
    ),
    Scene(
        "post-race-interpolated-boundary",
        12761 * 2,
        12760 * 2 - 1,
        ROM_SCRIPT,
        "safe-aperture",
        POST_RACE_ENV +
        (("MDKR_PRESENT_RATE", "60"),
         ("MDKR_PRESENT_SMOOTHING", "interpolate"),
         ("MDKR_SIMULATION_CADENCE", "original")),
    ),
)

WEBGPU_SIZES = ("720x540", "960x540", "1260x540")
ROUTE_PREFIXES = ("menu_init:", "level_load:")


def read_ppm(path: Path) -> Image:
    return Image(*read_ppm_bytes(path))


def clean_environment(
    backend: str, save_dir: Path, unsafe: bool,
    capture: int,
    extra: tuple[tuple[str, str], ...],
) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR_", "GE007_"))}
    env.update(
        MDKR_AUDIO="0",
        MDKR_RENDERER=backend,
        MDKR_RENDER_SCALE="1",
        MDKR_WIDESCREEN="1",
        MDKR_ASPECT="auto",
        MDKR_FOV="authored",
        MDKR_PRESENT_RATE="original",
        MDKR_PRESENT_SMOOTHING="off",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_TRACE="1",
        MDKR_DUMP_FROM=str(capture),
        MDKR_DUMP_EVERY="999",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        MDKR_SAVE_DIR=str(save_dir),
        LC_ALL="C",
    )
    env.update(extra)
    if unsafe:
        env["MDKR_TEST_FRAMED_WORLD_UNSAFE"] = "1"
    return env


def normalized_pace(output: str) -> tuple[str, ...]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker >= 0:
            rows.append(re.sub(r" dtms=\S+", " dtms=<wall>", line[marker:]))
    return tuple(rows)


def route_trace(output: str) -> tuple[str, ...]:
    return tuple(
        line[line.find(prefix):]
        for line in output.splitlines()
        for prefix in ROUTE_PREFIXES
        if prefix in line
    )


def run_arm(
    binary: Path,
    rom: Path,
    scene: Scene,
    backend: str,
    size: str,
    unsafe: bool,
    work: Path,
    timeout: int,
    verbose: bool,
) -> Arm:
    label = f"{scene.name}-{backend}-{size}-{'unsafe' if unsafe else 'fixed'}"
    run_dir = work / label
    frame_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frame_dir.mkdir(parents=True)
    save_dir.mkdir()
    command = [
        str(binary),
        "--headless-frames", str(scene.frames),
        "--window-size", size,
        "--input-script", str(scene.script),
        "--dump-frames", str(frame_dir),
        "--rom", str(rom),
        "--restored",
    ]
    if verbose:
        print(f"$ ({label}) {shlex.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=clean_environment(
                backend, save_dir, unsafe, scene.capture, scene.extra_env
            ),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        raise RuntimeError(f"{label}: timed out\n{output[-4000:]}") from exc

    output = proc.stdout
    (run_dir / "run.log").write_text(output, encoding="utf-8")
    fatal = FATAL_RE.search(output)
    frames = sorted(frame_dir.glob("*.ppm"))
    expected = f"frame_{scene.capture:04d}.ppm"
    if (proc.returncode != 0 or fatal is not None or len(frames) != 1 or
            frames[0].name != expected):
        raise RuntimeError(
            f"{label}: exit={proc.returncode}, "
            f"fatal={fatal.group(0) if fatal else 'none'}, "
            f"frames={[path.name for path in frames]}, expected={expected}\n"
            f"{output[-5000:]}"
        )
    if f"renderer backend: {backend}" not in output:
        raise RuntimeError(f"{label}: requested renderer was not selected")
    return Arm(
        scene=scene.name,
        backend=backend,
        size=size,
        unsafe=unsafe,
        image=read_ppm(frames[0]),
        pace=normalized_pace(output),
        route=route_trace(output),
    )


def run_track_select_scroll(
    binary: Path,
    rom: Path,
    backend: str,
    work: Path,
    timeout: int,
    verbose: bool,
) -> str:
    """Capture both carousel directions and reject overlay ink in gutters."""
    first_capture = 2190
    frame_count = 2320
    label = f"track-select-scroll-{backend}-1260x540"
    run_dir = work / label
    frame_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frame_dir.mkdir(parents=True)
    save_dir.mkdir()
    command = [
        str(binary),
        "--headless-frames", str(frame_count),
        "--window-size", "1260x540",
        "--input-script", str(TRACK_SCROLL_SCRIPT),
        "--dump-frames", str(frame_dir),
        "--rom", str(rom),
        "--restored",
    ]
    env = clean_environment(backend, save_dir, False, first_capture, ())
    env["MDKR_DUMP_EVERY"] = "1"
    if verbose:
        print(f"$ ({label}) {shlex.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        raise RuntimeError(f"{label}: timed out\n{output[-4000:]}") from exc

    output = proc.stdout
    (run_dir / "run.log").write_text(output, encoding="utf-8")
    fatal = FATAL_RE.search(output)
    frames = sorted(frame_dir.glob("*.ppm"))
    expected_count = frame_count - first_capture
    if (proc.returncode != 0 or fatal is not None or
            len(frames) != expected_count):
        raise RuntimeError(
            f"{label}: exit={proc.returncode}, "
            f"fatal={fatal.group(0) if fatal else 'none'}, "
            f"frames={len(frames)}, expected={expected_count}\n"
            f"{output[-5000:]}"
        )
    if f"renderer backend: {backend}" not in output:
        raise RuntimeError(f"{label}: requested renderer was not selected")

    worst = ("", 0.0)
    for path in frames:
        image = read_ppm(path)
        ratio = image.width / image.height
        safe_margin = (1.0 - ((4.0 / 3.0) / ratio)) * 0.5
        blue = max(
            strong_blue_fraction(image, (0.0, 0.0, safe_margin, 1.0)),
            strong_blue_fraction(
                image, (1.0 - safe_margin, 0.0, 1.0, 1.0)
            ),
        )
        if blue > worst[1]:
            worst = (path.name, blue)
    if worst[1] > 0.0:
        raise RuntimeError(
            f"{label}: carousel overlay crossed an authored edge in "
            f"{worst[0]} ({worst[1]:.3%} blue ink)"
        )
    return f"{len(frames)} transition frames; both gutters clean"


def column_profile(image: Image) -> list[float]:
    """Mean brightness per column: a signature of WHERE the world landed.

    Character select animates, so two captures of it are never pixel-equal.  A
    per-column profile is dominated by the scene's fixed geometry (sky, treeline,
    log, grass horizon), which is exactly the part a lens/region disagreement
    moves, so it survives an idle-animation phase difference that a pixel
    comparison could not.
    """
    profile: list[float] = []
    for x in range(image.width):
        total = 0
        for y in range(image.height):
            offset = (y * image.width + x) * 3
            total += (image.pixels[offset] + image.pixels[offset + 1] +
                      image.pixels[offset + 2])
        profile.append(total / (3.0 * image.height))
    return profile


def sample_profile(profile: list[float], position: float) -> float | None:
    count = len(profile)
    if position < 0.0 or position > count - 1:
        return None
    index = int(position)
    if index >= count - 1:
        return profile[count - 1]
    fraction = position - index
    return profile[index] * (1.0 - fraction) + profile[index + 1] * fraction


def best_horizontal_scale(
    reference: Image, candidate: Image
) -> tuple[float, float]:
    """Horizontal scale about the centre that best maps candidate onto reference.

    A world drawn with the safe aperture's 4:3 lens into the full presentation
    rectangle is the reference image scaled horizontally about the screen centre
    -- nothing else about it changes -- so the recovered scale IS the defect's
    magnitude.  1.0 means the two images share a lens; a 4:3 lens stretched to
    16:9 recovers 0.75.
    """
    if (reference.width, reference.height) != (candidate.width, candidate.height):
        raise RuntimeError("aperture-leak arm sizes differ")
    left = column_profile(reference)
    right = column_profile(candidate)
    centre = (len(left) - 1) / 2.0
    best = (1e9, 0.0)
    step = 0
    while step <= 150:
        scale = 0.70 + step * 0.005
        total = count = 0
        for x in range(0, len(left), 3):
            value = sample_profile(right, centre + (x - centre) / scale)
            if value is None:
                continue
            total += abs(left[x] - value)
            count += 1
        if count and total / count < best[0]:
            best = (total / count, scale)
        step += 1
    return best[1], best[0]


def stretch_horizontally(image: Image, factor: float) -> Image:
    """Widen the image about its centre and clip it to the surface."""
    centre = (image.width - 1) / 2.0
    pixels = bytearray(len(image.pixels))
    for y in range(image.height):
        row = y * image.width * 3
        for x in range(image.width):
            source = centre + (x - centre) / factor
            index = min(max(int(round(source)), 0), image.width - 1)
            offset = row + index * 3
            pixels[row + x * 3:row + x * 3 + 3] = image.pixels[offset:offset + 3]
    return Image(image.width, image.height, bytes(pixels))


def run_aperture_leak_arms(
    binary: Path,
    rom: Path,
    backend: str,
    work: Path,
    timeout: int,
    verbose: bool,
) -> str:
    """The aperture must not outlive the screen that asked for it.

    Track Select sets the safe 4:3 world region for its wooden frame.  That
    region is per-viewport state the projection latch reads every tick, while
    viewport_main() states the presentation region to the renderer for every
    unframed view.  If the region survives Track Select, the two disagree and
    every later screen -- character select first, then the race entered from it
    -- draws a 4:3 lens across the whole presentation rectangle.  Both reports
    behind this arm (a stretched player select after visiting Tracks) are that
    one leak, so the assertion is a route comparison, not a pixel constant: the
    same screen reached with and without a Track Select visit must share a lens.
    """
    size = "960x540"
    routes = {
        "after-track-select": CHARSELECT_LEAK_SCRIPT,
        "control": CHARSELECT_CONTROL_SCRIPT,
    }
    captured: dict[str, Arm] = {}
    for name, script in routes.items():
        scene = Scene(
            f"charselect-{name}", 2901, 2900, script, "presentation",
        )
        captured[name] = run_arm(
            binary, rom, scene, backend, size, False, work, timeout, verbose,
        )

    leak_route = captured["after-track-select"].route
    control_route = captured["control"].route
    if not any("menuId=15" in line for line in leak_route):
        raise RuntimeError(
            "aperture-leak arm never reached Track Select; the fixture no "
            f"longer exercises the leak (route {leak_route})"
        )
    if any("menuId=15" in line for line in control_route):
        raise RuntimeError(
            "aperture-leak control visited Track Select; it is no longer a "
            f"control (route {control_route})"
        )
    arrival: dict[str, int] = {}
    for name, route in (("after-track-select", leak_route),
                        ("control", control_route)):
        if not route or "menuId=3" not in route[-1]:
            raise RuntimeError(
                f"aperture-leak {name} arm did not end on character select "
                f"(route {route})"
            )
        frame = re.search(r"@frame~(\d+)", route[-1])
        if frame is None:
            raise RuntimeError(
                f"aperture-leak {name} arm has no character-select arrival "
                f"frame to compare against; the menu_init trace changed shape "
                f"(route {route[-1]})"
            )
        arrival[name] = int(frame.group(1))

    # Both arms capture at a hardcoded frame on a screen that keeps animating,
    # so the comparison is only meaningful while the two arms reach character
    # select at roughly the same time. Drift is a fixture problem, not a lens
    # problem, and has to be reported as one.
    drift = arrival["after-track-select"] - arrival["control"]
    if abs(drift) > 8:
        raise RuntimeError(
            f"aperture-leak fixture drift: the arms reach character select "
            f"{abs(drift)} frames apart (after-track-select at "
            f"{arrival['after-track-select']}, control at "
            f"{arrival['control']}). The two captures no longer land on the "
            f"same point of the screen's animation, so the lens comparison "
            f"below cannot be trusted. Retime the input scripts or the "
            f"capture frames; this is not a lens failure."
        )

    scale, residual = best_horizontal_scale(
        captured["control"].image, captured["after-track-select"].image
    )
    if abs(scale - 1.0) > 0.03:
        raise RuntimeError(
            f"charselect-aperture-leak/{backend}/{size}: character select "
            f"reached through Track Select presents a {scale:.3f}x horizontal "
            f"lens against the same screen reached directly. Track Select's "
            f"safe aperture is outliving its screen."
        )

    # The detector has to be able to see the defect it is guarding against.
    faulted = stretch_horizontally(captured["control"].image, 4.0 / 3.0)
    fault_scale, _ = best_horizontal_scale(captured["control"].image, faulted)
    if abs(fault_scale - 1.0) <= 0.03:
        raise RuntimeError(
            f"charselect-aperture-leak/{backend}/{size}: the synthetic 4:3 "
            f"stretch recovered {fault_scale:.3f}x, so this arm can no longer "
            "distinguish a leaked aperture from a correct lens"
        )
    return (f"lens ratio {scale:.3f} (residual {residual:.2f}); "
            f"synthetic 4:3 stretch rejected at {fault_scale:.3f}")


def find_pal_rom(directory: Path) -> Path | None:
    if not directory.is_dir():
        return None
    for path in sorted(directory.iterdir()):
        name = path.name.lower()
        if ("europe" in name or "pal" in name) and name.endswith(".z64"):
            return path
    return None


def menu_ink_rows(image: Image) -> tuple[int, int]:
    """First and last row carrying Track Select's blue outline ink.

    The world view inside the wooden aperture is sky and terrain; the outlined
    world/track captions and the carousel arrows are the only strongly blue ink
    on the page, which makes them a stable probe for where the authored 2D
    envelope actually landed.
    """
    first = -1
    last = -1
    for y in range(image.height):
        row = y * image.width * 3
        for x in range(image.width):
            offset = row + x * 3
            red, green, value = image.pixels[offset:offset + 3]
            if value > red + 25 and value > green + 20 and value > 75:
                if first < 0:
                    first = y
                last = y
                break
    return first, last


def run_pal_envelope_arm(
    binary: Path,
    pal_rom: Path,
    size: str,
    work: Path,
    timeout: int,
    verbose: bool,
) -> str:
    """The PAL surface is 320x264, and all 264 rows have to reach the screen.

    video.c raises every PAL video mode by PAL_HEIGHT_DIFFERENCE and the menus
    then lay themselves out across the taller field. A renderer that maps those
    coordinates through a fixed 240 rows magnifies PAL 2D art by 264/240 and
    walks everything below the vertical centre off the bottom of the host
    surface -- the Track Select card sits too low with its caption cut in half.
    Nothing in the NTSC matrix above can see that, which is how it survived a
    release. This arm reads the authored envelope straight off the pixels.
    """
    scene = Scene("pal-track-select", 2300, 2299, TRACK_SCRIPT, "safe-aperture",
                  gutter_bleed=False)
    label = f"pal-track-select-gl-{size}"
    run_dir = work / label
    frame_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frame_dir.mkdir(parents=True)
    save_dir.mkdir()
    command = [
        str(binary),
        "--headless-frames", str(scene.frames),
        "--window-size", size,
        "--input-script", str(scene.script),
        "--dump-frames", str(frame_dir),
        "--rom", str(pal_rom),
        "--restored",
    ]
    if verbose:
        print(f"$ ({label}) {shlex.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=clean_environment("gl", save_dir, False, scene.capture, ()),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        raise RuntimeError(f"{label}: timed out\n{output[-4000:]}") from exc

    output = proc.stdout
    (run_dir / "run.log").write_text(output, encoding="utf-8")
    fatal = FATAL_RE.search(output)
    frames = sorted(frame_dir.glob("*.ppm"))
    expected = f"frame_{scene.capture:04d}.ppm"
    if (proc.returncode != 0 or fatal is not None or len(frames) != 1 or
            frames[0].name != expected):
        raise RuntimeError(
            f"{label}: exit={proc.returncode}, "
            f"fatal={fatal.group(0) if fatal else 'none'}, "
            f"frames={[path.name for path in frames]}, expected={expected}\n"
            f"{output[-5000:]}"
        )
    image = read_ppm(frames[0])
    first, last = menu_ink_rows(image)
    if first < 0:
        raise RuntimeError(
            f"{label}: found no Track Select overlay ink at all"
        )
    # A margin of one percent of the surface. The authored envelope has real
    # padding above and below the captions, so ink in the outermost rows means
    # the envelope has been scaled or shifted past the edge, not that it fits
    # snugly.
    edge = max(1, round(image.height * 0.01))
    if first < edge or last >= image.height - edge:
        raise RuntimeError(
            f"{label}: the PAL 2D envelope is clipped by the host surface "
            f"(overlay ink rows {first}..{last} of {image.height}; must stay "
            f"clear of the outer {edge} rows). The PAL framebuffer is 264 rows "
            f"and every one of them has to be mapped."
        )
    centre = (first + last) / 2.0
    offset = abs(centre - image.height / 2.0) / image.height
    if offset > 0.06:
        raise RuntimeError(
            f"{label}: the PAL 2D envelope is off-centre by {offset:.1%} of "
            f"the surface (ink rows {first}..{last} of {image.height})"
        )
    return (f"{size}: PAL envelope rows {first}..{last} of {image.height}, "
            f"{offset:.1%} off centre")


def changed_fraction(
    fixed: Image, unsafe: Image, box: tuple[float, float, float, float]
) -> float:
    if (fixed.width, fixed.height) != (unsafe.width, unsafe.height):
        raise RuntimeError("fixed/control image dimensions differ")
    x0 = round(box[0] * fixed.width)
    y0 = round(box[1] * fixed.height)
    x1 = round(box[2] * fixed.width)
    y1 = round(box[3] * fixed.height)
    changed = total = 0
    for y in range(y0, y1):
        row = y * fixed.width * 3
        for x in range(x0, x1):
            offset = row + x * 3
            total += 1
            if (fixed.pixels[offset:offset + 3] !=
                    unsafe.pixels[offset:offset + 3]):
                changed += 1
    return changed / total if total else 0.0


def dark_fraction(image: Image, box: tuple[float, float, float, float]) -> float:
    x0 = round(box[0] * image.width)
    y0 = round(box[1] * image.height)
    x1 = round(box[2] * image.width)
    y1 = round(box[3] * image.height)
    dark = total = 0
    for y in range(y0, y1):
        row = y * image.width * 3
        for x in range(x0, x1):
            offset = row + x * 3
            total += 1
            if max(image.pixels[offset:offset + 3]) <= 8:
                dark += 1
    return dark / total if total else 0.0


def strong_blue_fraction(
    image: Image, box: tuple[float, float, float, float]
) -> float:
    """Count the blue Track Select arrow ink, not the brown tiled backdrop."""
    x0 = round(box[0] * image.width)
    y0 = round(box[1] * image.height)
    x1 = round(box[2] * image.width)
    y1 = round(box[3] * image.height)
    blue = total = 0
    for y in range(y0, y1):
        row = y * image.width * 3
        for x in range(x0, x1):
            offset = row + x * 3
            red, green, value = image.pixels[offset:offset + 3]
            total += 1
            if value > red + 25 and value > green + 20 and value > 75:
                blue += 1
    return blue / total if total else 0.0


def validate_pair(scene: Scene, fixed: Arm, unsafe: Arm) -> str:
    if fixed.pace != unsafe.pace:
        raise RuntimeError(
            f"{fixed.scene}-{fixed.backend}-{fixed.size}: renderer control "
            "changed simulation"
        )
    if fixed.route != unsafe.route:
        raise RuntimeError(
            f"{fixed.scene}-{fixed.backend}-{fixed.size}: renderer control "
            "changed the frontend route"
        )
    width, height = (int(value) for value in fixed.size.split("x", 1))
    ratio = width / height
    full_difference = changed_fraction(fixed.image, unsafe.image,
                                       (0.0, 0.0, 1.0, 1.0))
    if ratio > 4.0 / 3.0 + 0.01:
        safe_margin = (1.0 - ((4.0 / 3.0) / ratio)) * 0.5
        witness_width = safe_margin * 0.8
        side_dark = max(
            dark_fraction(fixed.image, (0.01, 0.20, witness_width, 0.72)),
            dark_fraction(
                fixed.image, (1.0 - witness_width, 0.20, 0.99, 0.72)
            ),
        )
        if side_dark > 0.25:
            raise RuntimeError(
                f"{fixed.scene}-{fixed.backend}-{fixed.size}: decorative or "
                f"cinematic side region is {side_dark:.1%} black"
            )
        if scene.name == "track-select":
            # The carousel builds eight neighbouring cards. Their frames may
            # slide through the wide backdrop, but the console-clipped labels
            # and blue arrows must not become controls in the right gutter.
            gutter_blue = strong_blue_fraction(
                fixed.image,
                (1.0 - witness_width, 0.15, 0.99, 0.75),
            )
            if gutter_blue > 0.0005:
                raise RuntimeError(
                    f"{fixed.scene}-{fixed.backend}-{fixed.size}: "
                    "off-canvas carousel controls leaked into the right "
                    f"gutter ({gutter_blue:.3%} blue ink)"
                )

    if scene.policy == "presentation":
        if fixed.image.pixels != unsafe.image.pixels:
            raise RuntimeError(
                f"{fixed.scene}-{fixed.backend}-{fixed.size}: safe-aperture "
                f"fault changed a presentation scene ({full_difference:.3%})"
            )
        return "presentation control inert; side region filled"

    if abs(ratio - 4.0 / 3.0) < 0.01:
        if fixed.image.pixels != unsafe.image.pixels:
            raise RuntimeError(
                f"{fixed.scene}-{fixed.backend}-{fixed.size}: 4:3 control "
                f"changed {full_difference:.3%} of pixels"
            )
        return "4:3 control inert"

    # The defect this control injects is a REGION defect: the live view is
    # composed against the presentation rectangle instead of the safe 4:3 one.
    # The bands that decide that policy are the decorative gutters, so measure
    # the gutters themselves. The box this replaced -- (0.20, 0.08, 0.82, 0.68)
    # -- lay wholly inside the safe area at every ratio the gate runs (4:3
    # inside 16:9 spans 0.125..0.875), so it could only ever see the aperture's
    # own re-projection and would have passed unchanged with a live view
    # painting straight through the gutters.
    margin = (1.0 - ((4.0 / 3.0) / ratio)) * 0.5
    gutter_difference = max(
        changed_fraction(fixed.image, unsafe.image,
                         (0.0, 0.08, margin, 0.68)),
        changed_fraction(fixed.image, unsafe.image,
                         (1.0 - margin, 0.08, 1.0, 0.68)),
    )

    if scene.gutter_bleed:
        # A full-frame live view released from the safe region has to paint the
        # gutters. If it stops doing so, the region policy is no longer what
        # keeps them decorative and this gate has stopped testing anything.
        if gutter_difference < 0.10:
            raise RuntimeError(
                f"{fixed.scene}-{fixed.backend}-{fixed.size}: broken control "
                f"did not reach the decorative gutters (gutter delta "
                f"{gutter_difference:.3%}, want >= 10%)"
            )
        return f"gutter control delta {gutter_difference:.1%}"

    # Track Select's browse aperture is an inset sub-rectangle. Neither arm may
    # reach a gutter: any reaction there is live world painted outside the safe
    # region, which is precisely what the wooden frame exists to prevent.
    if gutter_difference > 0.0:
        raise RuntimeError(
            f"{fixed.scene}-{fixed.backend}-{fixed.size}: the inset browse "
            f"aperture reached the decorative gutters (gutter delta "
            f"{gutter_difference:.3%}, want 0)"
        )
    aperture_difference = changed_fraction(
        fixed.image, unsafe.image, (margin, 0.08, 1.0 - margin, 0.68)
    )
    minimum = 0.08 if ratio < 2.0 else 0.20
    if aperture_difference < minimum:
        raise RuntimeError(
            f"{fixed.scene}-{fixed.backend}-{fixed.size}: broken control did "
            f"not expose the framed-view defect (aperture delta "
            f"{aperture_difference:.3%}, want >= {minimum:.0%})"
        )
    return (f"gutters inert; aperture control delta "
            f"{aperture_difference:.1%}")


def source_contract() -> None:
    header = (ROOT / "game" / "include" / "f3ddkr.h").read_text()
    camera = (ROOT / "game" / "src" / "camera.c").read_text()
    background = (ROOT / "game" / "src" / "rcp_dkr.c").read_text()
    renderer = (ROOT / "platform" / "fast3d" / "gfx_pc_dkr.c").read_text()
    snapshot_header = (ROOT / "platform" / "presentation_snapshot.h").read_text()
    snapshot = (ROOT / "platform" / "presentation_snapshot.c").read_text()
    menu = (ROOT / "game" / "src" / "menu.c").read_text()
    video = (ROOT / "game" / "src" / "video.c").read_text()
    required = (
        ("display-list region metadata", "G_MW_DKR_WORLD_REGION", header),
        ("explicit viewport policy", "ViewportWorldRegion", camera),
        # The aperture belongs to the screen that draws a frame, so scene entry
        # -- not one hand-picked exit path -- is what returns every viewport to
        # the presentation region.  Both entry funnels have to keep doing it.
        ("world region default", "void viewport_world_regions_reset(void)", camera),
        ("scene entry resets the region", "viewport_world_regions_reset();", camera),
        ("menu entry resets the region", "viewport_world_regions_reset();", menu),
        # Re-anchored onto cam_projection_for_viewport_region after the lens
        # latch refactor retired the old inline branch; the property asserted is
        # unchanged -- a safe-aperture view takes the authored 4:3 lens and
        # everything else takes the presentation aspect.
        ("safe-aperture projection", "safeWorldRegion ? CAMERA_ASPECT", camera),
        ("presentation projection", "layout.presentation_aspect", camera),
        ("ordinary world reset", "gDkrSetWorldRegion((*dlist)++, FALSE)", camera),
        ("full-surface clear reset", "gDkrSetWorldRegion((*dList)++, FALSE)", background),
        ("policy-aware backing", "viewport_world_region_uses_safe_aperture(0)", background),
        ("safe world mapping", "rsp.world_safe_region ? layout.safe", renderer),
        ("wide background space", "G_MTX_DKR_SPACE_WIDE_BG", renderer),
        ("snapshot world-region field", "uint8_t world_region", snapshot_header),
        ("discrete snapshot region cut",
         "history->last_world_region != sample->world_region", snapshot),
        ("opening-logo viewport", "void menu_logos_screen_init(void)", menu),
        ("track-select viewport", "void menu_track_select_init(void)", menu),
        ("track-select phase policy", "gTrackmenuType == TRACKMENU_TYPE_FREE && gMenuDelay >= 0", menu),
        ("tiled track-select background", "mtx_ortho_wide_background", menu),
        ("track-select overlay containment", "overlayFullyInsideAuthoredCanvas", menu),
        ("overlay containment needs real gutters",
         "if (!hasDecorativeGutters) {", menu),
        ("authored surface mapping", "region.height / dkr_logical_height", renderer),
        ("authored surface published", "gfx_dkr_set_logical_surface", video),
        ("post-race viewport", "void postrace_start(s32 finishState", menu),
        ("credits viewport", "void menu_credits_init(void)", menu),
    )
    missing = [name for name, needle, source in required if needle not in source]
    if missing:
        raise RuntimeError("missing source contracts: " + ", ".join(missing))
    if "last_viewport" in snapshot:
        raise RuntimeError(
            "snapshot viewport coordinates must remain continuously interpolated"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--roms", default="build/roms",
        help="directory holding the region releases; the PAL arm is not "
             "optional and this gate fails rather than skipping it",
    )
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--interpolated-only", action="store_true")
    parser.add_argument(
        "--scene",
        choices=tuple(scene.name for scene in SCENES) +
        ("track-select-scroll", "pal-track-select",
         "charselect-aperture-leak"),
        help="run one ordinary scene instead of the complete matrix",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if not binary.is_file() or not rom.is_file():
        print("check_framed_world_views: FAIL -- missing binary or ROM",
              file=sys.stderr)
        return 1

    try:
        source_contract()
        with tempfile.TemporaryDirectory(prefix="mdkr-framed-world-") as temp:
            work = Path(temp)
            summaries: list[str] = []
            ordinary_scenes = SCENES
            if args.scene:
                ordinary_scenes = tuple(
                    scene for scene in SCENES if scene.name == args.scene
                )
            run_pal = (
                not args.interpolated_only and
                (not args.scene or args.scene == "pal-track-select")
            )
            if run_pal:
                pal_rom = find_pal_rom(Path(os.path.abspath(args.roms)))
                if pal_rom is None:
                    raise RuntimeError(
                        "the PAL framebuffer is 264 rows and this gate covers "
                        "how those rows reach the host surface, so it needs a "
                        "PAL release; none was found below "
                        f"{args.roms}. This gate does not silently skip its "
                        "region arm."
                    )
                for size in ("720x540", "960x540", "1260x540"):
                    summaries.append(
                        "pal-track-select/gl/" + size + ": " +
                        run_pal_envelope_arm(
                            binary, pal_rom, size, work, args.timeout,
                            args.verbose,
                        )
                    )
            if args.interpolated_only:
                ordinary_scenes = ()
            for scene in ordinary_scenes:
                for backend, sizes in (
                    ("webgpu", WEBGPU_SIZES),
                    ("gl", ("1260x540",)),
                ):
                    for size in sizes:
                        fixed = run_arm(
                            binary, rom, scene, backend, size, False, work,
                            args.timeout, args.verbose,
                        )
                        unsafe = run_arm(
                            binary, rom, scene, backend, size, True, work,
                            args.timeout, args.verbose,
                        )
                        summaries.append(
                            f"{scene.name}/{backend}/{size}: "
                            f"{validate_pair(scene, fixed, unsafe)}"
                        )
            boundary_scenes = (
                INTERPOLATED_BOUNDARY_SCENES
                if not args.scene else ()
            )
            for scene in boundary_scenes:
                fixed = run_arm(
                    binary, rom, scene, "webgpu", "1260x540", False, work,
                    args.timeout, args.verbose,
                )
                unsafe = run_arm(
                    binary, rom, scene, "webgpu", "1260x540", True, work,
                    args.timeout, args.verbose,
                )
                summaries.append(
                    f"{scene.name}/webgpu/1260x540: "
                    f"{validate_pair(scene, fixed, unsafe)}"
                )
            run_scroll = (
                not args.interpolated_only and
                (not args.scene or args.scene == "track-select-scroll")
            )
            if run_scroll:
                for backend in ("webgpu", "gl"):
                    summaries.append(
                        f"track-select-scroll/{backend}/1260x540: "
                        f"{run_track_select_scroll(binary, rom, backend, work, args.timeout, args.verbose)}"
                    )
            run_leak = (
                not args.interpolated_only and
                (not args.scene or args.scene == "charselect-aperture-leak")
            )
            if run_leak:
                summaries.append(
                    "charselect-aperture-leak/webgpu/960x540: " +
                    run_aperture_leak_arms(
                        binary, rom, "webgpu", work, args.timeout,
                        args.verbose,
                    )
                )
            if args.verbose:
                for summary in summaries:
                    print("  " + summary)
    except RuntimeError as exc:
        print(f"check_framed_world_views: FAIL -- {exc}", file=sys.stderr)
        return 1

    print(
        "check_framed_world_views: PASS -- cinematics fill widescreen while "
        "wooden apertures contain live world views on WebGPU and OpenGL"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
