# Open items — Browser (wasm) build

> Part of the split [open-items index](README.md). Every defect this port has
> hit is recorded with its mechanism, the measurement that found it, the fix, and
> the check that would catch a regression. **Nothing is deleted when it is
> fixed** — a closed entry is the only warning the next person gets that the same
> trap exists, and retractions are recorded in place rather than removed.


## FIXED: wasm call signatures disagreed across translation units — wave "wasm-abi"

A clean wasm link warned about two incompatible call contracts:

- the game defined `f32 log(f32)` while SDL/libc supplied `double log(double)`;
- `waves.c` called `mdkr_bound_probe()` without a declaration, so C's legacy
  fallback gave the call an `int` return type while the definition returns `void`.

Both are real wasm ABI defects even when a particular call happens to survive:
WebAssembly function types include the return and parameter types, and the linker
was producing a runnable-looking module after warning about the mismatch.

The vehicle helper retains its original `log` name only in the non-port matching
build and is namespaced as `dkr_vehicle_logf` in native/wasm builds. Bounds and
trace probes now use small, libc-free shared headers. Enabling
`-Werror=implicit-function-declaration` then found three more hidden declarations
(`strchr` and two trace calls), plus web code that compiled calls to the
desktop-only `sdl_init_gl()` after an unconditional Emscripten return. Those are
all declared or preprocessor-separated now; the web window creation failure is
also checked instead of accepted.

The class is locked down at both seams:

- every Clang/Emscripten compile treats an implicit call as an error;
- wasm-ld runs with `--fatal-warnings`, so a cross-TU signature conflict cannot
  ship as a warning.

A full clean native build and a full clean wasm build pass these gates. The wasm
object exports `dkr_vehicle_logf` and imports a declared `mdkr_bound_probe`; the
audio check is byte-for-byte deterministic in Debug and Release, the segment
high-water report remains live with zero clamps, and the 26-entry wasm wave-table
layout check passes.

The analogous decomp/libc namespace and implicit-call audit is recorded in
[`MGB64_BACKFLOW.md`](../MGB64_BACKFLOW.md).

## FIXED: browser "memory access out of bounds" in the wave renderer — wave "wavetable"

**The first crash ever reported by a real player**, on the browser build. Their
trigger: several races, then switched to the plane and flew around. Then:

```
RuntimeError: memory access out of bounds
    at mdkr64_web.wasm:0xa50b7
    at mdkr64_web.wasm:0xaa247
    at mdkr64_web.wasm:0x128992
    at wrapper (mdkr64_web.js:7:205731)
    at Object.doRewind (mdkr64_web.js:7:207567)
```

Build: source `63c5d32`, wasm md5 `348a9d80edbef2a3d0b60c5cef9885f9`.

### Recovering the crashing binary (do this first, next time)

The demo repo is a publication target that is overwritten on every publish, so the
wasm on disk was *already gone*. It was **not** lost: that repo is a git repo and
each publish is a commit naming its source commit, so the exact artifact came back
out of history —

```bash
git -C ../golden-balloon-demo log --oneline -- mdkr64_web.wasm
git -C ../golden-balloon-demo show b4eb376:mdkr64_web.wasm > shipped_63c5d32.wasm  # md5 matches
```

`mdkr64_web.js.symbols` was first published one build *later* (`4d1d75b`, source
`fc42ee7`), so no same-link symbol map for `63c5d32` exists or ever will.

### Symbolication — what actually worked

Two earlier attempts are recorded as dead ends in `tools/web/symbolize_crash.py`
(`--profiling-funcs` and a plain `--emit-symbol-map` rebuild both change codegen, so
function indices are meaningless). A third also failed and is worth recording:
**aligning the two builds' body-size *sequences* with difflib gives only 15.7 %
ordinal agreement** — emcc reorders functions wholesale, so position is unusable.

What worked was a **body-size join against another *published Release* build**
(`fc42ee7`, which does carry a same-link map), then confirming it on **bytes**:

| frame | body size | unique exact-size match in `fc42ee7` | body bytes identical |
|---|---|---|---|
| `0xa50b7` | 17798 | `initialise_player_viewport_vars` | 93.2 %, all diffs short LEB immediates |
| `0xaa247` | 27422 | `render_scene` | 90.9 %, same |
| `0x128992` | 142733 | `main` (142882, +149; only body within 2 %) | 6.3 % — Asyncify state machine, expected |

`game/src/tracks.c` was untouched between the two builds, which is why the two inner
bodies are near-identical, and **the faulting instruction sits at byte-identical
offset +6210 with identical surrounding opcodes in both**. That is proof, not
inference. `llvm-objdump -d` (emsdk) prints wasm byte offsets, so the trapping
instruction reads straight out of the shipped binary:

```
a50a8: local.get 4 ; a50aa: i32.load 8      ; D_8012A5E8[k].unk8
a50af: i32.const 231208 ; a50b3: i32.load 0 ; D_800E3178
a50b6: i32.add
a50b7: i32.load8_s 0                        <-- FAULT
a50d3: i32.store 8                          ; D_8012A5E8[k].unk8++
```

= `waves.c` `func_800B92F4()`: `var_v0 = D_800E3178[D_8012A5E8[k].unk8];`. Neighbouring
`call`s resolve to `coss_f`/`sins_f`, and the enclosing `i < 8 && unk0[i] != 0xFF`
loop is `waves_get_y()` inlined. `render_level_geometry_and_objects`,
`waves_visibility`, `func_800B92F4` and `waves_get_y` all inline into
`initialise_player_viewport_vars`, which is why a 40-line function has a
17798-byte body.

### Root cause

`waves.c` treats `D_8012A5E8[2]` and `D_8012A600[24]` as **one 26-entry table**:

- `waves_visibility()` sentinel-clears all 26 slots — its reset loop names both
  arrays explicitly (`D_8012A5E8[0]`, `[1]`, `D_8012A600[v..v+3]`);
- it then appends every visible wave block through `D_8012A5E8[var_a3]`, indexing
  the *first* array straight on into the second;
- `func_800B92F4()` / `func_800B97A8()` walk
  `for (k = 0; D_8012A5E8[k].blockID != -1; k++)` over the same 26 slots.

As two separate C objects that only holds if the linker places them adjacently, and
**whether it does is luck**:

| target | `&D_8012A5E8` | `&D_8012A5E8[2]` | `&D_8012A600` | gap |
|---|---|---|---|---|
| Mach-O arm64 | `…a78` (8 mod 16) | `…a90` | `…a90` | **0** — works |
| wasm-ld (shipped `63c5d32`) | 231504 (0 mod 16) | 231528 | 231536 | **8 bytes** |

Both linkers give the 288-byte `D_8012A600` a 16-byte preferred alignment. Native
starts `D_8012A5E8` at 8 mod 16, so its 24 bytes *end* 16-aligned and no padding is
needed. wasm-ld starts it *at* 16, so its 24 bytes end at 8 mod 16 and `D_8012A600`
is padded up — an 8-byte hole. Slots 2..25 then land 8 bytes **below** the slots the
reset loop cleared, the `blockID != -1` walk never reliably terminates, `k` runs off
the table, and `unk8` is read from arbitrary memory. Indexed into the small
`D_800E3178` allocation that address leaves linear memory ⇒ wasm traps.

**Why the plane, and why "after a while".** Only 7 of the 20 playable tracks reach
`func_800B92F4` at all: it is gated on `gWaveController.xlu` (`LevelHeader.wavesXlu`),
and the other branch, `func_800B97A8()`, never touches `D_800E3178`. The walk also
has to get past slot 1 before the layout matters. Measured natively (probe on
`build/mdkr64`, `MDKR_LOAD_TRACK`, autopilot):

| track | max `var_a3` | max `k` | max `unk8` | `D_800E3178` size | native gap |
|---|---|---|---|---|---|
| 19 | **25** | 23 | 1175 | 1176 | 0 |
| 8 | 23 | 22 | 1074 | 1225 | 0 |
| 10 | 16 | 15 | 383 | 384 | 0 |
| 31 | 16 | 15 | 2024 | 2349 | 0 |
| 4 | 9 | 8 | 962 | 1053 | 0 |
| 30, 20 | 6 | 5 | 207 / 924 | 400 / 1200 | 0 |

So the 26-slot contiguity is **routinely load-bearing** (all 26 slots get used), and
a correct `unk8` already sits within a byte or two of the allocation limit. A plane
is the worst case for `var_a3`: from the air many wave segments are visible at once,
so the table fills and the walk goes deep — which is exactly the reported trigger.

### Why nothing caught it

- **Native cannot fault**: the gap is 0, by luck.
- **ASan found nothing** in 11 plane tracks × 7500 frames: both arrays are
  `__DATA,__common` tentative definitions, which ASan does **not** redzone.
- UBSan `-fsanitize=array-bounds` *did* flag it —
  `waves.c:535-539 index 2 out of bounds for 'unk8012A5E8[2]'`, and index 3 at
  `waves.c:691-710` — but only as an out-of-bounds *index*, with no crash, on a
  target where the memory it reached was benign. That is the tell that was there
  all along.

### Fix

`game/src/waves.c`, gated by `NATIVE_PORT`: back both names onto one 26-entry array
via a union, so contiguity is guaranteed by the language rather than by the linker,
on every target. `D_8012A600` stays a 24-element array so
`ARRAY_COUNT(D_8012A600)` in the reset loop is unchanged, and
`D_8012A5E8[0..25]` is now genuinely in bounds — the UB is gone too. Two
`_Static_assert`s lock the offset and the total size. Nothing outside `waves.c`
references either symbol.

### Verification

- `tests/check_wave_visible_table.py` — the 26 sentinel-clearing stores in the built
  wasm must form one stride-12 run. **Both directions, on real artifacts:**
  - fix applied → `run len=26 232680..232980` → PASS
  - fix reverted (`build-web-base`) → `run len=2 232688..232700 | GAP 8 | run len=24 232720..232996` → FAIL
  - the actual wasm that crashed the player (`63c5d32`, md5 `348a9d80…`) → FAIL
- Native behaviour is unchanged, as it must be (the arrays were already adjacent):
  frames byte-identical pre-fix vs post-fix on tracks 19, 8 and 31, 3000 frames each.
- `check_race_drive.py` PASS; `check_vehicle_sweep.py --tracks 8,4,10,30,19,20,31`
  PASS (13/13) — i.e. every track that actually executes `func_800B92F4`.

### The player's SECOND crash was the SAME defect — proven, not inferred

A second `memory access out of bounds` arrived from the LIVE build (source `9c643a0`,
wasm md5 `a08bbcb02f09bee2a13208d0f7adeae5`) — after the plane fix, before the fix
above. Normal gameplay this time, no plane.

```
at mdkr64_web.wasm:0xad5a1   initialise_player_viewport_vars  +7832 of 17798
at mdkr64_web.wasm:0x957d1   render_scene                     +8214 of 27422
at mdkr64_web.wasm:0x1020b5  main                             +114653 of 142882
```

That build ships a same-link `--emit-symbol-map`, so this symbolication is exact
(`tools/web/symbolize_crash.py`). `render_scene` is at **+8214, byte-identical to
the first crash** — same call site — but `initialise_player_viewport_vars` faults at
**+7832**, a different instruction from the first crash's +6210.

**The faulting site.** `llvm-objdump` on that exact wasm:

```
ad55a: i32.const 232376 ; i32.load 0      ; gWaveModel
ad566: i32.const 233280 ; i32.load16_s    ; gWaveBlockIDs[i]
ad56e: i32.const 28 ; i32.mul ; i32.add   ; &gWaveModel[gWaveBlockIDs[i]]   (local 8)
ad574/ad57f/ad58a: i32.load16_s 4 / 6 / 8 ; transform.x/y/z_position
ad591: i32.const 232372 ; i32.load 0      ; D_800E30D4
ad59a: local.get 8 ; i32.load 12          ; ->unkC   (LevelModel_Alternate +0x0C)
ad5a1: i32.load 0                         <-- FAULT
```

= `waves.c` `waves_render()`: `sp104 = D_800E30D4[spE0->unkC];`. `LevelModel_Alternate`
is 0x1C = 28 bytes with `u32 unkC` at 0x0C, matching exactly. So the bad value is the
**block id read out of `gWaveBlockIDs`** — nothing to do with the `unk8` at +8 that the
first crash read.

**Why it is nevertheless the same bug.** The walks do not only *read* past the table
when the contiguity invariant breaks — they **write**: `func_800B92F4()` does
`D_8012A5E8[k].unk8++`, `func_800B97A8()` does `unk8 += subdivisions`, for every
matching entry. In the shipped module `gWaveBlockIDs` sits at **+592** from
`&D_8012A5E8` (233280 − 232688), and `D_8012A5E8[k].unk8` is at `12k + 8`. So
`12k + 8 >= 592` ⇒ **k >= 49** puts the walk's own store inside `gWaveBlockIDs`.

Note also *why* the walk cannot terminate once the hole exists: the reset loop clears
sentinels at `232720 + 12j`, the walk reads `232688 + 12k`. For k >= 2 the walk's
addresses are `232712 + 12m`, and `12(m − j) = 8` has no integer solution — **the walk
and the sentinels are permanently 8 mod 12 apart and never coincide again.** It stops
only if that uninitialised memory happens to hold `0xFFFF`.

**Reproduced natively** by emulating the shipped browser layout exactly — `head[2]`,
an 8-byte `hole`, `tail[24]`, then `blockIDs[512]` at +592 — and toggling only the hole:

| variant | max k | deepest *write* k | render index | tracks 8 / 10 / 30 |
|---|---|---|---|---|
| hole present (= shipped) | **1199** | **50** → `gWaveBlockIDs[8]` | `blockID=60` vs `nseg=49`, `unkC=0x9a009a00` | **Bus error / SIGSEGV / SIGSEGV** |
| hole removed (= the fix) | ≤ 22 | ≤ 22 (inside the table) | always in range | exit 0 |

`12·50 + 8 = 608`, and `(608 − 592)/2 = 8` — the store lands on `gWaveBlockIDs[8]`, and
the out-of-range render index was reported at **`i=8`**. `0x9a009a00 × 4` is ~9.6 GB
past `D_800E30D4`, which is why wasm traps and the 16 MB-arena native build mostly did
not. Exact, independent confirmation of the chain:

> hole → walk desynchronised from the sentinels → walk runs away → its own `unk8`
> store corrupts `gWaveBlockIDs[8]` → `waves_render()` indexes `gWaveModel` past its
> array → `D_800E30D4[garbage]` → out of bounds at +7832.

**Verdict: one defect, two faulting instructions.** `tests/check_wave_visible_table.py`
already **FAILS against `a08bbcb0…`**, the binary that produced this second trace, as
well as against `348a9d80…`. No third fix was needed.

**Hardening added anyway.** Both walks are now bounded by `WAVE_VISIBLE_SLOTS`
(`WAVE_VISIBLE_WALK_LIVE`). It is provably a no-op — max k measured **24 of 26**, and
frames are byte-identical to the unbounded build over 3000 frames × tracks 19, 8, 31 —
but it converts a layout mistake from *silent corruption of an unrelated array three
subsystems away* into bounded degradation. Positive control: with the hole emulated
**and** the bound in place, all 7 wave tracks exit 0 instead of Bus error / SIGSEGV ×2.

### Still open from this wave

- **The same idiom may exist elsewhere.** This is a *class*: a decomp that splits one
  ROM array into two C objects and then indexes across the boundary. Two candidates
  were seen; both are now dispositioned (verified 2026-07-29):
  - `game/src/camera.c` `gCameraRelPosStackZ` carries a NATIVE_PORT containment:
    Z is sized `[CAMERA_MODEL_STACK_SIZE + 1]` to match X/Y with a
    `_Static_assert`, and the in-source measurement records peak
    `gCameraMatrixPos = 1` across 9 tracks, attract, 2P, the Adventure loop, and
    the menu graph under UBSan array-bounds (see the comment at the definition).
  - `game/src/objects.c` `func_80016748`'s `f32[4][3]`-for-`MtxF` is behind
    `#ifdef AVOID_UB`, and every native build configuration defines
    `AVOID_UB=1` (`CMakeLists.txt:76/109/143/217/582`), so the port compiles the
    real `MtxF` arm; the hack remains only for the matching N64 build.
  A sweep for adjacent same-type arrays indexed past their own bounds remains the
  right follow-up instrument for the class.
- The crashing wasm is **not committed** (build artifact); recover it from the demo
  repo as shown above. **The web build is bit-reproducible** — a local pre-fix build
  came out byte-identical to the shipped `a08bbcb0…`, which is what made the native
  layout-emulation experiment trustworthy. Keep it that way.
- **Two unguarded reads found while chasing the second crash, deliberately NOT fixed
  — reachability was measured and came out zero, so fixing them would be guessing:**
  - `waves.c` `waves_block_hq()`: `while (indexNum < gNumberOfLevelSegments && block !=
    gWaveModel[indexNum].block) indexNum++;` then reads `gWaveModel[indexNum].unkC`
    **unconditionally**, so a failed search reads one entry past the array — which
    overlays `gWaveGenList[0]`, whose +0x0C is `f32 z_position`. A float reinterpreted
    as a `u32` index is ~4 GB, i.e. a guaranteed wasm trap. It also *stores* the
    out-of-range `indexNum` into `gWaveBlockIDs`. Measured: **0 misses** in 20 tracks ×
    6000 frames and across the Adventure hub→race→hub loop (`hqCalls` up to 66401).
    Latent, not reachable in anything tested.
  - `tracks.c` `free_track()` calls `waves_free()` (which NULLs `gWaveModel` and
    `D_800E30D4`) but **never zeroes `gWaveBlockCount`**, which is the guard on both
    `waves_block_hq()` and `waves_render()`. `gWaveBlockCount` is only reset in the
    *next* level's setup, so there is a window where the guard says "waves are live"
    while the pointers are NULL. Measured: **0 occurrences** — no render happens in
    that window in any fixture. Latent.
  - Also noted: `D_800E30D4 = mempool_alloc_safe(gWaveTileCountX * gWaveTileCountZ *
    sizeof(uintptr_t), …)` is upstream decomp text (correct on the N64, where
    `sizeof(uintptr_t)` is 4 and matches its `s32` elements). On LP64 it accidentally
    over-allocates 2×, giving native a cushion that wasm32 does not have. Left alone —
    it is not a bug, but it is one more reason native masks overruns here. Measured
    `unkC` max 141 against a 165-element limit, and 59 against 64: in range, but tight.

## M8 web (wasm) build — DONE (boots, runs, RENDERS correctly in-browser)
The Emscripten/WebGPU build boots in a real browser, runs the full game loop at
60fps (Asyncify/rAF frame boundary), and RENDERS the title/attract, menus, and a
race CORRECTLY — verified in headless Chrome 150 driving the actual wasm + scored
vs native `--dump-frames`. WebGPU device init, AudioWorklet, ROM in MEMFS, and IDBFS
saves all work (see STATUS.md M8 + docs/architecture/web.md). THREE wasm32-specific
address/layout bugs were found + fixed (all LP64-only assumptions re-surfacing at
32-bit pointer width — the architecture decision 8 watch-item):
- [x] **Arena / N64-segment-token address collision (the render blocker).** On LP64
  the 16 MB arena sits at a high 64-bit address (0xNNN000000) whose low-32 never
  collides with DKR's segment tokens (0x0N000000, segments 0-15). On wasm32 (ILP32)
  the arena landed at 0x02000000 — inside the segment-token space — so dkr_resolve's
  arena reconstruction swallowed the framebuffer/z-buffer segment tokens (SETCIMG
  0x01000000 -> NULL, SETZIMG 0x02000000 -> arena base) and NO scene geometry
  resolved. FIX (stubs_dkr.c dkr_arena_init, `#ifdef __EMSCRIPTEN__`): posix_memalign
  the arena above the 256 MB segment ceiling (0x10000000) so segment tokens always
  fall through to the segment table and arena addresses never look like a token. No
  change to the shared resolver; native unaffected. Costs ~256 MB of wasm address
  space (heap grows to ~272 MB, under the 512 MB MAXIMUM_MEMORY) — a follow-up could
  use a low-address-avoiding allocator to reclaim it.
- [x] **Z-clear FILLRECT drawn white** (gfx_pc_dkr.c dkr_dp_fill_rectangle). The
  depth-clear skip compared RESOLVED SETCIMG/SETZIMG pointers with a `!= NULL`
  guard; on wasm32 the framebuffer segment token resolves to NULL (it is NOT a
  >4GB registered host pointer as on LP64 — see below), so the guard failed and the
  z-clear drew as an opaque white fill over the scene (the M3b symptom). FIX:
  compare the RAW SETCIMG/SETZIMG tokens (0x01000000 vs 0x02000000) instead —
  width-independent, identical result on native (attract title re-verified).

- [x] **RESOLVED — the white sky / menus-not-drawn: wasm32 global/rodata pointer
  recovery.** The remaining fidelity gap was the predicted gfx_ptr registry
  gap: on LP64 a non-arena host pointer (game global / rodata display list) is a
  >4GB address that `dkr_k0_to_physical` registers, so `dkr_resolve` recovers it from
  the registry; on wasm32 ALL pointers are <4GB so the `>0xFFFFFFFF` threshold never
  fires and the globals never register -> `dkr_resolve` returned NULL for them -> the
  sky-gradient / menu / kart geometry that lives in globals+rodata DLs never drew
  (white sky). Registration alone can't fix it (globals ALSO reach DLs via
  `(s32)ptr+K0BASE` / raw casts that bypass the registration functions). Verified
  in-browser (headless Chrome CDP diag): globals live low in wasm static data
  (~0x18330), referenced as flipped tokens `0x800183xx` or raw `0x000183xx`, all
  resolving to NULL. FIX (gfx_pc_dkr.c `dkr_resolve`, `#ifdef __EMSCRIPTEN__`):
  recover host pointers DIRECTLY from the token — on ILP32 the flip is reversible, so
  a bit-31-set token XORs back to the real address, and a raw value < 0x01000000 is a
  low global (segment-0 base is 0). Segment tokens (0x01000000..0x0FFFFFFF) still fall
  to the segment table; the registry/arena checks still run first. No registry needed
  on wasm32; native is byte-identical (LP64 keeps the registry path). VERIFIED in a
  real browser (headless Chrome 150, actual wasm) + compared to native `--dump-frames`:
  attract Ancient Lake (blue sky/clouds/palms/water) hist=0.996 block=1.000; OPTIONS
  menu (OPTIONS/ENGLISH/SUBTITLES ON/AUDIO OPTIONS/SAVE OPTIONS legible) hist=0.996
  block=1.000; in-race (Ancient Lake track + orange canyon + palms + HUD 5TH/LAP
  1/3/x0/TIME) renders correctly (eyeballed; the race comparator scores noisily as a
  dynamic scene at unsynced frames). Native re-verified unregressed: 20x race 0
  crashes, WebGPU renders title+OPTIONS, GL fallback boots.
- The earlier intermittent headless Chrome GPU-process exit has not reproduced in
  the isolated-profile runtime gate. GPU-process/device-loss markers, flat scenes,
  and stalled frame progress are now release failures rather than an informal
  residual. The ~256 MB arena-alignment address cost (from the segment-ceiling
  fix) is still open (reclaimable with a low-address-avoiding allocator).
