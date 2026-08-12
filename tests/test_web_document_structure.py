#!/usr/bin/env python3
"""Keep the launcher document's bypass link and section outline usable."""

from html.parser import HTMLParser
from pathlib import Path
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
    require('input.name = "room-invitation"' in online_script and
            'input.placeholder = "Paste link or enter 123 456…"' in online_script and
            'document.createElement("h3")' in online_script,
            "dynamic room forms need stable metadata and semantic headings")
    print("web document structure passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
