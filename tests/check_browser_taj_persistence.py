#!/usr/bin/env python3
"""Exercise Taj's real browser sidecar failure and durable reload path.

The picker gate proves the happy path.  This focused Chrome/WebGPU check
instead rejects the *actual* ``Module.__mdkrPersist({reason: "taj-mod"})``
Promise once, after the C sidecar has already replaced its MEMFS file.  The
subsequent ordinary shell flush must preserve the requested unlock, and a
fresh document must restore Taj without re-entering the code.
"""

from __future__ import annotations

import argparse
import tempfile
import time
from pathlib import Path

from check_browser_runtime import (
    CDPClient,
    ChromeProcess,
    CheckFailure,
    FATAL_MARKERS,
    OverlayServer,
    ROM_BYTES,
    add_config_script,
    click_play,
    find_chrome,
    page_websocket,
    persist_snapshot,
    require,
    resolve_path,
    select_rom,
    snapshot,
    wait_launcher,
    wait_value,
)


ROOT = Path(__file__).resolve().parent.parent
UNLOCK_SCRIPT = ROOT / "tests/input_scripts/taj_unlock_select.txt"
# The escaped opening line used to emit a literal "\" as the script's first
# line. platform_input_load_script() drops it silently (no second token), so
# nothing failed -- which is exactly why the loaded-entry count is asserted
# against RELOAD_SCRIPT_ENTRIES below rather than assumed.
RELOAD_SCRIPT = """\
1250 START 4
1330 START 4
1520 DOWN 4
1570 RIGHT 4
"""
RELOAD_SCRIPT_ENTRIES = len(
    [line for line in RELOAD_SCRIPT.splitlines() if line.strip()])
STATE_TEXT = (
    "mod_roster_version=3\ntaj_unlocked=1\n"
    "taj_migration_complete=1\nwizpig_unlocked=0\n"
    "wizpig_migration_complete=0\nterry_unlocked=0\n"
    "terry_migration_complete=0"
)


def wait_console(cdp: CDPClient, needle, timeout: float,
                 start: int = 0) -> str:
    """Wait for a console line: `needle` is a substring or a predicate."""
    matches = needle if callable(needle) else (lambda line: needle in line)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for line in cdp.console[start:]:
            if matches(line):
                return line
        time.sleep(0.08)
    raise CheckFailure(
        f"timed out waiting for {needle!r}; console tail:\n" +
        "\n".join(cdp.console[-40:]))


def require_clean_browser(cdp: CDPClient, label: str) -> None:
    """The same fatal-marker/exception surface the picker sibling asserts.

    This check injects one rejected persist Promise on purpose; that rejection
    is handled by the sidecar, so it must not surface as an uncaught page
    exception, a failed request, or an engine fatal. Without this the arm could
    pass while the page was throwing behind it.
    """
    console_text = "\n".join(cdp.console)
    fatal = [marker for marker in FATAL_MARKERS
             if marker.lower() in console_text.lower()]
    require(not fatal, f"{label} emitted fatal markers: {fatal}")
    require(not cdp.exceptions and not cdp.failures,
            f"{label} raised browser errors: exceptions={cdp.exceptions}, "
            f"failures={cdp.failures}")


def sidecar_text(cdp: CDPClient) -> str | None:
    value = cdp.evaluate("""(() => {
      const mod = globalThis.__mdkrTestState && globalThis.__mdkrTestState.module;
      if (!mod || !mod.FS) return null;
      try { return mod.FS.readFile('/save/taj_mod_state.ini', {encoding: 'utf8'}); }
      catch (_) { return null; }
    })()""")
    return value if isinstance(value, str) else None


def run(args: argparse.Namespace) -> None:
    rom = resolve_path(args.rom)
    shell_dir = resolve_path(args.shell_dir)
    engine_dir = resolve_path(args.engine_dir)
    chrome_path = find_chrome(args.chrome)
    for label, path in (
        ("ROM", rom),
        ("input fixture", UNLOCK_SCRIPT),
        ("web shell", shell_dir / "index.html"),
        ("web shell JavaScript", shell_dir / "mdkr64-shell.js"),
        ("wasm loader", engine_dir / "mdkr64_web.js"),
        ("wasm module", engine_dir / "mdkr64_web.wasm"),
    ):
        require(path.is_file(), f"missing {label}: {path}")
    require(rom.stat().st_size == ROM_BYTES, f"ROM is not 12 MiB: {rom}")

    server = OverlayServer(shell_dir, engine_dir)
    server.start()
    with tempfile.TemporaryDirectory(prefix="mdkr64_taj_persist_chrome_") as profile:
        chrome = ChromeProcess(chrome_path, Path(profile), args.chrome_flag,
                               args.verbose)
        cdp: CDPClient | None = None
        try:
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Network", "Log", "Inspector"):
                cdp.call(f"{domain}.enable")
            preload = add_config_script(cdp, {
                "headlessFrames": 4350,
                "inputScript": UNLOCK_SCRIPT.read_text(encoding="ascii"),
                "env": {"MDKR_AUDIO": "0", "MDKR_TRACE": "1"},
            })
            cdp.call("Page.navigate", {
                "url": server.origin + "/?browser-taj-persistence=1&mode=restored"
            })
            wait_launcher(cdp, args.timeout)
            select_rom(cdp, rom, args.timeout)
            click_play(cdp)
            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && globalThis.__mdkrTestSnapshot().phase",
                lambda value: value == "main-started",
                "wasm main() start", args.timeout)

            # Inject at the shell boundary actually called by the EM_JS bridge.
            # EEPROM and final-flush generations still use the unmodified owner.
            hooked = cdp.evaluate("""(() => {
              const mod = globalThis.__mdkrTestState && globalThis.__mdkrTestState.module;
              if (!mod || typeof mod.__mdkrPersist !== 'function' ||
                  typeof mod._taj_mod_report_persistence_failure !== 'function' ||
                  typeof mod._taj_mod_report_persistence_success !== 'function') return false;
              const persist = mod.__mdkrPersist;
              let rejected = false;
              mod.__mdkrPersist = (options) => {
                if (!rejected && options && options.reason === 'taj-mod') {
                  rejected = true;
                  return Promise.reject(new Error('injected Taj IDBFS rejection'));
                }
                return persist(options);
              };
              return true;
            })()""")
            require(hooked is True, "could not install real Taj persist rejection")

            wait_console(cdp, "magic_code_submit: accepted=1 id=-2", args.timeout)
            # The Wizpig/Terry extensible-roster work renamed the [TAJ]
            # markers to [ROSTER] and split the rejection wording into two
            # variants ("was not persisted" / "could not be committed").
            # The contract under test is the same either way: the rejected
            # commit is surfaced and the session stays alive.
            wait_console(
                cdp,
                lambda line: "[ROSTER] unlock state" in line and
                "session remains active" in line,
                args.timeout)
            memfs_state = sidecar_text(cdp)
            require(memfs_state == STATE_TEXT,
                    "failed Taj unlock did not retain its exact MEMFS sidecar: "
                    f"got {memfs_state!r}, expected {STATE_TEXT!r}")

            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && globalThis.__mdkrTestSnapshot().phase",
                lambda value: value in {"exited", "aborted", "main-threw"},
                "finite Taj browser run", args.timeout)
            final = snapshot(cdp)
            require(final.get("phase") == "exited" and final.get("exitCode") == 0,
                    f"failed-persist run did not exit cleanly: {final}")
            require_clean_browser(cdp, "failed-persist Taj run")
            flushed = persist_snapshot(cdp, args.timeout)
            require(flushed.get("error") is None,
                    f"ordinary retry flush failed: {flushed.get('error')}")
            durable = cdp.evaluate("""(async () => {
              const bytes = await idbReadFile('/save', '/save/taj_mod_state.ini');
              return bytes ? new TextDecoder().decode(bytes) : null;
            })()""", await_promise=True, timeout=args.timeout)
            require(durable == STATE_TEXT,
                    "ordinary retry did not durably retain the requested Taj unlock")

            # A document reload discards C static state. This proves recovery is
            # from IDBFS, not from the useful in-session unlock after rejection.
            cdp.call("Page.removeScriptToEvaluateOnNewDocument",
                     {"identifier": preload})
            start = len(cdp.console)
            reload_preload = add_config_script(cdp, {
                "headlessFrames": 1800,
                "inputScript": RELOAD_SCRIPT,
                "env": {"MDKR_AUDIO": "0", "MDKR_TRACE": "1"},
            })
            cdp.call("Page.reload", {"ignoreCache": True})
            wait_launcher(cdp, args.timeout)
            click_play(cdp)
            wait_value(
                cdp,
                "globalThis.__mdkrTestSnapshot && globalThis.__mdkrTestSnapshot().phase",
                lambda value: value == "main-started",
                "persisted-Taj reload main() start", args.timeout)
            # The reload arm is only meaningful if the engine actually received
            # the route it was given: a malformed line is dropped silently by
            # platform_input_load_script(), which would leave Taj restored but
            # never selected.
            loaded = wait_console(cdp, "[input-script] loaded ", args.timeout,
                                  start)
            require(
                f"loaded {RELOAD_SCRIPT_ENTRIES} entries" in loaded,
                f"reload route lost input entries: {loaded.strip()!r}, "
                f"expected {RELOAD_SCRIPT_ENTRIES}",
            )
            # The extensible-roster work (playable Wizpig and Terry) replaced
            # the taj_roster trace with mod_roster. Measured on this fixture's
            # fresh save: only Taj is present and enabled; the locked bonus
            # racers are absent from the roster entirely (wizpig/terry -1).
            wait_console(cdp, "mod_roster: base=8 count=9 taj=8 wizpig=-1 terry=-1 taj_enabled=1 wizpig_enabled=0 terry_enabled=0 drumstick=0 tt=0",
                         args.timeout, start)
            reload_console = cdp.console[start:]
            require(not any("magic_code_submit: accepted=1" in line
                            for line in reload_console),
                    "reload re-entered the magic-code route")
            require_clean_browser(cdp, "persisted-Taj reload")
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
        print("check_browser_taj_persistence: PASS -- real rejected Taj IDBFS "
              "commit retained the unlock and restored it after reload")
        return 0
    except (CheckFailure, OSError, ValueError) as error:
        print(f"check_browser_taj_persistence: FAIL -- {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
