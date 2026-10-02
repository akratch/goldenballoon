#!/usr/bin/env python3
"""Standalone process-level peer-loss lane for the beta online-live stack.

This lane pins the Phase-1 promise that a survivor endpoint whose opponent's
transport is SEVERED mid-session, or whose opponent NEVER fully joins, tears
down cleanly WITHOUT fabricating a finished/published race -- the process-level
shadow of the engine's OPPONENT_LEFT / OPPONENT_NEVER_STARTED recovery routing.

It reuses the exact harness the live-transport capstone (check_online_live_transport_e2e.py)
uses -- the native `mdkr_online_live_transport_e2e_driver` -- but drives it
against small in-process FAKE servers (no wrangler, no Durable Object, no ROM),
so it is deterministic and fast. The driver narrates every transition as an
`[E2E] key=value` line; the two invariants asserted per arm are:

  * the driver NEVER prints `[E2E] installed` (no race descriptor is installed
    against a peer that is gone / never arrived), and
  * the driver NEVER prints `[E2E] result=ok` (it never converges to a finished,
    hash-agreed race and so never publishes results),

while the process exits within its own budget (its transport does NOT wedge --
the mid-frame-stall teardown contract the live-transport driver already owns).

Two arms:

  A. severed transport (peer-loss shadow): the fake room answers create with a
     reducer-valid one-seat room, upgrades the `/connect` state socket, holds it
     briefly so the survivor sees a live link, then SEVERS it (closes the
     socket). The survivor must react and exit within its budget having reached
     the room (`[E2E] room=`) but never `installed`/`result=ok`.

  B. peer never joins (never-started shadow): the fake room serves the same valid
     one-seat room and keeps `/connect` open and silent -- a second endpoint
     never appears. The survivor waits for the opponent, never installs a
     descriptor, never authors a converged race, and exits at its budget.

WHAT THIS LANE DOES NOT PROVE -- read before trusting a green run. The
transport driver is transport-only: it does NOT boot the visible engine, so it
does NOT emit the ENGINE-level witnesses `[online-live] peer lost mid-race ...`
(OPPONENT_LEFT) or `[START] race-start barrier: ... aborting to the room`
(OPPONENT_NEVER_STARTED), nor does it send the `race_abort` control message.
Those live in the visible engine session (platform/app/main_app.cpp) and are
only reachable through the two-process MDKR_APP_TEST_ONLINE_LIVE_CLOUD path,
which needs a live MatchRoom Worker (the DEV `check_online_live_transport_e2e`
territory), or through a loopback fault-injection seam that does not exist in
this tree. The pure engine decisions those witnesses gate ARE pinned elsewhere,
at the unit level:

  * the peer-loss -> failure mapping (PingTimeout/PeerEnded in-race ->
    OPPONENT_LEFT, SealWindowExhausted -> CONNECTION_UNPLAYABLE) and the
    no-demotion rule: tests/test_online_live_adapter_beta.cpp
    (ctest `online_live_adapter_beta`);
  * the OPPONENT_LEFT / OPPONENT_NEVER_STARTED / CONNECTION_UNPLAYABLE recovery
    CARD copy (distinct titles, "Play Here" primary, no CONNECTION_CHECK reuse):
    tests/test_online_lobby_view_model.c (ctest `online_lobby_view_model`).

So this lane proves the TRANSPORT-layer truth (no ghost install / no ghost
finish / no wedge under severance or peer-absence) and the unit tests above
prove the ENGINE-layer reason mapping + recovery copy; the literal end-to-end
engine witness under a killed live peer is the one thing neither covers here.
The `race_abort` control-message SEND has no automated coverage at all (noted).

Standalone lane, NOT run-checks / CTest registered: it spawns the driver and
binds local ports, so run it on a quiet machine one at a time:

    python3 tests/check_online_peer_loss.py --build build-beta
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import harness_utils  # noqa: F401,E402 -- a direct run gets private saves, prefs and mods

ROOT = Path(__file__).resolve().parent.parent
TEST_TOKEN = "mdkr64-party-e2e-v1"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

ROOM_RE = re.compile(r"^\[E2E\] room=(\S+) code=(\d{6})$")
INSTALLED_RE = re.compile(r"^\[E2E\] installed\b")
RESULT_OK_RE = re.compile(r"^\[E2E\] result=ok\b")
RESULT_END_RE = re.compile(r"^\[E2E\] result=(ok|error|timeout)\b")


def _valid_created_room() -> bytes:
    """A minimal reducer-valid create response: identity + a one-seat lobby that
    passes mdkr_online_lobby_valid, so the driver reaches ROOM and waits for a
    second endpoint that (per this lane) never arrives."""
    compatibility = {
        "protocolVersion": 1,
        "buildId": list(range(1, 17)),
        "gameplayDigest": [0x80 + i for i in range(32)],
        "romRevision": 1,
        "cadenceHz": 30,
    }
    body = {
        "type": "match_created",
        "schemaVersion": 1,
        "roomId": "0123456789abcdefABCDEF",
        "credential": "c" * 43,
        "endpointId": "257",
        "fallbackCode": "123456",
        "inviteUrl": "http://127.0.0.1/room/#match=" + ("m" * 43),
        "iceServers": [],
        "lobby": {
            "protocolVersion": 1, "revision": 1, "matchEpoch": 0,
            "leaderGeneration": 1, "roomId": "12345", "leaderEndpointId": "257",
            "phase": "lobby", "compatibility": compatibility,
            "members": [{"endpointId": "257", "seatCount": 1, "connected": True,
                         "ready": False, "loaded": False}],
            "seats": [{"endpointId": "257", "selectionRevision": 0,
                       "localIndex": 0, "voteTrack": None, "characterId": None,
                       "vehicleId": None}],
            "selectedTrack": None, "selectedVehicleMask": 0,
        },
    }
    payload = json.dumps(body, separators=(",", ":")).encode()
    return (b"HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n"
            b"Cache-Control: no-store\r\nContent-Length: " +
            str(len(payload)).encode() + b"\r\nConnection: close\r\n\r\n" +
            payload)


class FakeRoomServer:
    """Answers create with a valid one-seat room and upgrades /connect.

    mode="sever": upgrade /connect, hold it open for ``sever_after`` seconds so
        the survivor sees a live link, then close the socket (transport severed).
    mode="quiet": upgrade /connect and keep it open and silent forever -- a
        second endpoint never appears.
    """

    def __init__(self, mode: str, sever_after: float = 1.5) -> None:
        assert mode in ("sever", "quiet")
        self.mode = mode
        self.sever_after = sever_after
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self._stop = False
        self._held: list[socket.socket] = []
        self._lock = threading.Lock()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    @property
    def origin(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    def _run(self) -> None:
        self.sock.settimeout(0.3)
        while not self._stop:
            try:
                conn, _ = self.sock.accept()
            except (socket.timeout, OSError):
                continue
            threading.Thread(target=self._handle, args=(conn,),
                             daemon=True).start()

    def _handle(self, conn: socket.socket) -> None:
        conn.settimeout(5.0)
        data = b""
        try:
            while b"\r\n\r\n" not in data and len(data) < 65536:
                chunk = conn.recv(4096)
                if not chunk:
                    conn.close()
                    return
                data += chunk
        except OSError:
            conn.close()
            return
        head = data.split(b"\r\n\r\n", 1)[0].decode("latin1")
        request_line = head.split("\r\n", 1)[0]
        if request_line.startswith("POST /api/match/create"):
            try:
                conn.sendall(_valid_created_room())
            except OSError:
                pass
            conn.close()
            return
        if "/connect" in request_line and "GET" in request_line:
            key = ""
            for line in head.split("\r\n")[1:]:
                if line.lower().startswith("sec-websocket-key:"):
                    key = line.split(":", 1)[1].strip()
            accept = base64.b64encode(
                hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
            response = (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n"
                "Sec-WebSocket-Protocol: gb-match-v1\r\n\r\n").encode()
            try:
                conn.sendall(response)
            except OSError:
                conn.close()
                return
            with self._lock:
                self._held.append(conn)
            if self.mode == "sever":
                # Hold the live link briefly, then sever it (close the socket).
                def _sever(sock: socket.socket) -> None:
                    time.sleep(self.sever_after)
                    try:
                        sock.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    try:
                        sock.close()
                    except OSError:
                        pass
                threading.Thread(target=_sever, args=(conn,),
                                 daemon=True).start()
            # mode="quiet": keep the socket open and silent (no frames ever).
            return
        try:
            conn.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                         b"Connection: close\r\n\r\n")
        except OSError:
            pass
        conn.close()

    def close(self) -> None:
        self._stop = True
        with self._lock:
            held = list(self._held)
        for conn in held:
            try:
                conn.close()
            except OSError:
                pass
        try:
            self.sock.close()
        except OSError:
            pass


class Driver:
    """A narrated online-race driver process, tailing its [E2E] stdout."""

    def __init__(self, binary: Path, args: list[str], verbose: bool) -> None:
        import os
        environment = dict(os.environ)
        environment["MDKR_INTERNAL_TEST_TOKEN"] = TEST_TOKEN
        self.verbose = verbose
        self.lines: list[str] = []
        self._lock = threading.Lock()
        self.proc = subprocess.Popen(
            [str(binary), *args], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, env=environment, text=True, bufsize=1)
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if self.verbose:
                print(f"driver: {line}", flush=True)
            with self._lock:
                self.lines.append(line)

    def snapshot(self) -> list[str]:
        with self._lock:
            return list(self.lines)

    def wait_exit(self, timeout: float) -> int | None:
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        if self.proc.stdout is not None:
            self.proc.stdout.close()


class CheckFailure(Exception):
    pass


def run_arm(binary: Path, mode: str, timeout_ms: int, verbose: bool) -> str:
    """Run one severance/absence arm. Returns a one-line verdict on success."""
    server = FakeRoomServer(mode)
    driver: Driver | None = None
    # Generous wall budget: the driver's own --timeout-ms plus teardown slack.
    wall = timeout_ms / 1000.0 + 15.0
    try:
        driver = Driver(
            binary,
            ["--origin", server.origin, "--journey", "create",
             "--character", "1", "--track", "5", "--ticks", "1",
             "--timeout-ms", str(timeout_ms)],
            verbose)
        started = time.monotonic()
        code = driver.wait_exit(wall)
        elapsed = time.monotonic() - started
        lines = driver.snapshot()
        if code is None:
            raise CheckFailure(
                f"{mode}: driver did not exit within {wall:.0f}s -- its "
                f"transport WEDGED under peer loss; last lines: {lines[-8:]}")
        got_room = any(ROOM_RE.match(line) for line in lines)
        installed = any(INSTALLED_RE.match(line) for line in lines)
        finished_ok = any(RESULT_OK_RE.match(line) for line in lines)
        end = next((line for line in lines if RESULT_END_RE.match(line)), None)
        if not got_room:
            raise CheckFailure(
                f"{mode}: driver never reached the room ([E2E] room=) against "
                f"the valid-create fake -- arm is vacuous; last lines: "
                f"{lines[-8:]}")
        if installed:
            raise CheckFailure(
                f"{mode}: driver INSTALLED a race descriptor against a peer that "
                f"was {mode!r} -- a ghost race started; last lines: {lines[-8:]}")
        if finished_ok:
            raise CheckFailure(
                f"{mode}: driver reported result=ok (a finished, hash-agreed "
                f"race) with no live peer -- a ghost finish/publish; last lines: "
                f"{lines[-8:]}")
        return (f"{mode}: reached room, then peer {mode} -> no descriptor "
                f"install, no converged finish, clean exit in {elapsed:.1f}s "
                f"(code {code}, end={end!r})")
    finally:
        if driver is not None:
            try:
                driver.close()
            except OSError:
                pass
        server.close()


def resolve_binary(build: str) -> Path:
    path = Path(build).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    return (path / "mdkr_online_live_transport_e2e_driver").resolve()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--timeout-ms", type=int, default=8000,
                        help="the driver's own budget per arm (default 8000)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = resolve_binary(args.build)
    if not binary.is_file():
        print(f"FAIL online peer loss: missing driver {binary}; build "
              "mdkr_online_live_transport_e2e_driver first "
              "(needs -DMDKR_NATIVE_PHONE_PARTY=ON)", file=sys.stderr)
        return 1

    verdicts: list[str] = []
    try:
        for mode in ("sever", "quiet"):
            verdict = run_arm(binary, mode, args.timeout_ms, args.verbose)
            print(f"  {verdict}", flush=True)
            verdicts.append(verdict)
    except CheckFailure as error:
        print(f"FAIL online peer loss: {error}", file=sys.stderr)
        return 1

    print("PASS online peer loss: a survivor whose peer's transport was severed "
          "mid-session, and one whose peer never joined, each reached the room "
          "but installed NO descriptor, reported NO converged finish, and tore "
          "down without wedging -- the transport-layer shadow of OPPONENT_LEFT / "
          "OPPONENT_NEVER_STARTED (engine reason-mapping + recovery copy are "
          "unit-pinned in online_live_adapter_beta / online_lobby_view_model)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
