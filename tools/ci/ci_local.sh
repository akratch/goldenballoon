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
# shell tool syntax, documentation links, and a Release configure+build of the
# native port. On a caller-attested dedicated desktop, `--with-compiled-tests`
# adds the ROM-free CTest suite and `--with-gpu-tests` adds the native graphics
# lane no hosted runner can execute.
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
#                        [--with-compiled-tests] [--with-gpu-tests]
#                        [--with-rom-suite [--rom PATH]]
#
# -e is safe here because every gate runs through step(), which invokes it as an
# `if` condition; a failing gate is scored, not fatal. -e only catches faults in
# this script's own control flow.
set -euo pipefail

# Yield CPU scheduling priority to the interactive desktop. Children inherit
# this niceness, including configure, compiler, CTest and an opted-in ROM lane.
if command -v renice >/dev/null 2>&1; then
  renice 15 -p "$$" >/dev/null 2>&1 || true
fi
# On macOS, background policy also constrains inherited I/O and scheduler
# pressure. This keeps compilers and CTest responsive to the user's foreground
# applications even if a dependency creates more worker threads internally.
if [ "$(uname -s)" = "Darwin" ] && command -v taskpolicy >/dev/null 2>&1; then
  taskpolicy -b -p "$$" >/dev/null 2>&1 || true
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build"
DO_BUILD=1
# Keep an ordinary local validation responsive. A dedicated CI host can opt
# into more compile parallelism with --jobs or MDKR_CI_JOBS.
JOBS="${MDKR_CI_JOBS:-2}"
WITH_ROM_SUITE=0
ROM_PATH="baserom.us.v80.z64"
# Native GPU/window tests are opt-in. Even a nominally hidden SDL surface can
# interact badly with a desktop/window-manager regression, and a local CI
# command must never take over the maintainer's workstation by default.
# Ambient shell state must never turn a safe local command into a window/GPU
# run. Only the explicit command-line flag below can enable this lane.
RUN_GPU_TESTS=0
# Even a supposedly ROM-free CTest inventory is executable code and can gain a
# mislabelled SDL/Cocoa test. Keep all compiled tests out of the occupied-Mac
# default. --with-gpu-tests implies this opt-in because that lane is CTest.
RUN_COMPILED_TESTS=0

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir)      BUILD_DIR="$2"; shift 2 ;;
    --no-build)       DO_BUILD=0; shift ;;
    --jobs)           JOBS="$2"; shift 2 ;;
    --with-compiled-tests) RUN_COMPILED_TESTS=1; shift ;;
    --with-gpu-tests) RUN_GPU_TESTS=1; shift ;;
    --skip-gpu-tests) RUN_GPU_TESTS=0; shift ;; # compatibility; now the default
    --with-rom-suite) WITH_ROM_SUITE=1; shift ;;
    --rom)            ROM_PATH="$2"; shift 2 ;;
    -h|--help)         sed -n '2,27p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
case "$JOBS" in
  ''|*[!0-9]*|0) echo "jobs must be a positive integer (got '$JOBS')" >&2; exit 2 ;;
esac
if [ "$RUN_GPU_TESTS" -eq 1 ]; then
  RUN_COMPILED_TESTS=1
fi

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
  # GPU/window tests are absent from ordinary CTest inventories. Register them
  # only when the caller made the dedicated-desktop opt-in explicit.
  step "CMake configure" cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
    -DMDKR_ENABLE_GPU_TESTS="$RUN_GPU_TESTS"
  build_one() {
    set -o pipefail
    cmake --build "$BUILD_DIR" --parallel "$JOBS" 2>&1 | tee "$BUILD_DIR/mdkr64-build.log"
  }
  step "Build native port" build_one
fi

# --- ROM-free test suite ---
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && [ "$RUN_COMPILED_TESTS" -eq 1 ]; then
  step "ROM-free CTest suite" \
    env MDKR64_HIDDEN=1 MDKR_AUDIO=0 \
      SDL_MAC_BACKGROUND_APP=1 SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1 \
      CTEST_PARALLEL_LEVEL=1 OMP_NUM_THREADS=1 RAYON_NUM_THREADS=1 \
      VECLIB_MAXIMUM_THREADS=1 \
      ctest --test-dir "$BUILD_DIR" --output-on-failure -j1 \
        -LE 'gpu|app_process|browser'

  # These tests open native graphics surfaces. They stay hidden/non-key by
  # contract, which is what keeps them off the desktop.
  if [ "$RUN_GPU_TESTS" -eq 1 ]; then
    step "GPU-labelled CTest lane (explicit --with-gpu-tests)" \
      env MDKR64_HIDDEN=1 MDKR_AUDIO=0 \
        SDL_MAC_BACKGROUND_APP=1 SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1 \
        CTEST_PARALLEL_LEVEL=1 OMP_NUM_THREADS=1 RAYON_NUM_THREADS=1 \
        VECLIB_MAXIMUM_THREADS=1 \
        ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error \
          -j1 -L gpu
  else
    echo "  SKIPPED: gpu-labelled CTest lane (pass --with-gpu-tests to add it)."
  fi
else
  if [ "$RUN_COMPILED_TESTS" -eq 0 ]; then
    echo "  SKIPPED: all compiled/CTest execution (default occupied-workstation mode)."
    echo "  Run --with-compiled-tests only on a dedicated test desktop."
  else
    echo "  (skipping ctest - no configured build at $BUILD_DIR; run without --no-build)"
  fi
fi

# --- Workstation-safe ROM-gated checks (opt-in; application roles stay gated) ---
if [ "$WITH_ROM_SUITE" -eq 1 ]; then
  if [ ! -f "$ROM_PATH" ]; then
    echo ""
    echo "  --with-rom-suite requested but ROM not found at: $ROM_PATH" >&2
    echo "  Pass --rom PATH to point at your own legally-dumped ROM." >&2
    fail=$((fail + 1))
    failed_steps="${failed_steps}\n  - Workstation-safe ROM-gated checks (ROM missing)"
  else
    # The runner's default is deliberately workstation-safe: native app,
    # renderer, browser and GPU lanes remain excluded. A complete release pass
    # is a separate dedicated-desktop operation documented in the release
    # checklist.
    step "Workstation-safe ROM-gated checks (tools/run_checks.py)" \
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
