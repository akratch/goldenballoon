#!/usr/bin/env python3
"""Prove two browser races reuse one wasm instance and return launcher focus."""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

from check_browser_runtime import (
    CDPClient,
    ChromeProcess,
    CheckFailure,
    OverlayServer,
    add_config_script,
    click_play,
    find_chrome,
    page_websocket,
    require,
    select_rom,
    wait_launcher,
    wait_value,
)


ROOT = Path(__file__).resolve().parent.parent
FATAL_MARKERS = (
    "[CRASH]",
    "[FATAL]",
    "memory access out of bounds",
    "RuntimeError:",
    "Aborted(",
    "device lost",
    "[MEM] free rejected",
)


def resolve(value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    return path.resolve()


def run(args: argparse.Namespace) -> None:
    shell = resolve(args.shell_dir)
    engine = resolve(args.engine_dir)
    rom = resolve(args.rom)
    chrome_path = find_chrome(args.chrome)
    for path in (
        shell / "index.html",
        shell / "mdkr64-shell.js",
        engine / "mdkr64_web.js",
        engine / "mdkr64_web.wasm",
        engine / "mdkr-save-tools.js",
        engine / "mdkr-save-tools.wasm",
        rom,
    ):
        require(path.is_file(), f"required browser artifact is missing: {path}")

    server = OverlayServer(shell, engine)
    server.start()
    with tempfile.TemporaryDirectory(
        prefix="mdkr64_persistent_browser_profile_"
    ) as profile:
        chrome = ChromeProcess(
            chrome_path, Path(profile), args.chrome_flag, args.verbose
        )
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Inspector"):
                cdp.call(f"{domain}.enable")
            add_config_script(
                cdp,
                {
                    "headlessTicks": args.ticks,
                    "env": {"MDKR_AUDIO": "0"},
                },
            )
            cdp.call("Page.navigate", {"url": server.origin + "/?session-loop=1"})
            wait_launcher(cdp, args.timeout)
            select_rom(cdp, rom, args.timeout)
            click_play(cdp)

            expression = """(() => {
              const snapshot = globalThis.__mdkrTestSnapshot &&
                globalThis.__mdkrTestSnapshot();
              const gate = document.getElementById("gate");
              const stage = document.getElementById("stage");
              const play = document.getElementById("play");
              return snapshot && {
                ...snapshot,
                launcherVisible: !!gate && !gate.hidden,
                stageHidden: !!stage && stage.hidden,
                playEnabled: !!play && !play.disabled,
                focused: document.activeElement && document.activeElement.id
              };
            })()"""
            first = wait_value(
                cdp,
                expression,
                lambda value: isinstance(value, dict)
                and len(value.get("engineRuns", [])) == 1
                and value.get("launcherVisible") is True
                and value.get("stageHidden") is True
                and value.get("playEnabled") is True,
                "first race to return to the live launcher",
                args.timeout,
            )
            require(first.get("wasmModuleCreations") == 1, f"first run: {first}")
            require(first.get("focused") == "play", f"launcher focus was {first}")

            click_play(cdp)
            second = wait_value(
                cdp,
                expression,
                lambda value: isinstance(value, dict)
                and len(value.get("engineRuns", [])) == 2
                and value.get("launcherVisible") is True
                and value.get("stageHidden") is True
                and value.get("playEnabled") is True,
                "second race to return through the same wasm instance",
                args.timeout,
            )
            runs = second.get("engineRuns", [])
            require(second.get("wasmModuleCreations") == 1, f"wasm recreated: {second}")
            require(
                [run.get("generation") for run in runs] == [1, 2]
                and all(run.get("exitCode") == 0 for run in runs)
                and all(run.get("shutdownComplete") is True for run in runs),
                f"invalid race generations: {runs}",
            )
            output = "\n".join(cdp.console)
            for marker in FATAL_MARKERS:
                require(marker not in output, f"browser session hit {marker!r}")
            require(
                output.count("[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0") == 2,
                "both browser epochs did not release their engine-owned state",
            )
            require(not cdp.failures, "browser/CDP failures: " + "; ".join(cdp.failures))
            print(
                "check_persistent_browser_session: PASS — two engine epochs, "
                "one wasm module, launcher restored with Play focus"
            )
        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()
            server.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-dir", default="build-web")
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--ticks", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        require(1 <= args.ticks <= 1000, "--ticks must be in [1, 1000]")
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError) as error:
        print(f"check_persistent_browser_session: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
