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

    # The numbered launcher sections are peers. Their headings cannot be h3
    # children of the preceding hidden How to play h2.
    require(document.headings.get("known-title") == "h2",
            "Known issues must be a top-level launcher section heading")
    require(document.headings.get("data-title") == "h2",
            "Stored data must be a top-level launcher section heading")
    print("web document structure passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
