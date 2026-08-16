#!/usr/bin/env python3
"""Run the fixed aggregate-only MatchRoom/Phone Party beta canary."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
import tempfile
import time
from typing import Any
import urllib.error
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
from check_browser_runtime import (  # noqa: E402
    CDPClient, ChromeProcess, CheckFailure, find_chrome, page_websocket,
    require, wait_value,
)


COMPATIBILITY = {
    "protocolVersion": 1,
    "buildId": list(range(1, 17)),
    "gameplayDigest": list(range(128, 160)),
    "romRevision": 1,
    "cadenceHz": 30,
}
QUALIFYING_ATTEMPTS = 20


def validated_origin(value: str, allow_http_loopback: bool) -> str:
    parsed = urllib.parse.urlsplit(value)
    loopback = parsed.hostname in {"127.0.0.1", "localhost", "::1"}
    if (parsed.username or parsed.password or parsed.query or parsed.fragment or
            parsed.path not in {"", "/"} or not parsed.hostname or
            (parsed.scheme != "https" and not (
                allow_http_loopback and parsed.scheme == "http" and loopback))):
        raise CheckFailure("origin must be one bare HTTPS origin")
    return urllib.parse.urlunsplit(
        (parsed.scheme, parsed.netloc, "", "", ""))


def post(origin: str, path: str, value: Any,
         credential: str = "") -> tuple[int, dict[str, Any]]:
    headers = {"Origin": origin, "Content-Type": "application/json"}
    if credential:
        headers["Authorization"] = f"Bearer {credential}"
    request = urllib.request.Request(origin + path,
        data=json.dumps(value, separators=(",", ":")).encode("utf-8"),
        headers=headers, method="POST")
    try:
        response = urllib.request.urlopen(request, timeout=15)
    except urllib.error.HTTPError as error:
        response = error
    try:
        decoded = json.loads(response.read().decode("utf-8", "strict"))
    except (UnicodeError, json.JSONDecodeError):
        decoded = {}
    return response.status, decoded if isinstance(decoded, dict) else {}


def elapsed_ms(started: float) -> int:
    return max(0, math.ceil((time.monotonic() - started) * 1000))


def p95(values: list[int], failure_value: int) -> int:
    if not values:
        return failure_value
    ordered = sorted(values)
    return ordered[math.ceil(len(ordered) * 0.95) - 1]


def match_attempt(origin: str) -> tuple[int | None, int | None]:
    room: dict[str, Any] = {}
    revision = 1
    create_started = time.monotonic()
    status, room = post(origin, "/api/match/create",
                        {"compatibility": COMPATIBILITY, "seatCount": 1})
    create_time = elapsed_ms(create_started)
    if status != 201:
        return None, None
    try:
        capability = urllib.parse.urlsplit(str(room.get("inviteUrl", ""))).fragment
        if not capability.startswith("match="):
            return create_time, None
        join_started = time.monotonic()
        join_status, joined = post(origin, "/api/match/join", {
            "capability": capability.removeprefix("match="),
            "compatibility": COMPATIBILITY, "seatCount": 1,
        })
        join_time = elapsed_ms(join_started)
        if join_status != 201:
            return create_time, None
        lobby = joined.get("lobby")
        if not isinstance(lobby, dict) or not isinstance(lobby.get("revision"), int):
            return create_time, None
        revision = int(lobby["revision"])
        return create_time, join_time
    finally:
        room_id = room.get("roomId")
        credential = room.get("credential")
        if isinstance(room_id, str) and isinstance(credential, str):
            try:
                post(origin, f"/api/match/{room_id}/command", {
                    "protocolVersion": 1, "expectedRevision": revision,
                    "commandId": "1", "type": "close", "value": 0,
                    "targetEndpointId": "0",
                }, credential)
            except OSError:
                pass


def browser(chrome_path: str, profile: Path, flags: list[str],
            verbose: bool) -> tuple[ChromeProcess, CDPClient]:
    process = ChromeProcess(chrome_path, profile, flags, verbose)
    cdp = CDPClient(page_websocket(process.wait_port()))
    for domain in ("Page", "Runtime", "Log", "Inspector"):
        cdp.call(f"{domain}.enable")
    return process, cdp


def phone_attempt(origin: str, chrome_path: str, profiles: Path,
                  flags: list[str], verbose: bool,
                  timeout: float) -> tuple[int | None, int | None]:
    host_process, host = browser(chrome_path, profiles / "host", flags, verbose)
    phone_process, phone = browser(chrome_path, profiles / "phone", flags, verbose)
    room: dict[str, Any] = {}
    try:
        host.call("Page.addScriptToEvaluateOnNewDocument", {"source": """
          globalThis.__mdkrPartyHostSurfaceTest=true;
        """})
        host.call("Page.navigate", {"url": origin + "/"})
        wait_value(host, "Boolean(globalThis.MDKRPartyHost) && "
                   "document.readyState==='complete'", bool,
                   "canary host launcher", timeout)
        host.evaluate("MDKRPartyHost.setRomReady(true); MDKRPartyHost.open()")
        room = wait_value(host, "MDKRPartyHost.state().room",
            lambda value: isinstance(value, dict) and
                isinstance(value.get("controllerUrl"), str),
            "canary controller invitation", timeout)
        phone.call("Page.addScriptToEvaluateOnNewDocument", {"source": """
          (() => {
            const NativeWebSocket=globalThis.WebSocket;
            globalThis.__canarySockets=[];
            globalThis.__canaryBlockSockets=false;
            function ObservedWebSocket(url, protocols) {
              if (!globalThis.__canaryBlockSockets) {
                const socket=new NativeWebSocket(url, protocols);
                globalThis.__canarySockets.push(socket);
                return socket;
              }
              const socket=new EventTarget();
              socket.readyState=NativeWebSocket.CLOSED;
              socket.binaryType='blob';
              socket.send=()=>{};
              socket.close=()=>{};
              setTimeout(()=>socket.dispatchEvent(new CloseEvent('close',
                {code:4000,reason:'synthetic signaling outage'})),0);
              return socket;
            }
            ObservedWebSocket.CONNECTING=NativeWebSocket.CONNECTING;
            ObservedWebSocket.OPEN=NativeWebSocket.OPEN;
            ObservedWebSocket.CLOSING=NativeWebSocket.CLOSING;
            ObservedWebSocket.CLOSED=NativeWebSocket.CLOSED;
            ObservedWebSocket.prototype=NativeWebSocket.prototype;
            globalThis.WebSocket=ObservedWebSocket;
          })();
        """})
        phone.call("Page.navigate", {"url": room["controllerUrl"]})
        wait_value(phone, "!document.getElementById('state-waiting').hidden", bool,
                   "canary pending phone", timeout)
        wait_value(host,
            "Boolean(document.querySelector('#party-pending-list .btn-primary'))",
            bool, "canary host approval", timeout)
        setup_started = time.monotonic()
        host.evaluate(
            "document.querySelector('#party-pending-list .btn-primary').click()")
        seat = wait_value(host, """(() => {
          const item=MDKRPartyHost.state().room?.controllers?.find(
            value=>['leased','connected'].includes(value.phase));
          if (!item?.seat || !MDKRPartyHost.remotePads()[item.seat-1]?.active) return 0;
          return item.seat;
        })()""", lambda value: isinstance(value, int) and value > 0,
            "canary direct data channels", timeout)
        require(1 <= seat <= 4, "canary received an invalid controller seat")
        setup_time = elapsed_ms(setup_started)
        wait_value(phone, "!document.getElementById('state-assigned').hidden", bool,
                   "canary assigned phone", timeout)
        rtt_started = time.monotonic()
        phone.evaluate("document.getElementById('input-test').click()")
        wait_value(phone, "!document.getElementById('use-controller').disabled", bool,
                   "canary input round trip", timeout)
        input_rtt = elapsed_ms(rtt_started)
        phone.evaluate("document.getElementById('use-controller').click()")
        wait_value(phone, "!document.getElementById('state-controller').hidden", bool,
                   "canary active controller", timeout)
        initial_sequence = host.evaluate(
            f"MDKRPartyHost.remotePads()[{seat - 1}].connectionSequence")
        host.evaluate(f"MDKRPartyHost.remotePads()[{seat - 1}].packets.length=0")
        phone.evaluate("""(() => {
          globalThis.__canaryBlockSockets=true;
          globalThis.__canarySockets.at(-1)?.close(4000,'synthetic signaling outage');
        })()""")
        wait_value(phone, "document.getElementById('connection-mark').classList.contains('limited')",
                   bool, "direct-only controller status", timeout)
        require(phone.evaluate(
            "!document.getElementById('state-controller').hidden"),
            "signaling loss hid healthy direct controls")
        phone.evaluate("document.querySelector('.touch-go').click()")
        wait_value(host, f"""MDKRPartyHost.remotePads()[{seat - 1}].packets.some(
          packet => {{
            const value=MDKRPartyProtocol.decode(packet);
            return (value.buttons & 32768)!==0 ||
              value.edges.some(edge => (edge.buttons & 32768)!==0);
          }})""", bool, "direct input during signaling outage", timeout)
        phone.evaluate("globalThis.__canaryBlockSockets=false")
        wait_value(host, f"""(() => {{
          const pad=MDKRPartyHost.remotePads()[{seat - 1}];
          return pad.active && pad.connectionSequence>{int(initial_sequence)};
        }})()""", bool, "same-lease controller recovery", timeout)
        wait_value(phone, "!document.getElementById('state-controller').hidden && "
                   "!document.getElementById('connection-mark').classList.contains('limited')",
                   bool, "phone controls after signaling recovery", timeout)
        host.evaluate(f"MDKRPartyHost.remotePads()[{seat - 1}].packets.length=0")
        phone.evaluate("document.querySelector('.touch-go').click()")
        wait_value(host, f"""MDKRPartyHost.remotePads()[{seat - 1}].packets.some(
          packet => {{
            const value=MDKRPartyProtocol.decode(packet);
            return (value.buttons & 32768)!==0 ||
              value.edges.some(edge => (edge.buttons & 32768)!==0);
          }})""", bool, "direct input after signaling recovery", timeout)
        require(not host.failures and not phone.failures,
                "canary browser protocol failure")
        return setup_time, input_rtt
    except (CheckFailure, OSError, ValueError):
        return None, None
    finally:
        room_id = room.get("roomId") if isinstance(room, dict) else None
        credential = room.get("hostCredential") if isinstance(room, dict) else None
        if isinstance(room_id, str) and isinstance(credential, str):
            try:
                post(origin, f"/api/party/{room_id}/close", {}, credential)
            except OSError:
                pass
        for connection in (host, phone):
            try:
                connection.close()
            except OSError:
                pass
        for process in (host_process, phone_process):
            try:
                process.close()
            except OSError:
                pass


def decision(value: dict[str, Any]) -> str:
    create = value["matchCreate"]
    join = value["matchJoin"]
    direct = value["phoneDirect"]
    return "GO" if (create["attempts"] == QUALIFYING_ATTEMPTS and
        create["successes"] >= 19 and create["p95Ms"] <= 2_500 and
        join["attempts"] == QUALIFYING_ATTEMPTS and join["successes"] >= 19 and
        join["p95Ms"] <= 2_500 and direct["attempts"] == QUALIFYING_ATTEMPTS and
        direct["successes"] >= 18 and direct["setupP95Ms"] <= 8_000 and
        direct["inputRttP95Ms"] <= 250) else "STOP"


def run(args: argparse.Namespace) -> int:
    origin = validated_origin(args.origin, args.allow_http_loopback)
    if args.attempts != QUALIFYING_ATTEMPTS and not args.development:
        raise CheckFailure("operated canary requires exactly 20 attempts")
    if args.output.exists():
        raise CheckFailure("output already exists")
    chrome_path = find_chrome(args.chrome)
    create_times: list[int] = []
    join_times: list[int] = []
    setup_times: list[int] = []
    rtt_times: list[int] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-party-canary-") as temporary:
        profile_root = Path(temporary)
        for index in range(args.attempts):
            created, joined = match_attempt(origin)
            if created is not None:
                create_times.append(created)
            if joined is not None:
                join_times.append(joined)
            setup, rtt = phone_attempt(origin, chrome_path,
                profile_root / f"phone-{index}", args.chrome_flag,
                args.verbose, args.timeout)
            if setup is not None and rtt is not None:
                setup_times.append(setup)
                rtt_times.append(rtt)
    result: dict[str, Any] = {
        "schemaVersion": 1,
        "source": "synthetic_canary_v1",
        "matchCreate": {"attempts": args.attempts,
                        "successes": len(create_times),
                        "p95Ms": p95(create_times, 2_501)},
        "matchJoin": {"attempts": args.attempts,
                      "successes": len(join_times),
                      "p95Ms": p95(join_times, 2_501)},
        "phoneDirect": {"attempts": args.attempts,
                        "successes": len(setup_times),
                        "setupP95Ms": p95(setup_times, 8_001),
                        "inputRttP95Ms": p95(rtt_times, 251)},
    }
    result["decision"] = decision(result)
    with args.output.open("x", encoding="utf-8") as handle:
        json.dump(result, handle, separators=(",", ":"), sort_keys=True)
        handle.write("\n")
    print("run_party_experience_canary: " + result["decision"] +
          f" attempts={args.attempts} outputWritten=1")
    return 0 if result["decision"] == "GO" else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--origin", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--attempts", type=int, default=QUALIFYING_ATTEMPTS)
    parser.add_argument("--development", action="store_true")
    parser.add_argument("--allow-http-loopback", action="store_true")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        if args.attempts < 1 or args.attempts > QUALIFYING_ATTEMPTS:
            raise CheckFailure("attempts must be between 1 and 20")
        return run(args)
    except (CheckFailure, OSError, ValueError):
        print("run_party_experience_canary: STOP code=canary_unavailable")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
