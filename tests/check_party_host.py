#!/usr/bin/env python3
"""Exercise launcher-owned phone pairing and approval UX in real Chromium."""

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


def resolve(value: str) -> Path:
    candidate = Path(value).expanduser()
    if not candidate.is_absolute():
        candidate = ROOT / candidate
    return candidate.resolve()


def run(args: argparse.Namespace) -> None:
    shell = resolve(args.shell_dir)
    for relative in ("index.html", "party/party-host.css", "party/party-host.js",
                     "party/qrcodegen.js"):
        require((shell / relative).is_file(), f"party host artifact missing: {relative}")
    server = OverlayServer(shell, shell)
    server.start()
    with tempfile.TemporaryDirectory(prefix="mdkr64_party_host_") as profile:
        chrome = ChromeProcess(find_chrome(args.chrome), Path(profile),
                               args.chrome_flag, args.verbose)
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Inspector", "Accessibility"):
                cdp.call(f"{domain}.enable")
            config = {
                "initialRoomState": {"type": "room_state", "transitionId": 1,
                                     "controllers": []},
            }
            source = """
              let partyInviteGeneration = 1;
              let partyTransition = 100;
              globalThis.__mdkrPartyHostTestConfig = {
                initialRoomState: %s,
                async request(path) {
                  if (path === '/api/party/create') return {
                    roomId:'abcdefghijklmnopqrstuv', hostCredential:'H'.repeat(43),
                    fallbackCode:'123456', inviteGeneration:1, inviteExpiresInMs:120000,
                    controllerUrl:location.origin+'/controller/#'+'A'.repeat(43)
                  };
                  if (path.endsWith('/rotate')) {
                    if (globalThis.__partyHoldRotate) {
                      globalThis.__partyRotateStarted=true;
                      await new Promise(resolve=>{globalThis.__partyReleaseRotate=resolve;});
                    }
                    partyInviteGeneration++;
                    const current=globalThis.MDKRPartyHost.state().room;
                    globalThis.MDKRPartyHost.applyRoomState({
                      transitionId:++partyTransition,
                      inviteGeneration:partyInviteGeneration,
                      controllers:current.controllers || []
                    });
                    return {
                      fallbackCode:'654321', inviteGeneration:partyInviteGeneration,
                      inviteExpiresInMs:120000,
                      controllerUrl:location.origin+'/controller/#'+'B'.repeat(43)
                    };
                  }
                  if (path.endsWith('/revoke')) return {
                    ok:true, transitionId:++partyTransition,
                    inviteGeneration:++partyInviteGeneration
                  };
                  return {ok:true};
                }
              };
            """ % json.dumps(config["initialRoomState"], separators=(",", ":"))
            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": source})
            cdp.call("Emulation.setDeviceMetricsOverride", {
                "width": 390, "height": 844, "deviceScaleFactor": 2, "mobile": True,
            })
            cdp.call("Page.navigate", {"url": server.origin + "/"})
            wait_value(cdp, "Boolean(globalThis.MDKRPartyHost)", bool,
                       "party host API", args.timeout)
            # publishPartyRomReady() derives the party room's romReady from the
            # same ROM availability that ungates the launcher Play button, so a
            # real ready launcher never has Play disabled. Forcing only the
            # party half would leave Play disabled, and click() on a disabled
            # button dispatches nothing -- Start local game could not hand off.
            cdp.evaluate("globalThis.MDKRPartyHost.setRomReady(true);"
                         "document.getElementById('play').disabled=false;"
                         "globalThis.MDKRPartyHost.open()")
            ready = wait_value(cdp, """(() => ({
              room: !document.getElementById('party-room').hidden,
              code: document.getElementById('party-code').textContent,
              qr: document.getElementById('party-qr').width,
              dialog: document.getElementById('party-dialog').open
            }))()""", lambda value: isinstance(value, dict) and value.get("room"),
                "pairing sheet", args.timeout)
            require(ready["dialog"] and ready["code"] == "123 456" and ready["qr"] > 200,
                    f"pairing invite did not render: {ready}")

            sas = cdp.evaluate("""(async () => {
              const host=await MDKRPartySas.createIdentity();
              const phone=await MDKRPartySas.createIdentity();
              const hostFingerprint=MDKRPartySas.sdpFingerprint(
                'a=fingerprint:sha-256 ab:cd:ef:01\\r\\n');
              const transcript={roomId:'abcdefghijklmnopqrstuv',
                hostPublicKey:host.publicKey,controllerPublicKey:phone.publicKey,
                hostFingerprint,
                controllerFingerprint:'sha-256 23:45:67:89'};
              const a=await MDKRPartySas.phrase(host.privateKey,phone.publicKey,transcript);
              const b=await MDKRPartySas.phrase(phone.privateKey,host.publicKey,transcript);
              const refused=await MDKRPartySas.phrase(host.privateKey,phone.publicKey,
                {...transcript,controllerFingerprint:''}).then(()=>false,()=>true);
              return {a,b,refused,hostFingerprint,
                hostLength:host.publicKey.length,phoneLength:phone.publicKey.length};
            })()""", await_promise=True)
            require(sas["a"] == sas["b"] and sas["hostLength"] == 87 and
                    sas["phoneLength"] == 87 and len(sas["a"].split()) == 2 and
                    sas["hostFingerprint"] == "sha-256 AB:CD:EF:01" and
                    sas["refused"] is True,
                    f"ECDH v2 pairing phrase mismatch: {sas}")

            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:2, controllers:[{
                controllerId:'phone-one', name:'Sam’s phone',
                phase:'pending', seat:null, leaseGeneration:0, connectionSequence:1
              }]});""")
            pending = wait_value(cdp, """(() => ({
              text: document.getElementById('party-pending-list').textContent,
              buttons: document.querySelectorAll('#party-pending-list button').length,
              approveDisabled: document.querySelector(
                '#party-pending-list .btn-primary').disabled,
              seat: document.querySelector('#party-pending-list select').value
            }))()""", lambda value: isinstance(value, dict) and value.get("buttons") == 2,
                "pending approval", args.timeout)
            # v2 ritual: the phrase binds the direct channel, so a pending
            # phone shows the placeholder and Approve is NOT phrase-gated.
            require("Sam’s phone" in pending["text"] and
                    "Phrase appears when the phone connects." in pending["text"] and
                    pending["approveDisabled"] is False and
                    pending["seat"] == "2",
                    f"identity/placeholder absent from approval: {pending}")
            cdp.evaluate("document.querySelector('#party-pending-list .btn-primary').click()")
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/approve'))",
                bool, "approve request", args.timeout)
            approved_request = cdp.evaluate("""(() =>
              globalThis.__mdkrPartyHostTestState.requestDetails.find(
                entry => entry.path.endsWith('/approve')))()""")
            require(approved_request["body"].get("seat") == 2,
                    f"host-selected free seat was not sent: {approved_request}")

            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:3, controllers:[{
                controllerId:'phone-one', name:'Sam’s phone',
                phase:'leased', seat:2, leaseGeneration:1, connectionSequence:1
              }]});""")
            seat = wait_value(cdp, """(() => ({
              ready: document.querySelector('[data-seat="2"]').dataset.ready,
              label: document.querySelector('[data-seat="2"] small').textContent,
              startDisabled: document.getElementById('party-start').disabled
            }))()""", lambda value: isinstance(value, dict) and value.get("ready") == "true",
                "approved seat", args.timeout)
            require(not seat["startDisabled"] and seat["label"] == "Phone reconnecting — neutral",
                    f"approved controller did not enable start: {seat}")

            cdp.evaluate("""(() => {
              globalThis.__partyQrEncode = qrcodegen.QrCode.encodeText;
              qrcodegen.QrCode.encodeText = () => { throw new Error('qr fixture failure'); };
              document.getElementById('party-extend').click();
            })()""")
            qr_fallback = wait_value(cdp, """(() => ({
              code:document.getElementById('party-code').textContent,
              hidden:document.querySelector('.party-qr-wrap').hidden,
              step:document.getElementById('party-scan-step').textContent,
              install:document.getElementById('party-install-copy').textContent
            }))()""", lambda value: isinstance(value, dict) and
                value.get("code") == "654 321", "rotated QR fallback", args.timeout)
            require(qr_fallback["hidden"] and "/controller/" in qr_fallback["install"] and
                    "Open this site" in qr_fallback["step"],
                    f"QR failure did not preserve manual code recovery: {qr_fallback}")
            cdp.evaluate("qrcodegen.QrCode.encodeText = globalThis.__partyQrEncode")
            cdp.call("Emulation.setPageScaleFactor", {"pageScaleFactor": 2})
            layout = cdp.evaluate("({width:document.documentElement.scrollWidth, viewport:innerWidth})")
            require(layout["width"] <= layout["viewport"], f"200% party sheet overflow: {layout}")

            cdp.evaluate("document.getElementById('party-close').click()")
            wait_value(cdp, "!document.getElementById('party-dialog').open", bool,
                       "closed sheet", args.timeout)
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/revoke'))",
                bool, "dismissed launcher invite revocation", args.timeout)
            revoked_presentation = cdp.evaluate("""(() => ({
              room:globalThis.MDKRPartyHost.state().room,
              code:document.getElementById('party-code').textContent,
              qrWidth:document.getElementById('party-qr').width,
              qrHidden:document.querySelector('.party-qr-wrap').hidden,
              expiry:document.getElementById('party-expiry').textContent
            }))()""")
            require(revoked_presentation["room"] and
                    "fallbackCode" not in revoked_presentation["room"] and
                    "controllerUrl" not in revoked_presentation["room"] and
                    revoked_presentation["code"] == "—— —— ——" and
                    revoked_presentation["qrWidth"] == 1 and
                    revoked_presentation["qrHidden"] and
                    revoked_presentation["expiry"] == "Invite is not displayed",
                    f"dismissed invite remained in launcher/DOM custody: {revoked_presentation}")
            require(cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)"),
                    "closing setup unexpectedly ended approved controller leases")

            cdp.evaluate("""(() => {
              try {
                document.getElementById('stage').hidden=false;
                globalThis.MDKRPartyHost.open();
                globalThis.__partyOpenDebug={ok:true};
              } catch (error) {
                globalThis.__partyOpenDebug={ok:false,error:String(error),stack:error.stack};
              }
            })()""")
            wait_value(cdp, """({
              open:globalThis.__mdkrPartyOverlayOpen,
              stageHidden:document.getElementById('stage').hidden,
              dialog:document.getElementById('party-dialog').open,
              debug:globalThis.__partyOpenDebug,
              ready:globalThis.MDKRPartyHost.state().romReady,
              room:Boolean(globalThis.MDKRPartyHost.state().room),
              api:String(globalThis.MDKRPartyHost.open),
              triggerDisabled:document.getElementById('add-phone-controllers').disabled,
              lifecycle:globalThis.__mdkrPartyHostTestState.lifecycle.slice()
            })""", lambda value: isinstance(value, dict) and value.get("open") is True,
                       "in-game local pause latch", args.timeout)
            wait_value(cdp, "Boolean(globalThis.MDKRPartyHost.state().room)", bool,
                       "in-game controller room", args.timeout)
            wait_value(cdp,
                "globalThis.MDKRPartyHost.state().room.inviteGeneration >= 4",
                bool, "reopened invite generation after revoke", args.timeout)
            cdp.evaluate("document.getElementById('party-close').click()")
            wait_value(cdp, "!globalThis.__mdkrPartyOverlayOpen", bool,
                       "in-game local pause release", args.timeout)
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/revoke'))",
                bool, "dismissed invite revocation", args.timeout)
            preserved = cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)")
            require(preserved, "in-game dismissal destroyed the controller room")

            cdp.evaluate("""(() => {
              document.getElementById('stage').hidden=true;
              globalThis.MDKRPartyHost.open();
            })()""")
            wait_value(cdp, "document.getElementById('party-dialog').open", bool,
                       "room reopened for launcher start", args.timeout)
            wait_value(cdp,
                "globalThis.MDKRPartyHost.state().room.inviteGeneration >= 6",
                bool, "second reopen correlated after revoke", args.timeout)
            cdp.evaluate("""(() => {
              globalThis.__partyPlayClicked=false;
              document.getElementById('play').addEventListener('click', event => {
                event.preventDefault();
                event.stopImmediatePropagation();
                globalThis.__partyPlayClicked=true;
              }, {capture:true, once:true});
              document.getElementById('party-start').click();
            })()""")
            started = wait_value(cdp, """(() => ({
              played:globalThis.__partyPlayClicked,
              dialog:document.getElementById('party-dialog').open,
              room:globalThis.MDKRPartyHost.state().room,
              code:document.getElementById('party-code').textContent,
              qrHidden:document.querySelector('.party-qr-wrap').hidden,
              revokes:globalThis.__mdkrPartyHostTestState.requests
                .filter(path=>path.endsWith('/revoke')).length
            }))()""", lambda value: isinstance(value, dict) and
                value.get("played") is True and value.get("dialog") is False and
                value.get("revokes", 0) >= 3,
                "launcher start invite revocation", args.timeout)
            require(started["room"] and "fallbackCode" not in started["room"] and
                    "controllerUrl" not in started["room"] and
                    started["code"] == "—— —— ——" and started["qrHidden"],
                    f"starting local play retained invite custody: {started}")

            cdp.evaluate("""(() => {
              document.getElementById('stage').hidden=false;
              globalThis.MDKRPartyHost.open();
            })()""")
            wait_value(cdp, "document.getElementById('party-dialog').open", bool,
                       "room reopened for explicit end", args.timeout)
            wait_value(cdp,
                "globalThis.MDKRPartyHost.state().room.inviteGeneration >= 8",
                bool, "post-start reopen correlated after revoke", args.timeout)
            rotate_generations = cdp.evaluate("""globalThis.__mdkrPartyHostTestState
              .requestDetails.filter(entry=>entry.path.endsWith('/rotate'))
              .map(entry=>entry.body.expectedInviteGeneration)""")
            require(rotate_generations == [1, 3, 5, 7],
                    f"rotate requests lost revoke/publication correlation: {rotate_generations}")

            removal_requests_before = cdp.evaluate("""globalThis.__mdkrPartyHostTestState
              .requests.filter(path=>path.endsWith('/remove')).length""")
            cdp.evaluate("document.querySelector('[data-seat=\"2\"] .party-seat-remove').click()")
            removal_confirmation = wait_value(cdp, """(() => ({
              open:document.getElementById('party-remove-dialog').open,
              focus:document.activeElement?.id,
              copy:document.getElementById('party-remove-copy').textContent,
              requests:globalThis.__mdkrPartyHostTestState.requests
                .filter(path=>path.endsWith('/remove')).length
            }))()""", lambda value: isinstance(value, dict) and
                value.get("open") is True, "remove-phone confirmation", args.timeout)
            require(removal_confirmation["focus"] == "party-remove-cancel" and
                    "Sam’s phone" in removal_confirmation["copy"] and
                    "Controller 2" in removal_confirmation["copy"] and
                    removal_confirmation["requests"] == removal_requests_before,
                    f"phone removal was not safe and specific: {removal_confirmation}")
            cdp.evaluate("document.getElementById('party-remove-cancel').click()")
            wait_value(cdp, "!document.getElementById('party-remove-dialog').open", bool,
                       "cancelled phone removal", args.timeout)
            require(cdp.evaluate("""globalThis.__mdkrPartyHostTestState.requests
                    .filter(path=>path.endsWith('/remove')).length""") ==
                    removal_requests_before,
                    "cancelling phone removal mutated the room")
            # The dialog's `close` event, and therefore the focus return, is
            # queued after `open` flips to false. Wait for the same condition
            # instead of sampling the one task in between.
            wait_value(cdp,
                       "document.activeElement?.classList.contains('party-seat-remove')",
                       lambda value: value is True,
                       "cancelled phone removal restored focus", args.timeout)

            cdp.evaluate("""(() => {
              document.querySelector('[data-seat="2"] .party-seat-remove').click();
              document.getElementById('party-remove-confirm').click();
            })()""")
            wait_value(cdp, """globalThis.__mdkrPartyHostTestState.requests
              .filter(path=>path.endsWith('/remove')).length""",
              lambda value: value == removal_requests_before + 1,
              "confirmed phone removal", args.timeout)
            cdp.evaluate("""globalThis.MDKRPartyHost.applyRoomState({
              type:'room_state', transitionId:++partyTransition,
              inviteExpiresAt:Date.now()+120000,
              inviteGeneration:partyInviteGeneration, phase:'open', controllers:[]
            })""")
            removed_seat = wait_value(cdp, """(() => ({
              hidden:document.querySelector('[data-seat="2"] .party-seat-remove').hidden,
              label:document.querySelector('[data-seat="2"] small').textContent,
              focusSeat:document.activeElement?.dataset?.seat
            }))()""", lambda value: isinstance(value, dict) and
                value.get("hidden") is True, "removed phone seat", args.timeout)
            require(removed_seat["label"] == "Available" and
                    removed_seat["focusSeat"] == "2",
                    f"confirmed removal did not neutralize/focus safely: {removed_seat}")

            cdp.evaluate("""(() => {
              globalThis.__partyHoldRotate=true;
              globalThis.__partyRotateStarted=false;
              document.getElementById('party-extend').click();
            })()""")
            wait_value(cdp, "globalThis.__partyRotateStarted", bool,
                       "page-bound invite rotation", args.timeout)
            bfcache = cdp.evaluate("""(async () => {
              dispatchEvent(new PageTransitionEvent('pagehide', {persisted:true}));
              globalThis.__partyHoldRotate=false;
              globalThis.__partyReleaseRotate();
              await new Promise(resolve=>setTimeout(resolve, 0));
              const hidden={room:globalThis.MDKRPartyHost.state().room,
                code:document.getElementById('party-code').textContent,
                qr:document.getElementById('party-qr').width,
                pads:globalThis.MDKRPartyHost.remotePads().map(p=>({
                  active:p.active,reserved:p.reserved,packets:p.packets.length}))};
              dispatchEvent(new PageTransitionEvent('pageshow', {persisted:true}));
              return {hidden,lifecycle:
                globalThis.__mdkrPartyHostTestState.lifecycle.slice(-2)};
            })()""", await_promise=True)
            require("fallbackCode" not in bfcache["hidden"]["room"] and
                    "controllerUrl" not in bfcache["hidden"]["room"] and
                    bfcache["hidden"]["code"] == "—— —— ——" and
                    bfcache["hidden"]["qr"] == 1 and
                    not any(pad["active"] or pad["reserved"] or pad["packets"]
                            for pad in bfcache["hidden"]["pads"]) and
                    bfcache["lifecycle"] == ["pagehide", "pageshow:persisted"],
                    f"BFCache lifecycle revived invite/input custody: {bfcache}")

            cdp.evaluate("document.getElementById('party-end').click()")
            confirmation = wait_value(cdp, """(() => ({
              open:document.getElementById('party-end-dialog').open,
              room:Boolean(globalThis.MDKRPartyHost.state().room),
              closeSent:globalThis.__mdkrPartyHostTestState.requests
                .some(path=>path.endsWith('/close')),
              focus:document.activeElement?.id
            }))()""", lambda value: isinstance(value, dict) and
                value.get("open") is True, "end-room confirmation", args.timeout)
            require(confirmation["room"] and not confirmation["closeSent"] and
                    confirmation["focus"] == "party-end-cancel",
                    f"end-room confirmation was not safe by default: {confirmation}")
            cdp.evaluate("document.getElementById('party-end-cancel').click()")
            wait_value(cdp, "!document.getElementById('party-end-dialog').open", bool,
                       "cancelled end-room confirmation", args.timeout)
            require(cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)"),
                    "cancelling end-room confirmation destroyed the room")
            cdp.evaluate("""(() => {
              document.getElementById('party-end').click();
              document.getElementById('party-end-confirm').click();
            })()""")
            wait_value(cdp,
                "globalThis.__mdkrPartyHostTestState.requests.some(p=>p.endsWith('/close'))",
                bool, "explicit room close request", args.timeout)
            require(not cdp.evaluate("Boolean(globalThis.MDKRPartyHost.state().room)"),
                    "End controller room did not clear the room")
            require(not cdp.failures, "browser/CDP failures: " + "; ".join(cdp.failures))
            fatal = [line for line in cdp.console if "Uncaught" in line or "TypeError" in line]
            require(not fatal, "party host console errors: " + "; ".join(fatal))
            print("check_party_host: PASS — mixed-source seats, safe phone removal, QR/code "
                  "fallback, dismiss/start revoke/preserve, confirmed close and 200% layout")
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
        print(f"check_party_host: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
