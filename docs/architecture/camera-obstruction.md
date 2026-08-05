# Camera obstruction and modern native presentation plan

> **Target:** safe obstruction correction becomes the native/browser default only
> after every CAM-00–CAM-09 release gate is evidenced.
> **Current state (2026-08-05):** experimental, release-gated implementation behind an
> explicit policy gate. The
> eight-slot presentation sidecar, projection-derived swept-sphere resolver,
> static/dynamic query sources, recovery, alternate shots, and emergency racer
> fade exist. Unset `MDKR_CAMERA_OBSTRUCTION` still selects Observe; this issue is
> not release-closed and correction is not yet the production default.
> **Evidence cutoff:** branch `codex/camera-obstruction-modern`, current candidate
> recorded in `docs/evidence/camera-runtime-modern-2026-08-05.md`; US v80 ROM SHA-256
> `7de1a8fb2a9558cfc3d9ad4497df698c1e89cf7095ac1531557df2af40ba8bcf`.
> **Scope:** native and browser ports under `NATIVE_PORT`; the matching N64 build
> remains untouched. **Product goal:** the best practical third-person racing
> camera on every supported screen and viewport, without changing vehicle physics,
> accepting render-driven simulation, or hiding failures behind the void curtain.

This is the implementation authority and living evidence record for the
camera-penetration defect. It records
the source investigation, the adversarial design review, the required architecture,
the work breakdown, and the evidence needed to call the defect closed. Target,
implemented, and release-evidenced behavior are distinguished explicitly.

## 1. Product decision

The native port will treat camera obstruction as a correctness boundary, not a
visual preset:

- A player-facing camera may not enter a hard visual occluder or expose geometry
  between the eye and its near plane.
- The existing DKR camera behaviors continue to author the desired shot. A native
  resolver produces the safe shot after every behavior, dialogue offset, and shake
  contribution is known.
- Safety is on by default in Pure, Restored, and Remastered modes. Pure remains an
  image-treatment/reference preset; it is not permission to retain a disruptive
  camera defect. A same-binary legacy mode exists only for diagnostics, fidelity
  comparisons, and the required broken-direction test.
- Resolution and pixel density never change the world-space camera. Effective
  aspect ratio, viewport shape, authored FOV, user FOV, and the horizontal FOV cap
  do, because they change the lens volume that must remain clear.
- The resolver is fixed-tick authority. Rendering consumes the resolved camera and
  never moves it. Window/FOV changes are latched so rendering cannot use a
  projection that has not been safety-validated.
- Vehicle physics and gameplay collision remain unchanged. Camera queries are
  pure: no interaction flags, door state, racer velocity, RNG, or object order may
  be written.
- Display aspect, FOV, resolution, and obstruction correction may not alter racer
  simulation or RNG. Presentation visibility uses the resolved camera; the legacy
  AI-activity admission gets an explicit canonical logical-camera basis.

This follows the mature spring-arm model—preserve an unobstructed desired boom,
retract on contact, and recover when clear—but raises the bar from a ray to the
actual lens guard. Unreal exposes a camera-specific probe, probe size, desired
unfixed position, collision displacement, and lag state in its
[SpringArmComponent](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/USpringArmComponent?lang=en-US).
Godot explicitly recommends sweeping the camera's near-plane pyramid because a
ray is inaccurate for camera collision in its
[SpringArm3D guidance](https://docs.godotengine.org/en/stable/tutorials/3d/spring_arm.html).
Those are design benchmarks, not dependencies.

## 2. Legacy mechanism and confirmed root cause

The defect is systemic:

1. `update_player_camera()` selects car, plane, hovercraft, loop, fixed, finish, or
   challenge behavior. Each behavior writes a desired world position directly;
   none performs an obstruction query.
2. Dialogue translation and shake then modify camera position after the selected
   behavior returns. A fix inside only `update_camera_car()` would therefore be
   incomplete.
3. Mode transitions can invoke `update_player_camera()` twice in one tick. A
   temporal solver placed there could integrate twice.
4. Most behaviors update `cameraSegmentID` before the late translations. Fixed and
   finish modes use the racer's Y rather than the camera's Y. Scene visibility is
   then seeded from a potentially stale or wrong segment.
5. `func_80027568()` does detect some terrain between a racer and camera, but only
   on levels with `useVoid`, and it excludes several camera modes. Its response is
   `void_check()`: synthesize a flat-colour curtain to hide the hole. It does not
   move the camera.
6. That detector queries terrain collision facets only. It does not include doors,
   props, or moving object models.
7. The shared gameplay terrain query keeps only the first ten overlapping level
   segments and writes a global 500-entry scratch list. The gameplay object query
   uses a separately capped list and writes interaction state. Neither is a safe
   or complete camera API.
8. The renderer's historical HLE near-plane defect is already fixed. Renderer
   clipping correctly clips a triangle after it reaches the lens; it cannot keep
   the eye outside the triangle.

A deterministic Ancient Lake witness reaches the canyon wall at frame 7700 and
holds the racer at `(-3040.91528, 29, -6474.04688)`. The captured view is dominated
and bisected by the adjacent wall. This is a useful visual witness, but final
acceptance must be based on geometric invariants as well as pixels.

### 2.1 Current implementation snapshot

- A native-only finalizer runs once per selected physical camera slot after the
  authored T.T. camera and before logical sort/LOD/visibility authority.
- All eight `gCameras` slots are snapshotted. Correction is published only to a
  presentation sidecar; gameplay, AI, audio event logic, and state hashes retain
  the authored cameras.
- The resolver latches an explicit per-viewport projection context, derives a
  conservative lens-enclosing sphere, and composes immutable static-track and
  dynamic hard-object queries without per-tick allocation. A ROM-free exact
  rounded eye-to-near-plane triangle narrow phase, continuous sweep, exact static
  and dynamic source adapters, fail-closed two-phase dispatcher, and opt-in
  authoritative-corridor shadow profiler, analytic swept-SAT front end, and
  bounded fail-closed grazing solver now exist. The exact two-phase result is now
  authoritative for every Modern final-pose path; the enclosing sphere remains
  its conservative broad phase and fallback. Dynamic model queries now use a
  deterministic immutable BVH for both enclosing-sphere and exact phases, with
  aggregate instance/node/chunk/triangle/stationary-test fences. Every final
  renderer pose/fallback tuple is sealed only after exact endpoint validation.
  The complete release matrix remains default-on blocking.
- Render view, distance, billboard/ortho tilt, model-relative calculations, and
  snapshots consume the scoped resolved camera. The render projection generation
  is checked against the fixed-tick validation record.
- Modern mode retracts immediately, recovers at a bounded fixed-step rate, tests a
  deterministic shoulder/elevation fan, recomputes the resolved segment and
  orientation, and can apply a per-viewport presentation-only racer fade when no
  readable boom fits.
- `legacy` and `center-ray` remain same-binary positive controls. Observe remains
  the current default until the ledger and definition of done are green.

### Source anchors for implementation

- Camera dispatch and late dialogue/shake position writes:
  `game/src/racer.c:8043-8121`; transition-time second update:
  `game/src/racer.c:8161-8176`.
- Direct follow-camera placement: hovercraft `game/src/racer.c:2428-2554`, plane
  `:3601-3762`, loop `:4045-4105`, and car `:8180-8374`; finish/fixed cameras
  `:8382-8460`.
- Conditional terrain detector and its 14-unit plane allowance:
  `game/src/tracks.c:1312-1429`; void-curtain call site `:2996-2997`; void purpose
  `:654-657`; `useVoid` allocation guard `:408-412`.
- Camera segment as scene-visibility seed: `game/src/tracks.c:2014-2031`.
- Terrain candidate limits and shared scratch population:
  `game/src/hasm/collision.c:46-320`; cap definition `game/src/collision.h:7`.
- Gameplay object collision list and side effects:
  `game/src/objects.c:7058-7095` and `:7697-7744`.
- Native fixed-tick ordering and the insertion seam:
  `game/src/thread3_main.c:590-700` and `:1177-1232`.
- Camera-driven AI visibility authority: `game/src/objects.c:5247-5321`.
- Effective display projection policy: `platform/display_config.h:1-87`,
  `platform/display_config.c`; view-matrix shake application:
  `game/src/camera.c:1506-1575`.
- Diagnostic camera-position interpolation:
  `platform/presentation_snapshot.c:1027-1041`.
- Fixed renderer near-plane mechanism and evidence:
  `docs/open-items/renderer.md:973-1008`.

### The 14-unit clue

The old detector subtracts 14 units from the camera-to-facet plane distance. That
number closely matches the stock lens:

```text
near = 10
vertical FOV = 60 degrees
aspect = 4/3
half-height = near * tan(vfov / 2)             = 5.7735
half-width  = half-height * aspect             = 7.6980
eye-to-near-corner = sqrt(near^2 + w^2 + h^2) = 13.88
```

The likely intent was therefore clearance for the original near-plane pyramid,
not merely a point camera. It does not generalize: 16:9, 21:9, portrait, split
viewports, authored zooms, user FOV, and the horizontal cap produce different
near-plane dimensions. The detector also uses the value only to decide whether to
draw the void curtain.

## 3. Red-team findings: ways a plausible fix would still fail

These are release blockers, not optional refinements.

| Tempting implementation | Why it fails |
|---|---|
| Raycast from racer origin to camera center | A clear center ray does not prove that the near plane or its edges are clear; the racer origin is also not the authored focus/pivot. |
| Fixed 14-unit sphere | Correctly hints at stock 4:3, but is too small for wider/FOV-adjusted lenses and unnecessarily conservative for some narrow projections. |
| Call `collision_objectmodel()` | It mutates gameplay interaction state, sees only the capped gameplay object set, and can perturb doors or other behaviors. |
| Reuse `generate_collision_candidates()` | Long booms can cross more than ten segments; the 500-entry global list can truncate and is shared mutable scratch. |
| Use only gameplay collision triangles | Visible `NO_COLLISION` scenery can still cover the camera; conversely water, decals, foliage, and cutouts should not all behave as hard walls. |
| Resolve inside each vehicle camera function | Dialogue/shake can move the eye afterward, transition updates can solve twice, and seven camera modes would drift apart. |
| Clamp immediately in both directions | Safe, but visually pops and chatters at triangle seams. Safety needs immediate retraction and intentionally slower, hysteretic expansion. |
| Smooth the corrected XYZ and feed it back as desired | The camera can accumulate error, lag behind authored behavior, or stay trapped after an obstruction clears. Desired and resolved state must remain separate. |
| Change `CAMERA_NEAR` or disable renderer clipping | Changes depth precision and symptoms, not the eye/world intersection. |
| Compute from output pixels | Makes 1080p and 4K cameras differ at the same aspect. Only projection geometry matters. |
| Read aspect/FOV during render | A resize can widen the render lens after the last safety solve. Projection and pose require one generation contract. |
| Resolve only gameplay cameras 0–3 | Cutscene cameras 4–7, P2 promotion, and the 3P T.T. spectator viewport can still select an unresolved bank. |
| Treat every visible triangle as hard | Foliage, water, decals, translucent effects, small signs, and fences can force nauseating close-ups or jitter. |
| Fade every obstruction | Fading a canyon wall or locked door destroys spatial trust and can expose the level void. Fade is only a soft-occluder policy. |
| Let render interpolation lerp safe endpoints | The chord between two safe poses can cross a wall. Any future high-rate camera sampling must revalidate the sampled lens or declare a discontinuity. |
| Ignore camera-driven simulation | `obj_visibility_tick()` uses each camera frustum to refresh a racer timer that gates AI steering and RNG. Changing the camera can change gameplay unless authority is explicit and tested. |

## 4. User-experience contract

### 4.1 Hard guarantees

- No hard occluder intersects the effective eye-to-near-plane lens guard.
- No non-overlap hard occluder blocks the required target-to-eye corridor after
  resolution, except a documented cinematic shot that intentionally hides its
  target. A focus point embedded in hard geometry is separately not-visible
  telemetry and an unavailable composition constraint, never a claimed clear
  corridor.
- Retraction is fast enough that a single fixed-tick move cannot expose the inside
  of a wall. Expansion is smooth, bounded, and cannot oscillate across a seam.
- The racer remains readable. If no third-person pose can fit, use deterministic
  emergency framing and racer fade rather than placing the eye in a wall.
- Open-space camera transforms remain byte-identical to the pre-fix native path.
- One aspect ratio at two resolutions produces the same camera trace.
- Each split-screen viewport resolves independently with no final-viewport
  contamination.

### 4.2 Desired feel

The priority order is:

1. physical safety;
2. target/racer visibility;
3. continuity with the prior resolved shot;
4. closeness to the authored desired shot;
5. maximum useful boom length.

The camera retracts immediately when danger increases. It returns more slowly when
space opens, with a small clearance hysteresis and blocker identity to prevent
triangle-to-triangle chatter. Obstruction correction must not add general camera
lag: DKR's existing vehicle cameras already author their own speed-sensitive feel.

### 4.3 Screen and viewport behavior

| Configuration | Required behavior |
|---|---|
| 4:3 reference | Stock composition in open space; dynamically derived guard should land near the historical 14-unit intent. |
| 16:9 / 16:10 | Hor+ composition retained; wider near plane is fully protected. |
| 21:9 / 32:9 | Use the actual horizontally capped projection. Prefer exact lens-guard narrow phase so a conservative sphere does not over-retract. |
| Portrait / narrow browser window | Use the actual narrow projection; no resolution-dependent boom changes. HUD safe area is unrelated to camera collision. |
| 2P top/bottom | Respect DKR's full-height viewport plus scissor behavior and the projection helper's existing policy. Do not infer projection from the scissor alone. |
| 3P / 4P | Resolve all gameplay viewports plus the optional 3P T.T. camera using the actual selected camera IDs. |
| HiDPI / SSAA 1x–4x | Identical world-space result at a fixed aspect/FOV. |
| Live resize / FOV change | Apply a new pose and projection atomically on the next authored camera generation; never render an unvalidated wider lens. |

## 5. Target architecture

### 5.1 Data model

Do not change the original `Camera` layout. Add native sidecar state for all eight
camera slots:

```c
typedef struct MdkrCameraObstructionState {
    Vec3f desired_eye;
    Vec3f resolved_eye;
    Vec3f last_safe_eye;
    Vec3f pivot;
    float desired_distance;
    float resolved_distance;
    float recovery_velocity;
    float guard_radius;
    uint64_t camera_generation;
    uint64_t projection_generation;
    uint32_t blocker_kind;
    uint32_t blocker_id;
    int32_t desired_segment_id;
    int32_t resolved_segment_id;
    uint8_t was_obstructed;
    uint8_t discontinuity;
    uint8_t valid;
} MdkrCameraObstructionState;
```

The exact fields may change, but the separation may not:

- **Desired pose:** final DKR-authored pose before obstruction correction.
- **Effective eye:** includes every position contribution the view matrix will use,
  including matrix-level shake.
- **Boom pivot versus readable target:** collision/recovery keeps the authored
  vehicle-local boom pivot. Sightline and emergency composition target the chassis
  center above the road-contact transform origin; using the ground origin creates
  false occlusion at road skins and crests.
- **Resolved pose:** the fixed-tick pose consumed by presentation visibility,
  rendering, audio, weather, LOD, and snapshots according to the consumer census.
- **Logical pose:** desired pose plus canonical authored projection/segment metadata
  retained only for simulation consumers that historically depended on camera
  admission. It is independent of host display settings and obstruction correction.
- **Temporal state:** native-only, reset on stage load, camera-bank change, mode
  teleport, spectate-point jump, and other discontinuities.

### 5.2 Fixed-tick ownership and ordering

The native gameplay order becomes:

```text
obj_update
  -> all ordinary objects, collidable objects, racers, cameras, weapons
hud_tick
scene_tt_camera_tick
camera_obstruction_tick       NEW: resolve every selected camera exactly once
obj_sort_tick                 canonical logical camera (authoritative list order)
obj_lod_tick                  canonical logical camera (can affect collision models)
obj_visibility_tick           canonical logical camera (AI/RNG admission)
obj_animate_tick
fog/presentation authority
render_scene                  READ-ONLY camera consumer
snapshot publish              resolved pose + projection generation
```

The new tick belongs after `scene_tt_camera_tick()` and before `obj_sort_tick()` in
both gameplay and loaded menu/cutscene paths. At that point dynamic object transforms
and all player-camera mode/dialogue/shake writes are final, while every camera-based
sort/LOD/visibility consumer is still ahead.

That position in the call graph does **not** make the following three passes
presentation-only. `obj_sort_tick()` reorders the authoritative object list used
by later collision/RNG iteration, `obj_lod_tick()` writes racer model selection
that can affect collision, and `obj_visibility_tick()` gates AI work and RNG. All
three therefore consume a separately named canonical logical 4:3 camera basis.
Resolved-camera sorting/LOD, if later required for rendering quality, must be a
private per-viewport presentation product and may not rewrite those authoritative
structures.

Audio spatial evaluation currently runs inside `obj_update()`, before the new
finalizer. Event existence, range, and pan decisions remain based on the logical
listener unless and until a presentation-only post-finalizer mixing transform is
split out and event/state/RNG invariance is proved. Moving the existing pass later
or silently substituting the resolved eye is not part of this fix.

Paused behavior requires an explicit rule: pose time does not advance, but a pending
display/FOV generation must still be resolved before the next authored image. Use
zero-time revalidation—no recovery integration—so changing video options cannot
manufacture a collision or move a stationary camera over time.

### 5.3 Effective projection handshake

Refactor the existing pure display calculations into one per-viewport query usable
before rendering:

```c
bool cam_effective_projection_for_viewport(
    int viewport, int camera_id, MdkrCameraProjection *out);
```

It must produce the exact logical viewport dimensions, effective aspect, vertical
and horizontal FOV, near/far planes, camera-bank identity, and a display-config
generation. The render path consumes the same latched record instead of recomputing
from mutable window state.

An authored image is valid only when:

```text
resolved_camera.projection_generation == render_projection.generation
```

Debug builds assert this. Production fails safe by holding the last validated
projection/pose pair for at most one authored image or synchronously revalidating;
it never renders the new wider lens with the old guard.

### 5.4 Lens guard

For near distance `n`, effective vertical FOV `v`, and aspect `a`:

```text
half_y = n * tan(v / 2)
half_x = half_y * a
broadphase_radius = sqrt(n*n + half_x*half_x + half_y*half_y) + skin
```

The final guard is the convex eye-to-near-plane pyramid expanded by a small skin.
With `forward` pointing through the image, the renderer convention is
`right x up == -forward`; treating those three vectors as a conventional
right-handed `{right, up, forward}` basis mirrors the solid. Implementation is
staged:

1. A swept sphere using `broadphase_radius` delivers a conservative correctness
   baseline.
2. The implemented ROM-free visual-triangle narrow phase tests the actual rounded
   lens pyramid/near-plane edges, avoiding enclosing-sphere false positives.
3. A fixed-basis continuous sweep must enumerate and narrow-test every candidate
   retained by the enclosing-sphere broad phase. Narrow-testing only the sphere's
   nearest hit is incorrect: that triangle can be a pyramid false positive while a
   later triangle is the real blocker.
4. Runtime integration derives its basis from the renderer's pure inverse-view
   recipe, including authored yaw/pitch/roll and the exact shake mode. Ordinary
   retraction translates the authored orientation. Alternate/elevated shots first
   build the final retargeted `Camera`, validate that oriented lens, then publish
   the same camera without recomputing rotation afterward.
5. An orientation-changing transition is either covered by a bounded conservative
   SE(3) sweep or published as an explicit presentation discontinuity after both
   endpoints are validated. A conservative sphere/full last-safe pose tuple
   handles invalid basis, projection, candidate, or convergence failures.

The projection generation identifies the lens dimensions. A separate basis
generation identifies camera bank/slot, complete camera rotation and pitch, shake
application mode, projection generation, and guard algorithm version. Eye position
is an endpoint, not a reusable basis identity. Stored fallback state is atomic:
`{Camera, effective eye, projection, exact guard, basis, shake mode}`. A partial or
newly recomputed tuple is not a known-safe pose.

Exact-lens integration is release-gated in this order; a later row cannot be used
to excuse missing evidence from an earlier row:

| ID | Engineering owner/output | Required evidence |
|---|---|---|
| LENS-01 | Geometry: exact per-triangle stationary distance/contact | ROM-free edge/face/vertex, skin, rotation, degeneracy, purity, and enclosing-sphere false-positive tests under strict warnings and ASan/UBSan |
| LENS-02 | Geometry: continuous fixed-basis sweep across a complete immutable world | A blocker intersected only at mid-corridor is found; a nearer sphere-only false positive cannot mask a later true blocker; deterministic order permutations agree |
| LENS-03 | Collision: static chunk and dynamic instance adapters retain every conservative candidate | Candidate counts reconcile to visited triangles/instances; invalid source or numeric/capacity failure is fail-closed; no allocation or global scratch |
| LENS-04 | Objects: oriented guard/eye conversion into uniform-scale object-local space | Rotated/scaled local query equals equivalent world query; mirror, nonuniform scale, shear, and non-finite transforms are rejected |
| LENS-05 | Camera/render: pure renderer-equivalent effective eye and basis | Golden yaw/pitch/roll/shake cases match the real inverse-view matrix for normal and cutscene banks without mutating render globals |
| LENS-06 | Runtime: ordinary, alternate, emergency, scripted, fallback, and post-validation use the exact final pose | No post-validation retarget; full fallback tuple/generations match; interpolation midpoint or explicit-discontinuity assertion passes |
| LENS-07 | QA/performance: rerun the 24-arm display/FOV matrix and fixed routes | Zero unsafe/hidden/degraded results and a recorded reduction in emergency/retraction rates versus `d637f2a`; performance remains inside section 7.6 budgets |
| LENS-08 | Release: breadth, sanitizer/compiler/browser, motion, and manual review | Concave/tunnel/moving-solid/resize-fault fixtures, current 47/20 sweep, GCC, resource plateau, browser timing/load budget, chatter metrics, and signed visual review all pass |

`010befb` closes LENS-01. `26fedaf` closes the ROM-free LENS-02 kernel,
the generic combined-query portion of LENS-03, and LENS-04; `f98e000` closes the
pure-helper portion of LENS-05. `0b3074d` supplies exact static-track and dynamic
published-instance adapters, `830701f` supplies the fail-closed two-phase policy,
and `fd28b0b` supplies opt-in single-corridor shadowing plus exact work telemetry.
`1b6ca47` adds a swept-SAT clear proof/conservative entry fast path while retaining
the sampled solver as a differential oracle. `842d62f` makes the fixed-tick path
use a 96-test Lipschitz interval proof when the conservative SAT entry does not
revalidate; the sampled solver is now reachable only through the explicit
development reference API. Budget exhaustion is `INVALID`, which the two-phase
dispatcher converts to the conservative promoted-sphere result and records as
degraded rather than ever publishing a false clear.
LENS-03 remains partial because the dynamic per-model acceleration/cap is not yet
release-safe. Runtime final-pose/fallback ownership and gameplay evidence remain
open. It is explicitly a false-closure defect to mark LENS-03 or LENS-05 fully
closed—or LENS-06 through LENS-08 started—because the shadow APIs compile or one
route returns zero invalid results.

Runtime dispatch uses a two-phase proof, not unconditional exact work:

1. Run the current enclosing-sphere/AABB query. `CLEAR` is also an exact-lens
   clear because the sphere contains every lens pose.
2. Only a sphere `HIT` invokes the complete exact source query. Every retained
   static triangle and dynamic instance/model candidate is narrow-tested; the
   sphere's winning triangle is not privileged.
3. An exact `CLEAR` rejects the sphere false positive and preserves the authored
   composition. An exact `HIT` supplies the real blocker. Exact `INVALID`, work
   overflow, or unavailable acceleration retains the conservative sphere result
   and raises release-gating degraded/performance telemetry; it is never clear.

This dispatch is first shadowed on the single authoritative boom corridor. It may
replace sphere decisions in anchor, stationary, recovery, and candidate-fan probes
only after counters prove those call sites fit the aggregate budget. Target
visibility remains its separate thin sightline query. Before default-on, every
place where a sphere false positive can force emergency framing must either use the
exact result or deliberately record the conservative fallback.

The development oracle is intentionally expensive: one grazing candidate can
require 128 conservative-advance evaluations, up to 1,024 interval samples, 16
contact refinements, and final publication revalidation. The fixed-tick path
instead permits at most 96 adaptive interval tests plus 16 contact refinements for
a marginal SAT interval; exhaustion fails closed. Initial source/runtime fences,
to be calibrated rather than silently weakened, are: static exact
candidates p99 <= 8 and hard cap 16 per viewport corridor; dynamic p99 <= 2
objects, hard cap 4 objects/64 BVH nodes/32 retained chunks/256 retained chunk
triangles; exact stationary evaluations p99 <= 64 and cap 128; zero interval
fallbacks in ordinary gameplay; exact corridor
p99 <= 0.25 ms in 1P and <= 1 ms aggregate in 4P while the inclusive camera budget
in section 7.6 remains authoritative. Dynamic models exceeding the triangle cap
remain sphere-conservative until they receive a per-model acceleration structure.

The first ROM-backed dark launch makes this a measured stop condition, not a
theoretical concern. In a 5,200-frame authored-4:3 Ancient Lake run,
`MDKR_CAMERA_EXACT_SHADOW=1` evaluated 187 promoted-sphere hits: 166 (88.8%) were
exact clears and 21 were exact hits, with zero exact invalid/degraded results.
Static retention was already bounded (636 candidates total, maximum 7 per sweep),
but 22 interval fallbacks consumed 19,792 samples and drove the worst sweep to
2,025 stationary tests. Dynamic work retained at most one instance but swept a
166-triangle model, above the provisional 128-triangle fence. In this Debug
diagnostic build, exact-static mean/p50 were 1.591/0.590 ms and its long-tail bin
reached 17.696 ms; exact-dynamic had two 15.346-ms tails. These timings do not
adjudicate the optimized release budget, but the work counts are release-fatal.
The paired shadow-off run had zero normalized published-camera differences across
all 5,187 selected detail rows.

The `1b6ca47` follow-up preserves the exact 2,038/166/21 decision
census and zero degraded results while changing the static work to 636 swept-SAT
tests, 44 stationary tests, zero advance/refinement/fallback/sample work, and a
maximum of four stationary tests per sweep. Debug exact-static mean/p50/p99/max
fell to 0.022/0.020/0.120/0.154 ms; exact-dynamic max fell to 0.066 ms. A seeded
256-case differential corpus agrees with the retained reference on status,
blocker, and hit fraction; two conservative SAT entry candidates require the
reference path to prove clear, but no case enters interval sampling and every
case stays within 130 stationary tests.

`842d62f` then replaces the fixed-tick revalidation-miss route with the bounded
Lipschitz interval proof. The deterministic tangent regression now takes that
bounded path and remains a hit while the reference API independently takes its
sampled path. The 256-case differential corpus has zero production fallbacks,
zero bounded-budget exhaustions, and at most 114 stationary tests; strict Clang
and focused ASan/UBSan suites pass. Repeating the optimized 5,200-frame shadow
preserves the decision census and reports `bounded=0`, `exhausted=0`, zero
fallbacks/samples, maximum four static stationary tests, and exact-static
p99/max of 0.120/0.118 ms (histogram rounding makes the p99 bin exceed the
measured maximum). PERF-01 implementation is complete, but its release gate
remains open for arbitrary-orientation/scale fuzz breadth, injected exhaustion
boundaries, GCC/wasm evidence, and the required route matrix.

`b433b2b` adds profiled object-local exact sweeps so dynamic telemetry separates
model faces scanned from faces surviving the core swept-AABB and reaching exact
narrow phase. On the same route, the retained 166-face model contributes 1,328
faces across eight instance sweeps, but 1,294 are AABB-rejected; only 34 are
narrowed in total, with maximum six narrowed triangles and three stationary tests
per dynamic sweep. Exact-dynamic mean/max remain 0.003/0.070 ms. This removes the
false inference that 166 faces reached exact SAT, but does not waive PERF-02: the
linear per-model scan is still structurally unbounded on other assets/routes and
must receive immutable chunk/BVH acceleration plus a hard fail-closed work fence.

`d8c8bb7` supplies that acceleration for both phases. Hard triangles remain in
stable source order; an auxiliary deterministic balanced BVH indexes contiguous
eight-triangle chunks. Cache publication validates topology, leaf coverage, and
parent containment. Queries revalidate model generation/cache identity and
node/chunk integrity, then apply aggregate caps of four retained instances, 64
node visits, 16 chunks, 128 retained triangles, and 128 exact stationary tests.
Any stale generation, malformed bounds/topology, or exhausted budget returns
`INVALID`; it cannot become an ordinary cull. On the same 5,200-frame route,
enclosing-sphere work peaks at 37 nodes, eight chunks, and 62 triangles; exact
dynamic work peaks at 29 nodes, five chunks, 40 triangles, six narrowed faces,
and three stationary tests. Both report zero invalid sweeps. Exact decisions
remain 166 clears and 21 hits, and normalized shadow-on/off detail hashes are
identical. This closes the structural linear-scan defect, not PERF-02 release
evidence. `da93930` adds executable ROM-free sphere/exact BVH equivalence across
identity and 64 rotated/scaled inputs plus malformed node/chunk, per-model cap,
stale-generation, and same-address generation-reuse faults. That gate found and
fixed noncanonical indexed-clear hit bytes. Actual load/free/address reuse,
aggregate multi-instance cap injection, whole-ROM bounds, moving-solid breadth,
and load/memory budgets remain mandatory.

The chunk/triangle halves of that fence were then measured rather than assumed,
and raised once. `55ea056` had added the exact path's conservative recovery, and
it exposed a real saturation: on adventure-postrace ticks 3465-3471 a
transitioning 166-triangle door demanded up to 17 retained chunks and 134
retained triangles against the 16/128 fences, so the exact kernel exhausted,
the recovery published the door's world AABB, and that AABB contained the
resolved eye on ticks 3466 and 3467 (`penetrated=1`). The fences were lifted to
1024/4096/32768 in a throwaway build and every camera route was re-run to record
each query's true demand. The 17,000-tick adventure-postrace route peaks at 39
node visits, 17 chunks, 134 triangles, and nine stationary tests; the
5,200-frame, 9,000-frame, 7,000-frame, 3,400-frame, and 3,600-frame routes never
exceed 33 nodes, seven chunks, 54 triangles, and eight stationary tests. Only
that one door saturates anything.

`MDKR_CAMERA_OBJECT_OCCLUSION_MAX_RETAINED_CHUNKS` therefore moves 16 -> 32 and
`MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_TRIANGLES` 128 -> 256, staying coupled at
exactly `MAX_RETAINED_CHUNKS * CHUNK_TRIANGLES` so a query can never be
triangle-fenced before it is chunk-fenced. The saturating door compiles to
ceil(166/8) = 21 chunks and 41 BVH nodes, so the new fences admit its complete
traversal with 52%/54% headroom and clear the measured 17/134 with 88%/91%.
The 64-node and 128-stationary-test fences are unchanged and remain correct:
64 already admits that model's entire 41-node tree, and no route exceeded nine
stationary tests. Neither raised constant sizes any storage -- the only
fence-dimensioned array is the `MAX_QUERY_NODES` traversal stack -- so the raise
costs zero bytes and only doubles the worst-case exact triangle work per query,
which no measured route approaches.

Therefore the 1,024-sample interval fallback remains a correctness oracle and test
reference only. The fixed-tick implementation uses swept separating-axis
intervals plus a bounded 1-Lipschitz clearance proof and returns `INVALID` when it
cannot prove clear inside the fence; composition retains the promoted-sphere
`HIT`. The per-model index is implemented; the next performance dependency is
proving its fixed fences and cache lifecycle across the dark-launch breadth
matrix before calibrating or changing any budget.

The racer and its attached vehicle parts are excluded. The pivot must be the
camera behavior's authored focus, not the raw racer origin. A pivot touching a wall
is legal; the query must support initial overlap and start the boom from the nearest
valid lens origin rather than reporting an unusable zero-length hit forever.

### 5.5 Camera occlusion world

Do not couple the target design to the gameplay candidate arrays.

#### Static track data

At level load, build a native, read-only camera occlusion cache with stable IDs:

- hard gameplay collision facets;
- opaque visual triangles that can occlude the lens even when marked
  `RENDER_NO_COLLISION`;
- batch/material classification and source segment/batch/triangle provenance;
- deterministic per-segment BVHs or another no-truncation spatial index.

Build synchronously and in stable asset order. Record triangle count, node count,
bytes, build time, and rejected/degenerate triangles. No ROM-derived cache is
shipped; it is rebuilt from the user's loaded assets.

#### Dynamic objects

- Build one local-space occlusion acceleration structure per object model.
- Maintain a stable per-tick world-AABB list for active hard/soft occluders.
- Transform the lens query into object-local space; do not mutate the object or its
  interaction record.
- Include doors and moving solids independently of `gCollisionObjectCount`.
- Exclude the followed racer, vehicle attachments, particles, pickups, and effects.
- Key temporal/fade state by object identity plus generation so freed/reused object
  addresses cannot inherit a blocker history.

#### Occluder classes

| Class | Examples | Response |
|---|---|---|
| Hard | track shell, cliffs, buildings, large opaque props, closed doors | Lens must never cross; retract or choose alternate shot. Never fade as the sole response. |
| Soft | foliage, small signs/poles, selected cutout fences | Prefer a clear alternate; otherwise per-viewport presentation-only fade. Must not alter gameplay opacity. |
| Nonblocking | water surface, decals, sky, particles, translucent effects | Ignore for camera position. |

Unknown opaque track geometry defaults to hard and appears in a classification
census. Unknown translucent/cutout geometry is reported and requires an explicit
policy before release. Heuristics alone are not a definition of done.

### 5.6 Pure query API

The geometry kernel is dependency-light and ROM-free testable:

```c
typedef struct MdkrCameraSweepInput {
    MdkrCameraLensGuard guard;
    MdkrCameraVec3 start_eye;
    MdkrCameraVec3 desired_eye;
    uint32_t mask;
    uint32_t ignored_object_generation;
} MdkrCameraSweepInput;

typedef struct MdkrCameraSweepHit {
    float fraction;
    float penetration_depth;
    MdkrCameraVec3 point;
    MdkrCameraVec3 normal;
    uint32_t kind;
    uint32_t stable_id;
    uint8_t started_overlapping;
} MdkrCameraSweepHit;

MdkrCameraSweepStatus mdkr_camera_sweep(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *hit);
```

The implemented baseline also exposes `mdkr_camera_sweep_object_local()` and
`MdkrCameraObstructionCombinedQuery`. Commits `010befb` and `26fedaf` add
`MdkrCameraRoundedLensGuard`, exact static triangle testing, continuous fixed-basis
all-triangle sweeps, object-local oriented conversion/sweep, and allocation-free
multi-source composition. `f98e000` adds the pure renderer-equivalent effective-eye
and basis helper. Commits `0b3074d`, `830701f`, and `fd28b0b` add track/dynamic
exact source adapters, fail-closed two-phase dispatch, per-call work telemetry,
and an opt-in non-publishing comparison shadow. Modern now publishes only from
the exact two-phase decision: the projection-derived enclosing sphere retains
conservative candidates, and the renderer-derived rounded lens decides the final
pose. Bounded continuous time-of-impact work, dynamic per-model BVHs, aggregate
work fences, and sealed final-pose publication are implemented. Cross-toolchain,
browser, content-breadth, motion-quality, and adversarial fault evidence remain
required before default-on.

Requirements:

- earliest time of impact with deterministic tie-breaking by stable ID;
- faces, edges, vertices, thin and two-sided visual shells;
- initial overlap, depenetration direction, and last-safe fallback;
- no allocations, global scratch, truncation, RNG, or writes outside `hit`;
- finite-input validation and a fail-safe result for NaN/degenerate geometry;
- stable behavior across Clang/GCC and native/wasm32 within declared float
  tolerances; authoritative output is quantized or tie-broken where necessary.

### 5.7 Resolver and shot selection

The ordinary path is a spring arm:

1. Calculate desired pivot, eye, look target, projection, and lens guard.
2. Sweep toward the desired eye.
3. If blocked, retract to hit fraction minus skin immediately.
4. Recompute look-at orientation and validate the final lens.
5. Recompute `cameraSegmentID` from resolved XYZ, including vertical position.
6. Publish blocker/correction metadata and update temporal state.

If the safe boom is below the mode's readability minimum, test a small,
deterministic candidate fan: elevated, left shoulder, right shoulder, and tighter
center. Reject unsafe candidates, then score remaining candidates by the UX priority
order in section 4. Candidate ordering, score ties, and maximum query count are
fixed so multiplayer and cross-platform behavior is deterministic.

If no third-person candidate fits:

- move to the last safe eye or a validated tight pivot pose;
- suppress/fade the local racer model as needed to avoid near-plane self-clipping;
- mark a discontinuity if the emergency move exceeds the teleport threshold;
- never accept a hard-occluder penetration to preserve boom length.

#### 5.7.1 Soft-occluder enrollment and fade architecture

Soft fading is not a flag heuristic. The current caches correctly keep
`RENDER_CUTOUT` and `RENDER_VTX_ALPHA` hard and report them as unknown policy:
cutout batches can be opaque fences or walls, and vertex-alpha batches can have
fully opaque structural vertices. `TextureHeader.flags` is runtime-mutated,
`TextureInfo.surfaceType` is gameplay metadata, and texture pointers are neither
stable nor durable release-policy identities. Treating any of those signals as a
global soft rule is a stop-ship defect.

CAM-08 is therefore operationalized as this ordered program:

1. Extend the ROM model corpus tool to emit a byte-stable sorted census with
   `{ROM revision, kind, model asset ID, segment, batch, source face range,
   texture asset ID, texture format/mode/flags, vertex-alpha histogram,
   triangle count, current class}`. Include CI palette alpha and every animated
   texture frame. The existing corpus baseline is 390 object models/6,562 batches/
   52,428 triangles and 55 level models/10,381 batches/90,617 triangles.
2. Check in a versioned, human-reviewed policy keyed by
   `{kind, model_asset_id, segment_or_none, batch_index}` with one of `HARD`,
   `SOFT_FADE`, or `IGNORE`, plus source batch/texture fingerprints. An asset
   change or missing provenance invalidates enrollment and fails closed to Hard.
   Cache-local sequential triangle IDs and object/model pointers are forbidden
   policy keys.
3. Preserve that immutable provenance before texture IDs become pointers and
   before object models lose their loader asset ID. Hard and soft query sources
   remain separate: hard hits determine legal eye position; a final target-to-eye
   soft query only proposes reviewed fade references.
4. Publish a bounded, generation-keyed fade set per viewport. Apply opacity at
   **batch** scope in `render_level_segment`/object mesh rendering, restore render
   state after every batch, and never write `Object.opacity`, texture flags,
   model data, collision state, or a global material. Whole-object fading is
   permitted only for a fingerprinted homogeneous all-soft model.
5. Add hysteresis, deterministic tie-breaking, capacity telemetry, and a fail-
   closed overflow path. A mixed hard/soft model fixture must prove its hard batch
   stays at opacity 255 while the enrolled batch fades differently in two
   simultaneous viewports.

Exit evidence includes the supported-ROM census/hash, policy review, malformed
corpus controls, static/dynamic/mixed-material renderer fixtures, 1P–4P pixel and
state-hash isolation, animated texture/shadow/depth checks, no allocations, and
repeat-run-identical fade envelopes. Until all of that exists, unknown cutout and
vertex-alpha content remains hard; a broad fade is not an acceptable shortcut.

### 5.8 Mode policy

| Camera family | Policy |
|---|---|
| Car / hovercraft / plane | Full lens sweep, immediate retract, damped expansion, alternate shot below minimum boom. |
| Loop | Full sweep in the loop's authored local orientation; preserve roll and avoid horizon correction that fights the mode. |
| Finish challenge orbit | Full sweep and deterministic shoulder/elevation alternatives; retain orbit phase. |
| Fixed door camera | Eye depenetration plus target-visibility validation; never move through the door it is presenting. |
| Finish race / spectate point | Validate candidate spectate points and choose the best safe authored point before applying local correction. |
| Scripted cutscene cameras 4–7 | Emergency eye safety always; line-of-sight correction only where the shot contract requires the subject visible. Record authored occlusions for content review. |
| 3P T.T. camera | Resolve the actual camera ID selected for the fourth viewport and maintain independent state. |
| Ordinary menu/model preview | Off unless backed by a loaded world and explicitly enrolled. Do not reinterpret temporary camera-shaped matrices. |

### 5.9 Authority consumer census

Before changing the default, enumerate every read of `gCameras`, `gCameraObject`,
`gSceneActiveCamera`, camera position, and `cameraSegmentID`. Classify each reader:

- pose author;
- presentation/culling/LOD/weather/audio consumer;
- simulation consumer;
- snapshot/hash/diagnostic consumer;
- temporary menu/effect camera.

The default decision is:

- resolved camera for render matrices, scene segment seed, presentation culling,
  weather/lens effects, and snapshots;
- no camera correction feedback into racer physics or desired-camera smoothing;
- `obj_sort_tick()`, `obj_lod_tick()`, and `obj_visibility_tick()` use a separately
  named **logical authority basis**:
  desired pre-correction pose, desired segment, and canonical authored 4:3
  projection. It may not read host aspect, user FOV, resolved eye, or soft-fade
  state. This preserves authoritative object order, collision-model choice, and
  the historical activity heuristic without allowing screen choice or camera
  retraction to change collision, AI steering, or RNG;
- existing audio event/range/pan evaluation uses the logical listener. A later
  resolved listener may affect presentation-only mixing only after that concern is
  split from sound-event creation and measured invariant;
- any other simulation consumer requires an explicit decision and an A/B
  state/event/RNG measurement. If later evidence supports making all racers
  unconditionally active on native hardware, treat that as a separate gameplay
  change with an independent oracle—not part of this defect fix.

`scene_build_last_viewport_basis()` and `scene_visibility_prepare_viewport()`
currently serve both authority-sensitive and presentation callers. Split or
parameterize that authority so a caller cannot
accidentally substitute one basis for the other. Name the APIs by purpose and add a
source test that rejects use of resolved/display projection in any logical pass.

No reader remains “whichever camera happened to be active.” The census becomes a
source test so new uncategorized readers fail CI.

### 5.10 Shake and future presentation interpolation

Create one helper that returns the effective eye used by the view matrix. The
resolver validates that eye, including matrix-level shake. Prefer solving the base
pose and limiting the shake sample to the remaining clearance so an explosion can
never punch through a wall.

Modern publishes validated resolved-camera sidecars and presentation snapshots.
It is not enough to lerp two safe endpoints. The current transition contract
therefore either:

- validates the full previous-to-current tuple against immutable static/dynamic
  geometry (exact for an unchanged basis, conservative for a rotated basis); or
- marks projection/shake/discontinuous/unsafe transitions as cuts so the renderer
  uses the validated current endpoint.

The snapshot carries resolved camera ID, projection generation and obstruction
discontinuity. An invalid/degraded tick exposes no resolved snapshot and retires
its predecessor so recovery cannot interpolate from unvalidated bytes.

## 6. Operational work breakdown

Work items are ordered gates. A later item may be developed behind a flag, but it
cannot become the default before all dependencies pass.

| ID | Owner role | Deliverable | Dependencies | Exit gate |
|---|---|---|---|---|
| CAM-00 | gameplay/QA | Commit deterministic wall, corner, tunnel, door, moving-object, and mode-transition witnesses; add desired/effective camera trace | none | Legacy arm reproduces each targeted failure; open-space control remains clear |
| CAM-01 | platform/camera | Per-viewport effective projection record and generation handshake shared by resolver/render/snapshot | CAM-00 | Unit matrix covers 4:3, 16:9, 21:9, 32:9, portrait, 2P/3P/4P, FOV cap, live resize; render-generation mismatch is impossible or fails closed |
| CAM-02 | geometry | Pure lens/sphere sweep kernel with deterministic tie-break, overlap recovery, and fuzz/unit suite | CAM-01 | ROM-free tests and sanitizers pass; no allocation/truncation; injected legacy ray control fails near-plane cases |
| CAM-03 | collision/content | Static track occlusion cache, provenance census, hard/soft/nonblocking policy | CAM-02 | All supported-ROM levels build without unknown hard errors; memory/build/query telemetry inside budgets |
| CAM-04 | gameplay/camera | Eight-slot sidecar, desired/resolved segment split, logical AI-visibility basis, and single fixed-tick finalizer; terrain correction for ordinary follow cameras | CAM-01–03 | Ancient Lake wall invariant passes; open-space camera bytes match; resolved segment matches resolved pose; logical AI admission/state is display/correction invariant; one solve per camera/tick |
| CAM-05 | objects | Pure object-model occlusion structures and dynamic world list | CAM-03–04 | Locked doors and moving solids block camera with zero interaction/gameplay state writes; 20-object cap is irrelevant |
| CAM-06 | camera/design | Temporal recovery, blocker hysteresis, minimum-boom emergency, deterministic alternate shot fan | CAM-04–05 | No penetration, seam chatter, shoulder flapping, or unbounded recovery in stress routes; motion thresholds pass |
| CAM-07 | camera/content | Loop, finish, fixed, spectate, cutscene-bank, P2 promotion, and 3P T.T. policies | CAM-06 | Mode matrix and camera-bank coverage pass; authored exceptions are enumerated, not silently ignored |
| CAM-08 | rendering/design | Per-viewport soft-occluder fade and racer emergency fade, generation-keyed and presentation-only | CAM-05–07 | No cross-viewport opacity contamination or state-hash change; hard occluders never rely on fade |
| CAM-09 | QA/performance | Full breadth, performance, soak, browser, sanitizer, release docs, default-on rollout | CAM-00–08 | Definition of done in section 10 is satisfied on all required configurations |

### 6.1 Implementation and evidence ledger

“Implemented” means code exists; “release-evidenced” additionally requires a
recorded build/ROM/backend/matrix result. A partial row may not be used to justify
default-on rollout.

| Gate | Status | Implemented/test evidence | Missing release evidence / next blocker |
|---|---|---|---|
| CAM-00 | Strong partial | Observe/detail trace; Ancient Lake same-binary controls; Modern geometric/target-visibility post-validation; hard-door attribution; moving/open-door publication; pause/restart/post-race lifecycle witnesses; ROM-free pinned concave-corner, tunnel, thin-pillar, thick-target, and moving-solid camera-chord controls | Commit a rotating-door-across-lens fixture and a real resolver correction witness for the moving-solid chord |
| CAM-01 | Strong partial | Exact per-viewport projection record, explicit T.T./cutscene context, render-generation check, 24-arm shape/FOV matrix, equal-aspect identity, fail-closed restored-pair fault gate | Inject projection failure concurrently with live resize/display-generation change |
| CAM-02 | Exact query/source shadow foundation; bounded cast implemented, release breadth open | Sphere path plus allocation-free rounded-pyramid static and continuous world/object-local/combined-source APIs; exact track/dynamic adapters; fail-closed two-phase policy; opt-in non-publishing corridor shadow; swept-SAT plus bounded Lipschitz interval proof with sampled reference isolated to the oracle API; SAT slicing plane, sphere-false-positive, mid-corridor, tangent, edge, outward-AABB, high-coordinate diagonal, seeded differential, purity, invalid, tie, strict, sanitizer, and exact work counters | Complete arbitrary-orientation/scale differential fuzzing and injected work-cap boundaries, then current-revision GCC and wasm32 sanitizer-equivalent evidence |
| CAM-03 | Strong partial | Static visual-triangle cache, stable provenance, full US-v80 ROM model/track census, triangle-AABB broadphase, exact source adapter, cache bytes/build time, candidate/work counters, bounded exact TOI, and query percentiles; first exact shadow retains at most 7 track triangles | Approved fingerprinted hard/soft/ignore policy and level-load percentage budget on reference hardware |
| CAM-04 | Strong partial | Eight-slot sidecar; author intent; resolved segment; scoped presentation reads; authority source guards; immutable renderer-derived final-pose tuple; every ordinary, alternate, emergency, scripted, and fallback publication is exact endpoint-validated | Final open-space state/event/input/RNG/audio A/B corpus and browser/backend parity rerun |
| CAM-05 | Strong partial; bounded temporal index implemented, release evidence open | Object-model cache; fixed-capacity, generation-keyed publication; stable-order eight-triangle chunks plus deterministic balanced BVH; build-time topology/coverage/containment validation and query-time integrity checks; aggregate sphere/exact instance/node/chunk/triangle/stationary fences; healthy sphere work exhaustion falls back to the published world AABB while corruption remains invalid; moving doors publish a conservative renderer-path temporal envelope; moving non-door solids and intersecting camera chords cut presentation interpolation; production-backed dense temporal oracle and invalid/recovery publication state-machine tests; ROM-free BVH equivalence/fault tests and runtime source/fallback/invalid counters | Inject aggregate multi-instance exhaustion; add rotating-door exact-lens coverage; extend actual address-reuse and whole-ROM work/memory/load soaks |
| CAM-06 | Strong partial | Immediate retract, bounded recovery, scripted endpoint/chord safety, deterministic shoulder/elevation fan, sticky prior-candidate/release-band hysteresis; previous/current exact tuple validation; fixed-basis exact transition sweep; rotated-basis conservative sweep; projection/shake tuple cuts; production-backed thick-slab `target_embedded` classification that remains not-visible telemetry | Quantified jerk/retract/recovery/blocker/candidate metrics, presentation-rate routes, and signed worst-1% motion review |
| CAM-07 | Strong partial | Follow/loop/fixed/finish/T.T./scripted families plus P2/cutscene/3P-T.T. slot mapping; current-source 47-row, 4P, WebGPU snapshot, and independent 3P+T.T. camera-3 route coverage | Approved cinematic exceptions and remaining pinned mode-transition review |
| CAM-08 | Partial | Per-viewport presentation-only emergency racer opacity; hard surfaces never fade | Soft-occluder enrollment/fade policy and state/cross-viewport pixel proof |
| CAM-09 | In progress | Release registration; same-binary oracle; 24-arm shape/FOV gate; lifecycle/3P/emergency gates; isolated native 4P p99/memory/authority gate; current Clang ASan/UBSan native target and ROM route; 84-test registered suite including new fault/oracle fixtures | Current-candidate 47-row/20-track, WebGPU/browser/wasm and resource-plateau/GCC breadth, soft policy/pixels, rotating-door/runtime moving-solid fixtures, motion/manual review, and a separately reviewed default flip |

Current recorded evidence (2026-08-05, macOS host, US-v80 hash from the banner):

- The current-candidate command/results ledger and remaining release work are
  retained in
  [`camera-runtime-modern-2026-08-05.md`](../evidence/camera-runtime-modern-2026-08-05.md).

- 84/84 registered CTests pass in `build-cam05-prod`. The full native target and
  exact geometry/query/transform/resolver/lens-pose tests also pass strict
  `-Wall -Wextra -Werror -Wpedantic`; focused exact geometry/query/transform
  tests and the full native target build under ASan/UBSan.
- The historical opt-in exact shadow ran only the single authoritative boom
  corridor and never published its decision. Paired shadow-on/off authored-4:3 runs produce
  zero normalized resolved-camera differences across 5,187 selected rows. Of
  187 exact invocations, 166 reject sphere false positives and 21 confirm hits;
  no exact query is invalid or degraded. The same run is a **performance fail**:
  22 sampled fallbacks, 19,792 samples, maximum 2,025 stationary tests, and a
  166-triangle dynamic model keep exact runtime replacement blocked. The paired
  command, host/build identity, census, and decision are retained in
  [`camera-exact-shadow-2026-08-05.md`](../evidence/camera-exact-shadow-2026-08-05.md).
- The `842d62f` bounded-cast follow-up repeats the identical exact decision census
  with zero bounded work, exhaustion, sampled fallback, or samples on-route and
  maximum four static stationary tests. Exact-static optimized p99 is 0.120 ms
  and measured max 0.118 ms. The tangent unit takes the bounded production path,
  while the 256-case differential corpus reports zero exhaustion and maximum 114
  stationary tests. This closes the reachable sampled-fallback implementation
  defect, but not PERF-01's cross-compiler/fuzz/fault-injection release evidence.
- `b433b2b` profiles the dynamic object-local kernel rather than treating all
  retained model faces as narrow phase. The same eight instance sweeps scan 1,328
  model faces, AABB-reject 1,294, narrow 34, and perform six stationary tests in
  total; maxima are six narrowed faces and three stationary tests per sweep.
  Exact-dynamic max is 0.070 ms. PERF-02 remains mandatory because scanning every
  face AABB is not a hard work bound, even when this route is inexpensive.
- `d8c8bb7` replaces both dynamic whole-model sphere and exact scans with an
  immutable per-model BVH and aggregate fail-closed budgets. The repeated route
  peaks at 37 nodes/eight chunks/62 triangles for sphere work and 29 nodes/five
  chunks/40 triangles for exact work, with zero invalid sweeps. Exact decisions
  remain 166 clear/21 hit; shadow-off/on normalized detail SHA-256 is identically
  `0930efd13694508cfedb5d18ea7b0edca497c2be4dc1e8acd136bacc5bc9def8`.
  All native CTests and focused ASan/UBSan pass. PERF-02 remains release-open
  for executable BVH fault/equivalence tests and the required lifecycle/breadth
  matrix.
- `da93930` registers `camera_object_bvh`, a white-box test of the production
  representation rather than a parallel test index. Full-world and indexed
  sphere/exact results are byte-identical for identity and 64 rotated/scaled
  cases. NaN bounds, invalid leaf values, duplicate children, corrupt chunk
  ranges/coverage, shrunken parents, node/chunk/triangle/stationary cap exhaustion,
  stale generations, and same-address generation reuse all fail closed. The gate
  found and fixed the indexed clear-output canonicalization defect and passes
  focused ASan/UBSan.
- The 5,200-frame Ancient Lake same-binary witness reports 217 penetrated legacy
  poses and 276 center-ray lens violations. Modern applies 95 corrections and
  publishes zero penetrated, invalid, degraded, or target-hidden resolved poses,
  zero duplicate solves, zero projection mismatches, 2,273 exact invocations,
  2,254 sphere-hit/exact-clear overrides, and 4,572 validated presentation
  transitions (4,393 clear and 636 conservatively cut; counters can overlap).
- The 24-arm 3,600-frame matrix crosses 320x240, 1280x960, 1920x1080,
  2560x1080, 5120x1440, and 1080x1920 with authored, minimum 20-degree,
  maximum 140-degree capped, and explicit uncapped 140-degree FOV. All arms
  publish zero penetrated, invalid, degraded, or target-hidden Modern poses and
  exercise exact queries in every arm. The equal-aspect 4:3 traces are identical
  inside every FOV arm. The 20-degree arms remain correction-free, which is the
  correct open-space outcome for their smaller lens footprint rather than a
  missing-coverage failure.
  This is a **safety pass, not a motion-quality pass**: elevated emergency framing
  occurs in 26/3,587 authored 4:3 frames, 88–118/3,587 authored 16:9–32:9
  frames, 108–234/3,587 capped-140 arms, and 333–651/3,587 uncapped-140
  landscape arms (zero in every 20-degree arm). Those rates keep CAM-06 and the
  exact lens-guard ultrawide work release-blocking.
- A 20-track/3,600-frame Modern sweep, 2P 16:9 race, non-sequential camera snapshot
  coverage, strict warning build, and focused/full sanitizer routes passed during
  development.
- The 9,000-frame Timber's Island gate produces 8,992 validated rows, 176
  corrected rows, zero penetrated/degraded/invalid rows, up to 12 published hard
  instances, 16 observed dynamic source hits, and four corrections whose source
  query hit dynamic geometry. Missing cache, identity, transform, capacity,
  unclassified, and sphere/exact invalid-query counters are release-fatal;
  explicit non-solids and healthy bounded sphere fallbacks are reported
  separately.
- The injected projection-latch gate fails tick 800 after a safe prior image and
  proves that restoration occurs only for the same camera/viewport/layout/FOV/
  display generation and only after a fresh static+dynamic stationary sweep. The
  restored render pair reports zero penetration, invalidity, degradation, or
  projection mismatch.
- The current strict `-Wall -Wextra -Werror -Wpedantic` ASan/UBSan executable
  passes the 5,200-frame same-binary controls, a 3,600-frame 32:9 uncapped-140
  Modern route, and the 16,989-summary Adventure lifecycle/generation soak.
  Modern reports zero penetrated, degraded, invalid, or target-hidden poses; the
  positive controls retain 217 legacy and 11 center-ray lens violations.
- A current-source 9,600-frame 4P GL route produced 21,964 viewport detail rows:
  four fresh intents per tick, zero stale intents, zero penetration/invalid/
  degraded results, and zero dynamic omission or projection-authority counters.
- The independent 3P+T.T. route produces 4,199 T.T. camera-3 summaries/details,
  331 corrected poses, four fresh selected camera intents per tick, and zero
  unsafe, stale, degraded, omission, or authority rows.
- Pause-quit, pause-restart, and Adventure post-race routes produce 49 reset
  witnesses and 26,659 safe summaries. Every level load has an intervening reset
  and restarts at camera tick 1; the Adventure route collects balloon 10, opens a
  hub door, and observes its moving dynamic publication. The route also records
  a maximum 66-tick interval where the racer focus remains embedded in a hard
  moving door after bounded local skin exclusion; it is reported
  `target_visible=0,target_embedded=1` and separately classified rather than
  falsely reported visible or topologically enclosed.
- The forced-no-alternate readability route produces six presentation-only racer
  fade rows in the 153..237 opacity envelope while retaining zero penetration,
  invalidity, degradation, or hidden targets. Elevated emergency endpoints are
  separately counted and require a clear full guard plus target sightline.
- A pre-`d637f2a` WebGPU/native Release artifact passes all 47 legal
  vehicle/track combinations: 47/47 rows, 20 swept tracks, 65 decoded level
  headers, zero failed rows, and no surfaced fatal, crash, or invalid markers.
  The canonical harness suppresses successful child telemetry, so geometric
  per-query claims continue to come from the dedicated runtime gates. This is
  retained breadth evidence, not a substitute for the current-candidate rerun.
- Current `MDKR_STATE_HASH=3` streams are byte-identical for all 3,600 rows
  between Observe and Modern at 1280x720, covering the authoritative camera,
  racer, object, RNG, interaction, progression, and settings field set.
- Earlier WebGPU snapshot coverage records camera 1 in 2P (728 captures),
  camera 3 in the 3P T.T. quadrant (399), and cutscene-bank camera 4 (2,127), with
  10,333/3,731/23,675 interpolated poses and exact endpoint controls.
- The `d637f2a` wasm32/WebGPU module links under Emscripten 4.0.10 and passes
  the ROM-absence artifact guard. The release-registered real Chromium run with
  Modern explicitly selected completed
  3,600 host opportunities, 3,580 canvas updates, live fullscreen and DPR/aspect
  changes, 981 in-race rows, p95/p99/max frame intervals of
  35.42/41.23/158.40 ms, exact
  persistence/reload, and clean WebGPU teardown. The release runner now supplies
  `--camera-obstruction modern` and rejects any penetration, invalid result,
  source degradation, projection mismatch, duplicate solve, missing hard dynamic
  cache/identity, unclassified object, invalid transform, or capacity failure.
- The isolated 12,500-frame Observe/Modern 4P gate records 5,491 active
  four-viewport ticks and byte-identical state streams. Modern finalizer timing is
  p50 60 us, p95 470 us, p99 690 us, and max 1.240 ms against the 833.333-us p99
  budget; Observe p99 is 70 us. Static cache maximum is 151,328 bytes and its
  measured build is 163.708 us. Query p99/fan, long-tail, histogram-overflow, and
  exact dynamic-sidecar-byte assertions all pass. Browser-specific timing and the
  level-load percentage budget remain open rather than inheriting this native pass.

### 6.2 Red-team closeout and remaining stop conditions

The 2026-08-05 adversarial pass found additional plausible false-closure paths. They
are now encoded as mechanisms or gates, not prose-only cautions:

| Finding | Operational response |
|---|---|
| Browser test only validated Modern when manually asked | `tools/run_checks.py` now registers the browser runtime with `--camera-obstruction modern`; browser summaries must show a correction and zero unsafe/degraded results. |
| Projection failure could restore a stale camera/lens | Restoration requires the immediately previous selection, no discontinuity, identical camera/viewport/layout/authored-FOV/gameplay/display context, a complete current query source, and a fresh stationary sweep. The injected fault gate exercises the branch. |
| An early anchor failure could reuse pre-teleport safe state | Runtime invalidates last-safe, previous-pivot, alternate, and obstruction recovery state before any resolver early return on an intent discontinuity. |
| A hard dynamic object could be silently omitted | Missing cache/identity, invalid transform, or capacity failure makes the dynamic source `INVALID`. Modern refuses publication for the tick; renderer and snapshot accessors consume authored fallback, and release telemetry remains degraded/invalid. Static-only publication is forbidden. |
| A dynamic `INVALID` could be converted to `CLEAR` by an adapter | The adapter preserves `INVALID`, marks the source degraded, and prevents both sphere-clear sealing and exact/final-pose publication. Source guards pin this behavior. |
| A failed camera publication could leave a stale validated interpolation source | Any Modern invalid/non-clear/degraded tick retires the complete prior validated tuple, publishes no resolved sidecar, and marks presentation discontinuous. The first recovered tuple cannot interpolate from the failed image. |
| A failed dynamic census could interpolate hard objects across an unknown transform | A failed publication makes all eligible hard objects discontinuous on that tick. The first complete recovered census also forces a current-pose cut for every hard instance; interpolation resumes only after two consecutive valid publications. |
| A dynamic ID could appear in telemetry without driving correction | The door gate now requires every high-bit dynamic blocker row to be corrected and reports blocked/corrected/uncorrected counts. |
| Zero lens penetration could still hide the racer | Post-validation and release summaries gate target visibility; the racer target is a chassis focus above the road-contact origin. A focus still overlapping after bounded local skin exclusion is `target_visible=0,target_embedded=1`; it is not claimed visible, and it does not masquerade as a solvable remote occluder. |
| The enclosing sphere can make extreme wide/high-FOV lenses appear to have no valid boom | A bounded deterministic elevated/azimuthal emergency fan publishes only clear, target-visible endpoints and records every use. This is a safety fallback, not acceptance of its composition; exact near-plane/pyramid narrow phase remains the ultrawide quality fix. |
| Mean process CPU could conceal 4P query tails or a frozen browser timer | Allocation-free intra-thread histograms report finalizer/slot/static/dynamic/publication p50/p95/p99/max and tails; the release gate uses sustained 4P coverage and state identity. |
| Cutout or vertex alpha could be globally misclassified as soft | ROM-derived immutable batch/texture provenance and a reviewed fingerprinted allowlist are required; missing provenance fails closed to hard. |
| A whole-object fade could leak across viewports or hide hard batches | The target renderer design is per-viewport, batch-local opacity; global `Object.opacity`, model, texture, and collision state are forbidden. |
| An "exact" pass could test only the nearest sphere hit | Each source must retain every conservative broad-phase candidate and choose the earliest exact rounded-lens contact. A first sphere false-positive may hide a later real pyramid blocker. |
| A mathematically valid basis could disagree with rendered pixels | The obstruction pose must come from a pure copy of the renderer's inverse-view recipe: matrix `+X`, `+Y`, and `-Z` are right, up, and forward; shake mode is part of the validated tuple. |
| A candidate could be validated before its look-at rotation is applied | Alternate and emergency candidates build their final `Camera` and exact lens pose before validation; publication copies that camera and may not retarget afterward. |
| Safe orientation endpoints could sweep through a wall | Fixed-orientation motion receives an exact continuous sweep. Orientation-changing motion requires conservative SE(3) coverage or a declared interpolation discontinuity with validated endpoints. |
| The sampled exact fallback could pass unit tests but stall a fixed tick | Kernel/source counters are mandatory in shadow and release runs. The first route found maximum 7 retained static candidates but 2,025 stationary tests from fallback sampling; the sampled fallback is now non-shipping oracle code. |
| One retained dynamic AABB could hide an unbounded full-model sweep | Both sphere and exact object queries now traverse the immutable per-model BVH under aggregate instance/node/chunk/triangle budgets. The first bounded route peaks at 37 sphere nodes/62 triangles and 29 exact nodes/40 triangles; fault injection and breadth remain release gates. |

Default-on remains blocked even though these defects are closed. The unresolved
items are product-quality and breadth work, not permission to weaken safety:
synthetic dynamic invalid/recovery and non-door chord fault fixtures; explicit
soft-occluder enrollment/fade and cross-viewport pixels; a moving-solid
correction transition plus pinned concave/tunnel fixtures; motion/chatter metrics
and manual review; browser/load-time budgets; simultaneous resize/projection-fault
coverage; and current-candidate 47-row/20-track, GCC, and resource-plateau breadth.

### 6.3 Exact-lens critical path from shadow to production

This order is mandatory. Later visual polish cannot be used to bypass an earlier
geometry, cost, or authority exit:

| Order / owner | Engineering deliverable | Automated verification | Exit evidence |
|---|---|---|---|
| PERF-01 geometry — implementation complete, release evidence open | `842d62f` replaces the production sampled interval fallback with a 96-test continuous Lipschitz interval proof. The sampled implementation is reachable only through the ROM-free oracle API. Count SAT, bounded tests/exhaustion, stationary tests, and refinements; budget exhaustion returns `INVALID`, and two-phase composition retains the conservative sphere `HIT`. | Property/fuzz corpus compares production against the oracle across translation, scale, tangent, initial-overlap, face/edge/vertex, high-coordinate, and thin/oversized triangles under Clang/GCC, strict warnings, ASan/UBSan, and wasm32. Inject every work-cap boundary. | Zero false clears versus the oracle; zero ordinary-route sampled fallbacks; stationary-test p99 <= 64 and max <= 128; deterministic hit bytes and stable-ID ordering. |
| PERF-02 collision — implementation complete, release evidence open | `d8c8bb7` builds stable-order eight-triangle chunks and an auxiliary deterministic balanced BVH, validates topology/coverage/containment before publication, checks generation and integrity during both sphere/exact traversal, and enforces aggregate instance/node/chunk/triangle/stationary budgets. `da93930` adds identity/rotated/scaled equivalence and malformed-node/chunk/per-model-cap/generation fault tests against the production representation. | Aggregate multi-instance cap injection, whole-ROM model census, repeated real load/free/address-reuse soak, and moving-door snapshot tests. | Dynamic p99 <= 2 instances, max <= 4; maximum 64 nodes/32 chunks/256 retained triangles/128 stationary tests per corridor; zero truncation/invalid/degraded results; cache within recorded load/memory budgets. |
| RUNTIME-01 camera/render — implementation complete, fault fixture open | Store renderer-derived exact guard and full fallback tuple per slot. Ordinary boom queries use the two-phase result. Alternate, emergency, recovery, scripted, stationary, and post-validation build and validate the final `Camera` orientation; publication performs no later retarget. Dynamic invalidity is never cleared or published, and failed ticks retire camera/object interpolation history. | Add a ROM-free state-machine seam that injects exact clear/hit/invalid, dynamic census failure/recovery, generation mismatch, stale tuple, final retarget, and orientation-changing interpolation. Runtime fixtures assert identical open-space bytes, authored fallback on source failure, and conservative sphere hit on healthy exact-work exhaustion. | Zero final-pose penetration or hidden resolvable target; zero stale generations; invalid never clears; failed publication cannot be snapshotted or interpolated; recovery requires a complete fresh tuple. |
| MOTION-01 design/QA | Tune expansion-only spring/hysteresis and deterministic shot scoring after safety decisions stabilize. Mark orientation-changing cuts discontinuous unless an SE(3) sweep is implemented. | Fixed routes compute jerk, retract latency, recovery duration, blocker churn, shoulder flips, emergency dwell, and discontinuity counts at 20–240 Hz presentation. Reduced-motion and camera-shake-off controls included. | Numeric thresholds pass and worst 1% clips receive signed manual review across 4:3, ultrawide, portrait, and split-screen. |
| VIS-01 rendering/content | Add reviewed soft-occluder policy and generation-keyed, per-viewport, batch-local fade. Hard geometry continues to require clearance; emergency racer fade remains presentation-only. | Two-view opposing visibility fixture, opaque/cutout/translucent pixels, freed-ID reuse, state/RNG/event/audio hashes, and GL/WebGPU/browser screenshots. | No cross-viewport contamination, no hard-wall fade substitution, no gameplay mutation, and approved content census. |
| RELEASE-01 QA/release | Rerun the complete current-revision matrix in optimized artifacts and archive the evidence schema below. Flip the default only in a dedicated reviewed commit with immediate Observe rollback. | 84 registered tests plus release suite, GCC/Clang, sanitizers, native backends, wasm/browser, resource plateau, load time, long 4P soak, same-binary Legacy/center-ray negative controls. | Every CAM-00–CAM-09 and LENS-01–LENS-08 row green; definition of done satisfied; release notes/manual review signed. |

Absolute screen-size independence is evaluated in world space: pixel resolution
must not alter a trace when effective aspect/FOV are equal, while each distinct
viewport shape gets its own latched projection and exact guard. The rollout must
therefore compare equal-aspect identity separately from 4:3/16:9/21:9/32:9,
portrait, split-screen, FOV-cap, DPI, fullscreen, and live-resize coverage.

Every final evidence row must record:

```text
gate | commit/binary | ROM hash | platform/backend | viewport/FOV/aspect |
route/seed | policy | trace/capture path | assertions | result | reviewer/date
```

### 6.4 Release execution protocol

The remaining work is executed in six reviewable waves. A wave may prepare later
fixtures, but it cannot declare their gate satisfied until every predecessor is
green on the same candidate commit. “Pass” means the raw trace/capture and parser
result are archived; a terminal summary alone is not evidence.

| Wave | Engineering output | Test/validation output | Promotion rule |
|---|---|---|---|
| A — adversarial fixtures | Add injectable dynamic-census invalid/recovery, non-door chord, thick-wall embedded target, concave corner, tunnel, rotating door, tuple mutation, and simultaneous resize/projection-fault seams. Keep each fault behind test-only or deterministic route control. | Every fixture has a positive control that fails for the intended reason and a Modern arm that either publishes a fully proven pose or deliberately publishes no sidecar and cuts. | No `INVALID -> CLEAR`, no stale snapshot/interpolation source, no unsafe pose, and stable provenance bytes over repeated seeds. |
| B — geometry and content breadth | Add the dense temporal-envelope oracle; inject aggregate multi-instance work caps; rerun every legal vehicle/track row and all 20 tracks; repeatedly load/free models and reuse object addresses. | Clang/GCC native units, ASan/UBSan, wasm build, 47/47 vehicle/track and 20/20 track reports, cache/work histograms, memory plateau, and load-time delta. | Zero false clears versus oracle, omission, unclassified hard material, stale generation, truncation, invalid/degraded healthy route, or unbounded memory growth. |
| C — motion product quality | Instrument retract latency, recovery duration, positional/angular velocity and jerk, blocker churn, shot-side flips, emergency/fade dwell, and discontinuity density. Tune only expansion/recovery/shot scoring; hard clearance is immutable. | Deterministic routes at 20/30/50/60 fixed cadence and 20–240 Hz presentation, shake on/off, reduced-motion, 1P–4P, 4:3/portrait/16:9/21:9/32:9, authored/FOV extrema. Archive worst 1% clips. | Section 7.3 numeric bounds pass; no repeatable nausea, steering ambiguity, shoulder flap, or prolonged unreadable framing in signed design/QA review. |
| D — visibility policy | Finish the fingerprinted hard/soft/nonblocking census. Either implement generation-keyed per-viewport batch-local soft fade or record an explicit release deferral. Never fade hard geometry. | Opposing split-view pixels, opaque/cutout/translucent fixtures, freed-ID reuse, GL/WebGPU/browser captures, and state/event/RNG/audio hashes. | No cross-viewport contamination, object/global material write, hard-wall substitution, identity leak, or authority delta. |
| E — platform parity and soak | Build optimized current source with Clang, GCC, and Emscripten; run native GL, native WebGPU, and Chromium, including fullscreen, DPR, live resize, projection fault, minimize/resume, pause/restart, and repeated level transitions. | Backend-equivalent safety/authority counters, browser frame/load distributions, 30-minute 4P obstruction soak, teardown diagnostics, and resource plateau. | Correctness matches native reference; p99/load/memory budgets in sections 7.6 and 10 pass; no sanitizer, browser console, teardown, or timing regression. |
| F — default decision | Freeze tuning and evidence schema; rerun the entire matrix from clean optimized artifacts; perform independent code, gameplay, and release review. Change only the default in a dedicated commit. | Signed CAM-00–CAM-09 and LENS-01–LENS-08 ledger, release note, rollback drill, ROM/build fingerprints, and archived captures/traces. | Default flips to Modern only if every row is green. Any failure keeps Observe default; rollback requires no save migration. |

Each engineering change follows the same loop:

1. Add or identify a failing positive control before changing policy.
2. Implement without broadening gameplay authority or mutating collision/object
   state.
3. Run ROM-free unit/source/fault tests, then the smallest deterministic ROM route.
4. Run the affected display/multiplayer/lifecycle/performance matrix on an
   optimized artifact; run sanitizer and alternate-toolchain tiers when geometry,
   lifetime, transform, or publication code changes.
5. Archive counters and captures using the evidence schema, update the ledger,
   and request the named review. Never waive a failed hard invariant through a
   threshold or content exception.

CI tiers are explicit:

- per-change: source authority guards, 81 native tests, geometry/fault units;
- camera-integration: same-binary controls, dynamic route, projection fault,
  emergency readability, 3P+T.T., and selected lifecycle route;
- nightly: display/FOV matrix, current 20-track/47-row breadth, 4P performance,
  sanitizers, model/address-reuse soak;
- release: all nightly gates plus GCC, GL/WebGPU/Chromium/wasm parity, manual
  worst-1% review, clean-install/load/teardown and rollback drill.

Failure ownership follows the first broken invariant: geometry/source failures go
to camera-collision engineering; stale generations or publication go to
camera/presentation; motion-only failures go to camera design; material/fade
failures go to rendering/content; authority/hash deltas go to gameplay/platform.
A downstream visual workaround never closes an upstream safety failure.

### Implemented and remaining file seams

- `platform/camera_obstruction.{c,h}`, `camera_obstruction_resolver.{c,h}`,
  `camera_obstruction_transform.{c,h}`, and `camera_obstruction_query.{c,h}`:
  dependency-light geometry, temporal policy, transforms, and source composition.
- `platform/display_config.{c,h}` and `game/src/camera.c`: exact per-viewport
  projection records and generation ownership.
- `game/src/thread3_main.c`: the single authoritative finalizer call in gameplay
  and loaded-scene paths.
- `game/src/racer.c`: desired pivot/focus metadata only; no duplicated collision
  solver in vehicle modes.
- `game/src/tracks.c`: static occlusion-world build/lifetime and resolved segment
  consumption; retain the void curtain as a fallback/background mechanism, not the
  primary correction.
- `game/src/objects.c`: stable object enumeration and read-only transforms; do not
  reuse interaction-mutating collision entry points.
- `game/src/camera_obstruction_runtime.{c,h}`,
  `camera_dynamic_occlusion.{c,h}`, and `camera_object_occlusion.{c,h}`: native
  sidecar, policy integration, and immutable dynamic/model cache ownership.
- `platform/presentation_snapshot*`: resolved pose/projection/discontinuity when
  diagnostic snapshots are enabled.
- ROM-free resolver/transform/query tests, authority/cache/dynamic/observe source
  guards, and `tests/check_camera_obstruction_runtime.py`; focused remaining
  screen/multiplayer/mode fixtures stay release-blocking.
- `CMakeLists.txt`, `tools/run_checks.py`, `tests/README.md`, release checklist,
  changelog, and open-item closeout when the work lands.

## 7. Test and validation program

### 7.1 ROM-free geometry units

Synthetic cases must include:

- no hit and exact endpoint touch;
- perpendicular and oblique walls;
- triangle face, edge, vertex, seam, and T-junction;
- paper-thin and two-sided shells;
- start inside, pivot inside, camera inside, and no valid boom;
- multiple blockers with stable earliest-hit and equal-fraction tie cases;
- degenerate/zero-area triangles, extreme coordinates, NaN/Inf rejection;
- rotated/scaled object-local meshes and moving transforms;
- stock 4:3 guard near 13.88, widescreen/high-FOV guards, portrait, and FOV cap;
- desired orientation changing after retraction;
- legacy center-ray positive control that passes while a near corner intersects;
- stable results after candidate/BVH insertion order is deliberately permuted.

Property/fuzz assertions:

- returned fraction is finite and in `[0, 1]`;
- moving no farther than the reported safe fraction is clear within tolerance;
- moving beyond a real hit violates clearance;
- increasing the guard cannot increase the safe boom;
- translating or uniformly scaling the entire scene transforms the answer
  consistently;
- query inputs and world are byte-identical before/after.

### 7.2 Runtime integration fixtures

Required deterministic routes:

- Ancient Lake canyon wall contact and high-speed wall approach;
- concave corner, tunnel ceiling, under-bridge, and narrow corridor;
- Timber's Island locked door and door opening while obstructing;
- moving/rotating solid object crossing the boom;
- car, hovercraft, and plane at every zoom, boost, reverse, drift, spinout, and
  maximum practical boom;
- loop entry/exit and roll continuity;
- Taj dialogue translation plus maximum shake near a wall;
- challenge finish orbit, normal finish spectate, fixed door, cutscene camera bank,
  P2 control promotion, 3P T.T., and 4P;
- pause/FOV change, live resize, minimize/resume, stage unload/reload, race restart,
  and camera-mode teleport.

Every route records geometric telemetry and selected pixel frames. Pixel tests prove
the visible symptom; geometric tests prove the camera did not merely hide it.

### 7.3 Motion-quality gates

Initial release targets, to be calibrated by CAM-00 and then frozen before CAM-06
implementation:

- unsafe clearance is corrected in the same authored tick; there is no one-frame
  grace period;
- with desired pose and blocker set held constant, resolved boom expansion is
  monotonic and reaches at least 95% of the clear desired boom within 750 ms for a
  correction up to 250 world units and 1.25 seconds for a larger correction;
- expansion advances no more than 20 world units or 25% of remaining error per
  authored tick, whichever is smaller, unless a mode transition marks a
  discontinuity;
- no clear/blocked/clear or left/right/left correction cycle occurs inside 12
  authored ticks unless the intermediate pose became geometrically invalid;
- triangle/facet changes within one hard object or continuous track surface do not
  reset recovery or count as a new blocker owner;
- emergency snaps, overlap depenetrations, and alternate-shot switches are all
  counted and must appear in the visual-review queue; none may be silently folded
  into ordinary recovery statistics;
- a 30-minute manual review covering high-speed 1P, 2P, ultrawide, and maximum-FOV
  play reports no repeatable correction-induced nausea, shoulder flapping, loss of
  racer readability, or steering ambiguity.

These are falsifiable starting bounds, not magic constants. CAM-00 may revise them
once, with traces and side-by-side captures recorded in this document. Later tuning
requires the same evidence and may not weaken hard clearance.

### 7.4 Display matrix

At minimum:

- 320x240 and 640x480 4:3;
- 1280x720, 1920x1080, and 3840x2160 16:9;
- 1920x1200 16:10;
- 2560x1080 and 3440x1440 21:9;
- 5120x1440 32:9;
- 1080x1920 portrait and a narrow resizable browser viewport;
- forced 4:3 inside widescreen;
- authored FOVs in the asset census, user minimum/default/maximum, and horizontal
  cap engaged/disengaged;
- SSAA 1x/2x/4x and GL/WebGPU/browser.

Do not create redundant world-space goldens for resolutions sharing an aspect.
Instead assert identical camera traces and use resolution-specific pixels only for
render/UI coverage.

### 7.5 Multiplayer and authority

- Compare 1P, 2P, 3P+T.T., and 4P camera IDs, solve counts, blockers, segments, and
  per-viewport fades.
- Move only one viewport's racer into an obstruction; every other viewport's camera,
  object route, opacity, and pixels must remain unchanged.
- Compare fixed/legacy arms for simulation state, gameplay events, consumed input,
  RNG stream, sound-event stream, and PCM. State/events/input/RNG/sound events must
  be identical; a delta blocks rollout. PCM may differ only through the audited
  resolved listener transform, must be deterministic for that pose, and must have
  an unchanged dry/event control. If the audio census shows that preserving logical
  listener position is less disruptive, record that product decision explicitly.
- Specifically measure `obj_visibility_tick()` racer admissions and resulting AI
  timers/steering from the logical basis. The resolved presentation camera may
  change pixels and draw admission, but may not change this simulation trace.

### 7.6 Performance, memory, and lifecycle

Telemetry per tick and viewport:

- broadphase nodes/triangles visited;
- hard/soft object candidates;
- sweep/refinement/alternate-shot query counts;
- cache bytes and build time;
- query time, correction distance, blocker ID, overlap/fallback events;
- projection/pose generations and segment transitions.

Release budgets:

- zero allocations during camera query/finalize;
- zero capacity truncations and zero unclassified occluders;
- measure the inclusive fixed-tick finalizer (authored snapshot, dynamic
  publication, every selected slot solve, and post-validation, excluding
  diagnostic `fprintf`) at the fastest supported NTSC cadence. It is no more than
  833,333 ns (5% of 16.667 ms) at p99 on the reference host; no more than 1% of
  samples may exceed that bound;
- record every sample above 1,666,667 ns (10%). An isolated release run permits at
  most 0.1% as an OS/browser scheduling tail and requires each to be reviewed;
  max is reported but is not a portable hard gate because descheduling and browser
  GC are not camera work. A repeatable algorithmic tail is release-fatal;
- separately report per-slot, static-query, dynamic-query, dynamic-publication,
  query-count, and zero-duration/browser timer bins. Static query fan is bounded
  at 64 calls per finalizer in the 4P gate;
- level-load cache construction adds no more than 10% to measured level-load time
  and remains within an explicitly recorded memory budget;
- browser/wasm32 meets the same correctness gates and reports its own time/memory
  distribution rather than inheriting native results.

`MDKR_CAMERA_PERF=1` uses a preallocated 10-us histogram and SDL's intra-thread
performance counter. It does not use the VI/rAF pacing clock, which is intentionally
frozen inside synthetic browser opportunities and would report false zeroes. When
disabled, it performs no clock reads or allocation. The release gate runs Observe
and Modern from one optimized binary, requires byte-identical state streams, and
records absolute cost as well as their delta.

Run Debug, Release, ASan, full alignment/undefined UBSan, GCC, supported native
backends, and browser WebGPU. Include repeated level load/unload, pause/restart, and
at least one long 4P obstruction soak to prove sidecar/object-generation cleanup.

### 7.7 Required existing regressions

At minimum run the registered equivalents of:

```text
display_config
video_config / video_config_runtime
presentation_snapshot
race_drive
door_blocks
collision_gridmask / collision_headroom / collision_untextured
race 2P / race multiplayer / viewport route isolation
challenge modes / Taj challenges / trophy series / first boss progression
determinism / authored RNG compatibility / state hash / render purity
camera snapshot coverage / presentation lifecycle and breadth
renderer backends / widescreen proportions / browser runtime
array bounds / native layout / full UBSan / resource plateau
```

The new gate is registered in `tools/run_checks.py`; it is not a manual-only script.

### 7.8 Same-binary controls

Add a diagnostic seam such as:

```text
unset | observe                  current default; telemetry, authored pose retained
MDKR_CAMERA_OBSTRUCTION=modern   experimental full static+dynamic correction
MDKR_CAMERA_OBSTRUCTION=center-ray intentionally incomplete diagnostic control
MDKR_CAMERA_OBSTRUCTION=legacy   original direct placement + void detector
MDKR_CAMERA_TRACE=1|2             summaries / per-query detail
MDKR_CAMERA_EXACT_SHADOW=1        non-publishing promoted-sphere/exact corridor comparison
```

Unknown values fail to Observe; a typo cannot silently select the known-unsafe
Legacy behavior. The immediate rollback ladder is `modern -> observe -> legacy`,
with no save persistence or authoritative-state migration.

The legacy arm must reproduce the wall/door lens violation in the same executable.
The modern arm must pass the invariant. A second injected control disables only the
near-plane guard while retaining a center ray, proving the test distinguishes a
point camera from a protected lens.

The option is diagnostic and environment/CLI-only. It is not persisted to saves and
does not become another player-facing quality knob.

## 8. Rollout and tuning

1. **Observe:** land CAM-00 instrumentation and fixtures with production behavior
   unchanged.
2. **Dark launch:** build caches and run queries without applying correction; compare
   predicted hits to captures and classify false positives/negatives.
3. **Terrain default:** enable hard track correction for ordinary follow cameras,
   retaining legacy A/B.
4. **Object and breadth default:** add doors/moving solids and all camera banks/modes.
5. **Polish:** enable alternate shots, soft fades, racer emergency fade, and player
   accessibility controls only after safety evidence is stable.
6. **Closeout:** run full release matrix, update the open item/changelog/handbook, and
   retain the legacy seam for at least one release cycle.

Tuning is data-driven. Produce heatmaps by track, mode, vehicle, viewport, blocker,
correction distance, and obstruction duration. Review the worst 1% of events
visually. Fix bad authored spectate points or misclassified assets at the content
policy rather than weakening global safety.

Suggested later player-facing controls:

- camera shake strength/off;
- obstruction recovery feel: responsive versus smooth, changing expansion only;
- racer emergency fade strength.

None may reduce hard clearance. FOV remains owned by the existing display settings.

## 9. Risk register and stop conditions

| Risk | Detection | Mitigation / stop condition |
|---|---|---|
| Camera correction or display choice changes AI via visibility | logical-admission/RNG/state A/B | Keep AI activity on desired pose + canonical authored projection; any delta stops rollout |
| Visual cache blocks invisible or soft material | provenance census + dark launch | Reclassify by stable asset/batch policy; do not lower global clearance |
| Visual cache misses `NO_COLLISION` geometry | legacy witness plus visual-triangle census | Final target includes opaque visual triangles, not gameplay facets alone |
| Ultrawide over-retracts | 21:9/32:9 correction distributions | Ship exact guard narrow phase before declaring ultrawide complete |
| Corner oscillation/shoulder flipping | blocker/candidate trace and motion metrics | hysteresis, prior-shot score, deterministic minimum dwell; stop if rapid alternation persists |
| Focus starts in geometry | overlap counters and targeted fixtures | depenetration/last-safe/emergency framing; never loop at zero boom |
| Resize races projection | generation assertions | latch pose/projection atomically; fail closed on mismatch |
| Dynamic object address reuse | generation soak under ASan | generation-key all blocker/fade state and clear on free/stage reset |
| Query stalls 4P/browser | p99/max telemetry | BVH/local-space caches, bounded alternates; do not accept truncation as optimization |
| Snapshot interpolation crosses wall | midpoint geometric control | revalidate sample or force discontinuity; do not re-enable smoothing on endpoint evidence alone |
| Void curtain masks remaining defect | modern run with void detector telemetry | geometric gate must pass with masking ignored; curtain is not acceptance evidence |
| Fidelity disputes | open-space byte identity + legacy seam | safe default, diagnostic legacy; document deliberate obstructed-shot differences |

## 10. Definition of done

The issue may be marked fixed only when all statements are true:

- Every CAM-00 through CAM-09 exit gate is satisfied.
- No required route reports hard lens penetration, stale segment identity, query
  truncation, unclassified hard material, invalid projection generation, or camera
  query side effects.
- Open-space desired and resolved poses are identical; equal-aspect resolutions
  produce identical camera traces.
- Simulation state, AI logical-admission trace, gameplay events, consumed input,
  RNG, and sound-event stream are invariant across legacy/modern correction and the
  complete display/FOV matrix. Any PCM delta is deterministic and isolated to the
  explicitly chosen camera-listener policy; no cue is added, removed, or reordered.
- All player, cutscene, promoted, and T.T. camera IDs used by loaded worlds have an
  explicit policy and coverage.
- Terrain, visible non-gameplay-collision geometry, closed doors, and moving solids
  are covered; soft/nonblocking exclusions are enumerated.
- Retraction/recovery, emergency framing, and alternate-shot motion pass numeric and
  visual review without seam chatter or shoulder flapping.
- 1P–4P, GL, WebGPU, browser, 4:3 through 32:9, portrait, FOV extremes, live resize,
  pause, lifecycle, sanitizers, determinism, state/event/input/RNG/PCM, performance,
  and memory gates pass.
- Same-binary legacy and center-ray controls demonstrably fail the exact invariants
  the production path passes.
- The consumer census and source guard cover every camera reader.
- Release notes state the safe-camera product decision and any intentional
  obstructed-shot fidelity differences.

Anything less is a partial mitigation and remains open in this document.
