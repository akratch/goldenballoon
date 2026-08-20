#!/usr/bin/env python3
"""End-to-end zero-internet Phone Party: native host + real browser, NO cloud.

This is the crown gate for local-only play. Every other LAN gate stubs a leg:
the transport test fakes the phone socket, the room/server tests never open a
browser, and the page unit tests reach only the controller's pure seams. This
lane runs the WHOLE no-internet triangle for real and with NO wrangler worker
anywhere in it:

* the packaged native host model (``mdkr_native_party_e2e_driver --lan``) whose
  MdkrLanPartyTransport OWNS the embedded MdkrLanPartyServer + in-process
  MdkrLanPartyRoom -- the signaling backend is in-process, not a cloud Worker;
* a headless Chromium controller that loads ``/controller/#<capability>`` from
  THAT embedded server over plain ``http`` (an insecure origin), redeems over
  its ``/party-ws``, and pairs with the pure-JS SAS fallback;
* real libdatachannel WebRTC with real DTLS fingerprints between the two.

GUARANTEEING THE JS SAS FALLBACK IS WHAT RUNS (or the crown assertion is
hollow). The pairing phrase is only a cross-proof of the pure-JS SHA-256 + P-256
twin against the native mbedtls crypto if the page could not have used
``crypto.subtle``. The browser exposes ``crypto.subtle`` on a *secure* context,
and a loopback origin (``127.0.0.1``) is a secure context by the browser's own
rules -- so serving on loopback would silently let the page use the vetted
engine crypto and make the phrase match prove nothing. This lane forces the
insecure path two ways and asserts it in the live page before it will accept a
phrase:

* PRIMARY (``lan-ip``): serve the page from a real non-loopback private IPv4
  (the same enumeration the server freezes into its Host allowlist). The
  browser reports ``isSecureContext === false`` and does NOT expose
  ``crypto.subtle`` -- genuinely, with zero page spoofing.
* FALLBACK (``loopback-forced``): only when the host has no private LAN IPv4
  (e.g. a CI box with loopback alone), serve on ``127.0.0.1`` and install a
  boot hook that makes ``isSecureContext`` report false and ``crypto.subtle``
  undefined, reproducing the identical insecure-origin conditions.

Either way ``assert_js_fallback`` pins, IN THE RUNNING PAGE, that
``isSecureContext === false``, ``crypto.subtle === undefined``, and the
pure-JS ``MDKRPartySas.fallback`` is present. ``getSubtle()`` in party-sas.js is
read at call time, so with subtle absent the JS twin is the ONLY code that can
have produced the words the page renders. Their equality with the native phrase
is therefore an end-to-end cross-proof of the two crypto implementations.

Scenarios, each with its own PASS line:

* ``golden_path`` -- browser loads over http from the embedded server, redeems
  the minted capability over ws, the native mbedtls phrase EQUALS the page's
  pure-JS phrase (the crown), and non-neutral pad packets cross the real
  ingress.
* ``code_path`` -- a WRONG six-digit code is refused, then the right code pairs
  through to Connected with matching phrases and input flow.
* ``throttle`` -- 13 rapid wrong codes trip the room's shared code bucket and
  the phone shows the typed "Too many code attempts" refusal.
* ``stop`` -- the host closes the room; the phone, re-redeeming over the
  still-served ws, learns and shows ``host_closed``.
"""

from __future__ import annotations

import argparse
import json
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, find_chrome, page_websocket,
    require, wait_value,
)
from check_party_native_e2e import (
    APPROVE, CONTROLLER, Driver, activate_controls, close_all, match_phrases,
    wait_connected, wait_pending,
)

ROOT = Path(__file__).resolve().parent.parent

INVITE = re.compile(r"^\[E2E\] invite url=(\S+) code=(\d{6})$")
ROOM_CLOSED = re.compile(r"^\[E2E\] room_closed")
RESULT_OK = re.compile(r"^\[E2E\] result=ok nonneutral=(\d+) packets=(\d+)$")

# Loopback-forced mode ONLY: reproduce the insecure-origin conditions a real LAN
# IP gives for free. isSecureContext drives the page's crypto-tolerance gate and
# crypto.subtle drives which SAS path runs; both are read live, so shadowing the
# instance property before any page script runs forces the pure-JS fallback.
FORCE_INSECURE = """(() => {
  try {
    Object.defineProperty(window, "isSecureContext",
      {configurable: true, get() { return false; }});
  } catch (_) {}
  try {
    Object.defineProperty(crypto, "subtle",
      {configurable: true, get() { return undefined; }});
  } catch (_) {}
})();"""


def resolve(value: str) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else ROOT / path).resolve()


def is_private_lan(ip: str) -> bool:
    """RFC 1918 private IPv4, excluding loopback and link-local -- the reachable
    LAN addresses the server allowlists and the page's privateHostname trusts."""
    parts = ip.split(".")
    if len(parts) != 4 or not all(p.isdigit() and 0 <= int(p) <= 255
                                  for p in parts):
        return False
    a, b = int(parts[0]), int(parts[1])
    return a == 10 or (a == 172 and 16 <= b <= 31) or (a == 192 and b == 168)


def discover_lan_host() -> str | None:
    """A non-loopback private IPv4 this machine answers on, or None. Mirrors the
    server's own getifaddrs enumeration closely enough that whatever this
    returns is in the /party-ws Host allowlist the server freezes at start()."""
    candidates: set[str] = set()
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.connect(("8.8.8.8", 80))
            candidates.add(probe.getsockname()[0])
        finally:
            probe.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None,
                                       socket.AF_INET):
            candidates.add(info[4][0])
    except OSError:
        pass
    private = sorted(ip for ip in candidates if is_private_lan(ip))
    return private[0] if private else None


class LanDriver(Driver):
    """The native host driver over the embedded LanPartyTransport. Reuses the
    cloud Driver's line pump / wait helpers; only the launch shape differs (no
    wrangler token, no --origin -- the transport is its own backend)."""

    def __init__(self, binary: Path, web_root: Path, host: str, *,
                 auto_approve: bool, packets: int, timeout_ms: int,
                 close_room: bool, verbose: bool):
        command = [str(binary), "--lan", "--lan-web-root", str(web_root),
                   "--lan-host", host, "--packets", str(packets),
                   "--timeout-ms", str(timeout_ms)]
        if auto_approve:
            command.append("--auto-approve")
        if close_room:
            command.append("--close-room-after-connected")
        self.proc = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1)
        self.verbose = verbose
        self.lines: list[str] = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()


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


def parse_invite(driver: LanDriver, host: str,
                 timeout: float) -> tuple[str, str, str]:
    """The invite the embedded server advertises: (full url, base url, code).
    The URL must be a plain-http capability link on the advertised LAN host and
    the driver's own ephemeral port -- proof the page loads from the embedded
    server, not a file:// or a wrangler asset."""
    _, matched = driver.wait_line(
        lambda line: INVITE.match(line), "invite url/code", timeout)
    url = matched.group(1)
    code = matched.group(2)
    prefix = f"http://{host}:"
    require(url.startswith(prefix) and "/controller/#" in url,
            f"invite is not a plain-http capability on the embedded server "
            f"({host}): {url}")
    fragment = url.rsplit("#", 1)[1]
    require(len(fragment) == 43,
            f"invite fragment is not a 43-char capability: {url}")
    base = url.split("#", 1)[0]
    return url, base, code


def assert_js_fallback(phone: CDPClient, mode: str, timeout: float) -> None:
    """The guarantee the crown assertion rests on: in the LIVE page, the browser
    exposes no crypto.subtle and reports an insecure context, and the pure-JS
    SAS twin is present. With subtle absent, party-sas.js's getSubtle() returns
    undefined at every call, so the JS fallback is the ONLY code that can derive
    the phrase -- there is no vetted-engine path left to hide behind."""
    wait_value(phone, "!!globalThis.MDKRPartySas", bool,
               "MDKRPartySas loaded", timeout)
    verdict = phone.evaluate(
        "JSON.stringify({"
        "sc: isSecureContext,"
        "subtle: (typeof crypto !== 'undefined' && crypto.subtle === undefined),"
        "rng: !!(typeof crypto !== 'undefined' && crypto.getRandomValues),"
        "sas: !!globalThis.MDKRPartySas,"
        "fb: !!(globalThis.MDKRPartySas && globalThis.MDKRPartySas.fallback)})")
    data = json.loads(verdict)
    require(data["sc"] is False,
            f"[{mode}] page reports a secure context, so crypto.subtle is "
            f"available and the JS SAS fallback is not what runs: {verdict}")
    require(data["subtle"] is True,
            f"[{mode}] crypto.subtle is present; the phrase match would not "
            f"cross-prove the JS SAS against native crypto: {verdict}")
    require(data["rng"] and data["sas"] and data["fb"],
            f"[{mode}] the page is missing the pure-JS SAS fallback it must "
            f"run on an insecure origin: {verdict}")


def wait_redeemed(phone: CDPClient, timeout: float) -> None:
    wait_value(phone,
               "!document.getElementById('state-waiting').hidden || "
               "!document.getElementById('state-assigned').hidden || "
               "!document.getElementById('state-controller').hidden",
               bool, "redeemed phone", timeout)


def wait_code_entry(phone: CDPClient, timeout: float) -> None:
    wait_value(phone, "!document.getElementById('state-code').hidden", bool,
               "code entry surface", timeout)


def submit_code(phone: CDPClient, code: str, timeout: float) -> None:
    wait_code_entry(phone, timeout)
    phone.evaluate(
        "(() => {"
        "const input = document.getElementById('room-code');"
        f"input.value = {code!r};"
        "document.getElementById('code-form').dispatchEvent("
        "new Event('submit', {bubbles: true, cancelable: true}));})()")


def wait_error_title(phone: CDPClient, timeout: float) -> str:
    wait_value(phone, "!document.getElementById('state-error').hidden", bool,
               "typed refusal surfaced", timeout)
    return str(phone.evaluate(
        "document.getElementById('error-title').textContent"))


def return_to_code(phone: CDPClient) -> None:
    """From an error surface, take the page back to code entry the way a person
    does -- the error's own recovery action, not a reload."""
    phone.evaluate(
        "(() => {const recovery = document.getElementById('error-recovery');"
        "if (!document.getElementById('state-error').hidden && recovery) "
        "recovery.click();})()")


def wrong_code(code: str) -> str:
    """A well-formed six-digit code guaranteed to differ from the real one, so
    the room refuses it (and, for the throttle, spends a bucket slot)."""
    lead = "1" if code[0] != "1" else "2"
    return lead + code[1:]


def scenario_golden_path(host: str, mode: str, inject: str | None,
                         binary: Path, web_root: Path, chrome_path: str,
                         base: Path, flags: list[str], verbose: bool,
                         timeout: float) -> None:
    driver = phone = phone_process = None
    try:
        driver = LanDriver(binary, web_root, host, auto_approve=True,
                           packets=50, timeout_ms=90_000, close_room=False,
                           verbose=verbose)
        url, _, _ = parse_invite(driver, host, timeout)
        phone_process, phone = phone_browser(
            chrome_path, base / "phone", flags, verbose, inject=inject)
        phone.call("Page.navigate", {"url": url})
        assert_js_fallback(phone, mode, timeout)
        wait_redeemed(phone, timeout)
        pending_index, controller_id = wait_pending(driver, timeout)
        match_phrases(driver, phone, controller_id, timeout)
        connected_index = wait_connected(driver, controller_id, timeout)
        lines = driver.snapshot()
        approve_indices = [index for index, line in enumerate(lines)
                           if APPROVE.match(line)]
        require(bool(approve_indices), "driver never echoed an approval")
        require(pending_index < approve_indices[0] < connected_index,
                "pending -> approve -> connected out of order: "
                f"{pending_index}/{approve_indices[0]}/{connected_index}")
        activate_controls(phone, timeout)
        driver.wait_line(RESULT_OK.match, "result=ok", timeout)
        require(driver.wait_exit(15) == 0, "driver exited non-zero")
    finally:
        close_all(driver, phone, phone_process)
    print("scenario golden_path: PASS — page loaded over http from the "
          f"embedded server ({mode}), redeemed the capability over ws, the "
          "native mbedtls phrase equalled the page's pure-JS SAS phrase, and "
          "50 non-neutral packets crossed the real ingress", flush=True)


def scenario_code_path(host: str, mode: str, inject: str | None, binary: Path,
                       web_root: Path, chrome_path: str, base: Path,
                       flags: list[str], verbose: bool, timeout: float) -> None:
    driver = phone = phone_process = None
    try:
        driver = LanDriver(binary, web_root, host, auto_approve=True,
                           packets=50, timeout_ms=90_000, close_room=False,
                           verbose=verbose)
        _, controller_base, code = parse_invite(driver, host, timeout)
        phone_process, phone = phone_browser(
            chrome_path, base / "phone", flags, verbose, inject=inject)
        # No fragment: the page opens straight onto its six-digit entry.
        phone.call("Page.navigate", {"url": controller_base})
        assert_js_fallback(phone, mode, timeout)
        submit_code(phone, wrong_code(code), timeout)
        refusal = wait_error_title(phone, timeout)
        require(phone.evaluate(
            "document.getElementById('state-waiting').hidden && "
            "document.getElementById('state-assigned').hidden && "
            "document.getElementById('state-controller').hidden"),
            f"a wrong code was not refused; page advanced past entry: {refusal}")
        require(not [line for line in driver.snapshot()
                     if CONTROLLER.match(line)],
                "a wrong code minted a controller at the host")
        return_to_code(phone)
        submit_code(phone, code, timeout)
        wait_redeemed(phone, timeout)
        _, controller_id = wait_pending(driver, timeout)
        match_phrases(driver, phone, controller_id, timeout)
        wait_connected(driver, controller_id, timeout)
        activate_controls(phone, timeout)
        driver.wait_line(RESULT_OK.match, "result=ok", timeout)
        require(driver.wait_exit(15) == 0, "driver exited non-zero")
    finally:
        close_all(driver, phone, phone_process)
    print(f"scenario code_path: PASS — wrong code refused ({refusal!r}), right "
          "code paired through to Connected with matching phrases and input "
          "flow", flush=True)


def scenario_throttle(host: str, mode: str, inject: str | None, binary: Path,
                      web_root: Path, chrome_path: str, base: Path,
                      flags: list[str], verbose: bool, timeout: float) -> None:
    driver = phone = phone_process = None
    attempts = 13
    try:
        driver = LanDriver(binary, web_root, host, auto_approve=False,
                           packets=50, timeout_ms=90_000, close_room=False,
                           verbose=verbose)
        _, controller_base, code = parse_invite(driver, host, timeout)
        guess = wrong_code(code)
        phone_process, phone = phone_browser(
            chrome_path, base / "phone", flags, verbose, inject=inject)
        phone.call("Page.navigate", {"url": controller_base})
        assert_js_fallback(phone, mode, timeout)
        titles: list[str] = []
        for attempt in range(attempts):
            if attempt:
                return_to_code(phone)
            submit_code(phone, guess, timeout)
            titles.append(wait_error_title(phone, timeout))
        require(titles[-1] == "Too many code attempts",
                f"the {attempts}th wrong code did not trip the throttle; "
                f"titles seen: {titles}")
        require(all(title != "Too many code attempts" for title in titles[:12]),
                f"the throttle tripped before the bucket filled: {titles}")
    finally:
        close_all(driver, phone, phone_process)
    print(f"scenario throttle: PASS — {attempts} rapid wrong codes tripped the "
          "room's shared bucket and the phone showed the typed "
          "'Too many code attempts' refusal", flush=True)


def scenario_stop(host: str, mode: str, inject: str | None, binary: Path,
                  web_root: Path, chrome_path: str, base: Path,
                  flags: list[str], verbose: bool, timeout: float) -> None:
    driver = phone = phone_process = None
    try:
        driver = LanDriver(binary, web_root, host, auto_approve=True,
                           packets=10, timeout_ms=90_000, close_room=True,
                           verbose=verbose)
        url, _, _ = parse_invite(driver, host, timeout)
        phone_process, phone = phone_browser(
            chrome_path, base / "phone", flags, verbose, inject=inject)
        phone.call("Page.navigate", {"url": url})
        assert_js_fallback(phone, mode, timeout)
        wait_redeemed(phone, timeout)
        _, controller_id = wait_pending(driver, timeout)
        wait_connected(driver, controller_id, timeout)
        activate_controls(phone, timeout)
        # The driver closes the room once input has demonstrably flowed; the
        # server stays up so the phone's re-redeem learns the terminal reason.
        driver.wait_line(ROOM_CLOSED.match, "host closed the room", timeout)
        wait_value(
            phone,
            "(!document.getElementById('state-error').hidden && "
            "document.getElementById('error-title').textContent) || ''",
            lambda value: value == "Controller room ended",
            "host_closed on the phone", timeout)
        require(driver.wait_exit(20) == 0, "driver exited non-zero")
    finally:
        close_all(driver, phone, phone_process)
    print("scenario stop: PASS — the host closed the room and the phone, "
          "re-redeeming over the still-served ws, showed host_closed",
          flush=True)


def run(args: argparse.Namespace) -> None:
    build = resolve(args.build)
    binary = build / "mdkr_native_party_e2e_driver"
    require(binary.is_file(),
            f"missing {binary}; build the mdkr_native_party_e2e_driver target "
            "first (requires -DMDKR_NATIVE_PHONE_PARTY=ON)")
    web_root = resolve(args.shell_dir)
    require((web_root / "controller/index.html").is_file(),
            "the LAN end-to-end lane requires the staged controller page")
    chrome_path = str(find_chrome(args.chrome))

    lan_host = discover_lan_host()
    if lan_host is not None:
        host, mode, inject = lan_host, "lan-ip", None
    else:
        host, mode, inject = "127.0.0.1", "loopback-forced", FORCE_INSECURE
    print(f"check_party_lan_e2e: serving from the embedded server on {host} "
          f"({mode}); the JS SAS fallback is guaranteed by "
          "isSecureContext==false + crypto.subtle absent", flush=True)

    with tempfile.TemporaryDirectory(prefix="mdkr-party-lan-e2e-") as temp:
        root = Path(temp)
        common = (host, mode, inject, binary, web_root, chrome_path)
        scenario_golden_path(*common, root / "golden", args.chrome_flag,
                             args.verbose, args.timeout)
        scenario_code_path(*common, root / "code", args.chrome_flag,
                           args.verbose, args.timeout)
        scenario_throttle(*common, root / "throttle", args.chrome_flag,
                          args.verbose, args.timeout)
        scenario_stop(*common, root / "stop", args.chrome_flag,
                      args.verbose, args.timeout)
    print("check_party_lan_e2e: PASS — native host + embedded server/room + "
          "real browser, NO wrangler: capability golden path with matched "
          f"JS/native phrases ({mode}), wrong-then-right code redemption, a "
          "13-code throttle trip, and host_closed on stop")


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
        print(f"check_party_lan_e2e: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
