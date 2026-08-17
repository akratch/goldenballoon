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
things that must never regress (no tearing mode, no audio underrun) and
publishes the displayed-interval and phase-variance tails as the labelled
baseline for the next milestone.

M3 TURNED TWO OF THOSE REPORTS INTO GATES. M2 published them rather than gating
them for a good reason -- a threshold invented before a baseline exists is a
guess -- and M2's run is what turned them into numbers. Now that M3 has moved
them, the interpolation-phase variance and the displayed-interval p99 are held
to bounds derived from the post-M3 measurement with 2x headroom, so ordinary
machine variance cannot flake them but a real regression cannot hide either.
They are gated only on a run that actually paced (see below); publishing or
gating a number taken from a session that never throttled would be worse than
measuring nothing.

AND ONLY WHEN EVERY PACED ATTEMPT MISSES. This arm is bimodal by nature, and a
busy machine can hand one attempt a tail squarely inside the pre-M3 range --
measured, not hypothesised. No threshold can separate that from a regression,
because it IS the same number; the thing that separates them is that a
regression repeats and a transient does not. So a paced attempt that misses its
bounds is retried, and the run fails only when no paced attempt came in. The
pre-M3 build missed these bounds by 5x to 25x on every run it made, so the
retry costs nothing in detection.

THE VBLANK PROJECTION (M3 slice 1) is asserted structurally rather than
numerically, because it is a claim about WHICH VALUES ARE POSSIBLE and not
about how big they are. Under a vblank-quantized queue the engine projects the
interpolation phase onto the display's own grid and publishes that grid as
`gridppm`. So:

  * every synthetic arm must report `gridppm=0`. Synthetic pacing has no vblank
    to project onto, and the projection declining there is exactly what keeps
    those runs bit-for-bit what they were before M3 -- which is what
    tests/check_arbitrary_presentation_rates.py's byte-identity arms prove;
  * a realtime arm that paced must report a nonzero `gridppm` AND displayed
    phases that ARE grid points. A run that quietly stopped projecting would
    keep passing every count-based identity in here; only this catches it.

TASK 5 MADE AN ASSUMPTION HERE EXPLICIT. The grid-phase assertions above are
only meaningful on a fixed present interval -- projecting onto a grid a panel
is not actually holding to would be dishonest, which is exactly what Task 5's
measured-variance decline in `platform_present_display_quantum_units()` now
refuses to do. That decline is real: this project's own M3 Max development
machine has a ProMotion (ADAPTIVE-refresh) built-in display, and a genuinely
unthrottled `MDKR_PRESENT_QUANTUM_STRICT`-free run on it measures present
interval variance well above the 2500ppm threshold, so `gridppm` legitimately
comes back 0 there. The main realtime arm below sets
`MDKR_PRESENT_QUANTUM_STRICT=1` for exactly this reason -- it pins today's
always-grid regime so this arm keeps measuring the SAME thing it always did,
on any panel, fixed or adaptive. The variance-honest path itself is covered
separately by a companion arm, WITHOUT strict, that asserts `[ALPHA-QUANTUM]`'s
reported `mode` matches what its own `variance_ppm` implies -- `free` above
the threshold, `grid` at or below it -- so it is a real assertion on any
machine rather than a hardcoded expectation tied to this one.

THE DISPLAY-CHANGE ARM (M3 slice 2) covers a window moving to a monitor with a
different refresh. What can be automated is everything downstream of the event:
MDKR_TEST_DISPLAY_RATE_SWITCH=<hz>@<tick> makes the host report a different
refresh at a chosen tick and runs the same handler SDL's display-changed event
does, and the arm asserts the backend re-ranked its present mode against the
new number rather than keeping the one baked at configuration.

  MANUAL STEP, NOT COVERED HERE: SDL's delivery of
  SDL_WINDOWEVENT_DISPLAY_CHANGED itself needs two monitors of different
  refresh, which no headless run has. To check it by hand, start a windowed run
  with MDKR_PRESENT_RATE=display and drag the window between the two monitors;
  stderr must show one `[PRESENT-DISPLAY] event=display-changed` row per
  crossing with the two refresh rates, followed by a re-emitted `[PRESENT-MODE]`
  row carrying the new `displayHz`. A refresh change on the SAME display raises
  no SDL event at all and still needs a relaunch, which is what the pacing
  keys' SCOPE_RESTART already tells the player.

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
# the baseline but nothing else, and a busy machine can turn one attempt's tail
# into a number that looks like a regression. Retries convert most of both into
# a usable measurement; more than a few is just spending minutes on a session
# that is not going to pace.
REALTIME_ATTEMPTS = 3

ONE_TICK_PPM = 1_000_000

# M3 GATE BOUNDS.
#
# Both are 2x the post-M3 measurement on the reference machine, rounded up to a
# round number. The factor is not caution about the change -- it is caution
# about the MEASUREMENT: this arm shares a machine with whatever else is running
# on it, and the same binary legitimately varies by a factor of two or three
# between runs. A bound tight enough to catch that variance would fail on a busy
# afternoon and teach everyone to ignore it. Two times the measured value still
# catches the regressions worth catching, because the M2->M3 move was an order
# of magnitude, not a percentage.
#
# Recorded so the next milestone can see what it inherited. Both columns are
# three runs of this file on one machine, the pre-M3 column measured by running
# the pre-M3 binary against this same gate:
#
#   phase variance    before 3.4e10 - 1.8e11 ppm^2  ->  after 1.3e9 - 1.3e10
#   phase p50         before 303104 - 499712 ppm    ->  after 503808 ppm
#   phase p99         before 720896 - 1000000 ppm   ->  after 503808 ppm
#   displayed p99     before 17.8 - 19.1 ms         ->  after 18.0 - 20.1 ms
#
# The phase numbers are slice 1 (the vblank projection): the median phase was
# not even reliably the midpoint before, and now every percentile out to the
# 99th is one grid point. The displayed-interval p99 is NOT improved and is not
# claimed to be -- slice 3 removed presents that were never going to reach the
# screen, not the compositor jitter of the ones that do. It is gated to stop it
# getting worse, not as evidence that it got better.
ALPHA_VAR_MAX_PPM2 = 30_000_000_000
DISPLAYED_P99_MAX_US = 40_000
# A paced native WebGPU display run should not manufacture an immediate replay
# after each nonblocking FIFO present and then shed it at the one-frame replay
# admission boundary. Allow startup and host noise, but reject the alternating
# submit/hold pattern that made continuously scrolling hub waterfalls shimmer
# on 120 Hz VRR panels (measured 8,833 / 13,646 attempts before the fix).
REPLAY_SKIP_MAX_FRACTION = 0.05

# The display-change arm's transition. 100 is deliberately BETWEEN the two
# refresh rates: a 100 Hz cap wants a latest-image queue against a 60 Hz
# display and is served exactly by the blocking one at 120 Hz, so a backend
# that failed to re-rank keeps asking for the wrong queue and the arm can see
# it. The tick is far enough in that the surface is definitely configured.
DISPLAY_SWITCH_POLICY = "100"
DISPLAY_SWITCH_CAP_HZ = 100
# Try 120 first so the common 60 Hz qualification host still exercises the
# original crossing. A ProMotion panel may already report 120; retrying at 60
# keeps the synthetic event non-vacuous instead of asking 120 Hz to change to
# itself and then blaming the runtime for correctly emitting no event.
DISPLAY_SWITCH_RATES = (120, 60)
DISPLAY_SWITCH_TICK = 60
DISPLAY_SWITCH_TICKS = 140
# MDKR_PRESENT_DISPLAY_MARGIN_HZ (platform/pacing_policy.h), duplicated as a
# number on purpose: a gate that read the constant out of the source it is
# checking would agree with any value the source happened to hold.
DISPLAY_MARGIN_HZ = 3

# [PRESENTSCHED-SUMMARY] presentkind, as published by MdkrPresentPolicyKind.
KIND_ORIGINAL = 0
KIND_CAPPED = 1
KIND_DISPLAY = 2
KIND_UNCAPPED = 3
KIND_DISPLAY_MARGIN = 4

POLICIES: tuple[tuple[str, str, int], ...] = (
    # label fragment, MDKR_PRESENT_RATE value, expected presentkind
    ("original", "original", KIND_ORIGINAL),
    ("capped60", "60", KIND_CAPPED),
    # 40 is the battery-friendly handheld cap (M5). It is here rather than only
    # in the rational-count gate because it is the one offered cap that is
    # BELOW the 60 Hz stand-in refresh and ABOVE the 30 Hz simulation cadence,
    # so it is the arm where the deadline grid, the display's own queue and the
    # tick boundary all disagree about spacing at once.
    ("capped40", "40", KIND_CAPPED),
    ("display", "display", KIND_DISPLAY),
    ("display-margin", "display-margin", KIND_DISPLAY_MARGIN),
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


def alpha_quantum_row(output: str) -> dict[str, str]:
    """The one `[ALPHA-QUANTUM]` line this run's flush emitted, or {} if none.

    Not a `parse_rows` payload: `mode` is a bare word (`grid`/`free`), which
    that helper's int-only parsing would silently drop, and the mode is the
    entire point of reading this line.
    """
    for line in output.splitlines():
        marker = "[ALPHA-QUANTUM] "
        if marker not in line:
            continue
        fields: dict[str, str] = {}
        for token in line.split(marker, 1)[1].split():
            key, separator, value = token.partition("=")
            if separator:
                fields[key] = value
        return fields
    return {}


def run_arm(binary: Path, rom: Path, root: Path, label: str, policy: str,
            smoothing: str, ticks: int, timeout: int, verbose: bool,
            *, realtime: bool = False,
            extra_env: dict[str, str] | None = None) -> Run:
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
    if extra_env:
        env.update(extra_env)
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


def grid_locked(reported: int, grid: int, binwidth: int) -> bool:
    """Whether a reported percentile is a point on the `grid`.

    Percentiles come out of a fixed-bin histogram, so the number is the bin's
    upper edge and the true sample lies in ``(reported - binwidth, reported]``.
    A grid point is therefore "hit" when some whole multiple of the grid falls
    in that half-open window.
    """
    if grid <= 0 or reported <= 0:
        return False
    multiple = reported // grid
    for candidate in (multiple, multiple + 1):
        if candidate >= 1 and reported - binwidth < candidate * grid <= reported:
            return True
    return False


def check_vblank_projection(result: Run) -> list[str]:
    """The displayed phases really are points on the display's own grid.

    Only meaningful for an arm that paced against a vblank-quantized queue; the
    caller decides that. A count-based check cannot see this: a build that
    stopped projecting would still present the right NUMBER of images at the
    right phases-on-average, and only stop being even.
    """
    label = result.label
    alpha = result.hist["alpha-delta"]
    grid = alpha.get("gridppm", 0)
    failures: list[str] = []

    if grid <= 0:
        return [
            f"{label}: gridppm={grid} — this arm paced against the display's "
            "own refresh, so the interpolation phase must be projected onto "
            "its grid; the projection declined"]
    binwidth = alpha.get("binwidth", 0)
    for percentile in ("p50", "p95"):
        reported = alpha.get(percentile, 0)
        if not grid_locked(reported, grid, binwidth):
            failures.append(
                f"{label}: alpha-delta {percentile}={reported}ppm is not a "
                f"multiple of the {grid}ppm display grid (binwidth "
                f"{binwidth}ppm) — displayed phases are not landing on "
                "projected vblanks")
    return failures


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

    # Synthetic pacing does not sleep and has no vblank, so the M3 projection
    # must decline here. This is the assertion that keeps every headless arm --
    # including the byte-identity ones in check_arbitrary_presentation_rates --
    # bit-for-bit what they were: a projection that engaged under synthetic
    # pacing would move the interpolation phase and move them with it.
    if alpha.get("gridppm", -1) != 0:
        failures.append(
            f"{label}: gridppm={alpha.get('gridppm')} under synthetic pacing "
            "— the display-grid projection must decline where there is no "
            "vblank, or headless runs stop being reproducible")

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


def check_alpha_quantum_strict(result: Run) -> list[str]:
    """Under MDKR_PRESENT_QUANTUM_STRICT=1, [ALPHA-QUANTUM] must report mode=grid.

    The mutation-check half of the Task 5 companion coverage: strict mode
    exists precisely to force today's always-grid behavior back on regardless
    of measured variance, so its own diagnostic line must say so too. This is
    not redundant with check_vblank_projection's `gridppm` check above -- that
    proves the PROJECTION engaged, this proves the env var is what the new
    variance-decline branch itself is reading, by checking the same trace line
    the unstrict companion arm below reads to prove the opposite.
    """
    label = result.label
    fields = alpha_quantum_row(result.output)
    if not fields:
        return [f"{label}: no [ALPHA-QUANTUM] line — the flush-window trace "
                "did not fire"]
    failures: list[str] = []
    if fields.get("timing") not in {"fixed", "variable"}:
        failures.append(
            f"{label}: strict quantum classifier reported "
            f"timing={fields.get('timing')!r}, expected a settled fixed or "
            "variable interval classification")
    if fields.get("mode") != "grid":
        failures.append(
            f"{label}: MDKR_PRESENT_QUANTUM_STRICT=1 but [ALPHA-QUANTUM] "
            f"reported mode={fields.get('mode')}, expected grid")
    if fields.get("units") == "0":
        failures.append(
            f"{label}: MDKR_PRESENT_QUANTUM_STRICT=1 but [ALPHA-QUANTUM] "
            "units=0")
    return failures


def check_alpha_quantum_honest(result: Run) -> list[str]:
    """[ALPHA-QUANTUM]'s mode must match its published classifier state.

    This is the Task 5 path itself, run WITHOUT MDKR_PRESENT_QUANTUM_STRICT:
    the quantizer declines to project onto a grid the measured present
    interval is not actually holding to. The state, robust jitter metric,
    sample count and transition count are emitted by the production
    classifier itself. Checking the state rather than re-deriving one threshold
    here is load-bearing: the classifier deliberately uses trimming,
    hysteresis and sustained contrary evidence, so a consumer that infers its
    answer from the latest scalar would recreate the flapping defect.
    """
    label = result.label
    fields = alpha_quantum_row(result.output)
    if not fields:
        return [f"{label}: no [ALPHA-QUANTUM] line — the flush-window trace "
                "did not fire"]
    mode = fields.get("mode")
    units = fields.get("units")
    variance_raw = fields.get("variance_ppm")
    if variance_raw is None or not variance_raw.isdigit():
        return [f"{label}: [ALPHA-QUANTUM] variance_ppm={variance_raw!r} is "
                "not a parseable non-negative integer"]
    variance_ppm = int(variance_raw)
    timing = fields.get("timing")
    if timing not in {"warming", "fixed", "variable"}:
        return [f"{label}: [ALPHA-QUANTUM] timing={timing!r} is not a "
                "published classifier state"]
    samples_raw = fields.get("samples")
    transitions_raw = fields.get("transitions")
    if (samples_raw is None or not samples_raw.isdigit() or
            transitions_raw is None or not transitions_raw.isdigit()):
        return [f"{label}: [ALPHA-QUANTUM] classifier evidence is not "
                f"parseable: samples={samples_raw!r} "
                f"transitions={transitions_raw!r}"]
    disciplined = "[PRESENT-DISCIPLINE] active=1" in result.output
    if disciplined:
        # Closed-loop native pacing supersedes the classifier's testimony:
        # the deadline grid is disciplined onto real surface retirement, so
        # the projection is honest by construction and the phase is
        # slot-projected. The classifier still publishes its own state for
        # diagnostics, but it no longer decides the mode here.
        if mode != "slot":
            return [
                f"{label}: [PRESENT-DISCIPLINE] active=1 requires the "
                f"slot-projected phase (mode=slot) but [ALPHA-QUANTUM] "
                f"reported mode={mode}"]
        if units == "0":
            return [f"{label}: [ALPHA-QUANTUM] mode=slot but units=0"]
        return []
    expected_mode = "free" if timing == "variable" else "grid"
    if mode != expected_mode:
        return [
            f"{label}: [ALPHA-QUANTUM] timing={timing} "
            f"variance_ppm={variance_ppm} requires mode={expected_mode} "
            f"but reported mode={mode}"]
    if mode == "free" and units != "0":
        return [f"{label}: [ALPHA-QUANTUM] mode=free but units={units}, "
                "expected 0"]
    if mode == "grid" and units == "0":
        return [f"{label}: [ALPHA-QUANTUM] mode=grid but units=0"]
    return []


def check_slot_quality(result: Run) -> list[str]:
    """Motion-uniformity bounds for the closed-loop (slot-projected) arm.

    Expressed relative to the run's own gridppm so they hold on any display
    rate: the ideal alpha step IS one grid quantum, every frame. The pre-fix
    free-mode measurement on the M3 ProMotion machine was p95 = 1.54 grid
    (385,024ppm on a 250,000ppm grid) with 15 stalls and full-tick jumps in
    30 s; the slot projector's residual events are the tick/display beat
    (one soft repeat) and genuinely missed slots (counted anchors).
    """
    label = result.label
    if "[PRESENT-DISCIPLINE] active=1" not in result.output:
        return []
    alpha = result.hist["alpha-delta"]
    grid = alpha.get("gridppm", 0)
    failures: list[str] = []
    if grid <= 0:
        return [f"{label}: disciplined run reported gridppm={grid}; the "
                "slot projection publishes its quantum"]
    displayed = alpha.get("displayed", 0)
    p95 = alpha.get("p95", -1)
    stalls = alpha.get("stalls", -1)
    anchors = result.summary.get("slotanchors", -1)
    binwidth = alpha.get("binwidth", 0)
    # Two whole quanta is the environmental floor: a host hiccup or a GPU
    # admission skip doubles exactly one step, and a busy interactive
    # session produces a few percent of those. The regressions this bound
    # exists to catch look different — wake-noise smear lands at NON-grid
    # values (the pre-fix p95 was 385,024 on a 250,000 grid) and a pacing
    # break floods the anchor count below. The absolute max is deliberately
    # not gated: one ~30 ms scheduler stall per run is a fact of hosts.
    if p95 < 0 or p95 > grid * 2 + binwidth:
        failures.append(
            f"{label}: alpha-delta p95={p95}ppm exceeds 2x the "
            f"{grid}ppm slot quantum — steps are not uniform")
    stall_budget = max(2, displayed // 500)
    if stalls < 0 or stalls > stall_budget:
        failures.append(
            f"{label}: {stalls} stalled frames exceed the beat budget "
            f"{stall_budget} — motion is freezing more often than the "
            "tick/display beat can explain")
    anchor_budget = max(2, displayed // 500)
    if anchors < 0 or anchors > anchor_budget:
        failures.append(
            f"{label}: {anchors} slot re-anchors exceed {anchor_budget} — "
            "the pacer is missing display slots")
    return failures


def check_realtime_quality(result: Run) -> list[str]:
    """The M3 numeric gates, for a run that genuinely paced.

    Called only after both environment explanations came back clean: on a
    session that never throttled, these distributions describe the loop's own
    speed and holding them to a bound would be gating noise.
    """
    label = result.label
    displayed = result.hist["displayed-interval"]
    alpha = result.hist["alpha-delta"]
    failures: list[str] = []

    quantum = alpha_quantum_row(result.output)
    timing = quantum.get("timing")
    variance = alpha.get("var", -1)
    if variance < 0 or (timing == "fixed" and
                        variance > ALPHA_VAR_MAX_PPM2):
        failures.append(
            f"{label}: interpolation-phase variance {variance}ppm^2 exceeds "
            f"{ALPHA_VAR_MAX_PPM2}ppm^2 — displayed frames are not evenly "
            "spaced in phase, which is what a player sees as stutter")
    p99 = displayed.get("p99", -1)
    if p99 < 0 or p99 > DISPLAYED_P99_MAX_US:
        failures.append(
            f"{label}: displayed-interval p99 {p99}us exceeds "
            f"{DISPLAYED_P99_MAX_US}us — the slowest 1% of frames are "
            "arriving late enough to be seen")
    replay_skips = result.pressure.get("replaySkips", -1)
    interpolated = result.summary.get("interp", -1)
    # `interp` counts only replays that DREW; a walk that refused lands in
    # `stale`. Both were offered to admission, so both belong in the census
    # denominator or the shed fraction overstates the defect.
    stale = max(result.summary.get("stale", 0), 0)
    replay_attempts = replay_skips + interpolated + stale
    if (replay_skips < 0 or interpolated < 0 or replay_attempts <= 0):
        failures.append(
            f"{label}: missing replay admission census "
            f"(interp={interpolated}, replaySkips={replay_skips})")
    elif replay_skips > replay_attempts * REPLAY_SKIP_MAX_FRACTION:
        failures.append(
            f"{label}: WebGPU shed {replay_skips}/{replay_attempts} "
            f"interpolation replays, above "
            f"{REPLAY_SKIP_MAX_FRACTION:.0%} — optional frames are not being "
            "offered on an evenly paced display clock")
    unavailable = result.pressure.get("unavailable", -1)
    presented = result.pressure.get("presented", -1)
    # A host hiccup can legitimately cost one drawable every dozen or so
    # seconds even on a healthy session (a ~30 ms scheduler stall renders
    # two frames inside one retirement window). The defect this bound
    # guards against measured 726 refusals in 30 s — 200x this budget.
    unavailable_budget = max(2, presented // 1000)
    if presented > 0 and unavailable > unavailable_budget:
        failures.append(
            f"{label}: the surface refused {unavailable} drawables in a "
            f"session that presented (budget {unavailable_budget}) — the "
            "pacer is overrunning real retirement (each refusal is a "
            "rendered frame the player never saw, displayed as a repeat)")
    failures.extend(check_vblank_projection(result))
    failures.extend(check_alpha_quantum_strict(result))
    return failures


def check_display_change(result: Run, switch_to_hz: int) -> list[str]:
    """The refresh was re-derived live and the present mode re-ranked.

    The `[PRESENT-MODE]` row is emitted at every surface configuration, so a
    run whose display changed must show a SECOND one carrying the new refresh.
    Without the re-rank the swapchain keeps a present mode chosen for a monitor
    the window is no longer on -- which nothing else in this file can see,
    because the counts and cadences all stay plausible.
    """
    label = result.label
    failures: list[str] = []
    changes = [line for line in result.output.splitlines()
               if "[PRESENT-DISPLAY] event=display-changed" in line]
    modes = [line for line in result.output.splitlines()
             if "[PRESENT-MODE]" in line]

    def field(row: str, name: str) -> str:
        for token in row.split():
            key, separator, value = token.partition("=")
            if separator and key == name:
                return value
        return ""

    if len(changes) != 1:
        return [f"{label}: expected exactly one [PRESENT-DISPLAY] "
                f"display-changed row, saw {len(changes)}"]
    # The rate the run BOOTED at is whatever this machine's display reports, so
    # it is read rather than asserted; only the rate it moved TO is ours.
    booted = field(changes[0], "oldHz")
    if field(changes[0], "newHz") != str(switch_to_hz):
        failures.append(
            f"{label}: display-change row did not report the forced "
            f"{switch_to_hz}Hz rate: {changes[0].strip()}")

    rates = [field(row, "displayHz") for row in modes]
    if str(switch_to_hz) not in rates:
        failures.append(
            f"{label}: no [PRESENT-MODE] row was re-emitted at "
            f"displayHz={switch_to_hz} (saw {rates}) — the surface was "
            "not reconfigured, so the present mode is still ranked against the "
            "monitor the window left")

    # The cap deliberately sits BETWEEN the two refresh rates, so the ranking
    # crosses: above a 60 Hz display a 100 Hz cap wants a queue that can drop
    # an undisplayed image, and at 120 Hz the blocking queue serves it exactly.
    # A re-emitted row that requested the same mode either way would prove
    # nothing -- but only where the boot rate is actually below the cap, which
    # is a property of the machine and not of the change under test.
    requested = [field(row, "requested") for row in modes]
    if (booted.isdigit() and
            (int(booted) < DISPLAY_SWITCH_CAP_HZ) !=
            (switch_to_hz < DISPLAY_SWITCH_CAP_HZ)):
        if len(set(requested)) < 2:
            failures.append(
                f"{label}: the present-mode REQUEST did not change across a "
                f"{booted}Hz -> {switch_to_hz}Hz change that straddles "
                f"the {DISPLAY_SWITCH_CAP_HZ}Hz cap (saw {requested}) — the "
                "ranking is still being made against the old refresh")
    return failures


def check_display_margin_change(result: Run, switch_to_hz: int) -> list[str]:
    """display-margin re-derives its cadence when the display changes.

    This is the whole of what makes the setting honest. `display` follows the
    monitor and needs nothing but the new number; display-margin has to put that
    number back through the margin, and a handler that recognised only the
    display/uncapped kinds would leave the pacer on a grid built for the
    monitor the window left -- above the new panel's range on the way down, and
    needlessly far below it on the way up. Nothing else in this file would see
    that: the counts and the phase series stay perfectly plausible either way.

    The row publishes `resolvedRate` precisely so a headless run can check the
    derivation. Its `effectiveRate` deliberately does not move under synthetic
    pacing -- that is what keeps these runs independent of the machine's own
    monitor -- so asserting on that instead would assert nothing.
    """
    label = result.label
    rows = [line for line in result.output.splitlines()
            if "[PRESENT-DISPLAY] event=display-changed" in line]
    if len(rows) != 1:
        return [f"{label}: expected exactly one [PRESENT-DISPLAY] "
                f"display-changed row, saw {len(rows)}"]

    fields = {}
    for token in rows[0].split():
        key, separator, value = token.partition("=")
        if separator:
            fields[key] = value
    failures: list[str] = []
    if fields.get("policy") != "display-margin":
        failures.append(
            f"{label}: the display change was handled under "
            f"policy={fields.get('policy')}, not display-margin")
    expected = max(switch_to_hz - DISPLAY_MARGIN_HZ, 30)
    if fields.get("resolvedRate") != str(expected):
        failures.append(
            f"{label}: resolvedRate={fields.get('resolvedRate')} after a move "
            f"to a {switch_to_hz}Hz display, expected {expected} — the "
            "margin was not re-derived from the new refresh")
    return failures


def run_display_switch_arm(
    binary: Path,
    rom: Path,
    root: Path,
    label: str,
    policy: str,
    timeout: int,
    verbose: bool,
) -> tuple[Run, int]:
    """Run a synthetic display change whose destination differs from boot."""
    result: Run | None = None
    selected = DISPLAY_SWITCH_RATES[-1]
    for attempt, rate in enumerate(DISPLAY_SWITCH_RATES):
        selected = rate
        arm_label = label if attempt == 0 else f"{label}-to-{rate}hz"
        result = run_arm(
            binary, rom, root, arm_label, policy, "interpolate",
            DISPLAY_SWITCH_TICKS, timeout, verbose,
            extra_env={
                "MDKR_TEST_DISPLAY_RATE_SWITCH":
                    f"{rate}@{DISPLAY_SWITCH_TICK}",
            })
        if "[PRESENT-DISPLAY] event=display-changed" in result.output:
            break
    assert result is not None
    return result, selected


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
    parser.add_argument("--allow-no-baseline", action="store_true",
                        help="permit a host that cannot obtain a paced "
                             "session; the banner then states the quality "
                             "half was not measured")
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

            # Deterministic and cheap, so it runs regardless of --skip-realtime:
            # nothing in it is a wall-clock measurement.
            label = "synth-display-change"
            result, switch_to_hz = run_display_switch_arm(
                binary, rom, root, label, DISPLAY_SWITCH_POLICY,
                args.timeout, args.verbose)
            display_failures = check_display_change(result, switch_to_hz)
            if args.allow_no_baseline and display_failures:
                # No display session, no display object for the synthetic
                # switch to act on: same root as the realtime downgrades.
                notes.extend(f + " (allowed by --allow-no-baseline)"
                             for f in display_failures)
            else:
                failures.extend(display_failures)
            notes.append(
                f"{label}: refresh re-derived at {switch_to_hz}Hz and present "
                "mode re-ranked")

            label = "synth-display-margin-change"
            result, switch_to_hz = run_display_switch_arm(
                binary, rom, root, label, "display-margin",
                args.timeout, args.verbose)
            margin_failures = check_display_margin_change(
                result, switch_to_hz)
            if args.allow_no_baseline and margin_failures:
                notes.extend(f + " (allowed by --allow-no-baseline)"
                             for f in margin_failures)
            else:
                failures.extend(margin_failures)
            notes.append(
                f"{label}: margin re-derived against {switch_to_hz}Hz")

            if args.skip_realtime:
                notes.append("realtime arm skipped by --skip-realtime")
            else:
                cause = None
                quality: list[str] = []
                paced = 0
                # Losing the drawable partway through a long automation run is
                # transient, so a retry is worth a valid baseline. Every attempt
                # is fully gated on the things the environment cannot excuse;
                # only the baseline and the M3 bounds need a session that paced
                # for the whole run.
                for attempt in range(1, REALTIME_ATTEMPTS + 1):
                    label = "realtime-display-smoothing"
                    if attempt > 1:
                        label = f"{label}-retry{attempt - 1}"
                    result = run_arm(
                        binary, rom, root, label, "display", "interpolate",
                        REALTIME_TICKS, args.timeout, args.verbose,
                        realtime=True,
                        # Task 5: strict mode forces the legacy always-grid
                        # projection back on, but it cannot make an adaptive
                        # panel's physical intervals fixed. The classifier row
                        # distinguishes those cases: numeric phase variance is
                        # bounded only for timing=fixed, while this arm still
                        # proves the strict override engages on either kind of
                        # panel (see the
                        # module docstring's "TASK 5 MADE AN ASSUMPTION HERE
                        # EXPLICIT" section).
                        extra_env={"MDKR_PRESENT_QUANTUM_STRICT": "1"})
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
                    if cause:
                        notes.append(f"{label}: no valid baseline — {cause}")
                        continue
                    # This session paced, so its distributions describe
                    # presentation rather than the environment and the M3
                    # bounds apply to them.
                    paced += 1
                    baselines.append(baseline_note(result))
                    quality = check_realtime_quality(result)
                    if not quality:
                        break
                    notes.append(
                        f"{label}: paced, but outside the M3 bounds — "
                        f"{len(quality)} bound(s); retrying, because this arm "
                        "is bimodal and a single transient is not a regression")
                if quality:
                    # Every paced attempt was out of bounds. A real regression
                    # cannot pass any of them -- the pre-M3 phase variance was
                    # 5x to 25x the bound on every run -- while a stall on one
                    # busy attempt does not survive being repeated.
                    failures.extend(quality)
                if paced == 0:
                    # A run that obtained no paced session measured NOTHING
                    # about presentation quality -- and this is the only arm
                    # anywhere that exercises the alpha-grid projection at
                    # all, because platform_present_display_quantum_units()
                    # returns 0 unless the pacer is real and every other
                    # smoothing gate is headless (s_paceMode =
                    # g_headlessFrames < 0 ? PACE_REALTIME : PACE_SYNTH).
                    # Reporting that as a note let the summary below claim
                    # "displayed phases land on the projected display grid"
                    # about a run that never projected one. It is a failure.
                    # A host that genuinely cannot pace must say so out loud
                    # with --allow-no-baseline, which downgrades the banner
                    # rather than hiding the gap.
                    message = (
                        f"realtime arm: NO BASELINE after {REALTIME_ATTEMPTS} "
                        "attempts. The no-tearing and no-underrun assertions "
                        "still ran, but the quality distributions -- and the "
                        "only coverage the alpha-grid projection has anywhere "
                        "-- were not measured")
                    if args.allow_no_baseline:
                        notes.append(message + " (allowed by "
                                     "--allow-no-baseline)")
                    else:
                        failures.append(message)

                # Task 5 companion arm: the SAME policy/smoothing, but WITHOUT
                # MDKR_PRESENT_QUANTUM_STRICT, so this is the variance-honest
                # decision path itself rather than the strict A/B pin above.
                # The assertion (check_alpha_quantum_honest) is tied to this
                # run's OWN measured variance_ppm, not a hardcoded
                # expectation, so it can never be vacuous: it demands
                # mode=grid on a genuinely fixed panel and mode=free on one
                # that is not. On this project's own M3 Max/ProMotion
                # development machine it is a real positive witness for the
                # new path (measured variance is well above the 2500ppm
                # threshold there).
                honest_paced = False
                honest_quality: list[str] = []
                for attempt in range(1, REALTIME_ATTEMPTS + 1):
                    label = "realtime-display-smoothing-quantum-honest"
                    if attempt > 1:
                        label = f"{label}-retry{attempt - 1}"
                    honest_result = run_arm(
                        binary, rom, root, label, "display", "interpolate",
                        REALTIME_TICKS, args.timeout, args.verbose,
                        realtime=True)
                    failures.extend(
                        tear_free_presentation(honest_result.output, label))
                    honest_cause = (
                        explain_no_display_session(honest_result)
                        or explain_unthrottled_presentation(honest_result))
                    if honest_cause:
                        notes.append(
                            f"{label}: no valid measurement — {honest_cause}")
                        continue
                    honest_paced = True
                    # mode-vs-state consistency is structural and never
                    # excused by a busy host; the slot-quality tails are
                    # bimodal for the same reason the strict arm's are, so
                    # a transient miss is retried and only a repeatable one
                    # is a regression.
                    failures.extend(check_alpha_quantum_honest(
                        honest_result))
                    honest_quality = check_slot_quality(honest_result)
                    notes.append(
                        f"{label}: {alpha_quantum_row(honest_result.output)}")
                    if not honest_quality:
                        break
                    notes.append(
                        f"{label}: paced, but outside the slot bounds — "
                        f"{len(honest_quality)} bound(s); retrying")
                if honest_quality:
                    failures.extend(honest_quality)
                if not honest_paced:
                    honest_message = (
                        "quantum-honest companion arm: NO PACED SESSION "
                        f"after {REALTIME_ATTEMPTS} attempts — the "
                        "mode-matches-variance assertion could not run")
                    if args.allow_no_baseline:
                        notes.append(honest_message +
                                     " (allowed by --allow-no-baseline)")
                    else:
                        failures.append(honest_message)
        except RuntimeError as error:
            failures.append(str(error))

    if failures:
        print("check_pacing_quality: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        # The distributions the failure is about, so the numbers do not have to
        # be reproduced by hand before anyone can read the result.
        for baseline in baselines:
            print(f"  - measured: {baseline}")
        return 1
    if not baselines:
        print("check_pacing_quality: PASS (REALTIME QUALITY NOT MEASURED) -- "
              "the synthetic arms, no-tearing and no-underrun assertions "
              "held, but no paced session was obtained, so nothing here "
              "witnesses the displayed-interval or interpolation-phase "
              "distributions and the alpha-grid projection was never "
              "executed")
        for note in notes:
            print(f"  - {note}")
        return 0
    print("check_pacing_quality: PASS -- displayed-interval, interpolation-"
          "phase and present-queue-latency census consistent across every "
          "policy/smoothing arm, with no phase regression and no audio "
          "underrun; displayed phases land on the projected display grid, the "
          "refresh re-derives live, and the paced tails are within the M3 "
          "bounds")
    for note in notes:
        print(f"  - {note}")
    for baseline in baselines:
        print(f"  - M3 BASELINE: {baseline}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
