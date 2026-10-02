#!/usr/bin/env bash
#
# package_windows_test_bundle.sh -- the ROM-free unit tests as a zip someone
# can run on a real Windows PC without installing anything.
#
# The Windows build is a MinGW-w64 (UCRT) cross-build, so its code paths are
# compiled here but never executed. This bundle carries the test executables
# to a Windows PC: unzip, double-click RUN-TESTS.cmd, send back
# windows-test-results.txt.
#
#   1. tools/mingw_cross_check.sh --intake-tests: the toolchain, the pinned
#      SDL2, the UCRT requirement and a BUILD_TESTING=ON configure in
#      build-mingw-intake.
#   2. Every test executable CTest registers there is built. One that does not
#      build is listed as excluded rather than stopping the bundle.
#   3. tools/windows_test_bundle/select_tests.py decides what ships and why
#      the rest does not (Python/Node drivers, the game itself, network
#      sockets, GPU), copies the executables and fixtures, and writes the
#      manifest tests/tests.txt and the list tests/selection.txt.
#   4. Each executable is stripped and must pass tools/check_windows_imports.sh
#      (stock Windows DLLs and the UCRT only: CMakeLists.txt links every
#      Windows test executable statically); the folder must pass
#      tools/check_no_rom.sh.
#   5. RUN-TESTS.cmd and README.txt (tools/windows_test_bundle/) are added with
#      CRLF line endings and everything is zipped into
#      dist/Golden-Balloon-windows-tests-<version>.zip.
#
# The bundle holds no ROM, nothing derived from one and nothing private: the
# test executables and the committed fixtures their CTest registrations name.
# tools/run_windows_tests_wine.sh runs the same zip under Wine as a
# supplementary check; Wine is not Windows.
#
# Usage: tools/package_windows_test_bundle.sh [--jobs N] [--version VERSION]
#   --version  name the zip (default: the short commit, "-dirty" when tracked
#              files differ from it)
set -euo pipefail

JOBS=8
VERSION=""
usage() {
    echo "usage: tools/package_windows_test_bundle.sh [--jobs N] [--version VERSION]" >&2
}
while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) [ $# -ge 2 ] || { usage; exit 2; }; JOBS="$2"; shift ;;
        --jobs=*) JOBS="${1#*=}" ;;
        --version) [ $# -ge 2 ] || { usage; exit 2; }; VERSION="$2"; shift ;;
        --version=*) VERSION="${1#*=}" ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "package_windows_test_bundle: unknown argument: $1" >&2; usage; exit 2 ;;
    esac
    shift
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
fail() { echo ""; echo "WINDOWS TEST BUNDLE: FAIL — $1" >&2; exit 1; }

COMMIT="$(git -C "$REPO_ROOT" rev-parse --short=8 HEAD)"
DIRTY=""
if [ -n "$(git -C "$REPO_ROOT" status --porcelain --untracked-files=no)" ]; then
    DIRTY="-dirty"
fi
if [ -z "$VERSION" ]; then
    VERSION="$COMMIT$DIRTY"
fi
[[ "$VERSION" =~ ^[0-9A-Za-z][0-9A-Za-z.+_-]*$ ]] \
    || fail "--version may hold letters, digits and . + _ - only: $VERSION"

MINGW_TARGET="x86_64-w64-mingw32"
OBJDUMP_BIN="${MINGW_TARGET}-objdump"
STRIP_BIN="${MINGW_TARGET}-strip"
command -v "$OBJDUMP_BIN" >/dev/null 2>&1 || fail "$OBJDUMP_BIN not found (brew install mingw-w64)"
command -v "$STRIP_BIN" >/dev/null 2>&1 || fail "$STRIP_BIN not found (brew install mingw-w64)"
command -v python3 >/dev/null 2>&1 || fail "python3 not found"
command -v zip >/dev/null 2>&1 || fail "zip not found"

BUILD_DIR="$REPO_ROOT/build-mingw-intake"
NAME="Golden-Balloon-windows-tests-$VERSION"
STAGE_PARENT="$REPO_ROOT/build-mingw-test-bundle"
STAGE="$STAGE_PARENT/$NAME"
ZIP="$REPO_ROOT/dist/$NAME.zip"
BUNDLE_SOURCES="$REPO_ROOT/tools/windows_test_bundle"

# --- 1. Toolchain, SDL2, configure (and the intake executables) -------------
tools/mingw_cross_check.sh --jobs "$JOBS" --intake-tests \
    || fail "tools/mingw_cross_check.sh --intake-tests failed"
[ -f "$BUILD_DIR/CTestTestfile.cmake" ] || fail "no CTest registrations in $BUILD_DIR"

# --- 2. Every test executable CTest runs directly --------------------------
TARGETS="$(sed -nE 's|^add_test\([^ ]+ "'"$BUILD_DIR"'/(mdkr_[A-Za-z0-9_]+)\.exe".*|\1|p' \
    "$BUILD_DIR/CTestTestfile.cmake" | LC_ALL=C sort -u)"
[ -n "$TARGETS" ] || fail "no test executables registered in $BUILD_DIR"
echo "building $(printf '%s\n' "$TARGETS" | wc -l | tr -d ' ') test executables (-j$JOBS) ..."
BUILD_LOG="$STAGE_PARENT-build.log"
# shellcheck disable=SC2086  # deliberate word-split of the target names
if ! cmake --build "$BUILD_DIR" --target $TARGETS -j"$JOBS" -- -k > "$BUILD_LOG" 2>&1; then
    echo "some test executables did not build; they are listed as excluded (see $BUILD_LOG)"
fi

# --- 3. Choose what ships ---------------------------------------------------
rm -rf "$STAGE_PARENT"
mkdir -p "$STAGE/tests"
python3 "$BUNDLE_SOURCES/select_tests.py" --build-dir "$BUILD_DIR" \
    --repo-root "$REPO_ROOT" --tests-dir "$STAGE/tests" --objdump "$OBJDUMP_BIN" \
    || fail "test selection failed"
COUNT="$(grep -vc '^#' "$STAGE/tests/tests.txt")"
[ "$COUNT" -gt 0 ] || fail "no test was selected"

# --- 4. Strip, then prove each executable is self-contained -----------------
IMPORTS="$STAGE_PARENT/imports.txt"
: > "$IMPORTS"
for exe in "$STAGE"/tests/*.exe; do
    "$STRIP_BIN" "$exe" || fail "could not strip $exe"
    OBJDUMP="$OBJDUMP_BIN" tools/check_windows_imports.sh "$exe" >/dev/null \
        || fail "$(basename "$exe") imports a non-system DLL or no UCRT"
    printf '%s: %s\n' "$(basename "$exe")" \
        "$("$OBJDUMP_BIN" -p "$exe" | awk '/DLL Name/{print $3}' | tr '\n' ' ')" >> "$IMPORTS"
done
echo "check_windows_imports: PASS for $(find "$STAGE/tests" -name '*.exe' | wc -l | tr -d ' ') executables (list: $IMPORTS)"

# --- 5. Runner, readme, provenance ------------------------------------------
crlf() { sed -e 's/\r$//' -e 's/$/\r/' "$1"; }
if LC_ALL=C grep -q '[^ -~]' "$BUNDLE_SOURCES/RUN-TESTS.cmd"; then
    fail "RUN-TESTS.cmd must be ASCII: cmd.exe reads it in the console code page"
fi
crlf "$BUNDLE_SOURCES/RUN-TESTS.cmd" > "$STAGE/RUN-TESTS.cmd"
crlf "$BUNDLE_SOURCES/README.txt" \
    | sed -e "s/@VERSION@/$VERSION/" -e "s/@COUNT@/$COUNT/" > "$STAGE/README.txt"
[ "$(wc -l < "$STAGE/README.txt")" -le 10 ] || fail "README.txt is longer than 10 lines"
printf '%s from commit %s%s, built %s with %s\r\n' "$NAME" "$COMMIT" "$DIRTY" \
    "$(date -u +%Y-%m-%dT%H:%MZ)" "$("${MINGW_TARGET}-gcc" --version | head -1)" \
    > "$STAGE/tests/BUILD.txt"

tools/check_no_rom.sh "$STAGE" >/dev/null || fail "tools/check_no_rom.sh refused the bundle"

# --- 6. Zip -------------------------------------------------------------------
mkdir -p "$REPO_ROOT/dist"
rm -f "$ZIP"
(cd "$STAGE_PARENT" && zip -qrX9 "$ZIP" "$NAME") || fail "zip failed"
SHA256="$(shasum -a 256 "$ZIP" | awk '{print $1}')"
printf '%s  %s\n' "$SHA256" "$(basename "$ZIP")" > "$ZIP.sha256"
echo ""
sed -n '1p;/^Excluded/p' "$STAGE/tests/selection.txt"
echo "tests: $COUNT; executables: $(find "$STAGE/tests" -name '*.exe' | wc -l | tr -d ' ')"
echo "zip: $ZIP ($(du -h "$ZIP" | awk '{print $1}'))"
echo "sha256: $SHA256"
echo "WINDOWS TEST BUNDLE: PASS"
