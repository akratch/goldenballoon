#!/usr/bin/env python3
"""The speed readout shows the car's real speed and disturbs nothing else.

Why this exists
---------------
`Enhancements.Speedometer` is declared `MDKR_ENH_PRESENTATION` in
`platform/enhancement_registry.c`. `tests/check_enhancement_authority.py` owns
half of that claim — that the readout cannot move the authoritative `[SIMHASH]`
stream. This gate owns the other half, which that one cannot see: that the
readout draws the right number, in the right place, and nowhere else.

Four arms, one captured frame each, from the same deterministic Time Trial
route:

  baseline   no override at all
  off        `=0`, explicitly
  mph        `=1`
  kmh        `=2`

What it asserts
---------------
1. `off` is byte-identical to `baseline`. An enhancement that is off must cost
   the frame nothing, not "almost nothing".

2. `mph` differs from `baseline` inside a bounded box in the bottom-left
   corner, by enough pixels to be a legible readout rather than a stray texel.

3. **Every byte of `mph` outside that box equals `baseline`.** This is the
   assertion the gate is worth having for. Appending anything to a display list
   that a HUD is still building is an invitation to leave the render mode, the
   primitive colour or the font selection somewhere the next authored element
   inherits it, and the symptom of that is a recoloured banana counter three
   draws later, not a crash. The box is checked to sit clear of every authored
   element — the weapon panel ends well above it — so "outside the box" really
   does mean "the entire rest of the interface and the whole world".

4. `kmh` differs from `mph`: kilometres per hour is not miles per hour, and a
   mode that silently aliased onto another would otherwise pass 1-3 happily.
   The two readings are also compared numerically at a shared frame.

5. Over the first second of a standing start the number never goes backwards
   AND actually climbs, read from the `[SPEEDO]` diagnostic line rather than
   from the pixels. Both halves are needed: "never goes backwards" alone is
   satisfied by a readout welded to a constant, which is exactly the positive
   control this assertion has to fail against.

6. All four arms produce the same `[SIMHASH]` v3 stream. This looks like
   `check_enhancement_authority.py`'s job, and formally it is — but that gate
   drives `nav_to_time_trial_race.txt` for 900 frames, which never gets out of
   the menus, so the readout never draws in it and its "presentation" verdict
   for this row is about the config key rather than about the effect. This
   route does reach a race with the readout on screen, so this is the arm where
   "drawing a speed cannot move authoritative state" is actually observed.

The `[SPEEDO]` line is emitted only under `MDKR_SPEEDO_TRACE=1` and prints the
same number the draw put on screen, taken from the same computation — so it is
a diagnostic of the readout, not a second implementation of it.

Every run is muted (`MDKR_AUDIO=0`) and headless (`--headless-frames`).
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

from check_native_ui_resolution import (Image, changed_pixels, inside,
                                        read_ppm, scaled_box)
# `read_ppm` above is check_native_ui_resolution's wrapper, which already
# returns its `Image`; harness_utils' returns the bare tuple.
from harness_utils import (ABORT_MARKERS, ASSERT_MARKERS, DEFAULT_BUILD_DIR,
                           find_fatal, resolve_binary)

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "race_drive_time_trial.txt"

# The route's countdown clears at frame 2859 and the car is still accelerating
# well past 3000, so a capture at 3150 lands mid-race with the readout showing a
# two-digit speed. FRAMES only has to outlast the capture.
CAPTURE_FRAME = 3150
FRAMES = 3200

# The readout's corner, as fractions of the captured image so the box survives a
# render-scale change. Measured: the readout occupies x 3%..19%, y 93%..99% of
# the frame, and the lowest authored element above it — the weapon panel — ends
# at 88%. y0=0.90 therefore separates the two with room to spare, and the box is
# wide enough for the longest string the readout can produce ("241 KPH").
SPEEDO_BOX = (0.00, 0.90, 0.32, 1.00)

# A legible three-to-eight character readout is thousands of pixels at this
# capture size. Requiring a real count stops a single changed texel — a stray
# blend, a one-pixel scissor shift — from being read as "the readout appeared".
MIN_READOUT_PIXELS = 500

# One second of a 30Hz standing start.
STANDING_START_FRAMES = 30
# The car is past 40mph one second after the lights, so this is a floor with a
# wide margin, not a fitted value. Its job is to reject a readout that is
# ordered but frozen.
MIN_STANDING_START_RISE = 20

SPEEDO_RE = re.compile(
    r"\[SPEEDO\] frame=(-?\d+) mode=(\d+) countdown=(-?\d+) "
    r"units=(-?[\d.]+) shown=(-?\d+) unit=(\S+)"
)


@dataclass(frozen=True)
class Sample:
    frame: int
    mode: int
    countdown: int
    units: float
    shown: int
    unit: str


@dataclass(frozen=True)
class Arm:
    label: str
    image: Image
    samples: tuple[Sample, ...]
    state_hash: tuple[str, ...]


def environment(save_dir: Path) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_RENDERER="gl",
        MDKR_SPEEDO_TRACE="1",
        MDKR_STATE_HASH="3",
        MDKR_DUMP_FROM=str(CAPTURE_FRAME),
        MDKR_DUMP_EVERY="999",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        MDKR_SAVE_DIR=str(save_dir),
        # Isolate the video config with the save (see check_door_blocks.py).
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    return env


def run_arm(binary: Path, rom: Path, work: Path, label: str,
            mode: str | None, timeout: int, verbose: bool) -> Arm:
    run_dir = work / label
    dump_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    dump_dir.mkdir(parents=True)
    save_dir.mkdir()
    command = [
        str(binary),
        "--headless-frames", str(FRAMES),
        "--window-size", "640x480",
        "--input-script", str(SCRIPT),
        "--dump-frames", str(dump_dir),
        "--rom", str(rom),
    ]
    if mode is not None:
        command += ["--video-set", f"Enhancements.Speedometer={mode}"]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command, cwd=run_dir, env=environment(save_dir), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"{label}: timed out\n{(exc.stdout or '')[-4000:]}") from exc

    output = proc.stdout or ""
    fatal = find_fatal(output, *ABORT_MARKERS, *ASSERT_MARKERS)
    dumps = sorted(dump_dir.glob("*.ppm"))
    if (proc.returncode != 0 or fatal is not None or len(dumps) != 1 or
            dumps[0].name != f"frame_{CAPTURE_FRAME:04d}.ppm"):
        raise RuntimeError(
            f"{label}: exit={proc.returncode}, fatal={fatal or 'none'}, "
            f"dumps={[path.name for path in dumps]}\n{output[-5000:]}")

    samples = tuple(
        Sample(int(m.group(1)), int(m.group(2)), int(m.group(3)),
               float(m.group(4)), int(m.group(5)), m.group(6))
        for m in SPEEDO_RE.finditer(output)
    )
    state_hash = tuple(line for line in output.splitlines()
                       if line.startswith("[SIMHASH]"))
    if not state_hash:
        raise RuntimeError(
            f"{label}: no [SIMHASH] rows; the state-stream instrument did not "
            f"arm, so the presentation claim below would prove nothing")
    return Arm(label=label, image=read_ppm(dumps[0]), samples=samples,
               state_hash=state_hash)


def box_for(image: Image) -> tuple[int, int, int, int]:
    return scaled_box(image, *SPEEDO_BOX)


def outside_box_equal(left: Image, right: Image,
                      box: tuple[int, int, int, int]) -> tuple[int, int] | None:
    """The first (x, y) outside ``box`` where the two images differ."""

    if (left.width, left.height) != (right.width, right.height):
        raise ValueError("A/B dimensions differ")
    x0, y0, x1, y1 = box
    for y in range(left.height):
        row = y * left.width * 3
        if not (y0 <= y < y1):
            span = slice(row, row + left.width * 3)
            if left.pixels[span] != right.pixels[span]:
                for x in range(left.width):
                    offset = row + x * 3
                    if left.pixels[offset:offset + 3] != right.pixels[
                            offset:offset + 3]:
                        return (x, y)
            continue
        for span_start, span_end in ((0, x0), (x1, left.width)):
            if span_start >= span_end:
                continue
            span = slice(row + span_start * 3, row + span_end * 3)
            if left.pixels[span] != right.pixels[span]:
                for x in range(span_start, span_end):
                    offset = row + x * 3
                    if left.pixels[offset:offset + 3] != right.pixels[
                            offset:offset + 3]:
                        return (x, y)
    return None


def standing_start(arm: Arm) -> list[Sample]:
    """The last sample at rest, then one second of samples under power.

    The window deliberately opens one frame BEFORE the countdown clears. The
    car takes its first step on the very frame the lights go out, so a window
    that starts there starts at 1mph rather than at rest, and "a standing start
    begins stationary" would be asserting something false about the fixture
    instead of something true about the readout.
    """

    for index, sample in enumerate(arm.samples):
        if sample.countdown == 0:
            start = max(0, index - 1)
            return list(arm.samples[start:start + STANDING_START_FRAMES + 1])
    return []


def check_confined(arm: Arm, baseline: Arm, box: tuple[int, int, int, int],
                   failures: list[str]) -> int:
    changed = changed_pixels(baseline.image, arm.image)
    escaped = [point for point in changed if not inside(*point, box)]
    if len(changed) < MIN_READOUT_PIXELS:
        failures.append(
            f"{arm.label}: the readout changed only {len(changed)} pixel(s), "
            f"below the {MIN_READOUT_PIXELS} a legible number occupies")
    if escaped:
        failures.append(
            f"{arm.label}: {len(escaped)} changed pixel(s) fell outside the "
            f"readout box {box}; first={escaped[0]}")
    return len(changed)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    if not binary.exists():
        print(f"check_enh_speedometer: FAIL — no binary at {binary}",
              file=sys.stderr)
        return 1
    if not rom.exists():
        print(f"check_enh_speedometer: FAIL — no ROM at {rom}",
              file=sys.stderr)
        return 1

    failures: list[str] = []
    summaries: list[str] = []
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr_enh_speedo_") as temp:
            work = Path(temp)
            baseline = run_arm(binary, rom, work, "baseline", None,
                               args.timeout, args.verbose)
            off = run_arm(binary, rom, work, "off", "0",
                          args.timeout, args.verbose)
            mph = run_arm(binary, rom, work, "mph", "1",
                          args.timeout, args.verbose)
            kmh = run_arm(binary, rom, work, "kmh", "2",
                          args.timeout, args.verbose)

            box = box_for(baseline.image)

            # 1. Off costs the frame nothing.
            if off.image.pixels != baseline.image.pixels:
                first = changed_pixels(baseline.image, off.image)[0]
                failures.append(
                    f"off: the frame is not byte-identical to the "
                    f"no-enhancement baseline; first difference at {first}")
            if off.samples:
                failures.append(
                    f"off: emitted {len(off.samples)} [SPEEDO] row(s) with "
                    f"the readout switched off")
            if baseline.samples:
                failures.append(
                    f"baseline: emitted {len(baseline.samples)} [SPEEDO] "
                    f"row(s) with no enhancement set at all")

            # 2 and 3. The readout appears, and only there.
            mph_changed = check_confined(mph, baseline, box, failures)
            kmh_changed = check_confined(kmh, baseline, box, failures)
            escape = outside_box_equal(baseline.image, mph.image, box)
            if escape is not None:
                failures.append(
                    f"mph: the frame outside the readout box {box} is not "
                    f"byte-identical to the baseline; first difference at "
                    f"{escape}. The readout disturbed the existing interface.")

            # 4. Kilometres per hour is not miles per hour.
            if kmh.image.pixels == mph.image.pixels:
                failures.append(
                    "kmh: the =2 frame is byte-identical to the =1 frame, so "
                    "the two units render the same readout")
            mph_by_frame = {sample.frame: sample for sample in mph.samples}
            shared = [sample for sample in kmh.samples
                      if sample.frame in mph_by_frame
                      and mph_by_frame[sample.frame].shown > 0]
            if not shared:
                failures.append(
                    "kmh: no frame carries a nonzero reading in both units, so "
                    "the two cannot be compared")
            else:
                moving = shared[len(shared) // 2]
                imperial = mph_by_frame[moving.frame]
                if moving.shown <= imperial.shown:
                    failures.append(
                        f"kmh: frame {moving.frame} reads {moving.shown} "
                        f"{moving.unit} against {imperial.shown} "
                        f"{imperial.unit}; kilometres per hour must be the "
                        f"larger number for the same speed")
                if moving.unit == imperial.unit:
                    failures.append(
                        f"kmh: both modes labelled the readout "
                        f"'{moving.unit}'")

            # 5. The standing start.
            window = standing_start(mph)
            if len(window) <= STANDING_START_FRAMES:
                failures.append(
                    f"mph: only {len(window)} [SPEEDO] row(s) around the "
                    f"countdown clearing, so the standing-start assertion has "
                    f"nothing to run on")
            elif [sample.frame for sample in window] != list(
                    range(window[0].frame, window[0].frame + len(window))):
                failures.append(
                    f"mph: the [SPEEDO] rows from frame {window[0].frame} are "
                    f"not consecutive, so they are not one second of a "
                    f"standing start")
            else:
                regressions = [
                    (before.frame, before.shown, after.shown)
                    for before, after in zip(window, window[1:])
                    if after.shown < before.shown
                ]
                if regressions:
                    frame, before, after = regressions[0]
                    failures.append(
                        f"mph: the readout went backwards during a standing "
                        f"start — {before} then {after} at frame {frame + 1} "
                        f"({len(regressions)} regression(s) in "
                        f"{STANDING_START_FRAMES} frames)")
                if window[0].shown != 0:
                    failures.append(
                        f"mph: the readout showed {window[0].shown} at the "
                        f"moment the countdown cleared; a standing start "
                        f"begins at rest")
                rise = window[-1].shown - window[0].shown
                if rise < MIN_STANDING_START_RISE:
                    failures.append(
                        f"mph: the readout rose by only {rise} over the first "
                        f"second of a standing start, below the "
                        f"{MIN_STANDING_START_RISE} floor — it is ordered but "
                        f"not tracking the car")
                summaries.append(
                    f"standingStart={window[0].shown}->{window[-1].shown}"
                    f" over {len(window)} frames")

            # 6. Drawing a speed cannot move authoritative state.
            for arm in (off, mph, kmh):
                if arm.state_hash != baseline.state_hash:
                    first = next(
                        (index for index, (a, b) in enumerate(
                            zip(baseline.state_hash, arm.state_hash))
                         if a != b),
                        min(len(baseline.state_hash), len(arm.state_hash)))
                    failures.append(
                        f"{arm.label}: the [SIMHASH] v3 stream diverges from "
                        f"the baseline at row {first}. A row declared "
                        f"presentation reached authoritative state.")

            summaries.insert(0, f"box={box}")
            summaries.append(f"mphPixels={mph_changed}")
            summaries.append(f"kmhPixels={kmh_changed}")
            summaries.append(f"rows={len(mph.samples)}")
            summaries.append(f"stateRows={len(baseline.state_hash)}")
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"check_enh_speedometer: FAIL — {exc}", file=sys.stderr)
        return 1

    if failures:
        print("check_enh_speedometer: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check_enh_speedometer: PASS — " + "; ".join(summaries)
          + "; every pixel outside the readout box is byte-identical, and the "
            "state stream is byte-identical in all four arms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
