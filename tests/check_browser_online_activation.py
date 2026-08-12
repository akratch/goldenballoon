#!/usr/bin/env python3
"""Qualify the clean-build/local-ROM Online Room activation handoff."""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, OverlayServer, find_chrome,
    page_websocket, require, wait_value,
)

ROOT = Path(__file__).resolve().parent.parent


def resolve(value: str) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else ROOT / path).resolve()


def run(args: argparse.Namespace) -> None:
    shell = resolve(args.shell_dir)
    for relative in ("index.html", "online/online-control-config.js",
                     "online/online-room.js", "mdkr-online-tools.js",
                     "mdkr-online-tools.wasm", "mdkr64-shell.js",
                     "build-info.json"):
        require((shell / relative).is_file(),
                f"browser Online Room activation artifact missing: {relative}")
    server = OverlayServer(shell, shell)
    server.start()
    with tempfile.TemporaryDirectory(prefix="mdkr-online-activation-") as profile:
        chrome = ChromeProcess(find_chrome(args.chrome), Path(profile),
                               args.chrome_flag, args.verbose)
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Inspector"):
                cdp.call(f"{domain}.enable")
            cdp.call("Page.navigate", {"url": server.origin + "/"})
            wait_value(cdp, "Boolean(globalThis.MDKROnlineRoom) && "
                       "document.readyState === 'complete'", bool,
                       "launcher activation API", args.timeout)
            require(cdp.evaluate("""({
              enabled:MDKROnlineRoom.enabled(),
              policy:globalThis.__mdkrOnlineControlReleasePolicy?.enabled,
              modelScripts:[...document.scripts].filter(script=>
                script.src.includes('mdkr-online-tools')).length
            })""") == {"enabled": False, "policy": False, "modelScripts": 0},
                    "published default did not remain a lazy disabled gate")

            result = cdp.evaluate("""(async()=>{
              const originalFetch=globalThis.fetch.bind(globalThis);
              globalThis.__activationFetches=[];
              globalThis.fetch=(input,...rest)=>{
                const url=typeof input==='string'?input:(input?.url||String(input));
                globalThis.__activationFetches.push(url);
                return originalFetch(input,...rest);
              };
              const clean={version:'1.2.1',
                source_commit:'0123456789abcdef0123456789abcdef01234567',
                source_dirty:false};
              globalThis.__mdkrOnlineControlReleasePolicy=Object.freeze({
                enabled:true,serviceOrigin:location.origin});
              onlineBuildInfo=clean;
              validatedOnlineRomBuild='us.v80';
              romBytes=new Uint8Array([1]);
              const first=await publishOnlineCompatibility(true);
              const ntsc=MDKROnlineRoom.compatibility();
              const scriptsAfterFirst=[...document.scripts].filter(script=>
                script.src.includes('mdkr-online-tools')).length;
              const second=await publishOnlineCompatibility(true);
              const scriptsAfterSecond=[...document.scripts].filter(script=>
                script.src.includes('mdkr-online-tools')).length;
              onlineBuildInfo={...clean,source_dirty:true};
              const dirty=await publishOnlineCompatibility(true);
              const dirtyEnabled=MDKROnlineRoom.enabled();
              onlineBuildInfo=clean;
              validatedOnlineRomBuild='pal.v80';
              const pal=await publishOnlineCompatibility(true);
              const palCompatibility=MDKROnlineRoom.compatibility();
              MDKROnlineRoom.disable();
              globalThis.__mdkrOnlineControlReleasePolicy=Object.freeze({
                enabled:true,serviceOrigin:'https://example.invalid'});
              const crossOrigin=await publishOnlineCompatibility(true);
              return {first,second,dirty,dirtyEnabled,pal,crossOrigin,ntsc,
                palCompatibility,scriptsAfterFirst,scriptsAfterSecond,
                apiFetches:globalThis.__activationFetches.filter(url=>
                  String(url).includes('/api/'))};
            })()""", await_promise=True)
            ntsc = result["ntsc"]
            pal = result["palCompatibility"]
            require(result["first"] and result["second"] and result["pal"] and
                    not result["dirty"] and not result["dirtyEnabled"] and
                    not result["crossOrigin"] and not result["apiFetches"],
                    f"activation fail-closed contract drifted: {result}")
            require(result["scriptsAfterFirst"] == 1 and
                    result["scriptsAfterSecond"] == 1,
                    f"Online Room model was not loaded exactly once: {result}")
            require(ntsc["protocolVersion"] == 1 and
                    ntsc["romRevision"] == 1 and ntsc["cadenceHz"] == 30 and
                    len(ntsc["buildId"]) == 16 and
                    len(ntsc["gameplayDigest"]) == 32 and
                    any(ntsc["buildId"]) and any(ntsc["gameplayDigest"]),
                    f"NTSC compatibility manifest is malformed: {ntsc}")
            require(pal["romRevision"] == 2 and pal["cadenceHz"] == 25 and
                    pal["buildId"] == ntsc["buildId"] and
                    pal["gameplayDigest"] == ntsc["gameplayDigest"],
                    f"PAL compatibility handoff drifted: {pal}")
            require(not cdp.failures,
                    "browser/CDP failures: " + "; ".join(cdp.failures))
            print("check_browser_online_activation: PASS — clean provenance + "
                  "local ROM activation, idempotence, NTSC/PAL mapping, dirty/cross-origin refusal")
        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()
            server.close()


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
    except (CheckFailure, OSError, ValueError) as error:
        print(f"check_browser_online_activation: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
