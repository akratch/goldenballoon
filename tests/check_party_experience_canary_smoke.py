#!/usr/bin/env python3
"""Smoke the operated canary and direct play through signaling loss locally."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from check_browser_online_two_person import free_port
from check_browser_runtime import CheckFailure, require
from check_party_capacity import start_worker, stop_worker


ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "run_party_experience_canary.py"
SPEC = importlib.util.spec_from_file_location("run_party_experience_canary", TOOL)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def run(args: argparse.Namespace) -> None:
    shell = (ROOT / args.shell_dir).resolve()
    require((shell / "controller/index.html").is_file(),
            "canary smoke requires the staged controller")
    with tempfile.TemporaryDirectory(prefix="mdkr-canary-smoke-") as temporary:
        root = Path(temporary)
        origin = f"http://127.0.0.1:{free_port()}"
        log_path = root / "wrangler.log"
        process: subprocess.Popen[bytes] | None = None
        with log_path.open("wb") as log:
            try:
                process = start_worker(origin, shell, root / "state", log, 10_000)
                output = root / "experience.json"
                result = MODULE.run(argparse.Namespace(origin=origin, output=output,
                    attempts=1, development=True, allow_http_loopback=True,
                    chrome=args.chrome, chrome_flag=args.chrome_flag,
                    timeout=args.timeout, verbose=args.verbose))
                require(result == 1, "one-attempt smoke incorrectly qualified as GO")
                aggregate = json.loads(output.read_text(encoding="utf-8"))
                require(isinstance(aggregate, dict) and set(aggregate) == {
                    "schemaVersion", "source", "matchCreate", "matchJoin",
                    "phoneDirect", "decision",
                }, "canary smoke emitted an unexpected aggregate shape")
                require(aggregate["schemaVersion"] == 1 and
                        aggregate["source"] == "synthetic_canary_v1" and
                        aggregate["decision"] == "STOP",
                        "canary smoke emitted the wrong bounded identity/decision")
                for lane in ("matchCreate", "matchJoin"):
                    require(isinstance(aggregate[lane], dict) and
                            set(aggregate[lane]) == {
                                "attempts", "successes", "p95Ms",
                            }, "canary smoke emitted an unexpected Match lane")
                require(isinstance(aggregate["phoneDirect"], dict) and
                        set(aggregate["phoneDirect"]) == {
                            "attempts", "successes", "setupP95Ms", "inputRttP95Ms",
                        }, "canary smoke emitted an unexpected phone lane")
                require(aggregate["matchCreate"]["attempts"] == 1 and
                        aggregate["matchCreate"]["successes"] == 1 and
                        aggregate["matchJoin"]["successes"] == 1 and
                        aggregate["phoneDirect"]["successes"] == 1,
                        "production-path canary journey failed")
            finally:
                stop_worker(process)
        details = log_path.read_text(encoding="utf-8", errors="replace")
        require("ERROR" not in details.upper(),
                "Wrangler reported an error during canary smoke")
    print("check_party_experience_canary_smoke: PASS — real Worker create/join, "
          "direct phone setup/input during signaling loss, same-lease epoch "
          "rebind and fresh input after recovery")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError, subprocess.SubprocessError,
            json.JSONDecodeError) as error:
        print(f"check_party_experience_canary_smoke: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
