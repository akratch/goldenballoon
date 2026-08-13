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


def load_controller(cdp: CDPClient, url: str, label: str, timeout: float) -> None:
    """Force a fresh controller document at ``url``.

    ``Page.navigate`` from ``/controller/`` to ``/controller/#<secret>`` is a
    same-document fragment navigation: no document is created, so neither the
    entry code nor the pending ``Page.addScriptToEvaluateOnNewDocument``
    configuration ever runs and the check would silently observe the previous
    document.  Detour through ``about:blank`` so each entry case really is a
    fresh load.
    """
    cdp.call("Page.navigate", {"url": "about:blank"})
    wait_value(cdp, "location.href", lambda value: value == "about:blank",
               f"{label} blank detour", timeout)
    cdp.call("Page.navigate", {"url": url})


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
                json.dumps(config, separators=(",", ":")) + ";" +
                "globalThis.__copiedControllerLink='';" +
                "globalThis.__sharedControllerLink=null;" +
                "Object.defineProperty(navigator,'clipboard',{configurable:true," +
                "value:{writeText:async(value)=>{globalThis.__copiedControllerLink=value;}}});" +
                "Object.defineProperty(navigator,'share',{configurable:true," +
                "value:async(value)=>{globalThis.__sharedControllerLink=value;}});"})
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

            lifecycle_resume = cdp.evaluate("""(() => {
              dispatchEvent(new Event('freeze'));
              const frozen=globalThis.__mdkrControllerTest.state();
              dispatchEvent(new Event('resume'));
              const resumed=globalThis.__mdkrControllerTest.state();
              const packet=globalThis.__mdkrControllerTestState.packets.at(-1).decoded;
              return {frozen,resumed,packet};
            })()""")
            require(lifecycle_resume["frozen"]["active"] is False and
                    lifecycle_resume["frozen"]["pad"] == {
                        "buttons": 0, "stickX": 0, "stickY": 0} and
                    lifecycle_resume["resumed"]["active"] is True and
                    lifecycle_resume["resumed"]["phase"] == "controller" and
                    lifecycle_resume["packet"]["buttons"] == 0 and
                    lifecycle_resume["packet"]["edges"] == [],
                    f"freeze/resume left dead controls or replayed input: "
                    f"{lifecycle_resume}")

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

            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source":
                     "globalThis.__mdkrControllerTestConfig.entryMode='embedded';"})
            load_controller(cdp, server.origin + "/controller/#" + secret,
                            "embedded entry", args.timeout)
            embedded_copy = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              heading:document.getElementById('error-title')?.textContent,
              primary:document.getElementById('error-recovery')?.textContent,
              copy:document.getElementById('copy-link')?.textContent,
              copyHidden:document.getElementById('copy-link')?.hidden,
              hash:location.hash}))()""", lambda value: isinstance(value, dict) and
                  value.get("phase") == "error", "embedded-browser recovery",
                  args.timeout)
            require(embedded_copy == {
                        "phase": "error", "heading": "Continue in Safari or Chrome",
                        "primary": "Share private link", "copy": "Copy private link",
                        "copyHidden": False, "hash": ""},
                    f"embedded-browser recovery copy was misleading: {embedded_copy}")
            cdp.evaluate("document.getElementById('error-recovery').click()")
            shared = wait_value(cdp, "globalThis.__sharedControllerLink",
                lambda value: isinstance(value, dict),
                "embedded-browser private-link share", args.timeout)
            require(shared == {"title": "Golden Balloon phone controller",
                        "text": "Open this private controller link in Safari or Chrome.",
                        "url": server.origin +
                            "/controller/#controller-test-capability"},
                    f"private share sheet lost capability context: {shared}")
            cdp.evaluate("document.getElementById('copy-link').click()")
            copied = wait_value(cdp, "globalThis.__copiedControllerLink",
                lambda value: isinstance(value, str) and value.endswith(
                    "/controller/#controller-test-capability"),
                "embedded-browser private-link copy", args.timeout)
            require(copied == server.origin +
                        "/controller/#controller-test-capability",
                    f"copy recovery lost the private link: {copied}")

            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source":
                     "globalThis.__mdkrControllerTestConfig.entryMode='duplicate';"})
            load_controller(cdp, server.origin + "/controller/#" + secret,
                            "duplicate entry", args.timeout)
            duplicate = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              hash:location.hash,
              invite:globalThis.__mdkrControllerTest?.inviteUrl()}))()""",
                lambda value: isinstance(value, dict) and
                    value.get("phase") == "duplicate", "duplicate-tab recovery",
                    args.timeout)
            require(duplicate == {"phase": "duplicate", "hash": "",
                        "invite": server.origin +
                            "/controller/#controller-test-capability"},
                    f"duplicate-tab reclaim lost its private navigation: {duplicate}")
            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source":
                     "globalThis.__mdkrControllerTestConfig.entryMode='';"})
            cdp.evaluate("document.getElementById('reclaim-controller').click()")
            reclaimed = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              hash:location.hash,href:location.href}))()""",
                lambda value: isinstance(value, dict) and
                    value.get("phase") == "assigned",
                "duplicate-tab acknowledged takeover", args.timeout)
            require(reclaimed == {"phase": "assigned", "hash": "",
                        "href": server.origin + "/controller/"},
                    f"duplicate-tab takeover did not scrub/redeem once: {reclaimed}")

            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source":
                     "globalThis.__mdkrControllerTestConfig.forceFallbackLease=true;"})
            load_controller(cdp, server.origin + "/controller/#" + secret,
                            "fallback-lease entry", args.timeout)
            fallback_lease = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              mode:globalThis.__mdkrControllerTestState?.leaseMode,
              keys:Object.keys(localStorage).filter(
                key=>key==='gb-controller-tab-lease'),
              storage:Object.entries(localStorage).map(([key,value])=>key+value)
                .join('|')}))()""", lambda value: isinstance(value, dict) and
                  value.get("phase") == "assigned",
                  "no-Web-Locks controller lease", args.timeout)
            require(fallback_lease["mode"] == "broadcast-storage" and
                    len(fallback_lease["keys"]) == 1 and
                    "controller-test-capability" not in fallback_lease["storage"],
                    f"fallback lease exposed a secret or was not acquired: "
                    f"{fallback_lease}")

            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": """
              globalThis.__mdkrControllerTestConfig.entryMode='';
              globalThis.__mdkrControllerTestConfig.codeMode=true;
              Object.defineProperty(History.prototype, 'replaceState', {
                configurable:true,
                value(){throw new DOMException('fixture denial', 'SecurityError');}
              });
            """})
            load_controller(cdp, server.origin + "/controller/#" + secret,
                            "scrub-denial entry", args.timeout)
            scrub_denied = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              hash:location.hash,href:location.href,
              leases:Object.keys(localStorage).filter(
                key=>key==='gb-controller-tab-lease').length}))()""",
                lambda value: isinstance(value, dict) and
                    value.get("phase") == "code" and value.get("hash") == "",
                "controller History API denial", args.timeout)
            require(scrub_denied["href"] == server.origin + "/controller/",
                    f"scrub denial retained or redeemed the capability: {scrub_denied}")
            require(scrub_denied["leases"] == 0,
                    f"pagehide retained the fallback tab lease: {scrub_denied}")
            public_recovery = cdp.evaluate("""(() => {
              globalThis.__copiedControllerLink='';
              globalThis.__sharedControllerLink=null;
              globalThis.__mdkrControllerTest.showEmbedded();
              const copy=document.getElementById('copy-link');
              const result={primary:document.getElementById('error-recovery').textContent,
                copy:copy.textContent,copyHidden:copy.hidden};
              copy.click();
              return result;
            })()""")
            require(public_recovery == {"primary": "Share controller page",
                        "copy": "Copy controller page", "copyHidden": False},
                    f"capability-free browser recovery was misleading: "
                    f"{public_recovery}")
            cdp.evaluate("document.getElementById('error-recovery').click()")
            public_shared = wait_value(cdp, "globalThis.__sharedControllerLink",
                lambda value: isinstance(value, dict),
                "capability-free controller page share", args.timeout)
            require(public_shared == {
                        "title": "Golden Balloon phone controller",
                        "text": "Open this controller page in Safari or Chrome, then enter the current room code.",
                        "url": server.origin + "/controller/"},
                    f"public share sheet implied a retained invitation: {public_shared}")
            wait_value(cdp, "globalThis.__copiedControllerLink",
                       lambda value: value == server.origin + "/controller/",
                       "capability-free controller page copy", args.timeout)

            cdp.evaluate("""(() => {
              dispatchEvent(new PageTransitionEvent('pagehide', {persisted:true}));
              dispatchEvent(new PageTransitionEvent('pageshow', {persisted:true}));
            })()""")
            restored = wait_value(cdp, """(() => ({
              phase:globalThis.__mdkrControllerTest?.state().phase,
              hash:location.hash,href:location.href,
              leases:Object.keys(localStorage).filter(
                key=>key==='gb-controller-tab-lease').length}))()""",
                lambda value: isinstance(value, dict) and
                    value.get("phase") == "code",
                "controller BFCache recovery reload", args.timeout)
            require(restored == {"phase": "code", "hash": "",
                        "href": server.origin + "/controller/", "leases": 0},
                    f"BFCache restore revived dead controller custody: {restored}")

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
                  "copy/reclaim/scrub-denial recovery, "
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
