#!/usr/bin/env bash
#
# build_app_bundle.sh -- Build a local ad-hoc-signed mdkr64.app bundle.
#
# There is no Swift/AppKit shell to link against: the CMake target `mdkr64` is
# already a complete, self-contained SDL2 executable. The native app shell owns
# `main()` and delegates command-line invocations to the engine entry point, so
# this script builds that one target and wraps it in a bundle.
#
# The SDL2 dylib discovery and deployment-target reconciliation
# (`sdl2_dylib_path`, `macos_dylib_minos`, `--strict-deployment-target`) exist
# because the engine links exactly one non-system dependency. Release builds
# select the pinned standalone SDL2 produced by build_release_sdl2.sh;
# `--bundle-sdl2` embeds it so the app has no package-manager dependency.
#
# This is the repeatable maintainer/developer packaging path. It applies an
# ad-hoc integrity signature after every bundle mutation, but does not apply a
# trusted Developer ID signature, notarize, create a DMG, or bundle a ROM.
# Users still bring their own ROM at runtime (the app shell's launcher,
# platform/app/, is what asks for it on first run).
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

canonical_output_app_path() {
    python3 - "$1" "$2" "$3" <<'PY'
import os
import pathlib
import plistlib
import re
import sys


def reject(reason: str) -> None:
    print(f"unsafe --output path: {reason}", file=sys.stderr)
    raise SystemExit(2)


raw = pathlib.Path(os.path.abspath(os.path.expanduser(sys.argv[1])))
if raw.is_symlink():
    reject("the output itself must not be a symbolic link")

candidate = raw.resolve(strict=False)
build_dir = pathlib.Path(sys.argv[2]).resolve(strict=False)
project_root = pathlib.Path(sys.argv[3]).resolve(strict=True)
user_home = pathlib.Path.home().resolve(strict=True)

if candidate in (pathlib.Path("/"), user_home, project_root):
    reject(f"refusing broad target {candidate}")
if candidate.suffix != ".app" or candidate.name == ".app":
    reject("target must have a nonempty name ending in .app")
if candidate.name != "mdkr64.app":
    reject("target basename must be mdkr64.app")
if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._ -]*\.app", candidate.name):
    reject("app bundle name contains unsupported characters")
if candidate.parent == pathlib.Path("/"):
    reject("app bundle must not be created directly under the filesystem root")
if candidate == build_dir or candidate in build_dir.parents:
    reject("target must not equal or contain the CMake build directory")
if candidate == project_root or candidate in project_root.parents:
    reject("target must not equal or contain the repository")
if candidate.exists():
    if not candidate.is_dir():
        reject("an existing target must be a real app-bundle directory")
    plist_path = candidate / "Contents" / "Info.plist"
    if plist_path.is_symlink() or not plist_path.is_file():
        reject("existing target is not an identified mdkr64 bundle")
    try:
        with plist_path.open("rb") as stream:
            plist = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException):
        reject("existing target has an invalid Info.plist")
    if (
        plist.get("CFBundleIdentifier") != "com.mdkr64.app"
        or plist.get("CFBundleExecutable") != "mdkr64"
    ):
        reject("existing target belongs to another application")

print(candidate)
PY
}

usage() {
    cat <<'EOF'
Usage: macos/Scripts/build_app_bundle.sh [options]

Builds the mdkr64 native executable via CMake, then assembles an ad-hoc-signed
local macOS app bundle around it (icon, Info.plist, and the first-run ROM
picker in the app shell's launcher).

Options:
  --release              Build with Release optimizations (default)
  --debug                Build with Debug settings
  --build-dir PATH       CMake build directory (default: build-macos)
  --output PATH          Output .app path (default: <build-dir>/mdkr64.app)
  --arch ARCH            Build one architecture: native, arm64, or x86_64
                         (default: native)
  --version VER          CFBundleShortVersionString / MDKR_VERSION (default: 1.3.0)
  --build-stamp SHA      Source commit shown in the About panel (default: empty)
  --party-origin URL     Phone Party service origin compiled into the launcher
                         (-DMDKR_PARTY_ORIGIN; default: empty). Must be empty
                         or an https:// origin -- CMake rejects anything else.
                         Empty is legal and means the built app shows no Phone
                         Party surface at all, so release lanes must pass the
                         deployed origin here. See
                         docs/multiplayer/DEPLOY_PHONE_PARTY.md.
  --deployment-target V  Minimum macOS version (default: 13.0)
  --strict-deployment-target
                         Fail if the local SDL2 dylib requires a newer macOS
                         version than --deployment-target.
  --bundle-sdl2          Copy the linked SDL2 dylib into Contents/Frameworks
                         and rewrite the engine binary's load path to the bundle.
  --validate-output-only Validate --output safety and exit without writing
  --no-cmake             Reuse an existing <build-dir>/mdkr64
  -h, --help             Show this help

By default the resulting bundle is ad-hoc signed for integrity only and still
depends on SDL2 at the path reported by pkg-config. For a distributable build,
use --strict-deployment-target --bundle-sdl2 and run the unsigned release
verifier. Developer ID signing/notarization is a separate optional path.
EOF
}

BUILD_TYPE="Release"
BUILD_DIR=""
OUTPUT_APP=""
ARCH="native"
APP_VERSION="1.3.0"
BUILD_STAMP=""
# Empty by default: a local developer build has no deployed Phone Party
# service to point at, and an empty origin is a legal (party-free) build.
PARTY_ORIGIN=""
DEPLOYMENT_TARGET="13.0"
STRICT_DEPLOYMENT_TARGET=false
BUNDLE_SDL2=false
RUN_CMAKE=true
VALIDATE_OUTPUT_ONLY=false
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
        --build-stamp)
            [[ $# -ge 2 ]] || die "--build-stamp requires a full commit SHA"
            BUILD_STAMP="$2"
            shift 2
            ;;
        --party-origin)
            # An explicit empty value is accepted (party-free build); only a
            # missing operand is an error.
            [[ $# -ge 2 ]] || die "--party-origin requires an https:// origin (or an empty string)"
            PARTY_ORIGIN="$2"
            shift 2
            ;;
        --deployment-target)
            [[ $# -ge 2 ]] || die "--deployment-target requires a version"
            DEPLOYMENT_TARGET="$2"
            shift 2
            ;;
        --strict-deployment-target) STRICT_DEPLOYMENT_TARGET=true; shift ;;
        --bundle-sdl2) BUNDLE_SDL2=true; shift ;;
        --validate-output-only) VALIDATE_OUTPUT_ONLY=true; shift ;;
        --no-cmake) RUN_CMAKE=false; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown argument: $1. Use --help." ;;
    esac
done

[[ "${APP_VERSION}" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] ||
    die "--version must look like 1.0 or 1.0.5 (no v prefix)"
if [[ -n "${BUILD_STAMP}" && ! "${BUILD_STAMP}" =~ ^[0-9a-fA-F]{40}$ ]]; then
    die "--build-stamp must be a full 40-character hexadecimal commit SHA"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/release_sdl2_config.sh"

if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${PROJECT_ROOT}/${BUILD_DIR:-build-macos}"
fi
if [[ -z "${OUTPUT_APP}" ]]; then
    OUTPUT_APP="${BUILD_DIR}/${APP_NAME}.app"
elif [[ "${OUTPUT_APP}" != /* ]]; then
    OUTPUT_APP="${PROJECT_ROOT}/${OUTPUT_APP}"
fi

command -v python3 >/dev/null 2>&1 || die "Required tool 'python3' not found."
if ! SAFE_OUTPUT_APP="$(canonical_output_app_path \
        "${OUTPUT_APP}" "${BUILD_DIR}" "${PROJECT_ROOT}")"; then
    die "Refusing unsafe --output target: ${OUTPUT_APP}"
fi
OUTPUT_APP="${SAFE_OUTPUT_APP}"
if [[ "${VALIDATE_OUTPUT_ONLY}" == true ]]; then
    info "Safe app output: ${OUTPUT_APP}"
    exit 0
fi

case "${ARCH}" in
    native) CMAKE_ARCH="$(uname -m)" ;;
    arm64|x86_64) CMAKE_ARCH="${ARCH}" ;;
    *) die "--arch must be native, arm64, or x86_64" ;;
esac

for tool in cmake pkg-config plutil ditto iconutil python3 sips codesign xattr otool shasum strings /usr/libexec/PlistBuddy; do
    if ! command -v "$tool" &>/dev/null; then
        die "Required tool '${tool}' not found."
    fi
done
if [[ "${BUNDLE_SDL2}" == true ]] && ! command -v install_name_tool &>/dev/null; then
    die "Required tool 'install_name_tool' not found."
fi

if ! pkg-config --exists sdl2; then
    die "SDL2 was not found by pkg-config. For a release, run" \
        "macos/Scripts/build_release_sdl2.sh and prepend its" \
        "install/lib/pkgconfig directory to PKG_CONFIG_PATH."
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
            die "SDL2 dylib requires macOS ${SDL2_MINOS}, newer than requested" \
                "target ${DEPLOYMENT_TARGET}. Use an SDL2 build with a compatible" \
                "minimum, or omit --strict-deployment-target for local builds."
        else
            warn "SDL2 dylib requires macOS ${SDL2_MINOS}; raising local bundle target from ${DEPLOYMENT_TARGET}."
            DEPLOYMENT_TARGET="${SDL2_MINOS}"
        fi
    fi
fi
if [[ "${BUNDLE_SDL2}" == true && -z "${SDL2_DYLIB}" ]]; then
    die "Could not resolve the SDL2 dylib from pkg-config; cannot bundle SDL2."
fi
SDL2_PC_DIR="$(pkg-config --variable=pcfiledir sdl2 2>/dev/null || true)"
SDL2_PC_FILE="${SDL2_PC_DIR:+${SDL2_PC_DIR}/sdl2.pc}"
SDL2_PREFIX="$(pkg-config --variable=prefix sdl2 2>/dev/null || true)"
SDL2_LICENSE="${SDL2_PREFIX:+${SDL2_PREFIX}/share/licenses/SDL2/LICENSE.txt}"
SDL2_RELEASE_MANIFEST="${SDL2_PREFIX:+${SDL2_PREFIX}/share/mdkr64/SDL2-release-manifest.txt}"
SDL2_INPUT_DYLIB_SHA256=""
if [[ "${BUNDLE_SDL2}" == true && -f "${SDL2_PC_FILE}" ]] &&
        grep -Eq '^Name:[[:space:]]*sdl2_compat([[:space:]]|$)' "${SDL2_PC_FILE}"; then
    die "pkg-config resolves sdl2 to sdl2-compat, which loads SDL3 dynamically." \
        "Bundling that SDL2 shim alone is not self-contained. Build the pinned" \
        "standalone dependency with macos/Scripts/build_release_sdl2.sh and" \
        "prepend its install/lib/pkgconfig directory to PKG_CONFIG_PATH."
fi
if [[ "${BUNDLE_SDL2}" == true ]] && grep -aFq 'sdl2-compat:' "${SDL2_DYLIB}"; then
    die "resolved SDL2 dylib is the SDL3-loading sdl2-compat shim"
fi
if [[ "${BUNDLE_SDL2}" == true && ! -f "${SDL2_LICENSE}" ]]; then
    die "standalone SDL2 license is missing: ${SDL2_LICENSE:-unknown prefix}"
fi
if [[ "${BUNDLE_SDL2}" == true && ! -f "${SDL2_RELEASE_MANIFEST}" ]]; then
    die "pinned SDL2 release manifest is missing: ${SDL2_RELEASE_MANIFEST:-unknown prefix}"
fi
if [[ "${BUNDLE_SDL2}" == true ]]; then
    grep -Fqx "version=${MDKR_RELEASE_SDL2_VERSION}" "${SDL2_RELEASE_MANIFEST}" ||
        die "SDL2 manifest version is not the pinned ${MDKR_RELEASE_SDL2_VERSION}"
    grep -Fqx "source_sha256=${MDKR_RELEASE_SDL2_SOURCE_SHA256}" \
        "${SDL2_RELEASE_MANIFEST}" ||
        die "SDL2 manifest source hash does not match the authenticated archive"
    SDL2_INPUT_DYLIB_SHA256="$(awk -F= \
        '$1 == "dylib_sha256" { print $2 }' "${SDL2_RELEASE_MANIFEST}")"
    [[ "${SDL2_INPUT_DYLIB_SHA256}" =~ ^[0-9a-f]{64}$ ]] ||
        die "SDL2 manifest has no canonical dylib SHA-256"
    SDL2_ACTUAL_DYLIB_SHA256="$(shasum -a 256 "${SDL2_DYLIB}" | awk '{print $1}')"
    [[ "${SDL2_ACTUAL_DYLIB_SHA256}" == "${SDL2_INPUT_DYLIB_SHA256}" ]] ||
        die "resolved SDL2 dylib does not match its authenticated release manifest"
fi

info "Project root      : ${PROJECT_ROOT}"
info "Build dir         : ${BUILD_DIR}"
info "Output app        : ${OUTPUT_APP}"
info "Build type        : ${BUILD_TYPE}"
info "Architecture      : ${CMAKE_ARCH}"
info "Version           : ${APP_VERSION}"
if [[ -n "${BUILD_STAMP}" ]]; then
    info "Source commit     : ${BUILD_STAMP}"
fi
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
if [[ -n "${PARTY_ORIGIN}" ]]; then
    info "Phone Party origin: ${PARTY_ORIGIN}"
else
    warn "Phone Party origin: (none) -- this build shows no Phone Party surface"
fi

if [[ "${RUN_CMAKE}" == true ]]; then
    info "Configuring mdkr64..."
    PATH_MAP_FLAGS="-ffile-prefix-map=${PROJECT_ROOT}=mdkr64"
    PATH_MAP_FLAGS+=" -fmacro-prefix-map=${PROJECT_ROOT}=mdkr64"
    PATH_MAP_FLAGS+=" -fdebug-prefix-map=${PROJECT_ROOT}=mdkr64"
    PATH_MAP_FLAGS+=" -ffile-prefix-map=${BUILD_DIR}=mdkr64-build"
    PATH_MAP_FLAGS+=" -fmacro-prefix-map=${BUILD_DIR}=mdkr64-build"
    PATH_MAP_FLAGS+=" -fdebug-prefix-map=${BUILD_DIR}=mdkr64-build"
    cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_OSX_ARCHITECTURES="${CMAKE_ARCH}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        "-DCMAKE_C_FLAGS=${PATH_MAP_FLAGS}" \
        "-DCMAKE_CXX_FLAGS=${PATH_MAP_FLAGS}" \
        "-DCMAKE_OBJC_FLAGS=${PATH_MAP_FLAGS}" \
        "-DCMAKE_OBJCXX_FLAGS=${PATH_MAP_FLAGS}" \
        -DMDKR_VERSION="${APP_VERSION}" \
        -DMDKR_BUILD_STAMP="${BUILD_STAMP}" \
        -DMDKR_PARTY_ORIGIN="${PARTY_ORIGIN}" \
        -DMDKR_WEBGPU_BACKEND=ON \
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

CMAKE_CACHE="${BUILD_DIR}/CMakeCache.txt"
[[ -f "${CMAKE_CACHE}" ]] || die "Missing CMake cache: ${CMAKE_CACHE}"
grep -Eq '^MDKR_WEBGPU_BACKEND:BOOL=ON$' "${CMAKE_CACHE}" ||
    die "Build cache does not enable the required WebGPU backend."
grep -Fqx "MDKR_VERSION:STRING=${APP_VERSION}" "${CMAKE_CACHE}" ||
    die "Build cache version does not match ${APP_VERSION}."
grep -Fqx "MDKR_BUILD_STAMP:STRING=${BUILD_STAMP}" "${CMAKE_CACHE}" ||
    die "Build cache provenance stamp does not match the requested value."
VERSION_OUTPUT="$("${ENGINE_BUILD_OUTPUT}" --version 2>&1)"
[[ "${VERSION_OUTPUT}" == "mdkr64 ${APP_VERSION}" ]] ||
    die "Built executable reports '${VERSION_OUTPUT}', expected 'mdkr64 ${APP_VERSION}'."

rm -rf "${OUTPUT_APP}"
mkdir -p "${OUTPUT_APP}/Contents/MacOS" "${OUTPUT_APP}/Contents/Resources"

INFO_PLIST="${OUTPUT_APP}/Contents/Info.plist"
cp "${PROJECT_ROOT}/macos/Resources/Info.plist" "${INFO_PLIST}"
/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable ${EXECUTABLE_NAME}" "${INFO_PLIST}"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${APP_VERSION}" "${INFO_PLIST}"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion ${APP_VERSION}" "${INFO_PLIST}"
/usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion ${DEPLOYMENT_TARGET}" "${INFO_PLIST}"
plutil -lint "${INFO_PLIST}" >/dev/null

PRIVACY_MANIFEST="${PROJECT_ROOT}/macos/Resources/PrivacyInfo.xcprivacy"
if [[ -f "${PRIVACY_MANIFEST}" ]]; then
    ditto "${PRIVACY_MANIFEST}" \
        "${OUTPUT_APP}/Contents/Resources/PrivacyInfo.xcprivacy"
fi

# SDL_GameControllerDB (zlib; lib/sdl_gamecontrollerdb/LICENSE.txt): plain-text
# community pad mappings, no ROM/asset content. The app entry point registers
# Contents/Resources as an immutable absolute root; platform_input_init() reads
# this file through that root without changing CWD or writing into the bundle.
GAMECONTROLLERDB_SRC="${PROJECT_ROOT}/lib/sdl_gamecontrollerdb/gamecontrollerdb.txt"
[[ -s "${GAMECONTROLLERDB_SRC}" ]] ||
    die "Tracked SDL game-controller mapping database is missing or empty: ${GAMECONTROLLERDB_SRC}"
ditto "${GAMECONTROLLERDB_SRC}" \
    "${OUTPUT_APP}/Contents/Resources/gamecontrollerdb.txt" ||
    die "Failed to copy the game-controller mapping database into the app bundle."
if [[ "${BUNDLE_SDL2}" == true ]]; then
    SDL2_LICENSE_DEST="${OUTPUT_APP}/Contents/Resources/ThirdParty/SDL2-LICENSE.txt"
    mkdir -p "$(dirname "${SDL2_LICENSE_DEST}")"
    ditto "${SDL2_LICENSE}" "${SDL2_LICENSE_DEST}"
fi
PHONE_PARTY_NOTICE_SRC="${PROJECT_ROOT}/third_party/native_phone_party/NOTICE.txt"
PHONE_PARTY_NOTICE_DEST="${OUTPUT_APP}/Contents/Resources/ThirdParty/NativePhoneParty-NOTICES.txt"
[[ -s "${PHONE_PARTY_NOTICE_SRC}" ]] ||
    die "Tracked native Phone Party notice is missing or empty: ${PHONE_PARTY_NOTICE_SRC}"
mkdir -p "$(dirname "${PHONE_PARTY_NOTICE_DEST}")"
ditto "${PHONE_PARTY_NOTICE_SRC}" "${PHONE_PARTY_NOTICE_DEST}" ||
    die "Failed to copy native Phone Party notices into the app bundle."

ICONSET_DIR="${BUILD_DIR}/AppIcon.iconset"
APP_ICON="${OUTPUT_APP}/Contents/Resources/AppIcon.icns"
ICON_SOURCE="${PROJECT_ROOT}/brand/appicon-source.png"
ICON_SOURCE_ARGS=()
if [[ -s "${ICON_SOURCE}" ]]; then
    ICON_SOURCE_ARGS=(--source "${ICON_SOURCE}")
elif [[ "${BUILD_TYPE}" == "Release" ]]; then
    die "Tracked branded icon source is required for a Release app bundle: ${ICON_SOURCE}"
else
    warn "brand/appicon-source.png not found; using the procedural icon for this Debug build."
fi
info "Generating app icon..."
python3 "${PROJECT_ROOT}/macos/Scripts/generate_app_icon.py" \
    --iconset "${ICONSET_DIR}" \
    --icns "${APP_ICON}" \
    ${ICON_SOURCE_ARGS[@]+"${ICON_SOURCE_ARGS[@]}"} \
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

# CMake must add the build-time SDL directory as LC_RPATH when upstream SDL2
# advertises its canonical @rpath install name. The copied app no longer needs
# that search path after the dependency is rewritten above. Remove every
# absolute rpath from the final executable rather than weakening the path-leak
# gate or relying on the release runner's checkout location.
ABSOLUTE_RPATHS=()
while IFS= read -r rpath; do
    [[ "${rpath}" == /* ]] && ABSOLUTE_RPATHS+=("${rpath}")
done < <(
    otool -l "${ENGINE_PATH}" | awk '
        $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
        in_rpath && $1 == "path" { print $2; in_rpath = 0 }
    '
)
# Bash 3.2 (the macOS system shell) treats "${arr[@]}" on an empty array as an
# unbound reference under set -u, so an empty expansion must be spelled out.
for rpath in ${ABSOLUTE_RPATHS[@]+"${ABSOLUTE_RPATHS[@]}"}; do
    install_name_tool -delete_rpath "${rpath}" "${ENGINE_PATH}" \
        || die "Failed to remove absolute build rpath: ${rpath}"
done

# Release copies do not ship compiler path records or non-exported local
# symbols. Some vendored C++ objects encode source paths in local lambda symbol
# names even when their debug records are absent. Keep the unmodified build
# output (and its symbols) in BUILD_DIR for debugging; normalize only the app
# payload that users receive.
strip -S -x "${ENGINE_PATH}" || die "Failed to strip compiler and local symbols."

if otool -l "${ENGINE_PATH}" | awk '
        $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
        in_rpath && $1 == "path" {
            if ($2 ~ /^\//) found = 1
            in_rpath = 0
        }
        END { exit found ? 0 : 1 }
    '; then
    die "Final app executable still contains an absolute runtime search path."
fi
if grep -aFq "${PROJECT_ROOT}" "${ENGINE_PATH}" ||
        grep -aFq "${BUILD_DIR}" "${ENGINE_PATH}"; then
    # Hosted failures must identify the retained record without requiring an
    # interactive runner. Limit output so a corrupt binary cannot flood logs.
    strings -a "${ENGINE_PATH}" \
        | grep -F -e "${PROJECT_ROOT}" -e "${BUILD_DIR}" \
        | head -n 20 >&2 || true
    die "Final app executable contains an absolute source/build path."
fi
if [[ -n "${SDL2_DYLIB}" ]]; then
    SDL2_SOURCE_DIR="$(dirname "${SDL2_DYLIB}")"
    if grep -aFq "${SDL2_SOURCE_DIR}" "${ENGINE_PATH}"; then
        die "Final app executable contains the build-time SDL2 path."
    fi
fi

VERSION_OUTPUT="$("${ENGINE_PATH}" --version 2>&1)"
[[ "${VERSION_OUTPUT}" == "mdkr64 ${APP_VERSION}" ]] ||
    die "Bundled executable reports '${VERSION_OUTPUT}', expected 'mdkr64 ${APP_VERSION}'."

echo "APPL????" > "${OUTPUT_APP}/Contents/PkgInfo"

# install_name_tool invalidates the linker's ad-hoc Mach-O signature. On Apple
# silicon that is an integrity failure, not merely an untrusted-developer case,
# and Finder reports the quarantined app as "damaged". Remove inherited source
# xattrs, sign nested code first, then seal the outer bundle. A later Developer
# ID release signature replaces these ad-hoc signatures inside-out.
xattr -cr "${OUTPUT_APP}"
if [[ -n "${SDL2_BUNDLED_PATH}" ]]; then
    info "Applying ad-hoc integrity signature to bundled SDL2..."
    codesign --force --sign - "${SDL2_BUNDLED_PATH}" \
        || die "Failed to ad-hoc sign bundled SDL2."
    SDL2_BUNDLED_SHA256="$(shasum -a 256 "${SDL2_BUNDLED_PATH}" | awk '{print $1}')"
    SDL2_BUNDLE_MANIFEST="${OUTPUT_APP}/Contents/Resources/ThirdParty/SDL2-MANIFEST.txt"
    printf 'version=%s\nsource_sha256=%s\ninput_dylib_sha256=%s\nbundled_dylib_sha256=%s\n' \
        "${MDKR_RELEASE_SDL2_VERSION}" \
        "${MDKR_RELEASE_SDL2_SOURCE_SHA256}" \
        "${SDL2_INPUT_DYLIB_SHA256}" \
        "${SDL2_BUNDLED_SHA256}" >"${SDL2_BUNDLE_MANIFEST}"
fi
info "Applying ad-hoc integrity signature and resource seal to app..."
codesign --force --sign - "${OUTPUT_APP}" \
    || die "Failed to ad-hoc sign app bundle."
"${PROJECT_ROOT}/macos/Scripts/verify_gatekeeper_bundle.sh" "${OUTPUT_APP}" \
    || die "App bundle integrity verification failed."

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
info "Code integrity: ad-hoc signature valid (not Developer ID trust)"
info "========================================"

warn "This app is ad-hoc sealed but not Developer ID signed or notarized."
warn "For the unsigned patch release, run verify_unsigned_release.sh before" \
    "packaging; use sign_and_notarize.sh only for the optional trusted path."
