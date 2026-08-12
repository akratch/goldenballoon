# S12 — Deterministic rollback multiplayer core

> **Laboratory core implemented; real-game `GO` not earned.** Pure manifest,
> input, snapshot, event, driver, roster and four-endpoint impairment gates pass.
> Engine arena registration and real-race/platform evidence remain open in the
> [operational ledger](../multiplayer/STATUS.md). This sprint contains no
> production matchmaking service. It proves that the existing game can support excellent online play
> before infrastructure or public UX is built.

**Goal:** Two to four local processes consume one canonical input stream,
predict missing remote input, restore a bounded local snapshot, resimulate, and
finish a standard race with byte-identical authoritative hashes—while each
process renders only its own local racer viewport.

**Depends on:** S10 session bridge and S11 LC-00/01/02 contracts.
**Unblocks:** S13 online rooms.

## Non-negotiable gates

Stop the program if either proof cannot be achieved without broad state-hash
exclusions:

1. **Restore identity:** snapshot tick `T`, advance to `N`, restore `T`, replay
   the same inputs to `N`; the v3 hash/event stream equals uninterrupted play.
2. **Viewport independence:** one canonical four-racer simulation produces the
   same authoritative stream when endpoints present racer 0 full-screen, racer
   1 full-screen, two local split views, or no view.

`MDKR_STATE_HASH=3` detects divergence; it is not itself a savestate. Snapshots
remain local to one process and never cross the network.

Two integration gates are equally strict:

3. **Session continuity:** lobby → race → results → rematch uses S10's
   `SessionRuntime` and `MdkrSessionBridge`; the browser keeps one wasm instance
   and native keeps one launcher/session runtime.
4. **Online clock integrity:** opening, navigating and closing the online shell
   overlay cannot pause or change the authored tick. An online manifest
   suppresses the original local-only pause path.

## Authority model

- Canonical slots 0–3 and all inputs are identical on every endpoint.
- Each endpoint owns one or more local seats and predicts the others.
- The fixed 30/25 Hz authored tick is the only network tick. Presentation rate
  and interpolation remain local.
- Inputs are complete pad states, not action RPCs. Default prediction repeats
  the last complete state.
- A tick becomes confirmed once all canonical inputs are known. Only confirmed
  ticks may commit saves, rumble, telemetry or irreversible host effects.
- State hashes are exchanged for detection, not anti-cheat. Agreement proves
  synchronization, not honest binaries.

## Milestones and stop/go gates

| Milestone | Achievement | Exit gate |
|---|---|---|
| R0 Authority frozen | Every authoritative field/side effect has a policy | Omitted-field and forbidden-I/O mutations fail |
| R1 Restore identity | Local snapshots restore and replay exactly | Three tracks, finish, items and 10,000 seeded ticks agree |
| R2 Viewport independence | Canonical simulation no longer depends on local views | 1P/2P/headless presentations produce one authority stream |
| R3 Bounded rollback | 2–4 processes converge under named impairment profiles | Correction, event, memory and CPU budgets pass |
| R4 Product lifecycle | Production shell completes results and two rematches | One wasm/runtime; online overlay cannot pause |
| R5 Rollback `GO` (A3) | Cross-platform evidence supports online integration | RB-13 publishes `GO`; any core gate failure is `STOP` |

S13 may consume reducer/manifest fixtures before R5, but no production online
race may open on a conditional or failed rollback decision.

## Backlog

| ID | Pri | Size | Depends | Deliverable |
|---|---|---:|---|---|
| RB-00 | P0 | M | S10 SF-04, S11 LC-00 | Authority inventory and fixed contract |
| RB-01 | P0 | M | RB-00, S11 LC-02 | Canonical tick input history/prediction |
| RB-02 | P0 | XL | RB-00 | Snapshot registry and restore proof |
| RB-03 | P0 | L | RB-02 | Bounded snapshot ring and memory budget |
| RB-04 | P0 | L | RB-02 | Reversible/confirmed side-effect journal |
| RB-05 | P0 | XL | RB-01,03,04 | Rollback driver and resimulation |
| RB-06 | P0 | XL | RB-00 | Canonical roster/local viewport separation |
| RB-07 | P0 | L | RB-05,06 | Two-/four-process deterministic harness |
| RB-08 | P0 | M | RB-07 | Seeded network and endpoint-clock simulator |
| RB-09 | P0 | L | RB-05,06 | Standard race start/results lifecycle |
| RB-10 | P1 | M | RB-09 | Disconnect grace and deterministic AI takeover |
| RB-11 | P0 | L | RB-07..10 | Cross-platform/presentation matrix |
| RB-12 | P0 | M | RB-09,11 | Session lifecycle and overlay integration |
| RB-13 | P0 | M | RB-11,12 | Performance budgets and go/no-go report |

### RB-00 — Inventory authority before snapshotting it

**Create:** `docs/ref/rollback-authority-v1.md`,
`tools/check_rollback_authority.py`, `tests/test_match_manifest.c`.

- Enumerate v3 hash families, authoritative globals/sidecars outside arenas,
  allocator/object pools, controller edge state, clocks, RNG, pause/results and
  every host side effect reachable from one race tick.
- Classify each value: snapshot, derive, presentation-local, host handle, or
  forbidden during resimulation. Every exclusion names all readers/writers.
- Define `MdkrMatchManifestV1`: protocol/build, ROM revision, cadence, gameplay
  settings/content digest, slots, track/rules and RNG seed.
- Add a source scanner requiring new mutable authoritative statics to register
  with snapshot or carry an explicit reviewed exclusion annotation.

**Negative control:** a test-only authoritative scalar changes after `T`; the
inventory/check must fail before the restore comparison can pass vacuously.

### RB-01 — Canonical input history

**Create:** `platform/net/net_input.h/.c`, `tests/test_net_input.c`.

```c
void mdkr_net_input_submit(unsigned slot, uint32_t tick,
                           const MdkrPadSample *sample, bool confirmed);
MdkrInputSet mdkr_net_input_for_tick(uint32_t tick);
void mdkr_net_input_confirm_through(uint32_t tick);
```

- Ring capacity covers configured rollback window plus redundant send/ack tail.
- Distinguish absent, predicted and confirmed samples; a correction reports the
  earliest dirty tick exactly once.
- Modulo-safe tick/sequence comparison; reject inputs older than retained history
  and implausibly far future.
- Repeat-last prediction is deterministic; initial prediction is neutral.
- Reuse S11's exact `MdkrPadSample`; do not introduce another controller shape.

**Negative controls:** future-window attack, tick wrap, late correction and a
20 ms press/release represented by successive states.

### RB-02 — Local snapshot registry

**Create:** `platform/rollback/rollback_snapshot.h/.c`, registry source,
`tests/test_rollback_snapshot.c`, `tests/check_rollback_restore.py`.

- Register stable arena ranges, allocator metadata and explicit global blocks.
  Registration freezes before match tick zero; overlap and writable host-handle
  ranges fail startup.
- Restore occurs only at the fixed-tick boundary with presentation/GPU work idle.
- Preserve in-process pointer addresses. Never normalize/serialize for another
  platform and never include ROM backing bytes.
- Capture one versioned header containing tick, manifest digest, range layout
  digest and checksum. Restore rejects different processes/manifests/layouts.
- After restore, rebuild derived caches only through explicit hooks; no “reset
  everything and hope” path.

**Proof:** all 20 standard tracks, every one of the ROM's 47 legal
track/player-vehicle pairs, all 15 balloon type/level configurations, one race
finish, particles, 2P/4P and a 10,180-tick hovercraft soak. Item-spawn assets are
leased before tick zero; dynamic model instances and attachments allocate from
the snapshotted object subpool, while shared-model reference counts are explicit
small authority ranges. Every item configuration uses the real inventory branch
inside a corrected window and passes an exact second replay; a suppressed
release proves the observation is non-vacuous. Each snapshot point is tested at
multiple replay distances. A one-byte omitted-range control must diverge at the
mutated tick.

### RB-03 — Bounded snapshot ring

**Create:** `platform/rollback/rollback_ring.h/.c`, unit/perf tests.

- Preallocate at match start; zero allocation/free in tick or resimulation.
- Default eight authored ticks, configurable only within measured memory/time
  caps. Full snapshots first; compression/delta only after profiling.
- Generation + checksum prevents partial/overwritten restore.
- Expose bytes, capture/restore microseconds and high water to diagnostics.

**Gate:** 4P worst-track p99 capture+restore remains below an initially measured
budget that leaves at least 50% of one authored tick for simulation. No guessed
numeric threshold is frozen before the baseline exists.

### RB-04 — Side-effect journal

**Create:** `platform/rollback/rollback_events.h/.c`,
`tests/test_rollback_events.c`.

- Assign gameplay presentation events stable `(tick, emitter, ordinal, kind)`
  ids. Predicted event can play once; corrected duplicates are suppressed.
- Save/EEPROM/Pak sync, achievements, diagnostics export and durable telemetry
  wait for confirmation. Online standard races write no retail progression or
  time-trial records regardless.
- Rumble/audio start-stop reconcile by id and are force-cleared before restore.
- Resimulation mode is explicit and observable; forbidden host I/O asserts in
  debug/tests rather than silently no-oping.

**Negative controls:** resimulate an item/audio/rumble/save tick and require one
device effect, zero online save writes and a failing forbidden-I/O arm.

### RB-05 — Rollback driver

**Create:** `platform/rollback/rollback_driver.h/.c` and schedule tests.
**Modify:** the existing fixed-ticket host driver at its one tick boundary.

Flow per authored tick:

1. ingest validated remote inputs;
2. if a correction dirties retained tick `D`, restore snapshot immediately
   before `D`, resimulate `D..current-1` with presentation off, and replace
   each completed boundary snapshot;
3. inject the canonical current-tick slot samples into ordinary pad edge
   calculation;
4. execute one normal fixed tick;
5. capture that newly completed tick boundary (snapshot `T` is always state
   *after* tick `T`, never its pre-state);
6. advance confirmation and commit eligible events once;
7. publish presentation from the final current state.

- Cap resimulation ticks per host opportunity. If correction exceeds window or
  CPU budget, enter explicit recovery; never spiral indefinitely.
- Presentation smoothing invalidates/reseeds its adjacent authored snapshots
  after rollback; it cannot interpolate across incompatible timelines.
- Audio time follows confirmed/current policy documented by RB-04, not number of
  resimulation passes.

**Gate:** randomized corrections agree with a clean delayed-input reference;
rollback-disabled positive control diverges.

### RB-06 — Separate canonical players from local viewports

**Create:** `platform/net/net_roster.h/.c`, a presentation mapping interface,
and `tests/check_network_viewport_invariance.py`.

- Introduce `canonical_player_count`, `local_seat_count`,
  `local_seat -> canonical_slot`, and `viewport -> canonical_slot`.
- Audit every authoritative read of `gNumberOfActivePlayers`,
  `cam_get_viewport_layout()`, `get_current_viewport()`, HUD/player loops and
  camera/player indices. Move authoritative per-player work to canonical loops;
  leave draw/HUD/camera presentation on local viewport loops.
- Do not render four hidden views and crop one. Each endpoint gets genuine
  full-screen/local split quality and cost.
- Camera state that is truly local must be separated from the authority hash,
  field by field with reader/writer evidence—not wholesale excluded.

**Go/no-go gate:** identical racer/world/progression/event hash stream under
slot-0 1P, slot-1 1P, slots-0+2 2P and no-render layouts. A test mutation that
leaks viewport selection into racer physics must fail.

### RB-07 — Multi-process harness

**Create:** `tests/net_session_harness.py`, `tests/check_net_determinism.py`.

- Spawn isolated game processes with separate saves/config, one manifest and
  canonical input generator. IPC transport is length-delimited binary over
  localhost and uses production codecs.
- Collect per-tick hash, event, consumed-input, rollback and finish rows.
- 2/3/4 endpoints, different local slots/viewports, GL/WebGPU/headless mixes.
- First mismatch reports exact tick, field-family diagnostic and input history
  tail; no giant raw-memory dump.

### RB-08 — Deterministic network and clock simulator

**Create:** `platform/net/net_impairment.h/.c` (test/dev only) and profile tests.

- Seeded latency, jitter, loss, duplication, reorder, burst outage and bandwidth
  cap. Apply after encoding and before decoding so the wire path is real.
- Independently perturb endpoint scheduler opportunity, monotonic-clock offset,
  oscillator drift, long frame and sleep/wake. Simulation `dt` remains the
  authored fixed value; the harness may change only when a host tick is offered.
- Named profiles: LAN, regional-good, regional-variable, poor, 2-second outage,
  and adversarial malformed stream.
- Emit expected versus measured rollback/confirmation distributions.

**Gate:** fixed seed produces byte-identical network/clock schedules; controls
prove each impairment is non-vacuous and no endpoint changes/skips an authored
simulation tick to catch up.

### RB-09 — Standard race lifecycle

- The S10 `MdkrSessionBridge` applies one signed manifest at an engine-safe
  direct-load transition; original menus are not synchronized in the first
  release. No room, transport or provider type crosses this bridge.
- Start barrier: track loaded, tick-zero snapshot captured, input delay filled,
  every endpoint ready. Countdown begins at canonical tick.
- Results wait for all confirmed inputs through the terminal tick, export one
  versioned result and return control to the persistent shell without replacing
  the process/wasm module or writing trusted retail records.
- Rematch creates a new manifest epoch/seed and clears histories atomically.

**Gate:** full 2P–4P race through results/rematch, abort during load/countdown,
and one endpoint slower to load.

### RB-10 — Disconnect and AI takeover

- Room control names one future takeover tick after the grace period. Every
  client continues prediction until that tick, then switches the same slot to
  the same AI state/seed.
- First release reconnects the person for the next race; no mid-race handback.
- If no control quorum/service can authorize the roster transition, freeze at a
  confirmed boundary rather than choose locally.

**Gate:** disconnect each slot at countdown, item use, finish and results;
remaining endpoints finish identically and host/leader identity is irrelevant.

### RB-11 — Cross-platform matrix

- Same release/protocol/ROM revision across macOS, Windows, Linux and wasm where
  hardware exists; US and EU are separate rooms. Compile authoritative code
  with a documented strict floating-point contract and audit libm/compiler
  differences instead of assuming equal source produces equal results.
- Presentation Original/interpolated, GL/WebGPU, 4:3/ultrawide, local 1P/2P,
  hidden/minimized lifecycle.
- Sanitizers on pure/ROM-free pieces and native real-ROM route. Browser lifecycle
  uses actual Chromium, not a JS mock.

### RB-12 — Session lifecycle and overlay integration

- Drive the production S10 shell through lobby → load → race → results → rematch
  twice using the rollback driver and fixture transports.
- Preserve session id, invite/room fixture, adapters and UI focus restoration
  while match epoch, snapshots, input histories and result generation rotate.
- Exercise online Controls/Connection details during prediction, correction and
  a transport outage. It is non-pausing, cancellable and cannot change input
  delay, gameplay settings or the canonical clock mid-race.
- A failed load, over-window correction or desync returns one typed recovery
  result to the shell. The engine never creates its own modal/network workflow.

**Negative controls:** invoke a second browser `callMain`, destroy the native
`SessionRuntime` on results and route authored pause through the online overlay;
each must fail the lifecycle/clock gate.

### RB-13 — Go/no-go report

Record measured snapshot bytes, capture/restore/resimulation p50/p95/p99,
maximum stable rollback window, hash/event results, worst correction depth and
each unsupported mode. Recommend proceed, redesign or remote-play fallback.

Do not call this sprint complete because two peers move on screen. It is complete
only when restore and viewport gates are mutation-proven and the complete
impairment matrix stays bounded.

## Definition of done

- Four processes finish the same standard race from predicted/corrected input
  with identical confirmed authority/event streams.
- Each displays only its local racer(s) at ordinary local quality.
- Rollback is bounded in memory and time; over-window input produces explicit
  recovery, not silent state correction.
- Audio, rumble and save side effects occur at most once and online races do not
  modify trusted progress.
- The production launcher/session bridge completes two rematches without a wasm
  or runtime restart; the online overlay cannot pause one endpoint.
- Every major claim has a broken-direction control and cross-platform evidence.
