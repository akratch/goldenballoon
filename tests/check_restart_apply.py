#!/usr/bin/env python3
"""Exercise real process replacement and recovery from deep Unicode paths.

The test copies the native executable and a legally supplied ROM to a private
extracted-layout fixture whose package directory contains a space, exactly as a
player's install does. The first autoplay session asks the production app to
perform one of its two real post-engine transitions:

* Restart & Apply, covering successful replacement plus post-replacement boot
  and handoff-staging failures. Each failure must clear the one-shot controls,
  return to the visible launcher recovery, and preserve the copied ROM and
  settings.
* Return to Launcher, which stages no ROM handoff and only asks main() to
  exec-replace the process with a launcher. The replacement must arrive as a
  launcher invocation -- not as an argument-bearing automation invocation,
  which arg_triage sends straight to the windowless engine -- and must still
  find the player's durable ROM selection.

No test seam replaces the executable or bypasses main_app.cpp.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from harness_utils import resolve_binary


TICKS = 12


def clean_environment(**updates: str) -> dict[str, str]:
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(updates)
    return env


def deep_directory(root: Path) -> Path:
    path = root / "Restart Apply — 日本語 🎈"
    # Windows extended-path handling is a release requirement. POSIX is covered
    # too, while staying far below its usual PATH_MAX and individual-name caps.
    while len(str(path)) < 300:
        path /= "long-unicode-path-évidence-0123456789"
    path.mkdir(parents=True)
    return path


def copy_package_executable(binary: Path, destination: Path) -> Path:
    package = destination / "Golden Balloon Extracted"
    package.mkdir()
    copied = package / binary.name
    shutil.copy2(binary, copied)
    # Windows packages keep SDL and other non-system imports beside the EXE.
    # Copy only dynamic-library siblings; source/build data never enters the
    # fixture and the test remains a real executable replacement.
    for library in binary.parent.glob("*.dll"):
        shutil.copy2(library, package / library.name)
    return copied


def fail(message: str, output: str) -> int:
    print(f"FAIL Restart & Apply: {message}", file=sys.stderr)
    print(output[-7000:], file=sys.stderr)
    return 1


def run_case(binary: Path, rom: Path, root: Path, name: str,
             restart_probe: str, *, force_stage_failure: bool = False,
             verbose: bool = False, timeout: int = 120) -> tuple[int, str, Path, Path]:
    deep = deep_directory(root / name)
    copied_binary = copy_package_executable(binary, deep)
    copied_rom = deep / "ROM 日本語 🎈" / "Diddy Kong Racing.z64"
    copied_rom.parent.mkdir()
    shutil.copy2(rom, copied_rom)
    prefs = deep / "preferences é"
    saves = deep / "saves 日本語"
    prefs.mkdir()
    saves.mkdir()
    # A real launcher remembers the validated selection in its own preference
    # store. Seed that durable state with the same deep Unicode ROM so recovery
    # has to restore a usable selection after the one-shot handoff is cleared.
    escaped_rom = str(copied_rom).replace("\\", "\\\\")
    (prefs / "mdkr64_app.ini").write_text(
        f"rom_path={escaped_rom}\n", encoding="utf-8")
    config = deep / "settings 🎈" / "mdkr64.ini"
    config.parent.mkdir()
    # This restart-scoped value is pre-existing file state, not an autoplay
    # override. Every replacement/recovery process must resolve it from the
    # same Unicode path after exec.
    config.write_text("Video.FrameLimit=240\n", encoding="utf-8")

    env = clean_environment(
        LC_ALL="C",
        MDKR_APP_AUTOPLAY="1",
        MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
        MDKR_APP_PREFS_DIR=str(prefs),
        MDKR_APP_TEST_RESTART_APPLY=restart_probe,
        MDKR_APP_TEST_RESTART_RECOVERY_FRAMES="4",
        MDKR_AUDIO="0",
        MDKR_ROM=str(copied_rom),
        MDKR_SAVE_DIR=str(saves),
        MDKR_VIDEO_CONFIG_PATH=str(config),
        MDKR64_HIDDEN="1",
    )
    if force_stage_failure:
        env["MDKR_APP_TEST_RESTART_STAGE_FAILURE"] = "1"
    if verbose:
        print(f"$ {copied_binary}  # {name}", flush=True)
    try:
        process = subprocess.run(
            [str(copied_binary)], cwd=deep, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        return 1, f"could not complete package-like launch: {error}", config, copied_rom
    return process.returncode, process.stdout or "", config, copied_rom


def require_invocation_shapes(output: str, shapes: tuple[str, ...]) -> str | None:
    """The exact invocation each process in the exec chain received.

    A relaunch that reaches the replacement as several arguments (the Windows
    CRT joins argv into a command line without quoting, so any space in the
    install path splits it) is classified as automation and runs headless: to
    the player the whole application closed. This is the shape assertion that
    makes that classification visible on every platform.
    """
    observed = tuple(
        line.split("invoked ", 1)[1].strip()
        for line in output.splitlines()
        if "[app-restart-test] invoked " in line
    )
    if observed != shapes:
        return f"invocation shapes were {observed!r}, expected {shapes!r}"
    return None


def require_preserved_config(config: Path, output: str) -> str | None:
    if "Video.FrameLimit=240" not in config.read_text(encoding="utf-8"):
        return "restart did not preserve the settings file"
    if "Diddy Kong Racing.z64" not in output:
        return "restart output did not retain the Unicode ROM path"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-rel")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.expanduser().resolve()
    if not binary.is_file():
        parser.error(f"missing native app binary: {binary}")
    if not rom.is_file():
        parser.error(f"missing ROM: {rom}")

    with tempfile.TemporaryDirectory(prefix="mdkr64-restart-apply-") as temporary:
        root = Path(temporary)
        code, output, config, copied_rom = run_case(
            binary, rom, root, "success", "1", verbose=args.verbose,
            timeout=args.timeout)
        if code != 0:
            return fail(f"successful replacement path exited {code}", output)
        if "[app-restart-test] requesting Restart & Apply for active ROM" not in output:
            return fail("initial process did not request production restart", output)
        if output.count("[app] Restart & Apply handoff accepted") != 1:
            return fail("replacement did not consume exactly one restart handoff", output)
        expected_boot = (
            f"[app-restart-test] replacement boot rom={copied_rom} frameLimit=240")
        if expected_boot not in output:
            return fail("replacement did not retain the Unicode ROM/config state", output)
        if output.count("[SDL] headless: reached 12 simulation ticks, exiting cleanly.") != 2:
            return fail("did not observe one completed engine run on each side of exec", output)
        # The launched process is bare; its replacement must arrive as an
        # explicit launcher invocation, never as an automation invocation.
        if problem := require_invocation_shapes(
                output, ("argc=1 ui=0 automation=0", "argc=2 ui=1 automation=0")):
            return fail(problem, output)
        if problem := require_preserved_config(config, output):
            return fail(problem, output)

        code, output, config, copied_rom = run_case(
            binary, rom, root, "boot failure", "boot-failure",
            verbose=args.verbose, timeout=args.timeout)
        if code != 0:
            return fail(f"post-restart boot recovery exited {code}", output)
        for marker in (
            "[app] Restart & Apply handoff accepted",
            "[app-restart-test] forcing post-restart boot failure",
            "[app] Restart & Apply recovery: Restart & Apply could not start the game.",
            "[app] boot recovery visible: Restart & Apply could not start the game.",
        ):
            if marker not in output:
                return fail(f"post-restart boot recovery missing {marker!r}", output)
        if output.count("[app] Restart & Apply handoff accepted") != 1:
            return fail("post-restart failure reused a stale handoff marker", output)
        if (f"[app-restart-test] recovery launcher rom={copied_rom} "
                "bootRecovery=1") not in output:
            return fail("post-restart recovery lost the durable ROM selection", output)
        if problem := require_invocation_shapes(
                output, ("argc=1 ui=0 automation=0", "argc=2 ui=1 automation=0",
                         "argc=2 ui=1 automation=0")):
            return fail(problem, output)
        if problem := require_preserved_config(config, output):
            return fail(problem, output)

        code, output, config, copied_rom = run_case(
            binary, rom, root, "stage failure", "stage-failure",
            force_stage_failure=True, verbose=args.verbose, timeout=args.timeout)
        if code != 0:
            return fail(f"restart staging recovery exited {code}", output)
        for marker in (
            "[app-restart-test] requesting Restart & Apply for active ROM",
            "[app] Restart & Apply recovery: Restart & Apply could not preserve the active ROM.",
            "[app] boot recovery visible: Restart & Apply could not preserve the active ROM.",
        ):
            if marker not in output:
                return fail(f"restart staging recovery missing {marker!r}", output)
        if "[app] Restart & Apply handoff accepted" in output:
            return fail("forced staging failure created a restart handoff", output)
        if (f"[app-restart-test] recovery launcher rom={copied_rom} "
                "bootRecovery=1") not in output:
            return fail("staging recovery lost the durable ROM selection", output)
        if problem := require_invocation_shapes(
                output, ("argc=1 ui=0 automation=0", "argc=2 ui=1 automation=0")):
            return fail(problem, output)
        if problem := require_preserved_config(config, output):
            return fail(problem, output)

        # Return to Launcher: the overlay's other exit. It stages no ROM
        # handoff, so the entire behaviour is "exec-replace me with a
        # launcher" -- the exact transition a player reported as closing the
        # whole application instead of returning.
        code, output, config, copied_rom = run_case(
            binary, rom, root, "return to launcher", "return-to-launcher",
            verbose=args.verbose, timeout=args.timeout)
        if code != 0:
            return fail(f"return-to-launcher replacement exited {code}", output)
        if "[app-restart-test] requesting Return to Launcher" not in output:
            return fail("initial process did not request the production return "
                        "transition", output)
        if problem := require_invocation_shapes(
                output, ("argc=1 ui=0 automation=0", "argc=2 ui=1 automation=0")):
            return fail(problem, output)
        # The replacement drew real launcher frames. A headless automation
        # invocation cannot produce this marker at all, which is what makes
        # "the whole app closed" a failure here rather than a silent pass.
        if (f"[app-restart-test] recovery launcher rom={copied_rom} "
                "bootRecovery=0") not in output:
            return fail("return to launcher did not reopen a launcher holding "
                        "the durable ROM selection", output)
        if "[app] Restart & Apply handoff accepted" in output:
            return fail("return to launcher staged a restart handoff", output)
        # One engine run only: the replacement is a launcher, not another race.
        if output.count(
                "[SDL] headless: reached 12 simulation ticks, exiting cleanly.") != 1:
            return fail("return to launcher re-entered the engine", output)
        if problem := require_preserved_config(config, output):
            return fail(problem, output)

    print("check_restart_apply: PASS -- successful replacement, post-restart "
          "boot/staging recovery, and Return to Launcher each reopened a real "
          "launcher and preserved Unicode/deep-path ROM and settings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
