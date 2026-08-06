# Test suite economics

The complete suite is 135 tasks and takes between 2¼ and 3¼ hours. This document
measures where that time goes, names the overlaps that could be collapsed, and
records what evidence a future implementer must produce before collapsing them.

**Nothing here removes or weakens a gate.** No task was deprecated, merged, or
subsetted in the pass that produced this document. Every verdict below is a
*candidate* with a stated precondition. The release gate remains the complete
run: `python3 tools/run_checks.py` with no restriction flags, printing
`complete suite, 135/135 tasks`. See
[`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md) §2b.

---

## 1. Method, and what these two runs can and cannot show

Two complete-suite logs, same 135-task manifest
([`../tools/run_checks.py`](../tools/run_checks.py)):

| | run 1 | run 2 |
|---|---|---|
| log | `/tmp/m9-full-suite.log` | `/tmp/final-suite.log` |
| wall clock | 193m53s | 135m07s |
| verdict | `FAIL — 5/135` | `FAIL — 2/135` |
| finished | 2026-08-05 22:58 | 2026-08-06 01:58 |
| host | loaded (concurrent work) | quiet |

**The runner accounts for its own time.** Summing every per-task duration gives
193m50s and 135m06s — within 3s and 1s of the two wall-clock totals. The runner
is sequential by construction ("It runs sequentially because several checks
intentionally create and remove `save/eeprom.bin`",
[`../tools/run_checks.py`](../tools/run_checks.py) lines 14–16), so there is no
hidden concurrency and no per-task overhead worth modelling. Task time *is*
suite time.

### Two confounds that limit what the delta means

**The two runs are not the same tree.** Run 2 ran at `57154d6`; run 1 predates
`0c6e433` (its `camera_obstruction_runtime` banner lacks the unset-default
clause that commit added). Between those trees, exactly these files changed
under `tests/`, `tools/`, `game/` and `platform/`:

```
game/src/camera_obstruction_runtime.c   tests/check_camera_obstruction_runtime.py
tests/check_asset_swap_invariants.py    tests/check_taj_character_select.py
tests/check_authored_rng_compat.py      tools/run_checks.py
platform/fast3d/PROVENANCE.md           tools/web/build_web.sh
```

`check_taj_character_select.py` alone changed by 197 lines. Every other check
script is byte-identical between the runs, so for every task except the seven
touched by that list, the run-1/run-2 delta is host load and the camera-default
engine change — not a rewritten gate.

**A failed task did not necessarily run to completion.** Its duration is then
not a measurement of the gate's cost:

- `browser_taj_character_select` — run 1, `FAIL` in **0s**. Not a measurement
  at all; the staging fault fixed in `143f1f6` aborted it before it started.
- `taj_character_select_ultrawide` — run 1, `FAIL` in 1m49s, against the
  pre-rewrite gate. Not comparable to run 2's 22s.
- `arbitrary_presentation_rates` — run 2, `FAIL` in 56s. This one **is**
  comparable: the script accumulates failures into a list and only aborts on a
  `RuntimeError`; the two reported failures are content assertions, so all 21
  arms ran (`tests/check_arbitrary_presentation_rates.py` lines 556–580).
- `browser_runtime` — run 1, `FAIL` in 2m03s on a cadence budget, versus 3m59s
  passing. The failing run stopped short.
- `app_adopted_pacing` — run 2, `FAIL` in 23s versus 13s passing.

Every table below is annotated accordingly. No cost or ratio claim in this
document rests on a row marked `FAILED` or `changed`.

---

## 2. The measured cost table

Every task that cost ≥60s in either run. 41 of 135 tasks qualify.

| task | role | run 1 | run 2 | run2/run1 | note |
|---|---|---|---|---|---|
| `native_layout` | layout | 37m09s | 36m47s | 0.99 | flat |
| `framed_world_views` | native | 17m56s | 7m51s | 0.44 | **halved** |
| `presentation_breadth` | release | 16m38s | 6m31s | 0.39 | **halved** |
| `presentation_matrix` | release | 11m10s | 3m05s | 0.28 | **halved** |
| `camera_obstruction_display_matrix` | release | 7m16s | 4m15s | 0.58 | |
| `full_ubsan` | instrumented | 7m04s | 7m25s | 1.05 | flat |
| `presentation_shadows` | release | 4m11s | 1m54s | 0.45 | **halved** |
| `browser_runtime` | browser | 2m03s | 3m59s | — | run 1 FAILED early |
| `2p_human_binding` | native | 3m34s | 1m03s | 0.29 | **halved** |
| `taj_character_select` | native | 3m34s | 43s | — | script rewritten between runs |
| `presentation_lifecycle` | release | 3m33s | 2m35s | 0.73 | |
| `arbitrary_presentation_rates` | release | 3m32s | 56s | 0.26 | run 2 FAILED but ran all 21 arms |
| `browser_taj_character_select` | browser | 0s | 3m27s | — | run 1 FAILED before starting |
| `resource_plateau` | native | 3m21s | 1m16s | 0.38 | **halved** |
| `browser_taj_persistence` | browser | 3m11s | 3m11s | 1.00 | flat (identical to the second) |
| `render_purity` | release | 2m51s | 55s | 0.32 | **halved** |
| `taj_results_portrait` | native | 2m46s | 32s | 0.19 | **halved** |
| `state_hash` | release | 2m42s | 2m26s | 0.90 | |
| `array_bounds_sweep` | instrumented | 2m24s | 2m25s | 1.01 | flat |
| `world_fx_matrix` | native | 2m18s | 1m36s | 0.70 | |
| `taj_character_select_pal` | native | 2m18s | 28s | — | script rewritten between runs |
| `remaster_lighting` | native | 2m13s | 1m05s | 0.49 | **halved** |
| `webgpu_content_census` | native | 1m28s | 2m09s | 1.47 | got *slower* on the quiet host |
| `camera_obstruction_runtime` | release | 2m02s | 1m15s | — | script + engine changed between runs |
| `shadow_plausibility` | native | 2m02s | 1m40s | 0.82 | |
| `browser_resource_plateau` | browser | 2m01s | 2m02s | 1.01 | flat |
| `native_ui_resolution` | native | 2m01s | 1m05s | 0.54 | |
| `challenge_modes` | native | 2m00s | 50s | 0.42 | **halved** |
| `taj_character_select_ultrawide` | native | 1m49s | 22s | — | FAILED run 1; script rewritten |
| `camera_obstruction_lifecycle_runtime` | release | 1m43s | 1m30s | 0.87 | |
| `bluey2_rematch` | native | 1m37s | 26s | 0.27 | **halved** |
| `taj_vehicle_sweep` | native | 1m35s | 1m46s | 1.12 | flat |
| `camera_obstruction_performance_runtime` | release | 1m31s | 1m19s | 0.87 | |
| `door_glyphs` | native | 1m29s | 20s | 0.22 | **halved** |
| `vehicle_sweep` | native | 1m29s | 1m39s | 1.11 | flat |
| `taj_hud_portrait` | native | 1m27s | 18s | 0.21 | **halved** |
| `texture_lineswap` | native | 1m14s | 21s | 0.28 | **halved** |
| `world_fx_capture` | native | 1m06s | 36s | 0.55 | |
| `taj_challenges` | native | 1m06s | 1m29s | 1.35 | got *slower* on the quiet host |
| `overlay_pause` | native | 1m05s | 4s | 0.06 | **halved** |
| `collision_headroom` | native | 1m03s | 1m13s | 1.16 | flat |

### Where the time is concentrated

| | run 1 | run 2 |
|---|---|---|
| top 5 tasks | 90m09s (46.5%) | 62m49s (46.5%) |
| top 10 tasks | 112m05s (57.8%) | 79m06s (58.5%) |
| top 20 tasks | 139m41s (72.1%) | 98m13s (72.7%) |
| the 94 tasks under 60s in both runs | — | 20m17s (15.0%) |

The concentration is identical to a tenth of a percent across a loaded and a
quiet host. **Deprecating cheap gates is not an optimisation.** Deleting all 94
sub-minute tasks — 70% of the manifest, including every source-role audit and
every mutation control that costs nothing — would buy 20 minutes and cost the
suite most of its coverage.

By manifest role:

| role | tasks | run 1 | run 2 | share of run 2 |
|---|---|---|---|---|
| `native` | 86 | 74m37s | 41m59s | 31.1% |
| `layout` | 1 | 37m09s | 36m47s | 27.2% |
| `release` | 23 | 61m56s | 30m42s | 22.7% |
| `browser` | 6 | 7m32s | 12m56s | 9.6% |
| `instrumented` | 2 | 9m28s | 9m50s | 7.3% |
| `asan` | 5 | 2m09s | 1m51s | 1.4% |
| `ctest` | 1 | 45s | 48s | 0.6% |
| `source` | 7 | 11s | 10s | 0.1% |
| `browser_save`, `rom`, `wasm` | 4 | 3s | 3s | 0.0% |

One task in one role is 27% of a quiet release run.

---

## 3. Load sensitivity, and what a flat task is telling you

Restricting to tasks that passed in **both** runs with an **unchanged** script —
the only rows where the delta is attributable to host conditions:

**Halved or better on the quiet host** (ratio ≤ 0.50): `overlay_pause` (0.06),
`taj_results_portrait` (0.19), `taj_hud_portrait` (0.21), `door_glyphs` (0.22),
`bluey2_rematch` (0.27), `presentation_matrix` (0.28), `texture_lineswap`
(0.28), `2p_human_binding` (0.29), `render_purity` (0.32), `resource_plateau`
(0.38), `presentation_breadth` (0.39), `challenge_modes` (0.42),
`framed_world_views` (0.44), `presentation_shadows` (0.45), `remaster_lighting`
(0.49).

These are all the same shape: a Python driver that spawns many short-to-medium
headless engine processes back to back. Under contention each spawn pays for the
whole machine's scheduling; on a quiet host it does not. `overlay_pause` at 0.06
(1m05s → 4s) is the extreme and deserves its own look — a 16× spread on an
unchanged script is larger than contention alone comfortably explains.

**Flat regardless of load** (ratio ≥ 0.95). These matter more than the halving
list, because a flat task is one whose cost is *not* contention, and therefore
one that cannot be made faster by running the suite on a quieter box:

- `native_layout` (0.99), `full_ubsan` (1.05), `array_bounds_sweep` (1.01) —
  compute-bound instrumented replays on warm build trees. See §6.
- `browser_taj_persistence` (**exactly** 3m11s in both runs),
  `browser_resource_plateau` (2m01s / 2m02s) — these are **real-time bound**.
  The browser gates poll rather than sleep (`check_browser_taj_persistence.py`
  lines 61–66 is an 80 ms poll loop, not a fixed wait), but the page advances at
  the display's rAF cadence, so N frames take N ÷ rate seconds of wall clock no
  matter how fast the host is. `browser_runtime`'s own pass line proves the
  arithmetic: "3600 wasm/WebGPU host opportunities … median 30.0 fps" — 3600
  frames at 30 fps is 120s of an observed 3m59s task. The browser lane's 12m56s
  has a hard real-time floor and is not an optimisation target.
- `vehicle_sweep` (1.11), `taj_vehicle_sweep` (1.12), `collision_headroom`
  (1.16), `taj_challenges` (1.35), `webgpu_content_census` (1.47) — *slower* on
  the quiet host. The camera-obstruction default flipped from legacy to modern
  in `0c6e433` between the runs, which adds real per-tick resolver work to every
  route these tasks drive. That is the honest reading available from these two
  logs; a clean before/after on one tree would confirm it.

---

## 4. Overlap analysis

Each cluster below states what each gate asserts that its siblings do not,
then a verdict:

- **MERGE CANDIDATE** — same binary route, disjoint assertions, could run in one
  pass.
- **REDUNDANT** — one gate's assertions are a strict subset of another's, proven.
- **KEEP-SEPARATE** — different build role, different failure isolation, or a
  control that must stay independent.

### 4.1 `world_fx_capture` / `world_fx_matrix` / `world_shadows` — KEEP-SEPARATE

Combined run-2 cost 2m52s. All three drive the native binary on both `gl` and
`webgpu`, and two of them share a route exactly: `nav_to_time_trial_race.txt`,
3500 frames, capture at 3499, `MDKR_LOAD_TRACK=5`. That is where the similarity
ends.

| | `world_fx_capture` | `world_fx_matrix` | `world_shadows` |
|---|---|---|---|
| scenes | 1 (track 5, 1P) | 8 (5 tracks + hub + 2P + 4P) | 1 (track 5, 1P) |
| `MDKR_WORLD_SHADOW` | pinned `0` | pinned `0` | the variable under test (`0`/`1`/fault) |
| telemetry read | `[WORLD-FX]`, `[SHADOW-PLAN]` | `[WORLD-FX]`, `[SHADOW-PLAN]` | `[DEPTH]`, `[WORLD-SHADOW]` |
| frames dumped | yes | **no** | yes |
| cadence | default | default | `enhanced` + `MDKR_SYNTH_FIELDS=1` |

Unique to each, with nothing equivalent in the siblings:

- `world_fx_capture` — an **exact** pinned caster census
  (`triangles=1199, ranges=130, static=541, dynamic=658, matrices=18`), the
  material-class census (`opaque > 1_000_000`, `masked > 0`), and the
  `MDKR_WORLD_FX_MATRIX_CONTROL=drop` mutation control that must collapse that
  census to zero *while leaving the rendered frame byte-identical*. That last
  pairing is the whole point of the gate: the instrument is provably inert.
- `world_fx_matrix` — the only gate that reaches the 1024-pixel cascade tiers,
  because it is the only one that runs 2P and 4P; and the only one asserting
  GL/WebGPU telemetry-tuple equality. Its `[WORLD-FX]` bounds are deliberately
  weaker (`> 0`, not exact) because they must hold across eight scenes.
- `world_shadows` — decal-fallback truthfulness
  (`on.decal < off.decal * 9/10`), the four-way darkening classification, the
  backend self-naming check, and the `MDKR_TEST_WORLD_SHADOW_RESOURCE_FAIL`
  fault arm that must latch at exactly 3 failures and reproduce the legacy frame
  byte-for-byte.

**Verdict: KEEP-SEPARATE.** `world_fx_capture` and `world_shadows` cannot share a
process even on their shared route, because the shared route is precisely where
they disagree: one requires shadows off for the whole run, the other toggles
them. `world_fx_matrix` shares no route with either. Combined saving from any
merge is under a minute and would cost three separate failure-isolation domains.

`shadow_plausibility` (1m40s) is often assumed to overlap `world_shadows`. It
does not: it is the only gate reading `staleCasters` / `staleWorst` /
`implausible` / `invalidWorldRecv`, backed by its own
`MDKR_TEST_SHADOW_BOGUS_CASTER=900.0` positive control. `world_shadows` never
parses `[WORLD-FX]` at all and asserts nothing about caster provenance.

### 4.2 The camera runtime family — one MERGE CANDIDATE, the rest KEEP-SEPARATE

Nine `release`-role tasks, 11m03s combined in run 2.

**Correct the premise first.** There is no shared 5200-frame route across this
family. Grepping the whole family for `5200` returns exactly two hits:

```
tests/check_camera_obstruction_runtime.py:158:    parser.add_argument("--frames", type=int, default=5200)
tests/check_camera_emergency_readability_runtime.py:18:  parser.add_argument("--frames", type=int, default=5200)
```

and the second imports `run`/`inspect`/`field`/`DETAIL` from the first. The
actual route matrix:

| task | frames | route | players |
|---|---|---|---|
| `camera_obstruction_runtime` | 5200 | `race_drive_long.txt` | 1P |
| `camera_emergency_readability_runtime` | 5200 | `race_drive_long.txt` | 1P |
| `camera_obstruction_display_matrix` | 3600 | `race_drive_long.txt` | 1P |
| `camera_projection_fallback_runtime` | 3400 | `race_drive_long.txt` | 1P |
| `camera_3p_tt_runtime` | 7000 | `race_3p_tt_camera.txt` | 3P |
| `camera_dynamic_obstruction_runtime` | 9000 | `adventure_hub_drive.txt` | 1P |
| `camera_obstruction_performance_runtime` | 12500 | `race_4p_split.txt` | 4P |
| `camera_obstruction_lifecycle_runtime` | per-scenario | 3 lifecycle scripts | 1P/2P |
| `camera_snapshot_coverage` | 6400 / 9200 presents | 3 presentation routes | 2P/3P/1P |

`camera_obstruction_lifecycle_runtime`'s `pause-restart` scenario also carries
the number 5200, but those are ticks from `check_presentation_lifecycle.SCENARIOS`
driving `race_pause_restart.txt` — a different subsystem, a different script, no
shared code path. It is a coincidence, not a shared route.

**MERGE CANDIDATE — `camera_obstruction_runtime`'s modern arm into
`camera_obstruction_display_matrix`.** These are literally the same command.
`display_matrix` imports `run()` from `obstruction_runtime` and its
`4:3-high` × `authored` cell is `run(binary, rom, "modern", frames, timeout,
window="1280x960", fov="authored")` — byte-identical to `obstruction_runtime`'s
modern arm apart from `--frames` (3600 vs 5200). Raising that one cell to 5200
frames would let it emit the modern-arm evidence `obstruction_runtime` reports
today. Estimated saving: one 5200-frame 1P run, roughly 20s quiet.

**KEEP-SEPARATE, with the mechanism that forbids merging:**

- `camera_obstruction_runtime`'s `legacy` / `center-ray` / `unset` arms.
  `MDKR_CAMERA_OBSTRUCTION` selects one policy per process. Four policies is
  four processes; there is no version of this that is one pass. The `legacy` and
  `center-ray` arms are **positive controls** that must reproduce lens
  penetration, and `unset` must reproduce `modern`'s counters exactly — the
  assertion that the shipped default is the tested default.
- `camera_projection_fallback_runtime` injects
  `MDKR_TEST_CAMERA_PROJECTION_FAIL_TICK=800`. Merging it into any clean-run
  gate would break that gate's "zero penetrated/degraded/invalid on every tick"
  assertion by design, at tick 800.
- `camera_emergency_readability_runtime` injects
  `MDKR_TEST_CAMERA_DISABLE_ALTERNATE=1`, removing an evasion path other gates
  rely on being available. Same conflict. It shares `obstruction_runtime`'s
  route and frame count and *still* cannot share its process.
- `camera_obstruction_performance_runtime` measures p50/p95/p99 off an
  allocation-free clock under `MDKR_CAMERA_PERF=1`. Every other gate in the
  family sets `MDKR_CAMERA_TRACE=2`, which formats a text row every tick. A
  merged process would perturb the exact percentiles it gates. It also asserts
  byte-identical `[SIMHASH]` streams between `observe` and `modern`, which
  requires two processes anyway.
- `camera_obstruction_lifecycle_runtime` needs process- and level-transition
  seams (quit-to-menu, pause/restart, post-race reload). A single continuous
  headless run cannot produce them.
- `camera_dynamic_obstruction_runtime`, `camera_3p_tt_runtime`,
  `camera_snapshot_coverage` — different routes, different player counts, and in
  the last case a different renderer and `--dump-frames`. A different route is a
  different process regardless of what is measured.

The three `source`-role camera tasks (`camera_track_occlusion_cache`,
`camera_object_occlusion_cache`, `camera_dynamic_occlusion`) launch no engine and
hold no ROM; all three report 0s. They are free and stay.

### 4.3 The presentation family — deliberate duplication, KEEP-SEPARATE

`presentation_breadth` (6m31s), `presentation_matrix` (3m05s) and
`arbitrary_presentation_rates` (56s) do overlap on content and rate. They do not
overlap on assertions, and the overlap is documented in the source as
intentional.

| | rate arms | content | workload | asserts |
|---|---|---|---|---|
| `arbitrary_presentation_rates` | original/30/60/90/120/144/165/240/uncapped + display + PAL 25/60 — 21 arms | track 5 only, NTSC + PAL | 21 × 600 ticks = 12,600 | state + event + input + **PCM** byte-identity, exact rational present counts |
| `presentation_matrix` | unset / 30 / 60 only | track 5 + track 26 for particles | ≈91,840 frames over 18 runs | state + event + input + PCM, **plus pixels** (the only one) |
| `presentation_breadth` | 60 (NTSC) / 50 (PAL) only | 17 arms: car/hover/plane, 3 bosses, 4 challenge types, 1P–4P, NTSC + PAL | ≈232,800 frames over 36 runs | **`[SIMHASH]` only** — no event, no input, no PCM — plus presentation-quality telemetry |

The content collisions and why each is not a duplicate:

- `breadth`'s `ancient-lake-car` reuses the matrix gate's baseline route. The
  source says why: *"'standard AI races' + the reference the matrix gate already
  runs, kept so a divergence between the two gates is attributable."* Breadth
  asserts SIMHASH-only there; matrix asserts SIMHASH + EVENTHASH + INPUTHASH +
  PCM + exact packet counters. Same fixture, strictly different question.
- `breadth`'s `challenge-battle-1` (track 26, rate 60) collides with `matrix`'s
  particle arms (track 26, rate 60). Matrix's exist to run a **pixel diff**
  between particle interpolation on and off. Breadth never dumps a frame.
- `breadth`'s three PAL arms collide with `arbitrary_rates`' two PAL arms only
  on track 5. `pal-boss` (38) and `pal-battle` (26) have no counterpart there,
  because `arbitrary_rates` drives track 5 exclusively.

Neither `presentation_breadth` nor `arbitrary_presentation_rates` will silently
skip its region arm — both hard-fail if no PAL ROM is found under `--roms`,
which is why `tools/run_checks.py` lines 646–655 pass `--roms` to them. The PAL
half of breadth is 3 of 17 arms and ≈5% of its frames; PAL is not why breadth is
expensive.

**Verdict: KEEP-SEPARATE on assertions. MERGE CANDIDATE on process count** — see
§6.3. The cost here is 36 + 18 sequential engine spawns, not duplicated
assertions.

`presentation_shadows`, `presentation_lifecycle`, `fixed_tick_schedules`,
`hud_render_authority` and `render_purity` are disjoint by construction: a
shadow-replay rigidity measurement, a lifecycle-transition invariance gate, a
scheduler-authority gate, a HUD/RNG/audio invariance gate, and a skip-render
invariance gate with its own divergence control.

### 4.4 The Taj select family — MERGE CANDIDATE, small payoff

Four manifest tasks run the **same script** with different arguments:

| task | args | spawns | run 1 | run 2 |
|---|---|---|---|---|
| `taj_character_select` | (none) | 9 | 3m34s | 43s |
| `taj_character_select_webgpu` | `--renderer webgpu` | 9 | 11s | 11s |
| `taj_character_select_ultrawide` | `--layout base --aspect 21:9` | 5 | 1m49s | 22s |
| `taj_character_select_pal` | `--layout base --require-pal` | 6 | 2m18s | 28s |

What each argument actually changes:

- `--layout` filters the four retail layouts (`base` 8, `drumstick` 9, `tt` 9,
  `complete` 10). Default runs all four; `--layout base` runs one. So the
  ultrawide and PAL tasks each re-run the `base` layout the default task already
  covers, and neither touches `drumstick`, `tt` or `complete`.
- `--aspect` is the only argument that changes test-side logic: it switches
  `FrameGeometry.for_frame` from an identity scale to the centred-fit
  letterbox/pillarbox math. That math is exercised **only** by the ultrawide arm.
- `--renderer` sets `MDKR_RENDERER` and nothing else. No Python assertion,
  threshold or geometry branches on it.
- `--require-pal` swaps which ROM file boots. No pixel box, threshold or
  geometry is PAL-specific in this file.

There is no expensive Python-side setup to amortise. Cost is per-spawn: 29
engine boots across the four tasks, each running 2250 frames of which the first
~1250 are title-screen navigation before anything under test happens. Merging
the four into one process that loops `(rom × renderer × aspect × layout)` is
mechanically trivial but saves only the Python interpreter starts — the boot
cost is per distinct combination and is unavoidable.

**Verdict: MERGE CANDIDATE for clarity, not for cost.** Estimated saving ~1m on
a quiet host. It was ~7m on the loaded one, which is a fact about the host, not
about the gate.

**One coverage finding falls out of this analysis and is worth more than the
minute.** The oversized-shadow negative control is guarded by
`if layout.name == "base" and args.aspect is None`. Because the ultrawide task
sets `--aspect`, it *skips* that control entirely — it runs strictly less than
the default task's own `base` iteration, plus the letterbox math. That is a gap
to close, not a cost to cut, and it is filed here because the cost analysis is
what surfaced it.

### 4.5 `vehicle_sweep` vs `taj_vehicle_sweep` — KEEP-SEPARATE, and one dead argument

Both run `check_vehicle_sweep.py` over the same ROM-derived 47 legal
track/vehicle pairs. `--taj` adds `MDKR_TAJ_TEST_PLAYER=0`,
`MDKR_TAJ_PHYSICS_TRACE=1`, `MDKR_TAJ_VISUAL_TRACE=1`, `MDKR_FORCE_SHIELD` and a
car-only dash trigger, then requires five extra trace markers on top of every
ordinary per-combo assertion.

The *assertion code* is a superset. The *subject* is not: with
`MDKR_TAJ_TEST_PLAYER=0`, player 0 is Taj, so the base assertions are evaluated
against a Taj-substituted racer, never against the ordinary one. **KEEP-SEPARATE.**

Separately: `tools/run_checks.py` passes `("--taj", "--frames", "5200")`, but
`check_vehicle_sweep.py` line 288 already declares `default=5200`. The explicit
`--frames 5200` changes nothing and can be dropped as noise. Zero seconds saved;
it is listed so the next reader does not assume the Taj arm runs a longer route
than the plain one. It does not.

### 4.6 The build-role duplicates — KEEP-SEPARATE, and not worth touching

Six scripts appear more than once under different build roles: `door_blocks`
(native / release / asan), `raw16_audio` (native / release / asan),
`filename_entry` (native / asan), `widescreen_shadow` (native / asan),
`presentation_lifecycle` (release, full; asan, `--only pause-quit`),
`check_vehicle_sweep` (§4.5).

These look like the most obvious redundancy in the manifest and are the least
worth removing. Each pair proves something the other cannot:
`key_cutscene_once` exists because a "show this once" latch vanishes entirely at
`-O2`; the ASan arms exist because a use-after-free is invisible to a
non-instrumented binary. `presentation_lifecycle_asan` deliberately runs a
strict subset arm (`--only pause-quit`) under ASan — the subset is the point,
because it is the arm with the retained-replay teardown.

Total run-2 cost of every duplicate arm: `door_blocks_release` 8s +
`raw16_audio_release` 4s + `door_blocks_asan` 20s + `raw16_audio_asan` 9s +
`filename_entry_asan` 6s + `widescreen_shadow_asan` 21s +
`presentation_lifecycle_asan` 55s = **2m03s**, 1.5% of the run.

**Verdict: KEEP-SEPARATE.** Different build role is exactly the "different
failure isolation" case, and the entire cluster is under two minutes.

### 4.7 Inside `native_layout`: `track_sweep` ⊂ `vehicle_sweep` — REDUNDANT candidate

`check_native_layout.py` runs a 14-entry `RUNTIME_CHECKS` matrix, each entry an
entire other check re-run against the alignment build. Two of those entries are
`check_track_sweep.py` and `check_vehicle_sweep.py`. From the run-2 log:

```
check_track_sweep:   20 track(s), 5200 frames each, autopilot, vehicle=track default
check_vehicle_sweep: 47 combination(s) over 20 track(s), 5200 frames each, autopilot
```

Same 20 tracks, same 5200 frames, same autopilot, and the 47 legal combinations
include each track's default vehicle. For the question this matrix asks — *does
any route trip `-fsanitize=alignment`* — the track sweep contributes no route
the vehicle sweep does not already drive. Its 20 runs are a strict subset of the
vehicle sweep's 47.

**Verdict: REDUNDANT within the alignment matrix, subject to one check.**
`check_track_sweep.py` makes its own assertion the vehicle sweep does not
(`animation targets initialized=1784, invalid=0`). That assertion is already
made by the standalone `track_sweep` task against `build-rel`. The evidence a
future implementer must produce is: that no assertion unique to
`check_track_sweep.py` is build-type-sensitive, i.e. that running it only on
`build-rel` and not on `build-align` loses nothing. Estimated saving: 20 of 67
sweep runs at the alignment build's measured cost, roughly **3m** today.

---

## 5. Precedent: this cycle has already deleted proven-redundant assertions

Removing an assertion is allowed here, and has been done, when the redundancy is
*proven in the source* rather than asserted. Two cases from this cycle set the
bar.

**The crest factor.** `check_audio_level_reference.py` measures whole-capture
RMS and sample peak. Crest is peak minus RMS. Bounding crest against a baseline
derived from the same capture, with both tolerances at ±1.0 dB, restates the RMS
assertion and can only fail when the peak moves — so the peak is asserted
directly and crest is reported but not bounded:

```python
# check_audio_level_reference.py, assert_crest()
# crest = dbfs(peak) - dbfs(rms), and assert_level already pins dbfs(rms)
# to +-RMS_TOL_DB of the same baseline this crest baseline was derived from.
# With both tolerances at 1.0 dB the crest inequality restates assert_level
# and can only fail when the peak moves, so assert the peak directly.
```

The docstring goes further and shows the removal is safe under the gate's own
control: at +3 dB the crest falls to 10.058 dB while the peak stays bit-identical
at 32768, and it is the RMS assertion that fires. The redundant assertion was
deleted; the number is still printed.

**The mutually-implied scheduler pair.** `harness_utils.py`'s
`completed_tick_conservation()` states one of two equivalent facts:

```python
# With the two checks above, this is equivalent to "exactly one ticket is
# still pending" -- issued=ticks-1 and pending=1 say the same thing here --
# so only one of the pair is stated.
```

The same function records the opposite lesson in the same docstring — the
*reason* the debt bounds are the caller's declared expectation and never read
back out of the run being judged:

> Comparing `lead` against `maxpending` instead would assert nothing: with
> `rebases=0` the trace samples clock-minus-issued at exactly the points that
> set the driver's own pending high-water, so the two sides are the same
> quantity and a 40-tick backlog satisfies the comparison as happily as a clean
> run does.

That is the precedent in both directions. A redundancy proven from the
definitions is deleted. A "simplification" that turns an assertion into a
tautology is a defect, and `e793d6b` was the commit that had to restore three
presentation gates' absolute expectations after exactly that happened.

**The bar for anything in §4 or §6: show the implication, in source, the way
these two do.** No merge in this document has met it yet.

---

## 6. The long poles

### 6.1 `native_layout` — 36m47s, 27% of a quiet release run

**It is not the build.** The task configures and builds `build-align`
(`CMAKE_BUILD_TYPE=Debug`, `-fsanitize=alignment -fno-omit-frame-pointer`) and
builds one target in `build-asan`. Both directories are repo-relative and
persist. Counting `Building C object` lines between the task banner and the
`Sanitizer fixed/legacy controls` banner:

- run 1 compiled **147** translation units — a near-cold rebuild.
- run 2 compiled **1** (`camera_obstruction_runtime.c.o`).
- Total task time: **37m09s vs 36m47s.**

A 147-file rebuild costs 22 seconds of a 37-minute task. The build is noise.

**It is the runtime matrix.** After three fast legacy controls
(`--legacy-mem02` / `--legacy-mem03` / `--legacy-mem04`, which must each be
*rejected* by the sanitizer), the gate re-runs fourteen entire checks against the
alignment binary, sequentially:

`nav_fixtures`, `attract_demo`, `track_sweep`, `vehicle_sweep`, `adventure_hub`,
`adventure_race_loop`, `trophy_series`, `adventure_two`, `collision_gridmask`,
`race_2p_split`, `race_multiplayer`, `challenge_modes`, `taj_challenges`,
`widescreen_proportions`.

All fourteen are also registered as their own manifest tasks against
`build-rel`. Their combined standalone cost:

| | run 1 | run 2 |
|---|---|---|
| the same 14 checks against `build-rel` (Release) | 7m51s | 7m41s |
| `native_layout` (Debug + `-fsanitize=alignment`) | 37m09s | 36m47s |
| **ratio** | **4.7×** | **4.8×** |

4.7× and 4.8× on two different hosts is a stable structural multiplier, and it
is mostly `-O0`. `-fsanitize=alignment` is a cheap instrumentation; `Debug` is
not.

**Restructuring options.**

1. **Build the alignment tree at `RelWithDebInfo` instead of `Debug`.** The
   suite already does exactly this for its heavier instrumented gate:
   `check_full_ubsan.py` configures `-DCMAKE_BUILD_TYPE=RelWithDebInfo` with
   full UBSan and runs six broad routes in 7m25s. `Debug` here is a choice, not
   a house rule. Evidence required before acting: (a) the three legacy controls
   must still be rejected at the new optimisation level — the gate already runs
   them, so it proves this about itself on the first run; (b) all fourteen
   sub-checks must still pass, none of which is the documented build-type-
   sensitive check (`key_cutscene_once` is, and is not in this matrix);
   (c) a measured before/after on one tree.
2. **Prune `track_sweep` from the matrix** — §4.7. ~3m today.
3. **Parallelise the matrix.** The fourteen sub-checks run in a plain sequential
   loop with no `-j`, no pool. But several of them write EEPROM
   (`trophy_series`, `adventure_two`, `challenge_modes`, `taj_challenges`), which
   is the same constraint that makes the top-level runner sequential. This is
   the *least* attractive of the three: it would require per-sub-check save-dir
   isolation first.
4. `--quick` already exists and cuts the matrix from 14 checks to 1
   (`widescreen_proportions`). It is an iteration tool. It is not a release
   profile — see §7.

### 6.2 `framed_world_views` — 7m51s quiet, 17m56s loaded

**It builds nothing.** No `cmake` or `ninja` invocation appears in the file; it
resolves `build-rel/mdkr64` and runs it. All cost is engine launches.

**Correcting a premise:** there is no "timeout note" for this gate anywhere in
this tree. The two real numbers are `--timeout`, default **300s**, applied
per-arm inside the script (line 740), and the runner's generic
`DEFAULT_TASK_TIMEOUT = 1800` for role `native`
([`../tools/run_checks.py`](../tools/run_checks.py) line 52) — this task does
*not* get the `BUILDING_TASK_TIMEOUT = 10800` budget, which applies only to
`instrumented` and `layout`. At 17m56s the loaded run used ~60% of its 30-minute
task budget. That is the note worth having, and it did not exist before this
document.

**The arm count.** ≈83 sequential engine launches:

- 9 scenes × (3 WebGPU sizes + 1 GL size) × (fixed, unsafe) = 72
- 3 interpolated-boundary scenes × 1 size × 2 arms = 6
- `track-select-scroll` on WebGPU and GL = 2
- 3 PAL envelope arms (GL, 3 sizes) = 3

PAL is 3 of ≈83 runs. The gate hard-fails rather than skipping if no PAL ROM is
found under `--roms`, which is why `tools/run_checks.py` passes it — but PAL is
not the reason this task is a long pole. Arm count is.

**Restructuring option: arm-level parallelism.** Each arm already runs in
`run_dir = work / label` under a task-scoped `TemporaryDirectory`, with
`save_dir = run_dir / "save"` — **per-arm filesystem isolation already exists**.
The runner's sequential rule is about the shared suite save directory; it does
not constrain what happens inside one task. Evidence required: (a) a repeated
run at the intended concurrency producing identical per-arm verdicts, since
these arms compare pixels and concurrent WebGPU contexts are the obvious risk;
(b) a bound on concurrency that keeps each arm inside its own 300s timeout.

`--scene NAME` and `--interpolated-only` already exist as subset flags.

### 6.3 `presentation_breadth` (6m31s) and `presentation_matrix` (3m05s)

Both are dominated by spawn count × per-arm tick budget, and both run their arms
in a plain `for` loop.

- `presentation_breadth`: 36 subprocess runs (17 content arms × base + high,
  plus a 2-run tenancy control), 4200–4800 ticks each, ≈232,800 frames. GL only,
  **no frame dumps at all** — it compares `[SIMHASH]` rows and telemetry.
- `presentation_matrix`: 18 runs, ≈91,840 frames, of which arm C's particle
  (4×8,400), effect (2×8,240) and deformation (2×6,280) arms are 66,640. Three
  runs are WebGPU and several dump PPMs for the tail window — which is why its
  wall-clock ratio to breadth (0.67) is worse than its frame ratio (0.39).

`presentation_breadth` is the cleanest parallelism candidate in the entire
suite: 36 independent spawns, each with its own `run_dir` and its own
`run_dir/save`, GL-only, no pixel comparison to be perturbed by a concurrent GPU
context. `presentation_matrix` is the same lever with the pixel caveat from
§6.2.

Neither should be reduced by dropping arms. Breadth exists because the matrix
gate is *"necessary evidence but not sufficient breadth by itself"* — its
docstring says so, and the arms it adds (hovercraft, plane, three bosses, four
challenge types, 4P, PAL) exist nowhere else.

### 6.4 `full_ubsan` (7m25s) and `array_bounds_sweep` (2m25s)

Both configure and build their own instrumented tree, both hold it under
`exclusive_build_dir()` for the task, both suppress compiler output unless the
build fails, and both were flat across the two runs (1.05, 1.01).

| | `full_ubsan` | `array_bounds_sweep` |
|---|---|---|
| build dir | `build-ubsan-full` | `build-ubsan` |
| build type | **RelWithDebInfo** | Debug |
| sanitizers | `undefined,float-cast-overflow` | `array-bounds,pointer-overflow,shift-exponent` |
| build parallelism | `-j{cpu_count}` | `-j{cpu_count}` |
| routes | 6, sequential | 8, sequential + a static class sweep + boundary controls |
| subset flags | `--gl-only` (drops 1 route, changes the config) | `--no-controls` (documented iteration-only) |

Both build directories were warm in both runs, so nearly all of that 9m50s is
route execution. Neither is a strong target: together they are 7.3% of the run,
and `full_ubsan` already demonstrates the `RelWithDebInfo` configuration that
§6.1 recommends for `native_layout`. The one thing to note is that
`array_bounds_sweep` is at `Debug` and might take the same lever — a smaller
version of opportunity 1, worth measuring after opportunity 1 lands.

---

## 7. Tiered profiles, without building a subset trap

A subset that reads like a full run is the failure mode this project has already
paid for: subset batteries hid a broken gate for three days after 1.0, and the
runner grew an explicit `SUBSET` marker in response
([`../tools/run_checks.py`](../tools/run_checks.py) lines 525–542):

```python
if not restrictions:
    return f"complete suite, {count}/{len(CHECKS)} tasks"
return (
    f"SUBSET {count}/{len(CHECKS)} tasks: " + " ".join(restrictions)
)
```

Any tiering proposal must be expressed **through** that mechanism, never around
it. The runner already has every flag a tier needs — `--only`, `--role`,
`--primary-only`, `--skip-instrumented`, `--skip-wasm` — and every one of them
stamps `SUBSET n/135` on the verdict line.

A workable shape, using the measured numbers:

| tier | selection | measured cost (run 2) | verdict line |
|---|---|---|---|
| per-commit | `--primary-only` — roles `source` + `native` + `ctest`, 94 tasks | 10s + 41m59s + 48s = **42m57s** | `SUBSET 94/135 tasks: --primary-only` |
| nightly | `--role` naming every role except `layout`, 134 tasks | 135m06s − 36m47s = **98m19s** | `SUBSET 134/135 tasks: --role …` |
| **release** | **no flags** | **135m07s measured** | **`complete suite, 135/135 tasks`** |

Three rules make this safe rather than a trap:

1. **The release gate is the full run.** `RELEASE_CHECKLIST.md` §2b already
   states that the release run must print `complete suite, N/N tasks`, "which is
   the only form that counts as a full run." Nothing in this document changes
   that. A tier is a fast signal between releases; it is never evidence for a
   cut.
2. **A tier is a named selection, not a new manifest.** If a tier needs a task
   list, it derives it from a manifest role the way the release checklist
   already derives the web lane from `BROWSER_ROLES` — a hand-maintained second
   list is how a gate goes missing.
3. **The complete suite runs before every cut.** The full 135 tasks, on the
   candidate tree, with the verdict line pasted into the release record. That
   rule exists because a subset battery has already hidden a broken gate here.

The tiering itself saves nothing at release time. Everything in §6 does.

---

## 8. Do not touch

These gates are expensive because expense is what they measure, or cheap and
load-bearing. They are listed so a future pass does not spend time re-deriving
that they are off the table.

**Mutation and fault controls.** Every one of these deliberately breaks
something and requires the gate to notice. A control that never runs is not a
saving; it converts its parent gate into an unfalsifiable assertion.

- `world_fx_capture`'s `matrix-drop` arm; `world_shadows`' resource-failure
  latch arm; `shadow_plausibility`'s `MDKR_TEST_SHADOW_BOGUS_CASTER`.
- `camera_projection_fallback_runtime` (injected latch failure) and
  `camera_emergency_readability_runtime` (forced no-alternate) — both of which
  exist *only* as controls and each of which needs its own process (§4.2).
- `camera_obstruction_runtime`'s `legacy` and `center-ray` positive controls and
  its `unset` default arm.
- `state_hash`'s `MDKR_RNGSEED=legacy` arm; `audio_level_reference`'s
  injected-gain arms; `collision_headroom`'s forced saturation;
  `boost_magnitude`'s perturbed constants; `charselect_motion`'s frozen-frame and
  fast-loop arms; `app_capture`'s `--self-test` mutations;
  `taj_results_portrait`'s ordinary-Diddy negative control; `render_purity`'s
  divergence control; `shadow_stage_reset`'s suppressed-reset control;
  `presentation_breadth`'s tenancy control.
- `native_layout`'s `--legacy-mem02/03/04` arms. These are the only reason the
  alignment matrix means anything, and they are seconds, not minutes.

**`determinism` (25s).** The check exists because headless rendering silently
was not reproducible — `osGetCount()` returned the host clock, so the
character-select animation phase differed on every run: 10 distinct images over
10 runs, median 18.9% of pixels differing. Every frame-comparison gate in the
project, the oracle scoring, and the GL/WebGPU parity scoring all assume what
this gate proves. It is 0.3% of the run.

**`state_hash` (2m26s, ratio 0.90).** The anchor for the instrument every
presentation gate is written against. Its own docstring: *"Every later gate —
render purity, the presentation-rate matrix, catch-up equivalence — is 'this
stream is identical' between two schedules. This check anchors the instrument."*
Its four arms are determinism, presentation invariance, a legacy-RNG divergence
control, and per-field-family controls. Removing any of them makes the identity
assertions in eight other gates vacuous.

**The six `browser`-role tasks (12m56s).** Real-time bound, not CPU bound (§3).
The only way
to make it faster is to drive fewer frames, which is a coverage decision, not an
economy. `browser_taj_persistence` taking *exactly* 3m11s on both a loaded and a
quiet host is the measurement that settles this.

**The 94 sub-minute tasks (20m17s, 15%).** Including all seven `source`-role
audits, which cost 10 seconds in total and include the CI contract, the WebGPU
fault-point census, and the address-domain narrowing audit.

---

## 9. The five opportunities, and what must be proven first

Estimates are quiet-host minutes off the 135m07s release run. Loaded-host
savings are larger and less meaningful.

| # | opportunity | est. saving | evidence required before acting |
|---|---|---|---|
| 1 | `native_layout`: build `build-align` at `RelWithDebInfo` instead of `Debug`. The measured Debug-vs-Release multiplier on the same 14 checks is 4.7×/4.8×; `full_ubsan` already runs full UBSan at `RelWithDebInfo`. | **15–24m** | The three `--legacy-mem02/03/04` controls must still be *rejected* at the new level (the gate proves this about itself on its first run). All 14 sub-checks must still pass. A measured before/after on one tree, both numbers recorded. |
| 2 | `framed_world_views`: arm-level worker pool over its ≈83 launches. Per-arm `run_dir` and `run_dir/save` isolation already exists. | **5m** (13m loaded) | Repeated runs at the chosen concurrency producing identical per-arm verdicts — these arms compare pixels and concurrent WebGPU contexts are the risk. A concurrency bound that keeps every arm inside its own 300s `--timeout`. |
| 3 | `presentation_breadth`: same lever over its 36 spawns. GL-only, no frame dumps, per-arm `run_dir/save` — the cleanest candidate in the suite. | **4m** (12m loaded) | Identical `[SIMHASH]` streams and telemetry verdicts across at least two runs at the chosen concurrency. Confirmation that the PAL arms' ROM handles are per-arm. |
| 4 | `presentation_matrix` (18 spawns) and `camera_obstruction_display_matrix` (24 spawns): same lever, plus folding `camera_obstruction_runtime`'s modern arm into `display_matrix`'s `4:3-high`/`authored` cell by raising it from 3600 to 5200 frames. | **3m** (6m loaded) | As #2 for the pixel-dumping arms. For the fold: a run showing the 5200-frame cell emits every counter `camera_obstruction_runtime`'s modern arm reports today, and that the `unset`-reproduces-`modern` assertion still has a `modern` result to compare against. |
| 5 | `native_layout`: drop `check_track_sweep.py` from `RUNTIME_CHECKS`. Its 20 routes are a strict subset of `check_vehicle_sweep.py`'s 47 over the same tracks, frames and autopilot. | **3m** today, ~1m after #1 | That no assertion unique to `check_track_sweep.py` — specifically the animation-target initialisation count — is build-type- or sanitizer-sensitive, i.e. that making it only against `build-rel` loses nothing. Proven the way §5's two cases are proven: from the definitions, in source. |

Combined, without deleting a single gate: **roughly 30–35 minutes off a 135-minute
release run**, and roughly an hour off a loaded one. Every one of them is a
change to *how* a gate runs, not to *what* it asserts.

Two further items, recorded but not costed:

- **`overlay_pause`** moved 1m05s → 4s on an unchanged script. A 16× spread is
  larger than contention comfortably explains; something else is going on and it
  is worth one measurement.
- **`taj_character_select_ultrawide` skips the oversized-shadow negative
  control** (§4.4), because that control is guarded on `args.aspect is None`.
  That is a coverage gap surfaced by a cost analysis, and closing it will make
  the suite slightly slower.
