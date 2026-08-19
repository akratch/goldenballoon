#!/usr/bin/env python3
"""The nine `nav_*` menu fixtures, asserted instead of merely documented.

Why this exists
---------------
`tests/input_scripts/nav_*.txt` are the menu-navigation routes.  Their expected
terminal state has always lived in a table in `tests/README.md` -- a human was
supposed to run each one and `grep` for a `menu_init:` line.  Five of the nine
had no automated consumer at all (`nav_to_options`, `nav_to_audio_options`,
`nav_to_save_options`, `nav_to_game_select`, `nav_to_time_trial_race`), so a
menu route could break and nothing would say so.

The "closedloop" wave's class sweep is what surfaced that: it enumerated every
`tests/input_scripts/*.txt` against its consumers in order to find the fixtures
that were calibrated against a racing line, and found instead that a third of the
directory was calibrated against nothing at all.  The routes were fine; the
coverage was imaginary.  This runs them.

Why these stay OPEN-LOOP, deliberately
--------------------------------------
Everything else in that sweep either drives with DKR's own AI or was converted to.
These nine are the stated exception.  A menu route asserts "this sequence of
button edges reaches menuId N": there is no racer, no physics and no `rand_range`
draw between the input and the assertion, so there is no racing line to
desynchronise.  Confirmed rather than assumed -- every route below reaches the
same terminal menu with the ROM-faithful RNG seed / arctan table / table-based
trig as with the superseded ones, since the frontend does not consume them.

The one thing that DOES move them is timing: menu input is edge-detected and the
frames are hand-placed with generous gaps, so a change to the frontend's own
update rate would break them.  That is exactly what they should catch, and
`tests/check_charselect_motion.py` measures the animation rate itself.

    MDKR_AUDIO=0 python3 tests/check_nav_fixtures.py -v      # ~3 min

Always muted + headless, per tests/README.md.  Exit 0 = pass.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

# script -> (frames, assertion).  Frames and terminal states are the ones
# tests/README.md has always documented; this file is now the authority and the
# README points here.
#
# The last two do not end in a menu: they load a level, so the assertion is the
# level_load line instead.  `nav_to_time_trial_race` is also the navigation
# `check_race_drive.py` drives with the autopilot, so it is covered twice over --
# here for "the menus still get there", there for "the race still works".
ROUTES = [
    ("nav_to_options",              1700, "menu", 12),
    ("nav_to_audio_options",        1900, "menu", 13),
    ("nav_to_save_options",         1950, "menu", 14),
    ("nav_to_magic_codes",          2050, "menu", 10),
    ("nav_to_character_select",     1600, "menu", 3),
    ("nav_to_game_select",          2000, "menu", 19),
    ("nav_to_file_select_adventure", 2100, "menu", 6),
    ("nav_to_track_select",         2300, "menu", 15),
    ("nav_to_time_trial_race",      2900, "level", 5),
]

MENU_RE = re.compile(r"menu_init: menuId=(\d+)")
LEVEL_RE = re.compile(r"level_load: levelId=(\d+) numPlayers=(-?\d+)")
MAGIC_CODE_RE = re.compile(r"magic_code_submit: accepted=(\d+) id=(-?\d+)")
MAGIC_RESTORE_RE = re.compile(
    r"magic_codes_restore: unlocked=([0-9A-F]{8}) active=([0-9A-F]{8})")
BAD_RE = re.compile(r"\[FATAL\]|\[CRASH\]|AddressSanitizer|Assertion")


def run_magic_code_submission(binary: str, rom: str, scripts: Path,
                              verbose: bool) -> list[str]:
    """Exercise valid and invalid submissions through the retail menu UI."""
    name = "magic_codes_accept_reject"
    script = str((scripts / (name + ".txt")).resolve())
    if not os.path.exists(script):
        return ["%s: missing %s" % (name, script)]

    with tempfile.TemporaryDirectory(prefix="mdkr-magic-code-fixture-") as run_root:
        save = os.path.join(run_root, "save")
        os.mkdir(save)
        # Isolate the video config with the save (see check_door_blocks.py).
        env = dict(os.environ, MDKR_AUDIO="0", MDKR_TRACE="1", MDKR_SAVE_DIR=save,
                   MDKR_VIDEO_CONFIG_PATH=os.path.join(save, "video.ini"))
        cmd = [binary, "--headless-frames", "3650", "--input-script", script,
               "--rom", rom]
        if verbose:
            print("  $ " + " ".join(cmd))
        p = subprocess.run(cmd, cwd=run_root, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=900)
        out = p.stdout.decode("utf-8", "replace")
        failures = []
        if p.returncode != 0:
            failures.append("%s: exit code %d" % (name, p.returncode))
        bad = BAD_RE.findall(out)
        if bad:
            failures.append("%s: %s in output" % (name, bad[0]))
        outcomes = [(int(m.group(1)), int(m.group(2)))
                    for m in MAGIC_CODE_RE.finditer(out)]
        expected = [(1, 4), (0, -1)]
        if outcomes != expected:
            failures.append("%s: outcomes %s, want %s" %
                            (name, outcomes, expected))

        sidecar = os.path.join(save, "magic_codes_state.ini")
        if not os.path.isfile(sidecar):
            failures.append("%s: accepted code created no sidecar" % name)
        restart_script = str((scripts / "nav_to_magic_codes.txt").resolve())
        restart_cmd = [binary, "--headless-frames", "2050", "--input-script",
                       restart_script, "--rom", rom]
        restart = subprocess.run(
            restart_cmd, cwd=run_root, env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=900)
        restart_out = restart.stdout.decode("utf-8", "replace")
        restored = [(int(m.group(1), 16), int(m.group(2), 16))
                    for m in MAGIC_RESTORE_RE.finditer(restart_out)]
        if restart.returncode != 0:
            failures.append("%s restart: exit code %d" %
                            (name, restart.returncode))
        elif not restored or restored[0] != (1 << 4, 1 << 4):
            failures.append("%s restart: restored %s, want ARNOLD on+unlocked" %
                            (name, restored))
        elif verbose:
            print("    %-30s outcomes=%s restart=%s" %
                  (name, outcomes, restored[0]))
    return failures


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--only", default=None, help="run just this route")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    scripts = Path(__file__).resolve().parent / "input_scripts"
    if not os.path.exists(binary) or not os.path.exists(rom):
        print("FAIL: missing %s or %s" % (binary, args.rom), file=sys.stderr)
        return 1

    failures = []
    for name, frames, kind, want in ROUTES:
        if args.only and args.only != name:
            continue
        script = str((scripts / (name + ".txt")).resolve())
        if not os.path.exists(script):
            failures.append("%s: missing %s" % (name, script))
            continue
        # A recorded time or a saved file changes the frontend route. Every arm
        # therefore gets a private cwd and MDKR_SAVE_DIR; the old harness
        # removed repository save/eeprom.bin before and after each route, which
        # could destroy a developer/player campaign merely by running a test.
        with tempfile.TemporaryDirectory(prefix="mdkr-nav-fixture-") as run_root:
            save = os.path.join(run_root, "save")
            os.mkdir(save)
            env = dict(
                os.environ,
                MDKR_AUDIO="0",
                MDKR_TRACE="1",
                MDKR_SAVE_DIR=save,
                MDKR_VIDEO_CONFIG_PATH=os.path.join(save, "video.ini"),
            )
            cmd = [binary, "--headless-frames", str(frames),
                   "--input-script", script, "--rom", rom]
            if args.verbose:
                print("  $ " + " ".join(cmd))
            p = subprocess.run(cmd, cwd=run_root, env=env,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=900)
        out = p.stdout.decode("utf-8", "replace")
        if p.returncode != 0:
            failures.append("%s: exit code %d" % (name, p.returncode))
        bad = BAD_RE.findall(out)
        if bad:
            failures.append("%s: %s in output" % (name, bad[0]))
        if kind == "menu":
            seen = [int(m.group(1)) for m in MENU_RE.finditer(out)]
            got = seen[-1] if seen else None
            if got != want:
                failures.append("%s: ended on menuId=%s, want %d (menu route: %s)"
                                % (name, got, want, seen))
            elif args.verbose:
                print("    %-30s menuId=%d  (route %s)" % (name, got, seen))
        else:
            loads = [(int(m.group(1)), int(m.group(2))) for m in LEVEL_RE.finditer(out)]
            races = [lv for lv, np in loads if np >= 0]
            if want not in races:
                failures.append("%s: never loaded level %d as a race (loads: %s)"
                                % (name, want, loads))
            elif args.verbose:
                print("    %-30s level_load levelId=%d  (race loads %s)"
                      % (name, want, races))
    # This is an additional assertion inside the existing nav gate, not a tenth
    # route: it proves the reported code-entry bug at the runtime boundary while
    # preserving the long-standing nine-route manifest and summary.
    if args.only is None or args.only == "nav_to_magic_codes":
        failures.extend(run_magic_code_submission(binary, rom, scripts, args.verbose))
    if failures:
        for f in failures:
            print("  - %s" % f, file=sys.stderr)
        print("check_nav_fixtures: FAIL")
        return 1
    print("check_nav_fixtures: PASS  (%d menu routes reach their documented "
          "terminal state)" % len(ROUTES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
