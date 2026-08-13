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


def source_contract() -> None:
    shell = (ROOT / "dist/web/mdkr64-shell.js").read_text(encoding="utf-8")
    native = (ROOT / "platform/online/compatibility_identity.c").read_text(
        encoding="utf-8")
    browser_room = (ROOT / "dist/web/online/online-room.js").read_text(
        encoding="utf-8")
    browser_presenter = (
        ROOT / "dist/web/online/online-room-presenter.js"
    ).read_text(encoding="utf-8")
    browser_live_state = (
        ROOT / "dist/web/online/online-room-live-state.js"
    ).read_text(encoding="utf-8")
    browser_html = (ROOT / "dist/web/index.html").read_text(encoding="utf-8")
    browser_css = (ROOT / "dist/web/online/online-room.css").read_text(
        encoding="utf-8")
    browser_abi = (ROOT / "platform/online/lobby_browser_wasm.c").read_text(
        encoding="utf-8")
    browser_abi_header = (
        ROOT / "platform/online/lobby_browser_wasm.h"
    ).read_text(encoding="utf-8")
    view_model = (ROOT / "platform/online/lobby_view_model.c").read_text(
        encoding="utf-8")
    app_host = (ROOT / "platform/app/main_app.cpp").read_text(encoding="utf-8")
    require('info.version.length <= 32' in shell and
            'bounded_length(version, 32u)' in native,
            "native/browser release-version bound is not exact")
    for domain in ("online-build", "gameplay-contract", "rollback=bounded-v1"):
        require(domain in shell and domain in native,
                f"native/browser compatibility domain drifted: {domain}")
    require("MDKR_ONLINE_BROWSER_ABI_VERSION 4u" in browser_abi_header and
            "mdkr_online_browser_verification_phrase" in browser_abi and
            "api.version() !== 4" in browser_room,
            "browser phrase ABI/version handshake is incomplete")
    require('id="online-room-verification"' in browser_html and
            'aria-labelledby="online-room-verification-title"' in browser_html and
            'id="online-room-verification-phrase" translate="no"' in browser_html and
            "overflow-wrap: anywhere" in browser_css,
            "browser phrase semantics or narrow-screen containment regressed")
    require("presenter.project(model" in browser_room and
            "presenter.liveAction(currentPresentation, action" in browser_room and
            'route === "check_setup" && options.fixture !== true' in
            browser_presenter and
            'select: (value) => testConfig ? selectCase(value) : false' in
            browser_room and
            "model.verificationPhrase" in browser_presenter and
            "Do not continue if even 1 word differs." in browser_presenter and
            '"Words Match"' in view_model and
            '"Words Differ"' in view_model and
            "MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH" in view_model and
            "failure == MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH" in
            browser_abi and
            "MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH" in app_host,
            "phrase rendering, warning, confirmation or native action gate is incomplete")
    require("liveState.ingest(value, priorRoom, priorInvite" in browser_room and
            'testConfig?.liveFixture === true' in browser_room and
            '["127.0.0.1", "::1", "localhost"].includes(location.hostname)' in
            browser_room and
            "const testState = testConfig" in browser_room and
            "bodyKeys:" in browser_room and
            "testState.requests.push({path, body:" not in browser_room and
            'if (type === "leave" || type === "close") return true;' in
            browser_room and
            'if (code === "host_closed" || code === "not_found") forgetLiveRoom();' in
            browser_room and
            'if (liveRoom.lobby.phase === "closed")' in browser_room and
            'value.closedReason === "host_closed" ? 14' in browser_live_state and
            "if (refreshed) showAuxiliary(\"The Room Changed\"" in browser_room and
            "liveRoom = priorRoom;" in browser_room and
            "scheduleLiveInviteExpiry();" in browser_room and
            'heading.textContent = "Invitation Expired"' in browser_room and
            'replace.textContent = "Create New Invitation"' in browser_room and
            "if (!liveInviteReplacementAllowed())" in browser_room and
            "liveInvite = null;" in browser_room and
            "now < liveInvite.expiresAt" in browser_room and
            "exactKeys(value, CANONICAL_STATE_KEYS)" in browser_live_state and
            "validWireKeys(value)" in browser_live_state and
            "priorInvite.inviteGeneration === state.inviteGeneration" in
            browser_live_state and
            "publicationRelation(priorState, state)" in browser_live_state and
            "expectedInviteResponseGeneration !== state.inviteGeneration" in
            browser_live_state and
            "mergeLiveState(value, responseGeneration)" in browser_room and
            "const maxLiveResponseBytes = 64 * 1024" in browser_room and
            "response.body.getReader()" in browser_room and
            "await reader.cancel()" in browser_room and
            "new TextEncoder().encode(event.data).byteLength" in browser_room and
            'socket.close(1009, "invalid_match_state")' in browser_room and
            'socket.close(1008, "invalid_match_state")' in browser_room and
            "socketGeneration !== liveSocketGeneration" in browser_room and
            "function failLiveSocketSetup(operation, socketGeneration)" in
            browser_room and
            'typeof subscription.close !== "function"' in browser_room and
            "return failLiveSocketSetup(operation, socketGeneration);" in
            browser_room and
            "if (liveSocketAttempt >= 5)" in browser_room and
            "markLiveSocketStable(socketGeneration)" in browser_room and
            "}, 30_000);" in browser_room and
            "liveSocketAttempt = 0;" in browser_room and
            "Math.floor(value.inviteExpiresInMs / 20)" in browser_live_state and
            "expiresAt: localExpiresAt" in browser_live_state and
            "last.leaderEndpointId === lobby.leaderEndpointId" in browser_live_state and
            "const phaseMatches" in browser_live_state and
            "inviteState < 0 || inviteState > 2" in browser_live_state and
            "Object.freeze({obsolete: false, state, invite})" in
            browser_live_state and
            "if (qrReady) auxiliary.append(canvas)" in browser_room,
            "live snapshot atomicity, stale-action, phase or invite handling is incomplete")
    room_entry = (ROOT / "dist/web/room/room-entry.js").read_text(
        encoding="utf-8")
    require(browser_room.index("const entryFragment = (() =>") <
            browser_room.index("const byId = (id)") and
            'location.replace(location.pathname + location.search);\n' in
            browser_room and
            'return {attempted: true, capability: "", scrubbed: false};' in
            browser_room and
            "if (!entryFragment.scrubbed) return;" in browser_room and
            "pendingEntryCapability" in browser_room and
            "mdkr-online-room-entry-v1" not in browser_room and
            "Private Rooms Aren’t Enabled in This Build" in browser_room and
            "if (location.hash)" in room_entry and
            "location.replace(location.pathname + location.search);" in
            room_entry and
            "location.replace(`../#match=${capability}`)" in room_entry,
            "fragment-only private-room handoff or disabled-build custody regressed")


def run(args: argparse.Namespace) -> None:
    shell = resolve(args.shell_dir)
    for relative in ("index.html", "online/online-control-config.js",
                     "online/online-room-live-state.js",
                     "online/online-room-presenter.js", "online/online-room.js",
                     "mdkr-online-tools.js",
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

            secret = "A" * 43
            cdp.call("Page.navigate", {"url": server.origin + "/#match=" + secret})
            disabled_invite = wait_value(cdp, """(() => ({
              ready:document.readyState==='complete' && Boolean(globalThis.MDKROnlineRoom),
              title:document.getElementById('online-room-title')?.textContent,
              open:document.getElementById('online-room-dialog')?.open,
              hash:location.hash,
              staged:sessionStorage.getItem('mdkr-online-room-entry-v1'),
              api:performance.getEntriesByType('resource').some(entry=>
                new URL(entry.name).pathname.startsWith('/api/'))}))()""",
                lambda value: isinstance(value, dict) and value.get("ready") and
                    value.get("title") == "Private Rooms Aren’t Enabled in This Build",
                "disabled-build invite custody", args.timeout)
            require(disabled_invite["open"] and disabled_invite["hash"] == "" and
                    disabled_invite["staged"] is None and not disabled_invite["api"],
                    f"disabled build retained or used invite authority: {disabled_invite}")

            result = cdp.evaluate("""(async()=>{
              const originalFetch=globalThis.fetch.bind(globalThis);
              globalThis.__activationFetches=[];
              globalThis.fetch=(input,...rest)=>{
                const url=typeof input==='string'?input:(input?.url||String(input));
                globalThis.__activationFetches.push(url);
                return originalFetch(input,...rest);
              };
              const clean={version:'1.3.0',
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
              onlineBuildInfo={...clean,
                version:'12345678901234567890123456789.0.0'};
              const overlong=await publishOnlineCompatibility(true);
              const overlongEnabled=MDKROnlineRoom.enabled();
              onlineBuildInfo=clean;
              validatedOnlineRomBuild='pal.v80';
              const pal=await publishOnlineCompatibility(true);
              const palCompatibility=MDKROnlineRoom.compatibility();
              MDKROnlineRoom.disable();
              globalThis.__mdkrOnlineControlReleasePolicy=Object.freeze({
                enabled:true,serviceOrigin:'https://example.invalid'});
              const crossOrigin=await publishOnlineCompatibility(true);
              return {first,second,dirty,dirtyEnabled,overlong,overlongEnabled,
                pal,crossOrigin,ntsc,
                palCompatibility,scriptsAfterFirst,scriptsAfterSecond,
                apiFetches:globalThis.__activationFetches.filter(url=>
                  String(url).includes('/api/'))};
            })()""", await_promise=True)
            ntsc = result["ntsc"]
            pal = result["palCompatibility"]
            require(result["first"] and result["second"] and result["pal"] and
                    not result["dirty"] and not result["dirtyEnabled"] and
                    not result["overlong"] and not result["overlongEnabled"] and
                    not result["crossOrigin"] and not result["apiFetches"],
                    f"activation fail-closed contract drifted: {result}")
            require(result["scriptsAfterFirst"] == 1 and
                    result["scriptsAfterSecond"] == 1,
                    f"Online Room model was not loaded exactly once: {result}")
            require(ntsc["protocolVersion"] == 1 and
                    ntsc["romRevision"] == 1 and ntsc["cadenceHz"] == 30 and
                    ntsc["buildId"] == list(bytes.fromhex(
                        "336892f0abf0a2c25c91a7ebcb266364")) and
                    ntsc["gameplayDigest"] == list(bytes.fromhex(
                        "fb34ef8ddfcf782a852375b8ce71d1bcd552a185cf2c18fe1e7727996b749522")),
                    f"NTSC compatibility manifest is malformed: {ntsc}")
            require(pal["romRevision"] == 2 and pal["cadenceHz"] == 25 and
                    pal["buildId"] == ntsc["buildId"] and
                    pal["gameplayDigest"] == ntsc["gameplayDigest"],
                    f"PAL compatibility handoff drifted: {pal}")
            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": """
              Object.defineProperty(History.prototype, 'replaceState', {
                configurable:true,
                value(){throw new DOMException('fixture denial', 'SecurityError');}
              });
            """})
            cdp.call("Page.navigate", {"url": server.origin +
                     "/room/#match=" + secret})
            role_scrub_failure = wait_value(cdp, """(() => ({
              ready:document.readyState==='complete',
              path:location.pathname,hash:location.hash,
              title:document.getElementById('room-entry-title')?.textContent,
              staged:sessionStorage.getItem('mdkr-online-room-entry-v1')}))()""",
                lambda value: isinstance(value, dict) and value.get("ready") and
                    value.get("path") == "/room/" and value.get("hash") == "",
                "role-page History API denial", args.timeout)
            require(role_scrub_failure["title"] == "Check the Room Invitation" and
                    role_scrub_failure["staged"] is None,
                    f"role page retained a capability after scrub denial: "
                    f"{role_scrub_failure}")
            cdp.call("Page.navigate", {"url": server.origin + "/#match=" + secret})
            root_scrub_failure = wait_value(cdp, """(() => ({
              ready:document.readyState==='complete' && Boolean(globalThis.MDKROnlineRoom),
              hash:location.hash,
              open:document.getElementById('online-room-dialog')?.open,
              staged:sessionStorage.getItem('mdkr-online-room-entry-v1'),
              api:performance.getEntriesByType('resource').some(entry=>
                new URL(entry.name).pathname.startsWith('/api/'))}))()""",
                lambda value: isinstance(value, dict) and value.get("ready") and
                    value.get("hash") == "",
                "launcher History API denial", args.timeout)
            require(not root_scrub_failure["open"] and
                    root_scrub_failure["staged"] is None and
                    not root_scrub_failure["api"],
                    f"launcher retained or redeemed a capability after scrub denial: "
                    f"{root_scrub_failure}")
            require(not cdp.failures,
                    "browser/CDP failures: " + "; ".join(cdp.failures))
            print("check_browser_online_activation: PASS — clean provenance + "
                  "local ROM activation, idempotence, NTSC/PAL mapping, "
                  "dirty/cross-origin/scrub refusal")
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
    parser.add_argument("--source-only", action="store_true",
                        help="validate native/browser identity source without Chrome")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        source_contract()
        if args.source_only:
            print("check_browser_online_activation: PASS — source-only exact "
                  "compatibility domains, version bound and phrase/action/AX contract")
            return 0
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError) as error:
        print(f"check_browser_online_activation: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
