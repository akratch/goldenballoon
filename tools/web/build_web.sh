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

emcmake cmake -S . -B build-web
cmake --build build-web -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# CMake owns the release version. Read back the configured cache value rather
# than copying a version literal into this shell script, then carry it through
# the staged artifact and visible browser identity.
MDKR_VERSION="$(sed -n 's/^MDKR_VERSION:STRING=//p' build-web/CMakeCache.txt | head -n 1)"
if [[ ! "$MDKR_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.-]+)?$ ]]; then
    echo "build_web: FAIL -- configured MDKR_VERSION is not a release version: ${MDKR_VERSION:-<missing>}" >&2
    exit 1
fi

mkdir -p dist/web
cp build-web/mdkr64_web.js build-web/mdkr64_web.wasm dist/web/
cp build-web/mdkr-save-tools.js build-web/mdkr-save-tools.wasm dist/web/
# Ship the symbol map produced by THIS link (see the --emit-symbol-map comment in
# CMakeLists.txt): it is the only way a browser stack trace of bare wasm code
# offsets can be turned back into function names, and a later rebuild does not
# reproduce the module byte-for-byte. Index->name text only; no game data.
if [[ -f build-web/mdkr64_web.js.symbols ]]; then
    cp build-web/mdkr64_web.js.symbols dist/web/
fi

# ---- provenance stamp -------------------------------------------------------
# Every build records the exact source commit it came from, so "what code is
# live?" always has an answer. The demo repo is a publication target with no
# source in it, which makes this stamp the ONLY link back to the code -- see
# docs/DEMO_REPO.md. `dirty` is recorded honestly rather than hidden; the
# publisher refuses to publish a dirty tree without an explicit override.
SRC_SHA="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
SRC_SHORT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
if [[ -z "$(git status --porcelain --untracked-files=all 2>/dev/null)" ]]; then
    SRC_DIRTY=false
else
    SRC_DIRTY=true
fi
BUILD_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
cat > dist/web/build-info.json <<JSON
{
  "project": "Golden Balloon",
  "version": "$MDKR_VERSION",
  "source_commit": "$SRC_SHA",
  "source_commit_short": "$SRC_SHORT",
  "source_dirty": $SRC_DIRTY,
  "built_utc": "$BUILD_UTC",
  "emcc": "$(emcc --version | head -1 | sed 's/"/\\"/g')",
  "wasm_bytes": $(wc -c < dist/web/mdkr64_web.wasm | tr -d ' ')
}
JSON
echo ">> provenance: $MDKR_VERSION, source $SRC_SHORT (dirty=$SRC_DIRTY) at $BUILD_UTC"

wasm_bytes=$(wc -c < dist/web/mdkr64_web.wasm | tr -d ' ')
save_tools_bytes=$(wc -c < dist/web/mdkr-save-tools.wasm | tr -d ' ')
echo ">> wasm: ${wasm_bytes} bytes"
echo ">> save tools wasm: ${save_tools_bytes} bytes"

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
