cmake_minimum_required(VERSION 3.16)
project(mdkr64 C)
include(CTest)
option(MDKR_BUILD_FUZZERS "Build coverage-guided save-format fuzzers" OFF)

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

# --- ROM-free unit tests ---------------------------------------------------
# Keep format-boundary and display math independently runnable: these regressions
# should fail in milliseconds without booting SDL, WebGPU or copyrighted assets.
if(BUILD_TESTING AND NOT EMSCRIPTEN)
    add_executable(mdkr_display_config_test
        ${CMAKE_SOURCE_DIR}/tests/test_display_config.c
        ${CMAKE_SOURCE_DIR}/platform/display_config.c)
    target_include_directories(mdkr_display_config_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_display_config_test PRIVATE m)
    endif()
    add_test(NAME display_config COMMAND mdkr_display_config_test)

    add_executable(mdkr_endian_utils_test
        ${CMAKE_SOURCE_DIR}/tests/test_endian_utils.c)
    target_include_directories(mdkr_endian_utils_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_endian_utils_test PRIVATE NATIVE_PORT=1)
    if(NOT MSVC)
        target_link_libraries(mdkr_endian_utils_test PRIVATE m)
    endif()
    add_test(NAME endian_utils COMMAND mdkr_endian_utils_test)

    add_executable(mdkr_magic_codes_test
        ${CMAKE_SOURCE_DIR}/tests/test_magic_codes.c
        ${CMAKE_SOURCE_DIR}/platform/asset_swap.c)
    target_include_directories(mdkr_magic_codes_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_magic_codes_test PRIVATE VERSION_us_v80)
    add_test(NAME magic_codes COMMAND mdkr_magic_codes_test)

    add_executable(mdkr_object_layout_test
        ${CMAKE_SOURCE_DIR}/tests/test_object_layout.c
        ${CMAKE_SOURCE_DIR}/game/src/object_layout.c)
    target_include_directories(mdkr_object_layout_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/game/libultra
        ${CMAKE_SOURCE_DIR}/game/libultra/src/audio
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_object_layout_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(mdkr_object_layout_test PRIVATE -fms-extensions)
    endif()
    if(MDKR_EXTRA_C_FLAGS)
        separate_arguments(
            MDKR_OBJECT_LAYOUT_TEST_FLAGS
            NATIVE_COMMAND
            "${MDKR_EXTRA_C_FLAGS}")
        target_compile_options(
            mdkr_object_layout_test
            PRIVATE
            ${MDKR_OBJECT_LAYOUT_TEST_FLAGS})
    endif()
    add_test(NAME object_layout COMMAND mdkr_object_layout_test)

    add_executable(mdkr_memory_allocator_test
        ${CMAKE_SOURCE_DIR}/tests/test_memory_allocator.c
        ${CMAKE_SOURCE_DIR}/game/src/memory.c)
    target_include_directories(mdkr_memory_allocator_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/game/libultra
        ${CMAKE_SOURCE_DIR}/game/libultra/src/audio
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_memory_allocator_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1
        MDKR_MEMORY_UNIT_TEST=1)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(mdkr_memory_allocator_test PRIVATE -fms-extensions)
    endif()
    if(MDKR_EXTRA_C_FLAGS)
        target_compile_options(
            mdkr_memory_allocator_test
            PRIVATE
            ${MDKR_OBJECT_LAYOUT_TEST_FLAGS})
    endif()
    add_test(NAME memory_allocator COMMAND mdkr_memory_allocator_test)

    add_executable(mdkr_gfx_ptr_registry_test
        ${CMAKE_SOURCE_DIR}/tests/test_gfx_ptr_registry.c
        ${CMAKE_SOURCE_DIR}/platform/gfx_ptr.c)
    target_include_directories(mdkr_gfx_ptr_registry_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME gfx_ptr_registry COMMAND mdkr_gfx_ptr_registry_test)

    add_executable(mdkr_runtime_contracts_test
        ${CMAKE_SOURCE_DIR}/tests/test_runtime_contracts.c
        ${CMAKE_SOURCE_DIR}/game/src/runtime_contracts.c)
    target_include_directories(mdkr_runtime_contracts_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR)
    target_compile_definitions(mdkr_runtime_contracts_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1)
    if(NOT MSVC)
        target_link_libraries(mdkr_runtime_contracts_test PRIVATE m)
    endif()
    add_test(NAME runtime_contracts COMMAND mdkr_runtime_contracts_test)

    add_executable(mdkr_video_config_test
        ${CMAKE_SOURCE_DIR}/tests/test_video_config.c
        ${CMAKE_SOURCE_DIR}/platform/video_config.c
        ${CMAKE_SOURCE_DIR}/platform/pacing_policy.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c)
    target_include_directories(mdkr_video_config_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_video_config_test PRIVATE m)
    endif()
    add_test(NAME video_config COMMAND mdkr_video_config_test)

    add_executable(mdkr_video_config_runtime_test
        ${CMAKE_SOURCE_DIR}/tests/test_video_config_runtime.c
        ${CMAKE_SOURCE_DIR}/platform/video_config.c
        ${CMAKE_SOURCE_DIR}/platform/pacing_policy.c
        ${CMAKE_SOURCE_DIR}/platform/video_config_runtime.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c
        # publish() asks the cascade planner for the 1P shadow budget instead of
        # republishing a hardcoded resolution nothing read. gfx_shadow_cascade.c
        # is pure (no globals, no GPU), so the ROM-free seam survives.
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_shadow_cascade.c)
    target_include_directories(mdkr_video_config_runtime_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_video_config_runtime_test PRIVATE m)
    endif()
    add_test(NAME video_config_runtime COMMAND mdkr_video_config_runtime_test)

    add_executable(mdkr_pacing_policy_test
        ${CMAKE_SOURCE_DIR}/tests/test_pacing_policy.c
        ${CMAKE_SOURCE_DIR}/platform/pacing_policy.c)
    target_include_directories(mdkr_pacing_policy_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME pacing_policy COMMAND mdkr_pacing_policy_test)

    add_executable(mdkr_render_scale_test
        ${CMAKE_SOURCE_DIR}/tests/test_render_scale.c)
    target_include_directories(mdkr_render_scale_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d)
    if(NOT MSVC)
        target_link_libraries(mdkr_render_scale_test PRIVATE m)
    endif()
    add_test(NAME render_scale COMMAND mdkr_render_scale_test)

    add_executable(mdkr_mip_chain_test
        ${CMAKE_SOURCE_DIR}/tests/test_mip_chain.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_mipgen.c)
    target_include_directories(mdkr_mip_chain_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_mip_chain_test PRIVATE m)
    endif()
    add_test(NAME mip_chain COMMAND mdkr_mip_chain_test)

    add_executable(mdkr_sprite_layout_test
        ${CMAKE_SOURCE_DIR}/tests/test_sprite_layout.c
        ${CMAKE_SOURCE_DIR}/game/src/sprite_layout.c)
    target_include_directories(mdkr_sprite_layout_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/game/libultra
        ${CMAKE_SOURCE_DIR}/game/libultra/src/audio
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_sprite_layout_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(mdkr_sprite_layout_test PRIVATE -fms-extensions)
    endif()
    add_test(NAME sprite_layout COMMAND mdkr_sprite_layout_test)

    add_executable(mdkr_rdp_interpolation_test
        ${CMAKE_SOURCE_DIR}/tests/test_rdp_interpolation.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_rdp_interpolation.c)
    target_include_directories(mdkr_rdp_interpolation_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_rdp_interpolation_test PRIVATE m)
    endif()
    add_test(NAME rdp_interpolation COMMAND mdkr_rdp_interpolation_test)

    add_executable(mdkr_rl1_experiment_test
        ${CMAKE_SOURCE_DIR}/tests/test_rl1_experiment.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_rl1_experiment.c)
    target_include_directories(mdkr_rl1_experiment_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d)
    if(NOT MSVC)
        target_link_libraries(mdkr_rl1_experiment_test PRIVATE m)
    endif()
    add_test(NAME rl1_experiment COMMAND mdkr_rl1_experiment_test)

    add_executable(mdkr_level_lighting_test
        ${CMAKE_SOURCE_DIR}/tests/test_level_lighting.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_level_lighting.c)
    target_include_directories(mdkr_level_lighting_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d)
    if(NOT MSVC)
        target_link_libraries(mdkr_level_lighting_test PRIVATE m)
    endif()
    add_test(NAME level_lighting COMMAND mdkr_level_lighting_test)

    add_executable(mdkr_shadow_frame_test
        ${CMAKE_SOURCE_DIR}/tests/test_shadow_frame.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_shadow_frame.c)
    target_include_directories(mdkr_shadow_frame_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d)
    add_test(NAME shadow_frame COMMAND mdkr_shadow_frame_test)

    add_executable(mdkr_sim_sched_test
        ${CMAKE_SOURCE_DIR}/tests/test_sim_sched.c
        ${CMAKE_SOURCE_DIR}/platform/sim_sched.c)
    target_include_directories(mdkr_sim_sched_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME sim_sched COMMAND mdkr_sim_sched_test)

    add_executable(mdkr_host_frame_driver_test
        ${CMAKE_SOURCE_DIR}/tests/test_host_frame_driver.c
        ${CMAKE_SOURCE_DIR}/platform/host_frame_driver.c
        ${CMAKE_SOURCE_DIR}/platform/sim_sched.c)
    target_include_directories(mdkr_host_frame_driver_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME host_frame_driver COMMAND mdkr_host_frame_driver_test)

    add_executable(mdkr_input_tick_queue_test
        ${CMAKE_SOURCE_DIR}/tests/test_input_tick_queue.c
        ${CMAKE_SOURCE_DIR}/platform/input_tick_queue.c)
    target_include_directories(mdkr_input_tick_queue_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME input_tick_queue COMMAND mdkr_input_tick_queue_test)

    add_executable(mdkr_audio_service_clock_test
        ${CMAKE_SOURCE_DIR}/tests/test_audio_service_clock.c
        ${CMAKE_SOURCE_DIR}/platform/audio_service_clock.c)
    target_include_directories(mdkr_audio_service_clock_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME audio_service_clock COMMAND mdkr_audio_service_clock_test)

    add_executable(mdkr_audio_queue_controller_test
        ${CMAKE_SOURCE_DIR}/tests/test_audio_queue_controller.c
        ${CMAKE_SOURCE_DIR}/platform/audio_queue_controller.c)
    target_include_directories(mdkr_audio_queue_controller_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME audio_queue_controller
        COMMAND mdkr_audio_queue_controller_test)

    add_executable(mdkr_presentation_snapshot_test
        ${CMAKE_SOURCE_DIR}/tests/test_presentation_snapshot.c
        ${CMAKE_SOURCE_DIR}/platform/presentation_snapshot.c)
    target_include_directories(mdkr_presentation_snapshot_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME presentation_snapshot
        COMMAND mdkr_presentation_snapshot_test)

    add_executable(mdkr_presentation_packet_test
        ${CMAKE_SOURCE_DIR}/tests/test_presentation_packet.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_presentation_packet.c)
    target_include_directories(mdkr_presentation_packet_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d)
    add_test(NAME presentation_packet COMMAND mdkr_presentation_packet_test)

    add_executable(mdkr_shadow_cascade_test
        ${CMAKE_SOURCE_DIR}/tests/test_shadow_cascade.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_shadow_cascade.c)
    target_include_directories(mdkr_shadow_cascade_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d)
    if(NOT MSVC)
        target_link_libraries(mdkr_shadow_cascade_test PRIVATE m)
    endif()
    add_test(NAME shadow_cascade COMMAND mdkr_shadow_cascade_test)

    add_executable(mdkr_font_registry_test
        ${CMAKE_SOURCE_DIR}/tests/test_font_registry.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_font_registry.c)
    target_include_directories(mdkr_font_registry_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME font_registry COMMAND mdkr_font_registry_test)

    add_executable(mdkr_font_sdf_test
        ${CMAKE_SOURCE_DIR}/tests/test_font_sdf.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_font_sdf.c)
    target_include_directories(mdkr_font_sdf_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_font_sdf_test PRIVATE m)
    endif()
    add_test(NAME font_sdf COMMAND mdkr_font_sdf_test)

    add_executable(mdkr_sha256_test
        ${CMAKE_SOURCE_DIR}/tests/test_sha256.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_sha256_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME sha256 COMMAND mdkr_sha256_test)

    add_executable(mdkr_save_codec_test
        ${CMAKE_SOURCE_DIR}/tests/test_save_codec.c
        ${CMAKE_SOURCE_DIR}/platform/save_codec.c)
    target_include_directories(mdkr_save_codec_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME save_codec COMMAND mdkr_save_codec_test)

    add_executable(mdkr_save_container_test
        ${CMAKE_SOURCE_DIR}/tests/test_save_container.c
        ${CMAKE_SOURCE_DIR}/platform/save_container.c
        ${CMAKE_SOURCE_DIR}/platform/save_codec.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_save_container_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME save_container COMMAND mdkr_save_container_test)

    add_executable(mdkr_save_tools_core_test
        ${CMAKE_SOURCE_DIR}/tests/test_save_tools_core.c
        ${CMAKE_SOURCE_DIR}/platform/save_tools_core.c
        ${CMAKE_SOURCE_DIR}/platform/save_codec.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_save_tools_core_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME save_tools_core COMMAND mdkr_save_tools_core_test)

    add_executable(mdkr_virtual_pak_test
        ${CMAKE_SOURCE_DIR}/tests/test_virtual_pak.c
        ${CMAKE_SOURCE_DIR}/platform/virtual_pak.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_virtual_pak_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_options(mdkr_virtual_pak_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME virtual_pak COMMAND mdkr_virtual_pak_test)

    add_executable(mdkr_webgpu_fault_test
        ${CMAKE_SOURCE_DIR}/tests/test_webgpu_fault.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_fault.c)
    target_include_directories(mdkr_webgpu_fault_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d)
    if(MSVC)
        target_compile_options(mdkr_webgpu_fault_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_webgpu_fault_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME webgpu_fault COMMAND mdkr_webgpu_fault_test)

    add_executable(mdkr_webgpu_lifecycle_test
        ${CMAKE_SOURCE_DIR}/tests/test_webgpu_lifecycle.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_lifecycle.c)
    target_include_directories(mdkr_webgpu_lifecycle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME webgpu_lifecycle COMMAND mdkr_webgpu_lifecycle_test)
    add_test(NAME webgpu_artifacts
        COMMAND ${CMAKE_COMMAND} -P
                ${CMAKE_SOURCE_DIR}/tests/check_webgpu_artifacts.cmake)
endif()

# Repository-publication policy is backend-independent and must run in every
# native CTest configuration, including OpenGL-only and sanitizer lanes.
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME public_surface
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_public_surface.py)
    add_test(
        NAME oracle_reference_replay
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_oracle_reference_replay.py)
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

# SDL queue-mode contract: still ROM-free, but deliberately placed after the
# dependency finder. CI selects SDL's silent dummy driver; running the same
# executable without that override is an inaudible physical-device witness.
if(BUILD_TESTING AND NOT EMSCRIPTEN)
    add_executable(mdkr_audio_sink_contract_test
        ${CMAKE_SOURCE_DIR}/tests/test_audio_sink_contract.c
        ${CMAKE_SOURCE_DIR}/platform/audio_queue_controller.c)
    target_include_directories(mdkr_audio_sink_contract_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${SDL2_INCLUDE_DIRS})
    target_link_directories(mdkr_audio_sink_contract_test PRIVATE
        ${SDL2_LIBRARY_DIRS})
    target_compile_definitions(mdkr_audio_sink_contract_test PRIVATE
        SDL_MAIN_HANDLED)
    target_link_libraries(mdkr_audio_sink_contract_test PRIVATE
        ${SDL2_LIBRARIES})
    add_test(NAME audio_sink_contract
        COMMAND mdkr_audio_sink_contract_test
                --duration-ms 750
                --require-driver dummy)
    set_tests_properties(audio_sink_contract PROPERTIES
        ENVIRONMENT "SDL_AUDIODRIVER=dummy")
endif()

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
# WebGPU (wgpu-native) is selectable on native and is the ONLY in-browser
# backend (M8); GL is the native throughput default and guaranteed fallback.
# Under Emscripten the code is ALWAYS on (there is no GL fallback in the
# browser) and cmake/webgpu.cmake returns an INTERFACE `webgpu` target wired to
# `--use-port=emdawnwebgpu` (Dawn's webgpu.h — same C API as wgpu-native, so the
# vendored gfx_webgpu.c compiles unchanged). Set -DMDKR_WEBGPU_BACKEND=OFF for a
# native GL-only binary with no wgpu dep. Runtime selection: MDKR_RENDERER.
option(MDKR_WEBGPU_BACKEND "Build the selectable native WebGPU render backend" ON)
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
# math_util.c + collision.c; hasm_native/ (written by another agent) is globbed
# so its C hand-asm replacements auto-join when they appear. The .s files and
# the hasm/ido/ alternate sources are intentionally NOT globbed.
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
#
# host_window.c is the deliberate exception: it still sits here uncompiled
# because gfx_webgpu.c really does call platformHasHostWebGpu() /
# platformHostWgpuDevice(). Those symbols are satisfied by the inert
# gfx_webgpu_stubs.c below, so host_window.c and gfx_webgpu_stubs.c are mutually
# exclusive — adding host_window.c to this list without dropping the stubs is a
# duplicate-symbol link error, not a merge.
set(PLATFORM_SOURCES
    ${CMAKE_SOURCE_DIR}/platform/main_pc.c
    ${CMAKE_SOURCE_DIR}/platform/display_config.c
    ${CMAKE_SOURCE_DIR}/platform/config_ini.c       # INI engine (game-agnostic)
    ${CMAKE_SOURCE_DIR}/platform/video_config.c     # presentation modes (pure core)
    ${CMAKE_SOURCE_DIR}/platform/video_config_runtime.c  # ini/env/argv + g_pc* publication
    ${CMAKE_SOURCE_DIR}/platform/pacing_policy.c    # original/enhanced simulation cadence
    ${CMAKE_SOURCE_DIR}/platform/sim_sched.c        # exact fixed-authority accumulator
    ${CMAKE_SOURCE_DIR}/platform/host_frame_driver.c # exact host-time -> fixed tick tickets
    ${CMAKE_SOURCE_DIR}/platform/input_tick_queue.c  # bounded host edges -> tick pad state
    ${CMAKE_SOURCE_DIR}/platform/audio_service_clock.c # host audio time -> bounded PCM debt
    ${CMAKE_SOURCE_DIR}/platform/audio_queue_controller.c # sink drain -> bounded PCM block
    ${CMAKE_SOURCE_DIR}/platform/present_sched.c    # its retrace-branch wiring (Phase 3 Wave B)
    ${CMAKE_SOURCE_DIR}/platform/sim_hash.c         # authoritative-state hash (Phase 1)
    ${CMAKE_SOURCE_DIR}/platform/gameplay_event_trace.c # ordered transient authority events
    ${CMAKE_SOURCE_DIR}/platform/input_consumption_trace.c # tick-consumed pad trace
    ${CMAKE_SOURCE_DIR}/platform/presentation_snapshot.c       # presentation shadow state (Phase 3 Wave A)
    ${CMAKE_SOURCE_DIR}/platform/presentation_snapshot_walk.c  # its gObjPtrList/camera capture wiring
    ${CMAKE_SOURCE_DIR}/platform/platform_sdl_min.c
    ${CMAKE_SOURCE_DIR}/platform/stubs_dkr.c
    ${CMAKE_SOURCE_DIR}/platform/rom_io.c
    ${CMAKE_SOURCE_DIR}/platform/rom_id.c          # which DKR revision is this?
    ${CMAKE_SOURCE_DIR}/platform/sha256.c
    ${CMAKE_SOURCE_DIR}/platform/save_codec.c
    ${CMAKE_SOURCE_DIR}/platform/virtual_pak.c
    ${CMAKE_SOURCE_DIR}/platform/segment_consts.c
    ${CMAKE_SOURCE_DIR}/platform/gu_port.c
    ${CMAKE_SOURCE_DIR}/platform/asset_swap.c
    ${CMAKE_SOURCE_DIR}/platform/gfx_ptr.c          # registry globals (PLAN #3)
    ${CMAKE_SOURCE_DIR}/platform/gfx_config_shim.c  # render/post-FX config globals (off)
    ${CMAKE_SOURCE_DIR}/platform/hasm_stubs_temp.c
    ${CMAKE_SOURCE_DIR}/platform/math_stubs_temp.c
    ${CMAKE_SOURCE_DIR}/platform/mixer.c            # M5: software aspMain audio mixer
    ${CMAKE_SOURCE_DIR}/platform/audi_port_dkr.c    # audio-time service + SDL/worklet sinks
    ${CMAKE_SOURCE_DIR}/platform/audio_compat.c     # clean-room synth: banks, filters, CSP player
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
# HLE) + the vendored mgb64 backends. The *.c glob excludes gfx_metal.mm
# (Obj-C++, Metal-only) and screenshot_series.c (unused capture helper). The
# WebGPU backend TUs are added back conditionally (they need the wgpu include +
# MDKR_WEBGPU_BACKEND). Under Emscripten the GL backend (gfx_opengl.c) is dropped
# — the wasm binary renders exclusively through WebGPU.
file(GLOB FAST3D_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/platform/fast3d/*.c)
list(REMOVE_ITEM FAST3D_SOURCES ${CMAKE_SOURCE_DIR}/platform/fast3d/screenshot_series.c)
list(REMOVE_ITEM FAST3D_SOURCES
    ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu.c
    ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_fault.c
    ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_shader.c)
if(EMSCRIPTEN)
    list(REMOVE_ITEM FAST3D_SOURCES ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_opengl.c)
endif()
if(MDKR_WEBGPU_BACKEND)
    list(APPEND FAST3D_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_fault.c
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
        ${CMAKE_SOURCE_DIR}/platform/host_window.c          # window/device adoption
        ${CMAKE_SOURCE_DIR}/platform/app_overlay_hooks.c)   # overlay hook dispatch
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
set(MDKR_VERSION "1.0.0" CACHE STRING "mdkr64 release version (semver)")
target_compile_definitions(${MDKR_TARGET} PRIVATE MDKR_VERSION_STRING="${MDKR_VERSION}")

# --- Flags ----------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang" OR EMSCRIPTEN)
    # Keep this before the reviewed -Werror classes below so those classes are
    # re-enabled after the umbrella suppression.
    target_compile_options(${MDKR_TARGET} PRIVATE -Wno-everything)
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
        target_link_libraries(${MDKR_TARGET} PRIVATE ${SDL2_LIBRARIES} m)
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
        set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/lib/imgui)
        set(IMGUI_SOURCES
            ${IMGUI_DIR}/imgui.cpp
            ${IMGUI_DIR}/imgui_draw.cpp
            ${IMGUI_DIR}/imgui_tables.cpp
            ${IMGUI_DIR}/imgui_widgets.cpp
            ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
            ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

        file(GLOB APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/platform/app/*.cpp)

        # Native "open file" panel, one implementation per platform. The glob
        # above would otherwise pull in every variant at once, so the ones that
        # do not belong on this platform are removed and the right one added
        # back. A user-selected file is permission-exempt, which is why this
        # replaced the directory scan that used to trip macOS TCC.
        list(REMOVE_ITEM APP_SOURCES
            ${CMAKE_SOURCE_DIR}/platform/app/file_dialog_win.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/file_dialog_stub.cpp)
        if(APPLE)
            enable_language(OBJCXX)
            list(APPEND APP_SOURCES ${CMAKE_SOURCE_DIR}/platform/app/file_dialog_mac.mm)
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
        if(APPLE)
            # AppKit: NSOpenPanel. UniformTypeIdentifiers: the UTType extension
            # filter (weak-linked; the 10.x fallback path is compiled too).
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
                ${CMAKE_SOURCE_DIR}/platform/rom_id.c)
            target_include_directories(mdkr_app_shell_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_app_shell_test PRIVATE cxx_std_17)
            add_test(NAME app_shell COMMAND mdkr_app_shell_test)

            # Schema round-trip: every key the settings panel renders must have a
            # category, a label, help text, and must resolve back by name. Runs
            # fully headless (no window, no GPU) via the shell's own self-check.
            add_test(NAME app_schema COMMAND ${MDKR_TARGET})
            set_tests_properties(app_schema PROPERTIES
                ENVIRONMENT "MDKR_APP_DUMP_SCHEMA=1;MDKR_AUDIO=0")

            # Shell render smoke: drive the launcher for a few frames and require
            # a real captured frame. This is what makes the gate honest — it
            # proves pixels reached a framebuffer, not merely that main() returned
            # 0 (main_app.cpp fails the run when a requested capture is missing).
            # Needs a GPU, so it is skipped when MDKR_SKIP_GPU_TESTS is set.
            if(NOT MDKR_SKIP_GPU_TESTS)
                add_test(NAME app_shell_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_shell_smoke PROPERTIES
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=4;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_shell_smoke.bmp;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    TIMEOUT 180)
            endif()
        endif()
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
    # MDKR_VERSION is declared here rather than at the top of the file because
    # the Windows resource is currently its only consumer — this whole block is
    # meant to stay liftable as one unit. Move it up the moment a second
    # platform needs a compiled-in version.
    if(EXISTS "${CMAKE_SOURCE_DIR}/windows/mdkr64.ico")
        enable_language(RC)
        set(MDKR_VERSION "0.0.0" CACHE STRING "Version embedded in the Windows VERSIONINFO resource")
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
        message(STATUS "mdkr64: Windows VERSIONINFO ${MDKR_VER_STR} (${MDKR_VER_CSV})")
        target_sources(${MDKR_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/windows/mdkr64.rc)
        # Scoped to the .rc so the version defines never reach the C compile of
        # the engine. No embedded quotes: mdkr64.rc stringizes MDKR_VER_STR.
        set_source_files_properties(${CMAKE_SOURCE_DIR}/windows/mdkr64.rc PROPERTIES
            COMPILE_DEFINITIONS "MDKR_VER_CSV=${MDKR_VER_CSV};MDKR_VER_STR=${MDKR_VER_STR}")
    endif()
endif()
