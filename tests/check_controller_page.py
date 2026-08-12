#!/usr/bin/env python3
"""Exercise the standalone, engine-free phone controller in real Chromium."""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from check_browser_runtime import (
    CDPClient,
    ChromeProcess,
    CheckFailure,
    OverlayServer,
    find_chrome,
    page_websocket,
    require,
    wait_value,
)


ROOT = Path(__file__).resolve().parent.parent


def resolve(value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    return path.resolve()


def run(args: argparse.Namespace) -> None:
    shell = resolve(args.shell_dir)
    chrome_path = find_chrome(args.chrome)
    required = (
        shell / "controller" / "index.html",
        shell / "controller" / "controller.css",
        shell / "controller" / "controller.js",
        shell / "input" / "touch-surface.js",
        shell / "party" / "party-protocol.js",
        shell / "party" / "party-sas.js",
        shell / "_headers",
    )
    for path in required:
        require(path.is_file(), f"controller artifact is missing: {path}")
    critical_bytes = sum(path.stat().st_size for path in required[:6])
    require(critical_bytes < 100 * 1024,
            f"controller critical path is {critical_bytes} bytes, budget is 100 KiB")
    headers = (shell / "_headers").read_text(encoding="utf-8")
    for value in ("frame-ancestors 'none'", "Referrer-Policy: no-referrer",
                  "X-Content-Type-Options: nosniff", "Cache-Control: no-store"):
        require(value in headers, f"controller header contract lacks {value!r}")

    server = OverlayServer(shell, shell)
    server.start()
    with tempfile.TemporaryDirectory(prefix="mdkr64_controller_profile_") as profile:
        chrome = ChromeProcess(chrome_path, Path(profile), args.chrome_flag, args.verbose)
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Inspector", "Accessibility"):
                cdp.call(f"{domain}.enable")
            config = {
                "capability": "controller-test-capability",
                "phrase": "Swift Balloon",
                "seat": 3,
                "autoApprove": True,
            }
            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source":
                "globalThis.__mdkrControllerTestConfig=" +
                json.dumps(config, separators=(",", ":")) + ";"})
            cdp.call("Emulation.setDeviceMetricsOverride", {
                "width": 320, "height": 568, "deviceScaleFactor": 2,
                "mobile": True,
            })
            secret = "AbCdEfGhIjKlMnOpQrStUv"
            cdp.call("Page.navigate", {
                "url": server.origin + "/controller/#" + secret})

            assigned = wait_value(
                cdp,
                """(() => ({
                  phase: globalThis.__mdkrControllerTest &&
                    globalThis.__mdkrControllerTest.state().phase,
                  hash: location.hash,
                  title: document.getElementById("assigned-title")?.textContent,
                  phrase: document.getElementById("pairing-phrase")?.textContent,
                  overflow: document.documentElement.scrollWidth > innerWidth
                }))()""",
                lambda value: isinstance(value, dict) and value.get("phase") == "assigned",
                "approved controller state", args.timeout)
            require(assigned["hash"] == "", "bearer fragment remained in the address bar")
            require(assigned["title"] == "Controller 3", f"seat copy: {assigned}")
            require(assigned["phrase"] == "Swift Balloon", f"phrase copy: {assigned}")
            require(not assigned["overflow"], "320×568 controller layout overflows horizontally")

            cdp.evaluate('document.getElementById("input-test").click()')
            wait_value(cdp,
                "!document.getElementById('use-controller').disabled", bool,
                "input test success", args.timeout)
            cdp.evaluate('document.getElementById("use-controller").click()')
            wait_value(cdp,
                "globalThis.__mdkrControllerTest.state().phase",
                lambda value: value == "controller", "controller surface", args.timeout)

            # Real Pointer Events through the shared surface: Go -> Drift slide,
            # second-finger Item chord, then cancellation to exact neutral.
            result = cdp.evaluate("""(() => {
              const actions = document.querySelector('.touch-actions');
              const go = document.querySelector('.touch-go').getBoundingClientRect();
              const drift = document.querySelector('.touch-drift').getBoundingClientRect();
              const item = document.querySelector('.touch-item').getBoundingClientRect();
              const send = (target, type, id, rect) => target.dispatchEvent(
                new PointerEvent(type, {pointerId:id, pointerType:'touch',
                  clientX:(rect.left+rect.right)/2, clientY:(rect.top+rect.bottom)/2,
                  bubbles:true, cancelable:true}));
              send(actions, 'pointerdown', 11, go);
              const goBits = globalThis.__mdkrControllerTest.state().pad.buttons;
              send(window, 'pointermove', 11, drift);
              const driftBits = globalThis.__mdkrControllerTest.state().pad.buttons;
              send(actions, 'pointerdown', 12, item);
              const chordBits = globalThis.__mdkrControllerTest.state().pad.buttons;
              send(window, 'pointercancel', 12, item);
              send(window, 'pointercancel', 11, drift);
              const neutralBits = globalThis.__mdkrControllerTest.state().pad.buttons;
              return {goBits, driftBits, chordBits, neutralBits,
                packets: globalThis.__mdkrControllerTestState.packets.length};
            })()""")
            require(result.get("goBits") == 32768, f"Go did not publish A: {result}")
            require(result.get("driftBits") == 32768 | 16, f"Drift chord: {result}")
            require(result.get("chordBits") == 32768 | 16 | 8192,
                    f"three-button chord: {result}")
            require(result.get("neutralBits") == 0 and result.get("packets", 0) >= 5,
                    f"controller did not return neutral: {result}")

            keyboard_stick = cdp.evaluate("""(() => {
              const stick = document.getElementById('phone-touch-stick');
              const send = (type, key) => stick.dispatchEvent(new KeyboardEvent(type,
                {key, bubbles:true, cancelable:true}));
              stick.focus();
              send('keydown', 'ArrowUp');
              const up = {...globalThis.__mdkrControllerTest.state().pad};
              send('keydown', 'd');
              const diagonal = {...globalThis.__mdkrControllerTest.state().pad};
              const direction = stick.getAttribute('aria-valuetext');
              send('keyup', 'ArrowUp');
              send('keyup', 'd');
              const neutral = {...globalThis.__mdkrControllerTest.state().pad};
              return {up, diagonal, direction, neutral};
            })()""")
            require(keyboard_stick["up"]["stickY"] == 80 and
                    keyboard_stick["up"]["stickX"] == 0,
                    f"keyboard up did not steer: {keyboard_stick}")
            require(keyboard_stick["diagonal"]["stickX"] == 57 and
                    keyboard_stick["diagonal"]["stickY"] == 57 and
                    keyboard_stick["direction"] == "Up right",
                    f"keyboard diagonal was not bounded/described: {keyboard_stick}")
            require(keyboard_stick["neutral"]["stickX"] == 0 and
                    keyboard_stick["neutral"]["stickY"] == 0,
                    f"keyboard steering did not return neutral: {keyboard_stick}")

            reconnect = cdp.evaluate("""(() => {
              globalThis.__mdkrControllerTest.reconnect(2);
              const before = globalThis.__mdkrControllerTest.state();
              globalThis.__mdkrControllerTest.reconnectComplete(9);
              return {before, after: globalThis.__mdkrControllerTest.state(),
                neutralizations: globalThis.__mdkrControllerTestState.neutralizations};
            })()""")
            require(reconnect["before"]["phase"] == "reconnecting" and
                    reconnect["before"]["pad"]["buttons"] == 0,
                    f"reconnect did not neutralize: {reconnect}")
            require(reconnect["after"]["phase"] == "controller" and
                    reconnect["after"]["connectionSequence"] == 9,
                    f"reconnect epoch did not advance: {reconnect}")

            cdp.evaluate("document.getElementById('settings-open').click()")
            wait_value(cdp, "document.getElementById('settings-dialog').open", bool,
                       "controller settings", args.timeout)
            cdp.evaluate("document.getElementById('leave-controller').click()")
            leave_prompt = wait_value(cdp, """(() => ({
              open:document.getElementById('leave-dialog').open,
              phase:globalThis.__mdkrControllerTest.state().phase,
              focus:document.activeElement?.id
            }))()""", lambda value: isinstance(value, dict) and
                value.get("open") is True, "controller leave confirmation", args.timeout)
            require(leave_prompt["phase"] == "controller" and
                    leave_prompt["focus"] == "leave-cancel",
                    f"controller leave confirmation was not safe by default: {leave_prompt}")
            cdp.evaluate("document.getElementById('leave-cancel').click()")
            wait_value(cdp, "!document.getElementById('leave-dialog').open", bool,
                       "cancelled controller leave", args.timeout)
            require(cdp.evaluate("globalThis.__mdkrControllerTest.state().phase") ==
                    "controller", "cancelling leave disconnected the controller")

            cdp.evaluate("""(() => {
              globalThis.__mdkrControllerTest.showCode();
              const input = document.getElementById('room-code');
              input.value = '123456';
              input.dispatchEvent(new Event('input', {bubbles:true}));
              document.getElementById('code-form').requestSubmit();
            })()""")
            code_join = wait_value(cdp,
                "globalThis.__mdkrControllerTest.state().phase",
                lambda value: value == "assigned", "fallback-code approval", args.timeout)
            require(code_join == "assigned", "fallback code did not reach assigned state")

            cdp.call("Emulation.setPageScaleFactor", {"pageScaleFactor": 2})
            scaled = cdp.evaluate("({w:document.documentElement.scrollWidth, v:innerWidth})")
            require(scaled["w"] <= scaled["v"], f"200% layout overflowed: {scaled}")

            cdp.evaluate("""(() => {
              document.querySelector('#state-assigned [data-action="leave"]').click();
            })()""")
            wait_value(cdp, "document.getElementById('leave-dialog').open", bool,
                       "assigned leave confirmation", args.timeout)
            require(cdp.evaluate("globalThis.__mdkrControllerTest.state().phase") ==
                    "assigned", "leave prompt released the controller before confirmation")
            cdp.evaluate("document.getElementById('leave-confirm').click()")
            left = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest.state().phase,
              title:document.getElementById('error-title').textContent,
              neutral:globalThis.__mdkrControllerTest.state().pad.buttons
            }))()""", lambda value: isinstance(value, dict) and
                value.get("phase") == "error", "confirmed controller leave", args.timeout)
            require(left["title"] == "Controller disconnected" and left["neutral"] == 0,
                    f"confirmed leave lacked truthful neutral recovery: {left}")

            paths = [request.path for request in server.requests]
            forbidden = ("mdkr64_web", ".wasm", "/rom", "/save", "hero.jpg")
            require(not any(any(word in request for word in forbidden) for request in paths),
                    f"controller requested engine/private assets: {paths}")
            require(not any(secret in request for request in paths),
                    f"HTTP request leaked capability fragment: {paths}")
            require(not cdp.failures, "browser/CDP failures: " + "; ".join(cdp.failures))
            fatal = [line for line in cdp.console if "Uncaught" in line or "TypeError" in line]
            require(not fatal, "controller console errors: " + "; ".join(fatal))
            print(f"check_controller_page: PASS — {critical_bytes // 1024} KiB engine-free page, fragment erased, "
                  "approval/test/controller/reconnect/confirmed-leave UX, three-finger chord, neutral safety, "
                  "bounded Arrow/WASD steering, 320×568/200% layout and private-asset boundary")
        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()
            server.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError) as error:
        print(f"check_controller_page: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
