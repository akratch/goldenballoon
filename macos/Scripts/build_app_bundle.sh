#!/usr/bin/env bash
#
# build_app_bundle.sh -- Build a local unsigned mdkr64.app bundle.
#
# There is no Swift/AppKit shell to link against: the CMake target `mdkr64` is
# already a complete, self-contained SDL2 executable (platform/main_pc.c is the
# real `main`), so this script builds that one target and wraps it in a bundle.
#
# The SDL2 dylib discovery and deployment-target reconciliation
# (`sdl2_dylib_path`, `macos_dylib_minos`, `--strict-deployment-target`) exist
# because the engine links exactly one non-system dependency -- Homebrew's
# libSDL2-2.0.0.dylib, as `otool -L build-rel/mdkr64` confirms; every other
# linked image is a system framework. `--bundle-sdl2` embeds that one dylib so
# the bundle runs on a machine without Homebrew.
#
# This is the repeatable maintainer/developer packaging path. It intentionally
# does not sign, notarize, create a DMG, or bundle a ROM. Users still bring
# their own ROM at runtime (the app shell's launcher, platform/app/, is what
# asks for it on first run).
#
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { printf "${GREEN}[INFO]${NC}  %s\n" "$*"; }
warn()  { printf "${YELLOW}[WARN]${NC}  %s\n" "$*"; }
error() { printf "${RED}[ERROR]${NC} %s\n" "$*" >&2; }

die() {
    error "$@"
    exit 1
}

version_gt() {
    local lhs="$1"
    local rhs="$2"
    local lhs_parts rhs_parts
    IFS=. read -r -a lhs_parts <<< "${lhs}"
    IFS=. read -r -a rhs_parts <<< "${rhs}"

    for i in 0 1 2; do
        local l="${lhs_parts[$i]:-0}"
        local r="${rhs_parts[$i]:-0}"
        if ((10#$l > 10#$r)); then
            return 0
        fi
        if ((10#$l < 10#$r)); then
            return 1
        fi
    done

    return 1
}

sdl2_dylib_path() {
    local libdir
    libdir="$(pkg-config --variable=libdir sdl2 2>/dev/null || true)"
    [[ -n "${libdir}" ]] || return 1

    for name in libSDL2-2.0.0.dylib libSDL2.dylib; do
        if [[ -f "${libdir}/${name}" ]]; then
            printf "%s\n" "${libdir}/${name}"
            return 0
        fi
    done

    return 1
}

macos_dylib_minos() {
    local dylib="$1"
    otool -l "${dylib}" 2>/dev/null | awk '
        $1 == "cmd" && $2 == "LC_BUILD_VERSION" {
            in_build_version = 1
            in_version_min = 0
            next
        }
        in_build_version && $1 == "minos" {
            print $2
            exit
        }
        $1 == "cmd" && $2 == "LC_VERSION_MIN_MACOSX" {
            in_version_min = 1
            in_build_version = 0
            next
        }
        in_version_min && $1 == "version" {
            print $2
            exit
        }
    '
}

usage() {
    cat <<'EOF'
Usage: macos/Scripts/build_app_bundle.sh [options]

Builds the mdkr64 native executable via CMake, then assembles an unsigned
local macOS app bundle around it (icon, Info.plist, and the first-run ROM
picker in the app shell's launcher).

Options:
  --release              Build with Release optimizations (default)
  --debug                Build with Debug settings
  --build-dir PATH       CMake build directory (default: build-macos)
  --output PATH          Output .app path (default: <build-dir>/mdkr64.app)
  --arch ARCH            Build one architecture: native, arm64, or x86_64
                         (default: native)
  --version VER          CFBundleShortVersionString / MDKR_VERSION (default: 1.0.0)
  --deployment-target V  Minimum macOS version (default: 13.0)
  --strict-deployment-target
                         Fail if the local SDL2 dylib requires a newer macOS
                         version than --deployment-target.
  --bundle-sdl2          Copy the linked SDL2 dylib into Contents/Frameworks
                         and rewrite the engine binary's load path to the bundle.
  --no-cmake             Reuse an existing <build-dir>/mdkr64
  -h, --help             Show this help

By default the resulting bundle is unsigned and still depends on SDL2 being
available at the path reported by pkg-config. For distributable build
candidates, use --strict-deployment-target --bundle-sdl2, then sign,
notarize, and package the bundle with the separate scripts in macos/Scripts/.
EOF
}

BUILD_TYPE="Release"
BUILD_DIR=""
OUTPUT_APP=""
ARCH="native"
APP_VERSION="1.0.0"
DEPLOYMENT_TARGET="13.0"
STRICT_DEPLOYMENT_TARGET=false
BUNDLE_SDL2=false
RUN_CMAKE=true
APP_NAME="mdkr64"
# CFBundleExecutable: the real binary, directly. It now contains the native
# ImGui app shell (platform/app/), so a Finder double-click with no arguments
# opens the launcher -- the first-run ROM picker, settings and diagnostics --
# and the same binary still runs the unchanged engine path for any invocation
# WITH arguments (platform/app/arg_triage.h).
#
# This retires macos/Sources/mdkr64_launcher_shim.sh, the bash/AppleScript
# stopgap that used to sit in front of the engine and carried its own
# transcribed copy of the ROM revision table. One binary, one revision table
# (platform/rom_id.c), no second executable to keep in sync.
EXECUTABLE_NAME="mdkr64"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release) BUILD_TYPE="Release"; shift ;;
        --debug) BUILD_TYPE="Debug"; shift ;;
        --build-dir)
            [[ $# -ge 2 ]] || die "--build-dir requires a path"
            BUILD_DIR="$2"
            shift 2
            ;;
        --output)
            [[ $# -ge 2 ]] || die "--output requires a path"
            OUTPUT_APP="$2"
            shift 2
            ;;
        --arch)
            [[ $# -ge 2 ]] || die "--arch requires native, arm64, or x86_64"
            ARCH="$2"
            shift 2
            ;;
        --version)
            [[ $# -ge 2 ]] || die "--version requires a value"
            APP_VERSION="$2"
            shift 2
            ;;
        --deployment-target)
            [[ $# -ge 2 ]] || die "--deployment-target requires a version"
            DEPLOYMENT_TARGET="$2"
            shift 2
            ;;
        --strict-deployment-target) STRICT_DEPLOYMENT_TARGET=true; shift ;;
        --bundle-sdl2) BUNDLE_SDL2=true; shift ;;
        --no-cmake) RUN_CMAKE=false; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown argument: $1. Use --help." ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${PROJECT_ROOT}/${BUILD_DIR:-build-macos}"
fi
if [[ -z "${OUTPUT_APP}" ]]; then
    OUTPUT_APP="${BUILD_DIR}/${APP_NAME}.app"
elif [[ "${OUTPUT_APP}" != /* ]]; then
    OUTPUT_APP="${PROJECT_ROOT}/${OUTPUT_APP}"
fi

case "${ARCH}" in
    native) CMAKE_ARCH="$(uname -m)" ;;
    arm64|x86_64) CMAKE_ARCH="${ARCH}" ;;
    *) die "--arch must be native, arm64, or x86_64" ;;
esac

for tool in cmake pkg-config plutil ditto iconutil python3 sips /usr/libexec/PlistBuddy; do
    if ! command -v "$tool" &>/dev/null; then
        die "Required tool '${tool}' not found."
    fi
done
if [[ "${BUNDLE_SDL2}" == true ]] && ! command -v install_name_tool &>/dev/null; then
    die "Required tool 'install_name_tool' not found."
fi

if ! pkg-config --exists sdl2; then
    die "SDL2 was not found by pkg-config. Install it with: brew install sdl2"
fi

REQUESTED_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}"
SDL2_DYLIB="$(sdl2_dylib_path || true)"
SDL2_MINOS=""
if [[ "${STRICT_DEPLOYMENT_TARGET}" == true && -z "${SDL2_DYLIB}" ]]; then
    die "Could not resolve the SDL2 dylib from pkg-config; cannot enforce --strict-deployment-target."
fi
if [[ -n "${SDL2_DYLIB}" ]]; then
    SDL2_MINOS="$(macos_dylib_minos "${SDL2_DYLIB}")"
    if [[ "${STRICT_DEPLOYMENT_TARGET}" == true && -z "${SDL2_MINOS}" ]]; then
        die "Could not determine the SDL2 dylib minimum macOS version; cannot enforce --strict-deployment-target."
    fi
    if [[ -n "${SDL2_MINOS}" ]] && version_gt "${SDL2_MINOS}" "${DEPLOYMENT_TARGET}"; then
        if [[ "${STRICT_DEPLOYMENT_TARGET}" == true ]]; then
            die "SDL2 dylib requires macOS ${SDL2_MINOS}, newer than requested target ${DEPLOYMENT_TARGET}. Use an SDL2 build with a compatible minimum deployment target, or omit --strict-deployment-target for local-only builds."
        else
            warn "SDL2 dylib requires macOS ${SDL2_MINOS}; raising local bundle target from ${DEPLOYMENT_TARGET}."
            DEPLOYMENT_TARGET="${SDL2_MINOS}"
        fi
    fi
fi
if [[ "${BUNDLE_SDL2}" == true && -z "${SDL2_DYLIB}" ]]; then
    die "Could not resolve the SDL2 dylib from pkg-config; cannot bundle SDL2."
fi

info "Project root      : ${PROJECT_ROOT}"
info "Build dir         : ${BUILD_DIR}"
info "Output app        : ${OUTPUT_APP}"
info "Build type        : ${BUILD_TYPE}"
info "Architecture      : ${CMAKE_ARCH}"
info "Version           : ${APP_VERSION}"
if [[ "${REQUESTED_DEPLOYMENT_TARGET}" != "${DEPLOYMENT_TARGET}" ]]; then
    info "Requested target  : ${REQUESTED_DEPLOYMENT_TARGET}"
fi
info "Deployment target : ${DEPLOYMENT_TARGET}"
if [[ -n "${SDL2_DYLIB}" ]]; then
    info "SDL2 dylib        : ${SDL2_DYLIB}${SDL2_MINOS:+ (min macOS ${SDL2_MINOS})}"
fi
if [[ "${STRICT_DEPLOYMENT_TARGET}" == true ]]; then
    info "Strict target     : enabled"
fi
if [[ "${BUNDLE_SDL2}" == true ]]; then
    info "Bundle SDL2       : enabled"
fi

if [[ "${RUN_CMAKE}" == true ]]; then
    info "Configuring mdkr64..."
    cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_OSX_ARCHITECTURES="${CMAKE_ARCH}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DMDKR_VERSION="${APP_VERSION}" \
        || die "CMake configuration failed."

    NCPU="$(sysctl -n hw.ncpu)"
    info "Building mdkr64 with ${NCPU} parallel jobs..."
    cmake --build "${BUILD_DIR}" --target mdkr64 --parallel "${NCPU}" \
        || die "mdkr64 build failed."
fi

ENGINE_BUILD_OUTPUT="${BUILD_DIR}/mdkr64"
if [[ ! -f "${ENGINE_BUILD_OUTPUT}" ]]; then
    die "Missing built executable: ${ENGINE_BUILD_OUTPUT}. Run without --no-cmake first."
fi

rm -rf "${OUTPUT_APP}"
mkdir -p "${OUTPUT_APP}/Contents/MacOS" "${OUTPUT_APP}/Contents/Resources"

INFO_PLIST="${OUTPUT_APP}/Contents/Info.plist"
cp "${PROJECT_ROOT}/macos/Resources/Info.plist" "${INFO_PLIST}"
/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable ${EXECUTABLE_NAME}" "${INFO_PLIST}"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${APP_VERSION}" "${INFO_PLIST}"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion ${APP_VERSION}" "${INFO_PLIST}"
/usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion ${DEPLOYMENT_TARGET}" "${INFO_PLIST}"
plutil -lint "${INFO_PLIST}" >/dev/null

for resource in PrivacyInfo.xcprivacy; do
    if [[ -f "${PROJECT_ROOT}/macos/Resources/${resource}" ]]; then
        ditto "${PROJECT_ROOT}/macos/Resources/${resource}" \
            "${OUTPUT_APP}/Contents/Resources/${resource}"
    fi
done

# SDL_GameControllerDB (zlib; lib/sdl_gamecontrollerdb/LICENSE.txt): plain-text
# community pad mappings, no ROM/asset content. platform/platform_sdl_min.c's
# platform_input_init() looks for it at a CWD-relative "gamecontrollerdb.txt"
# path, which is why the launcher shim `cd`s into Contents/Resources before
# exec'ing the engine.
GAMECONTROLLERDB_SRC="${PROJECT_ROOT}/lib/sdl_gamecontrollerdb/gamecontrollerdb.txt"
if [[ -f "${GAMECONTROLLERDB_SRC}" ]]; then
    ditto "${GAMECONTROLLERDB_SRC}" "${OUTPUT_APP}/Contents/Resources/gamecontrollerdb.txt"
fi

ICONSET_DIR="${BUILD_DIR}/AppIcon.iconset"
APP_ICON="${OUTPUT_APP}/Contents/Resources/AppIcon.icns"
ICON_SOURCE="${PROJECT_ROOT}/brand/appicon-source.png"
ICON_SOURCE_ARGS=()
if [[ -f "${ICON_SOURCE}" ]]; then
    ICON_SOURCE_ARGS=(--source "${ICON_SOURCE}")
else
    warn "brand/appicon-source.png not found; using the placeholder procedural icon."
fi
info "Generating app icon..."
python3 "${PROJECT_ROOT}/macos/Scripts/generate_app_icon.py" \
    --iconset "${ICONSET_DIR}" \
    --icns "${APP_ICON}" \
    "${ICON_SOURCE_ARGS[@]}" \
    || die "App icon generation failed."
[[ -s "${APP_ICON}" ]] || die "Generated app icon is missing: ${APP_ICON}"

# One executable: the app shell and the engine are the same binary.
ENGINE_PATH="${OUTPUT_APP}/Contents/MacOS/${EXECUTABLE_NAME}"

info "Installing mdkr64 (app shell + engine)..."
ditto "${ENGINE_BUILD_OUTPUT}" "${ENGINE_PATH}"
chmod +x "${ENGINE_PATH}"

SDL2_BUNDLED_PATH=""
SDL2_BUNDLE_LOAD_PATH=""
if [[ "${BUNDLE_SDL2}" == true ]]; then
    FRAMEWORKS_DIR="${OUTPUT_APP}/Contents/Frameworks"
    SDL2_BASENAME="$(basename "${SDL2_DYLIB}")"
    SDL2_BUNDLED_PATH="${FRAMEWORKS_DIR}/${SDL2_BASENAME}"
    SDL2_BUNDLE_LOAD_PATH="@executable_path/../Frameworks/${SDL2_BASENAME}"

    info "Bundling SDL2 dylib..."
    mkdir -p "${FRAMEWORKS_DIR}"
    ditto "${SDL2_DYLIB}" "${SDL2_BUNDLED_PATH}" \
        || die "Failed to copy SDL2 dylib into the app bundle."
    chmod u+w "${SDL2_BUNDLED_PATH}" 2>/dev/null || true

    # Only the one executable needs its SDL2 load path rewritten.
    SDL2_LOAD_PATHS=()
    while IFS= read -r load_path; do
        [[ -n "${load_path}" ]] && SDL2_LOAD_PATHS+=("${load_path}")
    done < <(
        otool -L "${ENGINE_PATH}" \
            | awk '/libSDL2[^[:space:]]*\.dylib/ { print $1 }' \
            | sort -u
    )
    if [[ "${#SDL2_LOAD_PATHS[@]}" -eq 0 ]]; then
        die "The engine binary does not link an SDL2 dylib; cannot rewrite bundle load path."
    fi
    for load_path in "${SDL2_LOAD_PATHS[@]}"; do
        install_name_tool -change "${load_path}" "${SDL2_BUNDLE_LOAD_PATH}" "${ENGINE_PATH}" \
            || die "Failed to rewrite SDL2 load path: ${load_path}"
    done

    if ! otool -L "${ENGINE_PATH}" | grep -Fq "${SDL2_BUNDLE_LOAD_PATH}"; then
        die "SDL2 bundle load path was not recorded in the engine binary."
    fi
fi

echo "APPL????" > "${OUTPUT_APP}/Contents/PkgInfo"

echo ""
info "========== App Bundle Summary =========="
info "App bundle    : ${OUTPUT_APP}"
info "Executable    : ${ENGINE_PATH} (CFBundleExecutable; app shell + engine)"
info "Architectures : $(lipo -info "${ENGINE_PATH}" 2>/dev/null || file "${ENGINE_PATH}")"
info "Bundle size   : $(du -sh "${OUTPUT_APP}" | cut -f1)"
info "App icon      : ${APP_ICON}"
info "SDL2 link     : $(otool -L "${ENGINE_PATH}" | grep -E 'libSDL2' | sed 's/^[[:space:]]*//' || echo 'not found')"
if [[ -n "${SDL2_BUNDLED_PATH}" ]]; then
    info "SDL2 bundled  : ${SDL2_BUNDLED_PATH}"
fi
info "Verify assets : ${PROJECT_ROOT}/macos/Scripts/verify_asset_free.sh '${OUTPUT_APP}'"
info "CLI/CI use    : '${ENGINE_PATH}' --rom ROM --headless-frames N   (any argument bypasses the launcher)"
info "========================================"

warn "This app is unsigned and not notarized. Use sign_and_notarize.sh only for distributable builds."
