# Golden Balloon 1.0.0

*Released 2026-07-30.*

Golden Balloon is a **native source port of the 1997 Nintendo 64 kart racer
*Diddy Kong Racing***. It is not an emulator: the game is compiled for your CPU
and renders through WebGPU or OpenGL, natively on your desktop or as WebAssembly
in a browser.

This is the first public release.

> **macOS artifact erratum (2026-08-01):** the original 1.0.0 DMG is withdrawn.
> Rewriting its SDL2 load path invalidated the linker's ad-hoc signature, so
> Gatekeeper correctly reported the app as damaged. Source and browser builds
> are unaffected. The replacement macOS release is required to pass the new
> Developer ID signing, notarization, stapling, mounted-DMG, and Gatekeeper
> pipeline before publication.

---

## Before you start: bring your own ROM

**No game data ships with this project.** No ROM, textures, audio, music, models
or level data — none in the release, none in the repository, and none in its git
history. Golden Balloon reads everything at runtime from a copy of the original
game that *you legally own and dumped yourself*.

Every build runs a guard that fails closed if any shipped file contains N64 ROM
data, so that promise is checked rather than merely stated.

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
best-effort. **Windows is new in 1.0**: it builds, runs, and plays (validated
by hand on real hardware), but has no CI cell yet and limited coverage breadth.
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
- **Windows is new in 1.0** and Linux is best-effort; see the table above.
- **Nothing is signed or notarized.**
- **One in-race fidelity route scores lower than the rest** (0.636 against
  0.855–0.998 on frontend routes) and has not been investigated. It is either a
  real in-race gap or an artefact of that route; nobody has measured which.
- **The zip-pad boost magnitude has never been checked against the ROM.** The
  mechanism is the game's own and no port change touches it, but that is not the
  same as having measured it.
- **Presentation above the authored tick rate is off by default.** Camera-only
  interpolation exists but is proven headless only; a 60 Hz default was evaluated
  and explicitly refused, for reasons recorded in the changelog.
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
