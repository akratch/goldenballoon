#!/usr/bin/env python3
"""Run every registered mdkr64 regression check with the correct artifact.

The suite has five invocation shapes:

* ROM-free CTest targets use the selected native build directory;
* ordinary behavioural checks use the selected native build;
* optimisation- and sanitizer-specific checks use dedicated Release/ASan builds;
* structural checks inspect an instrumented build or the linked wasm directly;
* browser gates serve wasm modules to an isolated real Chromium profile.

This runner owns those differences and fails if a new ``tests/check_*.py`` is
not registered below, or if a ``tests/test_*.py`` has no CMake ``add_test()``
to carry it into the ctest task. It runs sequentially because several checks intentionally
create and remove ``save/eeprom.bin``. The default is the complete suite; use
``--only`` only for iteration.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"

# The suite scrubs MDKR* out of the child environment so a developer's shell
# cannot steer a run. These are the variables the suite itself owns and must
# therefore survive the scrub.
MDKR_ENV_ALLOWLIST = frozenset({
    "MDKR_SAVE_DIR",
    "MDKR_TEST_SAVE_DIR",
    "MDKR_TEXCACHE_VERIFY",
})

# A wedged task is a failure, not a hung release run. These are deliberately
# loose upper bounds on a task that is still making progress, not measured
# runtimes: a budget tight enough to be interesting would turn a slow host into
# a false failure, which is the opposite of what this gate is for. Tasks that
# configure and compile their own instrumented tree before running a matrix get
# the larger budget. Use --timeout-scale on a host slower than this one.
DEFAULT_TASK_TIMEOUT = 1800
BUILDING_TASK_TIMEOUT = 10800

# The roles that constitute the web lane. The release checklist selects them by
# role so its task list cannot drift from this manifest.
BROWSER_ROLES = ("wasm", "browser", "browser_save")

# Distinct from any check's own exit status, so a wedged task cannot be
# mistaken for a task that ran and reported.
TIMEOUT_EXIT = 124


@dataclass(frozen=True)
class Check:
    name: str
    script: str
    role: str
    description: str
    args: tuple[str, ...] = ()
    timeout: int = DEFAULT_TASK_TIMEOUT


# Cheap, broad gates lead; long scenario/matrix checks follow. Keep every
# tests/check_*.py represented at least once: validate_manifest() enforces it.
CHECKS = (
    Check("rom_free_units", "", "ctest",
          "ROM-free display, endian, vehicle-audio, magic-code, object-layout, "
          "allocator, and subsystem contract unit tests"),
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
    Check("overlay_pause", "check_overlay_pause.py", "native",
          "production WebGPU app overlay holds exact race state, clock, kart, "
          "checkpoint, and lap before resuming"),
    Check("restart_apply", "check_restart_apply.py", "native",
          "package-like Unicode/deep-path Restart & Apply and Return to "
          "Launcher process replacement, launcher-shaped replacement "
          "invocation, ROM/settings persistence, and boot/staging recovery"),
    Check("app_capture", "check_app_capture.py", "native",
          "launcher screenshot dimensions, contrast, palette, draw bounds, "
          "and broken-direction mutations",
          args=("--self-test",)),
    Check("app_ui_input", "check_app_ui_input.py", "native",
          "real ImGui keyboard/gamepad selection, reload, scale matrix, "
          "save failure, and retry"),
    Check("audio_options_persistence", "check_audio_options_persistence.py", "native",
          "original Audio Options durable exit, visible save failure, retry, "
          "and explicit session-only state"),
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
    Check("camera_obstruction_runtime", "check_camera_obstruction_runtime.py", "release",
          "same-binary legacy/center-ray controls, modern resolved-lens safety "
          "witness, and the unset-default arm that must reproduce observe"),
    Check("camera_obstruction_display_matrix", "check_camera_obstruction_display_matrix.py", "release",
          "Modern lens/target safety across 24 shape/FOV arms, emergency census, and equal-aspect identity"),
    Check("camera_dynamic_obstruction_runtime", "check_camera_dynamic_obstruction_runtime.py", "release",
          "Timber's Island hard-door publication and dynamic-blocker correction attribution"),
    Check("camera_projection_fallback_runtime", "check_camera_projection_fallback_runtime.py", "release",
          "injected projection-latch failure restores only a freshly revalidated exact pair"),
    Check("camera_obstruction_lifecycle_runtime", "check_camera_obstruction_lifecycle_runtime.py", "release",
          "Modern camera state retirement and fresh safe solves across quit, restart, and hub/race reloads"),
    Check("camera_obstruction_performance_runtime", "check_camera_obstruction_performance_runtime.py", "release",
          "4P p50/p95/p99/tails, query fan, exact cache/sidecar bytes, and Observe/Modern authority identity"),
    Check("camera_emergency_readability_runtime", "check_camera_emergency_readability_runtime.py", "release",
          "forced-no-alternate gameplay stress triggers only safe per-viewport racer fading"),
    Check("camera_3p_tt_runtime", "check_camera_3p_tt_runtime.py", "release",
          "3P T.T. fourth viewport resolves camera 3 safely without relying on inherited autopilot progress"),
    Check("camera_motion_quality", "check_camera_motion_quality.py", "release",
          "MOTION-01 hard chatter/shoulder-flip/emergency-dwell invariants plus "
          "the reported jerk and latency baseline on the three pinned routes"),
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
    Check("presentation_shadows", "check_presentation_shadows.py", "release",
          "terrain-projected kart-shadow replay stays rigid, exact at authored "
          "endpoints, and measurably reduces the historical vertex-lerp pulse"),
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
    Check("pacing_quality", "check_pacing_quality.py", "native",
          "displayed-interval, interpolation-phase and present-queue-latency "
          "distributions across every policy/smoothing arm, plus a realtime "
          "no-tearing/no-underrun smoke that reports the M3 quality baseline"),
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
    Check("release_ready_web_provenance", "check_release_ready_web_provenance.py", "source",
          "candidate staged-web source-commit and clean-provenance fixtures"),
    Check("address_domains", "check_address_domains.py", "source",
          "raw pointer/token narrowing confined to typed boundary helpers"),
    Check("camera_track_occlusion_cache", "check_camera_track_occlusion_cache.py", "source",
          "native static visual-triangle camera cache lifecycle and provenance"),
    Check("camera_object_occlusion_cache", "check_camera_object_occlusion_cache.py", "source",
          "native object-model camera cache ownership and generation lifecycle"),
    Check("camera_dynamic_occlusion", "check_camera_dynamic_occlusion.py", "source",
          "fixed-tick dynamic hard-occluder publication, identity, and purity"),
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
    Check("framed_world_views", "check_framed_world_views.py", "native",
          "fixed-aspect live menu views remain inside their 4:3 regions"),
    Check("shadow_visual_ab", "check_shadow_visual_ab.py", "native",
          "moving-camera projected-shadow pixel A/B"),
    Check("audio_output", "check_audio_output.py", "native",
          "audio content, timing, and reverb"),
    Check("final_lap_music", "check_final_lap_music.py", "native",
          "real three-lap race, live sequencer tempo change, PCM beat grid, "
          "and stale-tempo control"),
    Check("audio_sink_evidence", "check_audio_sink_evidence.py", "native",
          "real-ROM accepted-SDL PCM capture, loss telemetry, and headless opt-out"),
    Check("vehicle_audio", "check_vehicle_audio.py", "native",
          "ROM-derived vehicle sound IDs, engine-loop activation, pitch, and "
          "idle/main crossfade"),
    Check("audio_level_reference", "check_audio_level_reference.py", "native",
          "absolute output level: RMS/crest/true-peak/per-band/per-slice against "
          "the frozen baseline, with injected-gain controls"),
    Check("resource_plateau", "check_resource_plateau.py", "native",
          "repeated GL/WebGPU stage, pool/audio/GPU/registry ownership plateau"),
    Check("raw16_audio", "check_raw16_audio.py", "native",
          "RAW16 bank census, endian oracle, and fixed/legacy PCM A/B"),
    Check("texture_lineswap", "check_texture_lineswap.py", "native",
          "RDP odd-row texture decode"),
    Check("intro_shrub_sprite", "check_intro_shrub_sprite.py", "native",
          "multi-batch billboard sprite tiles in the authored intro"),
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
    Check("taj_character_select", "check_taj_character_select.py", "native",
          "visible, contiguous Taj roster tile and real selection across all "
          "four 8/9/9/10-character retail layouts"),
    Check("taj_character_select_webgpu", "check_taj_character_select.py", "native",
          "complete Taj roster/model/shadow qualification on native WebGPU",
          ("--renderer", "webgpu")),
    Check("taj_character_select_ultrawide", "check_taj_character_select.py", "native",
          "Taj picker actor and authored roster safe area at 21:9",
          ("--layout", "base", "--aspect", "21:9")),
    Check("taj_character_select_pal", "check_taj_character_select.py", "native",
          "supported PAL Rev 1 Taj picker actor, scale, shadow, and navigation",
          ("--layout", "base", "--require-pal")),
    Check("taj_results_portrait", "check_taj_results_portrait.py", "native",
          "real two-player Rankings ownership and visible retail-sized native "
          "Taj portrait with an ordinary Diddy negative control"),
    Check("taj_hud_portrait", "check_taj_hud_portrait.py", "native",
          "real P2 Adventure hub HUD portrait pixels with observation-only "
          "lead state"),
    Check("taj_playable", "check_taj_playable.py", "native",
          "Taj unlock, virtual select, carpet lifecycle, OP handling, two-player "
          "identity, sidecar persistence, and Time Trial quarantine"),
    Check("taj_p2_adventure", "check_taj_p2_adventure.py", "native",
          "P2-visible Taj selection, retail Adventure lead handoff, post-swap "
          "live-port rebinding, and no virtual character IDs"),
    Check("taj_speed_profile", "check_taj_speed_profile.py", "native",
          "paired car, hovercraft, and plane real-ROM controls for Taj's 1.35x "
          "sustained-speed profile"),
    Check("taj_visual_lifecycle", "check_taj_visual_lifecycle.py", "native",
          "atomic picker/race companion loss, donor restoration, and bounded "
          "fresh-generation recomposition"),
    Check("taj_vehicle_sweep", "check_vehicle_sweep.py", "native",
          "Taj identity, presentation, shield anchoring, and dash evidence over "
          "all forty-seven legal track/vehicle pairs",
          ("--taj", "--frames", "5200")),
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
    Check("campaign_progression", "check_campaign_progression.py", "native",
          "silver-coin collection, persistence and world balloons; all four boss "
          "rematches and their Wizpig amulet pieces with a first-encounter "
          "control; Wizpig 2 setting the true-ending credits bit"),
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
    Check("browser_taj_character_select",
          "check_browser_taj_character_select.py", "browser",
          "real Chrome/WebGPU Taj unlock, physical picker actor, P1 placard, "
          "animation, identity mapping, privacy, and clean teardown"),
    Check("browser_taj_persistence", "check_browser_taj_persistence.py",
          "browser", "real rejected Taj IDBFS commit, exact retry bytes, and "
          "durable unlock restoration after document reload"),
    Check("browser_runtime", "check_browser_runtime.py", "browser",
          "real Chromium WebGPU, Modern camera geometry, pacing, rendering, IDBFS, and privacy",
          ("--camera-obstruction", "modern")),
)

# These check-shaped scripts are invoked by dependent CTests and require
# artifacts produced inside that CTest fixture rather than the runner's normal
# role arguments. ``rom_free_units`` owns their execution.
CTEST_COMPANION_SCRIPTS = {
    "check_controller_settings_persistence.py",
    "check_host_input_focus.py",
    "check_launcher_tabs.py",
    "check_overlay_input_handoff.py",
}


def cmake_registered_test_scripts() -> set[str]:
    """Script basenames a CMake ``add_test()`` command actually runs."""
    # CMakeLists.txt include()s cmake/tests.cmake for the bulk of the ROM-free
    # unit/contract registrations (kept out of the top-level file so the build
    # definition isn't buried under a thousand lines of test wiring); scan both
    # so that split doesn't make this manifest check blind to half the suite.
    registered: set[str] = set()
    for relpath in ("CMakeLists.txt", "cmake/tests.cmake"):
        # Comment stripping is load-bearing: a commented-out registration reads
        # exactly like a live one to a plain substring scan.
        text = "\n".join(
            line.split("#", 1)[0]
            for line in (ROOT / relpath).read_text(encoding="utf-8").splitlines())
        for block in re.finditer(r"add_test\((.*?)\)", text, re.DOTALL):
            registered.update(re.findall(r"tests/([A-Za-z0-9_]+\.py)", block.group(1)))
    return registered


def validate_manifest() -> None:
    discovered = {path.name for path in TESTS.glob("check_*.py")}
    registered = ({check.script for check in CHECKS if check.script} |
                  CTEST_COMPANION_SCRIPTS)
    missing = sorted(discovered - registered)
    stale = sorted(registered - discovered)
    duplicate_names = sorted(
        name for name in {check.name for check in CHECKS}
        if sum(check.name == name for check in CHECKS) != 1
    )
    # tests/test_*.py reach the suite only through the ctest task, so this
    # manifest cannot see them; the registration they depend on is CMake's.
    # Cross-check it here, because dropping an add_test() line otherwise
    # removes a gate from every lane without failing anything.
    unit_scripts = {path.name for path in TESTS.glob("test_*.py")}
    cmake_scripts = cmake_registered_test_scripts()
    unregistered_units = sorted(unit_scripts - cmake_scripts)
    stale_units = sorted(name for name in cmake_scripts - unit_scripts
                         if name.startswith("test_"))
    problems: list[str] = []
    if missing:
        problems.append("unregistered check scripts: " + ", ".join(missing))
    if stale:
        problems.append("manifest entries point at missing scripts: " + ", ".join(stale))
    if duplicate_names:
        problems.append("duplicate task names: " + ", ".join(duplicate_names))
    if unregistered_units:
        problems.append("tests/test_*.py missing a CMake add_test(): " +
                        ", ".join(unregistered_units))
    if stale_units:
        problems.append("add_test() names missing unit scripts: " +
                        ", ".join(stale_units))
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
    if "asan_init" in proc.stdout:
        return True
    # A statically linked sanitizer runtime imports nothing: the interceptors
    # are defined in the image instead.
    try:
        defined = subprocess.run(
            ["nm", "--defined-only", str(binary)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
            text=True,
        )
    except OSError:
        return False
    return "__asan_" in defined.stdout


def split_values(values: list[str] | None) -> list[str]:
    return [
        item.strip()
        for value in values or []
        for item in value.split(",")
        if item.strip()
    ]


def subset_label(args: argparse.Namespace, count: int) -> str:
    """Name every restriction applied to the manifest, for the verdict line."""
    restrictions: list[str] = []
    if args.only:
        restrictions.append("--only " + ",".join(split_values(args.only)))
    if args.role:
        restrictions.append("--role " + ",".join(split_values(args.role)))
    if args.primary_only:
        restrictions.append("--primary-only")
    if args.skip_instrumented:
        restrictions.append("--skip-instrumented")
    if args.skip_wasm:
        restrictions.append("--skip-wasm")
    if not restrictions:
        return f"complete suite, {count}/{len(CHECKS)} tasks"
    return (
        f"SUBSET {count}/{len(CHECKS)} tasks: " + " ".join(restrictions)
    )


def selected_roles(role_values: list[str] | None, checks: list[Check]) -> list[Check]:
    roles = split_values(role_values)
    if not roles:
        return checks
    known = {check.role for check in CHECKS}
    unknown = sorted(set(roles) - known)
    if unknown:
        raise RuntimeError(
            "unknown role(s): " + ", ".join(unknown)
            + "; known roles: " + ", ".join(sorted(known))
        )
    return [check for check in checks if check.role in roles]


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
            str(ROOT / "dist" / "web"),
            "--shell-dir",
            str(ROOT / "dist" / "web"),
            "--rom",
            str(rom),
        ]
    elif check.role == "browser_save":
        cmd += [
            "--engine-dir",
            str(ROOT / "dist" / "web"),
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
    # framed_world_views joins them for the region reason as well: PAL composes
    # against a 264-row framebuffer, so how a widescreen host maps the authored
    # 2D envelope is a different question there than on NTSC.
    if check.name in {"rom_revision", "simulation_cadence",
                      "arbitrary_presentation_rates", "presentation_breadth",
                      "taj_character_select_pal", "framed_world_views"}:
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
    if "wasm" in roles:
        required.extend(
            [("wasm", wasm), ("wasm loader", wasm.with_suffix(".js"))]
        )
    if "browser" in roles:
        required.extend(
            [
                ("staged wasm", ROOT / "dist" / "web" / "mdkr64_web.wasm"),
                ("staged wasm loader", ROOT / "dist" / "web" /
                 "mdkr64_web.js"),
            ]
        )
    if roles & {"browser", "browser_save"}:
        required.extend(
            [
                ("save-tools wasm", ROOT / "dist" / "web" /
                 "mdkr-save-tools.wasm"),
                ("save-tools loader", ROOT / "dist" / "web" /
                 "mdkr-save-tools.js"),
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
        "--role",
        action="append",
        metavar="ROLE[,ROLE...]",
        help="run only tasks with these manifest roles (repeatable); "
             "the web lane is " + ",".join(BROWSER_ROLES),
    )
    parser.add_argument(
        "--primary-only",
        action="store_true",
        help="run only checks assigned to --build (specialized gates must be run separately)",
    )
    parser.add_argument(
        "--timeout-scale",
        type=float,
        default=1.0,
        help="multiply every task's timeout budget (slow hosts only)",
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
        checks = selected_roles(args.role, selected_checks(args.only))
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
        if not key.startswith("MDKR") or key in MDKR_ENV_ALLOWLIST
    }
    # No task may touch the repository's playable save/ directory. The run owns
    # one directory and hands it to both sides: MDKR_SAVE_DIR is what the engine
    # writes through, MDKR_TEST_SAVE_DIR is where a check looks for the result.
    # An explicit MDKR_TEST_SAVE_DIR wins so a single task can still be aimed.
    save_scratch: tempfile.TemporaryDirectory[str] | None = None
    save_dir = environment.get("MDKR_TEST_SAVE_DIR", "")
    if not save_dir:
        save_scratch = tempfile.TemporaryDirectory(prefix="mdkr64-run-checks-save-")
        save_dir = save_scratch.name
    save_dir = os.path.abspath(save_dir)
    os.makedirs(save_dir, exist_ok=True)
    environment.update({
        "MDKR_AUDIO": "0",
        # Never inherit a developer's playable repository config. Individual
        # configuration/migration checks replace this with a writable fixture.
        "MDKR_VIDEO_CONFIG_PATH": os.devnull,
        "MDKR_SAVE_DIR": save_dir,
        "MDKR_TEST_SAVE_DIR": save_dir,
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
        budget = check.timeout
        if check.role in {"instrumented", "layout"}:
            # These configure and compile an instrumented tree before the run.
            budget = max(budget, BUILDING_TASK_TIMEOUT)
        timeout = max(1, int(round(budget * args.timeout_scale)))
        try:
            proc = subprocess.run(
                cmd, cwd=ROOT, env=environment, check=False, timeout=timeout
            )
            returncode = proc.returncode
        except subprocess.TimeoutExpired:
            # A wedged task is a failed task. Record it and keep going so the
            # summary still reports every other task's real verdict.
            returncode = TIMEOUT_EXIT
            print(
                f"[{index}/{len(checks)}] {check.name}: no exit within "
                f"{format_duration(timeout)} — killed",
                file=sys.stderr,
                flush=True,
            )
        except KeyboardInterrupt:
            print("\nrun_checks: interrupted", file=sys.stderr)
            return 130
        elapsed = time.monotonic() - started
        results.append((check, returncode, elapsed))
        if returncode == 0:
            label = "PASS"
        elif returncode == TIMEOUT_EXIT:
            label = f"FAIL (timeout after {format_duration(timeout)})"
        else:
            label = f"FAIL (exit {returncode})"
        print(f"[{index}/{len(checks)}] {check.name}: {label} in "
              f"{format_duration(elapsed)}", flush=True)
        if returncode != 0 and args.fail_fast:
            break

    failures = [(check, rc, elapsed) for check, rc, elapsed in results if rc != 0]
    print("\nrun_checks: summary", flush=True)
    for check, returncode, elapsed in results:
        if returncode == 0:
            label = "PASS"
        elif returncode == TIMEOUT_EXIT:
            label = "FAIL timeout"
        else:
            label = f"FAIL exit={returncode}"
        print(f"  {check.name:24s} {label:14s} {format_duration(elapsed):>7s}")
    total = time.monotonic() - suite_start
    # A subset can never read as a completed suite: the verdict line names the
    # restriction that produced it and how much of the manifest it covered.
    scope = subset_label(args, len(checks))
    if failures:
        print(
            f"run_checks: FAIL — {len(failures)}/{len(results)} task(s) failed "
            f"in {format_duration(total)} [{scope}]",
            file=sys.stderr,
        )
        return 1
    if len(results) != len(checks):
        print(
            f"run_checks: FAIL — ran {len(results)}/{len(checks)} selected tasks "
            f"[{scope}]",
            file=sys.stderr,
        )
        return 1
    print(
        f"run_checks: PASS — all {len(results)} tasks passed in "
        f"{format_duration(total)} [{scope}]"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
