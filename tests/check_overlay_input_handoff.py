#!/usr/bin/env python3
"""Give the pad back on every overlay close, not only the ones dispatch sees.

Issue #20: opening the pause menu with the controller and then activating its
on-screen "Resume" button killed controller input for the rest of the session,
while F1 and the controller's B/Circle back button were fine.  The difference
is not the device, it is *where* the menu closed.  F1, Escape, the pad's menu
button and B all close it from inside the overlay's SDL event handler, so the
pump saw the capture state change across one event and released the pad.
Resume closes it from the RENDER callback -- mouse, Enter, or ImGui gamepad
nav alike -- and there is no event across which the state changes, so the
suppression latch stayed set forever.

The contract this pins is therefore about the shape of the handoff rather than
one button: suppression is reconciled against the overlay's live answer, by one
shared routine, before dispatch as well as across it.  A source contract is the
right level for it -- the latch is a file-static inside the SDL pump, and the
behaviour that broke was the *absence* of a call, which no run of the paths
that still worked could ever reveal.
"""

from __future__ import annotations

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
PUMP = (ROOT / "platform" / "platform_sdl_min.c").read_text(encoding="utf-8")
OVERLAY = (ROOT / "platform" / "app" / "ui_overlay.cpp").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_body(source: str, signature: str) -> str:
    match = re.search(re.escape(signature) + r"\s*\{", source)
    require(match is not None, f"missing {signature}")
    start = match.end()
    depth = 1
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index]
    raise AssertionError(f"unterminated {signature}")


def check_shared_release_routine() -> str:
    body = function_body(PUMP, "static void overlay_capture_sync(uint64_t target_tick)")
    require("platformOverlayWantsInput()" in body,
            "the handoff must read the overlay's live capture state")
    require("s_gameInputSuppressed" in body,
            "the handoff must own the suppression latch")
    require("input_clear_game_sources();" in body,
            "releasing the pad must retire host sources, so a button held "
            "across the transition cannot arrive as a fresh press")
    require("input_capture_live(target_tick);" in body,
            "the handoff must publish the neutral sample immediately")
    return body


def check_single_latch_owner() -> None:
    assignments = [
        match.start()
        for match in re.finditer(r"s_gameInputSuppressed\s*=", PUMP)
    ]
    require(len(assignments) == 1,
            "the suppression latch must have exactly one assignment site; "
            f"found {len(assignments)}. Every overlay close path has to run "
            "the same release routine.")
    sync = re.search(
        r"static void overlay_capture_sync\(uint64_t target_tick\)\s*\{", PUMP)
    require(sync is not None, "missing overlay_capture_sync")
    require(assignments[0] > sync.start(),
            "the only assignment must be the shared release routine's")


def check_reconciled_before_dispatch() -> None:
    body = function_body(PUMP, "void platform_input_pump(void)")
    # The dispatch loop may live in a helper the pump calls -- it was factored
    # out so a second, just-in-time sample at the tick boundary could reuse it
    # verbatim rather than duplicate the swallow/focus/hotplug handling. Inline
    # the callee at its call site so the ORDERING assertions below still see one
    # continuous sequence. This follows the code; it does not relax the checks,
    # and every requirement below is unchanged. A pump that stopped calling the
    # helper AND stopped polling still fails, because the inlined body would
    # then contain no SDL_PollEvent either.
    for callee in ("input_dispatch_events",):
        call = f"{callee}(target_tick);"
        if call in body:
            body = body.replace(
                call, function_body(PUMP, f"static void {callee}(uint64_t target_tick)"))
    poll = body.find("SDL_PollEvent")
    require(poll >= 0, "the pump no longer polls SDL events")
    before = body[:poll]
    require("overlay_capture_sync(target_tick);" in before,
            "the pump must reconcile the overlay's capture state BEFORE it "
            "dispatches events. A close performed from the render callback "
            "(the on-screen Resume button, and the scripted schedule) never "
            "changes state across an event, so an edge observed during "
            "dispatch cannot see it and the pad is never given back.")

    dispatch = body[poll:]
    process = dispatch.find("platformOverlayProcessEvent(&e)")
    require(process >= 0, "the pump no longer offers events to the overlay")
    require("overlay_capture_sync(target_tick);" in dispatch[process:],
            "the toggle key/button flips capture inside process_event, so the "
            "same shared routine must run across dispatch too")


def check_every_close_shares_one_routine() -> None:
    # Overlay_install() seeds the initial state of a not-yet-running overlay,
    # which is not a close; everything after it is.
    install = function_body(OVERLAY, "void Overlay_install(SDL_Window *window)")
    running = OVERLAY.replace(install, "")
    direct = re.findall(r"g_overlay\.open\s*=\s*(?:false|0)\b", running)
    require(not direct,
            "an overlay close path bypassed setOpen(): every close has to run "
            "the one routine that restores audio, the cursor and the menu "
            "stack, so no path can invent its own idea of 'resume'")

    resume = re.search(
        r'PrimaryButton\("Resume"[^)]*\)\)\s*(?P<action>[^;]*;)', OVERLAY)
    require(resume is not None, "missing the overlay's Resume button")
    require("setOpen(false)" in resume.group("action"),
            "Resume must close through the shared setOpen() routine")

    for signature, label in (
        ("void navigateBack(OverlayBackInput input, bool keyRepeat)",
         "the Escape / B back-stack"),
        ("void overlayTestFrameTick()", "the scripted open/close schedule"),
    ):
        body = function_body(OVERLAY, signature)
        require("setOpen(" in body, f"{label} must close through setOpen()")


def check_hidden_overlay_holds_no_input() -> None:
    body = function_body(OVERLAY, "static int onRender(void)")
    hidden = re.search(
        r"if \(!g_overlay\.open && !g_overlay\.showFps\)\s*\{(?P<body>.*?)\n    \}",
        body, re.DOTALL)
    require(hidden is not None,
            "onRender no longer has a distinct not-drawing path")
    drain = hidden.group("body")
    # ImGui::NewFrame() is the only consumer of the queue that the overlay's
    # event handler keeps filling, and the only thing that releases a key ImGui
    # still believes is held. Neither runs while the overlay is hidden.
    require("ClearEventsQueue()" in drain,
            "a hidden overlay must drop the ImGui events it will never draw, "
            "or a race's worth of keystrokes is trickled into the menu the "
            "moment it opens")
    require("ClearInputKeys()" in drain and "ClearInputMouse()" in drain,
            "a hidden overlay must release ImGui's held keys/buttons, so the "
            "press that activated Resume cannot survive the close and "
            "re-activate whatever the menu shows next time")


def main() -> int:
    check_shared_release_routine()
    check_single_latch_owner()
    check_reconciled_before_dispatch()
    check_every_close_shares_one_routine()
    check_hidden_overlay_holds_no_input()
    print("overlay input handoff contract passed: one shared release routine, "
          "reconciled against live capture state before and across dispatch; "
          "every close path shares it; a hidden overlay latches no ImGui input")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
