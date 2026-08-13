#!/usr/bin/env python3
"""Prove every launcher/controller/room code asset receives one build stamp."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
STAMP = "stamp-fixture-123"
VERSION = "1.3.0"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    source = ROOT / "dist/web"
    script = ROOT / "tools/web/stamp_publish.sh"
    with tempfile.TemporaryDirectory(prefix="mdkr-web-stamp-") as temporary:
        staged = Path(temporary) / "web"
        shutil.copytree(source, staged)
        subprocess.run([str(script), "--dir", str(staged), "--stamp", STAMP,
                        "--version", VERSION], cwd=ROOT, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True)
        expected = {
            "index.html": [
                "style.css", "input/touch-surface.css", "party/party-host.css",
                "online/online-room.css", "manifest.webmanifest", "rom-id.js",
                "mdkr-save-ui.js", "input/touch-surface.js",
                "party/party-protocol.js", "party/party-sas.js",
                "party/qrcodegen.js", "party/party-host.js",
                "online/online-control-config.js",
                "online/online-room-live-state.js",
                "online/online-room-presenter.js", "online/online-room.js",
                "mdkr64-shell.js",
            ],
            "controller/index.html": [
                "../input/touch-surface.css", "controller.css",
                "../party/party-protocol.js", "../party/party-sas.js",
                "../input/touch-surface.js", "controller.js",
            ],
            "room/index.html": ["room-entry.css", "room-entry.js"],
        }
        for relative, assets in expected.items():
            text = (staged / relative).read_text(encoding="utf-8")
            for asset in assets:
                require(text.count(f'{asset}?v={STAMP}') == 1,
                        f"{relative} did not stamp {asset} exactly once")
            require(text.count(f'data-build-stamp="{STAMP}"') == 1,
                    f"{relative} did not receive one document build stamp")
            require("?v=" + STAMP + "?v=" not in text,
                    f"{relative} contains a double build stamp")
        index = (staged / "index.html").read_text(encoding="utf-8")
        require(f'<html lang="en" data-build-version="{VERSION}" '
                f'data-build-stamp="{STAMP}">' in index,
                "root document did not receive the release version")
        worker = (staged / "sw.js").read_text(encoding="utf-8")
        require('"online/online-room-live-state.js?v=" + BUILD' in worker,
                "offline shell does not precache the live-state boundary")
        require('"online/online-room-presenter.js?v=" + BUILD' in worker,
                "offline shell does not precache the Online Room presenter")

        missing = Path(temporary) / "missing-route"
        shutil.copytree(source, missing)
        (missing / "room/index.html").unlink()
        failed = subprocess.run(
            [str(script), "--dir", str(missing), "--stamp", STAMP,
             "--version", VERSION], cwd=ROOT, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True)
        require(failed.returncode != 0 and "no room/index.html" in failed.stdout,
                "publisher did not fail closed when the room route was absent")
    print("check_web_publish_stamp: PASS — root, controller and room assets are "
          "single-stamped; missing role route fails closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
