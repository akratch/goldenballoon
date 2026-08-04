#!/usr/bin/env python3
"""Verify the native controller-settings smoke persisted every default.

The producer runs the same Restore controller defaults transaction used by the
button.  This companion deliberately reads the resulting INI instead of
accepting a UI trace: a visible control that fails to commit is not qualified.
"""

from __future__ import annotations

import argparse
from pathlib import Path


EXPECTED = {
    "RumbleEnabled": "1",
    "RumbleProfile": "strong",
    "ControllerA": "a",
    "ControllerB": "b",
    "ControllerX": "b",
    "ControllerY": "c_up",
    "ControllerStart": "start",
    "ControllerLeftStick": "none",
    "ControllerRightStick": "none",
    "ControllerLeftShoulder": "l",
    "ControllerRightShoulder": "r",
    "ControllerDpadUp": "dpad_up",
    "ControllerDpadDown": "dpad_down",
    "ControllerDpadLeft": "dpad_left",
    "ControllerDpadRight": "dpad_right",
    "ControllerLeftTrigger": "z",
    "ControllerRightTrigger": "z",
    "ControllerRightStickUp": "c_up",
    "ControllerRightStickDown": "c_down",
    "ControllerRightStickLeft": "c_left",
    "ControllerRightStickRight": "c_right",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("config", type=Path)
    args = parser.parse_args()

    if not args.config.is_file():
        raise SystemExit(f"missing controller-settings config: {args.config}")
    # config_ini.c deliberately accepts repeated sections while preserving
    # flattened last-writer-wins entries. Mirror that narrow project contract
    # here instead of using Python's strict ConfigParser, which rejects an
    # otherwise valid repeated [Video] section in the unrelated schema rows.
    section = ""
    actual: dict[str, str] = {}
    with args.config.open(encoding="utf-8") as source:
        for raw in source:
            line = raw.strip()
            if not line or line.startswith(("#", ";")):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip()
                continue
            key, separator, value = line.partition("=")
            if section == "Input" and separator and key.strip():
                actual[key.strip()] = value.strip()
    if not actual:
        raise SystemExit("controller settings did not persist an [Input] section")

    failures = [
        f"Input.{key}={actual.get(key)!r}, expected {value!r}"
        for key, value in EXPECTED.items()
        if actual.get(key) != value
    ]
    if failures:
        raise SystemExit("controller defaults were not fully persisted:\n" +
                         "\n".join(failures))
    print("controller settings persistence: PASS — 19 mappings plus rumble defaults")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
