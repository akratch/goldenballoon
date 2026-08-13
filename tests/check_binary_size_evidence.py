#!/usr/bin/env python3
"""Record and defend packaged release artifact sizes against a checked-in baseline.

This is the binary-size *evidence* gate for the Phone Party GA exit list: it
exists so a release can point at recorded, dated, commit-stamped artifact sizes
and so a silent multi-hundred-percent growth cannot ship unnoticed. The ceilings
are deliberately generous (roughly 1.5x, with at least 8 MiB of headroom) — this
is a tripwire, not a size budget.

Baseline: tests/binary_size_baseline.json.

Re-baselining is deliberate and explicit:

    python3 tests/check_binary_size_evidence.py --record
    python3 tests/check_binary_size_evidence.py --record --lane macos

--record rewrites the recorded bytes, ceiling, date and commit for every
artifact that is actually present on disk, and leaves the rest untouched. Run it
on a machine (or CI job) that has just produced the artifacts for the lane you
are recording, then commit the JSON with the release.

Local runs skip artifacts this machine did not build and say so. CI runs should
name the lane they built:

    python3 tests/check_binary_size_evidence.py --require linux
    python3 tests/check_binary_size_evidence.py --strict     # every lane

A required lane must have every artifact present, baselined and under ceiling.

The web lane is intentionally absent: dist/web/*.wasm already has a hard,
release-blocking byte budget in tools/web/build_web.sh and an independent
re-check in .github/workflows/web-demo.yml. A third, tighter ceiling here would
only add a false-regression surface.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import date
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
BASELINE = Path(__file__).resolve().parent / "binary_size_baseline.json"
MIB = 1024 * 1024
HEADROOM_BYTES = 8 * MIB
HEADROOM_FACTOR = 1.5


class CheckFailure(Exception):
    """A recorded size, ceiling or required artifact did not hold."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailure(message)


def ceiling_for(measured: int) -> int:
    """Generous tripwire: 1.5x or +8 MiB, whichever is larger, rounded up."""
    generous = max(measured * 3 // 2, measured + HEADROOM_BYTES)
    return -(-generous // MIB) * MIB


def head_commit() -> str:
    try:
        return subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def measure(entry: dict[str, Any]) -> tuple[int | None, str, int]:
    """Return (bytes, resolved path text, number of matches) for one artifact."""
    matches = sorted(ROOT.glob(entry["pattern"]))
    if entry["kind"] == "bundle":
        matches = [item for item in matches if item.is_dir()]
    else:
        matches = [item for item in matches if item.is_file()]
    if not matches:
        return None, "", 0
    chosen = matches[-1]
    if entry["kind"] == "bundle":
        total = sum(item.stat().st_size
                    for item in chosen.rglob("*") if item.is_file())
    else:
        total = chosen.stat().st_size
    return total, str(chosen.relative_to(ROOT)), len(matches)


def human(value: int | None) -> str:
    if value is None:
        return "—"
    return f"{value:,} ({value / MIB:.1f} MiB)"


def load() -> dict[str, Any]:
    require(BASELINE.is_file(), f"missing size baseline: {BASELINE}")
    document = json.loads(BASELINE.read_text(encoding="utf-8"))
    require(isinstance(document.get("artifacts"), list) and document["artifacts"],
            "size baseline has no artifacts")
    for entry in document["artifacts"]:
        for field in ("name", "lane", "pattern", "kind"):
            require(field in entry, f"baseline entry missing {field}: {entry}")
        require(entry["kind"] in ("file", "bundle"),
                f"unknown artifact kind: {entry['kind']}")
    return document


def record(document: dict[str, Any], lanes: set[str] | None) -> int:
    stamp = date.today().isoformat()
    commit = head_commit()
    recorded = 0
    rows: list[tuple[str, str, str, str]] = []
    for entry in document["artifacts"]:
        if lanes is not None and entry["lane"] not in lanes:
            continue
        measured, where, _ = measure(entry)
        if measured is None:
            rows.append((entry["name"], "not present", "—", "kept"))
            continue
        previous = entry.get("bytes")
        entry["bytes"] = measured
        entry["ceiling"] = ceiling_for(measured)
        entry["recordedOn"] = stamp
        entry["recordedCommit"] = commit
        entry["recordedFrom"] = where
        recorded += 1
        rows.append((entry["name"], human(measured), human(entry["ceiling"]),
                     "new" if previous is None else f"was {human(previous)}"))
    print(f"{'artifact':<28} {'recorded':>24} {'ceiling':>24}  note")
    for name, size, ceiling, note in rows:
        print(f"{name:<28} {size:>24} {ceiling:>24}  {note}")
    document["recordedOn"] = stamp
    document["recordedCommit"] = commit
    BASELINE.write_text(
        json.dumps(document, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8")
    return recorded


def run(args: argparse.Namespace) -> None:
    document = load()
    known = {entry["lane"] for entry in document["artifacts"]}
    lanes: set[str] | None = None
    if args.lane:
        lanes = {value.strip() for item in args.lane for value in item.split(",")
                 if value.strip()}
        unknown = lanes - known
        require(not unknown, f"unknown lane(s) {sorted(unknown)}; known: {sorted(known)}")

    if args.record:
        count = record(document, lanes)
        print(f"check_binary_size_evidence: PASS — re-baselined {count} artifact(s) "
              f"in {BASELINE.relative_to(ROOT)}; commit the JSON with the release")
        return

    required: set[str] = set()
    if args.strict:
        required = set(known)
    for item in args.require:
        for value in item.split(","):
            if value.strip():
                required.add(value.strip())
    unknown = required - known
    require(not unknown, f"unknown required lane(s) {sorted(unknown)}; known: {sorted(known)}")

    rows: list[tuple[str, str, str, str, str]] = []
    failures: list[str] = []
    skipped = 0
    checked = 0
    for entry in document["artifacts"]:
        if lanes is not None and entry["lane"] not in lanes:
            continue
        name = entry["name"]
        strict = entry["lane"] in required
        measured, where, matches = measure(entry)
        baseline = entry.get("bytes")
        ceiling = entry.get("ceiling")

        if measured is None:
            note = "not built on this machine"
            if strict:
                failures.append(f"{name}: required lane '{entry['lane']}' "
                                f"produced no artifact matching {entry['pattern']}")
                verdict = "MISSING"
            else:
                skipped += 1
                verdict = "SKIP"
            rows.append((name, "—", human(baseline), human(ceiling),
                         f"{verdict} — {note}"))
            continue

        if matches > 1:
            failures.append(f"{name}: {matches} artifacts match {entry['pattern']}; "
                            f"size evidence must be unambiguous")
        if baseline is None or ceiling is None:
            note = ("no recorded baseline — run --record --lane "
                    f"{entry['lane']} on this machine")
            if strict:
                failures.append(f"{name}: present at {human(measured)} but never "
                                f"baselined; run --record --lane {entry['lane']}")
                verdict = "UNRECORDED"
            else:
                skipped += 1
                verdict = "RECORD"
            rows.append((name, human(measured), "—", "—", f"{verdict} — {note}"))
            continue

        checked += 1
        if measured > ceiling:
            over = measured - ceiling
            failures.append(
                f"{name}: {where} is {human(measured)}, over its ceiling "
                f"{human(ceiling)} by {human(over)} "
                f"(baseline {human(baseline)} recorded {entry.get('recordedOn')} "
                f"@ {entry.get('recordedCommit')}); investigate the growth, then "
                f"re-baseline deliberately with --record --lane {entry['lane']}")
            verdict = "OVER"
        else:
            drift = (measured - baseline) / baseline * 100 if baseline else 0.0
            verdict = f"ok {drift:+.1f}% vs baseline"
        rows.append((name, human(measured), human(baseline), human(ceiling), verdict))

    width = max(len(row[0]) for row in rows) if rows else 8
    print(f"{'artifact':<{width}} {'measured':>24} {'baseline':>24} "
          f"{'ceiling':>24}  verdict")
    for name, measured_text, baseline_text, ceiling_text, verdict in rows:
        print(f"{name:<{width}} {measured_text:>24} {baseline_text:>24} "
              f"{ceiling_text:>24}  {verdict}")
    print(f"baseline recorded {document.get('recordedOn')} @ "
          f"{document.get('recordedCommit')} in {BASELINE.relative_to(ROOT)}")

    require(not failures, "; ".join(failures))
    require(checked > 0,
            "no artifact could be measured against a recorded baseline; "
            "build a release lane or pass --require <lane> to make that fatal")
    print(f"check_binary_size_evidence: PASS — {checked} packaged artifact(s) "
          f"within their recorded ceilings, {skipped} not built here"
          + (f", required lanes {sorted(required)} complete" if required else ""))


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--record", action="store_true",
                        help="rewrite the checked-in baseline from the artifacts "
                             "present on this machine (deliberate re-baseline)")
    parser.add_argument("--lane", action="append", default=[],
                        help="restrict the run to these lanes (comma separated)")
    parser.add_argument("--require", action="append", default=[],
                        help="lanes whose artifacts must all be present and "
                             "baselined (CI-facing, comma separated)")
    parser.add_argument("--strict", action="store_true",
                        help="require every lane in the baseline")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"check_binary_size_evidence: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
