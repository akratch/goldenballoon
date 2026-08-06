#!/usr/bin/env python3
"""End-to-end coverage for DKR's four challenge/battle courses.

The four production routes are:

    11 Fire Mountain       eggs (RACETYPE_CHALLENGE_EGGS)
    25 Smokey Castle       treasure bananas (RACETYPE_CHALLENGE_BANANAS)
    26 Darkwater Beach     battle (RACETYPE_CHALLENGE_BATTLE)
    27 Icicle Pyramid      battle (RACETYPE_CHALLENGE_BATTLE)

Each win/loss arm resumes a checksum-valid Adventure save, loads the actual ROM
course and its real race type, runs one human plus three independent AI racers
through the renderer, and feeds terminal gameplay events through the production
mode predicates. The native driver cannot assign raceFinished, finishing order,
victory, progression, a post-race state, or save data.

The gate then requires production to:

* evolve egg hatches, treasure deposits, or battle health to the real threshold;
* assign a complete 1..4 result with the requested human win/loss;
* enter the normal result/retry path on a loss or TT-amulet sequence on a first
  Adventure win, tearing down the completed race in either case;
* persist exactly RACE_VISITED on loss, and RACE_VISITED|RACE_CLEARED plus one
  TT-amulet piece on win, with a valid EEPROM checksum.

Three positive controls suppress only the final eggs/bananas/battle predicate.
Scores still reach their thresholds, but no verdict, progression, result flow,
or save reward may appear. That proves the production gates—not the driver—are
what end the races.

Production has no tie or timeout verdict for these modes. Tied unfinished scores
are deterministically ordered by racer index in race_check_finish(); a challenge
ends only at the mode predicate. The normal arms exercise that tie-break for all
remaining racers rather than inventing unsupported outcomes.

Fire Mountain and Smokey Castle additionally carry a portrait pixel witness on
both backends: the arena is run twice, identically except for
MDKR_SUPPRESS_PORTRAITS, and the framebuffers must move apart while the gameplay
traces stay identical. Nothing else here can see a portrait that binds a racer,
builds geometry, submits a draw and paints nothing -- which is exactly what the
giant character portraits did for a whole release.
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from check_adventure_two import eeprom_image
from check_race_drive import scene_metrics
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, slot_checksum


ROOT = Path(__file__).resolve().parent.parent
BASE_SCRIPT = ROOT / "tests/input_scripts/adventure_resume_race.txt"
FRAMES = 5000
CONTROL_FRAMES = 3300
RELOAD_FRAMES = 2200
RACE_CLEARED = 2

COURSES = {
    11: ("eggs", 0x42, 3),
    25: ("bananas", 0x41, 10),
    26: ("battle", 0x40, 8),
    27: ("battle", 0x40, 8),
}
CONTROLS = ((11, "eggs"), (25, "bananas"), (26, "battle"))

ASSET_LEVEL_HEADERS_TABLE = 22
ASSET_LEVEL_HEADERS = 23
LEVELHEADER_RACE_TYPE = 0x4C
ROM_LAYOUTS = {
    (0xE402430D, 0xD2FCFC9D): (0x000ED0E0, 0x000ED1B0),
    (0x596E145B, 0xF7D9879F): (0x000ED170, 0x000ED240),
}

# One row per giant character portrait (BHV_CHARACTER_FLAG), emitted exactly
# once by obj_loop_characterflag() at the moment its lazy geometry build binds
# a racer. Fire Mountain and Smokey Castle both author one per player.
PORTRAIT_RE = re.compile(
    r"charflag_bound: playerID=(-?\d+) characterID=(-?\d+) texture=(\w+)"
)
PORTRAIT_INIT_RE = re.compile(
    r"charflag_init: authored=(-?\d+) playerID=(-?\d+)"
)
EXPECTED_PORTRAITS = 4
# Only the eggs and treasure arenas author the giant portraits -- and those are
# exactly the two courses the player report named (Fire Mountain and Smokey
# Castle). The battle arenas author none, so ZERO is the correct expectation
# there and is pinned rather than skipped: it is the thing that would have to
# change for this gate's scoping to become wrong.
PORTRAIT_COURSES = {11, 25}

# Paired pixel witness for the same portraits. Binding traces prove the model
# was built and bound; they cannot see whether the draw painted anything, and
# for a whole release it did not -- the run-time-built quad's triangle indices
# and UVs were packed as N64 words into host-order union fields, so every
# portrait submitted a backface-culled triangle over a vertex slot its batch
# never loaded and emitted ZERO pixels while every trace and every scene metric
# stayed green. The gate therefore runs the arena twice, identically except for
# MDKR_SUPPRESS_PORTRAITS, and requires the two framebuffers to DIFFER: only a
# portrait that actually paints can move them apart. The same pairing requires
# the gameplay traces to stay identical, which proves the difference is the
# portrait draw and not a run that diverged.
PORTRAIT_WITNESS_FRAMES = 2300
PORTRAIT_DUMP_FROM = 2100
PORTRAIT_DUMP_EVERY = 100
# Measured floors: the smallest per-frame portrait footprint over both arenas
# at these two frames is 4139 px (course 25 @2100) and the largest is 20112 px
# (course 11 @2200); a portrait-shaped quad is never smaller than a few
# thousand pixels at this resolution. The floors sit well under the measured
# minimum and infinitely above the zero a silent portrait produces.
PORTRAIT_MIN_PIXELS_PER_FRAME = 1500
PORTRAIT_MIN_PIXELS_TOTAL = 5000
# Both production backends, because the defect that hid the portraits lived in
# the geometry the display list carried, which every backend consumes.
PORTRAIT_BACKENDS = ("gl", "webgpu")

STATE_RE = re.compile(
    r"\[CHALLENGE\] phase=(\w+) course=(\d+) type=(\d+) tracks=(\d+) "
    r"racers=(\d+) flags=0x([0-9a-fA-F]+) tt=(\d+) tick=(\d+) (.*)"
)
RACER_RE = re.compile(
    r"r([0-3])=(-?\d+),(-?\d+),"
    r"([^, ]+),([^, ]+),([^, ]+),"
    r"(-?\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+)"
)
LEVEL_RE = re.compile(
    r"level_load: levelId=(\d+) numPlayers=(-?\d+).*@frame~(\d+)"
)
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)


def normalize_rom(data: bytes) -> bytes:
    if data[:4] == b"\x80\x37\x12\x40":
        return data
    out = bytearray(len(data))
    if data[:4] == b"\x37\x80\x40\x12":
        out[0::2], out[1::2] = data[1::2], data[0::2]
    elif data[:4] == b"\x40\x12\x37\x80":
        out[0::4] = data[3::4]
        out[1::4] = data[2::4]
        out[2::4] = data[1::4]
        out[3::4] = data[0::4]
    else:
        raise ValueError(f"unsupported ROM byte order {data[:4].hex()}")
    return bytes(out)


def rom_section(data: bytes, index: int, lut: int, base: int) -> bytes:
    start = struct.unpack_from(">I", data, lut + 4 * (index + 1))[0]
    end = struct.unpack_from(">I", data, lut + 4 * (index + 2))[0]
    return data[base + start:base + end]


def read_bits(data: bytes, offset: int, width: int) -> int:
    value = 0
    for bit in range(offset, offset + width):
        value = (value << 1) | (
            (data[bit // 8] >> (7 - (bit % 8))) & 1
        )
    return value


def save_layout(rom_path: Path) -> tuple[list[int], dict[int, int]]:
    data = normalize_rom(rom_path.read_bytes())
    crc = struct.unpack_from(">II", data, 0x10)
    if crc not in ROM_LAYOUTS:
        raise ValueError(
            f"unsupported ROM CRC {crc[0]:08x}/{crc[1]:08x}"
        )
    lut, base = ROM_LAYOUTS[crc]
    table_raw = rom_section(data, ASSET_LEVEL_HEADERS_TABLE, lut, base)
    table = list(struct.unpack(f">{len(table_raw) // 4}i", table_raw))
    table_end = table.index(-1) if -1 in table else len(table)
    headers = rom_section(data, ASSET_LEVEL_HEADERS, lut, base)
    race_types = {
        level: headers[table[level] + LEVELHEADER_RACE_TYPE]
        for level in range(table_end - 1)
    }
    eligible = [
        level for level, race_type in race_types.items()
        if race_type == 0 or (race_type & 0x40) or race_type == 8
    ]
    return eligible, race_types


def decode_save(
    payload: bytes, course: int, eligible: list[int],
) -> dict[str, int | bool]:
    if len(payload) < 40:
        return {"checksum_ok": False, "status": -1, "tt": -1}
    slot = payload[:40]
    checksum = int.from_bytes(slot[:2], "big")
    expected = slot_checksum(slot)
    ordinal = eligible.index(course)
    return {
        "checksum_ok": checksum == expected,
        "status": read_bits(slot, 16 + ordinal * 2, 2),
        "tt": read_bits(slot, 154, 3),
    }


def parse_states(output: str, course: int) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for match in STATE_RE.finditer(output):
        if int(match.group(2)) != course:
            continue
        racers: list[dict[str, int | float]] = []
        for racer_match in RACER_RE.finditer(match.group(9)):
            racers.append({
                "slot": int(racer_match.group(1)),
                "player": int(racer_match.group(2)),
                "index": int(racer_match.group(3)),
                "x": float(racer_match.group(4)),
                "y": float(racer_match.group(5)),
                "z": float(racer_match.group(6)),
                "score": int(racer_match.group(7)),
                "health": int(racer_match.group(8)),
                "eggs": int(racer_match.group(9)),
                "finished": int(racer_match.group(10)),
                "place": int(racer_match.group(11)),
            })
        rows.append({
            "phase": match.group(1),
            "type": int(match.group(3)),
            "tracks": int(match.group(4)),
            "count": int(match.group(5)),
            "flags": int(match.group(6), 16),
            "tt": int(match.group(7)),
            "tick": int(match.group(8)),
            "racers": racers,
        })
    return rows


def make_script(path: Path) -> None:
    taps = "".join(f"{frame} A 4\n" for frame in range(2800, 4801, 24))
    path.write_text(BASE_SCRIPT.read_text() + "\n# Challenge result flow\n" + taps)


def run_arm(
    binary: Path,
    rom: Path,
    course: int,
    outcome: str,
    *,
    break_gate: str | None = None,
    dump_frame: bool = False,
) -> dict[str, object]:
    frames = CONTROL_FRAMES if break_gate else FRAMES
    with tempfile.TemporaryDirectory(prefix="mdkr_challenge_") as temp:
        run_dir = Path(temp)
        (run_dir / "save").mkdir()
        (run_dir / "save/eeprom.bin").write_bytes(eeprom_image(False))
        script = run_dir / "challenge.txt"
        make_script(script)
        captures = run_dir / "frames"
        if dump_frame:
            captures.mkdir()

        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith("MDKR_")
        }
        env.update(
            MDKR_AUDIO="0",
            MDKR_TRACE="1",
            MDKR_AUTOPILOT="1",
            MDKR_LOAD_TRACK=str(course),
            MDKR_CHALLENGE_OUTCOME=outcome,
            MDKR_RENDER_SCALE="1",
        )
        if break_gate:
            env["MDKR_CHALLENGE_BREAK_GATE"] = break_gate
        command = [
            str(binary),
            "--headless-frames", str(frames),
            "--input-script", str(script),
        ]
        if dump_frame:
            env.update(MDKR_DUMP_FROM="2280", MDKR_DUMP_EVERY="99999")
            command.extend(("--dump-frames", str(captures)))
        command.extend(("--rom", str(rom)))
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=300,
            check=False,
        )
        save = (run_dir / "save/eeprom.bin").read_bytes()
        reload_rc: int | None = None
        reload_out = ""
        if break_gate is None:
            # Start a second process from the persisted image. Stop after the
            # course load but before the 120-tick event warmup, so this witnesses
            # decoding only and cannot manufacture another result.
            reload_env = {
                key: value for key, value in os.environ.items()
                if not key.startswith("MDKR_")
            }
            reload_env.update(
                MDKR_AUDIO="0",
                MDKR_TRACE="1",
                MDKR_AUTOPILOT="1",
                MDKR_LOAD_TRACK=str(course),
                MDKR_CHALLENGE_OUTCOME="win",
                MDKR_RENDER_SCALE="1",
            )
            reload_proc = subprocess.run(
                [
                    str(binary),
                    "--headless-frames", str(RELOAD_FRAMES),
                    "--input-script", str(script),
                    "--rom", str(rom),
                ],
                cwd=run_dir,
                env=reload_env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=300,
                check=False,
            )
            reload_rc = reload_proc.returncode
            reload_out = reload_proc.stdout
        metrics = []
        for frame in captures.glob("frame_*.ppm"):
            metrics.append((frame.name, *scene_metrics(str(frame))))
        return {
            "rc": proc.returncode,
            "out": proc.stdout,
            "save": save,
            "metrics": metrics,
            "dump_requested": dump_frame,
            "reload_rc": reload_rc,
            "reload_out": reload_out,
        }


def validate_portraits(output: str, course: int,
                       failures: list[str]) -> None:
    """The giant character portraits are really bound to real racers.

    These are the only assertions in this gate that can see them. The scene
    colour/flatness metrics stay comfortably green with every portrait absent,
    because the portraits are small quads against a large lit arena -- which is
    exactly how a silent portrait regression survives a full pass.

    The binding is lazy and one-shot: obj_loop_characterflag() runs
    get_racer_object(playerID) every tick until it returns non-NULL, latches
    characterID >= 0, and never re-enters. So a portrait that never binds emits
    NO row at all, and a portrait bound to the wrong racer emits a row with the
    wrong playerID. Requiring one row per player, each with a distinct
    0-based playerID, an in-range characterID and a resolved texture, is a
    positive witness for both failure modes.
    """
    spawns = PORTRAIT_INIT_RE.findall(output)
    if course not in PORTRAIT_COURSES:
        if spawns:
            failures.append(
                f"course {course}: {len(spawns)} character portraits spawned "
                "on a battle arena that authors none")
        return
    if not spawns or len(spawns) % EXPECTED_PORTRAITS != 0:
        failures.append(
            f"course {course}: {len(spawns)} character portraits spawned, "
            f"expected a nonzero multiple of {EXPECTED_PORTRAITS}")
        return
    for authored, stored in spawns:
        if int(stored) != int(authored):
            failures.append(
                f"course {course}: portrait entry playerIndex {authored} was "
                f"stored as playerID {stored}; the portrait will show a "
                "different racer than the level authored")
            break
    authored_set = sorted(int(a) for a, _ in spawns[:EXPECTED_PORTRAITS])
    if authored_set != list(range(EXPECTED_PORTRAITS)):
        failures.append(
            f"course {course}: authored portrait player indices {authored_set} "
            f"are not the 0-based 0..{EXPECTED_PORTRAITS - 1} set the loop "
            "func indexes gRacers with")
    rows = PORTRAIT_RE.findall(output)
    # A retried arm loads the course more than once and re-binds a fresh set,
    # so assert whole sets rather than one fixed total.
    if not rows or len(rows) % EXPECTED_PORTRAITS != 0:
        failures.append(
            f"course {course}: {len(rows)} character portraits bound, "
            f"expected a nonzero multiple of {EXPECTED_PORTRAITS}")
        return
    for start in range(0, len(rows), EXPECTED_PORTRAITS):
        group = rows[start:start + EXPECTED_PORTRAITS]
        players = sorted(int(player) for player, _, _ in group)
        if players != list(range(EXPECTED_PORTRAITS)):
            failures.append(
                f"course {course}: portrait set {start // EXPECTED_PORTRAITS} "
                f"did not bind one distinct racer each (playerIDs {players})")
    for player, character, texture in rows:
        if not 0 <= int(character) < 20:
            failures.append(
                f"course {course}: portrait for player {player} bound an "
                f"out-of-range characterID {character}")
        if texture != "ok":
            failures.append(
                f"course {course}: portrait for player {player} resolved no "
                "texture")


def raster(path: Path) -> tuple[int, int, bytes]:
    """Decode a binary P6 PPM into (width, height, pixels)."""
    data = path.read_bytes()
    fields: list[bytes] = []
    index = 0
    while len(fields) < 4:
        while data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b"#":
            index = data.index(b"\n", index) + 1
            continue
        end = index
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[index:end])
        index = end
    index += 1
    width, height = int(fields[1]), int(fields[2])
    return width, height, data[index:index + width * height * 3]


def raster_difference(left: Path, right: Path) -> int:
    """Count pixels whose RGB triples differ between two same-size rasters."""
    lw, lh, lp = raster(left)
    rw, rh, rp = raster(right)
    if (lw, lh) != (rw, rh) or len(lp) != len(rp):
        raise ValueError(
            f"raster geometry {lw}x{lh} vs {rw}x{rh} does not match"
        )
    return sum(
        1 for offset in range(0, len(lp), 3)
        if lp[offset:offset + 3] != rp[offset:offset + 3]
    )


def portrait_witness_arm(
    binary: Path, rom: Path, course: int, work: Path, backend: str,
    suppress: bool,
) -> tuple[str, Path]:
    """One short arena run, optionally with the portrait draw suppressed."""
    run_dir = work / backend / ("suppressed" if suppress else "drawn")
    (run_dir / "save").mkdir(parents=True)
    (run_dir / "save/eeprom.bin").write_bytes(eeprom_image(False))
    script = run_dir / "challenge.txt"
    make_script(script)
    captures = run_dir / "frames"
    captures.mkdir()
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith("MDKR_")
    }
    env.update(
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=str(course),
        MDKR_CHALLENGE_OUTCOME="win",
        MDKR_RENDER_SCALE="1",
        MDKR_RENDERER=backend,
        MDKR_DUMP_FROM=str(PORTRAIT_DUMP_FROM),
        MDKR_DUMP_EVERY=str(PORTRAIT_DUMP_EVERY),
    )
    if suppress:
        env["MDKR_SUPPRESS_PORTRAITS"] = "1"
    proc = subprocess.run(
        [
            str(binary),
            "--headless-frames", str(PORTRAIT_WITNESS_FRAMES),
            "--input-script", str(script),
            "--dump-frames", str(captures),
            "--rom", str(rom),
        ],
        cwd=run_dir,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=300,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"portrait witness {backend} arm exited {proc.returncode}"
        )
    return proc.stdout, captures


def validate_portrait_pixels(
    binary: Path, rom: Path, course: int, failures: list[str],
) -> None:
    """The portraits actually paint.

    Everything else in this gate is satisfied by a portrait that binds a racer,
    builds geometry and submits a draw that emits nothing. This pairs a normal
    arena run with one whose portrait draw is suppressed and requires the two
    framebuffers to move apart at every sampled frame.
    """
    with tempfile.TemporaryDirectory(prefix="mdkr_portrait_") as temp:
        work = Path(temp)
        for backend in PORTRAIT_BACKENDS:
            try:
                drawn_out, drawn = portrait_witness_arm(
                    binary, rom, course, work, backend, False)
                hidden_out, hidden = portrait_witness_arm(
                    binary, rom, course, work, backend, True)
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                failures.append(f"course {course}: portrait witness {error}")
                continue
            for output, label in (
                (drawn_out, "drawn"), (hidden_out, "hidden"),
            ):
                bad = BAD_RE.search(output)
                if bad:
                    failures.append(
                        f"course {course} {backend}: portrait witness {label} "
                        f"arm hit {bad.group(0)!r}")
                    return
            # Suppression must be presentation-only. If the two arms diverged
            # in gameplay, a framebuffer difference would prove nothing about
            # the portrait draw.
            drawn_rows = parse_states(drawn_out, course)
            hidden_rows = parse_states(hidden_out, course)
            if not drawn_rows or drawn_rows != hidden_rows:
                failures.append(
                    f"course {course} {backend}: suppressing the portrait "
                    f"draw changed the gameplay trace ({len(drawn_rows)} vs "
                    f"{len(hidden_rows)} states); the pixel witness is not "
                    "isolated to presentation")
                continue
            frames = sorted(path.name for path in drawn.glob("frame_*.ppm"))
            if not frames:
                failures.append(
                    f"course {course} {backend}: portrait witness produced "
                    "no frames")
                continue
            total = 0
            for name in frames:
                hidden_frame = hidden / name
                if not hidden_frame.exists():
                    failures.append(
                        f"course {course} {backend}: portrait witness "
                        f"suppressed arm is missing {name}")
                    break
                try:
                    changed = raster_difference(drawn / name, hidden_frame)
                except ValueError as error:
                    failures.append(f"course {course} {backend}: {error}")
                    break
                total += changed
                if changed < PORTRAIT_MIN_PIXELS_PER_FRAME:
                    failures.append(
                        f"course {course} {backend}: {name} changed only "
                        f"{changed} pixels when the portrait draw was "
                        f"suppressed, want >= {PORTRAIT_MIN_PIXELS_PER_FRAME}"
                        " -- the portraits are submitting a draw that paints "
                        "nothing")
            if total < PORTRAIT_MIN_PIXELS_TOTAL:
                failures.append(
                    f"course {course} {backend}: portraits painted {total} "
                    f"pixels across {len(frames)} frames, want >= "
                    f"{PORTRAIT_MIN_PIXELS_TOTAL}")


def human(row: dict[str, object]) -> dict[str, int | float] | None:
    return next(
        (r for r in row["racers"] if r["player"] == 0),  # type: ignore[index]
        None,
    )


def validate_common(
    result: dict[str, object],
    course: int,
    race_type: int,
    failures: list[str],
) -> list[dict[str, object]]:
    output = str(result["out"])
    if result["rc"] != 0:
        failures.append(f"exit code {result['rc']}")
    bad = BAD_RE.search(output)
    if bad:
        failures.append(f"runtime marker {bad.group(0)!r}")
    rows = parse_states(output, course)
    if not rows:
        failures.append("no [CHALLENGE] states")
        return rows
    load = next((row for row in rows if row["phase"] == "load"), None)
    if load is None:
        failures.append("no load state")
        return rows
    if load["type"] != race_type:
        failures.append(
            f"ROM race type {load['type']}, expected {race_type}"
        )
    if load["tracks"] != 0:
        failures.append("fixture did not run in Adventure mode")
    validate_portraits(output, course, failures)
    if load["count"] != 4 or len(load["racers"]) != 4:
        failures.append(
            f"spawned/probed {load['count']}/{len(load['racers'])} racers, want 4"
        )
        return rows

    racers = load["racers"]
    players = [r["player"] for r in racers]  # type: ignore[index]
    indices = [r["index"] for r in racers]  # type: ignore[index]
    if players.count(0) != 1 or players.count(-1) != 3:
        failures.append(f"player mapping {players}, want one human + three AI")
    if sorted(indices) != [0, 1, 2, 3]:
        failures.append(f"racer indices {indices}, want 0..3")
    for racer in racers:  # type: ignore[assignment]
        if not all(
            math.isfinite(float(racer[axis])) for axis in ("x", "y", "z")
        ):
            failures.append(
                f"racer {racer['index']} has non-finite position"
            )

    moving = [row for row in rows if row["phase"] in ("state", "event")]
    if moving:
        late = max(moving, key=lambda row: int(row["tick"]))
        for before in racers:  # type: ignore[assignment]
            after = next(
                (r for r in late["racers"] if r["index"] == before["index"]),  # type: ignore[index]
                None,
            )
            if after is None:
                failures.append(f"racer {before['index']} disappeared")
                continue
            distance = math.dist(
                (float(before["x"]), float(before["y"]), float(before["z"])),
                (float(after["x"]), float(after["y"]), float(after["z"])),
            )
            if distance < 5.0:
                failures.append(
                    f"racer {before['index']} moved only {distance:.2f} units"
                )
    else:
        failures.append("no moving state/event samples")

    metrics = result["metrics"]
    if result["dump_requested"] and not metrics:
        failures.append("requested rendered-frame capture produced no frame")
    if metrics:
        for name, colours, sigma in metrics:  # type: ignore[misc]
            if colours < 200 or sigma < 12.0:
                failures.append(
                    f"{name} render is flat ({colours} colours, sigma {sigma:.1f})"
                )
    return rows


def validate_normal(
    result: dict[str, object],
    course: int,
    outcome: str,
    eligible: list[int],
    failures: list[str],
) -> None:
    mode, race_type, threshold = COURSES[course]
    rows = validate_common(result, course, race_type, failures)
    if not rows:
        return
    events = [row for row in rows if row["phase"] == "event"]
    verdicts = [row for row in rows if row["phase"] == "verdict"]
    if len(events) < threshold:
        failures.append(f"only {len(events)} terminal events, want >= {threshold}")
    if len(verdicts) != 1:
        failures.append(f"{len(verdicts)} verdict states, want exactly one")
        return

    verdict = verdicts[0]
    human_racer = human(verdict)
    if human_racer is None:
        failures.append("verdict has no controller-one racer")
        return
    places = sorted(int(r["place"]) for r in verdict["racers"])  # type: ignore[index]
    if places != [1, 2, 3, 4]:
        failures.append(f"finish places {places}, want permutation 1..4")
    if any(int(r["finished"]) != 1 for r in verdict["racers"]):  # type: ignore[index]
        failures.append("not every racer was finalized by production")
    won = int(human_racer["place"]) == 1
    if won != (outcome == "win"):
        failures.append(
            f"human place {human_racer['place']} contradicts requested {outcome}"
        )

    if mode == "battle":
        if outcome == "win":
            ai_health = sorted(
                int(r["health"]) for r in verdict["racers"]  # type: ignore[index]
                if int(r["player"]) == -1
            )
            if int(human_racer["health"]) != 8 or ai_health != [0, 0, 0]:
                failures.append(
                    f"battle win health human/AI={human_racer['health']}/{ai_health}"
                )
        elif int(human_racer["health"]) != 0:
            failures.append(
                f"battle loss human health={human_racer['health']}, want 0"
            )
    else:
        terminal = human_racer if outcome == "win" else next(
            (r for r in verdict["racers"] if int(r["place"]) == 1),  # type: ignore[index]
            None,
        )
        if terminal is None or int(terminal["score"]) < threshold:
            failures.append(
                f"{mode} terminal score did not reach {threshold}"
            )

    loads = [
        (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        for m in LEVEL_RE.finditer(str(result["out"]))
    ]
    target_loads = [entry for entry in loads if entry[0] == course and entry[1] == 0]
    if outcome == "loss":
        if not any(row["phase"] == "postrace" for row in rows):
            failures.append("loss never entered production post-race results")
        if len(target_loads) < 2:
            failures.append("loss results did not tear down and retry the course")
    else:
        if not any(row["phase"] == "adventure" for row in rows):
            failures.append("win never entered the Adventure reward transition")
        if not any(level == 44 for level, _players, _frame in loads):
            failures.append("win never loaded the TT-amulet result sequence (44)")

    expected_flags = 3 if outcome == "win" else 1
    # The two-bit EEPROM course status is an ordinal, not the in-memory mask:
    # 0 unvisited, 1 visited, 2 cleared, 3 silver-coins-cleared.
    expected_status = 2 if outcome == "win" else 1
    expected_tt = 1 if outcome == "win" else 0
    if int(verdict["flags"]) != expected_flags:
        failures.append(
            f"in-memory course flags 0x{int(verdict['flags']):x}, "
            f"want 0x{expected_flags:x}"
        )
    if int(verdict["tt"]) != expected_tt:
        failures.append(
            f"in-memory TT amulet {verdict['tt']}, want {expected_tt}"
        )
    decoded = decode_save(result["save"], course, eligible)  # type: ignore[arg-type]
    if not decoded["checksum_ok"]:
        failures.append("persisted save checksum is invalid")
    if decoded["status"] != expected_status:
        failures.append(
            f"persisted course status {decoded['status']}, want {expected_status}"
        )
    if decoded["tt"] != expected_tt:
        failures.append(
            f"persisted TT amulet {decoded['tt']}, want {expected_tt}"
        )

    if result["reload_rc"] != 0:
        failures.append(f"save reload exited {result['reload_rc']}")
    reload_bad = BAD_RE.search(str(result["reload_out"]))
    if reload_bad:
        failures.append(f"save reload marker {reload_bad.group(0)!r}")
    reload_rows = parse_states(str(result["reload_out"]), course)
    reload_load = next(
        (row for row in reload_rows if row["phase"] == "load"), None,
    )
    if reload_load is None:
        failures.append("second process did not re-load the challenge save")
    else:
        if int(reload_load["flags"]) != expected_flags:
            failures.append(
                f"reloaded course flags 0x{int(reload_load['flags']):x}, "
                f"want 0x{expected_flags:x}"
            )
        if int(reload_load["tt"]) != expected_tt:
            failures.append(
                f"reloaded TT amulet {reload_load['tt']}, want {expected_tt}"
            )
        if any(row["phase"] == "event" for row in reload_rows):
            failures.append("reload witness ran long enough to inject an event")


def validate_control(
    result: dict[str, object],
    course: int,
    gate: str,
    eligible: list[int],
    failures: list[str],
) -> None:
    _mode, race_type, threshold = COURSES[course]
    rows = validate_common(result, course, race_type, failures)
    events = [row for row in rows if row["phase"] == "event"]
    if len(events) < threshold:
        failures.append(
            f"positive control {gate} delivered {len(events)} events, "
            f"want >= {threshold}"
        )
    if any(
        row["phase"] in ("verdict", "postrace", "adventure") for row in rows
    ):
        failures.append(
            f"positive control {gate} reached a result with terminal gate disabled"
        )
    if events:
        last = events[-1]
        if int(last["flags"]) != 1 or int(last["tt"]) != 0:
            failures.append(
                f"{gate} control changed in-memory flags/tt to "
                f"0x{int(last['flags']):x}/{last['tt']}, want 0x1/0"
            )
        if gate == "battle":
            h = human(last)
            if h is None or int(h["health"]) != 0:
                failures.append("battle control did not evolve human health to zero")
        elif max(int(r["score"]) for r in last["racers"]) < threshold:  # type: ignore[index]
            failures.append(
                f"{gate} control score did not evolve to {threshold}"
            )
    decoded = decode_save(result["save"], course, eligible)  # type: ignore[arg-type]
    if not decoded["checksum_ok"]:
        failures.append(f"{gate} control save checksum is invalid")
    # No verdict means no teardown/save request. The original valid slot must
    # remain byte-canonically unvisited even though the live race marked its
    # in-memory course as visited at setup.
    if decoded["status"] != 0 or decoded["tt"] != 0:
        failures.append(
            f"{gate} disabled gate persisted status/tt "
            f"{decoded['status']}/{decoded['tt']}, want 0/0"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--quick", action="store_true",
                        help="run one win/loss course and one positive control")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    for path in (binary, rom, BASE_SCRIPT):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    eligible, rom_types = save_layout(rom)
    failures: list[str] = []
    for course, (_mode, expected_type, _threshold) in COURSES.items():
        if rom_types.get(course) != expected_type:
            failures.append(
                f"ROM course {course} type={rom_types.get(course)}, "
                f"want {expected_type}"
            )
        if course not in eligible:
            failures.append(f"ROM course {course} is absent from save ordering")
    if failures:
        for failure in failures:
            print(f"  - {failure}")
        return 1

    courses = [11] if args.quick else list(COURSES)
    for course in courses:
        for outcome in ("win", "loss"):
            if args.verbose:
                print(f"  course {course:2d} {outcome}")
            result = run_arm(
                binary, rom, course, outcome,
                dump_frame=outcome == "win",
            )
            arm_failures: list[str] = []
            validate_normal(
                result, course, outcome, eligible, arm_failures,
            )
            failures.extend(
                f"course {course} {outcome}: {failure}"
                for failure in arm_failures
            )
            if args.verbose and result["metrics"]:
                print(f"    render {result['metrics']}")

    for course in [c for c in courses if c in PORTRAIT_COURSES]:
        if args.verbose:
            print(f"  portrait pixel witness (course {course})")
        validate_portrait_pixels(binary, rom, course, failures)

    controls = CONTROLS[:1] if args.quick else CONTROLS
    for course, gate in controls:
        if args.verbose:
            print(f"  positive control {gate} (course {course})")
        result = run_arm(
            binary, rom, course, "loss", break_gate=gate,
        )
        arm_failures = []
        validate_control(result, course, gate, eligible, arm_failures)
        failures.extend(
            f"positive control {gate}: {failure}"
            for failure in arm_failures
        )

    if failures:
        print("check_challenge_modes: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    arm_count = len(courses) * 2
    witnesses = len([c for c in courses if c in PORTRAIT_COURSES])
    print(
        f"check_challenge_modes: PASS "
        f"({arm_count} win/loss arms, {len(controls)} positive controls, "
        f"{witnesses} portrait pixel witnesses)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
