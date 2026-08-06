#!/usr/bin/env python3
"""Presentation quality is a measured distribution, not an impression.

Pacing work has historically been judged by counting presents: the right NUMBER
of images at the right rate. That number says nothing about whether the images
arrived EVENLY, which is the whole of what a player perceives as smoothness. A
run can hit its rational present count exactly and still stutter, because the
count is a total and stutter lives in the tail.

So this gate reads the three distributions the engine now publishes at shutdown
(`[PRESENTPERF-HIST]`, one row per series) plus the present-queue latency proxy
(`[PRESENTPERF-LATENCY]`), and holds them to invariants that must be true of any
correct run:

  * every present opportunity contributes exactly one interval sample, and
    every displayed frame exactly one phase sample, so the census cannot be
    quietly dropping or double-counting the thing it claims to measure;
  * the interpolation phase never runs backwards between two displayed frames
    (`regressions=0`) and never fails to advance at all (`stalls=0`);
  * with motion smoothing off, no displayed frame may advance the phase by less
    than a whole tick -- there are no interpolated images to display, so a
    sub-tick advance would mean one was displayed anyway;
  * the audio sink never starved (`underruns=0`).

ARMS. The synthetic arms cover every presentation policy crossed with both
smoothing settings and prove the plumbing. They deliberately assert only
structural identities, never wall-clock numbers: synthetic pacing does not
sleep, so its intervals are the host loop's own speed and mean nothing as a
quality measurement.

The REALTIME arm is where the quality numbers come from -- a genuinely paced,
compositor-visible run on the display policy with smoothing. It gates the
things that must never regress (no tearing mode, no audio underrun) and REPORTS
the displayed-interval and phase-variance tails as the labelled M3 baseline.
Those are reported rather than gated on purpose: a threshold invented before a
baseline exists is a guess, and this run is what turns it into a number.

WHEN THE ENVIRONMENT CANNOT PRODUCE A BASELINE. Two session conditions make the
realtime numbers meaningless without anything being wrong in the code:

  * no display session at all -- a locked screen or a session with no window
    server refuses every drawable, and the run measures a pacer nothing is
    consuming (`explain_no_display_session`);
  * a session that accepts presents without vsync-blocking them -- the display
    policy installs no software limiter and relies on FIFO to pace it, so an
    unthrottled surface lets the loop free-run and produces a displayed cadence
    far faster than the host's own refresh (`explain_unthrottled_presentation`).

Both are detected and named explicitly rather than reported as a generic
missing-marker failure -- the same distinction check_app_adopted_pacing draws,
and for the same reason: the generic message sends debugging into the pacing
code, which is the wrong place. In both cases the no-tearing and no-underrun
assertions still run, because neither can be excused by the environment; only
the BASELINE is withheld, since publishing it would seed M3's targets from a
measurement of nothing. The synthetic arms still prove the plumbing.

This arm is genuinely bimodal on a developer machine: the same command can
vsync-throttle on one run and free-run on the next, depending on how the
compositor treats the automation window. Read the printed baseline, or its
absence, rather than assuming a run produced one.

Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (ABORT_MARKERS, DEFAULT_BUILD_DIR, find_fatal,
                           parse_last, parse_rows, resolve_binary,
                           tear_free_presentation)

ROOT = Path(__file__).resolve().parent.parent

# Enough ticks for the boot/load transient to stop dominating, small enough that
# eight synthetic arms stay a few minutes total.
SYNTH_TICKS = 200
# The realtime arm is a wall-clock measurement, so its length is a DURATION:
# 30 seconds at the authored 30 Hz NTSC cadence.
REALTIME_SECONDS = 30
REALTIME_TICKS = REALTIME_SECONDS * 30
# A long automation window can lose its drawable partway through, which costs
# the baseline but nothing else. One retry converts most of those into a usable
# measurement; more than that is just spending minutes on a session that is not
# going to pace.
REALTIME_ATTEMPTS = 2

ONE_TICK_PPM = 1_000_000

# [PRESENTSCHED-SUMMARY] presentkind, as published by MdkrPresentPolicyKind.
KIND_ORIGINAL = 0
KIND_CAPPED = 1
KIND_DISPLAY = 2
KIND_UNCAPPED = 3

POLICIES: tuple[tuple[str, str, int], ...] = (
    # label fragment, MDKR_PRESENT_RATE value, expected presentkind
    ("original", "original", KIND_ORIGINAL),
    ("capped60", "60", KIND_CAPPED),
    ("display", "display", KIND_DISPLAY),
    ("uncapped", "uncapped", KIND_UNCAPPED),
)

SMOOTHINGS: tuple[tuple[str, str], ...] = (
    ("smoothing", "interpolate"),
    ("nosmoothing", "off"),
)

SERIES = ("present-interval", "displayed-interval", "alpha-delta")


@dataclass(frozen=True)
class Run:
    label: str
    output: str
    summary: dict[str, int]
    hist: dict[str, dict[str, int]]
    latency: dict[str, int]
    audio: dict[str, int]
    pressure: dict[str, int]
    arm: str


def hist_rows(output: str) -> dict[str, dict[str, int]]:
    """The `[PRESENTPERF-HIST]` rows keyed by their `series=` name.

    `parse_rows` drops non-integer values, which is exactly right for the
    numeric payload but also means `series=` and `arm=` are absent from the
    parsed dicts -- so the series name is recovered from the raw text here.
    """
    rows: dict[str, dict[str, int]] = {}
    for line in output.splitlines():
        marker = "[PRESENTPERF-HIST] "
        if marker not in line:
            continue
        payload = line.split(marker, 1)[1]
        name = ""
        for token in payload.split():
            key, separator, value = token.partition("=")
            if separator and key == "series":
                name = value
                break
        if name:
            rows[name] = parse_rows(line, "PRESENTPERF-HIST")[0]
    return rows


def arm_label(output: str) -> str:
    """The `arm=` field the engine stamped on its histogram rows."""
    for line in output.splitlines():
        marker = "[PRESENTPERF-HIST] "
        if marker not in line:
            continue
        for token in line.split(marker, 1)[1].split():
            key, separator, value = token.partition("=")
            if separator and key == "arm":
                return value
    return ""


def run_arm(binary: Path, rom: Path, root: Path, label: str, policy: str,
            smoothing: str, ticks: int, timeout: int, verbose: bool,
            *, realtime: bool = False) -> Run:
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUDIO_SERVICE_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK="5",
        MDKR_PRESENT_PERF="1",
        MDKR_PRESENT_RATE=policy,
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_PRESENT_SMOOTHING=smoothing,
        # Pinned rather than left to the native default: the queue-depth proxy
        # is sampled by the WebGPU present path, so `samples` is only
        # comparable with `surfaceupdates` on a run that is definitely using it.
        MDKR_RENDERER="webgpu",
        MDKR_SAVE_DIR=str(save_dir),
        # A hidden native Metal window has no drawable by design, and this gate
        # is about images that actually reach a surface. Use the established
        # compositor-visible headless seam rather than confusing offscreen
        # completion with a present.
        MDKR_TEST_VISIBLE_HEADLESS="1",
    )
    if realtime:
        # The tick budget bounds the run; this is what keeps the PACER real
        # inside that budget, so the intervals are wall time the host actually
        # slept rather than a synthetic per-present field count.
        env["MDKR_PACE_REALTIME"] = "1"
    command = [
        str(binary), "--headless-ticks", str(ticks),
        "--window-size", "320x240", "--rom", str(rom),
    ]
    if verbose:
        print(f"$ ({label}) MDKR_PRESENT_RATE={policy} "
              f"MDKR_PRESENT_SMOOTHING={smoothing} "
              f"realtime={int(realtime)} {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(f"{label}: exit {process.returncode}\n{output[-3000:]}")
    fatal = find_fatal(output, *ABORT_MARKERS)
    if fatal:
        raise RuntimeError(f"{label}: fatal marker {fatal}")
    histograms = hist_rows(output)
    missing = [name for name in SERIES if name not in histograms]
    if missing:
        raise RuntimeError(
            f"{label}: the pacing-quality census emitted no "
            f"{', '.join(missing)} row; MDKR_PRESENT_PERF plumbing is broken")
    audio_rows = parse_rows(output, "AUDIO-SINK")
    return Run(
        label=label,
        output=output,
        summary=parse_last(output, "PRESENTSCHED-SUMMARY", label=label,
                           expect_one=True),
        hist=histograms,
        latency=parse_last(output, "PRESENTPERF-LATENCY", label=label,
                           expect_one=True),
        audio=audio_rows[-1] if audio_rows else {},
        pressure=parse_rows(output, "WGPU-BACKPRESSURE")[-1]
        if parse_rows(output, "WGPU-BACKPRESSURE") else {},
        arm=arm_label(output),
    )


def explain_no_display_session(result: Run) -> str | None:
    """Name the likely cause when nothing the run submitted reached a surface.

    A locked screen, or a session with no active window server, refuses every
    drawable: the backend then reports submitted=N presented=0 unavailable=N in
    its one canonical telemetry row. The pacer, the census and the scheduler are
    all working correctly in that case -- there is simply no consumer -- so a
    realtime QUALITY measurement taken there is meaningless rather than failing.
    Reporting it as a generic missing-marker or out-of-range failure sends
    debugging into the pacing code, which is exactly the wrong place, so the
    signature is detected and named here instead.
    """
    pressure = result.pressure
    if not pressure:
        return None
    submitted = pressure.get("submitted", 0)
    presented = pressure.get("presented", -1)
    unavailable = pressure.get("unavailable", -1)
    if submitted > 0 and presented == 0 and unavailable == submitted:
        return (
            f"the window server provided no drawables (submitted={submitted} "
            f"presented=0 unavailable={unavailable}) — an active display "
            "session is required for this arm; rerun when one is available")
    return None


def explain_unthrottled_presentation(result: Run) -> str | None:
    """Name the case where the display policy never actually paced to a display.

    The display policy installs no software limiter: with `rate=0` it relies on
    the swapchain's FIFO present to block the loop at the refresh. That works
    only while the compositor genuinely vsync-throttles this window. Some
    sessions -- an offscreen or unthrottled surface, a compositor that discards
    for a window it is not showing -- accept every present immediately, and the
    loop then free-runs.

    The run is not wrong when that happens; the pacer, the census and the
    scheduler all behave exactly as designed. But the numbers it produces are
    the loop's own speed rather than a paced cadence, and publishing them as a
    quality baseline would seed M3's targets from a measurement of nothing. The
    signature is unmistakable -- a displayed cadence far faster than the refresh
    period the host reports -- so it is detected and named rather than reported
    as a suspiciously good result.
    """
    period_us = result.latency.get("periodus", 0)
    displayed_p50 = result.hist["displayed-interval"].get("p50", 0)
    if period_us <= 0 or displayed_p50 <= 0:
        return None
    if displayed_p50 * 2 < period_us:
        return (
            f"presents were not throttled to the display (displayed-interval "
            f"p50={displayed_p50}us against a {period_us}us refresh period) — "
            "this session's compositor accepted every present without "
            "vsync-blocking, so the run measured an unthrottled loop rather "
            "than paced presentation")
    return None


def check_plumbing(result: Run, kind: int, smoothing_on: bool) -> list[str]:
    """The structural identities that must hold for any correct run."""
    label = result.label
    failures: list[str] = list(tear_free_presentation(result.output, label))
    summary = result.summary
    present_hist = result.hist["present-interval"]
    displayed_hist = result.hist["displayed-interval"]
    alpha = result.hist["alpha-delta"]

    if summary.get("presentkind") != kind:
        failures.append(
            f"{label}: presentkind={summary.get('presentkind')}, expected {kind}")

    presents = alpha.get("presents", -1)
    displayed = alpha.get("displayed", -1)

    # The census must be counting the SAME presents the scheduler counted; if it
    # drifts, every distribution below describes a run that did not happen.
    if presents != summary.get("presents"):
        failures.append(
            f"{label}: census presents={presents} disagrees with scheduler "
            f"presents={summary.get('presents')}")

    # A displayed frame is an authored endpoint or a submitted interpolated
    # image. Original policy has no subloop at all, so every present is the
    # tick's own and every one is displayed.
    expected_displayed = (
        presents if kind == KIND_ORIGINAL
        else summary.get("realendpoints", 0) + summary.get("interp", 0))
    if displayed != expected_displayed:
        failures.append(
            f"{label}: displayed={displayed}, expected {expected_displayed} "
            f"(realendpoints={summary.get('realendpoints')} + "
            f"interp={summary.get('interp')})")
    if displayed > presents:
        failures.append(
            f"{label}: displayed={displayed} exceeds presents={presents}")

    # Every present after the first contributes exactly one interval sample, and
    # every displayed frame after the first exactly one of each of the other two.
    for name, hist, count in (
            ("present-interval", present_hist, presents),
            ("displayed-interval", displayed_hist, displayed),
            ("alpha-delta", alpha, displayed)):
        if count >= 1 and hist.get("n") != count - 1:
            failures.append(
                f"{label}: {name} n={hist.get('n')}, expected {count - 1} "
                f"for {count} samples")

    # Monotone-sane phase: never backwards, never standing still.
    if alpha.get("regressions", -1) != 0:
        failures.append(
            f"{label}: interpolation phase ran backwards "
            f"{alpha.get('regressions')} times between displayed frames")
    if alpha.get("stalls", -1) != 0:
        failures.append(
            f"{label}: {alpha.get('stalls')} displayed frames advanced the "
            "interpolation phase by nothing at all")
    if alpha.get("n", 0) > 0 and alpha.get("min", 0) <= 0:
        failures.append(
            f"{label}: alpha-delta min={alpha.get('min')}, every displayed "
            "frame must advance the phase")

    # With smoothing off there is no interpolated image to display, so a
    # displayed frame is always a tick endpoint and always a whole tick on.
    if not smoothing_on and alpha.get("n", 0) > 0:
        if alpha.get("min", 0) < ONE_TICK_PPM:
            failures.append(
                f"{label}: smoothing is off but a displayed frame advanced the "
                f"phase only {alpha.get('min')} ppm, under one tick — an "
                "interpolated image reached the screen")

    for name in SERIES:
        if result.hist[name].get("sqoverflow", 0) != 0:
            failures.append(
                f"{label}: {name} variance accumulator overflowed "
                f"({result.hist[name].get('sqoverflow')} samples)")

    # The latency proxy samples exactly the surface presents the renderer made.
    if result.latency.get("samples") != summary.get("surfaceupdates"):
        failures.append(
            f"{label}: latency samples={result.latency.get('samples')} "
            f"disagrees with surfaceupdates={summary.get('surfaceupdates')}")

    failures.extend(check_audio(result))
    return failures


def check_audio(result: Run) -> list[str]:
    """`underruns=0` wherever a live sink actually ran."""
    if not result.audio:
        # Headless runs open no audio device, so the controller makes no live
        # decisions and suppresses its row. Nothing to assert; the deterministic
        # coverage for these counters is tests/test_audio_queue_controller.c.
        return []
    underruns = result.audio.get("underruns", -1)
    if underruns != 0:
        return [f"{result.label}: audio sink starved {underruns} times "
                "(underruns must be 0)"]
    return []


def baseline_note(result: Run) -> str:
    displayed = result.hist["displayed-interval"]
    alpha = result.hist["alpha-delta"]
    latency = result.latency
    return (
        f"{result.label} [arm={result.arm}]: "
        f"displayed-interval p50={displayed.get('p50')}us "
        f"p95={displayed.get('p95')}us p99={displayed.get('p99')}us "
        f"max={displayed.get('max')}us var={displayed.get('var')}us^2 "
        f"n={displayed.get('n')}; alpha-delta p50={alpha.get('p50')}ppm "
        f"p99={alpha.get('p99')}ppm var={alpha.get('var')}ppm^2; "
        f"queue-depth mean={latency.get('meandepthmilli', 0) / 1000:.3f} "
        f"max={latency.get('maxdepth')} frames -> "
        f"latency mean={latency.get('meanlatencyus')}us "
        f"max={latency.get('maxlatencyus')}us "
        f"at {latency.get('refreshhz')}Hz")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--skip-realtime", action="store_true",
                        help="synthetic plumbing arms only")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []
    notes: list[str] = []
    baselines: list[str] = []

    with tempfile.TemporaryDirectory(prefix="mdkr-pacing-quality-") as temp:
        root = Path(temp)
        try:
            for policy_name, policy, kind in POLICIES:
                for smoothing_name, smoothing in SMOOTHINGS:
                    label = f"synth-{policy_name}-{smoothing_name}"
                    result = run_arm(
                        binary, rom, root, label, policy, smoothing,
                        SYNTH_TICKS, args.timeout, args.verbose)
                    expected_arm = (
                        f"{'capped' if kind == KIND_CAPPED else policy_name}"
                        f"/{smoothing_name}/synth")
                    if result.arm != expected_arm:
                        failures.append(
                            f"{label}: census arm={result.arm!r}, expected "
                            f"{expected_arm!r}")
                    failures.extend(check_plumbing(
                        result, kind, smoothing == "interpolate"))
                    notes.append(
                        f"{label}: {result.hist['alpha-delta'].get('presents')} "
                        f"presents / "
                        f"{result.hist['alpha-delta'].get('displayed')} "
                        "displayed")

            if args.skip_realtime:
                notes.append("realtime arm skipped by --skip-realtime")
            else:
                cause = None
                # Losing the drawable partway through a long automation run is
                # transient, so one retry is worth a valid baseline. Both
                # attempts are fully gated; only the baseline needs a session
                # that paced for the whole run.
                for attempt in range(1, REALTIME_ATTEMPTS + 1):
                    label = "realtime-display-smoothing"
                    if attempt > 1:
                        label = f"{label}-retry{attempt - 1}"
                    result = run_arm(
                        binary, rom, root, label, "display", "interpolate",
                        REALTIME_TICKS, args.timeout, args.verbose,
                        realtime=True)
                    if not result.arm.endswith("/realtime"):
                        failures.append(
                            f"{label}: census arm={result.arm!r} is not a "
                            "realtime run; MDKR_PACE_REALTIME did not take "
                            "effect")
                    # Gated regardless of how well the session paced: neither
                    # of these can be excused by the environment.
                    failures.extend(
                        tear_free_presentation(result.output, label))
                    failures.extend(check_audio(result))
                    if result.hist["alpha-delta"].get("regressions", -1) != 0:
                        failures.append(
                            f"{label}: interpolation phase ran backwards")
                    cause = (explain_no_display_session(result)
                             or explain_unthrottled_presentation(result))
                    if not cause:
                        # Reported, not gated: M3's targets come from these.
                        baselines.append(baseline_note(result))
                        break
                    notes.append(f"{label}: no valid baseline — {cause}")
                if cause:
                    notes.append(
                        f"realtime arm: NO BASELINE after {REALTIME_ATTEMPTS} "
                        "attempts. The no-tearing and no-underrun assertions "
                        "still ran; the distributions are not a valid quality "
                        "measurement and were not published")
        except RuntimeError as error:
            failures.append(str(error))

    if failures:
        print("check_pacing_quality: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_pacing_quality: PASS -- displayed-interval, interpolation-"
          "phase and present-queue-latency census consistent across every "
          "policy/smoothing arm, with no phase regression and no audio underrun")
    for note in notes:
        print(f"  - {note}")
    for baseline in baselines:
        print(f"  - M3 BASELINE: {baseline}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
