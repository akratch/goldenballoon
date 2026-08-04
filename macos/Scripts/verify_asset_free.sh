#!/bin/bash
set -euo pipefail
#
# verify_asset_free.sh -- confirm a macOS binary/app is free of ROM-derived assets
#
# This architecture (platform/rom_io.c / platform/asset_swap.c /
# platform/main_pc.c) is bring-your-own-ROM with no compiled-in placeholder
# asset tables AT ALL: `nm` over a Release build-rel/mdkr64 finds no
# imgRAre_*/ANIM_DATA_*-style symbols, because none exist in the source tree.
# Every asset byte is read from the user's ROM file at runtime through the LUT
# in platform/rom_io.c. The invariant enforced here is therefore that such
# symbols must be ABSENT, full stop -- if one ever appears, something regressed
# toward compiling in real asset data.
#
# A size-threshold test on those symbols would be meaningless on this platform:
# `nm -P`/`nm -S` report size 0 for EVERY symbol in the optimized macOS Release
# Mach-O this project's CMake config produces (verified against build-rel/mdkr64:
# 35426 symbols, none with a nonzero reported size), so any ">N bytes = FAIL"
# rule would silently degrade to "always PASS". Checks 3 (bootstrap magic, a
# byte-pattern scan) and 4 (data segment size, read from Mach-O load commands)
# are unaffected by symbol-table limitations and are the two structurally
# reliable checks.
#
# Purpose
# -------
# The N64 game port builds against stub/placeholder data so that the
# resulting binary never ships copyrighted ROM content (sprites, animations,
# Rareware logo bitmaps, etc.).  This script inspects a compiled Mach-O
# binary and fails the build if any contamination vector is detected.
#
# When to run
# -----------
#   - As a post-link CI step (e.g. in a GitHub Actions workflow).
#   - Locally before tagging a release.
#   - Any time the asset pipeline changes and you want a quick sanity check.
#
# Scope
# -----
# This verifier requires a BUILT binary/app-bundle argument (a Mach-O to
# inspect); run with no argument it prints usage and exits 1 by design -- it is
# not a source-tree scanner. The source-tree asset-free gate is
# tools/check_no_rom.sh, which is what gates a source-only checkout. Point this
# script at the built .app/binary (ideally freshly built from the tagged
# commit) as the complementary post-build check.
#
# Exit codes
#   0  All checks passed (WARNs are tolerated).
#   1  One or more checks FAILed -- binary is NOT asset-free.
#

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

PASS_COUNT=0
FAIL_COUNT=0
WARN_COUNT=0

pass() {
    printf "  [PASS] %s\n" "$1"
    (( PASS_COUNT++ )) || true
}

fail() {
    printf "  [FAIL] %s\n" "$1"
    (( FAIL_COUNT++ )) || true
}

warn() {
    printf "  [WARN] %s\n" "$1"
    (( WARN_COUNT++ )) || true
}

scan_bootstrap_magic_file() {
    python3 - "$1" <<'PY'
import pathlib
import sys

signatures = [
    ("z64 big-endian", b"\x80\x37\x12\x40"),
    ("v64 byte-swapped", b"\x37\x80\x40\x12"),
    ("n64 little-endian", b"\x40\x12\x37\x80"),
]

try:
    data = pathlib.Path(sys.argv[1]).read_bytes()
except OSError:
    sys.exit(2)

for label, sig in signatures:
    if sig in data:
        print(f"{label} ({sig.hex(' ')})")
        sys.exit(0)

sys.exit(1)
PY
}

asset_symbols_for_file() {
    local target="$1" nm_tool="${2:-nm}" nm_output
    if ! nm_output="$("${nm_tool}" -P "${target}" 2>&1)"; then
        printf '%s\n' "${nm_output}" >&2
        return 2
    fi
    # A valid stripped release image may contain zero symbol rows. `nm` success
    # is still useful evidence: command/read/format failures are nonzero and are
    # rejected above; the caller separately pins the binary container with file.
    printf '%s\n' "${nm_output}" |
        grep -E '^_?(ANIM_DATA_|imgRAre_|ASSET_DATA_|dkrAssetPayload|ROM_ASSET_)'
}

scan_bootstrap_magic_tree() {
    python3 - "$1" <<'PY'
import pathlib
import sys

signatures = [
    ("z64 big-endian", b"\x80\x37\x12\x40"),
    ("v64 byte-swapped", b"\x37\x80\x40\x12"),
    ("n64 little-endian", b"\x40\x12\x37\x80"),
]

root = pathlib.Path(sys.argv[1])
if not root.is_dir():
    print(f"resource tree is missing or unreadable: {root}", file=sys.stderr)
    sys.exit(2)
bad = []
errors = []
for path in root.rglob("*"):
    if not path.is_file():
        continue
    try:
        data = path.read_bytes()
    except OSError as error:
        errors.append(f"{path}: {error}")
        continue
    for label, sig in signatures:
        if sig in data:
            bad.append(f"{path.relative_to(root.parent.parent)}: {label} ({sig.hex(' ')})")
            break

if errors:
    print("\n".join(errors), file=sys.stderr)
    sys.exit(2)
if bad:
    print("\n".join(bad))
    sys.exit(1)

sys.exit(0)
PY
}

run_self_test() (
    local temporary clean contaminated status output
    temporary="$(mktemp -d "${TMPDIR:-/tmp}/mdkr-asset-guard.XXXXXX")"
    clean="${temporary}/clean.bin"
    contaminated="${temporary}/contaminated.bin"
    trap 'rm -rf "${temporary}"' EXIT
    printf 'asset-free fixture\n' > "${clean}"
    printf '\200\067\022\100' > "${contaminated}"

    output="$(scan_bootstrap_magic_file "${clean}" 2>&1)" && status=0 || status=$?
    [[ ${status} -eq 1 ]] || { printf '%s\n' "${output}" >&2; return 1; }
    output="$(scan_bootstrap_magic_file "${contaminated}" 2>&1)" && status=0 || status=$?
    [[ ${status} -eq 0 && "${output}" == *"z64 big-endian"* ]] || return 1
    scan_bootstrap_magic_file "${temporary}/missing" >/dev/null 2>&1 && status=0 || status=$?
    [[ ${status} -eq 2 ]] || return 1
    scan_bootstrap_magic_tree "${temporary}" >/dev/null 2>&1 && status=0 || status=$?
    [[ ${status} -eq 1 ]] || return 1
    scan_bootstrap_magic_tree "${temporary}/missing-tree" >/dev/null 2>&1 && status=0 || status=$?
    [[ ${status} -eq 2 ]] || return 1
    asset_symbols_for_file "${clean}" false >/dev/null 2>&1 && status=0 || status=$?
    [[ ${status} -eq 2 ]] || return 1
    # Invoked indirectly by asset_symbols_for_file as a command-shaped fixture.
    # shellcheck disable=SC2329
    stripped_nm() { printf '%s: no symbols\n' "$2" >&2; return 0; }
    asset_symbols_for_file "${clean}" stripped_nm >/dev/null 2>&1 && status=0 || status=$?
    [[ ${status} -eq 1 ]] || return 1
    printf 'verify_asset_free: self-test PASS\n'
)

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------

if [[ "${1:-}" == "--self-test" ]]; then
    run_self_test || { echo "verify_asset_free: self-test FAIL" >&2; exit 1; }
    exit 0
fi

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <path-to-macos-binary-or-app-bundle>"
    echo ""
    echo "Verifies that the built binary/app contains no ROM-derived copyrighted"
    echo "content. Returns 0 on success, 1 if contamination is detected."
    exit 1
fi

INPUT_PATH="$1"
BINARY="$INPUT_PATH"
APP_BUNDLE_INPUT=false
APP_BUNDLE=""

if [[ -d "$BINARY" && "$BINARY" == *.app ]]; then
    APP_BUNDLE_INPUT=true
    APP_BUNDLE="$BINARY"
    INFO_PLIST="${APP_BUNDLE}/Contents/Info.plist"
    if [[ ! -f "${INFO_PLIST}" ]]; then
        echo "ERROR: app bundle is missing Info.plist: ${APP_BUNDLE}"
        exit 1
    fi
    EXECUTABLE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "${INFO_PLIST}" 2>/dev/null || true)"
    if [[ -z "${EXECUTABLE_NAME}" ]]; then
        echo "ERROR: app bundle Info.plist is missing CFBundleExecutable: ${INFO_PLIST}"
        exit 1
    fi
    # CFBundleExecutable is now the real Mach-O: the app shell and the engine
    # are one binary (the bash picker shim it replaced is retired). The legacy
    # "<name>-engine" sibling is still honored so this verifier keeps working
    # against an older bundle that was built with the shim layout.
    BINARY="${APP_BUNDLE}/Contents/MacOS/${EXECUTABLE_NAME}"
    LEGACY_ENGINE="${APP_BUNDLE}/Contents/MacOS/${EXECUTABLE_NAME}-engine"
    if [[ -f "${LEGACY_ENGINE}" ]]; then
        BINARY="${LEGACY_ENGINE}"
    fi
fi

if [[ ! -f "$BINARY" ]]; then
    echo "ERROR: file not found: $BINARY"
    exit 1
fi

for tool in file python3; do
    command -v "${tool}" >/dev/null 2>&1 || {
        echo "ERROR: required inspection tool not found: ${tool}" >&2
        exit 1
    }
done
[[ -r "$BINARY" ]] || { echo "ERROR: binary is not readable: $BINARY" >&2; exit 1; }
BINARY_DESCRIPTION="$(file "$BINARY" 2>&1)" && FILE_STATUS=0 || FILE_STATUS=$?
if (( FILE_STATUS != 0 )); then
    printf '%s\n' "${BINARY_DESCRIPTION}" >&2
    echo "ERROR: could not classify binary format: $BINARY" >&2
    exit 1
fi
case "${BINARY_DESCRIPTION}" in
    *Mach-O*|*ELF*|*PE32*) ;;
    *) echo "ERROR: unsupported or unrecognized binary format: ${BINARY_DESCRIPTION}" >&2; exit 1 ;;
esac

# Native `nm`/`size` understand the host object format. A macOS release-prep
# machine cross-compiling Windows otherwise feeds PE32+ to Apple's Mach-O-only
# tools and reports a false contamination failure. Prefer the matching MinGW
# inspectors for PE when they are installed; Windows/MSYS and native ELF/Mach-O
# lanes continue using their ordinary tools. Explicit overrides are useful for
# other cross toolchain prefixes and remain opt-in.
MDKR_ASSET_NM_TOOL="${MDKR_ASSET_NM_TOOL:-nm}"
MDKR_ASSET_SIZE_TOOL="${MDKR_ASSET_SIZE_TOOL:-size}"
if [[ "${BINARY_DESCRIPTION}" == *PE32* ]]; then
    if [[ "${MDKR_ASSET_NM_TOOL}" == nm ]] &&
        command -v x86_64-w64-mingw32-nm >/dev/null 2>&1; then
        MDKR_ASSET_NM_TOOL=x86_64-w64-mingw32-nm
    fi
    if [[ "${MDKR_ASSET_SIZE_TOOL}" == size ]] &&
        command -v x86_64-w64-mingw32-size >/dev/null 2>&1; then
        MDKR_ASSET_SIZE_TOOL=x86_64-w64-mingw32-size
    fi
fi
for tool in "${MDKR_ASSET_NM_TOOL}" "${MDKR_ASSET_SIZE_TOOL}"; do
    command -v "${tool}" >/dev/null 2>&1 || {
        echo "ERROR: required inspection tool not found: ${tool}" >&2
        exit 1
    }
done

echo "============================================================"
echo "  Asset-Free Verification"
echo "  Binary: $BINARY"
echo "============================================================"
echo ""

# ---------------------------------------------------------------------------
# 1. Compiled-in DKR asset-table symbols
#
#    mdkr64 loads every game asset from the user's ROM file at runtime
#    (platform/rom_io.c's LUT-driven loader); nothing asset-shaped is ever
#    compiled in, so none of these symbol names should exist in the binary AT
#    ALL. Presence itself is the signal here -- a present-but-tiny table is
#    still a regression, because there is no legitimate reason any of these
#    would appear. The name list covers mdkr64's own asset plumbing
#    (platform/asset_swap.c, platform/asset_swap.h's AssetSectionsEnum-driven
#    table) plus adjacent decomp-lineage names, so a future code-sharing change
#    cannot reintroduce one under an older name.
# ---------------------------------------------------------------------------

echo "--- Check 1: Compiled-in DKR asset-table symbols ---"

ASSET_SYMBOLS="$(asset_symbols_for_file "$BINARY" "${MDKR_ASSET_NM_TOOL}" 2>&1)" && SYMBOL_STATUS=0 || SYMBOL_STATUS=$?
case "${SYMBOL_STATUS}" in
    0)
        fail "Compiled-in asset-table symbol(s) found -- this architecture must load all game data from the ROM at runtime, never compile it in:"
        printf '%s\n' "$ASSET_SYMBOLS" | sed 's/^/    /'
        ;;
    1)
        pass "No compiled-in asset-table symbol found (ANIM_DATA_*, imgRAre_*, ASSET_DATA_*, dkrAssetPayload*, ROM_ASSET_*)."
        ;;
    *)
        printf '%s\n' "$ASSET_SYMBOLS" | sed 's/^/    /' >&2
        fail "Could not inspect the binary symbol table."
        ;;
esac

echo ""

# ---------------------------------------------------------------------------
# 2. Known ROM byte signatures
#
#    N64 ROM headers contain identifiable ASCII strings.  If any of these
#    appear in the final binary, ROM content has leaked in.
# ---------------------------------------------------------------------------

echo "--- Check 2: embedded-ROM signature (bootstrap magic) ---"

# We detect an *embedded ROM* by the N64 bootstrap magic bytes appearing
# contiguously in the binary's data — that only happens if a real ROM image
# leaked in. We deliberately do NOT flag the ASCII title "Diddy Kong Racing":
# it appears legitimately in our own source (platform/rom_id.c's revision
# table, enum identifiers, log strings, etc.), so a name-string match is not
# evidence of leaked ROM data. Bulk leakage is caught by Check 3 (data
# segment size) and Check 1 (asset symbols); this check catches a raw ROM
# blob.

# N64 bootstrap magic (PI register init) appears at the start of ROM images in
# all common byte orders. In our source these bytes only exist as separate C
# constants, never contiguously, so a contiguous match means an actual ROM image
# or byte-swapped ROM dump is embedded.
SIG_MATCH="$(scan_bootstrap_magic_file "$BINARY" 2>&1)" && SIG_STATUS=0 || SIG_STATUS=$?
case "${SIG_STATUS}" in
    0) fail "N64 bootstrap magic (${SIG_MATCH}) found contiguously - a ROM image is embedded!" ;;
    1) pass "No embedded-ROM bootstrap magic detected." ;;
    *) printf '%s\n' "${SIG_MATCH}" >&2; fail "Could not scan binary bytes for ROM bootstrap magic." ;;
esac

echo ""

# ---------------------------------------------------------------------------
# 3. Data segment size check
#
#    A clean port binary (code + a handful of small tables, no game assets)
#    should have a data segment well under a few MB. `build-rel/mdkr64` in
#    this worktree measures ~4.5 MB of __DATA (mostly the statically-linked
#    wgpu-native prebuilt's rodata + glad's GL loader tables -- neither is
#    game data), so the BSS-aware Mach-O thresholds below (12 MB WARN / 16 MB
#    FAIL) sit with plenty of headroom: a compiled-in ~12 MB ROM payload would
#    still push well past FAIL.
# ---------------------------------------------------------------------------

echo "--- Check 3: Data segment size ---"

# `size` on macOS prints:  __TEXT  __DATA  __OBJC  others  ...
# We want the __DATA column.  With -m (Mach-O format) we can parse more
# reliably, but the default BSD output is simpler:
#   __TEXT  __DATA  __OBJC  others  hex     decimal
# Column 2 is __DATA.

SIZE_OUTPUT="$("${MDKR_ASSET_SIZE_TOOL}" "$BINARY" 2>&1)" && SIZE_STATUS=0 || SIZE_STATUS=$?
if (( SIZE_STATUS != 0 )); then
    printf '%s\n' "${SIZE_OUTPUT}" >&2
    DATA_SIZE=""
else
    DATA_SIZE="$(printf '%s\n' "${SIZE_OUTPUT}" | tail -1 | awk '{print $2}')"
fi

if [[ ! "$DATA_SIZE" =~ ^[1-9][0-9]*$ ]]; then
    fail "Could not determine a nonzero data segment size."
else
    DATA_KB=$(( DATA_SIZE / 1024 ))
    # Classify the binary so the right data-segment threshold applies. A ROM
    # would show up as ~12 MB of real initialized data on every platform; what
    # differs is how `size` measures the CLEAN baseline.
    IS_MACHO_IMAGE=false   # macOS `size` folds __bss into __DATA
    IS_ELF_IMAGE=false     # Linux `size` DATA is REAL data (bss is a separate col)
    if printf '%s\n' "${BINARY_DESCRIPTION}" | grep -qiE "Mach-O.*(executable|shared library|dylib)"; then
        IS_MACHO_IMAGE=true
    elif printf '%s\n' "${BINARY_DESCRIPTION}" | grep -qiE "ELF.*(executable|shared object)"; then
        IS_ELF_IMAGE=true
    fi
    if [[ "${IS_ELF_IMAGE}" == true ]]; then
        if (( DATA_SIZE > 4194304 )); then
            fail "Data segment is ${DATA_KB} KB (${DATA_SIZE} bytes). Threshold: 4 MB (ELF program). Assets may be compiled in."
        elif (( DATA_SIZE > 2097152 )); then
            warn "Data segment is ${DATA_KB} KB (${DATA_SIZE} bytes). Above 2 MB -- review for embedded assets."
        else
            pass "Data segment is ${DATA_KB} KB (${DATA_SIZE} bytes). Within limits (ELF program)."
        fi
    elif [[ "${APP_BUNDLE_INPUT}" == true || "${IS_MACHO_IMAGE}" == true ]]; then
        if (( DATA_SIZE > 16777216 )); then
            fail "Data segment is ${DATA_KB} KB (${DATA_SIZE} bytes). Threshold: 16 MB. Assets may be compiled in."
        elif (( DATA_SIZE > 12582912 )); then
            warn "Data segment is ${DATA_KB} KB (${DATA_SIZE} bytes). Above 12 MB -- review for embedded assets."
        else
            pass "Data segment is ${DATA_KB} KB (${DATA_SIZE} bytes). Within limits."
        fi
    elif (( DATA_SIZE > 512000 )); then
        fail "Data segment is ${DATA_KB} KB (${DATA_SIZE} bytes). Threshold: 500 KB. Assets may be compiled in."
    elif (( DATA_SIZE > 204800 )); then
        warn "Data segment is ${DATA_KB} KB (${DATA_SIZE} bytes). Above 200 KB -- review for embedded assets."
    else
        pass "Data segment is ${DATA_KB} KB (${DATA_SIZE} bytes). Well within limits."
    fi
fi

echo ""

# ---------------------------------------------------------------------------
# 4. App bundle resource hygiene
#
#    A clean app bundle may contain project-owned UI resources: the generated
#    AppIcon.icns, the privacy manifest, SDL_GameControllerDB's plain-text
#    mapping file, and the bundled SDL2 license notice. It must not carry ROMs,
#    extracted game assets,
#    screenshots, audio dumps, or other opaque payloads outside the
#    executables.
# ---------------------------------------------------------------------------

if [[ "${APP_BUNDLE_INPUT}" == true ]]; then
    echo "--- Check 4: App bundle resources ---"

    RESOURCE_FAIL=0
    RESOURCE_DIR="${APP_BUNDLE}/Contents/Resources"

    ICON_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIconFile' "${INFO_PLIST}" 2>/dev/null || true)"
    if [[ -z "${ICON_NAME}" ]]; then
        fail "App bundle Info.plist is missing CFBundleIconFile."
        RESOURCE_FAIL=1
    else
        case "${ICON_NAME}" in
            *.icns) ICON_FILE="${RESOURCE_DIR}/${ICON_NAME}" ;;
            *)      ICON_FILE="${RESOURCE_DIR}/${ICON_NAME}.icns" ;;
        esac

        if [[ -s "${ICON_FILE}" ]]; then
            pass "Declared app icon exists: ${ICON_FILE#"${APP_BUNDLE}"/}"
        else
            fail "Declared app icon is missing or empty: ${ICON_FILE#"${APP_BUNDLE}"/}"
            RESOURCE_FAIL=1
        fi
    fi

    if [[ ! -d "${RESOURCE_DIR}" ]]; then
        fail "App bundle is missing Contents/Resources."
        RESOURCE_FAIL=1
    else
        while IFS= read -r RESOURCE; do
            REL="${RESOURCE#"${APP_BUNDLE}"/}"
            BASE="$(basename "${RESOURCE}")"

            case "${REL}" in
                Contents/Resources/AppIcon.icns|\
                Contents/Resources/PrivacyInfo.xcprivacy)
                    ;;
                Contents/Resources/gamecontrollerdb.txt)
                    # SDL_GameControllerDB: plain-text community pad mappings
                    # (zlib; lib/sdl_gamecontrollerdb/LICENSE.txt). No
                    # ROM/asset content -- deliberately shipped so exotic and
                    # handheld controllers map out of the box.
                    ;;
                Contents/Resources/ThirdParty/SDL2-LICENSE.txt)
                    # Required notice for the standalone SDL2 dylib shipped in
                    # Contents/Frameworks. Its exact bytes are pinned by the
                    # unsigned-release verifier.
                    ;;
                Contents/Resources/ThirdParty/SDL2-MANIFEST.txt)
                    # Four-line, content-free dependency provenance: pinned
                    # SDL2 version/source plus the pre- and post-sign dylib
                    # hashes. verify_unsigned_release.sh validates its exact
                    # schema and values before accepting a release bundle.
                    ;;
                *)
                    fail "Unexpected app resource in asset-free bundle: ${REL}"
                    RESOURCE_FAIL=1
                    ;;
            esac

            case "${BASE}" in
                *.z64|*.Z64|*.n64|*.N64|*.v64|*.V64|baserom*|\
                *.bin|*.BIN|*.cdata|*.CDATA|*.ctl|*.CTL|*.tbl|*.TBL|*.sbk|*.SBK|*.seq|*.SEQ|\
                *.aifc|*.AIFC|*.aiff|*.AIFF|*.bmp|*.BMP|*.png|*.PNG|*.jpg|*.JPG|*.jpeg|*.JPEG|\
                *.gif|*.GIF|*.webp|*.WEBP|*.ppm|*.PPM|*.raw|*.RAW|*.wav|*.WAV|*.mp3|*.MP3|\
                *.ogg|*.OGG|*.flac|*.FLAC)
                    fail "Forbidden ROM/media-like resource in app bundle: ${REL}"
                    RESOURCE_FAIL=1
                    ;;
            esac
        done < <(find "${RESOURCE_DIR}" -type f -print)

        RESOURCE_MAGIC_MATCHES="$(scan_bootstrap_magic_tree "${RESOURCE_DIR}" 2>&1)" &&
            RESOURCE_MAGIC_STATUS=0 || RESOURCE_MAGIC_STATUS=$?
        case "${RESOURCE_MAGIC_STATUS}" in
            0) ;;
            1)
                printf '%s\n' "${RESOURCE_MAGIC_MATCHES}"
                fail "Embedded N64 ROM bootstrap magic found in app resource(s)."
                RESOURCE_FAIL=1
                ;;
            *)
                printf '%s\n' "${RESOURCE_MAGIC_MATCHES}" >&2
                fail "Could not inspect every app resource."
                RESOURCE_FAIL=1
                ;;
        esac
    fi

    if (( RESOURCE_FAIL == 0 )); then
        pass "No unexpected ROM/media resource payloads detected."
    fi

    echo ""

    echo "--- Check 5: App bundle frameworks ---"

    FRAMEWORK_FAIL=0
    FRAMEWORKS_DIR="${APP_BUNDLE}/Contents/Frameworks"

    if [[ ! -d "${FRAMEWORKS_DIR}" ]]; then
        pass "No embedded Frameworks directory present."
    else
        while IFS= read -r FRAMEWORK_FILE; do
            REL="${FRAMEWORK_FILE#"${APP_BUNDLE}"/}"
            BASE="$(basename "${FRAMEWORK_FILE}")"
            FRAMEWORK_DESCRIPTION="$(file "${FRAMEWORK_FILE}" 2>&1)" &&
                FRAMEWORK_FILE_STATUS=0 || FRAMEWORK_FILE_STATUS=$?
            if (( FRAMEWORK_FILE_STATUS != 0 )) ||
                [[ "${FRAMEWORK_DESCRIPTION}" != *Mach-O* ]]; then
                printf '%s\n' "${FRAMEWORK_DESCRIPTION}" >&2
                fail "App framework/library is not an inspectable Mach-O: ${REL}"
                FRAMEWORK_FAIL=1
                continue
            fi

            case "${REL}" in
                Contents/Frameworks/libSDL2-2.0.0.dylib|\
                Contents/Frameworks/libSDL2.dylib)
                    ;;
                *)
                    fail "Unexpected app framework/library payload in asset-free bundle: ${REL}"
                    FRAMEWORK_FAIL=1
                    ;;
            esac

            case "${BASE}" in
                *.z64|*.Z64|*.n64|*.N64|*.v64|*.V64|baserom*|\
                *.bin|*.BIN|*.cdata|*.CDATA|*.ctl|*.CTL|*.tbl|*.TBL|*.sbk|*.SBK|*.seq|*.SEQ|\
                *.aifc|*.AIFC|*.aiff|*.AIFF|*.bmp|*.BMP|*.png|*.PNG|*.jpg|*.JPG|*.jpeg|*.JPEG|\
                *.gif|*.GIF|*.webp|*.WEBP|*.ppm|*.PPM|*.raw|*.RAW|*.wav|*.WAV|*.mp3|*.MP3|\
                *.ogg|*.OGG|*.flac|*.FLAC)
                    fail "Forbidden ROM/media-like framework payload in app bundle: ${REL}"
                    FRAMEWORK_FAIL=1
                    ;;
            esac

            FRAMEWORK_SIG_MATCH="$(scan_bootstrap_magic_file "${FRAMEWORK_FILE}" 2>&1)" &&
                FRAMEWORK_SIG_STATUS=0 || FRAMEWORK_SIG_STATUS=$?
            case "${FRAMEWORK_SIG_STATUS}" in
                0)
                    fail "Embedded N64 ROM bootstrap magic found in app framework/library: ${REL} (${FRAMEWORK_SIG_MATCH})"
                    FRAMEWORK_FAIL=1
                    ;;
                1) ;;
                *)
                    printf '%s\n' "${FRAMEWORK_SIG_MATCH}" >&2
                    fail "Could not scan app framework/library bytes: ${REL}"
                    FRAMEWORK_FAIL=1
                    ;;
            esac

            ASSET_FRAMEWORK_SYMBOLS="$(asset_symbols_for_file "${FRAMEWORK_FILE}" 2>&1)" &&
                FRAMEWORK_SYMBOL_STATUS=0 || FRAMEWORK_SYMBOL_STATUS=$?
            case "${FRAMEWORK_SYMBOL_STATUS}" in
                0)
                    fail "Asset-data symbol found in app framework/library: ${REL}"
                    FRAMEWORK_FAIL=1
                    ;;
                1) ;;
                *)
                    printf '%s\n' "${ASSET_FRAMEWORK_SYMBOLS}" >&2
                    fail "Could not inspect app framework/library symbols: ${REL}"
                    FRAMEWORK_FAIL=1
                    ;;
            esac
        done < <(find "${FRAMEWORKS_DIR}" -type f -print)

        if (( FRAMEWORK_FAIL == 0 )); then
            pass "No unexpected ROM/media framework payloads detected."
        fi
    fi

    echo ""
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo "============================================================"
echo "  Summary"
echo "    PASS: $PASS_COUNT"
echo "    FAIL: $FAIL_COUNT"
echo "    WARN: $WARN_COUNT"
echo "============================================================"

if (( FAIL_COUNT > 0 )); then
    echo ""
    if [[ "${APP_BUNDLE_INPUT}" == true ]]; then
        echo "RESULT: FAILED -- app bundle did not pass asset-free verification."
    else
        echo "RESULT: FAILED -- binary did not pass asset-free verification."
    fi
    echo "        Do NOT distribute this build."
    exit 1
else
    echo ""
    if (( WARN_COUNT > 0 )); then
        echo "RESULT: PASSED (with ${WARN_COUNT} warning(s) -- review recommended)."
    else
        if [[ "${APP_BUNDLE_INPUT}" == true ]]; then
            echo "RESULT: PASSED -- app bundle is asset-free."
        else
            echo "RESULT: PASSED -- binary is asset-free."
        fi
    fi
    exit 0
fi
