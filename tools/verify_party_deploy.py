#!/usr/bin/env python3
"""Verify a deployed Golden Balloon Party origin without touching a phone.

    python3 tools/verify_party_deploy.py --origin https://party.example.com
    python3 tools/verify_party_deploy.py --self-test

`--self-test` runs the identical checks against a real `wrangler dev --local`
Worker on loopback, so the whole verification path is provable with no
Cloudflare account. TLS-only assertions are reported as skipped there and are
required against any https origin.

What it proves, in order:

  1. the launcher and controller documents load over the real transport with
     their promised security headers, and the controller's scripts resolve;
  2. `/api/*` is Worker-served and not shadowed by, nor shadowing, the static
     controller/party/room routes, including trailing-slash and 404 handling;
  3. a room can be created and an invite redeemed over HTTP(S), with every
     credential response marked `no-store`;
  4. the WSS signaling handshake round-trips role-specific envelopes between a
     synthetic host and a synthetic controller, and the service — not the
     client — decides the controller identity stamped on them;
  5. the six-digit fallback code works and rotation invalidates the old invite
     and the old code;
  6. cross-origin and origin-less API requests are refused, static routes are
     not rate-limit shadowed, and no workers.dev host is published;
  7. the deployed static build stamp is the local dist/web build.

No WebRTC media stack is involved: an offer/answer envelope exchange is what
the service is responsible for, and it is what is asserted.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import secrets
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent.parent
SERVICE = ROOT / "services/party"
WRANGLER = SERVICE / "node_modules/wrangler/bin/wrangler.js"
WEB = ROOT / "dist/web"
POLICY = SERVICE / "ops/free-rate-limit-rule.json"

SELF_TEST_HMAC_KEY = "verify-party-deploy-fixture-32-byte-secret"
SELF_TEST_OPS_TOKEN = "verify-party-deploy-ops-fixture-0123456789"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
SDP_OFFER = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
SDP_ANSWER = SDP_OFFER.replace("m=application 9", "m=application 9")


class CheckFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailure(message)


# --------------------------------------------------------------------------
# Minimal RFC 6455 client. A real browser is not available on a headless
# deploy host, and the service's contract here is a text envelope exchange, so
# a dependency-free client keeps this script runnable anywhere Python is.
# --------------------------------------------------------------------------

class WebSocketClient:
    def __init__(self, url: str, protocols: list[str], origin: str,
                 timeout: float = 20.0) -> None:
        parts = urllib.parse.urlsplit(url)
        secure = parts.scheme == "wss"
        port = parts.port or (443 if secure else 80)
        host = parts.hostname or ""
        self.buffer = b""
        self.timeout = timeout
        raw = socket.create_connection((host, port), timeout=timeout)
        if secure:
            context = ssl.create_default_context()
            raw = context.wrap_socket(raw, server_hostname=host)
        self.socket = raw
        self.socket.settimeout(timeout)
        key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
        target = parts.path or "/"
        if parts.query:
            target += "?" + parts.query
        request = [
            f"GET {target} HTTP/1.1",
            f"Host: {parts.netloc}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
            f"Sec-WebSocket-Protocol: {', '.join(protocols)}",
            f"Origin: {origin}",
            "", "",
        ]
        self.socket.sendall("\r\n".join(request).encode("ascii"))
        head = self._read_until(b"\r\n\r\n")
        text = head.decode("latin-1")
        status = text.split("\r\n", 1)[0]
        require(" 101 " in status,
                f"WebSocket upgrade to {parts.path} was refused: {status}")
        expected = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")
        headers = {}
        for line in text.split("\r\n")[1:]:
            if ":" in line:
                name, _, value = line.partition(":")
                headers[name.strip().lower()] = value.strip()
        require(headers.get("sec-websocket-accept") == expected,
                "WebSocket handshake accept token did not validate")
        self.protocol = headers.get("sec-websocket-protocol", "")

    def _read_until(self, marker: bytes) -> bytes:
        while marker not in self.buffer:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise CheckFailure("WebSocket peer closed during handshake")
            self.buffer += chunk
        index = self.buffer.index(marker) + len(marker)
        head, self.buffer = self.buffer[:index], self.buffer[index:]
        return head

    def _read_exact(self, count: int) -> bytes:
        while len(self.buffer) < count:
            try:
                chunk = self.socket.recv(max(4096, count - len(self.buffer)))
            except (socket.timeout, TimeoutError, ssl.SSLWantReadError) as error:
                raise CheckFailure(
                    "timed out waiting for a signaling frame") from error
            if not chunk:
                raise CheckFailure("WebSocket peer closed mid-frame")
            self.buffer += chunk
        value, self.buffer = self.buffer[:count], self.buffer[count:]
        return value

    def send_text(self, value: str) -> None:
        payload = value.encode("utf-8")
        header = bytearray([0x81])
        mask = secrets.token_bytes(4)
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < 1 << 16:
            header.append(0x80 | 126)
            header += struct.pack(">H", length)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", length)
        header += mask
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.socket.sendall(bytes(header) + masked)

    def recv_text(self, timeout: float | None = None) -> Any:
        """Return the next decoded JSON text frame, answering control frames."""
        deadline = time.monotonic() + (timeout if timeout is not None else self.timeout)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CheckFailure("timed out waiting for a signaling frame")
            self.socket.settimeout(remaining)
            first, second = self._read_exact(2)
            opcode = first & 0x0F
            length = second & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read_exact(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read_exact(8))[0]
            if second & 0x80:
                mask = self._read_exact(4)
                payload = bytes(byte ^ mask[index % 4]
                                for index, byte in enumerate(self._read_exact(length)))
            else:
                payload = self._read_exact(length)
            if opcode == 0x9:  # ping
                self.socket.sendall(bytes([0x8A, 0x80]) + secrets.token_bytes(4))
                continue
            if opcode == 0xA:  # pong
                continue
            if opcode == 0x8:  # close
                code = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else 0
                raise CheckFailure(
                    f"signaling socket closed: {code} {payload[2:].decode('utf-8', 'replace')}")
            require(opcode == 0x1, f"unexpected non-text signaling frame: {opcode}")
            return json.loads(payload.decode("utf-8"))

    def recv_signal(self, timeout: float | None = None) -> Any:
        """Next signaling envelope, skipping authoritative state broadcasts.

        Room/controller state is pushed on every commit, so a caller waiting
        for an offer/answer must not mistake a state frame for one.
        """
        deadline = time.monotonic() + (timeout if timeout is not None else self.timeout)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CheckFailure("timed out waiting for a signaling frame")
            value = self.recv_text(remaining)
            if isinstance(value, dict) and value.get("type") in (
                    "room_state", "controller_state"):
                continue
            return value

    def expect_silence(self, seconds: float) -> None:
        """Require that no signaling envelope is delivered within the window."""
        try:
            value = self.recv_signal(seconds)
        except CheckFailure as error:
            if "timed out" in str(error):
                return
            raise
        raise CheckFailure(f"socket received a frame it must not see: {value}")

    def close(self) -> None:
        try:
            self.socket.sendall(bytes([0x88, 0x80]) + secrets.token_bytes(4))
        except OSError:
            pass
        try:
            self.socket.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_args: Any) -> None:
        return None


def http(origin: str, path: str, value: Any | None = None,
         credential: str = "", origin_header: str | None = "",
         method: str | None = None, follow: bool = True
         ) -> tuple[int, dict[str, str], Any]:
    headers: dict[str, str] = {}
    if origin_header == "":
        headers["Origin"] = origin
    elif origin_header is not None:
        headers["Origin"] = origin_header
    data = None
    if value is not None:
        data = json.dumps(value, separators=(",", ":")).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if credential:
        headers["Authorization"] = f"Bearer {credential}"
    request = urllib.request.Request(
        origin + path, data=data, headers=headers,
        method=method or ("POST" if value is not None else "GET"))
    opener = urllib.request.build_opener() if follow else \
        urllib.request.build_opener(NoRedirect)
    try:
        response = opener.open(request, timeout=30)
    except urllib.error.HTTPError as error:
        response = error
    body = response.read()
    content_type = response.headers.get("Content-Type", "")
    decoded: Any = body.decode("utf-8", "replace")
    if "application/json" in content_type:
        try:
            decoded = json.loads(decoded)
        except json.JSONDecodeError:
            pass
    return (response.status,
            {key.lower(): value for key, value in response.headers.items()},
            decoded)


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------

def public_key() -> str:
    """An 87-character base64url token — the service's public-key wire shape."""
    return base64.urlsafe_b64encode(secrets.token_bytes(65)).decode("ascii").rstrip("=")


def assert_documents(origin: str, secure: bool, evidence: list[str]) -> None:
    status, headers, body = http(origin, "/")
    require(status == 200 and "text/html" in headers.get("content-type", ""),
            f"launcher document failed: {status} {headers.get('content-type')}")
    require("<!doctype html" in body.casefold(), "launcher document is not HTML")
    for name, needle in (("content-security-policy", "default-src 'self'"),
                         ("content-security-policy", "frame-ancestors 'none'"),
                         ("permissions-policy", "camera=()"),
                         ("referrer-policy", "no-referrer"),
                         ("x-content-type-options", "nosniff"),
                         ("cross-origin-opener-policy", "same-origin")):
        require(needle in headers.get(name, ""),
                f"launcher document lost {name}: {headers.get(name)!r}")
    if secure:
        require("max-age=" in headers.get("strict-transport-security", ""),
                "deployed launcher does not send HSTS; a phone that scans a QR "
                "code must be pinned to TLS")

    status, headers, page = http(origin, "/controller/")
    require(status == 200 and "text/html" in headers.get("content-type", ""),
            f"controller document failed: {status} {headers.get('content-type')}")
    policy = headers.get("content-security-policy", "")
    require("default-src 'self'" in policy and "script-src 'self'" in policy and
            "frame-ancestors 'none'" in policy,
            f"controller document lost its content security policy: {policy!r}")
    permissions = headers.get("permissions-policy", "")
    require("camera=()" in permissions and "microphone=()" in permissions and
            "geolocation=()" in permissions,
            f"controller document lost its Permissions-Policy: {permissions!r}")
    if secure:
        require("max-age=" in headers.get("strict-transport-security", ""),
                "deployed controller does not send HSTS")

    references = [value.split('="', 1)[1].rstrip('"')
                  for value in re.findall(r'(?:href|src)="[^"]+"', page)]
    scripts = [value for value in references if ".js" in value]
    styles = [value for value in references if ".css" in value]
    require(any("controller.js" in value for value in scripts) and
            any("party-protocol.js" in value for value in scripts) and
            any("party-sas.js" in value for value in scripts),
            f"controller page no longer loads its pad scripts: {scripts}")
    for value in scripts + styles:
        target = urllib.parse.urljoin("/controller/", value)
        status, headers, _ = http(origin, target)
        expected = "javascript" if ".js" in value else "css"
        require(status == 200 and expected in headers.get("content-type", ""),
                f"controller asset {target} failed: {status} "
                f"{headers.get('content-type')}")
    evidence.append(f"controller assets: {len(scripts)} scripts, "
                    f"{len(styles)} stylesheets served")

    # A phone that types the host without the slash must still land on the pad.
    status, headers, _ = http(origin, "/controller", follow=False)
    require(status == 200 or (status in (301, 307, 308) and
            headers.get("location", "").rstrip("/").endswith("/controller")
            or headers.get("location", "").endswith("/controller/")),
            f"/controller did not resolve to /controller/: {status} "
            f"{headers.get('location')!r}")

    for path in ("/room/", "/party/party-host.js", "/party/party-protocol.js",
                 "/party/party-sas.js", "/party/qrcodegen.js",
                 "/party/party-host.css"):
        status, headers, _ = http(origin, path)
        require(status == 200, f"party host route {path} failed: {status}")

    # An unknown path must never be rewritten into a page. dist/web ships no
    # 404.html, so `not_found_handling: 404-page` correctly falls through to the
    # Worker, which answers a typed non-200. What must not happen is a
    # single-page rewrite handing back the launcher or controller document.
    status, headers, missing = http(origin, "/no-such-route-9f3a2b")
    require(status in (404, 405),
            f"unknown static route was not refused: {status}")
    require("<!doctype html" not in str(missing).casefold(),
            "an unknown path was rewritten into an HTML document; "
            "not_found_handling is an SPA rewrite")
    require(headers.get("cache-control") == "no-store",
            f"the unknown-path refusal was cacheable: {headers.get('cache-control')}")


def assert_build_stamp(origin: str, evidence: list[str]) -> None:
    local = json.loads((WEB / "build-info.json").read_text(encoding="utf-8"))
    status, _, deployed = http(origin, "/build-info.json")
    require(status == 200 and isinstance(deployed, dict),
            f"deployed /build-info.json is not readable: {status}")
    require(deployed == local,
            "the deployed static build is not this dist/web: deployed "
            f"{deployed.get('source_commit')} vs local {local.get('source_commit')}")
    _, _, page = http(origin, "/")
    found = re.search(r'data-build-stamp="([^"]+)"', page)
    if found:
        require(found.group(1) == local.get("source_commit_short"),
                f"published build stamp {found.group(1)} does not match the local "
                f"build {local.get('source_commit_short')}")
        evidence.append(f"build stamp: {found.group(1)} (published)")
    else:
        evidence.append(f"build stamp: {local.get('source_commit_short')} "
                        "(unstamped dist/web served directly)")


def assert_origin_isolation(origin: str, evidence: list[str]) -> None:
    for label, header in (("cross-origin", "https://attacker.example"),
                          ("null-origin", "null")):
        status, headers, body = http(origin, "/api/party/create",
                                     {"hostPublicKey": public_key()},
                                     origin_header=header)
        require(status == 403 and body == {"error": "origin_forbidden"},
                f"{label} API request was not refused: {status} {body}")
        require(headers.get("cache-control") == "no-store",
                f"{label} refusal was cacheable")
    status, _, body = http(origin, "/api/party/create",
                           {"hostPublicKey": public_key()}, origin_header=None)
    require(status == 403 and body == {"error": "origin_forbidden"},
            f"origin-less browser-shaped API request was not refused: {status} {body}")
    status, headers, body = http(origin, "/api/not-a-route", {})
    require(status == 404 and body == {"error": "not_found"} and
            headers.get("cache-control") == "no-store",
            f"/api/ is not Worker-served — a static 404 answered instead: "
            f"{status} {body}")
    evidence.append("origin isolation: cross-origin, null and origin-less "
                    "API requests refused 403")


def assert_static_not_shadowed(origin: str, evidence: list[str]) -> None:
    """The one free edge rule matches /api/ only; static must survive a burst."""
    policy = json.loads(POLICY.read_text(encoding="utf-8"))
    expression = policy["rules"][0]["expression"]
    require(expression == 'starts_with(http.request.uri.path, "/api/")',
            f"the reviewed edge rule is no longer /api/-only: {expression}")
    burst = policy["rules"][0]["ratelimit"]["requests_per_period"]
    started = time.monotonic()
    codes = []
    for _ in range(burst + 10):
        status, _, _ = http(origin, "/controller/")
        codes.append(status)
    elapsed = time.monotonic() - started
    require(all(status == 200 for status in codes),
            f"a static burst of {len(codes)} was throttled: "
            f"{sorted(set(codes))} — the edge rule is shadowing static routes")
    evidence.append(f"static burst: {len(codes)} x /controller/ in "
                    f"{elapsed:.1f}s, all 200")


def assert_no_workers_dev(origin: str, workers_dev_host: str | None,
                          evidence: list[str]) -> None:
    text = (SERVICE / "wrangler.jsonc").read_text(encoding="utf-8")
    require(text.count('"workers_dev": false') >= 2 and
            '"workers_dev": true' not in text,
            "the Wrangler config no longer disables workers.dev in both the "
            "default and production environments")
    require('"preview_urls": false' in text,
            "the production environment no longer disables preview URLs")
    if workers_dev_host:
        try:
            status, _, _ = http(f"https://{workers_dev_host}", "/")
            require(False, f"{workers_dev_host} is reachable and served {status}; "
                           "a second unrestricted origin is published")
        except (urllib.error.URLError, OSError, socket.gaierror):
            evidence.append(f"workers.dev: {workers_dev_host} unreachable")
    else:
        evidence.append("workers.dev: disabled in config (no host given to probe)")


def assert_pairing(origin: str, evidence: list[str]) -> None:
    host_key = public_key()
    status, headers, room = http(origin, "/api/party/create",
                                 {"hostPublicKey": host_key})
    require(status == 201, f"room create failed: {status} {room}")
    require(headers.get("cache-control") == "no-store" and
            headers.get("referrer-policy") == "no-referrer",
            f"credential response was not no-store/no-referrer: {headers}")
    room_id = room["roomId"]
    host_credential = room["hostCredential"]
    controller_url = room["controllerUrl"]
    require(controller_url.startswith(origin + "/controller/#"),
            f"controllerUrl does not point at this origin: {controller_url}")
    capability = controller_url.split("#", 1)[1]

    status, headers, controller = http(origin, "/api/controller/redeem",
        {"capability": capability, "protocol": 1,
         "controllerPublicKey": public_key(), "name": "Verifier"})
    require(status == 201, f"invite redeem failed: {status} {controller}")
    require(headers.get("cache-control") == "no-store",
            "controller credential response was not no-store")
    controller_id = controller["controllerId"]
    require(re.fullmatch(r"[A-Za-z0-9_-]{22}", controller_id),
            f"controller identity has the wrong shape: {controller_id}")
    require(controller["hostPublicKey"] == host_key,
            "redeem did not hand the controller the real host key")

    scheme = "wss" if origin.startswith("https") else "ws"
    base = scheme + ":" + origin.split(":", 1)[1]
    host_socket = WebSocketClient(f"{base}/api/party/{room_id}/connect",
                                  ["gb-control-v1", f"gb-host.{host_credential}"],
                                  origin)
    controller_socket: WebSocketClient | None = None
    try:
        require(host_socket.protocol == "gb-control-v1",
                f"host socket negotiated {host_socket.protocol!r}")
        state = host_socket.recv_text()
        require(isinstance(state, dict) and state.get("type") == "room_state" and
                any(item.get("controllerId") == controller_id
                    for item in state.get("controllers", [])),
                f"host socket did not open on the authoritative room state: {state}")
        require(not any(key in json.dumps(state) for key in
                        (host_credential, controller["credential"], capability,
                         room["fallbackCode"])),
                "the host room state leaked a credential, invite or code")

        controller_socket = WebSocketClient(
            f"{base}/api/party/{room_id}/connect",
            ["gb-control-v1", f"gb-controller.{controller['credential']}"], origin)
        opened = controller_socket.recv_text()
        require(opened.get("type") == "controller_state" and
                opened.get("phase") == "pending",
                f"controller socket did not open on its own state: {opened}")

        # Identity injection: the controller claims someone else's identity and
        # the service must overwrite it with the socket's bound identity.
        forged = "A" * 22
        require(forged != controller_id, "identity fixture collided")
        controller_socket.send_text(json.dumps(
            {"type": "controller_hello", "controllerId": forged}))
        hello = host_socket.recv_signal()
        require(hello == {"type": "controller_hello",
                          "controllerId": controller_id},
                f"the service did not stamp the real phone identity: {hello}")

        status, _, approved = http(origin, f"/api/party/{room_id}/approve",
                                   {"controllerId": controller_id, "seat": 1},
                                   host_credential)
        require(status == 200, f"host approval failed: {status} {approved}")
        promoted = controller_socket.recv_text()
        require(promoted.get("type") == "controller_state" and
                promoted.get("phase") == "leased" and promoted.get("seat") == 1 and
                promoted.get("leaseGeneration") == 1,
                f"approved controller was not told its seat: {promoted}")

        # Role-specific envelopes: host offer -> controller, answer -> host.
        host_socket.send_text(json.dumps(
            {"type": "webrtc_offer", "to": controller_id, "peerGeneration": 1,
             "sdp": {"type": "offer", "sdp": SDP_OFFER}}))
        offer = controller_socket.recv_signal()
        require(offer == {"type": "webrtc_offer", "to": controller_id,
                          "peerGeneration": 1,
                          "sdp": {"type": "offer", "sdp": SDP_OFFER}},
                f"the offer envelope did not round-trip intact: {offer}")

        # A host offer aimed at a different controller must not be delivered.
        host_socket.send_text(json.dumps(
            {"type": "webrtc_offer", "to": "B" * 22, "peerGeneration": 2,
             "sdp": {"type": "offer", "sdp": SDP_OFFER}}))
        controller_socket.expect_silence(1.5)

        controller_socket.send_text(json.dumps(
            {"type": "webrtc_answer", "controllerId": forged, "peerGeneration": 1,
             "sdp": {"type": "answer", "sdp": SDP_ANSWER}}))
        answer = host_socket.recv_signal()
        require(answer == {"type": "webrtc_answer", "controllerId": controller_id,
                           "peerGeneration": 1,
                           "sdp": {"type": "answer", "sdp": SDP_ANSWER}},
                f"the answer envelope was not identity-stamped: {answer}")

        controller_socket.send_text(json.dumps(
            {"type": "webrtc_ice", "controllerId": forged, "peerGeneration": 1,
             "candidate": {"candidate": "candidate:1 1 udp 1 127.0.0.1 9 typ host",
                           "sdpMid": "0", "sdpMLineIndex": 0}}))
        ice = host_socket.recv_signal()
        require(ice.get("type") == "webrtc_ice" and
                ice.get("controllerId") == controller_id,
                f"the ICE envelope was not identity-stamped: {ice}")

        # An unknown envelope type must terminate the socket rather than be
        # forwarded to the other role.
        controller_socket.send_text(json.dumps(
            {"type": "pad_state", "controllerId": controller_id}))
        closed = False
        try:
            controller_socket.recv_signal(3)
        except CheckFailure as error:
            closed = "socket closed" in str(error) or "closed mid-frame" in str(error)
        require(closed, "an unknown signaling envelope did not close the socket")
        host_socket.expect_silence(1.0)
    finally:
        if controller_socket is not None:
            controller_socket.close()
        host_socket.close()
    evidence.append(f"signaling: offer/answer/ICE round-trip on {scheme.upper()}, "
                    "phone identity injected by the service")

    # Fallback code path.
    status, headers, fallback = http(origin, "/api/controller/code",
        {"code": room["fallbackCode"], "protocol": 1,
         "controllerPublicKey": public_key(), "name": "Fallback"})
    require(status == 201, f"fallback code redeem failed: {status} {fallback}")
    require(headers.get("cache-control") == "no-store",
            "fallback credential response was not no-store")

    # Rotation must invalidate both the old invite and the old code.
    status, _, rotated = http(origin, f"/api/party/{room_id}/rotate",
                              {"expectedInviteGeneration": room["inviteGeneration"]},
                              host_credential)
    require(status == 200 and rotated["inviteGeneration"] ==
            room["inviteGeneration"] + 1,
            f"invite rotation failed: {status} {rotated}")
    new_capability = rotated["controllerUrl"].split("#", 1)[1]
    require(new_capability != capability and
            rotated["fallbackCode"] != room["fallbackCode"],
            "rotation reissued the same invite or code")

    status, _, stale = http(origin, "/api/controller/redeem",
        {"capability": capability, "protocol": 1,
         "controllerPublicKey": public_key()})
    require(status != 201, f"the rotated-away invite still redeems: {status} {stale}")
    status, _, stale_code = http(origin, "/api/controller/code",
        {"code": room["fallbackCode"], "protocol": 1,
         "controllerPublicKey": public_key()})
    require(status != 201,
            f"the rotated-away fallback code still redeems: {status} {stale_code}")
    status, _, fresh = http(origin, "/api/controller/redeem",
        {"capability": new_capability, "protocol": 1,
         "controllerPublicKey": public_key()})
    require(status == 201, f"the rotated invite does not redeem: {status} {fresh}")
    evidence.append("invites: fallback code paired; rotation invalidated the old "
                    "link and code and issued a working pair")

    status, _, closed = http(origin, f"/api/party/{room_id}/close", {},
                             host_credential)
    require(status == 200, f"host could not close the verification room: {closed}")


def verify(origin: str, workers_dev_host: str | None,
           evidence: list[str]) -> None:
    secure = origin.startswith("https://")
    if not secure:
        evidence.append("TLS: SKIPPED (loopback self-test); required on https")
    else:
        evidence.append(f"TLS: certificate validated for {origin}")
    assert_documents(origin, secure, evidence)
    assert_build_stamp(origin, evidence)
    assert_origin_isolation(origin, evidence)
    assert_pairing(origin, evidence)
    assert_static_not_shadowed(origin, evidence)
    assert_no_workers_dev(origin, workers_dev_host, evidence)


# --------------------------------------------------------------------------
# Local self-test Worker
# --------------------------------------------------------------------------

def node_binary() -> Path:
    candidates: list[Path] = []
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        candidate = Path(directory) / "node"
        if candidate.is_file() and os.access(candidate, os.X_OK):
            candidates.append(candidate)
    candidates.extend(sorted(
        (Path.home() / ".nvm/versions/node").glob("v*/bin/node"), reverse=True))
    for candidate in candidates:
        try:
            version = subprocess.check_output(
                [str(candidate), "-p", "process.versions.node"], text=True).strip()
            if int(version.split(".", 1)[0]) >= 22:
                return candidate.resolve()
        except (OSError, subprocess.SubprocessError, ValueError):
            continue
    raise CheckFailure("Node 22+ is required to run the local Party Worker")


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def wait_worker(origin: str, process: subprocess.Popen[bytes],
                timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise CheckFailure(
                f"local Party Worker exited before readiness ({process.returncode})")
        try:
            with urllib.request.urlopen(origin + "/", timeout=0.5) as response:
                if response.status == 200:
                    return
        except OSError:
            time.sleep(0.1)
    raise CheckFailure("local Party Worker did not become ready")


def self_test(evidence: list[str], workers_dev_host: str | None) -> None:
    require(WRANGLER.is_file(),
            "missing the lockfile-pinned Wrangler; run (cd services/party && npm ci)")
    require((WEB / "index.html").is_file(), "missing dist/web")
    port = free_port()
    origin = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="mdkr-verify-party-") as temporary:
        state = Path(temporary)
        log_path = state / "wrangler.log"
        with log_path.open("wb") as log:
            command = [str(node_binary()), str(WRANGLER), "dev", "--local",
                       "--ip", "127.0.0.1", "--port", str(port),
                       "--inspector-port", "0",
                       "--persist-to", str(state / "persist"),
                       "--var", f"PARTY_ORIGIN:{origin}",
                       "--var", f"PARTY_HMAC_KEY:{SELF_TEST_HMAC_KEY}",
                       "--var", f"OPS_READ_TOKEN:{SELF_TEST_OPS_TOKEN}",
                       "--var", "MAX_ADMISSIONS_PER_DAY:10000",
                       "--var", "CONTROL_RESERVE_PER_DAY:5000",
                       "--log-level", "error"]
            process = subprocess.Popen(command, cwd=SERVICE, stdout=log,
                                       stderr=subprocess.STDOUT)
            try:
                wait_worker(origin, process, 90)
                evidence.append(f"self-test: real wrangler dev --local on {origin}")
                verify(origin, workers_dev_host, evidence)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
        details = log_path.read_text(encoding="utf-8", errors="replace")
        require("ERROR" not in details.upper(),
                "the local Party Worker logged an error:\n" + details[-4000:])


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify a deployed Party origin end to end, without a phone.")
    parser.add_argument("--origin",
                        help="deployed origin, e.g. https://party.example.com")
    parser.add_argument("--self-test", action="store_true",
                        help="run everything against a local wrangler dev Worker")
    parser.add_argument("--workers-dev-host",
                        help="optional workers.dev host that must NOT resolve")
    args = parser.parse_args()
    require(bool(args.origin) != bool(args.self_test),
            "pass exactly one of --origin or --self-test")

    evidence: list[str] = []
    started = time.monotonic()
    if args.self_test:
        self_test(evidence, args.workers_dev_host)
        target = "local wrangler dev --local Worker"
    else:
        origin = args.origin.rstrip("/")
        require(origin.startswith("https://"),
                "a deployed Party origin must be HTTPS: phones load the "
                "controller from a QR code and WSS requires TLS")
        verify(origin, args.workers_dev_host, evidence)
        target = origin
    for line in evidence:
        print(f"  - {line}")
    print(f"verify_party_deploy: PASS — {target} serves the launcher and "
          "controller with their promised headers, pairs a synthetic phone over "
          "WSS with service-stamped identity, rotates invites, refuses foreign "
          "origins and matches the local dist/web build "
          f"({time.monotonic() - started:.1f}s)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CheckFailure, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"verify_party_deploy: FAIL — {error}", file=sys.stderr)
        raise SystemExit(1)
