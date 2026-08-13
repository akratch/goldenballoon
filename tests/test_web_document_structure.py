#!/usr/bin/env python3
"""Keep the launcher document's bypass link and section outline usable."""

from html.parser import HTMLParser
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent


class Document(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.elements: dict[str, dict[str, str]] = {}
        self.skip_target = ""
        self.headings: dict[str, str] = {}

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = {name: value or "" for name, value in attrs}
        element_id = attributes.get("id")
        if element_id:
            self.elements[element_id] = attributes | {"tag": tag}
        if tag == "a" and "skip-link" in attributes.get("class", "").split():
            self.skip_target = attributes.get("href", "")
        if tag in {"h1", "h2", "h3", "h4", "h5", "h6"} and element_id:
            self.headings[element_id] = tag


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    document = Document()
    document.feed((ROOT / "dist" / "web" / "index.html").read_text(encoding="utf-8"))
    style = (ROOT / "dist" / "web" / "style.css").read_text(encoding="utf-8")

    # The ROM UI is intentionally hidden during capability detection. A bypass
    # link must land on the main landmark, which exists in every launcher state.
    require(document.skip_target == "#gate",
            "skip link must target the always-present main landmark")
    main = document.elements.get("gate")
    require(main is not None and main.get("tag") == "main",
            "skip target must identify the document main landmark")
    require("hidden" not in main,
            "skip target must remain available before capability detection")
    require(main.get("tabindex") == "-1",
            "main landmark must accept focus from the skip link")
    require(".skip-link:focus-visible" in style,
            "skip link must reveal itself for keyboard-visible focus")
    require(".fs-btn:hover, .fs-btn:focus-visible { opacity: 1; outline: none; }"
            not in style,
            "fullscreen control must retain the shared visible focus outline")
    advanced = document.elements.get("experimental-presentation")
    require(advanced is not None and advanced.get("tag") == "details",
            "experimental motion controls need a semantic disclosure")
    require("open" not in advanced,
            "experimental motion disclosure must be collapsed for a fresh visit")
    require(":where(button, a, select, input, summary):focus-visible" in style,
            "experimental motion summary needs keyboard-visible focus")
    for status_id in ("gate-msg", "rom-status"):
        status = document.elements.get(status_id)
        require(status is not None and status.get("tabindex") == "-1",
                f"{status_id} must accept programmatic recovery focus")
    rom_group = document.elements.get("rom-session-banner")
    rom_message = document.elements.get("rom-session-message")
    rom_retry = document.elements.get("rom-storage-retry")
    require(rom_group is not None and rom_group.get("role") == "group" and
            rom_group.get("aria-label") == "ROM storage",
            "ROM persistence warning and action need one named group")
    require(rom_message is not None and rom_message.get("role") == "status" and
            rom_message.get("aria-live") == "assertive",
            "ROM persistence outcome needs an assertive live message")
    require(rom_retry is not None and rom_retry.get("tag") == "button",
            "ROM persistence failure needs a public Retry action")
    stage_status = document.elements.get("stage-status")
    require(stage_status is not None and stage_status.get("role") == "status" and
            stage_status.get("aria-live") == "assertive",
            "fullscreen failures need visible live status")
    require("#stage.touch-ui-active #stage-status" in style,
            "touch fullscreen errors must reserve space below the top controls")
    online_dialog = document.elements.get("online-room-dialog")
    online_title = document.elements.get("online-room-title")
    require(online_dialog is not None and online_dialog.get("tag") == "dialog" and
            online_dialog.get("aria-labelledby") == "online-room-title",
            "online entry must use a labelled native modal dialog")
    require(online_title is not None and online_title.get("tabindex") == "-1",
            "online status heading must accept initial non-destructive focus")
    for action_id in ("online-room-close", "online-room-local",
                      "online-room-controllers"):
        action = document.elements.get(action_id)
        require(action is not None and action.get("tag") == "button",
                f"{action_id} must remain a native keyboard action")
    party_end_dialog = document.elements.get("party-end-dialog")
    require(party_end_dialog is not None and
            party_end_dialog.get("aria-labelledby") == "party-end-title",
            "ending every phone session needs a labelled confirmation dialog")
    for action_id in ("party-end-cancel", "party-end-confirm"):
        require(document.elements.get(action_id, {}).get("tag") == "button",
                f"{action_id} must remain a native confirmation action")
    party_remove_dialog = document.elements.get("party-remove-dialog")
    require(party_remove_dialog is not None and
            party_remove_dialog.get("aria-labelledby") == "party-remove-title" and
            party_remove_dialog.get("aria-describedby") == "party-remove-copy",
            "removing one phone needs a labelled and described confirmation dialog")
    for action_id in ("party-remove-cancel", "party-remove-confirm"):
        require(document.elements.get(action_id, {}).get("tag") == "button",
                f"{action_id} must remain a native confirmation action")
    for seat in range(1, 5):
        seat_element = document.elements.get(f"party-seat-{seat}")
        require(seat_element is not None and seat_element.get("tabindex") == "-1",
                f"Controller {seat} must accept contextual focus after removal")
    for copy_id in ("party-scan-step", "party-install-copy"):
        require(document.elements.get(copy_id, {}).get("tag") == "p",
                f"{copy_id} must remain independently replaceable fallback copy")
    launcher_stick = document.elements.get("touch-stick")
    require(launcher_stick is not None and launcher_stick.get("tabindex") == "0" and
            launcher_stick.get("aria-valuetext") == "Centered" and
            "arrow keys" in launcher_stick.get("aria-label", ""),
            "launcher analog control needs discoverable keyboard steering")

    # The numbered launcher sections are peers. Their headings cannot be h3
    # children of the preceding hidden How to play h2.
    require(document.headings.get("known-title") == "h2",
            "Known issues must be a top-level launcher section heading")
    require(document.headings.get("data-title") == "h2",
            "Stored data must be a top-level launcher section heading")

    controller = Document()
    controller.feed((ROOT / "dist" / "web" / "controller" / "index.html")
                    .read_text(encoding="utf-8"))
    room_code = controller.elements.get("room-code")
    require(room_code is not None and room_code.get("aria-invalid") == "false" and
            room_code.get("aria-describedby") == "code-error" and
            room_code.get("translate") == "no",
            "controller code entry needs inline error and identifier semantics")
    leave_dialog = controller.elements.get("leave-dialog")
    require(leave_dialog is not None and
            leave_dialog.get("aria-labelledby") == "leave-title",
            "leaving a controller needs a labelled confirmation dialog")
    for action_id in ("leave-cancel", "leave-confirm"):
        require(controller.elements.get(action_id, {}).get("tag") == "button",
                f"{action_id} must remain a native confirmation action")
    require(controller.elements.get("controller-retry", {}).get("tag") == "button",
            "finite phone reconnect recovery needs a native Retry action")
    controller_stick = controller.elements.get("phone-touch-stick")
    require(controller_stick is not None and
            controller_stick.get("tabindex") == "0" and
            controller_stick.get("aria-valuetext") == "Centered" and
            "arrow keys" in controller_stick.get("aria-label", ""),
            "phone analog control needs discoverable keyboard steering")

    room = Document()
    room.feed((ROOT / "dist" / "web" / "room" / "index.html")
              .read_text(encoding="utf-8"))
    require(room.skip_target == "#room-entry-main" and
            room.elements.get("room-entry-main", {}).get("tabindex") == "-1",
            "private-room handoff needs keyboard bypass focus")

    controller_style = (ROOT / "dist" / "web" / "controller" /
                        "controller.css").read_text(encoding="utf-8")
    party_style = (ROOT / "dist" / "web" / "party" /
                   "party-host.css").read_text(encoding="utf-8")
    online_style = (ROOT / "dist" / "web" / "online" /
                    "online-room.css").read_text(encoding="utf-8")
    room_style = (ROOT / "dist" / "web" / "room" /
                  "room-entry.css").read_text(encoding="utf-8")
    touch_style = (ROOT / "dist" / "web" / "input" /
                   "touch-surface.css").read_text(encoding="utf-8")
    require("overscroll-behavior: contain" in controller_style and
            "overscroll-behavior: contain" in party_style and
            "overscroll-behavior: contain" in online_style,
            "every multiplayer modal must contain overscroll")
    require("env(safe-area-inset-top)" in room_style and
            ".skip-link:focus-visible" in room_style,
            "room handoff must honor notches and visible skip focus")
    require("a:hover" in room_style and "a:active" in room_style,
            "room handoff link needs hover and pressed feedback")
    require(".touch-controls .touch-stick:focus-visible" in touch_style,
            "keyboard steering needs a shared visible focus treatment")

    online_script = (ROOT / "dist" / "web" / "online" /
                     "online-room.js").read_text(encoding="utf-8")
    online_presenter = (ROOT / "dist" / "web" / "online" /
                        "online-room-presenter.js").read_text(encoding="utf-8")
    online_live_state = (ROOT / "dist" / "web" / "online" /
                         "online-room-live-state.js").read_text(encoding="utf-8")
    room_entry = (ROOT / "dist" / "web" / "room" /
                  "room-entry.js").read_text(encoding="utf-8")
    party_host = (ROOT / "dist" / "web" / "party" /
                  "party-host.js").read_text(encoding="utf-8")
    controller_script = (ROOT / "dist" / "web" / "controller" /
                         "controller.js").read_text(encoding="utf-8")
    live_browser_gate = (ROOT / "tests" /
                         "check_browser_online_match_room.py").read_text(
                             encoding="utf-8")
    cmake_tests = (ROOT / "cmake" / "tests.cmake").read_text(encoding="utf-8")
    launcher_markup = (ROOT / "dist" / "web" /
                       "index.html").read_text(encoding="utf-8")
    require('input.name = "room-invitation"' in online_script and
            'input.placeholder = "Paste link or enter 123 456…"' in online_script and
            'document.createElement("h3")' in online_script,
            "dynamic room forms need stable metadata and semantic headings")
    require("presenter.project(model" in online_script and
            "Do not continue if even 1 word differs." in online_presenter and
            "exactKeys(value, CANONICAL_STATE_KEYS)" in online_live_state and
            'url.hash === `#match=${capability}`' in online_live_state and
            "publicationRelation(priorState, state)" in online_live_state and
            'if (relation === "obsolete")' in online_live_state and
            "state.inviteGeneration === priorState.inviteGeneration" in
            online_live_state and
            "expectedInviteResponseGeneration !== state.inviteGeneration" in
            online_live_state and
            "mergeLiveState(value, responseGeneration)" in online_script and
            "if (ingested.obsolete)" in online_script and
            "const maxLiveResponseBytes = 64 * 1024" in online_script and
            "total + value.byteLength > maxLiveResponseBytes" in online_script and
            "new TextEncoder().encode(event.data).byteLength" in online_script and
            'socket.close(1009, "invalid_match_state")' in online_script and
            "socketGeneration !== liveSocketGeneration" in online_script and
            "function failLiveSocketSetup(operation, socketGeneration)" in
            online_script and
            'typeof subscription.close !== "function"' in online_script and
            "if (liveSocketAttempt >= 5)" in online_script and
            "markLiveSocketStable(socketGeneration)" in online_script and
            "if (qrReady) auxiliary.append(canvas)" in online_script and
            "if (!liveInviteReplacementAllowed())" in online_script and
            "Share this room link or enter the code" in online_script and
            "__liveRaceNextRotate" in live_browser_gate and
            "__liveLoseLeadershipNextRotate" in live_browser_gate and
            "__liveFlap" in live_browser_gate and
            "__liveThrowNextSubscribe" in live_browser_gate and
            "accepted solo host close" in live_browser_gate and
            'const exactFragment = location.hash === `#match=${capability}`' in
            room_entry and
            "if (location.hash)" in room_entry and
            "location.replace(location.pathname + location.search);" in
            room_entry and
            "sessionStorage" not in room_entry and
            "mdkr-online-room-entry-v1" not in online_script and
            "function readEntryFragment()" in online_script and
            online_script.index("const entryFragment = readEntryFragment()") <
            online_script.index("const byId = (id)") and
            'return {attempted: true, capability: "", scrubbed: false};' in
            online_script and
            "if (!entryFragment.scrubbed) return;" in online_script and
            'addEventListener("hashchange"' in online_script and
            "const fragment = readEntryFragment();" in online_script and
            "pendingEntryCapability" in online_script and
            "Private Rooms Aren’t Enabled in This Build" in online_script and
            "if(TARGET mdkr_online_lobby_browser_wasm_test)" in cmake_tests and
            launcher_markup.index("online-room-live-state.js") <
            launcher_markup.index("online-room.js") and
            launcher_markup.index("online-room-presenter.js") <
            launcher_markup.index("online-room.js"),
            "Online Room semantic presenter is missing or loaded after its renderer")
    require("function normalizedPublishedRoomState(value)" in party_host and
            "function trustedPartyUrl(url)" in party_host and
            "base.origin !== location.origin" in party_host and
            'target.pathname.startsWith("/api/")' in party_host and
            "equivocated_party_state" in party_host and
            "if (socketReconnectAttempt >= 5)" in party_host and
            "controller_socket_setup_failed" in party_host and
            "function normalizedRevocation(value, expectedGeneration)" in party_host and
            "value.inviteGeneration !== expectedGeneration + 1" in party_host and
            "[expectedGeneration, value.inviteGeneration]" in party_host and
            "const invalidatesDisplayedInvite = inviteActive" in party_host and
            "publishedInviteGeneration !== priorInviteGeneration" in party_host and
            "function clearInvitePresentation(expired, announceExpiry = false)" in
            party_host and
            "const pendingRevocation = inviteRevocation" in party_host and
            "canvas.width = 1" in party_host and
            'addEventListener("pageshow", (event)' in party_host and
            "if (!event.persisted) return;" in party_host and
            "for (const controllerId of [...peers.keys()])" in party_host and
            "function abortPageBoundRequests()" in party_host and
            party_host.count("pageBound: true") == 2 and
            # Starting local play must still hand off through dismissRoom(),
            # with nothing but whitespace or commentary between the two
            # statements. Matched as a pattern so that documenting the
            # handoff does not read as removing it.
            bool(re.search(
                r"startingGame = true;\s*"
                r"(?:/\*.*?\*/\s*|//[^\n]*\n\s*)*"
                r"dismissRoom\(\);", party_host, re.S)) and
            'wrap.hidden = !qrReady' in party_host and
            'control("remove", controllerId, source)' in party_host and
            '$("party-remove-cancel").focus()' in party_host and
            'returnTarget.focus({preventScroll: true})' in party_host and
            "controllerRequestTimeoutMs = 10_000" in controller_script and
            "const pageBoundRequests = new Set()" in controller_script and
            "for (const controller of pageBoundRequests) controller.abort()" in
            controller_script and
            controller_script.index("const entryFragment = (() =>") <
            controller_script.index("const $ = (id)") and
            'location.pathname === "/controller/" && !location.search' in
            controller_script and
            '/^[A-Za-z0-9_-]{43}$/.test(value)' in controller_script and
            "binary.length !== 32" in controller_script and
            'globalThis.history.replaceState(null, "", "/controller/")' in
            controller_script and
            'return {raw: "", scrubbed: false};' in controller_script and
            "if (!entryFragment.scrubbed) return;" in controller_script and
            'entryFragment.raw = ""' in controller_script and
            "function controllerInviteUrl()" in controller_script and
            "function controllerRecoveryUrl()" in controller_script and
            "function trustedControllerLocation()" in controller_script and
            "function secureControllerRecoveryUrl()" in controller_script and
            "function shareRecoveryLabel()" in controller_script and
            "url.hash = capability" in controller_script and
            "function shareControllerInvite()" in controller_script and
            'privateLink\n          ? "Open this private controller link' in
            controller_script and
            'errorRecoveryAction === "share"' in controller_script and
            'errorRecoveryAction === "reload"' in controller_script and
            'errorRecoveryAction === "secure"' in controller_script and
            "location.reload()" in controller_script and
            "navigator.clipboard.writeText(inviteUrl)" in controller_script and
            "location.replace(inviteUrl)" in controller_script and
            "function acquireWebTabLease(name, force)" in controller_script and
            "function acquireFallbackTabLease(name, force)" in controller_script and
            "function handleTabLeaseLoss()" in controller_script and
            'typeof BroadcastChannel !== "function"' in controller_script and
            'const storageKey = "gb-controller-tab-lease"' in controller_script and
            'type: "released", id, to: message.id' in controller_script and
            "liveStoredOwner && !takeoverAcknowledged" in controller_script and
            '{mode: "exclusive", ifAvailable: true}' in controller_script and
            "{steal: true}" not in controller_script and
            'showError("duplicate_controller")' in controller_script and
            'if (event.persisted) location.reload()' in controller_script and
            'addEventListener("freeze", () => neutralize("freeze"))' in
            controller_script and
            "function resumeControllerInput()" in controller_script and
            "padHistory = [];\n      publishPad(true);" in controller_script and
            'addEventListener("resume", resumeControllerInput)' in
            controller_script and
            "function normalizedDeviceName(value)" in controller_script and
            "\\u202a-\\u202e\\u2060\\u2066-\\u2069" in controller_script and
            "function normalizedControllerState(value)" in controller_script and
            "if (reconnectAttempt >= 5)" in controller_script and
            "generation !== controlGeneration" in controller_script and
            'phase === "waiting" ? "approval_rejected" : "seat_reclaimed"' in
            controller_script and
            '["approval_rejected", "seat_reclaimed", "host_closed", "room_expired"]' in
            controller_script and
            'referrerPolicy: "no-referrer"' in controller_script,
            "Phone Party finite-recovery or bounded-response contract is missing")
    print("web document structure passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
