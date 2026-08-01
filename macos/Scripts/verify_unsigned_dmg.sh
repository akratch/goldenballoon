#!/usr/bin/env bash
# Qualify the exact app copied into an intentionally unsigned release DMG.

set -euo pipefail

die() {
    printf 'verify_unsigned_dmg: FAIL — %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: macos/Scripts/verify_unsigned_dmg.sh --version VER --commit SHA FILE.dmg

Mounts FILE.dmg read-only, requires exactly one app bundle plus an Applications
symlink, and runs the full unsigned LaunchServices/WebGPU qualification against
that mounted copy.
EOF
}

VERSION=""
COMMIT=""
DMG_PATH=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) [[ $# -ge 2 ]] || die "--version requires a value"; VERSION="$2"; shift 2 ;;
        --commit) [[ $# -ge 2 ]] || die "--commit requires a value"; COMMIT="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option: $1" ;;
        *) [[ -z "${DMG_PATH}" ]] || die "unexpected argument: $1"; DMG_PATH="$1"; shift ;;
    esac
done

[[ "${VERSION}" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] || die "--version is required"
[[ "${COMMIT}" =~ ^[0-9a-fA-F]{40}$ ]] || die "--commit must be a full commit SHA"
[[ -f "${DMG_PATH}" ]] || die "DMG not found: ${DMG_PATH}"
command -v hdiutil >/dev/null 2>&1 || die "hdiutil is required"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/dmg_mount_cleanup.sh"
MOUNT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mdkr-unsigned-dmg.XXXXXX")"
cleanup() {
    local status=$?
    trap - EXIT INT TERM HUP
    if ! mdkr_detach_dmg_mount "${MOUNT_DIR}"; then
        printf 'verify_unsigned_dmg: cleanup could not detach %s\n' \
            "${MOUNT_DIR}" >&2
        if (( status == 0 )); then
            status=1
        fi
    fi
    # Never recursively remove a live mount point. After a confirmed detach,
    # this mktemp directory is empty and rmdir is the narrowest safe cleanup.
    if ! mdkr_remove_detached_mount_dir "${MOUNT_DIR}"; then
        printf 'verify_unsigned_dmg: leaving live mount point for inspection: %s\n' \
            "${MOUNT_DIR}" >&2
    fi
    exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

hdiutil verify "${DMG_PATH}" >/dev/null || die "DMG integrity verification failed"
hdiutil attach -readonly -nobrowse -mountpoint "${MOUNT_DIR}" \
    "${DMG_PATH}" >/dev/null || die "DMG could not be mounted read-only"

shopt -s nullglob
APPS=("${MOUNT_DIR}"/*.app)
shopt -u nullglob
[[ "${#APPS[@]}" -eq 1 ]] ||
    die "mounted DMG must contain exactly one app bundle; found ${#APPS[@]}"
[[ -d "${APPS[0]}" && ! -L "${APPS[0]}" ]] ||
    die "mounted app must be a physical bundle inside the DMG"

APPLICATIONS_LINK="${MOUNT_DIR}/Applications"
[[ -L "${APPLICATIONS_LINK}" ]] ||
    die "mounted DMG must contain an Applications symlink"
[[ "$(readlink "${APPLICATIONS_LINK}")" == "/Applications" ]] ||
    die "Applications symlink must target /Applications"

ROOT_ENTRY_COUNT=0
while IFS= read -r -d '' entry; do
    case "$(basename "${entry}")" in
        "$(basename "${APPS[0]}")"|Applications) ;;
        *) die "mounted DMG contains unexpected root entry: $(basename "${entry}")" ;;
    esac
    ROOT_ENTRY_COUNT=$((ROOT_ENTRY_COUNT + 1))
done < <(find "${MOUNT_DIR}" -mindepth 1 -maxdepth 1 -print0)
[[ "${ROOT_ENTRY_COUNT}" -eq 2 ]] ||
    die "mounted DMG must contain exactly the app and Applications symlink"

"${SCRIPT_DIR}/verify_unsigned_release.sh" \
    --version "${VERSION}" --commit "${COMMIT}" "${APPS[0]}"

mdkr_detach_dmg_mount "${MOUNT_DIR}" || die "failed to detach verification mount"
printf 'verify_unsigned_dmg: PASS — mounted app passed LaunchServices/WebGPU qualification\n'
