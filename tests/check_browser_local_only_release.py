#!/usr/bin/env python3
"""Prove the published local-only browser artifact exposes local play only."""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, OverlayServer, find_chrome,
    page_websocket, require, wait_value,
)


ROOT = Path(__file__).resolve().parent.parent
CLOUD_PATHS = ("controller", "online", "party", "room")


def resolve(value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    return path.resolve()


def run(args: argparse.Namespace) -> None:
    shell = resolve(args.shell_dir)
    require(shell.is_dir(), f"local-only shell does not exist: {shell}")
    build_info = json.loads((shell / "build-info.json").read_text(encoding="utf-8"))
    require(build_info.get("release_scope") == "local-only" and
            build_info.get("cloud_features") == {
                "online_room": False, "phone_party": False,
            }, f"local-only build identity is incomplete: {build_info}")
    for path in CLOUD_PATHS:
        require(not (shell / path).exists(),
                f"cloud route survived in published payload: {path}/")
    for path in ("mdkr-online-tools.js", "mdkr-online-tools.wasm"):
        require(not (shell / path).exists(),
                f"online-only runtime survived in published payload: {path}")

    server = OverlayServer(shell, shell)
    server.start()
    with tempfile.TemporaryDirectory(prefix="mdkr-browser-local-only-") as profile:
        chrome = ChromeProcess(find_chrome(args.chrome), Path(profile),
                               args.chrome_flag, args.verbose)
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Network"):
                cdp.call(f"{domain}.enable")
            cdp.call("Page.navigate", {"url": server.origin + "/"})
            wait_value(cdp, "document.readyState", lambda value: value == "complete",
                       "local-only launcher load", args.timeout)
            surface = cdp.evaluate("""(() => ({
              onlineButton:!document.getElementById('online-room-open'),
              phoneButton:!document.getElementById('add-phone-controllers'),
              onlineDialog:!document.getElementById('online-room-dialog'),
              partyDialog:!document.getElementById('party-dialog'),
              partyStage:!document.getElementById('party-stage-button'),
              onlineApi:typeof globalThis.MDKROnlineRoom,
              partyApi:typeof globalThis.MDKRPartyHost,
              policy:typeof globalThis.__mdkrOnlineControlReleasePolicy,
              visible:document.body.innerText,
              resources:performance.getEntriesByType('resource').map(entry =>
                new URL(entry.name).pathname)
            }))()""")
            require(surface["onlineButton"] is True and
                    surface["phoneButton"] is True and
                    surface["onlineDialog"] is True and
                    surface["partyDialog"] is True and
                    surface["partyStage"] is True,
                    f"deferred controls survived in rendered launcher: {surface}")
            require(surface["onlineApi"] == "undefined" and
                    surface["partyApi"] == "undefined" and
                    surface["policy"] == "undefined",
                    f"deferred browser APIs were loaded: {surface}")
            visible = surface["visible"].lower()
            for phrase in ("online room", "private online", "phone party",
                           "phone controllers"):
                require(phrase not in visible,
                        f"deferred feature is visible in launcher text: {phrase}")
            cloud_prefixes = tuple(f"/{path}/" for path in CLOUD_PATHS)
            require(not any(path.startswith(cloud_prefixes)
                            for path in surface["resources"]),
                    f"launcher loaded cloud-only resources: {surface['resources']}")

            statuses = cdp.evaluate("""Promise.all([
              '/controller/', '/online/online-room.js',
              '/party/party-host.js', '/room/'
            ].map(async path => ({path, status:(await fetch(path)).status})))""",
                await_promise=True)
            require(all(item["status"] == 404 for item in statuses),
                    f"deferred public routes are still reachable: {statuses}")
            require(not cdp.failures,
                    "browser/CDP failures: " + "; ".join(cdp.failures))
            require(not cdp.exceptions,
                    "browser exceptions: " + "; ".join(cdp.exceptions))
            print("check_browser_local_only_release: PASS -- local launcher only, "
                  "cloud controls/APIs/routes absent")
        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()
            server.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shell-dir", required=True)
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"check_browser_local_only_release: FAIL -- {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
