#!/usr/bin/env python3
"""Validate the local-only US 1.1 vehicle-audio RNG witness from ares.

The trace is produced by ``MDKR64_ARES_VEHICLE_RNG_TRACE`` in the pinned
instrumented ares build. Only calls entering the retail ``rand_range`` routine
from return addresses inside ``racer_sound_car`` are recorded. The trace stays
outside the repository; this validator commits only its schema and digest.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import sys
from collections import Counter
from pathlib import Path


EXPECTED_PREFIX_SHA256 = (
    "9fd7cb9aebc163b00f9c8e4bfd292f90b684b4d46415ab5e0ef594c8bfb2d16e"
)
EXPECTED_PREFIX_ROWS = 3_500
EXPECTED_HEADER = (
    "ordinal", "pc", "ra", "seed_before", "min", "max", "racer",
    "player", "vehicle",
)
EXPECTED_RETURN_SITES = {"0x800057d0", "0x8000580c"}


def rng_step(seed: int) -> int:
    seed &= 0xFFFFFFFF
    value = ((seed << 32) | (seed >> 1)) & ((1 << 64) - 1)
    value ^= (seed & 0xFFFFF) << 12
    return (value ^ ((value >> 20) & 0xFFF)) & 0xFFFFFFFF


def validate(path: Path) -> tuple[int, int, str]:
    raw = path.read_bytes()
    raw_digest = hashlib.sha256(raw).hexdigest()

    with path.open("r", encoding="ascii", newline="") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != EXPECTED_HEADER:
            raise ValueError(
                f"header {tuple(reader.fieldnames or ())}, expected "
                f"{EXPECTED_HEADER}"
            )
        rows = list(reader)
    # The emulator exits on a presented-frame boundary while the CPU can be in
    # the next audio update, so the terminal census can contain one additional
    # first roll. Pin a long pre-terminal prefix and bound the complete run.
    if not 4_800 <= len(rows) <= 4_810:
        raise ValueError(f"rows {len(rows)}, expected 4800..4810")

    ordinals = [int(row["ordinal"]) for row in rows]
    if ordinals != list(range(1, len(rows) + 1)):
        raise ValueError("ordinal sequence is not contiguous")
    if Counter(row["pc"] for row in rows) != {"0x8006fb8c": len(rows)}:
        raise ValueError("trace contains a call outside retail rand_range")
    return_counts = Counter(row["ra"] for row in rows)
    if set(return_counts) != EXPECTED_RETURN_SITES:
        raise ValueError("racer_sound_car return-address census changed")
    if not (2_920 <= return_counts["0x800057d0"] <= 2_935 and
            1_870 <= return_counts["0x8000580c"] <= 1_885):
        raise ValueError(f"unexpected return-address census {return_counts}")
    for field, expected in (("min", "0"), ("max", "10"),
                            ("player", "0"), ("vehicle", "0")):
        values = Counter(row[field] for row in rows)
        if values != {expected: len(rows)}:
            raise ValueError(f"{field} census {values}, expected only {expected}")

    prefix = "\n".join(
        ",".join(row[field] for field in (
            "pc", "ra", "seed_before", "min", "max", "player", "vehicle"
        ))
        for row in rows[:EXPECTED_PREFIX_ROWS]
    ) + "\n"
    prefix_digest = hashlib.sha256(prefix.encode("ascii")).hexdigest()
    if prefix_digest != EXPECTED_PREFIX_SHA256:
        raise ValueError(
            f"normalized {EXPECTED_PREFIX_ROWS}-row prefix SHA-256 "
            f"{prefix_digest}, expected {EXPECTED_PREFIX_SHA256}"
        )

    # When the N64 scheduler does not switch threads between the two authored
    # jitter rolls, the second entry must see exactly one retail RNG step. An
    # interrupt may legitimately consume other shared draws between them, so
    # this is a lower-bound structural control rather than an exact count.
    direct_pairs = sum(
        left["ra"] == "0x800057d0"
        and right["ra"] == "0x8000580c"
        and rng_step(int(left["seed_before"])) == int(right["seed_before"])
        for left, right in zip(rows, rows[1:])
    )
    if direct_pairs < 900:
        raise ValueError(
            f"only {direct_pairs} direct first-to-second RNG transitions"
        )
    return len(rows), direct_pairs, raw_digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    try:
        rows, direct_pairs, raw_digest = validate(args.trace)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"validate_ares_vehicle_rng: FAIL -- {error}", file=sys.stderr)
        return 1
    print(
        "validate_ares_vehicle_rng: PASS -- "
        f"{rows} retail car-audio RNG calls, {direct_pairs} direct paired "
        f"transitions, prefix witness {EXPECTED_PREFIX_SHA256}, "
        f"raw trace {raw_digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
