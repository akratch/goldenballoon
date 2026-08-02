# Open items — 64-bit (LP64), endianness and portability

> Part of the split [open-items index](README.md). Every defect this port has
> hit is recorded with its mechanism, the measurement that found it, the fix, and
> the check that would catch a regression. **Nothing is deleted when it is
> fixed** — a closed entry is the only warning the next person gets that the same
> trap exists, and retractions are recorded in place rather than removed.


## FIXED: tagged macOS artifact exposed a stale magic-code endian failure

The report had two coupled symptoms: every submitted code ended at `BADCODE`,
while the Code List rendered `8` and a blank row as enabled. A clean reproduction
on pre-fix commit `6f5515f` identified one cause for both: after decryption, the
mixed magic-code table still held a big-endian u16 count and offsets. A
little-endian host therefore read 29 cheats as 7424 and the enabled-code string
offsets 187/220 as 47872/56320, walking unrelated bytes for both lookup and text.

The original decrypt-then-normalize repair is already an ancestor of the
`v1.0.0` tag, and clean tag/current builds both accept `ARNOLD` and render
`BIG CHARACTERS — ON`. No hosted `v1.0.0` release or workflow artifact exists,
and the tag-era macOS script permitted packaging an arbitrary prebuilt binary;
the reported DMG therefore cannot be authenticated as a build of the tag and is
consistent with a stale pre-fix local executable.

The runtime boundary is now transactional and fail closed: after normalizing the
plaintext u16 index, it verifies the self-describing first offset and every
code/description offset plus its in-blob NUL terminator. Any failure restores
the complete original byte representation and stops asset loading. The
`magic_codes` CTest pins the reported endian values, valid `ARNOLD` lookup and
description, damaged offsets, unterminated strings, and full rollback;
`check_asset_swap_invariants.py` independently pins decrypt-before-normalize
ordering with a reversed-order control. The `check_nav_fixtures.py` runtime arm
additionally enters `ARNOLD` through the real onscreen keyboard and requires
cheat id 4 to be accepted, then enters the near miss `ARNOLE` and requires it to
be rejected. The protected macOS pipeline now binds
the downloadable DMG to its commit, version, platform, and checksum.


## FIXED: macOS packaged app was reported as “damaged”

The report was distinct from the expected unidentified-developer warning: on an
M4 Mac mini running macOS 26.2, Finder rejected the app as damaged. The same
failure reproduced locally. The Release executable arrived from the linker with
a valid ad-hoc Apple-silicon signature; `build_app_bundle.sh --bundle-sdl2`
then used `install_name_tool` to rewrite its SDL2 load path and packaged the
mutated bundle without resealing it. `codesign --verify --deep --strict` failed
with an invalid resource/signature envelope and `spctl` rejected it.

The builder now removes inherited source xattrs, signs the bundled SDL2 image,
then seals the outer app ad hoc after all mutations. The intentionally unsigned
1.0.1+ path keeps that ad-hoc identity: the exact app in the read-only mounted
DMG must pass strict nested/resource/load-path verification and a LaunchServices
WebGPU present-and-capture smoke. Its `spctl` result is therefore only the
expected trust rejection for an ad-hoc identity, never a damaged-app rejection.

A separate, optional protected lane can replace that identity inside-out with
Developer ID + Hardened Runtime, require Accepted app and DMG notarizations,
valid staples, mounted-DMG re-verification, and Gatekeeper acceptance. It no
longer disables library validation or W^X speculatively, and the CI contract has
broken-direction controls for its credentials and signing/notarization steps.
That trusted artifact remains publication-disabled until an equivalent
post-sign LaunchServices/WebGPU runtime gate is automated; it is not a
prerequisite for the ad-hoc unsigned patch releases.


## FIXED: MIPS numeric conversion closeout

Three small errors leaned against one another in the port:

- `mtxf_to_mtx()` truncated 16.16 matrix words where the ROM uses `cvt.w.s`
  under the boot FCSR's round-to-nearest-even mode;
- `arctan2_f()` truncated its scaled inputs at the same conversion;
- `atan2_lookup()` added `0.5`, while the ROM computes an integer quotient
  `((numerator << 11) / denominator) & 0xFFE`.

`mdkr_mips_round_w_s()` now implements the MIPS tie-to-even and invalid
conversion contracts without inheriting the host process's mutable floating
point rounding mode. `mdkr_mips_atan_index()` models the assembly's byte offset
as the equivalent element index. `mtxf_to_mtxs()` deliberately remains
truncating because its assembly uses `trunc.w.s`.

The ROM-free runtime-contract test covers positive/negative half ties,
non-ties, NaN/infinity, and more than two million numerator/denominator pairs
against the literal assembly expression. `check_math_tables.py` still matches
all 1,025 arctangent/sine table entries and all 65,536 interpolated sine angles
parsed from the vendored assembly. A 7,500-frame live race completed with
finite steering/pace, live rendering, checkpoint 46, and lap 2.

## FIXED: core safety boundaries — wave "core-safety"

This wave closes the audit's remaining MEM-11, MEM-12, PORT-01, C-01, C-02,
and GAME-06 findings without changing valid campaign behavior.

- Texture loads now reserve capacity and exact command/palette storage before
  construction, validate source/frame spans, unwind palette state on failure,
  and commit cache state last. The ROM supplied two important valid boundary
  cases during the gate: final compressed alignment padding and a zero
  `textureSize` sentinel on a final/single frame.
- Custom reverb initialization owns both temporary allocations, validates the
  audio-table span, and selects a deterministic dry mixer on malformed data or
  allocation failure.
- Level-model offsets remain 32-bit serialized tokens until resolution;
  generated collision/index storage walks checked real host pointers. Object-map
  mutation uses checked `uintptr_t` ranges and `memmove`.
- Original US v80 ELF disassembly settled the audio-envelope question: IDO uses
  MIPS `cvt.w.d` and low-half extraction. Native helpers encode that exact
  behavior, including the indefinite word, and reconstruct signed 16.16 rates
  without shifting a negative integer.
- Pak extensions, course flags, trophy worlds, Taj fields, and snow shifts now
  distinguish logical bounds from intentional MIPS masked-shift semantics.
  Save-derived object models are checked against the particular object header at
  the final use site.

Validation on Apple arm64: 12/12 CTests in Debug and Release; 7,500-frame ASan
race; 7,500-frame Release race; optimized 240-frame
`array-bounds,pointer-overflow,shift-exponent` UBSan boot; 143.5-second PCM gate;
RAW16 same-binary fixed/legacy A/B; linked wasm/WebGPU build and ROM-absence
guard. Full `-fsanitize=undefined` reaches the separately open signed shift at
`rcp_dkr.c:430`; the limited sanitizer intentionally demonstrates that PORT-01
no longer needs a `tracks.c` suppression.

Follow-up capacity closeout widened all four ordinary `collision_get_y` caller
arrays to 16 elements; the wave builder retains its existing 30. The owning
UBSan/static/forced-overflow gate now measures a minimum 11 slots of live
per-call slack and still proves the legacy arm fails. `segmentsInside[8]`
remains deliberately unchanged: its caller explicitly treats eight or more
overlapping segments as "no waves," so enlarging it and accepting more would
change an original semantic decision rather than improve a capacity.

## FIXED: playable runtime boundaries — wave "runtimebounds"

**Audit IDs:** MEM-06, MEM-08, MEM-09, MEM-10, AUDIO-01, AUDIO-02
**Implementation:** `05cff1f`

Six source-proven P1 defects shared one missing rule: values crossing a lifetime,
branch join, normalization, or serialized-domain boundary were trusted without
establishing their complete contract.

- `level_free()` read its header after freeing it and compared a signed
  `skyDome` with unsigned `0xFF`, making weather teardown heap-dependent and
  leaking sky textures. It now snapshots child ownership, releases weather and
  signed-`-1` sky resources, frees the owner last, and clears both header aliases.
- Plane trick handling and ordinary weapon spawning now initialize their
  cross-branch state (`FALSE` and `NULL`) at declaration.
- The path helper returns success, writes the current object position when no
  checkpoint exists, and both callers use a checked finite X/Z normalizer.
  Zero/tiny/overflow/NaN directions become zero horizontal velocity.
- Audio allocates the five groups live callers address. Getter, setter,
  asset-key lookup, and sound-table lookup all enforce exclusive bounds.
- Vehicle audio validates all ten characters, the complete serialized 30-row
  span, and every vehicle before offset arithmetic. Flying-car/carpet maps
  explicitly to plane sound; loop-de-loop maps explicitly to car sound.

The ROM-free `runtime_contracts` CTest exhausts the declared base and special
vehicle domains, invalid and boundary IDs, exact/truncated/descending asset
spans, sound/group exclusive bounds, and finite/degenerate normalization.
`check_runtime_safety.py` then proves the production call sites retain those
contracts and deletes each required source fragment as a positive control.
Debug, Release, ASan, UBSan, and wasm builds pass. Focused navigation,
Adventure return, 143.5-second PCM, RAW16 A/B, all 47 legal vehicle
combinations, and the array/pointer/shift sanitizer sweep remain green.

## Hand-asm transcription audit — wave "hasmaudit"

A follow-up to the sixth bug shape the "gridmask" wave added: **C bodies that
transcribe hand-written assembly, which only this port compiles.** Upstream's
matching build takes `GLOBAL_ASM(...)` and links the real `.s`, so a transcription
error in that C is invisible to every amount of matching progress upstream will
ever make. `compute_grid_overlap_mask()` was one. This wave asked whether there
were others.

**The ground truth is in the tree**, which is what made this tractable rather than
speculative: `game/src/hasm/ido/math_util.s` (122 KB, 44 `LEAF`s),
`game/src/hasm/collision.s` (3 `glabel`s), `obj_animate.s`, `obj_shade_fast.s`,
`gzip_asm.s`.

### What was audited, and how

Every C function was paired with its assembly counterpart and diffed
instruction by instruction, ranked by the shape of the known defect —
**conditionals and loop bounds first** (`slt`/`slti`/`beq`/`bne` vs the C's
`<`/`>=`/`==`, and for each one *which operand*, and whether it is loop-rolling or
loop-invariant), then array indexing and pointer advance, then signed-vs-unsigned
and `sra`-vs-`srl`, then hot paths over cold.

| file | C bodies | verdict |
|---|---|---|
| `game/src/hasm/math_util.c` | 30 `NON_MATCHING` + 4 plain (of 44 `LEAF`s) | **2 divergences**, 1 fixed |
| `game/src/hasm/collision.c` | 3 | **1 divergence**, fixed |
| `game/src/hasm_native/obj_animate.c` | 2 (+2 helpers) | clean |
| `game/src/hasm_native/obj_shade_fast.c` | 2 | clean |
| `game/src/hasm_native/inflate_native.c` | 5 (+4 helpers) | clean |
| `game/src/objects.c` `func_80017A18` | 1 | **not compiled at all** — see below |
| `platform/math_stubs_temp.c` (the `.s` **data** section + the trig with no C body) | 9 symbols | **3 divergences**, 0 fixed |

Coverage worth stating honestly: `obj_animate`/`obj_shade_fast` were checked at
41/41 conditional branches, 18/18 compares, 11/11 loop trip counts and 93
sign-sensitive loads; `gzip_asm.s` at all 781 lines, 111 branch instructions and
137 of 158 rodata table entries, plus a 301-stream / 529-block zlib differential.
Four `math_util.c` bodies are guarded by `#ifdef NON_EQUIVALENT`, which this build
does **not** define — `mtxf_transform_dir`, `fix32_sqrt`, `bad_int_sqrt`,
`calc_dyn_lighting_for_level_segment` are therefore **not compiled** and were not
audited as live code. (`mtxf_transform_dir`'s dead body does contain a real error —
`*mf[1][0]` indexes the *next matrix in memory* instead of `(*mf)[1][0]` — which
would have to be fixed before that body is ever enabled. Its live implementation
is the weak stub in `platform/math_stubs_temp.c`, which is correct.)

### FIXED 1: `vec3f_rotate_py()` paired each angle with itself instead of pitch with yaw

**Mechanism.** `game/src/hasm/math_util.c`. The upstream `NON_MATCHING` body had
the two angles **transposed** in the x and y results:

```c
vec->x = z * cosY * sinX;      /* WRONG */
vec->y = -z * sinY;            /* WRONG */
vec->z = z * cosX * cosY;      /* right */
```

`LEAF(vec3f_rotate_py)` (`game/src/hasm/ido/math_util.s`, ROM `0x800706D0`) loads
the **pitch** first and the **yaw** second:

```
lh    a0, 0x2(a2)     # pitch
jal   sins_f
mul.s ft1, ft2, fv0   # ft1 = z * sin(pitch)
lh    a0, 0x2(a2)     # pitch
jal   coss_f
neg.s ft1             # y   = -z * sin(pitch)
mul.s ft2, fv0        # z1  =  z * cos(pitch)
lh    a0, 0x0(a2)     # yaw
jal   sins_f
mul.s ft0, ft2, fv0   # x   = z1 * sin(yaw)
lh    a0, 0x0(a2)     # yaw
jal   coss_f
mul.s ft2, fv0        # z   = z1 * cos(yaw)
```

The **offsets** settle it and the `.s` comments do not: `Vec3s`
(`game/include/structs.h:51`) is a union whose rotation view puts `y_rotation` at
`0x0` and `x_rotation` at `0x2`, so the pair loaded first (`0x2`) is the pitch. The
`.s` labels `0x0` "roll", which is what the transposition was copied from.

**A second proof that needs no assembly.** The function is documented as
`vec3f_rotate()` specialised to `(0, 0, z)` with zero roll, and `vec3f_rotate()` —
which does match *its* assembly — yields exactly `(z·cosX·sinY, −z·sinX,
z·cosX·cosY)` for that input. Before the fix the C contradicted its own sibling;
after it, they agree. That invariant is what the regression check asserts, so the
check does not rest on anyone's reading of the disassembly.

**Measured.** Self-test at boot, `pitch = 0`, `yaw = 0x4000` (90°), `z = 100`:

| | x | y | z |
|---|---|---|---|
| ROM formula / fixed | **100.0000** | −0.0000 | −0.0000 |
| transposed (pre-fix) | −0.0000 | **−100.0000** | −0.0000 |

i.e. **a horizontal direction came out vertical.** Case B (`pitch = 0x2000`,
`yaw = 0x1000`): fixed `(27.0598, −70.7107, 65.3281)`, matching
`100·cos45°·sin22.5°` etc. analytically; transposed `(65.3281, −38.2683, 65.3281)`.

**Reachability.** 250 live calls in 1500 boot/attract frames; 201–6761 per race
depending on track. The two arms' output hashes differ, so the transposition was
material to vectors the game actually asked for.

**Why it was silent.** Every call site feeds the result straight into a position or
a velocity, so a wrong direction still moves something plausibly — nothing crashes
and nothing logs. Call sites: particle emission (`particles.c:1163/1165/1214/2393`),
the lens flare (`weather.c:631`), spotlight direction (`lights.c:413`), object
sprite placement (`objects.c:493`).

**Behaviour-neutral for the racer simulation** — 0 of 359 `[PACE]` rows changed on
Ancient Lake, and a full 3-lap Time Trial records the **same course time 4713** in
both arms — because no call site feeds physics. That is *why* the check counts calls
and hashes outputs instead of diffing a `[PACE]` stream: a stream diff could never
see this fix, in either direction.

(Unrelated, noted while measuring: `check_race_finish_time`'s course time is **4713**
on current `main`, not the 4709 recorded in the "finishtime" wave below. The check
asserts a band and cross-checks traced against recorded, so this is informational —
and it is *not* from this wave, since both arms agree.)

Not wrapped in `#ifdef NATIVE_PORT`: it is a transcription correction, not a host
adaptation, exactly as with the grid-mask fix.

### FIXED 2: an untextured terrain batch was skipped instead of collided with

**Mechanism.** `generate_collision_candidates()`, `game/src/hasm/collision.c` —
the same function as the grid-mask defect. For `textureIndex == 255` the upstream
body did `continue`, dropping the batch, so untextured level geometry is not solid.
The ROM zeroes the surface and falls into the triangle loop
(`game/src/hasm/collision.s`):

```
800313D0  addiu $at, $zero, 0xFF
800313D4  or    $t2, $zero, $zero   # surface = SURFACE_DEFAULT
800313D8  beql  $v1, $at, .L8003141C
800313DC   lh   $t0, 0x4($t5)       # (branch-likely delay slot)
800313E0  sll   $v1, $v1, 3         # else &textures[textureIndex]
800313E4  add   $v1, $v1, $a0
800313E8  lb    $t2, 0x7($v1)       #      surface = surfaceType
```

`.L8003141C` (`collision.s:234`) is the **triangle-loop setup**, not the batch-skip
labels `.L80031488`/`.L8003148C` that this loop's four genuine `continue`s branch
to. The decisive evidence needs no reading of the branch target: `or $t2, $zero,
$zero` at `800313D4` would be **dead code** if `0xFF` meant "skip", because every
falling-through path overwrites `$t2` at `800313E8`. It exists only to give the
untextured case `SURFACE_DEFAULT`.

**Reachability: NOT REACHED, and that is stated as a limitation, not a result.**
`untexturedBatches=0` across all 20 main tracks and all 10 boss tracks
(2600 frames each), and again on tracks 5/38/32/15 with a 9400-frame budget
(6759 racer rows each). So the correction is **behaviour-neutral on shipped data**
— which is also the strongest available statement that it broke nothing. It is
landed as a latent-correctness fix with a counter left in place as the tripwire.

**Because it is unreached, comparing the two arms alone would prove nothing**, so
`MDKR_COLLTEX_FORCE=1` treats every batch as untextured. That makes the harm
observable, on Ancient Lake:

| arm | untextured batches | peak candidates | min racer y |
|---|---|---|---|
| default | 0 | 26 | 29 |
| `MDKR_COLLTEX=legacy` | 0 | 26 | 29 (bit-identical, 759 rows) |
| `MDKR_COLLTEX_FORCE=1` | 11334 | 26 | 29 |
| forced **+** legacy | 8938 | **1** | **−568** |

The forced+legacy arm's candidate list collapses to a single entry — the segment
pointer, with no facets behind it — and the racer drops straight through the
world. That is the shape the divergence would take on any level that did contain
an untextured collidable batch.

### Found, measured, and deliberately NOT fixed *(items 1–3 were fixed in wave "closedloop")*

> **Superseded, kept for the mechanism.** Items 1, 2 and 3 below are now the
> DEFAULT — see [wave "closedloop"](gameplay.md#fixed-three-rom-fidelity-divergences-and-the-fixture-class-that-was-blocking-them--wave-closedloop),
> which converted the fixtures first and then flipped them. The env vars have
> been inverted: `MDKR_RNGSEED=legacy`, `MDKR_ARCTAN=trunc` and `MDKR_TRIG=libm`
> now select the superseded behaviour. Item 4 is still open. The deferral
> reasoning below is left exactly as written because it is the record of *why*
> a measured fix waited for its fixtures, which is the transferable part.


1. **The RNG seed is wrong.** `platform/math_stubs_temp.c` has shipped
   `gCurrentRNGSeed = 0x00051234`, `gPrevRNGSeed = 0` since the first platform
   commit, invented to make the link succeed. The `.data` section of
   `game/src/hasm/ido/math_util.s` has **`0x5141564D`** (`'QAVM'`) for *both*.
   These are the live starting seeds: `set_rng_seed()` has exactly one caller in
   the whole game (`game/src/waves.c:364`, `set_rng_seed('WAVF')`, bracketed by
   `save_rng_seed()`/`load_rng_seed()`), so nothing re-seeds at boot and all 98
   `rand_range()` call sites — including `racer.c` (13) and `particles.c` (25) —
   descend from it. The port has been on a different random sequence from frame 0.
   **Silent because the run is still perfectly deterministic, just
   deterministically wrong**, which is precisely why `check_determinism.py` could
   never see it.
   *Deferred, not unproven*: measured **80 of 359 `[PACE]` racer rows changed**
   from row 279. Turning it on shifts every AI racing line, and the
   route-calibrated fixtures do not survive that — `check_race_drive`'s open-loop
   route strands the kart (cp 14 vs the 15 required, lap 0 vs 1), and
   `check_collision_gridmask`'s **positive control stops reproducing** (only 1
   racer loses the ground where it needs 2; the `MDKR_BOSS_SLOW` win arm stops
   reaching `racer_boss_finish()`). Re-cutting those fixtures is a wave of its own
   and must not happen as a side effect of an audit — flipping the default without
   it would trade a silent fidelity bug for a blind test suite.
   `MDKR_RNGSEED=rom` selects the ROM's values, and
   `tests/check_math_tables.py` asserts that arm is exactly right so the fix
   cannot rot while it waits.

2. **`gArcTanTable` is truncated where the ROM rounds.** Same file, same
   deferral. Verified entry by entry against `EXPORT(gArcTanTable)`:
   **truncating mismatches 491 of the 1025 live entries** (all one unit low),
   `+ 0.5f` mismatches **0 of 1025** — rounding reproduces the ROM's table
   exactly. One unit is 1/65536 of a turn, so nothing looks wrong; it just puts
   every `atan2s()`/`arctan2_f()` result up to an LSB off the ROM's, at 72 call
   sites including the AI's steering and the camera. Measured **110 of 359
   `[PACE]` rows changed** from row 233 — same fixture problem as the seed.
   `MDKR_ARCTAN=round` selects it.

3. **`sins_s16`/`coss_s16`/`sins_f`/`coss_f` approximate the ROM instead of
   reproducing it.** The ROM reads `gSineTable` and linearly interpolates on a
   4-bit fraction (`XLEAF(sins_s16)`: `lhu`/`lhu`/`subu`/`mul`/`srl 3`/`sll 1`);
   the port calls `sinf`/`cosf` and scales by 65536. Compared over **all 65536
   angles**: exact on only **21368 (32.6 %)**, mean absolute difference 2.1 units,
   **max 7** units of 65536. Tiny in absolute terms, but systematic and on the
   hottest math in the game (every rotation matrix, racer, camera and audio pan).
   **An exact implementation is available with no ROM data committed**:
   `round(sin(i·π/2/1024)·0x8000)` reproduces the ROM's `gSineTable` **exactly,
   0 of 1025 entries differing**, so the table can be generated at load time
   exactly as `gArcTanTable` is, and the ROM's lerp applied on top. Not done here
   because it lands in the same fixture-recalibration bucket as (1) and (2), and
   because getting hot trig wrong would be far worse than the divergence.
   `tests/check_math_tables.py` pins the `gSineTable` generator so that route
   stays open.

4. **`func_80017A18` is a `return 0` stub — object-model collision never reports a
   hit.** Not a transcription defect; an *unpaired* function, which the audit
   treats as a finding in its own right. `objects.c:5718` guards the body with
   `#ifdef NON_EQUIVALENT`, which this build does not define, so `objects.c.o`
   carries `U _func_80017A18` and the link resolves to the `WEAK` no-op in
   `platform/hasm_stubs_temp.c` (`nm`-verified in both objects). **Measured 8
   calls on boss track 38** over a 9400-frame race (0 on tracks 5/32/15), so the
   gap is live but narrow on the routes measured.
   **Not fixable within this audit, and the reasons matter:** there is *no
   ground-truth assembly for it anywhere in the tree* (`objects.c` cites
   `asm/nonmatchings/objects/func_80017A18.s`, which this repo does not vendor),
   so it cannot be differentially audited at all; upstream itself labels the body
   `NON_EQUIVALENT` (decomp.me/scratch/xNAlf), i.e. known not to reproduce the
   ROM; and enabling it is not a small change — under `NATIVE_PORT` it **does not
   compile** (`objects.c:5768` subscripts `collisionFacets`, a `dkrptr32` token),
   and `objects.c:5748` assigns `collisionPlanes` without `DKR_PTR`, which would
   silently yield a wild pointer once the build error above was fixed. A counter
   is now wired into the stub so the size of the gap can be measured rather than
   guessed. **This was not previously recorded anywhere** — the nearby
   `collision_objectmodel` entry below is about a stack array in the *caller*.

   > **FIXED 2026-07-26 by [wave "objcoll"](collision.md#fixed-object-model-collision-never-reported-a-hit-so-locked-doors-were-intangible--wave-objcoll), and two claims above are
   > retracted.** "No ground-truth assembly anywhere" and "upstream labels the body
   > `NON_EQUIVALENT`" were both **true when written and false when acted on** —
   > upstream matched it in `9da89ecb`. The `dkrptr32` claim was correct, and
   > `DKR_PTR` was the entire fix: two lines. The "8 calls on boss track 38 …
   > narrow on the routes measured" figure also badly understated the gap, because
   > every route measured was a *race track*: a single Timber's Island run makes
   > **948** calls. The lesson for this audit is that a finding recorded against a
   > moving upstream needs a re-check before it is used to justify not acting —
   > the reachability numbers need to come from the routes where the affected
   > objects actually live, not from whichever routes already existed.

5. **`mtxf_to_mtx` and `arctan2_f` truncate where the ROM rounds to nearest.**
   The assembly distinguishes the two float→int conversions deliberately:
   `mtxf_to_mtxs` uses `trunc.w.s` (`math_util.s:363-366`) while `mtxf_to_mtx`
   uses **`cvt.w.s`** (`:639-642`), which follows the FCSR mode — round-to-nearest
   on a booted N64. The C uses `FTOFIX32`, a C cast, i.e. truncation. Same at
   `arctan2_f` (`:2310-2311` `cvt.w.s` vs `(s32)(y * 255.0f)`). ≤1 LSB of 16.16,
   silent, and systematically toward zero. Not changed: the correction is
   `nearbyintf`, and it is not worth perturbing every RSP matrix in the game for
   a fidelity item this small until (1)–(3) are being landed together.

6. **`atan2_lookup` rounds the table index where the ROM truncates**
   (`math_util.c:743` `(s32)(y / x * 1024 + 0.5f)` vs `dsll 11`/`ddivu`/`andi
   0xFFE`). ≤1 index step ≈ 0.044°. Note this leans the *opposite* way from (2),
   so the two partially cancel today; fix them together or not at all.

### Verified faithful — do NOT "fix" these

- **`vec3s_reflect`'s `//!@bug`** (`math_util.c:260`, `vec[1].z` subtracting
  `vec->x`) **is real in the ROM.** `math_util.s:789-797` carries it explicitly,
  gated: the `#else` arm is `sub t5, t5, t0` with `t0` still holding `incident.x`,
  and only the `AVOID_UB` arm subtracts `t2`. The C is an exact transcription of
  the ROM path. Consistency note, not a bug: this build defines `AVOID_UB=1`, so
  the assembly *would* take the fixed branch if it were ever assembled, while the
  compiled C keeps the ROM behaviour — the C body has no `AVOID_UB` gate.
- **`generate_collision_candidates`'s unbounded segment-pointer store** and the
  `counter == 10` cap — already recorded under wave "gridmask"; still faithful.
- **`rand_range`'s 64-bit mixing looks wrong and is not.** `temp = (temp << 32) |
  (temp >> 1)` bears no resemblance to the assembly's 33-bit rotate
  (`dsll32`/`dsrl`/`dsll`/`dsrl32`/`or`), but every bit that differs is above bit
  31 and is discarded by the `sw`. Verified by modelling both semantics exactly:
  **0 divergences over 20008 seeds and two 200000-step chains.**
- **do-while → `for` rewrites** throughout `obj_animate`, `obj_shade_fast`,
  `inflate_native` and `collision.c`: the ROM runs one iteration on a zero count
  (and in two places spins ~2³² times or hangs). The C is the safer side of the
  divergence every time. Do not "correct" toward the ROM.
- **`inflate_native.c` is not a transcription** but an independent puff-style
  DEFLATE decoder. Every semantic divergence found is confined to malformed input,
  and in each case the C is the safer arm. Side effect worth knowing:
  `gzip_huft_build` and the 0x2800-byte `gHuftTable` allocated in `gzip_init()`
  are now dead code.

### Smaller things noticed and left alone

- **`mtxf_scale_y` silently implements the `AVOID_UB` branch, not the ROM's.**
  `math_util.s:1218-1220` has a gated bug — the `#else` arm reads
  `lwc1 ft4, 0x14(a3)`, i.e. garbage from the wrong register ("Should be a0"),
  where the `AVOID_UB` arm reads `a0`. The C body is unconditionally the fixed
  form. Behaviour agrees today because `CMakeLists.txt` defines `AVOID_UB=1`, so
  this is *not* a live divergence — but unlike `vec3s_reflect`, which keeps its
  ROM bug and says so in a `//!@bug` comment, `mtxf_scale_y`'s C says nothing
  about departing from ROM behaviour. Left as is; worth a comment if anyone
  touches it.
- **`coss_2` has no implementation in this port at all.** `math_util.s` defines
  `LEAF(coss_2)` (official name `mathCos`), `math_util.h` does not declare it and
  nothing calls it, so the link succeeds without it. Harmless today; it is simply
  missing, not stubbed, so a future caller gets a link error rather than a wrong
  answer — which is the right failure mode.
- **A malformed doc comment** at `math_util.c:374-379`: line 378 is ` /` where
  `*/` was meant, so the block comment above `mtxf_scale_y` swallows the
  `/* Official name: mathSquashY */` line. Compiles fine, no behavioural effect.
  Not touched, to keep the diff against upstream minimal.

### Regression checks

Three new, all headless + muted, all validated in **both** directions.

| check | asserts | positive control (fix reverted → FAIL) |
|---|---|---|
| `tests/check_math_rotpy.py` | the `vec3f_rotate` invariant, the ROM formula, the A/B wiring, and 250 live calls with differing output hashes | all four assertion families fire |
| `tests/check_collision_untextured.py` | 0 untextured batches + bit-identical arms on shipped data; forced arms 26 cand/y 29 vs **1 cand/y −568** | forced+fixed collapses to 1 candidate and y −568, indistinguishable from legacy |
| `tests/check_math_tables.py` | `MDKR_ARCTAN=round` reproduces `gArcTanTable` **exactly** (FNV `0xe0d93ef8`, 0/1025) and `MDKR_RNGSEED=rom` boots `0x5141564D`; the default arm is still the known divergence | ROM arm perturbed → caught; default flipped → tripwire fires with instructions |

`check_math_tables.py` parses the ground truth out of the vendored `.s` at run
time, so **no ROM-derived bytes are added to the repo.**

## SWEPT: decomp arrays split into two C objects and indexed across the boundary (wave "splitsweep")

The follow-up the "wavetable" wave asked for. That wave fixed **one** instance —
`waves.c`'s `D_8012A5E8[2]` + `D_8012A600[24]` treated as one 26-entry table — and
recorded that *the class was not swept*. This is the sweep: two instruments, the
whole existing fixture set, a triaged finding list, three fixes, and a check that
catches the next one at build time.

### The one number that matters

```
tools/compare_data_layout.py --stats
1001 data pairs have the SAME neighbour on both targets; 432 of them (43%) sit at a
DIFFERENT distance.
```

That is the class in one line: **adjacency on LP64 Mach-O predicts nothing about
wasm32.** wasm-ld gives every data symbol of 16 bytes or more a 16-byte preferred
alignment, so it leaves holes Mach-O does not; Mach-O 8-byte-aligns small symbols,
so it leaves holes wasm-ld does not; and neither reproduces the decomp's source
order (`gTrackWaves` and `D_8011D128` come out *reversed* on wasm32). Any decomp
idiom that indexes past the end of one global and expects to land on the next is a
coin flip, per pair, per link. Do not reason about it — measure it, then make it a
language guarantee.

### Instruments (both, on the full fixture set)

**1. UBSan `-fsanitize=array-bounds,pointer-overflow`, 99 runs.** This is the
instrument that had already seen the wave defect and been ignored. Built with
`-DMDKR_EXTRA_C_FLAGS=...` (a new hook, because `-Wno-everything` in CMakeLists.txt
lands *after* `CMAKE_C_FLAGS` and would otherwise swallow the flag), reports
collected via `UBSAN_OPTIONS=log_path` so the existing checks' stdout parsing is
untouched. Coverage: the 9 `nav_*` menu fixtures, a 6000-frame attract soak, boss
tracks 38 and 39–45 (`MDKR_BOSS_SLOW=1`), `check_race_drive`,
`check_race_2p_split`, `check_collision_gridmask`, `check_texture_lineswap`,
`check_adventure_hub`, `check_adventure_race_loop`, `check_race_finish_time`,
`check_track_sweep` (20 tracks) and `check_vehicle_sweep` (47 combos). Every run
exited 0; the reports are pure UB, no crashes.

**2. Layout comparison, LP64 Mach-O vs wasm32,** now committed as
`tools/compare_data_layout.py`. Native addresses come from `nm -n build/mdkr64`.
wasm32 addresses come from a `wasm-ld --Map`, which emcc does not emit by default —
the tool re-links `build-web`'s existing objects using the link command read
straight out of `build-web/CMakeFiles/mdkr64_web.dir/link.txt`, and then **verifies
the relinked wasm is byte-identical to the shipped one** and refuses to print
anything if it is not. (The web build is bit-reproducible; this is one more thing
that depends on keeping it that way.)

A third instrument was tried and is worth recording as a **dead end**: a static
`-Warray-bounds -Warray-bounds-pointer-arithmetic` compile of the whole tree
produces **zero** warnings. Every instance of this class indexes with a *variable*,
so the static warning cannot see any of it. ASan is also blind to the global cases
— both arrays in the wave defect were `__DATA,__common`, which ASan does not
redzone — and blind to `get_inside_segment_count_xz`-style overruns, which write
through a bare pointer rather than an indexed array.

### The deduplicated finding list — 9 sites, all of them

Ranked as instructed: reached on wasm32 > reachable in principle > latent.

| # | site | report | runs | verdict |
|---|---|---|---|---|
| 1 | `menu.c` `gTrackSelectIDs[4][6]` row 4 | `index 4 out of bounds for 's16[4][6]'` | 88/99 | **REACHED, load-bearing, behaviour was wrong on BOTH targets — FIXED** |
| 2 | `camera.c` `gScreenViewports[4]` idx 4 (×2 sites) | `index 4 out of bounds for 'ScreenViewport[4]'` | 99/99 | **REACHED, benign by arithmetic accident — FIXED** |
| 3 | `menu.c` `gCharacterVolumes[0][19]` (×4 sites) | `index 19 out of bounds for 'u8[2]'` | 93/99 | in-bounds of the whole object — allow-listed |
| 4 | `racer.c` `D_800DCDB0[0][0..31]` | `index N out of bounds for 's8[2]'` | 55/99 | in-bounds of the whole object — allow-listed |
| 5 | `tracks.c:2994` / `:3001` | `pointer-overflow` / null-pointer offset | 99/99 | a *different*, already-documented class — allow-listed |

Sites 3 and 4 are the **flattened 2-D index**: `u8 gCharacterVolumes[10][2]` walked
as one 20-byte run through `[0][n*2+1]` (max flat index 19, the last element), and
`s8 D_800DCDB0[16][2]` (32 bytes) indexed `[0][miscAnimCounter & 0x1F]`, i.e. 0..31
— exactly the whole object. Both are ISO-C UB and both are memory-safe,
layout-independent and identical to what the N64 does. Not the class; not touched.

Site 5 is `j = (s32) align16(...)` in `track_load_level()`: the decomp's `s32`
scratch holding an arena address. Correct on the N64 and on wasm32 (32-bit
pointers); on LP64 the round-trip trips `pointer-overflow`. That is the
LP64-pointer-truncation class, tracked in its own section.

**What the sweep did NOT report is evidence too.** Every one of the following is a
*directly indexed* constant-bound array, so `-fsanitize=array-bounds` instruments it
and 99 clean runs mean the index stayed in range. Peak values were then measured
directly with a throwaway probe over 9 tracks (every wave track + boss 38), the
attract demo, 2P split-screen, the Adventure hub loop and the menu graph:

| carried-in candidate | bound | measured peak | verdict |
|---|---|---|---|
| `objects.c` `collision_objectmodel` `spB4[10]`/`sp8C[10]` | 10 | **1** | latent, 9 slots spare — the probe firing also proves the function runs |
| `tracks.c` `D_8011D128[20]` / `gTrackWaves[20]` write at `[20]` | 20 | **8** | latent, 12 slots spare |
| `game.c` `gLevelPropertyStack[20]` pushed unbounded | 20 | **4** (one 4-word frame) | latent |
| `camera.c` `gCameraRelPosStackZ[5]` write at index 5 | 5 | **1** | latent — contained anyway, see below |
| `camera.c` `gModelMatrixF[5]` dereferenced while permanently NULL | — | pos peak **1**, needs 4 | latent and **faithful**: `D_80120DA0` only has storage for 5 matrices, so `gModelMatrixF[5]` is NULL on the N64 too and the real game cannot reach it either. Left alone. |
| `objects.c:5288` `func_80016748` `f32[4][3]`-for-`MtxF` | — | n/a | already safe: `AVOID_UB=1` selects the `MtxF` branch |
| `hasm/collision.c:126` `generate_collision_candidates` | 500 | 416 (previous wave) | faithful to the ROM, owned by the hasm audit, untouched |

### Fix 1 (the real one): `gTrackSelectIDs[4][6]` is a 5×6 grid, and `gFFLUnlocked` IS `[4][0]`

`trackmenu_init()` fills the track-select grid with
`for (i = 0; i < 5; i++) for (j = 0; j < 6; j++) gTrackSelectIDs[i][j] = ...` — five
rows, and the body even special-cases the fifth (`if (sp74 == 4 && i != 4)`). The
array is declared `[4][6]`. Row 4 is therefore written **12 bytes past the end**.

On the N64 that is exactly where `gFFLUnlocked` and the two following "UNUSED" s32
live, and **the game depends on it: `gFFLUnlocked` has no other writer anywhere.**
The decomp's own address-derived symbol names prove the adjacency —
`gTrackSelectIDs` is `0x801268E8`, so `&gTrackSelectIDs[4][0] == 0x80126918`, and
the next two symbols are literally named `D_8012691C` and `D_80126920`. The
"UNUSED" markers on those two are wrong; they are `gTrackSelectIDs[4][2..5]`.

Our linkers put `gFFLUnlocked` nowhere near it:

| target | `&gTrackSelectIDs` | `&gFFLUnlocked` | delta (must be 48) |
|---|---|---|---|
| native | `0x1009b3070` | `0x1009b2844` | **−2092** |
| wasm32 | `0x0002d950` | `0x0002d99c` | **+76** |

So `gFFLUnlocked` was never written, and row 4's 12 bytes landed on
`gTrackSelectRenderDetails[0]` natively / `gTrackTTSoundMask` + `D_80126848` on
wasm32. Measured with a probe on the `nav_to_track_select` route, fresh EEPROM:

```
before:  delta=-2092  gFFLUnlocked=0   maxTrackY=4
after:   delta=48     gFFLUnlocked=-1  maxTrackY=3      <- the ROM's behaviour
```

**This was a live, visible gameplay defect on both native and the browser build.**
`spaceWorldUnlocked = gFFLUnlocked == -1 ? 3 : 4` came out **4 on a fresh save**, so
`menu.c:9198`'s `gTrackSelectCursorY < spaceWorldUnlocked` let the cursor scroll
down onto the **Future Fun Land row before it is unlocked**, `maxTrackY` came out 4
so that row was *drawn*, and `gTrackIdForPreview = gTrackSelectIDs[4][x]` then read
an unrelated object as a level id — natively `gTrackSelectRenderDetails[0]`, which
the renderer rewrites every frame, so pressing A there loads an arbitrary level.

Fix: back both names onto one 5×6 object (`gTrackSelectIDsTable`), `NATIVE_PORT`,
two `_Static_assert`s locking the offset of `fflUnlocked` and the total size.
Verified as one 60-byte object on both targets (`tools/compare_data_layout.py`).
Both symbols are menu.c-local. Every existing check still passes, including all
nine `nav_*` fixtures and `check_race_finish_time` — i.e. nothing was navigating
through the phantom fifth row.

### Fix 2: `gScreenViewports[4]` — the most-reported UB in the codebase

`viewport_reset()` sets `gActiveCameraID = 4` by hand and then reads
`gScreenViewports[gActiveCameraID].flags`; `viewport_rsp_set()` does the same. The
array is `[4]`. **99 of 99 runs report it.** What it reads is `gViewportStack[2]` on
all three targets (N64 `0x800DD068` + 256, native `0x1008ec048` + 256, wasm32
`0x00019840` + 256 — all land on `gViewportStack + 0x20`), and
`VIEWPORT_EXTRA_BG == 0x0001` while `viewport_rsp_set()` only ever stores multiples
of 4 into `vscale`, so bit 0 is always 0 and the branch is always the right one.
Benign by arithmetic accident, not by design, and one relink from flipping the
full-screen camera onto the extra-background path.

Fix: a fifth `{ DEFAULT_VIEWPORT }` element under `NATIVE_PORT`. Provably
behaviour-identical — every write path (`camEnableUserView` / `camDisableUserView` /
`viewport_menu_set`, all called with index 0, plus `copy_viewports_to_stack()`,
which loops `i < 4`) touches only 0..3, so element 4 keeps its initialiser forever
and its `flags` is 0, exactly what the OOB read returned.

### Fix 3 (containment, provably a no-op): `gCameraRelPosStackZ`

`mtx_cam_push()` writes X, Y and Z at the same index and only *then* warns
`if (gCameraMatrixPos > CAMERA_MODEL_STACK_SIZE)`, so index 5 is legal by the code's
own contract. X and Y are `[+1]`; Z is not. A Z write at index 5 lands on
`gCameraTransform` natively, on **`gTransitionsDisabled`** on wasm32 (an unrelated
subsystem's flag), and on `perspNorm` on the N64. Measured peak
`gCameraMatrixPos` = **1**, and UBSan reported nothing here in 99 runs, so this is
unreachable today and sizing Z like X and Y is a no-op. It is in because the
alternative, if some future track does push a sixth matrix, is a silent 4-byte
write into another subsystem — the exact shape of the second browser crash.

### The detector: `tests/check_array_bounds_sweep.py`

The generalisation of `check_wave_visible_table.py`, and deliberately **not** a
table of symbol pairs. Reasons: (a) after the fixes there is no load-bearing
adjacency left that is not already a `_Static_assert`, so a pair table would assert
things the compiler already guarantees; (b) a pair table only knows about pairs
somebody already thought of, and the whole problem is the ones nobody has. What
generalises is the *cause*: an index leaving its array.

So the check builds the UBSan binary, drives 8 representative routes (~1.5 min),
and fails on any out-of-bounds index that is not in a committed allow-list, each
entry carrying the reason it is not a finding. Entries are keyed on
**(file, type, source snippet)** rather than line number, so moving code does not
re-baseline the list but *changing* the offending line does — that is exactly when a
human should look. (Demonstrated in passing: the allow-list kept matching when the
reported lines moved 7378 → 7383 and 2994 → 3001.)

Fails closed, four ways: the binary must actually import
`__ubsan_handle_out_of_bounds` (proving the sanitizer flag survived CMake's flag
ordering); every route must exit 0; the report set must be non-empty; and
`REQUIRED_SENTINELS` — allow-listed idioms on paths every route crosses — must all
still be reported, so "clean" can never be confused with "not measuring".

**Validated in both directions, on real builds:**

| tree | result |
|---|---|
| fixes applied | 4 sites, all allow-listed → **PASS** |
| `gTrackSelectIDsTable` union reverted | + `menu.c:8502 index 4 out of bounds for 's16[4][6]'` → **FAIL** |
| `gScreenViewports[5]` reverted | + `camera.c:1097 index 4 out of bounds for 'ScreenViewport[4]'` (and menu.c's, also reverted at that point) → **FAIL** |

### Left alone on purpose

- Everything in the "did NOT report" table above. Each is faithful to the ROM,
  measured unreachable with real slack, and patching on speculation adds risk
  without removing a defect. `check_array_bounds_sweep.py` now fires the day any of
  them becomes reachable, which is a better guarantee than a guessed fix.
- The two flattened-2-D idioms (`gCharacterVolumes`, `D_800DCDB0`). Both stay inside
  one object on every target. "Fixing" them would change decomp text for no
  behavioural gain.
- `tracks.c`'s `s32`-as-pointer sites — a different class with its own section.
- `hasm/collision.c` and `hasm/math_util.c` — owned by the hand-assembly audit.
- `waves_block_hq()`'s unguarded `gWaveModel[indexNum]` and `free_track()` not
  zeroing `gWaveBlockCount`: still 0 occurrences, still latent, still recorded in
  the "wavetable" section.
- `get_inside_segment_count_xz()` writes one `s32` per matching segment through a
  bare pointer with **no bound**, and `get_level_segment_waves()` checks
  `segmentCount >= 8` only *after* the call, so 8+ overlapping segments overflow
  `segmentsInside[8]` on the stack. **Neither instrument can see this one** —
  array-bounds needs an indexed array, and ASan does not cover it either since the
  overrun is through the callee's pointer. `inSegs[28]` at the other call site has
  much more slack. Recorded as the known blind spot of this sweep; the next
  instrument for it is a bound parameter, not a sanitizer.

### Files touched

`game/src/menu.c`, `game/src/camera.c` (fixes), `CMakeLists.txt` (the
`MDKR_EXTRA_C_FLAGS` sweep hook), `tests/check_array_bounds_sweep.py` and
`tools/compare_data_layout.py` (new).

## LP64 pointer-truncation crash class — SYSTEMATIC SWEEP (robust: wave)
The whole `(u32)/(s32)`-cast-of-a-pointer class was enumerated and classified,
not just the sites a single boot path happened to hit. Method: the build already
keeps `-Wint-to-pointer-cast -Wpointer-to-int-cast` VISIBLE (they survive
`-Wno-everything`), so every truncation is a compiler warning. Clean build = 211
unique cast sites across 36 files; each was read and put in one of three buckets:

  1. **BUG — truncate-then-dereference** (the crash class). `(Type*)((u32)hostptr +
     off)` or `(T*)(s32) realptr` whose result is dereferenced in HOST memory
     without going through the arena reconstructor. On LP64 `(u32)` truncates and
     `(s32)` sign-extends the 64-bit arena pointer -> wild -> segfault (ASLR-dep).
  2. **TOKEN — truncate-to-token-then-reconstruct** (CORRECT, left alone). `(u32)
     hostptr` fed to `asset_load`/`dmacopy`/`osPiStartDma` (dest reconstructed via
     `dkr_lo32_to_ptr` inside `osPiStartDma`), `OS_K0_TO_PHYSICAL`/`gDkrDmaDisplayList`/
     `rsp_segment` (DL word resolved by `dkr_resolve`), or a `gTextureCache/
     gSpriteCache/gModelCache/minimapSpriteIndex/tex->cmd` slot read back via
     `DKR_PTR`. `(uintptr_t)` here would BREAK the DL/DMA path.
  3. **VALUE — truncate-for-value/compare** (fine). pointer-difference sizes,
     low-nibble alignment extraction, `(s32)a==(s32)b` identity within one arena,
     printf, packed offset+flag words.

FIXED (all case-1, with `(uintptr_t)` — pointer-width, so the warning also
disappears, proving the swept sites are no longer truncating):
  - objects.c racerfx_alloc (gBoostTris/gBoostVerts) — boost geometry, every race.
  - objects.c finish-challenge `((Camera*)camera)->mode` (widened the `s32 camera`
    local to `uintptr_t`) — race finish.
  - objects.c objFreeAssets / ObjSetupObject cleanup — `tex_free`/`free_3d_model`
    of `(s32)`-truncated D_8011AE50/54, obj->modelInstances[i], obj->textures[i]
    (real pointers) on object teardown / spawn-failure.
  - objects.c + racer.c `sndp_stop((SoundHandle)(s32) racer->unkXX)` (10 in racer.c,
    5 in objects.c) + object_functions.c door/ttDoor->soundMask — SoundHandle/
    AudioPoint* are real runtime pointers; sndp_stop DEREFERENCES the handle.
    (Latent while audio was still silence-stubbed, but a live crash the moment
    a non-NULL handle exists — fixed now so M5 audio can't reintroduce it.)
  - lights.c lights_init (gShadeBuffer/gLightDirs) — per-frame lighting buffers.
  - memory.c pool init `_ALIGN16(slots)` (the macro truncates to u32; only the
    POINTER use at pool setup was a bug — the size uses are fine).
  - video.h FBALIGN — framebuffer/depthbuffer alignment (also fixes a latent
    mempool_free-of-truncated-pointer on video-mode change; the segment token
    `(s32)fb` is bit-identical before/after so rendering is unaffected).
  - textures_sprites.c material_set_blinking_lights (Spaceport Alpha) advance.
  - menu.c bootscreen_init_cpak gBootPakData[] (controller-pak / file-select).
  - menu.c cheatmenu_checksum `&__ROM_END`: native __ROM_END is a pointer VARIABLE
    so `&__ROM_END` is garbage, not the ROM extent -> unbounded ROM-buffer OOB
    reads. Bounded to the real us.v80 __ROM_END (0x00B8CFD0). Only reachable via
    the ROM-checksum cheat, not normal MAGIC-CODES navigation.

DEAD-CODE case-1 left as-is (no caller — cannot crash; documented so they are not
mistaken for live bugs): objects.c func_8000E5EC/E79C/E558 (object-map compaction),
tracks.c trackMakeAbsolute, memory.c align8/align4 (UNUSED), main.c mainproc bzero
(native boot bypasses mainproc). object_models.c D_8011D644 is write-only.

ASan spot-check (build-asan, game_select / file_select / magic_codes / race_drive)
surfaced TWO pre-existing stack-buffer bugs in code this sweep did NOT touch —
they are a DIFFERENT class (fixed-size stack arrays, pointer-width-independent),
do NOT segfault in the normal build (0/20 both before and after — the overrun lands
in adjacent unused stack), and are OUT OF SCOPE for the pointer-truncation sweep.
Tracked here so they are not lost:
  - [x] menu.c fileselect_render: `char trimmedFilename[4]` overflows by 1 (a
    filename_trim writes output[len]='\0' with len up to 4). Benign off-by-one in
    the decomp, independent of LP64. **FIXED:** the native buffer is 32 bytes with
    a compile-time save-name bound; filename-entry repeats under ASan.
  - [x] camera.c render_sprite_billboard (via weather.c lensflare_render) reads 2
    bytes past a stack sprite buffer — the SAME game-side billboard/sprite
    vertex-data issue already tracked ("BILLBOARD SPRITES stretch into thin bars").
    **FIXED:** callers now pass `ObjectTransform` and animation frame explicitly
    instead of casting smaller HUD/particle records to `Object`; the native-layout
    gate retains the old cast as its required ASan-failing control.
No ASan finding was an int-to-pointer / heap-wild-pointer error, so the
pointer-truncation fixes themselves are ASan-clean for their class.

-Werror decision: NOT flipped on. The remaining ~200 warnings are all case-2 token
truncations (`(u32)` is their PURPOSE) and case-3 value math; a full
`-Werror=int/pointer-cast` would require wrapping every one in a `DKR_TOKEN()`
macro — a ~150-site edit across the working DL/asset path, exactly the regression
risk this wave must avoid. The warnings stay VISIBLE (so a NEW case-1 truncation
shows up), and the live-path case-1 count is zero. Tracked as future work.

## From asset-swap workstream (M2-swap, commit b1600b2)
- [x] Multi-frame texture walking is asset-bounded and rejects a zero/invalid
  stride; only single-frame write-size-false assets may encode zero, where the
  decoded payload boundary owns the extent. The sprite/texture ROM census and
  moving-mip gate exercise the production layout.
- [x] Level-model BSP-tree node array — SWAPPED in M4 (see "M4 render state" above).
- [x] `LevelModelSegment.unk8` is serialized and normalized as a 32-bit value,
  but current production source never reads it. Its exact original meaning
  therefore cannot affect the shipped port; future use must classify it before
  assigning semantics.
- [x] `ObjHeaderParticleEntry` is explicitly two `s32` words, size 8; the asset
  normalizer swaps both words per entry and the object-layout contract locks the
  owning header offsets.
- [x] `ASSET_AUDIO` byteswap — CLOSED (M5). Rather than an in-place header swap
  (which the overlay-and-patch alBnkfNew/alSeqFileNew can't do safely on LP64,
  since ALBankFile/ALBank/ALInstrument/ALSound/ALWaveTable embed 4-byte on-disk
  pointer slots that are 8 bytes here), the bank/seqfile parsers were rewritten
  (then the decompiled `bnkf.c` under `NATIVE_PORT`, now the bank parser in
  `platform/audio_compat.c`) to read the raw big-endian image
  through explicit byte offsets and build fresh, host-laid-out, arena-resident
  structs (the mgb64 audio_compat.c model). Raw ADPCM/VADPCM sample data stays in
  ROM, byte-order-defined (fetched by the audiomgr DMA callback). Since wave
  "raw16", the three uncompressed PCM loads convert big-endian samples to host
  representation through `aLoadRaw16Buffer`; the ADPCM byte-stream load remains
  untouched. Also swapped at load: the
  compressed-MIDI ALCMidiHdr (trackOffset[16]+division, in music_sequence_init)
  and SoundData.soundBite/.range u16 (audio_init).
- [x] `ASSET_EMPTY_14` remains byte-order-defined intentionally: TLUT entries are
  loaded as raw bytes and the HLE reconstructs each RGBA16/IA16 entry MSB-first.
  CI palette bounds and transactional loads are enforced before commit.

## [RESOLVED in M3] LP64 asset-struct layout — the blocker below is fixed
architecture decision 8 implemented: every ROM-overlaid struct's embedded pointer fields
are now 4-byte `dkrptr32` token slots under NATIVE_PORT (game/include/dkr_native_ptr.h),
preserving on-disk layout; derefs go through DKR_PTR(), patch sites store DKR_TOK()
tokens, and _Static_assert offset/size locks guard each struct. Converted:
LevelHeader, TextureHeader, TextureInfo, LevelModel, LevelModelSegment,
ObjectModel, ObjectHeader. The post-inflate/whole-record asset swaps (the
"Integration pending" item below) are now wired, and the ASSET_MISC LevelHeader_70
sub-asset swap (asset_swap_misc_lightdata) is called at the func_8007F1E8 sites.
`--headless-frames 300` exits 0. The original blocker text is kept below for
history.

## M3 BLOCKER — LP64 asset-struct layout (found in M2, the "first real frame" gate)
DKR overlays its C structs directly on ROM asset bytes. Several of those structs
embed real C pointers, which are 8 bytes on the 64-bit host but 4 bytes on N64.
That inflates the struct and shifts EVERY field after the first pointer off the
on-disk N64 offset that asset_swap normalizes, so any read past that point is
garbage. asset_swap can't fix this — it's a struct-layout problem, not endianness.
Confirmed offenders (there are more):
- `LevelHeader` (structs.h): `s8 *AILevelTable` @0x20 (used via &field), the
  `LevelHeader_70 *unk70[1]` union @0x70, and `LevelHeader_70 *unk74[7]` @0x74.
  First wall the natural boot hits: level_load() reads `unk74[i]` at the wrong
  host offset → get_misc_asset(garbage) → func_8007F1E8 OOB (textures_sprites.c
  ~1694).
- `TextureHeader` (structs.h): `Gfx *cmd` @0x0C shifts numOfTextures/isCompressed/
  textureSize/uncompressedSize — breaks load_texture()'s compressed path (garbage
  uncompressedSize → out-of-arena DMA). Blocks ALL texture/font/sprite loading,
  so the menu can't render either.
- Same class expected in `LevelModel`/`LevelModelSegment`, `ObjectHeader`,
  `ObjectModel` (offset-table + pointer fields).
Fix options for M3: lay these structs out to match disk on LP64 (make the
embedded pointers 4-byte on-disk slots + hold the runtime pointer as a token
reconstructed via dkr_lo32_to_ptr, à la TextureHeader.cmd), OR read the on-disk
records through explicit byte offsets instead of struct overlay.

## Asset-swap gaps found while iterating M2
- ASSET_MISC sub-asset `LevelHeader_70` (Spaceport Alpha pulsating lights),
  reached via get_misc_asset(LevelHeader.unk74[i]). ASSET_MISC is punted
  (heterogeneous), so this record is still BE. Layout: 0x00 s32 unk0 (entry
  count), 0x04/0x08/0x0C s32 unk4/unk8/unkC, 0x10/0x14 ColourRGBA (u8x4, no
  swap), then unk0 × { 0x00 s32 unk0 (swap); u8 r,g,b,a } at 0x18. The type is
  only knowable at the func_8007F1E8 call site. (Currently unreachable — gated
  behind the LevelHeader LP64 wall above.)
