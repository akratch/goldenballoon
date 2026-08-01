# Golden Balloon 1.0.1

Windows WebGPU presentation no longer blocks the cooperative game/audio thread
after each submitted frame. This restores the responsive startup audio and menu
animation behavior of 1.0.0. Production submits only newly authored images and
polls GPU completion without a synchronous runtime drain.

*Released 2026-08-01.*

Golden Balloon 1.0.1 is the first native patch release. It repairs the macOS
package, keeps experimental host-pacing choices separate from the proven
Original mode, and keeps WebGPU as the native default. No game data is included.

## What changed

- **The macOS app is self-contained again.** The old package accidentally
  bundled Homebrew's `sdl2-compat` shim, which dynamically loaded an unbundled
  SDL3 from the build machine. The replacement uses SHA-pinned standalone SDL2
  2.32.10, embeds its license, targets arm64/macOS 13, and rejects Homebrew,
  SDL3, unresolved load paths, mismatched architectures, and newer hidden
  deployment targets.
- **The “damaged and can't be opened” failure is fixed at the package
  boundary.** Nested code is sealed before the outer app after every bundle
  mutation, and the finished DMG is checksum-verified, mounted read-only, and
  revalidated in place. The exact app inside the mounted, read-only DMG must
  then launch through LaunchServices, select WebGPU, present and capture four
  real surface frames, and load no Homebrew or SDL3 libraries.
- **WebGPU is the native default.** Dense opening-sequence captures found
  localized sky/terrain texture corruption in the OpenGL path that sparse
  sampling had missed. WebGPU does not show that corruption and is also the
  project's extensively tested browser backend. OpenGL remains available with
  `MDKR_RENDERER=gl` as a diagnostic path, not the default release path.
- **Windows startup audio and menu timing are restored.** A post-1.0.0 queue
  bound called `wgpuDevicePoll(..., true)` after every second native WebGPU
  submission, synchronously draining the native WebGPU queue on the same
  cooperative thread that advances gameplay and refills audio. Production now
  polls completion without blocking and reserves the explicit queue bound for
  browser/internal replay stress. The final portable candidate passed a
  real-hardware Windows retest from launcher through intro, character select,
  gameplay, audio, and relaunch.
- **Numeric and Uncapped frame limits no longer crash the native app.** The
  adopted OpenGL context now initializes its function loader before the pacing
  code can use sync/fence entry points. Real launcher tests exercise both
  WebGPU and explicit OpenGL with the 240 and Uncapped policies. For 1.0.1,
  every non-Original choice is marked **Experimental — Under Construction** in
  the launcher and affects host pacing/input/event-pump opportunities, not
  unique visual FPS. Any benefit may be negligible, while higher settings can
  use more CPU.
- **Unsafe motion smoothing is disabled for this patch.** Release-candidate
  play-testing found that delayed display-list replay could run after the game
  had begun rewriting mutable viewport, matrix, vertex, texture, and display-
  list dependencies for the following task. The safe 1.0.1 policy submits only
  new authored images. The primary US 1.1 build remains at its authored roughly
  30 unique visual FPS, with no duplicate swaps. This removes the off-center
  UI, missing vehicle parts, fractured geometry, and related high-rate artifacts
  from the release path.
- **Gameplay compatibility was re-checked.** Automated Original-mode replay
  remains byte-exact with the pre-patch baseline. A direct two-player gate now
  proves exact P1/P2 controller-to-racer binding, button edges, stick input,
  and independent motion across GL/WebGPU at 60/120 Hz; the determinism and
  render-purity controls remain green.
  Numeric and Uncapped choices change host pacing only; they do not accelerate
  the simulation or increase the authored visual frame rate.
- **Magic codes load and validate correctly.** The decrypted big-endian table
  is normalized and fully bounds-checked before publication, fixing the
  reported `8`/blank enabled-code rows and universal “code was incorrect”
  result without allowing a partial table through.
- **The launcher and in-game overlay are cleaner and safer to use.** Navigation,
  dropped-ROM intake, boot configuration, overlay state, confirmation flows,
  and panel rendering now have small named owners with typed state instead of
  a monolithic draw path. Invalid ROM replacements and failed preference writes
  leave the last-known-good choice active; a failed setting retains the value
  you entered and can be saved through the visible **Retry** button after write
  access is restored, without restarting the app. Escape and controller B now
  follow the same popup → confirmation → Settings → overlay back stack.
- **The native launcher now adapts to its window and display.** Its supported
  minimum is 640×480, where the side rail becomes compact top navigation and
  wide controls/actions stack instead of clipping. A persisted 0.75×–2.00× UI
  scale changes text, controls, and spacing together; moving between standard
  and HiDPI displays rebuilds the font atlas before the next frame.
- **Native accessibility scope is stated explicitly.** The launcher supports
  keyboard and gamepad navigation, visible focus, scalable high-contrast UI,
  and restrained motion. This patch does not claim a native VoiceOver,
  UI-Automation, or other screen-reader semantic tree.
- **Return to Launcher shuts down cleanly before relaunching.** The engine,
  borrowed renderer objects, host, and diagnostic log reader unwind before the
  resolved executable replaces the process. A failed relaunch is visible and
  returns an error instead of hanging on an orphaned diagnostics pipe.
- **The Linux portable release is GPU-gated and self-contained.** Linux and
  Windows validation builds compile the requested version and assert the binary
  reports it before packaging. The public portable files are
  `Golden-Balloon-1.0.1-linux-x86_64.AppImage` and
  `Golden-Balloon-1.0.1-linux-x86_64.tar.gz`. Before upload, deterministic
  Xvfb + Mesa software-GPU lanes content-validate the real launcher through
  default WebGPU and explicit GL, then extract the tarball, launch its `AppRun`
  from an unrelated directory using bundled SDL2, and repeat both pixel gates.
  Those Linux files are published only if that complete workflow passes; a
  failed or unavailable lane produces no endorsed Linux binary.
  Windows builds, import-checks, packages, and launches from its extracted zip,
  but `windows-latest` cannot guarantee a qualifying GPU/API surface. The 1.0.1
  Windows zip therefore received a separate manual native WebGPU gameplay,
  controller, audio, save, and relaunch acceptance pass on Windows hardware;
  the automated startup checks are not presented as a rendered gameplay gate.
- **Release checks fail closed more often.** Merge commits and pushed ref tips
  receive public-surface scans; committed-then-deleted history is checked;
  privacy and browser-presentation tasks are pinned by the CI contract; and
  the macOS target, dependencies, runtime loads, version, commit, renderer, and
  bundled license are independently verified.

## FPS behavior

The launcher identifies Original as **Recommended / Proven** and marks Match
Display, numeric choices, and Uncapped **Experimental — Under Construction**.
In 1.0.1 these settings only alter host pacing and input/event-pump
opportunities. They do not increase unique visual FPS, swap duplicate images,
or speed up the game's authored simulation. The primary US 1.1 build remains at
its authored roughly 30 unique visual FPS. Any benefit may be negligible, while
higher settings can use more CPU.

Motion smoothing and delayed display-list replay are fully disabled for this
patch. The replay experiment could not prove immutable ownership of every
viewport, matrix, vertex, texture, and nested display-list dependency through a
delayed walk. The faster historical one-field simulation remains a separate,
explicit compatibility option; it is not enabled by Frame Limit. Use Original
for the recommended release behavior.

## macOS first launch

This patch intentionally skips Developer ID signing and Apple notarization.
The app is ad-hoc signed for code/resource integrity, but macOS may show the
ordinary unidentified-developer warning. After the first blocked launch, open
**System Settings → Privacy & Security**, scroll down, choose **Open Anyway**,
and confirm **Open**. Apple documents that current flow in
[Open apps safely on your Mac](https://support.apple.com/102445). A “damaged”
warning is not expected and should be reported as a bug.

The release artifact is Apple silicon only, requires macOS 13 or later, and is
named `Golden-Balloon-1.0.1-macos-arm64-unsigned.dmg`. Its checksum and
provenance sidecars identify the exact source commit and record
`macos_signing: ad-hoc-unsigned` so it cannot be mistaken for the optional
Developer ID/notarized flavor. If that trusted flavor is published later, its
exact name is `Golden-Balloon-1.0.1-macos-arm64-signed-notarized.dmg` and its
provenance records `macos_signing: developer-id-notarized`.

## Current limitations

The patch does not expand its claims beyond the tested WebGPU paths: the
complete campaign is not automated end to end, OpenGL remains diagnostic while
its opening-sequence visual-parity issue is open, macOS requires the manual
first-open approval above, and Linux has less physical-hardware coverage.
Windows build/package/startup checks are automated, while native GPU gameplay,
controller, audio, and save acceptance remains a manual gate that passed for
this candidate. On Windows, wgpu-native automatically selects a compatible API
and may use Vulkan or Direct3D 12; `MDKR_RENDERER` does not force either one.
Windows 1.0.1 also requires reasonably short, ASCII-only paths for the extracted
app and ROM. If the launcher cannot save an otherwise valid ROM choice, launch
`GoldenBalloon.exe --rom C:\ASCII\game.z64` from Command Prompt and report it.
Explicit WebGPU API selection, richer diagnostics, wide-character filesystem
support, and a reviewed application manifest are deferred to 1.0.2.
Non-Original frame-limit
choices remain Experimental — Under Construction and may add CPU cost without a
noticeable benefit; use Original for the proven cadence. See
[README — Current limitations](README.md#current-limitations) and
[ROADMAP.md](ROADMAP.md) for the exact deferred scope.

## Two related projects

- Native desktop port: [akratch/goldenballoon](https://github.com/akratch/goldenballoon)
- Browser build: [akratch/golden-balloon](https://github.com/akratch/golden-balloon)

Both still require a legally owned US 1.1 or European 1.1 ROM. The ROM is read
locally and is never part of a release artifact.

---

# Golden Balloon 1.0.0

*Released 2026-07-30.*

Golden Balloon is a **native source port of the 1997 Nintendo 64 kart racer
*Diddy Kong Racing***. It is not an emulator: the game is compiled for your CPU
and renders through WebGPU or OpenGL, natively on your desktop or as WebAssembly
in a browser.

This is the first public release.

> **macOS artifact erratum (2026-08-01):** the original 1.0.0 DMG is known bad
> and must not be used.
> Rewriting its SDL2 load path invalidated the linker's ad-hoc signature, so
> Gatekeeper correctly reported the app as damaged. Source and browser builds
> are unaffected. Golden Balloon 1.0.1 replaces that artifact with a standalone
> SDL2 package whose nested and outer ad-hoc seals, dependency closure, WebGPU
> startup, deployment target, and mounted DMG are verified before publication.

---

## Before you start: bring your own ROM

**No game data ships with this project.** No ROM, textures, audio, music, models
or level data — none in the release, none in the repository, and none in its git
history. Golden Balloon reads everything at runtime from a copy of the original
game that *you legally own and dumped yourself*.

Every shipped artifact passes a release-packaging guard that fails closed if it
contains N64 ROM data, so that promise is checked rather than merely stated.

In the browser build your ROM is read **locally, in your own browser**. It is
never uploaded, transmitted, or stored on any server.

### Which ROM

| Revision | Status |
|---|---|
| **US 1.1** (`us.v80`) | Supported |
| **European 1.1** (`pal.v80`) | Supported — races byte-identically to US 1.1 |
| US 1.0, European 1.0, Japan | **Refused by name.** Recognised from their header CRCs and rejected before any asset is touched |

`.z64`, `.v64` and `.n64` byte orders are all normalised on load, so it does not
matter which dump format you have.

The three unsupported revisions are refused deliberately rather than allowed to
half-work. The reasons, and what adding them would take, are in
[`docs/ROM_REVISIONS.md`](docs/ROM_REVISIONS.md) and
[`ROADMAP.md`](ROADMAP.md).

## Platforms

macOS (Apple silicon) and the WebGPU browser build are supported. Linux is
best-effort. **Windows is new in 1.0**: CI validates its build, import table,
exact package, and extracted startup; native GPU gameplay acceptance remains a
manual real-hardware release step with limited coverage breadth.
The per-platform
table with the exact caveats is in [`README.md`](README.md#platform-support), which is
the one place it is maintained.

Release binaries are not signed or notarized, so macOS and Windows will require
their manual override to launch one. See [`ROADMAP.md`](ROADMAP.md).

## What works

- **The whole frontend.** Every screen navigable, with pixel fidelity against
  the real ROM measured route by route.
- **Racing.** All 20 tracks in all three vehicles — every one of the 47
  (track, vehicle) combinations the game permits. Full three-lap Time Trials
  finish, and your time is saved and reloaded.
- **Adventure.** Adventure One closes the full hub → balloon → lobby → race →
  hub loop with exact win/loss persistence. Adventure Two unlocks from the
  canonical save block and drives all 20 mirrored racing lines with reflected
  world, camera, stereo, minimap and steering.
- **Boss races, challenges and trophies.** All ten boss levels load and drive;
  the legal first-boss route runs end to end through the four-balloon door to a
  physical finish, hub return and exact reload. Every authored egg, treasure and
  battle course, all three Taj vehicle challenges, and all four trophy
  championships run through production results, progression and EEPROM.
- **Local multiplayer.** Two, three and four-player split screen with
  independent human racers, per-player HUD, the three-player minimap quadrant,
  and multiplayer results.
- **Audio.** Music, sound effects and reverb, through the decompiled synthesiser
  driving a software mixer. RAW16 instruments are decoded from their serialised
  big-endian PCM rather than read as host-endian noise.
- **Saves you own.** Export, import, inspect, edit, recover or erase your
  progress — in the browser launcher before a ROM is even selected, and from the
  native CLI. Four checksummed virtual Controller Paks with ghost custody.
  Browser storage is not a backup; the downloaded backup is.
- **Mobile touch controls.** Phones and tablets get an analog steering stick
  plus simultaneous Go, Brake, Drift, Item, Look and Pause controls with
  safe-area placement, and an explicit show/hide choice that persists across
  reload.

## Presentation

Three modes, selectable at launch or from the in-game **Options → Video
Options** screen:

- **Pure** — the original game presented honestly: authentic 4:3 framing at the
  authored field of view, no enhancements. This is the reference the fidelity
  suite scores against.
- **Restored** *(default)* — the original art direction at modern fidelity. Widescreen,
  supersampling, output-resolution HUD geometry and text, anisotropic filtering,
  mipmaps. Changes sharpness and stability, never look.
- **Remastered** *(work in progress)* — adds runtime-derived SDF text, restrained
  level-derived directional lighting for racers and character objects, stable
  terrain-projected world shadows, and a bounded linear-light finish.

Universal Hor+ widescreen preserves authored zooms rather than stretching them,
and works correctly in split screen. Aspect ratio, field of view and 1×–4×
supersampling apply live; settings tied to already-created GPU resources are
saved with an explicit restart notice.

Gameplay defaults to the game's authored two-field simulation cadence in every
presentation mode, because an independent measurement showed the historical
one-field cadence finishing a boss lane 13.9% fast. The one-field cadence remains
available as an explicit, clearly-labelled compatibility choice.

## Known limitations

Stated here so they are not discovered one at a time. The full list, with the
reason for each and the condition under which it would change, is in
[`ROADMAP.md`](ROADMAP.md).

- **The campaign is not complete.** Silver-coin challenges, the world 1–4 boss
  rematches, both Wizpig races and the credits sequence are not covered by any
  automated route. This is the largest gap in the release.
- **Windows native gameplay acceptance is manual** and Linux is best-effort;
  see the table above.
- **Nothing is signed or notarized.**
- **One in-race fidelity route scores lower than the rest** (0.636 against
  0.855–0.998 on frontend routes) and has not been investigated. It is either a
  real in-race gap or an artefact of that route; nobody has measured which.
- **The zip-pad boost magnitude has never been checked against the ROM.** The
  mechanism is the game's own and no port change touches it, but that is not the
  same as having measured it.
- **Presentation above the authored tick rate is disabled for 1.0.1.** Motion
  smoothing and retained replay are not production paths; every Frame Limit
  policy submits only new authored images and never swaps duplicates.
- **Hosted continuous integration has never run green.** Every claim in this
  release rests on local runs of the regression suite.
- **Three ROM revisions are refused**, as described above.

## Where things are

| | |
|---|---|
| Build and run | [`README.md`](README.md) |
| Everything that changed | [`CHANGELOG.md`](CHANGELOG.md) |
| What is deferred, and why | [`ROADMAP.md`](ROADMAP.md) |
| Known and fixed defects, with evidence | [`docs/open-items/`](docs/open-items/README.md) |
| Contributing | [`CONTRIBUTING.md`](CONTRIBUTING.md) |
| Legal position | [`DISCLAIMER.md`](DISCLAIMER.md), [`NOTICE.md`](NOTICE.md), [`LICENSE`](LICENSE) |

## Credits

Built on the community
[Diddy Kong Racing decompilation](https://github.com/DavidSM64/Diddy-Kong-Racing)
by **DavidSM64** and its contributors — without their work, none of this exists.
The renderer architecture descends from the **sm64ex / fast3d** lineage. The
visual oracle is built on [**ares**](https://ares-emu.net/). And the original
engineering is **Rare's**, which this project exists to study and preserve.

Unaffiliated with and unendorsed by any rights holder. All trademarks belong to
their respective owners.
