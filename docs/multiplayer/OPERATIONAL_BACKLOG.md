# Multiplayer operational backlog

Last reconciled: **2026-08-12**. This is the ordered delivery queue for the
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
| 2 | LP-01 | EVIDENCE REVIEW | Browser Phone Party pairs by QR/code with no app and drives explicit split-screen seats | MP-00 | Automated Chromium/WebRTC gates; four physical iOS/Android phones remain |
| 3 | LP-02 | DONE LOCAL / EVIDENCE REVIEW | Native launcher hosts the same browser controller without localhost, certificates or firewall prompts | LP-01 | Pinned static adapter, one-WSS bootstrap/reconnect, shared codec/router, QR/SAS vectors, launcher/overlay lifecycle, fail-neutral queues and release notices pass locally. A MinGW GCC 16.1 Release build and stock-Windows-DLL import audit also pass; executed Windows/macOS/Linux packages and a physical mixed-source race remain |
| 4 | RB-01 | IN PROGRESS | Four endpoints deterministically race, recover within bounds and unwind safely beyond them | MP-00 | Track/vehicle/item/soak, four-endpoint and real Chromium/Wasm matrices plus memory/timing budgets pass; PAL/desktop-OS, physical-device and written `GO` evidence remain |
| 5 | UX-01 | EVIDENCE REVIEW | Native Online Room covers every state/failure/action with accessible real input | MP-00 | 42-state gallery, 25 keyboard/gamepad actions; human SR/device pass remains |
| 6 | UX-02 | EVIDENCE REVIEW | Browser renders the same room model, not a second room state machine | UX-01 | Shared-C gallery passes 42 native-correlated views/25 routes; explicit MatchRoom binding passes create/join/share/rotate/select/ready/reconnect/recovery while Start remains locally gated; human SR/device evidence remains |
| 7 | UX-03 | IN PROGRESS / HUMAN EVIDENCE | Two new players complete invite, preflight, cancel and rematch without coaching | UX-02 | Two clean automated profiles pass real-Worker create/share/role-link join/outage/select/Ready/backtrack/leave with bounded timings and AX checks. Phone/host destructive leave has safe-default confirmation and cancel-before-mutation; semantic dynamic forms, contained modals and notch-safe handoff pass. Both analog surfaces provide visible-focus Arrow/WASD steering, bounded diagonals, spoken direction and fail-neutral lifecycle behavior. Uncoached human task study, physical assistive-device review, cancel/rematch observation and any resulting P0/P1 closure remain. |
| 8 | SV-01 | DONE LOCAL | MatchRoom persists bounded control state and survives object eviction/restart | MP-00 | Full local lifecycle; every-phase restart, hibernated socket, alarm expiry, v3 package dry-run, purpose-HMACed and generation-rotated invites, idempotency and split-brain gates |
| 9 | NET-01 | GATED BY A3 GO | Authenticated direct/one-hop graph carries redundant inputs | RB-01, SV-01 | 2–4 endpoint NAT/partial-graph/asymmetric-loss matrix; unauthorized input stays zero |
| 10 | ROOM-01 | GATED BY A3 GO | Exact preflight freezes one signed launch descriptor | UX-03, SV-01, NET-01 | build/ROM/settings/controller/route mismatch and downgrade controls |
| 11 | ROOM-02 | GATED BY A3 GO | Vote, load, countdown, results and rematch barriers are cancel-safe | ROOM-01 | stale callback/epoch/deploy replay cannot resurrect or start a race |
| 12 | ROOM-03 | GATED BY A3 GO | Leases, leader transfer, reconnect and deterministic AI takeover preserve custody | ROOM-02 | outage/rejoin/leave races; neutral gap; no double owner or hand-back |
| 13 | SEC-01 | READY after vertical slice | Red-team and abuse review closes product and service attack paths | SV-01..ROOM-03 | invite theft/replay, origin, size/rate, log/diagnostic, exhaustion and crypto review |
| 14 | OPS-01 | IN PROGRESS / DONE LOCAL ADMISSION + COST OBSERVABILITY + RECONCILIATION CONTRACT | Metrics and admission prove admitted work cannot bill or consume its internal close/control reserve | SV-01..SEC-01 | Real-Worker 64-way admission plus 64-way forged-control floods, persisted process restart, typed/no-store refusal, pre-budget room/role credential binding, weighted HTTP/socket reserve, socket-lifetime bound, secure static fallback and literal-zero kill switch pass locally; protected daily snapshot/refusal/retention, fixed no-extra-write reservation buckets with legacy tracking, one free `/api/`-only edge rule and a strict privacy-safe internal/provider/zero-charge reconciliation gate pass locally. The v2 ledger and controlled production-path runner fix a 20-attempt synthetic MatchRoom create/join and Phone Party direct/input success-latency contract with no user analytics or metrics write; a one-attempt local end-to-end smoke passes and remains non-qualifying. Hosted execution, edge false-positive test, distributed raw-request risk, provider export, real seven-day ledger and drill remain. |
| 15 | REL-01 | HUMAN / PROVISIONING | Invited alpha is deployable, observable and reversible | OPS-01 | named owners, DNS/secrets, canary, rollback, incident/privacy/capacity runbooks |
| 16 | REL-02 | GATED / LEDGER CONTRACT DONE LOCAL | Zero-cost beta meets its error budget without weakening refusal behavior | REL-01 | exact fail-closed seven-day validator passes adversarial fixtures; real contiguous hosted canary, device/network matrix, privacy review and signed go/no-go record remain |

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
- Native Online Room has 42 deterministic views covering all 10 view kinds and
  17 typed failures. All 25 public actions activate through keyboard and
  gamepad. Timeouts are shown only after a local deadline actually expires.
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
  local-first gate. Explicit gallery/live adapters load a 35 KiB ROM-, engine-,
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
- All 15 Worker→Durable Object calls carry internal protocol v1. All four object
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
python3 tests/check_browser_online_room.py --shell-dir dist/web
python3 tests/check_browser_online_activation.py --shell-dir dist/web
python3 tests/check_browser_online_match_room.py --shell-dir dist/web
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
