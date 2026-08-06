#!/usr/bin/env python3
"""Rendered and navigable Taj roster coverage for every retail unlock layout.

The character-select scene has four ROM-authored arrangements: the base eight,
Drumstick only, T.T. only, and all ten racers. Taj must be the next contiguous
slot in each arrangement. His actual Park Warden model must stand in the scene,
animate on hover, and raise the same numbered placard as the retail cast.
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

from check_adventure_two import eeprom_image
from harness_utils import read_ppm, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
ROM_WIDTH = 320
ROM_HEIGHT = 240
FRAMES = 2250
UNSELECTED_FRAME = 1520
SELECTED_FRAME = 1980
SELECTED_MOTION_FRAME = 2040
STATE_TEXT = (
    "taj_mod_version=1\n"
    "taj_unlocked=1\n"
    "adventure_migration_complete=1"
)
PAL_V80_CRCS = (0x596E145B, 0xF7D9879F)


@dataclass(frozen=True)
class Layout:
    name: str
    drumstick: bool
    tt: bool
    base_count: int


LAYOUTS = (
    Layout("base", False, False, 8),
    Layout("drumstick", True, False, 9),
    Layout("tt", False, True, 9),
    Layout("complete", True, True, 10),
)

# Character-select indexes are presentation slots, deliberately distinct from
# the retail Character enum/voice IDs. These human-readable trace identities
# prove each original cursor remains itself; the retail picker does not render
# character-name labels.
RETAIL_LABELS = {
    0: "KRUNCH",
    1: "DIDDY",
    2: "BUMPER",
    3: "BANJO",
    4: "CONKER",
    5: "TIPTUP",
    6: "PIPSY",
    7: "TIMBER",
}


def retail_label(layout: Layout, character: int) -> str:
    if character in RETAIL_LABELS:
        return RETAIL_LABELS[character]
    if character == 8:
        return "DRUMSTICK" if layout.drumstick else "T.T."
    if character == 9 and layout.drumstick and layout.tt:
        return "T.T."
    raise ValueError(f"unexpected retail slot {character} for {layout.name}")


def config_block(drumstick: bool, tt: bool) -> bytes:
    value = 1 << 25  # retail default subtitle flag
    if drumstick:
        value |= 1 << 1
    if tt:
        value |= 0xFFFFF << 4
    checksum = 5 + sum((value >> (index * 4)) & 0xF for index in range(14))
    value |= (checksum & 0xFF) << 56
    return value.to_bytes(8, "big")


def save_image(layout: Layout) -> bytes:
    image = bytearray(eeprom_image(False))
    image[120:128] = config_block(layout.drumstick, layout.tt)
    return bytes(image)


def input_script(layout: Layout) -> str:
    # Start at Diddy, traverse every retail actor exactly once (returning via
    # the top row where necessary), and only then enter Taj's physical slot.
    directions = ["LEFT", "DOWN", "RIGHT"]  # Krunch, Conker, Tiptup
    if layout.tt:
        directions.append("RIGHT")  # T.T.
    directions.append("UP")  # Diddy
    if layout.drumstick:
        directions.append("RIGHT")  # Drumstick
    directions.extend(("RIGHT", "RIGHT", "DOWN", "LEFT", "LEFT"))
    start = 1960 - (len(directions) - 1) * 40
    route = "\n".join(
        f"{start + index * 40} {direction} 4"
        for index, direction in enumerate(directions)
    )
    return f"""\
# Title -> character select.
1250 START 4
1330 START 4

# Visit every ordinary actor, then hover Taj long enough to prove his model,
# placard, and animation before confirming.
{route}
2100 A 4
2180 A 4
"""


def rom_header_crcs(path: Path) -> tuple[int, int] | None:
    """Identify a raw supported ROM by normalized N64 header CRCs."""
    try:
        with path.open("rb") as stream:
            head = stream.read(0x18)
    except OSError:
        return None
    if len(head) < 0x18:
        return None
    if head[:4] == b"\x37\x80\x40\x12":
        head = bytes(
            byte for index in range(0, len(head), 2)
            for byte in (head[index + 1], head[index])
        )
    elif head[:4] == b"\x40\x12\x37\x80":
        head = bytes(
            byte for index in range(0, len(head), 4)
            for byte in reversed(head[index:index + 4])
        )
    elif head[:4] != b"\x80\x37\x12\x40":
        return None
    return (
        int.from_bytes(head[0x10:0x14], "big"),
        int.from_bytes(head[0x14:0x18], "big"),
    )


def find_pal_v80(roms: Path) -> Path | None:
    if not roms.is_dir():
        return None
    for candidate in sorted(roms.iterdir()):
        if (candidate.is_file() and candidate.stat().st_size == 12 * 1024 * 1024
                and rom_header_crcs(candidate) == PAL_V80_CRCS):
            return candidate.resolve()
    return None


def centered_fit(outer_width: float, outer_height: float,
                 aspect: float) -> tuple[float, float, float, float]:
    """Letterbox/pillarbox an aspect inside a box, centred.

    The independent Python statement of mdkr_centered_fit() in
    platform/display_config.c.
    """
    if outer_width / outer_height > aspect:
        width = outer_height * aspect
        return (outer_width - width) * 0.5, 0.0, width, outer_height
    height = outer_width / aspect
    return 0.0, (outer_height - height) * 0.5, outer_width, height


@dataclass(frozen=True)
class FrameGeometry:
    """Where the authored 320x240 envelope actually lands in a capture.

    At the default aspect the renderer stretches the authored envelope over
    the whole drawable, so a ROM-logical rectangle is just (logical * scale).
    Under a forced presentation aspect it does not: mdkr_display_calculate_
    layout() centre-fits the presentation inside the drawable and then centre-
    fits the authored 4:3 envelope inside that presentation, so the scene is
    inset and smaller than the frame (21:9 in a 1280x960 capture puts it at
    731x549+274+206). Sampling a ROM-logical rectangle at raw pixel
    coordinates there reads the letterbox bar rather than the scene, which is
    an oracle bug and not a rendering defect -- the lower-centre placard
    rectangle lands entirely below the presentation at 21:9. Every sampling
    box in this check is expressed in ROM-logical pixels and mapped through
    this.
    """

    origin_x: float
    origin_y: float
    scale_x: float
    scale_y: float

    @classmethod
    def for_frame(cls, path: Path, aspect: str | None) -> "FrameGeometry":
        width, height, _ = read_ppm(path)
        if width % ROM_WIDTH or height % ROM_HEIGHT:
            raise ValueError(f"{path}: unexpected dimensions {width}x{height}")
        if aspect is None:
            return cls(0.0, 0.0, width // ROM_WIDTH, height // ROM_HEIGHT)
        numerator, denominator = aspect.split(":")
        presentation_aspect = float(numerator) / float(denominator)
        present_x, present_y, present_w, present_h = centered_fit(
            float(width), float(height), presentation_aspect)
        safe_x, safe_y, safe_w, safe_h = centered_fit(
            present_w, present_h, ROM_WIDTH / ROM_HEIGHT)
        return cls(present_x + safe_x, present_y + safe_y,
                   safe_w / ROM_WIDTH, safe_h / ROM_HEIGHT)

    def x(self, logical: int) -> int:
        return round(self.origin_x + logical * self.scale_x)

    def y(self, logical: int) -> int:
        return round(self.origin_y + logical * self.scale_y)

    @property
    def pixel_scale(self) -> float:
        """Captured pixels per ROM-logical pixel of area."""
        return self.scale_x * self.scale_y

    def area(self, logical_area: int) -> int:
        """Sampled pixels for a logical-pixel area, at this frame's scale."""
        return round(logical_area * self.pixel_scale)


def roster_counts(path: Path, geometry: FrameGeometry) -> tuple[int, int, int]:
    width, height, pixels = read_ppm(path)
    x0, x1 = geometry.x(135), geometry.x(198)
    y0, y1 = geometry.y(135), geometry.y(218)
    placard_x0, placard_x1 = geometry.x(158), geometry.x(192)
    placard_y0, placard_y1 = geometry.y(172), geometry.y(208)
    purple = red_count = gold = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            offset = (y * width + x) * 3
            red, green, blue = pixels[offset:offset + 3]
            if (red > 35 and green < 115 and blue > 100 and
                    blue > red * 1.25 and blue > green * 1.15):
                purple += 1
            if (x >= placard_x0 and x < placard_x1 and
                    y >= placard_y0 and y < placard_y1 and
                    red > 140 and red > green * 1.5 and red > blue * 1.5):
                red_count += 1
            if red > 150 and green > 120 and blue < 100:
                gold += 1
    return purple, red_count, gold


def row_side_counts(path: Path, geometry: FrameGeometry) -> tuple[int, int, int]:
    width, height, pixels = read_ppm(path)
    counts: list[int] = []
    for left, right in ((5, 130), (195, 310)):
        count = 0
        for y in range(geometry.y(148), geometry.y(210)):
            for x in range(geometry.x(left), geometry.x(right)):
                offset = (y * width + x) * 3
                red, green, blue = pixels[offset:offset + 3]
                if (max(red, green, blue) > 80 and
                        (red > green * 1.2 or blue > green * 1.15 or
                         (red > 160 and green > 160 and blue > 160))):
                    count += 1
        counts.append(count)
    return counts[0], counts[1], geometry.pixel_scale


def roster_motion(first: Path, second: Path, geometry: FrameGeometry) -> int:
    width, height, first_pixels = read_ppm(first)
    second_width, second_height, second_pixels = read_ppm(second)
    if (width, height) != (second_width, second_height):
        raise ValueError("motion frames have different dimensions")
    changed = 0
    for y in range(geometry.y(135), geometry.y(218)):
        for x in range(geometry.x(135), geometry.x(192)):
            offset = (y * width + x) * 3
            if any(abs(first_pixels[offset + channel] -
                       second_pixels[offset + channel]) > 20
                   for channel in range(3)):
                changed += 1
    return changed


def oversized_shadow_delta(scaled: Path, unscaled: Path,
                           geometry: FrameGeometry) -> tuple[int, int, int]:
    """Measure the exact full-size Park Warden shadow regression.

    The control differs only by retaining the actor header's unscaled shadow.
    Count pixels that it makes materially darker in the ground footprint below
    Taj, plus all materially changed pixels there. This rejects the original
    giant decal without asking production to remove Taj's grounding shadow.
    """
    width, height, scaled_pixels = read_ppm(scaled)
    control_width, control_height, control_pixels = read_ppm(unscaled)
    if (width, height) != (control_width, control_height):
        raise ValueError("shadow-control frames have different dimensions")
    darker = changed = 0
    x0, x1 = geometry.x(110), geometry.x(215)
    y0, y1 = geometry.y(202), geometry.y(232)
    for y in range(y0, y1):
        for x in range(x0, x1):
            offset = (y * width + x) * 3
            normal = scaled_pixels[offset:offset + 3]
            control = control_pixels[offset:offset + 3]
            normal_luma = 3 * normal[0] + 6 * normal[1] + normal[2]
            control_luma = 3 * control[0] + 6 * control[1] + control[2]
            if normal_luma - control_luma > 200:
                darker += 1
            if any(abs(normal[channel] - control[channel]) > 20
                   for channel in range(3)):
                changed += 1
    return darker, changed, (x1 - x0) * (y1 - y0)


# The numbered placard rectangle shared by roster_counts(),
# placard_colour_count() and placard_changed_pixels(), in ROM logical pixels.
PLACARD_LOGICAL_AREA = 34 * 36
# Absolute floor for "the placard is really on screen", as a fraction of that
# rectangle. Same value the browser sibling pins for the same rectangle.
PLACARD_MIN_COVERAGE = 0.15


def placard_colour_count(path: Path, player: int,
                         geometry: FrameGeometry) -> tuple[int, int]:
    width, height, pixels = read_ppm(path)
    count = 0
    for y in range(geometry.y(172), geometry.y(208)):
        for x in range(geometry.x(158), geometry.x(192)):
            offset = (y * width + x) * 3
            red, green, blue = pixels[offset:offset + 3]
            if player == 2:
                matched = (blue > 130 and blue > red * 1.5 and
                           blue > green * 1.3)
            elif player == 3:
                matched = (green > 130 and green > red * 1.5 and
                           green > blue * 1.4)
            else:
                matched = (red > 130 and blue > 130 and green < 110 and
                           red > green * 1.5 and blue > green * 1.5)
            count += matched
    return count, geometry.area(PLACARD_LOGICAL_AREA)


def placard_changed_pixels(before: Path, after: Path,
                           geometry: FrameGeometry) -> tuple[int, int]:
    """Return meaningful changes in Taj's own numbered-placard rectangle.

    A colour-only P3 check is unsound: the lower centre of this screen contains
    green grass even if Taj's placard was never drawn.  Comparing the exact
    same rectangle before and after the controller arrives at Taj makes the
    sign an observable event rather than a background-colour coincidence.
    """
    width, height, before_pixels = read_ppm(before)
    after_width, after_height, after_pixels = read_ppm(after)
    if (width, height) != (after_width, after_height):
        raise ValueError("placard frames have different dimensions")
    changed = 0
    for y in range(geometry.y(172), geometry.y(208)):
        for x in range(geometry.x(158), geometry.x(192)):
            offset = (y * width + x) * 3
            if any(abs(before_pixels[offset + channel] -
                       after_pixels[offset + channel]) > 20
                   for channel in range(3)):
                changed += 1
    return changed, geometry.area(PLACARD_LOGICAL_AREA)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--roms",
        help="directory of raw revision ROMs used with --require-pal",
    )
    parser.add_argument(
        "--require-pal", action="store_true",
        help="identify and qualify the supported European Rev 1 ROM from --roms",
    )
    parser.add_argument(
        "--layout", choices=tuple(layout.name for layout in LAYOUTS),
        help="run one authored unlock layout (default: all four)")
    parser.add_argument(
        "--evidence-dir",
        help="keep rendered frames and logs under this directory")
    parser.add_argument(
        "--aspect", choices=("4:3", "16:9", "21:9"),
        help="override the presentation aspect for a focused safe-area run")
    parser.add_argument(
        "--renderer", choices=("opengl", "webgpu"), default="opengl",
        help="renderer backend for captured picker evidence")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if args.require_pal:
        rom = find_pal_v80(Path(args.roms).resolve()) if args.roms else None
        if rom is None:
            print(
                "check_taj_character_select: FAIL -- supported PAL Rev 1 ROM "
                "not found in --roms",
                file=sys.stderr,
            )
            return 1
    failures: list[str] = []
    if not binary.exists() or not rom.exists():
        print("check_taj_character_select: FAIL -- missing binary or ROM", file=sys.stderr)
        return 1

    temporary_context = (
        tempfile.TemporaryDirectory(prefix="mdkr-taj-select-")
        if args.evidence_dir is None else None
    )
    temporary = (
        temporary_context.__enter__() if temporary_context is not None
        else args.evidence_dir
    )
    try:
        root = Path(temporary)
        root.mkdir(parents=True, exist_ok=True)
        layouts = tuple(
            layout for layout in LAYOUTS
            if args.layout is None or layout.name == args.layout)
        for layout in layouts:
            run_dir = root / layout.name
            save_dir = run_dir / "save"
            frames_dir = run_dir / "frames"
            save_dir.mkdir(parents=True)
            frames_dir.mkdir()
            (save_dir / "eeprom.bin").write_bytes(save_image(layout))
            (save_dir / "taj_mod_state.ini").write_text(STATE_TEXT, encoding="ascii")
            script = run_dir / "select.txt"
            script.write_text(input_script(layout), encoding="ascii")
            env = {
                key: value for key, value in os.environ.items()
                if not key.startswith(("MDKR", "GE007_"))
            }
            env.update(
                LC_ALL="C",
                MDKR_AUDIO="0",
                MDKR_TRACE="1",
                MDKR_TAJ_SELECT_TRACE="1",
                MDKR_TAJ_SELECT_FAIL_SPAWNS="1",
                MDKR_TAJ_SELECT_FAIL_SIGN_SPAWNS="1",
                MDKR_RENDERER=args.renderer,
                MDKR_RENDER_SCALE="1",
                MDKR_SAVE_DIR=str(save_dir),
                MDKR_DUMP_FROM=str(UNSELECTED_FRAME),
                MDKR_DUMP_EVERY="20",
                MDKR64_HIDDEN="1",
            )
            if args.aspect:
                env["MDKR_ASPECT"] = args.aspect
                env["MDKR_WIDESCREEN"] = "1"
            command = [
                str(binary), "--headless-frames", str(FRAMES),
                "--input-script", str(script),
                "--dump-frames", str(frames_dir), "--rom", str(rom),
            ]
            if args.verbose:
                print("$ " + " ".join(command), flush=True)
            process = subprocess.run(
                command, cwd=run_dir, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=90, check=False,
            )
            output = process.stdout or ""
            if args.evidence_dir is not None:
                (run_dir / "run.log").write_text(output, encoding="utf-8")
            if args.verbose:
                for line in output.splitlines():
                    if any(marker in line for marker in (
                            "[TAJSELECT]", "taj_roster:", "taj_select:",
                            "[CRASH]", "[FATAL]", "runtime error:")):
                        print(line)
            if process.returncode != 0:
                failures.append(f"{layout.name}: exit code {process.returncode}")
            for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
                if marker in output:
                    failures.append(f"{layout.name}: fatal marker {marker}")

            roster = (
                f"taj_roster: base={layout.base_count} taj={layout.base_count} "
                f"enabled=1 drumstick={int(layout.drumstick)} tt={int(layout.tt)}"
            )
            if roster not in output:
                failures.append(f"{layout.name}: missing contiguous roster marker {roster!r}")
            if "taj_select: player=0 controller=0 selected=1 donor=9" not in output:
                failures.append(f"{layout.name}: controller route did not select Taj")
            if not any(
                    "[TAJSELECT] event=compose" in line and "model=208" in line
                    for line in output.splitlines()):
                failures.append(
                    f"{layout.name}: Park Warden model was not composed")
            if not any(
                    "[TAJSELECT] event=sign_compose" in line and
                    "sign_model=302" in line
                    for line in output.splitlines()):
                failures.append(
                    f"{layout.name}: authored player placard was not composed")
            if "[TAJSELECT] event=compose_retry" not in output:
                failures.append(
                    f"{layout.name}: transient actor allocation was not retried")
            if "[TAJSELECT] event=sign_retry" not in output:
                failures.append(
                    f"{layout.name}: transient placard allocation was not retried")
            if "[TAJSELECT] event=sign_schema_verified" not in output:
                failures.append(
                    f"{layout.name}: placard-only batch schema was not verified")
            production_shadows = [
                float(match.group(1))
                for match in re.finditer(
                    r"\[TAJSELECT\] event=animation .*shadow=([0-9.]+)",
                    output,
                )
            ]
            if (not production_shadows or
                    any(not 0.0 < shadow < 3.0
                        for shadow in production_shadows)):
                failures.append(
                    f"{layout.name}: production Taj did not retain a reduced, "
                    f"nonzero grounding shadow ({production_shadows=})")
            for character in range(layout.base_count):
                label = retail_label(layout, character)
                marker = (
                    f"taj_hover: controller=0 index={character} taj=0 "
                    f"visual=2 sign=0 label={label}"
                )
                if marker not in output:
                    failures.append(
                        f"{layout.name}: ordinary character {character} did not "
                        f"retain identity {label} with Taj's placard hidden")
            taj_marker = (
                f"taj_hover: controller=0 index={layout.base_count} taj=1 "
                "visual=2 sign=1 label=TAJ"
            )
            if taj_marker not in output:
                failures.append(
                    f"{layout.name}: virtual slot did not resolve exclusively "
                    "to the TAJ trace identity")

            unselected = frames_dir / f"frame_{UNSELECTED_FRAME:04d}.ppm"
            selected = frames_dir / f"frame_{SELECTED_FRAME:04d}.ppm"
            selected_motion = (
                frames_dir / f"frame_{SELECTED_MOTION_FRAME:04d}.ppm")
            try:
                # The authored envelope is inset and smaller than the frame
                # under a forced aspect; every logical sampling box below is
                # mapped through the geometry rather than assuming the scene
                # fills the capture.
                geometry = FrameGeometry.for_frame(unselected, args.aspect)
                unselected_counts = roster_counts(unselected, geometry)
                selected_counts = roster_counts(selected, geometry)
                left_row, right_row, row_scale = row_side_counts(
                    unselected, geometry)
                motion = roster_motion(selected, selected_motion, geometry)
            except (OSError, ValueError) as error:
                failures.append(f"{layout.name}: rendered actor evidence failed: {error}")
                continue
            purple, ordinary_red, _ = unselected_counts
            _, selected_red, _ = selected_counts
            # Thresholds are stated per ROM-logical pixel, so they follow the
            # authored envelope's real size in the capture: a 21:9
            # presentation letterboxes 320x240 into fewer pixels than a 4:3
            # one, and demands proportionally fewer of them.
            pixel_scale = geometry.pixel_scale
            if purple < 500 * pixel_scale:
                failures.append(
                    f"{layout.name}: Taj model is not visibly present "
                    f"({purple=})"
                )
            if (left_row < 1500 * row_scale or
                    right_row < 1500 * row_scale):
                failures.append(
                    f"{layout.name}: authored lower-row actors collapsed or "
                    f"left the safe area ({left_row=}, {right_row=})")
            # A purely relative "4x the unselected red" test is vacuous at the
            # baseline it actually has: with Taj's placard hidden the rectangle
            # contains no red at all, so ordinary_red is 0, the bound is 0, and
            # ANY selected_red -- including 0, i.e. no placard drawn -- passes.
            # Use the same max(relative, absolute-floor) shape as the browser
            # sibling (check_browser_taj_character_select.py), measured over the
            # identical 34x36 logical placard rectangle roster_counts() samples.
            placard_region = geometry.area(PLACARD_LOGICAL_AREA)
            placard_floor = int(placard_region * PLACARD_MIN_COVERAGE)
            if selected_red < max(ordinary_red * 4, placard_floor):
                failures.append(
                    f"{layout.name}: Taj did not raise the P1 placard "
                    f"({ordinary_red=}, {selected_red=}, "
                    f"floor={placard_floor}, region={placard_region})"
                )
            if motion < 300 * pixel_scale:
                failures.append(
                    f"{layout.name}: Taj/placard hover pose did not animate "
                    f"({motion=})")

            # Render the exact pre-fix shadow policy as a negative control.
            # Production must keep a small grounding decal, while the control's
            # full Park Warden footprint materially darkens the grass behind
            # the lower row. Limit this extra renderer run to the base layout;
            # the same scale-normalisation helper owns every roster shape.
            # The control runs at every requested aspect - the ultrawide task
            # previously skipped it entirely, asserting strictly less than the
            # default task's own base iteration.
            if layout.name == "base":
                control_dir = root / "base-unscaled-shadow-control"
                control_save = control_dir / "save"
                control_frames = control_dir / "frames"
                control_save.mkdir(parents=True)
                control_frames.mkdir()
                (control_save / "eeprom.bin").write_bytes(save_image(layout))
                (control_save / "taj_mod_state.ini").write_text(
                    STATE_TEXT, encoding="ascii")
                control_script = control_dir / "select.txt"
                control_script.write_text(input_script(layout), encoding="ascii")
                control_env = dict(env)
                control_env.update(
                    MDKR_SAVE_DIR=str(control_save),
                    MDKR_TAJ_SELECT_UNSCALED_SHADOW="1",
                )
                control_process = subprocess.run(
                    [str(binary), "--headless-frames", str(FRAMES),
                     "--input-script", str(control_script), "--dump-frames",
                     str(control_frames), "--rom", str(rom)],
                    cwd=control_dir, env=control_env, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    timeout=90, check=False,
                )
                control_output = control_process.stdout or ""
                if args.evidence_dir is not None:
                    (control_dir / "run.log").write_text(
                        control_output, encoding="utf-8")
                if control_process.returncode != 0:
                    failures.append(
                        "base: unscaled-shadow control exited non-zero")
                if not any(
                        "[TAJSELECT]" in line and "shadow=3.000000" in line
                        for line in control_output.splitlines()):
                    failures.append(
                        "base: unscaled-shadow control did not restore the "
                        "full Park Warden decal")
                try:
                    darker, changed, shadow_region = oversized_shadow_delta(
                        unselected,
                        control_frames / f"frame_{UNSELECTED_FRAME:04d}.ppm",
                        geometry)
                    if (darker < shadow_region * 0.08 or
                            changed < shadow_region * 0.10):
                        failures.append(
                            "base: rendered shadow oracle did not reject the "
                            "full-size Park Warden decal "
                            f"({darker=}, {changed=}, {shadow_region=})")
                except (OSError, ValueError) as error:
                    failures.append(
                        f"base: rendered shadow-control evidence failed: {error}")

        # Each controller owns the same authored P1..P4 placard convention as
        # the retail cast. Run the physical navigation for P2, P3, and P4;
        # earlier players remain on distinct ordinary characters.
        if args.layout in (None, "base"):
            player_routes = {
                2: ((1450, "A", 2), (1510, "DOWN", 2),
                    (1570, "RIGHT", 2), (1630, "RIGHT", 2)),
                3: ((1450, "A", 2), (1490, "A", 3),
                    (1550, "DOWN", 3), (1610, "LEFT", 3)),
                4: ((1450, "A", 2), (1490, "A", 3), (1530, "A", 4),
                    (1590, "DOWN", 4), (1650, "LEFT", 4),
                    (1710, "LEFT", 4)),
            }
            for player, actions in player_routes.items():
                player_dir = root / f"p{player}-placard"
                player_save = player_dir / "save"
                player_dir.mkdir()
                player_save.mkdir()
                (player_save / "eeprom.bin").write_bytes(
                    save_image(LAYOUTS[0]))
                (player_save / "taj_mod_state.ini").write_text(
                    STATE_TEXT, encoding="ascii")
                player_script = player_dir / "select.txt"
                lines = ["1250 START 4 P1", "1330 START 4 P1"]
                lines.extend(
                    f"{frame} {button} 4 P{port}"
                    for frame, button, port in actions
                )
                player_script.write_text("\n".join(lines) + "\n",
                                         encoding="ascii")
                player_frames = player_dir / "frames"
                player_frames.mkdir()
                player_env = {
                    key: value for key, value in os.environ.items()
                    if not key.startswith(("MDKR", "GE007_"))
                }
                player_env.update(
                    LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
                    MDKR_TAJ_SELECT_TRACE="1", MDKR_RENDERER=args.renderer,
                    MDKR_RENDER_SCALE="1", MDKR_SAVE_DIR=str(player_save),
                    # Keep a pre-Taj frame so the placard oracle can reject
                    # static grass/background pixels that happen to resemble
                    # a controller colour, particularly P3 green.
                    MDKR_DUMP_FROM="1440", MDKR_DUMP_EVERY="20",
                    MDKR64_HIDDEN="1",
                )
                # A focused aspect run has to reach the per-controller
                # placards too; without this the P2..P4 routes silently
                # re-ran the 4:3 arm's own measurements.
                if args.aspect:
                    player_env["MDKR_ASPECT"] = args.aspect
                    player_env["MDKR_WIDESCREEN"] = "1"
                player_process = subprocess.run(
                    [str(binary), "--headless-frames", "1840",
                     "--input-script", str(player_script), "--dump-frames",
                     str(player_frames), "--rom", str(rom)],
                    cwd=player_dir, env=player_env, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    timeout=90, check=False,
                )
                player_output = player_process.stdout or ""
                if args.evidence_dir is not None:
                    (player_dir / "run.log").write_text(
                        player_output, encoding="utf-8")
                expected_state = (
                    f"hover=0x{1 << (player - 1):X} confirmed=0x0 "
                    f"sign_player={player - 1}"
                )
                placard_bound = any(
                    expected_state in line and "animation=1" in line
                    for line in player_output.splitlines()
                    if "[TAJSELECT]" in line
                )
                if player_process.returncode != 0 or not placard_bound:
                    failures.append(
                        f"P{player}: authored placard did not bind to its "
                        f"controller ({expected_state!r}, animation=1)")
                try:
                    player_frame = player_frames / "frame_1760.ppm"
                    before_tick = ((actions[-1][0] // 20) - 1) * 20
                    before_frame = player_frames / (
                        f"frame_{before_tick:04d}.ppm")
                    player_geometry = FrameGeometry.for_frame(
                        player_frame, args.aspect)
                    purple, _, _ = roster_counts(player_frame, player_geometry)
                    pixel_scale = player_geometry.pixel_scale
                    placard_pixels, placard_region = placard_colour_count(
                        player_frame, player, player_geometry)
                    placard_motion, motion_region = placard_changed_pixels(
                        before_frame, player_frame, player_geometry)
                    if purple < 500 * pixel_scale:
                        failures.append(
                            f"P{player}: Taj actor was not visible beside the "
                            f"P{player} placard ({purple=})")
                    if placard_pixels < placard_region * 0.20:
                        failures.append(
                            f"P{player}: rendered placard did not use the "
                            f"authored P{player} colour "
                            f"({placard_pixels=}/{placard_region})")
                    if (motion_region != placard_region or
                            placard_motion < placard_region * 0.12):
                        failures.append(
                            f"P{player}: Taj's own placard rectangle did not "
                            f"change when the controller arrived "
                            f"({placard_motion=}/{placard_region})")
                except (OSError, ValueError) as error:
                    failures.append(
                        f"P{player}: rendered placard evidence failed: {error}")

        # A permanently unavailable presentation must fail closed. The cursor
        # skips Taj's virtual slot and the confirmation route selects an
        # ordinary character instead of accepting an invisible Taj.
        if args.layout in (None, "base"):
            failure_dir = root / "presentation-unavailable"
            failure_save = failure_dir / "save"
            failure_dir.mkdir()
            failure_save.mkdir()
            base = LAYOUTS[0]
            (failure_save / "eeprom.bin").write_bytes(save_image(base))
            (failure_save / "taj_mod_state.ini").write_text(
                STATE_TEXT, encoding="ascii")
            failure_script = failure_dir / "select.txt"
            failure_script.write_text(input_script(base), encoding="ascii")
            failure_env = {
                key: value for key, value in os.environ.items()
                if not key.startswith(("MDKR", "GE007_"))
            }
            failure_env.update(
                LC_ALL="C",
                MDKR_AUDIO="0",
                MDKR_TRACE="1",
                MDKR_TAJ_SELECT_TRACE="1",
                MDKR_TAJ_SELECT_FAIL_SPAWNS="100",
                MDKR_RENDERER=args.renderer,
                MDKR_SAVE_DIR=str(failure_save),
                MDKR64_HIDDEN="1",
            )
            if args.aspect:
                failure_env["MDKR_ASPECT"] = args.aspect
                failure_env["MDKR_WIDESCREEN"] = "1"
            failure_process = subprocess.run(
                [str(binary), "--headless-frames", str(FRAMES),
                 "--input-script", str(failure_script), "--rom", str(rom)],
                cwd=failure_dir, env=failure_env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=90, check=False,
            )
            failure_output = failure_process.stdout or ""
            if args.evidence_dir is not None:
                (failure_dir / "run.log").write_text(
                    failure_output, encoding="utf-8")
            if failure_process.returncode != 0:
                failures.append(
                    "presentation-unavailable: non-zero process exit")
            if "[TAJSELECT] event=presentation_unavailable" not in failure_output:
                failures.append(
                    "presentation-unavailable: retry budget never failed closed")
            if "taj_select: player=0 controller=0 selected=1 donor=9" in failure_output:
                failures.append(
                    "presentation-unavailable: invisible Taj remained selectable")
            if "taj_select: player=0 controller=0 selected=0" not in failure_output:
                failures.append(
                    "presentation-unavailable: ordinary fallback was not selected")
            for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                           "runtime error:"):
                if marker in failure_output:
                    failures.append(
                        f"presentation-unavailable: fatal marker {marker}")

    finally:
        if temporary_context is not None:
            temporary_context.__exit__(None, None, None)

    if failures:
        for failure in failures:
            print(f"check_taj_character_select: FAIL -- {failure}", file=sys.stderr)
        return 1
    if args.layout:
        print(
            "check_taj_character_select: PASS -- modeled, animated, and "
            f"selectable Taj actor in {args.layout} roster")
    else:
        print(
            "check_taj_character_select: PASS -- modeled, animated, and selectable "
            "Taj actor across 8+Taj, 9+Taj, 9+Taj, and 10+Taj rosters"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
