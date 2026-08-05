#!/bin/bash
#
# check_no_rom.sh -- FAIL-CLOSED guard: no shipped artifact may contain ROM data.
#
# This is the mechanism that keeps a public release in the clear. Golden Balloon is
# bring-your-own-ROM: the user supplies a copy they legally own, it is read on
# their machine (in the browser build, entirely client-side and never uploaded),
# and nothing ROM-derived is ever redistributed. This script is what proves that
# claim about a build instead of merely asserting it.
#
# Mirrors the mgb64 web-demo ROM-absence guard. Two independent checks per file:
#
#   1. HEADER MAGIC AT OFFSET 0, in all three N64 byte orders -- catches a ROM
#      renamed onto a whitelisted artifact name:
#         80 37 12 40  .z64  big-endian (the on-disk form the engine loads)
#         37 80 40 12  .v64  byteswapped
#         40 12 37 80  .n64  little-endian
#   2. THE BIG-ENDIAN MAGIC SEQUENCE ANYWHERE in the file -- catches ROM content
#      appended to, or embedded inside, an artifact (e.g. baked into a wasm blob).
#
# Why not just grep for the game's name: the engine legitimately contains ROM
# validation strings, so a plain string search false-positives. The header magic is
# structural, so it does not.
#
# Usage:
#   tools/check_no_rom.sh dist/web            # scan a directory (recursively)
#   tools/check_no_rom.sh dist/web/a.wasm ... # scan specific files
#   tools/check_no_rom.sh                     # default: dist/web
#
# Exit 0 = clean and safe to publish. Exit 1 = STOP, do not publish.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
targets=("${@:-dist/web}")

files=()
for t in "${targets[@]}"; do
    if [[ -d "$t" ]]; then
        while IFS= read -r f; do files+=("$f"); done < <(find "$t" -type f)
    elif [[ -e "$t" ]]; then
        files+=("$t")
    else
        echo "check_no_rom: no such path: $t" >&2
        exit 1
    fi
done

if [[ ${#files[@]} -eq 0 ]]; then
    echo "check_no_rom: FAIL — nothing to scan (empty target); refusing to pass vacuously" >&2
    exit 1
fi

# Big-endian .z64 header magic. Also the byte sequence any embedded ROM carries.
be_magic=$'\x80\x37\x12\x40'

bad=0
for f in "${files[@]}"; do
    # (1) offset-0 header, any of the three byte orders. An unreadable artifact
    # must be named, not swallowed: under `set -e -o pipefail` a failing read
    # here would abort the whole scan with no diagnostic at all.
    read_status=0
    magic=$(head -c4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n') || read_status=$?
    if [[ "$read_status" -ne 0 ]]; then
        echo "check_no_rom: FAIL — could not read artifact header: $f" >&2
        bad=1
        continue
    fi
    case "$magic" in
        80371240|37804012|40123780)
            echo "check_no_rom: FAIL — N64 ROM header at offset 0: $f" >&2
            bad=1
            continue
            ;;
    esac
    # (2) big-endian magic anywhere (binary-safe fixed-string search).
    # grep exits 0 on a match, 1 on no match, and >=2 on an error (unreadable
    # file, I/O failure). Only status 1 is evidence of absence; anything else
    # means the artifact was not actually cleared and must fail closed.
    scan_status=0
    LC_ALL=C grep -qF "$be_magic" "$f" 2>/dev/null || scan_status=$?
    case "$scan_status" in
        0)
            echo "check_no_rom: FAIL — N64 ROM magic sequence embedded in: $f" >&2
            bad=1
            continue
            ;;
        1) ;;
        *)
            echo "check_no_rom: FAIL — could not scan for ROM magic (grep exit ${scan_status}): $f" >&2
            bad=1
            continue
            ;;
    esac
    # Runtime-derived font atlases must never be packaged as sidecar content.
    # The engine builds them from the user's ROM in memory on their machine.
    case "$(basename "$f" | tr '[:upper:]' '[:lower:]')" in
        *.sdf|*.atlas|*font_sdf*|*font-atlas*)
            echo "check_no_rom: FAIL — runtime-derived font artifact shipped: $f" >&2
            bad=1
            continue
            ;;
    esac
    printf '  ok  %s\n' "$f"
done

# UI-1 adds a runtime derivation path, so artifact scanning alone is not enough:
# prove its implementation has no output API and that its only production
# consumer hands the caller-owned buffer straight to the GPU uploader.
deriver="$repo_root/platform/fast3d/gfx_font_sdf.c"
renderer="$repo_root/platform/fast3d/gfx_pc_dkr.c"
if [[ ! -f "$deriver" || ! -f "$renderer" ]]; then
    echo "check_no_rom: FAIL — runtime font derivation sources are missing" >&2
    bad=1
else
    forbidden_io='(^|[^[:alnum:]_])(fopen|freopen|open|creat|write|fwrite|fprintf|remove|rename|mkdir|system|popen)[[:space:]]*\('
    if LC_ALL=C grep -nE "$forbidden_io" "$deriver"; then
        echo "check_no_rom: FAIL — font derivation module can write derived output" >&2
        bad=1
    else
        production_calls=$(grep -R -l \
            --include='*.c' 'gfx_font_sdf_upscale_rgba[[:space:]]*(' \
            "$repo_root/platform" 2>/dev/null |
            grep -vFx "$deriver" || true)
        expected_callsite="$repo_root/platform/fast3d/gfx_pc_dkr.c"
        if [[ "$production_calls" != "$expected_callsite" ]]; then
            echo "check_no_rom: FAIL — unexpected production font-derivation callsite(s):" >&2
            printf '%s\n' "$production_calls" >&2
            bad=1
        elif ! awk '
            /gfx_font_sdf_upscale_rgba[[:space:]]*\(/ { window=12 }
            window > 0 && /gfx_rapi->upload_texture[[:space:]]*\(/ { found=1 }
            window > 0 { window-- }
            END { exit(found ? 0 : 1) }
        ' "$renderer"; then
            echo "check_no_rom: FAIL — derived font buffer is not routed directly to GPU upload" >&2
            bad=1
        else
            echo "  ok  runtime font derivation is memory→GPU only; no file-output API or sidecar"
        fi
    fi
fi

if [[ "$bad" -ne 0 ]]; then
    echo "check_no_rom: FAIL — ROM or runtime-derived data policy failed above. DO NOT PUBLISH." >&2
    exit 1
fi

echo "check_no_rom: PASS — ${#files[@]} artifact(s) scanned, no ROM data present."
