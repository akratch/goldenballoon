# ROM-free unit-test and CTest-Python-contract registrations, extracted from
# CMakeLists.txt so the build definition (Dependencies/Sources/target) is not
# buried under ~900 lines of test wiring. Included from CMakeLists.txt right
# after the Dependencies section (this file's SDL queue-mode block needs the
# SDL2_* variables that section defines) and before any other target is
# declared, which keeps ctest registration order identical to before the
# extraction.
#
# App-shell tests that need the mdkr64/mdkr64_web target itself (app_shell,
# app_shell_smoke, etc.) stay in CMakeLists.txt near that target -- they were
# already correctly positioned after it, so moving them here would gain
# nothing and risks disturbing ctest ordering.

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

    add_executable(mdkr_camera_obstruction_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_obstruction.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction.c)
    target_include_directories(mdkr_camera_obstruction_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_camera_obstruction_test PRIVATE m)
    endif()
    add_test(NAME camera_obstruction COMMAND mdkr_camera_obstruction_test)

    add_executable(mdkr_camera_obstruction_resolver_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_obstruction_resolver.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction_resolver.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction.c)
    target_include_directories(mdkr_camera_obstruction_resolver_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_camera_obstruction_resolver_test PRIVATE m)
    endif()
    add_test(NAME camera_obstruction_resolver COMMAND mdkr_camera_obstruction_resolver_test)

    add_executable(mdkr_camera_obstruction_transform_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_obstruction_transform.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction_transform.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction.c)
    target_include_directories(mdkr_camera_obstruction_transform_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_camera_obstruction_transform_test PRIVATE m)
    endif()
    add_test(NAME camera_obstruction_transform COMMAND mdkr_camera_obstruction_transform_test)

    add_executable(mdkr_camera_object_bvh_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_object_bvh.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction_transform.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction.c)
    target_include_directories(mdkr_camera_object_bvh_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/game/include/sys
        ${CMAKE_SOURCE_DIR}/game/libultra
        ${CMAKE_SOURCE_DIR}/game/libultra/src/audio
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_camera_object_bvh_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1
        F3DDKR_GBI
        _FINALROM)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        # Match the engine's translated-N64 header mode; textures_sprites.h has
        # intentional unsigned enum constants outside ISO C's signed int range.
        target_compile_options(mdkr_camera_object_bvh_test PRIVATE
            -fms-extensions
            -Wno-c23-extensions)
    endif()
    if(NOT MSVC)
        target_link_libraries(mdkr_camera_object_bvh_test PRIVATE m)
    endif()
    add_test(NAME camera_object_bvh COMMAND mdkr_camera_object_bvh_test)

    add_executable(mdkr_camera_dynamic_temporal_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_dynamic_temporal.c
        ${CMAKE_SOURCE_DIR}/tests/camera_math_link_support.c
        ${CMAKE_SOURCE_DIR}/game/src/hasm/math_util.c
        ${CMAKE_SOURCE_DIR}/game/src/camera_dynamic_temporal.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction_transform.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction.c)
    target_include_directories(mdkr_camera_dynamic_temporal_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/game/include/sys
        ${CMAKE_SOURCE_DIR}/game/libultra
        ${CMAKE_SOURCE_DIR}/game/libultra/src/audio
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_camera_dynamic_temporal_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1
        F3DDKR_GBI
        _FINALROM)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(mdkr_camera_dynamic_temporal_test PRIVATE
            -fms-extensions
            -Wno-c23-extensions)
    endif()
    if(NOT MSVC)
        target_link_libraries(mdkr_camera_dynamic_temporal_test PRIVATE m)
    endif()
    add_test(NAME camera_dynamic_temporal COMMAND mdkr_camera_dynamic_temporal_test)

    add_executable(mdkr_camera_dynamic_precedence_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_dynamic_precedence.c
        ${CMAKE_SOURCE_DIR}/game/src/camera_dynamic_publication.c
        ${CMAKE_SOURCE_DIR}/game/src/camera_dynamic_temporal.c
        ${CMAKE_SOURCE_DIR}/game/src/camera_object_occlusion.c
        ${CMAKE_SOURCE_DIR}/tests/camera_math_link_support.c
        ${CMAKE_SOURCE_DIR}/game/src/hasm/math_util.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction_transform.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction.c)
    target_include_directories(mdkr_camera_dynamic_precedence_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/game/include/sys
        ${CMAKE_SOURCE_DIR}/game/libultra
        ${CMAKE_SOURCE_DIR}/game/libultra/src/audio
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_camera_dynamic_precedence_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1
        F3DDKR_GBI
        _FINALROM)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(mdkr_camera_dynamic_precedence_test PRIVATE
            -fms-extensions
            -Wno-c23-extensions)
    endif()
    if(NOT MSVC)
        target_link_libraries(mdkr_camera_dynamic_precedence_test PRIVATE m)
    endif()
    add_test(NAME camera_dynamic_precedence
        COMMAND mdkr_camera_dynamic_precedence_test)

    add_executable(mdkr_camera_dynamic_publication_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_dynamic_publication.c
        ${CMAKE_SOURCE_DIR}/game/src/camera_dynamic_publication.c)
    target_include_directories(mdkr_camera_dynamic_publication_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game/src)
    target_compile_definitions(mdkr_camera_dynamic_publication_test PRIVATE
        NATIVE_PORT=1)
    add_test(NAME camera_dynamic_publication
        COMMAND mdkr_camera_dynamic_publication_test)

    add_executable(mdkr_camera_target_visibility_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_target_visibility.c
        ${CMAKE_SOURCE_DIR}/platform/camera_target_visibility.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction_query.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction.c)
    target_include_directories(mdkr_camera_target_visibility_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_camera_target_visibility_test PRIVATE m)
    endif()
    add_test(NAME camera_target_visibility
        COMMAND mdkr_camera_target_visibility_test)

    add_executable(mdkr_camera_obstruction_query_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_obstruction_query.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction_query.c
        ${CMAKE_SOURCE_DIR}/platform/camera_obstruction.c)
    target_include_directories(mdkr_camera_obstruction_query_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_camera_obstruction_query_test PRIVATE m)
    endif()
    add_test(NAME camera_obstruction_query COMMAND mdkr_camera_obstruction_query_test)

    add_executable(mdkr_camera_lens_pose_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_lens_pose.c
        ${CMAKE_SOURCE_DIR}/tests/camera_math_link_support.c
        ${CMAKE_SOURCE_DIR}/game/src/hasm/math_util.c
        ${CMAKE_SOURCE_DIR}/game/src/camera_lens_pose.c)
    target_include_directories(mdkr_camera_lens_pose_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/game/include/sys
        ${CMAKE_SOURCE_DIR}/game/libultra
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_camera_lens_pose_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C=1
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1
        F3DDKR_GBI
        _FINALROM)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(mdkr_camera_lens_pose_test PRIVATE
            -fms-extensions
            -Wno-c23-extensions)
    endif()
    if(NOT MSVC)
        target_link_libraries(mdkr_camera_lens_pose_test PRIVATE m)
    endif()
    add_test(NAME camera_lens_pose COMMAND mdkr_camera_lens_pose_test)


    add_executable(mdkr_endian_utils_test
        ${CMAKE_SOURCE_DIR}/tests/test_endian_utils.c)
    target_include_directories(mdkr_endian_utils_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_endian_utils_test PRIVATE NATIVE_PORT=1)
    if(NOT MSVC)
        target_link_libraries(mdkr_endian_utils_test PRIVATE m)
    endif()
    add_test(NAME endian_utils COMMAND mdkr_endian_utils_test)

    # Content packs. Both are deliberately ROM-free and window-free: a pack
    # manifest is text, and a texture digest is a hash over bytes the caller
    # supplies, so neither needs the game to boot.
    add_executable(mdkr_mod_manifest_test
        ${CMAKE_SOURCE_DIR}/tests/test_mod_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/mod_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c)
    target_include_directories(mdkr_mod_manifest_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_mod_manifest_test PRIVATE m)
    endif()
    add_test(NAME mod_manifest COMMAND mdkr_mod_manifest_test)

    # The digest is a published contract -- every pack in existence names its
    # files by it -- so this test pins an exact value and proves the hash does
    # not read struct padding. It needs the fast3d include dir for
    # gfx_texture_cache_key.h.
    add_executable(mdkr_mod_texture_key_test
        ${CMAKE_SOURCE_DIR}/tests/test_mod_texture_key.c
        ${CMAKE_SOURCE_DIR}/platform/mod_texture_key.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_mod_texture_key_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/platform/fast3d)
    if(NOT MSVC)
        target_link_libraries(mdkr_mod_texture_key_test PRIVATE m)
    endif()
    add_test(NAME mod_texture_key COMMAND mdkr_mod_texture_key_test)

    # The console command parser. Its load-bearing property is that `set`
    # resolves through mdkr_video_key_from_name and refuses anything the schema
    # does not name -- without that, an in-game console is an arbitrary-write
    # primitive. Tested with no window and no ROM.
    add_executable(mdkr_dev_command_test
        ${CMAKE_SOURCE_DIR}/tests/test_dev_command.c
        ${CMAKE_SOURCE_DIR}/platform/dev_command.c
        ${CMAKE_SOURCE_DIR}/platform/video_config.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c
        ${CMAKE_SOURCE_DIR}/platform/pacing_policy.c)
    target_include_directories(mdkr_dev_command_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_dev_command_test PRIVATE m)
    endif()
    add_test(NAME dev_command COMMAND mdkr_dev_command_test)

    # The enhancement table and its authority classes. Group 4 asserts this
    # table and mdkr_video_key_is_enhancement() describe the same set, so the
    # two independent lists cannot drift apart silently.
    add_executable(mdkr_enhancement_registry_test
        ${CMAKE_SOURCE_DIR}/tests/test_enhancement_registry.c
        ${CMAKE_SOURCE_DIR}/platform/enhancement_registry.c
        ${CMAKE_SOURCE_DIR}/platform/video_config.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c
        ${CMAKE_SOURCE_DIR}/platform/pacing_policy.c)
    target_include_directories(mdkr_enhancement_registry_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_enhancement_registry_test PRIVATE m)
    endif()
    add_test(NAME enhancement_registry COMMAND mdkr_enhancement_registry_test)

    # Pack discovery, load order and path resolution. fs_utf8.c is a real link
    # dependency, not decoration: path access goes through mdkr_fopen_utf8 and
    # mdkr_path_query_utf8 so the Windows arm inherits the existing UTF-8
    # boundary instead of growing a second conversion.
    add_executable(mdkr_mod_registry_test
        ${CMAKE_SOURCE_DIR}/tests/test_mod_registry.c
        ${CMAKE_SOURCE_DIR}/platform/mod_registry.c
        ${CMAKE_SOURCE_DIR}/platform/mod_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        # mod_source.c (and miniz under it) are here because the registry no
        # longer has a path validator or a file reader of its own: it discovers
        # and reads both pack kinds through that one interface. The link
        # dependency is the point -- it is what makes "there is only one
        # validator" a fact the linker enforces rather than a comment.
        ${CMAKE_SOURCE_DIR}/platform/mod_source.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
    target_include_directories(mdkr_mod_registry_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/lib/miniz)
    if(NOT MSVC)
        target_link_libraries(mdkr_mod_registry_test PRIVATE m)
    endif()
    add_test(NAME mod_registry COMMAND mdkr_mod_registry_test
             ${CMAKE_CURRENT_BINARY_DIR}/mod_registry_scratch)

    # When a pack PNG is refused, relative to when it is decoded. A pack is a
    # file a player downloaded from a stranger, so the cache cap is only a
    # defence if it is consulted before the decoder is handed the bytes --
    # applied afterwards it bounds what is retained, never what is allocated,
    # and the allocation is the damage.
    #
    # tests/mod_texture_store_probe.c takes lib/stb/stb_image_impl.c's place
    # here, and that substitution is the test: it is the same stb_image,
    # configured identically, with the decode entry point and the decoder's
    # allocator each behind a counter. Nothing the store RETURNS distinguishes
    # "refused before decoding" from "decoded, then refused", so without that
    # seam the case could only assert the weaker claim, which held before the
    # fix as well.
    add_executable(mdkr_mod_texture_store_test
        ${CMAKE_SOURCE_DIR}/tests/test_mod_texture_store.c
        ${CMAKE_SOURCE_DIR}/tests/mod_texture_store_probe.c
        ${CMAKE_SOURCE_DIR}/platform/mod_texture_store.c
        ${CMAKE_SOURCE_DIR}/platform/mod_registry.c
        ${CMAKE_SOURCE_DIR}/platform/mod_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/platform/mod_source.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
    target_include_directories(mdkr_mod_texture_store_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/lib/stb
        ${CMAKE_SOURCE_DIR}/lib/miniz)
    # The probe instantiates stb_image, which is third-party and does not
    # survive this project's warning baseline -- the same one-file exemption
    # lib/stb/stb_image_impl.c and lib/miniz/miniz.c already have.
    if(MSVC)
        set_source_files_properties(
            ${CMAKE_SOURCE_DIR}/tests/mod_texture_store_probe.c
            PROPERTIES COMPILE_OPTIONS "/w")
    else()
        set_source_files_properties(
            ${CMAKE_SOURCE_DIR}/tests/mod_texture_store_probe.c
            PROPERTIES COMPILE_OPTIONS "-w")
        target_link_libraries(mdkr_mod_texture_store_test PRIVATE m)
    endif()
    add_test(NAME mod_texture_store COMMAND mdkr_mod_texture_store_test
             ${CMAKE_CURRENT_BINARY_DIR}/mod_texture_store_scratch)

    # The accessibility semantic model. ImGui exposes no accessibility tree, so
    # this module is the tree: it emits text, which is why the whole behaviour
    # is gateable in CI with no audio device and no window.
    add_executable(mdkr_a11y_model_test
        ${CMAKE_SOURCE_DIR}/tests/test_a11y_model.c
        ${CMAKE_SOURCE_DIR}/platform/a11y_model.c)
    target_include_directories(mdkr_a11y_model_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_a11y_model_test PRIVATE m)
    endif()
    add_test(NAME a11y_model COMMAND mdkr_a11y_model_test)

    # The drain worker's barge-in stamp. A different KIND of test from the one
    # above: the model is single-threaded and decidable on its text, this is two
    # real threads and the assertion is about WHICH utterance reaches a backend.
    #
    # MDKR_A11Y_SPEECH_TESTING is defined HERE AND NOWHERE ELSE. It compiles one
    # hook call into a11y_speech_worker.c at the instruction the barge-in race
    # needs, so the test can park the worker there instead of racing at it;
    # without the define the hook site preprocesses away entirely and no shipped
    # build contains a pointer, a branch or a call for it.
    #
    # Links no speech backend. The test file supplies the whole of the
    # a11y_speech.h interface itself, which is also what makes it impossible to
    # link a real one in by accident -- seven duplicate symbols. SDL is here for
    # its threads and semaphores and is never initialised, so this opens no
    # device and needs no driver, exactly like the audio ring tests below.
    add_executable(mdkr_a11y_speech_worker_test
        ${CMAKE_SOURCE_DIR}/tests/test_a11y_speech_worker.c
        ${CMAKE_SOURCE_DIR}/platform/a11y_speech_worker.c)
    target_include_directories(mdkr_a11y_speech_worker_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${SDL2_INCLUDE_DIRS})
    target_link_directories(mdkr_a11y_speech_worker_test PRIVATE
        ${SDL2_LIBRARY_DIRS})
    target_compile_definitions(mdkr_a11y_speech_worker_test PRIVATE
        SDL_MAIN_HANDLED MDKR_A11Y_SPEECH_TESTING)
    target_link_libraries(mdkr_a11y_speech_worker_test PRIVATE ${SDL2_LIBRARIES})
    if(MSVC)
        target_compile_options(mdkr_a11y_speech_worker_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_a11y_speech_worker_test PRIVATE
            -Wall -Wextra -Werror)
    endif()
    add_test(NAME a11y_speech_worker COMMAND mdkr_a11y_speech_worker_test)
    # Every wait in it is bounded and short; this is the backstop for a genuine
    # deadlock, which must fail the suite rather than hang it.
    set_tests_properties(a11y_speech_worker PROPERTIES TIMEOUT 120)

    # The save-state container. Deliberately links NOTHING from save_container.c
    # or save_codec.c: a save state is not the progress save, and the two must
    # not be able to become each other. The truncation sweep writes and re-reads
    # a real file at every offset, so it needs no ROM and no window.
    add_executable(mdkr_save_state_container_test
        ${CMAKE_SOURCE_DIR}/tests/test_save_state_container.c
        ${CMAKE_SOURCE_DIR}/platform/save_state.c)
    target_include_directories(mdkr_save_state_container_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_save_state_container_test PRIVATE m)
    endif()
    add_test(NAME save_state_container COMMAND mdkr_save_state_container_test)

    # Version comparison and the update-check interval. The clock is a
    # parameter, so this needs no real time and no network -- which is the
    # whole reason the policy is a separate translation unit from the fetch.
    add_executable(mdkr_update_check_test
        ${CMAKE_SOURCE_DIR}/tests/test_update_check.c
        ${CMAKE_SOURCE_DIR}/platform/update_check.c)
    target_include_directories(mdkr_update_check_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_update_check_test PRIVATE m)
    endif()
    add_test(NAME update_check COMMAND mdkr_update_check_test)

    # The [GPUINFO] record and the adapter choice over it. Both are pure and
    # need no GPU, which is the point: the selection policy is exhaustively
    # testable on a one-GPU machine and in hosted CI with none.
    add_executable(mdkr_gpu_diagnostics_test
        ${CMAKE_SOURCE_DIR}/tests/test_gpu_diagnostics.c
        ${CMAKE_SOURCE_DIR}/platform/gpu_diagnostics.c)
    target_include_directories(mdkr_gpu_diagnostics_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_gpu_diagnostics_test PRIVATE m)
    endif()
    add_test(NAME gpu_diagnostics COMMAND mdkr_gpu_diagnostics_test)

    add_executable(mdkr_adapter_policy_test
        ${CMAKE_SOURCE_DIR}/tests/test_adapter_policy.c
        ${CMAKE_SOURCE_DIR}/platform/adapter_policy.c
        ${CMAKE_SOURCE_DIR}/platform/gpu_diagnostics.c)
    target_include_directories(mdkr_adapter_policy_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_adapter_policy_test PRIVATE m)
    endif()
    add_test(NAME adapter_policy COMMAND mdkr_adapter_policy_test)

    # One read interface over a directory pack and a zip pack. The property
    # worth the test is that both go through ONE path validator: mutating the
    # zip arm alone fails only the directory assertions, which is exactly the
    # "fixed one caller, missed the other" shape a single funnel prevents.
    # miniz is third-party and does not survive the project's warning baseline
    # under GCC/mingw, so that one file is compiled with warnings off.
    add_executable(mdkr_mod_source_zip_test
        ${CMAKE_SOURCE_DIR}/tests/test_mod_source_zip.c
        ${CMAKE_SOURCE_DIR}/platform/mod_source.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
    target_include_directories(mdkr_mod_source_zip_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/lib/miniz)
    if(MSVC)
        set_source_files_properties(${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c
            PROPERTIES COMPILE_OPTIONS "/w")
    else()
        set_source_files_properties(${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c
            PROPERTIES COMPILE_OPTIONS "-w")
        target_link_libraries(mdkr_mod_source_zip_test PRIVATE m)
    endif()
    add_test(NAME mod_source_zip COMMAND mdkr_mod_source_zip_test
             ${CMAKE_CURRENT_BINARY_DIR}/mod_source_scratch)

    # Bounds-checked access to an entry INSIDE an asset section. Each aborting
    # case runs in a forked child and asserts WTERMSIG == SIGABRT plus the
    # message text, so a segfault fails rather than passing as "it died".
    add_executable(mdkr_asset_subentry_test
        ${CMAKE_SOURCE_DIR}/tests/test_asset_subentry.c
        ${CMAKE_SOURCE_DIR}/platform/asset_subentry.c)
    target_include_directories(mdkr_asset_subentry_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/game/include)
    target_compile_definitions(mdkr_asset_subentry_test PRIVATE NATIVE_PORT=1)
    if(NOT MSVC)
        target_link_libraries(mdkr_asset_subentry_test PRIVATE m)
    endif()
    add_test(NAME asset_subentry COMMAND mdkr_asset_subentry_test)

    add_executable(mdkr_vehicle_audio_contract_test
        ${CMAKE_SOURCE_DIR}/tests/test_vehicle_audio_contract.c
        ${CMAKE_SOURCE_DIR}/platform/asset_swap.c)
    target_include_directories(mdkr_vehicle_audio_contract_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_vehicle_audio_contract_test PRIVATE
        VERSION_us_v80)
    add_test(NAME vehicle_audio_contract COMMAND mdkr_vehicle_audio_contract_test)

    add_executable(mdkr_magic_codes_test
        ${CMAKE_SOURCE_DIR}/tests/test_magic_codes.c
        ${CMAKE_SOURCE_DIR}/platform/asset_swap.c)
    target_include_directories(mdkr_magic_codes_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_magic_codes_test PRIVATE VERSION_us_v80)
    add_test(NAME magic_codes COMMAND mdkr_magic_codes_test)

    add_executable(mdkr_taj_mod_test
        ${CMAKE_SOURCE_DIR}/tests/test_taj_mod.c
        ${CMAKE_SOURCE_DIR}/game/src/taj_mod.c
        ${CMAKE_SOURCE_DIR}/platform/taj_mod_state.c)
    target_include_directories(mdkr_taj_mod_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_taj_mod_test PRIVATE TAJ_MOD_TESTING=1)
    add_test(NAME taj_mod COMMAND mdkr_taj_mod_test)

    add_executable(mdkr_taj_select_layout_test
        ${CMAKE_SOURCE_DIR}/tests/test_taj_select_layout.c
        ${CMAKE_SOURCE_DIR}/game/src/taj_select_layout.c)
    target_include_directories(mdkr_taj_select_layout_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include)
    target_compile_definitions(mdkr_taj_select_layout_test PRIVATE
        NATIVE_PORT=1 VERSION_us_v80 _LANGUAGE_C MODERN_CC)
    add_test(NAME taj_select_layout COMMAND mdkr_taj_select_layout_test)

    add_executable(mdkr_taj_mod_state_file_test
        ${CMAKE_SOURCE_DIR}/tests/test_taj_mod_state_file.c
        ${CMAKE_SOURCE_DIR}/platform/taj_mod_state.c
        ${CMAKE_SOURCE_DIR}/platform/taj_mod_state_file.c
        ${CMAKE_SOURCE_DIR}/platform/user_paths.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c)
    target_include_directories(mdkr_taj_mod_state_file_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/tests)
    add_test(NAME taj_mod_state_file COMMAND mdkr_taj_mod_state_file_test)

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

    add_executable(mdkr_taj_physics_test
        ${CMAKE_SOURCE_DIR}/tests/test_taj_physics.c
        ${CMAKE_SOURCE_DIR}/game/src/taj_physics.c)
    target_include_directories(mdkr_taj_physics_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_taj_physics_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1
        TAJ_PHYSICS_TESTING=1)
    add_test(NAME taj_physics COMMAND mdkr_taj_physics_test)

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
        ${CMAKE_SOURCE_DIR}/platform/audio_volume.c
        ${CMAKE_SOURCE_DIR}/platform/user_paths.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c
        # publish() asks the cascade planner for the 1P shadow budget instead of
        # republishing a hardcoded resolution nothing read. gfx_shadow_cascade.c
        # is pure (no globals, no GPU), so the ROM-free seam survives.
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_shadow_cascade.c)
    target_include_directories(mdkr_video_config_runtime_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_video_config_runtime_test PRIVATE
        MDKR_VIDEO_RUNTIME_TESTING=1)
    if(NOT MSVC)
        target_link_libraries(mdkr_video_config_runtime_test PRIVATE m)
    endif()
    add_test(NAME video_config_runtime COMMAND mdkr_video_config_runtime_test)
    add_test(NAME video_config_deferred_apply_runtime
        COMMAND mdkr_video_config_runtime_test --deferred-apply-case)
    add_test(NAME video_config_pure_comfort_runtime
        COMMAND mdkr_video_config_runtime_test --pure-comfort-case)
    add_test(NAME video_config_corrupt_handoff_runtime
        COMMAND mdkr_video_config_runtime_test --corrupt-handoff-case)
    add_test(NAME video_config_embedded_nul_runtime
        COMMAND mdkr_video_config_runtime_test --embedded-nul-case)
    add_test(NAME video_config_durability_runtime
        COMMAND mdkr_video_config_runtime_test --durability-case)
    add_test(NAME video_config_launcher_merge_runtime
        COMMAND mdkr_video_config_runtime_test --launcher-merge-case)
    add_test(NAME video_config_launcher_session_runtime
        COMMAND mdkr_video_config_runtime_test --launcher-session-case)

    add_executable(mdkr_controller_mapping_test
        ${CMAKE_SOURCE_DIR}/tests/test_controller_mapping.c
        ${CMAKE_SOURCE_DIR}/platform/controller_mapping.c
        ${CMAKE_SOURCE_DIR}/platform/video_config.c
        ${CMAKE_SOURCE_DIR}/platform/pacing_policy.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c)
    target_include_directories(mdkr_controller_mapping_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_controller_mapping_test PRIVATE m)
    endif()
    add_test(NAME controller_mapping COMMAND mdkr_controller_mapping_test)

    add_executable(mdkr_user_paths_test
        ${CMAKE_SOURCE_DIR}/tests/test_user_paths.c
        ${CMAKE_SOURCE_DIR}/platform/user_paths.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c)
    target_include_directories(mdkr_user_paths_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME user_paths COMMAND mdkr_user_paths_test)
    add_test(NAME user_paths_pref_failure
        COMMAND mdkr_user_paths_test --pref-fail)

    add_executable(mdkr_fs_utf8_test
        ${CMAKE_SOURCE_DIR}/tests/test_fs_utf8.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c)
    target_include_directories(mdkr_fs_utf8_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME fs_utf8 COMMAND mdkr_fs_utf8_test)

    add_executable(mdkr_app_overlay_hooks_test
        ${CMAKE_SOURCE_DIR}/tests/test_app_overlay_hooks.c
        ${CMAKE_SOURCE_DIR}/platform/app_overlay_hooks.c)
    target_include_directories(mdkr_app_overlay_hooks_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME app_overlay_hooks COMMAND mdkr_app_overlay_hooks_test)

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

    add_executable(mdkr_texture_cache_key_test
        ${CMAKE_SOURCE_DIR}/tests/test_texture_cache_key.c)
    target_include_directories(mdkr_texture_cache_key_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME texture_cache_key COMMAND mdkr_texture_cache_key_test)

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

    add_executable(mdkr_viewport_route_cache_test
        ${CMAKE_SOURCE_DIR}/tests/test_viewport_route_cache.c
        ${CMAKE_SOURCE_DIR}/platform/viewport_route_cache.c)
    target_include_directories(mdkr_viewport_route_cache_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME viewport_route_cache COMMAND mdkr_viewport_route_cache_test)

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

    # The ring links SDL only for its atomics/barriers; it opens no device, so
    # this stays a plain ROM-free unit test with no driver requirement.
    add_executable(mdkr_audio_ring_test
        ${CMAKE_SOURCE_DIR}/tests/test_audio_ring.c
        ${CMAKE_SOURCE_DIR}/platform/audio_ring.c)
    target_include_directories(mdkr_audio_ring_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${SDL2_INCLUDE_DIRS})
    target_link_directories(mdkr_audio_ring_test PRIVATE
        ${SDL2_LIBRARY_DIRS})
    target_compile_definitions(mdkr_audio_ring_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(mdkr_audio_ring_test PRIVATE ${SDL2_LIBRARIES})
    add_test(NAME audio_ring COMMAND mdkr_audio_ring_test)

    # The concurrent half of the same contract. Kept a separate target because
    # it is a different KIND of test: the sequential one is deterministic and
    # asserts exact values, this one runs two real threads and can only assert
    # invariants. Uses SDL's own threads (the ring already links SDL for its
    # atomics), so the producer/consumer pair is the same primitive the port
    # actually runs on. Still opens no device.
    add_executable(mdkr_audio_ring_threaded_test
        ${CMAKE_SOURCE_DIR}/tests/test_audio_ring_threaded.c
        ${CMAKE_SOURCE_DIR}/platform/audio_ring.c)
    target_include_directories(mdkr_audio_ring_threaded_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${SDL2_INCLUDE_DIRS})
    target_link_directories(mdkr_audio_ring_threaded_test PRIVATE
        ${SDL2_LIBRARY_DIRS})
    target_compile_definitions(mdkr_audio_ring_threaded_test PRIVATE
        SDL_MAIN_HANDLED)
    target_link_libraries(mdkr_audio_ring_threaded_test PRIVATE
        ${SDL2_LIBRARIES})
    if(MSVC)
        target_compile_options(mdkr_audio_ring_threaded_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_audio_ring_threaded_test PRIVATE
            -Wall -Wextra -Werror)
    endif()
    add_test(NAME audio_ring_threaded COMMAND mdkr_audio_ring_threaded_test)
    # Two threads streaming ~1.6M frames plus a flood arm; generous under a
    # sanitizer, where every atomic is instrumented.
    set_tests_properties(audio_ring_threaded PROPERTIES TIMEOUT 300)

    # Closed-loop delivery resilience: real controller + real ring driven
    # against a modelled host device in virtual time. Asserts both directions
    # (the shipped design starves, the current one does not), so the arm proves
    # its own non-vacuity without needing a display or an output device.
    add_executable(mdkr_audio_resilience_test
        ${CMAKE_SOURCE_DIR}/tests/test_audio_resilience.c
        ${CMAKE_SOURCE_DIR}/platform/audio_ring.c
        ${CMAKE_SOURCE_DIR}/platform/audio_queue_controller.c)
    target_include_directories(mdkr_audio_resilience_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${SDL2_INCLUDE_DIRS})
    target_link_directories(mdkr_audio_resilience_test PRIVATE
        ${SDL2_LIBRARY_DIRS})
    target_compile_definitions(mdkr_audio_resilience_test PRIVATE
        SDL_MAIN_HANDLED)
    target_link_libraries(mdkr_audio_resilience_test PRIVATE ${SDL2_LIBRARIES})
    add_test(NAME audio_resilience COMMAND mdkr_audio_resilience_test)

    add_executable(mdkr_audio_sink_evidence_test
        ${CMAKE_SOURCE_DIR}/tests/test_audio_sink_evidence.c
        ${CMAKE_SOURCE_DIR}/platform/audio_sink_evidence.c)
    target_include_directories(mdkr_audio_sink_evidence_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME audio_sink_evidence COMMAND mdkr_audio_sink_evidence_test)

    add_executable(mdkr_audio_volume_test
        ${CMAKE_SOURCE_DIR}/tests/test_audio_volume.c
        ${CMAKE_SOURCE_DIR}/platform/audio_volume.c)
    target_include_directories(mdkr_audio_volume_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME audio_volume COMMAND mdkr_audio_volume_test)

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

    add_executable(mdkr_gfx_retained_task_test
        ${CMAKE_SOURCE_DIR}/tests/test_gfx_retained_task.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_retained_task.c)
    target_include_directories(mdkr_gfx_retained_task_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME gfx_retained_task COMMAND mdkr_gfx_retained_task_test)

    add_executable(mdkr_gfx_retained_task_budget_test
        ${CMAKE_SOURCE_DIR}/tests/test_gfx_retained_task_budget.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_retained_task.c)
    target_include_directories(mdkr_gfx_retained_task_budget_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME gfx_retained_task_budget
        COMMAND mdkr_gfx_retained_task_budget_test)
    set_tests_properties(gfx_retained_task_budget PROPERTIES
        ENVIRONMENT "MDKR_RETAINED_ARENA_COPY_BUDGET_BYTES=1024")

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

    add_executable(mdkr_font_outline_test
        ${CMAKE_SOURCE_DIR}/tests/test_font_outline.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_font_outline.c)
    target_include_directories(mdkr_font_outline_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/lib/stb)
    if(NOT MSVC)
        target_link_libraries(mdkr_font_outline_test PRIVATE m)
    endif()
    add_test(NAME font_outline COMMAND mdkr_font_outline_test)

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

    add_executable(mdkr_webgpu_callback_latch_test
        ${CMAKE_SOURCE_DIR}/tests/test_webgpu_callback_latch.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_callback_latch.c)
    target_include_directories(mdkr_webgpu_callback_latch_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d)
    find_package(Threads REQUIRED)
    target_link_libraries(mdkr_webgpu_callback_latch_test PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(mdkr_webgpu_callback_latch_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_webgpu_callback_latch_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME webgpu_callback_latch COMMAND mdkr_webgpu_callback_latch_test)

    add_executable(mdkr_webgpu_async_request_test
        ${CMAKE_SOURCE_DIR}/tests/test_webgpu_async_request.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_async_request.c)
    target_include_directories(mdkr_webgpu_async_request_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d)
    target_link_libraries(mdkr_webgpu_async_request_test PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(mdkr_webgpu_async_request_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_webgpu_async_request_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME webgpu_async_request COMMAND mdkr_webgpu_async_request_test)

    add_executable(mdkr_webgpu_surface_policy_test
        ${CMAKE_SOURCE_DIR}/tests/test_webgpu_surface_policy.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_webgpu_surface_policy.c
        ${CMAKE_SOURCE_DIR}/platform/pacing_policy.c)
    target_include_directories(mdkr_webgpu_surface_policy_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/fast3d
        ${CMAKE_SOURCE_DIR}/platform)
    if(MSVC)
        target_compile_options(mdkr_webgpu_surface_policy_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_webgpu_surface_policy_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME webgpu_surface_policy COMMAND mdkr_webgpu_surface_policy_test)
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
    add_test(
        NAME object_material_ownership
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_object_material_ownership.py)
    add_test(
        NAME webgpu_cached_bind_ownership
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_webgpu_cached_bind_ownership.py)
    add_test(
        NAME webgpu_pipeline_callback_ownership
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_webgpu_pipeline_callback_ownership.py)
    add_test(
        NAME scheduler_exit_completion_order
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_scheduler_exit_completion_order.py)
    add_test(
        NAME completed_tick_conservation
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_completed_tick_conservation.py)
    add_test(
        NAME harness_utils
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_harness_utils.py)
    add_test(
        NAME texture_cache_identity
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_texture_cache_identity.py)
    add_test(
        NAME camera_obstruction_authority
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_camera_obstruction_authority.py)
    add_test(
        NAME camera_track_occlusion_cache
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_camera_track_occlusion_cache.py)
    add_test(
        NAME camera_object_occlusion_cache
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_camera_object_occlusion_cache.py)
    add_test(
        NAME camera_dynamic_occlusion
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_camera_dynamic_occlusion.py)
    add_test(
        NAME camera_obstruction_observe
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_camera_obstruction_observe.py)
    add_test(
        NAME app_config_durability_ui_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_app_config_durability_ui.py)
    add_test(
        NAME async_rom_validation_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_async_rom_validation.py)
    add_test(
        NAME host_input_focus_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_host_input_focus.py)
    add_test(
        NAME overlay_input_handoff_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_overlay_input_handoff.py)
    add_test(
        NAME web_document_structure
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_web_document_structure.py)
    add_test(
        NAME product_claim_boundaries
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_product_claim_boundaries.py)
    add_test(
        NAME audio_sink_evidence_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_audio_sink_evidence_contract.py)
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
