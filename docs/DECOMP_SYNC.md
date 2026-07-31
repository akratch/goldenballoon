# Keeping the port in sync with the DKR decomp

The port vendored `game/{src,include,libultra}` from the upstream decomp
(`DavidSM64/Diddy-Kong-Racing`) and then layered a lot of `NATIVE_PORT`-gated
work on top: LP64 pointer/struct sizing, asset byteswaps, the frame-pacing and
racer probes, UB fixes, and audio wiring. Upstream keeps advancing — it reached
**100% matching** in July 2026, while this tree's recorded baseline is the
2026-07-25 sync at 99.38%. That progress has to be pulled in **without
clobbering the port patches**.

> ### Hazard: this tree carries one commit that is NOT on upstream `master`
> `game/src/objects.c`'s `func_80017A18` is upstream commit **`9da89ecb`**, which
> lives only on the decomp branch **`match-func_80017A18`**. It forks from
> `851b15dd`, which is **seven commits behind** our recorded baseline
> `3b2dd520`, and `git merge-base --is-ancestor 3b2dd520 9da89ecb` is **false**.
>
> Two consequences, and the second one bites:
> 1. A 3-way merge for that file cannot use `3b2dd520` as base and cannot use
>    `851b15dd` either (both upstream lines edited the same function). It was
>    resolved by *adoption*: our copy of `func_80017A18` was verified
>    byte-identical to `3b2dd520`'s, i.e. carrying no port patches, so the matched
>    body replaced it wholesale. Re-verify that equality before redoing this.
> 2. **A future sync from `master` will look like it wants to revert us**, because
>    `master` still has the `NON_EQUIVALENT` guard and the `GLOBAL_ASM` pragma. Do
>    not take it. Once the branch merges upstream, this note can go — until then,
>    treat `func_80017A18` as ours-newer-than-theirs. See
>    [OPEN_ITEMS.md wave "objcoll"](open-items/collision.md#fixed-object-model-collision-never-reported-a-hit-so-locked-doors-were-intangible--wave-objcoll).

> **Findings to send upstream (open).** `src/hasm/collision.c`
> `compute_grid_overlap_mask()`'s Z loop tests `z2 >= bbox_z1`, which the clamping
> directly above it has already forced, instead of the per-row `z2 >= cell_z` the
> assembly tests (`slt $at, $t5, $t0` at 0x800315D0, with `$t0` initialised to
> `bbox_z1` at 0x800315C4 and advanced by `cell_height` at 0x800315F4). The Z half
> of the returned grid mask therefore never filters anything. Harmless in a
> *matching* build, where that body is never compiled — but a real defect for
> anyone building `NON_MATCHING=1`: the extra candidates saturate
> `generate_collision_candidates()`'s 500-entry list, which truncates, and racers
> fall through the level. Still present at `3b2dd520`. Fixed here in wave
> "gridmask"; full write-up in `docs/OPEN_ITEMS.md`.
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
