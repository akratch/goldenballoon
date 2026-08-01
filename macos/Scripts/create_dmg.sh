#!/bin/bash
# create_dmg.sh — Create a DMG installer image from a macOS app bundle
#
# Entirely app-bundle-agnostic: APP_NAME and VERSION are derived from whatever
# .app it is pointed at, by reading that bundle's own Info.plist.
#
# Packages the given .app into a distributable DMG disk image. Uses the
# 'create-dmg' tool for a polished result when available, otherwise falls
# back to hdiutil for a basic DMG.
#
# Usage: ./create_dmg.sh <path-to.app> [output.dmg]
#   <path-to.app>   Path to the signed app bundle
#   [output.dmg]    Optional output path (defaults to AppName-version.dmg)

set -euo pipefail

STAGING_DIR=""
MOUNT_DIR=""
MOUNTED=false
cleanup() {
    if [[ "${MOUNTED}" == true && -n "${MOUNT_DIR}" ]]; then
        hdiutil detach "${MOUNT_DIR}" >/dev/null 2>&1 || true
    fi
    [[ -z "${STAGING_DIR}" ]] || rm -rf "${STAGING_DIR}"
    [[ -z "${MOUNT_DIR}" ]] || rm -rf "${MOUNT_DIR}"
}
trap cleanup EXIT

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

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
APP_PATH="${1:-}"
OUTPUT_DMG="${2:-}"

if [[ -z "${APP_PATH}" ]]; then
    die "Usage: $0 <path-to.app> [output.dmg]"
fi

if [[ ! -d "${APP_PATH}" ]]; then
    die "App bundle not found: ${APP_PATH}"
fi

# ---------------------------------------------------------------------------
# Check required tools
# ---------------------------------------------------------------------------
if ! command -v hdiutil &>/dev/null; then
    die "Required tool 'hdiutil' not found. This script must be run on macOS."
fi

HAS_CREATE_DMG=false
if command -v create-dmg &>/dev/null; then
    HAS_CREATE_DMG=true
    info "Found create-dmg - will use it for a polished DMG."
else
    warn "create-dmg not found - falling back to hdiutil (basic DMG)."
    warn "Install create-dmg for a nicer result: brew install create-dmg"
fi

# ---------------------------------------------------------------------------
# Resolve paths and extract metadata
# ---------------------------------------------------------------------------
APP_PATH="$(cd "$(dirname "${APP_PATH}")" && pwd)/$(basename "${APP_PATH}")"
APP_NAME="$(basename "${APP_PATH}" .app)"
INFO_PLIST="${APP_PATH}/Contents/Info.plist"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

# Determine output path. The DMG FILENAME is a slug of the brand
# ("Golden Balloon" -> "golden-balloon-1.0.0.dmg"), not the internal app name.
BRAND_SLUG="$(printf '%s' "${BRAND_NAME}" \
    | tr '[:upper:]' '[:lower:]' \
    | tr -cs 'a-z0-9' '-' \
    | sed -e 's/^-*//' -e 's/-*$//')"
[[ -n "${BRAND_SLUG}" ]] || BRAND_SLUG="${APP_NAME}"
if [[ -z "${OUTPUT_DMG}" ]]; then
    OUTPUT_DMG="$(dirname "${APP_PATH}")/${BRAND_SLUG}-${VERSION}.dmg"
fi

# Ensure output path is absolute
case "${OUTPUT_DMG}" in
    /*) ;; # already absolute
    *)  OUTPUT_DMG="$(pwd)/${OUTPUT_DMG}" ;;
esac

info "App bundle : ${APP_PATH}"
info "Brand      : ${BRAND_NAME}"
info "Version    : ${VERSION}"
info "Output DMG : ${OUTPUT_DMG}"

# Remove existing DMG if present
if [[ -f "${OUTPUT_DMG}" ]]; then
    warn "Removing existing DMG: ${OUTPUT_DMG}"
    rm -f "${OUTPUT_DMG}"
fi

# ---------------------------------------------------------------------------
# Create DMG
# ---------------------------------------------------------------------------
if [[ "${HAS_CREATE_DMG}" == true ]]; then
    info "Creating DMG with create-dmg..."

    # create-dmg returns non-zero if the DMG already exists, so we already
    # removed it above. It also returns exit code 2 when it can't set the
    # custom icon (non-fatal), so handle that.
    create-dmg \
        --volname "${BRAND_NAME} ${VERSION}" \
        --window-pos 200 120 \
        --window-size 600 400 \
        --icon-size 100 \
        --icon "${APP_NAME}.app" 150 185 \
        --app-drop-link 450 185 \
        --no-internet-enable \
        "${OUTPUT_DMG}" \
        "${APP_PATH}" \
        || {
            EXIT_CODE=$?
            # create-dmg exits 2 when it can't set a custom icon but the DMG
            # was still created successfully
            if [[ ${EXIT_CODE} -eq 2 ]] && [[ -f "${OUTPUT_DMG}" ]]; then
                warn "create-dmg exited with code 2 (cosmetic issue). DMG was created."
            else
                die "create-dmg failed with exit code ${EXIT_CODE}."
            fi
        }
else
    info "Creating DMG with hdiutil..."

    # Create a temporary directory with the app and an Applications symlink
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
fi

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
MOUNTED=true
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
hdiutil detach "${MOUNT_DIR}" >/dev/null || die "Failed to detach verification mount"
MOUNTED=false
info "Packaged app verification passed."

info "================================="
info "Done."
