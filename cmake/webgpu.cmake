# webgpu.cmake — pinned wgpu-native (WebGPU) prebuilt integration for mdkr64.
#
# Mirrors mgb64/cmake/webgpu.cmake (the SOURCE this backend was vendored from):
# consume wgpu-native's per-platform PREBUILT release (no Rust toolchain needed),
# pinned to an exact version and SHA-256-verified. wgpu-native is the gfx-rs C
# implementation of the standard webgpu.h C API; it dispatches to Metal on macOS,
# D3D12 on Windows, Vulkan on Linux. The webgpu.h API is identical to Dawn /
# emdawnwebgpu, so the SAME gfx_webgpu.c compiles for native and (M8) wasm.
#
# Two deltas from mgb64's copy, both offline/robustness only (same version + SHA):
#   1. If mgb64 already fetched this exact prebuilt on this machine, reuse its
#      extracted tree directly (no re-download) — faithful, since the SHA-pinned
#      artifact is byte-identical.
#   2. Otherwise, if the pinned .zip is present in a local download cache, fetch
#      from file:// (still SHA-verified); else download from GitHub as mgb64 does.
#
# Exposes: INTERFACE target `webgpu` (include dir + static lib + system deps).

# --- Emscripten: browser WebGPU via the emdawnwebgpu port; no native prebuilt ---
# (M8) — kept for parity with mgb64 so the wasm bring-up needs no edit here.
if(EMSCRIPTEN)
    add_library(webgpu INTERFACE)
    target_compile_options(webgpu INTERFACE "--use-port=emdawnwebgpu")
    target_link_options(webgpu INTERFACE "--use-port=emdawnwebgpu")
    return()
endif()

if(NOT MDKR_WEBGPU_BACKEND)
    return()
endif()

set(WGPU_NATIVE_VERSION "v29.0.1.1")
set(_wgpu_base "https://github.com/gfx-rs/wgpu-native/releases/download/${WGPU_NATIVE_VERSION}")

# Select only a published, pinned OS/CPU/compiler-ABI tuple. In particular,
# Windows' archive is GNU/MinGW: feeding it to MSVC is not supported.
include("${CMAKE_CURRENT_LIST_DIR}/webgpu_artifact.cmake")
if(APPLE)
    set(_wgpu_os Darwin)
    set(_wgpu_abi apple)
elseif(WIN32)
    set(_wgpu_os Windows)
    if(MSVC OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(_wgpu_abi msvc)
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        set(_wgpu_abi gnu)
    else()
        set(_wgpu_abi "${CMAKE_C_COMPILER_ID}")
    endif()
elseif(UNIX AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_wgpu_os Linux)
    set(_wgpu_abi gnu)
else()
    set(_wgpu_os "${CMAKE_SYSTEM_NAME}")
    set(_wgpu_abi "${CMAKE_C_COMPILER_ID}")
endif()
mdkr_select_wgpu_artifact(
    "${_wgpu_os}" "${CMAKE_SYSTEM_PROCESSOR}" "${_wgpu_abi}"
    _wgpu_supported _wgpu_asset _wgpu_sha _wgpu_reason)
if(NOT _wgpu_supported)
    message(FATAL_ERROR
        "MDKR_WEBGPU_BACKEND: ${_wgpu_reason}. "
        "Configure -DMDKR_WEBGPU_BACKEND=OFF for the portable OpenGL build.")
endif()

# Prefer a local cached archive if present; FetchContent still verifies the
# selected tuple's SHA before extraction. Do not reuse arbitrary extracted
# sibling directories: their architecture/version/ABI cannot be authenticated.
set(_wgpu_url "${_wgpu_base}/${_wgpu_asset}")
foreach(_zipcand
        "$ENV{MDKR_WGPU_LOCAL_CACHE}/${_wgpu_asset}")
    if(EXISTS "${_zipcand}")
        set(_wgpu_url "file://${_zipcand}")
        break()
    endif()
endforeach()
include(FetchContent)
# DOWNLOAD_EXTRACT_TIMESTAMP is a CMake 3.24 keyword. On older CMake
# (Ubuntu 22.04's apt ships 3.22 — the release lane's own distro) an unknown
# keyword is not an error to FetchContent_Declare itself; it is swallowed
# into the argument list and silently corrupts URL_HASH, failing configure
# with a misleading hash error. Guard by version: pre-3.24 behaves as if the
# keyword were TRUE anyway (old timestamp behavior), so the guarded form is
# semantically identical everywhere.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    set(_wgpu_ts_arg DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
else()
    set(_wgpu_ts_arg "")
endif()
FetchContent_Declare(wgpu_native
    URL "${_wgpu_url}"
    URL_HASH SHA256=${_wgpu_sha}
    ${_wgpu_ts_arg})
FetchContent_MakeAvailable(wgpu_native)
message(STATUS "WebGPU backend: fetched wgpu-native ${WGPU_NATIVE_VERSION} from ${_wgpu_url}")
# wgpu_native_SOURCE_DIR now contains include/webgpu/{webgpu,wgpu}.h + lib/.

add_library(webgpu INTERFACE)
target_include_directories(webgpu INTERFACE "${wgpu_native_SOURCE_DIR}/include")

if(WIN32)
    target_link_libraries(webgpu INTERFACE
        "${wgpu_native_SOURCE_DIR}/lib/libwgpu_native.a"
        d3dcompiler ws2_32 userenv bcrypt ntdll opengl32 ole32 oleaut32 propsys runtimeobject)
else()
    target_link_libraries(webgpu INTERFACE "${wgpu_native_SOURCE_DIR}/lib/libwgpu_native.a")
endif()

if(APPLE)
    # System frameworks the wgpu-native static lib references (Metal backend + the
    # Rust std runtime).
    target_link_libraries(webgpu INTERFACE
        "-framework Metal" "-framework QuartzCore" "-framework Foundation"
        "-framework CoreFoundation" "-framework IOKit" "-framework IOSurface"
        "-framework CoreGraphics" "-framework AppKit" "-framework Security")
elseif(UNIX)
    target_link_libraries(webgpu INTERFACE m dl pthread)
endif()

message(STATUS "WebGPU backend: wgpu-native ${WGPU_NATIVE_VERSION} (${_wgpu_asset})")
