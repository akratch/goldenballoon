#!/bin/bash
# Verify the integrity and distribution trust of a macOS app bundle.

set -euo pipefail

die() {
    printf 'verify_gatekeeper_bundle: FAIL — %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: macos/Scripts/verify_gatekeeper_bundle.sh [OPTIONS] APP.app

Default mode proves that all nested code and the outer resource seal are
internally valid. --distribution additionally requires Developer ID + Hardened
Runtime, a valid stapled notarization ticket, and Gatekeeper acceptance.

Options:
  --distribution          Require Developer ID and notarization.
  --expected-arch ARCH    Require exactly this architecture in every Mach-O.
  --expected-min-os VER   Require this minimum macOS version in the plist and
                          every Mach-O (numeric versions compare equivalently).
EOF
}

DISTRIBUTION=false
EXPECTED_ARCH=""
EXPECTED_MINOS=""
APP_PATH=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --distribution) DISTRIBUTION=true; shift ;;
        --expected-arch)
            [[ $# -ge 2 ]] || die "--expected-arch requires a value"
            EXPECTED_ARCH="$2"
            shift 2
            ;;
        --expected-min-os)
            [[ $# -ge 2 ]] || die "--expected-min-os requires a value"
            EXPECTED_MINOS="$2"
            shift 2
            ;;
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
[[ -z "${EXPECTED_ARCH}" || "${EXPECTED_ARCH}" =~ ^[A-Za-z0-9_]+$ ]] ||
    die "invalid expected architecture: ${EXPECTED_ARCH}"
[[ -z "${EXPECTED_MINOS}" || "${EXPECTED_MINOS}" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] ||
    die "invalid expected minimum macOS version: ${EXPECTED_MINOS}"
[[ -d "${APP_PATH}" ]] || die "app bundle not found: ${APP_PATH}"

for tool in codesign otool plutil xattr lipo /usr/libexec/PlistBuddy; do
    command -v "${tool}" >/dev/null 2>&1 || die "required tool not found: ${tool}"
done

INFO_PLIST="${APP_PATH}/Contents/Info.plist"
[[ -f "${INFO_PLIST}" ]] || die "missing Contents/Info.plist"
plutil -lint "${INFO_PLIST}" >/dev/null || die "invalid Info.plist"
EXECUTABLE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "${INFO_PLIST}" 2>/dev/null || true)"
[[ -n "${EXECUTABLE_NAME}" ]] || die "CFBundleExecutable is missing"
EXECUTABLE="${APP_PATH}/Contents/MacOS/${EXECUTABLE_NAME}"
[[ -x "${EXECUTABLE}" ]] || die "main executable is missing or not executable: ${EXECUTABLE}"
DECLARED_MINOS="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "${INFO_PLIST}" 2>/dev/null || true)"
[[ "${DECLARED_MINOS}" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] ||
    die "LSMinimumSystemVersion is missing or invalid"

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
MAIN_ARCHS="$(lipo -archs "${EXECUTABLE}" 2>/dev/null || true)"
[[ -n "${MAIN_ARCHS}" ]] || die "could not determine main executable architectures"

version_gt() {
    local lhs="$1" rhs="$2" lhs_parts rhs_parts
    IFS=. read -r -a lhs_parts <<< "${lhs}"
    IFS=. read -r -a rhs_parts <<< "${rhs}"
    for i in 0 1 2; do
        local l="${lhs_parts[$i]:-0}" r="${rhs_parts[$i]:-0}"
        if ((10#$l > 10#$r)); then return 0; fi
        if ((10#$l < 10#$r)); then return 1; fi
    done
    return 1
}

version_eq() {
    ! version_gt "$1" "$2" && ! version_gt "$2" "$1"
}

if [[ -n "${EXPECTED_MINOS}" ]] &&
        ! version_eq "${DECLARED_MINOS}" "${EXPECTED_MINOS}"; then
    die "Info.plist declares macOS ${DECLARED_MINOS}, expected ${EXPECTED_MINOS}"
fi

if [[ -n "${EXPECTED_ARCH}" && "${MAIN_ARCHS}" != "${EXPECTED_ARCH}" ]]; then
    die "main executable architectures '${MAIN_ARCHS}', expected exactly ${EXPECTED_ARCH}"
fi

mach_o_minos() {
    otool -l "$1" 2>/dev/null | awk '
        $1 == "cmd" && $2 == "LC_BUILD_VERSION" { in_version = 1; next }
        in_version && $1 == "minos" { print $2; exit }
        $1 == "cmd" && $2 == "LC_VERSION_MIN_MACOSX" { in_legacy = 1; next }
        in_legacy && $1 == "version" { print $2; exit }
    '
}

verify_mach_o_contract() {
    local code_path="$1" label="$2" code_minos code_archs arch install_name dependency resolved
    code_minos="$(mach_o_minos "${code_path}")"
    [[ -n "${code_minos}" ]] || die "could not read minimum macOS version: ${label}"
    if [[ -n "${EXPECTED_MINOS}" ]] &&
            ! version_eq "${code_minos}" "${EXPECTED_MINOS}"; then
        die "${label} targets macOS ${code_minos}, expected ${EXPECTED_MINOS}"
    elif version_gt "${code_minos}" "${DECLARED_MINOS}"; then
        die "${label} requires macOS ${code_minos}, newer than declared ${DECLARED_MINOS}"
    fi

    code_archs="$(lipo -archs "${code_path}" 2>/dev/null || true)"
    if [[ -n "${EXPECTED_ARCH}" ]]; then
        [[ "${code_archs}" == "${EXPECTED_ARCH}" ]] ||
            die "${label} architectures '${code_archs}', expected exactly ${EXPECTED_ARCH}"
    else
        for arch in ${MAIN_ARCHS}; do
            [[ " ${code_archs} " == *" ${arch} "* ]] ||
                die "${label} lacks required ${arch} architecture"
        done
    fi

    install_name="$(otool -D "${code_path}" 2>/dev/null | tail -n 1 || true)"
    while IFS= read -r dependency; do
        [[ -n "${dependency}" ]] || continue
        [[ -n "${install_name}" && "${dependency}" == "${install_name}" ]] && continue
        case "${dependency}" in
            /System/*|/usr/lib/*) ;;
            @executable_path/*)
                resolved="${APP_PATH}/Contents/MacOS/${dependency#@executable_path/}"
                [[ -e "${resolved}" ]] || die "${label} has an unresolved dependency: ${dependency}"
                ;;
            @loader_path/*)
                resolved="$(dirname "${code_path}")/${dependency#@loader_path/}"
                [[ -e "${resolved}" ]] || die "${label} has an unresolved dependency: ${dependency}"
                ;;
            @rpath/*)
                die "${label} has an unresolved @rpath dependency: ${dependency}"
                ;;
            *)
                if [[ "${DISTRIBUTION}" == true || "${HAS_BUNDLED_CODE}" == true ]]; then
                    die "${label} has a non-portable absolute dependency: ${dependency}"
                fi
                ;;
        esac
    done < <(otool -L "${code_path}" | tail -n +2 | awk '{print $1}')
}

if [[ -d "${FRAMEWORKS_DIR}" ]]; then
    while IFS= read -r dylib; do
        [[ -n "${dylib}" ]] || continue
        HAS_BUNDLED_CODE=true
        codesign --verify --strict --verbose=2 "${dylib}" >/dev/null 2>&1 ||
            die "invalid nested dylib signature: ${dylib}"
        if grep -aFq 'sdl2-compat:' "${dylib}"; then
            die "bundled SDL2 is the SDL3-loading sdl2-compat shim: ${dylib}"
        fi
        if [[ "${DISTRIBUTION}" == true ]]; then
            NESTED_DETAILS="$(codesign -dvvv "${dylib}" 2>&1)"
            NESTED_TEAM="$(printf '%s\n' "${NESTED_DETAILS}" | sed -n 's/^TeamIdentifier=//p' | head -n 1)"
            [[ -n "${OUTER_TEAM}" && "${NESTED_TEAM}" == "${OUTER_TEAM}" ]] ||
                die "nested dylib TeamIdentifier does not match the app: ${dylib}"
        fi
    done < <(find "${FRAMEWORKS_DIR}" -type f -name '*.dylib' -print)
fi

verify_mach_o_contract "${EXECUTABLE}" "main executable"
if [[ -d "${FRAMEWORKS_DIR}" ]]; then
    while IFS= read -r dylib; do
        [[ -n "${dylib}" ]] || continue
        verify_mach_o_contract "${dylib}" "nested dylib $(basename "${dylib}")"
    done < <(find "${FRAMEWORKS_DIR}" -type f -name '*.dylib' -print)
fi

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
