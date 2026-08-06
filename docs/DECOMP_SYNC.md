# Keeping the port in sync with the DKR decomp

The port vendored `game/{src,include,libultra}` from the upstream decomp
(`DavidSM64/Diddy-Kong-Racing`) and then layered a lot of `NATIVE_PORT`-gated
work on top: LP64 pointer/struct sizing, asset byteswaps, the frame-pacing and
racer probes, UB fixes, and audio wiring. Upstream keeps advancing — it reached
**100% matching** in July 2026, while this tree's recorded baseline is the
2026-07-25 sync at 99.38%. That progress has to be pulled in **without
clobbering the port patches**.

> ### Resolved 2026-08-07: this tree no longer carries anything off `master`
> Two functions used to be vendored from refs that were not on upstream `master`.
> Both have since landed upstream and the hazard is retired:
>
> * `game/src/objects.c`'s `func_80017A18` was upstream `9da89ecb`, living only on
>   the branch `match-func_80017A18`. It merged as **`db7482cc`** (PR #749). The
>   2026-08-07 sync took master's version wholesale — named parameters and the
>   `for` facet loop — and re-applied the three documented `NATIVE_PORT` deltas
>   (two `DKR_PTR()` reads, the `MDKR_OBJCOLL=legacy` early return). See
>   [OPEN_ITEMS.md wave "objcoll"](open-items/collision.md#fixed-object-model-collision-never-reported-a-hit-so-locked-doors-were-intangible--wave-objcoll).
> * `func_8008FF1C` was the *baseline commit itself*, `3b2dd520` — a local commit
>   on the private fork branch `fork/match-func_8008FF1C`, not an upstream commit
>   at all. It merged as **`8131d0da`** (PR #751).
>
> **The recorded baseline was wrong for the second time in a row**, in the same
> way the 2026-07-24 entry describes: `git merge-base --is-ancestor 3b2dd520
> origin/master` was **false**. It happened to be harmless — base blobs only need
> to equal what was actually vendored, and that fork tree *was* what we vendored —
> but it means the recorded hash was unreachable from any public remote. Run the
> `--is-ancestor` check against `origin/master`, not just against the checkout's
> `HEAD`, before recording a baseline. `HEAD` is whatever branch the decomp
> checkout happens to be sitting on.

> ### Hazard: `sync_decomp.sh` reads "theirs" from the decomp WORKING TREE
> That is deliberate (uncommitted WIP counts), but it means the sync vendors
> whatever branch the checkout is on. `../Diddy-Kong-Racing` sits on local
> branches (`tooling/decomp-workbench-v1` at the time of writing) that **differ
> from `master` in 16 vendored files**, so syncing straight from it would have
> silently vendored local workbench edits as if they were upstream. Point
> `--decomp` at a clean tree checked out at the upstream commit you intend to
> sync — e.g. a throwaway `git clone --shared` of the checkout — and confirm
> `git status --short` there is empty first.

> **Findings to send upstream (open).** `src/hasm/collision.c`
> `compute_grid_overlap_mask()`'s Z loop tests `z2 >= bbox_z1`, which the clamping
> directly above it has already forced, instead of the per-row `z2 >= cell_z` the
> assembly tests (`slt $at, $t5, $t0` at 0x800315D0, with `$t0` initialised to
> `bbox_z1` at 0x800315C4 and advanced by `cell_height` at 0x800315F4). The Z half
> of the returned grid mask therefore never filters anything. Harmless in a
> *matching* build, where that body is never compiled — but a real defect for
> anyone building `NON_MATCHING=1`: the extra candidates saturate
> `generate_collision_candidates()`'s 500-entry list, which truncates, and racers
> fall through the level. **Still present at `c6695703`** (re-checked 2026-08-07;
> upstream's only change to this file since `3b2dd520` was spelling `-1` as
> `VEHICLE_NO_OVERRIDE`). Fixed here in wave "gridmask" — our copy tests the
> per-row `z2 >= zRowNear`; full write-up in `docs/OPEN_ITEMS.md`.
>
> **General lesson for this doc:** the `#ifdef NON_MATCHING` C bodies under
> `game/src/hasm/` are the one class of vendored code upstream never executes, so
> they get no upstream validation at all. When a sync touches one, diff it against
> the `.s` beside it rather than only against the previous C.

## The rule
Never re-copy decomp files over `game/`. Always 3-way merge:

| side | what it is |
|---|---|
| **base**   | the decomp file at the recorded baseline commit (`.decomp-baseline`) |
| **theirs** | the decomp file *now* — working tree, so uncommitted WIP counts |
| **ours**   | our `game/` copy = base + our `NATIVE_PORT` patches |

**Only ever record a real upstream commit as the baseline.** `base` is read with
`git show BASELINE:path`, so anything we vendored that is *not* in that commit —
uncommitted WIP, or a commit on a local branch of the decomp checkout — makes
every base blob wrong, and `git merge-file` then reports conflicts in code that
upstream and we already agree on. Check `git merge-base --is-ancestor BASELINE
HEAD` in the decomp before trusting a sync's conflict list; if it is false, the
recorded hash is a local commit and the base is only approximate. (This bit us
once: the 2026-07-24 baseline was a local WIP commit plus an uncommitted
`racer.c` rework. See the note in `.decomp-baseline`.)

**A new upstream file is not free.** `CMakeLists.txt` globs
`game/src/*.c`, `game/src/hasm/*.c` and `game/src/hasm_native/*.c`, so anything
dropped into those directories is compiled. Decide per file; `src/hasm/libgcc.c`
is the worked example of one to refuse (see the sync log).

## Doing a sync
```bash
tools/sync_decomp.sh --dry-run      # see what would change
tools/sync_decomp.sh                # apply clean merges; leave conflict markers
```
Then:
1. Resolve any conflict markers by hand. Rule of thumb: take **theirs** for the
   decomp logic, keep **ours** for anything inside `#ifdef NATIVE_PORT`, and
   re-check that our patch still makes sense against the new upstream code.
2. Sanity-check the port patches survived, e.g. for `racer.c`:
   `grep -c NATIVE_PORT game/src/racer.c` and `grep -c mdkr_pace_probe_racer …`.
3. Rebuild and run the **muted, headless** validation suite (below).
4. Record the new baseline and commit:
   ```bash
   ( cd ../Diddy-Kong-Racing && git rev-parse HEAD ) > /tmp/h
   printf '%s  # decomp commit synced YYYY-MM-DD\n' "$(cat /tmp/h)" > .decomp-baseline
   ```

## Validation after any sync (all MUTED + HEADLESS)
Never run the binary without `--headless-frames` (that opens a window **and** the
audio device). `MDKR_AUDIO=0` is belt-and-braces (note: `off` is a no-op — the
code tests for `"0"`).

```bash
cmake --build build -j
# race stability
for i in $(seq 1 12); do MDKR_AUDIO=0 ./build/mdkr64 --headless-frames 2900 \
  --input-script tests/input_scripts/race_drive_time_trial.txt --rom baserom.us.v80.z64 \
  >/dev/null 2>&1 || echo CRASH; done
# racer physics unchanged (expect ~12 units/frame, clock advancing)
MDKR_AUDIO=0 MDKR_TRACE=1 ./build/mdkr64 --headless-frames 3200 \
  --input-script tests/input_scripts/race_drive_time_trial.txt --rom baserom.us.v80.z64 2>&1 \
  | grep -oE 'z=[-0-9.]+ clock=[0-9]+' | tail -4
# menus + idle-attract soak
for f in nav_to_options nav_to_character_select nav_to_time_trial_race; do
  MDKR_AUDIO=0 ./build/mdkr64 --headless-frames 2900 --input-script tests/input_scripts/$f.txt \
    --rom baserom.us.v80.z64 >/dev/null 2>&1 && echo "$f ok" || echo "$f FAIL"; done
MDKR_AUDIO=0 ./build/mdkr64 --headless-frames 12000 --rom baserom.us.v80.z64 >/dev/null 2>&1; echo "soak rc=$?"
```
A *matching* refactor upstream (same semantics, different codegen so it matches
the original asm) should be **behaviour-neutral** — the racer probe numbers
should be byte-identical before and after. If they move, investigate before
committing.

For a *rendered* A/B (the right evidence when the refactor is in menu or render
code, where the racer probe says nothing), save the pre-merge binary first, then
compare sampled frames:

```bash
git stash push -u && cmake --build build -j && cp build/mdkr64 /tmp/mdkr64_pre
git stash pop     && cmake --build build -j
for d in pre post; do mkdir -p /tmp/f_$d; done
MDKR_AUDIO=0 MDKR_DUMP_EVERY=100 /tmp/mdkr64_pre  --headless-frames 2300 \
  --input-script tests/input_scripts/nav_to_track_select.txt \
  --rom baserom.us.v80.z64 --dump-frames /tmp/f_pre  >/dev/null 2>&1
MDKR_AUDIO=0 MDKR_DUMP_EVERY=100 ./build/mdkr64    --headless-frames 2300 \
  --input-script tests/input_scripts/nav_to_track_select.txt \
  --rom baserom.us.v80.z64 --dump-frames /tmp/f_post >/dev/null 2>&1
diff -rq /tmp/f_pre /tmp/f_post
```

**`MDKR_DUMP_EVERY` is not an optimisation, it is the difference between a valid
and an invalid experiment.** Dumping *every* frame writes ~3.6 MB per frame, and
the pacer derives `updateRate` from the wall clock — so the I/O changes the
simulation. A 2300-frame unstrided dump produced 2300 frames on one run and 1853
truncated ones on the next *from the same source*, which reads exactly like a
behaviour regression and is not one. With a stride of 100 the same route is
byte-reproducible run to run (verify that first, on the pre binary, before
believing any pre/post difference).

## Sync log
| date | decomp commit | what came in | notes |
|---|---|---|---|
| 2026-07-24 | `527889b5` (+ working-tree `racer.c`) | `func_80049794` matching rework | Clean 3-way merge, no conflicts; all 7 `NATIVE_PORT` gates + probe preserved; racer probe byte-identical before/after (behaviour-neutral). `trackbg_render_flashy` (upstream PR #744) was already vendored — it was on the local branch at first vendor. Upstream score 97.91% → **98.19%**. |
| 2026-07-25 | `3b2dd520` | PR #746 (GCC build fix + `waves.c` UB data), #747 (`func_80049794` matched), `func_8008FF1C` matched | 2 conflicts, both 1-line resolutions. **`racer.c`**: upstream's now-matching `func_80049794` still declares `f32 sp60[4]` with the same "Should be MtxF, but produces a worse score" comment, so our `MtxF sp60` + `SP60_ROWS` fix **stands**; the whole `527889b5..3b2dd520` delta for that function was already vendored except one stale comment (the working-tree rework *was* #747). **`waves.c`**: upstream flattened the bogus `gWaveVertices[4][1]`/`gWaveTriangles[4][1]` to `[4]` (removing out-of-bounds inner indexing, behaviour-identical: old `[a][b]` == flat `a+b`); it did **not** touch `D_8012A5E8[2]`/`D_8012A600[24]`, which are still two separate objects, so our union + 2 `_Static_assert`s **stands** and `check_wave_visible_table` still passes. Took upstream's `#ifndef NON_MATCHING` around `unused_string.c`'s `memset`, which removes a byte-at-a-time `memset` our build was exporting over libc's. Deleted the never-assembled `src/hasm/llmuldiv_gcc.s`; **refused** the new `src/hasm/libgcc.c` (inline MIPS `ddiv`/`dremu` asm + `memset`/`memcmp`/`memmove` redefinitions; `game/src/hasm/*.c` is globbed, so vendoring it would break the build). Racer probe byte-identical; `nav_to_track_select` and `race_drive_long` frames byte-identical pre/post. Upstream score 97.91% → **99.38%**. |
| 2026-08-07 | `c6695703` | 12 upstream commits (PRs #749, #751, #753, #755, #756, #757, #758, #759, #760, #764, #766). 17 vendored files: 8 clean merges, 9 conflicts. | Upstream reached **100%**. **Both carried exceptions retired** — `func_80017A18` merged as `db7482cc` (#749) and `func_8008FF1C` (which *was* the old baseline, a private-fork commit) merged as `8131d0da` (#751); the tree now vendors only `master`. **Three port patches became redundant and were dropped in favour of upstream's own fix**: `racer.c` `func_8005B818`'s spline arrays (our `DKR_SPLINE_CTRL_N` 4→5 override; upstream's rematch #764 declares `[5]`), `tracks.c` `func_80026E54`'s `sp94` (our `f32 sp94[2*10]`; upstream now declares `sp94[20]`), and `waves.c`'s shift (upstream adopted the `unk2 - 1` spelling our comment had derived — `DKR_SHL32` **stays**, because `unk2` is `u8` so a bare shift is still UB ≥ 32; `(unk2+0x1F)&31 == (unk2-1)&31`, byte-identical). Kept ours: `printf.c`'s `DKR_VSPRINTF` rename (took upstream's new doc comment), `video.c`'s `NATIVE_PORT` pacing block (took upstream's `LOGIC_30FPS` constant in the vanilla arm), `collision.c`'s `DKR_PTR` (took upstream's `VEHICLE_NO_OVERRIDE`, `== -1`), `racer.c`'s `MtxF sp60`/`SP60_ROWS`, `unused_string.c`'s outer `NON_MATCHING` guard (nested upstream's new `#ifndef __GNUC__` inside it). **Refused** `src/gcc/libgcc.c` again — upstream merely moved it out of `src/hasm/`, and we never vendored it. Racer probe byte-identical pre/post over 3200 frames; the only residual trace deltas are renderer/texture counters and `dtms=`, and a same-binary control run differs by *more* lines than pre/post, so they are run-to-run noise. |
