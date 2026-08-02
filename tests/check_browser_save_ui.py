#!/usr/bin/env python3
"""Exercise browser save custody without a ROM, renderer, or game engine.

The full browser runtime check proves that imported progress reaches the game.
This fast sibling owns the hostile-input, transaction-fault, recovery, merge,
accessibility, and privacy matrix. It deliberately removes ``navigator.gpu``
before page code runs and never selects a ROM or loads the engine wasm.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

from check_browser_runtime import (
    CDPClient,
    CheckFailure,
    ChromeProcess,
    OverlayServer,
    add_config_script,
    find_chrome,
    page_websocket,
    require,
    resolve_path,
    select_file_input,
    wait_download,
    wait_value,
)


IMAGE_SIZE = 512
SLOT_SIZE = 40
MAX_INPUT = 64 * 1024
EXPECTED_FAULT_POINTS = (
    "after-stage-write",
    "after-stage-verify",
    "after-backup",
    "after-install",
    "after-persist",
    "after-reload",
)


def portable_container(
    payload: bytes,
    *,
    version: int = 1,
    payload_text: str | None = None,
    digest: str | None = None,
    created_at: str = "2026-07-27T12:34:56.000Z",
    app_version: str = "browser-save-check",
    source: str = "test",
) -> bytes:
    document = {
        "format": "mdkr64-save",
        "version": version,
        "payloadFormat": "dkr-eeprom-4k-be-v1",
        "payload": (
            payload_text
            if payload_text is not None
            else base64.b64encode(payload).decode("ascii")
        ),
        "sha256": digest or hashlib.sha256(payload).hexdigest(),
        "createdAt": created_at,
        "appVersion": app_version,
        "source": source,
    }
    return json.dumps(
        document, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8")


def encoded_bytes(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def browser_bytes(cdp: CDPClient) -> bytes | None:
    value = cdp.evaluate(
        """MDKRSaveUI.testApi.snapshot().then(
          (bytes) => bytes ? Array.from(bytes) : null)""",
        await_promise=True,
        timeout=30,
    )
    if value is None:
        return None
    require(
        isinstance(value, list) and all(isinstance(item, int) for item in value),
        f"save snapshot is not a byte array: {value!r}",
    )
    return bytes(value)


def require_storage(cdp: CDPClient, expected: bytes | None, context: str) -> None:
    actual = browser_bytes(cdp)
    differing = (
        [
            index
            for index, (left, right) in enumerate(zip(expected, actual))
            if left != right
        ]
        if expected is not None and actual is not None
        else []
    )
    require(
        actual == expected,
        f"{context} changed storage: expected "
        f"{hashlib.sha256(expected).hexdigest() if expected is not None else 'absent'}, "
        f"got {hashlib.sha256(actual).hexdigest() if actual is not None else 'absent'}; "
        f"first differing offsets={differing[:24]}",
    )


def seed_raw(cdp: CDPClient, data: bytes) -> None:
    require(len(data) == IMAGE_SIZE, "test seed must be exactly 512 bytes")
    value = cdp.evaluate(
        f"""(async () => {{
          const text = {json.dumps(encoded_bytes(data))};
          const bytes = Uint8Array.from(atob(text), value => value.charCodeAt(0));
          await MDKRSaveUI.testApi.seedRaw(bytes);
          return Array.from(await MDKRSaveUI.testApi.snapshot());
        }})()""",
        await_promise=True,
        timeout=30,
    )
    require(bytes(value) == data, "browser did not commit the requested test seed")


def import_memory(
    cdp: CDPClient, data: bytes, name: str, mime: str = "application/octet-stream"
) -> dict[str, Any]:
    value = cdp.evaluate(
        f"""(async () => {{
          const dialog = document.getElementById("save-dialog");
          if (dialog.open) dialog.close();
          const text = {json.dumps(encoded_bytes(data))};
          const bytes = Uint8Array.from(atob(text), value => value.charCodeAt(0));
          await MDKRSaveUI.testApi.acceptImportFile(
            new File([bytes], {json.dumps(name)}, {{type: {json.dumps(mime)}}}));
          const stored = await MDKRSaveUI.testApi.snapshot();
          return {{
            status: document.getElementById("save-status").textContent,
            dialogStatus:
              document.getElementById("save-dialog-status").textContent,
            dialogOpen: dialog.open,
            bodyText: document.getElementById("save-dialog-body").textContent,
            bodyElementCount:
              document.querySelectorAll("#save-dialog-body img, " +
                                        "#save-dialog-body script").length,
            pwned: globalThis.__mdkrSavePwned === true,
            stored: stored ? Array.from(stored) : null
          }};
        }})()""",
        await_promise=True,
        timeout=30,
    )
    require(isinstance(value, dict), f"import returned no result: {value!r}")
    return value


def click_button(cdp: CDPClient, scope: str, label: str) -> None:
    found = cdp.evaluate(
        f"""(() => {{
          const button = [...document.querySelectorAll(
            {json.dumps(scope + " button")})].find(
              item => item.textContent.trim().includes({json.dumps(label)}));
          if (!button) return false;
          button.click();
          return true;
        }})()"""
    )
    require(found is True, f"button is absent: {label}")


def close_dialog(cdp: CDPClient) -> None:
    cdp.evaluate(
        """(() => {
          const dialog = document.getElementById("save-dialog");
          if (dialog.open) dialog.close();
        })()"""
    )


def wait_dialog_status(
    cdp: CDPClient, needle: str, description: str, timeout: float
) -> str:
    return wait_value(
        cdp,
        'document.getElementById("save-dialog-status").textContent',
        lambda value: isinstance(value, str) and needle in value.lower(),
        description,
        timeout,
    )


def wait_save_status(
    cdp: CDPClient, needle: str, description: str, timeout: float
) -> str:
    return wait_value(
        cdp,
        'document.getElementById("save-status").textContent',
        lambda value: isinstance(value, str) and needle in value.lower(),
        description,
        timeout,
    )


def ax_roles(cdp: CDPClient) -> set[tuple[str, str]]:
    result = cdp.call("Accessibility.getFullAXTree")
    pairs: set[tuple[str, str]] = set()
    for node in result.get("nodes", []):
        role = node.get("role", {}).get("value")
        name = node.get("name", {}).get("value")
        if isinstance(role, str) and isinstance(name, str):
            pairs.add((role, name))
    return pairs


def run_check(args: argparse.Namespace) -> None:
    shell_dir = resolve_path(args.shell_dir)
    engine_dir = resolve_path(args.engine_dir)
    chrome_path = find_chrome(args.chrome)
    cli_path = resolve_path(args.cli) if args.cli else None
    for label, path in (
        ("shell index", shell_dir / "index.html"),
        ("shell JavaScript", shell_dir / "mdkr64-shell.js"),
        ("save UI JavaScript", shell_dir / "mdkr-save-ui.js"),
        ("save-tools loader", engine_dir / "mdkr-save-tools.js"),
        ("save-tools module", engine_dir / "mdkr-save-tools.wasm"),
    ):
        require(path.is_file(), f"missing {label}: {path}")
    if cli_path is not None:
        require(cli_path.is_file(), f"missing native save CLI: {cli_path}")

    server = OverlayServer(shell_dir, engine_dir)
    server.start()
    with tempfile.TemporaryDirectory(prefix="mdkr64_save_chrome_") as profile_name:
        profile = Path(profile_name)
        chrome = ChromeProcess(
            chrome_path, profile, args.chrome_flag, args.verbose
        )
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in (
                "Page",
                "Runtime",
                "Network",
                "Log",
                "Inspector",
                "Accessibility",
            ):
                cdp.call(f"{domain}.enable")
            add_config_script(cdp, {})
            cdp.call(
                "Page.addScriptToEvaluateOnNewDocument",
                {
                    "source": """
                      Object.defineProperty(Navigator.prototype, "gpu", {
                        configurable: true,
                        get() { return undefined; }
                      });
                    """
                },
            )
            cdp.call("Page.navigate", {"url": server.origin + "/?save-check=1"})
            wait_save_status(
                cdp, "ready", "ROM-free save-tools startup", args.timeout
            )
            gate = wait_value(
                cdp,
                """(() => ({
                  blocked: document.getElementById("play").dataset.blocked === "1",
                  gate: document.getElementById("gate-msg").textContent,
                  uiHidden: document.getElementById("rom-ui").hidden,
                  controls: [
                    "download-save", "download-save-raw", "import-save-button",
                    "edit-save", "restore-save-snapshot", "clear-save"
                  ].map(id => document.getElementById(id).disabled),
                  semantics: {
                    statusRole: document.getElementById("save-status")
                      .getAttribute("role"),
                    statusLive: document.getElementById("save-status")
                      .getAttribute("aria-live"),
                    dialogLabel: document.getElementById("save-dialog")
                      .getAttribute("aria-labelledby"),
                    dropRole: document.getElementById("save-drop")
                      .getAttribute("role"),
                    dropTabIndex: document.getElementById("save-drop").tabIndex
                  },
                  presentation: {
                    value: document.getElementById("mode").value,
                    labels: Array.from(
                      document.getElementById("mode").options,
                      option => option.textContent
                    )
                  }
                }))()""",
                lambda item: isinstance(item, dict) and item.get("blocked"),
                "the deliberate WebGPU rejection",
                args.timeout,
            )
            require(not gate["uiHidden"], f"save UI is hidden without WebGPU: {gate}")
            require(
                gate["controls"] == [False] * 6,
                f"save controls are disabled without WebGPU: {gate}",
            )
            require(
                gate["semantics"]
                == {
                    "statusRole": "status",
                    "statusLive": "polite",
                    "dialogLabel": "save-dialog-title",
                    "dropRole": "button",
                    "dropTabIndex": 0,
                },
                f"save accessibility semantics changed: {gate}",
            )
            require(
                gate["presentation"]
                == {
                    "value": "restored",
                    "labels": [
                        "Restored — original look, modern fidelity",
                        "Remastered — art-directed effects (work in progress)",
                        "Pure — original 4:3, no enhancements",
                    ],
                },
                "browser presentation choices are stale or misleading: "
                f"{gate['presentation']}",
            )

            keyboard = cdp.evaluate(
                """(() => {
                  const input = document.getElementById("import-save-input");
                  const drop = document.getElementById("save-drop");
                  let activations = 0;
                  input.click = () => { activations++; };
                  for (const key of ["Enter", " "]) {
                    drop.dispatchEvent(new KeyboardEvent("keydown", {
                      key, bubbles: true, cancelable: true
                    }));
                  }
                  return {activations, focused: (drop.focus(), document.activeElement.id)};
                })()"""
            )
            require(
                keyboard == {"activations": 2, "focused": "save-drop"},
                f"keyboard import target is not operable: {keyboard}",
            )

            blank = bytes(IMAGE_SIZE)
            seed_raw(cdp, blank)

            # Build a valid, semantically distinct candidate through the actual
            # editor and prove modal focus is both captured and restored.
            cdp.evaluate(
                """(() => {
                  const button = document.getElementById("edit-save");
                  button.focus();
                  button.click();
                })()"""
            )
            wait_value(
                cdp,
                """(() => {
                  const dialog = document.getElementById("save-dialog");
                  return dialog.open ? document.activeElement.id : "";
                })()""",
                lambda value: value == "save-dialog-close",
                "editor modal focus",
                args.timeout,
            )
            pairs = ax_roles(cdp)
            require(
                any(
                    role == "dialog"
                    and name.casefold() == "edit saved progress"
                    for role, name in pairs
                ),
                "save editor is absent from Chromium's accessibility tree: "
                + repr(
                    sorted(
                        pair for pair in pairs
                        if pair[0] in {"dialog", "button", "heading"}
                        and ("save" in pair[1].lower()
                             or "edit" in pair[1].lower())
                    )
                ),
            )
            require(
                ("button", "Close save manager") in pairs,
                "save editor close control has no accessible name",
            )
            click_button(cdp, "#save-dialog-body", "Create fresh slot")
            edited = cdp.evaluate(
                """(() => {
                  const input = document.querySelector(
                    '#save-dialog-body input[type="text"][maxlength="3"]');
                  if (!input) return false;
                  input.value = "DKR";
                  input.dispatchEvent(new Event("change", {bubbles: true}));
                  const preset = [...document.querySelectorAll(
                    "#save-dialog-body button")].find(
                      item => item.textContent.includes("Unlock Adventure Two"));
                  if (!preset) return false;
                  preset.click();
                  return !document.getElementById("save-edit-apply").disabled;
                })()"""
            )
            require(edited is True, "editor did not produce an applicable draft")
            click_button(cdp, "#save-dialog-actions", "Review changes")
            review = cdp.evaluate(
                """(() => ({
                  count: document.getElementById("save-edit-change-count").textContent,
                  diff: document.getElementById("save-edit-diff").textContent
                }))()"""
            )
            require(
                "bytes will change" in review.get("count", "")
                and "Adventure Two" in review.get("diff", "")
                and "File 1" in review.get("diff", ""),
                f"semantic editor review is incomplete: {review}",
            )
            click_button(cdp, "#save-dialog-actions", "Apply changes")
            wait_dialog_status(
                cdp, "verified", "editor transaction", args.timeout
            )
            candidate = browser_bytes(cdp)
            require(candidate is not None and candidate != blank, "editor wrote no data")
            summary = cdp.evaluate(
                "MDKRSaveUI.testApi.summary()", await_promise=True
            )
            require(
                summary["slots"][0]["status"] == "valid"
                and summary["slots"][0]["name"] == "DKR"
                and summary["config"]["adventureTwo"] == 1,
                f"editor semantic result is wrong: {summary}",
            )
            cdp.evaluate(
                """(() => {
                  const button = document.getElementById("save-dialog-close");
                  button.focus();
                  button.click();
                })()"""
            )
            wait_value(
                cdp,
                "document.activeElement.id",
                lambda value: value == "edit-save",
                "modal focus return",
                args.timeout,
            )

            # Automatic snapshots are independently discoverable and restore
            # through the same review/rollback transaction as an imported file.
            cdp.evaluate(
                "MDKRSaveUI.testApi.seedAutosave(0, new Uint8Array(512))",
                await_promise=True,
            )
            cdp.evaluate(
                'document.getElementById("restore-save-snapshot").click()'
            )
            wait_value(
                cdp,
                """(() => {
                  const dialog = document.getElementById("save-dialog");
                  return dialog.open ? dialog.textContent : "";
                })()""",
                lambda value: "Automatic snapshot 1" in value
                and "File 1: empty" in value,
                "automatic snapshot chooser",
                args.timeout,
            )
            click_button(
                cdp, "#save-dialog-body", "Review automatic snapshot 1"
            )
            wait_value(
                cdp,
                'document.getElementById("save-dialog-body").textContent',
                lambda value: "Import candidate" in value
                and "Currently stored" in value,
                "automatic snapshot comparison",
                args.timeout,
            )
            cdp.evaluate("window.confirm = () => true")
            click_button(
                cdp, "#save-dialog-actions", "Replace saved progress"
            )
            wait_dialog_status(
                cdp, "verified", "automatic snapshot restore", args.timeout
            )
            require_storage(cdp, blank, "automatic snapshot restore")
            cdp.evaluate(
                f"MDKRSaveUI.testApi.replace(new Uint8Array({list(candidate)}))",
                await_promise=True,
            )
            cdp.evaluate('document.getElementById("save-dialog").close()')

            downloads = profile / "downloads"
            downloads.mkdir()
            cdp.call(
                "Browser.setDownloadBehavior",
                {"behavior": "allow", "downloadPath": str(downloads)},
            )
            cdp.evaluate('document.getElementById("download-save").click()')
            portable_path = wait_download(
                downloads, ".mdkr-save", "portable save", args.timeout
            )
            portable = json.loads(portable_path.read_text(encoding="utf-8"))
            exported_payload = base64.b64decode(
                portable["payload"], validate=True
            )
            require(
                exported_payload == candidate
                and portable["sha256"] == hashlib.sha256(candidate).hexdigest(),
                "portable export does not contain the exact editor result",
            )
            cdp.evaluate('document.getElementById("download-save-raw").click()')
            raw_path = wait_download(
                downloads, ".eep", "raw save", args.timeout
            )
            require(raw_path.read_bytes() == candidate, "raw export differs")

            browser_import_path = portable_path
            if cli_path is not None:
                native_raw = profile / "native-import.eep"
                native_container = profile / "native-export.mdkr-save"
                native_raw_export = profile / "native-export.eep"
                commands = (
                    [str(cli_path), "import", str(portable_path), str(native_raw)],
                    [str(cli_path), "export", str(native_raw),
                     str(native_container)],
                    [str(cli_path), "export-raw", str(native_container),
                     str(native_raw_export)],
                )
                for command in commands:
                    result = subprocess.run(
                        command,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        check=False,
                    )
                    require(
                        result.returncode == 0,
                        f"native save interchange failed ({' '.join(command)}): "
                        f"{result.stderr.strip()}",
                    )
                require(
                    native_raw.read_bytes() == candidate
                    and native_raw_export.read_bytes() == candidate,
                    "native import/export changed browser save bytes",
                )
                inspected = subprocess.run(
                    [str(cli_path), "inspect", str(native_container)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    check=False,
                )
                require(
                    inspected.returncode == 0
                    and json.loads(inspected.stdout) == summary,
                    "native and browser semantic summaries differ: "
                    + inspected.stderr.strip(),
                )
                browser_import_path = native_container

            # Every transaction seam must reject and preserve the exact prior
            # image, including failures after the new bytes reached IndexedDB.
            fault_points = cdp.evaluate(
                "Array.from(MDKRSaveUI.testApi.faultPoints)"
            )
            require(
                fault_points == list(EXPECTED_FAULT_POINTS),
                f"browser fault-point contract changed: {fault_points}",
            )
            for point in fault_points:
                result = cdp.evaluate(
                    f"""(async () => {{
                      __mdkrTestConfig.saveFault = {json.dumps(point)};
                      let rejected = false;
                      let message = "";
                      try {{
                        await MDKRSaveUI.testApi.replace(new Uint8Array(512));
                      }} catch (error) {{
                        rejected = true;
                        message = String(error && error.message || error);
                      }}
                      const stored = await MDKRSaveUI.testApi.snapshot();
                      return {{
                        rejected, message,
                        armed: __mdkrTestConfig.saveFault,
                        stored: Array.from(stored || [])
                      }};
                    }})()""",
                    await_promise=True,
                    timeout=30,
                )
                require(
                    result["rejected"]
                    and point in result["message"]
                    and result["armed"] is None
                    and bytes(result["stored"]) == candidate,
                    f"fault {point} did not roll back exactly: {result}",
                )

            # Malformed and oversized imports are rejected before storage moves.
            invalid_inputs = [
                ("short.eep", bytes(511), "malformed"),
                ("long.eep", bytes(513), "malformed"),
                ("oversized.mdkr-save", bytes(MAX_INPUT + 1), "64 kib"),
                (
                    "wrong-hash.mdkr-save",
                    portable_container(candidate, digest="0" * 64),
                    "digest",
                ),
                (
                    "wrong-version.mdkr-save",
                    portable_container(candidate, version=2),
                    "unsupported",
                ),
                (
                    "bad-base64.mdkr-save",
                    portable_container(candidate, payload_text="***"),
                    "payload",
                ),
                ("malformed.mdkr-save", b'{"format":', "malformed"),
                (
                    "duplicate.mdkr-save",
                    portable_container(candidate).replace(
                        b'"version":1,', b'"version":1,"version":1,', 1
                    ),
                    "malformed",
                ),
            ]
            for name, data, marker in invalid_inputs:
                outcome = import_memory(cdp, data, name)
                require(
                    not outcome["dialogOpen"]
                    and marker in outcome["status"].lower()
                    and bytes(outcome["stored"]) == candidate,
                    f"invalid import {name} was not fail-closed: {outcome}",
                )

            hostile_marker = '<img src=x onerror="globalThis.__mdkrSavePwned=true">'
            hostile = portable_container(
                candidate,
                created_at=hostile_marker,
                app_version="</dd><script>globalThis.__mdkrSavePwned=true</script>",
                source="web & local",
            )
            hostile_outcome = import_memory(
                cdp, hostile, "hostile-metadata.mdkr-save", "application/json"
            )
            require(
                hostile_outcome["dialogOpen"]
                and hostile_marker in hostile_outcome["bodyText"]
                and "Backup details (informational)" in hostile_outcome["bodyText"]
                and hostile_outcome["bodyElementCount"] == 0
                and not hostile_outcome["pwned"]
                and bytes(hostile_outcome["stored"]) == candidate,
                f"metadata preview is unsafe or incomplete: {hostile_outcome}",
            )
            close_dialog(cdp)

            # Corrupt stored data remains downloadable byte-for-byte, while the
            # portable-valid label/export is withheld.
            corrupt = bytearray(candidate)
            corrupt[0] ^= 1
            seed_raw(cdp, bytes(corrupt))
            portable_count = len(list(downloads.glob("*.mdkr-save")))
            cdp.evaluate(
                "MDKRSaveUI.testApi.downloadPortable()", await_promise=True
            )
            require(
                len(list(downloads.glob("*.mdkr-save"))) == portable_count,
                "corrupt save produced an ordinary portable backup",
            )
            wait_save_status(
                cdp, "corrupt", "corrupt portable-export warning", args.timeout
            )
            raw_path.unlink()
            cdp.evaluate('document.getElementById("download-save-raw").click()')
            corrupt_raw = wait_download(
                downloads, ".eep", "corrupt forensic EEPROM", args.timeout
            )
            require(
                corrupt_raw.read_bytes() == bytes(corrupt),
                "corrupt forensic export changed the original bytes",
            )

            # Recovery resets only the selected corrupt block and retains every
            # independently checksummed valid block.
            seed_raw(cdp, blank)
            recovery = import_memory(cdp, bytes(corrupt), "recover.eep")
            require(
                recovery["dialogOpen"]
                and "corrupt" in recovery["dialogStatus"].lower(),
                f"corrupt import did not enter recovery: {recovery}",
            )
            replace_disabled = cdp.evaluate(
                """[...document.querySelectorAll("#save-dialog-actions button")]
                  .find(item => item.textContent.includes(
                    "Replace saved progress")).disabled"""
            )
            require(replace_disabled is True, "corrupt import replacement is enabled")
            click_button(cdp, "#save-dialog-actions", "Reset corrupt blocks")
            wait_dialog_status(
                cdp, "recovery draft", "recovery preview", args.timeout
            )
            cdp.evaluate("window.confirm = () => true")
            click_button(cdp, "#save-dialog-actions", "Replace saved progress")
            wait_dialog_status(
                cdp, "verified", "recovery transaction", args.timeout
            )
            recovered = browser_bytes(cdp)
            recovered_summary = cdp.evaluate(
                "MDKRSaveUI.testApi.summary()", await_promise=True
            )
            require(
                recovered is not None
                and recovered[:SLOT_SIZE] != bytes(corrupt[:SLOT_SIZE])
                and recovered[SLOT_SIZE:] == candidate[SLOT_SIZE:]
                and recovered_summary["slots"][0]["status"] == "empty",
                "corrupt-block recovery did not reset only the corrupt slot",
            )
            close_dialog(cdp)

            # Merge only slot one from the candidate into a blank destination.
            seed_raw(cdp, blank)
            merged_preview = import_memory(cdp, candidate, "merge-source.eep")
            require(
                merged_preview["dialogOpen"]
                and "Copy selected data" in merged_preview["bodyText"],
                f"block merge UI is absent: {merged_preview}",
            )
            selected = cdp.evaluate(
                """(() => {
                  const section = [...document.querySelectorAll(
                    "#save-dialog-body section")].find(
                      item => item.textContent.includes(
                        "Copy selected data into the current save"));
                  if (!section) return false;
                  const input = section.querySelector('input[type="checkbox"]');
                  input.checked = true;
                  input.dispatchEvent(new Event("change", {bubbles: true}));
                  return true;
                })()"""
            )
            require(selected is True, "could not select the first merge block")
            click_button(cdp, "#save-dialog-body", "Build merged preview")
            wait_dialog_status(
                cdp, "merged draft", "block-merge preview", args.timeout
            )
            click_button(cdp, "#save-dialog-actions", "Replace saved progress")
            wait_dialog_status(
                cdp, "verified", "block-merge transaction", args.timeout
            )
            merged = candidate[:SLOT_SIZE] + blank[SLOT_SIZE:]
            require_storage(cdp, merged, "block merge")
            close_dialog(cdp)

            # A one-field edit may touch its owning slot and checksum only.
            cdp.evaluate('document.getElementById("edit-save").click()')
            wait_value(
                cdp,
                'document.getElementById("save-dialog").open',
                lambda value: value is True,
                "one-field editor",
                args.timeout,
            )
            changed = cdp.evaluate(
                """(() => {
                  const input = document.querySelector(
                    '#save-dialog-body input[type="text"][maxlength="3"]');
                  input.value = "DKX";
                  input.dispatchEvent(new Event("change", {bubbles: true}));
                  return !document.getElementById("save-edit-apply").disabled;
                })()"""
            )
            require(changed is True, "one-field edit was not accepted")
            click_button(cdp, "#save-dialog-actions", "Apply changes")
            wait_dialog_status(
                cdp, "verified", "one-field editor transaction", args.timeout
            )
            one_field = browser_bytes(cdp)
            require(one_field is not None, "one-field edit removed the save")
            changed_offsets = [
                index
                for index, (before, after) in enumerate(zip(merged, one_field))
                if before != after
            ]
            require(
                changed_offsets
                and all(index < SLOT_SIZE for index in changed_offsets),
                f"one-field edit escaped its owning block: {changed_offsets}",
            )
            close_dialog(cdp)

            # Destructive cancellation changes nothing. Confirmation then
            # clears the complete store before a real file-input restore.
            cancel = cdp.evaluate(
                """(async () => {
                  let confirmations = 0;
                  window.confirm = () => { confirmations++; return false; };
                  document.getElementById("clear-save").click();
                  for (let attempt = 0;
                       attempt < 100 && confirmations === 0;
                       attempt++) {
                    await new Promise(resolve => setTimeout(resolve, 10));
                  }
                  const bytes = await MDKRSaveUI.testApi.snapshot();
                  return {confirmations, bytes: Array.from(bytes || [])};
                })()""",
                await_promise=True,
            )
            require(
                cancel["confirmations"] == 1
                and bytes(cancel["bytes"]) == one_field,
                f"cancelled erase changed storage: {cancel}",
            )
            cdp.evaluate(
                """(() => {
                  window.confirm = () => true;
                  document.getElementById("clear-save").click();
                })()"""
            )
            wait_save_status(
                cdp, "erased", "confirmed save erase", args.timeout
            )
            require_storage(cdp, None, "confirmed erase")
            erased_recovery = cdp.evaluate(
                """(async () => {
                  const automatic = await MDKRSaveUI.testApi.autosaves();
                  const previous = await MDKRSaveUI.testApi.previous();
                  return {
                    automatic: automatic.map(bytes => bytes === null),
                    previous: previous === null,
                  };
                })()""",
                await_promise=True,
            )
            require(
                erased_recovery == {
                    "automatic": [True, True, True],
                    "previous": True,
                },
                "confirmed erase retained local recovery data: "
                f"{erased_recovery}",
            )

            # Use the actual hidden file input rather than the test bridge for
            # the wipe→import path, then prove IDBFS persistence on navigation.
            select_file_input(cdp, "#import-save-input", browser_import_path)
            wait_value(
                cdp,
                """(() => {
                  const button = [...document.querySelectorAll(
                    "#save-dialog-actions button")].find(
                      item => item.textContent.includes(
                        "Replace saved progress"));
                  return document.getElementById("save-dialog").open &&
                    !!button && !button.disabled;
                })()""",
                lambda value: value is True,
                "real file-input import preview",
                args.timeout,
            )
            click_button(cdp, "#save-dialog-actions", "Replace saved progress")
            wait_dialog_status(
                cdp, "verified", "real file-input import", args.timeout
            )
            require_storage(cdp, candidate, "wipe-to-file-import")
            close_dialog(cdp)

            cdp.call("Page.navigate", {"url": server.origin + "/?save-check=reload"})
            wait_save_status(
                cdp, "ready", "save-only reload", args.timeout
            )
            require_storage(cdp, candidate, "save-only reload")
            blocked_after_reload = wait_value(
                cdp,
                'document.getElementById("play").dataset.blocked === "1"',
                lambda value: value is True,
                "WebGPU gate after save reload",
                args.timeout,
            )
            require(blocked_after_reload is True, "WebGPU unexpectedly became available")

            requested_paths = [request.path for request in server.requests]
            require(
                any("mdkr-save-tools.js" in path for path in requested_paths)
                and any("mdkr-save-tools.wasm" in path for path in requested_paths),
                f"save module artifacts were not requested: {requested_paths}",
            )
            require(
                not any("mdkr64_web" in path for path in requested_paths),
                f"save-only gate loaded the game engine: {requested_paths}",
            )
            require(
                all(
                    request.method in {"GET", "HEAD"}
                    and request.content_length in {0, -1}
                    for request in server.requests
                ),
                "save test issued an upload or request body",
            )
            remote_http = [
                request.get("url", "")
                for request in cdp.network
                if str(request.get("url", "")).startswith(("http://", "https://"))
                and not str(request.get("url", "")).startswith(server.origin)
            ]
            require(not remote_http, f"save workflow contacted remote URLs: {remote_http}")
            require(not cdp.failures, "browser/CDP failures: " + "; ".join(cdp.failures))
            require(
                not cdp.exceptions,
                "unhandled browser exceptions: " + "; ".join(cdp.exceptions),
            )

            print(
                "check_browser_save_ui: PASS — WebGPU/ROM/engine independent; "
                f"{len(fault_points)} transaction faults rolled back; "
                f"{len(invalid_inputs)} malformed inputs rejected; exports, "
                + ("native interchange, " if cli_path is not None else "")
                + "metadata, recovery points, autosave restore, merge, "
                "one-field edit, accessibility, "
                "wipe→file import→reload, and zero-upload audit passed",
                flush=True,
            )
        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()
            server.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-dir", default="build-web")
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--chrome")
    parser.add_argument(
        "--cli",
        help="optional mdkr-save executable for native↔browser interchange",
    )
    parser.add_argument(
        "--chrome-flag",
        action="append",
        default=[],
        help="extra Chromium flag (repeatable)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=60.0,
        help="seconds allowed for each browser milestone",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run_check(args)
        return 0
    except (CheckFailure, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"check_browser_save_ui: FAIL — {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
