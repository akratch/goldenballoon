#!/usr/bin/env python3
"""Internal replay-mechanism coverage for per-viewport cameras.

The snapshot unit test proves the immutable authored-camera latch and its
fail-closed controls. This gate proves game wiring that a synthetic unit cannot:
a real 2P race captures camera 1, a real 3P race captures the fourth TT spectator
camera, and a real WebGPU cinematic captures cutscene bank 4. Late, tightly
bounded PPM windows exercise the quarantined replay mechanism. Production 1.0.1
never enables delayed replay; the presentation matrix covers that invariant.

The 3P arm is especially important: gNumCameras is three even when the TT
spectator is drawn into the fourth quadrant.  A sequential ``0..gNumCameras``
snapshot walk therefore silently omits camera 3 while the rest of the game
looks healthy.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT_2P = ROOT / "tests/input_scripts/race_2p_split.txt"
SCRIPT_3P_TT = ROOT / "tests/input_scripts/race_3p_tt_camera.txt"
SCRIPT_CUTSCENE = ROOT / "tests/input_scripts/adventure_hub_drive.txt"
PRESENTS = 6400
TICKS = PRESENTS // 2
DUMP_FROM = 6320
CUTSCENE_PRESENTS = 9200
CUTSCENE_DUMP_FROM = 9120

SNAPSHOT_RE = re.compile(r"\[SNAPSHOT\] (.+)")
SCHED_RE = re.compile(r"\[PRESENTSCHED-SUMMARY\] (.+)")
CAMERA_VP_RE = re.compile(r"\[CAMERA-VP\] (.+)")
LOAD_RE = re.compile(r"level_load: levelId=5 numPlayers=(-?\d+)")
CUTSCENE_LOAD_RE = re.compile(
    r"level_load: levelId=36 numPlayers=-?\d+.*cutscene=15")
HUD_RE = re.compile(r"hud_init: hudPlayers=(\d+) numViewports=(\d+)")


@dataclass(frozen=True)
class Arm:
    name: str
    players_encoded: int
    script: Path
    camera_id: int
    crop: tuple[float, float, float, float]
    min_captures: int
    min_interpolations: int


ARMS = (
    # DKR's two-player layout is horizontal: camera 1 owns the lower half.
    Arm("2p-camera1", 1, SCRIPT_2P, 1, (0.03, 0.97, 0.53, 0.97),
        500, 1000),
    # The production CRIGHT edge switches the lower-right quadrant from the
    # minimap to camera 3's TT spectator.
    Arm("3p-tt-camera3", 2, SCRIPT_3P_TT, 3,
        (0.53, 0.97, 0.53, 0.97), 250, 500),
)


def parse_fields(output: str, pattern: re.Pattern[str], label: str) -> dict[str, int]:
    match = None
    for match in pattern.finditer(output):
        pass
    if match is None:
        raise RuntimeError(f"no [{label}] row was emitted")
    fields: dict[str, int] = {}
    for token in match.group(1).split():
        key, separator, value = token.partition("=")
        if not separator:
            continue
        try:
            fields[key] = int(value)
        except ValueError:
            continue
    return fields


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    index = 0
    fields: list[bytes] = []
    while len(fields) < 4:
        while index < len(data) and data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b"#":
            while index < len(data) and data[index:index + 1] != b"\n":
                index += 1
            continue
        end = index
        while end < len(data) and not data[end:end + 1].isspace():
            end += 1
        fields.append(data[index:end])
        index = end
    if fields[0] != b"P6" or fields[3] != b"255":
        raise RuntimeError(f"unsupported PPM header in {path.name}")
    # P6 has exactly one whitespace delimiter after maxval (CRLF counts as one
    # line ending). Do not skip arbitrary whitespace here: the first pixel is
    # binary and may legitimately start with 0x09/0x0a/0x20.
    if data[index:index + 2] == b"\r\n":
        index += 2
    elif index < len(data) and data[index:index + 1].isspace():
        index += 1
    else:
        raise RuntimeError(f"missing PPM pixel delimiter in {path.name}")
    width, height = int(fields[1]), int(fields[2])
    pixels = data[index:]
    if len(pixels) != width * height * 3:
        raise RuntimeError(
            f"{path.name}: {len(pixels)} pixel bytes, expected {width * height * 3}")
    return width, height, pixels


def crop_bytes(frame: tuple[int, int, bytes],
               crop: tuple[float, float, float, float]) -> bytes:
    width, height, pixels = frame
    x0 = max(0, min(width, int(width * crop[0])))
    x1 = max(x0 + 1, min(width, int(width * crop[1])))
    y0 = max(0, min(height, int(height * crop[2])))
    y1 = max(y0 + 1, min(height, int(height * crop[3])))
    rows = []
    for y in range(y0, y1):
        start = (y * width + x0) * 3
        rows.append(pixels[start:start + (x1 - x0) * 3])
    return b"".join(rows)


def intermediate_verdict(
    frames: dict[int, tuple[int, int, bytes]],
    crop: tuple[float, float, float, float],
    freeze_intermediates: bool = False,
) -> tuple[int, int]:
    """Return (candidates, intermediates distinct from both neighbours)."""
    candidates = moved = 0
    for index in sorted(frames):
        if index % 2 != 1 or index - 1 not in frames or index + 1 not in frames:
            continue
        before = crop_bytes(frames[index - 1], crop)
        current = (before if freeze_intermediates
                   else crop_bytes(frames[index], crop))
        after = crop_bytes(frames[index + 1], crop)
        candidates += 1
        if current != before and current != after:
            moved += 1
    return candidates, moved


def viewport_quality(frame: tuple[int, int, bytes],
                     crop: tuple[float, float, float, float]) -> tuple[int, float]:
    pixels = crop_bytes(frame, crop)
    colours = {pixels[index:index + 3] for index in range(0, len(pixels), 3)}
    luma = [
        0.2126 * pixels[index] + 0.7152 * pixels[index + 1] +
        0.0722 * pixels[index + 2]
        for index in range(0, len(pixels), 3)
    ]
    return len(colours), statistics.pstdev(luma)


def run_arm(binary: Path, rom: Path, root: Path, arm: Arm,
            timeout: int, verbose: bool) -> tuple[str, Path]:
    run_dir = root / arm.name
    frame_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frame_dir.mkdir(parents=True)
    save_dir.mkdir()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="original",
        MDKR_SYNTH_FIELDS="2",
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_RENDERER="gl",
        MDKR_PRESENT_RATE="60",
        MDKR_INTERNAL_TEST_TOKEN="mdkr64-presentation-replay-v1",
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_TEST_PRESENTATION_REPLAY="1",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_PRESENT_SNAPSHOT="1",
        MDKR_TEST_CAMERA_VP_ENDPOINTS="1",
        MDKR_DUMP_FROM=str(DUMP_FROM),
        MDKR_SAVE_DIR=str(save_dir),
    )
    command = [
        str(binary), "--headless-frames", str(PRESENTS),
        "--window-size", "320x240", "--input-script", str(arm.script),
        "--dump-frames", str(frame_dir), "--rom", str(rom),
    ]
    if verbose:
        print(f"$ ({arm.name}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"{arm.name}: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise RuntimeError(f"{arm.name}: fatal marker {marker}")
    return output, frame_dir


def check_arm(output: str, frame_dir: Path, arm: Arm) -> tuple[list[str], str]:
    failures: list[str] = []
    try:
        snapshot = parse_fields(output, SNAPSHOT_RE, "SNAPSHOT")
        sched = parse_fields(output, SCHED_RE, "PRESENTSCHED-SUMMARY")
        camera_vp = parse_fields(output, CAMERA_VP_RE, "CAMERA-VP")
    except RuntimeError as error:
        return [f"{arm.name}: {error}"], ""

    if (str(arm.players_encoded) not in LOAD_RE.findall(output) or
            (str(arm.players_encoded), str(arm.players_encoded + 1))
            not in HUD_RE.findall(output)):
        failures.append(
            f"{arm.name}: level 5 never loaded with the expected "
            f"{arm.players_encoded + 1}-player HUD")

    for key, expected in (("ticks", TICKS), ("presents", PRESENTS),
                          ("tickfields", 2), ("presentrate", 60)):
        if sched.get(key) != expected:
            failures.append(
                f"{arm.name}: scheduler {key}={sched.get(key)}, expected {expected}")
    if sched.get("interp", 0) < TICKS - 32:
        failures.append(
            f"{arm.name}: only {sched.get('interp', 0)} interpolated presents")
    if sched.get("interpviews", 0) <= 0:
        failures.append(f"{arm.name}: no interpolated view-projection was consumed")
    if snapshot.get("overflows") != 0:
        failures.append(
            f"{arm.name}: snapshot overflows={snapshot.get('overflows')}")
    if (camera_vp.get("alpha0checks", 0) <= 0 or
            camera_vp.get("alpha0mismatch", -1) != 0 or
            camera_vp.get("alpha0expected", 0) == 0 or
            camera_vp.get("alpha0expected") != camera_vp.get("alpha0actual")):
        failures.append(
            f"{arm.name}: alpha-zero VP is not byte-exact to the retained "
            "task's authored camera")
    if (camera_vp.get("nextchecks", 0) <= 0 or
            camera_vp.get("nextmismatch", -1) != 0 or
            camera_vp.get("nextexpected", 0) == 0 or
            camera_vp.get("nextexpected") != camera_vp.get("nextactual")):
        failures.append(
            f"{arm.name}: the prior alpha-one target did not become the next "
            "task's exact authored VP")
    if (camera_vp.get("midpointchecks", 0) <= 0 or
            camera_vp.get("midpointmoved", 0) <= 0 or
            camera_vp.get("midpointhash", 0) == 0):
        failures.append(
            f"{arm.name}: no semantic midpoint VP movement was observed")
    if camera_vp.get("mutationreject") != camera_vp.get("alpha0checks"):
        failures.append(
            f"{arm.name}: exact VP mutation control rejected "
            f"{camera_vp.get('mutationreject')}/"
            f"{camera_vp.get('alpha0checks')} one-bit mutations")

    camera_key = f"cam{arm.camera_id}"
    interp_key = f"caminterp{arm.camera_id}"
    captures = snapshot.get(camera_key, 0)
    interpolations = snapshot.get(interp_key, 0)
    if (snapshot.get("camera_mask", 0) & (1 << arm.camera_id)) == 0:
        failures.append(
            f"{arm.name}: camera_mask={snapshot.get('camera_mask', 0)} omits "
            f"camera {arm.camera_id}")
    if captures < arm.min_captures:
        failures.append(
            f"{arm.name}: camera {arm.camera_id} captured {captures} ticks, "
            f"want >= {arm.min_captures}")
    if interpolations < arm.min_interpolations:
        failures.append(
            f"{arm.name}: camera {arm.camera_id} resolved only "
            f"{interpolations} interpolated poses, want >= "
            f"{arm.min_interpolations}")

    frames: dict[int, tuple[int, int, bytes]] = {}
    try:
        for path in sorted(frame_dir.glob("frame_*.ppm")):
            frames[int(path.stem.split("_")[1])] = read_ppm(path)
    except (OSError, RuntimeError, ValueError) as error:
        failures.append(f"{arm.name}: could not read PPM witness: {error}")
        return failures, ""

    candidates, moved = intermediate_verdict(frames, arm.crop)
    control_candidates, control_moved = intermediate_verdict(
        frames, arm.crop, freeze_intermediates=True)
    if candidates < 30:
        failures.append(
            f"{arm.name}: only {candidates} bounded intermediate-frame pairs")
    if moved < 5:
        failures.append(
            f"{arm.name}: target viewport moved on only {moved}/{candidates} "
            "intermediate frames")
    if control_candidates != candidates or control_moved != 0:
        failures.append(
            f"{arm.name}: frozen-intermediate detector control was not "
            f"rejected ({control_moved}/{control_candidates} accepted)")

    colours = 0
    sigma = 0.0
    if frames:
        colours, sigma = viewport_quality(frames[max(frames)], arm.crop)
        if colours < 200 or not math.isfinite(sigma) or sigma < 8.0:
            failures.append(
                f"{arm.name}: target viewport looks blank: colours={colours}, "
                f"luma sigma={sigma:.1f}")

    note = (
        f"{arm.name}: camera {arm.camera_id} captured {captures} ticks and "
        f"resolved {interpolations} interpolated poses; target crop moved on "
        f"{moved}/{candidates} intermediate presents "
        f"({colours} colours, luma sigma {sigma:.1f}); frozen control 0/"
        f"{control_candidates}; {camera_vp.get('alpha0checks', 0)} exact "
        f"alpha-zero and {camera_vp.get('nextchecks', 0)} chained alpha-one "
        "endpoint checks")
    return failures, note


def run_cutscene_arm(binary: Path, rom: Path, root: Path, timeout: int,
                     verbose: bool) -> tuple[str, Path]:
    run_dir = root / "cutscene-camera4"
    frame_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frame_dir.mkdir(parents=True)
    save_dir.mkdir()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="original",
        MDKR_SYNTH_FIELDS="2",
        MDKR_TRACE="1",
        MDKR_RENDERER="webgpu",
        MDKR_PRESENT_RATE="60",
        MDKR_INTERNAL_TEST_TOKEN="mdkr64-presentation-replay-v1",
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_TEST_PRESENTATION_REPLAY="1",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_PRESENT_SNAPSHOT="1",
        MDKR_TEST_CAMERA_VP_ENDPOINTS="1",
        MDKR_DUMP_FROM=str(CUTSCENE_DUMP_FROM),
        MDKR_SAVE_DIR=str(save_dir),
    )
    command = [
        str(binary), "--headless-frames", str(CUTSCENE_PRESENTS),
        "--window-size", "320x240", "--input-script", str(SCRIPT_CUTSCENE),
        "--dump-frames", str(frame_dir), "--rom", str(rom),
    ]
    if verbose:
        print(f"$ (cutscene-camera4) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"cutscene-camera4: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise RuntimeError(f"cutscene-camera4: fatal marker {marker}")
    return output, frame_dir


def check_cutscene_arm(output: str, frame_dir: Path) -> tuple[list[str], str]:
    failures: list[str] = []
    try:
        snapshot = parse_fields(output, SNAPSHOT_RE, "SNAPSHOT")
        sched = parse_fields(output, SCHED_RE, "PRESENTSCHED-SUMMARY")
        camera_vp = parse_fields(output, CAMERA_VP_RE, "CAMERA-VP")
    except RuntimeError as error:
        return [f"cutscene-camera4: {error}"], ""
    if CUTSCENE_LOAD_RE.search(output) is None:
        failures.append("cutscene-camera4: adventure intro cutscene never loaded")
    if sched.get("presents") != CUTSCENE_PRESENTS:
        failures.append(
            f"cutscene-camera4: presents={sched.get('presents')}, expected "
            f"{CUTSCENE_PRESENTS}")
    captures = snapshot.get("cam4", 0)
    interpolations = snapshot.get("caminterp4", 0)
    if (snapshot.get("camera_mask", 0) & (1 << 4)) == 0:
        failures.append(
            f"cutscene-camera4: camera_mask={snapshot.get('camera_mask')} "
            "omits the authored cutscene bank")
    if captures < 500 or interpolations < 500:
        failures.append(
            "cutscene-camera4: insufficient authored bank-4 activity "
            f"(captures={captures}, interpolations={interpolations})")
    if (camera_vp.get("alpha0checks", 0) <= 0 or
            camera_vp.get("alpha0mismatch", -1) != 0 or
            camera_vp.get("alpha0expected", 0) == 0 or
            camera_vp.get("alpha0expected") != camera_vp.get("alpha0actual")):
        failures.append(
            "cutscene-camera4: bank-4 alpha-zero VP is not byte-exact")
    if (camera_vp.get("nextchecks", 0) <= 0 or
            camera_vp.get("nextmismatch", -1) != 0 or
            camera_vp.get("nextexpected", 0) == 0 or
            camera_vp.get("nextexpected") != camera_vp.get("nextactual")):
        failures.append(
            "cutscene-camera4: bank-4 alpha-one endpoint chain is not exact")
    if (camera_vp.get("midpointmoved", 0) <= 0 or
            camera_vp.get("mutationreject") !=
            camera_vp.get("alpha0checks")):
        failures.append(
            "cutscene-camera4: midpoint or one-bit semantic control is absent")
    frames: dict[int, tuple[int, int, bytes]] = {}
    try:
        for path in sorted(frame_dir.glob("frame_*.ppm")):
            frames[int(path.stem.split("_")[1])] = read_ppm(path)
    except (OSError, RuntimeError, ValueError) as error:
        failures.append(f"cutscene-camera4: could not read PPM witness: {error}")
    candidates, moved = intermediate_verdict(frames, (0.0, 1.0, 0.0, 1.0))
    if candidates < 30 or moved < 5:
        failures.append(
            f"cutscene-camera4: bank-4 midpoint witness moved on "
            f"{moved}/{candidates} frames")
    return failures, (
        f"cutscene-camera4: camera 4 captured {captures} ticks and resolved "
        f"{interpolations} interpolated poses; full frame moved on "
        f"{moved}/{candidates} midpoint presents; "
        f"{camera_vp.get('alpha0checks', 0)} exact alpha-zero and "
        f"{camera_vp.get('nextchecks', 0)} chained alpha-one endpoint checks")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT_2P, SCRIPT_3P_TT, SCRIPT_CUTSCENE):
        if not path.is_file():
            print(f"check_camera_snapshot_coverage: FAIL\n  - missing {path}")
            return 1

    failures: list[str] = []
    notes: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-camera-snapshot-") as tmp:
        root = Path(tmp)
        for arm in ARMS:
            try:
                output, frame_dir = run_arm(
                    binary, rom, root, arm, args.timeout, args.verbose)
            except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
                failures.append(str(error))
                continue
            arm_failures, note = check_arm(output, frame_dir, arm)
            failures.extend(arm_failures)
            if note:
                notes.append(note)

        try:
            output, frame_dir = run_cutscene_arm(
                binary, rom, root, args.timeout, args.verbose)
            arm_failures, note = check_cutscene_arm(output, frame_dir)
            failures.extend(arm_failures)
            if note:
                notes.append(note)
        except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
            failures.append(str(error))

    if failures:
        print("check_camera_snapshot_coverage: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        for note in notes:
            print(f"  - observed: {note}")
        return 1

    print("check_camera_snapshot_coverage: PASS")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
