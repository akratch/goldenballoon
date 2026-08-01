# Release-candidate test guide

This is the short human acceptance pass for Golden Balloon 1.0.1. Use the
published files from the [`v1.0.1` release](https://github.com/akratch/goldenballoon/releases/tag/v1.0.1),
not a local rebuild. They are bound to source commit
`aff75ca1b54dc76bf0238f0c6649f5470972ff2d`.

Use a legally owned US 1.1 or European 1.1 ROM. Do not attach ROMs, saves, game
screenshots, video, or audio to a bug report; share only text logs and hashes.

## 1. Verify the exact candidate

### Windows x64

File: `Golden-Balloon-1.0.1-windows-x64.zip`

SHA-256:
`e3be4045045e003a78113713a92145287629112401f585f26e2288b283cb4aac`

In PowerShell:

```powershell
(Get-FileHash .\Golden-Balloon-1.0.1-windows-x64.zip -Algorithm SHA256).Hash.ToLower()
```

Extract the ZIP into a writable, reasonably short, ASCII-only path such as
`C:\GB101`; run `C:\GB101\GoldenBalloon\GoldenBalloon.exe`, never the copy still
inside the ZIP. Keep the ROM in an ASCII-only path for this release. If the
launcher cannot save an otherwise valid ROM choice, run this from Command
Prompt and report the failure:

```bat
cd /d C:\GB101\GoldenBalloon
GoldenBalloon.exe --rom C:\GB101\roms\game.z64
```

### macOS Apple silicon

Files: `Golden-Balloon-1.0.1-macos-arm64-unsigned.dmg` and its `.sha256`
sidecar.

SHA-256:
`4dab44e0abe761f0988f890cc14f0d9c1e6e9b6c2c61c11afdeafe06217c25be`

In Terminal, with both files in the same directory:

```bash
shasum -a 256 -c Golden-Balloon-1.0.1-macos-arm64-unsigned.dmg.sha256
```

Mount the DMG, drag `mdkr64.app` to `/Applications`, and launch it from Finder
without renderer environment variables. The unidentified-developer warning is
expected; use **System Settings → Privacy & Security → Open Anyway**. A
“damaged” message is a failure.

### Linux x86-64

- AppImage SHA-256:
  `82f6fcc3d91503ab5b2d3ab24014aeee5815575f37a79214a5cbfbc924e9a9c2`
- tarball SHA-256:
  `ad5c81e26169b92d1b8a8e60816be6a92e5501a083f69533e485e90cbb247144`

Verify with `sha256sum`, then run either the AppImage or the tarball's
`Golden-Balloon.AppDir/AppRun` from a writable directory.

### Browser

Use the published [browser build](https://akratch.github.io/golden-balloon/) in
a WebGPU-capable browser. Hard-refresh before testing so an older cached build
cannot masquerade as the candidate.

## 2. Ten-minute common gameplay pass

Run this in the default **Original — Recommended / Proven** frame-limit mode.

1. Open the launcher and confirm Diagnostics reports `Renderer: webgpu`.
   Windows may report backend `4` (Direct3D 12) or `6` (Vulkan); either is
   valid. `MDKR_RENDERER=gl` selects diagnostic OpenGL and does not force a
   WebGPU-native API.
2. Select the supported ROM and start the game. Watch the complete opening and
   menu transition. The Nintendo logo must not repeat across the screen; the
   sky, menus, and loading animation must not fracture, zoom off-screen, or
   drift away from center.
3. Listen from boot through character select. Audio should begin promptly and
   remain continuous. Character-select animations should begin normally rather
   than sitting static during a long audio/video delay.
4. Start a one-player race and drive for at least one minute. Check that all
   vehicle wheels and parts remain visible, the camera and HUD stay aligned,
   sky/terrain geometry remains intact, controls respond immediately, and
   audio has no sustained lag, breakup, or growing offset.
5. Pause and resume once, change one harmless setting, then finish or exit the
   race. Quit the app cleanly and relaunch it. Confirm the ROM selection,
   setting, and EEPROM progress persist.
6. Open the magic-code screen and enter `ARNOLD`. It should be accepted as
   **BIG CHARACTERS**; `ARNOLE` should be rejected. The enabled-code list must
   not contain a lone `8` or a blank entry.
7. If two controllers are available, start a two-player race. Both players
   must steer and accelerate independently; Player 2 must not stall while
   Player 1 behaves normally.

## 3. Experimental frame-limit smoke

This is crash/artifact coverage, not a visual-FPS acceptance claim.

1. Return to the launcher and confirm every non-Original frame limit is marked
   **Experimental — Under Construction**.
2. Select one numeric limit, such as `240`, launch, and reach a race.
3. Repeat with `Uncapped`.
4. In both cases, require no crash, missing wheels, fractured geometry,
   off-center UI, severe hitching, or input/audio regression.
5. Restore **Original** before the final relaunch. These modes alter host
   pacing/input opportunities in 1.0.1; they do not increase unique visual FPS.

## 4. Browser-only custody check

After the common pass, export a save backup, erase the browser's stored
progress, import the backup, and confirm the preview and restored progress are
correct. In developer tools, no network request may contain ROM bytes; the ROM
must remain client-side.

## 5. Report the result

Record:

- PASS or FAIL;
- platform and OS version;
- exact artifact filename and SHA-256;
- GPU and driver;
- renderer/backend line from `mdkr64.log` on Windows (normally under
  `%APPDATA%\mdkr64\mdkr64\`);
- ROM revision;
- Original-mode result;
- numeric/Uncapped result, or “not run”; and
- the first failed step plus a redacted text log.

File failures through the repository's
[bug-report template](https://github.com/akratch/goldenballoon/issues/new?template=bug_report.md).
Any crash, “damaged” macOS warning, delayed startup audio/menu animation,
geometry corruption, missing vehicle parts, off-center UI, invalid known magic
code, Player 2 input starvation, or save/relaunch failure is a release failure.
