#!/bin/bash
#
# run_emscripten_unit_tests.sh -- ROM-free unit tests for code with a browser
# arm, built with the pinned emcc and run under node.
#
# CTest builds natively only, so code that behaves differently in the browser
# is covered here: the no-replace move that sets a damaged controller pak or
# ghost file aside (Emscripten's file system has no hard links, so it has an
# arm of its own) and the ghost bank that uses it, including its request to
# commit browser storage after a set-aside. node runs them on MEMFS, the
# in-memory file system the browser's IndexedDB save mount sits on; nothing
# is written to the host.
#
# Usage:
#   tools/web/run_emscripten_unit_tests.sh [output-dir]   # default build-web-tests
#
set -euo pipefail
cd "$(dirname "$0")/../.."

# The same pin, found the same way, as tools/web/build_web.sh.
MDKR_EMSCRIPTEN_VERSION="${MDKR_EMSCRIPTEN_VERSION:-4.0.10}"
for candidate in "${EMSDK_DIR:-}" "$HOME/emsdk" /usr/local/emsdk /opt/emsdk; do
    if [[ -n "$candidate" && -f "$candidate/emsdk_env.sh" ]]; then
        # shellcheck disable=SC1091
        source "$candidate/emsdk_env.sh" >/dev/null 2>&1 || true
        break
    fi
done
if ! command -v emcc >/dev/null 2>&1 || ! command -v node >/dev/null 2>&1; then
    echo "run_emscripten_unit_tests: emcc and node are required (install emsdk" >&2
    echo "  or set EMSDK_DIR)." >&2
    exit 2
fi
EMCC_VERSION_LINE="$(emcc --version | head -1)"
if [[ "$EMCC_VERSION_LINE" != *"$MDKR_EMSCRIPTEN_VERSION"* ]]; then
    echo "run_emscripten_unit_tests: Emscripten $MDKR_EMSCRIPTEN_VERSION is required," >&2
    echo "  but this is: $EMCC_VERSION_LINE" >&2
    exit 2
fi

OUT="${1:-build-web-tests}"
mkdir -p "$OUT"

# The web engine's own stack size: the ghost bank keeps large arrays on it.
COMMON=(-std=gnu11 -O1 -sENVIRONMENT=node -sEXIT_RUNTIME=1
        -sSTACK_SIZE=2097152 -Iplatform)
# The ghost bank's flags from cmake/tests.cmake (mdkr_ghost_bank_test).
GAME=(-Igame -Igame/src -Igame/include -Igame/libultra
      -Igame/libultra/src/audio -iquote game/include/PR
      -iquote game/include/sys -DVERSION_us_v80 -D_LANGUAGE_C -DMODERN_CC
      -DNON_MATCHING=1 -DAVOID_UB=1 -DNATIVE_PORT=1 -DF3DDKR_GBI -D_FINALROM
      -fms-extensions -Wno-c23-extensions)

echo ">> emcc: $EMCC_VERSION_LINE"
emcc "${COMMON[@]}" tests/test_fs_set_aside.c platform/fs_utf8.c \
    -o "$OUT/fs_set_aside.js"
emcc "${COMMON[@]}" "${GAME[@]}" tests/test_ghost_bank.c \
    platform/ghost_bank.c platform/fs_utf8.c platform/sha256.c \
    -o "$OUT/ghost_bank.js"

status=0
for test in fs_set_aside ghost_bank; do
    echo ">> node: $test"
    if node "$OUT/$test.js"; then
        echo "PASS: $test"
    else
        echo "FAIL: $test" >&2
        status=1
    fi
done
exit "$status"
