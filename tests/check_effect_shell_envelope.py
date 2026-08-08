#!/usr/bin/env python3
"""Hold the shield/magnet effect shell inside the interpolation envelope.

An interpolated present is a reconstruction of an image between two authored
ones. Whatever else that reconstruction is free to do, it is not free to put an
object somewhere neither authored image ever put it: a present at alpha in
(0, 1) lies *between* its endpoints, so an object's displacement from its own
tick's authored pose can never exceed the displacement that tick's two authored
poses have from each other. That is the whole content of this gate, and it is
the condition ``docs/evidence/smoothing-artifact-repro-2026-08.md`` §2.6 calls
(a) -- the load-bearing one.

Why a pixel gate exists at all when the same defect has an exact structural
witness (``ownertickcheck``/``ownertickmismatch``, asserted in
``check_smoothing_stage_bisection.py``): the structural witness knows the shape
of the defect that was found. This one does not. It measures where the shell
actually lands on screen, so a future stage that gets its endpoint pairing
wrong in some *different* way -- a stale recipe, a mis-scaled shear, a
substituted racer -- still trips it.

What the diagnosis note establishes, and why the thresholds below are not
tuned: on this route the shell's displacement from its own tick's authored pose
was **50.79 / 51.08 / 50.82 px** at alpha 1/4, 2/4, 3/4 against an
endpoint-to-endpoint envelope of **5.25 px mean, 18.42 px max** over the same
29 authored pairs. The failure margin is 9.7x on the mean and 2.8x against the
single worst authored tick in the window. No tolerance choice adjudicates that.

Two conditions, and a deliberate asymmetry between them:

* **(a) envelope-bound.** Every interpolated present's displacement is at most
  the window's worst authored step, plus a small segmentation tolerance. This
  is a hard failure. It also rejects the *held-matrix* arm (C6 in the note:
  24.53 px at alpha 3/4 over an 18.42 px bound), which is correct and
  deliberate -- disabling the effect stage is not a fix, and a gate that only
  rejected the shipped defect would go green the day someone "fixed" it that
  way.
* **(b) converge-to-zero at alpha -> 0.** The alpha-zero contract is byte-exact
  reproduction of the authored endpoint, so displacement must *start* small and
  grow. A constant offset is the only way to fail this while passing every
  endpoint check, which is what names the shipped defect rather than merely
  detecting it. But (b) is only meaningful when there is a ramp to measure: a
  correctly anchored shell under a chase camera is near-flat AND near-zero, and
  ordering near-zero noise would be a coin flip. So (b) is evaluated only when
  the alpha-3/4 mean clears the envelope mean -- i.e. exactly when the
  displacement is large enough for its shape to mean something -- and is
  reported as ``not-discriminating`` otherwise. That is not an escape hatch:
  when (b) does not apply, (a) has already passed with room to spare, which is
  the same statement.

One arm, because condition (a) needs only the production build. The contrast
arms that made the diagnosis readable (effect-off, all-off, 60 Hz) are in the
note; re-running them here would triple the gate's cost to re-derive a number
that is not the pass criterion.

Always muted + headless. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, parse_last, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
HASH_VERSION = "3"

# The diagnosis route, verbatim. Level 29 Jungle Falls under autopilot at the
# 120 Hz alpha grid: four presents per authored tick, so every fourth dumped
# frame is the real walk's own image and the three between it are the
# reconstructions this gate measures. The window is authored ticks 3200-3230,
# after the countdown clears around 3120 -- before it the racers are
# screen-static and every stage reads as innocent for the wrong reason.
PRESENT_RATE = "120"
PRESENTS_PER_TICK = 4
ROUTE_TICKS = 3230
ROUTE_TRACK = "29"
DUMP_FRAMES = 120

# MDKR_FORCE_SHIELD is expressed in AUTHORED TICKS. This window brackets the
# dump so the effect stage is live on every frame measured, and because the
# hook counts g_simTickCounter it does NOT need rescaling when the present rate
# changes -- the present-counted form lost a whole 60 Hz arm (effectreg=0) to
# exactly that trap in the 2026-08 diagnosis.
FORCE_SHIELD = "3170:150"

# The additive green shell against the level. Fixed thresholds, not adaptive:
# the shell is drawn additively over whatever is behind it, so a relative
# criterion would track the background instead of the object.
SHELL_G_OVER_R = 45
SHELL_G_OVER_B = 45
SHELL_G_MIN = 110
SHELL_MIN_PIXELS = 200

# Full-frame pixels of the 640x480 backend dump, origin top-left. The top edge
# clears the HUD band and the rival shells. Containment is asserted per frame
# rather than assumed -- the defect under measurement MOVES the shell, so a
# shell that left the crop would produce a smaller area and a biased centroid
# that could be mistaken for the defect's own signature. The 2026-08 diagnosis
# hit this for real with a tighter window and biased its central figure ~37%
# low before catching it.
WINDOW_Y0, WINDOW_Y1 = 150, 480
WINDOW_X0, WINDOW_X1 = 120, 600

# Segmentation tolerance on condition (a). The measured quantity is a centroid
# of a thresholded blob whose silhouette changes as it rotates, so two frames
# of a perfectly rigid object do not report centroids that agree to the pixel.
# 2.0 px is well under the 5.25 px the shell travels in a whole authored tick,
# and 25x under the 50.8 px the shipped defect displaces it by, so it cannot
# decide either direction of this gate's verdict.
ENVELOPE_TOLERANCE_PX = 2.0

# Condition (b) is evaluated only above this multiple of the envelope mean --
# see the module docstring. At or below it the displacement has no ramp to
# have a shape.
CONVERGE_APPLIES_ABOVE_ENVELOPE_MEAN = 1.0
# A genuine ramp puts alpha 1/4 at roughly a third of alpha 3/4. The shipped
# defect reads 0.9994 (50.79 / 50.82); the held-matrix arm reads 0.30
# (7.32 / 24.53) and must pass, since it converges to zero exactly as (b)
# requires. 0.6 is the midpoint of a gap that spans more than a factor of
# three.
CONVERGE_MAX_RATIO = 0.6

# [PRESENT-PACKET] fields this gate reads. Presence is asserted before any
# value is: a dict lookup with a default silently passes the day a counter is
# renamed, which turns the strongest claim a gate makes into a no-op.
PACKET_STAT_CONTRACT = (
    "effecthit", "effectoverride", "ownertickcheck", "ownertickmismatch")



RENDERER = "webgpu"

def read_ppm(path: Path) -> tuple[int, int, memoryview]:
    """Parse a binary P6 dump into (width, height, RGB bytes)."""

    data = path.read_bytes()
    fields: list[bytes] = []
    offset = 0
    while len(fields) < 4:
        while offset < len(data) and data[offset : offset + 1].isspace():
            offset += 1
        if data[offset : offset + 1] == b"#":
            while offset < len(data) and data[offset] != 0x0A:
                offset += 1
            continue
        start = offset
        while offset < len(data) and not data[offset : offset + 1].isspace():
            offset += 1
        fields.append(data[start:offset])
    if fields[0] != b"P6" or fields[3] != b"255":
        raise RuntimeError(f"{path}: not an 8-bit binary PPM")
    width = int(fields[1])
    height = int(fields[2])
    return width, height, memoryview(data)[offset + 1 :]


def shell_mask(width: int, height: int, pixels: memoryview) -> set[int]:
    """Pixel indices of the additive green shell inside the crop window."""

    mask: set[int] = set()
    y1 = min(height, WINDOW_Y1)
    x1 = min(width, WINDOW_X1)
    for y in range(WINDOW_Y0, y1):
        row = y * width
        base = row * 3
        for x in range(WINDOW_X0, x1):
            index = base + x * 3
            g = pixels[index + 1]
            if (g > pixels[index] + SHELL_G_OVER_R
                    and g > pixels[index + 2] + SHELL_G_OVER_B
                    and g > SHELL_G_MIN):
                mask.add(row + x)
    return mask


def largest_component(mask: set[int], width: int) -> set[int]:
    """8-connected largest component. The shell is one object; rival shells and
    HUD spill are separate ones and must not drag the centroid."""

    remaining = set(mask)
    best: set[int] = set()
    while remaining:
        if len(remaining) <= len(best):
            break
        seed = remaining.pop()
        component = {seed}
        stack = [seed]
        while stack:
            here = stack.pop()
            y, x = divmod(here, width)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    neighbour = (y + dy) * width + (x + dx)
                    if neighbour in remaining:
                        remaining.discard(neighbour)
                        component.add(neighbour)
                        stack.append(neighbour)
        if len(component) > len(best):
            best = component
    return best


def shell(path: Path) -> tuple[int, float, float] | None:
    """(area, cx, cy) of the shell in FULL-FRAME pixels, or None if absent.

    Raises when the component touches a crop edge: past that point the area and
    the centroid are both measuring the crop rather than the shell, and a
    number that measures the instrument is worse than no number.
    """

    width, height, pixels = read_ppm(path)
    mask = shell_mask(width, height, pixels)
    if len(mask) < SHELL_MIN_PIXELS:
        return None
    component = largest_component(mask, width)
    ys = [index // width for index in component]
    xs = [index % width for index in component]
    if (min(ys) <= WINDOW_Y0 or max(ys) >= min(height, WINDOW_Y1) - 1
            or min(xs) <= WINDOW_X0 or max(xs) >= min(width, WINDOW_X1) - 1):
        raise RuntimeError(
            f"{path.name}: the shell component's bounding box "
            f"(y {min(ys)}..{max(ys)}, x {min(xs)}..{max(xs)}) touches the "
            f"measurement window (y {WINDOW_Y0}..{WINDOW_Y1}, "
            f"x {WINDOW_X0}..{WINDOW_X1}). Every figure this gate would report "
            "is a property of the crop, not of the shell. Widen the window and "
            "re-derive the envelope before trusting either verdict")
    return len(component), sum(xs) / len(xs), sum(ys) / len(ys)


def run(binary: Path, rom: Path, root: Path, timeout: int,
        verbose: bool) -> tuple[str, Path]:
    run_dir = root / "route-effect-envelope"
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    frame_dir = run_dir / "frames"
    frame_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_EVENT_HASH="1",
        MDKR_INPUT_HASH="1",
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=ROUTE_TRACK,
        # WebGPU is what players run. GL is kept selectable because it is
        # the diagnostic backend, but the default has to be the renderer
        # whose picture ships -- an interpolation-quality claim proven
        # only on GL proves nothing about the shipped one, which is how
        # the replay-admission starvation stayed invisible.
        MDKR_RENDERER=RENDERER,
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
        MDKR_PRESENT_RATE=PRESENT_RATE,
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_FORCE_SHIELD=FORCE_SHIELD,
        MDKR_DUMP_FROM=str(ROUTE_TICKS * PRESENTS_PER_TICK - DUMP_FRAMES),
    )
    command = [
        str(binary), "--headless-ticks", str(ROUTE_TICKS),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", "320x240", "--dump-frames", str(frame_dir),
    ]
    if verbose:
        print(f"$ {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False)
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(f"exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer"):
        if marker in output:
            raise RuntimeError(f"fatal marker {marker}")
    return output, frame_dir


def mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def stdev(values: list[float]) -> float:
    if len(values) < 2:
        return 0.0
    average = mean(values)
    return math.sqrt(
        sum((value - average) ** 2 for value in values) / (len(values) - 1))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--renderer", choices=("webgpu", "gl"),
                        default="webgpu",
                        help="backend under test (default: the shipped one)")
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    global RENDERER
    RENDERER = args.renderer

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []
    rows: list[str] = []

    with tempfile.TemporaryDirectory(prefix="mdkr-effect-envelope-") as tmp:
        try:
            output, frame_dir = run(
                binary, rom, Path(tmp), args.timeout, args.verbose)
        except Exception as error:  # noqa: BLE001 - reported, not swallowed
            print(f"FAIL: {error}", file=sys.stderr)
            return 1

        packet = parse_last(output, "PRESENT-PACKET")
        missing = [key for key in PACKET_STAT_CONTRACT if key not in packet]
        if missing:
            print(
                "FAIL: [PRESENT-PACKET] carries no "
                f"{'/'.join(missing)} field. Those are this gate's stat "
                "contract (gfx_presentation_packet_get_stats -> "
                "present_sched.c); without them the run below measures pixels "
                "with no idea whether the stage under test even ran",
                file=sys.stderr)
            return 1
        if packet["effectoverride"] == 0:
            print(
                "FAIL: effectoverride=0 — the effect stage never substituted "
                "anything on this route, so every displacement measured below "
                "belongs to a stage that did not run. MDKR_FORCE_SHIELD is in "
                "PRESENT indices and needs rescaling if the rate moved",
                file=sys.stderr)
            return 1
        # The structural witness for the same defect. Cheap here, and it says
        # which of the two possible reasons a green pixel verdict has.
        #
        # Non-vacuity before value, for the same reason presence comes before
        # both: `mdkr_camera_replay_tick_agreement` returns early WITHOUT
        # counting when a recipe carries no stamp, and
        # `presentation_task_authoring_tick()` returns 0 outside an authoring
        # lifetime. So a stamping regression does not make the mismatch counter
        # rise -- it makes the whole witness stop counting, and a zero
        # mismatch over zero checks would read green while asserting nothing.
        # The effect stage has already been shown to fire above, so recipes
        # were replayed and this count cannot legitimately be zero.
        if packet["ownertickcheck"] == 0:
            print(
                "FAIL: ownertickcheck=0 over effectoverride="
                f"{packet['effectoverride']} — recipes were replayed but none "
                "was checked against its own capture tick, so "
                "ownertickmismatch=0 says nothing. The capture stamp has gone "
                "missing (presentation_task_authoring_tick -> "
                "GfxPresentationMatrixOwner.capture_tick)",
                file=sys.stderr)
            return 1
        if packet["ownertickmismatch"] != 0:
            failures.append(
                f"ownertickmismatch={packet['ownertickmismatch']} of "
                f"{packet['ownertickcheck']} checks — a replay resolved its "
                "residual against a capture from a different authored tick "
                "than the one it measured `authored` at. See "
                "docs/evidence/smoothing-artifact-repro-2026-08.md §2.3")

        frames = sorted(
            frame_dir.glob("*.ppm"), key=lambda p: int(p.stem.split("_")[1]))
        if len(frames) != DUMP_FRAMES:
            print(
                f"FAIL: dumped {len(frames)} frames, expected {DUMP_FRAMES}",
                file=sys.stderr)
            return 1

        try:
            poses = {
                int(path.stem.split("_")[1]): shell(path) for path in frames}
        except RuntimeError as error:
            print(f"FAIL: {error}", file=sys.stderr)
            return 1

    missing_shell = sorted(
        index for index, pose in poses.items() if pose is None)
    if missing_shell:
        print(
            f"FAIL: no shell segmented on {len(missing_shell)} frames "
            f"(first {missing_shell[0]}). The colour key or the forced-shield "
            "window has drifted from the route",
            file=sys.stderr)
        return 1

    indices = sorted(poses)
    authored = [index for index in indices if index % PRESENTS_PER_TICK == 0]

    # The envelope: how far the shell moves between two AUTHORED images. This
    # is the entire budget one tick of interpolation has to spend, derived from
    # this run rather than carried as a literal, so a route or content change
    # re-derives its own bound instead of being judged against a stale one.
    steps = []
    for first, second in zip(authored, authored[1:]):
        if second - first != PRESENTS_PER_TICK:
            continue
        _, x0, y0 = poses[first]
        _, x1, y1 = poses[second]
        steps.append(math.hypot(x1 - x0, y1 - y0))
    if len(steps) < 8:
        print(
            f"FAIL: only {len(steps)} adjacent authored pairs in the window, "
            "which is too few to bound anything",
            file=sys.stderr)
        return 1
    envelope_mean = mean(steps)
    envelope_max = max(steps)
    bound = envelope_max + ENVELOPE_TOLERANCE_PX
    rows.append(
        f"[SHELL-ENVELOPE] pairs={len(steps)} mean={envelope_mean:.2f} "
        f"max={envelope_max:.2f} bound={bound:.2f}")

    # Displacement of every interpolated present from the authored present that
    # OPENS ITS OWN TICK. Not from the nearest authored frame, and not from the
    # other arm: both of those are satisfied by a shell that is coherently in
    # the wrong place, which is exactly the defect this gate exists for.
    by_alpha: dict[int, list[float]] = {1: [], 2: [], 3: []}
    violations: list[tuple[int, int, float]] = []
    for index in indices:
        alpha = index % PRESENTS_PER_TICK
        if alpha == 0:
            continue
        opener = index - alpha
        if opener not in poses:
            continue
        _, x0, y0 = poses[opener]
        _, x1, y1 = poses[index]
        distance = math.hypot(x1 - x0, y1 - y0)
        by_alpha[alpha].append(distance)
        if distance > bound:
            violations.append((index, alpha, distance))

    for alpha in (1, 2, 3):
        series = by_alpha[alpha]
        areas = [poses[index][0] for index in indices
                 if index % PRESENTS_PER_TICK == alpha]
        rows.append(
            f"[SHELL-DISPLACEMENT] alpha={alpha}/{PRESENTS_PER_TICK} "
            f"n={len(series)} mean={mean(series):.2f} sd={stdev(series):.2f} "
            f"max={max(series):.2f} area={mean(areas):.0f}")

    # Condition (a).
    if violations:
        worst = max(violations, key=lambda row: row[2])
        rows.append(
            f"[ENVELOPE-BOUND] violations={len(violations)}/"
            f"{sum(len(v) for v in by_alpha.values())} worst={worst[2]:.2f}px "
            f"at frame={worst[0]} verdict=FAIL")
        failures.append(
            f"condition (a): {len(violations)} of "
            f"{sum(len(v) for v in by_alpha.values())} interpolated presents "
            f"put the shell further from its own tick's authored pose "
            f"({worst[2]:.2f} px at worst) than the WHOLE WINDOW's worst "
            f"authored step moves it ({envelope_max:.2f} px, {bound:.2f} px "
            f"with tolerance; the window mean is {envelope_mean:.2f} px). The "
            "bound is deliberately the window maximum rather than each "
            "present's own tick's step, so this is the loose form of the "
            "condition and failing it is unambiguous: an interpolated present "
            "lies between its endpoints, and this one does not lie between "
            "anything in the window. See "
            "docs/evidence/smoothing-artifact-repro-2026-08.md §2.6 "
            "condition (a)")
    else:
        rows.append(
            f"[ENVELOPE-BOUND] violations=0/"
            f"{sum(len(v) for v in by_alpha.values())} "
            f"worst={max(max(v) for v in by_alpha.values()):.2f}px "
            f"bound={bound:.2f}px verdict=PASS")

    # Condition (b), where it discriminates.
    low = mean(by_alpha[1])
    high = mean(by_alpha[3])
    if high <= envelope_mean * CONVERGE_APPLIES_ABOVE_ENVELOPE_MEAN:
        rows.append(
            f"[ALPHA-CONVERGENCE] low={low:.2f} high={high:.2f} "
            f"envelopemean={envelope_mean:.2f} verdict=not-discriminating")
    else:
        ratio = low / high if high else 0.0
        verdict = "PASS" if ratio <= CONVERGE_MAX_RATIO else "FAIL"
        rows.append(
            f"[ALPHA-CONVERGENCE] low={low:.2f} high={high:.2f} "
            f"ratio={ratio:.4f} max={CONVERGE_MAX_RATIO} verdict={verdict}")
        if verdict == "FAIL":
            failures.append(
                f"condition (b): displacement at alpha 1/4 is {low:.2f} px "
                f"against {high:.2f} px at alpha 3/4 (ratio {ratio:.4f}). An "
                "interpolated quantity converges to its authored endpoint as "
                "alpha falls; this one is already at full magnitude on the "
                "first interpolated frame, which is a constant offset wearing "
                "an interpolation's clothes. See "
                "docs/evidence/smoothing-artifact-repro-2026-08.md §2.6 "
                "condition (b)")

    rows.append(
        f"[EFFECT-STAGE] effecthit={packet['effecthit']} "
        f"effectoverride={packet['effectoverride']} "
        f"ownertickcheck={packet['ownertickcheck']} "
        f"ownertickmismatch={packet['ownertickmismatch']}")

    for row in rows:
        print(row)
    if failures:
        print("", file=sys.stderr)
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: the effect shell stays inside the interpolation envelope")
    return 0


if __name__ == "__main__":
    sys.exit(main())
