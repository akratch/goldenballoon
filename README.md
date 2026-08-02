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
| macOS (Apple silicon) | `Golden-Balloon-1.0.3-macos-arm64-unsigned.dmg` | Apple silicon hotfix build. Intentionally unsigned; see the first-open note below |
| Linux (x86-64) | `Golden-Balloon-1.0.3-linux-x86_64.tar.gz` | Published only if the release workflow's built + extracted WebGPU/GL pixel gates pass; the AppImage accompanies it. SDL2 is bundled; the host graphics driver is still required |
| Windows (x64) | `Golden-Balloon-1.0.3-windows-x64.zip` | Portable rendering-correctness hotfix. The 1.0.1 WebGPU base passed manual gameplay, controller, audio, save, and relaunch acceptance on Windows hardware; 1.0.3 hardens glyph/texture caches and Remastered composition |

Hosted Windows CI proves the native binary, import table, exact package, and
extracted startup, but does not provide a stable GPU environment for rendered
gameplay. The 1.0.1 base therefore received a separate real-hardware pass
covering its default WebGPU launch, intro and character-select timing, gameplay,
controller, audio, save, and relaunch. That manual evidence boundary remains
visible in the release notes.

If the named Linux files are absent from the release, its software-GPU publish
gate did not pass and there is no qualified 1.0.3 Linux binary; build from
source instead of redistributing an unverified workflow artifact.

Then:

1. Launch the app. It opens with a ROM picker.
2. Point it at your own legally-dumped ROM of the original game (`.z64`,
   `.v64`, and `.n64` all work; US 1.1 and European 1.1 are supported).
3. Play. A gamepad is recommended. On keyboard: arrows or WASD steer, `X`
   accelerates, `Z` brakes, `Space` hops and power-slides, `Shift` fires
   items, `Enter` is Start. `F1` opens the settings overlay in-game.

Each release file ships with a `.provenance.json` naming the exact source
commit and SHA-256 it was built from.

For the current 1.0.3 candidate pass, use the human routes in the
**[release checklist](docs/RELEASE_CHECKLIST.md#7-post-release-spot-checks)**
and verify every downloaded sidecar before opening an artifact. The
**[historical 1.0.1 exact-hash guide](docs/RELEASE_CANDIDATE_TEST_GUIDE.md)**
is retained as acceptance evidence for that release; it is not a 1.0.3 file
list. The guide is refreshed with the exact 1.0.3 artifact hashes only after
those draft assets exist, without moving the release tag.

> **macOS packaging notice:** the known-bad 1.0.0 DMG can produce Finder's
> “damaged and can't be opened” dialog because its executable was modified
> after the linker's integrity signature. This is not the ordinary
> unidentified-developer warning. The 1.0.3 patch is intentionally shipped
> without Developer ID signing/notarization, so first launch may show the normal
> unidentified-developer warning. After the first blocked launch, open **System
> Settings → Privacy & Security**, scroll down, choose **Open Anyway**, and
> confirm **Open**. Apple documents that current flow in
> [Open apps safely on your Mac](https://support.apple.com/102445).
> Its nested code is still sealed inside-out and the mounted DMG is checked for
> signature integrity, self-contained dependencies, WebGPU selection, and the
> macOS 13 deployment target. “Damaged” is never an expected result. A future
> Developer ID/notarized build, if published, has the distinct name
> `Golden-Balloon-1.0.3-macos-arm64-signed-notarized.dmg` and distinct
> `developer-id-notarized` provenance; it is not the artifact for this patch.

### No game data is included

No ROM, textures, audio, music, models, or level data are distributed here.
None in the repository, none in its git history, none in the releases. Golden
Balloon reads everything at runtime from your copy of the game. In the browser
the ROM is read locally and never uploaded, transmitted, or stored on any
server. Every release packaging path runs a structural asset/ROM guard:
[`tools/check_no_rom.sh`](tools/check_no_rom.sh) checks staged portable and web
artifacts, while [`macos/Scripts/verify_asset_free.sh`](macos/Scripts/verify_asset_free.sh)
checks the exact macOS bundle. This promise is checked rather than merely
stated. See [DISCLAIMER.md](DISCLAIMER.md) and [NOTICE.md](NOTICE.md).

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
| macOS (Apple silicon) | Supported. WebGPU by default; OpenGL is an explicit diagnostic backend pending visual-parity work |
| Web (WebGPU browser) | Supported. WebAssembly + WebGPU + AudioWorklet |
| Linux | Best effort. Builds and runs on GL and WebGPU; physical-GPU, Wayland, and controller breadth are unmeasured |
| Windows | Supported portable x64 patch build. Native CI validates build/package/startup; the 1.0.1 WebGPU base also passed manual gameplay, controller, audio, save, and relaunch acceptance on Windows hardware |

## Presentation modes

| Mode | What it is |
|---|---|
| `--pure` | The original game presented honestly: authentic 4:3 framing at the authored FOV, no enhancements. This is the reference the fidelity suite scores against |
| `--restored` | **The default.** The original art direction at modern fidelity: widescreen, supersampling, output-resolution HUD text, anisotropic filtering, mipmaps. Changes sharpness and stability, never look |
| `--remastered` | **Opt-in and work in progress.** Adds runtime SDF text, restrained level-derived directional lighting for racers and character objects, terrain-projected world shadows, and a bounded linear-light finish with per-world grading on top of Restored. Broader material relighting remains future work |

The in-game Options → Video Options screen exposes all of this, plus aspect
ratio, FOV, and supersampling, applied live where the renderer allows and saved
with a restart notice where it does not. Settings persist in `mdkr64.ini`.

`Video.FrameLimit` defaults to **Original (Recommended / Proven)**. Match
Display, the listed numeric choices, and Uncapped are all
**Experimental — Under Construction**. In 1.0.1+ they only alter host pacing and
input/event-pump opportunities: they do not increase unique visual FPS,
manufacture intermediate images, or change gameplay timing. The primary US 1.1
build remains at its authored roughly 30 unique visual FPS. Any practical
benefit may be negligible, while higher settings can use more CPU. Use Original
for the proven release behavior.

Motion smoothing and delayed display-list replay are disabled in 1.0.1+. A
delayed replay can run after the game has begun rewriting mutable viewport,
matrix, vertex, texture, and display-list storage for the next task; that is not
a safe basis for a release image. Keeping authored frames avoids the off-center
UI, missing vehicle parts, fractured geometry, and related artifacts found
during release-candidate play-testing.

Gameplay runs at DKR's authored simulation cadence in every mode. The
historical port behavior (a faster, gameplay-changing 60 Hz simulation) remains
available explicitly: `--video-set Gameplay.SimulationCadence=enhanced`. The
authored cadence is the default because an independent-emulator measurement
showed the faster cadence finishing a boss lane 13.9% early.

## Native launcher

The desktop launcher supports keyboard and gamepad navigation, visible focus,
and a persisted 0.75×–2.00× UI scale. It remains usable at a 640×480 logical
window by switching to compact top navigation and stacking controls that no
longer fit side by side; moving between standard and HiDPI displays rebuilds
its fonts before the next frame.

ROM and settings changes are transactional. An invalid ROM candidate never
replaces the last playable selection, and a setting that cannot be written
keeps the attempted value visible. Restore write access and press **Retry** to
save it in the same app session. The native accessibility scope does not yet
include a VoiceOver, UI-Automation, or other screen-reader semantic tree.

## Status

Every claim in this table names the check that demonstrates it. The full suite
is 91 check scripts expanding to 100 tasks; the manifest fails if a new check is
not registered, and each check is validated in both directions. See
[tests/README.md](tests/README.md).

| Area | State | Demonstrated by |
|---|---|---|
| Boot and menus | Every screen navigable. Pixel fidelity vs the real ROM: 85.5–99.8% on frontend screens, 63.6% in-race | `check_nav_fixtures.py`; [the oracle](docs/ORACLE.md) (manual run) |
| Racing | All 20 tracks, all three vehicles, all 47 legal combinations. Full 3-lap Time Trial with saved and reloaded times | `check_track_sweep.py`, `check_vehicle_sweep.py`, `check_race_finish_time.py` |
| Renderers | WebGPU is the native default and the browser backend; OpenGL remains an explicit diagnostic backend while its parity work continues. Production submits only authored images, WebGPU completion polling does not block gameplay/audio, and minimized windows stop GPU walks | `check_renderer_backends.py`, `check_gpu_backpressure.py`, and `check_surface_suspension.py` |
| Widescreen, UI, shadows | Hor+ world rendering, authored-FOV preservation, output-resolution HUD in the 4:3 safe region, live resize/HiDPI, split-screen projection, terrain-projected shadows | `check_native_ui_resolution.py`, `check_widescreen_proportions.py`, shadow and 2–4P gates on both backends |
| Memory-layout safety | Host-aligned object tails, bounds-checked object-map records | `check_native_layout.py` under halt-on-error alignment UBSan, with broken-direction controls |
| Local multiplayer | 2–4 player layouts, direct per-controller racer binding, per-player HUD, multiplayer results | `check_2p_human_binding.py`, `check_race_2p_split.py`, `check_race_multiplayer.py` |
| Audio | Music, SFX, and reverb through the clean-room audio engine | `check_audio_output.py`, `check_raw16_audio.py` |
| Browser | Real Chromium boots the wasm build, races, runs the AudioWorklet, restores ROM and saves across reload, and never sends the ROM over the network; display/numeric host schedules preserve the authored visual cadence and fixed state/event/input/PCM | `check_browser_runtime.py` (3,600-frame live run) + `check_browser_presentation_rates.py` |
| Mobile touch | Analog stick plus chorded Go/Brake/Drift/Item controls with safe-area placement | `check_touch_controls.py`, including a CDP three-finger chord traced to the game's own input read |
| Adventure mode | Both Adventure-mode loop variants: hub, balloons, lobby, races, mirrored Adventure Two, and win/loss persistence. This is not a claim that either full campaign is automated end to end; see [Current limitations](#current-limitations) | `check_adventure_hub.py`, `check_adventure_race_loop.py`, `check_adventure_two.py` |
| Bosses, challenges, trophies | All ten boss levels; the full first-boss progression with persistence; every authored challenge course; all four trophy championships | `check_first_boss_progression.py`, `check_challenge_modes.py`, `check_taj_challenges.py`, `check_trophy_series.py` |
| ROM support | US 1.1 and European 1.1 (byte-identical racing). Other revisions are identified by CRC and refused by name | `check_rom_revision.py`, [docs/ROM_REVISIONS.md](docs/ROM_REVISIONS.md) |

Hosted CI covers Linux GL/WebGPU, macOS, sanitizers, wasm, and clean-room
guards. Platform claims above still distinguish automated coverage from manual
physical-hardware acceptance. See [ROADMAP.md](ROADMAP.md).

## Current limitations

- The complete start-to-credits campaign is not automated or claimed complete.
  Silver-coin progression, later boss rematches, both Wizpig races, and the
  credits path remain outside the current gate breadth.
- Native macOS and the browser use the default WebGPU paths. OpenGL is retained
  only for diagnostics while its known opening-sequence visual-parity issue is
  investigated.
- The macOS 1.0.3 artifact has an ad-hoc integrity seal but no Developer ID
  trust signature or Apple notarization, so current macOS requires the manual
  first-open approval above.
- Linux is best effort and lacks the macOS/WebGPU path's physical-GPU,
  display-server, controller, and OS-version breadth. Windows build/package
  validation is automated; native GPU gameplay, controller, audio, and save
  acceptance remains a manual real-hardware release gate; the 1.0.1 base passed.
- On Windows, wgpu-native automatically selects a compatible API and may use
  Vulkan or Direct3D 12. `MDKR_RENDERER=webgpu|gl` selects the project renderer;
  it does not force either native API. Explicit selection and richer adapter
  diagnostics are deferred to a future portability release.
- Windows 1.0.3 does not yet handle every non-ASCII or very long filesystem
  path. Keep the extracted app and ROM in reasonably short, ASCII-only paths.
  If the launcher cannot save an otherwise valid ROM choice, run
  `GoldenBalloon.exe --rom C:\ASCII\game.z64` from Command Prompt and report the
  issue. Wide-character filesystem support and an application manifest are
  deferred to a future portability release.

The full deferred scope and the evidence required to close each item are in
[ROADMAP.md](ROADMAP.md).

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
