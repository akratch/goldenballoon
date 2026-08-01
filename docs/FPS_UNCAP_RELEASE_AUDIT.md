# FPS Uncap Broad-Release Audit and Fix Scope

- Status: **release blocked pending final clean-archive qualification**
- Audit type: historical parallel source review plus a live closure ledger
- Historical audit boundary: commit `8763dbf` through `5dfaec0`, including the
  working tree observed on 2026-08-01
- Branch observed: `feature/uncapped-presentation`
- Current implementation status: **the 1.0.1 production scope is intentionally
  narrowed to authored visual frames; focused fixes are locally verified, while
  the final tracked-archive build and artifact qualification remain open**

## Current closure ledger — 2026-08-01

This ledger records the current working tree. It does not rewrite the historical
audit snapshot below, and it is not final release evidence. In particular, the
tree still contains release-relevant untracked files, so a local pass cannot yet
prove that `git archive HEAD` contains the same implementation.

**Resolved** means the implementation and its focused local regression evidence
are green. **Pending** means an acceptance gate or release-boundary condition is
still open. Every resolved row must be rerun from the final clean tracked archive
before publication.

The latest local ROM-free baseline is 59/59 CTest tests passing on 2026-08-01
(50 non-GPU and 9 GPU), including the launcher captures, real keyboard/gamepad
input, same-session save retry, DPI transition, GL swap-interval fallback,
callback-latch unit, surface policy unit, user-path migration, viewport routing,
input queue, presentation
snapshot, and presentation-packet tests. Focused integration runs in this work
session also passed fixed-ticket schedules, arbitrary presentation rates,
simulation cadence, adopted pacing, weather RNG order, and 2P/4P viewport/input
oracles. Those integration results remain provisional until repeated from the
final archive.

| Item | Status | Current implementation and evidence |
|---|---|---|
| `REL-001` | **Pending** | The dependency-tracking contract in `tools/ci/check_release_ready.sh` covers the new production, macOS, and test files, but they are still untracked in the live tree. Closure requires committing them and passing the clean `git archive HEAD` build. |
| `CFG-001` | **Resolved** | Engine handoff now starts from `mdkr_video_config_defaults()` and is a one-shot promotion; `video_config_runtime` covers pattern-initialized defaults, precedence, 240, and repeat-handoff rejection, and adopted-pacing integration reached the requested policy. |
| `PRES-001` | **Resolved by scope reduction** | Production motion smoothing and delayed display-list replay are disabled for 1.0.1. A walk delayed until after task `K+1` begins can observe rewritten viewport, matrix, vertex, texture, and nested display-list dependencies from mutable arenas; partial freezing is not sufficient release evidence. Non-Original Frame Limit policies therefore submit only new authored images and never swap duplicates. |
| `PRES-002` | **Resolved by scope reduction** | Camera snapshot and bank experiments remain diagnostic infrastructure, but 1.0.1 performs no camera interpolation or retained replay. Cutscene, split-screen, UI, and vehicle geometry use the same authored task path as Original mode. Re-enabling smoothing in a future release requires an immutable `{T,T+1}` ownership model and new endpoint/midpoint evidence. |
| `SIM-001` | **Resolved** | Weather authority is interleaved after object authority and before HUD authority. `check_weather_rng_order.py`, `check_authored_rng_compat.py`, and the final fixed-ticket/rate matrices reported stable state, event, input, PCM, and non-weather control digests. |
| `APP-001` | **Resolved** | Return to Launcher is an orderly requested transition through `app_relaunch`, engine/host teardown, and synchronous diagnostic-log shutdown before exec. `app_lifecycle` exercises two lifecycles, more than 320 KiB of log traffic, and visible relaunch failure. |
| `GPU-001` | **Resolved** | Callback-thread GPU mutation is replaced by C11 atomic, generation-scoped latches plus unique ref-counted adapter/device request contexts. Focused CTest, direct TSan, and ASan/UBSan runs cover callbacks before generation commit, during retry, after timeout, and after replacement with exactly-once retirement. |
| `REL-002` | **Resolved** | The Windows release job explicitly installs and probes MinGW Python. `check_ci_contract.py` pins the package and `python3 --version` step. |
| `WEB-001` | **Resolved** | Pages deployment checks the exact linked `dist/web/mdkr64_web.wasm` before upload; CI-contract mutation coverage pins that artifact boundary. |
| `TEST-001` | **Resolved** | Adopted-pacing telemetry parsing is separate from per-fault policy. The configure, acquire, surface-view, encoder, and ImGui arms reach their own assertions; the focused adopted-pacing run passed both legal renderer-stop boundaries. |
| `MP-001` | **Resolved** | A bounded object-by-viewport/pass cache preserves local opacity and routing through opaque/FX, shadow, and water consumers. The unit and WebGPU/GL 2P/4P broken-direction oracle reported exact state with no final-viewport contamination. |
| `INPUT-001` | **Resolved** | Overflow recovery publishes a neutral ticket and then reconstructs held buttons, stick, and presence; same-ticket disconnect/reconnect retains held lanes. `input_tick_queue`, fixed schedules, and the focused two-human binding oracle passed. |
| `APP-002` | **Resolved** | Zero-size drawables skip ImGui construction and unavailable/occluded launchers use a bounded event wait. App shell and adopted-pacing smokes remain responsive and bounded. |
| `GPU-002` | **Resolved** | Alpha and present modes are resolved from each generation's capabilities. AppHost owns adopted-device recovery as a transactional prepare/drain/commit transition; focused loss injection resumed WebGPU rendering with 344/344 submissions retired, while forced recovery failure stopped visibly without switching to GL. |
| `PKG-001` | **Resolved** | Controller mappings are resolved from `SDL_GetBasePath()` and package self-tests launch from an unrelated directory for the Windows and Linux layouts. |
| `UX-001` | **Resolved** | Escape and controller B share the popup/confirmation/Settings/root back stack; `app_ui_policy` covers popup-first handling and key-repeat rejection. |
| `UX-002` | **Resolved** | Per-setting edit state survives frames, commits on Enter or blur, preserves invalid/failed text, and exposes retry. Real input/persistence smoke passes. |
| `UX-003` | **Resolved** | Slider preview is transient and durable commit occurs on deactivation. `app_ui_policy` proves 100 preview updates coalesce to one transaction and failed state remains retryable. |
| `UX-004` | **Resolved** | ROM candidates validate and persist before replacing the last-known-good ROM. `check_shell_dropfile.py` passed valid replacement, invalid retention, and Cancel recovery. |
| `UX-005` | **Resolved** | The launcher declares a 640x480 logical minimum, collapses to top navigation, clamps widths, and stacks controls. The 640x480-at-2.0x capture and content validator pass. |
| `UX-006` | **Resolved** | UI scale is persisted and exposed from 0.75x through 2.00x, with non-compounding metrics and pre-frame application. Policy, input, and minimum-size capture gates pass. |
| `TEST-002` | **Resolved** | Every launcher smoke has isolated preference/config/save roots. A guarded test-only contract drives real SDL keyboard and virtual-gamepad events through ImGui, persists 240, reloads it, and rejects partial/stale injection; `app_ui_input_persistence` passes. |
| `TEST-003` | **Resolved** | `check_app_capture.py` validates dimensions, palette, contrast, content regions, and draw bounds, with blank/offscreen/low-contrast mutations; normal, minimum-size, and DPI captures pass. |
| `REL-003` | **Resolved** | Web publish and release provenance inspect `git status --porcelain --untracked-files=all`; developer provenance records dirty state and hosted release paths fail closed. CI-contract mutations cover both cases. |
| `UX-007` | **Resolved** | Framebuffer-scale changes rebuild the atlas before `NewFrame` and retire renderer font resources safely. The 1x to 2x to 1x smoke and content validator pass. |
| `UX-008` | **Resolved** | The reviewed embedded Roboto Medium asset is retained for reproducibility, with distinct title/body/caption sizes and colors plus responsive density. Capture-content gates cover hierarchy and contrast. |
| `UX-009` | **Resolved** | README and `docs/APP_SHELL.md` explicitly scope native accessibility to keyboard/gamepad operation, visible focus, scaling, contrast, and restrained motion, without claiming a VoiceOver/UI-Automation semantic tree. |
| `GL-001` | **Resolved** | A valid GL launcher survives swap-interval rejection and installs an honest bounded 60 Hz software pacer; `app_gl_swap_interval_fallback` passes. |
| `TEST-004` | **Resolved** | Real Settings input now enters an unwritable location, shows the error, restores access, activates Retry in the same session, persists 240, and clears the failure; `app_ui_input_persistence` passes. |

### Cross-audit closure check

The earlier independent and adversarial reports were rechecked against the live
tree:

- enhanced-cadence audio is now credited per completed fixed ticket, and the
  30/45/60/120/uncapped enhanced matrix reports identical PCM, service counts,
  state, events, and input;
- every presentation policy uses the same exact suspension-gap predicate and
  equality semantics; unit tests cover below/at/above the boundary;
- production endpoint/intermediate replay is disabled for 1.0.1; non-Original
  policies never walk or swap a duplicate retained image;
- primitive-opacity normalization uses the alpha-zero authored denominator;
- the rendering API returns frame-open success, so WebGPU queue saturation is
  not counted as a rendered image;
- restart-scoped launcher settings are promoted into the immediately following
  engine session;
- mutable native settings and saves use the centralized SDL preference path,
  with packaged migration and immutable-bundle tests;
- first-frame WebGPU dumps pre-arm the final output target;
- the browser F3 readout derives authored presentation FPS from
  `Module.__mdkrFrames` rather than wrapping global rAF callbacks;
- canonical Golden Balloon archive names and exact no-DLL Windows contents are
  enforced; and
- the public-surface merge/tip/history checks, macOS 13 strict deployment target,
  privacy-step CI pins, and browser-rate release-checklist entry are present.

The prior request for automatic WebGPU-to-GL fallback is superseded by an
explicit release policy: WebGPU is the qualified default, GL is an opt-in
diagnostic backend, and a WebGPU failure must stop visibly and cleanly rather
than silently changing renderer. The adopted recovery gate now proves that this
fail-closed path preserves ownership and destroys every object exactly once.

Portable publication is also scoped explicitly. Linux builds/tests with
`BUILD_TESTING=ON`, then must content-validate both default WebGPU and explicit
GL launcher frames under Xvfb plus Mesa software rendering before and after
extracting the portable archive. Windows still builds, test-runs, checks imports,
packages, and launches from the extracted archive, but 1.0.1 publication is held
because the hosted runner cannot provide the required stable GPU gate. The macOS
workflow exercises the mounted unsigned DMG through LaunchServices and the
default WebGPU shell. Headless unit lanes exclude tests labelled `gpu` instead
of attempting the shell under SDL's dummy driver.

## Purpose

The remainder of this document preserves the historical implementation handoff
for the FPS-uncap audit. It defines the defects and evidence requested at the
audit snapshot; it does not override the current 1.0.1 closure ledger above. In
particular, historical requests for positive interpolation witnesses are future-
work requirements, not release criteria for the authored-frame-only 1.0.1
policy.

Line numbers are snapshot anchors, not durable identifiers. Implementers must
locate the named function or symbol again before editing because the working tree
was changing during the audit.

## Non-goals

- This document does not implement any fix.
- It does not authorize removal of tests, weakening of assertions, or converting
  release failures into warnings.
- It does not redefine the Original-cadence compatibility target. If compatibility
  with `8763dbf` is no longer required, that decision must be documented separately
  before changing RNG or simulation-order expectations.
- It does not make OpenGL the automatic fallback for WebGPU failures. Current
  fail-closed backend behavior is intentional.
- It does not authorize shipping ROM-derived data.

## Executive release decision

Do not publish a broad release until all P1 items are closed and all P1 acceptance
tests pass from a clean tracked archive. P2 items that can corrupt input, select
the wrong render pass, hang the launcher, or make required controls unreachable
must also be closed before broad release. Purely visual P3 work may be deferred
only through an explicit release-owner decision.

At the final audit snapshot:

- The native build completed.
- CTest passed 46/46 tests.
- `tests/check_ci_contract.py` passed after concurrent corrections.
- `tests/check_presentation_matrix.py` failed every retained deformation,
  particle, vertex-color, and semantic-effect witness.
- `tests/check_app_adopted_pacing.py` passed ordinary GL/WebGPU capped and
  uncapped arms, then failed its `host.surface-view` fault arm.
- `git diff --check 8763dbf` passed.
- Twenty-two release-referenced files were still untracked.

Passing CTest is therefore not release evidence for the known presentation,
packaging, lifecycle, or ImGui interaction failures below.

## Severity and closure rules

- **P1 — release blocker:** must be fixed and independently verified before any
  broad build is published.
- **P2 — required hardening:** must be fixed before broad release unless the
  release owner explicitly narrows supported functionality and documents it.
- **P3 — polish/portability:** can be deferred only with a named follow-up and no
  misleading product claim.
- An item is not closed because its code changed. It is closed only when its
  acceptance evidence passes from a clean, tracked source tree.

## Required implementation order

The work should be divided into five independently reviewable workstreams.

1. **Source and gate integrity**
   - Close `REL-001` first so clean-checkout results are meaningful.
   - Close `TEST-001`, `REL-002`, `WEB-001`, and `REL-003` before treating CI as
     release evidence.
2. **Simulation and presentation correctness**
   - Close `CFG-001`, `PRES-001`, `PRES-002`, and `SIM-001` before tuning visual
     interpolation behavior.
   - Close `MP-001` and `INPUT-001` before multiplayer qualification.
3. **Renderer and lifecycle hardening**
   - Close `APP-001` and `GPU-001` before testing return-to-launcher or device
     recovery.
   - Close `GPU-002`, `APP-002`, and `GL-001` afterward.
4. **ImGui UX excellence**
   - Close behavioral defects `UX-001` through `UX-004` first.
   - Design `UX-005` and `UX-006` together; responsive layout must account for
     every supported scale.
   - Finish DPI, typography, screenshot, and accessibility qualification last.
5. **Clean release qualification**
   - Produce artifacts only from a clean archive.
   - Run the complete matrix in “Final release evidence” below.

## Dependency summary

```text
REL-001 tracked tree
  ├─ clean native/web/package builds
  ├─ release provenance evidence
  └─ trustworthy CI and archive validation

CFG-001 initialized video handoff
  ├─ launcher Frame Limit interaction tests
  ├─ 240/uncapped adoption evidence
  └─ restart/persistence qualification

PRES-001 authored tick ownership
  ├─ deformation/particle/effect matrix
  └─ high-rate visual qualification

PRES-002 camera authority ── cutscene smoothing qualification
SIM-001 weather RNG order ── Original-cadence compatibility oracle

GPU-001 callback synchronization
  └─ GPU-002 generation-specific recovery capability selection

UX-006 user UI scale
  └─ UX-005 responsive layout + UX-007 DPI rebuild matrix
```

---

## Detailed fix specifications

### REL-001 — Track every release-referenced source, script, test, and fixture

- **Priority:** P1
- **Observed surfaces:** `CMakeLists.txt:176,189-196,592,885,978`,
  `.github/workflows/macos-release.yml:78,80,106,177,194,196`,
  `.github/workflows/release.yml:169`, and `tools/run_checks.py:59,91,94,117,183`.
- **Current untracked groups:**
  - macOS release helpers: `build_release_sdl2.sh`,
    `dmg_mount_cleanup.sh`, `release_sdl2_config.sh`,
    `run_launchservices_probe.py`, `stamp_macos_provenance.sh`,
    `verify_unsigned_dmg.sh`, and `verify_unsigned_release.sh`;
  - production: `platform/app/app_activation.{h,mm}` and
    `platform/user_paths.{c,h}`;
  - Windows: `tools/check_windows_imports.sh`;
  - checks/fixtures: `test_user_paths.c`, `check_cli_version.cmake`, five Python
    checks, and three input scripts.
- **Failure mechanism:** local builds see the files, but a clone or `git archive`
  does not. Builds can fail outright or silently omit intended coverage.
- **Required change:** review every untracked file for intended inclusion, add all
  intended files, remove only genuine local artifacts, and add an automated
  dependency-tracking contract using `git ls-files --error-unmatch`.
- **Do not:** solve this by making tracked workflows ignore missing files.
- **Acceptance evidence:**
  1. No release-relevant `??` entry in `git status --short`.
  2. Extract `git archive HEAD` into a temporary directory.
  3. Configure, build, and run native ROM-free gates there.
  4. Run CI-contract and script syntax checks there.
  5. Prove every workflow/CMake/check-runner path resolves to a tracked file.

### CFG-001 — Initialize launcher-to-engine video configuration before resolving

- **Priority:** P1
- **Code:** `mdkr_video_config_handoff_to_engine()` in
  `platform/video_config_runtime.c:220`; resolver contract in
  `platform/video_config.c:625`.
- **Failure mechanism:** local `MdkrVideoConfig resolved` is uninitialized before
  `mdkr_video_config_resolve()`. The resolver expects default values and source
  ranks to exist. Random source ranks can reject launcher/environment settings;
  untouched strings and numeric values remain indeterminate and are copied into
  global runtime state.
- **Observed symptom:** one audit run resolved WebGPU/240 as
  `policy=original rate=0`; later runs appeared correct when stack contents
  changed.
- **Required change:** initialize `resolved` with
  `mdkr_video_config_defaults()` before reading and resolving layers. Consider a
  helper that returns a completely initialized resolved object so the precondition
  cannot be forgotten at another call site.
- **Acceptance evidence:**
  - all schema slots have valid values, terminated strings, modes, and sources;
  - launcher-staged 240 and uncapped reach the engine on both GL and WebGPU;
  - environment and CLI locks retain correct precedence;
  - a build using `-ftrivial-auto-var-init=pattern`, where supported, passes;
  - a negative control removing initialization fails deterministically.

### PRES-001 — Carry explicit authored-tick ownership through retained presentation packets

- **Priority:** P1
- **Code:** packet capture in `gfx_start_frame()` near
  `platform/fast3d/gfx_pc_dkr.c:4840`; deformation/effect lookup near lines 3635
  and 3723; tick validation in
  `platform/fast3d/gfx_presentation_packet.c:431-440`.
- **Failure mechanism:** the walked task is stamped with
  `g_simTickCounter - 1`, but replay asks for `g_simTickCounter`.
  `gfx_presentation_packet_lookup_deformation()` rejects before identity or
  ordinal matching because current packet tick and requested tick differ by one.
- **Affected output:** animated model vertices, point-trail positions, point-trail
  RGBA, shield/magnet semantic matrices, and any dependent deformation counters.
  Root matrices and primitive alpha can still interpolate, hiding the failure.
- **Required change:** define an immutable authored-tick token at the game-side
  display-list authoring boundary. Carry it through task submission, real walk,
  capture, freeze, and replay. Capture and lookup must use the same token. Do not
  derive packet ownership independently from a mutable global counter. For 1.0.1
  Experimental modes, if the replayed task's true next deformation/effect recipe
  is unavailable, hold its exact current authored bytes. A backward
  `{T-1,T}` blend is not an acceptable substitute for the missing `{T,T+1}`
  pair.
- **Acceptance evidence:**
  - unit test advances the live simulation counter before walking an older task
    and still resolves via its explicit token;
  - packet validates previous/current adjacency internally;
  - a semantic observer at replay `G_VTX` decode reports nonzero endpoint checks,
    byte-identical expected/actual hashes, and zero alpha-zero mismatches;
  - retained deformation, particle position/color, and semantic-effect paths
    report exact current-endpoint holds and zero phase-shifted interpolation;
  - positive interpolation hits are required only after a real forward endpoint
    is captured and identified; they are deliberately not required by the
    1.0.1 byte-hold safety policy;
  - a wrong-token negative control fails closed.

### PRES-002 — Snapshot the exact camera bank authored for each viewport

- **Priority:** P1
- **Code:** cutscene-camera clears at `game/src/thread3_main.c:398` and 974;
  snapshot selection in `platform/presentation_snapshot_walk.c:138`;
  snapshot capture call from the `fb_update` boundary in `platform/stubs_dkr.c`.
- **Failure mechanism:** `gCutsceneCameraActive` is cleared before snapshot
  selection reconstructs camera IDs. A display list authored from cameras 4–7 can
  therefore be paired with snapshots from gameplay cameras 0–3.
- **Required change:** latch the exact camera ID used by each viewport at the
  authoritative camera-selection/render boundary and include those IDs in the
  tick-stamped snapshot. An alternative is to capture before teardown, but the
  snapshot must not infer ownership from a flag already cleared by lifecycle code.
- **Acceptance evidence:**
  - real cutscene run captures and interpolates bank 4+ while active;
  - midpoint pixels/camera transforms correspond to the cutscene camera;
  - entry and exit are marked discontinuous rather than blended across banks;
  - paused cutscene behavior is explicitly tested;
  - existing P2 and 3P TT camera coverage remains green.

### SIM-001 — Restore authored RNG order on weather-enabled tracks

- **Priority:** P1
- **Code:** `scene_weather_tick()` at `game/src/thread3_main.c:658` and 1194;
  object/HUD authority in `game/src/tracks.c:2473-2543`; weather RNG in
  `game/src/weather.c`.
- **Failure mechanism:** fixed authority currently advances weather RNG before
  object/HUD RNG. Canonical render order was object traversal, weather, then HUD.
  The first weather draw therefore starts from the wrong seed relative to object
  and HUD draws, and subsequent gameplay RNG diverges.
- **Required change:** interleave the authoritative weather tick after the 1P
  object passes and before the corresponding HUD RNG work. Remove the earlier
  standalone call. Resolve separately whether weather should restore the gameplay
  seed, because the `8763dbf` baseline bracketed render-weather RNG.
- **Acceptance evidence:**
  - weather-enabled 1P Original-cadence raw RNG/state oracle;
  - route reaches at least one splash roll and lightning reset;
  - normal versus skip-render and 30 versus 60 present produce identical
    authoritative RNG/state;
  - enhanced cadence uses its documented presentation/gameplay streams;
  - non-weather routes remain unchanged.

### APP-001 — Replace direct overlay re-exec with an orderly launcher-return lifecycle

- **Priority:** P1
- **Code:** `returnToLauncher()` in `platform/app/ui_overlay.cpp:118-128` and
  `DiagLog_install()` in `platform/app/diag_log.cpp:116-137`.
- **Failure mechanism:** `execvp()` preserves stdout/stderr pipe descriptors but
  destroys the detached reader thread. The new process duplicates inherited
  stderr as its “real” output and forwards its new log into the old unread pipe.
  The pipe fills and blocks the logging thread, then the application.
- **Additional risk:** direct exec bypasses normal engine, renderer, audio, and
  application teardown ordering.
- **Required change:** make Return to Launcher a requested lifecycle transition.
  Let the engine unwind, release renderer/audio state, flush saves, and shut down
  diagnostic logging. Restore stdout/stderr to retained original descriptors,
  close private tee descriptors, and set close-on-exec on descriptors not intended
  for a child. Launch only after cleanup.
- **Do not:** apply `FD_CLOEXEC` only to the current pipe ends without restoring
  stdout/stderr; `dup2` semantics and SIGPIPE can still produce a broken process.
- **Acceptance evidence:**
  - launcher → game → launcher → game works repeatedly on macOS and Linux;
  - re-exec test writes more than twice the platform pipe capacity;
  - final marker reaches the new log and console forwarding remains live;
  - GPU/audio teardown diagnostics occur before relaunch;
  - failed relaunch falls back visibly without hanging.

### GPU-001 — Synchronize spontaneous WebGPU device-loss state

- **Priority:** P1
- **Code:** renderer globals near `platform/fast3d/gfx_webgpu.c:80-98`, fatal
  latching near line 487, callbacks near lines 653-687, and callback registration
  at line 1184.
- **Failure mechanism:** an `AllowSpontaneous` callback accesses plain readiness
  and generation variables plus merely-`volatile` status while main-thread frame,
  recovery, handoff, and teardown code accesses the same objects. `volatile` is
  not synchronization; this is a C data race and undefined behavior.
- **Required change:** callback code should atomically latch a generation-scoped
  loss/error record. The main thread consumes it at a complete-frame boundary and
  performs every GPU-object mutation. If individual globals remain shared, use
  real C11 atomics with documented memory ordering.
- **Acceptance evidence:**
  - ThreadSanitizer-compatible ROM-free threaded callback test;
  - race callback against polling, host-device release, and generation replacement;
  - exactly one matching-generation loss is consumed;
  - stale callbacks are ignored;
  - recovery/teardown never mutates GPU objects from the callback thread.

### REL-002 — Install Python explicitly in the Windows release job

- **Priority:** P1
- **Code:** `.github/workflows/release.yml:155-170`,
  `.github/workflows/windows-validate.yml:62-72`, and
  `CMakeLists.txt:429-440`.
- **Failure mechanism:** the release job configures `BUILD_TESTING=ON`, which
  requires Python, and later invokes a Python-backed asset verifier, but its MSYS2
  package set omits Python. Success depends on incidental runner-global state.
- **Required change:** add the appropriate Python package to the release job and
  compare required tool/package sets in CI contract tests.
- **Acceptance evidence:** a fresh `windows-latest` runner passes
  `python3 --version`, CMake configure, build, asset verification, non-GPU CTest,
  import validation, and packaging.

### WEB-001 — Gate Pages deployment on linked wasm wave-table validation

- **Priority:** P1
- **Code:** `.github/workflows/web-demo.yml:25-56`; reference implementation in
  `.github/workflows/correctness.yml:164-167`.
- **Failure mechanism:** `web-demo` can independently build and publish a newly
  linked artifact without running the browser-only wave-table layout check.
- **Required change:** run `tests/check_wave_visible_table.py` against the exact
  `dist/web/mdkr64_web.wasm` that will be uploaded, before artifact upload or Pages
  deployment.
- **Acceptance evidence:** removal mutation fails CI contract; corrupt linked
  table control fails deployment; valid wasm passes; the check consumes the same
  artifact later uploaded.

### TEST-001 — Make the AppHost surface-view fault arm internally satisfiable

- **Priority:** P1
- **Code:** `host_telemetry()` and `run_host_fault()` in
  `tests/check_app_adopted_pacing.py:168-180,400-450`.
- **Failure mechanism:** `host_telemetry()` always requires
  `attempts == presented + unavailable`, but an acquired surface whose view
  creation fails is deliberately classified later as `(1,0,0)` with one encode
  failure. The generic check raises before case-specific validation runs.
- **Required change:** separate parsing from policy validation. Apply balanced
  attempt accounting to ordinary/acquisition outcomes, then validate acquired
  encode failures with their explicit state tuple.
- **Do not:** increment “unavailable” merely to appease the test unless that is the
  intended public metric definition and every telemetry consumer is updated.
- **Acceptance evidence:** configure, acquire, surface-view, encoder, and ImGui
  initialization fault arms all reach their own assertions; each has a broken
  direction control; ordinary GL/WebGPU 240 and uncapped arms remain green.

### MP-001 — Preserve per-viewport opacity and pass routing

- **Priority:** P2
- **Code:** authority traversal in `game/src/tracks.c:2473-2543`; render pass
  admission around lines 2642-2653 and 2739-2752.
- **Failure mechanism:** authority runs every viewport and leaves the final
  viewport's result in shared `obj->opacity`. Every actual viewport then uses that
  shared final value to choose opaque versus transparent/FX pass before computing
  its local draw-range opacity.
- **Required change:** retain per-object, per-viewport opacity and pass admission
  from the fixed prepass, then consume that data in render. A pure recomputation is
  acceptable only if it preserves the intended fixed-authority contract.
- **Acceptance evidence:** 2P and 4P fixture with an object faded only for the last
  viewport; earlier viewport pass/order, blending, shadow, water effect, and pixels
  remain stable as the other camera moves.

### INPUT-001 — Resynchronize held input after queue overflow neutralization

- **Priority:** P2
- **Code:** `platform/input_tick_queue.c:149,184-210`; current overflow unit near
  `tests/test_input_tick_queue.c:173`.
- **Failure mechanism:** overflow calls `force_port_neutral()` and then records the
  overflowing held sample as `observed`. After one neutral consume, the next
  identical held sample compares equal and queues no recovery transition.
- **Required change:** preserve neutral observed state or set an explicit
  full-resync flag. The next periodic full capture must reconstruct current held
  buttons, sticks, and presence without a physical release.
- **Acceptance evidence:** overflow → consume neutral → capture identical held
  sample for next ticket → held state recovers. Repeat for button, analog, and
  connected-controller state.

### APP-002 — Idle the WebGPU launcher when drawable presentation is unavailable

- **Priority:** P2
- **Code:** `AppHost::endFrameWebGpu()` in
  `platform/app/app_host.cpp:573-640`; interactive loop in
  `platform/app/main_app.cpp:453-477`.
- **Failure mechanism:** zero drawable and timeout/occluded acquisition return
  without presentation, while the outer loop immediately constructs another
  ImGui frame and retries. A minimized launcher can consume a full core.
- **Required change:** expose minimized/unavailable state from `AppHost`; wait for
  events with a bounded timeout or use a 16–33 ms idle cadence. Skip ImGui frame
  construction for a zero-size minimized window while retaining restore/quit
  responsiveness.
- **Acceptance evidence:** persistent occluded and zero-drawable injections for at
  least 250 ms have bounded attempt counts and low CPU; quit/restore remains
  prompt; presentation resumes cleanly.

### GPU-002 — Resolve surface capabilities per recovered adapter/surface generation

- **Priority:** P2
- **Code:** `wgpu_choose_alpha_mode()` at
  `platform/fast3d/gfx_webgpu.c:821-843`, `wgpu_choose_present_mode()` at
  861-949, surface configuration near 1043, and native recovery near 8069.
- **Failure mechanism:** function-static `resolved` and selected modes survive
  destruction of the old adapter/surface. The new generation may not advertise
  those modes, turning recovery into another validation/fatal failure.
- **Required change:** cache only user/requested policy. Resolve supported alpha
  and present modes from each generation's capability set, preferably in the
  existing configure query.
- **Acceptance evidence:** generation A advertises Immediate/Opaque; generation B
  advertises FIFO/Auto only; injected loss recovers using FIFO/Auto and submits and
  presents another frame.

### PKG-001 — Resolve bundled controller mappings relative to the executable

- **Priority:** P2
- **Code:** package placement in `tools/package_windows_zip.sh` and
  `tools/package_linux_appimage.sh`; runtime search in
  `platform/platform_sdl_min.c`; packaged-path policy in
  `platform/user_paths.c`.
- **Failure mechanism:** packages place `gamecontrollerdb.txt` beside the binary,
  but runtime searches only macOS packaged resources or paths relative to CWD.
  AppImage and Windows shortcut/CLI launches commonly use another CWD.
- **Required change:** add an absolute executable-base candidate using
  `SDL_GetBasePath()` and release it with `SDL_free()`.
- **Do not:** fix by changing CWD in `AppRun`; relative ROM arguments intentionally
  remain caller-CWD-relative.
- **Acceptance evidence:** extract Windows zip and Linux AppImage/tar, launch from
  an unrelated directory, and require the log to name the extracted package path
  and a nonzero mapping count.

### UX-001 — Implement the advertised Escape back-stack

- **Priority:** P2
- **Code:** overlay event handler in `platform/app/ui_overlay.cpp:157-184` and
  footer text near line 350.
- **Failure mechanism:** the footer says “Esc back,” but only F1/F10 and controller
  B have overlay navigation behavior. The engine swallows unhandled input while
  the overlay is open.
- **Required change:** add non-repeat Escape handling with the same state ladder as
  controller B: dismiss confirmation, then leave Settings, then close overlay.
  Allow an open ImGui popup/combo to consume Escape first.
- **Acceptance evidence:** event-level tests for every stack level, popup-first
  cancellation, key repeat, and parity with controller B.

### UX-002 — Preserve and commit open-domain text edits on blur

- **Priority:** P2
- **Code:** `drawSetting()` string branch in
  `platform/app/ui_settings.cpp:230-248`.
- **Failure mechanism:** the field buffer is reconstructed from the old desired
  value every frame. Only Enter commits. Clicking elsewhere silently discards the
  user's typed aspect, FOV, or texture-pack value despite the comment promising
  Enter/blur behavior.
- **Required change:** keep per-key edit buffers with synchronization rules for
  external config changes. Commit on Enter or `IsItemDeactivatedAfterEdit()`.
  Retain invalid text and show a local validation error rather than reverting it.
- **Acceptance evidence:** valid blur persists and publishes correctly; invalid
  blur keeps the text with a visible error; tab switching and external lock/state
  changes do not overwrite an active edit unexpectedly.

### UX-003 — Separate live slider preview from durable persistence

- **Priority:** P2
- **Code:** sliders in `platform/app/ui_settings.cpp:257-269`; persistence in
  `platform/video_config_runtime.c:486-569,596-655`.
- **Failure mechanism:** every drag frame serializes the full config, writes a
  temporary file, flushes, fsyncs/commits, replaces it, and may fsync the parent
  directory. In-game overlay dragging can hitch while the race continues.
- **Required change:** maintain transient slider state. Apply live preview through
  a non-durable publication path if required, then persist once on deactivation or
  explicit Apply. Coalescing/debounce must still guarantee the final value on
  orderly shutdown.
- **Acceptance evidence:** 100 drag updates produce one durable transaction; final
  desired/live values are correct; save failure is visible and retryable; no
  overlay-frame hitch exceeds the chosen budget.

### UX-004 — Make ROM replacement transactional

- **Priority:** P2
- **Code:** acquisition and `RomPanel_setRom()` in
  `platform/app/ui_rom.cpp:84-139`; ready/Cancel rendering near 188-230.
- **Failure mechanism:** a candidate immediately overwrites the current valid ROM
  and validation result. An invalid candidate makes `ready=false`, which also
  removes the Cancel control that could restore the prior selection.
- **Required change:** validate into temporary candidate path/info. Commit state
  and preferences only after success. Preserve the previous valid selection while
  displaying candidate error details and a working Cancel action.
- **Acceptance evidence:** valid ROM → Change → invalid picker result, typed path,
  and dropped file all preserve old Play state; Cancel restores the prior view;
  accepting a valid replacement updates preferences exactly once.

### UX-005 — Make the launcher responsive at every supported window size

- **Priority:** P2
- **Code:** resizable window creation in `platform/app/app_host.cpp:90-98` and
  151-161; fixed metrics in `platform/app/ui_common.cpp:10-15`; root layout in
  `platform/app/ui_launcher.cpp`; ROM path row in `platform/app/ui_rom.cpp:102-107`.
- **Failure mechanism:** no minimum size or responsive breakpoint exists. A fixed
  244 px rail, 340 px controls, 544 px ROM path, and same-line action can be
  clipped and unreachable.
- **Required change:** define the actual minimum supported size and implement
  adaptive layout: collapse navigation to top tabs below a breakpoint, clamp
  controls to `GetContentRegionAvail().x`, stack path/action controls, wrap header
  text, and calculate card heights from content.
- **Acceptance evidence:** screenshot plus keyboard/gamepad navigation at 640x480,
  800x600, 1280x800, minimum size, and every supported UI scale. ROM field, Use
  This Path, Play, Settings, and Quit must remain visible or scroll-reachable.

### UX-006 — Expose and persist the existing UI-scale capability

- **Priority:** P2
- **Code:** `AppTheme::setUiScale()` in
  `platform/app/app_theme.cpp:108-121`; no production caller currently exists.
- **Failure mechanism:** the API and comments promise player scaling, but
  `g_uiScale` remains 1.0 because no schema/pref/UI control invokes it.
- **Required change:** define a persisted live setting, recommended supported
  range 0.75–2.0 for qualification, and apply it at a safe point before launcher
  and overlay frames. Rebuild fonts only when required; do not compound style
  scaling.
- **Acceptance evidence:** 0.75, 1.0, 1.5, and 2.0 persist across restart; font,
  button, navigation, and control metrics scale; responsive-layout matrix remains
  unclipped; focus indication stays visible.

### TEST-002 — Isolate launcher smoke preferences and drive actual ImGui input

- **Priority:** P2
- **Code:** CTest smoke definitions near `CMakeLists.txt:993-1006`;
  `MDKR_APP_AUTOPLAY_VIDEO_SET` in `platform/app/main_app.cpp:87-107`.
- **Failure mechanism:** smoke tests use real shared preferences and can load a
  remembered ROM, so “first-run” coverage depends on the host. FPS settings tests
  call the runtime setter directly, bypassing focus, navigation, selection, and
  activation behavior.
- **Required change:** give every smoke a unique temporary prefs/config/save root.
  Add a test-only production entry that queues real SDL input events rather than
  calling the setting API. Exercise keyboard and gamepad navigation through the
  actual widgets.
- **Acceptance evidence:**
  1. Real shared prefs may contain a valid ROM, but isolated first-run still
     reports no ROM and disabled Play.
  2. Process A selects Frame Limit 240 through ImGui and exits.
  3. Process B reloads and proves persisted 240 and truthful restart state.
  4. Process C boots GL and WebGPU and proves the scheduler adopted 240.
  5. A control removing keyboard navigation or disconnecting the combo fails.

### TEST-003 — Validate screenshot content, not file existence

- **Priority:** P2
- **Code:** smoke capture in `platform/app/main_app.cpp`; package validation in
  `macos/Scripts/verify_unsigned_release.sh`.
- **Failure mechanism:** current gates require only a nonempty BMP. Blank,
  all-background, clipped, offscreen, or invisible-text output can pass.
- **Required change:** add a deterministic decoder/validator for dimensions,
  non-flat histogram, expected palette/content regions, and draw bounds. Maintain
  a perceptual baseline only where renderer output is stable enough. Upload every
  failed capture.
- **Acceptance evidence:** mutation controls clearing the framebuffer, moving the
  panel offscreen, and matching foreground/background colors all fail.

### REL-003 — Include untracked inputs in dirty/provenance policy

- **Priority:** P2
- **Code:** `tools/web/build_web.sh:64-70`,
  `tools/web/publish_demo.sh:50-59`, and local provenance stamping scripts.
- **Failure mechanism:** `git diff --quiet` ignores untracked files. A build can
  consume source absent from HEAD while claiming `source_dirty=false` and binding
  the artifact to the HEAD commit.
- **Required change:** release/publish paths must fail on
  `git status --porcelain --untracked-files=all`. Developer artifacts should record
  `source_dirty=true` and, ideally, a content/tree digest.
- **Acceptance evidence:** an untracked CMake-referenced source causes release
  publication/stamping to fail or developer provenance to record dirty state.

### UX-007 — Rebuild fonts safely after per-monitor DPI changes

- **Priority:** P3
- **Code:** initial atlas setup in `platform/app/app_theme.cpp:124-148`; per-frame
  framebuffer scale updates in `platform/app/app_host.cpp`.
- **Failure mechanism:** fonts are rasterized only for the initial monitor. Moving
  across different-DPI displays changes framebuffer scale without rebuilding the
  atlas or renderer font texture.
- **Required change:** detect scale/display changes before `NewFrame`, rebuild the
  atlas and backend texture safely, and preserve user UI scale.
- **Acceptance evidence:** simulated and real 1x → 2x → 1x moves retain logical
  size, improve raster sharpness, and do not use stale texture handles.

### UX-008 — Improve typography hierarchy and content density

- **Priority:** P3
- **Code:** font setup in `platform/app/app_theme.cpp:138-145`; launcher panel
  composition in `platform/app/ui_launcher.cpp` and `ui_settings.cpp`.
- **Failure mechanism:** body, title, and small text all use Roboto Medium. The
  visual result is heavy and flattens hierarchy. Wide windows also produce large
  unused regions while Settings remains vertically dense.
- **Required change:** use Regular for body/caption and Medium or Semibold for
  titles/actions; retain clear size hierarchy; constrain readable content width or
  use an adaptive two-column Settings layout at wide breakpoints; strengthen
  section grouping without decorative excess.
- **Acceptance evidence:** launcher and overlay captures in light/dark target
  environments, supported sizes/scales, and mixed DPI; no clipping, low-contrast
  text, or ambiguous action hierarchy.

### UX-009 — Define the native accessibility contract explicitly

- **Priority:** P3 unless broad release promises screen-reader accessibility
- **Current state:** keyboard navigation exists, but ImGui does not expose a native
  VoiceOver/UI Automation semantic tree by itself.
- **Required decision:** document whether the supported target is keyboard/gamepad
  accessibility plus scaling/contrast, or full native screen-reader support. If
  the latter is required, scope an accessibility bridge or native accessible shell
  controls and add platform qualification.
- **Acceptance evidence:** published accessibility claims match tested behavior;
  focus visibility, scaling, contrast, and reduced-distraction behavior are
  explicitly qualified.

### GL-001 — Do not abort the diagnostic launcher solely on swap-interval rejection

- **Priority:** P3
- **Code:** OpenGL host initialization in `platform/app/app_host.cpp:109-119`.
- **Failure mechanism:** a valid GL context is discarded when
  `SDL_GL_SetSwapInterval()` is unsupported, although engine GL already tolerates
  and reports effective interval behavior.
- **Required change:** warn, inspect the effective interval, and continue with a
  bounded software host pacer when necessary.
- **Acceptance evidence:** stubbed swap-interval rejection with valid context still
  renders, remains CPU/queue bounded, and reports its fallback honestly.

### TEST-004 — Exercise Settings persistence failure through the real UI

- **Priority:** P3
- **Code:** save-result messaging in `platform/app/ui_settings.cpp`; persistence
  failure return in `platform/video_config_runtime.c`.
- **Failure mechanism:** source has a visible `SAVE_FAILED` path, but no test drives
  it through an actual Settings widget or proves recovery.
- **Required change:** interact with a setting using an intentionally unwritable
  isolated configuration location, then restore writability and retry.
- **Acceptance evidence:** visible error; unchanged desired/live state after
  failure; retained editable value where appropriate; successful retry persists
  and clears the error.

---

## ImGui UX acceptance standard

The UX work must satisfy the following behavioral standard, not only produce
attractive screenshots:

- **Predictability:** every advertised key/button works and keyboard/gamepad
  behavior is symmetric where labels claim parity.
- **User agency:** Cancel never destroys the last known-good ROM or setting;
  invalid input remains correctable.
- **Immediate, honest feedback:** live versus restart-required settings are
  distinct; save/validation failures are visible and never claim success.
- **Responsiveness:** no control becomes unreachable at a supported size, UI scale,
  or DPI; focus is always visible.
- **Performance:** UI interaction does not perform synchronous durable writes per
  rendered drag frame or busy-spin while occluded.
- **Hierarchy and restraint:** title, body, caption, primary action, and warning
  levels are visibly distinct without excessive decoration.
- **Accessibility:** keyboard navigation, scale, contrast, and the chosen native
  semantics target are documented and tested.

## Required test matrix

### Core timing and presentation

- Original, Restored, and Remastered policy where applicable.
- Host-pacing policies: Original, Display, 30, 60, 120, 144, 165, 240, and
  Uncapped. For the 1.0.1 release path every policy must retain the authored
  image count (approximately 30 Hz NTSC or 25 Hz PAL), perform zero delayed
  replay walks, and perform zero duplicate swaps.
- OpenGL and WebGPU.
- Normal render, intentionally skipped render, catch-up tick, resize, occlusion,
  and return from occlusion.
- Single-player normal race, weather-enabled race, real cutscene, battle challenge
  particle witness, and forced shield/effect witness.
- 2P and 4P opacity/pass-routing scenario.

### Input and UI

- Mouse, keyboard, and gamepad.
- Confirmation, Settings, root overlay, combo popup, and ROM replacement states.
- Window sizes: 640x480, 800x600, 1280x800, and declared minimum.
- UI scales: 0.75, 1.0, 1.5, and 2.0.
- Framebuffer/DPI transitions: 1x, 2x, and 1x → 2x → 1x.
- Valid and invalid field input, blur, Enter, restart-required state, locked state,
  save failure, and retry.

### Platform and packaging

- macOS unsigned qualified path and, when enabled, trusted signed/notarized path.
- Windows fresh runner with only declared MSYS2 packages.
- Windows portable zip launched from an unrelated directory.
- Linux AppImage/tar launched from an unrelated directory.
- Web artifact built, wave-table checked, ROM-scanned, save-custody checked, then
  uploaded without rebuilding between check and upload.
- Launcher → game → launcher repetition on macOS and Linux.
- WebGPU device-loss generation replacement and capability downgrade.

## Final release evidence

All commands must be executed from an extracted clean `git archive HEAD`, except
platform signing/notarization steps that necessarily operate in their workflow.
Use an externally supplied, legally dumped ROM via `MDKR_TEST_ROM`; never add it to
the archive or logs.

Minimum evidence set:

```sh
git status --short
git ls-files --error-unmatch <every release dependency>
git diff --check 8763dbf

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure

python3 tests/check_ci_contract.py
python3 tests/test_public_surface.py
python3 tests/check_app_adopted_pacing.py \
  --build build-release/mdkr64 --rom "$MDKR_TEST_ROM"
python3 tests/check_presentation_matrix.py \
  --build build-release/mdkr64 --rom "$MDKR_TEST_ROM"
python3 tests/check_fixed_tick_schedules.py \
  --build build-release/mdkr64 --rom "$MDKR_TEST_ROM"
python3 tests/check_arbitrary_presentation_rates.py \
  --build build-release/mdkr64 --rom "$MDKR_TEST_ROM"
python3 tests/check_simulation_cadence.py \
  --build build-release/mdkr64 --rom "$MDKR_TEST_ROM"
```

Also require the repository's full release profile from `tools/run_checks.py`,
all platform package self-tests, and the workflow-specific macOS, Windows, Linux,
and web checks. Do not accept a local dirty-worktree pass as release evidence.

## Definition of done

- [ ] Every P1 item is fixed and has its required regression test.
- [ ] P2 correctness and required-control issues are fixed or the affected
      functionality is explicitly removed from broad-release scope.
- [ ] Every Frame Limit policy reports the authored image count, zero production
      replay walks, zero duplicate swaps, and byte-identical authoritative state,
      event, input, and PCM streams. Motion smoothing remains disabled in the
      resolved 1.0.1 configuration.
- [ ] Original-cadence weather RNG oracle is stable across render schedules.
- [ ] Cutscene, split-screen, UI, particles, and vehicle parts remain on the
      authored task path under every exposed Frame Limit policy.
- [ ] Input overflow reconstructs held state without release/repress.
- [ ] Return to Launcher completes lifecycle teardown and cannot deadlock logging.
- [ ] WebGPU callback/recovery tests pass under thread-race instrumentation.
- [ ] ImGui keyboard/gamepad behavior, transactional edits, responsive layout,
      persistence failure, UI scale, and screenshots are qualified.
- [ ] Every published Linux, macOS, and web artifact passes from clean tracked
      sources; Windows remains explicitly non-published until its GPU gate is
      qualified.
- [ ] Provenance records the exact clean source used to build each artifact.
- [ ] Release owner signs off on any deferred P3 item and updates public claims.

## Areas reviewed without an additional concrete defect

The audit did not find a separate issue in rational capped/uncapped scheduler
arithmetic, exact fixed-ticket placement, audio service clock/queue control,
three-slot snapshot publication/failure retention, backward identity deletion,
GL fence backpressure, current frame-admission propagation, or shipped-backend
selection. Those areas still require regression coverage after adjacent fixes, but
they should not be rewritten without new evidence.
