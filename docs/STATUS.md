# Status log

## Post-v0.4 — pre-release deep review and wave "shadowdeep" (2026-07-29)

Before cutting the next release, a four-lane deep review (adversarial review
of the new commits, residual renderer mining, mobile touch layer, and a
release-claims audit) plus one complete Release-led manifest run
(**66/66 tasks in 96m54s** as the manifest stood at that checkpoint —
it is 90 tasks today — including the 47m22s alignment-UBSan
native-layout matrix) mined the remaining issues. Findings and dispositions
are recorded in the renderer's wave "shadowdeep" entries in
`docs/OPEN_ITEMS.md`. Headlines:

- **Shipping builds never reset the shadow static caster cache** — the reset
  call was reachable only through the diagnostic trace path, which every
  shadow gate happened to enable. Fixed unconditionally at level load; the new
  registered `check_shadow_stage_reset.py` proves shipping/traced equality
  (static census 427) against a suppressed-reset control (694).
- **The void curtain froze into the cache as a phantom wall**; wavegen
  lava/water cast opaque shadows; `RENDER_NO_SHADOW` was never honored. A
  DL-build-time caster-exclusion seam closes all three.
- **Shadow bias is now authored in world units** (8.0) and normalized by each
  plan's z-span; masked ranges no longer inflate caster bounds; the far
  terminator fades out; the WebGPU shadow pipeline gained `unclippedDepth`
  parity; post-watermark static ranges merge instead of drawing one call per
  triangle; the stage AABB seeds correctly and rejects implausible vertices
  with telemetry.
- **The touch shell could blank the entire launcher on older engines** (the
  new capability-listener path called `MediaQueryList.addEventListener`
  unguarded inside the un-caught bootstrap). Feature-detected, try/caught, and
  the whole touch wiring is now non-fatal to the launcher; canvas gestures
  stay suppressed with the overlay hidden; the Look button holds the 44 px
  floor.
- The 2P gate's motion/separation statistics run over the racing window only,
  and its viewport-liveness window extends to the true end of racing.

Evidence at this checkpoint: Debug/Release/ASan builds clean; extended
`shadow_frame` unit (exclusion marks, plausibility clamp, masked-bounds
exclusion, watermark merge immutability) plus all shadow/lighting CTests pass;
`check_shadow_stage_reset`, `check_world_shadows` (GL+WebGPU+fault),
`check_world_fx_capture`, `check_shadow_visual_ab`, `check_widescreen_shadow`,
and `check_remaster_lighting` pass on the fixed build; linked wasm rebuilds
cleanly; before/after captures show start-area pillars and shore rock
naturally lit with kart/racer/structure shadows and pickup grounding intact.

## Post-v0.4 — Remastered shadow playthrough repair (2026-07-29)

The v0.4 world-depth candidate's first real playthrough reported shadows that
degraded the experience: hard shadow bands from no visible source, a heavy
splotchy look on bright art, and pickups floating without grounding. All four
root causes are repaired on main:

1. **Phantom shadows from culled geometry.** Static casters are cached for the
   whole stage (correct — off-screen geometry must still cast), but the
   cascade planner extended its caster depth range only over the triangles the
   game submitted that frame. Cached casters the game had CPU-culled fell
   outside the planned light z-range; GL depth-clamped them onto the light
   near plane as full-strength phantoms while WebGPU clipped them away. The
   stage cache now carries a world-space AABB that each committed view's
   bounds cover, so every drawn caster is planned at its true depth and both
   backends agree.
2. **Hash-invented sun heading.** Worlds with no authored directional signal
   (weather drift or sky scroll) derived their sun azimuth from a hash of
   level identity — an arbitrary compass direction that cascaded shadows made
   plainly visible. Uncued worlds now share the canonical key-light heading
   already used by the degenerate fallback; sky-gradient elevation and all
   authored-signal worlds are unchanged.
3. **Floating pickups.** The actor decal handoff suppressed the projected blob
   for every non-scenery caster batch, but billboard-sprite actors are
   deliberately excluded from the caster feed, so bananas/balloons lost their
   only grounding. Only 3D-model actors now trade their decal for a map
   shadow.
4. **Umbra depth.** Shadowed pixels were multiplied by 0.48 after lighting on
   art whose baked vertex colour already encodes authored occlusion. The
   umbra is now 0.62.

Evidence: `level_lighting`, `shadow_frame`, and `shadow_cascade` CTests plus
all 28 Debug CTests pass; `check_world_shadows` passes on GL and WebGPU with
near-identical change footprints (8536 vs 8526 pixels) including forced-loss
fallback/latch arms; `check_remaster_lighting` passes with Pure/Restored exact
and `[PACE]` invariant; `check_world_fx_capture` remains byte-identical.
Before/after Ancient Lake captures show the phantom band and striping on the
start-area pillars gone and scenery shading softened without losing kart,
racer, or structure shadows.

Known residual (recorded, not fixed): the static caster cache keys on raw
arena triangle addresses with no free-time invalidation hook, the same class
the texture cache already handles via `mempool_slot_clear`. See
`docs/OPEN_ITEMS.md`.

## Post-v0.4 candidate — adaptive mobile controls

Main now contains an unreleased phone/tablet control layer with real analog
steering and simultaneous race actions. It auto-detects coarse/no-hover touch
devices, observes display cutout safe areas, yields to gamepads, and clears
input across blur, page hide, visibility loss, fullscreen transitions, and
manual hiding. The registered browser gate preserves the implementation and
local Chromium evidence.
Released as part of `v0.5` (2026-07-29) with the shadow deep-review waves and
the registered touch gate; the public page now serves the `v0.5` build by
maintainer decision.

## Current integration snapshot — v0.4 world-depth release

This reviewed snapshot is released as **v0.4** and published at
<https://akratch.github.io/golden-balloon/>.

The defined Waves 1–3 are **23/23 complete** and integrated:

| Wave | Scope | Completion |
|---|---|---:|
| Wave 1 core safety | MEM-11, MEM-12, PORT-01, C-01, C-02, GAME-06 | 6/6 |
| Wave 1 WebGPU lifecycle | WGPU-01..06, WGPU-08, WGPU-10 | 8/8 |
| Wave 2 lighting | RL-2, RL-5, CO-1 | 3/3 |
| Wave 3 gameplay | 3P/4P, Adventure Two, challenge/battle, first boss, Taj, trophy series | 6/6 |

The manifest contains **82 check scripts / 90 full-run tasks**. That v0.4
snapshot exposed **28 CTests**; the current default native configuration
exposes **40**. Linux X11/Mesa now has a
clean Ubuntu/GCC build and real GL plus WebGPU/Vulkan render/present runs.
Wayland, physical Linux GPUs, WGPU-11's offline/external corpus remainder, the full Adventure graph, physical
peripheral breadth, broader ROM support, and broader independent-oracle contracts remain
outside these wave definitions. Save tooling and the in-game video/accessibility
screen landed after the wave count below.

The senior-review closeout also repaired the production fullscreen freeze
mechanism: oversized fullscreen supersampling now respects a proportional
render-area budget while output stays native, WebGPU resize debounce has one
owner, and real Chromium proves live frames through fullscreen entry and exit.

The first independent state lane now drives the real US 1.1 ROM and native port
through a full Ancient Lake lap. The authored two-field arm measures 63.663%
checkpoint/lap agreement and 1,259.956 position p95; the shipping 60 Hz arm
measures 7.526% and 7,767.070. Both complete a lap and both fail strict parity.
F-18 is therefore partial, not open; challenge breadth, multiplayer,
progression/save, audio, renderer-state, and standard-race parity remain.

The new all-racer Bubbler lane closes the cadence-policy part of that gap.
Unmodified US 1.1 Bubbler finishes at tick 3,459; native two-field Bubbler at
3,458 with a 1.00047× mean-speed ratio, while the historical one-field arm
finishes at 3,022 with a 1.13965× speed ratio. Interactive gameplay therefore
defaults to persisted `Gameplay.SimulationCadence=original`; the old one-field
behavior remains an explicit restart-scoped `enhanced` option.

The Remastered world-depth and colour epic is complete: typed capture-once caster geometry, stable 1P–4P cascaded
shadows on GL/WebGPU/browser, truthful projected-decal fallback, a scene-linear
filmic finish, and bounded runtime-derived per-world grading. Contact AO was
explicitly cut after its inherited projection proved unreachable and its
backend algorithms failed the common quality contract. The world-depth
rendering itself is cadence-neutral and keeps authored 2D outside world
post-processing.
The final browser gate sustains the temporary complete-loop authored cadence at
median 30.0 fps with 99.9% two-field updates, no sub-two-field update, and
35.35/35.64 ms p95/p99. Production shadows completed for 3,489/3,597 attempted
frames with truthful fallback during prewarm, and fullscreen entry/exit stayed
live.

## ROM-free push/PR correctness matrix

The repository now has a read-only correctness workflow separate from the
manually dispatched Pages publisher. Every push to `main` and every pull request
runs Linux WebGPU and OpenGL-only Release builds, a macOS WebGPU Release build
with the high-risk warning register restored, all 27 ROM-free CTests, a Linux
ASan+UBSan build, linked wasm, browser save custody, clean-room history, and the
shipped-artifact ROM guard. All actions are immutable-SHA pinned.

`check_ci_contract.py` is registered and positive-controls triggers,
matrix cells, action pins, save custody, and ROM guards. Local actionlint/YAML
validation, the macOS warning build, GL-only Release/CTest, Apple ASan with its
unsupported leak mode disabled, clean-room history, and exact browser-save lane
pass. F-10 remains partial until the hosted Linux/Web jobs have completed and
repository branch protection requires them.

## Typed native address domains

Native ROM extents and asset-LUT positions are numeric `MdkrRomOffset`/`u32`
values rather than fake pointers. Framebuffer duplication receives a byte count
rather than an end pointer truncated to 32 bits. Audio arena tokens and opaque
libultra scalar/message payloads cross through the named helpers in
`address_domains.h`; ROM-overlaid pointer slots remain isolated behind
`dkrptr32`/`DKR_TOK`/`DKR_PTR`.

`check_address_domains.py` is registered and rejects the
diagnostic-hiding `(u32)(uintptr_t)pointer` family anywhere in active native
code except the three unavoidable conversions in the two boundary headers.
Direct pointer/integer casts remain compiler errors. Its native-broken and
matching-only controls pass. Together with generation-checked lookups and
stable native/real-wasm cycle high-water, this closes F-12.

## Complete retail model corpus

`check_rom_model_corpus.py` independently inflates and validates all 390 object
models and all 55 level models in the supported revision-1 payload: 1,146 level
segments, 16,943 batches, 197,910 vertices, and 143,045 triangles. All
renderer-consumed geometry/visibility regions, sentinels, texture references,
and triangle-local indices pass on US 1.1 and European 1.1. Retail high-water
is 24/32 vertices and 16/16 triangles per batch.

This closes F-09/F-17's local offline dormant-model structural walk. F-09
retains the independent intermediate-state oracle; F-17 retains equivalent
browser/minimum-feature/non-Apple execution.

## Cooperative final shutdown and browser reload

Finite runs no longer terminate inside the frame pump. SDL quit, headless
completion, and renderer failure request a cooperative exit; `main()` then
closes audio, releases frontend/backend renderer ownership, and finally
destroys SDL. GL releases context children explicitly. WebGPU releases every
child and CPU resource table, destroys native-owned roots, and leaves
browser-borrowed roots to the shell. Generation-tagged asynchronous pipeline
callbacks cannot dereference shader metadata from a retired device.

`check_final_shutdown.py` passes GL/WebGPU in Debug, Release, and ASan and
requires zero terminal ownership in dependency order. The 76-case WebGPU
recovery matrix still passes, including command-finish cleanup and live GL
fallback. Real Chromium proves AudioWorklet closure, zero WebGPU/frontend
children, ordinary reload, and cleanup before adapter/asynchronous-pipeline
failure UI. A real-Chromium four-load gate now proves stable game/audio heaps,
exact physical/music/jingle/SFX list conservation, coherent renderer
generations, and zero ROM/arena/delayed-free/renderer scratch ownership. This
closes F-19; the subsequent typed address-domain census closes F-12.

## Consecutive capture and browser pipeline budget

GL frame dumps now retain the completed pre-swap composite, so VI presents
without a new graphics task no longer alternate valid content with undefined
black back-buffer storage. The registered renderer gate checks 215 consecutive
captures and rejects the historical A/B/A shape.

The real Chromium gate enforces exactly one NTSC/60 authored realtime clock,
no update or wall-field count below two, a 24–36 FPS median, 40.0/45.0 ms
p95/p99 budgets, and a two-frame maximum for async pipeline completion and
last-complete-frame holds. The v0.4 run measured median 30.0 FPS with
35.35/35.64 ms p95/p99; all 36 cold pipelines completed with one-frame
compile/hold maxima. These are temporary whole-loop containment budgets until
fixed authoritative simulation and high-rate presentation are separated.
F-28 and WGPU-09 are closed; persistent stage prewarm remains dormant by
evidence. The raw ~158 ms transition maximum remains reported separately.

## WebGPU shipped fault-matrix closure

The 105-point fault registry is fully wired and policy-classified. A structural
gate now requires every one of the 73 native/shared shipped points and all three
browser-only points in their executable runtime matrices, while forbidding
runtime credit for 29 dormant inherited routes. The current native run passes
76 injections plus forced vertex segmentation and shader-index exhaustion; the
real Chromium gate covers all browser-only points.

The last flaky native arm — surface-capture buffer allocation on an occluded
headless surface — now consumes the selected allocation-failure seam
deterministically without touching a null drawable. Normal capture still
requires a real surface texture. F-05 is closed; external implementations stay
in F-03/F-10/F-21, and dormant/minimum-capacity content breadth stays F-09/F-17.

## UI-4 — in-game video and accessibility options

The native-port Options menu now reaches a localized Video Options screen for
Pure/Restored/Remastered presentation, 1×–4× supersampling, aspect ratio,
authored-relative FOV, atomic filtering tiers, Remaster effects, and subtitles.
Aspect, FOV, and render scale publish live only after the config transaction
succeeds. Shader/sampler/texture/font/lighting-owned changes stage a restart and
remain visibly pending.

Configuration has separate active/desired snapshots and monotonic source ranks:
defaults, file, native preset, browser launcher, in-game, environment, then CLI.
Higher-ranked values are visibly locked and never baked into the user file.
Native writes synchronize a temporary and atomically replace `mdkr64.ini`;
browser writes replace `/save/mdkr64.ini` and enter the existing coalesced IDBFS
durability queue. Unknown keys survive. Malformed launcher input and unwritable
storage leave both the active setting and prior file unchanged.

`video_config` and `video_config_runtime` provide ROM/GPU-free coverage.
`check_video_options.py` passes on native WebGPU and GL across successful
mutation/reload, env locking, all-control storage failure, and malformed-launch
no-rewrite arms. The real Chromium gate drives the same menu, reloads 16:10,
FOV 50, and 3× SSAA from IDBFS, and proves campaign-only erase preserves video
preferences.

## WGPU-11 visible-content census and queued-pointer repair

The registered 46-route WebGPU census covers nine menus, 20 main tracks, ten
bosses, four challenge courses, Adventure intro/hub, and 3P/4P layouts. It
passes 249,339,186 strict F3DDKR commands with 35 opcodes, zero faults, 29
material identities, 37 pipeline keys, and a maximum 3/32 pipelines per
material.

Broadening to multiplayer corrected an earlier diagnosis: the preview child
was not an intentional unterminated list. Stage teardown erased `dRspInit`'s
host-pointer registration after a task had encoded it, so its low token
sometimes resolved as segment-1 arena garbage. One-generation registry grace
now protects queued tasks without retaining unobserved mappings indefinitely;
the repeated GL/WebGPU resource gate plateaus at registry high-water 41.

The subsequent capacity/fault closeout forces a one-entry shader table, proves
two bounded refusals without pointer reuse or pipeline corruption, recovers the
device once, and then switches the live process to GL. The native attachment-
only surface blit and presented-surface capture paths now execute under fault
injection. The registry is 105/105 classified: 72 shared shipped, one
native-only, three browser-only, and 29 explicitly dormant inherited routes;
the recovery gate passes 76 injected native cases plus forced vertex
  segmentation and shader-index exhaustion.

## Optimized full-UBSan and GCC/O1 checkpoint

The available-host production diagnostic set is empty under both the original
optimized Clang cell and a real GCC 16 `-O1` cell. The registered
`check_full_ubsan.py` gate now makes the compiler, optimization, build
directory, and GL-only/WebGPU breadth explicit. It names float-cast overflow
separately because GCC omits that class from its `undefined` alias, verifies
linked handlers and two aborting positive controls, then runs the 46-route
WebGPU census when available, all 47 legal track/vehicle combinations, 3P/4P
through results, challenge win/loss results, first-save filename entry, and a
7,500-frame race. It has no production allow-list.

The repair includes defined color/boost/fill packing, negative fixed-point
scales, MIPS-compatible pitch conversions, zero-pitch Doppler handling, and
four-player Doppler history without growing the existing sound-data allocation
(248 bytes on LP64, original 0xE0 on wasm32). Both sizes are compile-time
asserted. A baseline worktree comparison caught and rejected an intermediate
sanitizer-clean gameplay regression; Release and ASan 3P/4P pass on the final
exact-semantics form. The GCC pass additionally fixed false pointer/scalar
types, direct host-pointer stores into 32-bit asset slots, variadic float
reads, orphan collision facets, and indeterminate audio/camera/AI/plane state.
Debug, Release, ASan, wasm, real Chromium, and the 46-route native WebGPU census
all pass afterward. F-01 remains partial only because its written exit gate
also requires an external toolchain/platform witness.

- Wave `core-safety` COMPLETE (2026-07-27): audit findings MEM-11, MEM-12,
  PORT-01, C-01, C-02, and GAME-06 are closed. Texture/CI construction is
  transactional and exactly sized; custom-FX setup validates and owns its
  temporary assets with a dry fallback; level/object-map address arithmetic has
  explicit token/offset/host-pointer domains; libultra envelope math reproduces
  the original MIPS conversion and 16.16 behavior without C UB; the audited
  Pak/course/trophy/Taj/snow shifts and save-derived model selections are
  bounded at use. The two collision arrays with four slots of measured slack
  were deliberately retained because their capacity-plumbed producers recorded
  no bound hits.
  - Gates: Debug and Release CTest 12/12; ASan and Release 7,500-frame race;
    optimized 240-frame limited-UBSan boot; PCM and RAW16 audio gates; linked
    wasm/WebGPU build plus ROM-absence scan.
  - Full `-fsanitize=undefined` still stops at the pre-existing
    `rcp_dkr.c:430` signed shift. This is separate backlog, not an exception or
    suppression in the core-safety gate.

## relight — runtime-derived sun, smooth kart lighting, explicit colour space

Wave 2 closes RL-2, RL-5, and CO-1 without changing Pure or Restored. The game
derives a stable per-world direction, chromaticity, and restrained 10–16%
strength from the loaded `LevelHeader` and `LevelHeader_70` records; there is no
authored per-level table. DKR's baked vertex colour remains the ambient and
exposure base.

Racers and character/vehicle-animation objects carry their real compact model
normal streams and an object-local sun direction through native-only queued
F3DDKR metadata. GL, WebGPU/browser, and the source-maintained Metal path
interpolate those smooth normals per fragment. The old derivative-light option
bit is retained for shader-key compatibility but no derivative or coarse face
normal remains.

CO-1 is documented in `docs/COLOUR_LIGHTING_PIPELINE.md`: the N64 combiner
continues in authored sRGB code space, tagged Remastered fragments decode once
to linear RGB for directional light and fog, then encode once to the UNORM scene
target. Alpha remains coverage. Invalid rigs and bad/missing normal tokens fail
closed.

The ROM-free unit covers transfer functions, deterministic derivation, positive
controls, fallback/reset and direction packing. The real renderer gate drives
Ancient Lake and Fire Mountain on GL/WebGPU, requires nonzero racer and
character coverage with zero missing normals, exact `[PACE]`, backend parity,
and byte-identical Pure/Restored reverse arms. Measured production deltas are
subtle by design: GL `0.148/0.060` and WebGPU `0.150/0.061` mean absolute RGB
difference for Ancient Lake/Fire Mountain.

- M0 scaffold complete: game code + platform spine vendored, repo initialized.
- M1 compile+link COMPLETE: the whole game (game/src + hasm NON_MATCHING C +
  libultra audio synth) compiles and links natively into `build/mdkr64` with the
  libultra OS shim (platform/stubs_dkr.c), ROM I/O, segment constants, gu math
  port, and a minimal SDL2+OpenGL main. Renderer (platform/fast3d) and the
  hand-asm C replacements are satisfied by weak stubs until those agents land.
  - Build: `cmake -S . -B build && cmake --build build -j` -> 0 errors.
  - `./build/mdkr64 --headless-frames 3 --rom baserom.us.v80.z64` loads the ROM,
    brings up GL 4.1 (Metal-backed on macOS), allocates the 16 MB arena, enters
    the boot path (thread3_main -> init_game) and runs video_init / pi_init /
    audio_init, then faults in asset_table_load() reading the asset LUT as
    little-endian. That endianness normalization is the separate asset-byteswap
    workstream (architecture decision 7), deliberately not done here — it is the M2 gate.

- M2 boot IN PROGRESS (asset-swap wired + renderer online + boot reaches the
  main loop; blocked at the M3 LP64 asset-struct-layout wall):
  - Asset byteswapping wired into game/src/asset_loading.c per the
    platform/asset_swap.h contract: asset_swap_lut() on the master LUT in
    pi_init, asset_swap_normalize(type,...) after every asset_table_load dmacopy.
  - fast3d renderer brought online (M1 had it weak-stubbed): CMake globs
    platform/fast3d (gfx_pc_dkr.c HLE + the mgb64 GL backend); gfx_metal.mm and
    screenshot_series.c excluded. gfx_pc.h shim + fast3d_shim/ (GE-symbol shims)
    + gfx_ptr.c (registry globals) + gfx_config_shim.c (post-FX off) close the
    build. main_pc calls gfx_init(&gfx_opengl_api)+gfx_set_dimensions; the
    cooperative M_GFXTASK dispatch brackets the real gfx_run(dl) with
    gfx_start_frame/gfx_end_frame. gfx_run walks a real F3DDKR DL per frame with
    no crash. dkr_resolve() reconciles addresses: dkr_lo32_to_ptr() (arena) first,
    segment-table / gfx_ptr registry fallback.
  - Boot fixes (all NATIVE_PORT-gated) found iterating the headless boot:
    audio silence-stub bring-up (empty banks/seq, LP64 audio heap x4),
    amCreateAudioMgr custom-FX-param swap, decrypt_magic_codes
    sizeof(s32*)->sizeof(s32) overrun, init_particle_assets LP64
    offset-table->pointer-array rebuild, cam_init anti-piracy HW-register read
    gated out.
  - RESULT: `./build/mdkr64 --headless-frames 14` boots through init_game and
    cycles the main loop (gfx_run consuming DLs) and exits 0. Frame ~15 hits the
    M3 wall: level_load reads LevelHeader.unk74[] at an LP64-shifted offset ->
    garbage -> func_8007F1E8 OOB. MDKR_TRACE=1 traces asset loads, gfx tasks,
    and per-frame presents.
  - M3 blocker documented in docs/OPEN_ITEMS.md ("LP64 asset-struct layout").

- M3 video IN PROGRESS — first rendered frames, 300 headless frames exit 0:
  - architecture decision 8 implemented. New header game/include/dkr_native_ptr.h:
    `typedef u32 dkrptr32`, DKR_PTR(T,x)=(T*)dkr_lo32_to_ptr(x),
    DKR_TOK(p)=(dkrptr32)(uintptr_t)p, and DKR_ASSERT_OFFSET/SIZE locks (both
    reduce to plain casts under !NATIVE_PORT). Included from structs.h.
  - ROM-overlaid structs converted (embedded pointer fields -> dkrptr32,
    per-field #ifdef NATIVE_PORT, static-assert offset/size locks):
    LevelHeader (AILevelTable, unk70[], unk74[7], unkA4, pulseLightData),
    TextureHeader (cmd), TextureInfo (texture), LevelModel (6 ptrs),
    LevelModelSegment (8 ptrs), ObjectModel (11 ptrs), ObjectHeader (5 ptrs).
    Runtime structs (Object, Sprite, ModelInstance, ObjectModel_44, …) kept real
    pointers.
  - ~215 deref sites swept to DKR_PTR() (compiler-driven: subscript/member-ref
    hard errors + a temporary -Werror=int-conversion pass to surface implicit
    int<->pointer conversions). Offset->pointer patch sites store DKR_TOK()
    tokens; the tracks.c LOCAL_OFFSET_TO_RAM_ADDRESS macro already produces the
    lo32 token unchanged.
  - Post-inflate / whole-record asset swaps wired (the M2 "integration pending"
    item): ASSET_LEVEL_MODELS (tracks.c), ASSET_OBJECT_MODELS +
    asset_swap_object_animation (object_models.c), ASSET_OBJECTS (objects.c),
    ASSET_SPRITES (textures_sprites.c x4), ASSET_LEVEL_OBJECT_MAPS (objects.c),
    ASSET_LEVEL_HEADERS (game.c x3). gzip.h now exports gzip_inflate_output so
    the swap can size the just-inflated buffer.
  - ASSET_MISC LevelHeader_70 "pulsating light" sub-asset swap
    (asset_swap_misc_lightdata, dedup-guarded against per-level re-swap) called
    at the two func_8007F1E8 sites (game.c unk74[] loop, game_ui.c ASSET_MISC_58).
  - LP64 runtime pointer-truncation / pointer-array-size fixes found iterating
    boot: gzip_inflate input pointers (texture/level-model/object-model
    compressedStart), texture/sprite/model caches (read via DKR_PTR, compare via
    (s32)), sprite allocator (sizeof(Sprite)/pointer arrays vs hardcoded 4),
    gParticleDummys (*sizeof(Sprite*)), gObjectMap header skip (4 s32s not
    sizeof(uintptr_t)), ObjectModel_44 array stride, obj64 behavior-data pointers
    ((u8*) not (s32)), gMenuAssets/obj->sprites/minimapSpriteIndex sprite frees.
    One defensive guard in obj_init_animobject (skips a mis-resolved
    animation-target object whose behavior-data union is NULL — see OPEN_ITEMS).
  - Frame dumping (--dump-frames DIR -> DIR/frame_%04d.ppm, P6, glReadPixels of
    the default framebuffer before swap). platform_sdl_present no longer clears
    (was wiping the rendered frame pre-swap).
  - RESULT: `./build/mdkr64 --headless-frames 300 --dump-frames DIR` exits 0.
    Intro frames (~20-172) render a centred quad in white (geometry at correct
    screen position; textures not yet sampled). Menu-with-level-background frames
    (~173-300) present black. Both are next-milestone renderer work.

- M3b render bring-up (renderer specialist) — intro now renders TEXTURED,
  COLOURED geometry (was pure black/white); 300 headless frames exit 0, stable
  across repeated runs. The "white intro quad" was a MISDIAGNOSIS: it was the
  z-buffer clear FILLRECT being drawn to screen because SETCIMG/SETZIMG resolved
  to NULL (so the z-clear-skip heuristic mis-fired). Fixing address resolution
  correctly skips it, revealing the real (previously invisible) 3D geometry.
  Root causes found and fixed, in dependency order:
  1. ADDRESS RESOLUTION (the documented "menu black" blocker). DKR builds nearly
     every DL pointer with OS_K0_TO_PHYSICAL(p) / `(s32)p + K0BASE`, i.e. the
     token is `(u32)hostptr ^ 0x80000000` (± 0x80000000 mod 2^32 just toggles bit
     31). The old dkr_resolve never tried the flipped form, so segment bases,
     viewports, matrices, vertex/triangle/DMADL pointers ALL resolved to NULL and
     nothing 3D drew. New dkr_resolve (gfx_pc_dkr.c): registry FIRST (globals /
     rodata DLs — gViewportStack, the matrix stack, dMenuHudDrawModes, dRdpInit),
     then arena reconstruction of BOTH the flipped and raw low-32, then the
     segment table last. Registry-before-arena is essential: a global's low-32 can
     collide with the arena window and would otherwise hand back arena garbage
     (this collapsed the matrix stack to zeros). OS_K0_TO_PHYSICAL (os_convert.h)
     + gDma1p (gbi.h) now register only genuine >4GB non-arena host pointers
     (dkr_k0_to_physical / dkr_dl_register_host_ptr in stubs_dkr.c); registering a
     dkrptr32 TOKEN (e.g. TextureHeader.cmd) poisoned the registry, so that is
     excluded.
  2. gDma1p double-evaluation (a regression the registration change introduced):
     the macro used its `s` arg twice, so `OS_K0_TO_PHYSICAL((*mtx)++)` ran the
     `++` twice — the DL stored `*mtx + 1` while mtxf_to_mtx wrote `*mtx`, off by
     one Mtx. Fixed by evaluating `s` once into a temp.
  3. Mtx is `long[4][4]` — 128 bytes on LP64 vs 64 on N64. This desynced the
     matrix writer (mtxf_to_mtx addresses the fraction half at &m->m[2][0]), the
     gMatrixHeap/gTriangleHeap pool layout and the (*mtx)++ advance. Pinned
     Mtx_t's element to `int` under NATIVE_PORT (gbi.h) — 64 bytes on every width.
  4. TEXTURES never materialised (everything sampled the GL zero-texture = black).
     load_texture() uses asset_load() — the RAW-memcpy path that does NOT run
     asset_swap_normalize (only asset_table_load swaps). So the big-endian texture
     header's `numOfTextures` u16 read 0x0001 instead of 0x0100, `>>8` gave 0, and
     material_init NEVER ran. Fixed: read the frame count as the BE high byte and
     byteswap each per-frame TextureHeader in load_texture (new
     asset_swap.c swap_texture_header; texels stay BE, decoded by the F3DDKR
     texture path). Also fixed load_texture's `(s32) tex` pointer truncations that
     built the material lists at a bogus address.
  5. align16() (memory.c) did `(s32) address` — truncating any 64-bit pointer with
     a non-zero low nibble. This is what actually crashed material_init once it
     started running (wild write). Fixed to align through uintptr_t. General bug;
     also affected the sprite allocators.
  6. amCreateAudioMgr (audiomgr.c) wrote one audioMgrConfig past the single
     `audConfig` stack struct — a harmless OOB scribble on N64, a real
     stack-buffer-overflow on LP64 (ASan-flagged). Dropped (dead in stubbed audio).
  Renderer robustness: dkr_resolve arena-bounds clamps on every bulk read
  (vertex/triangle/matrix batches, texture decode rows, sub-DL command fetch) so a
  mis-decoded or edge-of-arena pointer can never read past the 16 MB arena.
  Diagnostics: MDKR_TRACE=2 now emits an opcode-level DL trace (per command name +
  key params, matrix/vertex transforms, combiner/shader/texture-bind results,
  per-frame emitted/on-screen triangle counts); optional MDKR_DL_FRAME=N filter.
  A SIGSEGV/SIGBUS backtrace handler (main_pc.c) makes layout-dependent faults
  diagnosable (MDKR_NO_CRASH_HANDLER=1 to defer to ASan).
  RESULT: intro (frames ~20-150) draws textured, coloured geometry at the correct
  centred screen position (110/300 dumped frames have content). REMAINING: (a) the
  menu-with-level-background frames (~170+) submit only background FILLRECTs and
  identity matrices in headless mode — no scene geometry is built without input, a
  game-state issue (M4 input), not a renderer bug; (b) intro sprites render as
  narrow textured bars — billboard/sprite orientation and per-triangle UV mapping
  want a closer look; (c) RGBA16 texels are left big-endian (decoder reads a byte
  pair) so 16-bit texture colours may be byte-swapped — verify vs a reference.

- M4 input + interactive menus (this wave) — the game now BOOTS INTO NAVIGABLE
  MENUS and advances on real input; scene geometry renders. 300 headless frames
  exit 0. Full boot path verified: MENU_BOOT(f14) -> MENU_LOGOS(f173) ->
  MENU_TITLE(f1134) -> [Press Start] -> MENU_CHARACTER_SELECT(3) or
  MENU_OPTIONS(12) depending on the navigation input.
  ROOT CAUSE of "menus never advance": joypad.c input_update gates its per-frame
  osContGetReadData behind a NON-BLOCKING recv on the SI queue; the stub
  osContStartReadData was a no-op that never posted an SI-completion message, so
  input was read exactly ONCE at boot and the game only ever saw neutral pads.
  Fixed by posting the SI completion synchronously (stubs_dkr.c) — the
  cooperative-model equivalent of the SI-DMA-done interrupt.
  Input path (platform/): platform_sdl_min.c captures SDL keyboard/controller
  transitions at each host opportunity into bounded per-button queues, then
  publishes one pad sample per fixed simulation ticket. Press+release between
  ticks is stretched across two DKR samples; analog uses the latest sample;
  disconnect/overlay capture neutralizes safely. Browser touch uses the same
  contract through a bounded JS snapshot queue, while optional --input-script
  entries (`frame TOKEN[+TOKEN] [hold]`) remain tick-indexed and bypass host
  poll frequency. osContGetReadData reads only the published tick state. Default (no
  --headless-frames) opens a visible vsync-paced window; --headless-frames /
  --dump-frames unchanged; MDKR_TRACE>=3 traces input reads; menu_init traces
  the menu-id timeline.
  Level-load / gameplay-init LP64 + asset crashes cleared on the title demo path
  (each only reachable once input advanced the game): waves_alloc (u32-truncated
  sub-pointers), func_8002F2AC (sizeof-as-element-stride), the level-model
  BSP-tree node array (was never byte-swapped -> unbounded recursion / stack
  overflow; count = numberOfSegments-1), render_level_segment's //!@bug
  uninitialised-batchInfo pre-read, and the BE-audio-bank deref reached by
  demo-level spatial sfx (no-op'd the play path until M5). Menu/font text:
  load_menu_text (4-byte BE offset table overlaid on an 8-byte char** array) and
  FontData (ROM-overlaid struct whose TextureHeader* array inflated the struct on
  LP64) both rebuilt for LP64 (dedicated blob buffer / dkrptr32 + asserts).
  Renderer: 16-bit texels + TLUT now decoded big-endian (RGBA16/IA16 colour fix)
  — sand/water/sky decode correctly.
  VISUAL STATE (dumped frames, read to confirm): the title screen shows the 3D demo
  level (beach/water) + the DKR logo + a two-row Start/Options menu; scripting
  Start selects, DOWN then Start reaches OPTIONS instead of character select
  (menuId 12 vs 3) — DEFINITIVE proof the selection responds to input. The
  OPTIONS menu draws its full vertical item list; character select loads.
  REMAINING (see OPEN_ITEMS "M4 render state"): menu TEXT and the DKR logo render
  as solid blocks / narrow bars — glyphs are RGBA32 TEXRECTs sampling a wide
  atlas (SETTILE fmt=0 siz=3), drawn solid because the atlas UV (S10.5) or the
  DKR text combiner/alpha (cc_id 00ef92c15ef92c15, othermode_l 0x00504240) path
  in gfx_pc_dkr.c drops the glyph's texel; sprites/billboards still draw as bars.
  Menu STRUCTURE + navigation are correct; glyph legibility is the open cosmetic.

- M3c renderer wave (this wave) — MENU TEXT is now LEGIBLE and the DKR LOGO renders
  CORRECTLY. 300 headless frames exit 0. All verified by reading dumped frames:
  * Title screen: full "DIDDY KONG RACING" logo (red DIDDY KONG + star, blue/green
    RACING, TM) over a fully-textured beach/sky/water scene; START/OPTIONS legible.
  * OPTIONS menu: OPTIONS / ENGLISH / SUBTITLES ON / AUDIO OPTIONS / SAVE OPTIONS /
    MAGIC CODES / RETURN — every item legible. PLAYER SELECT title legible.
  Three stacked root causes fixed in platform/fast3d/gfx_pc_dkr.c (all F3DDKR
  semantics, now documented there + in docs/OPEN_ITEMS.md):
    1. gSPTexture s/t scale is ALWAYS 0 in DKR — the game's Triangle/TEXRECT UVs
       are absolute S10.5 texel coords and the microcode never uses the scale as a
       multiplier. The F3DEX-style `tc*scale>>16` multiplied by 0, collapsing every
       texcoord to (0,0): all textured primitives sampled texel 0 (solid text,
       solid sprites, flat-shaded terrain). Treat scale 0 as unity. THE big one.
    2. 32-bit texture row stride was half — G_IM_SIZ_32b_LINE_BYTES==2 (RG/BA TMEM
       bank split), so the tile `line` counts only 2 of 4 bytes/texel; the plain
       contiguous RGBA32 arena source needs line*2. Half-stride garbled the RGBA32
       font atlas + logo strips into blocks/bands. Double line_bytes for 32-bit.
    3. The non-perspective (G_TP_NONE) *0.5 texcoord halving (kept for mgb64
       parity; models the RSP's fixed scale for perspective-off GEOMETRY texcoords)
       was wrongly applied to TEXRECTs (absolute RDP coords). DKR draws all text +
       the logo as G_TP_NONE texrects → 2x zoom → mangled. Skip *0.5 in texrects.
  RESOLVED (fidelity: billboard red-spikes wave): the billboard sprites that
  stretched to red spikes/bars (in-race player + AI karts, the faint title bar,
  the tall green PLAYER SELECT bars) are FIXED. The corrupt "local z≈-17488 +
  non-white rgba" verts were NOT game data — they were Gfx DISPLAY-LIST command
  bytes read as a Vertex, because the sprite vertex buffer had been over-run by the
  DL region. ROOT CAUSE: `Gsetcolor.color` in PR/gbi.h is N64 `unsigned long`
  (32-bit) → 8 bytes on LP64 with 8-byte alignment, which alone inflated
  `sizeof(Gfx)` from 8 to 16. `tex_load_sprite` reserves its sprite DL region with a
  LITERAL 8-byte Gfx stride (`numTextures*0x20`) but the writer advances by the real
  `sizeof(Gfx)`=16, so multi-tile sprites' DL commands over-ran into the following
  vertex region; the HLE then decoded a gDkrDmaDisplayList packet as a vertex (its
  texture-cmd-pointer bytes = huge z), flinging one billboard-quad corner off-screen
  → a thin spike. FIX: pin `Gsetcolor.color` to 32-bit under NATIVE_PORT (mirrors the
  Mtx_t LP64 fix) so `sizeof(Gfx)==8` and the literal-vs-sizeof layout math agrees.
  VERIFIED by pixels (race frame 2848: player kart + AI karts + item boxes + "GO!"
  render correctly, was red spikes), a raw-vertex/sprite-loader probe (DL pointer
  reached the exact vertex-region start; sizeof(Gfx)=16 confirmed then 8 after fix),
  0 billboard stretched-tris across race/title/menus, and the Ares oracle (real ROM
  shows the same characters as textured sprites; race_karts route). Regression: race
  20× + title 300f 5× + char-select/options/game-select/magic-codes = 0 crashes; DKR
  logo + menu text unchanged. RESIDUAL (separate, non-billboard): the headless
  PLAYER SELECT scene still shows a compact green shape — a `bb=0 slot=1` object
  whose verts straddle the near plane in the degenerate all-black headless scene
  (needs real near-plane clipping in the HLE, deferred; does not spike in real race
  gameplay). See OPEN_ITEMS + gfx_pc_dkr.c "LP64 STRUCT-SIZE LOCKS".

- M4-fix wave (crash) — the intermittent ASLR-dependent char-select SIGSEGV in
  dkr_sp_vertex is FIXED and ROOT-CAUSED. It was a single LP64 pointer truncation
  in render_level_segment (game/src/tracks.c): the per-batch vertex/triangle
  addresses were held in s32 locals and assigned `(s32) &DKR_PTR(Vertex,...)[off]`;
  on the 64-bit host that truncates the reconstructed arena pointer and, being
  SIGNED, sign-extends any pointer whose low-32 has bit 31 set (ASLR-dependent) to a
  wild 0xffffffff.. value. That value was fed straight to OS_K0_TO_PHYSICAL, which
  registered it (>4 GB, outside the arena), so dkr_resolve later handed the wild
  pointer back to dkr_sp_vertex -> fault on an unmapped page. Fix: hold the real
  Vertex*/Triangle* under NATIVE_PORT (matching every other gSPVertexDKR site) so
  OS_K0_TO_PHYSICAL tokenizes the arena address correctly. Belt-and-suspenders
  hardening in gfx_ptr.h + gfx_pc_dkr.c ensures a sign-extended pointer can never be
  stored, resolved, or dereferenced regardless of input. VERIFIED: char-select
  1500f / options 1500f / title 300f each 0 crashes over 20 runs (was 13/20 on
  char-select), ASan clean, char-select capture still renders "PLAYER SELECT" + scene
  geometry (no geometry dropped). See docs/OPEN_ITEMS.md (M4-fix) for detail.

- M6 playable race (this wave) — the game now DRIVES INTO AN ACTUAL RACE with a
  HUMAN-CONTROLLED racer that accelerates, steers, and MOVES through the track,
  which renders with a working HUD. This is the M6 "playable proof of work."
  Route taken = Time Trial / Tracks mode (recommended target): TITLE -> (2 taps)
  CHARACTER_SELECT(3) -> [A confirm, A finalize] -> CAUTION(28) -> [A] ->
  GAME_SELECT(19) -> [DOWN, A pick Tracks] -> TRACK_SELECT(15) -> [A world,
  A vehicle, A TT-off, A GO] -> level_load(levelId=5, numPlayers=0) = the real
  race (first Dino Domain track, Ancient Lake). Fixtures committed:
  tests/input_scripts/nav_to_time_trial_race.txt (reaches in-race),
  race_drive_time_trial.txt (nav + held throttle + L/R steer weave).
  VERIFIED (dumped frames read + numeric racer trace): the player racer grounds
  (groundedWheels=4), gains velocity, and its world position advances (z steps
  -6413 -> -6493 ...) while steering shifts x with the stick; the follow camera
  scrolls Ancient Lake (blue sky, palm trees, orange canyon, red/white start
  line + START gate) past the view; HUD shows position / LAP 1/3 / banana count /
  a running TIME. Robustness: 0 crashes over 20x the drive script (4600f each) +
  5x title 300f; nav_to_character_select / nav_to_options fixtures still pass; no
  new build warnings.
  ELEVEN root-caused LP64 / big-endian bugs fixed on the level-load->race path,
  in the order the run hit them (all NATIVE_PORT-gated; file:line):
   1. thread30_bgload.c bgload_tick — thread30 is a no-op in the cooperative
      model, so bgload's OS_MESG_BLOCK recv never cleared gThread30NeedToLoadLevel;
      bgload_active() stayed TRUE forever and the tracks menu froze gMenuDelay.
      Fix: do the "background" load synchronously when the delay expires (native
      DMA == memcpy) — the cooperative equivalent of thread30's loop body.
   2. menu.c func_8007FFEC (~2189) — the wood-panel geometry pointers
      (gWoodPanelTriangles/gMenuGeometry/gWoodPanelVertices) were laid out with
      (u32) casts truncating real >4GB arena pointers -> wild per-panel writes on
      GAME_SELECT init. Fixed via uintptr_t arithmetic.
   3. menu.c trackmenu_setup_render (~9245) — `i = (s32) level_name(id); draw_text(
      (char*)i)` truncated the track-name host char* -> fault in render_text_string.
      Keep the full pointer.
   4. obj_animate.c (163) — model->animatedVertexIndices (ObjectModel +0x4C) is a
      dkrptr32 slot read raw as (s16*); truncated -> wild indices[] read rendering
      the preview level. Use DKR_PTR like model->vertices/animations.
   5. objects.c obj_spawn_attachment (~2488) — attachment objects sized with the
      N64 `numModelIds*4 + 0x80`; on LP64 sizeof(Object) and the pointer array are
      bigger, so the modelInstances array at &object[1] spilled into the next
      object (its 0x68 union then read as garbage -> render_3d_model fault). Size
      from sizeof(Object) + numModelIds*sizeof(void*).
   6. collision.c generate/resolve_collisions + tracks.c camera-clip — the
      collision-candidate list packed a segment as a POSITIVE physical addr and a
      facet as a NEGATIVE K0 pointer into one s32, dispatching by sign. Neither
      survives LP64 (arena > 4GB; a truncated pointer's bit-31 is ASLR-dependent).
      New packing (collision.h DKR_COLL_*): 24-bit ARENA OFFSET + bit-31 type tag,
      reconstructed against the arena base — keeps the sign-based dispatch working.
   7. racer.c func_80053750 (~6203) — a fakematch dead read `someObj->y_position`
      dereferenced an uninitialised local on the first race frame. Skipped under
      AVOID_UB (result was discarded).
   8. tracks.c void_init (~496) — `(s32)(ptr + ...)` truncated+sign-extended the
      void-mesh arena pointer -> every gVoidVerts/Tris was 0xffffffff..; void_check
      handed it to gTrackVtxPtr and void_generate_primitive faulted. uintptr_t.
   9. game_ui.c hud_init (~340) — the HUD stale-counter array was placed
      `count*sizeof(s32)` (N64 ptr size) past gAssetHudElements, but entry[] holds
      real void* (8B) on LP64, so the counters overlapped entry[] and incrementing
      them corrupted the HUD element pointers -> wild sprite_free. Step past the
      real pointer-array width.
  10. game_ui.h HudDrawTexture.unk8 — the DrawTexture[] scan in texrect_draw* is
      NULL-terminated by a sentinel entry whose `texture` field must read zero. On
      LP64 DrawTexture grew 8->16B (its pointer 4->8), so the 4-byte s32 unk8 only
      covered HALF the 8-byte sentinel texture; the upper half was uninitialised
      stack -> ASLR-dependent wild `tex` in texrect_draw_scaled (2/20 runs).
      Widened unk8 to pointer width.
  11. asset_swap.c swap_level_model + objects.c/racer.c misc floats — THE movement
      bug. (a) The per-triangle collision facets (CollisionFacetPlanes: 4x u16
      basePlaneIndex/edgeBisectorPlane) were never byte-swapped, so basePlaneIndex
      read 0x4200 instead of 0x0042 -> track_init_collision built nan/garbage
      collision planes and resolve_collisions found no wheel-ground contact
      (groundedWheels stayed 0, throttle produced no motion). Swap the facet array
      (numberOfTriangles x 4 u16) in swap_level_model. (b) The per-vehicle wheel
      offset/radius + acceleration curves are big-endian f32 arrays in the
      heterogeneous ASSET_MISC blob (ObjectHeader.unk5C/unk5D, ASSET_MISC_RACER_*):
      read LE, -10.0f (0xC1200000) becomes a ~0 denormal, so wheels sat at the car
      centre with radius 0. Added dkr_misc_swap_words() (objects.c) — a dedup'd
      in-place word-swap of a named ASSET_MISC sub-asset — called at the racer f32
      fetch sites. With both, resolve_collisions grounds all 4 wheels and the car
      accelerates + steers.
  Diagnostics added (kept, low-noise, trace-gated): level_load() logs its args at
  MDKR_TRACE>=1 (like menu_init), so the in-race transition is greppable.

- robust: LP64 pointer-truncation crash-class SWEEP — eliminated the whole
  `(u32)/(s32)`-cast-of-a-pointer bug class (the class behind the user's
  func_8007FFEC segfault), not just the sites one boot path happened to hit. The
  build keeps `-Wint-to-pointer-cast -Wpointer-to-int-cast` VISIBLE (they survive
  `-Wno-everything`), so a clean build enumerated the class: 211 unique cast sites
  across 36 files. Each was read and bucketed:
    * BUG (truncate-then-DEREFERENCE) — the crash class. Fixed with `(uintptr_t)`
      (pointer-width on N64 and LP64; the warning also clears, which proves the
      swept site no longer truncates).
    * TOKEN (truncate-to-token, reconstructed later by osPiStartDma's
      dkr_lo32_to_ptr / dkr_resolve / DKR_PTR cache reads) — correct, left as-is;
      `(uintptr_t)` here would BREAK the DL/DMA path.
    * VALUE (pointer-diff size, low-nibble align, identity compare, printf) — fine.
  Case-1 fixes (all on reachable menu/game paths): objects.c racerfx_alloc boost
  geometry; finish-challenge `((Camera*)camera)->mode` (widened the s32 camera
  local); objFreeAssets/ObjSetupObject tex_free/free_3d_model; 15 sndp_stop/
  audspat SoundHandle/AudioPoint derefs (racer.c 10, objects.c 5,
  object_functions.c 2 — latent while audio was still silence-stubbed, live the moment a
  handle is non-NULL, so fixed now for M5); lights.c lights_init shade/dir buffers;
  memory.c pool-init `_ALIGN16(slots)`; video.h FBALIGN (also fixes a latent
  mempool_free-of-wild on video-mode change; the `(s32)fb` segment token is
  bit-identical before/after so rendering is unaffected); textures_sprites.c
  material_set_blinking_lights (Spaceport Alpha); menu.c bootscreen_init_cpak
  gBootPakData; menu.c cheatmenu_checksum `&__ROM_END` (native __ROM_END is a
  pointer VARIABLE, so `&__ROM_END` was garbage -> unbounded ROM-buffer OOB read;
  bounded to the real us.v80 offset 0x00B8CFD0). Dead-code case-1 documented and
  left as-is (no caller, cannot crash): func_8000E5EC/E79C/E558, trackMakeAbsolute,
  align8/align4, mainproc bzero (native boot bypasses mainproc).
  -Werror: left OFF. The ~256 remaining cast warnings are all case-2 token
  truncations (the `(u32)` IS the point) + case-3 value math; full
  `-Werror=int/pointer-cast` would require wrapping every one in a `DKR_TOKEN()`
  macro — a ~150-site edit across the working DL/asset path, exactly the regression
  risk this wave must avoid. Warnings stay visible so any NEW case-1 truncation is
  surfaced; live-path case-1 count is zero. (328 cast warnings before -> 256 after;
  the 72 removed are exactly the fixed case-1 sites, now pointer-width.)
  VALIDATION — 20x crash-loop over EVERY reachable menu + the race, before AND
  after: 0/20 crashes on all 11 paths both times. (Before is also 0 because the
  fixed sites are ASLR/state-dependent and these scripted inputs don't reproduce
  them run-to-run — the win is STRUCTURAL: no address layout or input can now
  trigger the truncations.) Paths: title, character_select, options,
  audio_options, save_options, magic_codes, game_select (the ORIGINAL func_8007FFEC
  screen), track_select, file_select (adventure), time_trial_nav, race_drive. New
  fixtures committed: nav_to_{audio_options,save_options,magic_codes,game_select,
  track_select,file_select_adventure}.txt. TROPHY has no dedicated fixture —
  reaching MENU_TROPHY_RACE_ROUND needs the TRACK_SELECT world-map cursor parked at
  the X=4 trophy column, timing/state-sensitive under scripted input; the
  level-load + in-race code it exercises is already covered by the time-trial
  fixtures, and its menu handler is a rankings-style screen with no cast sites.
- Historical frame-pacing milestone (superseded by the 2026-07-28 Bubbler
  containment above; headless now follows explicit resolved cadence and PAL
  uses its 50 Hz source clock) — the in-race SLOW MOTION is ROOT-CAUSED and
  FIXED. DKR normalises game speed against framerate in fb_update() (video.c:277):
  it drains the video message queue counting elapsed 60 Hz VI fields per rendered
  frame and returns that as `updateRate`, which scales movement, physics AND the
  race clock (lap_times[countLap] += updateRate, racer.c:4382). On N64 the VI
  interrupt posts those retraces asynchronously at 60 Hz; our cooperative shim had
  no async producer, so osRecvMesg's non-block drain always found 0 and updateRate
  pinned at LOGIC_60FPS=1 forever — the game scaled all motion for a 1/60 s
  timestep regardless of the real present cadence, so a sub-60fps race ran in slow
  motion (and a >60 Hz display would run fast). The race clock is NOT real-time
  based; it is updateRate-accumulated too, so it ran slow in lockstep with motion
  (it only "looked correct" because it has no visible reference and menus sustain
  60fps). FIX (modelled on mgb64's cooperative VI pacing): a wall-clock field
  pacer (platform/platform_sdl_min.c platform_vi_pace_measure) that paces each
  present to a 1/60 s floor (refresh-INDEPENDENT — caps high-refresh, drift-free
  accumulator) and measures the true elapsed-field count; the video-queue recv
  (platform/stubs_dkr.c, queue captured in osScAddClient OS_SC_ID_VIDEO) makes
  that many retraces available so fb_update's drain returns the correct updateRate
  (2 @ 30fps, ...). Deterministic in headless (fixed field count/frame, default 1
  → existing tests byte-identical); realtime for windowed. MEASURED (headless,
  scripted race drive, player-1 position+clock probe via mdkr_pace_probe_racer in
  racer.c): race-clock advance ÷ real-time = 1.000 (fix @60fps), 1.000 (fix
  @30fps, cadence-independent), 0.500 (bug @30fps = the slow-motion). No new
  warnings; title 300f exit 0 (all updateRate=1); nav_to_character_select /
  nav_to_options / nav_to_time_trial_race fixtures pass; race drive 5/5 clean.
  Env: MDKR_SYNTH_FIELDS=N, MDKR_PACE_REALTIME=1, MDKR_VI_PACE=off, MDKR_FIELD_HZ.
  Full detail: docs/OPEN_ITEMS.md "Frame pacing / slow-motion"; engine-difference
  vs mgb64's fixed-60 model: docs/MGB64_BACKFLOW.md.

- M5 audio (this wave) — the port now PRODUCES REAL SOUND. The stock libultra
  synthesizer runs natively; platform/mixer.h redefines abi.h's `a*` command
  macros to call the vendored software aspMain mixer (platform/mixer.c) directly,
  so building the Acmd list IS the synthesis. Wired via a NATIVE_PORT
  `#include "mixer.h"` at the tail of PR/abi.h (guarantees the override wins in
  every synth TU regardless of include order). The audio thread never runs in the
  cooperative model, so an audio-time service replaces it: amAudioSynthFrame
  (audiomgr.c) reproduces __amHandleFrameMsg's core (__clearAudioDMA + alAudioFrame
  into an arena PCM buffer) and dkr_audio_service_tick (platform/audi_port_dkr.c)
  consumes independently due quanta after ordered game ticks. Host time is credited
  at real presentation boundaries, so extra presentation never manufactures PCM.
  Host out = SDL2
  queue-mode device @22050 Hz stereo s16 (audi_port_dkr.c; osAi* moved there from
  stubs_dkr.c), skipped under --headless-frames (synthesis still runs for CI);
  MDKR_AUDIO_DUMP=out.wav / MDKR_AUDIO_RMS=1 capture the PCM for validation.
  ASSET_AUDIO un-stubbed (the deferred byteswap, CLOSED): bnkf.c rewritten as a
  big-endian, LP64-safe parser that reads the raw BE bank/seqfile through explicit
  offsets and builds fresh arena-resident host structs (the mgb64 audio_compat.c
  model — the overlay-and-patch alBnkfNew can't work on LP64 where the embedded
  4-byte on-disk pointer slots are 8 bytes); real sequence table + ALCMidiHdr
  header swap (music_sequence_init); SoundData u16 swap; sndp_play / sound_count
  un-stubbed. THE core problem was addressing: the stock synth's whole address ABI
  is 32-bit (osVirtualToPhysical→u32, ALDMAproc return→s32, ALSave.dramout→s32,
  K0_TO_PHYS→29-bit mask), so every address reaches the mixer truncated on LP64 —
  fixed by making ALL audio memory arena-resident (audio heap arena-backed, banks
  parsed into the arena, output/cmd buffers from the arena) and reconstructing each
  mixer address from its low-32 via dkr_lo32_to_ptr (mixer.h MIXER_RESOLVE); the
  K0_TO_PHYS `& 0x1FFFFFFF` mask is dropped for the audio path (load.c) since it
  stripped bits the reconstruction needs. Also fixed an LP64 free-list overrun:
  __allocParam hands out sizeof(ALParam) blocks reinterpreted as ALStartParamAlt
  (40B vs 32B here) — the write overran into the next pooled param's `next`; ALParam
  padded + _Static_assert-locked (synthInternals.h).
  VERIFIED objectively (headless, MDKR_AUDIO_DUMP): race RMS ~6800 (per-second
  segments 3000-12000, dynamic real audio; peak full-scale), menu-nav RMS ~4300,
  boot-logo frames correctly silent. ASan-clean over a menu+race synth run
  (build-asan). Robustness: race_drive 20x = 0 crashes, title 300f x5 = 0, all 9
  menu fixtures (character_select/options/game_select/track_select/time_trial/
  magic_codes/audio_options/save_options/file_select_adventure) exit 0; no new
  build warnings beyond the expected asset_load token-truncation class + the
  pre-existing linker alignment note.
  NATIVE REVERB is ON by default (MDKR_AUDIO_REVERB=0 disables, for A/B
  captures). It used to be off because the ALFx delay-line transfers overran
  into the adjacent pool allocations; root cause was NOT the struct growth but
  the u32 delay tap `&r->input[-d->input]` — an unsigned negation that wraps mod
  2^32 on the N64 and ZERO-EXTENDS on LP64, putting the tap ~8 GiB above the
  line, so both wrap-split lengths came out wrong and a 320-byte DMA became
  12800 bytes. Fixed in reverb.c (AL_FX_TAP), plus an 8-byte delay-line tail
  slack for the mixer's ROUND_UP_8 DMA granularity and an always-on bounds guard
  (`[AUDIO] fx-guard trips=N`, must be 0). Enabling reverb also removed a real
  gain-stage error — with the FX bypassed, the aux WET SEND was still summed to
  the main bus undelayed at unity, worth +2.5 dB of peak and 4.3x the master-bus
  clip events. See docs/OPEN_ITEMS.md "M5 open items" + docs/MGB64_BACKFLOW.md.

- M4.5 WebGPU backend (this wave) — WebGPU became the default native render
  backend at this checkpoint (the later native throughput qualification restored
  GL as the default), and it is a proven visual match to GL.
  This is the bridge to the in-browser build (M8): the SAME gfx_webgpu.c compiles
  for wasm (Emscripten supplies WebGPU; only native links wgpu-native).
  VENDORED verbatim from mgb64/src/platform/fast3d/ into platform/fast3d/:
  gfx_webgpu.c (205KB), gfx_webgpu.h, gfx_webgpu_shader.c (runtime WGSL emitter),
  gfx_webgpu_shader.h, gfx_webgpu_compat.h (the native/emscripten dialect seam).
  It drives the SAME GfxRenderingAPI vtable our F3DDKR front-end (gfx_pc_dkr.c)
  already uses, so the switch is `gfx_init(&gfx_webgpu_api)` — ZERO front-end
  change. The shader generator consumes the same gfx_cc.c combiner output as GL.
  ADAPTATION was minimal (the file stays byte-identical to mgb64 for M8 backflow):
  the mgb64-shell symbols it references are satisfied without source edits to the
  vendored .c. As of the native app shell (MDKR_APP, platform/app/), two of those
  symbol groups are now REAL rather than inert: the app-shell WebGPU-handoff
  getters are backed by platform/host_window.c (the launcher hands its device and
  surface to the engine, so both render into one window) and the ImGui overlay
  hooks by platform/app_overlay_hooks.c (the in-game F1 overlay). The stub
  definitions for those two groups are #ifndef MDKR_APP'd out of
  platform/gfx_webgpu_stubs.c, so exactly one definition of each reaches the
  link. With -DMDKR_APP=OFF (and in the wasm build, where the JS shell is the
  launcher) the inert versions are compiled instead and behavior is unchanged.
  platform/gfx_webgpu_stubs.c still provides unconditionally: the minimap overlay
  (minimap_overlay_draw_queued_frames_webgpu -> no-op; DKR draws its own in-game
  map, so there is no shell-drawn minimap to enable), savedirPath (prewarm is
  deferred per the M4.5 plan — DKR never calls gfx_webgpu_set_stage, so the
  pipeline-prewarm cache stays dormant and writes nothing), g_deterministic=0,
  and the screenshot-session globals wgpu_readback_possible reads. port_env.c is
  added to the build for port_env_bool. All g_pc* post-FX uniforms + gfx_cc.h +
  gfx_uniforms.h are already present (byte-identical to mgb64) and shared with GL.
  DEPENDENCY (native): cmake/webgpu.cmake mirrors mgb64's — the pinned wgpu-native
  prebuilt v29.0.1.1, SHA-256 verified. On the maintainer's M3 Max it reuses mgb64's already-
  fetched prebuilt tree (offline-safe); otherwise FetchContent downloads it (or a
  local cached zip via file://). option(MDKR_WEBGPU_BACKEND ON) gates the code +
  define + link; GL stays compiled so MDKR_RENDERER switches at runtime. OFF
  builds a GL-only binary that links NO wgpu (verified: 0 wgpu deps) and falls
  back to GL if MDKR_RENDERER=webgpu is set.
  RUNTIME SELECTOR: MDKR_RENDERER=webgpu|gl|metal (default GL since the native
  uncapped throughput qualification; metal is not compiled in mdkr64, so it
  warns + falls back to GL). Resolved once
  (platform_sdl_min.c mdkr_render_backend()) BEFORE the window is created, because
  the window kind differs per backend: WebGPU uses an SDL_WINDOW_METAL window +
  SDL_Metal_CreateView (macOS CAMetalLayer -> WGPUSurface via
  platformGetMetalLayer), GL keeps the SDL_WINDOW_OPENGL + glad path. main_pc
  passes the chosen &gfx_*_api to gfx_init. WebGPU presents inside gfx_end_frame
  (wgpu_end_frame -> surface present), so platform_sdl_present is a no-op for it.
  HEADLESS CAPTURE: WebGPU renders the scene OFFSCREEN and reads it back through
  the vtable (gfx_read_framebuffer_rgb -> rapi->read_framebuffer_rgb, added to
  gfx_pc_dkr.c) — works for a hidden window (no drawable needed), so
  --dump-frames captures WebGPU identically to GL's glReadPixels (both bottom-left
  RGB; shared PPM row-flip).
  PARITY vs GL (acceptance bar — identical scenes, both backends, dumped frames read
  by eye + scored with tools/compare_frames.py's oracle metrics on matching frame
  pairs; scores exclude GL's double-buffer black captures, see below):
    * boot logo:  hist 0.994, block 1.000  — DKR "DIDDY KONG RACING" logo pixel-
      identical (red DIDDY KONG + star, blue/green RACING, TM).
    * title + OPTIONS: hist 0.988, block 0.999 — the Ancient Lake attract (beach/
      water/sky) matches; the OPTIONS menu (OPTIONS/ENGLISH/SUBTITLES ON/AUDIO
      OPTIONS/SAVE OPTIONS/MAGIC CODES/RETURN) is PIXEL-IDENTICAL, every item
      legible, same colours + positions.
    * race: hist 0.995, block 0.999 across 300 frames — the kart-with-shadow race
      scene (Ancient Lake: orange canyon, palms, water, bananas, start-line
      barrier, player kart + AI kart) matches; HUD "5TH / LAP 1/3 / x0 / TIME
      00:03:18" identical and the TIME is frame-SYNCED between backends (proves
      pacing + billboard + audio-frame fixes intact under WebGPU).
    Differences are minor filtering only (hist ~0.99). One headless-only cosmetic
    delta: the panning attract demo shows a thin gold near-plane billboard SPIKE
    under GL that WebGPU clips away (the known bb near-plane straddle, OPEN_ITEMS
    "M4 render state"); WebGPU is the cleaner of the two and it never occurs in
    real gameplay.
    A GL CAPTURE ARTIFACT surfaced (not a WebGPU issue): during phases where the
    game submits a gfxtask only every OTHER present (the boot fade), GL's
    glReadPixels(GL_BACK) alternates content/black because the double-buffer swap
    flips the back buffer, so odd captures read black. WebGPU reads a PERSISTENT
    offscreen target, so it captures the held frame every present (more faithful
    to the real display). Parity scoring skips GL's black captures.
  ROBUSTNESS: race_drive 20x under WebGPU = 0 crashes; title 300f x5 = 0 crashes;
  GL fallback still boots; default (no env) selects + initializes WebGPU (Apple
  M3 Max, wgpu-native Metal). No new build warnings (the vendored + new TUs add 0;
  the 251 total are the pre-existing decomp token-truncation class). GL-only
  (MDKR_WEBGPU_BACKEND=OFF) build compiles + links clean, 0 wgpu deps.
  REMAINING RISK for M8 (browser): the offscreen-render + readback path and the
  WGPU_COMPAT_WAIT seam are exercised natively here; under Emscripten they need
  Asyncify (async device/adapter + buffer-map), which is the M8 task. See
  docs/architecture/web.md (de-risking notes) + docs/MGB64_BACKFLOW.md.

- M8 browser (wasm) build — the END-GOAL infrastructure is DONE and PROVEN: the
  game COMPILES + LINKS for wasm32, BOOTS in a real browser, brings up WebGPU, and
  runs the full cooperative game loop at 60fps via the Asyncify/requestAnimationFrame
  frame boundary — user supplies their own ROM. (Rendering fidelity was PARTIAL in
  this first wave — attract sky white; CLOSED in the follow-up wave below, the game
  now renders title/menus/race correctly in-browser.)
  VERIFIED in headless Chrome 150 (Apple Metal-3, --headless=new
  --enable-unsafe-webgpu) driving the actual mdkr64_web.wasm with the local ROM
  injected into MEMFS via a CDP harness: ROM load (12 MB), WebGPU device init
  (surface format 23, maxTextureDimension2D=16384), AudioWorklet sink active,
  "entering boot path", 16 MB arena, and the rAF loop advancing steadily (frame
  counter climbs at ~60/s, dtms~16.7, updateRate R=1) — no crash/abort/exit.
  BUILD (CMakeLists.txt EMSCRIPTEN branch, mirrors mgb64 ge007_web): mdkr64_web
  target, same engine sources as native minus GL (gfx_opengl.c/fast3d_gl_shim.c/
  platform_stdio.c/glad gated out; WebGPU-only in browser) plus web_audio_worklet.c;
  -sUSE_SDL=2; WebGPU via the webgpu INTERFACE target (--use-port=emdawnwebgpu);
  -sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT -sASYNCIFY_ADD=[main,gfx_init,wgpu_init,
  gfx_webgpu_bringup,osRecvMesg,platform_vi_pace_measure,thread3_main,
  main_game_loop,fb_update,eeprom_store]; -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=128M
  -sMODULARIZE -sEXPORT_NAME=createMDKR64 -sINVOKE_RUN=0 -lidbfs.js. Emits
  mdkr64_web.{js,wasm}; dist/web/ shell (index.html + mdkr64-shell.js + style.css)
  loads it. ASYNCIFY spine confirmed with -sASYNCIFY_ADVISE: main owns the
  gfx_init->rapi->init INDIRECT bring-up edge (gfx_init inlines into main at -O2)
  AND the per-frame stack; wgpu_init is the requestAdapter/requestDevice leaf;
  osRecvMesg is the rAF leaf. Both suspends unwind+rewind cleanly (no "unreachable").
  PLATFORM WEB ADAPTATIONS: platform_vi_pace_measure suspends to rAF (EM_ASYNC_JS
  platformWaitAnimationFrame) instead of nanosleep, KEEPING the VI-field updateRate
  pacing (field count from real rAF-timed wall clock — slow-motion fix intact);
  plain SDL window bound to #canvas (surface from the "#canvas" selector);
  web_audio_worklet.c (vendored+adapted from mgb64) drains audio off the main
  thread, fed the SAME mixer buffers as native at the osAiSetNextBuffer leaf, SDL
  fallback; --dump-frames + native crash-handler/system() gated off on web.
  ROM+SAVES: dist/web shell WebGPU-feature-detects, file-picks the .z64 (size+header
  validated) into MEMFS/IDBFS at /rom, IDBFS at /save with FS.syncfs after each
  EEPROM write (coalesced) + timer + pagehide; ROM persists across reload. Binaries
  gitignored; only the shell committed.
  TWO wasm32-specific bugs found + fixed (exactly the architecture decision 8 watch-item: layout/
  address assumptions that only held on LP64, re-surfaced at 32-bit pointer width):
   1. ARENA/SEGMENT-TOKEN COLLISION (the render blocker). On LP64 the 16 MB arena
      sits at a high 64-bit address whose low-32 never collides with DKR's N64
      segment tokens (0x0N000000); on wasm32 it landed at 0x02000000, INSIDE the
      segment space, so dkr_resolve's arena reconstruction swallowed the
      framebuffer/z-buffer segment tokens (SETCIMG 0x01000000->NULL) and no
      geometry resolved. FIX (stubs_dkr.c dkr_arena_init, web-only): posix_memalign
      the arena above the 256 MB segment ceiling (0x10000000) so segment tokens
      always reach the segment table and arena addresses never look like a token.
      After it, geometry emits + resolves (G_TRIN addr=90153d20->0x10153d20).
   2. Z-CLEAR FILLRECT DRAWN WHITE (gfx_pc_dkr.c). The depth-clear skip compared
      RESOLVED SETCIMG/SETZIMG pointers with a !=NULL guard; on wasm32 those
      segment tokens resolve to NULL (not registered like a >4GB LP64 host
      pointer), so the guard failed and the z-clear drew as an opaque white fill
      (the M3b symptom). FIX: compare the RAW tokens (0x01000000 vs 0x02000000) —
      width-independent, identical on native (re-verified: attract title renders,
      race fixture exits 0).
  REMAINING for a shippable web v1 (renderer fidelity on wasm32, same class as the
  native M3b/M4/M3c bring-up work, NOT done): the attract SKY renders white (a
  near-white full-screen fill / untextured-or-mis-shaded sky) while the water
  renders correctly — needs the wasm32 texture-materialisation / shading pass.
  The web build boots + runs but is NOT yet a pixel-correct render. Docs:
  docs/architecture/web.md (full status), docs/OPEN_ITEMS.md "M8 web render fidelity".

- M8 web render fidelity — CLOSED. The browser build now RENDERS the title/attract,
  menus, and a race CORRECTLY (was: boots + runs but white sky / menus-not-drawn).
  ROOT CAUSE (the gfx_ptr registry gap, confirmed in-browser via a CDP
  diagnostic): a non-arena host pointer to a game global / rodata display list is a
  >4GB address on LP64 that dkr_k0_to_physical registers, so dkr_resolve recovers it
  from the registry; on wasm32 every pointer is <4GB so the `>0xFFFFFFFF` threshold
  never fires, the globals never register, and dkr_resolve returned NULL for them ->
  all geometry living in globals/rodata DLs (sky gradient, menu text, kart) failed to
  draw. Registration alone can't fix it — globals also reach DLs via `(s32)ptr+K0BASE`
  / raw casts that never pass through the registration functions (diag showed globals
  at ~0x18330 referenced as flipped `0x800183xx` and raw `0x000183xx`, all NULL).
  FIX (gfx_pc_dkr.c dkr_resolve, #ifdef __EMSCRIPTEN__): recover host pointers DIRECTLY
  from the token — on ILP32 the OS_K0_TO_PHYSICAL flip is reversible, so a bit-31-set
  token XORs back to the real address and a raw value < 0x01000000 is a low
  global/rodata (segment-0 base is 0); segment tokens 0x01000000..0x0FFFFFFF still fall
  to the segment table. The registry/arena checks run first (unchanged), so this only
  catches what LP64 recovered via the registry. dkr_k0_to_physical / dkr_dl_register_
  host_ptr reverted to their original LP64-only `>0xFFFFFFFF` registration (native
  byte-identical); dkr_resolve's new branch is __EMSCRIPTEN__-gated. VERIFIED IN A REAL
  BROWSER (headless Chrome 150 / Apple Metal-3, actual mdkr64_web.wasm, ROM in MEMFS,
  input driven via --input-script in FS) and scored vs native --dump-frames (128x96
  luma, 32-bin hist intersection + 16x12 block correlation):
    * attract (Ancient Lake: blue sky + clouds + palms + green shore + water):
      hist=0.996 block=1.000 — structurally identical to native.
    * OPTIONS menu (OPTIONS / ENGLISH / SUBTITLES ON / AUDIO OPTIONS / SAVE OPTIONS,
      all legible, correct gradient colours): hist=0.996 block=1.000 — identical.
    * in-race (Ancient Lake track, orange canyon, palms, road + bananas, kart, HUD
      "5TH / LAP 1/3 / x0 / TIME 00:01:60"): renders correctly (eyeballed; the race
      comparator scores noisily since it is a dynamic scene at unsynced web/native
      frames — the block metric needs matched camera/kart pose).
  NATIVE REGRESSION (the only native-affecting change is the earlier raw-token z-clear
  compare): 20x race_drive = 0 crashes; WebGPU renders the attract title (frame 1378
  0.5% white) + OPTIONS (frame 1532 91% black text-on-black); GL fallback boots
  (GL 4.1 Metal). No new warnings. FOLLOW-UP: the isolated-profile
  `check_browser_runtime.py` gate now runs 3600 paced frames through the in-race
  pipeline load, fails on GPU-process/device-loss markers or stalled/flat output,
  and has not reproduced the earlier exit-code-15 failure. Detail:
  docs/OPEN_ITEMS.md "M8 web (wasm) build".

## menufi — menu 1:1 fidelity: the frontend Timber's Island background RENDERS

**The headline gap is closed.** The frontend title menu used to draw the DIDDY KONG
RACING logo + START/OPTIONS over a flat blue sky/ocean; it now draws them over the
real Timber's Island beach — sand, leaning palm trees, tropical foliage, the
animated character and its ground shadow — matching the real ROM captured through
the ares oracle.

**Root cause (one bug, found with the oracle + targeted instrumentation).** The
object map's per-entry *body* params were never byteswapped. `swap_level_object_map()`
in platform/asset_swap.c swaps only the common x/y/z and explicitly punts everything
at >= +0x08, because those fields are polymorphic per object behaviour. The frontend
title (level 23, RACETYPE_CUTSCENE_1, CUTSCENE_ID_NONE) has no racers and no cutscene
camera — its camera comes solely from a BHV_CAMERA_ANIMATION object spawned indirectly
by a BHV_ANIMATION director through `LevelObjectEntry_Animation.objectIdToSpawn`
(s16 @ +0x0C). Big-endian, that read as garbage (0xB200 = -19968 instead of 0xB2 = 178),
so the AnimCamera never spawned, `obj_loop_animcamera()` never ran, and the camera
stayed at the `cam_init()` default (200,200,200) — pointing at open ocean. The island
and all 244 map objects had been loading correctly all along; only the camera was wrong.

**Fix** (game/src/objects.c): `mdkr_objmap_swap_bodies()` walks the inflated object
map in `track_spawn_objects()` before the spawn loop, resolves each entry's behaviour
the same way `spawn_object()` does (translation table -> object header, cached), and
byteswaps that behaviour's 16-bit body fields — bounded by the entry's real stride so
short entries are never over-read. Covers RACER, AUDIO, AUDIO_LINE(+_2), FOG_CHANGER,
TEXTURE_SCROLL, LIGHT_RGBA, WEATHER, LENS_FLARE, LENS_FLARE_SWITCH, CHARACTER_FLAG and
ANIMATION. Also fixed (game/src/tracks.c): `func_8002F440` sized `sp90`/`sp80` to `[8]`
under NATIVE_PORT — the frustum clip yields up to 7 verts, and writing index 6 into the
N64 `[6]` arrays is a benign overrun there but trips `-fstack-protector` here.

**Verified.** Oracle `title_to_options` vs cached ares reference: `menu_shown`
57.7% -> 63.6%, `navigated` 49.5% -> 58.2% (the montage shows flat blue -> correct
beach; the residual is camera-flythrough phase between runners on a best-effort sync,
not a render gap). All 8 menu-nav fixtures pass with 0 crashes (options, character
select, game select, track select, audio/save options, magic codes, file select);
title 300f x3 = 0 crashes; no new warnings.

**Known regression (disclosed, tracked in docs/OPEN_ITEMS.md).** Making animation
targets actually spawn trips a pre-existing LP64 object-header pool corruption: after
the now-correctly-populated frontend, loading an in-race level crashes in
`racerfx_alloc()` -> `spawn_object()` on a freshly-loaded header that comes back all
zeros. `nav_to_time_trial_race` therefore fails; every menu fixture passes. Ruled out:
pool byte size (8x), pool slot count, blueprint size, gObjPtrList overflow, property-size
mismatch, and the new cache's header refcount balance (a resolve-only build does not
crash). ASan is blind to it (the pool is one host allocation). Gating the swap by level
type does not help — the corruption originates in the frontend itself.

## videoconf — presentation modes: Pure, Restored, Remastered

**M0 of the visual remaster program.**
The port now has a schema-driven video configuration system and three
presentation modes. This is the wave every later remaster wave depends on:
before it, `platform/gfx_config_shim.c` was 45 hardcoded constants with a
comment deferring configuration to "M3+".

**What shipped.** `platform/video_config.{c,h}` (pure core: schema, defaults,
presets, precedence, resolution), `platform/video_config_runtime.c` (ini, env,
argv, publication), `platform/config_ini.{c,h}` (game-agnostic INI engine),
`tests/test_video_config.c` (ROM-free unit tests) and
`tests/check_video_presets.py` (ROM-backed gate).

```
--pure         authentic 4:3 framing, authored FOV, no enhancements
--restored     original art direction at modern fidelity
--remastered   the full remaster (default)

defaults < mdkr64.ini < mode preset < MDKR_* env < --video-set
```

**`--pure` is not `Video.Widescreen=0`, and that is the wave's main finding.**
The obvious reading of "pure mode has no widescreen" is wrong: `Widescreen=0`
engages the pre-widescreen compatibility path
([display_config.c:116-127](../platform/display_config.c#L116)) where presentation,
safe and fullbleed collapse onto the drawable and 4:3 content is **stretched**
to fill the window. Shipping that as the purist mode would look worse than the
original hardware. Pure keeps the display policy engaged and pins
`Video.Aspect=4:3`, so `mdkr_centered_fit` pillarboxes an undistorted image and
Hor+ against a presentation aspect equal to the authored one yields exactly the
authored FOV. `--legacy-stretch` remains available as a separate compatibility
flag; it is not a mode.

Measured on a 2560×1440 drawable:

```
--pure            presentation=1920x1440+320+0  aspect=1.33333   (pillarboxed)
--restored        presentation=2560x1440+0+0    aspect=1.77778   (fills)
--legacy-stretch  presentation=2560x1440+0+0    aspect=1.33333   (stretched)
```

**Precedence is a property, not an ordering accident.** `MdkrVideoSource` is a
monotonic rank and `mdkr_video_config_set()` refuses a write from a lower-ranked
source, so "env beats preset beats file" is asserted directly by a unit test
rather than depending on the order the layers happen to be applied.
`mdkr_video_config_resolve()` takes the ini entries, an env-lookup function
pointer and argv, so every rule is exercised with no window, no GPU and no ROM.

**Verified.**

```
$ ./build/mdkr_video_config_test
all video_config tests passed

$ python3 tests/check_video_presets.py --build build --rom baserom.us.v80.z64
PASS video presets: 3400 [PACE] rows identical across pure/restored/remastered;
Pure pillarboxes 4:3; precedence ladder resolves; positive control diverges
```

The positive control is an `MDKR_RNGSEED=legacy` arm: it perturbs the boot seeds
and must diverge, which it does at row 3157 on a sub-unit position delta
(`x=-1682.5` vs `-1681.5`). Without it a comparator that had silently stopped
finding rows would read as a pass.

Full suite, run in isolation:

```
$ python3 tools/run_checks.py --build build --rom baserom.us.v80.z64 \
    --primary-only --skip-instrumented --skip-wasm
run_checks: PASS — all 28 tasks passed in 11m05s
```

No fixture was edited. `ctest` 3/3 (`display_config`, `endian_utils`,
`video_config`); `tools/check_no_rom.sh` PASS over 11 artifacts.

**Harness note, learned the hard way.** Two earlier runs reported
`race_finish_time` and `save_failsafe` failures that were **not** real:
`run_checks.py` is sequential *by design* because several checks create and
remove `save/eeprom.bin`, and a second suite had been started while the first
one's long tail (`vehicle_sweep`, ~3m30s) was still executing. The two suites
raced on that file. Both checks pass standalone, pass in a 21-check prefix in
the identical order, and pass in the isolated full run above. Run one suite at a
time per worktree.

**Two corrections found by building the plan rather than reading it.**
Publication must run *before* main's argument loop, because `--aspect` /
`--widescreen` / `--fov` are handled inside that loop and call display_config's
setters directly — publishing afterwards would discard them. And the impure half
moved to its own TU: adding `publish()` to `video_config.c` made the pure core
reference `g_pc*` and display_config symbols, which broke the ROM-free test's
link. The split is what keeps that seam real.

**Left open at this M0 checkpoint.** `Video.Mipmaps` and `Video.TexturePack`
resolved and reported but drove nothing yet. Mipmaps are implemented in the M1
checkpoint below; texture-pack loading remains deferred. Restored and Remastered
still render identically today; the mode exists so M2's lighting work has a
home. The in-game options menu is M5.

## imagequality — mipmaps reach the GPU (M1, in progress)

**M1 checkpoint: 9 items done, 1 cut, 2 deferred.** The port had mipmaps
disabled on both shipped backends:
`gfx_opengl.c:1744-48` dropped them because N64 textures are NPOT (32×48 and
similar) and `glGenerateMipmap` fails **silently** on macOS Metal for those,
leaving an incomplete texture; the WebGPU backend copied the workaround. For a
kart racer that is the worst place to lose them — the track surface is
permanently at a grazing angle receding to the horizon.

**What shipped.** `platform/fast3d/gfx_mipgen.{c,h}` builds the chain on the
CPU, once, at texture-cache fill. `glGenerateMipmap` is never called on either
backend, so the driver bug is bypassed by construction rather than worked
around. `gfx_rapi->upload_texture_mipped()` is a new optional vtable entry;
GL and WebGPU implement it. The explicit Metal backend is not built by this
port; macOS GL remains Metal-backed by the OS. `tests/test_mip_chain.c` is
ROM-free and GPU-free.

Two filtering rules, both covered by unit tests:

- **Linear light, not sRGB bytes.** Averaging the encoded bytes of black and
  white gives 127; averaging the light they represent gives 188. The first is
  the classic "mipmaps look muddy" artifact. Alpha is coverage, not light, so it
  averages with no transfer function.
- **Exact-area box, not a fixed 2-tap.** A 2-tap box silently drops the last row
  or column at odd dimensions.

**Cutout materials** get coverage-preserving reduction so fences and foliage do
not erode with distance. Measured on a picket fence (coverage 0.25, threshold
192): a plain box loses it completely at level 2; the pass keeps it at 0.500.

**Three defects found in that pass, two by measuring rather than by tests** —
each produced a plausible-looking result. Corrections compounding across levels;
the search predicate and the write-back disagreeing by rounding; and the
textbook "smallest scale reaching the target" rule overshooting below Nyquist
and turning a fence into a **solid wall**, which is worse than the erosion being
fixed.

**Verified.**

```
$ ./build/mdkr_mip_chain_test
all mip_chain tests passed

$ python3 tests/check_video_presets.py --build build --rom baserom.us.v80.z64
PASS video presets: 3400 [PACE] rows identical across pure/restored/remastered
```

**Honest limits.**

- The mip **benefit** is not demonstrated. Chains are verifiably built, uploaded
  and sampled, and frames differ with mipmapping as the only variable — but no
  measurable shimmer *reduction* could be shown on Ancient Lake, whose road is
  nearly flat grey. A high-frequency ground surface is needed to prove the win.
- The 2D level-0 pin has **no positive control**: removing it changes 0 of 60
  menu frames, because this game draws 2D at or above 1:1 so the computed LOD
  already lands on level 0. Kept as an explicit invariant, not claimed as a fix.
- **Changing the default mode changed rendering for every check that does not
  name one.** `check_shadow_visual_ab` failed: its "every touched pixel only
  gets darker" invariant assumes shadow coverage is the sole difference between
  its arms, and mipmaps plus 16× anisotropy flipped 2 RGB components brighter.
  A/B on the same build confirmed it — forced `--pure` passes, default fails —
  so the check now pins the reference presentation. M2's lighting work is a far
  larger perturbation and should pin modes from the start.

**Also fixed.** GL was forcing `GL_TEXTURE_MAX_ANISOTROPY` to the hardware
maximum whenever linear filtering was on, ignoring `Video.AnisotropicFiltering`
entirely — so GL disagreed with WebGPU and Pure filtered unfaithfully.

**Left open at this mip checkpoint.** WebGPU MSAA (IQ-8), render scale
(IQ-9), logical-size readback (IQ-10), texture packs (IQ-11), and mgb64
backflow (IQ-12). The later checkpoints below complete IQ-9, IQ-10, and IQ-12;
IQ-8 and IQ-11 remain deferred.

### imagequality follow-up — `Video.RenderScale` was inert on both backends

The remaster's headline fidelity setting did nothing. WebGPU never read
`g_pcRenderScale`. GL read it in exactly one place —
`gfx_opengl_scene_target_enabled()` — to decide *whether* a scene framebuffer
should exist, never to size one: `ensure_scene_target()` was called with the
unscaled drawable. Restored and Remastered have been advertising 2×
supersampling that resized nothing and changed no pixel.

The multiply lives in mgb64's shared frontend
(`gfx_sync_current_dimensions_from_window` sets `gfx_current_dimensions =
window * RenderScale`). This port replaced that frontend wholesale with
`gfx_pc_dkr.c` and carried across the setting, its clamp and its scene-target
gate — but not the line that scales.

Fixed in `gfx_set_dimensions`, so the display layout, viewports, scissors and
the backend's scene target all move into one scaled *render* space together and
the backend resolve is the single place it returns to output space. Scaling a
scene target without scaling the viewports that draw into it renders the frame
into one corner, which is why this cannot be a backend-local change.

**GL only, deliberately.** Its end-of-frame path already blits scene → drawable
with `GL_LINEAR` — exactly the supersample resolve — and its readback runs
against the resolved default framebuffer, so frame dumps stay output-sized
(verified 1920×1080 at both 1× and 4×). WebGPU presents with a plain
texture-to-texture copy that requires matching sizes and reads back the raw
scene; enabling scale there without a resolve would hand every pixel-comparison
gate supersampled frames and silently change what every fidelity score means.

**Verified.** 1× and 4× differ on GL; high-frequency energy over the lower half
of the frame drops 1.4%; a 4× nearest-neighbour crop of the most-changed region
(located by differencing the two runs, not chosen by eye) shows stair-stepping
on a balloon and tree edges resolving into clean curves.
`check_renderer_backends` still passes — GL and WebGPU reach the same race
within its pixel budgets despite GL now supersampling.

### imagequality — supersampling reaches the web build (IQ-9/IQ-10 complete)

The WebGPU half of the render-scale work. WebGPU was the native default at this
checkpoint and is the **only** backend in the browser, so until this landed the setting was
unreachable where it matters most.

`wgpu_run_resolve()` is an N×N box, not a bilinear shrink. Bilinear is an exact
box at 2× (each output centre lands on a texel boundary, so the four neighbours
weigh ¼) but at 4× it reads 4 of the 16 covered texels and discards the rest —
which would quietly make 4× supersampling *worse* than 2× on thin geometry.
Measured: high-frequency energy drops 4.0% with the box against 1.4% with GL's
single bilinear blit.

**IQ-10.** The resolve writes a persistent output-sized texture rather than the
surface, and the present copy, readback and both dump paths read that. The
live-frame readback resolves first. Frame dumps are 1920×1080 at both 1× and 4×.

**Initial conclusion, superseded below.** Turning supersampling on by default
broke 7 of 28 checks —
`check_widescreen_proportions` could no longer locate the art motif it measures,
and `check_renderer_backends` went from a mean GL/WebGPU MAD under 4 to 59. All
are exact-pixel comparisons and all were right to complain. Pinning those seven
back to 1× was the wrong direction: it would leave the shipped default as the
one configuration the pixel harness never looks at. So every mode preset stays
on the reference presentation and `check_video_presets` covers the scaled path
directly, asserting two properties that pull against each other — the 4× frame
must *differ* from 1× (or the setting is inert, which it silently was) and the
readback must stay output-sized (or every pixel gate starts scoring supersampled
images without saying so).

**Verified in a real browser.** `check_browser_runtime` PASSES at 1×: 3600
wasm/WebGPU frames, median 60.0 fps, exact EEPROM+ROM reload, AudioWorklet,
zero-upload network audit. The same gate at `?scale=4`, with its drawable
assertion moved to the scaled values, also PASSES — the engine consumed
`drawable=5040x2160` and `2560x1920` while the canvas backing store stayed
1260×540 and 640×480. That is the surface/output split holding under
emdawnwebgpu, a different WebGPU implementation from native wgpu-native.

The resolve does not flatten the image: the same frame has **more** distinct
colours at 4× than 1× (139333 vs 127186) with mean and standard deviation
unchanged to two decimals.

Native suite: 28/28. `ctest` 4/4.

### imagequality — supersampling ships on, and the diagnosis that nearly hid a bug

Supersampling is **on by default** in Restored and Remastered (`RenderScale=2`),
on both backends and in the browser.

Getting there required unwinding a wrong conclusion. Enabling it broke 7 of 28
checks, and the obvious reading — anti-aliasing changes every pixel, so
exact-pixel comparisons must be re-tolerated — was comfortable and false. Two of
the seven compare arms that supersampling affects *identically*:
`check_rom_revision` compares ROM byte orders and `check_renderer_backends`
compares backends. Filtering cannot explain either. That contradiction was
visible immediately and was not acted on.

Looking at the frames instead of the theory showed a blurry **upscale**, which
is a sizing bug. Cause: PERF-020 debounces window-drag resizes by comparing the
requested size against `s_cfg_w/h`, which track the configured surface. Once the
surface followed the OUTPUT size while the request was still the RENDER size,
`req == s_cfg` could never hold, so every frame took the debounce branch and the
committed scene size oscillated between the two. Fixed by debouncing the output
size and deriving the scene from it.

GL/WebGPU mean absolute difference on the same frame: **33.948 → 1.163** against
a budget of 4.000. All seven checks pass with supersampling on.

**Lesson worth keeping:** when a rendering change breaks a pixel check, first ask
whether that check compares two arms the change should affect *equally*. If it
does, it is a bug, not a tolerance problem.

**Also added.** `[DISPLAY] output=WxH render=WxH scale=N` — the existing layout
line works in render space, so its `drawable=` is the scaled scene, not the
window. `check_browser_runtime` now asserts `output=`, which is a more precise
assertion than the one it replaced; `drawable=` was only ever correct by the
accident of scale being 1.

**Verified.** `check_browser_runtime` PASSES with supersampling on: 3600
wasm/WebGPU frames at median 60.0 fps, exact EEPROM+ROM reload, AudioWorklet,
zero-upload network audit.

## restoration/remaster sprint — exact sprite bounds, RDP interpolation, SDF text, moving mips, RL-1

This sprint executes the first dependency-complete slice of the restoration and
visual-remaster plans. It closes two correctness defects, proves the already
shipped mip path in motion, lands the first Remastered-only interface treatment,
and resolves RL-1 without pretending that a diagnostic lighting experiment is a
production sun rig.

### Restoration correctness: sprite layout and clipped interpolation

**Sprite allocation is now derived from the serialized record, not an
allocation formula reconstructed at each caller.** `sprite_layout.c` validates
the variable-length frame-asset record, performs checked alignment/addition/
multiplication, and returns one exact layout for the `Sprite`, frame-pointer
array, triangles, display-list storage, vertices, and texture pointers. All four
sprite-asset readers share the validated boundary, and the builder unwinds
loaded textures, allocation, and cache state on failure. The independent census
normalizes z64/v64/n64 byte order and recognizes both supported US/EU v80
layouts; the available US v80 ROM contains 193 sprites and a maximum 48-byte
sprite record. Two records prove the historical under-allocation:
sprite 162 emits 29 `Gfx` commands where the old formula reserved 27 (16 bytes
short), and sprite 177 emits 67 where it reserved 66 (8 bytes short).
`test_sprite_layout` covers overflow, truncation,
alignment, and cleanup; `check_sprite_layout.py` locks the production call sites
and ROM census.

**Shade and fog interpolation now follow screen-linear RDP semantics.** The HLE
selects noperspective interpolation when the generated shader carries
`SHADER_OPT_NOPERSPECTIVE_INPUTS`; the exact legacy perspective path remains
available only as the diagnostic `MDKR_RDP_GRADIENTS=legacy` control. Clipping
recomputes fog from the interpolated clip-space vertex instead of interpolating
the already-derived fog scalar. The unit control produces fog 60 under the old
rule versus 28 from the clipped vertex. In production captures, the correction
changes 954,454 of 1,228,800 GL pixels (component MAD 10.533) and 954,422 on
WebGPU (component MAD 10.532), while all 2,900 `[PACE]` rows remain identical.

### Remastered interface: runtime-derived SDF fonts

Font lifetimes are explicit. `font.c` registers and unregisters each
`ASSET_FONTS` atlas at the real load/free boundary; a reference-counted registry
merges glyph cells into bounded source regions and invalidates derived cache
entries when the source dies. The renderer derives a region-isolated 4x coverage
field in memory, clamps every glyph to its registered cell to prevent neighbour
bleed, uploads it directly to the active backend, and uses point sampling at LOD
0 for DKR's actual text texrect/combiner path.

The path is gated by `RemasterFX` and therefore affects Remastered only.
`MDKR_FONT_SDF=0` is a test control, not a user-facing setting. Across GL and
WebGPU, Pure and Restored are byte-identical with the control; Remastered reports
two derived uploads and changes only known text regions, with identical timing.
The real browser reports 52 SDF uploads over 3,600 frames, zero registry
failures, zero stale texture-cache hits, and exact ROM/EEPROM reload.
`check_no_rom.sh` rejects derived-font filenames and includes a positive-control
artifact, while a source contract proves the derivation layer exposes no file
output and the only result consumer is the immediate GPU upload path.

### M1 proof and RL-1 decision

`check_mip_motion.py` drives 271.3 world units through 24 consecutive Everfrost
Peak frames with anisotropy fixed at 1 and mipmaps as the only variable. Temporal
second-difference energy falls 34.859 to 31.286 on GL (10.25%) and 34.428 to
30.972 on WebGPU (10.04%). Both enabled arms build 854 complete chains / 4,881
levels, both disabled arms build zero, and all four 3,500-row `[PACE]` streams
are identical. This closes the M1 visual-benefit gap; WebGPU MSAA and a
content-free texture-pack loader remain explicitly deferred.

RL-1 is a three-arm, Remastered-only diagnostic:

- baked vertex colour;
- baked colour retained as a low-frequency base plus restrained diagnostic sun;
- baked colour superseded by diagnostic directional light.

The decision is to **retain the baked base and reject supersession**. On Fire
Mountain, supersession moves mean luma 111.97 to 144.55 and saturation 155.23
to 136.14, erasing the authored dark/warm mood; the retained-base arm stays at
114.80 / 145.04. On Ancient Lake, retained-base luma is 146.62 against baked
150.29, while supersession drops to 134.47. Supersession is 2.5–3.5x farther
from the baked captures. The experiment also exposed coarse per-triangle
faceting, so at this checkpoint RL-2 still needed a level-owned sun and smooth
directionality without replacing vertex colour. Wave 2 subsequently closed
that work with the compact model-normal stream described at the top of this
log. `MDKR_RL1_ARM` remains a diagnostic seam and is not published through the
video schema.

### Verification

Debug, Release, and ASan builds compile and their 12 CTests pass. A fresh wasm
build passes the ROM-absence guard; real Chromium reaches 3,600 paced frames at
median 60 fps with mip/SDF telemetry, exact persistence, and zero ROM upload.
At this restoration checkpoint, the registered full manifest contained 38 check
scripts and 45 tasks. The
texture-lineswap visual gate was corrected during the full run: its old dynamic
mask assumed every changed framebuffer row mapped to the same source texel row,
which screen-linear interpolation correctly disproved on minified 3D surfaces.
The fixed gate retains the dynamic changed-pixel and improvement requirements
and applies the absolute legacy threshold only to its fixed minimap ROI, with
`--pure` explicit in every arm. No product tolerance was widened.

The clean serial rerun passes **45/45 tasks in 31m52s**, including the complete
20-track/47-vehicle alignment UBSan matrix, Release and ASan specializations,
linked wasm, and real Chromium. The browser arm reports 3,600 frames at median
60.0 fps, 52 SDF uploads, 901 mip chains / 5,110 levels, exact ROM/EEPROM
reload, and a passing zero-upload network audit.
