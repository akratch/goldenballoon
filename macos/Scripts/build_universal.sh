#!/bin/bash
# build_universal.sh — Build a universal (arm64 + x86_64) mdkr64 executable
#
# There is no second link stage in this project, so the script asks CMake to
# build the `mdkr64` EXECUTABLE directly for both architectures in one
# configure (CMAKE_OSX_ARCHITECTURES="arm64;x86_64"), which is what a
# single-invocation Clang multi-arch build produces natively.
#
# That only works if EVERY linked dependency also has both architecture
# slices. On this project's native macOS link that means:
#   - Homebrew's SDL2 dylib (pkg-config sdl2) -- Apple Silicon Homebrew only
#     ships arm64 by default; there is no bundled x86_64 slice unless a
#     Rosetta/x86_64 Homebrew prefix (/usr/local) is also installed and
#     pkg-config is pointed at it.
#   - The pinned wgpu-native prebuilt (cmake/webgpu.cmake) -- fetched as ONE
#     architecture-specific archive per configure, selected from
#     CMAKE_SYSTEM_PROCESSOR; it does not fetch both slices for a multi-arch
#     CMAKE_OSX_ARCHITECTURES list.
# Either gap fails the final link with "building for macOS-arm64_x86_64 but
# attempting to link with file built for arm64" (or an outright missing
# symbol architecture). This script does not attempt to paper over that --
# it builds, and if the link fails for exactly this reason it says so and
# exits, so the caller can fall back to a single-arch build
# (build_app_bundle.sh --arch arm64) and record the delta rather than ship a
# silently-broken slice.
#
# Usage: ./build_universal.sh [options]
#   --release              Build with Release optimizations (default)
#   --debug                Build with Debug symbols and no optimization
#   --build-dir PATH       CMake build directory (default: build-macos)
#   --deployment-target V  Minimum macOS version (default: 13.0)

set -euo pipefail

# ---------------------------------------------------------------------------
# Color helpers
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info()    { printf "${GREEN}[INFO]${NC}  %s\n" "$*"; }
warn()    { printf "${YELLOW}[WARN]${NC}  %s\n" "$*"; }
error()   { printf "${RED}[ERROR]${NC} %s\n" "$*" >&2; }

die() {
    error "$@"
    exit 1
}

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
BUILD_TYPE="Release"
BUILD_DIR=""
DEPLOYMENT_TARGET="13.0"

usage() {
    cat <<'EOF'
Usage: macos/Scripts/build_universal.sh [options]

Attempts a universal arm64 + x86_64 build of the mdkr64 executable in one
CMake configure. Fails loudly (with an explanation) if a linked dependency
(SDL2, wgpu-native) lacks an x86_64 slice on this machine -- see the header
comment in this script. This does not sign, notarize, or package a .app.

Options:
  --release              Build with Release optimizations (default)
  --debug                Build with Debug symbols and no optimization
  --build-dir PATH       CMake build directory (default: build-macos)
  --deployment-target V  Minimum macOS version (default: 13.0)
  -h, --help             Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release) BUILD_TYPE="Release"; shift ;;
        --debug)   BUILD_TYPE="Debug";   shift ;;
        --build-dir)
            [[ $# -ge 2 ]] || die "--build-dir requires a path"
            BUILD_DIR="$2"
            shift 2
            ;;
        --deployment-target)
            [[ $# -ge 2 ]] || die "--deployment-target requires a version"
            DEPLOYMENT_TARGET="$2"
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *)         usage >&2; die "Unknown argument: $1" ;;
    esac
done

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${PROJECT_ROOT}/${BUILD_DIR:-build-macos}"
fi

info "Project root : ${PROJECT_ROOT}"
info "Build dir    : ${BUILD_DIR}"
info "Build type   : ${BUILD_TYPE}"
info "Target       : ${DEPLOYMENT_TARGET}"

# ---------------------------------------------------------------------------
# Check required tools
# ---------------------------------------------------------------------------
for tool in cmake clang lipo pkg-config; do
    if ! command -v "$tool" &>/dev/null; then
        die "Required tool '${tool}' not found. Please install it before continuing."
    fi
done

info "cmake  : $(cmake --version | head -1)"
info "clang  : $(clang --version | head -1)"

SDL2_LIBDIR="$(pkg-config --variable=libdir sdl2 2>/dev/null || true)"
if [[ -n "${SDL2_LIBDIR}" ]]; then
    for name in libSDL2-2.0.0.dylib libSDL2.dylib; do
        if [[ -f "${SDL2_LIBDIR}/${name}" ]]; then
            SDL2_ARCHS="$(lipo -archs "${SDL2_LIBDIR}/${name}" 2>/dev/null || echo unknown)"
            info "SDL2 dylib archs: ${SDL2_ARCHS} (${SDL2_LIBDIR}/${name})"
            if [[ "${SDL2_ARCHS}" != *x86_64* ]]; then
                warn "SDL2 has no x86_64 slice; the universal link below will likely fail there."
            fi
            break
        fi
    done
fi

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
info "Configuring CMake for arm64;x86_64..."

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    || die "CMake configuration failed."

info "Configuration complete."

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
NCPU="$(sysctl -n hw.ncpu)"
info "Building mdkr64 with ${NCPU} parallel jobs..."

BUILD_LOG="$(mktemp "${TMPDIR:-/tmp}/mdkr64-universal-build.XXXXXX.log")"
if ! cmake --build "${BUILD_DIR}" --target mdkr64 --parallel "${NCPU}" 2>&1 | tee "${BUILD_LOG}"; then
    if grep -qE "building for macOS-[^ ]*x86_64.*but attempting to link with file built for|has no symbols for architecture x86_64|does not contain that architecture|symbol\(s\) not found for architecture x86_64" "${BUILD_LOG}"; then
        error "Universal link failed: a dependency (SDL2 and/or wgpu-native) has no x86_64 slice on this machine."
        error "Fall back to a single-architecture build: macos/Scripts/build_app_bundle.sh --arch arm64"
        rm -f "${BUILD_LOG}"
        exit 1
    fi
    rm -f "${BUILD_LOG}"
    die "mdkr64 build failed. Check the output above for details."
fi
rm -f "${BUILD_LOG}"

info "Build complete."

# ---------------------------------------------------------------------------
# Locate the universal executable and print summary
# ---------------------------------------------------------------------------
EXECUTABLE="${BUILD_DIR}/mdkr64"

if [[ ! -f "${EXECUTABLE}" ]]; then
    die "Expected executable not found: ${EXECUTABLE}"
fi

echo ""
info "========== Build Summary =========="
info "Executable   : ${EXECUTABLE}"
info "Architectures: $(lipo -info "${EXECUTABLE}" 2>/dev/null || file "${EXECUTABLE}")"
info "Size         : $(du -h "${EXECUTABLE}" | cut -f1)"
info "Verify assets: ${PROJECT_ROOT}/macos/Scripts/verify_asset_free.sh '${EXECUTABLE}'"
info "==================================="

info "Done."
