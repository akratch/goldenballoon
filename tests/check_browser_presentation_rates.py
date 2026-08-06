#!/usr/bin/env python3
"""Browser rAF host schedules preserve fixed authority and smooth images.

The real wasm/WebGPU build runs in Chromium. A shell-owned rAF seam injects
bounded timestamps while still yielding to the browser event loop, allowing
display-144, capped-60-on-144, irregular display, and a shared native
``uncapped`` config to be checked independently of the CI monitor. Browser
uncapped must resolve explicitly to display/rAF rather than claiming an
unbounded native swapchain. Every non-Original arm sends the raw public
interpolation request and proves production emits immutable intermediate images.
"""

from __future__ import annotations

import argparse
import json
import re
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from check_browser_runtime import (
    CDPClient,
    CheckFailure,
    ChromeProcess,
    OverlayServer,
    WGPU_RETIRE_RE,
    add_config_script,
    click_play,
    find_chrome,
    page_websocket,
    require,
    resolve_path,
    select_rom,
    wait_launcher,
    wait_value,
)
from harness_utils import (completed_tick_conservation,
                           parse_last as harness_parse_last, present_mode_rows)

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
ROM_BYTES = 12 * 1024 * 1024
TICKS = 60

FATAL_MARKERS = (
    "[CRASH]", "[FATAL]", "memory access out of bounds", "RuntimeError:",
    "Aborted(", "device lost", "GPU process exited unexpectedly",
)


@dataclass(frozen=True)
class Arm:
    name: str
    policy: str
    deltas_ns: list[int]
    expected_presents: int | None
    effective_kind: int
    requested_kind: int
    effective_rate: int = 0
    requested_rate: int = 0


@dataclass(frozen=True)
class Result:
    state: list[str]
    events: list[str]
    inputs: list[str]
    audio_digest: str
    audio_size: int
    summary: dict[str, int]
    audio: dict[str, int]
    replay: dict[str, int]
    packet: dict[str, int]
    retained: dict[str, int]
    backpressure: dict[str, int]
    console: list[str]


def parse_last(lines: list[str], name: str) -> dict[str, int]:
    """The shared row parser, over console lines and raising CheckFailure.

    This check reports through CheckFailure, which its own arm loop catches;
    letting the shared parser's RuntimeError past would skip that handling and
    lose the per-arm context the failure is reported with.
    """

    try:
        return harness_parse_last("\n".join(lines), name)
    except RuntimeError as error:
        raise CheckFailure(str(error)) from error


def marked(lines: list[str], prefix: str) -> list[str]:
    return [line[line.index(prefix):] for line in lines if prefix in line]


def browser_digest(cdp: CDPClient, path: str) -> dict[str, Any]:
    expression = f"""(async () => {{
      const bytes = __mdkrTestState.module.FS.readFile({json.dumps(path)});
      const digest = await crypto.subtle.digest("SHA-256", bytes);
      return {{
        size: bytes.length,
        digest: [...new Uint8Array(digest)].map(
          (value) => value.toString(16).padStart(2, "0")).join("")
      }};
    }})()"""
    value = cdp.evaluate(expression, await_promise=True, timeout=30)
    require(isinstance(value, dict), "browser PCM digest is unavailable")
    return value


def run_arm(server: OverlayServer, chrome_path: Path, rom: Path,
            fixture: str, arm: Arm, chrome_flags: list[str],
            timeout: float, verbose: bool) -> Result:
    with tempfile.TemporaryDirectory(
            prefix=f"mdkr64_browser_rate_{arm.name}_") as profile:
        chrome = ChromeProcess(chrome_path, Path(profile), chrome_flags, verbose)
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Inspector"):
                cdp.call(f"{domain}.enable")
            cdp.call(
                "Emulation.setDeviceMetricsOverride",
                {"width": 640, "height": 480,
                 "deviceScaleFactor": 1, "mobile": False},
            )
            config = {
                "headlessTicks": TICKS,
                "inputScript": fixture,
                "rafDeltasNs": arm.deltas_ns,
                "env": {
                    "MDKR_AUDIO": "0",
                    "MDKR_AUDIO_DUMP": "/tmp/browser-rate.wav",
                    "MDKR_AUDIO_SERVICE_TRACE": "1",
                    "MDKR_AUTOPILOT": "1",
                    "MDKR_EVENT_HASH": "1",
                    "MDKR_INPUT_HASH": "1",
                    "MDKR_LOAD_TRACK": "5",
                    "MDKR_PRESENT_RATE": arm.policy,
                    "MDKR_PRESENT_SMOOTHING": "interpolate",
                    "MDKR_PRESENT_SCHED_TRACE": "1",
                    "MDKR_STATE_HASH": "3",
                },
            }
            add_config_script(cdp, config)
            cdp.call("Page.navigate", {"url": server.origin + "/?rate=display"})
            wait_launcher(cdp, timeout)

            disclosure = cdp.evaluate(
                "(() => { const d = document.getElementById("
                "'experimental-presentation'); return d ? {tag: d.tagName, "
                "open: d.open, ownsRate: d.contains(document.getElementById("
                "'rate')), ownsSmoothing: d.contains(document.getElementById("
                "'smoothing'))} : null; })()")
            require(
                isinstance(disclosure, dict)
                and disclosure.get("tag") == "DETAILS"
                and disclosure.get("open") is True
                and disclosure.get("ownsRate") is True
                and disclosure.get("ownsSmoothing") is True,
                "a configured modern rate did not reveal its experimental "
                f"controls: {disclosure}",
            )

            browser_choices = cdp.evaluate(
                "[...document.querySelectorAll('#rate option')].map(o => o.value)")
            require(
                isinstance(browser_choices, list)
                and "display" in browser_choices
                and "uncapped" not in browser_choices,
                f"browser frame-rate choices are dishonest: {browser_choices}",
            )
            browser_labels = cdp.evaluate(
                "[...document.querySelectorAll('#rate option')].map(o => o.textContent)")
            require(
                isinstance(browser_labels, list)
                and len(browser_labels) == len(browser_choices)
                and "authored motion" in browser_labels[0]
                and "Match display" in browser_labels[1],
                f"browser frame-rate labels drifted: {browser_labels}",
            )
            smoothing_choices = cdp.evaluate(
                "[...document.querySelectorAll('#smoothing option')]"
                ".map(o => o.value)")
            require(
                smoothing_choices == ["off", "interpolate"],
                f"browser motion-smoothing choices are dishonest: "
                f"{smoothing_choices}",
            )
            note = cdp.evaluate(
                "document.querySelector('#frame-rate-status').textContent")
            normalized_note = (
                " ".join(note.split()) if isinstance(note, str) else note
            )
            normalized_note_lower = (
                normalized_note.lower()
                if isinstance(normalized_note, str) else normalized_note
            )
            require(
                isinstance(normalized_note, str)
                and "unique images" in normalized_note_lower
                and "physics, ai, input, timers, audio, or saves" in
                    normalized_note_lower
                and "requestAnimationFrame" in normalized_note
                and "cannot exceed" in normalized_note_lower,
                "browser launcher does not disclose frame-rate "
                f"limits: {note!r}",
            )

            select_rom(cdp, rom, timeout)
            click_play(cdp)
            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && "
                "globalThis.__mdkrTestSnapshot().phase",
                lambda value: value in {"exited", "aborted", "main-threw"},
                f"{arm.name} finite wasm run",
                timeout,
            )
            snapshot = cdp.evaluate("globalThis.__mdkrTestSnapshot()")
            require(
                isinstance(snapshot, dict)
                and snapshot.get("phase") == "exited"
                and snapshot.get("exitCode") == 0
                and snapshot.get("shutdownComplete") is True,
                f"{arm.name} did not shut down cleanly: {snapshot}",
            )
            console = list(cdp.console)
            for marker in FATAL_MARKERS:
                require(
                    not any(marker in line for line in console),
                    f"{arm.name}: fatal marker {marker}",
                )
            pcm = browser_digest(cdp, "/tmp/browser-rate.wav")
            return Result(
                marked(console, "[SIMHASH]"),
                marked(console, "[EVENTHASH]"),
                marked(console, "[INPUTHASH]"),
                str(pcm.get("digest")),
                int(pcm.get("size", 0)),
                parse_last(console, "PRESENTSCHED-SUMMARY"),
                parse_last(console, "AUDIO-SERVICE"),
                parse_last(console, "REPLAY-SUMMARY"),
                parse_last(console, "PRESENT-PACKET"),
                parse_last(console, "RETAINED-TASK"),
                parse_last(console, "WGPU-BACKPRESSURE"),
                console,
            )
        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()


def compare(arm: Arm, result: Result, baseline: Result) -> list[str]:
    failures: list[str] = []
    for label, actual, expected in (
            ("v3 state", result.state, baseline.state),
            ("ordered event", result.events, baseline.events),
            ("consumed input", result.inputs, baseline.inputs)):
        if actual != expected:
            difference = next(
                (index for index, pair in enumerate(zip(expected, actual))
                 if pair[0] != pair[1]),
                min(len(expected), len(actual)),
            )
            failures.append(
                f"{arm.name}: {label} stream diverged at tick {difference}")
    if result.audio_digest != baseline.audio_digest:
        failures.append(f"{arm.name}: temporary PCM differs from original")
    if result.audio_size != baseline.audio_size or result.audio_size <= 44:
        failures.append(
            f"{arm.name}: PCM bytes={result.audio_size}, expected "
            f"{baseline.audio_size} (>44)")

    summary = result.summary
    # Absolute pacing pin (the pending/lead entries this arm's zero-list used
    # to carry): every browser arm below declares catchup=0, so a real rAF
    # timeline may still deliver its opportunities unevenly but must never
    # leave the simulation owing more than the one terminal ticket.
    conservation_error = completed_tick_conservation(
        summary, TICKS, arm.name, expected_lead=1, expected_max_pending=1)
    if conservation_error:
        failures.append(conservation_error)
    for key in ("multidue", "lag", "catchup", "skips",
                "rebases", "blocked", "updatebad"):
        if summary.get(key, 0) != 0:
            failures.append(
                f"{arm.name}: {key}={summary.get(key)}, expected 0")
    for key, expected in (
            ("presentkind", arm.effective_kind),
            ("requestedkind", arm.requested_kind),
            ("presentrate", arm.effective_rate),
            ("requestedrate", arm.requested_rate)):
        if summary.get(key) != expected:
            failures.append(
                f"{arm.name}: {key}={summary.get(key)}, expected {expected}")
    if arm.expected_presents is not None:
        for key in ("presents", "entries"):
            if summary.get(key) != arm.expected_presents:
                failures.append(
                    f"{arm.name}: {key}={summary.get(key)}, expected "
                    f"{arm.expected_presents}")
    elif not (TICKS < summary.get("presents", 0) < TICKS * 8):
        failures.append(
            f"{arm.name}: irregular presents={summary.get('presents')} is not "
            "a bounded display-rate schedule")

    for key, expected in (
            ("fields", TICKS * 2), ("due", TICKS),
            ("serviced", TICKS), ("pending", 0),
            ("retired", 0), ("rebases", 0),
            ("calls", TICKS), ("idle", 0),
            ("notready", 0), ("dropped", 0),
            ("quantumfields", 2)):
        if result.audio.get(key) != expected:
            failures.append(
                f"{arm.name}: audio {key}={result.audio.get(key)}, "
                f"expected {expected}")
    if result.audio.get("samples") != baseline.audio.get("samples"):
        failures.append(
            f"{arm.name}: audio samples={result.audio.get('samples')}, "
            f"expected {baseline.audio.get('samples')}")
    replay_expected = arm.effective_kind != 0
    if replay_expected and result.summary.get("interp", 0) <= 0:
        failures.append(f"{arm.name}: production emitted no interpolated image")
    if not replay_expected and result.summary.get("interp", -1) != 0:
        failures.append(
            f"{arm.name}: Original mode performed unexpected interpolation")
    real_endpoints = result.summary.get("realendpoints", -1)
    if arm.effective_kind == 0 and real_endpoints != 0:
        failures.append(
            f"{arm.name}: Original realendpoints={real_endpoints}, expected 0")
    if result.summary.get("replayendpoints", -1) != 0:
        failures.append(
            f"{arm.name}: replayendpoints="
            f"{result.summary.get('replayendpoints')}, expected 0")
    if result.replay.get("walks", -1) != result.summary.get("interp", 0):
        failures.append(
            f"{arm.name}: production replay walks="
            f"{result.replay.get('walks')}, expected successful interpolation "
            f"count {result.summary.get('interp', 0)}")
    if replay_expected and (
            result.retained.get("captures", 0) <= 0 or
            result.retained.get("failures", -1) != 0 or
            result.retained.get("rejects", -1) != 0 or
            result.retained.get("arenaResolve", 0) <= 0 or
            result.retained.get("externalResolve", 0) <= 0 or
            result.packet.get("futurecaptures", 0) <= 0 or
            result.packet.get("futurefailures", -1) != 0):
        failures.append(
            f"{arm.name}: retained/future publication failed: "
            f"{result.retained}")
    pressure = result.backpressure
    admission_skips = pressure.get("skips", -1)
    endpoint_skips = pressure.get("endpointSkips", -1)
    replay_skips = pressure.get("replaySkips", -1)
    if admission_skips != endpoint_skips + replay_skips:
        failures.append(
            f"{arm.name}: WebGPU skips={admission_skips} do not reconcile "
            f"with endpoint/replay={endpoint_skips}+{replay_skips}")
    if arm.effective_kind == 0:
        if pressure.get("submitted", -1) + endpoint_skips != TICKS:
            failures.append(
                f"{arm.name}: Original submitted+endpoint-skipped "
                f"{pressure.get('submitted')}+{endpoint_skips} does not "
                f"account for {TICKS} authored tasks")
    else:
        if real_endpoints + endpoint_skips != TICKS:
            failures.append(
                f"{arm.name}: authored endpoint admission "
                f"{real_endpoints}+{endpoint_skips} does not account for "
                f"{TICKS} tasks")
        if pressure.get("submitted") != (
                real_endpoints + result.summary.get("interp", 0)):
            failures.append(
                f"{arm.name}: submitted={pressure.get('submitted')} does not "
                "match successful real+interpolated images "
                f"{real_endpoints}+{result.summary.get('interp', 0)}")
        if (real_endpoints + result.summary.get("interp", 0) +
                result.summary.get("stale", 0) !=
                result.summary.get("presents", -1)):
            failures.append(
                f"{arm.name}: real+interpolated+held images do not account "
                "for every browser presentation opportunity")
        if admission_skips > result.summary.get("stale", 0):
            failures.append(
                f"{arm.name}: admission skips={admission_skips} exceed "
                f"scheduler holds={result.summary.get('stale')}")
    for key in ("failures", "inflight", "runtimewaits", "runtimewaitns"):
        if pressure.get(key, 0) != 0:
            failures.append(
                f"{arm.name}: WebGPU {key}={pressure.get(key)}")
    abandoned = pressure.get("abandoned", -1)
    completed = pressure.get("completed", -1)
    submitted = pressure.get("submitted", -1)
    # `abandoned` is teardown-only: shutdown polls the completion owner once
    # and then retires whatever the page never gave it an event-loop turn to
    # finish (gfx_webgpu.c's wgpu_abandon_in_flight). The cap alone would let
    # any value below it pass unexplained, so the count must equal what the
    # renderer itself declared it retired -- and with no such row, zero.
    retired = [int(match.group(1)) for line in result.console
               if (match := WGPU_RETIRE_RE.search(line))]
    if not (0 <= abandoned <= pressure.get("cap", 0) and
            abandoned == sum(retired) and len(retired) <= 1 and
            completed + abandoned == submitted):
        failures.append(
            f"{arm.name}: WebGPU completion/teardown accounting is invalid: "
            f"submitted={submitted} completed={completed} "
            f"abandoned={abandoned} cap={pressure.get('cap')} "
            f"declared teardown retirements={retired}")

    surface_updates = result.summary.get("surfaceupdates", -1)
    intended_updates = pressure.get("submitted", -1)
    surface_deficit = intended_updates - surface_updates
    accounted_holds = (pressure.get("held", 0) +
                       pressure.get("unavailable", 0))
    if pressure.get("presented") != surface_updates:
        failures.append(
            f"{arm.name}: canvas-update counter differs from WebGPU presented "
            "telemetry")
    if (surface_deficit < 0 or surface_deficit > accounted_holds or
            pressure.get("failures", -1) != 0):
        failures.append(
            f"{arm.name}: canvas image deficit {surface_deficit} is not accounted "
            f"by held+unavailable={accounted_holds}, or WebGPU reported a "
            "completion failure")
    for key in ("freezefail", "restorefail"):
        if result.replay.get(key, 0) != 0:
            failures.append(
                f"{arm.name}: replay {key}={result.replay.get(key)}")

    mode_rows = present_mode_rows("\n".join(result.console))
    if not any(row.get("platform") == "web"
               and row.get("reason") == "raf-ceiling" for row in mode_rows):
        failures.append(f"{arm.name}: no explicit browser rAF present-mode row")
    if arm.policy == "uncapped" and not any(
            row.get("requested") == "uncapped"
            and row.get("effective") == "display"
            and row.get("reason") == "raf-ceiling" for row in mode_rows):
        failures.append(
            "web-uncapped: native-style request was not explicitly mapped "
            "to display")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine-dir", default="build-web")
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--input-script", default=str(DEFAULT_SCRIPT))
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    engine_dir = resolve_path(args.engine_dir)
    shell_dir = resolve_path(args.shell_dir)
    rom = resolve_path(args.rom)
    script = resolve_path(args.input_script)
    chrome_path = find_chrome(args.chrome)
    for label, path in (
            ("ROM", rom), ("input fixture", script),
            ("shell index", shell_dir / "index.html"),
            ("shell JavaScript", shell_dir / "mdkr64-shell.js"),
            ("wasm loader", engine_dir / "mdkr64_web.js"),
            ("wasm module", engine_dir / "mdkr64_web.wasm"),
            ("save-tools loader", engine_dir / "mdkr-save-tools.js"),
            ("save-tools module", engine_dir / "mdkr-save-tools.wasm")):
        require(path.is_file(), f"missing {label}: {path}")
    require(rom.stat().st_size == ROM_BYTES, f"ROM is not 12 MiB: {rom}")
    fixture = script.read_text(encoding="utf-8")
    shell_source = (shell_dir / "mdkr64-shell.js").read_text(encoding="utf-8")
    fps_start = shell_source.find("function wireFpsReadout()")
    fps_end = shell_source.find("// ---- Mobile touch controller", fps_start)
    require(fps_start >= 0 and fps_end > fps_start,
            "browser FPS readout implementation is missing")
    fps_source = shell_source[fps_start:fps_end]
    require("module.__mdkrFrames" in fps_source,
            "browser FPS readout does not count engine presentations")
    require("visual fps" in fps_source,
            "browser FPS readout does not identify actual visual updates")
    require("window.requestAnimationFrame =" not in fps_source,
            "browser FPS readout still counts global rAF opportunities")

    def rational_deltas(rate: int) -> list[int]:
        return [
            ((index + 1) * 1_000_000_000) // rate
            - (index * 1_000_000_000) // rate
            for index in range(rate)
        ]

    frame_60 = rational_deltas(60)
    frame_144 = rational_deltas(144)
    arms = [
        Arm("web-original", "original", frame_60, TICKS, 0, 0),
        Arm("web-display-144", "display", frame_144, TICKS * 144 // 30, 2, 2),
        Arm("web-cap-60-on-144", "60", frame_144, TICKS * 2, 1, 1, 60, 60),
        Arm("web-uncapped", "uncapped", frame_144, TICKS * 144 // 30, 2, 3),
        Arm("web-irregular", "display",
            [8_000_000, 13_000_000, 6_000_000,
             15_000_000, 11_000_000, 9_000_000],
            None, 2, 2),
    ]
    server = OverlayServer(shell_dir, engine_dir)
    server.start()
    failures: list[str] = []
    notes: list[str] = []
    try:
        results: dict[str, Result] = {}
        for arm in arms:
            if args.verbose:
                print(f"browser arm: {arm.name}", flush=True)
            results[arm.name] = run_arm(
                server, chrome_path, rom, fixture, arm,
                args.chrome_flag, args.timeout, args.verbose)
        baseline = results[arms[0].name]
        if not (len(baseline.state) == len(baseline.events) ==
                len(baseline.inputs) == TICKS):
            failures.append(
                "web-original: state/event/input lengths are "
                f"{len(baseline.state)}/{len(baseline.events)}/"
                f"{len(baseline.inputs)}, expected {TICKS}/{TICKS}/{TICKS}")
        for arm in arms:
            result = results[arm.name]
            failures.extend(compare(arm, result, baseline))
            notes.append(
                f"{arm.name}: {result.summary.get('presents')} rAF-gated "
                f"opportunities / {result.summary.get('surfaceupdates')} "
                f"visual surface updates / {result.summary.get('ticks')} "
                "fixed ticks")
    except CheckFailure as error:
        failures.append(str(error))
    finally:
        server.close()

    if failures:
        print("check_browser_presentation_rates: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_browser_presentation_rates: PASS -- display/capped/irregular "
          "rAF schedules preserve state, events, input, and PCM")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CheckFailure as error:
        print(f"check_browser_presentation_rates: FAIL\n  - {error}")
        raise SystemExit(1)
