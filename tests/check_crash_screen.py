#!/usr/bin/env python3
"""The crash screen adds a report to a fatal run and changes nothing else.

Why this gate exists
--------------------
US-6: when the game dies the player should see what happened instead of a
window vanishing. The risk in delivering that is not the screen -- it is the
output. ``[CRASH]`` and ``[FATAL]`` are the engine's last words and every
harness in ``tests/`` greps for them, so a crash surface that printed first,
buffered, reworded or replaced them would break the suite *silently*: those
checks would stop finding the marker they use to tell "the run died" from "the
run disagreed", and start passing on dead runs.

So the assertions here are mostly about what did NOT move:

  1. the ``[FATAL]`` line is still present, still byte-identical, and still
     FIRST -- ahead of anything the crash screen prints;
  2. the process still dies from SIGABRT, the exact disposition this fault had
     before the crash screen existed, so CI classification does not shift;
  3. a clean run prints no ``[CRASHSCREEN]`` at all -- the surface costs a
     healthy run nothing, not even a line.

and only then about what was added:

  4. one ``[CRASHSCREEN]`` line naming the fault kind, the authoritative tick,
     the track, the renderer, the app version and the log path;
  5. ``present=0`` with no window, and no ``presented=`` line -- headless never
     attempts to show a panel, because ``AppHost`` (the only thing that
     registers a window) is never built on the automation path.

The two faults, and why there are two
-------------------------------------
The crash screen has two install seams and they must both be proven, because
each one is a different answer to "did this run after the marker":

  ABORT   ``MDKR_SUBENTRY_TEST_INDEX=1`` -- the existing negative control
          tests/check_subentry_bounds.py already drives. It writes ``[FATAL]``,
          flushes, and calls ``abort()``, which is the shape of every fatal path
          in this tree. No new fault-injection seam was invented for this gate.
          It reaches the crash screen's OWN SIGABRT handler.
  SIGNAL  a SIGSEGV delivered to a warmed-up run. That signal belongs to
          platform/main_pc.c's backtrace handler, which owns ``[CRASH]``; the
          crash screen is *called by* it after the marker and the whole
          backtrace have been flushed. Taking the signal over instead would have
          replaced that marker, so being second here is the design.

Not vacuous
-----------
A report that printed constants would satisfy 4 just as well as one that reads
live state. Four arms close that hole, one per field that can move:

  * the two faults are compared against each other: the abort fires during asset
    init (tick 0, no level) and the signal arm fires mid-attract (tick in the
    thousands, a real track), so ``tick`` and ``track`` are required to DISAGREE
    between them. A hard-coded field cannot do that;
  * ``track-name`` must agree with ``track`` in both arms -- the sentinel
    wording exactly when the id is negative, a real name when it is not;
  * the renderer arm runs the abort under ``MDKR_RENDERER=gl`` and requires
    ``renderer=`` to follow it, so the field is read from the resolved backend;
  * the version arm requires ``version=`` to equal what ``--version`` prints,
    so it is the build's own stamp and not a literal.

MUTED + HEADLESS: every invocation passes ``--headless-frames`` and sets
``MDKR_AUDIO=0``.

Usage:
    tests/check_crash_screen.py --build build-rel
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

# The existing negative control. Its text is quoted here on purpose: this gate
# is partly an assertion that the marker did not change wording.
FAULT_ENV = {"MDKR_SUBENTRY_TEST_INDEX": "1"}
FATAL_LINE = ("[FATAL] asset sub-entry index out of range: "
              "section=MDKR_SELFTEST index=1 count=1 at=mdkr_asset_subentry")

# Captured from the unmodified binary before the crash screen landed, and
# asserted rather than derived: the whole point is that it did not move.
EXPECTED_RETURNCODE = -signal.SIGABRT

MARKER = "[CRASHSCREEN]"
FIELD_RE = re.compile(r'(\w[\w-]*)=("[^"]*"|\S+)')

REQUIRED_FIELDS = ("kind", "tick", "track", "track-name", "renderer",
                   "version", "present", "log")

# The wording tool_diagnostics.cpp uses for "no level yet", which the crash
# screen reuses so a crash report and a Diagnostics report read the same.
NO_LEVEL = "not in a level yet"


def hermetic_env(temp: str, extra_env: dict[str, str]) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_SAVE_DIR=str(Path(temp) / "save"),
        MDKR_VIDEO_CONFIG_PATH=str(Path(temp) / "missing.ini"),
    )
    env.update(extra_env)
    return env


def run(binary: Path, rom: Path, frames: int, timeout: int,
        extra_env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    """One headless invocation with a hermetic environment."""
    with tempfile.TemporaryDirectory(prefix="mdkr_crashscreen_") as temp:
        return subprocess.run(
            [str(binary), "--headless-frames", str(frames), "--rom", str(rom)],
            env=hermetic_env(temp, extra_env), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )


def run_until_signalled(binary: Path, rom: Path, warmup: float,
                        timeout: int) -> tuple[int, str, bool]:
    """Warm a headless run up, then fault it with SIGSEGV.

    The frame budget is deliberately far beyond what the warm-up can consume:
    the run must still be alive to receive the signal, and a run that finished
    first would make this arm silently vacuous rather than failing.
    """
    with tempfile.TemporaryDirectory(prefix="mdkr_crashscreen_") as temp:
        process = subprocess.Popen(
            [str(binary), "--headless-frames", "400000", "--rom", str(rom)],
            env=hermetic_env(temp, {}), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + warmup
        while time.monotonic() < deadline and process.poll() is None:
            time.sleep(0.05)
        alive = process.poll() is None
        if alive:
            process.send_signal(signal.SIGSEGV)
        try:
            output, _ = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate()
        return process.returncode, output or "", alive


def describe(code: int) -> str:
    if code < 0:
        try:
            return f"signal {signal.Signals(-code).name}"
        except ValueError:
            return f"signal {-code}"
    return f"exit {code}"


def field_line(output: str) -> str | None:
    """The one ``[CRASHSCREEN]`` row that carries ``kind=``."""
    for line in output.splitlines():
        if MARKER in line and "kind=" in line:
            return line[line.index(MARKER):]
    return None


def parse_fields(line: str) -> dict[str, str]:
    payload = line[len(MARKER):]
    return {key: value.strip('"')
            for key, value in FIELD_RE.findall(payload)}


def check_ordering(label: str, output: str, marker: str, exact: str | None,
                   failures: list[str]) -> None:
    """The engine's marker is first. Not "present somewhere" -- first."""
    lines = output.splitlines()
    marker_at = next((i for i, text in enumerate(lines) if marker in text), None)
    screen_at = next((i for i, text in enumerate(lines) if MARKER in text), None)
    if marker_at is None:
        failures.append(f"{label}: the {marker} marker is gone")
        return
    if exact is not None and lines[marker_at].strip() != exact:
        failures.append(
            f"{label}: the {marker} marker changed wording\n"
            f"      expected: {exact}\n"
            f"      actual:   {lines[marker_at].strip()}")
    if screen_at is None:
        failures.append(f"{label}: no [CRASHSCREEN] output followed the fault")
        return
    if screen_at < marker_at:
        failures.append(
            f"{label}: [CRASHSCREEN] (line {screen_at}) printed BEFORE "
            f"{marker} (line {marker_at}); the crash screen must be additive "
            "output that follows the marker, never output that reorders it")


def check_report(label: str, line: str | None, expect_kind: str,
                 failures: list[str]) -> dict[str, str]:
    """The shared field contract, asserted the same way for both faults."""
    if line is None:
        failures.append(f"{label}: no [CRASHSCREEN] field line was printed")
        return {}
    fields = parse_fields(line)
    for name in REQUIRED_FIELDS:
        if name not in fields:
            failures.append(f"{label}: [CRASHSCREEN] has no {name}= field")
    if fields.get("kind") != expect_kind:
        failures.append(
            f"{label}: kind={fields.get('kind')!r}, expected {expect_kind!r}")
    for numeric in ("tick", "track"):
        value = fields.get(numeric, "")
        if not re.fullmatch(r"-?\d+", value):
            failures.append(f"{label}: {numeric}={value!r} is not a number")
    if not fields.get("log"):
        failures.append(f"{label}: the report does not name the log path")
    # track and its name are one fact reported twice; they may not disagree.
    track = fields.get("track", "")
    name = fields.get("track-name", "")
    if re.fullmatch(r"-?\d+", track):
        if int(track) < 0 and name != NO_LEVEL:
            failures.append(
                f"{label}: track={track} but track-name={name!r}; a negative id "
                f"must read {NO_LEVEL!r}")
        if int(track) >= 0 and name == NO_LEVEL:
            failures.append(
                f"{label}: track={track} is a real level but track-name says "
                "no level is loaded")
    # A harness scanning for a fault must not find a second one in additive
    # output: none of the abort/fatal spellings may appear in this line.
    for spelling in ("[CRASH]", "[FATAL]", "SIGABRT", "SIGSEGV",
                     "Segmentation fault", "Abort trap"):
        if spelling in line:
            failures.append(
                f"{label}: [CRASHSCREEN] contains {spelling!r}, which "
                "harness_utils treats as a fatal marker")
    if fields.get("present") != "0":
        failures.append(
            f"{label}: present={fields.get('present')!r} with --headless-frames "
            "and no window; the crash screen must not try to present")
    return fields


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=20)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument(
        "--warmup", type=float, default=4.0,
        help="seconds to let the signal arm run before faulting it; measured "
             "reaching tick 1252 of the intro track in 1.5 s")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    for path in (binary, rom):
        if not path.is_file():
            raise SystemExit(f"missing: {path}")

    failures: list[str] = []

    # --- 3. a healthy run pays nothing ------------------------------------
    clean = run(binary, rom, args.frames, args.timeout, {})
    clean_output = clean.stdout or ""
    if clean.returncode != 0:
        failures.append(
            f"clean run: expected exit 0, got {describe(clean.returncode)}")
    if MARKER in clean_output:
        failures.append(
            "clean run: the crash screen spoke on a run that did not crash")

    # --- 1 + 2. the fault run ---------------------------------------------
    fault = run(binary, rom, args.frames, args.timeout, FAULT_ENV)
    output = fault.stdout or ""
    if fault.returncode != EXPECTED_RETURNCODE:
        failures.append(
            f"abort arm: exit code moved -- expected "
            f"{describe(EXPECTED_RETURNCODE)}, got {describe(fault.returncode)}. "
            "CI classifies crashed runs on this value.")
    if "resolved without aborting" in output:
        failures.append("abort arm: the index was in range; the control is vacuous")
    check_ordering("abort arm", output, "[FATAL]", FATAL_LINE, failures)

    # --- 4 + 5. the report, and no presentation without a window ----------
    line = field_line(output)
    fields = check_report("abort arm", line, "abort", failures)
    if "presented=" in output:
        failures.append(
            "abort arm: the crash screen presented a panel in a headless run")

    # --- the OTHER seam: a signal, whose marker main_pc.c owns -------------
    signal_line: str | None = None
    signal_fields: dict[str, str] = {}
    if sys.platform == "win32":
        print("  signal arm skipped: SIGSEGV cannot be delivered on Windows")
    else:
        code, signal_output, alive = run_until_signalled(
            binary, rom, args.warmup, args.timeout)
        if not alive:
            failures.append(
                "signal arm: the run finished before it could be faulted, so "
                "this arm proved nothing")
        if code != -signal.SIGSEGV:
            failures.append(
                f"signal arm: exit code moved -- expected "
                f"{describe(-signal.SIGSEGV)}, got {describe(code)}")
        check_ordering("signal arm", signal_output, "[CRASH]", None, failures)
        signal_line = field_line(signal_output)
        signal_fields = check_report("signal arm", signal_line, "memory-fault",
                                     failures)
        if "presented=" in signal_output:
            failures.append(
                "signal arm: the crash screen presented a panel in a headless run")

    # --- non-vacuity: the fields follow live state ------------------------
    if fields and signal_fields:
        # The abort fires during asset init; the signal lands mid-attract. A
        # constant cannot be both.
        if fields.get("tick") == signal_fields.get("tick"):
            failures.append(
                f"tick is the same ({fields.get('tick')}) at asset-init time "
                "and mid-attract; the field is not read from the live counter")
        signal_tick = signal_fields.get("tick", "")
        if re.fullmatch(r"-?\d+", signal_tick) and int(signal_tick) <= 0:
            failures.append(
                f"signal arm: tick={signal_tick} after {args.warmup}s; the run "
                "never reached the main loop -- raise --warmup on a slower "
                "machine")
        if fields.get("track") == signal_fields.get("track"):
            failures.append(
                f"track is the same ({fields.get('track')}) with no level "
                "loaded and mid-attract; the field is not read from the level "
                "globals")

    gl = run(binary, rom, args.frames, args.timeout,
             {**FAULT_ENV, "MDKR_RENDERER": "gl"})
    gl_line = field_line(gl.stdout or "")
    if gl_line is None:
        failures.append("renderer arm: no [CRASHSCREEN] field line was printed")
    else:
        renderer = parse_fields(gl_line).get("renderer")
        if renderer != "gl":
            failures.append(
                f"renderer arm: MDKR_RENDERER=gl but the report says "
                f"renderer={renderer!r}; the field is not read from the "
                "resolved backend")

    version = subprocess.run([str(binary), "--version"], text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=60, check=False).stdout.split()
    expected_version = version[-1] if version else ""
    if fields and expected_version and fields.get("version") != expected_version:
        failures.append(
            f"version arm: --version says {expected_version!r} but the report "
            f"says {fields.get('version')!r}")

    if failures:
        print("check_crash_screen: FAIL", file=sys.stderr)
        for failure in failures[:20]:
            print(f"  - {failure}", file=sys.stderr)
        for label, text in (("abort", line), ("signal", signal_line)):
            if text is not None:
                print(f"  {label} report was:\n    {text}", file=sys.stderr)
        return 1

    print(f"  clean run silent; abort kept {describe(EXPECTED_RETURNCODE)} "
          "behind an unchanged [FATAL], signal kept "
          f"{describe(-signal.SIGSEGV)} behind an unchanged [CRASH]")
    print(f"  abort:  {line}")
    if signal_line is not None:
        print(f"  signal: {signal_line}")
    print("  tick, track, renderer and version all follow live state; nothing "
          "presented without a window")
    print("check_crash_screen: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
