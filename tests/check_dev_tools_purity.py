#!/usr/bin/env python3
"""Every registered developer tool must leave the race exactly as it found it.

WHAT THIS GATE IS FOR. `platform/app/dev_tools.h`'s registry is a claim
mechanism: putting a tool in the table asserts that opening its window cannot
move authoritative state. This script is the thing that tests that claim. It
does NOT carry a list of tools -- it asks the binary for one (`--dump-tool-table`)
and generates an arm per row, so a tool added in a later task is born gated and
nobody has to remember to extend a test.

THE VACUITY THIS GATE IS ESPECIALLY EXPOSED TO. Every tool in the table is inert
today (`draw == NULL`), which is precisely the situation in which a comparison
gate passes while proving nothing. Four assertions exist only to close that:

  * zero parsed `[TOOLTABLE]` rows is a FAILURE, not "nothing to check";
  * each arm must show `[TOOLS-OPEN] name=<tool> ... forced=1`, so an arm that
    misspelled its tool cannot quietly compare a run against itself;
  * each arm must show `[TOOLS-ARMED]` and a `[TOOLS-SUMMARY] draws=N` with
    N > 0, so the tool host must actually have RUN, every frame, in the run
    being compared;
  * each arm must reach a real race (`cp > 0` on the player-one probe), so the
    race-outcome comparison below is comparing an outcome and not a menu.

WHY THE ARMS RUN THROUGH THE APP SHELL. A tool is an ImGui window, and the
in-game ImGui frame only exists when the app shell has installed its overlay
hooks -- a plain `--headless-frames` engine invocation never builds one. So the
per-tool arms run the shell's own unattended path (`MDKR_APP_AUTOPLAY`, a hidden
window, `--headless-ticks` synthesised by the shell) where `DevTools_draw()`
genuinely executes on every frame. Anything less would be measuring a code path
the player never runs. The `engine` arm below is the plain `--headless-frames`
invocation, kept because it is the one that proves the key costs nothing on the
CLI path, where the tool surface does not exist at all.

WHAT EACH ARM COMPARES against the tools-off baseline:

  state       the v3 `[SIMHASH]` stream, byte-identical. This is the assertion.
  frames      the presented-frame count (`[PACE]` rows). A tool that merely
              slowed the frame loop would leave the tick stream identical and
              change this, which is why the hash alone is not enough.
  outcome     the player-one probe -- position, race clock, checkpoint, lap,
              finish -- on every frame it is valid. Identical, in order.

`[PACE]`'s `dtms` and `cumwf` fields are wall-clock and vary between two runs of
the same build; only the probe payload after `| racer` is compared.

Always muted + headless. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import (ABORT_MARKERS, ASSERT_MARKERS, DEFAULT_BUILD_DIR,
                           find_fatal, resolve_binary)

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"

# Long enough that the route is past the countdown and genuinely driving: the
# outcome comparison is worth nothing on a run that never left the menus.
TICKS = 3600
# The complete authority contract, same as check_render_purity's arms.
HASH_VERSION = "3"
# Track 5 with DKR's own AI at the wheel, matching the other purity gates.
TRACK = "5"

TOOLTABLE_RE = re.compile(
    r'\[TOOLTABLE\] id=(\d+) name=(\S+) title="([^"]*)" hotkey=(\S+) '
    r'registered=(\d)')
# frame ordinal + the probe payload. Everything between them is pacing/wall
# clock and is deliberately not captured.
RACER_RE = re.compile(r"\[PACE\] frame=(\d+) .*\| racer (.*)$")
PACE_RE = re.compile(r"\[PACE\] frame=(\d+)")
FORCED_RE = re.compile(r"\[TOOLS-OPEN\] name=(\S+) id=(\d+) forced=1")
SUMMARY_RE = re.compile(r"\[TOOLS-SUMMARY\] draws=(\d+) dispatched=(\d+)")
CHECKPOINT_RE = re.compile(r"\bcp=(-?\d+)")


class ArmError(RuntimeError):
    """A run that could not produce evidence, as opposed to failing evidence."""


class Arm:
    """One complete run and the three streams the comparison is made over."""

    def __init__(self, label: str, output: str):
        self.label = label
        self.output = output
        self.state: list[str] = []
        self.frames: list[str] = []
        self.outcome: list[tuple[str, str]] = []
        for line in output.splitlines():
            if line.startswith("[SIMHASH]"):
                self.state.append(line)
                continue
            # mdkr_trace() prefixes its own "[TRACE] ", so these are searched
            # for inside the line rather than anchored at its start.
            pace = PACE_RE.search(line)
            if pace is None:
                continue
            self.frames.append(pace.group(1))
            racer = RACER_RE.search(line)
            if racer is not None:
                self.outcome.append((racer.group(1), racer.group(2)))

    @property
    def final_checkpoint(self) -> int:
        if not self.outcome:
            return -1
        match = CHECKPOINT_RE.search(self.outcome[-1][1])
        return int(match.group(1)) if match else -1


def clean_env(**overrides: str) -> dict[str, str]:
    """A run's environment, with the developer's own MDKR_* preferences dropped.

    An inherited MDKR_TOOLS or MDKR_RENDERER would silently change what the
    baseline is a baseline OF, which is the same trap check_determinism and
    check_render_purity strip for.
    """
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(LC_ALL="C", MDKR_AUDIO="0")
    env.update(overrides)
    return env


def complete(label: str, command: list[str], env: dict[str, str], cwd: Path,
             timeout: int, verbose: bool) -> str:
    if verbose:
        forced = env.get("MDKR_TOOLS_OPEN", "-")
        print(f"$ ({label}) MDKR_TOOLS={env.get('MDKR_TOOLS', '-')} "
              f"MDKR_TOOLS_OPEN={forced} {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False)
    output = process.stdout or ""
    if process.returncode != 0:
        raise ArmError(f"{label}: exit {process.returncode}\n{output[-3000:]}")
    marker = find_fatal(output, *ABORT_MARKERS, *ASSERT_MARKERS)
    if marker:
        raise ArmError(f"{label}: fatal marker {marker}\n{output[-3000:]}")
    return output


def dump_table(binary: Path, root: Path, timeout: int,
               verbose: bool) -> list[tuple[str, ...]]:
    """Enumerate the registry from the binary itself."""
    run_dir = root / "table"
    run_dir.mkdir(parents=True)
    # --headless-frames rides along unused: the dump returns before the engine
    # starts, and the project's rule is that no invocation of this binary is
    # ever written without a bounded frame budget beside MDKR_AUDIO=0.
    output = complete(
        "tooltable", [str(binary), "--dump-tool-table", "--headless-frames", "1"],
        clean_env(MDKR_APP_PREFS_DIR=str(run_dir / "prefs"),
                  MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
                  MDKR_SAVE_DIR=str(run_dir / "save")),
        run_dir, timeout, verbose)
    return TOOLTABLE_RE.findall(output)


def run_app_arm(binary: Path, rom: Path, root: Path, label: str,
                tool: str | None, timeout: int, verbose: bool) -> Arm:
    """One unattended app-shell race, with the tool surface off or one tool open."""
    run_dir = root / label
    (run_dir / "prefs").mkdir(parents=True)
    (run_dir / "save").mkdir(parents=True)
    env = clean_env(
        MDKR_APP_AUTOPLAY="1",
        MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
        MDKR_APP_AUTOPLAY_INPUT_SCRIPT=str(SCRIPT),
        MDKR_APP_PREFS_DIR=str(run_dir / "prefs"),
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=TRACK,
        MDKR_RENDERER="gl",
        MDKR_ROM=str(rom),
        MDKR_SAVE_DIR=str(run_dir / "save"),
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_TOOLS="1" if tool else "0",
        MDKR_TRACE="1",
        MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
        MDKR64_HIDDEN="1",
    )
    if tool:
        env["MDKR_TOOLS_OPEN"] = tool
    return Arm(label, complete(label, [str(binary)], env, run_dir, timeout,
                               verbose))


def run_engine_arm(binary: Path, rom: Path, root: Path, label: str,
                   enabled: bool, timeout: int, verbose: bool) -> Arm:
    """The plain CLI path: no shell, no ImGui, so no tool surface exists at all."""
    run_dir = root / label
    (run_dir / "save").mkdir(parents=True)
    env = clean_env(
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=TRACK,
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(run_dir / "save"),
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_TOOLS="1" if enabled else "0",
        MDKR_TRACE="1",
        MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
    )
    command = [str(binary), "--headless-frames", str(TICKS),
               "--input-script", str(SCRIPT), "--rom", str(rom),
               "--window-size", "320x240"]
    return Arm(label, complete(label, command, env, run_dir, timeout, verbose))


def compare(arm: Arm, baseline: Arm) -> list[str]:
    """The three stream comparisons. Order matters: state first, it is the point."""
    failures: list[str] = []
    for name, actual, reference, note in (
            ("v3 state", arm.state, baseline.state,
             "opening this tool moved authoritative state"),
            ("presented frame", arm.frames, baseline.frames,
             "opening this tool changed how many frames were presented, so it "
             "is paying for itself out of the frame loop"),
            ("race outcome", arm.outcome, baseline.outcome,
             "opening this tool changed where the racer ended up")):
        if actual == reference:
            continue
        if len(actual) != len(reference):
            failures.append(
                f"{arm.label}: {name} stream has {len(actual)} rows against "
                f"the baseline's {len(reference)} — {note}")
            continue
        index = next(i for i, (a, b) in enumerate(zip(reference, actual))
                     if a != b)
        failures.append(
            f"{arm.label}: {name} stream diverged at row {index} — {note}\n"
            f"    baseline: {reference[index]}\n    tool open: {actual[index]}")
    return failures


def check_evidence(arm: Arm, tool: str) -> list[str]:
    """The arm must have actually done the thing it claims to be a control for."""
    failures: list[str] = []
    forced = dict((name, int(ident)) for name, ident in FORCED_RE.findall(arm.output))
    if tool not in forced:
        failures.append(
            f"{arm.label}: no [TOOLS-OPEN] name={tool} forced=1 row — the arm "
            "never opened the tool it is named for, so its comparison is "
            "against an identical run")
    if "[TOOLS-ARMED]" not in arm.output:
        failures.append(
            f"{arm.label}: no [TOOLS-ARMED] row — DevTools_draw() never ran, "
            "so nothing about the tool host was exercised")
    summary = SUMMARY_RE.search(arm.output)
    if summary is None:
        failures.append(f"{arm.label}: no [TOOLS-SUMMARY] row")
    elif int(summary.group(1)) == 0:
        failures.append(
            f"{arm.label}: [TOOLS-SUMMARY] draws=0 — the tool host was armed "
            "and then never drew a frame")
    return failures


def check_reached_race(arm: Arm) -> list[str]:
    """A run that stayed in the menus makes the outcome comparison meaningless."""
    if not arm.state:
        return [f"{arm.label}: no [SIMHASH] rows — the run produced no "
                "authoritative state to compare"]
    if not arm.outcome:
        return [f"{arm.label}: the player-one probe never became valid — the "
                f"{TICKS}-tick route did not reach a race"]
    if arm.final_checkpoint <= 0:
        return [f"{arm.label}: finished at checkpoint {arm.final_checkpoint} — "
                "the racer is not driving, so comparing the outcome proves "
                "nothing"]
    return []


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--tool", action="append", dest="tools",
                        help="restrict to these tool names (repeatable)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT):
        if not os.path.exists(path):
            print(f"check_dev_tools_purity: FAIL — missing {path}",
                  file=sys.stderr)
            return 1

    failures: list[str] = []
    notes: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-dev-tools-purity-") as tmp:
        root = Path(tmp)
        try:
            table = dump_table(binary, root, args.timeout, args.verbose)
        except ArmError as error:
            print(f"check_dev_tools_purity: FAIL\n  - {error}")
            return 1

        # Fail closed. A gate that iterates an empty list reports success while
        # having tested nothing, and with every tool inert this one would look
        # exactly the same doing it.
        if not table:
            print("check_dev_tools_purity: FAIL\n  - --dump-tool-table parsed "
                  "ZERO [TOOLTABLE] rows. The registry is the only list this "
                  "gate has; with no rows it verifies no tools and must not "
                  "pass.")
            return 1

        names = [name for _id, name, _title, _hotkey, _reg in table]
        if args.tools:
            unknown = sorted(set(args.tools) - set(names))
            if unknown:
                print(f"check_dev_tools_purity: FAIL — --tool names no such "
                      f"tool: {', '.join(unknown)}")
                return 1
            names = [name for name in names if name in args.tools]

        registered = sum(int(reg) for *_rest, reg in table)
        print(f"check_dev_tools_purity: {len(table)} tools in the registry "
              f"({registered} with a draw callback), {TICKS} ticks per arm")

        try:
            baseline = run_app_arm(binary, rom, root, "baseline", None,
                                   args.timeout, args.verbose)
            arms = [(tool, run_app_arm(binary, rom, root, f"tool-{tool}", tool,
                                       args.timeout, args.verbose))
                    for tool in names]
            engine_off = run_engine_arm(binary, rom, root, "engine-off", False,
                                        args.timeout, args.verbose)
            engine_on = run_engine_arm(binary, rom, root, "engine-on", True,
                                       args.timeout, args.verbose)
        except (ArmError, subprocess.TimeoutExpired) as error:
            print(f"check_dev_tools_purity: FAIL\n  - {error}")
            return 1

        failures += check_reached_race(baseline)
        # Default off must cost nothing AND say nothing: a baseline that had
        # armed the host would not be a baseline.
        if "[TOOLS-ARMED]" in baseline.output:
            failures.append(
                "baseline: the tool host armed itself with Tools.Enabled=0 — "
                "the default-off guarantee is broken and every comparison "
                "below is against a contaminated reference")

        verified = 0
        for tool, arm in arms:
            arm_failures = (check_reached_race(arm) + check_evidence(arm, tool)
                            + compare(arm, baseline))
            failures += arm_failures
            if not arm_failures:
                verified += 1

        failures += compare(engine_on, engine_off)
        failures += check_reached_race(engine_off)
        if not failures:
            notes.append(
                f"baseline reached checkpoint {baseline.final_checkpoint} over "
                f"{len(baseline.state)} authoritative ticks and "
                f"{len(baseline.frames)} presented frames")
            notes.append(
                "engine arm: Tools.Enabled=1 on the plain --headless-frames "
                "path, where no tool surface exists, is byte-identical too")
            notes.append(
                f"{registered} of {len(table)} tools have a draw callback so "
                "far; the rest are asserted inert in the slot they will fill")

    if failures:
        print("check_dev_tools_purity: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(f"check_dev_tools_purity: PASS — {verified} of {len(names)} "
          f"registered tools verified inert: v3 [SIMHASH] stream, presented "
          f"frame count and race outcome all byte-identical to a "
          f"Tools.Enabled=0 race over {TICKS} ticks")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
