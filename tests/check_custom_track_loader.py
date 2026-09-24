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
            result = subprocess.run(
                [str(binary), '--headless-frames', '30', '--rom', str(rom)],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=90, check=False)
            corpus = re.findall(r'\[CUSTOM_TRACK_DECODE\] kind=(\d+) entries=(\d+) PASS', result.stdout)
            corpus_ok = (len(corpus) == 2 and {kind for kind, _ in corpus} == {'1', '3'}
                         and all(int(count) > 0 for _, count in corpus)) if enabled else not corpus
            maps = re.findall(r'\[CUSTOM_TRACK_MAP\] entries=(\d+) PASS', result.stdout)
            maps_ok = maps == [count for kind, count in corpus if kind == '1'] if enabled else not maps
            if result.returncode or result.stdout.count(marker) != int(enabled) or not corpus_ok or not maps_ok:
                print(f'FAIL custom track loader enabled={enabled} exit={result.returncode}')
                print('\n'.join(result.stdout.splitlines()[-30:]))
                return 1
            print(f'PASS custom track loader enabled={enabled}')
            for kind, count in corpus:
                print(f'PASS retail compression corpus kind={kind} entries={count}')
            if maps:
                print(f'PASS object-map structural corpus entries={maps[0]}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
