#!/usr/bin/env bash
#
# make_public_source_archive.sh -- create a tracked-files-only public source
# archive after running the public release guard.
#
# This intentionally uses `git archive`, not tar over the working directory, so
# ignored local ROMs, extracted assets, screenshots, audio dumps, and build
# products cannot be swept into a release artifact.
#
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

ref="HEAD"
out=""
force=0

usage() {
  cat <<'USAGE'
Usage: tools/make_public_source_archive.sh [--ref REF] [--out PATH] [--force]

Creates a gzip-compressed public source archive from tracked git content only.
The script fails if the non-ignored working tree is dirty, if release checks
fail, or if the archive listing contains obvious ROM/media/build-artifact paths.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --ref)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      ref="$2"
      shift 2
      ;;
    --out)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      out="$2"
      shift 2
      ;;
    --force)
      force=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

commit="$(git rev-parse --verify "${ref}^{commit}")"
short="$(git rev-parse --short=12 "$commit")"
prefix="mdkr64-${short}"

if [ -z "$out" ]; then
  out="dist/${prefix}.tar.gz"
fi

dirty="$(git status --porcelain --untracked-files=all)"
if [ -n "$dirty" ]; then
  echo "Refusing to build a public archive with non-ignored working-tree changes:" >&2
  printf '%s\n' "$dirty" >&2
  exit 1
fi

if [ -e "$out" ] && [ "$force" -ne 1 ]; then
  echo "Archive already exists: $out (use --force to replace)" >&2
  exit 1
fi

echo "== Running release guard =="
# Validate the exact public repository before archiving its tracked tree.
./tools/ci/check_release_ready.sh

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/mdkr64-archive.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT

mkdir -p "$(dirname "$out")"
tmparchive="$tmpdir/archive.tar.gz"
listfile="$tmpdir/list.txt"

echo
echo "== Creating source archive =="
git archive --format=tar --prefix="${prefix}/" "$commit" | gzip -n > "$tmparchive"
tar -tzf "$tmparchive" > "$listfile"

echo
echo "== Validating archive listing =="
# Media is rejected unless its exact repository path is in one of the reviewed
# first-party allowlists — the same path-only exemption, from the same two
# files, that tools/ci/check_release_ready.sh applies to git history. Listing
# entries carry the archive prefix, so strip it before comparing.
archive_allowlist="$tmpdir/allowlist.txt"
sed -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$/d' \
  tools/public_tracked_asset_allowlist.txt \
  tools/public_historical_asset_allowlist.txt > "$archive_allowlist"
if [ ! -s "$archive_allowlist" ]; then
  echo "Public asset allowlists define no entries; refusing to validate vacuously." >&2
  exit 1
fi

forbidden="$(sed "s#^${prefix}/##" "$listfile" \
  | grep -E '\.(z64|n64|v64|rom|bin|bmp|png|jpe?g|gif|webp|ico|icns|ppm|raw|wav|mp3|ogg|flac|ctl|tbl|aifc|aiff|sbk|seq|cdata|dmg|zip|7z|tar|tgz|gz)$|(^|/)baserom|(^|/)[^/]+\.app(/|$)|(^|/)screenshot_[^/]*\.(bmp|png|jpe?g|gif|webp|ppm|raw|jsonl|mp4|mov|m4v|webm)$' \
  | grep -Fvx -f "$archive_allowlist" || true)"
if [ -n "$forbidden" ]; then
  echo "Archive contains forbidden ROM/media/build-artifact path(s):" >&2
  printf '%s\n' "$forbidden" >&2
  exit 1
fi

entries="$(wc -l < "$listfile" | tr -d ' ')"
mv "$tmparchive" "$out"
shasum -a 256 "$out" > "${out}.sha256"

echo "  OK -- archive listing contains ${entries} tracked entries."
echo
echo "Archive:  $out"
echo "SHA-256:  $(cut -d' ' -f1 "${out}.sha256")"
