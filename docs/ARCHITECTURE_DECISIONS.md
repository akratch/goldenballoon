# mdkr64 — Diddy Kong Racing native PC port

Decomp-based source port of Diddy Kong Racing (N64), in the style of mgb64 (GoldenEye
port at the sibling `mgb64` checkout). Game logic is the decompiled C from
a local `Diddy-Kong-Racing` decomp checkout (vendored in `game/`), compiled
natively; libultra is replaced by a platform layer; graphics are HLE'd from DKR's
custom F3DDKR microcode to modern GPU APIs.

**Target for bring-up:** macOS arm64, OpenGL 4.1 core backend, SDL2 (system, 2.32),
ROM `baserom.us.v80.z64` (US 1.1) loaded at runtime for assets.

## Repo layout

```
game/src/        DKR decomp C (vendored; edits allowed but keep minimal + NATIVE_PORT-gated)
game/include/    DKR headers incl. f3ddkr.h, structs.h, PR/ (libultra headers)
game/libultra/   decompiled libultra (os/io mostly REPLACED; the audio synth was
                 removed entirely — audio is first-party clean-room, in platform/)
platform/        native platform layer (this project's own code + mgb64-derived)
platform/fast3d/ renderer: gfx_pc_dkr.c (F3DDKR front-end, new) + mgb64-derived backends
lib/             vendored: glad, sdl_gamecontrollerdb
docs/ref/        us.v80 linker script + symbol tables (reference for addresses)
```

## Key architecture decisions (settled — do not relitigate)

1. **Single-threaded cooperative model** (mgb64 pattern): no real threads.
   `osCreateThread`/`osStartThread` are no-ops; message queues are ring buffers;
   the main loop synthesizes retrace/PI/SP/DP messages. Game "threads" 1/3/30/4
   collapse into the main loop; thread30 bg-loading completes synchronously
   (DMA = memcpy, so "background" loads are instant).
2. **Memory:** one contiguous malloc'd arena (default 16 MB) standing in for RDRAM.
   `memory.c` pool init is patched (NATIVE_PORT) to use the arena. osMemSize = 4MB
   semantics preserved where pool math depends on it.
3. **32-bit pointer slots:** mgb64's pointer-tagging (`platform/gfx_ptr.h`).
   `OS_K0_TO_PHYSICAL`/`osVirtualToPhysical` on native = `gfx_ptr_store()` returning a
   32-bit tag; the F3DDKR HLE resolves tags → host pointers via `gfx_ptr_resolve()`.
   Round-trip `(type*)(u32)ptr` casts in game code are patched case-by-case
   (compiler flags `-Werror=int-to-pointer-cast` surfaces them).
4. **Assets from ROM at runtime:** `rom_io.c` loads the whole .z64 into memory;
   `dmacopy()` in `game/src/asset_loading.c` becomes memcpy from that buffer.
   Linker constants (`__ASSETS_LUT_START/END` etc.) become u32 constants for us.v80,
   extracted from `docs/ref/dkr.us.v80.ld` + symbols.
5. **Graphics = HLE of F3DDKR** in `platform/fast3d/gfx_pc_dkr.c` (new code; mgb64's
   `gfx_pc.c` at the sibling mgb64 checkout, `src/platform/fast3d/gfx_pc.c` is
   the structural template but its GE base-GBI decode does NOT apply).
   Below `GfxRenderingAPI` (`platform/fast3d/gfx_rendering_api.h`) everything is
   derived from mgb64 and maintained here for DKR's requirements.
6. **Audio:** DKR targets the libultra N-Audio ABI, but the synthesiser itself
   is **first-party clean-room code** — the `platform/audio_*.c` engine,
   shared with mgb64 and extended with DKR-specific behaviour. Its `Acmd`
   output is HLE'd by `platform/mixer.c` (aspMain emulator). The 49 decompiled
   SGI-legend synthesiser sources that originally filled this role were
   deleted at the swap; see [`architecture/audio.md`](architecture/audio.md)
   and [NOTICE.md](../NOTICE.md).
7. **Endianness (IMPORTANT):** ROM asset data is big-endian; the host is little-endian.
   Policy: **normalize at the asset-load boundary** — per-asset-type byteswap functions
   run right after `dmacopy` in `asset_loading.c` (NATIVE_PORT-gated), so ALL in-memory
   data (including level display lists, Vertex/Triangle batches, model headers) is
   native-endian afterwards. The F3DDKR HLE therefore assumes NATIVE-endian input
   everywhere. Layout references: `docs/ref/dkr_asset_spec.md` and
   `docs/ref/asset_fileTypes/*.hpp`. Gfx command words built by game code via macros
   are native u32 pairs already.
8. **ROM-overlaid structs on LP64 (M3 decision):** structs the game casts onto
   ROM/asset bytes (LevelHeader, TextureHeader, LevelModel(+segments), ObjectHeader,
   ObjectModel, …) must keep their exact N64/on-disk layout. Under NATIVE_PORT their
   embedded pointer fields become 4-byte `dkrptr32` token slots (u32); derefs go
   through `dkr_lo32_to_ptr()` (arena reconstruction). Game code that patches
   offsets→pointers in place stores the (truncated) arena pointer token — round-trips
   by construction. Every converted struct gets `_Static_assert(offsetof(...)==0x..)`
   locks against docs/ref/asset_fileTypes layouts. Runtime-only structs keep real
   host pointers — do NOT convert them.
9. **Version defines:** `-DVERSION_us_v80 -DNON_MATCHING=1 -DAVOID_UB=1 -DNATIVE_PORT=1
   -DF3DDKR_GBI -D_FINALROM -DNDEBUG`. No `TARGET_N64`. gcc/clang path uses the
   `#ifdef NON_MATCHING` C implementations in `game/src/hasm/`.

## F3DDKR HLE contract (platform/fast3d/gfx_pc_dkr.h)

Mirror mgb64's gfx_pc.h surface: `gfx_init(struct GfxRenderingAPI*, ...)`,
`gfx_start_frame()`, `gfx_run(Gfx* dl)`, `gfx_end_frame()`. The interpreter must handle
(see `game/include/f3ddkr.h`, usage in `game/src/rcp_dkr.c`):
- DKR `Vertex` (10B: s16 xyz, u8 rgba — NO UVs) and `Triangle` (16B: flags + 3 idx +
  3× s16 u,v) batches via `G_TRIN` (0x5) / `gSPPolygon` and `gSPVertexDKR`
  (32-vertex buffer). `G_VTX_APPEND` is **not** a running cursor: the RSP keeps
  one base — the length of the last flag-0 load — and every appended run lands
  at that same base, so consecutive appends overwrite each other and each run
  restarts its own triangle indices (`rsp.vtx_append_pos`, gfx_pc_dkr.c).
- `G_DMADL` (0x7) sub-display-lists.
- 3-slot matrix system (`gSPMatrixDKR`/`gSPSelectMatrixDKR`, `G_MW_MVPMATRIX` movewords).
- Billboarding (`G_MW_BILLBOARD` moveword: add vtx 0 anchor post-MVP).
- Standard RDP side: SETTIMG/SETTILE/LOADBLOCK/SETCOMBINE/SETOTHERMODE/FILLRECT/
  TEXRECT etc. (reuse mgb64 gfx_pc RDP handling patterns; combiner via gfx_cc.c).
- All DL-embedded addresses are **physical/tagged** — resolve with gfx_ptr_resolve().

## OS shim contract (platform/platform_os.h + platform/stubs_dkr.c)

Every os* symbol referenced by game/ must exist. Behavior per mgb64
(the sibling mgb64 checkout, `src/platform/{platform_os.h,stubs.c}`):
threads no-op; mesg queues = ring buffers; `osRecvMesg` OS_MESG_BLOCK on the retrace
queue drives frame pacing via `platform_frame_sync()`; `osPiStartDma`= memcpy + instant
DONE message; EEPROM (512B, `save/eeprom.bin`) and Controller Pak (file-backed) for
`save_data.c`; `osContGetReadData` fed from SDL (keyboard: arrows/Z=A/X=B/C=R etc.,
game controller via SDL_GameController); osGetCount/osGetTime from host clock
(COUNTER at 46.875 MHz).

## Milestones

- [x] M0 scaffold: repo, vendored code, CMake skeleton
- [x] M1 compile+link: all of game/ compiles natively, stubs satisfy link
      (build/mdkr64 links; headless run reaches init_game->audio_init, faults at
      the asset-LUT endianness boundary = the M2 asset-byteswap gate)
- [x] M2 boot: main_pc runs boot→menu logic without crash (logging, no video)
      DONE: asset byteswapping wired (asset_loading.c); fast3d renderer online
      (gfx_init(&gfx_opengl_api), gfx_start/run/end_frame per gfx task); the
      whole of init_game runs and the main loop cycles the intro, gfx_run
      consuming a real F3DDKR DL each frame (14 clean headless frames). The M3
      LP64 asset-struct wall documented in docs/OPEN_ITEMS.md is now resolved (M3).
- [x] M3 video: F3DDKR HLE renders boot/menus. Closed under M4: with real input
      the game advances into interactive menus that submit scene geometry, and it
      draws (title-screen demo level, menu backgrounds). The "menu black in
      headless" was a game-state/no-input artefact, now resolved.
- [~] M3 video (historical detail): F3DDKR HLE renders boot/menus — FIRST FRAMES achieved.
      DONE (architecture decision 8): every ROM-overlaid struct (LevelHeader,
      TextureHeader, TextureInfo, LevelModel(+Segment), ObjectModel, ObjectHeader)
      converted to 4-byte dkrptr32 token slots under NATIVE_PORT + _Static_assert
      offset/size locks; ~215 deref sites swept to DKR_PTR()/DKR_TOK(); the
      per-section post-inflate asset swaps wired (LEVEL_MODELS, OBJECT_MODELS,
      OBJECTS, SPRITES, LEVEL_OBJECT_MAPS, OBJECT_ANIMATIONS, LEVEL_HEADERS) +
      the ASSET_MISC LevelHeader_70 lightdata swap; a batch of LP64 runtime
      pointer-truncation / pointer-array-sizing bugs fixed (sprite/object/particle
      allocators, model+texture+sprite caches). `--dump-frames DIR` implemented
      (P6 PPM per present); the present no longer clears the just-rendered frame.
      M3b (renderer bring-up): the intro now renders TEXTURED, COLOURED geometry
      at the correct centred position (was black; the earlier "white quad" was the
      z-clear FILLRECT mis-drawn). Fixed, in dependency order: DL address
      resolution (OS_K0_TO_PHYSICAL bit-31 flip + registry-then-arena ordering;
      dkr_resolve/os_convert.h/gbi.h gDma1p/stubs_dkr.c), the gDma1p single-eval
      regression, the Mtx 128-vs-64-byte LP64 layout (gbi.h Mtx_t), texture
      materialisation (load_texture header byteswap + numOfTextures BE high byte +
      (s32) pointer-truncation fixes; asset_swap swap_texture_header), align16()
      (s32)->uintptr_t truncation, and an amCreateAudioMgr stack OOB. 300 frames
      exit 0, stable. REMAINING (see docs/OPEN_ITEMS.md "M3 rendering state"): the
      menu-with-level-background frames are black in HEADLESS only because no scene
      geometry is submitted without input (an M4 dependency, not a renderer bug);
      intro sprites draw as narrow bars (billboard/UV to verify); RGBA16 texels
      left big-endian (colours may be byte-swapped). M3 NOT fully closed until
      the menu scene is confirmed under M4 input.
- [~] M4 input: menu navigable, can start a race; core gameplay renders.
      DONE: SDL->OSContPad input (keyboard + SDL_GameController + --input-script),
      default windowed vsync mode, and the SI-queue fix that was the real "menus
      never advance" blocker (stub osContStartReadData posted no SI-completion, so
      input was read once). Boot reaches navigable menus: title (Press Start) ->
      character select / options, selection provably responds to input (DOWN then
      Start reaches OPTIONS not char-select). Scene geometry (title demo level,
      menu backgrounds) renders; cleared a chain of level-load/gameplay-init LP64
      + asset bugs (waves, shadows, BSP-tree byteswap, menu-text, font struct,
      BE-audio-bank deref) plus RGBA16/IA16/TLUT big-endian texel decode.
      starting an actual race not yet driven. See docs/OPEN_ITEMS.md.
      [M3c] menu glyph legibility + the DKR logo are now FIXED (gfx_pc_dkr.c): the
      title draws the full "DIDDY KONG RACING" logo + legible START/OPTIONS, the
      OPTIONS list (ENGLISH/SUBTITLES/AUDIO/SAVE/MAGIC CODES/RETURN) + PLAYER SELECT
      are legible, and the whole scene is textured. Root causes: DKR's gSPTexture
      scale is always 0 (absolute S10.5 UVs) so the F3DEX tc*scale>>16 zeroed every
      texcoord; the 32-bit tile LINE counts 2 bytes/texel (RG/BA bank split) so
      RGBA32 decoded at half stride; and the G_TP_NONE *0.5 texcoord halving was
      wrongly applied to absolute-coord TEXRECTs. Billboard bars/red-spikes: FIXED
      (fidelity: wave) — `Gsetcolor.color` `unsigned long` inflated sizeof(Gfx) 8→16
      on LP64, over-running tex_load_sprite's literal-8-Gfx-stride DL region into the
      sprite vertex buffer (HLE then read DL command bytes as verts); pinned to 32-bit
      under NATIVE_PORT. See STATUS.md M3c + docs/OPEN_ITEMS.md.
- [x] M4.5 WebGPU backend (required scope): DONE. Ported gfx_webgpu*.c from
      mgb64 into platform/fast3d/ and evolved it here (drives the same GfxRenderingAPI vtable —
      no F3DDKR front-end change); pinned wgpu-native prebuilt v29.0.1.1 via
      cmake/webgpu.cmake (SHA-256 verified, mirrors mgb64; reuses mgb64's fetch
      offline). option(MDKR_WEBGPU_BACKEND ON) gates code+define+link; GL stays
      compiled. Runtime select MDKR_RENDERER=webgpu|gl (default WebGPU, GL
      explicitly selectable for diagnostics; metal warns and resolves to the
      compiled default, WebGPU —
      gfx_metal.mm not built here). Window is
      backend-aware (SDL_WINDOW_METAL + Metal view for WebGPU vs GL context);
      headless --dump-frames captures WebGPU via the vtable's read_framebuffer_rgb
      (offscreen readback, no drawable needed). PARITY vs GL verified by eye +
      tools/compare_frames.py oracle metrics: logo/OPTIONS pixel-identical, race
      near-identical (hist ~0.99, block ~0.999); race 20x + title 300f x5 = 0
      crashes. Pipeline-prewarm cache DEFERRED (dormant; DKR never calls
      gfx_webgpu_set_stage) per docs/architecture/webgpu.md. See STATUS.md M4.5 +
      docs/architecture/web.md. This is the bridge to M8 (this repository's same
      file builds under Emscripten).
- [x] M5 audio: music + SFX via mixer HLE. DONE: the audio engine synthesises
      on the host, its Acmd macros overridden to the vendored software aspMain
      mixer (platform/mixer.c) via a NATIVE_PORT `#include "mixer.h"` at the tail of
      PR/abi.h; a per-frame synchronous pump (platform/audi_port_dkr.c +
      audiomgr.c amAudioSynthFrame) replaces the never-run audio thread, driven
      once per rendered frame from the stubs_dkr.c frame boundary; SDL2 queue-mode
      device @22050 Hz stereo s16 (headless-gated). ASSET_AUDIO un-stubbed: the
      bank parser was rewritten as a big-endian, LP64-safe parser building
      arena-resident host
      bank structs (the ASSET_AUDIO byteswap deferral is closed); real sequence
      table + MIDI header swap; SoundData u16 and VehicleSoundAsset mixed-field
      swap; sndp_play / sound_count
      un-stubbed. Addressing solved by routing all audio memory through the arena
      and reconstructing the truncated 32-bit synth ABI addresses in the mixer via
      dkr_lo32_to_ptr. VERIFIED objectively (headless PCM dump, MDKR_AUDIO_DUMP):
      race RMS ~6800 (per-second 3000-12000, varied), menus RMS ~4300, ASan-clean
      over menu+race synth, race_drive 20x + title 300f 0 crashes. Native reverb
      (ALFx delay lines) is ON (MDKR_AUDIO_REVERB=0 disables): the LP64 overrun
      was the u32 delay tap `&r->input[-d->input]` zero-extending instead of
      wrapping — fixed with a bounds guard + delay-line tail slack;
      see [docs/open-items/audio.md](open-items/audio.md). Turning it on also
      removed a spurious undelayed aux-send
      leak into the master bus (+2.5 dB of peak, 4.3x the clip events).
      **Superseded in detail:** the synthesiser this entry describes was the
      decompiled SGI one. It was later deleted and replaced by the first-party
      clean-room engine (`platform/audio_*.c`), measured within 0.5 dB of
      this baseline with a spectral cosine of 1.000. Everything above about the
      mixer, the pump, the sink and the addressing model still holds; see
      [docs/architecture/audio.md](architecture/audio.md).
- [~] M6 playable: the HEADLINE proof is DONE — the menus drive into an actual
      Time-Trial race (Tracks mode, Ancient Lake) and a HUMAN-CONTROLLED racer
      accelerates and steers, moving through the track, which renders (sky, palm
      trees, canyon, start line) with a working HUD (position/lap/timer). Route +
      driving committed as fixtures (tests/input_scripts/nav_to_time_trial_race.txt,
      race_drive_time_trial.txt). 0 crashes over 20x the drive script + 5x title
      300f; char-select/options fixtures still pass. Root-caused + fixed 11 LP64/
      endianness bugs on the level-load->race path (see STATUS.md M6). Billboard-
      sprite red-spikes over the karts: FIXED (fidelity: wave — sizeof(Gfx) LP64
      inflation via Gsetcolor.color; see STATUS.md). REMAINING: full-lap/save
      correctness, audio (M5). (60/30fps pacing: RESOLVED, pacing wave.)
- [ ] M7 polish: Metal backend, config, resolution scaling, gamepad bindings
- [x] M8 browser build (the end goal for this project): DONE + PROVEN IN-BROWSER. The
      wasm engine compiles+links clean, BOOTS in a real browser (headless Chrome 150 /
      Apple Metal-3 driving the actual mdkr64_web.wasm with a user ROM in MEMFS),
      brings up WebGPU, runs the full cooperative loop at 60fps via the Asyncify/
      requestAnimationFrame frame boundary, and RENDERS the title/attract, menus, and
      a race CORRECTLY (scored vs native: attract + OPTIONS hist=0.996 block=1.000;
      in-race eyeballed-correct with HUD). User supplies their own .z64, saves persist
      via IDBFS.
      DONE: CMakeLists EMSCRIPTEN branch (mdkr64_web, mirrors ge007_web);
      -sUSE_SDL=2 + --use-port=emdawnwebgpu (webgpu.cmake); narrowed Asyncify
      (ADD=main,gfx_init,wgpu_init,gfx_webgpu_bringup,osRecvMesg,... confirmed via
      -sASYNCIFY_ADVISE); platform_vi_pace_measure suspends to rAF keeping the
      VI-field updateRate pacing; web_audio_worklet.c (vendored from mgb64, fed the
      same mixer buffers, SDL fallback); dist/web shell (ROM file-picker -> MEMFS/
      IDBFS, WebGPU feature-detect, "no ROM distributed" notice, #canvas surface).
      Fixed THREE wasm32-specific bugs (architecture decision 8 watch-item, LP64-only assumptions at
      32-bit pointer width): (1) the arena/N64-segment-token address collision (arena
      forced above the 0x10000000 segment ceiling); (2) the z-clear FILLRECT drawing
      white (compare RAW SETCIMG/SETZIMG tokens, not resolved pointers); (3) THE
      render-fidelity fix — global/rodata pointer recovery: on wasm32 non-arena host
      pointers are <4GB so they never register in the gfx_ptr registry (LP64's >4GB
      test), so dkr_resolve returned NULL for globals -> sky/menu/kart geometry didn't
      draw. Fixed by recovering host pointers DIRECTLY from the token in dkr_resolve
      (ILP32 flip is reversible; __EMSCRIPTEN__-gated, native byte-identical). Native
      re-verified unregressed: 20x race 0 crashes, WebGPU renders title+OPTIONS, GL
      fallback boots. See STATUS.md M8 + docs/architecture/web.md + docs/OPEN_ITEMS.md.
- [ ] M9 player-owned saves (BROWSER 1.0 REQUIREMENT): export the exact 512-byte
      EEPROM as a raw or versioned local backup; import through validation,
      preview, rollback, atomic replacement and reread verification; provide a
      simple semantic editor that preserves unknown bytes and recomputes only
      affected checksums. Save management must work from the launcher without a
      ROM, WebGPU, or running engine, perform zero uploads, and restore exact
      progression after complete browser-site-data loss. Native CLI parity and
      corrupt-block recovery follow the same shared ROM-free byte codec. The
      formats and recovery contract are documented in `SAVE_MANAGEMENT.md` and
      enforced by the save codec/container tests.

## Build

```
cmake -S . -B build && cmake --build build -j   # produces build/mdkr64
./build/mdkr64 [--headless-frames N] [--rom path/to/baserom.us.v80.z64]
```
`--headless-frames N`: run N frames then exit (for automated testing); with
`--dump-frames DIR` writes one binary-PPM (P6) file per presented frame, as
`DIR/frame_%04d.ppm` — dependency-free, no image library linked. Convert with
`sips -s format png` (macOS) or any converter if you want PNGs.

## Working agreements

- Treat any local checkout of the upstream decompilation or of the sibling mgb64
  port as read-only reference material; changes belong in this tree.
- Keep decomp edits inside `#ifdef NATIVE_PORT` (or `#ifndef NATIVE_PORT` around
  N64-only code) where feasible; sweeping mechanical patches are fine when gated.
- Compile check: `cmake --build build -j 2>&1 | tail -40`.
- Update the milestone checklist and `STATUS.md` when you land something.
