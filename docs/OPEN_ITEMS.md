# Open items

This document was split by subsystem. It is now an index and a redirect;
the entries live under [`open-items/`](open-items/README.md).

| File | Covers |
|---|---|
| [`open-items/renderer.md`](open-items/renderer.md) | Renderer and visual fidelity |
| [`open-items/audio.md`](open-items/audio.md) | Audio |
| [`open-items/save.md`](open-items/save.md) | Saves and progression |
| [`open-items/collision.md`](open-items/collision.md) | Collision |
| [`open-items/allocator.md`](open-items/allocator.md) | Allocator and native memory layout |
| [`open-items/web.md`](open-items/web.md) | Browser (wasm) build |
| [`open-items/multiplayer.md`](open-items/multiplayer.md) | Multiplayer |
| [`open-items/gameplay.md`](open-items/gameplay.md) | Gameplay, race and Adventure |
| [`open-items/portability.md`](open-items/portability.md) | 64-bit (LP64), endianness and portability |
| [`open-items/misc.md`](open-items/misc.md) | Everything else |

[`open-items/README.md`](open-items/README.md) carries the still-open table
and the full by-subsystem index.

## Legacy section anchors

Before the split, every entry below was a section of this file. The table maps
each old section title to where its write-up lives now. Deep links written
against the old `OPEN_ITEMS.md#<section>` anchors do not resolve any more — use
the target column.

| Section (legacy anchor) | Now lives in |
|---|---|
| FIXED: v0.4 playthrough shadow defects — wave "shadowplay" | [open-items/renderer.md](open-items/renderer.md#fixed-v04-playthrough-shadow-defects--wave-shadowplay) |
| FIXED: pre-release shadow deep review — wave "shadowdeep" | [open-items/renderer.md](open-items/renderer.md#fixed-pre-release-shadow-deep-review--wave-shadowdeep) |
| FIXED: save progression and persistence isolation | [open-items/save.md](open-items/save.md#fixed-save-progression-and-persistence-isolation) |
| FIXED: MIPS numeric conversion closeout | [open-items/portability.md](open-items/portability.md#fixed-mips-numeric-conversion-closeout) |
| FIXED: core safety boundaries — wave "core-safety" | [open-items/portability.md](open-items/portability.md#fixed-core-safety-boundaries--wave-core-safety) |
| FIXED: restoration/remaster sprint — sprite bounds, RDP gradients, SDF text, moving mips, and RL-1 | [open-items/renderer.md](open-items/renderer.md#fixed-restorationremaster-sprint--sprite-bounds-rdp-gradients-sdf-text-moving-mips-and-rl-1) |
| FIXED: playable runtime boundaries — wave "runtimebounds" | [open-items/portability.md](open-items/portability.md#fixed-playable-runtime-boundaries--wave-runtimebounds) |
| FIXED: central allocator boundaries — wave "allocinv" | [open-items/allocator.md](open-items/allocator.md#fixed-central-allocator-boundaries--wave-allocinv) |
| FIXED: native representation boundaries: object tails, level records, and render proxies — wave "nativelayout" | [open-items/allocator.md](open-items/allocator.md#fixed-native-representation-boundaries-object-tails-level-records-and-render-proxies--wave-nativelayout) |
| FIXED: widescreen world billboards retained the N64's 4:3 stretch — wave "billboardaspect" | [open-items/renderer.md](open-items/renderer.md#fixed-widescreen-world-billboards-retained-the-n64s-43-stretch--wave-billboardaspect) |
| FIXED: RAW16 bass was decoded as host-endian noise — wave "raw16" | [open-items/audio.md](open-items/audio.md#fixed-raw16-bass-was-decoded-as-host-endian-noise--wave-raw16) |
| FIXED: object-model collision never reported a hit, so locked doors were intangible — wave "objcoll" | [open-items/collision.md](open-items/collision.md#fixed-object-model-collision-never-reported-a-hit-so-locked-doors-were-intangible--wave-objcoll) |
| FIXED: the filename renderer read beyond a one-byte global — wave "filename-cstr" | [open-items/renderer.md](open-items/renderer.md#fixed-the-filename-renderer-read-beyond-a-one-byte-global--wave-filename-cstr) |
| FIXED: banana sparkle sprite overran its own vertex region | [open-items/renderer.md](open-items/renderer.md#fixed-banana-sparkle-sprite-overran-its-own-vertex-region) |
| FIXED: near-clipped fog caused the moving lower-screen shadow | [open-items/renderer.md](open-items/renderer.md#fixed-near-clipped-fog-caused-the-moving-lower-screen-shadow) |
| FIXED: supersampling could not improve source-resolution font contours | [open-items/renderer.md](open-items/renderer.md#fixed-supersampling-could-not-improve-source-resolution-font-contours) |
| FIXED: wasm call signatures disagreed across translation units — wave "wasm-abi" | [open-items/web.md](open-items/web.md#fixed-wasm-call-signatures-disagreed-across-translation-units--wave-wasm-abi) |
| FIXED: three ROM-fidelity divergences, and the fixture class that was blocking them — wave "closedloop" | [open-items/gameplay.md](open-items/gameplay.md#fixed-three-rom-fidelity-divergences-and-the-fixture-class-that-was-blocking-them--wave-closedloop) |
| FIXED: a one-shot cutscene replayed for ever, because its latch was undefined behaviour the optimiser deleted — wave "keyshift" | [open-items/gameplay.md](open-items/gameplay.md#fixed-a-one-shot-cutscene-replayed-for-ever-because-its-latch-was-undefined-behaviour-the-optimiser-deleted--wave-keyshift) |
| SWEPT: three shapes no instrument could see — wave "boundsweep" | [open-items/collision.md](open-items/collision.md#swept-three-shapes-no-instrument-could-see--wave-boundsweep) |
| NOT A DEFECT: the boss win cutscene was skipped because the win was already recorded — wave "bossverdict" | [open-items/gameplay.md](open-items/gameplay.md#not-a-defect-the-boss-win-cutscene-was-skipped-because-the-win-was-already-recorded--wave-bossverdict) |
| Hand-asm transcription audit — wave "hasmaudit" | [open-items/portability.md](open-items/portability.md#hand-asm-transcription-audit--wave-hasmaudit) |
| SWEPT: decomp arrays split into two C objects and indexed across the boundary (wave "splitsweep") | [open-items/portability.md](open-items/portability.md#swept-decomp-arrays-split-into-two-c-objects-and-indexed-across-the-boundary-wave-splitsweep) |
| FIXED: racers fall through Tricky's volcano — the collision grid mask never filtered in Z (wave "gridmask") | [open-items/collision.md](open-items/collision.md#fixed-racers-fall-through-trickys-volcano--the-collision-grid-mask-never-filtered-in-z-wave-gridmask) |
| Save-file fail-safe and browser save recovery — wave "savefailsafe" | [open-items/save.md](open-items/save.md#save-file-fail-safe-and-browser-save-recovery--wave-savefailsafe) |
| FIXED: "interlaced" textures decoded scrambled — the odd-row TMEM word swap (wave "lineswap") | [open-items/renderer.md](open-items/renderer.md#fixed-interlaced-textures-decoded-scrambled--the-odd-row-tmem-word-swap-wave-lineswap) |
| FIXED: browser "memory access out of bounds" in the wave renderer — wave "wavetable" | [open-items/web.md](open-items/web.md#fixed-browser-memory-access-out-of-bounds-in-the-wave-renderer--wave-wavetable) |
| FIXED: browser audio event-queue drops — measured in the real runtime | [open-items/audio.md](open-items/audio.md#fixed-browser-audio-event-queue-drops--measured-in-the-real-runtime) |
| P3.6 two-player split-screen — wave "splitscreen" (WORKS) | [open-items/multiplayer.md](open-items/multiplayer.md#p36-two-player-split-screen--wave-splitscreen-works) |
| P3.5 Adventure — the hub is drivable and the trophy series is covered | [open-items/gameplay.md](open-items/gameplay.md#p35-adventure--the-hub-is-drivable-and-the-trophy-series-is-covered) |
| P3.3 MAGIC_CODES / FILE_SELECT text fidelity — wave "p33-text" | [open-items/renderer.md](open-items/renderer.md#p33-magiccodes--fileselect-text-fidelity--wave-p33-text) |
| M4.5 WebGPU backend — DONE (default backend; GL fallback). Open notes: | [open-items/renderer.md](open-items/renderer.md#m45-webgpu-backend--done-default-backend-gl-fallback-open-notes) |
| M5 audio — DONE (music + SFX via the aspMain software mixer) | [open-items/audio.md](open-items/audio.md#m5-audio--done-music--sfx-via-the-aspmain-software-mixer) |
| Playability wave — memory safety, saves, and the race finish | [open-items/gameplay.md](open-items/gameplay.md#playability-wave--memory-safety-saves-and-the-race-finish) |
| Phase 2 — menu 1:1 fidelity: every screen now scored (wave "oraclefix") | [open-items/renderer.md](open-items/renderer.md#phase-2--menu-11-fidelity-every-screen-now-scored-wave-oraclefix) |
| FIXED: no near-plane clipping in the HLE (wave "nearclip") | [open-items/renderer.md](open-items/renderer.md#fixed-no-near-plane-clipping-in-the-hle-wave-nearclip) |
| FIXED: headless renders were NOT reproducible (wave "determinism") | [open-items/renderer.md](open-items/renderer.md#fixed-headless-renders-were-not-reproducible-wave-determinism) |
| LP64 pointer-truncation crash class — SYSTEMATIC SWEEP (robust: wave) | [open-items/portability.md](open-items/portability.md#lp64-pointer-truncation-crash-class--systematic-sweep-robust-wave) |
| Frame pacing / slow-motion — RESOLVED (pacing wave) | [open-items/renderer.md](open-items/renderer.md#frame-pacing--slow-motion--resolved-pacing-wave) |
| M4 render state (this wave — input + interactive menus) | [open-items/renderer.md](open-items/renderer.md#m4-render-state-this-wave--input--interactive-menus) |
| From asset-swap workstream (M2-swap, commit b1600b2) | [open-items/portability.md](open-items/portability.md#from-asset-swap-workstream-m2-swap-commit-b1600b2) |
| F3DDKR semantics pinned down (M3c renderer wave — text/logo/sprites) | [open-items/renderer.md](open-items/renderer.md#f3ddkr-semantics-pinned-down-m3c-renderer-wave--textlogosprites) |
| From F3DDKR renderer workstream (M3-gfx, commit a8dcd00) | [open-items/renderer.md](open-items/renderer.md#from-f3ddkr-renderer-workstream-m3-gfx-commit-a8dcd00) |
| [RESOLVED in M3] LP64 asset-struct layout — the blocker below is fixed | [open-items/portability.md](open-items/portability.md#resolved-in-m3-lp64-asset-struct-layout--the-blocker-below-is-fixed) |
| M3 rendering state (UPDATED — M3b renderer bring-up) | [open-items/renderer.md](open-items/renderer.md#m3-rendering-state-updated--m3b-renderer-bring-up) |
| M3 BLOCKER — LP64 asset-struct layout (found in M2, the "first real frame" gate) | [open-items/portability.md](open-items/portability.md#m3-blocker--lp64-asset-struct-layout-found-in-m2-the-first-real-frame-gate) |
| Asset-swap gaps found while iterating M2 | [open-items/portability.md](open-items/portability.md#asset-swap-gaps-found-while-iterating-m2) |
| Integration pending | [open-items/misc.md](open-items/misc.md#integration-pending) |
| M8 web (wasm) build — DONE (boots, runs, RENDERS correctly in-browser) | [open-items/web.md](open-items/web.md#m8-web-wasm-build--done-boots-runs-renders-correctly-in-browser) |
| Menu 1:1 fidelity (frontend Timber's Island background) — wave "menufi" | [open-items/renderer.md](open-items/renderer.md#menu-11-fidelity-frontend-timbers-island-background--wave-menufi) |
| Race gameplay — tiny racer models + crawl speed — FIXED (single root cause) | [open-items/gameplay.md](open-items/gameplay.md#race-gameplay--tiny-racer-models--crawl-speed--fixed-single-root-cause) |
