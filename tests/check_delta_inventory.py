#!/usr/bin/env python3
"""Every //!@Delta site must carry an M0 classification, not just a flag.

`//!@Delta` marks a per-tick constant that was authored for the 30 Hz tick and
does not scale with the Enhanced 60 Hz simulation cadence (docs/CAMPAIGN_
FAITHFUL_ENHANCED.md, milestone M0). Marking a site is only half the job: M1-M3
apply a different fix shape depending on what kind of per-tick operation it is,
so an unclassified marker is a latent M1 defect waiting to happen -- exactly
the shape issue #26 and the menu auto-repeat bug already shipped as.

This gate is ROM-free and holds no opinion on whether a classification is
CORRECT, only that one is PRESENT: `//!@Delta <CLASS>`, where CLASS is one of

  CONTINUOUS  an integral over time (position/velocity integration, a decay
              or lerp toward a target, animation phase). Fix shape: additive
              terms take `* updateRateF * 0.5`; multiplicative decay `k`
              becomes `k ** (updateRate / 2)`.
  DISCRETE    an event, not an integral (an RNG draw, an AI decision, a state
              transition). Fix shape: gate to authored 30 Hz tick boundaries.
  THRESHOLD   a per-tick counter compared against a fixed constant bound. Fix
              shape: scale the bound, leave the counter alone (the landed
              example is menu_stick_delay_amount() in game/src/menu.c).
  INERT       provably unaffected under Enhanced -- equilibrium-governed (an
              opposing term cancels it at steady state) or already scaled on
              the same path. The classification comment must say which.

optionally followed by `: <rationale>` free text, which this gate does not
parse. A bare `//!@Delta` with no classification token fails the gate.
"""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOTS = (ROOT / "game", ROOT / "platform")
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".mm"}

CLASSES = ("CONTINUOUS", "DISCRETE", "THRESHOLD", "INERT")

# Matches the marker and captures whatever immediately follows it on the line.
MARKER_RE = re.compile(r"!@Delta(?P<rest>.*)$")
CLASSIFIED_RE = re.compile(
    r"^\s+(" + "|".join(CLASSES) + r")\b"
)

# Prose that mentions the marker family by name, rather than a marker itself.
# check_delta_inventory only scans for the literal `!@Delta` substring, so an
# English sentence about the markers ("...the //!@Delta sites...") would
# otherwise be misread as one unclassified site. Excluded explicitly, by exact
# line content, so a real new marker landing on this line cannot be swallowed
# silently -- if the text ever changes, the assertion below catches it.
EXCLUDED_MENTIONS: dict[tuple[Path, int], str] = {
    # (Empty since 2026-08-10: the one excluded prose mention in racer.c was
    # rewritten away on the release line; the merged tree has no prose line
    # containing the sigil. The mechanism stays for the next one.)
}


def find_markers(source: str) -> list[tuple[int, str]]:
    """Every line in `source` containing the marker sigil, 1-indexed."""
    return [
        (number, line)
        for number, line in enumerate(source.splitlines(), 1)
        if "!@Delta" in line
    ]


def is_classified(line: str) -> bool:
    match = MARKER_RE.search(line)
    if match is None:
        return False
    return CLASSIFIED_RE.match(match.group("rest")) is not None


def main() -> int:
    unclassified: list[str] = []
    counts: dict[str, int] = {name: 0 for name in CLASSES}
    total = 0

    for source_root in SOURCE_ROOTS:
        for path in sorted(source_root.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            relative = path.relative_to(ROOT)
            text = path.read_text(encoding="utf-8", errors="replace")
            for number, line in find_markers(text):
                key = (relative, number)
                if key in EXCLUDED_MENTIONS:
                    expected = EXCLUDED_MENTIONS[key]
                    if line != expected:
                        unclassified.append(
                            f"{relative}:{number}: excluded-mention text changed "
                            f"-- re-check whether this is now a real marker: {line.strip()}"
                        )
                    continue
                total += 1
                match = MARKER_RE.search(line)
                classified = match is not None and CLASSIFIED_RE.match(
                    match.group("rest")
                )
                if not classified:
                    unclassified.append(f"{relative}:{number}: {line.strip()}")
                    continue
                counts[classified.group(1)] += 1

    failures: list[str] = []
    if total == 0:
        failures.append(
            "found zero //!@Delta markers under game/ or platform/ -- either "
            "the whole family was deleted or this gate's scan is broken; "
            "either way the M0 inventory can no longer be trusted"
        )
    if unclassified:
        failures.append(
            f"{len(unclassified)} //!@Delta marker(s) carry no M0 classification:\n    "
            + "\n    ".join(unclassified)
        )

    # Positive control: a seeded bare marker, with no classification, must be
    # rejected by the same scanner used above. This is exercised directly
    # against find_markers()/is_classified() rather than the filesystem walk,
    # so the control cannot be satisfied by accident (e.g. by scanning the
    # wrong directory) the way a filesystem-based control could.
    control_source = "    racer->velocity -= 1.0; //!@Delta\n"
    control_markers = find_markers(control_source)
    if len(control_markers) != 1:
        failures.append("positive control setup is broken: expected exactly one marker line")
    elif is_classified(control_markers[0][1]):
        failures.append("positive control escaped: a bare //!@Delta was accepted as classified")

    # Negative control: the same line WITH a classification must be accepted,
    # so the gate is not simply rejecting every marker it sees.
    classified_source = "    racer->velocity -= 1.0; //!@Delta CONTINUOUS: velocity integration\n"
    classified_markers = find_markers(classified_source)
    if len(classified_markers) != 1 or not is_classified(classified_markers[0][1]):
        failures.append(
            "negative control failed: a properly classified //!@Delta was rejected"
        )

    if failures:
        raise AssertionError(
            "check_delta_inventory: FAIL\n  " + "\n  ".join(failures)
        )

    print(
        "check_delta_inventory: PASS -- "
        f"{total} //!@Delta markers, all classified "
        f"(CONTINUOUS={counts['CONTINUOUS']} DISCRETE={counts['DISCRETE']} "
        f"THRESHOLD={counts['THRESHOLD']} INERT={counts['INERT']}); "
        "bare-marker positive control rejected, classified negative control accepted"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
