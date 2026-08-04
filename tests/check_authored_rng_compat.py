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

from harness_utils import resolve_binary

ROOT = Path(__file__).resolve().parent.parent
ROUTE_TOOL = ROOT / "tools" / "dkr_oracle_route.py"
FRAMES = 4800
REFERENCE_COMMIT = "64936e36b4c9ef7ecdce5beb93cd662d4318548d"
EXPECTED_ROWS = 27_840
EXPECTED_SHA256 = "d74efe02aec07aa59710ce457e54180c28a22022f3d35e7087096d5130dba49b"
ARES_VEHICLE_RNG_PREFIX_SHA256 = (
    "9fd7cb9aebc163b00f9c8e4bfd292f90b684b4d46415ab5e0ef594c8bfb2d16e"
)
FIELDS = (
    "frame", "map", "slot", "x", "y", "z", "xv", "yv", "zv", "fvel",
    "vel", "cp", "next", "lap", "countlap", "fin", "fpos", "ridx",
    "pidx", "vehicle", "grounded", "clock", "start", "delta", "rate", "rng",
)
FLOAT_FIELDS = {"x", "y", "z", "xv", "yv", "zv", "fvel", "vel"}


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
    parser.add_argument("--build", default="build-rel")
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
        f"ares car-RNG witness {ARES_VEHICLE_RNG_PREFIX_SHA256[:12]})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
