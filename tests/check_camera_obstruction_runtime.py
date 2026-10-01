#!/usr/bin/env python3
"""ROM-backed wall witness for the modern camera obstruction runtime.

The same deterministic Ancient Lake route runs through five arms in one binary.
Legacy must reproduce an authored lens overlap, center-ray must prove that
protecting only the eye center is insufficient, and Modern must correct every
resolved lens overlap while leaving the logical camera untouched.

The last arm runs with MDKR_CAMERA_OBSTRUCTION removed from the environment.
Correction is opt-in -- it was the default for one wave and device acceptance
sent it back -- so the unset arm must report the OBSERVE gate and reproduce the
explicit Observe arm's counters exactly. That is the runtime witness that a
player who sets nothing gets the authored camera.

The explicit modern arm stays, and stays load-bearing, as the opt-in witness:
it must still select the MODERN gate and correct every resolved lens overlap,
which is what proves the correction is genuinely one setting away rather than
dead code behind a default that no longer selects it.
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

from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, find_fatal,
                           resolve_binary)
from check_camera_motion_quality import (HELD_DWELL_SLACK_SECONDS,
                                         MAX_HELD_DIVERGENCE_DEGREES,
                                         MAX_HELD_DWELL_SECONDS)


SCRIPT = Path(__file__).resolve().parent / "input_scripts/race_drive_long.txt"
DETAIL = "camera_obstruction_observe detail"
SUMMARY = "camera_obstruction_observe summary"


def run(binary: str, rom: str, policy: str, frames: int, timeout: int,
        window: str = "1280x960", fov: str = "authored",
        max_hfov: str | None = None,
        extra_env: dict[str, str] | None = None) -> str:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        MDKR_RENDERER="gl",
        MDKR_AUDIO="0",
        MDKR_PRESENT_RATE="original",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_CAMERA_TRACE="2",
    )
    # "unset" is the shipped configuration, not a policy string: the variable is
    # removed rather than emptied so an inherited value cannot mask the default.
    if policy == "unset":
        env.pop("MDKR_CAMERA_OBSTRUCTION", None)
    else:
        env["MDKR_CAMERA_OBSTRUCTION"] = policy
    # Camera.Comfort is off by default and every arm here measures the authored
    # motion. Removed rather than set, for the same reason as above: an
    # inherited MDKR_CAMERA_COMFORT must not quietly become what this gate
    # measured.
    env.pop("MDKR_CAMERA_COMFORT", None)
    if extra_env:
        env.update(extra_env)
    command = [
        str(Path(binary).resolve()), "--headless-frames", str(frames), "--window-size", window,
        "--fov", fov,
    ]
    if max_hfov is not None:
        command.extend(("--max-hfov", max_hfov))
    command.extend(("--input-script", str(SCRIPT), "--rom", str(Path(rom).resolve())))
    with tempfile.TemporaryDirectory(prefix="mdkr_camera_policy_") as temp:
        root = Path(temp)
        env.update(MDKR_SAVE_DIR=str(root / "save"),
                   MDKR_VIDEO_CONFIG_PATH=str(root / "missing.ini"))
        proc = subprocess.run(
            command, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=timeout,
        )
    if proc.returncode != 0:
        raise RuntimeError(f"{policy} exited {proc.returncode}\n" + "\n".join(proc.stdout.splitlines()[-80:]))
    marker = find_fatal(proc.stdout, *ASSERT_MARKERS)
    if marker:
        raise RuntimeError(f"{policy} emitted {marker}")
    return proc.stdout


def resolved_trace(output: str) -> list[str]:
    """Every published resolved pose, in order. Two runs that framed the same
    picture produce identical lists."""
    values = []
    for row in output.splitlines():
        if DETAIL not in row:
            continue
        match = re.search(r"\bresolved=\(([^)]+)\)", row)
        if match is None:
            raise RuntimeError("missing resolved pose")
        values.append(match.group(1))
    return values


def field(row: str, name: str) -> int:
    match = re.search(rf"\b{re.escape(name)}=(\d+)", row)
    if match is None:
        raise RuntimeError(f"missing {name} in trace row: {row[:240]}")
    return int(match.group(1))


def block_field(row: str, block: str, name: str) -> int:
    match = re.search(
        rf"\b{re.escape(block)}=\{{[^}}]*\b{re.escape(name)}=(\d+)", row
    )
    if match is None:
        raise RuntimeError(f"missing {block}.{name} in trace row: {row[:300]}")
    return int(match.group(1))


HELD_PUBLICATION = re.compile(
    r"camera_held_shot_published tick=(\d+) slot=(\d+) frame=\d+ active=([01]) "
    r"dwell=(\d+(?:\.\d+)?) divergence=(\d+(?:\.\d+)?)")


@dataclass(frozen=True)
class HeldExemption:
    """Corrections split into the ordinary ones and proven held publications.

    `nonheld` counts every other corrected tick; `nonheld_recovering` is the
    part of it the resolver published while RECOVERING (status 2).
    """

    nonheld: int
    publications: int
    episodes: int
    nonheld_recovering: int = 0


def held_exemption(output: str) -> HeldExemption:
    """Exempt only proven active held publications, each inside its bounds.

    Reduced comfort intentionally prolongs held recovery, so its held
    publications are not ordinary obstruction corrections. The exemption is
    bounded: every exempted publication must match a safe corrected detail and
    report dwell and divergence inside the runtime's held-shot bounds.
    Missing publications earn no exemption; duplicate/orphan/unsafe or
    out-of-bounds publication evidence is invalid.
    """
    details = {}
    active = set()
    epoch, last_tick = 0, -1
    for row in output.splitlines():
        if DETAIL not in row and not row.startswith("camera_held_shot_published "):
            continue
        tick = field(row, "tick")
        if tick < last_tick:
            epoch += 1  # Runtime tick serial restarts on level transitions.
        last_tick = tick
        if DETAIL in row:
            key = (epoch, tick, field(row, "physical_slot"))
            if key in details:
                raise RuntimeError("duplicate camera detail identity")
            details[key] = row
        elif row.startswith("camera_held_shot_published "):
            match = HELD_PUBLICATION.fullmatch(row)
            if match is None:
                raise RuntimeError("malformed held publication")
            if match[3] == "1":
                if float(match[4]) > MAX_HELD_DWELL_SECONDS + HELD_DWELL_SLACK_SECONDS or \
                        float(match[5]) > MAX_HELD_DIVERGENCE_DEGREES + 0.01:
                    raise RuntimeError("held publication outside its dwell/divergence bounds")
                key = (epoch, int(match[1]), int(match[2]))
                if key in active:
                    raise RuntimeError("duplicate active held publication")
                active.add(key)
    if not details:
        raise RuntimeError("missing camera details")
    for key in active:
        row = details.get(key)
        if row is None or any(field(row, name) != value for name, value in
                (("corrected", 1), ("alternate", 1), ("valid", 1),
                 ("clearance", 0), ("source_degraded", 0))) or \
                block_field(row, "resolve", "status") not in (1, 2):
            raise RuntimeError("held publication lacks matching safe correction")
    episodes = sum((epoch, tick - 1, slot) not in active for epoch, tick, slot in active)
    nonheld = [row for key, row in details.items()
               if key not in active and field(row, "corrected")]
    return HeldExemption(
        nonheld=len(nonheld), publications=len(active), episodes=episodes,
        nonheld_recovering=sum(block_field(row, "resolve", "status") == 2
                               for row in nonheld))


def comfort_inflation_failures(comfort: HeldExemption, modern: HeldExemption) -> list[str]:
    """Comfort may lengthen recovery; it may not create corrections.

    Everything comfort does not stretch keeps the original 2x bound: the
    ordinary corrections that are not recovery (retraction, emergency and any
    corrected-but-clear tick, which is the comfort-offset-counted-as-correction
    signature) and the number of held shots, which start where geometry blocks
    the authored boom. Recovery is the one thing comfort stretches, and only by
    its own rate: at 0.4x speed and fraction a return takes at most
    1/0.4 = 2.5x (speed-limited) or ln(0.75)/ln(0.9) = 2.73x (fraction-limited)
    as many ticks, so ordinary recovery ticks and held publications are bounded
    at 3x.
    """
    failures = []
    comfort_other = comfort.nonheld - comfort.nonheld_recovering
    modern_other = modern.nonheld - modern.nonheld_recovering
    if comfort_other > 2 * modern_other:
        failures.append(
            f"comfort arm reported {comfort_other} non-held, non-recovery "
            f"corrections against the authored-motion arm's {modern_other}; the "
            "comfort offset is being counted as an obstruction correction")
    if comfort.nonheld_recovering > 3 * modern.nonheld_recovering:
        failures.append(
            f"comfort arm spent {comfort.nonheld_recovering} non-held ticks "
            f"recovering against the authored-motion arm's "
            f"{modern.nonheld_recovering}; recovery outran the reduced rate's bound")
    if comfort.episodes > 2 * modern.episodes:
        failures.append(
            f"comfort arm started {comfort.episodes} held shots against the "
            f"authored-motion arm's {modern.episodes}; comfort moved the desired "
            "eye into geometry")
    if comfort.publications > 3 * modern.publications:
        failures.append(
            f"comfort arm published {comfort.publications} held ticks against the "
            f"authored-motion arm's {modern.publications}; held recovery outran the "
            "reduced recovery rate's bound")
    return failures


def inspect(policy: str, output: str) -> dict[str, int]:
    details = [line for line in output.splitlines() if DETAIL in line]
    summaries = [line for line in output.splitlines() if SUMMARY in line]
    if len(details) < 3000 or not summaries:
        raise RuntimeError(f"{policy} produced incomplete telemetry ({len(details)} detail rows)")
    gates = {match.group(1) for match in
             (re.search(r"\bgate=([A-Z_]+)\(", row) for row in summaries)
             if match is not None}
    if len(gates) != 1:
        raise RuntimeError(f"{policy} reported {sorted(gates)} gate(s), expected one")
    result = {
        "gate": gates.pop(),
        "rows": len(details),
        "corrected": sum(field(row, "corrected") for row in details),
        "penetrated": sum(field(row, "clearance") != 0 for row in details),
        "degraded": sum(field(row, "source_degraded") for row in details),
        "invalid": sum(field(row, "valid") == 0 for row in details),
        "target_hidden": sum(
            field(row, "target_visible") == 0 and
            field(row, "target_embedded") == 0
            for row in details
        ),
        "emergency": sum(field(row, "emergency") for row in details),
        "racer_faded": sum(field(row, "racer_opacity") < 255 for row in details),
        "exact_invoked": sum(
            block_field(row, "exact_runtime", "invoked") for row in details
        ),
        "exact_override": sum(
            block_field(row, "exact_runtime", "override") for row in details
        ),
        "exact_degraded": sum(
            block_field(row, "exact_runtime", "degraded") for row in details
        ),
        "transition_invoked": sum(
            block_field(row, "transition", "invoked") for row in details
        ),
        "transition_clear": sum(
            block_field(row, "transition", "clear") for row in details
        ),
        "transition_cut": sum(
            block_field(row, "transition", "cut") for row in details
        ),
    }
    # Only the corrective arms owe zero invalid/hidden results; importers pass
    # their own labels, and the ones that mean Modern prefix them with it.
    # "unset" is not one of them: the default resolves back to Observe, which
    # publishes nothing and therefore owes nothing about invalid or hidden
    # resolved results.
    corrective = policy.startswith("modern")
    if result["degraded"] or (corrective and
                              (result["invalid"] or result["target_hidden"])):
        raise RuntimeError(f"{policy} degraded/invalid runtime: {result}")
    for row in summaries:
        if "duplicates=0" not in row or "projection_mismatches=0" not in row:
            raise RuntimeError(f"{policy} authority/projection violation: {row}")
        for counter in (
            "missing_cache", "missing_identity", "uncategorized",
            "invalid_transform", "capacity_failures",
            "invalid_sweeps", "sphere_invalid_sweeps",
            "exact_invalid_sweeps",
        ):
            match = re.search(rf"{counter}=(\d+)", row)
            if match is None or int(match.group(1)) != 0:
                raise RuntimeError(f"{policy} dynamic publication violation {counter}: {row}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=5200)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not Path(path).is_file():
            raise SystemExit(f"missing: {path}")

    results = {}
    outputs = {}
    for policy in ("legacy", "center-ray", "modern", "observe", "unset"):
        outputs[policy] = run(binary, args.rom, policy, args.frames, args.timeout)
        results[policy] = inspect(policy, outputs[policy])
        print(f"  {policy:10s} {results[policy]}")

    # Camera.Comfort's reduced-motion arm. MDKR_CAMERA_OBSTRUCTION=modern is
    # set explicitly here because the corrected camera is the only camera
    # comfort can soften -- observe publishes nothing for it to act on -- and
    # it is no longer what an unset variable gives. Labelled "modern-..." so it
    # carries Modern's obligations.
    outputs["comfort"] = run(
        binary, args.rom, "modern", args.frames, args.timeout,
        extra_env={"MDKR_CAMERA_COMFORT": "reduced"})
    results["comfort"] = inspect("modern-comfort-reduced", outputs["comfort"])
    print(f"  {'comfort':10s} {results['comfort']}")

    failures = []
    # Comfort is presentation, so it may change the picture and may change
    # nothing else. Safety first: a softened desired eye is still solved
    # against, so the published pose must still be proven clear.
    if results["comfort"]["penetrated"] != 0:
        failures.append(
            f"comfort arm published {results['comfort']['penetrated']} "
            "penetrated resolved pose(s)"
        )
    if results["comfort"]["gate"] != "MODERN":
        failures.append(
            f"comfort arm selected {results['comfort']['gate']}, not MODERN"
        )
    # The runtime has to have actually read the setting, and it has to have
    # done something. A comfort option that silently no-ops is the failure this
    # pair exists to catch.
    if "comfort=reduced" not in outputs["comfort"]:
        failures.append("comfort arm did not report comfort=reduced")
    if "comfort=authored" not in outputs["modern"]:
        failures.append("default arm did not report comfort=authored")
    if resolved_trace(outputs["comfort"]) == resolved_trace(outputs["modern"]):
        failures.append(
            "comfort=reduced reproduced the authored-motion camera exactly; "
            "the vertical smoother did nothing"
        )
    # ...and it has to stay legible as the thing it is. "corrected" means the
    # resolver moved the camera off the shot it was asked for, and it feeds
    # was_obstructed, the resolver status, and the whole MOTION-01 census.
    # Comfort changes what is asked for. Counting that as an obstruction
    # correction reports a permanently retracted camera on a wall-free route --
    # it is a 35x inflation of this counter, and this bound is what catches it.
    # Active held-shot recovery legitimately lasts longer with reduced comfort.
    # Match actual, in-bounds publications before exempting those ticks, bound
    # how many there may be, and keep the original guard on everything else.
    comfort_held = held_exemption(outputs["comfort"])
    modern_held = held_exemption(outputs["modern"])
    print(f"  held exemption: modern={modern_held} comfort={comfort_held}")
    failures.extend(comfort_inflation_failures(comfort_held, modern_held))
    if results["legacy"]["penetrated"] == 0:
        failures.append("legacy positive control did not reproduce a lens overlap")
    if results["center-ray"]["penetrated"] == 0:
        failures.append("center-ray positive control did not expose a near-plane overlap")
    # target_hidden is the focus-occlusion outcome every other gate asserts is
    # zero, which leaves nothing requiring it to be producible at all. The
    # uncorrected legacy arm drives the focus behind real geometry on this
    # route, so it is the positive control: without it, publishing the focus as
    # permanently visible would satisfy the whole suite.
    if results["legacy"]["target_hidden"] == 0:
        failures.append("legacy positive control never hid the camera focus")
    if results["modern"]["corrected"] == 0:
        failures.append("modern path never applied a correction")
    if results["modern"]["exact_invoked"] == 0:
        failures.append("modern path never invoked the exact rounded-lens phase")
    if results["modern"]["exact_degraded"] != 0:
        failures.append(
            f"modern exact runtime degraded {results['modern']['exact_degraded']} time(s)"
        )
    if results["modern"]["transition_invoked"] == 0 or \
            results["modern"]["transition_clear"] == 0:
        failures.append("modern path never proved a fractional presentation chord clear")
    if results["modern"]["penetrated"] != 0:
        failures.append(
            f"modern published {results['modern']['penetrated']} penetrated resolved pose(s)"
        )
    # Correction is opt-in, so the observe arm is what a player who sets
    # nothing gets: the authored camera, uncorrected.
    if results["observe"]["corrected"] != 0:
        failures.append(
            f"observe arm applied {results['observe']['corrected']} correction(s)"
        )
    # The opt-in witness. An explicit modern must still reach the MODERN gate
    # and still do the work; a default flipped back to observe may not leave
    # the correction unreachable.
    if results["modern"]["gate"] != "MODERN":
        failures.append(
            f"explicit modern selected {results['modern']['gate']}, not MODERN"
        )
    if results["modern"]["corrected"] == 0:
        failures.append(
            "explicit modern applied no corrections; the opt-in camera is inert"
        )
    # Default policy. Naming the arm is not enough -- a gate label can be wrong
    # about what ran -- so the unset arm must also reproduce the Observe arm's
    # every counter.
    if results["unset"]["gate"] != "OBSERVE":
        failures.append(
            f"unset MDKR_CAMERA_OBSTRUCTION selected {results['unset']['gate']}, not OBSERVE"
        )
    if results["unset"] != results["observe"]:
        differing = sorted(
            key for key in results["observe"]
            if results["unset"][key] != results["observe"][key]
        )
        failures.append(
            "unset arm diverged from the explicit observe arm: " + ", ".join(
                f"{key} {results['unset'][key]} vs {results['observe'][key]}"
                for key in differing
            )
        )
    if failures:
        print("check_camera_obstruction_runtime: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check_camera_obstruction_runtime: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
