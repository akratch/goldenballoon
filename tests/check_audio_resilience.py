#!/usr/bin/env python3
"""Prove the SHIPPED binary wires up the callback/ring audio sink.

``tests/test_audio_resilience.c`` (ctest ``audio_resilience``) already proves the
mechanism in isolation: a lock-free SPSC ring that conceals underflow with a
crossfade, and a latency controller driven by a measured drain instead of an
elapsed-time estimate. What that ROM-free test cannot see is whether the engine
still *uses* any of it. A regression that reverted the sink to SDL_QueueAudio,
or that deleted the
``mdkr_audio_queue_controller_set_device_period()`` / ``note_drain()`` calls,
leaves every C unit green and ships broken audio.

So this check runs the real binary headless with the SDL dummy driver and reads
the four seams that only exist when the wiring is intact:

- ``[AUDIO] SDL device open: ... mode=callback`` -- callback mode, real ring;
- ``[AUDIO-RING] ...`` -- the ring actually carried the run's PCM, losing none;
- ``[AUDIO-SINK] ... deviceperiod=`` -- the controller was TOLD the negotiated
  period rather than guessing at a compile-time constant;
- ``[AUDIO-SINK] ... stalls=0`` -- the drain is measured, not inferred.

Each of those is an assertion about the binary's composition, which is the part
the unit test structurally cannot reach.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import (
    ABORT_MARKERS,
    DEFAULT_BUILD_DIR,
    fatal_re,
    parse_last,
    parse_rows,
    resolve_binary,
)


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tests" / "input_scripts" / "race_drive_time_trial.txt"
FRAMES = 900

# The device-open banner. ``mode=`` is captured rather than matched literally so
# a queue-mode regression fails with the mode it actually printed instead of
# with an unhelpful "no line matched".
OPEN_RE = re.compile(
    r"\[AUDIO\] SDL device open: (\d+) Hz, (\d+) ch, (\d+) buf "
    r"\(requested (\d+)\), ring=(\d+) frames, mode=(\w+)"
)
BAD_RE = fatal_re(*ABORT_MARKERS)

# The ring is sized for a host that stalls for whole display frames, not for one
# device period; anything shallower cannot absorb a scheduler hiccup and the
# concealment crossfade would become audible policy rather than a safety net.
MIN_RING_FRAMES = 8192


def run_engine(binary: Path, rom: Path, state_root: Path, *,
               enable_sink: bool) -> tuple[int, str]:
    """One headless run, with the host sink either wired up or opted out.

    ``MDKR_TEST_HEADLESS_AUDIO=1`` is refused unless ``SDL_AUDIODRIVER=dummy``,
    so both are always set; the control arm differs only by ``MDKR_AUDIO=0``,
    which keeps synthesis running but opens no host device.
    """

    save_dir = state_root / "save"
    save_dir.mkdir(parents=True, exist_ok=True)
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        SDL_AUDIODRIVER="dummy",
        MDKR_TEST_HEADLESS_AUDIO="1",
        # [AUDIO-SINK] and [AUDIO-SERVICE] are trace-gated; without this the
        # controller and synthesiser evidence is simply never printed.
        MDKR_AUDIO_SERVICE_TRACE="1",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_VIDEO_CONFIG_PATH=str(state_root / "video.ini"),
    )
    if not enable_sink:
        env["MDKR_AUDIO"] = "0"
    command = [
        str(binary), "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT), "--rom", str(rom),
    ]
    process = subprocess.run(
        command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=180, check=False,
    )
    return process.returncode, process.stdout.decode("utf-8", "replace")


def assert_clean_exit(returncode: int, output: str, label: str) -> None:
    if returncode != 0:
        raise AssertionError(f"{label} exited {returncode}\n{output[-5000:]}")
    bad = BAD_RE.search(output)
    if bad:
        raise AssertionError(f"{label} emitted {bad.group(0)!r}\n{output[-5000:]}")


def assert_callback_device(output: str) -> tuple[int, int]:
    """The device is in callback mode behind a usably deep ring.

    Returns ``(negotiated_period, ring_capacity)`` for the later cross-checks:
    every other assertion here is about those two numbers agreeing with what the
    ring and the controller independently report.
    """

    matches = OPEN_RE.findall(output)
    if len(matches) != 1:
        raise AssertionError(
            f"expected exactly one device-open banner, got {len(matches)}\n"
            + output[-5000:]
        )
    rate, channels, period, requested, capacity, mode = matches[0]
    if mode != "callback":
        raise AssertionError(
            f"device opened in mode={mode!r}; the ring sink requires callback "
            "mode, so this is a revert to SDL_QueueAudio"
        )
    period, capacity = int(period), int(capacity)
    if period <= 0:
        raise AssertionError(f"negotiated device period is {period}")
    if capacity < MIN_RING_FRAMES:
        raise AssertionError(
            f"ring capacity {capacity} is below the {MIN_RING_FRAMES}-frame floor"
        )
    if capacity & (capacity - 1):
        # The ring masks indices instead of taking a modulus, so a non-power-of-two
        # capacity would silently corrupt wraparound rather than merely be odd.
        raise AssertionError(f"ring capacity {capacity} is not a power of two")
    if (int(rate), int(channels)) != (22050, 2):
        raise AssertionError(
            f"unexpected device contract: {rate} Hz, {channels} ch")
    if int(requested) <= 0:
        raise AssertionError(f"requested period is {requested}")
    return period, capacity


def assert_ring_conserved(output: str, period: int, capacity: int) -> int:
    """The ring carried this run's PCM and dropped none of it.

    Returns the pushed frame count so the synthesiser cross-check can compare
    against it.
    """

    ring = parse_last(output, "AUDIO-RING", label="ring summary", expect_one=True)
    for key in ("period", "capacity", "pushed", "maxcallback", "evicted",
                "overflows"):
        if key not in ring:
            raise AssertionError(f"[AUDIO-RING] is missing {key}=: {ring}")
    if ring["period"] != period or ring["capacity"] != capacity:
        raise AssertionError(
            f"[AUDIO-RING] reports period/capacity {ring['period']}/"
            f"{ring['capacity']}, but the device opened at {period}/{capacity}"
        )
    if ring["pushed"] <= 0:
        raise AssertionError(f"no PCM was pushed into the ring: {ring}")
    if ring["maxcallback"] > period:
        raise AssertionError(
            f"a callback asked for {ring['maxcallback']} frames, more than the "
            f"negotiated period {period}; the pull path is mis-sized"
        )
    if ring["evicted"] != 0 or ring["overflows"] != 0:
        raise AssertionError(
            "a healthy run must never drop PCM on the floor: "
            f"overflows={ring['overflows']} evicted={ring['evicted']}"
        )
    # Deliberately NOT asserted: underruns/concealments. A headless run
    # free-runs far faster than real time, so the ring stays deep and starvation
    # never happens here -- an underruns==0 assertion would pass for the wrong
    # reason. Starvation and its crossfade are covered deterministically by the
    # audio_resilience ctest instead.
    return ring["pushed"]


def assert_controller_wiring(output: str, period: int, pushed: int) -> None:
    """The controller was told the period, and its drain is measured.

    ``deviceperiod`` is the direct footprint of
    ``mdkr_audio_queue_controller_set_device_period()``: drop that call and the
    controller falls back to its own default, which will not match the value SDL
    negotiated on this host.

    ``measureddrains`` is the footprint of ``note_drain()``: it counts the
    decisions that sized against a drain the audio callback actually measured,
    rather than one inferred from elapsed host time. Every live decision after
    the first must be one of those -- the first has no previous sample to
    difference against.

    Counting them is necessary because nothing else here can tell the two
    drain sources apart. ``stalls=0`` looks like it should, since the stall
    guard exists only to bound an *estimated* drain, but headless runs
    free-run far ahead of real time and never produce an interval long enough
    to trip it, so that assertion holds just as well with the measurement
    reverted. Both this field and ``deviceperiod`` are read back out of the
    controller rather than the sink, so they witness the wiring instead of the
    sink's opinion of it.
    """

    # [AUDIO-SINK] is also the tag the shutdown path uses for its drop and
    # enqueue-failure warnings, and those are printed AFTER the summary. Select
    # the controller row by a field only it carries rather than by position, so
    # a warning cannot displace the row being asserted on.
    sinks = [row for row in parse_rows(output, "AUDIO-SINK")
             if "decisions" in row]
    if len(sinks) != 1:
        raise AssertionError(
            f"expected one [AUDIO-SINK] controller summary, got {len(sinks)}\n"
            + output[-5000:]
        )
    sink = sinks[0]
    for key in ("deviceperiod", "stalls", "produced", "decisions",
                "measureddrains"):
        if key not in sink:
            raise AssertionError(f"[AUDIO-SINK] is missing {key}=: {sink}")
    if sink["deviceperiod"] != period:
        raise AssertionError(
            f"controller holds deviceperiod={sink['deviceperiod']} but SDL "
            f"negotiated {period}; set_device_period() was not called"
        )
    if sink["measureddrains"] != sink["decisions"] - 1:
        raise AssertionError(
            f"controller sized {sink['measureddrains']} of {sink['decisions']} "
            "decisions against a measured drain; every decision after the "
            "first must use one, so the sink is back on an elapsed-time "
            "estimate"
        )
    if sink["stalls"] != 0:
        raise AssertionError(
            f"stall guard fired {sink['stalls']} times; the controller is back "
            "on an elapsed-time drain estimate instead of the measured drain"
        )

    service = parse_last(output, "AUDIO-SERVICE", label="service summary")
    if "samples" not in service:
        raise AssertionError(f"[AUDIO-SERVICE] is missing samples=: {service}")
    if service["samples"] != pushed:
        raise AssertionError(
            f"the synthesiser produced {service['samples']} frames but the ring "
            f"took {pushed}; the sink is not carrying exactly what was made"
        )


def assert_sink_off_control(binary: Path, rom: Path, state_root: Path) -> None:
    """Positive control: with no host sink, the evidence must disappear.

    Every assertion above reads a log line. If those lines were emitted
    unconditionally -- or if the detectors matched something incidental -- the
    check would pass over a binary with no ring at all. ``MDKR_AUDIO=0`` keeps
    synthesis running but opens no device, so a run in which the banner or the
    ring summary still appears means the detectors are not looking at the sink.
    """

    returncode, output = run_engine(binary, rom, state_root, enable_sink=False)
    assert_clean_exit(returncode, output, "sink-disabled control")
    if OPEN_RE.search(output):
        raise AssertionError(
            "the sink-disabled control still opened an audio device")
    if "[AUDIO-RING]" in output:
        raise AssertionError(
            "the sink-disabled control still emitted a ring summary; the ring "
            "detector is not reading the host sink")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64", type=Path)
    parser.add_argument(
        "--no-controls", action="store_true",
        help="skip the sink-disabled arm that proves the detectors can fail",
    )
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.expanduser().resolve()
    for path in (binary, rom, SCRIPT):
        if not path.is_file():
            parser.error(f"missing required file: {path}")

    try:
        with tempfile.TemporaryDirectory(prefix="mdkr_audio_resilience_") as raw:
            root = Path(raw)
            returncode, output = run_engine(
                binary, rom, root / "wired", enable_sink=True)
            assert_clean_exit(returncode, output, "callback-ring route")
            period, capacity = assert_callback_device(output)
            pushed = assert_ring_conserved(output, period, capacity)
            assert_controller_wiring(output, period, pushed)
            if not args.no_controls:
                assert_sink_off_control(binary, rom, root / "control")
    except (AssertionError, OSError, RuntimeError,
            subprocess.TimeoutExpired) as error:
        print(f"check_audio_resilience: FAIL\n  {error}", file=sys.stderr)
        return 1

    print(
        "check_audio_resilience: PASS (callback ring sink, no PCM lost, "
        "controller told the negotiated period, measured drain, "
        "synth/ring frame conservation)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
