# macOS release packaging

The 1.8.0 release intentionally skips Developer ID signing and
notarization. It is still sealed with an ad-hoc signature after every bundle
mutation. That signature provides the code/resource integrity Apple silicon
requires; it is not a trust signature. Players should see macOS's normal
unidentified-developer warning on first open, never a “damaged” app error.

## Unsigned 1.8.0 release (default)

Build only from a clean release commit. The provenance sidecar names `HEAD`, so
stamping an artifact made from uncommitted source would be dishonest.

```bash
set -euo pipefail
if [[ -n "$(git status --porcelain=v1 --untracked-files=all)" ]]; then
  echo "release build requires a clean tracked, staged, and untracked tree" >&2
  exit 1
fi

RELEASE_VERSION=1.8.0
SOURCE_COMMIT="$(git rev-parse HEAD)"
# Choose deliberately. Leave PARTY_ORIGIN empty only for a partyless release;
# otherwise set it to the deployed HTTPS origin and use cloud-enabled.
PARTY_ORIGIN=""
PHONE_PARTY_MODE=partyless
if [[ -n "$PARTY_ORIGIN" ]]; then
  [[ "$PHONE_PARTY_MODE" == cloud-enabled ]]
else
  [[ "$PHONE_PARTY_MODE" == partyless ]]
fi
source ./macos/Scripts/release_sdl2_config.sh
SDL_PREFIX="$PWD/build-macos-deps/sdl2-${MDKR_RELEASE_SDL2_VERSION}/install"
PYTHON_313="${PYTHON_313:-/opt/homebrew/bin/python3.13}"
TOOL_DOWNLOAD_DIR="$(mktemp -d)"
trap 'rm -rf "$TOOL_DOWNLOAD_DIR"' EXIT
[[ "$("$PYTHON_313" --version)" == "Python 3.13.13" ]]

./macos/Scripts/build_release_sdl2.sh \
  --work-dir "build-macos-deps/sdl2-${MDKR_RELEASE_SDL2_VERSION}" \
  --prefix "$SDL_PREFIX" \
  --arch arm64 \
  --deployment-target 13.0

"$PYTHON_313" -m venv build-tools/python
build-tools/python/bin/python -m pip install --disable-pip-version-check \
  --require-hashes --only-binary=:all: \
  -r tools/character_importer_build_requirements.txt
mkdir -p build-tools/validators
curl -fsSL -o "$TOOL_DOWNLOAD_DIR/gltf-validator-source.tar.gz" \
  https://github.com/KhronosGroup/glTF-Validator/archive/bcd52cc4ba5f333b2999a58f67cc05ddf28b4fb1.tar.gz
curl -fsSL -o "$TOOL_DOWNLOAD_DIR/dart-sdk-macos-arm64.zip" \
  https://storage.googleapis.com/dart-archive/channels/stable/release/2.19.6/sdk/dartsdk-macos-arm64-release.zip
build-tools/python/bin/python tools/build_gltf_validator.py \
  --source-archive "$TOOL_DOWNLOAD_DIR/gltf-validator-source.tar.gz" \
  --dart-sdk-archive "$TOOL_DOWNLOAD_DIR/dart-sdk-macos-arm64.zip" \
  --output build-tools/validators/gltf_validator
build-tools/python/bin/python tools/build_character_importer.py \
  --output build-tools/character_importer
build-tools/python/bin/python tests/check_frozen_character_importer.py \
  build-tools/character_importer

PKG_CONFIG_PATH="$SDL_PREFIX/lib/pkgconfig" \
./macos/Scripts/build_app_bundle.sh \
  --release \
  --allow-online-beta \
  --party-origin "$PARTY_ORIGIN" \
  --build-dir build-macos-release \
  --output "dist/Golden Balloon.app" \
  --arch arm64 \
  --version "$RELEASE_VERSION" \
  --build-stamp "$SOURCE_COMMIT" \
  --deployment-target 13.0 \
  --strict-deployment-target \
  --bundle-sdl2 \
  --character-importer build-tools/character_importer \
  --character-importer-manifest build-tools/character_importer.manifest.json \
  --gltf-validator build-tools/validators/gltf_validator \
  --gltf-validator-manifest build-tools/validators/gltf_validator.manifest.json \
  --character-lod-tool build-macos-release/tools/mdkr-character-lod

./macos/Scripts/verify_unsigned_release.sh \
  --version "$RELEASE_VERSION" \
  --commit "$SOURCE_COMMIT" \
  "dist/Golden Balloon.app"

DMG_PATH="dist/Golden-Balloon-${RELEASE_VERSION}-macos-arm64-unsigned.dmg"
./macos/Scripts/create_dmg.sh "dist/Golden Balloon.app" "$DMG_PATH"
./macos/Scripts/verify_unsigned_dmg.sh \
  --version "$RELEASE_VERSION" --commit "$(git rev-parse HEAD)" "$DMG_PATH"
./macos/Scripts/stamp_macos_provenance.sh \
  --signing ad-hoc-unsigned \
  --phone-party "$PHONE_PARTY_MODE" \
  "$DMG_PATH" "$RELEASE_VERSION"
(
  cd "$(dirname "$DMG_PATH")"
  DMG_NAME="$(basename "$DMG_PATH")"
  shasum -a 256 "$DMG_NAME" > "$DMG_NAME.sha256"
)
./tools/release/verify_provenance.sh \
  --dist "$(dirname "$DMG_PATH")" \
  --version "$RELEASE_VERSION" \
  --commit "$SOURCE_COMMIT" \
  --require-asset "Golden-Balloon-${RELEASE_VERSION}-macos-arm64-unsigned.dmg" \
  --require platform=macos \
  --require macos_signing=ad-hoc-unsigned \
  --require "phone_party=$PHONE_PARTY_MODE"
```

The untracked-file check is release-critical: CMake discovers native app-shell
translation units under `platform/app/`, so an untracked source file must not
enter a binary whose provenance names the committed `HEAD`. The command also
requires the exact CPython 3.13.13 runtime; the importer builder rejects
any other patch version. Use fresh `build-tools`, `build-macos-deps`, and
`build-macos-release` directories because the attested helper builders refuse
to overwrite output. The validator source, Dart SDK, importer wheels, SDL
source, and produced helpers are checked against repository-pinned digests
before packaging.

`build_release_sdl2.sh` downloads the official SDL2 2.32.10 archive and checks
its pinned SHA-256 before extraction. It builds a real standalone SDL2 dylib
for macOS 13. The packager rejects Homebrew's `sdl2-compat` shim because that
shim loads SDL3 dynamically and copying the shim alone creates a bundle that
only works on machines where SDL3 happens to be installed.

The final verifier checks the nested signatures and resource seal, every Mach-O
architecture/minimum-OS/load-path contract, compiled and plist versions,
compiled source commit, asset-free policy, WebGPU default startup, a real
launcher pixel capture, and the absence of Homebrew or SDL3 runtime loads.
At runtime the sealed `Contents/Resources` tree is addressed only as an
immutable absolute resource root. Video configuration and game saves live under
`SDL_GetPrefPath("mdkr64", "mdkr64")`; the app never changes into Resources or
writes through its signature seal.
The DMG filename ends in `-unsigned.dmg`, and its provenance sidecar records
`"macos_signing": "ad-hoc-unsigned"` plus the deliberate `phone_party` mode;
neither can be mistaken for a notarized Developer ID build or for an artifact
with a different cloud-Phone-Party policy.

The equivalent protected workflow command is:

```bash
gh workflow run macos-release.yml \
  -f version=1.8.0 \
  -f trusted_signing=false
```

Leave `release_tag` empty while producing a test artifact. Publishing is
allowed only when it is exactly `v1.8.0` and that tag resolves to the workflow's
source commit; both the package and publish jobs enforce that binding.

## Human candidate play-test

Do this against the exact DMG and its two sidecars produced above, before
tagging or publishing anything:

1. In the artifact directory, run
   `shasum -a 256 -c Golden-Balloon-1.8.0-macos-arm64-unsigned.dmg.sha256`.
2. Open the DMG and drag `Golden Balloon.app` into a new, empty test folder. Launch that
   copy from Finder, with no `MDKR_RENDERER` environment override.
3. If macOS blocks the unidentified developer, first attempt the launch, then
   open **System Settings → Privacy & Security**, choose **Open Anyway**, and
   confirm **Open**. A “damaged” message is a hard failure.
4. Open the launcher's **Diagnostics** panel and confirm `Renderer: webgpu`.
   Select a supported ROM and verify the intro, title screen, menus, and a race
   have no fractured sky/terrain or repeated-logo corruption.
5. Confirm the frame limit initially reads **Original**. Exercise one numeric
   limit and **Uncapped**, then return to **Original**; neither transition may
   crash, accelerate gameplay, corrupt the image, or leave audio/input changed.

Keep the release tag and publication step blocked until this exact copied app
passes. The automated mounted-DMG LaunchServices/WebGPU check is mandatory, but
it does not replace this final human gameplay check.
If the build exposes Character Workshop, also complete section 5b of
[`docs/RELEASE_CANDIDATE_TEST_GUIDE.md`](../docs/RELEASE_CANDIDATE_TEST_GUIDE.md)
against this installed copy and validate its acceptance receipt against the DMG
and provenance sidecar bytes.

## Optional trusted release

The Developer ID/notarization path remains available for a later release. Set
up the protected `macos-release` environment with a **Developer ID Application**
certificate and App Store Connect team API key, then dispatch:

```bash
gh workflow run macos-release.yml \
  -f version=1.8.0 \
  -f trusted_signing=true
```

Required environment secrets are `MACOS_CERTIFICATE_P12_BASE64`,
`MACOS_CERTIFICATE_PASSWORD`, `APPLE_API_PRIVATE_KEY_BASE64`,
`APPLE_API_KEY_ID`, and `APPLE_API_ISSUER_ID`. Set
`DEVELOPER_ID_APPLICATION` to the exact certificate name. Keep all credentials
outside the repository and rotate them immediately if exposed.

With `trusted_signing=true`, the workflow signs nested code and the app with
Developer ID + Hardened Runtime, notarizes and staples the app, signs and
notarizes the DMG, and requires Gatekeeper acceptance. There is no
`--skip-notarize` path in the workflow. For 1.8.0, that optional artifact is
exactly `Golden-Balloon-1.8.0-macos-arm64-signed-notarized.dmg` and records
`developer-id-notarized` in provenance.

Gatekeeper acceptance is a static trust check, not a renderer smoke. Before a
trusted artifact can be published, run a dedicated **post-sign and post-staple**
LaunchServices test against the final app that requires the same WebGPU-default
four-present/capture telemetry and standalone-SDL runtime-load audit as the
unsigned verifier. `verify_unsigned_release.sh` intentionally rejects a trusted
authority, so it cannot stand in for that signed-runtime gate. The protected
workflow's signed lane is not release-qualified until that final runtime gate
is implemented and passes; do not infer runtime health from the earlier
ad-hoc-bundle smoke. The workflow enforces this itself: a dispatch that combines
`trusted_signing=true` with a non-empty `release_tag` is rejected in the
input-validation step, so a trusted artifact can be produced for inspection but
not published.
