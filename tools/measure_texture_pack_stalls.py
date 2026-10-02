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
and end-of-frame time, and what that frame spent on pack textures. The census
marks each level load, which is how frames are split into areas and visits.

Since the off-frame loader (issue #66) the per-frame figure that matters is
GAME-THREAD PACK WORK, packUs = hashUs + resolveUs + pumpUs + overrideUploadUs:
identity hashing, store lookups, installing finished textures (and, under the
BUDGET loader, reading and decoding them), and uploading replacements. Raw
frame time is not used for that: T12a showed pack-independent waits of 35-75 ms
that would be misattributed. packUs leaves one thing out on purpose: when a
replacement is still pending, or deferred past the frame's upload budget, that
frame draws the texture's ORIGINAL, and that ROM decode and upload is in
romUploadUs with every other ROM fill. It is the same work a run without the
pack does for that texture on that frame, so it is not work the pack adds; the
replacement's own upload, when it goes up, is in overrideUploadUs. (Against
1.7's synchronous loader, which uploaded only the replacement, it is extra, and
romUploadUs bounds it.) Each texture the store installs also prints a
[TEXPACK-INSTALL] row, which gives the request->resident latency in frames and
the set of identities that became resident (compare it across loaders).
`--loader inline|budget|worker` forces a loader for A/B runs; `--state-hash`
records the authoritative [SIMHASH] stream (use with --synthetic, the only
pacing under which two runs simulate identically).

The same rows carry a frame-wait split (T12c): the game pass, the pacer's
sleep and how late it woke, the event pump, swap, audio, the row's own print,
and host counters (off-CPU time, priority, whether the window was visible and
the app frontmost). Every frame over 33.3 ms is attributed to the part that
grew most over its run median. `--arms nopack` runs without a pack;
`--variant`, `--env KEY=VALUE`, `--hidden` and `--repeat` drive the comparison
arms (tests/README.md).

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
import threading
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
INSTALL_TAG = "[TEXPACK-INSTALL]"
ACTIVE_PACKS_RE = re.compile(r"\[MODS\] ([0-9]+) pack\(s\) active")
RATE_RE = re.compile(r"\[PRESENT-MODE\].*?\brate=([0-9]+)")
# Game-thread pack work per frame, in microseconds (see the module docstring).
# Game-thread pack work. Not romUploadUs: the original drawn while a
# replacement is pending or deferred costs what a run without the pack pays for
# that texture anyway (see the module docstring).
PACK_WORK_FIELDS = ("hashUs", "resolveUs", "pumpUs", "overrideUploadUs")
PACK_WORK_LIMIT_US = 4000
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


def parse_installs(output: str) -> list[dict]:
    """[TEXPACK-INSTALL] rows: key, resident, frames (latency), us."""
    rows = []
    for line in output.splitlines():
        position = line.find(INSTALL_TAG)
        if position < 0:
            continue
        row: dict = {}
        for token in line[position + len(INSTALL_TAG):].split():
            key, separator, value = token.partition("=")
            if separator:
                row[key] = value if key == "key" else int(value)
        rows.append(row)
    return rows


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
    # The off-frame loader's fields; absent (0) in pre-#66 logs.
    "pumpUs", "workerUs", "queued", "installed", "discarded", "refills",
    "deferred", "unbudgeted",
    # Frame-wait attribution (T12c); absent (0) in earlier logs.
    "gameUs", "startUs", "acquireUs", "submitUs", "presentUs", "paceUs",
    "paceReqUs", "paceCalls", "eventsUs", "swapUs", "fenceUs", "audioUs",
    "rowUs", "procCpuUs", "runnableUs", "lowQosUs", "pageins", "vcsw",
    "ivcsw", "replayUs", "replayWalkUs",
)
# Not sums: the latest wake past target is a maximum, the rest are states.
MAX_FIELDS = ("paceLateUs",)
LAST_FIELDS = ("tUs", "prio", "visible", "active")

# Where a waiting frame's time went (T12c). Each part is microseconds of one
# displayed frame; together they tile dtUs, and "unaccounted" is the rest.
# game = logic + start + walk + end; end = acquire + submit + present + other.
# With smoothing on, presentation replays run in the host boundary: replayUs
# is all of them, their walk is also in walkUs (replayWalkUs of it), and their
# acquire/submit/present are only in replayUs (gfx_frame_wait.h). So the game
# pass's own walk is walkUs - replayWalkUs.
def _game_walk(f: dict) -> int:
    return max(0, f["walkUs"] - f.get("replayWalkUs", 0))


WAIT_PARTS = (
    ("game logic", lambda f: max(0, f["gameUs"] - f["startUs"] -
                                 _game_walk(f) - f["endUs"])),
    ("frame start", lambda f: f["startUs"]),
    ("walk", _game_walk),
    ("acquire", lambda f: f["acquireUs"]),
    ("submit", lambda f: f["submitUs"]),
    ("present", lambda f: f["presentUs"]),
    ("end other", lambda f: max(0, f["endUs"] - f["acquireUs"] -
                                f["submitUs"] - f["presentUs"])),
    ("pacer asked", lambda f: min(f["paceUs"], f["paceReqUs"])),
    ("pacer oversleep", lambda f: max(0, f["paceUs"] - f["paceReqUs"])),
    ("events", lambda f: f["eventsUs"]),
    ("swap", lambda f: f["swapUs"]),
    ("fence", lambda f: f["fenceUs"]),
    ("audio", lambda f: f["audioUs"]),
    ("row print", lambda f: f["rowUs"]),
    ("replay", lambda f: f.get("replayUs", 0)),
    ("unaccounted", lambda f: max(0, f["dtUs"] - f["gameUs"] - f["paceUs"] -
                                  f["eventsUs"] - f["swapUs"] -
                                  f["fenceUs"] - f["audioUs"] -
                                  f["rowUs"] - f.get("replayUs", 0))),
)


def pack_us(frame: dict) -> int:
    return sum(frame.get(field, 0) for field in PACK_WORK_FIELDS)


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
        for key in MAX_FIELDS:
            pending[key] = max(pending.get(key, 0), row.get(key, 0))
        for key in LAST_FIELDS:
            pending[key] = row.get(key, -1)
        pending["dtUs"] += row.get("dtUs", 0)
        pending["presents"] += 1
        if row.get("swap", 1):
            pending.update(frame=row["frame"], tick=row["tick"],
                           visit_index=row["visit_index"],
                           pendingTextures=row.get("pending", 0))
            pending["packUs"] = pack_us(pending)
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
    pack = [frame["packUs"] / 1000.0 for frame in frames]
    return {
        "frames": len(times),
        "p50_ms": round(percentile(times, 0.50), 2),
        "p99_ms": round(percentile(times, 0.99), 2),
        "max_ms": round(max(times), 2) if times else 0.0,
        "pack_max_ms": round(max(pack), 2) if pack else 0.0,
        "pack_p99_ms": round(percentile(pack, 0.99), 2),
        "pack_over_4ms": sum(1 for frame in frames
                             if frame["packUs"] > PACK_WORK_LIMIT_US),
        "pump_ms": round(sum(frame["pumpUs"] for frame in frames) / 1000, 1),
        "worker_ms": round(
            sum(frame["workerUs"] for frame in frames) / 1000, 1),
        "queued": sum(frame["queued"] for frame in frames),
        "installed": sum(frame["installed"] for frame in frames),
        "refills": sum(frame["refills"] for frame in frames),
        "deferred": sum(frame["deferred"] for frame in frames),
        "unbudgeted": sum(frame["unbudgeted"] for frame in frames),
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
        "pack_ms": round(frame["packUs"] / 1000, 2),
        "pump_ms": round(frame["pumpUs"] / 1000, 2),
        "installed": frame["installed"],
        "refills": frame["refills"],
        "deferred": frame["deferred"],
        "unbudgeted": frame["unbudgeted"],
        "pending": frame["pendingTextures"],
        "cpu_ms": round(frame["cpuUs"] / 1000, 2),
        "walk_ms": round(walk / 1000, 2),
        "walk_other_ms": round(max(0, walk - accounted) / 1000, 2),
        "end_ms": round(frame["endUs"] / 1000, 2),
        "outside_render_ms": round(
            max(0, frame["dtUs"] - walk - frame["endUs"]) / 1000, 2),
    }


def wait_split(frame: dict) -> dict:
    return {name: part(frame) for name, part in WAIT_PARTS}


def wait_attribution(frames: list[dict], visits: list[dict]) -> dict:
    """Which wait each long frame spent its time in (T12c).

    A part's EXCESS is its value in the long frame minus its median over the
    run, so the pacer's ordinary 14 ms sleep does not count as the cause of a
    50 ms frame; the cause is the part with the largest excess. offCpuUs is
    the process's runnable-but-not-running time (host_sched_probe.h): a
    thread was ready and the host gave it no core.
    """
    if not frames or "gameUs" not in frames[0] or not any(
            frame["gameUs"] for frame in frames):
        return {"available": False}
    medians = {}
    for name, part in WAIT_PARTS:
        medians[name] = percentile([part(frame) for frame in frames], 0.50)
    def off_cpu(frame: dict) -> int:
        return max(0, frame["runnableUs"] - frame["procCpuUs"])
    stalls = []
    causes: collections.Counter = collections.Counter()
    for frame in frames:
        if frame["dtUs"] <= STALL_MS[0] * 1000:
            continue
        split = wait_split(frame)
        excess = {name: value - medians[name] for name, value in split.items()}
        cause = max(excess, key=lambda name: excess[name])
        causes[cause] += 1
        visit = visits[frame["visit_index"]]
        stalls.append({
            "frame": frame["frame"], "tick": frame["tick"],
            "t_s": round(frame.get("tUs", 0) / 1e6, 3),
            "area": visit["label"], "kind": visit["kind"],
            "frame_ms": round(frame["dtUs"] / 1000, 2),
            "cpu_ms": round(frame["cpuUs"] / 1000, 2),
            "cause": cause,
            "cause_excess_ms": round(excess[cause] / 1000, 2),
            "pace_late_ms": round(frame.get("paceLateUs", 0) / 1000, 2),
            "off_cpu_ms": round(off_cpu(frame) / 1000, 2),
            "proc_cpu_ms": round(frame["procCpuUs"] / 1000, 2),
            "low_qos_ms": round(frame["lowQosUs"] / 1000, 2),
            "pageins": frame["pageins"], "ivcsw": frame["ivcsw"],
            "prio": frame.get("prio", -1),
            "visible": frame.get("visible", -1),
            "active": frame.get("active", -1),
            "split_ms": {name: round(value / 1000, 2)
                         for name, value in split.items() if value >= 500},
        })
    normal = [frame for frame in frames if frame["dtUs"] <= 20000]
    def summary(values: list[float]) -> dict:
        return {"p50": round(percentile(values, 0.50), 2),
                "p99": round(percentile(values, 0.99), 2),
                "max": round(max(values), 2) if values else 0.0}
    priorities = collections.Counter(frame.get("prio", -1)
                                     for frame in frames)
    # When the window's honest occlusion bit or the app's frontmost state
    # changed: a background test window's pacing changes with both.
    transitions = []
    previous = None
    for frame in frames:
        state = (frame.get("visible", -1), frame.get("active", -1))
        if state != previous:
            transitions.append({"frame": frame["frame"],
                                "t_s": round(frame.get("tUs", 0) / 1e6, 3),
                                "visible": state[0], "active": state[1]})
            previous = state
    return {
        "available": True,
        "median_ms": {name: round(value / 1000, 2)
                      for name, value in medians.items()},
        "causes": dict(causes.most_common()),
        "stalls": stalls,
        "pace_late_ms": summary([frame.get("paceLateUs", 0) / 1000
                                 for frame in frames]),
        "off_cpu_ms_normal": summary([off_cpu(frame) / 1000
                                      for frame in normal]),
        "priorities": {str(key): value
                       for key, value in priorities.most_common()},
        "visible_frames": sum(1 for frame in frames
                              if frame.get("visible") == 1),
        "active_frames": sum(1 for frame in frames
                             if frame.get("active") == 1),
        "pageins": sum(frame["pageins"] for frame in frames),
        "window_transitions": transitions[:64],
        "window_transition_count": max(0, len(transitions) - 1),
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
    worst_pack = sorted(frames, key=lambda frame: -frame["packUs"])[:10]
    installs = parse_installs(output)
    latencies = [row["frames"] for row in installs if row.get("resident")]
    resident_keys = sorted({row["key"] for row in installs
                            if row.get("resident")})
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
        "worst_pack": [frame_record(frame, visits) for frame in worst_pack],
        "installs": {
            "rows": len(installs),
            "resident": len(latencies),
            "latency_frames_p50": percentile(latencies, 0.50),
            "latency_frames_p90": percentile(latencies, 0.90),
            "latency_frames_max": max(latencies) if latencies else 0,
            "resident_keys": resident_keys,
        },
        "override_uploads": sum(row.get("overrideUploads", 0)
                                for row in rows),
        "waits": wait_attribution(frames, visits),
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


# A macOS application's game thread runs at priority 46 (47 frontmost). A
# process macOS registered as BackgroundOnly runs at the default 31, and on a
# shared machine that alone produced most of the 35-75 ms waits T12a reported
# (T12c): coarser timer wakes, preempted event pumps and swaps. The engine
# makes a visible automation window an accessory app (46) for that reason;
# this refuses a run that did not get it.
APP_PRIORITY_MIN = 40


def scheduling_error(record: dict) -> str | None:
    """Evidence that the game thread ran at an application's priority."""
    priorities = (record.get("waits") or {}).get("priorities") or {}
    known = {int(key): count for key, count in priorities.items()
             if int(key) >= 0}
    if not known:
        return None  # the host does not report it (not macOS)
    typical = max(known, key=lambda key: known[key])
    if typical < APP_PRIORITY_MIN:
        return (f"game thread ran at priority {typical}, below an "
                f"application's {APP_PRIORITY_MIN}+: frame times are not a "
                f"player's (was the process registered BackgroundOnly?)")
    return None


def realtime_error(record: dict, output: str, ticks: int,
                   tick_hz: int) -> str | None:
    """Evidence, from the run itself, that it was paced in real time.

    The CLI flag only asks for it. Two independent witnesses: the process
    could not finish faster than its ticks take at the authored rate (the
    synthetic pacer runs several times faster), and the median displayed
    frame sits at the present period [PRESENT-MODE] reports (synthetic frames
    are a fraction of it).
    """
    rates = [int(value) for value in RATE_RE.findall(output)]
    rate = rates[-1] if rates and rates[-1] > 0 else tick_hz
    period_ms = 1000.0 / rate
    minimum_s = 0.9 * ticks / tick_hz
    record["pacing_evidence"] = {
        "present_rate": rate, "period_ms": round(period_ms, 2),
        "p50_ms": record["all"]["p50_ms"], "wall_s": record["wall_s"],
        "minimum_wall_s": round(minimum_s, 1),
    }
    if record["wall_s"] < minimum_s:
        return (f"not real time: {ticks} ticks took {record['wall_s']} s, "
                f"under the {minimum_s:.0f} s they take at {tick_hz} Hz")
    if not 0.85 * period_ms <= record["all"]["p50_ms"] <= 1.15 * period_ms:
        return (f"not real time: p50 {record['all']['p50_ms']} ms is not the "
                f"{period_ms:.2f} ms present period")
    return None


# ------------------------------------------------------------------ runs


# How long the pipe reader may take to reach end-of-file once the game has
# exited. Anything that still holds the game's output open past this (a
# helper the game left running) would otherwise hang the run forever.
READER_DRAIN_S = 30.0


class LogReaderError(RuntimeError):
    """The game exited but its output pipe never reached end-of-file."""


def run_game(command: list[str], cwd: Path, env: dict, log_path: Path,
             timeout: float, direct: bool,
             reader_timeout: float = READER_DRAIN_S) -> int | None:
    """Run the game with its output in log_path; None on timeout.

    The game prints a row per frame, and stderr is unbuffered: every row is
    a write() on the game thread. Pointed straight at a file, that write
    blocks whenever the file is busy -- T12c measured single rows taking
    31-155 ms on a loaded machine, which the row then reports as the frame's
    own stall. So by default the output goes through a pipe that a thread
    here drains into the file: the game's write is a memory copy, and a slow
    disk stalls this reader, not the frame being measured. --log-direct
    restores the file redirect for an A/B of exactly that.

    Raises LogReaderError when the pipe is still open reader_timeout seconds
    after the game ended: something else holds it, and the log may be cut.
    """
    with log_path.open("wb") as log:
        if direct:
            try:
                return subprocess.run(
                    command, cwd=cwd, env=env, stdout=log,
                    stderr=subprocess.STDOUT, timeout=timeout,
                    check=False).returncode
            except subprocess.TimeoutExpired:
                return None
        process = subprocess.Popen(
            command, cwd=cwd, env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)

        def drain() -> None:
            source = process.stdout.fileno()
            while True:
                chunk = os.read(source, 1 << 16)
                if not chunk:
                    return
                log.write(chunk)

        reader = threading.Thread(target=drain, daemon=True)
        reader.start()
        stuck = False
        try:
            try:
                returncode: int | None = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                returncode = None
        finally:
            # Completed, timed out, Ctrl-C or SIGTERM (SystemExit): the game
            # never outlives the run, as subprocess.run() guaranteed before.
            if process.poll() is None:
                process.kill()
                process.wait()
            reader.join(timeout=reader_timeout)
            stuck = reader.is_alive()
            if not stuck:
                process.stdout.close()
        if stuck:
            # The daemon reader stays parked on the open pipe; the file
            # holds everything read so far.
            raise LogReaderError(
                f"the game exited but its output pipe stayed open for "
                f"{reader_timeout:.0f} s (another process holds it); "
                f"{log_path} may be incomplete")
        return returncode


def run_arm(args, renderer: str, pack: Path | None, artifacts: Path) -> dict:
    label = f"{renderer}-{'pack' if pack else 'nopack'}"
    if args.loader and pack:
        label += f"-{args.loader}"
    if args.variant:
        label += f"-{args.variant}"
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
    if args.hidden:
        # The comparison arm for the frame-wait attribution: the automation
        # window genuinely hidden, as an ordinary --headless-ticks run is.
        del env["MDKR_TEST_VISIBLE_HEADLESS"]
        env["MDKR64_HIDDEN"] = "1"
    for assignment in args.env:
        key, _, value = assignment.partition("=")
        env[key] = value
    if not args.synthetic:
        env["MDKR_PACE_REALTIME"] = "1"
    if args.loader:
        env["MDKR_TEXTURE_PACK_LOADER"] = args.loader
    if args.state_hash:
        env["MDKR_STATE_HASH"] = "3"
        env["MDKR_STATE_HASH_FILE"] = str(run_root / "simhash.txt")
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
        log_error = None
        try:
            returncode = run_game(command, run_root, env, log_path, timeout,
                                  args.log_direct)
        except LogReaderError as exc:
            returncode = None
            log_error = str(exc)
        wall = time.monotonic() - started
        output = log_path.read_text(encoding="utf-8", errors="replace")
        record: dict = {
            "label": label, "renderer": renderer, "pack": pack is not None,
            "variant": args.variant, "env": list(args.env),
            "hidden": args.hidden, "log_direct": args.log_direct,
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
        if not load["quiet"]:
            # Surfaced, not just recorded: a busy machine's frame times are
            # not this build's. The data is still analysed (and shown, marked)
            # so the reader can see what the contention did.
            problems.append(
                f"machine still busy after --load-wait: load "
                f"{load['at_start'][0]:.2f} > {args.max_load:.2f}")
        busy_only = bool(problems)
        if log_error:
            problems.append(log_error)
        if returncode != 0:
            problems.append(f"exit {returncode}")
        if f"renderer backend: {renderer}" not in output:
            problems.append("requested backend was not active")
        for marker in FATAL_MARKERS:
            if marker in output:
                problems.append(f"fatal marker {marker}")
        active = [int(value) for value in ACTIVE_PACKS_RE.findall(output)]
        if pack is None and any(value != 0 for value in active):
            # Anchored on the count itself: "10 pack(s) active" contains
            # "0 pack(s) active" as a substring.
            problems.append("a pack was active in the no-pack arm")
        if FRAME_TAG not in output:
            problems.append("no [TEXPACK-FRAME] rows: instrument missing")
        if args.state_hash:
            hash_file = run_root / "simhash.txt"
            record["simhash_sha256"] = (sha256(hash_file)
                                        if hash_file.is_file() else None)
            record["simhash_rows"] = (
                sum(1 for _ in hash_file.open(encoding="utf-8",
                                              errors="replace"))
                if hash_file.is_file() else 0)
            if record["simhash_sha256"] is None:
                problems.append("no [SIMHASH] stream was written")
        complete = len(problems) == (1 if busy_only else 0)
        if complete:
            record.update(analyse(output))
            # Either kind of pack: a Rice pack prints how many identities it
            # used; a digest-keyed pack prints nothing of the sort, but every
            # replacement it supplied was uploaded as an override.
            if pack is not None and record["pack_used"] is None and \
                    record["override_uploads"] == 0:
                problems.append("the pack supplied no rendered texture")
            error = route_error(record)
            if error:
                problems.append(error)
            if not args.synthetic:
                error = realtime_error(record, output, ticks, tick_hz)
                if error:
                    problems.append(error)
                error = scheduling_error(record)
                if error:
                    problems.append(error)
        record["problems"] = problems
        record["busy"] = busy_only
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
    # The store's worker uses SDL threads; the bench resolves on its own
    # thread (INLINE loader), so it is built without them and without SDL.
    subprocess.run([compiler, "-O3", "-DNDEBUG", "-std=gnu11",
                    "-DMDKR_MOD_TEXTURE_NO_WORKER", *includes,
                    *map(str, sources), *objects, "-o", str(out)], check=True)
    return out


# --------------------------------------------------------------- report


def print_tables(records: list[dict], synthetic: bool) -> None:
    if synthetic:
        print("\nSYNTHETIC PACING: route check only. These are NOT player "
              "frame times.")
    for record in records:
        if record.get("problems"):
            print(f"\n{record['label']}: PROBLEM: "
                  f"{'; '.join(record['problems'])}")
    print("\n| arm | set | frames | p50 ms | p99 ms | max ms | >33.3 | >50 | "
          "pack work max ms | pack work p99 ms | frames pack >4 ms | loaded | "
          "decoded MiB | read ms | decode ms | worker ms | pump ms | "
          "upload ms |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
          "---|---|")
    for record in records:
        label = record["label"] + (" (BUSY)" if record.get("busy") else "")
        if "all" not in record:
            print(f"| {label} | FAILED: {'; '.join(record['problems'])} |")
            continue
        for key in ("cold", "warm", "all"):
            row = record[key]
            print(f"| {label} | {key} | {row['frames']} | "
                  f"{row['p50_ms']} | {row['p99_ms']} | {row['max_ms']} | "
                  f"{row['over_33_3']} | {row['over_50']} | "
                  f"{row['pack_max_ms']} | {row['pack_p99_ms']} | "
                  f"{row['pack_over_4ms']} | {row['loaded']} | "
                  f"{row['decoded_mib']} | {row['read_ms']} | "
                  f"{row['decode_ms']} | {row['worker_ms']} | "
                  f"{row['pump_ms']} | {row['upload_ms']} |")
    for record in records:
        if "all" not in record:
            continue
        installs = record.get("installs", {})
        if installs.get("rows"):
            print(f"\n{record['label']}: {installs['resident']} textures "
                  f"installed resident; request->resident latency in frames "
                  f"p50 {installs['latency_frames_p50']}, p90 "
                  f"{installs['latency_frames_p90']}, max "
                  f"{installs['latency_frames_max']}")
        for title, key in (("10 worst frames", "worst"),
                           ("10 frames with the most game-thread pack work",
                            "worst_pack")):
            print(f"\n{record['label']}: {title}")
            print("| frame ms | cpu ms | pack work ms | area | kind | "
                  "loaded/resolves | installed | refills | pending | "
                  "decoded MiB | open ms (absent) | read ms | decode ms | "
                  "pump ms | upload ms | hash ms | walk other ms | "
                  "outside render ms |")
            print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
                  "---|---|---|---|")
            for frame in record[key]:
                print(f"| {frame['frame_ms']} | {frame['cpu_ms']} | "
                      f"{frame['pack_ms']} | "
                      f"{frame['area']} | {frame['kind']} | "
                      f"{frame['loaded']}/{frame['resolves']} | "
                      f"{frame['installed']} | {frame['refills']} | "
                      f"{frame['pending']} | "
                      f"{frame['decoded_mib']} | {frame['open_ms']} "
                      f"({frame['absent_ms']}) | {frame['read_ms']} | "
                      f"{frame['decode_ms']} | {frame['pump_ms']} | "
                      f"{frame['upload_ms']} | "
                      f"{frame['hash_ms']} | {frame['walk_other_ms']} | "
                      f"{frame['outside_render_ms']} |")


def print_wait_tables(records: list[dict]) -> None:
    """Frame-wait attribution (T12c): what each >33.3 ms frame waited on."""
    for record in records:
        waits = record.get("waits") or {}
        if not waits.get("available"):
            continue
        frames = record["all"]["frames"]
        print(f"\n{record['label']}: frame waits -- "
              f"{len(waits['stalls'])} frames > {STALL_MS[0]} ms; causes "
              f"{waits['causes'] or '{}'}; pacer late p50/p99/max "
              f"{waits['pace_late_ms']['p50']}/{waits['pace_late_ms']['p99']}/"
              f"{waits['pace_late_ms']['max']} ms; off-CPU (normal frames) "
              f"p50/p99 {waits['off_cpu_ms_normal']['p50']}/"
              f"{waits['off_cpu_ms_normal']['p99']} ms; window visible "
              f"{waits['visible_frames']}/{frames}, app active "
              f"{waits['active_frames']}/{frames} "
              f"({waits['window_transition_count']} changes); priorities "
              f"{waits['priorities']}; page-ins {waits['pageins']}")
        if not waits["stalls"]:
            continue
        print("| t s | frame | area | frame ms | cpu ms | cause (excess ms) | "
              "pacer late ms | off-CPU ms | proc cpu ms | low-QoS ms | "
              "page-ins | prio | vis/act | parts >= 0.5 ms |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for stall in waits["stalls"]:
            parts = ", ".join(f"{name} {value}"
                              for name, value in stall["split_ms"].items())
            print(f"| {stall['t_s']} | {stall['frame']} | {stall['area']} | "
                  f"{stall['frame_ms']} | {stall['cpu_ms']} | "
                  f"{stall['cause']} ({stall['cause_excess_ms']}) | "
                  f"{stall['pace_late_ms']} | {stall['off_cpu_ms']} | "
                  f"{stall['proc_cpu_ms']} | {stall['low_qos_ms']} | "
                  f"{stall['pageins']} | {stall['prio']} | "
                  f"{stall['visible']}/{stall['active']} | {parts} |")


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
    parser.add_argument("--loader", choices=("worker", "budget", "inline"),
                        help="force the texture store's loader "
                             "(MDKR_TEXTURE_PACK_LOADER) for an A/B run")
    parser.add_argument("--state-hash", action="store_true",
                        help="record the [SIMHASH] v3 stream per run and "
                             "its SHA-256 (meaningful under --synthetic)")
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
    parser.add_argument("--repeat", type=int, default=1,
                        help="run every arm this many times")
    parser.add_argument("--variant", default="",
                        help="suffix for the arm labels of a variant run "
                             "(frame-wait attribution, T12c)")
    parser.add_argument("--env", action="append", default=[],
                        metavar="KEY=VALUE",
                        help="extra engine environment for a variant run; "
                             "repeatable")
    parser.add_argument("--log-direct", action="store_true",
                        help="redirect the game's output straight to the run "
                             "log file instead of through a pipe (A/B of the "
                             "logging's own stalls)")
    parser.add_argument("--hidden", action="store_true",
                        help="run the window hidden (MDKR64_HIDDEN=1) "
                             "instead of visible")
    parser.add_argument("--reanalyse", type=Path, metavar="JSON",
                        help="re-read the run logs an earlier --json names "
                             "and recompute its tables (no runs)")
    args = parser.parse_args()

    if args.reanalyse is not None:
        results = json.loads(args.reanalyse.read_text(encoding="utf-8"))
        for record in results.get("runs", []):
            if "all" not in record:
                continue
            output = Path(record["log"]).read_text(
                encoding="utf-8", errors="replace")
            record.update(analyse(output))
        print_tables(results.get("runs", []),
                     any(not run.get("realtime", True)
                         for run in results.get("runs", [])))
        print_wait_tables(results.get("runs", []))
        if args.json:
            args.json.write_text(json.dumps(results, indent=2))
            print(f"\nwrote {args.json}")
        return 0

    for assignment in args.env:
        if "=" not in assignment or not assignment.split("=", 1)[0]:
            parser.error(f"--env expects KEY=VALUE, got {assignment!r}")
    if args.texture_pack is None:
        # A no-pack run (the frame-wait attribution, T12c) needs no pack.
        if args.inventory or args.arms != "nopack":
            parser.error("--texture-pack is required unless --arms nopack")
        pack = None
        pack_sha = None
    else:
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
        "pack": ({"name": pack.name, "sha256": pack_sha,
                  "bytes": pack.stat().st_size} if pack is not None else None),
        "artifacts": str(artifacts),
        "host": {"cpus": os.cpu_count(), "platform": sys.platform},
    }
    if pack is not None:
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
        for _repeat in range(max(1, args.repeat)):
            for renderer in renderers:
                for arm_pack in arms:
                    records.append(
                        run_arm(args, renderer, arm_pack, artifacts))
                    if args.json:
                        results["runs"] = records
                        args.json.write_text(json.dumps(results, indent=2))
        results["runs"] = records
        print_tables(records, args.synthetic)
        print_wait_tables(records)

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
