cmake_minimum_required(VERSION 3.16)
project(mdkr64 C)
# Editors and clangd read build-rel/compile_commands.json (see .clangd). Without
# it every include resolves against the compiler's bare default search path and
# the editor reports missing headers that the real build finds.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(CTest)
option(MDKR_BUILD_FUZZERS "Build coverage-guided save-format fuzzers" OFF)
option(MDKR_ENABLE_FUZZERS
    "Build coverage-guided untrusted-input fuzzers (Clang/libFuzzer)" OFF)
option(MDKR_ENABLE_GPU_TESTS
    "Register native GPU/window tests (dedicated test desktop only)" OFF)
set(MDKR_ONLINE_ROOM_PREVIEW_DEFAULT OFF)
if(MDKR_ENABLE_GPU_TESTS)
    set(MDKR_ONLINE_ROOM_PREVIEW_DEFAULT ON)
endif()
option(MDKR_ENABLE_ONLINE_ROOM_PREVIEW
    "Compile the deferred native Online Room preview (development tests only)"
    ${MDKR_ONLINE_ROOM_PREVIEW_DEFAULT})

# BETA-ONLY native online enablement. When ON, this composes the production live
# online transport into the launcher's Online Room panel and opens the build gate
# that lets two native instances pair + race over the live MatchRoom service,
# fenced to 2 endpoints / STUN-only / retail identities. Modeled on the
# MDKR_NATIVE_PHONE_PARTY option below. It defaults OFF for a developer build
# and is set by .github/workflows/release.yml only for its explicit release
# builds; macos/Scripts/build_app_bundle.sh passes it only when a release
# explicitly opts in with --allow-online-beta. The release guardrails
# (check_release_local_only_surface.py, check_release_party_origin.py) require
# that explicit beta opt-in, keep the bare preview switch off, and independently
# preserve the browser publication's local-only boundary.
option(MDKR_ENABLE_ONLINE_BETA
    "Compose the live online transport into the launcher behind the native beta gate; OFF by default, release bundles pass this explicitly"
    OFF)
if(MDKR_ENABLE_ONLINE_BETA)
    # The Online Room panel and its live-adapter seam only compile under the
    # preview flag, so the beta forces it ON regardless of the default above.
    set(MDKR_ENABLE_ONLINE_ROOM_PREVIEW ON)
endif()

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
            # Registration is not runtime authorization. In particular, an old
            # build tree can retain this inventory after the developer returns
            # to an occupied desktop. The invoking environment must carry both
            # runtime capabilities; never mint either one from CMake state.
            set_tests_properties("${MDKR_CTEST_NAME}" PROPERTIES
                RESOURCE_LOCK mdkr_native_gpu)
        endif()
    endforeach()
endfunction()

# The translated game includes its legacy PR headers by basename (for example
# "sched.h"), while host SDK headers include POSIX headers with the same names
# using angle brackets. Keep those two namespaces distinct on compilers that
# support quote-only search paths. In particular, glibc's <spawn.h> includes
# <sched.h>; resolving that to game/include/PR/sched.h leaves struct
# sched_param incomplete and breaks a clean Ubuntu build. MSVC has no -iquote
# equivalent and its SDK does not have this collision.
function(mdkr_add_legacy_game_quote_includes target_name)
    if(MSVC)
        target_include_directories(${target_name} PRIVATE
            ${CMAKE_SOURCE_DIR}/game/include/PR
            ${CMAKE_SOURCE_DIR}/game/include/sys)
    else()
        target_compile_options(${target_name} PRIVATE
            -iquote${CMAKE_SOURCE_DIR}/game/include/PR
            -iquote${CMAKE_SOURCE_DIR}/game/include/sys)
    endif()
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
option(MDKR_CHARACTER_KTX2
       "Enable bounded Basis Universal transcoding for custom-character KTX2" ON)
if(MDKR_CHARACTER_KTX2)
    enable_language(CXX)
    include(${CMAKE_SOURCE_DIR}/cmake/character_basisu.cmake)
endif()
option(MDKR_CHARACTER_LOD_TOOLS
       "Build the bounded offline custom-character LOD simplifier" ON)
if(MDKR_CHARACTER_LOD_TOOLS AND NOT EMSCRIPTEN)
    enable_language(CXX)
    include(${CMAKE_SOURCE_DIR}/cmake/character_meshoptimizer.cmake)
endif()
enable_language(CXX)
include(${CMAKE_SOURCE_DIR}/cmake/character_text_shaping.cmake)
# FP contraction is pinned OFF for EVERY target in this project, not just the
# engine. The per-target block below states the requirement -- gameplay float
# state must be bit-reproducible, "lockstep peers and every golden float hash
# depend on it" -- but applying it PRIVATE to ${MDKR_TARGET} left two gaps:
# mdkr64_app, and every fixture in cmake/tests.cmake (which mentions this flag
# zero times while compiling game/src/hasm/math_util.c and game/src/tracks.c in
# eight targets). A fixture that fuses a*b+c into an FMA while the engine does
# not is computing different floats from the thing it is asserting about, so a
# golden-hash gate can agree by luck and mask a real difference -- or disagree
# for a reason that has nothing to do with the code under test.
#
# Directory scope is the fix rather than a per-target repeat: it cannot be
# forgotten by the next target somebody adds. AppleClang on arm64 fuses at -O2
# by default, so this is exactly the flag that keeps an arm64 host and an
# x86_64 peer computing the same float.
add_compile_options(-ffp-contract=off)

include(${CMAKE_SOURCE_DIR}/cmake/tests.cmake)

# The custom-character cache and its embedded texture payloads are both
# untrusted local inputs. Keep one libFuzzer process crossing the exact shipped
# memory loader, PNG decoder configuration, and KTX2 bridge so mutations can
# move between the container and decoder boundaries. This is additive and never
# enters ordinary builds.
if(MDKR_ENABLE_FUZZERS AND NOT EMSCRIPTEN)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "MDKR_ENABLE_FUZZERS requires Clang/libFuzzer")
    endif()
    if(NOT MDKR_CHARACTER_KTX2 OR NOT TARGET mdkr_basisu_transcoder)
        message(FATAL_ERROR
            "MDKR_ENABLE_FUZZERS requires MDKR_CHARACTER_KTX2=ON")
    endif()
    add_executable(mdkr_modern_character_asset_fuzzer
        ${CMAKE_SOURCE_DIR}/tests/fuzz_modern_character_asset.cpp
        ${CMAKE_SOURCE_DIR}/platform/modern_character_asset.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_ktx2.cpp
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/lib/stb/stb_image_impl.c)
    target_include_directories(mdkr_modern_character_asset_fuzzer PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/lib/stb)
    target_compile_features(mdkr_modern_character_asset_fuzzer PRIVATE
        cxx_std_17)
    target_link_libraries(mdkr_modern_character_asset_fuzzer PRIVATE
        mdkr_basisu_transcoder)
    target_compile_options(mdkr_modern_character_asset_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer)
    target_link_options(mdkr_modern_character_asset_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined)
    # Instrument the third-party decoder as well as the first-party bridge in
    # this dedicated build tree; otherwise only calls around it would be seen.
    target_compile_options(mdkr_basisu_transcoder PRIVATE
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-omit-frame-pointer)
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

    # ROM-, renderer-, provider- and storage-free Online Room projection. The
    # browser launcher loads this only through its explicit preview/evidence
    # adapter while production admission is closed. Linking the same C reducers
    # as native prevents a second JavaScript room state machine.
    add_executable(mdkr_online_tools
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_browser_wasm.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_fake_adapter.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_view_model.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
        ${CMAKE_SOURCE_DIR}/platform/session/session_core.c)
    target_include_directories(mdkr_online_tools PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_options(mdkr_online_tools PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_options(mdkr_online_tools PRIVATE
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createMDKROnlineTools"
        "-sINVOKE_RUN=0"
        "-sEXIT_RUNTIME=0"
        "-sFILESYSTEM=0"
        "-sMALLOC=emmalloc"
        "-sINITIAL_MEMORY=2097152"
        "-sSTACK_SIZE=65536"
        "-sEXPORTED_FUNCTIONS=_mdkr_online_browser_version,_mdkr_online_browser_count,_mdkr_online_browser_select,_mdkr_online_browser_slug,_mdkr_online_browser_title,_mdkr_online_browser_explanation,_mdkr_online_browser_status,_mdkr_online_browser_verification_phrase,_mdkr_online_browser_timeout_title,_mdkr_online_browser_timeout_explanation,_mdkr_online_browser_kind,_mdkr_online_browser_failure,_mdkr_online_browser_announcement,_mdkr_online_browser_member_count,_mdkr_online_browser_seat_count,_mdkr_online_browser_ready_count,_mdkr_online_browser_requires_admission,_mdkr_online_browser_local_play_available,_mdkr_online_browser_timeout_visible,_mdkr_online_browser_control_action,_mdkr_online_browser_control_label,_mdkr_online_browser_control_enabled,_mdkr_online_browser_dispatch,_mdkr_online_browser_pending,_mdkr_online_browser_complete,_mdkr_online_browser_live_project"
        "-sEXPORTED_RUNTIME_METHODS=cwrap"
        "-sENVIRONMENT=web")
    set_target_properties(mdkr_online_tools PROPERTIES
        OUTPUT_NAME "mdkr-online-tools"
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

option(MDKR_NATIVE_PHONE_PARTY
       "Build the native WSS/WebRTC Phone Party transport" ON)
set(MDKR_PARTY_ORIGIN "" CACHE STRING
    "Production HTTPS origin for browser phone controllers")
# M2: the value is compiled into the launcher, interpolated into invite URLs
# and compared byte-for-byte against the service's PARTY_ORIGIN. It must be
# one canonical origin -- https:// + lowercase host + at most one explicit
# port -- and nothing else. The launcher refuses a non-canonical origin at
# runtime too (mdkr_party_canonical_https_origin,
# platform/party/native_party_host.h); failing here names the mistake at
# configure time instead of shipping a build whose Phone Party surface only
# fails closed when opened.
if(MDKR_PARTY_ORIGIN)
    if(NOT MDKR_PARTY_ORIGIN MATCHES "^https://")
        message(FATAL_ERROR "MDKR_PARTY_ORIGIN must be empty or an https://"
            " origin, got '${MDKR_PARTY_ORIGIN}'")
    endif()
    string(REGEX REPLACE "^https://" "" _mdkr_party_authority
        "${MDKR_PARTY_ORIGIN}")
    if(_mdkr_party_authority MATCHES "@")
        message(FATAL_ERROR "MDKR_PARTY_ORIGIN must not carry userinfo"
            " ('@' in '${MDKR_PARTY_ORIGIN}'): RFC 3986 would read the part"
            " after the '@' as the real host")
    endif()
    if(_mdkr_party_authority MATCHES "[/?#]")
        message(FATAL_ERROR "MDKR_PARTY_ORIGIN must be scheme + host with an"
            " optional port only; remove the path, query, fragment or"
            " trailing slash from '${MDKR_PARTY_ORIGIN}'")
    endif()
    # Six or more digits is checked textually because an absurdly long port
    # must not depend on integer-overflow behaviour in GREATER.
    if(_mdkr_party_authority MATCHES ":[0-9][0-9][0-9][0-9][0-9][0-9]"
       OR (MDKR_PARTY_ORIGIN MATCHES ":([0-9]+)$"
           AND CMAKE_MATCH_1 GREATER 65535))
        message(FATAL_ERROR "MDKR_PARTY_ORIGIN port must be 1-65535,"
            " got '${MDKR_PARTY_ORIGIN}'")
    endif()
    if(NOT MDKR_PARTY_ORIGIN MATCHES
            "^https://[a-z0-9]([a-z0-9.-]*[a-z0-9])?(:[1-9][0-9]*)?$"
       OR _mdkr_party_authority MATCHES "\\.\\.|\\.-|-\\.")
        message(FATAL_ERROR "MDKR_PARTY_ORIGIN must be a canonical origin:"
            " https:// + lowercase DNS host + optional :port (no leading-zero"
            " or empty port, no empty or '-'-edged host labels, no trailing"
            " dot), got '${MDKR_PARTY_ORIGIN}'")
    endif()
    unset(_mdkr_party_authority)
endif()
set(MDKR_PARTY_COMPILED_ORIGIN "")
if(MDKR_NATIVE_PHONE_PARTY)
    set(MDKR_PARTY_COMPILED_ORIGIN "${MDKR_PARTY_ORIGIN}")
endif()
if(MDKR_APP AND MDKR_NATIVE_PHONE_PARTY)
    include(cmake/datachannel.cmake)
    if(BUILD_TESTING AND NOT EMSCRIPTEN)
        # Actual amended vendor dispatch/transport code, not duplicate models.
        # Edge coverage starts RTC global services but no app or peer session.
        foreach(MDKR_RTC_FIXTURE datachannel_prepared_dispatch
                datachannel_transport_retirement datachannel_transport_edges)
            add_executable(mdkr_${MDKR_RTC_FIXTURE}_test
                ${CMAKE_SOURCE_DIR}/tests/test_${MDKR_RTC_FIXTURE}.cpp)
            target_include_directories(mdkr_${MDKR_RTC_FIXTURE}_test PRIVATE
                ${mdkr_libdatachannel_SOURCE_DIR}/src
                ${mdkr_libdatachannel_SOURCE_DIR}/include/rtc
                ${mdkr_libdatachannel_SOURCE_DIR}/deps/plog/include
                ${CMAKE_SOURCE_DIR}/platform/online)
            target_compile_features(mdkr_${MDKR_RTC_FIXTURE}_test PRIVATE cxx_std_17)
            target_link_libraries(mdkr_${MDKR_RTC_FIXTURE}_test PRIVATE
                datachannel Threads::Threads)
            add_test(NAME ${MDKR_RTC_FIXTURE} COMMAND mdkr_${MDKR_RTC_FIXTURE}_test)
            set_tests_properties(${MDKR_RTC_FIXTURE} PROPERTIES TIMEOUT 60)
        endforeach()
    endif()
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

# Separated online BETA boot path (game/src/online/). Deliberately NOT part of
# the game/src/*.c glob above (that glob is non-recursive), and added ONLY inside
# this beta gate -- exactly like party_link.c below -- so a normal (beta OFF)
# build never compiles a byte of it and the beta-OFF engine object is untouched.
# It joins GAME_SOURCES (not PLATFORM_SOURCES) because it is engine/game code
# that includes the decomp game headers, so it inherits the same warning regime
# every other game/src TU does. Every TU here is itself #if MDKR_ENABLE_ONLINE_BETA.
if(MDKR_ENABLE_ONLINE_BETA)
    list(APPEND GAME_SOURCES
        ${CMAKE_SOURCE_DIR}/game/src/online/online_session.c    # separated online session
        ${CMAKE_SOURCE_DIR}/game/src/online/online_character_map.c # single-source online-id -> engine Character map
        ${CMAKE_SOURCE_DIR}/game/src/online/online_screen_util.c # shared native-screen draw/state helpers
        ${CMAKE_SOURCE_DIR}/game/src/online/online_charselect.c  # native online character select
        ${CMAKE_SOURCE_DIR}/game/src/online/online_vehicleselect.c # native online vehicle select
        ${CMAKE_SOURCE_DIR}/game/src/online/online_trackselect.c # native online track/cup select
        ${CMAKE_SOURCE_DIR}/game/src/online/online_race_boot.c   # direct online race boot
        ${CMAKE_SOURCE_DIR}/game/src/online/online_results.c    # native online results/standings
        ${CMAKE_SOURCE_DIR}/game/src/online/online_ceremony.c   # native online champion ceremony
        ${CMAKE_SOURCE_DIR}/game/src/online/online_race_pause.c) # non-blocking online pause overlay
endif()

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
    ${CMAKE_SOURCE_DIR}/platform/text_state_file.c  # atomic versioned text-sidecar backend
    ${CMAKE_SOURCE_DIR}/platform/magic_codes_state.c # persistent safe retail Magic Codes
    ${CMAKE_SOURCE_DIR}/platform/magic_codes_state_file.c # desktop/Web Magic Code backend
    ${CMAKE_SOURCE_DIR}/platform/taj_mod_state.c    # versioned global Taj unlock sidecar
    ${CMAKE_SOURCE_DIR}/platform/taj_mod_state_file.c # atomic desktop/Web state backend
    # Content packs: a read-only override layer in front of the ROM asset path.
    # No pack content is shipped, hosted or tracked; these only read what a
    # player put in mods/ themselves.
    ${CMAKE_SOURCE_DIR}/platform/mod_manifest.c     # one pack's pack.ini
    ${CMAKE_SOURCE_DIR}/platform/mod_source.c       # directory/zip pack byte source
    ${CMAKE_SOURCE_DIR}/platform/mod_registry.c     # ordered set of installed packs
    ${CMAKE_SOURCE_DIR}/platform/sprite_model_mesh.c
    ${CMAKE_SOURCE_DIR}/platform/sprite_models.c
    ${CMAKE_SOURCE_DIR}/platform/mod_texture_key.c  # published texture content digest
    ${CMAKE_SOURCE_DIR}/platform/mod_texture_store.c # decoded override textures (stb PNG)
    ${CMAKE_SOURCE_DIR}/platform/rice_crc.c        # Rice/GLideN64 texture identity
    ${CMAKE_SOURCE_DIR}/platform/mod_music.c        # decoded override music (dr_wav)
    ${CMAKE_SOURCE_DIR}/platform/modern_character_asset.c # validated private .mdkc character cache
    ${CMAKE_SOURCE_DIR}/platform/modern_character_install.c # transactional portable .mdkrchar import/removal
    ${CMAKE_SOURCE_DIR}/platform/modern_character_registry.c # bounded local generic-character discovery
    ${CMAKE_SOURCE_DIR}/platform/modern_character_pose.c # semantic clips, cross-fades, sockets, skin palettes
    ${CMAKE_SOURCE_DIR}/platform/modern_character_render.c # bounded PNG decode and immutable GPU resources
    ${CMAKE_SOURCE_DIR}/platform/modern_character_draw_store.c # exact-size immutable retained palettes
    ${CMAKE_SOURCE_DIR}/platform/modern_character_identity.c # bounded identity decode and game-ready portrait resampling
    ${CMAKE_SOURCE_DIR}/platform/modern_character_text.c # shared bounded UTF-8 to retail-font identity projection
    ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_character_text.c # project-owned bounded LTR custom-name glyph renderer
    ${CMAKE_SOURCE_DIR}/platform/modern_character_runtime.c # assignments, animation instances, retained draw commands
    ${CMAKE_SOURCE_DIR}/platform/modern_character_gpu_timing.c # bounded exact Workshop GPU timestamp distributions
    ${CMAKE_SOURCE_DIR}/platform/modern_character_capture_projection.c # subject-scoped exact capture-space fit witness
    ${CMAKE_SOURCE_DIR}/platform/modern_character_surface_intersection.c # bounded posed-character/vehicle surface crossing evidence
    ${CMAKE_SOURCE_DIR}/platform/workshop_preview_runtime.c # presentation-only exact inspection view/light state
    ${CMAKE_SOURCE_DIR}/platform/modern_character_donor.c # fail-visible retail driver/vehicle separation
    ${CMAKE_SOURCE_DIR}/platform/modern_character_gameplay_profile.c # exact bounded ROM donor summaries
    ${CMAKE_SOURCE_DIR}/platform/enhancement_registry.c # enhancements + authority class
    ${CMAKE_SOURCE_DIR}/platform/enh_speedometer.c  # speed readout (presentation only)
    ${CMAKE_SOURCE_DIR}/platform/enh_draw_distance.c # draw distance + LOD bias (presentation only)
    ${CMAKE_SOURCE_DIR}/platform/enh_ai_difficulty.c # opponent skill (gameplay; authored by default)
    ${CMAKE_SOURCE_DIR}/platform/save_state.c       # practice save states (NOT the progress save)
    ${CMAKE_SOURCE_DIR}/platform/asset_subentry.c   # bounds-checked asset sub-entry access
    ${CMAKE_SOURCE_DIR}/platform/custom_track_section.c # immutable custom-track asset sessions
    ${CMAKE_SOURCE_DIR}/platform/custom_track_decode.c # bounded rzip admission decode
    ${CMAKE_SOURCE_DIR}/platform/custom_track_model.c # checked BE texture-ID relocation
    ${CMAKE_SOURCE_DIR}/platform/custom_track_header.c # BE header/lighting admission
    ${CMAKE_SOURCE_DIR}/platform/custom_track_model_payload.c # bounded private model repack
    ${CMAKE_SOURCE_DIR}/platform/custom_track_texture.c # bounded decoded/raw texture admission
    # The console command parser. It was reachable only from cmake/tests.cmake
    # until the console shipped -- a unit-tested island the engine never linked.
    ${CMAKE_SOURCE_DIR}/platform/dev_command.c      # console command parsing
    ${CMAKE_SOURCE_DIR}/platform/video_config.c     # presentation modes (pure core)
    ${CMAKE_SOURCE_DIR}/platform/video_config_runtime.c  # ini/env/argv + g_pc* publication
    ${CMAKE_SOURCE_DIR}/platform/port_env.c        # cached environment feature gates
    ${CMAKE_SOURCE_DIR}/platform/a11y_model.c       # what the shell would say, as text
    ${CMAKE_SOURCE_DIR}/platform/a11y_race.c        # race events -> the same text stream
    ${CMAKE_SOURCE_DIR}/platform/a11y_speech_worker.c # drain thread: says it, off the render thread
    ${CMAKE_SOURCE_DIR}/platform/user_paths.c       # immutable bundle vs mutable user-data policy
    ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c          # UTF-8/long-path OS filesystem boundary
    ${CMAKE_SOURCE_DIR}/platform/pacing_policy.c    # original/enhanced simulation cadence
    ${CMAKE_SOURCE_DIR}/platform/steering_compat.c  # cadence-neutral car lateral response
    ${CMAKE_SOURCE_DIR}/platform/hud_layout.c       # presentation-only wide HUD anchoring
    ${CMAKE_SOURCE_DIR}/platform/sim_sched.c        # exact fixed-authority accumulator
    ${CMAKE_SOURCE_DIR}/platform/host_frame_driver.c # exact host-time -> fixed tick tickets
    ${CMAKE_SOURCE_DIR}/platform/input_tick_queue.c  # bounded host edges -> tick pad state
    ${CMAKE_SOURCE_DIR}/platform/pad_router.c        # one-owner local/remote controller ports
    ${CMAKE_SOURCE_DIR}/platform/party/party_protocol.c # fixed, validated Phone Party wire codec
    ${CMAKE_SOURCE_DIR}/platform/party/remote_pad.c  # bounded history/dedupe/timeout core
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
    ${CMAKE_SOURCE_DIR}/platform/gpu_diagnostics.c # exact Workshop comparison device identity
    ${CMAKE_SOURCE_DIR}/platform/input_consumption_trace.c # tick-consumed pad trace
    ${CMAKE_SOURCE_DIR}/platform/input_latency_census.c # input -> commit -> present budget (M4)
    ${CMAKE_SOURCE_DIR}/platform/session/session_core.c # shared launcher/session reducer
    ${CMAKE_SOURCE_DIR}/platform/session/session_bridge.c # versioned engine session ABI
    ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c # launcher-owned private-room reducer
    ${CMAKE_SOURCE_DIR}/platform/online/compatibility_identity.c # clean provenance compatibility bytes
    ${CMAKE_SOURCE_DIR}/platform/online/lobby_view_model.c # sanitized launcher UX projection
    ${CMAKE_SOURCE_DIR}/platform/online/lobby_fake_adapter.c # deterministic ON-03B adapter
    ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
    ${CMAKE_SOURCE_DIR}/platform/online/match_launch_builder.c
    ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c # frozen rollback match identity
    ${CMAKE_SOURCE_DIR}/platform/net/match_transport.c # launcher-owned authenticated input drain
    ${CMAKE_SOURCE_DIR}/platform/net/match_input_runtime.c # copy-out launcher-to-engine input provider
    ${CMAKE_SOURCE_DIR}/platform/net/match_input_packet.c # fixed hostile-input-safe carrier codec
    ${CMAKE_SOURCE_DIR}/platform/net/match_input_bundle.c # redundant three-frame carrier bundle
    ${CMAKE_SOURCE_DIR}/platform/net/match_input_repair.c # explicit input-gap repair codec
    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_graph.c # bounded direct/one-hop peer topology
    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_forward.c # bounded opaque one-hop forwarding
    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_transcript.c # canonical peer keys + phrase
    ${CMAKE_SOURCE_DIR}/platform/net/match_preflight.c # pure authenticated launch consensus gate
    ${CMAKE_SOURCE_DIR}/platform/net/net_input.c      # canonical predicted/confirmed input history
    ${CMAKE_SOURCE_DIR}/platform/net/net_local_input.c # endpoint seats -> canonical slots
    ${CMAKE_SOURCE_DIR}/platform/net/net_roster.c     # canonical players vs local viewports
    ${CMAKE_SOURCE_DIR}/platform/net/net_roster_runtime.c # immutable launcher roster publication
    ${CMAKE_SOURCE_DIR}/platform/net/online_race_results.c # finished-race placements engine->launcher handoff
    ${CMAKE_SOURCE_DIR}/platform/net/local_listener_mix.c # endpoint-only physical audio listener policy
    ${CMAKE_SOURCE_DIR}/platform/net/net_impairment.c  # named deterministic carrier fault profiles
    ${CMAKE_SOURCE_DIR}/platform/net/net_failure_ring.c # allocation-free online failure/stall forensics
    ${CMAKE_SOURCE_DIR}/platform/net/net_clock.c       # named endpoint clock/opportunity profiles
    ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_snapshot.c # bounded in-process authority snapshots
    ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_ring.c     # preallocated snapshot history
    ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_events.c   # confirmed side-effect journal
    ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_audio.c    # versioned rollback-safe sound previews
    ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_driver.c   # restore/resimulation scheduler
    ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_engine_registry.c # exact engine allocation adapter
    ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_game_authority.c # concrete v3 game range registry
    ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_game_runtime.c # feature-off real-level rollback lab
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
    ${CMAKE_SOURCE_DIR}/platform/ghost_bank.c # per-(level, vehicle) TT ghost bank (issue #46)
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
# Adventure Party (AP-02/AP-03): pure native session state machine + policy
# core, own directory, adventure_party_ prefix (never party_ — that is Phone
# Party — and never mdkr_adventure_ — those are the test hooks above). The
# OMIT arm exists from the first module commit: when ON the sources leave the
# game binary and MDKR_ADVENTURE_PARTY_OMIT=1 (set beside the other compile
# definitions below) lets future adapter call sites compile out. The unit
# tests in cmake/tests.cmake compile these sources directly and never consult
# this option, so ctest is identical on both arms.
option(MDKR_ADVENTURE_PARTY_OMIT
       "Exclude the Adventure Party modules from the game binary" OFF)
if(NOT MDKR_ADVENTURE_PARTY_OMIT)
    list(APPEND PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_state.c  # session states/generations/exact-once
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_policy.c # capability table + arbitration + authority
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_trace.c  # AP-05 read-only trace formatters + emit wrappers
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_spawn.c  # AP-07 deterministic formation candidate planner (pure)
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_runtime.c # AP-06a process-wide session singleton (R12); save_state consumes its is_active query
    )
endif()
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

# The accessibility voice, one implementation per platform — the same
# arrangement, for the same reason, as platform/app/file_dialog_* below: the
# interface in platform/a11y_speech.h is shared, exactly one backend compiles,
# and the drain worker above never learns which one it got. A platform with no
# engine gets the null backend and degrades to text-only with a stated reason
# rather than failing to link.
if(EMSCRIPTEN)
    list(APPEND PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/a11y_speech_web.c)     # Web Speech API
elseif(APPLE)
    # OBJCXX here as well as in the app-shell block below: this backend is
    # compiled into the ENGINE (a11y_model.c is), and a -DMDKR_APP=OFF build
    # still has to be able to speak.
    enable_language(OBJCXX)
    list(APPEND PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/a11y_speech_mac.mm)    # AVSpeechSynthesizer
elseif(WIN32)
    list(APPEND PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/a11y_speech_win.cpp)   # SAPI 5 / ISpVoice
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/a11y_speech_linux.c)   # speech-dispatcher, dlopen'd
else()
    list(APPEND PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/a11y_speech_null.c)    # text-only, and says so
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
    # Backend support TU (only meaningful with the webgpu code):
    #   gfx_webgpu_stubs.c — inert satisfier for mgb64-shell symbols (overlay,
    #                        host handoff, minimap, savedir) DKR has no analogue for
    list(APPEND PLATFORM_SOURCES
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

# Native online BETA live selection bridge (launcher->engine lobby snapshot +
# engine->launcher local-intent channel). Engine-resident like
# net_roster_runtime/online_race_results so BOTH the engine-side readers
# (game/src, beta-gated) and the launcher-side writer/poller
# (online_live_wiring.cpp in mdkr64_app) link ONE copy, but -- unlike those two,
# which every build compiles -- it is gated so the beta-OFF engine object never
# carries it. In PLATFORM_SOURCES it inherits the strict warning baseline below.
if(MDKR_ENABLE_ONLINE_BETA)
    list(APPEND PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/platform/net/party_link.c) # live selection bridge (beta)
endif()

# glad GL loader — desktop only (no GL in the browser build).
if(NOT EMSCRIPTEN)
    set(GLAD_SOURCES ${CMAKE_SOURCE_DIR}/lib/glad/src/glad.c)
else()
    set(GLAD_SOURCES "")
endif()

# stb_image, instantiated in its own TU so third-party source can be compiled
# with warnings off without weakening the strict baseline PLATFORM_SOURCES is
# held to. Every platform builds it: content-pack PNG decode is not
# desktop-only. See lib/stb/stb_image_impl.c for the decoder configuration.
set(STB_SOURCES ${CMAKE_SOURCE_DIR}/lib/stb/stb_image_impl.c)

# miniz, the zip reader behind platform/mod_source.c's archive arm. Third-party
# and, like stb_image, compiled with warnings off in its own TU rather than
# relaxing the baseline every first-party file is held to.
set(MINIZ_SOURCES ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)

# dr_wav, the WAV decoder behind platform/mod_music.c. Same arrangement as the
# two above: header-only third-party source instantiated in one TU of its own
# so it can be compiled with warnings off without relaxing anything a
# first-party file is held to. See lib/dr_libs/dr_wav_impl.c.
set(DR_LIBS_SOURCES ${CMAKE_SOURCE_DIR}/lib/dr_libs/dr_wav_impl.c)

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
    ${STB_SOURCES}
    ${MINIZ_SOURCES}
    ${DR_LIBS_SOURCES}
)
target_link_libraries(${MDKR_TARGET} PRIVATE mdkr_character_text_shaping)

# Include layout mirrors the DKR Makefile (-I . -I include -I include/libc
# -I include/PR -I include/sys -I src -I libultra), rooted at game/ so paths like
# "src/memory.h" and "libultra/src/audio/foo.h" resolve.
target_include_directories(${MDKR_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/game
    ${CMAKE_SOURCE_DIR}/game/src
    ${CMAKE_SOURCE_DIR}/game/include
    ${CMAKE_SOURCE_DIR}/game/libultra
    ${CMAKE_SOURCE_DIR}/game/libultra/src/audio
    ${CMAKE_SOURCE_DIR}/platform
    ${CMAKE_SOURCE_DIR}/platform/fast3d
    ${CMAKE_SOURCE_DIR}/platform/fast3d_shim
    ${CMAKE_SOURCE_DIR}/lib/glad/include
    ${CMAKE_SOURCE_DIR}/lib/stb          # stb_image.h, taken by mod_texture_store.c
    ${CMAKE_SOURCE_DIR}/lib/miniz        # miniz.h, taken by mod_source.c
    ${CMAKE_SOURCE_DIR}/lib/dr_libs      # dr_wav.h, taken by mod_music.c
    ${SDL2_INCLUDE_DIRS}
)
mdkr_add_legacy_game_quote_includes(${MDKR_TARGET})

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
# Adventure Party OMIT arm (option declared beside PLATFORM_SOURCES above):
# with the module sources excluded, this definition is what future adapter
# call sites #if out on, so an OMIT build cannot half-link the feature.
if(MDKR_ADVENTURE_PARTY_OMIT)
    target_compile_definitions(${MDKR_TARGET} PRIVATE
        MDKR_ADVENTURE_PARTY_OMIT=1)
endif()

# The online-beta direct-boot seam (mode_intro -> straight into the online race)
# lives in the game/decomp translation units that compile into ${MDKR_TARGET}.
# It is entirely #if MDKR_ENABLE_ONLINE_BETA, so a normal (beta OFF) build never
# sees the macro and the engine object stays byte-identical to the pinned OFF
# configuration.
# Beta-only, matching the mdkr64_app definition below. The only PLATFORM_SOURCES
# addition this macro makes is the beta live selection bridge (party_link.c,
# gated above); every decomp TU that keys on the macro is entirely #if-guarded,
# so a normal (beta OFF) build activates none of it.
if(MDKR_ENABLE_ONLINE_BETA)
    target_compile_definitions(${MDKR_TARGET} PRIVATE MDKR_ENABLE_ONLINE_BETA=1)
endif()

# --- Version stamping (PK-1, macOS packaging) ------------------------------
# Single source of truth for the packaged release version. Wired to a real
# surface (`mdkr64 --version`, platform/main_pc.c) and to the app bundle's
# CFBundleShortVersionString, which macos/Scripts/build_app_bundle.sh stamps
# from this same value via PlistBuddy. Kept as its own standalone statement
# (not folded into the block above) so it stays a one-line, low-conflict diff.
set(MDKR_VERSION "1.7.0" CACHE STRING "mdkr64 release version (semver)")
target_compile_definitions(${MDKR_TARGET} PRIVATE MDKR_VERSION_STRING="${MDKR_VERSION}")
# The hand-written native WebSocket client is compiled into the app and into
# several standalone transport/test targets. Give every one of those
# compilations the same product-version token, instead of maintaining a second
# release literal in match_signal_client.cpp.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/platform/online/match_signal_client.cpp PROPERTIES
    COMPILE_DEFINITIONS "MDKR_MATCH_USER_AGENT_VERSION=\"${MDKR_VERSION}\"")
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
# Vendored third-party source does not survive this project's warning baseline,
# and it is not this project's to fix. Suppress warnings for that FILE rather
# than relaxing the flags any first-party file is held to — the whole reason
# stb_image is instantiated in its own TU is to make this a one-file exemption.
if(MSVC)
    set_source_files_properties(${STB_SOURCES} ${MINIZ_SOURCES}
        ${DR_LIBS_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(${STB_SOURCES} ${MINIZ_SOURCES}
        ${DR_LIBS_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()
target_compile_options(${MDKR_TARGET} PRIVATE
    -fno-strict-aliasing
    # FP contraction is pinned OFF for every engine lane. Unpinned, each
    # compiler decides for itself whether a*b+c fuses into a single-rounding
    # FMA (AppleClang on arm64 fuses at -O2 by default; GCC and emcc make
    # their own calls per target), so identical source could compute
    # differently-rounded floats per toolchain. Gameplay float state must be
    # bit-reproducible across the native, wasm, and mingw builds — lockstep
    # peers and every golden float hash depend on it — so force the strict
    # IEEE two-rounding evaluation everywhere. This block is the shared flag
    # seam all three lanes inherit (MDKR_TARGET is mdkr64 or mdkr64_web; the
    # mingw cross build configures this same file through its toolchain
    # file), and `-ffp-contract=off` is accepted by Clang, AppleClang, emcc,
    # and GCC alike, so it needs no per-compiler gating.
    -ffp-contract=off
    # Address-domain crossings must be explicit (uintptr_t, DKR_TOK/DKR_PTR, or
    # OS_K0_TOKEN_TO_PHYSICAL). A raw narrowing conversion is a correctness
    # defect on LP64 and must never silently return to the tree.
    #
    # Of these two, only -Werror=pointer-to-int-cast is C-family-only: GCC's
    # cc1plus rejects it for C++ ("'-Werror=' argument
    # '-Werror=pointer-to-int-cast' is not valid for C++"), which surfaces on
    # Windows where a C++ TU (platform/a11y_speech_win.cpp, SAPI ISpVoice) is
    # compiled into this otherwise-C engine target (and on the macOS ObjC++
    # analogue). -Werror=int-to-pointer-cast, by contrast, IS accepted for C++
    # (verified: g++ 16.2) -- it was silently active on that C++/ObjC++ TU before
    # this scoping. Both are scoped to C sources anyway, deliberately: it drops
    # the cc1plus rejection and keeps the int<->pointer cast policy symmetric and
    # easy to reason about, at the cost of no longer applying int-to-pointer-cast
    # to the one C++ TU (which never tripped it). C-file command lines are
    # unchanged (the generator expression expands to the same flag for
    # COMPILE_LANGUAGE C).
    $<$<COMPILE_LANGUAGE:C>:-Werror=int-to-pointer-cast>
    $<$<COMPILE_LANGUAGE:C>:-Werror=pointer-to-int-cast>
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
        # AVFoundation: AVSpeechSynthesizer, behind platform/a11y_speech_mac.mm.
        target_link_libraries(${MDKR_TARGET} PRIVATE ${OPENGL_LIB}
            "-framework AVFoundation" "-framework Foundation")
    else()
        # CMAKE_DL_LIBS also carries the dlopen that platform/a11y_speech_linux.c
        # loads speech-dispatcher through — deliberately at runtime, so a tarball
        # still starts on a machine that does not have it.
        target_link_libraries(${MDKR_TARGET} PRIVATE OpenGL::GL ${CMAKE_DL_LIBS})
    endif()
    if(WIN32)
        # ole32: CoCreateInstance for SAPI's ISpVoice
        # (platform/a11y_speech_win.cpp). A stock Windows system DLL, already on
        # the import allowlist in tools/check_windows_imports.sh.
        target_link_libraries(${MDKR_TARGET} PRIVATE ole32)
    endif()
    # WebGPU backend: gate the code + link the pinned wgpu-native prebuilt. GL +
    # WebGPU stay compiled together so MDKR_RENDERER switches at runtime.
    if(MDKR_WEBGPU_BACKEND)
        target_compile_definitions(${MDKR_TARGET} PRIVATE MDKR_WEBGPU_BACKEND)
        target_link_libraries(${MDKR_TARGET} PRIVATE webgpu)
        if(MDKR_CHARACTER_KTX2)
            target_compile_definitions(${MDKR_TARGET} PRIVATE
                MDKR_CHARACTER_KTX2=1)
            target_link_libraries(${MDKR_TARGET} PRIVATE
                mdkr_character_ktx2_bridge)
        endif()
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
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        # The brand hero is generated into the build tree rather than tracked as
        # a byte array: the repo keeps the source art (brand/hero.png) and never
        # a multi-megabyte generated blob, the same split the embedded fonts use.
        set(MDKR_HERO_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/hero_png.h)
        add_custom_command(
            OUTPUT ${MDKR_HERO_HEADER}
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${CMAKE_CURRENT_BINARY_DIR}/generated
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tools/gen_image_header.py
                    ${CMAKE_SOURCE_DIR}/brand/hero.png
                    ${MDKR_HERO_HEADER}
                    --symbol kAppHeroPng
            DEPENDS ${CMAKE_SOURCE_DIR}/brand/hero.png
                    ${CMAKE_SOURCE_DIR}/tools/gen_image_header.py
            COMMENT "Embedding brand/hero.png for the launcher home"
            VERBATIM)

        set(APP_SOURCES
            ${MDKR_HERO_HEADER}
            ${CMAKE_SOURCE_DIR}/platform/app/app_config.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_config_escape.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_host.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_relaunch.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_restart.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_theme.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_launch_hold.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_ui_policy.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/app_window.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/arg_triage.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_adapter_output_index.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_candidate_index.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_edit_history.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_failure_index.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_portrait_import.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_portrait_studio.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_preview_cache.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_raw_draft_store.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_raw_intake_index.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_revision_index.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_spdx_expression.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_test_evidence_store.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_draft_store.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_draft_snapshot.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_draft_transfer.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_png_validation.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_visual_report.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/character_workshop_model.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/crash_screen.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/dev_tools.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/tool_collision.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/tool_console.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/tool_diagnostics.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/tool_freecam.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/tool_objects.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/tool_performance.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/diag_log.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/engine_boot.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/main_app.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/rom_validate.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/rom_browser.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/file_browser_ui.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/pack_install_job.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/pack_installer.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/session_runtime.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_common.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_diag.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_hero.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_launcher.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_online_room.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_overlay.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_phone_party.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_rom.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/ui_settings.cpp
            ${CMAKE_SOURCE_DIR}/platform/party/native_party_host.cpp
            ${CMAKE_SOURCE_DIR}/platform/party/native_remote_pad_ingress.cpp)

        if(MDKR_NATIVE_PHONE_PARTY)
            list(APPEND APP_SOURCES
                ${CMAKE_SOURCE_DIR}/platform/party/libdatachannel_party_transport.cpp
                ${CMAKE_SOURCE_DIR}/platform/party/lan_party_transport.cpp
                ${CMAKE_SOURCE_DIR}/platform/party/lan_party_server.cpp
                ${CMAKE_SOURCE_DIR}/platform/party/lan_party_launch.cpp
                ${CMAKE_SOURCE_DIR}/platform/party/lan_party_launch_config.cpp
                ${CMAKE_SOURCE_DIR}/platform/party/lan_party_room.cpp
                ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp
                ${CMAKE_SOURCE_DIR}/platform/party/party_event_queue.cpp
                ${CMAKE_SOURCE_DIR}/platform/party/party_retry_policy.cpp
                ${CMAKE_SOURCE_DIR}/platform/net/match_peer_crypto.cpp)
        else()
            list(APPEND APP_SOURCES
                ${CMAKE_SOURCE_DIR}/platform/party/libdatachannel_party_transport_stub.cpp
                ${CMAKE_SOURCE_DIR}/platform/party/lan_party_stub.cpp)
        endif()

        if(MDKR_ENABLE_ONLINE_BETA)
            # Native online beta: the live lobby adapter plus its production
            # MatchRoom HTTP transport, signal-client mesh backend and peer
            # mesh, and the launcher-side wiring that composes them behind the
            # beta gate (platform/app/online_live_wiring.cpp). These are exactly
            # the online TUs the e2e driver links; every other dependency
            # already rides in -- the lobby_*, net_*, match_* (transport/manifest/
            # preflight/launch/roster), session_* and sha256/input_tick_queue set
            # through PLATFORM_SOURCES, and match_peer_crypto + party_webrtc_
            # signaling + party_retry_policy + LibDataChannel/MbedTLS through the
            # MDKR_NATIVE_PHONE_PARTY block above, which the beta requires.
            if(NOT MDKR_NATIVE_PHONE_PARTY)
                message(FATAL_ERROR
                    "MDKR_ENABLE_ONLINE_BETA requires MDKR_NATIVE_PHONE_PARTY=ON")
            endif()
            list(APPEND APP_SOURCES
                ${CMAKE_SOURCE_DIR}/platform/online/match_live_adapter.cpp
                ${CMAKE_SOURCE_DIR}/platform/online/match_peer_liveness.cpp
                ${CMAKE_SOURCE_DIR}/platform/online/match_live_transport.cpp
                ${CMAKE_SOURCE_DIR}/platform/online/match_signal_client.cpp
                ${CMAKE_SOURCE_DIR}/platform/online/match_peer_transport.cpp
                ${CMAKE_SOURCE_DIR}/platform/online/match_peer_lost_reason.cpp
                ${CMAKE_SOURCE_DIR}/platform/online/online_track_table.c
                ${CMAKE_SOURCE_DIR}/platform/app/online_live_wiring.cpp)

            if(BUILD_TESTING)
                # KEEP IN SYNC: these online/transport CTest exes (this one plus
                # the party/match set in the MDKR_NATIVE_PHONE_PARTY block just
                # below) are the same eleven names mirrored in two other places --
                # the MinGW test-link dead-strip foreach (search
                # "_mdkr_online_test", later in this file) and ONLINE_TEST_EXES in
                # tools/mingw_cross_check.sh. Renaming/removing one means updating
                # all three; the dead-strip foreach hard-errors the online
                # configure on drift so this cannot rot silently.
                #
                # The launcher-side track/cup table (display order, picker
                # narrowing, trophy cups) pinned against the independent v80
                # ROM decode. Pure static data: ROM-, GPU- and network-free.
                add_executable(mdkr_online_track_table_test
                    ${CMAKE_SOURCE_DIR}/tests/test_online_track_table.c
                    ${CMAKE_SOURCE_DIR}/platform/online/online_track_table.c)
                target_include_directories(mdkr_online_track_table_test PRIVATE
                    ${CMAKE_SOURCE_DIR}
                    ${CMAKE_SOURCE_DIR}/platform)
                if(MSVC)
                    target_compile_options(mdkr_online_track_table_test PRIVATE
                        /W4 /WX)
                else()
                    target_compile_options(mdkr_online_track_table_test PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
                add_test(NAME online_track_table
                    COMMAND mdkr_online_track_table_test)
            endif()
        endif()

        add_library(mdkr_qrcodegen STATIC
            ${CMAKE_SOURCE_DIR}/third_party/qrcodegen/qrcodegen.cpp)
        target_include_directories(mdkr_qrcodegen PUBLIC
            ${CMAKE_SOURCE_DIR}/third_party/qrcodegen)
        target_compile_features(mdkr_qrcodegen PRIVATE cxx_std_17)
        if(BUILD_TESTING)
            add_executable(mdkr_native_party_qr_test
                ${CMAKE_SOURCE_DIR}/tests/test_native_party_qr.cpp)
            target_link_libraries(mdkr_native_party_qr_test PRIVATE mdkr_qrcodegen)
            target_compile_features(mdkr_native_party_qr_test PRIVATE cxx_std_17)
            add_test(NAME native_party_qr COMMAND mdkr_native_party_qr_test)

            if(MDKR_NATIVE_PHONE_PARTY)
                add_executable(mdkr_native_party_sas_test
                    ${CMAKE_SOURCE_DIR}/tests/test_native_party_sas.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/libdatachannel_party_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_event_queue.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_retry_policy.cpp)
                target_include_directories(mdkr_native_party_sas_test PRIVATE
                    ${CMAKE_SOURCE_DIR}/platform
                    ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                    ${MDKR_PARTY_GENERATED_INCLUDE})
                target_include_directories(mdkr_native_party_sas_test SYSTEM PRIVATE
                    ${mdkr_libdatachannel_SOURCE_DIR}/include)
                target_compile_features(mdkr_native_party_sas_test PRIVATE cxx_std_17)
                target_link_libraries(mdkr_native_party_sas_test PRIVATE
                    LibDataChannel::LibDataChannel MbedTLS::MbedTLS)
                if(MSVC)
                    target_compile_options(mdkr_native_party_sas_test PRIVATE /W4 /WX)
                else()
                    target_compile_options(mdkr_native_party_sas_test PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
                add_test(NAME native_party_sas COMMAND mdkr_native_party_sas_test)

                # Phase 3 local-only play: the LanPartyTransport driven under
                # the real MdkrNativePartyHost. Lives here (not cmake/tests.cmake)
                # because it needs libdatachannel + nlohmann + MbedTLS, which are
                # only configured inside this MDKR_NATIVE_PHONE_PARTY block. The
                # phones are simulated with a fake bridge socket + a real
                # loopback WebRTC answerer, so it stays network- and ROM-free.
                # MDKR_LAN_PARTY_TESTING compiles the transport's port-free test
                # factory + controller-attach seam (and the server's park hooks),
                # and nowhere else -- the MDKR_A11Y_SPEECH_TESTING pattern.
                add_executable(mdkr_lan_party_transport_test
                    ${CMAKE_SOURCE_DIR}/tests/test_lan_party_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/lan_party_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/lan_party_server.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/lan_party_room.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_event_queue.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/native_party_host.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/native_remote_pad_ingress.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_protocol.c)
                target_include_directories(mdkr_lan_party_transport_test PRIVATE
                    ${CMAKE_SOURCE_DIR}/platform
                    ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                    ${MDKR_PARTY_GENERATED_INCLUDE})
                target_include_directories(mdkr_lan_party_transport_test SYSTEM PRIVATE
                    ${mdkr_libdatachannel_SOURCE_DIR}/include)
                target_compile_definitions(mdkr_lan_party_transport_test PRIVATE
                    MDKR_LAN_PARTY_TESTING)
                target_compile_features(mdkr_lan_party_transport_test PRIVATE cxx_std_17)
                target_link_libraries(mdkr_lan_party_transport_test PRIVATE
                    LibDataChannel::LibDataChannel MbedTLS::MbedTLS Threads::Threads)
                if(WIN32)
                    target_link_libraries(mdkr_lan_party_transport_test PRIVATE ws2_32)
                endif()
                if(MSVC)
                    target_compile_options(mdkr_lan_party_transport_test PRIVATE /W4 /WX)
                else()
                    target_compile_options(mdkr_lan_party_transport_test PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
                add_test(NAME lan_party_transport
                    COMMAND mdkr_lan_party_transport_test)
                # Bounded internal waits; this backstops a genuine hang (a
                # handshake that never connects) rather than stalling the suite.
                set_tests_properties(lan_party_transport PROPERTIES TIMEOUT 120)

                add_executable(mdkr_match_peer_crypto_test
                    ${CMAKE_SOURCE_DIR}/tests/test_match_peer_crypto.cpp
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_crypto.cpp
                    ${CMAKE_SOURCE_DIR}/platform/net/match_preflight.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_graph.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c
                    ${CMAKE_SOURCE_DIR}/platform/sha256.c)
                target_include_directories(mdkr_match_peer_crypto_test PRIVATE
                    ${CMAKE_SOURCE_DIR}
                    ${CMAKE_SOURCE_DIR}/platform)
                target_compile_features(mdkr_match_peer_crypto_test PRIVATE cxx_std_17)
                target_link_libraries(mdkr_match_peer_crypto_test PRIVATE
                    MbedTLS::MbedTLS)
                if(MSVC)
                    target_compile_options(mdkr_match_peer_crypto_test PRIVATE /W4 /WX)
                else()
                    target_compile_options(mdkr_match_peer_crypto_test PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
                add_test(NAME match_peer_crypto COMMAND mdkr_match_peer_crypto_test)

                # online multiplayer: the native match-signaling client
                # (platform/online/match_signal_client.cpp), pinned wire-level
                # against a scriptable loopback fake of the
                # /api/match/{roomId}/signal endpoint
                # (tests/match_signal_test_server.cpp). Lives here (not
                # cmake/tests.cmake) because it needs nlohmann + MbedTLS + the
                # embedded CA header, which are only configured inside this
                # MDKR_NATIVE_PHONE_PARTY block. ROM-, GPU- and network-free:
                # every socket is 127.0.0.1 on an ephemeral port.
                add_executable(mdkr_match_signal_client_test
                    ${CMAKE_SOURCE_DIR}/tests/test_match_signal_client.cpp
                    ${CMAKE_SOURCE_DIR}/tests/match_signal_test_server.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_signal_client.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp)
                target_include_directories(mdkr_match_signal_client_test PRIVATE
                    ${CMAKE_SOURCE_DIR}/platform
                    ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                    ${MDKR_PARTY_GENERATED_INCLUDE})
                target_compile_features(mdkr_match_signal_client_test PRIVATE
                    cxx_std_17)
                target_link_libraries(mdkr_match_signal_client_test PRIVATE
                    MbedTLS::MbedTLS Threads::Threads)
                if(WIN32)
                    target_link_libraries(mdkr_match_signal_client_test PRIVATE
                        ws2_32)
                endif()
                if(MSVC)
                    target_compile_options(mdkr_match_signal_client_test PRIVATE
                        /W4 /WX)
                else()
                    target_compile_options(mdkr_match_signal_client_test PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
                add_test(NAME match_signal_client
                    COMMAND mdkr_match_signal_client_test)
                # Bounded waits throughout the suite (two deliberate ~2 s
                # welcome-timeout cases included); the ctest timeout backstops
                # a genuine hang instead of stalling the suite.
                set_tests_properties(match_signal_client PROPERTIES TIMEOUT 120)

                # online multiplayer: the launcher-owned WebRTC peer
                # mesh (platform/online/match_peer_transport.cpp) over REAL
                # libdatachannel DTLS on loopback, with the signaling relay
                # faked in-process through the MdkrMatchPeerSignalFeed seam.
                # Lives here (not cmake/tests.cmake) because it needs
                # libdatachannel + nlohmann + MbedTLS, which are only
                # configured inside this MDKR_NATIVE_PHONE_PARTY block.
                # ROM-, GPU- and network-free.
                add_executable(mdkr_match_peer_transport_test
                    ${CMAKE_SOURCE_DIR}/tests/test_match_peer_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_peer_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_peer_lost_reason.cpp
                    ${CMAKE_SOURCE_DIR}/platform/net/net_failure_ring.c
                    ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
                    ${CMAKE_SOURCE_DIR}/platform/online/match_signal_client.cpp
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_crypto.cpp
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_transcript.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_graph.c
                    ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
                    ${CMAKE_SOURCE_DIR}/platform/sha256.c
                    ${CMAKE_SOURCE_DIR}/platform/party/party_retry_policy.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp)
                target_include_directories(mdkr_match_peer_transport_test PRIVATE
                    ${CMAKE_SOURCE_DIR}
                    ${CMAKE_SOURCE_DIR}/platform
                    ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                    ${MDKR_PARTY_GENERATED_INCLUDE})
                target_include_directories(mdkr_match_peer_transport_test
                    SYSTEM PRIVATE
                    ${mdkr_libdatachannel_SOURCE_DIR}/include)
                target_compile_features(mdkr_match_peer_transport_test PRIVATE
                    cxx_std_17)
                target_link_libraries(mdkr_match_peer_transport_test PRIVATE
                    LibDataChannel::LibDataChannel MbedTLS::MbedTLS
                    Threads::Threads)
                if(WIN32)
                    target_link_libraries(mdkr_match_peer_transport_test PRIVATE
                        ws2_32)
                endif()
                if(MSVC)
                    target_compile_options(mdkr_match_peer_transport_test PRIVATE
                        /W4 /WX)
                else()
                    target_compile_options(mdkr_match_peer_transport_test PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
                add_test(NAME match_peer_transport
                    COMMAND mdkr_match_peer_transport_test)
                # Real loopback DTLS handshakes for a 3-endpoint mesh plus
                # restart recovery; internal waits are bounded, so this
                # timeout backstops a genuine hang only.
                set_tests_properties(match_peer_transport PROPERTIES TIMEOUT 180)

                # End-to-end lane driver: the real WSS/WebRTC transport under
                # the real host model, narrated on stdout. Deliberately no
                # add_test() — it needs a live local Party Worker and a paired
                # controller page that only tests/check_party_native_e2e.py
                # provides, so that check owns running it (the same
                # script-owns-binary split the GPU-adjacent gates use).
                # The LAN arm (--lan) links the embedded server + in-process
                # room + LanPartyTransport so tests/check_party_lan_e2e.py can
                # prove the whole no-internet path with NO wrangler. The pure
                # lan_party_launch.cpp carries the manifest builder; the getifaddrs
                # /SDL glue (lan_party_launch_config.cpp) is app-only and stays out.
                add_executable(mdkr_native_party_e2e_driver
                    ${CMAKE_SOURCE_DIR}/tests/test_native_party_e2e_driver.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/native_party_host.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/native_remote_pad_ingress.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_protocol.c
                    ${CMAKE_SOURCE_DIR}/platform/party/libdatachannel_party_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/lan_party_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/lan_party_server.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/lan_party_room.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/lan_party_launch.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_event_queue.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_retry_policy.cpp)
                target_include_directories(mdkr_native_party_e2e_driver PRIVATE
                    ${CMAKE_SOURCE_DIR}/platform
                    ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                    ${MDKR_PARTY_GENERATED_INCLUDE})
                target_include_directories(mdkr_native_party_e2e_driver SYSTEM PRIVATE
                    ${mdkr_libdatachannel_SOURCE_DIR}/include)
                target_compile_features(mdkr_native_party_e2e_driver PRIVATE cxx_std_17)
                target_link_libraries(mdkr_native_party_e2e_driver PRIVATE
                    LibDataChannel::LibDataChannel MbedTLS::MbedTLS
                    Threads::Threads)
                if(WIN32)
                    target_link_libraries(mdkr_native_party_e2e_driver PRIVATE
                        ws2_32)
                endif()
                if(MSVC)
                    target_compile_options(mdkr_native_party_e2e_driver PRIVATE /W4 /WX)
                else()
                    target_compile_options(mdkr_native_party_e2e_driver PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()

                # online multiplayer: the launcher-owned LIVE lobby adapter
                # (platform/online/match_live_adapter.cpp) composing the MatchRoom
                # transport, the peer mesh, the launch builder (retail
                # clamp), the preflight consensus barrier and the engine roster
                # runtime. The lobby side is driven by an in-process MatchRoom
                # double running the real reducer; the transport side rides the
                # loopback signal hub feeding real libdatachannel DTLS on
                # 127.0.0.1. ROM-, GPU- and network-free.
                set(MDKR_ONLINE_LIVE_ADAPTER_SOURCES
                    ${CMAKE_SOURCE_DIR}/tests/test_online_live_adapter.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_live_adapter.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_peer_liveness.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/online_track_table.c
                    ${CMAKE_SOURCE_DIR}/platform/online/match_peer_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_peer_lost_reason.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_signal_client.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_launch_builder.c
                    ${CMAKE_SOURCE_DIR}/platform/online/lobby_view_model.c
                    ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
                    ${CMAKE_SOURCE_DIR}/platform/online/lobby_fake_adapter.c
                    ${CMAKE_SOURCE_DIR}/platform/net/party_link.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_preflight.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_roster_runtime.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_roster.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_crypto.cpp
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_transcript.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_graph.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_transport.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_failure_ring.c
                    ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_input_bundle.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_input_repair.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_impairment.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_input.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_local_input.c
                    ${CMAKE_SOURCE_DIR}/platform/session/session_bridge.c
                    ${CMAKE_SOURCE_DIR}/platform/session/session_core.c
                    ${CMAKE_SOURCE_DIR}/platform/input_tick_queue.c
                    ${CMAKE_SOURCE_DIR}/platform/sha256.c
                    ${CMAKE_SOURCE_DIR}/platform/party/party_retry_policy.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp)
                add_executable(mdkr_online_live_adapter_test
                    ${MDKR_ONLINE_LIVE_ADAPTER_SOURCES})
                target_include_directories(mdkr_online_live_adapter_test PRIVATE
                    ${CMAKE_SOURCE_DIR}
                    ${CMAKE_SOURCE_DIR}/platform
                    ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                    ${MDKR_PARTY_GENERATED_INCLUDE})
                target_include_directories(mdkr_online_live_adapter_test
                    SYSTEM PRIVATE
                    ${mdkr_libdatachannel_SOURCE_DIR}/include)
                target_compile_features(mdkr_online_live_adapter_test PRIVATE
                    cxx_std_17)
                target_link_libraries(mdkr_online_live_adapter_test PRIVATE
                    LibDataChannel::LibDataChannel MbedTLS::MbedTLS
                    Threads::Threads)
                if(WIN32)
                    target_link_libraries(mdkr_online_live_adapter_test PRIVATE
                        ws2_32)
                endif()
                if(MSVC)
                    target_compile_options(mdkr_online_live_adapter_test PRIVATE
                        /W4 /WX)
                else()
                    target_compile_options(mdkr_online_live_adapter_test PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
                add_test(NAME online_live_adapter
                    COMMAND mdkr_online_live_adapter_test)
                # Two real loopback DTLS meshes reaching preflight consensus;
                # internal waits are bounded, so this timeout backstops a hang.
                set_tests_properties(online_live_adapter PROPERTIES TIMEOUT 180)

                # B4/A4 positive control: the SAME adapter, mesh and transport
                # with the failure ring compiled out. It runs the identical
                # --forensics scenario to the identical peer loss and writes no
                # dump, so the forensics lane's assertions are proven to read
                # the PRODUCTION recorder rather than the scenario.
                add_executable(mdkr_online_live_adapter_norecord
                    ${MDKR_ONLINE_LIVE_ADAPTER_SOURCES})
                target_compile_definitions(mdkr_online_live_adapter_norecord
                    PRIVATE MDKR_NET_FAILURE_RING_DISABLED=1)
                target_include_directories(mdkr_online_live_adapter_norecord
                    PRIVATE
                    ${CMAKE_SOURCE_DIR}
                    ${CMAKE_SOURCE_DIR}/platform
                    ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                    ${MDKR_PARTY_GENERATED_INCLUDE})
                target_include_directories(mdkr_online_live_adapter_norecord
                    SYSTEM PRIVATE
                    ${mdkr_libdatachannel_SOURCE_DIR}/include)
                target_compile_features(mdkr_online_live_adapter_norecord
                    PRIVATE cxx_std_17)
                target_link_libraries(mdkr_online_live_adapter_norecord PRIVATE
                    LibDataChannel::LibDataChannel MbedTLS::MbedTLS
                    Threads::Threads)
                if(WIN32)
                    target_link_libraries(
                        mdkr_online_live_adapter_norecord PRIVATE ws2_32)
                endif()
                if(MSVC)
                    target_compile_options(
                        mdkr_online_live_adapter_norecord PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        mdkr_online_live_adapter_norecord PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
                find_package(Python3 COMPONENTS Interpreter REQUIRED)
                add_test(
                    NAME net_failure_ring_impairment
                    COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_SOURCE_DIR}/tests/check_net_failure_ring_impairment.py
                            --record $<TARGET_FILE:mdkr_online_live_adapter_test>
                            --norecord
                            $<TARGET_FILE:mdkr_online_live_adapter_norecord>)
                set_tests_properties(net_failure_ring_impairment
                    PROPERTIES TIMEOUT 300)

                # F4: the SAME adapter TU, compiled WITH
                # MDKR_ENABLE_ONLINE_BETA (the target above deliberately does not
                # -- the define breaks its token-gate test), so the beta-only
                # race-end-card truthfulness decisions (mapLostReason in-race
                # branches + the F1 no-demotion rule) are actually exercised,
                # plus the SAS re-verify peer-loss latch-clear regression.
                # Mesh-free: only the exposed test wrappers are called (the
                # latch-clear ones build a lobby-free, backend-less adapter),
                # with no-op stubs for the boot-handoff hooks the beta build
                # references, so it links the adapter without the wiring TU.
                add_executable(mdkr_online_live_adapter_beta_test
                    ${CMAKE_SOURCE_DIR}/tests/test_online_live_adapter_beta.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_live_adapter.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_peer_liveness.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/online_track_table.c
                    ${CMAKE_SOURCE_DIR}/platform/online/match_peer_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_peer_lost_reason.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_signal_client.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_launch_builder.c
                    ${CMAKE_SOURCE_DIR}/platform/online/lobby_view_model.c
                    ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_preflight.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_roster_runtime.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_roster.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_crypto.cpp
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_transcript.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_graph.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_transport.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_failure_ring.c
                    ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_input_bundle.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_input_repair.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_impairment.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_input.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_local_input.c
                    ${CMAKE_SOURCE_DIR}/platform/session/session_bridge.c
                    ${CMAKE_SOURCE_DIR}/platform/session/session_core.c
                    ${CMAKE_SOURCE_DIR}/platform/input_tick_queue.c
                    ${CMAKE_SOURCE_DIR}/platform/sha256.c
                    ${CMAKE_SOURCE_DIR}/platform/party/party_retry_policy.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp)
                target_compile_definitions(mdkr_online_live_adapter_beta_test
                    PRIVATE MDKR_ENABLE_ONLINE_BETA=1)
                target_include_directories(mdkr_online_live_adapter_beta_test
                    PRIVATE
                    ${CMAKE_SOURCE_DIR}
                    ${CMAKE_SOURCE_DIR}/platform
                    ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                    ${MDKR_PARTY_GENERATED_INCLUDE})
                target_include_directories(mdkr_online_live_adapter_beta_test
                    SYSTEM PRIVATE
                    ${mdkr_libdatachannel_SOURCE_DIR}/include)
                target_compile_features(mdkr_online_live_adapter_beta_test PRIVATE
                    cxx_std_17)
                target_link_libraries(mdkr_online_live_adapter_beta_test PRIVATE
                    LibDataChannel::LibDataChannel MbedTLS::MbedTLS
                    Threads::Threads)
                if(WIN32)
                    target_link_libraries(mdkr_online_live_adapter_beta_test
                        PRIVATE ws2_32)
                endif()
                if(MSVC)
                    target_compile_options(mdkr_online_live_adapter_beta_test
                        PRIVATE /W4 /WX)
                else()
                    target_compile_options(mdkr_online_live_adapter_beta_test
                        PRIVATE -Wall -Wextra -Wpedantic -Werror)
                endif()
                add_test(NAME online_live_adapter_beta
                    COMMAND mdkr_online_live_adapter_beta_test)

                # W3 connection robustness: the production MatchRoom transport
                # (platform/online/match_live_transport.cpp) driven wire-level
                # against a scriptable loopback fake of the MatchRoom service
                # (HTTP create/command + the /connect RFC 6455 state socket),
                # plus the mesh signal backend against the loopback
                # signal fake. Pins the /connect reconnect ladder + revision
                # dedupe, terminal 4000-class closes, the per-address connect
                # budget, the bounded resolve, and the /signal replacement
                # socket. ROM-, GPU- and network-free (127.0.0.1 only).
                add_executable(mdkr_match_live_transport_test
                    ${CMAKE_SOURCE_DIR}/tests/test_match_live_transport.cpp
                    ${CMAKE_SOURCE_DIR}/tests/match_signal_test_server.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_live_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_signal_client.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
                    ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp)
                target_include_directories(mdkr_match_live_transport_test PRIVATE
                    ${CMAKE_SOURCE_DIR}
                    ${CMAKE_SOURCE_DIR}/platform
                    ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                    ${MDKR_PARTY_GENERATED_INCLUDE})
                target_compile_features(mdkr_match_live_transport_test PRIVATE
                    cxx_std_17)
                target_link_libraries(mdkr_match_live_transport_test PRIVATE
                    MbedTLS::MbedTLS Threads::Threads)
                if(WIN32)
                    target_link_libraries(mdkr_match_live_transport_test PRIVATE
                        ws2_32)
                endif()
                if(MSVC)
                    target_compile_options(mdkr_match_live_transport_test PRIVATE
                        /W4 /WX)
                else()
                    target_compile_options(mdkr_match_live_transport_test PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
                add_test(NAME match_live_transport
                    COMMAND mdkr_match_live_transport_test)
                # Deliberate multi-second waits inside (per-address budget,
                # abandoned-resolver drain); the ctest timeout backstops a
                # genuine hang only.
                set_tests_properties(match_live_transport PROPERTIES TIMEOUT 120)

                # W3 N7: coverage-guided fuzzers for the two hand-rolled wire
                # parser families (the signal client's RFC 6455 frame
                # extractor + server-message validators, and the MatchRoom
                # transport's HTTP response / /connect WS frame / lobby-JSON
                # parsers), each driven through a shipped *_fuzz_wire seam so
                # the corpus exercises the EXACT production code. OFF by
                # default and additive: a normal build never configures or
                # links these targets. Seed corpora live in
                # tests/fuzz_corpus/; see tests/README.md.
                if(MDKR_ENABLE_FUZZERS)
                    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                        message(FATAL_ERROR
                            "MDKR_ENABLE_FUZZERS requires Clang/libFuzzer")
                    endif()
                    add_executable(mdkr_match_signal_wire_fuzzer
                        ${CMAKE_SOURCE_DIR}/tests/fuzz_match_signal_wire.cpp
                        ${CMAKE_SOURCE_DIR}/platform/online/match_signal_client.cpp
                        ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp)
                    add_executable(mdkr_online_live_wire_fuzzer
                        ${CMAKE_SOURCE_DIR}/tests/fuzz_online_live_wire.cpp
                        ${CMAKE_SOURCE_DIR}/platform/online/match_live_transport.cpp
                        ${CMAKE_SOURCE_DIR}/platform/online/match_signal_client.cpp
                        ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
                        ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp)
                    foreach(MDKR_WIRE_FUZZER
                            mdkr_match_signal_wire_fuzzer
                            mdkr_online_live_wire_fuzzer)
                        target_include_directories(${MDKR_WIRE_FUZZER} PRIVATE
                            ${CMAKE_SOURCE_DIR}
                            ${CMAKE_SOURCE_DIR}/platform
                            ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                            ${MDKR_PARTY_GENERATED_INCLUDE})
                        target_compile_features(${MDKR_WIRE_FUZZER} PRIVATE
                            cxx_std_17)
                        target_link_libraries(${MDKR_WIRE_FUZZER} PRIVATE
                            MbedTLS::MbedTLS Threads::Threads)
                        if(WIN32)
                            target_link_libraries(${MDKR_WIRE_FUZZER} PRIVATE
                                ws2_32)
                        endif()
                        target_compile_options(${MDKR_WIRE_FUZZER} PRIVATE
                            -fsanitize=fuzzer,address,undefined
                            -fno-omit-frame-pointer)
                        target_link_options(${MDKR_WIRE_FUZZER} PRIVATE
                            -fsanitize=fuzzer,address,undefined)
                    endforeach()
                endif()

                # O2.2-sim: the automatable NET-01 determinism matrix. The same
                # two-endpoint mesh race, replayed under EACH of the six named
                # net_impairment carrier profiles (LAN..ADVERSARIAL). Convergent
                # profiles must still reach a byte-identical confirmed state hash
                # within the 30-tick rollback window; a profile that exceeds the
                # window (the 2 s outage) must fire the TYPED recovery path
                # (INPUT_GAP) rather than a silent desync. Impairment is a
                # test-only injection gated behind the race seam
                # (race_drain_local + carrier-gated race_resend); production
                # match_peer_transport / match_transport behaviour is untouched.
                # ROM-, GPU- and (external-)network-free -- part of the same
                # ROM-free family as online_live_adapter.
                add_test(NAME online_live_matrix
                    COMMAND mdkr_online_live_adapter_test --matrix)
                # Six full bring-up + 240-tick impaired races over real DTLS.
                set_tests_properties(online_live_matrix PROPERTIES TIMEOUT 360)

                # N5 A5/B1: the pre-flight route-quality measurement over the
                # same two real loopback DTLS meshes. Both endpoints replay
                # both lanes at their real cadence and payload size, exchange
                # the record in their MPF2 reports and must agree on the band;
                # a second run with a round trip a single authored tick cannot
                # absorb must widen BOTH endpoints' entry timing above the
                # manifest floor while the two still converge byte for byte.
                # The probe/echo round trip is timed against the adapter's
                # injected clock, so no external network is involved.
                add_test(NAME online_live_route
                    COMMAND mdkr_online_live_adapter_test --route)
                set_tests_properties(online_live_route PROPERTIES TIMEOUT 300)

                # N7 B3/B5: explicit input-gap repair on the reliable
                # unordered authority channel, over the same two real loopback
                # DTLS meshes. Twenty-five consecutive bundle sends in one
                # direction are suppressed -- far past the carrier's three
                # ticks of redundancy -- and the run that costs must be closed
                # by repair alone, with both endpoints committing the identical
                # canonical timeline. Two positive controls: the identical
                # burst with repair off must reach typed rollback exhaustion,
                # and the burst must not age out a valid authority packet.
                add_test(NAME online_live_repair
                    COMMAND mdkr_online_live_adapter_test --repair)
                set_tests_properties(online_live_repair PROPERTIES TIMEOUT 300)

                # online multiplayer CAPSTONE end-to-end race driver: the
                # REAL online stack (production MatchRoom HTTP + /connect WS
                # client, the signal-client mesh backend, the peer
                # mesh over real libdatachannel DTLS, the live adapter, the
                # clamp and the per-tick race feed). Deliberately no
                # add_test() -- it needs a live local MatchRoom Worker and a
                # second driver process that only tests/
                # check_online_live_transport_e2e.py provides, the same
                # script-owns-binary split the Party e2e driver uses.
                add_executable(mdkr_online_live_transport_e2e_driver
                    ${CMAKE_SOURCE_DIR}/tests/test_online_live_transport_e2e_driver.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_live_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_live_adapter.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_peer_liveness.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/online_track_table.c
                    ${CMAKE_SOURCE_DIR}/platform/online/match_peer_transport.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_peer_lost_reason.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_signal_client.cpp
                    ${CMAKE_SOURCE_DIR}/platform/online/match_launch_builder.c
                    ${CMAKE_SOURCE_DIR}/platform/online/lobby_view_model.c
                    ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
                    ${CMAKE_SOURCE_DIR}/platform/online/lobby_fake_adapter.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_preflight.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_roster_runtime.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_roster.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_crypto.cpp
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_transcript.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_peer_graph.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_transport.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_failure_ring.c
                    ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_input_bundle.c
                    ${CMAKE_SOURCE_DIR}/platform/net/match_input_repair.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_input.c
                    ${CMAKE_SOURCE_DIR}/platform/net/net_local_input.c
                    ${CMAKE_SOURCE_DIR}/platform/session/session_bridge.c
                    ${CMAKE_SOURCE_DIR}/platform/session/session_core.c
                    ${CMAKE_SOURCE_DIR}/platform/input_tick_queue.c
                    ${CMAKE_SOURCE_DIR}/platform/sha256.c
                    ${CMAKE_SOURCE_DIR}/platform/party/party_retry_policy.cpp
                    ${CMAKE_SOURCE_DIR}/platform/party/party_webrtc_signaling.cpp)
                target_include_directories(
                    mdkr_online_live_transport_e2e_driver PRIVATE
                    ${CMAKE_SOURCE_DIR}
                    ${CMAKE_SOURCE_DIR}/platform
                    ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                    ${MDKR_PARTY_GENERATED_INCLUDE})
                target_include_directories(
                    mdkr_online_live_transport_e2e_driver SYSTEM PRIVATE
                    ${mdkr_libdatachannel_SOURCE_DIR}/include)
                target_compile_features(
                    mdkr_online_live_transport_e2e_driver PRIVATE cxx_std_17)
                target_link_libraries(
                    mdkr_online_live_transport_e2e_driver PRIVATE
                    LibDataChannel::LibDataChannel MbedTLS::MbedTLS
                    Threads::Threads)
                if(WIN32)
                    target_link_libraries(
                        mdkr_online_live_transport_e2e_driver PRIVATE ws2_32)
                endif()
                if(MSVC)
                    target_compile_options(
                        mdkr_online_live_transport_e2e_driver PRIVATE /W4 /WX)
                else()
                    target_compile_options(
                        mdkr_online_live_transport_e2e_driver PRIVATE
                        -Wall -Wextra -Wpedantic -Werror)
                endif()
            endif()
        endif()

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
        # generated/ carries the embedded brand hero; lib/stb is the decoder it
        # is handed to (declarations only -- the implementation TU is elsewhere).
        target_include_directories(mdkr64_app PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated
            ${CMAKE_SOURCE_DIR}/lib/stb)
        if(MDKR_NATIVE_PHONE_PARTY)
            target_include_directories(mdkr64_app PRIVATE
                ${MDKR_LIBDATACHANNEL_JSON_INCLUDE}
                ${MDKR_PARTY_GENERATED_INCLUDE})
            target_include_directories(mdkr64_app SYSTEM PRIVATE
                ${mdkr_libdatachannel_SOURCE_DIR}/include)
            target_link_libraries(mdkr64_app PRIVATE
                LibDataChannel::LibDataChannel MbedTLS::MbedTLS)
            if(WIN32)
                # The app's own online/party TUs call Winsock directly
                # (WSAStartup/closesocket in match_signal_client.cpp,
                # match_live_transport.cpp and lan_party_server.cpp), so link
                # ws2_32 explicitly instead of relying on LibDataChannel/webgpu
                # to propagate it through their link interfaces. As a static lib
                # this rides through to mdkr64. IPHLPAPI is not linked here: no
                # app-owned TU calls it directly; it arrives with libjuice's
                # interface enumeration. Matches the if(WIN32) ws2_32 link every
                # online test target already carries.
                target_link_libraries(mdkr64_app PRIVATE ws2_32)
            endif()
        endif()
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
            MDKR_ENABLE_ONLINE_ROOM_PREVIEW=$<BOOL:${MDKR_ENABLE_ONLINE_ROOM_PREVIEW}>
            MDKR_APP_VERSION_STR="${MDKR_VERSION}"
            MDKR_APP_BUILD_STAMP="${MDKR_BUILD_STAMP}"
            MDKR_PARTY_ORIGIN="${MDKR_PARTY_COMPILED_ORIGIN}")
        if(MDKR_ENABLE_ONLINE_BETA)
            # Turns the header seam (OnlineRoom_makeGatedLiveAdapter, the token
            # gate, the panel-visibility gate) live and compiles the wiring TU.
            # Beta-only, so a normal build never sees this macro or the online TUs.
            target_compile_definitions(mdkr64_app PRIVATE MDKR_ENABLE_ONLINE_BETA=1)
            target_include_directories(mdkr64_app PRIVATE ${CMAKE_SOURCE_DIR})
        endif()
        target_include_directories(mdkr64_app PRIVATE
            ${IMGUI_DIR}
            ${IMGUI_DIR}/backends
            ${CMAKE_SOURCE_DIR}/platform
            ${CMAKE_SOURCE_DIR}/platform/app
            ${CMAKE_SOURCE_DIR}/lib/glad/include
            ${CMAKE_SOURCE_DIR}/lib/miniz
            ${CMAKE_SOURCE_DIR}/lib/stb
            ${SDL2_INCLUDE_DIRS})
        target_link_directories(mdkr64_app PRIVATE ${SDL2_LIBRARY_DIRS})
        target_link_libraries(mdkr64_app PRIVATE mdkr_qrcodegen)
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
            # comdlg32: GetOpenFileNameW. shell32: reveal a retained package in
            # Explorer without opening it. Both are stock Windows system DLLs
            # and deliberate, documented additions to the import allowlist in
            # .github/workflows/windows-validate.yml and tools/mingw_cross_check.sh.
            target_link_libraries(mdkr64_app PUBLIC
                OpenGL::GL comdlg32 shell32)
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
                ${CMAKE_SOURCE_DIR}/platform/modern_character_gameplay_profile.c
                ${CMAKE_SOURCE_DIR}/platform/rom_id.c
                ${CMAKE_SOURCE_DIR}/platform/rom_validation.c
                ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
                ${CMAKE_SOURCE_DIR}/platform/sha256.c)
            target_include_directories(mdkr_app_shell_test PRIVATE
                ${CMAKE_SOURCE_DIR}/game/include
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_app_shell_test PRIVATE cxx_std_17)
            add_test(NAME app_shell COMMAND mdkr_app_shell_test)

            add_executable(mdkr_session_runtime_test
                ${CMAKE_SOURCE_DIR}/tests/test_session_runtime.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/session_runtime.cpp
                ${CMAKE_SOURCE_DIR}/platform/session/session_core.c
                ${CMAKE_SOURCE_DIR}/platform/session/session_bridge.c
                ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
                ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c
                ${CMAKE_SOURCE_DIR}/platform/net/net_roster.c
                ${CMAKE_SOURCE_DIR}/platform/net/net_local_input.c
                ${CMAKE_SOURCE_DIR}/platform/net/net_input.c
                ${CMAKE_SOURCE_DIR}/platform/net/match_transport.c
                ${CMAKE_SOURCE_DIR}/platform/net/net_failure_ring.c
                ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c)
            target_include_directories(mdkr_session_runtime_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_session_runtime_test PRIVATE cxx_std_17)
            add_test(NAME session_runtime COMMAND mdkr_session_runtime_test)

            add_executable(mdkr_app_ui_policy_test
                ${CMAKE_SOURCE_DIR}/tests/test_app_ui_policy.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/app_ui_policy.cpp)
            target_include_directories(mdkr_app_ui_policy_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_app_ui_policy_test PRIVATE cxx_std_17)
            add_test(NAME app_ui_policy COMMAND mdkr_app_ui_policy_test)

            add_executable(mdkr_character_revision_index_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_revision_index.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_revision_index.cpp)
            target_include_directories(mdkr_character_revision_index_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_revision_index_test PRIVATE cxx_std_17)
            add_test(NAME character_revision_index
                COMMAND mdkr_character_revision_index_test)

            add_executable(mdkr_character_failure_index_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_failure_index.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_failure_index.cpp)
            target_include_directories(mdkr_character_failure_index_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_failure_index_test PRIVATE cxx_std_17)
            add_test(NAME character_failure_index
                COMMAND mdkr_character_failure_index_test)

            add_executable(mdkr_character_candidate_index_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_candidate_index.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_candidate_index.cpp)
            target_include_directories(mdkr_character_candidate_index_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_candidate_index_test PRIVATE cxx_std_17)
            add_test(NAME character_candidate_index
                COMMAND mdkr_character_candidate_index_test)

            add_executable(mdkr_character_edit_history_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_edit_history.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_edit_history.cpp)
            target_include_directories(mdkr_character_edit_history_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_edit_history_test PRIVATE cxx_std_17)
            add_test(NAME character_edit_history
                COMMAND mdkr_character_edit_history_test)

            add_executable(mdkr_character_raw_intake_index_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_raw_intake_index.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_raw_intake_index.cpp)
            target_include_directories(mdkr_character_raw_intake_index_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_raw_intake_index_test PRIVATE cxx_std_17)
            add_test(NAME character_raw_intake_index
                COMMAND mdkr_character_raw_intake_index_test)

            add_executable(mdkr_character_adapter_output_index_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_adapter_output_index.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_adapter_output_index.cpp)
            target_include_directories(mdkr_character_adapter_output_index_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_adapter_output_index_test PRIVATE cxx_std_17)
            add_test(NAME character_adapter_output_index
                COMMAND mdkr_character_adapter_output_index_test)

            add_executable(mdkr_character_raw_draft_store_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_raw_draft_store.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_raw_draft_store.cpp
                ${CMAKE_SOURCE_DIR}/platform/sha256.c)
            target_include_directories(mdkr_character_raw_draft_store_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_raw_draft_store_test PRIVATE cxx_std_17)
            add_test(NAME character_raw_draft_store
                COMMAND mdkr_character_raw_draft_store_test)

            add_executable(mdkr_character_spdx_expression_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_spdx_expression.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_spdx_expression.cpp)
            target_include_directories(mdkr_character_spdx_expression_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_spdx_expression_test PRIVATE cxx_std_17)
            add_test(NAME character_spdx_expression
                COMMAND mdkr_character_spdx_expression_test)

            add_executable(mdkr_character_test_evidence_store_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_test_evidence_store.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_test_evidence_store.cpp
                ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
                ${CMAKE_SOURCE_DIR}/platform/modern_character_gpu_timing.c
                ${CMAKE_SOURCE_DIR}/platform/sha256.c)
            target_include_directories(mdkr_character_test_evidence_store_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_test_evidence_store_test PRIVATE cxx_std_17)
            add_test(NAME character_test_evidence_store
                COMMAND mdkr_character_test_evidence_store_test)

            add_executable(mdkr_character_draft_store_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_draft_store.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_draft_store.cpp
                ${CMAKE_SOURCE_DIR}/platform/sha256.c)
            target_include_directories(mdkr_character_draft_store_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_draft_store_test PRIVATE cxx_std_17)
            add_test(NAME character_draft_store
                COMMAND mdkr_character_draft_store_test)

            add_executable(mdkr_character_draft_snapshot_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_draft_snapshot.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_draft_snapshot.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_png_validation.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_portrait_import.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_portrait_studio.cpp
                ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
                ${CMAKE_SOURCE_DIR}/platform/sha256.c
                ${CMAKE_SOURCE_DIR}/lib/stb/stb_image_impl.c)
            target_include_directories(mdkr_character_draft_snapshot_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app
                ${CMAKE_SOURCE_DIR}/lib/stb)
            target_compile_features(mdkr_character_draft_snapshot_test PRIVATE cxx_std_17)
            add_test(NAME character_draft_snapshot
                COMMAND mdkr_character_draft_snapshot_test)

            add_executable(mdkr_character_draft_transfer_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_draft_transfer.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_draft_transfer.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_draft_store.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_draft_snapshot.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_png_validation.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_portrait_import.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_portrait_studio.cpp
                ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
                ${CMAKE_SOURCE_DIR}/platform/sha256.c
                ${CMAKE_SOURCE_DIR}/lib/stb/stb_image_impl.c)
            target_include_directories(mdkr_character_draft_transfer_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app
                ${CMAKE_SOURCE_DIR}/lib/stb)
            target_compile_features(mdkr_character_draft_transfer_test PRIVATE
                cxx_std_17)
            add_test(NAME character_draft_transfer
                COMMAND mdkr_character_draft_transfer_test)

            add_executable(mdkr_workshop_preview_runtime_test
                ${CMAKE_SOURCE_DIR}/tests/test_workshop_preview_runtime.c
                ${CMAKE_SOURCE_DIR}/platform/workshop_preview_runtime.c)
            target_include_directories(mdkr_workshop_preview_runtime_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform)
            add_test(NAME workshop_preview_runtime
                COMMAND mdkr_workshop_preview_runtime_test)

            add_executable(mdkr_character_visual_report_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_visual_report.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_png_validation.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_visual_report.cpp
                ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
                ${CMAKE_SOURCE_DIR}/platform/sha256.c)
            target_include_directories(mdkr_character_visual_report_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_visual_report_test PRIVATE
                cxx_std_17)
            add_test(NAME character_visual_report
                COMMAND mdkr_character_visual_report_test)

            add_executable(mdkr_character_preview_cache_test
                ${CMAKE_SOURCE_DIR}/tests/test_character_preview_cache.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/character_preview_cache.cpp
                ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
                ${CMAKE_SOURCE_DIR}/platform/sha256.c)
            target_include_directories(mdkr_character_preview_cache_test PRIVATE
                ${CMAKE_SOURCE_DIR}/platform
                ${CMAKE_SOURCE_DIR}/platform/app)
            target_compile_features(mdkr_character_preview_cache_test PRIVATE
                cxx_std_17)
            add_test(NAME character_preview_cache
                COMMAND mdkr_character_preview_cache_test)

            add_executable(mdkr_app_config_test
                ${CMAKE_SOURCE_DIR}/tests/test_app_config.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/app_config.cpp
                ${CMAKE_SOURCE_DIR}/platform/app/app_config_escape.cpp
                ${CMAKE_SOURCE_DIR}/platform/user_paths.c
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
            # This unit supplies SDL stubs and its own console entry point;
            # SDL's Windows main remapping must not require an SDL2main shim.
            target_compile_definitions(mdkr_app_window_test PRIVATE SDL_MAIN_HANDLED)
            add_test(NAME app_window COMMAND mdkr_app_window_test)

            add_test(NAME app_background_activation_contract
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/check_app_background_activation.py)

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
            set_tests_properties(cli_version_exact PROPERTIES LABELS app_process)

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
                "recommended=\"Original \\(recommended\\)\" group=\"Higher refresh rates\" caveat=\"How often the app draws to your screen. It never changes how fast the game runs. Original draws each picture the game makes, once. higher rates repeat it, or draw new in-between pictures when Motion smoothing is Interpolated. Just Under Display suits a variable-refresh display, and 40 Hz suits a handheld screen that runs at 40 or 120 Hz. Rates above your display's refresh only apply where the system can drop a picture it has not shown yet, and a browser maps Uncapped and Just Under Display to Match Display.\"")

            add_test(NAME app_schema COMMAND ${MDKR_TARGET})
            set_tests_properties(app_schema PROPERTIES
                ENVIRONMENT "MDKR_APP_DUMP_SCHEMA=1;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                LABELS app_process
                PASS_REGULAR_EXPRESSION "${MDKR_FRAME_LIMIT_UI_CONTRACT_REGEX}")

            # Issue #62: the actual widget's option routing must expose all
            # three choices, with readable labels, instead of a text field.
            add_test(NAME app_opponent_skill_choices COMMAND ${MDKR_TARGET})
            set_tests_properties(app_opponent_skill_choices PROPERTIES
                ENVIRONMENT "MDKR_APP_DUMP_SCHEMA=1;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                LABELS app_process
                PASS_REGULAR_EXPRESSION
                    "choices key=Enhancements.AIDifficulty count=3 authored=\"Original — as released\" hard=\"Hard — faster opponents\" brutal=\"Brutal — fastest opponents\"")

            # Shell render smoke: drive the launcher for a few frames and require
            # a real captured frame. This is what makes the gate honest — it
            # proves pixels reached a framebuffer, not merely that main() returned
            # 0 (main_app.cpp fails the run when a requested capture is missing).
            # These tests create real native graphics surfaces. Keep them out
            # of ordinary build trees entirely: a plain `ctest` must be safe
            # on an interactive workstation, even when the caller forgets
            # label exclusion. Release evidence enables them explicitly on a
            # dedicated test desktop. MDKR_SKIP_GPU_TESTS remains a backwards-
            # compatible emergency override for existing automation.
            if(MDKR_ENABLE_GPU_TESTS AND NOT MDKR_SKIP_GPU_TESTS)
                set(MDKR_APP_TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/app-test-data")
                file(MAKE_DIRECTORY
                    "${MDKR_APP_TEST_ROOT}/shell/prefs"
                    "${MDKR_APP_TEST_ROOT}/shell/saves"
                    "${MDKR_APP_TEST_ROOT}/settings/prefs"
                    "${MDKR_APP_TEST_ROOT}/settings/saves"
                    "${MDKR_APP_TEST_ROOT}/workshop/prefs"
                    "${MDKR_APP_TEST_ROOT}/workshop/saves"
                    "${MDKR_APP_TEST_ROOT}/workshop-min/prefs"
                    "${MDKR_APP_TEST_ROOT}/workshop-min/saves"
                    "${MDKR_APP_TEST_ROOT}/online/prefs"
                    "${MDKR_APP_TEST_ROOT}/online/saves"
                    "${MDKR_APP_TEST_ROOT}/online-min/prefs"
                    "${MDKR_APP_TEST_ROOT}/online-min/saves"
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
                    "${MDKR_APP_TEST_ROOT}/wheel-handheld/prefs"
                    "${MDKR_APP_TEST_ROOT}/wheel-handheld/saves"
                    "${MDKR_APP_TEST_ROOT}/wheel-flipped/prefs"
                    "${MDKR_APP_TEST_ROOT}/wheel-flipped/saves"
                    "${MDKR_APP_TEST_ROOT}/dpi/prefs"
                    "${MDKR_APP_TEST_ROOT}/dpi/saves"
                    "${MDKR_APP_TEST_ROOT}/recovery/prefs"
                    "${MDKR_APP_TEST_ROOT}/recovery/saves"
                    "${MDKR_APP_TEST_ROOT}/gl/prefs"
                    "${MDKR_APP_TEST_ROOT}/gl/saves")
                file(REMOVE
                    "${MDKR_APP_TEST_ROOT}/shell/video.ini"
                    "${MDKR_APP_TEST_ROOT}/settings/video.ini"
                    "${MDKR_APP_TEST_ROOT}/workshop/video.ini"
                    "${MDKR_APP_TEST_ROOT}/workshop-min/video.ini"
                    "${MDKR_APP_TEST_ROOT}/online/video.ini"
                    "${MDKR_APP_TEST_ROOT}/online-min/video.ini"
                    "${MDKR_APP_TEST_ROOT}/top-tabs/video.ini"
                    "${MDKR_APP_TEST_ROOT}/top-tabs-small/video.ini"
                    "${MDKR_APP_TEST_ROOT}/controller/video.ini"
                    "${MDKR_APP_TEST_ROOT}/minimum/video.ini"
                    "${MDKR_APP_TEST_ROOT}/touch-handheld/video.ini"
                    "${MDKR_APP_TEST_ROOT}/wheel-handheld/video.ini"
                    "${MDKR_APP_TEST_ROOT}/wheel-flipped/video.ini"
                    "${MDKR_APP_TEST_ROOT}/dpi/video.ini"
                    "${MDKR_APP_TEST_ROOT}/recovery/video.ini"
                    "${MDKR_APP_TEST_ROOT}/gl/video.ini")
                file(WRITE "${MDKR_APP_TEST_ROOT}/shell/prefs/mdkr64_app.ini"
                    "# isolated first-run shell\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/settings/prefs/mdkr64_app.ini"
                    "# isolated first-run settings\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/workshop/prefs/mdkr64_app.ini"
                    "# isolated empty Character Workshop\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/workshop-min/prefs/mdkr64_app.ini"
                    "# minimum Character Workshop\nui_scale=2.00\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/online/prefs/mdkr64_app.ini"
                    "# isolated launcher-owned online preview\n")
                file(WRITE "${MDKR_APP_TEST_ROOT}/online-min/prefs/mdkr64_app.ini"
                    "# minimum online preview\nui_scale=2.00\n")
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

                # Character authoring is a dedicated launcher destination, not
                # a settings accordion. This empty-library route proves the
                # named panel, responsive shell, importer onboarding, and
                # render/present path without a ROM or third-party asset.
                add_test(NAME app_character_workshop_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_character_workshop_smoke PROPERTIES
                    FIXTURES_SETUP app_character_workshop_capture
                    WORKING_DIRECTORY "${MDKR_APP_TEST_ROOT}/workshop"
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=6;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_character_workshop.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=1280x720;MDKR_APP_PANEL=Character Workshop;MDKR_APP_UI_TRACE=1;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/workshop/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/workshop/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/workshop/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION "active-panel=Character Workshop workshop-primary=contextual rom=optional"
                    TIMEOUT 180)
                add_test(NAME app_character_workshop_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_app_capture.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_character_workshop.bmp
                        --self-test)
                set_tests_properties(app_character_workshop_content PROPERTIES
                    FIXTURES_REQUIRED app_character_workshop_capture
                    DEPENDS app_character_workshop_smoke
                    LABELS gpu
                    TIMEOUT 60)

                add_test(NAME app_character_workshop_minimum_smoke
                    COMMAND ${MDKR_TARGET})
                set_tests_properties(app_character_workshop_minimum_smoke PROPERTIES
                    FIXTURES_SETUP app_character_workshop_minimum_capture
                    WORKING_DIRECTORY "${MDKR_APP_TEST_ROOT}/workshop-min"
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=10;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_character_workshop_minimum.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=640x480;MDKR_APP_PANEL=Character Workshop;MDKR_APP_UI_TRACE=1;MDKR_APP_SMOKE_TOUCH_SCROLL=1;MDKR_APP_SMOKE_TOUCH_TOKEN=mdkr64-app-touch-v1;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/workshop-min/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/workshop-min/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/workshop-min/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION "compact-layout dense=1 contained=1 overlap=0 contentSeparated=1 primaryLabelContained=1;character-workshop-layout compact=1"
                    TIMEOUT 180)
                add_test(NAME app_character_workshop_minimum_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_app_capture.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_character_workshop_minimum.bmp
                        --compact-palette
                        --self-test)
                set_tests_properties(app_character_workshop_minimum_content PROPERTIES
                    FIXTURES_REQUIRED app_character_workshop_minimum_capture
                    DEPENDS app_character_workshop_minimum_smoke
                    LABELS gpu
                    TIMEOUT 60)

                add_test(NAME app_character_raw_intake
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_character_raw_intake_ui.py
                        --build ${CMAKE_CURRENT_BINARY_DIR})
                set_tests_properties(app_character_raw_intake PROPERTIES
                    DEPENDS app_character_workshop_smoke
                    LABELS gpu
                    TIMEOUT 180)

                add_test(NAME app_character_quit_lifecycle
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_character_quit_lifecycle_ui.py
                        --build ${CMAKE_CURRENT_BINARY_DIR})
                set_tests_properties(app_character_quit_lifecycle PROPERTIES
                    DEPENDS app_character_workshop_smoke
                    LABELS gpu
                    TIMEOUT 90)

                add_test(NAME app_character_workshop_history
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_character_workshop_history_ui.py
                        --build ${CMAKE_CURRENT_BINARY_DIR})
                set_tests_properties(app_character_workshop_history PROPERTIES
                    DEPENDS app_character_workshop_smoke
                    LABELS gpu
                    TIMEOUT 180)

                add_test(NAME app_character_draft_transfer
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_character_draft_transfer_ui.py
                        --build ${CMAKE_CURRENT_BINARY_DIR})
                set_tests_properties(app_character_draft_transfer PROPERTIES
                    DEPENDS app_character_workshop_smoke
                    LABELS gpu
                    TIMEOUT 300)

                add_test(NAME app_character_test_evidence
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_character_test_evidence_ui.py
                        --build ${CMAKE_CURRENT_BINARY_DIR})
                set_tests_properties(app_character_test_evidence PROPERTIES
                    DEPENDS app_character_workshop_smoke
                    LABELS gpu
                    TIMEOUT 300)

                add_test(NAME app_character_portrait_studio
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_character_portrait_studio_ui.py
                        --build ${CMAKE_CURRENT_BINARY_DIR})
                set_tests_properties(app_character_portrait_studio PROPERTIES
                    DEPENDS app_character_workshop_smoke
                    LABELS gpu
                    TIMEOUT 180)

                # The launcher-owned Online Room is a real UI surface even
                # while public online racing remains release-gated. Exercise
                # it through the deterministic reducer adapter so screenshots
                # never depend on a provider, a network, or mutable service
                # state. The production build still makes no network request
                # unless this explicit test-only preview flag is present.
                add_test(NAME app_online_room_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_online_room_smoke PROPERTIES
                    FIXTURES_SETUP app_online_room_capture
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=8;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_online_room.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=1280x720;MDKR_APP_PANEL=Online Room;MDKR_ONLINE_ROOM_PREVIEW=1;MDKR_APP_ONLINE_FAKE=1;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/online/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/online/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/online/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    TIMEOUT 180)

                add_test(NAME app_online_room_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_app_capture.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_online_room.bmp
                        --self-test)
                set_tests_properties(app_online_room_content PROPERTIES
                    FIXTURES_REQUIRED app_online_room_capture
                    DEPENDS app_online_room_smoke
                    LABELS gpu
                    TIMEOUT 60)

                # Minimum supported logical window at maximum UI scale. This
                # is the hostile-but-supported configuration for long failure
                # copy, large controls and the dense section selector.
                add_test(NAME app_online_room_minimum_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_online_room_minimum_smoke PROPERTIES
                    FIXTURES_SETUP app_online_room_minimum_capture
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=10;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_online_room_minimum.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=640x480;MDKR_APP_PANEL=Online Room;MDKR_ONLINE_ROOM_PREVIEW=1;MDKR_APP_ONLINE_FAKE=1;MDKR_APP_UI_TRACE=1;MDKR_APP_SMOKE_TOUCH_SCROLL=1;MDKR_APP_SMOKE_TOUCH_TOKEN=mdkr64-app-touch-v1;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/online-min/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/online-min/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/online-min/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    PASS_REGULAR_EXPRESSION "compact-layout dense=1 contained=1 overlap=0 contentSeparated=1"
                    LABELS gpu
                    TIMEOUT 180)

                add_test(NAME app_online_room_minimum_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_app_capture.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_online_room_minimum.bmp
                        --compact-palette
                        --self-test)
                set_tests_properties(app_online_room_minimum_content PROPERTIES
                    FIXTURES_REQUIRED app_online_room_minimum_capture
                    DEPENDS app_online_room_minimum_smoke
                    LABELS gpu
                    TIMEOUT 60)

                # Discover the scenario list from the product binary, then
                # launch one isolated renderer process per case. This is the
                # deterministic storybook gate for every view and typed
                # failure; it cannot silently omit a newly published case.
                add_test(NAME app_online_room_gallery
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_online_room_gallery.py
                        --build $<TARGET_FILE:${MDKR_TARGET}>)
                set_tests_properties(app_online_room_gallery PROPERTIES
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION
                        "PASS online room gallery: cases=43 views=10 failures=18 unique-captures=43"
                    TIMEOUT 300)

                add_test(NAME app_online_room_accessibility
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_online_room_a11y.py
                        --build $<TARGET_FILE:${MDKR_TARGET}>)
                set_tests_properties(app_online_room_accessibility PROPERTIES
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION
                        "PASS online room accessibility: cases=43 titles=43 keyboard-action-sets=43"
                    TIMEOUT 300)

                # One representative rendered case for every public action,
                # activated once through SDL keyboard and once through the
                # deterministic virtual gamepad. The executable publishes the
                # action inventory, so adding a CTA without a working route
                # fails this gate automatically.
                add_test(NAME app_online_room_actions
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_online_room_actions.py
                        --build $<TARGET_FILE:${MDKR_TARGET}>)
                set_tests_properties(app_online_room_actions PROPERTIES
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION
                        "PASS online room actions: actions=27 keyboard=27 gamepad=27"
                    TIMEOUT 300)

                # The modal online lobby must fully take over the launcher while
                # a live session is active: the nav rail, top tabs and the
                # generic offline Play are suppressed so the offline launch is
                # unreachable (the root fix for both reported online bugs). The
                # per-frame probe is beta-only, so this gate is registered only
                # for the native online beta build.
                if(MDKR_ENABLE_ONLINE_BETA)
                    add_test(NAME app_online_lobby_takeover
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_SOURCE_DIR}/tests/check_online_lobby_takeover.py
                            --build $<TARGET_FILE:${MDKR_TARGET}>)
                    set_tests_properties(app_online_lobby_takeover PROPERTIES
                        LABELS gpu
                        PASS_REGULAR_EXPRESSION
                            "PASS online lobby takeover: active-cases=7 entry-shell=1 offline-play-suppressed=1"
                        TIMEOUT 300)
                endif()

                # The responsive launcher has three distinct navigation modes.
                # Wide sidebar and dense dropdown coverage existed already;
                # keep the intermediate top-tab layout from regressing into
                # multiple blue selections or losing its active indicator.
                add_test(NAME app_top_tabs_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_top_tabs_smoke PROPERTIES
                    FIXTURES_SETUP app_top_tabs_capture
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=4;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_top_tabs.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=800x600;MDKR_APP_PANEL=Play;MDKR_ONLINE_ROOM_PREVIEW=1;MDKR_APP_SMOKE_NAV_TARGET=2;MDKR_APP_SMOKE_NAV_TOKEN=mdkr64-app-nav-v1;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/top-tabs/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/top-tabs/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/top-tabs/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    PASS_REGULAR_EXPRESSION "top navigation target=2 actual=2 queued=1"
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
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=4;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_top_tabs_scale075.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=640x480;MDKR_APP_PANEL=Play;MDKR_ONLINE_ROOM_PREVIEW=1;MDKR_APP_SMOKE_NAV_TARGET=2;MDKR_APP_SMOKE_NAV_TOKEN=mdkr64-app-nav-v1;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/top-tabs-small/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/top-tabs-small/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/top-tabs-small/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    PASS_REGULAR_EXPRESSION "top navigation target=2 actual=2 queued=1"
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

                # A MacBook trackpad two-finger scroll: the SDL_MOUSEWHEEL the
                # scripted notch delivers carries its motion in fractional
                # preciseY with the integer y truncated to zero, driven through
                # the production SDL -> imgui_impl_sdl2 adapter. A bridge that
                # reads the integer field scrolls nothing and this fails; scroll=1
                # proves preciseY reached the hovered panel and moved its ScrollY.
                add_test(NAME app_wheel_handheld_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_wheel_handheld_smoke PROPERTIES
                    FIXTURES_SETUP app_wheel_handheld_capture
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=12;MDKR_APP_SMOKE_SHOT=${CMAKE_CURRENT_BINARY_DIR}/app_wheel_handheld.bmp;MDKR_APP_SMOKE_WINDOW_SIZE=1280x720;MDKR_APP_SMOKE_WHEEL_SCROLL=1;MDKR_APP_SMOKE_WHEEL_TOKEN=mdkr64-app-wheel-v1;MDKR_APP_PANEL=Settings;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/wheel-handheld/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/wheel-handheld/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/wheel-handheld/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION "wheel handheld scroll=1"
                    TIMEOUT 180)
                add_test(NAME app_wheel_handheld_content
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/check_app_capture.py
                        ${CMAKE_CURRENT_BINARY_DIR}/app_wheel_handheld.bmp
                        --self-test)
                set_tests_properties(app_wheel_handheld_content PROPERTIES
                    FIXTURES_REQUIRED app_wheel_handheld_capture
                    DEPENDS app_wheel_handheld_smoke
                    LABELS gpu
                    TIMEOUT 60)

                # macOS "natural scrolling" delivers the wheel as
                # SDL_MOUSEWHEEL_FLIPPED with negated deltas. The scripted notch
                # is marked FLIPPED with an inverted sign, so it only scrolls the
                # panel DOWN (scroll=1) if the flag is honored before ImGui; a
                # backend that ignores it scrolls up, clamps at the top, and this
                # fails -- the "trackpad scrolls the wrong way" defect.
                add_test(NAME app_wheel_flipped_smoke COMMAND ${MDKR_TARGET})
                set_tests_properties(app_wheel_flipped_smoke PROPERTIES
                    ENVIRONMENT "MDKR_APP_SMOKE_FRAMES=12;MDKR_APP_SMOKE_WINDOW_SIZE=1280x720;MDKR_APP_SMOKE_WHEEL_SCROLL=1;MDKR_APP_SMOKE_WHEEL_TOKEN=mdkr64-app-wheel-v1;MDKR_APP_SMOKE_WHEEL_FLIPPED=1;MDKR_APP_PANEL=Settings;MDKR_APP_PREFS_DIR=${MDKR_APP_TEST_ROOT}/wheel-flipped/prefs;MDKR_VIDEO_CONFIG_PATH=${MDKR_APP_TEST_ROOT}/wheel-flipped/video.ini;MDKR_SAVE_DIR=${MDKR_APP_TEST_ROOT}/wheel-flipped/saves;MDKR64_HIDDEN=1;MDKR_AUDIO=0"
                    LABELS gpu
                    PASS_REGULAR_EXPRESSION "wheel handheld scroll=1"
                    TIMEOUT 180)

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
    if(MDKR_CHARACTER_KTX2)
        target_compile_definitions(${MDKR_TARGET} PRIVATE MDKR_CHARACTER_KTX2=1)
        target_link_libraries(${MDKR_TARGET} PRIVATE
            mdkr_character_ktx2_bridge)
    endif()
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

    # --- Test link dead-strip (MOVED OUT of if(WIN32)) ---------------------
    # The online/transport test-link dead-strip + no-speculative-devirt flags
    # are a GCC-codegen concern -- MinGW *and* the Linux release lane, both GCC
    # -- not a Windows one, so they now live in a compiler-gated top-level block
    # AFTER this if(WIN32)...endif(). Search
    # "Test-link dead-strip / speculative-devirt (GCC lanes: MinGW + Linux)".
    # Kept test-target-only there, exactly as when it lived here.

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

# ==========================================================================
# Test-link dead-strip / speculative-devirt (GCC lanes: MinGW + Linux)
# ==========================================================================
# TRUE CONDITION: this is a GCC-codegen concern, not a Windows one. It fires on
# every GCC lane -- MinGW (cross from macOS/Linux, or MSYS2) AND the Linux
# release lane, both GCC -- and on NO Clang lane (macOS AppleClang, Linux clang,
# wasm/emscripten Clang), which need none of these flags. That is why the gate
# below is CMAKE_C_COMPILER_ID STREQUAL "GNU" (the file's existing GCC idiom,
# always defined since project() is C) rather than if(WIN32): this block was
# moved out of the trailing Windows block precisely because the Linux release
# lane is GCC and hits the identical codegen. Widening one mechanism across both
# GCC platforms keeps a single source of truth (was: MinGW-only inside if(WIN32)).
#
# Several online test targets deliberately omit a defining TU, relying on the
# omitted definition never being demanded because nothing reachable references
# it (e.g. the MdkrMatchSignalClientFeed / MdkrOnlineFakeAdapterSeam wrappers
# that would otherwise pull in match_signal_client.cpp / lobby_fake_adapter.c).
# Clang leaves the relevant virtual call INDIRECT, so no reachable code names
# the omitted symbol and the ld64/ELF links resolve with no flags at all -- this
# is why the historical macOS/Linux-CLANG links passed unflagged. GCC is
# different: it speculatively binds a virtual call whose only header-visible
# override is `final` (MdkrMatchSignalClientFeed, MdkrOnlineFakeAdapterSeam) to
# that override, embedding a DIRECT reference to the omitted definition inside a
# REACHABLE function (MdkrMatchPeerMesh::State::sendSignal/pump) that no linker
# can dead-strip. -fno-devirtualize-speculatively is the load-bearing flag: it
# removes GCC's planted reference so the deliberate omission stays sound. This
# reproduces identically on MinGW GCC (originally proven at -O2) and on the
# Linux release lane (GCC at -O3) -- the Linux exposure is exactly why this is
# no longer Windows-only. -ffunction-sections/-fdata-sections + -Wl,--gc-sections
# are kept as dead-strip parity (belt-and-braces section GC); their effect in
# isolation was never separately confirmed, but with speculation off the links
# are clean on both lanes.
#
# TEST TARGETS ONLY -- no shipping target inherits these flags under ANY
# configure. ${MDKR_TARGET} (the shipped mdkr64 engine/app) and every engine/app
# archive are never named here and never carry omitted TUs; the flags are
# applied per-target-PRIVATE, only to the eleven named test exes, and only when
# if(TARGET) confirms the test exists. Disclosed cost: these test objects are
# compiled with speculative devirtualization DISABLED, so their codegen differs
# from the shipping build (which keeps it on); the tests exercise the same
# sources with the one speculated call left as an ordinary indirect dispatch.
#
# KEEP IN SYNC (three copies of the online/transport test-target names):
#   1. the add_executable(mdkr_online_* / mdkr_native_* / mdkr_match_* /
#      mdkr_lan_* ...) sites under MDKR_ENABLE_ONLINE_BETA / MDKR_NATIVE_PHONE_
#      PARTY + BUILD_TESTING earlier in this file (search
#      "add_executable(mdkr_online_track_table_test").
#   2. this dead-strip list.
#   3. ONLINE_TEST_EXES in tools/mingw_cross_check.sh (the MinGW --online lane).
# The if(TARGET)/elseif(FATAL_ERROR) pair hard-errors when the online-beta app
# configure (MDKR_ENABLE_ONLINE_BETA AND MDKR_APP) names a target that does not
# exist, so drift fails the configure instead of silently losing coverage; the
# BUILD_TESTING gate leaves tests-off configures untouched, and the beta-off
# lanes stay quiet (the beta-only target simply is not defined).
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND BUILD_TESTING)
    foreach(_mdkr_online_test
            mdkr_online_track_table_test
            mdkr_native_party_sas_test
            mdkr_lan_party_transport_test
            mdkr_match_peer_crypto_test
            mdkr_match_signal_client_test
            mdkr_match_peer_transport_test
            mdkr_native_party_e2e_driver
            mdkr_online_live_adapter_test
            mdkr_online_live_adapter_beta_test
            mdkr_match_live_transport_test
            mdkr_online_live_transport_e2e_driver)
        if(TARGET ${_mdkr_online_test})
            target_compile_options(${_mdkr_online_test} PRIVATE
                -ffunction-sections -fdata-sections
                -fno-devirtualize-speculatively)
            target_link_options(${_mdkr_online_test} PRIVATE
                -Wl,--gc-sections)
        elseif(MDKR_ENABLE_ONLINE_BETA AND MDKR_APP)
            message(FATAL_ERROR
                "GCC test dead-strip list names '${_mdkr_online_test}', but no "
                "such target exists in this online configure. The list has "
                "drifted from the test definitions -- update this foreach, the "
                "add_executable() site above, and ONLINE_TEST_EXES in "
                "tools/mingw_cross_check.sh together.")
        endif()
    endforeach()
endif()
