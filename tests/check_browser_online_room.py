#!/usr/bin/env python3
"""Qualify the browser's release-gated, launcher-owned Online Room entry."""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, OverlayServer, find_chrome,
    page_websocket, require, wait_value,
)

ROOT = Path(__file__).resolve().parent.parent


def resolve(value: str) -> Path:
    candidate = Path(value).expanduser()
    if not candidate.is_absolute():
        candidate = ROOT / candidate
    return candidate.resolve()


def run(args: argparse.Namespace) -> None:
    shell = resolve(args.shell_dir)
    for relative in ("index.html", "online/online-control-config.js",
                     "online/online-room.css",
                     "online/online-room-live-state.js",
                     "online/online-room-presenter.js",
                     "online/online-room.js"):
        require((shell / relative).is_file(),
                f"browser Online Room artifact missing: {relative}")
    server = OverlayServer(shell, shell)
    server.start()
    with tempfile.TemporaryDirectory(prefix="mdkr-browser-online-") as profile:
        chrome = ChromeProcess(find_chrome(args.chrome), Path(profile),
                               args.chrome_flag, args.verbose)
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Inspector", "Accessibility"):
                cdp.call(f"{domain}.enable")
            test_surface_source = """
              globalThis.__mdkrOnlineRoomSurfaceTest = true;
              globalThis.__mdkrPartyHostTestConfig = {
                initialRoomState:{type:'room_state',transitionId:1,controllers:[]},
                async request(path) {
                  if (path === '/api/party/create') return {
                    roomId:'abcdefghijklmnopqrstuv', hostCredential:'H'.repeat(43),
                    fallbackCode:'123456', inviteGeneration:1,
                    inviteExpiresInMs:120000,
                    controllerUrl:location.origin+'/controller/#'+'A'.repeat(43)
                  };
                  return {ok:true};
                }
              };
            """
            cdp.call("Emulation.setDeviceMetricsOverride", {
                "width": 320, "height": 568, "deviceScaleFactor": 2,
                "mobile": True,
            })
            cdp.call("Page.navigate", {"url": server.origin + "/"})
            wait_value(cdp, "Boolean(globalThis.MDKROnlineRoom)", bool,
                       "browser Online Room API", args.timeout)
            production_gate = cdp.evaluate("""(() => {
              MDKRPartyHost.setRomReady(true);
              MDKRPartyHost.open();
              MDKROnlineRoom.open();
              return {
                onlineHidden:document.getElementById('online-room-open').hidden,
                phoneHidden:document.getElementById('add-phone-controllers').hidden,
                stageHidden:document.getElementById('party-stage-button').hidden,
                phoneDisabled:document.getElementById('add-phone-controllers').disabled,
                onlineOpen:document.getElementById('online-room-dialog').open,
                partyOpen:document.getElementById('party-dialog').open
              };
            })()""")
            require(production_gate == {
                "onlineHidden": True, "phoneHidden": True,
                "stageHidden": True, "phoneDisabled": True,
                "onlineOpen": False, "partyOpen": False,
            }, f"cloud-dependent production surfaces were reachable: {production_gate}")

            cdp.call("Page.addScriptToEvaluateOnNewDocument",
                     {"source": test_surface_source})
            cdp.call("Page.navigate", {"url": server.origin + "/"})
            wait_value(cdp, "Boolean(globalThis.MDKROnlineRoom)", bool,
                       "test-gated browser Online Room API", args.timeout)
            cdp.evaluate("""(() => {
              globalThis.__onlineNetworkCalls=[];
              const originalFetch=globalThis.fetch.bind(globalThis);
              globalThis.fetch=(...args)=>{
                globalThis.__onlineNetworkCalls.push(String(args[0]));
                return originalFetch(...args);
              };
              globalThis.MDKROnlineRoom.open();
            })()""")
            opened = wait_value(cdp, """(() => ({
              open:document.getElementById('online-room-dialog').open,
              title:document.getElementById('online-room-title').textContent.trim(),
              focus:document.activeElement.id,
              local:document.getElementById('online-room-local').textContent.trim(),
              controllers:document.getElementById('online-room-controllers').disabled,
              calls:globalThis.__onlineNetworkCalls.slice()
            }))()""", lambda value: isinstance(value, dict) and
                value.get("focus") == "online-room-title",
                "release-gated Online Room", args.timeout)
            require(opened["open"] and
                    opened["title"] == "Online Racing Is Not Enabled in This Build" and
                    opened["local"] == "Choose ROM" and opened["controllers"] and
                    not opened["calls"],
                    f"browser release gate is incomplete or performed I/O: {opened}")
            refused = cdp.evaluate("""MDKROnlineRoom.configure({compatibility:{
              protocolVersion:1,buildId:Array(16).fill(1),
              gameplayDigest:Array(32).fill(2),romRevision:1,cadenceHz:30
            }}).then(result=>({result,enabled:MDKROnlineRoom.enabled(),
              calls:globalThis.__onlineNetworkCalls.slice(),
              modelScripts:[...document.scripts].filter(script=>
                script.src.includes('mdkr-online-tools')).length}))""",
                await_promise=True)
            require(refused == {"result": False, "enabled": False,
                               "calls": [], "modelScripts": 0},
                    f"disabled publisher policy was bypassed: {refused}")

            cdp.call("Emulation.setPageScaleFactor", {"pageScaleFactor": 2})
            layout = cdp.evaluate("""(() => {
              const dialog=document.getElementById('online-room-dialog');
              const rect=dialog.getBoundingClientRect();
              return {documentWidth:document.documentElement.scrollWidth,
                viewport:innerWidth, left:rect.left, right:rect.right,
                clientHeight:dialog.clientHeight, scrollHeight:dialog.scrollHeight};
            })()""")
            require(layout["documentWidth"] <= layout["viewport"] and
                    layout["left"] >= 0 and layout["right"] <= layout["viewport"] + 1 and
                    layout["clientHeight"] > 0 and
                    layout["scrollHeight"] >= layout["clientHeight"],
                    f"320x568/200% Online Room does not reflow: {layout}")

            # Enable the real launcher-owned local actions, then prove the
            # dialog reflects those owners instead of carrying a second copy of
            # ROM/controller readiness.
            cdp.evaluate("""(() => {
              const play=document.getElementById('play');
              const phone=document.getElementById('add-phone-controllers');
              play.disabled=false; delete play.dataset.blocked;
              globalThis.MDKRPartyHost.setRomReady(true);
              globalThis.__phoneRoute=0;
              phone.addEventListener('click',()=>globalThis.__phoneRoute++);
              globalThis.MDKROnlineRoom.sync();
            })()""")
            synced = cdp.evaluate("""({
              local:document.getElementById('online-room-local').textContent.trim(),
              controllers:document.getElementById('online-room-controllers').disabled
            })""")
            require(synced == {"local": "Play here", "controllers": False},
                    f"local recovery ownership drifted: {synced}")
            cdp.evaluate("document.getElementById('online-room-controllers').click()")
            wait_value(cdp, "globalThis.__phoneRoute", lambda value: value == 1,
                       "phone-controller handoff", args.timeout)
            require(not cdp.evaluate(
                "document.getElementById('online-room-dialog').open"),
                "controller handoff left two modal owners open")
            wait_value(cdp, "document.getElementById('party-dialog').open", bool,
                       "phone-controller sheet", args.timeout)
            cdp.evaluate("document.getElementById('party-close').click()")
            wait_value(cdp, "!document.getElementById('party-dialog').open", bool,
                       "phone-controller sheet dismissal", args.timeout)

            cdp.evaluate("document.getElementById('online-room-open').focus(); MDKROnlineRoom.open()")
            wait_value(cdp, "document.activeElement.id",
                       lambda value: value == "online-room-title",
                       "reopened Online Room focus", args.timeout)
            cdp.call("Input.dispatchKeyEvent", {
                "type": "keyDown", "key": "Escape", "code": "Escape",
                "windowsVirtualKeyCode": 27, "nativeVirtualKeyCode": 27,
            })
            cdp.call("Input.dispatchKeyEvent", {
                "type": "keyUp", "key": "Escape", "code": "Escape",
                "windowsVirtualKeyCode": 27, "nativeVirtualKeyCode": 27,
            })
            wait_value(cdp, "!document.getElementById('online-room-dialog').open",
                       bool, "Escape dismissal", args.timeout)
            wait_value(cdp, "document.activeElement.id",
                       lambda value: value == "online-room-open",
                       "trigger focus restoration", args.timeout)
            require(not cdp.failures,
                    "browser/CDP failures: " + "; ".join(cdp.failures))
            fatal = [line for line in cdp.console
                     if "Uncaught" in line or "TypeError" in line]
            require(not fatal, "Online Room console errors: " + "; ".join(fatal))
            print("check_browser_online_room: PASS — honest zero-I/O gate, "
                  "launcher-owned local routes, Escape focus and 320x568/200% reflow")
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
        print(f"check_browser_online_room: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
