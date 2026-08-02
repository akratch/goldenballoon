#!/usr/bin/env bash
#
# package_linux_appimage.sh -- package the built Linux `mdkr64` app into a
# portable AppImage (bundles SDL2) and a plain .tar.gz fallback. Graphics
# drivers remain host-provided; WebGPU is the default and GL is diagnostic.
#
# Adapted from mgb64's scripts/package_linux_appimage.sh. Runs in the release
# CI (ubuntu-22.04) or locally on Linux. Produces:
#   dist/Golden-Balloon-<version>-linux-x86_64.AppImage
#   dist/Golden-Balloon-<version>-linux-x86_64.tar.gz
#
# The app ships NO game data (bring-your-own-ROM); nothing here embeds ROM bytes.
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

binary="build/mdkr64"
version="dev"
# Release packaging is STRICT by default -- a missing bundled SDL2 runtime or
# a missing AppImage is a hard failure, so a release can't ship a broken/
# incomplete artifact while the job reports success. --dev relaxes both to
# warn-and-continue for local "developer package" builds.
dev=false
self_test=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) binary="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    --dev) dev=true; shift ;;
    --self-test) self_test=true; shift ;;
    -h|--help) echo "Usage: $0 [--binary PATH] [--version VER] [--dev] [--self-test]"; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done
if [[ "$version" != "dev" && ! "$version" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]]; then
  echo "ERROR: version must be dev or bare semver (for example 1.2.3)" >&2
  exit 1
fi

verify_linux_tarball() {
  local archive="$1"
  local staged="$2"
  local expected staged_manifest archive_manifest sdl_entries sdl_count
  staged_manifest="$(
    cd "$(dirname "$staged")"
    find "$(basename "$staged")" \( -type f -o -type l \) -print | LC_ALL=C sort
  )"
  sdl_entries="$(
    printf '%s\n' "$staged_manifest" |
      grep -E '^Golden-Balloon\.AppDir/usr/lib/libSDL2[^/]*$' || true
  )"
  sdl_count="$(printf '%s\n' "$sdl_entries" | sed '/^$/d' | wc -l | tr -d ' ')"
  if [[ "$dev" != true && "$sdl_count" != 1 ]]; then
    echo "ERROR: Linux release tarball must contain exactly one SDL2 runtime." >&2
    return 1
  fi
  if [[ "$dev" == true && "$sdl_count" -gt 1 ]]; then
    echo "ERROR: Linux developer tarball contains multiple SDL2 runtimes." >&2
    return 1
  fi
  expected="$(
    printf '%s\n' \
      Golden-Balloon.AppDir/AppRun \
      Golden-Balloon.AppDir/LICENSE \
      Golden-Balloon.AppDir/README.md \
      Golden-Balloon.AppDir/mdkr64.desktop \
      Golden-Balloon.AppDir/mdkr64.png \
      Golden-Balloon.AppDir/usr/bin/gamecontrollerdb.txt \
      Golden-Balloon.AppDir/usr/bin/mdkr64
    [[ -z "$sdl_entries" ]] || printf '%s\n' "$sdl_entries"
  )"
  expected="$(printf '%s\n' "$expected" | LC_ALL=C sort)"
  if [[ "$staged_manifest" != "$expected" ]]; then
    echo "ERROR: Linux staging tree differs from the exact release manifest." >&2
    diff -u <(printf '%s\n' "$expected") <(printf '%s\n' "$staged_manifest") >&2 || true
    return 1
  fi
  archive_manifest="$(tar -tzf "$archive" | sed '/\/$/d' | LC_ALL=C sort)"
  if [[ "$archive_manifest" != "$expected" ]]; then
    echo "ERROR: Linux tarball payload differs from the exact release manifest." >&2
    diff -u <(printf '%s\n' "$expected") <(printf '%s\n' "$archive_manifest") >&2 || true
    return 1
  fi
}

if [[ "$self_test" == true ]]; then
  dev=false
  test_root="$(mktemp -d "${TMPDIR:-/tmp}/mdkr-linux-package-test.XXXXXX")"
  trap 'rm -rf "$test_root"' EXIT
  test_appdir="$test_root/Golden-Balloon.AppDir"
  mkdir -p "$test_appdir/usr/bin" "$test_appdir/usr/lib"
  for path in AppRun LICENSE README.md mdkr64.desktop mdkr64.png; do
    : >"$test_appdir/$path"
  done
  : >"$test_appdir/usr/bin/gamecontrollerdb.txt"
  : >"$test_appdir/usr/bin/mdkr64"
  : >"$test_appdir/usr/lib/libSDL2-2.0.so.0"
  tar -C "$test_root" -czf "$test_root/package.tar.gz" Golden-Balloon.AppDir
  verify_linux_tarball "$test_root/package.tar.gz" "$test_appdir"
  : >"$test_appdir/unexpected.txt"
  tar -C "$test_root" -czf "$test_root/package.tar.gz" Golden-Balloon.AppDir
  if verify_linux_tarball "$test_root/package.tar.gz" "$test_appdir" >/dev/null 2>&1; then
    echo "ERROR: Linux tarball verifier accepted an unexpected archive entry." >&2
    exit 1
  fi
  echo "package_linux_appimage: self-test PASS"
  exit 0
fi

[[ -x "$binary" ]] || { echo "ERROR: binary not found/executable: $binary" >&2; exit 1; }

# appimagetool is a build-time EXECUTABLE dependency, so it is pinned to an
# immutable release and verified by SHA-256 (fail closed on mismatch) rather than
# fetched from the moving `continuous` alias and run unverified. Same pin as
# mgb64 (both ports are proven against the same appimagetool release).
APPIMAGETOOL_VERSION="1.9.1"
APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL_VERSION}/appimagetool-x86_64.AppImage"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}';
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}';
  else echo ""; fi
}

dist="dist"; mkdir -p "$dist"
work="$(mktemp -d)"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT
appdir="$work/Golden-Balloon.AppDir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib"

cp "$binary" "$appdir/usr/bin/mdkr64"

# Community controller-mapping DB (MC.2), next to the binary where
# SDL_GetBasePath() resolves it at controller init.
if ! cp lib/sdl_gamecontrollerdb/gamecontrollerdb.txt "$appdir/usr/bin/"; then
  if [[ "$dev" == true ]]; then
    echo "WARN: controller mapping database is unavailable in developer package." >&2
  else
    echo "ERROR: tracked controller mapping database is missing from release package." >&2
    exit 1
  fi
fi

# Bundle the linked SDL2 (GL/glibc come from the host; AppImage targets glibc>=host).
sdl="$(ldd "$binary" | awk '/libSDL2/{print $3; exit}')"
if [[ -n "${sdl:-}" && -f "$sdl" ]]; then
  cp -L "$sdl" "$appdir/usr/lib/"
  echo "bundled SDL2: $(basename "$sdl")"
elif [[ "$dev" == true ]]; then
  echo "WARN: could not resolve SDL2 to bundle; the developer package needs a system SDL2." >&2
else
  # A release artifact that silently omits its SDL2 runtime fails to launch on
  # a clean machine. Fail closed unless --dev.
  echo "ERROR: could not resolve a bundled SDL2 from '$binary' (ldd). A release" >&2
  echo "       package must include its runtime; re-run with --dev for a local build." >&2
  exit 1
fi

# AppRun launcher.
cat > "$appdir/AppRun" <<'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:${LD_LIBRARY_PATH:-}"
exec "$HERE/usr/bin/mdkr64" "$@"
EOF
chmod +x "$appdir/AppRun"

# Desktop entry + a minimal icon (AppImage requires both).
cat > "$appdir/mdkr64.desktop" <<'EOF'
[Desktop Entry]
Name=Golden Balloon
Exec=mdkr64
Icon=mdkr64
Type=Application
Categories=Game;
Comment=Golden Balloon native game launcher (bring-your-own-ROM)
EOF
icon_source="brand/appicon-source.png"
if [[ -s "$icon_source" ]]; then
  cp "$icon_source" "$appdir/mdkr64.png"
elif [[ "$dev" == true ]]; then
  echo "WARN: branded icon source is unavailable in developer package." >&2
  # 1x1 charcoal PNG placeholder (base64) — used if branding art is missing.
  base64 -d > "$appdir/mdkr64.png" <<'EOF'
iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==
EOF
else
  echo "ERROR: tracked branded icon source is required in the release package." >&2
  exit 1
fi

if ! cp LICENSE README.md "$appdir/"; then
  if [[ "$dev" == true ]]; then
    echo "WARN: LICENSE or README is unavailable in developer package." >&2
  else
    echo "ERROR: tracked LICENSE and README are required in the release package." >&2
    exit 1
  fi
fi

# Scan the exact tree consumed by both tar and appimagetool, including every
# file the packager added after the standalone binary was checked.
./tools/check_no_rom.sh "$appdir"

# .tar.gz (always produced — the reliable fallback).
artifact_prefix="Golden-Balloon-$version-linux-x86_64"
tarball="$dist/$artifact_prefix.tar.gz"
tar -C "$work" -czf "$tarball" Golden-Balloon.AppDir
verify_linux_tarball "$tarball" "$appdir"
echo "wrote $tarball"

# AppImage (release-required; developer mode may fall back to the tarball).
tool="$work/appimagetool"
# Always fetch the immutable artifact into the private staging directory. A
# same-named executable on PATH has unknown provenance and is never executed.
fetched=""
if command -v curl >/dev/null 2>&1; then
  curl -fsSL -o "$tool" "$APPIMAGETOOL_URL" && fetched=1 || fetched=""
elif command -v wget >/dev/null 2>&1; then
  wget -q -O "$tool" "$APPIMAGETOOL_URL" && fetched=1 || fetched=""
fi
if [[ -n "$fetched" && -f "$tool" ]]; then
  got="$(sha256_of "$tool")"
  if [[ -z "$got" ]]; then
    echo "ERROR: no sha256 tool available to verify appimagetool; refusing to run it." >&2
    exit 1
  elif [[ "$got" != "$APPIMAGETOOL_SHA256" ]]; then
    echo "ERROR: appimagetool ${APPIMAGETOOL_VERSION} digest mismatch:" >&2
    echo "       got      ${got}" >&2
    echo "       expected ${APPIMAGETOOL_SHA256}" >&2
    rm -f "$tool"
    exit 1
  fi
  chmod +x "$tool"
  echo "appimagetool ${APPIMAGETOOL_VERSION} verified (sha256 ${APPIMAGETOOL_SHA256:0:12}...)"
else
  echo "WARN: pinned appimagetool download unavailable; producing .tar.gz only." >&2
  tool=""
fi
appimage_made=false
if [[ -n "$tool" ]]; then
  appimage="$dist/$artifact_prefix.AppImage"
  if ARCH=x86_64 "$tool" --appimage-extract-and-run "$appdir" "$appimage"; then
    echo "wrote $appimage"
    appimage_made=true
  else
    echo "WARN: AppImage build failed." >&2
  fi
fi
if [[ "$appimage_made" != true ]]; then
  if [[ "$dev" == true ]]; then
    echo "WARN: no AppImage produced; developer package is .tar.gz only." >&2
  else
    # A release job must not report success without the AppImage. The .tar.gz
    # alone is a developer artifact, not a release deliverable.
    echo "ERROR: no AppImage produced (appimagetool unavailable or its build failed);" >&2
    echo "       refusing to complete the release package. Re-run with --dev for tar-only." >&2
    exit 1
  fi
fi
