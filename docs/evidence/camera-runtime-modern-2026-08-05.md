# Modern camera obstruction: current-candidate evidence and release plan

Date: 2026-08-05  
Branch: `codex/camera-obstruction-modern`  
Host: macOS arm64, Clang/AppleClang  
ROM: US v80 Rev 1, validated by the runtime banner

> **Default history (amended 2026-08-06).** During 1.0.5 integration release
> ownership briefly flipped the default so that an unset
> `MDKR_CAMERA_OBSTRUCTION` selected Modern. That flip was **reverted before
> 1.0.5 shipped** (commit 39053a7): Modern is opt-in, and an unset value once
> again selects Observe, exactly as the decision below states. An *unrecognized*
> value also resolves to Observe, with a one-shot diagnostic. The release work
> named in the final section remains open as breadth evidence. The measurements
> in this note are unamended.

> **Superseded (amended 2026-08-07).** The default flipped to Modern in the
> decisions wave, as a dedicated reviewed change and with ownership's approval.
> It was *not* taken on the "all gates green" bar this note recommends — the
> deferred rows below are still deferred. What changed since this handoff is
> that MOTION-01's hard invariants and baselines hold on every route with
> correction as the default path, the 24-arm display matrix holds across the
> aspect/HFOV grid, the seam-release hold closed the retract/expand chatter, and
> exact fan admission cut 24-arm emergency framings from 1585 to 46. See §10.1
> of `docs/architecture/camera-obstruction.md` for the exact accounting of what
> the flip rests on and what it did not wait for. `Camera.Obstruction=observe`
> is the rollback this note asks to be retained, and it is retained — as a
> shipped, tested, player-facing setting.

## Decision

The implementation is suitable for continued opt-in gameplay validation, but it
is **not yet approved as the default**. An unset or unrecognized
`MDKR_CAMERA_OBSTRUCTION` remains Observe. The geometric runtime, final-pose
authority, dynamic motion handling, lifecycle safety, projection matrix, native
performance budget, and Clang ASan/UBSan gates described below pass. Default-on
still requires the release work in the final section; no missing row may be
converted into an exception by weakening a safety assertion.

## Red-team root-cause closeout

| Failure mechanism | Root cause | Implemented control | Release witness |
|---|---|---|---|
| Near plane crosses a wall while the eye point is clear | Center-ray collision does not represent the rendered near-plane footprint | Renderer-derived rounded lens; enclosing-sphere broad phase followed by exact oriented-lens narrow phase | Same-binary Legacy and center-ray controls penetrate; Modern does not |
| A corrected eye becomes unsafe after look-at retargeting | Candidate validation previously preceded final camera orientation | Immutable final-pose tuple contains the final `Camera`, rendered eye, exact guard, projection, shake and retarget state; publisher rejects unvalidated tuples | Alternate, emergency, scripted, fallback and ordinary paths share the sealed publisher |
| Safe fixed endpoints clip during presentation interpolation | The presentation layer interpolates pose/projection independently of fixed-tick endpoint checks | Persist prior full tuple; exact sweep unchanged bases; conservative enclosing-sphere sweep rotated bases; cut on projection, shake, discontinuity or unsafe chord | Ancient Lake reports 4,572 checked transitions, 4,393 clear and 636 conservative cuts |
| A door clips only between fixed ticks | Dynamic collision originally represented only the current object pose | Moving doors publish a conservative 16-sample renderer-path temporal envelope plus outward motion/numeric padding | 17,000-tick post-race route observes moving/open door publication with zero unsafe pose |
| A moving non-door solid has no safe interpolated collision representation | Current-only geometry cannot prove fractional object motion | Render the solid at the current fixed pose and cut any camera chord intersecting its current broad phase | Transition cut path is source-guarded; dynamic runtime reports zero invalid/degraded rows |
| Bounded BVH work reports source failure on healthy complex geometry | A 16-chunk fence was conflated with corrupt cache/input | Every fence exit on both phases sets the exhaustion flag its source classifies on, so the answer is authoritative rather than derived from reported counters. Healthy exhaustion uses the published world AABB as a fail-closed hit and records `sphere_fallbacks`/`exact_fallbacks`; corruption remains `INVALID` | Adventure route (16,989 summaries) exercises zero fallbacks and zero sphere/exact invalid sweeps: since the e85ed75 capacity raise to 32 chunks / 256 triangles no fence binds on any measured route, so the fallback path is held by unit fault injection (`camera_object_bvh` drives each fence class under shrunken caller limits) rather than by route coverage |
| An incomplete dynamic census could be interpreted as empty space | The dynamic adapter previously converted `INVALID` to `CLEAR`, allowing a sphere-clear path to seal an unproven pose | Dynamic `INVALID` remains invalid and degraded. Every Modern publisher, renderer accessor, and snapshot accessor rejects the sidecar for that tick | Source guards and rebuilt 81-test/ROM suites pass; injectable runtime fault fixture remains a default-on gate |
| Recovery could interpolate from an unvalidated camera or object image | Failed camera/dynamic publication did not retire all predecessor state | A failed Modern tick retires its validated tuple and cuts. Failed dynamic publication marks all hard objects discontinuous; the first complete recovery publication cuts them again before interpolation can resume | Structural guards pass; deterministic failure/recovery and non-door-chord fixtures remain required before default-on |
| Camera safety and racer readability fight when the racer is inside a moving door | A bounded probe cannot establish visibility or topology while the focus point remains inside hard geometry | Endpoint readability uses current object geometry, not a temporal AABB; a focus point still overlapping after the bounded local skin exclusion is classified `target_embedded=1`, `target_visible=0`, and separately from a resolvable `target_hidden`. Safe fallback remains publishable because the composition constraint is unavailable | Maximum embedded run is 66 ticks; `target_hidden=0`, zero invalid publication |
| Projection failure restores a stale or mismatched tuple | Fallback state lacked complete display/camera provenance | Restore only the immediately prior selected tuple with matching viewport/layout/FOV/display/shake context, the same viewport world region, and a fresh exact stationary validation | Injected tick 800 passes with zero mismatch or penetration |

Safety remains separate from comfort. A conservative camera cut, elevated shot,
or temporary racer fade can be geometrically correct while still requiring motion
and visual-quality review.

## Implemented engineering contract

1. Fixed-tick authority captures one authored intent per selected camera and
   resolves exactly once without changing logical gameplay camera ownership.
2. Every Modern publisher accepts only a fully constructed and exact-validated
   final tuple. Post-validation re-derives renderer lens bytes to catch mutation.
3. Static and dynamic sources use deterministic stable-ID arbitration. Optional
   dynamic publication failure is release-visible, publishes no Modern sidecar,
   and retires camera/object interpolation history until a complete recovery.
4. Exact work is allocation-free and bounded. Healthy exhaustion on either the
   enclosing-sphere or the exact phase is a conservative hit, never a clear;
   malformed cache/generation/input remains invalid.
5. Projection, camera, dynamic object, and presentation generations are retired
   at level/load boundaries. No prior-level safe camera may cross a discontinuity.
6. Target visibility is a thin current-pose composition query. Hard non-overlap
   occlusion remains a failure. A focus point still overlapping after bounded
   local skin exclusion is reported honestly as `target_embedded=1` and
   `target_visible=0`; it is not mislabeled visible or topologically enclosed.
7. Observe, Legacy, center-ray, and Modern remain same-binary policies. Observe
   is the rollback/default policy until every release gate is complete.

## Current automated evidence

| Gate | Command or route | Result |
|---|---|---|
| Registered native suite | `ctest --test-dir build-cam05-prod --output-on-failure` | 84/84 pass |
| Dynamic temporal oracle | `mdkr_camera_dynamic_temporal_test` | 131 cases at 4,097 renderer samples each; endpoint, translation/rotation/scale, shortest-angle wrap, high coordinates, invalid input, and non-door chord controls pass |
| Dynamic invalid/recovery | `mdkr_camera_dynamic_publication_test` | invalid-until-proven census, failed-image global cut, first-recovery cut, and two-valid-publication interpolation resumption pass |
| Target classification | `mdkr_camera_target_visibility_test` | thin local skin visible; remote wall hidden; thick slab embedded and not visible; invalid source remains invalid |
| Pinned geometry | `mdkr_camera_obstruction_test` | concave stable tie, compact/oversized tunnel, and thin-pillar point-ray false-clear controls pass |
| Source/authority guards | `test_camera_obstruction_observe.py`, `check_camera_dynamic_occlusion.py` | pass |
| Same-binary wall witness | `check_camera_obstruction_runtime.py --frames 5200` | Legacy 217 penetrations; center-ray 276; Modern 95 corrections and zero penetrated/invalid/degraded/hidden rows |
| Dynamic objects | `check_camera_dynamic_obstruction_runtime.py --frames 9000` | 8,992 detail rows; 176 corrections; 16 dynamic source-hit rows; four dynamic-source corrections; zero unsafe/query-invalid rows |
| Projection restoration | `check_camera_projection_fallback_runtime.py --frames 3400` | injected tick 800 passes |
| Emergency readability | `check_camera_emergency_readability_runtime.py --frames 5200` | six fade rows, opacity 153..237; zero unsafe/hidden rows |
| 3P plus T.T. | `check_camera_3p_tt_runtime.py --frames 7000` | 4,199 camera-3 rows; 331 corrections; zero stale/unsafe/source-invalid rows |
| Lifecycle | `check_camera_obstruction_lifecycle_runtime.py` | 49 resets, 26,659 summaries; pause quit/restart and Adventure post-race all pass |
| Projection shapes | `check_camera_obstruction_display_matrix.py --frames 3600` | 24 arms cover 4:3 low/high, 16:9, 21:9, 32:9, portrait and authored/20/capped-140/uncapped-140 FOV; zero unsafe rows; equal-aspect traces identical |
| Isolated 4P performance | `check_camera_obstruction_performance_runtime.py --frames 12500` | 5,491 4P ticks; finalizer p50/p95/p99 60/470/690 us, max 1.240 ms; Observe p99 70 us; state hashes identical |
| Sanitized camera unit suite | ASan/UBSan build plus focused camera CTests | pass with `detect_leaks=0` (LeakSanitizer is unsupported on this macOS host) |
| Sanitized real-ROM policies | ASan/UBSan `check_camera_obstruction_runtime.py --frames 5200` | all three arms pass; no sanitizer/UB marker |

The strict camera geometry/query targets compile under the existing strict
preset. Building that preset's entire repository remains blocked by unrelated,
pre-existing `-Wself-assign` errors in the decompilation fakematches in
`game/src/memory.c`; the production and sanitizer full native targets build.

## Remaining release work, in execution order

The detailed owners, dependencies, CI tiers, artifact schema, review protocol,
and stop conditions live in
[`camera-obstruction.md`](../architecture/camera-obstruction.md#64-release-execution-protocol).

| Order | Required engineering/validation output | Falsifiable exit gate |
|---|---|---|
| 1. Remaining fault injection | Add failed camera publication/stale-tuple mutation and simultaneous resize/projection-fault fixtures; inject aggregate multi-instance query exhaustion | Invalid never becomes clear or publishable; failed images never interpolate; recovery uses a complete fresh tuple; every positive control proves the intended fault was reached |
| 2. Remaining moving-geometry breadth | Add rotating-door-across-exact-lens and real moving-solid resolver-correction fixtures | Modern publishes no unsafe pose; cut/fallback provenance is stable; each positive control reproduces its intended failure |
| 3. Motion quality | Record retract latency, recovery duration, positional/angular jerk, blocker churn, alternate-side flips, emergency dwell and discontinuity density at 20–240 Hz, shake on/off and reduced-motion control | Frozen numeric thresholds pass and the worst 1% clips receive signed design review on 4:3, portrait, ultrawide and split screen |
| 4. Content breadth/current artifact | Rerun all 47 legal vehicle/track rows and all 20 tracks on the current commit; soak repeated model load/free and real object-address reuse | 47/47 and 20/20 pass; zero missing/unclassified/invalid/degraded rows; memory reaches a plateau |
| 5. Toolchain/backend parity | Build current source under GCC and wasm32; run native GL, native WebGPU and real Chromium Modern routes, including live resize plus injected projection failure | Equivalent authority/safety counters; no browser load/timing regression beyond the frozen budget; clean teardown |
| 6. Visibility product policy | Implement reviewed per-viewport batch-local soft-occluder policy or explicitly defer it from this release; capture opposing-view split-screen pixels | No hard-wall fade substitution, cross-viewport contamination, object-state write, hash delta or stale identity |
| 7. Final default review | Archive commands, commit/build IDs, ROM fingerprint, full counter schema, performance distributions and visual sign-off; review rollback | CAM-00–CAM-09 and LENS-01–LENS-08 all green, then flip the default in a dedicated commit with Observe rollback retained |

Until step 7 is signed, the correct release behavior is opt-in Modern with
Observe as the production default. A green safety route does not authorize
silently dropping the remaining motion, backend, content, or manual-review gates.
