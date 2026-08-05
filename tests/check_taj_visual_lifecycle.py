#!/usr/bin/env python3
"""Exercise post-compose Taj companion loss without stale-object inspection.

The native test hooks request ``free_object`` after a validated presentation
pair exists.  The production free callback must detach both owners before the
deferred allocator destroys either object, clean its surviving sibling, and
compose a new generation.  This intentionally covers the failure timing that
ordinary allocation-fault tests cannot reach.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from check_taj_character_select import LAYOUTS, STATE_TEXT, input_script, save_image
from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
RACE_SCRIPT = ROOT / "tests/input_scripts/taj_unlock_select.txt"
FATAL_MARKERS = ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:")


def run_case(
    binary: Path,
    rom: Path,
    root: Path,
    domain: str,
    component: str,
    verbose: bool,
) -> list[str]:
    """Run exactly one component loss and return human-readable failures."""
    failures: list[str] = []
    run_dir = root / f"{domain}-{component}"
    save_dir = run_dir / "save"
    run_dir.mkdir()
    save_dir.mkdir()
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_TAJ_VISUAL_TRACE="1",
        MDKR_TAJ_SELECT_TRACE="1",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR64_HIDDEN="1",
    )
    if domain == "picker":
        layout = LAYOUTS[0]
        (save_dir / "eeprom.bin").write_bytes(save_image(layout))
        (save_dir / "taj_mod_state.ini").write_text(STATE_TEXT, encoding="ascii")
        script = run_dir / "picker.txt"
        script.write_text(input_script(layout), encoding="ascii")
        env["MDKR_TAJ_SELECT_DROP_AFTER_COMPOSE"] = component
        command = [
            str(binary), "--headless-frames", "2250", "--input-script",
            str(script), "--rom", str(rom),
        ]
        loss = f"[TAJSELECT] event=pair_lost_{component}"
        recomposed = "[TAJSELECT] event=recompose"
        drop = f"[TAJSELECT] event=test_drop component={component} generation=1"
        orphan = "[TAJSELECT] event=orphan_cleanup"
        suppressed = None
    else:
        env.update(
            MDKR_AUTOPILOT="1",
            MDKR_TAJ_VISUAL_DROP_AFTER_COMPOSE=component,
        )
        command = [
            str(binary), "--headless-frames", "6300", "--input-script",
            str(RACE_SCRIPT), "--rom", str(rom),
        ]
        loss = f"[TAJVIS] event=pair_lost_{component}"
        recomposed = "[TAJVIS] event=recompose"
        drop = f"[TAJVIS] event=test_drop component={component} generation=1"
        orphan = "[TAJVIS] event=orphan_cleanup"
        suppressed = "[TAJVIS] event=donor_suppressed"

    if verbose:
        print("$ " + " ".join(command), flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90,
        check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        failures.append(f"{domain}/{component}: exit {process.returncode}")
    for marker in FATAL_MARKERS:
        if marker in output:
            failures.append(f"{domain}/{component}: fatal marker {marker}")
    for marker in (drop, loss, orphan, recomposed):
        if marker not in output:
            failures.append(f"{domain}/{component}: missing {marker!r}")
    if "generation=2" not in output:
        failures.append(f"{domain}/{component}: no fresh generation after loss")
    if suppressed is not None and loss in output and recomposed in output:
        lost_at = output.index(loss)
        recomposed_at = output.index(recomposed, lost_at)
        if suppressed in output[lost_at:recomposed_at]:
            failures.append(
                f"{domain}/{component}: donor remained suppressed before "
                "a complete replacement pair")
    if verbose:
        for line in output.splitlines():
            if "[TAJVIS]" in line or "[TAJSELECT]" in line:
                print(line)
    return failures


# Presentation-only environment flags. Each one visibly changes what the Taj
# companions look like -- and NOTHING the simulation reads. Flipping any of them
# must leave the authoritative [SIMHASH] stream byte-identical.
PRESENTATION_FLAGS = (
    ("freeze_carpet", {"MDKR_TAJ_VISUAL_FREEZE_CARPET": "1"}),
    ("unscaled_carpet", {"MDKR_TAJ_VISUAL_UNSCALED_CARPET": "1"}),
    ("both", {"MDKR_TAJ_VISUAL_FREEZE_CARPET": "1",
              "MDKR_TAJ_VISUAL_UNSCALED_CARPET": "1"}),
)
HASH_FRAMES = 6400


def hash_stream(binary: Path, rom: Path, run_dir: Path,
                extra: dict[str, str], verbose: bool) -> tuple[int, list[str]]:
    """One scripted Taj race under MDKR_STATE_HASH=3; return its [SIMHASH] rows."""
    save_dir = run_dir / "save"
    run_dir.mkdir(parents=True, exist_ok=True)
    save_dir.mkdir(exist_ok=True)
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_STATE_HASH="3",
        MDKR_SAVE_DIR=str(save_dir),
    )
    env.update(extra)
    command = [
        str(binary), "--headless-frames", str(HASH_FRAMES),
        "--input-script", str(RACE_SCRIPT), "--rom", str(rom),
    ]
    if verbose:
        print("$ " + " ".join(command), flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, timeout=600, check=False,
    )
    rows = [line for line in (process.stdout or "").splitlines()
            if line.startswith("[SIMHASH]")]
    return process.returncode, rows


def run_sign_only_fault_case(binary: Path, rom: Path, root: Path,
                            verbose: bool) -> list[str]:
    """The picker must not leave a live, lit Taj nobody can select.

    The existing fault arm uses MDKR_TAJ_SELECT_FAIL_SPAWNS, which blocks the
    RIDER as well, so the "unavailable" state it reaches never has a composed
    actor to leave behind and cannot see this. MDKR_TAJ_SELECT_FAIL_SIGN_SPAWNS
    fails only the numbered placard: the rider composes normally, the sign
    retries to its limit, and the picker goes UNAVAILABLE -- which is what makes
    Taj unselectable. Before the teardown landed, the update_actor tail kept
    running for that surviving rider and unconditionally cleared
    OBJ_FLAGS_INVISIBLE, so a fully lit, correctly posed, hover-animated Taj
    stood in the line-up that the player could not choose.
    """
    failures: list[str] = []
    run_dir = root / "picker-sign-only"
    save_dir = run_dir / "save"
    run_dir.mkdir()
    save_dir.mkdir()
    layout = LAYOUTS[0]
    (save_dir / "eeprom.bin").write_bytes(save_image(layout))
    (save_dir / "taj_mod_state.ini").write_text(STATE_TEXT, encoding="ascii")
    script = run_dir / "picker.txt"
    script.write_text(input_script(layout), encoding="ascii")
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_TAJ_VISUAL_TRACE="1",
        MDKR_TAJ_SELECT_TRACE="1",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR64_HIDDEN="1",
        # Fail only the placard, never the actor.
        MDKR_TAJ_SELECT_FAIL_SIGN_SPAWNS="1000",
    )
    command = [
        str(binary), "--headless-frames", "2250", "--input-script",
        str(script), "--rom", str(rom),
    ]
    if verbose:
        print("$ " + " ".join(command), flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=90, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        failures.append(f"sign-only: exit {process.returncode}")
    for marker in FATAL_MARKERS:
        if marker in output:
            failures.append(f"sign-only: fatal marker {marker}")
    # The fault must actually land on the sign and only on the sign.
    if "[TAJSELECT] event=compose" not in output:
        failures.append("sign-only: the rider never composed, so this arm did "
                        "not exercise the sign-only path")
    if "[TAJSELECT] event=sign_retry" not in output:
        failures.append("sign-only: the placard never entered its retry budget")
    if "[TAJSELECT] event=sign_compose" in output:
        failures.append("sign-only: the placard composed despite the fault")
    if "[TAJSELECT] event=presentation_unavailable" not in output:
        failures.append("sign-only: the picker never reached UNAVAILABLE")
    if "[TAJSELECT] event=unavailable_teardown" not in output:
        failures.append(
            "sign-only: UNAVAILABLE left the composed Taj actor alive; an "
            "unselectable character stayed visible in the line-up")
    # Once unavailable, nothing may recompose behind the player's back.
    marker = "[TAJSELECT] event=presentation_unavailable"
    if marker in output:
        tail = output[output.index(marker):]
        for forbidden in ("event=compose\n", "event=pair_compose",
                          "event=sign_compose"):
            if forbidden in tail:
                failures.append(
                    f"sign-only: {forbidden!r} after UNAVAILABLE")
    if verbose:
        for line in output.splitlines():
            if "[TAJSELECT]" in line:
                print(line)
    return failures


def run_determinism_case(binary: Path, rom: Path, root: Path,
                         verbose: bool) -> list[str]:
    """The authoritative hash must not be a function of presentation.

    The Taj companions are ordinary entries in gObjPtrList, and their
    transform, scale and animation clock are rewritten every tick from a bob
    phase and a dash pulse -- three of those inputs behind the environment
    flags below. Before the hash learned to skip presentation objects
    (platform/sim_hash.c, PRESENTATION-OBJECT EXCLUSION) setting
    MDKR_TAJ_VISUAL_UNSCALED_CARPET moved the published [SIMHASH] stream from
    the tick the carpet composed onwards, which made the project's anchor
    instrument -- and therefore every render-purity, presentation-rate and
    catch-up gate built on it -- a function of an environment variable.

    Nothing else in the suite could see this: check_state_hash and
    check_determinism never select Taj.
    """
    failures: list[str] = []
    baseline_rc, baseline = hash_stream(
        binary, rom, root / "hash-baseline", {}, verbose)
    if baseline_rc != 0:
        failures.append(f"determinism/baseline: exit {baseline_rc}")
    if len(baseline) < HASH_FRAMES // 2:
        failures.append(
            f"determinism/baseline: only {len(baseline)} [SIMHASH] rows; "
            "the hash was not enabled or the run died early")
        return failures
    if not any("objs=" in row for row in baseline):
        failures.append("determinism/baseline: no object counts in the stream")
    for name, extra in PRESENTATION_FLAGS:
        rc, rows = hash_stream(binary, rom, root / f"hash-{name}", extra,
                               verbose)
        if rc != 0:
            failures.append(f"determinism/{name}: exit {rc}")
            continue
        if len(rows) != len(baseline):
            failures.append(
                f"determinism/{name}: {len(rows)} [SIMHASH] rows against "
                f"{len(baseline)} in the baseline")
            continue
        for index, (want, got) in enumerate(zip(baseline, rows)):
            if want != got:
                failures.append(
                    f"determinism/{name}: presentation flag changed the "
                    f"authoritative hash at row {index}: "
                    f"baseline {want!r} vs {got!r}")
                break
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if not binary.exists() or not rom.exists() or not RACE_SCRIPT.exists():
        print("check_taj_visual_lifecycle: FAIL -- missing binary, ROM, or fixture", file=sys.stderr)
        return 1

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-taj-visual-lifecycle-") as temporary:
        root = Path(temporary)
        for component in ("rider", "sign"):
            failures.extend(run_case(binary, rom, root, "picker", component,
                                     args.verbose))
        for component in ("carpet", "rider"):
            failures.extend(run_case(binary, rom, root, "race", component,
                                     args.verbose))
        failures.extend(run_sign_only_fault_case(binary, rom, root,
                                                 args.verbose))
        failures.extend(run_determinism_case(binary, rom, root, args.verbose))
    if failures:
        for failure in failures:
            print(f"check_taj_visual_lifecycle: FAIL -- {failure}", file=sys.stderr)
        return 1
    print("check_taj_visual_lifecycle: PASS -- picker and race companion loss "
          "clean up atomically and recompose fresh generations, and no "
          "presentation flag moves the authoritative state hash")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
