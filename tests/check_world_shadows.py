#!/usr/bin/env python3
"""Validate complete real-shadow maps, receivers, and actor-decal handoff.

Each shipped backend runs the same closed-loop race with the diagnostic feature
off and on. The gate requires:

* byte-identical normalized gameplay state;
* one structurally valid output frame from each arm;
* a bounded mix of map darkening and projected-actor-decal removal;
* materially fewer legacy decal triangles only after a complete map exists; and
* byte-identical projected-shadow fallback after forced optional-resource loss;
* no renderer validation, lifecycle, sanitizer, or fatal report.

The feature remains diagnostic until the wider scene/performance matrix passes.
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


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
BACKENDS = ("gl", "webgpu")
FRAMES = 3500
CAPTURE_FRAME = FRAMES - 1
DEPTH_RE = re.compile(
    r"\[DEPTH\] decalTriangles=(\d+) comparedTriangles=(\d+)"
)
SHADOW_RE = re.compile(
    r"\[WORLD-SHADOW\] backend=(gl|webgpu) attempted=(\d+) "
    r"complete=(\d+) fallback=(\d+) resourceFailures=(\d+) latched=(\d+)"
)
FATAL_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|validation error|device lost|shader compilation failed",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class Result:
    label: str
    pace: tuple[str, ...]
    width: int
    height: int
    pixels: bytes
    decal_triangles: int
    attempted: int
    complete: int
    fallback: int
    resource_failures: int
    latched: int


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def normalized_pace(output: str) -> tuple[str, ...]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker >= 0:
            rows.append(
                re.sub(r" dtms=\S+", " dtms=<wall>", line[marker:])
            )
    return tuple(rows)


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    parts = path.read_bytes().split(b"\n", 3)
    require(
        len(parts) == 4 and parts[0] == b"P6" and parts[2] == b"255",
        f"{path}: unsupported PPM header",
    )
    width, height = (int(value) for value in parts[1].split())
    require(
        len(parts[3]) == width * height * 3,
        f"{path}: malformed RGB raster",
    )
    return width, height, parts[3]


def run_arm(
    binary: Path,
    rom: Path,
    backend: str,
    enabled: bool,
    force_resource_failure: bool,
    root: Path,
    timeout: int,
    verbose: bool,
) -> Result:
    arm = (
        "fault" if force_resource_failure
        else ("on" if enabled else "off")
    )
    run_dir = root / f"{backend}-{arm}"
    frame_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frame_dir.mkdir(parents=True)
    save_dir.mkdir()
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_AUTOPILOT="1",
        MDKR_TRACE="1",
        MDKR_RENDERER=backend,
        MDKR_LOAD_TRACK="5",
        MDKR_DUMP_FROM=str(CAPTURE_FRAME),
        MDKR_DUMP_EVERY="999",
        MDKR64_HIDDEN="1",
        MDKR_SAVE_DIR=str(save_dir),
    )
    env["MDKR_WORLD_SHADOW"] = "1" if enabled else "0"
    if force_resource_failure:
        env["MDKR_TEST_WORLD_SHADOW_RESOURCE_FAIL"] = "1"
    command = [
        str(binary),
        "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT),
        "--dump-frames", str(frame_dir),
        "--rom", str(rom),
        "--window-size", "320x240",
        "--remastered",
    ]
    if verbose:
        print(f"$ ({backend}-{arm}) {' '.join(command)}", flush=True)
    try:
        process = subprocess.run(
            command,
            cwd=run_dir,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"{backend}-{arm}: timed out\n{(exc.stdout or '')[-5000:]}"
        ) from exc

    output = process.stdout
    fatal = FATAL_RE.search(output)
    frames = list(frame_dir.glob("*.ppm"))
    depth = list(DEPTH_RE.finditer(output))
    shadow = list(SHADOW_RE.finditer(output))
    require(
        process.returncode == 0 and fatal is None and
        len(frames) == 1 and len(depth) == 1 and len(shadow) == 1,
        f"{backend}-{arm}: exit={process.returncode}, "
        f"fatal={fatal.group(0) if fatal else 'none'}, "
        f"frames={len(frames)}, depthRows={len(depth)}, "
        f"shadowRows={len(shadow)}\n"
        f"{output[-6000:]}",
    )
    require(
        shadow[0].group(1) == backend,
        f"{backend}-{arm}: telemetry named the wrong backend",
    )
    pace = normalized_pace(output)
    require(pace, f"{backend}-{arm}: no gameplay-state rows")
    width, height, pixels = read_ppm(frames[0])
    return Result(
        f"{backend}-{arm}",
        pace,
        width,
        height,
        pixels,
        int(depth[0].group(1)),
        *(int(shadow[0].group(index)) for index in range(2, 7)),
    )


def verify_pair(backend: str, off: Result, on: Result) -> str:
    require(
        off.pace == on.pace,
        f"{backend}: enabling real shadows changed gameplay state",
    )
    require(
        (off.width, off.height) == (on.width, on.height),
        f"{backend}: output dimensions changed",
    )
    changed = dark_only = bright_only = mixed = 0
    for index in range(0, len(off.pixels), 3):
        before = off.pixels[index:index + 3]
        after = on.pixels[index:index + 3]
        if before == after:
            continue
        changed += 1
        darker = any(right < left for left, right in zip(before, after))
        brighter = any(right > left for left, right in zip(before, after))
        dark_only += darker and not brighter
        bright_only += brighter and not darker
        mixed += darker and brighter

    pixel_count = off.width * off.height
    # The linear-light world finish can turn a sub-LSB receiver/decal edge into
    # opposite channel deltas after tint and sRGB encode. Keep that mixed class
    # below 1% of the handoff; it must never become a broad hue-shift region.
    require(
        1000 < changed < pixel_count // 5 and
        dark_only > 500 and bright_only > 100 and
        mixed <= changed // 100,
        f"{backend}: implausible visual handoff changed={changed}, "
        f"darkOnly={dark_only}, brightOnly={bright_only}, mixed={mixed}",
    )
    require(
        0 < on.decal_triangles < off.decal_triangles * 9 // 10,
        f"{backend}: actor decal fallback did not hand off after map readiness "
        f"({off.decal_triangles} -> {on.decal_triangles})",
    )
    require(
        on.attempted > 0 and on.complete > 0 and
        on.fallback < on.attempted and
        on.resource_failures == 0 and on.latched == 0,
        f"{backend}: incomplete healthy-path telemetry "
        f"{on.attempted=}, {on.complete=}, {on.fallback=}, "
        f"{on.resource_failures=}, {on.latched=}",
    )
    return (
        f"{backend}: {changed}/{pixel_count} pixels "
        f"(dark={dark_only}, decal-release={bright_only}, mixed={mixed}), "
        f"decal triangles {off.decal_triangles}->{on.decal_triangles}"
    )


def verify_fallback(
    backend: str,
    off: Result,
    fault: Result,
) -> str:
    require(
        off.pace == fault.pace,
        f"{backend}: resource fallback changed gameplay state",
    )
    require(
        (off.width, off.height, off.pixels, off.decal_triangles) ==
        (
            fault.width,
            fault.height,
            fault.pixels,
            fault.decal_triangles,
        ),
        f"{backend}: resource fallback did not preserve the exact legacy frame",
    )
    require(
        fault.attempted > 0 and fault.complete == 0 and
        fault.fallback == fault.attempted and
        fault.resource_failures == 3 and fault.latched == 1,
        f"{backend}: forced resource loss was not bounded and latched "
        f"{fault.attempted=}, {fault.complete=}, {fault.fallback=}, "
        f"{fault.resource_failures=}, {fault.latched=}",
    )
    return (
        f"{backend}: forced loss retained {fault.decal_triangles} legacy "
        f"decal triangles; retries={fault.resource_failures}, latched"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom)
    if not rom.is_absolute():
        rom = (ROOT / rom).resolve()
    missing = [path for path in (binary, rom, SCRIPT) if not path.is_file()]
    if missing:
        for path in missing:
            print(f"FAIL: missing {path}")
        return 1

    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-world-shadow-") as temp:
            root = Path(temp)
            summaries = []
            for backend in BACKENDS:
                off = run_arm(
                    binary, rom, backend, False, False, root,
                    args.timeout, args.verbose,
                )
                on = run_arm(
                    binary, rom, backend, True, False, root,
                    args.timeout, args.verbose,
                )
                fault = run_arm(
                    binary, rom, backend, True, True, root,
                    args.timeout, args.verbose,
                )
                summaries.append(verify_pair(backend, off, on))
                summaries.append(verify_fallback(backend, off, fault))
    except RuntimeError as exc:
        print(f"FAIL: {exc}")
        return 1

    print("PASS world shadows")
    for summary in summaries:
        print(f"  {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
