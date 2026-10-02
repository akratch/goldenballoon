#!/usr/bin/env python3
"""This build against the release it replaces: retail simulation and saves.

Why this exists
---------------
Every determinism gate in the tree compares a build with itself: the same
binary twice (check_determinism.py, check_state_hash.py) or the same source on
two architectures (check_crossarch_determinism.py). None of them can notice
that a release changed how the retail game plays, because each side of the
comparison carries the change. A player notices at once: an AI that used to
take one line through a battle arena takes another, or a 1.7.0 save comes back
as someone else's.

This gate puts the published previous release on the other side. It runs a
build of that release (``--reference-build``, v1.7.0 by default) and this
build through the same ROM, the same input script and the same retail route,
and requires the authoritative state to agree tick for tick. It then has the
previous release write a Time Trial record and a Controller Pak ghost and makes
this build read both back.

What it asserts
---------------
1. **Retail simulation parity, per route.** Both builds run with fresh
   isolated saves and preferences, no content packs, Original simulation
   cadence and the same seeds, and their [SIMHASH] v3 streams must be
   byte-identical for every one of ``--frames`` ticks. A red route names the
   first divergent tick, both rows, and whether the population or only a value
   moved (simhash_streams.divergence_report).
2. **The route really raced.** The route's level is loaded as the race in both
   runs (with the requested vehicle), at least MIN_RACE_TICKS of the compared
   ticks come after that load, and no content pack is active. Equality over a
   menu, or over a pack the reference does not have, would prove nothing.
3. **Positive control.** One extra run of this build with
   ``MDKR_RNGSEED=legacy`` must DIVERGE from its own base stream. Without it
   an empty capture or a comparator that always says "identical" passes.
4. **The reference is what it claims to be.** Its source tree is a clean
   checkout of ``--reference-tag``, both builds share build type and every
   project option they both have, the two are not the same executable, and
   neither is older than its own source.
5. **Saves cross the boundary.** From a fresh save directory, the reference
   drives a Time Trial on Ancient Lake in the car and saves its ghost. This
   build then decodes that EEPROM with the game's own codec
   (MDKR_SAVE_COMPAT_FIXTURE), re-encodes the record blocks byte for byte and
   reads back the course record the reference finished with; and, in a fresh
   process, loads the reference's ghost for the same pair (same nodes,
   character and time) and plays it without rewriting the pak. A save written
   by the published release is what a player brings to this one.

The routes cover what the retail simulation actually does differently: an AI
race in two worlds and two vehicles, a boss race, and all four challenge
arenas (eggs, bananas and both battle arenas). The arenas with AI nodes --
Smokey Castle, Darkwater Beach and Icicle Pyramid; Fire Mountain has none --
are where the AI plans routes through the node graph (func_8001CD28) rather
than following the racing line. Each route is a single Tracks race retargeted
by MDKR_LOAD_TRACK from the one menu route every state-hash gate shares, on
the US ROM and, when ``--roms`` holds a PAL v80 image, on the PAL ROM too.

A divergence is a finding, not noise: both builds are deterministic run to run
(check_state_hash.py), so a red route means the two releases play that route
differently. Bisect it over ``<tag>..HEAD`` and classify it as a documented,
intended change or a regression; never widen the route set's tolerance.

Skipping
--------
Without ``--reference-build`` the gate prints why it did not run and exits
EXIT_SKIPPED (77). tools/run_checks.py reports that as SKIPPED -- never as a
pass -- and names it in the verdict line. A release run supplies the reference
build (docs/RELEASE_CHECKLIST.md).

Usage
-----
    tests/check_previous_release_parity.py --build build-rel \\
        --reference-build ../mdkr64-v170-ref/build-rel \\
        --rom baserom.us.v80.z64 [--roms DIR] [--jobs 2] [-v]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import NamedTuple

from harness_utils import DEFAULT_BUILD_DIR, find_fatal, find_revision_rom, resolve_binary
from simhash_streams import (HashedRun, SimHashRow, configuration_problems,
                             control_problem, divergence_report,
                             first_divergence, newest_source_mtime,
                             parse_cmake_cache, read_cmake_cache, run_hashed,
                             staleness_problems, stream_problems)

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"

# The menu route reaches the race at tick ~2620 (the boss route's race proper
# at ~2974, after its intro), so 6600 ticks leave more than MIN_RACE_TICKS of
# racing on every route.
DEFAULT_FRAMES = 6600
MIN_RACE_TICKS = 3000
DEFAULT_REFERENCE_TAG = "v1.7.0"
EXIT_SKIPPED = 77
ROM_BYTES = 12 * 1024 * 1024
WINDOW = "320x240"

# Project options that legitimately differ between two releases' builds.
RELEASE_IDENTITY_KEYS = ("MDKR_VERSION", "MDKR_BUILD_STAMP")


class Route(NamedTuple):
    name: str
    level: int
    vehicle: int | None
    what: str

    @property
    def load_track(self) -> str:
        return str(self.level) if self.vehicle is None else f"{self.level}:{self.vehicle}"


ROUTES = (
    Route("ancient-lake", 5, None, "AI race, Dino Domain, car"),
    Route("whale-bay", 8, 1, "AI race, Sherbet Island, hovercraft"),
    Route("tricky-tops-1", 38, None, "boss race, Tricky"),
    Route("fire-mountain", 11, None, "challenge arena, eggs"),
    Route("smokey-castle", 25, None, "challenge arena, bananas"),
    Route("darkwater-beach", 26, None, "battle arena"),
    Route("icicle-pyramid", 27, None, "battle arena"),
)

LEVEL_LOAD_RE = re.compile(
    r"level_load: levelId=(\d+) numPlayers=(-?\d+) entrance=(-?\d+) "
    r"vehicle=(-?\d+) cutscene=(-?\d+) @frame~(\d+)")
ACTIVE_PACKS_RE = re.compile(r"\[MODS\] ([1-9]\d*) pack\(s\) active")

# The saves arm: Time Trial, Ancient Lake, car, on the pacing
# check_ghost_matrix.py drives that pair with.
TT_LEVEL, TT_VEHICLE = 5, 0
TT_MEASURE_FRAMES = 9000
TT_READ_FRAMES = 9000
# Frames after the post-race OPTIONS stage opens: long enough for the SAVE
# GHOST confirmation and the pak write, short enough that no second race ends.
TT_WRITE_TAIL = 240
MIN_GHOST_FRAMES = 100
PAK_IMAGE = "controller-pak-1.mdp"
SAVE_COMPAT_RECORD_RE = re.compile(
    r"\[SAVE_COMPAT\] record level=(\d+) vehicle=(\d+) lap=(\d+) lapInitials=(\d+) "
    r"course=(\d+) courseInitials=(\d+)")
SAVE_COMPAT_REENCODE_RE = re.compile(
    r"\[SAVE_COMPAT\] (\S+) reencode=(identical|DIFFERENT) bytes=(\d+)")
SAVE_COMPAT_DONE_RE = re.compile(
    r"\[SAVE_COMPAT\] done slots=(\d+) records=(\d+) config=(\d+) mismatches=(\d+)")


# --------------------------------------------------------------------------- #
#  Pure helpers (unit-tested by tests/test_previous_release_parity.py)
# --------------------------------------------------------------------------- #


def race_start(output: str, route: Route) -> int | None:
    """The frame the route's race was loaded at, or ``None``.

    The last load of the route's level as a one-player race counts: the boss
    route loads its level twice, once for the intro and once for the race.
    """

    frame = None
    for match in LEVEL_LOAD_RE.finditer(output):
        level, players, vehicle = (int(match.group(1)), int(match.group(2)),
                                   int(match.group(4)))
        if level != route.level or players != 0:
            continue
        if route.vehicle is not None and vehicle != route.vehicle:
            continue
        frame = int(match.group(6))
    return frame


def route_problems(route_label: str, route: Route, output: str,
                   frames: int, side: str) -> list[str]:
    """Why a run cannot stand as evidence for ``route``."""

    problems: list[str] = []
    start = race_start(output, route)
    if start is None:
        loads = [(int(m.group(1)), int(m.group(2)), int(m.group(4)))
                 for m in LEVEL_LOAD_RE.finditer(output)]
        problems.append(
            f"{route_label} {side}: level {route.level} was never loaded as the race"
            f"{'' if route.vehicle is None else f' with vehicle {route.vehicle}'}; "
            f"loads (level, players, vehicle): {loads}. The route raced "
            "something else, so its equality would prove nothing about this one")
    elif frames - start < MIN_RACE_TICKS:
        problems.append(
            f"{route_label} {side}: only {frames - start} compared ticks after the "
            f"race loaded at {start}; raise --frames to at least "
            f"{start + MIN_RACE_TICKS}")
    packs = ACTIVE_PACKS_RE.search(output)
    if packs is not None:
        problems.append(f"{route_label} {side}: {packs.group(1)} content pack(s) were "
                        "active; the run was not the retail game")
    marker = find_fatal(output)
    if marker:
        problems.append(f"{route_label} {side}: log contains {marker!r}")
    return problems


def skip_banner(reason: str) -> str:
    rule = "=" * 72
    return (f"{rule}\ncheck_previous_release_parity: SKIPPED -- {reason}\n"
            "  Nothing was compared, so nothing here is evidence that this build "
            "plays\n  or saves like the published release. A release run passes "
            "--reference-build\n  (docs/RELEASE_CHECKLIST.md).\n" + rule)


def best_finish_clock(output: str) -> int | None:
    """The lowest race clock at which the traced human racer finished."""

    clocks: list[int] = []
    previous = None
    for match in re.finditer(r"\[PACE\] frame=\d+.*clock=(\d+) .*fin=(-?\d+)", output):
        finished = int(match.group(2))
        if finished == 1 and previous != 1:
            clocks.append(int(match.group(1)))
        previous = finished
    return min(clocks) if clocks else None


def ghost_events(output: str, kind: str) -> list[tuple[int, int, int, int, int, int]]:
    """``[TTGHOST] event=<kind>`` rows as (level, vehicle, status, nodes, character, time)."""

    return [tuple(int(v) for v in m.groups()[1:])
            for m in re.finditer(
                r"\[TTGHOST\] frame=(\d+) event=" + kind + r" level=(\d+) vehicle=(\d+) "
                r"status=(-?\d+) nodes=(-?\d+) character=(-?\d+) time=(-?\d+)", output)]


# --------------------------------------------------------------------------- #
#  The reference build
# --------------------------------------------------------------------------- #


def git(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    return subprocess.run(["git", "-C", str(root), *args], text=True, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def reference_problems(reference: Path, current: Path, tag: str,
                       allow_stale: bool) -> tuple[list[str], list[str]]:
    """(failures, notes) about whether the pair can support a parity claim."""

    problems: list[str] = []
    notes: list[str] = []
    if reference.resolve() == current.resolve():
        problems.append("--reference-build and --build are the same executable; "
                        "a build compared with itself passes without testing anything")
    elif (hashlib.sha256(reference.read_bytes()).digest()
          == hashlib.sha256(current.read_bytes()).digest()):
        problems.append("the reference executable is byte-identical to this build; "
                        "it cannot be a build of the previous release")
    ref_cache = read_cmake_cache(reference)
    cur_cache = read_cmake_cache(current)
    if not ref_cache or not cur_cache:
        problems.append("both builds need their CMakeCache.txt beside the executable, "
                        "so their configurations can be compared")
        return problems, notes
    one_sided = sorted({k for k in ref_cache.keys() ^ cur_cache.keys()
                        if k.startswith("MDKR_")})
    for key in one_sided:
        side = "this build" if key in cur_cache else "the reference"
        notes.append(f"option {key} exists only in {side}'s build; not compared")
    problems += configuration_problems(
        ref_cache, cur_cache, labels=(tag, "this build"),
        consequence="Build the reference exactly as this build was configured, or "
                    "this gate measures the configuration difference",
        ignore=tuple(one_sided) + RELEASE_IDENTITY_KEYS)

    source = Path(ref_cache.get("CMAKE_HOME_DIRECTORY", ""))
    if not source.is_dir():
        problems.append(f"the reference's source tree ({source}) is not available, so "
                        f"it cannot be shown to be {tag}")
    else:
        head = git(source, "rev-parse", "HEAD")
        want = git(source, "rev-parse", f"{tag}^{{commit}}")
        dirty = git(source, "status", "--porcelain", "--untracked-files=no")
        if head.returncode != 0 or want.returncode != 0:
            problems.append(f"cannot resolve HEAD and {tag} in the reference source "
                            f"{source}: {(head.stderr or want.stderr).strip()}")
        elif head.stdout.strip() != want.stdout.strip():
            problems.append(f"the reference source {source} is at "
                            f"{head.stdout.strip()[:12]}, not {tag} "
                            f"({want.stdout.strip()[:12]})")
        elif dirty.returncode != 0 or dirty.stdout.strip():
            problems.append(f"the reference source {source} has local changes; it is "
                            f"not the published {tag}")
        if not allow_stale:
            newest, newest_path = newest_source_mtime(source)
            problems += staleness_problems(
                {"the reference binary": reference}, newest, newest_path,
                consequence="Rebuild the reference from its own tree")
    if not allow_stale:
        newest, newest_path = newest_source_mtime(ROOT)
        problems += staleness_problems(
            {"this build": current}, newest, newest_path,
            consequence="Rebuild before comparing; a stale binary compares a tree "
                        "that no longer exists")
    return problems, notes


# --------------------------------------------------------------------------- #
#  Process-facing arms
# --------------------------------------------------------------------------- #


def isolation_env(run_dir: Path) -> dict[str, str]:
    """Preferences and user content roots owned by the run, and Original pacing."""

    return {
        "MDKR_APP_PREFS_DIR": str(run_dir / "prefs"),
        "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(run_dir / "characters"),
        # The previous release has no MDKR_MODS_DIR; its private cwd is what
        # keeps it off a checkout's mods/. This build gets both.
        "MDKR_MODS_DIR": str(run_dir / "mods"),
        "MDKR_SIMULATION_CADENCE": "original",
        "MDKR_PRESENT_RATE": "original",
        "MDKR_TRACE": "1",
    }


class Arm(NamedTuple):
    label: str
    binary: Path
    rom: Path
    route: Route
    extra_env: dict[str, str]


def run_route_arm(arm: Arm, root: Path, frames: int, timeout: int,
                  verbose: bool) -> HashedRun | str:
    run_dir = root / arm.label
    env = isolation_env(run_dir)
    env["MDKR_LOAD_TRACK"] = arm.route.load_track
    env.update(arm.extra_env)
    try:
        return run_hashed(arm.binary, arm.rom, run_dir, arm.label, frames, SCRIPT,
                          env, timeout, verbose, window=WINDOW)
    except RuntimeError as error:
        return str(error)


def run_plain(binary: Path, rom: Path, run_dir: Path, save_dir: Path, label: str,
              frames: int, script: Path | None, env_overrides: dict[str, str],
              timeout: int, verbose: bool) -> tuple[int, str]:
    """One hermetic engine run with a caller-owned save directory."""

    run_dir.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MDKR", "GE007_"))}
    env.update(LC_ALL="C", LANG="C", TZ="UTC", MDKR_AUDIO="0",
               MDKR_SAVE_DIR=str(save_dir),
               MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"))
    env.update(isolation_env(run_dir))
    env.update(env_overrides)
    command = [str(binary), "--headless-frames", str(frames), "--rom", str(rom)]
    if script is not None:
        command += ["--input-script", str(script)]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        process = subprocess.run(command, cwd=run_dir, env=env, text=True,
                                 errors="replace", stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, timeout=timeout, check=False)
    except subprocess.TimeoutExpired:
        return -1, f"{label}: timed out after {timeout}s"
    return process.returncode, process.stdout or ""


def time_trial_env() -> dict[str, str]:
    return {
        "MDKR_SYNTH_FIELDS": "2",
        "MDKR_AUTOPILOT": "1",
        # The autopilot's stuck-recovery deadlock (check_ghost_matrix.py).
        "MDKR_AUTOPILOT_UNSTICK": "1",
        "MDKR_LOAD_TRACK": f"{TT_LEVEL}:{TT_VEHICLE}",
    }


def saves_arm(reference: Path, current: Path, rom: Path, root: Path, tag: str,
              timeout: int, verbose: bool) -> tuple[list[str], list[str]]:
    """(failures, evidence) for assertion 5."""

    import check_ghost_matrix as ghosts  # the Time Trial menu route and its probes

    failures: list[str] = []
    evidence: list[str] = []
    work = root / "saves"
    work.mkdir()

    # 1. Where the reference's post-race OPTIONS stage offers SAVE GHOST.
    measure_script = work / "measure.txt"
    ghosts.generate_script(str(measure_script), TT_MEASURE_FRAMES)
    code, measure = run_plain(reference, rom, work / "measure", work / "measure-save",
                              f"{tag} measure", TT_MEASURE_FRAMES, measure_script,
                              time_trial_env(), timeout, verbose)
    offered = [tuple(int(g) for g in m.groups())
               for m in ghosts.OPTIONS_RE.finditer(measure) if m.group(4) == "1"]
    if code != 0 or not offered:
        return [f"saves: the {tag} Time Trial never offered SAVE GHOST "
                f"(exit {code}); nothing was written to carry across"], evidence
    options_frame, _option, option_count, _has = offered[0]

    # 2. The reference writes its record and ghost into a fresh save directory.
    ref_save = work / "reference-save"
    ref_save.mkdir()
    write_script = work / "write.txt"
    write_frames = options_frame + TT_WRITE_TAIL
    ghosts.generate_script(str(write_script), write_frames, options_frame=options_frame,
                           option_index=option_count - 2)
    code, write = run_plain(reference, rom, work / "write", ref_save, f"{tag} write",
                            write_frames, write_script, time_trial_env(), timeout,
                            verbose)
    saved = [e for e in ghost_events(write, "save") if e[2] == 0 and e[3] > 0]
    finish = best_finish_clock(write)
    eeprom = ref_save / "eeprom.bin"
    pak = ref_save / PAK_IMAGE
    if code != 0 or not saved or finish is None or not eeprom.is_file() or not pak.is_file():
        return [f"saves: the {tag} write run did not finish a race, save a ghost and "
                f"leave eeprom.bin and {PAK_IMAGE} (exit {code}, saves {saved}, "
                f"finish {finish})"], evidence
    _level, _vehicle, _status, nodes, character, ghost_time = saved[-1]
    evidence.append(f"{tag} wrote a course record at clock {finish} and a {nodes}-node "
                    f"ghost (character {character}, time {ghost_time})")

    # 3. This build's game codec decodes and re-encodes the reference's EEPROM.
    image = work / "reference-eeprom.bin"
    shutil.copyfile(eeprom, image)
    code, decoded = run_plain(current, rom, work / "decode", work / "decode-save",
                              "this build decode", 30, None,
                              {"MDKR_SAVE_COMPAT_FIXTURE": str(image)}, timeout, verbose)
    done = SAVE_COMPAT_DONE_RE.search(decoded)
    reencoded = {m.group(1): m.group(2) for m in SAVE_COMPAT_REENCODE_RE.finditer(decoded)}
    records = {(int(m.group(1)), int(m.group(2))): int(m.group(5))
               for m in SAVE_COMPAT_RECORD_RE.finditer(decoded)}
    if code != 0 or done is None:
        failures.append(f"saves: this build's MDKR_SAVE_COMPAT_FIXTURE seam did not run "
                        f"on the {tag} EEPROM (exit {code})")
    else:
        if done.group(2) != "1" or done.group(4) != "0":
            failures.append(f"saves: decoding the {tag} EEPROM reported records="
                            f"{done.group(2)} mismatches={done.group(4)}")
        for block in ("records-owned=0", "records-owned=1", "records"):
            if reencoded.get(block) != "identical":
                failures.append(f"saves: the {tag} {block} block did not re-encode byte "
                                f"for byte ({reencoded.get(block)})")
        if records.get((TT_LEVEL, TT_VEHICLE)) != finish:
            failures.append(f"saves: this build reads the {tag} course record for level "
                            f"{TT_LEVEL} car as {records.get((TT_LEVEL, TT_VEHICLE))}, but "
                            f"{tag} finished at clock {finish}")
        else:
            evidence.append(f"this build decodes that record as {finish} and re-encodes "
                            "both record blocks byte for byte")

    # 4. A fresh process of this build loads and plays the reference's ghost.
    read_save = work / "read-save"
    shutil.copytree(ref_save, read_save)
    pak_before = (read_save / PAK_IMAGE).read_bytes()
    read_script = work / "read.txt"
    ghosts.generate_script(str(read_script), TT_READ_FRAMES)
    code, read = run_plain(current, rom, work / "read", read_save, "this build read",
                           TT_READ_FRAMES, read_script, time_trial_env(), timeout,
                           verbose)
    loads = [e for e in ghost_events(read, "load") if e[3] >= 0]
    playback = [(int(m.group(1)), int(m.group(2))) for m in re.finditer(
        r"\[PACE\] frame=\d+.* ghost=(\d+) gbank=(-?\d+)", read)]
    played = max((frames for frames, bank in playback if bank in (0, 1)), default=0)
    if code != 0:
        failures.append(f"saves: this build's ghost read run exited {code}")
    if not loads:
        failures.append(f"saves: this build never loaded a ghost off the {tag} pak")
    elif loads[0] != (TT_LEVEL, TT_VEHICLE, 0, nodes, character, ghost_time):
        failures.append(f"saves: this build loaded {loads[0]} from the {tag} pak, which "
                        f"saved ({TT_LEVEL}, {TT_VEHICLE}, 0, {nodes}, {character}, "
                        f"{ghost_time})")
    if played < MIN_GHOST_FRAMES:
        failures.append(f"saves: the {tag} ghost played for {played} frames from a "
                        f"player bank (need {MIN_GHOST_FRAMES})")
    if (read_save / PAK_IMAGE).read_bytes() != pak_before:
        failures.append(f"saves: reading the {tag} ghost rewrote {PAK_IMAGE}")
    marker = find_fatal(read) or find_fatal(decoded) or find_fatal(write)
    if marker:
        failures.append(f"saves: a run logged {marker!r}")
    if not failures:
        evidence.append(f"this build loads that ghost ({nodes} nodes, character "
                        f"{character}, time {ghost_time}) and plays it for {played} "
                        f"frames without rewriting the pak")
    return failures, evidence


def find_pal_rom(directory: Path) -> Path | None:
    """The PAL v80 image in ``directory``, identified by its header, so a macOS
    "._" sidecar or a renamed file can never be taken for it."""

    return find_revision_rom(directory, "pal.v80")


# --------------------------------------------------------------------------- #
#  Entry point
# --------------------------------------------------------------------------- #


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR,
                        help="this tree's build directory or executable")
    parser.add_argument("--reference-build", default=None,
                        help="a build of the previous published release, configured "
                             "like --build; without it the gate is SKIPPED")
    parser.add_argument("--reference-tag", default=DEFAULT_REFERENCE_TAG)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--roms", default=None,
                        help="ROM directory; its PAL v80 image adds the PAL routes")
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    parser.add_argument("--route", action="append", dest="routes",
                        help="run only these route names (iteration only)")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--allow-stale", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if not args.reference_build:
        print(skip_banner("no --reference-build was supplied"))
        return EXIT_SKIPPED
    tag = args.reference_tag
    current = Path(os.path.abspath(resolve_binary(args.build)))
    reference = Path(os.path.abspath(resolve_binary(args.reference_build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (current, reference, rom, SCRIPT):
        if not path.exists():
            print(f"check_previous_release_parity: FAIL\n  - missing {path}")
            return 1
    roms = [("US", rom)]
    if args.roms is not None:
        pal = find_pal_rom(Path(args.roms))
        if pal is None:
            print(f"check_previous_release_parity: FAIL\n  - no PAL v80 ROM under "
                  f"{args.roms}; the release run compares both regions")
            return 1
        roms.append(("PAL", pal))
    routes = [r for r in ROUTES if not args.routes or r.name in args.routes]
    if not routes:
        print(f"check_previous_release_parity: FAIL\n  - no route named {args.routes}")
        return 1

    problems, notes = reference_problems(reference, current, tag, args.allow_stale)
    if problems:
        print("check_previous_release_parity: FAIL")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    print(f"check_previous_release_parity: {tag} against this build, "
          f"{args.frames} ticks per route, {len(routes)} route(s) x "
          f"{'/'.join(region for region, _ in roms)}")
    print(f"  {tag:<10} {reference}")
    print(f"  {'this':<10} {current}")
    for note in notes:
        print(f"  note: {note}")

    arms: list[Arm] = []
    for region, region_rom in roms:
        for route in routes:
            for side, binary in (("reference", reference), ("current", current)):
                arms.append(Arm(f"{region}-{route.name}-{side}", binary, region_rom,
                                route, {}))
    control = Arm(f"US-{routes[0].name}-current-legacy-rng", current, rom, routes[0],
                  {"MDKR_RNGSEED": "legacy"})
    arms.append(control)

    failures: list[str] = []
    verdicts: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-previous-release-") as tmp:
        root = Path(tmp)
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            results = dict(zip(
                (arm.label for arm in arms),
                pool.map(lambda arm: run_route_arm(arm, root, args.frames,
                                                   args.timeout, args.verbose), arms)))
        compared = 0
        for region, _rom in roms:
            for route in routes:
                label = f"{region} {route.name} ({route.what})"
                ref = results[f"{region}-{route.name}-reference"]
                cur = results[f"{region}-{route.name}-current"]
                errors = [r for r in (ref, cur) if isinstance(r, str)]
                if errors:
                    failures += [f"{label}: {error}" for error in errors]
                    continue
                assert isinstance(ref, HashedRun) and isinstance(cur, HashedRun)
                found = (stream_problems(ref.rows, args.frames, f"{label} {tag}")
                         + stream_problems(cur.rows, args.frames, f"{label} this build")
                         + route_problems(label, route, ref.output, args.frames, tag)
                         + route_problems(label, route, cur.output, args.frames,
                                          "this build"))
                if found:
                    failures += found
                    continue
                index = first_divergence(ref.rows, cur.rows)
                start = race_start(cur.output, route)
                if index is not None:
                    report = divergence_report(
                        label, ref.rows, cur.rows, index, labels=(tag, "this build"),
                        kind=f"retail simulation ({tag} vs this build)",
                        parties="the two releases",
                        value_suspects="Both builds are deterministic run to run, so "
                                       "a changed decision or calculation between the "
                                       "releases is the candidate")
                    report.insert(1, f"    the race loaded at tick {start}; "
                                     f"{index - start} racing ticks were identical"
                                     if index >= start else
                                     f"    the runs part before the race loads at {start}")
                    report.append(
                        f"    classify it: git bisect run over {tag}..HEAD with this "
                        f"route (MDKR_LOAD_TRACK={route.load_track}), then decide "
                        "whether the change is intended and documented or a regression")
                    failures += report
                    continue
                compared += len(cur.rows)
                verdicts.append(f"  {label}: {len(cur.rows)} ticks identical, "
                                f"{args.frames - start} of them racing (from {start})")

        base = results[f"US-{routes[0].name}-current"]
        legacy = results[control.label]
        if isinstance(legacy, str):
            failures.append(f"positive control: {legacy}")
        elif isinstance(base, HashedRun):
            failures += stream_problems(legacy.rows, args.frames, "control legacy-RNG")
            problem = control_problem(base.rows, legacy.rows)
            if problem is not None:
                failures.append(problem)
            else:
                verdicts.append(f"  positive control: MDKR_RNGSEED=legacy diverges at tick "
                                f"{first_divergence(base.rows, legacy.rows)}")

        save_failures, evidence = saves_arm(reference, current, rom, root, tag,
                                            args.timeout, args.verbose)
        failures += save_failures
        verdicts += [f"  saves: {line}" for line in evidence]

    for line in verdicts:
        print(line)
    if failures:
        print("check_previous_release_parity: FAIL")
        for failure in failures:
            print(failure if failure.startswith("    ") else f"  - {failure}")
        return 1
    print(f"check_previous_release_parity: PASS -- {compared} authoritative ticks "
          f"identical to {tag} across {len(routes) * len(roms)} route run(s); the "
          f"legacy-RNG control diverges; {tag}'s record and ghost read back unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
