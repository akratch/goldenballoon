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

The shipping-configuration arms
-------------------------------
Everything above used to be measured only with `MDKR_TRACE=1` exported, and that
is a configuration no player has ever run: the shipped native launchers set no
`MDKR_*` variable at all, and the web shell sets `MDKR_TRACE` only from a
`?trace=` query string. Twice, that gap let a shadow gate go green over a
player-visible bug. The recorded one is wave "shadowdeep" R1
(docs/open-items/renderer.md): `gfx_shadow_stage_begin()` -- the static-caster
cache reset -- had its only call site inside a branch gated by
`mdkr_resource_trace_enabled()`, which is true when EITHER `MDKR_TRACE` or
`MDKR_RESOURCE_STATS` is set. Every shadow gate exported `MDKR_TRACE=1`, so the
reset always fired under test and never fired in a shipping build, where the
previous level's casters kept shadowing the next one and address-keyed dedup
then dropped the new level's real casters.

The lesson is not "that one call site"; it is that a diagnostic variable which
also arms engine behaviour makes the whole traced measurement a statement about
a different program. So each backend now runs its off/on pair TWICE: once traced
(which is what unlocks `[PACE]`, hence the gameplay-state equality above) and
once in shipping configuration, with `MDKR_TRACE` absent. The shipping arms
carry every assertion that does not need a trace row -- the pixel handoff
classification, the decal-triangle handoff, the `[WORLD-SHADOW]` healthy-path
telemetry (emitted unconditionally at shutdown, not trace-gated) -- and add the
one assertion that names the failure mode directly: the shipping frame must be
byte-identical to the traced frame. A diagnostic variable that changes a single
pixel of the shipped image fails here, whatever the mechanism, without anyone
having to have predicted it.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (DEFAULT_BUILD_DIR, DEVICE_MARKERS, fatal_re,
                           read_ppm, resolve_binary)


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
FATAL_RE = fatal_re(*DEVICE_MARKERS, ignore_case=True)


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


def run_arm(
    binary: Path,
    rom: Path,
    backend: str,
    enabled: bool,
    force_resource_failure: bool,
    root: Path,
    timeout: int,
    verbose: bool,
    traced: bool = True,
) -> Result:
    arm = (
        "fault" if force_resource_failure
        else ("on" if enabled else "off")
    )
    if not traced:
        arm += "-shipping"
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
        MDKR_RENDERER=backend,
        MDKR_LOAD_TRACK="5",
        MDKR_TEST_RENDER_FULL_ADMISSION="1",
        MDKR_DUMP_FROM=str(CAPTURE_FRAME),
        MDKR_DUMP_EVERY="999",
        MDKR64_HIDDEN="1",
        MDKR_SAVE_DIR=str(save_dir),
        # Isolate the video config with the save (see check_door_blocks.py).
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    # The one variable under audit. A shipping arm leaves it out entirely -- the
    # env dict above is already scrubbed of every inherited MDKR_*/GE007_ key,
    # so "absent" here really is absent.
    if traced:
        env["MDKR_TRACE"] = "1"
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
    # `[PACE]` is trace-gated, so a shipping arm cannot have it and must not
    # pretend to: it carries an empty tuple and the gameplay-state equality is
    # asserted only between traced arms.
    require(bool(pace) == traced,
            f"{backend}-{arm}: {len(pace)} gameplay-state rows with "
            f"MDKR_TRACE {'set' if traced else 'unset'}")
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
    return verify_visual_pair(backend, off, on)


def verify_visual_pair(backend: str, off: Result, on: Result) -> str:
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
    # opposite channel deltas after tint and sRGB encode. The invariant is that
    # this class must never become a broad hue-shift REGION.
    #
    # It was written as 1% of `changed` and re-expressed against the frame at
    # the R2 light-depth-sign fix, because the old form measured the wrong
    # thing. `changed` is the size of the shadow handoff, which is exactly what
    # the fix under test moves: correcting the inverted cascade depth axis
    # stopped every surface standing on the ground from being umbra'd, so the
    # handoff halved (40540 -> 20692 pixels) while the mixed class itself FELL
    # (368 -> 263). A ratio against a shrinking denominator turned a strict
    # improvement into a failure -- the twelfth bug shape in DEVELOPER_HANDBOOK
    # section 3, in a threshold instead of a fixture. A fraction of the frame
    # says "not a broad region" directly and cannot be gamed by a smaller
    # handoff: it still rejects the pre-fix worst frames measured on this route
    # (5985 mixed pixels, 1.9% of the frame).
    require(
        1000 < changed < pixel_count // 5 and
        dark_only > 500 and bright_only > 100 and
        mixed <= pixel_count // 500,
        f"{backend}: implausible visual handoff changed={changed}, "
        f"darkOnly={dark_only}, brightOnly={bright_only}, mixed={mixed} "
        f"(cap {pixel_count // 500})",
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


def verify_shipping(backend: str, traced: Result, shipping: Result) -> str:
    """The traced measurement must describe the program players actually run.

    Byte equality is the assertion that names the failure mode instead of a
    symptom of it: whatever a diagnostic variable arms -- a cache reset, an extra
    allocation, a different admission order -- if it reaches the frame buffer, it
    shows up here. Wave "shadowdeep" R1 would have failed on this line, because
    the traced arm got a per-level static-caster reset that the shipping build
    did not.
    """
    require(
        (traced.width, traced.height) == (shipping.width, shipping.height),
        f"{backend}: output dimensions differ between the traced and shipping "
        f"configurations ({traced.width}x{traced.height} vs "
        f"{shipping.width}x{shipping.height})",
    )
    differing = sum(
        1
        for index in range(0, len(traced.pixels), 3)
        if traced.pixels[index:index + 3] != shipping.pixels[index:index + 3]
    )
    require(
        differing == 0,
        f"{backend}: MDKR_TRACE changes the rendered image -- {differing} of "
        f"{traced.width * traced.height} pixels differ between the traced arm "
        f"and the same route in shipping configuration. Every other assertion "
        f"in this gate is therefore about a program nobody ships. Find what the "
        f"trace variable arms besides printing (mdkr_resource_trace_enabled() "
        f"is the known precedent) and take the behaviour out of the diagnostic "
        f"branch",
    )
    require(
        traced.decal_triangles == shipping.decal_triangles,
        f"{backend}: legacy decal triangle count differs between the traced "
        f"({traced.decal_triangles}) and shipping ({shipping.decal_triangles}) "
        f"configurations",
    )
    require(
        (traced.attempted, traced.complete, traced.fallback,
         traced.resource_failures, traced.latched) ==
        (shipping.attempted, shipping.complete, shipping.fallback,
         shipping.resource_failures, shipping.latched),
        f"{backend}: shadow-map telemetry differs between the traced and "
        f"shipping configurations "
        f"{(traced.attempted, traced.complete, traced.fallback, traced.resource_failures, traced.latched)}"
        f" vs "
        f"{(shipping.attempted, shipping.complete, shipping.fallback, shipping.resource_failures, shipping.latched)}",
    )
    return f"{backend}: shipping configuration reproduces the traced frame exactly"


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
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
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
                # The same pair again with MDKR_TRACE absent -- the
                # configuration players run. See the module docstring.
                off_ship = run_arm(
                    binary, rom, backend, False, False, root,
                    args.timeout, args.verbose, traced=False,
                )
                on_ship = run_arm(
                    binary, rom, backend, True, False, root,
                    args.timeout, args.verbose, traced=False,
                )
                summaries.append(verify_pair(backend, off, on))
                summaries.append(verify_fallback(backend, off, fault))
                summaries.append(
                    "shipping " + verify_visual_pair(backend, off_ship, on_ship)
                )
                summaries.append(verify_shipping(backend + " off", off, off_ship))
                summaries.append(verify_shipping(backend + " on", on, on_ship))
    except RuntimeError as exc:
        print(f"FAIL: {exc}")
        return 1

    print("PASS world shadows")
    for summary in summaries:
        print(f"  {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
