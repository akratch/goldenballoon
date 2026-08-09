#!/usr/bin/env python3
"""Draw distance and model detail move the picture and nothing else.

Why this exists
---------------
`Enhancements.DrawDistance` and `Enhancements.LodBias` are declared
`MDKR_ENH_PRESENTATION` in `platform/enhancement_registry.c`.
`tests/check_enhancement_authority.py` owns one half of that claim — that
neither can move the authoritative `[SIMHASH]` stream — but it drives
`nav_to_time_trial_race.txt`, which never leaves the menus, so for these two
rows its verdict is about the config key and not about the effect. This gate
owns the other half, in a race, where the effect is actually on screen.

The specific risk this gate exists for
--------------------------------------
DKR's draw distance is a RENDER-SIDE CULL. In this port the same predicate,
`check_if_in_draw_range()` in `game/src/tracks.c`, is reached from two places
with completely different authority: `scene_authoritative_render_tick()`, which
writes `obj->opacity` through it and gates `obj_authoritative_texture_tick()` on
it, and `render_level_geometry_and_objects()`, which only draws. Widen the
threshold in the first and "how far you can see" becomes an input to the
simulation — the enhancement is GAMEPLAY, not PRESENTATION, and it is wrong.

The assertion that tells those two apart is not "the frame changed" and it is
not even "the hash matched". It is **the live object count per tick is
identical**. An update-side cull changes which objects tick, which changes which
objects are spawned and destroyed, which moves `objs=` in the `[SIMHASH]` row.
A render-side cull cannot touch it. That assertion is the point of this file;
everything else is supporting evidence.

The arms
--------
Draw distance, on the deterministic single-player Time Trial route:

  dd100   `Enhancements.DrawDistance=100` — the authored distance
  dd400   `=400` — the far end of the schema's range

Model detail, on the FOUR-player split-screen route. The racer LOD ladder is the
port's only distance-driven model selection, and the route has to be one where
the authored ladder actually chooses a reduced model, or a setting that holds
higher detail has nothing to hold. A Time Trial has one racer, always close to
its own camera and always already at model 0. Four-player split-screen is the
opposite end: its band table selects models 2 and 3, so the bias has somewhere
to go. This was measured, not assumed — see the `lodShifted` assertion below,
which fails rather than passes if the route stops exercising the ladder.

  lod0    `Enhancements.LodBias=0` — the authored ladder
  lod2    `=2` — hold the detailed model furthest out

What it asserts
---------------
1. `dd400`'s captured frame differs from `dd100`'s. The capture frame is chosen
   to be one where the setting actually admits objects, which the `[DRAWDIST]`
   row proves rather than assumes.

2. The two `[SIMHASH]` v3 streams are byte-identical.

3. **The live object count per tick is identical.** Compared explicitly, tick by
   tick, rather than left implicit inside the hash — a hash comparison that
   fails tells you something moved, and this gate needs to be able to say WHAT.

4. The `[DRAWDIST]` census shows the AUTHORED draw count per frame is unchanged
   between the two arms while the EXTENDED count is zero at 100% and nonzero at
   400%. This is the mechanical form of "the setting only adds": the frame
   difference in (1) is more objects being drawn, not a stray blend or a
   one-pixel scissor shift somewhere else.

5. `lod2`'s frame differs from `lod0`'s, with the same `[SIMHASH]` stream and
   the same live object count per tick — and the `[DRAWDIST]` census reports
   that the bias actually landed on a different model (`lodShifted`) on the
   captured frame, while `lod0` reports zero shifts on every frame of the
   route. `lodShifted` counts choices the bias CHANGED after the model range is
   clamped, not choices it was consulted about, so a bias that is read and then
   erased by the clamp reads as the zero it is.

Vacuity guards, checked before anything else: an arm with no `[SIMHASH]` rows or
no `[DRAWDIST]` rows fails, because both instruments are opt-in and a run where
one did not arm would let every comparison below pass by comparing nothing.

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

from check_native_ui_resolution import Image, changed_pixels, read_ppm
from harness_utils import (ABORT_MARKERS, ASSERT_MARKERS, DEFAULT_BUILD_DIR,
                           find_fatal, resolve_binary)

ROOT = Path(__file__).resolve().parent.parent

# Draw distance: the single-player Time Trial route the other in-race pixel
# gates use. Frame 2910 is mid-race and is a measured choice — the [DRAWDIST]
# census over the whole route says the widened distance admits objects there,
# and assertion 4 re-checks that at run time so a route change cannot silently
# move the capture onto an empty stretch of track.
DD_SCRIPT = ROOT / "tests" / "input_scripts" / "race_drive_time_trial.txt"
DD_CAPTURE = 2910
DD_FRAMES = 2960

# Model detail: the four-player split-screen route. Its band table is the one
# that selects reduced models, so it is the only cheap fixture where a setting
# that holds higher detail has anything to hold. Its race clock starts around
# frame 2662, so frame 4500 is well into the race.
LOD_SCRIPT = ROOT / "tests" / "input_scripts" / "race_4p_split.txt"
LOD_CAPTURE = 4500
LOD_FRAMES = 4600

SIMHASH_RE = re.compile(r"\[SIMHASH\] tick=(\d+) objs=(\d+) h=([0-9a-f]+)")
DRAWDIST_RE = re.compile(
    r"\[DRAWDIST\] frame=(\d+) scale=([\d.]+) lodBias=(\d+) "
    r"authored=(\d+) extended=(\d+) drawn=(\d+) lodShifted=(\d+)"
)


@dataclass(frozen=True)
class Census:
    frame: int
    scale: float
    lod_bias: int
    authored: int
    extended: int
    drawn: int
    lod_shifted: int


@dataclass(frozen=True)
class Arm:
    label: str
    image: Image
    state_hash: tuple[str, ...]
    live_objects: tuple[tuple[int, int], ...]
    census: tuple[Census, ...]


def environment(save_dir: Path, capture: int) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_RENDERER="gl",
        MDKR_STATE_HASH="3",
        MDKR_DRAWDIST_TRACE="1",
        MDKR_DUMP_FROM=str(capture),
        MDKR_DUMP_EVERY="999",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        MDKR_SAVE_DIR=str(save_dir),
    )
    return env


def run_arm(binary: Path, rom: Path, work: Path, label: str, script: Path,
            frames: int, capture: int, override: str, timeout: int,
            verbose: bool) -> Arm:
    run_dir = work / label
    dump_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    dump_dir.mkdir(parents=True)
    save_dir.mkdir()
    command = [
        str(binary),
        "--headless-frames", str(frames),
        "--window-size", "640x480",
        "--input-script", str(script),
        "--dump-frames", str(dump_dir),
        "--video-set", override,
        "--rom", str(rom),
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command, cwd=run_dir, env=environment(save_dir, capture), text=True,
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
            dumps[0].name != f"frame_{capture:04d}.ppm"):
        raise RuntimeError(
            f"{label}: exit={proc.returncode}, fatal={fatal or 'none'}, "
            f"dumps={[path.name for path in dumps]}\n{output[-5000:]}")

    rows = [line for line in output.splitlines() if line.startswith("[SIMHASH]")]
    matches = [SIMHASH_RE.match(line) for line in rows]
    if not rows or not all(matches):
        raise RuntimeError(
            f"{label}: {len(rows)} [SIMHASH] row(s), "
            f"{sum(1 for m in matches if m)} parsed. The state-stream "
            f"instrument did not arm in the shape this gate reads, so every "
            f"comparison below would compare nothing.")
    census = tuple(
        Census(int(m.group(1)), float(m.group(2)), int(m.group(3)),
               int(m.group(4)), int(m.group(5)), int(m.group(6)),
               int(m.group(7)))
        for m in DRAWDIST_RE.finditer(output)
    )
    if not census:
        raise RuntimeError(
            f"{label}: no [DRAWDIST] rows. MDKR_DRAWDIST_TRACE did not arm, so "
            f"the draw census that separates 'more objects were drawn' from "
            f"'some pixel moved' is missing.")
    return Arm(
        label=label,
        image=read_ppm(dumps[0]),
        state_hash=tuple(rows),
        live_objects=tuple((int(m.group(1)), int(m.group(2)))
                           for m in matches if m is not None),
        census=census,
    )


def census_at(arm: Arm, frame: int) -> Census | None:
    for row in arm.census:
        if row.frame == frame:
            return row
    return None


def compare_state(base: Arm, other: Arm, failures: list[str]) -> None:
    """The two assertions that make the presentation claim, in both forms."""

    # The live object count, tick by tick. Deliberately checked BEFORE the hash:
    # it is the one that distinguishes a render cull from an update cull, and
    # reporting it by name is worth more than "the hash moved".
    if len(base.live_objects) != len(other.live_objects):
        failures.append(
            f"{other.label}: {len(other.live_objects)} authoritative tick(s) "
            f"against {base.label}'s {len(base.live_objects)}")
    else:
        drift = [(tick, a, b)
                 for (tick, a), (_, b) in zip(base.live_objects,
                                              other.live_objects) if a != b]
        if drift:
            tick, expected, got = drift[0]
            failures.append(
                f"{other.label}: the LIVE OBJECT COUNT diverges from "
                f"{base.label} at tick {tick} — {expected} objects against "
                f"{got} ({len(drift)} tick(s) differ). This is an UPDATE-side "
                f"cull, not a render-side one: changing how far the game draws "
                f"changed which objects exist. The hook is in the wrong place.")

    if base.state_hash != other.state_hash:
        first = next(
            (index for index, (a, b) in enumerate(
                zip(base.state_hash, other.state_hash)) if a != b),
            min(len(base.state_hash), len(other.state_hash)))
        failures.append(
            f"{other.label}: the [SIMHASH] v3 stream diverges from "
            f"{base.label} at row {first}. A row declared presentation reached "
            f"authoritative state.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    if not binary.exists():
        print(f"check_enh_draw_distance: FAIL — no binary at {binary}",
              file=sys.stderr)
        return 1
    if not rom.exists():
        print(f"check_enh_draw_distance: FAIL — no ROM at {rom}",
              file=sys.stderr)
        return 1

    failures: list[str] = []
    summaries: list[str] = []
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr_enh_drawdist_") as temp:
            work = Path(temp)
            near = run_arm(binary, rom, work, "dd100", DD_SCRIPT, DD_FRAMES,
                           DD_CAPTURE, "Enhancements.DrawDistance=100",
                           args.timeout, args.verbose)
            far = run_arm(binary, rom, work, "dd400", DD_SCRIPT, DD_FRAMES,
                          DD_CAPTURE, "Enhancements.DrawDistance=400",
                          args.timeout, args.verbose)

            near_at = census_at(near, DD_CAPTURE)
            far_at = census_at(far, DD_CAPTURE)
            if near_at is None or far_at is None:
                failures.append(
                    f"dd: frame {DD_CAPTURE} has no [DRAWDIST] row in "
                    f"{'dd100' if near_at is None else 'dd400'}, so the "
                    f"captured frame is not one the census covers")
            else:
                # 4, checked first because it is what makes 1 mean anything.
                if near_at.scale != 1.0 or near_at.extended != 0:
                    failures.append(
                        f"dd100: the authored arm reports scale="
                        f"{near_at.scale} and {near_at.extended} extended "
                        f"draw(s); at 100% the setting must be inert")
                if far_at.scale != 4.0:
                    failures.append(
                        f"dd400: the census reports scale={far_at.scale}, not "
                        f"4.0 — the setting did not reach the cull")
                if far_at.extended <= 0:
                    failures.append(
                        f"dd400: the widened distance admitted no extra "
                        f"objects at frame {DD_CAPTURE}, so this capture "
                        f"cannot show the setting working. Pick a frame the "
                        f"[DRAWDIST] census says has extended draws.")
                if far_at.authored != near_at.authored:
                    failures.append(
                        f"dd400: the AUTHORED draw count at frame "
                        f"{DD_CAPTURE} is {far_at.authored} against dd100's "
                        f"{near_at.authored}. The setting is supposed to add "
                        f"draws, not change which objects the fixed tick "
                        f"routed.")
                summaries.append(
                    f"drawn@{DD_CAPTURE}={near_at.drawn}->{far_at.drawn} "
                    f"(+{far_at.extended} extended)")

            # Over the whole route, not just the captured frame.
            near_authored = [row.authored for row in near.census]
            far_authored = [row.authored for row in far.census]
            if near_authored != far_authored:
                differing = sum(1 for a, b in zip(near_authored, far_authored)
                                if a != b)
                failures.append(
                    f"dd400: the per-frame AUTHORED draw count differs from "
                    f"dd100 on {differing} frame(s) of "
                    f"{len(near_authored)}. The widened distance is "
                    f"re-deciding the fixed tick's routes instead of "
                    f"extending them.")
            total_extended = sum(row.extended for row in far.census)
            if total_extended <= 0:
                failures.append(
                    "dd400: the widened distance admitted no extra object on "
                    "any frame of the route; the setting does nothing")
            summaries.append(f"extendedDrawsOverRoute={total_extended}")

            # 1.
            if far.image.pixels == near.image.pixels:
                failures.append(
                    f"dd400: the captured frame is byte-identical to dd100's. "
                    f"Raising the draw distance to 400% changed nothing on "
                    f"screen.")
            else:
                summaries.append(
                    f"ddPixels={len(changed_pixels(near.image, far.image))}")

            # 2 and 3.
            compare_state(near, far, failures)

            detail_off = run_arm(binary, rom, work, "lod0", LOD_SCRIPT,
                                 LOD_FRAMES, LOD_CAPTURE,
                                 "Enhancements.LodBias=0", args.timeout,
                                 args.verbose)
            detail_max = run_arm(binary, rom, work, "lod2", LOD_SCRIPT,
                                 LOD_FRAMES, LOD_CAPTURE,
                                 "Enhancements.LodBias=2", args.timeout,
                                 args.verbose)

            off_at = census_at(detail_off, LOD_CAPTURE)
            max_at = census_at(detail_max, LOD_CAPTURE)
            if off_at is None or max_at is None:
                failures.append(
                    f"lod: frame {LOD_CAPTURE} has no [DRAWDIST] row, so the "
                    f"captured frame is not one the census covers")
            else:
                if off_at.lod_bias != 0 or max_at.lod_bias != 2:
                    failures.append(
                        f"lod: the census reports bias {off_at.lod_bias} and "
                        f"{max_at.lod_bias}; the setting did not reach the "
                        f"draw")
                if max_at.lod_shifted <= 0:
                    failures.append(
                        f"lod2: the bias changed no model choice at frame "
                        f"{LOD_CAPTURE}. Either this route no longer reaches "
                        f"the reduced end of the detail ladder, or the bias is "
                        f"being erased by the model-range clamp — in both "
                        f"cases the pixel assertion below would be testing "
                        f"nothing.")
                summaries.append(f"lodShifted@{LOD_CAPTURE}="
                                 f"{max_at.lod_shifted}")
            stray = [row.frame for row in detail_off.census if row.lod_shifted]
            if stray:
                failures.append(
                    f"lod0: the authored arm reports a shifted model choice on "
                    f"{len(stray)} frame(s), first at {stray[0]}; at bias 0 the "
                    f"setting must be inert")

            # 5.
            if detail_max.image.pixels == detail_off.image.pixels:
                failures.append(
                    "lod2: the captured frame is byte-identical to lod0's. "
                    "Holding the detailed model further out changed nothing "
                    "on screen.")
            else:
                summaries.append(
                    f"lodPixels="
                    f"{len(changed_pixels(detail_off.image, detail_max.image))}")
            compare_state(detail_off, detail_max, failures)

            summaries.append(f"ddTicks={len(near.live_objects)}")
            summaries.append(f"lodTicks={len(detail_off.live_objects)}")
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"check_enh_draw_distance: FAIL — {exc}", file=sys.stderr)
        return 1

    if failures:
        print("check_enh_draw_distance: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check_enh_draw_distance: PASS — " + "; ".join(summaries)
          + "; the live object count per tick and the whole [SIMHASH] v3 "
            "stream are identical in both pairs, so both settings are render "
            "culls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
