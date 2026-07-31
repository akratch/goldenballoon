# CMake toolchain: cross-compile the Windows (x86_64) mdkr64.exe from macOS/Linux
# using the free mingw-w64 toolchain (brew install mingw-w64).
#
# Ported from mgb64's cmake/mingw-w64-x86_64.cmake. This lane exists so "the
# Windows build compiles and links" is a locally checkable fact instead of a
# push-and-pray CI event (backlog PK-4/PK-5). It targets the same MinGW/MSYS2
# configuration the windows-latest CI job will use (mingw-w64-x86_64-gcc +
# mingw-w64-x86_64-SDL2) so the WIN32 branch in CMakeLists.txt — notably
# -mno-ms-bitfields and the GUI-subsystem link — is exercised.
#
# Drive it through tools/mingw_cross_check.sh, which supplies the vendored SDL2
# prefix. To use directly:
#   cmake -B build-mingw \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#     -DCMAKE_PREFIX_PATH=$PWD/build-mingw-deps/prefix/mingw64

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_MINGW_TARGET x86_64-w64-mingw32)

# Honour an explicit override, else resolve the brew-installed tools by name.
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER ${_MINGW_TARGET}-gcc)
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER ${_MINGW_TARGET}-g++)
endif()
if(NOT CMAKE_RC_COMPILER)
    set(CMAKE_RC_COMPILER ${_MINGW_TARGET}-windres)
endif()

# Derive the toolchain sysroot from the compiler itself rather than hardcoding
# the Homebrew Cellar version, so a toolchain upgrade doesn't silently break the
# find-root. System import libs (opengl32, ole32, shell32, uuid, winpthread)
# live under ${sysroot}/${target}.
find_program(_MINGW_CC ${CMAKE_C_COMPILER})
if(_MINGW_CC)
    execute_process(
        COMMAND ${_MINGW_CC} -print-sysroot
        OUTPUT_VARIABLE _MINGW_SYSROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

set(CMAKE_FIND_ROOT_PATH "")
if(_MINGW_SYSROOT AND EXISTS "${_MINGW_SYSROOT}/${_MINGW_TARGET}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${_MINGW_SYSROOT}/${_MINGW_TARGET}")
endif()
# The vendored SDL2 prefix is passed in via CMAKE_PREFIX_PATH by the driver
# script; mirror it into the find-root so config-mode find_package(SDL2) and
# the OpenGL system libs resolve under FIND_ROOT_PATH_MODE_*=ONLY.
if(CMAKE_PREFIX_PATH)
    list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH})
endif()

# Host tools (python3, the build generator) come from the host, not the target.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
