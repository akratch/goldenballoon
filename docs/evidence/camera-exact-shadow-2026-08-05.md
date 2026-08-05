# Exact camera-lens shadow diagnostic — 2026-08-05

This is developmental evidence, not a release-budget pass. It records the first
non-publishing, renderer-basis exact-lens shadow of the authoritative ordinary
boom corridor and its paired shadow-off control.

## Identity

| Field | Value |
|---|---|
| Branch / commit | `codex/camera-obstruction-modern` / `fd28b0b8d8b55a672205954adbdec370c70534c6` |
| ROM | US v80, SHA-256 `7de1a8fb2a9558cfc3d9ad4497df698c1e89cf7095ac1531557df2af40ba8bcf` |
| Host | Apple M3 Max, arm64, macOS 26.5.2 (25F84) |
| Build | CMake Debug, Clang strict `-Wall -Wextra -Werror -Wpedantic` |
| Backend / route | headless GL, `race_drive_long.txt`, 5,200 frames, 1280x960, authored FOV, Modern |
| Shadow arm | `MDKR_CAMERA_EXACT_SHADOW=1`, `MDKR_CAMERA_PERF=1`, `MDKR_CAMERA_TRACE=2` |
| Control | identical command without `MDKR_CAMERA_EXACT_SHADOW` |

The invocation used the strict native artifact and the standard deterministic
route. The local ROM path is intentionally omitted; no ROM or ROM-derived data is
stored in this repository.

## Correctness and decision census

| Measure | Result |
|---|---:|
| Selected detail rows | 5,187 |
| Exact guards built | 5,187 |
| Promoted-sphere clears (exact skipped) | 2,038 |
| Promoted-sphere hits / exact invocations | 187 |
| Exact clears (sphere false positives) | 166 (88.8% of invocations) |
| Exact hits | 21 |
| Exact invalid/degraded | 0 / 0 |
| Rows before an authoritative corridor was evaluated | 2,962 |
| Shadow-on/off normalized published-camera differences | 0 / 5,187 |

The normalized comparison removed only the appended `exact_shadow={...}` field
from detail rows; every remaining authored/resolved pose, blocker, validity,
target-visibility, projection, and authority field matched.

## Work census

| Source | Work |
|---|---|
| Static track | 187 sweeps; 222 retained segments; 636 retained triangles; maximum 7 triangles/sweep; 27,756 stationary tests; 6,932 advance iterations; 352 refinement tests; 22 interval fallbacks; 19,792 interval samples; 88 ambiguous sample intervals; 22 publication revalidations; maximum 2,025 stationary tests/sweep; zero invalid sweeps |
| Dynamic objects | 187 sweeps; 8 retained instances; 1,328 model triangles; maximum 1 instance/sweep; maximum 166 model triangles/sweep and in one model; zero invalid sweeps |

The static broad phase is not the observed bottleneck: its maximum of seven
candidates is below the provisional cap of 16. The unbounded sampled interval
fallback dominates the static tail. Dynamic work has a separate whole-model
problem: the retained 166-triangle model exceeds the provisional cap of 128.

## Debug timing census

| Section | Mean | p50 | Reported long tail / max |
|---|---:|---:|---:|
| Exact static | 1.591 ms | 0.590 ms | 17.696 ms |
| Exact dynamic | 0.170 ms | <=0.010 ms | 15.346 ms |
| Finalizer, shadow on | 0.271 ms | 0.140 ms | p99 2.520 ms; max 17.881 ms |
| Finalizer, shadow off | 0.206 ms | 0.140 ms | p99 2.470 ms; max 17.274 ms |

These are Debug/diagnostic timings with detailed stderr tracing and are not the
optimized release verdict. They are sufficient to reject the current algorithm:
the deterministic work census exceeds the stationary-test and dynamic-triangle
fences independent of host scheduling.

## Decision and next gate

- Keep the shadow non-publishing and default-off.
- Keep the sampled fallback only as a ROM-free correctness oracle.
- Implement a bounded continuous convex cast and prove zero false clears against
  that oracle; work exhaustion returns conservative `HIT`.
- Add stable per-model triangle acceleration before exact dynamic publication.
- Rerun this pair in optimized artifacts, then expand to the 24-arm display/FOV,
  multiplayer, moving-solid, concave/tunnel, browser, and breadth matrices.

The operational acceptance sequence and stop conditions are authoritative in
[`camera-obstruction.md`](../architecture/camera-obstruction.md).

## Follow-up: swept-SAT fast path

Commit `1b6ca47` added an analytic swept-SAT separating proof and conservative
entry candidate before the retained reference solver. Repeating the same shadow
arm produced the identical decision census (2,038 sphere clears, 166 exact clears,
21 exact hits, zero invalid/degraded), with this replacement work/timing census:

| Measure | Result |
|---|---:|
| Static analytic tests | 636 |
| Analytic revalidation misses | 0 |
| Static stationary tests | 44 total; maximum 4/sweep |
| Advance / refine / interval fallback / samples | 0 / 0 / 0 / 0 |
| Exact-static Debug mean / p50 / p99 / max | 0.022 / 0.020 / 0.120 / 0.154 ms |
| Exact-dynamic Debug max | 0.066 ms |

A seeded 256-case analytic/reference differential corpus also agrees on status,
blocker, and hit fraction. Two conservative entry candidates miss revalidation
and fall through to the reference clear proof; none uses interval sampling, and
maximum per-case work remains within 130 stationary tests. This is a substantial
measured improvement, not PERF-01 closure: arbitrary orientation/scale breadth,
explicit work-cap injection, GCC/wasm, optimized timing, and all release routes
remain mandatory.

## Follow-up: bounded fixed-tick grazing proof

Commit `842d62fd1c4e4d83d4a5e8a7938c257b18864c7e` removes the sampled
fallback from the fixed-tick API. A conservative SAT entry that does not
revalidate now enters a left-first adaptive interval proof using the
1-Lipschitz clearance bound, capped at 96 interval tests plus 16 contact
refinements. Exhaustion is `INVALID`; the two-phase dispatcher retains the
promoted-sphere hit and marks the result degraded. The 1,024-sample solver is
reachable only through `mdkr_camera_rounded_lens_sweep_reference()`.

ROM-free results:

- The exact tangent regression takes the bounded production branch, reports zero
  fallback/sample/exhaustion work, and still returns the conservative hit. The
  reference API independently takes its sampled fallback and returns the same hit.
- The seeded 256-case analytic/reference corpus agrees on status, blocker, and hit
  fraction, with zero production fallback/sample/exhaustion work and at most 114
  stationary tests.
- Strict Clang `-Wall -Wextra -Werror -Wpedantic`, focused ASan/UBSan, and all
  80 registered native CTests pass. The unrelated full strict aggregate still
  rejects historical `memory.c` Fakematch self-assignments; the camera-focused
  strict targets are clean.

Repeating the same 5,200-frame optimized shadow arm produced the unchanged
2,038/166/21 sphere-clear/exact-clear/exact-hit census and zero invalid/degraded
results:

| Measure | Result |
|---|---:|
| Static SAT / revalidation miss | 636 / 0 |
| Bounded tests / exhaustion | 0 / 0 |
| Stationary / advance / refine | 44 / 0 / 0 |
| Reference fallback / samples | 0 / 0 |
| Maximum static candidates / stationary tests | 7 / 4 |
| Exact-static optimized mean / p50 / p99 / max | 0.022 / 0.020 / 0.120 / 0.118 ms |
| Exact-dynamic optimized mean / max | 0.003 / 0.067 ms |

This closes the production reachability defect, not the release gate. Injected
work-cap boundaries, arbitrary-orientation/scale fuzzing, GCC/wasm breadth, the
full route matrix, and the 166-triangle dynamic-model acceleration remain open.

## Follow-up: dynamic narrow-phase census

Commit `b433b2b` adds a profiled object-local adapter and separates retained model
faces from faces that survive the core swept-AABB. Repeating the same shadow arm
keeps all decisions and invalid/degraded counters unchanged:

| Dynamic measure | Result |
|---|---:|
| Instance sweeps / retained instances | 187 / 8 |
| Model faces scanned / AABB-rejected / narrowed | 1,328 / 1,294 / 34 |
| Stationary tests | 6 |
| Maximum model faces / narrowed faces / stationary tests per sweep | 166 / 6 / 3 |
| Exact-dynamic optimized mean / max | 0.003 / 0.070 ms |

The earlier 166-face figure was a linear-scan breadth measurement, not 166 exact
SAT tests. Current-route narrow work is comfortably inside the provisional fence,
but the scan itself has no asset-independent bound. PERF-02 therefore still
requires immutable per-model chunks/BVH, corruption/cap injection, and fail-closed
overflow behavior before dynamic exact decisions can publish.

## Follow-up: bounded dynamic BVH census

Commit `d8c8bb7` replaces the linear per-model scan in both dynamic enclosing-
sphere and exact-lens phases with immutable eight-triangle chunks and an auxiliary
deterministic balanced BVH. Geometry and stable IDs stay in source order. Cache
publication validates leaf coverage, topology, and parent containment; query-time
generation, layout, node, chunk, and integrity checks return `INVALID` rather than
turning malformed state into a cull. Aggregate per-corridor limits are four
instances, 64 node visits, 16 retained chunks, 128 retained triangles, and 128
exact stationary tests.

The same optimized 5,200-frame Modern/shadow route reports:

| Measure | Result |
|---|---:|
| Enclosing-sphere dynamic sweeps / retained instances | 52,684 / 3,147 |
| Sphere nodes / rejected nodes | 49,551 / 20,109 |
| Sphere retained chunks / triangles | 6,240 / 49,790 |
| Maximum sphere instances / nodes / chunks / triangles | 1 / 37 / 8 / 62 |
| Exact dynamic sweeps / retained instances | 187 / 8 |
| Exact nodes / rejected nodes | 134 / 52 |
| Exact retained chunks / triangles | 19 / 152 |
| Exact AABB-rejected / narrowed / stationary | 118 / 34 / 6 |
| Maximum exact instances / nodes / chunks / triangles | 1 / 29 / 5 / 40 |
| Maximum exact narrowed / stationary | 6 / 3 |
| Sphere / exact invalid sweeps | 0 / 0 |
| Exact-static mean / p99 / measured max | 0.023 / 0.110 / 0.104 ms |
| Exact-dynamic mean / p99 / measured max | 0.003 / 0.100 / 0.105 ms |

The exact decision census remains 166 clears and 21 hits with zero degraded
results. A paired shadow-off/on rerun normalized only the diagnostic
`exact_shadow={...}` field; both 5,187-row streams hash to
`0930efd13694508cfedb5d18ea7b0edca497c2be4dc1e8acd136bacc5bc9def8`.

Validation at this commit: all 80 registered native CTests pass; strict Clang
builds the native target and camera-focused targets; focused ASan/UBSan passes
with leak detection disabled because the Apple ASan runtime rejects that option.
The full strict aggregate remains blocked only by the pre-existing Fakematch
self-assign warnings in `game/src/memory.c`.

This closes the structural scan defect, not PERF-02's release gate. Executable
ROM-free BVH equivalence, corrupt-node/index and every-cap fault injection,
model-address reuse, whole-ROM work/memory/load census, moving-solid correction,
GCC/wasm, browser, 1P–4P, and display/FOV breadth remain mandatory. Observe stays
the default and the exact result remains non-publishing.

## Follow-up: executable BVH equivalence and fault gate

Commit `da93930` registers the ROM-free `camera_object_bvh` CTest against the
actual production cache translation unit. It does not maintain a parallel
test-only index. The test covers:

- byte-identical full-world versus indexed sphere and exact rounded-lens results
  at identity and across 64 deterministic rotated/uniformly-scaled inputs;
- node, chunk, triangle, and exact stationary-test cap exhaustion;
- NaN node bounds, non-Boolean leaf tags, duplicate children, corrupt chunk
  ranges, incomplete coverage, and parent bounds that no longer contain a child;
- stale model generation and same-address/new-generation reuse.

All malformed/cap/stale cases return `INVALID`, or are rejected by the same
pre-publication validator used for built caches. The first differential run found
a real API defect: indexed clear results were zeroed rather than using the core's
canonical `fraction=1` and infinite clearance. The production sphere and exact
indexed APIs now canonicalize clear hits, and the byte comparison passes.

The gate passes in the optimized native build and under strict Clang plus
ASan/UBSan. The registered native suite is now 81/81. Remaining PERF-02 evidence
is aggregate multi-instance-cap injection, actual repeated model/object
load/free/address reuse, whole-ROM work/memory/load bounds, and moving-door/solid
breadth; cross-compiler/browser/display breadth remains part of SHADOW-02 and
RELEASE-01.

## Follow-up: exact-path recovery and measured fence sizing

The 16-chunk/128-triangle limits quoted above are superseded. Three commits
close the fence half of this record.

`55ea056` gives the exact path the conservative recovery the sphere path already
had. A fence-shaped `INVALID` from the model kernel falls back to the same
enclosing-sphere sweep of the instance's published world AABB and is counted as
`exact_fallbacks`; a corruption-shaped `INVALID` still fails closed. Mutation in
both directions pins the split: forcing every exact query into degradation
absorbed all 642 kernel failures with zero `INVALID` sweeps, while
corruption-shaped failures kept all 2,487 `INVALID` classifications.
Adventure-postrace lifecycle failures fall from 6,451 rows to 2.

Those two rows were one transitioning 166-triangle door saturating the exact
chunk budget outright, so the best answer available on those ticks was
conservative geometry containing the resolved eye (`penetrated=1` on ticks 3466
and 3467). Re-running every camera route with the fences lifted to
1024/4096/32768 recorded each query's true demand: the 17,000-tick
adventure-postrace route peaks at 39 node visits, 17 chunks, 134 triangles, and
nine stationary tests, and the 5,200/9,000/7,000/3,400/3,600-frame routes never
exceed 33 nodes, seven chunks, 54 triangles, and eight stationary tests. Only
that door saturates anything. `e85ed75` therefore raises
`MAX_RETAINED_CHUNKS` 16 -> 32 and its coupled `MAX_QUERY_TRIANGLES` 128 -> 256,
which admits that model's whole 21-chunk/166-triangle traversal. The 64-node and
128-stationary-test fences are unchanged; 64 already admitted its 41-node tree.
Neither raised constant sizes any storage, since the only fence-sized array is
the unchanged node stack, so the raise costs zero bytes.

`38be676` makes the exhaustion answer authoritative rather than derived. All six
fence exits in the exact rounded-lens kernel set
`MdkrCameraObjectOcclusionExactWork::exhausted`; the consumer's comparison of
published limits against reported work is retained only as a subordinate
fallback, and it cannot see the node-stack fence at all. Each flag site is
load-bearing under its own unit arm. No fence is reached across the 16,989
postrace summaries since the capacity raise, so the recovery path is held by
`camera_object_bvh` fault injection under shrunken caller limits rather than by
route coverage.
