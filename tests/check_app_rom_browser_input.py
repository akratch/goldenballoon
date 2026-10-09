#!/usr/bin/env python3
"""Exercise first-run ROM selection through SDL's virtual gamepad and ImGui.

The file is intentionally invalid game data: the test proves that the focused
Choose ROM action opens the in-window browser, the same gamepad selects its
entry, and full validation rejects it. No ROM is shipped or required.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from harness_utils import resolve_binary


def run_app(*args, **kwargs) -> subprocess.CompletedProcess:
    """Keep recoverable ImGui errors and timeout output visible to the gate."""
    try:
        result = subprocess.run(*args, **kwargs)
    except subprocess.TimeoutExpired as exc:
        output = exc.output or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        raise SystemExit(
            f"ROM browser subprocess timed out after {exc.timeout} seconds:\n"
            + output[-7000:]) from exc
    if "[imgui-error]" in (result.stdout or ""):
        raise SystemExit("ROM browser reported an ImGui error:\n"
                         + result.stdout[-7000:])
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", help="local original ROM for a valid-selection route")
    parser.add_argument("--window-size", default="640x480",
                        choices=("1280x800", "800x600", "640x480"))
    parser.add_argument("--runner", help="optional executable prefix for a cross-architecture AppImage (for example qemu-x86_64-static)")
    args = parser.parse_args()
    executable = Path(resolve_binary(args.build)).resolve()
    if not executable.is_file():
        parser.error(f"missing executable: {executable}")
    if args.runner and not shutil.which(args.runner):
        parser.error(f"missing runner: {args.runner}")
    launch = [args.runner, str(executable)] if args.runner else [str(executable)]

    with tempfile.TemporaryDirectory(prefix="mdkr-rom-browser-input-") as temp:
        root = Path(temp)
        downloads = root / "Downloads"
        downloads.mkdir()
        candidate = downloads / "invalid.z64"
        candidate.write_bytes(b"not a game ROM")
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
            "MDKR_APP_PANEL": "Play",
            "MDKR_APP_SMOKE_WINDOW_SIZE": args.window_size,
            "MDKR_APP_SMOKE_FRAMES": "60",
            "MDKR_APP_SMOKE_INPUT": "gamepad",
            "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
            "MDKR_APP_SMOKE_ROM_BROWSER": str(candidate),
            "MDKR_APP_SMOKE_ROM_BROWSER_TOKEN": "mdkr64-app-rom-browser-v1",
            "MDKR64_HIDDEN": "1",
            "MDKR_AUDIO": "0",
        })
        result = run_app(
            launch, cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=180, check=False,
        )
        expected = (
            "controller mappings from",
            "[app-ui-test] virtual SDL gamepad connected",
            "[app-ui-test] Choose ROM gamepad press queued=1",
            "[app-ui-test] ROM entry gamepad press queued=1",
            "[app-ui-test] ROM browser gamepad opened=1 modal=1 selected=1",
            f"candidate={candidate} invalid=1",
        )
        missing = [text for text in expected if text not in result.stdout]
        if result.returncode or missing:
            raise SystemExit(
                f"ROM browser gamepad smoke failed: exit={result.returncode} "
                f"missing={missing}\n{result.stdout[-7000:]}")

        # Production accepts all controllers, not just SDL's first one. An
        # idle virtual pad enumerates before the active smoke pad here; this
        # reproduces a built-in/Steam virtual/external pad ordering hazard.
        env["MDKR_APP_SMOKE_GAMEPAD_AUTOALL"] = "1"
        multi = run_app(
            launch, cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=180, check=False,
        )
        multi_missing = [text for text in expected if text not in multi.stdout]
        if multi.returncode or multi_missing:
            raise SystemExit(
                f"ROM browser second-gamepad smoke failed: exit={multi.returncode} "
                f"missing={multi_missing}\n{multi.stdout[-7000:]}")
        del env["MDKR_APP_SMOKE_GAMEPAD_AUTOALL"]

        # A Deck user's Downloads folder can contain more entries than fit in
        # the modal. Reach the last, initially offscreen ROM through repeated
        # real D-pad presses; first-entry success alone would miss a broken
        # child-window navigation/scroll handoff.
        filler = [downloads / f"{index:03d}.z64" for index in range(24)]
        for path in filler:
            path.write_bytes(b"not a game ROM")
        env["MDKR_APP_SMOKE_ROM_BROWSER_DEEP"] = "1"
        deep = run_app(
            launch, cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=180, check=False,
        )
        deep_missing = [text for text in expected if text not in deep.stdout]
        if "scrolled=1" not in deep.stdout:
            deep_missing.append("scrolled=1")
        if deep.returncode or deep_missing:
            raise SystemExit(
                f"ROM browser offscreen gamepad smoke failed: exit={deep.returncode} "
                f"missing={deep_missing}\n{deep.stdout[-7000:]}")
        del env["MDKR_APP_SMOKE_ROM_BROWSER_DEEP"]
        for path in filler:
            path.unlink()

        env["MDKR_APP_SMOKE_ROM_BROWSER_ROOT"] = "1"
        rooted = run_app(
            launch, cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=180, check=False,
        )
        if (rooted.returncode or
                "rootVisited=1 rootClosed=1" not in rooted.stdout or
                "candidate=(none)" not in rooted.stdout):
            raise SystemExit(
                f"ROM browser Root/B-close failed: exit={rooted.returncode}\n"
                + rooted.stdout[-7000:])

        # First launch often starts in an empty Downloads directory. With no
        # file Selectable, Up must receive focus so Root stays gamepad-reachable.
        candidate.unlink()
        env["MDKR_APP_SMOKE_ROM_BROWSER_EMPTY_START"] = "1"
        empty_root = run_app(
            launch, cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=180, check=False,
        )
        if (empty_root.returncode or
                "rootVisited=1 rootClosed=1" not in empty_root.stdout or
                "candidate=(none)" not in empty_root.stdout):
            raise SystemExit(
                f"ROM browser empty Downloads/Root failed: exit={empty_root.returncode}\n"
                + empty_root.stdout[-7000:])
        del env["MDKR_APP_SMOKE_ROM_BROWSER_EMPTY_START"]
        del env["MDKR_APP_SMOKE_ROM_BROWSER_ROOT"]
        candidate.write_bytes(b"not a game ROM")

        folder = downloads / "a-folder"
        folder.mkdir()
        env["MDKR_APP_SMOKE_ROM_BROWSER_NAV_FOLDER"] = str(folder)
        navigated = run_app(
            launch, cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=180, check=False,
        )
        nav_expected = (
            "[app-ui-test] ROM browser gamepad opened=1 modal=1 selected=1 "
            "folderOpened=1 back=1",
            f"candidate={candidate} invalid=1",
        )
        nav_missing = [text for text in nav_expected
                       if text not in navigated.stdout]
        if navigated.returncode or nav_missing:
            raise SystemExit(
                f"ROM browser back-navigation failed: exit={navigated.returncode} "
                f"missing={nav_missing}\n{navigated.stdout[-7000:]}")

        env["MDKR_APP_SMOKE_ROM_BROWSER_TOKEN"] = "stale-token"
        refused = run_app(
            launch, cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=30, check=False,
        )
        if (refused.returncode != 2 or
                "ROM browser requires the gamepad input contract" not in refused.stdout):
            raise SystemExit(
                "ROM browser accepted a stale smoke token:\n"
                + refused.stdout[-3000:])

        if args.rom:
            source = Path(args.rom).resolve()
            if not source.is_file():
                parser.error(f"missing original ROM: {source}")
            candidate.unlink()
            folder.rmdir()
            valid = downloads / "valid.z64"
            shutil.copyfile(source, valid)
            env["MDKR_APP_SMOKE_ROM_BROWSER"] = str(valid)
            env["MDKR_APP_SMOKE_ROM_BROWSER_TOKEN"] = "mdkr64-app-rom-browser-v1"
            env["MDKR_APP_SMOKE_ROM_BROWSER_EXPECT_VALID"] = "1"
            del env["MDKR_APP_SMOKE_ROM_BROWSER_NAV_FOLDER"]
            accepted = run_app(
                launch, cwd=root, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=180, check=False,
            )
            accepted_expected = (
                "[app-ui-test] ROM browser gamepad opened=1 modal=1 selected=1",
                f"candidate=(none) invalid=0 active={valid} valid=1",
            )
            accepted_missing = [text for text in accepted_expected
                                if text not in accepted.stdout]
            if accepted.returncode or accepted_missing:
                raise SystemExit(
                    f"ROM browser valid selection failed: exit={accepted.returncode} "
                    f"missing={accepted_missing}\n{accepted.stdout[-7000:]}")
            del env["MDKR_APP_SMOKE_ROM_BROWSER"]
            del env["MDKR_APP_SMOKE_ROM_BROWSER_TOKEN"]
            del env["MDKR_APP_SMOKE_ROM_BROWSER_EXPECT_VALID"]
            del env["MDKR_APP_SMOKE_INPUT"]
            del env["MDKR_APP_SMOKE_INPUT_TOKEN"]
            env["MDKR_APP_SMOKE_ROM_REMEMBERED"] = str(valid)
            env["MDKR_APP_SMOKE_ROM_REMEMBERED_TOKEN"] = (
                "mdkr64-app-rom-remembered-v1")
            remembered = run_app(
                launch, cwd=root, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=180, check=False,
            )
            witness = f"[app-ui-test] remembered ROM path={valid} ready=1"
            if remembered.returncode or witness not in remembered.stdout:
                raise SystemExit(
                    f"ROM browser remembered selection failed: "
                    f"exit={remembered.returncode}\n{remembered.stdout[-7000:]}")
    print("PASS: ROM picker gamepad routes, invalid rejection"
          + (", valid selection and fresh-process restore" if args.rom else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
