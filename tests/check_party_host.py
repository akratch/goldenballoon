#!/usr/bin/env python3
"""Exercise launcher-owned phone pairing and approval UX in real Chromium."""

from __future__ import annotations

import argparse
import json
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
    for relative in ("index.html", "party/party-host.css", "party/party-host.js",
                     "party/qrcodegen.js"):
        require((shell / relative).is_file(), f"party host artifact missing: {relative}")
    server = OverlayServer(shell, shell)
    server.start()
    with tempfile.TemporaryDirectory(prefix="mdkr64_party_host_") as profile:
        chrome = ChromeProcess(find_chrome(args.chrome), Path(profile),
                               args.chrome_flag, args.verbose)
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Inspector", "Accessibility"):
                cdp.call(f"{domain}.enable")
            config = {
                "initialRoomState": {"type": "room_state", "transitionId": 1,
                                     "controllers": []},
            }
            source = """
              globalThis.__mdkrPartyHostTestConfig = {
                initialRoomState: %s,
                async request(path) {
                  if (path === '/api/party/create') return {
                    roomId:'abcdefghijklmnopqrstuv', hostCredential:'H'.repeat(43),
                    fallbackCode:'123456', inviteGeneration:1, inviteExpiresInMs:120000,
                    controllerUrl:location.origin+'/controller/#'+'A'.repeat(43)
                  };
                  if (path.endsWith('/rotate')) return {
                    fallbackCode:'654321', inviteGeneration:2, inviteExpiresInMs:120000,
                    controllerUrl:location.origin+'/controller/#'+'B'.repeat(43)
                  };
                  return {ok:true};
                }
              };
            """ % json.dumps(config["initialRoomState"], separators=(",", ":"))
            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": source})
            cdp.call("Emulation.setDeviceMetricsOverride", {
                "width": 390, "height": 844, "deviceScaleFactor": 2, "mobile": True,
            })
            cdp.call("Page.navigate", {"url": server.origin + "/"})
            wait_value(cdp, "Boolean(globalThis.MDKRPartyHost)", bool,
                       "party host API", args.timeout)
            cdp.evaluate("globalThis.MDKRPartyHost.setRomReady(true); globalThis.MDKRPartyHost.open()")
            ready = wait_value(cdp, """(() => ({
              room: !document.getElementById('party-room').hidden,
              code: document.getElementById('party-code').textContent,
              qr: document.getElementById('party-qr').width,
              dialog: document.getElementById('party-dialog').open
            }))()""", lambda value: isinstance(value, dict) and value.get("room"),
                "pairing sheet", args.timeout)
            require(ready["dialog"] and ready["code"] == "123 456" and ready["qr"] > 200,
                    f"pairing invite did not render: {ready}")

            sas = cdp.evaluate("""(async () => {
              const host=await MDKRPartySas.createIdentity();
              const phone=await MDKRPartySas.createIdentity();
              const transcript={roomId:'abcdefghijklmnopqrstuv',
                hostPublicKey:host.publicKey,controllerPublicKey:phone.publicKey};
              const a=await MDKRPartySas.phrase(host.privateKey,phone.publicKey,transcript);
              const b=await MDKRPartySas.phrase(phone.privateKey,host.publicKey,transcript);
              return {a,b,hostLength:host.publicKey.length,phoneLength:phone.publicKey.length};
            })()""", await_promise=True)
            require(sas["a"] == sas["b"] and sas["hostLength"] == 87 and
                    sas["phoneLength"] == 87 and len(sas["a"].split()) == 2,
                    f"ECDH pairing phrase mismatch: {sas}")

            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:2, controllers:[{
                controllerId:'phone-one', name:'Sam’s phone', phrase:'Bright Balloon',
                phase:'pending', seat:null, leaseGeneration:0, connectionSequence:1
              }]});""")
            pending = wait_value(cdp, """(() => ({
              text: document.getElementById('party-pending-list').textContent,
              buttons: document.querySelectorAll('#party-pending-list button').length,
              seat: document.querySelector('#party-pending-list select').value
            }))()""", lambda value: isinstance(value, dict) and value.get("buttons") == 2,
                "pending approval", args.timeout)
            require("Sam’s phone" in pending["text"] and "Bright Balloon" in pending["text"] and
                    pending["seat"] == "2",
                    f"identity/phrase absent from approval: {pending}")
            cdp.evaluate("document.querySelector('#party-pending-list .btn-primary').click()")
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/approve'))",
                bool, "approve request", args.timeout)
            approved_request = cdp.evaluate("""(() =>
              globalThis.__mdkrPartyHostTestState.requestDetails.find(
                entry => entry.path.endsWith('/approve')))()""")
            require(approved_request["body"].get("seat") == 2,
                    f"host-selected free seat was not sent: {approved_request}")

            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:3, controllers:[{
                controllerId:'phone-one', name:'Sam’s phone', phrase:'Bright Balloon',
                phase:'leased', seat:2, leaseGeneration:1, connectionSequence:1
              }]});""")
            seat = wait_value(cdp, """(() => ({
              ready: document.querySelector('[data-seat="2"]').dataset.ready,
              label: document.querySelector('[data-seat="2"] small').textContent,
              startDisabled: document.getElementById('party-start').disabled
            }))()""", lambda value: isinstance(value, dict) and value.get("ready") == "true",
                "approved seat", args.timeout)
            require(not seat["startDisabled"] and seat["label"] == "Phone reconnecting — neutral",
                    f"approved controller did not enable start: {seat}")

            cdp.evaluate("document.getElementById('party-extend').click()")
            wait_value(cdp, "document.getElementById('party-code').textContent",
                       lambda value: value == "654 321", "rotated QR", args.timeout)
            cdp.call("Emulation.setPageScaleFactor", {"pageScaleFactor": 2})
            layout = cdp.evaluate("({width:document.documentElement.scrollWidth, viewport:innerWidth})")
            require(layout["width"] <= layout["viewport"], f"200% party sheet overflow: {layout}")

            cdp.evaluate("document.getElementById('party-close').click()")
            wait_value(cdp, "!document.getElementById('party-dialog').open", bool,
                       "closed sheet", args.timeout)
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/revoke'))",
                bool, "dismissed launcher invite revocation", args.timeout)
            require(cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)"),
                    "closing setup unexpectedly ended approved controller leases")

            cdp.evaluate("""(() => {
              try {
                document.getElementById('stage').hidden=false;
                globalThis.MDKRPartyHost.open();
                globalThis.__partyOpenDebug={ok:true};
              } catch (error) {
                globalThis.__partyOpenDebug={ok:false,error:String(error),stack:error.stack};
              }
            })()""")
            wait_value(cdp, """({
              open:globalThis.__mdkrPartyOverlayOpen,
              stageHidden:document.getElementById('stage').hidden,
              dialog:document.getElementById('party-dialog').open,
              debug:globalThis.__partyOpenDebug,
              ready:globalThis.MDKRPartyHost.state().romReady,
              room:Boolean(globalThis.MDKRPartyHost.state().room),
              api:String(globalThis.MDKRPartyHost.open),
              triggerDisabled:document.getElementById('add-phone-controllers').disabled,
              lifecycle:globalThis.__mdkrPartyHostTestState.lifecycle.slice()
            })""", lambda value: isinstance(value, dict) and value.get("open") is True,
                       "in-game local pause latch", args.timeout)
            wait_value(cdp, "Boolean(globalThis.MDKRPartyHost.state().room)", bool,
                       "in-game controller room", args.timeout)
            cdp.evaluate("document.getElementById('party-close').click()")
            wait_value(cdp, "!globalThis.__mdkrPartyOverlayOpen", bool,
                       "in-game local pause release", args.timeout)
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/revoke'))",
                bool, "dismissed invite revocation", args.timeout)
            preserved = cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)")
            require(preserved, "in-game dismissal destroyed the controller room")

            cdp.evaluate("globalThis.MDKRPartyHost.open()")
            wait_value(cdp, "document.getElementById('party-dialog').open", bool,
                       "room reopened for explicit end", args.timeout)
            cdp.evaluate("document.getElementById('party-end').click()")
            confirmation = wait_value(cdp, """(() => ({
              open:document.getElementById('party-end-dialog').open,
              room:Boolean(globalThis.MDKRPartyHost.state().room),
              closeSent:globalThis.__mdkrPartyHostTestState.requests
                .some(path=>path.endsWith('/close')),
              focus:document.activeElement?.id
            }))()""", lambda value: isinstance(value, dict) and
                value.get("open") is True, "end-room confirmation", args.timeout)
            require(confirmation["room"] and not confirmation["closeSent"] and
                    confirmation["focus"] == "party-end-cancel",
                    f"end-room confirmation was not safe by default: {confirmation}")
            cdp.evaluate("document.getElementById('party-end-cancel').click()")
            wait_value(cdp, "!document.getElementById('party-end-dialog').open", bool,
                       "cancelled end-room confirmation", args.timeout)
            require(cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)"),
                    "cancelling end-room confirmation destroyed the room")
            cdp.evaluate("""(() => {
              document.getElementById('party-end').click();
              document.getElementById('party-end-confirm').click();
            })()""")
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/close'))",
                bool, "explicit room close request", args.timeout)
            require(not cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)"),
                    "End controller room did not clear the room")
            require(not cdp.failures, "browser/CDP failures: " + "; ".join(cdp.failures))
            fatal = [line for line in cdp.console if "Uncaught" in line or "TypeError" in line]
            require(not fatal, "party host console errors: " + "; ".join(fatal))
            print("check_party_host: PASS — mixed-source seat choice, QR/code rotation, "
                  "in-game pause/revoke/preserve, confirmed close cleanup and 200% mobile layout")
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
        print(f"check_party_host: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
