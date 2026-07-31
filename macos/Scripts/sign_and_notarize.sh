#!/bin/bash
# sign_and_notarize.sh — Code sign and notarize a macOS app bundle
#
# Operates purely on Contents/Frameworks and the .app itself: frameworks and
# dylibs must be signed BEFORE the bundle is signed --deep, or the outer
# signature is invalidated by the inner ones. Entitlements are picked up from
# macos/Resources/Entitlements.plist via this script's own relative path.
#
# Signs the app with a Developer ID certificate, optionally submits it to
# Apple's notary service, waits for approval, and staples the ticket.
#
# Usage: ./sign_and_notarize.sh <path-to.app> [--skip-notarize]
#
# Required environment variables:
#   DEVELOPER_ID_APPLICATION  — Signing identity, e.g. "Developer ID Application: Name (TEAMID)"
#   APPLE_ID                  — Apple ID email (for notarization)
#   APPLE_TEAM_ID             — 10-character Apple Team ID
#   APPLE_APP_PASSWORD        — App-specific password or @keychain:label reference

set -euo pipefail

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

step() {
    printf "\n${GREEN}==> Step %s: %s${NC}\n" "$1" "$2"
}

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
SKIP_NOTARIZE=false
# AUDIT-0070: when notarizing, a rejected notarization or a failed Gatekeeper
# (spctl) assessment means the artifact is NOT distributable -- fail closed rather
# than print "Signing Complete". --allow-spctl-fail opts out for local/dev signing.
ALLOW_SPCTL_FAIL=false
APP_PATH=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-notarize)    SKIP_NOTARIZE=true; shift ;;
        --allow-spctl-fail) ALLOW_SPCTL_FAIL=true; shift ;;
        -*)              die "Unknown option: $1" ;;
        *)
            if [[ -z "${APP_PATH}" ]]; then
                APP_PATH="$1"; shift
            else
                die "Unexpected argument: $1"
            fi
            ;;
    esac
done

if [[ -z "${APP_PATH}" ]]; then
    die "Usage: $0 <path-to.app> [--skip-notarize] [--allow-spctl-fail]"
fi

if [[ ! -d "${APP_PATH}" ]]; then
    die "App bundle not found: ${APP_PATH}"
fi

# ---------------------------------------------------------------------------
# Check required tools
# ---------------------------------------------------------------------------
for tool in codesign xcrun zip; do
    if ! command -v "$tool" &>/dev/null; then
        die "Required tool '${tool}' not found."
    fi
done

# ---------------------------------------------------------------------------
# Check required environment variables
# ---------------------------------------------------------------------------
: "${DEVELOPER_ID_APPLICATION:?Set DEVELOPER_ID_APPLICATION to your signing identity}"

if [[ "${SKIP_NOTARIZE}" == false ]]; then
    : "${APPLE_ID:?Set APPLE_ID for notarization}"
    : "${APPLE_TEAM_ID:?Set APPLE_TEAM_ID for notarization}"
    : "${APPLE_APP_PASSWORD:?Set APPLE_APP_PASSWORD (app-specific password or @keychain:label)}"
fi

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
APP_PATH="$(cd "$(dirname "${APP_PATH}")" && pwd)/$(basename "${APP_PATH}")"
APP_NAME="$(basename "${APP_PATH}" .app)"
ENTITLEMENTS_PATH="$(dirname "${BASH_SOURCE[0]}")/../Resources/Entitlements.plist"

info "App bundle : ${APP_PATH}"
info "App name   : ${APP_NAME}"
info "Identity   : ${DEVELOPER_ID_APPLICATION}"
info "Notarize   : $(${SKIP_NOTARIZE} && echo 'SKIPPED' || echo 'YES')"

# If no entitlements file exists, create a minimal one in a temp location
if [[ ! -f "${ENTITLEMENTS_PATH}" ]]; then
    warn "Entitlements file not found at ${ENTITLEMENTS_PATH}"
    ENTITLEMENTS_PATH="$(mktemp "${TMPDIR:-/tmp}/entitlements.XXXXXX.plist")"
    cat > "${ENTITLEMENTS_PATH}" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.cs.allow-jit</key>
    <true/>
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key>
    <true/>
    <key>com.apple.security.cs.disable-library-validation</key>
    <true/>
</dict>
</plist>
PLIST
    info "Created minimal entitlements at ${ENTITLEMENTS_PATH}"
fi

# ---------------------------------------------------------------------------
# Step 1 — Sign embedded frameworks
# ---------------------------------------------------------------------------
step 1 "Sign embedded frameworks and libraries"

FRAMEWORKS_DIR="${APP_PATH}/Contents/Frameworks"

if [[ -d "${FRAMEWORKS_DIR}" ]]; then
    # Sign each framework
    find "${FRAMEWORKS_DIR}" -name '*.framework' -type d | while read -r fw; do
        info "Signing framework: $(basename "${fw}")"
        codesign --force --sign "${DEVELOPER_ID_APPLICATION}" \
            --options runtime \
            --timestamp \
            "${fw}" \
            || die "Failed to sign framework: ${fw}"
    done

    # Sign any standalone dylibs
    find "${FRAMEWORKS_DIR}" -name '*.dylib' -type f | while read -r dylib; do
        info "Signing dylib: $(basename "${dylib}")"
        codesign --force --sign "${DEVELOPER_ID_APPLICATION}" \
            --options runtime \
            --timestamp \
            "${dylib}" \
            || die "Failed to sign dylib: ${dylib}"
    done
else
    info "No Frameworks directory found - skipping embedded framework signing."
fi

# ---------------------------------------------------------------------------
# Step 2 — Sign the main app bundle
# ---------------------------------------------------------------------------
step 2 "Sign the main app bundle"

codesign --force --sign "${DEVELOPER_ID_APPLICATION}" \
    --options runtime \
    --entitlements "${ENTITLEMENTS_PATH}" \
    --timestamp \
    --deep \
    "${APP_PATH}" \
    || die "Code signing failed."

info "App signed successfully."

# ---------------------------------------------------------------------------
# Step 3 — Verify signature
# ---------------------------------------------------------------------------
step 3 "Verify code signature"

codesign --verify --deep --strict --verbose=2 "${APP_PATH}" \
    || die "Signature verification failed."

info "Signature verification passed."

# ---------------------------------------------------------------------------
# Step 4 — Notarize (unless skipped)
# ---------------------------------------------------------------------------
if [[ "${SKIP_NOTARIZE}" == true ]]; then
    warn "Notarization skipped (--skip-notarize)."
else
    step 4 "Submit for notarization"

    ZIP_PATH="${TMPDIR:-/tmp}/${APP_NAME}-notarize.zip"
    rm -f "${ZIP_PATH}"

    info "Creating ZIP for upload..."
    ditto -c -k --keepParent "${APP_PATH}" "${ZIP_PATH}" \
        || die "Failed to create ZIP archive."

    info "ZIP created: ${ZIP_PATH} ($(du -h "${ZIP_PATH}" | cut -f1))"

    info "Submitting to Apple notary service..."
    SUBMISSION_OUTPUT="$(xcrun notarytool submit "${ZIP_PATH}" \
        --apple-id "${APPLE_ID}" \
        --team-id "${APPLE_TEAM_ID}" \
        --password "${APPLE_APP_PASSWORD}" \
        --wait 2>&1)" \
        || {
            error "Notarization submission failed."
            echo "${SUBMISSION_OUTPUT}"

            # Try to extract the submission ID and fetch the log
            SUBMISSION_ID="$(echo "${SUBMISSION_OUTPUT}" | grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -1)"
            if [[ -n "${SUBMISSION_ID}" ]]; then
                error "Fetching notarization log for submission ${SUBMISSION_ID}..."
                xcrun notarytool log "${SUBMISSION_ID}" \
                    --apple-id "${APPLE_ID}" \
                    --team-id "${APPLE_TEAM_ID}" \
                    --password "${APPLE_APP_PASSWORD}" \
                    2>&1 || true
            fi
            exit 1
        }

    echo "${SUBMISSION_OUTPUT}"

    # Check if notarization was accepted
    if echo "${SUBMISSION_OUTPUT}" | grep -qi "accepted"; then
        info "Notarization accepted."
    elif [[ "${ALLOW_SPCTL_FAIL}" == true ]]; then
        warn "Notarization may not have been accepted (allowed by --allow-spctl-fail)."
    else
        # AUDIT-0070: a non-accepted notarization must not proceed to a "Signing
        # Complete" banner -- the bundle would be rejected by Gatekeeper for users.
        die "Notarization was not accepted; the bundle is NOT distributable. Review the output above (or pass --allow-spctl-fail for a local build)."
    fi

    # Clean up the zip
    rm -f "${ZIP_PATH}"

    # Staple the notarization ticket
    step 5 "Staple notarization ticket"

    xcrun stapler staple "${APP_PATH}" \
        || die "Failed to staple notarization ticket."

    info "Ticket stapled successfully."

    # Verify with Gatekeeper
    step 6 "Verify with Gatekeeper (spctl)"

    if spctl --assess --type execute --verbose=2 "${APP_PATH}"; then
        info "Gatekeeper (spctl) assessment passed."
    elif [[ "${ALLOW_SPCTL_FAIL}" == true ]]; then
        warn "spctl assessment did not pass (allowed by --allow-spctl-fail)."
    else
        # AUDIT-0070: Gatekeeper rejection means end users will be blocked from
        # launching the app -- fail the signing pipeline instead of masking it.
        die "spctl assessment FAILED: Gatekeeper would reject this bundle; it is NOT distributable. (Pass --allow-spctl-fail for a local build.)"
    fi
fi

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
info "========== Signing Complete =========="
info "App: ${APP_PATH}"
info "Identity: ${DEVELOPER_ID_APPLICATION}"
codesign -dvv "${APP_PATH}" 2>&1 | grep -E 'Authority|TeamIdentifier|Signature' | while read -r line; do
    info "  ${line}"
done
info "======================================"
info "Done."
