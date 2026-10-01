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

A bare ``python3 tools/run_checks.py`` runs the complete suite only with an
inherited human-attested ``MDKR_DEDICATED_TEST_DESKTOP=1``. Hidden windows and
background scheduling are defense in depth, not permission to run on an
occupied workstation. ``--list`` is the non-executing exception. Bulk children
are launched at background scheduling priority (wall-clock measurement checks
and browser lanes run foreground, one at a time -- see yield_wrapper). ``--with-app-tests``,
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
    # Inherit the caller's attestation; never manufacture one in this runner.
    "MDKR_DEDICATED_TEST_DESKTOP",
    "MDKR_SAVE_DIR",
    "MDKR_TEST_SAVE_DIR",
    "MDKR_TEST_BROWSER_STARTUP_DIAGNOSTICS",
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
    "widescreen_minimap_alignment",
    "enh_speedometer",
    "enh_draw_distance",
    "mod_texture_override",
    "sprite_layout",
    "rdp_interpolation",
    "texture_edge_classification",
    "font_sdf",
    "font_outline",
    "native_ui_resolution",
    "split_screen_pause_resolution",
    "split_screen_void_coverage",
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
    "input_script_capacity",
    "surface_suspension",
    "final_shutdown",
    "webgpu_content_census",
    "widescreen_shadow",
    "widescreen_shadow_asan",
    "video_presets",
    "widescreen_proportions",
    "widescreen_hud_layers",
    "split_screen_backdrop",
    "framed_world_views",
    "shadow_visual_ab",
    "intro_shrub_sprite",
    "line_particle_orientation",
    "race_drive",
    "race_2p_split",
    "race_2p_split_enhanced",
    "race_multiplayer",
    # Reads per-viewport pixels for the split-screen liveness assertions, so it
    # must not share a GPU surface with pooled checks.
    "adventure_party_hub",
    # AP-19's verdict is the four-camera DL/resource high-water itself. Running
    # beside another renderer process would invalidate the plateau evidence.
    "adventure_party_performance",
    "adventure_party_performance_soaks",
    "challenge_modes",
    "taj_challenges",
    "bonus_character_select",
    "custom_character_roster",
    "custom_character_identity_surfaces",
    "adventure_party_custom_characters",
    "custom_character_flag_portrait",
    "character_raw_intake_ui",
    "character_quit_lifecycle_ui",
    "character_workshop_history_ui",
    "character_draft_transfer_ui",
    "character_portrait_studio_ui",
    "character_test_evidence_ui",
    "custom_character_workshop_preview",
    "bonus_results_portraits",
    "wizpig_race_voice",
    "taj_selection_music",
    "sprite_models_gpu",
    "bonus_portrait_pack",
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
    "save_options_scroll_band",
    "track_preview_time_steady",
    "determinism",
    "rom_revision",
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
    "ghost_bank_capacity",   # dozens of full boot+race+save cycles, each with
                             # a host-paced 420s guard; banded in the pool its
                             # first write ran 401s and the rest timed out
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

# A check that cannot run without an input only the operator can supply exits
# with this status and says why. Only the checks named here may use it; from
# any other check it is an ordinary failure. A SKIPPED task is never counted as
# passed: the summary and the verdict line name it, the verdict never calls
# such a run a complete suite, and in a release run (--require-fresh or
# --require-shipping-sdl) a skip is a failure naming the missing input.
SKIP_EXIT = 77


@dataclass(frozen=True)
class SkipContract:
    reason: str
    missing_input: str


SKIPPABLE_CHECKS = {
    "previous_release_parity": SkipContract(
        "no --reference-build (a build of the previous published release) "
        "was supplied", "--reference-build"),
}
# Checks that are handed --reference-build when the operator supplies one.
REFERENCE_BUILD_CHECKS = frozenset(
    name for name, contract in SKIPPABLE_CHECKS.items()
    if contract.missing_input == "--reference-build")


def was_skipped(check: "Check", returncode: int) -> bool:
    return returncode == SKIP_EXIT and check.name in SKIPPABLE_CHECKS


def release_mode(args: argparse.Namespace) -> bool:
    """A run that asks for release qualification, and so may not skip."""
    return bool(args.require_fresh or args.require_shipping_sdl)


def counts_as_failure(check: "Check", returncode: int,
                      args: argparse.Namespace) -> bool:
    """Whether a task result fails the run (a skip does in a release run)."""
    return returncode != 0 and (not was_skipped(check, returncode)
                                or release_mode(args))


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
    Check("launcher_skip", "check_launcher_skip.py", "native",
          "the opt-in skip boots the remembered ROM through the same final "
          "check Play uses, and Shift or both shoulders held at launch opens "
          "the launcher anyway"),
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
    Check("app_scroll", "check_app_scroll.py", "native",
          "launcher panel scrolling through the production SDL adapter: a "
          "MacBook-trackpad-shaped fractional-preciseY wheel (integer y zero), "
          "a natural-scroll FLIPPED wheel (scrolls the right way), and a "
          "touchscreen drag all advance ScrollY"),
    Check("phone_panel_focus", "check_phone_panel_focus.py", "native",
          "keyboard and gamepad reach Start Local Play in the closed phone "
          "surface at 640x480; no room is opened"),
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
    Check("enhancement_authority", "check_enhancement_authority.py", "native",
          "every enhancement's declared authority class matches its measured "
          "effect on the authoritative state stream, in both directions"),
    Check("subentry_bounds", "check_subentry_bounds.py", "native",
          "an out-of-range asset sub-entry index aborts loudly with the "
          "section, index and count, instead of resolving to the section base"),
    Check("custom_track_loader", "check_custom_track_loader.py", "native",
          "five installed synthetic custom-track sections preserve retail tails, "
          "table ownership, checked mixed reads and detach restoration; "
          "not authored-course gameplay qualification"),
    Check("custom_track_loader_asan", "check_custom_track_loader.py", "asan",
          "sanitizer coverage of installed synthetic sections, native DMA host "
          "destinations and session detachment before retail boot continues"),
    Check("custom_save_inventory", "check_custom_save_inventory.py", "native",
          "real progression/time codecs preserve retail bytes with synthetic "
          "appended courses; guarded buffers and empty custom defaults"),
    Check("custom_save_inventory_asan", "check_custom_save_inventory.py", "asan",
          "sanitizer coverage of retail codec isolation from appended metadata"),
    Check("save_v170_compat", "check_save_v170_compat.py", "native",
          "the game's own codec decodes the save the published 1.7.0 build wrote "
          "to every fact 1.7.0 reported and re-encodes it byte for byte; a "
          "one-bit-shifted control is rejected"),
    Check("save_v170_compat_asan", "check_save_v170_compat.py", "asan",
          "sanitizer coverage of the game codec reading a caller-named EEPROM "
          "image through MDKR_SAVE_COMPAT_FIXTURE"),
    Check("dev_tools_purity", "check_dev_tools_purity.py", "native",
          "every registered developer tool leaves the authoritative state "
          "stream byte-identical when opened"),
    Check("enh_speedometer", "check_enh_speedometer.py", "native",
          "the speed readout draws, tracks a standing start, changes nothing "
          "outside its own box, and moves no authoritative state"),
    Check("enh_draw_distance", "check_enh_draw_distance.py", "native",
          "draw distance and LOD bias extend what is drawn without changing "
          "which objects exist, proving both are render culls"),
    Check("a11y_race", "check_a11y_race.py", "native",
          "race announcements fire, coalesce, toggle per category, and leave "
          "the authoritative state stream byte-identical"),
    Check("input_hotplug", "check_input_hotplug.py", "native",
          "a pad joining or leaving mid-run reaches the right channel, leaves "
          "exact neutral on removal, and never binds one device to two ports"),
    Check("rom_checker_page", "check_rom_checker_page.py", "native",
          "the hosted ROM checker uploads nothing and reaches the same verdict "
          "as the native binary, character for character, on every ROM present"),
    Check("rom_text_indices", "check_rom_text_indices.py", "native",
          "no driven route resolves a GAME_TEXT index at or above 259, the "
          "count the 1.0 revisions carry",
          # Eleven-plus serial engine runs across every fixture group: 3600s
          # still died mid-enumeration after finishing nav + the full
          # 20-track sweep + hub (~32 engine runs). Measured shape is a
          # two-hour gate; budget it honestly.
          timeout=7200),
    Check("a11y_shell", "check_a11y_shell.py", "native",
          "every focusable control in the launcher, settings and overlay speaks "
          "its name and value, enumerated from the schema rather than a list"),
    Check("future_fun_land", "check_future_fun_land.py", "native",
          "a full trophy set plus the Wizpig 1 bit opens the lighthouse, and "
          "one trophy short does not"),
    Check("mod_music_override", "check_mod_music_override.py", "native",
          "a pack's music replaces the sequenced original, obeys the music "
          "volume, works from a zip, and moves no authoritative state"),
    Check("mod_texture_override", "check_mod_texture_override.py", "native",
          "a pack's textures replace the ROM's, and the Custom content setting "
          "switches them off and back on mid-run through the settings route, "
          "off being byte-identical to having no pack installed"),
    Check("enh_ai_difficulty", "check_enh_ai_difficulty.py", "native",
          "opponent skill leaves the authored arm bit-identical to a build "
          "without the enhancement compiled in, and the harder arms measurably "
          "beat it without wedging an opponent",
          # Configures and compiles its own -DMDKR_ENH_AI_DIFFICULTY_OMIT
          # purity baseline before racing twelve-plus arms: the same shape
          # the instrumented/layout roles get the building budget for.
          timeout=BUILDING_TASK_TIMEOUT),
    Check("tool_freecam", "check_tool_freecam.py", "native",
          "detaching and re-attaching the free camera leaves the authoritative "
          "state stream and the re-attached frame byte-identical"),
    Check("crash_screen", "check_crash_screen.py", "native",
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
    Check("dl_high_water", "check_dl_high_water.py", "release",
          "the display-list high-water witness reports a retail 1P race "
          "against the row its buffer was allocated to, and a length past "
          "that row aborts fail-closed"),
    Check("fast3d_dl_hardening", "check_fast3d_dl_hardening.py", "release",
          "an injected display-list overflow is walked to a stop by both the "
          "interpreter and the overlay prepass, with the well-authored party "
          "and retail 1P routes reporting nothing"),
    # The release arm proves the walkers stop and the route survives. It cannot
    # prove what the reads themselves were: a command fetched one past the end
    # of an 80-byte global, and a fault printer quoting the words of an address
    # the walk had just refused, both land in mapped memory in a release build
    # and report success. Measured, not assumed -- with the printer fix
    # reverted the release arm still passes and this lane aborts. This is the
    # detector.
    Check("fast3d_dl_hardening_asan", "check_fast3d_dl_hardening.py", "asan",
          "ASan witness for the misauthored display-list route -- the "
          "past-the-end command fetch and fault-printer read the walkers close",
          ("--injected-only",)),
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
    Check("camera_held_shot_faults", "check_camera_held_shot_faults.py", "release",
          "normal Modern held-shot projection/query fault recovery and safe publication"),
    Check("camera_held_shot_faults_asan", "check_camera_held_shot_faults.py", "asan",
          "sanitizer held-shot projection/query fault recovery in isolated saves"),
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
    # Needs a build of the previous published release (--reference-build);
    # without one it is SKIPPED, loudly, never passed. See SKIPPABLE_CHECKS.
    Check("previous_release_parity", "check_previous_release_parity.py", "release",
          "retail [SIMHASH] identity with the previous published release on AI, "
          "boss and challenge-arena routes (US and PAL), a legacy-RNG control, "
          "and that release's Time Trial record and ghost read back unchanged",
          # Measured 10m20s alone under the runner's background scheduling.
          timeout=3600),
    Check("sim_hash_artifact", "check_sim_hash_artifact.py", "release",
          "on-disk per-tick [SIMHASH] artifact mode mirrors the stdout stream "
          "byte-for-byte; the first-divergence comparator fails closed on "
          "divergence, truncation, empty, missing and unparseable inputs"),
    Check("presentation_rng_split", "check_presentation_rng_split.py", "release",
          "the authoritative RNG pair stays pinned across presentation-only "
          "frames while the presentation stream advances, with a "
          "render-impurity control and a run-to-run identity arm"),
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
    Check("rollback_weapon_refcount", "check_rollback_weapon_refcount.py",
          "native",
          "a corrected weapon fire conserves the pinned item-model refcount "
          "(snapshot-covered counts replay; uncovered counts freeze)"),
    Check("rollback_vehicle_matrix", "check_rollback_vehicle_matrix.py", "native",
          "every ROM-derived legal standard-track vehicle pairing"),
    Check("ci_contract", "check_ci_contract.py", "source",
          "push/PR native, sanitizer, wasm, save-custody, and ROM policy"),
    Check("widescreen_hud_scope", "check_widescreen_hud_scope.py", "source",
          "the opt-in widescreen HUD offset stays confined to the HUD: the "
          "mode-aware anchor table is total over enum HudTypes and matches "
          "its golden snapshot, every draw-side slide consumer goes through "
          "hud_slide_draw_offset() (#51), and the centered ortho is restored "
          "before dialogue boxes/pause menu/Taj subtitles draw (#50)"),
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
    Check("test_assertions_armed", "check_test_assertions_armed_wrapper.py",
          "source",
          "every tests/*.c and tests/*.cpp TU calling assert() is TU- or "
          "CMake-target-armed against NDEBUG, with unarmed and armed "
          "positive controls",
          ("--self-test",)),
    Check("net_roster_owner_guard", "check_net_roster_owner_guard.py", "source",
          "local-Play boot cannot inherit a foreign online roster; ownership is "
          "explicit and the guard is idempotent (compiles + runs standalone)"),
    Check("harness_isolation", "check_harness_isolation.py", "source",
          "every check that isolates MDKR_SAVE_DIR also pins "
          "MDKR_VIDEO_CONFIG_PATH, so a repo-root mdkr64.ini cannot reach "
          "a check's engine"),
    Check("party_cmake_origin_gate", "check_party_cmake_origin_gate.py",
          "source",
          "the configure-time MDKR_PARTY_ORIGIN validator rejects every "
          "non-canonical origin shape (path/query/userinfo/trailing slash)"),
    Check("lan_controller_assets", "check_lan_controller_assets.py", "source",
          "the local-play controller asset set agrees across the C++ manifest "
          "(lan_party_launch.cpp), the packaging list, and the tracked "
          "dist/web tree, so a packaged build cannot stage a broken page"),
    Check("player_prose", "check_player_prose.py", "source",
          "banned AI-slop vocabulary in launcher UI strings, README.md and "
          "RELEASE_NOTES.md"),
    Check("cadence_gating", "check_cadence_gating.py", "source",
          "no updateRate ==/!= 1/2 mode test outside "
          "platform_sim_cadence_is_enhanced()"),
    Check("adventure_party_boundaries", "check_adventure_party_boundaries.py",
          "source",
          "AP-01 boundary gate: every Adventure count/lead branch declared in "
          "the inventory, no party-path 2P-global write, online-authority leak, "
          "ambiguous party-count global, or adventure_party vocabulary leak into "
          "the test hook / Phone Party infrastructure"),
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
    Check("widescreen_hud_layers", "check_widescreen_hud_layers.py", "native",
          "opt-in widescreen HUD pixel layout (#51): TT rows share one right "
          "anchor, the race-start hold stays offscreen, identity label and "
          "battle strip stay centered, 4:3 byte-identical with the option on"),
    Check("split_screen_backdrop", "check_split_screen_backdrop.py", "native",
          "multiplayer sky backdrop coverage (#61): the >=2-viewport gradient "
          "quad spans the widescreen frustum with no unpainted sides, and 4:3 "
          "still derives the ROM's authored 200x150 extent"),
    Check("split_screen_pause_resolution", "check_split_screen_pause_resolution.py", "native",
          "P1/P2 pause fonts and output resolution (#61): real owner-colour, "
          "glyph-contour, scaled-UI and original-font controls on GL/WebGPU "
          "in Restored and Remastered, with identical simulation", timeout=1800),
    Check("split_screen_void_coverage", "check_split_screen_void_coverage.py", "native",
          "Walrus Cove P1/P2 hole coverage without obscuring real scenery (#61): "
          "authored, disabled and depth-write controls on GL/WebGPU; "
          "Pure byte identity and unchanged authoritative state", timeout=2400),
    Check("widescreen_minimap_alignment",
          "check_widescreen_minimap_alignment.py", "native",
          "minimap marker-on-map alignment under the widescreen HUD (#57): "
          "ortho sprite texels keep the safe pixel scale so the map image "
          "and its markers shift as one (hub island and race track map), "
          "4:3 byte-identical with the option on"),
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
    Check("cave_reverb_bus", "check_cave_reverb_bus.py", "native",
          "cave SFX reach the AL_FX_BIGROOM aux bus: a Treasure Caves autopilot "
          "drive with the per-voice reverb-routing trace, asserting two aux buses "
          "and fxmix>0 SFX voices on bus 1"),
    Check("music_bus_isolation", "check_music_bus_isolation.py", "native",
          "music (CSP) voices stay on the AL_FX_CUSTOM music bus and never leak "
          "onto the big-room SFX bus after recycled SFX voices moved pool slots "
          "there: a Treasure Caves autopilot drive with the per-voice routing "
          "trace, asserting every music note-on is on bus 0"),
    Check("resource_plateau", "check_resource_plateau.py", "native",
          "repeated GL/WebGPU stage, pool/audio/GPU/registry ownership plateau"),
    Check("input_script_capacity", "check_input_script_capacity.py", "native",
          "input-script capacity boundary accepts 512 and rejects 513 entries"),
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
          "replay, completion controls, and save reload",
          timeout=7200),
    Check("bonus_character_select", "check_bonus_character_select.py", "native",
          "contiguous 13-racer picker with independent Taj, Wizpig, and Terry "
          "actor/placard composition, pose states, and controller navigation"),
    Check("custom_character_roster", "check_custom_character_roster.py", "native",
          "isolated generated custom package, independent paginated browser, "
          "portrait composition, and real controller entry route"),
    Check("custom_character_identity_surfaces",
          "check_custom_character_identity_surfaces.py", "native",
          "generated package donor authority plus authored minimap colour and "
          "portrait pixels across a real race and post-race flow"),
    Check("custom_character_flag_portrait",
          "check_custom_character_flag_portrait.py", "native",
          "generated package portrait ownership and isolated WebGPU pixels on "
          "the real Fire Mountain collection-arena flag"),
    Check("character_raw_intake_ui", "check_character_raw_intake_ui.py", "native",
          "ROM-free review-first data-only adapter handoff, multi-draft GLB "
          "intake, same-source branching, source-bound "
          "mapping restore, accessible no-overwrite LOD-copy authoring, "
          "close/resume navigation, exact switch/delete/install "
          "cleanup, legacy migration, source-byte "
          "purity, corruption refusal, keyboard speech, and 200% compact "
          "Workshop rendering"),
    Check("character_quit_lifecycle_ui",
          "check_character_quit_lifecycle_ui.py", "native",
          "ROM-free visible/cancellable shutdown settlement, global result "
          "publication, Play/import exclusion, source purity, and private "
          "result-file cleanup"),
    Check("character_workshop_history_ui",
          "check_character_workshop_history_ui.py", "native",
          "ROM-free exact-source Identity/Profile/Rig/Fit/Performance/Test "
          "history routing without installed-byte mutation"),
    Check("character_draft_transfer_ui",
          "check_character_draft_transfer_ui.py", "native",
          "ROM-free exclusive path-private exact-source export, mutation-free "
          "accessible review, additive atomic import, duplicate idempotence, "
          "and wrong-source refusal"),
    Check("character_portrait_studio_ui",
          "check_character_portrait_studio_ui.py", "native",
          "ROM-free deterministic portrait style/pixel tools at 200% compact "
          "layout with keyboard speech and installed-byte purity"),
    Check("character_test_evidence_ui",
          "check_character_test_evidence_ui.py", "native",
          "ROM-free durable 4x4 exact-test matrix, qualified baseline lifecycle, "
          "device/source/fit/LOD binding, corruption refusal, 200% layout, and "
          "keyboard plus virtual-controller speech traversal"),
    Check("custom_character_workshop_preview",
          "check_custom_character_workshop_preview.py", "native",
          "script-free exact select/car/hovercraft/plane launches, semantic "
          "camera/light inspection, exclusive stabilized PNG capture, and real "
          "one-to-four-player WebGPU character stress with fail-closed inputs"),
    Check("bonus_results_portraits", "check_bonus_results_portraits.py", "native",
          "real post-race Wizpig/Terry portrait ownership, retail dimensions, "
          "and distinct card pixels"),
    Check("wizpig_race_voice", "check_wizpig_race_voice.py", "native",
          "playable Wizpig positive/negative race cues use his own authored "
          "voice IDs, with consecutive-repeat suppression"),
    Check("taj_selection_music", "check_taj_selection_music.py", "native",
          "Taj music confirmation, reselection, title exit, custom return, multiplayer "
          "ownership, Game/File Select round trip, Adventure Party back-out and muted "
          "slider retry/exit across four unlock layouts; the Taj theme may load and "
          "stay resident only on Character Select"),
    Check("sprite_models_gpu", "check_sprite_models_gpu.py", "native",
          "synthetic replacement model draws and eleven one-shot/persistent "
          "GPU allocation failure recovery paths"),
    Check("bonus_portrait_pack", "check_bonus_portrait_pack.py", "native",
          "a pack redraws the generated Taj/Wizpig/Terry cards at their "
          "published digests on GL and on the shipped WebGPU default, the "
          "author dump publishes those digests and records an installed "
          "override whole at its own size, and a pack.ini that switches the "
          "pack off restores the generated cards byte-for-byte"),
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
          "identity, sidecar persistence, and Time Trial quarantine",
          timeout=3600),
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
          ("--taj", "--frames", "5200"), timeout=7200),
    Check("adventure_party_admission", "check_adventure_party_admission.py",
          "native",
          "AP-06 Adventure Party menu admission: 2/3/4 players reach the ordinary "
          "Adventure route and form a session with the enhancement on; a party's "
          "UN-STARTED file confirm is fail-closed refused (R26) then the started "
          "file proceeds; copy/erase keep host-only file authority (FIX 1); 1P and "
          "the off arm are stock; retail 2P globals never engaged"),
    Check("adventure_party_hub", "check_adventure_party_hub.py", "native",
          "AP-08 Adventure Party hub roster: 2/3/4 humans spawn atomically with N "
          "viewports (3P minimap), per-seat input binding, and a per-viewport hub "
          "HUD; off routes to Tracks; flat-field and swapped-binding controls fire"),
    Check("adventure_party_transition", "check_adventure_party_transition.py",
          "native",
          "AP-09/10 shared lobby interactions: exactly one arbitrated whole-party "
          "door transition (single + conflicting, 2P/3P), any-seat balloon "
          "collect-once, non-host pause open with host resume authority, and "
          "disconnect-forced shared pause; two positive controls fire"),
    Check("adventure_party_race_loop", "check_adventure_party_race_loop.py",
          "native",
          "AP-12/R16 default party race loop: party crosses hub->lobby->race "
          "(R16 two-hop), a default race fields six racers (N humans + 6-N CPUs, "
          "N viewports, per-seat binding) at 2P/3P/4P, and a host/non-host/CPU "
          "win, a retry and a mid-race quit-to-lobby each return the same party "
          "to the lobby (sgen stable, lgen advanced); two positive controls fire"),
    Check("adventure_party_performance",
          "check_adventure_party_performance.py", "native",
          "AP-19 formal four-player budgets and deterministic plateau: twenty "
          "world-lobby->race->world-lobby cycles keep main-pool/audio/renderer/"
          "pointer-registry ownership flat, every four-camera display list below "
          "the checked-in 10750/11000-command threshold, four controller bindings "
          "and session generations exact; a ROM-free 20000-lifetime formation/"
          "suspension/dissolution churn retains no state; mutation controls fire",
          timeout=14400),
    Check("adventure_party_performance_soaks",
          "check_adventure_party_performance_soaks.py", "native",
          "AP-19 game-side ownership soaks: one versus five four-player Taj "
          "racer rebuilds reach an exact normalized hub ownership endpoint; "
          "five real host-solo boss defeat/restore lifetimes plateau main-pool, "
          "audio, renderer and pointer-registry ownership; display-list, exact "
          "roster/controller/session and mutation oracles remain mandatory",
          timeout=14400),
    Check("adventure_party_taj", "check_adventure_party_taj.py", "native",
          "AP-11 Taj transaction + shared-scene envelope: a non-host summons Taj "
          "(shared dialogue latched once), the host owns the vehicle choice, and the "
          "WHOLE party (2P/3P, car to hovercraft and plane) transforms with the "
          "same seat->character identities and split layout (live==N, never a collapse "
          "to 1); the dialogue releases to ACTIVE_LOBBY in the same generation (R10); "
          "a host CHALLENGE-row selection during the party dialogue is fail-closed "
          "refused (R27: no TAJ_MODE_RACE, no extra racer, transform still works "
          "after); per-seat visibility/input/sound restoration and six positive "
          "controls are checked, and retail 2P Taj "
          "(check_taj_p2_adventure) is untouched"),
    Check("adventure_party_progress", "check_adventure_party_progress.py",
          "native",
          "AP-13 exact-once campaign progression: a party default-race win at "
          "each winner seat (P1/P2/P3/P4, by stable racerIndex) persists a slot "
          "BYTE-IDENTICAL to a 1P win (empty whitelist), gated by one aparty_award "
          "token issue+consume and one RACE_CLEARED write; a CPU-first loss and a "
          "quit write nothing, a win + re-entry of the cleared course still writes "
          "exactly one clear, and the save round-trips into 1P Adventure; two "
          "positive controls fire (CPU-as-win, doubled-award)"),
    Check("adventure_party_challenges", "check_adventure_party_challenges.py",
          "native",
          "AP-15 host-solo special-challenge envelope: a party (3P + a 2P arm) "
          "suspends to a retail four-racer host-solo challenge (SOLO_ACTIVITY + "
          "suspended roster, [CHALLENGE] racers=4, no party race field), a host WIN "
          "commits one T.T. amulet piece exactly once (one aparty_award challenge "
          "token issue+consume, ttAmulet 0->1 CLEARED, byte-identical to a 1P win), "
          "a re-entry of the cleared challenge refuses a second token, and a defeat "
          "commits nothing; two positive controls fire (defeat-as-win, "
          "strip-suspension). The shared return-to-lobby RESTORE is proven by "
          "check_adventure_party_boss_restore (identical arrival-adapter code)"),
    Check("adventure_party_boss_restore", "check_adventure_party_boss_restore.py",
          "native",
          "AP-17 host-solo boss envelope: a 3P party suspends to a retail host-solo "
          "boss (SOLO_ACTIVITY + suspended roster, no party race field, host "
          "finishes), a first WIN commits the world boss bit exactly once (one "
          "aparty_award boss token issue+consume, save delta), a LOSS awards nothing, "
          "the beaten boss re-suspends on a rematch and refuses a new token, and "
          "EVERY return restores the exact party (aparty_restore match=1); two "
          "positive controls fire (one-racer, doubled-award)"),
    Check("adventure_party_silver", "check_adventure_party_silver.py",
          "native",
          "AP-14 team-shared silver coins: a party silver-coin race (3P central "
          "scene + a 2P arm) banks ONE team tally -- any human (incl. a non-host "
          "seat) collects a coin, it vanishes for ALL viewports (invis=0x600), the "
          "tally increments 1..8 once each, every viewport's HUD shows the same "
          "total, and EIGHT team coins + a human first awards "
          "RACE_CLEARED_SILVER_COINS exactly once via a SILVER completion token "
          "(the finish reading the TEAM total, not the leading racer's own count), "
          "persisting a slot BYTE-IDENTICAL to a 1P silver win; a CPU-first finish "
          "with eight team coins awards nothing, and the post-clear replay is no "
          "longer a silver race (no coins, no second award); two positive controls "
          "fire (seven-coins, stripped-collection). Adventure Two: NOT RUN "
          "(AP-16 owns the A2 matrix)",
          # Five full-race arms plus two replayed positive controls: the gate
          # finishes but can exceed the default 30-minute task ceiling. Wedge
          # bound only; the arms keep their own per-run frame budgets.
          timeout=3600),
    Check("adventure_party_trophy", "check_adventure_party_trophy.py", "native",
          "AP-16 Part A = SPLIT: a party enters the real Adventure trophy series "
          "(forced past the headless-unreachable cabinet via the R20/R24 retarget "
          "precedent), every round fields the retail EIGHT-racer split (total=8, N "
          "humans + 8-N CPUs, N viewports -- the measured 4P DL high-water 6354 fits "
          "the 11000 budget), all four production rounds run with accumulating "
          "standings, and a gold championship writes the Dino trophy exactly once via "
          "ONE COMPLETION_TROPHY token, persisting a save slot BYTE-IDENTICAL to a 1P "
          "gold (whole 40-byte slot, empty whitelist -- subsumes no-balloon/no-RACE_CLEARED "
          "write for the rounds); a 1P-ref awards the same gold with NO party token; the "
          "party returns to the same lobby; two positive controls fire "
          "(duplicate-consume, stripped-field)"),
    Check("adventure_party_adventure_two", "check_adventure_party_adventure_two.py",
          "native",
          "AP-16 Part C matrix: an Adventure-Two-flagged party fixture drives the "
          "SAME party policy with no new game-side branch -- (1) 3P admission -> hub "
          "on A2 (party forms, A2 save resumed), (2) a default race win is exact-once "
          "and byte-identical to a 1P A2 win of the same mirrored course, (3) a silver "
          "scene banks one team tally over the A2 coin object set (any human collect, "
          "invis=0x600, one SILVER award); adventure_mode adventureTwo=1 mirrored=1 "
          "throughout; two positive controls fire"),
    Check("adventure_party_campaign", "check_adventure_party_campaign.py",
          "native",
          "AP-18 release campaign manifest + real-door breadth: ROM-derived exact "
          "classification of all six lobbies, thirty-four save courses, legal "
          "vehicles, world transitions, door classes and campaign branches; a "
          "real silver door, real first-boss door with multi-hop restore and "
          "vehicle witness, and two distinct simultaneously conflicting race "
          "doors; challenge-key and "
          "trophy-cabinet headless geometry limits remain explicit rather than "
          "being mistaken for behavioural passes; mutation controls fire",
          timeout=3600),
    Check("adventure_party_custom_characters",
          "check_adventure_party_custom_characters.py", "native",
          "Adventure Party/custom-character composites: a 3P donor-only party "
          "roster retains three presentation packages, Taj transform and a "
          "host-solo boss restore preserve every package, and a custom-equipped "
          "party win writes byte-identical progress to the same plain party win; "
          "two positive controls fire",
          timeout=3600),
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
    Check("save_options_scroll_band", "check_save_options_scroll_band.py", "native",
          "inverted rectangles on the Save Options pak-switch scroll draw "
          "nothing, as on hardware (issue #52)"),
    Check("track_preview_time_steady", "check_track_preview_time_steady.py", "native",
          "best time/lap digit sprites hold still while the track-preview "
          "flyby banks; the ortho sprite roll comes from the base viewport "
          "camera, not the cutscene bank (issue #59)"),
    Check("postrace_door_fling", "check_postrace_door_fling.py", "native",
          "post-race lobby returns stay grounded on the quit paths, with the "
          "rising-door carry-frame legacy control"),
    Check("track_exit_storage", "check_track_exit_storage.py", "native",
          "Hot Top Volcano's destination -1 exit reloads the central hub "
          "through the measured retail us.v80 path with trophy storage "
          "intact, instead of the menu-8 dead-end hang (issue #55)"),
    Check("trophy_series", "check_trophy_series.py", "native",
          "all five Adventure trophy championships, quit/retry, and Dino EEPROM reload"),
    Check("trophy_adventure_two", "check_trophy_adventure_two.py", "native",
          "Adventure Two Future Fun Land mirrored championship, gold award, "
          "cabinet display and persisted save identity"),
    Check("trophy_ffl_reload", "check_trophy_ffl_reload.py", "native",
          "Future Fun Land gold earned and displayed from unchanged EEPROM "
          "in fresh processes for both Adventure modes"),
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
          # 31 minutes on a quiet machine; the margin is a runtime target and
          # the individual writes retain their own 420s guard.
          timeout=3600),
    Check("boost_magnitude", "check_boost_magnitude.py", "native",
          "zip-pad boost per-frame speed trace, racer-count independence, and "
          "perturbed-boost-constant positive controls"),
    Check("save_failsafe", "check_save_failsafe.py", "native",
          "EEPROM recovery and persistence"),
    Check("save_write_notice", "check_save_write_notice.py", "native",
          "a failed durable save surfaces one player notice instead of losing "
          "progress silently (issue #54 Run B)"),
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
    Check("online_wire_schema_parity",
          "check_online_wire_schema_parity.py", "source",
          "browser Online Room key sets against the Worker match wire schema, "
          "its command, close, error and signaling vocabularies and the "
          "/api/ops/health reservation buckets, with the R36 v2-wire gap "
          "allowlisted member by member"),
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
    # The online crown gate: wrangler dev (real MatchRoom Durable Object) +
    # two real native driver processes race 1800 ticks over real WebRTC and
    # must agree on the confirmed state hash. Needs wrangler + serialized
    # ports, so it rides the browser_local lane like party_native_e2e.
    Check("online_live_transport_e2e", "check_online_live_transport_e2e.py",
          "browser_local",
          "two live native processes: create/join over the real Worker, "
          "hello/offer/ice over the real relay, phrase + preflight consensus, "
          "an 1800-tick race converging to one state hash, and a mid-frame "
          "/connect stall torn down promptly"),
    # The no-internet crown gate: the same driver under --lan drives the
    # embedded server + in-process room (NO wrangler) against a real headless
    # controller over plain http, so it drives Chromium and belongs to the
    # serialized browser_local lane exactly like party_native_e2e.
    Check("party_lan_e2e", "check_party_lan_e2e.py", "browser_local",
          "native host + embedded server/room + real browser with no cloud in "
          "the loop: capability golden path with the page's pure-JS SAS phrase "
          "matching the native mbedtls phrase, wrong-then-right code redeem, a "
          "13-code throttle trip, and host_closed on stop"),
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
    "check_camera_envelope_control.py",  # native lens CTest supplies its binary
    # Parser imported/exercised by test_camera_chain_evidence.py. Its CLI
    # accepts an operator's private trace, not standard runner build arguments.
    "check_camera_chain_evidence.py",
    "check_sprite_models.py",  # synthetic packs consumed by sprite_models CTest
    "check_ai_difficulty_ui.py",
    "check_match_transport_tls_io.py",
    "check_online_resolver_budget.py",
    "check_party_open_transaction.py",
    "check_network_lifetime.py",
    "check_match_signal_admission.py",
    # Cross-layer source contract registered as character_offset_studio_contract
    # in cmake/tests.cmake; the broad ctest task owns its artifact-free run.
    "check_character_offset_studio.py",
    "check_multiplayer_boundaries.py",
    # Registered as the net_failure_ring_impairment CTest (CMakeLists.txt, in
    # the MDKR_NATIVE_PHONE_PARTY block), which passes it both arms of the
    # impairment harness; it launches no game and needs no ROM, so the ctest
    # task owns it.
    "check_net_failure_ring_impairment.py",
    # Registered as the gamecontrollerdb_lint CTest (cmake/tests.cmake); a
    # plain source lint with no artifacts, run once by the ctest task.
    "check_gamecontrollerdb.py",
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
    # Release workflows build a platform-specific frozen importer, smoke it
    # before packaging, then smoke the staged copy again. The ordinary runner
    # has no frozen-importer artifact role and cannot substitute the game
    # binary for this executable-shaped contract.
    "check_frozen_character_importer.py",
}

# The cross-architecture determinism gate. It is registered as owned/known
# rather than as a CHECKS entry because this runner CANNOT manufacture what it
# needs: a second executable of a different CPU architecture, built from the
# same source with the same configuration. On macOS that means an x86_64 tree
# alongside the native arm64 one, and an x86_64 SDL2 built from source because
# Apple Silicon Homebrew ships arm64 only. Putting it in CHECKS would make every
# suite run on a host without that second tree red for a missing build
# directory rather than for a defect.
#
# It is NOT optional, and it is not a subset the release can quietly skip. The
# online compatibility identity does not carry the architecture
# (platform/online/compatibility_identity.h), so cross-architecture sessions are
# admitted today and no other gate in this tree has ever measured one. The
# operator runs it directly against the two build directories:
#
#   tests/check_crossarch_determinism.py --build build-rel \
#       --build-x86 build-rel-x86_64 --rom baserom.us.v80.z64
#
# See docs/architecture/cross-architecture-determinism.md for how to produce the
# second tree and for what the gate does and does not prove.
CROSSARCH_OPERATOR_SCRIPTS = {
    "check_crossarch_determinism.py",
}

# The native online-takeover (Golden Balloon beta) regression lanes. Every one
# boots the REAL engine off the ROM and is TIMING-SENSITIVE (wall-clock
# watchdogs, live loopback-transport convergence, countdown dwells that only
# behave with the machine to themselves), so they must NEVER enter this parallel
# runner's pool. They are OWNED by tools/run_online_checks.py -- the serial
# aggregating exit gate that runs each lane strictly one at a time -- and several
# are also runnable standalone. Registered here as owned/known so run_checks.py
# --list and check_ci_contract accept the tree WITHOUT the runner ever executing
# a flaky online lane. (online_profile_rematch ships as a real CHECKS entry
# above, and online_live_transport_e2e as the browser_local capstone, so those
# two are deliberately absent from this set.)
#
# check_online_process_convergence.py lives HERE, not in CHECKS, because its
# verdict is a property of MDKR_ENABLE_ONLINE_BETA builds only: the
# zero-viewport endpoint's determinism depends on the hud_audio_update() call
# in game/src/thread3_main.c that is compiled only under the beta gate (kept
# out of OFF builds so the isolation anchors stay byte-identical). Handing the
# lane this runner's beta-OFF --build (build-rel) reproduces the pre-fix
# row-2801 RNG fork by construction — a red about the invocation shape, not
# the tree. The default tools/run_online_checks.py sweep runs the US arm
# against build-beta, and --pal-rom joins the pal/cross arms.
ONLINE_TAKEOVER_SCRIPTS = {
    "check_online_beta_handoff.py",
    "check_online_camera_capture_continuity.py",
    "check_online_camera_lens.py",
    "check_online_ceremony.py",
    "check_online_character_map.py",
    "check_online_charselect.py",
    "check_online_engine_boot.py",
    "check_online_engine_boot_direct.py",
    "check_online_final_replay.py",
    "check_online_isolation_selftest.py",
    "check_online_joiner_terminal.py",
    "check_online_left_reentry.py",
    "check_online_lobby_drop.py",
    "check_online_lobby_single_endpoint.py",
    "check_online_lobby_start.py",
    "check_online_lobby_takeover.py",
    "check_online_lobby_tournament.py",
    "check_online_lobby_unconfigured.py",
    "check_online_midrace_transport_loss.py",
    "check_online_partition_integrity.py",
    "check_online_pause_overlay.py",
    "check_online_peer_loss.py",
    "check_online_process_convergence.py",
    "check_online_race_start_peer_loss.py",
    "check_online_racer_lod_animation.py",
    "check_online_rearm_third.py",
    "check_online_region_reentry.py",
    "check_online_resident_live.py",
    "check_online_results_chooser.py",
    "check_online_room_ready_rearm.py",
    "check_online_session_boot.py",
    "check_online_session_end.py",
    "check_online_session_results.py",
    "check_online_single_race_replay.py",
    "check_online_tournament.py",
    "check_online_tournament_cup_vehicle.py",
    "check_online_trackselect.py",
    "check_online_vehicleselect.py",
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
                  CTEST_COMPANION_SCRIPTS | WORKFLOW_COMPANION_SCRIPTS |
                  ONLINE_TAKEOVER_SCRIPTS | CROSSARCH_OPERATOR_SCRIPTS)
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


def subset_label(args: argparse.Namespace, count: int,
                 skipped: "list[Check] | tuple[Check, ...]" = ()) -> str:
    """Name every restriction applied to the manifest, for the verdict line.

    A run in which any task was SKIPPED is not a complete suite even when
    nothing restricted the selection: it says how many tasks were checked and
    how many were skipped instead.
    """
    skip_note = f", {len(skipped)} SKIPPED" if skipped else ""
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
    if not restrictions and not skipped:
        return f"complete suite, {count}/{len(CHECKS)} tasks"
    if not restrictions:
        return (f"INCOMPLETE {count - len(skipped)}/{len(CHECKS)} tasks checked"
                f"{skip_note}")
    return (
        f"SUBSET {count}/{len(CHECKS)} tasks{skip_note}: "
        + " ".join(restrictions)
    )


def final_verdict(
    results: "list[tuple[Check, int, float]]",
    selected: int,
    args: argparse.Namespace,
    total: float,
) -> tuple[int, list[tuple[str, bool]]]:
    """The run's exit status and closing lines as (text, to_stderr).

    Skips are named before any verdict. In a release run a skip is a failure
    that names the input the operator must supply; otherwise the run may pass,
    but only as PASS WITH SKIPS and never as a complete suite.
    """
    skipped = [check for check, rc, _elapsed in results if was_skipped(check, rc)]
    failures = [(check, rc) for check, rc, _elapsed in results
                if rc != 0 and not was_skipped(check, rc)]
    scope = subset_label(args, selected, skipped)
    lines: list[tuple[str, bool]] = []
    for check in skipped:
        lines.append((f"run_checks: SKIPPED — {check.name}: "
                      f"{SKIPPABLE_CHECKS[check.name].reason}; it compared "
                      "nothing and is not a pass", True))
    if release_mode(args):
        for check in skipped:
            lines.append((
                f"run_checks: FAIL — {check.name} was SKIPPED in a release run "
                "(--require-fresh/--require-shipping-sdl); supply "
                f"{SKIPPABLE_CHECKS[check.name].missing_input}", True))
        failed = len(failures) + len(skipped)
    else:
        failed = len(failures)
    if failed:
        lines.append((f"run_checks: FAIL — {failed}/{len(results)} task(s) failed "
                      f"in {format_duration(total)} [{scope}]", True))
        return 1, lines
    if len(results) != selected:
        lines.append((f"run_checks: FAIL — ran {len(results)}/{selected} selected "
                      f"tasks [{scope}]", True))
        return 1, lines
    if skipped:
        lines.append((
            f"run_checks: PASS WITH SKIPS — {len(results) - len(skipped)}/"
            f"{len(results)} tasks passed, {len(skipped)} SKIPPED "
            f"({', '.join(check.name for check in skipped)}) in "
            f"{format_duration(total)} [{scope}]", False))
        return 0, lines
    lines.append((f"run_checks: PASS — all {len(results)} tasks passed in "
                  f"{format_duration(total)} [{scope}]", False))
    return 0, lines


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

# These browser-local gates also consume a native driver. Route the selected
# build explicitly; their standalone default must not silently select another
# checkout/build, and missing drivers must fail preflight before a long suite.
NATIVE_BROWSER_DRIVERS = {
    "party_native_e2e": "mdkr_native_party_e2e_driver",
    "party_lan_e2e": "mdkr_native_party_e2e_driver",
    "online_live_transport_e2e": "mdkr_online_live_transport_e2e_driver",
}

# These browser-local gates consume the selected game executable itself, not a
# sibling driver. Keep file routing separate so custom executable names and
# Windows suffixes are retained exactly as selected by the caller.
NATIVE_BROWSER_BINARIES = {
    "browser_online_room_gallery": "native gallery binary",
}

# These checks launch the local Party Worker, directly or through the shared
# startup helper. Admit its lockfile-installed entrypoint before any suite
# subprocess or staged-artifact inspection. The LAN-only route deliberately
# does not depend on Wrangler. Never fetch a replacement tool from the network.
PARTY_WORKER_CHECKS = frozenset({
    "party_capacity",
    "party_experience_canary_smoke",
    "party_service_chaos",
    "party_native_e2e",
    "online_live_transport_e2e",
    "browser_online_two_person",
    "party_firewall_negative",
})
PARTY_WRANGLER = ROOT / "services" / "party" / "node_modules" / "wrangler" / "bin" / "wrangler.js"


def native_browser_driver(check: Check, native: Path) -> Path | None:
    name = NATIVE_BROWSER_DRIVERS.get(check.name)
    if name is None:
        return None
    suffix = ".exe" if os.name == "nt" else ""
    return native.parent / (name + suffix)


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
    reference_build: Path | None = None,
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
        if check.name in NATIVE_BROWSER_BINARIES:
            cmd += ["--build", str(native),
                    "--shell-dir", str(ROOT / "dist" / "web")]
        elif native_browser_driver(check, native) is not None:
            cmd += ["--build", str(native.parent),
                    "--shell-dir", str(ROOT / "dist" / "web")]
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
    # previous_release_parity compares both regions against the previous
    # release for the same reason.
    if (check.name in {"rom_revision", "rom_checker_page", "simulation_cadence",
                       "arbitrary_presentation_rates", "presentation_breadth",
                       "framed_world_views", "previous_release_parity"}
            or check.script == "check_taj_character_select.py"):
        cmd += ["--roms", str(roms)]
    if check.name in REFERENCE_BUILD_CHECKS and reference_build is not None:
        cmd += ["--reference-build", str(reference_build)]
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


# --- SDL flavor witness --------------------------------------------------------
# The shipped macOS DMG bundles the pinned upstream SDL2 (macos/README.md);
# Homebrew's `sdl2` alias resolves to sdl2-compat, a shim over SDL3 with
# different unfocused-joystick semantics. The 1.6.0 qualification runs drifted
# between the two flavors without anyone noticing, and a lane went green under
# the shim while red under the shipping library. Every suite run now names the
# flavor the binary under test actually linked, and a release qualification can
# refuse the shim outright with --require-shipping-sdl.
SDL_FLAVOR_SHIM = "sdl2-compat"
SDL_FLAVOR_UPSTREAM = "sdl2"
SDL_FLAVOR_STATIC = "static-or-unlinked"


def classify_sdl_link(link_lines: str) -> tuple[str, str]:
    """Classify the SDL the binary links from `otool -L` / `ldd` output.

    Returns (flavor, detail). Pure so the contract test can feed it synthetic
    loader output.
    """
    for raw in link_lines.splitlines():
        line = raw.strip()
        if "SDL2" not in line and "libSDL2" not in line:
            continue
        path = line.split(" (", 1)[0].split(" => ", 1)[-1].strip()
        if "sdl2-compat" in path.lower():
            return SDL_FLAVOR_SHIM, path
        version = re.search(r"current version ([0-9.]+)", line)
        detail = path if version is None else f"{path} (current version {version.group(1)})"
        return SDL_FLAVOR_UPSTREAM, detail
    return SDL_FLAVOR_STATIC, ""


def sdl_flavor_for_binary(binary: Path) -> tuple[str, str]:
    if sys.platform == "darwin":
        command = ["otool", "-L", str(binary)]
    elif sys.platform.startswith("linux"):
        command = ["ldd", str(binary)]
    else:
        return SDL_FLAVOR_STATIC, "no loader introspection on this platform"
    try:
        completed = subprocess.run(
            command, check=True, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except (OSError, subprocess.CalledProcessError) as error:
        return SDL_FLAVOR_STATIC, f"loader introspection failed: {error}"
    flavor, detail = classify_sdl_link(completed.stdout)
    if flavor == SDL_FLAVOR_UPSTREAM and sys.platform == "darwin":
        # The dylib name alone cannot tell a renamed shim from upstream; the
        # shim stamps its own name into the binary, as build_app_bundle.sh
        # also checks.
        try:
            with open(detail.split(" (", 1)[0], "rb") as handle:
                if b"sdl2-compat:" in handle.read():
                    return SDL_FLAVOR_SHIM, detail
        except OSError:
            pass
    return flavor, detail


def report_sdl_flavor(native: Path, require_shipping: bool) -> None:
    flavor, detail = sdl_flavor_for_binary(native)
    print(f"run_checks: sdl flavor: {flavor} {detail}".rstrip(), file=sys.stderr)
    if require_shipping and flavor == SDL_FLAVOR_SHIM:
        raise RuntimeError(
            "the native binary links sdl2-compat, not the shipping SDL2; "
            "configure with PKG_CONFIG_PATH pointing at the pinned SDL2 "
            "(macos/Scripts/build_release_sdl2.sh) or drop --require-shipping-sdl")


def preflight(
    checks: list[Check],
    native: Path,
    release: Path,
    asan: Path,
    rom: Path,
    wasm: Path,
    require_shipping_sdl: bool = False,
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
    for check in checks:
        if check.name in NATIVE_BROWSER_BINARIES:
            required.append((f"{check.name} {NATIVE_BROWSER_BINARIES[check.name]}",
                             native))
        driver = native_browser_driver(check, native)
        if driver is not None:
            required.append((f"{check.name} native driver", driver))
    worker_checks = sorted({check.name for check in checks} & PARTY_WORKER_CHECKS)
    if worker_checks:
        required.append(("lockfile-pinned Party Wrangler (" + ", ".join(worker_checks) + ")",
                         PARTY_WRANGLER))
    missing = [f"{label}: {path}" for label, path in required if not path.is_file()]
    if missing:
        guidance = ("\nInstall the local Party dependencies from "
                    "services/party/package-lock.json with npm ci in services/party; "
                    "preflight does not install dependencies."
                    if worker_checks and not PARTY_WRANGLER.is_file() else "")
        raise RuntimeError("missing required artifact(s):\n  " + "\n  ".join(missing) + guidance)

    if roles & {"native", "release", "ctest"} and native.is_file():
        report_sdl_flavor(native, require_shipping_sdl)

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
        help="ROM-revision directory (US and PAL v80). Optional for a subset "
             "run; a release run needs it, and twelve tasks fail without it",
    )
    parser.add_argument("--wasm", default="build-web/mdkr64_web.wasm")
    parser.add_argument(
        "--reference-build",
        default=None,
        help="a build of the previous published release, configured like "
             "--release-build, for previous_release_parity; without it that "
             "task is SKIPPED (named in the verdict), never passed, and a "
             "release run (--require-fresh/--require-shipping-sdl) FAILS",
    )
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
        "--require-shipping-sdl", action="store_true",
        help="fail preflight when the native binary links sdl2-compat instead "
             "of the pinned upstream SDL2 the releases bundle")
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

    # The maintainer's standing local authorization can satisfy the execution
    # guard below. Hidden-window hints remain defense in depth; they do not
    # themselves establish authorization or guarantee focus isolation.

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
    # Enumerate before the execution guard below. --list is a
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

    if os.environ.get("MDKR_DEDICATED_TEST_DESKTOP") != "1":
        print("run_checks: REFUSED — execution requires a human-attested "
              "dedicated test desktop (MDKR_DEDICATED_TEST_DESKTOP=1); "
              "use --list for non-executing inventory", file=sys.stderr)
        return 2

    native = resolve_binary(args.build)
    release = resolve_binary(args.release_build)
    asan = resolve_binary(args.asan_build)
    rom = resolve_path(args.rom)
    roms = resolve_path(args.roms)
    wasm = resolve_path(args.wasm)
    reference_build = (resolve_path(args.reference_build)
                       if args.reference_build else None)
    if release_mode(args) and reference_build is None:
        for check in checks:
            if check.name in REFERENCE_BUILD_CHECKS:
                print(f"run_checks: RELEASE RUN WITHOUT --reference-build — "
                      f"{check.name} will be SKIPPED and this run will FAIL",
                      file=sys.stderr, flush=True)
    try:
        preflight(checks, native, release, asan, rom, wasm,
                  require_shipping_sdl=args.require_shipping_sdl)
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
            args.no_rebuild_instrumented, reference_build,
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
        elif was_skipped(check, returncode):
            label = f"SKIPPED ({SKIPPABLE_CHECKS[check.name].reason})"
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
            args.no_rebuild_instrumented, reference_build,
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
                if counts_as_failure(check, returncode, args) and args.fail_fast:
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
                        f"SKIPPED ({SKIPPABLE_CHECKS[check.name].reason})"
                        if was_skipped(check, returncode)
                        else "FAIL (timeout)" if returncode == TIMEOUT_EXIT
                        else f"FAIL (exit {returncode})")
                    print(f"[{done}/{len(checks)}] {check.name}: {label} in "
                          f"{format_duration(elapsed)}", flush=True)
                    if counts_as_failure(check, returncode, args):
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
                    if counts_as_failure(check, returncode, args) and args.fail_fast:
                        break
            # Manifest order keeps the summary comparable across runs.
            order = {c.name: i for i, c in enumerate(checks)}
            results.sort(key=lambda row: order[row[0].name])
    except KeyboardInterrupt:
        print("\nrun_checks: interrupted", file=sys.stderr)
        return 130

    print("\nrun_checks: summary", flush=True)
    for check, returncode, elapsed in results:
        if returncode == 0:
            label = "PASS"
        elif was_skipped(check, returncode):
            label = "SKIPPED"
        elif returncode == TIMEOUT_EXIT:
            label = "FAIL timeout"
        else:
            label = f"FAIL exit={returncode}"
        print(f"  {check.name:24s} {label:14s} {format_duration(elapsed):>7s}")
    total = time.monotonic() - suite_start
    # A subset can never read as a completed suite: the verdict line names the
    # restriction that produced it and how much of the manifest it covered.
    # Neither can a run that skipped a task (final_verdict).
    code, lines = final_verdict(results, len(checks), args, total)
    for text, to_stderr in lines:
        print(text, file=sys.stderr if to_stderr else sys.stdout, flush=True)
    return code

if __name__ == "__main__":
    sys.exit(main())
