#!/bin/bash
# Sign a macOS app inside-out with Developer ID and optionally notarize it.

set -euo pipefail

info() { printf '[INFO]  %s\n' "$*"; }
die()  { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

SKIP_NOTARIZE=false
APP_PATH=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-notarize) SKIP_NOTARIZE=true; shift ;;
        -*) die "Unknown option: $1" ;;
        *)
            [[ -z "${APP_PATH}" ]] || die "Unexpected argument: $1"
            APP_PATH="$1"
            shift
            ;;
    esac
done

[[ -n "${APP_PATH}" ]] || die "Usage: $0 <path-to.app> [--skip-notarize]"
[[ -d "${APP_PATH}" ]] || die "App bundle not found: ${APP_PATH}"
: "${DEVELOPER_ID_APPLICATION:?Set DEVELOPER_ID_APPLICATION to the exact certificate identity}"
case "${DEVELOPER_ID_APPLICATION}" in
    "Developer ID Application: "*) ;;
    *) die "DEVELOPER_ID_APPLICATION must name a Developer ID Application certificate" ;;
esac

for tool in codesign security plutil shasum; do
    command -v "${tool}" >/dev/null 2>&1 || die "Required tool not found: ${tool}"
done

APP_PATH="$(cd "$(dirname "${APP_PATH}")" && pwd)/$(basename "${APP_PATH}")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENTITLEMENTS_PATH="${SCRIPT_DIR}/../Resources/Entitlements.plist"
[[ -f "${ENTITLEMENTS_PATH}" ]] || die "Entitlements file not found: ${ENTITLEMENTS_PATH}"
plutil -lint "${ENTITLEMENTS_PATH}" >/dev/null || die "Entitlements plist is invalid"

IDENTITIES="$(security find-identity -v -p codesigning 2>&1)"
printf '%s\n' "${IDENTITIES}" | grep -Fq -- "\"${DEVELOPER_ID_APPLICATION}\"" ||
    die "Developer ID Application identity is not available in the active keychains"

info "Signing nested code with: ${DEVELOPER_ID_APPLICATION}"
FRAMEWORKS_DIR="${APP_PATH}/Contents/Frameworks"
if [[ -d "${FRAMEWORKS_DIR}" ]]; then
    while IFS= read -r dylib; do
        [[ -n "${dylib}" ]] || continue
        info "Signing dylib: $(basename "${dylib}")"
        codesign --force --sign "${DEVELOPER_ID_APPLICATION}" \
            --options runtime --timestamp "${dylib}" ||
            die "Failed to sign nested dylib: ${dylib}"
    done < <(find "${FRAMEWORKS_DIR}" -depth -type f -name '*.dylib' -print)

    while IFS= read -r framework; do
        [[ -n "${framework}" ]] || continue
        info "Signing framework: $(basename "${framework}")"
        codesign --force --sign "${DEVELOPER_ID_APPLICATION}" \
            --options runtime --timestamp "${framework}" ||
            die "Failed to sign framework: ${framework}"
    done < <(find "${FRAMEWORKS_DIR}" -depth -type d -name '*.framework' -print)
fi

# Developer ID replaces the nested dylib's ad-hoc signature and therefore its
# full-file SHA-256. Refresh the sealed provenance row after nested signing but
# before the outer resource seal is created. This is static identity
# attestation; the signed lane must still run its own post-sign runtime smoke.
SDL2_MANIFEST="${APP_PATH}/Contents/Resources/ThirdParty/SDL2-MANIFEST.txt"
if [[ -f "${SDL2_MANIFEST}" ]]; then
    shopt -s nullglob
    SDL2_DYLIBS=("${APP_PATH}"/Contents/Frameworks/libSDL2*.dylib)
    shopt -u nullglob
    [[ "${#SDL2_DYLIBS[@]}" -eq 1 ]] ||
        die "SDL2 provenance requires exactly one bundled SDL2 dylib"
    SDL2_SIGNED_SHA256="$(shasum -a 256 "${SDL2_DYLIBS[0]}" | awk '{print $1}')"
    SDL2_MANIFEST_TMP="${SDL2_MANIFEST}.tmp.$$"
    awk -F= -v hash="${SDL2_SIGNED_SHA256}" '
        BEGIN { found = 0 }
        $1 == "bundled_dylib_sha256" {
            print "bundled_dylib_sha256=" hash
            found++
            next
        }
        { print }
        END { if (found != 1) exit 2 }
    ' "${SDL2_MANIFEST}" >"${SDL2_MANIFEST_TMP}" || {
        rm -f "${SDL2_MANIFEST_TMP}"
        die "SDL2 provenance manifest has no unique bundled hash row"
    }
    mv "${SDL2_MANIFEST_TMP}" "${SDL2_MANIFEST}"
fi

# Do not use --deep for signing. It can hide missed or incorrectly ordered
# nested code. Nested code is signed explicitly above; this final operation
# signs the main executable and seals the bundle resources.
info "Signing outer app with Hardened Runtime..."
codesign --force --sign "${DEVELOPER_ID_APPLICATION}" \
    --options runtime --timestamp \
    --entitlements "${ENTITLEMENTS_PATH}" \
    "${APP_PATH}" || die "Developer ID app signing failed"

codesign --verify --deep --strict --verbose=4 "${APP_PATH}" ||
    die "Developer ID signature verification failed"

if [[ "${SKIP_NOTARIZE}" == true ]]; then
    info "Notarization skipped for this local signing run."
else
    "${SCRIPT_DIR}/notarize_artifact.sh" "${APP_PATH}"
fi

info "Signing complete: ${APP_PATH}"
codesign -dvvv "${APP_PATH}" 2>&1 |
    grep -E 'Authority|TeamIdentifier|Signature|Runtime Version' || true
