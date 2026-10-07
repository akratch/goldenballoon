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
# --online extends the same plumbing to the online-enabled build. It configures
# with the three online flags (MDKR_ENABLE_ONLINE_BETA=ON, MDKR_NATIVE_PHONE_
# PARTY=ON, MDKR_PARTY_ORIGIN=https://party.goldenballoon.net) and BUILD_TESTING
# =ON in a SEPARATE build dir (build-mingw-online), pulls the pinned Mbed TLS /
# libdatachannel / wgpu-native deps, builds mdkr64.exe plus the 11 online/
# transport test exes, runs the import guard, asserts mdkr64.exe imports the
# Winsock/entropy DLLs (WS2_32/IPHLPAPI/bcrypt), and confirms each test exe is a
# PE32+ binary. The default (no --online) lane is unchanged, byte for byte, for
# existing callers. --deps-reuse-dir DIR (or MDKR_MINGW_DEPS_REUSE_DIR) points
# FetchContent at an already-fetched <name>-src tree (e.g. another build's
# _deps) to skip the libdatachannel clone; the tree is used read-only.
#
# MDKR_MINGW_PARTY_ORIGIN overrides the online profile origin; an explicitly
# empty value selects the canonical partyless release profile (LAN stays on).
#
# --intake-tests additionally builds the content-pack intake test executables
# (fs_utf8, mod_source_zip, pack_installer, pack_install_job) in a separate
# BUILD_TESTING=ON build dir (build-mingw-intake) and checks that each is a
# PE32+ binary importing only stock Windows DLLs and the UCRT. They cannot run
# here; each runs from any folder on Windows, works under %TEMP%, prints PASS
# or FAIL last and exits non-zero on failure.
#
# Usage:  tools/mingw_cross_check.sh [--clean] [--jobs N] [--no-webgpu]
#                                    [--online] [--deps-reuse-dir DIR]
#                                    [--intake-tests]
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
ONLINE=0
INTAKE=0
DEPS_REUSE_DIR="${MDKR_MINGW_DEPS_REUSE_DIR:-}"

usage() {
    echo "usage: tools/mingw_cross_check.sh [--clean] [--jobs N] [--no-webgpu]" >&2
    echo "                                  [--online] [--deps-reuse-dir DIR]" >&2
    echo "                                  [--intake-tests]" >&2
}
arg_error() {  # arg_error <message>: print message + usage to stderr, exit 2
    echo "mingw_cross_check: $1" >&2
    usage
    exit 2
}
# require_value <flag> <remaining-argc>: a separate-word value must follow <flag>.
# <remaining-argc> is $# at the point $1 is the flag, so it counts the flag itself
# -- a value exists only when it is >= 2. Guards the split-form flags (--jobs,
# --deps-reuse-dir) against dying with an opaque "$2: unbound variable" (set -u)
# when the flag is passed as the final argument.
require_value() {
    [ "$2" -ge 2 ] || arg_error "$1 requires a value"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1 ;;
        --no-webgpu) WEBGPU=OFF ;;
        --online) ONLINE=1 ;;
        --intake-tests) INTAKE=1 ;;
        --deps-reuse-dir) require_value "$1" "$#"; DEPS_REUSE_DIR="$2"; shift ;;
        --deps-reuse-dir=*) DEPS_REUSE_DIR="${1#*=}" ;;
        --jobs) require_value "$1" "$#"; JOBS="$2"; shift ;;
        --jobs=*) JOBS="${1#*=}" ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) arg_error "unknown argument: $1" ;;
    esac
    shift
done

# --deps-reuse-dir / MDKR_MINGW_DEPS_REUSE_DIR only feed the --online configure
# (they point FetchContent at an already-fetched deps tree). The default lane
# fetches nothing extra, so a value supplied without --online would be silently
# dropped -- warn instead so the caller knows it had no effect.
if [ -n "$DEPS_REUSE_DIR" ] && [ "$ONLINE" != "1" ]; then
    echo "mingw_cross_check: warning: --deps-reuse-dir/MDKR_MINGW_DEPS_REUSE_DIR" \
         "has no effect without --online; ignoring it for the default lane" >&2
    DEPS_REUSE_DIR=""
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

DEPS_DIR="$REPO_ROOT/build-mingw-deps"
PREFIX="$DEPS_DIR/prefix/mingw64"
# The online variant configures with different flags, so it gets its own build
# dir; the default lane's build-mingw is left exactly as existing callers know it.
if [ "$ONLINE" = "1" ]; then
    BUILD_DIR="$REPO_ROOT/build-mingw-online"
else
    BUILD_DIR="$REPO_ROOT/build-mingw"
fi
ARTIFACT="$BUILD_DIR/mdkr64.exe"

# The online/transport test executables the --online lane must produce.
# KEEP IN SYNC with the two CMake copies of these eleven names: the
# add_executable(mdkr_online_* / mdkr_native_* / mdkr_match_* / mdkr_lan_* ...)
# test definitions and the MinGW test-link dead-strip foreach (search
# "_mdkr_online_test") in CMakeLists.txt. That foreach hard-errors the online
# configure if a name here is renamed/removed on the CMake side without updating
# both; keep this list matched so the lane's build + PE32+ checks stay honest.
ONLINE_TEST_EXES="\
mdkr_online_track_table_test \
mdkr_native_party_sas_test \
mdkr_lan_party_transport_test \
mdkr_match_peer_crypto_test \
mdkr_match_signal_client_test \
mdkr_match_peer_transport_test \
mdkr_native_party_e2e_driver \
mdkr_online_live_adapter_test \
mdkr_online_live_adapter_beta_test \
mdkr_match_live_transport_test \
mdkr_online_live_transport_e2e_driver"

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
INTAKE_BUILD_DIR="$REPO_ROOT/build-mingw-intake"
INTAKE_TEST_EXES="mdkr_fs_utf8_test mdkr_mod_source_zip_test mdkr_pack_installer_test mdkr_pack_install_job_test"
if [ "$CLEAN" = "1" ]; then rm -rf "$BUILD_DIR" "$INTAKE_BUILD_DIR" "$DEPS_DIR/prefix"; fi

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
CONFIGURE_ARGS=(
    -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/mingw-w64-x86_64.cmake"
    -DCMAKE_PREFIX_PATH="$PREFIX"
    -DCMAKE_BUILD_TYPE=Release
    -DMDKR_WEBGPU_BACKEND=$WEBGPU
    # WebGPU is a platform-specific prebuilt, never a reusable source tree.
    -UFETCHCONTENT_SOURCE_DIR_WGPU_NATIVE
)
if [ "$ONLINE" = "1" ]; then
    CONFIGURE_ARGS+=(
        -DMDKR_ENABLE_ONLINE_BETA=ON
        -DMDKR_NATIVE_PHONE_PARTY=ON
        -DMDKR_ENABLE_ONLINE_ROOM_PREVIEW=OFF
        -DMDKR_CHARACTER_KTX2=ON
        "-DMDKR_PARTY_ORIGIN=${MDKR_MINGW_PARTY_ORIGIN-https://party.goldenballoon.net}"
        -DBUILD_TESTING=ON
    )
    # Optional: reuse an already-fetched deps tree (read-only) to skip the
    # libdatachannel clone + submodule checkout. Each override is added only if
    # the matching <name>-src dir is actually present.
    if [ -n "$DEPS_REUSE_DIR" ]; then
        for pair in \
            "MDKR_MBEDTLS:mdkr_mbedtls-src" \
            "MDKR_LIBDATACHANNEL:mdkr_libdatachannel-src"; do
            _name="${pair%%:*}"; _sub="${pair#*:}"
            if [ -d "$DEPS_REUSE_DIR/$_sub" ]; then
                CONFIGURE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_${_name}=$DEPS_REUSE_DIR/$_sub")
                echo "deps reuse: FETCHCONTENT_SOURCE_DIR_${_name} -> $DEPS_REUSE_DIR/$_sub"
            fi
        done
    fi
    echo "configuring ONLINE ($GEN, Release, beta ON, tests ON, MDKR_WEBGPU_BACKEND=$WEBGPU) ..."
else
    CONFIGURE_ARGS+=(-DBUILD_TESTING=OFF)
    echo "configuring ($GEN, Release, MDKR_WEBGPU_BACKEND=$WEBGPU) ..."
fi
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G "$GEN" \
    "${CONFIGURE_ARGS[@]}" \
    > "$BUILD_DIR-configure.log" 2>&1 || { cat "$BUILD_DIR-configure.log"; fail "cmake configure failed"; }

# --- 4. Build mdkr64.exe (+ online test exes under --online) -------------------
BUILD_TARGETS=(mdkr64)
if [ "$ONLINE" = "1" ]; then
    # shellcheck disable=SC2206  # deliberate word-split of the space-listed names
    BUILD_TARGETS+=($ONLINE_TEST_EXES)
    echo "building mdkr64 + $((${#BUILD_TARGETS[@]} - 1)) online test exes (-j$JOBS) ..."
else
    echo "building mdkr64 (-j$JOBS) ..."
fi
BUILD_LOG="$BUILD_DIR-build.log"
set +e
cmake --build "$BUILD_DIR" --target "${BUILD_TARGETS[@]}" -j"$JOBS" > "$BUILD_LOG" 2>&1
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
OBJDUMP_BIN="${CROSS_PREFIX:-x86_64-w64-mingw32}-objdump"
OBJDUMP="$OBJDUMP_BIN" \
    tools/check_windows_imports.sh "$ARTIFACT"

# --- 6. Online-variant acceptance ----------------------------------------------
if [ "$ONLINE" = "1" ]; then
    # The online build must reach the network stack: WS2_32 (Winsock), IPHLPAPI
    # (interface enumeration for ICE) and bcrypt (Mbed TLS entropy) are the
    # networking/entropy imports that prove the transport actually linked in.
    IMPORTS="$("$OBJDUMP_BIN" -p "$ARTIFACT" 2>/dev/null | awk '/DLL Name/{print $3}')"
    for dll in ws2_32 iphlpapi bcrypt; do
        printf '%s\n' "$IMPORTS" | grep -qiE "^${dll}\.dll$" \
            || fail "$ARTIFACT is missing the expected $dll import"
        echo "import OK: ${dll}.dll"
    done

    # Every online/transport test exe must be present as a native PE32+ binary.
    for t in $ONLINE_TEST_EXES; do
        exe="$BUILD_DIR/$t.exe"
        [ -f "$exe" ] || fail "expected test exe missing: $exe"
        if command -v file >/dev/null 2>&1; then
            file "$exe" | grep -qE 'PE32\+' \
                || fail "$exe is not a PE32+ binary"
        fi
        echo "test exe OK: $t.exe"
    done
    echo ""
    echo "online: mdkr64.exe + $(printf '%s\n' $ONLINE_TEST_EXES | wc -l | tr -d ' ') test exes built; imports + PE32+ verified"
fi

# --- 7. Content-pack intake test executables (--intake-tests) -----------------
if [ "$INTAKE" = "1" ]; then
    # Reuse the sources the lane above already fetched for this same target,
    # read-only, instead of cloning every dependency a second time.
    INTAKE_ARGS=()
    for src in "$BUILD_DIR"/_deps/*-src; do
        [ -d "$src" ] || continue
        name="$(basename "$src" -src | tr '[:lower:]' '[:upper:]')"
        INTAKE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_${name}=$src")
    done
    echo "configuring intake tests ($GEN, Release, BUILD_TESTING=ON, ${#INTAKE_ARGS[@]} fetched deps reused) ..."
    cmake -S "$REPO_ROOT" -B "$INTAKE_BUILD_DIR" -G "$GEN" \
        -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/mingw-w64-x86_64.cmake" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DMDKR_WEBGPU_BACKEND=$WEBGPU \
        -DBUILD_TESTING=ON \
        ${INTAKE_ARGS[@]+"${INTAKE_ARGS[@]}"} \
        > "$INTAKE_BUILD_DIR-configure.log" 2>&1 \
        || { cat "$INTAKE_BUILD_DIR-configure.log"; fail "intake-test configure failed"; }
    # shellcheck disable=SC2086  # deliberate word-split of the space-listed names
    cmake --build "$INTAKE_BUILD_DIR" --target $INTAKE_TEST_EXES -j"$JOBS" \
        > "$INTAKE_BUILD_DIR-build.log" 2>&1 \
        || { tail -40 "$INTAKE_BUILD_DIR-build.log"; fail "intake-test compile/link error (see $INTAKE_BUILD_DIR-build.log)"; }
    for t in $INTAKE_TEST_EXES; do
        exe="$INTAKE_BUILD_DIR/$t.exe"
        [ -f "$exe" ] || fail "expected intake test exe missing: $exe"
        if command -v file >/dev/null 2>&1; then
            file "$exe" | grep -qE 'PE32\+' || fail "$exe is not a PE32+ binary"
        fi
        OBJDUMP="$OBJDUMP_BIN" tools/check_windows_imports.sh "$exe" >/dev/null \
            || fail "$exe imports a non-system DLL or no UCRT"
        echo "intake test exe OK: $exe"
    done
    echo ""
fi

echo "MINGW CROSS-CHECK: PASS — $ARTIFACT"
