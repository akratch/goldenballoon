#!/usr/bin/env python3
"""Prove the real launcher/controller WebRTC path and input-test round trip."""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
from pathlib import Path

from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, OverlayServer, find_chrome,
    page_websocket, require, wait_value,
)

ROOT = Path(__file__).resolve().parent.parent


def client(chrome_path: str, profile: Path, flags: list[str], verbose: bool) -> tuple[ChromeProcess, CDPClient]:
    process = ChromeProcess(chrome_path, profile, flags, verbose)
    connection = CDPClient(page_websocket(process.wait_port()))
    for domain in ("Page", "Runtime", "Log", "Inspector"):
        connection.call(f"{domain}.enable")
    return process, connection


def relay(source: CDPClient, target: CDPClient, source_expr: str, target_expr: str) -> int:
    messages = source.evaluate(source_expr) or []
    for message in messages:
        target.evaluate(f"{target_expr}({json.dumps(message, separators=(',', ':'))})")
    return len(messages)


def run(args: argparse.Namespace) -> None:
    shell = (ROOT / args.shell_dir).resolve()
    server = OverlayServer(shell, shell)
    server.start()
    chrome_path = find_chrome(args.chrome)
    with tempfile.TemporaryDirectory(prefix="mdkr64_party_rtc_") as temp:
        base = Path(temp)
        host_process, host = client(chrome_path, base / "host", args.chrome_flag, args.verbose)
        phone_process, phone = client(chrome_path, base / "phone", args.chrome_flag, args.verbose)
        try:
            room_state = {"type": "room_state", "transitionId": 2,
                          "controllers": [{"controllerId": "phone-one",
                              "name": "Test phone", "phrase": "Bright Balloon",
                              "phase": "leased", "seat": 1, "leaseGeneration": 7,
                              "connectionSequence": 1}]}
            host_source = """
              globalThis.__mdkrPartyHostTestConfig={
                initialRoomState:%s,
                async request(path){return {roomId:'abcdefghijklmnopqrstuv',
                  hostCredential:'H'.repeat(43),fallbackCode:'123456',
                  inviteExpiresInMs:120000,
                  controllerUrl:location.origin+'/controller/#'+'A'.repeat(43)}}
              };
            """ % json.dumps(room_state, separators=(",", ":"))
            phone_source = """
              Object.defineProperty(navigator,'vibrate',{configurable:true,
                value:(milliseconds)=>{globalThis.__mdkrVibration=milliseconds;return true;}});
              globalThis.__mdkrControllerTestConfig={
                directSignaling:true,autoApprove:false,seat:1,connectionSequence:1,
                capability:'A'.repeat(43),
                async request(){return {roomId:'abcdefghijklmnopqrstuv',
                  controllerId:'phone-one',credential:'C'.repeat(43),protocol:1,
                  phrase:'Bright Balloon'}}
              };
            """
            host.call("Page.addScriptToEvaluateOnNewDocument", {"source": host_source})
            phone.call("Page.addScriptToEvaluateOnNewDocument", {"source": phone_source})
            host.call("Page.navigate", {"url": server.origin + "/"})
            phone.call("Page.navigate", {"url": server.origin + "/controller/#secret"})
            wait_value(host, "Boolean(globalThis.MDKRPartyHost)", bool,
                       "host party API", args.timeout)
            wait_value(phone, "Boolean(globalThis.__mdkrControllerTest)", bool,
                       "controller test API", args.timeout)
            host.evaluate("globalThis.MDKRPartyHost.setRomReady(true); globalThis.MDKRPartyHost.open()")
            wait_value(host, "!document.getElementById('party-room').hidden", bool,
                       "host room", args.timeout)

            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                relay(phone, host,
                      "globalThis.__mdkrControllerTestState.signals?.splice(0) || []",
                      "globalThis.MDKRPartyHost.receiveSignal")
                relay(host, phone,
                      "globalThis.__mdkrPartyHostTestState.signals?.splice(0) || []",
                      "globalThis.__mdkrControllerTest.receiveSignal")
                connected = host.evaluate("globalThis.MDKRPartyHost.remotePads()[0].active")
                if connected:
                    break
                time.sleep(0.03)
            require(bool(connected), "direct WebRTC controller did not become active")

            phase = phone.evaluate("globalThis.__mdkrControllerTest.state().phase")
            require(phase == "assigned", f"phone was not assigned after direct handshake: {phase}")
            phone.evaluate("document.getElementById('input-test').click()")
            wait_value(phone, "!document.getElementById('use-controller').disabled", bool,
                       "reliable input-test round trip", args.timeout)
            phone.evaluate("document.getElementById('use-controller').click()")
            phone.evaluate("""(() => {
              const go=document.querySelector('.touch-go');
              const r=go.getBoundingClientRect();
              go.parentElement.dispatchEvent(new PointerEvent('pointerdown',{
                pointerId:31,pointerType:'touch',clientX:(r.left+r.right)/2,
                clientY:(r.top+r.bottom)/2,bubbles:true,cancelable:true}));
            })()""")
            wait_value(host, "globalThis.MDKRPartyHost.remotePads()[0].packets.length",
                       lambda value: isinstance(value, int) and value > 0,
                       "unordered pad-state packet", args.timeout)
            packet = host.evaluate("""(() => {
              const p=globalThis.MDKRPartyHost.remotePads()[0];
              return {owner:p.owner,connectionSequence:p.connectionSequence,
                length:p.packets.at(-1).byteLength,drops:p.drops};
            })()""")
            require(packet["owner"] == 57 and packet["connectionSequence"] == 1 and
                    24 <= packet["length"] <= 64 and packet["drops"] == 0,
                    f"remote pad handoff metadata invalid: {packet}")
            require(bool(host.evaluate("globalThis.MDKRPartyHost.remotePads()[0].haptics")),
                    "phone vibration capability did not cross reliable control")
            require(bool(host.evaluate("globalThis.MDKRPartyHost.remotePads()[0].rumble(32768)")),
                    "host could not send bounded phone rumble")
            wait_value(phone, "globalThis.__mdkrVibration", lambda value: value == 250,
                       "optional phone vibration", args.timeout)
            require(not host.failures and not phone.failures,
                    "CDP failure in direct controller path")
            print("check_phone_party_webrtc: PASS — authenticated signaling relay, "
                  "direct unordered state/reliable control channels, input-test RTT, "
                  "bounded pad handoff and optional phone haptics")
        finally:
            host.close()
            phone.close()
            host_process.close()
            phone_process.close()
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
        print(f"check_phone_party_webrtc: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
