#!/usr/bin/env python3
"""Require the real interpolation fault to escape the diagnostic lens bound."""
import os
from pathlib import Path
import subprocess
import sys

env = dict(os.environ)
env.update(MDKR_INTERNAL_TEST_TOKEN='mdkr64-presentation-replay-v1',
           MDKR_TEST_ROTATION_LONG_ARC='1', MDKR_TRIG='table',
           MDKR_DEV_RUNTIME_TRIG='0')
result = subprocess.run([str(Path(sys.argv[1]).resolve())], env=env,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, timeout=30, check=False)
failures = [line for line in result.stdout.splitlines() if line.startswith('FAIL ')]
if (result.returncode != 1 or not failures or
        any(line != 'FAIL sampled corner within envelope' for line in failures) or
        any(marker in result.stdout for marker in
            ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'))):
    print(result.stdout)
    raise SystemExit('FAIL: long-arc control did not fail solely on containment')
print(f'PASS: long-arc control rejected at {len(failures)} corner checks')
