#!/usr/bin/env bash
#
# Local MinGW cross-compile lane (backlog PK-4/PK-5): prove the Windows build
# compiles and links from this macOS/Linux host, instead of discovering breaks
# only when the GitHub windows-latest job runs (QA-2).
#
# Ported from mgb64's tools/mingw_cross_check.sh — same mechanism, adapted to
# this tree:
#   toolchain  mingw-w64-x86_64-gcc   (brew install mingw-w64)
#   SDL2       mingw-w64-x86_64-SDL2  (vendored from repo.msys2.org, pinned below)
#   WebGPU     wgpu-native windows-x86_64-GNU prebuilt (cmake/webgpu.cmake
#              already pins it; the Windows tuple is in webgpu_artifact.cmake)
#   config     -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
#   target     mdkr64  (-> mdkr64.exe)
#
# The build exercises the WIN32 branch in CMakeLists.txt: -mno-ms-bitfields
# (bitfield layout — the decomp's structs are N64/GCC-packed, MinGW defaults to
# the MSVC packing), the GUI subsystem link, and the static GCC runtime.
#
# SDL2 is handed to the unmodified pkg_check_modules(SDL2 REQUIRED sdl2) finder
# by pointing pkg-config at the vendored MSYS2 prefix (PKG_CONFIG_LIBDIR +
# PKG_CONFIG_SYSROOT_DIR) rather than by adding a Windows branch to the finder.
#
# Idempotent and safe to re-run. Warnings are reported but do NOT fail the lane;
# only compile/link errors fail it. The warning count covers the TUs compiled by
# THIS invocation — pass --clean for a full-tree census.
#
# Usage:  tools/mingw_cross_check.sh [--clean] [--jobs N] [--no-webgpu]
set -euo pipefail

# --- Pinned SDL2 provenance ----------------------------------------------------
# We vendor the MSYS2 mingw64 SDL2 package and pin the version + the SHA256
# published in the official mingw64.db. Same pin as mgb64 so both ports are
# proven against one Windows SDL2.
SDL2_PKG="mingw-w64-x86_64-SDL2-2.32.10-1-any.pkg.tar.zst"
SDL2_URL="https://repo.msys2.org/mingw/mingw64/${SDL2_PKG}"
SDL2_SHA256="5991afbcfeb2f8b838ab80b2270d713a727199ca392715677a7c1931a0d9ecef"

MINGW_TARGET="x86_64-w64-mingw32"
JOBS=8
CLEAN=0
WEBGPU=ON
while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1 ;;
        --no-webgpu) WEBGPU=OFF ;;
        --jobs) JOBS="$2"; shift ;;
        --jobs=*) JOBS="${1#*=}" ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

DEPS_DIR="$REPO_ROOT/build-mingw-deps"
PREFIX="$DEPS_DIR/prefix/mingw64"
BUILD_DIR="$REPO_ROOT/build-mingw"
ARTIFACT="$BUILD_DIR/mdkr64.exe"

fail() { echo ""; echo "MINGW CROSS-CHECK: FAIL — $1"; exit 1; }

sha256_of() {
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}';
    elif command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}';
    else fail "no shasum/sha256sum available"; fi
}

# --- 1. Toolchain --------------------------------------------------------------
CC_BIN="$(command -v ${MINGW_TARGET}-gcc || true)"
[ -n "$CC_BIN" ] || fail "${MINGW_TARGET}-gcc not found on PATH (brew install mingw-w64)"
echo "toolchain: $($CC_BIN --version | head -1)  [$CC_BIN]"

# --- 2. Vendored SDL2 (fetch + verify + extract on first run) ------------------
if [ "$CLEAN" = "1" ]; then rm -rf "$BUILD_DIR" "$DEPS_DIR/prefix"; fi

if [ ! -f "$PREFIX/include/SDL2/SDL.h" ] || [ ! -f "$PREFIX/lib/libSDL2.dll.a" ]; then
    echo "vendoring SDL2 ($SDL2_PKG) ..."
    mkdir -p "$DEPS_DIR/pkgcache"
    PKG_PATH="$DEPS_DIR/pkgcache/$SDL2_PKG"
    if [ ! -f "$PKG_PATH" ]; then
        curl -fsSL -o "$PKG_PATH" "$SDL2_URL" || fail "SDL2 download failed: $SDL2_URL"
    fi
    GOT="$(sha256_of "$PKG_PATH")"
    if [ "$GOT" != "$SDL2_SHA256" ]; then
        rm -f "$PKG_PATH"
        fail "SDL2 checksum mismatch: got $GOT expected $SDL2_SHA256"
    fi
    echo "SDL2 checksum OK ($SDL2_SHA256)"
    command -v zstd >/dev/null 2>&1 || fail "zstd required to extract the .pkg.tar.zst (brew install zstd)"
    rm -rf "$DEPS_DIR/prefix"; mkdir -p "$DEPS_DIR/prefix"
    zstd -dc "$PKG_PATH" | tar -xf - -C "$DEPS_DIR/prefix" 2>/dev/null
    [ -f "$PREFIX/include/SDL2/SDL.h" ] || fail "SDL2 extraction did not yield include/SDL2/SDL.h"
fi
echo "SDL2 prefix: $PREFIX"

# The MSYS2 .pc files are written for an /mingw64 install root. Pointing
# pkg-config at the vendored copy (LIBDIR) and rebasing the prefix (SYSROOT_DIR)
# makes the tree's existing pkg_check_modules(SDL2 REQUIRED sdl2) resolve to the
# Windows SDL2 instead of the host's macOS/Linux one — no Windows branch is
# needed in the dependency finder itself.
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$DEPS_DIR/prefix"
[ -f "$PKG_CONFIG_LIBDIR/sdl2.pc" ] || fail "vendored SDL2 has no lib/pkgconfig/sdl2.pc"

# --- 3. Configure --------------------------------------------------------------
GEN="Unix Makefiles"   # ninja not required; generator does not affect codegen
echo "configuring ($GEN, Release, MDKR_WEBGPU_BACKEND=$WEBGPU) ..."
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G "$GEN" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/mingw-w64-x86_64.cmake" \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMDKR_WEBGPU_BACKEND=$WEBGPU \
    -DBUILD_TESTING=OFF \
    > "$BUILD_DIR-configure.log" 2>&1 || { cat "$BUILD_DIR-configure.log"; fail "cmake configure failed"; }

# --- 4. Build mdkr64.exe -------------------------------------------------------
echo "building mdkr64 (-j$JOBS) ..."
BUILD_LOG="$BUILD_DIR-build.log"
set +e
cmake --build "$BUILD_DIR" --target mdkr64 -j"$JOBS" > "$BUILD_LOG" 2>&1
RC=$?
set -e

WARN_COUNT="$(grep -c -iE 'warning:' "$BUILD_LOG" 2>/dev/null || true)"
if [ "$RC" != "0" ]; then
    echo "--- last 40 lines of build log ---"
    tail -40 "$BUILD_LOG"
    fail "compile/link error (see $BUILD_LOG)"
fi
[ -f "$ARTIFACT" ] || fail "build reported success but $ARTIFACT is missing"

# --- 5. Report -----------------------------------------------------------------
echo ""
echo "warnings: ${WARN_COUNT:-0} (non-fatal; full log: $BUILD_LOG)"
if command -v file >/dev/null 2>&1; then echo "artifact: $(file "$ARTIFACT")"; else echo "artifact: $ARTIFACT"; fi
echo ""

# The executable is intentionally single-file: SDL2 and the MinGW runtimes are
# static, while wgpu-native and SDL may import stock Windows DLLs.
OBJDUMP="${CROSS_PREFIX:-x86_64-w64-mingw32}-objdump" \
    tools/check_windows_imports.sh "$ARTIFACT"

echo "MINGW CROSS-CHECK: PASS — $ARTIFACT"
