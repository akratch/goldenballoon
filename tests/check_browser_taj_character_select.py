#!/usr/bin/env python3
"""Prove the shipped WebGPU build renders and selects a physical Taj actor.

This is intentionally narrower than ``check_browser_runtime.py``. It drives the
real ABRACADABRA keyboard route from a fresh browser profile, reaches the base
eight-character picker, and joins C-side model probes to Chrome screenshots.
That combination catches both historical failure shapes: an invisible virtual
cursor and the persistent all-caps TAJ overlay that mislabeled every hover. A
second run in the same isolated profile proves the unlock survives a real
IDBFS flush and reload without re-entering the code.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable

from check_browser_runtime import (
    CDPClient,
    ChromeProcess,
    CheckFailure,
    FATAL_MARKERS,
    OverlayServer,
    ROM_BYTES,
    add_config_script,
    capture_png,
    click_play,
    find_chrome,
    network_problems,
    page_websocket,
    png_pixels,
    persist_snapshot,
    require,
    resolve_path,
    select_rom,
    snapshot,
    wait_launcher,
    wait_value,
)


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/taj_unlock_select.txt"
FRAMES = 4350
RELOAD_FRAMES = 1800
RETAIL_LABELS = {
    0: "KRUNCH",
    1: "DIDDY",
    2: "BUMPER",
    3: "BANJO",
    4: "CONKER",
    5: "TIPTUP",
    6: "PIPSY",
    7: "TIMBER",
}
RELOAD_SCRIPT = """\
# Persisted unlock -> base character select -> Taj hover. No magic code route.
1250 START 4
1330 START 4
1520 DOWN 4
1570 RIGHT 4
"""
TAJ_PICKER_TRACE_SIGNATURE = (
    b"taj_hover: controller=%d index=%d taj=%d visual=%d sign=%d label=%s"
)


def git_output(*args: str) -> str:
    process = subprocess.run(
        ["git", *args], cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    require(process.returncode == 0,
            f"git {' '.join(args)} failed: {process.stderr.strip()}")
    return process.stdout.strip()


def verify_artifact_boundary(
    shell_dir: Path, engine_dir: Path, allow_overlay: bool
) -> None:
    """Reject the stale-shell/current-engine hybrid that masked issue #1."""
    wasm_path = engine_dir / "mdkr64_web.wasm"
    require(
        TAJ_PICKER_TRACE_SIGNATURE in wasm_path.read_bytes(),
        "web engine predates the exact Taj picker identity trace; rebuild it",
    )
    if allow_overlay:
        return

    require(
        shell_dir == engine_dir,
        "candidate mode requires one exact staged directory for shell and "
        "engine; pass --allow-overlay only for local development",
    )
    require(
        git_output("status", "--porcelain", "--untracked-files=all") == "",
        "candidate browser evidence requires a clean source tree",
    )
    head = git_output("rev-parse", "HEAD")
    info_path = engine_dir / "build-info.json"
    require(info_path.is_file(), f"missing web provenance: {info_path}")
    try:
        record = json.loads(info_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CheckFailure(f"cannot parse {info_path}: {error}") from error
    require(isinstance(record, dict),
            f"web provenance is not a JSON object: {info_path}")
    require(
        record.get("source_commit") == head,
        "staged web source_commit does not equal HEAD: "
        f"{record.get('source_commit')!r} != {head}",
    )
    require(
        record.get("source_dirty") is False,
        "staged web provenance is dirty or ambiguous: "
        f"source_dirty={record.get('source_dirty')!r}",
    )
    require(
        record.get("wasm_bytes") == wasm_path.stat().st_size,
        "staged web provenance wasm size does not match the served module",
    )


def wait_console(
    cdp: CDPClient,
    predicate: Callable[[str], bool],
    description: str,
    timeout: float,
    start: int = 0,
) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for line in cdp.console[start:]:
            if predicate(line):
                return line
        state = snapshot(cdp)
        require(
            state.get("phase") not in {"aborted", "main-threw", "graphics-failed"},
            f"browser failed while waiting for {description}: {state}",
        )
        time.sleep(0.08)
    tail = "\n".join(cdp.console[-40:])
    raise CheckFailure(f"timed out waiting for {description}; console tail:\n{tail}")


def wait_frames(cdp: CDPClient, count: int, description: str, timeout: float) -> None:
    start = snapshot(cdp).get("frames", 0)
    require(isinstance(start, (int, float)), f"{description} has no frame counter")
    wait_value(
        cdp,
        "globalThis.__mdkrTestSnapshot().frames",
        lambda value: isinstance(value, (int, float)) and value >= start + count,
        description,
        timeout,
    )


def canvas_rect(cdp: CDPClient) -> dict[str, float]:
    value = cdp.evaluate(
        """(() => {
          const canvas = document.getElementById("canvas");
          const rect = canvas.getBoundingClientRect();
          return {left: rect.left, top: rect.top, width: rect.width,
                  height: rect.height, hidden: canvas.hidden};
        })()"""
    )
    require(
        isinstance(value, dict)
        and value.get("hidden") is False
        and float(value.get("width", 0)) > 0
        and float(value.get("height", 0)) > 0,
        f"browser canvas is not visible: {value}",
    )
    return {key: float(value[key]) for key in ("left", "top", "width", "height")}


def region_samples(
    png: bytes,
    rect: dict[str, float],
    x0: float = 0.36,
    x1: float = 0.64,
    y0: float = 0.48,
    y1: float = 0.95,
) -> tuple[list[tuple[int, int, int]], int, int]:
    width, height, channels, pixels = png_pixels(png)
    left = max(0, int(rect["left"] + rect["width"] * x0))
    right = min(width, int(rect["left"] + rect["width"] * x1))
    top = max(0, int(rect["top"] + rect["height"] * y0))
    bottom = min(height, int(rect["top"] + rect["height"] * y1))
    require(right > left and bottom > top, "Taj screenshot region is empty")
    samples: list[tuple[int, int, int]] = []
    for y in range(top, bottom):
        row = y * width * channels
        for x in range(left, right):
            offset = row + x * channels
            if channels in (1, 2):
                red = green = blue = pixels[offset]
            else:
                red, green, blue = pixels[offset : offset + 3]
            samples.append((red, green, blue))
    return samples, right - left, bottom - top


def colour_counts(
    png: bytes, rect: dict[str, float]
) -> tuple[int, int, int, int]:
    # Taj's upper body occupies this lower-centre slot. It deliberately stops
    # above the old bottom-screen TAJ badge, so a purple rectangle or pixels
    # from adjacent retail racers cannot masquerade as the physical actor.
    actor_samples, actor_width, actor_height = region_samples(
        png, rect, 0.45, 0.55, 0.50, 0.82
    )
    purple = sum(
        red > 30
        and green < 135
        and blue > 95
        and blue > red * 1.22
        and blue > green * 1.12
        for red, green, blue in actor_samples
    )
    # Restrict red evidence to Taj's physical lower-centre placard slot so an
    # ordinary retail character cannot satisfy the P1-sign oracle.
    placard_samples, placard_width, placard_height = region_samples(
        png, rect, 0.47, 0.57, 0.66, 0.91
    )
    red_count = sum(
        red > 140 and red > green * 1.5 and red > blue * 1.5
        for red, green, blue in placard_samples
    )
    return (
        purple,
        red_count,
        actor_width * actor_height,
        placard_width * placard_height,
    )


def changed_pixels(
    first: bytes, second: bytes, rect: dict[str, float]
) -> tuple[int, int]:
    first_samples, width, height = region_samples(
        first, rect, 0.45, 0.55, 0.50, 0.82
    )
    second_samples, second_width, second_height = region_samples(
        second, rect, 0.45, 0.55, 0.50, 0.82
    )
    require(
        (width, height) == (second_width, second_height),
        "Taj screenshot regions changed dimensions",
    )
    changed = sum(
        any(abs(a - b) > 20 for a, b in zip(left, right))
        for left, right in zip(first_samples, second_samples)
    )
    return changed, width * height


def run(args: argparse.Namespace) -> None:
    rom = resolve_path(args.rom)
    shell_dir = resolve_path(args.shell_dir)
    engine_dir = resolve_path(args.engine_dir)
    chrome_path = find_chrome(args.chrome)
    for label, path in (
        ("ROM", rom),
        ("input fixture", SCRIPT),
        ("web shell", shell_dir / "index.html"),
        ("web shell JavaScript", shell_dir / "mdkr64-shell.js"),
        ("wasm loader", engine_dir / "mdkr64_web.js"),
        ("wasm module", engine_dir / "mdkr64_web.wasm"),
    ):
        require(path.is_file(), f"missing {label}: {path}")
    require(rom.stat().st_size == ROM_BYTES, f"ROM is not 12 MiB: {rom}")
    verify_artifact_boundary(shell_dir, engine_dir, args.allow_overlay)

    evidence = Path(args.evidence_dir).resolve() if args.evidence_dir else None
    if evidence is not None:
        evidence.mkdir(parents=True, exist_ok=True)

    server = OverlayServer(shell_dir, engine_dir)
    server.start()
    if args.verbose:
        print(f"browser server: {server.origin}", flush=True)
    with tempfile.TemporaryDirectory(prefix="mdkr64_taj_chrome_") as profile:
        chrome = ChromeProcess(
            chrome_path, Path(profile), args.chrome_flag, args.verbose
        )
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Network", "Log", "Inspector"):
                cdp.call(f"{domain}.enable")
            cdp.call(
                "Emulation.setDeviceMetricsOverride",
                {
                    "width": 960,
                    "height": 540,
                    "deviceScaleFactor": 1,
                    "mobile": False,
                },
            )
            preload = add_config_script(
                cdp,
                {
                    "headlessFrames": FRAMES,
                    "inputScript": SCRIPT.read_text(encoding="ascii"),
                    "env": {
                        "MDKR_AUDIO": "0",
                        "MDKR_TRACE": "1",
                        "MDKR_TAJ_SELECT_TRACE": "1",
                    },
                },
            )
            cdp.call(
                "Page.navigate",
                {"url": server.origin + "/?browser-taj-select=1&mode=restored"},
            )
            wait_launcher(cdp, args.timeout)
            select_rom(cdp, rom, args.timeout)
            click_play(cdp)
            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && "
                "globalThis.__mdkrTestSnapshot().phase",
                lambda value: value == "main-started",
                "wasm main() start",
                args.timeout,
            )

            wait_console(
                cdp,
                lambda line: "[mdkr64] renderer backend: webgpu" in line.lower(),
                "explicit WebGPU backend telemetry",
                args.timeout,
            )
            wait_console(
                cdp,
                lambda line: "magic_code_submit: accepted=1 id=-2" in line,
                "ABRACADABRA acceptance",
                args.timeout,
            )
            wait_console(
                cdp,
                lambda line: "[TAJSELECT] event=compose" in line
                and "model=208" in line,
                "physical Park Warden/Taj composition",
                args.timeout,
            )
            wait_console(
                cdp,
                lambda line: "[TAJSELECT] event=sign_schema_verified" in line,
                "verified placard-only batch schema",
                args.timeout,
            )
            wait_console(
                cdp,
                lambda line: "[TAJSELECT] event=sign_compose" in line
                and "sign_model=302" in line,
                "authored player placard composition",
                args.timeout,
            )

            wait_frames(
                cdp, 12, "twelve post-composition browser frames", args.timeout
            )
            rect = canvas_rect(cdp)
            ordinary = capture_png(cdp)

            wait_console(
                cdp,
                lambda line: "[TAJSELECT] event=animation" in line
                and "hover=0x1" in line
                and "confirmed=0x0" in line
                and "animation=1" in line,
                "Taj hover animation",
                args.timeout,
            )
            wait_frames(cdp, 4, "four Taj hover presentation frames", args.timeout)
            hover = capture_png(cdp)
            wait_frames(cdp, 20, "twenty Taj hover frames", args.timeout)
            hover_motion = capture_png(cdp)

            wait_console(
                cdp,
                lambda line: "[TAJSELECT] event=animation" in line
                and "confirmed=0x1" in line
                and "animation=5" in line,
                "Taj confirmation animation",
                args.timeout,
            )
            wait_frames(
                cdp, 4, "four Taj confirmation presentation frames", args.timeout
            )
            confirmed = capture_png(cdp)
            wait_console(
                cdp,
                lambda line: "taj_select: player=0 controller=0 selected=1 donor=9"
                in line,
                "final Taj identity mapping",
                args.timeout,
            )
            console_text = "\n".join(cdp.console)
            for character, label in RETAIL_LABELS.items():
                require(
                    f"taj_hover: controller=0 index={character} taj=0 "
                    f"visual=2 sign=0 label={label}" in console_text,
                    f"ordinary base character {character} did not retain "
                    f"identity {label} with Taj's placard hidden",
                )
            require(
                "taj_hover: controller=0 index=8 taj=1 visual=2 sign=1 "
                "label=TAJ" in console_text,
                "only the appended virtual slot may resolve as TAJ",
            )

            (
                ordinary_purple,
                ordinary_red,
                region_size,
                placard_region_size,
            ) = colour_counts(ordinary, rect)
            (
                hover_purple,
                hover_red,
                hover_region_size,
                hover_placard_region_size,
            ) = colour_counts(hover, rect)
            motion, motion_region_size = changed_pixels(hover, hover_motion, rect)
            if evidence is not None:
                for name, png in (
                    ("ordinary", ordinary),
                    ("taj-hover", hover),
                    ("taj-hover-motion", hover_motion),
                    ("taj-confirmed", confirmed),
                ):
                    (evidence / f"{name}.png").write_bytes(png)
                (evidence / "console.log").write_text(
                    "\n".join(cdp.console) + "\n", encoding="utf-8"
                )
                (evidence / "metrics.json").write_text(
                    json.dumps(
                        {
                            "canvas": rect,
                            "regionPixels": region_size,
                            "placardRegionPixels": placard_region_size,
                            "ordinaryPurple": ordinary_purple,
                            "ordinaryRed": ordinary_red,
                            "hoverPurple": hover_purple,
                            "hoverRed": hover_red,
                            "hoverMotionPixels": motion,
                        },
                        indent=2,
                        sort_keys=True,
                    ) + "\n",
                    encoding="utf-8",
                )
            if args.verbose:
                print(
                    "Taj pixels: "
                    f"ordinary purple={ordinary_purple} red={ordinary_red}; "
                    f"hover purple={hover_purple} red={hover_red}; "
                    f"motion={motion}; region={region_size}",
                    flush=True,
                )
            require(region_size == hover_region_size == motion_region_size,
                    "Taj pixel evidence used inconsistent regions")
            require(
                placard_region_size == hover_placard_region_size,
                "Taj placard evidence used inconsistent regions",
            )
            require(
                ordinary_purple >= region_size * 0.05,
                "physical Taj is not visibly purple before hover: "
                f"{ordinary_purple}/{region_size}",
            )
            require(
                hover_purple >= region_size * 0.05,
                "physical Taj disappeared while hovered: "
                f"{hover_purple}/{region_size}",
            )
            require(
                hover_red >= max(
                    ordinary_red * 4, int(placard_region_size * 0.15)
                ),
                "Taj did not raise a visibly numbered red P1 placard: "
                f"ordinary={ordinary_red}, hover={hover_red}, "
                f"region={placard_region_size}",
            )
            require(
                motion >= region_size * 0.05,
                "Taj hover pose did not visibly animate: "
                f"{motion}/{region_size} pixels changed",
            )

            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && "
                "globalThis.__mdkrTestSnapshot().phase",
                lambda value: value in {"exited", "aborted", "main-threw"},
                "finite Taj browser run",
                args.timeout,
            )
            final = snapshot(cdp)
            require(
                final.get("phase") == "exited"
                and final.get("exitCode") == 0
                and final.get("shutdownComplete") is True,
                f"Taj browser run did not exit cleanly: {final}",
            )
            console_text = "\n".join(cdp.console)
            fatal = [
                marker for marker in FATAL_MARKERS
                if marker.lower() in console_text.lower()
            ]
            require(not fatal, f"Taj browser run emitted fatal markers: {fatal}")
            require(
                not cdp.exceptions and not cdp.failures,
                f"Taj browser run raised browser errors: "
                f"exceptions={cdp.exceptions}, failures={cdp.failures}",
            )
            leaks = network_problems(cdp.network, server.origin, rom.name)
            require(not leaks, f"Taj browser run leaked ROM/network data: {leaks}")

            first_flush = persist_snapshot(cdp, args.timeout)
            require(
                first_flush.get("error") is None,
                f"Taj unlock IDBFS flush failed: {first_flush.get('error')}",
            )

            # Reload the same isolated browser profile without entering
            # ABRACADABRA. The picker must restore Taj from the durable sidecar,
            # not merely retain a process-local flag from the first run.
            cdp.call(
                "Page.removeScriptToEvaluateOnNewDocument",
                {"identifier": preload},
            )
            reload_console_start = len(cdp.console)
            reload_preload = add_config_script(
                cdp,
                {
                    "headlessFrames": RELOAD_FRAMES,
                    "inputScript": RELOAD_SCRIPT,
                    "env": {
                        "MDKR_AUDIO": "0",
                        "MDKR_TRACE": "1",
                        "MDKR_TAJ_SELECT_TRACE": "1",
                    },
                },
            )
            cdp.call("Page.reload", {"ignoreCache": True})
            wait_launcher(cdp, args.timeout)
            click_play(cdp)
            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && "
                "globalThis.__mdkrTestSnapshot().phase",
                lambda value: value == "main-started",
                "persisted-Taj reload main() start",
                args.timeout,
            )
            wait_console(
                cdp,
                lambda line: "taj_roster: base=8 taj=8 enabled=1" in line,
                "persisted Taj roster",
                args.timeout,
                reload_console_start,
            )
            wait_console(
                cdp,
                lambda line: "[TAJSELECT] event=animation" in line
                and "hover=0x1" in line and "animation=1" in line,
                "persisted Taj hover",
                args.timeout,
                reload_console_start,
            )
            wait_frames(cdp, 8, "settled persisted Taj presentation", args.timeout)
            reload_rect = canvas_rect(cdp)
            reload_png = capture_png(cdp)
            reload_purple, reload_red, reload_region, reload_placard = (
                colour_counts(reload_png, reload_rect)
            )
            require(
                reload_purple >= reload_region * 0.05
                and reload_red >= reload_placard * 0.15,
                "persisted reload did not visibly restore Taj and his P1 "
                f"placard: purple={reload_purple}/{reload_region}, "
                f"red={reload_red}/{reload_placard}",
            )
            reload_console = cdp.console[reload_console_start:]
            require(
                not any("magic_code_submit: accepted=1" in line
                        for line in reload_console),
                "reload re-entered the magic-code route instead of using "
                "persisted Taj state",
            )
            if evidence is not None:
                (evidence / "persisted-reload.png").write_bytes(reload_png)
            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && "
                "globalThis.__mdkrTestSnapshot().phase",
                lambda value: value in {"exited", "aborted", "main-threw"},
                "persisted Taj reload exit",
                args.timeout,
            )
            second_flush = persist_snapshot(cdp, args.timeout)
            require(
                second_flush.get("error") is None
                and second_flush.get("snapshot", {}).get("phase") == "exited"
                and second_flush.get("snapshot", {}).get("exitCode") == 0,
                f"persisted Taj reload did not exit cleanly: {second_flush}",
            )
            reload_fatal = [
                marker for marker in FATAL_MARKERS
                if marker.lower() in "\n".join(reload_console).lower()
            ]
            require(not reload_fatal,
                    f"persisted Taj reload emitted fatal markers: {reload_fatal}")
            cdp.call(
                "Page.removeScriptToEvaluateOnNewDocument",
                {"identifier": reload_preload},
            )

        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()
            server.close()

    print(
        "check_browser_taj_character_select: PASS -- real Chrome/WebGPU "
        "rendered every ordinary hover, animated/selected physical Taj, and "
        "restored him from durable browser storage"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-dir", default="dist/web")
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument(
        "--allow-overlay", action="store_true",
        help="development only: permit shell and engine from different "
             "directories and skip clean-candidate provenance checks",
    )
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=240.0)
    parser.add_argument("--evidence-dir")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError) as error:
        print(
            f"check_browser_taj_character_select: FAIL -- {error}",
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
