#!/usr/bin/env python3
"""Require repeated CPU/audio/GPU resource generations to reach a plateau.

The base Time Trial fixture enters Ancient Lake once. This check extends its
post-race A-button pulses so the original Try Again path loads the same race
again and again in one process: fourteen loads, and thirteen completed renderer
generations, in the default 40,000 frames, and the gate needs eleven loads.
The first generations may warm persistent caches; the last three equivalent
CPU/audio snapshots must be identical, while
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

By default the runs install a texture pack generated at startup: one short
headless run dumps the digests of the textures the route draws
(MDKR_MOD_TEXTURE_DUMP), and every one becomes a synthetic replacement in a
zipped digest pack. So the pack-texture cache is always judged, and every warm
race load must re-upload resident replacements at once (warm_reupload_error).
`--texture-pack` installs a given pack instead; `--no-pack` installs none.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

from check_mod_texture_override import quadrant_png
from harness_utils import DEFAULT_BUILD_DIR, present_mode_rows, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
BASE_SCRIPT = ROOT / "tests" / "input_scripts" / "race_full_3lap_tt.txt"
# Fourteen race samples on both renderers (a race every ~2,700 frames after the
# first), three more than the texture-cache window needs (CACHE_RACES_REQUIRED),
# so a content or pacing change that slows each race a little cannot by itself
# turn the gate red. The engine has no way to stop after N race loads, and
# stopping it from outside would lose the terminal censuses this gate reads,
# so the route is sized by frames, with that headroom.
DEFAULT_FRAMES = 40_000
# Per child run. Measured at 40,000 frames under the suite runner's background
# policy (`taskpolicy -b`) on a loaded M3 Max (load average 4-6), two rounds:
# GL 317-328 s, WebGPU 116-149 s. 900 s is 2.7x the worst. (240 s, the old
# ceiling, was already a contention red at 35,000 frames.) The dump run has
# its own ceiling, GENERATED_PACK_TIMEOUT; run_checks gives the task 2,400 s,
# room for the dump and both runs at their ceilings (the oracle test checks).
DEFAULT_RUN_TIMEOUT = 900.0
# The generated pack's dump run: through the first race's start (it loads near
# frame 2,000), which draws every texture a warm race load re-uploads.
GENERATED_PACK_FRAMES = 4_500
GENERATED_PACK_INI = "[pack]\nname=Plateau Fixture\npriority=100\n"
# The dump run's own ceiling, separate from --timeout so that the dump plus
# two backend runs, each at its ceiling, stay inside run_checks' task budget
# (test_resource_plateau_oracle checks the sum). Measured 39-41 s under the
# runner's background policy; 120 s is 2.9x that.
GENERATED_PACK_TIMEOUT = 120.0

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
CACHE_RE = re.compile(
    r"texture_cache_state: level=(-?\d+) players=(-?\d+) cutscene=(-?\d+) "
    r"pixels=(\d+) limit=(\d+) table=(\d+) entries=(\d+)"
    # Since issue #66 the census also says what the cache did to get there.
    # Optional, so a log from an older build still parses (and is held to the
    # strict comparison, having nothing finer to offer).
    r"(?: inflight=(\d+) pending=(\d+) firstResident=(\d+) "
    r"firstResidentBytes=(\d+) reloads=(\d+) evictions=(\d+))?"
)
# Row layout: level, players, cutscene, pixels, limit, table, entries, then,
# when the census carries them, inflight, pending, firstResident,
# firstResidentBytes, reloads, evictions.
CACHE_ACCOUNTED_FIELDS = 13
# New lookup entries the warmed window's one changed step may add by first use.
# Across the 30 retained real-pack runs of this route the largest change inside
# the window is 0 entries. The only late first use ever seen there is T12a's
# flake: one texture, one identity, which is two entries with a Rice pack (its
# digest and its Rice key). 8 allows four such identities in that one step.
# Slots are never released, so this bounds the table, not a tolerance on it.
CACHE_LATE_ENTRIES_MAX = 8
# Races that may still warm the cache. On this route first use grows from the
# first race to the second (~113 identities) and the second to the third (2),
# and is flat from the third on, so every race from the third is judged
# (T12d). A short run keeps at least its last three samples in the window.
CACHE_WARMUP_RACES = 2
# Race samples the gate requires. The window is every sample after warm-up,
# and at most one of its steps may change, so growth that recurs every P races
# shows two changed steps in every phase only when the window has 2P steps.
# Eleven samples leave nine in the window, eight steps: growth recurring every
# race, or every 2nd, 3rd or 4th race, fails whatever race it starts on.
CACHE_RACES_REQUIRED = 11


def cache_window(races: list[tuple[int, ...]]) -> list[tuple[int, ...]]:
    """The warmed races the cache is judged over: every race after warm-up.

    Not just the last three: growth that recurs every second or third race
    shows only one changed step inside any three samples, and passed (T12d).
    """
    return races[min(CACHE_WARMUP_RACES, max(0, len(races) - 3)):]


# The renderer's per-generation pack census (gfx_pc_dkr.c), labelled with the
# generation that just ended: level, players, cutscene, deferred, unbudgeted.
TEXPACK_GENERATION_RE = re.compile(
    r"texture_pack_generation: level=(-?\d+) players=(-?\d+) "
    r"cutscene=(-?\d+) .*?\bdeferred=(\d+) unbudgeted=(\d+)"
)


def cache_row(match: re.Match) -> tuple[int, ...]:
    return tuple(int(value) for value in match.groups() if value is not None)
FATAL_MARKERS = (
    "[CRASH]",
    "[FATAL]",
    "AddressSanitizer",
    "runtime error:",
    "allocation FAILED",
    "resource_state" + ": invalid",
)


def extended_route(frames: int = DEFAULT_FRAMES) -> str:
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
    # The native input loader admits 512 entries. Keep a generous navigation
    # allowance and spread at most 480 retry pulses over the complete soak.
    step = max(24, (max(0, frames - 7650) + 479) // 480)
    retained.extend(f"{frame} A 5" for frame in range(7650, frames, step))
    return "\n".join(retained) + "\n"


def cache_plateau_error(rows: list[tuple[int, ...]]) -> str | None:
    """The decoded pack-texture cache across the warmed races (cache_window).

    Identical samples are a plateau. Otherwise the cache may still have grown
    for exactly one reason: the race drew an identity for the FIRST time. The
    races are not identical scenes -- the post-race and ghost state changes the
    asset mix, and on WebGPU the set of textures drawn varies a little from run
    to run -- so a texture can be first drawn in a late race however long the
    route (the T12a flake: one 138,240-byte texture first drawn inside the final
    window). That is bounded by the game's own texture set and is not growth. Growth is the cache holding more than its
    first uses explain: re-decoding what it already had, evicting and
    reloading, or pixel/table accounting that moves on its own -- and first use
    that never stops. Store slots are never released, so an identity that is
    new in every race, or in every second or third race, is unbounded growth
    however exactly it is accounted. So, within the warmed window -- every race
    after the first CACHE_WARMUP_RACES -- every pixel byte the cache gains must
    be the bytes of an identity resident for the first time; nothing may be
    reloaded or evicted; the lookup table may grow only with new entries; at
    most ONE of the window's steps may change at all (first use that recurs,
    at any period, is growth); and that one step may add at most
    CACHE_LATE_ENTRIES_MAX entries. A census without those counters (an older
    build) gets the strict comparison over the same window.
    """
    races = [row for row in rows if row[:3] == (5, 0, 100)]
    if len(races) < 4:
        return "missing repeated texture-cache boundary samples"
    if any(row[3] > row[4] or row[4] == 0 for row in rows):
        return "decoded texture cache exceeded its reported budget"
    window = cache_window(races)
    accounted = all(len(row) >= CACHE_ACCOUNTED_FIELDS for row in window)
    # Churn first: a cache evicting and reloading at its cap can land on the
    # same totals twice in a row, and that is not a plateau.
    if accounted and (window[-1][11] != window[0][11] or
                      window[-1][12] != window[0][12]):
        return ("texture cache reloaded or evicted across warmed "
                f"generations (reloads {window[0][11]}->{window[-1][11]}, "
                f"evictions {window[0][12]}->{window[-1][12]}): {window}")
    if len(set(row[3:7] for row in window)) == 1:
        return None
    if not accounted:
        return f"texture cache did not plateau: {window}"
    for before, after in zip(window, window[1:]):
        pixels = after[3] - before[3]
        table = after[5] - before[5]
        entries = after[6] - before[6]
        first_bytes = after[10] - before[10]
        if pixels != first_bytes:
            return ("texture cache grew beyond first use: decoded pixels moved "
                    f"by {pixels} bytes but first-time residents account for "
                    f"{first_bytes}: {window}")
        if entries < 0 or table < 0 or (table > 0 and entries == 0):
            return ("texture-cache lookup table changed without new entries: "
                    f"table {before[5]}->{after[5]}, entries "
                    f"{before[6]}->{after[6]}: {window}")
    steps_changed = sum(1 for before, after in zip(window, window[1:])
                        if before[3:7] != after[3:7] or
                        before[9:11] != after[9:11])
    if steps_changed > 1:
        return (f"texture cache grew in {steps_changed} of the "
                f"{len(window) - 1} warmed steps: first use that recurs, race "
                f"after race or every few races, is growth, not warm-up: "
                f"{window}")
    new_entries = window[-1][6] - window[0][6]
    if new_entries > CACHE_LATE_ENTRIES_MAX:
        return (f"texture cache added {new_entries} lookup entries in one "
                f"warmed step, more than the {CACHE_LATE_ENTRIES_MAX} a late "
                f"first use accounts for: {window}")
    return None


def cache_gate_error(rows: list[tuple[int, ...]]) -> str | None:
    """cache_plateau_error() on a run long enough for it to mean what it says.

    With fewer than CACHE_RACES_REQUIRED race samples the window is too short
    to see two changed steps of growth recurring every 3rd or 4th race in some
    phases, so such a run is refused rather than passed (T12d).
    """
    races = [row for row in rows if row[:3] == (5, 0, 100)]
    if len(races) < CACHE_RACES_REQUIRED:
        return (f"only {len(races)} texture-cache race samples; the window "
                f"needs {CACHE_RACES_REQUIRED} to catch growth that recurs "
                "every 4th race")
    return cache_plateau_error(rows)


def pack_used(output: str, cache_races: list[tuple[int, ...]]) -> bool:
    """Evidence that the installed pack's textures were drawn: identities
    resident in the cache census (any pack), or the store's own line (a Rice
    pack prints it at exit)."""
    if (cache_races and len(cache_races[-1]) >= CACHE_ACCOUNTED_FIELDS and
            cache_races[-1][9] > 0):
        return True
    return re.search(r"\[MODS\] [1-9][0-9]* high-resolution textures? used "
                     r"this run", output) is not None


def scrubbed_environment(run_root: Path, renderer: str) -> dict[str, str]:
    """A child environment with no inherited game controls, and its save,
    preferences and video config under `run_root`."""
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update({
        "MDKR_AUDIO": "0",
        "MDKR_AUTOPILOT": "1",
        "MDKR_RESOURCE_STATS": "1",
        "MDKR_RENDERER": renderer,
        "MDKR_APP_PREFS_DIR": str(run_root),
        "MDKR_SAVE_DIR": str(run_root / "save"),
        # The run's own pack folder (absent on the no-pack arms).
        "MDKR_MODS_DIR": str(run_root / "mods"),
        # Isolate the video config with the save (see check_door_blocks.py).
        "MDKR_VIDEO_CONFIG_PATH": str(run_root / "save" / "video.ini"),
    })
    return env


def generate_pack(binary: Path, rom: Path, root: Path, timeout: float,
                  verbose: bool) -> Path:
    """A digest pack replacing every texture the route's first race draws.

    One headless GL run over the start of the same route with
    MDKR_MOD_TEXTURE_DUMP names the textures; each becomes textures/<digest>.png
    holding the same 16x16 two-colour PNG check_mod_texture_override.py
    builds, zipped as an installed pack would be. Nothing ROM-derived is kept:
    the dump is deleted as soon as its names are read. Raises RuntimeError.
    """
    corpus = root / "corpus"
    corpus.mkdir(parents=True)
    script = root / "dump_route.txt"
    script.write_text(extended_route(DEFAULT_FRAMES), encoding="utf-8")
    env = scrubbed_environment(root, "gl")
    env["MDKR_MOD_TEXTURE_DUMP"] = str(corpus)
    command = [str(binary), "--headless-frames", str(GENERATED_PACK_FRAMES),
               "--input-script", str(script), "--rom", str(rom)]
    if verbose:
        print("$ MDKR_MOD_TEXTURE_DUMP=... " + " ".join(command))
    started = time.monotonic()
    try:
        process = subprocess.run(command, cwd=root, env=env,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True,
                                 timeout=timeout, check=False)
        digests = sorted(entry.stem for entry in corpus.glob("*.png"))
    finally:
        shutil.rmtree(corpus, ignore_errors=True)
    if process.returncode != 0 or not digests:
        raise RuntimeError(
            f"the dump run for the generated pack exited {process.returncode} "
            f"with {len(digests)} texture(s):\n{process.stdout[-3000:]}")
    pack = root / "generated-pack.zip"
    image = quadrant_png(16)
    with zipfile.ZipFile(pack, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("pack.ini", GENERATED_PACK_INI)
        for digest in digests:
            archive.writestr(f"textures/{digest}.png", image)
    print(f"  generated pack: {len(digests)} replacement textures; dump run "
          f"{time.monotonic() - started:.0f}s of its {timeout:.0f}s")
    return pack


def cache_first_use_note(rows: list[tuple[int, ...]]) -> str:
    """How much of the warmed window's change was first use, for the log."""
    window = cache_window([row for row in rows if row[:3] == (5, 0, 100)])
    if len(window) < 3 or any(len(row) < CACHE_ACCOUNTED_FIELDS
                              for row in window):
        return ""
    identities = window[-1][9] - window[0][9]
    if identities == 0 and window[-1][6] == window[0][6]:
        return ""
    return (f"; late first use in the warmed window: {identities} "
            f"identit{'y' if identities == 1 else 'ies'} "
            f"({window[-1][10] - window[0][10]} bytes) and "
            f"{window[-1][6] - window[0][6]} new lookup entries")


def warm_reupload_error(rows: list[tuple[int, ...]]) -> str | None:
    """With a pack: each warm race load re-uploads its replacements at once.

    A level load makes the renderer forget its texture entries, so the
    replacements the store already holds go up again as the race draws them.
    For the first walks after the load they go up outside the per-frame upload
    budget -- the warm-load window in platform/fast3d/gfx_texpack_upload_
    budget.h, T12b's fix for warm loads that showed the original textures for
    several frames. Every race after the first is a warm load of a scene whose
    replacements are resident, so its generation row must count such uploads
    (`unbudgeted` > 0); none means the window no longer opens on a level load
    (T12d). The window's rules are unit-tested; this is the check that a level
    load still reaches them. Deferrals are not judged here: a texture first
    drawn after the window is budgeted, as it should be.
    """
    races = [row for row in rows if row[:3] == (5, 0, 100)]
    if len(races) < 3:
        return (f"only {len(races)} texture-pack race generations were "
                "reported; need three")
    cold = [index + 2 for index, row in enumerate(races[1:]) if row[4] == 0]
    if cold:
        return ("warm race load(s) " + ", ".join(map(str, cold)) +
                " re-uploaded no resident replacement outside the upload "
                "budget: the warm-load window did not open on the level load, "
                f"so the originals show until the budget allows: {races}")
    return None


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
            or music_alloc + music_free != 26
            or jingle_alloc + jingle_free != 16
            or sfx_alloc + sfx_free != 32
            or peak_physical > 40
            or peak_music > 26
            or valid != 1
        ):
            return f"audio voice/state ownership is incoherent: {row}"
    # Live audio ownership, per generation.
    #
    # This used to read the INSTANTANEOUS voicePhys/voiceMusic pair on one race
    # row. That worked only because the SGI alCSPStop() merely posted
    # AL_SEQP_STOPPING_EVT: the level teardown's music_stop() left the music
    # voices mapped, so the boundary sample still saw them. The clean-room
    # alCSPStop() releases synchronously, so voiceMusic is now 0/32 in EVERY
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
    texture_pack: Path | None = None,
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
        if texture_pack is not None:
            mods = run_root / "mods"
            mods.mkdir()
            shutil.copyfile(texture_pack, mods / "resource-fixture.zip")
        script = run_root / "resource_plateau.txt"
        script.write_text(extended_route(frames), encoding="utf-8")
        env = scrubbed_environment(run_root, renderer)
        command = [
            str(binary),
            "--headless-frames", str(frames),
            "--input-script", str(script),
            "--rom", str(rom),
        ]
        if verbose:
            print("$ " + " ".join(command))
        started = time.monotonic()
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
        elapsed = time.monotonic() - started
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
            gl_rows = [row for row in present_mode_rows(output)
                       if row.get("backend") == "gl"]
            if not gl_rows:
                return failure("no explicit GL present-mode result", output)
            present = gl_rows[-1]
            requested, effective, supported = (
                int(present.get(key, -1)) for key in
                ("requestedSwap", "effectiveSwap", "supported"))
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
        cache_rows = [cache_row(match) for match in CACHE_RE.finditer(output)]
        cache_races = [row for row in cache_rows if row[:3] == (5, 0, 100)]
        if texture_pack is not None and not pack_used(output, cache_races):
            return failure("requested pack supplied no rendered texture evidence", output)
        error = cache_gate_error(cache_rows)
        if error is not None:
            return failure(error, output)
        if texture_pack is not None:
            error = warm_reupload_error([
                tuple(map(int, match.groups()))
                for match in TEXPACK_GENERATION_RE.finditer(output)])
            if error is not None:
                return failure(error, output)
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
            "live/high; "
            f"packPixels={cache_races[-1][3]} table={cache_races[-1][5]} bytes"
            f"{cache_first_use_note(cache_rows)}; "
            f"{len(cache_races)} cache race samples (need "
            f"{CACHE_RACES_REQUIRED}); run {elapsed:.0f}s of its {timeout:.0f}s"
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
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--renderer", choices=("gl", "webgpu", "both"), default="both")
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    parser.add_argument("--timeout", type=float, default=DEFAULT_RUN_TIMEOUT,
                        help="seconds per child run (the dump, each backend)")
    packs = parser.add_mutually_exclusive_group()
    packs.add_argument("--texture-pack", type=Path,
                       help="copy a local ZIP into isolated mods instead of "
                            "the generated pack; require actual texture use")
    packs.add_argument("--no-pack", action="store_true",
                       help="run without any texture pack")
    parser.add_argument(
        "--artifacts-dir", type=Path,
        help="retain each backend route and complete child log here")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    if args.texture_pack is not None:
        args.texture_pack = args.texture_pack.expanduser().resolve()
        if not args.texture_pack.is_file() or args.texture_pack.suffix.lower() != ".zip":
            parser.error("--texture-pack must name an existing ZIP file")

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
    voices = (1, 39, 0, 1, 25, 0, 16, 0, 26, 1)
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
    # The texture-cache oracle: first use passes, growth beyond it does not.
    # Rows: level, players, cutscene, pixels, limit, table, entries, inflight,
    # pending, firstResident, firstResidentBytes, reloads, evictions.
    settled = (5, 0, 100, 1000, 4096, 320, 4, 0, 0, 4, 1000, 0, 0)
    late_first_use = (5, 0, 100, 1100, 4096, 320, 5, 0, 0, 5, 1100, 0, 0)
    if cache_plateau_error([settled] * 3 + [late_first_use]) is not None:
        print("check_resource_plateau: FAIL — a texture first drawn in a late "
              "race was reported as cache growth", file=sys.stderr)
        return 1
    if cache_plateau_error([settled] * 2 + [late_first_use] * 2) is not None:
        print("check_resource_plateau: FAIL — a texture first drawn in a late "
              "race and then settled was reported as cache growth",
              file=sys.stderr)
        return 1
    for label, grown in (
            ("pixel growth without a first use",
             (5, 0, 100, 1100, 4096, 320, 4, 0, 0, 4, 1000, 0, 0)),
            ("a reload in the warmed window",
             (5, 0, 100, 1100, 4096, 320, 4, 0, 0, 4, 1000, 1, 1)),
            ("a table that grew without entries",
             (5, 0, 100, 1000, 4096, 640, 4, 0, 0, 4, 1000, 0, 0)),
            ("a burst of new entries in one race",
             (5, 0, 100, 1000, 4096, 320, 54, 0, 0, 4, 1000, 0, 0))):
        if cache_plateau_error([settled] * 3 + [grown]) is None:
            print(f"check_resource_plateau: FAIL — cache positive control "
                  f"passed: {label}", file=sys.stderr)
            return 1
    # Steady growth, accounted exactly as first use every race, must fail.
    # So must growth that recurs every 2nd, 3rd or 4th race, starting on any
    # race after warm-up, over the races the gate requires; and a run with
    # fewer races is refused, because its window could miss some phases
    # (T12d).
    def periodic(period: int, phase: int,
                 races: int = CACHE_RACES_REQUIRED) -> list[tuple[int, ...]]:
        entries = [4] * races
        for race in range(CACHE_WARMUP_RACES + 1, races):
            step = race - CACHE_WARMUP_RACES - 1
            entries[race] = entries[race - 1] + (step % period == phase)
        return [(5, 0, 100, 1000 + 100 * (value - 4), 1 << 30, 320, value, 0,
                 0, value, 1000 + 100 * (value - 4), 0, 0)
                for value in entries]
    for period in (2, 3, 4):
        for phase in range(period):
            if cache_gate_error(periodic(period, phase)) is None:
                print(f"check_resource_plateau: FAIL — periodic-growth "
                      f"positive control passed: first use every {period} "
                      f"races, from step {phase}", file=sys.stderr)
                return 1
    if (cache_gate_error(periodic(1, 1)) is not None or
            cache_gate_error(periodic(9, 0)) is not None):
        print("check_resource_plateau: FAIL — a flat run, or one late first "
              "use, was reported as cache growth", file=sys.stderr)
        return 1
    if cache_gate_error(periodic(1, 1, CACHE_RACES_REQUIRED - 1)) is None:
        print("check_resource_plateau: FAIL — a run too short for the cache "
              "window was accepted", file=sys.stderr)
        return 1
    for label, rows in (
            ("new entries every race",
             [(5, 0, 100, 1000, 4096, 320, 4 + 5 * race, 0, 0, 4, 1000, 0, 0)
              for race in range(4)]),
            ("first-use pixels every race",
             [(5, 0, 100, 1000 + 100 * race, 4096, 320, 4 + race, 0, 0,
               4 + race, 1000 + 100 * race, 0, 0) for race in range(4)]),
            ("a table doubling with its entries",
             [(5, 0, 100, 1000, 4096, 320 * 2 ** race, 4 + 300 * race, 0, 0, 4,
               1000, 0, 0) for race in range(4)])):
        if cache_plateau_error(rows) is None:
            print(f"check_resource_plateau: FAIL — steady-growth positive "
                  f"control passed: {label}", file=sys.stderr)
            return 1
    # The warm-load window: a warm race load with no resident re-upload
    # outside the budget must fail; the shape of a real run must pass.
    warm = [(5, 0, 100, 0, 60)] + [(5, 0, 100, 0, 84)] * 6
    if warm_reupload_error(warm) is not None:
        print("check_resource_plateau: FAIL — warm re-uploads after each race "
              "load were reported as missing", file=sys.stderr)
        return 1
    if warm_reupload_error(warm[:3] + [(5, 0, 100, 9, 0)] + warm[4:]) is None:
        print("check_resource_plateau: FAIL — warm-load window positive "
              "control passed: a warm race load with no re-upload at once",
              file=sys.stderr)
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

    # The pack: given, none, or generated now from this build's own dump.
    pack = args.texture_pack
    pack_root: Path | None = None
    if pack is None and not args.no_pack:
        if args.artifacts_dir is not None:
            args.artifacts_dir.mkdir(parents=True, exist_ok=True)
        pack_root = Path(tempfile.mkdtemp(prefix="generated-pack-",
                                          dir=args.artifacts_dir))
        try:
            pack = generate_pack(binary, rom, pack_root,
                                 GENERATED_PACK_TIMEOUT, args.verbose)
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            print(f"check_resource_plateau: FAIL — could not generate the "
                  f"texture pack: {error}", file=sys.stderr)
            if args.artifacts_dir is None:
                shutil.rmtree(pack_root, ignore_errors=True)
            return 1

    renderers = ("gl", "webgpu") if args.renderer == "both" else (args.renderer,)
    try:
        failures = [
            error
            for renderer in renderers
            if (error := run_backend(
                binary, rom, renderer, args.frames, args.timeout,
                args.verbose, args.artifacts_dir, pack))
        ]
    finally:
        if pack_root is not None and args.artifacts_dir is None:
            shutil.rmtree(pack_root, ignore_errors=True)
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
