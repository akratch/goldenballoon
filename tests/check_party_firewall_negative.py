#!/usr/bin/env python3
"""Block the Party socket for real and assert the documented fail-neutral copy.

Firewall-negative observation, service/browser half. A real local Party Worker
is started, the identical create/join calls the product makes are proven to
succeed against it, and then the listener is killed so every subsequent socket
is refused at connect() — the same failure a blocking firewall produces. The
gate then asserts the shipped player-facing copy, verbatim, in three places:

  * an established room whose socket dies mid-session announces that it is
    reconnecting and stops after exactly five bounded attempts with the paused
    sentence, rather than retrying forever,
  * a display that tries to open a room against the blocked service shows the
    connection sentence, an enabled Try again, and the standing promise that
    keyboard, gamepads and touch controls still work offline,
  * a phone entering the room code recovers truthfully under both shapes a
    firewall actually takes — REJECT, where connect() is refused and the code
    view keeps its inline "could not reach" recovery, and DROP, where the
    connection is accepted and blackholed until the bounded request timeout
    turns it into the typed service_unavailable card.

Local play staying available is asserted rather than assumed: the launcher's
Choose ROM route is still present and enabled behind the failed dialog.

The native half of this exit item is tests/test_party_firewall_negative.cpp.

Headless only. Fresh port and temp directory per run.
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

from check_browser_online_two_person import (
    SERVICE, WRANGLER, free_port, node_binary, wait_worker,
)
from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, find_chrome, page_websocket,
    require, wait_value,
)

ROOT = Path(__file__).resolve().parent.parent

# The shipped strings. Typographic apostrophes are deliberate: these are copied
# from dist/web, not paraphrased.
DISPLAY_OPEN_FAILURE = "Check this display’s internet connection and try again."
DISPLAY_OFFLINE_PROMISE = (
    "Keyboard, gamepads and this screen’s touch controls still work offline.")
DISPLAY_RECONNECTING = (
    "Controller room reconnecting… Approved phone controls remain direct.")
DISPLAY_RECONNECT_PAUSED = (
    "The controller-room connection is paused. Approved phones may keep "
    "working directly; new pairings wait until you try again.")
DISPLAY_PAUSED_ANNOUNCE = (
    "Controller room paused after five reconnect attempts. Try again when ready.")
CONTROLLER_ERROR_TITLE = "Pairing unavailable"
CONTROLLER_ERROR_MESSAGE = (
    "The controller service could not be reached. Check the internet connection "
    "or play with the display’s keyboard or gamepads.")
CONTROLLER_REJECT_COPY = (
    "Could not reach the controller room. Check your connection and try again.")

SENTINEL = "__sentinel_not_written_by_the_product__"


class Blackhole:
    """A listener that accepts and never answers: the DROP-rule firewall.

    Rebinding the port the Worker just vacated is what turns a refused socket
    into a hung one, which is the failure that must resolve into the product's
    bounded request timeout rather than a spinner that never ends.
    """

    def __init__(self, port: int):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", port))
        self.listener.listen(16)
        self.listener.settimeout(0.25)
        self.accepted: list[socket.socket] = []
        self.running = True
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self) -> None:
        while self.running:
            try:
                connection, _ = self.listener.accept()
            except (socket.timeout, OSError):
                continue
            self.accepted.append(connection)

    def close(self) -> None:
        self.running = False
        self.thread.join(timeout=5)
        for connection in self.accepted:
            try:
                connection.close()
            except OSError:
                pass
        self.listener.close()


def wait_socket_accepts(port: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.05)
    raise CheckFailure(f"blackhole listener never accepted on port {port}")


def worker_command(origin: str, shell: Path, state: Path) -> list[str]:
    return [str(node_binary()), str(WRANGLER), "dev", "--local",
            "--ip", "127.0.0.1", "--port", origin.rsplit(":", 1)[1],
            "--inspector-port", "0", "--assets", str(shell),
            "--persist-to", str(state),
            "--var", f"PARTY_ORIGIN:{origin}",
            "--var", "PARTY_HMAC_KEY:firewall-fixture-32-byte-secret!!",
            "--var", "MAX_ADMISSIONS_PER_DAY:5000",
            "--var", "CONTROL_RESERVE_PER_DAY:5000",
            "--log-level", "error"]


def start_worker(origin: str, shell: Path, state: Path,
                 log: Any) -> subprocess.Popen[bytes]:
    process = subprocess.Popen(worker_command(origin, shell, state),
                               cwd=SERVICE, stdout=log, stderr=subprocess.STDOUT)
    wait_worker(origin, process, 60)
    return process


def stop_worker(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=10)


def create_room(origin: str) -> Any:
    body = json.dumps({"hostPublicKey": "H" * 87}).encode("utf-8")
    item = urllib.request.Request(
        origin + "/api/party/create", data=body,
        headers={"Origin": origin, "Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(item, timeout=15) as response:
        require(response.status == 201,
                f"baseline party create did not succeed: {response.status}")
        return json.loads(response.read().decode("utf-8"))


def wait_socket_refused(port: int, timeout: float) -> None:
    """Poll until the port genuinely refuses connect(): the firewall condition."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                pass
        except (ConnectionRefusedError, socket.timeout, OSError):
            return
        time.sleep(0.05)
    raise CheckFailure(
        f"port {port} still accepted connections after the Worker was killed")


def blocked_create_refuses(origin: str) -> None:
    """The same call that returned 201 above must now fail at the socket."""
    body = json.dumps({"hostPublicKey": "H" * 87}).encode("utf-8")
    item = urllib.request.Request(
        origin + "/api/party/create", data=body,
        headers={"Origin": origin, "Content-Type": "application/json"},
        method="POST")
    try:
        with urllib.request.urlopen(item, timeout=5) as response:
            raise CheckFailure(
                "party create still answered after the listener was killed: "
                f"{response.status}")
    except urllib.error.URLError as error:
        require(isinstance(error.reason, OSError),
                f"blocked create failed for the wrong reason: {error.reason}")


def page(chrome_path: str, profile: Path, flags: list[str],
         verbose: bool) -> tuple[ChromeProcess, CDPClient]:
    process = ChromeProcess(chrome_path, profile, flags, verbose)
    cdp = CDPClient(page_websocket(process.wait_port()))
    for domain in ("Page", "Runtime", "Log", "Inspector", "Accessibility"):
        cdp.call(f"{domain}.enable")
    return process, cdp


def sentinel_party_error(cdp: CDPClient) -> None:
    """The dialog's default markup already contains the failure sentence, so
    overwrite it first: the assertion must prove the product wrote it."""
    cdp.evaluate(
        f"document.getElementById('party-error-message').textContent="
        f"{json.dumps(SENTINEL)}")


def submit_code(cdp: CDPClient, code: str) -> None:
    cdp.evaluate("""(() => {
      const field = document.getElementById('room-code');
      field.value = %s;
      field.dispatchEvent(new Event('input', {bubbles: true}));
      document.getElementById('code-error').textContent = %s;
      document.getElementById('code-form').dispatchEvent(
        new Event('submit', {bubbles: true, cancelable: true}));
    })()""" % (json.dumps(code), json.dumps(SENTINEL)))


def run(args: argparse.Namespace) -> None:
    shell = (ROOT / args.shell_dir).resolve()
    require((shell / "index.html").is_file(), "missing web shell")
    require((shell / "controller/index.html").is_file(),
            "missing controller page")
    require(WRANGLER.is_file(), "missing lockfile-pinned Wrangler")
    chrome_path = find_chrome(args.chrome)

    with tempfile.TemporaryDirectory(prefix="mdkr-party-firewall-") as temporary:
        root = Path(temporary)
        port = free_port()
        origin = f"http://127.0.0.1:{port}"
        worker: subprocess.Popen[bytes] | None = None
        clients: list[tuple[ChromeProcess, CDPClient]] = []
        with (root / "wrangler.log").open("wb") as log:
            try:
                worker = start_worker(origin, shell, root / "state", log)

                # Baseline: the exact endpoint the product calls works here.
                # Without this the later failure could be a broken fixture.
                room = create_room(origin)
                require(len(str(room.get("fallbackCode", ""))) == 6,
                        f"baseline room had no usable code: {room}")

                established_process, established = page(
                    chrome_path, root / "display-established", args.chrome_flag,
                    args.verbose)
                clients.append((established_process, established))
                blocked_process, blocked = page(
                    chrome_path, root / "display-blocked", args.chrome_flag,
                    args.verbose)
                clients.append((blocked_process, blocked))
                phone_process, phone = page(
                    chrome_path, root / "phone", args.chrome_flag, args.verbose)
                clients.append((phone_process, phone))

                for client in (established, blocked):
                    client.call("Page.navigate", {"url": origin + "/"})
                    wait_value(client, "Boolean(globalThis.MDKRPartyHost) && "
                               "document.readyState==='complete'", bool,
                               "party display shell", args.timeout)
                phone.call("Page.navigate", {"url": origin + "/controller/"})
                wait_value(phone, "!document.getElementById('state-code').hidden",
                           bool, "controller code entry", args.timeout)

                # Second baseline, in the browser: opening a room succeeds
                # while the socket is open.
                established.evaluate(
                    "globalThis.MDKRPartyHost.setRomReady(true);"
                    "globalThis.MDKRPartyHost.open()")
                wait_value(established,
                           "!document.getElementById('party-room').hidden", bool,
                           "party room opened before the block", args.timeout)
                live_code = established.evaluate(
                    "document.getElementById('party-code').textContent")
                require(any(character.isdigit() for character in str(live_code)),
                        f"party room opened without a code: {live_code}")

                # The firewall drops in: kill the listener.
                sentinel_party_error(established)
                sentinel_party_error(blocked)
                phone.evaluate(
                    "document.getElementById('error-message').textContent="
                    + json.dumps(SENTINEL))
                stop_worker(worker)
                worker = None
                wait_socket_refused(port, 15)
                blocked_create_refuses(origin)

                # 1. Established room, socket killed mid-session: it announces
                # the reconnect, then stops after five bounded attempts.
                wait_value(established,
                           "document.getElementById('party-status').textContent",
                           lambda value: value == DISPLAY_RECONNECTING,
                           "mid-session reconnect announcement", args.timeout)
                wait_value(established,
                           "document.getElementById('party-error-message').textContent",
                           lambda value: value == DISPLAY_RECONNECT_PAUSED,
                           "bounded reconnect attempts", max(args.timeout, 45.0))
                paused = established.evaluate("""(() => ({
                  errorShown: !document.getElementById('party-error').hidden,
                  roomHidden: document.getElementById('party-room').hidden,
                  status: document.getElementById('party-status').textContent,
                  retryEnabled: !document.getElementById('party-retry').disabled,
                  retryFocused: document.activeElement.id === 'party-retry',
                  offline: [...document.querySelectorAll('#party-error p')]
                    .map(node => node.textContent)
                }))()""")
                require(paused["errorShown"] and paused["retryEnabled"] and
                        paused["retryFocused"] and
                        paused["status"] == DISPLAY_PAUSED_ANNOUNCE and
                        DISPLAY_OFFLINE_PROMISE in paused["offline"],
                        f"paused reconnect state was not truthful: {paused}")

                # 2. A display opening a room against the blocked service.
                blocked.evaluate(
                    "globalThis.MDKRPartyHost.setRomReady(true);"
                    "globalThis.MDKRPartyHost.open()")
                wait_value(blocked,
                           "document.getElementById('party-error-message').textContent",
                           lambda value: value == DISPLAY_OPEN_FAILURE,
                           "blocked room-open copy", args.timeout)
                refusal = blocked.evaluate("""(() => ({
                  errorShown: !document.getElementById('party-error').hidden,
                  heading: document.querySelector('#party-error h3').textContent,
                  retryEnabled: !document.getElementById('party-retry').disabled,
                  retryFocused: document.activeElement.id === 'party-retry',
                  offline: [...document.querySelectorAll('#party-error p')]
                    .map(node => node.textContent),
                  closeEnabled: !document.getElementById('party-close').disabled,
                  localPlay: Boolean(document.getElementById('drop')) &&
                    !document.getElementById('drop').disabled
                }))()""")
                require(refusal["errorShown"] and
                        refusal["heading"] == "Phone pairing is unavailable" and
                        refusal["retryEnabled"] and refusal["retryFocused"] and
                        DISPLAY_OFFLINE_PROMISE in refusal["offline"],
                        f"blocked room-open recovery was not truthful: {refusal}")
                require(refusal["closeEnabled"] and refusal["localPlay"],
                        "local play was not reachable behind the blocked party "
                        f"dialog: {refusal}")

                # 3. A phone entering the room code while connect() is refused.
                # A REJECT rule surfaces as a transport-level fetch failure, so
                # the product keeps the code view and offers the inline retry.
                submit_code(phone, str(room["fallbackCode"]))
                wait_value(phone,
                           "document.getElementById('code-error').textContent",
                           lambda value: value == CONTROLLER_REJECT_COPY,
                           "refused-socket code recovery", args.timeout)
                rejected = phone.evaluate("""(() => ({
                  codeShown: !document.getElementById('state-code').hidden,
                  errorShown: !document.getElementById('code-error').hidden,
                  invalid: document.getElementById('room-code')
                    .getAttribute('aria-invalid'),
                  joinEnabled: !document.getElementById('join-code').disabled
                }))()""")
                require(rejected["codeShown"] and rejected["errorShown"] and
                        rejected["invalid"] == "true" and rejected["joinEnabled"],
                        f"refused-socket code recovery was not truthful: {rejected}")

                # 4. The other firewall shape: the connection is accepted and
                # then blackholed. The bounded request timeout must turn that
                # into the typed service_unavailable card rather than a
                # spinner that never resolves.
                blackhole = Blackhole(port)
                try:
                    wait_socket_accepts(port, 10)
                    submit_code(phone, str(room["fallbackCode"]))
                    wait_value(
                        phone,
                        "document.getElementById('error-message').textContent",
                        lambda value: value == CONTROLLER_ERROR_MESSAGE,
                        "typed service_unavailable recovery",
                        max(args.timeout, 45.0))
                    recovery = phone.evaluate("""(() => ({
                      errorShown: !document.getElementById('state-error').hidden,
                      title: document.getElementById('error-title').textContent,
                      recovery:
                        document.getElementById('error-recovery').textContent,
                      recoveryEnabled:
                        !document.getElementById('error-recovery').disabled,
                      mark: document.getElementById('connection-mark')
                        .getAttribute('aria-label')
                    }))()""")
                    require(recovery["errorShown"] and
                            recovery["title"] == CONTROLLER_ERROR_TITLE and
                            recovery["recovery"] == "Enter another code" and
                            recovery["recoveryEnabled"] and
                            recovery["mark"] == "Not connected",
                            f"blackholed recovery state was not truthful: {recovery}")
                finally:
                    blackhole.close()

                for name, client in (("established display", established),
                                     ("blocked display", blocked),
                                     ("phone", phone)):
                    require(not client.failures,
                            f"{name} reported failures: "
                            + "; ".join(client.failures))
            finally:
                for process, client in clients:
                    client.close()
                    process.close()
                stop_worker(worker)

    print("check_party_firewall_negative: PASS — a killed listener refuses every "
          "socket and a rebound blackhole hangs them; the established room "
          "announces its reconnect and stops after five bounded attempts, a "
          "blocked room-open, a refused code entry and a blackholed code entry "
          "all land in the shipped recovery copy verbatim, and local play stays "
          "reachable throughout")


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
        print(f"check_party_firewall_negative: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
