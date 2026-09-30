#!/usr/bin/env python3
"""Audit unpublished held-camera chain logs, not production motion quality.

Checks history continuity and reported proof/return invariants. This does not
independently establish geometric clearance or validate published camera poses.
Logs remain private; only aggregate counts and offending line numbers print.
"""
import argparse
import math
import re
from pathlib import Path

MARKER = "camera_held_chain_probe "
ROW = re.compile(
    r"camera_held_chain_probe tick=(\d+) slot=(\d+) continued=([01]) "
    r"prior_length=(\d+) valid=([01]) target=\(([^,]+),([^\)]+)\) "
    r"recovering=([01]) remaining=(\S+) returned=([01])"
    r"(?: rejection=(history|pose|endpoint|transition|boom|visibility|degraded|framing|none))?$")


def audit(lines, required_returns=()):
    history = {}
    returns = set()
    counts = dict(samples=0, accepted=0, rejected=0, returned=0)
    rejections = {}
    for number, line in enumerate(lines, 1):
        if MARKER not in line:
            continue
        match = ROW.search(line.strip())
        if not match:
            raise ValueError(f"line {number}: malformed chain record")
        tick, slot, continued, length, valid = map(int, match.group(1, 2, 3, 4, 5))
        x, y = map(float, match.group(6, 7))
        recovering, returned = map(int, match.group(8, 10))
        remaining = float(match[9])
        rejection = match[11]
        if rejection is not None and (rejection == "none") != bool(valid):
            raise ValueError(f"line {number}: rejection disagrees with proof result")
        previous = history.get(slot)
        if continued and previous != (tick - 1, length):
            raise ValueError(f"line {number}: continuation lacks matching accepted history")
        if not continued and length != 0:
            raise ValueError(f"line {number}: new seed retains old length")
        if valid and recovering and not continued:
            raise ValueError(f"line {number}: recovery reseeded from production")
        if valid and not (math.isfinite(x) and math.isfinite(y) and
                          abs(x) <= 1 and abs(y) <= 1 and
                          math.isfinite(remaining) and remaining >= 0):
            raise ValueError(f"line {number}: accepted invalid framing/distance")
        if returned and not (valid and recovering and remaining == 0):
            raise ValueError(f"line {number}: invalid return claim")
        history[slot] = (tick, min(length + 1, 0xffffffff)) if valid and not returned else None
        counts["samples"] += 1
        counts["accepted" if valid else "rejected"] += 1
        if not valid:
            reason = rejection or "unclassified"
            rejections[reason] = rejections.get(reason, 0) + 1
        if returned:
            counts["returned"] += 1
            returns.add(tick)
    if not counts["samples"]:
        raise ValueError("no chain evidence")
    if not set(required_returns) <= returns:
        raise ValueError("required return ticks absent")
    return dict(counts, rejections=rejections)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--require-return-tick", type=int, action="append", default=[])
    args = parser.parse_args()
    try:
        with args.log.open() as stream:
            print("PASS chain evidence:", audit(stream, args.require_return_tick))
    except (OSError, ValueError) as error:
        parser.exit(1, f"FAIL chain evidence: {error}\n")
