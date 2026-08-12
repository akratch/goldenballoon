#!/usr/bin/env python3
"""Automated gate for the adaptive mobile touch controls.

Why this exists
---------------
The touch layer shipped with only a one-off hand-driven Chromium probe
(docs/MOBILE_TOUCH_CONTROLS_2026-07-29.md), and the follow-up
preference-persistence fix was likewise hand-verified. This gate promotes both
into the registered suite using the same real-Chromium CDP harness as
`check_browser_runtime.py`.

Three arms:

1. **Desktop control** — with fine-pointer desktop metrics and no stored
   preference, the overlay and its toggle stay hidden, and the launcher still
   reaches its ready state (the capability-listener path must be non-fatal on
   every ordinary load; a regression here blanked the whole launcher on
   engines without `MediaQueryList.addEventListener`).
2. **Persisted "shown" preference** — a stored explicit `"shown"` choice
   revives the overlay after reload even though the media queries do not
   match (the fix under test: the preference is read before the capability
   gate and acts as an opt-in equivalent to `?touch=1`).
3. **Live input path** — with `?touch=1`, the real wasm engine boots the ROM
   and CDP-dispatched touches drive the overlay: a three-finger chord (stick
   deflected right + Go + Drift) must reach the game's own per-frame input
   read as `osContGetReadData P1` rows carrying the A and R bits with a
   decisive positive stick, and releasing every touch must return the
   published pad to exact neutral. A press+release completed between rAF/wasm
   samples must appear as one consumed press and release through the bounded
   JS queue. Zero page errors are tolerated.

Positive controls: arm 1 and arm 2 are each other's controls (the same
overlay probe must read hidden without the stored preference and visible with
it), and the pre-fix shell (before `dc5f83b`) demonstrably fails arm 2 — the
preference was read after the capability gate, so no stored value could
revive the overlay. Arm 3's chord predicate requires the exact A+R bits plus
a decisive stick from rows that are provably neutral before injection and
must return to exact neutral after release.

Always muted (`MDKR_AUDIO=0`); the engine runs a bounded `headlessFrames`
window. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import Any

from check_browser_runtime import (
    CDPClient,
    CheckFailure,
    ChromeProcess,
    OverlayServer,
    add_config_script,
    find_chrome,
    page_websocket,
    require,
    resolve_path,
    select_rom,
    click_play,
    wait_launcher,
    wait_value,
)

import re
import tempfile

ROM_BYTES = 12 * 1024 * 1024
PAD_RE = re.compile(
    r"osContGetReadData P1 btn=0x([0-9a-fA-F]{4}) sx=(-?\d+) sy=(-?\d+)"
)
PAD_ANY_RE = re.compile(
    r"osContGetReadData P([1-4]) btn=0x([0-9a-fA-F]{4}) "
    r"sx=(-?\d+) sy=(-?\d+)"
)
INPUT_RE = re.compile(
    r"\[INPUTHASH\].*p1=(\d),([0-9a-fA-F]{4}),"
    r"([0-9a-fA-F]{4}),([0-9a-fA-F]{4}),(-?\d+),(-?\d+)"
)
N64_A = 0x8000
N64_R = 0x0010
TOUCH_PREF = "mdkr64.touch-controls"


def overlay_state(cdp: CDPClient) -> dict[str, Any]:
    return cdp.evaluate(
        """(() => {
          const controls = document.getElementById("touch-controls");
          const toggle = document.getElementById("touch-toggle");
          return {
            haveControls: !!controls,
            controlsVisible: !!controls && !controls.hidden,
            toggleVisible: !!toggle && !toggle.hidden,
          };
        })()"""
    )


def element_center(cdp: CDPClient, selector: str) -> tuple[float, float]:
    value = cdp.evaluate(
        f"""(() => {{
          const el = document.querySelector({selector!r});
          if (!el) return null;
          const rect = el.getBoundingClientRect();
          return {{x: rect.left + rect.width / 2,
                   y: rect.top + rect.height / 2,
                   w: rect.width, h: rect.height}};
        }})()"""
    )
    require(
        isinstance(value, dict) and value.get("w", 0) > 0,
        f"touch element {selector} is missing or zero-sized",
    )
    return float(value["x"]), float(value["y"])


def dispatch_touch(
    cdp: CDPClient, kind: str, points: list[dict[str, float]]
) -> None:
    cdp.call(
        "Input.dispatchTouchEvent",
        {"type": kind, "touchPoints": points},
    )


def pad_rows(cdp: CDPClient, start: int) -> list[tuple[int, int, int]]:
    rows = []
    for line in cdp.console[start:]:
        match = PAD_RE.search(line)
        if match:
            rows.append(
                (int(match.group(1), 16),
                 int(match.group(2)),
                 int(match.group(3)))
            )
    return rows


def wait_pad(
    cdp: CDPClient,
    start: int,
    predicate,
    label: str,
    timeout: float,
) -> tuple[int, int, int]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for row in pad_rows(cdp, start):
            if predicate(row):
                return row
        time.sleep(0.1)
    raise CheckFailure(
        f"{label}: no matching pad row within {timeout:.0f}s; "
        f"last rows: {pad_rows(cdp, start)[-6:]}; "
        f"console tail: {cdp.console[-12:]}"
    )


def wait_four_remote_pads(cdp: CDPClient, start: int, timeout: float) -> None:
    expected = {
        1: (0x8000, 11), 2: (0x4000, 22),
        3: (0x2000, 33), 4: (0x0010, 44),
    }
    seen: set[int] = set()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for line in cdp.console[start:]:
            match = PAD_ANY_RE.search(line)
            if not match:
                continue
            port = int(match.group(1))
            value = (int(match.group(2), 16), int(match.group(3)))
            if value == expected[port]:
                seen.add(port)
        if seen == set(expected):
            return
        time.sleep(0.05)
    raise CheckFailure(
        f"four-phone bridge: observed matching ports {sorted(seen)}, expected 1–4"
    )


def input_rows(cdp: CDPClient, start: int) -> list[tuple[int, ...]]:
    rows = []
    for line in cdp.console[start:]:
        match = INPUT_RE.search(line)
        if match:
            rows.append((
                int(match.group(1)),
                int(match.group(2), 16),
                int(match.group(3), 16),
                int(match.group(4), 16),
                int(match.group(5)),
                int(match.group(6)),
            ))
    return rows


def wait_input(cdp: CDPClient, start: int, predicate, label: str,
               timeout: float) -> tuple[int, ...]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for row in input_rows(cdp, start):
            if predicate(row):
                return row
        time.sleep(0.05)
    raise CheckFailure(
        f"{label}: no matching [INPUTHASH] row within {timeout:.0f}s; "
        f"last rows: {input_rows(cdp, start)[-6:]}"
    )


def check_shell_arms(
    server: OverlayServer,
    chrome_path: Path,
    chrome_flags: list[str],
    timeout: float,
    verbose: bool,
) -> None:
    with tempfile.TemporaryDirectory(prefix="mdkr64_touch_shell_") as profile:
        chrome = ChromeProcess(chrome_path, Path(profile), chrome_flags, verbose)
        cdp = None
        try:
            port = chrome.wait_port()
            cdp = CDPClient(page_websocket(port))
            for domain in ("Page", "Runtime", "Log"):
                cdp.call(f"{domain}.enable")
            cdp.call(
                "Emulation.setDeviceMetricsOverride",
                {"width": 960, "height": 540,
                 "deviceScaleFactor": 1, "mobile": False},
            )
            # wait_launcher deliberately rejects a ready-looking document
            # from before a navigation. Give both shell-only arms the same
            # per-document identity contract used by the engine-backed arm;
            # Page.addScriptToEvaluateOnNewDocument persists across the
            # second navigation below and installs the token afresh there.
            add_config_script(cdp, {})

            # Arm 1: ordinary desktop load — overlay and toggle stay hidden and
            # the launcher still becomes ready (touch wiring must be non-fatal).
            cdp.call("Page.navigate", {"url": server.origin + "/"})
            wait_launcher(cdp, timeout)
            state = overlay_state(cdp)
            require(
                state["haveControls"]
                and not state["controlsVisible"]
                and not state["toggleVisible"],
                f"desktop control arm: overlay unexpectedly active: {state}",
            )

            # Arm 2: a persisted explicit "shown" must revive the overlay on
            # the same non-touch environment after reload.
            cdp.evaluate(
                f"localStorage.setItem({TOUCH_PREF!r}, 'shown')"
            )
            cdp.call("Page.navigate", {"url": server.origin + "/"})
            wait_launcher(cdp, timeout)
            state = wait_value(
                cdp,
                """(() => {
                  const controls = document.getElementById("touch-controls");
                  const toggle = document.getElementById("touch-toggle");
                  return {controlsVisible: !!controls && !controls.hidden,
                          toggleVisible: !!toggle && !toggle.hidden};
                })()""",
                lambda item: isinstance(item, dict)
                and item.get("controlsVisible")
                and item.get("toggleVisible"),
                "persisted-shown overlay revival",
                timeout,
            )
            cdp.evaluate(f"localStorage.removeItem({TOUCH_PREF!r})")
        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()


def check_input_arm(
    server: OverlayServer,
    chrome_path: Path,
    chrome_flags: list[str],
    rom: Path,
    frames: int,
    timeout: float,
    verbose: bool,
) -> None:
    with tempfile.TemporaryDirectory(prefix="mdkr64_touch_game_") as profile:
        chrome = ChromeProcess(chrome_path, Path(profile), chrome_flags, verbose)
        cdp = None
        try:
            port = chrome.wait_port()
            cdp = CDPClient(page_websocket(port))
            for domain in ("Page", "Runtime", "Network", "Log", "Inspector"):
                cdp.call(f"{domain}.enable")
            cdp.call(
                "Emulation.setDeviceMetricsOverride",
                {"width": 960, "height": 540,
                 "deviceScaleFactor": 1, "mobile": True},
            )
            add_config_script(
                cdp,
                {
                    "headlessFrames": frames,
                    "env": {
                        "MDKR_TRACE": "3",
                        "MDKR_AUDIO": "0",
                        "MDKR_INPUT_HASH": "1",
                    },
                },
            )
            cdp.call(
                "Page.navigate",
                {"url": server.origin + "/?touch=1"},
            )
            wait_launcher(cdp, timeout)
            state = overlay_state(cdp)
            require(
                state["controlsVisible"],
                f"?touch=1 did not activate the overlay: {state}",
            )
            select_rom(cdp, rom, timeout)
            click_play(cdp)

            # The engine publishes one P1 row per frame at MDKR_TRACE=3; the
            # first row proves the game loop is sampling input.
            wait_pad(cdp, 0, lambda row: True,
                     "engine input sampling", timeout)

            stick_x, stick_y = element_center(cdp, "#touch-stick")
            go_x, go_y = element_center(cdp, ".touch-go")
            drift_x, drift_y = element_center(cdp, ".touch-drift")
            stick_radius = cdp.evaluate(
                """(() => {
                  const el = document.getElementById("touch-stick");
                  const rect = el.getBoundingClientRect();
                  return Math.max(24, Math.min(rect.width, rect.height) * 0.31);
                })()"""
            )
            deflect = float(stick_radius) * 0.95

            chord = [
                {"x": stick_x, "y": stick_y, "id": 1},
                {"x": go_x, "y": go_y, "id": 2},
                {"x": drift_x, "y": drift_y, "id": 3},
            ]
            dispatch_touch(cdp, "touchStart", chord)
            moved = [
                {"x": stick_x + deflect, "y": stick_y, "id": 1},
                {"x": go_x, "y": go_y, "id": 2},
                {"x": drift_x, "y": drift_y, "id": 3},
            ]
            dispatch_touch(cdp, "touchMove", moved)

            marker = len(cdp.console)
            chord_row = wait_pad(
                cdp,
                marker,
                lambda row: (row[0] & (N64_A | N64_R)) == (N64_A | N64_R)
                and row[1] >= 60,
                "three-finger chord (A+R + decisive right stick)",
                timeout,
            )
            if verbose:
                print(f"chord row: btn=0x{chord_row[0]:04x} "
                      f"sx={chord_row[1]} sy={chord_row[2]}", flush=True)

            dispatch_touch(cdp, "touchEnd", [])
            marker = len(cdp.console)
            wait_pad(
                cdp,
                marker,
                lambda row: row == (0, 0, 0),
                "exact neutral after releasing every touch",
                timeout,
            )

            # Slide-to-chord: ONE finger holds the throttle on Go, slides onto
            # Drift (A+R must appear without the throttle ever dropping),
            # slides back (R releases, A continues), then lifts (neutral).
            # This is the two-thumb design's core contract.
            dispatch_touch(cdp, "touchStart",
                           [{"x": go_x, "y": go_y, "id": 7}])
            marker = len(cdp.console)
            wait_pad(
                cdp, marker,
                lambda row: (row[0] & N64_A) == N64_A
                and (row[0] & N64_R) == 0,
                "slide arm: throttle-only on Go", timeout,
            )
            dispatch_touch(cdp, "touchMove",
                           [{"x": drift_x, "y": drift_y, "id": 7}])
            marker = len(cdp.console)
            wait_pad(
                cdp, marker,
                lambda row: (row[0] & (N64_A | N64_R)) == (N64_A | N64_R),
                "slide arm: A+R chord after sliding onto Drift", timeout,
            )
            slid_rows = pad_rows(cdp, 0)
            dispatch_touch(cdp, "touchMove",
                           [{"x": go_x, "y": go_y, "id": 7}])
            marker = len(cdp.console)
            wait_pad(
                cdp, marker,
                lambda row: (row[0] & N64_A) == N64_A
                and (row[0] & N64_R) == 0,
                "slide arm: R released, throttle retained after sliding back",
                timeout,
            )
            dispatch_touch(cdp, "touchEnd", [])
            marker = len(cdp.console)
            wait_pad(
                cdp, marker,
                lambda row: row == (0, 0, 0),
                "slide arm: neutral after lift", timeout,
            )

            # Stuck-throttle regression arm (the shipped Safari defect):
            # a thumb that wanders far OFF the pad and lifts there must still
            # release — the window-level pointer lifecycle owns the release,
            # never element hit-testing or pointer capture.
            dispatch_touch(cdp, "touchStart",
                           [{"x": go_x, "y": go_y, "id": 9}])
            marker = len(cdp.console)
            wait_pad(
                cdp, marker,
                lambda row: (row[0] & N64_A) == N64_A,
                "off-pad arm: throttle engaged", timeout,
            )
            dispatch_touch(cdp, "touchMove",
                           [{"x": 200.0, "y": 200.0, "id": 9}])
            dispatch_touch(cdp, "touchEnd", [])
            marker = len(cdp.console)
            wait_pad(
                cdp, marker,
                lambda row: row == (0, 0, 0),
                "off-pad arm: releasing far outside the pad must neutralize "
                "(stuck-Go regression)", timeout,
            )
            require(
                all((row[0] & N64_A) == N64_A
                    for row in slid_rows[-8:] if row != (0, 0, 0)),
                "slide arm: the throttle dropped during the slide "
                f"({slid_rows[-8:]})",
            )

            # Edge-queue arm: press and release Go back-to-back, without
            # waiting for wasm/rAF between the browser callbacks. The engine's
            # tick trace must still expose one A press followed by one release;
            # a latest-state-only sampler would see neutral twice and fail.
            marker = len(cdp.console)
            dispatch_touch(cdp, "touchStart",
                           [{"x": go_x, "y": go_y, "id": 11}])
            dispatch_touch(cdp, "touchEnd", [])
            wait_input(
                cdp, marker,
                lambda row: (row[1] & N64_A) != 0
                and (row[2] & N64_A) != 0,
                "between-rAF Go tap press", timeout,
            )
            wait_input(
                cdp, marker,
                lambda row: (row[1] & N64_A) == 0
                and (row[3] & N64_A) != 0,
                "between-rAF Go tap release", timeout,
            )
            queue_state = cdp.evaluate(
                "(() => { const s = globalThis.__mdkrTestState; "
                "const p = s && s.module && s.module.__mdkrTouchPad; "
                "return p ? {depth: p.events.length, overflow: p.overflow} "
                ": null; })()"
            )
            require(
                isinstance(queue_state, dict)
                and queue_state.get("depth", 999) <= 128
                and queue_state.get("overflow") == 0,
                f"touch edge queue is not bounded/clean: {queue_state}",
            )

            # In-game controller management is a real local overlay boundary:
            # opening it releases a held touch, C neutralizes the routed port,
            # and closing cannot resurrect the pre-overlay throttle. This uses
            # the linked wasm implementation of platformOverlayWantsPause/Input,
            # not merely the DOM latch checked by check_party_host.py.
            dispatch_touch(cdp, "touchStart",
                           [{"x": go_x, "y": go_y, "id": 12}])
            marker = len(cdp.console)
            wait_pad(cdp, marker, lambda row: (row[0] & N64_A) == N64_A,
                     "pre-overlay held throttle", timeout)
            cdp.evaluate("globalThis.MDKRPartyHost.open()")
            overlay = wait_value(cdp, """({
              open:globalThis.__mdkrPartyOverlayOpen,
              dialog:document.getElementById('party-dialog').open
            })""", lambda value: isinstance(value, dict) and
                value.get("open") is True and value.get("dialog") is True,
                "linked-wasm Party overlay", timeout)
            require(overlay["open"], f"Party overlay did not open: {overlay}")
            marker = len(cdp.console)
            wait_pad(cdp, marker, lambda row: row == (0, 0, 0),
                     "Party overlay fail-neutral", timeout)
            cdp.evaluate("document.getElementById('party-close').click()")
            wait_value(cdp, "!globalThis.__mdkrPartyOverlayOpen", bool,
                       "Party overlay release", timeout)
            marker = len(cdp.console)
            wait_pad(cdp, marker, lambda row: row == (0, 0, 0),
                     "no resurrected throttle after Party overlay", timeout)
            dispatch_touch(cdp, "touchEnd", [])

            # Phone-controller wasm bridge: publish one real v1 binary packet
            # through the launcher-owned queue. C must claim the seat, decode
            # it with remote_pad.c, expose it to osContGetReadData, then expire
            # it to exact neutral after 250 ms without a refresh.
            marker = len(cdp.console)
            injected = cdp.evaluate("""(() => {
              const pads = globalThis.MDKRPartyHost.remotePads();
              Object.assign(pads[0], {active:true, owner:123,
                connectionSequence:1});
              pads[0].packets.length = 0;
              pads[0].packets.push(globalThis.MDKRPartyProtocol.encode({
                flags:1, connectionSequence:1, sampleSequence:1,
                senderTimeMs:Math.floor(performance.now())>>>0,
                buttons:32768, stickX:42, stickY:0, edges:[]
              }));
              return {same:pads === globalThis.__mdkrTestState.module.__mdkrRemotePads,
                depth:pads[0].packets.length};
            })()""")
            require(injected == {"same": True, "depth": 1},
                    f"launcher/wasm remote queue was not shared: {injected}")
            wait_pad(cdp, marker,
                     lambda row: (row[0] & N64_A) == N64_A and row[1] == 42,
                     "validated phone packet at P1", timeout)
            marker = len(cdp.console)
            wait_pad(cdp, marker, lambda row: row == (0, 0, 0),
                     "250 ms remote-phone fail-neutral", timeout)
            # Transport loss must not silently give an approved phone's slot
            # to the keyboard/touch fallback. Keep the lease reserved, press
            # local Go, and require a sustained neutral stream; only explicit
            # host release may make the local source visible again.
            cdp.evaluate("""(() => {
              const p=globalThis.MDKRPartyHost.remotePads()[0];
              p.active=false; p.reserved=true;
            })()""")
            marker = len(cdp.console)
            dispatch_touch(cdp, "touchStart",
                           [{"x": go_x, "y": go_y, "id": 13}])
            time.sleep(0.35)
            reserved_rows = pad_rows(cdp, marker)
            require(reserved_rows and all((row[0] & N64_A) == 0
                                          for row in reserved_rows),
                    "local throttle took over a reconnecting phone lease: "
                    f"{reserved_rows[-8:]}")
            marker = len(cdp.console)
            cdp.evaluate("globalThis.MDKRPartyHost.remotePads()[0].reserved=false")
            wait_pad(cdp, marker, lambda row: (row[0] & N64_A) == N64_A,
                     "explicit phone-seat release to local touch", timeout)
            dispatch_touch(cdp, "touchEnd", [])
            marker = len(cdp.console)
            wait_pad(cdp, marker, lambda row: row == (0, 0, 0),
                     "post-release local touch neutral", timeout)
            remote_state = cdp.evaluate("""(() => {
              const p=globalThis.MDKRPartyHost.remotePads()[0];
              p.active=false; p.reserved=false;
              return {depth:p.packets.length,drops:p.drops};
            })()""")
            require(remote_state["depth"] == 0 and remote_state["drops"] == 0,
                    f"remote queue did not drain cleanly: {remote_state}")

            # Four-seat bridge arm. One launcher queue per approved phone must
            # reach the matching N64 port without OR-ing sources or collapsing
            # every phone onto P1.
            marker = len(cdp.console)
            four = cdp.evaluate("""(() => {
              const buttons=[32768,16384,8192,16];
              const sticks=[11,22,33,44];
              const pads=globalThis.MDKRPartyHost.remotePads();
              for(let i=0;i<4;i++) {
                Object.assign(pads[i], {active:true, owner:500+i,
                  connectionSequence:7, drops:0});
                pads[i].packets.length=0;
                pads[i].packets.push(globalThis.MDKRPartyProtocol.encode({
                  flags:1, connectionSequence:7, sampleSequence:10+i,
                  senderTimeMs:Math.floor(performance.now())>>>0,
                  buttons:buttons[i], stickX:sticks[i], stickY:0, edges:[]
                }));
              }
              return pads.map(p => p.packets.length);
            })()""")
            require(four == [1, 1, 1, 1],
                    f"four-phone queues were not independently populated: {four}")
            wait_four_remote_pads(cdp, marker, timeout)
            cdp.evaluate("""(() => {
              for(const pad of globalThis.MDKRPartyHost.remotePads()) {
                pad.active=false; pad.packets.length=0;
              }
            })()""")

            # The shell routes wasm stderr into testState.errors, so trace
            # rows land there under MDKR_TRACE; only genuine failure markers
            # count.
            errors = cdp.evaluate(
                "globalThis.__mdkrTestState ? "
                "globalThis.__mdkrTestState.errors.slice() : null"
            )
            require(isinstance(errors, list), "test state never initialized")
            bad = [
                line for line in errors
                if any(marker in line for marker in (
                    "[CRASH]", "[FATAL]", "Uncaught", "RuntimeError",
                    "abort(", "device lost", "validation error",
                ))
            ]
            require(
                bad == [],
                f"page recorded failure markers during the touch arm: {bad}",
            )
        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-dir", default="build-web")
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--frames", type=int, default=2200,
                        help="bounded engine window for the input arm")
    parser.add_argument("--timeout", type=float, default=150.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    try:
        rom = resolve_path(args.rom)
        shell_dir = resolve_path(args.shell_dir)
        engine_dir = resolve_path(args.engine_dir)
        for label, path in (
            ("ROM", rom),
            ("shell index", shell_dir / "index.html"),
            ("shell JavaScript", shell_dir / "mdkr64-shell.js"),
            ("wasm loader", engine_dir / "mdkr64_web.js"),
            ("wasm module", engine_dir / "mdkr64_web.wasm"),
        ):
            require(path.is_file(), f"missing {label}: {path}")
        require(rom.stat().st_size == ROM_BYTES, f"ROM is not 12 MiB: {rom}")
        chrome_path = find_chrome(args.chrome)

        server = OverlayServer(shell_dir, engine_dir)
        server.start()
        if args.verbose:
            print(f"browser server: {server.origin}", flush=True)

        check_shell_arms(
            server, chrome_path, args.chrome_flag, args.timeout, args.verbose
        )
        check_input_arm(
            server, chrome_path, args.chrome_flag, rom,
            args.frames, args.timeout, args.verbose,
        )
    except (CheckFailure, OSError, ValueError) as exc:
        print(f"check_touch_controls: FAIL — {exc}", file=sys.stderr)
        return 1
    print(
        "check_touch_controls: PASS — desktop load keeps the overlay hidden "
        "with a live launcher; a persisted 'shown' revives it without touch "
        "media queries; a CDP three-finger chord reaches osContGetReadData "
        "P1 with A+R plus a decisive stick; and a one-finger slide from Go "
        "onto Drift and back chords A+R then releases R without the throttle "
        "ever dropping; the in-game Party modal neutralizes and cannot "
        "resurrect held input; a reconnecting phone lease stays neutral until "
        "explicit release; a press+release between rAF callbacks survives as one "
        "consumed press/release; all paths end neutral with a bounded clean "
        "queue; four independent phone queues reach P1–P4; and the run ends "
        "with zero page errors"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
