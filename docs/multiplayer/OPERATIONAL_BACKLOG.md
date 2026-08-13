# Multiplayer operational backlog

Last reconciled: **2026-08-13**. This is the ordered delivery queue for the
local-plus-online multiplayer product. Acceptance details live in S10–S13; the
[status ledger](STATUS.md) records evidence; this file answers **what should be
worked next, in what order, and what achievement closes each milestone**.

## Non-negotiable product contract

1. **Local never waits for online.** A ROM-ready player can always choose
   **Play here**. Phone pairing failure releases every phone input and leaves
   keyboard, gamepads and touch usable.
2. **The launcher owns the party.** QR/code pairing, seats, invites, rooms,
   preflight, votes, readiness, barriers, reconnect, results and rematches live
   above the game. The engine receives only an immutable launch descriptor and
   canonical per-tick inputs.
3. **No account and no public matchmaking in v1.** Online is invite-only.
   Quick match, rankings and trusted progression are out of scope.
4. **Zero-cost means fail-closed admission, not infinite free capacity.** The
   production account has no paid plan, billing method, metered TURN or SFU.
   New network sessions stop below free ceilings; established control/close
   traffic and all local play retain reserve.
5. **A3 controls race admission.** Neither service data, test fixtures nor UI
   state can enable a production online race before the written rollback `GO`.
6. **Security and accessibility are slice requirements.** A ticket does not
   move to Done and hand them to a later hardening phase.

## Work states

| State | Meaning |
|---|---|
| `DONE` | Automated exit evidence passes and is linked from the ledger. |
| `DONE LOCAL` | Local production-seam evidence passes; hosted/provisioned evidence is explicitly not claimed. |
| `EVIDENCE REVIEW` | Implementation and automated gates pass; named human/device/release evidence remains. |
| `IN PROGRESS` | A bounded slice is actively implemented; its exit gate is not yet complete. |
| `READY` | Dependencies and acceptance contract are complete; work may start. |
| `GATED` | Starting would bypass a safety or architecture decision. |
| `HUMAN / PROVISIONING` | Code can progress, but closure needs devices, accounts, secrets, DNS or named approval. |

No ticket becomes Done because a happy-path demo worked. Its negative controls,
recovery, accessible route, redaction and rollback proof must pass too.

## The steady march

```text
M0 contracts ──► M1 local party ──► M2 rollback GO ──► M3 room UX
                                                        │
                                                        ▼
M8 beta ◄── M7 operated alpha ◄── M6 resilient room ◄── M4 hosted control
                                      ▲
                                      └──── M5 direct authenticated transport
```

M1 and M2 can advance in parallel. M4 may be implemented against local Workers
emulation before M2, but no hosted Online Race capability is deployed or
admitted until M2 is `GO` and M3 passes first-time usability.

## Ordered backlog

| Order | ID | State | Achievement | Depends | Exit evidence |
|---:|---|---|---|---|---|
| 1 | MP-00 | DONE | Versioned session, Party, room, descriptor and security boundaries are frozen | — | Cross-language codecs/reducers, fail-atomic mutations, threat/data records |
| 2 | LP-01 | IMPLEMENTED / EVIDENCE REFRESH | Browser Phone Party pairs by QR/code with no app and drives explicit split-screen seats | MP-00 | The browser host/controller paths now use actual receipt-relative invite TTLs, exact bounded HTTP/socket state, finite generation-guarded recovery, manual phone Retry, QR-to-`/controller/`+code fallback and server-targeted per-controller signaling. The service and client require one canonical same-origin HTTPS boundary (HTTP only on loopback). Private/public recovery copy is truthful. Duplicate-tab takeover explicitly neutralizes/closes the prior publisher before an ordinary exclusive Web Lock; rejection fails closed, while the no-Web-Locks fallback retains only a hashed expiring tab lease. The service rebuilds exact role-specific SDP/ICE/hello envelopes, injects phone identity from socket custody and exact-validates native host commands before reserve or mutation. Launcher-owned per-seat removal uses a named safe-default confirmation, cancel-without-mutation, contextual focus, exact lease release and a targeted terminal socket close without disturbing other phones. Authored Worker/browser fixtures cover origin refusal, tab custody, rotation generation, ambiguous signaling, QR failure, two-controller non-disclosure and removal isolation; dedicated execution plus four physical iOS/Android phones remain. |
| 3 | LP-02 | DONE LOCAL / EVIDENCE REVIEW | Native launcher hosts the same browser controller without localhost, certificates or firewall prompts | LP-01 | Pinned static adapter, one-WSS bootstrap/reconnect, shared codec/router, QR/SAS vectors, launcher/overlay lifecycle, fail-neutral queues and release notices pass locally. A MinGW GCC 16.1 Release build and stock-Windows-DLL import audit also pass; executed Windows/macOS/Linux packages and a physical mixed-source race remain |
| 4 | RB-01 | IN PROGRESS | Four endpoints deterministically race, recover within bounds and unwind safely beyond them | MP-00 | Track/vehicle/item/soak, four-endpoint and real Chromium/Wasm matrices plus memory/timing budgets pass; PAL/desktop-OS, physical-device and written `GO` evidence remain |
| 5 | UX-01 | IMPLEMENTED / RENDER EVIDENCE REFRESH | Native Online Room covers every state/failure/action with accessible real input | MP-00 | Shared model and native projection compile with a bounded, announced phrase plus explicit **Words Match**/**Words Differ** actions; rerun the 43-case/27-action keyboard/gamepad/speech gallery on a dedicated desktop, then human SR/device review |
| 6 | UX-02 | IMPLEMENTED / EXECUTABLE + RENDER EVIDENCE REFRESH | Browser renders the same room model, not a second room state machine | UX-01 | Native browser-ABI conformance previously drove all 43 cases/10 kinds/18 failures/27 actions plus both phrase paths under normal/ASan; the shipped pure presenter covers every projection and live action. The ABI-v4 exact live-state boundary deep-copies the public MatchRoom envelope, rejects private/unknown/impossible or regressing/equivocating state, correlates the control tail with final lobby authority, holds identity stable and revokes stale invite generations. Valid older HTTP publications are no-ops; a delayed rotate response can restore only the exact in-flight expected/current generation secret while newer member state and local leader custody remain authoritative. Role links now cross only a same-origin fragment redirect: the launcher scrubs before configuration access, never uses web storage, drops authority immediately in disabled builds, caps enabled pre-ROM closure custody at ten minutes, and abandons redemption if either History API scrub fails. Invite secrets use a conservative receipt-relative deadline, leave memory/DOM before expiry rendering and expose leader-only **New Invitation** recovery without disturbing joined friends. Public state commits only after projection/presentation succeeds, while expired secrets are never rolled back. Accepted guest Leave is terminal after credential revocation and stale-revision recovery explicitly says the action was not applied. The exhaustive Node boundary gate and browser timer/publication/race expiry/scrub-failure arms are authored/syntax-valid, and focused C targets compile; run the dedicated executable/browser/rendered native correlation/phrase/invite AX/reflow batch. Start stays locally gated. |
| 7 | UX-03 | IN PROGRESS / HUMAN EVIDENCE | Two new players complete invite, preflight, cancel and rematch without coaching | UX-02 | Two clean automated profiles pass real-Worker create/share/role-link join/outage/select/Ready/backtrack/leave with bounded timings and AX checks. Phone/host destructive leave has safe-default confirmation and cancel-before-mutation; semantic dynamic forms, contained modals and notch-safe handoff pass. Both analog surfaces provide visible-focus Arrow/WASD steering, bounded diagonals, spoken direction and fail-neutral lifecycle behavior. Uncoached human task study, physical assistive-device review, cancel/rematch observation and any resulting P0/P1 closure remain. |
| 8 | SV-01 | DONE LOCAL | MatchRoom persists bounded control state and survives object eviction/restart | MP-00 | Full local lifecycle; every-phase restart, hibernated socket, alarm expiry, v3 package dry-run, purpose-HMACed and generation-rotated invites, idempotency and split-brain gates |
| 9 | NET-01 | FOUNDATION LOCAL / LIVE CHANNEL BINDING GATED BY A3 GO | Authenticated direct/one-hop graph carries redundant inputs | RB-01, SV-01 | Isolated exact-target signaling relay with fail-contained delivery/replacement commit, dormant browser adapter, full-direction-bound peer crypto with seal-window-owned monotonic nonces/concurrent-browser exclusion, authenticated immediate-source generation forwarding and $0 lifetime reservation pass locally; 2–4 endpoint NAT/partial-graph/asymmetric-loss matrix and unauthorized-input live proof remain |
| 10 | ROOM-01 | FOUNDATION LOCAL / LIVE BINDING GATED BY A3 GO | Exact preflight freezes one signed launch descriptor | UX-03, SV-01, NET-01 | Native/browser fixed-report, canonical-graph and authenticated-direction-bound three-fragment carrier vectors cover build/ROM/transcript/generation/route/phrase/channel disagreement, route equivocation, cross-source splicing/forged attribution, withdrawal, stale/conflicting retries and corrupt state over a direct/opaque one-hop-capable format. Shared launcher phrase UI and explicit confirmation are implemented; real authenticated-channel binding and physical phrase evidence remain |
| 11 | ROOM-02 | FOUNDATION LOCAL / LIVE BINDING GATED BY A3 GO | Vote, load, countdown, results and rematch barriers are cancel-safe | ROOM-01 | Shared C/TypeScript reducer trace and broader lifecycle suites cover cancellation, phase/revision/epoch barriers, results and rematch atomically; stale live callbacks and carrier/deploy replay remain vertical-slice evidence |
| 12 | ROOM-03 | FOUNDATION LOCAL / LIVE BINDING GATED BY A3 GO | Leases, leader transfer, reconnect and deterministic AI takeover preserve custody | ROOM-02 | Reducer custody/reconnect/leader tests and four-process deterministic takeover prove neutral history, no double owner and no hand-back locally; live outage/rejoin/leave timing remains |
| 13 | SEC-01 | INDEPENDENT LOCAL RED-TEAM READY / LIVE SLICE GATED | Red-team and abuse review closes product and service attack paths | SV-01..ROOM-03 | The dated handoff maps invite/auth/generation/replay/origin/bounds/privacy/cost/crypto hypotheses to exact code and safe commands; independent review may start now, while live carrier, hosted abuse and provisioned-network findings remain open |
| 14 | OPS-01 | IN PROGRESS / DONE LOCAL ADMISSION + COST OBSERVABILITY + RECONCILIATION CONTRACT | Metrics and admission prove admitted work cannot bill or consume its internal close/control reserve | SV-01..SEC-01 | Real-Worker 64-way admission plus 64-way forged-control floods, persisted process restart, typed/no-store refusal, pre-budget room/role credential binding, weighted HTTP/socket reserve, socket-lifetime bound, secure static fallback and literal-zero kill switch pass locally; protected daily snapshot/refusal/retention, fixed no-extra-write reservation buckets with legacy tracking, one free `/api/`-only edge rule and a strict privacy-safe internal/provider/zero-charge reconciliation gate pass locally. Provider schema v2 includes the separate Durable Object duration ceiling. The v3 ledger and controlled production-path runner fix a 20-attempt synthetic MatchRoom create/join and Phone Party direct/input success-latency contract with no user analytics or metrics write; a one-attempt local end-to-end smoke passes and remains non-qualifying. Hosted execution, edge false-positive test, distributed raw-request risk, provider export, real seven-day ledger and drill remain. |
| 15 | REL-01 | HUMAN / PROVISIONING | Invited alpha is deployable, observable and reversible | OPS-01 | named owners, DNS/secrets, canary, rollback, incident/privacy/capacity runbooks |
| 16 | REL-02 | GATED / LEDGER CONTRACT DONE LOCAL | Zero-cost beta meets its error budget without weakening refusal behavior | REL-01 | exact fail-closed seven-day validator passes adversarial fixtures; real contiguous hosted canary, device/network matrix, privacy review and signed go/no-go record remain |

LP-01's latest custody delta also fixes two recovery paths that previously used
the already-scrubbed controller URL. Phone entry now scrubs before configuration
access, rejects query/route ambiguity, abandons redemption on scrub failure and
keeps the capability closure-only. Embedded/unsupported **Share private link**
with **Copy private link** fallback reconstructs the exact private link only for
that user gesture; capability-free recovery shares exact public `/controller/`
and requires the current code. Duplicate **Use this tab**
first makes the prior tab neutralize/close, then reconstructs it for one
ordinary exclusive-lock navigation and immediate re-scrub. Lock errors refuse;
the no-Web-Locks fallback stores only a hashed 15-second lease. The browser
copy/share/reclaim/fallback-lease/scrub-denial arms are authored and await the
dedicated desktop evidence refresh.

## Current executable checkpoint

The following is real now and should be extended rather than rewritten:

- `MdkrSessionCore`, room reducer, lobby view model and fake adapter are pure,
  versioned and fail-atomic.
- Native Phone Party is launcher-owned across engine loans and rematches. A
  pinned, media-disabled libdatachannel/Mbed TLS adapter opens one verified WSS
  dependency, creates or reconnects the ordinary PartyRoom, negotiates direct
  unordered state plus reliable control DataChannels, and feeds the same
  bounded `MdkrRemotePad`/`PadRouter` path as the browser. The launcher and F1
  overlay render the same ECC-Q invite, six-digit fallback, transcript phrase,
  explicit replacement slot, approval/removal confirmations and typed local
  fallback. No game source contains matchmaking, URLs, credentials or service
  code. Physical/device/platform release evidence is deliberately still open.
- The 148-byte checksum-protected V3 descriptor copy-owns match identity across
  launcher, bridge and engine; the engine does not own party state.
- Native Online Room has 43 deterministic cases covering all 10 view kinds and
  18 typed failures. Its shared model now exposes 27 public actions, including
  human-paced **Words Match** and **Words Differ** decisions; malformed/stale
  phrases fail atomically, mismatch preserves the room for a fresh secure
  preflight, and rematches repeat comparison. The prior 25-action rendered
  input baseline passes; expanded 27-action desktop evidence remains. Timeouts
  are shown only after a local deadline actually expires.
- A confirmed online-race leave orders engine stop before disconnect and cannot
  terminate a local race.
- Browser Online Room remains an honest launcher-owned unavailable surface in
  the shipped default. `online-control-config.js` is the publisher-owned,
  immutable release switch and ships with `enabled: false`; the model remains
  unloaded and opening the surface makes no room-service request. An enabled
  release is accepted only for the page's exact origin, a clean semantic-version
  build with a 40-hex source commit and a locally full-SHA-verified supported
  ROM. The launcher derives bounded build/gameplay compatibility identifiers
  without retaining or transmitting ROM bytes. Activation is idempotent;
  dirty provenance, unknown ROMs and cross-origin targets return to the same
  local-first gate. Explicit gallery/live adapters load a sub-128-KiB ROM-, engine-,
  provider- and storage-free Wasm projection compiled from the native C
  reducer/view model. The live seam validates MatchRoom snapshots then projects
  them through that same C model. Create, fragment/code join, QR/share,
  generation rotation, selection, Ready, reconnect and corrupt-state recovery
  pass; an adversarial service admission field still cannot expose Start Race.
- Two clean Chrome profiles now share the real local Worker/Durable Objects and
  complete create, role-link handoff, join, socket outage/recovery, distinct
  selections, Ready convergence, selection backtrack and leave. Local recovery
  stays visible inside the live surface. Browser share/clipboard integration is
  bounded to two seconds before showing the room-code fallback; Retry restores
  both authenticated state and its state socket. This is automation evidence,
  not a substitute for the remaining uncoached two-person study.
- Local MatchRoom emulation now provides capability/code create/join,
  leader-only generation-checked invite rotation, credential-bound commands,
  complete race/rematch barriers, bounded control
  history, hibernated state sockets and exact restart/expiry recovery. Raw
  invite/code/credential values are absent from durable storage.
- The publisher applies one build token to every launcher, Phone Party,
  controller and `/room/` JavaScript/CSS reference and fails closed if either
  role document is absent, preventing mixed protocol/client generations from
  ordinary browser caches. A real-browser two-release gate now proves the old
  worker/new document waiting window, build-isolated caches, atomic old-build
  offline fallback and new activation only after the old client exits.
- All 16 Worker→Durable Object calls carry internal protocol v1. All four object
  classes keep the legacy-unversioned reader for old-Worker/new-object skew and
  reject unknown versions before parsing/storage. Future protocol/schema work
  is ordered as expand → 24-hour admission-off drain → emit → late contract;
  percentage gradual deployment is forbidden for the v1 full-stack Worker.
- A real local Worker capacity run exactly consumes a small admission line,
  rejects a 64-way mixed room-creation flood plus new joins with one bounded
  `service_budget_safe` response, and still services authenticated MatchRoom
  state/mutation/close plus Phone Party close. Static launcher/controller/room
  routes and security headers remain available. A separate literal-zero run
  refuses the first admission. The browser renders calm assertive copy with
  **Choose ROM** and **Try Again**; no-ROM state is never mislabeled **Play
  Here**. A separately keyed, no-store operator snapshot reports exact bounded
  daily aggregates, rejects missing/wrong credentials, contains no tested
  identity/capability canary, latches the first refusal without flood-amplified
  writes and expires its shard after 32 days. A strict offline reconciliation
  gate rejects schema/arithmetic drift, private-field additions, paid billing,
  changed Free ceilings and either internal/provider stop line without
  reflecting input. A companion exact health view reconstructs admitted units
  from fixed operation buckets inside existing writes, flags old-Worker traffic
  and proves forged/refused floods cannot amplify metrics. Hosted provider
  exports, the hosted controlled synthetic-canary run and a real seven-day
  zero-currency ledger remain. MatchRoom state sockets now byte-bound forbidden UTF-8/binary input
  before close, and 24,576 seeded reducer commands prove deterministic,
  fail-atomic bounded state across repeated reconstruction.
- The seven-day beta ledger now has an exact 512 KiB-bounded schema and
  adversarial validator. It re-runs daily cost reconciliation and health
  weights, requires contiguous UTC dates, immutable build/deployment digests,
  local-play probes, no open incidents and daily/final `GO`, without reflecting
  injected input. No fixture day counts as operated evidence.
- Production race admission remains false. No hosted MatchRoom or online input
  path is claimed.

### Release-switch operating rule

`dist/web/online/online-control-config.js` is changed only in a reviewed,
immutable release commit—never with a query parameter, local storage, remote
feature flag or service response. Promotion order is strict:

1. keep the file disabled through local, fixture and hosted-control validation;
2. obtain written A3 `GO`, close UX-03 P0/P1 findings and record security,
   privacy, capacity and operations approval;
3. build from a clean commit, enable only the same-origin canary, and prove
   **Play here** with the service blocked before inviting any cohort;
4. expand the cohort only while quota/control reserves and the signed error
   budget pass; rollback republishes the disabled static policy first;
5. race admission remains a separate locally compiled gate. Enabling room
   control cannot manufacture **Start Race**.

Evidence commands:

```bash
# Dedicated test desktop only: these commands launch real Chromium profiles.
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_BROWSER_TESTS_ALLOWED=1 python3 tests/check_browser_online_room.py --shell-dir dist/web
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_BROWSER_TESTS_ALLOWED=1 python3 tests/check_browser_online_activation.py --shell-dir dist/web
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_BROWSER_TESTS_ALLOWED=1 python3 tests/check_browser_online_match_room.py --shell-dir dist/web

# Occupied workstation: do not invoke a runner or any file under tests/.
# Use source inspection, syntax/parser/type checks and bounded compilation only.
```

## Slice template

Every implementation PR/ticket must carry these fields. If one is unknown, the
ticket is not Ready.

```text
Outcome:          one player-observable sentence
Owner:            code owner and operational owner
Dependencies:     ticket IDs and protocol/schema versions
In scope:         exact state/actions/effects
Out of scope:     tempting adjacent work
Happy path:       deterministic fixture or task
Negative paths:   stale, duplicate, unauthorized, timeout, cancel, exhaustion
Security delta:   capability, trust, bounds, retention, logging, abuse
Privacy delta:    fields collected/transmitted/stored/deleted
Accessibility:    focus order, names, live priority, keyboard/gamepad/touch/SR
Responsive:       minimum native and browser viewports, 200%, rotation
Cost delta:       requests/bytes/storage/alarms at normal and adversarial load
Observability:    bounded metric/event; no secrets, names, addresses or inputs
Rollout:          local → fixture → canary → cohort; admission default false
Rollback:         kill switch, version compatibility and durable-data handling
Evidence:         exact automated command plus named human/device evidence
Documentation:    protocol, threat/data map, UX catalog and runbook updates
```

## Definition of Ready

- The ticket advances one milestone and names a single state authority.
- Wire/storage/UI schema changes are versioned before adapters are written.
- Every external string maps to a bounded product enum before reaching UI or
  logs. Provider messages never become player copy.
- Payload, collection, queue, retry, time and retention bounds are explicit.
- Local fallback and cancel/timeout behavior are designed before loading copy.
- Free-tier request/byte/storage/alarm cost is modeled with adversarial margin.
- The test can run locally without production credentials; human provisioning
  is isolated to a final named evidence step.

## Definition of Done

- Happy path and all named negative controls pass on the production seam.
- Duplicate and stale work is idempotent or rejected without partial mutation.
- Cancel retires callback tokens/epochs and cannot be resurrected.
- Loss, hide, disconnect and overflow publish neutral controller/input state.
- Keyboard, gamepad and touch can reach every visible action; screen readers
  hear state/title and actions with correct polite/assertive priority.
- 640×480 native and 320×568 browser layouts reflow at 200%; focus is visible,
  stable on updates and restored after modal dismissal.
- Diagnostics omit capability secrets, codes, credentials, SDP, addresses,
  names, ROM/save/snapshot/frame/audio data and per-tick inputs.
- Admission remains locally configured and false by default. Service fields
  cannot change it.
- The evidence ledger, UX copy catalog, protocol/threat/data docs and relevant
  runbooks are reconciled in the same change.

## Security baseline from the first byte

| Area | Required control | Mandatory negative proof |
|---|---|---|
| Capabilities | 128-bit random secrets, role/path scope, short expiry, rotation, hashed server custody | old/wrong-role/replayed capability fails |
| Identity | Ephemeral peer keys and transcript/SAS binding; no account identity | mismatched transcript/peer cannot approve or send |
| Commands | protocol, phase, epoch, actor, monotonic id and expected revision | stale/duplicate/conflicting/unauthorized command is atomic |
| Inputs | authenticated endpoint/slot custody, redundancy bounds, neutral on loss | spoofed slot, oversized bundle and post-revoke input stay zero |
| Browser | same-origin requests, restrictive CSP/Permissions Policy, fragments erased before request | third-party asset/origin and leaked fragment fail gates |
| Service | strict method/type/body/rate/state bounds; hibernatable bounded sockets | exhaustion refuses new admission while close/control reserve passes |
| Storage | minimum bounded state, explicit TTL/alarm deletion, versioned migrations | restart/deploy/expiry cannot retain secrets or fork state |
| Diagnostics | structured bounded enums/counters only | secret/name/address/SDP/input canaries never appear |
| Supply chain | pinned dependencies, provenance and trusted release guidance | unpinned/new external origin fails release checks |

## Required before the written A3 `GO` — carrier review findings (2026-08-13)

These came out of the code review of the online-multiplayer campaign. None was
exploitable at the time: the carrier, preflight and signaling modules are
imported by nothing but their tests, and the publisher policy still ships
disabled. Each becomes reachable the moment a live carrier is bound, so all five
were required to close before the written `GO`. **All five are now closed
(2026-08-13);** the remaining `GO` conditions are the human/device evidence
tracked in `STATUS.md`, not these findings.

**Decided during landing review, 2026-08-13 — tied-publication invite custody
is fail-closed.** `online-room-live-state.js` previously let a publication that
ties on every ordering pair (revision, matchEpoch, leaderGeneration,
inviteGeneration), or that is outright obsolete, install an invite secret it
carried. Ruled: **only a strictly newer publication may change custody.** A
delayed or replayed rotate response may now preserve custody the launcher
already holds, but may never introduce or replace a secret; re-delivering the
byte-identical held secret stays idempotent. `test_online_room_live_state.mjs`
covers both halves. This closed the disagreement between that test and the
implementation rather than lowering the test — the original author should know
the decision went against the implementation.

All five closed on 2026-08-13. Rows are retained with their evidence rather
than deleted: closing a security finding means pointing at the proof, not
removing the record.

| # | Finding | State | Evidence |
|---|---|---|---|
| G-1 | **The verification phrase was worth ~15 bits, not 30, against an active MITM.** The transcript had no key-commitment round, so a man in the middle could pick its key toward A *and* its key toward B and grind both for a phrase collision — a birthday search, not a preimage. | **CLOSED** | Two-round ZRTP-style commitment added: `platform/net/match_peer_transcript.c` `mdkr_match_peer_commitment`/`_verify`, `dist/web/online/match-peer-crypto.js` `matchPeerCommitment`/`verifyMatchPeerCommitment`. The digest re-runs round 2 itself and refuses to emit a phrase if any commitment does not open, so the check cannot be skipped. Transcript domain bumped to `-v2` and envelope version byte to `2`; a v1 envelope is refused before decryption. Native and browser share one pinned digest `7ae1f0da…c3d2`, phrase `Neon-Parrot Nimble-Thunder Brave-Wing`, key `4569b1ed…dd1e` and 132-byte envelope vector. Both suites sweep every commitment and nonce byte. **Re-measured:** the old birthday search found a collision in **19,231 keypairs per side (0.15 s)**; against the committed round, **0 successes in 3,000,000 full sessions** (0.0028 expected at 2^-30). See `docs/ref/match-peer-carrier-v1.md` §Evidence. |
| G-2 | **The nonce-reuse guard bound the key object, not the key material.** Two derivations of identical inputs yielded two wrappers over one AES key, each starting a seal window at sequence 1 — a repeated `(key, nonce)` under AES-GCM leaks the GHASH subkey. | **CLOSED** | Derivation now mints the window. C: `MdkrMatchPeerSealingKey` carries the key and its one window together, `mdkr_match_peer_seal_window_init` is gone, and a caller-owned `MdkrMatchPeerKeyring` returns the *same slot* for identical inputs (reentrant — no global state); a full ring fails closed. JS: `deriveMatchPeerKey` returns `{key, sealWindow}` and a module registry keyed on a hash of the inputs returns the same record on repeat; `createMatchPeerSealWindow` is no longer exported. Pinned in both suites — including that both ECDH directions resolve to one shared record whose sequence advances 1 then 2 instead of restarting. |
| G-3 | **Global budget exhaustion from unauthenticated requests.** Admission was charged before the body was parsed, so ~1,000 empty POSTs could spend the day's pairing reserve and fail-close the service until UTC midnight. | **CLOSED** | Every route in `services/party/src/worker.ts` now validates shape and (where present) credentials before charging; ordering contract documented on `budget()` at `:46-66`. **Design decision:** a validated *and authenticated* request that is then refused still charges and is never refunded — by then it has consumed the Durable Object fanout it is billed for, and refunding refusals would make the cheapest abuse the free one. Pinned by `test/worker.test.ts:566-648`: 64 malformed/unauthenticated requests across 16 shapes consume **zero** units. Against the unfixed worker the same 64 spent 184 pairing + 96 control units. |
| G-4 | **Unbounded `peerGenerationHighWater` growth.** One entry per observed peer generation, never evicted, for the socket's lifetime. | **CLOSED** | `dist/web/online/match-signal-client.js` bounds the append-only map at `MAX_TRACKED_PEER_GENERATIONS = 64` and fails the socket closed with `peer_generation_overflow` on overflow. Eviction was rejected deliberately: dropping a high-water mark reopens the generation rollback the mark exists to refuse. A legal room contributes at most 3. Pinned by an identity-flood test in `tests/test_match_signal_client_js.mjs`, verified to fail when the cap is disabled. |
| G-5 | **`hibernation-contract.test.ts` was a source-text grep suite.** Assertions like `expect(matchRoom.match(/\\bsocket\\.close\\(/g)).toHaveLength(1)` cannot fail for the reason they claim and break on legal refactors. | **CLOSED** | Rewritten as five behavioral tests that drive the real objects through `evictDurableObject(…, {webSockets: "hibernate"})` and observe the actual socket lifecycle: sockets survive eviction, restored attachments still drive routing, close custody publishes absence for exactly the closing generation, a dead peer neither fails a command nor blocks a survivor, and native/HTTP authority stays serialized. Two claims remain grep-shaped because workerd exposes no way to observe them — absence of timers/outbound sockets, and close-call provenance — and both were shrunk so they cannot pass vacuously: the first pairs each matcher with a positive control, the second resolves the helper by name and asserts every close falls inside its span. |

## UI/UX acceptance ladder

Each new state walks this ladder in order:

1. Pure model fixture: title, explanation, calm status, one primary action,
   secondary/Cancel, timeout outcome and announcement priority.
2. Static renderer: non-color status, readable hierarchy, local fallback and no
   layout overflow.
3. Production input: mouse/touch, keyboard and gamepad activate the actual
   widget and dispatch the typed action.
4. Assistive route: state is announced once, errors are assertive without
   repetition, every action has a stable accessible name and no focus trap.
5. Recovery: timeout/cancel/retry/leave succeeds, stale completion is terminal,
   and focus returns to the action or stable destination.
6. Human task: first-time players complete it on representative desktop,
   handheld, phone and network conditions.

Do not add volatile ping numbers to the primary journey. Use **Direct**,
**Connected**, **Reconnecting** and **Limited connection**; put bounded technical
details behind a disclosure. Celebrate only durable achievements such as the
first tested controller, everyone Ready or a confirmed result.

## Provisioning and human-evidence queue

These are real blockers to release, not blockers to continued engineering:

- Four physical phones across current iOS Safari and Android Chrome for a full
  mixed-source local race, rotation/background/reconnect/haptics and VoiceOver/
  TalkBack review.
- Windows, macOS and Linux native WebRTC adapter/device qualification.
- Representative home, carrier, CGNAT, restrictive enterprise and IPv6 network
  matrix for private rooms.
- Written A3 `GO` after rollback performance, platform and human review.
- Production account/DNS/secrets and named security, privacy, operations and
  incident owners. Provision only after the deploy checklist proves no billing
  dependency and default admission remains closed.

Until these close, keep implementing local fixtures, browser parity, service
emulation, migrations, red-team tests, dashboards and runbook drills. Never
substitute local evidence for a production or physical-device claim.
