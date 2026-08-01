#!/bin/bash
# Submit a signed .app or .dmg to Apple's notary service, staple, and assess it.

set -euo pipefail

info() { printf '[INFO]  %s\n' "$*"; }
die()  { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

TARGET_PATH="${1:-}"
[[ -n "${TARGET_PATH}" ]] || die "Usage: $0 <signed.app|signed.dmg>"
[[ -e "${TARGET_PATH}" ]] || die "Artifact not found: ${TARGET_PATH}"

case "${TARGET_PATH}" in
    *.app) ARTIFACT_KIND="app" ;;
    *.dmg) ARTIFACT_KIND="dmg" ;;
    *) die "Only signed .app and .dmg artifacts can be notarized" ;;
esac

for tool in xcrun codesign python3 ditto spctl; do
    command -v "${tool}" >/dev/null 2>&1 || die "Required tool not found: ${tool}"
done

# App Store Connect API keys are preferred for CI. Apple-ID credentials remain
# supported for a maintainer's local keychain/app-password workflow.
NOTARY_AUTH=()
if [[ -n "${APPLE_API_KEY_PATH:-}" || -n "${APPLE_API_KEY_ID:-}" ||
      -n "${APPLE_API_ISSUER_ID:-}" ]]; then
    : "${APPLE_API_KEY_PATH:?Set APPLE_API_KEY_PATH}"
    : "${APPLE_API_KEY_ID:?Set APPLE_API_KEY_ID}"
    : "${APPLE_API_ISSUER_ID:?Set APPLE_API_ISSUER_ID}"
    [[ -f "${APPLE_API_KEY_PATH}" ]] || die "API private key not found"
    NOTARY_AUTH=(
        --key "${APPLE_API_KEY_PATH}"
        --key-id "${APPLE_API_KEY_ID}"
        --issuer "${APPLE_API_ISSUER_ID}"
    )
    AUTH_LABEL="App Store Connect API key"
else
    : "${APPLE_ID:?Set APPLE_ID or the APPLE_API_* variables}"
    : "${APPLE_TEAM_ID:?Set APPLE_TEAM_ID}"
    : "${APPLE_APP_PASSWORD:?Set APPLE_APP_PASSWORD}"
    NOTARY_AUTH=(
        --apple-id "${APPLE_ID}"
        --team-id "${APPLE_TEAM_ID}"
        --password "${APPLE_APP_PASSWORD}"
    )
    AUTH_LABEL="Apple ID app password"
fi

TARGET_PATH="$(cd "$(dirname "${TARGET_PATH}")" && pwd)/$(basename "${TARGET_PATH}")"
TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mdkr-notary.XXXXXX")"
trap 'rm -rf "${TEMP_DIR}"' EXIT

SUBMISSION_PATH="${TARGET_PATH}"
if [[ "${ARTIFACT_KIND}" == "app" ]]; then
    SUBMISSION_PATH="${TEMP_DIR}/$(basename "${TARGET_PATH}" .app).zip"
    info "Creating notarization ZIP with ditto..."
    ditto -c -k --keepParent "${TARGET_PATH}" "${SUBMISSION_PATH}" ||
        die "Failed to create notarization ZIP"
fi

info "Submitting $(basename "${TARGET_PATH}") with ${AUTH_LABEL}..."
set +e
SUBMISSION_OUTPUT="$(xcrun notarytool submit "${SUBMISSION_PATH}" \
    "${NOTARY_AUTH[@]}" --wait --output-format json 2>&1)"
SUBMISSION_RC=$?
set -e
printf '%s\n' "${SUBMISSION_OUTPUT}"

STATUS="$(printf '%s' "${SUBMISSION_OUTPUT}" | python3 -c \
    'import json,sys
try:
    print(json.load(sys.stdin).get("status", ""))
except Exception:
    print("")')"
SUBMISSION_ID="$(printf '%s' "${SUBMISSION_OUTPUT}" | python3 -c \
    'import json,sys
try:
    print(json.load(sys.stdin).get("id", ""))
except Exception:
    print("")')"

if [[ ${SUBMISSION_RC} -ne 0 || "${STATUS}" != "Accepted" ]]; then
    if [[ -n "${SUBMISSION_ID}" ]]; then
        info "Fetching rejection log for ${SUBMISSION_ID}..."
        xcrun notarytool log "${SUBMISSION_ID}" "${NOTARY_AUTH[@]}" 2>&1 || true
    fi
    die "Notarization was not Accepted (status=${STATUS:-unreadable}, exit=${SUBMISSION_RC})"
fi

info "Stapling and validating notarization ticket..."
xcrun stapler staple "${TARGET_PATH}" || die "Failed to staple ticket"
xcrun stapler validate "${TARGET_PATH}" || die "Stapled ticket validation failed"

if [[ "${ARTIFACT_KIND}" == "app" ]]; then
    "$(dirname "${BASH_SOURCE[0]}")/verify_gatekeeper_bundle.sh" \
        --distribution "${TARGET_PATH}"
else
    codesign --verify --strict --verbose=2 "${TARGET_PATH}" ||
        die "DMG signature verification failed after stapling"
    SPCTL_OUTPUT="$(spctl --assess --type open \
        --context context:primary-signature --verbose=4 \
        "${TARGET_PATH}" 2>&1)" || {
        printf '%s\n' "${SPCTL_OUTPUT}" >&2
        die "Gatekeeper rejected the notarized DMG"
    }
fi

info "Notarization complete and Gatekeeper accepted the ${ARTIFACT_KIND}."
