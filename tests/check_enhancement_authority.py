#!/usr/bin/env python3
"""Every enhancement's declared authority class matches its measured effect.

Why this exists
---------------
`platform/enhancement_registry.c` gives each enhancement an AUTHORITY CLASS.
`MDKR_ENH_PRESENTATION` claims the setting cannot move authoritative state;
`MDKR_ENH_GAMEPLAY` claims it does. Those are claims, and a claim nobody tests
is a comment.

This gate tests both directions, for every row, from the table the RUNNING
BINARY has:

  presentation  the `[SIMHASH]` v3 stream must be byte-identical with the
                enhancement at its default and at its probe value.
  gameplay      the stream must DIFFER.

Both directions matter. Testing only the presentation direction would let a
gameplay-changing setting be mislabelled as cosmetic and slip through; testing
only the gameplay direction would let a setting that does nothing be labelled as
if it did.

Why the table comes from the binary
-----------------------------------
The rows are parsed from `[ENHTABLE]` lines the binary emits under
`MDKR_ENH_DUMP_TABLE=1`, including the probe value each row declares for itself.
Keeping either in this file would create a second list: add an enhancement,
forget to update the test, and the gate exercises one fewer setting while still
printing PASS. The count it verified is printed for the same reason — a shrinking
number is visible.

A zero-row parse is a FAILURE, not an empty pass. That is the specific way this
gate could go vacuous, and it is checked first.

What is deliberately not proven yet
-----------------------------------
See EXPECTED_INERT below. Enhancements whose effect is not implemented cannot
move the stream, so their `gameplay` claim cannot yet be true. They are listed
by name with the task that closes them rather than being quietly excluded, and
the gate FAILS if a name on that list starts working — an expectation that has
silently become wrong is worse than no expectation.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD = ROOT / "build" / "mdkr64"
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
FRAMES = 900
HASH_VERSION = "3"

# Rows whose effect is not built yet, so their declared class cannot be
# measured. Each names the task that closes it. The gate fails if one of these
# turns out to move the stream after all, because then the note is the stale
# thing rather than the code.
EXPECTED_INERT = {
    "Enhancements.Speedometer":  "S2 task 3",
    "Enhancements.DrawDistance": "S2 task 4",
    "Enhancements.LodBias":      "S2 task 4",
    "Enhancements.AIDifficulty": "S2 task 5",
}


def run(binary: Path, rom: Path, work: Path, label: str,
        overrides: list[str], verbose: bool) -> list[str]:
    """One headless race; returns its [SIMHASH] rows."""
    run_dir = work / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK="5",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
    )
    command = [
        str(binary), "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", "640x480",
    ]
    for override in overrides:
        command += ["--video-set", override]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    proc = subprocess.run(command, cwd=run_dir, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=900, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"{label}: exit {proc.returncode}\n"
                           f"{(proc.stdout or '')[-3000:]}")
    rows = [ln for ln in (proc.stdout or "").splitlines()
            if ln.startswith("[SIMHASH]")]
    if not rows:
        raise RuntimeError(f"{label}: no [SIMHASH] rows; the instrument did "
                           f"not arm, so nothing below would mean anything")
    return rows


def dump_table(binary: Path, rom: Path, work: Path,
               verbose: bool) -> list[dict[str, str]]:
    run_dir = work / "table"
    run_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_ENH_DUMP_TABLE="1",
               MDKR_RENDERER="gl")
    command = [str(binary), "--headless-frames", "2", "--rom", str(rom)]
    if verbose:
        print(f"$ (table) {' '.join(command)}", flush=True)
    proc = subprocess.run(command, cwd=run_dir, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=300, check=False)
    rows = []
    for line in (proc.stdout or "").splitlines():
        if not line.startswith("[ENHTABLE] "):
            continue
        row = {}
        for field in line[len("[ENHTABLE] "):].split():
            if "=" in field:
                name, _, value = field.partition("=")
                row[name] = value
        rows.append(row)
    return rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=str(DEFAULT_BUILD))
    ap.add_argument("--rom", default=str(ROOT / "baserom.us.v80.z64"))
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    # Resolve to absolute before anything else: every run below uses
    # cwd=run_dir, so a relative --build or --rom would be looked up under the
    # temporary directory and fail with a bare FileNotFoundError that reads
    # like a missing build rather than a path bug.
    binary = Path(args.build).resolve()
    if binary.is_dir():
        binary = binary / "mdkr64"
    rom = Path(args.rom).resolve()
    if not binary.exists():
        print(f"check_enhancement_authority: FAIL — no binary at {binary}")
        return 1
    if not rom.exists():
        print(f"check_enhancement_authority: FAIL — no ROM at {rom}")
        return 1

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="enh_authority_") as tmp:
        work = Path(tmp)

        table = dump_table(binary, rom, work, args.verbose)
        # The vacuity guard, first: a gate that iterates an empty list and
        # prints PASS is the failure mode this whole design is arranged around.
        if not table:
            print("check_enhancement_authority: FAIL — parsed zero [ENHTABLE] "
                  "rows. Either MDKR_ENH_DUMP_TABLE stopped working or the "
                  "registry is empty; either way this gate proves nothing.")
            return 1
        print(f"  parsed {len(table)} enhancement row(s) from the binary")

        for row in table:
            key = row.get("key", "?")
            authority = row.get("authority", "?")
            probe = row.get("probe", "")
            if not probe:
                failures.append(f"{key}: row declares no probe value")
                continue

            base = run(binary, rom, work, f"{key}-default", [], args.verbose)
            alt = run(binary, rom, work, f"{key}-probe",
                      [f"{key}={probe}"], args.verbose)
            identical = base == alt
            inert_note = EXPECTED_INERT.get(key)

            if inert_note is not None:
                if identical:
                    print(f"  {key:32s} {authority:12s} inert as expected "
                          f"(effect lands in {inert_note})")
                else:
                    failures.append(
                        f"{key}: listed in EXPECTED_INERT ({inert_note}) but "
                        f"its probe value moved the state stream. The effect "
                        f"landed — remove it from that list and let the real "
                        f"assertion run.")
                continue

            if authority == "presentation" and not identical:
                failures.append(
                    f"{key}: declared presentation, but setting it to "
                    f"'{probe}' changed the authoritative state stream. Either "
                    f"the effect reaches state it must not, or the row is "
                    f"mislabelled.")
            elif authority == "gameplay" and identical:
                failures.append(
                    f"{key}: declared gameplay, but setting it to '{probe}' "
                    f"left the state stream byte-identical. It does nothing.")
            else:
                print(f"  {key:32s} {authority:12s} ok")

    if failures:
        print("check_enhancement_authority: FAIL")
        for f in failures:
            print(f"  {f}")
        return 1
    print(f"check_enhancement_authority: PASS — {len(table)} row(s) verified, "
          f"{len(EXPECTED_INERT)} awaiting their effect")
    return 0


if __name__ == "__main__":
    sys.exit(main())
