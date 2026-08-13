#!/usr/bin/env python3
"""End-to-end proof of the issue #33 portable-mode config write.

A Windows player whose account name contains non-ASCII characters cannot write
settings under %APPDATA% because that path lives below an un-encodable home
directory. The owner-approved fix is a portable.txt marker beside the
executable: when present, config (mdkr64.ini), save/, and mods/ resolve next to
the executable instead of the per-user home directory, sidestepping the home
path entirely.

This check drives the real built binary from a *different* working directory
with a clean environment (no MDKR_VIDEO_CONFIG_PATH / MDKR_SAVE_DIR, no HOME
leak) and asserts:

  portable   With portable.txt beside the binary, mdkr64.ini is written NEXT TO
             THE BINARY and never in the working directory.
  control    Without the marker, the same invocation keeps the historical
             CWD-relative spelling -- proving the marker, not the launch
             directory, is what relocates the write.

The non-ASCII-home reproduction itself is a real-Windows CI concern
(.github/workflows/windows-validate.yml); the path *logic* it exercises is what
this cross-platform check pins on every runner.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary  # noqa: E402

TIMEOUT = 60


def clean_environment() -> dict[str, str]:
    """Every MDKR_* stripped so no inherited override selects the config path,
    and HOME redirected to an empty scratch dir so nothing leaks from the
    developer's real home."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("MDKR_")}
    env.pop("MDKR_VIDEO_CONFIG_PATH", None)
    env.pop("MDKR_SAVE_DIR", None)
    env.update(
        MDKR64_HIDDEN="1",
        MDKR_AUDIO="0",
        MDKR_NO_CRASH_HANDLER="1",
        SDL_VIDEODRIVER="dummy",
        SDL_AUDIODRIVER="dummy",
    )
    return env


def run_write(install_dir: Path, work_dir: Path, home: Path) -> tuple[int, str]:
    command = [
        str(install_dir / "mdkr64"),
        "--video-launch-persist",
        "--video-launch-mode", "remastered",
        "--video-list",
    ]
    env = clean_environment()
    env["HOME"] = str(home)
    try:
        proc = subprocess.run(
            command, cwd=work_dir, env=env,
            text=True, capture_output=True, timeout=TIMEOUT, check=False,
        )
    except subprocess.TimeoutExpired as exc:
        return 124, (exc.stdout or "") + (exc.stderr or "")
    return proc.returncode, proc.stdout + proc.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    source_binary = Path(resolve_binary(args.build)).resolve()
    if not source_binary.exists():
        print(f"FAIL: binary not found: {source_binary}", file=sys.stderr)
        return 1

    failures = 0
    with tempfile.TemporaryDirectory(prefix="mdkr_portable_") as root_str:
        root = Path(root_str)

        # --- portable arm: marker beside the binary relocates the write ---
        install = root / "portable" / "GoldenBalloon"
        work = root / "portable" / "run-cwd"
        home = root / "portable" / "empty-home"
        for path in (install, work, home):
            path.mkdir(parents=True)
        # The binary links its libraries by absolute path (no @executable_path
        # rpath), so a plain copy runs from any directory.
        shutil.copy2(source_binary, install / "mdkr64")
        (install / "portable.txt").write_text("portable\n", encoding="utf-8")

        rc, output = run_write(install, work, home)
        if args.verbose:
            print(output)
        beside = install / "mdkr64.ini"
        in_cwd = work / "mdkr64.ini"
        if rc != 0:
            print(f"FAIL: portable run exited {rc}", file=sys.stderr)
            failures += 1
        if not beside.is_file():
            print("FAIL: portable mode did not write mdkr64.ini next to the "
                  f"binary ({beside})", file=sys.stderr)
            failures += 1
        else:
            print(f"  ok: portable config written next to binary -> {beside}")
        if in_cwd.exists():
            print("FAIL: portable mode leaked mdkr64.ini into the working "
                  f"directory ({in_cwd})", file=sys.stderr)
            failures += 1

        # --- control arm: no marker keeps the CWD-relative spelling ---
        c_install = root / "control" / "GoldenBalloon"
        c_work = root / "control" / "run-cwd"
        c_home = root / "control" / "empty-home"
        for path in (c_install, c_work, c_home):
            path.mkdir(parents=True)
        shutil.copy2(source_binary, c_install / "mdkr64")

        rc, output = run_write(c_install, c_work, c_home)
        if args.verbose:
            print(output)
        c_beside = c_install / "mdkr64.ini"
        c_in_cwd = c_work / "mdkr64.ini"
        if rc != 0:
            print(f"FAIL: control run exited {rc}", file=sys.stderr)
            failures += 1
        if c_beside.exists():
            print("FAIL: without a marker, config was written next to the "
                  f"binary ({c_beside})", file=sys.stderr)
            failures += 1
        if not c_in_cwd.is_file():
            print("FAIL: without a marker, config was not written CWD-relative "
                  f"({c_in_cwd})", file=sys.stderr)
            failures += 1
        else:
            print(f"  ok: control config stayed CWD-relative -> {c_in_cwd}")

    if failures:
        print(f"portable paths: {failures} failure(s)", file=sys.stderr)
        return 1
    print("portable paths: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
