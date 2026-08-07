# Camera obstruction engineering handoff

Date: 2026-08-05
Branch: `codex/camera-obstruction-modern`
ROM used by runtime gates: US Rev 1 at the original repository path; no ROM or
ROM-derived data is present in this worktree or its commits.

> **Default history (amended 2026-08-06).** During 1.0.5 integration release
> ownership took a default flip so that an unset `MDKR_CAMERA_OBSTRUCTION`
> selected Modern. That flip was **reverted before 1.0.5 shipped** (commit
> 39053a7). Modern is opt-in — through the launcher's Camera obstruction setting
> or `MDKR_CAMERA_OBSTRUCTION=modern` — and unset selects Observe, which is the
> state this handoff recommends. The deferred gates recorded below remain open
> as breadth work and were never converted into safety exceptions. Everything
> else in this note stands as the record of the state at handoff.

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

## Handoff decision

The current Modern implementation is validated for opt-in native gameplay and
for continued design/content evaluation. It is not approved as the production
default. Unset or unknown `MDKR_CAMERA_OBSTRUCTION` remains Observe; retain that
rollback until the explicitly deferred breadth, backend, motion, and product
policy gates below are completed or formally waived by release ownership.

This handoff stops at the user-requested validation boundary. The heaviest
47-row/20-track, browser/wasm/backend, long resource-soak, and signed manual
motion-review gates were not rerun on this final commit and are not implied by
the passing focused evidence.

## Landed commits

| Commit | Purpose |
|---|---|
| `e3abfaf` | Exact renderer-derived final-pose authority, transition validation, dynamic temporal handling, fail-closed publication, and honest embedded-target telemetry |
| `ad69d74` | Runtime, lifecycle, display, performance, projection, 3P+T.T., dynamic, source, and browser release-gate hardening |
| `d426111` | Red-team closeout, evidence ledger, ordered release waves, rollback and stop conditions |
| `0619537` | Production-backed temporal-envelope oracle, dynamic invalid/recovery state machine, non-door chord fixture, explicit target-visibility policy, and pinned concave/tunnel/pillar geometry |

The branch is intentionally not merged and the default is intentionally not
flipped. A default change must remain a separate reviewed commit.

## Implemented safety contract

- Modern resolves once per selected physical camera after authored behavior and
  publishes only through the presentation sidecar; logical gameplay camera
  authority remains unchanged.
- Every ordinary, alternate, emergency, scripted, recovery, and fallback result
  is a complete renderer-derived camera/lens/projection/shake tuple and must pass
  exact final-pose validation before publication.
- Render and the resolver hold separate projection records for one viewport. The
  guard is built from the presentation lens, which is a superset of every image
  that viewport can publish, while render draws the latched record and owns the
  generation handshake. A held validated tuple is reissued only into the world
  region it was validated for.
- Dynamic source `INVALID` remains invalid. A failed census publishes no Modern
  sidecar, cuts every eligible hard object, retires camera/object interpolation
  history, and forces the first complete recovery image to cut again.
- Moving doors use a 16-sample renderer-transform temporal AABB with analytic
  outward padding. Moving non-door solids render at the current fixed pose and
  cut intersecting camera chords.
- Healthy bounded work exhaustion becomes a conservative world-AABB hit on the
  enclosing-sphere and the exact phase alike; every fence exit flags the
  exhaustion its source classifies on, and corrupt input/cache/generation
  remains invalid.
- A target behind a remote blocker is `HIDDEN`. A focus still overlapping after
  bounded local-skin exclusion is `EMBEDDED` and not visible. Invalid source or
  numeric input is never collapsed into either an ordinary hidden or clear result.
- Presentation interpolation validates the previous/current complete tuple or
  declares a discontinuity. Projection, shake, orientation, source, and lifecycle
  changes cannot silently lerp safe endpoints through geometry.

## Final candidate evidence

| Gate | Final result |
|---|---|
| Native build | `mdkr64` builds in `build-cam05-prod` |
| Registered native suite | 84/84 CTests pass |
| Strict camera targets | Geometry, resolver, query, BVH, lens-pose, temporal, publication, and target-visibility targets compile with the strict preset |
| Focused ASan/UBSan | Full native target plus new temporal/publication/visibility and geometry fixtures pass with `ASAN_OPTIONS=detect_leaks=0` |
| Same-binary real-ROM route | Legacy: 217 penetrations; center-ray: 276; Modern: 95 corrections and zero penetration, invalidity, degradation, or hidden resolvable target |
| Dynamic real-ROM route | 8,992 detail rows, 176 corrections, 16 dynamic source hits, four dynamic corrections, zero unsafe/query-invalid rows |
| Display/FOV matrix | 24 arms previously rerun on this implementation line: 4:3 low/high, 16:9, 21:9, 32:9, portrait; authored/20/capped-140/uncapped-140; zero unsafe rows |
| Lifecycle | Pause quit/restart and 16,989-summary Adventure moving-door route pass; state resets at each load |
| Performance | Isolated 4P run: p50/p95/p99 60/470/690 us, max 1.240 ms; Observe p99 70 us; state hashes identical |

The production-backed temporal oracle covers endpoint identity, translation,
rotation, scale, shortest-angle wrap, high coordinates, 128 seeded cases, and
4,097 renderer samples per case. Pinned fixtures cover concave equal-time
corners, compact/oversized tunnels, thin-pillar point-ray false clears, thick
embedded targets, remote hidden targets, local-skin recovery, non-door chord
edge/overlap/clear controls, and invalid sources.

Useful reproduction commands:

```sh
cmake --build build-cam05-prod --target mdkr64 -j8
ctest --test-dir build-cam05-prod --output-on-failure
python3 tests/check_camera_obstruction_runtime.py \
  --build build-cam05-prod --rom "/absolute/path/to/rev1.z64" --frames 5200
python3 tests/check_camera_dynamic_obstruction_runtime.py \
  --build build-cam05-prod --rom "/absolute/path/to/rev1.z64" --frames 9000
```

## Explicitly deferred gates

These remain release/default-on work, not defects hidden by this handoff:

1. Inject failed camera publication/stale tuple and simultaneous live-resize plus
   projection failure; inject aggregate multi-instance query exhaustion.
2. Add a rotating-door-across-exact-lens fixture and a real moving-solid resolver
   correction route, beyond the production-backed chord control.
3. Rerun the current 47 legal vehicle/track rows and all 20 tracks; soak repeated
   model load/free and real object-address reuse to a memory plateau.
4. Build and run the current commit with GCC and wasm32, native GL/WebGPU, and
   real Chromium including resize, fullscreen/DPR, teardown, and load budgets.
5. Record retract/recovery/jerk/churn/shot-flip/emergency/discontinuity metrics and
   complete signed worst-1% visual review across 4:3, portrait, ultrawide, and
   split screen.
6. Implement reviewed per-viewport batch-local soft-occluder fade or explicitly
   defer that product feature; prove opposing split-view isolation and authority
   hash identity.
7. Perform the final CAM-00–CAM-09/LENS-01–LENS-08 audit and rollback drill before
   a dedicated default-on commit.

The full execution protocol and failure ownership are in
[`camera-obstruction.md`](../architecture/camera-obstruction.md#64-release-execution-protocol).
The current counter ledger is in
[`camera-runtime-modern-2026-08-05.md`](camera-runtime-modern-2026-08-05.md).

## Known environmental notes

- The strict camera targets pass. Building the entire strict repository is still
  stopped by pre-existing decompilation `-Wself-assign` fakematches in
  `game/src/memory.c`; this is unrelated to the camera changes.
- LeakSanitizer is unsupported on this macOS host, so focused ASan/UBSan runs use
  `detect_leaks=0`. Address and undefined-behavior instrumentation remain active.
- Do not copy the ROM into this worktree. Commit hooks scan the tree and history
  for ROM extensions, ROM headers, oversized blobs, and private planning paths.

## Resume point

Start with deferred gate 1, not tuning. Keep Observe as default, preserve the
same-binary Legacy and center-ray positive controls, and require each new fixture
to prove its fault was reached before accepting its Modern result. Safety
assertions must not be weakened to accommodate motion, content, or backend work.
