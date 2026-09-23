#!/usr/bin/env python3
"""Verify loaded Taj select sequence and non-silent MIDI dispatch, then retail return.

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
SCENARIOS = ("confirm", "reselect", "title", "multiplayer", "custom", "custom-return")
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
        if scenario != "confirm":
            script = "\n".join(line for line in script.splitlines()
                               if not line.startswith(("2100 ", "2180 "))) + "\n"
            frames = 3400
            if scenario in ("custom", "custom-return"):
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
        if scenario in ("custom", "custom-return"):
            env["MDKR_CUSTOM_CHARACTER_DIRECTORY"] = str(install_custom_fixture(root))
        result = subprocess.run(
            [str(Path(resolve_binary(build)).resolve()), "--rom", str(rom.resolve()),
             "--headless-frames", str(frames), "--window-size", "320x240", "--input-script", str(route)],
            cwd=root, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        if result.returncode:
            raise RuntimeError(result.stdout[-5000:])
        notes = [json.loads(line) for line in (root / "notes.jsonl").read_text().splitlines()]
        channels = verify(result.stdout, notes, expected)
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
            run_scenario(args.build, args.rom, scenario, layout)


if __name__ == "__main__":
    main()
