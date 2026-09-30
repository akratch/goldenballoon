#!/usr/bin/env python3
"""Exercise normal Modern held-shot recovery under injected failures.

Uses the motion runner's isolated private save/config directories and pins a
known active-chain episode. Requires healthy publication after each injected
failure; failed-to-trigger injections cannot pass.
"""
import argparse
import re
from pathlib import Path

import check_camera_motion_quality as motion

FAULT_TICK = 2136
FAULTS = {
    "projection": "MDKR_TEST_CAMERA_PROJECTION_FAIL_TICK",
    "query": "MDKR_TEST_CAMERA_EXACT_INVALID_TICK",
}


def verify(output, kind):
    if kind not in FAULTS:
        raise ValueError("unknown fault")
    rows = output.splitlines()
    if not any(f"camera_held_chain_probe tick={FAULT_TICK - 1} " in row and
               "valid=1 " in row for row in rows):
        raise ValueError("no accepted held candidate before fault")
    published = [row for row in rows if row.startswith(
        f"camera_held_shot_published tick={FAULT_TICK - 1} ")]
    if len(published) != 1 or not re.fullmatch(
            rf"camera_held_shot_published tick={FAULT_TICK - 1} "
            r"slot=0 frame=\d+ active=1", published[0]):
        raise ValueError("no unambiguous active held-shot publication before fault")
    summaries = [row for row in rows if "camera_obstruction_observe summary" in row]
    if not summaries:
        raise ValueError("no publication evidence")
    for row in summaries:
        if not re.search(r"resolved=\{corrected=\d+ penetrated=0 invalid=0 degraded=0\}", row):
            raise ValueError("unsafe publication summary")
        if "projection_mismatches=0 " not in row:
            raise ValueError("projection mismatch")
    fault = [row for row in summaries if f"tick={FAULT_TICK} " in row]
    if len(fault) != 1:
        raise ValueError("missing or ambiguous fault-tick summary")
    if kind == "projection":
        details = [row for row in rows if
                   f"camera_obstruction_observe detail tick={FAULT_TICK} " in row]
        if len(details) != 1 or not re.search(
                r"resolve=\{status=4 valid=1 .* discontinuity=1 .* clearance=0 ", details[0]):
            raise ValueError("projection fault lacks validated fallback/cut")
        if not any(f"camera_held_chain_probe tick={FAULT_TICK + 1} " in row and
                   "continued=0 " in row for row in rows):
            raise ValueError("chain not retired across failed projection")
    else:
        marked = [row for row in summaries if re.search(r"probe_degraded=[1-9]", row)]
        if marked != fault:
            raise ValueError("invalid-query seam missing, misplaced or repeated")
        if any("target_hidden=0 " not in row for row in summaries):
            raise ValueError("query fault lost target visibility")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=motion.DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--only", choices=tuple(FAULTS))
    args = parser.parse_args()
    binary = Path(motion.resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if not binary.is_file() or not rom.is_file():
        parser.error("missing binary or ROM")
    for kind in (args.only,) if args.only else FAULTS:
        output = motion.run_route(motion.ROUTES[0], binary, rom, None, present_env={
            "MDKR_CAMERA_TRACE": "2",
            FAULTS[kind]: str(FAULT_TICK)})
        verify(output, kind)
        print(f"PASS held-shot {kind} fault: safe publication and required recovery", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
