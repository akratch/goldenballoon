#!/usr/bin/env python3
"""Native uncapped GL/WebGPU production is explicitly queue-bounded.

Both real GPU backends run the same visible, finite, synthetic-uncapped route.
WebGPU must retire queue-completion callbacks with no more than two submitted
frames outstanding. OpenGL interval 0 must use completion fences with the same
bound. This is live backend evidence, not a headless scheduler-only model.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import resolve_binary


TICKS = 30
EXPECTED_PRESENTS = TICKS * 1000 // 30
FIELDS_RE = re.compile(r"\[PRESENTSCHED-SUMMARY\] (.*)")
WGPU_RE = re.compile(r"\[WGPU-BACKPRESSURE\] (.*)")
GL_RE = re.compile(r"\[GL-BACKPRESSURE\] (.*)")
FATAL_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|frame queue failed|GPU completion fence failed"
)


@dataclass(frozen=True)
class Run:
    backend: str
    output: str
    summary: dict[str, int]
    pressure: dict[str, int]


def parse_last(output: str, pattern: re.Pattern[str], label: str) -> dict[str, int]:
    rows = list(pattern.finditer(output))
    if len(rows) != 1:
        raise RuntimeError(f"{label}: expected one telemetry row, got {len(rows)}")
    fields: dict[str, int] = {}
    for token in rows[0].group(1).split():
        key, separator, value = token.partition("=")
        if not separator:
            continue
        try:
            fields[key] = int(value)
        except ValueError:
            continue
    return fields


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
    expected_backend = backend or "gl"
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
            parse_last(output, FIELDS_RE, f"{label} scheduler"),
            parse_last(
                output, WGPU_RE if expected_backend == "webgpu" else GL_RE,
                f"{label} backpressure"),
        )


def validate(run: Run) -> list[str]:
    failures: list[str] = []
    summary = run.summary
    pressure = run.pressure
    for key, expected in (
            ("ticks", TICKS), ("issued", TICKS),
            ("entries", EXPECTED_PRESENTS),
            ("presents", EXPECTED_PRESENTS),
            ("presentkind", 3), ("requestedkind", 3)):
        if summary.get(key) != expected:
            failures.append(
                f"{run.backend}: {key}={summary.get(key)}, expected {expected}")
    for key in ("multidue", "lead", "lag", "catchup", "skips",
                "rebases", "blocked", "updatebad"):
        if summary.get(key, 0) != 0:
            failures.append(
                f"{run.backend}: scheduler {key}={summary.get(key)}")

    if pressure.get("cap") != 2:
        failures.append(f"{run.backend}: cap={pressure.get('cap')}, expected 2")
    high_water = pressure.get("highwater", 0)
    if not (1 <= high_water <= 2):
        failures.append(
            f"{run.backend}: in-flight high-water {high_water} is outside 1..2")
    submitted = pressure.get("submitted", 0)
    completed = pressure.get("completed", -1)
    if submitted < EXPECTED_PRESENTS // 2:
        failures.append(
            f"{run.backend}: only {submitted} live GPU submissions")
    if completed != submitted:
        failures.append(
            f"{run.backend}: completed={completed}, submitted={submitted}")
    for key in ("inflight", "failures"):
        if pressure.get(key) != 0:
            failures.append(
                f"{run.backend}: {key}={pressure.get(key)}, expected 0")
    if pressure.get("waits", 0) < 1:
        failures.append(f"{run.backend}: completion wait path never ran")
    if pressure.get("polls", 0) < 1:
        failures.append(f"{run.backend}: completion polling never ran")
    if pressure.get("rateMilliHz", 0) <= 0:
        failures.append(f"{run.backend}: achieved submit rate was not measured")

    if run.backend == "webgpu":
        if pressure.get("skips") != 0:
            failures.append(
                f"webgpu: native run skipped {pressure.get('skips')} frames")
        if pressure.get("abandoned") != 0:
            failures.append(
                f"webgpu: abandoned {pressure.get('abandoned')} completions")
        if not re.search(
                r"\[PRESENT-MODE\] backend=webgpu policy=uncapped .*"
                r"requested=immediate effective=(?:immediate|fifo)",
                run.output):
            failures.append("webgpu: missing effective uncapped present mode")
        if not re.search(
                r"\[WGPU-SHUTDOWN\].*liveChildren=0.*cpuArrays=0",
                run.output):
            failures.append("webgpu: child-resource shutdown did not reach zero")
    else:
        if pressure.get("effectiveSwap") != 0:
            failures.append(
                f"gl: effectiveSwap={pressure.get('effectiveSwap')}, expected 0")
        if not re.search(
                r"\[PRESENT-MODE\] backend=gl policy=uncapped .*"
                r"requestedSwap=0 effectiveSwap=0 supported=1",
                run.output):
            failures.append("gl: interval-0 policy was not applied")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build-rel")
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
        # live GPU gate so a future backend-policy edit cannot silently put the
        # CAMetalLayer-limited uncapped path back in front of users.
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
    print("check_gpu_backpressure: PASS -- native uncapped GL/WebGPU queue "
          "production is completion-bounded; native default is GL")
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
