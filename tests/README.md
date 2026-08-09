# Regression fixtures

Input scripts for `--input-script` headless verification of menu navigation.
Run e.g.:
```
MDKR_TRACE=1 ./build/mdkr64 --headless-frames 1700 \
  --input-script tests/input_scripts/nav_to_character_select.txt \
  --rom baserom.us.v80.z64 2>&1 | grep menu_init
```
Expected: `menuId=3` (character select). The options script yields `menuId=12`.
Both prove real input reaches the game and menus advance/diverge on navigation.

**Always run muted and headless** — `MDKR_AUDIO=0` plus `--headless-frames N`.
Omitting `--headless-frames` opens a window *and* the SDL audio device.

## Complete suite runner and `--build` contract

Every behavioural script accepts the same `--build` value: either a directory
(`--build build-rel`) or the executable inside it
(`--build build-rel/mdkr64`). Use the runner for a complete pass:

```bash
python3 tools/run_checks.py \
  --build build-rel --release-build build-rel --asan-build build-asan \
  --wasm build-web/mdkr64_web.wasm
```

The manifest registers 145 of the 149 `tests/check_*.py` scripts and expands to
158 tasks. The four it does not name directly
(`check_controller_settings_persistence.py`, `check_host_input_focus.py`,
`check_launcher_tabs.py`, `check_overlay_input_handoff.py`) are CTest companions that `rom_free_units` owns, so
every check script still runs exactly once in a default pass. That task also
runs the ROM-free display/endian/magic-code/object-layout/allocator/
runtime-contract, sprite-layout, RDP-interpolation, font-registry/SDF, and RL-1
CTests, while filename entry, locked-door collision, RAW16 audio, native-layout
safety, and widescreen/shadow safety run in their specialized
primary/Release/ASan/alignment configurations. Startup fails if a new check
script is not
registered, or if a `tests/test_*.py` has no CMake `add_test()` to carry it into
the ctest task. The run owns one temporary save directory and exports it as both
`MDKR_SAVE_DIR` and `MDKR_TEST_SAVE_DIR`, so no suite run writes the
repository's playable `save/`; tasks run sequentially because they share that one
directory and several fixtures intentionally replace its `eeprom.bin`. A
standalone check still defaults to `save/` and restores the EEPROM images it
found there. Inherited `MDKR*` hooks are cleared except for those two and
`MDKR_TEXCACHE_VERIFY`, `MDKR_VIDEO_CONFIG_PATH` is pointed at the null device,
and audio is forced off. `--only NAME`, `--role ROLE`, `--primary-only`,
`--skip-instrumented`, and `--skip-wasm` are iteration/configuration tools; a
default run is the complete gate.

Headless frame numbers are presentation-loop iterations, not authoritative VI
fields. Fixtures calibrated before authored cadence became the default must set
both `MDKR_SIMULATION_CADENCE=enhanced` and `MDKR_SYNTH_FIELDS=1` in their clean
environment when their scripts, captures, or thresholds intentionally retain
that historical one-field timeline. Shipping-cadence gates instead select
`original`/two fields explicitly (or test the fresh default), and the cadence
gate verifies that distinction. Never repair a fixture mismatch by weakening
its gameplay or pixel threshold without first classifying its time base.

The v0.3 release gate passed the **38-task optimized native/sanitizer stage in
22m17s**, the **29-task Debug primary stage in 12m31s**, and both wasm-only
tasks — including the linked layout check and real Chromium — in **1m05s**. The
vehicle sweep always emits a failed child's diagnostic tail and recognizes
UBSan text; there is no retry or tolerated-failure path in the registered gate.
The ROM-free CI-contract task fails closed if push/PR triggers,
Linux GL/WebGPU, macOS WebGPU warnings, sanitizers, linked wasm, browser save
custody, immutable action pins, or either ROM guard disappears.
The address-domain source task separately rejects every raw native
pointer-to-32-bit narrowing outside `address_domains.h` and
`dkr_native_ptr.h`; direct conversions remain compiler errors.

`camera_obstruction_authority` is a ROM-free source-level CTest for the camera
occlusion seam. Until the resolver is integrated it permits no tick calls; once
present, it requires exactly one `camera_obstruction_tick` call in each fixed
tick route (`mode_game` and `update_menu_scene`), after the TT camera author and
before sort/LOD/visibility. It also rejects resolved/display/projection camera
APIs from the simulation-owned sort, LOD, and visibility passes: those consumers
must retain their legacy logical basis builders or an explicitly named logical
basis/view replacement.

`camera_obstruction_resolver` is the ROM-free policy layer above the immutable
geometry sweep. It exercises immediate skin-backed retraction, bounded fixed-step
recovery, last-safe revalidation after projection changes, discontinuity reset,
invalid-world fail-safe behavior, deterministic repeatability, and the diagnostic
center-ray near-plane false-clear control. It intentionally owns no DKR state or
environment parsing; the fixed-tick integration supplies those inputs.

`camera_object_bvh` is a ROM-free white-box test of the production immutable
object-model index. It compares indexed sphere and exact rounded-lens results
byte-for-byte with full-world queries under identity and 64 rotated/scaled
inputs, exercises node/chunk/triangle/stationary budget exhaustion, and injects
NaN bounds, invalid leaf tags, duplicate children, corrupt chunk coverage,
shrunken parents, stale generations, and same-address generation reuse. Every
fault must return `INVALID` (or be rejected by the pre-publication validator),
never an ordinary clear. This test intentionally includes the production cache
translation unit so it cannot drift into a second test-only BVH implementation.

`camera_dynamic_temporal` exercises the production moving-object presentation
envelope against 4,097 renderer-transform samples per case. It covers endpoint
identity, translation/rotation/scale, shortest-angle wrap, high coordinates, 128
deterministic randomized poses, invalid transforms, and the moving non-door
camera-chord cut broad phase. No dense intermediate object AABB may escape the
published envelope.

`camera_dynamic_publication` executes the invalid-until-proven dynamic census
state machine. It proves that a failed census has no current query source, the
first recovered census advertises a discontinuity with no previous transform,
all hard objects cut on the failed image, and interpolation resumes only after
two consecutive complete publications.

`camera_dynamic_precedence` pins the dynamic narrow-phase winner ordering
against the production comparator itself, one key at a time and in both
directions: time outside the public tie window, then spawn generation, model
generation, source triangle, hit stable ID, and authoritative list index. It
includes the production translation unit so the ordering under test is the one
the sweeps call, rather than a second copy of the rule.

`camera_dynamic_temporal` and `camera_lens_pose` link `game/src/hasm/math_util.c`
so the renderer matrix recipe they compare against is the production one. Both
carry an absolute golden arm, so a sign error in `mtxf_from_transform` fails them
instead of moving both sides of a differential together.

`camera_target_visibility` pins the current-pose readability classification. A
thin local skin may be excluded, a remote blocker is `HIDDEN`, a focus that
remains in a thick slab is `EMBEDDED` and not visible, and source/numeric failure
is `INVALID`. These outcomes cannot collapse into a convenient clear result.

`camera_obstruction_observe` protects the camera authority boundary and rollout
gate. The two fixed-tick calls sample all eight authored slots, publish the
dynamic occluder list, latch selected viewport projections, and resolve into a
native presentation sidecar. They never write `Camera`/`gCameras`, selection,
sort/LOD/visibility, audio, or other authoritative state. `render_scene` and the
interpolation snapshot walker consume the sidecar only inside presentation.
Correction is opt-in: unset `MDKR_CAMERA_OBSTRUCTION` selects `observe` — the
authored camera — and the launcher's **Camera** setting exports `modern` onto
the same variable when a player opts in. Set `center-ray` for the deliberately
incomplete diagnostic control, or `legacy` for original direct placement; an
unrecognised value falls back to the default, `observe`. `MDKR_CAMERA_COMFORT=reduced`
turns on the presentation-only reduced-motion filter over the corrected camera;
unset and unrecognised values are `authored`. Set
`MDKR_CAMERA_TRACE=1` for summaries or `=2` for per-slot desired/effective,
projection/guard, mapping, intent age/pivot/target, and hit diagnostics; this is
intentionally separate from `MDKR_TRACE`. The sidecar receives last-author-wins
intent records at the post-dialogue/shake racer seam, T.T.'s final spectate pose,
and scripted cutscene transform writes. The finalizer consumes one fresh intent
per selected physical slot; only an unchanged, already-consumed paused pose may
reuse stale intent. Render projection generation and every presentation camera
read are checked/routed through the same scoped sidecar.

`check_camera_obstruction_runtime.py` is the ROM-backed, same-binary Ancient Lake
release witness. It runs Legacy, intentionally incomplete Center-ray, and Modern
for 5,200 deterministic frames. Legacy and Center-ray must positively reproduce
lens penetration; Modern must actually correct poses while publishing zero
penetrated resolved lenses, degraded sources, invalid slots, duplicate solves,
projection mismatches, missing caches, invalid transforms, or capacity failures.

```bash
python3 tests/check_camera_obstruction_runtime.py --build build-rel \
  --rom baserom.us.v80.z64
```

`check_camera_obstruction_display_matrix.py` runs 24 Modern geometric arms: 4:3
low/high, 16:9, 21:9, 32:9, and portrait crossed with authored, minimum 20-degree,
maximum 140-degree capped, and maximum 140-degree uncapped FOV policy. At every
FOV it requires equal-aspect resolved-camera traces to match byte-for-byte,
catching any attempt to derive world framing from resolution, render scale, or
rounded internal render targets.

The remaining registered camera runtime gates are deliberately orthogonal:

- `check_camera_dynamic_obstruction_runtime.py` requires every dynamic-instance
  blocker attribution to coincide with an applied correction and rejects every
  hard-object publication omission.
- `check_camera_projection_fallback_runtime.py` injects a latch failure and allows
  restoration only for a freshly revalidated exact camera/projection pair.
- `check_camera_obstruction_lifecycle_runtime.py` drives quit, race restart, and
  hub/race reloads; each level generation needs a reset witness and fresh tick-1
  solve. Its Adventure arm also proves an open/moving door is published safely.
- `check_camera_emergency_readability_runtime.py` disables alternate shoulders in
  a test arm and requires real corrected gameplay rows to exercise the 96..254
  per-viewport racer opacity envelope without compromising clearance or target
  visibility. Emergency elevated/azimuthal endpoints are counted separately.
- `check_camera_3p_tt_runtime.py` validates the 3P T.T. fourth viewport/camera 3
  directly, avoiding an unrelated inherited autopilot-progress limitation.
- `check_camera_motion_quality.py` captures the MOTION-01 motion metrics
  (retract latency, recovery, jerk, shoulder flips, emergency dwell, published
  cuts, blocker churn) on the lake, hub, and 3P T.T. spectate routes under
  Modern. Chatter and shoulder-flip invariants gate hard; the analog
  distributions print as the labelled baseline for threshold-setting.
- `check_camera_obstruction_performance_runtime.py` runs one optimized binary in
  Observe, Modern, and Modern + reduced motion over a long 4P route, requiring
  at least 5,000 active four-viewport fixed ticks (more than 83 seconds at 60 Hz).
  It gates finalizer/query percentiles, tail counts, query fan, static cache
  bytes/build time, exact dynamic-sidecar allocation bytes, and byte-identical
  v3 authority state across all three — which is `Camera.Comfort`'s
  presentation-only proof as well as the camera's.
  `MDKR_CAMERA_PERF=1` is allocation-free and reads an
  intra-thread performance clock; it never uses the synthetic VI/rAF clock.

The three source-role camera cache tasks are static audits of the immutable
occlusion caches the runtime queries, and hold no ROM:

- `check_camera_track_occlusion_cache.py` pins the static visual-triangle cache
  built from a finalized level model: it is copied once in segment/batch/face
  asset order, never mutated, and released with the level, so a camera boom
  spanning arbitrary segments cannot read gameplay state or a stale allocation.
- `check_camera_object_occlusion_cache.py` pins the object-model cache's
  ownership and generation lifecycle: the cache owns its copies, its registry
  key is the model allocation, and the entry is erased before that address can
  be reused by a later loaded object.
- `check_camera_dynamic_occlusion.py` pins the fixed-tick dynamic hard-occluder
  publication: identity assignment, the precedence order used to break ties
  between competing hits, and the purity of the census with respect to
  authoritative state.
The ROM-model task independently inflates all 445 supported object/level models
and validates every renderer-consumed geometry/visibility region, batch
sentinel, texture reference, and triangle-local vertex index, including dormant
variants no route instantiates.

`check_renderer_backends.py` additionally writes every GL present from frame 15
through 229 and rejects an isolated black buffer between completed images. This
owns the F-28 consecutive-capture contract.

### App-shell drag-and-drop ROM acquisition — `tests/check_shell_dropfile.py`

```bash
python3 tests/check_shell_dropfile.py --build build \
  --rom baserom.us.v80.z64
```

Q2: the NSOpenPanel and typed-path ROM-acquisition arms call `RomPanel_setRom()`
directly and are unit-tested (`tests/test_app_shell.cpp`); the drag-and-drop
arm is entered by an `SDL_DROPFILE` handler and had no coverage of its own.
`MDKR_APP_SMOKE_DROP=<path>` (inside the existing `MDKR_APP_SMOKE_FRAMES`
launcher smoke) queues that event for `AppHost::pumpAndShouldQuit()` after
SDL's platform translation boundary, where it uses the exact live-event
handler and ownership contract. It is not passed through `SDL_PushEvent`:
reserved platform events cannot be portably round-tripped through SDL2-on-SDL3
compatibility layers. The gate drives the handler both directions: a supported
ROM must be accepted (same path back, `valid=1`, the picker's own verdict
message, and persisted to prefs), and a non-ROM file must be refused gracefully
(`valid=0`, an explanatory message, exit 0, nothing persisted) — no crash
either way. Every run points `MDKR_APP_PREFS_DIR` (a test-only override
alongside `MDKR_SAVE_DIR`) at a private temporary directory, so this is the one
check that can reach a successful `AppConfig::save()` without ever touching the
real machine-shared `SDL_GetPrefPath("mdkr64","mdkr64")` prefs file.

`user_paths` is the ROM/SDL-window-free packaged-data contract. It supplies a
deterministic preference provider to a synthetic `.app`, verifies that video
config and the complete known save set migrate outside the bundle, checks
Resources and legacy CWD fixtures remain byte-identical, proves existing
destinations and explicit `MDKR_VIDEO_CONFIG_PATH`/`MDKR_SAVE_DIR` overrides
win, and exercises the fail-closed preference-discovery path.

### Cooperative final shutdown — `tests/check_final_shutdown.py`

```bash
python3 tests/check_final_shutdown.py --build build \
  --rom baserom.us.v80.z64
```

The gate runs finite GL and WebGPU sessions in isolated save directories. It
requires exactly one terminal marker in the order headless boundary → audio →
backend → frontend → SDL, with zero live backend/frontend objects. Positive
controls reject nonzero ownership and reordered teardown. Run it against
Debug, Release, and ASan after changing exit, renderer, SDL, or audio
ownership.

### Repeated browser resource ownership — `tests/check_browser_resource_plateau.py`

```bash
python3 tests/check_browser_resource_plateau.py \
  --engine-dir build-web --shell-dir dist/web \
  --rom baserom.us.v80.z64
```

The gate runs four real race loads through production pause-menu restarts in an
isolated Chromium profile. It requires stable warmed game/audio heaps, exact
physical/music/jingle/SFX voice-state conservation, coherent non-growing
WebGPU generations, stable pointer-registry ownership, and zero terminal
frontend/backend/host/AudioWorklet ownership. Its synthetic controls prove that
missing generation evidence and a false voice-conservation row are rejected.

The ordinary `browser_runtime` task also owns the temporary authored-cadence
containment budget: exactly one NTSC/60 source-clock initialization, no
sub-two-field update, 24–36 FPS median, 40.0 ms p95, 45.0 ms p99, and at most
two render frames for either async pipeline completion or a consecutive
last-complete-frame hold. It reports the raw maximum and frame number without
using that scheduler/menu-transition
outlier as a pipeline proxy.

`check_webgpu_fault_matrix.py` cross-checks the public 113-point registry
against the executable runtime matrices. All 83 native-required points must be
named by `check_webgpu_recovery.py` or `check_app_adopted_pacing.py`; all three
browser-required points (one browser-only and two with intentionally different
native/browser policies) must be named by `check_browser_runtime.py`; and none
of the 29 dormant inherited routes may receive runtime-coverage credit.

Wave 2 adds the ROM-free `level_lighting` CTest and
`check_remaster_lighting.py`. The latter runs Ancient Lake and Fire Mountain
through both shipped backends, requires a normalized and restrained
header-derived rig, proves smooth normals reach racer and character fragments,
compares production against the baked control, and requires Pure/Restored to
remain byte-identical when the diagnostic seam toggles.

## World-depth visual gates

The world-depth epic adds the first three checks below; the fidelity and
presentation waves extended the same list, which ends with its browser
production coverage:

- `check_world_fx_capture.py` proves the typed capture instrument is neutral
  and depends on the exact matrix registry;
- `check_world_fx_matrix.py` measures bounded, backend-identical ownership over
  bright/dark/snow/water/hub/boss and 2P/4P content, including exact map count
  and memory policy;
- `check_world_shadows.py` proves visible map darkening, actor projected-decal
  release, exact gameplay-state equivalence, and byte-identical fallback under
  forced optional-resource loss — and repeats the whole off/on pair per backend
  in **shipping configuration** (`MDKR_TRACE` absent), requiring the same pixel
  handoff and a byte-identical frame;
- `check_state_hash.py` anchors the fidelity architecture's Phase 1
  instrument: two identical runs must produce byte-identical per-tick
  `[SIMHASH]` streams (`MDKR_STATE_HASH=3`; versioned 64-bit hash over the
  authoritative runtime contract), a different window size AND the
  other renderer backend must not change one bit of authoritative state
  (the first machine-checked slice of the presentation-invariance
  invariant), and the `MDKR_RNGSEED=legacy` positive control must diverge
  at tick 0. Three process-determinism arms re-run levels 41, 37 and 11 and
  require each to agree with itself. The `sim_sched` CTest covers the
  exact-integer fixed-step accumulator (hour-long zero-drift NTSC/PAL,
  catch-up budgets, stall rebase, rational alpha).
  **Field-set versions.** The gate runs **v3**. v1 and v2 remain selectable
  byte-for-byte for archived comparisons; v2 first widened the object/particle
  integrator and exposed ten process-nondeterministic levels that v1 missed.
  v3 adds globals and progression, behavior properties, interactions, racer
  physics/AI/controller state, model animation cadence, and the former
  render-owned distance/opacity/LOD fields. It excludes host pointers and
  presentation-only caches. Nine independent controls use
  `MDKR_TEST_HASH_PERTURB=<family>:<tick>` to flip one covered byte only while
  hashing and require exactly one changed row; the archived v2/v1 x-rotation
  control remains. The exact field table and exclusions live beside the
  implementation in `platform/sim_hash.c`.

  **Current production policy:** Original gameplay cadence remains authoritative.
  Non-Original Frame Limit values schedule additional presentation/input-pump
  opportunities. With MotionSmoothing=off they hold the last authored image;
  with MotionSmoothing=interpolate they may submit a complete immutable
  in-between image. Presentation cannot issue a simulation ticket, consume an
  input ticket, advance audio, or mutate the v3 authority stream.
- `check_render_purity.py` is the fidelity spec's §12.2.1 gate. Skipping half
  of all scene renders (`MDKR_TEST_SKIP_RENDER=odd`) must leave the raw v3
  `[SIMHASH]` stream byte-identical. There is no test-only state subtraction:
  HUD and ordinary-object texture dice execute in fixed authority with
  cadence-compatible RNG ownership; opacity and racer LOD are draw-local;
  animation cadence, visibility, distance/order, light phase, and the
  course-arrow timer advance once per fixed tick. An explicit
  `MDKR_TEST_RENDER_IMPURITY=1` gameplay-RNG write must make normal and skipped
  schedules diverge, proving sensitivity. The gate's first runs root-caused the
  skydome camera-follow write and two 1-ULP add/subtract restore pairs on racers.
  Phase 3 Wave B added three internal arms on the same binary. All require
  `MDKR_INTERNAL_TEST_TOKEN=mdkr64-presentation-replay-v1`; without that exact
  token every replay flag is inert. **C** (`MDKR_TEST_REPLAY_WALK=1`)
  re-walks every tick's captured display list a second time through the HLE,
  with the frozen shadow-matrix registry restored and the same
  view-projection, and requires the `[SIMHASH]` stream to stay byte-identical
  to arm A's; **D** dumps frames from that same replay and requires them
  byte-identical to a non-replay run, catching a near-but-not-exact redraw
  arm C alone cannot see (root cause of its first failures: the replay was
  lighting the scene with a shadow map one tick ahead of the geometry casting
  it, since `gfx_end_frame` swaps the caster read index before the replay
  runs); **E** (`MDKR_TEST_REPLAY_WALK=recompose`) forces every gameplay
  matrix through the `world x view_projection` recomposition slice 2's camera
  interpolation depends on, camera still unchanged, and requires state and
  pixels untouched — its first run found 5178 of 35119 matrices did not
  recompose back to their own display list, the path camera-only
  interpolation now verifies per matrix before trusting it, falling back to
  the list's own matrix when the decomposition does not hold. These remain
  versioned broken-direction controls; production uses the same retained HLE
  replay machinery only after the complete task/dependency transaction and
  adjacent-state publication succeed.
- `check_camera_snapshot_coverage.py` closes the non-sequential camera-ID
  boundary with real content. A two-player race must capture/interpolate camera
  1 in the lower half, and the production 3P HUD toggle must replace the minimap
  with the time-trial spectator on camera 3 even though `gNumCameras` remains
  three. A real new-game cinematic must also retain and interpolate authored
  cutscene-bank camera 4 after the lifecycle flag is cleared. The split-screen
  viewport crops must contain live intermediate pixels while their frozen
  controls contain none; the WebGPU cinematic must provide its own midpoint
  witness. A semantic VP observer additionally requires every alpha-zero
  override to equal the retained task's captured authored VP byte-for-byte and
  carries each alpha-one target forward to prove it becomes the following
  task's exact alpha-zero VP. A one-bit mutation control must be rejected for
  every observed endpoint. Production latches the complete recipe and canonical
  VP at display-list projection emission; only matrix keys consumed by the real
  HLE walk may receive a replay substitution. The shadow-frame unit preserves
  an unwalked build-only matrix while overriding its walked sibling, and the
  snapshot unit separately proves a camera-bank switch is a cut, never a
  cross-bank blend. Snapshot overflow must remain zero and fixed-ticket/present
  accounting must be exact. The split-screen arms use diagnostic GL for fast,
  synchronous PPM reads; the cutscene arm uses the shipping WebGPU backend.
- `check_hud_render_authority.py` drives the race-start and wrong-way HUD state
  machines through 1P, 2P, and 4P routes. Normal presentation and a schedule
  that skips every odd scene draw must produce byte-identical raw v3 state,
  ordered event, and HUD-action streams. The gate also requires the single
  READY/GO cue, the correct 1P/2P music-start or 4P music-mute branch, and a
  forced wrong-way nag timer that completes. This protects the fixed-tick owner
  from silently drifting back into HUD rendering.
- `check_live_toggle_settings.py` gates the same settings being CHANGED
  mid-run. `Video.FrameLimit`, `Video.MotionSmoothing` and
  `Video.AllowTearing` apply at the host-frame boundary and
  `Camera.Obstruction` at the next level load, driven headlessly by
  `MDKR_TEST_SETTINGS_TOGGLE=Key=value@tick`, which fires from
  `platform_input_pump()` so an edit lands at the same point inside the frame
  the in-game overlay's would. Every arm asserts the change reached the pacer
  and the swapchain (`[SETTINGS-APPLY]`, `[PRESENT-POLICY] event=live-apply`,
  a re-emitted `[PRESENT-MODE]`), that the v3 state, event and input streams
  stay byte-identical to an untoggled baseline, and that nothing reached
  retired replay state. The **pace** arm drives the settings panel's
  Presentation pace quick choice through the same runtime call the radio button
  makes (`Presentation.Pace=original|smooth@tick`) and owns the property no
  other arm can see: the quick choice writes both pacing keys in ONE
  transaction, so four `[SETTINGS-APPLY]` rows must arrive on exactly TWO
  boundary applies. Two sequential setters would re-latch twice, and one frame
  boundary between them would run with half the change applied. The **camera** arm asserts the LEVEL deferral in the
  direction that fails silently: the census must still report `gate=OBSERVE`
  on every tick between the edit and the level load. The **soak** arm flips
  motion smoothing 24 times across a level load — the stale walk-entry hazard
  the deferred apply exists to close — and runs again on the ASan lane, where
  a stale segment base would be a read of freed level memory rather than a
  plausible image.
- `check_presentation_matrix.py` is the fidelity spec's §12.2.2 gate:
  presentation rate must never move the authoritative tick. All stream
  comparisons use the same **v3** authority contract as state-hash and render
  purity. **Arm A** proves the promoted host-frame driver issues one exact
  two-field ticket per scheduled game pass, with no debt or update-rate
  violation on the original schedule. **Arm B** requires both the per-tick
  `[SIMHASH]` state stream, ordered `[EVENTHASH]` gameplay-event stream, and
  `[INPUTHASH]` consumed-pad stream to be byte-identical across
  `MDKR_PRESENT_RATE` unset, `=30`, and `=60` (`=30` has one presentation
  opportunity per tick and no midpoint; `=60` presents twice per tick).
  **Arm C** is the smoothness
  witness: with `MDKR_PRESENT_RATE=60` every interpolated midpoint must differ
  from both retained endpoint frames. Production exposes the completed real
  walk at alpha zero, then re-walks its immutable private task with camera,
  object, deformation, particle, fade, and effect state rebuilt at the exact
  rational alpha. The
  positive control, `MDKR_PRESENT_SMOOTHING=off`, presents at the same rate
  but must repeat the tick's own image, so every intermediate frame is
  byte-identical to its neighbours — without that control arm C could not
  tell interpolation from any other source of frame-to-frame difference. Arm B
  repeats the unset and `=60` authority endpoints on WebGPU and compares state,
  events, consumed input, PCM, fixed-ticket accounting, and live object replay
  with the GL control. Arm C's long backend-pixel sensitivity battery remains
  GL-specific; the renderer parity/backpressure gates own WebGPU pixels. Arm B
  also requires nonzero generation-keyed root and composed-child ownership,
  exact owner-census reconciliation, and nonzero object matrix rebuilds. Arm C
  adds an object-specific pixel control:
  `MDKR_TEST_OBJECT_INTERPOLATION=off` keeps the identical interpolated camera
  but disables object rebuilding, and the intermediate backend frames must
  change against the fully smoothed arm. The same gate requires a bounded
  retained packet to publish without allocation failure, observes nonzero
  billboard-matrix and world-anchor registrations/overrides, and bounds the
  stale-key tail. Build-time pointers are refreshed with the exact bytes seen
  by the real HLE walk. A delayed alpha-zero GL control injects matrix and
  billboard-anchor rewrites and must reproduce the frozen bytes with matching
  semantic hashes and zero live-pointer fallback. A separate real-time WebGPU
  arm must execute production midpoints from the poisoned-live private task,
  reconcile every queue admission outcome, and perform no runtime GPU wait.
  Synthetic WebGPU overload remains a distinct load-shedding assertion: it may
  hold optional images, but it may not slow or mutate authority.

  The packet also retains tick-stamped model/particle vertex batches under
  generation/model/animation/topology/root-stream keys. A read-only structural
  census follows the already-authored alternate-buffer task without backend or
  game callbacks, publishing the true forward ``{T,T+1}`` deformation/effect
  stream while the frozen task-T owner bindings remain unchanged. Arm B
  requires nonzero future publications and zero capture failures. Arm C proves
  deformation, point-trail XYZ/RGBA, primitive-alpha fades, and shield/effect
  recipes produce independent pixel differences against their exact hold
  controls. Ambiguous stable keys, phase gaps, unsafe stale fallbacks, and
  authoritative-stream differences fail closed. Its
  first run diverged from tick 1345 on two real defects, both about phase
  rather than magnitude: the synthetic-pacing COUNTER was being advanced once
  per present instead of once per tick (its monotonic clamp fabricates a tick
  whenever read without the clock moving), and the authoritative tick index
  was bumped before the interpolated presents drained, so the last input pump
  before a tick was applying the next tick's scripted input one tick early.
  `check_arbitrary_presentation_rates.py` extends the same contract beyond the
  old integer-field grid. Over 600 fixed ticks it requires exact rational
  presentation totals for NTSC `30`, `40`, `60`, `90`, `120`, `144`, `165`, `240`, and a
  deterministic 1000 Hz uncapped stand-in, plus PAL `60` at exactly 2.4
  opportunities per 25 Hz tick. `display-margin` is checked against the
  `display` arm that ran on the same monitor in the same session: it must
  resolve to exactly three Hz below whatever refresh that arm resolved to, and
  produce the exact rational count for it. Every arm must keep v3 state, ordered events,
  consumed input, temporary PCM, audio time, and fixed two-field update counts
  byte-identical to its region's original arm; replay/packet failures and
  deformation/effect-key collisions remain zero. A second forced-WebGPU matrix
  runs Enhanced one-field simulation at original policy, `30`, `45`, `60`,
  `120`, and uncapped. It requires identical state/event/input/PCM streams and
  the exact 300-quantum audio schedule in every arm; a one-quantum timing
  perturbation proves the PCM comparator can detect a real mismatch without
  changing gameplay authority.
- `check_presentation_shadows.py` is the focused Ancient Lake visual regression
  for terrain-projected kart decals. It keeps the 3,300-tick v3 state, event,
  and input streams plus every authored endpoint exact, then compares the
  production rigid lateral decal translation against a token-gated historical
  vertex-lerp control. The control must reproduce severe area/residual pulses;
  production must stay below the fixed visual thresholds while preserving the
  receiver-authored Y coordinate and mesh identity.
  `check_presentation_breadth.py` applies that v3 comparison to 17 NTSC/PAL
  content arms spanning every boss/challenge class, car/hovercraft/plane, and
  1P/4P, while also bounding snapshot, replay, matrix-recomposition, retained-
  packet publication, deformation-key collision health, adjacent forward
  capture, rewritten-dependency safety, and the battle arm's point-trail
  registrations. Ordinary production must expose real alpha-zero endpoints,
  perform nonzero immutable midpoint walks, resolve both private-arena and
  copied-external dependencies, and perform zero delayed endpoint redraws.
  `check_presentation_lifecycle.py` covers the teardown half the content matrix
  cannot see: a 2P pause-to-Track-Select path with no following `level_load`, a
  production pause-menu race restart, and the full Adventure loss/post-race/
  lobby/hub return. Original and 60 Hz arms must keep v3 state, ordered events,
  consumed input, and temporary PCM byte-identical while retained walks cross
  every arena retirement/reissue with zero snapshot, packet, freeze, restore,
  or key-collision failure. A one-row hash perturbation proves the comparator's
  broken direction. The dangerous 2P no-`level_load` teardown repeats under the
  linked ASan artifact as its own manifest task, so a retained-pointer lifetime
  regression cannot depend on allocator luck to crash.
- `check_fixed_tick_schedules.py` drives the application with deterministic
  two-, three-, four-, five-, and six-field host opportunities plus periodic
  suspension rebases. Every arm must complete 1,800 exact two-field game
  passes with byte-identical v3 state, ordered gameplay events, consumed input,
  and temporary PCM. It requires real multi-ticket catch-up, intermediate render elision,
  rebase counters, zero ticket/input-queue debt, and zero `updateRate`
  violations. A one-field diagnostic must diverge and be counted; independent
  trace-only perturbations must change exactly one event/input row and zero
  state rows; an independent one-quantum audio timing control must change PCM
  without changing state/events/input. A focused WebGPU arm then runs the real
  two-controller join route through its first race ticks with and without
  five-field debt. Its state, ordered-event, and all-port consumed-input streams
  must remain byte-identical, catch-up/elision must be live, and player 2 must
  contribute real press/release edges. The ROM-free `host_frame_driver` CTest
  independently injects rational
  30/50/60/90/120/144/165/240 Hz, 59.94-like, irregular, burst, rebase, PAL, and
  uncapped-like schedules with exact alpha and long-run drift assertions. The
  `audio_service_clock` proves grouped/split host-time equivalence, catch-up
  ordering, suspension debt retirement, and one audio quantum per two source
  fields even under enhanced one-field game cadence. The shared
  `audio_queue_controller` CTest proves bounded, aligned production across
  simulated 30/60/120/144/240/1000 Hz service schedules, counter wrap and
  stalls. `audio_sink_contract` then opens SDL queue mode with silence and
  requires exact format, active drain, bounded backlog, pause and clear; CI
  uses the dummy driver, while an optional physical-device invocation exercises
  the same path without ROM audio. `audio_volume` independently proves exact
  unity and mute, perceptual gain, bounded ramps, queue-overflow rejection, and
  reconnect crossfade state without SDL or a ROM. The `input_tick_queue`
  CTest covers between-tick tap stretching, independent
  buttons/ports, latest-analog policy, disconnect/reconnect, catch-up ticket
  targeting, target reordering, and overflow-to-neutral behavior.
- `check_shadow_stage_reset.py` proves the static caster cache resets at level
  load in SHIPPING builds: identical terminal `[WORLD-FX] static=` census with
  and without `MDKR_TRACE`, plus a `MDKR_TEST_SHADOW_STAGE_RESET_SKIP=1`
  control whose census must grow (the historical defect was a reset reachable
  only through the diagnostic path, which every other shadow gate accidentally
  enabled). **That accident is now closed at the source**: every gate in the
  shadow family carries a shipping-configuration arm with `MDKR_TRACE` unset —
  `check_world_shadows.py` and `check_shadow_plausibility.py` and
  `check_shadow_visual_ab.py` and `check_presentation_shadows.py` all require
  byte-identical frames against their traced arm, and
  `check_widescreen_shadow.py` (which dumps no frames) requires an identical
  `[SHADOW]` decal mode, heap capacities, overflow-drop and non-decal census.
  A diagnostic variable that reaches the frame buffer now fails the gate
  whatever its mechanism, instead of needing to have been predicted;
- `check_shadow_plausibility.py` asserts the property four separate shadow
  defects have now violated: **every rendered shadow maps to a caster that is
  really there**. Two halves, because the class has two. *Provenance* reads the
  `[WORLD-FX]` census and requires `staleCasters=0` (the tenancy counter — the
  stage cache dedups by raw arena `Triangle` address, so a recycled or
  rewritten-in-place address would otherwise keep casting whatever it held
  first, from wherever that was), `implausible=0`, `allocFails=0`, and a valid
  `[SHADOW-PLAN]` at the exact budget tier the fixture's view count should
  produce. *Attribution* differences a shadow-on/shadow-off frame pair and
  requires the darkened footprint to exist, to stay one-sided, and not to
  swallow the frame — the shape an inverted or pancaked depth axis makes.
  It runs three differently authored worlds at 1P plus the `race_2p_split` and
  `race_4p_split` fixtures, so all three planner tiers (2048px/2 cascades,
  1024px/2, 1024px/1) are covered. Broken direction:
  `MDKR_TEST_SHADOW_BOGUS_CASTER=<world units>` displaces the first admission of
  every static caster, so the depth map holds geometry the object has left —
  that arm must fail, and does (1945 stale admissions at +900 units);
- `check_touch_controls.py` gates the mobile touch layer in real Chromium:
  a desktop arm (overlay hidden, launcher live — the capability-listener path
  must be non-fatal), a persisted-"shown" revival arm (fails on the
  pre-`dc5f83b` shell), a CDP three-finger chord that must reach
  `osContGetReadData P1` with A+R plus a decisive stick and return to exact
  neutral, and a press+release completed between rAF callbacks that must still
  produce one `[INPUTHASH]` press then release through the bounded JS queue; and
- `check_browser_runtime.py` requires the production WebGPU handoff to reach
  complete maps without resource failure or latching while retaining the
  existing cadence, resize, persistence, audio, and fault budgets.

The lighting gate explicitly sets `MDKR_WORLD_SHADOW=0` because its A/B owns
only RL-5 smooth sun and grade. The shadow gate owns the independent production
feature. Keeping these variables isolated prevents one effect from hiding an
inert or overbroad result in the other.

## Restoration/remaster visual gates

The current visual sprint adds seven self-contained gates. Run them directly
during renderer iteration; the complete manifest still remains the release bar.

```bash
python3 tests/check_sprite_layout.py --build build --rom baserom.us.v80.z64
python3 tests/check_intro_shrub_sprite.py --build build --rom baserom.us.v80.z64
python3 tests/check_rdp_interpolation.py --build build --rom baserom.us.v80.z64
python3 tests/check_texture_edge_classification.py --build build --rom baserom.us.v80.z64
python3 tests/check_font_sdf.py --build build --rom baserom.us.v80.z64
python3 tests/check_mip_motion.py --build build --rom baserom.us.v80.z64
python3 tests/check_rl1_vertex_colour_ab.py --build build --rom baserom.us.v80.z64
```

`check_sprite_layout` combines unit boundary cases with a byte-order-aware census
of every supported sprite record. It also pins the multi-run census: of 600
frames across 193 sprites, exactly two exceed five tiles (sprite 108 frame 0, the
intro shrubs; sprite 178 frame 6, a burst frame no swept route loads), so that
pair is the whole blast radius of an append-base regression.
`tests/check_intro_shrub_sprite.py` covers the
other half of the same subsystem at the RSP boundary: a sprite wider than five
tiles is emitted as several `G_VTX_APPEND` runs that share one base, so it
renders the authored intro and scores three fixed regions of one deterministic
frame to prove every tile lands in its own quad rather than over the first one.
`check_rdp_interpolation` compares corrected
and exact-legacy gradient arms on both backends and requires timing identity.
`check_texture_edge_classification` does the same for the cutout-versus-blend
render-mode decision: DKR forked `G_RM_AA_ZB_XLU_LINE_MOD` to clear
`ALPHA_CVG_SEL` while keeping `CVG_X_ALPHA`, and reading only `CVG_X_ALPHA` turned
that authored alpha blend into a hard 0.19 alpha test. The gate pins the
character-select frame where the difference lands and bounds it from both sides,
so an inert change and an over-broad one both fail.
`check_font_sdf` runs mode/backend/control arms: Pure and Restored must be
byte-identical, while Remastered must upload derived fields and change only
known text regions. `check_mip_motion` measures temporal second-difference energy
over a fixed 24-frame moving Everfrost Peak window with anisotropy pinned to one.
`check_rl1_vertex_colour_ab` records the three RL-1 arms on Ancient Lake and
Fire Mountain and enforces the measured decision to retain baked colour as the
ambient base. Each gate has a failure direction; none is a screenshot-only
approval.

### Character-select dancer motion — `tests/check_charselect_motion.py`

```bash
MDKR_AUDIO=0 python3 tests/check_charselect_motion.py --build build --rom baserom.us.v80.z64 -v
```

Productizes the ad-hoc analysis that disproved a "dancers static" report
(byte-identical captures across 86 commits, whole-screen motion RMS ~14.1,
dominant period ~19-20 frames via `tools/anim_period.py`) into a permanent
gate — until this existed, a real regression in `obj_loop_char_select()` or
`music_animation_fraction()` would have shipped undetected. It reuses
`check_mip_motion.py`'s six-window ensemble shape (`WINDOW_FRAMES=24`,
`WINDOW_COUNT=6`, `CAPTURE_COUNT=144`): mean whole-screen motion RMS over the
144-frame character-select capture must clear a floor and at least 5/6
windows must individually clear a per-window floor, with a loosely-banded
periodicity assertion (peak autocorrelation lag in [14, 26] frames, r >= 0.3)
alongside it. That band also rejects the historical 8x-too-fast oscillation.
Whole-screen RMS was chosen over the legacy cropped dancer analysis because it
reproduces the original ad-hoc numbers directly and was measured to still catch
a freeze localized to just the character models (butterflies alone left
animating drops per-window motion from a real 5.76-6.90 to 4.24-4.65, cleanly
below the chosen floor). The older rate-only script was retired when its fixed
capture window no longer reached the settled character-select screen; this
gate subsumes both of its useful assertions without weakening either one.

No env-gated animation-freeze hook exists in the engine, and the plan
explicitly prefers an analyzer-level control over adding one for this gate.
The required broken-direction controls are built without any engine change.
The frozen arm replaces every captured grid with frame 0; measured motion
collapses to exactly 0.0 in every window and must fail both motion floors. The
rate arm loops five phases sampled across one healthy cycle, recreating the
historical roughly five-frame oscillation while preserving visible movement;
it must fail the [14, 26]-frame period band. The check's own PASS depends on
both controls being rejected by the same scorer used for the real capture.

### Output-resolution HUD/text — `tests/check_native_ui_resolution.py`

```bash
python3 tests/check_native_ui_resolution.py --build build \
  --rom baserom.us.v80.z64 -v
```

This gate runs GL and WebGPU in Remastered mode at 2x scene resolution, once
with the production output-resolution UI path and once with
`MDKR_UI_NATIVE_RES=0`. It requires identical normalized gameplay state, more
than 500 active output-pass frames, zero world-after-overlay draws, zero pass
startup failures, and no output pass in the disabled control. At frame 3000 it
requires every changed pixel to remain inside the top HUD/minimap regions,
requires center and bottom-left world crops to be byte-identical, and measures
at least 1.15x top-HUD edge energy. Current GL/WebGPU values are 1.270x/1.296x.

The multiplayer gates extend the same ordering contract to 2P, 3P, and 4P:
every required viewport/quadrant must remain live while output UI is active,
with zero late-world draws and begin failures. Each run uses an isolated
`MDKR_SAVE_DIR`; no multiplayer check reads or writes the player's save.

`check_runtime_safety.py` is the production-seam companion to the
`runtime_contracts` CTest. The unit exhausts audio group/sound/vehicle row
domains, finite X/Z normalization boundaries, save-derived model selection,
16-slot course flags, trophy worlds, Pak extensions, exact texture/CI capacity,
custom-FX asset spans, MIPS `cvt.w.d`/low-half behavior, and signed 16.16 audio
reconstruction. Each boundary has valid lower/upper cases and invalid cases on
both sides. The source census locks owner-last level teardown, both racer
recovery callers, initialized plane/item state, exclusive sound bounds, and
both special-vehicle mappings. It deletes every required fragment in memory and
must reject each mutation, so its source assertions have executable failure
directions.

## Open loop vs closed loop — read this before adding or editing a fixture

An **open-loop** fixture replays fixed button presses at fixed frames and asserts
against the one trajectory those presses happened to produce. That is fine for a
menu, where nothing between the input and the assertion has any state to
accumulate. It is a trap for anything with a racer in it: the line is chaotic with
respect to *any* change in the simulation, so a physics or RNG change re-strands
the kart and the check fails for a reason unrelated to what it tests. Three
separate recalibrations had already been forced that way before the "closedloop"
wave, and the ROM-faithful RNG seed / arctan table / table-based trig broke four
fixtures at once.

A **closed-loop** fixture steers toward something each frame and asserts on the
*outcome*. Two mechanisms exist, both no-ops unless set:

- `MDKR_AUTOPILOT=1` — hand the human racer to `racer_AI_pathing_inputs()`, DKR's
  own driver. Use this for anything on a race track.
- `MDKR_DRIVE_ROUTE=...` — steer at named level objects and waypoints
  (`platform/mdkr_adventure.c`). Use this in the Adventure hub and world lobbies,
  where the AI-node graph is seven disconnected components and the autopilot can
  only lap the beach.

Outcomes to reach rather than replay, when a check needs a specific event:
`MDKR_LOAD_TRACK`, `MDKR_FORCE_LAPS`, `MDKR_BOSS_WIN`, `MDKR_BOSS_PRECLEARED`,
`MDKR_ADVENTURE_WIN`, `MDKR_COLLTEX_FORCE`. Writing the one verdict field after
the natural prerequisite beats perturbing the physics until the branch happens
to be taken — `MDKR_BOSS_WIN` replaced `MDKR_BOSS_SLOW` for exactly that reason
(see the grid-mask section below).

### Every fixture, classified

| script | frames | driven by | loop |
|---|---|---|---|
| `nav_to_options` | 1700 | `check_nav_fixtures.py` → `menu_init: menuId=12` | open, **by design** — menus only |
| `nav_to_audio_options` | 1900 | `check_nav_fixtures.py` → `menuId=13` | open, by design |
| `nav_to_save_options` | 1950 | `check_nav_fixtures.py` → `menuId=14` | open, by design |
| `nav_to_magic_codes` | 2050 | `check_nav_fixtures.py`, `check_array_bounds_sweep.py` → `menuId=10`; the nav gate also submits valid `ARNOLD` and invalid `ARNOLE` through the onscreen keyboard | open, by design |
| `nav_to_character_select` | 1600 | `check_nav_fixtures.py`, `check_charselect_motion.py`, `check_determinism.py`, `check_array_bounds_sweep.py` → `menuId=3` | open, by design |
| `nav_to_game_select` | 2000 | `check_nav_fixtures.py` → `menuId=19` | open, by design |
| `nav_to_file_select_adventure` | 2100 | `check_nav_fixtures.py`, `check_array_bounds_sweep.py` → `menuId=6` | open, by design |
| `nav_to_track_select` | 2300 | `check_nav_fixtures.py`, `check_array_bounds_sweep.py` → `menuId=15` | open, by design |
| `nav_charselect_after_track_select` | 2901 | `check_framed_world_views.py` (aperture-leak arm) → character select reached *through* Track Select | open, by design — menus only |
| `nav_charselect_late` | 2901 | `check_framed_world_views.py` (aperture-leak arm) → the same screen, same age, no Track Select visit; the pair's recovered horizontal lens ratio must be 1.0 | open, by design |
| `nav_to_time_trial_race` | 2900 / 3500 / 4000 / 7500 | `check_nav_fixtures.py` → `level_load levelId=5`; **`check_widescreen_shadow.py`**; **`check_race_drive.py`**; **`check_shadow_plausibility.py`** (+ `MDKR_LOAD_TRACK` to retarget the 1P worlds) | open to the grid, then **closed** (`MDKR_AUTOPILOT`) |
| `race_drive_long` | 3900–4300 | `check_texture_lineswap.py`, `check_rom_revision.py`, `check_determinism.py` | open, **by design** — these three compare *pixels between two arms of the same route*, so the route only has to be identical to itself |
| `race_drive_time_trial` | 1500 | `check_determinism.py` | open, by design — same reason |
| `race_full_3lap` | 12000 | `check_array_bounds_sweep.py` | **closed** (`MDKR_AUTOPILOT`) |
| `race_full_3lap_tt` | 6500–13000 | `check_race_finish_time.py`, `check_collision_gridmask.py`, `check_boss_win_verdict.py`, `check_collision_untextured.py`, `check_track_sweep.py`, `check_vehicle_sweep.py`, `check_array_bounds_sweep.py`, `check_collision_headroom.py` | **closed** (`MDKR_AUTOPILOT`, + `MDKR_LOAD_TRACK` to retarget) |
| `race_2p_split` | 3500 / 9600 | `check_race_2p_split.py`, `check_shadow_plausibility.py` (2-view shadow budget tier) | **closed** — autopilot drives *both* humans, through results→track-select |
| `race_3p_split` / `race_4p_split` | 3500 / 9600 | `check_race_multiplayer.py`; `race_4p_split` also `check_shadow_plausibility.py` (4-view tier) | **closed** — every human racer, all four quadrants, 3P minimap, and results→track-select |
| `adventure_hub_drive` | 2300 / 6500 / 12000 | `check_filename_entry.py`, `check_widescreen_proportions.py`, `check_adventure_hub.py`, `check_save_failsafe.py`, `check_texture_lineswap.py` | **closed** (`MDKR_DRIVE_ROUTE` island tour; filename check stops at the character grid) |
| `adventure_resume_race` / `adventure_two_resume_race` | 5200 | `check_adventure_two.py` | **closed** — canonical unlock/save identity, all 20 mirrored racing lines, viewport, stereo, minimap, steering, and pixel reflection control |
| `adventure_race_loop` | 7000 / 17000 | `check_door_glyphs.py`, `check_adventure_race_loop.py` | **closed** (`MDKR_DRIVE_ROUTE` + `MDKR_AUTOPILOT`) |

A run counts as a failure on a non-zero exit, a `[CRASH]` backtrace, a `[FATAL]`
abort, or a missing assert line.

The build itself is also a call-ABI gate. Clang/Emscripten rejects every implicit
function declaration, and wasm-ld warnings are fatal. This specifically prevents
the historical `f32 log(f32)` versus libc `double log(double)` collision and the
implicit-`int` bounds-probe calls from producing a runnable-looking wasm module.
A web clean build may emit only Binaryen's
`warning: no output file specified, not emitting output`, caused by the required
`--emit-symbol-map` print operation.

The nine `nav_*` routes are asserted by `tests/check_nav_fixtures.py`; before the
"closedloop" wave their expected terminal state lived only in this table and five
of them had no automated consumer at all.

## In-game video options — `tests/check_video_options.py`

```bash
python3 tests/check_video_options.py --build build
MDKR_RENDERER=gl python3 tests/check_video_options.py --build build
```

The focused fixture enters native-port menu 29, changes every player-facing
video/accessibility control, returns to Options, and runs four isolated arms:
successful atomic save plus fresh-process reload; environment-owned lock with no
override baking; an unwritable temporary path where every video transaction
fails closed; and malformed internal browser-launch arguments that return 2
without changing an existing file. It also requires the live renderer to move
to 3× supersampling. Every arm uses a private working directory and
`MDKR_SAVE_DIR`; it cannot read or delete a player's real EEPROM or config.

The full `check_browser_runtime.py` injects the same fixture into the actual wasm
build, persists through IDBFS, creates a fresh document, and requires the
16:10/FOV-50/3× state to return. Save-only wipe and ROM forget controls must
retain `/save/mdkr64.ini`.

## Original Audio Options persistence — `tests/check_audio_options_persistence.py`

```bash
python3 tests/check_audio_options_persistence.py \
  --build build --rom baserom.us.v80.z64
```

This enters the retail Audio Options screen, changes both original sliders, and
proves the native settings file commits before the menu transition. A second,
isolated arm uses a missing config parent to force the real atomic writer to
fail. The menu must stay open, render the save warning, accept an A-button
retry, and leave only after B explicitly chooses session-only levels. Neither
arm can touch the player's normal settings or save directory.

## Native renderer routing and fail-closed selection — `tests/check_renderer_backends.py`

```bash
python3 tests/check_renderer_backends.py --build build-rel -v
```

The GL and WebGPU arms independently replay `nav_to_time_trial_race.txt` through
frame 3200 in isolated save directories. They must identify and initialize the
requested backend, emit identical menu/level transition traces, enter playable
Ancient Lake, and dump the same frame set. On the measured baseline, six
non-flat scene pairs have per-frame RGB mean absolute difference at most 1.694
(mean 0.872); the gate allows 4.0 per frame and 2.0 on average. It permits one
one-sided sample at a transition because GL reads the current back buffer while
WebGPU's asynchronous readback can retain the last presented scene.

Those sparse whole-frame metrics are a coarse backend-plumbing check, not GL
visual qualification: dense intro capture exposed localized GL corruption they
did not reject. A separate no-selector arm captures frames 640 and 652 at the
known bad interval and requires them to resolve to WebGPU and match an explicit
WebGPU run byte for byte. This protects the release default while GL remains a
diagnostic backend.

The startup negative arm forces WebGPU window creation to fail and requires the
engine's deliberate `EXIT_FAILURE` path with no crash marker and no GL
initialization. GL remains testable only when the run explicitly sets
`MDKR_RENDERER=gl`; there is no automatic renderer switch.

## Native GPU production and surface suspension

```bash
python3 tests/check_gpu_backpressure.py \
  --build build-rel --rom baserom.us.v80.z64
python3 tests/check_pacing_quality.py \
  --build build-rel --rom baserom.us.v80.z64
python3 tests/check_app_adopted_pacing.py \
  --build build-rel --rom baserom.us.v80.z64
python3 tests/check_overlay_pause.py \
  --build build-rel --rom baserom.us.v80.z64
python3 tests/check_overlay_pause_cutscene.py \
  --build build-rel --rom baserom.us.v80.z64
python3 tests/check_surface_suspension.py \
  --build build-rel --rom baserom.us.v80.z64
```

`check_gpu_backpressure.py` runs real native GL and WebGPU at the synthetic
uncapped opportunity rate with visible swapchains. Production WebGPU submits
only authored tick images, nonblocking-polls completion callbacks, and must
record zero runtime wait calls/nanoseconds; an orderly shutdown may drain any
remaining work. GL interval-0 fences keep their two-frame ceiling and must
exercise the fence-wait path. Every submission retires by shutdown with no
failure or abandoned completion. The gate also requires effective
immediate/interval-0 diagnostics, zero leaked child resources, and a measured
achieved submission rate.
An additional no-selector arm requires the production native default to resolve
to WebGPU, preventing a backend-policy edit from bypassing the qualified path.

`check_pacing_quality.py` measures how EVENLY images arrive rather than how many
of them do. The engine publishes three distributions at shutdown under
`MDKR_PRESENT_PERF=1` — wall interval between present opportunities, wall
interval between frames that actually reached the screen, and interpolation
phase advanced between displayed frames — as fixed-bucket
`[PRESENTPERF-HIST]` histograms carrying p50/p95/p99/max/variance, alongside a
`[PRESENTPERF-LATENCY]` present-queue-depth estimate (mean/max GPU frames in
flight at present, times the refresh period) that quantifies the cost of a FIFO
present mode.

Synthetic arms cover every presentation policy — including the battery-friendly
`40` cap and `display-margin` — crossed with both smoothing
settings and assert only structural identities: the census must count the same
presents the scheduler did, every present after the first must contribute
exactly one interval sample and every displayed frame exactly one phase sample,
the interpolation phase must never run backwards or fail to advance, and with
smoothing off no displayed frame may advance the phase by less than a whole tick
(there are no interpolated images to display). Synthetic pacing does not sleep,
so those arms deliberately assert no wall-clock number.

The realtime arm is a genuinely paced, compositor-visible run
(`MDKR_PACE_REALTIME=1`) on the display policy with smoothing. It gates
no-tearing-mode and `underruns=0`, and REPORTS the displayed-interval and
phase-variance tails as the labelled quality baseline. Those are reported rather
than gated because a threshold chosen before a baseline exists is a guess.

Two session conditions make those numbers meaningless without anything being
wrong in the code, and both are detected and named explicitly rather than
reported as a generic failure: a session with no window server refuses every
drawable, and a session that accepts presents without vsync-blocking them lets
the display policy free-run (it installs no software limiter and relies on FIFO
to pace it). In both cases the no-tearing and no-underrun assertions still run —
neither can be excused by the environment — and only the baseline is withheld.
The arm is genuinely bimodal on a developer machine: the same command can
vsync-throttle on one run and free-run on the next, so read the printed baseline
or its absence rather than assuming one was produced. `--skip-realtime` runs the
synthetic arms alone.

`underruns` in `[AUDIO-SINK]` counts the sink observed fully drained after it
had been fed at least once, which is the audible starvation a gate can require
to be zero; it excludes the boot prime, where the queue is legitimately empty
because nothing has been enqueued yet. `floorbreaches` is the softer
"would not have survived another refill gap this long" signal, reported for
trend. Headless runs open no audio device, so the deterministic coverage for
both counters is `tests/test_audio_queue_controller.c`.
`check_app_adopted_pacing.py` enters through the real ImGui launcher and makes
the engine adopt the launcher's context/device. Numeric 240 Hz and Uncapped
must complete on the WebGPU default and explicit GL path with fully drained GPU
work and no completion failures. This is the regression for the null GL entry-point crash
which affected both of those launcher choices.

`check_overlay_pause.py` enters through that same production app-owned WebGPU
window, navigates the ordinary title-to-Time-Trial route, and opens the ImGui
overlay only after the kart and race clock are live. The complete v3
authoritative-state hash plus position, clock, checkpoint, and lap telemetry
must remain exactly fixed for the whole open interval and advance again after
close. Its PCM arm also requires the independent overlay pause mix, reduced but
live music, suppressed race effects, bounded sample edges, and restoration of
the latest underlying authored mix on resume. This prevents an input-capturing
overlay from masquerading as a pause menu while gameplay or its dominant
feedback continues behind it. Its handoff arm opens the overlay through a real
dispatched key event and closes it from the render callback, which is the shape
the on-screen Resume button has under a mouse click, Enter, or gamepad nav.
Capture must be claimed on the open and given back on the close: issue #20 was a
suppression latch that only ever moved when the capture state changed across a
dispatched event, so Resume left the pad muted for the rest of the session while
F1, Escape and B/Circle were fine. `check_overlay_input_handoff.py` is the
ROM-free companion that pins the same contract in source — one shared release
routine, reconciled against the overlay's live answer both before and across
dispatch — because the defect was the absence of a call, which no run of the
paths that still worked could reveal.

`check_overlay_pause_cutscene.py` covers the other half of that pause: a scene
running under a MENU rather than in a race. `check_overlay_pause.py` proves the
simulation freezes; this proves the PICTURE survives the freeze. It opens the
overlay during the title screen's scripted camera and during the new-game intro
animation, and requires every frame presented while it is open to keep roughly
the boundary frame's colour count, stay within a few mean levels of that frame,
and start moving again after the close. Both arms went to a single flat colour —
each level's own background fill — and stayed there for the rest of the session:
the pause hands the game a zero update rate, `update_menu_scene` (unlike
`mode_game`) keeps running `obj_update` at it because that is what re-arms the
cutscene camera each tick, and the animation-path followers in `game/src/objects.c`
divided by that rate. One paused frame produced inf/NaN, stored it in the
spline's own interpolation parameter, and every camera and object matrix built
from it was NaN from then on. The colour-count assertion alone would pass on a
scene that kept animating behind the overlay, so the hold assertion runs beside
it. Frame capture from an app-shell run comes from `MDKR_APP_AUTOPLAY_DUMP_FRAMES`,
which forwards `--dump-frames` through the synthesized engine argv.

`check_surface_suspension.py` compares equal-tick control and minimized arms on
both native backends. The minimized interval must stop real display-list walks
and replay, perform no more than two presentation boundaries, and rebase once
on resume. All 30 v3 state, ordered-event and consumed-input rows plus the
temporary PCM digest must remain byte-identical to the visible control.

## WebGPU lifecycle and recovery — `tests/check_webgpu_recovery.py`

```bash
python3 tests/check_webgpu_recovery.py \
  --build build --rom baserom.us.v80.z64
```

This integration matrix injects 76 failures across instance, surface, adapter,
device, queue, and configure bring-up; every surface-status repair class;
featureless depth clipping; both native device-loss outcomes; and all
Pure/Remastered scene-target, shader, texture, draw, post, resolve, mip, capture,
and readback constructors reached by DKR. Required resources may rebuild the
WebGPU device once, then must terminate cleanly without switching renderers;
mip and diagnostic failures must stay local. Every
case uses a private save directory and rejects driver validation errors,
assertions, aborts, and uncaptured device errors. The ROM-free
`webgpu_lifecycle` and `webgpu_artifacts` CTests exhaust the pure
transition/usage policy and explicit OS/CPU/ABI artifact selector.

`tests/check_webgpu_fault_matrix.py` is the ROM/GPU-free companion. It requires
every public fault name to be wired to a real backend site and classifies every
site by product route, dialect, and failure policy. It deliberately identifies
the inherited MGB64 minimap, modern-mesh, and prewarm paths — and GE007's
uncalled mid-draw readback probes — as dormant in DKR; those names are not
counted as runtime coverage.

## Real browser runtime — `tests/check_browser_runtime.py`

```bash
python3 tests/check_browser_runtime.py \
  --engine-dir build-web --shell-dir dist/web \
  --rom baserom.us.v80.z64 --camera-obstruction modern
```

The registered task passes `--camera-obstruction modern`; omitting the flag
skips that arm entirely. It sets the first browser document's camera policy and
`MDKR_CAMERA_TRACE=1`, then requires at least `--frames` minus 100 telemetry
rows, the requested gate on every one of them, zero duplicate solves,
projection mismatches, penetrated or invalid or degraded resolved lenses and
hidden targets, nonzero applied corrections on the Modern arm, and one dynamic
publication row per camera row with zero missing caches, missing identities,
uncategorized hits, invalid transforms, and capacity failures.

This is a runtime gate, not a wasm-file inspection. Using only the Python standard
library and the Chrome DevTools Protocol, it serves the committed shell with the
freshly linked JS/wasm, launches an isolated Chromium profile, and chooses the
external ROM through the real file input. The ROM is never copied into the served
tree.

The first document must run 3,600 rAF-paced frames through the title and menus
into Ancient Lake. Five screenshots must be non-flat and changing; original-mode
cadence must remain 24–36 fps with at least 80% `updateRate == 2` and no
sub-two-field updates; the racer must advance;
the AudioWorklet must consume PCM; its first active block must report nonzero
fixed-mode RAW16 loads/bytes; and the C renderer must observe 1260×540 DPR-2,
640×480 DPR-1, then 1260×540 DPR-2 backing stores after live CSS resizes. All
music, jingle, and SFX event queues are measured, may not drop a post, and may not
consume more than half their budgets. The production run forces an
attachment-only surface to exercise the render-blit present fallback, drops one
overlay pass, and requires shader/attribute/varying telemetry to remain within
the granted device limits with zero table overflow or pipeline failure.

After flushing IDBFS, the same profile reloads without another picker action. The
exact 512-byte EEPROM hash and 12 MiB ROM must exist before `main()`. Erasing
progress must retain the ROM; forgetting the ROM must empty both stores. CDP and
the local server audit every request: only local GET/HEAD requests with no body
are allowed, and the ROM filename may not enter any URL.

The gate self-validates the important failure directions: flat-black visual data,
a synthetic ROM POST, a mismatched EEPROM hash, and a forced one-entry SFX queue
must all be rejected or detected. The test bridge is injected in memory before
page JavaScript; it is inert for ordinary visitors and accepts no fixture from the
URL.

`check_browser_presentation_rates.py` is the independent browser pacing gate.
It runs the actual wasm/WebGPU engine in isolated Chromium profiles with exact
rational rAF timestamps while still yielding through the real browser event
loop. Original, display-at-144, capped-60-on-144, irregular display, and a
shared native `uncapped` config must all complete 60 fixed authored ticks with
byte-identical v3 state, ordered events, consumed input, and temporary PCM.
Display 144 must issue exactly 288 presents; capped 60 exactly 120. The browser
launcher must omit unbounded presentation and disclose the rAF ceiling, while
the shared `uncapped` value must report requested `uncapped`, effective
`display`/FIFO, and its `raf-ceiling` reason.

A final stored-ROM reload forces engine-level WebGPU adapter failure. The canvas
must be hidden, the launcher and independent recovery controls restored, and an
actionable graphics error remain stable instead of a black frame.

The third arm sets `MDKR_TEST_WEBGPU_WINDOW_FAIL=1`. It must log the injected
window failure, change the cached selection to GL before `gfx_init`, and produce
a non-flat GL menu frame. The comparator's positive control pairs the late race
scene with black; measured MAD is 92.9, far beyond the 4.0 threshold. Debug and
Release both pass.

## Browser save custody — `tests/check_browser_save_ui.py`

```bash
python3 tests/check_browser_save_ui.py \
  --engine-dir build-web --shell-dir dist/web
```

This is the fast ROM-free companion to the full browser runtime. It removes
`navigator.gpu` before page code runs, never selects a ROM, and rejects any load
of `mdkr64_web.js`/`.wasm`. The launcher must still expose enabled save controls
and instantiate only the small shared-codec save-tools module.

The check drives raw and portable export, safe untrusted metadata preview, eight
malformed/oversized inputs, all six IDBFS transaction fault points, byte-exact
rollback, corrupt-original forensic export, corrupt-block recovery, block merge,
one-field edit containment, destructive cancellation, complete wipe, real
file-input import, and reload persistence. Chromium's accessibility tree,
dialog focus/return, keyboard drop target, live status regions, and every local
network request are asserted. The publish workflow runs this gate independently
of the long game/WebGPU check.

## New-save filename C-string check — `tests/check_filename_entry.py`

```bash
python3 tests/check_filename_entry.py --build build-asan -v
```

This is the shortest first-session route through FILE SELECT and the new-save
character grid. It runs in a fresh temporary working directory, so it neither
depends on nor mutates `save/eeprom.bin`. The check requires `menuId=6`, a zero
exit, and no sanitizer or fatal text through frame 2300.

The historical one-byte `gCurFilenameCharBeingDrawn` binary is retained only as
an external positive-control artifact during validation. Running the same harness
with `--expect-asan` requires a non-zero exit, an AddressSanitizer report, the
symbol name, and the `font.c` scanner site; it aborts at frame 2052. The fixed
ASan, Debug, and Release builds all pass. This both-direction requirement prevents
the route from going green merely because it stopped reaching filename entry.

## Native representation boundaries — `tests/check_native_layout.py`

```bash
python3 tests/check_native_layout.py \
  --rom baserom.us.v80.z64 --asan-build build-asan -v
```

This is the dedicated gate for three host-ABI defects: heterogeneous object tails
that inherited N64 alignment/pointer sizes, variably aligned serialized object
records exposed as naturally aligned native unions, and small render records cast
to larger scene objects for prefix access.

It first builds a halt-on-error alignment-UBSan target and the ROM-free layout
unit. Linked sanitizer handlers are checked explicitly. Three exact historical
controls must then fail in their known directions: the MEM-02 tail emits four
alignment reports, the MEM-03 HUD-record view emits two, and the MEM-04 bare
transform aborts under ASan when the fake object prefix reads its animation
frame. The fixed unit and a source contract that rejects native fake renderer
casts must remain clean.

The full arm drives all nine menus, all 20 tracks, every one of the 47 legal
track/vehicle combinations, Adventure hub and hub→race→hub routes, boss/object
collision, 21:9 two-player, and `check_widescreen_proportions.py` under that
alignment build. The last route pixel-measures the HUD and world balloons across
4:3, 16:10, 16:9, 21:9, forced 4:3, and changed FOV, and still requires exact legacy stretching to
be rejected. Use `--quick` only during iteration; the registered release task is
the complete arm.

## Input-script format

```
<frame> <TOKEN>[+<TOKEN>...] [holdFrames] [P1|P2|P3|P4]
```
Buttons `A B Z START L R CUP CDOWN CLEFT CRIGHT`; directional tokens
`UP DOWN LEFT RIGHT` drive the **analog stick** as well as the matching D-pad bit
(DKR menus read the stick). `holdFrames` defaults to 3. The trailing field is the
**controller port**, 1-based, default `P1` — so every pre-existing script parses
unchanged. A malformed port field is a hard error: the binary prints
`[input-script] bad port field ...` and exits **1** rather than run a truncated
route (a partially-loaded script would otherwise exit 0 and "pass" while covering
nothing).

## In-race drive check — `tests/check_race_drive.py` (RUN THIS AFTER ANY GAMEPLAY CHANGE)

Everything above passes as long as the process exits 0 with no `[CRASH]`/`[FATAL]`.
That is not enough for gameplay: when the player racer ends up somewhere it should
never be, the segment lookup stops matching, `traverse_segments_bsp_tree()` returns no
visible segments, and the screen becomes a **flat fog-brown field with only the HUD and
minimap on it** — while the race clock keeps counting and the run still exits 0. A
whole class of asset/physics bugs is invisible to a survival-only fixture (that is how
the `ASSET_MISC_8` denormal-divisor bug survived the full matrix; see
`docs/OPEN_ITEMS.md`).

```
python3 tests/check_race_drive.py -v        # ~40 s, muted + headless
```

It drives `nav_to_time_trial_race.txt` with **`MDKR_AUTOPILOT=1`** for 7500 frames —
closed loop, since the "closedloop" wave — and asserts three independent things:

1. **Position sanity** — no `inf`/`nan`, `y` inside a generous track band, and no
   teleport. A teleport is identified by its *shape*: the per-frame change in step
   length (`MAX_ACCEL = 40`, measured 5.8 on a healthy run) plus a generous absolute
   ceiling (`MAX_STEP = 150`). The old absolute 40 units/frame cap does not survive a
   route that actually takes a zip pad in an eight-racer race — measured 44.9
   units/frame sustained over 55 frames, on grass, all four wheels down, ramping
   smoothly, i.e. `BOOST_LARGE` doing what a boost does.
2. **Forward progress** — the traced `cp=` (`courseCheckpoint`, checkpoints crossed
   since the race start, never reset per lap) must reach ≥ 30, `lap=` must reach ≥ 2,
   and the kart must never average < 1.5 units/frame over 240 frames. Position alone
   cannot tell "driving the track" from "ping-ponging between a respawn point and a
   wall".
3. **The scene still draws** — sampled frames (via `MDKR_DUMP_EVERY`) must contain a
   real rendered scene, measured as distinct 5-bit-quantized colours + luma sigma over
   a centre crop that excludes the HUD band.

Healthy reference: 4381 in-race frames, final `cp=48 lap=2`, max step 44.9 at frame
3909, max step-to-step acceleration 5.8, slowest 240-frame mean speed 10.35, sampled
frames 1115–3356 distinct colours / sigma 21–46 (the sampled set shifts a little
run to run because the frame-dump stride and the AI line are independent).

⚠️ **Why it is no longer an input replay.** It used to run `race_drive_long.txt` —
throttle held, stick LEFT for 25 frames in every 120 — and assert against that one
line. That line is chaotic with respect to any change in the simulation: P3.5's
`ParticleBehaviour` stride fix moved it once (forcing `MIN_FINAL_CP` 20 → 15), and the
ROM-faithful RNG seed / arctan table / table-based trig moved it again, to cp 14, lap 0
and a 240-frame window averaging 0.11 units/frame with the kart nosed into a wall.
Nothing about the port was broken either time. `racer_AI_pathing_inputs()` steers
toward the next AI node every frame, so it corrects after a perturbation instead of
accumulating it. The autopilot overrides the stick **after** update_player_racer's
normal input dispatch, so every line of physics downstream — including
`func_80050A28()`, where the `ASSET_MISC_8` divisor bug fired — runs unchanged.

**Verified in the broken direction.** With `ASSET_MISC_8` removed from
`dkr_misc_normalize_tables()`'s word-array list, i.e. the denormal-divisor bug
deliberately reintroduced, this check fails on six assertions at once: exit code −6,
`[FATAL] update_player_racer: non-finite position (nan, inf, nan) … lateral=-inf`,
14 in-race frames of 1000, checkpoint 0 of 30, lap 0 of 2, one dumped frame of four.

`race_drive_long.txt` is still in the tree and still open-loop, because
`check_texture_lineswap.py`, `check_rom_revision.py` and `check_determinism.py` use it
to compare **pixels between two arms of the same route** — for that, the route only has
to be identical to itself.

⚠️ **The 44.9 figure quoted above is historical and no longer reproduces.** This route
crossed a zip pad in 2026-07; it does not any more (measured: zero frames of surface
`SURFACE_ZIP_PAD` across the whole race, and this check now reports max step 14.3).
The AI line moved when the wave "closedloop" corrections landed — the same reason this
file keeps warning against calibrating on a line. `MAX_STEP`/`MAX_ACCEL` are unaffected,
because they were deliberately set so the check does not depend on whether a pad is
crossed. The boost magnitude itself is now measured by `check_boost_magnitude.py`
below, which does not rely on any route reaching a pad.

## Zip-pad boost magnitude — `tests/check_boost_magnitude.py` (RUN THIS AFTER ANY CHANGE TO RACER VELOCITY, `boostTimer`/`boostType`, OR `normalise_time()`)

```bash
python3 tests/check_boost_magnitude.py -v      # ~3 min, muted + headless, four runs
```

Closes the long-standing register question "is the zip-pad boost the magnitude DKR
authored, or a port defect?" (`docs/open-items/gameplay.md`, wave "zippad"). Answer:
**authored.**

It arms the boost rather than driving over a pad, for the reason in the ⚠️ above — no
committed route can be relied on to keep crossing one particular pad, so a check
calibrated on one would be measuring the AI, not the boost.
`MDKR_ZIPPAD_BOOST=<frame>[:<ticks>]` (`objects.c mdkr_zippad_boost_hook`, no-op unless
set) arms **player one only, once**, in exactly the state `racer.c:5727` arms it in for
`SURFACE_ZIP_PAD` on a car: `boostTimer = normalise_time(ticks)` (default 45, the
authored constant), `boostType = BOOST_LARGE`. Everything downstream is untouched
decomp code, so what is measured is the shipping boost with a deterministic trigger.
`MDKR_BOOST_TRACE=1` emits the per-update `[BOOST]` row the check parses (boost state,
`racer->velocity`, world position, wheel surface).

Four runs, all sequential:

| arm | fixture | `MDKR_ZIPPAD_BOOST` | cruise | boost frames | peak &#124;velocity&#124; |
|---|---|---|---|---|---|
| 8-racer Tracks | `nav_to_time_trial_race.txt` | `4000` | 12.27 | 45 | **22.357** |
| solo Time Trial | `race_full_3lap_tt.txt` | `4000` | 12.67 | 45 | **22.336** |
| control | `nav_to_time_trial_race.txt` | `4000:15` | 12.27 | 15 | 20.880 |
| control | `nav_to_time_trial_race.txt` | `4000:120` | 12.23 | 120 | 22.358 |

The two baseline arms differ by **0.021 velocity units — 0.09%** with the boost armed
identically, which is the assertion that answers the register: the mechanism has no
racer-count coupling, and `normalise_time()` has no framerate term either (it is a PAL
5/6 rescale of the constant and nothing else).

**The trace is asserted on `|racer->velocity|`, not on the position step.** The step is
what the other motion checks use, but it carries cornering and gradient — i.e. the
racing line — into the measurement: normalised by cruise, the two fixtures' ramps
differ by up to 0.17 where the tolerance would have to be 0.25. The velocity trace does
not; the two agree to 0.05 across the plateau. Asserted: 45 boost frames exactly, a
cruising entry state, a monotone ramp, a plateau inside `[21.8, 22.7]` over frames
+25..+34, peak inside `[21.5, 23.0]`, and decay back under 0.75 of the plateau by
frames +55..+88.

**Verified in the broken direction, both ways, on every run.** The `<ticks>` field is
the perturbed boost constant, and both controls must fail or the check reports
`POSITIVE CONTROL BROKEN`:

* `:15` trips **four** assertions — duration, peak, ramp monotonicity (the velocity
  turns over at frame +16), and the plateau (12.98–14.57 against an envelope of
  21.8–22.7).
* `:120` trips **two** — duration, and the tail: the speed is still 0.96 of the plateau
  where the authored boost has decayed to 0.56. This is the arm that proves the
  per-frame trace assertion is load-bearing, because it does **not** change the peak.

That last point is the physically interesting result: **the boost saturates.** Holding
`boostTimer` 2.7x longer reaches the same 22.358, because `traction = 2.0f` per update
against the drag term reaches terminal velocity well inside 45 ticks. A zip pad cannot
produce an unbounded speed however long it is held — its magnitude is bounded by the
authored physics, not by the timer.

## Presentation-mode check — `tests/check_video_presets.py`

```bash
python3 tests/check_video_presets.py --build build \
  --rom baserom.us.v80.z64 -v
```

Runs the `nav_to_time_trial_race` route once per presentation mode and requires
all three normalized `[PACE]` streams to be identical (3400 rows). Same
reasoning as the widescreen check: the modes are allowed to change how the frame
looks, never what the race does.

It also asserts that **Pure reproduces the authored 4:3 framing** — on a
2560×1440 drawable Pure must pillarbox (`presentation=1920x1440+320`, aspect
1.33333) while Restored fills (`2560x1440+0`, 1.77778). That assertion is
deliberate protection against a plausible-looking "simplification": setting
`Video.Widescreen=0` does *not* produce a 4:3 image, it engages the
pre-widescreen path that stretches 4:3 across the window. Pure pins
`Video.Aspect=4:3` instead.

The precedence ladder (`default < preset < env < --video-set`) is checked
through `--video-list`, which exits before ROM load and so costs nothing. Note
that passing a mode flag explicitly counts as a *preset* application even when
it resolves to the same value the default held; `[default]` appears only when no
mode flag is given.

The positive control is an `MDKR_RNGSEED=legacy` arm, which perturbs the boot
seeds and therefore the simulation: its stream **must** differ from baseline. It
currently diverges at row 3157 on a sub-unit position delta. Without that arm, a
comparator that had silently stopped finding rows would read as a pass.

The gate strips every inherited `MDKR_*` variable before running, so a developer
with `MDKR_RENDER_SCALE` exported cannot accidentally turn every arm into the
same env-precedence override.

An odd-size Pure framebuffer arm also checks the real captured side gutters.
Every pixel within each gutter must be uniform and both sides must match; the
gate deliberately accepts the game's authored clear color instead of assuming
that unused presentation space is black.

## Widescreen + shadow-depth check — `tests/check_widescreen_shadow.py`

```bash
python3 tests/check_widescreen_shadow.py --build build \
  --rom baserom.us.v80.z64 -v
```

This runs the same closed-loop race at 4:3, 16:9 and 21:9 and requires the
normalized `[PACE]` streams to match exactly. That is stronger than a screenshot:
DKR's object renderer advances animation, can consume gameplay RNG, and feeds the
racer's on-screen simulation timer, so an over-eager widescreen cull change can
alter the race while still looking plausible.

The same harness asserts that every production shadow draw group requests decal
depth, normal heap high-water marks stay inside all three physical allocations,
and a tiny `MDKR_SHADOW_CAPS=2,8,16` fault-injection run drops whole meshes
without a crash. Its final `MDKR_SHADOW_DECAL=0` arm deliberately restores the
old non-decal route and must be detected, proving the check cannot pass merely
because the fixture rendered no shadows. Run it against an ASan build as well as
the GL and WebGPU builds.

A sixth arm repeats the production 4:3 case in **shipping configuration**
(`MDKR_TRACE` unset). `[PACE]` cannot exist there, so what it asserts is the
strongest untraced subset: the `[SHADOW]` decal mode, all three heap capacities,
the overflow-drop count and the non-decal group count must be identical to the
traced arm, and the arm must carry no `[PACE]` rows at all. This gate dumps no
frames, so the pixel half of the same question is carried by
`check_world_shadows.py`, `check_shadow_visual_ab.py`,
`check_shadow_plausibility.py` and `check_presentation_shadows.py`.

For the multiplayer projection path, `check_race_2p_split.py` also accepts
`--window-size WIDTHxHEIGHT`; use `--window-size 1260x540` to exercise both
top/bottom viewports at 21:9 while retaining its independent liveness check for
each half.

### Widescreen/FOV asset proportions — `tests/check_widescreen_proportions.py`

```bash
python3 tests/check_widescreen_proportions.py --build build \
  --rom baserom.us.v80.z64 --renderer webgpu -v
```

This dependency-free pixel gate drives Timber's Island through two deterministic
approach samples of balloon 10: frame 6300 keeps the SAFE_2D HUD glyph clear of
the rainbow behind it, and frame 6410 brings the world-space F3DDKR billboard
close enough for a meaningful changed-FOV measurement. It segments the saturated
blue zigzag inside each balloon and runs seven isolated arms at equal height:
4:3, 16:10, 16:9, 21:9 with the default 104° cap, forced 4:3 centered inside a
21:9 drawable, 16:9 at 75° FOV with the cap off, and exact 21:9 legacy stretching.

Measured on the current Release GL and native WebGPU paths:

| arm | HUD motif | world motif | required interpretation |
|---|---:|---:|---|
| 4:3 | 99×36 | 48×18 | reference |
| 16:10 | 99×36 | 48×18 | identical authored size |
| 16:9 | 99×36 | 48×18 | identical authored size |
| 21:9, cap 104° | 99×36 | 50×19 | 1.053× lens scale; shape retained |
| 21:9, forced 4:3 | 99×36 | 48×18 | centered presentation; authored size |
| 16:9, FOV 75° | 99×36 | 36×14 | 0.752× lens scale; shape retained |
| 21:9 legacy | 173×36 | 84×18 | known-bad 1.75× horizontal stretch |

Every arm must reach the same normalized frame-6410 racer state and collect the
same balloon at frame 6476. Production motif aspect must remain within 8% of the
4:3 reference, size must track the analytic focal scale, and the legacy arm must
fall well outside that threshold. The legacy arm is the positive control: if the
detector cannot distinguish the former distortion, the check fails. It passes
Debug GL, Release WebGPU, and ASan GL.

The same check performs a source census of the unusual post-projection
billboard mode. Exactly two audited producers may enable it, both in `camera.c`
(world and ortho); the world builder must call the shared aspect/FOV correction;
and the F3DDKR decoder must retain one local-offset path. A new direct producer
fails the gate until its transform is classified and covered. Current world
callers — collectibles, weapons/bubbles, boost effects, particles, rain splashes,
lens flare, and the magnet reticle — all converge on that builder. Vehicle-part
sprites use the ordinary isotropic perspective matrix; HUD/menu sprites and
rectangles use SAFE_2D.

### Cinematic and framed live views — `tests/check_framed_world_views.py`

```bash
python3 tests/check_framed_world_views.py --build build \
  --rom baserom.us.v80.z64 -v
```

This distinguishes unframed cinematics from wooden-frame apertures. Opening
logos, animated credits, the unframed Track Select setup page, and initial
post-race footage must fill the horizontal presentation; Track Select's browser
and zoom phases plus later single-player post-race footage must keep live
geometry inside their wooden frames. The reverse Track Select transition must
restore containment before its frame appears. Decorative backgrounds must fill
the side regions without black gutters. The gate captures 4:3, 16:9, and 21:9
output on WebGPU and repeats the ultrawide witnesses on OpenGL.

Three additional WebGPU witnesses run Original simulation cadence at 60 Hz
with interpolation enabled and capture the in-between present crossing Track
Select setup, its reverse transition, and the first framed post-race phase.
Safe-aperture/presentation policy changes are discrete: the snapshot must use
the new camera recipe whole, never blend a safe aperture into a widescreen
projection. Animated viewport coordinates continue interpolating once the
policy is stable.

Each arm is paired with `MDKR_TEST_FRAMED_WORLD_UNSAFE=1`. The renderer fault
must be inert for presentation scenes and all 4:3 captures, while framed scenes
must reproduce visible side-band bleed at wider ratios. Frontend routes and
normalized gameplay traces must stay unchanged between the two arms.

One further arm, `--scene charselect-aperture-leak`, asserts that an aperture
does not outlive the screen that asked for it. Track Select's safe 4:3 region is
per-viewport state that the projection latch reads every tick, while the
unframed render path states the presentation region to the renderer; if the
region survives Track Select the two disagree and every later screen draws a 4:3
lens across the whole presentation rectangle. The arm captures character select
twice at 16:9 — once reached through Track Select, once reached directly on the
same frame — and recovers the horizontal scale that best maps one onto the
other. Character select animates, so the comparison is a per-column brightness
profile rather than a pixel identity. The recovered ratio must be 1.0; a
synthetic 4:3 stretch of the control must be rejected, which is what keeps the
detector honest. Measured before the fix, the leaked route recovered 0.750.

`check_shadow_visual_ab.py` is the slower pixel-level companion:

```bash
python3 tests/check_shadow_visual_ab.py --build build \
  --rom baserom.us.v80.z64 --renderer webgpu -v
```

It compares 300 consecutive moving-camera frames against the deliberate
`MDKR_SHADOW_DECAL=0` regression. The gameplay streams must remain identical,
some (but not all) frames must differ, and every affected production pixel must
be darker: decal bias may restore missing shadow coverage, but may not perturb
geometry or the rest of the frame. The PPM comparison is dependency-free and
needs no checked-in golden image.

A third arm runs the production decal route in **shipping configuration**
(`MDKR_TRACE` unset) and requires every dumped frame to be byte-identical to the
traced arm's. The comparison is deliberately on pixels rather than on the
`[SHADOW]`/`[DEPTH]` totals: this route is wall-clock paced (it pins neither
`MDKR_SYNTH_FIELDS` nor the cadence, because its texture-filtering invariant
needs `--pure`), so those cumulative counters move run to run — measured 2371 /
2510 / 2590 presents across three arms of one build — while frames keyed to
simulated frame numbers do not.

## Idle / attract-demo soak (no input script)

Not every crash needs input. Letting the title screen idle starts the **attract
demo** at ~frame 5100, which loads real race levels back to back — a path no
`nav_*` fixture reaches, and the only thing that caught the object sub-pool
exhaustion documented in `docs/OPEN_ITEMS.md`. Soak it with:
```
MDKR_AUDIO=0 MDKR_TRACE=1 ./build/mdkr64 --headless-frames 12000 \
  --rom baserom.us.v80.z64 2>&1 | grep -E 'level_load|CRASH|FATAL'
```
Expect level loads at ~14, ~173, ~1134 (frontend), then a new demo level roughly
every 1500 frames, and no `[CRASH]`/`[FATAL]`. The run must reach frame 12000 and
exit **rc=0**; the demo cycles back to the frontend (levelId=23) at ~frame 8132.
This soak is the *only* thing that exercises the audio event-queue saturation and
the boost-graphics draw — both of which wedged or crashed here before, and neither
of which any `nav_*`/`race_*` fixture reaches. Treat a non-zero rc, a stalled
frame counter, or an `[EVTQ] post DROPPED` line as a failure.

`MDKR_EVTQ_STATS=1` adds per-audio-queue high-water-mark reporting
(`[EVTQ] qN(ptr) new peak P of S`) — that is how the music queue was sized. It
walks the alloc list on every post, so use it only for measurement runs.

**`MDKR_AUDIO=off` is a no-op** — `platform/audi_port_dkr.c` tests
`disable[0] == '0'`, so only `MDKR_AUDIO=0` skips the device. `--headless-frames`
is what actually guarantees silence (it disables the SDL audio device
unconditionally); `MDKR_AUDIO=0` is belt-and-braces.

### Autopilot — `MDKR_AUTOPILOT=1` (drive with DKR's own AI)

The open-loop input route holds the racing line for one lap and then strands the
kart, so it cannot be used to reach a race finish. `MDKR_AUTOPILOT=1` instead
drives the **human** racer with `racer_AI_pathing_inputs()` — the same driver every
CPU racer uses and the one that laps real tracks in the attract demo. Race logic,
physics and finish handling are untouched, so what it exercises is the real code
path.

```bash
MDKR_AUTOPILOT=1 MDKR_AUDIO=0 ./build/mdkr64 --headless-frames 12000 \
  --input-script tests/input_scripts/race_full_3lap.txt --rom baserom.us.v80.z64
```

Measured on Ancient Lake (deterministic — identical across runs): lap 1 at clock
1703, lap 2 at 3353, i.e. a consistent ~1650/lap racing line, versus 2776 for lap
1 and a stuck kart on lap 2 open-loop. It is a **no-op unless set**.

It drives **every** racer whose `playerIndex != PLAYER_COMPUTER`, so on a
two-player route it covers *both* human racers with no extra machinery.
## Track coverage sweep — `tests/check_track_sweep.py` (RUN THIS AFTER ANY ASSET/RACE CHANGE)

Everything else in this suite validates **one track in one vehicle** (Ancient
Lake, car). There are 20 playable tracks and three player vehicles, and this
port's recurring failure mode is a per-track asset that decodes wrong and fails
*silently* — `ASSET_MISC_20` (boost), `ASSET_MISC_8` (steer divisor) and the
per-triangle collision facets were all that shape.

```bash
python3 tests/check_track_sweep.py                        # all 20 tracks
python3 tests/check_track_sweep.py --tracks 5,9 --frames 4200
python3 tests/check_track_sweep.py --vehicle 1            # force hovercraft
python3 tests/check_track_sweep.py --expect-fail N        # diagnostic only: tolerate a named failure
```

It uses two hooks, both **no-ops unless set**:
- `MDKR_LOAD_TRACK=<levelId>[:<vehicle>]` (`game/src/game.c`) retargets the *race*
  load of one proven route, so 20 tracks need one fixture instead of twenty. It
  rewrites only the load matching `gTrackIdToLoad` — DKR's own "track to race"
  global — so menu backgrounds and the track-select preview are untouched.
- `MDKR_AUTOPILOT=1` drives with DKR's own AI, so no per-track input tuning.

Both sweeps pin Original presentation/cadence with `MDKR_SYNTH_FIELDS=2`,
producing one authored gameplay ticket per headless host opportunity. They
therefore measure content and physics coverage, not a developer's persisted
frame limit, compiler, sanitizer, renderer, or compositor speed; the separate
pacing gates retain responsibility for real-clock behavior.

Per track it asserts: exit code 0, the level actually loaded as a race, finite
position samples, real forward progress (`courseCheckpoint` climbing), and no
`[FATAL]`/sanitizer/assert output.

**Historical first run:** level 9 alone died while updating a snowball object.
That diagnosis no longer reproduces: level 9 completes a 12000-frame autopilot
race with `maxcp=80` in both the old- and ROM-faithful-math arms. The finding is
therefore retracted on current source, not treated as a known failure. Release
validation must run without `--expect-fail`; otherwise a new level-9 regression
would be silently accepted.

⚠️ **With no `--vehicle`, this sweep drives all 20 tracks in the CAR** — it does
*not* use each track's own default vehicle, and its help text used to claim it did.
`MDKR_LOAD_TRACK` rewrites the level id inside `level_load()`, long after the menus
picked the vehicle from `leveltable_vehicle_default(gTrackIdForPreview)`, and the
previewed track on this route is always Ancient Lake — a car track. So this sweep
also races a car on the three hovercraft-only tracks (Whale Bay 8, Pirate Lagoon 4,
Boulder Canyon 19), which the real track-select menu would never offer. Use
`check_vehicle_sweep.py` below for per-vehicle coverage.

## Vehicle coverage sweep — `tests/check_vehicle_sweep.py` (RUN THIS AFTER ANY RACER/PHYSICS CHANGE)

Every race this port had ever validated was a **car** race, on every track, for the
reason in the warning above. DKR has three player vehicles with genuinely separate
physics modules — `update_player_racer()`'s `switch (vehicleID)` dispatches to
`func_8004F7F4` (car), `func_80046524` (hovercraft) and `func_80049794` (plane) —
and several tracks cannot be raced in a car at all.

```bash
python3 tests/check_vehicle_sweep.py                 # the full legitimate matrix (47), ~30 min
python3 tests/check_vehicle_sweep.py --matrix        # print the matrix and exit
python3 tests/check_vehicle_sweep.py --vehicles 1,2  # hovercraft + plane rows only
python3 tests/check_vehicle_sweep.py --tracks 6,32   # the two loop-de-loop tracks
```

**It sweeps 47 combinations, not 60.** `LevelHeader.available_vehicles` (ROM offset
0x4E) is the bitmask the track-select menu offers and `LevelHeader.vehicle` (0x4D)
is its default; `level_global_init()` packs them into
`gGlobalLevelTable[i].vehicles` as `(available << 4) | (default & 0xF)`, which
`leveltable_vehicle_usable()`/`leveltable_vehicle_default()` read back. Forcing a
plane onto a hovercraft-only track is a meaningless input, not a bug, so the sweep
is exactly the mask: car on 16 tracks, hovercraft on all 20, plane on 11.

The mask is decoded **from the ROM**, independently of any port code (like
`tools/dump_misc_asset.py`), and then cross-checked against the game's own reading,
published by `level_global_init()` as
`[TRACE] level_vehicles: id=N default=D avail=0xM`. Without that cross-check the
sweep would only be agreeing with its own assumption.

Per combination it asserts exit code 0, that the level loaded as a race **with the
requested vehicle**, that the racer was really updated by that vehicle module, all
position samples finite, forward progress (`courseCheckpoint`) plus real
displacement, no `[FATAL]`/sanitizer output — and two things a survival check cannot
see:

- **A plane must use the vertical axis.** `y` spread ≥ 60 world units. A plane that
  loads, "drives", and sits at one altitude is a defect; the pitch axis is an input
  path neither other vehicle touches. Measured plane rows span 331–1235; the
  flattest car row is 210 and the flattest row of any kind is 76 (Whale Bay,
  hovercraft). `MDKR_AUTOPILOT` does drive pitch — `func_80045C48` derives
  `gCurrentStickY` from the spline's vertical derivative.
- **Per track, the vehicles must trace DIFFERENT paths.** The position stream is
  hashed per row; two vehicles on one track colliding means the override did
  nothing. Together with `[PVEH]` (below) this is what makes "we really raced a
  hovercraft" falsifiable rather than assumed.

`[PVEH] frame=N player=P vehicleID=V` is a new greppable line (emitted on change, so
one line per race) reporting the `vehicleID` `update_player_racer()` actually
dispatches on. It is deliberately **not** part of `[PACE]`, whose format stays
byte-compatible with every existing check. `MDKR_LOAD_TRACK=<level>:<vehicle>` only
rewrites the *argument* to `level_load()`; whether the racer object ends up with
that vehicle is a separate question, and a silently-ignored override would have
driven a car through all 47 rows and passed.

**`VEHICLE_LOOPDELOOP` (4) is permitted mid-race and reported as `+loopdeloop`.**
That is not slack: DKR tracks place a vehicle-change trigger object
(`object_functions.c`, the `racer->vehicleID = trigger->vehicleID` branch) around
corkscrew sections, which swaps the racer into the loop physics module
(`func_8004CC20`) and restores `vehicleIDPrev` at the exit trigger. Walrus Cove (6)
in both car and hovercraft, and DarkMoon Caverns (32) in hovercraft, do exactly
this — which is also the only coverage that module has. The *first* dispatch must
still equal the requested vehicle, so the falsifiability above is intact.

**The first plane race ever run in this port aborted**: `f32 sp60[4]` in
`func_80049794` holding the 4x4 `MtxF` that `mtxf_from_transform()` writes — a
48-byte stack overflow, `__stack_chk_fail`, **exit 134 with nothing on stderr**.
Same shape as the `timetrial_ghost_read` and `fileselect_render` overflows
(HANDOFF.md §3). See the comment at that declaration in `racer.c`. Positive
control: restore `f32 sp60[4]` and every plane row fails with `exited 134`.

Two tracks narrow the mask further, but only at 2+ players (`menu.c`, `VERSION >=
VERSION_79`): Spaceport Alpha (15) drops the hovercraft, Frosty Village (28) drops
the plane. This is a one-player sweep, so both are swept in all three vehicles;
`--two-player-mask` applies the 2P narrowing instead.

## Two-player split-screen — `race_2p_split.txt` + `tests/check_race_2p_split.py`

```bash
MDKR_AUDIO=0 python3 tests/check_race_2p_split.py -v                  # ~2.5 min, WebGPU
MDKR_AUDIO=0 python3 tests/check_race_2p_split.py -v --renderer gl    # and again on GL
MDKR_AUDIO=0 python3 tests/check_race_2p_split.py -v --cadence enhanced
```

**Every other fixture is a one-player route, and a one-player build passes all of
them.** This is the only route that reaches `gNumberOfActivePlayers == 2`.

Two things about it are easy to get wrong:

1. **A second player can only join on their own pad.** `charselect_new_player()`
   scans `gMenuButtons[0..3]` for a pad that is not yet in `gActivePlayersArray`,
   so *no* amount of input on port 0 can produce a second player. That is why the
   input-script format grew the `P1..P4` port field (see above). Positive control:
   ignore the port field in `script_apply()` and the check fails with
   `no [PACE2] rows — PLAYER_TWO never published a racer probe`.
2. **The two-player menu graph is a different graph.** In
   `menu_character_select_loop()` the branch is
   `confirmOffset >= gNumberOfActivePlayers` with `confirmOffset == 1` when
   entering char select from the title, so one player goes to
   CAUTION(28)/GAME_SELECT(19) and two players go **straight to
   TRACK_SELECT(15)**. Track select then skips `TRACKMENU_CHOOSE` (the Time-Trial
   toggle — multiplayer forces `set_time_trial_enabled(FALSE)`) and adds the
   "how many CPU racers" stage. Do not copy the one-player tap timings.

The default invocation gates the shipping `SimulationCadence=original` path;
`tools/run_checks.py` also registers a separate `race_2p_split_enhanced` arm for
the opt-in historical one-field mode. The long route deliberately enables the
native autopilot hook so it can finish without physical controllers; its
cadence-specific path measurements are an AI/end-to-end smoke contract, not
evidence about human input routing. Direct human binding belongs to the focused
gate below.

The current fixed-one-field Enhanced reference is 4,719 shared racing frames.
The in-racer pace probes end at P1 checkpoint 53/lap 2 and P2 checkpoint 39/lap
2; max step is 23.3/44.7, max step-to-step change is 1.6/4.0, and the slowest
240-frame mean is 10.70/1.82. P2's slow window is the chaotic AI hook's authored
lane, not evidence of controller starvation. The post-update oracle supplies the
end-to-end result: P1 crosses on lap 3 and takes position 1; the production
N-1-finished rule classifies the still-racing P2 last on lap 2 at position 2.
The check requires those exact finish roles, distinct positions, results at
frame 7632, and track select at 7931. In-memory missing-P2, swapped-position,
and invalid-P2 controls prove the terminal classifier fails in each broken
direction. `check_2p_human_binding.py`, with autopilot off, separately proves
the real P1/P2 controller paths and first-sector response.

Each arm asserts the **exit code** plus the output-overlay ordering contract and
eight independent things: the
`hud_init: hudPlayers=1 numViewports=2` layout line, a two-player
`level_load: ... numPlayers=1`, the existence of the `[PACE2]` player-2 probe,
per-player motion sanity (finite position, y band, no discontinuous teleport,
checkpoint/lap progress, no stall) for **both** players, that the two racers stay
apart in world space (so "the same racer traced twice" cannot pass), the top
and bottom halves of each sampled in-race frame scored **separately**, so a live
viewport cannot mask a dead one, and (since 2026-07-29) the full **2P post-race
classification and flow**: the terminal oracle records P1 first/P2 classified
last, `MENU_RESULTS` loads, and results returns to `MENU_TRACK_SELECT` —
mirroring `check_race_multiplayer.py`'s 3P/4P arms. The
fixture taps A instead of holding it (post-race menu input is edge-triggered),
and motion is judged only while each racer's own race clock advances, because
DKR freezes finished racers for the fade/results sequence.

As in `check_race_drive.py`, teleport detection judges the *shape* of the motion:
`MAX_ACCEL = 40` limits the frame-to-frame change in step length, with a generous
`MAX_STEP = 150` absolute backstop. The former 40-unit absolute ceiling was stale:
on 2026-07-31 player 2 took a real zip pad and ramped smoothly through 40.15,
40.73, ... 44.75 units/frame before ramping down, while the whole run's largest
step-to-step change was only 4.0. The known ASSET_MISC_8 break instead moved
1296.8 units in one frame, so the shape test retains ample separation in the
broken direction.

The fixture's track-preview transition also covers the queued display-list
pointer lifetime repaired by the later all-content census. The original report
called it an optional/unterminated child; the measured command was a valid
`G_DL` to `dRspInit` whose registry entry had been cleared before its queued
task executed. One-generation registry grace fixes the token/segment collision,
and `MDKR_DL_STRICT=1` now passes this route with zero faults.

The deterministic 9,600-frame contract was re-measured on forced WebGPU on
2026-08-08 with the retail vehicle-audio RNG restored (`c6fbd94`). cp, the
slowest-window mean speed, and both-players-traced frame count are all pure
functions of the AI racing line, which moves under any legitimate physics/RNG/
collision change — a per-cadence pin within a percent or two of one recorded
run turns a healthy build into a false "not driving the track" / "stalled" /
"lost player" failure (this happened once already: a `ParticleBehaviour`
stride fix forced `check_race_drive.py`'s own `MIN_FINAL_CP` 20 -> 15). Both
cadences now use `check_race_drive.py`'s calibration norm instead — a floor
set well below the measurement, wide enough to separate "drove multiple laps"
/ "not wedged" / "raced for most of the run" from "went nowhere":
checkpoint `>=25/18` (observed 53/39 original, 53/40 enhanced), `min_both_frames`
`>=1300` original / `>=2900` enhanced (observed 2180 / 4852, ~40% headroom
each), and slowest 240-frame mean `>=12.0/6.0` original / `>=5.0/0.6` enhanced
(observed 25.39/14.59 and 10.78/1.52). `min_final_lap>=2` is unchanged and is
the check's real "both players progressed" gate — it is a monotone lap
counter, not a trajectory-sensitive statistic, so it keeps its tight, honest
floor. Both cadences retain the shared finite-position, track-band, separation,
`MAX_STEP=150`, and `MAX_ACCEL=40` protections. Neither cadence may reach
results without oracle-confirmed `raceFinished=1`, distinct positions P1=1 and
P2=2, P1 lap `>=3`, and P2 lap `>=2`; the three terminal broken-direction
controls must also reject. These autopilot thresholds intentionally say nothing
about physical controller binding.

Positive control for the render half of the check:
```bash
MDKR_AUDIO=0 python3 tests/check_race_2p_split.py --keep-frames /tmp/2p
python3 tests/check_race_2p_split.py --blank-half-control /tmp/2p
```
which flat-fills one half of each real frame and requires the per-half metric to
**reject** it (measured: 1 colour / sigma 0.0). Without that, "both halves scored
fine" is unfalsifiable.

`[PACE2]` is a separate greppable line from `[PACE]`, so the player-1 `[PACE]`
format stays byte-compatible with `check_race_drive.py` and
`check_race_finish_time.py`.

**Per-player in-race input is separately verified by
`check_2p_human_binding.py`.** Autopilot is off. Neutral, P1-only, P2-only, and
both-active scenarios run at Enhanced fixed-one-field cadence across GL/WebGPU
and 60/120 Hz presentation. The gate checks the exact held, pressed, released,
deadzone-adjusted stick, player, racer, port, and update-delta values at the
production racer dispatch boundary. It also requires byte-identical v3 state,
consumed input, binding, and per-clock trajectories across all four
renderer/rate arms. In the measured P2-only arm, P2 reaches checkpoint 5 over a
6,296.6-unit path while P1 remains exactly stationary; the converse P1 arm and
the both-active arm also progress. Drop-P2, swapped-port, and neutral-as-active
controls prove that the gate rejects starvation, cross-wiring, and false motion.

## Three-/four-player split-screen and results — `tests/check_race_multiplayer.py`

```bash
python3 tests/check_race_multiplayer.py --build build -v
python3 tests/check_race_multiplayer.py --build build --players 3
python3 tests/check_race_multiplayer.py --build build --players 4
```

This is not a wider survival-only version of the 2P check. Each arm navigates
the real per-controller frontend, loads Ancient Lake with the exact encoded
player count, and proves all of the following:

1. `hudPlayers=2/3` and `numViewports=3/4` select the intended production
   layout.
2. `[PACE]`, `[PACE2]`, `[PACE3]`, and (for 4P) `[PACE4]` come from distinct
   human racers. Each stream must remain finite, stay in the track's vertical
   band, avoid one-frame teleports and long stalls, and make substantial
   checkpoint/lap progress. Every non-last-place racer must reach lap 3
   (`[PACE] lap=2` before its terminal update).
3. Every racer pair has a nontrivial median world-space separation, so copying
   one valid probe into several slots cannot pass.
4. Every scene quadrant is scored independently. In 3P, the intentionally
   mostly-black fourth quadrant has a separate minimap contract requiring the
   track, racer arrows, and start line instead of pretending it is a fourth
   camera.
5. The race advances through multiplayer `MENU_RESULTS` (menu 17), then the
   default Select Track action returns to `MENU_TRACK_SELECT` (menu 15). The
   input fixtures use spaced A edges; the former long A hold could never
   advance this edge-triggered flow.
6. End-of-update `[ORACLE]` rows prove every human becomes `raceFinished=1` and
   receives each finish position exactly once. DKR intentionally terminates an
   ordinary multiplayer race after N-1 racers finish and classifies the sole
   remainder last; that one DNF may stop at `cp>=30/lap>=1`, while every other
   racer retains the stricter `cp>=40/lap>=2` requirement.
7. The same visual scorer must reject each quadrant after it is flat-filled.
   This positive control runs automatically in both arms.

Measured Debug/WebGPU reference: 3P loads at frame 2361, reaches results at 7632,
and returns to track select at 7931; 4P does so at 2441, 7932, and 8231. On the
2026-07-31 fidelity line, 4P P1/P3/P4 finish normally and P2 is the authored
last-place classification at `cp=33/lap=1`; its end-of-update state is
`fin=1/fpos=4`, and it continues production finish-camera driving through
`cp=34`. The 4P P2 pace probe retains 5,225 active rows, reaches `cp=33/lap=1`,
and has a slowest 240-frame mean of 1.28; the tightened gate requires 4,500
rows, `cp>=32`, and mean speed >=1.0. All other human streams remain well above
those floors. Scene quadrants measure 1003–2603 quantized colours and
sigma 24.9–58.7; the 3P minimap measures 85–126 colours, sigma 33.1–33.9, and
5.6–5.8% nonblack coverage.

The fixture validates scripted local pads and the production renderer/HUD/menu
paths. It does not claim a pixel oracle against retail hardware or physical
controller hotplug behavior. Battle/challenge modes are outside this ordinary
race fixture, but their authored one-human/three-AI outcomes are now closed by
`check_challenge_modes.py`; Taj's distinct two-racer path is closed by
`check_taj_challenges.py`.

## Determinism check — `tests/check_determinism.py` (RUN THIS AFTER ANY PORT-LAYER CHANGE)

Every frame comparison in this project — the ares oracle scoring, GL-vs-WebGPU
parity, native-vs-wasm, and every "dump a PNG and look at it" verification —
assumes a headless run is **reproducible**. Until the `osGetCount()` fix it was
not, and nothing caught it: a non-reproducible renderer still exits 0 on every
fixture.

```bash
python3 tests/check_determinism.py                    # 3 fixtures x GL/WebGPU x 2 runs
python3 tests/check_determinism.py --runs 3 --every 200 -v
python3 tests/check_determinism.py --renderer webgpu  # focused backend rerun
```
It strips all `MDKR*`/`GE007_*` caller settings, explicitly runs both native
backends, and requires every dumped frame to be byte-identical across N runs.
Measured before the fix, `nav_to_character_select` frame 1450 over 10
runs: **10 distinct images, 45/45 pairs differing, median 18.9 % of pixels**
(minimum 4.7 %). Verified to FAIL on a build with the fix reverted.

**If you touch the pacer, the counter, or anything the game reads as time, run
this.** The failure mode is silent: no crash, no assertion, every fixture green,
and every visual comparison quietly meaningless.

## Authored state/RNG compatibility — `tests/check_authored_rng_compat.py`

This gate is intentionally narrower and stricter than the general determinism
suite: it covers only the original/authored two-field cadence on the 4,800-frame
`race_state_oracle` route. All 27,840 all-racer rows—including positions,
velocities, race progress, logical delta, and the shared authored RNG—must have
the raw SHA-256
`d74efe02aec07aa59710ce457e54180c28a22022f3d35e7087096d5130dba49b`.
The check also flips one RNG bit in the first row and fails unless its own
validator rejects that mutation.

The reference is clean commit
`64936e36b4c9ef7ecdce5beb93cd662d4318548d`. This deliberately replaces the
pre-vehicle-audio hash. The old port skipped `racer_sound_car()` for ordinary
cars, so it also skipped the retail routine's shared `rand_range()` draws and
froze the wrong world/RNG stream.

That authority decision was not inferred from the changed hash. The pinned,
instrumented ares ran the owned US 1.1 ROM through the same regular-car route
and trapped the real `rand_range` entry at `0x8006FB8C` only when its return
address was inside retail `racer_sound_car` (`0x80005254..0x80005D08`). It
recorded 4,801–4,802 player-zero, vehicle-zero calls (the presented-frame exit
can admit one terminal first roll): 2,926–2,927 from the first jitter roll and
1,875 from the conditional second roll, all over `[0,10]`. Two independent runs
had the same normalized first-3,500-call SHA-256
`9fd7cb9aebc163b00f9c8e4bfd292f90b684b4d46415ab5e0ef594c8bfb2d16e`.
This command regenerates and validates that witness; the CSV is ROM-derived and
must never be committed:

```bash
tools/prepare_ares_oracle.sh
tools/run_oracle.sh race_state_oracle --skip-native \
  --vehicle-rng-trace /tmp/mdkr-ares-car-rng.csv \
  --rom /path/to/owned-us-v11.z64
```

The native digest was independently reproduced from the detached accepted
commit with this method (use an empty path for the worktree and your own
supported ROM):

```bash
set -o pipefail
git worktree add --detach /tmp/mdkr-authored-reference \
  64936e36b4c9ef7ecdce5beb93cd662d4318548d
cmake -S /tmp/mdkr-authored-reference \
  -B /tmp/mdkr-authored-reference/build-reference \
  -DCMAKE_BUILD_TYPE=Release -DMDKR_WEBGPU_BACKEND=OFF -DBUILD_TESTING=OFF
cmake --build /tmp/mdkr-authored-reference/build-reference -j
cd /tmp/mdkr-authored-reference
env -i PATH="$PATH" LC_ALL=C MDKR_AUDIO=0 \
  MDKR_SIMULATION_CADENCE=original MDKR_SYNTH_FIELDS=2 MDKR_TRACE=1 \
  MDKR_ORACLE_STATE=1 MDKR_RENDERER=gl \
  MDKR_SAVE_DIR=/tmp/mdkr-authored-reference/reference-save \
  ./build-reference/mdkr64 --headless-frames 4800 \
  --input-script <(python3 tools/dkr_oracle_route.py native-script \
    race_state_oracle --arm original) --rom /path/to/owned-us-v11.z64 2>&1 | \
  python3 -c "import hashlib,sys; r=[x for x in sys.stdin.buffer if b'[ORACLE]' in x]; print(len(r), hashlib.sha256(b''.join(r)).hexdigest())"
cd -
git worktree remove --force /tmp/mdkr-authored-reference
```

That command independently reproduced exactly 27,840 rows and the pinned
digest on 2026-08-03. The raw trace is ROM-derived and must never be committed.
Current native route compilation emits positive input entries one ticket early:
the fixed-ticket host publishes script entry N to the simulation sample traced
at N+1, while route JSON frames name the intended authored present/tick phase.
This phase conversion preserves the exact accepted digest above; removing it
produces 27,832 rows and is the gate's required timing-regression direction.

## Audio output — `tests/check_audio_output.py` (RUN THIS AFTER ANY CHANGE UNDER `game/src/audio*`, `platform/audio_*.c`, `platform/mixer*` OR `platform/audi_port_dkr.c`)

The only check that asserts on **sound**. Until it landed, `README.md` and
`CHANGELOG.md` both listed audio as working and nothing in `tests/` looked at it —
every fixture runs muted by the hard rule above, so the claim rested on somebody
having listened to it.

**It opens no audio device.** `--headless-frames` returns before SDL audio is touched
and `MDKR_AUDIO=0` is set on top; synthesis still runs headlessly, and
`MDKR_AUDIO_DUMP` taps the PCM to a file while `MDKR_AUDIO_RMS=1` prints the mixer's
own RMS / peak / main-bus-clip / fx-guard accounting plus a per-service music trace. The
captures are **ROM-derived**: they go to a temp dir that is deleted unless
`--keep-audio` is passed, and a `.wav` must never be committed (`.gitignore` covers
`*.wav`).

```bash
MDKR_AUDIO=0 python3 tests/check_audio_output.py            # ~12 s, 2 runs x 4300 frames
MDKR_AUDIO=0 python3 tests/check_audio_output.py --control dry-vs-dry   # must exit 1
MDKR_AUDIO=0 python3 tests/check_audio_output.py --control boot-only    # must exit 1
```

Seven assertions over `race_drive_time_trial.txt` at 4300 frames, which crosses boot
silence, the attract intro, the title, character select, track select and a level load
into a race — six music sequences at five different tempos, with engine SFX on top:

| # | asserted | measured |
|---|---|---|
| 1 | 22050 Hz / 2 ch / 16-bit, and 736 sample-frames per pump == 2 NTSC fields | 33.379 ms vs 33.367 ms |
| 2 | not silent, and silent where it should be | whole rms 6580, windows 2372-8908, pre-boot rms 65 with 99.61 % exact zeros |
| 3 | not saturated | rail 0.00174 %, main-bus clip 0.01084 %, worst overshoot 1.29 dBFS |
| 4 | both channels alive, not a mono duplicate | L 6594 / R 6567, rms(L-R) 2864, corr +0.9053 |
| 5 | the music changes across transitions | 8-band spectral L1 0.525-1.145 over seven adjacent pairs |
| 6 | the beat grid sits at the tempo the sequencer programmed | argmax 0.9981x-1.0041x of the live quarter note on three windows; +-5 % detunings score negative |
| 7 | the reverb path contributes | 5.10 % of the dry mix is bit-exactly zero; the wet mix carries rms 100.9 there; fx-guard 0 |

Assertion 6 is the one that separates "the synthesiser is running" from "the
synthesiser is running at the wrong rate" — the class of bug behind the "crazy fast
dance" report. Its reference is `alCSPGetTempo()`, read out of the sequence player
each frame, **not** `music_tempo()`: `music_tempo()` reports only the BPM `audio.c`
last programmed from `gSeqSoundTable`, and it goes stale on any sequence carrying its
own `AL_MIDI_META_TEMPO` events. The attract intro (sequence 35) walks 130 -> 140 ->
120 BPM while `music_tempo()` still says 110.

**Note what is deliberately NOT asserted: peak below full scale.** The peak IS full
scale on healthy audio (DKR's mix touches the rail; the RSP mixer saturates on
hardware too), so that assertion would fail on a correct build. What separates healthy
from destroyed is the *fraction* at the rail and the pre-clamp overshoot, which is
what assertion 3 measures.

The check is proven in both directions. Five signal-level controls run on every
invocation — the analysis is re-applied to corrupted copies of the real capture
(zeroed, `R := L`, x6 gain, one block repeated, resampled by 1.12x) and the check
**fails if any of them passes**. The two `--control` arms above are engine-level.

Not covered, and needing a device or a human ear: that anything is audible at all
(no device is opened); timbre, tuning and instrument assignment (a wrong bank gives a
healthy RMS and a healthy beat grid); whether the 0.011 % of clamped samples is
audible; and realtime pacing, since headless synthesis runs on a fixed cadence so
underruns and DAC drift are invisible here.

## Final-lap music — `tests/check_final_lap_music.py`

```bash
python3 tests/check_final_lap_music.py --build build \
  --rom baserom.us.v80.z64
```

This finishes a real three-lap Time Trial with the closed-loop racer and proves
that the final-lap request crosses the complete native audio path. It requires
lap three, the same sequence changing from 126 to 141 BPM, a matching live
sequencer change from 476160 to about 425472 microseconds per quarter note, and
a beat grid at the new tempo in the captured PCM. The WAV is ROM-derived and
exists only in the test's temporary directory; no audio device is opened.

This deliberately checks both `music_tempo()` and `alCSPGetTempo()`. Updating
only the former makes the UI/debug value say 141 BPM while the sequencer keeps
playing at 126 BPM. Its broken-direction control assigns the old live tempo to
the final-lap PCM window and requires the beat-grid oracle to reject it.

## Accepted SDL sink evidence — `tests/check_audio_sink_evidence.py`

This short real-ROM engine route is the native bridge between deterministic
pre-sink PCM and the SDL application queue. It uses the test-only
`MDKR_TEST_HEADLESS_AUDIO=1` opt-in together with `SDL_AUDIODRIVER=dummy`, then
sets `MDKR_AUDIO_SINK_DUMP` and requires a valid, nonempty 22050 Hz stereo s16 WAV
whose payload and telemetry exactly match the blocks the native sink accepted
into its output ring.
The route must have zero rejected, dropped, or recovery-repaired blocks. It also runs
a control proving ordinary headless mode remains sinkless when the opt-in is absent.

```bash
python3 tests/check_audio_sink_evidence.py
python3 tests/check_audio_sink_evidence.py --control-no-opt-in
```

The dummy driver proves queue acceptance only. It does not qualify the device mixer,
DAC, speakers, hotplug, latency, or audible output; those remain manual physical
release-matrix work.

The deterministic capture now follows the same source-time contract as the fixed
simulation clock: original NTSC advances two source fields per game pass and emits
one 736-sample audio quantum, while enhanced one-field simulation services audio
only every second pass. `check_fixed_tick_schedules.py` additionally requires the
PCM to remain byte-identical under 3–6-field catch-up and suspension rebases;
`check_arbitrary_presentation_rates.py` requires the same identity across native
30–240 Hz, uncapped-like, and PAL-60 presentation schedules, plus Enhanced
forced-WebGPU original/30/45/60/120/uncapped schedules. A live device is still
required to qualify SDL or AudioWorklet underrun, backlog, and DAC-drift
behavior.

## Shipped callback/ring sink wiring — `tests/check_audio_resilience.py`

The `audio_resilience` CTest proves the *mechanism* — the lock-free SPSC ring in
`platform/audio_ring.c`, its underflow-concealing crossfade, and a latency
controller driven by a measured drain rather than an elapsed-time estimate — with
no ROM and no device. It cannot see whether the engine still uses any of it. A
revert to `SDL_QueueAudio`, or a deleted
`mdkr_audio_queue_controller_set_device_period()` or
`mdkr_audio_queue_controller_note_drain()` call, leaves every C unit green and
ships broken audio. `tests/check_audio_resilience.py` closes that gap by running
the real binary against the dummy driver and asserting on the four seams that
only exist when the wiring is intact: the device-open banner must report
`mode=callback` with a power-of-two ring of at least 8192 frames; the
`[AUDIO-RING]` shutdown summary must agree with that banner's period and
capacity, must never report an overflow or an evicted frame, and must never see a
callback ask for more than one period; the `[AUDIO-SINK]` controller row must
carry the *negotiated* `deviceperiod=`; and its `stalls=` must be zero, because
the stall guard exists only to bound an estimated drain and a headless run — which
free-runs far ahead of real time — trips it whenever the estimate is back. Frame
conservation ties it together: `pushed=` on the ring line must equal `samples=`
on the `[AUDIO-SERVICE]` line, so the ring is carrying exactly what the
synthesiser made and nothing else.

Underruns and concealments are deliberately *not* asserted here. Headless runs
outpace real time by a wide margin, so the ring stays deep and starvation never
occurs; a zero-underrun assertion would pass for the wrong reason. Starvation and
its crossfade stay with the deterministic CTest. The run needs both
`MDKR_TEST_HEADLESS_AUDIO=1` and `SDL_AUDIODRIVER=dummy` (the opt-in is refused
without the dummy driver) plus `MDKR_AUDIO_SERVICE_TRACE=1` for the controller and
service rows. A control arm re-runs with `MDKR_AUDIO=0`, which keeps synthesis but
opens no host device, and requires the banner and the ring summary to be absent —
proof that the detectors are reading the sink and would notice its removal.

```bash
python3 tests/check_audio_resilience.py --build build-rel --rom baserom.us.v80.z64
python3 tests/check_audio_resilience.py --no-controls
```

## Absolute output level — `tests/check_audio_level_reference.py` (RUN THIS ALONGSIDE `check_audio_output.py`, SAME TRIGGER PATHS)

`check_audio_output.py` is **scale-blind by construction**: it asserts a *floor* on
RMS and a *ceiling* on saturation, and its tempo, stereo and spectral-change
assertions are ratios and correlations that a flat gain leaves exactly unchanged.
`check_raw16_audio.py` compares two arms of the same build. `[EVTQ]` telemetry counts
events; the resource-plateau `voicePeak` law counts voices. So a **systematic loudness
bias** — one wrong shift in a gain stage, an extra `/2`, a master trim added "to stop
the clipping" — passed the entire suite. This check is the one that would not.

**It opens no audio device**, on the same terms as `check_audio_output.py`; the
capture is ROM-derived and deleted unless `--keep-audio` is given.

```bash
MDKR_AUDIO=0 python3 tests/check_audio_level_reference.py               # ~42 s
MDKR_AUDIO=0 python3 tests/check_audio_level_reference.py --control gain+3  # must exit 1
MDKR_AUDIO=0 python3 tests/check_audio_level_reference.py --control gain-3  # must exit 1
MDKR_AUDIO=0 python3 tests/check_audio_level_reference.py \
    --reference build/ares-oracle/console.raw     # opt-in console lane
```

Eight assertions over the same `race_drive_time_trial.txt` 4300-frame route, so a
level move lands in both bodies of evidence:

| # | asserted | measured | tolerance |
|---|---|---|---|
| 1 | 22050 Hz / 2 ch / 16-bit and the exact sample-frame count | 3 164 800 frames = 143.53 s | +-8192 frames |
| 2 | whole-capture RMS | 7457.9 = **-12.857 dBFS** | +-1.0 dB |
| 3 | per-channel RMS | L -12.857 / R -12.856 dBFS | +-1.2 dB |
| 4 | crest factor (peak - RMS) | **12.857 dB** | +-1.0 dB |
| 5 | saturation, WAV side and engine side | rails 0.07248 %, main-bus clip 0.07233 %, worst pre-clamp 35951 = +0.81 dBFS | ceilings |
| 6 | true peak, 4x oversampled | L **+1.002** / R **+1.478 dBFS** | +-2.0 dB |
| 7 | absolute per-band RMS, 8 bands | -17.949 / -19.357 / -21.587 / -21.877 / -22.934 / -24.898 / -27.279 / -32.869 dBFS | +-2.5 dB |
| 8 | per-slice RMS, fifteen 10 s slices | -21.790 … -10.362 dBFS | +-2.0 dB |

Assertion 4 is the one that keeps working when tolerances drift. The program already
touches full scale, so a build that got **louder** cannot raise its peak — it closes
the crest instead. Under the `+3 dB` engine control the sample peak is bit-identical
at 32768 while the crest falls to 10.058 dB.

Assertion 6 exists because sample peak is pinned at the rail and therefore carries no
information; the inter-sample overshoot is what a real reconstruction filter and every
downstream resampler actually produce, and it is measured, not assumed.

**Both directions, and both are engine-level, not analysis-level.**
`MDKR_AUDIO_TEST_GAIN_DB` scales the synthesised PCM inside `dkr_audio_service_tick()` —
before the engine's own RMS accounting, before the dump, before the sink — and
**refuses to act whenever a host output device is open**, so it is file-domain only
and can never make sound. With the variable unset the capture is byte-identical to a
build compiled without the seam (verified by `cmp`).

| arm | whole RMS | crest | rails | verdict |
|---|---|---|---|---|
| reference | -12.857 dBFS | 12.857 dB | 0.07248 % | PASS |
| `--control gain+3` | -10.058 dBFS (+2.799) | 10.058 dB | 1.21170 % | **FAIL, 28 assertions, exit 1** |
| `--control gain-3` | -15.857 dBFS (-3.000) | 12.857 dB | 0.00000 % | **FAIL, 28 assertions, exit 1** |

Four signal-level controls also run on every invocation (+-3.0 and +-1.5 dB applied to
the real capture in memory); the check **fails if any of them passes**. The +-1.5 dB
pair is there so the band cannot quietly become a rubber ruler that only catches gross
errors.

**The console lane (`--reference`) is opt-in and cannot be committed.** It compares
against the real ROM's own synthesiser output, captured from the audio-interface DMA
stream inside the instrumented ares of [`docs/ORACLE.md`](../docs/ORACLE.md)
(`MDKR64_ARES_AUDIO_DUMP`). When it is given, the port arm is re-run on the *same*
oracle route the console capture used, then envelope-aligned and compared. Measured on
`race_state_oracle`: **port/console +0.016 dB** over a 153.16 s aligned overlap. Full
numbers and the method's limits are in
[`docs/open-items/audio.md`](../docs/open-items/audio.md).

## RAW16 byte order and timbre — `tests/check_raw16_audio.py` (RUN THIS AFTER ANY RAW16, MIXER LOAD, ENDIAN-HELPER, OR AUDIO-BANK CHANGE)

`check_audio_output.py` passed on the broken build: a byte-reversed instrument can
still have healthy RMS, stereo, tempo, clipping, and reverb. This focused gate
checks the format boundary that the broad signal test cannot hear.

```bash
MDKR_AUDIO=0 python3 tests/check_raw16_audio.py --build build
MDKR_AUDIO=0 python3 tests/check_raw16_audio.py --build build-rel
MDKR_AUDIO=0 python3 tests/check_raw16_audio.py --build build-asan
```

It has three independent assertions:

1. Source classification requires exactly the three `alRaw16Pull()` loads to use
   the converted path and leaves `_decodeChunk()`'s one ADPCM byte-stream load
   ordinary.
2. A bounds-checked independent parser inventories the US/PAL v80 banks:
   233 unique music waves (208 ADPCM + **25 RAW16**) and 444 unique SFX waves
   (443 ADPCM + **1 RAW16**). The principal 18,432-byte bass sample is 27.34x
   rougher under the old byte interpretation.
3. One executable runs production `MDKR_RAW16=fixed` and exact
   `MDKR_RAW16=legacy`. Both arms must issue **9,441 loads / 676,240 bytes**,
   remain bit-identical before the first RAW16 output block, then diverge
   strongly afterward. Measured: first difference at sample-frame 1,499,298,
   17.68% changed post-boundary samples, difference RMS 1,280.5, and an **8.51x**
   worst-block legacy roughness ratio.

The legacy arm is load-bearing: if conversion disappears, both outputs become
identical and the check fails. Temporary PCM is ROM-derived and deleted unless
`--keep-audio` is explicitly passed; never commit it.

`tests/test_endian_utils.c` is the ROM-free companion. CTest covers aligned and
misaligned big-endian integer/f32 readers, `GE_SWAP*`, literal byte reversal,
PCM conversion, invalid arguments, and no-write-on-error behavior. A real
big-endian runtime has not been run.

Actual browser reachability is separate: `check_browser_runtime.py` requires the
AudioWorklet route to report `mode=fixed` with nonzero RAW16 loads and bytes.

## Vehicle engine audio — `tests/check_vehicle_audio.py` (RUN THIS AFTER VEHICLE SOUND, ASSET_AUDIO, OR ENGINE-PITCH CHANGES)

The broad PCM gate cannot prove which loop is playing or whether its pitch
tracks throttle and speed. This focused gate independently decodes the selected
`VehicleSoundAsset` row from the retail ROM, drives production car, hovercraft,
and plane routes, and reads the opt-in `MDKR_VEHICLE_AUDIO_TRACE` witness after
the sound handles have been updated.

```bash
MDKR_AUDIO=0 python3 tests/check_vehicle_audio.py --build build
```

It requires each runtime sound ID to equal the ROM's big-endian value, changing
base pitch, and an active main engine handle. The car arm additionally requires
sustained driving intensity, throttle-pitch ramp, and both sides of the
idle/main crossfade. The original defect
fails five independent assertions: sound ID 115 became 29440, the main handle
never opened, and intensity, throttle pitch, and base pitch remained frozen.
`vehicle_audio_contract` is its millisecond, ROM-free CTest companion: it checks
the complete 0x4C mixed-field swap map, byte-field preservation, and
transactional rejection of short records. `runtime_contracts` exhaustively
checks the shared vehicle-model mapping used by both initialization and update.

## Line-particle ribbon orientation — `tests/check_line_particle_orientation.py`

A line particle is a ribbon laid along the emitter's path, spread either side of
it by a half-width offset on **one** of the parent's local axes. Which axis is
authored per particle descriptor (`lineOrientation`: 0 → local X, 1 → Y, 2 → Z),
and a plane's wing contrail is authored 0 — flat across the wings. This check
flies Hot Top Volcano in a plane under `MDKR_AUTOPILOT`, reads the ribbon basis
out of `MDKR_LINE_PARTICLE_TRACE=1`'s `[LINEPART]` rows, and requires every
emitted ribbon to spread along local X.

```bash
MDKR_AUDIO=0 python3 tests/check_line_particle_orientation.py -v   # ~1 min, muted + headless
```

It asserts the **geometry** (the local offset vector actually used to build the
vertices), not the orientation number that selected it, so an orientation value
that stopped reaching the vertex maths cannot pass. The world-space vector is
reported but never asserted — it is the local vector after the plane's own
pitch/roll, which is exactly why the local one is the roll-independent
statement. Two guards keep it honest: an in-process positive control replays the
measured pre-fix ribbon (orientation 1, local `(0, 2, 0)`) and must see it
rejected, and a non-vacuity floor requires at least 100 ribbons, so a route that
stopped emitting trails fails instead of passing silently.

The defect it closes is a bit-order one, not a byte-order one, and byte-swapping
cannot reach it: `lineOrientation` is a 6-bit C bitfield in the descriptor's
`0x0A` halfword, which the N64's compiler packs from the most significant bit
down and a little-endian host packs from the least significant bit up. Read
LSB-first, the four padding bits of the stock value `0x0001` became orientation
1, and every wing contrail in the game was built as a vertical sheet — a
hairline seen edge-on from the chase camera instead of a broad flat band. Read
the derivation next to the `PARTICLE_DESC_*` accessors in `game/src/particles.h`
before adding any new field to a ROM-sourced struct.

## Texture "interlace" check — `tests/check_texture_lineswap.py` (RUN THIS AFTER ANY TEXTURE-DECODE CHANGE)

About **30 %** of every texture DKR uploads is *pre-swizzled*: its odd rows have the
two halves of each 64-bit word exchanged in ROM, because it is loaded with one of
libultra's `...S` macros (`LOADBLOCK` with `dxt = 0`) and the RDP's texture fetch
would have undone the exchange. The HLE has no TMEM, so it has to undo the
exchange itself
(`unswap_row()` in `platform/fast3d/gfx_pc_dkr.c`). Getting it wrong is **silent**:
nothing crashes, every fixture stays green, and the image is "mostly right, just
kind of scrambled" — the minimap becomes a field of dashes, sprites get comb edges.

```bash
MDKR_AUDIO=0 python3 tests/check_texture_lineswap.py -v      # ~2 min, muted + headless
```

It renders `race_drive_long` (3900 frames) **twice from the same binary**, with
`--pure` explicit in both arms — normally and with `MDKR_LINESWAP=off`, which
reproduces the pre-fix decode byte-for-byte — and measures the artifact itself:
at texel-row spacing `step = width/320`,
`parity = mean|I(y)−I(y+step)| / mean|I(y)−I(y+2·step)|`. A clean image scores
below 1, a swizzled one above. Scored over the changed-pixel mask and over the
minimap ROI. The fixed ROI has known texel-row spacing and retains the absolute
broken-arm threshold. The dynamic mask also contains minified 3D surfaces where
one framebuffer row is not one source texel row, so it requires a material
changed-pixel count, clean-arm ceiling, and improvement ratio instead. Measured:
changed mask `off` 1.01–1.58, normal 0.61–0.73 (1.52–2.60x); minimap ROI `off`
1.46–1.58, normal 0.62–0.69 (2.12–2.50x).

Because the broken arm is re-rendered every run, the check **cannot pass
vacuously** — a build that stops un-swizzling makes both arms identical, and the
changed-pixel count, fixed-ROI absolute threshold, and improvement assertions
fail. Verified in both directions
(see docs/OPEN_ITEMS.md "wave lineswap" for the pasted output).

`MDKR_LINESWAP=off` is also the way to A/B the decoder by eye, in the same style as
`MDKR_NEARCLIP=off`. The `[TEX] lineSwappedUploads=N` line printed at headless exit
is how you confirm a route reaches the path at all (0 = the route loads no
pre-swizzled texture, so any assertion on it would be vacuous).

The ROM-free `texture_cache_identity` CTest protects the decoder/cache boundary.
It requires the cache key to include the resolved source-row pitch and loaded
span as well as palette, font derivation, mipmap, and cutout policy, and requires
the lookup and insertion paths to use that complete key. This prevents a
same-address tile reinterpretation or first-use mip policy from silently binding
pixels uploaded for a different material.

## Locked doors block — `tests/check_door_blocks.py` (RUN THIS AFTER ANY CHANGE TO OBJECT-MODEL COLLISION OR `obj_loop_door` / `obj_loop_exit`)

```bash
MDKR_AUDIO=0 python3 tests/check_door_blocks.py -v            # ~6 min, two arms
```

Asserts that a hub door the player has not earned is **physically solid**. Before
wave "objcoll" it was not: `func_80017A18()` — the per-facet object-model collision
test, and the only non-NULL writer of `collisionData->collidedObj` in the game —
linked to a `return 0` stub, so every collision-meshed object was intangible. With
zero balloons the kart drove through a shut door and `obj_loop_exit()` (which has
no door check, by design) warped it into the Dino Domain lobby at frame ~6731 and
Ancient Lake at ~7017.

The route approaches the two hub door leaves on their center line before aiming
at the exit. This is load-bearing: the older diagonal advloop reproducer can
latch the exit from a near-side corner without touching the door mesh at all.

**Why it has two arms.** The failure mode is *silence* — you drive through, nothing
crashes, nothing is logged — so running only the fixed build proves nothing
(CONTRIBUTING.md rule 2). `MDKR_OBJCOLL=legacy` restores the stub's `return 0`, so
one binary drives both sides, the same trick as `MDKR_COLLTEX=legacy`:

| arm | expected |
|---|---|
| default | reaches Timber's Island (levelId 0), **not** 12 or 5; `[OBJCOLL]` hits > 0 |
| `MDKR_OBJCOLL=legacy` | reaches 12 **and** 5; `[OBJCOLL]` hits == 0 |

The fixed arm asserts a negative, which only means something beside the legacy
arm's positive. It also asserts the hub *is* reached and that hits are non-zero, so
"blocked" cannot be satisfied by a route that broke early or never touched a door.

`[OBJCOLL] objectmodel_collision_hits=N` at headless exit is the reachability
number: about 1742 on this centered-door route, **9** on boss track 38, 0 on
tracks 5/32/15. Object
collision is overwhelmingly a hub-world phenomenon, which is why the pre-fix
measurement (taken only on race tracks) made the gap look 100x smaller than it was.

## Door numerals stay per door — `tests/check_door_glyphs.py`

```bash
python3 tests/check_door_glyphs.py -v
python3 tests/check_door_glyphs.py --rate 240 -v
python3 tests/check_door_glyphs.py --rate uncapped -v
```

The four ordinary Dino Domain race doors share one cached `ObjectModel` but require
different numbers of balloons: Ancient Lake 1, Fossil Canyon 2, Jungle Falls 3,
and Hot Top Volcano 5. Golden Balloon 1.0.1 moved `obj_door_number()` out of the
per-object display-list build and into a view-dependent fixed-tick traversal.
That function writes `TriangleBatchInfo.texOffset` on the shared model, so the
last admitted door supplied its digit to every visible door. A deterministic
WebGPU/Original capture rendered all three front signs as 5 at frame 6820 and as
2 at frame 6880 even though the four `Object_Door.balloonCount` values remained
1/2/3/5.

This gate enters the real lobby on a fresh Adventure save and records the texture
offset selected at the material/display-list boundary. Both GL and WebGPU must
submit all four differently numbered doors in one frame, keep every authored
door/count pair stable, and choose `balloonCount * 4` independently for each
one-digit atlas entry. The trace also exposes the cached batch's shared stored
offset. At least one stored/required mismatch is mandatory: that counterfactual
proves the test would detect the old shared-state path instead of merely
confirming that the route reached the lobby. In Original presentation mode the gate also
captures frame 6820 from both native renderers and compares the three door faces.
This final-pixel check catches the separate OpenGL sampler-cache defect where a
new texture object could skip wrap/filter setup and repeat its digit across the
wood. It rejects blank scene output, requires three visually distinct central
glyphs, and synthesizes blank, common-glyph, and repeated-sampler fail-red
controls from the live capture. The fixed GL capture agrees with WebGPU within
normal rasterization noise.

The ROM-free `object_material_ownership` CTest protects the corresponding
source boundary. It rejects any native exposure of the legacy mutating door
API, requires door and racer material selection to remain draw-local, and
requires OpenGL's sampler memoization to include texture-object identity. This
keeps the failure class closed even in lanes that cannot run the gameplay ROM.

## Collision grid mask / boss race — `tests/check_collision_gridmask.py` (RUN THIS AFTER ANY COLLISION OR BOSS-FLOW CHANGE)

**The only check that races a BOSS level.** The 20 ids in `check_track_sweep.py`
come from `ASSET_MISC_MAIN_TRACKS_IDS`; the ten boss levels live in a separate ROM
table, `ASSET_MISC_BOSS_TRACKS_IDS` (`tools/dump_misc_asset.py` sub-asset 30) =
**38, 46, 40, 53, 1, 52, 41, 54, 37, 55**, so no earlier sweep touched them.

```bash
MDKR_AUDIO=0 python3 tests/check_collision_gridmask.py -v      # ~4 min
MDKR_AUDIO=0 python3 tests/check_collision_gridmask.py --skip-control --skip-win
```

`compute_grid_overlap_mask()`'s Z-row test compared against a value the clamping
above had already forced, making the Z half of the collision pre-filter a no-op.
That is **silent**: an over-permissive pre-filter still gives correct collisions,
only far too many candidates — until the 500-entry `gCollisionCandidates` list
saturates and `generate_collision_candidates()` **truncates**, at which point the
facets a racer is standing on can be in the discarded tail and the racer falls out
of the level. On Tricky's volcano spiral (levels 38 and 46) it did: the original
level-38 measurement saw the boss lose the ground for 301 frames and the player
for 103, and **the first boss race could not be finished at all**. Full write-up
in `docs/OPEN_ITEMS.md` "wave gridmask".

That sentence records the historical pre-fix/gridmask fixture. The current legal
campaign route is closed separately by `check_first_boss_progression.py`, whose
checkpoint-36 steering recovery physically reaches the narrow finish in both
win and loss arms. The unrecovered stock-AI miss remains its required failing
control.

Four runs from one binary. `MDKR_GRIDMASK=off` restores the pre-fix tautology, so
the broken arm is re-produced on every run and must *exhibit* the defect before the
fixed arm is credited — the check cannot pass vacuously, and it fails closed if
either probe disappears. Reference:

| arm | maxCandidates | truncated | longest ground loss | peak y | `fin=` |
|---|---|---|---|---|---|
| `MDKR_GRIDMASK=off` | 500 (cap) | 36 | 51 (boss) / 2 (human) | 7 | 0 |
| fixed | 270 | 0 | 18 (boss) / 27 (human) | 4868 | 1 |

The integrated fixture is level 46: restoring object-model collision moved the
level-38 autopilot line 2.5 units and it no longer reaches the summit, while level
46 still reproduces the broken grid mask and finishes with production collision.
A racer counts as falling only when its ground-loss run exceeds the **fixed arm's
own ceiling** (`MAX_FIXED_AIRBORNE`) and at least doubles that same racer's
fixed-arm run. Requiring both racers encoded one trajectory; saturation,
truncation, a paired fall, and broken-only failure to finish carry the defect.

Plus a **byte-identity negative control** on Ancient Lake: where nothing truncates
the two arms' 3857 `[PACE]` racer rows must be identical, so the fix provably does
not perturb anything it should not (measured identical on all 30 tracks), and a
**win arm** via `MDKR_BOSS_WIN=1`.

Two new trace probes and two new hooks, all no-ops unless set:

- `[COLL] maxCandidates=N truncated=N cap=500` at headless exit — the mechanism.
- `[GRND] frame=N pi=P ri=R gw=K surf=a,b,c,d xrot=X zrot=Z` per racer per frame.
  **Per racer is load-bearing**: the decisive observation was that the AI boss falls
  through even when the player does not, so a player-only measurement can miss it.
  `xrot`/`zrot` are the tilt — during the fall the roll freezes at 75 and never
  recovers, which is what "it tilts and never tilts back" was.
- `MDKR_GRIDMASK=off|legacy` — A/B the fix (same contract as `MDKR_NEARCLIP=off`).
- `MDKR_BOSS_WIN=1` — write `racer->finishPosition = 1` once the race has finished,
  so the **human wins** with its racing line untouched (platform/mdkr_adventure.c).
  It replaced `MDKR_BOSS_SLOW=1`, which scaled the boss's velocity to 0.15×: because
  DKR's AI paths relative to the field, that moved the *human's* line too, and with
  the ROM-faithful math it moved it off the Fire Mountain summit — 84 frames with all
  four wheels off the ground, no finish, and `racer_boss_finish()` unreachable at any
  budget. Reach an outcome by writing the field the code branches on, not by
  perturbing the physics until the branch happens to be taken.
  A boss race has two racers and `MDKR_AUTOPILOT` drives the human with the boss's
  own AI, so the human always comes second and `racer_boss_finish()`'s
  `finishPosition == 1` arm was unreachable from any headless route — the same
  "unreachable by construction" shape as the Time-Trial record write. With it, the
  boss verdict is asserted **both ways**: `finishPos=2` → cutscene 5 (lose),
  `finishPos=1` → cutscene 4 (win). That is what makes "I came first and it told me
  I lost" falsifiable.

It also asserts the **boss cutscene flow**: on a fresh `save/eeprom.bin` the first
entry must select cutscene 3 (the challenge — Tricky's *"Now I challenge you to a
RACE!"*), not 5 (lose), and no cutscene-5 load may precede the race load. The check
deletes `save/eeprom.bin` before each run and after the last; with a save present
the boss entry is a *revisit*, which plays no challenge cutscene at all and would
make those assertions vacuous.

⚠️ Do **not** add an upper bound on the broken arm's peak y. One was written and
removed: it encodes where the AI line happens to fall off, and any change that
shifts the racing line (e.g. a longer pre-race cutscene) makes the broken arm reach
the summit first and fail the bound while still fully exhibiting the defect. See the
note in `docs/OPEN_ITEMS.md`.

## Collision candidate headroom — `tests/check_collision_headroom.py` (RUN THIS AFTER ANY CHANGE TO `generate_collision_candidates` OR LEVEL GEOMETRY)

The `j >= cap` pre-check guard added at both
insert sites in `generate_collision_candidates()` (wave "boundsweep",
`docs/open-items/collision.md`) stops the out-of-bounds write, but a saturated
500-entry `gCollisionCandidates` list still silently **drops** every candidate
past the cap — the same mechanism that dropped racers through Tricky's volcano
before wave "gridmask". Boss levels 41 and 54 measure 416 of 500, only 84 slots
of margin. This item is instrument-and-gate only: it does **not** raise the
cap — the layout/perf effects of a larger allocation are unmeasured.

```bash
MDKR_AUDIO=0 python3 tests/check_collision_headroom.py -v      # ~3 min
```

Three things, in order:

1. **Guard presence and ORDER** — a static source check that the
   `j >= mdkr_coll_cap(MAX_COLLISION_CANDIDATES)) { goto out; }` text is still
   present at both insert sites in `game/src/hasm/collision.c` (the segment
   insert and the facet insert), *and* that each guard sits above the store it
   guards. If wave "boundsweep"'s fix ever regresses to the ROM's bare
   `j == MAX_COLLISION_CANDIDATES` equality test, this fails before anything
   else runs. Presence alone is not the property: the facet insert once carried
   its pre-check *below* its own store, which lets the segment insert hand it
   `j == cap` and puts one element at index `cap`.
2. **Per-level sweep** — one `MDKR_AUTOPILOT` run per level, all ten boss
   levels (`ASSET_MISC_BOSS_TRACKS_IDS` = 38, 46, 40, 53, 1, 52, 41, 54, 37, 55)
   at 13000 frames plus Ancient Lake (track 5, an ordinary race) at 6500,
   reading the `[COLL] maxCandidates=N truncated=N cap=N` line every route
   already emits (`platform/platform_sdl_min.c`). Fails if `truncated != 0`
   (saturation must never happen in normal play) or if a level's
   `maxCandidates` exceeds a frozen baseline ceiling (measured peak + 16 slots
   of slack) — the regression tripwire for headroom quietly shrinking.
3. **Positive control** — `MDKR_COLLCAP=150` on boss level 41's own route (same
   track, script and frame budget as its row in the sweep, cap alone lowered
   from 500 to 150, well under its natural 416 peak). Asserts the guard still
   holds under a forced boundary (`maxCandidates` never exceeds the lowered
   cap — the fix from step 1 is doing its job, not merely present as text) and
   that the run truncates, then evaluates the SAME "truncated must be 0" rule
   step 2 applies against this forced arm and requires it to report a
   failure — proof the sweep's assertion is not vacuous.
4. **Allocation-lowering control** — `MDKR_COLLALLOC=1 MDKR_COLLCAP=150` on the
   same route. `MDKR_COLLCAP` alone moves the boundary *inside* a full-size
   500-entry allocation, so a store at index `cap` is a real element of a real
   array and nothing reports it — which is why the boundary control ran green
   while the facet insert's guard sat below its store. `MDKR_COLLALLOC=1` sizes
   the allocation to the effective cap plus one canary element and arms the
   canary at index `cap`: the first slot a missing or mis-ordered guard writes,
   and one no correct path can reach. `[COLL] canary=` must be 0 (`-1` means
   unarmed, which is itself a failure in this arm). Verified to discriminate:
   `canary=1` on a build with the facet guard moved back below its store.

Measured baseline (this binary, `MDKR_AUTOPILOT`, one run per level):

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
| 55 | 13000 | 92 | 0 | 500 | 408 |
| 5 (Ancient Lake, ordinary race) | 6500 | 30 | 0 | 500 | 470 |

Levels 41 and 54 remain the tightest margins in the game, unchanged from wave
"boundsweep"'s original measurement — this check exists so that stops being true
silently. `[COLPEAK] candidates new peak N of M` (`platform/stubs_dkr.c`
`mdkr_coll_candidates()`, `MDKR_TRACE`-gated, same pattern as `[EVTQ]`'s
per-queue peak telemetry in `platform/audio_event_queue.c`) prints each time a
run's high-water mark advances, for tracing *when* in a route the peak moves;
the sweep above reads the unconditional `[COLL]` summary line instead, which
needs no `MDKR_TRACE` and is what every other collision check already parses.

## One-shot cutscene latch — `tests/check_key_cutscene_once.py` (RUN THIS AFTER ANY CHANGE TO `cutsceneFlags`, `level_load()` OR `DKR_SHL32`)

```bash
MDKR_AUDIO=0 python3 tests/check_key_cutscene_once.py --build build-rel -v   # ~4 min
```

**This one needs an OPTIMISED build and will not detect its own defect without
one.** Configure it once:

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j8
```

Reported from the browser as *"I collected the Key on the first race, and now the
key animation is playing after EVERY race."* The world-key cutscene's "already
shown" latch is `CUTSCENE_DINO_DOMAIN_KEY << (world + 31)` — a shift count of 32..35,
which is what MIPS `sllv` masking means and what C calls undefined behaviour. clang
folds it to 0 at `-O2` and deletes the load, the test and the store, so the gate
always fires and nothing is ever recorded. **The native build defaults to Debug and
the web build to Release**, so the same source is correct natively and broken in the
browser. See `docs/OPEN_ITEMS.md` wave "keyshift".

The check injects a save with the Dino Domain key collected and drives
`adventure_race_loop.txt` with `check_adventure_race_loop.py`'s closed-loop route.
It asserts the route really ran (6808 in-race rows, 62286.9 units — so nothing else
can pass vacuously), that the key gate is evaluated on both entries to the world
hub, that the latch mask is non-zero, that the second evaluation sees the latch
**set**, that the return leg contains exactly one lobby load rather than two, and
that the latch is present in `save/eeprom.bin` afterwards.

To exercise the control, build the reverted form and assert it fails:

```bash
cmake -S . -B build-relctl -DCMAKE_BUILD_TYPE=Release \
      -DMDKR_EXTRA_C_FLAGS=-DMDKR_SHL32_CONTROL && cmake --build build-relctl -j8
python3 tests/check_key_cutscene_once.py --build build-relctl --expect-fail   # must PASS
```

Measured: 4 of the 6 assertions fail on that build while both coverage assertions
still pass. The same build at `-DCMAKE_BUILD_TYPE=Debug` **passes**, which is the
whole point of the wave.

The class detector is separate and mechanical — UBSan `-fsanitize=shift-exponent`
over routes that cross a world hub, Timber's Island at the Taj balloon threshold, a
water track and a Tracks race. With the fix in place it reports 0 sites; reverted it
reports 6 (`game.c:528/532/535`, `game.c:629`, `objects.c:1822`, `waves.c:2321`).
`waves.c:2321` is hit by every route, so it is the sentinel that proves the
instrument is alive.

## Boss win verdict — `tests/check_boss_win_verdict.py` (RUN THIS AFTER ANY CHANGE THAT WRITES `courseFlagsPtr` OR `settings->bosses`)

```bash
MDKR_AUDIO=0 python3 tests/check_boss_win_verdict.py -v              # ~3 min
MDKR_AUDIO=0 python3 tests/check_boss_win_verdict.py --break-invariant   # must FAIL
```

`racer_boss_finish()` (`game/src/vehicle_tricky.c:360`) decides between "play the
win cutscene, award the amulet, write the save" and "just transition back" on
**one bit** — `courseFlagsPtr[courseId] & RACE_CLEARED`. The already-cleared arm
pushes nothing onto the level-property stack, so *no cutscene level is ever
queued*, nothing is awarded and the save is not written; the player is returned to
the world lobby standing at the boss door. That is the ROM's own re-race path and
it is **indistinguishable from a broken first win**.

RACE_CLEARED on a boss course has exactly one legitimate writer: that same
function's win branch, which sets it together with `settings->bosses |= 1 <<
worldId`. Both fields are otherwise written *by index* from unrelated subsystems
(golden balloons, doors and triggers all do `courseFlagsPtr[<index>] |= ...`; the
save packs `bosses` as 12 loose bits), so **one stray index silently kills every
first boss win** — no crash, no log. This check pins that invariant:

| arm | `bosses` / `courseFlags` at the verdict | outcome |
|---|---|---|
| `first` (fresh save) | `0x0` / `0x40001` | cutscene 4 loads @9139, cutscene 5 never loads |
| `cleared` (`MDKR_BOSS_PRECLEARED=38`) | `0x2` / `0x40003` | **no cutscene at all**, overworld @8432 |
| `lose` (`MDKR_BOSS_WIN` unset) | `0x0` / `0x40001` | cutscene **5** loads — the failure animation |

The `cleared` arm is the **positive control**: it reproduces the reported
*"I won the first boss race, got no win cutscene, no amulet, and was dropped back
in front of the boss door"* from an unmodified binary, which is what makes the
`first` arm's invariant assertions falsifiable rather than decorative.
`--break-invariant` runs the regression itself — it pre-clears the boss course in
the `first` arm too — and the check must then fail.

**The other half of the report.** The player wrote *"I was awarded first place,
still got the failure animation"*. Those are two separate wrong presentations —
cutscene 4 missing, and cutscene 5 appearing — and only the first used to be
asserted, so a build that pushed **both** on the same finish passed. The `first`
arm now also requires that cutscene 5 (`level_properties_push(i, 5, -1, 5)`, the
`finishPos != 1` limb) never loads. The `lose` arm is that assertion's
**non-vacuity control**: with `MDKR_BOSS_WIN` unset the autopilot finishes where
it naturally does and DKR takes its own losing limb, so the harness is shown
producing the exact `level_load: levelId=57 … entrance=5 … cutscene=5` line the
negative assertion reads. Without it, "cutscene 5 never loaded" would be a claim
about a log line nothing had ever been seen to emit.

Two hooks, both no-ops unless set (`platform/mdkr_adventure.c`):

- `MDKR_WATCH_COURSEFLAGS=<levelId>` — one `[BOSSW] frame=N courseFlags[L]=0x… (was
  0x…) bosses=0x… (was 0x…) courseId=… worldId=…` line per observed **change**.
  It **polls at the frame boundary** rather than hooking each writer, deliberately:
  that is what makes it catch a write through a *wrong* index. Low bits are the
  race flags (1 visited, 2 cleared, 4 silver coins), bits 16+ are per-level
  balloon/door/trigger flags.
- `MDKR_BOSS_PRECLEARED=<levelId>` — hold that boss course at "already beaten"
  (`RACE_VISITED|RACE_CLEARED` plus the world's `bosses` bit), i.e. the state a
  save carries once the boss has been beaten once. Re-applied every frame because
  FILE SELECT reallocates `Settings` and `clear_game_progress()` would wipe it.

## CPU-opponent stuck recovery — `tests/check_ai_unstick_opponents.py`

```bash
python3 tests/check_ai_unstick_opponents.py --build build -v      # ~2 min, 12 races
python3 tests/check_ai_unstick_opponents.py --self-test           # reducer control, instant
```

`racer_AI_pathing_inputs()` arms a 120-unit cooldown (`unk215`) after every AI
stuck-recovery, and that cooldown decays on exactly one condition —
`unk214 == 0 && racer->velocity < -0.5`, i.e. the reverse window has expired
**and** the kart is moving. Nothing else in `racer.c` reads or writes `unk215`,
so a kart that comes out of the reverse window still unable to move can never
clear the cooldown and can never re-arm the recovery. Measured on the
autopiloted **human** at Hot Top Volcano's crater: 18,677 motionless frames
(`docs/open-items/gameplay.md`). Whether the same wedge is reachable for a
genuine **opponent** decides that item's classification, because
`mdkr_autopilot_unstick()` is reachable only from the human-autopilot limb — a
real CPU racer has no recovery at all.

This gate measures it. `MDKR_AI_STUCK_TRACE=<stride>` (`game/src/racer.c`, inert
unless set) installs an observation-only witness in `update_AI_racer()` — the
limb genuine opponents actually take; `update_player_racer()`'s
`racer_AI_pathing_inputs()` call is inside the *non*-computer branch and is
reached only by a human kart the finish hand-off has relabelled — and it refuses
any racer index ever seen carrying a real player index. It maintains two
per-opponent counters in update-rate units: `stall` (cooldown armed while
`|velocity| < 0.5`, the wedge signature) and `nodecay` (cooldown armed while the
decay branch's own condition is false, which legitimately covers the reverse
window), plus `arm`/`clear` events bracketing every episode.

12 races on Hot Top Volcano, each with a distinct boot RNG seed
(`MDKR_RNGSEED=0x<hex>`, `platform/math_util_native.c` — nothing re-seeds at
boot, so each value is an independent random stream for the whole run; measured
divergence is total within one lap). The check **fails closed**: zero cooldown
episodes across all races means the witness stopped reporting, not that the
game got better.

| window | value | measured legitimate maximum |
|---|---|---|
| `WEDGE_WINDOW` (`stall`) | 900 | 229 update units |
| `NODECAY_WINDOW` (`nodecay`) | 2400 | 729 update units |

Both come from a 32-race, 32-seed measurement pass that produced 23 legitimate
stuck-then-recover episodes on genuine opponents — 23 `arm`, 23 `clear`, none
left armed. The 229 is the shape worth knowing: opponent 6 left the track at
`(-96.1, -987.7, 2357.9)`, all wheels off a surface, velocity pinned at `0.472`
(below the 0.5 the decay branch needs), cooldown at 120 with the reverse window
already expired — the wedge exactly — for 208 update units, and then DKR's own
**out-of-bounds respawn** put it back on the track and the cooldown decayed
`120 → 114 → 84 → 54 → 24 → 0` in six samples of ordinary driving. That respawn
is the opponent's recovery, and it is why the same track wedges the autopilot
(which lands *inside* the world, grounded, so respawn never fires) and not the
field. A genuine deadlock is unbounded, so 4× the legitimate maximum separates
the two without being calibrated on any racing line.

`--self-test` feeds synthetic telemetry in the engine's exact row shape through
the same reducer and requires the wedge to be reported and the measured
229-unit episode not to be. The engine-side witness is proven live by the
episode census a passing run prints.

**If this check ever fails, do not change gameplay to make it pass.**
`racer_AI_pathing_inputs()` is matching decompiled code with no `GLOBAL_ASM` and
no `NON_MATCHING`; preservation-port-versus-fix is an owner decision and the open
item exists to hold it. Report the telemetry.

This gate deliberately does **not** export `MDKR_TRACE` — that variable arms
engine behaviour as well as printing (see the shadow-gate note under
`check_world_shadows.py`), so a measurement taken with it set describes a
different program.

## Bluey 2 rematch — `tests/check_bluey2_rematch.py`

```bash
python3 tests/check_bluey2_rematch.py --build build --rom /path/to/owned-us-v11.z64
```

This is the standing regression for the reported faster Bluey rematch. It
builds a checksum-valid checkpoint with Bluey 1 and the four Snowflake silver
races complete, marks Bluey 2 visited, resumes the file, and traverses the
production hub/lobby route before retargeting the final race load to course 52.
No first-visit cutscene redirect is allowed. Bluey must naturally finish first
at checkpoint 24/lap 2, and the human's production boss verdict must be a loss.

Original two-field simulation is the product contract. Enhanced one-field
simulation is the fail-red control: on the reference build Original finishes at
3,518 logic ticks while Enhanced finishes at 3,019 with 1.1696x mean object
speed. The check requires a materially earlier/faster Enhanced result, then
substitutes the Original result for that control and verifies that its own
sensitivity assertions reject the pair.

The Original arm also dumps headless PCM to its temporary directory. It pins
the authored sequence-30 stop and sequence-57 race-cue start (3.204 seconds
apart, with the race cue 0.968 seconds after GO) while requiring every 250 ms
window around GO to remain active. This distinguishes the original game's cue
transition from an audio-device or mixer dropout. Full release/Ares measurements
and the visible-timer explanation are in
[`docs/BLUEY2_PARITY.md`](../docs/BLUEY2_PARITY.md).

## First boss campaign progression — `tests/check_first_boss_progression.py`

```bash
MDKR_AUDIO=0 python3 tests/check_first_boss_progression.py -v
MDKR_AUDIO=0 python3 tests/check_first_boss_progression.py --break-invariant  # must FAIL
```

This is the legal Adventure route the isolated boss verdict check does not cover.
It starts from a checksum-valid checkpoint with three Dino races cleared, three
local balloons, and two actual Timber's Island balloon bits. It then drives the
production chain `Timber's Island -> Dino lobby -> Hot Top Volcano -> Dino lobby
-> Tricky 1`, requiring the fourth race's three laps, the four-local-balloon boss
door, and first-visit challenge channel 3.

Production object collision legitimately nudges Tricky 1's stock human AI line
2.5 units thousands of frames before the summit. The line reaches checkpoint 37
but misses the narrow finish extent. `MDKR_BOSS_ROUTE=1` leaves that whole line
alone through checkpoint 35, then supplies steering only toward a point beyond
the finish. It does not move the kart or write lap, `raceFinished`, verdict, boss
or save state.

The check runs three arms:

| arm | required result |
|---|---|
| win | physical finish first; only then `MDKR_BOSS_WIN` changes place 2 -> 1 with `raceFinished=1`; cutscene 4 |
| loss | the identical recovered route naturally finishes second; cutscene 5 |
| broken-route control | `MDKR_BOSS_WIN=1`, recovery absent: checkpoint 37, no `bossfinish`, no force, no win cutscene |

Both campaign arms return to the Dino hub, write checksum-valid EEPROM, and are
loaded in a second process. Win persists Hot Top and Tricky cleared plus Dino's
boss bit; loss persists Hot Top cleared and Tricky visited only. Both retain
zero TT/Wizpig amulet pieces: the first world boss awards a balloon and boss
clear, while the amulet piece belongs to the second boss.

## Campaign progression — `tests/check_campaign_progression.py`

```bash
MDKR_AUDIO=0 python3 tests/check_campaign_progression.py -v          # ~11 min
MDKR_AUDIO=0 python3 tests/check_campaign_progression.py --quick     # ~50 s
MDKR_AUDIO=0 python3 tests/check_campaign_progression.py --quick --break-invariant  # must FAIL
```

`--break-invariant` drives seam A on the stock AI line instead. Measured, that
line takes 6 of the 8 coins, so nothing is written and six assertions go red —
which is what keeps the silver-coin arm from being satisfied by a race that
merely happened.

This is where the campaign stops being ungated. It picks up from the state the
first-boss check leaves behind and gates each later progression **seam** with a
save fixture entering it and an assertion on what production writes leaving it,
so the fixtures compose into a witnessed campaign. Which courses belong to which
world is read out of the ROM's own level headers, never from the level names —
they disagree, and a fixture that guesses silently exercises nothing.

| Seam | What it drives | What it must write |
|---|---|---|
| A | Ancient Lake entered through the real hub and lobby, all eight silver coins collected by the game's own coin objects | `RACE_CLEARED_SILVER_COINS` written once, status 2 -> 3 in EEPROM, `balloons` (6,4) -> (7,5), reloaded by a second process, no amulet |
| B | all four worlds' second boss races, won, each on the EEPROM the previous one persisted | the world's `1 << (world + 6)` bit, `wizpigAmulet` +1 exactly (1, 2, 3, 4 across the chain), one amulet cutscene, boss course cleared, and no carried bit dropped |
| B control | the same race and the same win from a save whose first boss was never beaten | the FIRST-boss bit, **no** rematch bit, **no** amulet, no amulet cutscene |
| B driven | the Dino Domain rematch entered by driving the human through the lobby's own boss door, not by `MDKR_LOAD_TRACK` | the same state as the retarget arm, plus `WARP_BOSS_REMATCH` live at eight world balloons with `WARP_BOSS_FIRST` disabled, and the lobby's exit actually taken |
| C | the carried four-piece save loaded into Timber's Island, then Wizpig 1 raced | the hub redirected to `WIZPIGMOUTHSEQUENCE` with `CUTSCENE_WIZPIG_FACE` latched and persisted; then Wizpig 1's central-area boss bit and cleared course |
| C control | the same hub load one rematch earlier, at three pieces | **no** redirect and **no** `CUTSCENE_WIZPIG_FACE` |
| T | all four trophy championships, chained on one another's saves through `check_trophy_series.py`'s own driver | `trophies` 0x3 → 0xf → 0x3f → 0xff, each step matching production's own `trophyaward:` line, and nothing else in the save moved |
| T.T. | the four challenge courses won in four separate processes on one another's saves | `ttAmulet` 1, 2, 3, 4, one `TTAMULETSEQUENCE` each, each course cleared |
| T.T. control | the FIRST challenge replayed on the save it produced | **no** piece and **no** cutscene — the award is gated on the course not already being cleared |
| E | Wizpig 2, won, and the post-win cutscene stack driven to the credits | `bosses & 0x20` live and persisted, `MENU_CREDITS` reached, and `menu_credits_init` choosing "TO BE CONTINUED …" + `SEQUENCE_CRESCENT_ISLAND` with `gViewingCreditsFromCheat` zero |

Seams B, C, T, T.T. and E run on **carried** saves: `Slot.from_save()` reads a
real persisted EEPROM back and the next seam runs on it, overriding only the
fields a later gate needs. So `wizpigAmulet == 4`, `trophies == 0xFF` and
`ttAmulet == 4` are each something production wrote four times, not something a
fixture asserted.

The driven seam-B arm's lobby approach is watched by
[`tests/route_plan.py`](route_plan.py) against the run's own `[PACE]` position
stream, so a route that stops closing reports the waypoint, the closest approach
and whether it oscillated, was held off, or ran out of budget — instead of
looping to the frame limit and reporting only that the boss never loaded. It is
not a slow arm: measured, the lobby is entered at frame ~2947 and the rematch at
~3124.

Seam C's redirect is invisible in the level-load stream and needs the
`wizpigface:` trace: `game_load_level` logs the level it was *asked* for, then
the branch at `game/src/game.c:642` pushes the hub, swaps in the mouth sequence,
and pops the hub back — so a redirected hub load and an ordinary one print the
same two `levelId=0` lines. Reading those as "the cutscene never fired" is the
mistake this seam was first reported with.

Seam A needs `MDKR_SILVER_ROUTE=1`. DKR places silver coins off the racing line
on purpose, so `MDKR_AUTOPILOT`'s stock AI sweeps up only 5–6 of 8 (measured on
Ancient Lake over three laps) and the `silverCoinCount >= 8` write can never
happen. Like `MDKR_BOSS_ROUTE`, the hook supplies **steering only**, and only
toward a coin within 1,400 units so the kart keeps making checkpoint progress; it
never touches `silverCoinCount` or the coin objects, so a coin still counts only
when `obj_loop_silvercoin` sees its own `distance < 80`.

The contrast arm runs `credits_via_cheat.txt` and confirms it reaches
`MENU_CREDITS` from a save file that was never started — which is why arriving at
credits is not by itself evidence about the campaign.

**What this deliberately does not prove** is in
[`tests/fixtures/README.md`](fixtures/README.md), which also keeps the measured
obstacle behind every route here. What is left is Future Fun Land's own state —
its four cleared races, its balloons, the four world keys and the world-arrival
cutscene flag are constructed, because this check does not drive to that world.
The door into it is gated separately, below.

## Controller hotplug — `tests/check_input_hotplug.py`

```bash
MDKR_AUDIO=0 python3 tests/check_input_hotplug.py --build build
```

Drives real device add and remove through `SDL_JoystickAttachVirtual`, so it
needs no hardware. Eight independently scored assertions over two arms, watching
`[PAD-CHANNEL]` rows emitted from the same three accessors `osContGetReadData`
reads — so the trace cannot agree with SDL while disagreeing with the game.

**It found a real defect on its first run.** A pad connected *before launch* was
bound to two channels: `SDL_Init` queues a `CONTROLLERDEVICEADDED` for each
existing device, `platform_input_init()` enumerated while that event was still
queued, and `SDL_GameControllerOpen()` is reference-counted — so the second open
stored the same pointer in the next free slot. The game saw a controller in P2
that nobody was holding, the next real pad went to P3, and P2's rumble buzzed
P1's pad. See [`open-items/multiplayer.md`](../docs/open-items/multiplayer.md).

The **boot arm is the point**: mid-run hotplug produces one ADDED event and no
enumeration, so every existing gate missed this. Tick `0` of the test schedule
fires from inside `platform_input_init()` *before* its startup enumeration,
which is the only way to reproduce a pad that was already plugged in.

Negative control: the fix gated behind a runtime flag makes the gate exit 1 with
exactly four failures. Three of those are downstream of the one root cause — the
disconnect fail-safe and the padless-rumble guard were already correct, which
the overflow arm's independent passes show.

## Shell self-voicing — `tests/check_a11y_shell.py`

```bash
MDKR_AUDIO=0 python3 tests/check_a11y_shell.py --build build
```

Walks `Tab` and the arrows through the launcher, every settings section and the
in-game overlay, and requires **every focusable control** to have produced a
`[SPEAK]` utterance carrying its name and current value. The expected set is
enumerated from the app's own schema dump, not from a list here, so a setting
added and never voiced fails this gate with no test edit.

**It found three real defects on its first run**, which is the argument for
enumerating rather than listing:

1. `App.UpdateCheck` and `Tools.Enabled` had **no control at all** — both carry
   the Interface category, but that section was hand-written and drew only two
   rows, so they were reachable by ini and env only. `test_app_ui_policy.cpp`
   asserted they were *visible*, which is a statement about policy, not about
   anything actually being drawn.
2. The three settings with the longest help said **nothing whatsoever** —
   `mdkr_a11y_focus()` correctly refuses over-long text whole rather than
   truncating mid-word, and those paragraphs exceed the buffer. Fixed at the
   emission point by keeping as many whole sentences as fit.
3. The ImGui SDL2 backend drops key events carrying an unrecognised window id.

The positive control has two arms: removing the emission entirely fails naming
all 52 controls, and skipping a single key fails naming that one — so the gate
isolates rather than only detecting all-or-nothing.

## Race announcements — `tests/check_a11y_race.py`

```bash
MDKR_AUDIO=0 python3 tests/check_a11y_race.py --build build --rom baserom.us.v80.z64
```

Drives a full autopiloted race and requires the `[SPEAK]` stream to carry the
five things a player who cannot see the screen needs in order to follow one: the
start, at least one change of position, the laps, the final lap, and the result.
Lap calls must match `Lap N of M` — a bare "Lap 2" does not say how much race is
left — and the finish must be spoken at `critical` priority, because it is the
one line no later utterance may cut off.

Three arms, each answering a question the others cannot:

| Arm | Asks | Passes when |
|---|---|---|
| full race | is a race followable by ear? | all five moments spoken, laps named `Lap N of M`, the result `critical` |
| purity | did listening change the race? | `[SIMHASH]` v3 rows byte-identical with announcements on and off — 5400 rows on the current route |
| categories | does each switch own its own category? | silencing `race_event`, `race_lap` or `race_position` leaves the other two intact |

The purity arm is the authority proof: announcements are
`MDKR_ENH_PRESENTATION`, so the byte-identical hash stream is what makes that
classification a measured fact rather than an intention.

**The coalescing assertion is the one with teeth.** Position changes in a
mid-pack scrap land about three authoritative ticks apart, and a stream that
spoke all of them would cut every line off with the next one — audible as noise,
not as information. `MDKR_A11Y_RACE_POSITION_MIN_TICKS` (60 ticks,
`platform/a11y_race.h`) debounces them, and the gate measures the tightest gap
between consecutive `cat=race_position` utterances against the `[PACE]` clock,
failing under 50 frames. Asserting 50 rather than 60 leaves the tick-to-frame
mapping a frame or two of slack at the edges while staying an order of magnitude
above what an uncoalesced stream produces. The arm refuses to score itself
vacuous: fewer than two timestamped position calls is a failure naming that
cause, so "the race had no overtaking" and "the spacing was never tested" cannot
be mistaken for a pass.

The route runs under `MDKR_SYNTH_FIELDS=1` for determinism. That is sound here
and would not be for a pacing measurement — the synthetic pacer's presentation
and GPU counters are meaningless, but authoritative ticks, the hash stream and
utterance ordering are not.

## Future Fun Land unlock — `tests/check_future_fun_land.py`

```bash
MDKR_AUDIO=0 python3 tests/check_future_fun_land.py -v               # ~4 min
```

Four gold trophies plus a beaten Wizpig 1 open the lighthouse rocket
(`begin_lighthouse_rocket_cutscene`, `game/src/thread3_main.c`), which is the only
way into the last world. The trophies are **produced**, not stated: the check
imports `check_trophy_series.py`'s championship driver and chains four of them.

Three arms, because a refusal writes nothing at all — no bit, no level load, no
trace — so a single arm would pass with the whole unlock deleted:

| Arm | Save | Must happen |
|---|---|---|
| unlock | four production golds, Wizpig 1 beaten | `ASSET_LEVEL_ROCKETSEQUENCE` once, `ASSET_LEVEL_FUTUREFUNLANDHUB` reached, `CUTSCENE_LIGHTHOUSE_ROCKET` persisted |
| one short | worlds 1–3 gold, world 4 driven to second place (a production **silver**) | the signpost triggered and **nothing** else |
| no Wizpig 1 | the four-gold save with `bosses & 1` cleared | the signpost triggered and **nothing** else |

Every arm asserts the `rocketsign: trigger` line, which is emitted at the
honk/collision and *before* the gate is evaluated. That is what separates "the
route never reached the signpost" from "the signpost read the save and said no",
and it is the only thing that makes the two negative arms mean anything.

## Hand-asm transcription checks (RUN THESE AFTER ANY CHANGE UNDER `game/src/hasm*`)

Three checks guard the bug class described in `HANDOFF.md` §3's sixth shape: **C
bodies that transcribe hand-written assembly, which only this port compiles.**
Upstream's matching build links the real `.s`, so it never executes that C and its
tests can never catch an error in it. Before trusting anything under
`game/src/hasm/`, diff the C against the `.s` next to it. Full findings:
`docs/OPEN_ITEMS.md` wave "hasmaudit".

```bash
MDKR_AUDIO=0 python3 tests/check_math_rotpy.py -v            # ~20 s
MDKR_AUDIO=0 python3 tests/check_math_tables.py -v           # ~5 s, no race
MDKR_AUDIO=0 python3 tests/check_collision_untextured.py -v  # ~2 min
```

**`check_math_rotpy.py`** — `vec3f_rotate_py()` must pair pitch with yaw. The
upstream body transposed them in the x and y results, turning a horizontal
direction vertical (`pitch 0, yaw 90°` → the ROM's `(z,0,0)` became `(0,−z,0)`).
Its strongest assertion needs no golden numbers and no disassembly: `vec3f_rotate`
applied to `(0,0,z)` with zero roll **is** `vec3f_rotate_py` by definition, so the
two must agree — and they only do with the fix. `MDKR_ROTPY=legacy` restores the
transposition from the same binary.

> The fix is **behaviour-neutral for the racer simulation** (0 of 359 `[PACE]` rows
> change), because every call site feeds particles, lights or sprites rather than
> physics. So do **not** try to assert this one with a `[PACE]` diff — it would pass
> vacuously. That is why the probe counts calls and hashes every result instead.

**`check_collision_untextured.py`** — an untextured terrain batch
(`textureIndex == 255`) is collidable with `SURFACE_DEFAULT`; the upstream body
`continue`d and dropped it. **The case is not reached by any shipped level** (0
batches across all 20 main and all 10 boss tracks), so the two normal arms are
asserted *bit-identical* — that is the safety statement and the tripwire. Because
unreached arms cannot distinguish a working fix from no fix,
`MDKR_COLLTEX_FORCE=1` treats every batch as untextured; forced+legacy then
collapses the candidate list to **1** and drops the racer to **y ≈ −568**, while
forced+fixed holds 26 candidates at y 29.

**`check_math_tables.py`** — the `.s` **data** section this port substitutes for.
`platform/math_util_native.c` regenerates `gArcTanTable` and supplies the RNG seeds
because `game/src/hasm/ido/math_util.s` is not assembled. Two divergences are
**still live by default and deliberately deferred**, and this check exists so they
cannot rot and so nobody deletes the opt-in switches without reading why:

- `gArcTanTable` truncates where the ROM rounds — 491 of 1025 entries one unit low.
  `MDKR_ARCTAN=round` reproduces the ROM's table **exactly** (0/1025), which is
  what the check asserts, by hashing the table the binary built against the
  `.half` directives parsed out of the `.s` at run time. **No ROM bytes are
  committed.**
- `gCurrentRNGSeed`/`gPrevRNGSeed` are `0x00051234`/`0`; the ROM boots
  `0x5141564D` for both. `MDKR_RNGSEED=rom` selects the ROM's.

> **Why they are deferred, and what to do when you land them.** Both are reached
> and material (80 and 110 of 359 `[PACE]` rows respectively), so turning either on
> shifts every AI racing line. `check_race_drive`'s open-loop route then strands
> the kart short of a lap, and `check_collision_gridmask`'s **positive control
> stops reproducing its defect**. Re-cut both fixtures in the same change and
> invert the two "still the known divergence" assertions in
> `check_math_tables.py` — which is exactly what its failure message tells you.

## Wave-visibility table layout — `tests/check_wave_visible_table.py` (RUN THIS AFTER ANY `waves.c` OR LINK-FLAG CHANGE)

The only check here that inspects a **built artifact** rather than running the game.
It needs no ROM and starts no process, so it is trivially audio-safe.

```bash
python3 tests/check_wave_visible_table.py                                  # build-web/ or dist/web/
python3 tests/check_wave_visible_table.py --wasm build-web-base/mdkr64_web.wasm -v
```

`waves.c` treats `D_8012A5E8[2]` and `D_8012A600[24]` as **one 26-entry table** —
`waves_visibility()` sentinel-clears all 26 slots and then appends visible wave
blocks through `D_8012A5E8[var_a3]` with `var_a3` measured up to **25**, indexing the
first array straight on into the second. As two separate C objects that only works if
the linker places them adjacently. Mach-O/arm64 does (gap 0). **wasm-ld does not**:
it 16-byte-aligns the 288-byte `D_8012A600`, leaving an **8-byte hole** after the
24-byte `D_8012A5E8`, so slots 2..25 land below the cleared sentinels, the
`blockID != -1` walk runs off the table, and `func_800B92F4()`'s
`D_800E3178[D_8012A5E8[k].unk8]` loads through a garbage index. That is the
`memory access out of bounds` a player hit in the browser — full write-up in
[`../docs/open-items/web.md`](../docs/open-items/web.md#fixed-browser-memory-access-out-of-bounds-in-the-wave-renderer--wave-wavetable).

The check asserts the *shape*, not addresses (those move every link): the 26
sentinel-clearing stores the reset loop emits must form a single arithmetic run with
stride `sizeof(unk8012A5E8)` == 12 and no gap. It **fails closed** — if it cannot
locate those stores it fails rather than passing vacuously, because "found nothing"
cannot tell a fixed build from one whose codegen moved.

Verified in both directions on real artifacts:

| artifact | result |
|---|---|
| fix applied | `run len=26 232680..232980` → **PASS** |
| fix reverted | `run len=2 232688..232700` \| **GAP 8** \| `run len=24 232720..232996` → **FAIL** |
| the wasm behind the player's **first** crash (`63c5d32`, md5 `348a9d80…`) | → **FAIL** |
| the wasm behind the player's **second** crash (`9c643a0`, md5 `a08bbcb0…`) | → **FAIL** |

Both player crashes were the same defect at two different instructions: the first read
past the table (`D_800E3178[D_8012A5E8[k].unk8]`), the second had the runaway walk's own
`unk8` **store** land on `gWaveBlockIDs[8]`, after which `waves_render()` indexed
`gWaveModel` past its array. One check covers both.

### Reproducing the browser layout on native (how the second crash was pinned)

Native is immune to this defect, so the only way to exercise it natively is to *build
the browser's layout*. Replace the union with the shipped module's exact relative
placement and toggle only the hole:

```c
static struct {                 /* offsets measured in the shipped wasm */
    unk8012A5E8   head[2];      /* +0    &D_8012A5E8    */
    unsigned char hole[8];      /* +24   wasm-ld padding -- REMOVE for the control */
    unk8012A5E8   tail[24];     /* +32   &D_8012A600    */
    unsigned char between[272]; /* +320                 */
    s16           blockIDs[512];/* +592  &gWaveBlockIDs */
} g;
```

With the hole: walk runs to k=1199, its store at k=50 hits `gWaveBlockIDs[8]`, and
tracks 8/10/30 die with Bus error / SIGSEGV / SIGSEGV. Without it: k ≤ 22, all clean.
This works because **the web build is bit-reproducible** — a local pre-fix build is
byte-identical to the shipped `a08bbcb0…`, so the offsets above are the real ones.

**Native cannot catch this defect at all** — the gap is 0 there by luck, so every
native fixture passes either way, and ASan does not redzone the
`__DATA,__common` symbols involved. UBSan `-fsanitize=array-bounds` *does* flag the
out-of-bounds indices (`waves.c:535-539`, `:691-710`) without any crash; that is the
native tell.

## Out-of-bounds index sweep — `tests/check_array_bounds_sweep.py` (RUN THIS AFTER ANY `game/src` CHANGE)

The generalisation of the check above, and the detector for the whole split-array
class: a UBSan `-fsanitize=array-bounds,pointer-overflow` build driven over 8
representative routes (~1.5 min), failing on **any** out-of-bounds index that is not
in its committed allow-list. It builds `build-ubsan/` itself; `--no-build` reuses it.

```bash
MDKR_AUDIO=0 python3 tests/check_array_bounds_sweep.py -v
```

Why it exists rather than a table of symbol pairs: the class is *an index leaving its
array*, and a pair table only knows the pairs somebody already thought of. It found
two live defects (`gTrackSelectIDs`/`gFFLUnlocked` and `gScreenViewports[4]`) that no
crash and no native fixture had ever revealed — see docs/OPEN_ITEMS.md wave
"splitsweep".

Every allow-list entry carries the reason it is not a finding, and entries are keyed
on **(file, type, source snippet)** rather than line number, so moving code does not
re-baseline the list but changing the offending line does.

It **fails closed** four ways, because "no reports" cannot distinguish clean from not
measuring: the binary must actually import `__ubsan_handle_out_of_bounds` (the
sanitizer flag has to survive CMake's flag ordering — note that `-Wno-everything`
lands *after* `CMAKE_C_FLAGS`, which is why the flags go through
`-DMDKR_EXTRA_C_FLAGS`), every route must exit 0, the report set must be non-empty,
and `REQUIRED_SENTINELS` (allow-listed idioms on paths every route crosses) must all
still be reported.

When it fails, triage before allow-listing. The question is *not* "does it crash
here" — the wave-table defect never crashed natively. It is: does the index leave its
array; what object does it land on **on LP64 and on wasm32**
(`tools/compare_data_layout.py` answers that); and does anything depend on it landing
there (`gFFLUnlocked` did). If the adjacency is load-bearing, back both names onto one
object with `_Static_assert`s, as `game/src/waves.c` and `game/src/menu.c` do.

Known blind spot: an overrun written through a **bare pointer** rather than an indexed
array is invisible to this check and to ASan. `get_inside_segment_count_xz()` is the
live example.

## ROM revision + byte order — `tests/check_rom_revision.py` (RUN THIS AFTER ANY ROM-LOADER CHANGE)

Which of the five released DKR revisions did the user hand us, and does the browser
shell agree with the engine about it? The old gate was size + magic + "the internal
title contains 'diddy'", which **all five revisions pass** — so a European or
Japanese cart was accepted and then SIGSEGV'd before the first frame, reading its
asset lookup table from the us.v80 offset. Two of the five are now supported
(us.v80, pal.v80); the rest are refused by name.

```bash
python3 tests/check_rom_revision.py           # skips any revision not present
python3 tests/check_rom_revision.py --roms build/roms -v
```

Asserts: each present revision is classified and named; unsupported ones are
refused **cleanly** (exit 1, no crash trace, no renderer bring-up); a non-DKR N64
ROM is called out as such; synthesised `.v64`/`.n64` copies race byte-identically
to the `.z64`; `platform/rom_id.c` and `dist/web/rom-id.js` produce identical
revision tables and identical verdicts (the JS is run under node); and pal.v80 —
whose whole claim to support is a byte-identical asset payload — renders and drives
byte-identically to us.v80.

It synthesises its byte-swapped and non-DKR inputs at runtime under a temp dir and
deletes them, and **skips cleanly** for any revision that is absent, so it passes on
a machine with only `baserom.us.v80.z64`. Verified in both directions; the pasted
output of both positive controls, and the two flaws the controls exposed in the
check itself, are in [../docs/ROM_REVISIONS.md](../docs/ROM_REVISIONS.md) — which is
also the per-revision failure taxonomy.

## Visual checks

Never open a window to look at something — dump and convert:
```
MDKR_AUDIO=0 MDKR_DUMP_FROM=1240 ./build/mdkr64 --headless-frames 1252 \
  --dump-frames /tmp/shot --rom baserom.us.v80.z64
sips -s format png /tmp/shot/frame_1250.ppm --out /tmp/shot/shot.png
```
`MDKR_DUMP_FROM` keeps a late-scene capture from writing thousands of early PPMs.
`MDKR_DUMP_EVERY=N` adds a stride on top of it, so sampling ~10 frames across a
6000-frame drive costs one pass and 10 PPMs instead of thousands (that is what
`check_race_drive.py` uses). Frame ~1250 is the Timber's Island frontend
(sand/palms/character); frame ~2880 of `race_drive_time_trial` is the Ancient Lake
start line (full-size karts + HUD).

### Boost / exhaust graphics — `MDKR_FORCE_BOOST=<frame>[:<len>]`

`MDKR_FORCE_BOOST` gives every racer, on every frame in `[frame, frame+len)`
(default `len` 90), exactly the state a `SURFACE_ZIP_PAD` gives it
(`boostTimer = normalise_time(45)`, `boostType = BOOST_LARGE`), so a dumped frame
is guaranteed to contain rendered boost flames. It is a no-op unless the variable
is set (`objects.c dkr_force_boost_hook`).

```
MDKR_AUDIO=0 MDKR_DUMP_FROM=2810 MDKR_FORCE_BOOST=2830:120 ./build/mdkr64 \
  --headless-frames 2960 --dump-frames /tmp/boost \
  --input-script tests/input_scripts/race_drive_time_trial.txt \
  --rom baserom.us.v80.z64
sips -s format png /tmp/boost/frame_2910.ppm --out /tmp/boost/boost.png
```
Expect a bright, character-tinted exhaust plume out the back of every kart
(player white/yellow, Banjo green-yellow, Krunch magenta) at frames 2850..2950,
and none at 2820. This is the regression check for the `ASSET_MISC_20` boost-table
decode (`objects.c dkr_boost_table`); if that decode breaks again,
`render_sprite_billboard` aborts with `[FATAL] ... NULL sprite`.

Why a hook and not a fixture: originally, no deterministic input script reached a zip
pad (the port's driving physics stranded the racer), and in the attract demo only
racers the camera is *not* following ever boost, so their boost objects are correctly
culled before the draw. The stranding is fixed (`docs/OPEN_ITEMS.md`) and
`race_drive_long.txt` now takes a genuine zip pad on camera at ~frame 5650 — dump
around there for an unforced boost. `MDKR_FORCE_BOOST` is kept because it puts a boost
on *every* racer at a chosen frame, which is a stricter check of the table decode.

## Full-race / finish path — `race_full_3lap.txt` + `MDKR_FORCE_LAPS`

`race_drive_long.txt` proves the racer drives and completes ONE lap.
`race_full_3lap.txt` drives toward the race's natural end. Note its limit: the
open-loop route (hold accelerate, pulse LEFT for 25 frames every 120) holds the
racing line for one lap and then drifts — measured lap 1 at clock 2776, lap 2 not
until 10557 — so do **not** rely on it to reach the finish.

To exercise the finish deterministically, shorten the race instead:
```bash
MDKR_FORCE_LAPS=1 MDKR_AUDIO=0 ./build/mdkr64 --headless-frames 8000 \
  --input-script tests/input_scripts/race_full_3lap.txt --rom baserom.us.v80.z64
```
`MDKR_FORCE_LAPS=N` rewrites `LevelHeader.laps` once at the load boundary
(`platform/asset_swap.c`), so every one of the ~12 readers across `racer.c` and
`game_ui.c` sees a consistent value. It is a **no-op unless set**.

Note `MENU_RESULTS` (menuId 17) is the **trophy-race** results screen; a Time Trial
finish is not expected to reach it, so its absence is not a defect.

## Adventure — `adventure_hub_drive.txt` + `tests/check_adventure_hub.py`

The only fixture that leaves the frontend on the **Adventure** side. It walks
FILE SELECT → new-game name entry → the intro cutscene → Timber's Island, then
drives the hub with a real player-input throttle/steer pattern.

```bash
python3 tests/check_adventure_hub.py -v        # ~40 s, muted + headless
```

Reference (deterministic): `menu_init` 19 @1781, 6 @1931, 23 @2236;
`level_load: levelId=36 cutscene=15` @2236 (the intro cutscene) then
`level_load: levelId=0` @**5867** (`ASSET_LEVEL_CENTRALAREAHUB`); 6133 in-hub
frames, path length 44441 world units, bounding box 5989 × 4069, y −35..539,
final `cp=23 lap=2`, max single-frame step 13.8, sampled frames 1014–2345
distinct colours / sigma 36.9–57.7.

The cutscene budget is not padding: it ends when its terminating animation entry
(actor 23, `animationStartDelay` 3630) counts down one tick per game update, and
it is **not** skippable — `cinematic_start()` is called with both skip masks 0.
So the hub cannot be reached before frame ~5867; do not shorten the budget.

The check allows **one** sampled frame to score below the render thresholds: the
route brushes a hub door, and DKR's own door fade whites the screen out for ~30
frames (measured 209 colours / sigma 7.9 around frame 6600).

This route is what caught three latent LP64 defects, all reachable by ordinary
play and none reachable from any other fixture — the `GameTextTableStruct.entries`
stride (which corrupted the arena via a 64 KB `asset_load`), the
`ParticleBehaviour` 0xA0 record stride, and the `func_80026E54` `sp94[10]` stack
overflow. Full write-up in `docs/OPEN_ITEMS.md` "P3.5". `MDKR_AUTOPILOT=1` also
drives the hub (it laps the hub spline: cp 66 / lap 6 in ~6100 frames).

**`MENU_RESULTS` (menuId 17) is NOT reachable from a 1-player Adventure race** —
`menu.c` gates it on `gNumberOfActivePlayers >= 2`, and a won 1-player Adventure
race goes through `race_transition_adventure()` back to the hub instead. See
`docs/OPEN_ITEMS.md` "P3.5 Part B" before spending time on it.

## Adventure Two — `tests/check_adventure_two.py`

```bash
python3 tests/check_adventure_two.py                    # all 20 race tracks
python3 tests/check_adventure_two.py --tracks 5,19 -v  # focused iteration
```

This is a real mode/save route, not `CHEAT_MIRRORED_TRACKS` injected into a
Tracks race. Every arm installs a checksum-valid N64-format EEPROM, selects
Adventure Two at GAME SELECT, resumes a slot carrying
`CUTSCENE_ADVENTURE_TWO`, drives through Timber's Island and Dino Domain, and
enters a race. `MDKR_LOAD_TRACK` retargets that final load so the same campaign
path drives all 20 main tracks.

The check requires finite racer state and checkpoint progress on every track,
the exact Adventure Two bit in the rewritten save, and the canonical
big-endian global unlock block. Trace witnesses prove that stereo panning,
minimap placement, human steering, and the negative N64 viewport path all
execute their mirror operations.

The pixel control compares Adventure One and Two before AI difficulty can
diverge. It crops out the deliberately fixed safe-4:3 HUD and horizontally
reflects the Adventure One world. Measured over 14 samples: same-orientation
MAD **49.154**, reflected MAD **2.816**. Reverting the clip-X translation makes
the two modes byte-identical and the positive control fails.

## Adventure race loop — `adventure_race_loop.txt` + `tests/check_adventure_race_loop.py`

The fixture above drives the hub; **this** one enters a race from it and comes
back. It is the first route that runs `obj_loop_exit()` → `racer->exitObj` →
`racer_enter_door()` → `func_8006D968()`, i.e. DKR's actual level-to-level
transition machinery.

```bash
python3 tests/check_adventure_race_loop.py -v   # ~55 s, muted + headless
```

Current reference:

```
level_load: levelId=0  @5867                       Timber's Island
level_load: levelId=12 @6635                       Dino Domain lobby
level_load: levelId=5  @6936                       Ancient Lake
[PACE] frame=12186 ... clock=4711 rlap=3 fin=1 fpos=1 ridx=0
level_load: levelId=12 @13624 entrance=3           win return to lobby
level_load: levelId=0  @13976 entrance=2           back on Timber's Island
```

The primary arm records 6685 race-level rows, 62600 world units through the
natural finish, and max driving step 20.4; 3022 rows / 35551 units prove the kart
is alive after returning to the hub. The scene sampler spans the race, win
transition, lobby, and hub.

**Steering comes from a test hook, not from the script.** Hand-timed stick input
cannot hit a hub door — 18 open-loop routes failed, and `MDKR_AUTOPILOT=1` laps
the hub's AI-node loop for ever (its closest approach to *any* hub exit is 2144
units). `MDKR_DRIVE_ROUTE` (`platform/mdkr_adventure.c`) steers closed-loop at
named objects instead:

```bash
MDKR_OBJDUMP=1     # per level: every exit / door / balloon / dialogue NPC, with
                   # the fields that decide whether the player may pass (=2 adds
                   # the AI-node graph). This is how the route was authored.
MDKR_DRIVE_ROUTE="0:200,500:B10:E12;12:E5:E0"
                   # per level, ordered steps: E<destMapId> = drive at the exit to
                   # that level, B<id> = collect that golden balloon, x,z = a
                   # waypoint. Step progress persists per level, so the lobby
                   # drives INTO the race on the way in and back OUT on the way
                   # home. Unlisted levels are untouched, so MDKR_AUTOPILOT drives
                   # the race itself.
MDKR_DRIVE_WPR=<units>      waypoint radius (default 220)
MDKR_DRIVE_VERBOSE=1        per-30-frame progress trace
```

Five things about this route are worth knowing before changing it:

- **The route collects a golden balloon so the doors really open.** The Dino
  Domain doors want 1 (`obj_loop_door`: `*settings->balloonsPtr >=
  door->balloonCount`); with balloon 10 banked the lobby's Ancient Lake door
  reports `open=1`. Only 4 of the hub's 7 balloons are collectable — the other 3
  are Taj-challenge balloons, inert until their `tajFlags` bit is set.
  Production object-model collision makes a closed door a hard gate. The
  same-binary `check_door_blocks.py` control proves that the exact legacy
  collision path still drives through, so this fixture must continue to collect
  its balloon rather than relying on the old defect.
- **Do not bump Taj or T.T.** They open a dialogue on contact and then call
  `disable_racer_input()` every frame, which zeroes the kart's velocity; the
  dialogue is advanced by `input_pressed()` off the pad, which the drive hook
  cannot write. Measured wedge: frozen at (−125.3, 149.6) for 3000+ frames, 84
  units from Taj.
- **Never hold A in an Adventure fixture.** Every post-race screen is driven by
  `input_pressed()`, which is edge-detected: while A is held there are no further
  edges and every later tap is silently swallowed. This is why this fixture has
  no `A <long hold>` line even though `adventure_hub_drive.txt` does.
- **Starting-grid slot zero is not "the player."** `gRacers` preserves starting
  order. The old finish probe sampled `(*gRacers)[0]`, followed an AI, and
  produced the false GAME-03 report that the human reached lap three without
  finishing. The gate now follows `gRacersByPort[PLAYER_ONE]` and asserts its
  `racerIndex`.
- **Natural finishing place is observed, not asserted.** The old checkpoint
  finished fifth; the current runtime-boundary build naturally finishes first.
  `MDKR_ADVENTURE_WIN=1` and `MDKR_ADVENTURE_LOSS=1` act only after production
  marks the same natural finish, then request first and non-first verdicts.
  Both arms must report the same pre-control place/frame/clock/lap and preserve
  identical lap, clock, position, and physics.

Each arm runs in its own temporary working directory and captures its new
`save/eeprom.bin` before cleanup. The checker derives Ancient Lake's save ordinal
from the ROM, validates the slot checksum, and decodes course and balloon bits
itself. Loss must be status 1 / `(1,0,0,0,0,0)`; win must be status 2 /
`(2,1,0,0,0,0)`. Shared repository save state is never read or removed.

## Recording a time — `race_full_3lap_tt.txt` + `tests/check_race_finish_time.py`

**`race_full_3lap.txt` can never save a time, and neither can any `*_time_trial`
fixture.** Their names are misleading: they select *Tracks* mode and then confirm
**TIME TRIAL = OFF**. DKR writes course/lap records only from
`race_finish_time_trial()`, whose whole body is gated on `if (gIsTimeTrial)`, so on
those routes the record write is unreachable by construction (measured at the
finish: `gIsTimeTrial=0`, `gNumRacers=8`, `display_times=0`, `best_times=0x00`).

`race_full_3lap_tt.txt` is the route that does record a time. It differs by one
input — a stick **DOWN** at the track-select Time-Trial stage, which flips
`gTracksMenuTimeTrialHighlightIndex` 0 → 1 — plus repeated A taps after the finish,
because the post-race screens are edge-detected and need a fresh press per stage
(BEGIN → SHRINK_VIEWPORT → RACE_TIMES → ENTER_INITIALS → RACE_RECORDS → OPTIONS →
END). Use it with `MDKR_AUTOPILOT=1`; a solo Time Trial has 1 racer, so no throttle
input is wanted.

```bash
python3 tests/check_race_finish_time.py -v        # ~90 s, muted + headless
```

It asserts the full chain, not just survival: `rlap=` reaches the level's lap
count, `fin=` flips 0 → 1, the race clock freezes, the frozen value is plausible,
that **exact** value is present in `save/eeprom.bin` as a 16-bit big-endian
record, a second run reads it back rather than resetting it, **and the process exit
code is 0**. That last one matters: the `timetrial_ghost_read` stack overflow it
caught aborted with nothing at all on stderr.

It also asserts that the route **reaches the ghost-playback path**
(`ghost=` ≥ 100 frames; measured 5010, `gbank=0` = the player's own ghost). The
fixture's trailing A taps pick "TRY AGAIN" after the finish, which re-enters the
level and plays back the ghost recorded on the previous lap set — the only thing
that exercises `timetrial_ghost_read()`. Asserting the coverage means a future
route change that stopped re-entering the level fails loudly instead of quietly
covering nothing.

⚠️ If you positive-control the `timetrial_ghost_read` fix, revert **both** coupled
parts — the array size `[4]` → `[3]` *and* the loop bound `<` → `<=`. Reverting
only the size leaves the loop writing three elements: memory-safe, so no runtime
check can catch it, but silently wrong. The `_Static_assert` makes that state fail
to compile; don't neuter it. See the three-state table in `docs/OPEN_ITEMS.md`.

`rlap=`/`fin=`/`fpos=` are `[PACE]` fields fed from the post-assignment seam in
`race_check_finish()`. The older `lap=`/`cp=` come from inside
`update_player_racer`, which stops running for a racer the moment it finishes —
so they are permanently one lap stale at the finish and cannot see
`raceFinished`. Assert on the race-check fields for anything finish-related;
`ridx=` proves the stable controller-mapped racer is being observed rather than
a starting-grid slot. `ghost=`/`gbank=` are fed from
`timetrial_ghost_read()` and count interpolated ghost frames (bank 0/1 = the
player's own ghost, 2 = staff).

Reference (deterministic, Ancient Lake, car): `level_load` frame ~2641, finish at
traced clock **4709** / frame **7495**, fastest lap **1515**; `save/eeprom.bin` md5
`bfd4de76775de4b5c5d972a97eeb6ed7`, course record **4683** at byte 338.

⚠️ **These numbers moved in P3.5** (they were clock 4777 / frame 7589, fastest lap
1558, md5 `a144ba9f…`) for the same reason `race_drive_long`'s did — the
`ParticleBehaviour` on-disk-stride fix changed the trajectory of a route driven by
`MDKR_AUTOPILOT`. The check now matches the EEPROM record to the traced clock
**within 32** rather than exactly: the traced value comes from the `[PACE]` probe
inside `update_player_racer` (which stops for a finished racer) while the record is
what `race_finish_time_trial()` sums when it runs, so the two reads sit a few
updates apart. They coincided before and no longer do. See `docs/OPEN_ITEMS.md`
"P3.5".

## Ghost matrix — `tests/check_ghost_matrix.py`

`check_race_finish_time.py` above exercises `timetrial_ghost_read()` on **one** of
the 47 legal (track, vehicle) pairs — Ancient Lake in the car. That one row is how
the 3-vs-4 control-point overflow was found, and it was found by accident. This
check covers the other 46.

The pair matters, not just the track: a Controller Pak ghost slot is keyed on
**both** ids, so `func_80074B34()` / `func_80075000()` (`game/src/save_data.c`)
match on `levelId` *and* `vehicleId`. Ancient Lake in a car round-tripping says
nothing about Ancient Lake in a plane.

```bash
python3 tests/check_ghost_matrix.py               # all 47 pairs, ~15 min
python3 tests/check_ghost_matrix.py --subset      # every track and vehicle once
python3 tests/check_ghost_matrix.py --pairs 5:0,15:2
python3 tests/check_ghost_matrix.py --matrix      # print the pairs and exit
```

Reference run (us.v80, `build-rel`): **46 pairs round-tripped a ghost through a
fresh process, 1 documented non-producer, 15.4 min** — measured alongside other
ROM work on the same machine, so treat it as an upper bound rather than a floor.
Three pairs needed the fallback cadence (19 hovercraft, 30 plane, 33 hovercraft).

Three runs per pair, each in its own `MDKR_SAVE_DIR` — 47 ghosts do not fit in the
pak's 6 slots (`DKR_GHOST_SLOT_COUNT`), and per-pair isolation is what keeps them
from contending:

1. **measure** — drive to the finish and read the frame the post-race OPTIONS
   stage opens offering SAVE GHOST. A three-lap finish lands anywhere between
   ~7370 and ~12470 frames depending on the pair, so a fixed script cannot aim at
   a menu row across 47 rows; `[TTGHOST] event=options` makes the aim a
   measurement. Its trailing taps pick TRY AGAIN, which re-enters the level and
   plays back the just-recorded ghost — the in-process half of the coverage, and
   the original overflow route.
2. **write** — fresh save dir, identical re-drive, menu steered onto SAVE GHOST
   (the only route to `timetrial_save_player_ghost()`). Asserts
   `[TTGHOST] event=save … status=0` with a non-empty node count and that
   `controller-pak-1.mdp` appeared.
3. **read** — a **fresh process** on that save dir. `[TTGHOST] event=load` must
   report the same pair and the same `nodes`/`character`/`time` that were
   written, and playback must run from a player bank (0/1, not the staff ghost).

`nodes`/`character`/`time` are the whole of `GhostHeader` apart from its checksum,
so matching all three is the serialised record surviving a process boundary in
every field the format carries. On top of the trace comparison the write run
locates that header **in the pak image itself**, by its measured contents rather
than a hardcoded offset — the same discipline `check_race_finish_time.py` uses for
the EEPROM course record, and for the same reason: a layout or byte-order
regression in the ghost serialiser moves those bytes and fails here, where an
offset-keyed probe would keep passing. The read run then asserts the pak comes out
**byte-identical**, because an identical re-drive cannot beat the stored time and
must not rewrite player data.

The pak image holds `GhostHeader`/`GhostNode` exactly as the machine laid them
out, which is what a real Controller Pak holds too (raw N64-order structs); on a
little-endian host the same design yields little-endian fields, so the check
builds its search pattern from `sys.byteorder`. It asserts "the struct
round-tripped verbatim", not "the port picked an endianness".

The save format is frozen — this check proves the **current** layout round-trips;
it must never be relaxed to accommodate a layout change.

Every run asserts the **exit code** and the presence of the markers a healthy run
emits, never merely the absence of a crash marker: the defect class this exists
for (`__stack_chk_fail`) prints nothing at all to stderr.

`--re-entry-pair` (default `5:0`) additionally drives write → reload → read →
re-enter → read again, because the original overflow only appeared on a *repeat*
read within one process.

### Pacing decides which pairs can be lapped at all

Whether `MDKR_AUTOPILOT` completes a lap is **pacing-dependent, in both
directions** — neither simulation cadence laps the whole matrix:

| pair | Original (`SYNTH_FIELDS=2`) | Enhanced (`SYNTH_FIELDS=1`) |
| --- | --- | --- |
| track 4, hovercraft | finishes frame 5614 | stalls at `rlap` 1, `cp` 25 |
| track 33, car | finishes frame 6837 | stalls at `rlap` 1, `cp` 52 |
| track 19, hovercraft | stalls at `rlap` 0 | finishes |
| track 30, plane | stalls at `rlap` 0 | finishes |
| track 33, hovercraft | stalls at `rlap` 0 | finishes |

All measured over a 40000-frame budget. Track 33 appears on **both** sides, which
is the point: this splits per *pair*, not per track. Track 4 offers **only** the
hovercraft, so pinning Enhanced would leave a whole track unlappable; pinning
Original loses three more pairs. So each pair is driven on the **first cadence
that gets it round**
(`CADENCE_ARMS`, Original first — the pacing `check_vehicle_sweep.py` sweeps this
same matrix under), and any pair that needed the fallback is tagged
`[enhanced cadence]` in the output rather than quietly absorbed.

A run that *dies* is never allowed to fall through to the next cadence — that would
let a crash read as "that pacing just could not lap it".

This is a pacing-dependent AI/driving divergence, not a ghost defect. It is
recorded here because it decides which pairs this check can cover, and because it
is invisible to `check_vehicle_sweep.py`, which asserts forward progress
(`maxcp >= 3`) rather than lap completion.

⚠️ One legal pair is a documented **non-producer** and is asserted to stay that way
rather than skipped: Spaceport Alpha (15) **in the car**. Its autopilot racing line
dead-ends at `courseCheckpoint` 10 and completes no lap in 40000 frames on *either*
cadence, and DKR only records a ghost for a course time under 10800 frames
(`race_finish_time_trial()`). The pair is legal in the ROM's `available_vehicles`
mask and is driveable by a human; it is the AI line that does not get a car round
that track. If it ever finishes, the check fails and tells you to promote it out of
`NO_GHOST_PAIRS`.

## Taj vehicle challenges — `tests/check_taj_challenges.py`

This is the end-to-end gate for all three dynamically created Timber's Island
vehicle challenges. It covers car, hovercraft, and plane at their live ROM
thresholds (5/10/18 balloons), with first win, loss, abort, completed replay,
and a disabled-completion positive control for each vehicle.

First-time arms exercise the real threshold offer, Taj dialogue and
transformation. Replay enters at the production acceptance seam because a
completed challenge is not auto-offered. Every arm checks two-racer movement,
production finishing places and result dialogue, exact `tajFlags`, EEPROM
checksum, and a second-process reload; first-win arms also score a rendered
frame.

```bash
python3 tests/check_taj_challenges.py --quick -v  # five car arms
python3 tests/check_taj_challenges.py -v          # all 15 arms
```

The fixture driver contributes only carpet progress/hold events or the same
quit request used by the pause menu. It never writes the human finish, place,
result menu, progress flags, or save; the assertions below define the exact
boundary.

## Playable Taj mod — `tests/check_taj_playable.py`

This is the ROM-backed end-to-end gate for the native virtual Taj character. It
enters `ABRACADABRA` through the real Magic Codes keyboard, selects the next
contiguous virtual display identity, races and restarts with the carpet
composition, then reboots
against the persisted global sidecar. Separate two-, three-, and four-player
arms prove Taj belongs to the selecting controller and viewport; the two-player
arm also proves ordinary Diddy can coexist even though both resolve to retail
donor row 9. The car/hovercraft/plane arms prove the selected identity reaches
every ordinary vehicle dispatch. An imported-save arm reconciles existing
`tajFlags` before the first select visit. Exact audio traces cover the Taj
highlight, confirmation, and horn paths. A real WebGPU race capture is joined
to the carpet/rider composition and donor-suppression traces, then checked for
Taj's purple robe and the red/gold carpet. Instrumented witnesses additionally
require the ROM-authored carpet clock to publish changing vertex hashes through
both render buffers and require the playable composition to preserve the retail
7:15 carpet-to-Park-Warden scale ratio. The rendered lower-centre silhouette
rejects the former oversized 1:1 donor scale. A negative-control run freezes
the mesh and restores that 1:1 scale; the same witness validator must reject
both defects. The two-player arm also A/Bs production against the exact former
split-screen filter and requires the carpet-only grounding shadow to darken the
captured raster. The Time Trial arm first establishes
canonical records, then reaches Taj's normal finish hook, requires the finish
presentation binding to remain live, and proves those existing record tables
are byte-semantically unchanged.

```bash
MDKR_AUDIO=0 python3 tests/check_taj_playable.py --build build \
  --rom baserom.us.v80.z64 -v
```

The check is also registered as `taj_playable` in `tools/run_checks.py`. It is
intentionally not a CTest because it needs a caller-supplied supported ROM; the
Taj state and tuning seams remain covered by the ROM-free `taj_mod` and
`taj_physics` CTests; `taj_mod_state_file` separately proves that an atomic
sidecar store creates a missing save directory and leaves no temporary file.

`tests/check_taj_results_portrait.py` carries the same identity through a real
two-player race into Rankings. It captures Taj beside an ordinary Diddy,
requires the project-owned Taj card to match the retail portrait's 40x40
contract, and checks its authored purple, blue, face, and jewel regions against
the unchanged Diddy negative control. The route then returns to Track Select,
starts the race again, and repeats the capture after a full stage/menu teardown;
this protects both the portrait identity and its native display-list/texture
lifetime.

`tests/check_taj_hud_portrait.py` proves the same card in the retail P2
Adventure HUD slot. It selects Taj as P2, reaches the real hub without changing
lead state, and joins the exact HUD identity trace to visible purple, blue,
face, and jewel pixels at the authored portrait anchor.

`tests/check_taj_p2_adventure.py` covers the lead-player seam ordinary Tracks
multiplayer cannot reach. It enters `JOINTVENTURE` through the retail Magic
Codes keyboard, selects visible Taj as P2, and uses the explicitly test-only
`MDKR_TAJ_P2_LEAD_HANDOFF=1` at the safe hub boundary to invoke the production
`swap_lead_player()` transaction. The following real two-player Adventure race
must bind swapped settings slot 0 to live controller port 2, preserve both
viewports, and keep every observed ROM-facing character ID in the retail 0..9
range. `MDKR_TAJ_P2_LEAD_TRACE=1` is observation-only.

`tests/check_taj_speed_profile.py` measures all three vehicles against paired
ordinary controls and holds sustained speed to 1.35x +/-2%. The registered Taj
arm of `tests/check_vehicle_sweep.py` runs every one of the ROM's 47 legal
track/vehicle pairs and requires live Taj identity, carpet/rider presentation,
shield anchoring, and a deterministic car dash witness.

`tests/check_taj_visual_lifecycle.py` injects loss of each picker and race
companion only after a complete pair exists. The surviving half must be cleaned
up atomically, donor rendering must recover during the gap, and a bounded fresh
generation must recompose without stale ownership.

`tests/check_taj_character_select.py` is the rendered roster gate the gameplay
journey cannot replace. It constructs the base-eight, Drumstick-only, T.T.-only,
and complete-ten retail layouts, requires Taj at contiguous indices 8/9/9/10,
navigates a real controller route into each slot, and checks captured pixels and
instrumented model probes for the standing Park Warden/Taj actor, unchanged
ordinary-character hover across every available retail slot, animated Taj
hover, the structurally verified placard-only asset batch, safe-area occupancy
of the surrounding cast, and final Taj identity. Focused captures prove the
authored P2, P3, and P4 placards. A rendered negative control restores the full
Park Warden shadow that originally remained after Taj was scaled for the picker;
the oracle requires production to retain a small grounding shadow while
rejecting the control's large dark footprint. Focused PAL and 21:9 runs qualify
the supported second ROM revision and widescreen safe area; every sampling box
is stated in ROM-logical pixels and mapped through the presentation the
renderer actually composed, because a forced aspect centre-fits the authored
320x240 envelope inside the drawable (21:9 in a 1280x960 capture puts it at
731x549+274+206) rather than stretching it over the frame. Reading those boxes
at raw pixel coordinates samples the letterbox bar, which is how an oracle can
report a placard that is plainly on screen as absent. One injected
allocation failure must
recover, while an exhausted retry budget must visibly fail closed and make the
invisible slot unselectable.
`taj_select_layout` is the ROM-free companion CTest: the same row definition
drives both actor placement and every directional cursor candidate. The picker
gate defaults to OpenGL and accepts `--renderer webgpu`; the release runner
qualifies the complete roster on both shipped renderer paths and adds a focused
21:9 arm.

`tests/check_browser_taj_character_select.py` closes the platform gap through
the shipped shell and a real Chrome/WebGPU process. Starting from a fresh
browser profile, it enters `ABRACADABRA` through the public keyboard route and
joins the runtime model/animation probes to four browser screenshots. It fails
if Taj is not visible before hover, the numbered P1 placard is absent, the pose
does not animate, final identity mapping is wrong, or the former all-caps badge
has replaced the physical actor again. It then flushes IDBFS, reloads the same
isolated profile without entering the code, and requires the physical Taj actor
and placard to return from durable state. The gate is registered as
`browser_taj_character_select` in `tools/run_checks.py`.

`tests/check_browser_taj_persistence.py` rejects the first real
`Module.__mdkrPersist({reason: "taj-mod"})` promise after the C side has replaced
its MEMFS sidecar. It requires the visible C failure path, exact retained bytes,
a successful ordinary retry flush, the matching IndexedDB record, and a fresh
document that restores Taj without re-entering `ABRACADABRA`.

## Adventure trophy series — `tests/check_trophy_series.py`

```bash
python3 tests/check_trophy_series.py -v
```

This starts from a checksum-valid checkpoint with the Dino Domain cabinet
legitimately unlocked, enters it through production collision/dialogue code, and
drives all four championship rounds. Additional arms select the other three
world schedules at the same production entry boundary. Assertions cover all 16
tracks, per-round point accumulation, a stable 32-point tie, gold/silver/bronze
and no-award finals, the trophy cinematic, the rankings QUIT path, post-quit
retry, checksum-valid EEPROM, and a fresh-process cabinet display from reload.

`MDKR_TROPHY_COMPLETE_AFTER` advances lap state only after a real race has run;
`race_check_finish()` still creates the finish order. `MDKR_TROPHY_ORDER` accepts
only a full permutation and runs at rankings init; malformed input is rejected.
Neither control writes points, ranks, trophy bits, cinematics, or save data.

## Save-file fail-safe — `tests/check_save_failsafe.py` (RUN THIS AFTER ANY SAVE/EEPROM CHANGE)

`save/eeprom.bin` is the one piece of **persistent untrusted input** in the port.
On the web build it lives in IndexedDB, so whatever is in it comes back on every
reload — which makes "a bad save puts the game somewhere the player cannot leave"
a permanent trap rather than a one-off.

```bash
MDKR_AUDIO=0 python3 tests/check_save_failsafe.py -v     # ~30 s, muted + headless
```

Four cases, each starting from a wiped `save/` (and wiping it again afterwards — a
leftover save sends FILE SELECT down the resume path and breaks
`check_adventure_hub.py`):

| case | image | asserts |
|---|---|---|
| 1 | 100 of 512 bytes (a torn store) | title screen at ~1134; only the three boot levels loaded; `save/eeprom.bin.bad` byte-identical to the input; `eeprom.bin` back to 512 bytes |
| 2 | 512 random bytes | as above |
| 3 | `wizpigAmulet=7` in slot 0 (checksum **valid**), real started slots in 1–2 | exit 0, no `[CRASH]`, FILE SELECT reached, slot 0 erased, **slots 1–2 untouched** |
| 4 | no save file at all | clean boot and **no** `.bad`/`.tmp` — this is what keeps 1–2 from passing vacuously |
| 5 | `tajFlags = 0x08` (car challenge **beaten but never offered**) + 5 balloons | the kart still drives Timber's Island: path >= 25000 units and <= 45 % of post-hub rows stationary |

Case 3 is the one that matters most: the slot checksum is a plain 16-bit byte sum,
so corrupt bytes satisfy it by chance, and `wizpigAmulet` is 3 bits on disk while
the amulet has four pieces. Values 5 and 7 reach `objects.c`'s
`BHV_DYNAMIC_LIGHT_OBJECT_2` spawner as an unclamped `modelIndex` and SIGSEGV in
the racer collision path ~750 frames into Timber's Island. 4 of 40 randomly
corrupted checksum-valid images hit it before the fix; 0 of 40 after.

Case 5 is the one actually hit in play. `tajFlags` holds "Taj has OFFERED this
challenge" (0x01/0x02/0x04) and "you have BEATEN it" (0x08/0x10/0x20) as two
triples, written minutes apart by two different functions with two different save
flushes. The offer gate (`objects.c:1794`) reads only the OFFERED half and the
auto-offer dialogue never asks, so "beaten but never offered" replays a finished
challenge on every hub entry — and Taj's dialogue holds `disable_racer_input()`
down, so the player just stops. It is measured by DRIVING, not by exit code:
healthy 37767.6 units / 24 % stationary, wedged 14439.4 / 76 %.

⚠️ Three separate fixes are under this check — `eeprom_load()`'s validation in
`platform/stubs_dkr.c`, and the amulet rejection plus the BEATEN-implies-OFFERED
repair in `game/src/save_data.c`. Revert them **one at a time** when
positive-controlling: they fail different cases (1–2, 3, and 5), and reverting more
than one at once hides which you broke. Measured failures for each are tabulated in
`docs/OPEN_ITEMS.md` "savefailsafe".

**The symptom as first reported — "boot drops me straight into a race" — was NOT
reproduced, and cannot be caused by a save file.** (The report was later corrected to
"a genie test we already completed" after normal menu navigation, which is case 5
and does reproduce; the boot-path disproof below is what rules the boot path out.) `menu_file_select_loop()`
calls `init_racer_headers()` (which zeroes `settings->courseId`) three lines before
the resume returns, so `get_track_id_to_load()` always yields levelId 0, and
`courseId` is not in the save format at all. 98 boots across every corruption shape
reach MENU_TITLE at frame 1134. If you see a race load early in an *idle* boot, it
is the ordinary title attract demo (`levelId=18` @~5132, `levelId=28` @~6632,
`numPlayers=0 cutscene=100`), which exits on START.

## Sprint ROM-free units — eleven new CTests

```bash
ctest --test-dir build --output-on-failure \
  -R '^(mod_manifest|mod_registry|mod_source_zip|mod_texture_key|dev_command|enhancement_registry|a11y_model|save_state_container|update_check|gpu_diagnostics|adapter_policy)$'
```

Eleven CTest units added by the [`docs/sprints/`](../docs/sprints/README.md)
work. All are ROM-free and window-free, all run in milliseconds, and
`rom_free_units` carries them into every lane. They are registered in
`cmake/tests.cmake`; `tools/run_checks.py` does not name them individually
because its manifest cross-check covers `tests/test_*.py`, and these are C.

| Unit | What it owns | The assertion that would otherwise rot |
|---|---|---|
| `mod_manifest` | One `pack.ini`, parsed and validated | Over-long name/author/version is **rejected**, not truncated |
| `mod_registry` | Pack discovery, load order, path resolution | Traversal rejected against a bait file that genuinely exists; equal-priority tie-break pinned to an explicit ASCII case fold, so load order cannot shift with locale |
| `mod_source_zip` | One read interface over directory and zip packs | Both kinds go through **one** path validator — mutating only the zip arm fails only the *directory* assertions; and every traversal bait file genuinely exists, so an unguarded implementation would return it |
| `mod_texture_key` | The published texture digest | An exact pinned value, plus a `0xaa`-filled union proving padding bytes are not hashed |
| `dev_command` | Console command parsing | `set` refuses any key outside the schema, so a console cannot become an arbitrary-write primitive |
| `enhancement_registry` | The enhancement table and its authority classes | The table and `mdkr_video_key_is_enhancement()` describe exactly the same set |
| `a11y_model` | The accessibility utterance stream | Coalescing is per category, interruption is per priority, and each CRITICAL case is paired with an identical NORMAL case so the exemption is the only difference |
| `save_state_container` | Save-state file format and validation | Truncation refused at all 88 offsets; a header declaring a payload the file does not contain is refused in both directions |
| `update_check` | Version comparison and the once-a-day interval | `1.10.0` beats `1.9.0` — numeric, not lexicographic; a release supersedes the nightly it came from, and a nightly never supersedes a release |
| `gpu_diagnostics` | The `[GPUINFO]` adapter record | Every candidate carries a non-empty reason, and an adapter name containing control characters or quotes cannot forge a line in the block |
| `adapter_policy` | Choosing an adapter and backend | A preference matching nothing falls back and names the string that missed, rather than failing to start; UNKNOWN class ranks between discrete and integrated in both directions |

Each of these was mutation-checked when it landed — the implementation was
deliberately broken and the suite confirmed to fail — because several of them
guard a silent failure rather than a crash, and a check that passes both with
and without the fix is not a check.

## Draw distance and model detail — `tests/check_enh_draw_distance.py`

```bash
MDKR_AUDIO=0 python3 tests/check_enh_draw_distance.py --build build
```

The assertion that matters is **not** "the frame changed" — it is that the
**live object count per tick is identical** between 100% and 400%. That is what
separates a render cull from an update cull, and it is checked *before* the
hash comparison so a divergence is named rather than merely detected: *"changing
how far the game draws changed which objects exist. The hook is in the wrong
place."*

The sprint warned this could go wrong, and it nearly did. Under `NATIVE_PORT`,
`check_if_in_draw_range()` has **two callers, both authoritative** — they write
`obj->opacity`, which `[SIMHASH]` v3 hashes — and **none in the render path**:
`render_level_geometry_and_objects()` replays the tick's per-viewport decision
out of the route cache instead. Widening that threshold anywhere inside the
function would have been an update-side edit. The setting therefore goes through
a separate read-only predicate that writes nothing and only *adds* draws.

`MDKR_DRAWDIST_TRACE=1` tallies drawn objects per frame, split authored versus
extended. The gate also requires the **authored** count to be identical on every
frame, proving the setting extends rather than re-decides.

The LOD arms run on a 4-player split, not Time Trial: a Time Trial has one
racer already at model 0, so the bias has nothing to hold. The census counts
choices the bias actually *changed* after clamping, so a route that stops
exercising the ladder fails rather than passing on an incidentally different
frame.

## Pack music — `tests/check_mod_music_override.py`

```bash
MDKR_AUDIO=0 python3 tests/check_mod_music_override.py --build build
```

Six headless captures, no audio device, and **no ROM audio anywhere** — the
pack's WAV is a 440 Hz sine the test synthesises, exactly 440 cycles in one
second at 22050 Hz so the loop seam is continuous.

The design constraint the gate exists to hold: the replacement is
**presentation-only**. The sequence player is not skipped — it still posts its
play event and the CSP runs unchanged; the single state change is the player's
volume field, the same one the music slider writes. Skipping instead of muting
would sound identical and break determinism silently, so assertion 5 requires
the `[SIMHASH]` v3 rows *and* the sequence trace to be identical across all
three arms.

Assertion 2 measures a **residual**, not a resemblance: with effects volume at
zero the bus holds nothing but the replacement, so the pack's own synthesised
waveform must fit the capture to 0.006%. Arm C installs the same pack as a
`.zip` with no directory present and requires byte-identical output — which is
what proves the registry now discovers archives.

The positive control stubs the begin hook to return 0; assertions 1–4 all fail.
Assertion 6 keeps passing under that stub, honestly, because it then compares
two equally unmodded arms — recorded in the docstring rather than papered over.

## Opponent skill — `tests/check_enh_ai_difficulty.py`

```bash
MDKR_AUDIO=0 python3 tests/check_enh_ai_difficulty.py --build build
```

The only gameplay-class enhancement, so it is the only one required to *move*
the state stream. Six races per arm on Ancient Lake, and the route is chosen
deliberately: the autopiloted player **wins** there at `authored`, so mean
opponent finish has headroom. On Hot Top Volcano the same fixture finishes the
player last, pinning the metric at 4.0 and making the assertion unfalsifiable —
recorded in the docstring so nobody "simplifies" the route later.

The purity arm is the strong form: the gate builds a binary with the
enhancement **compiled out entirely** (`MDKR_ENH_AI_DIFFICULTY_OMIT`) and
requires 12,000 `[SIMHASH]` v3 rows byte-identical to the `authored` arm across
two seeds. `authored` returns by an early return before any float reaches an
ALU — never a multiply by 1.0f.

The wedge assertion is imported from `check_ai_unstick_opponents.py`, not
restated, and the lap-time floor is derived from the authored best lap and the
scale the binary itself reports rather than hard-coded.

## Free camera — `tests/check_tool_freecam.py`

```bash
MDKR_AUDIO=0 python3 tests/check_tool_freecam.py --build build
```

Detach at a tick, move, re-attach, and require **both** that the `[SIMHASH]` v3
stream is byte-identical to an un-detached run and that the frame after
re-attaching is byte-identical too — with the frames *during* detachment
differing, so the gate is not vacuous.

**This gate has already earned itself.** The free camera substitutes the latched
projection record, which is presentation-scoped and unhashed — but the globals
`cam_rebuild_native_projection()` derives from it are not, and the *next* tick's
visibility pass rebuilds its cull planes from them without refreshing. That
visibility answer gates AI RNG. The first run diverged at tick 2232, 232 ticks
after detaching at 2000. The fix went in the tool, not the gate: a closing hook
re-derives the authored lens through `cam_set_fov()` — the game's own rebuild,
not a saved copy.

Its positive control is three variants of "re-attach by restoring a saved record
instead of ceasing to substitute". Two of them **pass**, which is itself
informative: the port's own `camera_obstruction_projection_matches_render()`
handshake rejects a stale generation and falls back to the authored matrix, so a
naive re-apply is caught by existing machinery. The third — live identity with a
stale lens spliced in, which is what a re-apply would have to do to get past
validation — fails on exactly the intended assertion.

## Crash report — `tests/check_crash_screen.py`

```bash
MDKR_AUDIO=0 python3 tests/check_crash_screen.py --build build
```

The crash screen is **strictly additive**. Every harness here greps
`[CRASH]`/`[FATAL]`, so the gate's first job is proving those markers still
arrive **first and unchanged**, by line index, in both a SIGABRT and a SIGSEGV
arm. It also asserts the exit disposition is the exact value the pre-change
binary produced (`-6` and `-11`), so CI classification cannot shift underneath
the suite.

Two arms rather than one, because a single one would be vacuous: the abort path
fires during asset init with `tick=0` and `track=-1` frozen, so a hard-coded
constant would pass it. The gate therefore requires the two arms to **disagree**
on tick and track, requires `track-name` to be consistent with `track` in both,
and requires `renderer=` and `version=` to follow the real environment.

The report deliberately never spells a fault `SIGABRT` or `SIGSEGV` — those
strings are in `harness_utils.ABORT_MARKERS`, and the gate fails if any of them
appears inside the report line. `MDKR_NO_CRASH_HANDLER=1` suppresses the whole
surface, which the ~9 harnesses that defer to ASan rely on.

## Speed readout — `tests/check_enh_speedometer.py`

```bash
MDKR_AUDIO=0 python3 tests/check_enh_speedometer.py --build build
```

Four arms — baseline, `=0`, `=1` (mph), `=2` (kph) — capturing the same race
frame. `=0` is byte-identical to baseline. `=1` changes pixels **only inside the
readout box**, and the check walks every row outside that box asserting no
differing byte: the weapon panel ends at 88% height and the box starts at 90%,
so "outside the box" is the rest of the interface and the whole world.

The value assertion is not merely monotonic. A frozen sampler is
non-decreasing, so the window must also **start at rest and actually climb** —
the positive control (`return 6.0f`) fails on both halves of that.

Assertion 6 is the one that makes the authority claim real for this row: all
four arms run under `MDKR_STATE_HASH=3` on a route that *reaches a race with the
readout on screen*, and all 3,200 rows must be identical.
`check_enhancement_authority.py`'s own fixture never leaves the menus, so it
cannot observe this — it names this file in `EFFECT_GATES` instead.

## Developer-tool purity — `tests/check_dev_tools_purity.py`

```bash
MDKR_AUDIO=0 python3 tests/check_dev_tools_purity.py --build build
```

Registering a tool in `platform/app/dev_tools.cpp` is a **claim**: that opening
its window cannot move authoritative state. This gate enumerates the table from
the running binary's `[TOOLTABLE]` dump and tests that claim for every entry, so
a tool added later is born gated with no edit here.

For each tool it runs two headless races under `MDKR_STATE_HASH=3` — one with
`Tools.Enabled=0`, one with the tool forced open — and requires the `[SIMHASH]`
streams to be byte-identical. Frame count and race outcome are compared too, so
a tool that merely slows the frame loop enough to shift pacing is also caught.

**Parsing zero rows is a failure.** Every tool is an observer with no body until
its own task lands, so an empty table would otherwise sail through.

## Asset sub-entry bounds — `tests/check_subentry_bounds.py`

```bash
MDKR_AUDIO=0 python3 tests/check_subentry_bounds.py --build build
```

The negative control for `mdkr_asset_subentry()`. Section indices were always
bounds-checked; indices *within* a section were not, and the one accessor that
did compare — `get_misc_asset()` — returned the **section base silently** when
the index was out of range, converting an out-of-bounds read into a confidently
wrong one. That is the failure this gate exists to keep closed.

Five arms. A positive control with no hook must exit 0 with no diagnostic, so
the check cannot pass merely because everything aborts. Two synthetic indices
(1 and `UINT32_MAX`) go through `MDKR_SUBENTRY_TEST_INDEX`, and two live ones
(`100000` and `-1`) go through `MDKR_SUBENTRY_TEST_MISC_INDEX` into the real
`get_misc_asset()` against the real loaded section. Each aborting arm asserts
`returncode == -SIGABRT` **specifically** — a segfault would pass a
merely-non-zero test — and regex-checks that the reported count is a plausible
ROM count rather than the index echoed back.

The companion `asset_subentry` CTest covers the accessor itself with no ROM:
each aborting case runs in a forked child and asserts both the signal and the
message text.

## Hosted ROM checker — `tests/check_rom_checker_page.py`

```bash
MDKR_AUDIO=0 python3 tests/check_rom_checker_page.py --build build
```

`dist/web/rom-check.html` asks a player to hand a 12 MB cartridge dump to a web
page, and this is what makes that a reasonable thing to ask.

**Nothing leaves the machine.** The page's text must contain no `fetch(`, no
`XMLHttpRequest`, no `WebSocket`, no `<form action`, no `sendBeacon`, no
`EventSource`, no `importScripts` and no `@import`; separately, every
URL-bearing attribute and every CSS `url()` in the file is *parsed* and required
to be same-directory relative, which is the general guarantee the literal list
only names mechanisms for. Every local file it does reference must ship beside
it.

**It answers what the engine answers.** The page loads `dist/web/rom-id.js`
rather than copying it — asserted with `check_rom_revision.py`'s own row parser,
so "carries its own revision table" means exactly what that check means by it.
Its verdict logic sits in one DOM-free block between two markers, which this
gate lifts out, loads under `node` beside `rom-id.js`, and runs over every
cartridge dump under `build/roms/`. The sentence it produces must equal the
`[ROM]` line the native binary prints for the same file, character for character
— both sides are handed the same display name, so the whole sentence is compared
and not a suffix of it. A synthesised `.v64` and `.n64` must identify as the
same release and hash to the same whole-image SHA-256 as the `.z64`; without
normalisation before hashing, every byteswapped dump would read as damaged.

`node` absent skips the two runtime assertions with a printed reason; the text
assertions always run.

Verified non-vacuous in three directions: a `fetch(` inserted into the page
fails the first assertion, a suffix appended to the verdict sentence fails the
comparison on all five dumps, and hashing before normalisation instead of after
fails the byte-order assertion on both `.v64` and `.n64`.

## Game-text index census — `tests/check_rom_text_indices.py`

```bash
MDKR_AUDIO=0 python3 tests/check_rom_text_indices.py --build build
MDKR_AUDIO=0 python3 tests/check_rom_text_indices.py --only nav,hub   # subset
```

Before a US 1.0 or European 1.0 ROM can be accepted, the port has to be shown
not to ask it for a text entry it does not have: `us.v77`'s `GAME_TEXT` holds
259 entries where `us.v80` holds 343 (`docs/ROM_REVISIONS.md` §5).

`set_current_text()` range-tests the id it is *given* and then adds the language
offset (`+85` German, `+170` French, `+255` Japanese) **afterwards**, so the
resolved index is never re-tested. That resolved index is what this gate bounds.
`game/src/game_text.c` gained one `NATIVE_PORT` census site at exactly that
point, emitting `[TEXTIDX] section=GAME_TEXT index=… requested=… language=…
count=…`. It is off unless `MDKR_TEXTIDX` is set; `MDKR_TEXTIDX=1` writes to
stderr, and any other value is a **file path** the census is appended to.

The path form is what lets the gate drive the real route gates unmodified — the
nine `nav_*` fixtures, the 20-track sweep, the hub tour, the campaign seams and
the trophy series — by pointing each at a shim named `mdkr64` that execs the real
binary with its own census file. Those gates parse the engine's stderr, so a
second stream interleaved into it would change what they read. Nothing in them
is modified, so the routes driven here cannot drift from the ones the suite runs.

Fails if any observed index reaches 259, printing the index and the route; fails
if any route group launched no engine at all, or if no index was observed
anywhere, because an enumeration over nothing proves nothing. The **coverage is
printed rather than implied** — distinct ids reached, the fraction of the
section they are, and the languages seen — and the docstring states plainly that
the claim is "these routes never resolve an index at or above 259", not "the port
never does". One of the three text-bearing asset sections is instrumented;
`MENU_TEXT` and `LEVEL_NAMES` are not.

**Recorded baseline** (us.v80, 60 engine runs across the five route groups):
maximum resolved index **81**, on `adventure_resume_race.txt @9000 frames
track=54`; 26 distinct ids of the 340 the ROM addresses (7.6%) over 40 accesses;
every access in English. The `nav_*` fixtures and all 20 tracks of the sweep
contribute **zero** — `load_game_text_table()` runs on level entry and a race
triggers no dialogue, so the whole census comes from the hub, the campaign seams
and the trophy cabinet. The printed projection notes that the same ids under
French (+170) reach 251, still in range, and under Japanese (+255) would reach
336, which is why a JP build needs its own enumeration and not this one.

Verified non-vacuous by lowering the ceiling below an observed index: the gate
then failed naming index 50 and the hub route that produced it.

## Enhancement authority — `tests/check_enhancement_authority.py`

```bash
MDKR_AUDIO=0 python3 tests/check_enhancement_authority.py --build build
```

Every enhancement in `platform/enhancement_registry.c` declares an authority
class. This gate tests that claim **in both directions**: a `presentation` row
must leave the `[SIMHASH]` v3 stream byte-identical when flipped to its probe
value, and a `gameplay` row must change it. One direction alone would let a
gameplay-changing setting be mislabelled cosmetic, or a setting that does
nothing be labelled as though it did.

The row list and each row's probe value are parsed from `[ENHTABLE]` lines the
**running binary** emits under `MDKR_ENH_DUMP_TABLE=1`, never from a list in the
test — a second list drifts, and a drifted one still prints PASS while silently
covering one fewer setting. `tests/test_enhancement_registry.c` fails a row that
declares no probe value, so adding an enhancement forces you to say how to
exercise it.

**Parsing zero rows is a failure**, checked before anything else. With every
effect currently unimplemented that is the likeliest way this gate could go
vacuous, and a mutant that drops every parsed row exits 1 on it.

`EXPECTED_INERT` names the rows whose effect is not built yet, each with the
task that closes it. The gate **fails if a listed row starts moving the
stream** — an expectation that has silently become wrong is worse than none.

## Checks whose detail lives with their source

The gates below are registered in `tools/run_checks.py` and run by the complete
suite. Their own module docstrings carry the full assertion list; what follows is
the invocation and what each one owns, so the manifest and this file agree.
`check_ci_contract.py` asserts that agreement: every registered `tests/check_*.py`
must be named here.

### Source-region simulation cadence — `tests/check_simulation_cadence.py`

```bash
python3 tests/check_simulation_cadence.py --build build \
  --rom baserom.us.v80.z64 --roms build/roms
```

Containment gate for NTSC and PAL source clocks, the original/enhanced pacing
mechanism, and the explicit oracle policy. Needs the ROM-revision directory
because the region clause covers both releases, and the PAL release is not the
default `--rom`.

### Field-level byte-swap audit — `tests/check_asset_swap_invariants.py`

```bash
python3 tests/check_asset_swap_invariants.py --rom baserom.us.v80.z64
```

Closes the byte-swap misread class rather than chasing instances: spec-derived
ROM field invariants with byte-reversed positive controls, plus raw
`asset_load()` swap coverage. The defining property of the class is that
deterministically wrong input data keeps every self-consistency oracle green —
BHV_WAVE_GENERATOR's `u16` fields were misread from bring-up until v0.8.

### Retail model corpus — `tests/check_rom_model_corpus.py`

```bash
python3 tests/check_rom_model_corpus.py --rom baserom.us.v80.z64
```

Walks every object and level model asset, its batches, sentinels, and render
states without relying on a reachable game route. Complements the live WebGPU
census: that one answers "what did our routes emit", this one answers "what did
those routes leave unvisited".

### Strict display-list and material census — `tests/check_webgpu_content_census.py`

```bash
python3 tests/check_webgpu_content_census.py --build build \
  --rom baserom.us.v80.z64
```

Runs all nine authored menu routes, every main race, every boss, all four
challenges, the Adventure intro/hub, and the 3P/4P layouts through the real
WebGPU backend under `MDKR_DL_STRICT=1`. Each process must report zero
display-list faults, every material identity and dynamic pipeline key, no shader
guard or pipeline failure, and at least 4x shader-index and 2x per-material
pipeline/bind-group headroom. It also runs the texture-cache content audit
(`MDKR_TEXCACHE_VERIFY=1`) and requires zero stale cache hits, which is the route
class where the historical key-aliasing bug lived.

### Repeated stage/ownership plateau — `tests/check_resource_plateau.py`

```bash
python3 tests/check_resource_plateau.py --build build \
  --rom baserom.us.v80.z64
```

Loads the same race four times in one process through the original Try Again
path and requires CPU pool, audio, GPU, and pointer-registry generations to reach
a plateau. The first generations may warm persistent caches; the later ones may
not grow.

### Rolling attract demo — `tests/check_attract_demo.py`

```bash
python3 tests/check_attract_demo.py --build build --rom baserom.us.v80.z64
```

Title-demo vehicle and path selection, long-soak stability, and input teardown.
GAME-08 was the source-labelled retail defect this owns: `load_level_for_menu()`
forced `VEHICLE_PLANE` for every menu level, so the demo's AI racers consumed the
plane-node family while individual frames still looked plausible.

### Launcher screenshot contract — `tests/check_app_capture.py`

```bash
python3 tests/check_app_capture.py --build build --self-test
```

Launcher screenshot dimensions, contrast, palette, and draw bounds. `--self-test`
additionally requires the broken-direction mutation controls to fail; the runner
always passes it, so the controls are release evidence rather than an optional
extra.

### Real launcher widget input — `tests/check_app_ui_input.py`

```bash
python3 tests/check_app_ui_input.py --build build --rom baserom.us.v80.z64
```

Drives the real ImGui widgets through SDL keyboard and gamepad input in a process
with private app preferences, video config, and save roots. Proves first-run
selection, cross-process reload, gamepad parity, visible save failure with
unchanged desired state, and recovery once the configuration path becomes
writable.

It also presses the Presentation pace radio button and requires ONE press to
persist BOTH pacing keys — `FrameLimit=display` with
`MotionSmoothing=interpolate` for Smooth, and the authored pair for Original —
then reads them back in a fresh process. Configs the quick choice has no name
for (a numeric cap, `40`, `display-margin`) must read back as `custom` rather
than being shown as one of the two named pairs.

### Per-viewport route isolation — `tests/check_viewport_route_isolation.py`

```bash
python3 tests/check_viewport_route_isolation.py --build build-rel \
  --rom baserom.us.v80.z64
```

2P/4P object-route isolation with a last-viewport fade control. The fixture
leaves player one's racer opaque in every early viewport and faded only in the
final one; the production arm must consume the retained object x viewport route,
and the broken-direction arm substitutes the final viewport's route everywhere,
reproducing MP-001 exactly.

### Weather RNG order — `tests/check_weather_rng_order.py`

```bash
python3 tests/check_weather_rng_order.py --build build-rel \
  --rom baserom.us.v80.z64
```

Wizpig 1 (level 37) is the only normal level route that exercises rain. The gate
records the raw authoritative state hash plus the seed immediately around every
splash placement roll and lightning timer reset, and requires presentation
invariance. Only digests and row counts are stored; no ROM-derived state ships.

### Address-domain narrowing — `tests/check_address_domains.py`

```bash
python3 tests/check_address_domains.py
```

Source gate keeping native pointer-to-32-bit crossings inside named domain
boundary helpers. The compiler rejects direct pointer/integer casts; this owns
the two-cast spelling such as `(u32)(uintptr_t)pointer`, which suppresses the
diagnostic while still discarding the high half. Matching-only `!NATIVE_PORT`
branches are excluded.

### CI contract — `tests/check_ci_contract.py`

```bash
python3 tests/check_ci_contract.py
```

Fails closed when the workflows lose required coverage: push/PR/manual policy,
the native matrix, the sanitizer lane, linked wasm and save custody, ROM guards,
release provenance, and the immutable action pins. It carries its own negative
fixtures — every guard is re-run against a deliberately mutated copy of the
source it inspects and must reject it. It also derives the release version from
`CMakeLists.txt`'s `MDKR_VERSION` and requires this file's task list to match
`tools/run_checks.py`'s manifest.

The exact text it pins lives beside it in `tests/ci_contract_manifest.py`: one
`Pin` per literal a named file must, or must never, contain, and one `Control`
per deliberate break of a pin. Reword a workflow step, a refusal message or an
artifact filename and you edit the pin and its control in the manifest; the
check itself only holds what it computes — version agreement, the three-way
frame-limit help comparison, occurrence counts, step ordering, the structural
regexes over CMake and the packagers' archive manifests, the no-write output
guards, and this file's coverage sweep.

### Optimized full-UBSan route gate — `tests/check_full_ubsan.py`

```bash
python3 tests/check_full_ubsan.py --rom baserom.us.v80.z64
```

Builds its own `-fsanitize=undefined,float-cast-overflow` tree, verifies the
required handlers actually linked, runs its positive controls, then drives the
broad content and stateful routes. It holds an exclusive lock on its build
directory: two concurrent runs would interleave a reconfigure with a compile and
fail for reasons unrelated to the code under test.

### Post-engine process replacement — `tests/check_restart_apply.py`

```bash
python3 tests/check_restart_apply.py --build build-rel --rom baserom.us.v80.z64
```

Drives both of the production app's real post-engine transitions from a private
extracted-layout fixture on a deep Unicode path whose package directory contains
a space, exactly as a player's install does — no test seam replaces the
executable or bypasses `main_app.cpp`. For Restart & Apply, successful
replacement is one arm; the post-replacement boot failure and the
handoff-staging failure are the other two, and each must clear the one-shot
controls, return to the visible launcher recovery, and leave the copied ROM and
settings intact. Return to Launcher stages no ROM handoff and must arrive as a
launcher invocation rather than an argument-bearing automation invocation, which
`arg_triage` sends straight to the windowless engine — to the player that reads
as the application closing. The gate asserts the exact invocation each process
in the exec chain received, and the replacement must still find the durable ROM
selection.

### Staged-web provenance fixtures — `tests/check_release_ready_web_provenance.py`

```bash
python3 tests/check_release_ready_web_provenance.py
```

Deterministic fixtures for `tools/ci/check_web_build_provenance.py`, the guard
the release-readiness script runs over a staged `dist/web`. Each case writes a
synthetic `build-info.json` and pins the exact exit code and message: the clean
exact candidate must pass, while a dirty tree, a source commit that is not HEAD,
a missing version, and a wrong version must each be rejected with the reason
named. A provenance guard that stopped inspecting anything fails here instead of
accepting every candidate.
