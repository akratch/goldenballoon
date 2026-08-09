#!/usr/bin/env python3
"""A blind player must be able to follow a race, and the race must not notice.

Two claims, and the second is the harder one.

FOLLOWING THE RACE. A full eight-racer Tracks race is driven to its finish and
every `[SPEAK]` line it produces is read. The race has to announce that it
started, that the player's position changed, each lap, the final lap and the
result -- and it has to announce the position changes SPARINGLY. A mid-pack
scrap swaps positions every few ticks, and a voice that restarts on each swap
says nothing a player can use: platform/a11y_model.c treats every new utterance
as barging in on the one being spoken, so an uncoalesced stream is heard as
fragments of numbers. This gate therefore measures the SPACING between
consecutive `cat=race_position` lines, not merely their presence, and a build
that speaks every change fails it.

The clock those spacings are measured against is the existing per-frame `[PACE]`
line, which the same run already prints. Both markers are flushed line by line
to the same merged stream, so their interleaving is their emission order.

NOT NOTICING. Announcements are presentation. The proof is the authoritative
`[SIMHASH]` v3 stream: two runs of the same route, one with the announcer armed
and one without, must produce byte-identical hashes. This is not a formality --
arming the announcer arms the port's gameplay-event emission sites as well, so
if any of those had a side effect the divergence would show up here.

Audio safety: every run sets MDKR_AUDIO=0 and is bounded by an explicit
--headless-frames budget. The announcer emits text; no speech engine is reached.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

SCRIPT = "tests/input_scripts/nav_to_time_trial_race.txt"

# The route's own measurements. The race clock starts at ~frame 3120 and the AI
# field crosses the line at ~frame 7800; 9000 covers the finish and the results
# transition with room to spare. The shorter budget reaches the race and several
# position changes, which is all the purity and category arms need.
FULL_FRAMES = 9000
SHORT_FRAMES = 5400

SPEAK_RE = re.compile(r"^\[SPEAK\] cat=(\S+) pri=(\S+) text=(.*)$")
PACE_FRAME_RE = re.compile(r"\[PACE\] frame=(\d+)")
SIMHASH_RE = re.compile(r"^\[SIMHASH\] .*$")

# MDKR_A11Y_RACE_POSITION_MIN_TICKS in platform/a11y_race.h is 60 authoritative
# ticks, and this route pins one tick to one frame (enhanced cadence, synthetic
# fields). Asserting 50 rather than 60 leaves room for that mapping to change by
# a frame or two at the edges while staying far above anything an uncoalesced
# stream could produce: consecutive position changes in a tight pack are three
# ticks apart, which is the game's own debounce and nothing more.
MIN_POSITION_FRAMES = 50

RACE_CATEGORIES = ("race_position", "race_lap", "race_event")


class GateFailure(RuntimeError):
    """A failure whose message is the finding, not a stack trace."""


def clean_environment(**updates: str) -> dict[str, str]:
    """A process that inherits none of the caller's MDKR configuration."""
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(updates)
    return env


def session(root: Path, name: str, speech: bool) -> dict[str, str]:
    """One isolated run, with race announcements on or off.

    The settings FILE carries the choice rather than the environment, so the
    arms differ in exactly the way a player's two sessions would differ.
    """
    home = root / name
    home.mkdir(parents=True, exist_ok=True)
    config = home / "video.ini"
    config.write_text(
        "[Accessibility]\nSpeech={0}\nSpeechRace={0}\n".format(1 if speech else 0),
        encoding="utf-8")
    return clean_environment(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_VIDEO_CONFIG_PATH=str(config),
        MDKR_SAVE_DIR=str(home),
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_AUTOPILOT="1",
        MDKR_A11Y_TRACE="1",
    )


def run(executable: Path, rom: Path, env: dict[str, str], frames: int,
        label: str, timeout: int) -> str:
    completed = subprocess.run(
        [str(executable), "--headless-frames", str(frames),
         "--input-script", SCRIPT, "--rom", str(rom)],
        env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False)
    output = completed.stdout or ""
    if completed.returncode != 0:
        raise GateFailure(
            f"{label} exited {completed.returncode}\n{output[-4000:]}")
    return output


def utterances(output: str) -> list[tuple[str, str, str]]:
    found = []
    for line in output.splitlines():
        match = SPEAK_RE.match(line)
        if match:
            found.append(match.groups())
    return found


def position_frames(output: str) -> list[int]:
    """The frame each `cat=race_position` utterance was spoken on.

    Read from the nearest preceding `[PACE] frame=` line, which is emitted once
    per frame by the same process on the same merged stream.
    """
    frames: list[int] = []
    current = -1
    for line in output.splitlines():
        pace = PACE_FRAME_RE.search(line)
        if pace:
            current = int(pace.group(1))
            continue
        if line.startswith("[SPEAK] cat=race_position") and current >= 0:
            frames.append(current)
    return frames


def check_full_race(output: str) -> str:
    spoken = utterances(output)
    if not spoken:
        raise GateFailure(
            "the race produced zero [SPEAK] lines: a player relying on speech "
            "drove a whole race and was told nothing about it")
    by_category: dict[str, list[str]] = {}
    for category, _priority, text in spoken:
        by_category.setdefault(category, []).append(text)

    events = by_category.get("race_event", [])
    laps = by_category.get("race_lap", [])
    positions = by_category.get("race_position", [])

    missing = []
    if not any(text.startswith("Race started") for text in events):
        missing.append("the start of the race")
    if not positions:
        missing.append("any change of position")
    if not laps:
        missing.append("any lap")
    if not any(text == "Final lap" for text in events):
        missing.append("the final lap")
    if not any(text.startswith("Finished") for text in events):
        missing.append("the result")
    if missing:
        raise GateFailure(
            "the race never announced " + ", ".join(missing) +
            f"\nwhat it did say: {[text for _c, _p, text in spoken]}")

    # A lap call must name the lap the player is on and how many there are; a
    # bare "Lap 2" leaves a player unable to tell how much race is left.
    for text in laps:
        if not re.fullmatch(r"Lap \d+ of \d+", text):
            raise GateFailure(
                f"a lap announcement does not say which lap of how many: {text!r}")

    # The result is the one line that must survive whatever happens next, so it
    # is the one line the model may not cancel.
    for category, priority, text in spoken:
        if category == "race_event" and text.startswith("Finished"):
            if priority != "critical":
                raise GateFailure(
                    "the finish was announced at priority "
                    f"{priority!r}: a newer utterance may cut the result off "
                    "before the player hears where they came")

    # THE COALESCING ASSERTION.
    frames = position_frames(output)
    if len(frames) < 2:
        raise GateFailure(
            "fewer than two position announcements were timestamped, so the "
            "spacing between them was never tested. Either the race had no "
            "overtaking or [PACE] is not being printed; both make this gate "
            f"vacuous. frames={frames}")
    gaps = [later - earlier for earlier, later in zip(frames, frames[1:])]
    tightest = min(gaps)
    if tightest < MIN_POSITION_FRAMES:
        offenders = [(frames[i], gaps[i]) for i in range(len(gaps))
                     if gaps[i] < MIN_POSITION_FRAMES]
        raise GateFailure(
            f"position announcements are not coalesced: {len(offenders)} of "
            f"{len(gaps)} consecutive pairs are closer than "
            f"{MIN_POSITION_FRAMES} frames (tightest {tightest}). A player in "
            "a mid-pack scrap hears each line cut off by the next one and "
            "learns nothing.\n"
            f"  (frame, gap) = {offenders[:10]}")

    return (f"{len(spoken)} utterances: {len(events)} events, {len(laps)} laps, "
            f"{len(positions)} position calls, tightest position gap "
            f"{tightest} frames")


def check_purity(executable: Path, rom: Path, root: Path, timeout: int) -> str:
    """The authoritative stream may not notice that anybody is listening."""
    streams = {}
    for arm, speech in (("quiet", False), ("spoken", True)):
        env = session(root, f"purity-{arm}", speech)
        env["MDKR_STATE_HASH"] = "3"
        output = run(executable, rom, env, SHORT_FRAMES,
                     f"purity arm {arm!r}", timeout)
        streams[arm] = [line for line in output.splitlines()
                        if SIMHASH_RE.match(line)]
        if not streams[arm]:
            raise GateFailure(
                f"purity arm {arm!r} printed no [SIMHASH] rows, so the "
                "comparison below would compare nothing")
        if arm == "spoken" and not utterances(output):
            raise GateFailure(
                "the purity arm that is supposed to be talking said nothing, "
                "so it proves the announcer costs nothing only because it was "
                "never running")
    quiet, spoken = streams["quiet"], streams["spoken"]
    if len(quiet) != len(spoken):
        raise GateFailure(
            f"the announcer changed the number of authoritative ticks: "
            f"{len(quiet)} rows silent, {len(spoken)} rows speaking")
    for index, (a, b) in enumerate(zip(quiet, spoken)):
        if a != b:
            raise GateFailure(
                "the announcer moved authoritative state; the [SIMHASH] v3 "
                f"streams diverge at row {index}:\n  silent:   {a}\n"
                f"  speaking: {b}")
    return f"{len(quiet)} [SIMHASH] v3 rows byte-identical"


def check_category_toggles(executable: Path, rom: Path, root: Path,
                           timeout: int) -> str:
    """Each switch silences its own category and leaves the others alone."""
    reports = []
    for muted in RACE_CATEGORIES:
        kept = [name for name in RACE_CATEGORIES if name != muted]
        env = session(root, f"mute-{muted}", True)
        # The category names the switch uses, minus the "race_" the trace
        # prefixes them with.
        env["MDKR_A11Y_RACE_CATEGORIES"] = ",".join(
            name[len("race_"):] for name in kept)
        output = run(executable, rom, env, SHORT_FRAMES,
                     f"category arm {muted!r}", timeout)
        spoken = utterances(output)
        heard = {category for category, _priority, _text in spoken}
        if muted in heard:
            examples = [text for category, _p, text in spoken
                        if category == muted]
            raise GateFailure(
                f"switching {muted} off did not silence it: {examples[:5]}")
        silent_too = [name for name in kept if name not in heard]
        if silent_too:
            raise GateFailure(
                f"switching {muted} off also silenced {silent_too}. A player "
                "turning one kind of call off must keep the rest; heard "
                f"{sorted(heard)}")
        reports.append(f"{muted} off leaves {'+'.join(sorted(heard))}")
    return "; ".join(reports)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64", type=Path)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()

    executable = Path(resolve_binary(args.build))
    if not executable.is_file():
        parser.error(f"missing executable: {executable}")
    rom = args.rom.expanduser()
    if not rom.is_file():
        parser.error(f"missing ROM: {rom}")
    if not Path(SCRIPT).is_file():
        parser.error(f"missing input script: {SCRIPT}")

    with tempfile.TemporaryDirectory(prefix="mdkr64_a11y_race_") as temporary:
        root = Path(temporary)
        try:
            env = session(root, "full", True)
            env["MDKR_TRACE"] = "1"   # the [PACE] clock the spacing is measured on
            race = check_full_race(
                run(executable, rom, env, FULL_FRAMES, "full race",
                    args.timeout))
            purity = check_purity(executable, rom, root, args.timeout)
            toggles = check_category_toggles(executable, rom, root,
                                             args.timeout)
        except GateFailure as failure:
            print(f"FAIL a11y race: {failure}", file=sys.stderr)
            return 1
        except subprocess.TimeoutExpired as expired:
            print(f"FAIL a11y race: {expired}", file=sys.stderr)
            return 1

    print(f"PASS a11y race: {race}; purity: {purity}; toggles: {toggles}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
