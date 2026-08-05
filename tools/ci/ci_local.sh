#!/usr/bin/env bash
#
# ci_local.sh -- run the ROM-free CI gates locally.
#
# correctness.yml is hosted (push/PR/workflow_dispatch), but the release and
# validation lanes (release.yml, windows-validate.yml, macos-release.yml) are
# manual-only. This script is the one-command local mirror of the ROM-free
# gates: it runs what correctness.yml's `policy` job runs, needs no ROM or game
# data by default, and reports a single pass/fail summary.
#
# COVERED HERE: release/provenance hygiene, ignored-artifact hygiene, public
# shell tool syntax, documentation links, a Release configure+build of the
# native port, the ROM-free CTest suite, and -- on a machine with a display --
# the gpu-labelled CTest lane no hosted runner can execute.
#
# DELIBERATELY NOT COVERED: the full ROM-gated regression battery
# (tools/run_checks.py, ~76 tasks across several dedicated build directories --
# build-rel, build-asan, a linked wasm build, and a real Chrome profile). It
# needs a legally-owned ROM the contributor supplies locally, takes much
# longer, and is sequential by design -- several checks create and remove
# save/eeprom.bin -- so it must not race a concurrent invocation on a shared
# checkout. It remains the owner-run pre-release gate documented in
# docs/RELEASE_CHECKLIST.md; pass --with-rom-suite to also run it from here.
#
# Usage:
#   tools/ci/ci_local.sh [--build-dir DIR] [--no-build] [--jobs N]
#                        [--skip-gpu-tests] [--with-rom-suite [--rom PATH]]
#
# -e is safe here because every gate runs through step(), which invokes it as an
# `if` condition; a failing gate is scored, not fatal. -e only catches faults in
# this script's own control flow.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build"
DO_BUILD=1
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
WITH_ROM_SUITE=0
ROM_PATH="baserom.us.v80.z64"
SKIP_GPU_TESTS="${MDKR_CI_SKIP_GPU_TESTS:-0}"

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir)      BUILD_DIR="$2"; shift 2 ;;
    --no-build)       DO_BUILD=0; shift ;;
    --jobs)           JOBS="$2"; shift 2 ;;
    --skip-gpu-tests) SKIP_GPU_TESTS=1; shift ;;
    --with-rom-suite) WITH_ROM_SUITE=1; shift ;;
    --rom)            ROM_PATH="$2"; shift 2 ;;
    -h|--help)         sed -n '2,27p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
case "$SKIP_GPU_TESTS" in
  0|1) ;;
  *) echo "MDKR_CI_SKIP_GPU_TESTS must be 0 or 1 (got '$SKIP_GPU_TESTS')" >&2; exit 2 ;;
esac

pass=0; fail=0; failed_steps=""

step() {
  local name="$1"; shift
  printf '\n\033[1m== %s ==\033[0m\n' "$name"
  if "$@"; then
    pass=$((pass + 1)); printf '\033[32m  PASS: %s\033[0m\n' "$name"
  else
    fail=$((fail + 1)); failed_steps="${failed_steps}\n  - ${name}"
    printf '\033[31m  FAIL: %s\033[0m\n' "$name"
  fi
}

# --- Release / provenance hygiene (the most important gate for a decomp) ---
# check_release_ready.sh itself runs check_clean_room.sh, check_no_rom.sh, and
# the reachable-history text audit, so they are not repeated here as separate
# top-level steps.
step "Release hygiene (no ROM data, clean-room, public history)" \
  bash tools/ci/check_release_ready.sh
step "Ignored-artifact hygiene" bash tools/ci/check_high_risk_ignored_artifacts.sh
step "Public shell tool syntax" python3 tools/check_shell_syntax.py --repo-root .
step "Public documentation links" python3 tools/check_markdown_links.py --repo-root .

# --- Build the native port (ROM-free) ---
if [ "$DO_BUILD" -eq 1 ]; then
  step "CMake configure" cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  build_one() {
    set -o pipefail
    cmake --build "$BUILD_DIR" --parallel "$JOBS" 2>&1 | tee "$BUILD_DIR/mdkr64-build.log"
  }
  step "Build native port" build_one
fi

# --- ROM-free test suite ---
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  step "ROM-free CTest suite" \
    ctest --test-dir "$BUILD_DIR" --output-on-failure -LE gpu

  # The gpu-labelled tests open a real window/context, so they cannot run on a
  # headless runner and correctness.yml excludes them everywhere. Without a
  # scripted runner here nothing drives them at all, so they run by default on
  # a machine that has a display and are skipped only on an explicit opt-out --
  # never silently.
  if [ "$SKIP_GPU_TESTS" -eq 1 ]; then
    echo "  SKIPPED: gpu-labelled CTest lane (--skip-gpu-tests / MDKR_CI_SKIP_GPU_TESTS=1)."
    echo "  These tests were NOT run; a pass below does not cover them."
  else
    step "GPU-labelled CTest lane (needs a real display; --skip-gpu-tests to opt out)" \
      ctest --test-dir "$BUILD_DIR" --output-on-failure -L gpu
  fi
else
  echo "  (skipping ctest - no configured build at $BUILD_DIR; run without --no-build)"
fi

# --- Full ROM-gated regression battery (opt-in; owner-run pre-release gate) ---
if [ "$WITH_ROM_SUITE" -eq 1 ]; then
  if [ ! -f "$ROM_PATH" ]; then
    echo ""
    echo "  --with-rom-suite requested but ROM not found at: $ROM_PATH" >&2
    echo "  Pass --rom PATH to point at your own legally-dumped ROM." >&2
    fail=$((fail + 1))
    failed_steps="${failed_steps}\n  - Full ROM-gated regression battery (ROM missing)"
  else
    # Deliberately the COMPLETE suite (no --only): a subset battery can hide a
    # broken gate that the full run would have caught, and this script has no
    # way to know which subset would have been safe to skip. run_checks.py is
    # sequential by design (several checks create/remove save/eeprom.bin), so
    # do not run this concurrently with another invocation against the same
    # checkout/build directories.
    step "Full ROM-gated regression battery (tools/run_checks.py)" \
      python3 tools/run_checks.py --rom "$ROM_PATH"
  fi
fi

# --- Summary ---
printf '\n\033[1m== ci_local summary ==\033[0m\n'
printf '  passed: %d\n  failed: %d\n' "$pass" "$fail"
if [ "$fail" -ne 0 ]; then
  printf '\033[31mFAILED steps:%b\033[0m\n' "$failed_steps"
  exit 1
fi
printf '\033[32mAll requested CI gates passed.\033[0m\n'
