#!/usr/bin/env bash
#
# smoke_public_source_archive.sh -- prove a public source archive builds/tests.
#
# This validates the artifact a user downloads, not the current working tree:
# extract the tarball into a temporary no-git directory, configure, build, and
# run the ROM-free CTest suite from that extracted source.
#
set -euo pipefail

archive=""
keep=0
build_type="Release"
jobs="${GE007_BUILD_JOBS:-4}"
max_warnings=""

usage() {
  cat <<'USAGE'
Usage: tools/smoke_public_source_archive.sh ARCHIVE [options]

Options:
  --keep              Keep the extracted temporary tree for inspection.
  --build-type TYPE   CMake build type (default: Release).
  --jobs N            Parallel build jobs (default: GE007_BUILD_JOBS or 4).
  --max-warnings N    Fail if the archive build emits more than N warnings.
  -h, --help          Show this help.

The archive is expected to be a gzip-compressed tarball produced by
tools/make_public_source_archive.sh. The smoke runs without a ROM and must not
require a .git directory. Build warnings are always summarized; use
--max-warnings 0 for a release-clean archive gate.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --keep)
      keep=1
      shift
      ;;
    --build-type)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      build_type="$2"
      shift 2
      ;;
    --jobs)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      jobs="$2"
      shift 2
      ;;
    --max-warnings)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      max_warnings="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --*)
      usage >&2
      exit 2
      ;;
    *)
      if [ -n "$archive" ]; then
        usage >&2
        exit 2
      fi
      archive="$1"
      shift
      ;;
  esac
done

if [ -z "$archive" ]; then
  usage >&2
  exit 2
fi

if [ ! -f "$archive" ]; then
  echo "Archive not found: $archive" >&2
  exit 2
fi

case "$jobs" in
  ''|*[!0-9]*|0)
    echo "--jobs must be a positive integer, got: $jobs" >&2
    exit 2
    ;;
esac

if [ -n "$max_warnings" ]; then
  case "$max_warnings" in
    *[!0-9]*)
      echo "--max-warnings must be a non-negative integer, got: $max_warnings" >&2
      exit 2
      ;;
  esac
fi

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/mdkr64-source-smoke.XXXXXX")"
cleanup() {
  if [ "$keep" -eq 1 ]; then
    echo "Kept extracted archive tree: $tmpdir"
  else
    rm -rf "$tmpdir"
  fi
}
trap cleanup EXIT INT TERM

echo "== Extracting source archive =="
tar -xzf "$archive" -C "$tmpdir"

roots_file="$tmpdir/top-level-dirs.txt"
find "$tmpdir" -mindepth 1 -maxdepth 1 -type d | sort > "$roots_file"
root_count="$(wc -l < "$roots_file" | tr -d ' ')"
if [ "$root_count" -ne 1 ]; then
  echo "Expected archive to contain exactly one top-level directory, found $root_count" >&2
  sed 's/^/  /' "$roots_file" >&2
  exit 1
fi
srcdir="$(sed -n '1p' "$roots_file")"

if [ -e "$srcdir/.git" ]; then
  echo "Archive unexpectedly contains a .git directory: $srcdir/.git" >&2
  exit 1
fi

echo "  source: $srcdir"

echo
echo "== Configuring archive build =="
cmake -S "$srcdir" -B "$srcdir/build" -DCMAKE_BUILD_TYPE="$build_type"

echo
echo "== Building archive =="
build_log="$srcdir/build/mdkr64-source-smoke-build.log"
cmake --build "$srcdir/build" --parallel "$jobs" 2>&1 | tee "$build_log"

echo
echo "== Summarizing archive build warnings =="
# MGB64's tools/summarize_build_warnings.py has no counterpart here; gate
# directly on a compiler-warning line count from the build log instead.
warning_count="$(grep -c -E ': warning:' "$build_log" || true)"
echo "  ${warning_count} compiler warning(s) found in the archive build log."
if [ -n "$max_warnings" ] && [ "$warning_count" -gt "$max_warnings" ]; then
  echo "Archive build exceeded the warning budget: ${warning_count} > ${max_warnings}" >&2
  exit 1
fi

echo
echo "== Running archive CTest =="
ctest --test-dir "$srcdir/build" --output-on-failure -LE gpu

echo
echo "Source archive smoke passed."
