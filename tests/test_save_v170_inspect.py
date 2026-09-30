#!/usr/bin/env python3
"""This tree's mdkr-save reports a 1.7.0 save exactly as 1.7.0's did.

tests/fixtures/save_v170/inspect-1.7.0.json is what the published 1.7.0
`mdkr-save inspect` printed for the EEPROM in that directory. A player who
backed up a save with 1.7.0 reads it back through this command, so every fact
1.7.0 reported -- block states, the file name, each course's state, balloons,
world flags, amulets -- must come back unchanged, for the raw image and for the
.mdkr-save container 1.7.0 exported from it. A newer tool may report more; it
may not report differently.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness_utils import read_hex_fixture  # noqa: E402

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "save_v170"


def differences(expected: object, actual: object, where: str = "$") -> list[str]:
    """Where ``actual`` fails to carry ``expected``; extra keys are allowed."""

    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return [f"{where}: expected an object, got {actual!r}"]
        found: list[str] = []
        for key, value in expected.items():
            if key not in actual:
                found.append(f"{where}.{key}: missing")
            else:
                found += differences(value, actual[key], f"{where}.{key}")
        return found
    if isinstance(expected, list):
        if not isinstance(actual, list) or len(actual) != len(expected):
            return [f"{where}: expected {expected!r}, got {actual!r}"]
        found = []
        for index, (want, got) in enumerate(zip(expected, actual)):
            found += differences(want, got, f"{where}[{index}]")
        return found
    return [] if expected == actual else [f"{where}: 1.7.0 said {expected!r}, now {actual!r}"]


def inspect(cli: Path, path: Path) -> object:
    process = subprocess.run([str(cli), "inspect", str(path)], capture_output=True,
                             text=True, check=False)
    if process.returncode != 0:
        raise AssertionError(f"inspect {path} failed ({process.returncode}): "
                             f"{process.stdout}{process.stderr}")
    return json.loads(process.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    arguments = parser.parse_args()
    cli = arguments.cli.resolve()

    expected = json.loads((FIXTURE / "inspect-1.7.0.json").read_text(encoding="utf-8"))
    # The fact the whole comparison rests on: 1.7.0 saw one valid Adventure
    # file with Ancient Lake (save index 3) cleared. An empty or unrelated
    # report would compare equal to itself and prove nothing.
    slot = expected["slots"][0]
    if (expected["blocks"][0] != "valid" or slot["courses"][3] != 2
            or slot["balloons"][:2] != [2, 1]):
        print("FAIL: inspect-1.7.0.json is not the report this fixture documents")
        return 1

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-save-v170-") as directory:
        raw = Path(directory) / "eeprom.eep"
        raw.write_bytes(read_hex_fixture(FIXTURE / "eeprom.hex"))
        for label, path in (("raw EEPROM", raw),
                            ("1.7.0 container", FIXTURE / "eeprom.mdkr-save")):
            found = differences(expected, inspect(cli, path))
            failures += [f"{label}: {line}" for line in found]
            if not found:
                print(f"PASS {label}: every fact 1.7.0 reported is reported unchanged")
    if failures:
        print("FAIL: this mdkr-save reads the 1.7.0 save differently")
        for line in failures:
            print(f"  - {line}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
