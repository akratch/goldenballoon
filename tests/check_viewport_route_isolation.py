#!/usr/bin/env python3
"""2P/4P object-route isolation with a last-viewport fade control.

The fixture deliberately leaves player one's racer opaque in every early
viewport and faded only in the final viewport.  The production arm must consume
the retained object x viewport route; the broken-direction arm substitutes the
final viewport's route for every viewport, reproducing MP-001 exactly.

Both arms run identical authority and therefore must have byte-identical v3
state hashes.  Draw-route traces prove pass, blend opacity, shadow, and water
provenance.  A frame comparison proves the final viewport stays substantially
stable while every contaminated earlier viewport changes visibly.
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

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
FRAMES = 2740
TRACE_TICK = 2700
DUMP_FROM = 2680
DUMP_EVERY = 20
# Draw-route telemetry is the exact ownership proof. Pixel differences are its
# visual witness and can shrink while the racer is partly occluded, so require
# the strong floor in two of the three captures and only nonzero evidence in
# every capture.
STRONG_VISIBLE_CHANNELS = 5000
SCRIPTS = {
    2: ROOT / "tests" / "input_scripts" / "race_2p_split.txt",
    4: ROOT / "tests" / "input_scripts" / "race_4p_split.txt",
}

DRAW_RE = re.compile(
    r"^\[VIEWPORT-DRAW\] tick=(\d+) requested=(\d+) source=(\d+) "
    r"pass=(\w+) admitted=(\d+) opacity=(-?\d+) visible=(-?\d+) "
    r"shadow=(\d+) water=(\d+)$"
)


@dataclass(frozen=True)
class Draw:
    requested: int
    source: int
    draw_pass: str
    opacity: int
    visible: int
    shadow: int
    water: int


@dataclass(frozen=True)
class Result:
    state: tuple[str, ...]
    draws: tuple[Draw, ...]
    frames: tuple[Path, ...]


def changed_channels(left: Path, right: Path,
                     bounds: tuple[float, float, float, float]) -> int:
    lw, lh, lp = read_ppm(left)
    rw, rh, rp = read_ppm(right)
    if (lw, lh) != (rw, rh):
        raise RuntimeError(
            f"frame dimensions differ: {lw}x{lh} vs {rw}x{rh}")
    x0, x1 = int(lw * bounds[0]), int(lw * bounds[1])
    y0, y1 = int(lh * bounds[2]), int(lh * bounds[3])
    changed = 0
    for y in range(y0, y1):
        start = (y * lw + x0) * 3
        end = (y * lw + x1) * 3
        changed += sum(a != b for a, b in zip(lp[start:end], rp[start:end]))
    return changed


def run_arm(binary: Path, rom: Path, root: Path, players: int,
            broken: bool, renderer: str, timeout: int,
            verbose: bool) -> Result:
    label = f"{players}p-{'shared-control' if broken else 'isolated'}"
    run_dir = root / label
    frame_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frame_dir.mkdir(parents=True)
    save_dir.mkdir()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_DUMP_EVERY=str(DUMP_EVERY),
        MDKR_DUMP_FROM=str(DUMP_FROM),
        MDKR_RENDERER=renderer,
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_SIMULATION_CADENCE="original",
        MDKR_STATE_HASH="3",
        MDKR_SYNTH_FIELDS="2",
        MDKR_TEST_VIEWPORT_FADE_LAST="1",
        MDKR_TEST_VIEWPORT_ROUTE_TRACE_TICK=str(TRACE_TICK),
        MDKR_TRACE="1",
    )
    if broken:
        env["MDKR_TEST_SHARED_VIEWPORT_ROUTES"] = "1"
    command = [
        str(binary), "--headless-frames", str(FRAMES),
        "--window-size", "640x480",
        "--input-script", str(SCRIPTS[players]),
        "--dump-frames", str(frame_dir), "--rom", str(rom),
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "Assertion failed",
                   "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    load_pattern = re.compile(
        rf"level_load: levelId=5 numPlayers={players - 1}\b")
    hud_pattern = re.compile(
        rf"hud_init: hudPlayers={players - 1} numViewports={players}\b")
    if load_pattern.search(output) is None or hud_pattern.search(output) is None:
        raise RuntimeError(
            f"{label}: fixture did not enter the intended {players}P "
            "Ancient Lake viewport layout")
    state = tuple(line for line in output.splitlines()
                  if line.startswith("[SIMHASH]"))
    if len(state) != FRAMES:
        raise RuntimeError(
            f"{label}: expected {FRAMES} state rows, got {len(state)}")
    draws: list[Draw] = []
    for line in output.splitlines():
        match = DRAW_RE.match(line)
        if (match is None or int(match.group(1)) != TRACE_TICK or
                int(match.group(5)) == 0):
            continue
        draws.append(Draw(
            requested=int(match.group(2)), source=int(match.group(3)),
            draw_pass=match.group(4), opacity=int(match.group(6)),
            visible=int(match.group(7)), shadow=int(match.group(8)),
            water=int(match.group(9)),
        ))
    frames = tuple(sorted(frame_dir.glob("frame_*.ppm")))
    expected_frames = tuple(range(DUMP_FROM, FRAMES, DUMP_EVERY))
    actual_frames = tuple(int(path.stem.split("_")[-1]) for path in frames)
    if actual_frames != expected_frames:
        raise RuntimeError(
            f"{label}: frame captures {actual_frames}, expected "
            f"{expected_frames}")
    return Result(state, tuple(draws), frames)


def expected_draws(players: int, broken: bool) -> tuple[Draw, ...]:
    rows: list[Draw] = []
    last = players - 1
    for viewport in range(players):
        if broken:
            rows.append(Draw(viewport, last, "blend", 64, 64, 1, 1))
        elif viewport == last:
            rows.append(Draw(viewport, viewport, "blend", 64, 64, 1, 1))
        else:
            rows.extend((
                Draw(viewport, viewport, "opaque", 255, 255, 1, 1),
                # Opaque racers retain the authored transparent FX pass for
                # shield/magnet drawing; visible zero means no duplicate body,
                # shadow, or water draw occurs there.
                Draw(viewport, viewport, "blend", 255, 0, 1, 1),
            ))
    return tuple(rows)


def viewport_bounds(players: int) -> tuple[tuple[str, tuple[float, ...]], ...]:
    if players == 2:
        return (
            ("top", (0.03, 0.97, 0.03, 0.47)),
            ("bottom", (0.03, 0.97, 0.53, 0.97)),
        )
    return (
        ("top-left", (0.03, 0.47, 0.03, 0.47)),
        ("top-right", (0.53, 0.97, 0.03, 0.47)),
        ("bottom-left", (0.03, 0.47, 0.53, 0.97)),
        ("bottom-right", (0.53, 0.97, 0.53, 0.97)),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--renderer", choices=("gl", "webgpu"),
                        default="webgpu")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, *SCRIPTS.values()):
        if not path.exists():
            print(f"check_viewport_route_isolation: FAIL\n  - missing {path}")
            return 1

    failures: list[str] = []
    notes: list[str] = []
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-viewport-route-") as tmp:
            root = Path(tmp)
            for players in (2, 4):
                isolated = run_arm(binary, rom, root, players, False,
                                   args.renderer, args.timeout, args.verbose)
                shared = run_arm(binary, rom, root, players, True,
                                 args.renderer, args.timeout, args.verbose)
                if isolated.state != shared.state:
                    tick = next(
                        (i for i, pair in
                         enumerate(zip(isolated.state, shared.state))
                         if pair[0] != pair[1]), None)
                    failures.append(
                        f"{players}P: render-only control changed authority "
                        f"at tick {tick}")
                if isolated.draws != expected_draws(players, False):
                    failures.append(
                        f"{players}P isolated routes {isolated.draws} do not "
                        f"match {expected_draws(players, False)}")
                if shared.draws != expected_draws(players, True):
                    failures.append(
                        f"{players}P shared control routes {shared.draws} do "
                        f"not match {expected_draws(players, True)}")

                bounds = viewport_bounds(players)
                per_frame: list[dict[str, int]] = []
                for left, right in zip(isolated.frames, shared.frames):
                    per_frame.append({
                        name: changed_channels(left, right, region)
                        for name, region in bounds
                    })
                for index, counts in enumerate(per_frame):
                    earlier = list(counts.values())[:-1]
                    final = list(counts.values())[-1]
                    if min(earlier) == 0:
                        failures.append(
                            f"{players}P frame {isolated.frames[index].name}: "
                            f"shared-route control did not change every "
                            f"earlier viewport: {counts}")
                    if final * 4 >= min(earlier):
                        failures.append(
                            f"{players}P frame {isolated.frames[index].name}: "
                            f"final viewport was not substantially stable "
                            f"relative to contaminated earlier views: {counts}")
                for name, _ in bounds[:-1]:
                    strong_frames = sum(
                        counts[name] >= STRONG_VISIBLE_CHANNELS
                        for counts in per_frame
                    )
                    if strong_frames < len(per_frame) - 1:
                        failures.append(
                            f"{players}P {name}: shared-route control exceeded "
                            f"{STRONG_VISIBLE_CHANNELS} changed channels in only "
                            f"{strong_frames}/{len(per_frame)} frames: "
                            f"{[counts[name] for counts in per_frame]}")
                notes.append(
                    f"{players}P changed-channel counts "
                    f"{[tuple(row.values()) for row in per_frame]}")
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        failures.append(str(error))

    if failures:
        print("check_viewport_route_isolation: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        if args.verbose:
            for note in notes:
                print(f"  {note}")
        return 1
    print(
        f"check_viewport_route_isolation: PASS ({args.renderer}) — 2P/4P "
        "retained pass/order/"
        "opacity with shadow+water provenance; state identical to the shared-"
        "route control; earlier viewport pixels reject final-viewport "
        "contamination while the final view remains stable")
    for note in notes:
        print(f"  {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
