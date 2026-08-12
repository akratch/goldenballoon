#!/usr/bin/env python3
"""Contract tests for the aggregate-only operated experience canary."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "run_party_experience_canary.py"
SPEC = importlib.util.spec_from_file_location("run_party_experience_canary", TOOL)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def metric() -> dict[str, object]:
    return {
        "matchCreate": {"attempts": 20, "successes": 19, "p95Ms": 2_500},
        "matchJoin": {"attempts": 20, "successes": 19, "p95Ms": 2_500},
        "phoneDirect": {"attempts": 20, "successes": 18,
                        "setupP95Ms": 8_000, "inputRttP95Ms": 250},
    }


def main() -> int:
    assert MODULE.validated_origin("https://party.example", False) == \
        "https://party.example"
    assert MODULE.validated_origin("http://127.0.0.1:8787/", True) == \
        "http://127.0.0.1:8787"
    for value in ("http://party.example", "https://user@party.example",
                  "https://party.example/path", "https://party.example/?secret=x",
                  "https://party.example/#secret"):
        try:
            MODULE.validated_origin(value, False)
        except MODULE.CheckFailure:
            pass
        else:
            raise AssertionError(f"accepted unsafe origin shape: {value}")
    assert MODULE.p95(list(range(1, 21)), 999) == 19
    assert MODULE.p95([], 999) == 999
    value = metric()
    assert MODULE.decision(value) == "GO"
    value["phoneDirect"]["inputRttP95Ms"] = 251  # type: ignore[index]
    assert MODULE.decision(value) == "STOP"

    original_match = MODULE.match_attempt
    original_phone = MODULE.phone_attempt
    original_find_chrome = MODULE.find_chrome
    try:
        MODULE.match_attempt = lambda _origin: (100, 120)
        MODULE.phone_attempt = lambda *_args: (500, 40)
        MODULE.find_chrome = lambda _value: "fixture-chrome"
        with tempfile.TemporaryDirectory(prefix="mdkr-canary-contract-") as temporary:
            output = Path(temporary) / "experience.json"
            args = argparse.Namespace(origin="https://party.example",
                output=output, attempts=20, development=False,
                allow_http_loopback=False, chrome=None, chrome_flag=[],
                timeout=1.0, verbose=False)
            assert MODULE.run(args) == 0
            result = json.loads(output.read_text(encoding="utf-8"))
            assert result == {
                "schemaVersion": 1, "source": "synthetic_canary_v1",
                "matchCreate": {"attempts": 20, "successes": 20, "p95Ms": 100},
                "matchJoin": {"attempts": 20, "successes": 20, "p95Ms": 120},
                "phoneDirect": {"attempts": 20, "successes": 20,
                                "setupP95Ms": 500, "inputRttP95Ms": 40},
                "decision": "GO",
            }
            try:
                MODULE.run(args)
            except MODULE.CheckFailure as error:
                assert str(error) == "output already exists"
            else:
                raise AssertionError("canary overwrote operated evidence")
    finally:
        MODULE.match_attempt = original_match
        MODULE.phone_attempt = original_phone
        MODULE.find_chrome = original_find_chrome

    print("test_party_experience_canary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
