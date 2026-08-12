# The browser (wasm) build

> **This work has landed** — the browser build ships and is gated by
> `check_browser_runtime` against actual Chromium. This document was written
> ahead of the M8 wave; the "why the architecture was already web-ready"
> analysis and the watch-items are the parts still worth reading. The status
> section at the end records what was actually achieved.

The goal was DKR playable in-browser with a user-supplied ROM. This is the
reason the platform layer stays cooperative-single-threaded and
pointer-token-based.

## Browser 1.0 player-data requirement

IDBFS persistence is implemented, but browser storage is not durable custody.
Browser 1.0 therefore also requires launcher-level save export, transactional
import, and a validated simple editor that work without a ROM, WebGPU, or a
running engine. The shared codec, raw/container formats, storage transaction,
UX, security model, and test gates are documented in
[`../SAVE_MANAGEMENT.md`](../SAVE_MANAGEMENT.md).

## Why the current architecture is already web-ready
- **Cooperative single thread** (architecture decision 1): no wasm pthreads needed. The blocking
  `osRecvMesg` on the retrace queue IS the frame boundary — under emscripten it
  suspends via Asyncify and resumes on requestAnimationFrame, exactly as mgb64 does
  (`platformFrameSync` → `emscripten_sleep`/rAF wait). Port DKR's `platform_frame_sync`
  to the same EM_ASYNC_JS rAF wait under `__EMSCRIPTEN__`.
- **dkrptr32 tokens** (architecture decision 8): pointers are natively 32-bit on wasm32, so the
  token/arena scheme is a no-op-cost win there (no truncation risk at all).
- **16MB arena** (architecture decision 2): fixed, malloc'd — fine under wasm (grows via ALLOW_MEMORY_GROWTH).
- **Assets from ROM at runtime** (architecture decision 4): ships asset-free; ROM provided in-browser.

## Direct ports from mgb64 (files exist, proven)
- `src/platform/fast3d/gfx_webgpu.c` (205KB), `gfx_webgpu_shader.c`, `gfx_webgpu.h`,
  `gfx_webgpu_compat.h` — the WebGPU backend. Comes in at M4.5 for native; the SAME
  file serves wasm (emscripten supplies WebGPU via its port, not wgpu-native).
- `src/platform/web_audio_worklet.c/.h` — AudioWorklet output path (M5 audio must be
  done; worklet replaces SDL audio device under wasm).
- CMakeLists `EMSCRIPTEN` branch (mgb64 CMakeLists.txt ~2118-2260): `ge007_web`
  target. Adapt to `mdkr64_web`. Key link options to replicate:
  - `-sUSE_SDL=2` (emscripten SDL2 port; window/input)
  - `-sUSE_WEBGPU=1` (emscripten WebGPU port) — verify exact flag in mgb64
  - **Asyncify, narrowed** (critical): NOT blanket `-sASYNCIFY` (1.5–3× perf hit on
    instrumented paths). Use `-sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT` + an
    `-sASYNCIFY_ADD=[...]` list naming the suspend spine. For DKR the spine is:
    `main` (owns the indirect gfx_init→rapi->init bring-up edge), the game boot
    entry (DKR: `thread3_main`/`main_game_loop` — owns game-loop indirect dispatch),
    `osRecvMesg` (real leaf: rAF wait + hidden-tab sleep inline here), and any
    level-load yield leaf if we add one. Confirm the set with `-sASYNCIFY_ADVISE`.
  - `-sALLOW_MEMORY_GROWTH=1`, `-sINITIAL_MEMORY` sized for 16MB arena + heaps.
  - `-sEXIT_RUNTIME=0`, `-sASYNCIFY_STACK_SIZE` tuned.

## M4.5 done — what it de-risks / what still remains for M8
At M4.5 the WebGPU backend became the native default and matched GL across the
fixture set captured then (`docs/STATUS.md` M4.5). A later throughput experiment temporarily made
GL the native default, but dense opening-sequence captures exposed localized GL
texture corruption that the sparse comparison had missed. WebGPU is therefore
again the qualified native default and remains the browser's only backend; GL
is an explicit native diagnostic path pending visual-parity work.
Findings that matter for the Emscripten step:
- **The backend keeps one native/browser implementation:** the current
  `gfx_webgpu.c` descends from mgb64's backend but includes this project's DKR
  lifecycle, presentation, and recovery work. This repository's same source file
  compiles for both native and wasm. The remaining shell symbols
  it references are satisfied by platform/gfx_webgpu_stubs.c (inert) — those stubs
  are native-only glue; under Emscripten the overlay/minimap/host-handoff are
  equally absent, so the same stubs (or web equivalents) apply. NONE of them are
  on a suspend path.
- **The dialect seam already exists**: gfx_webgpu_compat.h has the full
  `__EMSCRIPTEN__` branch (WGPU_COMPAT_PUMP = emscripten_sleep + ProcessEvents,
  WGPU_COMPAT_PRESENT = no-op, canvas surface via
  WGPUEmscriptenSurfaceSourceCanvasHTMLSelector "#canvas"). It is vendored intact.
  The web build MUST keep the "#canvas" selector in sync with the index.html shell.
- **Asyncify spine (confirm with -sASYNCIFY_ADVISE)**: the WebGPU async surfaces
  we exercise natively via synchronous wgpuDevicePoll are, on web, the suspend
  points. From the vendored code they are: wgpu_init/gfx_webgpu_bringup
  (requestAdapter + requestDevice), and the buffer-map in read_framebuffer_rgb /
  the end_frame PPM dump (wgpuBufferMapAsync). Our headless frame-dump path drives
  read_framebuffer_rgb every frame — on web that map is an Asyncify unwind, so the
  dump path is web-heavy; gate --dump-frames off in the shipped web build (it is a
  validation tool, not a gameplay path). mgb64's ASYNCIFY_ADD list (M8 spec above)
  already names wgpu_init + gfx_webgpu_bringup; add DKR's gfx_init edge + the
  osRecvMesg rAF leaf.
- **-sUSE_WEBGPU=1 is the WRONG flag**: mgb64 uses `--use-port=emdawnwebgpu`
  (webgpu.cmake EMSCRIPTEN branch), NOT `-sUSE_WEBGPU=1` (the older, unmaintained
  binding). cmake/webgpu.cmake's EMSCRIPTEN branch already wires
  `--use-port=emdawnwebgpu` — reuse that wiring; do not hand-roll the flag.
- **No wgpu-native under wasm**: cmake/webgpu.cmake returns an INTERFACE-only
  `webgpu` target under EMSCRIPTEN (no prebuilt fetch). The native link
  (libwgpu_native.a + Metal/AppKit/Security frameworks) is desktop-only.
- **Offscreen-render + present-copy is drawable-tolerant**: natively the backend
  renders the scene offscreen and copies to the surface at present; a hidden /
  no-drawable window still renders + reads back. On web the canvas is always
  present, so the direct-to-surface (PERF-008) path can engage — fine.
- **Readback swizzle / format**: native picks a BGRA8-preferring surface format
  (WGPU_COMPAT_PREFER_FIRST_SURFACE_FORMAT 0); web takes caps.formats[0]
  (RGBA8 on some GPUs). read_framebuffer_rgb already handles BGRA->RGB; verify the
  web format path when wiring the wasm frame-dump/validation, if any.

## Browser ROM + saves (user-supplied, asset-free binary)
- ROM: HTML file picker → read ArrayBuffer → write to emscripten FS (MEMFS) at the
  path rom_io.c expects, OR keep a copy in IndexedDB (via IDBFS) so it persists
  across reloads. rom_io.c already loads the whole ROM into the arena — feed it the
  in-FS path. Validate size/name same as native.
- Saves: mount IDBFS at `save/`, `FS.syncfs` after EEPROM writes (the __wasi_fd_sync
  suspend site mgb64 documents — already covered by the Asyncify EEPROM-save path).
  Two tabs share one IndexedDB store, so write ownership is decided before
  anything mounts `/save`: the first session takes an exclusive
  `navigator.locks` claim and is the only one that syncs IDBFS; a later tab
  becomes a spectator, plays from its in-memory copy, and shows a notice instead
  of last-writer-winning over the store. Where Web Locks is absent the claim
  falls back to a weaker localStorage heartbeat.

## Serving / packaging (mirror mgb64 dist/web)
- Output: `mdkr64_web.wasm` + `.js` glue + a minimal `index.html` shell with a
  canvas, ROM file-picker UI, and a "no ROM distributed" notice. Copy mgb64's
  dist/web shell structure. WebGPU requires a Chromium-based browser or WebGPU-
  enabled Firefox; feature-detect and show a message otherwise.
- Serve over HTTPS (WebGPU + SharedArrayBuffer-free since single-thread — no COOP/COEP
  headers strictly required, confirm). Static host is fine (it's a static wasm app).
- A published page also ships `sw.js` and `manifest.webmanifest`. The service
  worker is registered with the publish stamp appended, so each build gets its
  own cache and an installed page can never serve one build's wasm beside
  another's JS glue; it deliberately does not call `skipWaiting()`. Every role
  document carries the same `data-build-stamp`. An old active worker may return
  a newer network document during the waiting-worker window, but it will not
  write that document into its old cache. Offline recovery therefore returns a
  complete old release, and activation deletes the old cache only after all old
  controlled documents close.

## Acceptance — met

- `emcmake cmake` + `cmake --build` produces mdkr64_web.{wasm,js}. Loads in Chrome,
  user picks a .z64, game boots to menu, playable with keyboard + gamepad, saves
  persist across reload. Frame pace ~stable via rAF.
- Browser presentation is explicitly rAF-bounded. The launcher exposes
  `original`, `display`, and numeric ceilings but not native-style unbounded
  pacing. A shared config containing `uncapped` resolves to `display` with
  requested/effective diagnostics. Numeric policies skip rAF opportunities on
  the same absolute rational deadline grid as native; `display` consumes one
  opportunity per callback. With motion smoothing Off, those opportunities hold
  the latest authored image. With Interpolated selected, they submit unique
  images reconstructed from immutable adjacent authored tasks while authority
  remains at 30 Hz NTSC or 25 Hz PAL. Hidden documents suspend at the Asyncify
  boundary and the resumed interval rebases instead of producing background
  catch-up.

## Watch-items
- The `_Static_assert` offset locks (architecture decision 8) will re-validate at wasm32 pointer
  width — GOOD, they'll catch any layout assumption that only held on LP64.
- Audio worklet needs the mixer (platform/mixer.c) producing sample buffers; ensure
  M5 didn't couple to an SDL-only audio callback.
- Metal backend is native-only — exclude from wasm (WebGPU only in browser).

## M8 STATUS — RUNTIME ACCEPTANCE AUTOMATED (platform matrix still partial)

Implemented (`M8:` commits). The wasm engine COMPILES + LINKS clean, BOOTS in a
real browser, brings up WebGPU, and runs the full game loop through the
Asyncify/rAF frame boundary. Original mode retains the authored 30 Hz complete
loop; opt-in display/numeric policies change presentation and input/event-pump
opportunities without changing the fixed gameplay tick. Production motion
smoothing can fill those opportunities with immutable adjacent-task
interpolation; excess GPU work is held nonblockingly. Verified in headless Chrome
150 (Apple Metal-3,
`--headless=new --enable-unsafe-webgpu`) by the committed, dependency-free
`tests/check_browser_runtime.py` gate driving the actual `mdkr64_web.wasm`.

### What was built
- **CMake EMSCRIPTEN branch** (`CMakeLists.txt`): `mdkr64_web` target (mirrors
  mgb64 `ge007_web`). Same engine sources as native minus desktop-only bits —
  gates out `gfx_opengl.c`, `fast3d_gl_shim.c`, `platform_stdio.c`, glad; adds
  `web_audio_worklet.c`. SDL2 via `-sUSE_SDL=2`; WebGPU via the `webgpu` INTERFACE
  target (`--use-port=emdawnwebgpu`, from cmake/webgpu.cmake). Emits
  `mdkr64_web.{js,wasm}` (MODULARIZE, `createMDKR64`). Native build untouched
  (re-verified: builds clean, renders the attract title, race fixture exits 0).
- **Platform web adaptations**:
  - `platform_frame_sync` / `platform_vi_pace_measure` (platform_sdl_min.c): under
    `__EMSCRIPTEN__` the pacer suspends to `requestAnimationFrame`
    (`EM_ASYNC_JS platformWaitAnimationFrame`) instead of `nanosleep`, ALWAYS
    yielding at every presentation boundary (even on an overrun) so the
    cooperative loop never hangs the tab. Numeric caps may skip additional rAF
    opportunities before an authored image is ready. KEEPS the VI-field `updateRate` semantics — field count is
    derived from real (rAF-timed) wall clock, so the slow-motion fix holds
    in-browser. Original mode measures 30fps complete ticks (`R=2`); display
    and numeric policies preserve that tick stream; Interpolated motion can
    present between authored images without advancing it.
  - Web window (platform_sdl_min.c): a plain SDL window (no GL/Metal) bound to the
    page `#canvas`; the WebGPU surface comes from the `"#canvas"` selector
    (gfx_webgpu_compat.h) — kept in sync with the index.html canvas id.
  - Audio (audi_port_dkr.c + web_audio_worklet.c, vendored+adapted from mgb64):
    the AudioWorklet sink (off-main-thread drain) feeds the SAME mixer buffers as
    the native SDL device (leaf swap at `osAiSetNextBuffer`); SDL-emscripten audio
    is the automatic fallback if AudioWorklet is unavailable. The runtime gate
    requires a running context and measured PCM delivery (about 1.32 million
    sample-frames during the 3,600-frame route).
  - `--dump-frames` gated off on web (its wgpuBufferMapAsync is an Asyncify unwind).
  - Native-only bits gated: `execinfo.h`/signal crash handler (main_pc.c),
    `system("mkdir")` (stubs_dkr.c).
- **Asyncify (narrowed)**: `-sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT`
  `-sASYNCIFY_ADD=main,wgpu_init,osRecvMesg`.
  Confirmed with `-sASYNCIFY_ADVISE`: the surviving standalone roots are `main`
  (owns the `gfx_init -> rapi->init` INDIRECT bring-up edge after -O2 inlines
  gfx_init into it, AND sits on the per-frame stack), `wgpu_init` (bring-up leaf,
  requestAdapter/requestDevice), `osRecvMesg` (the rAF leaf). The rest inline into
  those and are deliberately omitted: listing absent symbols only emits warnings
  without adding instrumentation. Bring-up suspend AND the
  per-frame rAF suspend both unwind/rewind correctly — NO Asyncify "unreachable".
- **ROM + saves** (dist/web/index.html + mdkr64-shell.js): WebGPU feature-detect
  gate; `.z64` file picker validates size (12 MB) + header, writes the ArrayBuffer
  into MEMFS/IDBFS at `/rom/baserom.us.v80.z64`; `--rom` points the engine there.
  IDBFS mounted at `/rom` (ROM persists across reload) and `/save` (EEPROM);
  `FS.syncfs` after each EEPROM write (coalesced) + on a timer + pagehide. "No ROM
  distributed" notice + non-WebGPU-browser message. `#canvas` matches the surface
  selector. Binaries gitignored; only the shell is committed.

### wasm32-specific bugs found + fixed (the `_Static_assert` watch-item paid off)
1. **Arena/segment-token address collision (THE render blocker).** On LP64 the
   arena sits at a high 64-bit address (e.g. 0xc16000000) whose low-32 never
   collides with DKR's N64 segment tokens (0x0N000000). On wasm32 (ILP32) the
   arena landed at 0x02000000 — INSIDE the segment-token space — so `dkr_resolve`'s
   arena reconstruction swallowed the framebuffer/z-buffer segment tokens (SETCIMG
   0x01000000 -> NULL, SETZIMG 0x02000000 -> arena base) and NO geometry resolved.
   FIX (stubs_dkr.c `dkr_arena_init`, web-only): allocate the arena ABOVE the
   256 MB segment ceiling (`posix_memalign` to 0x10000000) so segment tokens always
   fall through to the segment table and arena addresses never look like a token.
   After this, geometry emits + resolves (`G_TRIN addr=90153d20->0x10153d20`).
2. **Z-clear FILLRECT drawn white** (gfx_pc_dkr.c). The z-buffer-clear skip
   compared RESOLVED pointers with a `!= NULL` guard; on wasm32 the framebuffer
   segment token resolves to NULL (it isn't a >4GB registered host pointer as on
   LP64), so the guard failed and the depth clear drew as an opaque white fill over
   the scene (the M3b symptom). FIX: compare the RAW SETCIMG/SETZIMG tokens
   (0x01000000 vs 0x02000000) — width-independent, identical result on native.

### What is PROVEN vs REMAINING
- PROVEN on every release run in a real browser (headless Chrome, actual wasm):
  the shipped shell accepts the user ROM through its real file input; WebGPU and
  Asyncify run 3,600 rAF frames at a sane ~60 Hz median; the route enters Ancient
  Lake and advances a racer; five sampled menu/race canvases are live and
  changing; CSS/DPR transitions produce 1260×540, 640×480, then 1260×540 engine
  drawables; the AudioWorklet consumes PCM; and the process exits cleanly.
- A separate real-Chromium schedule harness injects exact 144 Hz and irregular
  rAF timestamps. It verifies the expected host-opportunity counts for display
  and capped policies while requiring the same authored-image count, no
  duplicate swaps, and byte-identical v3 state, ordered events, input, and PCM.
- IDBFS is checked in both directions. The same isolated profile reloads without
  another picker action, and the exact 512-byte EEPROM hash plus the 12 MiB ROM
  are present before `main()`. Clearing progress retains the ROM; forgetting the
  ROM empties both stores. CDP and the local HTTP server reject external requests,
  request bodies, and any URL that names the ROM.
- The live-sink audio queue is now measured in-browser. The stock 150-entry SFX
  budget intermittently dropped posts; unrestricted demand reached 195. The port
  uses 512 entries and the gate fails if any audio queue drops or exceeds half its
  capacity. A forced one-entry positive control proves the diagnostic direction.
- RENDER FIDELITY — NOW CLOSED (follow-up wave). The white-sky / menus-not-drawn gap
  was the gfx_ptr registry gap: on wasm32 non-arena host pointers (game globals /
  rodata display lists) are <4GB so LP64's `>0xFFFFFFFF` registration never fires and
  dkr_resolve returned NULL for them -> the sky-gradient / menu-text / kart geometry
  that lives in globals+rodata DLs never drew. FIX (gfx_pc_dkr.c dkr_resolve,
  __EMSCRIPTEN__-gated): recover host pointers DIRECTLY from the token (ILP32 flip is
  reversible; raw <0x01000000 is a low global), no registry needed on wasm32; native
  byte-identical. VERIFIED in a real browser + scored vs native: attract Ancient Lake
  hist=0.996 block=1.000; OPTIONS menu hist=0.996 block=1.000; in-race renders
  correctly (track + HUD, eyeballed). The runtime gate now fails on GPU process
  exits, device-loss text, flat output, or a stalled frame loop; the earlier
  intermittent headless pipeline-compile exit has not reproduced in the isolated
  profile. See `docs/STATUS.md` "M8 web render fidelity" + docs/OPEN_ITEMS.md.
- REMAINING platform evidence: this gate has run on macOS arm64/Chrome 150 only.
  Other browser/OS/GPU combinations, actual keyboard/gamepad input, and the
  standalone Metal renderer still require their own runtime matrix. The ~256 MB
  wasm arena-alignment address cost is also reclaimable with a
  low-address-avoiding allocator.

### Manual test steps (for a full interactive check in a real browser)
1. `source ~/emsdk/emsdk_env.sh`
2. `emcmake cmake -S . -B build-web && cmake --build build-web -j`
3. `cp build-web/mdkr64_web.{js,wasm} dist/web/`
4. `cd dist/web && python3 -m http.server 8000`
5. Open `http://localhost:8000/` in Chrome/Edge 113+ (WebGPU). Pick your
   `baserom.us.v80.z64`, click Play. It boots + runs; input is keyboard (arrows
   steer, X=A, Z=B, C=Z, Enter=Start) or an SDL gamepad. Saves persist across
   reload (IDBFS).
