#!/usr/bin/env python3
"""Exercise the launcher-owned browser binding over a MatchRoom-shaped adapter."""

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

FIXTURE = r"""
(() => {
  globalThis.__mdkrOnlineRoomTestConfig={liveFixture:true};
  const compatibility={protocolVersion:1,
    buildId:Array.from({length:16},(_,i)=>i+1),
    gameplayDigest:Array.from({length:32},(_,i)=>128+i),
    romRevision:1,cadenceHz:30};
  const host='100', guest='200';
  let fixtureNow=Date.now();
  Date.now=()=>fixtureNow;
  let callback=null, staleCallback=null, closed=null, subscriptions=0;
  let flapRemaining=0;
  let throwSubscribeRemaining=0;
  let inviteGeneration=1, code='123456';
  let nextInviteTtl=600000;
  let raceNextRotate=false;
  let loseLeadershipNextRotate=false;
  let closedReason=null;
  const roomExpiresAt=fixtureNow+86400000;
  let inviteExpiresAt=fixtureNow+600000;
  let lobby={protocolVersion:1,revision:1,matchEpoch:0,leaderGeneration:1,
    roomId:'42',leaderEndpointId:host,phase:'lobby',compatibility,
    members:[{endpointId:host,seatCount:1,connected:true,ready:false,loaded:false}],
    seats:[{endpointId:host,selectionRevision:0,voteTrack:null,localIndex:0,
      characterId:null,vehicleId:null}],selectedTrack:null,selectedVehicleMask:0};
  const snapshot=(extra={})=>({type:'match_state',schemaVersion:1,
    expiresAt:roomExpiresAt,inviteExpiresAt,
    inviteGeneration,closedReason,lobby:structuredClone(lobby),
    controlTail:[],...extra});
  const publish=()=>queueMicrotask(()=>callback?.(snapshot()));
  const joined=()=>{
    if (!lobby.members.some(item=>item.endpointId===guest)) {
      lobby.members.push({endpointId:guest,seatCount:1,connected:true,
        ready:false,loaded:false});
      lobby.seats.push({endpointId:guest,selectionRevision:0,voteTrack:null,
        localIndex:0,characterId:null,vehicleId:null});
      lobby.revision++;
      return true;
    }
    return false;
  };
  globalThis.__mdkrOnlineRoomLiveConfig={enabled:true,compatibility,
    async request(path,{body,credential}) {
      globalThis.__liveRequests.push({path,body:structuredClone(body),credential,
        hash:location.hash});
      if (path==='/api/match/create') return {...snapshot(),roomId:'roomroomroomroomroomro',
        endpointId:host,credential:'H'.repeat(43),fallbackCode:code,
        inviteExpiresInMs:600000,
        inviteUrl:location.origin+'/room/#match='+'A'.repeat(43)};
      if (path==='/api/match/join' || path==='/api/match/code') {
        joined();
        return {...snapshot(),roomId:'roomroomroomroomroomro',endpointId:guest,
          credential:'G'.repeat(43)};
      }
      if (path.endsWith('/state')) return snapshot();
      if (path.endsWith('/rotate')) {
        if (body.expectedInviteGeneration!==inviteGeneration) return {error:'invalid_state'};
        inviteGeneration++; code=inviteGeneration===2?'654321':'777888';
        const ttl=nextInviteTtl; nextInviteTtl=600000;
        inviteExpiresAt=Math.min(fixtureNow+ttl,roomExpiresAt);
        const response={...snapshot(),fallbackCode:code,
          inviteExpiresInMs:ttl,
          inviteUrl:location.origin+'/room/#match='+'B'.repeat(43)};
        if (raceNextRotate) {
          raceNextRotate=false;
          joined();
          callback?.(snapshot());
        }
        if (loseLeadershipNextRotate) {
          loseLeadershipNextRotate=false;
          joined(); lobby.leaderEndpointId=guest;
          lobby.leaderGeneration++; lobby.revision++;
          callback?.(snapshot());
        }
        return response;
      }
      if (path.endsWith('/command')) {
        if (body.expectedRevision!==lobby.revision) return {error:'stale_revision'};
        const seat=lobby.seats[Number(body.targetEndpointId)];
        const actor=credential?.startsWith('G')?guest:host;
        const member=lobby.members.find(item=>item.endpointId===actor);
        if (body.type==='set_character') { seat.characterId=body.value; seat.selectionRevision++; member.ready=false; }
        else if (body.type==='set_vehicle') { seat.vehicleId=body.value; seat.selectionRevision++; member.ready=false; }
        else if (body.type==='set_vote') { seat.voteTrack=body.value; member.ready=false; }
        else if (body.type==='set_ready') member.ready=body.value===1;
        else if (body.type==='leave') {
          lobby.members=lobby.members.filter(item=>item.endpointId!==actor);
          lobby.seats=lobby.seats.filter(item=>item.endpointId!==actor);
        } else if (body.type==='close') lobby.phase='closed';
        lobby.revision++; publish();
        return {accepted:true,duplicate:false,error:'ok',revision:lobby.revision};
      }
      return {error:'not_found'};
    },
    subscribe(_room,onState,onClose) {
      subscriptions++;
      if (throwSubscribeRemaining>0) {
        throwSubscribeRemaining--;
        throw new Error('fixture subscribe construction failed');
      }
      callback=onState; closed=onClose;
      if (flapRemaining>0) queueMicrotask(()=>{
        if (closed!==onClose) return;
        flapRemaining--; staleCallback=callback; callback=null; closed=null;
        onClose();
      });
      return {close(){ callback=null; closed=null; }};
    }};
  globalThis.__liveRequests=[];
  globalThis.__livePeerJoin=()=>{ const added=joined();
    const peer=lobby.members.find(item=>item.endpointId===guest);
    const seat=lobby.seats.find(item=>item.endpointId===guest);
    peer.ready=true; seat.characterId=1; seat.vehicleId=0; seat.voteTrack=3;
    seat.selectionRevision=2; if (!added) lobby.revision++; publish(); };
  globalThis.__liveDisconnect=()=>{ const value=closed; staleCallback=callback;
    callback=null; closed=null; value?.(); };
  globalThis.__liveStalePublish=()=>staleCallback?.({...snapshot(),unexpected:true});
  globalThis.__liveFlap=()=>{ flapRemaining=10; const value=closed;
    staleCallback=callback; callback=null; closed=null; value?.(); };
  globalThis.__liveStopFlap=()=>{ flapRemaining=0; };
  globalThis.__liveThrowNextSubscribe=()=>{ throwSubscribeRemaining=1;
    const value=closed; staleCallback=callback; callback=null; closed=null;
    value?.(); };
  globalThis.__liveExpireInvite=()=>{ fixtureNow=inviteExpiresAt; publish(); };
  globalThis.__liveUseShortInvite=()=>{ nextInviteTtl=200; };
  globalThis.__liveRaceNextRotate=()=>{ raceNextRotate=true; };
  globalThis.__liveLoseLeadershipNextRotate=()=>{
    loseLeadershipNextRotate=true;
  };
  globalThis.__liveAdvanceInviteClock=()=>{ fixtureNow=inviteExpiresAt; };
  globalThis.__liveGuestLeave=()=>{
    lobby.members=lobby.members.filter(item=>item.endpointId!==guest);
    lobby.seats=lobby.seats.filter(item=>item.endpointId!==guest);
    lobby.revision++; publish(); };
  globalThis.__liveCorrupt=()=>callback?.({...snapshot(),lobby:{...structuredClone(lobby),receipts:[]}});
  globalThis.__liveHostClose=()=>{ lobby.phase='closed'; lobby.revision++;
    closedReason='host_closed'; publish(); };
  globalThis.__liveResetSolo=()=>{
    inviteGeneration=1; code='123456'; nextInviteTtl=600000;
    inviteExpiresAt=fixtureNow+600000; closedReason=null;
    lobby={protocolVersion:1,revision:1,matchEpoch:0,leaderGeneration:1,
      roomId:'42',leaderEndpointId:host,phase:'lobby',compatibility,
      members:[{endpointId:host,seatCount:1,connected:true,ready:false,loaded:false}],
      seats:[{endpointId:host,selectionRevision:0,voteTrack:null,localIndex:0,
        characterId:null,vehicleId:null}],selectedTrack:null,selectedVehicleMask:0};
  };
  globalThis.__liveSubscriptions=()=>subscriptions;
})();
"""


def resolve(value: str) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else ROOT / path).resolve()


def click_action(cdp: CDPClient, action: int) -> None:
    result = cdp.evaluate(f"""(() => {{
      const button=document.querySelector('[data-online-action="{action}"]');
      if (!button || button.disabled) return {{clicked:false,
        buttons:[...document.querySelectorAll('[data-online-action]')]
          .map(item=>[item.dataset.onlineAction,item.textContent,item.disabled])}};
      button.click(); return {{clicked:true}};
    }})()""")
    require(result["clicked"], f"live action {action} was unavailable: {result}")


def run(args: argparse.Namespace) -> None:
    shell = resolve(args.shell_dir)
    for relative in ("index.html", "online/online-control-config.js",
                     "online/online-room-live-state.js",
                     "online/online-room-presenter.js", "online/online-room.js",
                     "online/online-room.css", "mdkr-online-tools.js",
                     "mdkr-online-tools.wasm", "room/index.html",
                     "room/room-entry.js", "room/room-entry.css"):
        require((shell / relative).is_file(), f"missing browser artifact: {relative}")
    server = OverlayServer(shell, shell)
    server.start()
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-live-room-profile-") as profile:
            chrome = ChromeProcess(find_chrome(args.chrome), Path(profile),
                                   args.chrome_flag, args.verbose)
            cdp: CDPClient | None = None
            try:
                cdp = CDPClient(page_websocket(chrome.wait_port()))
                for domain in ("Page", "Runtime", "Log", "Inspector", "Accessibility"):
                    cdp.call(f"{domain}.enable")
                cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": FIXTURE})
                cdp.call("Emulation.setDeviceMetricsOverride", {
                    "width": 960, "height": 720, "deviceScaleFactor": 1,
                    "mobile": False,
                })
                cdp.call("Page.navigate", {"url": server.origin + "/"})
                wait_value(cdp, "Boolean(globalThis.MDKROnlineRoom)", bool,
                           "live Online Room API", args.timeout)
                require(cdp.evaluate("MDKROnlineRoom.ready", await_promise=True) is True,
                        "live shared-C model failed to initialize")
                cdp.evaluate("MDKROnlineRoom.open()")
                wait_value(cdp, "document.activeElement.id",
                           lambda value: value == "online-room-title",
                           "live dialog focus", args.timeout)
                require(cdp.evaluate("document.getElementById('online-room-title').textContent")
                        == "Play Online", "live route did not start at the shared entry model")

                click_action(cdp, 2)
                role_recovery = cdp.evaluate("""(() => {
                  const input=document.querySelector('#online-room-auxiliary input');
                  input.value=location.origin+'/controller/#'+'C'.repeat(43);
                  [...document.querySelectorAll('#online-room-auxiliary button')]
                    .find(item=>item.textContent==='Join Room').click();
                  return document.getElementById('online-room-auxiliary').textContent;
                })()""")
                require("Check the Invite" in role_recovery and
                        "six-digit room code" in role_recovery,
                        f"wrong-role controller link had no recovery: {role_recovery}")

                click_action(cdp, 1)
                wait_value(cdp, "document.getElementById('online-room-title').textContent",
                           lambda value: value == "Private Room",
                           "created private room", args.timeout)
                require(cdp.evaluate("MDKROnlineRoom.current().admission") is False,
                        "live service data elevated local race admission")
                click_action(cdp, 3)
                invite = cdp.evaluate("""(() => ({
                  code:document.querySelector('.online-room-code')?.textContent,
                  width:document.querySelector('.online-room-qr')?.width,
                  replace:[...document.querySelectorAll('button')]
                    .some(item=>item.textContent==='Replace Invitation')
                }))()""")
                require(invite["code"] == "123 456" and invite["width"] > 0 and
                        invite["replace"],
                        f"online invite surface is incomplete: {invite}")
                qr_fallback = cdp.evaluate("""(() => {
                  const saved=globalThis.qrcodegen;
                  globalThis.qrcodegen=undefined;
                  document.querySelector('[data-online-action="3"]').click();
                  const value={qr:Boolean(document.querySelector('.online-room-qr')),
                    copy:document.getElementById('online-room-auxiliary').textContent,
                    code:document.querySelector('.online-room-code')?.textContent};
                  globalThis.qrcodegen=saved;
                  return value;
                })()""")
                require(not qr_fallback["qr"] and
                        qr_fallback["code"] == "123 456" and
                        "Share this room link or enter the code" in qr_fallback["copy"],
                        f"missing QR dependency had no usable fallback: {qr_fallback}")
                cdp.evaluate("__liveRaceNextRotate()")
                require(cdp.evaluate("""(() => { const b=[...document.querySelectorAll('button')]
                  .find(item=>item.textContent==='Replace Invitation'); b.click(); return true; })()"""),
                        "invite replacement control missing")
                wait_value(cdp, "document.querySelector('.online-room-code')?.textContent",
                           lambda value: value == "654 321", "rotated room invite", args.timeout)
                # The raced rotation deliberately left the guest in the room,
                # and the lobby view model gives "Friends Joined" priority
                # over every invite status once member_count >= 2 -- the
                # solo expiry statuses below are unreachable until the guest
                # leaves. As written this arm was unsatisfiable: the status
                # wait could never flip to "Invitation Expired".
                cdp.evaluate("__liveGuestLeave()")
                wait_value(cdp, "document.getElementById('online-room-model-status').textContent",
                           lambda value: value == "Invite Ready",
                           "solo room restored after raced-rotation guest", args.timeout)
                cdp.evaluate("__liveExpireInvite()")
                expired = wait_value(cdp, """(() => ({
                  status:document.getElementById('online-room-model-status').textContent,
                  oldSecret:Boolean(document.querySelector('.online-room-code, .online-room-qr')),
                  replacement:[...document.querySelectorAll('[data-online-action="3"]')]
                    .map(item=>({label:item.textContent,disabled:item.disabled}))}))()""",
                    lambda value: isinstance(value, dict) and
                        value.get("status") == "Invitation Expired",
                    "expired invitation recovery", args.timeout)
                require(not expired["oldSecret"] and expired["replacement"] == [{
                            "label": "New Invitation", "disabled": False}],
                        f"expired invitation leaked or lacked recovery: {expired}")
                cdp.evaluate("__liveUseShortInvite()")
                click_action(cdp, 3)
                wait_value(cdp, "document.querySelector('.online-room-code')?.textContent",
                           lambda value: value == "777 888",
                           "replacement after invite expiry", args.timeout)
                cdp.evaluate("__liveAdvanceInviteClock()")
                timed_expiry = wait_value(cdp, """(() => ({
                  heading:document.querySelector('#online-room-auxiliary h3')?.textContent,
                  oldSecret:Boolean(document.querySelector('.online-room-code, .online-room-qr')),
                  button:document.querySelector('#online-room-auxiliary button')?.textContent
                }))()""", lambda value: isinstance(value, dict) and
                    value.get("heading") == "Invitation Expired",
                    "receipt-relative invite timer", args.timeout)
                require(not timed_expiry["oldSecret"] and
                        timed_expiry["button"] == "Create New Invitation",
                        f"invite timer retained secret or lost recovery: {timed_expiry}")

                cdp.evaluate("__livePeerJoin()")
                wait_value(cdp, "document.getElementById('online-room-model-status').textContent",
                           lambda value: value == "Friends Joined",
                           "peer room publication", args.timeout)
                click_action(cdp, 4)
                wait_value(cdp, "MDKROnlineRoom.current().controls[0].action",
                           lambda value: value == 6, "character selection", args.timeout)
                for action, following in ((6, 7), (7, 8), (8, 9)):
                    click_action(cdp, action)
                    wait_value(cdp, "MDKROnlineRoom.current().controls[0].action",
                               lambda value, expected=following: value == expected,
                               f"selection transition {action}", args.timeout)
                click_action(cdp, 9)
                ready = wait_value(cdp, """(() => ({
                  status:document.getElementById('online-room-model-status').textContent,
                  actions:[...document.querySelectorAll('[data-online-action]')]
                    .map(item=>Number(item.dataset.onlineAction)),
                  admission:MDKROnlineRoom.current().admission}))()""",
                    lambda value: isinstance(value, dict) and value.get("status") == "Everyone Ready",
                    "everyone-ready non-admission state", args.timeout)
                require(11 not in ready["actions"] and ready["admission"] is False,
                        f"service snapshot exposed Start Race before GO: {ready}")

                before = cdp.evaluate("__liveSubscriptions()")
                cdp.evaluate("__liveDisconnect()")
                wait_value(cdp, "__liveSubscriptions()",
                           lambda value: value > before, "bounded state reconnect", args.timeout)
                stale_result = cdp.evaluate("""(() => {
                  const before=__mdkrOnlineRoomTestState.errors.length;
                  __liveStalePublish();
                  return {before,after:__mdkrOnlineRoomTestState.errors.length,
                    title:document.getElementById('online-room-title').textContent};
                })()""")
                require(stale_result["before"] == stale_result["after"] and
                        stale_result["title"] == "Choose Your Racers",
                        f"superseded subscription mutated current UX: {stale_result}")
                cdp.evaluate("__liveCorrupt()")
                wait_value(cdp, "document.getElementById('online-room-title').textContent",
                           lambda value: value == "Could Not Reach the Room",
                           "corrupt snapshot recovery", args.timeout)
                click_action(cdp, 14)
                wait_value(cdp, "document.getElementById('online-room-title').textContent",
                           lambda value: value == "Choose Your Racers",
                           "recovery from newest authenticated state", args.timeout)

                ax = cdp.call("Accessibility.getFullAXTree").get("nodes", [])
                names = {node.get("name", {}).get("value", "") for node in ax}
                folded = {name.casefold() for name in names}
                require("change selection" in folded and "leave room" in folded,
                        f"live room actions missing from accessibility tree: {sorted(names)}")
                flap_start = cdp.evaluate("__liveSubscriptions()")
                cdp.evaluate("__liveFlap()")
                wait_value(cdp, "document.getElementById('online-room-title').textContent",
                           lambda value: value == "Could Not Reach the Room",
                           "bounded socket-flap recovery", args.timeout)
                require(cdp.evaluate("__liveSubscriptions()") == flap_start + 5,
                        "state socket exceeded five automatic reconnect attempts")
                cdp.evaluate("__liveStopFlap()")
                click_action(cdp, 14)
                wait_value(cdp, "document.getElementById('online-room-title').textContent",
                           lambda value: value == "Choose Your Racers",
                           "explicit retry after socket-flap exhaustion", args.timeout)
                setup_failure_start = cdp.evaluate("__liveSubscriptions()")
                cdp.evaluate("__liveThrowNextSubscribe()")
                wait_value(cdp, "__liveSubscriptions()",
                           lambda value: value == setup_failure_start + 2,
                           "recovery from synchronous subscription failure",
                           args.timeout)
                wait_value(cdp, "document.getElementById('online-room-title').textContent",
                           lambda value: value == "Choose Your Racers",
                           "state restored after subscription setup failure",
                           args.timeout)
                # Invitation rotation is a lobby-phase operation: once the
                # room advances to SELECTING ("Choose Your Racers") the view
                # model never offers the replacement control (action 3), so
                # as previously sequenced this arm was unsatisfiable. Leave
                # through the room's own control (the public configure() API
                # refuses fixture configs -- trustedFixture is boot-only, so
                # a disable()/configure() rebuild can never come back), reset
                # the fixture to a fresh solo room, create it, let a friend
                # join, and race the rotation there.
                click_action(cdp, 24)
                wait_value(cdp, "document.getElementById('online-room-title').textContent",
                           lambda value: value == "Play Online",
                           "left the selecting room before the leadership arm",
                           args.timeout)
                cdp.evaluate("__liveResetSolo()")
                click_action(cdp, 1)
                wait_value(cdp, "document.getElementById('online-room-title').textContent",
                           lambda value: value == "Private Room",
                           "fresh lobby before leadership-loss rotation", args.timeout)
                cdp.evaluate("__livePeerJoin()")
                wait_value(cdp, "document.getElementById('online-room-model-status').textContent",
                           lambda value: value == "Friends Joined",
                           "peer joined leadership-arm lobby", args.timeout)
                cdp.evaluate("__liveLoseLeadershipNextRotate()")
                # Rotation is the invite panel's own "Replace Invitation"
                # button; strip action 3 with a READY invite only (re)shows
                # the panel, which is why priming the fixture and clicking 3
                # here never rotated anything.
                click_action(cdp, 3)
                require(cdp.evaluate("""(() => { const b=[...document.querySelectorAll('button')]
                  .find(item=>item.textContent==='Replace Invitation'); if (!b) return false;
                  b.click(); return true; })()"""),
                        "leadership-arm invite replacement control missing")
                leadership_loss = wait_value(cdp, """(() => ({
                  connection:Boolean(document.querySelector('[data-online-action="5"]')),
                  replacement:Boolean(document.querySelector('[data-online-action="3"]')),
                  secret:Boolean(document.querySelector('.online-room-code, .online-room-qr')),
                  auxiliary:document.getElementById('online-room-auxiliary').textContent
                }))()""", lambda value: isinstance(value, dict) and
                    value.get("connection") is True,
                    "leadership loss during invitation rotation", args.timeout)
                require(not leadership_loss["replacement"] and
                        not leadership_loss["secret"] and
                        "Create New Invitation" not in leadership_loss["auxiliary"],
                        f"former leader retained invite custody: {leadership_loss}")
                terminal_request_count = cdp.evaluate("__liveRequests.length")
                cdp.evaluate("__liveHostClose()")
                terminal = wait_value(cdp, """(() => ({
                  title:document.getElementById('online-room-title').textContent,
                  oldSecret:Boolean(document.querySelector('.online-room-code, .online-room-qr')),
                  actions:[...document.querySelectorAll('[data-online-action]')]
                    .map(item=>Number(item.dataset.onlineAction))}))()""",
                    lambda value: isinstance(value, dict) and
                        value.get("title") == "Room Ended",
                    "terminal host-close recovery", args.timeout)
                require(not terminal["oldSecret"] and 23 in terminal["actions"],
                        f"terminal room retained secret or lost Home: {terminal}")
                click_action(cdp, 23)
                wait_value(cdp, "document.getElementById('online-room-title').textContent",
                           lambda value: value == "Play Online",
                           "offline terminal return home", args.timeout)
                require(cdp.evaluate("__liveRequests.length") == terminal_request_count,
                        "terminal Return Home made a doomed network request")
                # Already home with the boot-time fixture still configured;
                # only the fixture's room state needs resetting. disable()
                # would be a one-way door: the public configure() API refuses
                # fixture configs (trustedFixture is boot-only).
                cdp.evaluate("__liveResetSolo()")
                click_action(cdp, 1)
                wait_value(cdp, "document.getElementById('online-room-title').textContent",
                           lambda value: value == "Private Room",
                           "fresh solo room before host close", args.timeout)
                solo_close_start = cdp.evaluate("__liveRequests.length")
                click_action(cdp, 24)
                wait_value(cdp, "document.getElementById('online-room-title').textContent",
                           lambda value: value == "Play Online",
                           "accepted solo host close", args.timeout)
                solo_close_requests = cdp.evaluate(
                    f"__liveRequests.slice({solo_close_start})")
                require([item["path"].rsplit("/", 1)[-1]
                         for item in solo_close_requests] == ["command"],
                        f"accepted host close made a doomed state request: "
                        f"{solo_close_requests}")
                requests = cdp.evaluate("__liveRequests.slice()")
                require(any(item["path"] == "/api/match/create" for item in requests) and
                        any(item["path"].endswith("/rotate") for item in requests) and
                        all(not item["credential"] for item in requests
                            if item["path"] in ("/api/match/create", "/api/match/join")),
                        f"live request ownership/scoping drifted: {requests}")
                resources = cdp.evaluate(
                    "performance.getEntriesByType('resource').map(item=>item.name)")
                require(not any("mdkr64_web.wasm" in item for item in resources),
                        "room control path loaded the game engine")
                require(not cdp.failures,
                        "browser/CDP failures: " + "; ".join(cdp.failures))
                fatal = [line for line in cdp.console
                         if "Uncaught" in line or "TypeError" in line]
                require(not fatal, "live room console errors: " + "; ".join(fatal))
            finally:
                if cdp is not None:
                    cdp.close()
                chrome.close()

        # A fragment link is consumed and erased before the first join request.
        # This uses a new browser process so no in-memory bearer from the create
        # path can make the result vacuously pass.
        with tempfile.TemporaryDirectory(prefix="mdkr-live-join-profile-") as profile:
            chrome = ChromeProcess(find_chrome(args.chrome), Path(profile),
                                   args.chrome_flag, args.verbose)
            cdp = None
            try:
                cdp = CDPClient(page_websocket(chrome.wait_port()))
                for domain in ("Page", "Runtime", "Log", "Inspector"):
                    cdp.call(f"{domain}.enable")
                cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": FIXTURE})
                secret = "A" * 43
                cdp.call("Page.navigate", {"url": server.origin +
                         "/room/#match=" + secret + "&extra=1"})
                invalid = wait_value(cdp, """(() => ({
                  title:document.getElementById('room-entry-title')?.textContent,
                  hash:location.hash,
                  staged:sessionStorage.getItem('mdkr-online-room-entry-v1')}))()""",
                    lambda value: isinstance(value, dict) and
                        value.get("title") == "Check the Room Invitation",
                    "exact room-fragment rejection", args.timeout)
                require(invalid["hash"] == "" and invalid["staged"] is None,
                        f"invalid room fragment survived or staged: {invalid}")
                cdp.call("Page.navigate", {"url": server.origin + "/room/#match=" + secret})
                wait_value(cdp, "Boolean(globalThis.MDKROnlineRoom)", bool,
                           "deep-link Online Room API", args.timeout)
                require(cdp.evaluate("MDKROnlineRoom.ready", await_promise=True) is True,
                        "deep-link join did not initialize")
                joined = wait_value(cdp, """(() => ({
                  title:document.getElementById('online-room-title').textContent,
                  open:document.getElementById('online-room-dialog').open,
                  hash:location.hash,
                  staged:sessionStorage.getItem('mdkr-online-room-entry-v1'),
                  requests:__liveRequests.slice()}))()""",
                    lambda value: isinstance(value, dict) and
                        value.get("title") == "Private Room",
                    "fragment deep-link join", args.timeout)
                join_requests = [item for item in joined["requests"]
                                 if item["path"] == "/api/match/join"]
                require(joined["open"] and joined["hash"] == "" and
                        joined["staged"] is None and
                        len(join_requests) == 1 and
                        join_requests[0]["body"] == {
                            "capability": secret,
                            "compatibility": {
                                "protocolVersion": 1,
                                "buildId": list(range(1, 17)),
                                "gameplayDigest": list(range(128, 160)),
                                "romRevision": 1, "cadenceHz": 30,
                            }, "seatCount": 1,
                        } and join_requests[0]["hash"] == "",
                        f"deep-link secret was not consumed safely: {joined}")
                resources = cdp.evaluate(
                    "performance.getEntriesByType('resource').map(item=>item.name)")
                require(all(secret not in item for item in resources),
                        "invite fragment entered a resource URL")
                require(not cdp.failures,
                        "deep-link browser/CDP failures: " + "; ".join(cdp.failures))
            finally:
                if cdp is not None:
                    cdp.close()
                chrome.close()
    finally:
        server.close()
    print("check_browser_online_match_room: PASS — shared-C live create/share/rotate/"
          "select/ready, reconnect, corrupt-state recovery and pre-GO admission gate")


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
        print(f"check_browser_online_match_room: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
