#!/usr/bin/env bash
#
# package_windows_zip.sh -- package the built Windows exe into a portable .zip
# as GoldenBalloon.exe (single file: SDL2 is statically linked; no DLLs ship).
#
# There is no separate GUI launcher in this package: mdkr64.exe, built from
# platform/main_pc.c, IS the game, and it is bring-your-own-ROM. RUN_ME.txt
# below therefore has to document the --rom flag and where to place a baserom,
# because nothing in the shipped zip can prompt the user for one.
#
# Runs in the release CI under MSYS2/MinGW64 (mirrors
# tools/mingw_cross_check.sh's toolchain -- MSYS2-pinned SDL2 2.32.10-1, same
# vendored/verified pin used for the local cross-compile lane), or locally on
# Windows. Produces:
#   dist/mdkr64-windows-<version>.zip
#
# The app ships NO game data (bring-your-own-ROM); nothing here embeds ROM bytes.
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

binary="build/mdkr64.exe"
version="dev"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) binary="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    -h|--help) echo "Usage: $0 [--binary PATH] [--version VER]"; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done
[[ -f "$binary" ]] || { echo "ERROR: binary not found: $binary" >&2; exit 1; }

dist="dist"; mkdir -p "$dist"
stage="$(mktemp -d)/MDKR64"
mkdir -p "$stage"
cp "$binary" "$stage/mdkr64.exe"

# Bundle the runtime DLLs the exe links (SDL2 + the MinGW runtime libs), resolved
# via ldd against the MSYS2 MinGW prefix (skip Windows system DLLs).
mingw_prefix="${MINGW_PREFIX:-/mingw64}"
ldd "$binary" | awk '{print $3}' | while read -r dll; do
  case "$dll" in
    "$mingw_prefix"/*) cp -u "$dll" "$stage/" && echo "bundled $(basename "$dll")" ;;
  esac
done
# Ensure SDL2.dll made it (belt and suspenders).
[[ -f "$stage/SDL2.dll" ]] || { [[ -f "$mingw_prefix/bin/SDL2.dll" ]] && cp "$mingw_prefix/bin/SDL2.dll" "$stage/"; }

cp LICENSE README.md "$stage/" 2>/dev/null || true
# Community controller-mapping DB (MC.2), next to the exe where SDL_GetBasePath()
# resolves it at controller init.
cp lib/sdl_gamecontrollerdb/gamecontrollerdb.txt "$stage/" 2>/dev/null || true
cat > "$stage/RUN_ME.txt" <<'EOF'
MDKR64 (Windows portable build)

1. Unzip to a writable folder you own (e.g. C:\Games\MDKR64), NOT Program Files.
2. Place your own legally-dumped Diddy Kong Racing ROM next to mdkr64.exe,
   named baserom.us.v80.z64, OR launch from a command prompt with
   `mdkr64.exe --rom PATH\TO\your.z64`.
3. Double-click mdkr64.exe (or run it from a command prompt to see log output).
4. This app ships NO game data. See README.md / DISCLAIMER.md.
EOF

( cd "$(dirname "$stage")" && zip -r -q "$OLDPWD/$dist/mdkr64-windows-$version.zip" MDKR64 )
echo "wrote $dist/mdkr64-windows-$version.zip"
rm -rf "$(dirname "$stage")"
