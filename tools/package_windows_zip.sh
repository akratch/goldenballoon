#!/usr/bin/env bash
#
# package_windows_zip.sh -- package the built Windows exe into a portable .zip
# as GoldenBalloon.exe (one executable: SDL2 is statically linked; no DLLs ship).
#
# The native app shell is part of the executable. A bare double-click opens the
# ROM picker; command-line flags remain available for automation and diagnostics.
#
# Runs in the release CI under MSYS2/MinGW64, where SDL2 is a hosted-repository
# build input, or locally on Windows. The separate mingw_cross_check.sh lane
# uses a hash-verified SDL2 archive; the package's import-table check proves the
# resulting zip is self-contained but does not claim source-package identity.
# Produces:
#   dist/Golden-Balloon-<version>-windows-x64.zip
#
# The app ships NO game data (bring-your-own-ROM); nothing here embeds ROM bytes.
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
readonly PHONE_PARTY_NOTICE_SHA256="dc48863706380100072297911937267b5eaee28a40a972516e07b285cc7635dd"

binary="build/mdkr64.exe"
version="dev"
self_test=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) binary="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    --self-test) self_test=true; shift ;;
    -h|--help) echo "Usage: $0 [--binary PATH] [--version VER] [--self-test]"; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done
if [[ "$version" != "dev" && ! "$version" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]]; then
  echo "ERROR: version must be dev or bare semver (for example 1.0.5)" >&2
  exit 1
fi

verify_windows_archive() {
  local archive="$1"
  local expected actual
  command -v unzip >/dev/null 2>&1 || {
    echo "ERROR: unzip is required to verify the Windows archive." >&2
    return 1
  }
  expected="$(printf '%s\n' \
    GoldenBalloon/ \
    GoldenBalloon/GoldenBalloon.exe \
    GoldenBalloon/LICENSE \
    GoldenBalloon/NativePhoneParty-NOTICES.txt \
    GoldenBalloon/README.md \
    GoldenBalloon/RUN_ME.txt \
    GoldenBalloon/gamecontrollerdb.txt | LC_ALL=C sort)"
  # MSYS2 unzip may emit CRLF for archive entries even though the Bash-built
  # expected manifest uses LF. Normalize only the listing transport; carriage
  # returns are not valid in any accepted manifest entry.
  actual="$(unzip -Z1 "$archive" | tr -d '\r' | LC_ALL=C sort)"
  if [[ "$actual" != "$expected" ]]; then
    echo "ERROR: Windows archive payload differs from the exact release manifest." >&2
    diff -u <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") >&2 || true
    return 1
  fi
  if printf '%s\n' "$actual" | grep -Eqi '(^|/)[^/]*\.dll$'; then
    echo "ERROR: Windows archive contains a DLL; SDL2 must remain statically linked." >&2
    return 1
  fi
  local notice_hash
  notice_hash="$(unzip -p "$archive" \
    GoldenBalloon/NativePhoneParty-NOTICES.txt | sha256sum | awk '{print $1}')"
  if [[ "$notice_hash" != "$PHONE_PARTY_NOTICE_SHA256" ]]; then
    echo "ERROR: Windows archive carries an unreviewed native Phone Party notice." >&2
    return 1
  fi
}

if [[ "$self_test" == true ]]; then
  test_root="$(mktemp -d "${TMPDIR:-/tmp}/mdkr-windows-package-test.XXXXXX")"
  trap 'rm -rf "$test_root"' EXIT
  mkdir -p "$test_root/GoldenBalloon"
  : >"$test_root/GoldenBalloon/GoldenBalloon.exe"
  : >"$test_root/GoldenBalloon/LICENSE"
  cp third_party/native_phone_party/NOTICE.txt \
    "$test_root/GoldenBalloon/NativePhoneParty-NOTICES.txt"
  : >"$test_root/GoldenBalloon/README.md"
  : >"$test_root/GoldenBalloon/RUN_ME.txt"
  : >"$test_root/GoldenBalloon/gamecontrollerdb.txt"
  ( cd "$test_root" && zip -r -q package.zip GoldenBalloon )
  verify_windows_archive "$test_root/package.zip"
  : >"$test_root/GoldenBalloon/SDL2.dll"
  ( cd "$test_root" && zip -q package.zip GoldenBalloon/SDL2.dll )
  if verify_windows_archive "$test_root/package.zip" >/dev/null 2>&1; then
    echo "ERROR: Windows archive verifier accepted the SDL2.dll control." >&2
    exit 1
  fi
  echo "package_windows_zip: self-test PASS"
  exit 0
fi

[[ -f "$binary" ]] || { echo "ERROR: binary not found: $binary" >&2; exit 1; }

# CMake links SDL2, libgcc, libstdc++, and winpthread statically for this target.
# Verify that contract before packaging; copying a build-machine DLL would hide
# a link regression and recreate the Explorer zip-preview failure this layout
# is designed to prevent.
./tools/check_windows_imports.sh "$binary"

dist="$(pwd)/dist"; mkdir -p "$dist"
package_root="$(mktemp -d "${TMPDIR:-/tmp}/mdkr-windows-package.XXXXXX")"
cleanup() { rm -rf "$package_root"; }
trap cleanup EXIT
stage="$package_root/GoldenBalloon"
mkdir -p "$stage"
cp "$binary" "$stage/GoldenBalloon.exe"

cp LICENSE README.md "$stage/"
cp third_party/native_phone_party/NOTICE.txt \
  "$stage/NativePhoneParty-NOTICES.txt"
# Community controller-mapping DB (MC.2), next to the exe where SDL_GetBasePath()
# resolves it at controller init.
cp lib/sdl_gamecontrollerdb/gamecontrollerdb.txt "$stage/"
cat > "$stage/RUN_ME.txt" <<'EOF'
Golden Balloon (Windows portable build)

1. Unzip the whole GoldenBalloon folder to a writable location you own.
2. Double-click GoldenBalloon.exe. The launcher asks you to choose your own
   legally dumped Diddy Kong Racing ROM (.z64, .v64, or .n64).
3. For command-line use: GoldenBalloon.exe --rom PATH\TO\your.z64

Windows 10 or 11 x64 and a current graphics driver are recommended. Unicode
and extended-length ROM, config, diagnostic, and save paths are supported.
Do not launch the executable from inside the zip: extract the whole folder so
settings and the controller database remain available.

Portable settings (for Windows account names with non-English characters):
if your Windows username contains non-English letters and settings will not
save, create an empty file named portable.txt in this GoldenBalloon folder
(next to GoldenBalloon.exe). The game then keeps your settings, saves, and any
add-ons in this folder instead of your Windows user folder. If that folder is
ever unwritable, the game falls back to saving here on its own and says so.

WebGPU is the default. If startup fails, copy the launcher's Diagnostics details
before closing it. `set MDKR_RENDERER=gl` selects the diagnostic OpenGL backend
for that Command Prompt session. Press F1 in-game for the pause overlay.

This app ships no game data. See README.md for controls and support details.
EOF

./tools/check_no_rom.sh "$stage"

archive="$dist/Golden-Balloon-$version-windows-x64.zip"
( cd "$package_root" && zip -r -q "$archive" GoldenBalloon )
verify_windows_archive "$archive"
echo "wrote $archive"
