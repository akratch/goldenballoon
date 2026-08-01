#!/usr/bin/env bash
# Build the pinned, standalone SDL2 dylib used by self-contained macOS releases.
#
# Homebrew's `sdl2` alias may resolve to sdl2-compat, a compatibility shim that
# loads SDL3 dynamically. Copying only that shim into an app does not make the
# app self-contained. Building upstream SDL2 from its authenticated source also
# lets us set an honest deployment target instead of inheriting a bottle's host
# OS minimum.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The dynamic absolute path keeps sourcing independent of the caller's CWD.
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/release_sdl2_config.sh"

die() {
    printf 'build_release_sdl2: FAIL — %s\n' "$*" >&2
    exit 1
}

canonical_sdl_paths() {
    python3 - "$1" "$2" "$3" "${MDKR_RELEASE_SDL2_VERSION}" <<'PY'
import os
import pathlib
import sys


def reject(reason: str) -> None:
    print(f"unsafe SDL2 build path: {reason}", file=sys.stderr)
    raise SystemExit(2)


raw_work = pathlib.Path(os.path.abspath(os.path.expanduser(sys.argv[1])))
raw_prefix = pathlib.Path(os.path.abspath(os.path.expanduser(sys.argv[2])))
project_root = pathlib.Path(sys.argv[3]).resolve(strict=True)
user_home = pathlib.Path.home().resolve(strict=True)
version = sys.argv[4]
if raw_work.is_symlink() or raw_prefix.is_symlink():
    reject("work directory and prefix must not be symbolic links")

work = raw_work.resolve(strict=False)
prefix = raw_prefix.resolve(strict=False)
if work.name != f"sdl2-{version}":
    reject(f"work directory basename must be sdl2-{version}")
if work.parent == pathlib.Path("/"):
    reject("work directory must not be created directly under the filesystem root")
if work == user_home:
    reject("work directory must not be the user home directory")
if work == project_root or work in project_root.parents:
    reject("work directory must not equal or contain the repository")
if any(path.suffix.lower() == ".app" for path in (work, *work.parents)):
    reject("work directory must not be inside an application bundle")
if prefix != work / "install":
    reject("prefix must be the work directory's exact install child")
if work.exists() and not work.is_dir():
    reject("existing work path must be a directory")
if prefix.exists() and not prefix.is_dir():
    reject("existing prefix must be a directory")

print(work)
print(prefix)
PY
}

usage() {
    cat <<EOF
Usage: macos/Scripts/build_release_sdl2.sh [options]

Downloads the pinned upstream SDL2 source archive, verifies its SHA-256, and
builds a standalone shared SDL2 library for the requested macOS target. The
installed pkg-config file can then be selected for build_app_bundle.sh by
prepending <prefix>/lib/pkgconfig to PKG_CONFIG_PATH.

Options:
  --work-dir PATH       Source/build/cache directory
                        (default: build-macos-deps/sdl2-${MDKR_RELEASE_SDL2_VERSION})
  --prefix PATH         Installation prefix
                        (default: <work-dir>/install)
  --archive PATH        Use an already-downloaded source archive (still hashed)
  --arch ARCH           native, arm64, or x86_64 (default: arm64)
  --deployment-target V Minimum macOS version (default: 13.0)
  --validate-paths-only Validate work/prefix safety and exit without writing
  -h, --help            Show this help
EOF
}

PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
WORK_DIR=""
PREFIX=""
ARCH="arm64"
DEPLOYMENT_TARGET="13.0"
ARCHIVE=""
VALIDATE_PATHS_ONLY=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --work-dir)
            [[ $# -ge 2 ]] || die "--work-dir requires a path"
            WORK_DIR="$2"
            shift 2
            ;;
        --prefix)
            [[ $# -ge 2 ]] || die "--prefix requires a path"
            PREFIX="$2"
            shift 2
            ;;
        --archive)
            [[ $# -ge 2 ]] || die "--archive requires a path"
            ARCHIVE="$2"
            shift 2
            ;;
        --arch)
            [[ $# -ge 2 ]] || die "--arch requires a value"
            ARCH="$2"
            shift 2
            ;;
        --deployment-target)
            [[ $# -ge 2 ]] || die "--deployment-target requires a value"
            DEPLOYMENT_TARGET="$2"
            shift 2
            ;;
        --validate-paths-only) VALIDATE_PATHS_ONLY=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[[ "${DEPLOYMENT_TARGET}" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] ||
    die "deployment target must look like 13.0 or 13.0.1"
case "${ARCH}" in
    native) CMAKE_ARCH="$(uname -m)" ;;
    arm64|x86_64) CMAKE_ARCH="${ARCH}" ;;
    *) die "--arch must be native, arm64, or x86_64" ;;
esac

if [[ -z "${WORK_DIR}" ]]; then
    WORK_DIR="${PROJECT_ROOT}/build-macos-deps/sdl2-${MDKR_RELEASE_SDL2_VERSION}"
elif [[ "${WORK_DIR}" != /* ]]; then
    WORK_DIR="${PROJECT_ROOT}/${WORK_DIR}"
fi
if [[ -z "${PREFIX}" ]]; then
    PREFIX="${WORK_DIR}/install"
elif [[ "${PREFIX}" != /* ]]; then
    PREFIX="${PROJECT_ROOT}/${PREFIX}"
fi
if [[ -n "${ARCHIVE}" && "${ARCHIVE}" != /* ]]; then
    ARCHIVE="${PROJECT_ROOT}/${ARCHIVE}"
fi

command -v python3 >/dev/null 2>&1 || die "required tool not found: python3"
if ! SAFE_PATHS="$(canonical_sdl_paths "${WORK_DIR}" "${PREFIX}" "${PROJECT_ROOT}")"; then
    die "refusing unsafe SDL2 work directory or prefix"
fi
[[ "${SAFE_PATHS}" == *$'\n'* ]] || die "could not canonicalize SDL2 paths"
WORK_DIR="${SAFE_PATHS%%$'\n'*}"
PREFIX="${SAFE_PATHS#*$'\n'}"
if [[ "${VALIDATE_PATHS_ONLY}" == true ]]; then
    printf 'Safe SDL2 work directory: %s\nSafe SDL2 prefix: %s\n' \
        "${WORK_DIR}" "${PREFIX}"
    exit 0
fi

for tool in cmake curl shasum tar otool lipo; do
    command -v "${tool}" >/dev/null 2>&1 || die "required tool not found: ${tool}"
done

DOWNLOAD_DIR="${WORK_DIR}/download"
if [[ -z "${ARCHIVE}" ]]; then
    ARCHIVE="${DOWNLOAD_DIR}/SDL2-${MDKR_RELEASE_SDL2_VERSION}.tar.gz"
    mkdir -p "${DOWNLOAD_DIR}"
    if [[ ! -f "${ARCHIVE}" ]]; then
        printf 'Downloading SDL2 %s from %s\n' \
            "${MDKR_RELEASE_SDL2_VERSION}" "${MDKR_RELEASE_SDL2_URL}"
        curl --fail --location --retry 3 --silent --show-error \
            "${MDKR_RELEASE_SDL2_URL}" --output "${ARCHIVE}"
    fi
fi
[[ -f "${ARCHIVE}" ]] || die "source archive not found: ${ARCHIVE}"

ACTUAL_SHA="$(shasum -a 256 "${ARCHIVE}" | awk '{print $1}')"
[[ "${ACTUAL_SHA}" == "${MDKR_RELEASE_SDL2_SOURCE_SHA256}" ]] ||
    die "SDL2 archive SHA-256 mismatch: expected" \
        "${MDKR_RELEASE_SDL2_SOURCE_SHA256}, got ${ACTUAL_SHA}"

# Never trust or reuse an extracted source tree or prior object files. Each run
# starts from the authenticated archive in a private, precisely scoped
# temporary directory; the downloaded archive remains cached in WORK_DIR.
mkdir -p "${WORK_DIR}"
RUN_DIR="$(mktemp -d "${WORK_DIR}/run.XXXXXX")"
cleanup() { rm -rf "${RUN_DIR}"; }
trap cleanup EXIT
SOURCE_PARENT="${RUN_DIR}/source"
SOURCE_DIR="${SOURCE_PARENT}/SDL2-${MDKR_RELEASE_SDL2_VERSION}"
BUILD_DIR="${RUN_DIR}/build"
mkdir -p "${SOURCE_PARENT}"
tar -xzf "${ARCHIVE}" -C "${SOURCE_PARENT}"
[[ -f "${SOURCE_DIR}/CMakeLists.txt" ]] ||
    die "authenticated archive did not contain" \
        "SDL2-${MDKR_RELEASE_SDL2_VERSION}/CMakeLists.txt"

# SDL uses __FILE__ in a few renderer diagnostics. Normalize those macros (and
# any debug paths emitted by a future build-type change) so the public dylib is
# independent of the maintainer or CI checkout location.
PATH_MAP_FLAGS="-ffile-prefix-map=${SOURCE_DIR}=SDL2-${MDKR_RELEASE_SDL2_VERSION}"
PATH_MAP_FLAGS+=" -fmacro-prefix-map=${SOURCE_DIR}=SDL2-${MDKR_RELEASE_SDL2_VERSION}"
PATH_MAP_FLAGS+=" -fdebug-prefix-map=${SOURCE_DIR}=SDL2-${MDKR_RELEASE_SDL2_VERSION}"
cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_OSX_ARCHITECTURES="${CMAKE_ARCH}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
    "-DCMAKE_C_FLAGS=${PATH_MAP_FLAGS}" \
    "-DCMAKE_CXX_FLAGS=${PATH_MAP_FLAGS}" \
    "-DCMAKE_OBJC_FLAGS=${PATH_MAP_FLAGS}" \
    "-DCMAKE_OBJCXX_FLAGS=${PATH_MAP_FLAGS}" \
    -DSDL_SHARED=ON \
    -DSDL_STATIC=OFF \
    -DSDL_TEST=OFF \
    -DSDL_TESTS=OFF \
    -DSDL2_DISABLE_INSTALL=OFF
cmake --build "${BUILD_DIR}" --target install --parallel "$(sysctl -n hw.ncpu)"

DYLIB="${PREFIX}/lib/libSDL2-2.0.0.dylib"
PC_FILE="${PREFIX}/lib/pkgconfig/sdl2.pc"
LICENSE_FILE="${PREFIX}/share/licenses/SDL2/LICENSE.txt"
[[ -f "${DYLIB}" ]] || die "installed SDL2 dylib is missing: ${DYLIB}"
[[ -f "${PC_FILE}" ]] || die "installed SDL2 pkg-config file is missing: ${PC_FILE}"
[[ -f "${LICENSE_FILE}" ]] || die "installed SDL2 license is missing: ${LICENSE_FILE}"
ACTUAL_LICENSE_SHA="$(shasum -a 256 "${LICENSE_FILE}" | awk '{print $1}')"
[[ "${ACTUAL_LICENSE_SHA}" == "${MDKR_RELEASE_SDL2_LICENSE_SHA256}" ]] ||
    die "installed SDL2 license hash mismatch"

MINOS="$(otool -l "${DYLIB}" | awk '
    $1 == "cmd" && $2 == "LC_BUILD_VERSION" { in_version = 1; next }
    in_version && $1 == "minos" { print $2; exit }
    $1 == "cmd" && $2 == "LC_VERSION_MIN_MACOSX" { in_legacy = 1; next }
    in_legacy && $1 == "version" { print $2; exit }
')"
[[ "${MINOS}" == "${DEPLOYMENT_TARGET}" ]] ||
    die "installed SDL2 minimum is ${MINOS:-unknown}, expected ${DEPLOYMENT_TARGET}"

ARCHS="$(lipo -archs "${DYLIB}")"
[[ " ${ARCHS} " == *" ${CMAKE_ARCH} "* ]] ||
    die "installed SDL2 lacks ${CMAKE_ARCH}: ${ARCHS}"
if otool -L "${DYLIB}" | tail -n +2 | awk '{print $1}' |
        grep -Evq '^(@rpath/libSDL2|/System/|/usr/lib/)'; then
    otool -L "${DYLIB}" >&2
    die "installed SDL2 has a non-system runtime dependency"
fi
if grep -aFq 'sdl2-compat:' "${DYLIB}"; then
    die "installed library is sdl2-compat, not standalone SDL2"
fi
if grep -aFq "${RUN_DIR}" "${DYLIB}" ||
        grep -aFq "${PROJECT_ROOT}" "${DYLIB}"; then
    die "installed SDL2 contains an absolute developer/build path"
fi

# Record the exact authenticated input and installed binary bytes. Packaging
# verifies this before copying; it then records the post-signature bundled hash
# so the final app can attest the dylib it actually contains.
DYLIB_SHA256="$(shasum -a 256 "${DYLIB}" | awk '{print $1}')"
MANIFEST_DIR="${PREFIX}/share/mdkr64"
MANIFEST_PATH="${MANIFEST_DIR}/SDL2-release-manifest.txt"
mkdir -p "${MANIFEST_DIR}"
printf 'version=%s\nsource_sha256=%s\ndylib_sha256=%s\n' \
    "${MDKR_RELEASE_SDL2_VERSION}" \
    "${MDKR_RELEASE_SDL2_SOURCE_SHA256}" \
    "${DYLIB_SHA256}" >"${MANIFEST_PATH}"

printf 'build_release_sdl2: PASS — SDL2 %s, %s, min macOS %s\n' \
    "${MDKR_RELEASE_SDL2_VERSION}" "${ARCHS}" "${MINOS}"
printf 'Use for packaging:\n  PKG_CONFIG_PATH=%q macos/Scripts/build_app_bundle.sh ...\n' \
    "${PREFIX}/lib/pkgconfig"
