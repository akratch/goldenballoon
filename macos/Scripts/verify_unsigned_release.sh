#!/usr/bin/env bash
# Verify a no-Developer-ID macOS release candidate end to end.
#
# "Unsigned" in release notes means no trusted Developer ID identity and no
# notarization. The app must still carry an ad-hoc integrity signature after
# its load paths are rewritten; otherwise Apple-silicon Gatekeeper may report
# the bundle as damaged instead of giving the expected unidentified-developer
# warning.

set -euo pipefail

die() {
    printf 'verify_unsigned_release: FAIL — %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: macos/Scripts/verify_unsigned_release.sh --version VER --commit SHA APP.app

Requires an ad-hoc-sealed, asset-free app with the requested compiled/plist
version and source commit. Launches the ROM-free shell smoke with no renderer
override through LaunchServices, proves WebGPU is the default and reaches a
real surface present, and rejects SDL3/Homebrew runtime loads.
EOF
}

VERSION=""
COMMIT=""
APP_PATH=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) [[ $# -ge 2 ]] || die "--version requires a value"; VERSION="$2"; shift 2 ;;
        --commit) [[ $# -ge 2 ]] || die "--commit requires a value"; COMMIT="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option: $1" ;;
        *) [[ -z "${APP_PATH}" ]] || die "unexpected argument: $1"; APP_PATH="$1"; shift ;;
    esac
done

[[ "${VERSION}" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] || die "--version is required"
[[ "${COMMIT}" =~ ^[0-9a-fA-F]{40}$ ]] || die "--commit must be a full commit SHA"
[[ -d "${APP_PATH}" ]] || die "app bundle not found: ${APP_PATH}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The dynamic absolute path keeps sourcing independent of the caller's CWD.
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/release_sdl2_config.sh"
INFO_PLIST="${APP_PATH}/Contents/Info.plist"
[[ -f "${INFO_PLIST}" ]] || die "missing Info.plist"
CONTROLLER_DB="${APP_PATH}/Contents/Resources/gamecontrollerdb.txt"
[[ -s "${CONTROLLER_DB}" ]] ||
    die "bundled game-controller mapping database is missing or empty"
APP_ICON="${APP_PATH}/Contents/Resources/AppIcon.icns"
[[ -s "${APP_ICON}" ]] || die "bundled branded app icon is missing or empty"
EXECUTABLE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "${INFO_PLIST}" 2>/dev/null || true)"
EXECUTABLE="${APP_PATH}/Contents/MacOS/${EXECUTABLE_NAME}"
[[ -x "${EXECUTABLE}" ]] || die "main executable is missing or not executable"
SDL2_LICENSE="${APP_PATH}/Contents/Resources/ThirdParty/SDL2-LICENSE.txt"
[[ -f "${SDL2_LICENSE}" ]] || die "bundled SDL2 license is missing"
SDL2_LICENSE_SHA256="$(shasum -a 256 "${SDL2_LICENSE}" | awk '{print $1}')"
[[ "${SDL2_LICENSE_SHA256}" == "${MDKR_RELEASE_SDL2_LICENSE_SHA256}" ]] ||
    die "bundled SDL2 license does not match the pinned" \
        "SDL2 ${MDKR_RELEASE_SDL2_VERSION} notice"
SDL2_MANIFEST="${APP_PATH}/Contents/Resources/ThirdParty/SDL2-MANIFEST.txt"
[[ -f "${SDL2_MANIFEST}" ]] || die "bundled SDL2 provenance manifest is missing"
[[ "$(wc -l <"${SDL2_MANIFEST}" | tr -d '[:space:]')" == "4" ]] ||
    die "bundled SDL2 provenance manifest must contain exactly four rows"
grep -Fqx "version=${MDKR_RELEASE_SDL2_VERSION}" "${SDL2_MANIFEST}" ||
    die "bundled SDL2 version does not match the pinned release version"
grep -Fqx "source_sha256=${MDKR_RELEASE_SDL2_SOURCE_SHA256}" "${SDL2_MANIFEST}" ||
    die "bundled SDL2 source provenance does not match the authenticated archive"
SDL2_INPUT_SHA256="$(awk -F= '$1 == "input_dylib_sha256" { print $2 }' "${SDL2_MANIFEST}")"
SDL2_BUNDLED_SHA256="$(awk -F= '$1 == "bundled_dylib_sha256" { print $2 }' "${SDL2_MANIFEST}")"
[[ "${SDL2_INPUT_SHA256}" =~ ^[0-9a-f]{64}$ ]] ||
    die "bundled SDL2 input provenance hash is not canonical"
[[ "${SDL2_BUNDLED_SHA256}" =~ ^[0-9a-f]{64}$ ]] ||
    die "bundled SDL2 final hash is not canonical"
shopt -s nullglob
SDL2_DYLIBS=("${APP_PATH}"/Contents/Frameworks/libSDL2*.dylib)
shopt -u nullglob
[[ "${#SDL2_DYLIBS[@]}" -eq 1 ]] ||
    die "expected exactly one bundled SDL2 dylib, found ${#SDL2_DYLIBS[@]}"
SDL2_ACTUAL_SHA256="$(shasum -a 256 "${SDL2_DYLIBS[0]}" | awk '{print $1}')"
[[ "${SDL2_ACTUAL_SHA256}" == "${SDL2_BUNDLED_SHA256}" ]] ||
    die "final bundled SDL2 dylib does not match its sealed SHA-256 provenance"
SDL2_SIGNATURE_DETAILS="$(codesign -dvvv "${SDL2_DYLIBS[0]}" 2>&1)"
printf '%s\n' "${SDL2_SIGNATURE_DETAILS}" | grep -Fq 'Signature=adhoc' ||
    die "bundled SDL2 dylib is not ad-hoc integrity signed"
if printf '%s\n' "${SDL2_SIGNATURE_DETAILS}" | grep -Fq 'Authority='; then
    die "bundled SDL2 dylib unexpectedly carries a trusted signing authority"
fi

"${SCRIPT_DIR}/verify_gatekeeper_bundle.sh" \
    --expected-arch arm64 \
    --expected-min-os 13.0 \
    "${APP_PATH}"
"${SCRIPT_DIR}/verify_asset_free.sh" "${APP_PATH}"

SIGNATURE_DETAILS="$(codesign -dvvv "${APP_PATH}" 2>&1)"
printf '%s\n' "${SIGNATURE_DETAILS}" | grep -Fq 'Signature=adhoc' ||
    die "app is not ad-hoc integrity signed"
if printf '%s\n' "${SIGNATURE_DETAILS}" | grep -Fq 'Authority='; then
    die "app unexpectedly carries a trusted signing authority"
fi

PLIST_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "${INFO_PLIST}")"
[[ "${PLIST_VERSION}" == "${VERSION}" ]] ||
    die "Info.plist version ${PLIST_VERSION} does not match ${VERSION}"
PLIST_BUILD_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "${INFO_PLIST}")"
[[ "${PLIST_BUILD_VERSION}" == "${VERSION}" ]] ||
    die "Info.plist build version ${PLIST_BUILD_VERSION} does not match ${VERSION}"
BINARY_VERSION="$("${EXECUTABLE}" --version 2>&1)"
[[ "${BINARY_VERSION}" == "mdkr64 ${VERSION}" ]] ||
    die "binary reports '${BINARY_VERSION}', expected 'mdkr64 ${VERSION}'"
grep -aFq "${COMMIT}" "${EXECUTABLE}" ||
    die "compiled About-panel provenance does not contain commit ${COMMIT}"

# A locked macOS console intentionally vends no CAMetalLayer drawable:
# NSWindow loses NSWindowOcclusionStateVisible and wgpu-native reports
# WGPUSurfaceGetCurrentTextureStatus_Occluded. Refuse to start the strict smoke
# in that state so maintainers get an actionable prerequisite instead of a
# misleading capture-only result (which is never accepted as a present).
CONSOLE_SESSION_STATE="$(/usr/sbin/ioreg -n Root -d1 2>/dev/null || true)"
# Avoid a grep -q pipeline here. With pipefail enabled, grep can close the pipe
# after its first match and make printf report SIGPIPE, turning a real match
# into a false condition when ioreg returns the session as one long line.
if grep -Eqi '"CGSSessionScreenIsLocked"[[:space:]]*=[[:space:]]*(Yes|true|1)' \
        <<<"${CONSOLE_SESSION_STATE}"; then
    die "console session is locked; unlock it before strict WebGPU surface-present verification (capture-only proof is not accepted)"
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mdkr-unsigned-verify.XXXXXX")"
cleanup() { rm -rf "${WORK_DIR}"; }
trap cleanup EXIT
SCHEMA_LOG="${WORK_DIR}/schema.log"
DEFAULTS_LOG="${WORK_DIR}/defaults.log"
SMOKE_BMP="${WORK_DIR}/launcher.bmp"
SMOKE_LOG="${WORK_DIR}/launcher.log"
SMOKE_STDOUT="${WORK_DIR}/launcher.stdout.log"
SMOKE_STDERR="${WORK_DIR}/launcher.stderr.log"
PREFS_DIR="${WORK_DIR}/prefs"
mkdir -p "${PREFS_DIR}"
LAUNCH_TIMEOUT_SECONDS="${MDKR_RELEASE_LAUNCH_TIMEOUT_SECONDS:-60}"
[[ "${LAUNCH_TIMEOUT_SECONDS}" =~ ^[1-9][0-9]*$ ]] ||
    die "MDKR_RELEASE_LAUNCH_TIMEOUT_SECONDS must be a positive integer"
(( LAUNCH_TIMEOUT_SECONDS <= 600 )) ||
    die "MDKR_RELEASE_LAUNCH_TIMEOUT_SECONDS must not exceed 600"

# Prove the packaged binary's semantic contract and clean defaults directly,
# then use the rendered smoke below to prove the frame-rate controls remain
# visible and separate from the gameplay-changing cadence setting.
MDKR_APP_DUMP_SCHEMA=1 MDKR_AUDIO=0 "${EXECUTABLE}" \
    >"${SCHEMA_LOG}" 2>&1 || die "packaged settings schema self-check failed"
# kFrameLimitHelp contains both ' and ", so the expected line is carried in a
# quoted heredoc rather than a quoted string. check_ci_contract.py regenerates
# it from platform/app/ui_settings.cpp and fails if the two disagree.
frame_limit_contract=$(cat <<'FRAME_LIMIT_CONTRACT'
[app] frame-limit UI contract: recommended="Original (recommended)" group="Higher refresh rates" caveat="Original presents each authored image once. Higher rates repeat authored images when smoothing is Off, or create in-between images when it is Interpolated. Gameplay speed does not change. Higher rates can use more CPU and GPU time. Rates above your display's refresh need a display connection that can drop an image it has not shown yet. Where the system does not offer one, they present at your display's refresh instead, unless Allow Tearing is on. A European 50 Hz game is worth pairing Match Display with Interpolated: its authored image lasts 40 ms, which no whole number of 60 Hz refreshes fits, so Original holds it for two refreshes and then three and the motion ripples. Match Display with Interpolated removes that without changing game speed, music pitch, or timers. Just Under Display is for a display with a variable refresh rate: it paces a few Hz below the top of that range, which keeps the display adapting to the game rather than falling back to a fixed refresh, and it re-reads the rate if you move the window to another monitor. 40 Hz is a battery-friendly choice on a handheld whose display runs at 40 or 120 Hz, where every image is held for the same length of time. Uncapped removes the native limit only when new interpolated images are available; held frames stay display-paced. A browser always maps Uncapped and Just Under Display to Match Display."
FRAME_LIMIT_CONTRACT
)
grep -Fq "${frame_limit_contract}" "${SCHEMA_LOG}" \
    || die "packaged frame-limit guidance drifted"

mkdir -p "${WORK_DIR}/defaults-prefs"
MDKR_VIDEO_CONFIG_PATH="${WORK_DIR}/defaults.ini" \
MDKR_APP_PREFS_DIR="${WORK_DIR}/defaults-prefs" \
MDKR_SAVE_DIR="${WORK_DIR}/defaults-save" \
MDKR_AUDIO=0 "${EXECUTABLE}" --video-list \
    >"${DEFAULTS_LOG}" 2>&1 || die "packaged video-default self-check failed"
grep -Eq '^  Gameplay\.SimulationCadence[[:space:]]+original[[:space:]]+\[default\]$' \
    "${DEFAULTS_LOG}" || die "packaged gameplay cadence is not Original by default"
grep -Eq '^  Video\.FrameLimit[[:space:]]+original[[:space:]]+\[default\]$' \
    "${DEFAULTS_LOG}" || die "packaged frame delivery is not Original by default"
grep -Eq '^  Video\.MotionSmoothing[[:space:]]+off[[:space:]]+\[default\]$' \
    "${DEFAULTS_LOG}" || die "packaged motion smoothing is not Off by default"
grep -Eq '^  Video\.Mode[[:space:]]+restored[[:space:]]+\[default\]$' \
    "${DEFAULTS_LOG}" || die "packaged presentation is not Restored by default"

if "${SCRIPT_DIR}/run_launchservices_probe.py" \
        --timeout "${LAUNCH_TIMEOUT_SECONDS}" \
        --executable "${EXECUTABLE}" \
        --stdout "${SMOKE_STDOUT}" \
        --stderr "${SMOKE_STDERR}" \
        --work-dir "${WORK_DIR}" \
        --env MDKR_APP_SMOKE_FRAMES=4 \
        --env "MDKR_APP_SMOKE_SHOT=${SMOKE_BMP}" \
        --env MDKR_APP_PANEL=Settings \
        --env MDKR_APP_UI_TRACE=1 \
        --env MDKR_APP_REQUIRE_PRESENT=1 \
        --env "MDKR_APP_PREFS_DIR=${PREFS_DIR}" \
        --env "MDKR_VIDEO_CONFIG_PATH=${WORK_DIR}/mdkr64.ini" \
        --env "MDKR_SAVE_DIR=${WORK_DIR}/save" \
        --env MDKR_AUDIO=0 \
        "${APP_PATH}"; then
    :
else
    LAUNCH_STATUS=$?
    [[ -f "${SMOKE_STDOUT}" ]] && tail -n 40 "${SMOKE_STDOUT}" >&2
    [[ -f "${SMOKE_STDERR}" ]] && tail -n 80 "${SMOKE_STDERR}" >&2
    die "LaunchServices ROM-free launcher smoke failed (status ${LAUNCH_STATUS})"
fi
{
    [[ ! -f "${SMOKE_STDOUT}" ]] || cat "${SMOKE_STDOUT}"
    [[ ! -f "${SMOKE_STDERR}" ]] || cat "${SMOKE_STDERR}"
} >"${SMOKE_LOG}"

grep -Fq '[app] host: WebGPU' "${SMOKE_LOG}" || {
    tail -n 80 "${SMOKE_LOG}" >&2
    die "launcher did not select WebGPU by default"
}
grep -Fq '[app-ui] settings action=play restartPending=0' "${SMOKE_LOG}" ||
    die "launcher did not render the default Settings action"
grep -Fq '[app-ui] frame-rate-controls visible=1 gameplay-accuracy-separated=1' \
    "${SMOKE_LOG}" ||
    die "launcher did not render the visible frame-rate controls"
# Anchored: an unanchored substring search for the Original frame limit also
# matches a hypothetical value such as `original-uncapped`, so the packaged
# default would still read as proven after the default had changed. The trace
# always prints a ` label=` field next, so the value must end right there.
grep -Eq '^\[app-ui\] frame-limit value=original[[:space:]]' "${SMOKE_LOG}" ||
    die "launcher did not expose the Original frame-limit default"
grep -Eq '^\[app\] smoke: rendered 4 frames, drawable [1-9][0-9]*x[1-9][0-9]*' \
    "${SMOKE_LOG}" || die "launcher smoke did not render four drawable frames"
SMOKE_PRESENT_COUNT="$(grep -Ec '^\[app\] smoke: surface presents=' "${SMOKE_LOG}" || true)"
[[ "${SMOKE_PRESENT_COUNT}" == "1" ]] ||
    die "launcher emitted ${SMOKE_PRESENT_COUNT} smoke present rows; expected exactly one"
SMOKE_PRESENT_ROW="$(grep -E '^\[app\] smoke: surface presents=' "${SMOKE_LOG}")"
SMOKE_PRESENT_RE='^\[app\] smoke: surface presents=([1-9][0-9]*)$'
[[ "${SMOKE_PRESENT_ROW}" =~ ${SMOKE_PRESENT_RE} ]] || {
    tail -n 80 "${SMOKE_LOG}" >&2
    die "launcher smoke did not prove a WebGPU surface present"
}
SMOKE_PRESENTED_FRAMES="${BASH_REMATCH[1]}"
TELEMETRY_COUNT="$(grep -Ec '^\[APP-WGPU-PRESENT\]' "${SMOKE_LOG}" || true)"
[[ "${TELEMETRY_COUNT}" == "1" ]] ||
    die "launcher emitted ${TELEMETRY_COUNT} AppHost telemetry rows; expected exactly one"
TELEMETRY_ROW="$(grep -E '^\[APP-WGPU-PRESENT\]' "${SMOKE_LOG}")"
TELEMETRY_RE='^\[APP-WGPU-PRESENT\][[:space:]]attempts=([0-9]+)[[:space:]]presented=([0-9]+)[[:space:]]unavailable=([0-9]+)[[:space:]]lastStatus=(-?[0-9]+)[[:space:]]encodeFailures=([0-9]+)[[:space:]]captureRequests=([0-9]+)[[:space:]]captureFailures=([0-9]+)$'
[[ "${TELEMETRY_ROW}" =~ ${TELEMETRY_RE} ]] ||
    die "launcher AppHost telemetry was not a complete, canonical row"
PRESENT_ATTEMPTS="${BASH_REMATCH[1]}"
PRESENTED_FRAMES="${BASH_REMATCH[2]}"
UNAVAILABLE_FRAMES="${BASH_REMATCH[3]}"
LAST_SURFACE_STATUS="${BASH_REMATCH[4]}"
ENCODE_FAILURES="${BASH_REMATCH[5]}"
CAPTURE_REQUESTS="${BASH_REMATCH[6]}"
CAPTURE_FAILURES="${BASH_REMATCH[7]}"
(( PRESENT_ATTEMPTS == PRESENTED_FRAMES + UNAVAILABLE_FRAMES )) ||
    die "launcher presentation attempts do not equal presented + unavailable"
(( PRESENTED_FRAMES >= 4 )) ||
    die "launcher telemetry did not prove all four required surface presents"
(( SMOKE_PRESENTED_FRAMES == PRESENTED_FRAMES )) ||
    die "launcher smoke and shutdown telemetry disagree on surface presents"
# A just-activated LaunchServices window may initially be occluded while the
# compositor attaches its CAMetalLayer. Bound that startup allowance tightly;
# it is not permission for an indefinitely unavailable surface.
(( UNAVAILABLE_FRAMES <= 64 )) ||
    die "launcher exceeded the bounded 64-attempt unavailable-surface allowance"
[[ "${LAST_SURFACE_STATUS}" == "1" || "${LAST_SURFACE_STATUS}" == "2" ]] ||
    die "launcher ended on non-success WebGPU surface status ${LAST_SURFACE_STATUS}"
(( ENCODE_FAILURES == 0 && CAPTURE_REQUESTS == 1 && CAPTURE_FAILURES == 0 )) ||
    die "launcher shutdown telemetry reported a rendering or capture failure"
[[ -s "${SMOKE_BMP}" ]] || die "launcher smoke did not produce its pixel capture"

# LaunchServices may strip DYLD_* variables. Keep a separate direct execution
# whose only purpose is dyld's loaded-image audit; the Finder-equivalent smoke
# above remains the authoritative activation, graphics, capture, and shutdown
# proof.
RUNTIME_LOAD_LOG="${WORK_DIR}/runtime-load.log"
(
    while IFS='=' read -r name _; do
        case "${name}" in MDKR*|GE007_*) unset "${name}" ;; esac
    done < <(env)
    export DYLD_PRINT_LIBRARIES=1
    "${EXECUTABLE}" --version
) >"${RUNTIME_LOAD_LOG}" 2>&1 || die "direct runtime-load audit failed"
if grep -Eq '(/opt/homebrew|/usr/local/(opt|Cellar)|libSDL3[^/]*\.dylib)' "${RUNTIME_LOAD_LOG}"; then
    grep -E '(/opt/homebrew|/usr/local/(opt|Cellar)|libSDL3[^/]*\.dylib)' "${RUNTIME_LOAD_LOG}" >&2
    die "launcher loaded a Homebrew or SDL3 runtime dependency"
fi

printf 'verify_unsigned_release: PASS — version %s, commit %.12s, ad-hoc integrity, standalone SDL2, WebGPU default\n' \
    "${VERSION}" "${COMMIT}"
