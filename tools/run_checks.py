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
to carry it into the ctest task. Under ``--jobs`` the CPU-bound checks pool;
the roles that share a build tree or a local port, the render/GPU checks, and
the wall-clock measurement checks stay serial (see the SERIAL_ROLES /
GPU_SERIAL_NAMES / SERIAL_NAMES model below).

A bare ``python3 tools/run_checks.py`` runs the complete suite. There is no
human gate on testing: automation windows render hidden or ordered behind the
desktop by design (set ``MDKR_TEST_VISIBLE_HEADLESS=1`` to watch one), so a run
never seizes focus from interactive work, and bulk children are launched at
background scheduling priority (wall-clock measurement checks and the browser
lanes run foreground, one at a time -- see yield_wrapper). ``--with-app-tests``,
``--with-compiled-tests``, ``--with-browser-tests`` and ``--with-gpu-tests``
are accepted but ignored: those classes now run by default.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
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
# role so its task list cannot drift from this manifest. ``browser_local``
# checks own their arguments and artifacts (so they do not use the ordinary
# browser command route), but they still launch real Chromium and/or a local
# Worker and therefore belong to the serialized web lane.
BROWSER_ROLES = ("wasm", "browser", "browser_save", "browser_local")

# The roles that build or execute the native game, a renderer, or an
# instrumented binary. Hidden-window hints (MDKR64_HIDDEN, the SDL background
# hints, the macOS accessory policy) keep these off the foreground, but they
# still own CPU and — for the render-bearing checks below — a GPU surface.
# ``instrumented``/``layout`` additionally compile a tree in place. This tuple
# is the app-executing surface the --jobs classification reasons over.
APP_ROLES = ("rom", "native", "release", "asan", "instrumented", "layout")

# ---------------------------------------------------------------------------
# --jobs serialization model.
#
# ``--jobs N`` pools CPU-bound tasks (each in its own save scratch) and keeps
# three disjoint classes sequential. ``--jobs 1`` is byte-for-byte the
# historical sequential runner.
#
#   1. SERIAL_ROLES — roles that share ONE build tree or bind fixed local
#      ports, so two of them cannot run at once no matter what they assert:
#      instrumented/layout are compiled in place, the wasm module is one
#      directory, and every browser lane serves dist/web and binds a local
#      port / spawns a Chromium profile / runs a local Worker. ``ctest`` is a
#      single broad compiled inventory (some tests GPU-labelled); it runs alone
#      after the pool drains rather than beside it. This set deliberately does
#      NOT include rom/native/release/asan — those roles pool, and only their
#      individual GPU/measurement checks are pulled out by name below. (An
#      earlier revision serialized all four wholesale, which put 192/210 tasks
#      back on the sequential path and erased the speedup.)
#
#   2. GPU_SERIAL_NAMES — rom/native/release/asan checks whose VERDICT reads
#      rendered pixels, drives a GPU surface/adapter, or otherwise contends for
#      the renderer. These flake under parallel GPU contention — the repo's
#      hard-won "GPU gates need a quiet machine" — so they serialize even though
#      their role pools. Every entry was classified by reading the check: it
#      captures a framebuffer, compares pixels, or opens a WebGPU adapter whose
#      census is the verdict. validate_manifest() re-derives the render-marker
#      set from the scripts and FAILS if a pooled engine check grows a pixel or
#      frame capture without being listed here — so a future render check
#      cannot silently rejoin the pool and reintroduce a flake.
#
#   3. SERIAL_NAMES — checks whose verdict is a wall-clock or host-load
#      measurement (realtime pacing arms, adopted-window handoffs, replay-cost
#      ceilings). Host contention turns those into false failures — the same
#      effect docs/TEST_SUITE_ECONOMICS.md §1 measured between its loaded and
#      quiet runs.
#
# Everything else pools: the CPU-bound native/rom/release/asan checks (audio,
# save, state-hash, input, rollback, numeric camera-geometry SIMHASH,
# progression) and the pure ``source`` checks. Their verdicts are deterministic
# functions of the ROM and inputs, independent of render timing, so a pooled
# run reproduces the sequential verdict byte-for-byte (the equivalence the
# economics doc requires before pooling an arm).
SERIAL_ROLES = frozenset({
    "ctest",
    "instrumented",
    "layout",
    "wasm",
    "browser",
    "browser_save",
    "browser_local",
})
# rom/native/release/asan checks whose verdict is render/GPU-surface bound.
# Curated from a read of each script (a captured framebuffer, a pixel A/B, or a
# WebGPU material/adapter census). Kept in manifest order for reviewability;
# validate_manifest() proves this set still covers every pixel/frame-capture
# check in a pooled role. Measurement-sensitive render checks (pacing_quality,
# presentation_matrix, app_adopted_pacing, motion_quality_battery) live in
# SERIAL_NAMES instead — the union is what the scheduler serializes.
GPU_SERIAL_NAMES = frozenset({
    "overlay_pause_cutscene",
    "enh_speedometer",
    "enh_draw_distance",
    "mod_texture_override",
    "sprite_layout",
    "rdp_interpolation",
    "texture_edge_classification",
    "font_sdf",
    "font_outline",
    "native_ui_resolution",
    "mip_motion",
    "rl1_vertex_colour_ab",
    "remaster_lighting",
    "world_fx_capture",
    "world_fx_matrix",
    "world_shadows",
    "render_purity",
    "camera_snapshot_coverage",
    "arbitrary_presentation_rates",
    "smoothing_stage_bisection",
    "effect_shell_envelope",
    "wave_midpoint_envelope",
    "smooth_verdict",
    "presentation_shadows",
    "presentation_breadth",
    "live_toggle_settings",
    "live_toggle_settings_asan",
    "weather_presentation_identity",
    "viewport_route_isolation",
    "shadow_stage_reset",
    "shadow_plausibility",
    "charselect_motion",
    "tool_freecam",
    "app_capture",
    "renderer_backends",
    "gpu_backpressure",
    "resource_plateau",
    "surface_suspension",
    "final_shutdown",
    "webgpu_content_census",
    "widescreen_shadow",
    "widescreen_shadow_asan",
    "video_presets",
    "widescreen_proportions",
    "framed_world_views",
    "shadow_visual_ab",
    "intro_shrub_sprite",
    "line_particle_orientation",
    "race_drive",
    "race_2p_split",
    "race_2p_split_enhanced",
    "race_multiplayer",
    "challenge_modes",
    "taj_challenges",
    "bonus_character_select",
    "bonus_results_portraits",
    "taj_character_select",
    "taj_character_select_webgpu",
    "taj_character_select_ultrawide",
    "taj_character_select_pal",
    "taj_results_portrait",
    "taj_hud_portrait",
    "taj_playable",
    "texture_lineswap",
    "adventure_two",
    "door_glyphs",
    "adventure_hub",
    "adventure_race_loop",
    "determinism",
    "rom_revision",
    "online_process_convergence",
})
SERIAL_NAMES = frozenset({
    "pacing_quality",        # realtime arms assert displayed-interval tails
    "boost_magnitude",       # MDKR_PACE_REALTIME arm
    "webgpu_recovery",       # realtime WebGPU arm
    "presentation_matrix",   # presentation perf census cost gate
    "app_adopted_pacing",    # real windows + adopted realtime pacing
    "motion_quality_battery",  # replay-cost ceiling is a CPU-time measurement
    # These long routes are scheduling-sensitive. Campaign progression is a
    # CPU-heavy synthetic traversal that exceeded its process timeout after
    # Darwin background-band demotion; the ghost and framed-view arms also use
    # host-paced bounds. Keep them alone and at the invoking shell's priority so
    # their frame and wall-clock budgets retain the semantics they assert.
    "campaign_progression",
    "ghost_matrix",
    "framed_world_views",
    # The nanosecond-budget family. Each asserts p50/p95/p99 wall-clock
    # ceilings measured inside the engine; a pooled neighbour's cache and
    # memory-bandwidth pressure inflates exactly the tails under test. The
    # first pooled full-suite runs on a quiet 14-core machine failed every
    # one of these at --jobs 6 AND at --jobs 3 (the camera gate by 3.2% at
    # jobs 3 after 3x over at jobs 6 -- pure pool pressure). Measurements
    # are only meaningful with the machine to themselves, which is exactly
    # what this set exists to provide.
    "camera_obstruction_performance_runtime",  # finalizer/observe/comfort p99
    "rollback_track_matrix",     # per-level capture/restore/resim budgets
    "rollback_item_matrix",      # per-weapon capture-restore budgets
    "rollback_vehicle_matrix",   # per-level/vehicle timing histograms
    "persistent_rollback_rematch",  # ASAN engine with internal 240s deadlines
    # The unit battery contains the audio sink contract, whose drain/stall
    # accounting is wall-clock against a real device: the documented flake
    # ("passes standalone, stalls under a busy batch") is pool pressure by
    # another name. One minute of solo time buys a deterministic verdict.
    "rom_free_units",
    # Passed the first pooled traversal by luck, failed the second with one
    # budget violation: its convergence run carries the same nanosecond
    # timing clauses as the matrices above.
    "rollback_soak",
})

# The strict verdict-bearing render/GPU-surface markers validate_manifest()
# scans for. A pooled rom/native/release/asan check that matches any of these
# is reading pixels or driving a GPU surface and MUST be serialized (listed in
# GPU_SERIAL_NAMES or SERIAL_NAMES). Deliberately strict: bare "webgpu" (a
# backend-invariance hash runs on WebGPU yet stays deterministic) and "capture"
# (audio capture) are NOT markers, or every audio/state check would be flagged.
GPU_VERDICT_MARKERS = re.compile(
    r"\.ppm|\bframebuffer\b|\bluminance\b|\bpixels?\b|read_rgba|capture_frame|"
    r"save_ppm|draw_bounds|rgba8|CAPTURE_FRAME|\bswapchain\b|present_queue|"
    r"\bfence\b|backpressure|device_lost|queue_completion|surface_suspension",
    re.IGNORECASE,
)


def is_serial(check: "Check") -> bool:
    """A task the pool must not run beside another (see the model above)."""
    return (check.role in SERIAL_ROLES
            or check.name in SERIAL_NAMES
            or check.name in GPU_SERIAL_NAMES)

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
    Check("overlay_pause_cutscene", "check_overlay_pause_cutscene.py", "native",
          "title/new-game cameras, attract racers, and a race-start flyover "
          "freeze and resume intact across the app overlay, without a flat "
          "scene, zero-rate racer crash, or camera-bank snap"),
    Check("restart_apply", "check_restart_apply.py", "native",
          "package-like Unicode/deep-path Restart & Apply and Return to "
          "Launcher process replacement, launcher-shaped replacement "
          "invocation, ROM/settings persistence, and boot/staging recovery"),
    Check("app_capture", "check_app_capture.py", "native",
          "launcher screenshot dimensions, contrast, palette, draw bounds, "
          "and broken-direction mutations",
          args=("--self-test",)),
    Check("app_ui_input", "check_app_ui_input.py", "native",
          "real ImGui keyboard/gamepad selection, the Presentation pace quick "
          "choice writing both pacing keys, reload, scale matrix, "
          "save failure, and retry"),
    Check("audio_options_persistence", "check_audio_options_persistence.py", "native",
          "original Audio Options durable exit, visible save failure, retry, "
          "and explicit session-only state"),
    Check("portable_paths", "check_portable_paths.py", "native",
          "portable.txt beside the executable relocates the settings write next "
          "to the game instead of an un-encodable home path (issue #33)"),
    Check("collision_untextured", "check_collision_untextured.py", "native",
          "untextured terrain collision"),
    Check("runtime_safety", "check_runtime_safety.py", "native",
          "teardown, racer, and audio boundary contracts"),
    Check("simulation_cadence", "check_simulation_cadence.py", "native",
          "NTSC/PAL source clocks, original/enhanced pacing mechanism, and "
          "explicit oracle policy"),
    Check("enhancement_authority", "check_enhancement_authority.py", "rom",
          "every enhancement's declared authority class matches its measured "
          "effect on the authoritative state stream, in both directions"),
    Check("subentry_bounds", "check_subentry_bounds.py", "rom",
          "an out-of-range asset sub-entry index aborts loudly with the "
          "section, index and count, instead of resolving to the section base"),
    Check("dev_tools_purity", "check_dev_tools_purity.py", "rom",
          "every registered developer tool leaves the authoritative state "
          "stream byte-identical when opened"),
    Check("enh_speedometer", "check_enh_speedometer.py", "rom",
          "the speed readout draws, tracks a standing start, changes nothing "
          "outside its own box, and moves no authoritative state"),
    Check("enh_draw_distance", "check_enh_draw_distance.py", "rom",
          "draw distance and LOD bias extend what is drawn without changing "
          "which objects exist, proving both are render culls"),
    Check("a11y_race", "check_a11y_race.py", "rom",
          "race announcements fire, coalesce, toggle per category, and leave "
          "the authoritative state stream byte-identical"),
    Check("input_hotplug", "check_input_hotplug.py", "rom",
          "a pad joining or leaving mid-run reaches the right channel, leaves "
          "exact neutral on removal, and never binds one device to two ports"),
    Check("rom_checker_page", "check_rom_checker_page.py", "rom",
          "the hosted ROM checker uploads nothing and reaches the same verdict "
          "as the native binary, character for character, on every ROM present"),
    Check("rom_text_indices", "check_rom_text_indices.py", "rom",
          "no driven route resolves a GAME_TEXT index at or above 259, the "
          "count the 1.0 revisions carry",
          # Eleven-plus serial engine runs across every fixture group: 3600s
          # still died mid-enumeration after finishing nav + the full
          # 20-track sweep + hub (~32 engine runs). Measured shape is a
          # two-hour gate; budget it honestly.
          timeout=7200),
    Check("a11y_shell", "check_a11y_shell.py", "rom",
          "every focusable control in the launcher, settings and overlay speaks "
          "its name and value, enumerated from the schema rather than a list"),
    Check("future_fun_land", "check_future_fun_land.py", "rom",
          "a full trophy set plus the Wizpig 1 bit opens the lighthouse, and "
          "one trophy short does not"),
    Check("mod_music_override", "check_mod_music_override.py", "rom",
          "a pack's music replaces the sequenced original, obeys the music "
          "volume, works from a zip, and moves no authoritative state"),
    Check("mod_texture_override", "check_mod_texture_override.py", "rom",
          "a pack's textures replace the ROM's, and the Custom content setting "
          "switches them off and back on mid-run through the settings route, "
          "off being byte-identical to having no pack installed"),
    Check("enh_ai_difficulty", "check_enh_ai_difficulty.py", "rom",
          "opponent skill leaves the authored arm bit-identical to a build "
          "without the enhancement compiled in, and the harder arms measurably "
          "beat it without wedging an opponent",
          # Configures and compiles its own -DMDKR_ENH_AI_DIFFICULTY_OMIT
          # purity baseline before racing twelve-plus arms: the same shape
          # the instrumented/layout roles get the building budget for.
          timeout=BUILDING_TASK_TIMEOUT),
    Check("tool_freecam", "check_tool_freecam.py", "rom",
          "detaching and re-attaching the free camera leaves the authoritative "
          "state stream and the re-attached frame byte-identical"),
    Check("crash_screen", "check_crash_screen.py", "rom",
          "a fatal run still prints its [CRASH]/[FATAL] marker first, exits "
          "with the same disposition, and adds a report naming the fault, "
          "tick, track, renderer and log path"),
    Check("sprite_layout", "check_sprite_layout.py", "native",
          "independent ROM sprite census and display-list allocation controls"),
    Check("rdp_interpolation", "check_rdp_interpolation.py", "native",
          "screen-linear shade/fog and clipped-fog A/B on GL and WebGPU"),
    Check("texture_edge_classification",
          "check_texture_edge_classification.py", "native",
          "authored cutout-vs-blend render-mode A/B on GL and WebGPU"),
    Check("font_sdf", "check_font_sdf.py", "native",
          "runtime-derived text mode isolation, lifecycle, bleed, GL/WebGPU"),
    Check("font_outline", "check_font_outline.py", "native",
          "outline text redraw: Pure exact, Restored/Remastered on, no bleed"),
    Check("native_ui_resolution", "check_native_ui_resolution.py", "native",
          "output-resolution HUD/text, scene isolation, ordering, GL/WebGPU"),
    Check("mip_motion", "check_mip_motion.py", "native",
          "moving Everfrost track-surface temporal stability on GL/WebGPU"),
    Check("rl1_vertex_colour_ab", "check_rl1_vertex_colour_ab.py", "native",
          "bounded Ancient Lake/Fire Mountain three-arm lighting decision"),
    Check("remaster_lighting", "check_remaster_lighting.py", "native",
          "runtime-derived sun, smooth-normal lighting, colour boundary, and mode isolation"),
    Check("line_particle_orientation", "check_line_particle_orientation.py", "native",
          "plane wing contrail ribbons spread along the ROM-authored local axis"),
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
          "witness, the unset-default arm that must reproduce observe, and the "
          "reduced-motion comfort arm"),
    Check("camera_obstruction_display_matrix", "check_camera_obstruction_display_matrix.py", "release",
          "Modern lens/target safety across 24 shape/FOV arms, emergency census, and equal-aspect identity"),
    Check("camera_dynamic_obstruction_runtime", "check_camera_dynamic_obstruction_runtime.py", "release",
          "Timber's Island hard-door publication and dynamic-blocker correction attribution"),
    Check("camera_projection_fallback_runtime", "check_camera_projection_fallback_runtime.py", "release",
          "injected projection-latch failure restores only a freshly revalidated exact pair"),
    Check("camera_probe_invalid_recovery", "check_camera_probe_invalid_recovery.py", "release",
          "one injected unprovable exact probe leaves its telemetry mark and "
          "costs no tick its published, target-visible pose"),
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
          "release", "exact original/30/40/60/120/144/165/240/display-margin/"
          "uncapped native presentation with NTSC/PAL state, event, input, "
          "and PCM invariance"),
    Check("live_toggle_settings", "check_live_toggle_settings.py", "release",
          "mid-run FrameLimit/MotionSmoothing/AllowTearing/Camera.Obstruction "
          "and Presentation pace changes apply at their declared boundary, "
          "re-rank the present mode, "
          "leave the authoritative streams byte-identical, and survive a "
          "24-flip smoothing soak without reaching retired replay state"),
    Check("presentation_matrix", "check_presentation_matrix.py", "release",
          "presentation rate vs fixed-ticket state/event authority and pixels "
          "(spec 12.2.2)"),
    Check("smoothing_stage_bisection", "check_smoothing_stage_bisection.py",
          "release",
          "per-stage smoothing attribution at the 120 Hz alpha grid: "
          "leave-one-out ranking, authored-endpoint exactness, and the "
          "uncaptured-external fail-closed refusal"),
    Check("effect_shell_envelope", "check_effect_shell_envelope.py", "release",
          "the shield/magnet shell's displacement from its own tick's authored "
          "pose stays inside the endpoint-to-endpoint interpolation envelope "
          "at the 120 Hz alpha grid"),
    Check("wave_midpoint_envelope", "check_wave_midpoint_envelope.py",
          "release",
          "wave surfaces are reconstructed, not replayed: every interpolated "
          "midpoint the wave stage touches differs from both of its authored "
          "endpoints, against a topology-refusal control that leaves the "
          "authored frames byte-identical"),
    Check("smooth_verdict", "check_smooth_verdict.py", "release",
          "the [SMOOTH-VERDICT] per-surface-class census is real, not "
          "vacuous: WATER_WAVE, OBJECT_ROOT, and WORLD_SCROLL all report on "
          "one Remastered scripted race, WATER_WAVE's rows pin its Task-7 "
          "ownership (blend>0, noowner=0), and the topology-key comparison "
          "actually ran"),
    Check("motion_quality_battery", "check_motion_quality_battery.py",
          "release",
          "the perceptual motion battery: fast-rotation shortest-arc and snap, "
          "no blended frame across a teleport, monotonic displayed alpha on "
          "the grid, the tick-boundary step bound, and the camera-cut and "
          "effect-envelope cross-references — each row with a committed "
          "negative control"),
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
    # The release arm proves the toggle applies and stays gameplay-neutral. It
    # cannot prove the thing the hazard is actually about: a stale segment base
    # is a read of freed level memory, which a release build dereferences into a
    # plausible image and reports as success. This lane is the detector.
    Check("live_toggle_settings_asan", "check_live_toggle_settings.py", "asan",
          "ASan witness for the 24-flip motion-smoothing toggle soak — the "
          "stale walk-entry / retired segment-table hazard the live apply closes",
          ("--soak-only",)),
    Check("state_hash", "check_state_hash.py", "release",
          "authoritative-hash determinism, window/backend invariance, legacy-RNG control"),
    Check("weather_rng_order", "check_weather_rng_order.py", "release",
          "weather-enabled authored object/weather/HUD RNG order and presentation invariance"),
    Check("weather_presentation_identity",
          "check_weather_presentation_identity.py", "release",
          "snow/rain vertex batches carry a presentation identity and are "
          "actually substituted under smoothing, with a non-vacuous "
          "volume-wrap guard and per-route determinism"),
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
          "no-tearing/no-underrun smoke that reports the M3 quality baseline",
          # The suite's automation windows render occluded by doctrine and the
          # machine may have no display session at all; the realtime and
          # display-switch arms cannot measure either way. The gate's own
          # design says a host that cannot pace must say so OUT LOUD -- this
          # flag downgrades those arms to loud notes in-suite. The full
          # measurement is a prepared-machine step in RELEASE_CHECKLIST.md.
          args=("--allow-no-baseline",)),
    Check("surface_suspension", "check_surface_suspension.py", "native",
          "minimized GL/WebGPU render elision and resume rebase"),
    Check("final_shutdown", "check_final_shutdown.py", "native",
          "cooperative GL/WebGPU/audio/platform final teardown"),
    Check("webgpu_recovery", "check_webgpu_recovery.py", "native",
          "WebGPU lifecycle fault injection and fail-closed recovery policy"),
    Check("webgpu_fault_matrix", "check_webgpu_fault_matrix.py", "source",
          "every WebGPU fault point wired and product-route classified"),
    Check("rollback_authority", "check_rollback_authority_wrapper.py", "source",
          "frozen mutable-authority census, rollback exclusions and omitted-state control",
          ("--self-test",)),
    Check("match_launch_direct_load", "check_match_launch_direct_load.py", "native",
          "V3 descriptor direct-load track and canonical racer selections"),
    Check("online_process_convergence", "check_online_process_convergence.py", "native",
          "four isolated endpoint authority/input/event convergence"),
    Check("online_profile_rematch", "check_online_profile_rematch.py", "native",
          "multi-epoch impaired carrier with persistent launcher identity"),
    Check("persistent_rollback_rematch", "check_persistent_rollback_rematch.py", "asan",
          "three rollback epochs and deep teardown under AddressSanitizer"),
    Check("rollback_soak", "check_rollback_soak.py", "native",
          "long rollback race convergence and retained-history recovery"),
    Check("rollback_track_matrix", "check_rollback_track_matrix.py", "native",
          "all standard tracks correct and replay a delayed input"),
    Check("rollback_item_matrix", "check_rollback_item_matrix.py", "native",
          "all 15 standard-race balloon levels activate inside a corrected "
          "input window with real effects and a suppressed-release control"),
    Check("rollback_vehicle_matrix", "check_rollback_vehicle_matrix.py", "native",
          "every ROM-derived legal standard-track vehicle pairing"),
    Check("ci_contract", "check_ci_contract.py", "source",
          "push/PR native, sanitizer, wasm, save-custody, and ROM policy"),
    Check("release_ready_web_provenance", "check_release_ready_web_provenance.py", "source",
          "candidate staged-web source-commit and clean-provenance fixtures"),
    Check("web_publish_stamp", "check_web_publish_stamp.py", "source",
          "single-build cache stamps across launcher, controller and room routes"),
    Check("browser_publish_skew", "check_browser_publish_skew.py",
          "browser_local",
          "waiting-worker deploy skew, build-isolated caches and atomic offline fallback"),
    Check("address_domains", "check_address_domains.py", "source",
          "raw pointer/token narrowing confined to typed boundary helpers"),
    Check("delta_inventory", "check_delta_inventory.py", "source",
          "every //!@Delta simulation-cadence site carries an M0 classification"),
    Check("player_prose", "check_player_prose.py", "source",
          "banned AI-slop vocabulary in launcher UI strings, README.md and "
          "RELEASE_NOTES.md"),
    Check("cadence_gating", "check_cadence_gating.py", "source",
          "no updateRate ==/!= 1/2 mode test outside "
          "platform_sim_cadence_is_enhanced()"),
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
    Check("audio_resilience", "check_audio_resilience.py", "native",
          "shipped-binary callback/ring sink wiring: mode, ring conservation, "
          "negotiated device period, and measured drain"),
    Check("vehicle_audio", "check_vehicle_audio.py", "native",
          "ROM-derived vehicle sound IDs, engine-loop activation, pitch, and "
          "idle/main crossfade"),
    Check("terry_flight_audio", "check_terry_flight_audio.py", "native",
          "Terry fly-cycle flap cue/cadence, plane-engine suppression, and "
          "ground-engine control"),
    Check("taj_theme", "check_taj_theme.py", "native",
          "Taj entrance theme resets the hub mix and plays every authored layer"),
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
    Check("bonus_character_select", "check_bonus_character_select.py", "native",
          "contiguous 13-racer picker with independent Taj, Wizpig, and Terry "
          "actor/placard composition, pose states, and controller navigation"),
    Check("bonus_results_portraits", "check_bonus_results_portraits.py", "native",
          "real post-race Wizpig/Terry portrait ownership, retail dimensions, "
          "and distinct card pixels"),
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
    Check("object_wedge", "check_object_wedge.py", "native",
          "embedded-point recovery in object-mesh collision, its norecover "
          "positive control, and post-impact escape/pitch recovery"),
    Check("door_glyphs", "check_door_glyphs.py", "native",
          "per-door balloon numeral binding across shared models and GL/WebGPU"),
    Check("adventure_race_loop", "check_adventure_race_loop.py", "native",
          "Adventure hub/race return loop"),
    Check("postrace_door_fling", "check_postrace_door_fling.py", "native",
          "post-race lobby returns stay grounded on the quit paths, with the "
          "rising-door carry-frame legacy control"),
    Check("trophy_series", "check_trophy_series.py", "native",
          "all four Adventure trophy championships, quit/retry, and EEPROM reload"),
    Check("race_finish_time", "check_race_finish_time.py", "native",
          "three-lap finish and EEPROM time"),
    Check("ghost_matrix", "check_ghost_matrix.py", "native",
          "Time Trial ghost recorded, saved, and reloaded by a fresh process "
          "over all forty-seven legal track/vehicle pairs",
          # The 47-pair driver was still making progress when its old 40-minute
          # ceiling fired on the qualification host. This is a wedge bound, not
          # a runtime target; individual children retain their own 420s guard.
          timeout=3600),
    Check("ghost_bank_capacity", "check_ghost_bank_capacity.py", "native",
          "more than six Time Trial ghost pairs in ONE save directory: no "
          "spurious NO ROOM FOR GHOSTS, and evicted pairs return from the "
          "ghost bank byte-identical (issue #46)",
          timeout=2400),
    Check("boost_magnitude", "check_boost_magnitude.py", "native",
          "zip-pad boost per-frame speed trace, racer-count independence, and "
          "perturbed-boost-constant positive controls"),
    Check("save_failsafe", "check_save_failsafe.py", "native",
          "EEPROM recovery and persistence"),
    Check("save_100_entry", "check_save_100_entry.py", "native",
          "a 100%-complete save is actually enterable through the FILE_SELECT "
          "input gate, with the AT2-config-bit defect as a positive control"),
    Check("boss_win_verdict", "check_boss_win_verdict.py", "native",
          "boss win/lose state contract"),
    Check("bluey2_rematch", "check_bluey2_rematch.py", "native",
          "progression-valid Bluey rematch parity, Enhanced fail-red control, "
          "and authored cue-transition PCM"),
    Check("first_boss_progression", "check_first_boss_progression.py", "native",
          "legal fourth Dino race through Tricky 1 save/reload"),
    Check("ai_unstick_opponents", "check_ai_unstick_opponents.py", "native",
          "CPU-opponent AI stuck-recovery cooldown across seeded Hot Top "
          "Volcano races, with a reducer positive control"),
    Check("campaign_progression", "check_campaign_progression.py", "native",
          "silver-coin collection, persistence and world balloons; all four boss "
          "rematches chained on their own saves to four Wizpig amulet pieces, "
          "with a first-encounter control; the four-piece Wizpig 1 unlock and "
          "race against a three-piece control; Wizpig 2 setting the true-ending "
          "credits bit",
          # The complete chain launches many long synthetic-frame processes.
          # The loose outer bound detects a wedge without reintroducing the
          # background-scheduling timeout that motivated serialization.
          timeout=3600),
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
    Check("persistent_app_session", "check_persistent_app_session.py", "native",
          "two complete engine epochs through one native launcher runtime"),
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
    Check("persistent_browser_session", "check_persistent_browser_session.py", "browser",
          "two complete engine epochs through one wasm module and returned launcher"),
    Check("touch_controls", "check_touch_controls.py", "browser",
          "bounded touch edge transport, overlay gating/persistence, chord and neutral"),
    Check("controller_page", "check_controller_page.py", "browser_local",
          "engine-free phone controller approval, touch and reconnect UX"),
    Check("party_host", "check_party_host.py", "browser_local",
          "launcher-owned QR pairing, approval, seats, expiry and mobile layout"),
    # Provisionally registered by the release session so the manifest guard
    # passes; descriptions taken from each script's own docstring. The online
    # campaign owner should adjust role/description when this work lands.
    Check("app_background_activation", "check_app_background_activation.py",
          "source",
          "native UI automation stays renderable without stealing desktop "
          "focus"),
    Check("party_experience_canary_smoke",
          "check_party_experience_canary_smoke.py", "browser_local",
          "operated canary and direct play smoke through local signaling "
          "loss"),
    Check("browser_online_room", "check_browser_online_room.py",
          "browser_local",
          "zero-I/O online gate, local-owner handoffs, focus and mobile reflow"),
    Check("browser_online_activation", "check_browser_online_activation.py",
          "browser_local",
          "clean-build/local-ROM release handoff, idempotence, region mapping, "
          "dirty and cross-origin refusal"),
    Check("browser_online_room_gallery", "check_browser_online_room_gallery.py",
          "browser_local",
          "shared-C 43-case browser room model, 27 keyboard routes, "
          "verification phrase, touch, accessibility and zoomed mobile reflow"),
    Check("browser_online_match_room", "check_browser_online_match_room.py",
          "browser_local",
          "explicit live MatchRoom create/join/share/rotate/select/ready, "
          "reconnect, corrupt-state recovery and pre-GO admission gate"),
    Check("browser_online_two_person", "check_browser_online_two_person.py",
          "browser_local",
          "two clean profiles over the real local Worker: share/join, "
          "outage, selection, Ready backtrack, accessibility and leave"),
    Check("party_capacity", "check_party_capacity.py", "browser_local",
          "real-Worker admission/forged-control floods, restart, weighted "
          "HTTP/socket reserve, telemetry, static recovery and zero kill switch"),
    Check("party_internal_api", "check_party_internal_api.py", "source",
          "versioned Worker/Durable Object envelope and pre-storage skew rejection"),
    Check("party_edge_policy", "check_party_edge_policy.py", "source",
          "free-plan /api-only edge rate limit and static local-play isolation"),
    Check("party_production_config", "check_party_production_config.py",
          "source",
          "production Worker environment: custom-domain route matching "
          "PARTY_ORIGIN, /api-only worker routing, and a fail-closed "
          "placeholder host"),
    # Role "source": the suite runs the validator's self-test arm, which
    # builds its own synthetic fixtures and takes no artifact. The gate's
    # positional-target arm belongs to the release lanes, which hand it a
    # packaged executable; the native role's --build/--rom shape is neither,
    # and killed the task with an argparse error the first time any full
    # suite run reached it.
    Check("party_binary_surface", "check_party_binary_surface.py", "source",
          "packaged-artifact Phone Party host/transport linkage and compiled "
          "service origin, plus the validator's own rejection paths",
          args=("--self-test",)),
    Check("release_party_origin", "check_release_party_origin.py", "source",
          "every distributable release lane gates on a valid Phone Party "
          "origin and compiles it into the artifact, plus the gate's own "
          "rejection paths", args=("--self-test",)),
    Check("release_local_only_surface", "check_release_local_only_surface.py",
          "source",
          "native previews compile out and Pages publishes no deferred cloud "
          "surface"),
    Check("binary_size_evidence", "check_binary_size_evidence.py", "source",
          "recorded packaged-artifact sizes for every release lane against "
          "generous checked-in ceilings, with skip-on-absent and a strict "
          "CI-facing lane mode"),
    Check("party_service_chaos", "check_party_service_chaos.py",
          "browser_local",
          "live-session restart lease rebind and epoch rotation, concurrent "
          "abuse flood at zero admission cost, literal-zero kill switch, and "
          "quota exhaustion as a paired phone sees it"),
    Check("party_firewall_negative", "check_party_firewall_negative.py",
          "browser_local",
          "real Worker killed mid-session and rebound as a blackhole: bounded "
          "reconnect stop, blocked room-open and blocked code entry land in "
          "the shipped recovery copy, local play stays reachable"),
    Check("phone_party_webrtc", "check_phone_party_webrtc.py", "browser_local",
          "direct phone state/control channels, input-test RTT and wasm handoff queue"),
    Check("party_native_e2e", "check_party_native_e2e.py", "browser_local",
          "native host driver, real local Worker and real controller page: "
          "seatless pending entry, matched phrases, Connected with real "
          "input, dropped-offer retry under 60s and 3s-stall recovery"),
    Check("browser_presentation_rates", "check_browser_presentation_rates.py",
          "browser", "display/capped/irregular rAF scheduling, fixed authority, "
          "and explicit uncapped/display-margin-to-display semantics"),
    Check("browser_taj_character_select",
          "check_browser_taj_character_select.py", "browser",
          "real Chrome/WebGPU Taj unlock, physical picker actor, P1 placard, "
          "animation, identity mapping, privacy, and clean teardown"),
    Check("browser_taj_persistence", "check_browser_taj_persistence.py",
          "browser", "real rejected Taj IDBFS commit, exact retry bytes, and "
          "durable unlock restoration after document reload"),
    Check("browser_magic_codes_persistence",
          "check_browser_magic_codes_persistence.py", "browser",
          "real rejected Magic Code IDBFS commit, exact sidecar bytes, and "
          "enabled-state restoration after document reload"),
    Check("browser_runtime", "check_browser_runtime.py", "browser",
          "real Chromium WebGPU, Modern camera geometry, pacing, rendering, IDBFS, and privacy",
          ("--camera-obstruction", "modern")),
)

# These check-shaped scripts are invoked by dependent CTests and require
# artifacts produced inside that CTest fixture rather than the runner's normal
# role arguments. ``rom_free_units`` owns their execution.
CTEST_COMPANION_SCRIPTS = {
    "check_multiplayer_boundaries.py",
    "check_controller_settings_persistence.py",
    "check_host_input_focus.py",
    "check_launcher_tabs.py",
    "check_network_viewport_invariance.py",
    "check_online_room_a11y.py",
    "check_online_room_actions.py",
    "check_online_room_gallery.py",
    "check_overlay_input_handoff.py",
}

# This gate consumes publish-web, an artifact made and verified inside the
# Pages workflow after the ordinary dist/web build. The general runner cannot
# manufacture that release-only stage, so workflow ownership is explicit here.
WORKFLOW_COMPANION_SCRIPTS = {
    "check_browser_local_only_release.py",
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
                  CTEST_COMPANION_SCRIPTS | WORKFLOW_COMPANION_SCRIPTS)
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
    # A check can own its own arguments and still launch Chromium or a local
    # Worker. Historically those checks were called ``source`` and entered the
    # parallel pool, which could start several browser profiles at once. Scan
    # the registered scripts as a fail-closed backstop: future browser harnesses
    # must use a role in SERIAL_ROLES rather than relying on a remembered name.
    intensive_markers = (
        "ChromeProcess", "find_chrome(", "playwright", "selenium",
        "WRANGLER", "wrangler dev", "run_attempt(",
    )
    automation_role_leaks = []
    for check in CHECKS:
        if not check.script or check.role in SERIAL_ROLES:
            continue
        source = (TESTS / check.script).read_text(encoding="utf-8")
        if any(marker in source for marker in intensive_markers):
            automation_role_leaks.append(f"{check.name} [{check.role}]")
    # Fail-closed proof of the GPU classification: a rom/native/release/asan
    # check that reads pixels or drives a GPU surface (GPU_VERDICT_MARKERS) must
    # be serialized, or parallel GPU contention can flip its verdict. If such a
    # check is still in the pool, the manifest is wrong — a new render check was
    # added without listing it in GPU_SERIAL_NAMES/SERIAL_NAMES, or a serialized
    # one was moved out. This is what keeps "default pool" safe for these roles.
    gpu_verdict_leaks = []
    for check in CHECKS:
        # APP_ROLES is every app-executing role; instrumented/layout among them
        # are already serial by role, so is_serial() below narrows the scan to
        # the pooled engine roles (rom/native/release/asan).
        if not check.script or check.role not in APP_ROLES:
            continue
        if is_serial(check):
            continue
        # Strip line comments before scanning: a check that merely *mentions*
        # a pixel gate in prose (e.g. "lands before its pixel gate") is not
        # reading pixels, and must not be forced serial for a word in a comment.
        # Markers in code or string literals (a ".ppm" suffix, a frame regex)
        # still count.
        source = "\n".join(
            line.split("#", 1)[0]
            for line in (TESTS / check.script).read_text(
                encoding="utf-8").splitlines())
        if GPU_VERDICT_MARKERS.search(source):
            gpu_verdict_leaks.append(f"{check.name} [{check.role}]")
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
    if automation_role_leaks:
        problems.append("browser/Worker checks use a parallel-safe role: " +
                        ", ".join(automation_role_leaks))
    if gpu_verdict_leaks:
        problems.append(
            "pooled checks read pixels / drive a GPU surface but are not "
            "serialized (add to GPU_SERIAL_NAMES): " +
            ", ".join(gpu_verdict_leaks))
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


def cmake_cache_bool(binary: Path, name: str) -> bool:
    cache = binary.parent / "CMakeCache.txt"
    if not cache.is_file():
        return False
    prefix = f"{name}:BOOL="
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix):
            return line[len(prefix):].strip().upper() in {"1", "ON", "TRUE", "YES"}
    return False


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


# Roles whose tasks must run at the invoking shell's scheduling priority.
# These are the browser lanes: their verdicts carry frames-per-second floors,
# hitch budgets and per-step readiness deadlines measured against a real
# Chromium, and every one of them is already serialized by SERIAL_ROLES, so
# exempting them keeps at most ONE foreground child running at a time.
FOREGROUND_ROLES = frozenset({"browser", "browser_save", "browser_local"})


def yield_wrapper(check: Check) -> list[str]:
    """Launcher prefix that makes a bulk task yield to interactive work.

    The suite's workstation-safety guarantee is that a run never fights the
    person at the desk.  It used to be implemented by the RUNNER lowering its
    own priority (nice 10 + `taskpolicy -b` on itself), which every child
    inherited.  On Apple Silicon the Darwin background band confines a
    process to the EFFICIENCY cores and throttles its I/O -- measured on an
    idle M4 Max, that doubled the rollback matrices' p99s (1.51ms against an
    840us ceiling), drained the audio sink contract, and timed browser steps
    out at varying points across otherwise-identical runs.  Nice cannot be
    given back by a child, so the demotion has to happen per-child, not on
    the runner.

    So the model is inverted: the runner keeps the invoking shell's
    priority, bulk children are LAUNCHED into the background band, and the
    wall-clock measurement set (SERIAL_NAMES, plus the browser roles) runs
    foreground.  The exempted tasks are exactly the serialized ones, so the
    foreground cost is bounded at one child at a time."""
    if check.name in SERIAL_NAMES or check.role in FOREGROUND_ROLES:
        return []
    if sys.platform == "darwin":
        return ["taskpolicy", "-b"]
    if os.name == "posix":
        return ["nice", "-n", "10"]
    return []


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
        command = yield_wrapper(check) + [
            "ctest",
            "--test-dir",
            str(native.parent),
            "--output-on-failure",
        ]
        return command
    cmd = yield_wrapper(check) + [sys.executable, str(TESTS / check.script)]
    if check.role in {"source", "browser_local"}:
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


# A pooled task can be a full game process, and an ASan game build resident set
# runs well over a gigabyte. Sizing the pool purely by --jobs is what let a
# loaded run get OOM-killed. Budget ~2 GiB per concurrent worker against total
# physical RAM and never let the pool exceed what the host can hold.
PER_JOB_MEMORY_BUDGET = 2 * 1024 * 1024 * 1024


def total_physical_memory() -> int | None:
    """Total RAM in bytes, or None when it cannot be determined portably."""
    try:
        pages = os.sysconf("SC_PHYS_PAGES")
        page_size = os.sysconf("SC_PAGE_SIZE")
    except (ValueError, OSError, AttributeError):
        return None
    if pages < 0 or page_size < 0:
        return None
    return pages * page_size


def stale_artifacts(
    artifacts: tuple[Path | str, ...]) -> list[tuple[str, str]]:
    """Artifacts older than the newest compiled source: (artifact, source).

    A binary that predates the tree it claims to test produces verdicts about
    a tree that no longer exists. This is not hypothetical: during the v1.3.0
    qualification an incrementally rebuilt worktree binary reproduced a
    RETIRED weather-oracle digest and nearly re-pinned a wrong constant.
    Sources are the git-tracked files that feed the binaries (game/, platform/,
    lib/, cmake/, CMakeLists.txt); tests and tools are interpreted, not
    compiled in. Unknown (no git, unreadable) means no claim either way.
    """
    try:
        listing = subprocess.run(
            ["git", "-C", str(ROOT), "ls-files", "-z", "--",
             "game", "platform", "lib", "cmake", "CMakeLists.txt"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            check=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return []
    newest_mtime = 0.0
    newest_source = ""
    for name in listing.stdout.decode(errors="replace").split("\0"):
        if not name:
            continue
        try:
            mtime = os.stat(ROOT / name).st_mtime
        except OSError:
            continue
        if mtime > newest_mtime:
            newest_mtime, newest_source = mtime, name
    if not newest_source:
        return []
    stale: list[tuple[str, str]] = []
    for artifact in artifacts:
        try:
            if os.stat(artifact).st_mtime < newest_mtime:
                stale.append((str(artifact), newest_source))
        except OSError:
            continue  # preflight owns missing-artifact reporting
    return stale


def memory_capped_jobs(requested: int) -> tuple[int, str | None]:
    """Clamp the requested worker count to what physical RAM can hold.

    Returns the effective count and, when it was reduced, a human note. A
    single worker is always allowed: the serial path must run even on a host
    the budget would otherwise round down to zero.
    """
    if requested <= 1:
        return max(1, requested), None
    total = total_physical_memory()
    if not total:
        return requested, None
    cap = max(1, total // PER_JOB_MEMORY_BUDGET)
    if cap >= requested:
        return requested, None
    return cap, (
        f"--jobs {requested} exceeds this host's memory budget "
        f"({total // (1024 * 1024 * 1024)} GiB); capping the pool at {cap} "
        f"worker(s) to avoid an OOM kill"
    )


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

    if ("ctest" in roles and
            not cmake_cache_bool(native, "MDKR_ENABLE_GPU_TESTS")):
        # Informative, not blocking: the suite is still a valid run without the
        # GPU-labelled CTests, it simply covers less. Configure with
        # -DMDKR_ENABLE_GPU_TESTS=ON to include them.
        print(
            "run_checks: note: build lacks -DMDKR_ENABLE_GPU_TESTS=ON, so "
            "GPU-labelled CTests are absent from this run",
            file=sys.stderr,
        )

    # Deliberately NOT all of BROWSER_ROLES. This guard is about the STAGED
    # tree under dist/web/, which the served browser lanes read. "browser",
    # "browser_save", and "browser_local" all serve that staged tree, so a
    # stale/dirty stage poisons every one of them — a stale mdkr-online-tools
    # wasm silently hung the whole online/party lane before this was widened.
    # The "wasm" role's sole check inspects the linked module in build-web/
    # directly and never touches dist/web/, so demanding a fresh stage from it
    # would fail a lane whose inputs are provably unaffected.
    if roles & {"browser", "browser_save", "browser_local"}:
        # A commit landing mid-suite would otherwise poison every browser
        # task silently except the one check that reads build-info.json —
        # and that one only when its turn comes, deep into the run.
        info_path = ROOT / "dist" / "web" / "build-info.json"
        try:
            record = json.loads(info_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise RuntimeError(f"staged web build-info unreadable: {error}")
        if not isinstance(record, dict):
            raise RuntimeError(
                f"staged web build-info must contain a JSON object: {info_path}")
        # Fail closed the way tools/ci/check_web_build_provenance.py does: a
        # git invocation that cannot answer is not evidence of freshness.
        try:
            completed = subprocess.run(
                ["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                check=True, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
        except (OSError, subprocess.CalledProcessError) as error:
            raise RuntimeError(
                f"cannot resolve HEAD to date the staged web build: {error}")
        head = completed.stdout.strip()
        if not head:
            raise RuntimeError(
                "cannot resolve HEAD to date the staged web build: "
                "git rev-parse returned nothing")
        staged = record.get("source_commit")
        if not isinstance(staged, str):
            raise RuntimeError(
                "staged web build-info source_commit is missing or not a "
                "string; rerun tools/web/build_web.sh")
        if staged != head:
            raise RuntimeError(
                "staged web output predates HEAD "
                f"({staged!r} != {head!r}); rerun tools/web/build_web.sh"
            )
        # `is False` deliberately rejects 0, null, strings, and omissions: a
        # stage built from a dirty tree matches HEAD while containing sources
        # that HEAD does not describe.
        if record.get("source_dirty") is not False:
            raise RuntimeError(
                f"staged web output source_dirty={record.get('source_dirty')!r}; "
                "rebuild from a clean source tree with tools/web/build_web.sh"
            )

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
    # Accepted and ignored. These once opted in to test classes that are now
    # simply part of the suite; they remain so that existing invocations,
    # scripts and workflows keep working unchanged.
    for deprecated in ("--with-gpu-tests", "--with-app-tests",
                       "--with-compiled-tests", "--with-browser-tests"):
        parser.add_argument(
            deprecated,
            action="store_true",
            help="deprecated no-op, kept for compatibility; this class runs by "
                 "default",
        )
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument(
        "--require-fresh", action="store_true",
        help="fail instead of warn when a tested artifact is older than the "
             "newest compiled source (qualification runs should set this)")
    parser.add_argument(
        "--jobs", type=int, default=1,
        help="run parallel-safe tasks with this many workers; tasks whose "
             "verdicts depend on wall-clock measurements, a quiet GPU, a "
             "shared build tree, or a fixed local port stay sequential "
             "regardless (see SERIAL_ROLES/GPU_SERIAL_NAMES/SERIAL_NAMES). "
             "The pool is additionally capped to what physical RAM can hold. "
             "--jobs 1 is byte-for-byte today's sequential runner.")
    parser.add_argument("--list", action="store_true", help="list tasks and exit")
    args = parser.parse_args()

    # Desktop safety is a window-layer property, not a refusal to run: every
    # automation surface is created hidden or ordered behind the desktop, so a
    # suite run never steals focus from interactive work. There is deliberately
    # no human attestation step — the only human gate in this project is
    # blessing a release candidate.

    # Bulk tasks yield to interactive work by being LAUNCHED into the
    # background band (see yield_wrapper): the runner itself keeps the
    # invoking shell's priority.  It used to demote itself with nice 10 +
    # `taskpolicy -b`, which every child inherited and no child could give
    # back -- on Apple Silicon the background band pins work to the
    # efficiency cores, which invalidated every wall-clock verdict in
    # SERIAL_NAMES and the browser lane on an otherwise idle M4 Max.

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
            if check.role not in BROWSER_ROLES
        ]
    if args.primary_only:
        checks = [
            check for check in checks if check.role in {"source", "native", "ctest"}
        ]
    # Enumerate before the opt-in gates below remove roles. --list is a
    # read-only inventory of the manifest: it starts no process, so it must
    # show the whole suite (and answer --only for a gated task) without the
    # dedicated-desktop attestation. Gating what --list can *name* would send
    # contributors looking for task names that appear not to exist.
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

    stale = stale_artifacts((native, release, asan, wasm))
    if stale:
        for artifact, source in stale:
            print(f"run_checks: STALE ARTIFACT — {artifact} is older than "
                  f"{source}; its verdicts describe a tree that no longer "
                  "exists. Rebuild before trusting this run.",
                  file=sys.stderr, flush=True)
        if args.require_fresh:
            print("run_checks: FAIL — --require-fresh refuses stale artifacts",
                  file=sys.stderr)
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
        # The suite is automation by definition: no engine or app window it
        # spawns may activate the app, become key, or raise itself over the
        # user's desktop. The window layer renders automation surfaces behind
        # existing windows (app_activation.h), so drawable-dependent gates
        # keep real surfaces. A check that genuinely needs a foreground
        # window must opt out explicitly in its own environment.
        "MDKR64_HIDDEN": "1",
        # Several legacy scenario scripts deliberately discard every MDKR*
        # variable before constructing a hermetic engine environment. SDL's
        # own hint names survive that scrub and prevent Cocoa from promoting
        # the command-line process or activating a window if it is shown.
        # Keep MDKR64_HIDDEN as the cross-platform policy and these as a macOS
        # fail-safe underneath it.
        "SDL_MAC_BACKGROUND_APP": "1",
        "SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN": "1",
        # Never inherit a developer's playable repository config. Individual
        # configuration/migration checks replace this with a writable fixture.
        "MDKR_VIDEO_CONFIG_PATH": os.devnull,
        "MDKR_SAVE_DIR": save_dir,
        "MDKR_TEST_SAVE_DIR": save_dir,
        "PYTHONUNBUFFERED": "1",
        # Keep library/build helpers from silently fanning one serialized task
        # back out across every CPU core. Explicit harness-level --jobs still
        # controls the parallel-safe pool.
        "CMAKE_BUILD_PARALLEL_LEVEL": "2",
        "CTEST_PARALLEL_LEVEL": "1",
        "OMP_NUM_THREADS": "1",
        "RAYON_NUM_THREADS": "1",
        "VECLIB_MAXIMUM_THREADS": "1",
    })
    print(
        f"run_checks: {len(checks)} task(s); native={native}; "
        f"release={release}; asan={asan}; wasm={wasm}",
        flush=True,
    )
    def task_timeout(check: Check) -> int:
        budget = check.timeout
        if check.role in {"instrumented", "layout"}:
            # These configure and compile an instrumented tree before the run.
            budget = max(budget, BUILDING_TASK_TIMEOUT)
        return max(1, int(round(budget * args.timeout_scale)))

    def run_streaming(index: int, check: Check) -> tuple[int, float]:
        """Sequential execution, byte-for-byte the pre---jobs behavior."""
        cmd = command_for(
            check, native, release, asan, rom, roms, wasm,
            args.no_rebuild_instrumented,
        )
        print(
            f"\n[{index}/{len(checks)}] {check.name} — {check.description}\n"
            f"$ {shlex.join(cmd)}",
            flush=True,
        )
        started = time.monotonic()
        timeout = task_timeout(check)
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
        elapsed = time.monotonic() - started
        if returncode == 0:
            label = "PASS"
        elif returncode == TIMEOUT_EXIT:
            label = f"FAIL (timeout after {format_duration(timeout)})"
        else:
            label = f"FAIL (exit {returncode})"
        print(f"[{index}/{len(checks)}] {check.name}: {label} in "
              f"{format_duration(elapsed)}", flush=True)
        return returncode, elapsed

    def run_captured(check: Check, log_dir: str) -> tuple[int, float, str]:
        """Parallel execution: own save scratch, output captured to a log.

        Interleaved live streams from concurrent engines are unreadable and
        un-diffable, so a pooled task writes its complete output to one file
        and the scheduler prints the header/verdict when it finishes. A
        failure replays the log tail so the console still tells the story.
        """
        cmd = command_for(
            check, native, release, asan, rom, roms, wasm,
            args.no_rebuild_instrumented,
        )
        log_path = os.path.join(log_dir, f"{check.name}.log")
        started = time.monotonic()
        timeout = task_timeout(check)
        # ignore_cleanup_errors: a timed-out check can leave an orphaned
        # grandchild (engine, Chromium, wrangler) still writing into the
        # scratch save while rmtree walks it; that race must fail THIS task
        # at worst, never crash the whole run.
        with tempfile.TemporaryDirectory(
                prefix=f"mdkr64-save-{check.name}-",
                ignore_cleanup_errors=True) as task_save, \
                open(log_path, "wb") as log:
            task_env = dict(environment)
            task_env["MDKR_SAVE_DIR"] = task_save
            task_env["MDKR_TEST_SAVE_DIR"] = task_save
            log.write((shlex.join(cmd) + "\n").encode())
            log.flush()
            try:
                proc = subprocess.run(
                    cmd, cwd=ROOT, env=task_env, check=False,
                    timeout=timeout, stdout=log, stderr=subprocess.STDOUT,
                )
                returncode = proc.returncode
            except subprocess.TimeoutExpired:
                returncode = TIMEOUT_EXIT
        return returncode, time.monotonic() - started, log_path

    results: list[tuple[Check, int, float]] = []
    suite_start = time.monotonic()
    jobs, memory_note = memory_capped_jobs(max(1, args.jobs))
    if memory_note:
        print(f"run_checks: {memory_note}", file=sys.stderr, flush=True)
    try:
        if jobs == 1:
            for index, check in enumerate(checks, 1):
                returncode, elapsed = run_streaming(index, check)
                results.append((check, returncode, elapsed))
                if returncode != 0 and args.fail_fast:
                    break
        else:
            from concurrent.futures import ThreadPoolExecutor
            pooled = [c for c in checks if not is_serial(c)]
            serial = [c for c in checks if is_serial(c)]
            log_scratch = tempfile.mkdtemp(prefix="mdkr64-run-checks-logs-")
            print(f"run_checks: --jobs {jobs}: {len(pooled)} pooled task(s), "
                  f"{len(serial)} sequential; task logs in {log_scratch}",
                  flush=True)
            done = 0
            failed_early = False
            pool = ThreadPoolExecutor(max_workers=jobs)
            try:
                futures = {pool.submit(run_captured, c, log_scratch): c
                           for c in pooled}
                from concurrent.futures import as_completed
                for future in as_completed(futures):
                    check = futures[future]
                    try:
                        returncode, elapsed, log_path = future.result()
                    except Exception as error:  # harness fault, not a gate verdict
                        returncode, elapsed = 1, 0.0
                        log_path = os.path.join(log_scratch,
                                                f"{check.name}.log")
                        print(f"run_checks: {check.name} harness fault: "
                              f"{error}", file=sys.stderr, flush=True)
                    done += 1
                    results.append((check, returncode, elapsed))
                    label = "PASS" if returncode == 0 else (
                        "FAIL (timeout)" if returncode == TIMEOUT_EXIT
                        else f"FAIL (exit {returncode})")
                    print(f"[{done}/{len(checks)}] {check.name}: {label} in "
                          f"{format_duration(elapsed)}", flush=True)
                    if returncode != 0:
                        # A harness fault (or a task that raised before opening
                        # its log) may leave no file. Replay whatever exists;
                        # never let a missing log crash the whole run.
                        try:
                            with open(log_path, "rb") as log:
                                tail = log.read().decode(
                                    errors="replace").splitlines()
                        except OSError as read_error:
                            tail = [f"(no captured log: {read_error})"]
                        # Gates print their verdict first and their evidence
                        # after it, so a chatty engine log can scroll the
                        # verdict line out of a fixed tail: the suite log then
                        # shows a task that failed with no reason anywhere.
                        # Replay the last verdict-bearing lines that the tail
                        # window would drop, then the tail itself.
                        dropped = tail[:-60]
                        verdicts = [line for line in dropped if "FAIL" in line]
                        if verdicts:
                            print("    | --- verdict lines above the tail "
                                  "window ---", flush=True)
                            for line in verdicts[-15:]:
                                print(f"    | {line}", flush=True)
                            print("    | --- log tail ---", flush=True)
                        for line in tail[-60:]:
                            print(f"    | {line}", flush=True)
                        if args.fail_fast:
                            failed_early = True
                            break
            except KeyboardInterrupt:
                # Cancel everything still queued and stop waiting: Ctrl-C
                # must interrupt NOW, not after the remaining pooled tasks
                # drain (Executor.__exit__ would otherwise wait for them).
                pool.shutdown(wait=False, cancel_futures=True)
                raise
            finally:
                pool.shutdown(wait=True, cancel_futures=failed_early)
            # The measurement-sensitive tail runs alone, in manifest order, on
            # a host the pool has finished loading.
            if not failed_early:
                for check in serial:
                    done += 1
                    returncode, elapsed = run_streaming(done, check)
                    results.append((check, returncode, elapsed))
                    if returncode != 0 and args.fail_fast:
                        break
            # Manifest order keeps the summary comparable across runs.
            order = {c.name: i for i, c in enumerate(checks)}
            results.sort(key=lambda row: order[row[0].name])
    except KeyboardInterrupt:
        print("\nrun_checks: interrupted", file=sys.stderr)
        return 130

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
