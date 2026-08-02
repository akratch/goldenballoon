#!/bin/bash
# create_dmg.sh — Create a DMG installer image from a macOS app bundle
#
# Entirely app-bundle-agnostic: APP_NAME and VERSION are derived from whatever
# .app it is pointed at, by reading that bundle's own Info.plist.
#
# Packages the given .app into a fixed-layout distributable DMG disk image with
# the macOS system hdiutil. Release behavior must never vary according to an
# unpinned third-party executable found on PATH; image metadata is not claimed
# to be byte-for-byte reproducible.
#
# Usage: ./create_dmg.sh [--validate-output-only] <path-to.app> [output.dmg]
#   <path-to.app>   Path to the sealed app bundle
#   [output.dmg]    Optional output path (defaults to AppName-version.dmg)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/dmg_mount_cleanup.sh"

STAGING_DIR=""
MOUNT_DIR=""
cleanup() {
    local status=$?
    trap - EXIT INT TERM HUP
    if [[ -n "${MOUNT_DIR}" ]] && ! mdkr_detach_dmg_mount "${MOUNT_DIR}"; then
        printf 'create_dmg: cleanup could not detach %s\n' "${MOUNT_DIR}" >&2
        if (( status == 0 )); then
            status=1
        fi
    fi
    [[ -z "${STAGING_DIR}" ]] || rm -rf -- "${STAGING_DIR}"
    # Never recursively remove a live mount point. After a confirmed detach,
    # the mktemp mount directory is empty and rmdir is the narrowest cleanup.
    if [[ -n "${MOUNT_DIR}" ]] &&
            ! mdkr_remove_detached_mount_dir "${MOUNT_DIR}"; then
        printf 'create_dmg: leaving live mount point for inspection: %s\n' \
            "${MOUNT_DIR}" >&2
    fi
    exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# ---------------------------------------------------------------------------
# Color helpers
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()    { printf "${GREEN}[INFO]${NC}  %s\n" "$*"; }
warn()    { printf "${YELLOW}[WARN]${NC}  %s\n" "$*"; }
error()   { printf "${RED}[ERROR]${NC} %s\n" "$*" >&2; }

die() {
    error "$@"
    exit 1
}

canonical_dmg_output() {
    python3 - "$1" "$2" <<'PY'
import os
import pathlib
import sys


def reject(reason: str) -> None:
    print(f"unsafe DMG output path: {reason}", file=sys.stderr)
    raise SystemExit(2)


raw = pathlib.Path(os.path.abspath(os.path.expanduser(sys.argv[1])))
app = pathlib.Path(sys.argv[2]).resolve(strict=True)
if raw.is_symlink():
    reject("the output itself must not be a symbolic link")
if raw.exists() and not raw.is_file():
    reject("an existing output must be a regular file")

candidate = raw.resolve(strict=False)
home = pathlib.Path.home().resolve(strict=True)
if candidate in (pathlib.Path("/"), home, app):
    reject(f"refusing broad or source target {candidate}")
if candidate.parent == pathlib.Path("/"):
    reject("output must not be created directly under the filesystem root")
if candidate.suffix.lower() != ".dmg" or candidate.name.lower() == ".dmg":
    reject("target must have a nonempty name ending in .dmg")
if app in candidate.parents:
    reject("output must not be written inside the source app bundle")

print(candidate)
PY
}

canonical_source_app() {
    python3 - "$1" <<'PY'
import os
import pathlib
import plistlib
import sys


def reject(reason: str) -> None:
    print(f"unsafe source app: {reason}", file=sys.stderr)
    raise SystemExit(2)


raw = pathlib.Path(os.path.abspath(os.path.expanduser(sys.argv[1])))
if raw.is_symlink():
    reject("the source .app itself must not be a symbolic link")
try:
    candidate = raw.resolve(strict=True)
except FileNotFoundError:
    reject("bundle does not exist")
if not candidate.is_dir() or candidate.suffix != ".app":
    reject("source must be a real directory ending in .app")

plist_path = candidate / "Contents" / "Info.plist"
if plist_path.is_symlink() or not plist_path.is_file():
    reject("bundle has no regular Contents/Info.plist")
try:
    with plist_path.open("rb") as stream:
        plist = plistlib.load(stream)
except (OSError, plistlib.InvalidFileException):
    reject("Contents/Info.plist is invalid")

executable_name = plist.get("CFBundleExecutable")
if (
    not isinstance(executable_name, str)
    or not executable_name
    or pathlib.PurePath(executable_name).name != executable_name
):
    reject("CFBundleExecutable is missing or unsafe")
executable = candidate / "Contents" / "MacOS" / executable_name
if executable.is_symlink() or not executable.is_file():
    reject("bundle executable is missing, non-regular, or a symlink")

print(candidate)
PY
}

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
VALIDATE_OUTPUT_ONLY=false
if [[ "${1:-}" == "--validate-output-only" ]]; then
    VALIDATE_OUTPUT_ONLY=true
    shift
fi

APP_PATH="${1:-}"
OUTPUT_DMG="${2:-}"

if [[ -z "${APP_PATH}" ]]; then
    die "Usage: $0 <path-to.app> [output.dmg]"
fi

command -v python3 >/dev/null 2>&1 || die "Required tool 'python3' not found."
if ! SAFE_APP_PATH="$(canonical_source_app "${APP_PATH}")"; then
    die "Refusing unsafe source app: ${APP_PATH}"
fi
APP_PATH="${SAFE_APP_PATH}"

if [[ "${VALIDATE_OUTPUT_ONLY}" == true ]]; then
    [[ -n "${OUTPUT_DMG}" ]] ||
        die "--validate-output-only requires an explicit output.dmg"
    command -v python3 >/dev/null 2>&1 || die "Required tool 'python3' not found."
    if ! SAFE_OUTPUT_DMG="$(canonical_dmg_output "${OUTPUT_DMG}" "${APP_PATH}")"; then
        die "Refusing unsafe DMG output target: ${OUTPUT_DMG}"
    fi
    info "Safe DMG output: ${SAFE_OUTPUT_DMG}"
    exit 0
fi

# ---------------------------------------------------------------------------
# Check required tools
# ---------------------------------------------------------------------------
command -v hdiutil >/dev/null 2>&1 ||
    die "Required tool 'hdiutil' not found. This script must be run on macOS."

# ---------------------------------------------------------------------------
# Resolve paths and extract metadata
# ---------------------------------------------------------------------------
APP_NAME="$(basename "${APP_PATH}" .app)"
INFO_PLIST="${APP_PATH}/Contents/Info.plist"

VERIFY_DISTRIBUTION=false
if codesign -dvvv "${APP_PATH}" 2>&1 |
        grep -Fq 'Authority=Developer ID Application:'; then
    VERIFY_DISTRIBUTION=true
fi
if [[ "${VERIFY_DISTRIBUTION}" == true ]]; then
    "${SCRIPT_DIR}/verify_gatekeeper_bundle.sh" \
        --distribution "${APP_PATH}" ||
        die "Source app failed Gatekeeper bundle verification"
else
    "${SCRIPT_DIR}/verify_gatekeeper_bundle.sh" "${APP_PATH}" ||
        die "Source app failed Gatekeeper bundle verification"
fi

# Player-facing brand name. The .app is still built as mdkr64.app (the internal
# name), so the bundle BASENAME must not drive anything a user reads. Take the
# display name from the bundle's own Info.plist (CFBundleName, set to the
# product brand by build_app_bundle.sh) and fall back to the basename only when
# the plist has none -- the script stays app-bundle-agnostic either way.
BRAND_NAME="${APP_NAME}"

# Try to extract version from Info.plist
VERSION="unknown"
if [[ -f "${INFO_PLIST}" ]] && command -v /usr/libexec/PlistBuddy &>/dev/null; then
    VERSION="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "${INFO_PLIST}" 2>/dev/null || echo "")"
    if [[ -z "${VERSION}" ]]; then
        VERSION="$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "${INFO_PLIST}" 2>/dev/null || echo "unknown")"
    fi
    info "Detected version: ${VERSION}"
    PLIST_BRAND="$(/usr/libexec/PlistBuddy -c "Print :CFBundleName" "${INFO_PLIST}" 2>/dev/null || echo "")"
    if [[ -n "${PLIST_BRAND}" ]]; then
        BRAND_NAME="${PLIST_BRAND}"
    fi
    info "Detected brand  : ${BRAND_NAME}"
else
    warn "Could not read version from Info.plist. Using 'unknown'."
fi

# Determine output path. The DMG filename is the canonical, case-preserving
# brand stem ("Golden Balloon" -> "Golden-Balloon-1.2.3.dmg"), not the
# internal app name.
BRAND_FILE_STEM="$(printf '%s' "${BRAND_NAME}" \
    | tr -cs 'A-Za-z0-9' '-' \
    | sed -e 's/^-*//' -e 's/-*$//')"
[[ -n "${BRAND_FILE_STEM}" ]] || BRAND_FILE_STEM="${APP_NAME}"
if [[ -z "${OUTPUT_DMG}" ]]; then
    OUTPUT_DMG="$(dirname "${APP_PATH}")/${BRAND_FILE_STEM}-${VERSION}.dmg"
fi

# Canonicalize and narrow the exact file before the replacement below. Public
# callers may choose the directory, but never a broad target, source bundle,
# symlink, non-DMG name, or filesystem-root child.
if ! SAFE_OUTPUT_DMG="$(canonical_dmg_output "${OUTPUT_DMG}" "${APP_PATH}")"; then
    die "Refusing unsafe DMG output target: ${OUTPUT_DMG}"
fi
OUTPUT_DMG="${SAFE_OUTPUT_DMG}"

info "App bundle : ${APP_PATH}"
info "Brand      : ${BRAND_NAME}"
info "Version    : ${VERSION}"
info "Output DMG : ${OUTPUT_DMG}"

# Remove existing DMG if present
if [[ -f "${OUTPUT_DMG}" ]]; then
    warn "Removing existing DMG: ${OUTPUT_DMG}"
    rm -f -- "${OUTPUT_DMG}"
fi

# ---------------------------------------------------------------------------
# Create DMG
# ---------------------------------------------------------------------------
info "Creating DMG with the system hdiutil..."

# Create a temporary directory with the app and an Applications symlink.
STAGING_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dmg-staging.XXXXXX")"

cp -R "${APP_PATH}" "${STAGING_DIR}/"
ln -s /Applications "${STAGING_DIR}/Applications"

VOLUME_NAME="${BRAND_NAME} ${VERSION}"

hdiutil create \
    -volname "${VOLUME_NAME}" \
    -srcfolder "${STAGING_DIR}" \
    -ov \
    -format UDZO \
    -imagekey zlib-level=9 \
    "${OUTPUT_DMG}" \
    || die "hdiutil create failed."

# ---------------------------------------------------------------------------
# Verify and print summary
# ---------------------------------------------------------------------------
if [[ ! -f "${OUTPUT_DMG}" ]]; then
    die "DMG was not created. Something went wrong."
fi

echo ""
info "========== DMG Summary =========="
info "Output : ${OUTPUT_DMG}"
info "Size   : $(du -h "${OUTPUT_DMG}" | cut -f1)"

# Verify the DMG can be mounted
info "Verifying DMG integrity..."
hdiutil verify "${OUTPUT_DMG}" >/dev/null \
    || die "DMG integrity verification failed"
info "DMG integrity check passed."

info "Mounting DMG read-only and re-verifying the packaged app..."
MOUNT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mdkr-dmg-mount.XXXXXX")"
hdiutil attach -readonly -nobrowse -mountpoint "${MOUNT_DIR}" \
    "${OUTPUT_DMG}" >/dev/null || die "DMG could not be mounted"
PACKAGED_APP="${MOUNT_DIR}/${APP_NAME}.app"
[[ -d "${PACKAGED_APP}" ]] || die "Mounted DMG is missing ${APP_NAME}.app"
if [[ "${VERIFY_DISTRIBUTION}" == true ]]; then
    "${SCRIPT_DIR}/verify_gatekeeper_bundle.sh" \
        --distribution "${PACKAGED_APP}" ||
        die "App inside mounted DMG failed verification"
else
    "${SCRIPT_DIR}/verify_gatekeeper_bundle.sh" "${PACKAGED_APP}" ||
        die "App inside mounted DMG failed verification"
fi
mdkr_detach_dmg_mount "${MOUNT_DIR}" || die "Failed to detach verification mount"
info "Packaged app verification passed."

info "================================="
info "Done."
