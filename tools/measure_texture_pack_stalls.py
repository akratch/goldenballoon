#!/usr/bin/env python3
"""Measure the frame stalls a texture pack adds at scene changes (issue #66).

A Rice or digest-keyed pack is resolved lazily: the first time the renderer
binds a texture the pack replaces, the store opens the pack, reads the PNG and
decodes it, inside the display-list walk. Entering a new area can therefore
pay for dozens of large PNGs within one frame. This tool measures how much of
a frame that is, against the same route with no pack, so a fix can be sized
from numbers instead of a guess.

It drives one fixed route -- Tracks mode: Ancient Lake, back to Track Select,
a second course, then both again -- so every area is visited COLD (first time
in the process) and WARM (again, same process). Each run is a real windowed
process at real-time pacing (MDKR_PACE_REALTIME=1): synthetic-pacer frame
times are not what a player sees and are refused for the report (--synthetic
exists only to check a route quickly, and its tables say so).

The engine side is MDKR_TEXTURE_PACK_TIMING=1 plus the resource census
(MDKR_RESOURCE_STATS=1); see the block above dkr_bind_tile in
platform/fast3d/gfx_pc_dkr.c and mod_texture_store.h. Every presented frame
emits one [TEXPACK-FRAME] row: wall time since the previous present, the walk
and end-of-frame time, and what that frame spent opening the pack, reading
(and inflating) PNGs, decoding them, hashing texture identities and uploading
the results. The census marks each level load, which is how frames are split
into areas and visits.

Usage (the pack is private: it is copied into a fresh temporary mods
directory per run and never anywhere else):

    python3 tools/measure_texture_pack_stalls.py --build build-rel \\
        --rom /path/to/baserom.us.v80.z64 --texture-pack /path/to/pack.zip \\
        --json /private/path/stalls.json

    # pack inventory and a single-threaded decode-everything benchmark only
    python3 tools/measure_texture_pack_stalls.py --inventory \\
        --texture-pack /path/to/pack.zip

Exit status is 0 when every requested run completed and covered the route,
1 otherwise. The numbers themselves are evidence, not a gate.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ASSET_LEVEL_HEADERS order, game/include/asset_enums.h.
LEVEL_NAMES = {
    0: "Timber's Island", 1: "Bluey 1", 2: "Dragon Forest hub",
    3: "Fossil Canyon", 4: "Pirate Lagoon", 5: "Ancient Lake",
    6: "Walrus Cove", 7: "Hot Top Volcano", 8: "Whale Bay",
    9: "Snowball Valley", 10: "Crescent Island", 11: "Fire Mountain",
    12: "Dino Domain hub", 13: "Everfrost Peak", 14: "Sherbet Island hub",
    15: "Spaceport Alpha", 16: "Horseshoe Gulch", 17: "Spacedust Alley",
    18: "Greenwood Village", 19: "Boulder Canyon", 20: "Windmill Plains",
    21: "Front end", 22: "Character select", 23: "Title sequence",
    24: "Snowflake Mountain hub", 25: "Smokey Castle",
    26: "Darkwater Beach", 27: "Icicle Pyramid", 28: "Frosty Village",
    29: "Jungle Falls", 30: "Treasure Caves", 31: "Haunted Woods",
    32: "Darkmoon Caverns", 33: "Star City", 34: "Trophy race",
    35: "Future Fun Land hub", 36: "Opening sequence", 39: "Options background",
}

# Levels the menus load as backgrounds. A race is a level load with
# cutscene 0 that is not one of these; the Track Select preview of a course is
# the same level id with cutscene 1.
MENU_LEVELS = {21, 22, 23, 36, 39}

# The route, in ORIGINAL-cadence simulation ticks. Inputs are tick-indexed
# (platform_input_commit_tick), so they land on the same game state at any
# wall-clock speed; the Enhanced cadence runs two ticks per original one and
# the route is scaled by two. The front end is
# tests/input_scripts/nav_to_time_trial_race.txt (one-ticket lead included);
# the pause menu of a 1P Tracks race is CONTINUE / RESTART RACE / SELECT TRACK
# (menu.c menu_pause_init), and Track Select's DOWN moves to the next world's
# first course (always unlocked).
ROUTE = [
    # Title -> character select -> caution -> Game Select -> Tracks.
    (1249, "START"), (1329, "START"), (1449, "A"), (1539, "A"), (1749, "A"),
    (1899, "DOWN"), (1999, "A"),
    # Track Select at (0,0): Ancient Lake; vehicle; Time Trial off; GO.
    (2149, "A"), (2319, "A"), (2459, "A"), (2599, "A"),
    # Race 1 (Ancient Lake, cold). Pause -> SELECT TRACK.
    (3500, "START"), (3620, "DOWN"), (3740, "DOWN"), (3860, "A"),
    # Track Select again: DOWN to the next world's first course; race 2.
    (4110, "DOWN"), (4260, "A"), (4430, "A"), (4570, "A"), (4710, "A"),
    # Race 2 (second course, cold). Pause -> SELECT TRACK.
    (5610, "START"), (5730, "DOWN"), (5850, "DOWN"), (5970, "A"),
    # UP back to Ancient Lake; race 3 (warm).
    (6220, "UP"), (6370, "A"), (6540, "A"), (6680, "A"), (6820, "A"),
    (7720, "START"), (7840, "DOWN"), (7960, "DOWN"), (8080, "A"),
    # DOWN to the second course again; race 4 (warm).
    (8330, "DOWN"), (8480, "A"), (8650, "A"), (8790, "A"), (8930, "A"),
]
ROUTE_END_TICKS = 9900
HOLD_TICKS = 4

FRAME_TAG = "[TEXPACK-FRAME]"
CACHE_RE = re.compile(
    r"texture_cache_state: level=(-?\d+) players=(-?\d+) cutscene=(-?\d+)")
STALL_MS = (33.3, 50.0)
FATAL_MARKERS = ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:")


def route_script(scale: int) -> str:
    lines = [
        "# Generated by tools/measure_texture_pack_stalls.py (issue #66).",
        f"# Original-cadence ticks scaled by {scale}.",
    ]
    for tick, token in ROUTE:
        lines.append(f"{tick * scale} {token} {HOLD_TICKS * scale}")
    return "\n".join(lines) + "\n"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def wait_for_quiet(max_load: float, wait_s: float) -> dict:
    """Record the load average, waiting (bounded) for a busy machine."""
    started = time.monotonic()
    first = os.getloadavg()
    load = first
    while load[0] > max_load and time.monotonic() - started < wait_s:
        print(f"  load {load[0]:.2f} > {max_load:.2f}; waiting", flush=True)
        time.sleep(30)
        load = os.getloadavg()
    return {
        "before": [round(value, 2) for value in first],
        "at_start": [round(value, 2) for value in load],
        "waited_s": round(time.monotonic() - started, 1),
        "quiet": load[0] <= max_load,
    }


# --------------------------------------------------------------- parsing


def parse_run(output: str) -> tuple[list[dict], list[dict]]:
    """Frame rows (each tagged with its area visit) and the visit list."""
    frames: list[dict] = []
    visits: list[dict] = [{
        "index": 0, "level": None, "cutscene": None, "label": "boot",
        "visit": 0, "kind": "boot",
    }]
    seen: collections.Counter = collections.Counter()
    for line in output.splitlines():
        match = CACHE_RE.search(line)
        if match:
            level, _players, cutscene = (int(value) for value in match.groups())
            key = (level, cutscene)
            name = LEVEL_NAMES.get(level, f"level {level}")
            if level not in MENU_LEVELS and cutscene == 1:
                name += " (Track Select preview)"
            elif level not in MENU_LEVELS and cutscene == 0:
                name += " (race)"
            visits.append({
                "index": len(visits), "level": level, "cutscene": cutscene,
                "label": name, "visit": seen[key],
                "kind": "cold" if seen[key] == 0 else "warm",
            })
            seen[key] += 1
            continue
        position = line.find(FRAME_TAG)
        if position < 0:
            continue
        row: dict = {}
        for token in line[position + len(FRAME_TAG):].split():
            key, separator, value = token.partition("=")
            if separator:
                row[key] = int(value)
        row["visit_index"] = len(visits) - 1
        frames.append(row)
    return frames, visits


SUM_FIELDS = (
    "cpuUs", "walkUs", "endUs", "misses", "hashUs", "resolves", "loaded",
    "probes", "absent", "files", "fileBytes", "decodedBytes", "openUs",
    "absentUs", "readUs", "decodeUs", "resolveUs", "overrideUploads",
    "overrideBytes", "overrideUploadUs", "romUploads", "romUploadUs",
)


def displayed_frames(rows: list[dict]) -> list[dict]:
    """Present-to-present intervals of frames that reached the screen.

    A present that held the previous image (swap=0) is folded into the next
    displayed one, with its work, because that is what the player waited for.
    The first row has no predecessor and is dropped.
    """
    out: list[dict] = []
    pending: dict | None = None
    for row in rows[1:]:
        if pending is None:
            pending = {key: 0 for key in SUM_FIELDS}
            pending["dtUs"] = 0
            pending["presents"] = 0
        for key in SUM_FIELDS:
            pending[key] += row.get(key, 0)
        pending["dtUs"] += row.get("dtUs", 0)
        pending["presents"] += 1
        if row.get("swap", 1):
            pending.update(frame=row["frame"], tick=row["tick"],
                           visit_index=row["visit_index"])
            out.append(pending)
            pending = None
    return out


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    # Nearest rank: the smallest value with at least `fraction` of the
    # samples at or below it.
    rank = max(0, min(len(ordered) - 1,
                      math.ceil(fraction * len(ordered)) - 1))
    return ordered[rank]


def summarize(frames: list[dict]) -> dict:
    times = [frame["dtUs"] / 1000.0 for frame in frames]
    return {
        "frames": len(times),
        "p50_ms": round(percentile(times, 0.50), 2),
        "p99_ms": round(percentile(times, 0.99), 2),
        "max_ms": round(max(times), 2) if times else 0.0,
        "over_33_3": sum(1 for value in times if value > STALL_MS[0]),
        "over_50": sum(1 for value in times if value > STALL_MS[1]),
        "resolves": sum(frame["resolves"] for frame in frames),
        "loaded": sum(frame["loaded"] for frame in frames),
        "decoded_mib": round(
            sum(frame["decodedBytes"] for frame in frames) / 2**20, 1),
        "probes": sum(frame["probes"] for frame in frames),
        "absent": sum(frame["absent"] for frame in frames),
        "open_ms": round(sum(frame["openUs"] for frame in frames) / 1000, 1),
        "absent_ms": round(
            sum(frame["absentUs"] for frame in frames) / 1000, 1),
        "read_ms": round(sum(frame["readUs"] for frame in frames) / 1000, 1),
        "decode_ms": round(
            sum(frame["decodeUs"] for frame in frames) / 1000, 1),
        "upload_ms": round(
            sum(frame["overrideUploadUs"] for frame in frames) / 1000, 1),
    }


def frame_record(frame: dict, visits: list[dict]) -> dict:
    visit = visits[frame["visit_index"]]
    walk = frame["walkUs"]
    accounted = (frame["hashUs"] + frame["resolveUs"] +
                 frame["overrideUploadUs"] + frame["romUploadUs"])
    return {
        "frame": frame["frame"], "tick": frame["tick"],
        "area": visit["label"], "kind": visit["kind"],
        "frame_ms": round(frame["dtUs"] / 1000, 2),
        "resolves": frame["resolves"], "loaded": frame["loaded"],
        "probes": frame["probes"], "absent": frame["absent"],
        "files": frame["files"],
        "file_kib": round(frame["fileBytes"] / 1024, 1),
        "decoded_mib": round(frame["decodedBytes"] / 2**20, 2),
        "open_ms": round(frame["openUs"] / 1000, 2),
        "absent_ms": round(frame["absentUs"] / 1000, 2),
        "read_ms": round(frame["readUs"] / 1000, 2),
        "decode_ms": round(frame["decodeUs"] / 1000, 2),
        "resolve_ms": round(frame["resolveUs"] / 1000, 2),
        "hash_ms": round(frame["hashUs"] / 1000, 2),
        "upload_ms": round(frame["overrideUploadUs"] / 1000, 2),
        "uploads": frame["overrideUploads"],
        "upload_mib": round(frame["overrideBytes"] / 2**20, 2),
        "rom_upload_ms": round(frame["romUploadUs"] / 1000, 2),
        "cpu_ms": round(frame["cpuUs"] / 1000, 2),
        "walk_ms": round(walk / 1000, 2),
        "walk_other_ms": round(max(0, walk - accounted) / 1000, 2),
        "end_ms": round(frame["endUs"] / 1000, 2),
        "outside_render_ms": round(
            max(0, frame["dtUs"] - walk - frame["endUs"]) / 1000, 2),
    }


def analyse(output: str) -> dict:
    rows, visits = parse_run(output)
    frames = displayed_frames(rows)
    counts = collections.Counter(
        (visit["level"], visit["cutscene"]) for visit in visits[1:])
    repeated = {key for key, count in counts.items() if count >= 2}

    def in_repeated(frame: dict, kind: str) -> bool:
        visit = visits[frame["visit_index"]]
        return ((visit["level"], visit["cutscene"]) in repeated and
                visit["kind"] == kind)

    per_visit = []
    for visit in visits:
        mine = [frame for frame in frames
                if frame["visit_index"] == visit["index"]]
        if mine:
            per_visit.append({**visit, **summarize(mine)})
    worst = sorted(frames, key=lambda frame: -frame["dtUs"])[:10]
    return {
        "presents": len(rows),
        "displayed": len(frames),
        "all": summarize(frames),
        "cold": summarize([f for f in frames if in_repeated(f, "cold")]),
        "warm": summarize([f for f in frames if in_repeated(f, "warm")]),
        "repeated_areas": sorted(
            f"{LEVEL_NAMES.get(level, level)} (cutscene {cutscene})"
            for level, cutscene in repeated),
        "visits": per_visit,
        "worst": [frame_record(frame, visits) for frame in worst],
    }


def route_error(result: dict) -> str | None:
    races = collections.Counter(
        visit["level"] for visit in result["visits"]
        if visit["level"] is not None and visit["level"] not in MENU_LEVELS
        and visit["cutscene"] == 0)
    if races.get(5, 0) < 2:
        return f"Ancient Lake was not raced twice: {dict(races)}"
    others = [level for level, count in races.items()
              if level != 5 and count >= 2]
    if not others:
        return f"no second course was raced twice: {dict(races)}"
    return None


# ------------------------------------------------------------------ runs


def run_arm(args, renderer: str, pack: Path | None, artifacts: Path) -> dict:
    label = f"{renderer}-{'pack' if pack else 'nopack'}"
    run_root = Path(tempfile.mkdtemp(prefix=f"{label}-", dir=artifacts))
    scale = 2 if args.cadence == "enhanced" else 1
    ticks = ROUTE_END_TICKS * scale
    mods = run_root / "mods"
    script = run_root / "route.txt"
    script.write_text(route_script(scale), encoding="utf-8")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update({
        "LC_ALL": "C",
        # Muted, but the synthesiser still runs (audi_port_dkr.c), so its
        # cost stays inside the measured frames.
        "MDKR_AUDIO": "0",
        "MDKR_AUTOPILOT": "1",
        "MDKR_RENDERER": renderer,
        "MDKR_SIMULATION_CADENCE": args.cadence,
        "MDKR_RESOURCE_STATS": "1",
        "MDKR_TEXTURE_PACK_TIMING": "1",
        "MDKR_APP_PREFS_DIR": str(run_root),
        "MDKR_SAVE_DIR": str(run_root / "save"),
        "MDKR_VIDEO_CONFIG_PATH": str(run_root / "save" / "video.ini"),
        # A hidden native window has no drawable; frame times are only
        # meaningful when images actually reach a surface.
        "MDKR_TEST_VISIBLE_HEADLESS": "1",
    })
    if not args.synthetic:
        env["MDKR_PACE_REALTIME"] = "1"
    command = [
        str(args.binary), "--headless-ticks", str(ticks),
        "--window-size", args.window_size,
        "--input-script", str(script), "--rom", str(args.rom),
    ]
    load = wait_for_quiet(args.max_load, args.load_wait)
    # The private pack is copied only now, after the wait, and is removed in
    # the finally below however the run ends: completed, failed, timed out,
    # Ctrl-C, or SIGTERM (which main() turns into SystemExit for exactly this
    # reason). subprocess.run() kills the game first on any exception, so
    # nothing still holds the copy open when it is deleted.
    try:
        if pack is not None:
            mods.mkdir()
            shutil.copyfile(pack, mods / "texture-pack.zip")
        print(f"{label}: load {load['at_start']} -> running "
              f"({ticks} ticks, {args.cadence})", flush=True)
        log_path = run_root / "run.log"
        started = time.monotonic()
        tick_hz = 60 if scale == 2 else 30
        timeout = args.timeout or (ticks / tick_hz) * 2 + 180
        try:
            with log_path.open("w", encoding="utf-8") as log:
                process = subprocess.run(
                    command, cwd=run_root, env=env, stdout=log,
                    stderr=subprocess.STDOUT, text=True, timeout=timeout,
                    check=False)
            returncode = process.returncode
        except subprocess.TimeoutExpired:
            returncode = None
        wall = time.monotonic() - started
        output = log_path.read_text(encoding="utf-8", errors="replace")
        record: dict = {
            "label": label, "renderer": renderer, "pack": pack is not None,
            "cadence": args.cadence, "realtime": not args.synthetic,
            "window": args.window_size, "ticks": ticks,
            "returncode": returncode, "wall_s": round(wall, 1), "load": load,
            "log": str(log_path),
            "present_mode": next((line.strip() for line in output.splitlines()
                                  if "[PRESENT-MODE]" in line), None),
            "pack_used": next((line.strip() for line in output.splitlines()
                               if "high-resolution texture" in line and
                               "used this run" in line), None),
        }
        problems = []
        if returncode != 0:
            problems.append(f"exit {returncode}")
        if f"renderer backend: {renderer}" not in output:
            problems.append("requested backend was not active")
        for marker in FATAL_MARKERS:
            if marker in output:
                problems.append(f"fatal marker {marker}")
        if pack is not None and record["pack_used"] is None:
            problems.append("the pack supplied no rendered texture")
        if pack is None and "pack(s) active" in output and \
                "0 pack(s) active" not in output:
            problems.append("a pack was active in the no-pack arm")
        if FRAME_TAG not in output:
            problems.append("no [TEXPACK-FRAME] rows: instrument missing")
        if not problems:
            record.update(analyse(output))
            error = route_error(record)
            if error:
                problems.append(error)
        record["problems"] = problems
        return record
    finally:
        if pack is not None and not args.keep_pack:
            # The run's log stays; the private pack copy does not.
            shutil.rmtree(mods, ignore_errors=True)


# ------------------------------------------------------------ inventory


def inventory(pack: Path, artifacts: Path, repeats: int, max_load: float,
              load_wait: float) -> dict:
    sides = collections.Counter()
    variants = collections.Counter()
    total_pixels = 0
    count = 0
    with zipfile.ZipFile(pack) as archive:
        for info in archive.infolist():
            if not info.filename.lower().endswith(".png"):
                continue
            with archive.open(info) as stream:
                header = stream.read(33)
            if header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
                continue
            width, height = struct.unpack(">II", header[16:24])
            count += 1
            total_pixels += width * height
            side = max(width, height)
            bucket = next((edge for edge in (64, 128, 256, 512, 1024, 2048)
                           if side <= edge), 4096)
            sides[f"<= {bucket}"] += 1
            stem = info.filename.rsplit("/", 1)[-1].rsplit(".", 1)[0]
            variants[stem.rsplit("_", 1)[-1] if "_" in stem else "?"] += 1
    result = {
        "png_files": count,
        "max_side_histogram": dict(sorted(
            sides.items(), key=lambda item: int(item[0].split()[-1]))),
        "variants": dict(variants.most_common()),
        "decoded_mib_if_all_loaded": round(total_pixels * 4 / 2**20, 1),
    }
    bench = compile_bench(artifacts)
    if bench is None:
        result["bench"] = "not built (no host C compiler)"
        return result
    mods = Path(tempfile.mkdtemp(prefix="bench-mods-", dir=artifacts))
    try:
        shutil.copyfile(pack, mods / "texture-pack.zip")
        load = wait_for_quiet(max_load, load_wait)
        process = subprocess.run(
            [str(bench), str(mods), str(repeats)], capture_output=True,
            text=True, timeout=1800, check=False)
        result["bench_load"] = load
        if process.returncode == 0:
            last = process.stdout.strip().splitlines()[-1]
            result["bench"] = json.loads(last)
        else:
            result["bench"] = f"failed: {process.stderr.strip()[-400:]}"
    finally:
        shutil.rmtree(mods, ignore_errors=True)
    return result


def compile_bench(artifacts: Path) -> Path | None:
    compiler = shutil.which(os.environ.get("CC", "cc"))
    if compiler is None:
        return None
    out = artifacts / "texture_pack_decode_bench"
    sources = [
        ROOT / "tools" / "texture_pack_decode_bench.c",
        ROOT / "platform" / "mod_texture_store.c",
        ROOT / "platform" / "mod_registry.c",
        ROOT / "platform" / "mod_manifest.c",
        ROOT / "platform" / "config_ini.c",
        ROOT / "platform" / "fs_utf8.c",
        ROOT / "platform" / "mod_source.c",
    ]
    third_party = [ROOT / "lib" / "stb" / "stb_image_impl.c",
                   ROOT / "lib" / "miniz" / "miniz.c"]
    includes = [f"-I{ROOT / 'platform'}", f"-I{ROOT / 'lib' / 'stb'}",
                f"-I{ROOT / 'lib' / 'miniz'}"]
    objects = []
    # Same optimisation as a CMake Release build of the game.
    for source in third_party:
        obj = artifacts / (source.stem + ".o")
        subprocess.run([compiler, "-O3", "-DNDEBUG", "-w", *includes, "-c",
                        str(source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    subprocess.run([compiler, "-O3", "-DNDEBUG", "-std=gnu11", *includes,
                    *map(str, sources), *objects, "-o", str(out)], check=True)
    return out


# --------------------------------------------------------------- report


def print_tables(records: list[dict], synthetic: bool) -> None:
    if synthetic:
        print("\nSYNTHETIC PACING: route check only. These are NOT player "
              "frame times.")
    print("\n| arm | set | frames | p50 ms | p99 ms | max ms | >33.3 | >50 | "
          "loaded | decoded MiB | open ms | read ms | decode ms | upload ms |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for record in records:
        if record.get("problems"):
            print(f"| {record['label']} | FAILED: "
                  f"{'; '.join(record['problems'])} |")
            continue
        for key in ("cold", "warm", "all"):
            row = record[key]
            print(f"| {record['label']} | {key} | {row['frames']} | "
                  f"{row['p50_ms']} | {row['p99_ms']} | {row['max_ms']} | "
                  f"{row['over_33_3']} | {row['over_50']} | {row['loaded']} | "
                  f"{row['decoded_mib']} | {row['open_ms']} | "
                  f"{row['read_ms']} | {row['decode_ms']} | "
                  f"{row['upload_ms']} |")
    for record in records:
        if record.get("problems"):
            continue
        print(f"\n{record['label']}: 10 worst frames")
        print("| frame ms | cpu ms | area | kind | loaded/resolves | "
              "decoded MiB | open ms (absent) | read ms | decode ms | "
              "upload ms | hash ms | walk other ms | outside render ms |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for frame in record["worst"]:
            print(f"| {frame['frame_ms']} | {frame['cpu_ms']} | "
                  f"{frame['area']} | {frame['kind']} | "
                  f"{frame['loaded']}/{frame['resolves']} | "
                  f"{frame['decoded_mib']} | {frame['open_ms']} "
                  f"({frame['absent_ms']}) | {frame['read_ms']} | "
                  f"{frame['decode_ms']} | {frame['upload_ms']} | "
                  f"{frame['hash_ms']} | {frame['walk_other_ms']} | "
                  f"{frame['outside_render_ms']} |")


def _terminate(signum, _frame) -> None:
    """SIGTERM -> SystemExit, so every pending finally still runs.

    Python's default SIGTERM action ends the process without unwinding, which
    would leave a run's private pack copy behind. Later SIGTERMs are ignored so
    the cleanup itself cannot be cut short; SIGKILL remains the hard stop.
    """
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    raise SystemExit(128 + signum)


def main() -> int:
    signal.signal(signal.SIGTERM, _terminate)
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default="build-rel",
                        help="build directory or mdkr64 executable")
    parser.add_argument("--rom", type=Path)
    parser.add_argument("--texture-pack", type=Path)
    parser.add_argument("--expect-sha256",
                        help="refuse to run unless the pack has this digest")
    parser.add_argument("--renderer", choices=("webgpu", "gl", "both"),
                        default="both")
    parser.add_argument("--arms", choices=("pack", "nopack", "both"),
                        default="both")
    parser.add_argument("--cadence", choices=("enhanced", "original"),
                        default="enhanced",
                        help="simulation cadence (enhanced = 60 Hz frames)")
    parser.add_argument("--window-size", default="960x540")
    parser.add_argument("--synthetic", action="store_true",
                        help="route check only: synthetic pacing, NOT player "
                             "frame times")
    parser.add_argument("--max-load", type=float, default=4.0,
                        help="wait before a run while the 1-minute load "
                             "average is above this")
    parser.add_argument("--load-wait", type=float, default=900.0)
    parser.add_argument("--timeout", type=float, default=0.0)
    parser.add_argument("--inventory", action="store_true",
                        help="pack inventory and decode benchmark only")
    parser.add_argument("--open-probe-repeats", type=int, default=200)
    parser.add_argument("--artifacts-dir", type=Path,
                        help="keep run logs here (default: a temp dir)")
    parser.add_argument("--keep-pack", action="store_true",
                        help="keep each run's private pack copy")
    parser.add_argument("--json", type=Path, help="write the results here")
    parser.add_argument("--reanalyse", type=Path, metavar="JSON",
                        help="re-read the run logs an earlier --json names "
                             "and recompute its tables (no runs)")
    args = parser.parse_args()

    if args.reanalyse is not None:
        results = json.loads(args.reanalyse.read_text(encoding="utf-8"))
        for record in results.get("runs", []):
            if record.get("problems"):
                continue
            output = Path(record["log"]).read_text(
                encoding="utf-8", errors="replace")
            record.update(analyse(output))
        print_tables(results.get("runs", []),
                     any(not run.get("realtime", True)
                         for run in results.get("runs", [])))
        if args.json:
            args.json.write_text(json.dumps(results, indent=2))
            print(f"\nwrote {args.json}")
        return 0

    if args.texture_pack is None:
        parser.error("--texture-pack is required")
    pack = args.texture_pack.expanduser().resolve()
    if not pack.is_file():
        parser.error("--texture-pack must name an existing file")
    pack_sha = sha256(pack)
    if args.expect_sha256 and pack_sha != args.expect_sha256.lower():
        print(f"pack SHA-256 {pack_sha} != expected {args.expect_sha256}",
              file=sys.stderr)
        return 1
    artifacts = (args.artifacts_dir.expanduser().resolve()
                 if args.artifacts_dir else
                 Path(tempfile.mkdtemp(prefix="texpack-stalls-")))
    artifacts.mkdir(parents=True, exist_ok=True)
    if args.json:
        args.json = args.json.expanduser().resolve()
        args.json.parent.mkdir(parents=True, exist_ok=True)
    results: dict = {
        "pack": {"name": pack.name, "sha256": pack_sha,
                 "bytes": pack.stat().st_size},
        "artifacts": str(artifacts),
        "host": {"cpus": os.cpu_count(), "platform": sys.platform},
    }
    print(f"pack {pack.name}: sha256 {pack_sha}")

    if args.inventory:
        results["inventory"] = inventory(pack, artifacts,
                                         args.open_probe_repeats,
                                         args.max_load, args.load_wait)
        print(json.dumps(results["inventory"], indent=2))
    else:
        if args.rom is None:
            parser.error("--rom is required unless --inventory")
        build = Path(args.build).expanduser()
        args.binary = (build / "mdkr64" if build.is_dir() else build).resolve()
        args.rom = args.rom.expanduser().resolve()
        if not args.binary.is_file() or not args.rom.is_file():
            parser.error("the binary or ROM does not exist")
        renderers = (("webgpu", "gl") if args.renderer == "both"
                     else (args.renderer,))
        arms = {"pack": (pack,), "nopack": (None,),
                "both": (None, pack)}[args.arms]
        records = []
        for renderer in renderers:
            for arm_pack in arms:
                records.append(run_arm(args, renderer, arm_pack, artifacts))
                if args.json:
                    results["runs"] = records
                    args.json.write_text(json.dumps(results, indent=2))
        results["runs"] = records
        print_tables(records, args.synthetic)

    if args.json:
        args.json.write_text(json.dumps(results, indent=2))
        print(f"\nwrote {args.json}")
    failed = [record["label"] for record in results.get("runs", [])
              if record.get("problems")]
    if failed:
        print(f"incomplete runs: {', '.join(failed)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
