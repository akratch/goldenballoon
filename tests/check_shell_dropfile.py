#!/usr/bin/env python3
"""Q2: drag-and-drop ROM acquisition through the SDL_DROPFILE handler path.

Why this exists
---------------
The 1.0 ROM-picker rewrite deleted `rom_scan.{h,cpp}` and left exactly three
manual, permission-free acquisition paths (see docs/APP_SHELL.md "Getting a
ROM in"): drag-and-drop (`SDL_DROPFILE`), the native open panel, and a typed
path. The panel and typed-path arms both call `RomPanel_setRom()` directly and
are covered by `tests/test_app_shell.cpp`'s ROM-validator unit tests. The drop
arm is different: it is reached by an `SDL_DROPFILE` event flowing
through `AppHost::pumpAndShouldQuit()` -> `Launcher::draw()` ->
`AppHost::takeDroppedFile()` -> `RomPanel_setRom()`, and nothing exercised that
plumbing end to end — it was hand-verified once on a release DMG and never
re-verified after the rewrite.

`MDKR_APP_SMOKE_DROP=<path>` (platform/app/main_app.cpp, inside the existing
`MDKR_APP_SMOKE_FRAMES` headless launcher loop) queues exactly one
`SDL_DROPFILE` for `AppHost::pumpAndShouldQuit()` partway through the run. The
event is materialized after SDL's platform translation boundary, then uses the
same handler and ownership contract as a live Finder/Explorer drag. This is not
a direct call to the panel function, and it avoids round-tripping a reserved
platform event through SDL2-on-SDL3 compatibility layers, whose drop payload
layouts differ. The loop then prints what the launcher ended up holding:

    [app] smoke: drop requested=<path> got=<path> valid=<0|1>
    [app] smoke: drop message=<RomInfo.message>

This check drives that hook in both directions:

* **accepted** — a real supported ROM is dropped; the process must survive,
  report the SAME path back, `valid=1`, and a message naming this build as
  supporting it (the identical `mdkr_validate_rom()` verdict any of the other
  three acquisition paths would produce for the same file), and the ROM must
  be persisted to prefs exactly as an NSOpenPanel/typed-path accept would be.
* **rejected (the broken direction)** — a garbage, non-ROM file is dropped;
  the process must still survive (exit 0, no crash/sanitizer marker),
  `valid=0`, a non-empty explanatory message, and nothing persisted to prefs.

Prefs isolation
----------------
`RomPanel_setRom()` persists an accepted ROM via `AppConfig::save()`, which
writes to `SDL_GetPrefPath("mdkr64", "mdkr64")` — one location shared by every
local build and every concurrent test/dev session on the machine (see
tests/README.md and CONTRIBUTING.md). This check is the first automated path
that can reach a *successful* `AppConfig::save()` (the pre-existing
`app_shell_smoke` CTest never drops a file, so it only ever reads prefs via
`RomPanel_ensureInit()`), so every run sets `MDKR_APP_PREFS_DIR` to a private
temporary directory — a test-only override added alongside this check
(platform/app/app_config.cpp), mirroring `MDKR_SAVE_DIR`
(platform/stubs_dkr.c) for the same reason. The real shared prefs file is
never opened by this check.

Always muted (`MDKR_AUDIO=0`) and offscreen (`MDKR64_HIDDEN=1`) per
tests/README.md / CONTRIBUTING.md. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional

from harness_utils import resolve_binary

ROOT = Path(__file__).resolve().parent.parent

FRAMES = 4  # matches the app_shell_smoke CTest's MDKR_APP_SMOKE_FRAMES=4

DROP_RE = re.compile(r"^\[app\] smoke: drop requested=(.*) got=(.*) valid=(\d+)$")
MESSAGE_PREFIX = "[app] smoke: drop message="
RENDERED_RE = re.compile(r"^\[app\] smoke: rendered (\d+) frames")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)

# Deliberately not a ROM: right ballpark of size, wrong magic in every byte
# order dkr_rom_normalize_byte_order() checks. Exercises the "someone dropped
# a random file" verdict, not the separate "too small to be a ROM" one.
GARBAGE_BYTES = (b"not-an-n64-rom-garbage-drop-fixture-" * 4)[:128]


class DropResult:
    def __init__(self, returncode: int, output: str):
        self.returncode = returncode
        self.output = output
        self.requested: Optional[str] = None
        self.got: Optional[str] = None
        self.valid: Optional[int] = None
        self.message: Optional[str] = None
        self.rendered_frames: Optional[int] = None
        for line in output.splitlines():
            m = DROP_RE.match(line)
            if m:
                self.requested, self.got, valid_str = m.groups()
                self.valid = int(valid_str)
                continue
            if line.startswith(MESSAGE_PREFIX):
                self.message = line[len(MESSAGE_PREFIX):]
                continue
            m2 = RENDERED_RE.match(line)
            if m2:
                self.rendered_frames = int(m2.group(1))


def run_drop(binary: Path, drop_path: Path, prefs_dir: Path, timeout: int,
             verbose: bool) -> DropResult:
    prefs_dir.mkdir(parents=True, exist_ok=True)
    # Clear every inherited MDKR*/GE007_ hook (tests/README.md contract) so no
    # ambient environment variable changes what this smoke does, then set
    # exactly what this check needs.
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR64_HIDDEN="1",
        MDKR_APP_SMOKE_FRAMES=str(FRAMES),
        MDKR_APP_SMOKE_DROP=str(drop_path),
        MDKR_APP_PREFS_DIR=str(prefs_dir),
    )
    command = [str(binary)]
    if verbose:
        print(f"$ MDKR_APP_SMOKE_DROP={drop_path} {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    return DropResult(process.returncode, process.stdout or "")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []

    with tempfile.TemporaryDirectory(prefix="mdkr-shell-dropfile-") as tmp:
        root = Path(tmp)
        garbage = root / "garbage_drop.z64"
        garbage.write_bytes(GARBAGE_BYTES)

        accept_prefs = root / "prefs-accept"
        reject_prefs = root / "prefs-reject"

        accept = run_drop(binary, rom, accept_prefs, args.timeout, args.verbose)
        reject = run_drop(binary, garbage, reject_prefs, args.timeout, args.verbose)

        # Read back inside the `with` block — TemporaryDirectory deletes the
        # whole tree, prefs included, on exit.
        accept_ini = accept_prefs / "mdkr64_app.ini"
        accept_ini_exists = accept_ini.exists()
        accept_ini_text = accept_ini.read_text() if accept_ini_exists else ""
        reject_ini_exists = (reject_prefs / "mdkr64_app.ini").exists()

    # --- direction 1: a real ROM dropped must be accepted ------------------
    if accept.returncode != 0:
        failures.append(
            f"accepted-ROM arm exited {accept.returncode} (must be 0, no "
            f"crash on a real drop)\n{accept.output[-2000:]}")
    bad = BAD_RE.search(accept.output)
    if bad:
        failures.append(f"accepted-ROM arm printed a fatal marker: {bad.group(0)!r}")
    if accept.rendered_frames != FRAMES:
        failures.append(
            f"accepted-ROM arm rendered {accept.rendered_frames} frames, want {FRAMES} "
            "(the loop must keep going after the drop, not stop early)")
    if accept.requested is None:
        failures.append(
            "accepted-ROM arm printed no '[app] smoke: drop requested=' line — "
            "the SDL_DROPFILE handler never reached AppHost::takeDroppedFile()/"
            f"RomPanel_setRom()\n{accept.output[-2000:]}")
    else:
        if accept.requested != str(rom):
            failures.append(
                f"accepted-ROM arm requested path mismatch: {accept.requested!r} "
                f"vs {rom!s}")
        if accept.got != str(rom):
            failures.append(
                "accepted-ROM arm: LauncherState.romPath does not equal the "
                f"dropped path exactly — the picker path is not honoured: "
                f"got={accept.got!r} want={rom!s}")
        if accept.valid != 1:
            failures.append(
                f"accepted-ROM arm: valid={accept.valid}, want 1 for a "
                f"supported ROM (mdkr_validate_rom message: {accept.message!r})")
        if not accept.message or "this build supports" not in accept.message:
            failures.append(
                "accepted-ROM arm message does not read as an accepted verdict "
                f"(same RomInfo.message any picker path would show): "
                f"{accept.message!r}")
    if accept.valid == 1:
        if not accept_ini_exists:
            failures.append(
                "accepted-ROM arm: nothing persisted to the isolated prefs dir — "
                "a drop-accepted ROM must be remembered exactly like an "
                "NSOpenPanel/typed-path accept (RomPanel_setRom -> AppConfig::save)")
        elif f"rom_path={rom}" not in accept_ini_text:
            failures.append(
                f"accepted-ROM arm: prefs do not contain the dropped ROM's "
                f"path\n{accept_ini_text}")

    # --- direction 2 (broken direction): garbage dropped must be REJECTED,
    # gracefully — no crash, an explanatory error, nothing persisted --------
    if reject.returncode != 0:
        failures.append(
            f"garbage-drop arm exited {reject.returncode} (must be 0 — a bad "
            f"drop must be refused, not crash)\n{reject.output[-2000:]}")
    bad = BAD_RE.search(reject.output)
    if bad:
        failures.append(f"garbage-drop arm printed a fatal marker: {bad.group(0)!r}")
    if reject.rendered_frames != FRAMES:
        failures.append(
            f"garbage-drop arm rendered {reject.rendered_frames} frames, want "
            f"{FRAMES} (the launcher must keep rendering after a refused drop)")
    if reject.requested is None:
        failures.append(
            "garbage-drop arm printed no '[app] smoke: drop requested=' line\n"
            f"{reject.output[-2000:]}")
    else:
        if reject.valid != 0:
            failures.append(
                f"garbage-drop arm: valid={reject.valid}, want 0 for a "
                f"non-ROM file (message: {reject.message!r})")
        if not reject.message:
            failures.append(
                "garbage-drop arm: no error message surfaced for the refused "
                "drop — a rejected ROM must explain itself, not fail silently")
    if reject_ini_exists:
        failures.append(
            f"garbage-drop arm: a refused ROM was persisted to prefs "
            f"({reject_prefs / 'mdkr64_app.ini'})")

    if failures:
        print("check_shell_dropfile: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(
        "check_shell_dropfile: PASS — SDL_DROPFILE handler accepted "
        f"{rom.name} (valid=1, persisted) and refused {garbage.name} "
        "(valid=0, no crash, nothing persisted)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
