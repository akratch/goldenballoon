# Which smoothing stage moves the pixels — 120 Hz stage attribution

Date: 2026-08-08
Branch: `codex/v1.0.3-reliability-ux` (worktree `presentation-gold-standard`)
Host: Apple M3 Max, macOS arm64, AppleClang release build (`build-rel`)
ROM: US v80 Rev 1, validated by the runtime banner
Instrument: `tests/check_smoothing_stage_bisection.py` (registered as
`smoothing_stage_bisection`), run at `MDKR_PRESENT_RATE=120`

Motion smoothing is seven independently gated stages sharing one interpolated
replay walk (`docs/architecture/presentation-interpolation.md` §2). Every
existing gate asks a yes/no question of one stage against a route chosen to
make that stage visible. None of them says which stage carries the motion when
all seven run together, and that is precisely the question an artifact hunt has
to start from. This note answers it.

## Method

The same scripted route is rendered once per stage configuration at the densest
alpha grid production can request: `MDKR_PRESENT_RATE=120` against the 30 Hz
authoritative tick, i.e. three interpolated presents (alpha 1/4, 2/4, 3/4)
between every pair of authored images. The configurations are **all-on**
(production), **all-off** (every `MDKR_TEST_*_INTERPOLATION` opt-out set — only
the interpolated camera still moves), and **leave-one-out**, one arm per stage.

Each arm dumps the same 120 backend frames — 30 authoritative ticks, 90
interpolated images — through the offscreen dump path
(`platform_dump_frame`, backend readback before the swap, never the swapchain).
Each arm is differenced against its route's all-on arm over those 90
intermediates only. Two metrics, because they answer different questions:

- **changedfrac** — fraction of PPM payload bytes that differ, averaged over
  the 90 intermediates. *How much of the screen the stage touched.*
- **meanabs** — mean absolute difference over the whole payload, same average.
  *The same area weighted by how far those bytes moved.*

Neither is readable alone, and their ratio (`meanabs / changedfrac`) is the
mean magnitude among the bytes that actually changed: a wide faint change and a
narrow violent one look alike in one metric and opposite in the other.

**ceilingshare** is a stage's `changedfrac` over the all-off arm's, i.e. its
share of everything the seven stages together contribute above a
camera-only interpolation.

Leave-one-out rather than one-stage-at-a-time on purpose: a stage measured
alone is measured in a scene the other six never composed, which is not the
scene the artifact appears in.

### Why two routes

No single route fires all seven. Route A registers particle vertices and then
holds every one of them (`particledeformoverride=0`), so ranking `particle`
there would report an absent stage as an innocent one. Route B is the tree's
only witness for a world-space point-trail mesh that actually moves between
adjacent authored ticks.

| Route | Level | Ticks | Sampled window | Stages ranked |
|---|---|---|---|---|
| A | 29 Jungle Falls, in-race, `MDKR_FORCE_SHIELD` | 3230 | ticks 3200–3230 | object, deformation, vertex_color, primitive_alpha, effect, uv_scroll |
| B | 26 battle challenge | 4200 | ticks 4170–4200 | particle |

Route A's window is deliberately after authored tick ~3120, where the route's
countdown clears — before it the racers are screen-static and every model stage
reads as innocent for the wrong reason.

## Attribution

### Route A — Jungle Falls, in-race

| Rank | Stage | changedfrac | meanabs | intermediates differing | ceilingshare |
|---|---|---|---|---|---|
| — | **all-off (ceiling)** | 0.126741 | 4.271366 | 90 / 90 | 1.0000 |
| 1 | object | 0.126741 | 4.271366 | 90 / 90 | 1.0000 |
| 2 | effect (shield shear) | 0.070298 | 3.070600 | 90 / 90 | 0.5547 |
| 3 | uv_scroll | 0.051181 | 0.128874 | 78 / 90 | 0.4038 |
| 4 | deformation | 0.012790 | 0.364297 | 90 / 90 | 0.1009 |
| 5 | vertex_color | 0.002392 | 0.003787 | 90 / 90 | 0.0189 |
| 6 | primitive_alpha | 0.000000 | 0.000000 | 0 / 90 | 0.0000 |
| — | particle | *not exercised on this route* | | | |

### Route B — battle challenge

| Rank | Stage | changedfrac | meanabs | intermediates differing | ceilingshare |
|---|---|---|---|---|---|
| — | **all-off (ceiling)** | 0.255066 | 2.434888 | 90 / 90 | 1.0000 |
| 1 | particle | 0.005591 | 0.028988 | 90 / 90 | 0.0219 |

### Disposition — what each stage did on each route

Two counters per stage, from the all-on arm's `[PRESENT-PACKET]` row, because
one number cannot separate two questions. **Reach** is how many times the
replay looked the stage's data up; **fire** is how many times it actually
substituted an interpolated value. A stage can be reached and never fire, and
one here is.

| Stage | Reach counter | Route A | Route B | Fire counter | Route A | Route B |
|---|---|---|---|---|---|---|
| object | `matrixhit` | 92,094 | 139,608 | `matrixoverride` | 65,424 | 113,589 |
| deformation | `deformoverride` † | 332,667 | 388,920 | `deformoverride` | 332,667 | 388,920 |
| vertex_color | `colorhit` | 767,829 | 679,350 | `coloroverride` | 110,337 | 194,583 |
| particle | `particledeformhit` | 20,166 | 66,576 | `particledeformoverride` | **0** | 33,012 |
| primitive_alpha | `primalphahit` | 654,816 | 790,161 | `primalphaoverride` | 119,129 | 178,038 |
| effect | `effecthit` | 708 | **0** | `effectoverride` | 708 | **0** |
| uv_scroll | `uvscrollhit` | 70,389 | 222,099 | `uvscrolloverride` | 70,389 | 222,099 |

† `deformhit` is shared with the particle lookup and only falls from 767,829 to
20,166 when the deformation seam is off, so it cannot serve as that stage's
reach witness; its override is the clean one.

Every stage gets an explicit disposition on both routes, ranked or not:

| Route | Stage | Disposition |
|---|---|---|
| A | object, deformation, vertex_color, primitive_alpha, effect, uv_scroll | `fires` — ranked |
| A | particle | `reached-never-interpolated` — 20,166 lookups, 0 overrides |
| B | particle | `fires` — ranked |
| B | object, deformation, vertex_color, primitive_alpha, uv_scroll | `fires` — not ranked here |
| B | effect | `absent` — never reached |

### The opt-outs demonstrably reach the binary

Every leave-one-out arm must drive its stage's reach counter to **exactly
zero**, and the all-off arm must zero all seven. Without that check the
harness's worst failure is silent: a mistyped `MDKR_TEST_*` name produces an
arm identical to all-on, whose zero pixel difference is indistinguishable from
a stage that ran and changed nothing — and this table contains a real zero of
the second kind, so "zero difference" cannot validate itself.

Measured, route A, all-on → that stage's off arm:

| Stage | Reach counter | all-on | stage off |
|---|---|---|---|
| object | `matrixhit` | 92,094 | 0 |
| deformation | `deformoverride` | 332,667 | 0 |
| vertex_color | `colorhit` | 767,829 | 0 |
| particle | `particledeformhit` | 20,166 | 0 |
| primitive_alpha | `primalphahit` | 654,816 | 0 |
| effect | `effecthit` | 708 | 0 |
| uv_scroll | `uvscrollhit` | 70,389 | 0 |

`object`-off additionally zeroes all six other reach counters — the same
subsumption the pixels show, visible in the packet census independently.

## What the ranking says

**`object` is not a peer of the other six — it is the gate they hang off.**
The `object`-off arm reproduces the all-off arm *byte-for-byte* on all 120
frames, not merely to within the metric. The mechanism is in the code:
`dkr_replay_object_alpha_valid` is `object_alpha_valid &&
dkr_replay_object_interpolation_enabled() && …`
(`platform/fast3d/gfx_pc_dkr.c`, the replay-walk entry), and deformation,
vertex colour, particle and primitive alpha all test that same flag before
their own seam. So `MDKR_TEST_OBJECT_INTERPOLATION=off` disables five stages,
not one. Its 1.0000 share is the ceiling restated, and **it must not be added
to the others' shares** — the six numbers are not a partition. The seam
inventory presents these as seven peer opt-outs; that is true of the envs and
false of the mechanism, and the artifact hunt should treat `object` as the
substrate rather than as a candidate.

**Ranking the genuine peers: effect ≫ uv_scroll ≫ deformation ≫ vertex_color.**
The two leaders are the same size by area and nothing alike by magnitude.
Shield shear carries a meanabs of 3.07 against a 4.27 ceiling on 7.0% of the
frame — mean magnitude 44 per changed byte, a hard displacement of a small
region, which is what a two-lifetime recipe reconstruction around each racer
should look like. UV scroll covers 5.1% of the frame at meanabs 0.129 — mean
magnitude 2.5, a scrolling texture phase nudging many texels by a fraction of
a shade.
Those two are where a 120 Hz artifact hunt should look first, and they fail in
visibly different ways — a shear artifact would be a shape defect on one
object, a UV artifact a phase pop across a whole surface.

**Two stages measure zero, for two different reasons. Neither is a removal
candidate.**

- **`primitive_alpha`: 119,129 overrides applied, zero pixels changed on this
  window.** The stage ran and substituted interpolated alpha 119 thousand
  times, and not one of those substitutions survived to the backend frame.
  The disarm check makes this readable rather than suspicious: the same env
  takes `primalphahit` from 654,816 to 0, so it unambiguously reached the
  binary and unambiguously disarmed the stage, and the arm really is a
  leave-one-out. So the zero is genuine — the strongest artifact-innocence
  signal in the table *for this content* — and it is also a question worth
  asking on its own: an override that never reaches a pixel is either alpha
  already at its endpoint value, or a fade flattened downstream.
  `check_presentation_matrix.py`'s primitive-alpha arm proves it is not always
  invisible — it runs the battle challenge, where point-trail fades dominate.
  Read this cell as *innocent on Jungle Falls*, not *inert*.
- **`particle` on route A: reached 20,166 times, fired 0.** Not innocence at
  all, and not absence either — the replay looked the point-trail meshes up on
  every walk and never found an interpolable pair. Ranked from route B
  instead, where it fires 33,012 times and contributes 2.2% of the ceiling.

**Route sensitivity is real and the window matters more than the route.** The
first run of this harness sampled ticks 3270–3300 and reported `uv_scroll` at
exactly zero across all 90 intermediates. The stage was firing (70,599
overrides) — the camera had simply left the waterfall. Moving the window to
ticks 3200–3230 put it at rank 3 with 78 of 90 intermediates differing. Any
zero in this table is a statement about a 30-tick window, not about a stage.

## The uncaptured-external fail-closed arm

The same gate carries the evidence for `dkr_retain_resolved_pointer`'s
fail-closed change (residual obligation 2 in
`docs/architecture/presentation-interpolation.md`).

```
[UNCAPTURED-OWNERSHIP] arms=11 interpolated_replays=114957 uncapturedext=0
[UNCAPTURED-EXTERNAL] control_ext=0 control_interp=1791 forced_ext=68817
                      forced_refusals=1779 forced_interp=12 forced_stale=1788
                      untokened_refusals=0
```

- **Production resolves zero uncaptured externals.** `uncapturedext=0` on all
  eleven production arms across both routes — every all-on, all-off and
  leave-one-out arm, **114,957 interpolated replays**. The assertion requires
  the stat field to be *present* before reading it: a dict lookup with a
  default would pass silently the day `uncapturedext` is renamed or dropped,
  which is the one failure mode that would quietly retire this whole claim.
  The arm and replay counts are emitted by the run rather than summed by hand
  — an earlier draft of this note said "22,577 replays", which was both an
  addition slip (the two all-on arms are 9,657 + 12,567 = 22,224) and a
  coverage overclaim, since only those two arms asserted anything.
- **The refusal branch is not vacuous.** With `MDKR_TEST_UNCAPTURED_EXTERNAL=1`
  plus the versioned token, every external lookup is forced to miss: 68,817
  uncaptured resolutions, 1,779 of 1,791 interpolated walks refused, and the
  refusals become *held authored images* (`stale` 9 → 1,788) rather than
  dropped frames.
- **The seam is token-gated.** The same variable without
  `MDKR_INTERNAL_TEST_TOKEN` refuses nothing.

### What was actually reading live memory before

Instrumenting the miss path on the pre-fix tree found **22 distinct addresses
per interpolated walk**, in two populations:

| Population | Count | Disposition |
|---|---|---|
| Port static display lists in the executable's `__DATA` — `dRdpInit`, `dRspInit`, the `dRenderSettings*` table, `dDebugFontSettings`, `dDialogueBoxBegin`/`DrawModes`, `dTransitionFadeSettings`, `dTextureRectangleModes` | 18 | Genuinely uncaptured non-arena externals, reached as `G_DL`/`G_DMADL` targets. Now copied by `dkr_capture_nonarena_list` during the real walk. |
| Segment-token resolutions already inside the retained private image | 4 | Never a hazard — owned by construction. `dkr_retain_resolved_pointer` now short-circuits the retained arena window before consulting the dependency table. |

Nothing rewrites the static lists today, so this is not a bug fix. It is what
makes the ownership claim complete rather than *"complete except for storage we
believe is immutable"* — and the belief is no longer load-bearing, because a
list that ever does become mutable now costs a held authored image instead of a
silent misread.

## Explicitly open

- **The table ranks one 30-tick window per route.** It is an instrument for
  aiming the artifact hunt, not a claim about the whole game. A stage's zero is
  a property of the window (see the `uv_scroll` re-sample above).
- **`primitive_alpha`'s 119,129 no-op overrides are unexplained.** The harness
  measures that they change nothing here; it does not say why.
- **No route ranks all seven stages together.** `effect` never fires on route B
  and `particle` never fires on route A, so the two ceilings are not
  comparable to each other — only within a route.
- **GL only.** The WebGPU backend reads back through the same
  `gfx_read_framebuffer_rgb` path and is expected to agree, but this note does
  not assert it.
