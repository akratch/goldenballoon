# Open items — Allocator and native memory layout

> One subsystem of the split [open-items index](README.md), which states how these files are kept.


## FIXED: central allocator boundaries — wave "allocinv"

**Audit ID:** MEM-05
**Implementation:** `07ea545`

Nine allocator hazards shared one missing-boundary mechanism. Ordinary terminal
frees formed pointers to `slots[-1]`; the 257th delayed free wrote out of bounds;
invalid and overflowing sizes reached align/list arithmetic; the "safe" API
returned null to callers that assume it cannot; pool lookup compared unrelated
pointers and silently classified foreign addresses as the main pool; fixed
allocation and 4/8-byte alignment narrowed host pointers; successful frees
printed failure; and the inactive interrupt shim returned an indeterminate mask.

Native and browser allocation now validate positive representable sizes, pool
geometry, pool identity, and host-width ranges before mutation. Fixed-address
allocation calculates whether it needs zero, one, or two list splits and rejects
insufficient metadata without partially changing the pool. Exact-fit allocations
remain legal at slot capacity. The delayed-free queue grows before writing its
257th entry, preserving every queued object's complete lifetime. Allocation
failure in the safe API is diagnosed where it occurs and terminates there.

The ROM-free `memory_allocator` CTest drives only/head/middle/tail frees,
previous/next/both coalescing, 257 queued frees through two ticks, foreign and
one-past pointers, invalid/overflowing sizes and timers, byte/slot exhaustion,
transactional fixed allocation, and high-half pointer alignment. It passes
Debug, Release, ASan, and UBSan. Complete builds pass in all four native
configurations and wasm32/WebGPU.

## FIXED: native representation boundaries: object tails, level records, and render proxies — wave "nativelayout"

**Audit IDs:** MEM-02, MEM-03, MEM-04
**Implementation:** `a7cdb95740baa2e367c58d68e1030a7a3ed543d8`

### What was actually wrong

Three apparently separate bug lists had one host-ABI cause:

1. `spawn_object()` built `Object`'s optional tail by advancing a byte pointer
   using N64 four-byte size/alignment assumptions. LP64 pointer-bearing
   `ShadowData`, `WaterEffect`, `ObjectInteraction`, `ObjectCollision`,
   `AttachPoint`, and `ParticleEmitter` objects could begin at 4 mod 8. The light
   array reserved four bytes per host pointer, and the final size subtracted
   endpoints after narrowing them to `s32`.
2. Object maps are streams of even, variable strides. A legal 10-byte record
   makes the next record start at 2 mod 4, but `LevelObjectEntry_Hud`'s `s32`
   raised the complete union's natural alignment to four. The loader trusted
   embedded lengths and object IDs before behavior-specific typed dispatch.
3. HUD, menu, particle, weather, boost, and bubble records were cast to
   `Object *`/`ObjectSegment *` because their first 24 bytes looked similar. The
   renderer also reads the animation frame immediately after that prefix. A bare
   lens-flare `ObjectTransform` therefore over-read its real stack object and
   depended on the next local/global byte.

The first halt-on-error alignment run reached the new-save route and stopped at
frame ~2052 on misaligned `ShadowData`. Recover mode reported **88** native
alignment violations across these three classes. Those diagnostics are evidence
of the old code, not an allow-list in the fixed build.

### Coherent repair

`game/src/object_layout.c` owns a small ROM-free checked cursor. Sizing and
placement are now one operation for `Object`, the full model-pointer array,
behavior storage, every optional typed tail, every emitter, and every light
pointer. It checks power-of-two alignment, base/address arithmetic, addition,
multiplication, capacity, final alignment, and allocator narrowing. After the
blueprint is copied, every pointer is rebased and its required alignment is
asserted.

The object-map boundary now validates:

- an eight-byte common header;
- even stride and `stride <= bytes_remaining`;
- complete translation-table object-ID range;
- translated object-header range;
- behavior resolution and behavior-specific minimum size.

The serialized HUD view is explicitly packed to the stream's real two-byte
contract and locked at size 12, `offsetY` byte 8, alignment 2. Alternating
10/12/14-byte unit records exercise both modulo-four starts and every rejection.

That census exposed one additional real over-read. Retail `BHV_BOOST` map records
are eight bytes, while `racerfx_alloc()` creates a ten-byte private form carrying
`racerIndex`. The old initializer always read byte eight. On the measured title
map that byte was the next record's `0x94`, so it formed an out-of-range boost
table pointer that happened to stay dormant. Native code now defaults the map
form to controller zero, reads the field only from the ten-byte dynamic form, and
rejects an index outside the character table.

Finally, native billboard/orthographic render APIs take an actual
`ObjectTransform` and explicit frame. Genuine objects retain wrappers; small
records pass their real fields. Matching/N64 code retains its original calls.
The native source gate rejects any reintroduced fake renderer cast.

### Both-direction proof

`tests/test_object_layout.c` has three exact legacy arms:

| arm | required broken-direction result |
|---|---|
| MEM-02 object-tail shape | alignment UBSan emits **4** reports |
| MEM-03 HUD-record shape | alignment UBSan emits **2** reports |
| MEM-04 bare transform as `Object` | ASan aborts with `stack-buffer-overflow` at the animation-frame read |

The fixed unit passes in Debug, Release, alignment UBSan, and ASan. The registered
`tests/check_native_layout.py` also verifies that the sanitizer handlers really
reached the linked binaries, then runs halt-on-first-error alignment UBSan over:

- all nine menu routes;
- all 20 tracks;
- all 47 legal track/vehicle combinations;
- Adventure hub traversal and the complete hub→race→hub loop;
- boss/object-collision fixed and legacy behavior;
- 21:9 two-player split screen;
- measured three-/four-player racer, quadrant, minimap, and results flow;
- the seven-arm HUD/world balloon aspect/FOV fixture.

Every arm passes. The final fixture carries the player-facing refinement: routing
all small sprite records through the new field API still preserves HUD and
world-balloon proportions at 4:3, 16:10, 16:9, 21:9, forced 4:3, and 75-degree
FOV; only explicit legacy stretching is distorted, and that arm must be rejected.
The three-/four-player runtime fixtures now pass separately through
`check_race_multiplayer.py`; they are not unresolved native prefix casts.

The standard Release-led manifest at `7a7f2f7` passes all **38/38 tasks in
19m51s**, with the native-layout task taking 11m50s. An earlier invocation had
one anonymous signal-6 exit on the last alignment vehicle row after the other 46
passed. That exact route passed 20/20 immediate repetitions and the complete
layout task passed on a strict rerun. The sweep had captured but suppressed its
child log; it now always prints failure tails and recognizes UBSan text. No
retry, skip, or tolerated failure was added, and the next standard complete
manifest passed the row and all 38 tasks.

The v0.2 release gate later passed the current **37-task optimized
native/sanitizer stage in 21m09s**, the **28-task Debug primary stage in
11m43s**, and both wasm-only tasks — including real Chromium — in **1m05s**.
