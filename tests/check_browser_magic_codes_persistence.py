#!/usr/bin/env python3
"""Reject one real Magic Code IDBFS commit, then prove durable reload."""

from __future__ import annotations

import argparse
import tempfile
from pathlib import Path

from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, OverlayServer, ROM_BYTES,
    add_config_script, click_play, find_chrome, page_websocket,
    persist_snapshot, require, resolve_path, select_rom, snapshot,
    wait_launcher, wait_value,
)
from check_browser_taj_persistence import require_clean_browser, wait_console


ROOT = Path(__file__).resolve().parent.parent
ENTRY_SCRIPT = ROOT / "tests/input_scripts/magic_codes_accept_reject.txt"
RELOAD_SCRIPT = ROOT / "tests/input_scripts/nav_to_magic_codes.txt"
STATE_TEXT = (
    "magic_codes_version=1\nunlocked=00000010\nactive=00000010\n"
    "checksum=5F049711"
)


def sidecar_text(cdp: CDPClient) -> str | None:
    value = cdp.evaluate("""(() => {
      const mod = globalThis.__mdkrTestState && globalThis.__mdkrTestState.module;
      if (!mod || !mod.FS) return null;
      try { return mod.FS.readFile('/save/magic_codes_state.ini', {encoding: 'utf8'}); }
      catch (_) { return null; }
    })()""")
    return value if isinstance(value, str) else None


def run(args: argparse.Namespace) -> None:
    rom = resolve_path(args.rom)
    shell_dir = resolve_path(args.shell_dir)
    engine_dir = resolve_path(args.engine_dir)
    chrome_path = find_chrome(args.chrome)
    for label, path in (
        ("ROM", rom), ("entry fixture", ENTRY_SCRIPT),
        ("reload fixture", RELOAD_SCRIPT),
        ("web shell", shell_dir / "index.html"),
        ("wasm loader", engine_dir / "mdkr64_web.js"),
        ("wasm module", engine_dir / "mdkr64_web.wasm"),
    ):
        require(path.is_file(), f"missing {label}: {path}")
    require(rom.stat().st_size == ROM_BYTES, f"ROM is not 12 MiB: {rom}")

    server = OverlayServer(shell_dir, engine_dir)
    server.start()
    with tempfile.TemporaryDirectory(prefix="mdkr64_magic_persist_chrome_") as profile:
        chrome = ChromeProcess(chrome_path, Path(profile), args.chrome_flag,
                               args.verbose)
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Network", "Log", "Inspector"):
                cdp.call(f"{domain}.enable")
            preload = add_config_script(cdp, {
                "headlessFrames": 3650,
                "inputScript": ENTRY_SCRIPT.read_text(encoding="ascii"),
                "env": {"MDKR_AUDIO": "0", "MDKR_TRACE": "1"},
            })
            cdp.call("Page.navigate", {
                "url": server.origin + "/?browser-magic-persistence=1&mode=restored"
            })
            wait_launcher(cdp, args.timeout)
            select_rom(cdp, rom, args.timeout)
            click_play(cdp)
            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && globalThis.__mdkrTestSnapshot().phase",
                lambda value: value == "main-started", "wasm main() start",
                args.timeout)

            hooked = cdp.evaluate("""(() => {
              const mod = globalThis.__mdkrTestState && globalThis.__mdkrTestState.module;
              if (!mod || typeof mod.__mdkrPersist !== 'function' ||
                  typeof mod._magic_codes_report_persistence_failure !== 'function' ||
                  typeof mod._magic_codes_report_persistence_success !== 'function') return false;
              const persist = mod.__mdkrPersist;
              let rejected = false;
              mod.__mdkrPersist = (options) => {
                if (!rejected && options && options.reason === 'magic-codes') {
                  rejected = true;
                  return Promise.reject(new Error('injected Magic Code IDBFS rejection'));
                }
                return persist(options);
              };
              return true;
            })()""")
            require(hooked is True,
                    "could not install real Magic Code persist rejection")
            wait_console(cdp, "magic_code_submit: accepted=1 id=4", args.timeout)
            wait_console(cdp, "[MAGIC CODES] browser storage rejected",
                         args.timeout)
            require(sidecar_text(cdp) == STATE_TEXT,
                    "rejected commit did not retain the exact MEMFS sidecar")

            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && globalThis.__mdkrTestSnapshot().phase",
                lambda value: value in {"exited", "aborted", "main-threw"},
                "finite Magic Code browser run", args.timeout)
            final = snapshot(cdp)
            require(final.get("phase") == "exited" and final.get("exitCode") == 0,
                    f"failed-persist run did not exit cleanly: {final}")
            require_clean_browser(cdp, "failed-persist Magic Code run")
            flushed = persist_snapshot(cdp, args.timeout)
            require(flushed.get("error") is None,
                    f"ordinary retry flush failed: {flushed.get('error')}")
            durable = cdp.evaluate("""(async () => {
              const bytes = await idbReadFile('/save', '/save/magic_codes_state.ini');
              return bytes ? new TextDecoder().decode(bytes) : null;
            })()""", await_promise=True, timeout=args.timeout)
            require(durable == STATE_TEXT,
                    "ordinary flush did not durably retain Magic Code state")

            cdp.call("Page.removeScriptToEvaluateOnNewDocument",
                     {"identifier": preload})
            start = len(cdp.console)
            reload_preload = add_config_script(cdp, {
                "headlessFrames": 2050,
                "inputScript": RELOAD_SCRIPT.read_text(encoding="ascii"),
                "env": {"MDKR_AUDIO": "0", "MDKR_TRACE": "1"},
            })
            cdp.call("Page.reload", {"ignoreCache": True})
            wait_launcher(cdp, args.timeout)
            click_play(cdp)
            wait_console(cdp,
                         "magic_codes_restore: unlocked=00000010 active=00000010",
                         args.timeout, start)
            require(not any("magic_code_submit: accepted=1" in line
                            for line in cdp.console[start:]),
                    "reload re-entered a code instead of restoring the sidecar")
            require_clean_browser(cdp, "persisted Magic Code reload")
            cdp.call("Page.removeScriptToEvaluateOnNewDocument",
                     {"identifier": reload_preload})
        finally:
            if cdp is not None:
                cdp.close()
            chrome.close()
            server.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-dir", default="build-web")
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=240.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        print("check_browser_magic_codes_persistence: PASS -- rejected IDBFS "
              "commit retained the selected code and restored it after reload")
        return 0
    except (CheckFailure, OSError, ValueError) as error:
        print(f"check_browser_magic_codes_persistence: FAIL -- {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
