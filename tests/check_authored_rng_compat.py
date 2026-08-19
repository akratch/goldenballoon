#!/usr/bin/env python3
"""Freeze the legacy all-racer state/RNG stream for the authored 30 Hz route.

This is a raw compatibility oracle, not a statistical gameplay gate. It
protects every emitted racer row and the shared authored RNG value against the
accepted vehicle-audio/Taj integration baseline. The prior hash predated the
ordinary-car audio dispatch and therefore froze a port bug: the retail ROM's
``racer_sound_car`` consumes the shared RNG stream. That ownership was proved
directly with the pinned ares PC/return-address witness documented in
tests/README.md before this oracle was rebaselined. The fixture is encoded as a
row count, exact schema, and SHA-256 so no private ROM-derived log is shipped.
The shared route compiler advances native script entries by one fixed ticket,
matching the host input boundary's N-to-N+1 publication contract; this keeps
the exact accepted stream stable instead of rebaselining around test timing.

REBASELINE 2026-08-05 (d74efe02 -> 53c8ca2c). The superseded hash froze a
second port defect, in the same shape as the first: 670c984 corrected the
8th-place racing-line selector in func_80042D20(). racePosition is 1-indexed
(1..8) and D_800DCDA0 holds eight entries, so 8th place reads index 8, one past
the end. The port had clamped that to D_800DCDA0[7] == 2, a value the retail
game never produces; on hardware the read lands in the adjacent table and
yields D_800DCDA8[0] == 1.

That is not an argument from the disassembly's symbol names -- it is checked
against the shipped ROM on every run by check_racing_line_overrun_witness()
below, which locates the two tables by content and asserts their adjacency and
the overrun byte. The pin moved only after the ROM said which side was right,
and only after a build of this tree carrying nothing but the pre-670c984 clamp
reproduced d74efe02 exactly -- proving that single selector, and nothing else
in the 52 commits since the old pin (the bounded/extent-carrying inflate path,
the game-text terminator count, the texture-header handling, or the camera
obstruction port), accounts for the difference.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
ROUTE_TOOL = ROOT / "tools" / "dkr_oracle_route.py"
FRAMES = 4800
REFERENCE_COMMIT = "670c984837ce21a9bd5ff54f0a2d4339267fb872"
EXPECTED_ROWS = 27_840
EXPECTED_SHA256 = "53c8ca2c1e67c03c3e59157c088e7eae5a2a537ffa79b298661245eead102d30"
# Superseded by the racing-line rebaseline documented above. Kept named so a
# bisect that lands on the old stream reports which pin it matched.
SUPERSEDED_SHA256 = "d74efe02aec07aa59710ce457e54180c28a22022f3d35e7087096d5130dba49b"
ARES_VEHICLE_RNG_PREFIX_SHA256 = (
    "9fd7cb9aebc163b00f9c8e4bfd292f90b684b4d46415ab5e0ef594c8bfb2d16e"
)
FIELDS = (
    "frame", "map", "slot", "x", "y", "z", "xv", "yv", "zv", "fvel",
    "vel", "cp", "next", "lap", "countlap", "fin", "fpos", "ridx",
    "pidx", "vehicle", "grounded", "clock", "start", "delta", "rate", "rng",
)
FLOAT_FIELDS = {"x", "y", "z", "xv", "yv", "zv", "fvel", "vel"}


# game/src/racer.c's three consecutive s8 tables, by content. The AI balloon
# table is included so the signature is long enough to be unique in the image;
# the assertion is that D_800DCDA8 begins in the byte immediately after
# D_800DCDA0's eighth, which is what makes index 8 read a 1 and not a 2.
AI_BALLOON_ACTION_TABLE = bytes((1, 1, 2, 2, 4, 3, 0, 6, 4, 3, 2, 2, 5, 5, 5, 0))
RACING_LINE_TABLE = bytes((0, 0, 0, 1, 1, 2, 2, 2))          # D_800DCDA0
RACING_LINE_NEIGHBOUR = bytes((1, 1, 1, 2, 3, 2, 3, 2))      # D_800DCDA8
RACING_LINE_OVERRUN_VALUE = 1     # D_800DCDA8[0]: what 8th place actually reads
RACING_LINE_CLAMPED_VALUE = 2     # D_800DCDA0[7]: the invented clamp it replaced


def check_racing_line_overrun_witness(image: bytes) -> int:
    """Prove from the ROM which value 8th place's out-of-bounds read produces.

    Returns the ROM offset of D_800DCDA0. Raises when the tables are not
    adjacent, are not unique, or when the byte one past D_800DCDA0 is not the
    value this gate's stream was rebaselined onto -- any of which would mean the
    pin rests on an assumption the shipped image does not support.
    """
    # Locate D_800DCDA0 by the run that ENDS at it, so the bytes the assertions
    # are about are not themselves part of what is being searched for.
    signature = AI_BALLOON_ACTION_TABLE + RACING_LINE_TABLE
    hits = []
    start = image.find(signature)
    while start >= 0:
        hits.append(start)
        start = image.find(signature, start + 1)
    if len(hits) != 1:
        raise ValueError(
            "racing-line witness: expected exactly one AI-balloon/D_800DCDA0 "
            f"run in the ROM, found {len(hits)}")
    table = hits[0] + len(AI_BALLOON_ACTION_TABLE)
    past = table + len(RACING_LINE_TABLE)
    # Overrun byte first, so each assertion below is independently falsifiable:
    # it shares its value with RACING_LINE_NEIGHBOUR[0], and the adjacency test
    # would otherwise absorb every mutation of it.
    if image[past] != RACING_LINE_OVERRUN_VALUE:
        raise ValueError(
            f"racing-line witness: the byte past D_800DCDA0 (ROM {past:#x}) is "
            f"{image[past]}, not {RACING_LINE_OVERRUN_VALUE} -- the stream pin "
            "assumes that is what 8th place reads")
    if image[past:past + len(RACING_LINE_NEIGHBOUR)] != RACING_LINE_NEIGHBOUR:
        raise ValueError(
            f"racing-line witness: D_800DCDA0 (ROM {table:#x}) is not followed "
            "by D_800DCDA8 -- the two tables the pin assumes are adjacent are "
            "not adjacent in this image")
    if image[past - 1] != RACING_LINE_CLAMPED_VALUE:
        raise ValueError(
            "racing-line witness: D_800DCDA0[7] is not the superseded clamp "
            f"value {RACING_LINE_CLAMPED_VALUE}")
    return table


def validate(rows: list[str]) -> str:
    if len(rows) != EXPECTED_ROWS:
        raise ValueError(f"expected {EXPECTED_ROWS} [ORACLE] rows, got {len(rows)}")
    for row_index, row in enumerate(rows):
        tokens = row.split()
        if tokens[:2] != ["[TRACE]", "[ORACLE]"] or len(tokens) != len(FIELDS) + 2:
            raise ValueError(f"row {row_index}: malformed prefix/field count: {row}")
        for expected, token in zip(FIELDS, tokens[2:]):
            key, separator, value = token.partition("=")
            if separator != "=" or key != expected or not value:
                raise ValueError(f"row {row_index}: expected {expected}=..., got {token!r}")
            try:
                if expected in FLOAT_FIELDS:
                    if not math.isfinite(float(value)):
                        raise ValueError("non-finite")
                else:
                    int(value, 10)
            except ValueError as exc:
                raise ValueError(f"row {row_index}: invalid {expected} value {value!r}") from exc
    digest = hashlib.sha256(("\n".join(rows) + "\n").encode("utf-8")).hexdigest()
    if digest == SUPERSEDED_SHA256:
        raise ValueError(
            f"raw stream SHA-256 {digest} is the SUPERSEDED pin: this build "
            "still clamps 8th place's racing-line selector to D_800DCDA0[7] "
            "instead of taking the adjacent table's first byte -- see the "
            "rebaseline note at the top of this file")
    if digest != EXPECTED_SHA256:
        raise ValueError(f"raw stream SHA-256 {digest}, expected {EXPECTED_SHA256}")
    return digest


def run(binary: Path, rom: Path, timeout: int, verbose: bool) -> list[str]:
    with tempfile.TemporaryDirectory(prefix="mdkr-authored-rng-") as tmp:
        run_dir = Path(tmp)
        script = run_dir / "race_state_oracle_original.txt"
        route = subprocess.run(
            [sys.executable, str(ROUTE_TOOL), "native-script", "race_state_oracle",
             "--arm", "original"],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=30, check=False,
        )
        if route.returncode != 0:
            raise RuntimeError(f"route compiler exited {route.returncode}:\n{route.stdout}")
        script.write_text(route.stdout, encoding="utf-8")

        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update(
            LC_ALL="C", MDKR_AUDIO="0", MDKR_SIMULATION_CADENCE="original",
            MDKR_SYNTH_FIELDS="2", MDKR_TRACE="1", MDKR_ORACLE_STATE="1",
            MDKR_RENDERER="gl", MDKR_SAVE_DIR=str(run_dir / "save"),
            # Isolate the video config with the save (see check_door_blocks.py).
            MDKR_VIDEO_CONFIG_PATH=str(run_dir / "save" / "video.ini"),
        )
        command = [str(binary), "--headless-frames", str(FRAMES),
                   "--input-script", str(script), "--rom", str(rom)]
        if verbose:
            print("$ " + " ".join(command), flush=True)
        process = subprocess.run(
            command, cwd=run_dir, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=timeout, check=False,
        )
        if process.returncode != 0:
            raise RuntimeError(
                f"mdkr64 exited {process.returncode}:\n{(process.stdout or '')[-4000:]}")
        return [line for line in (process.stdout or "").splitlines()
                if "[ORACLE]" in line]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, ROUTE_TOOL):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1
    try:
        image = rom.read_bytes()
        table = check_racing_line_overrun_witness(image)
        # Witness control: the witness must reject an image whose overrun byte
        # is the superseded clamp value, or it is not testing anything.
        mutated = bytearray(image)
        mutated[table + len(RACING_LINE_TABLE)] = RACING_LINE_CLAMPED_VALUE
        try:
            check_racing_line_overrun_witness(bytes(mutated))
        except ValueError:
            pass
        else:
            raise RuntimeError("racing-line witness accepted a mutated ROM")
        del image, mutated

        rows = run(binary, rom, args.timeout, args.verbose)
        digest = validate(rows)

        # Broken-oracle control: prove a one-field change is rejected.
        mutated = list(rows)
        old_rng = mutated[0].rsplit("rng=", 1)[1]
        mutated[0] = mutated[0].rsplit("rng=", 1)[0] + f"rng={int(old_rng) ^ 1}"
        try:
            validate(mutated)
        except ValueError:
            pass
        else:
            raise RuntimeError("mutated-row positive control was not rejected")
    except (OSError, RuntimeError, subprocess.TimeoutExpired, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print(
        "check_authored_rng_compat: PASS "
        f"({EXPECTED_ROWS} rows, {digest}, reference {REFERENCE_COMMIT[:12]}, "
        f"ares car-RNG witness {ARES_VEHICLE_RNG_PREFIX_SHA256[:12]}, "
        f"racing-line overrun witness ROM {table:#x}+8 == "
        f"{RACING_LINE_OVERRUN_VALUE})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
