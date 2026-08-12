#!/bin/bash
#
# build_web.sh -- reproducible WebGPU browser build, staged into dist/web/.
#
# Produces the bring-your-own-ROM browser build: wasm + loader + the shell that
# lets the player pick their own ROM file locally. The ROM is read in the browser
# and never uploaded; nothing ROM-derived is ever written into dist/web/.
#
# The last step runs tools/check_no_rom.sh, which fails closed if any staged
# artifact contains N64 ROM header magic. That guard is what makes the
# bring-your-own-ROM claim checkable rather than merely asserted, so this script
# refuses to finish without it passing.
#
# Usage:
#   tools/web/build_web.sh                 # uses emcmake from PATH
#   EMSDK_DIR=~/emsdk tools/web/build_web.sh
#   tools/web/build_web.sh --clean
#
set -euo pipefail
cd "$(dirname "$0")/../.."

CLEAN=0
[[ "${1:-}" == "--clean" ]] && CLEAN=1

# Activate emsdk if emcmake is not already on PATH.
if ! command -v emcmake >/dev/null 2>&1; then
    for candidate in "${EMSDK_DIR:-}" "$HOME/emsdk" /usr/local/emsdk /opt/emsdk; do
        if [[ -n "$candidate" && -f "$candidate/emsdk_env.sh" ]]; then
            # shellcheck disable=SC1091
            source "$candidate/emsdk_env.sh" >/dev/null 2>&1 || true
            break
        fi
    done
fi
if ! command -v emcmake >/dev/null 2>&1; then
    echo "build_web: emcmake not found. Install emsdk and/or set EMSDK_DIR." >&2
    exit 2
fi

echo ">> emcc: $(emcc --version | head -1)"

[[ "$CLEAN" -eq 1 ]] && rm -rf build-web

# The release version, from the one place it is declared.
APP_VERSION="$(sed -n 's/^set(MDKR_VERSION "\([^"]*\)".*/\1/p' CMakeLists.txt | head -1)"
if [[ -z "$APP_VERSION" ]]; then
    echo "build_web: FAIL -- could not read MDKR_VERSION from CMakeLists.txt." >&2
    exit 1
fi
# MDKR_VERSION is a CACHE variable, so re-running `emcmake cmake -S . -B
# build-web` over an existing build-web/ keeps whatever version that cache was
# first configured with; a release bump would otherwise be silently ignored
# until someone remembered --clean. Carry the declaration onto the configure
# line so the cache always tracks it. CMakeLists stays authoritative -- the
# literal is read from it here, never written into this script.
emcmake cmake -S . -B build-web -DMDKR_VERSION="$APP_VERSION"
cmake --build build-web -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# CMake owns the release version. Read back the configured cache value rather
# than copying a version literal into this shell script, then carry it through
# the staged artifact and visible browser identity.
MDKR_VERSION="$(sed -n 's/^MDKR_VERSION:STRING=//p' build-web/CMakeCache.txt | head -n 1)"
if [[ ! "$MDKR_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.-]+)?$ ]]; then
    echo "build_web: FAIL -- configured MDKR_VERSION is not a release version: ${MDKR_VERSION:-<missing>}" >&2
    exit 1
fi
# The declaration and the version this wasm was actually configured with must be
# the same string, or the page stamp and the artifact would disagree (a stale
# build-web/ cache is the usual cause). Fail closed rather than publish a build
# that names two versions -- and fail here, before anything in dist/web is
# touched, so a refusal never costs the tree its staged provenance.
if [[ "$APP_VERSION" != "$MDKR_VERSION" ]]; then
    echo "build_web: FAIL -- CMakeLists declares $APP_VERSION but build-web/ is configured for $MDKR_VERSION." >&2
    echo "  Reconfigure (tools/web/build_web.sh --clean) before publishing." >&2
    exit 1
fi

mkdir -p dist/web

# Everything that can refuse this build is decided HERE, while dist/web is still
# untouched. Staging is destructive -- it clears the previous link's output
# before copying this one in -- so a gate that fires after the clear would leave
# the tree with artifacts and no provenance to describe them, which is exactly
# how a suite run once found dist/web/build-info.json missing hours after a
# "completed" build. Clearing and regenerating must be one indivisible step.
REQUIRED_BUILD_OUTPUT=(
    build-web/mdkr64_web.js build-web/mdkr64_web.wasm
    build-web/mdkr-save-tools.js build-web/mdkr-save-tools.wasm
    build-web/mdkr-online-tools.js build-web/mdkr-online-tools.wasm
)
for required in "${REQUIRED_BUILD_OUTPUT[@]}"; do
    if [[ ! -f "$required" ]]; then
        echo "build_web: FAIL -- the link did not produce $required." >&2
        exit 1
    fi
done

wasm_bytes=$(wc -c < build-web/mdkr64_web.wasm | tr -d ' ')
save_tools_bytes=$(wc -c < build-web/mdkr-save-tools.wasm | tr -d ' ')
online_tools_bytes=$(wc -c < build-web/mdkr-online-tools.wasm | tr -d ' ')
echo ">> wasm: ${wasm_bytes} bytes"
echo ">> save tools wasm: ${save_tools_bytes} bytes"
echo ">> online view tools wasm: ${online_tools_bytes} bytes"

# Hard ceiling well under the 100 MiB GitHub Pages per-file limit. Fail closed so
# an accidentally-embedded asset blob cannot sail through into a release.
if [[ "$wasm_bytes" -ge 41943040 ]]; then
    echo "build_web: FAIL -- wasm exceeds the 40 MiB budget (${wasm_bytes} bytes)." >&2
    echo "  Something large was linked in. Do not publish until this is understood." >&2
    exit 1
fi
if [[ "$save_tools_bytes" -ge 524288 ]]; then
    echo "build_web: FAIL -- save-tools wasm exceeds 512 KiB (${save_tools_bytes} bytes)." >&2
    echo "  The save module must remain ROM- and engine-independent." >&2
    exit 1
fi
if [[ "$online_tools_bytes" -ge 131072 ]]; then
    echo "build_web: FAIL -- online view tools wasm exceeds 128 KiB (${online_tools_bytes} bytes)." >&2
    echo "  Matchmaking providers, the renderer and game code do not belong in this module." >&2
    exit 1
fi

# ---- provenance stamp -------------------------------------------------------
# Every build records the exact source commit it came from, so "what code is
# live?" always has an answer. The demo repo is a publication target with no
# source in it, which makes this stamp the ONLY link back to the code -- see
# docs/DEMO_REPO.md. `dirty` is recorded honestly rather than hidden; the
# publisher refuses to publish a dirty tree without an explicit override.
# Composed before staging so writing it cannot fail on anything but I/O; the
# version it names was reconciled with CMakeLists above. The publisher stamps
# that version onto <html data-build-version> so a backup exported in the
# browser records the same product version the native tools write into theirs.
SRC_SHA="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
SRC_SHORT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
if [[ -z "$(git status --porcelain --untracked-files=all 2>/dev/null)" ]]; then
    SRC_DIRTY=false
else
    SRC_DIRTY=true
fi
BUILD_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
BUILD_INFO_JSON="$(cat <<JSON
{
  "project": "Golden Balloon",
  "version": "$MDKR_VERSION",
  "source_commit": "$SRC_SHA",
  "source_commit_short": "$SRC_SHORT",
  "source_dirty": $SRC_DIRTY,
  "built_utc": "$BUILD_UTC",
  "emcc": "$(emcc --version | head -1 | sed 's/"/\\"/g')",
  "wasm_bytes": $wasm_bytes
}
JSON
)"

# ---- staging ----------------------------------------------------------------
# Clear the previous link's output before staging this one. An emitted file that
# a later toolchain stops producing (a .data package, a worker shim, an old
# symbol map) would otherwise sit in dist/web forever and be published as though
# it belonged to this build. The list is exact and covers ONLY build output:
# index.html, the shell scripts, style.css, the manifest and assets/ are tracked
# sources and must survive untouched.
#
# From the first rm to the last write this block makes no decisions and cannot
# refuse: only I/O can fail here. If it does, the trap clears the whole staged
# set rather than leaving a half-built dist/web that later tools would read as a
# real, complete build.
STAGED_BUILD_OUTPUT=(
    mdkr64_web.js mdkr64_web.wasm mdkr64_web.js.symbols
    mdkr64_web.data mdkr64_web.worker.js mdkr64_web.wasm.map
    mdkr-save-tools.js mdkr-save-tools.wasm mdkr-save-tools.js.symbols
    mdkr-save-tools.data mdkr-save-tools.worker.js
    mdkr-online-tools.js mdkr-online-tools.wasm mdkr-online-tools.js.symbols
    mdkr-online-tools.data mdkr-online-tools.worker.js
    build-info.json
)
clear_staged_build_output() {
    for stale in "${STAGED_BUILD_OUTPUT[@]}"; do
        rm -f "dist/web/$stale"
    done
}
staging_failed() {
    local status=$?
    # Never let the cleanup's own failure cut this handler short: the operator
    # has to be told what state dist/web was left in either way.
    clear_staged_build_output || true
    echo "build_web: FAIL -- staging into dist/web did not complete; the" >&2
    echo "  partially staged build output has been removed. dist/web now has" >&2
    echo "  its tracked sources only. Re-run this script." >&2
    exit $(( status == 0 ? 1 : status ))
}
trap staging_failed EXIT

clear_staged_build_output
cp build-web/mdkr64_web.js build-web/mdkr64_web.wasm dist/web/
cp build-web/mdkr-save-tools.js build-web/mdkr-save-tools.wasm dist/web/
cp build-web/mdkr-online-tools.js build-web/mdkr-online-tools.wasm dist/web/
# Ship the symbol map produced by THIS link (see the --emit-symbol-map comment in
# CMakeLists.txt): it is the only way a browser stack trace of bare wasm code
# offsets can be turned back into function names, and a later rebuild does not
# reproduce the module byte-for-byte. Index->name text only; no game data.
if [[ -f build-web/mdkr64_web.js.symbols ]]; then
    cp build-web/mdkr64_web.js.symbols dist/web/
fi
printf '%s\n' "$BUILD_INFO_JSON" > dist/web/build-info.json

trap - EXIT
echo ">> provenance: v$MDKR_VERSION, source $SRC_SHORT (dirty=$SRC_DIRTY) at $BUILD_UTC"

echo ">> ROM-absence guard"
tools/check_no_rom.sh dist/web

echo ">> staged in dist/web:"
ls -1 dist/web

cat <<'NOTE'

Serve it locally (a plain file:// open will not work -- wasm needs HTTP):
    python3 -m http.server -d dist/web 8000
    # then open http://localhost:8000 in a WebGPU-capable browser and pick your ROM

dist/web/*.js and *.wasm are git-ignored build output and are never committed.
NOTE
