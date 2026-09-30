#!/usr/bin/env python3
"""ROM-private native loader integration, not custom-course gameplay proof."""
import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default=DEFAULT_BUILD_DIR)
    parser.add_argument('--rom', required=True)
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    marker = '[CUSTOM_TRACK_LOADER] PASS sections=5 detached=1'
    for enabled in (False, True):
        with tempfile.TemporaryDirectory(prefix='mdkr_track_loader_') as temp:
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(('MDKR', 'GE007_'))}
            env.update(MDKR_AUDIO='0', MDKR_SAVE_DIR=str(Path(temp) / 'save'),
                       MDKR_VIDEO_CONFIG_PATH=str(Path(temp) / 'missing.ini'))
            if enabled:
                env['MDKR_CUSTOM_TRACK_LOADER_SELFTEST'] = '1'
                env['MDKR_LEVEL_NAMES_SELFTEST'] = '1'
            result = subprocess.run(
                [str(binary), '--headless-frames', '30', '--rom', str(rom)],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=90, check=False)
            # A boot-time corpus abort precedes the AI fixture. Report the
            # actual failure rather than misdiagnosing its absent later marker.
            if result.returncode:
                print(f'FAIL native loader process enabled={enabled} exit={result.returncode}')
                print('\n'.join(line for line in result.stdout.splitlines()
                                if '[CUSTOM_TRACK' in line or '[FATAL]' in line or '[CRASHSCREEN]' in line))
                print('\n'.join(result.stdout.splitlines()[-10:]))
                return 1
            ai_marker = '[CUSTOM_TRACK_AI_NODE] repeated_id_objects=129 stale_link=1 invalid_roots=6 cyclic_search=1 wide_cost=1 PASS'
            if result.stdout.count(ai_marker) != int(enabled):
                print('FAIL engine AI-node rebuild/traversal fixture')
                print('\n'.join(result.stdout.splitlines()[-30:]))
                return 1
            corpus = re.findall(r'\[CUSTOM_TRACK_DECODE\] kind=(\d+) entries=(\d+) PASS', result.stdout)
            corpus_ok = (len(corpus) == 2 and {kind for kind, _ in corpus} == {'1', '3'}
                         and all(int(count) > 0 for _, count in corpus)) if enabled else not corpus
            maps = re.findall(r'\[CUSTOM_TRACK_MAP\] entries=(\d+) PASS', result.stdout)
            maps_ok = maps == [count for kind, count in corpus if kind == '1'] if enabled else not maps
            behaviors = re.findall(r'\[CUSTOM_TRACK_BEHAVIOR_CORPUS\] maps=(\d+) records=(\d+) PASS', result.stdout)
            behaviors_ok = (len(behaviors) == 1 and [behaviors[0][0]] == maps and
                            int(behaviors[0][1]) > 0) if enabled else not behaviors
            short_blocks = re.findall(
                r'\[CUSTOM_TRACK_BEHAVIOR_SHORT\] BEGIN\n(.*?)'
                r'\[CUSTOM_TRACK_BEHAVIOR_SHORT\] END', result.stdout, re.S)
            short_ok = not enabled and '[CUSTOM_TRACK_BEHAVIOR_SHORT]' not in result.stdout
            if enabled and len(short_blocks) == 1:
                failures = short_blocks[0].splitlines()
                matches = [re.fullmatch(
                    r'\[FATAL\] level-object id (\d+) behavior (\d+) at map byte (\d+) '
                    r'has 8 bytes; initializer requires at least (\d+)', line)
                    for line in failures]
                short_ok = (len(matches) == 2 and all(matches) and
                            matches[0].group(1, 2) == matches[1].group(1, 2) and
                            int(matches[0][3]) == 0 and int(matches[1][3]) > 8 and
                            int(matches[0][4]) > 8 and matches[0][4] == matches[1][4] and
                            result.stdout.count('[CUSTOM_TRACK_BEHAVIOR_SHORT]') == 2)
            behaviors_ok = behaviors_ok and short_ok
            inventories = re.findall(
                r'\[CUSTOM_TRACK_AI_INVENTORY\] nodes=(\d+) duplicate_and_capacity_controls=1 PASS',
                result.stdout)
            behaviors_ok = behaviors_ok and (
                len(inventories) == 1 and int(inventories[0]) > 0 if enabled else not inventories)
            if not behaviors_ok:
                print('FAIL initialized object-map behavior corpus')
                print('\n'.join(line for line in result.stdout.splitlines()
                                if '[CUSTOM_TRACK' in line or '[FATAL]' in line))
                return 1
            if enabled:
                print(f'PASS initialized behavior corpus maps={behaviors[0][0]} records={behaviors[0][1]}')
                print(f'PASS AI inventory corpus nodes={inventories[0]} duplicate/capacity controls')
            names = re.findall(r'\[LEVEL_NAMES\] PASS entries=(\d+) languages=4', result.stdout)
            names_ok = len(names) == 1 and int(names[0]) > 0 if enabled else not names
            scalars = re.findall(r'\[CUSTOM_TRACK_SCALAR_CORPUS\] headers=(\d+) PASS', result.stdout)
            scalars_ok = scalars == names if enabled else not scalars
            if not scalars_ok:
                print('FAIL scalar-header corpus coverage')
                return 1
            repacked = re.findall(r'\[CUSTOM_TRACK_REPACK\] entries=(\d+) native_staging=1 PASS', result.stdout)
            repack_ok = repacked == [count for kind, count in corpus if kind == '3'] if enabled else not repacked
            budgets = re.findall(r'\[CUSTOM_TRACK_COLLISION_BUDGET\] maximum=(\d+) arena=(\d+)', result.stdout)
            budget_ok = (len(budgets) == 1 and all(int(n) > 0 for n in budgets[0])) if enabled else not budgets
            textures = re.findall(r'\[CUSTOM_TRACK_TEXTURE_CORPUS\] accepted=(\d+) palette=(\d+) refused=(\d+)', result.stdout)
            textures_ok = (len(textures) == 1 and int(textures[0][0]) > 0 and
                           int(textures[0][2]) == 0) if enabled else not textures
            lighting = re.findall(r'\[CUSTOM_TRACK_LIGHT_CORPUS\] colour_refs=(\d+) pulse_refs=(\d+) PASS', result.stdout)
            lighting_ok = (len(lighting) == 1 and all(int(n) > 0 for n in lighting[0])) if enabled else not lighting
            sky = re.findall(r'\[CUSTOM_TRACK_SKY_CORPUS\] refs=(\d+) PASS', result.stdout)
            sky_ok = len(sky) == 1 and int(sky[0]) > 0 if enabled else not sky
            waves = re.findall(r'\[CUSTOM_TRACK_WAVE_CORPUS\] models=(\d+) segments=(\d+) PASS', result.stdout)
            waves_ok = (len(waves) == 1 and 0 < int(waves[0][0]) <= int(waves[0][1])) if enabled else not waves
            links = re.findall(r'\[CUSTOM_TRACK_LIGHT_LINK_CORPUS\] pairs=(\d+) PASS', result.stdout)
            links_ok = len(links) == 1 and int(links[0]) > 0 if enabled else not links
            pulse_textures = re.findall(r'\[CUSTOM_TRACK_PULSE_TEXTURE_CORPUS\] selected=(\d+) PASS', result.stdout)
            pulse_ok = len(pulse_textures) == 1 and int(pulse_textures[0]) > 0 if enabled else not pulse_textures
            if not pulse_ok:
                print('FAIL selected pulsing texture corpus')
                print('\n'.join(line for line in result.stdout.splitlines()
                                if '[CUSTOM_TRACK' in line or '[FATAL]' in line))
                return 1
            if enabled:
                print(f'PASS selected pulsing textures={pulse_textures[0]}')
            if not links_ok:
                print('FAIL model/header lighting relationship census')
                print('\n'.join(line for line in result.stdout.splitlines()
                                if '[CUSTOM_TRACK' in line))
                print('\n'.join(result.stdout.splitlines()[-30:]))
                return 1
            if enabled:
                print(f'PASS model/header lighting pairs={links[0]}')
            if result.returncode or result.stdout.count(marker) != int(enabled) or not corpus_ok or not maps_ok or not names_ok or not repack_ok or not budget_ok or not textures_ok or not lighting_ok or not sky_ok or not waves_ok:
                print(f'FAIL custom track loader enabled={enabled} exit={result.returncode}')
                print('\n'.join(result.stdout.splitlines()[-30:]))
                return 1
            print(f'PASS custom track loader enabled={enabled}')
            for kind, count in corpus:
                print(f'PASS retail compression corpus kind={kind} entries={count}')
            if maps:
                print(f'PASS object-map structural corpus entries={maps[0]}')
            if repacked:
                print(f'PASS model repack/native staging corpus entries={repacked[0]}')
            if budgets:
                maximum, arena = map(int, budgets[0])
                print(f'INFO conservative collision extent maximum={maximum} arena={arena} fits={maximum <= arena}')
            if textures:
                print(f'PASS texture corpus accepted={textures[0][0]} palette_required={textures[0][1]} refused=0')
            if names:
                print(f'PASS localized name corpus entries={names[0]} languages=4')
            if lighting:
                print(f'PASS lighting corpus colour_refs={lighting[0][0]} pulse_refs={lighting[0][1]}')
            if sky:
                print(f'PASS special sky texture corpus refs={sky[0]}')
            if waves:
                print(f'PASS authored wave census models={waves[0][0]} segments={waves[0][1]}')
    for label, variable, value, refusal in (
        ('short level header', 'MDKR_LEVEL_HEADER_SHORT_SELFTEST', '1',
         '[FATAL] level header span invalid: index=0 '),
        ('unterminated level name', 'MDKR_LEVEL_NAMES_SELFTEST', 'unterminated',
         '[FATAL] unterminated level name: index=0 '),
    ):
        with tempfile.TemporaryDirectory(prefix='mdkr_metadata_negative_') as temp:
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(('MDKR', 'GE007_'))}
            env.update(MDKR_AUDIO='0', MDKR_SAVE_DIR=str(Path(temp) / 'save'),
                       MDKR_VIDEO_CONFIG_PATH=str(Path(temp) / 'missing.ini'))
            env[variable] = value
            result = subprocess.run(
                [str(binary), '--headless-frames', '30', '--rom', str(rom)], env=env,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                timeout=90, check=False)
            if result.returncode == 0 or refusal not in result.stdout:
                print(f'FAIL {label} was not rejected by its own guard')
                print('\n'.join(result.stdout.splitlines()[-30:]))
                return 1
            print(f'PASS {label} rejected by its own guard')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
