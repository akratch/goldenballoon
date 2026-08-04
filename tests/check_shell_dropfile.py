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
* **final Play recheck** — after a valid asynchronous drop settles, the smoke
  requests the production Play recheck and proves it keeps rendering until one
  Play action carries that exact ROM path. A one-byte mutation between those
  two checks must instead produce no Play action.

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
TRANSACTION_RE = re.compile(
    r"^\[app\] smoke: drop transaction active=(.*) activeValid=(\d+) "
    r"candidateVisible=(\d+) cancelAvailable=(\d+) persistenceWarning=(\d+)$")
FINAL_PLAY_RE = re.compile(
    r"^\[app\] smoke: final Play recheck requested=(\d+) initialPath=(.*) "
    r"initialValid=(\d+) mutationApplied=(\d+) serviceFrames=(\d+) actions=(\d+) "
    r"actionRom=(.*) finalValid=(\d+) settled=(\d+)$")
REPLACEMENT_PLAY_RE = re.compile(
    r"^\[app\] smoke: replacement Play candidate=(.*) initialReady=(\d+) "
    r"replacementPending=(\d+) superseded=(\d+) serviceFrames=(\d+) "
    r"actions=(\d+) actionRom=(.*) settled=(\d+) active=(.*) "
    r"candidateVisible=(\d+)$")
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
        self.active: Optional[str] = None
        self.active_valid: Optional[int] = None
        self.candidate_visible: Optional[int] = None
        self.cancel_available: Optional[int] = None
        self.persistence_warning: Optional[int] = None
        self.play_requested: Optional[int] = None
        self.play_initial_path: Optional[str] = None
        self.play_initial_valid: Optional[int] = None
        self.play_mutation_applied: Optional[int] = None
        self.play_service_frames: Optional[int] = None
        self.play_actions: Optional[int] = None
        self.play_action_rom: Optional[str] = None
        self.play_final_valid: Optional[int] = None
        self.play_settled: Optional[int] = None
        self.replacement_play_candidate: Optional[str] = None
        self.replacement_play_initial_ready: Optional[int] = None
        self.replacement_play_pending: Optional[int] = None
        self.replacement_play_superseded: Optional[int] = None
        self.replacement_play_service_frames: Optional[int] = None
        self.replacement_play_actions: Optional[int] = None
        self.replacement_play_action_rom: Optional[str] = None
        self.replacement_play_settled: Optional[int] = None
        self.replacement_play_active: Optional[str] = None
        self.replacement_play_candidate_visible: Optional[int] = None
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
                continue
            m3 = TRANSACTION_RE.match(line)
            if m3:
                self.active = m3.group(1)
                self.active_valid = int(m3.group(2))
                self.candidate_visible = int(m3.group(3))
                self.cancel_available = int(m3.group(4))
                self.persistence_warning = int(m3.group(5))
                continue
            m4 = FINAL_PLAY_RE.match(line)
            if m4:
                (requested, self.play_initial_path, initial_valid, mutation_applied,
                 service_frames, actions, self.play_action_rom, final_valid,
                 settled) = m4.groups()
                self.play_requested = int(requested)
                self.play_initial_valid = int(initial_valid)
                self.play_mutation_applied = int(mutation_applied)
                self.play_service_frames = int(service_frames)
                self.play_actions = int(actions)
                self.play_final_valid = int(final_valid)
                self.play_settled = int(settled)
                continue
            m5 = REPLACEMENT_PLAY_RE.match(line)
            if m5:
                (self.replacement_play_candidate, initial_ready, pending,
                 superseded, service_frames, actions,
                 self.replacement_play_action_rom, settled,
                 self.replacement_play_active, candidate_visible) = m5.groups()
                self.replacement_play_initial_ready = int(initial_ready)
                self.replacement_play_pending = int(pending)
                self.replacement_play_superseded = int(superseded)
                self.replacement_play_service_frames = int(service_frames)
                self.replacement_play_actions = int(actions)
                self.replacement_play_settled = int(settled)
                self.replacement_play_candidate_visible = int(candidate_visible)


def run_drop(binary: Path, drop_path: Path, prefs_dir: Path, timeout: int,
             verbose: bool, create_prefs: bool = True,
             final_play: bool = False, mutate_before_play: bool = False,
             replacement_play: Optional[Path] = None) -> DropResult:
    if create_prefs:
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
    if final_play:
        env["MDKR_APP_SMOKE_DROP_PLAY"] = "1"
    if mutate_before_play:
        env["MDKR_APP_SMOKE_DROP_PLAY_MUTATE"] = "1"
    if replacement_play is not None:
        env["MDKR_APP_SMOKE_REPLACEMENT_PLAY"] = str(replacement_play)
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
        source_bytes = rom.read_bytes()
        truncated = root / "supported_header_but_truncated.z64"
        truncated.write_bytes(source_bytes[:0x40])
        corrupted = root / "supported_header_but_corrupted.z64"
        corrupted_bytes = bytearray(source_bytes)
        corrupted_bytes[len(corrupted_bytes) // 2] ^= 0x01
        corrupted.write_bytes(corrupted_bytes)
        final_play_rom = root / "final_play_candidate.z64"
        final_play_rom.write_bytes(source_bytes)
        final_play_mutated_rom = root / "final_play_mutated_candidate.z64"
        final_play_mutated_rom.write_bytes(source_bytes)
        replacement_play_rom = root / "replacement_play_candidate.z64"
        replacement_play_rom.write_bytes(source_bytes)

        accept_prefs = root / "prefs-accept"
        reject_prefs = root / "prefs-reject"

        accept = run_drop(binary, rom, accept_prefs, args.timeout, args.verbose)
        reject = run_drop(binary, garbage, reject_prefs, args.timeout, args.verbose)
        replacement = run_drop(binary, garbage, accept_prefs, args.timeout, args.verbose)
        truncated_replacement = run_drop(
            binary, truncated, accept_prefs, args.timeout, args.verbose)
        corrupted_replacement = run_drop(
            binary, corrupted, accept_prefs, args.timeout, args.verbose)
        unremembered_prefs = root / "missing-parent" / "prefs"
        unremembered = run_drop(
            binary, rom, unremembered_prefs, args.timeout, args.verbose,
            create_prefs=False)
        final_play = run_drop(
            binary, final_play_rom, root / "prefs-final-play", args.timeout,
            args.verbose, final_play=True)
        final_play_mutated = run_drop(
            binary, final_play_mutated_rom, root / "prefs-final-play-mutated",
            args.timeout, args.verbose, final_play=True, mutate_before_play=True)
        replacement_play = run_drop(
            binary, rom, root / "prefs-replacement-play", args.timeout,
            args.verbose, replacement_play=replacement_play_rom)

        # Read back inside the `with` block — TemporaryDirectory deletes the
        # whole tree, prefs included, on exit.
        accept_ini = accept_prefs / "mdkr64_app.ini"
        accept_ini_exists = accept_ini.exists()
        accept_ini_text = accept_ini.read_text() if accept_ini_exists else ""
        reject_ini_exists = (reject_prefs / "mdkr64_app.ini").exists()
        replacement_ini_text = accept_ini.read_text() if accept_ini.exists() else ""
        unremembered_prefs_exists = unremembered_prefs.exists()

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

    # No existing playable selection can be harmed on a first-run preference
    # failure. Keep the verified ROM available for this process and make the
    # fact that its path was not remembered explicit.
    if unremembered.returncode != 0:
        failures.append(
            f"session-only ROM arm exited {unremembered.returncode}\n"
            f"{unremembered.output[-2000:]}")
    if (unremembered.active != str(rom) or unremembered.active_valid != 1 or
            unremembered.candidate_visible != 0 or
            unremembered.persistence_warning != 1):
        failures.append(
            "valid first ROM was not retained session-locally after preference "
            "failure: "
            f"active={unremembered.active!r} valid={unremembered.active_valid} "
            f"candidate={unremembered.candidate_visible} "
            f"warning={unremembered.persistence_warning}")
    if unremembered_prefs_exists:
        failures.append("session-only ROM arm unexpectedly created preference data")

    # --- final Play recheck: a settled valid selection must be checked again
    # asynchronously, render through the check, and emit exactly one action --
    for label, result, expected_actions, expected_final_valid, mutation in (
        ("final Play", final_play, 1, 1, 0),
        ("mutated final Play", final_play_mutated, 0, 0, 1),
    ):
        if result.returncode != 0:
            failures.append(f"{label} arm exited {result.returncode}\n"
                            f"{result.output[-2000:]}")
        bad = BAD_RE.search(result.output)
        if bad:
            failures.append(f"{label} arm printed a fatal marker: {bad.group(0)!r}")
        if (result.play_requested != 1 or result.play_initial_valid != 1 or
                result.play_settled != 1 or result.play_service_frames is None or
                result.play_service_frames < 1):
            failures.append(
                f"{label} arm did not complete the asynchronous production Play "
                "recheck while rendering: "
                f"requested={result.play_requested} initialValid={result.play_initial_valid} "
                f"serviceFrames={result.play_service_frames} settled={result.play_settled}")
        if result.play_initial_path != str(
                final_play_rom if label == "final Play" else final_play_mutated_rom):
            failures.append(f"{label} arm selected the wrong ROM path: "
                            f"{result.play_initial_path!r}")
        if result.play_mutation_applied != mutation:
            failures.append(f"{label} arm mutation hook mismatch: "
                            f"got={result.play_mutation_applied} want={mutation}")
        if result.play_actions != expected_actions:
            failures.append(f"{label} arm emitted {result.play_actions} Play actions, "
                            f"want {expected_actions}")
        if result.play_final_valid != expected_final_valid:
            failures.append(f"{label} arm final validity={result.play_final_valid}, "
                            f"want {expected_final_valid}")
        if expected_actions == 1 and result.play_action_rom != str(final_play_rom):
            failures.append("final Play action did not carry the exact ROM that "
                            f"the launcher rechecked: {result.play_action_rom!r}")
        if expected_actions == 0 and result.play_action_rom != "(none)":
            failures.append("mutated final Play arm leaked a bootable ROM action: "
                            f"{result.play_action_rom!r}")

    # A replacement check cannot turn the persistent Play action into a dead
    # button. Play abandons the unresolved candidate and final-checks the
    # already-proven active ROM without publishing or retaining the candidate.
    if replacement_play.returncode != 0:
        failures.append(
            "replacement-Play arm exited "
            f"{replacement_play.returncode}\n{replacement_play.output[-2000:]}")
    if (replacement_play.replacement_play_candidate != str(replacement_play_rom) or
            replacement_play.replacement_play_initial_ready != 1 or
            replacement_play.replacement_play_pending != 1 or
            replacement_play.replacement_play_superseded != 1 or
            replacement_play.replacement_play_settled != 1 or
            replacement_play.replacement_play_service_frames is None or
            replacement_play.replacement_play_service_frames < 1 or
            replacement_play.replacement_play_actions != 1 or
            replacement_play.replacement_play_action_rom != str(rom) or
            replacement_play.replacement_play_active != str(rom) or
            replacement_play.replacement_play_candidate_visible != 0):
        failures.append(
            "Play did not supersede the unresolved replacement with one exact "
            "final-check action for the active ROM: "
            f"candidate={replacement_play.replacement_play_candidate!r} "
            f"ready={replacement_play.replacement_play_initial_ready} "
            f"pending={replacement_play.replacement_play_pending} "
            f"superseded={replacement_play.replacement_play_superseded} "
            f"frames={replacement_play.replacement_play_service_frames} "
            f"actions={replacement_play.replacement_play_actions} "
            f"actionRom={replacement_play.replacement_play_action_rom!r} "
            f"active={replacement_play.replacement_play_active!r} "
            f"candidateVisible={replacement_play.replacement_play_candidate_visible}")

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

    # --- transactional replacement: invalid candidate cannot destroy the
    # already accepted active ROM or its persisted preference ----------------
    if replacement.returncode != 0:
        failures.append(
            f"replacement arm exited {replacement.returncode}\n"
            f"{replacement.output[-2000:]}")
    if replacement.active != str(rom) or replacement.active_valid != 1:
        failures.append(
            "invalid replacement did not preserve the prior active ROM: "
            f"active={replacement.active!r} valid={replacement.active_valid}")
    if replacement.candidate_visible != 1 or replacement.cancel_available != 1:
        failures.append(
            "invalid replacement did not keep candidate error + Cancel visible: "
            f"candidate={replacement.candidate_visible} "
            f"cancel={replacement.cancel_available}")
    if f"rom_path={rom}" not in replacement_ini_text or str(garbage) in replacement_ini_text:
        failures.append(
            "invalid replacement changed the last-known-good ROM preference\n"
            f"{replacement_ini_text}")

    # Header-only validation used to accept both of these: the first has the
    # complete supported header but not the cartridge body; the second keeps the
    # exact supported header while changing a body byte. Both must be rejected by
    # the launcher before Play, while the last-known-good preference survives.
    for label, result, expected_text in (
        ("supported-header truncated", truncated_replacement, "exactly 12 MB"),
        ("supported-header body-corrupt", corrupted_replacement, "SHA-256"),
    ):
        if result.returncode != 0:
            failures.append(
                f"{label} arm exited {result.returncode}\n{result.output[-2000:]}")
        if result.valid != 0:
            failures.append(
                f"{label} arm was marked Ready despite full-image validation: "
                f"valid={result.valid} message={result.message!r}")
        if result.active != str(rom) or result.active_valid != 1:
            failures.append(
                f"{label} arm displaced the last-known-good ROM: "
                f"active={result.active!r} valid={result.active_valid}")
        if result.candidate_visible != 1 or result.cancel_available != 1:
            failures.append(
                f"{label} arm did not preserve an actionable candidate + Cancel state")
        if not result.message or expected_text not in result.message:
            failures.append(
                f"{label} arm lacks the expected actionable {expected_text!r} verdict: "
                f"{result.message!r}")

    if failures:
        print("check_shell_dropfile: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(
        "check_shell_dropfile: PASS — SDL_DROPFILE handler accepted "
        f"{rom.name} (valid=1, persisted) and refused {garbage.name} "
        "(valid=0, no crash, nothing persisted); invalid replacement preserved "
        "the active ROM with candidate error and Cancel; a supported header with "
        "either a truncated or corrupted body was refused before Play; a first "
        "verified ROM remained playable when its path could not be remembered; "
        "Play superseded an unresolved replacement and final-checked the active ROM; "
        "the final asynchronous Play recheck emitted one exact-path action and "
        "refused a post-selection body mutation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
