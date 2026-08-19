#!/usr/bin/env python3
"""More than six Time Trial ghost pairs must coexist in ONE save directory.

Why this exists
---------------
`tests/check_ghost_matrix.py` (its docstring, "The three runs per pair") gives
every one of the 47 legal (track, vehicle) pairs its own private
`MDKR_SAVE_DIR` precisely because "the 6-slot pak ghost directory
(`DKR_GHOST_SLOT_COUNT`, save_data.c) is never contended -- 47 ghosts do not
fit on one pak". That six-pair ceiling is the original ROM's own DKRACING-GHOSTS
file format, and on the port it is exactly what issue #46 reports: after six
distinct pairs, every further pair is refused with
CONTROLLER_PAK_NO_ROOM_FOR_GHOSTS, rendered as the generic "CONTROLLER PAK
FULL" dialog.

platform/ghost_bank.c removes the ceiling without touching the authored
format: before the game loads or saves a pair, the least-recently-used window
pair is swapped out to a per-pair bank file under <save>/ghost-bank/ and the
requested pair's banked record is swapped back in, byte for byte. This check
is the ghost-matrix counterpart for that machinery: it drives MORE pairs than
the window holds against ONE shared save directory and asserts the contention
the matrix deliberately avoids is now handled.

What is asserted
----------------
Driving mechanics (navigation prefix, cadence arms, options-frame measurement,
generated tap scripts, exit-code-first verdicts) are imported directly from
tests/check_ghost_matrix.py rather than re-derived, so the two checks cannot
drift apart.

  1. Every pair's ghost SAVE against the shared directory reports status 0.
     No [TTGHOST] row of any run -- load or save -- may ever report status 6
     (CONTROLLER_PAK_NO_ROOM_FOR_GHOSTS): that is the defect this gate exists
     for, and it fails loud on the first spurious occurrence.
  2. After all pairs are written, each pair loads back in a FRESH process
     against the same directory with the exact nodes/character/time it saved.
     With more pairs than slots, that is only possible if evicted pairs are
     restored from their bank files.
  3. Byte identity: the serialised record (GhostHeader + nodes) captured from
     the pak image right after each save must reappear VERBATIM in the pak
     image after the pair's read run. For every pair whose record was absent
     from the image before its read run (i.e. genuinely evicted), that is a
     byte-identical round trip through eviction and re-selection; at least
     pairs-minus-six such restorations must be observed, and the bank must
     say so itself ([GHOSTBANK] event=restore).
  4. The pak image never grows: the note keeps the authored 0x6700-byte
     window shape; only the live six can be resident at once.

Usage:
    tests/check_ghost_bank_capacity.py                # default 8 pairs
    tests/check_ghost_bank_capacity.py --pairs 10
    tests/check_ghost_bank_capacity.py --list         # print the pool used

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md. Exit 0 = every pair saved, reloaded, and round-tripped.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import itertools
import os
import shutil
import sys
import tempfile
import time

from check_ghost_matrix import (BUDGET, CADENCE_ARMS, CONTROLLER_PAK_GOOD,
                                NO_GHOST_PAIRS, PAK_IMAGE, READ_BUDGET,
                                find_ghost_header, generate_script, run_child)
from check_vehicle_sweep import MAIN_TRACK_IDS, VEHICLE_NAMES, rom_vehicle_matrix
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

# save_data.h SIDeviceStatus: the refusal this whole gate exists to abolish.
CONTROLLER_PAK_NO_ROOM_FOR_GHOSTS = 6

GHOST_HEADER_BYTES = 8
GHOST_NODE_BYTES = 12


def no_room_rows(run) -> list[tuple]:
    """Every [TTGHOST] row that reported NO_ROOM_FOR_GHOSTS."""
    return [e for e in run.io if e[4] == CONTROLLER_PAK_NO_ROOM_FOR_GHOSTS]


def pak_bytes(save_dir: str) -> bytes:
    path = os.path.join(save_dir, PAK_IMAGE)
    if not os.path.exists(path):
        return b""
    with open(path, "rb") as handle:
        return handle.read()


def measure_pair(binary: str, rom: str, level: int, vehicle: int,
                 workdir: str, verbose: bool):
    """Find the cadence arm that laps this pair and its SAVE GHOST frame.

    Same protocol as check_ghost_matrix.check_pair's measure phase, against a
    private save directory (the deterministic re-drive on the shared
    directory could never beat its own banked time, so SAVE GHOST would not
    be offered there).
    Returns (arm, options_frame, save_index, failures, detail).
    """
    tag = f"{level}_{vehicle}"
    save_dir = os.path.join(workdir, f"measure_{tag}")
    script = os.path.join(workdir, f"measure_{tag}.txt")
    generate_script(script, BUDGET)
    attempts = []
    for arm in CADENCE_ARMS:
        shutil.rmtree(save_dir, ignore_errors=True)
        attempt = run_child(binary, rom, script, save_dir, level, vehicle,
                            BUDGET, verbose, arm)
        died = attempt.fatal(f"measure[{arm}]")
        if died:
            return None, 0, 0, died, "died"
        if no_room := no_room_rows(attempt):
            return None, 0, 0, [
                f"measure[{arm}]: spurious NO_ROOM_FOR_GHOSTS on a FRESH "
                f"save directory: {no_room[0]}"], "no-room on fresh dir"
        attempts.append((arm, attempt))
        if attempt.finished:
            offered = [o for o in attempt.options if o[3] == 1]
            if not offered:
                return None, 0, 0, [
                    f"measure[{arm}]: finished but SAVE GHOST never offered"
                ], "no SAVE GHOST"
            frame, _opt, count, _has = offered[0]
            if count - 2 < 1:
                return None, 0, 0, [
                    f"measure[{arm}]: option count {count} leaves no SAVE "
                    "GHOST row"], "bad option list"
            return arm, frame, count - 2, [], ""
    laps = ", ".join(f"{a}: rlap={r.max_rlap}" for a, r in attempts)
    return None, 0, 0, None, f"did not finish ({laps})"


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--pairs", type=int, default=8,
                        help="distinct (track, vehicle) pairs to bank into one "
                             "save directory; must exceed the 6-slot window")
    parser.add_argument("--list", action="store_true",
                        help="print the candidate pool and exit 0")
    parser.add_argument(
        "--jobs", type=int, default=4,
        help="concurrent MEASURE runs (private save directories). The write "
             "and read phases stay strictly sequential: contending for the "
             "one shared save directory is this check's entire subject, and "
             "its assertions depend on their order.")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if args.pairs < 7:
        print("FAIL: --pairs must exceed the 6-slot window, or nothing here "
              "is being tested")
        return 1

    binary = resolve_binary(args.build)
    for path in (binary, args.rom):
        if not os.path.exists(path):
            print(f"FAIL: {path} not found -- build first")
            return 1

    rom_matrix = rom_vehicle_matrix(args.rom)
    pool = [(level, vehicle) for level in MAIN_TRACK_IDS
            for vehicle in sorted(VEHICLE_NAMES)
            if (rom_matrix[level][1] >> vehicle) & 1
            and (level, vehicle) not in NO_GHOST_PAIRS]
    if args.list:
        for level, vehicle in pool:
            print(f"  level {level:<3} {VEHICLE_NAMES[vehicle]}")
        print(f"{len(pool)} candidate pair(s)")
        return 0

    print(f"check_ghost_bank_capacity: banking {args.pairs} pairs into ONE "
          f"save directory (window holds 6)")

    failures: list[str] = []
    workdir = tempfile.mkdtemp(prefix="mdkr64-ghost-bank-")
    shared = os.path.join(workdir, "save_shared")
    started = time.time()
    try:
        # --- measure: find each candidate's arm and SAVE GHOST frame -------
        chosen: list[tuple[int, int, object, int, int]] = []
        candidates = iter(pool)
        pending = list(itertools.islice(candidates, args.pairs))
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=max(1, args.jobs)) as executor:
            while pending and len(chosen) < args.pairs:
                results = list(executor.map(
                    lambda pair: (pair, measure_pair(
                        binary, args.rom, pair[0], pair[1], workdir,
                        args.verbose)),
                    pending))
                pending = []
                for (level, vehicle), (arm, frame, index, died,
                                       detail) in results:
                    if died:
                        failures.extend(
                            f"[measure {level}:{VEHICLE_NAMES[vehicle]}] {d}"
                            for d in died)
                        continue
                    if arm is None:
                        # A pacing non-finisher is the matrix's subject, not
                        # this gate's; pull the next candidate instead.
                        print(f"  level {level:<3} {VEHICLE_NAMES[vehicle]:<11}"
                              f" note   {detail}; substituting")
                        try:
                            pending.append(next(candidates))
                        except StopIteration:
                            pass
                        continue
                    chosen.append((level, vehicle, arm, frame, index))
        if failures:
            raise SystemExit  # reported below
        if len(chosen) < args.pairs:
            failures.append(
                f"only {len(chosen)} of the requested {args.pairs} pairs "
                "could be measured; the pool is exhausted")
            raise SystemExit
        chosen = chosen[:args.pairs]

        # --- write: save every pair's ghost into the SHARED directory ------
        written: dict[tuple[int, int], tuple[int, int, int, bytes]] = {}
        for level, vehicle, arm, frame, index in chosen:
            tag = f"{level}_{vehicle}"
            script = os.path.join(workdir, f"write_{tag}.txt")
            generate_script(script, BUDGET, options_frame=frame,
                            option_index=index)
            run = run_child(binary, args.rom, script, shared, level, vehicle,
                            BUDGET, args.verbose, arm)
            problems = run.fatal(f"write {tag}")
            for row in no_room_rows(run):
                problems.append(
                    f"write {tag}: NO_ROOM_FOR_GHOSTS surfaced with the bank "
                    f"active: {row} -- issue #46 is back")
            saves = run.events("save")
            if not saves:
                problems.append(
                    f"write {tag}: timetrial_save_player_ghost() never ran")
            else:
                _f, _k, s_level, s_vehicle, s_status, s_nodes, s_char, \
                    s_time = saves[-1]
                if s_status != CONTROLLER_PAK_GOOD:
                    problems.append(
                        f"write {tag}: save status {s_status}, expected "
                        f"{CONTROLLER_PAK_GOOD} -- pair "
                        f"{len(written) + 1} of {args.pairs} was refused")
                elif (s_level, s_vehicle) != (level, vehicle):
                    problems.append(
                        f"write {tag}: saved under ({s_level}, {s_vehicle})")
                elif s_nodes <= 0:
                    problems.append(f"write {tag}: empty payload ({s_nodes})")
                else:
                    image = pak_bytes(shared)
                    at = find_ghost_header(image, s_char, s_time, s_nodes)
                    if at < 0:
                        problems.append(
                            f"write {tag}: serialised GhostHeader not in "
                            f"{PAK_IMAGE} after the save")
                    else:
                        start = at - 2  # the header begins at its checksum
                        record = image[start:start + GHOST_HEADER_BYTES +
                                       s_nodes * GHOST_NODE_BYTES]
                        written[(level, vehicle)] = (s_nodes, s_char, s_time,
                                                     record)
                        # The pak container must keep its authored
                        # real-hardware-faithful size (MDKR_VPAK_IMAGE_SIZE,
                        # platform/virtual_pak.h): the extra pairs live in
                        # the bank, never in a grown pak.
                        if len(image) != 32192:
                            problems.append(
                                f"write {tag}: {PAK_IMAGE} is {len(image)} "
                                "bytes, not the authored 32192")
            state = "ok" if not problems else "FAIL"
            print(f"  write level {level:<3} {VEHICLE_NAMES[vehicle]:<11} "
                  f"{state:<6} {run.seconds:5.1f}s")
            failures.extend(problems)
        if failures:
            raise SystemExit
        if len(written) != args.pairs:
            failures.append(
                f"only {len(written)} of {args.pairs} pairs produced a "
                "verifiable pak record")
            raise SystemExit

        # --- read: every pair must come back from the ONE directory --------
        restored = 0
        bank_restores = 0
        for level, vehicle, arm, _frame, _index in chosen:
            tag = f"{level}_{vehicle}"
            s_nodes, s_char, s_time, record = written[(level, vehicle)]
            evicted = pak_bytes(shared).find(record) < 0
            script = os.path.join(workdir, f"read_{tag}.txt")
            generate_script(script, READ_BUDGET)
            run = run_child(binary, args.rom, script, shared, level, vehicle,
                            READ_BUDGET, args.verbose, arm)
            problems = run.fatal(f"read {tag}")
            for row in no_room_rows(run):
                problems.append(f"read {tag}: NO_ROOM_FOR_GHOSTS: {row}")
            loads = [e for e in run.events("load") if e[5] >= 0]
            if not loads:
                problems.append(
                    f"read {tag}: no payload-reporting [TTGHOST] event=load; "
                    "the pair never came back from the shared directory")
            else:
                _f, _k, l_level, l_vehicle, l_status, l_nodes, l_char, \
                    l_time = loads[0]
                if l_status != CONTROLLER_PAK_GOOD:
                    problems.append(
                        f"read {tag}: load status {l_status} -- the banked "
                        "ghost was not restored")
                if (l_level, l_vehicle, l_nodes, l_char, l_time) != \
                        (level, vehicle, s_nodes, s_char, s_time):
                    problems.append(
                        f"read {tag}: got ({l_level}:{l_vehicle} nodes="
                        f"{l_nodes} char={l_char} time={l_time}), wrote "
                        f"({level}:{vehicle} nodes={s_nodes} char={s_char} "
                        f"time={s_time})")
            after = pak_bytes(shared)
            if after.find(record) < 0:
                problems.append(
                    f"read {tag}: the record saved for this pair is not in "
                    f"{PAK_IMAGE} after its own read run -- re-selection did "
                    "not restore the saved bytes")
            elif evicted:
                restored += 1
                if "event=restore" not in run.out:
                    problems.append(
                        f"read {tag}: the record reappeared but the bank "
                        "never traced [GHOSTBANK] event=restore -- whatever "
                        "put it back was not the bank")
                else:
                    bank_restores += 1
            state = "ok" if not problems else "FAIL"
            print(f"  read  level {level:<3} {VEHICLE_NAMES[vehicle]:<11} "
                  f"{state:<6} {run.seconds:5.1f}s"
                  f"{'  [restored from bank]' if evicted and not problems else ''}")
            failures.extend(problems)

        minimum = args.pairs - 6
        if restored < minimum:
            failures.append(
                f"only {restored} pair(s) were ever evicted-and-restored, "
                f"but {args.pairs} pairs in a 6-slot window force at least "
                f"{minimum}; the byte-identity claim was never exercised")
    except SystemExit:
        pass
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    total = time.time() - started
    if failures:
        print()
        for failure in failures:
            print(f"FAIL: {failure}")
        print(f"\ncheck_ghost_bank_capacity: FAIL ({len(failures)} problem(s), "
              f"{total / 60:.1f} min)")
        return 1
    print(f"\ncheck_ghost_bank_capacity: PASS -- {args.pairs} pairs in one "
          f"save directory, every save and reload clean, {restored} "
          f"byte-identical eviction round-trip(s) ({bank_restores} traced by "
          f"the bank), {total / 60:.1f} min")
    return 0


if __name__ == "__main__":
    sys.exit(main())
