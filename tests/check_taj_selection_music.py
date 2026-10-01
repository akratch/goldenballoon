#!/usr/bin/env python3
"""Verify loaded Taj select sequence and non-silent MIDI dispatch, then retail return.

The Taj theme (sequence 27) belongs to Character Select only. Every arm also
checks the ordered menu/sequence timeline: sequence 27 may load only while
Character Select is the live menu, must never remain the resident tune of any
other menu (Caution, Game Select, File Select, title, Tracks), and must never
play with a disabled channel. The muted arms drive the real music slider
through the settings runtime and prove the deferred switch in both directions,
and that leaving Character Select hands back the retail tune even while muted.

This is not an audible/PCM oracle or proof of complete authored instrumentation.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from check_taj_character_select import LAYOUTS, STATE_TEXT, input_script, save_image
from harness_utils import DEFAULT_BUILD_DIR, fatal_re, resolve_binary

LOADED = re.compile(r"music_sequence_loaded: player=music sequence=(\d+) curTime=(\d+)")
MENU = re.compile(r"menu_init: menuId=(\d+) @frame~\d+")
VOLUME = re.compile(r"\[SETTINGS-TOGGLE\] tick=\d+ key=Audio\.MusicVolume "
                    r"value=(\d+) result=-?\d+ applied=1")
HOVER = re.compile(r"taj_hover: controller=0 index=-?\d+ taj=([01]) ")
SCENARIOS = ("confirm", "reselect", "title", "multiplayer", "custom", "custom-return",
             "game-select", "party-backout", "muted", "muted-exit")
# Player two reaches Taj with base-grid moves. The exit/return music policy the
# arm proves does not depend on which retail racers are unlocked.
BASE_ONLY = frozenset({"party-backout"})
TAJ_SEQUENCE = 27
MENU_CHARACTER_SELECT = 3
MENU_GAME_SELECT = 19
MENU_FILE_SELECT = 6
MENU_CAUTION = 28
ROOT = Path(__file__).resolve().parents[1]


def install_custom_fixture(root):
    """Use generated, license-clean art and the existing test-only validator."""
    sys.path.insert(0, str(ROOT / "tools"))
    import character_manifest_wizard as wizard
    from test_character_asset_probe import make_animated_glb, make_portrait_png

    source = root / "source"
    source.mkdir()
    model, portrait = source / "model.glb", source / "portrait.png"
    model.write_bytes(make_animated_glb())
    portrait.write_bytes(make_portrait_png(40))
    manifest, _ = wizard.build_manifest(
        model, "org.mdkr.music-proof", "Music Proof", "CC0-1.0",
        "Generated MDKR fixture", "https://example.invalid/music-proof",
        "diddy", ["car", "hovercraft", "plane"], portrait=portrait,
        minimap_rgb=[80, 180, 240])
    manifest_path, license_path = source / "manifest.json", source / "LICENSE.txt"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    license_path.write_text("CC0 1.0 Universal\n", encoding="utf-8")
    package, catalog = source / "music-proof.mdkrchar", root / "characters"
    commands = [
        [sys.executable, str(ROOT / "tools/character_asset_probe.py"), "pack",
         "--model", str(model), "--manifest", str(manifest_path),
         "--license", str(license_path), "--portrait", str(portrait),
         "--output", str(package)],
        [sys.executable, str(ROOT / "tests/run_character_manager_fixture.py"),
         "--directory", str(catalog), "install", str(package)],
    ]
    for command in commands:
        result = subprocess.run(command, cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=90)
        if result.returncode:
            raise RuntimeError(result.stdout[-5000:])
    return catalog


def verify(output, notes, expected=(27, 26)):
    if fatal_re("Assertion", "Validation Error").search(output):
        raise ValueError("native fatal diagnostic")
    loaded = [tuple(map(int, m.groups())) for m in LOADED.finditer(output)]
    taj = next((i for i, (seq, _) in enumerate(loaded) if seq == 27), None)
    if taj is None or tuple(seq for seq, _ in loaded[taj:]) != expected:
        raise ValueError(f"missing loaded Taj-to-retail transition: {loaded}")
    channels = []
    for i in range(taj, len(loaded)):
        start = loaded[i][1]
        end = loaded[i + 1][1] if i + 1 < len(loaded) else float("inf")
        active = {r["chan"] for r in notes if r.get("player") == "music"
                  and r.get("event") == "note_on" and start <= r["cur_time"] < end
                  and r["computed_volume"] > 0}
        if not active:
            raise ValueError(f"sequence {loaded[i][0]} has no nonzero-volume notes")
        channels.append(sorted(active))
    return channels


def timeline(output):
    """Menu entries, loaded sequences, applied music-slider edits and player
    one's Taj hover, in emission order. Every row is a flushed stderr line."""
    events = []
    for line in output.splitlines():
        for kind, pattern in (("menu", MENU), ("load", LOADED),
                              ("volume", VOLUME), ("hover", HOVER)):
            match = pattern.search(line)
            if match:
                events.append((kind, int(match.group(1))))
                break
    return events


def scope_violations(events):
    """Sequence 27 may load only on Character Select and must not remain the
    resident tune of any other menu."""
    problems, menu, resident = [], None, None
    for kind, value in events:
        if kind == "menu":
            if menu not in (None, MENU_CHARACTER_SELECT) and resident == TAJ_SEQUENCE:
                problems.append(f"Taj theme was still resident when menu {menu} ended")
            menu = value
        elif kind == "load":
            if value == TAJ_SEQUENCE and menu != MENU_CHARACTER_SELECT:
                problems.append(f"Taj theme loaded in menu {menu}")
            resident = value
    if menu not in (None, MENU_CHARACTER_SELECT) and resident == TAJ_SEQUENCE:
        problems.append(f"Taj theme was still resident at the end of menu {menu}")
    return problems


def disabled_taj_channels(output, notes):
    """A Taj theme interval may never reject a note on a disabled channel."""
    loaded = [tuple(map(int, m.groups())) for m in LOADED.finditer(output)]
    disabled = set()
    for i, (seq, start) in enumerate(loaded):
        if seq != TAJ_SEQUENCE:
            continue
        end = loaded[i + 1][1] if i + 1 < len(loaded) else float("inf")
        disabled |= {r["chan"] for r in notes if r.get("player") == "music"
                     and r.get("event") == "note_reject"
                     and r.get("reason") == "channel-disabled"
                     and start <= r["cur_time"] < end}
    return sorted(disabled)


def menu_route(events):
    return [value for kind, value in events if kind == "menu"]


def require_route(events, expected):
    """``expected`` must appear in order, contiguously, after the first entry
    to Character Select."""
    route = menu_route(events)
    start = route.index(MENU_CHARACTER_SELECT) if MENU_CHARACTER_SELECT in route else len(route)
    if route[start:start + len(expected)] != list(expected):
        raise ValueError(f"menu route {route} does not contain {list(expected)}")


def segment(events, kind, value, occurrence):
    """Index of the ``occurrence``-th (0-based) ``(kind, value)`` event."""
    hits = [i for i, event in enumerate(events) if event == (kind, value)]
    if len(hits) <= occurrence:
        raise ValueError(f"missing {kind}={value} #{occurrence + 1} in {events}")
    return hits[occurrence]


def deferred_switch(events, muted_at, raised_at, hover, sequence):
    """While the slider is at zero the cursor moves (``hover``) but nothing
    loads; the first load after the slider is raised is ``sequence``."""
    window = events[muted_at + 1:raised_at]
    if ("hover", hover) not in window:
        raise ValueError(f"no taj={hover} hover while muted: {window}")
    if any(kind == "load" for kind, _ in window):
        raise ValueError(f"a sequence loaded while the slider was at zero: {window}")
    after = [value for kind, value in events[raised_at + 1:] if kind == "load"]
    if not after or after[0] != sequence:
        raise ValueError(f"raising the slider did not load {sequence}: {after}")


def verify_muted(events):
    """Mute retry in both directions: hover onto Taj, then off him, each while
    the slider is at zero; raising it performs the deferred switch."""
    volumes = [value for kind, value in events if kind == "volume"]
    if volumes != [0, 100, 0, 100]:
        raise ValueError(f"music slider edits were not all applied: {volumes}")
    deferred_switch(events, segment(events, "volume", 0, 0),
                    segment(events, "volume", 100, 0), 1, TAJ_SEQUENCE)
    deferred_switch(events, segment(events, "volume", 0, 1),
                    segment(events, "volume", 100, 1), 0, 26)


def verify_muted_exit(events):
    """Leaving Character Select while muted still replaces the Taj theme, so
    raising the slider in Game Select cannot resurface it."""
    volumes = [value for kind, value in events if kind == "volume"]
    if volumes != [0, 100]:
        raise ValueError(f"music slider edits were not all applied: {volumes}")
    muted_at = segment(events, "volume", 0, 0)
    raised_at = segment(events, "volume", 100, 0)
    window = events[muted_at + 1:raised_at]
    if not any(kind == "menu" and value != MENU_CHARACTER_SELECT for kind, value in window):
        raise ValueError(f"Character Select was not left while muted: {window}")
    if ("load", 26) not in window:
        raise ValueError(f"the retail tune was not restored while muted: {window}")


def run_scenario(build, rom, scenario, layout):
    with tempfile.TemporaryDirectory(prefix="mdkr-taj-selection-music-") as tmp:
        root = Path(tmp)
        save = root / "save"
        save.mkdir()
        (save / "eeprom.bin").write_bytes(save_image(layout))
        (save / "taj_mod_state.ini").write_text(STATE_TEXT)
        route = root / "input.txt"
        script = input_script(layout)
        expected = (27, 26)
        frames = 2600
        extra_env, extra_args, menus = {}, [], ()
        if scenario == "game-select":
            # Confirm Taj, dismiss Caution, enter Adventure's File Select,
            # then back out twice to Character Select (Taj still highlighted)
            # and finally move off him.
            script += "2500 A 4\n2800 A 4\n3100 B 4\n3400 B 4\n3800 RIGHT 4\n"
            frames = 4200
            expected = (27, 26, 27, 26)
            menus = (MENU_CHARACTER_SELECT, MENU_CAUTION, MENU_GAME_SELECT,
                     MENU_FILE_SELECT, MENU_GAME_SELECT, MENU_CHARACTER_SELECT)
        elif scenario == "muted-exit":
            # Taj theme playing, slider to zero, confirm into Game Select,
            # then raise the slider there.
            script += "2500 A 4\n"
            frames = 3100
            extra_env["MDKR_TEST_SETTINGS_TOGGLE"] = (
                "Audio.MusicVolume=0@2050,Audio.MusicVolume=100@2700")
            menus = (MENU_CHARACTER_SELECT, MENU_CAUTION, MENU_GAME_SELECT)
        elif scenario != "confirm":
            script = "\n".join(line for line in script.splitlines()
                               if not line.startswith(("2100 ", "2180 "))) + "\n"
            frames = 3400
            if scenario == "party-backout":
                # Adventure Party: P1 leaves Taj for Pipsy, P2 joins and takes
                # Taj (owning the music), both confirm into Game Select, back
                # out. On return P1 owns the music and is not on Taj.
                script += ("2000 A 4 P2\n2100 RIGHT 4 P1\n2200 DOWN 4 P2\n"
                           "2260 RIGHT 4 P2\n2320 RIGHT 4 P2\n2450 A 4 P1\n"
                           "2500 A 4 P2\n2600 A 4 P1\n2900 B 4 P1\n3400 LEFT 4 P1\n")
                frames = 3700
                expected = (27, 26, 27, 26)
                extra_args = ["--video-set", "Enhancements.AdventureParty=1"]
                menus = (MENU_CHARACTER_SELECT, MENU_GAME_SELECT, MENU_CHARACTER_SELECT)
            elif scenario == "muted":
                script += "2600 RIGHT 4\n"
                frames = 3100
                extra_env["MDKR_TEST_SETTINGS_TOGGLE"] = (
                    "Audio.MusicVolume=0@1500,Audio.MusicVolume=100@2300,"
                    "Audio.MusicVolume=0@2450,Audio.MusicVolume=100@2800")
            elif scenario in ("custom", "custom-return"):
                script += "2200 R 4 P1\n2500 A 4 P1\n"
                if scenario == "custom-return":
                    # Unconfirm the custom Diddy donor, then replay the same
                    # layout-specific Diddy-to-Taj navigation. Changing retail
                    # slots must clear custom identity before Taj owns music.
                    script += "2700 B 4 P1\n"
                    for line in input_script(layout).splitlines():
                        fields = line.split()
                        if len(fields) == 3 and fields[1] in ("LEFT", "RIGHT", "UP", "DOWN"):
                            script += f"{int(fields[0]) + 1600} {fields[1]} 4 P1\n"
                    script += "3900 RIGHT 4 P1\n"
                    frames = 4300
                    expected = (27, 26, 27, 26)
            elif scenario == "multiplayer":
                script += "2200 A 4 P2\n2500 B 4 P2\n2800 RIGHT 4 P1\n"
                expected = (27, 26, 27, 26)
            elif scenario == "reselect":
                script += "2200 RIGHT 4\n2500 LEFT 4\n2800 RIGHT 4\n"
                expected = (27, 26, 27, 26)
            else:
                script += "2300 B 4\n"
                expected = (27, 7)
        route.write_text(script)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("MDKR", "GE007_"))}
        env.update(MDKR_AUDIO="0", MDKR64_HIDDEN="1", MDKR_RENDERER="webgpu",
                   MDKR_TRACE="1", MDKR_APP_PREFS_DIR=str(root), MDKR_SAVE_DIR=str(save),
                   MDKR_VIDEO_CONFIG_PATH=str(root / "video.ini"),
                   MDKR_MUSIC_MIDI_TRACE_JSONL=str(root / "notes.jsonl"))
        env.update(extra_env)
        if scenario in ("custom", "custom-return"):
            env["MDKR_CUSTOM_CHARACTER_DIRECTORY"] = str(install_custom_fixture(root))
        result = subprocess.run(
            [str(Path(resolve_binary(build)).resolve()), "--rom", str(rom.resolve()),
             "--headless-frames", str(frames), "--window-size", "320x240", "--input-script", str(route),
             *extra_args],
            cwd=root, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        if result.returncode:
            raise RuntimeError(result.stdout[-5000:])
        notes = [json.loads(line) for line in (root / "notes.jsonl").read_text().splitlines()]
        channels = verify(result.stdout, notes, expected)
        events = timeline(result.stdout)
        problems = scope_violations(events)
        if problems:
            raise ValueError(f"Taj theme escaped Character Select: {problems}; "
                             f"route {menu_route(events)}")
        disabled = disabled_taj_channels(result.stdout, notes)
        if disabled:
            raise ValueError(f"Taj theme played with disabled channels {disabled}")
        if menus:
            require_route(events, menus)
        if scenario == "muted":
            verify_muted(events)
        elif scenario == "muted-exit":
            verify_muted_exit(events)
        # The timeline checks must reject a Taj theme that outlives its menu:
        # one loaded in Game Select, and one carried unchanged through Caution.
        for leak in ([("menu", MENU_GAME_SELECT), ("load", TAJ_SEQUENCE)],
                     [("menu", MENU_CHARACTER_SELECT), ("load", TAJ_SEQUENCE),
                      ("menu", MENU_CAUTION), ("menu", MENU_GAME_SELECT),
                      ("load", 26)]):
            if not scope_violations(events + leak):
                raise RuntimeError(f"a leaked Taj theme was accepted: {leak}")
        if scenario in ("custom", "custom-return") and not re.search(
                r"custom_roster_select: controller=0 race_player=0 "
                r"package=org\.mdkr\.music-proof donor=9 catalog=\d+", result.stdout):
            raise ValueError("missing custom character identity witness")
        if scenario == "custom-return" and "custom_roster_select: controller=0 source=retail cleared=1" not in result.stdout:
            raise ValueError("custom identity did not clear on retail navigation")
        if scenario == "multiplayer":
            owners = [tuple(map(int, row)) for row in re.findall(
                r"charselect_music: owner=(\d+) sequence=(\d+)", result.stdout)]
            if owners != [(0, 27), (1, 26), (0, 27), (0, 26)]:
                raise ValueError(f"incorrect multiplayer music ownership: {owners}")
        for output, rows in ((result.stdout, []),
                             (LOADED.sub("", result.stdout), notes),
                             (result.stdout, [dict(r, computed_volume=0) for r in notes])):
            try:
                verify(output, rows, expected)
            except ValueError:
                continue
            raise RuntimeError("invalid playback evidence was accepted")
        print(f"check_taj_selection_music: PASS — {layout.name}/{scenario} loaded {expected}, active channels {channels}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--scenario", choices=("all",) + SCENARIOS, default="all")
    parser.add_argument("--layout", choices=("all",) + tuple(x.name for x in LAYOUTS), default="all")
    args = parser.parse_args()
    for layout in LAYOUTS:
        if args.layout not in ("all", layout.name):
            continue
        for scenario in SCENARIOS if args.scenario == "all" else (args.scenario,):
            if scenario in BASE_ONLY and layout.name != "base":
                continue
            run_scenario(args.build, args.rom, scenario, layout)


if __name__ == "__main__":
    main()
