#!/usr/bin/env python3
"""Run the private-room UX through two clean browsers and the real local Worker."""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, find_chrome, page_websocket,
    require, wait_value,
)

ROOT = Path(__file__).resolve().parent.parent
SERVICE = ROOT / "services/party"
WRANGLER = SERVICE / "node_modules/wrangler/bin/wrangler.js"
COMPATIBILITY = {
    "protocolVersion": 1,
    "buildId": list(range(1, 17)),
    "gameplayDigest": list(range(128, 160)),
    "romRevision": 1,
    "cadenceHz": 30,
}


def resolve(value: str) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else ROOT / path).resolve()


def node_binary() -> Path:
    candidates: list[Path] = []
    from_path = shutil_which("node")
    if from_path:
        candidates.append(Path(from_path))
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
    raise CheckFailure("Node 22+ is required for the local Party Worker")


def shutil_which(name: str) -> str | None:
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        candidate = Path(directory) / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def wait_worker(origin: str, process: subprocess.Popen[bytes], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise CheckFailure(f"Party Worker exited before readiness ({process.returncode})")
        try:
            with urllib.request.urlopen(origin + "/", timeout=0.5) as response:
                if response.status == 200:
                    return
        except OSError:
            time.sleep(0.1)
    raise CheckFailure("Party Worker did not become ready")


def connect_browser(chrome: ChromeProcess) -> CDPClient:
    cdp = CDPClient(page_websocket(chrome.wait_port()))
    for domain in ("Page", "Runtime", "Network", "Log", "Inspector",
                   "Accessibility"):
        cdp.call(f"{domain}.enable")
    cdp.call("Emulation.setDeviceMetricsOverride", {
        "width": 960, "height": 720, "deviceScaleFactor": 1, "mobile": False,
    })
    return cdp


def install_observers(cdp: CDPClient) -> None:
    cdp.evaluate("""(() => {
      globalThis.__uxRequests=[];
      globalThis.__uxCreated=null;
      globalThis.__uxFailApi=false;
      const actualFetch=globalThis.fetch.bind(globalThis);
      globalThis.__uxActualFetch=actualFetch;
      globalThis.fetch=async(input,init)=>{
        const url=new URL(typeof input==='string'?input:input.url,location.href);
        if (globalThis.__uxFailApi && url.pathname.startsWith('/api/')) {
          throw new TypeError('fixture service outage');
        }
        const response=await actualFetch(input,init);
        if (url.pathname.startsWith('/api/')) {
          globalThis.__uxRequests.push({path:url.pathname,status:response.status});
          if (url.pathname==='/api/match/create' && response.ok) {
            globalThis.__uxCreated=await response.clone().json();
          }
        }
        return response;
      };
      const ActualWebSocket=globalThis.WebSocket;
      globalThis.__uxSockets=[];
      globalThis.WebSocket=class extends ActualWebSocket {
        constructor(...args) { super(...args); globalThis.__uxSockets.push(this); }
      };
      globalThis.__mdkrOnlineControlReleasePolicy=Object.freeze({
        enabled:true,serviceOrigin:location.origin});
    })()""")


def configure(cdp: CDPClient) -> None:
    result = cdp.evaluate(
        f"MDKROnlineRoom.configure({json.dumps({'compatibility': COMPATIBILITY})})",
        await_promise=True,
    )
    require(result is True, "Online Room did not accept the local UX fixture")


def click_action(cdp: CDPClient, action: int, value: int | None = None) -> None:
    result = cdp.evaluate(f"""(() => {{
      const button=document.querySelector('[data-online-action="{action}"]');
      if (!button || button.disabled) return {{clicked:false,
        model:globalThis.MDKROnlineRoom?.current?.()}};
      const select=document.querySelector('#online-room-selection select');
      if (select && {json.dumps(value)} !== null) {{
        select.value=String({json.dumps(value)});
        select.dispatchEvent(new Event('change',{{bubbles:true}}));
      }}
      button.click(); return {{clicked:true}};
    }})()""")
    require(result.get("clicked") is True,
            f"Online Room action {action} unavailable: {result}")


def select_racer(cdp: CDPClient, character: int, track: int) -> None:
    wait_value(cdp, "MDKROnlineRoom.current()?.controls[0]?.action",
               lambda value: value == 6, "character choice", 20)
    click_action(cdp, 6, character)
    wait_value(cdp, "MDKROnlineRoom.current()?.controls[0]?.action",
               lambda value: value == 7, "vehicle choice", 20)
    click_action(cdp, 7, 0)
    wait_value(cdp, "MDKROnlineRoom.current()?.controls[0]?.action",
               lambda value: value == 8, "track choice", 20)
    click_action(cdp, 8, track)
    wait_value(cdp, "MDKROnlineRoom.current()?.controls[0]?.action",
               lambda value: value == 9, "Ready action", 20)


def fatal_lines(cdp: CDPClient) -> list[str]:
    return [line for line in cdp.console
            if "Uncaught" in line or "TypeError:" in line]


def run(args: argparse.Namespace) -> None:
    shell = resolve(args.shell_dir)
    for path in (shell / "index.html", shell / "room/index.html",
                 shell / "online/online-room-live-state.js",
                 shell / "online/online-room-presenter.js",
                 shell / "online/online-room.js", shell / "mdkr-online-tools.wasm",
                 WRANGLER):
        require(path.is_file(), f"two-person UX artifact missing: {path}")
    port = free_port()
    origin = f"http://127.0.0.1:{port}"
    worker: subprocess.Popen[bytes] | None = None
    host_chrome: ChromeProcess | None = None
    guest_chrome: ChromeProcess | None = None
    host: CDPClient | None = None
    guest: CDPClient | None = None
    with tempfile.TemporaryDirectory(prefix="mdkr-two-person-") as temporary:
        log_path = Path(temporary) / "wrangler.log"
        log = log_path.open("wb")
        command = [str(node_binary()), str(WRANGLER), "dev", "--local",
                   "--ip", "127.0.0.1", "--port", str(port),
                   "--inspector-port", "0", "--assets", str(shell),
                   "--persist-to", str(Path(temporary) / "worker-state"),
                   "--var", f"PARTY_ORIGIN:{origin}",
                   "--var", "PARTY_HMAC_KEY:ux-fixture-secret-32-bytes-long!!",
                   "--var", "MAX_ADMISSIONS_PER_DAY:10000",
                   "--var", "CONTROL_RESERVE_PER_DAY:5000",
                   "--log-level", "error"]
        try:
            worker = subprocess.Popen(command, cwd=SERVICE, stdout=log,
                                      stderr=subprocess.STDOUT)
            wait_worker(origin, worker, args.timeout)
            chrome_path = find_chrome(args.chrome)
            host_chrome = ChromeProcess(chrome_path, Path(temporary) / "host-profile",
                                        args.chrome_flag, args.verbose)
            guest_chrome = ChromeProcess(chrome_path, Path(temporary) / "guest-profile",
                                         args.chrome_flag, args.verbose)
            host = connect_browser(host_chrome)
            guest = connect_browser(guest_chrome)

            # The shipped online-control-config.js is fail-closed
            # (enabled:false) until the multiplayer GO, and the invite entry
            # landing runs AT PAGE LOAD: a boot without the enabled policy
            # DESTROYS the parked invite capability ("This invitation was
            # removed from this browser") before any post-load override can
            # run, stranding the guest at Play Online with zero requests.
            # This check exercises the GO configuration, so pin the enabled
            # policy in a boot script -- as an accessor the served config
            # file's own assignment cannot downgrade (its strict-mode write
            # goes through the no-op setter without throwing).
            for cdp in (host, guest):
                cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": (
                    "(() => { const v = Object.freeze({enabled:true,"
                    "serviceOrigin:location.origin});"
                    "Object.defineProperty(globalThis,"
                    "'__mdkrOnlineControlReleasePolicy',"
                    "{configurable:false, get:() => v, set:() => {}}); })();"
                )})

            host.call("Page.navigate", {"url": origin + "/"})
            guest.call("Page.navigate", {"url": origin + "/"})
            for label, cdp in (("host", host), ("guest", guest)):
                wait_value(cdp, "Boolean(globalThis.MDKROnlineRoom) && "
                           "document.readyState==='complete'", bool,
                           f"{label} clean launcher", args.timeout)
                install_observers(cdp)
                configure(cdp)

            # A clean-profile guest can back out to the ordinary ROM picker
            # without creating or joining anything.
            guest.evaluate("MDKROnlineRoom.open()")
            click_action(guest, 17)
            wait_value(guest, "document.activeElement.id",
                       lambda value: value == "drop", "local fallback", 10)
            require(guest.evaluate("__uxRequests.length") == 0,
                    "local fallback contacted the room service")

            host.evaluate("MDKROnlineRoom.open()")
            created_at = time.monotonic()
            click_action(host, 1)
            wait_value(host, "document.getElementById('online-room-title').textContent",
                       lambda value: value == "Private Room", "host room creation",
                       args.timeout)
            create_seconds = time.monotonic() - created_at
            require(create_seconds < 10, f"room creation took {create_seconds:.2f}s")
            click_action(host, 3)
            invite = wait_value(host, """(() => ({
              url:globalThis.__uxCreated?.inviteUrl||'',
              code:document.querySelector('.online-room-code')?.textContent||'',
              qr:document.querySelector('.online-room-qr')?.width||0
            }))()""", lambda value: isinstance(value, dict) and bool(value.get("url")),
                "shareable invitation", args.timeout)
            require(re.match(r"^\d{3} \d{3}$", invite["code"]) and invite["qr"] > 0,
                    f"host invite was not scan/code accessible: {invite}")
            host.evaluate("""[...document.querySelectorAll('#online-room-auxiliary button')]
              .find(button=>button.textContent==='Share Link').click()""")
            wait_value(host, "document.getElementById('online-room-auxiliary').textContent",
                       lambda value: "Invitation Ready" in value or
                           "Share the Room Code" in value,
                       "share fallback", 10)

            # Wrong-role paste is recoverable and does not mutate the room.
            guest.evaluate("MDKROnlineRoom.open()")
            click_action(guest, 2)
            recovery = guest.evaluate("""(() => {
              const input=document.querySelector('#online-room-auxiliary input');
              input.value=location.origin+'/controller/#'+'C'.repeat(43);
              [...document.querySelectorAll('#online-room-auxiliary button')]
                .find(button=>button.textContent==='Join Room').click();
              return document.getElementById('online-room-auxiliary').textContent;
            })()""")
            require("Check the Invite" in recovery and "six-digit" in recovery,
                    f"wrong-role recovery was a dead end: {recovery}")

            # The real /room/ role route transfers the fragment through
            # same-origin session storage, erases it, and opens in profile B.
            join_at = time.monotonic()
            guest.call("Page.navigate", {"url": invite["url"]})
            wait_value(guest, "Boolean(globalThis.MDKROnlineRoom) && "
                       "location.pathname==='/'", bool, "room-link handoff",
                       args.timeout)
            install_observers(guest)
            configure(guest)
            joined = wait_value(guest, """(() => ({
              title:document.getElementById('online-room-title').textContent,
              open:document.getElementById('online-room-dialog').open,
              hash:location.hash,
              members:MDKROnlineRoom.current()?.members||0
            }))()""", lambda value: isinstance(value, dict) and
                value.get("members") == 2, "guest private-room join", args.timeout)
            join_seconds = time.monotonic() - join_at
            require(joined["open"] and joined["hash"] == "" and join_seconds < 10,
                    f"guest join was unsafe or slow: {joined}, {join_seconds:.2f}s")
            wait_value(host, "MDKROnlineRoom.current()?.members",
                       lambda value: value == 2, "host peer publication", args.timeout)

            # A real socket outage must recover from the newest authenticated
            # state without losing membership or revealing Start.
            sockets_before = guest.evaluate("__uxSockets.length")
            guest.evaluate("""(() => {
              globalThis.__uxFailApi=true;
              const socket=globalThis.__uxSockets.at(-1);
              socket?.close(4000,'ux outage');
            })()""")
            wait_value(guest, "document.getElementById('online-room-title').textContent",
                       lambda value: value == "Could Not Reach the Room",
                       "visible outage recovery", args.timeout)
            guest.evaluate("globalThis.__uxFailApi=false")
            click_action(guest, 14)
            wait_value(guest, "MDKROnlineRoom.current()?.members",
                       lambda value: value == 2, "authenticated room recovery",
                       args.timeout)
            wait_value(guest, "__uxSockets.length",
                       lambda value: value > sockets_before,
                       "state socket reconnection", args.timeout)

            # The shipped presenter hard-locks check_setup for every
            # non-fixture config (online-room-presenter.js routes it to
            # "setup_locked") until rollback qualification is approved --
            # the human release gate. Against the real service the
            # two-person journey can go no further today, so pre-GO this
            # check proves the LOCK: the click surfaces the calm lock
            # message, advances nothing, and sends no service command.
            # --post-go runs the full selection/ready journey unchanged
            # once the product flips at GO.
            click_action(host, 4)
            locked = wait_value(host, """(() => ({
              aux:document.getElementById('online-room-auxiliary').textContent,
              action:MDKROnlineRoom.current()?.controls[0]?.action
            }))()""", lambda value: isinstance(value, dict) and
                "Online Racing Is Still Locked" in (value.get("aux") or ""),
                "pre-GO setup lock message", args.timeout)
            require(locked["action"] == 4,
                    f"pre-GO lock advanced the room: {locked}")
            lock_commands = [item for item in host.evaluate("__uxRequests.slice()")
                             if item["path"].endswith("/command")]
            require(lock_commands == [],
                    f"pre-GO Check Setup sent a service command: {lock_commands}")
            for label, cdp in (("host", host), ("guest", guest)):
                visible = cdp.evaluate(
                    """[...document.querySelectorAll('[data-online-action]')]
                      .map(button=>Number(button.dataset.onlineAction))""")
                require(11 not in visible,
                        f"{label} exposed Start Race before GO: {visible}")
            if not args.post_go:
                ready_seconds = 0.0
                print("  pre-GO: setup lock held (no advance, no command); "
                      "run --post-go after rollback qualification approval "
                      "for the selection/ready journey")
            if args.post_go:
                click_action(host, 4)
                click_action(guest, 4)
                select_racer(host, character=0, track=5)
                select_racer(guest, character=1, track=3)
                ready_at = time.monotonic()
                click_action(host, 9)
                wait_value(host, "document.getElementById('online-room-model-status').textContent",
                           lambda value: value == "Waiting for Friends",
                           "host waiting state", args.timeout)
                click_action(guest, 9)
                for label, cdp in (("host", host), ("guest", guest)):
                    state = wait_value(cdp, """(() => ({
                      status:document.getElementById('online-room-model-status').textContent,
                      admission:MDKROnlineRoom.current()?.admission,
                      actions:[...document.querySelectorAll('[data-online-action]')]
                        .map(button=>Number(button.dataset.onlineAction))
                    }))()""", lambda value: isinstance(value, dict) and
                        value.get("status") == "Everyone Ready",
                        f"{label} everyone-ready state", args.timeout)
                    require(state["admission"] is False and 11 not in state["actions"],
                            f"{label} exposed race admission before GO: {state}")
                ready_seconds = time.monotonic() - ready_at
                require(ready_seconds < 10,
                        f"two-profile Ready convergence took {ready_seconds:.2f}s")

                # Backtracking from Ready is explicit, observable by the friend,
                # and can return to the durable achievement.
                click_action(guest, 10)
                wait_value(host, "document.getElementById('online-room-model-status').textContent",
                           lambda value: value == "Waiting for Friends",
                           "Ready backtrack publication", args.timeout)
                guest.evaluate("""[...document.querySelectorAll('#online-room-auxiliary button')]
                  .find(button=>button.textContent==='Save Selection').click()""")
                wait_value(guest, """(() => ({
                  action:MDKROnlineRoom.current()?.controls[0]?.action,
                  enabled:!document.querySelector('[data-online-action="9"]')?.disabled,
                  saving:document.getElementById('online-room-auxiliary').textContent
                    .includes('Saving Selection')
                }))()""", lambda value: isinstance(value, dict) and
                    value.get("action") == 9 and value.get("enabled") is True and
                    value.get("saving") is False,
                    "selection backtrack recovery", args.timeout)
                click_action(guest, 9)
                wait_value(host, "document.getElementById('online-room-model-status').textContent",
                           lambda value: value == "Everyone Ready",
                           "Ready reconvergence", args.timeout)

                ax = guest.call("Accessibility.getFullAXTree").get("nodes", [])
                names = {node.get("name", {}).get("value", "") for node in ax}
                folded = {name.casefold() for name in names}
                require("change selection" in folded and "leave room" in folded,
                        f"guest Ready actions are not accessible: {sorted(names)}")

            click_action(guest, 24)
            wait_value(host, "MDKROnlineRoom.current()?.members",
                       lambda value: value == 1, "guest leave publication", args.timeout)
            click_action(host, 24)
            require(not host.failures and not guest.failures,
                    "browser network/CDP failures: " +
                    "; ".join(host.failures + guest.failures))
            require(not fatal_lines(host) and not fatal_lines(guest),
                    "two-person browser console error: " +
                    "; ".join(fatal_lines(host) + fatal_lines(guest)))
            require(not any("mdkr64_web.wasm" in request.get("url", "")
                            for request in host.network + guest.network),
                    "two-person room control loaded the game engine")
            journey = (f"Ready {ready_seconds:.2f}s; role recovery, outage, "
                       "backtrack, AX and leave" if args.post_go else
                       "pre-GO setup lock held; role recovery, outage and leave")
            print("check_browser_online_two_person: PASS — two clean profiles + "
                  f"real Worker create {create_seconds:.2f}s, "
                  f"join {join_seconds:.2f}s, {journey}")
        except Exception as error:
            log.flush()
            details = log_path.read_text(encoding="utf-8", errors="replace")[-8000:]
            if isinstance(error, CheckFailure):
                raise CheckFailure(f"{error}\nWrangler log:\n{details}") from error
            raise
        finally:
            for cdp in (host, guest):
                if cdp is not None:
                    cdp.close()
            for chrome in (host_chrome, guest_chrome):
                if chrome is not None:
                    chrome.close()
            if worker is not None and worker.poll() is None:
                worker.terminate()
                try:
                    worker.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    worker.kill()
                    worker.wait(timeout=5)
            log.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument(
        "--post-go", action="store_true",
        help="run the selection/ready journey; requires the product's "
             "rollback-qualification GO (pre-GO the presenter locks setup "
             "and this check proves the lock instead)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"check_browser_online_two_person: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
