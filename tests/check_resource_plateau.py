#!/usr/bin/env python3
"""Require repeated CPU/audio/GPU resource generations to reach a plateau.

The base Time Trial fixture enters Ancient Lake once. This check extends its
post-race A-button pulses so the original Try Again path loads the same race four
times in one process. The route reaches four loads and therefore three
completed renderer generations. The first generations may warm persistent
caches; the last three equivalent CPU/audio snapshots must be identical, while
completed renderer generations must have coherent, bounded, non-growing
ownership. Renderer counts are intentionally not required to be byte-for-byte
equal because the production post-race/ghost state changes the asset mix.
The display-list pointer registry expires unobserved mappings by generation
while retaining already-queued globals for one boundary; its session high-water
must plateau with no ambiguous/full insert.

The game emits ``resource_state`` only after a level is fully initialized. The
snapshot walks the allocator's linked live-slot model rather than inspecting its
backing array, so stale/unlinked metadata cannot masquerade as ownership. The
renderer emits a generation only when the next level is fully initialized; this
includes teardown and prevents a mid-frame sample from hiding deferred release.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
BASE_SCRIPT = ROOT / "tests" / "input_scripts" / "race_full_3lap_tt.txt"
DEFAULT_FRAMES = 25_000

RESOURCE_RE = re.compile(
    r"resource_state: level=(-?\d+) players=(-?\d+) cutscene=(-?\d+) "
    r"mainLive=(\d+) mainUsed=(\d+) mainFree=(\d+) mainLargest=(\d+) "
    r"audioUsed=(\d+) audioCapacity=(\d+) audioAllocs=(\d+) "
    r"voicePhys=(\d+)/(\d+)/(\d+) voiceMusic=(\d+)/(\d+) "
    r"voiceJingle=(\d+)/(\d+) sfxStates=(\d+)/(\d+) "
    r"voicePeak=(\d+)/(\d+) voiceValid=(\d+)"
)
RENDERER_RE = re.compile(
    r"renderer_generation: level=(-?\d+) players=(-?\d+) cutscene=(-?\d+) "
    r"texStart=(\d+) texCreated=(\d+) texDeleted=(\d+) texLive=(\d+) "
    r"texPeak=(\d+) shaderCreates=(\d+)"
)
REGISTRY_RE = re.compile(
    r"registry_state: level=(-?\d+) players=(-?\d+) cutscene=(-?\d+) "
    r"live=(\d+) high=(\d+) ambiguous=(\d+) fullFails=(\d+) maxProbe=(\d+)"
)
GL_PRESENT_MODE_RE = re.compile(
    r"\[PRESENT-MODE\] backend=gl .*requestedSwap=(-?\d+) "
    r"effectiveSwap=(-?\d+) supported=(\d+)"
)
FATAL_MARKERS = (
    "[CRASH]",
    "[FATAL]",
    "AddressSanitizer",
    "runtime error:",
    "allocation FAILED",
    "resource_state" + ": invalid",
)


def extended_route() -> str:
    retained: list[str] = []
    for line in BASE_SCRIPT.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            retained.append(line)
            continue
        fields = stripped.split()
        if fields and fields[0].isdigit() and int(fields[0]) < 7650:
            retained.append(line)
    retained.append("# Repeated post-race advances select TRY AGAIN each cycle.")
    retained.extend(f"{frame} A 5" for frame in range(7650, 26_001, 24))
    return "\n".join(retained) + "\n"


def plateau_error(rows: list[tuple[int, ...]]) -> str | None:
    race_rows = [
        row for row in rows
        if row[0] == 5 and row[1] == 0 and row[2] == 100
    ]
    if len(race_rows) < 4:
        return (
            f"only {len(race_rows)} completed Ancient Lake race generations "
            "were observed; need four"
        )
    main_generations = [row[3:7] for row in race_rows[-3:]]
    if len(set(main_generations)) != 1:
        return (
            "main-pool ownership grew or changed across equivalent warmed "
            f"generations: {main_generations}"
        )
    audio_generations = [row[7:10] for row in rows]
    if not audio_generations or len(set(audio_generations)) != 1:
        return (
            "audio heap changed after initialization: "
            f"{audio_generations}"
        )
    for row in rows:
        physical_alloc, physical_free, physical_lame = row[10:13]
        music_alloc, music_free = row[13:15]
        jingle_alloc, jingle_free = row[15:17]
        sfx_alloc, sfx_free = row[17:19]
        peak_physical, peak_music = row[19:21]
        valid = row[21]
        if (
            physical_alloc + physical_free + physical_lame != 40
            or music_alloc + music_free != 24
            or jingle_alloc + jingle_free != 16
            or sfx_alloc + sfx_free != 32
            or peak_physical > 40
            or peak_music > 24
            or valid != 1
        ):
            return f"audio voice/state ownership is incoherent: {row}"
    # Live audio ownership, per generation.
    #
    # This used to read the INSTANTANEOUS voicePhys/voiceMusic pair on one race
    # row. That worked only because the SGI alCSPStop() merely posted
    # AL_SEQP_STOPPING_EVT: the level teardown's music_stop() left the music
    # voices mapped, so the boundary sample still saw them. The clean-room
    # alCSPStop() releases synchronously, so voiceMusic is now 0/24 in EVERY
    # boundary row -- natively and in the browser alike -- and the old form
    # became unsatisfiable rather than merely weaker.
    #
    # voicePeak is the high-water over the generation that just ended (sampled
    # once per audio pump, game/src/audio.c), which is what the assertion was
    # always trying to establish. Required on EVERY race generation rather than
    # on one of them, so a run that goes silent after the first race now fails.
    for row in race_rows:
        if row[19] == 0 or row[20] == 0:
            return (
                "a race generation ran without live physical/music voice "
                f"ownership (voicePeak={row[19]}/{row[20]}): {row}"
            )
    return None


def renderer_plateau_error(rows: list[tuple[int, ...]]) -> str | None:
    race_rows = [
        row for row in rows
        if row[0] == 5 and row[1] == 0 and row[2] == 100
    ]
    if len(race_rows) < 3:
        return (
            f"only {len(race_rows)} completed Ancient Lake renderer "
            "generations were observed; need three"
        )
    for row in rows:
        start, created, deleted, live, peak = row[3:8]
        if deleted > start + created or start + created - deleted != live:
            return f"incoherent texture ownership accounting: {row}"
        if peak < start or peak < live:
            return f"incoherent texture live high-water accounting: {row}"
    warmed = race_rows[-3:]
    live = [row[6] for row in warmed]
    net = [row[4] - row[5] for row in warmed]
    peaks = [row[7] for row in warmed]
    shader_creates = [row[8] for row in warmed]
    if any(change > 0 for change in net):
        return (
            "a completed warmed generation retained newly created texture "
            f"handles: net={net}, rows={warmed}"
        )
    if live != sorted(live, reverse=True):
        return f"live texture ownership grew across warmed generations: {live}"
    if any(value > 1024 for value in live + peaks):
        return (
            "texture ownership exceeded the frontend cache's hard bound: "
            f"live={live}, peaks={peaks}"
        )
    if shader_creates != sorted(shader_creates, reverse=True):
        return (
            "new shader-variant churn grew across warmed generations: "
            f"{shader_creates}"
        )
    return None


def registry_plateau_error(rows: list[tuple[int, ...]]) -> str | None:
    race_rows = [
        row for row in rows
        if row[0] == 5 and row[1] == 0 and row[2] == 100
    ]
    if len(race_rows) < 4:
        return (
            f"only {len(race_rows)} Ancient Lake registry generations "
            "were observed; need four"
        )
    for row in rows:
        live, high, ambiguous, full_fails, max_probe = row[3:8]
        if live > high or high > 4096 or max_probe > 4:
            return f"incoherent pointer-registry accounting: {row}"
        if ambiguous != 0 or full_fails != 0:
            return f"pointer-registry insertion failed or was ambiguous: {row}"
    warmed = race_rows[-3:]
    high = [row[4] for row in warmed]
    if len(set(high)) != 1:
        return f"pointer-registry high-water grew without a bound: {high}"
    return None


def diagnostic_tail(output: str) -> str:
    markers = (
        "[PRESENT-MODE]", "[RESOURCE-HEARTBEAT]", "resource_state:",
        "renderer_generation:", "registry_state:", "[GL-BACKPRESSURE]",
        "[WGPU-LIMITS]", "[PTRREG]", "[FATAL]", "[CRASH]",
    )
    evidence = [
        line for line in output.splitlines()
        if any(marker in line for marker in markers)
    ]
    if not evidence:
        evidence = output.splitlines()
    return "\n".join(evidence[-30:])


def run_backend(
    binary: Path, rom: Path, renderer: str, frames: int,
    timeout: float, verbose: bool, artifacts_dir: Path | None,
) -> str | None:
    cleanup_on_success = artifacts_dir is None
    if artifacts_dir is None:
        run_root = Path(tempfile.mkdtemp(prefix=f"mdkr_plateau_{renderer}_"))
    else:
        artifacts_dir.mkdir(parents=True, exist_ok=True)
        # Every retained run owns a fresh save/config root. Reusing
        # <artifacts>/<renderer>/save would let a prior EEPROM change the next
        # route and make diagnostic evidence depend on invocation history.
        run_root = Path(tempfile.mkdtemp(
            prefix=f"{renderer}-", dir=artifacts_dir))
    log_path = run_root / "run.log"

    def failure(message: str, output: str = "") -> str:
        tail = diagnostic_tail(output)
        detail = f"{renderer}: {message}; artifacts retained at {run_root}"
        return detail if not tail else f"{detail}\n{tail}"

    try:
        script = run_root / "resource_plateau.txt"
        script.write_text(extended_route(), encoding="utf-8")
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith("MDKR_")
        }
        env.update({
            "MDKR_AUDIO": "0",
            "MDKR_AUTOPILOT": "1",
            "MDKR_RESOURCE_STATS": "1",
            "MDKR_RENDERER": renderer,
            "MDKR_SAVE_DIR": str(run_root / "save"),
        })
        command = [
            str(binary),
            "--headless-frames", str(frames),
            "--input-script", str(script),
            "--rom", str(rom),
        ]
        if verbose:
            print("$ " + " ".join(command))
        try:
            with log_path.open("w", encoding="utf-8") as log:
                process = subprocess.run(
                    command,
                    cwd=run_root,
                    env=env,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    text=True,
                    timeout=timeout,
                    check=False,
                )
        except subprocess.TimeoutExpired:
            output = log_path.read_text(encoding="utf-8", errors="replace")
            return failure(f"timed out after {timeout:.0f}s", output)
        output = log_path.read_text(encoding="utf-8", errors="replace")
        if process.returncode != 0:
            return failure(f"exit code {process.returncode}", output)
        for marker in FATAL_MARKERS:
            if marker in output:
                line = next(
                    (item for item in output.splitlines() if marker in item),
                    marker,
                )
                return failure(f"fatal diagnostic: {line}", output)
        expected_backend = (
            "renderer backend: gl"
            if renderer == "gl"
            else "renderer backend: webgpu"
        )
        if expected_backend not in output:
            return failure("requested backend was not active", output)
        if renderer == "gl":
            present = GL_PRESENT_MODE_RE.search(output)
            if present is None:
                return failure("no explicit GL present-mode result", output)
            requested, effective, supported = map(int, present.groups())
            if (requested, effective, supported) != (0, 0, 1):
                return failure(
                    "headless GL did not obtain the required unlocked swap "
                    f"contract (requested={requested}, effective={effective}, "
                    f"supported={supported})",
                    output,
                )
        if renderer == "webgpu" and "[WGPU-LIMITS]" not in output:
            return failure("no terminal GPU resource census was emitted", output)
        if "[PTRREG]" not in output:
            return failure("no terminal pointer-registry census was emitted", output)
        rows = [tuple(map(int, match.groups())) for match in RESOURCE_RE.finditer(output)]
        error = plateau_error(rows)
        if error is not None:
            return failure(error + "\n" + "\n".join(
                line for line in output.splitlines()
                if "resource_state:" in line or "renderer_generation:" in line
            ), output)
        renderer_rows = [
            tuple(map(int, match.groups()))
            for match in RENDERER_RE.finditer(output)
        ]
        error = renderer_plateau_error(renderer_rows)
        if error is not None:
            return failure(error + "\n" + "\n".join(
                line for line in output.splitlines()
                if "renderer_generation:" in line
            ), output)
        registry_rows = [
            tuple(map(int, match.groups()))
            for match in REGISTRY_RE.finditer(output)
        ]
        error = registry_plateau_error(registry_rows)
        if error is not None:
            return failure(error + "\n" + "\n".join(
                line for line in output.splitlines()
                if "registry_state:" in line
            ), output)
        race_rows = [
            row for row in rows
            if row[0] == 5 and row[1] == 0 and row[2] == 100
        ]
        renderer_race_rows = [
            row for row in renderer_rows
            if row[0] == 5 and row[1] == 0 and row[2] == 100
        ]
        warmed = race_rows[-1]
        renderer_warmed = renderer_race_rows[-1]
        registry_race_rows = [
            row for row in registry_rows
            if row[0] == 5 and row[1] == 0 and row[2] == 100
        ]
        registry_warmed = registry_race_rows[-1]
        print(
            f"  {renderer}: {len(race_rows)} race loads/"
            f"{len(renderer_race_rows)} completed renderer generations; "
            f"main={warmed[3]} slots/{warmed[4]} bytes; "
            f"audio={warmed[7]}/{warmed[8]} bytes; "
            f"voices={warmed[10]}/{warmed[11]}/{warmed[12]} "
            "allocated/free/lame; "
            f"states={warmed[17]}/{warmed[18]} allocated/free; "
            f"textures={renderer_warmed[3]} start+"
            f"{renderer_warmed[4]}-"
            f"{renderer_warmed[5]}={renderer_warmed[6]} live; "
            f"shaders={renderer_warmed[8]}; "
            f"registry={registry_warmed[3]}/{registry_warmed[4]} "
            "live/high"
        )
        if cleanup_on_success:
            shutil.rmtree(run_root)
        return None
    except (OSError, ValueError) as error:
        output = ""
        if log_path.exists():
            output = log_path.read_text(encoding="utf-8", errors="replace")
        return failure(f"evidence handling failed: {error}", output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--renderer", choices=("gl", "webgpu", "both"), default="both")
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    parser.add_argument("--timeout", type=float, default=240.0)
    parser.add_argument(
        "--artifacts-dir", type=Path,
        help="retain each backend route and complete child log here")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    if not binary.is_file() or not rom.is_file() or not BASE_SCRIPT.is_file():
        print("check_resource_plateau: FAIL — required artifact is missing",
              file=sys.stderr)
        return 1
    if args.frames < DEFAULT_FRAMES:
        print(
            f"check_resource_plateau: FAIL — need at least {DEFAULT_FRAMES} frames",
            file=sys.stderr,
        )
        return 1

    # Parser/checker positive control: generation-over-generation growth must
    # fail even if every value remains individually plausible.
    voices = (1, 39, 0, 1, 23, 0, 16, 0, 32, 1)
    synthetic = [
        (5, 0, 100, 10, 1000, 2000, 1900, 500, 1000, 20, *voices),
        (5, 0, 100, 10, 1000, 2000, 1900, 500, 1000, 20, *voices),
        (5, 0, 100, 11, 1016, 1984, 1884, 500, 1000, 20, *voices),
        (5, 0, 100, 12, 1032, 1968, 1868, 500, 1000, 20, *voices),
    ]
    if plateau_error(synthetic) is None:
        print("check_resource_plateau: FAIL — growth positive control passed",
              file=sys.stderr)
        return 1
    invalid_voices = list(synthetic)
    invalid_voices[-1] = (*invalid_voices[-1][:-1], 0)
    if plateau_error(invalid_voices) is None:
        print(
            "check_resource_plateau: FAIL — voice-ownership positive "
            "control passed",
            file=sys.stderr,
        )
        return 1
    renderer_synthetic = [
        (5, 0, 100, 10, 4, 4, 10, 14, 0),
        (5, 0, 100, 10, 4, 4, 10, 14, 0),
        (5, 0, 100, 10, 5, 4, 11, 15, 0),
    ]
    if renderer_plateau_error(renderer_synthetic) is None:
        print(
            "check_resource_plateau: FAIL — renderer growth positive "
            "control passed",
            file=sys.stderr,
        )
        return 1
    registry_synthetic = [
        (5, 0, 100, 10, 20, 0, 0, 3),
        (5, 0, 100, 11, 21, 0, 0, 3),
        (5, 0, 100, 12, 22, 0, 0, 3),
        (5, 0, 100, 13, 23, 0, 0, 3),
    ]
    if registry_plateau_error(registry_synthetic) is None:
        print(
            "check_resource_plateau: FAIL — registry growth positive "
            "control passed",
            file=sys.stderr,
        )
        return 1

    renderers = ("gl", "webgpu") if args.renderer == "both" else (args.renderer,)
    failures = [
        error
        for renderer in renderers
        if (error := run_backend(
            binary, rom, renderer, args.frames, args.timeout, args.verbose,
            args.artifacts_dir))
    ]
    if failures:
        print("check_resource_plateau: FAIL", file=sys.stderr)
        for failure in failures:
            print("  " + failure.replace("\n", "\n  "), file=sys.stderr)
        return 1
    print(
        "check_resource_plateau: PASS — repeated stage, main-pool, audio-heap, "
        "renderer, and pointer-registry generations plateau"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
