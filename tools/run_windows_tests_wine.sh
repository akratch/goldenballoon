#!/usr/bin/env bash
#
# run_windows_tests_wine.sh -- SUPPLEMENTARY lane: run a Windows test bundle
# (tools/package_windows_test_bundle.sh) under Wine in an existing Docker
# image, and report PASS/FAIL per test.
#
# Wine is not Windows. This lane exists to find a bundle problem, or a defect
# that is not Wine's own, before the bundle reaches a Windows PC; it never
# stands in for that run. tools/windows_test_bundle/wine_runner.py mirrors
# RUN-TESTS.cmd: the bundle in a folder with spaces and non-ASCII letters, a
# scratch folder per test with TEMP/TMP inside it, every test from an ASCII
# folder and the tests marked "unicode" in the manifest again from a folder
# with spaces, Latin-1 and CJK letters.
#
# Nothing is installed, built or pulled: the image must already exist
# (default mdkr-wine:jammy, amd64, Wine 11). The zip and the runner reach the
# container as a tar stream on stdin, so no host folder is mounted. Without a
# reachable Docker runtime or the image the lane prints SKIPPED and exits 3.
#
# Usage: tools/run_windows_tests_wine.sh [--image IMAGE] [--output FILE]
#                                        [--only REGEX] [--jobs N] [ZIP]
#   ZIP     default: the newest dist/Golden-Balloon-windows-tests-*.zip
#   FILE    default: build-mingw-test-bundle/wine-lane.txt (the full lane
#           output, in the packager's git-ignored folder)
#   REGEX   run only the tests whose name it matches (Python syntax)
#   N       tests run at once (default 6); every Windows program start costs
#           seconds under Wine in an emulated amd64 container
# Exit: 0 all passed, 1 a test failed, 2 usage, 3 skipped.
set -euo pipefail

IMAGE="mdkr-wine:jammy"
OUTPUT=""
ONLY=""
JOBS=6
ZIP=""
usage() {
    echo "usage: tools/run_windows_tests_wine.sh [--image IMAGE] [--output FILE] [--only REGEX] [--jobs N] [ZIP]" >&2
}
while [ $# -gt 0 ]; do
    case "$1" in
        --image) [ $# -ge 2 ] || { usage; exit 2; }; IMAGE="$2"; shift ;;
        --output) [ $# -ge 2 ] || { usage; exit 2; }; OUTPUT="$2"; shift ;;
        --only) [ $# -ge 2 ] || { usage; exit 2; }; ONLY="$2"; shift ;;
        --jobs) [ $# -ge 2 ] || { usage; exit 2; }; JOBS="$2"; shift ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "run_windows_tests_wine: unknown argument: $1" >&2; usage; exit 2 ;;
        *) [ -z "$ZIP" ] || { usage; exit 2; }; ZIP="$1" ;;
    esac
    shift
done

[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { usage; exit 2; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
skip() { echo "SUPPLEMENTARY WINE LANE: SKIPPED — $1"; exit 3; }

if [ -z "$ZIP" ]; then
    for candidate in "$REPO_ROOT"/dist/Golden-Balloon-windows-tests-*.zip; do
        [ -f "$candidate" ] || continue
        if [ -z "$ZIP" ] || [ "$candidate" -nt "$ZIP" ]; then ZIP="$candidate"; fi
    done
    [ -n "$ZIP" ] || { echo "no bundle in dist/; run tools/package_windows_test_bundle.sh first" >&2; exit 2; }
fi
[ -f "$ZIP" ] || { echo "no such bundle: $ZIP" >&2; exit 2; }
if [ -z "$OUTPUT" ]; then
    mkdir -p "$REPO_ROOT/build-mingw-test-bundle"
    OUTPUT="$REPO_ROOT/build-mingw-test-bundle/wine-lane.txt"
fi

command -v docker >/dev/null 2>&1 || skip "no docker command"
# colima publishes its socket here rather than at the default context's path.
if ! docker info >/dev/null 2>&1 && [ -z "${DOCKER_HOST:-}" ] &&
        [ -S "$HOME/.colima/default/docker.sock" ]; then
    export DOCKER_HOST="unix://$HOME/.colima/default/docker.sock"
fi
docker info >/dev/null 2>&1 || skip "no Docker runtime is reachable (start colima or Docker Desktop)"
docker image inspect "$IMAGE" >/dev/null 2>&1 || skip "image $IMAGE is not present (this lane never builds or pulls it)"

STREAM="$(mktemp -d "${TMPDIR:-/tmp}/wine-lane.XXXXXX")"
CONTAINER="mdkr-wine-lane-$$"
# An interrupted lane must not leave its container running.
trap 'rm -rf "$STREAM"; docker rm -f "$CONTAINER" >/dev/null 2>&1 || true' EXIT
cp "$ZIP" "$STREAM/bundle.zip"
cp "$REPO_ROOT/tools/windows_test_bundle/wine_runner.py" "$STREAM/wine_runner.py"

echo "SUPPLEMENTARY WINE LANE (Wine is not Windows): $(basename "$ZIP") in $IMAGE"
set +e
tar -C "$STREAM" -cf - bundle.zip wine_runner.py |
    docker run --rm -i --pull never --platform linux/amd64 --name "$CONTAINER" \
        -e WINE_LANE_ONLY="$ONLY" -e WINE_LANE_JOBS="$JOBS" "$IMAGE" \
        sh -c 'mkdir -p /lane && tar -C /lane -xf - &&
               WINEDEBUG=-all LANG=C.UTF-8 python3 /lane/wine_runner.py /lane/bundle.zip' \
        2>&1 | tee "$OUTPUT"
STATUS="${PIPESTATUS[1]}"
set -e
echo "full lane output: $OUTPUT"
exit "$STATUS"
