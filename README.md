# Golden Balloon

A native and browser source port of the 1997 Nintendo 64 kart racer, built on
the community [decompilation](https://github.com/DavidSM64/Diddy-Kong-Racing).
Not an emulator: the game is compiled for your machine and runs from a ROM you
already own. No game assets are included.

**[▶ Play in your browser](https://akratch.github.io/golden-balloon/)** ·
**[⬇ Download the latest release](https://github.com/akratch/goldenballoon/releases/latest)**

## Quick start

1. **Get the game.** Download the build for your platform from the
   [latest release](https://github.com/akratch/goldenballoon/releases/latest),
   or use the [browser version](https://akratch.github.io/golden-balloon/)
   (needs a WebGPU browser — current Chrome or Edge).
2. **Provide your ROM.** You need a legally acquired dump of the original game
   — US 1.1 or European 1.1, as `.z64`, `.v64`, or `.n64`. macOS and Windows
   open a file picker; on Linux, drag the ROM onto the launcher or paste its
   path. In the browser the ROM is read locally and never uploaded. Not sure
   which release your copy is? Drop it on the
   [ROM checker](https://akratch.github.io/golden-balloon/rom-check.html) — it
   names the release and says whether this port runs it, in your browser, with
   nothing uploaded.
3. **Play.** A gamepad is recommended and fully remappable, with rumble.
   Keyboard: arrows/WASD steer, `X` accelerates, `Z` brakes, `Space`
   hops/power-slides, `Shift` uses items, `Enter` is Start. `F1` pauses and
   opens settings in-game. Have fun!

First-launch notes:

- **macOS**: the app is unsigned. If macOS warns about an unidentified
  developer, use **System Settings → Privacy & Security → Open Anyway**
  ([Apple's guide](https://support.apple.com/102445)).
- **Windows**: the app is not code-signed. If SmartScreen warns, choose
  **More info → Run anyway**.
- **Windows, non-English account names**: if your Windows username contains
  non-English characters and settings will not save, create an empty file named
  `portable.txt` next to `GoldenBalloon.exe`. The game then keeps your settings,
  saves, and add-ons in the game's own folder instead of your Windows user
  folder. If that folder is ever unwritable, the game falls back to saving next
  to itself automatically and tells you it did.

## Features

- **Widescreen** with the original FOV and art direction preserved, plus
  aspect, FOV, and supersampling controls. The classic 4:3 look is one
  setting away (`--pure`).
- **High framerates without touching the game.** Frame limits up to uncapped,
  a variable-refresh-friendly "Just Under Display" cap, a 40 Hz choice for
  handhelds, and optional motion smoothing — the simulation, physics, music,
  and lap times always run at their authored pace, and nothing tears.
- **Settings apply while you play.** One **Presentation pace** switch picks
  the original look or the smooth one; frame limit, smoothing, and tearing
  change on the next frame.
- **An optional camera that stays out of walls**, with a reduced-motion
  option. Off by default — the authored camera is the default everywhere.
- **Local multiplayer** for 2–4 players, per-controller binding, split-screen.
- **Playable Taj**, an unlockable extra racer with his own tuning.
- **PAL done right**: the European ROM keeps its exact timing and pitch, with
  smooth presentation available on 60 Hz displays. German is available in the
  language menu on every ROM revision.
- **Saves that behave**: EEPROM progress, ghosts, and settings persist —
  including in the browser, with export/import in the launcher.

## Platforms

| Platform | Status |
|---|---|
| Windows (x64) | Portable zip, no install. Tested on real hardware each release |
| macOS (Apple silicon) | DMG. WebGPU with the Restored presentation is the qualified visual path; OpenGL is diagnostic-only |
| Linux (x86-64) | AppImage and tarball, SDL2 bundled. Best effort — GPU/desktop breadth varies |
| Browser | Current Chrome/Edge with WebGPU. Runs the same engine compiled to wasm |

Every release file ships with a `.provenance.json` naming the exact source
commit and checksum it was built from.

## Known limitations

- The complete start-to-credits campaign is not automated or claimed complete.
- Linux does not yet have a native **Choose ROM File** dialog. Drag and drop
  or paste the path instead.
- The Remastered presentation (lighting, shadows, SDF text) is opt-in and
  still in progress.
- The desktop app supports keyboard/gamepad navigation, UI scaling and reduced
  motion, and an Accessibility section gathers them. It does not yet expose a
  contrast setting — earlier notes claimed one; there was never a control for
  it. It also does not present itself to a screen reader, so it is not
  advertised as screen-reader compatible.
- Other ROM revisions (JP, US 1.0, EU 1.0) are recognized and refused by
  name; only US 1.1 and EU 1.1 are supported. The
  [ROM checker](https://akratch.github.io/golden-balloon/rom-check.html) tells
  you which one you have and what is still missing for it.

## Custom content

You can replace the game's textures and music with your own. Put a folder or a
`.zip` in `mods/` — next to your save data on macOS and Windows, next to the
game on a portable build — with a small `pack.ini` naming the pack, plus a
`textures/` or `music/` folder inside it. The game picks it up at launch, and
**Tab** switches replaced textures off and back on while you play, so you can
see both versions of the same corner. Settings → Content lists what loaded, and
names anything it skipped and why.

Nothing is included and nothing is hosted here: a pack is files you or someone
else made, on your own machine. Replacement models and characters are not
supported.

[docs/MODDING.md](docs/MODDING.md) has the details, including
`tools/mod_texture_dump.py`, which writes out every texture the game draws
under the exact filename a pack needs.

## No game data is included

No ROM, textures, audio, music, models, or level data are distributed here or
in any release. Golden Balloon reads everything at runtime from your copy of
the game, and every release artifact is scanned to guarantee it. We do not
condone piracy. See [DISCLAIMER.md](DISCLAIMER.md) and [NOTICE.md](NOTICE.md).

## Building from source

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/mdkr64 --rom /path/to/your.z64
```

Prerequisites: CMake 3.16+, a C11 toolchain, pkg-config, SDL2, python3, and
network access at configure time (the WebGPU runtime is fetched and
hash-verified). On macOS: `brew install cmake pkg-config sdl2`. Running
`./build/mdkr64` with no arguments opens the launcher.

Browser build (needs [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
**4.0.10**, the version CI pins — `./emsdk install 4.0.10 && ./emsdk activate 4.0.10`):

```bash
tools/web/build_web.sh
python3 -m http.server -d dist/web 8000
```

Useful flags: `--pure|--restored|--remastered`, `--video-set Key=Value`,
`--video-list`, `--aspect`, `--fov`, `--window-size`; `MDKR_RENDERER=webgpu|gl`
selects the backend.

## For developers

The game logic is the vendored decompilation; everything that makes it run on
a modern machine (renderer, audio engine, input, pacing, saves, web build) is
first-party code under [`platform/`](platform/). Start here:

- [CONTRIBUTING.md](CONTRIBUTING.md) — build, rules, and what a fix ships with
- [docs/DEVELOPER_HANDBOOK.md](docs/DEVELOPER_HANDBOOK.md) — architecture and
  the 64-bit/endianness bug shapes behind most defects here
- [docs/README.md](docs/README.md) — full documentation index
- [tests/README.md](tests/README.md) — the regression suite (140+ gates)
- [ROADMAP.md](ROADMAP.md) — deferred work and what would close it

The browser build publishes to
[`akratch/golden-balloon`](https://github.com/akratch/golden-balloon) (note
the hyphen); code changes belong in this repository.

## Credits

- [DavidSM64](https://github.com/DavidSM64/Diddy-Kong-Racing) and the decomp
  contributors, without whom none of this exists
- The sm64ex / fast3d lineage, for the renderer architecture this builds on
- [ares](https://ares-emu.net/), the reference emulator behind the visual
  comparisons
- [DKR-R](https://github.com/ThatGuyMcd/DKR-R) by
  [ThatGuyMcd](https://github.com/ThatGuyMcd), whose presentation-identity
  work informed several of Golden Balloon's interpolation safety rules
- The original developers at Rare, whose work this project exists to study
  and preserve

## Legal

[LICENSE](LICENSE) (MIT, first-party code only) ·
[NOTICE.md](NOTICE.md) (per-component provenance) ·
[DISCLAIMER.md](DISCLAIMER.md) (full position)

Unaffiliated with and unendorsed by any rights holder. All trademarks belong
to their respective owners. If you are a rights holder with a concern, open an
issue and it will be addressed promptly.
