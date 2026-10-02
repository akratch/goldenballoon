#!/usr/bin/env python3
"""A visible automation window runs like an application and never takes the desktop.

The real-time gates and tools/measure_texture_pack_stalls.py run the game in a
VISIBLE bounded window (MDKR_TEST_VISIBLE_HEADLESS=1). With only SDL's
background-app hint, macOS registered that process as BackgroundOnly and ran
its game thread at the default priority 31, where every application runs at
46 (47 frontmost); T12c traced most of the "idle-wait" frames it measured to
that. The engine now makes such a process an accessory application. Unlike
BackgroundOnly, an accessory application CAN be activated, so the window is
also shown ordered behind and never raised (platform_sdl_min.c).

This holds both halves, per renderer:

  visible   MDKR_TEST_VISIBLE_HEADLESS=1 on its own.
  runner    the same plus the environment tools/run_checks.py gives every task
            (MDKR64_HIDDEN=1, SDL_MAC_BACKGROUND_APP=1,
            SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1).
            Both must be an accessory app (LaunchServices ApplicationType
            "UIElement"), run the game thread at an application's priority
            (the [TEXPACK-FRAME] rows' prio, >= 46), and never be the
            frontmost application: NSApp isActive is 0 on every frame row,
            and `lsappinfo front` never names the process while it runs.
  control   an ordinary hidden bounded run, which keeps the old policy: it
            must read BackgroundOnly at the default priority. If the probe
            read every process as an accessory app at 46, the arms above
            would prove nothing.

macOS only. Elsewhere it exits 77 (SKIPPED, named by the runner). It also
exits 77 after the other assertions pass when the console is locked or has
no user session: nothing can become frontmost then, so "never took the
desktop" cannot be observed and is not claimed.
"""

from __future__ import annotations

import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import harness_utils  # noqa: E402 -- also isolates a direct run (saves, prefs, mods)

SKIP_EXIT = 77
APP_PRIORITY_MIN = 46
DEFAULT_PRIORITY_MAX = 40
TICKS = 600  # Enhanced, real time: ten seconds of frames per arm
ROW_TAG = "[TEXPACK-FRAME]"
RUNNER_ENV = {
    "MDKR64_HIDDEN": "1",
    "SDL_MAC_BACKGROUND_APP": "1",
    "SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN": "1",
}


def resolve_binary(build: str) -> Path:
    path = Path(build).expanduser().resolve()
    return path / "mdkr64" if path.is_dir() else path


def console_observable() -> tuple[bool, str]:
    """Whether a process could become frontmost on this console right now."""
    try:
        text = subprocess.run(["ioreg", "-n", "Root", "-d1"],
                              capture_output=True, text=True, timeout=10,
                              check=False).stdout
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, f"ioreg unavailable ({exc})"
    reason = harness_utils.console_lock_reason(text)
    return (False, reason) if reason else (True, "console unlocked")


def front_pid() -> int | None:
    asn = subprocess.run(["lsappinfo", "front"], capture_output=True,
                         text=True, timeout=10, check=False).stdout.strip()
    if not asn:
        return None
    out = subprocess.run(["lsappinfo", "info", "-only", "pid", asn],
                         capture_output=True, text=True, timeout=10,
                         check=False).stdout
    match = re.search(r'"pid"\s*=\s*(\d+)', out)
    return int(match.group(1)) if match else None


def application_type(pid: int) -> str | None:
    out = subprocess.run(["lsappinfo", "info", "-only", "ApplicationType",
                          str(pid)], capture_output=True, text=True,
                         timeout=10, check=False).stdout
    match = re.search(r'"ApplicationType"\s*=\s*"([^"]*)"', out)
    return match.group(1) if match else None


def rows(output: str) -> list[dict[str, int]]:
    parsed = []
    for line in output.splitlines():
        position = line.find(ROW_TAG)
        if position < 0:
            continue
        row = {}
        for token in line[position + len(ROW_TAG):].split():
            key, _, value = token.partition("=")
            if key in ("prio", "active", "visible", "frame"):
                row[key] = int(value)
        parsed.append(row)
    return parsed


def run_arm(binary: Path, rom: Path, root: Path, renderer: str, label: str,
            extra: dict[str, str], visible: bool) -> dict:
    run_dir = root / f"{renderer}-{label}"
    (run_dir / "save").mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_", "SDL_"))}
    env.update({
        "LC_ALL": "C",
        "MDKR_AUDIO": "0",
        "MDKR_AUTOPILOT": "1",
        "MDKR_RENDERER": renderer,
        "MDKR_SIMULATION_CADENCE": "enhanced",
        "MDKR_PACE_REALTIME": "1",
        "MDKR_TEXTURE_PACK_TIMING": "1",
        "MDKR_APP_PREFS_DIR": str(run_dir),
        "MDKR_SAVE_DIR": str(run_dir / "save"),
        "MDKR_VIDEO_CONFIG_PATH": str(run_dir / "save" / "video.ini"),
    })
    if visible:
        env["MDKR_TEST_VISIBLE_HEADLESS"] = "1"
    env.update(extra)
    log_path = run_dir / "run.log"
    types: collections.Counter = collections.Counter()
    fronts: collections.Counter = collections.Counter()
    with log_path.open("wb") as log:
        process = subprocess.Popen(
            [str(binary), "--headless-ticks", str(TICKS),
             "--window-size", "640x360", "--rom", str(rom)],
            cwd=run_dir, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 180
            while process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.5)
                if ROW_TAG.encode() not in log_path.read_bytes()[-4096:]:
                    continue  # not drawing yet: LaunchServices may not know it
                kind = application_type(process.pid)
                if kind is not None:
                    types[kind] += 1
                fronts["self" if front_pid() == process.pid else "other"] += 1
            returncode = process.wait(timeout=5) if process.poll() is None \
                else process.returncode
        except subprocess.TimeoutExpired:
            returncode = None
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    frame_rows = rows(log_path.read_text(encoding="utf-8", errors="replace"))
    priorities = collections.Counter(row["prio"] for row in frame_rows
                                     if "prio" in row)
    return {
        "label": f"{renderer}/{label}",
        "returncode": returncode,
        "rows": len(frame_rows),
        "types": dict(types),
        "fronts": dict(fronts),
        "priority": (priorities.most_common(1)[0][0] if priorities else None),
        "active_rows": sum(1 for row in frame_rows if row.get("active") == 1),
        "log": str(log_path),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--build", default="build-rel")
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--renderer", choices=("webgpu", "gl", "both"),
                        default="both")
    parser.add_argument("--keep", action="store_true",
                        help="keep the per-arm run directories")
    args = parser.parse_args()

    if sys.platform != "darwin":
        print(f"automation_window_policy: SKIPPED — macOS only (activation "
              f"policy, LaunchServices type and NSApp frontmost state are "
              f"Cocoa's); this host is {sys.platform}", file=sys.stderr)
        return SKIP_EXIT
    binary = resolve_binary(args.build)
    rom = args.rom.expanduser().resolve()
    if not binary.is_file() or not rom.is_file():
        print(f"automation_window_policy: FAIL — missing binary {binary} or "
              f"ROM {rom}", file=sys.stderr)
        return 1
    observable, console = console_observable()
    renderers = ("webgpu", "gl") if args.renderer == "both" \
        else (args.renderer,)
    failures: list[str] = []
    root = Path(tempfile.mkdtemp(prefix="mdkr64-automation-window-"))
    try:
        for renderer in renderers:
            arms = (("visible", {}, True), ("runner", RUNNER_ENV, True),
                    ("control", {}, False))
            for label, extra, visible in arms:
                result = run_arm(binary, rom, root, renderer, label, extra,
                                 visible)
                print(f"{result['label']}: exit {result['returncode']}, "
                      f"{result['rows']} frame rows, type {result['types']}, "
                      f"priority {result['priority']}, frontmost samples "
                      f"{result['fronts']}, isActive rows "
                      f"{result['active_rows']}", flush=True)
                name = result["label"]
                if result["returncode"] != 0 or result["rows"] < 100:
                    failures.append(f"{name}: run did not complete "
                                    f"(exit {result['returncode']}, "
                                    f"{result['rows']} rows; {result['log']})")
                    continue
                if not result["types"]:
                    failures.append(f"{name}: LaunchServices never reported "
                                    f"an application type")
                    continue
                if visible:
                    if set(result["types"]) != {"UIElement"}:
                        failures.append(
                            f"{name}: application type {result['types']}, "
                            f"expected an accessory app (UIElement) throughout")
                    if result["priority"] is None or \
                            result["priority"] < APP_PRIORITY_MIN:
                        failures.append(
                            f"{name}: game thread priority "
                            f"{result['priority']}, expected an "
                            f"application's {APP_PRIORITY_MIN}+")
                    if result["active_rows"] or result["fronts"].get("self"):
                        failures.append(
                            f"{name}: became the frontmost application "
                            f"(isActive on {result['active_rows']} rows, "
                            f"frontmost in {result['fronts'].get('self', 0)} "
                            f"samples) -- an automation window took the "
                            f"desktop")
                else:
                    if set(result["types"]) != {"BackgroundOnly"} or \
                            result["priority"] is None or \
                            result["priority"] >= DEFAULT_PRIORITY_MAX:
                        failures.append(
                            f"{name}: control read type {result['types']} at "
                            f"priority {result['priority']}; a hidden "
                            f"bounded run is BackgroundOnly at the default "
                            f"priority, so the probe cannot tell the policies "
                            f"apart")
    finally:
        if not args.keep:
            subprocess.run(["rm", "-rf", str(root)], check=False)
    if failures:
        for failure in failures:
            print(f"automation_window_policy: FAIL — {failure}",
                  file=sys.stderr)
        return 1
    if not observable:
        print(f"automation_window_policy: SKIPPED — accessory type and "
              f"application priority PASS on {', '.join(renderers)}, but "
              f"{console}: no process can become frontmost, so 'never takes "
              f"the desktop' was not observed. Re-run on an unlocked "
              f"console.", file=sys.stderr)
        return SKIP_EXIT
    print(f"automation_window_policy: PASS — {', '.join(renderers)}: "
          f"visible automation is an accessory app at an application's "
          f"priority and never became frontmost ({console}); the hidden "
          f"control stays BackgroundOnly at the default priority")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
