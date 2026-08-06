#!/usr/bin/env python3
"""Minimized native windows stop GPU replay and rebase cleanly on resume."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (completed_tick_conservation, DEFAULT_BUILD_DIR,
                           fatal_re, GPU_MARKERS, parse_last, resolve_binary)


TICKS = 30
MINIMIZE_START = 8
MINIMIZE_END = 18
SURFACE_RE = re.compile(
    r"\[SURFACE-PACING\] presentable=(\d+) renderElided=(\d+) "
    r"resumeRebase=(\d+) tick=(\d+) frame=(\d+)"
)
FATAL_RE = fatal_re(*GPU_MARKERS)


@dataclass(frozen=True)
class Result:
    backend: str
    minimized: bool
    state: list[str]
    events: list[str]
    inputs: list[str]
    pcm_digest: str
    summary: dict[str, int]
    replay: dict[str, int]
    pressure: dict[str, int]
    surface: list[tuple[int, int, int, int, int]]


def marked(output: str, marker: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith(marker)]


def clean_environment(**updates: str) -> dict[str, str]:
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(updates)
    return env


def run(binary: Path, rom: Path, backend: str, minimized: bool,
        timeout: int, verbose: bool) -> Result:
    label = f"{backend}-{'minimized' if minimized else 'control'}"
    with tempfile.TemporaryDirectory(prefix=f"mdkr64_surface_{label}_") as temp:
        root = Path(temp)
        pcm = root / "audio.wav"
        env = clean_environment(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_AUDIO_DUMP=str(pcm),
            MDKR_AUDIO_SERVICE_TRACE="1",
            MDKR_AUTOPILOT="1",
            MDKR_EVENT_HASH="1",
            MDKR_INPUT_HASH="1",
            MDKR_LOAD_TRACK="5",
            MDKR_PRESENT_RATE="uncapped",
            MDKR_PRESENT_SCHED_TRACE="1",
            MDKR_RENDERER=backend,
            MDKR_SAVE_DIR=str(root / "save"),
            MDKR_STATE_HASH="3",
            MDKR_TEST_VISIBLE_HEADLESS="1",
        )
        if minimized:
            env["MDKR_TEST_MINIMIZE_TICKS"] = (
                f"{MINIMIZE_START}:{MINIMIZE_END}")
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
        if not pcm.is_file() or pcm.stat().st_size <= 44:
            raise RuntimeError(f"{label}: temporary PCM was not produced")
        return Result(
            backend, minimized,
            marked(output, "[SIMHASH]"),
            marked(output, "[EVENTHASH]"),
            marked(output, "[INPUTHASH]"),
            hashlib.sha256(pcm.read_bytes()).hexdigest(),
            parse_last(output, "PRESENTSCHED-SUMMARY",
                       label=f"{label} scheduler", expect_one=True),
            parse_last(output, "REPLAY-SUMMARY",
                       label=f"{label} replay", expect_one=True),
            parse_last(output, "WGPU-BACKPRESSURE", "GL-BACKPRESSURE",
                       label=f"{label} backpressure", expect_one=True),
            [tuple(map(int, match.groups())) for match in
             SURFACE_RE.finditer(output)],
        )


def validate_pair(control: Result, minimized: Result) -> list[str]:
    failures: list[str] = []
    backend = control.backend
    for label, actual, expected in (
            ("v3 state", minimized.state, control.state),
            ("ordered events", minimized.events, control.events),
            ("consumed input", minimized.inputs, control.inputs)):
        if actual != expected:
            failures.append(f"{backend}: minimized {label} differs from control")
    if minimized.pcm_digest != control.pcm_digest:
        failures.append(f"{backend}: minimized PCM differs from control")
    if not (len(control.state) == len(control.events) == len(control.inputs) ==
            len(minimized.state) == len(minimized.events) ==
            len(minimized.inputs) == TICKS):
        failures.append(f"{backend}: fixed trace length is not {TICKS}")

    for result in (control, minimized):
        arm = "minimized" if result.minimized else "control"
        conservation_error = completed_tick_conservation(
            result.summary, TICKS, f"{backend}-{arm}")
        if conservation_error:
            failures.append(conservation_error)
        for key, expected in (("presentkind", 3),):
            if result.summary.get(key) != expected:
                failures.append(
                    f"{backend}-{arm}: {key}={result.summary.get(key)}, "
                    f"expected {expected}")
        for key in ("multidue", "lag", "catchup", "skips",
                    "blocked", "updatebad"):
            if result.summary.get(key, 0) != 0:
                failures.append(
                    f"{backend}-{arm}: {key}={result.summary.get(key)}")
        highwater = result.pressure.get("highwater", 0)
        submitted = result.pressure.get("submitted", 0)
        if backend == "webgpu":
            # Native WebGPU deliberately polls without synchronously draining
            # the cooperative game/audio thread; its telemetry `cap` applies
            # to browser/internal replay, not production native submission.
            # check_gpu_backpressure owns that contract in detail. Here the
            # census must still be internally possible and nonblocking.
            if not (1 <= highwater <= submitted):
                failures.append(
                    f"{backend}-{arm}: invalid GPU high-water "
                    f"{highwater} for {submitted} submissions")
            if result.pressure.get("runtimewaits", 0) != 0 or \
                    result.pressure.get("runtimewaitns", 0) != 0:
                failures.append(
                    f"{backend}-{arm}: production path synchronously drained "
                    "the GPU queue")
        elif highwater > result.pressure.get("cap", 0):
            failures.append(f"{backend}-{arm}: GPU queue exceeded its cap")
        if result.pressure.get("failures", 0) != 0:
            failures.append(f"{backend}-{arm}: GPU completion failure")
        if backend == "webgpu":
            for key in ("abandoned", "inflight"):
                if result.pressure.get(key, 0) != 0:
                    failures.append(
                        f"{backend}-{arm}: {key}="
                        f"{result.pressure.get(key, 0)}")
            admission_skips = result.pressure.get("skips", 0)
            if admission_skips != (
                    result.pressure.get("endpointSkips", 0) +
                    result.pressure.get("replaySkips", 0)):
                failures.append(
                    f"{backend}-{arm}: admission skips are not fully "
                    "classified")
            # Native WebGPU admission is deliberately nonblocking. A fast
            # control can shed more endpoints than a minimized run because the
            # hidden interval gives the GPU time to retire its queue. Prove
            # exact ownership instead of comparing throughput between arms.
            real_walks = result.replay.get("realwalks", 0)
            elided = result.summary.get("elided", 0)
            if real_walks != submitted or \
                    submitted + admission_skips + elided != TICKS:
                failures.append(
                    f"{backend}-{arm}: ticks do not partition into submitted "
                    f"({submitted}), admission-skipped ({admission_skips}), "
                    f"and surface-elided ({elided}) work")
            if (result.pressure.get("presented", 0) +
                    result.pressure.get("unavailable", 0) != submitted):
                failures.append(
                    f"{backend}-{arm}: surface outcomes do not account for "
                    f"all {submitted} submissions")

    if control.surface:
        failures.append(f"{backend}: control unexpectedly changed visibility")
    expected_rows = [(0, 1, 0), (1, 0, 1)]
    if [row[:3] for row in minimized.surface] != expected_rows:
        failures.append(
            f"{backend}: visibility rows are {minimized.surface}, "
            f"expected hidden then rebased resume")
    elif minimized.surface[1][4] - minimized.surface[0][4] > 2:
        failures.append(
            f"{backend}: presented {minimized.surface[1][4] - minimized.surface[0][4]} "
            "frames while minimized")
    hidden_ticks = minimized.summary.get("elided", 0)
    if hidden_ticks < MINIMIZE_END - MINIMIZE_START:
        failures.append(
            f"{backend}: only {hidden_ticks} render tasks were elided")
    if minimized.summary.get("rebases", 0) < 1:
        failures.append(f"{backend}: resume did not rebase scheduler history")
    if (backend != "webgpu" and
            minimized.replay.get("realwalks", TICKS) >=
            control.replay.get("realwalks", 0) - 5):
        failures.append(
            f"{backend}: minimized real walks did not fall decisively "
            f"({minimized.replay.get('realwalks')} vs "
            f"{control.replay.get('realwalks')})")
    for key in ("freezefail", "restorefail"):
        if minimized.replay.get(key, 0) != 0:
            failures.append(
                f"{backend}: minimized replay {key}={minimized.replay.get(key)}")
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
            print(f"check_surface_suspension: FAIL\n  - missing {path}")
            return 1

    failures: list[str] = []
    notes: list[str] = []
    try:
        for backend in ("webgpu", "gl"):
            control = run(
                binary, rom, backend, False, args.timeout, args.verbose)
            minimized = run(
                binary, rom, backend, True, args.timeout, args.verbose)
            failures.extend(validate_pair(control, minimized))
            notes.append(
                f"{backend}: {minimized.summary.get('elided')} tasks elided, "
                f"{minimized.replay.get('realwalks')}/"
                f"{control.replay.get('realwalks')} real walks, "
                f"{minimized.summary.get('rebases')} resume rebase(s)")
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        failures.append(str(error))

    if failures:
        print("check_surface_suspension: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_surface_suspension: PASS -- minimized native surfaces stop "
          "GPU walks and resume without authority/audio/input drift")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
