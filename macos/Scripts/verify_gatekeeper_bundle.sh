#!/bin/bash
# Verify the integrity and distribution trust of a macOS app bundle.

set -euo pipefail

die() {
    printf 'verify_gatekeeper_bundle: FAIL — %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: macos/Scripts/verify_gatekeeper_bundle.sh [--distribution] APP.app

Default mode proves that all nested code and the outer resource seal are
internally valid. --distribution additionally requires Developer ID + Hardened
Runtime, a valid stapled notarization ticket, and Gatekeeper acceptance.
EOF
}

DISTRIBUTION=false
APP_PATH=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --distribution) DISTRIBUTION=true; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option: $1" ;;
        *)
            [[ -z "${APP_PATH}" ]] || die "unexpected argument: $1"
            APP_PATH="$1"
            shift
            ;;
    esac
done

[[ -n "${APP_PATH}" ]] || { usage >&2; exit 1; }
[[ -d "${APP_PATH}" ]] || die "app bundle not found: ${APP_PATH}"

for tool in codesign otool plutil xattr /usr/libexec/PlistBuddy; do
    command -v "${tool}" >/dev/null 2>&1 || die "required tool not found: ${tool}"
done

INFO_PLIST="${APP_PATH}/Contents/Info.plist"
[[ -f "${INFO_PLIST}" ]] || die "missing Contents/Info.plist"
plutil -lint "${INFO_PLIST}" >/dev/null || die "invalid Info.plist"
EXECUTABLE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "${INFO_PLIST}" 2>/dev/null || true)"
[[ -n "${EXECUTABLE_NAME}" ]] || die "CFBundleExecutable is missing"
EXECUTABLE="${APP_PATH}/Contents/MacOS/${EXECUTABLE_NAME}"
[[ -x "${EXECUTABLE}" ]] || die "main executable is missing or not executable: ${EXECUTABLE}"

if xattr -lr "${APP_PATH}" 2>/dev/null | grep -Fq 'com.apple.quarantine'; then
    die "source bundle already carries quarantine metadata"
fi

VERIFY_OUTPUT="$(codesign --verify --deep --strict --verbose=4 "${APP_PATH}" 2>&1)" || {
    printf '%s\n' "${VERIFY_OUTPUT}" >&2
    die "nested code or the outer resource seal is invalid"
}

OUTER_DETAILS="$(codesign -dvvv "${APP_PATH}" 2>&1)"
OUTER_TEAM="$(printf '%s\n' "${OUTER_DETAILS}" | sed -n 's/^TeamIdentifier=//p' | head -n 1)"

FRAMEWORKS_DIR="${APP_PATH}/Contents/Frameworks"
HAS_BUNDLED_CODE=false
if [[ -d "${FRAMEWORKS_DIR}" ]]; then
    while IFS= read -r dylib; do
        [[ -n "${dylib}" ]] || continue
        HAS_BUNDLED_CODE=true
        codesign --verify --strict --verbose=2 "${dylib}" >/dev/null 2>&1 ||
            die "invalid nested dylib signature: ${dylib}"
        if [[ "${DISTRIBUTION}" == true ]]; then
            NESTED_DETAILS="$(codesign -dvvv "${dylib}" 2>&1)"
            NESTED_TEAM="$(printf '%s\n' "${NESTED_DETAILS}" | sed -n 's/^TeamIdentifier=//p' | head -n 1)"
            [[ -n "${OUTER_TEAM}" && "${NESTED_TEAM}" == "${OUTER_TEAM}" ]] ||
                die "nested dylib TeamIdentifier does not match the app: ${dylib}"
        fi
    done < <(find "${FRAMEWORKS_DIR}" -type f -name '*.dylib' -print)
fi

while IFS= read -r dependency; do
    [[ -n "${dependency}" ]] || continue
    case "${dependency}" in
        /System/*|/usr/lib/*|@executable_path/*|@loader_path/*|@rpath/*) ;;
        *)
            if [[ "${DISTRIBUTION}" == true ||
                  "${HAS_BUNDLED_CODE}" == true ]]; then
                die "main executable has a non-portable absolute dependency: ${dependency}"
            fi
            ;;
    esac
done < <(otool -L "${EXECUTABLE}" | tail -n +2 | awk '{print $1}')

ENTITLEMENTS="$(codesign -d --entitlements :- "${APP_PATH}" 2>/dev/null || true)"
if printf '%s\n' "${ENTITLEMENTS}" | grep -Eq \
    'com\.apple\.security\.cs\.(disable-library-validation|allow-unsigned-executable-memory)'; then
    die "bundle carries an unnecessary high-risk code-signing entitlement"
fi

if [[ "${DISTRIBUTION}" == true ]]; then
    command -v xcrun >/dev/null 2>&1 || die "xcrun is required in distribution mode"
    command -v spctl >/dev/null 2>&1 || die "spctl is required in distribution mode"
    printf '%s\n' "${OUTER_DETAILS}" | grep -Fq 'Authority=Developer ID Application:' ||
        die "outer signature is not Developer ID Application"
    [[ -n "${OUTER_TEAM}" && "${OUTER_TEAM}" != "not set" ]] ||
        die "Developer ID signature has no TeamIdentifier"
    printf '%s\n' "${OUTER_DETAILS}" | grep -Eq 'flags=.*\(.*runtime.*\)' ||
        die "Hardened Runtime is not enabled"
    xcrun stapler validate "${APP_PATH}" >/dev/null 2>&1 ||
        die "stapled notarization ticket is missing or invalid"
    SPCTL_OUTPUT="$(spctl --assess --type execute --verbose=4 "${APP_PATH}" 2>&1)" || {
        printf '%s\n' "${SPCTL_OUTPUT}" >&2
        die "Gatekeeper rejected the app"
    }
fi

MODE="integrity"
[[ "${DISTRIBUTION}" == true ]] && MODE="Developer ID + notarization"
printf 'verify_gatekeeper_bundle: PASS — %s; nested signatures, resource seal, and load paths valid\n' "${MODE}"
