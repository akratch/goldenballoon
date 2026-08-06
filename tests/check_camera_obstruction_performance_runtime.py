#!/usr/bin/env python3
"""4P fixed-tick camera cost, query fan, memory, and authority gate."""

from __future__ import annotations

import argparse
import math
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, find_fatal,
                           resolve_binary)


SCRIPT = Path("tests/input_scripts/race_4p_split.txt")
PERF_RE = re.compile(r"^\[CAMERAPERF\] section=(\w+) (.*)$", re.MULTILINE)
RUN_RE = re.compile(r"^\[CAMERAPERF-RUN\] (.*)$", re.MULTILINE)
CACHE_RE = re.compile(
    r"^\[CAM-OCCLUSION\].*\bbytes=(\d+) build_ns=(\d+)$", re.MULTILINE)
P99_BUDGET_NS = 833_333       # 5% of a fastest-cadence 60 Hz fixed tick
TAIL_BUDGET_NS = 1_666_667    # 10% diagnostic tail threshold
MIN_ACTIVE_4P_TICKS = 5_000   # 83 seconds at the fastest 60 Hz cadence


@dataclass(frozen=True)
class Result:
    metrics: dict[str, dict[str, int]]
    selected: dict[str, int]
    state: tuple[str, ...]
    max_cache_bytes: int
    max_cache_build_ns: int


def fields(text: str) -> dict[str, int]:
    parsed: dict[str, int] = {}
    for token in text.split():
        key, separator, value = token.partition("=")
        if separator:
            parsed[key] = int(value)
    return parsed


def run(binary: Path, rom: Path, root: Path, policy: str,
        frames: int, timeout: int) -> Result:
    run_root = root / policy
    run_root.mkdir()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_CAMERA_OBSTRUCTION=policy,
        MDKR_CAMERA_PERF="1",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(run_root / "save"),
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_STATE_HASH="3",
        MDKR_SYNTH_FIELDS="1",
        MDKR_VIDEO_CONFIG_PATH=str(run_root / "missing.ini"),
    )
    process = subprocess.run(
        [str(binary), "--headless-frames", str(frames),
         "--window-size", "1920x1080", "--input-script", str(SCRIPT.resolve()),
         "--rom", str(rom)],
        cwd=run_root, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(f"{policy}: exit {process.returncode}\n{output[-4000:]}")
    marker = find_fatal(output, *ASSERT_MARKERS)
    if marker:
        raise RuntimeError(f"{policy}: emitted {marker}")
    metrics = {match.group(1): fields(match.group(2))
               for match in PERF_RE.finditer(output)}
    run_match = RUN_RE.search(output)
    required_sections = {
        "finalizer", "slot", "static_query", "dynamic_query",
        "dynamic_publication", "exact_static_query", "exact_dynamic_query",
    }
    if set(metrics) != required_sections or run_match is None:
        raise RuntimeError(f"{policy}: incomplete performance summary")
    cache_rows = [(int(byte_count), int(build_ns))
                  for byte_count, build_ns in CACHE_RE.findall(output)]
    return Result(
        metrics=metrics,
        selected=fields(run_match.group(1)),
        state=tuple(row for row in output.splitlines() if row.startswith("[SIMHASH]")),
        max_cache_bytes=max((row[0] for row in cache_rows), default=0),
        max_cache_build_ns=max((row[1] for row in cache_rows), default=0),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=12500)
    parser.add_argument("--timeout", type=int, default=900)
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    for path in (binary, rom, SCRIPT):
        if not path.is_file():
            raise SystemExit(f"missing: {path}")

    failures: list[str] = []
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr_camera_perf_") as temp:
            root = Path(temp)
            observe = run(binary, rom, root, "observe", args.frames, args.timeout)
            modern = run(binary, rom, root, "modern", args.frames, args.timeout)
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"check_camera_obstruction_performance_runtime: FAIL\n  - {error}",
              file=sys.stderr)
        return 1

    finalizer = modern.metrics["finalizer"]
    static = modern.metrics["static_query"]
    dynamic = modern.metrics["dynamic_query"]
    publication = modern.metrics["dynamic_publication"]
    exact_static = modern.metrics["exact_static_query"]
    exact_dynamic = modern.metrics["exact_dynamic_query"]
    if modern.selected.get("selected4", 0) < MIN_ACTIVE_4P_TICKS:
        failures.append(
            f"4P active coverage too short: {modern.selected.get('selected4', 0)} ticks")
    if finalizer.get("hits", 0) < args.frames - 100:
        failures.append(f"incomplete finalizer census: {finalizer.get('hits', 0)}")
    if finalizer.get("p99_ns", P99_BUDGET_NS + 1) > P99_BUDGET_NS:
        failures.append(
            f"finalizer p99 {finalizer.get('p99_ns')}ns exceeds {P99_BUDGET_NS}ns")
    allowed_p99_tail = math.ceil(finalizer.get("hits", 0) * 0.01)
    if finalizer.get("over_833333ns", allowed_p99_tail + 1) > allowed_p99_tail:
        failures.append("more than 1% of finalizers exceed the 5% fixed-tick budget")
    allowed_long_tail = max(1, finalizer.get("hits", 0) // 1000)
    if finalizer.get("over_1666667ns", allowed_long_tail + 1) > allowed_long_tail:
        failures.append("more than 0.1% of finalizers exceed the 10% tail threshold")
    if finalizer.get("overflow", 1) != 0:
        failures.append("finalizer timing histogram overflowed")
    for name, metric in (("static", static), ("dynamic", dynamic),
                         ("publication", publication),
                         ("exact static", exact_static),
                         ("exact dynamic", exact_dynamic)):
        if metric.get("p99_ns", 100_001) > 100_000:
            failures.append(f"{name} p99 {metric.get('p99_ns')}ns exceeds 100000ns")
    # A section that was never entered reports p99 = 0 and passes the ceiling
    # above, so without a floor an all-zero clock is the best possible result.
    # The 4P Modern route enters each of these every fixed tick.
    for name, metric in (("slot", modern.metrics["slot"]),
                         ("static query", static), ("dynamic query", dynamic),
                         ("dynamic publication", publication)):
        if metric.get("hits", 0) <= 0:
            failures.append(f"{name} section was never measured")
    if sum(metric.get("total_ns", 0) for metric in modern.metrics.values()) <= 0:
        failures.append("camera performance clock measured no elapsed time")
    if static.get("hits", 0) > finalizer.get("hits", 0) * 64:
        failures.append("static query fan exceeded 64 calls per fixed tick")
    if exact_static.get("hits", 0) == 0 or exact_dynamic.get("hits", 0) == 0:
        failures.append("authoritative exact source census was absent")
    if exact_static.get("hits", 0) > finalizer.get("hits", 0) * 64:
        failures.append("exact static query fan exceeded 64 calls per fixed tick")
    if observe.state != modern.state or len(modern.state) != args.frames:
        failures.append(
            f"Observe/Modern authority streams differ or are incomplete: "
            f"{len(observe.state)}/{len(modern.state)}")
    if modern.max_cache_bytes == 0 or modern.max_cache_build_ns == 0:
        failures.append("camera track-cache memory/build census was absent")
    if modern.selected.get("dynamic_bytes", 0) == 0:
        failures.append("dynamic camera allocation census was absent")

    print(
        "  finalizer="
        f"p50 {finalizer.get('p50_ns')}ns / p95 {finalizer.get('p95_ns')}ns / "
        f"p99 {finalizer.get('p99_ns')}ns / max {finalizer.get('max_ns')}ns; "
        f"4P ticks={modern.selected.get('selected4', 0)}; "
        f"max track cache={modern.max_cache_bytes} bytes / "
        f"{modern.max_cache_build_ns}ns build; "
        f"Observe p99={observe.metrics['finalizer'].get('p99_ns')}ns"
    )
    if failures:
        print("check_camera_obstruction_performance_runtime: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check_camera_obstruction_performance_runtime: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
