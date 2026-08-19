#!/usr/bin/env python3
"""Native high-rate policies cannot starve the authored game/audio thread.

Both real GPU backends run the same visible, finite, synthetic-uncapped route
with motion smoothing off. WebGPU must poll queue completion without
synchronously draining the device on the cooperative game/audio thread. If the
synthetic schedule outruns the device, authored attempts may be explicitly held
at the two-frame admission boundary; every hold must be classified and
accounted. OpenGL interval 0 retains completion fences. This is live backend
evidence, not a headless scheduler-only model.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (completed_tick_conservation, DEFAULT_BUILD_DIR,
                           fatal_re, GPU_MARKERS, parse_last,
                           present_mode_rows, resolve_binary)


TICKS = 30
EXPECTED_OPPORTUNITIES = TICKS * 1000 // 30
FATAL_RE = fatal_re(*GPU_MARKERS)


@dataclass(frozen=True)
class Run:
    backend: str
    output: str
    summary: dict[str, int]
    pressure: dict[str, int]


def clean_environment(**updates: str) -> dict[str, str]:
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(updates)
    return env


def run_backend(binary: Path, rom: Path, backend: str | None,
                timeout: int, verbose: bool) -> Run:
    label = backend or "default"
    expected_backend = backend or "webgpu"
    with tempfile.TemporaryDirectory(
            prefix=f"mdkr64_{label}_backpressure_") as temp:
        root = Path(temp)
        env = clean_environment(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_LOAD_TRACK="5",
            MDKR_PRESENT_RATE="uncapped",
            MDKR_PRESENT_SCHED_TRACE="1",
            MDKR_SAVE_DIR=str(root / "save"),
            # Isolate the video config with the save (see check_door_blocks.py).
            MDKR_VIDEO_CONFIG_PATH=str(root / "save" / "video.ini"),
            MDKR_TEST_VISIBLE_HEADLESS="1",
        )
        if backend is not None:
            env["MDKR_RENDERER"] = backend
        command = [
            str(binary), "--headless-ticks", str(TICKS),
            "--video-launch-set", "Video.RenderScale=1",
            "--window-size", "320x240", "--rom", str(rom),
        ]
        if verbose:
            print(f"$ ({label}) {' '.join(command)}", flush=True)
        process = subprocess.run(
            command, cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
        output = process.stdout or ""
        if process.returncode != 0:
            raise RuntimeError(
                f"{label}: exit {process.returncode}\n{output[-4000:]}")
        fatal = FATAL_RE.search(output)
        if fatal:
            raise RuntimeError(f"{label}: emitted {fatal.group(0)!r}")
        if f"[mdkr64] renderer backend: {expected_backend}" not in output:
            raise RuntimeError(
                f"{label}: expected {expected_backend} backend was not active")
        return Run(
            expected_backend, output,
            parse_last(output, "PRESENTSCHED-SUMMARY",
                       label=f"{label} scheduler", expect_one=True),
            parse_last(
                output,
                "WGPU-BACKPRESSURE" if expected_backend == "webgpu"
                else "GL-BACKPRESSURE",
                label=f"{label} backpressure", expect_one=True),
        )


def validate(run: Run) -> list[str]:
    failures: list[str] = []
    summary = run.summary
    pressure = run.pressure
    conservation_error = completed_tick_conservation(summary, TICKS,
                                                      run.backend)
    if conservation_error:
        failures.append(conservation_error)
    for key, expected in (
            ("entries", EXPECTED_OPPORTUNITIES),
            ("presents", EXPECTED_OPPORTUNITIES),
            ("presentkind", 3), ("requestedkind", 3)):
        if summary.get(key) != expected:
            failures.append(
                f"{run.backend}: {key}={summary.get(key)}, expected {expected}")
    for key in ("multidue", "lag", "catchup", "skips",
                "rebases", "blocked", "updatebad"):
        if summary.get(key, 0) != 0:
            failures.append(
                f"{run.backend}: scheduler {key}={summary.get(key)}")

    high_water = pressure.get("highwater", 0)
    submitted = pressure.get("submitted", 0)
    completed = pressure.get("completed", -1)
    # Smoothing is off, so only authored tasks attempt GPU admission. GL waits
    # on its bounded fences; WebGPU may hold an authored image rather than block
    # the cooperative game/audio thread. Startup can legitimately have up to
    # three ticks without a graphics task.
    endpoint_skips = pressure.get("endpointSkips", 0)
    accounted_authored = submitted + (
        endpoint_skips if run.backend == "webgpu" else 0)
    if not (TICKS - 3 <= accounted_authored <= TICKS):
        failures.append(
            f"{run.backend}: {submitted} submissions + {endpoint_skips} "
            f"endpoint holds for {TICKS} authored ticks")
    if completed != submitted:
        failures.append(
            f"{run.backend}: completed={completed}, submitted={submitted}")
    for key in ("inflight", "failures"):
        if pressure.get(key) != 0:
            failures.append(
                f"{run.backend}: {key}={pressure.get(key)}, expected 0")
    if pressure.get("polls", 0) < 1:
        failures.append(f"{run.backend}: completion polling never ran")
    if pressure.get("rateMilliHz", 0) <= 0:
        failures.append(f"{run.backend}: achieved submit rate was not measured")

    if run.backend == "webgpu":
        if not (1 <= high_water <= submitted):
            failures.append(
                f"webgpu: invalid in-flight high-water {high_water} for "
                f"{submitted} submissions")
        if pressure.get("runtimewaits", -1) != 0 or \
                pressure.get("runtimewaitns", -1) != 0:
            failures.append(
                "webgpu: production frame path synchronously waited for GPU "
                f"completion: {pressure.get('runtimewaits')} waits / "
                f"{pressure.get('runtimewaitns')} ns")
        if (pressure.get("skips", -1) != endpoint_skips or
                pressure.get("replaySkips", -1) != 0):
            failures.append(
                "webgpu: smoothing-off admission skips are not exclusively "
                f"authored endpoints: {pressure}")
        if pressure.get("abandoned") != 0:
            failures.append(
                f"webgpu: abandoned {pressure.get('abandoned')} completions")
        if not any(row.get("policy") == "uncapped" and row.get("tearing") == "0"
                   and row.get("requested") == "mailbox"
                   and row.get("effective") in ("mailbox", "fifo")
                   for row in present_mode_rows(run.output)
                   if row.get("backend") == "webgpu"):
            failures.append("webgpu: missing effective uncapped present mode")
        if not re.search(
                r"\[WGPU-SHUTDOWN\].*liveChildren=0.*cpuArrays=0",
                run.output):
            failures.append("webgpu: child-resource shutdown did not reach zero")
    else:
        if pressure.get("cap") != 2 or not (1 <= high_water <= 2):
            failures.append(
                f"gl: in-flight high-water {high_water}/"
                f"{pressure.get('cap')} is outside the two-fence bound")
        if pressure.get("waits", 0) < 1:
            failures.append("gl: completion fence wait path never ran")
        if pressure.get("effectiveSwap") != 0:
            failures.append(
                f"gl: effectiveSwap={pressure.get('effectiveSwap')}, expected 0")
        if not any(row.get("policy") == "uncapped"
                   and row.get("requestedSwap") == "0"
                   and row.get("effectiveSwap") == "0"
                   and row.get("supported") == "1"
                   for row in present_mode_rows(run.output)
                   if row.get("backend") == "gl"):
            failures.append("gl: interval-0 policy was not applied")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom):
        if not path.is_file():
            print(f"check_gpu_backpressure: FAIL\n  - missing {path}")
            return 1

    failures: list[str] = []
    runs: list[Run] = []
    try:
        for backend in ("webgpu", "gl"):
            run = run_backend(
                binary, rom, backend, args.timeout, args.verbose)
            runs.append(run)
            failures.extend(validate(run))
        # No selector means the production native default. Keep this in the
        # live GPU gate so a future backend-policy edit cannot silently change
        # the renderer players receive without validating its completion and
        # nonblocking-runtime contract.
        default_run = run_backend(
            binary, rom, None, args.timeout, args.verbose)
        failures.extend(validate(default_run))
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        failures.append(str(error))

    if failures:
        print("check_gpu_backpressure: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_gpu_backpressure: PASS -- production submits authored images "
          "without a runtime queue drain; completion polling remains live")
    for run in runs:
        pressure = run.pressure
        print(
            f"  - {run.backend}: {pressure['submitted']} submitted, "
            f"high-water {pressure['highwater']}/{pressure['cap']}, "
            f"{pressure['waits']} waits, "
            f"{pressure['rateMilliHz'] / 1000:.1f} submits/s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
