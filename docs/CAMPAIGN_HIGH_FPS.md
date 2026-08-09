# Campaign — S-tier high-FPS gameplay

The objective: make this the best way to play Diddy Kong Racing on any platform,
with motion smoothing good enough that the question "should it default on?" is
decided by evidence rather than nerve.

This document is the plan of record. It is written to be falsifiable: every item
carries the mechanism, the evidence that established it, and the bar it must
clear. Items move to CLOSED only with a witness attached — a gate, a measured
number, or a stated negative result. "It looked fine" closes nothing.

---

## 0. The rule that orders everything

**Fix the instruments before trusting any measurement.**

This project has now had three separate cases of a green signal meaning less
than it appeared:

1. A "WebGPU starvation defect" that was an artifact of the synthetic pacer.
   Three fixes were built, one shipped, it regressed the stutter gate, and the
   whole thing was reverted when re-measured under a real pacer.
2. `check_pacing_quality` could obtain no paced session, report it as a *note*,
   and still print a banner asserting the property it had not evaluated.
3. `rotation_arc_audit` grades `stepped = (int32_t)(delta * alpha)`, which
   truncates toward zero — so its two clauses hold arithmetically for every
   possible input. It is a real mutation detector for the lerp function, and a
   **tautology** as evidence about the composed pose.

The common shape: a number that cannot fail is indistinguishable from a number
that passed. Every new gate in this campaign must state what would make it fail
and, where practical, ship a control that proves it does.

### The pacer trap, stated correctly

`platform_sdl_min.c`:

```c
s_paceMode = (g_headlessFrames < 0) ? PACE_REALTIME : PACE_SYNTH;
```

**The switch is `--headless-ticks`, not `MDKR_SYNTH_FIELDS`** — the latter only
pins the field count. Because `platform_present_display_quantum_units()` returns
0 unless the pacer is real, **the alpha-grid projection is dead code in every
headless gate**. Any counter whose meaning depends on elapsed real time — GPU
frames in flight, admission skips, retire windows, displayed intervals — is
meaningless under it. Structural counters (owners, generations, registrations,
tick agreement) are unaffected and remain trustworthy headless.

---

## 1. Closed in this campaign

| Item | Evidence |
|---|---|
| `check_pacing_quality` can no longer pass having measured nothing | No baseline is a failure; `--allow-no-baseline` downgrades the banner explicitly. Re-run PASSES with its full banner, which also establishes this host does pace. |
| `check_smoothing_stage_bisection` reports the measured `uncapturedext`, not a literal | PASS across 11 arms, 114,957 interpolated replays. |
| The pacer trap is documented at the right switch | `open-items/renderer.md`; the old entry blamed the field-count pin. |
| Held-matrix classification, and the moving-hold class gated | `objhit=6552 objhold=0` on a 120 Hz race route; packet level `matrixoverride=9276` vs `matrixhold=150` (1.6%). The ~25% figure that motivated the investigation does not reproduce. |

**Negative results are results.** The moving-hold hypothesis was the leading
candidate for the owner's "shimmer when the camera moves fast" report. It is not
confirmed on this evidence, and the instrument that settled it is now a standing
assertion so the class cannot return silently.

---

## 2. Open — ordered by player impact

### P1 · Scrolling surfaces (waterfalls, rivers, lava)
The owner's actual complaint. `obj_loop_texscroll` advances scroll through a
2-bit accumulator, so a rate that is not a multiple of 4 emits an **alternating**
whole-unit delta (127 → 31, 32, 32, 32). The two-tick confirmation rule requires
agreement, so those batches hold.

ROM enumeration: 23 texscroll entries across 17 levels; **13 levels carry at
least one fractional rate**, and the minimum authored magnitude is 12
quarter-units/tick — so no shipped content takes the zero-displacement path
originally hypothesised. **Jungle Falls is itself fractional (127 mod 4 = 3)**
yet is recorded confirming 726/726, and both gates that exercise UV scroll run
Jungle Falls *only*. Resolving that tension is the crux.

**Bar:** the true predicate derived from the accumulator plus the confirmation
rule; a gate arm on a fractional scroller that is not Jungle Falls; before/after
hold rates on both a fractional and a clean scroller. The wrap rule may not be
relaxed — it prevents a worse artifact.

### P2 · Camera-cut notes filed in the wrong ID space
Note sites pass the player index; the consumer keys on
`gActiveCameraID + (gCutsceneCameraActive ? 4 : 0)`. When a cutscene camera is
active the note misses and is then cleared unconditionally, so the camera
**blends across a hard cut** — the one artifact class the discontinuity
machinery exists to prevent, and the 2000-unit teleport threshold provably
cannot see it.

**Bar:** one ID space; unconsumed notes carried rather than destroyed; a counter
for unconsumed notes asserted zero on a route that exercises a cutscene camera
together with a mode change. A pose-based classifier cannot witness a cut that is
invisible from the pose, so the counter *is* the witness.

### P3 · World content with no presentation identity
Rain, snow and rain splashes; the skydome and lens flare (both pinned to the
tick-T camera while the camera interpolates); water and lava wave geometry
(whose *texture* glides while its *geometry* steps). None is a spawned object, so
none gets an owner, and all are replayed verbatim. Precipitation has the largest
per-tick screen displacement in the scene.

**Bar:** substitution counters nonzero on a route with weather and a visible sky;
`check_state_hash` and `check_render_purity` unmoved after every case.

**DONE (2026-08-09):** rain, snow, lens flare and rain splashes. Snow route
167,800 registered batches / 476,187 moved substitutions / 3,816 wrap-guard
holds; rain 2,072 / 1,278 / 0; 8 renderer-owned transforms live at peak with
1,339 captures copying them; zero snapshot overflows, zero discontinuity
blends; authoritative rows byte-identical with and without smoothing on both
routes. Gate: `tests/check_weather_presentation_identity.py`.

**REMAINING: wave geometry (water and lava), analysed, not implemented.**
Residual obligation is 0 — the geometry holds at the authored pose rather than
moving wrongly, so this is added smoothness and not a defect repair. What the
source establishes:

* `gWaveVertices[4]` / `gWaveTriangles[4]` come from `mempool_alloc_safe`
  (`waves_alloc`), so they are **arena** memory and the retained whole-arena
  copy already contains them at both endpoints. Unlike the lens flare and the
  splashes, no external-transform registration is needed.
* `waves_update` opens with `gWaveVertexFlip = 1 - gWaveVertexFlip`. The mesh is
  already double-buffered, so the previous tick's vertices are live in the other
  slot — both endpoints of the pair are resident by construction.
* All wave motion is in the vertices. The `mtx_cam_push` bracket in
  `waves_render` carries only the tile origin, which is why the **texture**
  glides (animated-texture path) while the **geometry** steps.

Why address identity cannot pair the endpoints: the flip makes snapshot T point
at `gWaveVertices[flip]` and T+1 at `gWaveVertices[1-flip]`, so the addresses
differ every tick, forever. This is the snow/rain shape and wants the same
logical batch identity — keyed on `(gWaveBlockIDs[i], sub-tile, subdivision
row)`, which is stable across the flip.

**The design question — RESOLVED 2026-08-09 by source read, no API change
needed.** `gfx_presentation_packet_lookup_deformation` keys on `(owner,
viewport, ordinal)` while `register_vertex_identity` takes no ordinal, which
looked like it forced a choice between per-row tokens and growing the API. The
capture side settles it: in `gfx_pc_dkr.c`'s `G_VTX` case, a
`PARTICLE_VERTICES`-class binding captures with **ordinal hardcoded `0u`**;
only `PROJECTED_SHADOW_VERTICES` passes `packet_binding.ordinal`, and the
generic matrix-owned path draws from `dkr_deformation_next_ordinal()`. So for
the identity class waves would use, ordinal is always 0 and **the identity
token must be unique per emitted batch** — one token per `(block, sub-tile,
subdivision row)`. That is the same rule snow and rain already follow (one
token per flake physics slot, one per rain layer); waves are not a new case.

**And the wrong choice is fail-closed, not a wrong blend.** If two batches did
share a key, `deformation_capture` does not let the later one overwrite the
earlier: it marks the entry `ambiguous`, increments `deformation_collisions`
and returns false, so the batch holds. The feared artifact — row A's vertices
blended out of row B's — cannot occur. `deformcollisions` is already reported
in the `[PRESENT-PACKET]` row, so **`deformcollisions == 0` is the assertion
that proves the token scheme is right**, and a mistake costs a hold rather than
a corruption. This makes the case safe to implement and cheap to falsify.

Remaining unknown, to settle with measurement rather than argument: the value
of `max_vertex_delta`. Wave X/Z are fixed grid positions and never move; only Y
changes, as `(gWaveHeightTable[i0] + gWaveHeightTable[i1]) *
gWaveController.magnitude` scaled per vertex. Ordinary motion advances each
index by `updateRate` (one table entry); a seed wrap in `waves_tick` jumps the
index by `-seedSize` and so samples an arbitrary phase. Derive the limit from
the table's own maximum adjacent-entry slope times `magnitude`, with a safety
factor, and confirm against measured per-tick deltas — do not pick a round
number.

**Bar for the wave case:** a water route (Crescent Island) and a lava route
(Hot Top Volcano) arm on the weather gate, each asserting registered batches
> 0, moved substitutions > 0, and authoritative rows byte-identical with and
without smoothing. The wrap guard needs its own evidence here rather than
inheritance: `waves_tick` wraps `gWaveHeightIndices` at
`gWaveController.seedSize`, and the wrap is in the *index*, not the vertex, so
measure the emitted vertex delta at a wrap before assuming `max_vertex_delta`
fires on it.

### P4 · Render-only residuals are frozen for the whole interval
`carBob`, tumble offsets and crash lift are added *inside* a bracket restored
before the snapshot, so the replay adds a constant tick-T adjustment at every
alpha. Translation glides at 120 Hz while bounce and spin step at 30 Hz —
worst exactly on a rocket hit or hard landing, the moment smoothing matters most.
No gate can see it: endpoint exactness holds, and the arc audit grades snapshot
angles rather than the composed pose.

**MEASURED (2026-08-09), and it downgrades this item.** `MDKR_RESIDUAL_CENSUS=1`
reports the per-tick change in the residual — which is exactly the size of the
pop a player receives. On a 592-tick 120 Hz race route:

| | Y (world units) | rotation Z |
|---|---|---|
| p50 | 0.000 | 0 |
| p95 | 0.815 | 70 |
| p99 | 1.505 | 157 |
| max | 5.064 | 198 (1.09°) |

233 of 592 ticks carry a nonzero pop, but only 13 exceed one world unit. Against
a kart travelling tens of units per tick, p95 is a small fraction of a pixel and
the rare max is around a pixel. **The defect is real and confirmed present; its
magnitude on ordinary racing content is not what the audit's "largest remaining
artifact" framing implied.**

**Still open, and the reason this is not closed:** the predicted worst case —
a rocket hit or a hard plane landing, where `carBob` and the crash lift swing
tens of units per tick — is *not exercised by this route*. The instrument is
committed and armed by one env var, so measuring it is now cheap.

**Bar to close:** the same census on a route that forces a rocket hit. If the
worst case stays sub-pixel, document and close without a code change; if it does
not, make the residual interpolable rather than constant.

### P5 · Correctness hygiene
- Billboard *anchor* lacks the camera-cut check its *matrix* has, so sprite
  position interpolates while scale and tilt hold.
- `dkr_replay_interior_alpha` infers endpoint-ness from `numerator == 0`, so the
  ownership proof rests on the pacer's ticket accounting rather than caller
  intent. Latent; make it an explicit parameter.
- `presentation_snapshot_note_spawn` fails **open** on a full identity table,
  publishing a new object under a dead one's generation. Latent (4096 slots vs
  ~512 population); the direction is inverted.
- Camera sums two independently-snapped angles (`rotation_x + pitch`);
  sum-of-shortest-arcs is not the shortest arc of the sum.
- Test hooks window in *present* units via `g_frameCounter`; under smoothing
  that runs 2–4× too fast. Use `g_simTickCounter`.

### P6 · Evidence architecture
- **A ground-truth oracle.** No gate compares an interpolated frame to truth;
  every verdict is arm-vs-arm or endpoint identity. Generalise the effect-shell
  envelope to all objects: assert each object's screen position lies on the
  segment between its authored T and T+1 positions. This would have caught the
  shipped one-tick effect-shell lead on day one.
- **Re-site the 120 Hz smoothing gates onto the realtime pacer**, which is the
  only route to genuine WebGPU smoothing coverage.
- **Publish a held fraction per frame** — every hold is a piece of the image
  running at 30 Hz inside a 120 Hz frame.

---

## 3. Cross-platform behaviour

Established: the alpha-grid projection is active for **exactly one** policy —
`display` on a reported fixed refresh — which is precisely what the one-click
"Smooth" choice expands to. Every other path already declines it. Under VRR the
panel scans out on arrival, so the projection becomes a phase error up to half a
refresh, and VRR is not detectable through SDL.

- **Stand the projection down behaviourally.** When displayed intervals stop
  landing on the derived grid, set the quantum to 0 and use raw measured alpha —
  which under VRR is the *correct* value. This makes "Smooth" safe on every panel
  without asking the player what hardware they own.
- **Do not** auto-expand Smooth to "Just Under Display": on a fixed 60 Hz panel a
  57 Hz software grid against a 60 Hz blocking queue beats once per second.
- **Surface the inert combination.** Motion smoothing + Frame limit Original arms
  nothing, silently.
- **Lead with PAL.** 25 Hz on a 60 Hz display alternates 2/3 refreshes; smoothing
  removes that ripple without touching speed or pitch. It is the strongest
  argument for the feature and it is currently buried.

Reference implementations (RT64, Zelda 64: Recompiled, Ship of Harkinian) compute
the weight from rational tick accumulators **without** a display-grid projection —
ours is the anomaly, and it is the part that breaks.

---

## 4. Practices worth adopting from prior art

RT64 is the closest analogue in existence and its scars are instructive:

- **Explicit identity beats heuristics.** RT64 built an extended GBI to tag
  matrices because auto-matching is "prone to errors"; Zelda64Recomp tags per
  actor *and per limb*. We own the DKR source and could tag.
- **Fallback on identity failure = snap to the authored transform, never guess.**
  Every project converged on this. We already do.
- **Per-component interpolation policy** — position/rotation/scale/tile/vertex
  independently skippable.
- **A one-frame skip flag** on spawn, teleport, respawn, slot reuse and cutscene
  pops. Particle pools recycle slots; without it a dying particle smears into the
  one that reused its slot.
- **Camera: simple lerp, not decomposition** — RT64 and Zelda64Recomp agree
  independently.
- **UV scroll: wrap-aware modulo**, keeping the previous delta when the integers
  are consistent with a wrap, and gating tile identity on texture-content hash so
  flipbooks are never mistaken for scrolls.
- **Mirrored limbs.** N64 animations mirror geometry via *negative scale*; naive
  decomposition turns models inside-out. RT64 carries explicit rebiasing for it.
- **The long tail is real.** Once everything else is smooth, every element left at
  tick rate reads as a bug. Budget for it.

---

## 5. The structural ceiling, stated honestly

Transform-space replay re-executes the same command array: it can change *where*
something is drawn, not *what*. Permanently at 30 Hz regardless of any future
work: the entire 2D layer (HUD, menus, wipes), all texture and palette animation,
animation frames and LOD selection, spawn and despawn, and every colour except
primitive alpha.

That residue is not small in absolute terms — it is *separable* (mostly HUD) and
perceptually forgiving (spawns). But it is exactly the residue a sensitive player
notices **more** at high refresh, because smooth pursuit of a gliding background
makes a 30 Hz neighbour look like it is vibrating. Any future "default it on"
decision has to survive that fact, not ignore it.

What the approach buys instead: exact alpha endpoints (impossible for optical
flow), no disocclusion invention, no UI warping, deterministic and individually
fixable artifacts, and — decisively for this game — no simulation change. The
alternative of running the simulation faster is already measured and dead: an
Enhanced one-field cadence finishes a reference lane 1.14× fast, so DKR's physics
are not invariant to repartitioning. The community's own 60 fps GameShark code is
a tick-doubler with documented broken slope physics. We do not ship that.

---

## 6. The default question

Motion smoothing stays **opt-in** until all of the following exist:

1. A paced measurement at 120 Hz **on a real 120 Hz panel**. Every automated
   number to date was taken on a 60 Hz host, where a 120 Hz request is already
   physically unpresentable.
2. The alpha-grid projection exercised by a gate that cannot silently skip, plus
   a VRR answer.
3. Evidence that the 30 Hz residue does not read *worse* against a smooth
   background than against a 30 Hz one.
4. A cost measurement on low-power hardware. Current figures come from an M3 Max;
   the machine that reported the shimmer is a handheld.

This is not settling. It is refusing to repeat the overconfidence that produced
the reverted WebGPU work.
