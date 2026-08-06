# Golden Balloon

A native and browser source port of the 1997 Nintendo 64 kart racer. Not an
emulator: the game is compiled for your machine, using a ROM you already own.
WebGPU with the Restored presentation is the qualified visual path; OpenGL and
Remastered remain available for diagnostics and experimentation.

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
required; current Chrome or Edge is the qualified browser path, while other
WebGPU implementations are capability-detected but not yet in this project's
runtime matrix), or grab a desktop build from the
**[latest release](https://github.com/akratch/goldenballoon/releases/latest)**:

| Platform | File | Notes |
|---|---|---|
| macOS (Apple silicon) | `Golden-Balloon-1.0.5-macos-arm64-unsigned.dmg` | Candidate filename; publication remains blocked until the full human acceptance pass. Intentionally unsigned; see the first-open note below |
| Linux (x86-64) | `Golden-Balloon-1.0.5-linux-x86_64.tar.gz` | Published only if the release workflow's built + extracted WebGPU/GL pixel gates pass; the AppImage accompanies it. SDL2 is bundled; the host graphics driver is still required |
| Windows (x64) | `Golden-Balloon-1.0.5-windows-x64.zip` | Candidate filename; hosted CI never publishes it, so the exact extracted archive requires current Windows hardware acceptance for WebGPU, controller, audio, saves, Unicode/long paths, and relaunch before a maintainer may attach it |

Hosted Windows CI validates the native binary, import table, package, and
extracted startup, but does not provide a stable GPU environment for rendered
gameplay. The 1.0.1 base therefore received a separate real-hardware pass
covering its default WebGPU launch, intro and character-select timing, gameplay,
controller, audio, save, and relaunch. That manual evidence boundary remains
visible in the release notes.

If the named Linux files are absent from the release, its software-GPU publish
gate did not pass and there is no qualified 1.0.5 Linux binary; build from
source instead of redistributing an unverified workflow artifact.

Then:

1. Launch the app. macOS and Windows offer a native ROM picker. On Linux,
   drag the ROM onto the launcher or paste its absolute path; the app does not
   search your disk.
2. Point it at your own legally-dumped ROM of the original game (`.z64`,
   `.v64`, and `.n64` all work; US 1.1 and European 1.1 are supported). The
   complete normalized image is verified before Play becomes available.
3. Play. A gamepad is recommended. On keyboard: arrows or WASD steer, `X`
   accelerates, `Z` brakes, `Space` hops and power-slides, `Shift` fires
   items, `Enter` is Start. `F1` opens the settings overlay in-game.

Each release file ships with a `.provenance.json` naming the exact source
commit and SHA-256 it was built from.

For the current 1.0.5 candidate, follow the
**[human acceptance guide](docs/RELEASE_CANDIDATE_TEST_GUIDE.md)** and verify
each artifact's checksum and provenance sidecar before opening it. Maintainer
policy and automated release gates are in the
**[release checklist](docs/RELEASE_CHECKLIST.md)**.

> **macOS packaging notice:** the known-bad 1.0.0 DMG can produce Finder's
> “damaged and can't be opened” dialog because its executable was modified
> after the linker's integrity signature. This is not the ordinary
> unidentified-developer warning. The 1.0.5 candidate is intended to ship
> without Developer ID signing/notarization, so first launch may show the normal
> unidentified-developer warning. After the first blocked launch, open **System
> Settings → Privacy & Security**, scroll down, choose **Open Anyway**, and
> confirm **Open**. Apple documents that current flow in
> [Open apps safely on your Mac](https://support.apple.com/102445).
> Its nested code is still sealed inside-out and the mounted DMG is checked for
> signature integrity, self-contained dependencies, WebGPU selection, and the
> macOS 13 deployment target. “Damaged” is never an expected result. A future
> Developer ID/notarized build, if published, has the distinct name
> `Golden-Balloon-1.0.5-macos-arm64-signed-notarized.dmg` and distinct
> `developer-id-notarized` provenance; it is not the artifact for this patch.

### No game data is included

No ROM, textures, audio, music, models, or level data are distributed here.
Golden Balloon reads everything at runtime from your copy of the game. In the
browser the ROM is read locally and never uploaded, transmitted, or stored on
any server. Every release packaging path runs a structural asset/ROM guard:
[`tools/check_no_rom.sh`](tools/check_no_rom.sh) checks staged portable and web
artifacts, while [`macos/Scripts/verify_asset_free.sh`](macos/Scripts/verify_asset_free.sh)
checks the exact macOS bundle. See [DISCLAIMER.md](DISCLAIMER.md) and
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
| macOS (Apple silicon) | Supported. WebGPU with Restored presentation is the qualified path; OpenGL is an explicit diagnostic backend pending visual-parity work |
| Web (WebGPU browser) | WebGPU with Restored presentation is runtime-qualified on current Chromium. Safari/Firefox WebGPU is capability-detected but not yet in the project's release matrix |
| Linux | Best effort. Builds and runs on GL and WebGPU; physical-GPU, Wayland, and controller breadth are unmeasured |
| Windows | Supported portable x64 patch build. Native CI validates build/package/startup; the 1.0.1 WebGPU base also passed manual gameplay, controller, audio, save, and relaunch acceptance on Windows hardware |

## Presentation modes

| Mode | What it is |
|---|---|
| `--pure` | Original 4:3 framing and authored FOV, without presentation enhancements |
| `--restored` | **The qualified default.** Widescreen, supersampling, output-resolution HUD text, anisotropic filtering, and mipmaps while retaining the original art direction |
| `--remastered` | **Opt-in and work in progress.** Adds runtime SDF text, restrained level-derived directional lighting for racers and character objects, terrain-projected world shadows, and a bounded linear-light finish with per-world grading on top of Restored. It is not a qualified visual path until its remaining visual gates close. Broader material relighting remains future work |

The in-game Options → Video Options screen exposes all of this, plus aspect
ratio, FOV, and supersampling, applied live where the renderer allows and saved
with a restart notice where it does not. Settings persist in `mdkr64.ini`.

Native Settings also provides persistent window, controller, rumble, master,
music, and effects controls. Fullscreen is borderless desktop fullscreen and
can also be toggled with **F11** or **Alt+Enter**. Every normalized controller
button, D-pad direction, trigger, and right-stick direction can be remapped to
an N64 button or left unbound; the left stick remains analog steering. Rumble
can be disabled or set to Light, Balanced, or Strong without making the game's
Rumble Pak disappear. Audio previews audibly while dragged and ramps changes to
avoid clicks. If a restart-scoped presentation change is staged during play,
**Restart & Apply** performs an orderly engine shutdown and relaunches the same
validated ROM; the launcher applies the same change on the next **Play**. If
the replacement cannot stage or start, its one-shot handoff is cleared and the
normal launcher returns with a visible recovery message instead of exiting.

`Video.FrameLimit` defaults to **Original**, which presents the game's authored
motion. Native Settings keeps Frame Limit and Motion smoothing directly visible
and separates them from the gameplay-changing cadence control. Match Display,
numeric caps, and native Uncapped change presentation
opportunities without changing gameplay speed. Pair one of those policies with
`Video.MotionSmoothing=interpolate` to draw unique in-between images from
adjacent authored tasks; leave smoothing off to hold the latest authored image.
The browser is bounded by `requestAnimationFrame`, so a shared `uncapped`
setting maps honestly to Match Display there. WebGPU admission is nonblocking:
if the GPU is saturated, presentation holds a complete image and the game/audio
thread continues.

This matters most on the European (PAL) ROM. That release is authored at 50 Hz,
so each of its images is meant to be on screen for 40 ms — which is not a whole
number of refreshes on a 60 Hz monitor. Original therefore shows one image for
two refreshes and the next for three, and the alternation reads as unevenness
even though the game is running at exactly the speed it was authored to. Match
Display with Motion smoothing Interpolated fills that in with real in-between
images at your monitor's rate. Karts, cameras and music keep the European
game's original timing and pitch; only the number of pictures changes.

With Motion smoothing Off, Match Display and native Uncapped service held
authored images at the display cadence. Repeating the same image faster cannot
add motion and previously allowed an unbounded no-swap loop to consume a core
and disrupt audio service. Interpolated Uncapped remains genuinely uncapped.

The interpolation design, supported content, and known limits are documented in
[the uncapped-presentation notes](docs/UNCAPPED_PRESENTATION.md).

Every presentation and rendering mode keeps DKR's authored simulation cadence
unless the player explicitly selects Enhanced. That historical compatibility
path runs a faster, gameplay-changing 60 Hz simulation and remains available as
`--video-set Gameplay.SimulationCadence=enhanced`. Original is the default
because the independent-emulator Bluey lane measured Enhanced about 14% faster,
finishing 437 fields (7.28 seconds) early.

## Native launcher

The desktop launcher supports keyboard and gamepad navigation, visible focus,
and a persisted 0.75×–2.00× UI scale. It remains usable at a 640×480 logical
window by switching to compact top navigation and stacking controls that no
longer fit side by side; moving between standard and HiDPI displays rebuilds
its fonts before the next frame.

ROM and settings changes are transactional. An invalid ROM candidate never
replaces the last playable selection, and a setting that cannot be written
keeps the attempted value visible. Restore write access and press **Retry** to
save it in the same app session. On first use, a verified ROM remains playable
for the current session even if its path cannot be remembered; the launcher
says that it must be chosen again next time. If a validated ROM still fails
during engine boot, the engine tears down and returns to the launcher with the failure
visible; it does not leave the player at a terminal or dead window. F1 opens a
true pause boundary: simulation state and the race clock remain fixed while the
overlay owns input. Existing audio continues naturally, while game-timed audio
cues are frozen with the simulation. The desktop app supports keyboard and
gamepad use, visible focus, scalable high-contrast UI, and reduced motion. It
does not yet speak to screen readers, so it is
not advertised as screen-reader compatible.

## Status

The main automated coverage is summarized below. See
[tests/README.md](tests/README.md) for the full inventory and scope.

| Area | State | Demonstrated by |
|---|---|---|
| Boot and menus | Every screen navigable. Pixel fidelity vs the real ROM: 85.5–99.8% on frontend screens, 63.6% in-race | `check_nav_fixtures.py`; [the oracle](docs/ORACLE.md) (manual run) |
| Racing | All 20 tracks, all three vehicles, all 47 legal combinations. Full 3-lap Time Trial with saved and reloaded times | `check_track_sweep.py`, `check_vehicle_sweep.py`, `check_race_finish_time.py` |
| Renderers | WebGPU with Restored presentation is the qualified visual path. OpenGL remains an explicit diagnostic backend while its parity work continues, and Remastered remains opt-in WIP pending visual qualification. Optional presentation-only interpolation produces unique in-between images from immutable adjacent tasks; WebGPU never waits on gameplay/audio, drops excess visual work under saturation, and minimized windows stop GPU walks | `check_presentation_matrix.py`, `check_renderer_backends.py`, `check_gpu_backpressure.py`, and `check_surface_suspension.py` |
| Widescreen, UI, shadows | Hor+ world rendering, authored-FOV preservation, widescreen cinematics with contained wooden-frame previews, discrete safe/wide interpolation boundaries, output-resolution HUD in the 4:3 safe region, live resize/HiDPI, split-screen projection, terrain-projected shadows | `check_native_ui_resolution.py`, `check_widescreen_proportions.py`, `check_framed_world_views.py`, shadow and 2–4P gates on both backends |
| Memory-layout safety | Host-aligned object tails, bounds-checked object-map records | `check_native_layout.py` under halt-on-error alignment UBSan, with broken-direction controls |
| Local multiplayer | 2–4 player layouts, direct per-controller racer binding, per-player HUD, multiplayer results | `check_2p_human_binding.py`, `check_race_2p_split.py`, `check_race_multiplayer.py` |
| Playable Taj mod | Visible contiguous picker actor in every retail unlock layout; P1–P4 ownership; car/hovercraft/plane tuning; all 47 legal course/vehicle pairs; P2-led Adventure rebinding; native Rankings portrait; persistence and Time Trial quarantine | `check_taj_character_select.py`, `check_taj_playable.py`, `check_taj_p2_adventure.py`, `check_taj_results_portrait.py`, `check_taj_speed_profile.py`, Taj `check_vehicle_sweep.py`, and the browser Taj gates |
| Audio | Music, SFX, reverb, responsive vehicle engines, the final-lap tempo change, and the F1 pause mix through the clean-room audio engine | `check_audio_output.py`, `check_final_lap_music.py`, `check_vehicle_audio.py`, `check_raw16_audio.py`, `check_overlay_pause.py` |
| Browser | Real Chromium boots the wasm build, races, runs the AudioWorklet, restores ROM and saves across reload, and never sends the ROM over the network; display/numeric host schedules preserve fixed state/event/input/PCM and can fill rAF opportunities with presentation-only interpolation | `check_browser_runtime.py` (3,600-frame live run) + `check_browser_presentation_rates.py` |
| Mobile touch | Analog stick plus chorded Go/Brake/Drift/Item controls with safe-area placement | `check_touch_controls.py`, including a CDP three-finger chord traced to the game's own input read |
| Adventure mode | Both Adventure-mode loop variants: hub, balloons, lobby, races, mirrored Adventure Two, and win/loss persistence. This is not a claim that either full campaign is automated end to end; see [Current limitations](#current-limitations) | `check_adventure_hub.py`, `check_adventure_race_loop.py`, `check_adventure_two.py` |
| Boss/challenge/trophy coverage (not campaign completion) | All ten boss levels; the full first-boss progression with persistence; every authored challenge course; all four trophy championships | `check_first_boss_progression.py`, `check_challenge_modes.py`, `check_taj_challenges.py`, `check_trophy_series.py` |
| ROM support | US 1.1 and European 1.1 (byte-identical racing). Selection and every engine boot require exact size, byte-order normalization, revision identity, complete-image SHA-256, and valid asset-table bounds. Other revisions are refused by name | `test_app_shell`, `check_shell_dropfile.py`, `check_rom_revision.py`, [docs/ROM_REVISIONS.md](docs/ROM_REVISIONS.md) |

Hosted CI workflows exist for Linux GL/WebGPU, macOS, sanitizers, wasm, and
clean-room guards, but no hosted run has gone green yet, so every claim above
rests on local runs until one does. Platform claims above still distinguish
automated coverage from manual physical-hardware acceptance. See
[ROADMAP.md](ROADMAP.md).

## Current limitations

- The complete start-to-credits campaign is not automated or claimed complete.
  Silver-coin progression, later boss rematches, both Wizpig races, and the
  credits path remain outside the current gate breadth.
- Linux does not yet have a native **Choose ROM File** dialog. Drag and drop a
  ROM onto the launcher or enter its full path instead; the limitation avoids a
  new desktop-portal dependency and is not a disabled button that fails later.
- WebGPU with Restored presentation is the qualified visual path. OpenGL is
  retained only for diagnostics while its known opening-sequence visual-parity
  issue is investigated; Remastered remains opt-in WIP until its visual gates
  close.
- The planned macOS 1.0.5 artifact has an ad-hoc integrity seal but no Developer ID
  trust signature or Apple notarization, so current macOS requires the manual
  first-open approval above.
- The Windows build is not code-signed. Once a Windows archive is attached to a
  release, Windows SmartScreen will show an "unrecognized app" warning on first
  launch; choose **More info**, then **Run anyway** to continue.
- Linux is best effort and lacks the macOS/WebGPU path's physical-GPU,
  display-server, controller, and OS-version breadth. Windows build/package
  validation is automated; native GPU gameplay, controller, audio, and save
  acceptance remains a manual real-hardware release gate; the 1.0.1 base passed.
- On Windows, wgpu-native automatically selects a compatible API and may use
  Vulkan or Direct3D 12. `MDKR_RENDERER=webgpu|gl` selects the project renderer;
  it does not force either native API. Explicit selection and richer adapter
  diagnostics are deferred to a future portability release.
- The published Windows 1.0.3 archive predates the current source tree's
  UTF-8/extended-length filesystem boundary and reviewed application manifest.
  The 1.0.5 candidate must pass its Unicode and >260-character path gate on
  Windows hardware before those new guarantees can be claimed for a release.
- Modern camera obstruction correction is **opt-in**, through the launcher's
  Camera obstruction setting. Turned on, the runtime resolves every authored
  camera slot and publishes no penetrated, degraded, or invalid pose on any
  pinned route. What is not claimed is breadth — the CAM-00–CAM-09 release gates
  cover the qualified routes, not every camera bank and mode in the game, so an
  uncovered shot is possible, and that is why the authored camera is still the
  default. `MDKR_CAMERA_OBSTRUCTION` overrides the setting and additionally
  reaches the `center-ray` and `legacy` diagnostic arms. The correction is
  presentation-only and moves no authoritative state. The plan and evidence
  are in
  [docs/architecture/camera-obstruction.md](docs/architecture/camera-obstruction.md).

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
environment, CLI). `--pure` never rewrites presentation or gameplay values, so
reference comparisons cannot disturb a saved Remastered setup; master, music,
effects, window, controller-mapping, and rumble settings remain adjustable as
comfort-only exceptions.

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
