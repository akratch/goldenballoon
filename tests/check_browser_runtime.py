#!/usr/bin/env python3
"""Run the shipped Emscripten/WebGPU game in a real Chromium browser.

This is the browser counterpart to the native behavioural suite. It serves the
committed shell with the freshly linked ``build-web`` engine, selects the legal
external ROM through the real file input, and drives the same frame-indexed input
fixture used by native checks. Chrome is controlled through the DevTools Protocol
using only the Python standard library; no Playwright, Puppeteer, Selenium, or
checked-in game data is required.

The first browser document must:

* pass the real adapter gate and ROM identity/byte-order path;
* instantiate WebGPU/wasm and advance a 3600-opportunity rAF-driven loop;
* follow the title-to-Time-Trial fixture into an actual race;
* complete a non-vacuous delayed-input rollback, exact replay, and
  effect-journal commit inside the production WebAssembly runtime;
* produce non-flat, changing screenshots across menus and gameplay;
* sustain the contained authored ~30 Hz NTSC complete-loop cadence without
  crashes, one-field simulation leaks, or device loss;
* consume PCM through the real AudioWorklet with measured event-queue headroom;
* enter and exit real element fullscreen while wasm continues presenting;
* propagate live CSS-size and DPR changes into exact engine drawables;
* create and flush the 512-byte EEPROM into IDBFS;
* make no network request containing or naming the ROM.

The document is then reloaded in the same isolated Chrome profile. The shell
must find the persisted ROM without another file selection, restore the exact
EEPROM bytes before ``main()``, and boot again. Finally, the two recovery buttons
are exercised: clearing progress must retain the ROM, and forgetting the ROM must
leave both stores empty. The short reload also forces a one-entry SFX queue to
prove that the drop detector fails in the intended direction.

The shell's regression bridge is inert for normal visitors. CDP installs its
configuration before page JavaScript runs; fixture text exists only in the test
process and the module's private MEMFS.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
import queue
import re
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib
from collections import deque
from dataclasses import dataclass
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable

from harness_utils import (completed_tick_conservation, config_checksum,
                           slot_checksum_valid)


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROM = "baserom.us.v80.z64"
DEFAULT_SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
ROM_BYTES = 12 * 1024 * 1024
BROWSER_FRAME_P95_BUDGET_MS = 40.0
BROWSER_FRAME_P99_BUDGET_MS = 45.0
BROWSER_PIPELINE_FRAME_BUDGET = 2
BROWSER_GPU_IN_FLIGHT_CAP = 2

PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+) R=(\d+) wf=(\d+) cumwf=(\d+) dtms=([0-9.]+)"
    r"(?: \| racer x=([-+0-9.einf]+) y=([-+0-9.einf]+)"
    r" z=([-+0-9.einf]+) clock=(-?\d+) cp=(-?\d+) lap=(-?\d+))?"
)
PACE_INIT_RE = re.compile(
    r"pace init: mode=realtime cadence=original minFields=2 compensate=1 "
    r"synthFields=2 fieldHz=60 sourceFieldHz=60"
)
EVTQ_PEAK_RE = re.compile(
    r"\[EVTQ\] q(\d+)\(([^)]+)\) new peak (\d+) of (\d+)"
)
EVTQ_DROP_RE = re.compile(
    r"\[EVTQ\] post DROPPED q(\d+)\(([^)]+)\) type=(-?\d+) "
    r"peak=(\d+) of (\d+) total=(\d+)"
)
RAW16_ACTIVE_RE = re.compile(
    r"\[AUDIO\] raw16 active mode=(fixed|legacy) loads=(\d+) bytes=(\d+)"
)
FONT_RE = re.compile(
    r"\[FONT\] sdfUploads=(\d+) outlineUploads=(\d+) registryFailures=(\d+) "
    r"clippedTexels=(\d+)")
MIP_RE = re.compile(r"\[MIP\] uploads=(\d+) levels=(\d+)")
DISPLAY_RE = re.compile(
    r"\[DISPLAY\] output=(\d+)x(\d+) render=(\d+)x(\d+) "
    r"scale=([0-9.]+) effectiveScale=([0-9.]+)"
)
RL1_RE = re.compile(r"\[RL1\] arm=([a-z-]+) triangles=(\d+)")
LIGHT_RE = re.compile(
    r"\[LIGHT\] arm=([a-z-]+) valid=(\d+) .* "
    r"strength=([-0-9.]+) sources=(0x[0-9a-fA-F]+) "
    r"racerTris=(\d+) characterTris=(\d+) missingNormals=(\d+) "
    r"space=([a-z0-9/-]+)"
)
WGPU_LIMIT_RE = re.compile(
    r"\[WGPU-LIMITS\]\s+shaders=(\d+)/(\d+)\s+pipelines=(\d+)\s+"
    r"attrs=(\d+)/(\d+)\s+varyings=(\d+)/(\d+)\s+tableOverflow=(\d+)\s+"
    r"pipelineFailures=(\d+)\s+vertexBytes=(\d+)\s+vertexSegments=(\d+)\s+"
    r"vertexSegmentCap=(\d+)\s+textures=(\d+)\s+samplers=(\d+)\s+"
    r"bindGroups=(\d+)/(\d+)"
)
WGPU_SHUTDOWN_RE = re.compile(
    r"\[WGPU-SHUTDOWN\]\s+roots=(owned|borrowed)\s+shaders=(\d+)\s+"
    r"textures=(\d+)\s+pendingPipelines=(\d+)\s+liveChildren=(\d+)\s+"
    r"cpuArrays=(\d+)"
)
ROLLBACK_READY_RE = re.compile(
    r"^\[ROLLBACK\] lab ready: ranges=(\d+) snapshot=(\d+) bytes "
    r"ring=(\d+) bytes target=(\d+) epoch=0$",
    re.MULTILINE,
)
ROLLBACK_STATS_RE = re.compile(
    r"^\[ROLLBACK\] lab stats: ticks=(\d+) captures=(\d+) restores=(\d+) "
    r"capture_avg_ns=(\d+) capture_p50_ns=(\d+) capture_p95_ns=(\d+) "
    r"capture_p99_ns=(\d+) capture_max_ns=(\d+) restore_avg_ns=(\d+) "
    r"restore_p50_ns=(\d+) restore_p95_ns=(\d+) restore_p99_ns=(\d+) "
    r"restore_max_ns=(\d+) timing_overflow=(\d+)/(\d+) "
    r"over_8333333ns=(\d+)/(\d+) over_16666667ns=(\d+)/(\d+)$",
    re.MULTILINE,
)
ROLLBACK_RESIM_RE = re.compile(
    r"^\[ROLLBACK\] resimulation stats: samples=(\d+) avg_ns=(\d+) "
    r"p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+) max_ns=(\d+) "
    r"timing_overflow=(\d+) over_8333333ns=(\d+) "
    r"over_16666667ns=(\d+)$",
    re.MULTILINE,
)
ROLLBACK_FRAME_RE = re.compile(
    r"^\[ROLLBACK\] authored-frame stats: samples=(\d+) avg_ns=(\d+) "
    r"p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+) max_ns=(\d+) "
    r"timing_overflow=(\d+) over_8333333ns=(\d+) "
    r"over_16666667ns=(\d+)$",
    re.MULTILINE,
)
ROLLBACK_EFFECTS_RE = re.compile(
    r"^\[ROLLBACK\] effects: tracked=(\d+) emitted=(\d+) "
    r"duplicates=(\d+) committed=(\d+) cancelled=(\d+) "
    r"overflows=(\d+) forbidden_io=(\d+)$",
    re.MULTILINE,
)
ROLLBACK_ITEM_ARM_RE = re.compile(
    r"^\[ROLLBACK\] item probe armed: balloon=(\d+) level=(\d+) "
    r"weapon=(-?\d+) quantity=(\d+) release=(\d+) mutation=(\d+)$",
    re.MULTILINE,
)
ROLLBACK_ITEM_RESULT_RE = re.compile(
    r"^\[ROLLBACK\] item probe result: balloon=(\d+) level=(\d+) "
    r"weapon=(-?\d+) quantity=(-?\d+) spawns=(\d+) rumble=(\d+) "
    r"boost=(-?\d+) shield=(-?\d+) shieldType=(-?\d+) observed=(\d+)$",
    re.MULTILINE,
)
# The browser route reaches a live race through the real title/menu flow. Tick
# 150 can still be inside the authored starting countdown on Ancient Lake, where
# item release is intentionally suppressed; tick 300 is deep enough to prove a
# real gameplay mutation without depending on a track-specific countdown edge.
ROLLBACK_TARGET_TICK = 300
ROLLBACK_DEPTH = 4
WGPU_PERF_RE = re.compile(
    r"\[WGPU-PERF\]\s+asyncCreates=(\d+)\s+asyncReady=(\d+)\s+"
    r"asyncFailed=(\d+)\s+holdFrames=(\d+)\s+maxHoldStreak=(\d+)\s+"
    r"maxPipelineFrames=(\d+)\s+maxPending=(\d+)"
)
WGPU_BACKPRESSURE_RE = re.compile(r"\[WGPU-BACKPRESSURE\]\s+(.*)")
PRESENT_SCHED_SUMMARY_RE = re.compile(r"\[PRESENTSCHED-SUMMARY\]\s+(.*)")
WORLD_SHADOW_RE = re.compile(
    r"\[WORLD-SHADOW\]\s+backend=webgpu\s+attempted=(\d+)\s+"
    r"complete=(\d+)\s+fallback=(\d+)\s+resourceFailures=(\d+)\s+"
    r"latched=(\d+)"
)
# The two optional groups are NON-capturing on purpose: the summary line grows
# as the runtime gains per-family and comfort telemetry, and every index below
# (row[0] gate, row[1:3] authority, row[3] corrected, row[4:7] resolved,
# row[7] target_hidden) is positional. Matching the new fields without
# capturing them keeps this gate readable against both the current line and an
# older recorded one.
CAMERA_OBSTRUCTION_RE = re.compile(
    r"camera_obstruction_observe summary .* gate=(\w+)\(logical_camera_unchanged\) "
    r"(?:comfort=\w+ )?"
    r"duplicates=(\d+) projection_mismatches=(\d+) "
    r"resolved=\{corrected=(\d+) penetrated=(\d+) invalid=(\d+) degraded=(\d+)\} "
    # probe_degraded landed between resolved={} and target_hidden with the
    # scoped degradation-veto work; optional so the regex still reads the
    # summary of an engine that predates it.
    r"(?:probe_degraded=\d+ )?"
    r"target_hidden=(\d+) target_embedded=(\d+) depenetrate_only=(\d+) "
    r"(?:safety_only=\d+ )?"
    r"emergency=(\d+)"
)
CAMERA_DYNAMIC_RE = re.compile(
    r"camera_obstruction_observe summary .* dynamic=\{published=(\d+) peak=(\d+) "
    r"observed_doors=(\d+) observed_solids=(\d+) missing_cache=(\d+) "
    r"missing_identity=(\d+) excluded_non_solid=(\d+) uncategorized=(\d+) "
    r"invalid_transform=(\d+) capacity_failures=(\d+) transitioning_doors=(\d+) "
    r"active_transitioning_doors=(\d+).*? bytes=(\d+)\}"
)
GFX_SHUTDOWN_RE = re.compile(
    r"\[GFX-SHUTDOWN\]\s+texturesCreated=(\d+)\s+texturesDeleted=(\d+)\s+"
    r"live=(\d+)->(\d+)\s+shaders=(\d+)\s+backendReleased=(\d+)\s+"
    r"cpuScratch=(\d+)"
)
# The renderer's own account of frames it retired at teardown without a
# completion callback; the only thing that may raise [WGPU-BACKPRESSURE]
# abandoned above zero.
WGPU_RETIRE_RE = re.compile(
    r"\[webgpu\] shutdown retiring (\d+) submitted frame\(s\)"
)
DEVTOOLS_RE = re.compile(r"DevTools listening on (ws://127\.0\.0\.1:(\d+)/\S+)")
FATAL_MARKERS = (
    "[CRASH]",
    "[FATAL]",
    "memory access out of bounds",
    "RuntimeError:",
    "Aborted(",
    "device lost",
    "GPU process exited unexpectedly",
    "[EVTQ] post DROPPED",
)


class CheckFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailure(message)


def parse_single_integer_summary(
    lines: list[str], pattern: re.Pattern[str], name: str
) -> dict[str, int]:
    rows = [match.group(1) for line in lines if (match := pattern.search(line))]
    require(len(rows) == 1, f"browser emitted {len(rows)} {name} rows")
    fields: dict[str, int] = {}
    for token in rows[0].split():
        key, separator, value = token.partition("=")
        if separator:
            fields[key] = int(value)
    return fields


def resolve_path(value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = ROOT / path
    return path.resolve()


def find_chrome(explicit: str | None) -> Path:
    candidates: list[str] = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("CHROME")
    if env:
        candidates.append(env)
    candidates.extend(
        [
            "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
            "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
            "/Applications/Chromium.app/Contents/MacOS/Chromium",
            "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
            "google-chrome",
            "google-chrome-stable",
            "chromium",
            "chromium-browser",
            "msedge",
        ]
    )
    for candidate in candidates:
        expanded = os.path.expanduser(candidate)
        if os.path.sep in expanded:
            path = Path(expanded)
            if path.is_file() and os.access(path, os.X_OK):
                return path.resolve()
        else:
            found = shutil.which(expanded)
            if found:
                return Path(found).resolve()
    raise CheckFailure(
        "no Chromium browser found; pass --chrome or set CHROME "
        "(Google Chrome/Chromium/Edge)"
    )


@dataclass(frozen=True)
class HttpRequest:
    method: str
    path: str
    content_length: int


class OverlayServer:
    """Serve shell assets from dist/web and engine files from build-web."""

    def __init__(self, shell_dir: Path, engine_dir: Path):
        self.shell_dir = shell_dir
        self.engine_dir = engine_dir
        self.requests: list[HttpRequest] = []
        self._lock = threading.Lock()
        outer = self

        class Handler(SimpleHTTPRequestHandler):
            server_version = "mdkr64-browser-check"

            def log_message(self, _format: str, *_args: Any) -> None:
                return

            def end_headers(self) -> None:
                self.send_header("Cache-Control", "no-store")
                self.send_header("Cross-Origin-Opener-Policy", "same-origin")
                self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
                super().end_headers()

            def _record(self) -> None:
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                except ValueError:
                    length = -1
                with outer._lock:
                    outer.requests.append(
                        HttpRequest(self.command, self.path, length)
                    )

            def do_GET(self) -> None:  # noqa: N802 - stdlib callback name
                self._record()
                super().do_GET()

            def do_HEAD(self) -> None:  # noqa: N802 - stdlib callback name
                self._record()
                super().do_HEAD()

            def translate_path(self, path: str) -> str:
                clean = urllib.parse.unquote(
                    urllib.parse.urlsplit(path).path
                ).lstrip("/")
                if not clean:
                    clean = "index.html"
                leaf = Path(clean).name
                if clean == leaf and leaf in {
                    "mdkr64_web.js",
                    "mdkr64_web.wasm",
                    "mdkr64_web.js.symbols",
                    "mdkr-save-tools.js",
                    "mdkr-save-tools.wasm",
                }:
                    target = outer.engine_dir / leaf
                else:
                    target = outer.shell_dir / clean
                try:
                    target = target.resolve()
                    root = (
                        outer.engine_dir.resolve()
                        if clean == leaf
                        and (
                            leaf.startswith("mdkr64_web")
                            or leaf.startswith("mdkr-save-tools")
                        )
                        else outer.shell_dir.resolve()
                    )
                    target.relative_to(root)
                except (OSError, ValueError):
                    return str(outer.shell_dir / "__rejected__")
                return str(target)

            def guess_type(self, path: str) -> str:
                if path.endswith(".wasm"):
                    return "application/wasm"
                return super().guess_type(path)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(
            target=self.httpd.serve_forever,
            name="mdkr64-http",
            daemon=True,
        )

    @property
    def origin(self) -> str:
        host, port = self.httpd.server_address[:2]
        return f"http://{host}:{port}"

    def start(self) -> None:
        self.thread.start()

    def close(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=3)


class ChromeProcess:
    def __init__(
        self,
        binary: Path,
        profile: Path,
        extra_flags: list[str],
        verbose: bool,
    ):
        self.verbose = verbose
        self.lines: deque[str] = deque(maxlen=4000)
        self.port: int | None = None
        self._ready = threading.Event()
        command = [
            str(binary),
            "--headless=new",
            "--enable-unsafe-webgpu",
            "--remote-debugging-port=0",
            "--remote-allow-origins=*",
            f"--user-data-dir={profile}",
            "--window-size=960,540",
            "--mute-audio",
            "--autoplay-policy=no-user-gesture-required",
            "--disable-background-networking",
            "--disable-background-timer-throttling",
            "--disable-backgrounding-occluded-windows",
            "--disable-component-update",
            "--disable-default-apps",
            "--disable-features=Translate",
            "--disable-renderer-backgrounding",
            "--disable-sync",
            "--metrics-recording-only",
            "--no-default-browser-check",
            "--no-first-run",
            *extra_flags,
            "about:blank",
        ]
        if verbose:
            print("$ " + " ".join(command), flush=True)
        self.proc = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.threads = [
            threading.Thread(
                target=self._read_stream,
                args=(self.proc.stdout, "chrome-out"),
                daemon=True,
            ),
            threading.Thread(
                target=self._read_stream,
                args=(self.proc.stderr, "chrome-err"),
                daemon=True,
            ),
        ]
        for thread in self.threads:
            thread.start()

    def _read_stream(self, stream: Any, label: str) -> None:
        if stream is None:
            return
        for raw in iter(stream.readline, ""):
            line = raw.rstrip()
            self.lines.append(f"[{label}] {line}")
            match = DEVTOOLS_RE.search(line)
            if match:
                self.port = int(match.group(2))
                self._ready.set()
            if self.verbose and (
                "DevTools listening" in line
                or "ERROR:" in line
                or "GPU process" in line
            ):
                print(f"[{label}] {line}", flush=True)

    def wait_port(self, timeout: float = 20.0) -> int:
        if not self._ready.wait(timeout):
            code = self.proc.poll()
            tail = "\n".join(list(self.lines)[-30:])
            raise CheckFailure(
                f"Chrome did not expose a DevTools endpoint "
                f"(exit={code})\n{tail}"
            )
        assert self.port is not None
        return self.port

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        for thread in self.threads:
            thread.join(timeout=1)


def recv_exact(sock: socket.socket, size: int, initial: bytearray) -> bytes:
    while len(initial) < size:
        chunk = sock.recv(max(4096, size - len(initial)))
        if not chunk:
            raise EOFError("WebSocket closed")
        initial.extend(chunk)
    out = bytes(initial[:size])
    del initial[:size]
    return out


class CDPClient:
    """Minimal RFC6455 client sufficient for the local DevTools protocol."""

    def __init__(self, url: str):
        parsed = urllib.parse.urlsplit(url)
        require(parsed.scheme == "ws", f"unsupported DevTools URL: {url}")
        require(parsed.hostname is not None, f"DevTools URL has no host: {url}")
        self.sock = socket.create_connection(
            (parsed.hostname, parsed.port or 80), timeout=10
        )
        self.sock.settimeout(None)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        path = parsed.path or "/"
        if parsed.query:
            path += "?" + parsed.query
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {parsed.hostname}:{parsed.port or 80}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode("ascii"))
        response = bytearray()
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise CheckFailure("DevTools WebSocket handshake closed")
            response.extend(chunk)
            require(
                len(response) <= 65536,
                "oversized DevTools WebSocket handshake",
            )
        header, leftover = bytes(response).split(b"\r\n\r\n", 1)
        require(
            header.startswith(b"HTTP/1.1 101"),
            "DevTools WebSocket rejected handshake:\n"
            + header.decode("latin1", "replace"),
        )
        accept = base64.b64encode(
            hashlib.sha1(
                (
                    key
                    + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
                ).encode("ascii")
            ).digest()
        ).decode("ascii")
        require(
            f"Sec-WebSocket-Accept: {accept}".lower()
            in header.decode("latin1").lower(),
            "DevTools WebSocket returned the wrong accept key",
        )
        self.buffer = bytearray(leftover)
        self.pending: dict[int, queue.Queue[dict[str, Any]]] = {}
        self.pending_lock = threading.Lock()
        self.send_lock = threading.Lock()
        self.next_id = 1
        self.closed = False
        self.events: deque[dict[str, Any]] = deque(maxlen=30000)
        self.console: list[str] = []
        self.network: list[dict[str, Any]] = []
        self.failures: list[str] = []
        self.exceptions: list[str] = []
        # Page.reload returns before the old execution context is necessarily
        # destroyed. The browser gate installs a fresh token with every test
        # configuration and wait_launcher requires that token before it can
        # click controls in the replacement document.
        self.expected_document_token: str | None = None
        self.reader = threading.Thread(
            target=self._reader, name="mdkr64-cdp", daemon=True
        )
        self.reader.start()

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        mask = os.urandom(4)
        length = len(payload)
        header = bytearray([0x80 | opcode])
        if length < 126:
            header.append(0x80 | length)
        elif length <= 0xFFFF:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", length))
        header.extend(mask)
        masked = bytes(value ^ mask[index & 3] for index, value in enumerate(payload))
        with self.send_lock:
            self.sock.sendall(header + masked)

    def _recv_frame(self) -> tuple[bool, int, bytes]:
        first, second = recv_exact(self.sock, 2, self.buffer)
        final = bool(first & 0x80)
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", recv_exact(self.sock, 2, self.buffer))[0]
        elif length == 127:
            length = struct.unpack("!Q", recv_exact(self.sock, 8, self.buffer))[0]
        require(length <= 128 * 1024 * 1024, "oversized DevTools frame")
        mask = recv_exact(self.sock, 4, self.buffer) if masked else b""
        payload = recv_exact(self.sock, length, self.buffer)
        if masked:
            payload = bytes(
                value ^ mask[index & 3] for index, value in enumerate(payload)
            )
        return final, opcode, payload

    def _reader(self) -> None:
        fragments = bytearray()
        fragment_opcode = 0
        try:
            while not self.closed:
                final, opcode, payload = self._recv_frame()
                if opcode == 0x8:
                    break
                if opcode == 0x9:
                    self._send_frame(0xA, payload)
                    continue
                if opcode == 0xA:
                    continue
                if opcode in (0x1, 0x2):
                    fragments = bytearray(payload)
                    fragment_opcode = opcode
                elif opcode == 0x0 and fragment_opcode:
                    fragments.extend(payload)
                else:
                    continue
                if not final:
                    continue
                if fragment_opcode == 0x1:
                    self._dispatch(
                        json.loads(fragments.decode("utf-8", "replace"))
                    )
                fragments = bytearray()
                fragment_opcode = 0
        except Exception as exc:  # surfaced to any pending command
            if not self.closed:
                self.failures.append(f"CDP reader stopped: {exc}")
        finally:
            self.closed = True
            with self.pending_lock:
                waiters = list(self.pending.values())
            for waiter in waiters:
                waiter.put({"error": {"message": "DevTools connection closed"}})

    @staticmethod
    def _remote_value(value: dict[str, Any]) -> str:
        if "value" in value:
            item = value["value"]
            if isinstance(item, (dict, list)):
                return json.dumps(item, sort_keys=True)
            return str(item)
        return str(value.get("description", value.get("unserializableValue", "")))

    def _dispatch(self, message: dict[str, Any]) -> None:
        identifier = message.get("id")
        if isinstance(identifier, int):
            with self.pending_lock:
                waiter = self.pending.get(identifier)
            if waiter:
                waiter.put(message)
            return
        method = message.get("method", "")
        params = message.get("params", {})
        self.events.append(message)
        if method == "Runtime.consoleAPICalled":
            text = " ".join(
                self._remote_value(arg) for arg in params.get("args", [])
            )
            if text:
                self.console.append(text)
        elif method == "Log.entryAdded":
            text = str(params.get("entry", {}).get("text", ""))
            if text:
                self.console.append(text)
        elif method == "Runtime.exceptionThrown":
            details = params.get("exceptionDetails", {})
            description = self._remote_value(details.get("exception", {}))
            self.exceptions.append(
                description or str(details.get("text", "JavaScript exception"))
            )
        elif method == "Network.requestWillBeSent":
            self.network.append(dict(params.get("request", {})))
        elif method == "Network.loadingFailed":
            request_id = str(params.get("requestId", "?"))
            error = str(params.get("errorText", "loading failed"))
            self.failures.append(f"request {request_id}: {error}")
        elif method in {
            "Inspector.targetCrashed",
            "Target.targetCrashed",
        }:
            self.failures.append(method)

    def call(
        self,
        method: str,
        params: dict[str, Any] | None = None,
        timeout: float = 20.0,
    ) -> dict[str, Any]:
        with self.pending_lock:
            identifier = self.next_id
            self.next_id += 1
            waiter: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=1)
            self.pending[identifier] = waiter
        try:
            self._send_frame(
                0x1,
                json.dumps(
                    {"id": identifier, "method": method, "params": params or {}},
                    separators=(",", ":"),
                ).encode("utf-8"),
            )
            try:
                response = waiter.get(timeout=timeout)
            except queue.Empty as exc:
                raise CheckFailure(f"CDP {method} timed out") from exc
        finally:
            with self.pending_lock:
                self.pending.pop(identifier, None)
        if "error" in response:
            error = response["error"]
            raise CheckFailure(
                f"CDP {method} failed: {error.get('message', error)}"
            )
        return dict(response.get("result", {}))

    def evaluate(
        self,
        expression: str,
        *,
        await_promise: bool = False,
        timeout: float = 20.0,
    ) -> Any:
        result = self.call(
            "Runtime.evaluate",
            {
                "expression": expression,
                "awaitPromise": await_promise,
                "returnByValue": True,
                "userGesture": True,
            },
            timeout=timeout,
        )
        if "exceptionDetails" in result:
            details = result["exceptionDetails"]
            raise CheckFailure(
                "browser evaluation failed: "
                + str(
                    details.get("exception", {}).get(
                        "description", details.get("text", details)
                    )
                )
            )
        remote = result.get("result", {})
        if "value" in remote:
            return remote["value"]
        if remote.get("subtype") == "null":
            return None
        return remote.get("unserializableValue")

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        try:
            self._send_frame(0x8, b"")
        except Exception:
            pass
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()
        self.reader.join(timeout=2)


def page_websocket(port: int, timeout: float = 15.0) -> str:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/json/list", timeout=2
            ) as response:
                targets = json.load(response)
            for target in targets:
                if target.get("type") == "page" and target.get(
                    "webSocketDebuggerUrl"
                ):
                    return str(target["webSocketDebuggerUrl"])
        except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
            last_error = exc
        time.sleep(0.1)
    raise CheckFailure(f"no page target from Chrome DevTools: {last_error}")


def add_config_script(cdp: CDPClient, config: dict[str, Any]) -> str:
    document_token = os.urandom(16).hex()
    config = dict(config)
    config["documentToken"] = document_token
    source = (
        "globalThis.__mdkrTestConfig = "
        + json.dumps(config, separators=(",", ":"))
        + ";"
    )
    result = cdp.call(
        "Page.addScriptToEvaluateOnNewDocument", {"source": source}
    )
    identifier = result.get("identifier")
    require(bool(identifier), "CDP did not return a preload-script identifier")
    cdp.expected_document_token = document_token
    return str(identifier)


def wait_value(
    cdp: CDPClient,
    expression: str,
    predicate: Callable[[Any], bool],
    description: str,
    timeout: float,
) -> Any:
    deadline = time.monotonic() + timeout
    last: Any = None
    while time.monotonic() < deadline:
        if cdp.closed:
            raise CheckFailure(
                f"DevTools target closed while waiting for {description}"
            )
        try:
            last = cdp.evaluate(expression, timeout=min(10.0, timeout))
            if predicate(last):
                return last
        except CheckFailure:
            # Navigation briefly destroys the old execution context.
            pass
        time.sleep(0.08)
    raise CheckFailure(f"timed out waiting for {description}; last value={last!r}")


def wait_launcher(cdp: CDPClient, timeout: float) -> dict[str, Any]:
    expected_document_token = cdp.expected_document_token
    require(
        bool(expected_document_token),
        "launcher wait has no configured document token",
    )
    expression = """(() => {
      const ui = document.getElementById("rom-ui");
      const play = document.getElementById("play");
      const msg = document.getElementById("gate-msg");
      const status = document.getElementById("rom-status");
      return {
        documentToken: globalThis.__mdkrTestConfig &&
          globalThis.__mdkrTestConfig.documentToken,
        ready: document.readyState === "complete",
        ui: !!ui && !ui.hidden,
        playDisabled: !play || play.disabled,
        blocked: !!(play && play.dataset.blocked),
        message: msg ? msg.textContent : "",
        romStatus: status ? status.textContent : ""
      };
    })()"""
    value = wait_value(
        cdp,
        expression,
        lambda item: launcher_is_ready(item, expected_document_token),
        "the WebGPU launcher gate",
        timeout,
    )
    require(not value.get("blocked"), f"WebGPU gate rejected Chrome: {value}")
    return value


def select_rom(cdp: CDPClient, rom: Path, timeout: float) -> None:
    document = cdp.call("DOM.getDocument", {"depth": -1, "pierce": True})
    root_id = document.get("root", {}).get("nodeId")
    require(isinstance(root_id, int), "CDP returned no DOM root")
    query = cdp.call(
        "DOM.querySelector", {"nodeId": root_id, "selector": "#rom-input"}
    )
    node_id = query.get("nodeId")
    require(isinstance(node_id, int) and node_id != 0, "ROM input is absent")
    cdp.call(
        "DOM.setFileInputFiles",
        {"nodeId": node_id, "files": [str(rom)]},
        timeout=30,
    )
    cdp.evaluate(
        'document.getElementById("rom-input").dispatchEvent('
        'new Event("change", {bubbles:true}))'
    )
    status = wait_value(
        cdp,
        """(() => {
          const play = document.getElementById("play");
          const status = document.getElementById("rom-status");
          return {disabled: play.disabled, className: status.className,
                  text: status.textContent};
        })()""",
        lambda item: isinstance(item, dict)
        and not item.get("disabled")
        and item.get("className") == "ok",
        "ROM validation",
        timeout,
    )
    require("looks good" in status.get("text", ""), f"unexpected ROM verdict: {status}")


def select_file_input(cdp: CDPClient, selector: str, path: Path) -> None:
    document = cdp.call("DOM.getDocument", {"depth": -1, "pierce": True})
    root_id = document.get("root", {}).get("nodeId")
    require(isinstance(root_id, int), "CDP returned no DOM root")
    query = cdp.call(
        "DOM.querySelector", {"nodeId": root_id, "selector": selector}
    )
    node_id = query.get("nodeId")
    require(
        isinstance(node_id, int) and node_id != 0,
        f"file input is absent: {selector}",
    )
    cdp.call(
        "DOM.setFileInputFiles",
        {"nodeId": node_id, "files": [str(path)]},
        timeout=30,
    )
    cdp.evaluate(
        f'document.querySelector({json.dumps(selector)}).dispatchEvent('
        'new Event("change", {bubbles:true}))'
    )


def wait_download(
    directory: Path, suffix: str, description: str, timeout: float
) -> Path:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        candidates = [
            path
            for path in directory.glob(f"*{suffix}")
            if not path.name.endswith(".crdownload") and path.stat().st_size > 0
        ]
        if candidates:
            return max(candidates, key=lambda path: path.stat().st_mtime_ns)
        time.sleep(0.08)
    raise CheckFailure(f"timed out waiting for {description} download")


def click_play(cdp: CDPClient) -> None:
    cdp.evaluate('document.getElementById("play").click()')


def snapshot(cdp: CDPClient) -> dict[str, Any]:
    value = cdp.evaluate(
        "globalThis.__mdkrTestSnapshot ? globalThis.__mdkrTestSnapshot() : null"
    )
    require(isinstance(value, dict), "browser test snapshot is unavailable")
    return value


def persist_snapshot(cdp: CDPClient, timeout: float) -> dict[str, Any]:
    expression = """new Promise((resolve) => {
      persist(async (err) => resolve({
        error: err ? String(err) : null,
        snapshot: globalThis.__mdkrTestSnapshot(),
        romCount: await idbCount("/rom"),
        saveCount: await idbCount("/save")
      }));
    })"""
    value = cdp.evaluate(
        expression, await_promise=True, timeout=timeout
    )
    require(isinstance(value, dict), "persist callback returned no result")
    return value


def capture_png(cdp: CDPClient) -> bytes:
    result = cdp.call(
        "Page.captureScreenshot",
        {
            "format": "png",
            "fromSurface": True,
            "captureBeyondViewport": False,
        },
        timeout=30,
    )
    data = result.get("data")
    require(isinstance(data, str) and data, "Chrome returned an empty screenshot")
    return base64.b64decode(data)


def resize_canvas(
    cdp: CDPClient,
    css_width: int,
    css_height: int,
    dpr: float,
    backing_width: int,
    backing_height: int,
    timeout: float,
) -> dict[str, Any]:
    cdp.call(
        "Emulation.setDeviceMetricsOverride",
        {
            "width": css_width,
            "height": css_height,
            "deviceScaleFactor": dpr,
            "mobile": False,
        },
    )
    cdp.evaluate('window.dispatchEvent(new Event("resize"))')
    value = wait_value(
        cdp,
        """(() => {
          const canvas = document.getElementById("canvas");
          return {
            width: canvas.width, height: canvas.height,
            cssWidth: canvas.style.width, cssHeight: canvas.style.height,
            innerWidth: window.innerWidth, innerHeight: window.innerHeight
          };
        })()""",
        lambda item: isinstance(item, dict)
        and item.get("width") == backing_width
        and item.get("height") == backing_height
        and item.get("cssWidth") == f"{css_width}px"
        and item.get("cssHeight") == f"{css_height}px",
        f"{css_width}x{css_height} CSS / {backing_width}x{backing_height} backing resize",
        timeout,
    )
    # Let the engine observe the backing-store dimensions and let WebGPU's
    # deliberate resize debounce commit the new targets.
    start = snapshot(cdp).get("frames", 0)
    wait_value(
        cdp,
        "globalThis.__mdkrTestSnapshot().frames",
        lambda frame: isinstance(frame, (int, float)) and frame >= start + 6,
        "six post-resize presented frames",
        timeout,
    )
    return value


def exercise_audio_overflow(cdp: CDPClient, timeout: float) -> dict[str, Any]:
    """Drive the real AudioWorklet ring past capacity through its message port.

    This is deliberately a test-config-only shell seam. It proves that the
    production worklet reports a bounded oldest-sample discard and begins a
    continuity recovery, rather than silently accepting an unbounded backlog.
    """
    injected = cdp.evaluate(
        "globalThis.__mdkrTestForceAudioOverflow && "
        "globalThis.__mdkrTestForceAudioOverflow()"
    )
    require(
        isinstance(injected, dict)
        and injected.get("frames", 0) > injected.get("capacity", 0) > 0,
        f"AudioWorklet overflow seam was unavailable: {injected}",
    )
    minimum_drops = injected["frames"] - injected["capacity"]
    return wait_value(
        cdp,
        "globalThis.__mdkrTestSnapshot().audio",
        lambda audio: isinstance(audio, dict)
        and audio.get("ringDroppedFrames", 0) >= minimum_drops
        and audio.get("recoveries", 0) > 0
        and audio.get("recoverySamples", 0) >= 128
        and audio.get("completedRecoveries", 0) > 0
        # Every armed recovery must also FINISH, and no ramp may be left
        # part-way through. A crossfade that stops advancing is the observable
        # signature of an underrun taken inside the envelope: the worklet used
        # to skip its fade decrement on a starved sample, which froze the mix at
        # a partial blend, stalled completedRecoveries, and blocked the
        # fade===0 re-arm for every later discard.
        and audio.get("completedRecoveries", 0) >= audio.get("recoveries", 0)
        and audio.get("recoveryFrames", -1) == 0,
        "AudioWorklet bounded overflow recovery",
        timeout,
    )


def exercise_fullscreen(
    cdp: CDPClient,
    timeout: float,
) -> dict[str, Any]:
    cdp.evaluate(
        'document.getElementById("fullscreen").click(); true'
    )
    entered = wait_value(
        cdp,
        """(() => {
          const stage = document.getElementById("stage");
          const canvas = document.getElementById("canvas");
          const button = document.getElementById("fullscreen");
          return {
            active: document.fullscreenElement === stage,
            width: canvas.width,
            height: canvas.height,
            pressed: button.getAttribute("aria-pressed"),
            disabled: button.disabled
          };
        })()""",
        lambda item: isinstance(item, dict)
        and item.get("active") is True
        and item.get("pressed") == "true"
        and item.get("disabled") is False,
        "fullscreen entry",
        timeout,
    )
    fullscreen_start = snapshot(cdp).get("frames", 0)
    require(
        isinstance(fullscreen_start, (int, float)),
        "fullscreen test has no frame count",
    )
    wait_value(
        cdp,
        "globalThis.__mdkrTestSnapshot().frames",
        lambda frame: isinstance(frame, (int, float))
        and frame >= fullscreen_start + 12,
        "twelve live fullscreen frames",
        timeout,
    )

    exit_start = snapshot(cdp).get("frames", 0)
    cdp.evaluate(
        'document.getElementById("fullscreen").click(); true'
    )
    wait_value(
        cdp,
        """(() => ({
          active: document.fullscreenElement !== null,
          pressed: document.getElementById("fullscreen").getAttribute(
            "aria-pressed"),
          disabled: document.getElementById("fullscreen").disabled
        }))()""",
        lambda item: isinstance(item, dict)
        and item.get("active") is False
        and item.get("pressed") == "false"
        and item.get("disabled") is False,
        "fullscreen exit",
        timeout,
    )
    wait_value(
        cdp,
        "globalThis.__mdkrTestSnapshot().frames",
        lambda frame: isinstance(frame, (int, float))
        and frame >= exit_start + 12,
        "twelve live post-fullscreen frames",
        timeout,
    )
    return entered


def exercise_fullscreen_rejection(
    cdp: CDPClient,
    timeout: float,
) -> dict[str, Any]:
    armed = cdp.evaluate(
        """(() => {
          const stage = document.getElementById("stage");
          if (!stage || typeof stage.requestFullscreen !== "function") {
            return false;
          }
          globalThis.__mdkrOriginalRequestFullscreen =
            stage.requestFullscreen.bind(stage);
          stage.requestFullscreen = () =>
            Promise.reject(new DOMException("injected denial", "NotAllowedError"));
          document.getElementById("fullscreen").click();
          return true;
        })()"""
    )
    require(armed is True, "fullscreen rejection seam was unavailable")
    result = wait_value(
        cdp,
        """(() => {
          const status = document.getElementById("stage-status");
          const button = document.getElementById("fullscreen");
          return {
            hidden: status.hidden,
            text: status.textContent,
            active: document.fullscreenElement !== null,
            pressed: button.getAttribute("aria-pressed"),
            disabled: button.disabled
          };
        })()""",
        lambda item: isinstance(item, dict)
        and item.get("hidden") is False
        and item.get("active") is False
        and item.get("pressed") == "false"
        and item.get("disabled") is False
        and "blocked by this browser" in item.get("text", ""),
        "visible fullscreen rejection",
        timeout,
    )
    cdp.evaluate(
        """(() => {
          const stage = document.getElementById("stage");
          stage.requestFullscreen = globalThis.__mdkrOriginalRequestFullscreen;
          delete globalThis.__mdkrOriginalRequestFullscreen;
          return true;
        })()"""
    )
    return result


def exercise_fullscreen_exit_rejection(
    cdp: CDPClient,
    timeout: float,
) -> dict[str, Any]:
    cdp.evaluate('document.getElementById("fullscreen").click(); true')
    wait_value(
        cdp,
        "document.fullscreenElement === document.getElementById('stage')",
        lambda value: value is True,
        "fullscreen entry before rejected exit",
        timeout,
    )
    armed = cdp.evaluate(
        """(() => {
          if (typeof document.exitFullscreen !== "function") return false;
          globalThis.__mdkrOriginalExitFullscreen =
            document.exitFullscreen.bind(document);
          document.exitFullscreen = () =>
            Promise.reject(new DOMException("injected exit denial", "NotAllowedError"));
          document.getElementById("fullscreen").click();
          return true;
        })()"""
    )
    require(armed is True, "fullscreen exit-rejection seam was unavailable")
    result = wait_value(
        cdp,
        """(() => {
          const status = document.getElementById("stage-status");
          const stage = document.getElementById("stage");
          const button = document.getElementById("fullscreen");
          return {
            hidden: status.hidden,
            text: status.textContent,
            active: document.fullscreenElement === stage,
            pressed: button.getAttribute("aria-pressed"),
            disabled: button.disabled,
          };
        })()""",
        lambda item: isinstance(item, dict)
        and item.get("hidden") is False
        and item.get("active") is True
        and item.get("pressed") == "true"
        and item.get("disabled") is False
        and "couldn't exit fullscreen" in item.get("text", "").lower(),
        "visible fullscreen exit rejection",
        timeout,
    )
    cdp.evaluate(
        """(() => {
          document.exitFullscreen = globalThis.__mdkrOriginalExitFullscreen;
          delete globalThis.__mdkrOriginalExitFullscreen;
          document.getElementById("fullscreen").click();
          return true;
        })()"""
    )
    wait_value(
        cdp,
        "document.fullscreenElement === null",
        lambda value: value is True,
        "fullscreen exit after restoring API",
        timeout,
    )
    return result


def assert_narrow_touch_status_layout(cdp: CDPClient) -> None:
    cdp.call(
        "Emulation.setDeviceMetricsOverride",
        {"width": 320, "height": 640, "deviceScaleFactor": 1, "mobile": True},
    )
    try:
        layout = cdp.evaluate(
            """(() => {
              const stage = document.getElementById("stage");
              const status = document.getElementById("stage-status");
              const toggle = document.getElementById("touch-toggle");
              const fullscreen = document.getElementById("fullscreen");
              const hadTouchClass = stage.classList.contains("touch-ui-active");
              const wasToggleHidden = toggle.hidden;
              stage.classList.add("touch-ui-active");
              toggle.hidden = false;
              const sr = status.getBoundingClientRect();
              const tr = toggle.getBoundingClientRect();
              const fr = fullscreen.getBoundingClientRect();
              stage.classList.toggle("touch-ui-active", hadTouchClass);
              toggle.hidden = wasToggleHidden;
              return {
                statusBelowChrome: sr.top >= Math.max(tr.bottom, fr.bottom),
                statusInViewport: sr.left >= 0 && sr.right <= innerWidth &&
                  sr.top >= 0 && sr.bottom <= innerHeight,
              };
            })()"""
        )
        require(
            isinstance(layout, dict)
            and layout.get("statusBelowChrome") is True
            and layout.get("statusInViewport") is True,
            f"narrow touch fullscreen status overlaps controls: {layout}",
        )
    finally:
        cdp.call("Emulation.clearDeviceMetricsOverride")


def png_pixels(data: bytes) -> tuple[int, int, int, bytes]:
    require(data.startswith(b"\x89PNG\r\n\x1a\n"), "screenshot is not PNG")
    position = 8
    width = height = color_type = bit_depth = 0
    compressed = bytearray()
    while position + 12 <= len(data):
        length = struct.unpack(">I", data[position : position + 4])[0]
        kind = data[position + 4 : position + 8]
        payload = data[position + 8 : position + 8 + length]
        position += 12 + length
        require(position <= len(data), "truncated screenshot PNG")
        if kind == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(
                ">IIBB", payload[:10]
            )
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
    require(width > 0 and height > 0, "screenshot PNG has no dimensions")
    require(bit_depth == 8, f"unsupported screenshot PNG bit depth {bit_depth}")
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color_type)
    require(channels is not None, f"unsupported screenshot PNG color type {color_type}")
    raw = zlib.decompress(bytes(compressed))
    stride = width * channels
    require(
        len(raw) == height * (stride + 1),
        "screenshot PNG scanline size mismatch",
    )
    rows = bytearray(height * stride)
    previous = bytearray(stride)
    source = 0
    for y in range(height):
        filter_type = raw[source]
        source += 1
        encoded = raw[source : source + stride]
        source += stride
        decoded = bytearray(stride)
        for x, value in enumerate(encoded):
            left = decoded[x - channels] if x >= channels else 0
            up = previous[x]
            up_left = previous[x - channels] if x >= channels else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = up
            elif filter_type == 3:
                predictor = (left + up) // 2
            elif filter_type == 4:
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else up_left
            else:
                raise CheckFailure(f"unsupported screenshot PNG filter {filter_type}")
            decoded[x] = (value + predictor) & 0xFF
        start = y * stride
        rows[start : start + stride] = decoded
        previous = decoded
    return width, height, channels, bytes(rows)


@dataclass(frozen=True)
class SceneStats:
    width: int
    height: int
    distinct: int
    luma_sigma: float
    nonblack_fraction: float
    digest: str


def scene_stats(png: bytes) -> SceneStats:
    width, height, channels, pixels = png_pixels(png)
    step = max(1, int(math.sqrt((width * height) / 60000)))
    colors: set[tuple[int, int, int]] = set()
    lumas: list[float] = []
    nonblack = 0
    sampled = bytearray()
    for y in range(0, height, step):
        row = y * width * channels
        for x in range(0, width, step):
            offset = row + x * channels
            if channels in (1, 2):
                r = g = b = pixels[offset]
            else:
                r, g, b = pixels[offset : offset + 3]
            colors.add((r, g, b))
            luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
            lumas.append(luma)
            if r + g + b > 18:
                nonblack += 1
            sampled.extend((r, g, b))
    sigma = statistics.pstdev(lumas) if len(lumas) > 1 else 0.0
    return SceneStats(
        width,
        height,
        len(colors),
        sigma,
        nonblack / max(1, len(lumas)),
        hashlib.sha256(sampled).hexdigest(),
    )


def scenes_are_live(stats: list[SceneStats]) -> tuple[bool, str]:
    live = [
        item
        for item in stats
        if item.distinct >= 64
        and item.luma_sigma >= 8.0
        and item.nonblack_fraction >= 0.08
    ]
    changing = len({item.digest for item in live})
    ok = len(live) >= max(3, len(stats) - 1) and changing >= 3
    return (
        ok,
        f"{len(live)}/{len(stats)} live, {changing} distinct live rasters",
    )


def validate_positive_controls() -> None:
    # A flat black raster must not satisfy the visual liveness oracle.
    black = SceneStats(960, 540, 1, 0.0, 0.0, "black")
    require(
        not scenes_are_live([black, black, black])[0],
        "visual positive control was not rejected",
    )
    # The network oracle must reject a synthetic ROM upload.
    fake = [{"url": "https://example.invalid/upload", "method": "POST", "hasPostData": True}]
    require(
        network_problems(fake, "http://127.0.0.1:1", "game.z64"),
        "network-leak positive control was not rejected",
    )
    # The persistence equality is exact, not merely a file-exists check.
    require(
        not same_save(
            {"exists": True, "size": 512, "hash": "aaaaaaaa"},
            {"exists": True, "size": 512, "hash": "bbbbbbbb"},
        ),
        "save-mismatch positive control was not rejected",
    )
    # Page.reload may briefly leave the old, fully-ready launcher observable.
    # A wait for the next document must reject that stale document even though
    # all of its visible readiness fields still look valid.
    stale_launcher = {
        "documentToken": "old-document",
        "ready": True,
        "ui": True,
        "blocked": False,
        "message": "",
    }
    require(
        not launcher_is_ready(stale_launcher, "new-document"),
        "stale-document launcher positive control was not rejected",
    )


def launcher_is_ready(item: Any, expected_document_token: str) -> bool:
    return (
        isinstance(item, dict)
        and item.get("documentToken") == expected_document_token
        and item.get("ready")
        and item.get("ui")
        and (item.get("blocked") or not item.get("message"))
    )


def same_save(first: dict[str, Any], second: dict[str, Any]) -> bool:
    return (
        first.get("exists") is True
        and second.get("exists") is True
        and first.get("size") == second.get("size") == 512
        and isinstance(first.get("hash"), str)
        and first.get("hash") == second.get("hash")
    )


def save_generation_is_checksum_safe(raw: Any) -> bool:
    if not isinstance(raw, list) or len(raw) != 512:
        return False
    try:
        image = bytes(raw)
    except (TypeError, ValueError):
        return False

    def block_safe(block: bytes) -> bool:
        if block in (bytes(len(block)), b"\xff" * len(block)):
            return True
        return slot_checksum_valid(block)

    if not all(block_safe(image[index:index + 40])
               for index in range(0, 120, 40)):
        return False
    config = image[120:128]
    if config not in (bytes(8), b"\xff" * 8):
        value = int.from_bytes(config, "big")
        payload = value & 0x00FFFFFFFFFFFFFF
        if (value >> 56) != config_checksum(payload) & 0xFF:
            return False
    return block_safe(image[128:320]) and block_safe(image[320:512])


def network_problems(
    requests: list[dict[str, Any]], origin: str, rom_name: str
) -> list[str]:
    expected = urllib.parse.urlsplit(origin)
    problems: list[str] = []
    for request in requests:
        url = str(request.get("url", ""))
        method = str(request.get("method", "GET")).upper()
        parsed = urllib.parse.urlsplit(url)
        if method not in {"GET", "HEAD"}:
            problems.append(f"{method} {url}")
        if request.get("hasPostData") or request.get("postData"):
            problems.append(f"request body on {method} {url}")
        if rom_name.lower() in urllib.parse.unquote(url).lower():
            problems.append(f"ROM filename entered network log: {url}")
        if parsed.scheme in {"http", "https"} and (
            parsed.hostname != expected.hostname or parsed.port != expected.port
        ):
            problems.append(f"external request: {url}")
    return problems


def parse_pace(lines: list[str]) -> list[tuple[int, int, int, float, int | None, int | None]]:
    rows: list[tuple[int, int, int, float, int | None, int | None]] = []
    for line in lines:
        match = PACE_RE.search(line)
        if not match:
            continue
        rows.append(
            (
                int(match.group(1)),
                int(match.group(2)),
                int(match.group(3)),
                float(match.group(5)),
                int(match.group(10)) if match.group(10) is not None else None,
                int(match.group(11)) if match.group(11) is not None else None,
            )
        )
    return rows


def assert_browser_rollback(console_text: str) -> dict[str, int]:
    """Require one non-vacuous delayed correction in the shipped wasm runtime."""
    ready = [tuple(map(int, row)) for row in ROLLBACK_READY_RE.findall(
        console_text
    )]
    require(len(ready) == 1, f"browser rollback ready rows: {ready!r}")
    ranges, snapshot_bytes, ring_bytes, target = ready[0]
    require(
        ranges >= 140
        and snapshot_bytes > 0
        and ring_bytes == snapshot_bytes * 32
        and ring_bytes <= 16 * 1024 * 1024
        and target == ROLLBACK_TARGET_TICK,
        f"browser rollback authority shape is invalid: {ready[0]!r}",
    )

    stats = [tuple(map(int, row)) for row in ROLLBACK_STATS_RE.findall(
        console_text
    )]
    require(len(stats) == 1, f"browser rollback stats rows: {stats!r}")
    stats_row = stats[0]
    authored_ticks = stats_row[0]
    require(
        authored_ticks >= ROLLBACK_TARGET_TICK
        and stats_row[1] == authored_ticks + 9
        and stats_row[2] == 3
        and stats_row[4] <= stats_row[5] <= stats_row[6]
        and stats_row[9] <= stats_row[10] <= stats_row[11]
        and not any(stats_row[13:19]),
        f"browser rollback capture/restore budget failed: {stats_row!r}",
    )

    resim = [tuple(map(int, row)) for row in ROLLBACK_RESIM_RE.findall(
        console_text
    )]
    require(len(resim) == 1, f"browser rollback resim rows: {resim!r}")
    resim_row = resim[0]
    require(
        resim_row[0] == ROLLBACK_DEPTH * 2
        and resim_row[1] > 0
        and resim_row[2] <= resim_row[3] <= resim_row[4]
        and resim_row[5] > 0
        and not any(resim_row[6:]),
        f"browser rollback resimulation budget failed: {resim_row!r}",
    )

    frame = [tuple(map(int, row)) for row in ROLLBACK_FRAME_RE.findall(
        console_text
    )]
    require(len(frame) == 1, f"browser rollback frame rows: {frame!r}")
    frame_row = frame[0]
    require(
        frame_row[0] == authored_ticks - 1
        and frame_row[1] > 0
        and frame_row[2] <= frame_row[3] <= frame_row[4]
        and frame_row[4] <= 8_333_333
        and frame_row[5] > 0
        and frame_row[6] == 0
        and frame_row[8] == 0,
        f"browser rollback authored-frame budget failed: {frame_row!r}",
    )

    effects = [tuple(map(int, row)) for row in ROLLBACK_EFFECTS_RE.findall(
        console_text
    )]
    require(
        len(effects) == 1
        and effects[0][1] > 0
        and effects[0][5:] == (0, 0),
        f"browser rollback effect journal failed: {effects!r}",
    )
    arms = [tuple(map(int, row)) for row in ROLLBACK_ITEM_ARM_RE.findall(
        console_text
    )]
    results = [tuple(map(int, row)) for row in ROLLBACK_ITEM_RESULT_RE.findall(
        console_text
    )]
    # This browser journey uses MDKR_AUTOPILOT so its long visual oracle reaches
    # a checkpoint. Autopilot deliberately owns the racer instead of the human
    # Z-release path; item breadth belongs to the separate 15-row real-player
    # gate. Accidentally arming that probe here must not be mistaken for wasm
    # parity.
    require(
        not arms and not results,
        f"browser rollback unexpectedly armed a human item probe: "
        f"arms={arms!r} results={results!r}",
    )

    first_tick = ROLLBACK_TARGET_TICK - ROLLBACK_DEPTH + 1
    witnesses = (
        f"[ROLLBACK] delayed-input correction passed ticks={first_tick}.."
        f"{ROLLBACK_TARGET_TICK} depth={ROLLBACK_DEPTH} "
        "non_input_divergence=1 exact_replay=1",
        "[ROLLBACK] first-boundary restore roundtrip passed tick=1",
    )
    require(
        all(console_text.count(marker) == 1 for marker in witnesses),
        "browser rollback is missing an exact correction/roundtrip witness",
    )
    forbidden = (
        "[FATAL]", "[CRASH]", "overflow=1", "overflows=1",
        "forbidden_io=1", "observed=0", "simulation witness mismatch",
        "corrected item breadth was not observed",
    )
    require(
        not any(marker in console_text for marker in forbidden),
        "browser rollback emitted a forbidden diagnostic",
    )
    return {
        "ticks": authored_ticks,
        "ranges": ranges,
        "snapshot": snapshot_bytes,
        "ring": ring_bytes,
        "capture_p99": stats_row[6],
        "restore_p99": stats_row[11],
        "resim_p99": resim_row[4],
        "frame_p99": frame_row[4],
    }


def parse_event_queue_peaks(
    lines: list[str],
) -> dict[int, tuple[str, int, int]]:
    """Return q-index -> (pointer, peak, capacity) from monotonic peak logs."""
    queues: dict[int, tuple[str, int, int]] = {}
    for line in lines:
        match = EVTQ_PEAK_RE.search(line)
        if not match:
            continue
        index = int(match.group(1))
        pointer = match.group(2)
        peak = int(match.group(3))
        capacity = int(match.group(4))
        require(0 < peak <= capacity, f"invalid event-queue peak line: {line}")
        previous = queues.get(index)
        if previous:
            require(
                previous[0] == pointer and previous[2] == capacity,
                f"event queue q{index} changed identity during one run",
            )
            require(
                peak >= previous[1],
                f"event queue q{index} high-water mark moved backwards",
            )
        queues[index] = (pointer, peak, capacity)
    return queues


def run_check(args: argparse.Namespace) -> None:
    rom = resolve_path(args.rom)
    shell_dir = resolve_path(args.shell_dir)
    engine_dir = resolve_path(args.engine_dir)
    script_path = resolve_path(args.input_script)
    chrome_path = find_chrome(args.chrome)
    for label, path in (
        ("ROM", rom),
        ("shell index", shell_dir / "index.html"),
        ("shell JavaScript", shell_dir / "mdkr64-shell.js"),
        ("save UI JavaScript", shell_dir / "mdkr-save-ui.js"),
        ("ROM identity JavaScript", shell_dir / "rom-id.js"),
        ("wasm loader", engine_dir / "mdkr64_web.js"),
        ("wasm module", engine_dir / "mdkr64_web.wasm"),
        ("save-tools loader", engine_dir / "mdkr-save-tools.js"),
        ("save-tools module", engine_dir / "mdkr-save-tools.wasm"),
        ("input fixture", script_path),
    ):
        require(path.is_file(), f"missing {label}: {path}")
    require(rom.stat().st_size == ROM_BYTES, f"ROM is not 12 MiB: {rom}")
    require(args.frames >= 3300, "--frames must reach the race (>=3300)")
    require(0 < args.reload_frames < args.frames, "invalid --reload-frames")
    fixture = script_path.read_text(encoding="utf-8")

    validate_positive_controls()
    server = OverlayServer(shell_dir, engine_dir)
    server.start()
    if args.verbose:
        print(f"browser server: {server.origin}", flush=True)

    screens: list[tuple[int, bytes, SceneStats]] = []
    with tempfile.TemporaryDirectory(prefix="mdkr64_chrome_") as profile_name:
        corrupt_rom = Path(profile_name) / "supported-header-body-corrupt.z64"
        corrupt_bytes = bytearray(rom.read_bytes())
        corrupt_bytes[0x200000] ^= 0x5A
        corrupt_rom.write_bytes(corrupt_bytes)
        chrome = ChromeProcess(
            chrome_path,
            Path(profile_name),
            args.chrome_flag,
            args.verbose,
        )
        cdp: CDPClient | None = None
        try:
            port = chrome.wait_port()
            cdp = CDPClient(page_websocket(port))
            for domain in ("Page", "Runtime", "Network", "Log", "Inspector"):
                cdp.call(f"{domain}.enable")
            cdp.call(
                "Emulation.setDeviceMetricsOverride",
                {
                    "width": 960,
                    "height": 540,
                    "deviceScaleFactor": 1,
                    "mobile": False,
                },
            )
            first_config = {
                "headlessFrames": args.frames,
                "inputScript": fixture,
                # Prove that the C EEPROM path really awaits browser durability:
                # simulation must not advance while this one acknowledgment is
                # deliberately held.
                "persistDelayOnceMs": 150,
                "env": {
                    "MDKR_TRACE": "1",
                    "MDKR_PRESENT_SCHED_TRACE": "1",
                    "MDKR_AUTOPILOT": "1",
                    "MDKR_AUDIO": "1",
                    "MDKR_EVTQ_STATS": "1",
                    "MDKR_TEST_HEADLESS_AUDIO": "1",
                    # Exercise the capability-correct present fallback on a
                    # real browser GPU. The default CopyDst route was the
                    # shipped baseline and remains covered by native parity.
                    "MDKR_TEST_WEBGPU_SURFACE_USAGES": "attachment",
                    # Open the otherwise inert overlay pass and fail its first
                    # encoder request. The game frame must still present and
                    # the remaining 3,599 frames must proceed.
                    "MDKR_TEST_WEBGPU_OVERLAY": "1",
                    "MDKR_WEBGPU_PIPELINE_TRACE": "1",
                    "MDKR_WEBGPU_FAULT": "overlay.pass",
                    # Exercise the production WebAssembly ownership registry,
                    # snapshot ring, delayed-input replay, and effect journal.
                    # Human item breadth is intentionally kept in its dedicated
                    # non-autopilot matrix. The bridge is
                    # installed only by CDP before page code and accepts only
                    # bounded MDKR_* keys, so this does not widen the launcher.
                    "MDKR_ROLLBACK_LAB": "1",
                    "MDKR_ROLLBACK_LAB_ROUNDTRIP": "1",
                    "MDKR_ROLLBACK_LAB_DELAYED_INPUT": "1",
                    "MDKR_ROLLBACK_LAB_TARGET_TICK": str(
                        ROLLBACK_TARGET_TICK
                    ),
                },
            }
            if args.camera_obstruction:
                first_config["env"]["MDKR_CAMERA_OBSTRUCTION"] = args.camera_obstruction
                first_config["env"]["MDKR_CAMERA_TRACE"] = "1"
            preload = add_config_script(cdp, first_config)
            cdp.call(
                "Page.navigate",
                # mode=remastered is not decoration, and it is not a widening.
                # This gate asserts the REMASTERED feature set below -- the
                # derived font path, the uploaded mip chains, the smooth-sun
                # lighting row, and the completed world-shadow handoff
                # (WORLD_SHADOW_RE) -- and until v0.7.2 it got them by
                # accident, because the launcher's own default WAS Remastered.
                # v0.7.2 flipped that default to Restored (CHANGELOG: "The web
                # launcher now defaults to Restored"), which silently aimed
                # every one of those assertions at a shell that no longer turns
                # the features on. The gate must name the mode it is testing
                # rather than inherit it. mdkr64-shell.js honours ?mode= : it
                # seeds the <select> from the query string and forces
                # `--video-launch-mode` whenever qs.has("mode"), overriding any
                # stored preference. Page.reload keeps the query string, so the
                # reload arms below inherit it -- deliberately, EXCEPT the
                # config-reload arm, which requires a document with no launcher
                # override at all and therefore navigates away from `mode=`
                # explicitly. See there.
                {"url": server.origin + "/?browser-check=1&mode=remastered"},
            )
            wait_launcher(cdp, args.timeout)

            # Save custody must work before selecting a ROM or starting WebGPU.
            # Exercise the real C codec, IDBFS transaction, editor, downloads,
            # complete store loss, and import path before the engine owns /save.
            wait_value(
                cdp,
                'document.getElementById("save-status").textContent',
                lambda value: isinstance(value, str) and "ready" in value.lower(),
                "ROM-free save tools",
                args.timeout,
            )
            blank_summary = cdp.evaluate(
                """(async () => {
                  await MDKRSaveUI.testApi.replace(new Uint8Array(512));
                  return await MDKRSaveUI.testApi.summary();
                })()""",
                await_promise=True,
                timeout=args.timeout,
            )
            require(
                isinstance(blank_summary, dict)
                and blank_summary.get("sha256")
                == hashlib.sha256(bytes(512)).hexdigest(),
                f"ROM-free save seed failed: {blank_summary}",
            )
            rollback = cdp.evaluate(
                """(async () => {
                  __mdkrTestConfig.saveFault = "after-install";
                  let rejected = false;
                  try {
                    await MDKRSaveUI.testApi.replace(
                      new Uint8Array(512).fill(255));
                  } catch (_) {
                    rejected = true;
                  }
                  const bytes = await MDKRSaveUI.testApi.snapshot();
                  return {
                    rejected,
                    exact: bytes && bytes.length === 512 &&
                      bytes.every((value) => value === 0)
                  };
                })()""",
                await_promise=True,
                timeout=args.timeout,
            )
            require(
                rollback == {"rejected": True, "exact": True},
                f"injected browser save failure did not roll back: {rollback}",
            )

            cdp.evaluate('document.getElementById("edit-save").click()')
            wait_value(
                cdp,
                """(() => {
                  const dialog = document.getElementById("save-dialog");
                  return !!dialog && dialog.open &&
                    [...dialog.querySelectorAll("button")].some(
                      (button) => button.textContent.includes("Create fresh slot"));
                })()""",
                lambda value: value is True,
                "save editor draft",
                args.timeout,
            )
            cdp.evaluate(
                """(() => {
                  const button = [...document.querySelectorAll(
                    "#save-dialog-body button")].find(
                      (item) => item.textContent.includes("Create fresh slot"));
                  button.click();
                  return true;
                })()"""
            )
            cdp.evaluate(
                """(() => {
                  const input = document.querySelector(
                    '#save-dialog-body input[type="text"][maxlength="3"]');
                  input.value = "DKR";
                  input.dispatchEvent(new Event("change", {bubbles:true}));
                  window.confirm = () => true;
                  document.getElementById("save-edit-apply").click();
                  return true;
                })()"""
            )
            wait_value(
                cdp,
                'document.getElementById("save-dialog-status").textContent',
                lambda value: isinstance(value, str)
                and "verified" in value.lower(),
                "save editor transaction",
                args.timeout,
            )
            edited_summary = cdp.evaluate(
                "MDKRSaveUI.testApi.summary()", await_promise=True
            )
            require(
                edited_summary.get("slots", [{}])[0].get("status") == "valid"
                and edited_summary.get("slots", [{}])[0].get("name") == "DKR",
                f"semantic editor did not persist the intended slot: {edited_summary}",
            )
            cdp.evaluate('document.getElementById("save-dialog").close()')

            downloads = Path(profile_name) / "downloads"
            downloads.mkdir()
            cdp.call(
                "Browser.setDownloadBehavior",
                {"behavior": "allow", "downloadPath": str(downloads)},
            )
            cdp.evaluate('document.getElementById("download-save").click()')
            portable_path = wait_download(
                downloads, ".mdkr-save", "portable save", args.timeout
            )
            portable = json.loads(portable_path.read_text(encoding="utf-8"))
            require(
                portable.get("format") == "mdkr64-save"
                and portable.get("version") == 1
                and portable.get("payloadFormat") == "dkr-eeprom-4k-be-v1",
                f"portable backup schema is wrong: {portable}",
            )
            portable_payload = base64.b64decode(
                portable.get("payload", ""), validate=True
            )
            require(
                len(portable_payload) == 512
                and hashlib.sha256(portable_payload).hexdigest()
                == portable.get("sha256"),
                "portable backup payload/digest is invalid",
            )
            cdp.evaluate(
                'document.getElementById("download-save-raw").click()'
            )
            raw_path = wait_download(
                downloads, ".eep", "raw EEPROM", args.timeout
            )
            require(
                raw_path.read_bytes() == portable_payload,
                "portable and raw browser exports disagree",
            )

            # Controller Pak custody is a separate persisted artifact from
            # EEPROM. Drive its real file-input validator, all controlled
            # rollback boundaries, and its downloadable full-set bundle before
            # the engine takes ownership of /save.
            pak_image = bytearray(32192)
            pak_image[0:8] = b"MDKRPFS1"
            struct.pack_into(">I", pak_image, 8, 1)
            struct.pack_into(">I", pak_image, 12, len(pak_image))
            pak_image[32:64] = hashlib.sha256(pak_image).digest()
            pak_fixture = Path(profile_name) / "controller-pak-fixture.mdkr-paks"
            pak_fixture.write_text(
                json.dumps({
                    "format": "mdkr64-controller-paks",
                    "version": 1,
                    "createdAt": "browser-check",
                    "packs": [{
                        "port": 1,
                        "byteLength": len(pak_image),
                        "image": base64.b64encode(pak_image).decode("ascii"),
                    }],
                }),
                encoding="utf-8",
            )
            select_file_input(cdp, "#import-paks-input", pak_fixture)
            wait_value(
                cdp,
                'document.getElementById("save-status").textContent',
                lambda value: isinstance(value, str)
                and "imported and verified 1 controller pak" in value.lower(),
                "Controller Pak bundle import",
                args.timeout,
            )
            imported_pak = cdp.evaluate(
                """MDKRSaveUI.testApi.controllerPaks().then((packs) =>
                  packs.map(({port, bytes}) => ({
                    port, bytes: Array.from(bytes)
                  })))""",
                await_promise=True,
                timeout=args.timeout,
            )
            require(
                len(imported_pak) == 1
                and imported_pak[0].get("port") == 1
                and bytes(imported_pak[0].get("bytes", [])) == pak_image,
                "Controller Pak import did not preserve the exact image",
            )
            pak_faults = cdp.evaluate(
                "MDKRSaveUI.testApi.pakFaultPoints"
            )
            require(
                isinstance(pak_faults, list) and len(pak_faults) == 5,
                f"Controller Pak fault vocabulary is incomplete: {pak_faults}",
            )
            for fault_index, fault_point in enumerate(pak_faults):
                rollback = cdp.evaluate(
                    f"""(async () => {{
                      const before =
                        (await MDKRSaveUI.testApi.controllerPaks())[0].bytes;
                      __mdkrTestConfig.saveFault =
                        {json.dumps(fault_point)};
                      let rejected = false;
                      try {{
                        const candidate =
                          new Uint8Array(32192).fill({fault_index + 1});
                        await MDKRSaveUI.testApi.replaceControllerPaks(
                          [candidate, null, null, null]);
                      }} catch (_) {{
                        rejected = true;
                      }}
                      const after =
                        (await MDKRSaveUI.testApi.controllerPaks())[0].bytes;
                      return {{
                        rejected,
                        exact: before.length === after.length &&
                          before.every((value, index) =>
                            value === after[index])
                      }};
                    }})()""",
                    await_promise=True,
                    timeout=args.timeout,
                )
                require(
                    rollback == {"rejected": True, "exact": True},
                    f"{fault_point} did not restore the exact Pak: {rollback}",
                )

            cdp.evaluate('document.getElementById("download-paks").click()')
            pak_export_path = wait_download(
                downloads, ".mdkr-paks", "Controller Pak", args.timeout
            )
            pak_export = json.loads(
                pak_export_path.read_text(encoding="utf-8"))
            require(
                pak_export.get("format") == "mdkr64-controller-paks"
                and pak_export.get("version") == 1
                and len(pak_export.get("packs", [])) == 1
                and pak_export["packs"][0].get("port") == 1
                and base64.b64decode(
                    pak_export["packs"][0].get("image", ""), validate=True
                ) == pak_image,
                f"Controller Pak export schema/image is wrong: {pak_export}",
            )

            cdp.evaluate(
                """(() => {
                  window.confirm = () => true;
                  document.getElementById("clear-save").click();
                  return true;
                })()"""
            )
            wait_value(
                cdp,
                'document.getElementById("save-status").textContent',
                lambda value: isinstance(value, str)
                and "erased" in value.lower(),
                "pre-engine complete save wipe",
                args.timeout,
            )
            cleared = cdp.evaluate(
                """Promise.all([
                  MDKRSaveUI.testApi.snapshot(),
                  MDKRSaveUI.testApi.controllerPaks()
                ]).then(([eeprom, packs]) => ({
                  eeprom,
                  pakCount: packs.length
                }))""",
                await_promise=True,
            )
            require(
                cleared == {"eeprom": None, "pakCount": 0},
                f"save wipe left persisted data behind: {cleared}",
            )

            select_file_input(cdp, "#import-save-input", portable_path)
            wait_value(
                cdp,
                """(() => {
                  const dialog = document.getElementById("save-dialog");
                  const button = [...document.querySelectorAll(
                    "#save-dialog-actions button")].find(
                      (item) => item.textContent.includes(
                        "Replace saved progress"));
                  return !!dialog && dialog.open && !!button && !button.disabled;
                })()""",
                lambda value: value is True,
                "save import preview",
                args.timeout,
            )
            cdp.evaluate(
                """(() => {
                  window.confirm = () => true;
                  const button = [...document.querySelectorAll(
                    "#save-dialog-actions button")].find(
                      (item) => item.textContent.includes(
                        "Replace saved progress"));
                  button.click();
                  return true;
                })()"""
            )
            wait_value(
                cdp,
                'document.getElementById("save-dialog-status").textContent',
                lambda value: isinstance(value, str)
                and "verified" in value.lower(),
                "save import transaction",
                args.timeout,
            )
            imported = cdp.evaluate(
                """MDKRSaveUI.testApi.snapshot().then(
                  (bytes) => Array.from(bytes || []))""",
                await_promise=True,
            )
            require(
                bytes(imported) == portable_payload,
                "wipe → import did not restore the exact exported EEPROM",
            )
            cdp.evaluate('document.getElementById("save-dialog").close()')

            select_rom(cdp, rom, args.timeout)
            click_play(cdp)
            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && globalThis.__mdkrTestSnapshot().phase",
                lambda value: value == "main-started",
                "wasm main() start",
                args.timeout,
            )

            targets = sorted(
                {
                    min(args.frames - 100, value)
                    for value in (400, 1200, 2000, 2800, 3400)
                    if min(args.frames - 100, value) > 0
                }
            )
            for target in targets:
                milestone = wait_value(
                    cdp,
                    "globalThis.__mdkrTestSnapshot()",
                    lambda value, target=target: isinstance(value, dict)
                    and (
                        value.get("frames", 0) >= target
                        or value.get("phase") in {
                            "aborted", "main-threw", "graphics-failed"
                        }
                    ),
                    f"browser frame {target}",
                    args.timeout,
                )
                require(
                    milestone.get("frames", 0) >= target,
                    f"browser stopped before frame {target}: "
                    f"phase={milestone.get('phase')!r} "
                    f"errors={milestone.get('errors', [])[-12:]}",
                )
                png = capture_png(cdp)
                stats = scene_stats(png)
                screens.append((target, png, stats))
                print(
                    f"  frame {target:4d}: colours={stats.distinct:5d} "
                    f"sigma={stats.luma_sigma:5.1f} "
                    f"nonblack={stats.nonblack_fraction:5.1%}",
                    flush=True,
                )
                if target == 400:
                    exercise_fullscreen_rejection(cdp, args.timeout)
                    assert_narrow_touch_status_layout(cdp)
                    print(
                        "    fullscreen entry denial: visible recovery message, "
                        "windowed play retained; narrow touch notice clears chrome",
                        flush=True,
                    )
                    fullscreen = exercise_fullscreen(cdp, args.timeout)
                    print(
                        "    fullscreen: "
                        f"{fullscreen['width']}x{fullscreen['height']} "
                        "entered/exited with 24 live frames",
                        flush=True,
                    )
                    exercise_fullscreen_exit_rejection(cdp, args.timeout)
                    print(
                        "    fullscreen exit denial: visible exit-specific recovery message",
                        flush=True,
                    )
                    overflow = exercise_audio_overflow(cdp, args.timeout)
                    print(
                        "    audio overflow: "
                        f"{overflow['ringDroppedFrames']} ring frames, "
                        f"{overflow['recoveries']} continuity recovery",
                        flush=True,
                    )
                resize = {
                    # HiDPI ultrawide: CSS and backing-store pixels must remain
                    # separate, with one proportional scale on both axes.
                    1200: (630, 270, 2.0, 1260, 540),
                    # Return to an ordinary 4:3 drawable.
                    2000: (640, 480, 1.0, 640, 480),
                    # Enter the race at 21:9 HiDPI after a second live resize.
                    2800: (630, 270, 2.0, 1260, 540),
                }.get(target)
                if resize:
                    resized = resize_canvas(cdp, *resize, args.timeout)
                    print(
                        f"    resize: css={resized['cssWidth']}x"
                        f"{resized['cssHeight']} backing={resized['width']}x"
                        f"{resized['height']} dpr={resize[2]:g}",
                        flush=True,
                    )

            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && globalThis.__mdkrTestSnapshot().phase",
                lambda value: value in {"exited", "aborted", "main-threw"},
                "finite browser run to exit",
                args.timeout,
            )
            first_flush = persist_snapshot(cdp, args.timeout)
            require(
                first_flush.get("error") is None,
                f"first IDBFS flush failed: {first_flush.get('error')}",
            )
            first_snapshot = first_flush["snapshot"]
            require(
                first_snapshot.get("phase") == "exited"
                and first_snapshot.get("exitCode") == 0
                and first_snapshot.get("exitRequested") is True
                and first_snapshot.get("shutdownComplete") is True,
                f"first wasm run did not exit cleanly: {first_snapshot}",
            )
            require(
                first_snapshot.get("abortReason") is None,
                f"first wasm run aborted: {first_snapshot}",
            )
            audio = first_snapshot.get("audio")
            require(
                isinstance(audio, dict)
                and audio.get("ready") is True
                and audio.get("failed") is False
                and audio.get("posted", 0) >= 10000
                and audio.get("contextState") == "closed"
                and audio.get("shutdownComplete") is True
                # Ring-starvation ceiling: some warm-up underflow is normal
                # (the worklet runs before the first PCM lands), but a
                # sustained-starvation regression (e.g., a broken occupancy
                # estimate against the 0.4 s ring cap) would blow far past
                # three seconds' worth of silent output samples.
                and audio.get("underflows", 0) < 3 * 22050,
                "browser AudioWorklet did not run, consume PCM, and close "
                f"cleanly (or starved its ring): {audio}",
            )
            # `--headless-frames` budgets host opportunities. The browser
            # bridge deliberately publishes only successful canvas commits,
            # which can be fewer while WebGPU holds an incomplete async-
            # pipeline frame. The exact relationship is proved below by
            # joining scheduler and backend shutdown telemetry.
            surface_frames = first_snapshot.get("frames")
            require(
                isinstance(surface_frames, (int, float))
                and float(surface_frames).is_integer()
                and 0 < surface_frames <= args.frames,
                "browser published an invalid committed-surface count: "
                f"{surface_frames}",
            )
            first_save = first_snapshot.get("save", {})
            require(
                first_save.get("exists") is True
                and first_save.get("size") == 512,
                f"first run did not create a 512-byte EEPROM: {first_save}",
            )
            first_video_config = first_snapshot.get("videoConfig", {})
            require(
                first_video_config.get("exists") is True
                and first_video_config.get("size", 0) > 32,
                "browser launcher did not seed /save/mdkr64.ini on first boot: "
                f"{first_video_config}",
            )
            durability = first_snapshot.get("saveDurability")
            require(
                isinstance(durability, dict)
                and durability.get("requested", 0) > 0
                and durability.get("committed")
                    == durability.get("requested")
                and durability.get("pending") == 0
                and durability.get("lastError") is None,
                "the browser did not acknowledge every requested save "
                f"generation: {durability}",
            )
            persistence_wait = first_snapshot.get("persistenceWait")
            require(
                isinstance(persistence_wait, dict)
                and persistence_wait.get("requestedMs") == 150
                and persistence_wait.get("elapsedMs", 0) >= 120
                and persistence_wait.get("startFrame", 0) > 0
                and persistence_wait.get("startFrame")
                    == persistence_wait.get("endFrame"),
                "simulation advanced while an EEPROM generation awaited "
                f"IndexedDB durability: {persistence_wait}",
            )
            recovery = first_snapshot.get("saveRecovery", {})
            automatic = recovery.get("automatic", [])
            require(
                any(
                    isinstance(item, dict)
                    and item.get("exists") is True
                    and item.get("size") == 512
                    and save_generation_is_checksum_safe(item.get("bytes"))
                    for item in automatic
                ),
                "gameplay did not retain a checksum-safe automatic save "
                f"generation: {recovery}",
            )
            require(
                all(
                    not isinstance(item, dict)
                    or item.get("exists") is not True
                    or save_generation_is_checksum_safe(item.get("bytes"))
                    for item in automatic
                ),
                "automatic recovery ring contains a checksum-damaged "
                f"generation: {recovery}",
            )
            require(
                first_flush.get("romCount", 0) > 0
                and first_flush.get("saveCount", 0) > 0,
                f"IDBFS stores are empty after flush: {first_flush}",
            )

            storage = cdp.evaluate(
                "globalThis.__mdkrTestState.storage"
            )
            require(storage.get("mounted") is True, f"IDBFS was not mounted: {storage}")
            require(
                storage.get("pickedRomWritten") is True,
                f"fresh picker ROM was not written: {storage}",
            )
            require(
                storage.get("storedRomBeforeWrite", {}).get("exists") is False,
                f"isolated profile unexpectedly began with a ROM: {storage}",
            )
            require(
                storage.get("romBeforeMain", {}).get("size") == ROM_BYTES,
                f"engine did not receive the exact ROM size: {storage}",
            )

            visual_ok, visual_summary = scenes_are_live(
                [item[2] for item in screens]
            )
            require(
                visual_ok,
                "browser screenshots are not live: "
                f"{visual_summary}; WebGPU console tail="
                f"{[line for line in cdp.console if 'webgpu' in line.lower()][-20:]}",
            )

            pace = parse_pace(cdp.console)
            require(len(pace) >= args.frames - 10, f"only {len(pace)} [PACE] rows")
            dt = [
                row[3]
                for row in pace
                if 200 <= row[0] < args.frames and 0.0 < row[3] < 100.0
            ]
            require(len(dt) >= 1000, f"only {len(dt)} usable frame intervals")
            median_dt = statistics.median(dt)
            budget_rows = sorted(
                (row[3], row[0]) for row in pace
                if 200 <= row[0] < args.frames and row[3] > 0.0
            )
            require(
                len(budget_rows) >= 1000,
                f"only {len(budget_rows)} frame intervals for hitch budget",
            )
            p95_dt = budget_rows[math.ceil(len(budget_rows) * 0.95) - 1][0]
            p99_dt = budget_rows[math.ceil(len(budget_rows) * 0.99) - 1][0]
            max_dt, max_dt_frame = budget_rows[-1]
            actual_raf = cdp.evaluate(
                "globalThis.__mdkrActualRafDeltas || []")
            raf_rows = sorted(
                float(value) for value in actual_raf
                if isinstance(value, (int, float)) and value > 0
            ) if isinstance(actual_raf, list) else []
            raf_summary = (
                {
                    "count": len(raf_rows),
                    "p50": raf_rows[len(raf_rows) // 2],
                    "p95": raf_rows[math.ceil(len(raf_rows) * 0.95) - 1],
                    "p99": raf_rows[math.ceil(len(raf_rows) * 0.99) - 1],
                }
                if raf_rows else {}
            )
            cadence_ok = (
                p95_dt <= BROWSER_FRAME_P95_BUDGET_MS
                and p99_dt <= BROWSER_FRAME_P99_BUDGET_MS
            )
            over_p95_budget = [
                (frame, delta) for delta, frame in budget_rows
                if delta > BROWSER_FRAME_P95_BUDGET_MS
            ]
            cadence_failure = (
                "browser cadence exceeded the published hitch budget: "
                f"p95={p95_dt:.2f}/{BROWSER_FRAME_P95_BUDGET_MS:.2f} ms, "
                f"p99={p99_dt:.2f}/{BROWSER_FRAME_P99_BUDGET_MS:.2f} ms; "
                f"over-p95-budget={len(over_p95_budget)}/{len(budget_rows)} "
                f"frames={over_p95_budget[:12]}; rAF={raf_summary}"
            )
            fps = 1000.0 / median_dt
            authored_rows = [row for row in pace if row[0] >= 200]
            stable_r = sum(row[1] == 2 for row in authored_rows)
            stable_ratio = stable_r / max(1, len(authored_rows))
            require(24.0 <= fps <= 36.0, f"median browser cadence is {fps:.1f} fps")
            require(
                all(row[1] >= 2 and row[2] >= 2 for row in authored_rows),
                "browser emitted an update or wall-field count below the "
                "authored two-field minimum",
            )
            require(
                stable_ratio >= 0.80,
                f"only {stable_ratio:.1%} of browser frames used updateRate 2",
            )
            race_rows = [row for row in pace if row[4] is not None]
            require(len(race_rows) >= 150, f"only {len(race_rows)} in-race [PACE] rows")
            require(
                max(row[4] or -1 for row in race_rows) >= 1,
                "browser racer did not advance beyond checkpoint 0",
            )
            console_text = "\n".join(cdp.console)
            rollback_summary = assert_browser_rollback(console_text)
            if args.camera_obstruction:
                camera_rows = [
                    match.groups()
                    for line in cdp.console
                    if (match := CAMERA_OBSTRUCTION_RE.search(line))
                ]
                expected_gate = args.camera_obstruction.replace("-", "_").upper()
                require(
                    len(camera_rows) >= args.frames - 100
                    and all(row[0] == expected_gate for row in camera_rows),
                    "browser camera obstruction policy/telemetry was incomplete: "
                    f"rows={len(camera_rows)} expected={expected_gate}",
                )
                require(
                    all(int(value) == 0 for row in camera_rows for value in row[1:3])
                    and all(int(value) == 0 for row in camera_rows for value in row[4:7])
                    # A depenetrate-only camera (door/cutscene) may briefly lose
                    # its target; every other camera must never hide it.
                    and all(int(row[7]) == 0 for row in camera_rows
                            if int(row[9]) == 0),
                    "browser camera obstruction reported an authority, projection, "
                    "penetration, invalid, or degraded result",
                )
                if args.camera_obstruction == "modern":
                    require(
                        sum(int(row[3]) for row in camera_rows) > 0,
                        "browser Modern route never exercised camera correction",
                    )
                dynamic_rows = [
                    match.groups()
                    for line in cdp.console
                    if (match := CAMERA_DYNAMIC_RE.search(line))
                ]
                require(
                    len(dynamic_rows) == len(camera_rows)
                    and all(
                        int(value) == 0
                        for row in dynamic_rows
                        for value in (row[4], row[5], row[7], row[8], row[9])
                    ),
                    "browser dynamic camera publication was incomplete or degraded",
                )
            require(
                len(PACE_INIT_RE.findall(console_text)) == 1,
                "browser did not initialize exactly one NTSC authored-cadence "
                "realtime clock",
            )
            wgpu_shutdown_rows = [
                match.groups()
                for line in cdp.console
                if (match := WGPU_SHUTDOWN_RE.search(line))
            ]
            require(
                len(wgpu_shutdown_rows) == 1
                and wgpu_shutdown_rows[0][0] in {"owned", "borrowed"}
                and int(wgpu_shutdown_rows[0][1]) > 0
                and int(wgpu_shutdown_rows[0][2]) > 0
                and all(int(value) == 0 for value in wgpu_shutdown_rows[0][3:]),
                "browser WebGPU final teardown did not release every child "
                "under its declared root-ownership mode: "
                f"{wgpu_shutdown_rows}",
            )
            world_shadow_rows = [
                tuple(map(int, match.groups()))
                for line in cdp.console
                if (match := WORLD_SHADOW_RE.search(line))
            ]
            require(
                len(world_shadow_rows) == 1
                and world_shadow_rows[0][0] > 0
                and world_shadow_rows[0][1] > 0
                and world_shadow_rows[0][2] <
                    world_shadow_rows[0][0]
                and world_shadow_rows[0][3] == 0
                and world_shadow_rows[0][4] == 0,
                "browser real-shadow handoff never became complete or "
                f"degraded unexpectedly: {world_shadow_rows}",
            )
            wgpu_perf_rows = [
                tuple(map(int, match.groups()))
                for line in cdp.console
                if (match := WGPU_PERF_RE.search(line))
            ]
            require(
                len(wgpu_perf_rows) == 1,
                f"browser emitted {len(wgpu_perf_rows)} WebGPU perf rows",
            )
            (
                async_creates,
                async_ready,
                async_failed,
                hold_frames,
                max_hold_streak,
                max_pipeline_frames,
                max_pending,
            ) = wgpu_perf_rows[0]
            pipeline_trace = [
                line for line in cdp.console
                if "[WGPU-PIPELINE]" in line
            ]
            require(
                async_creates > 0
                and async_ready == async_creates
                and async_failed == 0
                and hold_frames
                    <= async_creates * BROWSER_PIPELINE_FRAME_BUDGET
                and max_hold_streak <= BROWSER_PIPELINE_FRAME_BUDGET
                and max_pipeline_frames <= BROWSER_PIPELINE_FRAME_BUDGET
                and 0 < max_pending <= async_creates,
                "browser async-pipeline telemetry is incoherent: "
                f"{wgpu_perf_rows[0]}; trace={pipeline_trace[-12:]}",
            )
            # Collect the independent pipeline evidence before reporting a
            # cadence miss. A loaded or occluded host can otherwise mask a
            # renderer regression in the same expensive real-browser run.
            require(cadence_ok, cadence_failure)
            sched = parse_single_integer_summary(
                cdp.console,
                PRESENT_SCHED_SUMMARY_RE,
                "PRESENTSCHED-SUMMARY",
            )
            required_sched = {
                "entries",
                "ticks",
                "presents",
                "surfaceupdates",
                "simticks",
                "issued",
                "pending",
                "catchup",
                "elided",
                "updates",
                "updatebad",
                "updatemin",
                "updatemax",
                "effectiveupdates",
                "bootstrap",
                "effectivebootstrap",
                "tickfields",
            }
            require(
                required_sched <= sched.keys(),
                "browser scheduler summary is missing accounting fields: "
                f"{sorted(required_sched - sched.keys())}",
            )
            # The requested frame budget is the golden here. Passing the run's
            # own sched["ticks"] made the tick expectation self-fulfilling: it
            # reduced to "simticks equals ticks" and would have accepted any
            # number of authoritative ticks at all.
            #
            # A real rAF timeline is not a metronome: a late frame leaves the
            # fixed-tick clock owing more than one tick, which the scheduler
            # pays back as grouped catch-up. So the anchor is args.frames plus
            # an absolutely bounded catch-up allowance -- measured at 4 ticks
            # per 3600 frames on a loaded host -- rather than whatever number
            # the run happened to reach. The debt high-water marks are then
            # bounded by that same catch-up: a browser that grouped four
            # tickets in total cannot have held more than five at once.
            catchup_budget = max(8, args.frames // 100)
            require(
                0 <= sched["catchup"] <= catchup_budget,
                f"browser catch-up ticks {sched['catchup']} exceed the "
                f"{catchup_budget}-tick budget for {args.frames} host "
                f"opportunities: {sched}",
            )
            debt_bound = (1, 1 + sched["catchup"])
            conservation_error = completed_tick_conservation(
                sched, args.frames + sched["catchup"], "browser scheduler",
                expected_lead=debt_bound, expected_max_pending=debt_bound)
            require(
                conservation_error is None
                and sched["entries"] == args.frames
                and sched["presents"] == args.frames
                and sched["ticks"] == sched["entries"] + sched["catchup"]
                and sched["elided"] == sched["catchup"],
                "browser host opportunities, catch-up ticks, and completed "
                f"work do not account exactly: {conservation_error}; {sched}",
            )
            require(
                sched["updates"] + 1 == sched["simticks"]
                and sched["effectiveupdates"] + 1 == sched["simticks"]
                and sched["effectivebootstrap"] == sched["bootstrap"],
                "browser fixed-tick accounting does not include exactly one "
                f"matching bootstrap update: {sched}",
            )
            require(
                sched["updatebad"] == 0
                and sched["updatemin"] == sched["tickfields"]
                and sched["updatemax"] == sched["tickfields"],
                "browser emitted a non-fixed steady-state game update: "
                f"{sched}",
            )
            backpressure = parse_single_integer_summary(
                cdp.console,
                WGPU_BACKPRESSURE_RE,
                "WGPU-BACKPRESSURE",
            )
            required_backpressure = {
                "cap",
                "submitted",
                "completed",
                "presented",
                "held",
                "unavailable",
                "inflight",
                "highwater",
                "skips",
                "failures",
                "abandoned",
            }
            require(
                required_backpressure <= backpressure.keys(),
                "browser WebGPU backpressure row is missing accounting "
                "fields: "
                f"{sorted(required_backpressure - backpressure.keys())}",
            )
            startup_non_submitted = (
                sched["presents"] - backpressure["submitted"]
            )
            require(
                0 <= startup_non_submitted <= 3,
                "browser exceeded its bounded three-opportunity graphics "
                f"bootstrap: {startup_non_submitted} opportunities; "
                f"scheduler={sched} backpressure={backpressure}",
            )
            accounted_submissions = (
                backpressure["presented"]
                + backpressure["held"]
                + backpressure["unavailable"]
            )
            accounted_opportunities = (
                startup_non_submitted + backpressure["submitted"]
            )
            require(
                backpressure["submitted"]
                    == accounted_submissions
                and sched["presents"] == accounted_opportunities
                and backpressure["presented"]
                    == sched["surfaceupdates"]
                    == int(surface_frames)
                and backpressure["held"] == hold_frames
                and backpressure["unavailable"] == 0,
                "browser canvas commits, WebGPU holds, and startup bootstrap "
                "do not exactly account for every host opportunity: "
                f"surface={surface_frames} holdFrames={hold_frames} "
                f"startup={startup_non_submitted} scheduler={sched} "
                f"backpressure={backpressure}",
            )
            # `abandoned` is teardown-only: at shutdown the renderer polls the
            # completion owner once and then retires whatever the page never
            # gave it an event-loop turn to complete (gfx_webgpu.c's
            # wgpu_abandon_in_flight). That is why it is not pinned at zero
            # here -- but the cap alone would let any number up to the cap pass
            # unexplained, so the count has to be exactly what the renderer
            # itself declared it retired. No diagnostic row means the only
            # acceptable value is zero.
            retired_rows = [
                int(match.group(1))
                for line in cdp.console
                if (match := WGPU_RETIRE_RE.search(line))
            ]
            require(
                (backpressure["completed"] + backpressure["abandoned"]
                    == backpressure["submitted"])
                and backpressure["abandoned"] == sum(retired_rows)
                and len(retired_rows) <= 1
                and 0 <= backpressure["abandoned"] <= backpressure["cap"]
                and backpressure["inflight"] == 0
                and backpressure["cap"] == BROWSER_GPU_IN_FLIGHT_CAP
                and 0 < backpressure["highwater"] <= backpressure["cap"]
                and backpressure["skips"] == 0
                and backpressure["failures"] == 0,
                "browser WebGPU queue completion or bounded shutdown "
                f"accounting is incoherent: {backpressure}; "
                f"declared teardown retirements={retired_rows}",
            )
            gfx_shutdown_rows = [
                tuple(int(value) for value in match.groups())
                for line in cdp.console
                if (match := GFX_SHUTDOWN_RE.search(line))
            ]
            require(
                len(gfx_shutdown_rows) == 1
                and gfx_shutdown_rows[0][0] >= gfx_shutdown_rows[0][1]
                and gfx_shutdown_rows[0][2] >= 0
                and gfx_shutdown_rows[0][3] == 0
                and gfx_shutdown_rows[0][4] > 0
                and gfx_shutdown_rows[0][5] == 1
                and gfx_shutdown_rows[0][6] == 0,
                f"browser frontend teardown is incoherent: {gfx_shutdown_rows}",
            )
            teardown_markers = (
                "[AUDIO-SHUTDOWN] device=0 web=1 complete=1",
                "[WGPU-SHUTDOWN]",
                "[GFX-SHUTDOWN]",
                "[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0",
                "[SDL-SHUTDOWN] window=0 glContext=0 controllers=0 sdl=0",
            )
            teardown_positions = [
                console_text.find(marker) for marker in teardown_markers
            ]
            require(
                all(position >= 0 for position in teardown_positions)
                and teardown_positions == sorted(teardown_positions),
                "browser teardown did not run in "
                "audio->WebGPU->frontend->host->SDL order: "
                f"{teardown_positions}",
            )
            font_rows = [
                match.groups()
                for line in cdp.console
                if (match := FONT_RE.search(line))
            ]
            # Remastered derives text two ways on this route: SDF contours for
            # DKR's own coloured lettering, and an outline redraw for the two
            # plain faces. BOTH are asserted independently. Asserting only the
            # sum would let a regression that stopped one path entirely pass on
            # the other's uploads, and this fixture measurably exercises both --
            # 58 SDF and 2 outline uploads on the run that established these
            # numbers, so neither assertion is vacuous.
            #
            # The fourth field must be zero: no glyph may be clipped by the
            # per-cell backstop.
            require(
                len(font_rows) == 1
                and int(font_rows[0][0]) > 0
                and int(font_rows[0][1]) > 0
                and int(font_rows[0][2]) == 0
                and int(font_rows[0][3]) == 0,
                "browser Remastered font path did not derive cleanly: "
                f"{font_rows}",
            )
            mip_rows = [
                match.groups()
                for line in cdp.console
                if (match := MIP_RE.search(line))
            ]
            require(
                len(mip_rows) == 1
                and int(mip_rows[0][0]) > 0
                and int(mip_rows[0][1]) > int(mip_rows[0][0]),
                f"browser Remastered mip chains were not uploaded: {mip_rows}",
            )
            rl1_rows = [
                match.groups()
                for line in cdp.console
                if (match := RL1_RE.search(line))
            ]
            require(
                rl1_rows == [("baked", "0")],
                "RL-1 diagnostic escaped into the default browser path: "
                f"{rl1_rows}",
            )
            light_rows = [
                match.groups()
                for line in cdp.console
                if (match := LIGHT_RE.search(line))
            ]
            require(
                len(light_rows) == 1
                and light_rows[0][0] == "smooth-sun"
                and light_rows[0][1] == "1"
                and 0.10 <= float(light_rows[0][2]) <= 0.16
                and int(light_rows[0][4]) > 0
                and int(light_rows[0][5]) > 0
                and int(light_rows[0][6]) == 0
                and light_rows[0][7]
                    == "srgb-authored/linear-light/srgb-output",
                "browser Remastered lighting path did not run cleanly: "
                f"{light_rows}",
            )
            limit_rows = [
                tuple(int(value) for value in match.groups())
                for line in cdp.console
                if (match := WGPU_LIMIT_RE.search(line))
            ]
            require(
                len(limit_rows) == 1,
                f"missing/duplicate WebGPU limit telemetry: {limit_rows}",
            )
            (
                shaders,
                shader_cap,
                pipelines,
                attrs,
                attr_cap,
                vary,
                vary_cap,
                overflow,
                pipeline_failures,
                vertex_bytes,
                vertex_segments,
                vertex_segment_cap,
                textures,
                samplers,
                bind_groups,
                bind_group_cap,
            ) = limit_rows[0]
            require(
                shaders > 0
                and shaders < shader_cap
                and pipelines > 0
                and attrs <= attr_cap
                and vary <= vary_cap
                and overflow == 0
                and pipeline_failures == 0
                and vertex_bytes > 0
                and vertex_segments == 1
                and vertex_bytes * 2 < vertex_segment_cap
                and textures > 0
                and 0 < samplers <= 64
                and 0 < bind_groups <= bind_group_cap
                and bind_groups * 2 < bind_group_cap,
                f"WebGPU material corpus exceeded a runtime limit: {limit_rows[0]}",
            )
            require(
                console_text.count(
                    "overlay pass unavailable; presenting game frame without overlay"
                )
                == 1,
                "browser did not survive exactly one injected overlay-pass "
                "creation failure",
            )
            raw16_rows = [
                match.groups()
                for line in cdp.console
                if (match := RAW16_ACTIVE_RE.search(line))
            ]
            require(
                raw16_rows,
                "browser race never reached a RAW16 instrument load",
            )
            require(
                all(
                    mode == "fixed" and int(loads) > 0 and int(byte_count) > 0
                    for mode, loads, byte_count in raw16_rows
                ),
                f"browser RAW16 loader did not use the production endian path: "
                f"{raw16_rows}",
            )
            event_queues = parse_event_queue_peaks(cdp.console)
            require(
                {item[2] for item in event_queues.values()} == {50, 256, 512},
                "browser did not measure all music/jingle/SFX queues: "
                f"{event_queues}",
            )
            tight_queues = {
                index: values
                for index, values in event_queues.items()
                if values[1] * 2 > values[2]
            }
            require(
                not tight_queues,
                "browser event queue used more than half its capacity: "
                f"{tight_queues}",
            )
            require(
                "level_load: levelId=5" in console_text,
                "browser fixture never loaded Ancient Lake",
            )
            require(
                # output= is the window; drawable= in the layout line is the
                # scaled scene once Video.RenderScale > 1, so it is the wrong
                # field to ask "did the engine see my resize?" with.
                "[DISPLAY] output=1260x540" in console_text
                and "[DISPLAY] output=640x480" in console_text,
                "engine did not consume both live browser resize dimensions",
            )
            fatal = [
                marker
                for marker in FATAL_MARKERS
                if marker.lower() in console_text.lower()
            ]
            event_queue_lines = [
                line for line in cdp.console if "[EVTQ]" in line
            ]
            require(
                not fatal,
                f"browser console contains fatal markers: {fatal}; "
                f"queues={event_queues}; tail={event_queue_lines[-16:]}",
            )

            # Replace the preload before reload: the second document only needs
            # to prove stored-ROM/save restore and a second clean boot.
            cdp.call(
                "Page.removeScriptToEvaluateOnNewDocument",
                {"identifier": preload},
            )
            second_console_start = len(cdp.console)
            second_config = {
                "headlessFrames": args.reload_frames,
                # First Forget attempt must fail transactionally, leaving the
                # stored ROM playable. The later real Forget consumes no fault.
                "idbClearFailOnce": True,
                "env": {
                    "MDKR_TRACE": "1",
                    "MDKR_AUDIO": "1",
                    "MDKR_EVTQ_STATS": "1",
                    "MDKR_TEST_SFX_EVENT_CAPACITY": "1",
                    "MDKR_TEST_HEADLESS_AUDIO": "1",
                },
            }
            second_preload = add_config_script(cdp, second_config)
            cdp.call("Page.reload", {"ignoreCache": True})
            wait_launcher(cdp, args.timeout)
            stored_ui = wait_value(
                cdp,
                """(() => {
                  const play = document.getElementById("play");
                  const status = document.getElementById("rom-status");
                  return {disabled: play.disabled, text: status.textContent,
                          forget: !document.getElementById("forget").hidden};
                })()""",
                lambda item: isinstance(item, dict)
                and not item.get("disabled")
                and item.get("forget")
                and "stored" in item.get("text", ""),
                "stored-ROM launcher state",
                args.timeout,
            )
            require("stored" in stored_ui["text"], f"stored ROM is unreachable: {stored_ui}")

            cdp.evaluate('document.getElementById("forget").click()')
            wait_value(
                cdp,
                'document.getElementById("forget-rom-dialog").open',
                lambda item: item is True,
                "failed-forget confirmation",
                args.timeout,
            )
            cdp.evaluate('document.getElementById("forget-rom-confirm").click()')
            failed_forget = wait_value(
                cdp,
                """(() => ({
                  status: document.getElementById("rom-status").textContent,
                  playDisabled: document.getElementById("play").disabled,
                  forgetHidden: document.getElementById("forget").hidden,
                  focus: document.activeElement && document.activeElement.id,
                  dialogOpen: document.getElementById("forget-rom-dialog").open
                }))()""",
                lambda item: isinstance(item, dict)
                and "Couldn't clear" in item.get("status", "")
                and not item.get("playDisabled")
                and not item.get("forgetHidden")
                and item.get("focus") == "forget"
                and not item.get("dialogOpen"),
                "transactional failed Forget",
                args.timeout,
            )
            require(
                not failed_forget["playDisabled"],
                f"failed Forget displaced the stored ROM: {failed_forget}",
            )

            # A candidate with the exact supported header/CRC pair but one
            # damaged body byte must fail the complete-image hash without
            # displacing the already-verified stored ROM.
            select_file_input(cdp, "#rom-input", corrupt_rom)
            cdp.evaluate(
                'document.getElementById("rom-input").dispatchEvent('
                'new Event("change", {bubbles:true}))'
            )
            replacement = wait_value(
                cdp,
                """(() => {
                  const play = document.getElementById("play");
                  const status = document.getElementById("rom-status");
                  return {disabled: play.disabled, className: status.className,
                          text: status.textContent};
                })()""",
                lambda item: isinstance(item, dict)
                and not item.get("disabled")
                and item.get("className") == "err"
                and "SHA-256" in item.get("text", "")
                and "previously verified" in item.get("text", ""),
                "transactional corrupt-ROM replacement",
                args.timeout,
            )
            require(
                "modified or damaged" in replacement["text"],
                f"corrupt replacement verdict was not actionable: {replacement}",
            )
            click_play(cdp)
            wait_value(
                cdp,
                "globalThis.__mdkrTestState && globalThis.__mdkrTestState.phase",
                lambda value: value == "main-started",
                "reload main() start",
                args.timeout,
            )
            second_storage = cdp.evaluate(
                "globalThis.__mdkrTestState.storage"
            )
            require(
                second_storage.get("mounted") is True
                and second_storage.get("pickedRomWritten") is False,
                f"reload did not use mounted storage: {second_storage}",
            )
            require(
                second_storage.get("storedRomBeforeWrite", {}).get("size")
                == ROM_BYTES,
                f"persisted ROM was not restored before main: {second_storage}",
            )
            require(
                same_save(
                    first_save,
                    second_storage.get("saveBeforeMain", {}),
                ),
                "EEPROM bytes did not round-trip exactly across browser reload: "
                f"first={first_save} second={second_storage.get('saveBeforeMain')}",
            )
            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && globalThis.__mdkrTestSnapshot().phase",
                lambda value: value in {"exited", "aborted", "main-threw"},
                "reload run to exit",
                args.timeout,
            )
            second_flush = persist_snapshot(cdp, args.timeout)
            require(
                second_flush.get("error") is None
                and second_flush.get("snapshot", {}).get("exitCode") == 0
                and second_flush.get("snapshot", {}).get("phase") == "exited"
                and second_flush.get("snapshot", {}).get(
                    "shutdownComplete"
                ) is True,
                f"reload run did not exit/persist cleanly: {second_flush}",
            )
            event_drop_controls = [
                match
                for line in cdp.console[second_console_start:]
                if (match := EVTQ_DROP_RE.search(line))
                and int(match.group(1)) == 2
                and int(match.group(5)) == 1
            ]
            require(
                event_drop_controls,
                "one-entry SFX positive control did not emit a q2 capacity-1 "
                "drop diagnostic",
            )

            # A wasm build cannot fall back to desktop GL. Prove the other half
            # of the recovery contract: an engine-level WebGPU bring-up failure
            # replaces the canvas with a stable, actionable launcher message,
            # while keeping the independent save/ROM recovery controls usable.
            cdp.call(
                "Page.removeScriptToEvaluateOnNewDocument",
                {"identifier": second_preload},
            )

            # UI-4: exercise the real in-game menu in wasm, persist its config
            # through the engine's IDBFS coalescer, then start a fresh document
            # without launcher overrides and require the live display policy to
            # resolve from that file. This is the browser half of
            # check_video_options.py's native transaction matrix.
            video_console_start = len(cdp.console)
            video_fixture = (
                Path(__file__).resolve().parent
                / "input_scripts"
                / "nav_video_options_mutate.txt"
            ).read_text(encoding="utf-8")
            video_preload = add_config_script(
                cdp,
                {
                    "headlessFrames": 2900,
                    "inputScript": video_fixture,
                    "env": {
                        "MDKR_TRACE": "1",
                        "MDKR_AUDIO": "0",
                    },
                },
            )
            cdp.call("Page.reload", {"ignoreCache": True})
            wait_launcher(cdp, args.timeout)
            click_play(cdp)
            video_exit = wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && globalThis.__mdkrTestSnapshot()",
                lambda item: isinstance(item, dict)
                and item.get("phase") == "exited"
                and item.get("exitCode") == 0,
                "browser Video Options mutation run",
                args.timeout,
            )
            video_flush = persist_snapshot(cdp, args.timeout)
            require(
                video_flush.get("error") is None
                and video_exit.get("videoConfig", {}).get("exists") is True,
                f"browser Video Options config did not persist: {video_flush}",
            )
            video_text = cdp.evaluate(
                """new TextDecoder().decode(
                  module.FS.readFile("/save/mdkr64.ini"))"""
            )
            for entry in (
                "Mode=custom",
                "RemasterFX=1",
                "Aspect=16:10",
                "RenderScale=3",
                "AnisotropicFiltering=8",
                "Mipmaps=1",
                "GameplayFOV=50",
            ):
                require(
                    entry in video_text,
                    f"browser config omitted {entry!r}:\n{video_text}",
                )
            video_console = cdp.console[video_console_start:]
            require(
                any(
                    "video_options: item=0 value=PURE result=2" in line
                    for line in video_console
                )
                and any(
                    "video_options: item=1 value=3X result=1" in line
                    for line in video_console
                )
                and any(
                    "video_options: item=2 value=16:10 result=1" in line
                    for line in video_console
                )
                and any(
                    "video_options: item=4 value=RESTORED result=2" in line
                    for line in video_console
                ),
                "wasm did not traverse live and restart-staged Video Options: "
                + "\n".join(video_console[-80:]),
            )
            cdp.call(
                "Page.removeScriptToEvaluateOnNewDocument",
                {"identifier": video_preload},
            )

            config_reload_console_start = len(cdp.console)
            config_reload_preload = add_config_script(
                cdp,
                {
                    "headlessFrames": 120,
                    "env": {"MDKR_TRACE": "1", "MDKR_AUDIO": "0"},
                },
            )
            # NAVIGATE, not reload, and to the URL WITHOUT `mode=`. This arm's
            # whole point is a fresh document with NO launcher override, so that
            # the display policy has to come from /save/mdkr64.ini. Page.reload
            # preserves the query string, and the initial navigate carries
            # `mode=remastered` for the Remastered feature assertions further up
            # — `qs.has("mode")` makes mdkr64-shell.js force
            # `--video-launch-mode` past any stored config, which is exactly the
            # override this arm must not have.
            cdp.call(
                "Page.navigate",
                {"url": server.origin + "/?browser-check=1"},
            )
            wait_launcher(cdp, args.timeout)
            click_play(cdp)
            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && globalThis.__mdkrTestSnapshot()",
                lambda item: isinstance(item, dict)
                and item.get("phase") == "exited"
                and item.get("exitCode") == 0,
                "browser Video Options reload",
                args.timeout,
            )
            config_reload_console = cdp.console[config_reload_console_start:]
            require(
                any(
                    "[DISPLAY] widescreen=on aspect=forced(1.60000) "
                    "gameplay-vfov=scaled(50.0@authored60)" in line
                    for line in config_reload_console
                ),
                "fresh browser document did not resolve the in-game aspect/FOV "
                "from /save/mdkr64.ini:\n"
                + "\n".join(config_reload_console[-80:]),
            )
            require(
                any(
                    "[DISPLAY] output=" in line and "scale=3.00" in line
                    for line in config_reload_console
                ),
                "fresh browser document did not restore 3x supersampling:\n"
                + "\n".join(config_reload_console[-80:]),
            )
            display_rows = [
                tuple(float(value) for value in match.groups())
                for line in config_reload_console
                if (match := DISPLAY_RE.search(line))
            ]
            require(
                bool(display_rows)
                and display_rows[-1][4] == 3.0
                and display_rows[-1][5] < 3.0
                and display_rows[-1][2] * display_rows[-1][3]
                <= 2560 * 1440,
                "browser render-area budget did not proportionally cap the "
                "3x target:\n"
                + "\n".join(config_reload_console[-80:]),
            )
            cdp.call(
                "Page.removeScriptToEvaluateOnNewDocument",
                {"identifier": config_reload_preload},
            )

            # The production run above proved overlay.pass is a local failure.
            # Exercise the other optional-overlay constructor independently:
            # losing the surface view must likewise present the game without
            # the overlay and must never enter graphics-failed.
            overlay_view_console_start = len(cdp.console)
            overlay_view_preload = add_config_script(
                cdp,
                {
                    "headlessFrames": 120,
                    "env": {
                        "MDKR_TEST_WEBGPU_OVERLAY": "1",
                        "MDKR_WEBGPU_FAULT": "overlay.view",
                    },
                },
            )
            cdp.call("Page.reload", {"ignoreCache": True})
            wait_launcher(cdp, args.timeout)
            click_play(cdp)
            overlay_view_exit = wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && globalThis.__mdkrTestSnapshot()",
                lambda item: isinstance(item, dict)
                and item.get("phase") == "exited"
                and item.get("exitCode") == 0,
                "overlay-view local-degradation run",
                args.timeout,
            )
            overlay_view_console = cdp.console[overlay_view_console_start:]
            require(
                any(
                    "[webgpu-fault] injected overlay.view@1" in line
                    for line in overlay_view_console
                )
                and overlay_view_exit.get("phase") == "exited",
                "overlay.view was not reached or did not stay local: "
                + "\n".join(overlay_view_console[-40:]),
            )
            cdp.call(
                "Page.removeScriptToEvaluateOnNewDocument",
                {"identifier": overlay_view_preload},
            )

            # A fresh ROM must remain playable if IndexedDB accepts the
            # in-memory write but rejects the immediate durable sync. Surface
            # that precisely, prove an independent save fault stacks above it,
            # then hold automatic retries while the public control exercises a
            # failure and a successful retry itself.
            rom_warning_preload = add_config_script(
                cdp,
                {
                    # Keep the engine session alive while both explicit Retry
                    # transactions run. A short finite run can reach its
                    # orderly engine-exit flush first; that flush legitimately
                    # persists the ROM and makes a later Retry a no-op, which
                    # would not be evidence about the public control.
                    "headlessFrames": 100000,
                    "romSyncFailCount": 2,
                    "disableAutoPersistence": True,
                    "holdRomPersistenceForPublicRetry": True,
                },
            )
            cdp.call("Page.reload", {"ignoreCache": True})
            wait_launcher(cdp, args.timeout)
            select_rom(cdp, rom, args.timeout)
            click_play(cdp)
            wait_value(
                cdp,
                """(() => {
                  const banner = document.getElementById("rom-session-banner");
                  return {stage: !document.getElementById("stage").hidden,
                          hidden: !banner || banner.hidden,
                          text: banner ? banner.textContent : ""};
                })()""",
                lambda item: isinstance(item, dict)
                and item.get("stage") and not item.get("hidden")
                and "session only" in item.get("text", "").lower()
                and "earlier ROM" in item.get("text", ""),
                "session-only ROM persistence warning",
                args.timeout,
            )
            cdp.evaluate("globalThis.__mdkrTestForceSavePersistenceFailure()")
            dual_warning = wait_value(
                cdp,
                """(() => {
                  const rom = document.getElementById("rom-session-banner");
                  const save = document.getElementById("save-banner");
                  const rr = rom.getBoundingClientRect();
                  const sr = save.getBoundingClientRect();
                  return {romHidden: rom.hidden, saveHidden: save.hidden,
                          romText: rom.textContent, saveText: save.textContent,
                          separated: rr.bottom <= sr.top};
                })()""",
                lambda item: isinstance(item, dict)
                and not item.get("romHidden") and not item.get("saveHidden")
                and item.get("separated") is True
                and "session only" in item.get("romText", "").lower()
                and "injected save storage failure" in item.get("saveText", ""),
                "stacked ROM and save durability warnings",
                args.timeout,
            )
            require(dual_warning["separated"],
                    f"durability warnings overlap: {dual_warning}")
            # The messages are independently live regions, but their visual
            # container must continue to stack rather than overlap when a
            # phone-width viewport or enlarged text wraps either message.
            cdp.call(
                "Emulation.setDeviceMetricsOverride",
                {"width": 320, "height": 640, "deviceScaleFactor": 1,
                 "mobile": False},
            )
            narrow_warning = cdp.evaluate(
                """(() => {
                  const group = document.getElementById("durability-banners");
                  const rom = document.getElementById("rom-session-banner");
                  const save = document.getElementById("save-banner");
                  rom.style.fontSize = "30px";
                  save.style.fontSize = "30px";
                  const rr = rom.getBoundingClientRect();
                  const sr = save.getBoundingClientRect();
                  return {role: group.getAttribute("role"),
                          label: group.getAttribute("aria-label"),
                          separated: rr.bottom <= sr.top,
                          withinViewport: rr.left >= 0 && sr.left >= 0 &&
                            rr.right <= window.innerWidth &&
                            sr.right <= window.innerWidth};
                })()""",
            )
            require(
                narrow_warning.get("role") == "group"
                and narrow_warning.get("label") == "Storage notices"
                and narrow_warning.get("separated") is True
                and narrow_warning.get("withinViewport") is True,
                f"narrow/enlarged durability notices are not accessible: {narrow_warning}",
            )
            retry_pending = cdp.evaluate(
                """(() => {
                  const retry = document.getElementById("rom-storage-retry");
                  const message = document.getElementById("rom-session-message");
                  if (!retry || retry.hidden || retry.disabled || !message) return null;
                  retry.focus();
                  retry.click();
                  return {
                    activeId: document.activeElement && document.activeElement.id,
                    retryHidden: retry.hidden,
                    retryDisabled: retry.disabled,
                    retryText: retry.textContent,
                    message: message.textContent,
                  };
                })()"""
            )
            require(
                isinstance(retry_pending, dict)
                and retry_pending.get("retryHidden") is False
                and retry_pending.get("retryDisabled") is True
                and retry_pending.get("retryText") == "Retrying…"
                and retry_pending.get("message") == "Retrying browser storage…",
                f"public ROM Retry did not enter a visible pending state: {retry_pending}",
            )
            wait_value(
                cdp,
                """(() => {
                  const retry = document.getElementById("rom-storage-retry");
                  const message = document.getElementById("rom-session-message");
                  return {
                    activeId: document.activeElement && document.activeElement.id,
                    retryHidden: retry.hidden,
                    retryDisabled: retry.disabled,
                    retryText: retry.textContent,
                    message: message.textContent,
                  };
                })()""",
                lambda item: isinstance(item, dict)
                and item.get("activeId") == "rom-storage-retry"
                and item.get("retryHidden") is False
                and item.get("retryDisabled") is False
                and item.get("retryText") == "Retry browser storage"
                and "still could not be saved" in item.get("message", ""),
                "public ROM Retry restores focus after a failed transaction",
                args.timeout,
            )
            retry_success_pending = cdp.evaluate(
                """(() => {
                  const retry = document.getElementById("rom-storage-retry");
                  const message = document.getElementById("rom-session-message");
                  if (!retry || retry.hidden || retry.disabled || !message) return null;
                  retry.click();
                  return {
                    retryHidden: retry.hidden,
                    retryDisabled: retry.disabled,
                    retryText: retry.textContent,
                    message: message.textContent,
                  };
                })()"""
            )
            require(
                isinstance(retry_success_pending, dict)
                and retry_success_pending.get("retryHidden") is False
                and retry_success_pending.get("retryDisabled") is True
                and retry_success_pending.get("retryText") == "Retrying…"
                and retry_success_pending.get("message") == "Retrying browser storage…",
                "successful public ROM Retry did not enter a visible pending "
                f"state: {retry_success_pending}",
            )
            wait_value(
                cdp,
                """(() => {
                  const banner = document.getElementById("rom-session-banner");
                  const retry = document.getElementById("rom-storage-retry");
                  const snap = globalThis.__mdkrTestSnapshot();
                  return {hidden: banner.hidden, text: banner.textContent,
                          success: banner.classList.contains("ok"),
                          retryHidden: retry.hidden,
                          activeId: document.activeElement && document.activeElement.id,
                          stored: snap.storedRomAvailable};
                })()""",
                lambda item: isinstance(item, dict)
                and item.get("hidden") is False
                and item.get("success") is True
                and item.get("retryHidden") is True
                and item.get("activeId") == "canvas"
                and item.get("stored") is True
                and "saved to browser storage" in item.get("text", ""),
                "public ROM durability retry succeeds visibly",
                args.timeout,
            )
            wait_value(
                cdp,
                """(() => {
                  const banner = document.getElementById("rom-session-banner");
                  const message = document.getElementById("rom-session-message");
                  const snap = globalThis.__mdkrTestSnapshot();
                  return {hidden: banner.hidden, text: message.textContent,
                          stored: snap.storedRomAvailable};
                })()""",
                lambda item: isinstance(item, dict)
                and item.get("hidden") is True
                and item.get("text") == ""
                and item.get("stored") is True,
                "ROM durability warning clears after a successful retry",
                args.timeout,
            )
            # Both waits above already require storedRomAvailable, so asserting
            # it again proved nothing. What is not yet proven is that the shell
            # flag corresponds to a durable object: read the store itself.
            recovered_rom_files = cdp.evaluate(
                'idbCount("/rom")', await_promise=True, timeout=args.timeout
            )
            require(isinstance(recovered_rom_files, int)
                    and recovered_rom_files > 0,
                    "public ROM Retry reported success while IndexedDB held "
                    f"{recovered_rom_files!r} ROM objects")
            cdp.call("Emulation.clearDeviceMetricsOverride")
            cdp.call(
                "Page.removeScriptToEvaluateOnNewDocument",
                {"identifier": rom_warning_preload},
            )

            failure_config = {
                "headlessFrames": 120,
                "env": {
                    "MDKR_WEBGPU_FAULT": "bringup.adapter",
                },
            }
            failure_preload = add_config_script(cdp, failure_config)
            cdp.call("Page.reload", {"ignoreCache": True})
            wait_launcher(cdp, args.timeout)
            click_play(cdp)
            graphics_failure = wait_value(
                cdp,
                """(() => {
                  const snap = globalThis.__mdkrTestSnapshot &&
                    globalThis.__mdkrTestSnapshot();
                  return {
                    phase: snap && snap.phase,
                    shutdownComplete: snap && snap.shutdownComplete,
                    text: document.getElementById("gate-msg").textContent,
                    gate: !document.getElementById("gate").hidden,
                    stage: !document.getElementById("stage").hidden,
                    playDisabled: document.getElementById("play").disabled,
                    saveDisabled:
                      document.getElementById("clear-save").disabled,
                    activeId: document.activeElement && document.activeElement.id
                  };
                })()""",
                lambda item: isinstance(item, dict)
                and item.get("phase") == "graphics-failed"
                and item.get("shutdownComplete") is True
                and item.get("saveDisabled") is False,
                "actionable WebGPU failure panel",
                args.timeout,
            )
            require(
                graphics_failure.get("gate") is True
                and graphics_failure.get("stage") is False
                and graphics_failure.get("playDisabled") is True
                and graphics_failure.get("saveDisabled") is False
                and graphics_failure.get("activeId") == "gate-msg"
                and "no usable GPU device" in graphics_failure.get("text", "")
                and "can't render" in graphics_failure.get("text", ""),
                f"WebGPU failure did not produce a stable recovery UI: "
                f"{graphics_failure}",
            )

            # Browser-only async pipeline creation is a required-material path,
            # not an acceptable permanent missing-geometry mode. Force its
            # callback failure and require the same stable recovery panel.
            cdp.call(
                "Page.removeScriptToEvaluateOnNewDocument",
                {"identifier": failure_preload},
            )
            async_console_start = len(cdp.console)
            add_config_script(
                cdp,
                {
                    "headlessFrames": 120,
                    "env": {
                        "MDKR_WEBGPU_FAULT": "shader.pipeline-async",
                    },
                },
            )
            cdp.call("Page.reload", {"ignoreCache": True})
            wait_launcher(cdp, args.timeout)
            click_play(cdp)
            async_failure = wait_value(
                cdp,
                """(() => {
                  const snap = globalThis.__mdkrTestSnapshot &&
                    globalThis.__mdkrTestSnapshot();
                  return {
                    phase: snap && snap.phase,
                    shutdownComplete: snap && snap.shutdownComplete,
                    text: document.getElementById("gate-msg").textContent,
                    gate: !document.getElementById("gate").hidden,
                    stage: !document.getElementById("stage").hidden,
                    saveDisabled:
                      document.getElementById("clear-save").disabled
                  };
                })()""",
                lambda item: isinstance(item, dict)
                and item.get("phase") == "graphics-failed"
                and item.get("shutdownComplete") is True
                and item.get("saveDisabled") is False,
                "async-pipeline recovery panel",
                args.timeout,
            )
            async_console = cdp.console[async_console_start:]
            require(
                async_failure.get("gate") is True
                and async_failure.get("stage") is False
                and "pipeline" in async_failure.get("text", "").lower()
                and any(
                    "[webgpu-fault] injected shader.pipeline-async@1" in line
                    for line in async_console
                )
                and any(
                    "async pipeline create failed" in line
                    for line in async_console
                ),
                "async pipeline failure did not stop missing geometry "
                "transactionally: "
                f"panel={async_failure}\n"
                + "\n".join(async_console[-60:]),
            )

            # Recovery path 1: save wipe must leave the ROM alone.
            cdp.evaluate(
                """(() => {
                  window.confirm = () => true;
                  document.getElementById("clear-save").click();
                  return true;
                })()"""
            )
            clear_status = wait_value(
                cdp,
                'document.getElementById("save-status").textContent',
                lambda item: isinstance(item, str)
                and "erased" in item.lower(),
                "saved-progress erase",
                args.timeout,
            )
            clear_counts = cdp.evaluate(
                """Promise.all([idbCount("/save"), idbCount("/rom")])
                   .then(([save, rom]) => ({save, rom}))""",
                await_promise=True,
                timeout=args.timeout,
            )
            require(
                clear_counts.get("save") == 1 and clear_counts.get("rom", 0) > 0,
                "save wipe touched the wrong store: "
                f"status={clear_status!r} counts={clear_counts}",
            )

            # Recovery path 2: forgetting the ROM must empty the remaining store.
            cdp.evaluate('document.getElementById("forget").click()')
            # wait_value already fails unless the dialog reports open, so the
            # assertion that used to follow could not reject anything.
            wait_value(
                cdp,
                'document.getElementById("forget-rom-dialog").open',
                lambda item: item is True,
                "stored-ROM confirmation",
                args.timeout,
            )
            cdp.evaluate('document.getElementById("forget-rom-confirm").click()')
            forget_status = wait_value(
                cdp,
                'document.getElementById("rom-status").textContent',
                lambda item: isinstance(item, str)
                and "forgotten" in item.lower(),
                "stored-ROM erase",
                args.timeout,
            )
            forget_counts = cdp.evaluate(
                """Promise.all([idbCount("/save"), idbCount("/rom")])
                   .then(([save, rom]) => ({save, rom}))""",
                await_promise=True,
                timeout=args.timeout,
            )
            forget_state = cdp.evaluate(
                "globalThis.__mdkrTestSnapshot().storedRomAvailable"
            )
            require(
                forget_counts.get("save") == 1
                and forget_counts.get("rom") == 0
                and forget_state is False,
                "forget control left browser files behind: "
                f"status={forget_status!r} counts={forget_counts} "
                f"stored={forget_state!r}",
            )

            requests = list(cdp.network)
            net_problems = network_problems(requests, server.origin, rom.name)
            require(not net_problems, "network privacy failure: " + "; ".join(net_problems))
            urls = [str(item.get("url", "")) for item in requests]
            require(
                any(url.endswith("/mdkr64_web.js") for url in urls)
                and any(url.endswith("/mdkr64_web.wasm") for url in urls),
                "browser did not request both linked engine artifacts",
            )
            require(
                all(
                    request.method in {"GET", "HEAD"}
                    and request.content_length in {0, -1}
                    for request in server.requests
                ),
                f"HTTP server received a request body: {server.requests}",
            )
            require(not cdp.failures, "browser/CDP failures: " + "; ".join(cdp.failures))
            # Emscripten's noExitRuntime path implements a deliberate C exit by
            # throwing ExitStatus after publishing __mdkrExitCode. DevTools
            # reports that control-flow exception even though callMain handles
            # it. The six finite exit(0) runs and handled graphics-failure
            # stop above can therefore contribute at most seven benign
            # ExitStatus rows; every other exception remains a failure.
            exit_statuses = [
                item
                for item in cdp.exceptions
                if item.strip() == "ExitStatus"
                or "Program terminated with exit(0)" in item
            ]
            serious_exceptions = [
                item
                for item in cdp.exceptions
                if "ResizeObserver loop" not in item
                and item not in exit_statuses
            ]
            require(
                len(exit_statuses) <= 7,
                f"unexpected extra Emscripten exit exceptions: {exit_statuses}",
            )
            require(
                not serious_exceptions,
                "unhandled browser exceptions: " + "; ".join(serious_exceptions),
            )

            if args.keep_screens:
                destination = resolve_path(args.keep_screens)
                destination.mkdir(parents=True, exist_ok=True)
                for frame, png, _stats in screens:
                    (destination / f"browser_{frame:04d}.png").write_bytes(png)

            print(
                "check_browser_runtime: PASS — "
                f"{args.frames} wasm/WebGPU host opportunities, "
                f"{int(surface_frames)} canvas updates, median {fps:.1f} fps "
                f"({stable_ratio:.1%} R=2, no sub-two-field updates), "
                f"p95/p99/max "
                f"{p95_dt:.2f}/{p99_dt:.2f}/{max_dt:.2f} ms "
                f"(max frame {max_dt_frame}), "
                f"pipelines {async_creates} ready with {hold_frames} held "
                f"frames (max streak {max_hold_streak}, compile "
                f"{max_pipeline_frames} frames), {len(race_rows)} in-race rows; "
                f"world shadows {world_shadow_rows[0][1]}/"
                f"{world_shadow_rows[0][0]} complete with "
                f"{world_shadow_rows[0][2]} fallback frames; "
                f"{visual_summary}; exact EEPROM+ROM reload; "
                f"wasm rollback {rollback_summary['ticks']} ticks/"
                f"{rollback_summary['snapshot']} B snapshot/"
                f"p99 {rollback_summary['resim_p99']} ns resim; "
                f"AudioWorklet posted {audio['posted']} frames; RAW16 fixed "
                f"{raw16_rows[0][1]} loads/{raw16_rows[0][2]} bytes at first "
                f"active block; font SDF {font_rows[0][0]} + outline "
                f"{font_rows[0][1]} uploads; "
                f"mips {mip_rows[0][0]}/{mip_rows[0][1]} levels; "
                f"lighting {light_rows[0][4]}/{light_rows[0][5]} "
                "racer/character tris; event queues "
                + ", ".join(
                    f"{peak}/{capacity}"
                    for _pointer, peak, capacity in sorted(
                        event_queues.values(), key=lambda item: item[2]
                    )
                )
                + "; save/ROM "
                "recovery, all three browser-only WebGPU fault points, "
                "in-game video config mutation/reload, event-drop positive "
                "control, and zero-upload network audit "
                "passed",
                flush=True,
            )
        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()
            server.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-dir", default="build-web")
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--rom", default=DEFAULT_ROM)
    parser.add_argument("--input-script", default=str(DEFAULT_SCRIPT))
    parser.add_argument("--chrome")
    parser.add_argument(
        "--chrome-flag",
        action="append",
        default=[],
        help="extra Chromium flag (repeatable)",
    )
    parser.add_argument("--frames", type=int, default=3600)
    parser.add_argument("--reload-frames", type=int, default=180)
    parser.add_argument(
        "--camera-obstruction",
        choices=("observe", "legacy", "center-ray", "modern"),
        help="set and geometrically validate the first browser run's camera policy",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=150.0,
        help="seconds allowed for each browser milestone",
    )
    parser.add_argument("--keep-screens")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run_check(args)
        return 0
    except (CheckFailure, OSError, ValueError, zlib.error) as exc:
        print(f"check_browser_runtime: FAIL — {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
