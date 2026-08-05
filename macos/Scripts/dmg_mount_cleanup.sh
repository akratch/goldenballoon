#!/usr/bin/env bash
# Shared, source-only helpers for cleaning up an exact temporary DMG mount.
#
# This file is sourced, never executed: it defines functions and runs no
# top-level command. It deliberately sets no shell options -- the sourcing
# script owns `set -euo pipefail`, and setting them here would silently change
# the caller's shell.
#
# Callers must pass a dedicated mktemp directory. Mount state is derived from
# the filesystem device at cleanup time, not from a boolean set after attach;
# that closes the signal/partial-attach window between hdiutil returning and a
# shell assignment running.

mdkr_dmg_mount_is_active() {
    local mount_dir="$1"
    local parent_dir mount_device parent_device

    [[ -n "${mount_dir}" && -d "${mount_dir}" ]] || return 1
    parent_dir="$(dirname "${mount_dir}")"
    mount_device="$(/usr/bin/stat -f '%d' "${mount_dir}" 2>/dev/null)" || return 1
    parent_device="$(/usr/bin/stat -f '%d' "${parent_dir}" 2>/dev/null)" || return 1
    [[ "${mount_device}" != "${parent_device}" ]]
}

mdkr_detach_dmg_mount() {
    local mount_dir="$1"
    local attempt

    mdkr_dmg_mount_is_active "${mount_dir}" || return 0
    for attempt in 1 2 3; do
        if hdiutil detach "${mount_dir}" >/dev/null 2>&1 &&
                ! mdkr_dmg_mount_is_active "${mount_dir}"; then
            return 0
        fi
        if (( attempt < 3 )); then
            sleep 1
        fi
    done
    # The caller created and mounted this exact read-only temporary directory.
    # Force is limited to that target and follows bounded ordinary retries.
    if hdiutil detach -force "${mount_dir}" >/dev/null 2>&1 &&
            ! mdkr_dmg_mount_is_active "${mount_dir}"; then
        return 0
    fi
    return 1
}

mdkr_remove_detached_mount_dir() {
    local mount_dir="$1"

    [[ -n "${mount_dir}" ]] || return 0
    mdkr_dmg_mount_is_active "${mount_dir}" && return 1
    rmdir "${mount_dir}" 2>/dev/null || true
}
