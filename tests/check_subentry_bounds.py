#!/usr/bin/env python3
"""Negative control: an out-of-range asset sub-entry index aborts, loudly.

Section indices have always been bounds-checked; entry indices *inside* a
section were not, and the one check that existed -- ``get_misc_asset()`` --
compared the index against ``gAssetsMiscTableLength`` and then returned the
SECTION BASE when it failed. That is the silent shape: an out-of-range read
becomes plausible wrong data with no diagnostic. ``platform/asset_subentry.c``
replaces it with an abort naming the section, the requested index and the actual
count (S5 M1, docs/open-items/portability.md wave "subentry").

A guard nobody has ever seen fire is indistinguishable from a guard that does
not work, so this gate drives deliberately out-of-range indices through two
env-gated hooks and asserts the process **aborts** -- not segfaults, not exits
zero, not reads something plausible:

  MDKR_SUBENTRY_TEST_INDEX       synthetic one-entry section, driven from
                                 pi_init() (game/src/asset_loading.c). Proves
                                 the accessor itself.
  MDKR_SUBENTRY_TEST_MISC_INDEX  the REAL get_misc_asset() against the REAL
                                 ASSET_MISC section, driven from
                                 init_object_tables() (game/src/objects.c).
                                 Proves the replacement of the silent fallback
                                 is on the live path, with the ROM's own count.

The first arm is the positive control for the whole gate: with no hook set, the
same binary must run clean and print none of these diagnostics, so a build that
aborted unconditionally could not pass.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

DIAGNOSTIC = "[FATAL] asset sub-entry index out of range"
UINT32_MAX = 4294967295


def run(binary: Path, rom: Path, frames: int, timeout: int,
        extra_env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="mdkr_subentry_") as temp:
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_SAVE_DIR=str(Path(temp) / "save"),
            MDKR_VIDEO_CONFIG_PATH=str(Path(temp) / "missing.ini"),
        )
        env.update(extra_env)
        return subprocess.run(
            [str(binary), "--headless-frames", str(frames), "--rom", str(rom)],
            env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=timeout, check=False,
        )


def describe(code: int) -> str:
    if code < 0:
        try:
            return f"signal {signal.Signals(-code).name}"
        except ValueError:
            return f"signal {-code}"
    return f"exit {code}"


def expect_abort(label: str, process: subprocess.CompletedProcess[str],
                 needles: list[str], failures: list[str]) -> None:
    output = process.stdout or ""
    # SIGABRT specifically. A segfault would also be "non-zero", and a segfault
    # is precisely the outcome an unbounded read produces on a bad day -- if the
    # guard were absent, this arm could still "fail loudly" for the wrong reason.
    if process.returncode != -signal.SIGABRT:
        failures.append(
            f"{label}: expected SIGABRT, got {describe(process.returncode)}")
    if DIAGNOSTIC not in output:
        failures.append(f"{label}: no named diagnostic in the output")
    if "resolved without aborting" in output:
        failures.append(f"{label}: the index was in range; the control is vacuous")
    for needle in needles:
        if needle not in output:
            failures.append(f"{label}: diagnostic is missing {needle!r}")
    if failures:
        tail = "\n".join(output.splitlines()[-6:])
        if tail and not any(t.endswith(tail) for t in failures):
            failures.append(f"{label}: last output lines were:\n{tail}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=20)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    for path in (binary, rom):
        if not path.is_file():
            raise SystemExit(f"missing: {path}")

    failures: list[str] = []

    # --- positive control: the guard is invisible on a supported ROM ------
    clean = run(binary, rom, args.frames, args.timeout, {})
    clean_output = clean.stdout or ""
    if clean.returncode != 0:
        failures.append(
            f"unhooked run: expected exit 0, got {describe(clean.returncode)}")
    if DIAGNOSTIC in clean_output:
        failures.append("unhooked run: the sub-entry guard fired on us.v80")
    if "[SUBENTRY]" in clean_output:
        failures.append("unhooked run: a test hook ran without being asked to")

    # --- the accessor itself ---------------------------------------------
    expect_abort(
        "synthetic index 1 of a 1-entry section",
        run(binary, rom, args.frames, args.timeout,
            {"MDKR_SUBENTRY_TEST_INDEX": "1"}),
        ["section=MDKR_SELFTEST", "index=1", "count=1"],
        failures)

    expect_abort(
        "synthetic huge unsigned index",
        run(binary, rom, args.frames, args.timeout,
            {"MDKR_SUBENTRY_TEST_INDEX": str(UINT32_MAX)}),
        ["section=MDKR_SELFTEST", f"index={UINT32_MAX}", "count=1"],
        failures)

    # --- the live get_misc_asset() path, with the ROM's own count ---------
    misc = run(binary, rom, args.frames, args.timeout,
               {"MDKR_SUBENTRY_TEST_MISC_INDEX": "100000"})
    expect_abort("live get_misc_asset(100000)", misc,
                 ["section=ASSET_MISC", "index=100000"], failures)
    # The count must be the ROM's real ASSET_MISC entry count, not a placeholder
    # and not the index echoed back: a guard that reports count == index would
    # match every substring above while proving nothing.
    match = re.search(r"section=ASSET_MISC index=100000 count=(\d+)",
                      misc.stdout or "")
    if match is None:
        failures.append("live get_misc_asset: no ASSET_MISC count in the diagnostic")
    elif not 0 < int(match.group(1)) < 100000:
        failures.append(
            f"live get_misc_asset: implausible ASSET_MISC count {match.group(1)}")

    # A negative index is the other half of the old silent fallback
    # (`index < 0` returned the section base too). It converts to a huge
    # unsigned value and must be rejected by the same bound.
    expect_abort(
        "live get_misc_asset(-1)",
        run(binary, rom, args.frames, args.timeout,
            {"MDKR_SUBENTRY_TEST_MISC_INDEX": "-1"}),
        ["section=ASSET_MISC", f"index={UINT32_MAX}"],
        failures)

    if failures:
        print("check_subentry_bounds: FAIL", file=sys.stderr)
        for failure in failures[:20]:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("  unhooked run clean; 4 out-of-range indices aborted with the named "
          "diagnostic")
    print("check_subentry_bounds: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
