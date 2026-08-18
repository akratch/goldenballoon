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
readonly PHONE_PARTY_NOTICE_SHA256="dc48863706380100072297911937267b5eaee28a40a972516e07b285cc7635dd"

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
      Golden-Balloon.AppDir/NativePhoneParty-NOTICES.txt \
      Golden-Balloon.AppDir/README.md \
      Golden-Balloon.AppDir/RUN_ME.txt \
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
  local staged_notice_hash archive_notice_hash
  staged_notice_hash="$(sha256sum \
    "$staged/NativePhoneParty-NOTICES.txt" | awk '{print $1}')"
  archive_notice_hash="$(tar -xOzf "$archive" \
    Golden-Balloon.AppDir/NativePhoneParty-NOTICES.txt | sha256sum | awk '{print $1}')"
  if [[ "$staged_notice_hash" != "$PHONE_PARTY_NOTICE_SHA256" ||
        "$archive_notice_hash" != "$PHONE_PARTY_NOTICE_SHA256" ]]; then
    echo "ERROR: Linux package carries an unreviewed native Phone Party notice." >&2
    return 1
  fi
}

if [[ "$self_test" == true ]]; then
  dev=false
  test_root="$(mktemp -d "${TMPDIR:-/tmp}/mdkr-linux-package-test.XXXXXX")"
  trap 'rm -rf "$test_root"' EXIT
  test_appdir="$test_root/Golden-Balloon.AppDir"
  mkdir -p "$test_appdir/usr/bin" "$test_appdir/usr/lib"
  for path in AppRun LICENSE README.md RUN_ME.txt mdkr64.desktop mdkr64.png; do
    : >"$test_appdir/$path"
  done
  cp third_party/native_phone_party/NOTICE.txt \
    "$test_appdir/NativePhoneParty-NOTICES.txt"
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
# appimagetool otherwise fetches the mutable `continuous` runtime internally
# without verifying it. Fetch it ourselves and fail closed on any upstream
# change so one reviewed source commit cannot silently produce different
# executable bytes.
APPIMAGE_RUNTIME_SHA256="1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf"
APPIMAGE_RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"

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
if ! cp third_party/native_phone_party/NOTICE.txt \
    "$appdir/NativePhoneParty-NOTICES.txt"; then
  if [[ "$dev" == true ]]; then
    echo "WARN: native Phone Party notices are unavailable in developer package." >&2
  else
    echo "ERROR: native Phone Party notices are required in the release package." >&2
    exit 1
  fi
fi

cat > "$appdir/RUN_ME.txt" <<'EOF'
Golden Balloon (Linux x86-64 portable build)

AppImage:
1. Copy the AppImage to a writable folder and make it executable:
     chmod +x Golden-Balloon-*-linux-x86_64.AppImage
2. Run it by double-clicking it or from a terminal.

Tarball fallback:
1. Extract the entire archive to a writable folder you own.
2. Run Golden-Balloon.AppDir/AppRun. Do not move AppRun away from its folder.

The Linux launcher intentionally does not search your disk or open a native
file picker. Drag your legally dumped ROM onto the launcher, or paste its
absolute path. US 1.1 and European 1.1 .z64, .v64, and .n64 dumps are supported.

WebGPU is the default and requires a current host graphics driver. If startup
fails, copy the Diagnostics details first. `MDKR_RENDERER=gl` selects the
diagnostic OpenGL backend; it is useful for narrowing down a driver problem,
not the recommended presentation path. Press F1 in-game for the pause overlay.

This app ships no game data. See README.md for controls and support details.
EOF

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

runtime=""
if [[ -n "$tool" ]]; then
  runtime="$work/runtime-x86_64"
  runtime_fetched=""
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$runtime" "$APPIMAGE_RUNTIME_URL" && runtime_fetched=1 || runtime_fetched=""
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$runtime" "$APPIMAGE_RUNTIME_URL" && runtime_fetched=1 || runtime_fetched=""
  fi
  if [[ -n "$runtime_fetched" && -f "$runtime" ]]; then
    runtime_got="$(sha256_of "$runtime")"
    if [[ -z "$runtime_got" ]]; then
      echo "ERROR: no sha256 tool available to verify the AppImage runtime." >&2
      exit 1
    elif [[ "$runtime_got" != "$APPIMAGE_RUNTIME_SHA256" ]]; then
      # The runtime's only upstream home is the mutable `continuous` tag, so a
      # legitimate upstream rebuild changes these bytes with no new release to
      # re-pin against. A release must still fail closed -- unreviewed runtime
      # bytes cannot be published -- but a developer package has the same
      # tarball-only escape the download-failure branch below already gives it,
      # so an upstream rebuild cannot block local packaging work.
      echo "WARN: AppImage runtime digest mismatch:" >&2
      echo "       got      ${runtime_got}" >&2
      echo "       expected ${APPIMAGE_RUNTIME_SHA256}" >&2
      if [[ "$dev" == true ]]; then
        echo "WARN: unpinned AppImage runtime rejected; producing .tar.gz only." >&2
        tool=""
        runtime=""
      else
        echo "ERROR: refusing release package with an unpinned AppImage runtime." >&2
        echo "       Re-pin APPIMAGE_RUNTIME_SHA256 after reviewing the new bytes," >&2
        echo "       or re-run with --dev for tar-only." >&2
        exit 1
      fi
    else
      echo "AppImage runtime verified (sha256 ${APPIMAGE_RUNTIME_SHA256:0:12}...)"
    fi
  elif [[ "$dev" == true ]]; then
    echo "WARN: pinned AppImage runtime download unavailable; producing .tar.gz only." >&2
    tool=""
    runtime=""
  else
    echo "ERROR: pinned AppImage runtime download unavailable; refusing release package." >&2
    exit 1
  fi
fi

appimage_made=false
if [[ -n "$tool" ]]; then
  appimage="$dist/$artifact_prefix.AppImage"
  if ARCH=x86_64 "$tool" --appimage-extract-and-run \
      --runtime-file "$runtime" "$appdir" "$appimage"; then
    echo "wrote $appimage"
    appimage_made=true
  # Some binfmt-restricted environments (x86_64 containers under CPU
  # emulation) refuse to exec the AppImage-runtime ELF at all, so the
  # --appimage-extract-and-run flag above is never reached. The payload is
  # still the same sha256-verified bytes: unpack its squashfs directly and
  # run the inner AppRun. Direct exec succeeds everywhere else, so this
  # branch stays dormant on the hosted lanes.
  elif command -v unsquashfs >/dev/null 2>&1; then
    echo "WARN: direct appimagetool exec failed; extracting its payload instead." >&2
    # readelf's "Start of section headers" line ends in "(bytes into file)",
    # so the number is field 5 on all three lines, never $NF.
    tool_offset="$(readelf -h "$tool" | awk '
      /Start of section headers/ { start = $5 }
      /Size of section headers/ { size = $5 }
      /Number of section headers/ { count = $5 }
      END { print start + size * count }')"
    if [[ -n "$tool_offset" ]] && \
        unsquashfs -q -o "$tool_offset" -d "$work/appimagetool-root" "$tool" && \
        ARCH=x86_64 "$work/appimagetool-root/AppRun" \
          --runtime-file "$runtime" "$appdir" "$appimage"; then
      echo "wrote $appimage (via extracted appimagetool payload)"
      appimage_made=true
    else
      echo "WARN: AppImage build failed (extracted-payload fallback)." >&2
    fi
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
