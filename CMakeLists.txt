cmake_minimum_required(VERSION 3.16)
project(mdkr64 C)
# Editors and clangd read build-rel/compile_commands.json (see .clangd). Without
# it every include resolves against the compiler's bare default search path and
# the editor reports missing headers that the real build finds.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(CTest)
option(MDKR_BUILD_FUZZERS "Build coverage-guided save-format fuzzers" OFF)

# The launcher smoke tests share one physical graphics device and their isolated
# files do not make concurrent GPU context creation safe.  Apply one named CTest
# lock after all native tests have registered so adding another gpu-labelled test
# cannot accidentally reintroduce the parallel race.
function(mdkr_serialize_gpu_ctests)
    get_property(MDKR_CTEST_NAMES DIRECTORY PROPERTY TESTS)
    foreach(MDKR_CTEST_NAME IN LISTS MDKR_CTEST_NAMES)
        get_property(MDKR_CTEST_LABELS TEST "${MDKR_CTEST_NAME}" PROPERTY LABELS)
        list(FIND MDKR_CTEST_LABELS "gpu" MDKR_GPU_LABEL_INDEX)
        if(NOT MDKR_GPU_LABEL_INDEX EQUAL -1)
            set_tests_properties("${MDKR_CTEST_NAME}" PROPERTIES
                RESOURCE_LOCK mdkr_native_gpu)
        endif()
    endforeach()
endfunction()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    # Native defaults to Debug (fast iteration); the web build defaults to
    # Release below (an unoptimized Asyncify wasm is enormous and slow).
    if(EMSCRIPTEN)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    else()
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    endif()
endif()

if(MDKR_BUILD_FUZZERS AND NOT EMSCRIPTEN)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MDKR_BUILD_FUZZERS requires Clang/libFuzzer")
    endif()
    add_executable(mdkr_save_formats_fuzzer
        ${CMAKE_SOURCE_DIR}/tests/fuzz_save_formats.c
        ${CMAKE_SOURCE_DIR}/platform/save_codec.c
        ${CMAKE_SOURCE_DIR}/platform/save_container.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_save_formats_fuzzer PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_options(mdkr_save_formats_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer)
    target_link_options(mdkr_save_formats_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined)
endif()

# --- Dependencies ---------------------------------------------------------
# Native links system SDL2 + OpenGL. Under Emscripten neither exists as a
# package: SDL2 comes from the emcc port (-sUSE_SDL=2) and there is no GL
# backend in the wasm binary (WebGPU only), so skip the finders entirely.
if(NOT EMSCRIPTEN)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    if(APPLE)
        find_library(OPENGL_LIB OpenGL REQUIRED)
    else()
        find_package(OpenGL REQUIRED)
    endif()
endif()

# --- Test wiring -----------------------------------------------------------
# ROM-free unit tests, the SDL queue-mode contract test, and the CTest-wrapped
# Python contract checks -- extracted to keep the build definition below from
# being buried under a thousand lines of test registration. See that file's
# header for why it is included at this exact point.
include(${CMAKE_SOURCE_DIR}/cmake/tests.cmake)

if(NOT EMSCRIPTEN)
    add_executable(mdkr-save
        ${CMAKE_SOURCE_DIR}/tools/mdkr_save_cli.c
        ${CMAKE_SOURCE_DIR}/platform/save_codec.c
        ${CMAKE_SOURCE_DIR}/platform/save_container.c
        ${CMAKE_SOURCE_DIR}/platform/save_tools_core.c
        ${CMAKE_SOURCE_DIR}/platform/virtual_pak.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr-save PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_options(mdkr-save PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    if(BUILD_TESTING)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(
            NAME save_cli
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/test_save_cli.py
                    --cli $<TARGET_FILE:mdkr-save>)
    endif()
else()
    # ROM- and renderer-free save tooling. The launcher loads this tiny module
    # before any ROM is selected so backup, import, recovery, and editing remain
    # available when WebGPU or the game engine cannot start.
    add_executable(mdkr_save_tools
        ${CMAKE_SOURCE_DIR}/platform/save_tools_wasm.c
        ${CMAKE_SOURCE_DIR}/platform/save_codec.c
        ${CMAKE_SOURCE_DIR}/platform/save_container.c
        ${CMAKE_SOURCE_DIR}/platform/save_tools_core.c
        ${CMAKE_SOURCE_DIR}/platform/virtual_pak.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_save_tools PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_options(mdkr_save_tools PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_options(mdkr_save_tools PRIVATE
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createMDKRSaveTools"
        "-sINVOKE_RUN=0"
        "-sEXIT_RUNTIME=0"
        "-sEXPORTED_FUNCTIONS=_malloc,_free,_mdkr_save_tools_validate_pak,_mdkr_save_tools_load,_mdkr_save_tools_blank,_mdkr_save_tools_input_format,_mdkr_save_tools_metadata,_mdkr_save_tools_summary_size,_mdkr_save_tools_summary,_mdkr_save_tools_copy_raw,_mdkr_save_tools_container_size,_mdkr_save_tools_export_container,_mdkr_save_tools_corrupt_mask,_mdkr_save_tools_diff_count,_mdkr_save_tools_accept_baseline,_mdkr_save_tools_recover,_mdkr_save_tools_copy_blocks,_mdkr_save_tools_slot_state,_mdkr_save_tools_slot_name,_mdkr_save_tools_slot_field,_mdkr_save_tools_course,_mdkr_save_tools_balloon,_mdkr_save_tools_world_flags,_mdkr_save_tools_config,_mdkr_save_tools_reset_records"
        "-sEXPORTED_RUNTIME_METHODS=cwrap,FS,IDBFS,HEAPU8"
        "-lidbfs.js"
        "-sENVIRONMENT=web")
    set_target_properties(mdkr_save_tools PROPERTIES
        OUTPUT_NAME "mdkr-save-tools"
        SUFFIX ".js")
endif()

# --- WebGPU backend (M4.5 / M8) -------------------------------------------
# WebGPU (wgpu-native) is the qualified native default and the ONLY in-browser
# backend (M8); GL stays compiled as an explicit native diagnostic path.
# Under Emscripten the code is ALWAYS on (there is no GL fallback in the
# browser) and cmake/webgpu.cmake returns an INTERFACE `webgpu` target wired to
# `--use-port=emdawnwebgpu` (Dawn's webgpu.h — the compatibility seam lets this
# repository's gfx_webgpu.c compile for both APIs). Set -DMDKR_WEBGPU_BACKEND=OFF for a
# native GL-only binary with no wgpu dep. Runtime selection: MDKR_RENDERER.
option(MDKR_WEBGPU_BACKEND "Build the default native WebGPU render backend" ON)
if(EMSCRIPTEN)
    set(MDKR_WEBGPU_BACKEND ON CACHE BOOL "" FORCE)   # web = WebGPU only
endif()
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(cmake/webgpu.cmake)

# --- Native app shell (ImGui launcher + in-game overlay) -------------------
# The portable in-process shell: first-run ROM picker, schema-driven settings,
# F1 in-game overlay, diagnostics. Native ONLY — the browser build keeps its own
# JS shell (dist/web/index.html), which already provides the launcher, ROM
# picker and settings surface there, so compiling a second one into the wasm
# would be dead weight and a second source of truth.
option(MDKR_APP "Build the native ImGui app shell (launcher + in-game overlay)" ON)
if(EMSCRIPTEN)
    set(MDKR_APP OFF CACHE BOOL "" FORCE)
endif()
if(MDKR_APP)
    # The engine is `project(mdkr64 C)` and stays C-only; ImGui and the shell are
    # the only C++ in the tree. Enabling CXX here rather than in project() keeps
    # -DMDKR_APP=OFF and the Emscripten build free of a C++ toolchain
    # requirement they have no other reason to have.
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# --- Sources --------------------------------------------------------------
# Game logic (decompiled C). hasm/ supplies the NON_MATCHING C bodies for
# math_util.c + collision.c; hasm_native/ contains the native C replacements
# for hand-written assembly. The .s files and the hasm/ido/ alternate sources
# are intentionally NOT globbed.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/game/src/*.c
    ${CMAKE_SOURCE_DIR}/game/src/hasm/*.c
    ${CMAKE_SOURCE_DIR}/game/src/hasm_native/*.c)

# Native platform layer (this project's own M1 code). Listed explicitly rather
# than globbed, so a vendored mgb64 TU dropped into platform/ is not dragged into
# the build until the milestone that actually wires it up.
#
# The GoldenEye-only TUs that used to sit here uncompiled (config_pc.c,
# port_trace.c, frame_clamp.c, audio_ring.c) were removed at the v0.1 hygiene
# pass: each needed headers this repo does not have (settings.h, savedir.h,
# bondconstants.h, boss.h, game/bondview.h, game/gun.h) or keyed off a
# GoldenEye-only concept (speedgraphframes), so none of them could ever have
# compiled here. mgb64 remains their home; recover from git history if a
# milestone genuinely needs one.
set(PLATFORM_SOURCES
    ${CMAKE_SOURCE_DIR}/platform/main_pc.c
    ${CMAKE_SOURCE_DIR}/platform/display_config.c
    ${CMAKE_SOURCE_DIR}/platform/camera_obstruction.c # pure native camera sweep kernel
    ${CMAKE_SOURCE_DIR}/platform/camera_obstruction_resolver.c # fixed-step spring-arm policy
    ${CMAKE_SOURCE_DIR}/platform/camera_obstruction_transform.c # pure object-local camera sweep adapter
    ${CMAKE_SOURCE_DIR}/platform/camera_obstruction_query.c # pure track/object sweep composition
    ${CMAKE_SOURCE_DIR}/platform/camera_target_visibility.c # target readability classification
    ${CMAKE_SOURCE_DIR}/platform/config_ini.c       # INI engine (game-agnostic)
    ${CMAKE_SOURCE_DIR}/platform/taj_mod_state.c    # versioned global Taj unlock sidecar
    ${CMAKE_SOURCE_DIR}/platform/taj_mod_state_file.c # atomic desktop/Web state backend
    ${CMAKE_SOURCE_DIR}/platform/video_config.c     # presentation modes (pure core)
    ${CMAKE_SOURCE_DIR}/platform/video_config_runtime.c  # ini/env/argv + g_pc* publication
    ${CMAKE_SOURCE_DIR}/platform/user_paths.c       # immutable bundle vs mutable user-data policy
    ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c          # UTF-8/long-path OS filesystem boundary
    ${CMAKE_SOURCE_DIR}/platform/pacing_policy.c    # original/enhanced simulation cadence
    ${CMAKE_SOURCE_DIR}/platform/sim_sched.c        # exact fixed-authority accumulator
    ${CMAKE_SOURCE_DIR}/platform/host_frame_driver.c # exact host-time -> fixed tick tickets
    ${CMAKE_SOURCE_DIR}/platform/input_tick_queue.c  # bounded host edges -> tick pad state
    ${CMAKE_SOURCE_DIR}/platform/controller_mapping.c # normalized SDL controls -> player N64 bindings
    ${CMAKE_SOURCE_DIR}/platform/viewport_route_cache.c  # object x viewport draw admission
    ${CMAKE_SOURCE_DIR}/platform/audio_service_clock.c # host audio time -> bounded PCM debt
    ${CMAKE_SOURCE_DIR}/platform/audio_queue_controller.c # sink drain -> bounded PCM block
    ${CMAKE_SOURCE_DIR}/platform/audio_ring.c        # SPSC sink ring + underflow concealment
    ${CMAKE_SOURCE_DIR}/platform/audio_sink_evidence.c # accepted-SDL PCM diagnostic capture
    ${CMAKE_SOURCE_DIR}/platform/audio_volume.c       # player mix + click-free master gain
    ${CMAKE_SOURCE_DIR}/platform/present_sched.c    # its retrace-branch wiring (Phase 3 Wave B)
    ${CMAKE_SOURCE_DIR}/platform/sim_hash.c         # authoritative-state hash (Phase 1)
    ${CMAKE_SOURCE_DIR}/platform/gameplay_event_trace.c # ordered transient authority events
    ${CMAKE_SOURCE_DIR}/platform/input_consumption_trace.c # tick-consumed pad trace
    ${CMAKE_SOURCE_DIR}/platform/presentation_snapshot.c       # presentation shadow state (Phase 3 Wave A)
    ${CMAKE_SOURCE_DIR}/platform/presentation_snapshot_walk.c  # its gObjPtrList/camera capture wiring
    ${CMAKE_SOURCE_DIR}/platform/platform_sdl_min.c
    ${CMAKE_SOURCE_DIR}/platform/app_overlay_hooks.c # zero-cost overlay dispatch/stub
    ${CMAKE_SOURCE_DIR}/platform/stubs_dkr.c
    ${CMAKE_SOURCE_DIR}/platform/rom_io.c
    ${CMAKE_SOURCE_DIR}/platform/rom_id.c          # which DKR revision is this?
    ${CMAKE_SOURCE_DIR}/platform/rom_validation.c  # authoritative full-image gate
    ${CMAKE_SOURCE_DIR}/platform/sha256.c
    ${CMAKE_SOURCE_DIR}/platform/save_codec.c
    ${CMAKE_SOURCE_DIR}/platform/virtual_pak.c
    ${CMAKE_SOURCE_DIR}/platform/segment_consts.c
    ${CMAKE_SOURCE_DIR}/platform/gu_port.c
    ${CMAKE_SOURCE_DIR}/platform/asset_swap.c
    ${CMAKE_SOURCE_DIR}/platform/gfx_ptr.c          # registry globals (PLAN #3)
    ${CMAKE_SOURCE_DIR}/platform/gfx_config_shim.c  # render/post-FX config globals (off)
    ${CMAKE_SOURCE_DIR}/platform/hasm_stubs_temp.c
    ${CMAKE_SOURCE_DIR}/platform/math_util_native.c
    ${CMAKE_SOURCE_DIR}/platform/mixer.c            # M5: software aspMain audio mixer
    ${CMAKE_SOURCE_DIR}/platform/audi_port_dkr.c    # audio-time service + SDL/worklet sinks
    # Clean-room native audio engine, split by stage (audio_compat_internal.h
    # carries the include prologue and the few declarations that cross these).
    ${CMAKE_SOURCE_DIR}/platform/audio_compat.c     # engine globals + DKR extensions
    ${CMAKE_SOURCE_DIR}/platform/audio_bank.c       # instrument-bank + seqfile parsing
    ${CMAKE_SOURCE_DIR}/platform/audio_sequence.c   # compressed + uncompressed MIDI readers
    ${CMAKE_SOURCE_DIR}/platform/audio_envmix.c     # envelope mixer + equal-power pan table
    ${CMAKE_SOURCE_DIR}/platform/audio_fx.c         # reverb/FX unit + low-pass filter
    ${CMAKE_SOURCE_DIR}/platform/audio_filters.c    # ADPCM/RAW16 load, resampler, buses, save
    ${CMAKE_SOURCE_DIR}/platform/audio_synth.c      # synthesis driver + alSyn* voice API
    ${CMAKE_SOURCE_DIR}/platform/audio_seqplayer.c  # shared sequence-player state
    ${CMAKE_SOURCE_DIR}/platform/audio_cspplayer.c  # compressed-sequence (CSP) player
    ${CMAKE_SOURCE_DIR}/platform/audio_event_queue.c # clean-room ALEventQueue + occupancy stats
    ${CMAKE_SOURCE_DIR}/platform/audio_fx_transfer.c # reverb delay-line transfer planner + guard
    ${CMAKE_SOURCE_DIR}/platform/mdkr_adventure.c   # P3.5b: Adventure route test hooks (no-op unless set)
    ${CMAKE_SOURCE_DIR}/platform/mdkr_challenge.c   # Challenge/battle terminal-event test driver
    ${CMAKE_SOURCE_DIR}/platform/mdkr_taj.c         # Taj challenge lifecycle/outcome test driver
)
# GL-only support TUs — the browser build is WebGPU-only, so they'd only pull in
# glad/GL symbols that don't exist under wasm. Keep them native-only.
if(NOT EMSCRIPTEN)
    list(APPEND PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/fast3d_gl_shim.c   # GE-symbol shims for gfx_opengl.c
        ${CMAKE_SOURCE_DIR}/platform/platform_stdio.c   # ge007_sprintf (used by gfx_opengl.c)
    )
else()
    # M8: the AudioWorklet output backend (web-only; empty TU on native).
    list(APPEND PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/web_audio_worklet.c)
endif()

# Renderer front-end + backends (platform/fast3d/). gfx_pc_dkr.c (the F3DDKR
# HLE) + the vendored mgb64 backends. The standalone Metal backend
# (gfx_metal.mm) was dropped post-1.0.6 — it was never built by any target
# here; see NOTICE.md and mgb64 if a Metal backend is ever revisited. The *.c
# glob below also excludes screenshot_series.c (unused capture helper). The
# WebGPU backend TUs are added back conditionally (they need the wgpu include +
# MDKR_WEBGPU_BACKEND). Under Emscripten the GL backend (gfx_opengl.c) is dropped
# — the wasm binary renders exclusively through WebGPU.
file(GLOB FAST3D_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/platform/fast3d/*.c)
list(REMOVE_ITEM FAST3D_SOURCES ${CMAKE_SOURCE_DIR}/platform/fast3d/screenshot_series.c)
list(REMOVE_ITEM FAST3D_SOURCES
    ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu.c
    ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_async_request.c
    ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_callback_latch.c
    ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_fault.c
    ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_surface_policy.c
    ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_shader.c)
if(EMSCRIPTEN)
    list(REMOVE_ITEM FAST3D_SOURCES ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_opengl.c)
endif()
if(MDKR_WEBGPU_BACKEND)
    list(APPEND FAST3D_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_async_request.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_callback_latch.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_fault.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_surface_policy.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_shader.c)
    # Backend support TUs (only meaningful with the webgpu code):
    #   port_env.c         — port_env_bool used by the (dormant) pipeline prewarm
    #   gfx_webgpu_stubs.c — inert satisfiers for mgb64-shell symbols (overlay,
    #                        host handoff, minimap, savedir) DKR has no analogue for
    list(APPEND PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/port_env.c
        ${CMAKE_SOURCE_DIR}/platform/gfx_webgpu_stubs.c)
endif()

# App-shell seams, compiled into the ENGINE (they are plain C and the engine is
# what calls them). host_window.c and gfx_webgpu_stubs.c are no longer mutually
# exclusive: the stubs file #ifndef's out its handoff/overlay half under
# MDKR_APP, so exactly one definition of each symbol reaches the link.
if(MDKR_APP)
    list(APPEND PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/host_window.c)         # window/device adoption
endif()

# glad GL loader — desktop only (no GL in the browser build).
if(NOT EMSCRIPTEN)
    set(GLAD_SOURCES ${CMAKE_SOURCE_DIR}/lib/glad/src/glad.c)
else()
    set(GLAD_SOURCES "")
endif()

# --- Target name ----------------------------------------------------------
# Native binary is `mdkr64`; the wasm engine is `mdkr64_web` (emits
# mdkr64_web.js + mdkr64_web.wasm, loaded by dist/web/index.html).
if(EMSCRIPTEN)
    set(MDKR_TARGET mdkr64_web)
else()
    set(MDKR_TARGET mdkr64)
endif()

add_executable(${MDKR_TARGET}
    ${GAME_SOURCES}
    ${PLATFORM_SOURCES}
    ${FAST3D_SOURCES}
    ${GLAD_SOURCES}
)

# Include layout mirrors the DKR Makefile (-I . -I include -I include/libc
# -I include/PR -I include/sys -I src -I libultra), rooted at game/ so paths like
# "src/memory.h" and "libultra/src/audio/foo.h" resolve.
target_include_directories(${MDKR_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/game
    ${CMAKE_SOURCE_DIR}/game/src
    ${CMAKE_SOURCE_DIR}/game/include
    ${CMAKE_SOURCE_DIR}/game/include/PR
    ${CMAKE_SOURCE_DIR}/game/include/sys
    ${CMAKE_SOURCE_DIR}/game/libultra
    ${CMAKE_SOURCE_DIR}/game/libultra/src/audio
    ${CMAKE_SOURCE_DIR}/platform
    ${CMAKE_SOURCE_DIR}/platform/fast3d
    ${CMAKE_SOURCE_DIR}/platform/fast3d_shim
    ${CMAKE_SOURCE_DIR}/lib/glad/include
    ${CMAKE_SOURCE_DIR}/lib/stb
    ${SDL2_INCLUDE_DIRS}
)

# --- Compile definitions (PLAN §8) ---------------------------------------
target_compile_definitions(${MDKR_TARGET} PRIVATE
    VERSION_us_v80          # common.h -> VERSION_80 / REGION_NA
    _LANGUAGE_C             # gates the type + gbi definitions in the PR headers
    MODERN_CC               # PR/os_libc.h: size_t bcopy/bzero/bcmp (match system <strings.h>)
    NON_MATCHING=1
    AVOID_UB=1
    NATIVE_PORT=1
    F3DDKR_GBI
    _FINALROM
    SDL_MAIN_HANDLED
)

# --- Version stamping (PK-1, macOS packaging) ------------------------------
# Single source of truth for the packaged release version. Wired to a real
# surface (`mdkr64 --version`, platform/main_pc.c) and to the app bundle's
# CFBundleShortVersionString, which macos/Scripts/build_app_bundle.sh stamps
# from this same value via PlistBuddy. Kept as its own standalone statement
# (not folded into the block above) so it stays a one-line, low-conflict diff.
set(MDKR_VERSION "1.1.0" CACHE STRING "mdkr64 release version (semver)")
target_compile_definitions(${MDKR_TARGET} PRIVATE MDKR_VERSION_STRING="${MDKR_VERSION}")
# The save CLI stamps app_version into every container it writes, so it must
# carry the same release value the engine and app shell do. Its target is
# declared above this statement, hence the guarded definition here rather than
# beside add_executable(mdkr-save).
if(TARGET mdkr-save)
    target_compile_definitions(mdkr-save PRIVATE
        MDKR_APP_VERSION_STR="${MDKR_VERSION}")
endif()

# --- Flags ----------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang" OR EMSCRIPTEN)
    # Keep this before the reviewed -Werror classes below so those classes are
    # re-enabled after the umbrella suppression.
    target_compile_options(${MDKR_TARGET} PRIVATE -Wno-everything)
endif()
# PLATFORM_SOURCES is maintained port code; unlike the matching decomp and
# vendored renderer TUs, it must build with the normal strict warning baseline.
# Per-source flags are intentionally applied after the target-wide Clang
# suppression above, leaving legacy/decomp diagnostics untouched while making
# new platform regressions fail at the file that introduced them.
if(MSVC)
    set_source_files_properties(${PLATFORM_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/W4;/WX")
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang" OR
       CMAKE_C_COMPILER_ID STREQUAL "GNU" OR EMSCRIPTEN)
    set_source_files_properties(${PLATFORM_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
endif()
target_compile_options(${MDKR_TARGET} PRIVATE
    -fno-strict-aliasing
    # Address-domain crossings must be explicit (uintptr_t, DKR_TOK/DKR_PTR, or
    # OS_K0_TOKEN_TO_PHYSICAL). A raw narrowing conversion is a correctness
    # defect on LP64 and must never silently return to the tree.
    -Werror=int-to-pointer-cast
    -Werror=pointer-to-int-cast
    # Matching-era source contains many harmless legacy warnings, but these
    # classes directly predict native correctness defects and must stay live.
    -Werror=return-type
    -Werror=uninitialized
    -Werror=array-bounds
    -Werror=shift-negative-value
)
if(NOT EMSCRIPTEN)
    # -fcommon: legacy tentative-definition merging the decomp relies on. emcc's
    # clang defaults to -fno-common and the port links clean without it, so it is
    # native-only (avoids an unknown-flag-in-some-configs surprise on wasm).
    target_compile_options(${MDKR_TARGET} PRIVATE -fcommon)
endif()
# emcc is Clang-based, so the Clang-only parse flags apply to it too.
# The decomp is warning-noisy. Clang's umbrella suppression is useful, but GCC
# does not implement it; worse, GCC rejects Clang's shift-sign-overflow and
# null-pointer-arithmetic warning names when promoted with -Werror. Keep the
# shared risk classes above and add only diagnostics the selected compiler
# actually owns. This split is also what makes the Linux GCC CI cells real.
if(CMAKE_C_COMPILER_ID MATCHES "Clang" OR EMSCRIPTEN)
    target_compile_options(${MDKR_TARGET} PRIVATE
        -Werror=shift-sign-overflow
        -Werror=null-pointer-arithmetic
        -ferror-limit=0
        -fms-extensions
        # An undeclared call is not merely style on wasm: Clang assumes `int`
        # and wasm-ld can then bridge two incompatible function signatures.
        # Keep this after -Wno-everything so the class cannot return silently.
        -Werror=implicit-function-declaration
    )
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    # GCC's object-size analysis catches writes that cross scalar subobjects
    # and loops whose runtime bound can exceed the destination array. Both are
    # native memory-safety defects, even when the surrounding struct happens
    # to place the next field contiguously today.
    target_compile_options(${MDKR_TARGET} PRIVATE
        -Werror=stringop-overflow
    )
endif()

# --- Sweep hook: flags that must land AFTER -Wno-everything ----------------
# `-Wno-everything` above silences the whole warning set, which also disables
# the static instruments this project uses to hunt the split-array /
# index-past-the-end bug class (-Warray-bounds) and the UBSan checks that
# caught the wave-table defect (-fsanitize=array-bounds). CMAKE_C_FLAGS is
# placed BEFORE target_compile_options, so passing them there does not work.
# Off by default; the sweep builds set it, e.g.
#   cmake -S . -B build-ubsan -DMDKR_EXTRA_C_FLAGS="-fsanitize=array-bounds"
if(MDKR_EXTRA_C_FLAGS)
    separate_arguments(MDKR_EXTRA_C_FLAGS_LIST NATIVE_COMMAND "${MDKR_EXTRA_C_FLAGS}")
    target_compile_options(${MDKR_TARGET} PRIVATE ${MDKR_EXTRA_C_FLAGS_LIST})
endif()

if(NOT EMSCRIPTEN)
    # --- Native link ------------------------------------------------------
    target_link_directories(${MDKR_TARGET} PRIVATE ${SDL2_LIBRARY_DIRS})
    if(MINGW)
        # Windows links SDL2 statically — see the "Self-contained exe" block
        # below for the full rationale and the pkg-config --static plumbing.
        # ld prefers libSDL2.dll.a over libSDL2.a when both are visible, so
        # the archive must be forced with a -Bstatic bracket; the static
        # system-lib tail (SDL2_STATIC_LIBRARIES from Libs.private) follows in
        # that block, after this first mention wins symbol resolution.
        # mingw32 first: it carries the -mwindows entry glue that lets our
        # plain int main() satisfy the GUI subsystem (the pc file's original
        # -lmingw32 -lSDL2main -lSDL2 ordering, minus the SDL2main WinMain
        # shim that an SDL_MAIN_HANDLED build must not pull).
        target_link_libraries(${MDKR_TARGET} PRIVATE
            mingw32 SDL2main "-Wl,-Bstatic" SDL2 "-Wl,-Bdynamic" m)
    else()
        # The app-shell library publishes SDL2 to its sole consumer. Avoid
        # spelling the same dylib both here and through mdkr64_app: ld64 warns
        # about the duplicate even though it is harmless. CLI-only builds do
        # not have that interface and link SDL2 directly.
        if(NOT MDKR_APP)
            target_link_libraries(${MDKR_TARGET} PRIVATE ${SDL2_LIBRARIES})
        endif()
        target_link_libraries(${MDKR_TARGET} PRIVATE m)
    endif()
    if(APPLE)
        target_link_libraries(${MDKR_TARGET} PRIVATE ${OPENGL_LIB})
    else()
        target_link_libraries(${MDKR_TARGET} PRIVATE OpenGL::GL ${CMAKE_DL_LIBS})
    endif()
    # WebGPU backend: gate the code + link the pinned wgpu-native prebuilt. GL +
    # WebGPU stay compiled together so MDKR_RENDERER switches at runtime.
    if(MDKR_WEBGPU_BACKEND)
        target_compile_definitions(${MDKR_TARGET} PRIVATE MDKR_WEBGPU_BACKEND)
        target_link_libraries(${MDKR_TARGET} PRIVATE webgpu)
    endif()

    # =======================================================================
    # Native app shell — a separate C++ static library with CLEAN flags.
    #
    # It must not inherit the engine's C-only decomp flags (-Wno-everything,
    # -fms-extensions, -fcommon, the VERSION_us_v80/_LANGUAGE_C definitions),
    # and just as importantly it must NOT get the engine's include dirs:
    # game/include contains shim headers for the decomp that shadow the system
    # angle-includes ImGui and SDL pull in. The app<->engine seam headers
    # therefore live in the collision-free platform/app/ directory, and the app
    # lib's include path is only platform/ + platform/app + lib/imgui.
    #
    # main() comes from platform/app/main_app.cpp; main_pc.c renames its main to
    # mdkr64_headless_main under -DMDKR_APP.
    # =======================================================================
    if(MDKR_APP)
        find_package(Threads REQUIRED)
        set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/lib/imgui)
        set(IMGUI_SOURCES
            ${IMGUI_DIR}/imgui.cpp
            ${IMGUI_DIR}/imgui_draw.cpp
            ${IMGUI_DIR}/imgui_tables.cpp
            ${IMGUI_DIR}/imgui_widgets.cpp
            ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
            ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

        # The launcher is an owned, actively changing surface. Keep this
        # manifest explicit: a new .cpp must be reviewed for the native app
        # instead of silently changing every desktop build through a glob.
        # The CI contract compares it with platform/app/ so omissions and
        # unreviewed additions fail closed at review time.
        set(APP_SOURCES
            ${CMAKE_SOURCE_DIR}/platform/app/app_config.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_config_escape.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_host.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_relaunch.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_restart.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_theme.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_ui_policy.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_window.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/arg_triage.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/diag_log.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/engine_boot.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/main_app.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/rom_validate.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_common.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_diag.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_launcher.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_overlay.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_rom.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_settings.cpp)

        # Native "open file" panel, one implementation per platform. A
        # user-selected file is permission-exempt, which is why this replaced
        # the directory scan that used to trip macOS TCC.
        if(APPLE)
            enable_language(OBJCXX)
            list(APPEND APP_SOURCES
                ${CMAKE_SOURCE_DIR}/platform/app/app_activation_mac.mm
                ${CMAKE_SOURCE_DIR}/platform/app/file_dialog_mac.mm)
        elseif(WIN32)
            list(APPEND APP_SOURCES ${CMAKE_SOURCE_DIR}/platform/app/file_dialog_win.cpp)
        else()
            # Linux and friends: no permission-free native panel exists without a
            # new hard dependency (the portal route needs libdbus at configure
            # time), so isAvailable() is false and the ROM panel draws no Browse
            # button. Drag-and-drop + the typed path are the documented paths.
            list(APPEND APP_SOURCES ${CMAKE_SOURCE_DIR}/platform/app/file_dialog_stub.cpp)
        endif()

        add_library(mdkr64_app STATIC ${IMGUI_SOURCES} ${APP_SOURCES})
        target_compile_features(mdkr64_app PRIVATE cxx_std_17)
        # Keep warnings strict on the launcher code we own without inheriting
        # that policy into vendored Dear ImGui.  The engine's maintained C
        # sources use the same fail-closed baseline above; app-shell C++ should
        # not be able to introduce a warning merely because the wider decomp
        # target still needs legacy suppression.
        if(MSVC)
            set_source_files_properties(${APP_SOURCES} PROPERTIES
                COMPILE_OPTIONS "/W4;/WX")
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
               CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set_source_files_properties(${APP_SOURCES} PROPERTIES
                COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
        endif()
        # GL_SILENCE_DEPRECATION: macOS deprecates the GL API but still ships it.
        # Provenance stamp for the About panel's version line. Release CI threads
        # the real commit through (-DMDKR_BUILD_STAMP=<sha>, the same commit
        # tools/release/stamp_provenance.sh binds the artifact to); a plain dev
        # build has none and the version line simply omits it rather than
        # printing a placeholder that looks like provenance but is not.
        set(MDKR_BUILD_STAMP "" CACHE STRING "Provenance commit shown in the About panel")
        target_compile_definitions(mdkr64_app PRIVATE
            MDKR_APP
            GL_SILENCE_DEPRECATION
            MDKR_APP_VERSION_STR="${MDKR_VERSION}"
            MDKR_APP_BUILD_STAMP="${MDKR_BUILD_STAMP}")
        target_include_directories(mdkr64_app PRIVATE
            ${IMGUI_DIR}
            ${IMGUI_DIR}/backends
            ${CMAKE_SOURCE_DIR}/platform
            ${CMAKE_SOURCE_DIR}/platform/app
            ${CMAKE_SOURCE_DIR}/lib/glad/include
            ${SDL2_INCLUDE_DIRS})
        target_link_directories(mdkr64_app PRIVATE ${SDL2_LIBRARY_DIRS})
        if(MINGW)
            # Same static-SDL2 rule as the engine target: SDL2_LIBRARIES here
            # carries mingw32/SDL2main, whose WinMain shim demands an SDL_main
            # we do not define (SDL_MAIN_HANDLED build). Link the bracketed
            # static archive only; the system-lib tail rides in from the
            # engine target's link interface.
            target_link_libraries(mdkr64_app PUBLIC
                mingw32 SDL2main "-Wl,-Bstatic" SDL2 "-Wl,-Bdynamic")
        else()
            target_link_libraries(mdkr64_app PUBLIC ${SDL2_LIBRARIES})
        endif()
        target_link_libraries(mdkr64_app PRIVATE Threads::Threads)
        if(APPLE)
            # AppKit: NSOpenPanel. UniformTypeIdentifiers: the UTType extension
            # filter available throughout the supported macOS 13+ range.
            target_link_libraries(mdkr64_app PUBLIC
                ${OPENGL_LIB}
                "-framework AppKit"
                "-framework UniformTypeIdentifiers")
        elseif(WIN32)
            # comdlg32: GetOpenFileNameW. A stock Windows system DLL — it is a
            # deliberate, documented addition to the import allowlist in
            # .github/workflows/windows-validate.yml and tools/mingw_cross_check.sh.
            target_link_libraries(mdkr64_app PUBLIC OpenGL::GL comdlg32)
        else()
            target_link_libraries(mdkr64_app PUBLIC OpenGL::GL)
        endif()
        # WebGPU launcher path: the in-house ImGui-on-WebGPU renderer, so the
        # launcher UI and the F1 overlay render through the same device the game
        # does. platform/fast3d is safe on the app include path (fast3d seam
        # headers only; NOT the decomp shims under game/include).
        if(MDKR_WEBGPU_BACKEND)
            target_sources(mdkr64_app PRIVATE
                ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_imgui.cpp)
            target_include_directories(mdkr64_app PRIVATE
                ${CMAKE_SOURCE_DIR}/platform/fast3d)
            target_compile_definitions(mdkr64_app PRIVATE MDKR_WEBGPU_BACKEND)
            target_link_libraries(mdkr64_app PUBLIC webgpu)
        endif()

        # The engine's entry point now comes from the app shell.
        target_compile_definitions(${MDKR_TARGET} PRIVATE MDKR_APP)
        target_link_libraries(${MDKR_TARGET} PRIVATE mdkr64_app)

        if(BUILD_TESTING)
            # ROM-free, window-free unit tests for the shell's pure logic.
            add_executable(mdkr_app_shell_test
                ${CMAKE_SOURCE_DIR}/tests/test_app_shell.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/arg_triage.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/rom_validate.cpp
                ${CMAKE_SOURCE_DIR}/platform/rom_id.c
                ${CMAKE_SOURCE_DIR}/platform/rom_validation.c
                ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
                ${CMAKE_SOURCE_DIR}/platform/sha256.c)
            target_include_directories(mdkr_app_shell_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_app_shell_test PRIVATE cxx_std_17)
            add_test(NAME app_shell COMMAND mdkr_app_shell_test)

            add_executable(mdkr_app_ui_policy_test
                ${CMAKE_SOURCE_DIR}/tests/test_app_ui_policy.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/app_ui_policy.cpp)
            target_include_directories(mdkr_app_ui_policy_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_app_ui_policy_test PRIVATE cxx_std_17)
            add_test(NAME app_ui_policy COMMAND mdkr_app_ui_policy_test)

            add_executable(mdkr_app_config_test
                ${CMAKE_SOURCE_DIR}/tests/test_app_config.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/app_config.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/app_config_escape.cpp
                ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c)
            target_include_directories(mdkr_app_config_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app
                ${SDL2_INCLUDE_DIRS})
            target_link_directories(mdkr_app_config_test PRIVATE
                ${SDL2_LIBRARY_DIRS})
            target_compile_features(mdkr_app_config_test PRIVATE cxx_std_17)
            target_compile_definitions(mdkr_app_config_test PRIVATE
                MDKR_APP_CONFIG_TESTING=1)
            target_link_libraries(mdkr_app_config_test PRIVATE ${SDL2_LIBRARIES})
            add_test(NAME app_config_persistence COMMAND mdkr_app_config_test)
            set_tests_properties(app_config_persistence PROPERTIES TIMEOUT 30)

            add_executable(mdkr_app_window_test
                ${CMAKE_SOURCE_DIR}/tests/test_app_window.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/app_window.cpp)
            target_include_directories(mdkr_app_window_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app
                ${SDL2_INCLUDE_DIRS})
            target_compile_features(mdkr_app_window_test PRIVATE cxx_std_17)
            add_test(NAME app_window COMMAND mdkr_app_window_test)

            add_executable(mdkr_app_lifecycle_test
                ${CMAKE_SOURCE_DIR}/tests/test_app_lifecycle.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/app_restart.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/app_relaunch.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/diag_log.cpp
                ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c)
            target_include_directories(mdkr_app_lifecycle_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app
                ${SDL2_INCLUDE_DIRS})
            target_link_directories(mdkr_app_lifecycle_test PRIVATE
                ${SDL2_LIBRARY_DIRS})
            target_compile_features(mdkr_app_lifecycle_test PRIVATE cxx_std_17)
            find_package(Threads REQUIRED)
            target_link_libraries(mdkr_app_lifecycle_test PRIVATE
                ${SDL2_LIBRARIES} Threads::Threads)
            file(MAKE_DIRECTORY
                "${CMAKE_CURRENT_BINARY_DIR}/app-test-data/lifecycle-prefs")
            add_test(NAME app_lifecycle COMMAND mdkr_app_lifecycle_test)
            set_tests_properties(app_lifecycle PROPERTIES
                ENVIRONMENT "MDKR_APP_PREFS_DIR=${CMAKE_CURRENT_BINARY_DIR}/app-test-data/lifecycle-prefs"
                PASS_REGULAR_EXPRESSION "app lifecycle passed"
                TIMEOUT 30)
            add_test(NAME cli_version_exact
                COMMAND ${CMAKE_COMMAND}
                    -DMDKR_EXECUTABLE=$<TARGET_FILE:${MDKR_TARGET}>
                    -DMDKR_EXPECTED_VERSION=${MDKR_VERSION}
                    -P ${CMAKE_SOURCE_DIR}/tests/check_cli_version.cmake)

            # Schema round-trip: every key the settings panel renders must have a
            # category, a label, help text, and must resolve back by name. Runs
            # fully headless (no window, no GPU) via the shell's own self-check.
            #
            # The caveat below is kFrameLimitHelp (platform/app/ui_settings.cpp)
            # verbatim, except that its one literal ";" is written "." — CTest
            # splits a PASS_REGULAR_EXPRESSION on unescaped semicolons and would
            # otherwise accept either half on its own. check_ci_contract.py
            # regenerates and compares this string, so it cannot go stale.
            set(MDKR_FRAME_LIMIT_UI_CONTRACT_REGEX
                "recommended=\"Original \\(recommended\\)\" group=\"Higher refresh rates\" caveat=\"Original presents each authored image once. Higher rates repeat authored images when smoothing is Off, or create in-between images when it is Interpolated. Gameplay speed does not change. Higher rates can use more CPU and GPU time. Rates above your display's refresh need a display connection that can drop an image it has not shown yet. Where the system does not offer one, they present at your display's refresh instead, unless Allow Tearing is on. A European 50 Hz game paces unevenly under Original: its authored image lasts 40 ms, which no whole number of 60 Hz refreshes fits, so Original holds it for two refreshes and then three and the motion ripples. Match Display with Interpolated removes that ripple without changing game speed, music pitch, or timers. Just Under Display is for a display with a variable refresh rate: it paces a few Hz below the top of that range, which keeps the display adapting to the game rather than falling back to a fixed refresh, and it re-reads the rate if you move the window to another monitor. 40 Hz is a battery-friendly choice on a handheld whose display runs at 40 or 120 Hz, where every image is held for the same length of time. Uncapped removes the native limit only when new interpolated images are available. held frames stay display-paced. A browser always maps Uncapped and Just Under Display to Match Display.\"")

            add_test(NAME app_schema COMMAND ${MDKR_TARGET})
            set_tests_properties(app_schema PROPERTIES
                ENVIRONMENT "MDKR_APP_DUMP_SCHEMA=1;MDKR_AUDIO=0"
                PASS_REGULAR_EXPRESSION "${MDKR_FRAME_LIMIT_UI_CONTRACT_REGEX}")

            # Shell render smoke: drive the launcher for a few frames and require
            # a real captured frame. This is what makes the gate honest — it
            # proves pixels reached a framebuffer, not merely that main() returned
            # 0 (main_app.cpp fails the run when a requested capture is missing).
            # Needs a GPU, so it is skipped when MDKR_SKIP_GPU_TESTS is set.
            if(NOT MDKR_SKIP_GPU_TESTS)
                set(MDKR_APP_TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/app-test-data")
                file(MAKE_DIRECTORY
                    "${MDKR_APP_TEST_ROOT}/shell/prefs"
                    "${MDKR_APP_TEST_ROOT}/shell/saves"
                    "${MDKR_APP_TEST_ROOT}/settings/prefs"
                    "${MDKR_APP_TEST_ROOT}/settings/saves"
                    "${MDKR_APP_TEST_ROOT}/top-tabs/prefs"
                    "${MDKR_APP_TEST_ROOT}/top-tabs/saves"
                    "${MDKR_APP_TEST_ROOT}/top-tabs-small/prefs"
                    "${MDKR_APP_TEST_ROOT}/top-tabs-small/saves"
                    "${MDKR_APP_TEST_ROOT}/controller/prefs"
                    "${MDKR_APP_TEST_ROOT}/controller/saves"
                    "${MDKR_APP_TEST_ROOT}/minimum/prefs"
                    "${MDKR_APP_TEST_ROOT}/minimum/saves"
                    "${MDKR_APP_TEST_ROOT}/touch-handheld/prefs"
                    "${MDKR_APP_TEST_ROOT}/touch-handheld/saves"
                    "${MDKR_APP_TEST_ROOT}/dpi/prefs"
                    "${MDKR_APP_TEST_ROOT}/dpi/saves"
                    "${MDKR_APP_TEST_ROOT}/recovery/prefs"
                    "${MDKR_APP_TEST_ROOT}/recovery/saves"
                    "${MDKR_APP_TEST_ROOT}/gl/prefs"
                    "${MDKR_APP_TEST_ROOT}/gl/saves")
                file(REMOVE
                    "${MDKR_APP_TEST_ROOT}/shell/video.ini"
                    "${MDKR_APP_TEST_ROOT}/settings/video.ini"
                    "${MDKR_APP_TEST_ROOT}/top-tabs/video.ini"
                    "${MDKR_APP_TEST_ROOT}/top-tabs-small/video.ini"
                    "${MDKR_APP_TEST_ROOT}/controller/video.ini"
                    "${MDKR_APP_TEST_ROOT}/minimum/video.ini"
                    "${MDKR_APP_TEST_ROOT}/touch-handheld/video.ini"
                    "${MDKR_APP_TEST_ROOT}/dpi/video.ini"
                    "${MDKR_APP_TEST_ROOT}/recovery/video.ini"
                    "${MDKR_APP_TEST_ROOT}/gl/video.ini")
                file(WRITE "${MDKR_APP_TEST_ROOT}/shell/prefs/mdkr64_app.ini"
                    "# isolated first-run shell\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/settings/prefs/mdkr64_app.ini"
                    "# isolated first-run settings\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/top-tabs/prefs/mdkr64_app.ini"
                    "# intermediate-width navigation qualification\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/top-tabs-small/prefs/mdkr64_app.ini"
                    "# minimum-scale top-navigation qualification\nui_scale=0.75\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/controller/prefs/mdkr64_app.ini"
                    "# isolated controller settings\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/minimum/prefs/mdkr64_app.ini"
                    "# responsive-layout qualification\nui_scale=2.00\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/touch-handheld/prefs/mdkr64_app.ini"
                    "# 7-inch Windows handheld qualification\nui_scale=1.25\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/gl/prefs/mdkr64_app.ini"
                    "# isolated GL fallback\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/dpi/prefs/mdkr64_app.ini"
                    "# isolated DPI lifecycle\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/recovery/prefs/mdkr64_app.ini"
                    "# compact failed-engine recovery\nui_scale=2.00\n")

                # Every capture-consuming test below is paired to the smoke that
                # WRITES its ${CMAKE_CURRENT_BINARY_DIR}/app_*.bmp through a
                # CTest fixture, not only DEPENDS. DEPENDS orders two tests when
                # both are in the same run and is SILENTLY IGNORED otherwise, so
                # `ctest -R app_settings_capture_content`, a `--repeat` pass that
                # reruns one half of a pair, or any future change that stops
                # labelling the pure-Python content tests `gpu` (and so drops
                # them out of mdkr_serialize_gpu_ctests' shared lock) would leave
                # a consumer reading a stale or absent capture. FIXTURES_REQUIRED
                # pulls the producer into the run and fails the consumer closed
                # if it did not succeed. DEPENDS is kept alongside as the
                # explicit ordering statement.
                add_test(NAME app_shell_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_shell_smoke PROPERTIES
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=4;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_shell_smoke.bmp;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/shell/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/shell/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/shell/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    TIMEOUT 180)

                # Keep the first-run Settings view in the rendered contract:
                # presentation-rate controls are directly visible, while the
                # gameplay-changing cadence control is clearly separated.
                add_test(NAME app_settings_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_settings_smoke PROPERTIES
                    FIXTURES_SETUP app_settings_capture
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=4;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_settings_smoke.bmp;MDKR_APP_PANEL=Settings;MDKR_APP_UI_TRACE=1;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/settings/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/settings/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/settings/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION "frame-rate-controls visible=1 gameplay-accuracy-separated=1"
                    TIMEOUT 180)

                add_test(NAME app_settings_capture_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_app_capture.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_settings_smoke.bmp
                        --self-test)
                set_tests_properties(app_settings_capture_content PROPERTIES
                    FIXTURES_REQUIRED app_settings_capture
                    DEPENDS app_settings_smoke
                    LABELS gpu
                    TIMEOUT 60)

                # The responsive launcher has three distinct navigation modes.
                # Wide sidebar and dense dropdown coverage existed already;
                # keep the intermediate top-tab layout from regressing into
                # multiple blue selections or losing its active indicator.
                add_test(NAME app_top_tabs_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_top_tabs_smoke PROPERTIES
                    FIXTURES_SETUP app_top_tabs_capture
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=4;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_top_tabs.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=800x600;MDKR_APP_PANEL=Game ROM;MDKR_APP_SMOKE_NAV_TARGET=1;MDKR_APP_SMOKE_NAV_TOKEN=mdkr64-app-nav-v1;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/top-tabs/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/top-tabs/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/top-tabs/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    PASS_REGULAR_EXPRESSION "top navigation target=1 actual=1 queued=1"
                    LABELS gpu
                    TIMEOUT 180)
                add_test(NAME app_top_tabs_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_launcher_tabs.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_top_tabs.bmp)
                set_tests_properties(app_top_tabs_content PROPERTIES
                    FIXTURES_REQUIRED app_top_tabs_capture
                    DEPENDS app_top_tabs_smoke
                    LABELS gpu
                    TIMEOUT 60)

                add_test(NAME app_top_tabs_scale075_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_top_tabs_scale075_smoke PROPERTIES
                    FIXTURES_SETUP app_top_tabs_scale075_capture
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=4;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_top_tabs_scale075.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=640x480;MDKR_APP_PANEL=Game ROM;MDKR_APP_SMOKE_NAV_TARGET=1;MDKR_APP_SMOKE_NAV_TOKEN=mdkr64-app-nav-v1;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/top-tabs-small/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/top-tabs-small/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/top-tabs-small/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    PASS_REGULAR_EXPRESSION "top navigation target=1 actual=1 queued=1"
                    LABELS gpu
                    TIMEOUT 180)
                add_test(NAME app_top_tabs_scale075_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_launcher_tabs.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_top_tabs_scale075.bmp)
                set_tests_properties(app_top_tabs_scale075_content PROPERTIES
                    FIXTURES_REQUIRED app_top_tabs_scale075_capture
                    DEPENDS app_top_tabs_scale075_smoke
                    LABELS gpu
                    TIMEOUT 60)

                # Render every controller control and execute the exact Restore
                # defaults transaction. The dependent parser checks the isolated
                # INI, so this is a real UI-to-persistence contract rather than
                # a pre-widget diagnostic trace.
                add_test(NAME app_controller_settings_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_controller_settings_smoke PROPERTIES
                    FIXTURES_SETUP app_controller_settings_capture
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=4;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_controller_settings_smoke.bmp;MDKR_APP_SMOKE_CONTROLLER_SETTINGS=1;MDKR_APP_SMOKE_CONTROLLER_RESTORE=1;MDKR_APP_PANEL=Settings;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/controller/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/controller/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/controller/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION "controller settings rendered mappings=19 rumble=2 restoreAvailable=1 restoreSucceeded=1"
                    TIMEOUT 180)
                add_test(NAME app_controller_settings_persistence
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_controller_settings_persistence.py
                        ${MDKR_APP_TEST_ROOT}/controller/video.ini)
                set_tests_properties(app_controller_settings_persistence PROPERTIES
                    FIXTURES_REQUIRED app_controller_settings_capture
                    DEPENDS app_controller_settings_smoke
                    LABELS gpu
                    TIMEOUT 30)

                add_test(NAME app_boot_recovery_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_boot_recovery_smoke PROPERTIES
                    FIXTURES_SETUP app_boot_recovery_capture
                    ENVIRONMENT "MDKR_APP_BOOT_RECOVERY=The renderer could not start. Your ROM and settings were kept.;MDKR_APP_SMOKE_FRAMES=4;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_boot_recovery.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=640x480;MDKR_APP_UI_TRACE=1;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/recovery/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/recovery/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/recovery/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION "compact-layout dense=1 contained=1 overlap=0 contentSeparated=1"
                    TIMEOUT 180)
                add_test(NAME app_boot_recovery_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_app_capture.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_boot_recovery.bmp
                        --expect-recovery
                        --compact-palette
                        --self-test)
                set_tests_properties(app_boot_recovery_content PROPERTIES
                    FIXTURES_REQUIRED app_boot_recovery_capture
                    DEPENDS app_boot_recovery_smoke
                    LABELS gpu
                    TIMEOUT 60)

                add_test(NAME app_settings_minimum_scale2 COMMAND ${MDKR_TARGET})
                set_tests_properties(app_settings_minimum_scale2 PROPERTIES
                    FIXTURES_SETUP app_settings_minimum_scale2_capture
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=4;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_settings_minimum_scale2.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=640x480;MDKR_APP_PANEL=Settings;MDKR_APP_UI_TRACE=1;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/minimum/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/minimum/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/minimum/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION "compact-layout dense=1 contained=1 overlap=0 contentSeparated=1"
                    TIMEOUT 180)
                add_test(NAME app_settings_minimum_scale2_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_app_capture.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_settings_minimum_scale2.bmp
                        --compact-palette
                        --self-test)
                set_tests_properties(app_settings_minimum_scale2_content PROPERTIES
                    FIXTURES_REQUIRED app_settings_minimum_scale2_capture
                    DEPENDS app_settings_minimum_scale2
                    LABELS gpu
                    TIMEOUT 60)

                # A 1280x720 / 1.25x surface models the effective launcher
                # geometry of common 7-inch Windows handhelds. The scripted
                # drag is marked SDL_TOUCH_MOUSEID and crosses the production
                # SDL -> ImGui adapter, proving both target metrics and direct
                # content panning rather than only checking constants.
                add_test(NAME app_touch_handheld_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_touch_handheld_smoke PROPERTIES
                    FIXTURES_SETUP app_touch_handheld_capture
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=10;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_touch_handheld.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=1280x720;MDKR_APP_SMOKE_TOUCH_SCROLL=1;MDKR_APP_SMOKE_TOUCH_TOKEN=mdkr64-app-touch-v1;MDKR_APP_PANEL=Settings;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/touch-handheld/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/touch-handheld/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/touch-handheld/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION "touch handheld targets=1 scroll=1"
                    TIMEOUT 180)
                add_test(NAME app_touch_handheld_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_app_capture.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_touch_handheld.bmp
                        --self-test)
                set_tests_properties(app_touch_handheld_content PROPERTIES
                    FIXTURES_REQUIRED app_touch_handheld_capture
                    DEPENDS app_touch_handheld_smoke
                    LABELS gpu
                    TIMEOUT 60)

                add_test(NAME app_ui_input_persistence
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_app_ui_input.py
                        $<TARGET_FILE:${MDKR_TARGET}>)
                set_tests_properties(app_ui_input_persistence PROPERTIES
                    LABELS gpu
                    TIMEOUT 900)

                add_test(NAME app_dpi_transition_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_dpi_transition_smoke PROPERTIES
                    FIXTURES_SETUP app_dpi_transition_capture
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=5;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_dpi_transition.bmp;MDKR_APP_SMOKE_DPI_SEQUENCE=1,2,1;MDKR_APP_PANEL=Settings;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/dpi/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/dpi/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/dpi/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION "dpi transition scale=2 atlasGeneration="
                    TIMEOUT 180)
                add_test(NAME app_dpi_transition_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_app_capture.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_dpi_transition.bmp
                        --self-test)
                set_tests_properties(app_dpi_transition_content PROPERTIES
                    FIXTURES_REQUIRED app_dpi_transition_capture
                    DEPENDS app_dpi_transition_smoke
                    LABELS gpu
                    TIMEOUT 60)

                add_test(NAME app_gl_swap_interval_fallback COMMAND ${MDKR_TARGET})
                set_tests_properties(app_gl_swap_interval_fallback PROPERTIES
                    ENVIRONMENT "MDKR_RENDERER=gl;MDKR_TEST_GL_SWAP_INTERVAL_FAILURE=1;MDKR_APP_SMOKE_FRAMES=4;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/gl/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/gl/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/gl/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION "OpenGL swap interval unavailable; using bounded 60 Hz launcher pacing"
                    TIMEOUT 180)
            endif()
        endif()
    endif()

    # Must stay after every native GPU test registration. The resource lock is
    # independent of CTest's parallel job count, so plain `ctest -j` remains
    # reliable while non-GPU unit tests continue to run in parallel.
    if(BUILD_TESTING)
        mdkr_serialize_gpu_ctests()
    endif()
else()
    # ======================================================================
    # M8 — Emscripten (wasm) engine. Single-threaded, Asyncify-driven frame
    # loop (the blocking osRecvMesg on the retrace queue suspends to
    # requestAnimationFrame), WebGPU via emdawnwebgpu, MEMFS ROM + IDBFS saves.
    # Mirrors mgb64's ge007_web target. Everything below is additive — the
    # native branch never enters it.
    # ======================================================================
    target_compile_definitions(${MDKR_TARGET} PRIVATE MDKR_WEBGPU_BACKEND)
    target_link_libraries(${MDKR_TARGET} PRIVATE webgpu)   # --use-port=emdawnwebgpu
    target_compile_options(${MDKR_TARGET} PRIVATE "-sUSE_SDL=2")
    target_link_options(${MDKR_TARGET} PRIVATE
        "-sUSE_SDL=2"
        # A cross-TU type conflict becomes a wasm function-signature warning,
        # which otherwise still produces a runnable-looking module.
        "-Wl,--fatal-warnings"
        # --- Asyncify (narrowed) ------------------------------------------
        # Blanket -sASYNCIFY instruments EVERY path that could reach a suspend
        # (1.5-3x on instrumented paths) for a handful of real suspend sites.
        # IGNORE_INDIRECT stops reachability inference at function-pointer edges,
        # so only functions reaching a suspend via DIRECT calls auto-instrument;
        # ADD force-instruments the functions that OWN an indirect suspend-reaching
        # call (their indirect call site then gets the unwind/rewind scaffolding).
        #
        # DKR's suspend spine (confirm with -sASYNCIFY_ADVISE):
        #   * Per-frame rAF wait: the blocking osRecvMesg on the video queue ->
        #     platform_vi_pace_measure -> pace_sleep_until -> platformWaitAnimation-
        #     Frame (EM_ASYNC_JS). The whole game-loop chain to it is DIRECT
        #     (main -> thread3_main -> main_game_loop -> fb_update -> osRecvMesg),
        #     so it auto-instruments. Only symbols that survive the Release link
        #     belong in ADD; inlined-away documentation names make every clean
        #     emcc link warn and provide no instrumentation.
        #   * WebGPU bring-up: gfx_init calls rapi->init() through the vtable
        #     (an INDIRECT edge) -> wgpu_init -> gfx_webgpu_bringup -> WGPU_COMPAT_
        #     WAIT (emscripten_sleep, requestAdapter/requestDevice). gfx_init OWNS
        #     that indirect edge, so it MUST be ADDed (main too: at -O2 gfx_init
        #     inlines into main, moving the edge there). wgpu_init + gfx_webgpu_
        #     bringup are the leaves.
        #   * EEPROM save: eeprom_store -> fclose -> __wasi_fd_sync (a default
        #     emscripten suspend import); its DIRECT chain auto-instruments.
        # If optimization changes which functions survive, ASYNCIFY_ADVISE and
        # the browser smoke gate must establish the new minimal roots explicitly.
        # The frame-dump readback (wgpu buffer-map) is DELIBERATELY excluded: it
        # is gated off in the web build (no --dump-frames), so its indirect caller
        # is never bridged and the chain never fires.
        "-sASYNCIFY"
        "-sASYNCIFY_STACK_SIZE=65536"
        "-sASYNCIFY_IGNORE_INDIRECT"
        "-sASYNCIFY_ADD=main,wgpu_init,osRecvMesg"
        # --- Memory -------------------------------------------------------
        # Real high-water is ~40 MB (12 MB ROM + 16 MB arena + audio heaps + 2 MB
        # stack + malloc/GPU staging). 128 MiB initial is ~3x headroom; growth
        # absorbs any overflow up to the 512 MiB ceiling.
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=134217728"      # 128 MiB
        "-sMAXIMUM_MEMORY=536870912"      # 512 MiB ceiling
        "-sSTACK_SIZE=2097152"            # 2 MiB (DKR's deep boot/DL recursion)
        # --- Module / runtime --------------------------------------------
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createMDKR64"
        "-sINVOKE_RUN=0"                  # shell seeds the ROM into FS, then callMain
        "-sEXIT_RUNTIME=0"
        # The committed shell sets MDKR_TRACE from ?trace= and the browser
        # regression installs deterministic MDKR_* controls in preRun. Without
        # exporting ENV those writes target an absent Module property and
        # silently do nothing even though getenv() is linked into the wasm.
        "-sEXPORTED_RUNTIME_METHODS=callMain,FS,IDBFS,ENV"
        "-lidbfs.js"                      # IDBFS for persistent saves
        "-sENVIRONMENT=web"
        # --- Crash symbolication -----------------------------------------
        # A release wasm has no name section, so a browser stack trace is bare code
        # offsets ("mdkr64_web.wasm:0xa50b7"). The first crash report from a player
        # was exactly that, and it could not be symbolised after the fact: a rebuild
        # with --profiling-funcs changes codegen outright (1305 of 1399 function
        # bodies changed size), and even a rebuild with --emit-symbol-map alone did
        # not reproduce the shipped module byte-for-byte, so no after-the-fact
        # mapping is trustworthy. The map has to come from the SAME link as the wasm
        # that ships. It is a plain index->name side table -- no ROM data, no game
        # data -- and is staged next to the wasm by tools/web/build_web.sh.
        "--emit-symbol-map"
        # SDL2's message-box ASM_CONST breaks --closure; keep it off (matches mgb64).
        "$<$<CONFIG:Debug>:-sASSERTIONS=2>"
        "$<$<CONFIG:Debug>:-sSAFE_HEAP=1>"   # ILP32 OOB/misalign tool for the wasm32 decomp
    )
    # Output the engine as mdkr64_web.{js,wasm} (MODULARIZE emits a factory in
    # the .js). The dist/web shell (index.html) loads it.
    set_target_properties(${MDKR_TARGET} PROPERTIES SUFFIX ".js")
endif()

# ==========================================================================
# --- MinGW / Windows (PK-4/PK-5/PK-6) -------------------------------------
# Every Windows-specific CMake decision lives in this ONE block, deliberately
# appended at the end of the file: the Windows lane is additive (it never
# rewrites a decision made above) and a single trailing block merges cleanly
# against concurrent edits elsewhere in this file.
#
# Windows is built by cross-compiling from macOS/Linux with mingw-w64
# (tools/mingw_cross_check.sh + cmake/mingw-w64-x86_64.cmake) and, on CI, by
# MSYS2 mingw-w64 on windows-latest. MSVC is NOT supported: the decomp needs
# GCC/Clang extensions (statement expressions, -fms-extensions anonymous
# members, -fcommon tentative definitions) that MSVC's C front end lacks.
# ==========================================================================
if(WIN32)
    if(MSVC)
        message(FATAL_ERROR
            "MSVC is not supported. Build Windows with mingw-w64 (MSYS2 "
            "MinGW 64-bit natively, or tools/mingw_cross_check.sh to cross-"
            "compile from macOS/Linux).")
    endif()

    # --- Struct layout -----------------------------------------------------
    # MinGW/GCC on Windows DEFAULTS to -mms-bitfields (MSVC-compatible bitfield
    # packing), which lays bitfields out differently from the SysV/GCC packing
    # every other platform in this port uses. The decomp's structs mirror N64
    # memory layouts and are read/written as raw ROM images, so an MSVC-packed
    # bitfield is silent data corruption, not a portability nit. Force the GCC
    # packing so Windows agrees with macOS/Linux/wasm byte-for-byte.
    target_compile_options(${MDKR_TARGET} PRIVATE -mno-ms-bitfields)
    # The decomp uses anonymous struct/union members (an MS extension GCC
    # accepts only with this flag); the Clang branch above enables it already.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU")
        target_compile_options(${MDKR_TARGET} PRIVATE -fms-extensions)
    endif()
    # The ROM-free tests that compile decomp structs must be built with the
    # SAME packing as the engine, or they measure a layout that never ships:
    # under MinGW's default they would assert against MSVC-packed bitfields and
    # either fail spuriously or, worse, agree with the wrong thing.
    foreach(_mdkr_layout_test
            mdkr_object_layout_test
            mdkr_memory_allocator_test
            mdkr_sprite_layout_test
            mdkr_runtime_contracts_test)
        if(TARGET ${_mdkr_layout_test})
            target_compile_options(${_mdkr_layout_test} PRIVATE -mno-ms-bitfields)
            if(CMAKE_C_COMPILER_ID MATCHES "GNU")
                target_compile_options(${_mdkr_layout_test} PRIVATE -fms-extensions)
            endif()
        endif()
    endforeach()

    # --- Subsystem ---------------------------------------------------------
    # Explorer double-click and game launchers start the exe with no inherited
    # console, so a console-subsystem build pops an empty console window on
    # every launch. Default to the GUI subsystem; MDKR_WIN_CONSOLE=ON restores
    # the console for debugging (live stdout without shell redirection).
    # SDL_MAIN_HANDLED is already defined above, so MinGW keeps our int main()
    # under -mwindows and no WinMain is needed.
    option(MDKR_WIN_CONSOLE "Windows: link the console subsystem (default: GUI, -mwindows)" OFF)
    if(NOT MDKR_WIN_CONSOLE)
        target_link_options(${MDKR_TARGET} PRIVATE -mwindows)
    endif()

    # --- Self-contained exe ------------------------------------------------
    # A MinGW-built exe otherwise imports libgcc_s_seh-1.dll / libwinpthread-1
    # .dll, which exist on the BUILD machine's toolchain but not on stock
    # Windows ("The code execution cannot proceed because ... was not found").
    # winpthread is bracketed -Bstatic/-Bdynamic at the end of the link line so
    # ld takes the static archive over any import lib mentioned earlier.
    #
    # SDL2 is linked STATICALLY on Windows. The shipped-beside model produced
    # exactly the failure it exists to prevent — "SDL2.dll was not found" the
    # first time a player ran the exe from Explorer's zip preview, where
    # Windows extracts only the double-clicked file to %TEMP%. One file, zero
    # bundled DLLs, kills the whole class: the exe now imports only Windows
    # system DLLs (dinput8/winmm/imm32/... from SDL2's Libs.private are all
    # inbox since XP; the windows-validate allowlist gains dinput8
    # deliberately). The static archive comes from the same pinned SDL2 the
    # cross-deps prefix builds, so behavior is unchanged.
    # The static archive's undefined system symbols (Libs.private). pkg-config
    # populates SDL2_STATIC_LIBRARIES with the full closure; filter SDL2 itself
    # out (already linked, bracketed, above) so it is not re-mentioned outside
    # the bracket where ld would resolve it to the import lib.
    set(MDKR_SDL2_STATIC_TAIL ${SDL2_STATIC_LIBRARIES})
    list(REMOVE_ITEM MDKR_SDL2_STATIC_TAIL SDL2 SDL2main mingw32)
    target_link_libraries(${MDKR_TARGET} PRIVATE ${MDKR_SDL2_STATIC_TAIL})
    #
    # -static-libstdc++ is required as soon as MDKR_APP is on: the app shell and
    # ImGui are C++, which otherwise imports libstdc++-6.dll — caught by the
    # import-table allowlist in .github/workflows/windows-validate.yml, which is
    # exactly what that gate is for. It is unconditional rather than inside
    # if(MDKR_APP) because it is harmless in a C-only link and this way the C++
    # dependency cannot silently reappear if the option is toggled.
    target_link_options(${MDKR_TARGET} PRIVATE -static-libgcc -static-libstdc++)
    target_link_libraries(${MDKR_TARGET} PRIVATE
        "-Wl,-Bstatic" winpthread "-Wl,-Bdynamic")

    # --- Icon + version resource (PK-6) ------------------------------------
    # windows/mdkr64.rc embeds the app icon and a VERSIONINFO block (what
    # Explorer's Properties>Details and Windows game launchers read). The .ico
    # is generated from dist/web/assets/favicon.png by tools/wrap_png_as_ico.sh
    # and committed.
    #
    # MDKR_VERSION is the shared release value declared above: the CLI, app
    # shell, macOS plist, and this Windows VERSIONINFO resource must agree.
    if(EXISTS "${CMAKE_SOURCE_DIR}/windows/mdkr64.ico")
        enable_language(RC)
        set(MDKR_VER_STR "${MDKR_VERSION}")
        if(MDKR_VER_STR STREQUAL "" OR MDKR_VER_STR STREQUAL "0.0.0")
            set(MDKR_VER_STR "0.0.0-dev")
        endif()
        # Numeric core = the leading dotted digits before any pre-release
        # suffix, padded to the 4-part FILEVERSION shape (major,minor,patch,build).
        string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" _mdkr_num "${MDKR_VER_STR}")
        if(_mdkr_num STREQUAL "")
            set(_mdkr_num "0")
        endif()
        string(REPLACE "." ";" _mdkr_parts "${_mdkr_num}")
        list(LENGTH _mdkr_parts _mdkr_n)
        while(_mdkr_n LESS 4)
            list(APPEND _mdkr_parts 0)
            list(LENGTH _mdkr_parts _mdkr_n)
        endwhile()
        list(GET _mdkr_parts 0 1 2 3 _mdkr_csv4)
        string(REPLACE ";" "," MDKR_VER_CSV "${_mdkr_csv4}")
        string(REPLACE "," "." MDKR_MANIFEST_VERSION "${MDKR_VER_CSV}")
        set(MDKR_MANIFEST_PATH
            "${CMAKE_CURRENT_BINARY_DIR}/GoldenBalloon.manifest")
        configure_file(
            "${CMAKE_SOURCE_DIR}/windows/GoldenBalloon.manifest.in"
            "${MDKR_MANIFEST_PATH}"
            @ONLY)
        # Keep the generated RC beside its manifest so windres resolves the
        # resource filename relative to the build input. This avoids relying
        # on quoted absolute paths surviving CMake -> shell -> windres.
        file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/windows/mdkr64.ico"
             MDKR_ICON_RC_PATH)
        set(MDKR_RC_PATH "${CMAKE_CURRENT_BINARY_DIR}/mdkr64.rc")
        configure_file(
            "${CMAKE_SOURCE_DIR}/windows/mdkr64.rc.in"
            "${MDKR_RC_PATH}"
            @ONLY)
        message(STATUS "mdkr64: Windows VERSIONINFO ${MDKR_VER_STR} (${MDKR_VER_CSV})")
        target_sources(${MDKR_TARGET} PRIVATE ${MDKR_RC_PATH})
        # Scoped to the .rc so the version defines never reach the C compile of
        # the engine. No embedded quotes: mdkr64.rc stringizes MDKR_VER_STR.
        #
        # OBJECT_DEPENDS is what makes the manifest a real input: the .rc names
        # GoldenBalloon.manifest as a resource filename, which no dependency
        # scanner sees, so an incremental build that regenerated only the
        # manifest would otherwise keep linking the previous resource object
        # and ship a stale assembly identity / longPathAware / activeCodePage
        # block.
        set_source_files_properties(${MDKR_RC_PATH} PROPERTIES
            COMPILE_DEFINITIONS "MDKR_VER_CSV=${MDKR_VER_CSV};MDKR_VER_STR=${MDKR_VER_STR}"
            OBJECT_DEPENDS "${MDKR_MANIFEST_PATH}")
    endif()
endif()
