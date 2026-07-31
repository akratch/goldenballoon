#!/usr/bin/env bash
#
# check_high_risk_ignored_artifacts.sh -- fail if a launch checkout contains
# ignored ROM-derived data, local ROMs, captures, or other risky media outside
# normal build output directories.
#
# Ordinary development checkouts often have ignored build trees and extracted
# ROM assets. That is fine for local work, but the final public-launch pass
# should run from a fresh checkout or a scrubbed workspace so no local artifact
# can be accidentally archived, attached, screenshared, or copied into a release.
#
set -euo pipefail

limit=80
repo_root=""

usage() {
  cat <<'USAGE'
Usage: tools/ci/check_high_risk_ignored_artifacts.sh [options]

Options:
  --repo-root PATH   Repository root to inspect (default: git top-level).
  --limit N          Maximum paths to print before truncating (default: 80).
  -h, --help         Show this help.
USAGE
}

require_non_negative_int() {
  local name="$1"
  local value="$2"
  case "$value" in
    ''|*[!0-9]*)
      echo "${name} must be a non-negative integer, got: ${value}" >&2
      exit 2
      ;;
  esac
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --repo-root)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      repo_root="$2"
      shift 2
      ;;
    --limit)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      limit="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

require_non_negative_int "--limit" "$limit"

if [ -n "$repo_root" ]; then
  cd "$repo_root"
else
  cd "$(git rev-parse --show-toplevel)"
fi

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "This guard requires a git checkout so ignored files can be resolved." >&2
  exit 2
fi

is_build_output_path() {
  case "$1" in
    build/*|build-*/*|dist/*|bin/*|__pycache__/*|*/__pycache__/*|tools/mktex/build/*)
      return 0
      ;;
  esac
  return 1
}

is_high_risk_path() {
  local path="$1"

  if is_build_output_path "$path"; then
    return 1
  fi

  case "$path" in
    baserom*|*/baserom*|oracle_captures/*|oracle-work/*)
      return 0
      ;;
    assets/*)
      return 0
      ;;
    frame_trace*|*/frame_trace*|screenshot_*|*/screenshot_*|ref_before_*|*/ref_before_*|diag_*|*/diag_*)
      return 0
      ;;
    *.z64|*.n64|*.v64|*.rom|*.cdata|*.cdata.*|*.eeprom|*.eep|*.sav|*.srm)
      return 0
      ;;
    # ROM ARCHIVES: a ROM handed over as .zip/.7z/.rar/.tar/.gz is still a ROM.
    # See .gitignore -- five DKR ROM revisions once sat in this project root
    # as untracked-but-not-ignored .zip files, one `git add -A` away from
    # being committed. Now ignored, but still worth flagging here as a
    # concrete artifact a launch pass should not sweep past.
    *.zip|*.7z|*.rar|*.tar|*.tgz|*.gz)
      return 0
      ;;
    *.bin|*.rz|*.ctl|*.tbl|*.sbk|*.seq|*.aifc|*.aiff|*.seg)
      return 0
      ;;
    *.log|*.jsonl)
      return 0
      ;;
    *.bmp|*.png|*.jpg|*.jpeg|*.gif|*.webp|*.ppm|*.raw)
      return 0
      ;;
    *.wav|*.mp3|*.ogg|*.flac|*.m4a|*.aac)
      return 0
      ;;
    *.mp4|*.mov|*.m4v|*.mkv|*.avi|*.webm)
      return 0
      ;;
  esac

  return 1
}

count=0
printed=0
tmp="${TMPDIR:-/tmp}/mdkr64-high-risk-ignored.$$"
trap 'rm -f "$tmp"' EXIT

git ls-files --others --ignored --exclude-standard > "$tmp"

while IFS= read -r path; do
  [ -n "$path" ] || continue
  if is_high_risk_path "$path"; then
    count=$((count + 1))
    if [ "$printed" -lt "$limit" ]; then
      printf '  %s\n' "$path"
      printed=$((printed + 1))
    fi
  fi
done < "$tmp"

if [ "$count" -gt 0 ]; then
  echo
  echo "High-risk ignored artifacts are present outside normal build output." >&2
  echo "Found ${count} ignored path(s) that look like ROMs, extracted assets, captures, saves, or media." >&2
  if [ "$printed" -lt "$count" ]; then
    echo "Only the first ${printed} path(s) are shown above." >&2
  fi
  echo "For the final public-launch pass, run from a fresh clone or move/delete these local artifacts." >&2
  exit 1
fi

echo "PASS: no high-risk ignored ROM/media artifacts found outside build output"
