# Multiplayer delivery program

Start with the ordered [operational backlog](OPERATIONAL_BACKLOG.md), then use
the [status ledger](STATUS.md) for current evidence and S10–S13 for normative
acceptance detail.

> **Active implementation program.** A0 automated foundations and the browser
> Phone Party direct path exist; physical-device release evidence, native host,
> real-game rollback and online races remain gated. See the
> [live operational ledger](STATUS.md). This is the governing execution document for
> local phone controllers and private online multiplayer. The architecture
> rationale lives in
> [the online multiplayer proposal](../architecture/online-multiplayer.md); the
> ticket-level work lives in S10–S13 under [`../sprints/`](../sprints/README.md).

## Product promise

Golden Balloon will offer one coherent Party experience without teaching the
original game about QR codes, identities, room services or matchmaking:

- **Play here:** solo, ordinary couch play, or internet-assisted phone
  controllers. Keyboard, physical gamepads and display touch remain genuinely
  offline.
- **Online room:** two to four friends, each with a locally validated ROM, join
  a private room and race through deterministic rollback.
- **Guaranteed-$0 operation:** capacity and difficult networks are refused
  before an infrastructure charge can occur. Universal connectivity, public
  anonymous matching and IP-hiding relay are not promised in this profile.

Phone Party's one-scan browser path needs the rendezvous service to pair a new
phone. Once direct WebRTC is established, ordinary controller input survives a
signaling outage. Copy must say **Internet is needed to pair**; “offline phone
controllers” is not an allowed claim.

## Governing architecture

The launcher owns the multiplayer product. The engine is a deterministic race
runtime below one narrow, versioned session bridge.

| Launcher/session shell owns | Session bridge owns | Game and deterministic platform own |
|---|---|---|
| Play intent, Party scenes and navigation | Versioned manifest handoff | Kart/world simulation |
| Private admission and future matchmaker interface | Canonical tick input sets | Direct loading of selected race |
| QR, links, codes, guest identity and approval | Snapshot capture/restore boundary | Canonical racer slots and deterministic AI |
| Local source/seat management | Confirmed/predicted input history | Local cameras, HUD and presentation |
| Signaling, WebRTC, encrypted relay and retries | State-hash/event publication | Viewport/authority separation |
| Compatibility, update and connection UX | Side-effect commit boundary | Deterministic results |
| Privacy consent, diagnostics and aggregate telemetry | Result export and session policy | No sockets, JSON, room codes or identities |

`game/src` may be changed where deterministic authority, direct load, pause
policy, snapshotting or viewport separation require it. It must not include a
network/service header or render Party UI. A source gate enforces this boundary.

### One persistent session lifetime

The native and browser applications must both prove this lifecycle before any
production room service is built:

```text
HOME → LOCAL_SETUP / ONLINE_LOBBY → LOADING → RACING → RESULTS
                    ↑                            │          │
                    └──────── cancel/fail ───────┘          └─ rematch → LOADING
```

The launcher-owned `SessionRuntime` survives every engine state. Browser rematch
does not instantiate wasm again or invoke a second unsafe `callMain`; native
rematch does not drop the room while the engine unwinds. Transport callbacks
write bounded host-owned queues, and C drains them only at existing safe
boundaries.

Room phase, connection health, engine lifecycle and UI scene are orthogonal
state machines. A reconnect must not counterfeit a room-phase transition, and a
modal must not become the source of network truth.

## Delivery ladder

Each achievement is usable evidence. No later achievement can waive an earlier
gate.

| Achievement | Player-visible outcome | Required proof |
|---|---|---|
| A0 — Session foundation | Native/browser Party shell can enter a dummy race, return and rematch | One session id and transport fixture survive the round trip; online-safe overlay never pauses one peer |
| A1 — Browser Phone Party | Two then four phones scan, receive approved seats and drive local split-screen | Physical iOS/Android run, direct path, lifecycle neutralization and mixed inputs |
| A2 — Native Phone Party | Desktop app hosts the same controller page and protocol | Windows/macOS/Linux adapter evidence; no local server, certificate or firewall prompt |
| A3 — Rollback laboratory | Four isolated processes finish one race identically under impairment | Restore identity, viewport independence, bounded rollback and side-effect gates |
| A4 — Private online alpha | Invited friends create/join, race, return and rematch | Compatibility/update flow, partial-connectivity graph, reconnect and typed failures |
| A5 — Zero-cost private beta | Operated release survives quota, abuse, deploy and regional failure exercises | Seven-day bounded metrics, runbooks, kill switches, privacy review and $0 invariant |

Public quick match, ranked play, voice/public chat, spectator mode and mandatory
TURN/SFU remain outside A0–A5. The launcher exposes an inert `AdmissionProvider`
extension point so those features do not later require engine changes.

## Sprint order and concurrency

| Sprint | Outcome | Start rule | Finish rule |
|---|---|---|---|
| [S10](../sprints/S10-multiplayer-session-foundation.md) | Launcher-owned SessionRuntime and lifecycle proof | Immediate | All foundation stop/go gates pass |
| [S11](../sprints/S11-local-party-controllers.md) | Browser then native Phone Party | Start touch/pad work after S10 contracts freeze | A1 direct-path release; A2 follows without blocking rollback work |
| [S12](../sprints/S12-rollback-multiplayer-core.md) | Deterministic rollback laboratory | Start authority inventory after S10 bridge and S11 pad sample freeze | Written `GO` report |
| [S13](../sprints/S13-zero-cost-online-operations.md) | Private online alpha/beta | Prototype UI and pure reducers early; production races wait for S12 `GO` | A4/A5 gates pass |

Parallel work is allowed only at explicit seams. Service adapters cannot invent
an engine transition while S12 is unresolved; S12 does not wait for S11's
native transport finish once the pad contract is frozen.

## Work management and release cadence

The Markdown backlog is the stable acceptance specification; the issue tracker
mirrors each id and adds `Owner`, `State`, `Target achievement`, `PR` and
`Evidence` links. Ticket states are:

```text
PROPOSED → READY → IN PROGRESS → EVIDENCE REVIEW → DONE
                  └────────────── BLOCKED ──────────────┘
```

- `READY` requires the shared Definition of Ready below and one named owner.
- Keep per-engineer WIP to one implementation ticket plus one review/evidence
  ticket. Finish the lowest dependency-ready P0 before opening polish.
- A PR names one primary ticket, updates its fixtures/docs/evidence skeleton and
  preserves feature-off/local behavior. Cross-ticket refactors name every gate
  they can affect.
- Integrate behind separate `party_shell`, `phone_party`, `online_room` and
  `match_relay` capabilities. Service-side kill switches can refuse new work;
  they cannot remove **Play here** or invalidate the cached local build.
- Demo each milestone through its player journey, then review automation,
  physical-device evidence, threat/data-map delta, cost headroom and rollback.
  A screenshot or “two clients moved” is never the milestone artifact.
- Estimates are planning aids, not exit criteria. Failed evidence returns the
  ticket to `IN PROGRESS`; exceptions expire with an owner and may never waive a
  security, privacy, cost, determinism or local-availability invariant.

## Golden player journeys

### Play here

1. A ready ROM shows **Play here** and **Online room**. Local never waits for a
   service health check.
2. **Play here** opens four controller tiles. Existing sources appear
   immediately; **Add phones** opens a two-minute join window.
3. The display says **Scan to use this phone as a controller**, shows QR + code,
   and keeps a visible Close action.
4. The phone opens in its normal browser, shows the same 20-bit
   two-compound-word phrase, and
   waits for display approval. Name entry is optional and never blocks testing.
5. Approval assigns a controller number/color. **Press Go** on the phone lights
   the matching display tile and marks it Ready.
6. **Start game** launches ordinary local play. Tiles represent controller
   ports—not character identity—so the original character-select flow is not a
   confusing second “join.”
7. During local play, the shell overlay may pause and reopen controller setup at
   a safe menu/pause boundary.

### Online room

1. **Online room** offers **Create private room** and **Join room**. Join accepts
   a pasted role-specific link or six-character code.
2. Preflight checks build/protocol, ROM revision, cadence, gameplay digest,
   controller readiness, update state and connection feasibility before
   character selection.
3. The lobby groups seats by endpoint, shows calm quality words and keeps raw
   diagnostics behind **Connection details**.
4. Vote/load/countdown are explicit barriers with progress, cancel and exact
   recovery copy.
5. During a race, local input is immediate. The online shell overlay does not
   pause one endpoint; gameplay-affecting settings wait for results.
6. Results commit only after confirmed inputs and hash agreement. Rematch keeps
   the Party, rotates the match epoch and returns through the same shell scene.

Role-specific routes are mandatory:

- `/controller/#…` means “this device becomes a controller.”
- `/room/#…` means “this device runs the game and joins the room.”

## Interface and motion standard

The desired feeling is calm, direct and celebratory—not busy. These rules are
release contracts:

- Respond visually on pointer-down; commit ordinary actions on pointer-up with
  cancel-by-drag-away. Network acknowledgement never delays press feedback.
- Analog steering tracks the captured pointer 1:1 and respects where the thumb
  landed. Do not interpolate, debounce or animate the value sent to the game.
- Sheets originate from their trigger, remain interruptible and reverse along
  the same path. Use critically damped motion without decorative bounce; only a
  momentum gesture may carry a small overshoot.
- Status, completion, warning and error feedback are distinct. Every wait has a
  label, bounded timeout, Cancel and one next action.
- Preserve spatial mapping: controller tile/color/number matches the phone and,
  where the presentation hook exists, the in-game local marker.
- Reduced motion uses short cross-fades/static transitions. Reduced
  transparency uses solid surfaces. Increased contrast adds explicit borders
  and non-color status cues.
- System fonts, scalable type, safe-area insets, zoom, keyboard/gamepad
  navigation, visible focus and screen-reader live regions are required.
- The controller works at 320×568 CSS pixels, 200% text and either orientation.
  Orientation lock, Wake Lock, vibration and device motion are enhancements,
  never prerequisites.
- Real-hand acceptance covers Accelerate+Steer+Drift, Accelerate+Steer+Item and
  Brake+Steer. Target-size conformance alone cannot approve an action layout.

The complete component/state/copy catalog is maintained in
`docs/ux/multiplayer-patterns.md` when S10 begins. No platform may create a
second label or error taxonomy without updating that catalog and shared
fixtures.

## Security, privacy and cost invariants

These begin in S10; they are not a late audit:

1. A controller invite has 128 random secret bits, is short lived and
   multi-redeem only within its two-minute window so a group can scan one QR.
   Every redeem mints a distinct pending credential and still requires host
   approval. The invite is held in a URL fragment and removed before any
   network operation; rotation revokes it for every further redeem.
2. Controller and room invites have different scopes. A controller credential
   can never become a game endpoint or choose its own seat.
3. QR possession alone cannot inject input. The display approves a matching
   phrase and one current seat generation.
4. All untrusted lengths, types, versions, ticks, ports and state transitions
   are bounded and fail without partial mutation.
5. Direct channels use DTLS. WebSocket gameplay fallback adds application-layer
   authenticated encryption so the relay handles ciphertext only.
6. Direct online peers can learn network-address information; private-room copy
   discloses this. Public anonymous matching requires funded relay-only privacy.
7. No ROM, save, snapshot, PCM, framebuffer, SDP, input packet, capability or
   player name enters centralized logs.
8. Static controller resources ship locally with response CSP,
   `frame-ancestors 'none'`, no-referrer, nosniff and a restrictive
   Permissions Policy. No trackers, CDN scripts, fonts or embeds.
9. The free account has separate control, pairing and fallback reserves. Relay
   admission cannot consume the control-plane floor; provider exhaustion returns
   typed capacity errors and a $0 bill.
10. Local play remains visible and functional during DNS, Worker, Durable
    Object, relay, quota and deploy failure.

Every sprint carries a threat-model delta, abuse cases and deliberately broken
negative controls. A final security ticket verifies accumulated work; it never
introduces the first validation rule.

## Definition of Ready

A multiplayer ticket may enter implementation only when:

- dependencies and the owning layer are named;
- player journey, UI states, copy/error ids and accessibility behavior exist;
- protocol/schema inputs and bounded outputs are frozen or explicitly absent;
- security, privacy, cost and lifecycle effects are enumerated;
- the positive gate and at least one meaningful broken-direction control are
  named;
- physical-device/manual evidence is named where automation cannot prove feel;
- it can finish without an unresolved architectural decision.

## Definition of Done

A ticket is Done only when:

- code, fixtures and user/operator documentation land together;
- positive, negative, lifecycle, accessibility and privacy/cost gates pass;
- one intentional mutation demonstrates that the principal gate can fail;
- all waits/queues/histories/retries/log dimensions are bounded;
- browser and native behavior pass shared transition fixtures where both exist;
- new UI is exercised by keyboard, gamepad/touch as applicable and a screen
  reader model;
- measured latency, size or capacity replaces any planning estimate;
- no unsupported claim appears in player-facing copy.

## Decision and evidence records

Each sprint closes with a short evidence report containing commit/build ids,
device/browser/OS matrix, commands, measurements, failures, mutations and a
`GO`, `CONDITIONAL GO` or `STOP` decision. `CONDITIONAL GO` names an owner and
expiry; it is not a permanent waiver.

Required durable documents by A5:

```text
docs/ref/session-protocol-v1.md
docs/ref/party-protocol-v1.md
docs/ref/online-lobby-protocol-v1.md
docs/ref/match-launch-descriptor-v1.md
docs/ux/multiplayer-patterns.md
docs/security/multiplayer-threat-model.md
docs/privacy/multiplayer-data-map.md
docs/ops/multiplayer/{deploy,incident,privacy,capacity,rollback}.md
docs/evidence/multiplayer/{session,phone-party,rollback,online-beta}.md
```

Documentation uses the repository's standing form: mechanism → measured
evidence → fix → verification. Screenshots illustrate; state fixtures and
interaction tests remain the executable authority.
