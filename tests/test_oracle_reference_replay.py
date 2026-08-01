#!/usr/bin/env python3
"""ROM-free contracts for the independent-oracle replay compiler."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "oracle_reference_replay.py"
sys.path.insert(0, str(ROOT))

from tools.compare_oracle_state import State, by_clock, normalize_timebase  # noqa: E402


HEADER = (
    "frame,valid,racer_index,logic_update_rate,framebuffer_serial,"
    "input_buttons,stick_x,stick_y\n"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_tool(work: Path, trace_text: str) -> subprocess.CompletedProcess[str]:
    trace = work / "ares.csv"
    base = work / "base.txt"
    trace.write_text(trace_text, encoding="utf-8")
    base.write_text(
        "# fixture\n1 START 3\n8 A 10\n12 B 2\n", encoding="utf-8"
    )
    return subprocess.run(
        [
            sys.executable,
            str(TOOL),
            "--ares-trace", str(trace),
            "--base-native-input", str(base),
            "--fields-out", str(work / "fields.txt"),
            "--input-out", str(work / "input.txt"),
            "--ares-frame-start", "100",
            "--field-start", "7",
            "--input-start", "9",
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def check_compile() -> None:
    with tempfile.TemporaryDirectory(prefix="mdkr-oracle-replay-") as raw:
        work = Path(raw)
        result = run_tool(
            work,
            HEADER
            + "99,1,0,2,9,0x8000,0,0\n"
            + "100,1,0,3,10,0x8000,0,0\n"
            + "101,1,0,3,10,0x8200,-80,0\n"
            + "102,1,0,2,11,0x8000,0,0\n",
        )
        require(result.returncode == 0, result.stdout)
        fields = (work / "fields.txt").read_text(encoding="utf-8")
        inputs = (work / "input.txt").read_text(encoding="utf-8")
        require("\n7 3\n8 2\n" in fields, "field schedule did not collapse serials")
        require("\n8 A 1\n" in inputs, "base input was not clipped at handoff")
        require("\n9 A+LEFT 1\n10 A 1\n" in inputs, "observed input RLE is wrong")
        require("12 B" not in inputs, "post-handoff base input leaked into replay")
        require("Source ares trace SHA-256" in fields, "source provenance missing")


def check_fail_closed() -> None:
    with tempfile.TemporaryDirectory(prefix="mdkr-oracle-replay-bad-") as raw:
        work = Path(raw)
        result = run_tool(
            work,
            HEADER + "100,1,0,2,10,0x0000,-40,0\n",
        )
        require(result.returncode != 0, "unrepresentable analog input was accepted")
        require("cannot represent analog state" in result.stdout, result.stdout)

        result = run_tool(
            work,
            HEADER
            + "100,1,0,2,10,0x8000,0,0\n"
            + "101,1,0,2,12,0x8000,0,0\n",
        )
        require(result.returncode != 0, "framebuffer serial gap was accepted")
        require("framebuffer serial gap" in result.stdout, result.stdout)


def check_stable_ares_phase() -> None:
    rows = [
        State(10, 1, 1.0, 0.0, 0.0, 0, 0, framebuffer_serial=1),
        State(11, 2, 999.0, 0.0, 0.0, 0, 0, framebuffer_serial=1),
        State(12, 2, 2.0, 0.0, 0.0, 0, 0, framebuffer_serial=2),
    ]
    normalized, basis = normalize_timebase(rows)
    require(basis == "lap_times", "fixture did not use lap clock")
    clocks = by_clock(normalized)
    require(clocks[1].x == 1.0, "first framebuffer observation was not retained")
    require(clocks[2].x == 2.0, "stable later framebuffer was not selected by clock")


def main() -> int:
    check_compile()
    check_fail_closed()
    check_stable_ares_phase()
    print("oracle reference replay: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
