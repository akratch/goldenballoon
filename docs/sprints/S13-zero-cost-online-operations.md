# S13 — Zero-cost private online rooms and operations

> **Foundation and automated native UX implemented; production remains gated.**
> The pure launcher room reducer, protocol-v1 lifecycle/authorization tests,
> per-seat race selection, canonical descriptor, V3 descriptor handoff, engine
> direct-load application, 42-case native state/failure gallery and 25-action
> keyboard/gamepad gate exist. The browser has an honest zero-I/O release-gated
> entry plus an isolated shared-C fake/gallery binding. Local workerd now proves
> the bounded MatchRoom lifecycle and recovery; live adapter, human/device
> evidence, hosted deployment and transport remain backlog. The ordered delivery queue is the
> [operational backlog](../multiplayer/OPERATIONAL_BACKLOG.md). This
> sprint turns S12's proven local netcode into an operated private-room product
> while guaranteeing a $0 infrastructure bill. Capacity and NAT compatibility
> degrade honestly before cost can accrue.

**Goal:** Friends create/join a private room by code/link/role-specific QR, pass
an exact compatibility preflight, race over an authenticated WebRTC
connectivity graph with WebSocket control, survive ordinary reconnects and
leave/rematch without making any player the room host.

**Depends on:** S10 foundation, S11 pairing/service primitives and S12 `GO`.

## Cost and reliability contract

- Static client/controller hosting remains free and independently cacheable.
- Production uses only Workers Free + SQLite Durable Objects. Do not attach a
  paid Workers plan or metered TURN/SFU product to the production account.
- Game inputs are peer-to-peer. Room actors see control transitions and only
  disagreement/health exceptions, not successful per-tick traffic.
- A small WebSocket input relay is admission-controlled below free daily limits.
  Capacity-full rejects new fallback leases; direct rooms/local play continue.
- Direct ICE cannot connect every NAT/firewall pair. Copy and metrics distinguish
  `service unavailable`, `relay capacity full` and `networks cannot connect`.
- Budget alerts are monitoring only and are not treated as a spending cap.
- Optional sponsored or user/community-provided TURN is a separate configuration
  and credential provider, disabled in guaranteed-$0 production.
- Public anonymous quick match is deliberately unsupported. Direct peers may
  learn network-address information; the private-room consent copy says so.
  IP-hiding public play requires funded relay capacity and abuse operations.

## Golden path

1. **Online room** → choose **Create private room** or **Join room**; one local
   seat per endpoint for v1.
2. **Create private room** → six-character code, role-specific `/room/` link and
   QR; guest name is optional. Controller invitations always use `/controller/`.
3. Joiners get a preflight before character selection: protocol/build, US/EU
   revision, cadence, gameplay settings/content digest and direct-connect probe.
4. Lobby shows seats, leader, character, ready and calm connection quality.
5. Track vote resolves with manifest seed; every endpoint loads and captures
   tick zero; cancelable three-second barrier starts the race.
6. Direct WebRTC edges carry redundant frame inputs. A peer may forward
   authenticated input one hop when the direct graph is incomplete; the room
   WebSocket carries phases, leases, disagreement and reconnect control.
7. Results are casual/local, rematch keeps the party, leader transfers
   automatically, and no trusted progression is written.

## Service boundaries

```text
services/party (extended from S11)
  Worker             authentication envelope, routing, origin/rate/size policy
  PartyRoom DO       invite, pending phone controllers, couch seats
  MatchRoom DO       online roster, manifest, lobby, barrier, control log
  RelayBudget DO     rare fallback lease admission only

launcher/session shell
  MdkrSessionCore    executable UI/engine/connectivity state authority from S10
  RoomClient         pure online-room reducer + typed commands
  SignalingTransport WebSocket/HTTPS adapter
  MatchTransport     authenticated direct/forwarded graph + encrypted WS fallback
  ControllerHost     approved phone-controller sources from S11
  UpdateCoordinator  compatible/waiting/required lifecycle

engine boundary
  MdkrSessionBridge  manifest/input/event/result ABI from S10
  NetSession         S12 rollback/input/confirmation runtime
```

No service component links game code or knows kart state. Room reducers are pure
and replayable from versioned commands before they are hosted in a Durable
Object. `AdmissionProvider.findQuickMatch()` returns `UNSUPPORTED`; adding a
future matchmaker replaces an adapter, not the game or Party scene model.

## Milestones and release gates

| Milestone | Achievement | Exit gate |
|---|---|---|
| O0 Contract frozen | Room reducer, roles and data/cost/security policy agree | Shared native/browser/service fixtures pass; role/cost mutations fail |
| O1 Private-room vertical slice | Two qualified endpoints create, race, results and rematch | One persistent session; exact manifest; no trusted progress writes |
| O2 Resilient direct graph | Four endpoints race without every pairwise edge | Partial graph, asymmetric failure, reconnect and service-outage gates |
| O3 Operated alpha (A4) | Invited rooms receive typed recovery/update UX | Device/network/usability matrix and red-team report pass |
| O4 Zero-cost beta (A5) | Release is observable, reversible and cannot bill | Seven-day error budget, quota/load/chaos/privacy evidence and runbooks |

Production endpoints remain closed through O0. The invited alpha remains closed
until S12 says `GO`; O4 cannot waive an O0–O3 failure.

## Backlog

| ID | Pri | Size | Depends | Deliverable |
|---|---|---:|---|---|
| ON-00 | P0 | L | S10, S12 RB-00 | Protocol, threat/cost delta and pure room reducer |
| ON-00S | P0 | M | ON-00 | Canonical per-seat selection through direct load (**implemented; evidence review**) |
| ON-01 | P0 | L | ON-00 | MatchRoom Durable Object and recovery (**implemented locally; evidence review**) |
| ON-02 | P0 | L | ON-00, S12 GO | Authenticated direct/forwarded transport graph |
| ON-03 | P0 | L | S10, ON-00S | Launcher lobby UX with fake then live adapters |
| ON-04 | P0 | M | ON-01,03 | Compatibility preflight and signed manifest |
| ON-05 | P0 | M | ON-01,04 | Vote/load/start/results/rematch barriers |
| ON-06 | P0 | L | ON-01,02, S11 | Reconnect, leases, leader and AI takeover |
| ON-07 | P1 | L | ON-01,02 | Hard-capped WebSocket match fallback |
| ON-08 | P0 | M | ON-01..07 | Security/privacy/abuse verification and red-team gate |
| ON-09 | P0 | L | ON-01..08 | Observability, free-quota and SLO dashboard |
| ON-10 | P0 | L | ON-01..09 | Load, chaos and deployment-version testing |
| ON-11 | P0 | M | ON-10 | Runbooks, staged beta and rollback |
| ON-12 | P2 | XL | ON-11, S11 M5 | Mixed couch+online seats and nearby mode |

### ON-00 — Freeze the online contract before hosting it

**Create:** `services/party/src/match/{protocol.ts,reducer.ts}`, shared schemas,
binary/JSON fixtures and `docs/ref/online-lobby-protocol-v1.md`.

The service reducer stays deliberately small: `LOBBY → LOADING → RACING →
RESULTS → LOBBY`, plus terminal `CLOSED`. Preflight, selection presentation and
countdown are launcher/session projections over that authority; they are not
extra service-owned UI state. Every external command is scoped by its room
route and authenticated endpoint credential, then carries protocol version,
monotonic actor command id and expected monotonic room revision. That revision
continues across rematches, so a previous-round command cannot become current
again merely because a visible phase repeats.

- Update the S10 threat model/data map before an endpoint exists. Freeze roles,
  capability scopes, IP-disclosure consent, retention, size/rate bounds,
  pairing/control/relay reserve floors and typed exhaustion behavior.
- Freeze the peer-key handshake, group transcript verification phrase, key
  rotation/erasure and signaling-service trust boundary using S10's reviewed
  crypto adapter and cross-platform vectors.
- Reducer is deterministic, synchronous and side-effect free. It returns new
  state + effects; Durable Object adapter performs/persists effects.
- Idempotency window covers retries/reconnect. Invalid phase/role transitions
  return typed errors without partial mutation.
- Leader can configure/vote/start; gameplay authority never depends on leader.
- Roster slots have leases and endpoints, not raw WebSockets.
- Bounded control log contains no SDP after negotiation and expires with room.
- The launcher consumes reducer fixtures through S10 interfaces; no
  Cloudflare/WebRTC type and no guest string enters `game/src`.

**Negative controls:** duplicated start, old epoch, leader leaving during load,
late ready after countdown and deploy replay through old/new reducer versions.

**Gate:** C/TypeScript/service reducers replay the same valid/invalid traces to
equivalent state/errors, and every bounded role/cost/retention rule maps to its
S10 threat/data record before O0 closes.

### ON-00S — Freeze race selection in the launcher

The current reducer carries seats and track votes, while
`MdkrMatchManifestV1` carries only a legal-vehicle mask. Neither is a per-seat
selection contract. Do not begin ON-03 character/vehicle UI by patching retail
game menus or overloading that mask.

**Implemented checkpoint:** the reducer now owns seat-scoped character/vehicle
commands, unique-character conflicts, selection revisions, ownership, Ready
invalidation and legal-mask validation. The checksum-protected fixed 148-byte
[`MdkrMatchLaunchDescriptorV1`](../ref/match-launch-descriptor-v1.md) freezes
the manifest plus every occupied selection and rejects every single-byte
mutation. `MdkrSessionLaunchV3` and `SessionRuntime` validate and copy-own that
descriptor while retaining endpoint-local controller/viewport masks outside
match identity. The engine-lifetime runtime copy-owns the descriptor, direct
loads its track and applies canonical character/vehicle choices before racer
creation. Admission compares the manifest track, race type, authored cadence
and legal-vehicle capability mask to the loaded ROM before tick zero.

- Add a versioned canonical selection record for every occupied slot containing
  `slot`, `character_id`, `vehicle_id` and selection revision. The room reducer,
  launcher view model and engine launch bridge consume the same fixtures.
- Validate character uniqueness, supported character ids and the selected
  track's ROM-derived vehicle mask in the launcher. Revalidate the frozen values
  against the loaded ROM at engine admission before tick zero.
- A track-vote change that makes a vehicle illegal returns the affected seat to
  a clearly labelled **Choose vehicle** state; it never silently substitutes.
- Only the owning endpoint changes its seats. Ready is cleared by any accepted
  selection change. Duplicate/stale commands are idempotent or rejected using
  the existing command receipt/version rules.
- Freeze track, per-seat choices, input delay and seed into one immutable launch
  descriptor. The game receives this descriptor and canonical pad inputs; it
  owns no matchmaking, voting, reconnect or selection state machine.
- Preserve endpoint-local presentation, controller source labels and guest names
  outside deterministic match identity except where a stable canonical slot id
  is required. Never serialize free-form names into rollback authority.

**Negative controls:** duplicate character, illegal vehicle, stale selection,
cross-endpoint seat mutation, ready-then-change, vote invalidation, malformed
reserved bytes and manifest/loaded-ROM selection mismatch all fail atomically.

**Gate:** 1–4 seats, keyboard/gamepad/phone source mixtures and two-local-seat
fixtures reach one byte-identical frozen descriptor on every endpoint; mutation
fixtures fail before engine boot. Only then may ON-03 bind polished UI to the
selection commands.

#### ON-00S implementation slices — execute in this order

| Slice | State | Deliverable | Exit evidence |
|---|---|---|---|
| ON-00S-A | DONE | Pure seat-selection commands, ownership, revisions, uniqueness, legal vehicles and Ready invalidation | Reducer positive/negative controls pass without service or game dependencies |
| ON-00S-B | DONE | Fixed 148-byte descriptor and pure Loading-lobby builder | Round-trip, stable-seat-order and exhaustive one-byte mutation gates pass |
| ON-00S-C | DONE | Fail-atomic `MdkrSessionLaunchV3` copied into persistent `SessionRuntime`/bridge | Wrong epoch/reserved/selection controls fail; launcher-source mutation cannot affect the loan |
| ON-00S-D | DONE | Copy descriptor into engine-lifetime runtime; direct-load track and canonical seat character/vehicle arrays before racer creation | Real-ROM direct-load row passes without the diagnostic track override; wrong track/mask/character/vehicle controls fail before tick zero; V2 remains laboratory-only |
| ON-00S-E | DONE | End-to-end 1–4 seat/source-mixture fixture and boundary documentation | Four isolated endpoints freeze identical V3 identity while slot-0, slot-1, two-local and no-render controller/view maps differ safely for 3,600 ticks |

ON-00S and automated ON-03C/ON-03E browser control are in evidence review;
ON-03C5 two-person usability is the next executable product slice.
Keep visual character/vehicle controls bound to this launcher contract. A second
game-menu selection model would recreate the ownership ambiguity this ticket
removed.

### ON-03 execution slices — UX before live services

| Slice | Depends | Player outcome | Security/accessibility gate |
|---|---|---|---|
| ON-03A — state and copy contract | DONE | Pure projection over validated S10 session + lobby snapshots emits one title, explanation, primary/secondary/Cancel set, timeout recovery and announcement priority | Exhaustive typed/unknown failure fixtures exclude raw transport/provider copy; Start requires local GO policy, leader, 2+ members and everyone Ready |
| ON-03B — fake adapter vertical slice | DONE | Create/join → invite → preflight → select → ready → vote → load/cancel → race → results → second race works offline through the real reducers | Versioned UI commands and callback tokens reject stale/conflicting/route-confused work atomically; exact double actions are idempotent; canceled load cannot resurrect |
| ON-03C — responsive interaction polish | EVIDENCE REVIEW | Native renders all 42 deterministic views/failures and all 25 public actions activate through keyboard and gamepad; full spoken title/action walks pass. Browser keeps an honest zero-I/O default entry, uses an isolated module compiled from the same C reducer/view model for all 42 cases/25 keyboard routes, and binds the live MatchRoom control seam. Human screen-reader/device review remains | Wide and 640×480/200% native captures, real touch scrolling, native and browser 42-case galleries, 42 spoken native action sets, 25×2 native input actions, browser 25-keyboard/touch/a11y paths and every-case portrait/landscape 200% reflow |
| ON-03D — typed failure and recovery | DONE AUTOMATED / EVIDENCE REVIEW | All 17 typed failures plus six elapsed-timeout outcomes render deterministic safe actions. Retry preserves preflight rooms, invite replacement starts a clean join, room settings rerun checks, engine/epoch failures return to the intact lobby, and confirmed Leave Race stops before disconnect | Unknown/provider copy remains bounded; stale/duplicate callbacks reject atomically; local routes, privacy-safe Connection Doctor and non-automatic update guidance are action-gated |
| ON-03E — live adapter binding | DONE BROWSER CONTROL / NATIVE+CARRIER GATED | Browser create/join/share/rotate/select/ready/reconnect effects bind MatchRoom beneath the unchanged shared-C model. Native carrier/race phases remain post-A3 | Fixture replay stays identical; malformed snapshots fail closed; service data cannot reveal Start or bypass reducer validation |

The UI never waits on a service to show **Play here**, never replaces calm
quality words with volatile latency numbers, and never enables a production race
action while A3 is gated.

#### ON-03C remaining slices — do not skip to live networking

| Order | Slice | Deliverable | Exit evidence |
|---:|---|---|---|
| 1 | C1 native shell | **DONE:** Online Room destination, gated production copy, fake-adapter journey surface, launcher-owned selections/actions and responsive scroll owner | Wide capture plus 640×480 at 200% UI scale; production-input touch reaches the below-fold primary action; generic launcher and handheld captures remain green |
| 2 | C2 deterministic state gallery | **DONE:** product binary publishes and renders 42 reducer-constructed cases covering 10 views, 17 failures and six elapsed timeout outcomes | Unique non-flat captures, semantic/copy inventory, stale/duplicate deadline and callback controls pass |
| 3 | C3 native input/accessibility | **DONE automated / human review remains:** drive every public action and enumerate every state/action announcement | 25 actions pass through keyboard and virtual gamepad; all 42 title/action sets pass keyboard speech walk; physical SR/device review remains |
| 4 | C4 browser binding | **AUTOMATED CONTROL COMPLETE / EVIDENCE REVIEW:** the honest zero-I/O entry remains production behavior. Explicit gallery/live adapters load a 35 KiB ROM-/engine-/provider-free shared-C projection; JS owns bounded transport, validation, DOM and launcher routes | All 42 views/25 routes/touch/AX/reflow pass; live fragment erasure, create/join, QR/share, rotate, select/Ready, reconnect and corrupt-state recovery pass without game Wasm. Adversarial service admission cannot reveal Start. Remaining: human screen-reader/device and first-time-friend review |
| 5 | C5 first-time usability | **AUTOMATED FLOW COMPLETE / HUMAN EVIDENCE REMAINS:** two clean Chrome profiles over the real local Worker complete create/share, wrong-role recovery, `/room/` join, outage/retry, distinct selections, Ready/backtrack/re-Ready and leave; Start remains absent | Run the uncoached human task, record human time-to-share/time-to-ready, cancel/rematch, backtracks and recovery success; resolve P0/P1 findings before release |

Every C slice leaves the default build in the honest unavailable state and makes
zero provider calls. Test flags are local process inputs; room/service data can
never set the release-admission flag.

### ON-01 — MatchRoom Durable Object

**Modify:** S11 `services/party`; add SQLite migration and service tests.

**Implemented locally / evidence review:** migration `v3` adds a distinct
`MatchRoom`; `match/protocol.ts` and `match/reducer.ts` preserve the frozen
four-endpoint/four-seat, revision, receipt and phase bounds. Create and direct
or six-digit-code Join use purpose-separated digests, endpoint commands derive
their actor only from a 256-bit bearer, and state sockets hibernate. The
complete two-endpoint barrier/rematch plus reconstruction in every phase,
concurrent CAS, exact/conflicting replay, compatibility, capacity, raw-code
storage, body, origin and expiry negatives pass in local workerd. This does not
enable production race admission.

Leader-only invite rotation is generation-checked and replaces the link/code
pair together without destroying the room. Old links/codes, nonleader rotation
and a stale concurrent generation reject; collision probes happen before the
room mutation so a new generation is never published with an unreachable code.

The public state projection omits credential digests, reducer receipts,
per-member command high-water marks and command fingerprints. Non-Join commands
must omit compatibility bytes so there is one canonical C/TypeScript command
encoding; a fixed cross-language FNV-1a vector guards replay equivalence.

- One object per unguessable room id; display code maps through expiring hashed
  capability, not object name enumeration.
- Hibernatable WebSockets with serialized connection attachments. Constructor
  may run repeatedly; initialize schema idempotently and restore persisted phase.
- Transactionally persist room epoch, roster, manifest intent, phase and last
  control sequence. Keep transient socket/SDP/input data in bounded memory.
- No timers that prevent hibernation. Use idempotent alarms only for expiry.
- On restart, clients reconnect by lease and resend bounded unacknowledged
  control tail. Existing peer data channels remain useful.

**Gate:** restart/evict object at every phase; state either resumes exactly or
closes with the documented recoverable reason.

### ON-02 — Authenticated WebRTC connectivity graph

**Create:** shared `party-peer` transport extensions and native adapter seam.

- Attempt pairwise channels at up to four endpoints, but do not require all six
  edges. Select deterministic bounded routes from measured reachability. A
  direct/one-hop route requires graph diameter ≤2; a longer chain must use
  admitted fallback or refuse honestly. Never expose SDP/IP in logs or
  diagnostics.
- `match-input-v1`: unordered, `maxRetransmits: 0`; redundant recent frames from
  S12's canonical input codec. `match-control-peer-v1` is reliable for peer
  acknowledgements and diagnostics; the room DO remains phase authority.
- Each input bundle is source-authenticated end to end and contains epoch,
  source slot, sequence/tick window and hop count. An intermediate peer may
  forward it once but cannot impersonate its source; loops and stale epochs are
  rejected before decode into histories.
- Derive a distinct sender→recipient gameplay key through the authenticated
  room handshake. Forwarded and relayed packets remain recipient-encrypted;
  never give every participant one symmetric key that permits impersonation.
- Show one stable room verification phrase in Connection details. High-entropy
  role links resist guessing, but neither a link nor a short code alone proves a
  non-malicious signaling service. Friends can compare the phrase to detect key
  substitution. Never claim anonymity or protection from a malicious endpoint.
- Race direct and one-hop paths during recovery and deduplicate by source/epoch/
  sequence. WebSocket fallback uses the same inner encrypted bytes.
- Backpressure drops superseded input bundles. Message size stays below measured
  path MTU; never send snapshots or large logs on a gameplay channel.
- A match can continue with fixed roster if room signaling/control briefly
  disconnects; it cannot invent a roster transition locally.

**Gate:** 2/3/4 endpoint real browsers plus native combinations, including a
four-endpoint partial graph of diameter two, an unsupported length-three chain,
asymmetric edge loss, malicious forwarding, forwarding-peer departure, one
connection restart and signaling outage.

### ON-03 — Lobby UX

**Create:** platform views that consume the actual S10 `MdkrSessionCore` and
online reducer snapshots and dispatch typed commands. A “conceptually shared”
view model is insufficient.

- Primary actions remain **Play here** and **Online room**.
  Local never waits for an online capability check.
- Create returns code/link/QR; join accepts paste/type/deep link. Guest names are
  local defaults, length/safety normalized server-side.
- Seat rows group local parties, show controller readiness and connection words
  (`Good`, `Variable`, `Poor`) with raw RTT/jitter in details.
- Every wait has progress, cancel and bounded timeout. Every error names one
  next action. No generic `Network error` when a typed reason exists.
- Keep **Connection details** one disclosure away. A stepwise Connection Doctor
  distinguishes service, compatibility, direct graph and fallback capacity;
  it never asks players to interpret ICE candidates or error codes.
- The in-race shell overlay is non-pausing. It shows current route/quality,
  Controls and deliberate Leave; gameplay-affecting settings are deferred.
- Keyboard/gamepad complete, visible focus, status live regions, reduced motion,
  non-color status and compact mobile layout.

**Gate:** shared transition-fixture tests plus real browser/native command routes,
first-time friend-pair usability sessions and local availability with Worker
DNS/HTTP failure. Measure create-to-share, join-to-ready, wrong-role scans,
backtracking and whether each failure's next action succeeds.

### ON-04 — Compatibility preflight

- Client computes one manifest proposal from existing provenance, supported ROM
  revision enum, simulation cadence, gameplay keys and content/mod digest. Never
  send ROM bytes, filename or a digest usable as hosted content lookup.
- Alpha requires exact build provenance and no gameplay-changing mods/codes.
- Room compares field by field and returns actionable mismatch (`Update Golden
  Balloon`, `EU 1.1 cannot join a US 1.1 room`, etc.).
- Presentation/accessibility/language settings remain local and are excluded by
  an explicit allowlist backed by state-invariance tests.
- Measure direct connection quality before ready; poor is warning, impossible is
  blocked or offered capped fallback.
- A waiting service-worker update may wait through the session. A required
  version blocks ready with **Update and rejoin**, preserves only the scoped
  invite in session storage and activates outside a live race. Never mix client,
  reducer or wire versions silently inside one room epoch.

**Negative control:** each manifest field mismatches independently; presentation
changes must not reject.

**Gate:** every supported exact match reaches Ready on native/browser fixtures;
every gameplay mismatch blocks with its prescribed recovery action, and a
required update rejoins the same invitation outside a live race.

### ON-05 — Match phase barriers

- Character conflicts resolve before load; each seat votes one track. Seeded tie
  resolution is in signed manifest.
- Estimate endpoint monotonic-clock offset/drift with bounded peer/control
  samples. Select input delay from measured conditions before the race and lock
  it in the manifest; recalculate only for a later match epoch.
- Loading barrier includes loaded track id, manifest digest, tick-zero snapshot
  digest and filled input-delay status—not a client-declared boolean alone.
- Countdown names a sufficiently future local start instant and canonical tick
  and remains cancelable until commit. Bounded host pacing may change when a
  fixed tick is offered; it never changes `dt`, skips or duplicates a tick.
- Excessive drift, background throttling or inability to meet the barrier yields
  a typed recovery before countdown rather than starting a doomed race.
- Results require terminal tick confirmation and hash agreement; disagreement
  invalidates casual result and offers sanitized diagnostics.
- Rematch increments epoch, rotates RNG seed and clears all prior input/snapshot
  histories before new load.

**Gate:** 2P–4P load skew, cancel, duplicate ready/start, finish disagreement,
result return and two rematches preserve the room while rotating match state.

### ON-06 — Reconnect and membership

- Separate socket, endpoint and seat leases. Refresh/reconnect can replace a
  socket without duplicating a seat.
- Exponential backoff with jitter and a visible countdown; do not synchronize a
  reconnect storm.
- During race, S12 prediction covers brief gaps. After grace, room names one
  future AI-takeover tick. First release returns reconnected person next race.
- Leader transfer is immediate for options but never changes data topology.
- 1v1 disagreement has no majority winner; a room-authorized command freezes at
  a confirmed boundary or invalidates the race. No endpoint pauses locally.

**Gate:** each participant/leader leaves at every phase, tab refresh, sleep/wake,
duplicate reconnect, expired lease and room-object restart.

### ON-07 — Hard-capped fallback

- Extend S11 RelayBudget leases for match input. Admission accounts for players,
  expected messages, maximum 30-minute lease and safety reserve below free-plan
  daily requests/duration.
- Fall back only after direct ICE timeout and explicit consent to degraded mode.
- Rate limit at connection and room; stale/superseded frames are discarded
  before broadcast. Control traffic always has reserved capacity.
- Gameplay bundles use the S10 application-layer AEAD envelope with per-room/
  epoch keys, nonce discipline and replay rejection. The relay can route only
  ciphertext and bounded metadata; transport TLS alone is not sufficient.
- Kill switch refuses new relay leases. It does not tear down direct matches.

Operational invariant: no code path can upgrade the account or enable metered
Realtime. Provider quota exhaustion produces a typed capacity error and $0 bill.

**Gate:** force direct failure at each reserve boundary; eligible rooms receive
decryptable/deduplicated fallback input, over-capacity rooms receive one typed
recovery, and ciphertext/nonce-replay controls fail closed.

### ON-08 — Security, privacy and abuse verification

This closes accumulated controls; it cannot introduce a missing prerequisite
validation rule. Any such discovery reopens the owning ticket.

- Strict origin, method, media-type, body-size, schema/version and state-machine
  validation. Fuzz codecs and command reducer.
- 128-bit internal ids/capabilities; six-character codes rate-limited and short
  lived. Store credential digests, rotate epochs, constant-time verification.
- CSP/no-referrer/nosniff, no third-party resources, no input/SDP logging.
- Guest-only private rooms need reject/kick and room close. Public chat/reporting
  is not in scope; curated quick-chat only after gameplay stabilizes.
- CI privacy capture rejects 12 MiB bodies, save/snapshot magic, known asset
  paths, PCM/frame data, credentials in URLs and external ROM requests.
- Dependency audit, locked packages, pinned GitHub Actions and secret scanning.
- Red-team role confusion, code enumeration, QR replay, duplicate-tab lease
  theft, command replay, future-tick/rollback-window exhaustion, forwarding
  impersonation, malformed SDP, AEAD nonce/replay faults and free-quota attacks.
- Show direct-peer address disclosure before creating/joining a private room.
  Do not imply anonymity. Quick match remains unavailable in guaranteed-$0.

**Gate:** a checked-in red-team report maps every S10 threat to prevention,
detection and recovery evidence; privacy/dependency/secret gates are clean. No
P0 security rule first appears in this ticket.

### ON-09 — Free observability and objectives

Prefer counters/histograms with bounded cardinality. No player names, room codes,
IP/SDP, input bytes or raw state hashes in centralized logs.

Measure locally and upload aggregates only with documented retention. Store a
bounded daily aggregate sufficient for the release error budget; raw provider
request logs are short-lived debugging aids, not the SLO source of truth:

- create/join/preflight/start success by typed reason;
- direct versus fallback path, RTT/jitter/loss, rollback depth, confirmed lag;
- reconnect and AI takeover, first divergent tick family;
- Room constructor/recovery count, command errors and relay admissions;
- daily free-plan request/duration/storage reserve;
- product UX: create-to-share, join-to-ready, start abandonment, recovery-action
  success and update/rejoin completion, all without name/code dimensions.

Initial beta objectives:

- ≥99.5% start success on the qualified direct-connect network profile;
- ≥99.9% of started races avoid infrastructure-caused abort;
- 100% detected hash disagreements invalidate results;
- p95 reconnect ≤5 seconds for a 10-second induced link interruption;
- p95 rollback ≤3 ticks and p99 ≤6 on qualified regional profile;
- zero backend-captured ROM/save/snapshot bytes;
- local play works during every synthetic backend outage.

Broader real-world no-TURN connection success is reported separately, never
folded into the qualified number.

**Gate:** dashboards reconcile against a deterministic synthetic day, reserve
alarms fire before admission floors, and a forbidden high-cardinality/name/code
metric fails CI.

### ON-10 — Load, chaos and deploy safety

**Implemented local checkpoint:** the production Worker/static-assets shape
passes an exact small-budget stop line, a 64-request mixed create flood, typed
and no-store create/join refusal, preserved authenticated MatchRoom and Phone
Party control/close traffic, secure static fallback, accessible browser
recovery and an independent literal-zero kill switch. A real process restart
after exhaustion reconstructs the refusal latch plus both room types without
reopening admission. Its separately keyed
operator route exposes only an exact no-store daily aggregate, rejects
unauthorized reads before Durable Object access, latches each refusal category
once and alarm-deletes the shard after 32 days. Provider reconciliation,
broader metrics, deploy-skew/service-worker choreography, the broader chaos
matrix and staging canary remain open.

The static-client half of deploy skew is now executable: two stamped releases
run through a real Chromium service-worker lifecycle. The old worker keeps
control without `skipWaiting`, refuses to cache a newer document under its old
build key, recovers one complete old release during an injected outage, then
allows the waiting worker to activate and delete stale cache only after the old
document closes. The Worker/object half now has an explicit v1 envelope on all
15 calls, accepts legacy-unversioned requests for old-Worker/new-object skew,
and rejects unknown versions at all four object classes before parsing/storage.
Any v2 follows expand → full-room drain → emit → late contract. Hosted version
assignment and rollback still remain.

The remaining provider-edge risk now has a checked least-cost control: a
Free-plan zone payload spends its single 30-per-10-second IP rule only on
`/api/`, before Worker invocation. Static launcher/controller/room/service-
worker routes are structurally outside it, and a provider HTML `429` maps to the
typed local-first capacity view. Installation, shared-NAT observation and
distributed low-rate exhaustion remain hosted evidence; the rule is not called
a quota reserve.

Authenticated Phone Party sockets now reserve their full 512-message lifetime
(with an independent 120/10-second burst cap), and MatchRoom state sockets have
a smaller explicit weight. This closes the valid-credential slow-flood bypass
of internal accounting. No application-level counter can reserve capacity
against arbitrary raw requests rejected at or before the provider boundary;
that residual availability risk requires provider controls and hosted evidence,
while free-plan exhaustion must continue to leave static local play available.

Control reserve is no longer chargeable by a merely well-shaped bearer. Each
43-character host/controller/match token is a 128-bit nonce plus 128-bit
room/role HMAC; the Worker verifies it before budget or object access, while the
room still stores only a full purpose-HMAC digest. A 64-request mixed forged-
control flood leaves control at zero, followed by successful authentic control.

- Local workerd load model plus a small staging canary. Model room creation,
  idle hibernation, signaling bursts, racing control silence and reconnect.
- Restart Room at every phase; inject delayed/duplicated commands, storage error,
  WebSocket close, Worker deploy and free quota refusal.
- Protocol/version pinned for a room's lifetime. Gradual deploy routes old rooms
  to compatible code or rejects migration; never mix reducers silently.
- Stage static assets and Worker compatibility as one release manifest. Exercise
  active/waiting service-worker choreography, hard refresh and rollback while a
  room is open; never activate a breaking asset set during a race.
- Prove gameplay continues on the peer graph through signaling outage and
  freezes at a confirmed tick when a required roster transition lacks authority.
- Capacity math and measured CPU/request counts checked into the run report.

**Gate:** version skew, object restart, quota refusal, signaling outage, static
rollback and service-worker update scenarios all land in their specified
recoverable state while **Play here** remains available.

### ON-11 — Runbooks and beta

**Create:** `docs/ops/multiplayer/{deploy,incident,privacy,capacity,rollback}.md`.

Runbooks cover:

- deploy/migrate/verify/rollback Worker and static clients;
- kill new rooms, kill fallback only, or disable online CTA independently;
- free quota nearing/exhausted; abuse flood; signaling regional outage;
- elevated desync/rollback/disconnect; leaked secret/capability;
- data deletion and privacy request; dependency/security advisory.

Staged release: maintainer rooms → invited cohort → private beta. Advancement
requires seven consecutive days of the bounded SLO aggregate, device/network
matrix, usability evidence and no privacy/cost invariant breach. One-action
rollback leaves Play here and Phone Party intact.

### ON-12 — Mixed couch+online and nearby mode

After one-seat endpoints are stable:

- endpoint contributes two local seats, using S11 source router/mapping;
- lobby groups seats under one endpoint; loss of endpoint handles both leases at
  one canonical control tick;
- each endpoint renders only its local 1P/2P views;
- nearby mode labels online-assisted signaling versus true offline native LAN;
- native true-LAN discovery/signaling may bypass cloud while reusing exact room,
  manifest and match protocols.

Enroll each topology separately: 2+1+1, 2+2, 3+1, one display with four phone
controllers, and combinations with physical controllers.

## Definition of done

- Private rooms go from link/code/QR to a complete race/rematch with typed,
  recoverable failures and no player-host dependency.
- Direct/one-hop gameplay remains responsive and bounded under the S12
  impairment matrix; fallback payloads remain end-to-end encrypted.
- Object restarts/deploys/reconnects are ordinary tested paths.
- Production cannot create an infrastructure charge; capacity refuses first.
- Local/couch/phone-controller play stays available through backend outage,
  quota exhaustion and online rollback.
- The privacy corpus contains no game/user payload beyond documented metadata.
- Public quick match remains visibly unsupported, peer-IP disclosure is honest,
  and every wait/recovery/update path passes the shared UX/accessibility gates.
