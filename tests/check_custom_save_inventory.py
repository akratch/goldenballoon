#!/usr/bin/env python3
"""Real retail codecs with synthetic appended metadata; no authored-course claim."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from harness_utils import ASSERT_MARKERS, fatal_re, resolve_binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='build-rel')
    parser.add_argument('--rom', required=True)
    args = parser.parse_args()
    marker = '[CUSTOM_SAVE] PASS appended=8 retail-bytes=identical guards=intact'
    for enabled in (False, True):
        with tempfile.TemporaryDirectory(prefix='mdkr_custom_save_') as directory:
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(('MDKR', 'GE007_'))}
            env.update(MDKR_AUDIO='0', MDKR_SAVE_DIR=str(Path(directory) / 'save'),
                       MDKR_VIDEO_CONFIG_PATH=str(Path(directory) / 'missing.ini'))
            if enabled:
                env['MDKR_CUSTOM_SAVE_SELFTEST'] = '1'
            result = subprocess.run(
                [str(Path(resolve_binary(args.build)).resolve()), '--headless-frames', '30',
                 '--rom', str(Path(args.rom).resolve())], env=env,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                timeout=90, check=False)
            if (result.returncode or result.stdout.count(marker) != int(enabled)
                    or fatal_re(*ASSERT_MARKERS).search(result.stdout)
                    or 'runtime error:' in result.stdout):
                print(f'FAIL custom save inventory enabled={enabled} exit={result.returncode}')
                print('\n'.join(result.stdout.splitlines()[-35:]))
                return 1
            print(f'PASS custom save inventory enabled={enabled}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
