#!/usr/bin/env python3
"""Every GAME_TEXT index the driven routes ask for, and the ceiling it must clear.

Why this exists
---------------
`platform/rom_id.c` refuses US 1.0 and European 1.0 because this build is
compiled for us.v80 DATA and nothing bounds-checks an index *inside* an asset
section.  `docs/ROM_REVISIONS.md` Sec 5 measured the gap that matters most:
us.v80's `GAME_TEXT` holds 343 entries where us.v77's holds 259.  A v80-only
string index handed to a v77 ROM is an out-of-range read.

`game/src/game_text.c` makes it worse than a plain index-out-of-range, and the
shape is worth stating because it is what this check actually measures.
`set_current_text()` range-tests the id it is GIVEN::

    if (gTextTableExists && textID >= 0 && textID < gTextTableEntries) {
        language = get_language();
        switch (language) {
            case LANGUAGE_GERMAN:   textID += 85;  break;
            case LANGUAGE_FRENCH:   textID += 170; break;
            case LANGUAGE_JAPANESE: textID += 255; break;
        }

...and then adds the language offset AFTERWARDS.  The resolved index is never
re-tested.  So it is not enough for the port to ask for small ids: an id of 84
under French resolves to 254 and under Japanese to 339, and only the resolved
value is what indexes the section.  That resolved value is what is recorded and
what is bounded here.

How it is measured
------------------
`game/src/game_text.c` gained one `#ifdef NATIVE_PORT` census site at exactly
that point -- the single place in the port where a GAME_TEXT entry index is
formed -- emitting::

    [TEXTIDX] section=GAME_TEXT index=<resolved> requested=<pre-language>
              language=<n> count=<gTextTableEntries>

It is off unless `MDKR_TEXTIDX` is set, and when the variable holds a path the
census is appended THERE rather than to stderr.  That matters: this check drives
the real route gates, and those gates parse the engine's stderr.  A second
stream interleaved into it would change what they read, so each engine run gets
its own census file instead, via a shim named `mdkr64` that the route gates are
pointed at with `--build`.  Nothing in those gates is modified, so the routes
driven here are the routes they actually drive -- not a copy that can drift.

What is asserted
----------------
  1. Every route gate below actually launched the engine.  A gate that produced
     no census at all is a FAILURE, not a quiet pass: the whole claim is an
     enumeration, and an enumeration over nothing proves nothing.
  2. At least one GAME_TEXT index was observed overall, for the same reason.
  3. The maximum resolved index observed is below **259** -- the smallest
     `GAME_TEXT` entry count among the five released revisions
     (`docs/ROM_REVISIONS.md` Sec 5: us.v77 and pal.v77 hold 259; jpn.v79,
     us.v80 and pal.v80 hold 343).  The offending index and the route that
     produced it are printed.

COVERAGE -- what this does and does not claim
---------------------------------------------
The claim is **"these routes never resolve a GAME_TEXT index at or above
259"**, not "the port never does".  The corpus is not the whole game.

Routes driven (the corpus `docs/sprints/S5-rom-region-breadth.md` Task 4 names):

  * the nine `nav_*` menu fixtures, via `tests/check_nav_fixtures.py`;
  * the 20-track race sweep, via `tests/check_track_sweep.py`;
  * the Timber's Island hub tour, via `tests/check_adventure_hub.py`;
  * the adventure campaign seams -- silver coins, boss doors, rematches, the
    credits cheat -- via `tests/check_campaign_progression.py`;
  * the trophy championship series, via `tests/check_trophy_series.py`.

What that leaves uncovered, stated plainly:

  * **Sections.** Three asset sections carry text: `GAME_TEXT`, `MENU_TEXT` and
    `LEVEL_NAMES`.  Only `GAME_TEXT` is instrumented, because it is the one
    whose entry count differs between the revisions this sprint is trying to
    accept.  `MENU_TEXT` is addressed by compile-time enum constants through a
    rebuilt pointer array (`load_menu_text()` in `game/src/menu.c`) and is not
    counted here at all.
  * **Entries.** The run prints the fraction of the section's addressable ids
    the corpus reached.  It is small.  The frontend menus reach none of them:
    `load_game_text_table()` runs on level entry (`game/src/thread3_main.c`),
    so the `nav_*` fixtures that stop in a menu contribute nothing, which the
    per-route table below makes visible rather than hiding.
  * **Languages.** The corpus runs in the default language, so the +85/+170/+255
    resolved offsets are exercised only where a route changes language.  The
    per-route table prints the languages seen, and the run then prints the
    resolved index the same observed ids WOULD reach under French (+170, the
    largest offset this build can select) and Japanese (+255, REGION_JP only).
    German (+85) is strictly smaller than French and is not printed separately.
    That projection is arithmetic, not a measurement, and does not fail the gate
    -- but it is printed, because an unexercised language arm is the largest
    single hole in this enumeration and a recorded maximum that quietly omitted
    it would read as a bound it is not.

That is why the sprint orders this check BEFORE widening the accepted set and
not instead of the bounds check that landed with it: the enumeration narrows the
risk, `platform/asset_subentry.c` is what makes an unenumerated index loud.

Usage::

    MDKR_AUDIO=0 python3 tests/check_rom_text_indices.py [--build build]
                         [--rom baserom.us.v80.z64] [--only nav,hub] [-v]

Every engine run is muted and headless -- the route gates below enforce both,
and this check adds nothing that could re-enable audio.  Exit 0 = pass.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent

# GAME_TEXT entry count per released revision, from docs/ROM_REVISIONS.md Sec 5's
# section-by-section walk. Decompilation/analysis metadata -- an entry COUNT, not
# any ROM content. us.v77 and pal.v77 share a byte-identical asset payload;
# jpn.v79 differs from us.v80 only in LEVEL_OBJECT_MAPS and LEVEL_MODELS, so its
# GAME_TEXT matches us.v80's.
GAME_TEXT_ENTRIES = {
    "us.v77": 259,
    "pal.v77": 259,
    "jpn.v79": 343,
    "us.v80": 343,
    "pal.v80": 343,
}

# The ceiling every observed index must clear: the smallest count above. Stated
# as a literal as well as derived, so a table edit that raised the floor could
# not silently relax the gate.
CEILING = min(GAME_TEXT_ENTRIES.values())
assert CEILING == 259, "the 1.0 GAME_TEXT entry count is the gate; it is 259"

# The route gates, driven unmodified. Each is given --build <shim> --rom <rom>;
# nothing else about how it drives the engine is restated here, which is the
# point -- these routes cannot drift away from the ones the suite runs.
#
# `budget` is this check's own subprocess timeout in seconds, sized off measured
# wall time with a wide margin. A gate that overruns it is reported as a route
# that failed to produce a census, never as a pass.
ROUTE_GATES = [
    ("nav", "check_nav_fixtures.py", "the nine nav_* menu fixtures", 900),
    ("tracks", "check_track_sweep.py", "the 20-track race sweep", 2400),
    ("hub", "check_adventure_hub.py", "the Timber's Island hub tour", 900),
    ("campaign", "check_campaign_progression.py", "the adventure campaign seams", 3600),
    ("trophy", "check_trophy_series.py", "the trophy championship series", 3600),
]

RUN_RE = re.compile(
    r"^\[TEXTIDX-RUN\] track=(?P<track>\S*) world=(?P<world>\S*) "
    r"argv=(?P<argv>.*)$")
IDX_RE = re.compile(
    r"^\[TEXTIDX\] section=(?P<section>\S+) index=(?P<index>-?\d+) "
    r"requested=(?P<requested>-?\d+) language=(?P<language>-?\d+) "
    r"count=(?P<count>-?\d+)$"
)

LANGUAGE_NAMES = {0: "english", 1: "german", 2: "french", 3: "japanese"}

# The shim the route gates are pointed at. It is named `mdkr64` and lives in its
# own directory so harness_utils.resolve_binary() finds it from --build, and it
# execs the real binary so the exit status, stdout and stderr the gate sees are
# untouched. One census file per invocation ($$ is the shim's pid), headed by the
# argv, so an index can be attributed to the exact run that produced it. The two
# env hooks are recorded too: which track a sweep run loaded and which world a
# trophy run raced are the difference between two otherwise identical argvs.
SHIM = """#!/bin/sh
SINK="{runs}/run.$$.log"
printf '[TEXTIDX-RUN] track=%s world=%s argv=%s\\n' \\
    "${{MDKR_LOAD_TRACK-}}" "${{MDKR_TROPHY_WORLD-}}" "$*" >> "$SINK"
MDKR_TEXTIDX="$SINK" exec "{real}" "$@"
"""


def build_shim(workdir: Path, real: Path) -> tuple[Path, Path]:
    """Write the census shim. Returns (build_dir_for_gates, runs_dir)."""
    shim_dir = workdir / "shim"
    runs = shim_dir / "runs"
    runs.mkdir(parents=True)
    binary = shim_dir / "mdkr64"
    binary.write_text(SHIM.format(runs=runs, real=real), encoding="ascii")
    binary.chmod(0o755)
    return shim_dir, runs


def route_label(header: re.Match) -> str:
    """A short, path-free name for one engine invocation.

    Deliberately drops the ROM and temporary-directory paths the argv carries:
    the useful identity of a run is its input script, its length, and which
    track or world it was pointed at.
    """
    parts = header.group("argv").split()
    script = "(no script)"
    frames = "?"
    for i, part in enumerate(parts):
        if part == "--input-script" and i + 1 < len(parts):
            script = os.path.basename(parts[i + 1])
        elif part == "--headless-frames" and i + 1 < len(parts):
            frames = parts[i + 1]
    label = "%s @%s frames" % (script, frames)
    if header.group("track"):
        label += " track=%s" % header.group("track")
    if header.group("world"):
        label += " world=%s" % header.group("world")
    return label


def read_census(runs: Path) -> list[dict]:
    """Every [TEXTIDX] record under `runs`, each tagged with its invocation."""
    records = []
    for path in sorted(runs.glob("run.*.log")):
        current = "(unattributed)"
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            run = RUN_RE.match(line)
            if run:
                current = route_label(run)
                continue
            hit = IDX_RE.match(line)
            if hit:
                records.append({
                    "run": current,
                    "section": hit.group("section"),
                    "index": int(hit.group("index")),
                    "requested": int(hit.group("requested")),
                    "language": int(hit.group("language")),
                    "count": int(hit.group("count")),
                })
    return records


def drive(gate: str, shim_dir: Path, rom: Path, budget: int,
          verbose: bool) -> tuple[str, int | None]:
    """Run one route gate against the shim. Returns (status, exit code)."""
    script = ROOT / "tests" / gate
    if not script.is_file():
        return ("missing %s" % gate, None)
    command = [sys.executable, str(script), "--build", str(shim_dir),
               "--rom", str(rom)]
    if verbose:
        print("      $ %s %s --build <shim> --rom <rom>" % (sys.executable, gate))
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"   # belt-and-braces; each gate sets it too
    try:
        done = subprocess.run(command, cwd=ROOT, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=budget, check=False)
    except subprocess.TimeoutExpired:
        return ("timed out after %ds" % budget, None)
    # The gate's own verdict is deliberately NOT this check's verdict: it can be
    # red for a reason that has nothing to do with text indices, and the census
    # it produced on the way is still valid evidence. It is reported, not acted
    # on -- except that a gate which launched no engine is caught below.
    return ("exit %d" % done.returncode, done.returncode)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--only", default=None,
                    help="comma-separated subset of: "
                         + ",".join(name for name, _s, _d, _b in ROUTE_GATES))
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    if not binary.is_file():
        sys.exit("no build at %s" % binary)
    rom = Path(args.rom)
    if not rom.is_file():
        rom = ROOT / args.rom
    if not rom.is_file():
        print("check_rom_text_indices: SKIP - no ROM at %s (bring-your-own-ROM)"
              % args.rom)
        return 0
    rom = rom.resolve()

    selected = ROUTE_GATES
    if args.only:
        wanted = {n.strip() for n in args.only.split(",") if n.strip()}
        unknown = wanted - {name for name, _s, _d, _b in ROUTE_GATES}
        if unknown:
            sys.exit("unknown route group(s): %s" % ", ".join(sorted(unknown)))
        selected = [g for g in ROUTE_GATES if g[0] in wanted]

    print("check_rom_text_indices: build=%s ceiling=%d (the us.v77/pal.v77 "
          "GAME_TEXT entry count)" % (binary.name, CEILING))
    failures: list[str] = []
    workdir = Path(tempfile.mkdtemp(prefix="mdkr_textidx_"))
    per_gate: list[tuple[str, str, str, list[dict]]] = []
    try:
        for name, gate, description, budget in selected:
            shim_dir, runs = build_shim(workdir / name, binary)
            status, _code = drive(gate, shim_dir, rom, budget, args.verbose)
            records = read_census(runs)
            launches = len(list(runs.glob("run.*.log")))
            per_gate.append((name, description, status, records))
            print("   %-9s %-34s %s, %d engine run(s), %d index access(es)"
                  % (name, description, status, launches, len(records)))
            if launches == 0:
                failures.append(
                    "%s (%s) never launched the engine (%s) - its part of the "
                    "enumeration is vacuous, so the ceiling is unproven for it"
                    % (name, gate, status))

        records = [r for _n, _d, _s, rs in per_gate for r in rs]
        print()
        print("   per-route GAME_TEXT indices")
        any_route = False
        for name, _description, _status, rs in per_gate:
            by_run: dict[str, list[dict]] = {}
            for record in rs:
                by_run.setdefault(record["run"], []).append(record)
            for run in sorted(by_run):
                hits = by_run[run]
                indices = sorted({h["index"] for h in hits})
                languages = sorted({h["language"] for h in hits})
                any_route = True
                print("     %-9s %-52s max=%-4d ids=%s lang=%s"
                      % (name, run, max(indices),
                         ",".join(str(i) for i in indices[:8])
                         + (" ..." if len(indices) > 8 else ""),
                         ",".join(LANGUAGE_NAMES.get(code, str(code))
                                  for code in languages)))
        if not any_route:
            print("     (none)")

        if not records:
            failures.append(
                "no GAME_TEXT index was observed on any route. Either the "
                "MDKR_TEXTIDX census in game/src/game_text.c is not compiled in, "
                "or no driven route reached a text access - this check would "
                "have passed vacuously")
        else:
            worst = max(records, key=lambda r: r["index"])
            distinct = sorted({r["index"] for r in records})
            addressable = max(r["count"] for r in records)
            print()
            print("   maximum resolved index %d on %s (requested=%d, language=%s)"
                  % (worst["index"], worst["run"], worst["requested"],
                     LANGUAGE_NAMES.get(worst["language"], worst["language"])))
            print("   coverage: %d distinct id(s) of the %d this ROM addresses "
                  "(%.1f%%), over %d access(es); 1 of the 3 text-bearing asset "
                  "sections instrumented (GAME_TEXT; not MENU_TEXT, not "
                  "LEVEL_NAMES)"
                  % (len(distinct), addressable,
                     100.0 * len(distinct) / addressable if addressable else 0.0,
                     len(records)))
            # A projection, printed and clearly labelled as one, because the
            # honest reading of the census depends on it: the language offset is
            # added AFTER the range test, so the same routes run in another
            # language resolve the same pre-language ids to higher numbers. This
            # is arithmetic on the observed ids, not a measurement, so it never
            # fails the gate -- but leaving it unsaid would let the recorded
            # maximum read as a bound it is not.
            worst_requested = max(r["requested"] for r in records)
            for offset, language in ((170, "French"), (255, "Japanese")):
                projected = worst_requested + offset
                verdict = ("would exceed the %d-entry 1.0 count"
                           % CEILING) if projected >= CEILING else "stays in range"
                print("   projection: largest pre-language id %d + %d (%s) = %d, "
                      "which %s%s"
                      % (worst_requested, offset, language, projected, verdict,
                         " [REGION_JP only]" if offset == 255 else ""))

            over = [r for r in records if r["index"] >= CEILING]
            if over:
                offender = max(over, key=lambda r: r["index"])
                failures.append(
                    "GAME_TEXT index %d is at or above the %d-entry 1.0 count. "
                    "Produced by %s (requested id %d, language %s, %d access(es) "
                    "over the ceiling). On a us.v77 or pal.v77 ROM that index is "
                    "out of range, and set_current_text() does not re-test it "
                    "after the language offset"
                    % (offender["index"], CEILING, offender["run"],
                       offender["requested"],
                       LANGUAGE_NAMES.get(offender["language"], offender["language"]),
                       len(over)))
            bad_section = sorted({r["section"] for r in records} - {"GAME_TEXT"})
            if bad_section:
                failures.append(
                    "the census reported section(s) %s, which this check does not "
                    "know how to bound" % ", ".join(bad_section))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print()
    if failures:
        for failure in failures:
            print("check_rom_text_indices: FAIL - %s" % failure)
        print("check_rom_text_indices: FAIL (%d)" % len(failures))
        return 1
    print("check_rom_text_indices: PASS - no driven route resolves a GAME_TEXT "
          "index at or above %d" % CEILING)
    return 0


if __name__ == "__main__":
    sys.exit(main())
