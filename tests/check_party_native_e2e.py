#!/usr/bin/env python3
"""End-to-end Phone Party: native host driver, real local Worker, real phone page.

Every other Phone Party gate stubs at least one leg of the triangle. This one
runs all three for real: the packaged native host model with its production
libdatachannel transport (built as ``mdkr_native_party_e2e_driver``), a live
local Party Worker (``wrangler dev --local`` with real Durable Objects), and
the shipped ``/controller/`` page in headless Chromium. The native transport
speaks plain ``ws://`` to the loopback Worker under the internal test token
(``MDKR_INTERNAL_TEST_TOKEN=mdkr64-party-e2e-v1``); without that token the
driver's fail-closed HTTPS-only posture is unchanged, which
``mdkr_native_party_bringup_test`` pins separately.

Three scenarios, each with its own PASS line:

* ``golden_path`` — create room, phone redeems the minted capability, the
  pending controller (which the real Worker reports with ``"seat": null``)
  becomes visible to the driver BEFORE approval, the driver's ECDH pairing
  phrase matches the phrase the page renders, approval leads to Connected,
  and 50 non-neutral pad packets cross the real ingress.
* ``offer_drop`` — the FIRST ``webrtc_offer`` frame Worker->phone is swallowed
  in the page; the native signaling retry deadline recreates the peer and the
  driver still reaches Connected in under 60 seconds, then input flows.
* ``stall`` — SIGSTOP the driver for 3 seconds mid-stream; on SIGCONT the
  controller must be Connected and fresh non-neutral packets must cross the
  ingress within 5 seconds (the transport event queue plus the host custody
  self-heal, through the real stack).
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Callable

from check_browser_online_two_person import free_port
from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, find_chrome, page_websocket,
    require, wait_value,
)
from check_party_capacity import start_worker, stop_worker

ROOT = Path(__file__).resolve().parent.parent
TEST_TOKEN = "mdkr64-party-e2e-v1"

INVITE = re.compile(r"^\[E2E\] invite url=(\S+) code=(\d{6})$")
CONTROLLER = re.compile(r"^\[E2E\] controller=(\S+) phase=(\S+) seat=(\d+)$")
PHRASE = re.compile(r"^\[E2E\] controller=(\S+) phrase=(.+)$")
APPROVE = re.compile(r"^\[E2E\] approve controller=(\S+) seat=(\d+)$")
NONNEUTRAL = re.compile(r"^\[E2E\] nonneutral=(\d+) packets=(\d+)$")
RESULT_OK = re.compile(r"^\[E2E\] result=ok nonneutral=(\d+) packets=(\d+)$")

# Hold non-neutral input on the shared touch surface: a keyboard stick hold
# (the page's accessibility path) keeps stickY deflected on every heartbeat
# packet until keyup, and one accessibility Go press adds the A-button pulse.
HOLD_GO = """(() => {
  document.getElementById('phone-touch-stick').dispatchEvent(
    new KeyboardEvent('keydown',{key:'ArrowUp',bubbles:true,cancelable:true}));
  document.querySelector('.touch-go').click();
})()"""

# Swallows exactly the first webrtc_offer frame the Worker relays to the
# phone, before the page installs its own socket listeners. Everything else
# — controller_state, webrtc_ice, the retried offer — passes untouched.
DROP_FIRST_OFFER = """(() => {
  const NativeWebSocket = globalThis.WebSocket;
  globalThis.__mdkrE2EDroppedOffers = 0;
  class FilteredWebSocket extends NativeWebSocket {
    addEventListener(type, listener, options) {
      if (type !== "message" || typeof listener !== "function") {
        return super.addEventListener(type, listener, options);
      }
      const wrapped = (event) => {
        if (globalThis.__mdkrE2EDroppedOffers === 0 &&
            typeof event.data === "string") {
          try {
            if (JSON.parse(event.data).type === "webrtc_offer") {
              globalThis.__mdkrE2EDroppedOffers = 1;
              return undefined;
            }
          } catch (_) {}
        }
        return listener.call(this, event);
      };
      return super.addEventListener(type, wrapped, options);
    }
  }
  globalThis.WebSocket = FilteredWebSocket;
})();"""


def resolve(value: str) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else ROOT / path).resolve()


class Driver:
    """The native host driver as a narrated child process."""

    def __init__(self, binary: Path, origin: str, packets: int,
                 timeout_ms: int, verbose: bool):
        environment = dict(os.environ)
        environment["MDKR_INTERNAL_TEST_TOKEN"] = TEST_TOKEN
        self.proc = subprocess.Popen(
            [str(binary), "--origin", origin, "--auto-approve",
             "--packets", str(packets), "--timeout-ms", str(timeout_ms)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=environment, text=True, bufsize=1)
        self.verbose = verbose
        self.lines: list[str] = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if self.verbose:
                print(f"driver: {line}", flush=True)
            with self.lock:
                self.lines.append(line)

    def snapshot(self) -> list[str]:
        with self.lock:
            return list(self.lines)

    def wait_line(self, predicate: Callable[[str], object], description: str,
                  timeout: float) -> tuple[int, object]:
        """Return (index, predicate value) of the first matching line."""
        deadline = time.monotonic() + timeout
        scanned = 0
        while time.monotonic() < deadline:
            lines = self.snapshot()
            for index in range(scanned, len(lines)):
                value = predicate(lines[index])
                if value:
                    return index, value
            scanned = len(lines)
            if self.proc.poll() is not None and scanned == len(self.snapshot()):
                break
            time.sleep(0.05)
        raise CheckFailure(
            f"driver never printed {description}; last lines: "
            f"{self.snapshot()[-8:]}")

    def wait_exit(self, timeout: float) -> int:
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise CheckFailure(
                f"driver did not exit; last lines: {self.snapshot()[-8:]}"
            ) from error

    def close(self) -> None:
        if self.proc.poll() is None:
            # A scenario abort can leave the child SIGSTOPped; resume it so
            # terminate() can be honored, then escalate if it is wedged.
            try:
                self.proc.send_signal(signal.SIGCONT)
            except OSError:
                pass
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        if self.proc.stdout is not None:
            self.proc.stdout.close()


def latest_phase(lines: list[str], controller_id: str) -> str:
    phase = ""
    for line in lines:
        matched = CONTROLLER.match(line)
        if matched and matched.group(1) == controller_id:
            phase = matched.group(2)
    return phase


def latest_nonneutral(lines: list[str]) -> int:
    value = 0
    for line in lines:
        matched = NONNEUTRAL.match(line) or RESULT_OK.match(line)
        if matched:
            value = int(matched.group(1))
    return value


def phone_browser(chrome_path: str, profile: Path, flags: list[str],
                  verbose: bool,
                  inject: str | None = None) -> tuple[ChromeProcess, CDPClient]:
    process = ChromeProcess(chrome_path, profile, flags, verbose)
    cdp = CDPClient(page_websocket(process.wait_port()))
    for domain in ("Page", "Runtime", "Log", "Inspector"):
        cdp.call(f"{domain}.enable")
    if inject:
        cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": inject})
    return process, cdp


def wait_pending(driver: Driver, timeout: float) -> tuple[int, str]:
    """The pending controller line, which must precede any approval: with the
    real Worker a pending entry arrives inside room_state with "seat": null,
    so this is the seatless-entry parse guard proven through the true stack.
    """
    index, matched = driver.wait_line(
        lambda line: (value := CONTROLLER.match(line)) and
        value.group(2) == "pending" and value,
        "controller phase=pending", timeout)
    require(matched.group(3) == "0",
            f"pending controller carried a seat: {matched.group(0)}")
    lines = driver.snapshot()
    for line in lines[:index]:
        require(not APPROVE.match(line) and not (
            (value := CONTROLLER.match(line)) and value.group(2) != "pending"),
            f"driver acted on the controller before pending was visible: {line}")
    return index, matched.group(1)


def match_phrases(driver: Driver, phone: CDPClient, controller_id: str,
                  timeout: float) -> None:
    _, matched = driver.wait_line(
        lambda line: (value := PHRASE.match(line)) and
        value.group(1) == controller_id and value,
        "controller pairing phrase", timeout)
    driver_phrase = matched.group(2).strip()
    page_phrase = wait_value(
        phone, "document.getElementById('pairing-phrase').textContent",
        lambda value: isinstance(value, str) and value.strip() != "" and
        value.strip() != "Bright Balloon",
        "rendered pairing phrase", timeout)
    require(page_phrase.strip() == driver_phrase,
            f"pairing phrases diverge: driver={driver_phrase!r} "
            f"page={page_phrase.strip()!r}")


def wait_connected(driver: Driver, controller_id: str,
                   timeout: float) -> int:
    index, _ = driver.wait_line(
        lambda line: (value := CONTROLLER.match(line)) and
        value.group(1) == controller_id and value.group(2) == "connected" and
        value,
        "controller phase=connected", timeout)
    return index


def activate_controls(phone: CDPClient, timeout: float) -> None:
    wait_value(phone, "!document.getElementById('state-assigned').hidden",
               bool, "assigned phone", timeout)
    # A press before the direct control channel has opened is deliberately a
    # no-op on the page (a person simply presses again), so keep pressing
    # until the round trip unlocks the button.
    deadline = time.monotonic() + timeout
    while True:
        phone.evaluate("document.getElementById('input-test').click()")
        if phone.evaluate("!document.getElementById('use-controller').disabled"):
            break
        if time.monotonic() >= deadline:
            raise CheckFailure("input-test round trip never unlocked "
                               "the use-controller button")
        time.sleep(0.25)
    phone.evaluate("document.getElementById('use-controller').click()")
    wait_value(phone, "!document.getElementById('state-controller').hidden",
               bool, "active controller surface", timeout)
    phone.evaluate(HOLD_GO)


def parse_invite(driver: Driver, origin: str, timeout: float) -> str:
    _, matched = driver.wait_line(
        lambda line: INVITE.match(line), "invite url/code", timeout)
    url = matched.group(1)
    require(url.startswith(origin + "/controller/#") and
            len(url.rsplit("#", 1)[1]) == 43,
            f"invite URL is not a capability on the local Worker: {url}")
    return url


def close_all(driver: Driver | None, phone: CDPClient | None,
              phone_process: ChromeProcess | None) -> None:
    for closable in (phone, phone_process, driver):
        if closable is None:
            continue
        try:
            closable.close()
        except OSError:
            pass


def scenario_golden_path(origin: str, binary: Path, chrome_path: str,
                         base: Path, flags: list[str], verbose: bool,
                         timeout: float) -> None:
    driver = phone = phone_process = None
    try:
        driver = Driver(binary, origin, packets=50, timeout_ms=90_000,
                        verbose=verbose)
        url = parse_invite(driver, origin, timeout)
        phone_process, phone = phone_browser(
            chrome_path, base / "phone", flags, verbose)
        phone.call("Page.navigate", {"url": url})
        # The driver auto-approves as soon as the pending entry lands, so the
        # page's waiting card can be approved past within one poll interval;
        # the driver's own line ordering below is the pending-first proof.
        wait_value(phone,
                   "!document.getElementById('state-waiting').hidden || "
                   "!document.getElementById('state-assigned').hidden",
                   bool, "redeemed phone", timeout)
        pending_index, controller_id = wait_pending(driver, timeout)
        match_phrases(driver, phone, controller_id, timeout)
        connected_index = wait_connected(driver, controller_id, timeout)
        lines = driver.snapshot()
        approve_indices = [index for index, line in enumerate(lines)
                           if APPROVE.match(line)]
        require(bool(approve_indices), "driver never echoed an approval")
        approve_index = approve_indices[0]
        require(pending_index < approve_index < connected_index,
                "pending -> approve -> connected did not happen in order: "
                f"{pending_index}/{approve_index}/{connected_index}")
        activate_controls(phone, timeout)
        driver.wait_line(RESULT_OK.match, "result=ok", timeout)
        require(driver.wait_exit(10) == 0, "driver exited non-zero")
    finally:
        close_all(driver, phone, phone_process)
    print("scenario golden_path: PASS — real Worker invite, seatless pending "
          "entry visible before approval, matching ECDH phrases, Connected, "
          "50 non-neutral packets through the real ingress", flush=True)


def scenario_offer_drop(origin: str, binary: Path, chrome_path: str,
                        base: Path, flags: list[str], verbose: bool,
                        timeout: float) -> None:
    driver = phone = phone_process = None
    try:
        driver = Driver(binary, origin, packets=50, timeout_ms=110_000,
                        verbose=verbose)
        url = parse_invite(driver, origin, timeout)
        phone_process, phone = phone_browser(
            chrome_path, base / "phone", flags, verbose,
            inject=DROP_FIRST_OFFER)
        phone.call("Page.navigate", {"url": url})
        _, controller_id = wait_pending(driver, timeout)
        wait_value(phone, "globalThis.__mdkrE2EDroppedOffers",
                   lambda value: value == 1, "first offer swallowed", timeout)
        dropped_at = time.monotonic()
        wait_connected(driver, controller_id, 60.0)
        elapsed = time.monotonic() - dropped_at
        require(elapsed < 60.0,
                f"retry after a dropped offer took {elapsed:.1f}s")
        for line in driver.snapshot():
            require("could not connect" not in line,
                    f"driver gave the controller up instead of retrying: {line}")
        require(phone.evaluate("globalThis.__mdkrE2EDroppedOffers") == 1,
                "more than one offer frame was swallowed")
        activate_controls(phone, timeout)
        driver.wait_line(RESULT_OK.match, "result=ok", timeout)
        require(driver.wait_exit(10) == 0, "driver exited non-zero")
    finally:
        close_all(driver, phone, phone_process)
    print(f"scenario offer_drop: PASS — first offer swallowed, fresh peer "
          f"and offer reached Connected in {elapsed:.1f}s (<60s), input "
          "flowed after recovery", flush=True)


def scenario_stall(origin: str, binary: Path, chrome_path: str, base: Path,
                   flags: list[str], verbose: bool, timeout: float) -> None:
    driver = phone = phone_process = None
    stopped = False
    try:
        driver = Driver(binary, origin, packets=400, timeout_ms=150_000,
                        verbose=verbose)
        url = parse_invite(driver, origin, timeout)
        phone_process, phone = phone_browser(
            chrome_path, base / "phone", flags, verbose)
        phone.call("Page.navigate", {"url": url})
        _, controller_id = wait_pending(driver, timeout)
        wait_connected(driver, controller_id, timeout)
        activate_controls(phone, timeout)
        driver.wait_line(
            lambda line: (value := NONNEUTRAL.match(line)) and
            int(value.group(1)) >= 50 and value,
            "50 pre-stall non-neutral packets", timeout)

        os.kill(driver.proc.pid, signal.SIGSTOP)
        stopped = True
        time.sleep(0.3)  # drain lines written before the stop landed
        baseline = latest_nonneutral(driver.snapshot())
        time.sleep(2.7)
        # The stopped driver cannot print, so everything from this index on
        # was emitted after the resume; the heal detection below must only
        # look there (the normal approve flow already emits phase=leased
        # once, long before the stall).
        resume_index = len(driver.snapshot())
        os.kill(driver.proc.pid, signal.SIGCONT)
        stopped = False

        deadline = time.monotonic() + 5.0
        recovered = False
        while time.monotonic() < deadline:
            lines = driver.snapshot()
            if (latest_nonneutral(lines) > baseline and
                    latest_phase(lines, controller_id) == "connected"):
                recovered = True
                break
            time.sleep(0.05)
        require(recovered,
                "input did not resume on a Connected controller within 5s "
                f"of SIGCONT; baseline={baseline} "
                f"last lines: {driver.snapshot()[-8:]}")
        healed = any(
            (value := CONTROLLER.match(line)) and
            value.group(1) == controller_id and value.group(2) == "leased"
            for line in driver.snapshot()[resume_index:])
        driver.wait_line(RESULT_OK.match, "result=ok", timeout)
        require(driver.wait_exit(10) == 0, "driver exited non-zero")
    finally:
        if stopped and driver is not None:
            try:
                os.kill(driver.proc.pid, signal.SIGCONT)
            except OSError:
                pass
        close_all(driver, phone, phone_process)
    detail = ("ingress overflow revoked and self-healed the seat"
              if healed else "the bounded queues absorbed the burst")
    print("scenario stall: PASS — 3s SIGSTOP mid-stream, Connected resumed "
          f"and fresh non-neutral input crossed within 5s ({detail})",
          flush=True)


def run(args: argparse.Namespace) -> None:
    build = resolve(args.build)
    binary = build / "mdkr_native_party_e2e_driver"
    require(binary.is_file(),
            f"missing {binary}; build the mdkr_native_party_e2e_driver "
            "target first (requires -DMDKR_NATIVE_PHONE_PARTY=ON)")
    shell = resolve(args.shell_dir)
    require((shell / "controller/index.html").is_file(),
            "the end-to-end lane requires the staged controller page")
    chrome_path = find_chrome(args.chrome)
    with tempfile.TemporaryDirectory(prefix="mdkr-party-native-e2e-") as temp:
        root = Path(temp)
        origin = f"http://127.0.0.1:{free_port()}"
        log_path = root / "wrangler.log"
        process: subprocess.Popen[bytes] | None = None
        with log_path.open("wb") as log:
            try:
                process = start_worker(origin, shell, root / "state", log,
                                       10_000)
                scenario_golden_path(origin, binary, chrome_path,
                                     root / "golden", args.chrome_flag,
                                     args.verbose, args.timeout)
                scenario_offer_drop(origin, binary, chrome_path,
                                    root / "offer-drop", args.chrome_flag,
                                    args.verbose, args.timeout)
                scenario_stall(origin, binary, chrome_path, root / "stall",
                               args.chrome_flag, args.verbose, args.timeout)
            finally:
                stop_worker(process)
        details = log_path.read_text(encoding="utf-8", errors="replace")
        require("ERROR" not in details.upper(),
                "Wrangler reported an error during the end-to-end lane")
    print("check_party_native_e2e: PASS — native host driver, real local "
          "Worker and real controller page: golden path with a seatless "
          "pending entry and matched phrases, dropped-offer signaling retry "
          "under 60s, and 3s-stall recovery with input back within 5s")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-rel")
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError,
            subprocess.SubprocessError) as error:
        print(f"check_party_native_e2e: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
