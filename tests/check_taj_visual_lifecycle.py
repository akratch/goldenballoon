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
    if failures:
        for failure in failures:
            print(f"check_taj_visual_lifecycle: FAIL -- {failure}", file=sys.stderr)
        return 1
    print("check_taj_visual_lifecycle: PASS -- picker and race companion loss "
          "clean up atomically and recompose fresh generations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
