# macOS release signing

The release workflow builds a self-contained Apple-silicon app, signs every
nested library and the app with Developer ID + Hardened Runtime, notarizes and
staples the app, packages it into a signed DMG, notarizes and staples the DMG,
then requires Gatekeeper acceptance before upload. It never receives a ROM.

## One-time Apple setup

1. As the Apple Developer Account Holder, create a **Developer ID Application**
   certificate (not Apple Development, Mac Development, Mac App Distribution,
   or Developer ID Installer). Install it and its private key in Keychain
   Access, then export both together as a password-protected `.p12`.
2. In App Store Connect, under **Users and Access > Integrations > Team Keys**,
   create a team API key for notarization. An individual API key cannot be used
   by `notarytool`. Download its `AuthKey_XXXXXXXXXX.p8` file (Apple only offers
   the download once), and record the Key ID and Issuer ID.
3. In GitHub, create an environment named `macos-release`. Enable required
   reviewers and prevent untrusted branches from deploying to it.
4. Add these environment secrets:

   - `MACOS_CERTIFICATE_P12_BASE64` — base64 of the exported `.p12`
   - `MACOS_CERTIFICATE_PASSWORD` — the `.p12` export password
   - `APPLE_API_PRIVATE_KEY_BASE64` — base64 of the `.p8`
   - `APPLE_API_KEY_ID` — the App Store Connect key ID
   - `APPLE_API_ISSUER_ID` — the App Store Connect issuer ID

5. Add the environment variable `DEVELOPER_ID_APPLICATION` with the exact
   certificate name, for example
   `Developer ID Application: Your Name (ABCDEFGHIJ)`. Obtain the exact value
   with `security find-identity -v -p codesigning`.

On macOS, values can be loaded with GitHub CLI without printing the key bytes:

```bash
base64 -i DeveloperID.p12 | gh secret set MACOS_CERTIFICATE_P12_BASE64 --env macos-release
gh secret set MACOS_CERTIFICATE_PASSWORD --env macos-release
base64 -i AuthKey_XXXXXXXXXX.p8 | gh secret set APPLE_API_PRIVATE_KEY_BASE64 --env macos-release
gh secret set APPLE_API_KEY_ID --env macos-release
gh secret set APPLE_API_ISSUER_ID --env macos-release
gh variable set DEVELOPER_ID_APPLICATION --env macos-release \
  --body 'Developer ID Application: Your Name (ABCDEFGHIJ)'
```

Keep the `.p12`, `.p8`, and their passwords outside the repository. Revoke and
rotate either credential immediately if it is exposed.

## Running a release

A dry run produces a signed workflow artifact without modifying a GitHub
Release:

```bash
gh workflow run macos-release.yml -f version=1.0.1
```

To upload into an existing release after the protected-environment approval:

```bash
gh workflow run macos-release.yml \
  -f version=1.0.1 -f release_tag=v1.0.1
```

The workflow fails closed on an unavailable identity, invalid nested signature,
nonportable Homebrew load path, rejected notarization, missing staple, failed
Gatekeeper assessment, corrupt DMG, ROM-derived asset, or missing output. The
final artifact includes a SHA-256 file and commit-bound provenance sidecar.

For a local credential test, build with `build_app_bundle.sh --bundle-sdl2`,
export the same `DEVELOPER_ID_APPLICATION` and `APPLE_API_*` values, then run
`sign_and_notarize.sh`. `--skip-notarize` exists only for local certificate
diagnostics; the release workflow contains no notarization bypass.
