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
    add_executable(mdkr_custom_track_texture_test
        ${CMAKE_SOURCE_DIR}/tests/test_custom_track_texture.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_texture.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_decode.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c
        ${CMAKE_SOURCE_DIR}/game/src/runtime_contracts.c)
    target_include_directories(mdkr_custom_track_texture_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform ${CMAKE_SOURCE_DIR}/game/src ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/lib/miniz)
    target_compile_definitions(mdkr_custom_track_texture_test PRIVATE
        VERSION_us_v80 _LANGUAGE_C MODERN_CC NON_MATCHING=1 AVOID_UB=1 NATIVE_PORT=1)
    if(NOT MSVC)
        target_link_libraries(mdkr_custom_track_texture_test PRIVATE m)
    endif()
    add_test(NAME custom_track_texture COMMAND mdkr_custom_track_texture_test)
    add_executable(mdkr_custom_track_model_test
        ${CMAKE_SOURCE_DIR}/tests/test_custom_track_model.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_model.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_model_payload.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_decode.c
        ${CMAKE_SOURCE_DIR}/game/src/hasm_native/inflate_native.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
    target_include_directories(mdkr_custom_track_model_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform ${CMAKE_SOURCE_DIR}/lib/miniz
        ${CMAKE_SOURCE_DIR}/game/src ${CMAKE_SOURCE_DIR}/game/include)
    target_compile_definitions(mdkr_custom_track_model_test PRIVATE
        _LANGUAGE_C NATIVE_PORT=1 MODERN_CC)
    add_test(NAME custom_track_model COMMAND mdkr_custom_track_model_test)
    add_executable(mdkr_custom_track_header_test
        ${CMAKE_SOURCE_DIR}/tests/test_custom_track_header.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_header.c)
    target_include_directories(mdkr_custom_track_header_test PRIVATE ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME custom_track_header COMMAND mdkr_custom_track_header_test)
    add_executable(mdkr_custom_track_package_test
        ${CMAKE_SOURCE_DIR}/tests/test_custom_track_package.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_package_admission.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_prepared.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_section.c
        ${CMAKE_SOURCE_DIR}/game/src/object_layout.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_header.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_model.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_model_payload.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_texture.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_decode.c
        ${CMAKE_SOURCE_DIR}/game/src/runtime_contracts.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_package.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/mod_source.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
    target_include_directories(mdkr_custom_track_package_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform ${CMAKE_SOURCE_DIR}/lib/miniz
        ${CMAKE_SOURCE_DIR}/game/src ${CMAKE_SOURCE_DIR}/game/include)
    target_compile_definitions(mdkr_custom_track_package_test PRIVATE
        MDKR_TRACK_PACKAGE_TEST_ALLOCATOR=1 MDKR_CUSTOM_TRACK_TEST_ALLOCATOR=1
        VERSION_us_v80 _LANGUAGE_C MODERN_CC NON_MATCHING=1 AVOID_UB=1 NATIVE_PORT=1)
    target_include_directories(mdkr_custom_track_package_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/game/libultra/src/audio)
    if(NOT MSVC)
        target_link_libraries(mdkr_custom_track_package_test PRIVATE m)
    endif()
    add_test(NAME custom_track_package COMMAND mdkr_custom_track_package_test
        ${CMAKE_CURRENT_BINARY_DIR}/custom_track_package_scratch)
    add_executable(mdkr_custom_track_manifest_test
        ${CMAKE_SOURCE_DIR}/tests/test_custom_track_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/mod_source.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
    target_include_directories(mdkr_custom_track_manifest_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform ${CMAKE_SOURCE_DIR}/lib/miniz)
    if(NOT MSVC)
        target_link_libraries(mdkr_custom_track_manifest_test PRIVATE m)
    endif()
    add_test(NAME custom_track_manifest COMMAND mdkr_custom_track_manifest_test)
    add_executable(mdkr_custom_track_decode_test
        ${CMAKE_SOURCE_DIR}/tests/test_custom_track_decode.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_decode.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
    target_include_directories(mdkr_custom_track_decode_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform ${CMAKE_SOURCE_DIR}/lib/miniz)
    add_test(NAME custom_track_decode COMMAND mdkr_custom_track_decode_test)
    # The real PI DMA entry (platform/pi_dma.c) with a recording ROM read:
    # a DMA must land at the caller's pointer, never a rebuilt arena token.
    add_executable(mdkr_dma_destination_test
        ${CMAKE_SOURCE_DIR}/tests/test_dma_destination.c
        ${CMAKE_SOURCE_DIR}/platform/pi_dma.c)
    target_include_directories(mdkr_dma_destination_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_dma_destination_test PRIVATE
        VERSION_us_v80 _LANGUAGE_C MODERN_CC NON_MATCHING=1 AVOID_UB=1 NATIVE_PORT=1)
    add_test(NAME dma_destination COMMAND mdkr_dma_destination_test)
    add_executable(mdkr_custom_track_table_test ${CMAKE_SOURCE_DIR}/tests/test_custom_track_table.c
        ${CMAKE_SOURCE_DIR}/platform/custom_track_section.c)
    target_include_directories(mdkr_custom_track_table_test PRIVATE ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_custom_track_table_test PRIVATE MDKR_CUSTOM_TRACK_TEST_ALLOCATOR=1)
    add_test(NAME custom_track_table COMMAND mdkr_custom_track_table_test)
    add_executable(mdkr_rom_browser_test
        ${CMAKE_SOURCE_DIR}/tests/test_rom_browser.cpp
        ${CMAKE_SOURCE_DIR}/platform/app/rom_browser.cpp)
    target_include_directories(mdkr_rom_browser_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/app)
    target_compile_features(mdkr_rom_browser_test PRIVATE cxx_std_17)
    add_test(NAME rom_browser COMMAND mdkr_rom_browser_test)

    add_executable(mdkr_file_browser_ui_test
        ${CMAKE_SOURCE_DIR}/tests/test_file_browser_ui.cpp
        ${CMAKE_SOURCE_DIR}/platform/app/file_browser_ui.cpp
        ${CMAKE_SOURCE_DIR}/platform/app/rom_browser.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui_draw.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui_tables.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui_widgets.cpp)
    target_include_directories(mdkr_file_browser_ui_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/app ${CMAKE_SOURCE_DIR}/lib/imgui)
    target_compile_features(mdkr_file_browser_ui_test PRIVATE cxx_std_17)
    add_test(NAME file_browser_ui COMMAND mdkr_file_browser_ui_test)

    add_executable(mdkr_pack_installer_test
        ${CMAKE_SOURCE_DIR}/tests/test_pack_installer.cpp
        ${CMAKE_SOURCE_DIR}/platform/app/pack_installer.cpp
        ${CMAKE_SOURCE_DIR}/platform/mod_registry.c
        ${CMAKE_SOURCE_DIR}/platform/mod_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/platform/mod_source.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
    target_include_directories(mdkr_pack_installer_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/app
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/lib/miniz)
    target_compile_features(mdkr_pack_installer_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_pack_installer_test PRIVATE Threads::Threads)
    if(NOT MSVC)
        target_link_libraries(mdkr_pack_installer_test PRIVATE m)
    endif()
    add_test(NAME pack_installer COMMAND mdkr_pack_installer_test)
    # The app shell that ships this code is built as C++17 (see MDKR_APP in
    # CMakeLists.txt), but this file is included before that pin, and
    # cxx_std_17 is only a minimum: GCC 16 would compile these tests as C++20,
    # where path::u8string() returns std::u8string. Build them as shipped.
    set_target_properties(mdkr_pack_installer_test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    # The launcher's install job: a copy finishing in the same frame as Cancel
    # or Quit is never published, and discarded copies are deleted off the
    # launcher thread.
    add_executable(mdkr_pack_install_job_test
        ${CMAKE_SOURCE_DIR}/tests/test_pack_install_job.cpp
        ${CMAKE_SOURCE_DIR}/platform/app/pack_install_job.cpp
        ${CMAKE_SOURCE_DIR}/platform/app/pack_installer.cpp
        ${CMAKE_SOURCE_DIR}/platform/mod_registry.c
        ${CMAKE_SOURCE_DIR}/platform/mod_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/platform/mod_source.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
    target_include_directories(mdkr_pack_install_job_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/app
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/lib/miniz)
    target_compile_features(mdkr_pack_install_job_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_pack_install_job_test PRIVATE Threads::Threads)
    if(NOT MSVC)
        target_link_libraries(mdkr_pack_install_job_test PRIVATE m)
    endif()
    set_target_properties(mdkr_pack_install_job_test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME pack_install_job COMMAND mdkr_pack_install_job_test)
    # Windows installs by pathname. The same sources built with
    # MDKR_PACK_PATH_ARM run that installer here, so its logic is proved on
    # every host the suite runs on, not only on Windows.
    if(NOT WIN32)
        add_executable(mdkr_pack_installer_path_test
            ${CMAKE_SOURCE_DIR}/tests/test_pack_installer.cpp
            ${CMAKE_SOURCE_DIR}/platform/app/pack_installer.cpp
            ${CMAKE_SOURCE_DIR}/platform/mod_registry.c
            ${CMAKE_SOURCE_DIR}/platform/mod_manifest.c
            ${CMAKE_SOURCE_DIR}/platform/config_ini.c
            ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
            ${CMAKE_SOURCE_DIR}/platform/mod_source.c
            ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
        target_include_directories(mdkr_pack_installer_path_test PRIVATE
            ${CMAKE_SOURCE_DIR}/platform/app
            ${CMAKE_SOURCE_DIR}/platform
            ${CMAKE_SOURCE_DIR}/lib/miniz)
        target_compile_definitions(mdkr_pack_installer_path_test PRIVATE
            MDKR_PACK_PATH_ARM=1)
        target_compile_features(mdkr_pack_installer_path_test PRIVATE cxx_std_17)
        target_link_libraries(mdkr_pack_installer_path_test PRIVATE Threads::Threads m)
        set_target_properties(mdkr_pack_installer_path_test PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        add_test(NAME pack_installer_path COMMAND mdkr_pack_installer_path_test)
    endif()

    add_executable(mdkr_audio_bank_parser_test
        ${CMAKE_SOURCE_DIR}/tests/test_audio_bank_parser.c)
    target_include_directories(mdkr_audio_bank_parser_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/libultra/src/audio)
    mdkr_add_legacy_game_quote_includes(mdkr_audio_bank_parser_test)
    target_compile_definitions(mdkr_audio_bank_parser_test PRIVATE
        _LANGUAGE_C NATIVE_PORT=1 MODERN_CC)
    if(NOT MSVC)
        target_link_libraries(mdkr_audio_bank_parser_test PRIVATE m)
    endif()
    add_test(NAME audio_bank_parser COMMAND mdkr_audio_bank_parser_test)

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

    add_executable(mdkr_camera_motion_shoulder_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_motion_shoulder.c)
    target_include_directories(mdkr_camera_motion_shoulder_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_camera_motion_shoulder_test PRIVATE m)
    endif()
    add_test(NAME camera_motion_shoulder COMMAND mdkr_camera_motion_shoulder_test)

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
        ${CMAKE_SOURCE_DIR}/platform/presentation_snapshot.c
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
    add_executable(mdkr_camera_lens_pose_native_test
        ${CMAKE_SOURCE_DIR}/tests/test_camera_lens_pose.c
        ${CMAKE_SOURCE_DIR}/platform/presentation_snapshot.c
        ${CMAKE_SOURCE_DIR}/tests/camera_math_link_support.c
        ${CMAKE_SOURCE_DIR}/game/src/hasm/math_util.c
        ${CMAKE_SOURCE_DIR}/game/src/camera_lens_pose.c
        ${CMAKE_SOURCE_DIR}/platform/math_util_native.c)
    foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
        get_target_property(camera_test_property mdkr_camera_lens_pose_test ${property})
        if(camera_test_property)
            set_property(TARGET mdkr_camera_lens_pose_native_test PROPERTY ${property} "${camera_test_property}")
        endif()
    endforeach()
    target_compile_definitions(mdkr_camera_lens_pose_native_test PRIVATE MDKR_CAMERA_PRODUCTION_MATH=1)
    if(NOT MSVC)
        target_link_libraries(mdkr_camera_lens_pose_native_test PRIVATE m)
    endif()
    add_test(NAME camera_lens_pose_native COMMAND mdkr_camera_lens_pose_native_test)
    set_tests_properties(camera_lens_pose_native PROPERTIES
        ENVIRONMENT "MDKR_TRIG=table;MDKR_DEV_RUNTIME_TRIG=0")


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

    # Adventure Party session state machine (AP-02). Pure and ROM-free by
    # design; these tests are the module's whole edge-case surface — every
    # legal/illegal state-event pair, generation staleness, exact-once award
    # consumption and roster suspend/restore — so they must keep running even
    # when MDKR_ADVENTURE_PARTY_OMIT strips the module from the game binary
    # (the test compiles the sources directly and does not care).
    add_executable(mdkr_adventure_party_state_test
        ${CMAKE_SOURCE_DIR}/tests/test_adventure_party_state.c
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_state.c)
    target_include_directories(mdkr_adventure_party_state_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_adventure_party_state_test PRIVATE m)
    endif()
    add_test(NAME adventure_party_state COMMAND mdkr_adventure_party_state_test)

    # Adventure Party policy core (AP-03): the v1 capability table with its
    # fail-closed unknowns, the three-way count vocabulary, deterministic
    # transition arbitration and exact-once token issuance. Links the state
    # module too because the exact-once and count tests exercise a real
    # session end to end — that composition is the invariant.
    add_executable(mdkr_adventure_party_policy_test
        ${CMAKE_SOURCE_DIR}/tests/test_adventure_party_policy.c
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_policy.c
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_state.c)
    target_include_directories(mdkr_adventure_party_policy_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_adventure_party_policy_test PRIVATE m)
    endif()
    add_test(NAME adventure_party_policy COMMAND mdkr_adventure_party_policy_test)

    # Adventure Party read-only trace schema (AP-05). Pure formatters over the
    # state/policy value structs; links adventure_party_state.c only for
    # adventure_party_state_name() (the session line). The load-bearing check is
    # the cross-language golden contract: this C test compares one exemplar line
    # per fact class byte-exact against tests/data/adventure_party_trace_golden.txt,
    # and tests/adventure_party_trace.py parses the SAME file, so drift in either
    # language fails one of the two. The golden path is passed explicitly so the
    # comparison is independent of the CTest working directory.
    add_executable(mdkr_adventure_party_trace_test
        ${CMAKE_SOURCE_DIR}/tests/test_adventure_party_trace.c
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_trace.c
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_state.c)
    target_include_directories(mdkr_adventure_party_trace_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_adventure_party_trace_test PRIVATE m)
    endif()
    add_test(NAME adventure_party_trace
        COMMAND mdkr_adventure_party_trace_test
                ${CMAKE_SOURCE_DIR}/tests/data/adventure_party_trace_golden.txt)

    # Adventure Party formation planner (AP-07, pure half). Deterministic
    # candidate geometry with no ROM: symmetry across the heading axis, byte
    # determinism, rigid rotation invariance, bounded/duplicate-free fallback,
    # count coverage 2/3/4, and typed rejection of degenerate params. Links m
    # for sinf/cosf (the one accepted per-platform variance). The
    # collision-validating game adapter is NOT this module and lands with Wave C.
    add_executable(mdkr_adventure_party_spawn_test
        ${CMAKE_SOURCE_DIR}/tests/test_adventure_party_spawn.c
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_spawn.c)
    target_include_directories(mdkr_adventure_party_spawn_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_adventure_party_spawn_test PRIVATE m)
    endif()
    add_test(NAME adventure_party_spawn COMMAND mdkr_adventure_party_spawn_test)

    # Adventure Party runtime singleton (AP-06a, controller ruling R12): the one
    # process-wide session and the two queries adapters and save_state reach it
    # through. Links adventure_party_state.c because the lifecycle assertions
    # drive a real FORM through the reducer. Compiled directly and never keyed on
    # MDKR_ADVENTURE_PARTY_OMIT, so it exercises the real runtime on both arms.
    add_executable(mdkr_adventure_party_runtime_test
        ${CMAKE_SOURCE_DIR}/tests/test_adventure_party_runtime.c
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_runtime.c
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_state.c)
    target_include_directories(mdkr_adventure_party_runtime_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_adventure_party_runtime_test PRIVATE m)
    endif()
    add_test(NAME adventure_party_runtime
        COMMAND mdkr_adventure_party_runtime_test)

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

    # Generic modern-character caches are generated from a tiny license-clean
    # animated GLB at test time. The native half proves that the engine-facing
    # loader consumes the exact compiler output and rejects corrupt headers and
    # payloads before publishing any section pointer.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME camera_chain_evidence
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_camera_chain_evidence.py)
    add_test(NAME camera_held_shot_fault_reporting
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_camera_held_shot_faults.py)
    add_test(NAME camera_envelope_long_arc_control
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/check_camera_envelope_control.py
            $<TARGET_FILE:mdkr_camera_lens_pose_native_test>)
    add_test(NAME character_asset_probe
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_character_asset_probe.py)
    add_executable(mdkr_modern_character_asset_test
        ${CMAKE_SOURCE_DIR}/tests/test_modern_character_asset.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_asset.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_install.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_registry.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_pose.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_text.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_render.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_identity.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_runtime.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_surface_intersection.c
        ${CMAKE_SOURCE_DIR}/platform/workshop_preview_runtime.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_donor.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_mipgen.c
        ${CMAKE_SOURCE_DIR}/lib/stb/stb_image_impl.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_modern_character_asset_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/platform/fast3d
        ${CMAKE_SOURCE_DIR}/lib/stb
        ${CMAKE_SOURCE_DIR}/lib/miniz)
    if(NOT MSVC)
        target_link_libraries(mdkr_modern_character_asset_test PRIVATE m)
    endif()
    add_test(NAME modern_character_asset
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/run_modern_character_asset_test.py
                --loader $<TARGET_FILE:mdkr_modern_character_asset_test>)

    # The registry's per-asset scan over a real installed-cache directory. It
    # gets its own target rather than joining the loader test above because the
    # defect it pins is in the SCAN, not in the loader: a fixed local indexed by
    # a validated count. Under the sanitizer arm the pre-fix code reports a
    # stack-buffer-overflow write; nothing else in the tree reached it.
    add_executable(mdkr_modern_character_registry_test
        ${CMAKE_SOURCE_DIR}/tests/test_modern_character_registry.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_registry.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_asset.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_text.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_identity.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/lib/stb/stb_image_impl.c)
    target_include_directories(mdkr_modern_character_registry_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/lib/stb)
    if(NOT MSVC)
        target_link_libraries(mdkr_modern_character_registry_test PRIVATE m)
    endif()
    add_test(NAME modern_character_registry
        COMMAND mdkr_modern_character_registry_test
                ${CMAKE_SOURCE_DIR}/tests/fuzz_corpus/modern_character_asset/sixtyfive-animations.mdkc)
    add_test(NAME character_package_manager
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_character_package_manager.py)
    add_test(NAME character_source_adapter
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_character_source_adapter.py)
    add_test(NAME character_importer_build
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_character_importer_build.py)
    add_test(NAME character_text_font_generator
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_character_text_font_generator.py)
    add_test(NAME gltf_validator_adapter
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_gltf_validator_adapter.py)
    add_test(NAME character_manifest_wizard
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_character_manifest_wizard.py)
    add_test(NAME character_spike_fixture
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_character_spike_fixture.py)
    add_test(NAME character_spike_evidence
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_character_spike_evidence.py)
    add_test(NAME character_release_evidence
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_character_release_evidence.py)
    add_executable(mdkr_character_workshop_model_test
        ${CMAKE_SOURCE_DIR}/tests/test_character_workshop_model.cpp
        ${CMAKE_SOURCE_DIR}/platform/app/character_workshop_model.cpp)
    target_include_directories(mdkr_character_workshop_model_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/platform/app)
    target_compile_features(mdkr_character_workshop_model_test PRIVATE cxx_std_17)
    add_test(NAME character_workshop_model
        COMMAND mdkr_character_workshop_model_test)
    # Exercise the real native input widget with ImGui's CPU-only core. No
    # platform/render backend, OS clipboard, shell hook, SDL, GPU or ROM.
    add_executable(mdkr_online_join_code_input_test
        ${CMAKE_SOURCE_DIR}/tests/test_online_join_code_input.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui_draw.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui_tables.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui_widgets.cpp)
    target_include_directories(mdkr_online_join_code_input_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/app ${CMAKE_SOURCE_DIR}/lib/imgui)
    target_compile_features(mdkr_online_join_code_input_test PRIVATE cxx_std_17)
    target_compile_definitions(mdkr_online_join_code_input_test PRIVATE
        IMGUI_DISABLE_WIN32_FUNCTIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
    add_test(NAME online_join_code_input COMMAND mdkr_online_join_code_input_test)
    # The launcher's navigation setup on a launcher-shaped frame, ImGui's CPU
    # core only: Escape must never leave forward Tab with nowhere to go.
    add_executable(mdkr_app_navigation_test
        ${CMAKE_SOURCE_DIR}/tests/test_app_navigation.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui_draw.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui_tables.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/imgui_widgets.cpp)
    target_include_directories(mdkr_app_navigation_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/app ${CMAKE_SOURCE_DIR}/lib/imgui)
    target_compile_features(mdkr_app_navigation_test PRIVATE cxx_std_17)
    target_compile_definitions(mdkr_app_navigation_test PRIVATE
        IMGUI_DISABLE_WIN32_FUNCTIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
    add_test(NAME app_navigation COMMAND mdkr_app_navigation_test)
    add_executable(mdkr_online_room_takeover_policy_test
        ${CMAKE_SOURCE_DIR}/tests/test_online_room_takeover_policy.cpp
        # The actual adapter interface header includes its inline fake seam.
        # GCC can emit that virtual-view candidate even when this fixture owns
        # only ViewAdapter. Link its real ROM-free dependencies; do not stub
        # the function or disable the compiler's virtual-call optimization.
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_fake_adapter.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_view_model.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
        ${CMAKE_SOURCE_DIR}/platform/session/session_core.c)
    target_include_directories(mdkr_online_room_takeover_policy_test PRIVATE
        ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/platform ${CMAKE_SOURCE_DIR}/platform/app)
    target_compile_features(mdkr_online_room_takeover_policy_test PRIVATE cxx_std_17)
    add_test(NAME online_room_takeover_policy COMMAND mdkr_online_room_takeover_policy_test)
    add_executable(mdkr_online_teardown_tracker_test
        ${CMAKE_SOURCE_DIR}/tests/test_online_teardown_tracker.cpp)
    target_include_directories(mdkr_online_teardown_tracker_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/app)
    target_compile_features(mdkr_online_teardown_tracker_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_online_teardown_tracker_test PRIVATE Threads::Threads)
    add_test(NAME online_teardown_tracker COMMAND mdkr_online_teardown_tracker_test)
    set_tests_properties(online_teardown_tracker PROPERTIES TIMEOUT 30)
    # Observe real shared futures without RTC, SDL, sockets or application code.
    add_executable(mdkr_app_cleanup_completion_test
        ${CMAKE_SOURCE_DIR}/tests/test_app_cleanup_completion.cpp)
    target_include_directories(mdkr_app_cleanup_completion_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/app)
    target_compile_features(mdkr_app_cleanup_completion_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_app_cleanup_completion_test PRIVATE Threads::Threads)
    add_test(NAME app_cleanup_completion COMMAND mdkr_app_cleanup_completion_test)
    set_tests_properties(app_cleanup_completion PROPERTIES TIMEOUT 30)
    # Compile the same reserved worker used by the amended RTC token. No RTC,
    # sockets, renderer or application is linked into this focused fixture.
    add_executable(mdkr_datachannel_cleanup_worker_test
        ${CMAKE_SOURCE_DIR}/tests/test_datachannel_cleanup_worker.cpp)
    target_include_directories(mdkr_datachannel_cleanup_worker_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/online)
    target_compile_features(mdkr_datachannel_cleanup_worker_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_datachannel_cleanup_worker_test PRIVATE Threads::Threads)
    add_test(NAME datachannel_cleanup_worker COMMAND mdkr_datachannel_cleanup_worker_test)
    set_tests_properties(datachannel_cleanup_worker PROPERTIES TIMEOUT 30)
    foreach(MDKR_STARTUP_FIXTURE datachannel_startup_stages rtc_initialization_transaction datachannel_work_admission datachannel_prepared_work)
        add_executable(mdkr_${MDKR_STARTUP_FIXTURE}_test
            ${CMAKE_SOURCE_DIR}/tests/test_${MDKR_STARTUP_FIXTURE}.cpp)
        target_include_directories(mdkr_${MDKR_STARTUP_FIXTURE}_test PRIVATE
            ${CMAKE_SOURCE_DIR}/platform/online)
        target_compile_features(mdkr_${MDKR_STARTUP_FIXTURE}_test PRIVATE cxx_std_17)
        target_link_libraries(mdkr_${MDKR_STARTUP_FIXTURE}_test PRIVATE Threads::Threads)
        add_test(NAME ${MDKR_STARTUP_FIXTURE} COMMAND mdkr_${MDKR_STARTUP_FIXTURE}_test)
        set_tests_properties(${MDKR_STARTUP_FIXTURE} PROPERTIES TIMEOUT 30)
    endforeach()
    # Callback identity is shared by Phone Party's actual locked admissions.
    # This pure fixture has no RTC, network, renderer or application dependency.
    add_executable(mdkr_party_callback_identity_test
        ${CMAKE_SOURCE_DIR}/tests/test_party_callback_identity.cpp)
    target_include_directories(mdkr_party_callback_identity_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_party_callback_identity_test PRIVATE cxx_std_17)
    add_test(NAME party_callback_identity COMMAND mdkr_party_callback_identity_test)
    set_tests_properties(party_callback_identity PROPERTIES TIMEOUT 30)
    add_executable(mdkr_party_peer_setup_retry_test
        ${CMAKE_SOURCE_DIR}/tests/test_party_peer_setup_retry.cpp)
    target_include_directories(mdkr_party_peer_setup_retry_test PRIVATE ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_party_peer_setup_retry_test PRIVATE cxx_std_17)
    add_test(NAME party_peer_setup_retry COMMAND mdkr_party_peer_setup_retry_test)
    set_tests_properties(party_peer_setup_retry PROPERTIES TIMEOUT 30)
    add_executable(mdkr_network_lifetime_test ${CMAKE_SOURCE_DIR}/tests/test_network_lifetime.cpp)
    target_include_directories(mdkr_network_lifetime_test PRIVATE ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_network_lifetime_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_network_lifetime_test PRIVATE Threads::Threads)
    add_test(NAME network_lifetime COMMAND mdkr_network_lifetime_test)
    set_tests_properties(network_lifetime PROPERTIES TIMEOUT 30)
    add_executable(mdkr_async_work_budget_test
        ${CMAKE_SOURCE_DIR}/tests/test_async_work_budget.cpp
        ${CMAKE_SOURCE_DIR}/tests/async_work_budget_other_tu.cpp)
    target_include_directories(mdkr_async_work_budget_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_async_work_budget_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_async_work_budget_test PRIVATE Threads::Threads)
    add_test(NAME async_work_budget COMMAND mdkr_async_work_budget_test)
    set_tests_properties(async_work_budget PROPERTIES TIMEOUT 30)
    add_executable(mdkr_ai_difficulty_value_test
        ${CMAKE_SOURCE_DIR}/tests/test_ai_difficulty_value.c)
    target_include_directories(mdkr_ai_difficulty_value_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME ai_difficulty_value COMMAND mdkr_ai_difficulty_value_test)
    add_executable(mdkr_character_async_job_test
        ${CMAKE_SOURCE_DIR}/tests/test_character_async_job.cpp)
    target_include_directories(mdkr_character_async_job_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/app)
    target_compile_features(mdkr_character_async_job_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_character_async_job_test PRIVATE Threads::Threads)
    add_test(NAME character_async_job
        COMMAND mdkr_character_async_job_test)
    add_executable(mdkr_modern_character_gpu_timing_test
        ${CMAKE_SOURCE_DIR}/tests/test_modern_character_gpu_timing.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_gpu_timing.c)
    target_include_directories(mdkr_modern_character_gpu_timing_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME modern_character_gpu_timing
        COMMAND mdkr_modern_character_gpu_timing_test)
    add_executable(mdkr_modern_character_draw_store_test
        ${CMAKE_SOURCE_DIR}/tests/test_modern_character_draw_store.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_draw_store.c)
    target_include_directories(mdkr_modern_character_draw_store_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/platform/fast3d)
    add_test(NAME modern_character_draw_store
        COMMAND mdkr_modern_character_draw_store_test)
    add_executable(mdkr_modern_character_capture_projection_test
        ${CMAKE_SOURCE_DIR}/tests/test_modern_character_capture_projection.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_capture_projection.c)
    target_include_directories(
        mdkr_modern_character_capture_projection_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(
            mdkr_modern_character_capture_projection_test PRIVATE m)
    endif()
    add_test(NAME modern_character_capture_projection
        COMMAND mdkr_modern_character_capture_projection_test)

    add_executable(mdkr_modern_character_surface_intersection_test
        ${CMAKE_SOURCE_DIR}/tests/test_modern_character_surface_intersection.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_surface_intersection.c)
    target_include_directories(
        mdkr_modern_character_surface_intersection_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_options(
        mdkr_modern_character_surface_intersection_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    if(NOT MSVC)
        target_link_libraries(
            mdkr_modern_character_surface_intersection_test PRIVATE m)
    endif()
    add_test(NAME modern_character_surface_intersection
        COMMAND mdkr_modern_character_surface_intersection_test)
    add_executable(mdkr_png_write_layout_test
        ${CMAKE_SOURCE_DIR}/tests/test_png_write_layout.c
        ${CMAKE_SOURCE_DIR}/lib/stb/stb_image_impl.c)
    target_include_directories(mdkr_png_write_layout_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform ${CMAKE_SOURCE_DIR}/lib/stb)
    if(NOT MSVC)
        target_link_libraries(mdkr_png_write_layout_test PRIVATE m)
    endif()
    add_test(NAME png_write_layout COMMAND mdkr_png_write_layout_test)

    add_executable(mdkr_stb_conversion_ownership_test
        ${CMAKE_SOURCE_DIR}/tests/test_stb_conversion_ownership.c)
    target_include_directories(mdkr_stb_conversion_ownership_test PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/stb)
    if(NOT MSVC)
        target_compile_options(mdkr_stb_conversion_ownership_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        target_link_libraries(mdkr_stb_conversion_ownership_test PRIVATE m)
    endif()
    add_test(NAME stb_conversion_ownership COMMAND mdkr_stb_conversion_ownership_test)

    add_executable(mdkr_character_portrait_studio_test
        ${CMAKE_SOURCE_DIR}/tests/test_character_portrait_studio.cpp
        ${CMAKE_SOURCE_DIR}/platform/app/character_png_validation.cpp
        ${CMAKE_SOURCE_DIR}/platform/app/character_portrait_import.cpp
        ${CMAKE_SOURCE_DIR}/platform/app/character_portrait_studio.cpp
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c
        ${CMAKE_SOURCE_DIR}/lib/stb/stb_image_impl.c)
    target_include_directories(mdkr_character_portrait_studio_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/platform/app
        ${CMAKE_SOURCE_DIR}/lib/stb)
    target_compile_features(mdkr_character_portrait_studio_test PRIVATE cxx_std_17)
    add_test(NAME character_portrait_studio
        COMMAND mdkr_character_portrait_studio_test)
    add_test(NAME collada_to_glb
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_collada_to_glb.py)
    add_test(NAME high_fidelity_character
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_high_fidelity_character.py)
    if(MDKR_CHARACTER_LOD_TOOLS)
        add_test(NAME character_lod_builder
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/test_character_lod_builder.py
                    --helper $<TARGET_FILE:mdkr-character-lod>)
    endif()

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
        ${CMAKE_SOURCE_DIR}/lib/miniz
        ${SDL2_INCLUDE_DIRS})
    # Issue #66: the store decodes on a background thread, so this links SDL
    # for its threads and mutexes (never initialised, no device opened, as in
    # the speech-worker test below). The two TESTING defines compile the
    # store's park/fail/limit seams and mod_source's I/O counters into THIS
    # target only; nothing that ships contains either.
    target_link_directories(mdkr_mod_texture_store_test PRIVATE
        ${SDL2_LIBRARY_DIRS})
    target_link_libraries(mdkr_mod_texture_store_test PRIVATE ${SDL2_LIBRARIES})
    target_compile_definitions(mdkr_mod_texture_store_test PRIVATE
        SDL_MAIN_HANDLED MDKR_MOD_TEXTURE_STORE_TESTING MDKR_MOD_SOURCE_TESTING)
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
    # Every wait in it is bounded by the worker's own progress; this is the
    # backstop for a hand-off deadlock, which must fail rather than hang.
    set_tests_properties(mod_texture_store PROPERTIES TIMEOUT 120)

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
    # The adventure_party runtime + state sources are here because the
    # party-active refusal is wired to the runtime singleton: the test drives a
    # real session through it to prove validate()/read() fail closed while a
    # party is live, and pass unchanged otherwise. Still NOTHING from
    # save_container.c or save_codec.c — the two save formats stay separate.
    add_executable(mdkr_save_state_container_test
        ${CMAKE_SOURCE_DIR}/tests/test_save_state_container.c
        ${CMAKE_SOURCE_DIR}/platform/save_state.c
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_runtime.c
        ${CMAKE_SOURCE_DIR}/platform/adventure_party/adventure_party_state.c)
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
    # Windows reads folder packs by pathname. The same sources built with
    # MDKR_PACK_PATH_ARM run that reader here, so its behaviour is proved on
    # every host the suite runs on, not only on Windows.
    if(NOT WIN32)
        add_executable(mdkr_mod_source_zip_path_test
            ${CMAKE_SOURCE_DIR}/tests/test_mod_source_zip.c
            ${CMAKE_SOURCE_DIR}/platform/mod_source.c
            ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
            ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
        target_include_directories(mdkr_mod_source_zip_path_test PRIVATE
            ${CMAKE_SOURCE_DIR}/platform
            ${CMAKE_SOURCE_DIR}/lib/miniz)
        target_compile_definitions(mdkr_mod_source_zip_path_test PRIVATE
            MDKR_PACK_PATH_ARM=1)
        target_link_libraries(mdkr_mod_source_zip_path_test PRIVATE m)
        add_test(NAME mod_source_zip_path COMMAND mdkr_mod_source_zip_path_test
                 ${CMAKE_CURRENT_BINARY_DIR}/mod_source_path_scratch)
    endif()

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

    add_executable(mdkr_magic_codes_state_test
        ${CMAKE_SOURCE_DIR}/tests/test_magic_codes_state.c
        ${CMAKE_SOURCE_DIR}/platform/magic_codes_state.c)
    target_include_directories(mdkr_magic_codes_state_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_magic_codes_state_test PRIVATE
        MAGIC_CODES_STATE_TESTING=1)
    add_test(NAME magic_codes_state COMMAND mdkr_magic_codes_state_test)

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

    add_executable(mdkr_custom_character_roster_test
        ${CMAKE_SOURCE_DIR}/tests/test_custom_character_roster.c
        ${CMAKE_SOURCE_DIR}/game/src/custom_character_roster.c
        ${CMAKE_SOURCE_DIR}/platform/modern_character_text.c)
    target_include_directories(mdkr_custom_character_roster_test PRIVATE
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_custom_character_roster_test PRIVATE
        NATIVE_PORT=1 VERSION_us_v80 _LANGUAGE_C MODERN_CC)
    add_test(NAME custom_character_roster COMMAND mdkr_custom_character_roster_test)

    add_executable(mdkr_character_text_test
        ${CMAKE_SOURCE_DIR}/tests/test_character_text.c
        ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_character_text.c)
    target_include_directories(mdkr_character_text_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/platform/fast3d
        ${CMAKE_SOURCE_DIR}/lib/stb)
    target_link_libraries(mdkr_character_text_test PRIVATE
        mdkr_character_text_shaping)
    if(NOT WIN32)
        target_link_libraries(mdkr_character_text_test PRIVATE m)
    endif()
    add_test(NAME character_text COMMAND mdkr_character_text_test)

    add_executable(mdkr_taj_mod_state_file_test
        ${CMAKE_SOURCE_DIR}/tests/test_taj_mod_state_file.c
        ${CMAKE_SOURCE_DIR}/platform/magic_codes_state.c
        ${CMAKE_SOURCE_DIR}/platform/magic_codes_state_file.c
        ${CMAKE_SOURCE_DIR}/platform/taj_mod_state.c
        ${CMAKE_SOURCE_DIR}/platform/taj_mod_state_file.c
        ${CMAKE_SOURCE_DIR}/platform/text_state_file.c
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
        ${CMAKE_SOURCE_DIR}/game/src/memory.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_snapshot.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_engine_registry.c)
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

    add_executable(mdkr_fast3d_dl_guards_test
        ${CMAKE_SOURCE_DIR}/tests/test_fast3d_dl_guards.c)
    target_include_directories(mdkr_fast3d_dl_guards_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/platform/fast3d
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR)
    target_compile_definitions(mdkr_fast3d_dl_guards_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        NATIVE_PORT=1)
    add_test(NAME fast3d_dl_guards COMMAND mdkr_fast3d_dl_guards_test)

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

    # The Rice/GLideN64 texture key, held to the reference implementation in
    # tools/ricepack/rice_crc.py. The two must agree exactly: the runtime uses
    # this to resolve a pack's textures while the offline importer uses the
    # Python, so a divergence would stop shipped packs resolving while every
    # authoring tool kept working.
    add_executable(mdkr_rice_crc_test
        ${CMAKE_SOURCE_DIR}/tests/test_rice_crc.c
        ${CMAKE_SOURCE_DIR}/platform/rice_crc.c)
    target_include_directories(mdkr_rice_crc_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    # `rice_crc` is already taken by the Python implementation's own test.
    # This one exists to hold the two together.
    add_test(NAME rice_crc_parity COMMAND mdkr_rice_crc_test)

    # Issue #33: portable.txt marker + home-write fallback resolve config/save/
    # mods next to the executable. Two processes because portable detection
    # caches once per run; they share no on-disk state (see the test header).
    add_executable(mdkr_portable_paths_test
        ${CMAKE_SOURCE_DIR}/tests/test_portable_paths.c
        ${CMAKE_SOURCE_DIR}/platform/user_paths.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c)
    target_include_directories(mdkr_portable_paths_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform ${CMAKE_SOURCE_DIR}/tests)
    add_test(NAME portable_paths_marker COMMAND mdkr_portable_paths_test)
    add_test(NAME portable_paths_fallback
        COMMAND mdkr_portable_paths_test --fallback)
    # The marker arm creates portable.txt beside the shared test executable and
    # removes it on exit; a --fallback arm scheduled into that window sees the
    # marker and fails five assertions ("not portable without a marker" first).
    # Both arms probe the same on-disk directory, so they must never overlap
    # under ctest -j.
    set_tests_properties(portable_paths_marker portable_paths_fallback
        PROPERTIES RESOURCE_LOCK portable_paths_executable_dir)
    # Issue #54: an AppImage runs from a read-only mount, so portable.txt and the
    # write-fallback must resolve beside the real on-disk AppImage ($APPIMAGE),
    # not the mount. Its own process because portable detection caches once.
    add_test(NAME portable_paths_appimage
        COMMAND mdkr_portable_paths_test --appimage)

    # Issue #54: a non-packaged native build resolves saves under the per-user
    # preference directory (launch-independent). An EMPTY $CWD/save is NOT
    # grandfathered; a POPULATED legacy $CWD/save is copy-migrated there. Two
    # processes because the CWD/pref resolution caches once per run.
    add_executable(mdkr_save_resolution_test
        ${CMAKE_SOURCE_DIR}/tests/test_save_resolution.c
        ${CMAKE_SOURCE_DIR}/platform/user_paths.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c)
    target_include_directories(mdkr_save_resolution_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform ${CMAKE_SOURCE_DIR}/tests)
    add_test(NAME save_resolution_per_user COMMAND mdkr_save_resolution_test)
    add_test(NAME save_resolution_legacy
        COMMAND mdkr_save_resolution_test --legacy)

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

    add_executable(mdkr_session_core_test
        ${CMAKE_SOURCE_DIR}/tests/test_session_core.c
        ${CMAKE_SOURCE_DIR}/platform/session/session_core.c)
    target_include_directories(mdkr_session_core_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME session_core COMMAND mdkr_session_core_test)

    # Native online BETA live selection bridge: forward-feed snapshot runtime,
    # one-shot reverse-feed intent channel, and the pure view-model+lobby ->
    # snapshot projection. party_link.c reads the launcher lobby/view-model
    # structs by value only, so no lobby_core.c/lobby_view_model.c link is
    # needed. Compiled here WITHOUT the beta macro (the file carries no #if beta
    # guards) so the bridge is unit-tested in every build. ROM-free.
    add_executable(mdkr_party_link_test
        ${CMAKE_SOURCE_DIR}/tests/test_party_link.c
        ${CMAKE_SOURCE_DIR}/platform/net/party_link.c)
    target_include_directories(mdkr_party_link_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME party_link COMMAND mdkr_party_link_test)

    add_executable(mdkr_session_bridge_test
        ${CMAKE_SOURCE_DIR}/tests/test_session_bridge.c
        ${CMAKE_SOURCE_DIR}/platform/session/session_bridge.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_roster.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_local_input.c)
    target_include_directories(mdkr_session_bridge_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME session_bridge COMMAND mdkr_session_bridge_test)

    add_executable(mdkr_match_transport_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_transport.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_transport.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_failure_ring.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_input.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_local_input.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_roster.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
        ${CMAKE_SOURCE_DIR}/platform/session/session_bridge.c)
    target_include_directories(mdkr_match_transport_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME match_transport COMMAND mdkr_match_transport_test)

    add_executable(mdkr_match_input_runtime_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_input_runtime.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_input_runtime.c)
    target_include_directories(mdkr_match_input_runtime_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME match_input_runtime COMMAND mdkr_match_input_runtime_test)

    add_executable(mdkr_match_input_packet_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_input_packet.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_input_packet.c)
    target_include_directories(mdkr_match_input_packet_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME match_input_packet COMMAND mdkr_match_input_packet_test)

    add_executable(mdkr_match_input_bundle_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_input_bundle.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_input_bundle.c)
    target_include_directories(mdkr_match_input_bundle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR})
    add_test(NAME match_input_bundle COMMAND mdkr_match_input_bundle_test)

    add_executable(mdkr_match_input_repair_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_input_repair.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_input_repair.c)
    target_include_directories(mdkr_match_input_repair_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR})
    add_test(NAME match_input_repair COMMAND mdkr_match_input_repair_test)

    add_executable(mdkr_match_peer_graph_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_peer_graph.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_peer_graph.c)
    target_include_directories(mdkr_match_peer_graph_test PRIVATE
        ${CMAKE_SOURCE_DIR})
    if(MSVC)
        target_compile_options(mdkr_match_peer_graph_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_match_peer_graph_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME match_peer_graph COMMAND mdkr_match_peer_graph_test)

    add_executable(mdkr_match_peer_forward_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_peer_forward.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_peer_forward.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_peer_graph.c)
    target_include_directories(mdkr_match_peer_forward_test PRIVATE
        ${CMAKE_SOURCE_DIR})
    if(MSVC)
        target_compile_options(mdkr_match_peer_forward_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_match_peer_forward_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME match_peer_forward COMMAND mdkr_match_peer_forward_test)

    add_executable(mdkr_match_peer_transcript_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_peer_transcript.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_peer_transcript.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_peer_graph.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_match_peer_transcript_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    if(MSVC)
        target_compile_options(mdkr_match_peer_transcript_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_match_peer_transcript_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME match_peer_transcript COMMAND mdkr_match_peer_transcript_test)

    add_executable(mdkr_match_preflight_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_preflight.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_preflight.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_peer_graph.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_match_preflight_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    if(MSVC)
        target_compile_options(mdkr_match_preflight_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_match_preflight_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME match_preflight COMMAND mdkr_match_preflight_test)

    add_executable(mdkr_match_route_quality_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_route_quality.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_preflight.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_peer_graph.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_impairment.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_match_route_quality_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    if(MSVC)
        target_compile_options(mdkr_match_route_quality_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_match_route_quality_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME match_route_quality COMMAND mdkr_match_route_quality_test)

    add_executable(mdkr_online_race_results_test
        ${CMAKE_SOURCE_DIR}/tests/test_online_race_results.c
        ${CMAKE_SOURCE_DIR}/platform/net/online_race_results.c)
    target_include_directories(mdkr_online_race_results_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    if(MSVC)
        target_compile_options(mdkr_online_race_results_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_online_race_results_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME online_race_results COMMAND mdkr_online_race_results_test)

    add_executable(mdkr_online_lobby_core_test
        ${CMAKE_SOURCE_DIR}/tests/test_online_lobby_core.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c)
    target_include_directories(mdkr_online_lobby_core_test PRIVATE
        ${CMAKE_SOURCE_DIR})
    target_compile_definitions(mdkr_online_lobby_core_test PRIVATE
        MDKR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME online_lobby_core COMMAND mdkr_online_lobby_core_test)

    add_executable(mdkr_online_compatibility_identity_test
        ${CMAKE_SOURCE_DIR}/tests/test_online_compatibility_identity.c
        ${CMAKE_SOURCE_DIR}/platform/online/compatibility_identity.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_online_compatibility_identity_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    if(MSVC)
        target_compile_options(mdkr_online_compatibility_identity_test PRIVATE
            /W4 /WX)
    else()
        target_compile_options(mdkr_online_compatibility_identity_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME online_compatibility_identity
        COMMAND mdkr_online_compatibility_identity_test)

    add_executable(mdkr_online_lobby_view_model_test
        ${CMAKE_SOURCE_DIR}/tests/test_online_lobby_view_model.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_view_model.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
        ${CMAKE_SOURCE_DIR}/platform/session/session_core.c)
    target_include_directories(mdkr_online_lobby_view_model_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    # In a beta build, compile the beta-gated race-scoped recovery cards
    # (OPPONENT_LEFT / OPPONENT_NEVER_STARTED) into lobby_view_model.c so this
    # test can pin their copy. A release (beta OFF) build leaves the macro unset
    # and lobby_view_model.o stays byte-identical.
    if(MDKR_ENABLE_ONLINE_BETA)
        target_compile_definitions(mdkr_online_lobby_view_model_test PRIVATE
            MDKR_ENABLE_ONLINE_BETA=1)
    endif()
    add_test(NAME online_lobby_view_model
        COMMAND mdkr_online_lobby_view_model_test)

    add_executable(mdkr_online_lobby_fake_adapter_test
        ${CMAKE_SOURCE_DIR}/tests/test_online_lobby_fake_adapter.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_fake_adapter.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_view_model.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
        ${CMAKE_SOURCE_DIR}/platform/session/session_core.c)
    target_include_directories(mdkr_online_lobby_fake_adapter_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME online_lobby_fake_adapter
        COMMAND mdkr_online_lobby_fake_adapter_test)

    # Compile the Emscripten-facing projection as ordinary C so its complete
    # 43-case/27-action ABI can be exercised without starting a browser.
    add_executable(mdkr_online_lobby_browser_wasm_test
        ${CMAKE_SOURCE_DIR}/tests/test_online_lobby_browser_wasm.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_browser_wasm.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_fake_adapter.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_view_model.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
        ${CMAKE_SOURCE_DIR}/platform/session/session_core.c)
    target_include_directories(mdkr_online_lobby_browser_wasm_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    if(MSVC)
        target_compile_options(mdkr_online_lobby_browser_wasm_test PRIVATE
            /W4 /WX)
    else()
        target_compile_options(mdkr_online_lobby_browser_wasm_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME online_lobby_browser_wasm
        COMMAND mdkr_online_lobby_browser_wasm_test)

    add_executable(mdkr_match_launch_descriptor_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_launch_descriptor.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
        ${CMAKE_SOURCE_DIR}/platform/online/match_launch_builder.c
        ${CMAKE_SOURCE_DIR}/platform/online/lobby_core.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c)
    target_include_directories(mdkr_match_launch_descriptor_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME match_launch_descriptor
        COMMAND mdkr_match_launch_descriptor_test)

    add_executable(mdkr_pad_router_test
        ${CMAKE_SOURCE_DIR}/tests/test_pad_router.c
        ${CMAKE_SOURCE_DIR}/platform/pad_router.c)
    target_include_directories(mdkr_pad_router_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME pad_router COMMAND mdkr_pad_router_test)

    add_executable(mdkr_party_protocol_test
        ${CMAKE_SOURCE_DIR}/tests/test_party_protocol.c
        ${CMAKE_SOURCE_DIR}/platform/party/party_protocol.c)
    target_include_directories(mdkr_party_protocol_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME party_protocol COMMAND mdkr_party_protocol_test)

    add_executable(mdkr_remote_pad_test
        ${CMAKE_SOURCE_DIR}/tests/test_remote_pad.c
        ${CMAKE_SOURCE_DIR}/platform/party/party_protocol.c
        ${CMAKE_SOURCE_DIR}/platform/party/remote_pad.c)
    target_include_directories(mdkr_remote_pad_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME remote_pad COMMAND mdkr_remote_pad_test)

    add_executable(mdkr_native_remote_pad_ingress_test
        ${CMAKE_SOURCE_DIR}/tests/test_native_remote_pad_ingress.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/native_remote_pad_ingress.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/party_protocol.c)
    target_include_directories(mdkr_native_remote_pad_ingress_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_native_remote_pad_ingress_test PRIVATE cxx_std_17)
    find_package(Threads REQUIRED)
    target_link_libraries(mdkr_native_remote_pad_ingress_test PRIVATE
        Threads::Threads)
    if(MSVC)
        target_compile_options(mdkr_native_remote_pad_ingress_test PRIVATE
            /W4 /WX)
    else()
        target_compile_options(mdkr_native_remote_pad_ingress_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME native_remote_pad_ingress
        COMMAND mdkr_native_remote_pad_ingress_test)

    # The transport's bounded event queue, isolated from the network so its
    # overflow policy (drop the oldest droppable pad packet; never drop a
    # room/control event; never latch) is directly testable without a real
    # WebSocket/WebRTC stack.
    add_executable(mdkr_party_event_queue_test
        ${CMAKE_SOURCE_DIR}/tests/test_party_event_queue.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/party_event_queue.cpp)
    target_include_directories(mdkr_party_event_queue_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_party_event_queue_test PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(mdkr_party_event_queue_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_party_event_queue_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME party_event_queue COMMAND mdkr_party_event_queue_test)

    # The transport's C3 signaling-retry decision, isolated from the network
    # (and from libdatachannel/mbedtls entirely) so the 20 s unauthenticated
    # deadline, 3-attempt give-up, and socket-reopen resend can be exercised
    # as plain arithmetic instead of through a real WebSocket/WebRTC stack.
    add_executable(mdkr_party_retry_policy_test
        ${CMAKE_SOURCE_DIR}/tests/test_party_transport_retry.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/party_retry_policy.cpp)
    target_include_directories(mdkr_party_retry_policy_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_party_retry_policy_test PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(mdkr_party_retry_policy_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_party_retry_policy_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME party_retry_policy COMMAND mdkr_party_retry_policy_test)

    # A7's soft-fail/hard-fail live liveness policy, isolated from the WebRTC
    # transport entirely so the scripted miss/hit sequence (first miss stays
    # Transient, Nth escalates to Unreachable) is exercised as plain
    # arithmetic instead of through the real control-ping ladder.
    add_executable(mdkr_match_peer_liveness_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_peer_liveness.cpp
        ${CMAKE_SOURCE_DIR}/platform/online/match_peer_liveness.cpp)
    target_include_directories(mdkr_match_peer_liveness_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_match_peer_liveness_test PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(mdkr_match_peer_liveness_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_match_peer_liveness_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME match_peer_liveness COMMAND mdkr_match_peer_liveness_test)

    add_executable(mdkr_native_party_host_test
        ${CMAKE_SOURCE_DIR}/tests/test_native_party_host.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/native_party_host.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/native_remote_pad_ingress.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/party_protocol.c)
    target_include_directories(mdkr_native_party_host_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_native_party_host_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_native_party_host_test PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(mdkr_native_party_host_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_native_party_host_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME native_party_host COMMAND mdkr_native_party_host_test)

    # Packaged-build Phone Party bring-up smoke: GPU-, network- and ROM-free,
    # so every release lane (including the Windows build+inspect lane, which
    # has no qualifying adapter) can qualify the pairing surface a phone
    # actually needs. qrcodegen.cpp is compiled in directly rather than linked
    # through mdkr_qrcodegen because this file is included from CMakeLists.txt
    # before that library — or MDKR_APP/MDKR_VERSION/MDKR_PARTY_ORIGIN — exist;
    # the release version and origin arrive by environment instead.
    add_executable(mdkr_native_party_bringup_test
        ${CMAKE_SOURCE_DIR}/tests/test_native_party_bringup.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/native_party_host.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/native_remote_pad_ingress.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/party_protocol.c
        ${CMAKE_SOURCE_DIR}/third_party/qrcodegen/qrcodegen.cpp)
    target_include_directories(mdkr_native_party_bringup_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/third_party/qrcodegen)
    target_compile_features(mdkr_native_party_bringup_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_native_party_bringup_test PRIVATE
        Threads::Threads)
    if(MSVC)
        target_compile_options(mdkr_native_party_bringup_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_native_party_bringup_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME native_party_bringup
        COMMAND mdkr_native_party_bringup_test)

    # A party session must survive the desktop events that interrupt it. The
    # packaged launcher cannot be driven with a live session from a test --
    # UiLauncher owns the host and no launcher smoke seam reaches Phone Party --
    # so these assert at the host + ingress level, driving the lifecycle each
    # event actually produces rather than the window message that causes it.
    add_executable(mdkr_party_session_lifecycle_test
        ${CMAKE_SOURCE_DIR}/tests/test_party_session_lifecycle.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/native_party_host.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/native_remote_pad_ingress.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/party_protocol.c)
    target_include_directories(mdkr_party_session_lifecycle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_party_session_lifecycle_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_party_session_lifecycle_test PRIVATE
        Threads::Threads)
    if(MSVC)
        target_compile_options(mdkr_party_session_lifecycle_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_party_session_lifecycle_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME party_session_lifecycle
        COMMAND mdkr_party_session_lifecycle_test)

    # Firewall-negative, native half: a refused connect, a refused WSS upgrade
    # and a silent mid-session drop must all fail neutral, release seats rather
    # than leave stale input, never self-retry, and say the shipped sentence.
    add_executable(mdkr_party_firewall_negative_test
        ${CMAKE_SOURCE_DIR}/tests/test_party_firewall_negative.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/native_party_host.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/native_remote_pad_ingress.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/party_protocol.c)
    target_include_directories(mdkr_party_firewall_negative_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_party_firewall_negative_test PRIVATE
        cxx_std_17)
    target_link_libraries(mdkr_party_firewall_negative_test PRIVATE
        Threads::Threads)
    if(MSVC)
        target_compile_options(mdkr_party_firewall_negative_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_party_firewall_negative_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME party_firewall_negative
        COMMAND mdkr_party_firewall_negative_test)

    # Phase 3 local-only play: the embedded HTTP+WS server that serves the
    # controller page and the /party-ws control socket from ONE port with no
    # internet. Wire-level test against 127.0.0.1 on an ephemeral port, so it
    # stays GPU-, network- and ROM-free like the rest of the party suite.
    # The /party-ws upgrade enforces a LAN Host allowlist (DNS-rebinding
    # defense at THIS layer); the test drives it through the allowed loopback
    # Hosts and asserts foreign Hosts are refused.
    #
    # MDKR_LAN_PARTY_TESTING is defined HERE AND NOWHERE ELSE. It compiles
    # the send-path park hook and the HTTP-deadline override into
    # lan_party_server.cpp so the teardown-vs-send ordering and the slow-drip
    # deadline are deterministically testable; without the define neither
    # seam exists in any shipped build (the MDKR_A11Y_SPEECH_TESTING pattern).
    add_executable(mdkr_lan_party_server_test
        ${CMAKE_SOURCE_DIR}/tests/test_lan_party_server.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/lan_party_server.cpp)
    target_include_directories(mdkr_lan_party_server_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_lan_party_server_test PRIVATE
        MDKR_LAN_PARTY_TESTING)
    target_compile_features(mdkr_lan_party_server_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_lan_party_server_test PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(mdkr_lan_party_server_test PRIVATE ws2_32)
    endif()
    if(MSVC)
        target_compile_options(mdkr_lan_party_server_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_lan_party_server_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME lan_party_server COMMAND mdkr_lan_party_server_test)
    # Every wait in it is bounded and short; this is the backstop for a
    # genuine hang (a stop() that never joins), which must fail the suite
    # rather than stall it.
    set_tests_properties(lan_party_server PROPERTIES TIMEOUT 120)

    # Phase 3 local-only play: the pure launcher-side config helpers -- pick the
    # QR's advertised LAN host from the machine's addresses, and build the
    # controller-asset manifest from a packaged web tree. No getifaddrs, no
    # resource bundle, no libdatachannel: just the two decisions worth pinning
    # (lan_party_launch.cpp). The getifaddrs/resource glue is app-only and, like
    # ui_*.cpp, has no unit harness.
    add_executable(mdkr_lan_party_launch_test
        ${CMAKE_SOURCE_DIR}/tests/test_lan_party_launch.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/lan_party_launch.cpp)
    target_include_directories(mdkr_lan_party_launch_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_lan_party_launch_test PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(mdkr_lan_party_launch_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_lan_party_launch_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME lan_party_launch COMMAND mdkr_lan_party_launch_test)

    # Phase 3 local-only play: the in-process local room -- the zero-internet
    # equivalent of the Cloudflare Party room DO. It mints the invite
    # capability + fallback code, holds seat leases, throttles code redemption,
    # and relays the pairing protocol between the phones' WebSockets and the
    # native host. The room is pure (no server, no libdatachannel, no network):
    # this test drives it against a fake controller socket and an in-memory
    # host sink with a deterministic clock and RNG, so it stays as cheap and
    # hermetic as the rest of the party unit suite.
    add_executable(mdkr_lan_party_room_test
        ${CMAKE_SOURCE_DIR}/tests/test_lan_party_room.cpp
        ${CMAKE_SOURCE_DIR}/platform/party/lan_party_room.cpp)
    target_include_directories(mdkr_lan_party_room_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_features(mdkr_lan_party_room_test PRIVATE cxx_std_17)
    target_link_libraries(mdkr_lan_party_room_test PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(mdkr_lan_party_room_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_lan_party_room_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME lan_party_room COMMAND mdkr_lan_party_room_test)
    set_tests_properties(lan_party_room PROPERTIES TIMEOUT 60)

    # Rollback primitives are kept ROM-, renderer- and window-free. Their
    # boundaries must stay cheap enough to run on every native presubmit.
    add_executable(mdkr_net_input_test
        ${CMAKE_SOURCE_DIR}/tests/test_net_input.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_input.c)
    target_include_directories(mdkr_net_input_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME net_input COMMAND mdkr_net_input_test)

    add_executable(mdkr_net_local_input_test
        ${CMAKE_SOURCE_DIR}/tests/test_net_local_input.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_local_input.c)
    target_include_directories(mdkr_net_local_input_test PRIVATE
        ${CMAKE_SOURCE_DIR})
    add_test(NAME net_local_input COMMAND mdkr_net_local_input_test)

    add_executable(mdkr_joypad_rollback_test
        ${CMAKE_SOURCE_DIR}/tests/test_joypad_rollback.c
        ${CMAKE_SOURCE_DIR}/game/src/joypad_rollback.c)
    target_include_directories(mdkr_joypad_rollback_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/game/libultra)
    target_compile_definitions(mdkr_joypad_rollback_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1)
    add_test(NAME joypad_rollback COMMAND mdkr_joypad_rollback_test)

    add_executable(mdkr_match_manifest_test
        ${CMAKE_SOURCE_DIR}/tests/test_match_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c)
    target_include_directories(mdkr_match_manifest_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME match_manifest COMMAND mdkr_match_manifest_test)

    add_executable(mdkr_net_roster_test
        ${CMAKE_SOURCE_DIR}/tests/test_net_roster.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_roster.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_roster_runtime.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_launch_descriptor.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c)
    target_include_directories(mdkr_net_roster_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME net_roster COMMAND mdkr_net_roster_test)

    add_executable(mdkr_local_listener_mix_test
        ${CMAKE_SOURCE_DIR}/tests/test_local_listener_mix.c
        ${CMAKE_SOURCE_DIR}/platform/net/local_listener_mix.c)
    target_include_directories(mdkr_local_listener_mix_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME local_listener_mix COMMAND mdkr_local_listener_mix_test)

    add_executable(mdkr_net_impairment_test
        ${CMAKE_SOURCE_DIR}/tests/test_net_impairment.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_impairment.c)
    target_include_directories(mdkr_net_impairment_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME net_impairment COMMAND mdkr_net_impairment_test)

    # Links the REAL path policy (with a link-time preference provider in the
    # test, as tests/test_save_resolution.c does) so the dump's fallback to the
    # app shell's log directory is proven end to end, without SDL.
    add_executable(mdkr_net_failure_ring_test
        ${CMAKE_SOURCE_DIR}/tests/test_net_failure_ring.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_failure_ring.c
        ${CMAKE_SOURCE_DIR}/platform/user_paths.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c)
    target_include_directories(mdkr_net_failure_ring_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME net_failure_ring COMMAND mdkr_net_failure_ring_test)

    add_executable(mdkr_net_clock_test
        ${CMAKE_SOURCE_DIR}/tests/test_net_clock.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_clock.c)
    target_include_directories(mdkr_net_clock_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME net_clock COMMAND mdkr_net_clock_test)

    add_executable(mdkr_rollback_snapshot_test
        ${CMAKE_SOURCE_DIR}/tests/test_rollback_snapshot.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_snapshot.c)
    target_include_directories(mdkr_rollback_snapshot_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME rollback_snapshot COMMAND mdkr_rollback_snapshot_test)

    add_executable(mdkr_rollback_ring_test
        ${CMAKE_SOURCE_DIR}/tests/test_rollback_ring.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_snapshot.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_ring.c)
    target_include_directories(mdkr_rollback_ring_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME rollback_ring COMMAND mdkr_rollback_ring_test)

    add_executable(mdkr_rollback_events_test
        ${CMAKE_SOURCE_DIR}/tests/test_rollback_events.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_events.c)
    target_include_directories(mdkr_rollback_events_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME rollback_events COMMAND mdkr_rollback_events_test)

    add_executable(mdkr_rollback_audio_test
        ${CMAKE_SOURCE_DIR}/tests/test_rollback_audio.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_audio.c)
    target_include_directories(mdkr_rollback_audio_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME rollback_audio COMMAND mdkr_rollback_audio_test)

    add_executable(mdkr_gameplay_event_trace_test
        ${CMAKE_SOURCE_DIR}/tests/test_gameplay_event_trace.c
        ${CMAKE_SOURCE_DIR}/platform/gameplay_event_trace.c)
    target_include_directories(mdkr_gameplay_event_trace_test PRIVATE
        ${CMAKE_SOURCE_DIR})
    add_test(NAME gameplay_event_trace COMMAND mdkr_gameplay_event_trace_test)

    add_executable(mdkr_rollback_driver_test
        ${CMAKE_SOURCE_DIR}/tests/test_rollback_driver.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_input.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_snapshot.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_ring.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_events.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_driver.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_failure_ring.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c)
    target_include_directories(mdkr_rollback_driver_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME rollback_driver COMMAND mdkr_rollback_driver_test)

    add_executable(mdkr_rollback_four_endpoint_test
        ${CMAKE_SOURCE_DIR}/tests/test_rollback_four_endpoint.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_input.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_impairment.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_snapshot.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_ring.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_events.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_driver.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_failure_ring.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c)
    target_include_directories(mdkr_rollback_four_endpoint_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME rollback_four_endpoint COMMAND mdkr_rollback_four_endpoint_test)

    add_executable(mdkr_rollback_game_runtime_test
        ${CMAKE_SOURCE_DIR}/tests/test_rollback_game_runtime.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_game_runtime.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_audio.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_events.c
        ${CMAKE_SOURCE_DIR}/platform/gameplay_event_trace.c
        ${CMAKE_SOURCE_DIR}/platform/net/match_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/net/net_input.c)
    target_include_directories(mdkr_rollback_game_runtime_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/game/libultra)
    target_compile_definitions(mdkr_rollback_game_runtime_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1
        MDKR_ROLLBACK_LAB_RESIM_TARGET_TICK=4)
    add_test(NAME rollback_game_runtime COMMAND mdkr_rollback_game_runtime_test)

    add_executable(mdkr_rollback_game_authority_test
        ${CMAKE_SOURCE_DIR}/tests/test_rollback_game_authority.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_game_authority.c
        ${CMAKE_SOURCE_DIR}/platform/rollback/rollback_snapshot.c)
    target_include_directories(mdkr_rollback_game_authority_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/include/PR
        ${CMAKE_SOURCE_DIR}/game/libultra
        ${CMAKE_SOURCE_DIR}/platform)
    target_compile_definitions(mdkr_rollback_game_authority_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1)
    add_test(NAME rollback_game_authority COMMAND mdkr_rollback_game_authority_test)

    add_executable(mdkr_pacing_policy_test
        ${CMAKE_SOURCE_DIR}/tests/test_pacing_policy.c
        ${CMAKE_SOURCE_DIR}/platform/pacing_policy.c)
    target_include_directories(mdkr_pacing_policy_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME pacing_policy COMMAND mdkr_pacing_policy_test)

    add_executable(mdkr_steering_compat_test
        ${CMAKE_SOURCE_DIR}/tests/test_steering_compat.c
        ${CMAKE_SOURCE_DIR}/platform/steering_compat.c)
    target_include_directories(mdkr_steering_compat_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_steering_compat_test PRIVATE m)
    endif()
    add_test(NAME steering_compat COMMAND mdkr_steering_compat_test)

    add_executable(mdkr_hud_layout_test
        ${CMAKE_SOURCE_DIR}/tests/test_hud_layout.c
        ${CMAKE_SOURCE_DIR}/platform/hud_layout.c)
    target_include_directories(mdkr_hud_layout_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_hud_layout_test PRIVATE m)
    endif()
    add_test(NAME hud_layout COMMAND mdkr_hud_layout_test)

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
    if(NOT MSVC)
        # resolve_object_pair uses sqrtf; glibc needs an explicit -lm (found
        # by the Linux lane — macOS bundles it in libSystem and never fails).
        target_link_libraries(mdkr_presentation_snapshot_test PRIVATE m)
    endif()
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

    # A save the published 1.7.0 build wrote (tests/fixtures/save_v170) must
    # decode to the same progress and re-encode byte for byte, raw and as its
    # 1.7.0 .mdkr-save container.
    add_executable(mdkr_save_v170_fixture_test
        ${CMAKE_SOURCE_DIR}/tests/test_save_v170_fixture.c
        ${CMAKE_SOURCE_DIR}/platform/save_container.c
        ${CMAKE_SOURCE_DIR}/platform/save_codec.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_save_v170_fixture_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    add_test(NAME save_v170_fixture
        COMMAND mdkr_save_v170_fixture_test
                ${CMAKE_SOURCE_DIR}/tests/fixtures/save_v170)

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

    # The per-(level, vehicle) ghost bank behind issue #46. The test's fake
    # pak stubs carry the real save_data.h signatures, so it compiles against
    # the game headers the way the camera kernel tests do.
    add_executable(mdkr_ghost_bank_test
        ${CMAKE_SOURCE_DIR}/tests/test_ghost_bank.c
        ${CMAKE_SOURCE_DIR}/platform/ghost_bank.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/platform/sha256.c)
    target_include_directories(mdkr_ghost_bank_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/game
        ${CMAKE_SOURCE_DIR}/game/src
        ${CMAKE_SOURCE_DIR}/game/include
        ${CMAKE_SOURCE_DIR}/game/libultra
        ${CMAKE_SOURCE_DIR}/game/libultra/src/audio)
    mdkr_add_legacy_game_quote_includes(mdkr_ghost_bank_test)
    target_compile_definitions(mdkr_ghost_bank_test PRIVATE
        VERSION_us_v80
        _LANGUAGE_C
        MODERN_CC
        NON_MATCHING=1
        AVOID_UB=1
        NATIVE_PORT=1
        F3DDKR_GBI
        _FINALROM)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(mdkr_ghost_bank_test PRIVATE
            -fms-extensions
            -Wno-c23-extensions)
    endif()
    add_test(NAME ghost_bank COMMAND mdkr_ghost_bank_test)

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

    add_executable(mdkr_void_render_policy_test
        ${CMAKE_SOURCE_DIR}/tests/test_void_render_policy.c)
    target_include_directories(mdkr_void_render_policy_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(MSVC)
        target_compile_options(mdkr_void_render_policy_test PRIVATE /W4 /WX)
    else()
        target_compile_options(mdkr_void_render_policy_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME void_render_policy COMMAND mdkr_void_render_policy_test)

    # Void-curtain pair walker (issue #53 fallout): links the PRODUCTION
    # game/src/tracks.c so the s8-narrowing and saturation-orphan defects are
    # exercised against the shipped walker, not a test-local copy. tracks.c
    # references the whole engine; the dead-strip/--gc-sections link is what
    # confines this ROM-free target to the void subsystem's closure (three
    # renderer symbols are stubbed in the test). MSVC has no equivalent
    # linker contract, and no MSVC lane builds these tests today.
    if(NOT MSVC)
        add_executable(mdkr_void_pairs_test
            ${CMAKE_SOURCE_DIR}/tests/test_void_pairs.c
            ${CMAKE_SOURCE_DIR}/game/src/tracks.c)
        target_include_directories(mdkr_void_pairs_test PRIVATE
            ${CMAKE_SOURCE_DIR}/game
            ${CMAKE_SOURCE_DIR}/game/src
            ${CMAKE_SOURCE_DIR}/game/include
            ${CMAKE_SOURCE_DIR}/game/include/PR
            ${CMAKE_SOURCE_DIR}/game/include/sys
            ${CMAKE_SOURCE_DIR}/game/libultra
            ${CMAKE_SOURCE_DIR}/game/libultra/src/audio
            ${CMAKE_SOURCE_DIR}/platform
            ${CMAKE_SOURCE_DIR}/platform/fast3d
            ${CMAKE_SOURCE_DIR}/platform/fast3d_shim)
        target_compile_definitions(mdkr_void_pairs_test PRIVATE
            VERSION_us_v80
            _LANGUAGE_C
            MODERN_CC
            NON_MATCHING=1
            AVOID_UB=1
            NATIVE_PORT=1
            F3DDKR_GBI
            _FINALROM)
        # tracks.c is decomp engine code: give it the same relaxed parse and
        # tentative-definition flags the app target compiles it with.
        target_compile_options(mdkr_void_pairs_test PRIVATE
            -fno-strict-aliasing
            -fcommon)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            target_compile_options(mdkr_void_pairs_test PRIVATE
                -Wno-everything
                -fms-extensions
                -Wno-c23-extensions)
        endif()
        if(APPLE)
            target_link_options(mdkr_void_pairs_test PRIVATE -Wl,-dead_strip)
        else()
            target_compile_options(mdkr_void_pairs_test PRIVATE
                -ffunction-sections
                -fdata-sections)
            target_link_options(mdkr_void_pairs_test PRIVATE -Wl,--gc-sections)
        endif()
        if(MINGW)
            # PE/COFF section GC alone does not isolate this production TU:
            # MinGW still diagnoses its unrelated engine references. Remove
            # the unused code before the final link, retaining the actual
            # walker and the test's assertions rather than stubbing the engine.
            include(CheckIPOSupported)
            check_ipo_supported(RESULT MDKR_VOID_PAIRS_IPO_SUPPORTED
                OUTPUT MDKR_VOID_PAIRS_IPO_ERROR LANGUAGES C)
            if(NOT MDKR_VOID_PAIRS_IPO_SUPPORTED)
                message(FATAL_ERROR
                    "MinGW void_pairs requires IPO to isolate tracks.c: "
                    "${MDKR_VOID_PAIRS_IPO_ERROR}")
            endif()
            set_property(TARGET mdkr_void_pairs_test PROPERTY
                INTERPROCEDURAL_OPTIMIZATION TRUE)
            if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
                # CMake's MinGW executable link stages objects with plain ar,
                # which also needs native symbols when no LTO plugin is loaded.
                target_compile_options(mdkr_void_pairs_test PRIVATE -ffat-lto-objects)
                # Keep link-time compiler work bounded as well as the build.
                target_link_options(mdkr_void_pairs_test PRIVATE -flto=1)
            endif()
        endif()
        target_link_libraries(mdkr_void_pairs_test PRIVATE m)
        add_test(NAME void_pairs COMMAND mdkr_void_pairs_test)
    endif()

    # ROM-free units over PRODUCTION engine translation units, isolated the
    # same way as void_pairs above: the dead-strip/--gc-sections link keeps
    # only the closure of what the test calls (the test stubs the few engine
    # symbols left on that path), and MinGW additionally needs IPO. MSVC has
    # no equivalent linker contract and no MSVC lane builds these tests.
    function(mdkr_add_engine_unit_test target test_name)
        add_executable(${target} ${ARGN})
        target_include_directories(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/game
            ${CMAKE_SOURCE_DIR}/game/src
            ${CMAKE_SOURCE_DIR}/game/include
            ${CMAKE_SOURCE_DIR}/game/include/PR
            ${CMAKE_SOURCE_DIR}/game/include/sys
            ${CMAKE_SOURCE_DIR}/game/libultra
            ${CMAKE_SOURCE_DIR}/game/libultra/src/audio
            ${CMAKE_SOURCE_DIR}/platform
            ${CMAKE_SOURCE_DIR}/platform/fast3d
            ${CMAKE_SOURCE_DIR}/platform/fast3d_shim)
        target_compile_definitions(${target} PRIVATE
            VERSION_us_v80
            _LANGUAGE_C
            MODERN_CC
            NON_MATCHING=1
            AVOID_UB=1
            NATIVE_PORT=1
            F3DDKR_GBI
            _FINALROM)
        target_compile_options(${target} PRIVATE -fno-strict-aliasing -fcommon)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE
                -Wno-everything -fms-extensions -Wno-c23-extensions)
        endif()
        if(APPLE)
            target_link_options(${target} PRIVATE -Wl,-dead_strip)
        else()
            target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
            target_link_options(${target} PRIVATE -Wl,--gc-sections)
        endif()
        if(MINGW)
            include(CheckIPOSupported)
            check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES C)
            if(NOT ipo_supported)
                message(FATAL_ERROR "MinGW ${test_name} requires IPO: ${ipo_error}")
            endif()
            set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
                target_compile_options(${target} PRIVATE -ffat-lto-objects)
                target_link_options(${target} PRIVATE -flto=1)
            endif()
        endif()
        target_link_libraries(${target} PRIVATE m)
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()

    if(NOT MSVC)
        # Challenge-arena route search: native func_8001CD28 against the
        # original algorithm, and the AI-node rebuild guards by behaviour.
        mdkr_add_engine_unit_test(mdkr_ainode_route_test ainode_route
            ${CMAKE_SOURCE_DIR}/tests/test_ainode_route.c
            ${CMAKE_SOURCE_DIR}/game/src/objects.c
            ${CMAKE_SOURCE_DIR}/game/src/runtime_contracts.c)
        # 1.8 guards in weather_reset and clear_lap_records, by behaviour.
        mdkr_add_engine_unit_test(mdkr_engine_guards_test engine_guards
            ${CMAKE_SOURCE_DIR}/tests/test_engine_guards.c
            ${CMAKE_SOURCE_DIR}/game/src/weather.c
            ${CMAKE_SOURCE_DIR}/game/src/thread3_main.c
            ${CMAKE_SOURCE_DIR}/game/src/runtime_contracts.c)
        # Time Trial record saves: identical bytes whatever the memory pool
        # held, 1.7.0 residue still loads, codecs use the retail inventory.
        mdkr_add_engine_unit_test(mdkr_save_records_test save_records
            ${CMAKE_SOURCE_DIR}/tests/test_save_records.c
            ${CMAKE_SOURCE_DIR}/game/src/save_data.c
            ${CMAKE_SOURCE_DIR}/game/src/runtime_contracts.c
            ${CMAKE_SOURCE_DIR}/platform/save_codec.c)
        # Synth voice stealing and the shared update pool (issue #66): a
        # stolen voice freed while the pool is dry must not leak.
        mdkr_add_engine_unit_test(mdkr_audio_synth_voice_test audio_synth_voice
            ${CMAKE_SOURCE_DIR}/tests/test_audio_synth_voice.c
            ${CMAKE_SOURCE_DIR}/platform/audio_synth.c
            ${CMAKE_SOURCE_DIR}/platform/audio_event_queue.c)
        # Pulsing-light uploads decoded from the display list the production
        # material_set_blinking_lights emits: legacy layouts byte-identical,
        # every load inside the texture, every sample the N64's byte (wrapped).
        mdkr_add_engine_unit_test(mdkr_pulse_material_test pulse_material
            ${CMAKE_SOURCE_DIR}/tests/test_pulse_material.c
            ${CMAKE_SOURCE_DIR}/game/src/textures_sprites.c
            ${CMAKE_SOURCE_DIR}/game/src/runtime_contracts.c)
        # A replacement sprite model's material, decoded from what production
        # material_load_simple emits for the billboard flags both paths share:
        # fading writes no depth, the light tint follows the ENV cycle.
        # The presentation replay of a sprite-model recipe: interpolated pose
        # and shortest-arc phase, one tick-agreement check per recipe.
        mdkr_add_engine_unit_test(mdkr_sprite_model_replay_test sprite_model_replay
            ${CMAKE_SOURCE_DIR}/tests/test_sprite_model_replay.c
            ${CMAKE_SOURCE_DIR}/game/src/camera.c
            ${CMAKE_SOURCE_DIR}/game/src/hasm/math_util.c
            ${CMAKE_SOURCE_DIR}/platform/math_util_native.c
            ${CMAKE_SOURCE_DIR}/platform/presentation_snapshot.c
            ${CMAKE_SOURCE_DIR}/platform/fast3d/gfx_presentation_packet.c
            ${CMAKE_SOURCE_DIR}/platform/sprite_model_mesh.c)
        mdkr_add_engine_unit_test(mdkr_sprite_model_material_test sprite_model_material
            ${CMAKE_SOURCE_DIR}/tests/test_sprite_model_material.c
            ${CMAKE_SOURCE_DIR}/game/src/textures_sprites.c
            ${CMAKE_SOURCE_DIR}/game/src/runtime_contracts.c)
    endif()

    # PAL video-mode height-raise idempotency (Return-to-Launcher -> Play
    # vertical menu shift): the native persistent launcher calls video_init()
    # once per in-process engine epoch, so the PAL height raise must not
    # compound the persistent global table. Links the production helper; it is
    # pure C (no engine closure), so no stubbing or dead-strip is needed.
    if(NOT MSVC)
        add_executable(mdkr_video_mode_table_test
            ${CMAKE_SOURCE_DIR}/tests/test_video_mode_table.c
            ${CMAKE_SOURCE_DIR}/game/src/video_mode_table.c)
        target_include_directories(mdkr_video_mode_table_test PRIVATE
            ${CMAKE_SOURCE_DIR}/game
            ${CMAKE_SOURCE_DIR}/game/src
            ${CMAKE_SOURCE_DIR}/game/include
            ${CMAKE_SOURCE_DIR}/game/include/PR
            ${CMAKE_SOURCE_DIR}/game/include/sys
            ${CMAKE_SOURCE_DIR}/game/libultra
            ${CMAKE_SOURCE_DIR}/game/libultra/src/audio
            ${CMAKE_SOURCE_DIR}/platform
            ${CMAKE_SOURCE_DIR}/platform/fast3d
            ${CMAKE_SOURCE_DIR}/platform/fast3d_shim)
        target_compile_definitions(mdkr_video_mode_table_test PRIVATE
            VERSION_us_v80
            _LANGUAGE_C
            MODERN_CC
            NON_MATCHING=1
            AVOID_UB=1
            NATIVE_PORT=1
            F3DDKR_GBI
            _FINALROM)
        target_compile_options(mdkr_video_mode_table_test PRIVATE
            -fno-strict-aliasing
            -fcommon)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            target_compile_options(mdkr_video_mode_table_test PRIVATE
                -Wno-everything
                -fms-extensions
                -Wno-c23-extensions)
        endif()
        add_test(NAME video_mode_table COMMAND mdkr_video_mode_table_test)
    endif()
endif()

# Repository-publication policy is backend-independent and must run in every
# native CTest configuration, including OpenGL-only and sanitizer lanes.
if(BUILD_TESTING)
    if(MDKR_CHARACTER_KTX2)
        add_executable(mdkr_modern_character_ktx2_test
            ${CMAKE_SOURCE_DIR}/tests/test_modern_character_ktx2.cpp)
        target_link_libraries(mdkr_modern_character_ktx2_test PRIVATE
            mdkr_character_ktx2_bridge mdkr_basisu_transcoder)
        target_include_directories(mdkr_modern_character_ktx2_test SYSTEM PRIVATE
            "${_mdkr_basisu_root}/external/basisu/zstd")
        target_compile_options(mdkr_modern_character_ktx2_test PRIVATE
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Werror>)
        add_test(NAME modern_character_ktx2
            COMMAND mdkr_modern_character_ktx2_test)
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(MDKR_NODE_EXECUTABLE NAMES node nodejs)
    add_test(
        NAME public_surface
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_public_surface.py)
    add_test(
        NAME multiplayer_boundaries
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_multiplayer_boundaries.py)
    # AP-05 trace schema, Python half of the cross-language golden contract:
    # parses tests/data/adventure_party_trace_golden.txt (the same file the C
    # unit test compares against) and asserts field round-trip + schema. Runs
    # with --self-test so its own parser controls fire in CTest.
    add_test(
        NAME adventure_party_trace_schema
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/adventure_party_trace.py --self-test)
    # Source-shaped: the D1 deferral is only safe if every menu transition
    # that closes the racer-bindings window also services it; those hooks
    # live in call sites the taj_mod unit binary cannot reach.
    add_test(
        NAME taj_service_points
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_taj_service_points.py)
    # The shipped controller-mapping database: every line parses, no
    # (GUID, platform) pair appears twice, and the curated NSO N64 HIDAPI
    # entries (issue #55) stay present and behaviorally equivalent to the
    # upstream DirectInput entry across refreshes of the upstream snapshot.
    add_test(
        NAME gamecontrollerdb_lint
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_gamecontrollerdb.py)
    # This is deliberately the source-only arm.  The full check owns real
    # Chromium activation evidence and therefore remains in run_checks.py's
    # serialized browser lane; ordinary CTest must never launch a browser.
    add_test(
        NAME online_compatibility_source_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_browser_online_activation.py
                --source-only)
    if(MDKR_NODE_EXECUTABLE)
        add_test(
            NAME browser_startup_diagnostics_js
            COMMAND ${MDKR_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/test_browser_startup_diagnostics.mjs)
        set_tests_properties(browser_startup_diagnostics_js PROPERTIES TIMEOUT 30)
        add_test(
            NAME party_protocol_js
            COMMAND ${MDKR_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/test_party_protocol_js.cjs)
        add_test(
            NAME touch_surface_js
            COMMAND ${MDKR_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/web/touch-surface.test.cjs)
        add_test(
            NAME controller_embedded_ua_js
            COMMAND ${MDKR_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/web/controller-embedded-ua.test.cjs)
        add_test(
            NAME match_peer_crypto_js
            COMMAND ${MDKR_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/test_match_peer_crypto_js.mjs)
        add_test(
            NAME match_preflight_js
            COMMAND ${MDKR_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/test_match_preflight_js.mjs)
        add_test(
            NAME match_signal_client_js
            COMMAND ${MDKR_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/test_match_signal_client_js.mjs)
        # The presenter consumes JSON from the native browser-ABI harness.
        # Emscripten builds intentionally do not define that host executable;
        # do not leave a generator expression naming an absent target in their
        # CTest inventory.
        if(TARGET mdkr_online_lobby_browser_wasm_test)
            add_test(
                NAME online_room_presenter_js
                COMMAND ${MDKR_NODE_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/tests/test_online_room_presenter.mjs
                        $<TARGET_FILE:mdkr_online_lobby_browser_wasm_test>)
        endif()
        add_test(
            NAME online_room_live_state_js
            COMMAND ${MDKR_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/test_online_room_live_state.mjs)
        # The Online Room page harness (S1 fallback-only /state refresh, S4
        # typed 4000-class terminal closes) -- the page-level behavioral
        # coverage over the real presenter + live-state modules. Registered
        # beside its two siblings above (W3 fix round: it previously ran
        # nowhere).
        add_test(
            NAME online_room_client_js
            COMMAND ${MDKR_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/test_online_room_client.mjs)
    endif()
    add_test(
        NAME run_checks_sdl_flavor
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_run_checks_sdl_flavor.py)
    add_test(
        NAME oracle_reference_replay
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_oracle_reference_replay.py)
    add_test(
        NAME resource_plateau_oracle
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_resource_plateau_oracle.py)
    add_test(
        NAME oracle_route_classification
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_oracle_route_classification.py)
    add_test(
        NAME oracle_audio_routes
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_oracle_audio_routes.py)
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
        NAME crossarch_determinism
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_crossarch_determinism.py)
    add_test(
        NAME previous_release_parity
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_previous_release_parity.py)
    add_test(
        NAME check_save_dir_hermeticity
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_check_save_dir_hermeticity.py)
    add_test(
        NAME app_scroll_environment
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_app_scroll_environment.py)
    add_test(
        NAME clean_room_guard
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_clean_room_guard.py)
    add_test(
        NAME source_custody
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_source_custody.py)
    add_test(
        NAME check_adventure_party_performance_plateau
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_check_adventure_party_performance_plateau.py)
    add_test(
        NAME texture_cache_identity
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_texture_cache_identity.py)
    add_test(
        NAME ricepack_import
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_ricepack_import.py)
    add_test(
        NAME rice_crc
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_rice_crc.py)
    add_test(
        NAME camera_obstruction_authority
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_camera_obstruction_authority.py)
    add_test(
        NAME water_effect_tick_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_water_effect_tick_contract.py)
    add_test(
        NAME durable_rollback_firewall
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_durable_rollback_firewall.py)
    add_test(
        NAME rumble_rollback_adapter
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_rumble_rollback_adapter.py)
    add_test(
        NAME audio_rollback_adapter
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_audio_rollback_adapter.py)
    add_test(
        NAME ai_takeover_adapter
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_ai_takeover_adapter.py)
    add_test(
        NAME postrace_rollback_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_postrace_rollback_contract.py)
    add_test(
        NAME network_viewport_invariance
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_network_viewport_invariance.py)
    add_test(
        NAME match_transport_tls_io
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_match_transport_tls_io.py)
    set_tests_properties(match_transport_tls_io PROPERTIES TIMEOUT 30)
    add_test(
        NAME online_lobby_takeover_source
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_online_lobby_takeover.py --source-only)
    set_tests_properties(online_lobby_takeover_source PROPERTIES TIMEOUT 30)
    add_test(
        NAME online_resolver_budget
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_online_resolver_budget.py)
    set_tests_properties(online_resolver_budget PROPERTIES TIMEOUT 30)
    add_test(NAME party_open_transaction
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_party_open_transaction.py)
    set_tests_properties(party_open_transaction PROPERTIES TIMEOUT 30)
    add_test(NAME network_lifetime_source
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_network_lifetime.py)
    set_tests_properties(network_lifetime_source PROPERTIES TIMEOUT 30)
    add_test(NAME match_signal_admission_source
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_match_signal_admission.py)
    set_tests_properties(match_signal_admission_source PROPERTIES TIMEOUT 30)

    add_test(
        NAME run_checks_artifact_routing
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_run_checks_artifact_routing.py)
    set_tests_properties(run_checks_artifact_routing PROPERTIES TIMEOUT 30)
    add_test(
        NAME browser_startup_diagnostics
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_browser_startup_diagnostics.py)
    set_tests_properties(browser_startup_diagnostics PROPERTIES TIMEOUT 30)
    add_test(
        NAME party_worker_startup_reporting
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_party_worker_startup.py)
    set_tests_properties(party_worker_startup_reporting PROPERTIES TIMEOUT 30)
    add_test(
        NAME party_worker_reporting
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_party_worker_reporting.py)
    set_tests_properties(party_worker_reporting PROPERTIES TIMEOUT 30)
    add_test(
        NAME native_party_notice_pins
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_native_party_notice_pins.py)
    set_tests_properties(native_party_notice_pins PROPERTIES TIMEOUT 30)
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
        NAME character_offset_studio_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_character_offset_studio.py)
    add_test(
        NAME web_document_structure
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_web_document_structure.py)
    add_test(
        NAME product_claim_boundaries
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_product_claim_boundaries.py)
    add_test(
        NAME party_usage_reconciliation
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_party_usage_reconciliation.py)
    add_test(
        NAME party_beta_ledger
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_party_beta_ledger.py)
    add_test(
        NAME party_experience_canary
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_party_experience_canary.py)
    # Phone Party service/deploy static gates. These were runnable by hand but
    # unwired, which is how check_party_internal_api.py sat red from the commit
    # that introduced it. Registering them makes a regression a suite failure.
    add_test(
        NAME party_internal_api
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_party_internal_api.py)
    add_test(
        NAME party_edge_policy
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_party_edge_policy.py)
    add_test(
        NAME party_production_config
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_party_production_config.py)
    add_test(
        NAME party_cmake_origin_gate
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_party_cmake_origin_gate.py)
    add_test(
        NAME party_binary_surface
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_party_binary_surface.py
                --self-test)
    add_test(
        NAME release_party_origin
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_release_party_origin.py
                --self-test)
    # Phase 3 local-only play: the trimmed controller asset set must agree
    # across the C++ manifest builder and the packaging source-of-truth list,
    # and every listed file must exist in dist/web -- so a packaged build stages
    # exactly what the phone loads and the page never ships broken.
    add_test(
        NAME party_lan_controller_assets
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_lan_controller_assets.py)
    add_test(
        NAME audio_sink_evidence_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_audio_sink_evidence_contract.py)
    add_test(
        NAME pacing_quality_reporting
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_pacing_quality_reporting.py)
    add_test(
        NAME camera_motion_reporting
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_camera_motion_reporting.py)
    add_test(
        NAME widescreen_shadow_reporting
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_widescreen_shadow_reporting.py)
    add_test(
        NAME ai_difficulty_ui
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_ai_difficulty_ui.py)
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

    # Synthetic replacement geometry and placement; no ROM, pack or GPU needed.
    add_executable(mdkr_sprite_model_mesh_test
        ${CMAKE_SOURCE_DIR}/tests/test_sprite_model_mesh.c
        ${CMAKE_SOURCE_DIR}/platform/sprite_model_mesh.c)
    target_include_directories(mdkr_sprite_model_mesh_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_sprite_model_mesh_test PRIVATE m)
    endif()
    add_test(NAME sprite_model_mesh COMMAND mdkr_sprite_model_mesh_test)

    add_executable(mdkr_sprite_models_test
        ${CMAKE_SOURCE_DIR}/tests/test_sprite_models.c
        ${CMAKE_SOURCE_DIR}/platform/sprite_models.c
        ${CMAKE_SOURCE_DIR}/platform/sprite_model_mesh.c
        ${CMAKE_SOURCE_DIR}/platform/mod_registry.c
        ${CMAKE_SOURCE_DIR}/platform/mod_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c
        ${CMAKE_SOURCE_DIR}/platform/fs_utf8.c
        ${CMAKE_SOURCE_DIR}/platform/mod_source.c
        ${CMAKE_SOURCE_DIR}/lib/stb/stb_image_impl.c
        ${CMAKE_SOURCE_DIR}/lib/miniz/miniz.c)
    target_include_directories(mdkr_sprite_models_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform ${CMAKE_SOURCE_DIR}/lib/stb
        ${CMAKE_SOURCE_DIR}/lib/miniz)
    if(NOT MSVC)
        target_link_libraries(mdkr_sprite_models_test PRIVATE m)
    endif()
    add_test(NAME sprite_models COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/check_sprite_models.py
        $<TARGET_FILE:mdkr_sprite_models_test>)

    # Compile the actual launcher sampler against a deterministic device seam;
    # the fixture supplies SDL boundary functions and links no SDL runtime.
    add_executable(mdkr_app_launch_hold_test
        ${CMAKE_SOURCE_DIR}/tests/test_app_launch_hold.cpp
        ${CMAKE_SOURCE_DIR}/platform/app/app_launch_hold.cpp)
    target_include_directories(mdkr_app_launch_hold_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/app ${SDL2_INCLUDE_DIRS})
    target_compile_features(mdkr_app_launch_hold_test PRIVATE cxx_std_17)
    target_compile_definitions(mdkr_app_launch_hold_test PRIVATE SDL_MAIN_HANDLED)
    add_test(NAME app_launch_hold COMMAND mdkr_app_launch_hold_test)
    set_tests_properties(app_launch_hold PROPERTIES TIMEOUT 30)
endif()
