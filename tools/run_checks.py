#!/usr/bin/env python3
"""Run every registered mdkr64 regression check with the correct artifact.

The suite has five invocation shapes:

* ROM-free CTest targets use the selected native build directory;
* ordinary behavioural checks use the selected native build;
* optimisation- and sanitizer-specific checks use dedicated Release/ASan builds;
* structural checks inspect an instrumented build or the linked wasm directly;
* browser gates serve wasm modules to an isolated real Chromium profile.

This runner owns those differences and fails if a new ``tests/check_*.py`` is
not registered below. It runs sequentially because several checks intentionally
create and remove ``save/eeprom.bin``. The default is the complete suite; use
``--only`` only for iteration.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"


@dataclass(frozen=True)
class Check:
    name: str
    script: str
    role: str
    description: str
    args: tuple[str, ...] = ()


# Cheap, broad gates lead; long scenario/matrix checks follow. Keep every
# tests/check_*.py represented at least once: validate_manifest() enforces it.
CHECKS = (
    Check("rom_free_units", "", "ctest",
          "ROM-free display, endian, magic-code, object-layout, allocator, "
          "and subsystem contract unit tests"),
    Check("asset_swap_invariants", "check_asset_swap_invariants.py", "rom",
          "field-level byte-swap audit: spec-derived ROM invariants with "
          "byte-reversed positive controls, plus raw asset_load() swap coverage"),
    Check("math_tables", "check_math_tables.py", "native",
          "ROM math-table fidelity and strong required-provider symbols"),
    Check("math_rotpy", "check_math_rotpy.py", "native", "hand-assembly rotation parity"),
    Check("shell_dropfile", "check_shell_dropfile.py", "native",
          "app-shell SDL_DROPFILE ROM acquisition: accepted and refused, "
          "isolated from shared prefs"),
    Check("app_adopted_pacing", "check_app_adopted_pacing.py", "native",
          "app-shell WebGPU-default and GL adopted handoffs at numeric and "
          "uncapped rates"),
    Check("app_capture", "check_app_capture.py", "native",
          "launcher screenshot dimensions, contrast, palette, draw bounds, "
          "and broken-direction mutations"),
    Check("app_ui_input", "check_app_ui_input.py", "native",
          "real ImGui keyboard/gamepad selection, reload, scale matrix, "
          "save failure, and retry"),
    Check("collision_untextured", "check_collision_untextured.py", "native",
          "untextured terrain collision"),
    Check("runtime_safety", "check_runtime_safety.py", "native",
          "teardown, racer, and audio boundary contracts"),
    Check("simulation_cadence", "check_simulation_cadence.py", "native",
          "NTSC/PAL source clocks, original/enhanced pacing mechanism, and "
          "explicit oracle policy"),
    Check("sprite_layout", "check_sprite_layout.py", "native",
          "independent ROM sprite census and display-list allocation controls"),
    Check("rdp_interpolation", "check_rdp_interpolation.py", "native",
          "screen-linear shade/fog and clipped-fog A/B on GL and WebGPU"),
    Check("font_sdf", "check_font_sdf.py", "native",
          "runtime-derived text mode isolation, lifecycle, bleed, GL/WebGPU"),
    Check("native_ui_resolution", "check_native_ui_resolution.py", "native",
          "output-resolution HUD/text, scene isolation, ordering, GL/WebGPU"),
    Check("mip_motion", "check_mip_motion.py", "native",
          "moving Everfrost track-surface temporal stability on GL/WebGPU"),
    Check("rl1_vertex_colour_ab", "check_rl1_vertex_colour_ab.py", "native",
          "bounded Ancient Lake/Fire Mountain three-arm lighting decision"),
    Check("remaster_lighting", "check_remaster_lighting.py", "native",
          "runtime-derived sun, smooth-normal lighting, colour boundary, and mode isolation"),
    Check("world_fx_capture", "check_world_fx_capture.py", "native",
          "capture-once world caster ownership, material classes, and state/frame invariance"),
    Check("world_fx_matrix", "check_world_fx_matrix.py", "native",
          "representative world and 1P-4P caster/view ownership baseline"),
    Check("world_shadows", "check_world_shadows.py", "native",
          "GL/WebGPU cascaded maps, receivers, state invariance, and truthful decal fallback"),
    Check("render_purity", "check_render_purity.py", "release",
          "skip-render authoritative invariance (spec 12.2.1) with divergence control"),
    Check("camera_snapshot_coverage", "check_camera_snapshot_coverage.py", "release",
          "real 2P camera 1 and 3P time-trial camera 3 snapshot interpolation, "
          "WebGPU cutscene-bank camera 4, pixels, and frozen controls"),
    Check("hud_render_authority", "check_hud_render_authority.py", "release",
          "1P/2P/4P countdown, wrong-way RNG/timer, audio, and event "
          "invariance under skipped presentation"),
    Check("fixed_tick_schedules", "check_fixed_tick_schedules.py", "release",
          "fixed two-field authority across lateness, catch-up, suspension, "
          "state/event invariance, and sensitivity controls"),
    Check("arbitrary_presentation_rates", "check_arbitrary_presentation_rates.py",
          "release", "exact original/30/60/120/144/165/240/uncapped native "
          "presentation with NTSC/PAL state, event, input, and PCM invariance"),
    Check("presentation_matrix", "check_presentation_matrix.py", "release",
          "presentation rate vs fixed-ticket state/event authority and pixels "
          "(spec 12.2.2)"),
    Check("presentation_breadth", "check_presentation_breadth.py", "release",
          "presentation-rate invariance across spec 12.3 content breadth: "
          "bosses, all challenge types, car/hovercraft/plane, 1P-4P, NTSC/PAL"),
    Check("presentation_lifecycle", "check_presentation_lifecycle.py", "release",
          "presentation-rate state/event/input/PCM invariance across pause quit, "
          "race restart, post-race results, and arena teardown/reissue"),
    Check("presentation_lifecycle_asan", "check_presentation_lifecycle.py", "asan",
          "ASan witness for retained replay teardown on 2P pause-to-menu quit",
          ("--only", "pause-quit")),
    Check("state_hash", "check_state_hash.py", "release",
          "authoritative-hash determinism, window/backend invariance, legacy-RNG control"),
    Check("weather_rng_order", "check_weather_rng_order.py", "release",
          "weather-enabled authored object/weather/HUD RNG order and presentation invariance"),
    Check("viewport_route_isolation", "check_viewport_route_isolation.py", "release",
          "2P/4P per-viewport object pass, opacity, shadow/water, and pixel isolation"),
    Check("authored_rng_compat", "check_authored_rng_compat.py", "native",
          "exact legacy all-racer state and authored-RNG stream compatibility"),
    Check("shadow_stage_reset", "check_shadow_stage_reset.py", "native",
          "shipping-build shadow stage reset at level load, with a suppressed-reset control"),
    Check("shadow_plausibility", "check_shadow_plausibility.py", "native",
          "caster provenance and shadow attribution across 3 worlds and every "
          "1P-4P budget tier, with an injected bogus-caster control"),
    Check("charselect_motion", "check_charselect_motion.py", "native",
          "character-select dancer motion/rate ensemble, with frozen-frame "
          "and fast-loop broken-direction controls"),
    Check("attract_demo", "check_attract_demo.py", "native",
          "rolling-demo vehicle/path selection, soak, and input teardown"),
    Check("nav_fixtures", "check_nav_fixtures.py", "native", "all menu routes"),
    Check("video_options", "check_video_options.py", "native",
          "in-game presentation/accessibility controls, persistence, locks, and faults"),
    Check("determinism", "check_determinism.py", "native",
          "byte-reproducible frame output"),
    Check("renderer_backends", "check_renderer_backends.py", "native",
          "GL/WebGPU coarse route parity, fail-closed startup, and dense "
          "default-WebGPU intro identity"),
    Check("gpu_backpressure", "check_gpu_backpressure.py", "native",
          "live uncapped GL fences and bounded WebGPU queue completions"),
    Check("surface_suspension", "check_surface_suspension.py", "native",
          "minimized GL/WebGPU render elision and resume rebase"),
    Check("final_shutdown", "check_final_shutdown.py", "native",
          "cooperative GL/WebGPU/audio/platform final teardown"),
    Check("webgpu_recovery", "check_webgpu_recovery.py", "native",
          "WebGPU lifecycle fault injection and fail-closed recovery policy"),
    Check("webgpu_fault_matrix", "check_webgpu_fault_matrix.py", "source",
          "every WebGPU fault point wired and product-route classified"),
    Check("ci_contract", "check_ci_contract.py", "source",
          "push/PR native, sanitizer, wasm, save-custody, and ROM policy"),
    Check("address_domains", "check_address_domains.py", "source",
          "raw pointer/token narrowing confined to typed boundary helpers"),
    Check("rom_model_corpus", "check_rom_model_corpus.py", "rom",
          "all object/level model assets, batches, sentinels, and render states"),
    Check("webgpu_content_census", "check_webgpu_content_census.py", "native",
          "all menus, races, bosses, and challenges under strict display-list "
          "and WebGPU material/capacity census"),
    Check("widescreen_shadow", "check_widescreen_shadow.py", "native",
          "aspect-invariant simulation and transactional shadows"),
    Check("video_presets", "check_video_presets.py", "native",
          "presentation modes: cross-mode simulation invariance, Pure 4:3 framing, "
          "precedence ladder"),
    Check("widescreen_proportions", "check_widescreen_proportions.py", "native",
          "pixel-level HUD/world billboard proportions across aspect and FOV"),
    Check("shadow_visual_ab", "check_shadow_visual_ab.py", "native",
          "moving-camera projected-shadow pixel A/B"),
    Check("audio_output", "check_audio_output.py", "native",
          "audio content, timing, and reverb"),
    Check("audio_level_reference", "check_audio_level_reference.py", "native",
          "absolute output level: RMS/crest/true-peak/per-band/per-slice against "
          "the frozen baseline, with injected-gain controls"),
    Check("resource_plateau", "check_resource_plateau.py", "native",
          "repeated GL/WebGPU stage, pool/audio/GPU/registry ownership plateau"),
    Check("raw16_audio", "check_raw16_audio.py", "native",
          "RAW16 bank census, endian oracle, and fixed/legacy PCM A/B"),
    Check("texture_lineswap", "check_texture_lineswap.py", "native",
          "RDP odd-row texture decode"),
    Check("race_drive", "check_race_drive.py", "native",
          "closed-loop racing and rendered scene"),
    Check("race_2p_split", "check_race_2p_split.py", "native",
          "two-player split-screen race at the shipping original cadence",
          ("--cadence", "original")),
    Check("2p_human_binding", "check_2p_human_binding.py", "native",
          "direct two-player controller/racer binding and motion across "
          "GL/WebGPU at 60/120 Hz"),
    Check("race_2p_split_enhanced", "check_race_2p_split.py", "native",
          "two-player split-screen race at the opt-in enhanced cadence",
          ("--cadence", "enhanced")),
    Check("race_multiplayer", "check_race_multiplayer.py", "native",
          "three-/four-player racers, quadrants, minimap, and results flow"),
    Check("challenge_modes", "check_challenge_modes.py", "native",
          "all eggs, treasure, and battle courses: win/loss, results, progression, "
          "save, and terminal-gate controls"),
    Check("taj_challenges", "check_taj_challenges.py", "native",
          "car, hovercraft, and plane Taj challenges: first win, loss, abort, "
          "replay, completion controls, and save reload"),
    Check("adventure_hub", "check_adventure_hub.py", "native",
          "Adventure hub traversal"),
    Check("adventure_two", "check_adventure_two.py", "native",
          "Adventure Two unlock/save identity and all twenty mirrored tracks"),
    Check("door_blocks", "check_door_blocks.py", "native",
          "locked-door object collision and legacy positive control"),
    Check("door_glyphs", "check_door_glyphs.py", "native",
          "per-door balloon numeral binding across shared models and GL/WebGPU"),
    Check("adventure_race_loop", "check_adventure_race_loop.py", "native",
          "Adventure hub/race return loop"),
    Check("trophy_series", "check_trophy_series.py", "native",
          "all four Adventure trophy championships, quit/retry, and EEPROM reload"),
    Check("race_finish_time", "check_race_finish_time.py", "native",
          "three-lap finish and EEPROM time"),
    Check("boost_magnitude", "check_boost_magnitude.py", "native",
          "zip-pad boost per-frame speed trace, racer-count independence, and "
          "perturbed-boost-constant positive controls"),
    Check("save_failsafe", "check_save_failsafe.py", "native",
          "EEPROM recovery and persistence"),
    Check("boss_win_verdict", "check_boss_win_verdict.py", "native",
          "boss win/lose state contract"),
    Check("bluey2_rematch", "check_bluey2_rematch.py", "native",
          "progression-valid Bluey rematch parity, Enhanced fail-red control, "
          "and authored cue-transition PCM"),
    Check("first_boss_progression", "check_first_boss_progression.py", "native",
          "legal fourth Dino race through Tricky 1 save/reload"),
    Check("collision_gridmask", "check_collision_gridmask.py", "native",
          "collision candidate filter and boss flow"),
    Check("collision_headroom", "check_collision_headroom.py", "native",
          "per-level collision-candidate high-water sweep, guard-present check, "
          "and forced-saturation positive control"),
    Check("rom_revision", "check_rom_revision.py", "native",
          "ROM identity, byte order, and revision parity"),
    Check("track_sweep", "check_track_sweep.py", "native",
          "all twenty race tracks"),
    Check("vehicle_sweep", "check_vehicle_sweep.py", "native",
          "all forty-seven legal track/vehicle pairs"),
    Check("key_cutscene_once", "check_key_cutscene_once.py", "release",
          "optimisation-sensitive one-shot cutscene latch"),
    Check("door_blocks_release", "check_door_blocks.py", "release",
          "optimized locked-door object collision and legacy positive control"),
    Check("raw16_audio_release", "check_raw16_audio.py", "release",
          "optimized RAW16 endian oracle and fixed/legacy PCM A/B"),
    Check("door_blocks_asan", "check_door_blocks.py", "asan",
          "locked-door object collision under AddressSanitizer"),
    Check("raw16_audio_asan", "check_raw16_audio.py", "asan",
          "RAW16 endian boundary under AddressSanitizer"),
    Check("filename_entry", "check_filename_entry.py", "native",
          "new-save filename UI in selected build"),
    Check("filename_entry_asan", "check_filename_entry.py", "asan",
          "new-save filename UI under AddressSanitizer"),
    Check("widescreen_shadow_asan", "check_widescreen_shadow.py", "asan",
          "forced shadow overflow under AddressSanitizer"),
    Check("array_bounds_sweep", "check_array_bounds_sweep.py", "instrumented",
          "UBSan array/pointer/shift class sweep"),
    Check("full_ubsan", "check_full_ubsan.py", "instrumented",
          "optimized full-undefined broad content and stateful route gate"),
    Check("native_layout", "check_native_layout.py", "layout",
          "alignment-safe object tails, records, and renderer field APIs"),
    Check("wave_visible_table", "check_wave_visible_table.py", "wasm",
          "linked wasm wave-table contiguity"),
    Check("browser_save_ui", "check_browser_save_ui.py", "browser_save",
          "ROM/WebGPU-free save custody, hostile inputs, faults, and accessibility"),
    Check("browser_resource_plateau", "check_browser_resource_plateau.py", "browser",
          "repeated wasm stage/audio/WebGPU/host ownership conservation"),
    Check("touch_controls", "check_touch_controls.py", "browser",
          "bounded touch edge transport, overlay gating/persistence, chord and neutral"),
    Check("browser_presentation_rates", "check_browser_presentation_rates.py",
          "browser", "display/capped/irregular rAF scheduling, fixed authority, "
          "and explicit uncapped-to-display semantics"),
    Check("browser_runtime", "check_browser_runtime.py", "browser",
          "real Chromium WebGPU, pacing, rendering, IDBFS, and privacy"),
)


def validate_manifest() -> None:
    discovered = {path.name for path in TESTS.glob("check_*.py")}
    registered = {check.script for check in CHECKS if check.script}
    missing = sorted(discovered - registered)
    stale = sorted(registered - discovered)
    duplicate_names = sorted(
        name for name in {check.name for check in CHECKS}
        if sum(check.name == name for check in CHECKS) != 1
    )
    problems: list[str] = []
    if missing:
        problems.append("unregistered check scripts: " + ", ".join(missing))
    if stale:
        problems.append("manifest entries point at missing scripts: " + ", ".join(stale))
    if duplicate_names:
        problems.append("duplicate task names: " + ", ".join(duplicate_names))
    if problems:
        raise RuntimeError("; ".join(problems))


def resolve_binary(value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    if path.is_dir():
        path /= "mdkr64"
    return path.resolve()


def resolve_path(value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    return path.resolve()


def cmake_build_type(binary: Path) -> str | None:
    cache = binary.parent / "CMakeCache.txt"
    if not cache.is_file():
        return None
    prefix = "CMAKE_BUILD_TYPE:STRING="
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix):
            return line[len(prefix):].strip()
    return None


def has_asan(binary: Path) -> bool:
    try:
        proc = subprocess.run(
            ["nm", "-u", str(binary)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
            text=True,
        )
    except OSError:
        return False
    return "asan_init" in proc.stdout


def selected_checks(pattern_values: list[str] | None) -> list[Check]:
    if not pattern_values:
        return list(CHECKS)
    patterns = [
        pattern.strip()
        for value in pattern_values
        for pattern in value.split(",")
        if pattern.strip()
    ]
    selected: list[Check] = []
    matched: set[str] = set()
    for check in CHECKS:
        aliases = (check.name, check.script, Path(check.script).stem)
        for pattern in patterns:
            if any(fnmatch.fnmatchcase(alias, pattern) for alias in aliases):
                selected.append(check)
                matched.add(pattern)
                break
    unmatched = [pattern for pattern in patterns if pattern not in matched]
    if unmatched:
        raise RuntimeError("no checks matched: " + ", ".join(unmatched))
    return selected


def command_for(
    check: Check,
    native: Path,
    release: Path,
    asan: Path,
    rom: Path,
    roms: Path,
    wasm: Path,
    no_rebuild_instrumented: bool,
) -> list[str]:
    if check.role == "ctest":
        return [
            "ctest",
            "--test-dir",
            str(native.parent),
            "--output-on-failure",
            "-LE",
            "gpu",
        ]
    cmd = [sys.executable, str(TESTS / check.script)]
    if check.role == "source":
        pass
    elif check.role == "rom":
        cmd += ["--rom", str(rom)]
    elif check.role == "native":
        cmd += ["--build", str(native), "--rom", str(rom)]
    elif check.role == "release":
        cmd += ["--build", str(release), "--rom", str(rom)]
    elif check.role == "asan":
        cmd += ["--build", str(asan), "--rom", str(rom)]
    elif check.role == "instrumented":
        cmd += ["--rom", str(rom)]
        if no_rebuild_instrumented:
            cmd.append("--no-build")
    elif check.role == "layout":
        cmd += [
            "--rom",
            str(rom),
            "--asan-build",
            str(asan.parent),
        ]
        if no_rebuild_instrumented:
            cmd.append("--no-build")
    elif check.role == "wasm":
        cmd += ["--wasm", str(wasm)]
    elif check.role == "browser":
        cmd += [
            "--engine-dir",
            str(wasm.parent),
            "--shell-dir",
            str(ROOT / "dist" / "web"),
            "--rom",
            str(rom),
        ]
    elif check.role == "browser_save":
        cmd += [
            "--engine-dir",
            str(wasm.parent),
            "--shell-dir",
            str(ROOT / "dist" / "web"),
            "--cli",
            str(native.parent / "mdkr-save"),
        ]
    else:
        raise AssertionError(f"unknown check role: {check.role}")
    # presentation_breadth needs the ROM directory for the same reason
    # simulation_cadence does: spec 12.3's region clause is NTSC *and* PAL, and
    # the PAL release is not the default --rom.
    if check.name in {"rom_revision", "simulation_cadence",
                      "arbitrary_presentation_rates", "presentation_breadth"}:
        cmd += ["--roms", str(roms)]
    cmd += list(check.args)
    return cmd


def format_duration(seconds: float) -> str:
    minutes, remainder = divmod(int(round(seconds)), 60)
    if minutes:
        return f"{minutes}m{remainder:02d}s"
    return f"{remainder}s"


def preflight(
    checks: list[Check],
    native: Path,
    release: Path,
    asan: Path,
    rom: Path,
    wasm: Path,
) -> None:
    roles = {check.role for check in checks}
    required: list[tuple[str, Path]] = []
    if roles & {
        "native",
        "release",
        "asan",
        "instrumented",
        "layout",
        "browser",
        "rom",
    }:
        required.append(("ROM", rom))
    if roles & {"native", "ctest"}:
        required.append(("native binary", native))
    if "release" in roles:
        required.append(("Release binary", release))
    if roles & {"asan", "layout"}:
        required.append(("ASan binary", asan))
    if roles & {"wasm", "browser"}:
        required.append(("wasm", wasm))
    if "browser" in roles:
        required.append(("wasm loader", wasm.with_suffix(".js")))
    if roles & {"browser", "browser_save"}:
        required.extend(
            [
                ("save-tools wasm", wasm.parent / "mdkr-save-tools.wasm"),
                ("save-tools loader", wasm.parent / "mdkr-save-tools.js"),
            ]
        )
    if "browser_save" in roles:
        required.append(("native save CLI", native.parent / "mdkr-save"))
    missing = [f"{label}: {path}" for label, path in required if not path.is_file()]
    if missing:
        raise RuntimeError("missing required artifact(s):\n  " + "\n  ".join(missing))

    if "release" in roles:
        build_type = cmake_build_type(release)
        if build_type not in {"Release", "RelWithDebInfo", "MinSizeRel"}:
            rendered = build_type or "unknown (no adjacent CMakeCache.txt)"
            raise RuntimeError(
                f"{release} is not a proven optimized build "
                f"(CMAKE_BUILD_TYPE={rendered})"
            )
    if roles & {"asan", "layout"} and not has_asan(asan):
        raise RuntimeError(f"{asan} does not import __asan_init")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build",
        default="build-rel",
        help="primary build directory or binary (default: build-rel)",
    )
    parser.add_argument(
        "--release-build",
        default="build-rel",
        help="optimized build for configuration-sensitive checks",
    )
    parser.add_argument(
        "--asan-build",
        default="build-asan",
        help="AddressSanitizer build for memory checks",
    )
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--roms",
        default="build/roms",
        help="optional ROM-revision directory passed to check_rom_revision",
    )
    parser.add_argument("--wasm", default="build-web/mdkr64_web.wasm")
    parser.add_argument(
        "--only",
        action="append",
        metavar="GLOB[,GLOB...]",
        help="run only matching task/script names (repeatable; iteration only)",
    )
    parser.add_argument(
        "--primary-only",
        action="store_true",
        help="run only checks assigned to --build (specialized gates must be run separately)",
    )
    parser.add_argument(
        "--skip-instrumented",
        action="store_true",
        help="skip ASan and UBSan tasks (iteration only; not a release run)",
    )
    parser.add_argument(
        "--skip-wasm",
        action="store_true",
        help="skip linked-wasm checks (iteration only; not a release run)",
    )
    parser.add_argument(
        "--no-rebuild-instrumented",
        action="store_true",
        help="reuse existing UBSan and layout instrumented builds",
    )
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--list", action="store_true", help="list tasks and exit")
    args = parser.parse_args()

    try:
        validate_manifest()
        checks = selected_checks(args.only)
    except RuntimeError as exc:
        print(f"run_checks: FAIL — {exc}", file=sys.stderr)
        return 2

    if args.skip_instrumented:
        checks = [
            check
            for check in checks
            if check.role not in {"asan", "instrumented", "layout"}
        ]
    if args.skip_wasm:
        checks = [
            check for check in checks
            if check.role not in {"wasm", "browser", "browser_save"}
        ]
    if args.primary_only:
        checks = [
            check for check in checks if check.role in {"source", "native", "ctest"}
        ]

    if args.list:
        for check in checks:
            source = check.script or "CTest:all"
            print(
                f"{check.name:24s} [{check.role:12s}] "
                f"{source}: {check.description}"
            )
        return 0
    if not checks:
        print("run_checks: FAIL — selection is empty", file=sys.stderr)
        return 2

    native = resolve_binary(args.build)
    release = resolve_binary(args.release_build)
    asan = resolve_binary(args.asan_build)
    rom = resolve_path(args.rom)
    roms = resolve_path(args.roms)
    wasm = resolve_path(args.wasm)
    try:
        preflight(checks, native, release, asan, rom, wasm)
    except RuntimeError as exc:
        print(f"run_checks: FAIL — {exc}", file=sys.stderr)
        return 2

    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("MDKR")
    }
    environment.update({
        "MDKR_AUDIO": "0",
        # Never inherit a developer's playable repository config. Individual
        # configuration/migration checks replace this with a writable fixture.
        "MDKR_VIDEO_CONFIG_PATH": os.devnull,
        "PYTHONUNBUFFERED": "1",
    })

    print(
        f"run_checks: {len(checks)} task(s); native={native}; "
        f"release={release}; asan={asan}; wasm={wasm}",
        flush=True,
    )
    results: list[tuple[Check, int, float]] = []
    suite_start = time.monotonic()
    for index, check in enumerate(checks, 1):
        cmd = command_for(
            check,
            native,
            release,
            asan,
            rom,
            roms,
            wasm,
            args.no_rebuild_instrumented,
        )
        print(
            f"\n[{index}/{len(checks)}] {check.name} — {check.description}\n"
            f"$ {shlex.join(cmd)}",
            flush=True,
        )
        started = time.monotonic()
        try:
            proc = subprocess.run(cmd, cwd=ROOT, env=environment, check=False)
            returncode = proc.returncode
        except KeyboardInterrupt:
            print("\nrun_checks: interrupted", file=sys.stderr)
            return 130
        elapsed = time.monotonic() - started
        results.append((check, returncode, elapsed))
        label = "PASS" if returncode == 0 else f"FAIL (exit {returncode})"
        print(f"[{index}/{len(checks)}] {check.name}: {label} in "
              f"{format_duration(elapsed)}", flush=True)
        if returncode != 0 and args.fail_fast:
            break

    failures = [(check, rc, elapsed) for check, rc, elapsed in results if rc != 0]
    print("\nrun_checks: summary", flush=True)
    for check, returncode, elapsed in results:
        label = "PASS" if returncode == 0 else f"FAIL exit={returncode}"
        print(f"  {check.name:24s} {label:14s} {format_duration(elapsed):>7s}")
    total = time.monotonic() - suite_start
    if failures:
        print(
            f"run_checks: FAIL — {len(failures)}/{len(results)} task(s) failed "
            f"in {format_duration(total)}",
            file=sys.stderr,
        )
        return 1
    if len(results) != len(checks):
        print(
            f"run_checks: FAIL — ran {len(results)}/{len(checks)} selected tasks",
            file=sys.stderr,
        )
        return 1
    print(
        f"run_checks: PASS — all {len(results)} tasks passed in "
        f"{format_duration(total)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
