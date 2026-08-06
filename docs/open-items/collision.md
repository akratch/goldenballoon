# Open items — Collision

> One subsystem of the split [open-items index](README.md), which states how these files are kept.

**Currently open** (per [`README.md`](README.md#still-open)'s open/closed
table): 2 items.

| Item | Where |
|---|---|
| Boss levels 41/54 collision-candidate headroom: cap deliberately NOT raised; the 84-slot margin is measured/gated but still worth watching | [§ G4 follow-up: per-level headroom measured and gated, cap NOT raised](#g4-follow-up-per-level-headroom-measured-and-gated-cap-not-raised) |
| "Lose animation before the race" / "1st place recorded as a loss" report on the first boss — symptom 1 ruled out by construction, symptom 2 not reproduced. Sometimes conflated with gameplay.md's wave "bossverdict", which explains a different, later report (no cutscene at all) and remains an unconfirmed hypothesis for this one; `check_boss_win_verdict.py` does not assert against symptom 2 | [§ Report 3 — the lose animation ... NOT REPRODUCED](#report-3--the-lose-animation-played-before-the-race-and-1st-place-recorded-as-a-loss-not-reproduced) |


## FIXED: object-model collision never reported a hit, so locked doors were intangible — wave "objcoll"

**Report (playtest log, in ordinary play):** *"I can pass through doors for areas that I
do NOT have enough balloons to reach."*

### Mechanism

`func_80017A18()` is the per-facet object-model collision test called from
`collision_objectmodel()` (`objects.c`). It is the **only** thing that makes a
collision-meshed object solid, and — this is the part nobody had connected — the
**only non-NULL writer of `collisionData->collidedObj` anywhere in the game**.

`objects.c` guarded its body with `#ifdef NON_EQUIVALENT`, which this build does
not define, so `objects.c.o` carried `U _func_80017A18` and the link resolved to a
`return 0` `WEAK` stub in `platform/hasm_stubs_temp.c`. Every collision-meshed
object was therefore intangible.

The door is not what let the player through, and neither is `obj_loop_door()`,
whose gate logic is correct — the shut door simply had no collision. Once past the
leaf, `obj_loop_exit()` latches normally: by design it tests only a sphere
(`radius` 253 for the hub → Dino Domain exit) and a half-plane through the exit's
own origin, and **has no door check at all**. That is faithful to the ROM and must
not be "fixed" by gating the exit on the door.

### Measured evidence

Before the fix, clean EEPROM, **0 balloons**:

| observation | value |
|---|---|
| `MDKR_OBJDUMP` door states, Timber's Island | all 8 doors `open=0` — the gate logic was right |
| Dino Domain lobby (levelId 12) entered | frame **6589** |
| Ancient Lake (levelId 5) entered | frame **6890** |
| stub calls in one hub run | **948** (previously recorded as "8 on boss track 38, 0 on tracks 5/32/15 — live but narrow"; it is two orders of magnitude wider in hubs, which is exactly where the progression gates are) |
| `lldb` at the call site, call #300 | `behaviorId=14` (`BHV_DOOR`), `doorID=0`, `balloonCount=1` — the 1-balloon Dino Domain door itself |

Ruled out first, each by measurement, so nobody re-treads them: the door gate
logic; a flag-namespace collision (doors use IDs 0,1,4,5,7,8,9,13, balloons
2,3,6,10,11,12,14, and the level has **zero** `BHV_TRIGGER`/`BONUS`/`MODECHANGE`
objects — disjoint, though the namespace is 15/16 full and a cross-hub collision
check is still worth writing); save/runtime index agreement (`courseId` ==
`worldId` == 0 and `level_world_id(0)` == 0, with `level_world_id` =
`[0,12,14,24,2,35]`); the 20-slot `gCollisionObjects` cap (measured **11** in the
hub, so no door is dropped); `collision_objectmodel`'s `spB4[10]` (peak 1 of 10);
and `Object_Door` LP64 layout (`get_object_property_size` uses host `sizeof`).

### The blast radius is wider than doors

`objects.c`'s `collisionData->collidedObj = obj` is gated on this function's
return, so while it was stubbed `collidedObj` was permanently NULL and **five
interaction sites were dead**:

| site | what was dead |
|---|---|
| `object_functions.c:3610` | the door's *"you need N balloons"* jingle + textbox — **it had never once fired** |
| `:3836` | T.T. door textbox |
| `:878` | lighthouse / rocket signpost (has a `Z_TRIG` fallback, so partly reachable) |
| `:638`, `:666` | trophy cabinet |
| `:5708`, `:5710` | racer interaction |

### Fix

Upstream **matched** the body: decomp commit `9da89ecb "match: func_80017A18"`
(branch `match-func_80017A18`), which removes both the `#ifdef NON_EQUIVALENT`
guard and the `#pragma GLOBAL_ASM` fallback, and drops the `// NON EQUIVALENT`
comment from `objects.h`.

> **Update 2026-08-07.** That branch has since merged into upstream `master` as
> `db7482cc` (PR #749), so the "we carry a commit that is not on `master`" hazard
> is retired. The 2026-08-07 sync replaced our copy with master's text — which
> renames the parameters (`numCollisions`, `originPointsX`, … `scale`) and spells
> the facet walk as `for (j = 0; j < collisionFacetCount; j++)` rather than the
> branch's `do`/`while` — and re-applied the port deltas below.

Our vendored copy of the function was **byte-identical to upstream baseline
`3b2dd520`** — verified by extracting both function bodies and diffing — i.e. it
carried no port patches, which makes the 3-way merge resolution unambiguous:
adopt the matched body wholesale.

Two port adaptations, both no-ops on the N64 build (`dkr_native_ptr.h`), because
`collisionPlanes`/`collisionFacets` are `dkrptr32` tokens here:

```c
planes = DKR_PTR(f32, arg0->collisionPlanes);
node   = &DKR_PTR(CollisionFacetPlanes, arg0->collisionFacets)[j];
```

This is the same convention `object_models.c` already uses at every site that
*builds* this data — and that is also why **no byte-swap was needed**, the worry
that had made this look expensive: `collisionPlanes`/`collisionFacets` are
generated at runtime from vertex cross-products, and `func_80060910()`, which
computes the real edge-bisector plane indices, was already fully implemented in
the port. Nothing here is read raw from ROM.

### Three claims this wave retracts

The stub's own comment and the `hasmaudit` write-up asserted three blockers. All
three were false by the time anyone acted on them, and the shared cause is that
they were recorded once and never re-checked against a moving upstream:

1. *"Upstream labels the body `NON_EQUIVALENT` … there is no ground truth."*
   **Superseded** — it is matched.
2. *"It does not compile under `NATIVE_PORT` because `collisionFacets` is a
   `dkrptr32` token."* **True, and `DKR_PTR` is the entire fix** — two lines.
3. *"`collisionPlanes` assigned without `DKR_PTR` would silently yield a wild
   pointer."* Same two lines.

### Verification

`tests/check_door_blocks.py`. The failure mode here is **silence** — you simply
drive through, nothing crashes — so a check that only ran the fixed build would
prove nothing (CONTRIBUTING.md rule 2). Both arms therefore run from **one
binary** via `MDKR_OBJCOLL=legacy`, which restores the stub's `return 0`, exactly
as `MDKR_COLLTEX=legacy` does for the untextured-batch case:

```
$ MDKR_AUDIO=0 python3 tests/check_door_blocks.py --build build -v
PASS: door blocking check
  fixed  arm: reached [0, 21, 22, 23, 36, 39], 1742 object-collision hits -- locked doors held
  legacy arm: reached [0, 5, 12, 21, 22, 23, 36, 39], 0 hits -- drove through the shut door, as before the fix
```

The fixed arm asserts a *negative* (cannot get in), which is only meaningful next
to the legacy arm's *positive* (could get in — same route, same binary). It also
asserts the hub **is** reached and that hits are non-zero, so "blocked" cannot be
satisfied by a route that broke early or never touched a door. The route includes
a center-line waypoint before the paired hub door leaves; without it, the older
diagonal advloop route can latch the exit from the near-side corner without ever
touching the physical door mesh.

`[OBJCOLL]` telemetry is kept but repurposed: it now counts **hits** rather than
stubbed misses, so it stays a live measure of how much object collision a route
exercises.

### Regression this wave CAUSES, fully attributed and NOT papered over

**`check_collision_gridmask.py` and `check_boss_win_verdict.py` both FAIL after
this change.** Both pass with `MDKR_OBJCOLL=legacy` and fail without it, so the
attribution is not in doubt — it is this change. They are left failing rather than
re-baselined, because widening their frame budget would hide the behaviour instead
of explaining it.

What actually happens on boss track 38, measured:

| | legacy arm (collision stubbed) | fixed arm |
|---|---|---|
| collision hits | 0 | **9**, all at frames **4598–4606** and nowhere else |
| first trajectory divergence | — | frame **4600**, **2.5 world units** |
| peak Y | 4868 | 4755 |
| outcome at the summit | `fin=1` at frame **8818** | falls, `level_transition_begin(1)` back to the hub at ~**8839** |
| `fin=1` within 20 000 frames | yes | **never** |

The nine hits are one ~9-frame brush against a single `BHV_HIT_TESTER_2`
(`behaviorId` 72, one of the `unk11 == 2` collision-meshed solids) partway up the
volcano. **Nothing collides at the summit** — the fall is 4 150 frames downstream
of the last hit. So this is chaotic amplification of a small, legitimate
collision, not a collision that pushes the racer off.

Two things follow, and they matter for how this is judged:

1. **The collision itself looks correct.** One object, nine consecutive frames, a
   2.5-unit displacement, clean separation. A wrong `DKR_PTR` would give wild
   planes and either garbage displacement or a crash; instead the hub blocks
   doors exactly as designed and `check_determinism` still passes.
2. **The boss fixture had no margin.** The legacy arm finished with 582 frames of
   slack on a single deterministic trajectory, and `MDKR_RNGSEED=legacy` changes
   *nothing* about it (byte-identical peak Y and hit count in a 2×2), so the route
   is a knife-edge with no stochastic spread to absorb a 2.5-unit nudge.

**Whether the ROM's racer falls there is an oracle question and is not answered
here.** Do not "fix" it by tuning budgets or by reverting collision; run
`tools/run_oracle.sh` on boss 38 and compare. If the ROM finishes, the port has a
real physics divergence *that this change exposed rather than created*. Either way
both boss fixtures now need a route that survives object collision — the same
missing capability as the navigation-primitive item.

> **Lead integration resolution (2026-07-26):** the release gates now drive boss
> level **46**, Tricky's second Fire Mountain arena. It exercises the same
> grid-mask, first-visit cutscene, and win/lose verdict contracts, deliberately
> saturates the broken grid mask, and finishes with production object collision
> enabled. The fixed/broken grid arms measure 270/500 peak candidates, 0/36
> truncations, and `fin=1`/`fin=0`; the verdict's first-win and already-cleared
> arms both reach `racer_boss_finish()`. Level 38 remains open as its own
> racing-line/oracle comparison. No budget was widened and no gate enables
> `MDKR_OBJCOLL=legacy`.

### What the next person should watch

- **This changes physics wherever collision-meshed objects exist** — doors, logs,
  bridges/whale ramps, snowballs, hit testers. Measured spread: hubs move a lot
  (1730 hits on the hub tour), race tracks barely (9 on boss 38, 0 on 5/32/15) —
  but as the boss regression shows, "barely" is not "not at all".
- **Wedging** is bounded but not impossible: the matched body's own `counter > 10`
  bail resets the racer to `x2/y2/z2`.
- `MDKR_OBJCOLL=trace` prints the frame of every hit. That is what turned "9 hits
  somewhere in 9400 frames" into "all nine at 4598–4606, none at the summit", and
  it is the first thing to reach for when a route's outcome changes.
- **This is the first fidelity fix landing where the oracle already has routes**
  (the hub), unlike the three flips in wave "closedloop" that measured no payoff
  because no route drives a racing line. An oracle before/after on the hub routes
  is the obvious next measurement and partly answers that gap.

## SWEPT: three shapes no instrument could see — wave "boundsweep"

Three recorded defects, each the visible member of a class that **no runtime
instrument in this tree could detect**. Per CONTRIBUTING.md rule 6 each was swept
mechanically rather than fixed in isolation, with a new enumerator —
`tools/sweep_bug_shapes.py` — built because the instrument did not exist. The
sweep found **three more instances than the three reported**, one of them with a
one-slot margin.

Scope note: the third class (variable shift counts) was fixed by **wave
"keyshift"**, concurrently and better — it proved the sites are folded to zero at
`-O2`, i.e. live rather than latent. What this wave contributes there is the
enumeration that closes the class and the permanent detector; see Class 3 below.
Classes 1 and 2 are this wave's.

### What was measured BEFORE anything changed

33 headless runs: all 20 main tracks + all 10 boss tracks (`MDKR_AUTOPILOT`,
6500 frames; boss tracks re-run at 13000 to reach Tricky's summit), the Adventure
hub (12000), the attract demo (6000) and a menu route (2300). Every run exited 0.

| write | caller capacity | peak count | **min per-call slack** | calls at the bound |
|---|---|---|---|---|
| `get_inside_segment_count_xz` → `segmentsInside[]` | 8 | 4 (track 4) | **4** | **0** |
| `get_inside_segment_count_xyz` → `inSegs[]` | 28 | 4 | **24** | **0** |
| `collision_get_y` → `yOut[]` | 8, 8, 9, 10, 30 | 7 (boss 40, 53) | **4** | **0** |
| `func_800BDC80` → `D_8011C3B8[]`/`D_8011C8B8[]` | 64 / remaining room | 8 | **56** | **0** |
| `func_800BDC80` → its own `spD8[]` | 300 | 9 | **291** | **0** |
| `generate_collision_candidates` → `gCollisionCandidates[]` | 500 | 416 (levels 41, 54) | **84** | **0** truncations |

**Slack is per call, and that distinction cost this write-up a wrong claim.** The
first version of this table read "`collision_get_y`: peak 7 against the smallest
caller array of 8 — one slot". That is false, and the probe caught it: the peak of
7 occurs at a caller holding 30 or 10 elements, while the 8-element callers never
exceed 4. A peak count means nothing on its own when one callee serves callers of
five different capacities, so the probe records `min(capacity - count)` over
individual calls instead. The honest tightest margin in the class is **4**, at two
sites, not 1 at one.

The same numbers come out of the unbounded arm (`MDKR_SEGBOUND=legacy
MDKR_COLLCAP=legacy`) of the *fixed* binary, which is the no-op proof for the
high-water marks themselves. **Everything below is latent. Nothing here fixes a
live defect**; what it fixes is the failure mode, which in every case is a silent
write into someone else's memory rather than a trap.

### Class 1 — an unbounded write through a bare pointer

A callee writes N elements through a pointer parameter where N is not derivable
from the parameters. **Nothing can see it**: UBSan `array-bounds` needs an indexed
array *type* and the callee has only a pointer; ASan only helps if the overrun is
actually reached, and the table above says none is. The instrument is the bound
parameter itself.

`tools/sweep_bug_shapes.py bare-pointer` enumerates every function in `game/src`
that writes through a pointer parameter in a loop: **27 instances at the time of
this wave, all triaged** (the reasons live in `SHAPE_TRIAGE` in
`tests/check_array_bounds_sweep.py`, which fails if any of them stops matching —
and equally if a new one appears with no reason). The population is now **31**:
the camera-obstruction port added `mdkr_camera_interpolated_view_projections:out`,
`mdkr_camera_dynamic_index_clear:buckets` and
`mdkr_camera_object_occlusion_sort_chunk_range:order`, and the last two carry no
bound-ish parameter. They are the current untriaged set — see that file, not this
table.

| verdict | n | which |
|---|---|---|
| **BOUNDED in this commit** | 5 | `get_inside_segment_count_xz`, `get_inside_segment_count_xyz`, `collision_get_y`, `func_800BDC80` (×2 output pointers) |
| bounded by an existing count parameter | 6 | `resolve_collisions` (`numEntries`), `fb_memcpy` (`len`), `get_controller_pak_file_list` (×2), `font_codes_to_string`, `string_to_font_codes`, `filename_decompress` (`length`) |
| unreachable — zero callers | 2 | `music_get_fx_mix_all_channels` (UNUSED), `func_8000E79C` |
| **COMPILED OUT by the game-core memory-safety wave** | 2 | `strcpy`, `strcat` (`unused_string.c`, now whole-file `#ifndef NON_MATCHING`) |
| the C-string contract (bounded by the input's NUL) | 1 | `fontConvertString` |
| **BOUNDED by the game-core memory-safety wave** | 2 | `parse_string_with_number` (×2 — both `REGION` arms; explicit `DKR_PARSED_STRING_MAX` capacity, `_Static_assert`ed at both call sites) |
| already handled by an earlier wave | 3 | `filename_trim`, `func_8002FF6C`, `func_80026E54` — all three had their *callers* resized, with `NATIVE_PORT` comments |
| recorded, another wave's file | 1 | `savemenu_blank_save_destination` (menu.c) |
| **BOUNDED by the virtual-Pak wave** | 4 | `func_800756D4` ×4 (save_data.c — pointer-plus-capacity contract; sole caller passes six elements) |

**`collision_get_y` is the one that matters, and it was not in the brief.** The
decomp's own comment states the defect — *"There is no limit for surfaces
returned, so not feeding a large enough yOut array could cause problems"* — and it
is the widest-open member of the class: the count is per **triangle** (every
collision facet of every batch whose X/Z footprint contains the query point)
rather than per segment, and it has **five** callers with five different array
sizes (`colY[8]`, `yOut[8]`, `yOut[9]`, `yVals[10]`, `colY[30]`), so no single
capacity can be assumed. It is reached on *every* route including the menus,
because the frontend renders Timber's Island. Measured min slack **4** (tied with
the reported instance for the tightest in the class), peak count 7 on boss levels
40 and 53.

`func_800BDC80` is the third find: three unbounded writes in one function (two
caller arrays and its own 300-element local), no cap of any kind, and the second
output pointer indexes `D_8011C8B8[128]` at a **running** offset, so its real
capacity varies per call — the call site passes `min(ARRAY_COUNT(D_8011C3B8),
128 - D_8011D0B8)`.

All five fixes are **`NATIVE_PORT`-gated host adaptations**, not transcription
corrections: the ROM's signatures genuinely have no bound.

**What the caller should do when the bound is hit was the interesting question**,
and the answer differs by site:

- `get_inside_segment_count_xz` — do nothing new. `cnt` is deliberately **not**
  clamped, so the return value stays byte-identical to the ROM's, and the caller's
  existing `if (segmentCount == 0 || segmentCount >= 8) return 0;` — the ROM's own
  behaviour for the overflowing case — keeps rejecting exactly that case. Only the
  writes stop. Clamping to 7 would silently turn an overflow into "7 valid
  segments", which is a real behaviour change.
- `collision_get_y` and `func_800BDC80` — the count **must** be clamped, because
  callers read `out[0 .. count-1]`. An unclamped count would hand them
  uninitialised stack, which is a worse bug than the one being fixed.

### Class 2 — a cap tested by equality against an insert that can step over it

`generate_collision_candidates` tests `if (j == MAX_COLLISION_CANDIDATES)` after
the **facet** insert. The **segment** insert at the top of the outer loop advances
`j` with no test at all — faithfully, `game/src/hasm/collision.s`:

```
.L80031368:
  8003137C  sw    $v0, 0x0($s2)     # gCollisionCandidates[j] = seg
  80031388  addiu $s3, $s3, 0x1     # j++          <-- NO cap test
  ...
  80031464  addiu $at, $zero, 0x1F4 # 500
  80031470  beq   $s3, $at, .L800314A8   # the ONLY cap test, on the facet insert
```

Enter the loop body with `j == 499`, write slot 499, leave with `j == 500` and no
exit. The facet insert then writes slot **500** — one past
`mempool_alloc_safe(MAX_COLLISION_CANDIDATES * 4)` — `j` becomes 501, and `j ==
500` is never true again, so the overrun runs on for every remaining triangle of
every remaining segment.

Fixed with a `j >= cap` **pre-check at both inserts**, `NATIVE_PORT`-gated (the
assembly really has no guard, so this is an adaptation). The ROM's post-increment
equality test is left exactly as it transcribes: with the pre-checks in place `j`
can never exceed the cap, so `==` and `>=` are the same test, and keeping the ROM's
form makes the diff an addition rather than an edit. Behaviour before the boundary
is unchanged on every path; at the boundary it stops with
`gNumCollisionCandidates == 500`, which is what the ROM's own test produces.

The sweep enumerated the class as *"a counter that indexes a write and is compared
by equality to a constant"*: **36 instances at the time of this wave, 7 of them
against something that is plausibly a capacity** (the current tree measures 50 and
18 — the camera-obstruction port and the 1.0.5 bounds work both added members).
Of the original 7:

| site | verdict |
|---|---|
| `generate_collision_candidates: j == MAX_COLLISION_CANDIDATES` | **the defect — guarded** |
| `generate_collision_candidates: counter == 10` | safe: **one** increment, immediately after the only write, from 0 — it lands on 10 exactly. The contrast with the line above is the whole lesson |
| `audspat_point_create: gNumAudioPoints == MAX_AUDIO_POINTS` | safe: the test is a **pre**-check that returns early, single increment past it |
| `waves_visibility: var_v1 != ARRAY_COUNT(D_8012A600)` | safe **but coupled** — see below |
| `func_8001F460: var_t0 != D_8011AE78`, `func_80021600: j != D_8011AE78` | safe: linear-search idiom after `for (i = 0; i < D_8011AE78 && …; i++)`, so `!=` means "not found" |
| `func_8001F460: var_s2 != 0x7F` | safe: 0x7F is a stream sentinel, not a capacity |

The remaining 29 were `i == 0`-style state tests, counted rather than enumerated;
the current tree counts 32 of them.

**The one new find here is `waves_visibility`.** Its reset loop strides **four**
and terminates on `!=`, so it stops only because `ARRAY_COUNT(D_8012A600)` is 24 —
which is `WAVE_VISIBLE_SLOTS - 2`, a ROM fact recorded a hundred lines above it and
exactly the sort of number a future correction moves. At 25 or 27 slots the loop
never equals the bound and walks off the end writing `blockID = -1`, in the same
data neighbourhood as the two browser crashes in the "wavetable" section. A
`_Static_assert` on the divisibility now holds the coupling. No code, no
behaviour, no way to reintroduce it silently.

### Class 3 — a variable shift count that can reach 32

**Wave "keyshift" fixed every site in this class while this sweep was running, and
its account is the correct one — read that section, not this one, for the defect.**
What is recorded here is the enumeration and the permanent detector; the fix and
its evidence are theirs. Two corrections to what this wave believed on the way in:

- the sites were **not** "currently correct by luck". At `-O2` clang folds them to
  zero, and the port's *web* build is Release, so this was a **live, reported**
  defect — the key cutscene replaying after every race — not a latent one. A probe
  run at `-O0` said nothing about it, which is the lesson `DEVELOPER_HANDBOOK.md` §3 now
  carries;
- the right fix is `DKR_SHL32(x, n)` in `game/include/macros.h`, which reproduces
  MIPS `sllv` for *every* count, rather than the per-site `& 31` this wave was
  writing, which only reproduces it for the counts each site happens to use.

**The enumeration still stands, and it is the coverage claim.**
`tools/sweep_bug_shapes.py shift-count` finds every shift in `game/src` whose count
is not a literal: **147 instances, and exactly 6 of them are the `var + K (K ≥ 24)`
mask idiom** — four in `game.c` (the key gate, its latch, the boss-approach flag,
and wave "keyshift"'s own trace probe), one in `objects.c` (the Taj offer) and one
in `waves.c` (`obj_wave_height`). That is precisely the set wave "keyshift" fixed,
reached from an independent direction — it swept the *save fields* and arrived at
the shifts, this swept the *shift syntax* and arrived at the same six — so the
class is closed by two methods that agree. The other **141** (133 in the current
tree) are shifts by a plain variable with no added constant; none reported a bad
exponent on any route.

The enumerator matches `DKR_SHL32(x, n)` as well as `<<`, deliberately: after the
fix the six sites contain no shift operator at all, and a `<<`-only sweep would
have reported the class as empty — the exact way a sweep silently stops
covering the thing it was built for.

**The detector is now permanent.** `-fsanitize=shift-exponent` was added to
`tests/check_array_bounds_sweep.py`'s instrumented build, alongside a required
`__ubsan_handle_shift_out_of_bounds` import so that "no shift reports" can never be
confused with "the flag was dropped" — which matters more here than for
array-bounds, because a *source* fix at every site legitimately empties the report
list and would otherwise leave nothing proving the instrument is still armed.

Before the fix landed, that phase reported two of the six on routes the sweep
already drove: `game.c`'s `8 << (worldId + 31)` on **boss38** and `waves.c`'s
`var_t0 <<= (unk2 + 0x1F)` on **nav_to_track_select** (the frontend renders
Timber's Island, which has water). Note what that means about instrument choice:
the Debug UBSan build is the only place these are visible **as shifts**, because at
`-O2` the statement is deleted outright and there is no shift left to report.
Neither instrument alone is sufficient — UBSan at `-O0` sees the UB, and only a
Release build shows the consequence.

### The instrument: `tools/sweep_bug_shapes.py`

A line-based parser over comment-stripped, **arm-selected** source (it evaluates
`#ifdef`/`#if defined` against this build's macro set, so it sees the code we
compile). Two things it got wrong first, both caught by its own fails-closed
checks and worth knowing:

- `objects.c`'s `#ifdef ANTI_TAMPER` opens a brace in **both** arms, so a pure
  brace matcher swallowed **8006 lines of objects.c into one "function"** and
  mis-attributed every finding in it. Arm selection fixed that;
- `printf.c`, `menu.c` and `game_ui.c` then still failed to balance, because a
  preprocessor arm is *allowed* to carry an unbalanced brace (`#ifdef
  HAVE_LONGLONG` opens an `else {` whose `}` is outside the `#endif`). No arm
  selection can fix that, so the parser is line-based instead: a top-level
  definition starts in column 0 and ends at the first line that is exactly `}`.

Three more false positives came out of running it and reading the results:
`*idx += n` on a scalar out-parameter read as a pointer walk (three `particles.c`
functions), and `attachPoint->obj[i] = …` read as a write through a parameter
called `obj` (that one is an indexed array member, which UBSan array-bounds
already covers — a different class).

It fails closed three ways: a `PARSE SHORTFALL` if a file's parsed definitions are
fewer than its column-0 `}` lines; `--selftest`, which requires the known instances
of all three classes to still be found; and the check's own requirement that the
TRIAGE set be non-empty.

### Checks, and both directions

`tests/check_array_bounds_sweep.py` grew from one phase to three. Phase 3 exists
because **every bound in this commit is unreached**, so phases 1 and 2 would pass
unchanged on a build with all of them deleted.

| direction | measured result |
|---|---|
| phase 1, `-fsanitize=shift-exponent` before wave "keyshift" landed | `game.c` + `waves.c` "shift exponent 32 is too large" on the existing boss38 and nav_to_track_select routes → **FAIL** |
| phase 1, after it landed | no shift reports; `__ubsan_handle_shift_out_of_bounds` still imported, so the flag cannot silently die |
| phase 2, any TRIAGE finding without a reason, or growth in the INFO population | **FAIL**, listing it |
| phase 3, `MDKR_SEGMARGIN=100000`, bound in place | `xzMax=24/8 clamped=349`, **exit 0** — 24 overlapping segments into an 8-slot array, held |
| phase 3, same margin + `MDKR_SEGBOUND=legacy` | **exit -6 / 134 (SIGABRT)** — the stack protector catching the smashed frame |
| phase 3, `MDKR_COLLCAP=32`, guards in place | `maxCandidates=32 truncated=237` — 237 crossings of the boundary, never one past it |
| phase 3, `MDKR_COLLCAP=legacy` | `maxCandidates=353` — 321 slots past the cap, the step-over reproduced |
| phase 3, bounds-plumbed (two routes, union) | `xz=8 xyz=28 colY=10 shTri=64 shHgt=300`, min slack 6/24/5/56/291 — every callee really was handed its caller's `ARRAY_COUNT` |

Every one of those numbers is identical on the **Release** build
(`docs/RELEASE_CHECKLIST.md` §2), which matters here more than usual: wave
"keyshift" had just shown that a guard can exist at `-O0` and be deleted at `-O2`.

`MDKR_SEGMARGIN` widens the callee's own 4-unit acceptance margin, so the overflow
is produced by the **real mechanism** (many overlapping segments), not a faked
index. Measured in the control: 24 overlapping segments against the 8-slot array,
349 calls at the bound; the guarded arm exits 0, the unguarded arm dies with
SIGABRT (the stack protector catching the smashed frame).

**A better boundary proof than the synthetic one already existed and still holds.**
`check_collision_gridmask`'s deliberately-broken arm (`MDKR_GRIDMASK=off`)
saturates the candidate list **73 times** on boss 38 — i.e. the real cap boundary
is crossed by an existing check — and every number it asserts is unchanged with the
pre-guards in place: `truncated=73 airborne=301 peakY=2118 fin=0` broken,
`truncated=0 airborne=27 peakY=4868 fin=1` fixed, exactly the recorded values. The
guard changes nothing at the boundary; it only removes the step-over past it.

**Phase 3 found two defects in itself on its first run, and that is the argument
for writing the assertions this way.** The segment control was pointed at
`nav_to_track_select`, a route where `get_inside_segment_count_xz` is never
entered (measured `xzMax=0`) — the control's own "did the boundary get reached"
assertion caught it, where a bare "the legacy arm crashes" test would have passed
vacuously in the fixed arm and failed for the wrong reason in the other. And the
candidate control reported `truncated=0` at `cap=32` because the truncation
counter was still comparing against the ROM's 500 rather than the effective cap.

**The no-op proof.** The pre-change binary (built from `main`) and the post-change
binary were driven over 31 routes — 20 main tracks, 10 boss tracks and the
Adventure hub — and every `[PACE]` racer row is **byte-identical**, ~36 000 rows.
(The attract demo is excluded and the comparison says so out loud: it has no
*player* racer, so it emits no `[PACE]` rows and could only pass vacuously.)

### New env hooks

`MDKR_SEGMARGIN=<n>` (widen the segment-overlap acceptance margin, default 4),
`MDKR_SEGBOUND=legacy` (remove the bounds added here),
`MDKR_COLLCAP=<n>|legacy` (lower the candidate cap, or remove its guards). All
three are no-ops unless set.

### Left alone, and why

- **The 29 informational equality-cap sites and the 141 informational variable
  shifts** (32 and 133 in the current tree). Counted, not enumerated: the check
  fails if either population *grows*, which catches a new instance without
  pretending 170 harmless lines were each read closely. Note the consequence —
  the equality-cap count has since crossed its `SHAPE_INFO_MAX` ceiling, so the
  check is doing exactly what it was built to do and is waiting for someone to
  read the new members.
- **`menu.c`'s `savemenu_blank_save_destination`.** A real member of class 1 in
  another wave's file. `save_data.c`'s `func_800756D4` was in this bucket on the
  grounds that the port had no Controller Pak; it has one, and the virtual-Pak
  wave gave that API an explicit output capacity (see the class-1 table above).
- **`music_get_fx_mix_all_channels`, `func_8000E79C`.** Genuinely unbounded and
  genuinely uncalled. Bounding dead code buys nothing; the sweep will report them
  the day one gains a caller. `unused_string.c`'s `strcpy`/`strcat` were here
  until the game-core memory-safety wave compiled the whole file out under
  `#ifndef NON_MATCHING` — they carry libc's external names, so on a hosted build
  they interposed the platform's implementations for the entire program.
- **`filename_decompress` writes `output[length]`**, so its buffer must be
  `length + 1`. True at every call site today; noted because it is the same
  off-by-one family as the `filename_trim` overrun an earlier wave had to fix.
- **The allocation is not lowered with `MDKR_COLLCAP`.** The evidence a check
  needs is the write *index*, which `[COLL] maxCandidates` already reports; making
  the emulated overrun a real heap write would trade a deterministic assertion for
  a nondeterministic crash.

### G4 follow-up: per-level headroom measured and gated, cap NOT raised

The class-2 guard above (`j >= mdkr_coll_cap(MAX_COLLISION_CANDIDATES)`) is
confirmed still present
at both insert sites — the segment insert and the facet insert — in
`game/src/hasm/collision.c`. What it does not do is add headroom: a saturated
list still silently drops every candidate past the cap, the exact mechanism wave
"gridmask" fixed one instance of. Levels 41 and 54 remain the tightest margins in
the game, unchanged since the table above: 84 of 500 slots.

This wave adds two things, both instrument/gate-only — **the cap is not
raised**, because the layout/perf effect of a larger `gCollisionCandidates`
allocation is unmeasured:

- `[COLPEAK] candidates new peak N of M` — `MDKR_TRACE`-gated, printed each time
  a run's high-water mark advances (`platform/stubs_dkr.c`
  `mdkr_coll_candidates()`), in the exact pattern of `[EVTQ]`'s per-queue peak
  telemetry in `platform/audio_event_queue.c`. Purely additive: it cannot change
  which candidates are kept, only whether a peak gets a trace line. The
  unconditional `[COLL] maxCandidates=N truncated=N cap=N` summary every route
  already emitted at headless exit (needing no `MDKR_TRACE`) is unchanged and is
  what the check below actually parses.
- `tests/check_collision_headroom.py` — one `MDKR_AUTOPILOT` run per level, all
  ten boss levels plus one ordinary race (Ancient Lake), reading that `[COLL]`
  line. Fails if truncation happens in normal play, or if a level's high-water
  mark exceeds a frozen baseline ceiling (measured peak + 16 slots of slack) —
  the regression tripwire for headroom quietly shrinking further. A positive
  control (`MDKR_COLLCAP=150` on level 41's own route, well under its natural
  416 peak) confirms the guard holds under a forced boundary and that doing so
  trips the check's own "truncated must be 0" rule — the control is not
  vacuous. Registered in `tools/run_checks.py` as `collision_headroom`.

Measured per-level high-water (this binary, `MDKR_AUTOPILOT`, one run per
level, `race_full_3lap_tt.txt`):

| track | frames | peak candidates | truncated | cap | margin |
|---|---|---|---|---|---|
| 38 (Tricky 1) | 13000 | 270 | 0 | 500 | 230 |
| 46 (Tricky 2) | 13000 | 270 | 0 | 500 | 230 |
| 40 | 13000 | 55 | 0 | 500 | 445 |
| 53 | 13000 | 55 | 0 | 500 | 445 |
| 1 | 13000 | 152 | 0 | 500 | 348 |
| 52 | 13000 | 152 | 0 | 500 | 348 |
| **41** | 13000 | **416** | 0 | 500 | **84** |
| **54** | 13000 | **416** | 0 | 500 | **84** |
| 37 | 13000 | 149 | 0 | 500 | 351 |
| 55 | 13000 | 97 | 0 | 500 | 403 |
| 5 (Ancient Lake, ordinary race) | 6500 | 30 | 0 | 500 | 470 |

Identical to the original "boundsweep" measurement for levels 41/54/38/46 (the
rows that table already recorded), and fills in the remaining six boss levels
plus one ordinary race that table did not enumerate individually. No level
truncates. **Decision: accept, do not raise.** Every measured route — boss and
ordinary alike — stays well clear of the 500 cap, and `check_collision_headroom.py`
is now the tripwire if that stops being true.

**Deliberately deferred.** Boss levels 41 and 54 peak at 416 of 500
candidates — 84 slots of measured headroom — and the cap can no longer be
stepped over: a `j >= cap` pre-check guards both inserts ahead of their
stores. All ten boss levels plus an ordinary race are swept per-level with
zero truncation, and the gate freezes each level's high-water so headroom
cannot quietly shrink without failing. Raising the cap would spend memory on
margin that nothing has approached while removing the pressure that keeps
this measured. There is no custom-content pipeline in this build, so headroom
cannot shrink from unvetted levels, and racer count does not multiply
per-call load because the candidate list resets per racer. The honest limit
is that the sweep drives one deterministic line per level, so 84 slots is a
measured floor for that line rather than a proof of the level's worst case —
which is exactly why the ceiling is frozen and watched rather than papered
over with a bigger number.

## FIXED: racers fall through Tricky's volcano — the collision grid mask never filtered in Z (wave "gridmask")

Reported from the browser build, playing the **first boss race (Tricky, Dino
Domain)**, in three instalments:

1. *"Riding over an elevated edge tilts the vehicle, it never tilts back, and in
   places it can then clip through the map."*
2. *"Actually both me AND the boss clip through the map and fall off at that point,
   going up the mountain/volcano — so it's a more localized sim issue."*
3. *"On first load of the first boss, it went straight to the 'you lose' animation,
   then started the race"*, later *"I finished the race and was awarded first place,
   and still got the failure animation, and was returned without having won."*

**One defect, and it is not the tilt.** (1) and (2) are the same fault; (3) is
addressed separately at the end of this entry and is **not reproduced**.

### Mechanism

`generate_collision_candidates()` (`game/src/hasm/collision.c`) builds the list
`resolve_collisions()` walks. It pre-filters triangles with a coarse **8×8
occupancy mask** per terrain segment, from `compute_grid_overlap_mask()`: the mask's
low byte is the X columns the query rectangle spans, the high byte the Z rows, and a
triangle is kept only if it shares ≥1 column **and** ≥1 row with the query.

The upstream `NON_MATCHING` C body's Z loop tested

```c
if (cell_z + cell_height >= z1 && z2 >= bbox_z1) {   /* WRONG */
```

and the clamping directly above it has already forced `z2 >= bbox_z1`
(`if (z2 < bbox_z1) z2 = bbox_z1;`). The second half is therefore a **tautology**:
every Z row from the first accepted one through row 7 is set, and the Z half of the
mask stops filtering anything. The X loop, ten lines earlier, is correct
(`x2 >= cell_x`).

**The ROM tests the rolling per-row value, exactly mirroring X.**
`compute_grid_overlap_mask`, verified instruction by instruction in
`game/src/hasm/collision.s`:

```
800315C4  or   $t0, $t1, $zero   # cell_z = bbox_z1
800315C8  slt  $at, $t4, $a2     # (cell_z + cell_height) < z1 ?
800315D0  slt  $at, $t5, $t0     # z2 < cell_z ?      <-- the test, vs. cell_z
800315F4  add  $t0, $t0, $t2     # cell_z += cell_height   (branch delay slot)
```

`$t5` is the clamped `z2`; `$t0` is `cell_z`, initialised to `bbox_z1` and advanced
every iteration — the same rolling register the X loop uses at `80031590`.

**Upstream never executes that C.** The matching N64 build takes
`GLOBAL_ASM("asm/collision/compute_grid_overlap_mask.s")`; the `NON_MATCHING` body
exists for readability. This port builds with `NON_MATCHING=1`, so we are the first
to run it. Checked against upstream `3b2dd520` — which is now also this tree's
`.decomp-baseline`, and is at 99.38 %: **`src/hasm/collision.c` is untouched by any
of those commits and still carries the tautology.** Verified after the sync that
the *only* line this commit removes from that file is the buggy condition
(`git diff <main> HEAD -- game/src/hasm/collision.c`). Worth sending upstream.

### Why it is silent almost everywhere and catastrophic in one place

An over-permissive *pre-filter* still yields **correct** collisions — it just feeds
`resolve_collisions()` far too many candidates. But `gCollisionCandidates` is
`MAX_COLLISION_CANDIDATES` == **500** entries (`mempool_alloc_safe(500 * 4)`,
`tracks.c:2922`) and the fill loop **truncates** on full:

```c
if (j == MAX_COLLISION_CANDIDATES) { goto out; }
```

So nothing goes wrong until a racer's search rectangle sees more than 500 collidable
triangles — and then the *discarded tail* can hold the very facets the racer is
standing on. All four wheels report `SURFACE_NONE`, `groundedWheels` goes to 0, and
the racer free-falls out of the level.

Tricky's arena is a **spiral ramp up a volcano**, so many track segments stack
inside one X/Z footprint — and the segment test is 2-D (X and Z only). It is the
worst case in the game for this.

### Measurement

New probes, both trace-gated: `[COLL] maxCandidates=N truncated=N cap=500` at
headless exit, and `[GRND] frame=N pi=P ri=R gw=K surf=… xrot=… zrot=…` per racer
per frame. `MDKR_GRIDMASK=off` restores the tautology from the same binary.

Boss level 38 (Tricky 1), `MDKR_AUTOPILOT=1`, 9400 frames:

| | `MDKR_GRIDMASK=off` | fixed |
|---|---|---|
| peak candidates | **500** (saturated) | 310 |
| truncations | **73** | **0** |
| longest ground loss, boss (`pi=-1 ri=1`) | **301 frames**, from frame 6859 | 16 |
| longest ground loss, human (`pi=0 ri=0`) | **103 frames**, from frame 7057 | 27 |
| peak y | 2118 (falls off mid-spiral) | **4868** (climbs it all) |
| `fin=` | 0 — **the race never finishes** | **1** |

The **boss falls first** (frame 6859 vs 7057) and for three times as long, which is
exactly the reporter's second observation and the thing that rules out per-racer
state: `generate_collision_candidates()` is called per racer per frame and the
truncation depends only on the geometry at the query position.

**The tilt was a symptom, not the bug.** Pitch and roll are derived from where the
four wheels touched down (`func_80054FD0`, `racer.c`), so with no ground there is
nothing to restore them from. Measured over frames 6900–7160:

| | `off` | fixed |
|---|---|---|
| roll (`zrot`) during the fall | **frozen at 75** for all 103 frames | 75 → 41 → 400 → 95 → 115 → 155 |
| peak \|pitch\| (`xrot`) | 4793 | 1797 |

"It tilts and never tilts back" is that frozen 75.

### Scope: how much of the game was affected

All 20 main tracks and all 10 boss tracks, both arms, 6500 frames each with
`MDKR_AUTOPILOT` (boss tracks re-run at 13000 to reach the summit):

- **Only Tricky's two arenas truncate**: level 38 (73 truncations) and level 46
  (75) — the same Fire Mountain geometry, boss 1 and boss 2.
- Everything else stays under the cap. The fix still lowers the peak nearly
  everywhere (e.g. track 32: 184 → 107; 17: 127 → 75; 55: 297 → 99), i.e. every
  track was doing 1.5–3× the collision work it should.
- **Two tracks sit close to the cap even after the fix** — levels 41 and 54 peak at
  **416** in both arms (the fix does not lower them: at their hot spot the query
  really does span all eight Z rows). 84 candidates of headroom. Recorded, not
  acted on.
- **The fix is behaviour-neutral where nothing truncates.** All 30 tracks' `[PACE]`
  racer position streams are **byte-identical** between the two arms (3859 rows
  each). That is the strongest available statement that this only changes what it
  had to.

Boss level ids came from the ROM, not from source: `ASSET_MISC_BOSS_TRACKS_IDS`
(`tools/dump_misc_asset.py`, sub-asset 30) = **38, 46, 40, 53, 1, 52, 41, 54, 37,
55**. They are *not* in `ASSET_MISC_MAIN_TRACKS_IDS`, which is why no existing sweep
covered them. **The project status table's claim that boss races are "not started"
was stale** — a boss race loads, plays its cutscene, and races correctly.

### Fix

`game/src/hasm/collision.c`, one line plus the `MDKR_GRIDMASK` A/B hook:

```c
s32 zRowNear = cell_z;                       /* the ROM's rolling per-row value */
if (mdkr_gridmask_legacy()) zRowNear = bbox_z1;   /* A/B, no-op unless set */
if (cell_z + cell_height >= z1 && z2 >= zRowNear) { mask |= v1; }
```

Deliberately **not** wrapped in `#ifdef NATIVE_PORT`: this is not a host
adaptation, it is a transcription correction that makes the C agree with the
assembly it is a rendering of. The comment cites the exact instructions.

### Regression check

`tests/check_collision_gridmask.py` — four runs from one binary, muted + headless,
~4 min. Because the broken arm is re-produced on every run and has to *exhibit* the
defect before the fixed arm is credited, it cannot pass vacuously. Fails closed if
either probe disappears.

**Positive control (fix reverted, rebuilt):**

```
$ MDKR_AUDIO=0 python3 tests/check_collision_gridmask.py
  - fixed arm truncated the candidate list 73 time(s) (want 0) …
  - fixed arm peaked at 500 candidates, at or above the 500 cap
  - fixed arm: racer pi=-1 ri=1 spent 301 consecutive frames with no wheel on a surface (want <= 45) …
  - fixed arm only climbed to y=2118 (want >= 3500) …
  - fixed arm never reached fin=1 …
  - the LOSE cutscene (cutscene=5) was never loaded …
  - no 'bossfinish:' line — racer_boss_finish() never ran …
check_collision_gridmask: FAIL                              (exit 1)

$ MDKR_AUDIO=0 python3 tests/check_collision_gridmask.py    # fix restored
check_collision_gridmask: PASS  (boss 38: broken truncated=73 airborne=301 peakY=2118
  fin=0 | fixed truncated=0 airborne=27 peakY=4868 fin=1)   (exit 0)
```

**One assertion was written, measured to be fragile, and removed** — an upper
bound on the *broken* arm's peak y (2118 on this build). While positive-controlling
the cutscene assertions (which lengthen the pre-race cutscene by ~136 frames and so
shift the whole AI racing line) the broken arm reached the summit *first* and fell
later: still 73 truncations, still a 301-frame ground loss, but peak y 4868. An
assertion that fails on an unrelated route shift is not measuring the defect. The
truncation count, the saturated cap, the per-racer ground loss and the unfinished
race are all route-independent and carry it instead.

No regressions: `check_race_drive`, `check_determinism`, `check_race_finish_time`,
`check_adventure_hub`, `check_adventure_race_loop`, `check_race_2p_split` and
`check_track_sweep` all PASS.

### Report 3 — "the lose animation played before the race", and "1st place recorded as a loss": NOT REPRODUCED

**An animation before the race is correct behaviour.** For a not-yet-visited
`RACETYPE_BOSS` level, `level_load()` (`game/src/game.c:475`) pushes the real level
onto the level-property stack and swaps in a *cutscene* level via `ASSET_MISC_67`
(38 → 57), then `thread3_main` pops back and the race starts. Level 57 holds **five**
cutscenes in one object map, selected by `gCutsceneID` against each animation
object's `channel` (`objects.c func_8001E4C4`); a census of level 57 counts
`3:23 4:35 5:16 6:30 7:24` animation objects, and `racer_boss_finish()`'s pushes in
`game/src/vehicle_tricky.c` name them: **3 = challenge (first visit), 4 = win,
5 = lose, 6 = wizpig amulet, 7 = rematch**.

Measured on a **fresh `save/eeprom.bin`**, boss 38:

```
level_load: levelId=38 @2641
bossredirect: boss=38 -> cutsceneLevel=57 cutsceneId=3 bosses=0x0 courseFlags=0x0 worldId=1
level_load: levelId=38 @3348                     <- the race
bossfinish: finishPos=2 …                        <- autopilot comes 2nd
level_load: levelId=57 entrance=5 cutscene=5 @9107   <- the LOSE cutscene, AFTER
level_load: levelId=0  @9950                     <- back to the hub
```

and the rendered cutscene at frame 3070 is Tricky saying **"Now I challenge you to a
RACE!"** — channel 3, correct.

**Channel 5 before a race is unreachable by construction.** `level_load()` assigns
only two literals, `CUTSCENE_ID_UNK_3` and `CUTSCENE_ID_UNK_7`; the sole producer of
cutscene 5 is `racer_boss_finish()`'s losing branch, which by definition runs after
a finish. There is no out-of-bounds/respawn path that sets a lose flag either — the
only other `raceFinished = TRUE` writes are the egg and silver-coin *challenge*
modes (`object_functions.c:802`, `:4319`).

**"Awarded 1st, recorded as a loss" does not reproduce either.** A boss race has two
racers and `MDKR_AUTOPILOT` drives the human with the boss's own AI, so the human
always finishes second and the win branch was unreachable from any headless route.
`MDKR_BOSS_SLOW=1` (new test hook, `platform/stubs_dkr.c`) scales the boss's forward
velocity so the human wins. Both directions, measured:

| | `finishPos` read by `racer_boss_finish()` | cutscene loaded |
|---|---|---|
| normal | **2** | **5** (lose) @9107 |
| `MDKR_BOSS_SLOW=1` | **1** | **4** (win) @9139 |

So `finishPosition` follows the actual finish and the verdict follows
`finishPosition`. Both are asserted by the check now, so a real regression here would
be caught. Reasoning that agrees: for a boss race `race_check_finish()`
(`objects.c:6698`, `:6786`) sets `raceFinished` and `finishPosition` **in the same
statement pair**, so the two can never be out of step, and `gNumFinishedRacers`
starts at 1 (`objects.c:1287`) so position 1 really means first.

**What is left unexplained, honestly.** The reporter reached the boss through
**Adventure** (hub → Dino Domain lobby → boss door); every measurement here reaches
it through `MDKR_LOAD_TRACK`, which retargets a Tracks-mode route. The Adventure path
differs in `settings->bosses` / `courseFlagsPtr[38]` (both 0 here) and in what a
mid-race abort returns to. Building an Adventure route to a boss needs four *won*
Dino Domain races, which is a wave of its own. The most economical explanation is
that report 3 is downstream of the defect fixed above: with the collision truncation
present, the first Tricky race **cannot be completed** — it aborts mid-spiral — so
whatever the reporter saw was produced while the race was in that state, and their
browser build also predates today's `main`. **A re-test on current `main` is the
next step, not more code.**

### Found and deliberately NOT fixed

1. ~~**`generate_collision_candidates()` can still run off the end of
   `gCollisionCandidates`.**~~ **NOW GUARDED; the ROM hazard is kept on record.**
   The ROM's cap test is an *equality*, `if (j == 500) goto out`, and its
   segment-pointer insert at the top of each segment iteration has no test at
   all. Enter an iteration with `j == 499`, the segment store lands at 499 and
   `j` becomes 500; the facet loop then writes index 500 (OOB — the array is
   exactly `500 * 4` bytes, `tracks.c`), `j` becomes 501, and `j == 500` is
   never true again, so it runs unbounded to the end of the segment list.
   `gCollisionSurfaces[j]` (500 bytes) goes with it. **Faithful to the ROM** —
   `collision.s` `.L80031368` increments `$s3` with no check and `.L80031470` is
   `beq $s3, 0x1F4`. This wave added a `NATIVE_PORT`
   `j >= mdkr_coll_cap(MAX_COLLISION_CANDIDATES)` pre-check, but only the
   segment-side insert was genuinely ahead of its store; the facet-side check sat
   one store *late*, so a full list still wrote one entry past the pool block.
   Both checks now precede their stores (`game/src/hasm/collision.c`,
   `generate_collision_candidates`), and the ROM's `==` test is retained below
   them rather than edited. No measured track reaches 500 at all (peak 416), so
   this is the failure mode rather than a behaviour change. **Still an
   original-game hazard on any N64 build.**
2. **`counter == 10` segment cap.** At most 10 overlapping segments are considered.
   Measured peak on every track swept: **3**. Nowhere near it.
3. ~~**`collision_objectmodel()` used `s32 spB4[10]` / `f32 sp8C[10]`
   against a count capped at 20.**~~ **FIXED:** both arrays now match the complete
   20-object candidate capacity. The live peak remains one, but safety no longer
   depends on that content observation.
4. **`objects.c func_80016748` (~line 5288) has the same `f32[4][3]`-for-`MtxF`
   idiom as the plane bug** — `f32 pad[2]; f32 obj1TransformMtx[4][3]` (56 bytes) for
   a 64-byte `MtxF`. **Already safe in this build**: `CMakeLists.txt:183` defines
   `AVOID_UB=1`, which selects the `MtxF` branch. The hazard is that dropping
   `AVOID_UB` would silently reintroduce an 8-byte stack overflow. No change made;
   noted because it was carried in as an open defect and it is not one today.
5. ~~**`gLevelPropertyStack[5 * 4]`** (`game/src/game.c`) is pushed without a
   bound.~~ **FIXED in the game-core memory-safety wave.**
   `racer_boss_finish()`'s Future-Fun-Land win branch pushes four frames and
   `level_load()` may already hold one — exactly 5, i.e. right at the limit with
   zero slack, and a sixth push wrote into the adjacent globals.
   `level_properties_push()` now drops a push that would exceed
   `ARRAY_COUNT(gLevelPropertyStack)` under `NATIVE_PORT`. Still not reached by
   anything measured.
6. **`func_800214E4()` (`objects.c`) reads `D_8011AE74[D_8011AE78]`** — one past the
   collected count — when its actor search finds no match, then passes it straight to
   `obj_init_animobject()`. Inside the 128-slot allocation, so not a wild pointer,
   but it is a stale/uninitialised entry. Not triggered on any boss route measured.

### EVTQ drop types 6 and 21 (from the same browser session)

`AL_SEQP_ENV_EVT` (**6**) and `AL_CSP_NOTEOFF_EVT` (**21**), decoded from
`enum ALMsg` in `game/include/PR/libaudio.h` (0-based: 2 = `AL_SEQP_MIDI_EVT`,
9 = `AL_SEQP_API_EVT`, 6 = `AL_SEQP_ENV_EVT`, 21 = `AL_CSP_NOTEOFF_EVT`).

The existing verdict — real, browser-only, boot-transient, non-fatal — **still
holds, with one addition**: a dropped `AL_CSP_NOTEOFF_EVT` means a note is never
released, so it costs a *stuck note and a leaked voice*, not just silence. That is
worse than a lost MIDI event. The in-browser measurement now exists: these old
sequence-event types have not recurred on current head, every queue is identified
and measured, and any future drop fails the release gate. See
[the resolved queue item](audio.md#fixed-browser-audio-event-queue-drops--measured-in-the-real-runtime).

**The suggested link to the boss cutscene is refuted, by construction.** Boss
cutscene selection and sequencing touch no audio state at all: the channel is chosen
by `cutscene_id_set()` from `level_load()`, and progression is driven by
`Object_AnimatedObject.pauseCounter` / `startDelay` frame counters with
`func_800214C4()` returning `D_8011AD22[1 - D_8011AD21]`, a counter bumped from
`func_8002125C()`. No `AL_*` event feeds any of it. Independently: report 3 does not
reproduce natively either, where there are **0** EVTQ drops.
