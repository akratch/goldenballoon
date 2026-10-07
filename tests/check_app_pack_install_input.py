#!/usr/bin/env python3
"""Install a ZIP pack through Content, ImGui, and an SDL virtual gamepad.

The local fixture contains only a pack.ini. No ROM or third-party pack art is
needed. macOS forces the Linux-style in-window ZIP picker only under a
versioned smoke token; Linux uses that picker for ordinary players.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

from harness_utils import resolve_binary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", help="unused; the ZIP fixture needs no ROM")
    parser.add_argument("--window-size", choices=("1280x800", "800x600", "640x480"),
                        help="exercise the Content picker in a smaller launcher window")
    args = parser.parse_args()
    executable = Path(resolve_binary(args.build)).resolve()
    if not executable.is_file():
        parser.error(f"missing executable: {executable}")

    with tempfile.TemporaryDirectory(prefix="mdkr-pack-install-input-") as temp:
        root = Path(temp)
        downloads = root / "Downloads"
        downloads.mkdir()
        fixture = downloads / "TestTextures.ZIP"
        with zipfile.ZipFile(fixture, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("pack.ini", "[pack]\nname=Test Textures\n")
        (root / "prefs").mkdir()
        (root / "saves").mkdir()
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update({
            "MDKR_APP_PREFS_DIR": str(root / "prefs"),
            "MDKR_VIDEO_CONFIG_PATH": str(root / "video.ini"),
            "MDKR_SAVE_DIR": str(root / "saves"),
            "MDKR_MODS_DIR": str(root / "mods"),
            "MDKR_CHARACTERS_DIR": str(root / "characters"),
            "MDKR_APP_PANEL": "Content",
            "MDKR_APP_SMOKE_FRAMES": "80",
            "MDKR_APP_SMOKE_INPUT": "gamepad",
            "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
            "MDKR_APP_SMOKE_PACK_BROWSER": str(fixture),
            "MDKR_APP_SMOKE_PACK_BROWSER_TOKEN": "mdkr64-app-pack-browser-v1",
            "MDKR64_HIDDEN": "1",
            "MDKR_AUDIO": "0",
        })
        if args.window_size:
            env["MDKR_APP_SMOKE_WINDOW_SIZE"] = args.window_size
        result = subprocess.run(
            [str(executable)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=180, check=False,
        )
        expected = (
            "[app-ui-test] virtual SDL gamepad connected",
            "[app-ui-test] Install pack gamepad press queued=1",
            "[app-ui-test] ZIP entry gamepad press queued=1",
            "[app-ui-test] pack browser gamepad opened=1 modal=1 "
            "selected=1 installed=1",
        )
        missing = [marker for marker in expected if marker not in result.stdout]
        installed = [root / "mods" / fixture.name,
                     root / "prefs" / "mods" / fixture.name]
        copies = [path for path in installed if path.is_file()]
        if result.returncode or missing or len(copies) != 1 or \
                copies[0].read_bytes() != fixture.read_bytes():
            raise SystemExit(
                f"pack-install gamepad smoke failed: exit={result.returncode} "
                f"missing={missing} copies={copies}\n{result.stdout[-7000:]}")

        env["MDKR_APP_SMOKE_PACK_BROWSER_TOKEN"] = "stale-token"
        refused = subprocess.run(
            [str(executable)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=30, check=False)
        if (refused.returncode != 2 or
                "pack browser requires the gamepad input contract" not in refused.stdout):
            raise SystemExit("pack picker accepted a stale token:\n" + refused.stdout[-3000:])

    print("PASS: controller-only Content picker installed a validated ZIP pack"
          f" ({args.window_size or 'default window'})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
