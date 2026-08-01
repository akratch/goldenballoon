# Fixes / patterns worth back-porting to mgb64

Running log of things discovered while building mdkr64 that would improve mgb64
(the sibling `mgb64` checkout). Each entry says what, where it lives here,
and why mgb64 would want it. Nothing here has been applied to mgb64 — it is a
read-only reference for this project; apply these there separately.

## 2026-07-26 — runtime domains must be established before lookup or branch joins

**Disposition: transferable boundary and test pattern; no claim that the named
DKR call sites exist in MGB64.**

MDKR closed six P1 findings in one wave (`05cff1f`): an owner was freed before
child teardown decisions, two locals crossed branch joins uninitialized, a
three-output path helper returned without writing and its callers divided by
zero, one allocated sound group was indexed as five, sound IDs treated a count
as an inclusive maximum, and special vehicle enums were multiplied directly
into a 30-row serialized audio table.

Carry these invariants into MGB64:

1. release owned children before their owner, or snapshot all teardown state and
   keep the owner live until the final free;
2. initialize every value consumed after a branch join at declaration;
3. output-parameter failure paths either write complete safe outputs or return
   status that every caller checks — prefer both at numerical boundaries;
4. table counts are exclusive upper bounds, and allocation, initialization,
   getters, setters, and asset-derived indices must share the same domain;
5. map gameplay enums explicitly into serialized-table enums before arithmetic;
   reject unknown values and validate the complete byte span first.

The reusable gate shape is two-layered: a ROM-free property test exhausts every
valid value, each exclusive bound, negatives, malformed spans, and non-finite
math; a production source census proves every real call site still uses the
helper. MDKR mutates each required source fragment in memory and requires the
census to fail, preventing a decorative static check from passing forever.

## M4.5 WebGPU backend vendoring — nothing to back-port; two notes
The WebGPU backend (gfx_webgpu*.c) was vendored from mgb64 **verbatim** and needed
**zero source edits** to run in mdkr64 — it is a clean drop-in at the
`GfxRenderingAPI` seam, exactly as designed. So there is no fix to send back. Two
observations for the mgb64 side:
1. **De-coupling opportunity (optional):** gfx_webgpu.c hard-references mgb64-shell
   symbols the standalone engine doesn't strictly need — the ImGui overlay
   (platformOverlayRender/WantsInput), the minimap overlay
   (minimap_overlay_draw_queued_frames_webgpu), the app-shell handoff
   (platformHasHostWebGpu + host getters), savedirPath (prewarm), and the
   screenshot-session globals (g_screenshotFrameSessionActive /
   g_autoScreenshotFrame / g_autoScreenshotGameTimer read by
   wgpu_readback_possible). mdkr64 satisfies all of them with one inert TU
   (platform/gfx_webgpu_stubs.c). If mgb64 ever wants gfx_webgpu.c to be a
   fully-portable module, these could move behind a thin
   `gfx_webgpu_hooks.h` weak-hook interface. Not urgent — the current coupling is
   harmless for a monorepo.
2. **Prewarm defaults ON (GE007_PIPECACHE default 1):** in a game that never calls
   gfx_webgpu_set_stage (mdkr64's DKR loop doesn't), the recorder is dormant
   (s_prewarm_cur_stage stays -1) so nothing is written — but confirm mgb64's own
   boss.c always sets the stage, else the cache silently never warms. mdkr64
   defers prewarm entirely (M4.5 plan).

## Strong candidates

1. **Arena lo32 pointer reconstruction (`dkr_lo32_to_ptr`)** — `platform/stubs_dkr.c`.
   mdkr64 allocates its RDRAM-stand-in arena at a single mmap/malloc region and
   reconstructs full host pointers from 32-bit-truncated DL words by re-attaching the
   arena's high bits (bounds-checked), falling back to the tag registry only for
   out-of-arena pointers. mgb64 uses only the `gfx_ptr` tagging table
   (`src/platform/gfx_ptr.h`), which requires `gfx_ptr_store()` discipline at every
   producer site and can leak/collide. The arena-first scheme is simpler, faster
   (no lookup), and covers every allocation the game makes; the registry stays as a
   fallback for statics/stack. Direct drop-in for mgb64's `osVirtualToPhysical`/DL
   resolution path since it also funnels everything through one resolver.

2. **Backend decoupling from `gfx_pc.h`** — `platform/gfx_pc.h` (shim) +
   `platform/fast3d/gfx_pc_dkr.h`. The vendored mgb64 GL/Metal backends `#include
   "../gfx_pc.h"` for `struct GfxDimensions`/`gfx_current_dimensions` only. In mgb64
   that header drags the whole GE front-end surface into every backend TU. Splitting
   the shared types into a tiny `gfx_dimensions.h` (as our shim proves works) makes
   the backends genuinely game-agnostic — this is exactly what made them verbatim-
   reusable here, minus two `#ifdef` patches (see item 3).

3. **GE-specific heuristics inside `gfx_opengl.c`** — two auto-VI-filter heuristics
   in mgb64's `gfx_opengl.c` are GoldenEye-content-specific but live in the "generic"
   backend (we had to NATIVE_PORT-gate them out; see mdkr64 commit `7573c40`).
   Upstream should move them behind a game hook/callback so the backend stays clean
   for reuse.

4. **`-fsyntax-only` per-TU standalone checks in agent/CI workflow** — every new
   platform file here compiles standalone with an 8-flag `cc -fsyntax-only` line
   (documented per file). Cheap guard against include-order rot; mgb64's platform
   files are not all standalone-compilable, which slowed reuse.

## To VERIFY in mgb64 (M3c renderer wave — texrect UV)

7. **TEXRECT s/t vs the G_TP_NONE `*0.5` texcoord halving.** mgb64's
   `gfx_apply_tile_uv_transform` (src/platform/fast3d/gfx_pc.c ~line 1354) halves
   u/v when `!(other_mode_h & G_TP_PERSP)` — correct for RSP-computed perspective-
   off GEOMETRY texcoords, but WRONG for TEXTURE RECTANGLE s/t, which are absolute
   RDP coordinates. In DKR this halved all text + the title logo (drawn as
   G_TP_NONE texrects) to 2x zoom; mdkr64 gates the `*0.5` with a `dkr_in_texrect`
   flag (gfx_pc_dkr.c). VERIFY whether any GoldenEye texrect is emitted in
   G_TP_NONE mode — if so, GE has the same latent halving bug and wants the same
   guard. (If GE only ever texrects in COPY/1CYCLE with G_TP default, it's a
   no-op there and no change is needed.)
8. **32-bit texture source stride (G_IM_SIZ_32b_LINE_BYTES == 2).** mdkr64 had to
   DOUBLE the tile line for RGBA32 decode because the N64 tile `line` counts only
   the low RG bank (2 bytes/texel) while the source buffer is contiguous 4-byte
   texels. If mgb64 decodes any 32-bit textures from contiguous host buffers,
   check its import path applies the same *2 (else 32-bit textures decode at half
   stride → banding). GE uses few/no 32-bit textures, so likely latent-only.

## Weaker / situational

5. **Audio heap sizing on LP64** — DKR needed `AUDIO_HEAP_SIZE × 4` because libaudio
   structs carry pointers (8B on LP64) and the N64-sized heap overflowed *silently*
   (alHeapAlloc has no failure path the game checks). If mgb64 ever sees mystery
   corruption near audio init, check its heap headroom; consider an alHeapAlloc
   overflow assert upstream (cheap, catches a whole class).

6. **PPM frame dump (`--dump-frames`)** — dependency-free P6 dumps after present
   (M3 agent, platform layer). mgb64 has `screenshot_series.c` (needs stb); the PPM
   path is a zero-dep fallback useful for headless CI verification.

9. **Wild-pointer (sign-extended-token) guard in `gfx_ptr` — SHARED CODE, real
   hazard.** mgb64's `gfx_ptr.h` `gfx_ptr_store` / `gfx_resolve_addr` are the direct
   ancestors of mdkr64's (same registry, same low-32 keying). mdkr64 hit an
   intermittent ASLR-dependent SIGSEGV whose mechanism is NOT DKR-specific: a
   game-code LP64 pointer truncation (`(s32) &array[i]`) sign-extended an in-heap
   host pointer whose low-32 had bit 31 set into a wild `0xffffffff........` value,
   then fed it to the `OS_K0_TO_PHYSICAL`/`osVirtualToPhysical` producer. Because
   that value is >4 GB and outside any known region, `gfx_ptr_store` happily
   REGISTERED it; a later `gfx_ptr_resolve` (keyed on the low-32) then handed the
   wild pointer straight to a DL consumer, which faulted. mgb64 is exposed to the
   exact same delivery path for any GE LP64 truncation that reaches a producer —
   and mgb64 is registry-ONLY (no arena reconstruction, see item 1), so it has no
   second line of defense. Cheap belt-and-suspenders, mechanically portable to
   mgb64's `gfx_ptr.h`: (a) in `gfx_ptr_store`, `if ((full >> 32) == 0xffffffffu)
   return;` — never register an all-ones-high value (user mappings never live
   there; it is always a sign-extended token); (b) validate every resolver result
   with a `plausible()` predicate (non-null && high-32 != 0xffffffff) and return
   NULL instead. mdkr64's fix: platform/gfx_ptr.h `gfx_ptr_store` guard +
   platform/fast3d/gfx_pc_dkr.c `dkr_ptr_plausible()` applied in `dkr_resolve` and
   the vertex/triangle/matrix consumers. (The TRUE fix always belongs at the
   truncation site — for DKR it was `s32`->`Vertex*` in tracks.c
   render_level_segment — but the guard makes the registry structurally incapable
   of delivering a wild pointer regardless of upstream bugs.)

## LP64 `sizeof(Gfx)` — a DESIGN DIFFERENCE (no back-port; documented to avoid a trap)

- **`Gsetcolor.color` is `unsigned long` in both projects' `PR/gbi.h`, but it is
  harmless in mgb64 and was a real bug in mdkr64 — because the two ports made
  OPPOSITE choices for the DL word width.** mgb64's `Gwords` is
  `{ uintptr_t w0; uintptr_t w1; }` (include/PR/gbi.h ~line 1362), so on LP64 a Gfx
  packet is 16 bytes BY DESIGN (it carries full 64-bit host pointers in the DL word)
  and every GE code path uses `sizeof(Gfx)==16` consistently. mdkr64 instead chose
  32-bit **tokenized** DL words (`Gwords { unsigned int w0, w1; }` + arena/registry
  resolution), so its Gfx must be 8 bytes — but the un-pinned `unsigned long color`
  inflated `sizeof(Gfx)` to 16, and DKR's `tex_load_sprite` reserves its DL region
  with a LITERAL 8-byte Gfx stride (`numTextures*0x20`) while advancing the writer
  by `sizeof(Gfx)`. The mismatch over-ran the sprite vertex region → billboard "red
  spikes". Fixed here by pinning `Gsetcolor.color` to 32-bit under NATIVE_PORT
  (mirrors this gbi.h's Mtx_t fix; see PR/gbi.h + gfx_pc_dkr.c "LP64 STRUCT-SIZE
  LOCKS"). **Back-port relevance:** none today (mgb64's 16-byte Gfx is internally
  consistent). BUT if mgb64 ever adds an ILP32/wasm32 target or switches to 32-bit
  tokenized DL words, it must apply the same `Gsetcolor.color` pin AND audit GE for
  any code that mixes a literal Gfx byte-stride with `sizeof(Gfx)` (the trap is the
  literal-vs-sizeof mismatch, not the `long` per se).

## M5 audio — CONVERGED on mgb64's engine (was an engine difference)

> **Updated:** mdkr64 no longer runs the stock SGI synthesiser. `platform/audio_compat.c`,
> `audio_event_queue.c` and `audio_fx_transfer.c` are ported from mgb64, and the 49
> decompiled `game/libultra/src/audio/**.c` files are deleted. The two projects now
> share one audio engine, so fixes flow BOTH ways — this section is kept because it
> records why the swap was needed and which DKR-specific behaviour had to be added
> on top. See "DKR EXTENSIONS" at the end of `platform/audio_compat.c`.
>
> Three integration points bit us and are worth back-porting as cautions:
> 1. **`ALSynConfig.fxType` is an ARRAY in DKR** (`fxType[2]`, one per aux bus) but a
>    scalar in mgb64. Reading it as a scalar silently compares an array address to the
>    enum, never matches `AL_FX_CUSTOM`, and falls back to bypass FX params — reverb is
>    allocated but has no delay line, so reverb-on and reverb-off output is identical.
> 2. **The allocator must stay arena-resident.** mgb64's `alHeapDBAlloc` calloc's and
>    ignores the ALHeap; DKR's `MIXER_RESOLVE`/`dkr_lo32_to_ptr` rebuilds every mixer
>    address from its low 32 bits, which is only valid inside the arena. It is now a
>    real bump allocator over the ALHeap again.
> 3. **`ALChanState_Custom` changes the chanState STRIDE.** DKR adds per-channel
>    `fade`/`unk11` volume lanes, so every access goes through the `CSP_CHAN()` cast.
> 4. **mgb64's envmixer applies a SQUARE-LAW volume curve; DKR's does not.**
>    `alEnvmixerParam` mapped volume as `(v*v)>>15` at both the START_VOICE and the
>    SET_VOLUME control sites — a perceptual-loudness curve. DKR's synthesiser applies
>    volume linearly (its "map volume non-linearly" step is the vestigial no-op
>    `(fVol+fVol)/2`). Inheriting the curve cost ~4 dB overall AND, because it is
>    quadratic, attenuated quiet voices far more than loud ones — which collapsed the
>    RAW16 bass against the rest of the mix and dropped the RAW16 fixed/legacy
>    divergence from 1304.9 to 183.0. If mgb64 wants that curve it must stay
>    GE-side; it is not a shared-engine default.

- **(historical) DKR ran the STOCK libultra synthesizer; mgb64 reimplements it.** Both share
  platform/mixer.c (the aspMain software mixer). But mgb64's `audio_compat.c`
  reimplements alAudioFrame / alSynStartVoice / the bank parser with REAL host
  pointers, so the mixer receives real pointers. mdkr64 keeps the decompiled
  libultra synth (env/resample/reverb/load/synthesizer/…), whose entire address
  ABI is 32-bit — `osVirtualToPhysical`→u32, the `ALDMAproc` return→s32,
  `ALSave.dramout`→`(s32)param`, `K0_TO_PHYS`→`&0x1FFFFFFF`. So on LP64 every
  buffer/state/sample address reaches the mixer truncated. mdkr64's fix (all audio
  memory arena-resident + reconstruct each mixer address from its low-32 via
  `dkr_lo32_to_ptr`, in `platform/mixer.h` MIXER_RESOLVE) is the reconstruction
  path this project already uses for gfx. **Back-port relevance:** superseded — mdkr64 adopted mgb64's
  audio path, so the two share one engine. The reconstruction-in-the-macro pattern
  still applies to any port that keeps a stock-libultra synth, and mdkr64 still
  relies on it because its audio memory remains arena-resident.
- **`ALParam` free-list slot must cover the largest "alternate view" variant on
  LP64 — a latent libaudio bug in ANY port that keeps the stock synth.**
  `__allocParam()` returns `sizeof(ALParam)` blocks that callers cast to
  `ALStartParam`/`ALStartParamAlt`. Those embed 8-byte pointers, so
  `ALStartParamAlt` is 40 bytes vs `ALParam`'s 32 on LP64 (all 28 on N64) —
  `alSynStartVoiceParams` writing `update->wave` overruns the block and corrupts
  the next pooled param's `next`. mdkr64 pads `ALParam` (synthInternals.h) with a
  `_Static_assert` lock. If mgb64 (or any port) ever compiles the stock
  `synthesizer.c`/`syn*` on a 64-bit target, it needs the same size lock.
- **`mixer.c` include order on macOS:** put `<ultra64.h>` FIRST (before
  `<string.h>`), else the system `_FORTIFY_SOURCE` macros for `bcopy/bcmp/bzero`
  break `os_libc.h`'s function declarations of the same names. mdkr64 reordered
  the vendored mixer.c's includes; mgb64 may hit the same on a fresh macOS toolchain.

## Not applicable to mgb64 (noted to avoid confusion)

- The ROM-overlaid-struct `dkrptr32` token scheme (architecture decision 8): GE's decomp
  reads ROM data through explicit accessors/offset tables rather than struct
  overlays, so mgb64 doesn't have this class of bug.
- `asset_swap.c`: DKR's typed asset LUT is game-specific; GE's per-file-table
  swapping already exists in mgb64's loaders.

## M4 input — nothing to back-port (mdkr64 was catching up to mgb64)

- The "menus never advance" bug was mdkr64's stub `osContStartReadData` being a
  no-op. mgb64 already does the right thing (`stubs.c`: `osSendMesg(mq, NULL,
  OS_MESG_NOBLOCK)` — post the SI-completion so the game's non-blocking recv on
  the SI queue succeeds and `osContGetReadData` runs). mdkr64 now matches it.
  No backflow; noted so the divergence isn't reintroduced.
- mdkr64's SDL->OSContPad mapping + `--input-script` (platform_sdl_min.c) mirror
  mgb64's platform_sdl/stubs input design (game's own N64 button bits, event
  pump before the retrace). Nothing new for mgb64.

## M6 race path — transferable LP64 patterns (audit mgb64 for these)

These are general 64-bit-host hazards found while bringing up the DKR race. The
specific sites are DKR's, but the *shapes* are decomp-wide — worth grepping GE for.

- **Array-of-structs scan terminated by a zeroed trailing word.** DKR's
  texrect_draw* iterate `for (i=0; (p=element[i].texture); i++)` over a caller's
  struct, relying on a following struct whose leading pointer field reads zero as
  the sentinel. The caller allocates `{ RealStruct; s32 terminator; }` and zeroes
  the s32. On LP64 the struct's own pointer member inflates 4->8, so the sentinel
  entry's pointer field is 8 bytes but the terminator slot is only 4 — the upper
  half is uninitialised stack, yielding an ASLR-dependent wild pointer past the
  "single" element. Fix: make the terminator slot pointer-width (or `{0}`-init the
  whole wrapper). If GE has any "list terminated by a trailing zero word" pattern
  where the element struct contains a pointer, it has this bug latent.

- **Allocation sized with a hardcoded N64 sizeof + `count*4` pointer array.** DKR's
  obj_spawn_attachment did `objSize = numIds*4 + 0x80` then laid a pointer array at
  `&object[1]`. On LP64 both the struct size and the per-slot pointer width grow,
  so the allocation under-sizes and the trailing array spills into the next object.
  Fix: `sizeof(T) + count*sizeof(void*)`. Grep GE for `+ 0x`-style struct sizes and
  `* 4`/`* 8` array sizings feeding an allocator.

- **Pointer arithmetic through `(s32)`/`(u32)`.** Same class already noted from the
  M4-fix wave; M6 hit three more (menu wood-panel geometry, the void-mesh aligner,
  a track-name char* packed into an s32). On a >4GB arena the cast truncates and,
  when signed, sign-extends bit-31 into 0xffffffff.. — ASLR-dependent. Always do
  pointer arithmetic/alignment through uintptr_t. GE's gfx/heap code likely has
  analogues; the fix is mechanical and safe on both widths.

## Systematic pointer-truncation sweep (robust: wave) — audit mgb64 for these shapes

The M4/M6 waves fixed truncation sites one boot path at a time. This wave
enumerated the WHOLE class off the compiler warnings and found three shapes that
are not DKR-specific and that mgb64's registry-only resolver (no arena
reconstruction, see item 1) has no backstop for:

- **Alignment macros that cast the pointer through `(s32)`/`(u32)`.** DKR's
  `FBALIGN` (`((u16*)(((s32)(a)+0x3F)&~0x3F))`) and `_ALIGN16`
  (`(((u32)(a)&~0xF)+0x10)`) truncate a 64-bit host pointer whenever they are
  applied to a POINTER (they are fine on a size). The framebuffer one also fed a
  later `mempool_free()` of the truncated pointer (latent free-of-wild on video
  mode change). Fix: align through `uintptr_t` (identical bits on N64). GE almost
  certainly has framebuffer/heap alignment macros of the same form — grep its
  headers for `& ~0x` / `+ 0x3F`-style aligners that take a pointer and cast
  through a 32-bit int.

- **A real runtime-pointer struct field truncated through `(s32)` at the CALL
  site of a callee that dereferences it.** DKR stops racer engine sounds with
  `sndp_stop((SoundHandle)(s32) racer->unk10)` — `unk10` is a real
  `ALSoundState*`, `sndp_stop` writes `state->flags`. The `(s32)` is a decomp
  "cast required to match" artifact; on LP64 it sign-extends the handle to a wild
  pointer. Any GE audio-handle / object-handle "stop"/"free" that casts a real
  pointer field through `(s32)`/`(u32)` before handing it to a dereferencing
  callee has this bug. The fix is just to drop the truncation (`(uintptr_t)` or
  pass the field directly); the field is already the right type.

- **A pointer stashed in a `(s32)` LOCAL, then cast back and dereferenced.** DKR's
  finish-challenge did `s32 camera = (s32) get_active_camera(); ((Camera*)camera)
  ->mode = ...`. On LP64 the local must be `uintptr_t` (or the real pointer type).
  Grep GE for `s32`/`u32` locals assigned from a pointer-returning call and later
  cast back to a pointer.

- **A native stand-in for an N64 LINKER SYMBOL declared as a pointer VARIABLE.**
  DKR's `__ROM_END` is `extern u8 __ROM_END[]` on N64 (so `&__ROM_END` == the ROM
  end address); the native port declared it `u8 *__ROM_END`, so `(s32)&__ROM_END`
  is the host address of the variable — garbage that drove an unbounded ROM-buffer
  scan. If mgb64 replaces any `u8 foo[]` linker-bound symbol with a `u8 *foo`
  variable, every `&foo` / `(intptr)&foo` use in game code silently changes
  meaning. Prefer `u8 foo[1]` (array) or audit the `&foo` sites.
## Frame pacing — a DIFFERENCE worth understanding (mgb64 is the reference here)

mgb64's cooperative VI pacing was the model for mdkr64's frame-pacing fix, and the
comparison is instructive because the two engines drive their logic timestep
DIFFERENTLY — do NOT copy mdkr64's retrace-field synthesis into mgb64:

- **GE (mgb64) runs a FIXED-60 Hz integer-tick sim.** platformFrameSync
  (src/platform/platform_sdl.c ~4234) paces to an absolute 1/60 s deadline and
  posts exactly ONE retrace to each scheduler client per frame
  (`osSendMesg(client->msgQ, &retraceMsg, OS_MESG_NOBLOCK)`). GE never varies a
  per-frame "update rate" from the retrace count; when the renderer can't keep up
  it substeps physics from a separate wall-clock timer (g_ClockTimer), and the
  1/60 s floor is what keeps a fast / high-refresh path from outrunning 60 Hz
  (see the platform_sdl.c comment at the FrameCap "display" case ~2271-2282).

- **DKR (mdkr64) instead reads the retrace COUNT as its timestep multiplier.**
  fb_update (game/src/video.c) drains the video queue and returns the number of
  60 Hz fields elapsed as `updateRate`, which scales movement, physics AND the
  race clock (racer.c:4382). So DKR *needs* multiple retraces per slow frame; a
  one-retrace-per-frame model (mgb64's) would pin updateRate at 1 and desync
  motion from wall time (the slow-motion bug this wave fixed). mdkr64's
  `platform_vi_pace_measure` (platform/platform_sdl_min.c) therefore keeps mgb64's
  1/60 s floor but ALSO makes `floor(elapsed / field)` retraces available per
  present so the drain measures the true field delta.

  Backflow value for mgb64: none to apply, but the 1/60 s software floor +
  drift-free absolute-deadline accumulator is the shared correctness primitive,
  and mgb64 already has it. If mgb64 ever adds a variable-rate (frameskip-
  compensated) mode, mdkr64's field-budget recv is the reference. The KEY shared
  lesson: tie the logic timestep to WALL-CLOCK elapsed, never to the display
  vblank count — otherwise a 120 Hz ProMotion panel runs the sim ~2x fast. Both
  ports now do this; keep it that way.

## Not applicable to mgb64 (M6)

- The collision-candidate arena-offset+tag repacking, the ASSET_MISC per-index
  float byte-swap, and the collision-facet swap are all DKR-asset-specific
  (GE has no equivalent ROM-overlaid collision/misc blobs). Noted so they aren't
  mistaken for general fixes.

## Fixed-point trig amplitude convention (M-race)

- **Verify any native `sins_s16`/`coss_s16`-style helper returns amplitude
  0x10000, not 0x7FFF.** In mdkr64 the WEAK bring-up stub
  (`platform/math_stubs_temp.c`) scaled `sinf()` by `32767.0f`, which halved every
  sine/cosine because the decomp's callers assume 1.0 maps to **0x10000** — matrix
  builders do `sins_s16(a) * (1.0f/0x10000)` and integer callers do
  `(sins_s16(a) * v) >> 16`. The real N64 routine reads a u16 sine table and
  `sll v0,1` (×2) it (`src/hasm/ido/math_util.s`), i.e. peak 0x8000×2 = 0x10000.
  The ×0.5 error made every rotation matrix's 3×3 ≈0.5× (cos·cos products 0.25×) —
  which shrank rendered models to ~¼-size AND, because DKR routes racer velocity
  through the same `mtxf_from_inverse_transform`/`mtxf_transform_point`, throttled
  movement to a constant crawl. GE's port uses the same fixed-point angle
  convention (0x10000 == full turn); if its native trig helpers are approximated
  the same way, confirm the amplitude is 0x10000 or the whole world silently
  renders/moves at half scale. One-line fix: `sinf(x) * 65536.0f`.

## Latent N64 array-overread UB: find it with ASan once the subsystem runs (M-race)

- A general porting hazard, not DKR-specific: decompiled N64 code frequently
  reads/writes ONE element past a fixed-size stack/global array, relying on the
  specific N64 stack/global layout to make the extra slot benign (e.g. a
  loop that fills `arr[0..4]` of an `arr[4]`; a bare `ObjectTransform` cast to the
  larger `Object` so a downstream `obj->animFrame` read runs off the end; a
  1-indexed lookup into a 0-indexed table's last+1 slot). On an LP64 host these
  land on the stack canary / ASan redzone / an unrelated global and crash
  **intermittently** — and only once the relevant subsystem is actually exercised
  (in DKR they stayed dormant until a movement bug was fixed and racers began to
  drive/rank/boost). Lesson for GE: run the ASan build (`-fsanitize=address`)
  through newly-reachable gameplay, and fix each flagged overread at the array
  (size it to the real access extent, or clamp the index) NATIVE_PORT-gated so the
  matching build is untouched. Cheap, deterministic, and catches the class before
  it becomes a flaky field crash.

## Scalar character passed as a C string: audit decomp declarations, not adjacency

- **What happened.** MDKR64 declared `gCurFilenameCharBeingDrawn` as one `char`
  and passed its address to the font renderer, which scans to a NUL. Every DKR ROM
  map sizes the symbol at four bytes: the decomp lost three semantic zero bytes
  that made it a valid one-character string. ASan aborts deterministically on the
  new-save filename grid; a normal build merely reads whatever global follows.
- **Why MGB64 should sweep it.** This shape is generic to decompilation: a
  data-symbol declaration inferred from its first meaningful byte can omit
  padding that is actually an array terminator. Search for addresses of scalar
  `char`/`u8` objects passed to font, `strlen`, logging, path, formatting, or other
  string APIs. Do not accept "the next global is zero" as an invariant; native and
  wasm linkers need not preserve ROM adjacency.
- **Repair and gate.** Prefer the size proven by the ROM symbol map, zero-initialize
  the whole buffer, write element zero, and keep element one explicitly NUL. Lock
  the size with `_Static_assert`. Drive the real UI under ASan and retain a
  broken-direction control that requires the exact symbol and scanner site, so a
  route that no longer reaches the screen cannot pass vacuously.

## Wasm call ABI: namespace libc-like decomp symbols and make warnings fatal

- **What happened.** DKR carried a global `f32 log(f32)` approximation. SDL/libc
  also contributes `double log(double)`, so wasm-ld saw the same symbol with two
  incompatible WebAssembly function types. Separately, bounds probes were called
  without declarations, giving callers C's legacy implicit-`int` contract while
  the definition returned `void`.
- **Why MGB64 should sweep it.** A decomp's original symbol namespace predates a
  host libc, and wasm function signatures encode parameter and return types more
  rigidly than a native calling convention may appear to. Search global game
  symbols named like libc/math functions, and reject every implicit declaration.
  A link that emits a signature warning must not be treated as a successful
  browser artifact.
- **Durable gate.** MDKR64 enables
  `-Werror=implicit-function-declaration` after its broad decomp warning
  suppression and passes `-Wl,--fatal-warnings` to wasm-ld. The first clean run
  found additional undeclared trace/libc calls and browser code referencing a
  desktop-only function after an unconditional return. MGB64 should adopt the
  same two gates, namespace collisions only in port builds when matching symbols
  must be retained, and clean-build every translation unit rather than trusting
  an incremental link.

## LP64 class: an allocation sized for N64 pointers backing an array of HOST pointers (objhdr wave)

- **What.** `mdkr64 game/src/objects.c allocate_object_pools()` allocated the
  loaded-object-header table as `count * 4` bytes while the table's element type
  is a real `ObjectHeader *` — 4 bytes on N64, **8** on a 64-bit host. Every index
  past `count/2` wrote its pointer outside the allocation, straight over the
  refcount array that the allocator had placed immediately after it. The visible
  symptom was maximally misleading: a corrupted refcount byte made the loader take
  its "already loaded" fast path and hand back a table slot that had never been
  written, so the caller saw *"a freshly-loaded asset header that reads back all
  zeros"* and everyone (including two debugging passes) hunted a use-after-free or
  a pool overrun that did not exist.
- **Why mgb64 wants it.** This is a whole class, not one bug: **any** N64 decomp
  allocation whose size is a hardcoded `n * 4` (or `n * sizeof(s32)`) but whose
  *storage type* is a host pointer is silently a 2x heap overrun on LP64. Grep for
  it directly — `mempool_alloc*`/`malloc` sites with a literal `* 4`, then check
  the declared type of the variable being assigned. The fix is mechanical and
  target-neutral: use `sizeof(T *)`, which is 4 on N64, so the matching build is
  byte-identical. mdkr64 had already converted several such sites to
  `sizeof(uintptr_t) * N`; this one was missed because the multiplier was a bare
  `4` rather than a `sizeof`.
- **Why the sanitizers miss it.** The overrun is a plain write inside one large
  game-owned arena/pool, so there is no ASan redzone anywhere near it and no
  malloc metadata to corrupt. It only ever manifests as another *game* structure
  changing value. Sanitizers will not find this class — a size audit will.

## LP64 class: N64 byte budgets for pools that hold pointer-bearing structs (objhdr wave)

- **What.** DKR's object sub-pool is a fixed `0x15800`-byte budget tuned for N64.
  Everything in it (the `Object` struct plus its variable-length trailing payload,
  and the object headers) is built from real host pointers, so on LP64 the same
  scene needs roughly **twice** the bytes. It survived normal play and only blew up
  on the most object-dense level reached (the attract demo's second track), where
  `spawn_object()` returned NULL and the original code — which assumes spawning a
  racer cannot fail — dereferenced it.
- **Why mgb64 wants it.** Same shape wherever a port keeps an N64-sized fixed
  budget for structures that grew under LP64. Two lessons: (1) scale the budget by
  `sizeof(void *) / 4` rather than picking a magic multiplier, so it is exactly the
  original constant on N64; (2) **measure**, don't guess — a peak-usage probe
  (walk the pool's slot list on each successful allocation, track max used bytes
  and max used blocks) turns "is the pool big enough?" into a number, and also
  distinguishes byte exhaustion from slot exhaustion, which have different fixes.
  A per-`level_load` probe additionally separates *a leak* (usage grows across
  loads) from *genuine demand* (usage returns to zero, one level is just bigger).

## Freed handles left in a lookup table (asset/header caches)

- **What.** DKR's `try_free_object_header()` frees the header but leaves the
  pointer in `gLoadedObjectHeaders[index]`; only the separate refcount byte gates
  the "already loaded" path. On real hardware that is benign. On a port it means a
  **single** corrupted refcount byte upgrades itself from "reload an asset" into a
  use-after-free — and the table is exactly the kind of thing a neighbouring LP64
  overrun will splatter.
- **Why mgb64 wants it.** Cheap, target-neutral hardening for any refcounted
  asset/header cache carried over from a decomp: null the slot on free, and assert
  loudly on the impossible combination (`refcount != 0` with a NULL slot) instead
  of returning it. It costs nothing at runtime and converts a class of
  silent-garbage bugs into an immediate, located abort. Corollary: resist the
  temptation to "contain" such a symptom with a defensive NULL check at the
  *consumer* (mdkr64 had one in `spawn_object()`); it hides the corruption and
  moves the crash somewhere unrelated. Guard loudly, at the source of the
  invariant, or not at all.

## Polymorphic on-disk records cannot be byteswapped by a generic asset swapper

- **What.** DKR's level object map is an array of variable-stride entries: a common
  header (`objectID`, `size`, `x/y/z`) followed by **behaviour-specific** body
  params whose field layout depends on the object's behaviour — which is only
  knowable by resolving `objectID` through a translation table into an object
  header. mdkr64's generic byteswapper could only handle the common prefix and
  punted everything past it, leaving all body params big-endian. The failure was
  silent and remote: one `s16 objectIdToSpawn` read as `0xB200` instead of `0x00B2`,
  so an animation director spawned the wrong target, so the camera-animation object
  never existed, so the frontend camera sat at its `cam_init()` default and the
  title screen rendered open ocean instead of Timber's Island. Nothing logged an
  error anywhere in that chain.
- **Why mgb64 wants it.** Any format with a tagged/polymorphic record body has this
  property, and the generic "swap the header, punt the body" approach is a
  correctness hole that looks like a rendering or gameplay bug, never like an
  endianness bug. The workable pattern (see `mdkr_objmap_swap_bodies()` /
  `mdkr_objmap_swap_entry_body()` in `game/src/objects.c`): resolve each record's
  type **exactly the way the game's own consumer resolves it**, dispatch on that to
  a per-type field list, swap each field **only when it fits inside that record's
  real stride** (so short records are never over-read), and **cache the
  type-resolution** — the resolver here decompresses an asset on a miss, so a
  naive per-record lookup made level loads pathologically slow. Two diagnostics
  that pay off: log the post-swap value of a known field against its expected
  range, and treat "a field that is plausible when byteswapped and absurd when not"
  as proof rather than a coincidence.

## libultra sequence player: an exhausted event queue becomes an infinite loop

- **What.** `alEvtqPostEvent` (`libultra/src/audio/event.c`) silently drops an
  event when its free list is empty, and `alEvtqNextEvent` reports an empty queue
  by returning `evt->type = -1, delta = 0`. `__CSPVoiceHandler`
  (`audio/mips1/csplayer.c`) is a `do { switch (type) … } while (nextDelta == 0);`
  with no case for -1, so an empty queue spins that loop forever. Because the
  player is *self-perpetuating* (each handled event posts its successor), a single
  dropped event under a transient burst breaks the chain, the queue drains, and
  the game wedges at 100% CPU with the game loop otherwise healthy.
- **Why mgb64 wants it.** Any project vendoring libultra's audio has this exact
  code. Two general lessons: (1) a fixed-size resource pool tuned for N64 is a
  *hang* risk, not just a dropout risk, when the consumer's termination condition
  depends on the pool never being empty — audit vendored `do/while` loops whose
  exit condition is "the queue gave me something"; (2) the failure looks nothing
  like audio — it presents as the whole game freezing, and only `sample`/`perf` on
  the wedged process points at the audio thread. Worth an unconditional
  belt-and-braces guard in any port: make the empty-queue case return a nonzero
  delta so the handler yields and is retried, and log the dropped post, rather than
  letting an out-of-resource condition turn into an unrecoverable spin.
- **Testing note that generalises.** The bug is only reachable on a long *idle*
  path (an attract/demo mode), which no input-driven fixture exercises. Soak the
  idle path as its own regression test.
- **CONFIRMED PRESENT IN mgb64 — verified by reading its tree, not inferred.**
  mgb64 reimplements the audio path rather than vendoring libultra verbatim, and
  the reimplementation reproduces the hazard exactly:
  - `src/platform/audio_compat.c:2450` `alEvtqNextEvent()` — empty queue sets
    `evt->type = -1` and returns `delta = 0`. Same sentinel, same contract.
  - `src/platform/audio_compat.c:2477` `alEvtqPostEvent()` — `if (item == NULL) {
    osSetIntMask(mask); return; }`. Silent drop, no counter, no log. mgb64 is
    *quieter* than stock libultra here: it dropped SGI's `_DEBUG`
    `__osError(ERR_ALEVENTNOFREE)` as well.
  - `src/platform/audio_compat.c:3963` sequence-player handler —
    `do { switch (nextEvent.type) { … default: break; } … } while (nextDelta == 0);`.
    The `default: break;` makes the spin *guaranteed*: the sentinel matches
    `default`, does nothing, and the loop re-runs with the queue still empty.
  - `src/snd.c:1569` `sndPlayerVoiceHandler()` — same shape, and its `default:`
    arm hands the sentinel to `sndHandleEvent()` as though it were a real event.
  - `src/platform/audio_compat.c:2278` — `client->samplesLeft +=
    native_time_to_samples_no_round(handler(client))`. So returning 0 from a
    handler is not a fix either; it relocates the spin into the driver's loop.
    Any guard must return a **nonzero** delta.
- **The fix as applied here**, if mgb64 wants to copy it: in each handler, right
  after `alEvtqNextEvent()`, test for the sentinel; on a hit, re-arm the player's
  own self-perpetuating API heartbeat (`AL_SEQP_API_EVT` / `AL_SNDP_API_EVT`) and
  `break` with `nextDelta = frameTime`. The player idles one frame and stays
  responsive instead of hanging; the worst case is that the sequence in flight
  stops. See `game/libultra/src/audio/mips1/csplayer.c` and `game/src/audiosfx.c`
  here. Pair it with a drop counter in `alEvtqPostEvent` — the silence is what
  made this expensive to find — and with a `MDKR_EVTQ_STATS`-style high-water-mark
  probe, which is what turned "raise the queue?" into a measurement (here: real
  demand 121 against a budget of 120, so the N64 number was one short, and
  nothing was running away).

## LP64 class: an on-disk record stride that no longer equals `sizeof(struct)` (boostfx wave)

- **What.** DKR's boost/exhaust graphics table (`ASSET_MISC` sub-asset 20) is an
  array of 10 `Object_Boost` records. On disk the stride is **0x80**. The C struct
  in `game/include/structs.h` mirrors the record exactly — but its last two fields
  are runtime *pointers* (`Sprite *sprite` at 0x78, `TextureHeader *tex` at 0x7C),
  so on an LP64 host `sizeof(Object_Boost)` is **0x88**. The nine call sites all do
  `((Object_Boost *) get_misc_asset(ASSET_MISC_20))[i]`, which is correct on the
  N64 and off by `i * 8` bytes here. Every field of every entry after entry 0 was
  garbage. (Compounded by the record still being big-endian, since `ASSET_MISC` is
  heterogeneous and is punted by the generic swapper — see the section above.)
- **Why this shape is generic.** It appears wherever an on-disk record contains a
  field the game later overwrites with a pointer — a very common N64 idiom
  (`0` in ROM, patched to a loaded resource at runtime). The struct then serves two
  roles at once: on-disk layout AND runtime state. On the N64 those roles have the
  same size; on LP64 they diverge, silently. Grep shape: **a struct that is both
  cast onto raw asset bytes and has a pointer member.** Related but distinct from
  the two entries above (`allocation sized for N64 pointers`,
  `N64 byte budgets for pointer-bearing pools`) — those get the *allocation* wrong;
  this one gets the *indexing* wrong even when the allocation is fine.
- **The fix pattern.** You cannot expand the record in place (it lives inside a
  larger, tightly packed section blob), and you must not index it in place. Convert
  once, at first use, into a caller-owned native array:
  1. a **per-field** byteswap, not a blanket 32-bit word swap — this record packs
     `s16 spriteId; s16 textureId` at 0x6C and four `u8`/`s8` at 0x70, which a word
     swap would scramble (`asset_swap_misc_boost()` in `platform/asset_swap.c`);
  2. a **host-layout copy**: `memcpy` the prefix that is offset-identical
     (everything before the first pointer field), zero the pointer fields, and
     write at the *host* stride;
  3. a single accessor that every consumer goes through
     (`GET_BOOST_TABLE()` / `dkr_boost_table()` in `game/src/objects.{c,h}`), so a
     future call site cannot re-introduce the raw indexing;
  4. build it **exactly once** per section load — the game writes runtime state back
     into that array, so a rebuild would silently reset animation state; and
  5. `_Static_assert` the on-disk stride and every field offset through the first
     pointer field, so a struct edit breaks the build instead of the graphics.
- **Diagnostic that identifies it fast.** Decoded small integer IDs that look like
  *halves of neighbouring float words* (here: `12032, 0, 0, -16319, -32705, 66,
  16451, 0, 16450, 0` — `0x2F00`, `0xC001`, `0x8080`… fragments of `41c00000`,
  `42800000`, `3f800000`) mean the read is landing at the wrong offset, not that
  the values are merely byteswapped. A byteswap error alone permutes bytes within a
  field; a stride error mixes bytes *across* fields.

## A big-endian float read natively is a DENORMAL, not garbage — it hides until a divide (racerfix wave)

- **What.** DKR's `ASSET_MISC` section is a bag of unrelated sub-assets (byte arrays,
  s16 id lists, structs, float tables) and is therefore punted by the generic asset
  swapper. Sub-asset 8 is an `f32[10]` of per-character steering divisors; nothing
  byte-swapped it. Read natively on a little-endian host, `600.0f` (`0x44160000`)
  becomes `0x00001644` = **7.99e-42** — a denormal, not a NaN and not an absurd
  magnitude.
- **Why that matters.** Denormals behave like `0.0` in almost every arithmetic
  context, so a completely wrong table produces *plausible* results everywhere it is
  merely multiplied or added. It only detonates where the value is a **divisor**:

  ```c
  racer->lateral_velocity += (racer->velocity * gCurrentStickX) / miscAsset[characterId];
  ```

  While `gCurrentStickX == 0` the numerator is `-0.0` and the quotient is `-0.0` —
  no symptom at all through the whole start of the race. The instant the stick
  deflects at speed, `-11.97 * 70 / 7.99e-42` overflows f32 to `-inf`, and one frame
  later the player object is at `(-inf, y, -inf)`.
- **The generalisable rules.**
  1. **A denormal in a decoded float is proof of a byte-order error**, the same way
     "plausible when byteswapped, absurd when not" is. Add it to the diagnostic
     vocabulary: dump the raw `u32`s next to the interpreted floats — `00001644`
     against `44160000` is instantly legible; `7.99e-42` alone looks like "roughly
     zero, probably fine".
  0. **DO NOT "clean up" mgb64's synthetic COUNTER** (`src/platform/stubs.c:335`:
     `g_deterministic` / `s_syntheticCount` / `SYNTHETIC_TICKS_PER_FRAME`, and
     `randomSetSeed(0x12345678)` in `boss.c:405`). It looks like scaffolding. It is
     load-bearing, and mdkr64 proved it the expensive way: mdkr64's `osGetCount()`
     returned the host monotonic clock, and because a headless frame loop runs at
     whatever rate the machine allows while the pacer synthesises a fixed field
     count per frame, the COUNTER advanced by a load-dependent amount per simulated
     frame. Any game code integrating COUNTER deltas then diverged run to run —
     mdkr64's menu/character animation did, giving 10 distinct images in 10 runs
     (median 18.9 % of pixels) and silently invalidating every frame-comparison
     check it had. mgb64 is *more* exposed than mdkr64 was, because it seeds its
     RNG from the counter (`random.c:350`), so the divergence would not be confined
     to animation phase. This is the one item in this document that flows the other
     way: mgb64 had the right design and the mdkr64 port layer failed to carry it
     over. (See docs/OPEN_ITEMS.md "wave determinism".)
  2. **Enumerate the sub-assets once, in one place, at load** — do not normalize
     lazily at whichever call site someone happened to debug. Every table nobody had
     looked at stayed big-endian for three waves precisely because the mechanism was
     opt-in per consumer. (`dkr_misc_normalize_tables()` in `game/src/objects.c`
     holds one list per swap kind: 32-bit words, 16-bit halfwords, and per-field
     record swizzles.) The kinds are not interchangeable — a word swap on a 16-bit
     array *transposes the two halfwords inside every word*, and a halfword swap on a
     float array byte-reverses each half of it — so the list a sub-asset belongs to
     is part of its classification, and belonging to the wrong one is silent.
  2b. **Bound every normalized table, and verify the bound by removing the swap.**
     Several of these tables sit on paths no test fixture reaches, where a green
     matrix says nothing. A plausibility bound that the correct decode clears easily
     and the byte-reversed decode violates outright (`dkr_misc_verify_tables()`) is
     the only thing standing between a wrong swap and a silent wrong table — and it
     is only trustworthy once you have watched it fire with the swap disabled.
  3. **Grep for divides by asset-derived values** when auditing a punted asset
     section. That is where an endianness bug stops being cosmetic.
- **How the failure presented (why it is worth recognising).** Not a crash, not a
  NaN warning: the player left the world, the segment/BSP walk found nothing visible,
  and the game drew a flat fog field with the HUD still on it while the race clock
  kept counting. Every headless fixture still exited 0. Two cheap countermeasures,
  both worth having in mgb64:
  - assert the invariant **loudly** where it is cheap — a non-finite player position
    now prints `[FATAL] …` and aborts; and
  - make at least one fixture assert on **semantics, not survival** — traced spline
    progress (checkpoint index, lap) plus a "is the frame still a real scene?" metric
    (distinct quantized colours + luma sigma over a centre crop that excludes the
    HUD). Measured here: 59 colours / sigma 5.9 when the world stopped drawing,
    620–2720 / 22–47 when healthy. See `tests/check_race_drive.py`.
## ALFx delay line: the u32 back-tap, and the mixer's 8-byte DMA tail

Two separate hazards in the same routine. mgb64 is **clear on the first** and
**SUSPECTED exposed on the second**. Both verified by reading the mgb64 tree
(the sibling `mgb64` checkout, `src/platform/audio_compat.c`), not
inferred.

### 1. `&fx->input[-delay->input]` with a u32 offset — NOT PRESENT in mgb64
`ALDelay.input` / `.output` are `u32` *backwards* offsets into the delay line
(`src/libultra/audio/synthInternals.h:218-230` there, identical to ours). Stock
libultra taps the line with `&r->input[-d->input]`. On the N64 the unsigned
negation wraps mod 2^32 against a 32-bit pointer and lands N samples *below*
`r->input`; on LP64 the array subscript **zero-extends** it and the tap lands
~8 GiB *above*. Everything downstream then misbehaves — the `curr_ptr < base`
correction never fires, the wrap branch is always taken, and the split length
`before_end` (thousands of samples) is used as the transfer count instead of
`count` (measured here: 160 → 6400 samples, a 12800-byte DMA). The DRAM address
survives (it is truncated to 32 bits by `osVirtualToPhysical` and rebuilt, which
restores the modular arithmetic), so the write starts in the right place and
runs off the end of the delay line into the `ALLowPass`/`ALResampler` structs
allocated right after it. Cost us a SIGSEGV in `_filterBuffer` and a
hardware-watchpoint hunt; see docs/OPEN_ITEMS.md "M5 open items".

mgb64 already casts at all four tap sites and is therefore safe:
- `src/platform/audio_compat.c:1780` — `&fx->input[-((s32)delay->output - delay->rsdelta)]`
- `src/platform/audio_compat.c:1794` — `&fx->input[-(s32)delay->output]`
- `src/platform/audio_compat.c:1853` — `&fx->input[-(s32)delay->input]`
- `src/platform/audio_compat.c:1854` — `&fx->input[-(s32)delay->output]`

Recorded here anyway because **the casts look cosmetic and are load-bearing**:
anyone "tidying" `-(s32)delay->output` back to `-delay->output` reintroduces a
silent, ASLR-dependent heap smash that ASan cannot see (single arena allocation,
no redzone). Worth a comment at those four lines in mgb64. mgb64 keeps the same
`previous_output = &fx->input[delay->output]` positive subscript
(`audio_compat.c:1890`) — correct, that one is positive on the N64 too.

### 2. Delay-line allocation has no slack for `ROUND_UP_8` — SUSPECTED in mgb64
`platform/mixer.c` (both projects, same vendored file) emulates the RSP's 8-byte
DMA granularity: `mixerSaveBuffer`/`mixerLoadBuffer` move `ROUND_UP_8(sb_count)`
bytes, **not** `sb_count` (mgb64 `src/platform/mixer.c:279,289,310`). The reverb
helpers legitimately issue transfers whose DRAM range ends *exactly* at
`&fx->base[fx->length]` — the wrap split writes `before_end` samples ending on
the boundary (mgb64 `src/platform/audio_compat.c:1712-1718`) — and the chorus
path issues deliberately odd `count + ram_align` loads
(`src/platform/audio_compat.c:1782-1783`, `ram_align` ∈ 0..3). But
`alFxNew` allocates the line with no tail:

    src/platform/audio_compat.c:2000
        fx->base = alHeapAlloc(heap, length, sizeof(*fx->base));

So whenever a transfer's byte count is not a multiple of 8, up to **6 bytes are
written/read past `fx->base`'s allocation** — and the very next `alHeapAlloc`
calls in the same function hand out `delay->rs`, `delay->rs->state`, `delay->lp`
and `delay->lp->fstate`, i.e. the victim is a live pointer again, exactly the
shape of hazard 1.

Whether it *bites* depends on GE's FX params and `out_count` granularity: if
every `delay->input`/`delay->output` and every `out_count` is a multiple of 4
samples, `ROUND_UP_8` is a no-op and nothing happens. Hence SUSPECTED. Cheap
check for mgb64: assert `((before_end << 1) & 7) == 0` and
`(((count + ram_align) << 1) & 7) == 0` in `native_fx_load_buffer` /
`native_fx_save_buffer`, or just fix it unconditionally the way we did —

- allocate `length + 4` samples (`AL_FX_TAIL_SLACK`, `game/libultra/src/audio/
  synthInternals.h` here) with a `_Static_assert` that the slack ≥ 8 bytes, and
- bounds-guard the DRAM side of every delay-line transfer against
  `[base, base + length + slack]`, printing once and clamping instead of
  corrupting (`_fxClampXfer` in `game/libultra/src/audio/mips1/reverb.c`, exposed
  as `alFxGuardTrips()` and printed as `[AUDIO] fx-guard trips=N`).

The guard is worth copying on its own merits: it caught a fourth tap site we had
missed on the first pass, in the very first run after the fix. mgb64's helpers
already early-out on `count <= 0` / `fx->base == NULL`, which is partial
hardening of the same area — the missing half is the *upper* bound.

### 3. Free bonus if mgb64 ever bypasses its FX: don't leak the aux send
Watch the disable path. In stock libultra `alFxPull` returns early **after**
pulling its source, which leaves the aux bus's voice sum (the wet *send*) sitting
in `AL_AUX_L/R_OUT` — and `alMainBusPull` then mixes `AL_AUX_*` into `AL_MAIN_*`
at unity for every source (mgb64 keeps this: `src/libultra/audio/mainbus.c`
equivalent). So "FX off" does not mean "dry": it means every voice's wet send is
added to the master undelayed and unattenuated, on top of its dry. Here that was
worth **+2.5 dB of peak** and **4.3× the master-bus clip events** (measured:
worst pre-clamp magnitude 52805 = +4.14 dBFS with FX off vs 41191 = +1.99 dBFS
with FX on, over a 12000-frame soak). If mgb64 has any bypass switch on that
filter, it needs to clear the aux buffers on the way out.

---

# Phase 3 session findings (2026-07-25)

Ordered by value to mgb64. The first two are **shared-code** items, i.e. they
concern files mdkr64 vendored from mgb64 verbatim, so they are the ones most
likely to be live bugs in GoldenEye right now.

## 1. CONFIRMED SHARED CODE — WebGPU viewport clamping ALTERS the viewport transform

**This is the highest-value item in this document.** `wgpu_clamp_rect()` exists in
mgb64 (`src/platform/fast3d/gfx_webgpu.c:3196`) and is applied to the viewport at
`:3397`. mdkr64 proved what that clamp actually does when a game requests an
**over-sized** viewport:

- DKR's `mtx_ortho` deliberately sets `vp.vscale[1] = width * 2`, i.e. it asks for
  a viewport **320 logical px tall on a 240-tall target** (the game over-sizes it
  so a background can scroll).
- GL and Metal accept that transform and merely clip the pixels. **WebGPU
  *validates* containment**, so the clamp trimmed `(0,−160,1280,1280)` to
  `(0,0,1280,960)` — cutting the half-height 160 → 120, which is a **uniform 0.75×
  squash about screen centre** of every primitive drawn through that viewport.
- Screen-space TEXRECT text was unaffected (it bypasses the viewport transform),
  which is exactly why the symptom presented as "the text and the panels disagree"
  rather than as a viewport problem. It cost a full agent-session to localise.
- Measured: menu buttons spanned rows 91–139 (h=48) against the real ROM's 81–145
  (h=64, the value in the game's own button table); horizontal extents were
  already exact. Predicted rows 120/930 of 960 matched measured `firstLit=76 /
  lastLit=929`.

**mgb64's own comment at `:3371` already says the clamp "clips a Y-flipped rect"
and mentions widescreen / split-screen** — and mdkr64's copy carried a comment
warning the clamp "ALTERS the viewport transform". That warning was correct and
under-weighted.

**Why GoldenEye is more exposed than DKR was:** GE has up to **four-way
split-screen**, which sets several viewports per frame, and any of them that is
over-sized or Y-flipped past the target edge gets silently rescaled rather than
clipped. A 0.75×-style squash in one quadrant of a 4P game is easy to mistake for
an aspect-ratio or HUD-layout issue.

**Fix shape used in mdkr64:** rasterize with the clamped rect, but return the
clip-space affine that restores the *requested* transform (`x' = x*sx + bx*w`), so
containment is satisfied without changing where geometry lands. Search
`wgpu_viewport_fix` in mdkr64 `platform/fast3d/gfx_webgpu.c`.

**How to test cheaply:** A/B the same scene with `MDKR_RENDERER=gl`-equivalent vs
WebGPU. In mdkr64 the identical native run scored 93.7 % on GL and 70.4 % on
WebGPU against the ROM oracle, with black bars over the top 19 / bottom 7 rows.
Any GL-vs-WebGPU divergence localised to *geometry position* (not filtering) is
this bug.

## 2. LIKELY GAP — is the GPU texture cache invalidated when the texture arena is freed?

mdkr64 hit a genuinely nasty one: **the HLE texture cache aliased freed arena
memory.** Its `dkr_bind_tile()` keyed GPU-texture cache entries on the **source
address** (+ fmt/siz/width/height/palette) and nothing invalidated an entry when
the game *freed* that memory. Once the pool handed the same bytes to a different
asset, the next lookup **hit** and bound the previous asset's uploaded texture.

Silent by construction: **no crash, no missing draw, correct geometry, wrong
image.** It survived every crash-count fixture and the whole visual-fidelity
sweep, and only became obvious on a screen that draws two textures in alternating
30-px bands, where every other band came out the wrong world's colour.

**Status in mgb64 — please verify, we could not settle it from the outside.**
mgb64 clearly knows about this class: `gfx_ptr_invalidate_range()` exists
(`src/platform/gfx_ptr.h:153`), `image.c texArenaFreeAll` calls it for the texture
pool (`src/game/image.c:163`), and there is a
`gfx_texture_cache_delete_by_palette_addr()`. But what we found at
`texArenaFreeAll` invalidates the **gfx_ptr registry**, and we did not find a
corresponding **`gfx_texture_cache`** invalidation for the freed span.

The question to answer is narrow: *can a `gfx_texture_cache` entry whose key is a
`tex->data` address inside the freed pool be HIT later by a different texture that
the allocator placed at the same address?* If the lookup re-verifies something
content- or identity-derived (mgb64's `settex_cache` does re-check
`.valid && .texturenum`, `gfx_pc.c:1102`) then you are protected and this is a
non-issue — say so and we will record it. If it re-verifies only the key, this bug
is live and invisible.

**Cheap detector, worth having permanently either way:** record a content hash of
each texture's source bytes at upload time, and on every cache *hit* re-hash and
compare. mdkr64 keeps this behind `MDKR_TEXCACHE_VERIFY=1`; it printed
`STALE HIT ... uploadedHash=dcff67eb nowHash=f10b5339` and named the bug
immediately. With the fix: 0 stale hits across every fixture. Without: 20 stale
hits from a specific frame. Cost of the fix itself: ~25 µs/frame (< 0.2 % of a
frame), invalidating at the single point where arena bytes become reusable
(`mempool_slot_clear`), **before** coalescing rewrites the slot size.

## 3. A REAL BUG CLASS: a stack array one element short of what the same function reads

Three instances found in mdkr64, all the same shape and all invisible on N64:

| site | declared | actually written/read |
|---|---|---|
| `timetrial_ghost_read` (racer.c) | `f32 vectorX/Y/Z[3]` | loop runs `i <= ARRAY_COUNT(...)` → 4; `catmull_rom_interpolation()` reads `data[0..3]` |
| `fileselect_render` (menu.c) | `char trimmedFilename[4]` | `filename_trim()` writes `strlen(input)+1`, inputs up to `"GAME A"` → 7 |
| `func_8002F440` (tracks.c) | `sp90[6]` / `sp80[6]` | clipping a triangle against 4 planes yields up to 7 verts |

On N64 the extra store lands in adjacent stack slack — in one case the decomp
still *has* the padding local (`pad_sp58`) that used to absorb it — so real
hardware reads back the value it just wrote and behaves correctly. On a host build
it hits the `-fstack-protector` canary (`__stack_chk_fail`, SIGABRT) or, without
the canary, silently corrupts a neighbouring local *and* feeds garbage to the
consumer.

**How to find them cheaply:** grep for `<=` against `ARRAY_COUNT`/`sizeof` in loop
bounds, and for interpolators that read `data[i+3]`. Then run ASan over each
subsystem *as it first becomes reachable* — these only fire once the code path
runs, so they surface one gameplay milestone at a time.

**Fix shape:** size the array to the true requirement under the port's `#ifdef`,
switch the loop bound to `<` so the identical stores happen in bounds, leave the
N64 declaration untouched, and lock it with a `_Static_assert` tied to the real
requirement.

## 4. METHODOLOGY: coupled fixes defeat naive positive controls

Worth internalising, because it cost a review cycle here. The ghost fix above is
**two coupled edits** (array size *and* loop bound). Reverting only one produces a
**third** state that never existed:

| size | loop | result |
|---|---|---|
| `[4]` | `<` | correct |
| `[3]` | `<=` | the true original — **SIGABRT**, check fails |
| `[3]` | `<` | memory-safe, **silently wrong** (reads uninitialised `data[3]`) — every check passes |

A reviewer who reverts "the array size" to test the fix lands in row 3, sees green,
and concludes the fix was unnecessary. The `_Static_assert` is what blocks that
half-revert — and **neutering the assert to make the half-revert compile is the
step that invalidates the control.** If you write a coupled fix, say so at the
revert instructions, not just in the code comment.

## 5. ROM-oracle harness lessons (all four cost real time here)

mdkr64's oracle is modelled on mgb64's. Four traps, each of which presented as a
*rendering* fidelity gap and was not:

1. **An unknown `--setting` key makes ares abort before loading the ROM.** mdkr64
   passed `Audio/Driver=None`; that key does not exist in the pinned ares (only
   `Device/Frequency/Latency/Blocking/Dynamic/Mute/Volume/Balance` are bound), so
   every run produced **zero frames** — and the documented "silent by
   construction" guarantee was resting on a setting ares rejected. **mgb64 already
   does this correctly** (`SDL_AUDIODRIVER=dummy` in `tools/*.sh`); mdkr64 adopted
   your approach. Nothing to change — recorded so nobody "improves" it back.
2. **Injecting input port-blindly joins every player.** The controller hook
   returned the same buttons for *every* connected pad, and ares' N64 has four
   ports. In DKR that made four players join at the character-select screen, which
   **changes the menu graph** (its CAUTION screen only appears for a single
   player), so the two runners walked different paths and scored ~50 % for two
   correctly-rendered *different screens*. GE is a 4-controller game — if your
   injection hook is port-blind, any menu that counts joined players will diverge.
   Discriminate on **instance identity**, not on the port node's name: in ares'
   `Gamepad`, the `port` member is reassigned in the constructor to the *Pak*
   subport, so `port->name()` is `"Pak"` for every controller and a name test
   matches nothing (which silently disables injection entirely).
3. **One global sync delta cannot align a multi-tap route.** Real hardware's level
   load is a genuine multi-frame stall that the emulator presents frames through;
   a host port loading from memory takes far fewer (measured: ~40 frames vs ~20).
   The error **accumulates per tap**. Fix: per-event/per-mark offsets calibrated
   from measured screen-arrival frames, plus a comparator that reports **both** the
   exact-frame score and a bounded-search "aligned" score *with the offset it
   needed*. A high aligned score with a large offset means "right screen, wrong
   time"; a low aligned score is the real gap. Never quote the aligned number
   alone — it is the more flattering of the two by construction.
4. **A mark on a transition scores two black frames as ~50 %.** Identical
   histograms, no structure to correlate. Detect near-constant frames and exclude
   them from the headline number instead of averaging in a meaningless value.

Also: emulator dumps may have **non-square pixels** (ares dumps N64 output at
640×240 covering a 4:3 area). A comparator that resizes both runners to a common
raster is correct, but **any ad-hoc crop you do yourself must scale to square
pixels first** — otherwise a squashed crop reads as a render bug. That mistake
produced a confident, wrong "the real ROM applies a spherical warp" conclusion
here.

## 6. METHODOLOGY: test hooks, and using the game's own AI as a test driver

Four env hooks carried mdkr64's Phase 3, all **no-ops unless set** (the contract
matters — it keeps them out of the shipping path and lets them live permanently):

- `MDKR_FORCE_BOOST=frame:len` — force a transient effect so a dumped frame is
  guaranteed to contain it.
- `MDKR_FORCE_LAPS=N` — shorten a race so the finish is reachable. Applied **once
  at the asset-load boundary** (rewriting the level header's lap byte) rather than
  at the ~12 read sites across the race loop and HUD, so every reader stays
  consistent by construction.
- `MDKR_LOAD_TRACK=<levelId>[:<vehicle>]` — retarget the *race* load of one proven
  menu route, so N tracks need one input fixture instead of N. Targeted by matching
  the game's **own** "track to race" global, which leaves menu backgrounds and
  preview loads untouched.
- `MDKR_AUTOPILOT=1` — **the most useful of the four.** It drives the *human*
  racer with the game's own AI pathing routine, the one every CPU racer uses.
  Hand-tuned open-loop input held the racing line for exactly one lap and then
  stranded the kart in a corner; the AI holds a consistent line indefinitely, which
  is what finally made the race-finish path testable. GE has AI-driven characters —
  the same trick (hand a player-controlled entity to the existing AI for the
  duration of a test) should transfer directly.

One caveat found the hard way: the AI throttle routine early-returned when the
field contained no CPU racers, so a solo time-trial got steering but no throttle
and the AI's stuck-recovery drove it backwards. The workaround used the game's own
idiom — present the racer as a CPU racer for the duration of the AI call only.

## 7. COVERAGE: validating one level hides the entire per-level asset bug class

mdkr64 had a fully working race — HUD, audio, physics, a saved lap record — on
**one** track. The moment a sweep drove all 20, one track hard-crashed
(SIGSEGV in an object update, dereferencing a behaviour-data union left NULL by a
mis-resolved animation target). 19/20 passed, so the failure was not systemic —
it was exactly the kind of single-asset defect that only a sweep finds.

Build the level-override hook **early**, and make the sweep assert behaviour and
not just exit codes: this project's per-asset bugs are overwhelmingly silent
(a big-endian float that reads as a denormal, a `-0.0` numerator, a table with the
wrong stride). Our sweep asserts, per level: exit code, that the level really
loaded, that position samples are finite, that progress actually advanced, and
that no fatal/sanitizer text appeared.

## 8. TO CHECK (low confidence, cheap to rule out)

`FILL_RECTANGLE` outside `G_CYC_FILL`/`G_CYC_COPY` rasterizes through the
combiner, where with no texel and no shade only the `d` term matters (`(a−b)*c+d`
degenerates to `d`). DKR's dialogue boxes rely on exactly that — `G_CC_ENVIRONMENT`
plus `gDPSetEnvColor`, then plain fill rects in 1-cycle mode — and mdkr64's HLE
read `prim_color` unconditionally, so **every dialogue-box background was
invisible** (traced: `rgb_d=5` = ENVIRONMENT, `env=00000080`, `prim=00000000`).
We found no equivalent non-FILL-cycle fill-rect path in mgb64's `gfx_pc.c`, so this
may simply not apply to GE. If GE does draw any flat rect in 1- or 2-cycle mode,
resolve the colour from the register the combiner actually names.

---

# Widescreen/FOV and projected-shadow findings (2026-07-25)

The source investigation is preserved here alongside the implementation facts.
MGB64 was inspected read-only at HEAD
`f9fd34f4e245c1561a62669c8737b628e737e9db`; no changes were made there.

## 1. CONFIRMED DESIGN RULE — wider rendering must not widen simulation

mdkr64's first Hor+ implementation widened object admission along with the
camera. It looked correct, then its closed-loop race diverged at frame 3252.
This was not numerical noise:

- DKR decides whether `render_object()` runs through the visibility set
  ([mdkr64 tracks.c:1909](../game/src/tracks.c#L1909));
- racer render setup writes an on-screen timer used by CPU physics
  ([mdkr64 objects.c:4070](../game/src/objects.c#L4070);
  [mdkr64 racer.c:9000](../game/src/racer.c#L9000));
- model rendering advances animation and animated textures, and random texture
  animation consumes the gameplay RNG
  ([mdkr64 objects.c:3782](../game/src/objects.c#L3782);
  [mdkr64 textures_sprites.c:1695](../game/src/textures_sprites.c#L1695)).

Saving only the timer, then saving the timer plus RNG, still did not make the
widened route invariant because the renderer mutates a broader object/model
surface. The safe DKR boundary is raw level geometry wide, object admission
faithful
([mdkr64 tracks.c:2538](../game/src/tracks.c#L2538);
[mdkr64 tracks.c:2747](../game/src/tracks.c#L2747)).

MGB64 already contains the right *principle*: its render cull widen has scoped
suppression and byte-for-byte restoration around simulation-consumed tests
([mgb64 bondview.c:2095](../../mgb64/src/game/bondview.c#L2095)), and its
widescreen portal pass marks widen-added rooms draw-only while restoring the
faithful room state
([mgb64 bg.c:2578](../../mgb64/src/game/bg.c#L2578);
[mgb64 bg.c:17255](../../mgb64/src/game/bg.c#L17255)).

The new backflow is the **test requirement**: compare exact normalized gameplay
state across 4:3/16:9/21:9, not only pixels. "Render" functions must be assumed
impure until measured otherwise. Snapshot/restore is safe only if every mutated
field and global RNG stream is known.

## 2. Explicit world / safe-2D / full-bleed policy should become shared vocabulary

mdkr64 now maps its 320×240 logical space to three regions:

| Class | Policy |
|---|---|
| World | live/forced presentation aspect, Hor+ |
| Safe 2D | centered undistorted 4:3 contained in presentation |
| Full bleed | undistorted 4:3 cover, cropped to fill every host pixel |

The policy lives in one pure module
([mdkr64 display_config.h:1](../platform/display_config.h#L1)) and the F3DDKR
interpreter applies it uniformly to viewport, scissor, triangles, and rectangles
([mdkr64 gfx_pc_dkr.c:1548](../platform/fast3d/gfx_pc_dkr.c#L1548)).

MGB64's design document already proposes a `DrawClass`-driven 2D table
([DISPLAY_INPUT_PLAN.md:120](../../mgb64/docs/design/DISPLAY_INPUT_PLAN.md#L120)),
but the status table still shows aspect and 2D layout work as incomplete
([DISPLAY_INPUT_PLAN.md:245](../../mgb64/docs/design/DISPLAY_INPUT_PLAN.md#L245)).
Carry the three names into that work:

- menu/cutscene art normally wants **safe**;
- HUD may want **safe** initially, then an optional edge-anchored modern class;
- fades, clears, masks, and backdrop fills want **full bleed**;
- world and viewmodel draws need their own lens policies.

Do not infer policy from "2D versus 3D." A fullscreen transition and a menu
panel are both 2D but require opposite fit/cover behavior.

## 3. Split-screen projection must follow the actual viewport transform

DKR two-player mode retains a full-height RSP viewport and exposes top/bottom
halves with scissors. Computing lens aspect from the scissor would therefore
double the intended aspect. mdkr64 uses the RSP viewport dimensions and keeps
the clip independent
([mdkr64 camera.c:867](../game/src/camera.c#L867);
[mdkr64 camera.c:953](../game/src/camera.c#L953)).

MGB64 currently hands each player's stored view size, FOV, and aspect to the
renderer
([mgb64 lvl.c:1633](../../mgb64/src/game/lvl.c#L1633)). Its own plan correctly
calls for tracing final per-player viewports before replacing the active aspect
path
([DISPLAY_INPUT_PLAN.md:247](../../mgb64/docs/design/DISPLAY_INPUT_PLAN.md#L247)).

The acceptance gate should assert all of the following independently:

1. each player actually exists;
2. each player's simulation progresses;
3. each viewport/scissor pair is the requested pair;
4. every screen region is visually live;
5. the normalized multiplayer state stream is identical across host aspects.

Whole-frame variance cannot detect one dead quadrant when the other three are
healthy.

## 4. Browser sizing: one source metric and one proportional clamp

mdkr64's shell owns CSS size/DPR/backing-store sizing and applies one common
scale under edge and pixel budgets
([mdkr64 mdkr64-shell.js:116](../dist/web/mdkr64-shell.js#L116)). Its native
frontend likewise clamps both target axes proportionally and calls backend
`on_resize()`
([mdkr64 gfx_pc_dkr.c:2271](../platform/fast3d/gfx_pc_dkr.c#L2271)).

MGB64 already has the more general version of the same rule:

- use a stable CSS-size × DPR source on the browser rather than feeding the
  configured backing store back through render scale
  ([mgb64 gfx_pc.c:4838](../../mgb64/src/platform/fast3d/gfx_pc.c#L4838));
- clamp both axes with one factor selected from the active backend's reported
  limit
  ([mgb64 gfx_pc.c:4897](../../mgb64/src/platform/fast3d/gfx_pc.c#L4897)).

Keep those two invariants when the explicit aspect modes land. Independent
per-axis minima are a silent distortion bug; reading a backing store that the
renderer itself resizes creates a positive-feedback resolution loop.

## 5. LIKELY LIVE CHECK — projected/blob shadows must request decal depth

mdkr64's random ground-shadow flicker was a source-state omission, not a missing
backend feature. Its terrain-wrapped shadow vertices are coplanar with the
receiver, but the draw requested ordinary depth comparison. Adding
`RENDER_DECAL` selects `ZMODE_DEC` and the already-correct GL/WebGPU/Metal depth
bias
([mdkr64 tracks.c:3702](../game/src/tracks.c#L3702);
[mdkr64 tracks.c:3741](../game/src/tracks.c#L3741)).

MGB64's backends already implement this contract:

- GL: `LEQUAL` plus polygon offset for `ZMODE_DEC`
  ([mgb64 gfx_opengl.c:1766](../../mgb64/src/platform/fast3d/gfx_opengl.c#L1766));
- WebGPU: negative pipeline depth and slope bias
  ([mgb64 gfx_webgpu.c:2137](../../mgb64/src/platform/fast3d/gfx_webgpu.c#L2137));
- Metal: negative draw depth and slope bias
  ([mgb64 gfx_metal.mm:3521](../../mgb64/src/platform/fast3d/gfx_metal.mm#L3521)).

GoldenEye's `doshadow()` constructs a projected four-vertex quad and delegates
render-state selection through `sub_GAME_7F073038()`
([mgb64 model.c:11188](../../mgb64/src/game/model.c#L11188)). Audit the final
decoded depth state of that draw and any other projected receiver overlays.
Backend support is not evidence that the source call site actually selects it.

The robust check is:

1. add a diagnostic that restores the old non-decal request;
2. count final decoded `ZMODE_DEC` primitives at triangle emission;
3. require identical simulation state between arms;
4. compare consecutive moving-camera frames;
5. require the fixed arm's changed pixels to be localized and only darker.

mdkr64's GL/WebGPU A/B found intermittent differences in 76/300 and 74/300
frames respectively, with zero brightened components
([check_shadow_visual_ab.py:1](../tests/check_shadow_visual_ab.py#L1)).

## 6. Fixed-cap geometry builders should be transactions

The DKR shadow builder owns fixed descriptor/triangle/vertex arrays. A local
check immediately before one write is insufficient because a partial object
leaves later descriptors referring to mismatched spans. mdkr64 now:

- snapshots all counts per object;
- reserves every buffer before writes;
- rolls back all counts if any buffer fills;
- publishes `meshStart = -1` for a dropped or empty mesh;
- writes and validates an in-bounds terminal descriptor
  ([mdkr64 tracks.c:4070](../game/src/tracks.c#L4070)).

The fault seam lowers physical capacities and proves whole-mesh drops under ASan
([mdkr64 stubs_dkr.c:971](../platform/stubs_dkr.c#L971)). This pattern transfers
to any MGB64 dynamic mesh, decal, particle, or shadow-map capture builder that
uses parallel fixed-cap arrays.

Two associated array shapes are worth grepping:

- an array indexed as `id + 1` needs `max_id + 2` slots, not `max_id + 1`
  ([mdkr64 tracks.c:1859](../game/src/tracks.c#L1859));
- four 32-entry slices require 128 elements even if the decomp symbol happened
  to declare 120
  ([mdkr64 tracks.c:129](../game/src/tracks.c#L129)).

## 7. Matching/native separation remains mandatory

The display-space transport uses otherwise-unused F3DDKR command bits only under
`NATIVE_PORT`; matching builds emit the original matrix command
([mdkr64 f3ddkr.h:151](../game/include/f3ddkr.h#L151)). MGB64's porting guide
requires the same isolation for native-only behavior
([PORTING_AND_EXPANSION.md:8](../../mgb64/docs/PORTING_AND_EXPANSION.md#L8)).

Keep user-facing aspect/FOV defaults reversible, and do not let host drawable
state enter matching or simulation-visible structures.
## Preserved pre-implementation audit note (VIS-01/VIS-02)

Two parts of the VIS-01/VIS-02 findings should be carried between projects:

1. **Treat widescreen as projection + visibility + draw-class policy, not a
   global X-coordinate trick.** MDKR64's world projection is simpler than BOND's,
   but its culling planes are hard-coded independently of the projection and its
   renderer stretches 320×240 viewports, scissors, and rectangles directly to the
   drawable. MGB64 already has the harder portal-visibility isolation, live
   CSS/DPR/device-limit dimensions, and `DrawClass`; keep those pieces generic
   enough for MDKR64 to reuse. Both projects should share a gate over
   4:3/16:10/16:9/21:9 and 1–4 players that checks Hor+ vertical framing, no
   wide-edge cull pop, undistorted 4:3-safe UI, intentional full-bleed draws, and
   simulation-state invariance.
2. **Assert the coplanar-decal contract end to end.** DKR describes its projected
   shadows as decals but the live shadow draw flags do not request
   `RENDER_DECAL`; a sampled racer texture also lacked that bit, so GL/Metal/WebGPU
   never reach their existing decal depth-bias paths for the shadow. This is a
   DKR-specific leading cause, **not a confirmed MGB64 defect**. The transferable
   test is: every material classified as a coplanar decal must arrive at the
   backend as decal z-mode, and consecutive-frame flat/slope/camera sweeps must
   prove equivalent qualitative bias on GL, Metal, and WebGPU. A single screenshot
   cannot detect alternating depth coverage.

## 2026-07-26 — real-browser release gate and live-sink audio pressure

**Disposition: reusable test architecture; inspect MGB64 before treating the
audio result as a shared defect.**

MDKR64 now runs its shipped shell and freshly linked wasm in an isolated real
Chromium profile (`tests/check_browser_runtime.py`). The dependency-free CDP
harness is worth porting as a pattern:

- select the external ROM through the actual file input; never serve or inject
  its bytes through HTTP;
- audit both CDP and the local server for external URLs, request bodies, and ROM
  names;
- require multiple changing rendered scenes and real race progress, not merely a
  live canvas or frame counter;
- switch CSS size and DPR while C is running, and assert the exact backing-store
  dimensions observed by the renderer;
- require AudioWorklet PCM delivery and fail on device-loss/GPU-process markers;
- flush IDBFS, reload the same isolated profile, and compare the exact save hash
  before `main()`; test save-only erase and ROM erase separately;
- include broken-direction controls for flat output, a synthetic upload,
  mismatched persistence, and event-queue exhaustion.

Three MDKR64 implementation traps are directly transferable:

1. A shell writing `Module.ENV.MDKR_*` is ineffective unless Emscripten exports
   `ENV` in `EXPORTED_RUNTIME_METHODS`. MDKR64's documented trace control silently
   did nothing until the real browser gate required its output.
2. A coalesced `syncfs` callback must wait for the *final* queued sync. Returning a
   callback immediately when another sync is in flight makes callers believe
   persistence is durable while the write is still pending. Carry the first error
   through the coalesced chain and release all waiters only at quiescence.
3. Fixed-cadence headless synthesis can seriously understate live-sink event-queue
   pressure. MDKR64 measured only 19 SFX events under its fixed native pump, but
   up to 195 through the browser AudioWorklet occupancy controller; the old
   150-entry queue intermittently dropped posts. Its port budget is now 512, with
   a release assertion that every queue stays at or below half capacity.

For MGB64, do not copy the number 512. Instrument each event queue in the actual
browser runtime, identify queue/capacity in every drop diagnostic, measure without
the truncating cap, and size from that ceiling. The already-recorded MGB64
empty-queue spin hazard remains the higher-severity invariant: a dropped
self-perpetuating event must degrade with a nonzero handler delay, never wedge
the audio driver.

## 2026-07-26 — linked no-op reachability and route-domain coverage

**Disposition: MGB64 already has useful static guards, but neither proves that a
gameplay-critical path reaches a real implementation.**

MDKR64 compiled the only per-facet object-model collision body out and silently
resolved its undefined symbol to a weak `return 0` fallback. The program linked,
ran, and raced; every collision-meshed hub object was simply intangible. A
race-only probe reported eight calls, while the first hub route reported 948
calls and the final fixed/legacy gate measured 1,730 hits versus zero
([objects.c](../game/src/objects.c);
[check_door_blocks.py](../tests/check_door_blocks.py)).

MGB64 has two good pieces already:

- its native stub-surface guard bans a small named set of known gameplay/audio
  fallbacks
  ([check_native_stub_surface.py](../../mgb64/tools/check_native_stub_surface.py));
- its ASM audit pairs the roughly 388 inert `GLOBAL_ASM` references with their
  compiled native C siblings
  ([asm_audit.py](../../mgb64/tools/fidelity/asm_audit.py)).

The remaining release invariant is linked-symbol reachability:

1. inventory every gameplay helper whose retail assembly disappears under
   `NONMATCHING`/`NATIVE_PORT`;
2. resolve the final binary's provider with `nm`/link-map evidence and reject an
   unexpected weak/no-op provider;
3. count calls **and successful effects**, not only entries;
4. drive each helper in the domain where it matters — hubs, mission objectives,
   doors, props, and interaction volumes as well as ordinary combat routes;
5. give every silent fallback a same-binary positive control that restores the
   old no-op and must visibly change the outcome.

This is not a claim that MGB64 currently links a bad fallback. It is a gap between
its strong source-level inventory and the final linked/runtime proof. The DKR
lesson is specifically that "zero hits on the routes we ran" and "the real body
is linked" are different assertions.

## 2026-07-26 — RAW16 boundary: byte order belongs to data, not the host

**Disposition: MDKR defect fixed; MGB64 already routes the right calls but its
conversion remains host-assumptive.**

MDKR's three `alRaw16Pull()` sites copied serialized big-endian signed PCM into a
host-native `s16` mixer buffer without conversion. The nearby fourth load is
ADPCM and must remain a byte stream. The broken output passed the broad audio
gate — format, RMS, stereo, tempo, clipping, and reverb were all plausible — so the
repair needed a timbre/encoding-specific instrument.

MGB64 already sends its three corresponding sites through
`mixerLoadBufferSwap16` (`audio_compat.c:4412/4442/4474` in the inspected
snapshot), which is the correct source classification. The helper
unconditionally reverses each byte pair, however. That works on current
little-endian builds and would corrupt already-native PCM on a big-endian host.
This is portability debt, not a claim that MGB64 currently ships such a target.

MDKR's transferable repair and gate are:

1. Build numeric values from the serialized bytes. Do not branch on or infer the
   host when the data already defines its order. Keep literal `bswap16`/
   `bswap32` helpers as unconditional reversal operations, distinct from
   `read_be*` conversion.
2. Make the buffer conversion unaligned-safe and transactional: odd byte count,
   short destination, or invalid pointers fail before touching output.
3. Statically require exactly three converted RAW16 loads and one untouched
   ADPCM load.
4. Independently census the actual banks. MDKR has 25 RAW16 music waves and one
   additional RAW16 SFX wave; a music-only audit missed the latter.
5. Run fixed and exact legacy modes from one binary, require an identical PCM
   prefix up to the first RAW16 block, then require a strong post-boundary
   divergence. MDKR's principal bass is 27.34x rougher under legacy decoding.
6. Require the actual browser AudioWorklet route to report nonzero fixed-mode
   loads and bytes. Compilation alone does not establish reachability.

The ROM-free endian unit should run on every supported host and eventually under
a real big-endian ABI/QEMU target. MDKR's byte-built implementation is
host-neutral by construction, but no real big-endian runtime was available, so
that matrix cell remains explicitly unclaimed.

## 2026-07-26 — post-projection billboard offsets are a separate widescreen class

**Disposition: the exact F3DDKR defect is DKR-specific; carry the transform audit
and pixel gate to MGB64's effect/monitor billboards.**

MDKR's initial widescreen implementation made perspective geometry isotropic and
mapped HUD art uniformly, yet world collectible balloons remained horizontally
stretched. F3DDKR projects a single anchor normally, then adds billboard-local
X/Y offsets directly in clip space
([mdkr64 gfx_pc_dkr.c](../platform/fast3d/gfx_pc_dkr.c);
[mdkr64 camera.c](../game/src/camera.c)). Those offsets bypass both the
perspective focal scale and the viewport aspect. Retaining the N64's 4:3
compensation measured:

```text
4:3     68x27 blue balloon motif
16:9    93x27  (1.37x aspect distortion)
21:9   121x27  (1.78x aspect distortion)
```

The robust correction does not replace one `scaleY` argument. It builds the
retail matrix first and scales its complete output columns:

```text
clipY = tan(authoredVFOV / 2) / tan(effectiveVFOV / 2)
clipX = clipY * authoredTVAspect / effectiveViewportAspect
```

Scaling output columns preserves every rolled sprite's original pixel-space
transform; changing one non-uniform builder input fixed an upright balloon but
did not preserve off-diagonal terms. It also keeps ordinary Hor+ at authored
size, lets a user FOV or horizontal cap scale billboards with the world, and
leaves exact legacy stretching reversible.

MGB64's inspected billboard effects are generally constructed as world-space
quad vertices and then pass through the ordinary model/view/projection path — for
example the bullet-spark audit in
[`ASM_AUDIT.md`](../../mgb64/docs/fidelity/ASM_AUDIT.md)—so this is **not a claim
that MGB64 has the same live defect**. Its monitor screens, muzzle flashes,
sparks, explosions, and other effect billboards are still the right census:

1. classify whether each local corner is expressed in world/view space or added
   after projection;
2. do not treat an isotropic perspective matrix as proof for post-projection
   offsets;
3. test rotation as well as an upright quad;
4. compare equal-height 4:3/16:9/21:9 captures and at least one changed-FOV arm;
5. use a deliberate legacy/aspect regression as a positive control.

MDKR's dependency-free gate measures the same golden-balloon art through the
SAFE_2D and world-billboard paths at two deterministic approach frames. Fixed
output is 99×36 for the HUD at every production aspect and 48×18 for the world
motif at both 4:3 and 16:9; capped 21:9 and 75° FOV follow their analytic focal
scales. The exact legacy arm remains 173×36 / 84×18 and must fail the production threshold
([check_widescreen_proportions.py](../tests/check_widescreen_proportions.py)).
That paired-art pattern is broadly reusable when one asset can be found in two
transform classes.

The refinement makes the producer census executable as well as documented:
exactly the two audited world/ortho builders may enable F3DDKR billboard mode,
the world builder must retain its aspect/FOV correction, and the renderer must
retain one decoded post-projection-offset path. Any new direct producer fails
until classified. Runtime arms now include 16:10 and forced 4:3 inside 21:9;
ROM-free cases include portrait layout/projection and a rotated transform. MGB64
should apply the same "new producer requires classification" rule to effect and
monitor billboard microcode even if its current quads are world-space.

## 2026-07-26 — native representation boundaries need one builder, one parser, and field APIs

**Disposition: transferable LP64/serialized-data audit class; no claim that the
named MDKR defects are live in MGB64 without reproduction.**

MDKR's first halt-on-error alignment run stopped on a pointer-bearing
`ShadowData` placed at 4 mod 8. Recover mode produced 88 diagnostics across three
mechanisms that all worked on N64 by ABI/adjacency luck:

1. a heterogeneous `Object` tail advanced as bytes, aligned/rounded to four,
   reserved light pointers as `count * 4`, and narrowed host pointers to `s32`
   to calculate the final allocation;
2. an asset stream walked by embedded even byte strides was exposed as a native
   union whose largest member required four-byte alignment;
3. HUD/weather/particle/effect records were cast to full scene-object types so a
   renderer could read a transform prefix and the next animation-frame field.

The transferable repair is structural:

- Use one checked `size_t`/`uintptr_t` cursor for both sizing and placement.
  Every typed append supplies `sizeof(T)` and `_Alignof(T)`; arrays use checked
  multiplication; final allocator narrowing is explicit. A hand-computed size
  followed by separate pointer arithmetic is two layouts and will drift.
- Treat every embedded-stride stream as an untrusted representation boundary.
  Validate the common header, minimum/even stride, remaining bytes, ID/table
  domain, and per-dispatch minimum before forming a typed view. Keep serialized
  size distinct from host `sizeof`.
- Replace "prefix-compatible" casts with APIs that accept the fields actually
  consumed. If a temporary proxy is unavoidable, make it the declared target
  type, fully initialized and correctly aligned; never rely on what happens to
  follow a smaller source object.

The object-map sweep found a fourth high-value variant. DKR has an eight-byte
retail boost record and a ten-byte runtime-private extension. One initializer
always read the extension byte; for a map object it was actually the first byte
of the next record (`0x94` in the measured stream). The pointer it selected was
invalid but normally dormant. MGB64 should audit any initializer shared between
serialized and runtime-created variants: `sizeof(the largest C type)` is not
proof that the current record contains that form.

MDKR's both-direction gate is reusable as a pattern:

- a maximal synthetic tail with at least two host pointers;
- alternating 10/12/14-byte records to cover both legal modulo-four starts;
- exact old misaligned accesses that alignment UBSan must still report;
- a bare-prefix-as-large-object access that ASan must reject;
- a linked-handler check so a silently dropped sanitizer flag cannot produce a
  false pass;
- full menus/track/vehicle/mission/multiplayer runtime domains under
  halt-on-first-error instrumentation.

Do not copy MDKR's exact structs or default boost index. Carry the invariants and
the controls, then reproduce against MGB64's own builders, disk records, and
render-proxy callsites.

## 2026-07-26 — never infer player identity from race-array position

**Disposition: transferable harness invariant; no claim that MGB64 currently
misidentifies a player.**

MDKR's Adventure finish probe read `(*gRacers)[0]` and described it as "the
human." That array is starting-grid order. Slot zero was an AI, so the resulting
lap/finish trace supported a plausible but false diagnosis: the human appeared
to reach the final lap without finishing. Stable controller identity showed the
human completed the race normally. A later repair wave moved its natural place
from fifth to first, exposing a second lesson: natural AI-relative place is not
a stable win/loss oracle either.

The transferable gate has three parts:

1. Follow a stable controller/entity mapping, not spawn, render, distance,
   score, or sorted finishing order. Emit the stable ID alongside every sampled
   state so a future reorder fails visibly.
2. Sample at the state-machine seam the consumer uses. MDKR's winning result is
   consumed in the same finish call, so a probe at function entry can miss the
   only `finished=true` state; the correct point is after finish assignment and
   before reward dispatch.
3. Pair success and failure traces with independently decoded persistence.
   MDKR's symmetric verdict gate requires identical pre-control natural
   place/frame/clock/lap, then requests non-first and first results and checks
   visited/no reward versus cleared/exactly one reward. The checker derives field
   order from ROM metadata and validates the save checksum rather than trusting
   another game-side trace.

For MGB64, apply this to mission players, co-op participants, multiplayer slots,
and any bot/human mixed array. A log that says "objective complete" is not an
end-to-end progression assertion until the intended player's durable unlock or
mission status is decoded from the resulting save.

## 2026-07-26 — allocator boundaries need property tests, not isolated guards

**Disposition: transferable allocator invariant class; reproduce against
MGB64's allocator before copying implementation details.**

MDKR's central allocator had nine boundary failures that survived because normal
gameplay rarely hits the exact endpoints together: terminal frees formed
`&slots[-1]`; delayed-free entry 257 was written before the capacity diagnostic;
zero, negative, and align-overflow sizes entered list arithmetic; "safe"
allocation returned null to unchecked callers; unrelated host pointers were
relationally compared and foreign addresses defaulted to pool zero; fixed
allocation narrowed ranges to 32 bits and could partially split before discovering
it needed a second unavailable metadata slot; two alignment helpers truncated
host pointers; and an inactive interrupt shim returned an indeterminate mask.

The transferable repair principles are:

- validate positive, representable request sizes before alignment;
- represent "not in a pool" explicitly and compare address ranges as
  `uintptr_t` offsets only after proving the lower bound;
- determine every metadata split before mutating a free-list node;
- form neighbour pointers only inside valid-index branches;
- preserve delayed lifetimes by growing before the capacity write, rather than
  freeing a still-live object early;
- make an API named/used as infallible terminate at its diagnostic boundary;
- retain full host width in every pointer alignment/range helper.

The useful artifact is MDKR's ROM-free `memory_allocator` CTest, not its concrete
pool structs. It exercises only/head/middle/tail frees, previous/next/both
coalescing, 256 and 257 delayed frees through expiry, null/foreign/one-past
addresses, invalid and near-`INT_MAX` sizes, timer bounds, byte and metadata-slot
exhaustion, exact fit at metadata capacity, transactional fixed allocation, and
high-half/alignment-overflow pointers. The same test passes Debug, Release, ASan,
and UBSan, and the implementation links on wasm32. MGB64 should build an
equivalent property matrix around its own allocator and queue ownership model.

## Video config precedence as a monotonic rank (videoconf wave)

MGB64's `settings.h` carries `SettingOverrideSource { NONE, ENV, CLI, FAITHFUL,
REMASTER }`, but the enum is descriptive — it records *where* a value came from
for display purposes, while the actual precedence comes from the order
`config_pc.c` applies its layers.

MDKR's `MdkrVideoSource` is ordered by precedence and load-bearing:

```c
typedef enum MdkrVideoSource {
    MDKR_VIDEO_SOURCE_DEFAULT = 0,
    MDKR_VIDEO_SOURCE_FILE,
    MDKR_VIDEO_SOURCE_PRESET,
    MDKR_VIDEO_SOURCE_ENV,
    MDKR_VIDEO_SOURCE_CLI
} MdkrVideoSource;
```

`mdkr_video_config_set()` refuses a write whose source ranks below whatever set
the value last. That converts "env beats preset beats file" from an ordering
convention into a property a unit test asserts directly, and it makes
re-application safe — a live settings change can re-apply from its own layer
without a lower one clobbering it.

The second half is the seam that makes it testable: `mdkr_video_config_resolve()`
takes the parsed ini entries, an **env-lookup function pointer**, and argv. Pass
NULL for the lookup and the environment layer is skipped, so the whole ladder is
exercised from a fake environment with no window, no GPU and no ROM. The impure
half (real `getenv`, ini I/O, publication into `g_pc*`) lives in a separate TU
so the pure core links standalone into the test binary — merging them back
silently breaks that.

MGB64 would gain the same testability with a `settings_resolve()` that takes its
inputs as parameters, leaving `config_pc.c` as the thin layer that supplies them.

**Also worth carrying over:** the video report prints both the resolved value
*and* the effective downstream state. In MDKR `--video-list` shows
`Video.Aspect 4:3 [preset]` alongside `(effective display) widescreen=1
aspect=1.7778` when a later `--aspect` flag has overridden it. A report that
shows only its own resolved values will quietly disagree with what actually
renders.

## CPU-built mip chains (imagequality wave) — applies verbatim

Confirmed still present upstream at the time of writing: the same workaround at
the same file and line, `mgb64/src/platform/fast3d/gfx_opengl.c:1744-48`

```c
/* Use non-mipmap filters.  On macOS Metal, NPOT textures (32x48, etc.)
 * can fail glGenerateMipmap silently, leaving the texture incomplete.
 * The driver then substitutes a "zero texture" → garbage output.
 * GL_LINEAR/GL_NEAREST without mipmaps avoids this entirely. */
```

and its WebGPU mirror at `gfx_webgpu.c:2864` (`mipmapFilter =
WGPUMipmapFilterMode_Nearest; /* single-level like the GL path */`).

The comment is accurate about the driver bug and the wrong conclusion follows
from it. `glGenerateMipmap` is the thing that fails; building the chain on the
CPU removes the call entirely, so NPOT stops being special. mdkr64's
`platform/fast3d/gfx_mipgen.{c,h}` is a drop-in: it is pure (no GL, no WebGPU,
no globals), takes an RGBA8 buffer, and returns level pointers plus dimensions.
Its unit test needs no device.

Wiring is three pieces:

1. One optional `GfxRenderingAPI` entry, `upload_texture_mipped`. Make it the
   **last** struct member — all backends use positional initializers, so any
   other position silently shifts every following entry, and a backend that
   omits it then gets NULL, which is exactly "no mip support".
2. The sampler memo must gain a key. mgb64 memoizes `set_sampler_parameters` on
   `(linear, cms, cmt)` the same way; without adding the mip/LOD decision to
   that key, a 2D draw following a 3D draw with the same filter and wrap keeps
   the mipmapped sampler.
3. Screen-space 2D pins to level 0. In mdkr64 that pin proved unnecessary in
   practice (this game never minifies its 2D), but GoldenEye's scaled HUD and
   menu art may genuinely need it — worth measuring there rather than assuming
   either way.

Two filtering rules the implementation encodes, both worth carrying over
because getting either wrong looks like a bug in something else:

- Filter in **linear light**. Averaging sRGB bytes of black and white gives 127;
  averaging the light gives 188. Alpha is coverage, not light — no transfer
  function.
- Use an **exact-area box**, not a fixed 2-tap, or odd dimensions silently drop
  their last row or column.

For alpha-tested cutouts, coverage-preserving reduction is a separate entry
point. Three non-obvious failure modes are documented in its source comments:
corrections compounding when applied inside the reduction loop; the search
predicate and the write-back disagreeing by rounding; and the textbook
"smallest scale reaching the target" rule overshooting below Nyquist so a fence
becomes a solid wall.

**One caveat if mgb64 adopts a default-on remaster mode alongside this:**
enabling mipmaps and anisotropy by default changes rendering for every test that
does not pin a mode. In mdkr64 exactly one check of twenty-eight noticed — a
shadow A/B asserting that touched pixels only ever get darker — and the fix was
to pin the reference presentation in that check, not to loosen its tolerance.

## Clipped fog must be re-derived, not byte-interpolated (renderer-fog wave)

mdkr64 reproduced a persistent, subtle dark/bright region over the lower third
of a driving view. Primitive provenance proved it was a large fogged terrain
triangle crossing the near plane, not the vehicle shadow. Its CPU clipper
linearly blended `LoadedVertex.fog` at the generated intersection even though
that byte had already been calculated from post-divide `z/w` and clamped.
The shader then used the default perspective interpolation for what is an RDP
screen-space coefficient.

The both-direction capture was unusually clean:

- legacy clipped-fog byte + perspective GPU fog: dark foreground reproduced;
- recomputed `fog(new_z / new_w)` alone: gross shadow removed;
- no-perspective fog interpolation alone: gross shadow removed;
- both corrections: stable nearby road/sand across the moving sequence.

mdkr64 now re-derives fog for clip-generated vertices and always sets
`SHADER_OPT_NOPERSPECTIVE_FOG` on fog draws. MGB64 already has the shader flag,
but `gfx_loaded_vertex_lerp()` still visibly lerps `dst->fog` from the two bytes.
That is a concrete audit lead, not a claim that MGB64 currently exhibits the
same symptom: check whether its later `fog_coord` path supersedes the byte for
every production material, and reproduce before changing it.

## ROM-derived high-resolution font contours (renderer-font wave)

MGB64's current font path supersamples an 8–13-pixel source glyph with bilinear
filtering, and its own v0.4 backlog correctly notes that this becomes smooth but
not truly crisp. mdkr64 now has a ROM-free, backend-neutral alternative in
`platform/fast3d/gfx_font_sdf.{c,h}`: a bounded signed-distance reconstruction
creates a 4x alpha contour while alpha-weighted colour sampling preserves the
authored multicolour glyph interior.

The important integration details are reusable even though DKR's atlas format is
different:

- identify font sources at the font loader, never by texture dimensions;
- carry exact glyph-cell regions and derive each cell independently so adjacent
  atlas entries cannot contribute to one another;
- keep logical texture dimensions for the existing glyph UV/metric contract
  while uploading the larger physical texture;
- make remaster state part of the texture-cache key;
- use the prefiltered 4x coverage with point, clamped, level-zero sampling so a
  hardware bilinear tap cannot cross a packed glyph-cell boundary;
- invalidate derived cache entries whenever shared registry metadata changes;
- unregister before freeing the source allocation;
- retain a legacy-mode gate and a ROM-free contour/overflow unit test.

This is directly relevant to MGB64 backlog M4.2, but its CI/I8 glyph source and
94-glyph bank mapping still need their own measured integration.

## Transactional assets and exact MIPS audio math (core-safety wave)

Two reusable findings came out of closing MDKR64's remaining core-safety audit.

First, cache capacity is only one boundary in an asset constructor. The safe
transaction is: resolve/fallback the ID, reserve a cache destination, validate
the serialized span, compute the complete aligned allocation (including every
backend command sidecar), reserve shared palette/storage capacity, build, and
commit cache/count state last. Every failure after shared-state reservation must
restore that state. MGB64 should apply this ordering to each texture/model/audio
cache and add exact-capacity plus cap+1 and injected-failure gates. A useful ROM
format warning: a final compressed record may name alignment padding or use a
zero next-record stride; validate those as explicitly final-only conventions,
not as blanket malformed data or blanket exceptions.

Second, copied libultra C is not a sufficient specification for narrowing
floating-point conversions. MDKR64's original US v80 ELF shows `_getRate`
lowered through MIPS `cvt.w.d` followed by low-half extraction, including the
signed indefinite word for invalid/out-of-range input. The portable repair
models that word operation directly and reconstructs signed 16.16 through
unsigned halves plus a bit-preserving copy. If MGB64 carries the same synth,
compare its target ELF/library assembly before choosing clamp or C-cast
semantics, then gate the observed out-of-range values and PCM output.

The address-arithmetic corollary is equally transferable: serialized 32-bit
tokens, byte offsets, and real host pointers are different types of value.
Never create a low-address C pointer solely to perform N64 modulo arithmetic;
keep the token unsigned until the checked resolve boundary.

## Preserve baked ambient while adding smooth, linear-light directionality

mdkr64's first relight experiment found that replacing baked vertex colour with
a fixed directional model was 2.5–3.5× farther from authored bright and dark
captures and exposed coarse triangle facets. The production repair is a useful
pattern for MGB64, although its header fields and model-normal formats must be
re-audited rather than copied:

- derive a stable rig from normalized level-owned sky/fog/weather/colour data,
  with a deterministic identity fallback instead of a hand-authored stage table;
- normalize the rig's exposure and cap its additive strength, leaving baked
  vertex colour responsible for low-frequency mood;
- carry the model's existing smooth normals to the fragment shader; never infer
  a face normal from screen derivatives on low-poly characters;
- rotate the stable world sun into each object's local space when normals are
  model-local, and queue that direction with the draw rather than reading mutable
  object state later;
- cross one documented colour boundary: complete the legacy combiner in
  authored sRGB code space, decode its result, apply diffuse and fog in linear
  RGB, then encode once to the UNORM scene target;
- gate the entire path by presentation mode, with exact legacy reverse arms.

The portable artifact is `gfx_level_lighting.{c,h}` and its ROM-free transfer /
derivation / packing test. The native-only F3DDKR MoveWord indices and DKR's
`BATCH_VTX_COL` compact-normal cursor are game-specific. MGB64 should first
inventory which object classes own genuine smooth normals and which lighting
fields are stable for a loaded map.

## WebGPU lifecycle, capability, and recovery backflow (2026-07-27)

MDKR64 implemented the shared audit findings WGPU-02/03/04/06 and found one
shell-specific integration defect while proving them. The portable pieces to
carry back are:

- a WebGPU-header-free surface lifecycle policy for optimal, persistent
  suboptimal, timeout, outdated, lost, and repeated-error states;
- capability-derived surface usages, with a fullscreen render-pass present path
  when `CopyDst` is unavailable and offscreen-only readback when `CopySrc` is;
- null-safe encoder/pass/finish unwinding with deterministic injection at each
  point;
- callback-only device-loss latching and all teardown/reinit at a complete-frame
  boundary;
- re-query surface alpha/present/usage capabilities after recreation, and
  escalate a surface-format change because pipelines are format-bound;
- distinct persistent result storage for a device request and its retry, so a
  late timed-out callback cannot overwrite the retry;
- runtime telemetry for granted attribute/varying limits, shader-table overflow,
  and pipeline-creation failure.

MDKR64's native policy is one device rebuild followed by a live OpenGL switch.
MGB64 should adapt that to its own host/overlay ownership rather than copy the
platform glue literally. Browser builds instead need a stable error UI. During
this wave MDKR64 discovered its C seam still called
`window.mgb64ShowError`, but its actual shell defined no such handler; the error
therefore remained a black canvas. MGB64 should verify — not infer — that its named
handler exists and is exercised by an engine-level failure injection.

The real Chromium gate also proved the attachment-only render-blit path over
3,600 frames. MDKR64's route measured 14/1,024 shaders, 6/16 attributes, 5/16
varyings, zero shader-table overflow, and zero pipeline failures. This does not
close WGPU-11 for either project; MGB64's larger material corpus and a synthetic
overflow-safe frontend-handle test are still required.
