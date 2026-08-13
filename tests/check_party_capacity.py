#!/usr/bin/env python3
"""Exercise $0 admission/control reserves through a restartable real Worker."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from check_browser_online_two_person import (
    SERVICE, WRANGLER, click_action, free_port, node_binary, wait_worker,
)
from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, find_chrome, page_websocket,
    require, wait_value,
)


ROOT = Path(__file__).resolve().parent.parent
COMPATIBILITY = {
    "protocolVersion": 1,
    "buildId": list(range(1, 17)),
    "gameplayDigest": list(range(128, 160)),
    "romRevision": 1,
    "cadenceHz": 30,
}
OPS_READ_TOKEN = "capacity-ops-fixture-0123456789abcdef"


def request(origin: str, path: str, value: Any | None = None,
            credential: str = "") -> tuple[int, dict[str, str], Any]:
    headers = {"Origin": origin}
    data = None
    if value is not None:
        data = json.dumps(value, separators=(",", ":")).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if credential:
        headers["Authorization"] = f"Bearer {credential}"
    item = urllib.request.Request(origin + path, data=data, headers=headers,
                                  method="POST" if value is not None else "GET")
    try:
        response = urllib.request.urlopen(item, timeout=10)
    except urllib.error.HTTPError as error:
        response = error
    body = response.read()
    content_type = response.headers.get("Content-Type", "")
    decoded: Any = body.decode("utf-8", "replace")
    if "application/json" in content_type:
        decoded = json.loads(decoded)
    return response.status, {key.lower(): value for key, value in response.headers.items()}, decoded


def worker_command(origin: str, shell: Path, state: Path,
                   max_admissions: int) -> list[str]:
    return [str(node_binary()), str(WRANGLER), "dev", "--local",
            "--ip", "127.0.0.1", "--port", origin.rsplit(":", 1)[1],
            "--inspector-port", "0", "--assets", str(shell),
            "--persist-to", str(state),
            "--var", f"PARTY_ORIGIN:{origin}",
            "--var", "PARTY_HMAC_KEY:capacity-fixture-32-byte-secret!!",
            "--var", f"OPS_READ_TOKEN:{OPS_READ_TOKEN}",
            "--var", f"MAX_ADMISSIONS_PER_DAY:{max_admissions}",
            "--var", "CONTROL_RESERVE_PER_DAY:5000",
            "--log-level", "error"]


def start_worker(origin: str, shell: Path, state: Path, log: Any,
                 max_admissions: int) -> subprocess.Popen[bytes]:
    process = subprocess.Popen(worker_command(origin, shell, state, max_admissions),
                               cwd=SERVICE, stdout=log, stderr=subprocess.STDOUT)
    wait_worker(origin, process, 60)
    return process


def stop_worker(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def assert_static(origin: str) -> None:
    for path in ("/", "/controller/", "/room/"):
        status, headers, body = request(origin, path)
        require(status == 200 and isinstance(body, str) and "<!doctype html>" in body,
                f"static route {path} failed during capacity refusal: {status}")
        require(headers.get("x-content-type-options") == "nosniff" and
                headers.get("cross-origin-opener-policy") == "same-origin" and
                "default-src 'self'" in headers.get("content-security-policy", ""),
                f"static route {path} lost security headers: {headers}")
    status, _, policy = request(origin, "/online/online-control-config.js")
    require(status == 200 and "enabled: false" in policy,
            "capacity path did not retain the disabled online release policy")


def run(args: argparse.Namespace) -> None:
    shell = (ROOT / args.shell_dir).resolve()
    require((shell / "index.html").is_file(), "missing web shell")
    require(WRANGLER.is_file(), "missing lockfile-pinned Wrangler")
    with tempfile.TemporaryDirectory(prefix="mdkr-capacity-") as temporary:
        root = Path(temporary)
        log_path = root / "wrangler.log"
        with log_path.open("wb") as log:
            port = free_port()
            origin = f"http://127.0.0.1:{port}"
            process: subprocess.Popen[bytes] | None = None
            try:
                # Two worst-case reservations exactly consume the deliberately
                # tiny fixture admission budget: one MatchRoom and one Phone
                # Party. Control has its independent reserve.
                process = start_worker(origin, shell, root / "state", log, 20)
                assert_static(origin)
                match_status, match_headers, match = request(origin,
                    "/api/match/create", {"compatibility": COMPATIBILITY,
                                           "seatCount": 1})
                require(match_status == 201 and
                        match_headers.get("cache-control") == "no-store",
                        f"initial MatchRoom reservation failed: {match_status}, {match}")
                party_status, _, party = request(origin, "/api/party/create",
                                                  {"hostPublicKey": "H" * 87})
                require(party_status == 201,
                        f"initial Phone Party reservation failed: {party_status}, {party}")

                def refused_create(kind: int) -> tuple[int, Any]:
                    if kind % 2:
                        status, _, body = request(origin, "/api/party/create",
                                                  {"hostPublicKey": "H" * 87})
                    else:
                        status, _, body = request(origin, "/api/match/create",
                            {"compatibility": COMPATIBILITY, "seatCount": 1})
                    return status, body

                with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                    refused = list(pool.map(refused_create, range(64)))
                require(all(status == 503 and body == {"error": "service_budget_safe"}
                            for status, body in refused),
                        f"admission flood did not fail closed: {refused[:6]}")
                assert_static(origin)

                # A process/deploy restart must reconstruct the stop line and
                # both established object types from the same persisted state.
                # Otherwise restart would reopen admission or strand control.
                stop_worker(process)
                process = None
                process = start_worker(origin, shell, root / "state", log, 20)
                assert_static(origin)
                restart_status, _, restart_snapshot = request(
                    origin, "/api/ops/capacity", credential=OPS_READ_TOKEN)
                require(restart_status == 200 and
                        restart_snapshot["admitted"] == {
                            "pairingUnits": 20, "controlUnits": 0} and
                        restart_snapshot["refusalObserved"] == {
                            "pairing": True, "control": False} and
                        restart_snapshot["level"] == "closed",
                        f"restart lost the capacity stop line: {restart_snapshot}")

                def forged_control(kind: int) -> tuple[int, Any]:
                    if kind % 2:
                        status, _, body = request(origin,
                            "/api/match/AAAAAAAAAAAAAAAAAAAAAA/state", {}, "F" * 43)
                    else:
                        status, _, body = request(origin,
                            "/api/party/BBBBBBBBBBBBBBBBBBBBBB/close", {}, "F" * 43)
                    return status, body

                with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                    forged = list(pool.map(forged_control, range(64)))
                require(all(status == 401 and body == {"error": "unauthorized"}
                            for status, body in forged),
                        f"forged control flood did not reject uniformly: {forged[:6]}")
                after_forgery_status, _, after_forgery = request(
                    origin, "/api/ops/capacity", credential=OPS_READ_TOKEN)
                require(after_forgery_status == 200 and
                        after_forgery["admitted"]["controlUnits"] == 0 and
                        after_forgery["refusalObserved"]["control"] is False,
                        f"forged control drained the reserve: {after_forgery}")
                restart_match, _, restart_match_state = request(origin,
                    f"/api/match/{match['roomId']}/state", {}, match["credential"])
                require(restart_match == 200 and
                        restart_match_state["lobby"]["revision"] == 1,
                        f"restart lost MatchRoom state: {restart_match_state}")

                # The same real refusal must land in the bounded product copy,
                # keep local recovery visible, and adapt Play Here to Choose ROM
                # for a clean profile that has no verified cartridge yet.
                chrome = ChromeProcess(find_chrome(args.chrome),
                    root / "capacity-browser", args.chrome_flag, args.verbose)
                cdp: CDPClient | None = None
                try:
                    cdp = CDPClient(page_websocket(chrome.wait_port()))
                    for domain in ("Page", "Runtime", "Log", "Inspector",
                                   "Accessibility"):
                        cdp.call(f"{domain}.enable")
                    cdp.call("Page.navigate", {"url": origin + "/"})
                    wait_value(cdp, "Boolean(globalThis.MDKROnlineRoom) && "
                               "document.readyState==='complete'", bool,
                               "capacity launcher", args.timeout)
                    configured = cdp.evaluate(
                        """(async()=>{
                          globalThis.__mdkrOnlineControlReleasePolicy=Object.freeze({
                            enabled:true,serviceOrigin:location.origin});
                          return MDKROnlineRoom.configure(%s);
                        })()""" % json.dumps({"compatibility": COMPATIBILITY}),
                        await_promise=True)
                    require(configured is True,
                            "capacity launcher could not initialize room control")
                    cdp.evaluate("MDKROnlineRoom.open()")
                    click_action(cdp, 1)
                    refusal = wait_value(cdp, """(() => ({
                      title:document.getElementById('online-room-title').textContent,
                      copy:document.getElementById('online-room-explanation').textContent,
                      controls:[...document.querySelectorAll('[data-online-action]')]
                        .map(button=>({action:Number(button.dataset.onlineAction),
                          label:button.textContent,disabled:button.disabled}))
                    }))()""", lambda value: isinstance(value, dict) and
                        value.get("title") == "Online Rooms Are Full Right Now",
                        "typed capacity recovery", args.timeout)
                    require("keep hosting free" in refusal["copy"] and
                            any(item == {"action": 17, "label": "Choose ROM",
                                         "disabled": False}
                                for item in refusal["controls"]) and
                            any(item["action"] == 14 for item in refusal["controls"]),
                            f"capacity recovery lacked local/retry actions: {refusal}")
                    ax = cdp.call("Accessibility.getFullAXTree").get("nodes", [])
                    names = {node.get("name", {}).get("value", "") for node in ax}
                    folded = {name.casefold() for name in names}
                    require("choose rom" in folded and "try again" in folded,
                            f"capacity recovery actions missing from AX tree: {names}")
                    click_action(cdp, 17)
                    wait_value(cdp, "document.activeElement.id",
                               lambda value: value == "drop",
                               "capacity local recovery route", 10)

                    # Exhausted pairing must not bypass or starve the separately
                    # weighted WebSocket control path. Exercise both upgrade
                    # shapes through the real browser rather than inferring them
                    # from HTTP state/command traffic.
                    sockets = cdp.evaluate("""(async()=>{
                      const specs=%s;
                      const opened=[];
                      for (const spec of specs) {
                        const socket=new WebSocket(spec.url, spec.protocols);
                        await new Promise((resolve,reject)=>{
                          const timer=setTimeout(()=>reject(new Error("socket timeout")),5000);
                          socket.onopen=()=>{clearTimeout(timer);resolve();};
                          socket.onerror=()=>{clearTimeout(timer);reject(new Error("socket error"));};
                        });
                        opened.push(socket);
                      }
                      const protocols=opened.map(socket=>socket.protocol);
                      opened.forEach(socket=>socket.close());
                      return protocols;
                    })()""" % json.dumps([
                        {"url": origin.replace("http", "ws", 1) +
                         f"/api/match/{match['roomId']}/connect",
                         "protocols": ["gb-match-v1",
                                       f"gb-match.{match['credential']}"]},
                        {"url": origin.replace("http", "ws", 1) +
                         f"/api/match/{match['roomId']}/signal",
                         "protocols": ["gb-match-signal-v1",
                                       f"gb-match.{match['credential']}"]},
                        {"url": origin.replace("http", "ws", 1) +
                         f"/api/party/{party['roomId']}/connect",
                         "protocols": ["gb-control-v1",
                                       f"gb-host.{party['hostCredential']}"]},
                    ]), await_promise=True)
                    require(sockets == ["gb-match-v1", "gb-match-signal-v1",
                                        "gb-control-v1"],
                            f"control-reserve sockets failed: {sockets}")
                    require(not cdp.failures,
                            "capacity browser failures: " + "; ".join(cdp.failures))
                finally:
                    if cdp is not None:
                        cdp.close()
                    chrome.close()

                # New joins are admission too and must refuse even when their
                # capability is otherwise current and valid.
                capability = match["inviteUrl"].split("#match=", 1)[1]
                join_status, join_headers, join = request(origin, "/api/match/join",
                    {"capability": capability, "compatibility": COMPATIBILITY,
                     "seatCount": 1})
                require(join_status == 503 and
                        join == {"error": "service_budget_safe"} and
                        join_headers.get("cache-control") == "no-store",
                        f"exhausted join was not typed/no-store: {join_status}, {join}")

                # Existing authenticated state, mutation, and close traffic
                # retain custody despite an adversarial admission flood.
                state_status, _, state = request(origin,
                    f"/api/match/{match['roomId']}/state", {}, match["credential"])
                require(state_status == 200 and state["lobby"]["revision"] == 1,
                        f"control reserve could not read established room: {state}")
                command = {"protocolVersion": 1, "expectedRevision": 1,
                           "commandId": "1", "type": "set_character",
                           "value": 0, "targetEndpointId": "0"}
                command_status, _, command_result = request(origin,
                    f"/api/match/{match['roomId']}/command", command,
                    match["credential"])
                require(command_status == 200 and command_result["accepted"] is True,
                        f"control reserve rejected established mutation: {command_result}")
                close = {"protocolVersion": 1,
                         "expectedRevision": command_result["revision"],
                         "commandId": "2", "type": "close", "value": 0,
                         "targetEndpointId": "0"}
                close_status, _, close_result = request(origin,
                    f"/api/match/{match['roomId']}/command", close,
                    match["credential"])
                require(close_status == 200 and close_result["accepted"] is True,
                        f"MatchRoom close reserve failed: {close_result}")
                party_close, _, party_result = request(origin,
                    f"/api/party/{party['roomId']}/close", {},
                    party["hostCredential"])
                require(party_close == 200 and party_result.get("ok") is True,
                        f"Phone Party close reserve failed: {party_result}")
                still_refused, _, body = request(origin, "/api/match/create",
                    {"compatibility": COMPATIBILITY, "seatCount": 1})
                require(still_refused == 503 and
                        body == {"error": "service_budget_safe"},
                        "closing a room incorrectly refunded the daily stop line")

                unauth_status, unauth_headers, unauth = request(
                    origin, "/api/ops/capacity")
                require(unauth_status == 401 and
                        unauth == {"error": "unauthorized"} and
                        unauth_headers.get("cache-control") == "no-store",
                        f"capacity snapshot was not secret-gated: "
                        f"{unauth_status}, {unauth}")
                wrong_status, _, wrong = request(
                    origin, "/api/ops/capacity", credential="wrong-credential")
                require(wrong_status == 401 and wrong == {"error": "unauthorized"},
                        f"capacity snapshot accepted a wrong secret: {wrong_status}, {wrong}")
                health_unauth_status, _, health_unauth = request(
                    origin, "/api/ops/health")
                require(health_unauth_status == 401 and
                        health_unauth == {"error": "unauthorized"},
                        "health aggregate was not secret-gated")
                status, headers, snapshot = request(
                    origin, "/api/ops/capacity", credential=OPS_READ_TOKEN)
                require(status == 200 and headers.get("cache-control") == "no-store",
                        f"capacity snapshot failed: {status}, {snapshot}")
                require(snapshot == {
                    "schemaVersion": 1,
                    "admitted": {"pairingUnits": 20, "controlUnits": 57},
                    "refusalObserved": {"pairing": True, "control": False},
                    "remaining": {"admissionUnits": 0,
                                  "controlUnits": 19823},
                    "admissionPercent": 100,
                    "level": "closed",
                    "day": datetime.now(timezone.utc).date().isoformat(),
                }, f"capacity snapshot violated its bounded schema: {snapshot}")
                serialized = json.dumps(snapshot, sort_keys=True)
                forbidden = [match["roomId"], match["credential"],
                             match["fallbackCode"], party["roomId"],
                             party["hostCredential"], party["fallbackCode"],
                             capability, "H" * 87, "127.0.0.1"]
                require(all(value not in serialized for value in forbidden),
                        f"capacity snapshot leaked identity/capability data: {serialized}")
                health_status, health_headers, health = request(
                    origin, "/api/ops/health", credential=OPS_READ_TOKEN)
                require(health_status == 200 and
                        health_headers.get("cache-control") == "no-store" and
                        health == {
                            "schemaVersion": 2,
                            "reservationRequests": 10,
                            "reservations": {
                                "matchCreate": 1,
                                "matchLinkJoin": 0,
                                "matchCodeJoin": 0,
                                "matchControl": 4,
                                "matchRotate": 0,
                                "matchSocket": 1,
                                "matchSignalSocket": 1,
                                "partyCreate": 1,
                                "partyLinkJoin": 0,
                                "partyCodeJoin": 0,
                                "partyControl": 1,
                                "partyRotate": 0,
                                "partySocket": 1,
                                "legacy": 0,
                            },
                            "admitted": {
                                "pairingUnits": 20,
                                "controlUnits": 57,
                            },
                            "tracked": {
                                "pairingUnits": 20,
                                "controlUnits": 57,
                            },
                            "tracking": "complete",
                            "day": datetime.now(timezone.utc).date().isoformat(),
                        }, f"health aggregate was not exact: {health}")
                health_serialized = json.dumps(health, sort_keys=True)
                require(all(value not in health_serialized for value in forbidden),
                        f"health aggregate leaked identity/capability data: {health_serialized}")
            finally:
                stop_worker(process)

            # Literal zero is a separate operator kill-switch proof, not an
            # inference from reaching a small nonzero ceiling.
            port = free_port()
            zero_origin = f"http://127.0.0.1:{port}"
            process = None
            try:
                process = start_worker(zero_origin, shell, root / "zero-state",
                                       log, 0)
                assert_static(zero_origin)
                for path, value in (
                    ("/api/match/create",
                     {"compatibility": COMPATIBILITY, "seatCount": 1}),
                    ("/api/party/create", {"hostPublicKey": "H" * 87}),
                ):
                    status, headers, body = request(zero_origin, path, value)
                    require(status == 503 and
                            body == {"error": "service_budget_safe"} and
                            headers.get("cache-control") == "no-store",
                            f"literal-zero kill switch failed for {path}: {status}, {body}")
                zero_status, _, zero_snapshot = request(
                    zero_origin, "/api/ops/capacity", credential=OPS_READ_TOKEN)
                require(zero_status == 200 and
                        zero_snapshot["admitted"] == {
                            "pairingUnits": 0, "controlUnits": 0} and
                        zero_snapshot["refusalObserved"] == {
                            "pairing": True, "control": False} and
                        zero_snapshot["remaining"]["admissionUnits"] == 0 and
                        zero_snapshot["level"] == "closed",
                        f"zero-stop snapshot was not exact: {zero_snapshot}")
                zero_health_status, _, zero_health = request(
                    zero_origin, "/api/ops/health", credential=OPS_READ_TOKEN)
                require(zero_health_status == 200 and
                        zero_health["reservationRequests"] == 0 and
                        all(count == 0 for count in
                            zero_health["reservations"].values()) and
                        zero_health["tracking"] == "complete" and
                        zero_health["admitted"] == zero_health["tracked"],
                        f"refused work polluted health aggregates: {zero_health}")
            finally:
                stop_worker(process)
        details = log_path.read_text(encoding="utf-8", errors="replace")
        require("ERROR" not in details.upper(),
                "Wrangler reported an error during capacity test:\n" + details[-4000:])
    print("check_party_capacity: PASS — 64-way admission + forged-control floods, "
          "persisted process restart, typed/no-store refusal, MatchRoom/Phone "
          "Party control reserve, exact capacity/health aggregates, static "
          "fallback and zero switch")


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
    except (CheckFailure, OSError, ValueError, subprocess.SubprocessError,
            json.JSONDecodeError) as error:
        print(f"check_party_capacity: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
