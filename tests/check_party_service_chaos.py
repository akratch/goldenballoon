#!/usr/bin/env python3
"""Prove a live Phone Party survives restart, abuse, the kill switch and quota.

Four GA exit proofs, all driven headlessly against a real local Worker:

1. A process restart against the same persisted state rebinds every approved
   controller lease onto its own seat, rotates the connection epoch, refuses the
   generations that were current before the restart, and evicts nobody.
2. A concurrent malformed/forged/cross-room/oversize flood during that live
   session buys zero admission units, cannot touch a second room's seats, and
   never interrupts the session's control, close or signaling traffic.
3. Flipping admission to the literal-zero kill switch stops new pairing dead
   while every already-paired controller keeps its seat and keeps working.
4. Exhausting the day's admission ceiling refuses new pairing with the typed,
   no-store, identity-free enum the phone maps to its recovery copy, leaves the
   paired seat untouched, and persists nothing beyond one latched refusal flag.
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Callable

from check_browser_online_two_person import free_port
from check_browser_runtime import CheckFailure, require
from check_party_capacity import OPS_READ_TOKEN, request, start_worker, stop_worker


ROOT = Path(__file__).resolve().parent.parent
HOST_KEY = "H" * 87
CONTROLLER_KEYS = {"a": "A" * 87, "b": "B" * 87, "c": "C" * 87}
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
SETUP_CEILING = 400
OFFER = {"type": "offer", "sdp": "v=0\r\n"}
ANSWER = {"type": "answer", "sdp": "v=0\r\n"}


# ---------------------------------------------------------------- HTTP helpers

def raw_request(origin: str, path: str, body: bytes | None,
                headers: dict[str, str] | None = None
                ) -> tuple[int, dict[str, str], Any]:
    """`request()` with an attacker-controlled body and header set.

    check_party_capacity.request only emits well-formed JSON, which cannot
    express the malformed, oversize or unsupported-media shapes this gate has to
    prove are free. Every other call in this file uses the shared helper.
    """
    item = urllib.request.Request(origin + path, data=body,
                                  headers={"Origin": origin, **(headers or {})},
                                  method="POST")
    try:
        response = urllib.request.urlopen(item, timeout=15)
    except urllib.error.HTTPError as error:
        response = error
    payload = response.read()
    decoded: Any = payload.decode("utf-8", "replace")
    if "application/json" in response.headers.get("Content-Type", ""):
        decoded = json.loads(decoded)
    return (response.status,
            {key.lower(): value for key, value in response.headers.items()},
            decoded)


def capacity(origin: str) -> dict[str, Any]:
    status, headers, body = request(origin, "/api/ops/capacity",
                                    credential=OPS_READ_TOKEN)
    require(status == 200 and headers.get("cache-control") == "no-store",
            f"capacity snapshot unavailable: {status}, {body}")
    return body


def health(origin: str) -> dict[str, Any]:
    status, _, body = request(origin, "/api/ops/health", credential=OPS_READ_TOKEN)
    require(status == 200, f"health snapshot unavailable: {status}, {body}")
    return body


def admitted(origin: str) -> tuple[int, int]:
    snapshot = capacity(origin)["admitted"]
    return snapshot["pairingUnits"], snapshot["controlUnits"]


def create_room(origin: str) -> dict[str, Any]:
    status, headers, body = request(origin, "/api/party/create",
                                    {"hostPublicKey": HOST_KEY})
    require(status == 201 and headers.get("cache-control") == "no-store",
            f"party create failed: {status}, {body}")
    body["capability"] = body["controllerUrl"].split("#", 1)[1]
    return body


def redeem(origin: str, capability: str, key: str, name: str) -> dict[str, Any]:
    status, _, body = request(origin, "/api/controller/redeem",
                              {"capability": capability, "protocol": 1,
                               "name": name, "controllerPublicKey": key})
    require(status == 201, f"controller redeem failed for {name}: {status}, {body}")
    return body


def approve(origin: str, room: dict[str, Any], controller: dict[str, Any],
            seat: int) -> dict[str, Any]:
    status, _, body = request(origin, f"/api/party/{room['roomId']}/approve",
                              {"controllerId": controller["controllerId"],
                               "seat": seat}, room["hostCredential"])
    require(status == 200 and body.get("ok") is True and body.get("seat") == seat,
            f"host approval failed for seat {seat}: {status}, {body}")
    return body


def rotate(origin: str, room: dict[str, Any], generation: int) -> dict[str, Any]:
    status, _, body = request(origin, f"/api/party/{room['roomId']}/rotate",
                              {"expectedInviteGeneration": generation},
                              room["hostCredential"])
    require(status == 200 and body.get("inviteGeneration") == generation + 1,
            f"host rotate {generation} failed: {status}, {body}")
    room["capability"] = body["controllerUrl"].split("#", 1)[1]
    room["fallbackCode"] = body["fallbackCode"]
    room["inviteGeneration"] = body["inviteGeneration"]
    return body


# ----------------------------------------------------------- WebSocket helpers

def _handshake(origin: str, path: str, protocols: list[str]
               ) -> tuple[socket.socket, int, str, bytes]:
    parsed = urllib.parse.urlsplit(origin)
    connection = socket.create_connection(
        (parsed.hostname or "127.0.0.1", parsed.port or 80), timeout=15)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    connection.sendall((
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {parsed.hostname}:{parsed.port}\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
        f"Origin: {origin}\r\n"
        f"Sec-WebSocket-Protocol: {', '.join(protocols)}\r\n\r\n"
    ).encode("ascii"))
    buffer = bytearray()
    while b"\r\n\r\n" not in buffer:
        chunk = connection.recv(4096)
        if not chunk:
            connection.close()
            raise CheckFailure(f"party socket handshake closed for {path}")
        buffer.extend(chunk)
        require(len(buffer) <= 65536, "oversized party socket handshake")
    header, leftover = bytes(buffer).split(b"\r\n\r\n", 1)
    text = header.decode("latin1", "replace")
    status = int(text.split(" ", 2)[1])
    if status == 101:
        accept = base64.b64encode(
            hashlib.sha1((key + GUID).encode("ascii")).digest()).decode("ascii")
        require(f"sec-websocket-accept: {accept}".lower() in text.lower(),
                f"party socket returned the wrong accept key for {path}")
        require("sec-websocket-protocol: gb-control-v1" in text.lower(),
                f"party socket did not negotiate gb-control-v1 for {path}:\n{text}")
    return connection, status, text, leftover


def probe_socket(origin: str, path: str, protocols: list[str]) -> int:
    """Handshake status for an adversarial upgrade, without keeping the socket."""
    connection, status, _, _ = _handshake(origin, path, protocols)
    connection.close()
    return status


class PartySocket:
    """Minimal RFC6455 client for the real gb-control-v1 party protocol."""

    def __init__(self, origin: str, path: str, protocols: list[str], label: str):
        connection, status, text, leftover = _handshake(origin, path, protocols)
        if status != 101:
            connection.close()
            raise CheckFailure(f"{label} socket refused: {status}\n{text}")
        self.sock = connection
        self.buffer = bytearray(leftover)
        self.label = label
        self.closed: tuple[int, str] | None = None
        self.stopped = ""
        self.seen: list[dict[str, Any]] = []

    # -- framing ------------------------------------------------------------
    def send(self, value: dict[str, Any]) -> None:
        payload = json.dumps(value, separators=(",", ":")).encode("utf-8")
        mask = os.urandom(4)
        header = bytearray([0x81])
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length <= 0xFFFF:
            header.append(0x80 | 126)
            header.extend(struct.pack(">H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack(">Q", length))
        header.extend(mask)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def _fill(self, count: int, deadline: float) -> bool:
        while len(self.buffer) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            self.sock.settimeout(min(remaining, 1.0))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError as error:
                self.stopped = f"transport error: {error}"
                return False
            if not chunk:
                self.stopped = "transport closed without a WebSocket close frame"
                return False
            self.buffer.extend(chunk)
        return True

    def _frame(self, deadline: float) -> tuple[int, bytes] | None:
        if not self._fill(2, deadline):
            return None
        opcode = self.buffer[0] & 0x0F
        require(self.buffer[1] & 0x80 == 0,
                f"{self.label} received a masked server frame")
        length = self.buffer[1] & 0x7F
        offset = 2
        if length == 126:
            if not self._fill(4, deadline):
                return None
            length = struct.unpack(">H", bytes(self.buffer[2:4]))[0]
            offset = 4
        elif length == 127:
            if not self._fill(10, deadline):
                return None
            length = struct.unpack(">Q", bytes(self.buffer[2:10]))[0]
            offset = 10
        if not self._fill(offset + length, deadline):
            return None
        payload = bytes(self.buffer[offset:offset + length])
        del self.buffer[:offset + length]
        return opcode, payload

    def _pump(self, deadline: float) -> dict[str, Any] | None:
        """Next protocol message, or None once the deadline or the peer stops."""
        while True:
            frame = self._frame(deadline)
            if frame is None:
                return None
            opcode, payload = frame
            if opcode == 0x8:
                code = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else 0
                self.closed = (code, payload[2:].decode("utf-8", "replace"))
                return None
            if opcode in (0x9, 0xA):
                continue
            require(opcode == 0x1,
                    f"{self.label} received unexpected opcode {opcode}")
            value = json.loads(payload.decode("utf-8"))
            self.seen.append(value)
            return value

    # -- assertions ---------------------------------------------------------
    def expect(self, predicate: Callable[[dict[str, Any]], bool], label: str,
               timeout: float = 20.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while True:
            value = self._pump(deadline)
            if value is None:
                raise CheckFailure(
                    f"{self.label} never saw {label} (closed={self.closed}, "
                    f"stopped={self.stopped!r}, seen={self.seen[-6:]})")
            if predicate(value):
                return value

    def drain(self, window: float = 2.0) -> list[dict[str, Any]]:
        """Everything delivered inside a bounded window; for negative proofs."""
        deadline = time.monotonic() + window
        values: list[dict[str, Any]] = []
        while True:
            value = self._pump(deadline)
            if value is None:
                require(not self.stopped,
                        f"{self.label} lost its transport while idle: "
                        f"{self.stopped}")
                return values
            values.append(value)

    def expect_close(self, code: int, reason: str, timeout: float = 20.0) -> None:
        deadline = time.monotonic() + timeout
        while self.closed is None and self._pump(deadline) is not None:
            continue
        require(self.closed == (code, reason),
                f"{self.label} close was {self.closed}, expected {(code, reason)}")

    def dead(self, timeout: float = 20.0) -> bool:
        """True once the peer end of this connection is gone."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.sock.settimeout(0.5)
            try:
                if not self.sock.recv(65536):
                    return True
            except socket.timeout:
                continue
            except OSError:
                return True
        return False

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def host_socket(origin: str, room: dict[str, Any], label: str) -> PartySocket:
    socket_ = PartySocket(origin, f"/api/party/{room['roomId']}/connect",
                          ["gb-control-v1", f"gb-host.{room['hostCredential']}"],
                          label)
    socket_.expect(lambda value: value.get("type") == "room_state",
                   "initial room_state")
    return socket_


def controller_socket(origin: str, room: dict[str, Any],
                      controller: dict[str, Any], label: str
                      ) -> tuple[PartySocket, dict[str, Any]]:
    socket_ = PartySocket(
        origin, f"/api/party/{room['roomId']}/connect",
        ["gb-control-v1", f"gb-controller.{controller['credential']}"], label)
    state = socket_.expect(lambda value: value.get("type") == "controller_state",
                           "initial controller_state")
    return socket_, state


def controllers_of(state: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {item["controllerId"]: item for item in state["controllers"]}


def signal_round_trip(host: PartySocket, controller: PartySocket,
                      controller_id: str, generation: int) -> None:
    """One host->phone offer and phone->host answer over the live sockets."""
    host.send({"type": "webrtc_offer", "to": controller_id,
               "peerGeneration": generation, "sdp": OFFER})
    controller.expect(
        lambda value: value.get("type") == "webrtc_offer" and
        value.get("to") == controller_id and
        value.get("peerGeneration") == generation, "relayed webrtc_offer")
    controller.send({"type": "webrtc_answer", "controllerId": controller_id,
                     "peerGeneration": generation, "sdp": ANSWER})
    host.expect(lambda value: value.get("type") == "webrtc_answer" and
                value.get("controllerId") == controller_id and
                value.get("peerGeneration") == generation, "relayed webrtc_answer")


# ------------------------------------------------------------------- scenarios

def establish(origin: str) -> dict[str, Any]:
    """A room with two approved phone leases and three live sockets."""
    room = create_room(origin)
    first = redeem(origin, room["capability"], CONTROLLER_KEYS["a"], "Phone A")
    second = redeem(origin, room["capability"], CONTROLLER_KEYS["b"], "Phone B")
    approve(origin, room, first, 1)
    approve(origin, room, second, 2)
    require(admitted(origin) == (14, 4),
            f"setup admission accounting drifted: {capacity(origin)}")

    host = host_socket(origin, room, "host")
    first_socket, first_state = controller_socket(origin, room, first, "phone A")
    second_socket, second_state = controller_socket(origin, room, second, "phone B")
    require(first_state["seat"] == 1 and first_state["leaseGeneration"] == 1 and
            first_state["connectionSequence"] == 2 and
            second_state["seat"] == 2 and second_state["leaseGeneration"] == 2 and
            second_state["connectionSequence"] == 2,
            f"initial leases were not seated as designed: "
            f"{first_state}, {second_state}")
    signal_round_trip(host, first_socket, first["controllerId"], 2)
    signal_round_trip(host, second_socket, second["controllerId"], 2)
    require(admitted(origin) == (14, 88),
            f"three prepaid sockets did not reserve 84 control units: "
            f"{capacity(origin)}")
    return {"room": room, "first": first, "second": second, "host": host,
            "capability_v1": room["capability"],
            "sockets": [host, first_socket, second_socket],
            "first_socket": first_socket, "second_socket": second_socket}


def restart_proof(origin: str, boot: Callable[[int], None],
                  halt: Callable[[], None], session: dict[str, Any]
                  ) -> dict[str, Any]:
    """Scenario 1: a restart rebinds every lease onto its own seat."""
    room, first, second = session["room"], session["first"], session["second"]
    halt()
    boot(SETUP_CEILING)
    for socket_ in session["sockets"]:
        require(socket_.dead(), f"{socket_.label} outlived the killed Worker")
        socket_.close()
    require(admitted(origin) == (14, 88),
            f"restart lost persisted admission accounting: {capacity(origin)}")

    first_socket, first_state = controller_socket(origin, room, first, "phone A/2")
    second_socket, second_state = controller_socket(origin, room, second, "phone B/2")
    # party-room.ts upgradeWebSocket: a non-pending controller credential runs the
    # `reconnect` room command, which keeps seat and leaseGeneration and advances
    # connectionSequence. That advance is the connection epoch the phone and host
    # use to discard pre-restart peer generations.
    require(first_state == {"type": "controller_state",
                            "transitionId": first_state.get("transitionId"),
                            "phase": "connected", "seat": 1, "leaseGeneration": 1,
                            "connectionSequence": 3},
            f"phone A did not rebind onto its own seat: {first_state}")
    require(second_state["seat"] == 2 and second_state["leaseGeneration"] == 2 and
            second_state["connectionSequence"] == 3 and
            second_state["phase"] == "connected",
            f"phone B did not rebind onto its own seat: {second_state}")

    host = host_socket(origin, room, "host/2")
    room_state = next(value for value in host.seen
                      if value.get("type") == "room_state")
    seats = controllers_of(room_state)
    require(len(seats) == 2 and
            seats[first["controllerId"]]["seat"] == 1 and
            seats[first["controllerId"]]["leaseGeneration"] == 1 and
            seats[first["controllerId"]]["controllerPublicKey"] ==
            CONTROLLER_KEYS["a"] and
            seats[second["controllerId"]]["seat"] == 2 and
            seats[second["controllerId"]]["leaseGeneration"] == 2 and
            seats[second["controllerId"]]["controllerPublicKey"] ==
            CONTROLLER_KEYS["b"],
            f"a lease was reassigned across the restart: {room_state}")
    signal_round_trip(host, first_socket, first["controllerId"], 3)
    signal_round_trip(host, second_socket, second["controllerId"], 3)

    # Generations that were current before the restart are now stale and refused.
    rotate(origin, room, 1)
    stale_status, _, stale = request(origin, f"/api/party/{room['roomId']}/rotate",
                                     {"expectedInviteGeneration": 1},
                                     room["hostCredential"])
    require(stale_status == 409 and stale == {"error": "invalid_state"},
            f"a pre-restart invite generation was still accepted: "
            f"{stale_status}, {stale}")
    replay_status, _, replay = request(origin, "/api/controller/redeem",
                                       {"capability": session["capability_v1"],
                                        "protocol": 1,
                                        "controllerPublicKey": CONTROLLER_KEYS["c"]})
    require(replay_status == 409 and replay == {"error": "invite_rotated"},
            f"a pre-restart capability still admitted a phone: "
            f"{replay_status}, {replay}")

    # A second establishment for phone A must not evict phone B's channel.
    third_socket, third_state = controller_socket(origin, room, first, "phone A/3")
    require(third_state["seat"] == 1 and third_state["leaseGeneration"] == 1 and
            third_state["connectionSequence"] == 4,
            f"re-establishing phone A moved its lease: {third_state}")
    quiet = second_socket.drain()
    require(all(value.get("type") != "controller_state" for value in quiet) and
            second_socket.closed is None,
            f"phone B was evicted by phone A's rebind: {quiet}, "
            f"{second_socket.closed}")
    signal_round_trip(host, second_socket, second["controllerId"], 3)
    first_socket.close()

    session.update({"host": host, "first_socket": third_socket,
                    "second_socket": second_socket,
                    "sockets": [host, third_socket, second_socket]})
    return {"first_state": first_state, "second_state": second_state,
            "seats": seats}


def abuse_proof(origin: str, session: dict[str, Any]) -> dict[str, Any]:
    """Scenario 2: a live session outlasts a concurrent abuse flood."""
    room, first, second = session["room"], session["first"], session["second"]
    host, first_socket = session["host"], session["first_socket"]

    # A second room, so the flood has real foreign seats to fail to touch. Its
    # host socket is opened only after the flood: the authoritative evidence is
    # the persisted room the Worker then replays, not an idle client connection.
    other = create_room(origin)
    third = redeem(origin, other["capability"], CONTROLLER_KEYS["c"], "Phone C")
    approve(origin, other, third, 1)

    oversize = json.dumps({"capability": "x" * 32768, "protocol": 1,
                           "controllerPublicKey": CONTROLLER_KEYS["a"]}).encode()
    forged = "F" * 43
    shapes: list[tuple[str, Callable[[], tuple[int, Any]], int, Any]] = [
        ("malformed json", lambda: raw_request(
            origin, "/api/party/create", b"{",
            {"Content-Type": "application/json"})[::2], 400,
         {"error": "invalid_json"}),
        ("no media type", lambda: raw_request(
            origin, "/api/party/create", b"{}",
            {"Content-Type": "text/plain"})[::2], 415,
         {"error": "unsupported_media_type"}),
        ("forged host credential", lambda: request(
            origin, f"/api/party/{room['roomId']}/close", {}, forged)[::2], 401,
         {"error": "unauthorized"}),
        ("cross-room host credential", lambda: request(
            origin, f"/api/party/{other['roomId']}/revoke", {},
            room["hostCredential"])[::2], 401, {"error": "unauthorized"}),
        ("cross-room controller credential", lambda: request(
            origin, f"/api/party/{room['roomId']}/remove",
            {"controllerId": third["controllerId"]},
            third["credential"])[::2], 401, {"error": "unauthorized"}),
        ("oversize payload", lambda: raw_request(
            origin, "/api/controller/redeem", oversize,
            {"Content-Type": "application/json"})[::2], 413,
         {"error": "request_too_large"}),
        ("forged socket credential", lambda: (probe_socket(
            origin, f"/api/party/{room['roomId']}/connect",
            ["gb-control-v1", f"gb-controller.{forged}"]), None), 401, None),
    ]

    before_capacity = capacity(origin)
    before_health = health(origin)
    require(before_capacity["refusalObserved"] == {"pairing": False,
                                                   "control": False},
            f"the session had already latched a refusal: {before_capacity}")

    def flood(index: int) -> tuple[str, int, Any]:
        name, call, _, _ = shapes[index % len(shapes)]
        status, body = call()
        return name, status, body

    mark = len(host.seen)
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        pending = [pool.submit(flood, index) for index in range(112)]
        # The established session must keep transacting for the whole flood.
        for generation in range(2, 10):
            rotate(origin, room, generation)
            signal_round_trip(host, first_socket, first["controllerId"], 4)
        results = [item.result() for item in pending]

    expected = {name: (status, body) for name, _, status, body in shapes}
    for name, status, body in results:
        wanted_status, wanted_body = expected[name]
        require(status == wanted_status and
                (wanted_body is None or body == wanted_body),
                f"abuse shape '{name}' was not refused as designed: "
                f"{status}, {body}")
    require(len(results) == 112, f"the abuse flood did not complete: {len(results)}")

    after_capacity = capacity(origin)
    after_health = health(origin)
    require(after_capacity["admitted"]["pairingUnits"] ==
            before_capacity["admitted"]["pairingUnits"],
            f"the abuse flood spent admission units: {before_capacity}, "
            f"{after_capacity}")
    require(after_capacity["admitted"]["controlUnits"] -
            before_capacity["admitted"]["controlUnits"] == 80,
            f"the live session's eight rotations did not draw the control "
            f"reserve: {before_capacity}, {after_capacity}")
    require(after_capacity["refusalObserved"] == {"pairing": False,
                                                  "control": False},
            f"refused abuse latched a capacity refusal: {after_capacity}")
    reservation_delta = {key: after_health["reservations"][key] -
                         before_health["reservations"][key]
                         for key in after_health["reservations"]}
    require(reservation_delta == {**{key: 0 for key in reservation_delta},
                                  "partyRotate": 8},
            f"the abuse flood registered billable reservations: "
            f"{reservation_delta}")

    # The foreign room replays exactly the three transitions it was built from
    # (create, redeem, approve) with its first invite generation still current:
    # the flood rotated, revoked and removed nothing inside it.
    other_host = host_socket(origin, other, "other host")
    foreign_state = next(value for value in other_host.seen
                         if value.get("type") == "room_state")
    foreign_seat = controllers_of(foreign_state).get(third["controllerId"], {})
    require(foreign_state["transitionId"] == 3 and
            foreign_state["inviteGeneration"] == 1 and
            foreign_state["phase"] == "open" and
            len(foreign_state["controllers"]) == 1 and
            foreign_seat.get("seat") == 1 and
            foreign_seat.get("leaseGeneration") == 1 and
            foreign_seat.get("connectionSequence") == 1 and
            foreign_seat.get("phase") == "leased",
            f"a second room's seat moved during the flood: {foreign_state}")
    rotate(origin, other, 1)

    # The paired phone saw signaling only: no seat, lease or epoch change.
    phone_quiet = first_socket.drain()
    require(all(value.get("type") != "controller_state" for value in phone_quiet)
            and first_socket.closed is None,
            f"the paired phone saw a state change during the flood: {phone_quiet}")
    broadcasts = [value for value in host.seen[mark:]
                  if value.get("type") == "room_state"]
    require(len(broadcasts) >= 8 and all(
        controllers_of(value)[first["controllerId"]]["seat"] == 1 and
        controllers_of(value)[first["controllerId"]]["leaseGeneration"] == 1 and
        controllers_of(value)[first["controllerId"]]["connectionSequence"] == 4 and
        controllers_of(value)[second["controllerId"]]["seat"] == 2 and
        controllers_of(value)[second["controllerId"]]["leaseGeneration"] == 2
        for value in broadcasts),
            f"a seat moved in a broadcast during the flood: {broadcasts[:2]}")

    # Short-code guessing is well formed, so it is billed by design; it must still
    # be bounded per requester and must never resolve a room.
    guess_before = admitted(origin)
    guesses = [request(origin, "/api/controller/code",
                       {"code": "999999", "protocol": 1,
                        "controllerPublicKey": CONTROLLER_KEYS["a"]})
               for _ in range(13)]
    statuses = [status for status, _, _ in guesses]
    require(all(status in (404, 429) for status in statuses) and 429 in statuses,
            f"code guessing was not bounded by the requester limit: {statuses}")
    require(all(room["roomId"] not in json.dumps(body) and
                other["roomId"] not in json.dumps(body)
                for _, _, body in guesses),
            "a refused code guess leaked a room id")
    guess_after = admitted(origin)
    require(guess_after[0] - guess_before[0] == 39 and
            guess_after[1] == guess_before[1],
            f"code guessing was not billed at its designed rate: "
            f"{guess_before}, {guess_after}")

    # The session is still transacting after everything above.
    rotate(origin, room, 10)
    signal_round_trip(host, first_socket, first["controllerId"], 4)
    other_host.close()
    return {"reservation_delta": reservation_delta, "statuses": statuses,
            "foreign_seat": foreign_seat, "rotations": 9}


def kill_switch_proof(origin: str, boot: Callable[[int], None],
                      halt: Callable[[], None], session: dict[str, Any]
                      ) -> dict[str, Any]:
    """Scenario 3: literal-zero admission stops growth, not the party."""
    room, first, second = session["room"], session["first"], session["second"]
    before = capacity(origin)
    require(before["refusalObserved"]["pairing"] is False,
            f"pairing refusal was already latched before the flip: {before}")
    halt()
    for socket_ in session["sockets"]:
        socket_.close()

    boot(0)
    require(admitted(origin) == (before["admitted"]["pairingUnits"],
                                 before["admitted"]["controlUnits"]),
            f"the kill switch discarded persisted accounting: {capacity(origin)}")

    for path, value in (("/api/party/create", {"hostPublicKey": HOST_KEY}),
                        ("/api/controller/redeem",
                         {"capability": room["capability"], "protocol": 1,
                          "controllerPublicKey": CONTROLLER_KEYS["c"]}),
                        ("/api/controller/code",
                         {"code": room["fallbackCode"], "protocol": 1,
                          "controllerPublicKey": CONTROLLER_KEYS["c"]})):
        status, headers, body = request(origin, path, value)
        require(status == 503 and body == {"error": "service_budget_safe"} and
                headers.get("cache-control") == "no-store",
                f"the kill switch admitted new pairing at {path}: {status}, {body}")
    flipped = capacity(origin)
    require(flipped["refusalObserved"] == {"pairing": True, "control": False} and
            flipped["remaining"]["admissionUnits"] == 0 and
            flipped["admissionPercent"] == 100 and flipped["level"] == "closed",
            f"the kill switch did not latch a closed stop line: {flipped}")

    # Everything already paired coasts: sockets, seats, signaling and control.
    first_socket, first_state = controller_socket(origin, room, first, "phone A/4")
    second_socket, second_state = controller_socket(origin, room, second, "phone B/4")
    require(first_state["seat"] == 1 and first_state["leaseGeneration"] == 1 and
            second_state["seat"] == 2 and second_state["leaseGeneration"] == 2 and
            first_state["phase"] == "connected" and
            second_state["phase"] == "connected",
            f"the kill switch disturbed an existing seat: {first_state}, "
            f"{second_state}")
    host = host_socket(origin, room, "host/4")
    signal_round_trip(host, first_socket, first["controllerId"],
                      first_state["connectionSequence"])
    signal_round_trip(host, second_socket, second["controllerId"],
                      second_state["connectionSequence"])
    rotate(origin, room, room["inviteGeneration"])
    after = capacity(origin)
    require(after["admitted"]["pairingUnits"] == before["admitted"]["pairingUnits"],
            f"the kill switch still grew pairing: {before}, {after}")
    require(after["admitted"]["controlUnits"] -
            before["admitted"]["controlUnits"] == 94,
            f"the kill switch starved established control traffic: "
            f"{before}, {after}")

    session.update({"host": host, "first_socket": first_socket,
                    "second_socket": second_socket,
                    "sockets": [host, first_socket, second_socket]})
    return {"before": before, "after": after, "first_state": first_state,
            "second_state": second_state}


def exhaustion_proof(origin: str, boot: Callable[[int], None],
                     halt: Callable[[], None], shell: Path,
                     session: dict[str, Any]) -> dict[str, Any]:
    """Scenario 4: quota exhaustion as the already-paired phone experiences it."""
    room, first = session["room"], session["first"]
    pairing, _ = admitted(origin)
    halt()
    for socket_ in session["sockets"]:
        socket_.close()

    # A ceiling exactly one Phone Party create above the day's spend: the next
    # create lands on it, and every pairing unit after that is refused.
    boot(pairing + 10)
    first_socket, first_state = controller_socket(origin, room, first, "phone A/5")
    host = host_socket(origin, room, "host/5")
    require(first_state["seat"] == 1 and first_state["leaseGeneration"] == 1,
            f"the paired seat did not survive the ceiling restart: {first_state}")
    signal_round_trip(host, first_socket, first["controllerId"],
                      first_state["connectionSequence"])

    last = create_room(origin)
    exhausted = capacity(origin)
    require(exhausted["admitted"]["pairingUnits"] == pairing + 10 and
            exhausted["remaining"]["admissionUnits"] == 0 and
            exhausted["admissionPercent"] == 100 and
            exhausted["level"] == "closed",
            f"admission did not reach its ceiling: {exhausted}")

    secrets = [room["roomId"], room["hostCredential"], room["capability"],
               room["fallbackCode"], first["credential"], first["controllerId"],
               last["roomId"], last["hostCredential"], last["capability"],
               last["fallbackCode"], HOST_KEY, CONTROLLER_KEYS["a"]]
    for path, value in (("/api/party/create", {"hostPublicKey": HOST_KEY}),
                        ("/api/controller/redeem",
                         {"capability": last["capability"], "protocol": 1,
                          "controllerPublicKey": CONTROLLER_KEYS["c"]}),
                        ("/api/controller/code",
                         {"code": last["fallbackCode"], "protocol": 1,
                          "controllerPublicKey": CONTROLLER_KEYS["c"]})):
        status, headers, body = request(origin, path, value)
        require(status == 503 and body == {"error": "service_budget_safe"},
                f"exhausted pairing at {path} was not the typed refusal: "
                f"{status}, {body}")
        require(headers.get("cache-control") == "no-store",
                f"exhausted refusal at {path} was cacheable: {headers}")
        serialized = json.dumps({"headers": headers, "body": body})
        require(all(secret not in serialized for secret in secrets),
                f"exhausted refusal at {path} carried identity material: "
                f"{serialized}")

    # Refusals are bounded in what they persist: one latch per category, and no
    # reservation, counter or accounting growth however many arrive.
    latched_capacity = capacity(origin)
    latched_health = health(origin)

    def refused(index: int) -> tuple[int, Any]:
        if index % 2:
            status, _, body = request(origin, "/api/party/create",
                                      {"hostPublicKey": HOST_KEY})
        else:
            status, _, body = request(origin, "/api/controller/redeem",
                                      {"capability": last["capability"],
                                       "protocol": 1,
                                       "controllerPublicKey": CONTROLLER_KEYS["c"]})
        return status, body

    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        flood = list(pool.map(refused, range(64)))
    require(all(status == 503 and body == {"error": "service_budget_safe"}
                for status, body in flood),
            f"the exhausted flood was not uniformly typed: {flood[:6]}")
    require(capacity(origin) == latched_capacity,
            f"64 refusals moved the capacity snapshot: {latched_capacity}, "
            f"{capacity(origin)}")
    after_health = health(origin)
    require(after_health["reservations"] == latched_health["reservations"] and
            after_health["reservationRequests"] ==
            latched_health["reservationRequests"] > 0,
            f"64 refusals persisted reservation state: {latched_health}, "
            f"{after_health}")

    # The already-paired phone coasted through all of it.
    quiet = first_socket.drain()
    require(all(value.get("type") != "controller_state" for value in quiet) and
            first_socket.closed is None,
            f"the paired phone changed state while pairing was full: {quiet}")
    signal_round_trip(host, first_socket, first["controllerId"],
                      first_state["connectionSequence"])
    rotate(origin, room, room["inviteGeneration"])
    room_state = host.expect(lambda value: value.get("type") == "room_state",
                             "exhausted room_state")
    seat = controllers_of(room_state).get(first["controllerId"], {})
    require(seat.get("seat") == 1 and seat.get("leaseGeneration") == 1,
            f"the paired seat moved under exhaustion: {room_state}")

    # The wire enum is exactly the key the shipped phone client maps to copy.
    controller_source = (shell / "controller/controller.js").read_text(
        encoding="utf-8")
    host_source = (shell / "party/party-host.js").read_text(encoding="utf-8")
    require('service_budget_safe: ["Phone pairing is full right now"'
            in controller_source,
            "the controller page no longer maps service_budget_safe to its copy")
    require("Keyboard, gamepads and this display’s touch controls still "
            "work offline." in controller_source,
            "the controller capacity copy lost its local-play recovery line")
    require('error?.message === "service_budget_safe"' in host_source and
            "Phone pairing is full right now." in host_source,
            "the host page no longer maps service_budget_safe to its copy")

    # Host close still reaches the phone with pairing exhausted.
    close_status, _, closed = request(origin, f"/api/party/{room['roomId']}/close",
                                      {}, room["hostCredential"])
    require(close_status == 200 and closed.get("ok") is True,
            f"close was refused while pairing was exhausted: {close_status}, {closed}")
    first_socket.expect_close(4000, "host_closed")
    host.close()
    return {"exhausted": exhausted, "refusals": len(flood) + 3,
            "reservations": latched_health["reservationRequests"]}


# ------------------------------------------------------------------------ main

def run(args: argparse.Namespace) -> None:
    shell = (ROOT / args.shell_dir).resolve()
    require((shell / "index.html").is_file(), "missing web shell")
    require((shell / "controller/controller.js").is_file(),
            "missing shipped controller page")
    with tempfile.TemporaryDirectory(prefix="mdkr-party-chaos-") as temporary:
        root = Path(temporary)
        state = root / "state"
        log_path = root / "wrangler.log"
        with log_path.open("wb") as log:
            origin = f"http://127.0.0.1:{free_port()}"
            running: list[subprocess.Popen[bytes]] = []

            def boot(ceiling: int) -> None:
                running.append(start_worker(origin, shell, state, log, ceiling))

            def halt() -> None:
                while running:
                    stop_worker(running.pop())

            started = time.monotonic()

            def stage(label: str) -> None:
                if args.verbose:
                    print(f"[{time.monotonic() - started:7.2f}s] {label}",
                          flush=True)

            try:
                boot(SETUP_CEILING)
                stage("worker up")
                session = establish(origin)
                stage("session established")
                restart = restart_proof(origin, boot, halt, session)
                stage("restart proof")
                abuse = abuse_proof(origin, session)
                stage("abuse proof")
                killed = kill_switch_proof(origin, boot, halt, session)
                stage("kill switch proof")
                exhausted = exhaustion_proof(origin, boot, halt, shell, session)
                stage("exhaustion proof")
            finally:
                halt()
        details = log_path.read_text(encoding="utf-8", errors="replace")
        require("ERROR" not in details.upper(),
                "Wrangler reported an error during the chaos test:\n"
                + details[-4000:])
    control_growth = (killed["after"]["admitted"]["controlUnits"] -
                      killed["before"]["admitted"]["controlUnits"])
    print("check_party_service_chaos: PASS — restart rebound both leases onto "
          f"seats {restart['first_state']['seat']}/"
          f"{restart['second_state']['seat']} at lease "
          f"{restart['first_state']['leaseGeneration']}/"
          f"{restart['second_state']['leaseGeneration']}, epoch "
          f"{restart['first_state']['connectionSequence']}, and refused "
          "pre-restart generations (409 invalid_state, 409 invite_rotated); a "
          "112-way abuse flood during the live session bought 0 admission "
          f"units and 0 reservations while {abuse['rotations']} rotations and "
          "signaling kept flowing and no foreign seat moved; the literal-zero "
          "kill switch refused every new pairing (503 service_budget_safe) "
          f"while established control still drew {control_growth} units; "
          f"exhaustion refused {exhausted['refusals']} pairings with the typed "
          "no-store identity-free enum, persisted nothing beyond one latch "
          f"({exhausted['reservations']} reservations unchanged), and the "
          "paired seat coasted to host_closed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError, KeyError, StopIteration,
            subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(f"check_party_service_chaos: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
