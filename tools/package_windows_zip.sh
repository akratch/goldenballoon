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
readonly PHONE_PARTY_NOTICE_SHA256="dd753a65bb8de6de10a5bfc5de8ff6ae3690e333466c034ef4efbaa7ddb54a20"

# Trimmed local-play (no internet) controller assets staged beside the exe
# (Task 5), emitted as the exact zip entries they produce: each file plus the
# directory entries `zip -r` records for it. Source of truth:
# tools/lan_controller_assets.txt, cross-checked against the C++ manifest builder
# by tests/check_lan_controller_assets.py.
lan_web_zip_entries() {
  {
    printf 'GoldenBalloon/dist/\n'
    printf 'GoldenBalloon/dist/web/\n'
    while IFS= read -r asset; do
      case "$asset" in ''|\#*) continue ;; esac
      printf 'GoldenBalloon/dist/web/%s\n' "$asset"
      printf 'GoldenBalloon/dist/web/%s/\n' "${asset%%/*}"
    done < tools/lan_controller_assets.txt
  } | LC_ALL=C sort -u
}

# Copy the trimmed controller assets into $1/dist/web, failing closed on any
# missing source file.
stage_lan_web_assets() {
  local stage_dir="$1" asset src dest
  while IFS= read -r asset; do
    case "$asset" in ''|\#*) continue ;; esac
    src="dist/web/$asset"
    dest="$stage_dir/dist/web/$asset"
    [[ -s "$src" ]] || {
      echo "ERROR: local-play controller asset missing or empty: $src" >&2
      return 1
    }
    mkdir -p "$(dirname "$dest")"
    cp "$src" "$dest"
  done < tools/lan_controller_assets.txt
}

binary="build/mdkr64.exe"
character_importer=""
character_importer_manifest=""
gltf_validator=""
gltf_validator_manifest=""
character_lod_tool=""
version="dev"
self_test=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) binary="$2"; shift 2 ;;
    --character-importer) character_importer="$2"; shift 2 ;;
    --character-importer-manifest) character_importer_manifest="$2"; shift 2 ;;
    --gltf-validator) gltf_validator="$2"; shift 2 ;;
    --gltf-validator-manifest) gltf_validator_manifest="$2"; shift 2 ;;
    --character-lod-tool) character_lod_tool="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    --self-test) self_test=true; shift ;;
    -h|--help) echo "Usage: $0 [--binary PATH] --character-importer PATH --character-importer-manifest PATH --gltf-validator PATH --gltf-validator-manifest PATH --character-lod-tool PATH [--version VER] [--self-test]"; exit 0 ;;
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
  local web_entries
  web_entries="$(lan_web_zip_entries)"
  expected="$(printf '%s\n' \
    GoldenBalloon/ \
    GoldenBalloon/GoldenBalloon.exe \
    GoldenBalloon/tools/ \
    GoldenBalloon/tools/character_importer.exe \
    GoldenBalloon/tools/character_importer.exe.manifest.json \
    GoldenBalloon/tools/mdkr-character-lod.exe \
    GoldenBalloon/tools/CPython-LICENSE.txt \
    GoldenBalloon/tools/PyInstaller-COPYING.txt \
    GoldenBalloon/tools/validators/ \
    GoldenBalloon/tools/validators/gltf_validator.exe \
    GoldenBalloon/tools/validators/gltf_validator.exe.manifest.json \
    GoldenBalloon/tools/validators/LICENSE.txt \
    GoldenBalloon/tools/validators/NOTICES.txt \
    GoldenBalloon/LICENSE \
    GoldenBalloon/BasisU-LICENSE.txt \
    GoldenBalloon/BasisU-Zstd-LICENSE.txt \
    GoldenBalloon/BasisU-README.md \
    GoldenBalloon/CharacterText-HarfBuzz-COPYING.txt \
    GoldenBalloon/CharacterText-SheenBidi-LICENSE.txt \
    GoldenBalloon/Meshoptimizer-LICENSE.md \
    GoldenBalloon/Meshoptimizer-README.md \
    GoldenBalloon/NativePhoneParty-NOTICES.txt \
    GoldenBalloon/README.md \
    GoldenBalloon/RUN_ME.txt \
    GoldenBalloon/gamecontrollerdb.txt \
    ${web_entries} | LC_ALL=C sort)"
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
  command -v python3 >/dev/null 2>&1 || {
    echo "ERROR: python3 is required to verify raw Windows archive bytes." >&2
    return 1
  }
  notice_hash="$(python3 - "$archive" <<'PY'
import hashlib
import json
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1], "r") as archive:
    payload = archive.read("GoldenBalloon/NativePhoneParty-NOTICES.txt")
    importer = archive.read("GoldenBalloon/tools/character_importer.exe")
    manifest = json.loads(archive.read(
        "GoldenBalloon/tools/character_importer.exe.manifest.json"))
    if manifest.get("executable") != "character_importer.exe":
        raise SystemExit("packaged importer manifest names another executable")
    if manifest.get("executable_bytes") != len(importer):
        raise SystemExit("packaged importer size differs from its manifest")
    if manifest.get("executable_sha256") != hashlib.sha256(importer).hexdigest():
        raise SystemExit("packaged importer hash differs from its manifest")
    notices = {
        "GoldenBalloon/tools/CPython-LICENSE.txt":
            "78b12c3a81360b357002334f0e70ea0e92eebf7a9b358805c03c48484945f3bb",
        "GoldenBalloon/tools/PyInstaller-COPYING.txt":
            "dcf75fdb959db1e3b41c0f8505069d2ece781b5ec6b3d0a4d30975cfc6580245",
        "GoldenBalloon/BasisU-LICENSE.txt":
            "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4",
        "GoldenBalloon/BasisU-Zstd-LICENSE.txt":
            "2c1a7fa704df8f3a606f6fc010b8b5aaebf403f3aeec339a12048f1ba7331a0b",
        "GoldenBalloon/BasisU-README.md":
            "5336f7e852dcc067ffaa0a12c3d36892d7fe7c09ddf74a035da6a2811d0d4cbf",
        "GoldenBalloon/CharacterText-HarfBuzz-COPYING.txt":
            "ba8f810f2455c2f08e2d56bb49b72f37fcf68f1f4fade38977cfd7372050ad64",
        "GoldenBalloon/CharacterText-SheenBidi-LICENSE.txt":
            "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30",
        "GoldenBalloon/Meshoptimizer-LICENSE.md":
            "f03037ca7bad1e3eb7f4a63fa6084a8baabd5ba30d3c239a9a7f35705d873e26",
    }
    for name, expected in notices.items():
        if hashlib.sha256(archive.read(name)).hexdigest() != expected:
            raise SystemExit(f"packaged importer notice changed: {name}")
    validator = archive.read(
        "GoldenBalloon/tools/validators/gltf_validator.exe")
    validator_manifest = json.loads(archive.read(
        "GoldenBalloon/tools/validators/gltf_validator.exe.manifest.json"))
    if validator_manifest.get("executable") != "gltf_validator.exe":
        raise SystemExit("packaged validator manifest names another executable")
    if validator_manifest.get("executable_bytes") != len(validator):
        raise SystemExit("packaged validator size differs from its manifest")
    if validator_manifest.get("executable_sha256") != hashlib.sha256(
            validator).hexdigest():
        raise SystemExit("packaged validator hash differs from its manifest")
    validator_notices = {
        "GoldenBalloon/tools/validators/LICENSE.txt":
            "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30",
        "GoldenBalloon/tools/validators/NOTICES.txt":
            "d7a1cefe85110c1308632d0384b7a67a18c125193e54175c50d1982d8c81a2f4",
    }
    for name, expected in validator_notices.items():
        if hashlib.sha256(archive.read(name)).hexdigest() != expected:
            raise SystemExit(f"packaged validator notice changed: {name}")
print(hashlib.sha256(payload).hexdigest())
PY
)"
  if [[ "$notice_hash" != "$PHONE_PARTY_NOTICE_SHA256" ]]; then
    echo "ERROR: Windows archive carries an unreviewed native Phone Party notice." >&2
    return 1
  fi
}

if [[ "$self_test" == true ]]; then
  test_root="$(mktemp -d "${TMPDIR:-/tmp}/mdkr-windows-package-test.XXXXXX")"
  trap 'rm -rf "$test_root"' EXIT
  mkdir -p "$test_root/GoldenBalloon/tools/validators"
  : >"$test_root/GoldenBalloon/GoldenBalloon.exe"
  : >"$test_root/GoldenBalloon/LICENSE"
  cp third_party/basisu/LICENSE.txt \
    "$test_root/GoldenBalloon/BasisU-LICENSE.txt"
  cp third_party/basisu/Zstd-LICENSE.txt \
    "$test_root/GoldenBalloon/BasisU-Zstd-LICENSE.txt"
  cp third_party/basisu/README.md \
    "$test_root/GoldenBalloon/BasisU-README.md"
  cp third_party/character_text/HarfBuzz-COPYING.txt \
    "$test_root/GoldenBalloon/CharacterText-HarfBuzz-COPYING.txt"
  cp third_party/gltf_validator/LICENSE.txt \
    "$test_root/GoldenBalloon/CharacterText-SheenBidi-LICENSE.txt"
  cp third_party/meshoptimizer/LICENSE.md \
    "$test_root/GoldenBalloon/Meshoptimizer-LICENSE.md"
  cp third_party/meshoptimizer/README.md \
    "$test_root/GoldenBalloon/Meshoptimizer-README.md"
  tr -d '\r' < third_party/native_phone_party/NOTICE.txt \
    > "$test_root/GoldenBalloon/NativePhoneParty-NOTICES.txt"
  : >"$test_root/GoldenBalloon/README.md"
  : >"$test_root/GoldenBalloon/RUN_ME.txt"
  : >"$test_root/GoldenBalloon/gamecontrollerdb.txt"
  : >"$test_root/GoldenBalloon/tools/character_importer.exe"
  : >"$test_root/GoldenBalloon/tools/mdkr-character-lod.exe"
  cp third_party/character_importer/CPython-LICENSE.txt \
    "$test_root/GoldenBalloon/tools/CPython-LICENSE.txt"
  cp third_party/character_importer/PyInstaller-COPYING.txt \
    "$test_root/GoldenBalloon/tools/PyInstaller-COPYING.txt"
  : >"$test_root/GoldenBalloon/tools/validators/gltf_validator.exe"
  cp third_party/gltf_validator/LICENSE.txt \
    "$test_root/GoldenBalloon/tools/validators/LICENSE.txt"
  cp third_party/gltf_validator/NOTICES.txt \
    "$test_root/GoldenBalloon/tools/validators/NOTICES.txt"
  python3 - "$test_root/GoldenBalloon/tools/character_importer.exe.manifest.json" <<'PY'
import json
import sys

with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump({
        "executable": "character_importer.exe",
        "executable_bytes": 0,
        "executable_sha256":
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    }, stream)
PY
  python3 - "$test_root/GoldenBalloon/tools/validators/gltf_validator.exe.manifest.json" <<'PY'
import json
import sys

with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump({
        "executable": "gltf_validator.exe",
        "executable_bytes": 0,
        "executable_sha256":
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    }, stream)
PY
  while IFS= read -r asset; do
    case "$asset" in ''|\#*) continue ;; esac
    mkdir -p "$test_root/GoldenBalloon/dist/web/$(dirname "$asset")"
    : >"$test_root/GoldenBalloon/dist/web/$asset"
  done < tools/lan_controller_assets.txt
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
[[ -n "$character_importer" && -f "$character_importer" ]] || {
  echo "ERROR: --character-importer must name the frozen Windows helper." >&2
  exit 1
}
[[ -n "$character_importer_manifest" && -f "$character_importer_manifest" ]] || {
  echo "ERROR: --character-importer-manifest must name its attestation." >&2
  exit 1
}
[[ -n "$gltf_validator" && -f "$gltf_validator" ]] || {
  echo "ERROR: --gltf-validator must name the native Windows validator." >&2
  exit 1
}
[[ -n "$gltf_validator_manifest" && -f "$gltf_validator_manifest" ]] || {
  echo "ERROR: --gltf-validator-manifest must name its attestation." >&2
  exit 1
}
[[ -n "$character_lod_tool" && -f "$character_lod_tool" ]] || {
  echo "ERROR: --character-lod-tool must name the native Windows LOD helper." >&2
  exit 1
}

python3 tools/verify_character_importer.py \
  --executable "$character_importer" \
  --manifest "$character_importer_manifest" \
  --target windows-x86_64
python3 tools/verify_gltf_validator.py \
  --executable "$gltf_validator" \
  --manifest "$gltf_validator_manifest" \
  --target windows-x86_64

# CMake links SDL2, libgcc, libstdc++, and winpthread statically for this target.
# Verify that contract before packaging; copying a build-machine DLL would hide
# a link regression and recreate the Explorer zip-preview failure this layout
# is designed to prevent.
./tools/check_windows_imports.sh "$binary"
# The frozen importer's CRT is inside it; see --static-crt in that script.
./tools/check_windows_imports.sh --static-crt "$character_importer"
# Dart AOT, CRT linked in; same reasoning as the importer above.
./tools/check_windows_imports.sh --static-crt "$gltf_validator"
./tools/check_windows_imports.sh "$character_lod_tool"

dist="$(pwd)/dist"; mkdir -p "$dist"
package_root="$(mktemp -d "${TMPDIR:-/tmp}/mdkr-windows-package.XXXXXX")"
cleanup() { rm -rf "$package_root"; }
trap cleanup EXIT
stage="$package_root/GoldenBalloon"
mkdir -p "$stage"
cp "$binary" "$stage/GoldenBalloon.exe"
mkdir -p "$stage/tools"
cp "$character_importer" "$stage/tools/character_importer.exe"
cp "$character_lod_tool" "$stage/tools/mdkr-character-lod.exe"
cp "$character_importer_manifest" \
  "$stage/tools/character_importer.exe.manifest.json"
cp third_party/character_importer/CPython-LICENSE.txt \
  third_party/character_importer/PyInstaller-COPYING.txt "$stage/tools/"
mkdir -p "$stage/tools/validators"
cp "$gltf_validator" "$stage/tools/validators/gltf_validator.exe"
cp "$gltf_validator_manifest" \
  "$stage/tools/validators/gltf_validator.exe.manifest.json"
cp third_party/gltf_validator/LICENSE.txt \
  third_party/gltf_validator/NOTICES.txt "$stage/tools/validators/"

cp LICENSE README.md "$stage/"
cp third_party/basisu/LICENSE.txt "$stage/BasisU-LICENSE.txt"
cp third_party/basisu/Zstd-LICENSE.txt "$stage/BasisU-Zstd-LICENSE.txt"
cp third_party/basisu/README.md "$stage/BasisU-README.md"
cp third_party/character_text/HarfBuzz-COPYING.txt \
  "$stage/CharacterText-HarfBuzz-COPYING.txt"
cp third_party/gltf_validator/LICENSE.txt \
  "$stage/CharacterText-SheenBidi-LICENSE.txt"
cp third_party/meshoptimizer/LICENSE.md "$stage/Meshoptimizer-LICENSE.md"
cp third_party/meshoptimizer/README.md "$stage/Meshoptimizer-README.md"
# A Windows Git checkout may materialize tracked text with CRLF. Canonicalize
# the distributed notice to the reviewed LF byte sequence pinned above.
tr -d '\r' < third_party/native_phone_party/NOTICE.txt \
  > "$stage/NativePhoneParty-NOTICES.txt"
# Community controller-mapping DB (MC.2), next to the exe where SDL_GetBasePath()
# resolves it at controller init.
cp lib/sdl_gamecontrollerdb/gamecontrollerdb.txt "$stage/"
# Local-only Phone Party controller page, staged beside the exe where
# SDL_GetBasePath() resolves dist/web at LAN start (Task 5).
stage_lan_web_assets "$stage"
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

Portable data: non-English account and folder names work normally. To
deliberately keep settings, saves, and add-ons in this GoldenBalloon folder,
create an empty file named portable.txt next to GoldenBalloon.exe. Without it,
an unwritable user-data folder falls back here and reports the location instead
of losing settings.

WebGPU is the default. If startup fails, copy the launcher's Diagnostics details
before closing it. `set MDKR_RENDERER=gl` selects the diagnostic OpenGL backend
for that Command Prompt session. Press F1 in-game for the pause overlay.

This app ships no game data. See README.md for controls and support details.
The complete Character Workshop importer and its pinned Khronos glTF Validator
are bundled; no Python installation is required. Their terms are in the tools
and tools\validators folders.
EOF

./tools/check_no_rom.sh "$stage"

archive="$dist/Golden-Balloon-$version-windows-x64.zip"
( cd "$package_root" && zip -r -q "$archive" GoldenBalloon )
verify_windows_archive "$archive"
echo "wrote $archive"
