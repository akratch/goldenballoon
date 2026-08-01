#!/usr/bin/env python3
"""Three-/four-player race, viewport, and post-race regression gate.

The historical multiplayer check stops at two players.  That leaves distinct
production paths unmeasured: the four-quadrant camera layout, the three-player
minimap quadrant, P3/P4 controller-to-racer binding, and the multiplayer results
screen reached after a race.  This check drives both legal local-player counts
through the real frontend and asserts each boundary independently.

Every engine invocation is muted and headless.  The visual assertion includes
its own positive control: each quadrant is replaced with a flat field in turn
and the same scorer must reject it.
"""

from __future__ import annotations

import argparse
import itertools
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass

from harness_utils import resolve_binary


FRAMES = 9600
VISUAL_FIRST = 3000
VISUAL_LAST = 7000
VISUAL_EVERY = 800
MIN_ACTIVE_ROWS = 3500
MAX_STEP = 60.0
Y_MIN, Y_MAX = -250.0, 500.0
MIN_FINAL_CHECKPOINT = 40
MIN_FINAL_LAP = 2
# In an ordinary multiplayer race, production ends the race once every racer
# except one has finished, marks the remaining racer last, and advances to the
# results screen.  That one classified DNF still has to have raced substantially.
MIN_DNF_CHECKPOINT = 30
MIN_DNF_LAP = 1
STALL_WINDOW = 240
MIN_STALL_MEAN = 0.5
MIN_PAIR_MEDIAN_SEPARATION = 30.0

MENU_RESULTS = 17
MENU_TRACK_SELECT = 15
ANCIENT_LAKE = 5

MENU_RE = re.compile(r"menu_init: menuId=(\d+) @frame~(\d+)")
LOAD_RE = re.compile(
    r"level_load: levelId=(\d+) numPlayers=(-?\d+).*@frame~(\d+)"
)
HUD_RE = re.compile(
    r"hud_init: hudPlayers=(\d+) numViewports=(\d+) @frame~(\d+)"
)
PACE1_RE = re.compile(
    r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+) "
    r"clock=(\d+) cp=(-?\d+) lap=(-?\d+)"
)
PACEN_RE = re.compile(
    r"\[PACE([2-4])\] frame=(\d+) \| racer[2-4] "
    r"x=(\S+) y=(\S+) z=(\S+) clock=(\d+) cp=(-?\d+) lap=(-?\d+)"
)
UI_RE = re.compile(
    r"\[UI-3\] frame=\d+ active=(\d+) draws=(\d+) lateWorld=(\d+) "
    r".*beginFailures=(\d+)"
)
ORACLE_RE = re.compile(
    r"\[ORACLE\] frame=(\d+) map=(\d+) slot=(\d+) .* "
    r"cp=(-?\d+) next=-?\d+ lap=(-?\d+) countlap=-?\d+ "
    r"fin=(-?\d+) fpos=(-?\d+) ridx=-?\d+ pidx=(-?\d+)"
)


@dataclass(frozen=True)
class Case:
    players: int
    script: str

    @property
    def encoded_players(self) -> int:
        """The game stores local-player count as N-1 in level/HUD state."""
        return self.players - 1


CASES = (
    Case(3, "tests/input_scripts/race_3p_split.txt"),
    Case(4, "tests/input_scripts/race_4p_split.txt"),
)


@dataclass(frozen=True)
class Pace:
    x: float
    y: float
    z: float
    clock: int
    checkpoint: int
    lap: int


@dataclass(frozen=True)
class Terminal:
    frame: int
    checkpoint: int
    lap: int
    finished: int
    position: int


@dataclass(frozen=True)
class Metrics:
    colours: int
    sigma: float
    nonblack: float


# Crops stay inside the four viewport rectangles and exclude the thick centre
# dividers. They scale with both ordinary and supersampled dumps.
QUADRANTS = {
    "top-left": (0.03, 0.47, 0.03, 0.47),
    "top-right": (0.53, 0.97, 0.03, 0.47),
    "bottom-left": (0.03, 0.47, 0.53, 0.97),
    "bottom-right": (0.53, 0.97, 0.53, 0.97),
}


def finite(value: float) -> bool:
    return math.isfinite(value) and abs(value) < 1.0e30


def read_ppm(path: str) -> tuple[int, int, bytes]:
    with open(path, "rb") as stream:
        data = stream.read()
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
    if fields[0] != b"P6" or int(fields[3]) != 255:
        raise ValueError(f"{path}: expected binary 8-bit PPM")
    if index >= len(data) or not data[index:index + 1].isspace():
        raise ValueError(f"{path}: missing PPM header delimiter")
    # Consume the header delimiter, not arbitrary whitespace: a legal first
    # raster byte may itself be 0x09, 0x0A, 0x0D, 0x20, etc.
    index += 2 if data[index:index + 2] == b"\r\n" else 1
    width, height = int(fields[1]), int(fields[2])
    pixels = data[index:index + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path}: truncated raster")
    return width, height, pixels


def region_metrics(
    width: int,
    height: int,
    pixels: bytes,
    bounds: tuple[float, float, float, float],
) -> Metrics:
    x0, x1 = int(width * bounds[0]), int(width * bounds[1])
    y0, y1 = int(height * bounds[2]), int(height * bounds[3])
    colours: set[tuple[int, int, int]] = set()
    count = total = total_sq = nonblack = 0
    for y in range(y0, y1, 4):
        row = y * width * 3
        for x in range(x0, x1, 4):
            offset = row + x * 3
            red, green, blue = pixels[offset:offset + 3]
            colours.add((red >> 3, green >> 3, blue >> 3))
            luma = (red * 299 + green * 587 + blue * 114) // 1000
            count += 1
            total += luma
            total_sq += luma * luma
            if max(red, green, blue) >= 24:
                nonblack += 1
    mean = total / count
    variance = max(0.0, total_sq / count - mean * mean)
    return Metrics(len(colours), variance**0.5, nonblack / count)


def region_is_live(metrics: Metrics, minimap: bool) -> bool:
    if minimap:
        # The three-player fourth quadrant is intentionally mostly black. Its
        # track, racer arrows, and start line still provide measurable structure.
        return (
            metrics.colours >= 12
            and metrics.sigma >= 8.0
            and metrics.nonblack >= 0.015
        )
    return metrics.colours >= 150 and metrics.sigma >= 12.0


def blank_quadrant(
    width: int,
    height: int,
    pixels: bytes,
    bounds: tuple[float, float, float, float],
    minimap: bool,
) -> bytes:
    result = bytearray(pixels)
    x0, x1 = int(width * bounds[0]), int(width * bounds[1])
    y0, y1 = int(height * bounds[2]), int(height * bounds[3])
    fill = bytes((0, 0, 0) if minimap else (110, 82, 48))
    row_fill = fill * (x1 - x0)
    for y in range(y0, y1):
        start = (y * width + x0) * 3
        result[start:start + len(row_fill)] = row_fill
    return bytes(result)


def parse_pace(output: str) -> dict[int, dict[int, Pace]]:
    rows: dict[int, dict[int, Pace]] = {player: {} for player in range(1, 5)}
    for line in output.splitlines():
        match = PACE1_RE.search(line)
        if match:
            rows[1][int(match.group(1))] = Pace(
                float(match.group(2)),
                float(match.group(3)),
                float(match.group(4)),
                int(match.group(5)),
                int(match.group(6)),
                int(match.group(7)),
            )
            continue
        match = PACEN_RE.search(line)
        if match:
            player = int(match.group(1))
            rows[player][int(match.group(2))] = Pace(
                float(match.group(3)),
                float(match.group(4)),
                float(match.group(5)),
                int(match.group(6)),
                int(match.group(7)),
                int(match.group(8)),
            )
    return rows


def check_motion(
    player: int,
    rows: dict[int, Pace],
    failures: list[str],
) -> tuple[dict[str, float | int] | None, set[int]]:
    if not rows:
        failures.append(
            f"P{player}: no pace rows; that controller never acquired a human racer"
        )
        return None, set()

    max_clock = max(row.clock for row in rows.values())
    # A finished racer remains published with a frozen clock and position. Drop
    # the terminal clock value so post-finish waiting cannot look like a stall.
    active = [
        frame
        for frame in sorted(rows)
        if 0 < rows[frame].clock < max_clock
    ]
    if len(active) < MIN_ACTIVE_ROWS:
        failures.append(
            f"P{player}: only {len(active)} active race rows "
            f"(want >= {MIN_ACTIVE_ROWS})"
        )
    if not active:
        return None, set()

    for frame in active:
        row = rows[frame]
        if not all(finite(value) for value in (row.x, row.y, row.z)):
            failures.append(f"P{player}: non-finite position at frame {frame}: {row}")
            break
        if not Y_MIN <= row.y <= Y_MAX:
            failures.append(
                f"P{player}: y={row.y:.1f} outside [{Y_MIN}, {Y_MAX}] "
                f"at frame {frame}"
            )
            break

    steps: list[tuple[int, float]] = []
    max_step = 0.0
    max_step_frame = active[0]
    for previous, current in zip(active, active[1:]):
        if current != previous + 1:
            continue
        a, b = rows[previous], rows[current]
        distance = math.dist((a.x, a.y, a.z), (b.x, b.y, b.z))
        steps.append((current, distance))
        if distance > max_step:
            max_step, max_step_frame = distance, current
    if max_step > MAX_STEP:
        failures.append(
            f"P{player}: {max_step:.1f}-unit one-frame teleport at "
            f"{max_step_frame} (limit {MAX_STEP})"
        )

    worst_mean = float("inf")
    worst_frame = active[0]
    distances = [distance for _, distance in steps]
    for offset in range(0, len(distances) - STALL_WINDOW + 1, STALL_WINDOW // 2):
        mean = statistics.fmean(distances[offset:offset + STALL_WINDOW])
        if mean < worst_mean:
            worst_mean = mean
            worst_frame = steps[offset][0]
    if worst_mean < MIN_STALL_MEAN:
        failures.append(
            f"P{player}: mean speed {worst_mean:.2f} over {STALL_WINDOW} "
            f"frames at {worst_frame} (want >= {MIN_STALL_MEAN})"
        )

    last = max(rows.values(), key=lambda row: row.clock)

    return (
        {
            "rows": len(active),
            "checkpoint": last.checkpoint,
            "lap": last.lap,
            "max_step": max_step,
            "worst_mean": worst_mean,
        },
        set(active),
    )


def parse_terminal_states(output: str, players: int) -> dict[int, Terminal]:
    """Latest end-of-update Ancient Lake state for each human controller."""
    terminal: dict[int, Terminal] = {}
    for line in output.splitlines():
        match = ORACLE_RE.search(line)
        if match is None or int(match.group(2)) != ANCIENT_LAKE:
            continue
        player_index = int(match.group(8))
        if not 0 <= player_index < players:
            continue
        terminal[player_index + 1] = Terminal(
            frame=int(match.group(1)),
            checkpoint=int(match.group(4)),
            lap=int(match.group(5)),
            finished=int(match.group(6)),
            position=int(match.group(7)),
        )
    return terminal


def print_result(failures: list[str]) -> int:
    if failures:
        print("check_race_multiplayer: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_race_multiplayer: PASS")
    return 0


def run_case(
    case: Case,
    binary: str,
    rom: str,
    renderer: str | None,
    window_size: str,
    keep_root: str | None,
    verbose: bool,
) -> list[str]:
    failures: list[str] = []
    if keep_root:
        frame_dir = os.path.join(keep_root, f"{case.players}p")
        os.makedirs(frame_dir, exist_ok=True)
    else:
        frame_dir = tempfile.mkdtemp(prefix=f"mdkr_{case.players}p_")

    env = dict(
        os.environ,
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_ORACLE_STATE="1",
        MDKR_UI_OVERLAY_TRACE="1",
        MDKR_SAVE_DIR=os.path.join(frame_dir, "save"),
        MDKR_DUMP_FROM=str(VISUAL_FIRST),
        MDKR_DUMP_EVERY=str(VISUAL_EVERY),
    )
    if renderer:
        env["MDKR_RENDERER"] = renderer
    command = [
        binary,
        "--headless-frames",
        str(FRAMES),
        "--window-size",
        window_size,
        "--input-script",
        case.script,
        "--dump-frames",
        frame_dir,
        "--rom",
        rom,
    ]
    if verbose:
        print(f"$ {' '.join(command)}")
    process = subprocess.run(command, capture_output=True, text=True, env=env)
    output = process.stdout + process.stderr

    if process.returncode != 0:
        failures.append(f"{case.players}P: exit code {process.returncode}")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in output:
            line = next(line for line in output.splitlines() if marker in line)
            failures.append(f"{case.players}P: {line.strip()}")

    loads = [
        (int(match.group(1)), int(match.group(2)), int(match.group(3)))
        for line in output.splitlines()
        if (match := LOAD_RE.search(line))
    ]
    race_loads = [
        frame
        for level, players, frame in loads
        if level == ANCIENT_LAKE and players == case.encoded_players
    ]
    if not race_loads:
        failures.append(
            f"{case.players}P: no Ancient Lake level load with "
            f"numPlayers={case.encoded_players}; saw {loads}"
        )
        race_frame = -1
    else:
        race_frame = race_loads[0]

    layouts = [
        (int(match.group(1)), int(match.group(2)), int(match.group(3)))
        for line in output.splitlines()
        if (match := HUD_RE.search(line))
    ]
    wanted_layout = (case.encoded_players, case.players)
    if not any((hud, viewports) == wanted_layout for hud, viewports, _ in layouts):
        failures.append(
            f"{case.players}P: no hudPlayers={wanted_layout[0]} "
            f"numViewports={wanted_layout[1]}; saw {layouts}"
        )
    ui_rows = [
        tuple(int(value) for value in match.groups())
        for line in output.splitlines()
        if (match := UI_RE.search(line))
    ]
    if not any(active and draws for active, draws, _, _ in ui_rows):
        failures.append(
            f"{case.players}P: output-resolution UI pass was never active"
        )
    if any(late for _, _, late, _ in ui_rows):
        failures.append(
            f"{case.players}P: world primitives followed the UI pass"
        )
    if ui_rows and ui_rows[-1][3] != 0:
        failures.append(
            f"{case.players}P: output UI begin failures reached {ui_rows[-1][3]}"
        )

    pace = parse_pace(output)
    active_sets: dict[int, set[int]] = {}
    summaries: dict[int, dict[str, float | int]] = {}
    for player in range(1, case.players + 1):
        summary, active = check_motion(player, pace[player], failures)
        active_sets[player] = active
        if summary:
            summaries[player] = summary

    # The historical [PACE] probes live inside update_player_racer() and stop
    # carrying terminal state as soon as raceFinished flips.  [ORACLE] is sampled
    # after the complete update for every racer, so it can prove the production
    # finish-position contract without changing any game state.
    terminal = parse_terminal_states(output, case.players)
    positions: list[int] = []
    classified_dnfs: list[int] = []
    for player in range(1, case.players + 1):
        state = terminal.get(player)
        if state is None:
            failures.append(f"P{player}: no terminal [ORACLE] state")
            continue
        if state.finished != 1:
            failures.append(
                f"P{player}: raceFinished stayed {state.finished} at frame {state.frame}"
            )
        positions.append(state.position)

        summary = summaries.get(player)
        if summary is None:
            continue
        checkpoint = int(summary["checkpoint"])
        lap = int(summary["lap"])
        if checkpoint >= MIN_FINAL_CHECKPOINT and lap >= MIN_FINAL_LAP:
            continue
        # race_check_finish() deliberately classifies the sole remaining racer
        # last once N-1 racers finish. Permit exactly that authored DNF shape,
        # while retaining substantial progress and every terminal-state check.
        if (
            state.finished == 1
            and state.position == case.players
            and checkpoint >= MIN_DNF_CHECKPOINT
            and lap >= MIN_DNF_LAP
        ):
            classified_dnfs.append(player)
            continue
        failures.append(
            f"P{player}: insufficient progress: checkpoint {checkpoint}, lap {lap}; "
            f"want >= {MIN_FINAL_CHECKPOINT}/{MIN_FINAL_LAP}, or the sole "
            f"last-place DNF at >= {MIN_DNF_CHECKPOINT}/{MIN_DNF_LAP}"
        )

    if sorted(positions) != list(range(1, case.players + 1)):
        failures.append(
            f"{case.players}P: terminal human finish positions are {sorted(positions)}; "
            f"want each of 1..{case.players} exactly once"
        )
    if len(classified_dnfs) > 1:
        failures.append(
            f"{case.players}P: multiple early last-place classifications: "
            f"{classified_dnfs}"
        )

    for first, second in itertools.combinations(range(1, case.players + 1), 2):
        common = sorted(active_sets[first] & active_sets[second])
        separations = [
            math.dist(
                (pace[first][frame].x, pace[first][frame].y, pace[first][frame].z),
                (pace[second][frame].x, pace[second][frame].y, pace[second][frame].z),
            )
            for frame in common
        ]
        if not separations:
            failures.append(f"{case.players}P: P{first}/P{second} have no common active rows")
            continue
        median = statistics.median(separations)
        if median < MIN_PAIR_MEDIAN_SEPARATION:
            failures.append(
                f"{case.players}P: P{first}/P{second} median separation "
                f"{median:.1f} < {MIN_PAIR_MEDIAN_SEPARATION}; probes may alias"
            )

    menus = [
        (int(match.group(1)), int(match.group(2)))
        for line in output.splitlines()
        if (match := MENU_RE.search(line))
    ]
    result_frames = [
        frame for menu, frame in menus if menu == MENU_RESULTS and frame > race_frame
    ]
    if not result_frames:
        failures.append(
            f"{case.players}P: race never reached MENU_RESULTS ({MENU_RESULTS}); "
            f"saw {menus}"
        )
    else:
        result_frame = result_frames[0]
        if not any(
            menu == MENU_TRACK_SELECT and frame > result_frame
            for menu, frame in menus
        ):
            failures.append(
                f"{case.players}P: MENU_RESULTS never returned to "
                f"MENU_TRACK_SELECT ({MENU_TRACK_SELECT}); saw {menus}"
            )

    dumped: list[tuple[int, str]] = []
    for filename in os.listdir(frame_dir):
        match = re.fullmatch(r"frame_(\d+)\.ppm", filename)
        if not match:
            continue
        frame = int(match.group(1))
        if VISUAL_FIRST <= frame <= VISUAL_LAST:
            dumped.append((frame, os.path.join(frame_dir, filename)))
    dumped.sort()
    if len(dumped) < 5:
        failures.append(
            f"{case.players}P: only {len(dumped)} in-race visual samples (want >= 5)"
        )

    for frame, path in dumped:
        try:
            width, height, pixels = read_ppm(path)
        except (OSError, ValueError) as error:
            failures.append(f"{case.players}P frame {frame}: {error}")
            continue
        for index, (name, bounds) in enumerate(QUADRANTS.items(), start=1):
            minimap = case.players == 3 and index == 4
            metrics = region_metrics(width, height, pixels, bounds)
            if verbose:
                kind = "minimap" if minimap else f"P{index}"
                print(
                    f"  {case.players}P frame {frame} {name} ({kind}): "
                    f"colours={metrics.colours} sigma={metrics.sigma:.1f} "
                    f"nonblack={metrics.nonblack:.1%}"
                )
            if not region_is_live(metrics, minimap):
                failures.append(
                    f"{case.players}P frame {frame} {name} is not live: "
                    f"colours={metrics.colours}, sigma={metrics.sigma:.1f}, "
                    f"nonblack={metrics.nonblack:.1%}"
                )

    # Both-direction check: flatten every quadrant of one real race frame and
    # require the same production scorer to reject all four corruptions.
    if dumped:
        frame, path = dumped[len(dumped) // 2]
        width, height, pixels = read_ppm(path)
        for index, (name, bounds) in enumerate(QUADRANTS.items(), start=1):
            minimap = case.players == 3 and index == 4
            corrupted = blank_quadrant(width, height, pixels, bounds, minimap)
            metrics = region_metrics(width, height, corrupted, bounds)
            if region_is_live(metrics, minimap):
                failures.append(
                    f"{case.players}P positive control: flattened {name} was "
                    f"accepted (colours={metrics.colours}, sigma={metrics.sigma:.1f})"
                )

    if verbose or failures:
        print(
            f"  {case.players}P race load={race_frame}; "
            f"layouts={sorted(set((h, v) for h, v, _ in layouts))}; "
            f"menus={menus}"
        )
        for player in sorted(summaries):
            summary = summaries[player]
            print(
                f"    P{player}: rows={summary['rows']} "
                f"cp={summary['checkpoint']} lap={summary['lap']} "
                f"maxStep={summary['max_step']:.1f} "
                f"slowest={summary['worst_mean']:.2f} "
                + (
                    "terminal=missing"
                    if player not in terminal
                    else f"terminal=fin{terminal[player].finished}/"
                         f"pos{terminal[player].position}/lap{terminal[player].lap}"
                )
            )

    if not keep_root:
        shutil.rmtree(frame_dir, ignore_errors=True)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--renderer", choices=("gl", "webgpu"))
    parser.add_argument("--window-size", default="640x480")
    parser.add_argument("--players", type=int, choices=(3, 4))
    parser.add_argument("--keep-frames")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if not re.fullmatch(r"[1-9]\d*x[1-9]\d*", args.window_size):
        print("FAIL: --window-size must be WIDTHxHEIGHT", file=sys.stderr)
        return 1
    binary = resolve_binary(args.build)
    selected = tuple(case for case in CASES if args.players in (None, case.players))
    required = (binary, args.rom, *(case.script for case in selected))
    for path in required:
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    if args.keep_frames:
        os.makedirs(args.keep_frames, exist_ok=True)
    failures: list[str] = []
    for case in selected:
        print(f"check_race_multiplayer: {case.players}-player arm")
        failures.extend(
            run_case(
                case,
                binary,
                args.rom,
                args.renderer,
                args.window_size,
                args.keep_frames,
                args.verbose,
            )
        )
    return print_result(failures)


if __name__ == "__main__":
    sys.exit(main())
