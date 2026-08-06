# Open items — Renderer and visual fidelity

> One subsystem of the split [open-items index](README.md), which states how these files are kept.

**Currently open** (per [`README.md`](README.md#still-open)'s open/closed
table): 6 items.

| Item | Where |
|---|---|
| Shadow strength is one preset-wide umbra multiplier, not per-surface: no N·L term, bias not slope-scaled, umbra values unmeasured against a reference image | [§ OPEN: shadow strength is one preset-wide choice](#open-shadow-strength-is-one-preset-wide-choice-not-a-per-surface-one) |
| Shadow gate trustworthiness: the harness environment (`MDKR_TRACE`) has twice diverged from the shipping build and let a real defect through green gates | [§ OPEN: shadow gate trustworthiness](#open-shadow-gate-trustworthiness--harness-environment-has-diverged-from-the-shipping-build-twice) |
| M1 residuals deliberately deferred: WebGPU 4x MSAA (IQ-8) and a loader-only, content-free texture-pack path (IQ-11) | [`docs/STATUS.md` § imagequality — mipmaps reach the GPU (M1, in progress)](../STATUS.md#imagequality--mipmaps-reach-the-gpu-m1-in-progress) |
| **(index-level item)** WGPU-11 external/oracle corpus breadth — local asset, capacity, and fault closeout is DONE (46 native routes, 249M+ strict commands, zero faults); browser complete-corpus/minimum-feature hardware, independent state reference, and external native platforms remain. Absorbs the former M4.5 "open notes" row (below), whose notes are all FIXED/CLOSED | [§ Still open](README.md#still-open), [`check_webgpu_content_census.py`](../../tests/check_webgpu_content_census.py) |
| **(index-level item)** Fidelity architecture / presentation breadth — motion smoothing and frame limit ship off/original by default, so the retained-interpolation machinery is inert on defaults; remaining work is audible/loopback DAC qualification and the rest of the platform/device matrix. No subsystem section exists for this item; it is tracked at the index level | [§ Still open](README.md#still-open) |


## FIXED: three effects drew zero pixels, because hand-packed triangles were built in N64 byte order

`Triangle.vertices` and `TexCoords.texCoords` are `u32` **union aliases** over the
byte struct `{flags, vi0, vi1, vi2}` and the s16 pair `{u, v}`. Runtime-built
geometry packed those words the way the N64 does — high byte first. On a
little-endian host the bytes land in the wrong fields:
`DKR_TRIANGLE(0x40, 0, 1, 3)` = `0x40000103` stores as `03 01 00 40`, i.e.
`flags = 0x03` (`BACKFACE_DRAW` lost), indices 1, 0 and **64** out of a
four-vertex batch, and U transposed with V. Every such triangle is
backface-culled, degenerate and out of batch, so it emits nothing.

**Why nothing caught it.** The canonical in-memory layout for ROM-sourced
triangles is already host-natural — `platform/asset_swap.c`'s `swap_triangles()`
deliberately leaves the four index/flag bytes alone and byte-swaps each UV s16 in
place (see `asset_swap_notes.md` decision 6) — so disk geometry read correctly
through the named fields and only the hand-packed words were wrong. All three
victims bound their model, built geometry and submitted a draw; the traces were
green and the scene metrics were green, because what they painted was nothing:

- the giant character portraits in **both collection arenas**
  (`BHV_CHARACTER_FLAG`, Fire Mountain and Smokey Castle);
- the **boost shockwave plume** (`objects.c func_8000B38C`);
- the **entirety of Star City's rainfall** (`weather.c`).

**Fix:** `DKR_TRIANGLE`/`DKR_TEXCOORDS` in `game/include/structs.h` pack in the
**host's** byte order, gated on `__BYTE_ORDER__`, so a big-endian build keeps the
original packing bit-for-bit. The two call sites that hand-packed words —
`object_functions.c`'s character-flag quad and `objects.c`'s rim — now name their
halves instead of shifting a composite.

**Verification:** `tests/check_challenge_modes.py` gains a paired pixel witness on
both arenas and both backends. The arena runs twice, identical except for
`MDKR_SUPPRESS_PORTRAITS` (a presentation-only test hook in `objects.c`), and the
two framebuffers must **differ** while the gameplay traces stay byte-identical.
Restoring the stock packing fails it with exactly zero differing pixels while
every earlier assertion in the suite still passes — which is the point: nothing
that existed before could see a draw that paints nothing. `MDKR_SUPPRESS_PORTRAITS`
is documented in `DEVELOPER_HANDBOOK.md`.

`check_world_fx_capture.py`'s caster census re-baselines with its mechanism
recorded: the eighth-place racing-line correction changes which AI enters the
light view on the pinned route, adding one racer's body and wheels to the dynamic
set. The delta reproduces exactly across three runs on both backends, static
stays level-scoped and unchanged, and the renderer commits measure a zero census
delta in isolation.

## FIXED: PAL was mapped as if it were NTSC — a fixed 240-line surface and no clip-ratio overdraw

Two independent defects, both of them "the host is not the RSP", both PAL-only.

**1. The 2D mapping divided by a hardcoded 240.** NTSC and MPAL compose against
320x240, but PAL raises the framebuffer to 320x264 (`video.c` adds
`PAL_HEIGHT_DIFFERENCE` to every mode) and the game lays its menus out over all
264 rows — `gTrackSelectViewportY`, the ortho viewport's `vtrans`, the PAL text
offsets and the full-surface scissor are all in that taller space. Dividing them
by 240 magnifies PAL 2D art by 264/240 and walks everything below the vertical
centre off the bottom of the surface. The mapping now tracks the surface
`video_init` actually publishes, through `gfx_dkr_set_logical_surface()`.

**2. The last four logical columns were the frame clear, not the scene.** On the
N64 the viewport is only the NDC→screen affine map; triangles are clipped against
the clip volume scaled by `gSPClipRatio`, and the RDP scissor is what bounds the
image. DKR's `dRspInit` sets `FRUSTRATIO_2`, so geometry survives to twice the
viewport extent. A host GPU clips at NDC ±1, which *is* the viewport. PAL is
exactly the case where that matters: `camera.c`'s `viewport_scissor_set()` shifts
the one-player viewport centre four logical pixels left on PAL while leaving the
scissor at the full 320x264 surface, so logical columns 316..319 sit outside the
viewport. Hardware fills them from the ratio-2 overdraw — an ares PAL capture of
the Ancient Lake start line shows scene content in every visible framebuffer
column — while the port showed the frame-clear colour there, a flat band 24 host
pixels wide at 1920 that changed hue every frame.

`dkr_update_clip_expansion()` widens the rectangle handed to the backend until it
covers the scissor, capped at the ratio-2 box the RSP would really have clipped
to, and folds the difference back into clip space so the world-to-window mapping
is the identical affine transform (`window_x = Vx + (ndc+1)/2 * Vw` expanded to
`Ex + (ndc*Vw/Ew + bias_x + 1)/2 * Ew`). **An overhang counts only at a full
authored pixel:** the game states both rectangles in whole 320x240/264 units, so
on hardware they overlap exactly or differ by whole columns, and a thinner gap is
this port's own `lroundf` rounding on two separately scaled rectangles. Without
that floor the framed menu world views — which land half an authored pixel inside
their scissor on NTSC too — would move for nothing. NTSC output is byte-identical,
proven by `cmp`.

**Also in this wave.** DKR states its full-surface clip as
`gDPSetScissor(0, 0, 0, w-1, h-1)`; `rcp_dkr.c`'s own source comments call the
`-1` an unnecessary fill-mode habit. Scaled to the host that discarded
`region.width / 320` columns — a hard-edged six-pixel band of stale pixels down
the right edge at 1920, which the widescreen background tiles then painted. A
clip reaching **all four** surface bounds within one logical pixel now snaps to
the region, so a split-screen or menu sub-rectangle that happens to touch one
bound keeps its authored inset exactly. The secondary-viewport backing fill
restores the presentation world region, and track-select overlay suppression
applies only where decorative gutters actually exist.

**Verification:** `check_framed_world_views.py` asserts the decorative gutters
directly — the old side-band box contained no gutter pixels — and gains PAL arms;
both directions proven by mutation. NTSC track-select output is pixel-identical.

## FIXED: `G_VTX_APPEND` is a base, not a running cursor — the floating intro shrubs

The F3DDKR RSP keeps **one** count: the length of the last flag-0 load. A flag-0
load writes at the start of the vertex array and stores its count; every flag-1
load writes immediately after *that*. Two consecutive appends therefore land on
the same base and the second overwrites the first. The HLE treated the append
destination as a running cursor and advanced it per append.

`sprite_init_frame()` depends on the real contract. A sprite frame wider than one
RSP batch is emitted in runs of five quads: each run issues its own appended
20-vertex load and then restarts its triangle indices at vertex 1. With the base
advancing, run 2 landed at slot 21 while its triangles still named slots 1..4, so
every sprite with a sixth tile drew that tile's texture over the **first** tile's
quad and never drew it in its own place. The intro shrubs gained a duplicate
crown tile and lost their bottom band, which left them hanging above the ground.

**Blast radius, measured rather than assumed:** exactly two six-tile sprite frames
in the whole ROM — the intro shrubs, and one burst frame no route loads.

**Fix:** `rsp.vtx_append_pos` is assigned only on a flag-0 load
(`gfx_pc_dkr.c dkr_sp_vertex`). Hardware reference from the ares oracle confirms
the fixed frames match authored intent.

**Verification:** the new registered `tests/check_intro_shrub_sprite.py` scores the
deterministic intro frame's crown, stem and body regions on both backends;
`check_sprite_layout.py` classifies multi-run frames and pins that census. Both
are mutation-proven. A failed tile bind now skips the draw with a one-shot
diagnostic instead of silently painting the previously bound texture at 1x1 scale.

The remaining F3DDKR interpreter contracts audited alongside it — vertex and
polygon encodings, triangle index base, billboard slot, matrix select, movewords,
the texture command, cull sense and tile lifetimes — are each verified against the
microcode contract with the evidence recorded beside the code, including the exact
change required should the cull-sense invariant ever separate from the flip state.

## FIXED: the cascade light-depth axis pointed at the sun — wave "shadowsign"

**Repro-sweep verdict for the v0.4 "random shadows from random objects" report:
REPRODUCES at tip.** Post-1.0 R2 re-ran it because the world-depth epic landed
after the report and nobody had re-measured. Sweep: Remastered, GL, autopilot to
frame 3350–3590 on six worlds (Ancient Lake 5, Whale Bay 8, Everfrost Peak 13,
Greenwood Village 19, Snowball Valley 9, and 31), shadow-on vs shadow-off frame
pairs, `MDKR_WORLD_FX_TRACE` census on every arm.

What the sweep found was not the caster feed. Provenance was clean everywhere:
`staleCasters=0`, `implausible=0`, `allocFails=0`, identical `[WORLD-FX]`
censuses between arms on all six worlds — the "shadowplay"/"shadowdeep" phantom
classes really are closed. The symptom is downstream, in the one line that
decides which way depth grows:

**`gfx_shadow_cascade.c` built `world_to_clip` row 2 from `+light_axis_z`.**
`sun_direction_world` points TOWARDS the sun — it is the same vector the RL-5
receiver dots against a surface normal, where a +Y normal must be lit by a +Y
sun. Projecting depth along it made the depth buffer grow *towards* the light,
so with both backends' shadow pass at "keep the smallest depth" (GL_LESS /
`WGPUCompareFunction_Less`, clear 1.0) every cascade stored the surface
**furthest** from the sun in each column — the ground — and the receiver's
LESS_EQUAL compare then lit exactly that surface and shadowed everything
standing on it.

Player-visible consequences, all four of which are the report:

- Every kart, character and standing object was uniformly at full umbra in open
  sunlight. Everfrost frame 3470, driver's torso: 83.57 mean with shadows on
  against 123.04 with them off — a flat 32%, the umbra applied whole.
- Nothing ever cast onto the ground. Same frame, track surface beside the kart:
  **97.37 in both arms, to the digit.**
- And because the actor-decal handoff had already suppressed the blob shadow
  once the maps were ready, karts lost their only grounding and floated.
- Large away-from-sun faces (cliff walls, the Ancient Lake shore rock) read as
  hard-edged full-face blackouts with no visible caster — which is what "random
  shadows … that don't seem to make sense in places" describes.

Acne was ruled out before the sign was touched, not after: raising the
comparison bias 25x (`MDKR_SHADOW_BIAS=200`) moved that torso only to 94.61.
Nothing about a bias explains a receiver occluded by the ground it stands on.

Fix: negate row 2 and its translation term, so depth increases away from the
light. One sign, no new state. It also restores the winding both backends'
shadow pass already assumes — they cull FRONT faces deliberately (second-depth
shadow mapping, the standard acne mitigation), and mapping a right-handed light
basis onto GL clip space with `+z` had inverted which faces those were.

Evidence: on Ancient Lake frame 3400 the shadow-only footprint falls 60138 →
9071 changed pixels and the opposite-channel "mixed" class 429 → 1; frame 3460,
45111 → 23232 and 5985 → 5; karts now cast visible kart-shaped shadows onto the
track and are no longer darkened themselves. `check_world_shadows` passes on
both backends with near-identical footprints (GL 20692, WebGPU 20725) including
the forced-loss fallback latch; `check_shadow_stage_reset`,
`check_shadow_visual_ab`, `check_remaster_lighting`, `check_video_options`,
`check_video_presets` and all 33 CTests pass. New registered gate
`check_shadow_plausibility.py` covers the property from both ends (see
tests/README.md).

**Threshold changed, deliberately:** `check_world_shadows.py` capped the mixed
class at 1% of `changed`. `changed` is the size of the shadow handoff, which is
exactly what this fix moves — the handoff halved (40540 → 20692) while the mixed
class itself *fell* (368 → 263), so a strict improvement failed a ratio against
a shrinking denominator. That is DEVELOPER_HANDBOOK §3's twelfth shape living in
a threshold instead of a fixture. It is now a fraction of the frame
(`pixel_count // 500`), which states "never a broad hue-shift region" directly
and still rejects the pre-fix worst frame measured on this route (5985 mixed,
1.9% of the frame).

## OPEN: shadow strength is one preset-wide choice, not a per-surface one

Filed at R2 as the settings-UX residual. `Video.WorldShadows` now exists
(off/soft/full, LIVE, defaulting to full so the shipped image is unchanged), and
it answers the "remaster shadows degrade the UX" half of the v0.4 report at the
level the report was made — but it is still one global umbra multiplier.

What it cannot express, and what a future pass should measure before adding
knobs for:

1. **The receiver has no N·L term.** A surface facing away from the sun is
   already unlit by RL-5 and is then multiplied by the umbra as well, so
   away-from-sun faces darken twice. It is *defensible* — a surface facing away
   from the light is in shadow — but it is why large flat faces read heavier
   than the shadows cast onto them, and no measurement separates the two today.
2. **The bias is not slope-scaled.** `g_pcSunShadowBias` is a constant 8 world
   units divided by the plan's light z-span. Grazing-angle receivers therefore
   get the same margin as face-on ones. The seam to measure this is now in place
   (`MDKR_SHADOW_BIAS`, `MDKR_SHADOW_UMBRA`, raw float env overrides).
3. **DKR's vertex colour bakes its own occlusion**, so *any* umbra is a second
   multiply on art that already has one. `soft` (0.78) exists because 0.62
   exists because 0.48 was too heavy; none of the three came from a measurement
   against a reference image, only from playthrough judgement.

Not fixed here because each needs a measurement this wave did not take, and
because the exit gate for R2 was the plausibility property, not the aesthetic.

**Deliberately deferred.** World shadows only exist under the Remastered
preset: the shadow feed is gated on `g_pcRemasterFX`, which is zero in both
Pure and Restored, and the shipping default preset is Restored. A
default-configuration player never sees a pixel this item describes. Within
Remastered, `Video.WorldShadows` (off/soft/full, live, default full) answers
the original "shadows degrade the UX" report at the level it was made. The
residual is honest and specific: the receiver has no N·L term, so
away-from-sun faces darken twice; the bias is one span-derived scalar rather
than slope-scaled; and the umbra values came from playthrough judgement, not
from a measurement against a reference image. Fixing this properly means
establishing a reference to measure against — which for an opt-in remaster
preset with no authored ground truth is a design exercise, not a bug fix. The
`MDKR_SHADOW_BIAS` and `MDKR_SHADOW_UMBRA` seams exist so that when a
reference is chosen the measurement is a parameter sweep rather than a
rewrite.

## OPEN: shadow gate trustworthiness — harness environment has diverged from the shipping build twice

Not a rendering defect; a defect in how the shadow gates themselves prove
what they claim. Both instances are already fixed at the source, but the
underlying *class* — a gate whose harness environment differs from the
shipping build can go green over a real defect — is still open, because
nothing yet forces every shadow gate to also run in the exact configuration a
player ships with.

1. **Wave "shadowplay":** the four root causes below (phantom static casters,
   hash-invented sun heading, ungrounded billboard pickups, and a
   double-darkening umbra) were found only by a manual v0.4 playthrough while
   `check_world_shadows.py` and its siblings passed the whole time. The
   automated gates were exercising real shadow code but not the specific
   configurations/geometry the report hit.
2. **Wave "shadowdeep" R1:** *"every shadow gate exported `MDKR_TRACE=1`,
   which is why none saw it"* — the only `gfx_shadow_stage_begin()` caller
   sat inside `level_load()`'s `mdkr_resource_trace_enabled()` block, so the
   static caster cache was never reset in a shipping build (no
   `MDKR_TRACE`), but every registered shadow check set that variable and so
   never observed the un-reset path. A shipping-build player hit this; every
   gate that ran it was blind to it by construction.

**Deliberately deferred, not fixed here.** Both underlying bugs are fixed —
`check_shadow_stage_reset.py` now holds shipping and traced arms to the same
terminal static-caster census, closing R1 specifically. What is still open is
the *general* fix: an audit of every gate that exports a diagnostic env var
the shipping build does not set, plus a shipping-configuration arm (no
`MDKR_TRACE`) added to `check_world_shadows.py` and its siblings so the
harness environment can never again diverge from what players run. That is a
gate-infrastructure change, not a rendering one, and it touches every gate in
the renderer suite rather than one file — out of scope for a docs-only pass.

## FIXED: v0.4 playthrough shadow defects — wave "shadowplay"

The first real v0.4 playthrough reported "random shadows from random objects
projecting into the world that don't seem to make sense in places" and that
the remaster shadows "kinda degrade the UX". Reproduced headlessly on Ancient
Lake (`MDKR_WORLD_SHADOW` A/B frame dumps, GL and WebGPU): a hard diagonal
band with dither striping across the start-area stone pillars, a full-face
blackout of the green shore rock, and pickup bananas floating after their
blob decal vanished. Four root causes, each with a mechanical witness:

1. **Planner depth range ignored cached casters** (`gfx_shadow_frame.c`,
   `gfx_shadow_cascade.c`). Static casters are correctly cached for the whole
   stage — off-screen geometry must keep casting — but the cascade planner
   extended its light z-range only over triangles observed *that frame*
   (`view->bounds_*`). Cached casters the game CPU-culled fell outside the
   range; GL's process-global `GL_DEPTH_CLAMP` pancaked them onto the light
   near plane as full-strength phantoms while WebGPU (no `unclippedDepth` on
   the shadow pipeline) clipped them, so the backends also disagreed. The
   stage cache now accumulates a world-space AABB at admission and
   `gfx_shadow_capture_commit()` folds it into every committed view's bounds.
2. **Hash-invented sun azimuth** (`gfx_level_lighting.c`). Worlds with no
   weather drift or sky scroll derived their azimuth from a level-identity
   hash (`source=0x4x` in every `level_light:` trace row of the reproduction).
   Harmless when the sun only fed 10–16% directional lighting; plainly wrong
   once cascaded shadows drew it on the ground. Uncued worlds now share the
   canonical `(0.35, _, 0.45)` key-light heading; sky-derived elevation and
   authored-signal worlds unchanged.
3. **Billboard-sprite actors lost their only grounding** (`tracks.c` decal
   mark). The actor-decal handoff suppressed the blob for every non-scenery
   batch once the map was ready, but billboard sprites are excluded from the
   caster feed by design, so bananas/balloons floated. Only
   `OBJECT_MODEL_TYPE_3D_MODEL` actors now trade decal for map shadow.
4. **Umbra 0.48 double-darkened baked art** (`video_config_runtime.c`).
   DKR's vertex colour already encodes authored occlusion; multiplying
   shadowed pixels to 48% read as splotches. Now 0.62.

Evidence: `level_lighting`/`shadow_frame`/`shadow_cascade` CTests and all 28
Debug CTests pass; `check_world_shadows` passes on GL and WebGPU with
near-identical footprints (8536 vs 8526 changed pixels) including forced-loss
fallback/latch; `check_remaster_lighting` and `check_world_fx_capture` pass;
before/after captures show the pillar band and striping gone.

**Residual, recorded not fixed at this wave:** the static caster cache keys on
raw arena `Triangle` addresses (`static_key_contains`) and nothing invalidates
entries when arena memory is freed mid-stage. **Superseded:** wave "shadowdeep"
found two live instances of exactly this class (the void curtain's per-frame
rewritten fixed buffers, and cross-level address recycling once the missing
stage reset was exposed) and closed them; see the next section.

## FIXED: pre-release shadow deep review — wave "shadowdeep"

The pre-release renderer review after wave "shadowplay" found that the
address-keyed static
cache residual was not hypothetical — it was live in two forms, and the wave
also closed the remaining player-visible shadow findings:

1. **Shipping builds never reset the static caster cache** (R1). The only
   `gfx_shadow_stage_begin()` caller sat inside `level_load()`'s
   `mdkr_resource_trace_enabled()` block, so without `MDKR_TRACE` every
   previous level's static geometry kept casting into the next level and
   recycled arena addresses false-dedup'ed new geometry out of the map. Every
   shadow gate exported `MDKR_TRACE=1`, which is why none saw it. The reset is
   now unconditional at level load, and the new registered
   `check_shadow_stage_reset.py` holds shipping and traced arms to an
   identical terminal `[WORLD-FX] static=` census (427 on the Ancient Lake
   route) with a suppressed-reset positive control that must grow (measured
   694).
2. **The void curtain froze into the cache as a phantom wall** (R2). Runtime
   full-height void geometry draws under the static world-origin slot from two
   fixed per-viewport buffers rewritten every frame; the address-keyed cache
   froze frames 1–2's camera-relative curtain as a permanent caster. A new
   DL-build-time caster-exclusion seam (mirroring the projected-decal marks)
   now excludes the void, all wavegen water/lava surfaces (which draw with an
   opaque render mode on Hot Top Volcano, Trophy Race, and split-screen), and
   every level batch the ROM flags `RENDER_NO_SHADOW`.
3. **Stage-AABB hardening** (R4): the AABB seeds from its first admission
   (no zero-bound leak after shutdown), and finite-but-implausible vertices
   (|coord| > 250k) are rejected with an `implausible_triangles` counter
   before they can collapse depth resolution for the rest of a stage.
4. **World-unit shadow bias** (R5): the receivers now divide an authored
   world-unit bias (8.0) by each plan's light z-span instead of applying a
   constant NDC fraction whose world magnitude silently scaled with however
   much caster depth the stage spanned. Masked ranges no longer extend caster
   bounds (they are classified but never drawn into the maps).
5. **Backend parity and polish**: the WebGPU shadow pipeline sets
   `unclippedDepth` like every other pipeline (GL/WebGPU pancake-vs-clip
   asymmetry closed at the structural level, R7); both receivers fade to lit
   over the outer 8% of the map footprint instead of a hard sliding far
   terminator (R8); translucent 3D-model actors keep their projected decal
   (they are never captured as casters, R9); and static admissions after the
   published watermark merge into shared ranges instead of one draw call per
   cached triangle (R10) — published range counts remain immutable, and the
   unit test proves both properties.
6. Cleanups: the dead level-identity hash is gone from the lighting derivation
   (`SOURCE_HASH` now documents "invented heading"), and the GL shadow
   perma-fail message no longer tells players to change a setting that nothing
   reads (R16).

Recorded, deliberately not fixed here: shadow-pass front-face culling on
open-shell ROM terrain (R12, needs a `flags` histogram + art review), the
receiver `world_valid` asymmetry (R13, counter first), the structural
one-frame caster lag (R14), decal receivers excluded from map shadows by
authored intent (R15), and GL DEPTH24 vs WebGPU Depth32Float precision
divergence. See the review report's P2 table.

## FIXED: restoration/remaster sprint — sprite bounds, RDP gradients, SDF text, moving mips, and RL-1

Two correctness bugs and three remaster decisions shared one rule: each path
needed an explicit ownership or measurement boundary instead of an inference
from a plausible image.

### Exact sprite records

The sprite builder mixed N64-width constants, native pointer arrays, frame
counts, and alignment, while four separate readers decoded the flexible record.
Some legal records therefore needed more space than the historical expression
reserved. The shared
`sprite_build_layout()` path now validates the serialized frame-asset
record and computes every native region with checked arithmetic. The census
normalizes all three ROM byte orders and recognizes both supported US/EU v80
layouts; the available US v80 ROM has 193 sprites and a maximum 48-byte record.
Sprite 162 emits 29 `Gfx` commands where the old formula reserved 27 (16 bytes
short), and sprite 177 emits 67 where it reserved 66 (8 bytes short). Unit cases
cover malformed/truncated records, overflow, alignment, and failure cleanup; the
ROM-backed gate proves every production loader uses the shared layout.

### Screen-linear RDP gradients

Shade and fog attributes were carried through the generic perspective-correct
varying path. DKR's RDP setup expects screen-linear interpolation for shaders
with `SHADER_OPT_NOPERSPECTIVE_INPUTS`, and a vertex created by near-plane
clipping needs fog derived from its new clip-space position rather than a blend
of two already-derived scalar fog values. The renderer now selects interpolation
from the shader options and recomputes clipped fog. The positive control yields
fog 60 under the old rule and 28 under the corrected one. GL and WebGPU change
roughly 954k of 1.229m pixels with component mean absolute difference 10.53,
while 2,900 `[PACE]` rows remain identical. `MDKR_RDP_GRADIENTS=legacy` is an
exact diagnostic control.

### Runtime-derived SDF text

Fonts now have explicit registration lifetimes at the `ASSET_FONTS` load/free
boundary. The renderer derives a 4x region-isolated coverage field in memory,
clamps each glyph to its registered cell, uploads it directly, and invalidates
derived entries when the source atlas dies. It writes no files and ships no
derived asset. The feature is reachable only when `RemasterFX` is enabled:
Pure and Restored remain byte-identical to the disabled control on GL and
WebGPU, while Remastered changes only known text regions. Actual Chromium
reports 52 uploads, zero registry failures, and zero stale cache hits over 3,600
frames. `check_no_rom.sh` rejects derived-font filenames and its deliberate
positive-control artifact.

### Moving mip proof and RL-1

Everfrost Peak finally supplies the missing moving-camera evidence for M1:
over 24 consecutive frames and 271.3 world units, mipmaps reduce temporal
second-difference energy 10.25% on GL and 10.04% on WebGPU with anisotropy fixed
at one. The enabled arms build 854 chains / 4,881 levels, controls build zero,
and all 3,500 timing rows match.

RL-1 compares baked colour, baked colour plus a fixed diagnostic sun, and full
supersession on Ancient Lake and Fire Mountain. Supersession removes too much of
the authored mood: Fire Mountain moves from luma/saturation 111.97/155.23 to
144.55/136.14, while retaining the base stays at 114.80/145.04. Ancient Lake
shows the same direction, and supersession is 2.5–3.5x farther from the baked
captures. The production decision is therefore to retain baked colour as the
low-frequency ambient base. At this restoration checkpoint RL-2 remained open
because the diagnostic light also revealed coarse per-triangle facets.
Wave 2 subsequently closed it with real compact model-normal streams and
level-derived lighting; `MDKR_RL1_ARM` remains evidence only, not a published
setting.

Debug, Release, and ASan builds and all 12 CTests pass. Fresh wasm passes the
ROM-absence gate, and the real Chromium runtime reaches the race at median 60
fps with exact persistence and zero ROM upload. The registered manifest expands
to 38 scripts / 45 tasks.

## FIXED: widescreen world billboards retained the N64's 4:3 stretch — wave "billboardaspect"

**Refinement requirement (playtest):** widescreen/FOV must not stretch balloons or
other assets; everything should remain correctly proportioned.

### What the original widescreen gates missed

The landed display policy already handled two transform classes correctly:

- perspective geometry uses an aspect-correct projection;
- HUD/menu art maps uniformly into a centered 4:3 SAFE_2D rectangle.

Regular world sprites use neither complete path. F3DDKR first projects one anchor
vertex with the current world matrix, then transforms each sprite-local vertex
only by matrix slot 2 and adds its X/Y directly to the anchor in clip space
(`platform/fast3d/gfx_pc_dkr.c:dkr_sp_vertex`). Those local offsets never pass
through the perspective matrix.

`render_sprite_billboard()` built slot 2 with the retail
`gVideoAspectRatio`—4:3 for the supported NTSC ROM. That value compensates the
original 320×240 viewport. On a viewport with physical aspect `A`, clip X scales
by width while clip Y still scales for 4:3, so the sprite widens by:

```text
A / (4 / 3)
```

This is exactly the kind of defect the broad checks could miss:
`check_widescreen_shadow.py` asserts simulation state, the display CTest covered
projection/layout math, and the two-player gate scores liveness. None measured
an actual world billboard's internal proportions.

### Failing measurement

A deterministic Timber's Island route puts a golden collectible balloon ahead
of the racer at frame 6300 and collects it at frame 6331. Equal-height GL dumps
and a saturated-blue connected-component measurement over the balloon's zigzag
gave:

| pre-fix arm | motif bounds | aspect vs 4:3 |
|---|---:|---:|
| 4:3 | 68×27 | 1.000 |
| 16:9 | 93×27 | 1.368 |
| 21:9 | 121×27 | 1.780 |
| 16:9, FOV 75° | 93×26 | 1.376 |

The HUD balloon in the same frame retained its proportions in every production
arm, confirming the defect was the world-billboard path rather than texture
decode or the safe-area policy. The 21:9 factor also independently matches the
analytic legacy stretch, 1.75 within rasterization tolerance.

### Why the correction is applied to matrix output columns

A first upright-sprite correction can be made by changing
`mtxf_billboard(..., scaleY)` to the host aspect. That fixes shape but makes both
sprite dimensions grow with viewport width. It also does not preserve an
arbitrarily rolled sprite: the retail billboard builder applies its non-uniform
term to only one matrix element, so replacing the argument changes the original
off-diagonal transform.

Commit `fa7adcc` instead:

1. builds the complete retail billboard matrix unchanged;
2. calculates the active viewport/lens correction in the ROM-free display
   module;
3. scales the matrix's complete X output column by
   `clipY * authoredTVAspect / effectiveViewportAspect`;
4. scales the complete Y output column by
   `clipY`, where
   `clipY = tan(authoredVFOV/2) / tan(effectiveVFOV/2)`.

This maps every rotated local vector to the same physical-pixel transform as the
authored viewport, not only an upright axis. Ordinary Hor+ has `clipY = 1`, so
equal-height 4:3 and 16:9 output retain identical sprite size. The 104° cap and a
user FOV scale billboards with the perspective world rather than leaving them at
a fixed overlay-like size. Exact legacy mode omits the aspect term and therefore
keeps the former distortion on purpose.

Vehicle-part sprites already traverse a complete model/view/projection matrix.
The only other `gDkrEnableBillboard` producer is
`render_ortho_triangle_image()`, which is in SAFE_2D and was already
distortion-free. The source census therefore closes the class, not only the
reported balloon instance.

### Both-direction regression gate

`tests/check_widescreen_proportions.py` runs seven isolated arms through the HUD
at frame 6300 and the closer world balloon at frame 6410. Current Release GL and
WebGPU produce identical measurements:

| fixed arm | HUD motif | world motif | expected world scale |
|---|---:|---:|---:|
| 4:3 | 99×36 | 48×18 | 1.000 |
| 16:10 | 99×36 | 48×18 | 1.000 |
| 16:9 | 99×36 | 48×18 | 1.000 |
| 21:9, cap 104° | 99×36 | 50×19 | 1.053 |
| 21:9, forced 4:3 | 99×36 | 48×18 | 1.000 |
| 16:9, FOV 75° | 99×36 | 36×14 | 0.752 |
| 21:9 exact legacy | 173×36 | 84×18 | known-bad 1.75× X |

Production motif aspect remains within 3.2% of the 4:3 reference and both axes
track the analytic lens scale. The legacy arm must exceed the same production
tolerance, so the detector proves it can reject the former bug. Every arm also
requires the same normalized frame-6410 racer state and the same frame-6476
collection event.

The original five arms passed Debug GL, Debug WebGPU, Release WebGPU, and ASan
GL; the seven-arm refinement passes Debug GL, Release WebGPU, and ASan GL.
ROM-free CTest covers isotropic 1P/quadrant/half-height/16:10/portrait projection,
arbitrary rotated
billboard-column preservation, the horizontal cap, gameplay FOV, safe/full-bleed
uniformity, and exact legacy factors. Both 21:9 two-player renderer runs, clean
wasm compilation, linked-wasm structure, and the real Chromium three-resize
runtime pass after the change.
The complete Release-led manifest passes all 37/37 tasks in 7m53s at this
checkpoint.

## FIXED: the filename renderer read beyond a one-byte global — wave "filename-cstr"

`filename_render()` handed `&gCurFilenameCharBeingDrawn` to `draw_text()` even
though the symbol was declared as one byte. The font scanner consequently read
into the following global until linker layout happened to provide a zero.
This is ordinary first-session play: the `adventure_hub_drive` route reaches the
new-save character grid at frame 2052, where ASan reported a global-buffer-overflow.

All retail symbol maps size this symbol at four bytes. It is now a four-byte,
zero-initialized character buffer, with a compile-time size lock; the renderer
updates byte zero and explicitly restores byte one as the C terminator. This
preserves the ROM layout as well as the C-string contract.

`tests/check_filename_entry.py` is the permanent gate. It runs in an isolated
temporary save directory and requires the filename screen to be reached. Its
positive-control mode was run against the preserved pre-fix ASan binary
(`sha256 0b84565fe12e013f9c37e0a03427d28addb1b940b96d7c97ecbd5bb722526cd2`)
and requires the exact sanitizer report naming this symbol and the font scan.
That control fails at frame 2052; the fixed ASan, Debug, and Release binaries all
reach frame 2300 cleanly. The wasm object exports the symbol at exactly four bytes,
and the browser-visible-table check still passes.

The transferable scalar-byte-as-string defect shape is recorded in
[`MGB64_BACKFLOW.md`](../MGB64_BACKFLOW.md).

## FIXED: banana sparkle sprite overran its own vertex region

The banana-counter sparkle was not a blending or widescreen artifact. Sprite
162's seven frames emit 29 display-list commands, but `tex_load_sprite()`
reserved only 27. The last two commands landed in the following vertex region,
so the sparkle quad acquired a long bright corner that appeared as a subtle
screen-height streak. Sprite 177 (the lava spurt) was independently short by one
command.

The allocator now derives one exact layout from the sprite's frame table. A
frame with `n` tiles requires `2 + 2n + ceil(n/5)` commands: pipe sync and end,
one texture command and polygon command per tile, and one vertex load per group
of five tiles. Header/frame pointers, triangles, commands, vertices, and texture
pointers are independently aligned; every add is overflow-checked. The builder
receives explicit end pointers, refuses malformed offsets or an insufficient
region before writing, and requires all three output cursors to finish at their
independently calculated ends. Failure unwinds every loaded texture and the
sprite allocation. The cache now rejects its 101st live sprite before changing
the count or loading anything, and every provisional-count failure rolls back,
closing the sprite-specific part of audit item MEM-11 as well.

A US 1.1 asset sweep found only sprites 162 and 177 under-allocated by the old
formula; 129 sprites were exact fits, which explains why this survived broad
play. A controlled Pure-mode binary A/B drove the same race and produced
byte-identical frames except for ten banana-collection sparkle frames
(3251–3254 and 3286–3291). In those frames the old build contains the long
streak; the fixed build retains the intended sparkle without the corrupt tail.
The same route is clean under ASan.

## FIXED: near-clipped fog caused the moving lower-screen shadow

The subtle dark/bright region covering roughly the lower third while driving was
not the kart's projected shadow. Primitive provenance identified large fogged
road/sand triangles crossing the camera near plane. The CPU clipper creates new
vertices in homogeneous space, but it used to obtain each new vertex's fog byte
by linearly blending the endpoint fog bytes. Those bytes had already been
computed from `z/w` and clamped, so blending them in clip-space `t` was
mathematically invalid. The GPU then perspective-corrected the fog varying a
second time even though RDP fog is a screen-space coefficient.

The clipper now recomputes generated fog from the new vertex's actual `z/w`, and
fog shaders use the existing backend-neutral no-perspective flag. A four-arm
WebGPU capture isolated both halves: legacy clipping plus legacy GPU
interpolation reproduces the dark foreground; either correction removes the
gross shadow; both together implement the RDP contract. The later restoration
sprint generalized the shader contract to screen-linear RDP shade as well;
texture coordinates retain perspective correction. The combined fix keeps the
nearby road and sand consistently lit across adjacent moving frames on GL and
WebGPU.

## FIXED: supersampling could not improve source-resolution font contours

Whole-scene supersampling only repeats the ROM font atlas texels before its
resolve, so menu, dialogue, HUD, and filename text remained blocky at 2x or 4x.
This was a source-sampling limitation, not a broken supersample resolve.

Remastered mode now identifies font textures and their exact glyph cells at
`load_font()` rather than guessing from texture dimensions. At first text use it
reconstructs a 4x coverage atlas from each registered ROM alpha contour with a
bounded signed-distance transform and alpha-weighted colour sampling. Each
glyph is isolated to its own cell, so a neighbouring atlas entry cannot bleed
into its contour. The GPU texture is 4x larger while the renderer retains the
logical N64 dimensions, so all existing glyph metrics, kerning, safe-4:3
placement, and authored colours remain unchanged. The prefiltered coverage uses
point, clamped, level-zero sampling to prevent a hardware bilinear tap crossing
cell boundaries; Pure and Restored retain the legacy atlas. Unload unregisters
the source, invalidates any derived cache entry, and then frees the texture.

The ROM-free `font_registry` and `font_sdf` CTests cover lifecycle, bounded
region union, capacity failure, invalid/overflowing dimensions, output bounds,
contour coverage, colour retention, and adjacent-cell isolation. The GL/WebGPU
mode gate reports two derived uploads and changes only the known text rectangle
in Remastered; Pure and Restored are pixel-identical to the disabled control.
The real browser exercises 52 clean SDF uploads over 3,600 frames.

## FIXED: "interlaced" textures decoded scrambled — the odd-row TMEM word swap (wave "lineswap")

Reported from the browser build as *"the golden balloon glyph in the top left looks
corrupted, as do a few other textures — mostly right, just kind of scrambled."*

**It was not a browser bug and not a small one.** It reproduced identically in the
native build on **both** backends (`MDKR_RENDERER=webgpu` and `=gl`, pixel-for-pixel
the same artifact), because the fault is in the shared F3DDKR HLE texture decoder,
not in any `GfxRenderingAPI` implementation. Roughly **30 % of every texture the
game uploads** was affected.

### Mechanism

Real RDP TMEM stores every **odd** row of a texture with the two 32-bit halves of
each 64-bit word exchanged, and the texture-fetch unit un-exchanges them on the way
out. `LOADBLOCK` normally performs that exchange itself at a rate given by its
`dxt` field, so DRAM holds a plain linear image and the two exchanges cancel.

`dxt` is a 1.11 reciprocal — `CALC_DXT(width, b) = ceil(2048 / words_per_line)` —
so it can only express a row length *exactly* when the row is a power-of-two number
of 64-bit words. For every other width libultra offers the `...S` macro family:

> `/* Allow tmem address and render tile to be specified.`
> `   The S at the end means odd lines are already word Swapped */`
> — `game/include/PR/gbi.h:2699`

Those pass **`dxt = 0`** (no load-time exchange) and rely on the *asset* being
pre-swizzled to cancel the fetch-time exchange instead. DKR's asset tool does
exactly that and records it as `TextureHeader.flags` bit **0x04** —
`RENDER_LINE_SWAP`, commented "Interlaced texture" in `game/include/structs.h:120`
and described in `docs/ref/dkr_asset_spec.md` as *"Any non-power-of-two texture that
is loaded from gDPLoadBlock … needs to be interlaced."* `material_init()`
(`game/src/textures_sprites.c:1631`) then picks the `S` macros for those textures.

We have **no TMEM**: `dkr_upload_tile_texture()` walks DRAM rows straight through at
`tile.line_size_bytes`, so the fetch-time exchange never happens for us. A
pre-swizzled asset therefore decoded with every odd row's texel groups transposed.
`dkr_dp_load_block()` had literally thrown the discriminator away —
`platform/fast3d/gfx_pc_dkr.c`, `(void)uls; (void)ult; (void)dxt;`.

### Measurement

An instrumented build dumped every uploaded texture with its tile parameters. Census
of unique uploads, three routes:

| route | total uploads | `dxt == 0` | share |
|---|---|---|---|
| `adventure_hub_drive` (7300 f) | 947 | **306** | 32 % |
| `race_drive_long` (4100 f) | 962 | **286** | 30 % |
| `nav_to_magic_codes` (2200 f) | 284 | **105** | 37 % |

**The shared property is `dxt == 0` itself, i.e. the asset's own 0x04 flag — NOT the
dimensions.** That distinction matters, because "non-power-of-two" is the tempting
heuristic and it is wrong in *both* directions. Measured over the 2193 uploads above:

- 17 of the 697 `dxt == 0` uploads are power-of-two in both dimensions
  (4x1 and 16x16 RGBA32, 128x8 RGBA16) — and they really are pre-swizzled: the
  16x16 RGBA32 one is a flame sprite that goes from a combed grid to a clean flame
  with a bright core.
- 157 of the 1496 `dxt != 0` uploads are non-power-of-two (8x12, 12x28, 16x24,
  20x28, 24x30, 32x19 …) and must be left alone; un-swizzling them scrambles them.

`docs/ref/dkr_asset_spec.md`'s "any non-power-of-two texture … *needs* to be
interlaced" is a requirement on that class, not a characterisation of the set. The
display list's `dxt` is the direct, exact signal, so that is what the fix keys on.

Only three formats appear at all across those routes — **RGBA16**, **RGBA32** and
**IA8** — and all three have affected instances (`dxt == 0` uploads per route,
hub / race / menu): RGBA16 168 / 163 / 42, RGBA32 49 / 38 / 6, IA8 89 / 85 / 57.
No CI4/CI8, IA16, IA4, I4 or I8 texture is loaded by any of them.

Swap granularity was derived from `siz##_LINE_BYTES` (a TMEM word spans
`8 / LINE_BYTES` texels → 4b:16, 8b:8, 16b:4, 32b:4 texels) and then **confirmed
empirically** against the dumps: an offline un-swizzle at that granularity turns
every one of the 306 scrambled hub textures into a clean image, and turns every one
of the 641 `dxt != 0` textures into a scrambled one (the negative control). In
source bytes that is 8 bytes for 4b/8b/16b and 16 for 32b — the same doubling the
existing `G_IM_SIZ_32b_LINE_BYTES == 2` correction already applies to `line_bytes`.

### Fix

`platform/fast3d/gfx_pc_dkr.c`:

- `dkr_dp_load_block()` records `line_swapped = (dxt == 0)` on the TMEM slot;
  `dkr_dp_load_tile()` records `false` (LOADTILE walks the source row by row, so
  DRAM order is plain linear — and DKR never issues one anyway).
- `dkr_upload_tile_texture()` un-swaps odd rows through a scratch row buffer
  (`unswap_row()`) when that flag is set and `height > 1`.
- `line_swapped` joins the texture-cache key, so the same bytes cannot bind a
  decode made under the other rule.
- `MDKR_LINESWAP=off` reproduces the pre-fix decode for A/B measurement, in the
  same style as `MDKR_NEARCLIP=off`. **Verified to reproduce the pre-fix binary's
  frames byte-for-byte** (4/4 sampled frames), which is what makes it usable as the
  regression check's positive control.

No `game/` change: the discriminator is already on the wire in the display list.

Cross-check: with the fix in, all **306** in-engine texture decodes are
byte-identical to the offline un-swizzle of the pre-fix decode, and all **641**
`dxt != 0` textures are byte-identical to before (no collateral).

### What it was visibly breaking

Beyond the reported balloon glyph (28x32 RGBA32 — a comb-edged mess before, a clean
gold balloon with a blue zigzag after):

- **The minimap, in-race and in the hub.** A field of disconnected dashes before; a
  continuous track outline with a legible player arrow and start/finish chequer
  after. This was the single largest visible casualty and it had never been filed.
- Every **palm frond, plant and tree** on Timber's Island and Ancient Lake.
- The **plane's propeller disc** in the frontend flyby (a dithered smear → a clean
  blur).
- The **particle / smoke / pinwheel atlases** (IA8).
- Frontend/menu text was **not** affected — those glyph textures are power-of-two
  and take the `dxt != 0` path (the MAGIC CODES screen is 0 pixels changed).

### Regression check

`tests/check_texture_lineswap.py` (`race_drive_long`, 3900 frames, muted +
headless). It renders the route **twice from the same binary** — normally and with
`MDKR_LINESWAP=off` — and measures the artifact directly: at texel-row spacing
`step = width/320`,

```
parity = mean |I(x,y) - I(x,y+step)| / mean |I(x,y) - I(x,y+2*step)|
```

A clean image is smoother between adjacent rows than between rows two apart
(parity < 1); a swizzled one is rougher (parity > 1). Scored over the
changed-pixel mask (self-targeting, no hand-picked rectangle) and over the minimap
ROI.

| | changed-mask | minimap ROI |
|---|---|---|
| `MDKR_LINESWAP=off` | 1.12 – 2.11 | 1.54 – 2.18 |
| normal | 0.60 – 0.71 | 0.60 – 0.68 |
| ratio | 1.69 – 3.51x | 2.27 – 3.67x |

Because the broken arm is re-rendered on every run, **the check cannot pass
vacuously** — it has to see the artifact and then measure that the normal arm is
better.

The table and pasted output below are the original lineswap checkpoint. After
screen-linear RDP shade restoration, the dynamic mask also spans minified 3D
surfaces where framebuffer-row spacing is not source-texel-row spacing. The
current gate therefore keeps its absolute broken-arm threshold only on the
fixed minimap ROI; the dynamic mask still requires at least 500 changed pixels,
a clean-arm ceiling, and at least 1.4x improvement. Current mask measurements
are 1.01–1.58 to 0.61–0.73 (1.52–2.60x), and the fixed ROI is 1.46–1.58 to
0.62–0.69 (2.12–2.50x). Both arms now name `--pure` explicitly.

**Positive control (both directions, measured).** Reverting *only* the decode
change (`if (line_swapped && (y & 1))` neutered, counter and A/B plumbing left in
place so the failure is attributable to the decode and not to missing
instrumentation) and rebuilding:

```
$ MDKR_AUDIO=0 python3 tests/check_texture_lineswap.py -v      # reverted
  dxt==0 uploads: on=286 off=286
  frame_3400.ppm: changed=     0  mask off=0.000 on=0.000 (0.00x)  minimap off=1.540 on=1.540 (1.00x)
  ...
check_texture_lineswap: FAIL            (exit 1; 12 assertions, 3 per frame)
  - frame_3400.ppm: only 0 pixel(s) differ between the arms (want >= 500) …
  - frame_3400.ppm minimap-roi: un-swizzled arm still looks combed (parity 1.540, want <= 1.0)
  - frame_3400.ppm minimap-roi: parity only improved 1.00x (off 1.540 -> on 1.540, want >= 1.4x)

$ MDKR_AUDIO=0 python3 tests/check_texture_lineswap.py -v      # fix restored
  frame_3400.ppm: changed=  6349  mask off=1.528 on=0.657 (2.33x)  minimap off=1.540 on=0.677 (2.27x)
  frame_3550.ppm: changed=  4837  mask off=1.620 on=0.710 (2.28x)  minimap off=1.904 on=0.665 (2.86x)
  frame_3700.ppm: changed= 11613  mask off=1.121 on=0.665 (1.69x)  minimap off=1.892 on=0.614 (3.08x)
  frame_3850.ppm: changed=  5423  mask off=2.109 on=0.600 (3.51x)  minimap off=2.181 on=0.595 (3.67x)
check_texture_lineswap: PASS            (exit 0)
```

The reported symptom's own route also passes, with the ROI bound relaxed for the
balloon's smaller, flatter ROI (see the check's docstring for the exact command):
mask off 1.25–1.31 / on 0.61–0.64 (1.99–2.08x), balloon ROI off 0.97 / on 0.56–0.64.

No regressions: `check_race_drive.py`, `check_determinism.py` and
`check_adventure_hub.py` all PASS with the fix in.

### Left open (found, evidence recorded, deliberately NOT patched)

1. **The 4-bit un-swizzle granularity is unexercised.** The chunk size is a single
   expression, `(siz == G_IM_SIZ_32b) ? 16 : 8` source bytes, so 4b textures share
   the *proven* 8-byte constant with 8b and 16b — there is no separate 4-bit branch
   to be wrong. What is unverified is that any 4b texture exists at runtime: the
   three routes above load **zero** (`siz == 0`) textures, even though
   `material_init()` has `TEX_FORMAT_CI4`/`IA4`/`I4` arms and `font.c:1516` issues a
   `gDPLoadMultiBlock_4bS`. Same for CI4/CI8 palettes: `dkr_dp_load_tlut()` is never
   reached on these routes. Not patched further, and not asserted on, because
   nothing demonstrates the path is reachable.
2. **`dkr_dp_load_tile()` is dead code for DKR.** No `gDPLoadTile`/`LoadTextureTile`
   call site exists anywhere in `game/src/` — every texture load is a `LOADBLOCK`
   variant. It is left in place (it is correct as written, and the HLE is shared with
   mgb64), with `line_swapped = false` recorded so it can never inherit a stale flag
   from a previous `LOADBLOCK` into the same TMEM slot.
3. **`dxt != 0` accumulator drift is *not* a problem here, but it is real.** With
   `dxt = ceil(2048/wpl)` and a non-power-of-two `wpl`, the RDP's line counter
   drifts by `wpl*dxt - 2048` per row. DKR's own non-interlaced non-power-of-two
   cases stay far inside one line — e.g. 20x28 RGBA32 (`wpl = 5`, `dxt = 410`) drifts
   2/2048 per row, 56/2048 over 28 rows, and the debug `printf` font's 192x11 IA8
   (`wpl = 24`, `dxt = 86`) drifts 176/2048 over 11 rows. Our decoder ignores `dxt`
   magnitude entirely, so it is *immune* to the drift the hardware would show; that
   is a latent divergence from hardware, in our favour, and nothing in DKR exposes
   it. Recorded, not modelled.

## P3.3 MAGIC_CODES / FILE_SELECT text fidelity — wave "p33-text"
Two independent renderer defects, both found by reading the oracle montage's diff
column rather than the score. Neither was in `menu.c`: the menu layout code was
correct all along.

- **Dialogue-box backgrounds never drew — FIXED.** *Mechanism:* outside
  `G_CYC_FILL`/`G_CYC_COPY` a `FILL_RECTANGLE` is rasterized as an ordinary
  screen-space rect — the combiner still runs, but the span carries no texel and no
  shade, so only the combiner's `d` term is meaningful, `(a-b)*c+d` degenerating to
  `d`. `render_dialogue_box` (font.c) relies on exactly that: it selects
  `G_CC_ENVIRONMENT` (`0,0,0,ENVIRONMENT`) plus `gDPSetEnvColor`, then issues plain
  FILLRECTs in **1-cycle** mode. `dkr_dp_fill_rectangle` sourced the flat colour
  from `prim_color` unconditionally. *Measured evidence:* on MAGIC_CODES the traced
  FILLRECTs read `cyc=0 rgb_d=5 alp_d=5` (= ENVIRONMENT) with
  `env=00000080` (black @ alpha 128, exactly
  `set_current_dialogue_background_colour(7,0,0,0,128)`) but `prim=00000000` —
  fully transparent, so the panel vanished; rects `(50,54)-(270,128)` matched
  dialogue box 7's `(50,50,270,132)`. The montage showed the keyboard glyphs
  perfectly aligned over bare sky with the whole dark panel missing. *Fix:*
  `dkr_fillrect_flat_color()` resolves the register the combiner actually names
  (ENVIRONMENT / PRIMITIVE / 1 / 0), keeping `prim_color` as the fallback for terms
  a flat fill cannot supply. *Verification:* MAGIC_CODES **79.3 % → 94.8 %**
  (hist 73.4→91.5, block 85.1→98.1). SAVE_OPTIONS 93.4→94.9 and AUDIO_OPTIONS
  92.0→94.0 improved as a side effect — they draw dialogue-box backgrounds too,
  which is the positive control.

- **WebGPU viewport clamping silently squashed all ortho geometry to 0.75× — FIXED.**
  *Mechanism:* `mtx_ortho` (camera.c) deliberately sets `vp.vscale[1] = width*2`,
  asking for a viewport **320 logical pixels tall on a 240-tall target**
  (`(0,-40,320,320)` logical, `(0,-160,1280,1280)` at 4× HiDPI). GL and Metal keep
  such a transform and merely clip; WebGPU *validates* `setViewport` containment, so
  `wgpu_clamp_rect` trimmed it — and clamping **changes the transform**. The clamp
  cut the half-height from 160 to 120: a uniform **0.75 vertical squash about the
  screen centre**, affecting every ortho pass (wood-panel buttons, HUD triangles,
  fade transitions) while leaving screen-space TEXRECT text correct — which is why
  it read as a "text vs panel" mismatch. *Measured evidence:* FILE_SELECT's wooden
  buttons spanned rows **91–139 (height 48)** against the real ROM's **81–145
  (height 64 = `gFileSelectButtons[].height`)**, with horizontal extents already
  exact (24–112 / 116–204 / 208–296); `y_native = 120 + (y_ares-120)*0.75` fit both
  edges, and the clamp arithmetic reproduces 0.75 exactly. *Fix:*
  `wgpu_viewport_fix()` rasterizes with the clamped (legal) rect but returns the
  clip-space affine that restores the requested transform, applied as
  `x' = x*sx + bx*w`, `y' = y*sy + by*w` to a staged copy of the batch. Geometry the
  rescale pushes outside the NDC cube maps outside the clamped rect (off-target), so
  clipping it is correct. *Verification:* the buttons are now **pixel-exact** —
  native rows `(81,115),(117,145)` == ares on all three. Note the perspective
  viewport was also marginally out of range (`vtrans=(642,474)` → flipped y = −6),
  so DKR's deliberate 2 px/6 px viewport nudge is now honoured too.

- **DEFERRED — FILE_SELECT's residual is the shared island backdrop, not its text.**
  FILE_SELECT ends at **85.2 %** (hist 89.4 / block 81.1), short of the 92 % target,
  but the shortfall is not text or layout. Scoring sub-regions with
  `compare_frames.py`'s own metrics: wooden buttons block **95.5 %**, "GAME SELECT"
  title block **96.0 %**, COPY/ERASE row block **97.1 %**, all UI block **93.7 %** —
  i.e. control-screen quality (OPTIONS block 97.9, CHARACTER_SELECT 97.7). The loss
  is the backdrop: sky bottom band block **65.8 %**. That is a genuine render
  difference, not animation phase — over a 14×14 native×ares frame search **no pair
  exceeds 64.8 %** on the sky band, and the band is near-static in both runners
  (native 147–150, ares 157–167). It is also *shared*, not FILE_SELECT-specific: the
  `title_menu` mark sits at 87.1 % (block 84.8) identically in every route. Separately,
  a uniform **+28.7 luma** sky offset (native 118 vs ares 89.4 in the UI-free left
  strip) appears identically on OPTIONS, which still scores 94.3 % — harmless to the
  block metric, which mean-centres and normalises, so it costs hist only. Fixing the
  backdrop belongs with the P3.2 preview-camera/backdrop work, not P3.3.

## M4.5 WebGPU backend — DONE (qualified fail-closed default). Open notes:

> **Ledger note:** the open-items index no longer carries this section as its
> own "still open" row — every note below already carries its own FIXED/CLOSED
> strikethrough, and `gfx_webgpu_set_stage()` has zero production callers
> (dormant, not open). The one still-live thread, external-platform
> validation, is the same thread [WGPU-11](README.md#still-open) already
> tracks, and this row's remaining breadth is merged there.

- **Post-bring-up failure and loss handling are implemented.** Every
  instance/surface/adapter/device/queue/configure failure is injected by
  `check_webgpu_recovery.py`; native may perform one bounded same-backend device
  rebuild, then stops visibly if WebGPU is still unhealthy. It never changes a
  live process to GL. The browser restores an actionable launcher/error view.
  Surface statuses,
  usage capabilities, featureless depth clipping, and unsupported artifact
  tuples have ROM-free policy gates; remaining external-platform validation is
  recorded in this open-items register.
- **Native GL diagnostics and fail-closed startup are automated.**
  `tests/check_renderer_backends.py` drives GL and WebGPU through the same
  deterministic route into Ancient Lake, compares stable scene samples, and
  positive-controls the comparator against black. It also injects a WebGPU SDL
  window failure and requires the deliberate `EXIT_FAILURE` path with no crash
  marker and no GL initialization. GL is reachable only through explicit
  `MDKR_RENDERER=gl`; Debug and Release both render through that diagnostic
  path.
- ~~**Pipeline-prewarm cache DEFERRED (dormant, by design):**~~ **CLOSED BY
  MEASUREMENT; CACHE REMAINS DORMANT:** the vendored
  gfx_webgpu.c has a record/replay pipeline-prewarm cache, but it only runs when
  the game calls gfx_webgpu_set_stage() (mgb64's boss.c does; DKR's loop does
  not), so s_prewarm_cur_stage stays -1 and it records/writes nothing. savedirPath
  (platform/gfx_webgpu_stubs.c) exists only to link. If WebGPU cold-pipeline hitching
  on first-visit materials ever shows up, wire gfx_webgpu_set_stage(levelId) at
  level_load and gfx_webgpu_prewarm_stage(levelId) after it, and give savedirPath a
  real save/ path. The real-browser gate now enforces 20.0 ms p95 / 25.0 ms p99
  cadence and a two-frame async compile/hold maximum; the measured route was
  17.54–17.59/17.82–18.07 ms with one-frame maxima. Persistent prewarm is not
  justified by those measurements.
- ~~**GL frame-dump double-buffer artifact (GL-side, not WebGPU):**~~ **FIXED:**
  during phases
  where the game submits a gfxtask only every OTHER present (e.g. the boot logo
  fade), the GL `--dump-frames` path (glReadPixels(GL_BACK) in platform_sdl_min.c,
  before SwapWindow) captures alternating content/black frames, because the
  double-buffer swap flips which buffer is GL_BACK. WebGPU has no such artifact (it
  reads a persistent offscreen scene target, matching what the real display holds).
  Impact: only the automated GL-vs-WebGPU parity scoring, which now skips GL's
  black captures. The backend now retains the completed pre-swap composite and
  the dump path reuses it on presents without a new graphics task. The pre-fix
  700-frame control contained 85 black frames and repeated isolated A/B/A
  artifacts; the corrected run has none, and the renderer gate checks 215
  consecutive presents.
- **Headless attract near-plane billboard spike differs by backend — FIXED** (wave
  "nearclip"): in the degenerate all-black headless attract scene, a
  near-plane-straddling billboard rendered as a thin gold spike under GL but was
  clipped by WebGPU's pipeline. It was never a WebGPU item — it was the HLE's
  missing near-plane clipping, so the two backends disagreed only because one of
  them happened to clip what the HLE should have. The HLE now clips, which removes
  the backend divergence at its source.
- ~~**Runtime device-init failure = loud, not graceful.**~~ **FIXED:** readiness
  is checked after real surface configuration. Native may rebuild WebGPU once,
  then stops visibly if the same backend remains unhealthy; it never changes a
  live process to GL. Browser failures replace the frozen canvas with recovery
  controls and a reload message.

## Phase 2 — menu 1:1 fidelity: every screen now scored (wave "oraclefix")

**All of Phase 2's screens have oracle routes and real numbers.** Getting there
required fixing four harness defects first — every one of them presented as a
rendering fidelity gap and none of them was. See docs/ORACLE.md for the
mechanisms; the short version:

1. `--setting Audio/Driver=None` is an unknown key in the pinned ares, which makes
   it print `Invalid setting` and **return before loading the ROM** — every oracle
   run produced **zero frames**, and the "silent by construction" guarantee rested
   on a setting ares rejected. Now `SDL_AUDIODRIVER=dummy`.
2. Input was injected into **all four** controller ports (`Gamepad::read()` runs
   per controller and the hook was port-blind), so four players JOINED at PLAYER
   SELECT. That changes the menu graph — `charselect_confirm` only reaches CAUTION
   when `gNumberOfActivePlayers == 1` — so the real ROM skipped CAUTION and ran
   ahead into track select while our port sat on CAUTION: two correctly-rendered
   but *different* screens scoring ~50%.
3. A single global sync delta cannot align a multi-tap route (real-hardware level
   loads stall for ~40 frames where ours take ~20; the error accumulates per tap).
4. Marks at `logical` 0 scored two **black** frames as 50% (hist=100%, block=0%).

**Scoreboard** (aligned score; offsets in docs/ORACLE.md terms — never read the
aligned number without its offset):

| screen | aligned | hist | block |
|---|---|---|---|
| boot logo (`boot_to_title`) | **99.8%** | 99.5% | 100.0% |
| CHARACTER_SELECT | **95.0%** | 92.4% | 97.7% |
| OPTIONS | **94.4%** | 91.4% | 97.4% |
| SAVE_OPTIONS | **93.4%** | 89.9% | 96.8% |
| CAUTION | **95.1%** | 93.4% | 96.9% |
| AUDIO_OPTIONS | **92.0%** | 89.0% | 95.1% |
| TRACK_SELECT | **93.7%** | 91.2% | 96.3% |
| GAME_SELECT | **91.8%** | 90.4% | 93.3% |
| title menu (dynamic island scene) | 87.1% | 89.3% | 84.8% |
| FILE_SELECT | 83.7% | 88.3% | 79.1% |
| MAGIC_CODES | 79.3% | 73.4% | 85.1% |

The static text screens land at 92–95 % with block 94–98 %, i.e. essentially at
the Phase 2 target (hist >0.95 / block >0.98) but not yet past it. The title menu
is a live camera flythrough and will never score frame-precisely.

**`gPlayerHasSeenCautionMenu` — RETRACTED, there is no asymmetry.** An earlier
revision of this file claimed the flag was save-backed and that our side was
failing to persist or honour it. That was **wrong**: `menu.c:547` declares it a
plain global initialised to `FALSE`, and the comment at `menu.c:7339` says
"Only seen once per **game session**". Showing CAUTION once per fresh run is
correct N64 behaviour on both runners. Nothing to fix.

Save persistence itself was verified separately and **works**: delete
`save/eeprom.bin`, run once → a 512-byte file with 120 non-zero bytes; run again
→ byte-identical md5, i.e. the file is read back rather than reset
(`platform/stubs_dkr.c` loads once on first access and writes through on every
`osEepromWrite`, with an IDBFS sync for the web build).

### FIXED: TRACK_SELECT scored 61 % — the HLE texture cache aliased freed arena memory (wave "texalias")

**The preview camera was never the bug.** Every previous characterisation of this
screen in this file was wrong and is retracted below. **61.0 % → 93.7 %** aligned
(hist 69.6 → 91.2, block 52.4 → 96.3), against a ~95 % noise floor.

**Mechanism.** `dkr_bind_tile()` (`platform/fast3d/gfx_pc_dkr.c`) keys its
GPU-texture cache on the **source address** plus `fmt/siz/width/height/palette`.
Nothing invalidated an entry when the game *freed* that memory, so mempool would
hand the same arena bytes to a different asset and the next lookup **hit**,
binding the previous asset's uploaded texture. It is silent by construction: no
crash, no missing draw, correct geometry — just the wrong image. DKR's menus
trigger it constantly because every screen frees the previous screen's assets and
reloads into the same pool, and same-purpose assets share dimensions and format,
so the key matches exactly and only the *content* differs.

**Measured evidence.**
1. The TRACK SELECT background is drawn by `func_8008F618()` (`menu.c`) as
   30-px horizontal bands that alternate between a world's **two** background
   textures (`gTrackSelectBgData` rows 0–5 for Dino Domain read tex 0,1,0,1,0,1).
   The real ROM draws all of them as the same sand; ours drew every other band as
   a blue "sky/snow" texture.
2. The row walk is *correct* — instrumented, it emits rows 0–8 with tex
   `0,1,0,1,0,1,0+2,1+3,0+2`, exactly as the table says.
3. Both Dino textures load, at distinct addresses, 64x32 RGBA16, and **both have
   sandy texels**: mean RGB (243,200,58) for slot 0 and slot 1 (content hashes
   `aad79aea` / `f10b5339`, so they are genuinely different sand images, not the
   same one twice). Slot 4 (Snowflake Mountain) is the blue one, (84,152,244).
4. Forcing every band to slot 0 rendered the whole background as correct sand;
   forcing every band to slot 1 rendered it all blue — so the *bind*, not the
   asset, was wrong.
5. Recording each cache entry's source-content hash at upload and re-checking it
   on every hit named the mechanism outright, at the TRACK SELECT frames:
   ```
   [TEXCACHE] STALE HIT f=1978 slot=276 addr=0x5c81c11c0 64x32 fmt=0 siz=2 uploadedHash=dcff67eb nowHash=f10b5339
   [TEXCACHE] STALE HIT f=1978 slot=277 addr=0x5c81c2260 64x32 fmt=0 siz=2 uploadedHash=00afd720 nowHash=42533e26
   [TEXCACHE] STALE HIT f=1978 slot=278 addr=0x5c81c3300 64x32 fmt=0 siz=2 uploadedHash=5420c269 nowHash=9359c3fe
   ```
   Note the shift: slot 278's *uploaded* hash is slot 276's *current* hash. An
   earlier load had placed the same ten world backgrounds two slots along, so
   every band was showing the texture from two worlds later. Slot 0
   (`…1b10e0`) never appears — which is exactly why one band in two looked right.

**Fix.** `gfx_dkr_texcache_invalidate_range(base, size)` in `gfx_pc_dkr.c` drops
every entry whose uploaded source span overlaps `[base, base+size)`, called from
`mempool_slot_clear()` (`game/src/memory.c`, `#ifdef NATIVE_PORT`) — the single
point at which arena bytes become reusable — **before** the free-slot coalescing
rewrites `slot->size`. Steady-state cost is one pass over the 1024-entry cache
per free and no hashing: measured on `race_drive_time_trial` (2900 frames), 2.22–
2.26 s with the fix vs 2.12–2.17 s without, i.e. ~25 µs/frame (<0.2 % of a frame).

**Verification.**
- `MDKR_TEXCACHE_VERIFY=1` hashes each bind's source and counts hits whose content
  changed (`gfx_dkr_texcache_stale_hits`). **0 stale hits** on all 9 menu-nav
  fixtures + `race_drive_time_trial`. **Positive control:** comment out the one
  call in `mempool_slot_clear()` and it reports stale hits from frame 1490 on.
- Oracle `title_to_track_select`: `track_select` 61.0 % → **93.7 %**
  (hist 91.2 / block 96.3), montage shows the same viewpoint in both runners.
- Same bug was quietly costing other screens: `game_select` 87.6 % → **91.8 %**,
  `caution` 92.0 % → **95.1 %**. No screen regressed —
  `title_to_character_select`'s `char_select` is byte-identical at 95.0 %
  (92.4 / 97.7) and the title menu is unchanged at 87.1 %.
- 0 crashes in 10x2900 frames each on `nav_to_track_select`,
  `nav_to_game_select`, `nav_to_character_select`;
  `tests/check_determinism.py` PASS; `tests/check_race_drive.py` PASS.

**Retractions.** The preview is **not** a static camera and this was **not** a
camera-placement bug. Both runners run the *same animated flythrough* — the real
ROM dissolves the round world icon into a camera that starts at the start line
(START banner overhead) and drives down the track; so does ours. The earlier
"both cameras are parked, and the cross-runner score is flat at ~60 % across the
whole 260-frame window" reading was an artifact of the background mismatch: with
half the screen structurally wrong, no choice of phase could score better than
~60 %, and the ±120 aligned search locked onto meaningless frames. The
`BHV_CAMERA_ANIMATION` / `objectIdToSpawn` lead was a red herring; the object-map
body swap already covers this level.

**Resolved follow-up (historical pre-fix measurement).** At this checkpoint the
*WebGPU* `track_select` score was **70.4 %**, versus 93.7 % on GL, because clamping
the deliberately oversized ortho viewport changed its transform and left black
bars at the top and bottom. The later **WebGPU viewport clamping … FIXED** entry
above records the implemented clip-space affine, pixel-exact button bounds, and
final verification; this is no longer an open WebGPU or track-select defect.

The MAGIC_CODES (79 %) and FILE_SELECT (84 %) figures at this checkpoint also
predate their later investigations. The environment-colour fix raised
MAGIC_CODES to 94.8%; FILE_SELECT's remaining difference was isolated to the
shared island backdrop rather than missing UI or a WebGPU layout error.

## FIXED: no near-plane clipping in the HLE (wave "nearclip")
The HLE emitted every triangle unclipped and leaned on `GL_DEPTH_CLAMP`. Depth
clamp does not clip — it only stops the depth test discarding out-of-range
fragments — so a vertex behind the eye (`w <= 0`) still went through the
perspective divide, and dividing by a negative `w` **mirrors that corner through
the origin**, flinging it to the far side of the screen. The triangle then
rasterised as a long stretched sliver instead of being cut at the near plane.
That is the "stretched bar / spike" artifact class.

**Fix** (`platform/fast3d/gfx_pc_dkr.c`): Sutherland–Hodgman against a single
plane, `z + w >= 0`, in homogeneous clip space, before the divide. A triangle
becomes at most a quad, which is fan-triangulated. Position, `u/v`, and RGBA are
interpolated linearly in clip space. Fog is the necessary exception discovered
later: its stored byte has already been derived from post-divide `z/w`, so a
generated vertex recomputes fog from its new position and the GPU interpolates
that coefficient in screen space. See the lower-screen-fog fix above.

**Why `z + w >= 0` and not merely `w > 0`.** It is the true near plane, and for a
well-formed perspective projection it also *implies* a positive `w`: writing
`z = -(f+n)/(f-n)·z_e - 2fn/(f-n)` and `w = -z_e` gives
`z + w = (-2f/(f-n))·(z_e + n)`, so `z + w >= 0` iff `z_e <= -n`, and every such
point has `w = -z_e >= n > 0`. One plane therefore both cuts at the right depth
and guarantees the divide is well defined. `MDKR_NEARCLIP=w` keeps the
`w`-only variant for A/B; `MDKR_NEARCLIP=off` reproduces the artifact.

**The backface cull moved onto the clipped polygon.** Previously it was *skipped
entirely* whenever any `w <= 0` (`a.w > 0 && b.w > 0 && c.w > 0 && cross < 0`),
so straddling triangles were never culled. Now the test is the clipped polygon's
total signed area. Clipping preserves winding, so a fully-inside triangle yields
exactly the old decision, while a straddling one is decided from real on-screen
geometry.

**Incidence** (per 2900-frame fixture): ~27 000 triangles actually clipped and
~88 000 dropped as entirely behind the plane — i.e. roughly 7–10 clipped and
21–33 wrongly-drawn triangles *per frame*, not a rare edge case. Counters are in
the `MDKR_TRACE>=2` per-frame line (`nearclip=`, `dropped=`, `degen=`).

**Visual evidence** (A/B in one build via `MDKR_NEARCLIP`, on a now-deterministic
baseline): the Time-Trial pre-race screen had a dark-brown wedge smeared over the
right and bottom **~30 % of the frame**; clipped, it is replaced by the correct
road surface and canyon wall. Track select had the same overspill bleeding
outside the wooden picture frame; clipped, the preview renders correctly inside
it. Character select is **0 pixels different** — the artifact documented there had
already been fixed by the menu-fidelity wave (its "black background" precondition
no longer holds), so that note is now stale.

**Residual:** `degen` (a clipped vertex still having non-positive `w`, which a
sane projection makes impossible) fires **16 times in 2900 frames on
`nav_to_track_select` only**, 0 on the others. Those triangles are skipped and
counted rather than emitted as wild geometry. It implies a degenerate game
matrix on that screen — the same family as the "all-zero matrix treated as
identity" case in `dkr_load_matrix`. Not chased; it is counted, so it cannot hide.

**NOTE — this was verified only after fixing headless determinism (below).** The
first attempt to verify it compared before/after frames and appeared to show a
severe regression (limbs and shadows missing, 18.7 % of pixels changed). That was
entirely run-to-run variance in the baseline; the clip was innocent. Any renderer
change here must be A/B'd against a deterministic baseline or the result is noise.

## FIXED: headless renders were NOT reproducible (wave "determinism")
**This invalidated every frame-comparison result in the project until now** —
oracle scoring, GL-vs-WebGPU parity, native-vs-wasm, and every "dump a PNG and
look at it" check on any screen with music-synced animation. Found while trying
to verify a renderer change by comparing before/after frames: the *baseline*
would not reproduce.

**Measured.** `nav_to_character_select` frame 1450, 10 runs of the same binary
with identical flags: **10 distinct images, 45/45 pairs differing, median 18.9 %
of pixels, minimum 4.7 %.** Never once identical.

**Root cause — the host wall clock, NOT ASLR.** `osGetCount()`
(`platform/stubs_dkr.c`) returned the host monotonic clock scaled to the N64
COUNTER rate. On the N64 the COUNTER and the VI retrace rate are both real time
and therefore locked to each other; a headless run breaks that lock, because the
frame loop advances as fast as the machine allows while the pacer synthesises a
**fixed** field count per frame. So the COUNTER advanced by a machine-load-
dependent amount per simulated frame. `audio.c music_animation_fraction()`
integrates COUNTER deltas into `gMusicAnimationTick`, and
`object_functions.c:2134` (`obj_loop_charselect`) feeds that straight into the
character-select models' `animFrame` — so the characters' animation phase, and
the rendered image, differed every run.

The ASLR hypothesis this codebase's history makes tempting (the arena base does
vary per run) was **wrong**, and disproving it was the turning point: after the
fix, 5 runs across 5 *different* arena bases produce a byte-identical frame.

**How it was localised.** The per-frame draw telemetry (`MDKR_TRACE=2`
`gfx_run frame N: emitted=… onscreen=…`) is a cheap whole-state fingerprint that
costs no disk: frames 0..1298 were byte-identical between runs and divergence
began at frame **1299**, immediately after the menu transition that starts a
music-synced animation, as a 2-triangle difference that then grew. A pixel diff
of the frame confirmed the shape of it — the sky, log, grass, trees and title text
were untouched and *only the character models* changed, which is exactly the
geometry `music_animation_fraction()` drives.

**Fix.** Derive the COUNTER from the pacer's cumulative 60 Hz field total whenever
pacing is synthetic (`--headless-frames`), restoring the N64's locked
COUNTER/retrace relationship; a windowed run still uses the host clock, where it
is the correct source. One 60 Hz field is exactly `46875000/60 = 781250` COUNTER
ticks, so the conversion is exact.
`platform_pace_is_synthetic()` / `platform_sim_field_count()`
(`platform/platform_sdl_min.c`, declared in `platform_os.h`) expose the existing
accumulator; `osGetCount()` consumes it.

**mgb64 already does this** (`src/platform/stubs.c:335`: `g_deterministic`,
`s_syntheticCount`, `SYNTHETIC_TICKS_PER_FRAME`, plus `randomSetSeed(0x12345678)`
for deterministic replay in `boss.c`). This was prior art the mdkr64 port layer
failed to carry over, not a novel design — and it is a reason **not** to "clean
up" mgb64's synthetic counter. See docs/MGB64_BACKFLOW.md.

**Regression coverage.** `tests/check_determinism.py` — runs each fixture N times
and requires every dumped frame to be byte-identical. Verified to FAIL on a build
with only `osGetCount()` reverted (`nav_to_character_select` frame 1400, first
differing byte at offset 1014820) and to pass with the fix. Full matrix after the
fix: 9 menu fixtures ×20 = 0 crashes, `race_drive_time_trial` ×20 = 0 crashes,
`check_race_drive.py` PASS, 12000-frame soak rc=0 / 0 `[FATAL]`, audio unchanged
(`fx-guard trips=0`, mainbus clip 515 / 0.00603 % / +1.29 dBFS — the documented
baseline).

**Open follow-on:** `osGetTime()` (same file) is still host-clock based. Nothing
currently reads it in a way that reaches rendering (its only game-side callers are
audio bookkeeping), so it was left alone rather than bundled into this fix — but
it is the same hazard and should move to the simulated clock if anything starts
depending on it.

## High-rate native delivery cadence — CLOSED (immutable-presentation wave)

The 1.0.1 containment was correct: delayed replay held only a display-list
pointer while the game reused its viewport, matrix, vertex, texture, and child
storage for the next task. Non-Original policies therefore changed host pacing
without producing safe new images.

The production renderer now keeps gameplay on Original two-field tickets and
publishes an atomic private rendering task instead. It double-buffers the
16 MiB arena, rebases the top list and segment table, copies external
matrix/vertex/triangle/viewport/texture/TLUT/smooth-normal spans observed by the
real HLE walk, and acquires only an exact authored-tick token. A read-only census
of the already-authored alternate task supplies the true `{T,T+1}` deformation
and effect pair without backend or game callbacks. Generation, topology,
viewport, animation, particle, and effect incompatibility holds task T rather
than extrapolating.

Public `Video.MotionSmoothing=interpolate` now produces unique intermediate
camera, object, model, particle, fade, and shield images under Display, numeric,
and native Uncapped policies. Off retains authored motion. WebGPU admission
never waits on gameplay/audio, classifies every shed endpoint/replay attempt,
and reserves one of its two slots for authored work. The native/browser UI
keeps gameplay cadence, frame delivery, smoothing, and visual preset separate;
Enhanced remains an explicitly gameplay-changing compatibility mode.

The closure evidence is authority-first: exact v3 state, ordered events,
consumed input, PCM, audio time, and saves across NTSC/PAL arbitrary rates;
byte-exact alpha-zero semantics/pixels while the entire live arena is poisoned;
independent midpoint pixel controls for every retained class; unload/restart/
post-race lifetime routes; native GL/WebGPU queue accounting; and real Chromium
rAF schedules. See [`UNCAPPED_PRESENTATION.md`](../UNCAPPED_PRESENTATION.md) and
`tests/check_presentation_matrix.py`. Physical platform breadth remains release
qualification, not an open ownership defect.

## Frame pacing / slow-motion — RESOLVED (pacing wave)
- [x] **Root cause of in-race slow motion (and high-refresh fast motion).** DKR
  normalises game speed against framerate in `fb_update()` (game/src/video.c:277):
  it non-block-drains the video message queue counting how many 60 Hz VI fields
  elapsed per rendered frame and returns that as `updateRate` (LOGIC_60FPS=1 @
  60fps, LOGIC_30FPS=2 @ 30fps, ...). On N64 the VI interrupt posts those retrace
  messages ASYNCHRONOUSLY at 60 Hz (video.c:290-292 drains them). Our cooperative
  shim had NO async producer — `osRecvMesg` synthesised exactly one retrace per
  blocking receive and the non-block drain always found 0 — so `updateRate` pinned
  at `LOGIC_60FPS=1` permanently. The game then scaled ALL movement/physics for a
  1-field (1/60 s) timestep per rendered frame regardless of the real present
  cadence: when the renderer/vsync ran slower than 60 fps, motion ran slow
  (slow-motion); on a >60 Hz display it would run fast. DKR's own frameskip
  compensation (the `updateRate` multiplier) was defeated.
- [x] **How the race clock advances (why it looked "correct").** The race clock is
  NOT real-time based — it is `updateRate`-accumulated exactly like motion:
  `tempRacer->lap_times[tempRacer->countLap] += updateRate;` (game/src/racer.c:4382),
  displayed via get_timestamp_from_frames (frame-count / 60). So the clock is
  LOCKED to motion through `updateRate`; before the fix it ran SLOW in the same
  proportion as motion (measured: 0.5x real-time at a 30fps cadence — see below).
  It only "looks correct" because a player has no external reference for in-game
  clock speed, and because lighter menus sustain 60 fps (updateRate 1 = correct)
  while a heavier race scene drops below 60 (updateRate stuck at 1 = slow). The
  clock is not independently correct; nothing in the game reads osGetTime for it.
- [x] **Fix — wall-clock VI-field pacer** (platform/platform_sdl_min.c
  `platform_vi_pace_measure` + platform/stubs_dkr.c video-queue recv). Modelled on
  mgb64's cooperative VI pacing (src/platform/platform_sdl.c platformFrameSync +
  stubs.c osRecvMesg: pace to a 1/60 s floor, drive retraces from the frame loop).
  Each present paces to a 1/60 s wall-clock floor (refresh-INDEPENDENT — caps a
  fast/high-refresh path to 60 Hz, drift-free field accumulator) and measures the
  true number of 60 Hz fields the frame occupied; the video-queue recv then makes
  exactly that many retrace messages available, so `fb_update`'s drain returns the
  matching `updateRate` (2 @ 30fps, ...) and DKR's frameskip compensation is
  restored. Deterministic in headless (each frame = a fixed field count, default 1,
  so existing headless tests are byte-identical); realtime for a windowed run.
  Env knobs: MDKR_SYNTH_FIELDS=N (synthetic cadence), MDKR_PACE_REALTIME=1,
  MDKR_VI_PACE=off (reproduce the pre-fix bug), MDKR_FIELD_HZ.
- [x] **Objective measurement (headless, scripted race_drive_time_trial).** At an
  identical wall-time (cumulative 60 Hz fields), race-clock advance ÷ real-time:
  fix @60fps = 1.000, fix @30fps = 1.000 (cadence-independent, correct), bug
  @30fps = 0.500 (half real-time = the slow-motion). Motion tracks it: the
  bug-repro run reaches z=-6491 / clock=1389 by wall-field 8400, exactly where a
  60fps run is by wall-field 4200 — i.e. half real-time progress. Player-1
  world-position + race-clock are published each frame from racer.c under
  NATIVE_PORT (`mdkr_pace_probe_racer`) and logged by the pacer trace ([PACE]
  lines, MDKR_TRACE>=1). Real-wall-clock headless run: an M3 Max sustains ~60 fps
  (avg 16.67 ms/frame, updateRate 1) so the fixed windowed path renders at correct
  60 fps speed.
- Residual timing risk: in a windowed run vsync (swap interval 1) still blocks in
  SwapWindow on top of the software 1/60 s floor; on a >60 Hz panel the two can
  beat and make `updateRate` oscillate 1↔2 frame-to-frame (average speed stays
  correct — the accumulator is drift-free — but there is minor jitter). mgb64
  accepts the same tradeoff; a render-interpolation or adaptive-vsync pass would
  remove it. The synth2-fix trajectory differs slightly from synth1-fix per
  race-clock tick (larger physics timesteps in nonlinear collision) — this is
  DKR's own frameskip approximation, not a regression; wall-clock speed is exact.


## M4 render state (this wave — input + interactive menus)
- Menus are navigable and scene geometry renders; the boot path reaches the title
  screen and, on scripted input, character select / options (see STATUS.md M4).
- [x] MENU / UI TEXT + the DKR LOGO now render CORRECTLY (M3c renderer wave).
  Verified by reading dumped frames: the title screen draws the "DIDDY KONG RACING"
  logo (red DIDDY KONG + star, blue/green RACING, TM), START/OPTIONS is legible,
  the OPTIONS list reads ENGLISH / SUBTITLES ON / AUDIO OPTIONS / SAVE OPTIONS /
  MAGIC CODES / RETURN, and PLAYER SELECT is legible. The whole scene is now
  textured (was flat-shaded). THREE stacked root causes, all in gfx_pc_dkr.c:
    1. gSPTexture s/t scale is ALWAYS 0 in DKR (its Triangle/TEXRECT UVs are
       absolute S10.5 texel coords; the microcode does not use the scale as a
       coordinate multiplier). The F3DEX-style `tc*scale>>16` multiplied by zero,
       collapsing every texcoord to (0,0) so all textured primitives sampled
       texel 0 — solid text, solid sprites, flat-shaded terrain. Fix: treat a
       zero scale as unity (dkr_vbo_texcoord). This one bug caused MOST of the
       "textures not sampled" symptoms.
    2. 32-bit texture row stride was HALVED. gDPLoadTextureBlock derives the tile
       `line` with G_IM_SIZ_32b_LINE_BYTES==2 (RG/BA TMEM bank split → a TMEM row
       is width*2 bytes), but our arena source is plain contiguous RGBA32 (4
       bytes/texel). Decoding at line*8 read half-stride → rows overlap → the
       RGBA32 font atlas + logo strips garbled to blocks/bands. Fix: double
       line_bytes for G_IM_SIZ_32b (dkr_upload_tile_texture).
    3. The non-perspective (G_TP_NONE) *0.5 texcoord halving (a real RSP behaviour
       for perspective-off GEOMETRY texcoords, kept for parity with mgb64) was
       wrongly applied to TEXRECTs, whose s/t are absolute RDP coords. DKR draws
       all text + the logo as G_TP_NONE texrects, so they sampled at 2x zoom →
       mangled glyphs / squished logo. Fix: dkr_in_texrect flag skips the *0.5 in
       the texrect path (dkr_apply_tile_uv).
- [x] Some BILLBOARD SPRITES still stretch into thin bars (a faint bar mid-title,
  a green streak on PLAYER SELECT). NOT a renderer bug: the billboard transform is
  correct and clean sprite quads (all local z==0, per-vertex rgba==white, as the
  runtime sprite builder in textures_sprites.c render_sprite writes them) render
  as proper quads. The bars come from specific objects whose vertex data is
  CORRUPT in arena memory — e.g. a first-batch sprite with raw verts 0/3 carrying
  local z≈-17488/-18432 (hex 0xbbb0/0xb800) and non-white rgba, which the flat
  sprite builder never produces. A vertex with clip z≈-24592 at w≈937 is behind
  the near plane; GL_DEPTH_CLAMP (enabled for the GL4.1 path) draws it instead of
  near-clipping, so the quad stretches off-screen into a bar. Root cause is a
  game-side LP64/vertex-construction bug in a specific object path, NOT the
  F3DDKR HLE. **RESOLVED by the native-layout and restoration sprint:** field-
  oriented sprite calls removed the prefix casts; one asset-bounded sprite
  layout owns all readers/builders; near-plane clipping replaced depth-clamped
  wedges; and widescreen pixel gates now require proportion-correct billboards.
- [x] RESOLVED (M4-fix) — the intermittent ASLR-dependent char-select SIGSEGV in
  `dkr_sp_vertex` (`verts=0xffffffff........`, low-32 varying run-to-run). ROOT
  CAUSE was a single LP64 pointer-truncation in `render_level_segment`
  (game/src/tracks.c): the batch vertex/triangle addresses were held in `s32`
  locals and assigned with `vertices = (s32) &DKR_PTR(Vertex, ...)[off]`. On the
  64-bit host `DKR_PTR` reconstructs the full arena pointer; `(s32)` truncates it
  to 32 bits and, being SIGNED, sign-extends any pointer whose low-32 has bit 31
  set — which is ASLR-dependent (the 16 MB-aligned arena base's bit 31 varies per
  run). That wild `0xffffffff........` value was passed straight to
  `OS_K0_TO_PHYSICAL`, which (being >4 GB and outside the arena) REGISTERED it in
  the gfx_ptr table; `dkr_resolve` then hit that registry entry and handed the
  wild pointer back to `dkr_sp_vertex`, which faulted on the unmapped page. FIX:
  make the locals `Vertex*`/`Triangle*` under NATIVE_PORT and drop the `(s32)`
  cast, matching every other gSPVertexDKR/gSPPolygon site (game_ui.c, menu.c,
  weather.c) — the full arena pointer round-trips through OS_K0_TO_PHYSICAL's
  in-arena tokenizer correctly. HARDENING (belt-and-suspenders, so no wild pointer
  can ever be dereferenced regardless of input): `gfx_ptr_store` now refuses to
  register any value with all-ones high 32 bits; `dkr_resolve` validates every
  result with `dkr_ptr_plausible()` and returns NULL rather than a sign-extended
  pointer; and `dkr_sp_vertex` / `dkr_sp_polygon` / `dkr_load_matrix` skip a
  non-arena pointer that isn't host-plausible. Verified: char-select 1500f, options
  1500f, title 300f each 0 crashes over 20 runs (was 13/20 on char-select before);
  ASan clean over 6 char-select runs. The char-select capture confirms content still
  renders (legible textured "PLAYER SELECT" + scene geometry — nothing dropped).
  NOTE (related, latent, NOT a live crash): the collision-scratch block in
  tracks.c func_8002B0F4 (~lines 2936-2948) and line 2941 also use `(s32)`
  truncation on a running arena offset `j`, and textures_sprites.c:577/587 use
  `(u32) &gCiPalettes[...]`, but those store into dkrptr32 (u32) slots / feed a
  zero-extending `(u32)` cast, so the sign-extension is discarded and they
  round-trip via arena reconstruction — they never reach OS_K0_TO_PHYSICAL as a
  full 64-bit wild pointer the way the render path did. Left as-is (working).
- [x] DONE (M6) — the menus now drive into an actual TIME-TRIAL race and a
  human-controlled racer accelerates + steers + MOVES through the track (Ancient
  Lake), which renders with a working HUD. Eleven LP64/big-endian bugs on the
  level-load->race path root-caused and fixed (full list + file:line in STATUS.md
  "M6 playable race"). The two that mattered most:
    * The per-triangle COLLISION FACETS (CollisionFacetPlanes basePlaneIndex /
      edgeBisectorPlane, u16) were never byte-swapped by swap_level_model, so
      track_init_collision built nan/garbage collision planes and no wheel ever
      grounded (throttle produced zero motion). Now swapped (asset_swap.c).
    * The per-vehicle wheel-offset/radius + acceleration f32 arrays live in the
      heterogeneous ASSET_MISC blob and were left big-endian -> decoded to ~0
      denormals -> wheels at car centre, radius 0. New dkr_misc_swap_words()
      (objects.c) byte-swaps a named ASSET_MISC word sub-asset once, called at the
      racer f32 fetch sites (racer.c).
  ASSET_MISC endianness note: the blob is heterogeneous (byte tables like
  TRACKS_MENU_IDS sit next to f32 arrays), so it can't be blanket-swapped —
  dkr_misc_swap_words is per-index and dedup'd. If a new race feature reads
  another ASSET_MISC sub-asset as f32*/s32* and gets ~0/garbage, add a
  dkr_misc_swap_words(index) at that fetch site.
- [x] RESOLVED (fidelity: billboard red-spikes) — IN-RACE billboard sprites (the
  player kart + AI karts) no longer render as stretched red bars/spikes; they
  render as recognizable textured character karts. Same fix cleared the title's
  faint mid-screen bar and the tall green bars on PLAYER SELECT. ROOT CAUSE was a
  single LP64 struct-size inflation: `Gsetcolor.color` in PR/gbi.h is N64 `unsigned
  long` (32-bit on N64, so the RDP setcolor packet is 8 bytes and every Gfx union
  member is 8 bytes "by law"). On LP64 `unsigned long` is 8 bytes with 8-byte
  alignment, which ALONE inflates `sizeof(Gfx)` from 8 to 16. `tex_load_sprite`
  (textures_sprites.c) reserves the sprite DL region with a LITERAL 8-byte Gfx
  stride — `gSpriteVertices = gSpriteDLists + numTextures*0x20 (=4*sizeof(Gfx)@8) +
  numberOfFrames*sizeof(Gfx)` — but the DL WRITER (`dlptr++` in sprite_init_frame)
  advances by the real `sizeof(Gfx)`=16, so for multi-tile sprites the DL commands
  over-run PAST gSpriteVertices and overwrite the sprite quad vertices with Gfx
  command bytes. The HLE then read those bytes as a Vertex: a `gDkrDmaDisplayList`
  packet decodes to a vertex with huge local z (a texture-cmd pointer's bytes) and
  non-white rgba, so one billboard-quad corner flew far off-screen after the slot-2
  billboard transform → a thin red/coloured spike. Confirmed by (1) a raw-vertex
  probe showing the quad region contained G_DMADL/G_VTX/G_TRIN command bytes with
  embedded arena pointers, and (2) a per-frame sprite-loader probe showing the DL
  pointer reaching the exact vertex-region start (`dl == vtx`) at frame 6 of a
  10-frame sprite, `sizeof(Gfx)=16`. FIX: pin `Gsetcolor.color` to a 32-bit type
  under NATIVE_PORT (mirrors the existing Mtx_t LP64 fix two structs up) so
  `sizeof(Gfx)==8` and the literal-vs-sizeof layout math agrees again. Verified:
  race frame 2848 renders the player kart (with shadow), AI karts, item boxes,
  bananas and "GO!" correctly (was red spikes); 0 corrupt billboard verts and 0
  billboard stretched-tris across the whole race / title / menus; title logo +
  menu text unchanged; race_drive 20× = 0 crashes, title 300f 5× = 0, char-select
  /options/game-select/magic-codes fixtures 0 crashes. The interpreter reads Gfx at
  whatever stride the type reports, so it was always consistent with the writer —
  the corruption was purely the game-side literal(8)-vs-sizeof(16) mismatch. See
  gfx_pc_dkr.c header "LP64 STRUCT-SIZE LOCKS" and PR/gbi.h Gsetcolor comment.
- [x] RESIDUAL — CLOSED two ways (see "wave nearclip" at the top of this file).
  The original note read: the PLAYER SELECT scene in HEADLESS shows a compact green
  textured shape low-left — a `bb=0 slot=1` ~12-vertex object whose vertices
  straddle the NEAR PLANE (several w<=0) in the degenerate headless char-select
  where the whole scene background is black, drawn stretched by GL_DEPTH_CLAMP
  instead of near-clipped.
  (1) Its *precondition* is gone: the menu-fidelity wave made char-select render
  the real scene, so there is no black-background degenerate frame any more.
  (2) The HLE now does real Sutherland-Hodgman near-plane clipping, so the whole
  artifact class is fixed at the source — and, as the original note insisted,
  NOT by culling near-plane triangles. Character select is now 0 pixels different
  with clipping on vs off; the artifact class shows up instead on the Time-Trial
  pre-race screen and track select, where clipping removes large smeared wedges.
- [x] Race correctness beyond the proof: full 3-lap completion, results, exact
  time/save reload, and live audio are covered. (60/30fps pacing: RESOLVED — see
  "Frame pacing / slow-motion" at the top of this file.)
- [x] RGBA16/IA16 texels + G_LOADTLUT palette now decoded big-endian
  (gfx_pc_dkr.c) — colours were byte-swapped; sand/water/sky decode correctly.
- [x] Level-model BSP-tree node array now byte-swapped in load_track_model
  (tracks.c): node count = numberOfSegments-1; swaps leftNode/rightNode/
  splitValue (s16). Was the "menus reach the title then crash" stack overflow.

## F3DDKR semantics pinned down (M3c renderer wave — text/logo/sprites)
- **gSPTexture scale is unused as a coordinate multiplier.** DKR emits every
  gSPTexture with s==t==0x0000 (1292/1292 G_TEXTURE cmds in a title frame). Its
  Triangle UVs and TEXRECT s/t are ABSOLUTE S10.5 texel values, so the microcode
  does NOT do the F3DEX `tc*scale>>16`. HLE must treat a 0 scale as unity 1.0,
  never multiply texcoords by 0. (dkr_vbo_texcoord.)
- **32-bit tile LINE counts 2 bytes/texel, not 4** (PR/gbi.h G_IM_SIZ_32b_LINE_BYTES
  == 2, the RG/BA TMEM bank split). The rendered tile `line` for a width-W RGBA32
  texture is (W*2+7)>>3 words → line*8 == W*2 bytes == HALF the real contiguous
  source pitch (W*4). HLE decoding a plain RGBA32 arena buffer must use line*8*2
  as the source row stride. (16/8/4-bit LINE_BYTES already equal real bytes/texel.)
- **TEXRECT s/t are absolute RDP coords, exempt from the G_TP_NONE *0.5.** The
  non-perspective texcoord *0.5 models the RSP's fixed scale when it computes
  perspective-off GEOMETRY texcoords; TEXRECT coordinates come straight from the
  RDP and must map 1:1. Guard the *0.5 with an "in texrect" flag. (Applies to
  mgb64's identical gfx_apply_tile_uv_transform if a GE texrect ever uses
  G_TP_NONE — see MGB64_BACKFLOW.md.)
- **Billboard transform (gDkrEnableBillboard):** verts 1..n = M_slot2 * local +
  clip(vtx0), sharing vtx0's w, added in CLIP space before the perspective divide
  (matches f3ddkr.h). Confirmed correct against traced data; clean flat sprite
  quads (local z==0) render right. Malformed sprites come from corrupt vertex
  data, not this path.

## From F3DDKR renderer workstream (M3-gfx, commit a8dcd00)
- [x] Backends include `../gfx_pc.h` (missing) — added `platform/gfx_pc.h` shim that includes `fast3d/gfx_pc_dkr.h` (M2).
- [x] `gfx_ptr` registry globals (keys/vals/state, segment table, counters) — defined once in `platform/gfx_ptr.c` (M2).
- [~] OS shim must `gfx_ptr_store()` EVERY host pointer that reaches a DL word — on LP64 (not wasm32) `dkr_resolve()` reconstructs arena pointers from their low-32 via `dkr_lo32_to_ptr()` (arena is size-aligned), so per-word `gfx_ptr_store` is NOT needed for arena data. The registry now backs only segment-table sets + non-arena host pointers. Revisit for a wasm32 (ILP32) target.
- [x] `ultratypes.h` uses `uint32_t` for native `u32`/`vu32`; display-list,
  matrix, triangle, texture, and object layouts are compile-time locked and
  exercised natively and in wasm.
- [x] Renderer linkage: `gfx_init(&gfx_opengl_api)` + `gfx_set_dimensions(w,h)` in main_pc; the M_GFXTASK dispatch (stubs_dkr.c) brackets `gfx_run(dl)` with `gfx_start_frame`/`gfx_end_frame`; present/swap stays the shim's job (platform_frame_sync) (M2).
- [x] Backface winding is exercised across every track, menu, boss, both
  renderers, and the browser; the original first-render hypothesis is obsolete.
- [x] `gfx_get_pixel_depth` had no callers and returned invented zero depth. The
  misleading public API and implementation were removed; framebuffer readback
  remains an explicit supported vtable capability.
- Note: gfx_opengl.c is built with post-FX OFF (platform/gfx_config_shim.c) and GE-only auto-VI-filter heuristics NATIVE_PORT-gated out; two GE-tree headers are shimmed in platform/fast3d_shim (viGetX/Y=320x240, texDebug no-op). gfx_metal.mm + screenshot_series.c are excluded from the build (TODO M3/M7).

## M3 rendering state (UPDATED — M3b renderer bring-up)
- [x] The "white intro quad" was NOT the logo — it was the z-buffer clear
  FILLRECT drawn to screen because SETCIMG/SETZIMG resolved to NULL (the
  z-clear-skip heuristic mis-fired). Address resolution is now correct so it is
  skipped, and the real 3D geometry (previously all NULL-resolved) draws.
- [x] "Menu black = geometry off-screen/culled/mis-transformed" was a
  MISDIAGNOSIS. Root cause was ADDRESS RESOLUTION: DKR encodes DL pointers as
  `(u32)hostptr ^ 0x80000000` (OS_K0_TO_PHYSICAL / `(s32)p + K0BASE`), which the
  old dkr_resolve never undid, so every matrix/vertex/triangle/viewport/segment
  pointer resolved to NULL. See STATUS.md "M3b" for the full fix chain
  (dkr_resolve registry-then-arena, gDma1p single-eval, Mtx 64-byte pin, texture
  materialisation via load_texture header swap + align16 uintptr_t).
- [x] Menu-with-level-background "black in headless" was a no-input artefact:
  once M4 input advanced the game into the title screen (which loads a demo
  level) and the interactive menus, scene geometry is submitted and DRAWS.
  Confirmed via --input-script + dumped frames.
- [x] Intro/billboard sprite bars are resolved by field-oriented sprite calls,
  the shared asset-bounded sprite layout, correct absolute S10.5 UV handling,
  near-plane clipping, and aspect/FOV correction. Current native/browser visual
  gates exercise intro, menus, world billboards, and sprite layout.
- [x] RGBA16/IA16 texels + TLUT decoded big-endian in the decoder (M4). Texels
  stay BE at load (swap_texture policy); gfx_pc_dkr.c now reads the 16-bit values
  MSB-first. (IA8/I8/CI texels are single-byte and were already OK.)
- [x] The unused `gfx_get_pixel_depth` zero-return stub was removed rather than
  advertised as renderer behavior.
- [x] The `obj_init_animobject` host-only defensive return is gone. The earlier
  claim that supported content produced mis-resolved animation targets was not
  reproduced once the binding became measurable. The production seam now
  requires an animation-capable behavior, matching runtime/header behavior IDs,
  and a complete arena-backed `Object_AnimatedObject`; invalid data is attributed
  and fatal rather than silently skipped. The all-20-track gate observed 1,332
  valid initializations and zero invalid bindings, with menu/challenge/boss
  routes checked separately.
- Remaining LP64 pointer-truncation / pointer-array-size bugs almost certainly
  exist beyond the render path (only sites hit by the boot->menu path were
  fixed). Pattern to watch: `(Type*)(u32)ptr` / `(s32)ptr` gratuitous round-trips
  and `count * 4` pointer-array allocations (should be `* sizeof(ptr)`), and
  pointer values stored in s32/void*[] slots then read back without dkr_lo32_to_ptr.

## Menu 1:1 fidelity (frontend Timber's Island background) — wave "menufi"

### FIXED: the frontend title menu now renders Timber's Island, not flat blue

**Symptom (oracle-confirmed).** `tools/run_oracle.sh title_to_options` showed our
port drawing the DIDDY KONG RACING logo + START/OPTIONS over a flat blue sky/ocean
(the ocean region measured a perfectly uniform RGB(49,98,222), zero variance, zero
frame-to-frame delta), while the real ROM (ares) draws them over the Timber's
Island beach — sand, palm trees, tropical foliage and an animated character.

**Root cause: the object map's per-entry BODY params were never byteswapped.**
`platform/asset_swap.c swap_level_object_map()` byteswaps only the common
per-entry `x/y/z` (+0x02/+0x04/+0x06) and explicitly punts everything from +0x08
("Per-behavior body params (>= +0x08) PUNTED"). Those body params are polymorphic
per object behaviour, so they cannot be swapped without resolving the behaviour.
The frontend title (level 23, `RACETYPE_CUTSCENE_1`, `CUTSCENE_ID_NONE`) has no
racers and no cutscene camera: its camera is driven entirely by a
`BHV_CAMERA_ANIMATION` object, which is spawned indirectly by a `BHV_ANIMATION`
"director" via `LevelObjectEntry_Animation.objectIdToSpawn` (s16 @ +0x0C).
Left big-endian, that field read as garbage (e.g. 0xB200 = -19968 instead of
0x00B2 = 178), so every director spawned the wrong target — the `AnimCamera`
object never existed, `obj_loop_animcamera()` never ran, `write_to_object_render_stack()`
(the only writer of the misc/title cameras) was never called, `gCutsceneCameraActive`
stayed FALSE and the camera sat at the `cam_init()` default (200,200,200) — a
viewpoint that happens to look at open ocean. The island geometry and all 244 map
objects were loading correctly the whole time; only the camera was wrong.

**Fix.** `mdkr_objmap_swap_bodies()` / `mdkr_objmap_swap_entry_body()` in
`game/src/objects.c`, run from `track_spawn_objects()` right after the object map
is inflated and x/y/z-swapped, before the spawn loop. It walks the entries, resolves
each entry's behaviour exactly the way `spawn_object()` does (level-object
translation table -> object header), and byteswaps that behaviour's 16-bit body
fields. Field offsets come from `game/include/level_object_entries.h`; the
behaviour->struct mapping is the `obj_init` dispatch (objects.c ~L10390). Every
field is swapped only when it fits inside the entry's real stride (`entry[1] & 0x3F`),
so short entries are never over-read. Behaviours covered (the ones with 16/32-bit
body fields): RACER, AUDIO, AUDIO_LINE(+_2), FOG_CHANGER, TEXTURE_SCROLL,
LIGHT_RGBA, WEATHER, LENS_FLARE, LENS_FLARE_SWITCH, CHARACTER_FLAG, ANIMATION.
Behaviour lookups are cached for the program lifetime (`mdkr_obj_behavior_of`) —
`load_object_header()` gzip-decompresses on a miss, so resolving per entry without
a cache made level loads pathologically slow.

**Verified.** `objectIdToSpawn` now reads 178/155/151/149/…; the director spawns
`AnimCamera` (bhv 51), `obj_loop_animcamera` runs, and the frontend renders the
beach: sand, leaning palms, foliage, the animated character + its shadow, sky.
Oracle `title_to_options` (native re-run vs cached ares frames):
`menu_shown` 57.7% -> 63.6%, `navigated` 49.5% -> 58.2%. Those numbers understate
the change — the montage shows the scene going from entirely flat blue to the
correct beach; the residual delta is camera-flythrough phase between the two
runners (a dynamic scene at best-effort sync, per docs/ORACLE.md), not a render gap.

### Also fixed: shadow stack overrun (`func_8002F440`, game/src/tracks.c)
Once the frontend's animated character spawned, its shadow crashed the port with
`__stack_chk_fail`. `func_8002F440` declares `sp90[6]`/`sp80[6]` but indexes them
by `D_8011C238[].unk0`, which is `func_8002FF6C(3, …, 4, …)`'s output — clipping a
3-vertex triangle against a 4-plane frustum yields up to **7** verts (the struct's
own `unk2[8]` bounds it). Writing index 6 is a benign stack overrun on N64 (no
canary) but fatal under `-fstack-protector`. Sized to `[8]` under NATIVE_PORT;
the N64 declarations are untouched.

### FIXED: object-header table overrun (was "latent object-header pool corruption")
The in-race crash — `racerfx_alloc()` -> `spawn_object()` on an object header that
reads back **all zeros** — was an LP64 heap overrun of the object-header table
itself, not pool corruption and not a use-after-free.

**Root cause (`game/src/objects.c`, `allocate_object_pools()`).**
```c
gLoadedObjectHeaders = mempool_alloc_safe(gAssetsObjectHeadersTableLength * 4, ...);
gObjectHeaderReferences = mempool_alloc_safe(gAssetsObjectHeadersTableLength, ...);
```
`gLoadedObjectHeaders` is declared `ObjectHeader *(*)[ASSET_OBJECTS_COUNT]` — an
array of **real host pointers**, 4 bytes on N64 but **8 on a 64-bit host**. The
allocation hardcodes 4. On US v80 the table has 304 entries: 1216 bytes allocated,
2432 needed. Every header index >= 152 therefore stored its 8-byte pointer **past
the end of the allocation**, and mempool places `gObjectHeaderReferences`
immediately after it (base+1216), so those stores land directly on the refcount
array — and, for indices >= ~190, on whatever was allocated after that.

**Evidence** (instrumented Release build, frontend only, 400 headless frames, no
input):
```
[HDRDBG] tableLen=304  hdrTable base=..f220 alloc=1216 bytes need=2432 bytes  refs base=..f6e0
[HDRDBG] entry stride=8  last valid idx within alloc=151  refs delta=1216
[HDRDBG] new max index=133 -> slot ..f648 (alloc ends ..f6e0)
[HDRDBG] new max index=267 -> slot ..fa78 (alloc ends ..f6e0)   *** OVERRUN ***
[HDRDBG] new max index=270 -> slot ..fa90 (alloc ends ..f6e0)   *** OVERRUN ***
[HDRDBG] new max index=299 -> slot ..fb78 (alloc ends ..f6e0)   *** OVERRUN ***
[HDRDBG] *** refs[] SPLATTERED: 5 entries have refcount!=0 with a non-arena header ptr
[HDRDBG]     refs[112]=16 refs[113]=113 refs[114]=21 refs[115]=76 refs[116]=5
             (== little-endian 0x054c157110 — the header pointer for index 166,
              whose slot &hdrTable[166] is byte-identical to &refs[112] at the
              8-byte stride)   hdr[112..116] = NULL (those indices never loaded)
```
The mechanism of the crash follows directly: a splattered refcount byte makes
`load_object_header()` take its `refs[index] != 0` "already loaded" branch and
return `(*gLoadedObjectHeaders)[index]` — a slot that was **never written**. The
header was never "freshly loaded and zeroed"; it was never loaded at all.

**Why the menu-fidelity fix exposed it.** Correcting `objectIdToSpawn` makes the
frontend's animation directors resolve to their real targets, which live at HIGH
header indices (267 / 270 / 299). Before the fix those indices were never loaded,
so nothing ever wrote past 151.

**Fix.** Size the allocation with `sizeof(ObjectHeader *)` (== 4 on N64, so the
matching build is unchanged) and NULL-initialise the table. Hardening on the same
lifecycle: `try_free_object_header()` now nulls `(*gLoadedObjectHeaders)[index]`
after the free (the original leaves a dangling pointer — benign on N64 because the
refcount alone gates the cache, but it turns one bad refcount byte into a
use-after-free); `load_object_header()` aborts loudly on `refcount != 0` with a
NULL slot; and the defensive NULL-`modelIds` guard in `spawn_object()` is replaced
by a loud abort, so a future corruption cannot hide as a silent failed spawn.

**The previous "ruled out" list was correct** — pool byte size, slot count,
`OBJECT_BLUEPRINT_SIZE`, `gObjPtrList` overflow and animation-target property
sizes were all innocent. ASan is blind to this one for a second reason beyond the
single-allocation pool: the overrun is a plain in-arena write within one mempool
region, so there is no redzone anywhere near it.

**Validated** (all headless, `MDKR_AUDIO=off`): `nav_to_time_trial_race` 20/20,
`race_drive_time_trial` 20/20, all 8 menu-nav fixtures 20/20 each, title 300f x3.

### Also FIXED (found while validating): object sub-pool too small for LP64 objects
Letting the title screen idle past ~frame 5100 starts the **attract demo**, which
loads real race levels. The second one crashed at `objects.c` line 1526,
`racerObj->trans.rotation.y_rotation = ...` — the original code assumes a racer
`spawn_object()` never fails. Instrumentation showed it returning NULL from the
`mempool_alloc_pool(gObjectMemoryPool, sizeOfobj) == NULL` path:
```
[SPDBG] pool 1 NO BLOCK for 2032 bytes: slots 297/512, free total=1824
        largest=1824 used=86240 (pool data=88064)
[SPDBG] spawn_object NULL path #6 hdr=6 objType=81
```
Not a leak — a probe at every `level_load` showed pool 1 at `usedBytes=0,
freeTotal=88064` each time. It is genuine byte exhaustion: **everything allocated
in that sub-pool (`Object` + its trailing payload — `modelInstances[]`, shading,
shadow, interaction, collision, attach points, emitters, `lightData[]` — plus the
`ObjectHeader`s) is built from real host pointers**, so the same scene costs
roughly twice the bytes on LP64 as it does on N64, while `OBJECT_POOL_SIZE` was
still the N64 budget `0x15800`. Slot count was never the limit (peak 297 of 512).
Fix: `OBJECT_POOL_SIZE` scales by `sizeof(void *) / 4` under NATIVE_PORT (exactly
`0x15800` on N64, `0x2b000` here). Measured peaks against the new budget: driven
Time-Trial race `0x12fe0` (77792); attract demo through its first track `0x14b50`
(84752); the second demo track — the one that crashed — demanded at least
`0x158d0` (88272 = 86240 in use plus the 2032-byte request that failed), i.e. more
than the entire old `0x15800` budget on its own. This crash reproduces identically
**before** the header-table fix, i.e. it is a separate pre-existing bug that the
menu-fidelity camera fix made reachable (the title cinematic now actually advances
into the attract demo).

Two follow-ons so this class stops presenting as an unexplained SIGSEGV: a failed
pool allocation now prints the pool's occupancy (bytes vs slots, largest free
block) to stderr instead of being silent — `stubbed_printf` is a no-op, so DKR's
`"Memory fail!!"` messages never went anywhere — bounded to the first 8 reports;
and `track_setup_racers()` aborts with a message on a NULL racer spawn rather than
dereferencing it.

**Regression coverage.** No `nav_*` fixture reaches the attract demo (they all
press START at ~frame 1250). The soak that does is just a long idle headless run —
see `tests/README.md`.

### FIXED: attract demo hung in the audio sequence player (was a blocker)
*Symptom as originally found:* with the two fixes above the attract demo no longer
crashed — it **hung**. Idle the title screen: demo level 18 loads at ~frame 5132
and plays, level 28 loads at ~frame 6632, and a few frames later the process pins
one core at 100% and stops advancing frames entirely. `sample(1)` put 100% of
stacks in
`main_game_loop -> fb_update -> osRecvMesg -> dkr_audio_service_tick -> amAudioSynthFrame ->
alAudioFrame -> __CSPVoiceHandler / alEvtqNextEvent`; the game loop itself is fine.

**Root cause chain (each step measured):**
1. `game/src/audio.c:247` sizes the music sequence player's event queue at 120
   entries (`sound_seqplayer_init(24, 120)`).
2. During the level-18 demo that queue **saturates**. Instrumenting
   `alEvtqPostEvent`'s empty-free-list branch (then in the decompiled event
   queue, now `platform/audio_event_queue.c`):
   `[EVTQ] post DROPPED (free list empty) allocList=120 type=9`. A normal driven
   race is nowhere near the limit — `race_drive_time_trial`, 4300 frames with
   full music and SFX, produces **0** drops — and the queue depth sampled at every
   `level_load` is 0 / 49 / 56 / 50 / 61 of 120, so this is a transient burst on
   the demo path, **not** a leak across level loads.
3. `alEvtqPostEvent` silently drops the event when the free list is empty. libultra's
   sequence player is **self-perpetuating** — each handled event posts its
   successor — so dropping one breaks the chain and the queue then drains to empty.
4. On an empty queue `alEvtqNextEvent` returns `evt->type = -1,
   delta = 0`. `__CSPVoiceHandler` is a
   `do { switch (type) … } while (seqp->nextDelta == 0);` with **no case for -1**,
   so it spins forever. Confirmed directly: `[CSPDBG] spin iter=100001 evtType=-1
   nextDelta=0 frameTime=16000`, incrementing without bound.
   The original libultra sources documented this state as an out-of-resource
   condition caused by overflowing the event queue with non-self-perpetuating
   events, with "enlarge the queue" as the prescribed remedy — but did not
   defend against reaching it.

**Attribution — not caused by the two fixes above.** Neither touches audio; a
normal race with music shows zero queue pressure; and the pre-fix build crashes in
`track_setup_racers` at the level-28 load, i.e. strictly *before* this point, which
is why the hang was never seen. It is newly reachable for the same reason the pool
exhaustion was: the menu-fidelity camera fix makes the title cinematic actually
advance into the attract demo. On `main` the demo never starts at all (verified:
12000 idle frames, no demo level loads), so `main` cannot hit it.

**THE FIX — both remedies, in the order of principle. Applied.**

*1. The spin is now impossible (primary, and correct regardless of sizing).* An
out-of-resource condition must degrade, never hang.
- `game/include/PR/libaudio.h` — the -1 sentinel is named `AL_EVTQ_EMPTY_EVT` and
  the hazard is documented at the definition. It is **not** an event type; it is
  "the queue is empty".
- The compact sequence player's `__CSPVoiceHandler` (then in the decompiled
  synthesiser, now `platform/audio_event_queue.c`) and
  `game/src/audiosfx.c` (`sndp_voice_handler`) — the only two handlers with this
  shape in the tree — now test for the sentinel immediately after
  `alEvtqNextEvent()`, re-arm the player's own self-perpetuating API heartbeat
  (`AL_SEQP_API_EVT` / `AL_SNDP_API_EVT`), and leave with `nextDelta = frameTime`.
  The player idles a frame and stays responsive; worst case the sequence in flight
  stops. (`seqplayer.c`'s `__seqpVoiceHandler` is declared but never defined —
  DKR uses only the compact sequence player and `alSeqpNew` is a stub — so there is
  no third site.) audiosfx.c's `default:` arm was additionally feeding the sentinel
  into `sndp_handle_event()` as if it were a real event; that is closed too.
- Returning **0** from a handler would not have worked: `synthesizer.c`'s
  `__allocVoiceHandler` does `client->samplesLeft += _timeToSamplesNoRound(drvr,
  (*client->handler)(client))`, so a 0 return just relocates the spin into the
  driver's `for`-loop. The guard must return nonzero.
- The event queue (now `platform/audio_event_queue.c`) — a dropped post is no longer silent: counted
  in `alEvtqDropCount` and reported to stderr (capped at 8 lines). The silence is
  most of why this was expensive to find.

*Proof the spin fix alone is sufficient:* with the queue deliberately forced back
to 120 (so the drop still happens) the 12000-frame idle soak exits **rc=0** at
frame 12000, logging exactly `1` dropped post and `0` crashes, with audio still
being synthesised across the drop (running RMS ~7100 before and after, total
5410 over the run). Previously this run wedged at frame 6634.

*2. The queue size, decided on measurement.* `MDKR_EVTQ_STATS=1` (added to
`event.c`) reports per-queue high-water marks. Measured:

| queue | budget | peak (attract demo) | peak (race_drive_time_trial 4300f) |
|---|---|---|---|
| music seq player | 120 | **121** | 71 |
| jingle seq player | 50 | 3 | 3 |
| SFX player | 150 | 36 | 56 |

The demand is real, not a headless artifact: the headless pump uses a **fixed
frameSize** (`platform/audi_port_dkr.c`), so simulated audio time per rendered
frame is identical to a real-time run, and queue depth is a function of how far
ahead the sequence schedules events, not of wall-clock speed. And it is bounded —
re-measuring with the queue temporarily at 1024 gives the **same** peak of 121
with **0** drops. So 120 is simply one entry short at the frame the demo level's
sequence starts; nothing is running away and nothing is leaking (the earlier
per-`level_load` depths of 0/49/56/50/61 were the same transient-burst picture).
`game/src/audio.c` now uses **256** under `NATIVE_PORT` (~2.1x the measured peak,
~6.5 KB of the 16 MB arena; `sizeof(ALEventListItem)` ~48 bytes on LP64). The
jingle and SFX queues are left alone — ample headroom. With 256 the 12000-frame
soak logs **0** drops.

**Repro of the original hang** (for the record; fixed now):
`MDKR_AUDIO=0 MDKR_TRACE=1 ./build/mdkr64 --headless-frames 8000
--rom baserom.us.v80.z64 > out.log 2>&1` — `out.log` stopped growing at
~frame 6634 while the process kept burning CPU. Note the trace output must be
redirected to a **file**: capturing it in a shell variable (`out=$(…)`) makes zsh
re-append a multi-megabyte string per line and looks exactly like a hang. Note
also that `MDKR_AUDIO=off` does **nothing** — `audi_port_dkr.c` tests
`disable[0] == '0'`, so only `MDKR_AUDIO=0` disables the device. `--headless-frames`
is the actual guarantee of silence.

### FIXED: boost sub-asset ASSET_MISC_20 decoded to garbage (endianness + LP64 stride)
Revealed, not caused, by fixing the hang above: past the old ~6634 wedge point the
demo reached ~frame **6957** and segfaulted in `render_sprite_billboard`
(`game/src/camera.c:1188`, `sprite->numberOfFrames`) called from
`func_800135B8` (`game/src/objects.c:4191`, boost graphics) with `sprite == NULL`.

Fixed in the `phase1-boost` wave — `asset_swap_misc_boost()`
(`platform/asset_swap.c`) + `dkr_boost_table()` / `GET_BOOST_TABLE()`
(`game/src/objects.{c,h}`). The `render_sprite_billboard` NULL-sprite skip that was
added as hardening is now a **loud `[FATAL]` + `abort()`** instead: a NULL sprite
there is a bug on real hardware too (the N64 original has no check), and a silent
skip is exactly what hid this defect.

**Root cause, measured.** A raw dump of `get_misc_asset(ASSET_MISC_20)`:
```
000: 00000000 41c00000 42800000 3f800000   <- 41c00000 = BIG-endian 24.0f
060: 40800000 43400000 42000000 002f00ce   <- 0x6C: spriteId=47 texId=206 (BE)
080: 00000000 41c00000 42800000 3f800000   <- entry 1: ROM stride is 0x80
0e0: 40800000 43400000 42000000 002e00cf   <- entry 1: spriteId=46 texId=207
```
Two independent defects:
1. **The sub-asset is never byte-swapped.** It is big-endian in memory, so every
   `f32` the game reads out of `Object_Boost_Inner` is a denormal (~0) and the
   `s16` IDs are byte-reversed. There *is* an established mechanism for this —
   `dkr_misc_swap_words()` (`objects.c:8524`), used for the wheel-collision points
   and the stone-grip table — but `ASSET_MISC_20` is not registered with it, and a
   blanket 32-bit word swap would be **wrong** here anyway: it would scramble the
   packed `s16 spriteId / s16 textureId` pair at 0x6C and the `u8` quad at 0x70.
   This one needs a per-field swizzle.
2. **LP64 stride mismatch.** The ROM entry stride is `0x80`; `sizeof(Object_Boost)`
   is `0x88` here (`0x78 Sprite *sprite`, `0x80 TextureHeader *tex` — two 8-byte
   host pointers where the N64 had two 4-byte ones). So `boostObj[i]` walks off
   after entry 0 regardless of endianness. Observed decoded `spriteId`s across the
   10 entries: `12032, 0, 0, -16319, -32705, 66, 16451, 0, 16450, 0` — halves of
   float words. `tex_load_sprite()` returns NULL for the implausible ones, which is
   the NULL that crashed the renderer.

**Pre-existing and NOT demo-specific.** An instrumented `race_drive_time_trial`
decodes byte-for-byte the same garbage. The demo is simply the only fixture in
which a racer boosts, so it is the only one that reaches the draw. Boost graphics
have therefore never rendered correctly on this port.

**Confirmed on-disk layout (0x80 bytes).** Hand-decoded from the raw ROM bytes of
all ten entries (us.v80: misc word offset 178, 1280 bytes = 10 × 0x80):

| off | type | entry 0 value | note |
|-----|------|---------------|------|
| 0x00 | `Object_Boost_Inner carBoostData` | pos (0, 24, 64), 1.0, 0.1, 32, 4, 192, 32 | 9 × f32 |
| 0x24 | `Object_Boost_Inner hovercraftBoostData` | pos (0, 64, 48), same tail | 9 × f32 |
| 0x48 | `Object_Boost_Inner flyingBoostData` | pos (0, 8, 100), same tail | 9 × f32 |
| 0x6C | `s16 spriteId` | `0x002F` = 47 | BE on disk |
| 0x6E | `s16 textureId` | `0x00CE` = 206 | BE on disk |
| 0x70..0x73 | `u8 unk70,unk71,unk72; s8 unk73` | 0 | runtime state; **not** swapped |
| 0x74 | `f32 unk74` | 0.0 | runtime state |
| 0x78 | `u32 sprite` | 0 | runtime host pointer (8 bytes on LP64) |
| 0x7C | `u32 tex` | 0 | runtime host pointer (8 bytes on LP64) |

`Object_Boost_Inner` is exactly 9 × f32 (0x24). Entry 9 is the odd one out — its
`flyingBoostData` is pos (0, 30, 64) / 2.2 / 0.3 / 18 / 2 / 200 / 40, i.e. a bigger
flame; that is the `gBoostObjOverrideID = 9` slot used by
`obj_loop_pigrocketeer()` for Wizpig's rocket. Everything below 0x78 has identical
host and on-disk offsets (all naturally-aligned ≤4-byte scalars, and 0x78 is
already 8-aligned), which is what lets the converter `memcpy` the prefix and then
swap at on-disk offsets. Locked with `_Static_assert`s in `objects.c`.

**The fix.**
1. `asset_swap_misc_boost(dst, dstStride, dstCapacity, src, srcBytes)`
   (`platform/asset_swap.c`) — per-field swizzle (27 `f32`, two `s16`, four bytes
   left alone, one `f32`) **plus** a host-layout copy at `sizeof(Object_Boost)`
   stride, zeroing the two trailing pointer fields.
2. `dkr_boost_table()` (`objects.c`) owns a `static Object_Boost[10]` and converts
   **once** per `gAssetsMiscSection` load (dedup on the section pointer, same
   pattern as `dkr_misc_swap_words`) — the game writes runtime state back into the
   array, so a rebuild would wipe it. Aborts loudly if the sub-asset length is not a
   multiple of 0x80 or does not yield 10 entries.
3. All nine consumers go through `GET_BOOST_TABLE()` (`objects.h`), which is
   `dkr_boost_table()` on NATIVE_PORT and the plain `get_misc_asset()` cast
   otherwise, so the N64 build is unchanged and no call site can index the raw blob.

**Decoded `spriteId/textureId`, before → after:**
```
before: 12032/-12800  0/0  0/0  -16319/0  -32705/0  66/0  16451/0  0/0  16450/0  0/1024
after:     47/206    46/207 53/209 53/209  49/208  49/208 49/208 49/208 49/208 49/208
```
(`MDKR_TRACE=1` prints the `after` line once per section load: `boost_table: 10
entries; spriteId/textureId: …`.)

**Visual verification.** `MDKR_FORCE_BOOST=<frame>[:<len>]` (`objects.c
dkr_force_boost_hook`, no-op unless set) hands every racer the exact state a
`SURFACE_ZIP_PAD` gives it, so a dumped frame is guaranteed to contain a boost. With
`race_drive_time_trial` and `MDKR_FORCE_BOOST=2830:120`, frame 2820 (control) shows
clean karts and frames 2850/2880/2910 show a bright character-tinted exhaust plume
out the back of every kart (player white/yellow, Banjo green-yellow, Krunch
magenta) — see `tests/README.md`. Two things make a plain fixture insufficient and
are worth knowing:
- **The attract demo boosts, but never on camera.** Racers 1/2/3 boost at
  ~5494–5589, 6957–7040, 7663–7786 and 8128–8132; the demo camera does not follow
  them, so `render_level_geometry_and_objects()` correctly culls their boost objects
  (the boost object is `move_object`'d to the racer, picks up that racer's
  `segmentID`, and that segment is not in the visible set). Racer 0 never boosts.
- **`race_drive_time_trial` does reach a zip pad — far too late.** The player's
  first boost is at ~frame 5448, long after the racer had already been stranded (see
  the item below), so the whole scene was culled by then. *Superseded:* with the
  stranding fixed, `race_drive_long.txt` takes a genuine zip pad on camera at
  ~frame 5650, so `MDKR_FORCE_BOOST` is now a convenience rather than the only way
  to get a boost into a dumped frame.

### Driven racer gets stranded mid-race, then the whole scene stops drawing — FIXED
Found while looking for a fixture that reaches a zip pad (boostfx wave). NOT a boost
bug and not a renderer bug — a big-endian asset table nobody had classified.

**Symptom.** Running `race_drive_time_trial` (or any held-throttle route) on Ancient
Lake: the racer accelerates normally out of the start (~11.9 world-units/frame), then
its position jumps backwards and up, oscillates, and reaches `-inf, 34, -inf`. Once
the camera follows a non-finite position, `traverse_segments_bsp_tree()` returns
**zero** visible segments, `objectsVisible[]` is all-FALSE except index 0, and every
object with a real `segmentID` is culled in all three loops of
`render_level_geometry_and_objects()`. The dumped frame is a flat fog-brown field
with only the HUD and minimap on it, race timer still counting. No crash and no
assert, so every fixture in the matrix still passed.

**Root cause — `ASSET_MISC_8` is a big-endian `f32[10]` that nothing byte-swapped.**
`ASSET_MISC` is heterogeneous and is deliberately punted by `asset_swap_normalize()`
(docs/asset_swap_notes.md). Only the handful of sub-assets that an earlier wave had
tripped over were normalized, by *lazy* `dkr_misc_swap_words()` calls at the consumer.
Sub-asset 8 — the per-character steer-slide divisor — was not one of them.

A BE `f32` read natively is a **denormal**, not an obvious zero: `600.0f`
(`0x44160000`) reads back as `0x00001644` = 7.99e-42. The whole table, raw:
`440E8000 44160000 44480000 44160000 44160000 44160000 44138000 44610000 44160000
44160000` = 570, 600, 800, 600, 600, 600, 590, 900, 600, 600 — plausible per-character
values, all denormal in the wrong byte order.

Denormals behave like `0.0` almost everywhere, so a wrong table is silent until
something DIVIDES by it. `func_80050A28()` (`game/src/racer.c`, "degrade lateral
velocity") does exactly that:

```c
racer->lateral_velocity += (racer->velocity * gCurrentStickX) / miscAsset[racer->characterId];
```

With the stick centred the numerator is `-0.0`, so the quotient is `-0.0` and nothing
shows. On the FIRST steering input at speed (the guard is `velocity <= -2.0`,
`drift_direction == 0`, `!raceFinished`) the quotient overflows f32:

```
[RDBG f=3200] P6 after-attack   lat=0.000042  v=-11.971550   (stickX=0)
[RDBG f=3200] MISC8 charId=9 val=7.9874e-42 bits=00001644
[RDBG f=3201] MISC8 stickX=70 v=-11.9715
[RDBG f=3201] P7 after-physics  lat=-inf      v=-11.976278
```

and from there, all inside the same frame:

```
lateral_velocity = -inf
  -> mtxf_transform_point(sp60, lat, 0, vel, &x_velocity, …, &z_velocity)  (racer.c func_8004F7F4)
  -> obj->x_velocity = obj->z_velocity = -inf
  -> move_object(obj, -inf, -0.45, -inf)
  -> obj->trans.x/z = -inf     (y stays finite — only x/z go through the velocity path)
  -> the 4 wheel points, the segment lookup, the camera, the BSP walk.
```

The onset frame is exactly the frame the input script first deflects the stick
(`3200 RIGHT 220` in `race_drive_time_trial.txt` → break at 3201). The "jumps
backwards and up" that the original report saw is the game's own out-of-bounds
recovery putting the racer back at a checkpoint with `y_velocity = 9.5`, over and
over, roughly every 190 frames.

**Fix** (`objects.c dkr_misc_normalize_tables`, called from `allocate_object_pools()`
right after the MISC section loads): normalize every `ASSET_MISC` sub-asset that is a
pure 32-bit-word (`f32`/`s32`) array **once, from one explicit documented list**,
instead of lazily at whichever call sites someone happened to notice. Newly covered:
`MISC_4` (per-character model scale), `MISC_8`, `MISC_17`, `MISC_18`, `MAGNET_DATA`,
`MISC_32` (stone grip — the previous notes claimed it was covered; it was not) and
`RACER_HITBOX_SIZE`. Deliberately NOT listed, because a blanket word swap scrambles
them: `SHIELD_DATA` (4×s16 + 2×f32), `MISC_20` (boost record — has its own per-field
swizzle), `RUMBLE_DATA` and `MISC_23` (s16 arrays). *Those four are now covered too —
see the next section.*

### CLOSED: the last four big-endian ASSET_MISC sub-assets (wave "miscswap")
`SHIELD_DATA` (21), `RUMBLE_DATA` (19), `MISC_23` (23) and `GHOST_UNLOCK_TIMES` (24)
— the sub-assets the previous wave listed as needing a per-field swizzle — are now
normalized. `dkr_misc_swap_word_tables()` is renamed **`dkr_misc_normalize_tables()`**
(it is no longer word-only) and runs three explicit lists plus a verification pass.

**Two new mechanisms**, both in `game/src/objects.c`:
- `dkr_misc_swap_halfwords(index)` — the 16-bit sibling of `dkr_misc_swap_words()`,
  sharing the one-shot `sMiscSwapDone[]` dedup (the flag is per *index*, not per
  kind, so a sub-asset is swapped exactly once in exactly one way). Covers
  `RUMBLE_DATA`, `MISC_23`, `GHOST_UNLOCK_TIMES`.
- `dkr_misc_swap_shield_records()` — per-field, **in place**. The handoff expected
  this to need the boost table's copy-out treatment; it does not.
  `RacerShieldGfx` is `4×s16 + 2×f32` with no trailing pointer fields, so
  `sizeof` == the 0x10 on-disk stride on N64 and LP64 alike (locked with
  `_Static_assert`s on the size and all six field offsets). 480 bytes ÷ 0x10 =
  exactly 30 records = `NUMBER_OF_PLAYER_VEHICLES(3) × NUMBER_OF_CHARACTERS(10)`,
  matching the `shield[vehicleID * 10 + racerIndex]` indexing in
  `render_racer_shield`.

**Why a 32-bit swap is wrong for these** (the reason they were excluded, now
concrete): a word swap does not just reverse each halfword, it *transposes the two
halfwords inside every word*. For `RUMBLE_DATA` that swaps strength with duration
in every `{strength, duration}` pair.

**`SHIELD_DATA` is not the usual silent-denormal case.** Read in native order,
`scale` (`0x3ECCCCCD`) decodes to **−4.29e8**, not ~0 — an active shield would scale
the effect object by a huge *negative* factor. `turnSpeed` does go denormal
(1.0f → 4.6e-41), and `y_position`/`y_offset` read 3072/1024 instead of 12/4.

**Decoded values, us.v80** (`MDKR_TRACE=1` prints these once per section load):
```
shield_table: 30 records; [0] pos=(0,12,0) y_off=4 scale=0.4000 turnSpeed=1.0000
rumble_table: 19 pairs; 45/18 70/10 60/60 100/15 60/54 50/15 ...
misc23_table: 576 u16 (48 levels x 12); [0] course=10800/34 lap=3600/34
ghost_unlock_times: 20 entries; 4020 5820 4080 5700 4740 5640 5940 4200 ...
```
10800/3600 frames = the 3:00 course / 1:00 lap defaults; ghost times are 67–142 s;
the 20 ghost entries pair 1:1 with the 20 `MAIN_TRACKS_IDS` entries before its `-1`
terminator (verified).

**THE VERIFICATION PROBLEM, and what was done about it.** *No fixture in the
regression matrix reaches any of these four* — the shield needs a racer actually
holding a shield, `RUMBLE_DATA` needs a rumble pak, and `MISC_23` /
`GHOST_UNLOCK_TIMES` sit on save-file and time-trial-record paths. So "0 crashes
across the matrix" proves nothing about them, which is precisely how `MISC_8` stayed
big-endian for three waves. Three independent things stand in for a fixture:
1. **An independent offline decoder**, `tools/dump_misc_asset.py`, which
   re-implements `asset_table_load()`'s ROM lookup rather than reusing port code, so
   agreeing with the game's `[TRACE]` lines is real cross-checking. It agrees
   exactly on all four tables.
2. **Plausibility bounds** in `dkr_misc_verify_tables()`, run after every swap.
   Chosen so the correct decode clears them with room to spare and the byte-reversed
   decode violates them outright; a violation is a loud `[FATAL]` + `abort()`, the
   same policy as the boost table and the non-finite-position check.
3. **Positive controls** — each swap removed in turn, confirming its guard fires:

   | table | invariant | correct decode | swap removed → |
   |---|---|---|---|
   | `SHIELD_DATA` | `0 < scale, turnSpeed < 100` | 0.4 / 1.0 | `scale=-4.29492e+08 turnSpeed=4.6006e-41` → abort |
   | `RUMBLE_DATA` | every u16 ≤ 255 | max 100 | `RUMBLE_DATA[0] = 11520` → abort |
   | `GHOST_UNLOCK_TIMES` | every u16 ≤ 36000 | max 8520 | `GHOST_UNLOCK_TIMES[0] = 46095` → abort |
   | `MISC_23` | time fields ≤ 36000 | max 10800 | `MISC_23[36] = 45084` → abort |

   NaN and inf both fail the `SHIELD_DATA` comparisons, so no `isfinite()` is needed.
   `MISC_23`'s *initials* fields are deliberately unbounded — they are packed
   character triples, and byte-reversed they peak at 20749, under any useful bound;
   the time fields (indices ≡ 0, 2 mod 4, per `clear_lap_records`) discriminate on
   their own. Bounds rather than hardcoded expected values, because expected values
   would be ROM-version specific.

**Regression matrix** (all muted + headless): 9 menu fixtures ×20 = 0 crashes,
`race_drive_time_trial` ×20 = 0 crashes, `check_race_drive.py` PASS, 12000-frame
attract soak rc=0 with 0 `[FATAL]`/`[FX BUG]` lines. This shows no regression; per
the above it is *not* what shows the swizzles are right.

**Deliberate non-fix, documented so it is not mistaken for an oversight.**
`render_racer_shield` (`objects.c`) bounds with `if (racerIndex > NUMBER_OF_CHARACTERS)`
where `>=` is meant, so `racerIndex == 10` would index `vehicleID * 10 + 10` — for
`VEHICLE_PLANE` that is record 30, one past the 30-record table, reading into
`MAGNET_DATA`. It is decomp-faithful (the N64 does the same), stays inside the MISC
section so it cannot fault, and is unreachable in practice (8 racers, `racerIndex`
0..7). Left alone.

**Also added — the invariant, loudly.** A non-finite player position now prints
`[FATAL] update_player_racer: non-finite position …` and aborts
(`racer.c`, right after the per-vehicle update dispatch). It is never legitimate, and
silence is what let this survive a full validation matrix.

**Regression coverage.** `tests/input_scripts/race_drive_long.txt` +
`tests/check_race_drive.py` (see tests/README.md). Verified to FAIL on a deliberately
reverted build with "teleport: 1296.8 world units in one frame at frame 3363", "only
reached checkpoint 0 (want >= 20)" and "only reached lap 0". Render-liveness metric on
the real failure frame (`race_drive_time_trial` frame 4290): 59 distinct colours /
luma sigma 5.9, against 620..2720 / 22..47 on healthy frames.

**Follow-on: the racer now drives properly.** With steering fixed, the tuned fixture
crosses ~29 checkpoints, completes a lap and takes a genuine `SURFACE_ZIP_PAD` boost
at ~frame 5650 (sustained ~22.3 units/frame, ~2× the ~12 top speed) — so an
input-script route DOES now reach a zip pad, and `MDKR_FORCE_BOOST` is no longer the
only way to see boost graphics on camera.
