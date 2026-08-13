#!/usr/bin/env python3
"""Prove service-worker deploy skew remains build-atomic and offline-safe."""

from __future__ import annotations

import argparse
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

from check_browser_runtime import (
    CDPClient, ChromeProcess, CheckFailure, find_chrome, page_websocket, require,
    wait_value,
)


ROOT = Path(__file__).resolve().parent.parent
STAMPER = ROOT / "tools/web/stamp_publish.sh"
OLD = "deploy-old-001"
NEW = "deploy-new-002"


class SwitchingServer:
    def __init__(self, root: Path):
        self._root = root
        self._offline = False
        self._lock = threading.Lock()
        outer = self

        class Handler(SimpleHTTPRequestHandler):
            server_version = "mdkr64-publish-skew"

            def log_message(self, _format: str, *_args: Any) -> None:
                return

            def end_headers(self) -> None:
                self.send_header("Cache-Control", "no-store")
                self.send_header("Service-Worker-Allowed", "/")
                super().end_headers()

            def do_GET(self) -> None:  # noqa: N802 - stdlib callback name
                with outer._lock:
                    offline = outer._offline
                if offline:
                    try:
                        self.connection.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    self.connection.close()
                    return
                super().do_GET()

            def translate_path(self, path: str) -> str:
                clean = urllib.parse.unquote(
                    urllib.parse.urlsplit(path).path).lstrip("/")
                if not clean or clean.endswith("/"):
                    clean += "index.html"
                with outer._lock:
                    root = outer._root
                try:
                    target = (root / clean).resolve()
                    target.relative_to(root.resolve())
                    return str(target)
                except (OSError, ValueError):
                    return str(root / "__rejected__")

            def guess_type(self, path: str) -> str:
                if path.endswith(".wasm"):
                    return "application/wasm"
                return super().guess_type(path)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever,
                                       name="mdkr-publish-skew", daemon=True)

    @property
    def origin(self) -> str:
        host, port = self.httpd.server_address[:2]
        return f"http://{host}:{port}"

    def start(self) -> None:
        self.thread.start()

    def switch(self, root: Path) -> None:
        with self._lock:
            self._root = root

    def set_offline(self, value: bool) -> None:
        with self._lock:
            self._offline = value

    def close(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=3)


def stage(source: Path, target: Path, stamp: str) -> None:
    shutil.copytree(source, target)
    subprocess.run([str(STAMPER), "--dir", str(target), "--stamp", stamp,
                    "--version", "1.3.0"], cwd=ROOT, check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def document_state(cdp: CDPClient, expected: str, timeout: float) -> dict[str, Any]:
    return wait_value(cdp, """(() => ({
      ready:document.readyState,
      stamp:document.documentElement.dataset.buildStamp||'',
      shell:document.querySelector('script[src*="mdkr64-shell.js"]')?.src||'',
      controller:navigator.serviceWorker.controller?.scriptURL||''
    }))()""", lambda value: isinstance(value, dict) and
        value.get("ready") == "complete" and value.get("stamp") == expected,
        f"{expected} document", timeout)


def registration_state(cdp: CDPClient) -> dict[str, str]:
    return cdp.evaluate("""(async()=>{
      const deadline=Date.now()+15000;
      while(Date.now()<deadline){
        const r=await navigator.serviceWorker.getRegistration();
        const state={active:r?.active?.scriptURL||'',
          waiting:r?.waiting?.scriptURL||'',installing:r?.installing?.scriptURL||''};
        if(state.waiting)return state;
        await new Promise(resolve=>setTimeout(resolve,50));
      }
      const r=await navigator.serviceWorker.getRegistration();
      return {active:r?.active?.scriptURL||'',waiting:r?.waiting?.scriptURL||'',
        installing:r?.installing?.scriptURL||''};
    })()""", await_promise=True, timeout=20)


def run(args: argparse.Namespace) -> None:
    source = (ROOT / args.shell_dir).resolve()
    require((source / "sw.js").is_file(), "published shell has no service worker")
    with tempfile.TemporaryDirectory(prefix="mdkr-publish-skew-") as temporary:
        root = Path(temporary)
        old = root / "old"
        new = root / "new"
        stage(source, old, OLD)
        stage(source, new, NEW)
        server = SwitchingServer(old)
        chrome: ChromeProcess | None = None
        cdp: CDPClient | None = None
        try:
            server.start()
            chrome = ChromeProcess(find_chrome(args.chrome), root / "profile",
                                   args.chrome_flag, args.verbose)
            cdp = CDPClient(page_websocket(chrome.wait_port()))
            for domain in ("Page", "Runtime", "Log", "Inspector", "Network"):
                cdp.call(f"{domain}.enable")

            cdp.call("Page.navigate", {"url": server.origin + "/"})
            first = document_state(cdp, OLD, args.timeout)
            require(f"?v={OLD}" in first["shell"],
                    f"old document loaded a mismatched shell: {first}")
            active = cdp.evaluate(
                "navigator.serviceWorker.ready.then(r=>r.active.scriptURL)",
                await_promise=True, timeout=args.timeout)
            require(f"?v={OLD}" in active,
                    f"old service worker did not install: {active}")

            cdp.call("Page.navigate", {"url": server.origin + "/?cycle=controlled"})
            controlled_old = document_state(cdp, OLD, args.timeout)
            require(f"?v={OLD}" in controlled_old["controller"],
                    f"old release was not controlled by its worker: {controlled_old}")

            server.switch(new)
            cdp.call("Page.navigate", {"url": server.origin + "/?cycle=deploy"})
            skewed = document_state(cdp, NEW, args.timeout)
            require(f"?v={NEW}" in skewed["shell"] and
                    f"?v={OLD}" in skewed["controller"],
                    f"deploy skew was not old-worker/new-document: {skewed}")
            registration = registration_state(cdp)
            require(f"?v={OLD}" in registration["active"] and
                    f"?v={NEW}" in registration["waiting"],
                    f"new worker skipped the safe waiting phase: {registration}")
            caches = cdp.evaluate("caches.keys()", await_promise=True)
            require(f"mdkr64-shell-{OLD}" in caches and
                    f"mdkr64-shell-{NEW}" in caches,
                    f"waiting window lacked two isolated caches: {caches}")

            # The old worker has seen the new document, but must not have put it
            # in the old cache. With the server gone, reload must atomically
            # recover the complete old release—not new markup with unreachable
            # new assets.
            failures_before = len(cdp.failures)
            server.set_offline(True)
            cdp.call("Page.navigate", {"url": server.origin + "/?cycle=offline"})
            offline = document_state(cdp, OLD, args.timeout)
            require(f"?v={OLD}" in offline["shell"] and
                    f"?v={OLD}" in offline["controller"],
                    f"waiting-window offline recovery mixed releases: {offline}")
            server.set_offline(False)

            # Once every old controlled document is gone, the waiting worker may
            # activate, delete the old cache, and own a coherent new page.
            cdp.call("Page.navigate", {"url": "about:blank"})
            time.sleep(0.5)
            cdp.call("Page.navigate", {"url": server.origin + "/"})
            promoted = document_state(cdp, NEW, args.timeout)
            wait_value(cdp, "navigator.serviceWorker.controller?.scriptURL||''",
                       lambda value: isinstance(value, str) and f"?v={NEW}" in value,
                       "new active service worker", args.timeout)
            final_caches = cdp.evaluate("caches.keys()", await_promise=True)
            require(final_caches == [f"mdkr64-shell-{NEW}"],
                    f"activation retained a stale build cache: {final_caches}")
            require(f"?v={NEW}" in promoted["shell"],
                    f"promoted document did not retain new assets: {promoted}")
            require(not cdp.exceptions,
                    "publish-skew browser exceptions: " + "; ".join(cdp.exceptions))
            unexpected = [failure for failure in cdp.failures[failures_before:]
                          if "ERR_EMPTY_RESPONSE" not in failure]
            require(not unexpected,
                    "unexpected publish-skew network failures: " + "; ".join(unexpected))
        finally:
            if cdp is not None:
                cdp.close()
            if chrome is not None:
                chrome.close()
            server.close()

    print("check_browser_publish_skew: PASS — waiting worker, build-isolated "
          "caches, atomic old-build offline fallback and clean new-build activation")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shell-dir", default="dist/web")
    parser.add_argument("--chrome")
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
        return 0
    except (CheckFailure, AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"check_browser_publish_skew: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
