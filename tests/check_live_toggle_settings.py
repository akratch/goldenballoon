#!/usr/bin/env python3
"""A presentation setting a player can change mid-run must change only the picture.

Video.FrameLimit, Video.MotionSmoothing, Video.AllowTearing and
Camera.Obstruction were SCOPE_RESTART because their receivers latch, and a
setter that wrote them under a running engine was unsafe in both directions.
They are LIVE (the first three) and LEVEL (the camera) now, applied by
video_config.h's deferred-apply mechanism at a declared boundary. This gate is
what makes that claim checkable rather than argued.

THE HAZARD IT EXISTS FOR. gfx_pc_dkr.c's replay holds walk-entry state -- the
saved RDP/RSP registers and, fatally, the saved SEGMENT TABLE -- refreshed by
gfx_start_frame only while present_sched_replay_armed() is true and cleared by
nothing but gfx_dkr_replay_invalidate(). Turn motion smoothing off and the
arming flag goes false while the already-latched subloop keeps running; turn it
back on and the subloop replays against a segment table captured before the
change, whose bases point at freed level memory. The fix is that the apply is
not the setter at all: the boundary applier invalidates the replay history
FIRST, in both directions, so no armed subloop can ever reach a walk entry
captured under the other policy.

The `soak` arm drives that exact sequence -- off/on/off/on across a level load,
many times, at a rate whose subloop is genuinely running -- and the ASan lane
is what turns "no crash" into "no wild read". A stale segment base is a
use-after-free of level memory, which is precisely what ASan reports and what a
release build would happily dereference into a plausible-looking image.

WHAT EVERY ARM ASSERTS.

  no fault      exit 0 and no crash/assert/sanitizer marker.
  it applied    one [SETTINGS-APPLY] row per changed key, naming old and new,
                at the boundary that key's scope declares -- and for the
                presentation domain, a [PRESENT-POLICY] event=live-apply row
                and a re-emitted [PRESENT-MODE] row proving the change reached
                the pacer and the swapchain rather than only the config.
  it is only    the v3 authority, ordered gameplay-event and consumed-input
  the picture   streams are BYTE-IDENTICAL to a run that never toggled. These
                are presentation settings; a state hash that moves means one of
                them is not.
  nothing stale [REPLAY-SUMMARY] staletenants=0 and restorefail=0. The shadow
                registry's stale-identity counter is the nearest published
                observable to the hazard class, and a policy flip that left a
                retired tenant reachable would show up here first.

THE CAMERA ARM ASSERTS THE DEFERRAL ITSELF, in the direction that matters: the
census must still report gate=OBSERVE on every tick between the toggle and the
level load, and gate=MODERN only after. A frame boundary that applied the
camera domain early would be a mid-race cut with no other symptom, and this is
the only thing that would see it.

WHY LAUNCH-RANK VALUES AND NOT THE ENVIRONMENT. The arms need a base policy the
in-game setter is allowed to replace. MDKR_PRESENT_RATE would pin the key at
ENV rank, above RUNTIME, and every toggle would resolve as LOCKED -- a gate
that passes by testing nothing. --video-launch-set is LAUNCHER rank, below
RUNTIME, which is exactly the situation a player is in.

Always muted + headless. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (ABORT_MARKERS, ASSERT_MARKERS, DEFAULT_BUILD_DIR,
                           find_fatal, parse_last, present_mode_rows,
                           resolve_binary)

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"

# Long enough that the route crosses at least one level load (the camera arm
# needs one) and that a subloop-engaged run accumulates real replay traffic;
# short enough that six arms stay a few minutes.
TICKS = 600
HASH_VERSION = "3"

# The base policy every arm starts from. 120 is above the 30 Hz authoritative
# tick rate, so the presentation subloop is genuinely engaged and the replay
# path this gate is about is genuinely running -- a base of `original` would
# make the smoothing arms toggle a feature that was never on.
BASE_FRAME_LIMIT = "120"
BASE_SMOOTHING = "interpolate"

# Toggle ticks. Both are past the boot/load transient, and far enough apart that
# each apply has many host opportunities to be observed at.
FIRST_TICK = 150
SECOND_TICK = 300

# The soak. 24 flips over the run is roughly one every 20 ticks: often enough
# that a leaked walk entry has many chances to be replayed, spaced enough that
# each flip's subloop actually runs between them.
SOAK_FLIPS = 24
SOAK_FIRST_TICK = 100
SOAK_PERIOD = 20

CAMERA_TICK = 40

SETTINGS_APPLY_RE = re.compile(
    r"\[SETTINGS-APPLY\] domain=(\S+) boundary=(\S+) key=(\S+) "
    r"old=(\S+) new=(\S+)")
SETTINGS_TOGGLE_RE = re.compile(
    r"\[SETTINGS-TOGGLE\] tick=(\d+) key=(\S+) value=(\S+) result=(\d+) "
    r"applied=(\d+)")
LIVE_APPLY_RE = re.compile(
    r"\[PRESENT-POLICY\] event=live-apply policy=(\S+) rate=(\d+) "
    r"smoothing=(\d+) tearing=(\d+) subloopWas=(\d+) subloopNow=(\d+)")
CAMERA_GATE_RE = re.compile(r"gate=([A-Z_-]+)\(")


@dataclass(frozen=True)
class Arm:
    label: str
    toggles: tuple[tuple[str, str, int], ...]   # key, value, tick
    camera_trace: bool = False


@dataclass(frozen=True)
class Result:
    label: str
    output: str
    state: list[str]
    events: list[str]
    inputs: list[str]
    replay: dict[str, int]
    packet: dict[str, int]
    retained: dict[str, int]
    summary: dict[str, int]


def stream(output: str, marker: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith(marker)]


def toggle_env(toggles: tuple[tuple[str, str, int], ...]) -> str:
    return ",".join(f"{key}={value}@{tick}" for key, value, tick in toggles)


def run(binary: Path, rom: Path, root: Path, arm: Arm, timeout: int,
        verbose: bool) -> Result:
    run_dir = root / arm.label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    # A clean environment, and in particular no MDKR_PRESENT_* : an inherited
    # one would pin the very keys the arms toggle at a rank the setter refuses.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_EVENT_HASH="1",
        MDKR_INPUT_HASH="1",
        MDKR_LOAD_TRACK="5",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_SYNTH_FIELDS="2",
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
        # The setter PERSISTS. Without this every arm would rewrite the
        # developer's real settings file, and the arms would see each other's
        # writes instead of the launch-rank base they were given.
        MDKR_VIDEO_CONFIG_PATH=str(run_dir / "mdkr64.ini"),
    )
    if arm.camera_trace:
        env["MDKR_CAMERA_TRACE"] = "1"
    if arm.toggles:
        env["MDKR_TEST_SETTINGS_TOGGLE"] = toggle_env(arm.toggles)
    command = [
        str(binary), "--headless-ticks", str(TICKS),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", "320x240",
        "--video-launch-set", f"Video.FrameLimit={BASE_FRAME_LIMIT}",
        "--video-launch-set", f"Video.MotionSmoothing={BASE_SMOOTHING}",
    ]
    if verbose:
        print(f"$ ({arm.label}) MDKR_TEST_SETTINGS_TOGGLE="
              f"{env.get('MDKR_TEST_SETTINGS_TOGGLE', '')} "
              f"{' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"{arm.label}: exit {process.returncode}\n{output[-3000:]}")
    # find_fatal already covers [CRASH]/[FATAL]/AddressSanitizer/UBSan/runtime
    # error; `extra` is REGEX SOURCE, so re-passing them as literals would make
    # "[CRASH]" a character class that matches a lone S in any summary row.
    marker = find_fatal(output, *ABORT_MARKERS, *ASSERT_MARKERS)
    if marker:
        raise RuntimeError(f"{arm.label}: fatal marker {marker}\n"
                           f"{output[-3000:]}")
    return Result(
        arm.label,
        output,
        stream(output, "[SIMHASH]"),
        stream(output, "[EVENTHASH]"),
        stream(output, "[INPUTHASH]"),
        parse_last(output, "REPLAY-SUMMARY"),
        parse_last(output, "PRESENT-PACKET"),
        parse_last(output, "RETAINED-TASK"),
        parse_last(output, "PRESENTSCHED-SUMMARY"),
    )


def check_streams(result: Result, baseline: Result) -> list[str]:
    """Presentation-only means the authoritative streams do not move. Ever."""
    failures: list[str] = []
    for name, actual, reference in (
            ("v3 state", result.state, baseline.state),
            ("ordered event", result.events, baseline.events),
            ("consumed input", result.inputs, baseline.inputs)):
        if actual == reference:
            continue
        index = next(
            (i for i, pair in enumerate(zip(reference, actual))
             if pair[0] != pair[1]),
            min(len(reference), len(actual)),
        )
        expected = reference[index] if index < len(reference) else "<missing>"
        got = actual[index] if index < len(actual) else "<missing>"
        failures.append(
            f"{result.label}: {name} stream diverged at row {index}; a "
            f"presentation setting moved authoritative state\n"
            f"    baseline: {expected}\n    toggled:  {got}")
    return failures


def check_no_stale(result: Result) -> list[str]:
    """The published detectors nearest the stale-walk hazard, all held at zero.

    None of these is the hazard itself -- the hazard's symptom is a wild read
    that a release build cannot see. They are the things the renderer ALREADY
    counts that a replay reaching retired state would disturb on its way there:

      staletenants        the shadow registry served a matrix identity whose
                          tenant had been retired -- the same class of mistake
                          one level generation later.
      restorefail /       a replay could not restore the frozen registry, or a
      freezefail          real walk could not freeze it. After a live re-arm
                          both must simply not happen: the applier retires the
                          history, and the next real walk builds a fresh one.
      packet stale /      the presentation packet fell back to held or stale
      unsafestalefallback data, or its dependency check disagreed with what the
      dependencymismatch  walk actually consumed.
      retained rejects /  the retained task refused an acquire, failed a
      failures /          capture, or -- livePoison -- observed the replay
      livePoison          reading the LIVE mutable arena instead of its own
                          immutable copy, which is the read a stale walk entry
                          would perform.
    """
    failures: list[str] = []
    for tag, row, fields in (
            ("REPLAY-SUMMARY", result.replay,
             ("staletenants", "restorefail", "freezefail")),
            ("PRESENT-PACKET", result.packet,
             ("stale", "stalematrixhold", "stalevertexhold",
              "unsafestalefallback", "dependencymismatch", "freezefail")),
            ("RETAINED-TASK", result.retained,
             ("failures", "rejects", "budgetRejects", "livePoison"))):
        for field in fields:
            value = row.get(field)
            if value is None:
                failures.append(f"{result.label}: [{tag}] carried no {field}")
            elif value != 0:
                failures.append(
                    f"{result.label}: [{tag}] {field}={value}; a policy change "
                    f"left retired replay state reachable")
    return failures


def applies(output: str, domain: str) -> list[tuple[str, str, str, str, str]]:
    return [match.groups() for match in SETTINGS_APPLY_RE.finditer(output)
            if match.group(1) == domain]


def check_toggles_landed(result: Result, arm: Arm) -> list[str]:
    """Every scripted edit must have been ACCEPTED, not merely attempted.

    A key pinned above RUNTIME rank resolves as LOCKED and the run would
    otherwise pass while having toggled nothing -- the single most likely way
    for this gate to become decorative.
    """
    failures: list[str] = []
    rows = SETTINGS_TOGGLE_RE.findall(result.output)
    if len(rows) != len(arm.toggles):
        failures.append(
            f"{result.label}: {len(rows)} [SETTINGS-TOGGLE] rows for "
            f"{len(arm.toggles)} scripted edits")
    for _tick, key, value, _code, applied in rows:
        if applied != "1":
            failures.append(
                f"{result.label}: the engine refused {key}={value}; the arm "
                f"toggled nothing (a higher-rank layer is pinning it)")
    return failures


def check_presentation_apply(result: Result, expected: list[tuple[str, str, str]]
                             ) -> list[str]:
    """One ordered apply per transaction, naming old -> new, and reaching both
    the pacer ([PRESENT-POLICY] event=live-apply) and the backend's present-mode
    ranking (a re-emitted [PRESENT-MODE])."""
    failures: list[str] = []
    rows = applies(result.output, "presentation")
    got = [(key, old, new) for _d, _b, key, old, new in rows]
    if got != expected:
        failures.append(
            f"{result.label}: presentation applies were {got}, expected "
            f"{expected}")
    for _domain, boundary, key, _old, _new in rows:
        if boundary != "frame":
            failures.append(
                f"{result.label}: {key} applied at boundary={boundary}; a LIVE "
                f"key must apply at the host-frame boundary")
    live = LIVE_APPLY_RE.findall(result.output)
    if len(live) != len(expected):
        failures.append(
            f"{result.label}: {len(live)} [PRESENT-POLICY] event=live-apply "
            f"rows for {len(expected)} applies; the pacer was not re-latched")
    # The boot row plus one per apply. Fewer means the backend kept the ranking
    # it baked at configuration, which is the bug Video.AllowTearing's old
    # RESTART scope was standing in for.
    modes = len(present_mode_rows(result.output))
    if modes < 1 + len(expected):
        failures.append(
            f"{result.label}: {modes} [PRESENT-MODE] rows for "
            f"{len(expected)} applies; the present mode was not re-ranked")
    return failures


def check_camera_arm(result: Result) -> list[str]:
    """The LEVEL deferral, asserted in the direction that can go wrong silently."""
    failures: list[str] = []
    rows = applies(result.output, "camera")
    if len(rows) != 1:
        failures.append(
            f"{result.label}: {len(rows)} camera applies, expected 1")
        return failures
    # modern -> observe: the corrected camera is the default now, so the change
    # a player actually makes here is choosing the original camera back.
    _domain, boundary, key, old, new = rows[0]
    if (boundary, key, old, new) != ("level", "Camera.Obstruction",
                                     "modern", "observe"):
        failures.append(
            f"{result.label}: camera apply was {(boundary, key, old, new)}, "
            f"expected ('level', 'Camera.Obstruction', 'modern', 'observe')")

    lines = result.output.splitlines()
    toggle_at = next((i for i, line in enumerate(lines)
                      if "[SETTINGS-TOGGLE]" in line), None)
    apply_at = next((i for i, line in enumerate(lines)
                     if "[SETTINGS-APPLY] domain=camera" in line), None)
    if toggle_at is None or apply_at is None:
        failures.append(f"{result.label}: no toggle/apply pair to order")
        return failures
    if apply_at <= toggle_at:
        failures.append(
            f"{result.label}: the camera apply preceded the edit that staged it")

    def gates(window: list[str]) -> set[str]:
        found: set[str] = set()
        for line in window:
            found.update(CAMERA_GATE_RE.findall(line))
        return found

    # THE ASSERTION THIS ARM EXISTS FOR. Between the edit and the level load the
    # engine must still be running the OLD policy: a frame boundary that applied
    # the camera domain early is a mid-race cut, and the census is the only
    # thing that would ever report it. The arm now toggles modern -> observe,
    # so the old policy is MODERN and the staged one is OBSERVE; the direction
    # is what changed, not the claim.
    during = gates(lines[toggle_at:apply_at])
    if "OBSERVE" in during:
        failures.append(
            f"{result.label}: the census reported OBSERVE before the level "
            f"boundary; a LEVEL key was applied mid-race")
    if "MODERN" not in during:
        failures.append(
            f"{result.label}: no MODERN census rows between the edit and the "
            f"level load; the arm proved nothing about the wait")
    after = gates(lines[apply_at:])
    if "OBSERVE" not in after:
        failures.append(
            f"{result.label}: the census never reported OBSERVE after the level "
            f"boundary; the applied policy did not reach the camera runtime")
    return failures


def soak_toggles() -> tuple[tuple[str, str, int], ...]:
    """Alternating smoothing flips, starting by turning it OFF.

    Off-then-on is the hazard's own order: turning it off is what stops
    gfx_start_frame refreshing the walk entry, and turning it back on is what
    would replay the stale one.
    """
    return tuple(
        ("Video.MotionSmoothing",
         "off" if flip % 2 == 0 else "interpolate",
         SOAK_FIRST_TICK + flip * SOAK_PERIOD)
        for flip in range(SOAK_FLIPS)
    )


ARMS: tuple[Arm, ...] = (
    Arm("baseline", ()),
    Arm("smoothing",
        (("Video.MotionSmoothing", "off", FIRST_TICK),
         ("Video.MotionSmoothing", "interpolate", SECOND_TICK))),
    Arm("framelimit",
        (("Video.FrameLimit", "original", FIRST_TICK),
         ("Video.FrameLimit", "120", SECOND_TICK))),
    Arm("tearing", (("Video.AllowTearing", "on", FIRST_TICK),)),
    Arm("camera", (("Camera.Obstruction", "observe", CAMERA_TICK),),
        camera_trace=True),
    Arm("soak", soak_toggles()),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default=str(ROOT / "baserom.us.v80.z64"))
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument(
        "--soak-only", action="store_true",
        help="Run only the hazard soak. This is what the ASan lane wants: the "
             "other arms assert stream identity, which a sanitizer build's "
             "timing does not change but whose runtime cost is pure overhead.")
    args = parser.parse_args()

    # Absolute: every arm runs in its own cwd, so a relative build path would
    # resolve against the temporary directory instead of the checkout.
    binary = Path(resolve_binary(args.build)).resolve()
    # Absolute for the same reason as the binary: the arms run in temporaries.
    rom = Path(args.rom).resolve()
    if not rom.is_file():
        print(f"rom not found: {rom}", file=sys.stderr)
        return 1

    arms = ARMS
    if args.soak_only:
        arms = tuple(arm for arm in ARMS if arm.label in ("baseline", "soak"))

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-live-toggle-") as temporary:
        root = Path(temporary)
        results: dict[str, Result] = {}
        for arm in arms:
            results[arm.label] = run(binary, rom, root, arm, args.timeout,
                                     args.verbose)
            print(f"[live-toggle] {arm.label}: ran {TICKS} ticks with "
                  f"{len(arm.toggles)} scripted edit(s)", flush=True)

        baseline = results["baseline"]
        if applies(baseline.output, "presentation") or \
                applies(baseline.output, "camera"):
            failures.append(
                "baseline: a run with no scripted edit emitted a "
                "[SETTINGS-APPLY] row; the boundary is applying on its own")

        for arm in arms:
            result = results[arm.label]
            failures += check_no_stale(result)
            if arm.label == "baseline":
                continue
            failures += check_toggles_landed(result, arm)
            # (c) byte-identity of the authoritative streams. This is the whole
            # justification for calling these settings presentation-only.
            failures += check_streams(result, baseline)

        if "smoothing" in results:
            failures += check_presentation_apply(
                results["smoothing"],
                [("Video.MotionSmoothing", "interpolate", "off"),
                 ("Video.MotionSmoothing", "off", "interpolate")])
        if "framelimit" in results:
            failures += check_presentation_apply(
                results["framelimit"],
                [("Video.FrameLimit", "120", "original"),
                 ("Video.FrameLimit", "original", "120")])
            # The subloop must actually engage and disengage: a FrameLimit that
            # reached the config but not the pacer would leave it latched, which
            # is the exact thing SCOPE_RESTART used to be protecting.
            live = LIVE_APPLY_RE.findall(results["framelimit"].output)
            if len(live) == 2:
                if live[0][5] != "0":
                    failures.append(
                        "framelimit: original did not disengage the subloop "
                        f"(subloopNow={live[0][5]})")
                if live[1][5] != "1":
                    failures.append(
                        "framelimit: 120 did not re-engage the subloop "
                        f"(subloopNow={live[1][5]})")
        if "tearing" in results:
            failures += check_presentation_apply(
                results["tearing"], [("Video.AllowTearing", "off", "on")])
            live = LIVE_APPLY_RE.findall(results["tearing"].output)
            # The backend's headless automation swap deliberately ignores a
            # tearing opt-in (harness_utils.tear_free_presentation documents
            # why), so the reachable claim is that the value got as far as the
            # pacer's own view of the policy.
            if not live or live[0][3] != "1":
                failures.append(
                    "tearing: the live apply did not report tearing=1; the "
                    "opt-in never reached present_sched")
        if "camera" in results:
            failures += check_camera_arm(results["camera"])
        if "soak" in results:
            soak = results["soak"]
            rows = applies(soak.output, "presentation")
            if len(rows) != SOAK_FLIPS:
                failures.append(
                    f"soak: {len(rows)} applies for {SOAK_FLIPS} flips; the "
                    f"hazard sequence did not run to completion")
            # Every flip must have alternated. A run that coalesced them would
            # never re-arm the replay and would prove nothing.
            news = [new for _d, _b, _k, _old, new in rows]
            if news != ["off" if i % 2 == 0 else "interpolate"
                        for i in range(len(news))]:
                failures.append(
                    f"soak: applies did not alternate off/interpolate: {news}")
            print(f"[live-toggle] soak: {len(rows)} smoothing flips applied, "
                  f"interp={soak.summary.get('interp')} "
                  f"stale={soak.summary.get('stale')} "
                  f"realwalks={soak.replay.get('realwalks')} "
                  f"staletenants={soak.replay.get('staletenants')}",
                  flush=True)

    if failures:
        print("\n".join(failures), file=sys.stderr)
        print(f"live-toggle settings: {len(failures)} failure(s)",
              file=sys.stderr)
        return 1
    print("live-toggle settings: all arms passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
