#!/usr/bin/env python3
"""Prove repeated wasm stage/audio generations conserve every owner.

The native resource plateau is necessary but cannot expose wasm32 pointer
layout, browser-owned WebGPU roots, asynchronous pipelines, or AudioWorklet
shutdown. This gate enters Ancient Lake and selects ``Restart Race`` three
times, producing four complete load/teardown generations in real
Chromium/WebGPU with an isolated browser profile. The native gate separately
retains four naturally completed races.
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

from check_browser_runtime import (
    CDPClient,
    ChromeProcess,
    CheckFailure,
    DEFAULT_SCRIPT,
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
from check_resource_plateau import (
    REGISTRY_RE,
    RENDERER_RE,
    RESOURCE_RE,
    plateau_error,
    registry_plateau_error,
    renderer_plateau_error,
)


ROOT = Path(__file__).resolve().parent.parent
FATAL_MARKERS = (
    "[CRASH]",
    "[FATAL]",
    "AddressSanitizer",
    "runtime error:",
    "memory access out of bounds",
    "device lost",
    "GPU process exited unexpectedly",
    "allocation FAILED",
    "resource_state: invalid",
)
MINIMUM_FRAMES = 7_200


def restart_cycle_route() -> str:
    lines = DEFAULT_SCRIPT.read_text(encoding="utf-8").splitlines()
    lines.append("# Browser ownership cycle: pause, select Restart Race, repeat.")
    for frame in (4_000, 5_000, 6_000):
        lines.extend(
            (
                f"{frame} START 4",
                f"{frame + 40} DOWN 4",
                f"{frame + 80} A 4",
            )
        )
    return "\n".join(lines) + "\n"


def resolve(value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    return path.resolve()


def validate_output(output: str) -> tuple[int, tuple[int, ...]]:
    for marker in FATAL_MARKERS:
        require(marker not in output, f"browser resource cycle hit {marker!r}")

    raw_rows = [
        tuple(map(int, match.groups()))
        for match in RESOURCE_RE.finditer(output)
    ]
    # A natural post-race "Try Again" reload carries cutscene id 100. The
    # Tracks pause-menu `PAUSE_RESET` route carries 0 after the same full
    # level teardown/reload. Normalize only Ancient Lake/one-player restart
    # rows so the shared ownership laws compare like generations.
    rows = [
        (row[0], row[1], 100, *row[3:])
        if row[0] == 5 and row[1] == 0 and row[2] == 0
        else row
        for row in raw_rows
    ]
    error = plateau_error(rows)
    require(
        error is None,
        "browser CPU/audio ownership: "
        f"{error}; rows={rows}",
    )

    raw_renderer_rows = [
        tuple(map(int, match.groups()))
        for match in RENDERER_RE.finditer(output)
    ]
    renderer_rows = [
        (row[0], row[1], 100, *row[3:])
        if row[0] == 5 and row[1] == 0 and row[2] == 0
        else row
        for row in raw_renderer_rows
    ]
    error = renderer_plateau_error(renderer_rows)
    require(error is None, f"browser renderer ownership: {error}")

    raw_registry_rows = [
        tuple(map(int, match.groups()))
        for match in REGISTRY_RE.finditer(output)
    ]
    registry_rows = [
        (row[0], row[1], 100, *row[3:])
        if row[0] == 5 and row[1] == 0 and row[2] == 0
        else row
        for row in raw_registry_rows
    ]
    error = registry_plateau_error(registry_rows)
    require(error is None, f"browser pointer ownership: {error}")

    race_rows = [
        row for row in rows
        if row[0] == 5 and row[1] == 0 and row[2] == 100
    ]
    require(
        output.count("[WGPU-SHUTDOWN]") == 1
        and "pendingPipelines=0 liveChildren=0 cpuArrays=0" in output,
        "browser WebGPU children did not reach zero",
    )
    require(
        output.count("[GFX-SHUTDOWN]") == 1
        and "backendReleased=1 cpuScratch=0" in output,
        "browser renderer frontend/scratch ownership did not reach zero",
    )
    require(
        output.count("[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0") == 1,
        "browser ROM/arena/allocator metadata did not reach zero",
    )
    require(
        output.count("[AUDIO-SHUTDOWN] device=0 web=1 complete=1") == 1,
        "browser AudioWorklet did not close exactly once",
    )
    return len(race_rows), race_rows[-1]


def run(args: argparse.Namespace) -> None:
    shell_dir = resolve(args.shell_dir)
    engine_dir = resolve(args.engine_dir)
    rom = resolve(args.rom)
    chrome_path = find_chrome(args.chrome)
    for path in (
        shell_dir / "index.html",
        shell_dir / "mdkr64-shell.js",
        engine_dir / "mdkr64_web.js",
        engine_dir / "mdkr64_web.wasm",
        engine_dir / "mdkr-save-tools.js",
        engine_dir / "mdkr-save-tools.wasm",
        rom,
    ):
        require(path.is_file(), f"required browser artifact is missing: {path}")
    require(args.frames >= MINIMUM_FRAMES, f"need at least {MINIMUM_FRAMES} frames")

    server = OverlayServer(shell_dir, engine_dir)
    server.start()
    with tempfile.TemporaryDirectory(
        prefix="mdkr64_browser_resource_profile_"
    ) as profile:
        chrome = ChromeProcess(
            chrome_path, Path(profile), args.chrome_flag, args.verbose
        )
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Inspector"):
                cdp.call(f"{domain}.enable")
            cdp.call(
                "Emulation.setDeviceMetricsOverride",
                {
                    "width": 480,
                    "height": 360,
                    "deviceScaleFactor": 1,
                    "mobile": False,
                },
            )
            add_config_script(
                cdp,
                {
                    "headlessFrames": args.frames,
                    "inputScript": restart_cycle_route(),
                    "env": {
                        "MDKR_AUDIO": "1",
                        "MDKR_AUTOPILOT": "1",
                        "MDKR_RESOURCE_STATS": "1",
                        "MDKR_SIMULATION_CADENCE": "enhanced",
                        "MDKR_SYNTH_FIELDS": "1",
                        "MDKR_TEST_HEADLESS_AUDIO": "1",
                    },
                },
            )
            cdp.call(
                "Page.navigate",
                {
                    "url": server.origin
                    + "/?browser-check=1&mode=pure&scale=1"
                },
            )
            wait_launcher(cdp, args.timeout)
            select_rom(cdp, rom, args.timeout)
            click_play(cdp)
            final = wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && "
                "globalThis.__mdkrTestSnapshot()",
                lambda value: isinstance(value, dict)
                and value.get("phase") == "exited"
                and value.get("exitCode") == 0
                and value.get("shutdownComplete") is True,
                "four-generation browser resource cycle",
                args.timeout,
            )
            require(
                final.get("audio", {}).get("shutdownComplete") is True
                and final.get("audio", {}).get("contextState") == "closed",
                f"browser audio owner remained live: {final.get('audio')}",
            )
            output = "\n".join(cdp.console)
            race_count, warmed = validate_output(output)

            # Both-direction parser controls: the same captured trace must fail
            # if one conserved voice-list row lies or if a race generation is
            # removed.
            require("voiceValid=1" in output, "no voice ownership rows captured")
            corrupt = output.replace("voiceValid=1", "voiceValid=0", 1)
            try:
                validate_output(corrupt)
            except CheckFailure:
                pass
            else:
                raise CheckFailure("voice-ownership positive control passed")
            shortened_lines = output.splitlines()
            resource_lines = [
                line for line in shortened_lines
                if "resource_state: level=5 players=0 cutscene=0" in line
            ]
            require(
                len(resource_lines) >= 4,
                "positive control needs four pause-restart resource rows",
            )
            shortened_lines.remove(resource_lines[-2])
            shortened = "\n".join(shortened_lines)
            try:
                validate_output(shortened)
            except CheckFailure:
                pass
            else:
                raise CheckFailure("missing-generation positive control passed")

            print(
                "check_browser_resource_plateau: PASS — "
                f"{race_count} wasm race loads; "
                f"main={warmed[3]} slots/{warmed[4]} bytes; "
                f"audio={warmed[7]}/{warmed[8]} bytes; "
                f"voices={warmed[10]}/{warmed[11]}/{warmed[12]}; "
                f"voicePeak={warmed[19]}/{warmed[20]}; "
                "WebGPU/frontend/ROM/arena/AudioWorklet owners reached zero"
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
    parser.add_argument("--frames", type=int, default=MINIMUM_FRAMES)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError) as exc:
        print(f"check_browser_resource_plateau: FAIL — {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
