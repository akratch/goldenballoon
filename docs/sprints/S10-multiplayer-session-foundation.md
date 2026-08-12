# S10 — Multiplayer session and launcher foundation

> **Implementation in evidence review.** The shared core/bridge and persistent
> native/browser lifecycle proofs exist. Human/device acceptance and the final
> `GO` decision remain open in the [operational ledger](../multiplayer/STATUS.md).
> This sprint resolves the launcher/engine
> lifecycle and ownership questions before production pairing, rollback or room
> infrastructure begins. It ships no public online play.

**Goal:** Native and browser shells own one persistent Party session, enter a
dummy deterministic race, return to an external results/lobby scene and rematch
without losing the session. Local and online overlays obey different pause
policies. Browser/native UI implementations consume the same real session core,
not merely similar diagrams.

**Depends on:** nothing. **Unblocks:** S11–S13.

## Non-negotiable decisions

- Matchmaking, invitations, identity, compatibility, signaling, networking,
  retries, update UX and Party presentation stay in the launcher/session layer.
- `game/src` receives a versioned manifest and canonical inputs and emits
  deterministic events/results. It never learns about providers or room UI.
- One pure `MdkrSessionCore` is compiled into native and wasm. JavaScript and
  C++ are platform adapters/views, not independent session authorities.
- Native may run transport I/O off-thread, but reducers and engine mutation occur
  only through bounded queues at named main/tick boundaries.
- Browser callbacks mutate JavaScript-owned queues; wasm drains without Asyncify
  re-entry.
- Local overlays may pause. Online overlays may not stop one endpoint's
  authoritative clock. Room-wide pause is deferred until a tick protocol exists.
- UI scenes, room phases, transport health and engine lifecycle are separate
  state dimensions with explicit composition rules.
- Security/privacy/cost requirements are designed with the first schemas and
  fixtures. No “harden later” ticket may carry a prerequisite validation rule.

## Foundation state model

`MdkrSessionCore` owns:

```text
Intent:       NONE | LOCAL | ONLINE_PRIVATE
Scene:        HOME | LOCAL_SETUP | JOIN | LOBBY | LOADING | RACE_CHROME | RESULTS | RECOVERY
Engine:       STOPPED | BOOTING | READY | RACING | FINISHED | FAILED
Connectivity: OFFLINE | CONNECTING | DIRECT | FORWARDED | RELAYED | DEGRADED | LOST
Room:         NONE | OPEN | PREFLIGHT | SELECTING | LOADING | COUNTDOWN | RACING | RESULTS | CLOSED
```

Invalid combinations are rejected by transition tables. Rendering a scene has
no side effects. Commands enter the reducer; returned effects are performed by
adapters and acknowledged with a new command.

## Milestones

| Milestone | Achievement | Exit gate |
|---|---|---|
| F0 Boundaries frozen | One ownership/ABI/state/copy vocabulary | Source boundary and shared-fixture controls fail when deliberately crossed |
| F1 UX prototype | Play here, Phone Party and Online room flows are interactive with fake data | First-time usability sessions; keyboard/touch/gamepad/screen-reader routes |
| F2 Engine round trip | Native and browser enter dummy race, results, lobby and rematch | One session id survives; browser keeps one wasm instance; native keeps runtime |
| F3 Safe lifecycle | Local/online overlay, background, update and recovery policies are executable | Online overlay cannot pause the local clock; every interruption reaches typed recovery |
| F4 Foundation `GO` | S11–S13 can implement against stable seams | Evidence report contains no open P0 architecture decision |

## Backlog

| ID | Pri | Size | Depends | Deliverable |
|---|---|---:|---|---|
| SF-00 | P0 | S | — | Architecture decisions, source ownership and risk register |
| SF-01 | P0 | M | SF-00 | Session schemas, transition tables and shared fixtures |
| SF-02 | P0 | L | SF-01 | Pure cross-platform `MdkrSessionCore` |
| SF-03 | P0 | M | SF-01 | UI pattern/copy catalog and interactive fake-data prototype |
| SF-04 | P0 | M | SF-00,01 | Versioned `MdkrSessionBridge` engine ABI |
| SF-05 | P0 | L | SF-02,04 | Native persistent-runtime lifecycle proof |
| SF-06 | P0 | L | SF-02,04 | Browser single-wasm lifecycle proof |
| SF-07 | P0 | M | SF-02,04 | Local versus online overlay/pause policy |
| SF-08 | P0 | M | SF-01,02 | Admission/room/transport adapter interfaces and fake adapters |
| SF-09 | P0 | M | SF-00..04 | Security, privacy and cost baseline |
| SF-10 | P0 | S | SF-01,09 | Bounded observability and error taxonomy |
| SF-11 | P0 | M | SF-03,05..10 | Human/device/accessibility acceptance |
| SF-12 | P0 | M | SF-00..11 | Integration, mutations and foundation `GO` report |

### SF-00 — Freeze ownership before naming components

**Create:** `docs/architecture/multiplayer-session-boundary.md`, ADR entries,
and `docs/evidence/multiplayer/session.md` skeleton.

- Inventory the native blocking boot, browser Asyncify lifetime, overlay pause,
  input pump, ROM validation, update and return-to-launcher paths.
- Mark every proposed responsibility launcher, session bridge, deterministic
  platform, game, service or presentation-only.
- Add a source scanner rejecting service/network includes, room strings and JSON
  parsers under `game/src`.
- Record why repeat `callMain`, local-only online pause, in-game matchmaking and
  a player-host room are rejected.

**Negative control:** add a test-only `room_code` symbol under `game/src`; the
boundary gate must fail by path and symbol.

### SF-01 — One executable state contract

**Create:** `platform/session/session_types.h`,
`docs/ref/session-protocol-v1.md`, schema source and
`tests/fixtures/session/*.json`.

- Define the orthogonal state dimensions, commands, effects, error ids,
  timeouts, cancellation and recovery destinations.
- Every command carries generation/expected-state information; stale effects
  cannot mutate a newer scene or engine session.
- Generate or validate C and TypeScript enums from one schema. Lock wire-stable
  values; UI-only ordering remains local.
- Include happy paths and every invalid transition in shared fixtures.

**Negative controls:** acknowledge an old effect generation, claim RACING while
Engine is STOPPED and render a network effect from a view function.

### SF-02 — Pure SessionCore

**Create:** `platform/session/session_core.h/.c`, unit tests and wasm exports.

- Synchronous reducer: state + command → state + bounded effects.
- No sockets, SDL, DOM, ImGui, ROM, allocation after init, wall clock or global
  mutable singleton.
- Inject monotonic time through commands; retry schedules are data.
- Build the same C source into native and browser. JS/C++ views consume immutable
  snapshots and dispatch typed commands.
- Fuzz commands and run every shared trace on native and wasm.

**Gate:** byte-equivalent state/effect traces on native and wasm. Removing one
generation check must fail a stale-effect fixture.

### SF-03 — Prototype the complete player journey

**Create:** `docs/ux/multiplayer-patterns.md`, a browser fake-data prototype and
native launcher scene sketches backed by the same fixtures.

- Specify screen purpose, primary/secondary actions, state table, focus entry
  and return, live-region text, cancellation and exact error copy.
- Prototype Home → Play here → controller tiles → Phone Party, plus Create/Join
  → preflight → lobby → loading → results/rematch.
- Press feedback begins on pointer-down. Sheets are interruptible, originate at
  the trigger and reverse along the same path. Default movement is critically
  damped without ornamental bounce.
- Reduced motion uses cross-fades, reduced transparency uses solid surfaces and
  increased contrast adds boundaries/non-color indicators.
- Advanced diagnostics stay one disclosure away; local play remains the stable
  primary recovery action.

**Acceptance:** at least five first-time party groups complete the fake local
journey without spoken instruction; record hesitation, mis-scans, wrong-role
links, backtracking and copy changes. This is discovery evidence, not a launch
sample-size claim.

### SF-04 — Versioned engine session bridge

**Create:** `platform/session/session_bridge.h/.c` and bridge contract tests.

The narrow ABI supports:

```c
bool mdkr_session_apply_launch(const MdkrSessionLaunchV2 *launch);
void mdkr_session_submit_inputs(uint32_t tick, const MdkrInputSet *inputs);
bool mdkr_session_poll_event(MdkrSessionEvent *event);
bool mdkr_session_set_engine_phase(MdkrEnginePhase phase);
bool mdkr_session_export_result(MdkrMatchResultV1 *result);
```

- Fixed-size/versioned types, explicit ownership and no launcher pointers in
  authoritative memory.
- `MdkrSessionLaunchV2` wraps the shared canonical wire manifest and keeps
  endpoint-local seat and viewport masks outside synchronized match identity;
  no duplicate manifest type is permitted at the bridge. Viewports are a
  subset of locally owned seats and may be empty for a no-render verifier.
  V2 is required because the viewport byte was reserved in V1; old envelopes
  fail admission instead of acquiring new meaning.
- The persistent launcher runtime freezes a fresh, exact-epoch envelope before
  each online boot/rematch. It publishes a copy of the roster for the blocking
  engine lifetime and retires it after worker teardown; the game never holds a
  launcher pointer.
- `check_online_process_convergence.py` exercises that exact production
  composition in four isolated processes (slot 0, slot 1, two local seats and
  one local/no-render verifier), compares every v3/input row and canonical
  transition/world/result event projection, proves the verifier executes zero
  world passes while omitting endpoint-local feedback, and rejects a remote
  viewport before engine admission. It is the required neutral
  baseline for later renderer/input-map consumption, not a substitute for it.
- Effects are idempotent and generation checked.
- Production input remains behind `platform_pad_*`/canonical tick seams.
- Direct-load, rollback and viewport hooks may extend the ABI only with new
  versions and fixtures.

**Gate:** native/wasm ABI fixtures reject wrong versions, stale generations,
launcher pointers and oversize values; the source-boundary mutation catches a
room/service type crossing into the game.

### SF-05 — Native lifecycle proof

**Create:** `platform/app/session_runtime.*`, `ui_party.*` and integration gates.

- `SessionRuntime` is owned above `runEngineSession` and survives engine unwind.
- I/O worker writes bounded queues; UI and engine read immutable snapshots or
  drain at named boundaries. Thread sanitizers cover pure queues/core.
- Dummy adapter retains a session id and pending rematch through race return.
- App window/device ownership follows existing adopt/release rules.

**Gate:** lobby → finite dummy race → results → rematch twice, then Return Home;
one runtime/session id and zero dangling overlay/device callbacks.

### SF-06 — Browser lifecycle proof

**Create:** persistent `dist/web/party/party-shell.js/.css`, wasm bridge tests.

- Party DOM is a sibling of launcher/game stage and survives engine scene
  changes.
- Instantiate one wasm module. Prove race/result/rematch without reload or a
  second `callMain`; if the current main lifetime cannot support it, stop and
  implement an explicit in-engine session-idle boundary.
- Async callbacks enqueue; C drains at input/session boundaries.
- A waiting service-worker update is shown in lobby/results and never activated
  during a live session. **Update and rejoin** preserves a private invite only in
  session storage.

**Gate:** real Chromium completes two rematches, background/foreground and a
waiting update with one module/entry invocation and the expected focus scene.

### SF-07 — Pause, focus and interruption policy

- Local F1/Controls retains current pause/audio/input-swallow behavior.
- Online F1 renders non-pausing race chrome with connection and deliberate Leave
  actions; gameplay-affecting settings are disabled until Results.
- Suppress authored local pause in an online manifest. Room-wide pause is an
  explicit unsupported capability in v1.
- Hidden/minimized online endpoints continue within qualified platform policy or
  become a typed disconnect; they never manufacture a host pause.
- Escape/Back always has one predictable destination and cannot accidentally
  leave a room.

**Mutation:** let the online overlay zero one endpoint's update rate; the
multi-process clock gate must fail immediately.

### SF-08 — Future-proof adapters without premature service work

Define narrow interfaces with fake implementations:

```text
AdmissionProvider  private create/join; quick-match returns UNSUPPORTED
RoomTransport      reliable versioned commands
SignalTransport    bounded offer/answer/candidate exchange
MatchTransport     source-authenticated input graph
ControllerHost     approved remote pad sources
UpdateCoordinator  compatible / update-ready / update-required
```

No Cloudflare type crosses these interfaces. Public matching, accounts and TURN
are not implemented.

**Gate:** native/wasm fake adapters replay create/join, outage, cancellation,
stale response and unsupported quick-match traces to equivalent core states.

### SF-09 — Security/privacy/cost foundations

**Create:** `docs/security/multiplayer-threat-model.md` and
`docs/privacy/multiplayer-data-map.md`.

- Threat-model role confusion, QR replay, stale generations, name injection,
  malformed SDP/packets, rollback/future-tick abuse, duplicate tabs, peer IP
  disclosure, service MITM, relay visibility and quota exhaustion.
- Classify every proposed field by purpose, receiver, retention and log policy.
- Define application-layer encryption envelope for any future WebSocket gameplay
  fallback.
- Select a reviewed, standard browser/native key-agreement + AEAD construction,
  transcript binding, key erasure and shared test vectors behind a crypto
  adapter. Do not invent a cipher/protocol or let room/relay actors receive
  gameplay keys. Document what a compromised signaling service can and cannot
  learn or substitute.
- Define separate control/pairing/relay capacity reserves and typed exhaustion.
- CI secrets/URL/body corpus is created before production endpoints.

**Gate:** every protocol field appears in the threat/data map, credentials and
forbidden payloads fail the capture corpus, and exhausting relay capacity cannot
consume the reserved pairing/control floors.

### SF-10 — Error and evidence taxonomy

- Stable error ids map to platform-local copy. Never use generic “Network error”
  when the cause is known.
- Metrics use bounded dimensions and no names, codes, addresses, SDP, inputs or
  raw hashes.
- Define scan-to-ready, input-to-consume, glass-to-display comparison,
  start/reconnect success, rollback depth and quota-reserve measurements.
- Every wait publishes start, progress, timeout, cancel and outcome.

**Gate:** shared snapshots enumerate every wait/error id and recovery action;
CI rejects unbounded metric dimensions and raw name/code/address fields.

### SF-11 — Human and accessibility acceptance

- Keyboard-only, gamepad-only, touch and screen-reader-model routes.
- 320×568 through desktop/TV layouts, 200% text, safe areas, high contrast,
  reduced motion/transparency and orientation change.
- Interruption drills: back gesture, notification, lock, app switch, update
  ready, transport lost and service unavailable.
- Record slow-motion/frame-by-frame interaction captures for press feedback,
  focus continuity and sheet interruption.

**Gate:** the matrix records pass/fail evidence and recovery completion for
native/browser routes; no critical action depends on motion, color, hover,
orientation or precise pointer movement alone.

### SF-12 — Foundation integration decision

Run native/wasm state fixtures, source boundary, lifecycle loops, overlay clock
mutation, security corpus, accessibility routes and the fake-adapter outage
matrix. Publish measured results and `GO`/`STOP`.

`GO` requires no unresolved question about runtime ownership, browser rematch,
online pause, source boundaries, state authority, update lifetime or credential
roles. Feature polish may remain; architectural ambiguity may not.

## Definition of done

- Native and browser preserve one launcher-owned session through two dummy
  races and a return home.
- Browser uses one wasm instance; native keeps one SessionRuntime.
- Online overlay cannot pause one endpoint or allow the authored pause path.
- Browser/native state traces are equivalent; views do not perform effects.
- Private admission/matchmaking is an adapter outside the engine.
- Security, privacy, cost, error and UI pattern documents exist with executable
  fixtures and meaningful mutations.
- The foundation evidence report says `GO` before production S11/S13 networking.
