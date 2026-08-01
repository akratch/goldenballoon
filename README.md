# Golden Balloon

A native and browser source port of the 1997 Nintendo 64 kart racer. Not an
emulator: the game is compiled for your machine and renders through WebGPU or
OpenGL, using a ROM you already own.

> **Two repositories, one project — the hyphen matters:**
>
> - **Native/desktop app, source code, releases, and development:**
>   [`akratch/goldenballoon`](https://github.com/akratch/goldenballoon) — you are here.
> - **Playable web build and its generated publication files:**
>   [`akratch/golden-balloon`](https://github.com/akratch/golden-balloon) —
>   [launch the browser version](https://akratch.github.io/golden-balloon/).
>
> The web repository is generated from this source repository; code changes belong
> here, not in the publication repository.

## Download and play

**[Play in your browser](https://akratch.github.io/golden-balloon/)** (WebGPU
browser required: Chrome/Edge 113+, or WebGPU-enabled Firefox/Safari), or grab
a desktop build from the
**[latest release](https://github.com/akratch/goldenballoon/releases/latest)**:

| Platform | File | Notes |
|---|---|---|
| macOS (Apple silicon) | `golden-balloon-1.0.0.dmg` | The original 1.0.0 DMG is withdrawn: its post-package signature is invalid. Use the next Developer ID signed and notarized rebuild |
| Windows (x64) | `golden-balloon-windows-1.0.0.zip` | Single self-contained `GoldenBalloon.exe`, no installer |
| Linux (x86-64) | `golden-balloon-linux-1.0.0-x86_64.tar.gz` | Also available for arm64. Needs SDL2 and Mesa GL |

Then:

1. Launch the app. It opens with a ROM picker.
2. Point it at your own legally-dumped ROM of the original game (`.z64`,
   `.v64`, and `.n64` all work; US 1.1 and European 1.1 are supported).
3. Play. A gamepad is recommended. On keyboard: arrows or WASD steer, `X`
   accelerates, `Z` brakes, `Space` hops and power-slides, `Shift` fires
   items, `Enter` is Start. `F1` opens the settings overlay in-game.

Each release file ships with a `.provenance.json` naming the exact source
commit and SHA-256 it was built from.

> **macOS 1.0.0 packaging notice:** the unsigned DMG can produce Finder's
> “damaged and can't be opened” dialog because its executable was modified
> after the linker's integrity signature. This is not the ordinary
> unidentified-developer warning. The packaging path is fixed in `Unreleased`;
> the replacement must pass Developer ID signing, Apple notarization, stapling,
> mounted-DMG verification, and Gatekeeper assessment before publication.

### No game data is included

No ROM, textures, audio, music, models, or level data are distributed here.
None in the repository, none in its git history, none in the releases. Golden
Balloon reads everything at runtime from your copy of the game. In the browser
the ROM is read locally and never uploaded, transmitted, or stored on any
server. Every build runs [`tools/check_no_rom.sh`](tools/check_no_rom.sh),
which fails closed if any shipped file contains N64 ROM data, so this promise
is checked rather than merely stated. See [DISCLAIMER.md](DISCLAIMER.md) and
[NOTICE.md](NOTICE.md).

## What this is

An unofficial, fan-made source port built for research, preservation, and
learning. The game logic comes from the community
[Diddy Kong Racing decompilation](https://github.com/DavidSM64/Diddy-Kong-Racing).
Everything that makes it run on a modern machine (the graphics HLE, the audio
engine, input, frame pacing, saves, the web build) is original work under
[`platform/`](platform/).

The name comes from the game's own lore: the golden balloons that gate every
Adventure race. It is a project codename, not a product name of any rights
holder.

### Platform support

| Platform | Status |
|---|---|
| macOS (Apple silicon) | Supported. OpenGL by default, WebGPU available |
| Web (WebGPU browser) | Supported. WebAssembly + WebGPU + AudioWorklet |
| Linux | Best effort. Builds and runs on GL and WebGPU; physical-GPU, Wayland, and controller breadth are unmeasured |
| Windows | New in 1.0. Runs and plays; no CI cell yet, limited hardware coverage. Treat as early and report what you find |

## Presentation modes

| Mode | What it is |
|---|---|
| `--pure` | The original game presented honestly: authentic 4:3 framing at the authored FOV, no enhancements. This is the reference the fidelity suite scores against |
| `--restored` | **The default.** The original art direction at modern fidelity: widescreen, supersampling, output-resolution HUD text, anisotropic filtering, mipmaps. Changes sharpness and stability, never look |
| `--remastered` | Work in progress. Adds runtime SDF text and restrained level-derived directional lighting on top of Restored. Tonemapping, broader materials, and color grading are future work |

The in-game Options → Video Options screen exposes all of this, plus aspect
ratio, FOV, and supersampling, applied live where the renderer allows and saved
with a restart notice where it does not. Settings persist in `mdkr64.ini`.

Gameplay runs at DKR's authored simulation cadence in every mode. The
historical port behavior (a faster, gameplay-changing 60 Hz simulation) remains
available explicitly: `--video-set Gameplay.SimulationCadence=enhanced`. The
authored cadence is the default because an independent-emulator measurement
showed the faster cadence finishing a boss lane 13.9% early.

## Status

Every claim in this table names the check that demonstrates it. The full suite
is 82 check scripts expanding to 90 tasks; the manifest fails if a new check is
not registered, and each check is validated in both directions. See
[tests/README.md](tests/README.md).

| Area | State | Demonstrated by |
|---|---|---|
| Boot and menus | Every screen navigable. Pixel fidelity vs the real ROM: 85.5–99.8% on frontend screens, 63.6% in-race | `check_nav_fixtures.py`; [the oracle](docs/ORACLE.md) (manual run) |
| Racing | All 20 tracks, all three vehicles, all 47 legal combinations. Full 3-lap Time Trial with saved and reloaded times | `check_track_sweep.py`, `check_vehicle_sweep.py`, `check_race_finish_time.py` |
| Renderers | OpenGL is the native throughput default; WebGPU remains selectable and is the browser backend. Uncapped GPU production is bounded and minimized windows stop GPU walks | `check_renderer_backends.py`, `check_gpu_backpressure.py`, and `check_surface_suspension.py` |
| Widescreen, UI, shadows | Hor+ world rendering, authored-FOV preservation, output-resolution HUD in the 4:3 safe region, live resize/HiDPI, split-screen projection, terrain-projected shadows | `check_native_ui_resolution.py`, `check_widescreen_proportions.py`, shadow and 2–4P gates on both backends |
| Memory-layout safety | Host-aligned object tails, bounds-checked object-map records | `check_native_layout.py` under halt-on-error alignment UBSan, with broken-direction controls |
| Local multiplayer | 2–4 player layouts, per-player HUD, multiplayer results | `check_race_2p_split.py`, `check_race_multiplayer.py` |
| Audio | Music, SFX, and reverb through the clean-room audio engine | `check_audio_output.py`, `check_raw16_audio.py` |
| Browser | Real Chromium boots the wasm build, races, runs the AudioWorklet, restores ROM and saves across reload, never sends the ROM over the network; display/numeric rAF schedules preserve fixed state/event/input/PCM | `check_browser_runtime.py` (3,600-frame live run) + `check_browser_presentation_rates.py` |
| Mobile touch | Analog stick plus chorded Go/Brake/Drift/Item controls with safe-area placement | `check_touch_controls.py`, including a CDP three-finger chord traced to the game's own input read |
| Adventure mode | Both adventures end to end: hub, balloons, lobby, races, mirrored Adventure Two, win/loss persistence | `check_adventure_hub.py`, `check_adventure_race_loop.py`, `check_adventure_two.py` |
| Bosses, challenges, trophies | All ten boss levels; the full first-boss progression with persistence; every authored challenge course; all four trophy championships | `check_first_boss_progression.py`, `check_challenge_modes.py`, `check_taj_challenges.py`, `check_trophy_series.py` |
| ROM support | US 1.1 and European 1.1 (byte-identical racing). Other revisions are identified by CRC and refused by name | `check_rom_revision.py`, [docs/ROM_REVISIONS.md](docs/ROM_REVISIONS.md) |

Hosted CI lanes exist (Linux GL/WebGPU, macOS, sanitizers, wasm, clean-room
guards) but have not yet run green; the first hosted run is blocked on account
billing, so current claims rest on local runs. See [ROADMAP.md](ROADMAP.md).

## Building from source

Native:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/mdkr64 --rom /path/to/your.z64
```

Prerequisites: CMake 3.16+, a C11 toolchain, SDL2, python3, and network access
at configure time (the WebGPU runtime is fetched and hash-verified). Launching
`./build/mdkr64` with no arguments opens the same launcher the releases use.

Browser (needs [emsdk](https://emscripten.org/docs/getting_started/downloads.html)):

```bash
tools/web/build_web.sh
python3 -m http.server -d dist/web 8000
```

Open <http://localhost:8000> and pick your ROM. The launcher's Stored data
panel can export, import, edit, and recover progress; browser storage is not a
backup, see [save management](docs/SAVE_MANAGEMENT.md).

### Useful flags

| Flag | Purpose |
|---|---|
| `--pure` / `--restored` / `--remastered` | Presentation mode (default: restored) |
| `--video-set Key=Value` | Override one video setting |
| `--video-list` | Print resolved settings with their source, then exit |
| `--headless-frames N` | Run N frames and exit; never opens the audio device |
| `--dump-frames DIR` | Write one binary-PPM (P6) file per presented frame |
| `--input-script FILE` | Replay scripted input, optionally per controller port |
| `--window-size WIDTHxHEIGHT` | Initial drawable size |
| `--aspect auto\|4:3\|16:9\|16:10\|21:9\|RATIO` | Presentation aspect |
| `--fov authored\|20..140` | Scale gameplay FOV while preserving authored zooms |
| `--legacy-stretch` | Reproduce the former stretched 4:3 presentation exactly |

`MDKR_RENDERER=webgpu|gl` selects the backend. Settings resolve in one
documented order (schema defaults, then `mdkr64.ini`, preset, in-game menu,
environment, CLI), and `--pure` never rewrites `mdkr64.ini`, so comparing
against the reference cannot disturb a saved setup.

## For developers

Start with [docs/DEVELOPER_HANDBOOK.md](docs/DEVELOPER_HANDBOOK.md). Beyond
the architecture, it documents the recurring 64-bit and endianness bug shapes
that account for nearly every hard defect in this port; read §3 before
debugging anything.

- [CONTRIBUTING.md](CONTRIBUTING.md): build, the audio-safety rule, the test
  suite, and what a fix has to ship with
- [docs/README.md](docs/README.md): documentation index
- [ROADMAP.md](ROADMAP.md): deferred work and the conditions for taking it up
- [docs/ARCHITECTURE_DECISIONS.md](docs/ARCHITECTURE_DECISIONS.md): settled
  decisions and milestone history
- [docs/architecture/](docs/architecture/README.md): per-subsystem notes
- [docs/open-items/](docs/open-items/README.md): every known and fixed defect,
  by subsystem, with the measurement that found it
- [docs/ORACLE.md](docs/ORACLE.md): pixel comparison against the real ROM,
  including four ways that harness has lied
- [docs/SAVE_MANAGEMENT.md](docs/SAVE_MANAGEMENT.md): save workflows and gates
- [docs/DECOMP_SYNC.md](docs/DECOMP_SYNC.md): merging with the upstream decomp
- [docs/MGB64_BACKFLOW.md](docs/MGB64_BACKFLOW.md): findings shared with the
  sibling GoldenEye port
- [docs/RELEASE_CHECKLIST.md](docs/RELEASE_CHECKLIST.md): pre-release gates

## Credits

- [DavidSM64](https://github.com/DavidSM64/Diddy-Kong-Racing) and the decomp
  contributors, without whose work none of this exists
- The sm64ex / fast3d lineage, for the renderer architecture this builds on
- [ares](https://ares-emu.net/), the reference emulator behind the visual oracle
- The original developers at Rare, whose engineering this project exists to
  study and preserve

## Legal

[LICENSE](LICENSE) (MIT, first-party code only) ·
[NOTICE.md](NOTICE.md) (per-component provenance) ·
[DISCLAIMER.md](DISCLAIMER.md) (full position)

Unaffiliated with and unendorsed by any rights holder. All trademarks belong
to their respective owners. If you are a rights holder with a concern, open an
issue and it will be addressed promptly.
