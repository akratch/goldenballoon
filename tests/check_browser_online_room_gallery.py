#!/usr/bin/env python3
"""Compare and operate every shared-C Online Room view in real Chromium."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, OverlayServer, find_chrome,
    page_websocket, require, wait_value,
)
from harness_utils import resolve_binary

ROOT = Path(__file__).resolve().parent.parent
ROW_RE = re.compile(
    r"^online-gallery-v1 slug=([a-z0-9-]+) kind=(\d+) failure=(\d+) "
    r"primary=(\d+) timeout=([01]) admission=([01]) timeout-visible=([01])$"
)


@dataclass(frozen=True)
class Case:
    slug: str
    kind: int
    failure: int
    primary: int
    admission: bool
    timeout_visible: bool
    title: str
    verification_phrase: str
    labels: tuple[str, ...]
    actions: tuple[int, ...]


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def native_cases(binary: str, root: Path) -> list[Case]:
    prefs = root / "native-prefs"
    saves = root / "native-saves"
    prefs.mkdir()
    saves.mkdir()
    completed = subprocess.run(
        [binary], env=clean_environment(
            LC_ALL="C", MDKR_APP_DUMP_ONLINE_GALLERY="1",
            MDKR_APP_PREFS_DIR=str(prefs),
            MDKR_VIDEO_CONFIG_PATH=str(root / "native-video.ini"),
            MDKR_SAVE_DIR=str(saves), MDKR_AUDIO="0"),
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=60,
    )
    require(completed.returncode == 0,
            f"native gallery inventory failed:\n{completed.stdout[-3000:]}")
    semantic: dict[str, tuple[int, int, int, bool, bool]] = {}
    copies: dict[str, tuple[str, str, tuple[str, ...]]] = {}
    actions: dict[str, tuple[int, ...]] = {}
    for line in completed.stdout.splitlines():
        match = ROW_RE.match(line)
        if match:
            slug, kind, failure, primary, _timeout, admission, visible = match.groups()
            semantic[slug] = (int(kind), int(failure), int(primary),
                              admission == "1", visible == "1")
        elif line.startswith("online-gallery-copy-v2|"):
            fields = line.split("|")
            require(len(fields) == 8, f"malformed native copy row: {line}")
            copies[fields[1]] = (fields[2], fields[7], tuple(fields[3:7]))
        elif line.startswith("online-gallery-actions-v1|"):
            fields = line.split("|")
            require(len(fields) == 6, f"malformed native action row: {line}")
            actions[fields[1]] = tuple(int(value) for value in fields[2:])
    require(len(semantic) == len(copies) == len(actions) == 43,
            "native product did not publish the complete 43-case contract")
    result = []
    for slug, (kind, failure, primary, admission, visible) in semantic.items():
        title, verification_phrase, raw_labels = copies[slug]
        raw_actions = actions[slug]
        ordered_labels: list[str] = []
        ordered_actions: list[int] = []
        for position in (3, 0, 1, 2):
            action = raw_actions[position]
            label = raw_labels[position]
            if not action or not label or action in ordered_actions:
                continue
            ordered_actions.append(action)
            ordered_labels.append(label)
        result.append(Case(slug, kind, failure, primary, admission, visible,
                           title, verification_phrase, tuple(ordered_labels),
                           tuple(ordered_actions)))
    return result


def key_press(cdp: CDPClient, key: str, code: str, virtual: int) -> None:
    base = {"key": key, "code": code,
            "windowsVirtualKeyCode": virtual, "nativeVirtualKeyCode": virtual}
    text_value = "\r" if key == "Enter" else " " if code == "Space" else ""
    cdp.call("Input.dispatchKeyEvent", {
        "type": "keyDown", "text": text_value, **base})
    cdp.call("Input.dispatchKeyEvent", {"type": "keyUp", **base})


def open_if_needed(cdp: CDPClient) -> None:
    cdp.evaluate("""(() => {
      const dialog=document.getElementById('online-room-dialog');
      if (!dialog.open) globalThis.MDKROnlineRoom.open();
    })()""")
    wait_value(cdp, "document.getElementById('online-room-dialog').open", bool,
               "shared room dialog", 30)


def run(args: argparse.Namespace) -> None:
    shell = Path(args.shell_dir).expanduser()
    if not shell.is_absolute():
        shell = (ROOT / shell).resolve()
    binary = resolve_binary(args.build)
    for relative in ("index.html", "online/online-control-config.js",
                     "online/online-room-live-state.js",
                     "online/online-room-presenter.js", "online/online-room.js",
                     "mdkr-online-tools.js", "mdkr-online-tools.wasm"):
        require((shell / relative).is_file(), f"shared room artifact missing: {relative}")
    require((shell / "mdkr-online-tools.wasm").stat().st_size < 131072,
            "Online Room projection exceeded its 128 KiB isolation budget")

    with tempfile.TemporaryDirectory(prefix="mdkr-browser-room-gallery-") as temporary:
        root = Path(temporary)
        cases = native_cases(binary, root)
        server = OverlayServer(shell, shell)
        server.start()
        try:
            with tempfile.TemporaryDirectory(prefix="mdkr-browser-room-profile-") as profile:
                chrome = ChromeProcess(find_chrome(args.chrome), Path(profile),
                                       args.chrome_flag, args.verbose)
                cdp: CDPClient | None = None
                try:
                    cdp = CDPClient(page_websocket(chrome.wait_port()))
                    for domain in ("Page", "Runtime", "Log", "Inspector",
                                   "Accessibility"):
                        cdp.call(f"{domain}.enable")
                    cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": """
                      globalThis.__mdkrOnlineRoomTestConfig={case:'entry',autoComplete:false};
                    """})
                    cdp.call("Emulation.setDeviceMetricsOverride", {
                        "width": 960, "height": 720, "deviceScaleFactor": 1,
                        "mobile": False,
                    })
                    cdp.call("Page.navigate", {"url": server.origin + "/"})
                    cdp.call("Page.bringToFront")
                    wait_value(cdp, "Boolean(globalThis.MDKROnlineRoom)", bool,
                               "Online Room launcher API", args.timeout)
                    ready = cdp.evaluate("globalThis.MDKROnlineRoom.ready",
                                         await_promise=True)
                    require(ready is True, "shared C Online Room module did not initialize")
                    require(cdp.evaluate("MDKROnlineRoom.inventory().length") == 43,
                            "browser did not enumerate all native gallery cases")
                    open_if_needed(cdp)

                    signatures: set[str] = set()
                    action_case: dict[int, int] = {}
                    for index, case in enumerate(cases):
                        rendered = cdp.evaluate(f"""(() => {{
                          if (!MDKROnlineRoom.select({index})) return null;
                          const model=MDKROnlineRoom.current();
                          const buttons=[...document.querySelectorAll(
                            '#online-room-model-actions [data-online-action]')];
                          const dialog=document.getElementById('online-room-dialog');
                          return {{slug:dialog.dataset.gallerySlug,
                            kind:Number(dialog.dataset.viewKind),
                            failure:Number(dialog.dataset.failure),
                            title:document.getElementById('online-room-title').textContent,
                            verificationPhrase:model.verificationPhrase,
                            renderedPhrase:document.getElementById(
                              'online-room-verification-phrase').textContent,
                            phraseHidden:document.getElementById(
                              'online-room-verification').hidden,
                            labels:buttons.map(button=>button.textContent),
                            actions:buttons.map(button=>Number(button.dataset.onlineAction)),
                            primary:model.controls[0].action,
                            timeoutVisible:model.timeoutVisible,
                            admission:model.admission,
                            selectCount:document.querySelectorAll(
                              '#online-room-selection select').length,
                            signature:document.getElementById('online-room-title').textContent+'|'+
                              document.getElementById('online-room-explanation').textContent+'|'+
                              document.getElementById('online-room-model').innerHTML,
                            overflow:dialog.scrollWidth>dialog.clientWidth+1}};
                        }})()""")
                        require(rendered is not None, f"{case.slug}: browser selection failed")
                        require((rendered["slug"], rendered["kind"], rendered["failure"],
                                 rendered["primary"], rendered["timeoutVisible"],
                                 rendered["admission"]) ==
                                (case.slug, case.kind, case.failure, case.primary,
                                 case.timeout_visible, case.admission),
                                f"{case.slug}: browser semantic model drifted: {rendered}")
                        require(rendered["title"] == case.title and
                                rendered["verificationPhrase"] ==
                                  case.verification_phrase and
                                rendered["renderedPhrase"] ==
                                  case.verification_phrase and
                                rendered["phraseHidden"] ==
                                  (not bool(case.verification_phrase)) and
                                tuple(rendered["labels"]) == case.labels and
                                tuple(rendered["actions"]) == case.actions,
                                f"{case.slug}: browser copy/action order drifted: {rendered}")
                        expected_selects = 1 if case.primary in (6, 7, 8) else 0
                        require(rendered["selectCount"] == expected_selects,
                                f"{case.slug}: semantic choice control mismatch")
                        require(not rendered["overflow"],
                                f"{case.slug}: dialog has horizontal overflow")
                        require(rendered["signature"] not in signatures,
                                f"{case.slug}: browser DOM duplicates another gallery case")
                        signatures.add(rendered["signature"])
                        for action in case.actions:
                            action_case.setdefault(action, index)

                    require(set(action_case) == set(range(1, 28)),
                            f"browser inventory does not expose all actions: {sorted(action_case)}")

                    phrase_cases = [case for case in cases
                                    if case.verification_phrase]
                    require(len(phrase_cases) == 1,
                            "browser/native contract must expose exactly one phrase view")
                    phrase_case = phrase_cases[0]
                    require(phrase_case.primary == 26 and
                            phrase_case.labels[:2] ==
                              ("Words Match", "Words Differ"),
                            "verification phrase lacks both explicit decisions")
                    phrase_index = cases.index(phrase_case)
                    cdp.evaluate(f"MDKROnlineRoom.select({phrase_index})")
                    require(cdp.evaluate("""(() => {
                      const region=document.getElementById('online-room-verification');
                      const output=document.getElementById(
                        'online-room-verification-phrase');
                      const announcement=document.getElementById(
                        'online-room-announcement');
                      return !region.hidden &&
                        region.getAttribute('aria-labelledby')===
                          'online-room-verification-title' &&
                        output.getAttribute('translate')==='no' &&
                        announcement.textContent.includes(output.textContent) &&
                        announcement.textContent.includes(
                          'Do not continue if even 1 word differs.');
                    })()"""),
                            "phrase card or assistive announcement is incomplete")

                    mismatch_index = next(
                        i for i, case in enumerate(cases)
                        if case.slug == "failure-verification-mismatch")
                    mismatch_case = cases[mismatch_index]
                    require(mismatch_case.failure == 18 and
                            mismatch_case.title == "Words Did Not Match" and
                            mismatch_case.labels[0] == "Reconnect Securely",
                            "phrase mismatch lacks dedicated actionable recovery")
                    cdp.evaluate(f"MDKROnlineRoom.select({mismatch_index})")
                    require(cdp.evaluate("""(() => {
                      const a=document.getElementById('online-room-announcement');
                      return a.getAttribute('aria-live')==='assertive' &&
                        a.textContent.startsWith('Words Did Not Match.') &&
                        document.getElementById('online-room-verification').hidden;
                    })()"""),
                            "phrase mismatch is not assertive or leaves stale phrase visible")

                    # Reflow every state at the minimum supported mobile width
                    # and browser zoom. Vertical scrolling is expected; two-axis
                    # scrolling is not.
                    for viewport in ((320, 568, "portrait"),
                                     (568, 320, "landscape")):
                        width, height, orientation = viewport
                        cdp.call("Emulation.setDeviceMetricsOverride", {
                            "width": width, "height": height,
                            "deviceScaleFactor": 2, "mobile": True,
                        })
                        cdp.call("Emulation.setPageScaleFactor",
                                 {"pageScaleFactor": 2})
                        for index, case in enumerate(cases):
                            layout = cdp.evaluate(f"""(() => {{
                              MDKROnlineRoom.select({index});
                              const d=document.getElementById('online-room-dialog');
                              const r=d.getBoundingClientRect();
                              return {{doc:document.documentElement.scrollWidth,
                                view:innerWidth,left:r.left,right:r.right,
                                client:d.clientWidth,scroll:d.scrollWidth}};
                            }})()""")
                            require(layout["doc"] <= layout["view"] and
                                    layout["left"] >= -1 and
                                    layout["right"] <= layout["view"] + 1 and
                                    layout["scroll"] <= layout["client"] + 1,
                                    f"{case.slug}: {orientation} 200% overflow: {layout}")

                    # Activate every product action through a focused native DOM
                    # control and a real Chromium Enter event. Resetting the C
                    # fixture before each action isolates routes and request ids.
                    cdp.call("Emulation.setPageScaleFactor", {"pageScaleFactor": 1})
                    cdp.call("Emulation.setDeviceMetricsOverride", {
                        "width": 960, "height": 720, "deviceScaleFactor": 1,
                        "mobile": False,
                    })
                    for action, index in sorted(action_case.items()):
                        opened = cdp.evaluate(f"""(() => {{
                          MDKROnlineRoom.select({index});
                          if (!document.getElementById('online-room-dialog').open) {{
                            MDKROnlineRoom.open();
                            return true;
                          }}
                          return false;
                        }})()""")
                        if opened:
                            wait_value(cdp, "document.activeElement.id",
                                       lambda value: value == "online-room-title",
                                       f"action {action} reopened dialog focus",
                                       args.timeout)
                        # Let launcher-owned close/open focus restoration from
                        # the preceding route drain before assigning this
                        # control. This mirrors the next painted interaction
                        # frame and prevents an old rAF from stealing focus.
                        cdp.evaluate("new Promise(resolve => requestAnimationFrame(() => "
                                     "requestAnimationFrame(resolve)))",
                                     await_promise=True)
                        focused = cdp.evaluate(f"""(() => {{
                          const button=document.querySelector(
                            `[data-online-action="{action}"]`);
                          if (!button) return false;
                          button.focus(); return document.activeElement===button;
                        }})()""")
                        require(focused, f"action {action} control could not receive focus")
                        before = cdp.evaluate(
                            "globalThis.__mdkrOnlineRoomTestState.activations.length")
                        key_press(cdp, " ", "Space", 32)
                        wait_value(cdp,
                            "globalThis.__mdkrOnlineRoomTestState.activations.length",
                            lambda value, before=before: value > before,
                            f"action {action} keyboard activation", args.timeout)
                        if action == 25:
                            focused = wait_value(cdp, "document.activeElement.textContent",
                                lambda value: value == "Leave Race and Room",
                                "Leave Race confirmation focus", args.timeout)
                            require(focused == "Leave Race and Room",
                                    "destructive confirmation did not receive explicit focus")
                            key_press(cdp, " ", "Space", 32)
                            wait_value(cdp,
                                "globalThis.__mdkrOnlineRoomTestState.activations.at(-1).result",
                                lambda value: value == 1,
                                "confirmed Leave Race", args.timeout)
                        result = cdp.evaluate(
                            "globalThis.__mdkrOnlineRoomTestState.activations.at(-1)")
                        require(result["action"] == action and result["result"] > 0,
                                f"action {action} did not reach its typed route: {result}")

                    # One actual touch goes through the primary DOM button, too.
                    cdp.evaluate("""(() => {
                      MDKROnlineRoom.select(0); MDKROnlineRoom.open();
                    })()""")
                    rect = cdp.evaluate("""(() => {
                      const button=document.querySelector('[data-online-action="1"]');
                      button.scrollIntoView({block:'center'});
                      const r=button.getBoundingClientRect();
                      return {x:(r.left+r.right)/2,y:(r.top+r.bottom)/2};
                    })()""")
                    before = cdp.evaluate(
                        "globalThis.__mdkrOnlineRoomTestState.activations.length")
                    cdp.call("Input.dispatchTouchEvent", {"type": "touchStart",
                        "touchPoints": [{"x": rect["x"], "y": rect["y"],
                                          "radiusX": 1, "radiusY": 1}]})
                    cdp.call("Input.dispatchTouchEvent", {"type": "touchEnd",
                                                          "touchPoints": []})
                    wait_value(cdp,
                        "globalThis.__mdkrOnlineRoomTestState.activations.length",
                        lambda value: value > before,
                        "touch Create Room activation", args.timeout)

                    # Recovery is assertive and exposes named native controls in
                    # the accessibility tree without leaking provider language.
                    failure_index = next(i for i, case in enumerate(cases)
                                         if case.slug == "failure-service-unavailable")
                    cdp.evaluate(f"MDKROnlineRoom.select({failure_index})")
                    open_if_needed(cdp)
                    ax = cdp.call("Accessibility.getFullAXTree")
                    nodes = ax.get("nodes", [])
                    names = {node.get("name", {}).get("value", "") for node in nodes}
                    normalized_names = {name.casefold() for name in names}
                    roles = {node.get("role", {}).get("value", "") for node in nodes}
                    require({"could not reach the room", "try again", "play here",
                             "return home"}.issubset(normalized_names) and
                            {"dialog", "button"}.issubset(roles),
                            "recovery controls are incomplete in the accessibility tree: "
                            f"names={sorted(names)} roles={sorted(roles)}")
                    require(cdp.evaluate("""(() => {
                      const a=document.getElementById('online-room-announcement');
                      return a.getAttribute('aria-live')==='assertive' &&
                        a.textContent.startsWith('Could Not Reach the Room.');
                    })()"""), "recovery announcement is not assertive")

                    resources = cdp.evaluate("performance.getEntriesByType('resource').map(e=>e.name)")
                    require(any("mdkr-online-tools.js" in url for url in resources) and
                            any("mdkr-online-tools.wasm" in url for url in resources),
                            "shared Online Room module was not really fetched")
                    require(not any("/api/" in url or "mdkr64_web.wasm" in url
                                    for url in resources),
                            "gallery binding contacted a provider or loaded the game engine")
                    require(not test_state_errors(cdp),
                            "shared Online Room test adapter reported an error")
                    require(not cdp.failures,
                            "browser/CDP failures: " + "; ".join(cdp.failures))
                    fatal = [line for line in cdp.console
                             if "Uncaught" in line or "TypeError" in line]
                    require(not fatal, "shared room console errors: " + "; ".join(fatal))
                finally:
                    if cdp is not None:
                        cdp.close()
                    chrome.close()
        finally:
            server.close()
    print("check_browser_online_room_gallery: PASS — 43 shared-C cases, "
          "27 keyboard actions, phrase decisions, touch, a11y and "
          "portrait/landscape 200% reflow")


def test_state_errors(cdp: CDPClient) -> list[str]:
    return cdp.evaluate("globalThis.__mdkrOnlineRoomTestState.errors.slice()")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"check_browser_online_room_gallery: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
