# Online multiplayer — product and architecture proposal

> **Architecture record; implementation is gated and in progress.** Written
> 2026-08-11 and reconciled 2026-08-12. Session, Phone Party, rollback laboratory,
> shared room UX and local MatchRoom control foundations are implemented; a
> deployed carrier, written rollback `GO`, physical/human evidence and production
> provisioning are not claimed. Estimates remain planning ranges, not promises.
>
> **Operational authority:** [multiplayer delivery program](../multiplayer/README.md).
> **Implementation backlog:** [S10 session foundation](../sprints/S10-multiplayer-session-foundation.md)
> → [S11 Phone Party](../sprints/S11-local-party-controllers.md) →
> [S12 rollback core](../sprints/S12-rollback-multiplayer-core.md) →
> [S13 zero-cost online operations](../sprints/S13-zero-cost-online-operations.md).

## Recommendation in one page

Build **launcher-owned, deterministic rollback multiplayer** for private rooms
first. The launcher/session shell owns the Party lifecycle, room UX,
compatibility, networking and recovery; the game accepts only a versioned match
manifest and canonical input sets and emits deterministic events/results.
Every client continues to run the same fixed 30 Hz NTSC or 25 Hz PAL simulation
from its own locally validated ROM. A small cloud room service owns membership,
compatibility, the match manifest, start barriers, reconnect leases and the
ordered control log, but it never receives a ROM, save, savestate, track data or
game framebuffer.

Use two transports in the guaranteed-zero-cost profile:

- a WebSocket connection to one stateful room actor for reliable control and a
  capacity-bounded gameplay fallback;
- unordered, partially reliable WebRTC data channels forming an authenticated
  connectivity graph between endpoints. Inputs are tick-numbered, redundantly
  bundled, source-authenticated and deduplicated. A peer may forward one hop, so
  four players do not require all six direct pairings to succeed.

This is “serverless” operationally: there is no permanent game-server fleet and
no user machine is the match host. It is not “server-free”; rooms, abuse
protection and signaling are real service dependencies. Managed TURN is omitted
from the guaranteed-$0 profile because its free allowance has billable overage
and no hard spending cap. That makes universal NAT/firewall connectivity an
explicit tradeoff; a sponsored/BYO TURN configuration can restore it later.

Do **not** begin with public matchmaking, ranked results, campaign co-op, voice
chat, spectators or every battle mode. The first useful release is 2–4 friends,
one local seat per endpoint, same engine build and ROM revision, standard races,
room code/link/QR entry, graceful reconnect, deterministic desync detection and
excellent controller feel. Add mixed couch-and-online seats after the authority
versus viewport split is proven.

Public quick match is not merely postponed UX in the guaranteed-$0 profile:
direct peers may learn network-address information, while anonymous matching
needs relay-only privacy, moderation/abuse controls and funded capacity. The
launcher keeps a matchmaker adapter seam whose v1 response is `UNSUPPORTED`.

The primary technical stop/go question is not Cloudflare, WebRTC or bandwidth:
it is whether the game can restore a tick-exact local rollback snapshot and run
the same authoritative simulation while different clients present different
local viewports. Prove those locally before building a service.

## Why this fits Golden Balloon

The project already has unusually strong foundations:

- one C simulation runs in native and wasm builds;
- gameplay remains on an authored fixed tick while presentation may run faster;
- `MDKR_STATE_HASH=3` covers the current authoritative state and is already
  deterministic across repeat processes, GL/WebGPU and window sizes;
- controller input already enters through four `OSContPad` records;
- 2–4 player split-screen, per-controller assignment, controller hotplug,
  touch controls, browser offline caching and tab-safe local saves exist;
- the browser already validates and stores the ROM locally without uploading it.

It also has two architectural constraints that rule out the usual dedicated
server:

1. A real authoritative simulation needs ROM-derived track, collision, object
   and rules data. Hosting that data, or asking players to upload it, conflicts
   with the project's core custody and distribution policy.
2. The original game commonly treats human-player count, controller port,
   viewport count, camera selection and HUD work as one concept. Online clients
   must agree about four simulated racers while disagreeing, legitimately, about
   which one or two cameras to draw locally.

The room service can therefore be authoritative about **who may play, which
slot/key each endpoint owns, and when session transitions occur**, but not about
per-tick inputs or resulting kart positions. Endpoints authenticate slot/tick
inputs and derive gameplay integrity from deterministic agreement plus
cross-client state hashes. That is appropriate for friend rooms and casual
play, not a credible ranked anti-cheat system.

## What comparable systems teach

| System/pattern | What it demonstrates | Apply here | Avoid here |
|---|---|---|---|
| [GGPO rollback](https://github.com/pond3r/ggpo) | Predict local/remote input, execute immediately, and resimulate after late corrections so offline timing and muscle memory survive online. | Adopt the model and test discipline; use a small project-native C core around the existing fixed tick. | Dropping the old SDK in wholesale; its transport/platform assumptions and Windows-focused sample do not solve this engine's snapshots or wasm path. |
| [Photon topologies](https://doc.photonengine.com/fusion/v2/fusion-choose) | Dedicated authority, player-host authority and shared authority have fundamentally different fairness, cost and failure behavior; web clients are poor hosts. | Choose shared deterministic authority and a cloud room that survives any one player leaving. | A browser/native player as the invisible host; host migration cannot repair lost game state automatically. |
| [Cloudflare Durable Objects](https://developers.cloudflare.com/durable-objects/best-practices/rules-of-durable-objects/) | One single-threaded, strongly coordinated actor per room maps cleanly to membership and lobby state; hibernatable WebSockets keep idle lobbies cheap. | One object per room, persistent room phase/epoch, connection attachments, idempotent recovery after object restart. | Running the game loop or a per-frame timer in the object; active input should drive work and clients should own rollback history. |
| [WebRTC Data Channels](https://www.w3.org/TR/webrtc/) and [TURN](https://developers.cloudflare.com/realtime/turn/what-is-turn/) | Data channels support unordered/limited-retransmit delivery; TURN makes restrictive NAT/firewall cases connect reliably. | Fast path for small redundant input bundles; use direct ICE plus a capped WebSocket fallback at $0, and short-lived TURN credentials only in a funded/BYO profile. | Claiming universal connectivity without TURN, exposed long-lived TURN secrets, or treating unreliable delivery as a reliable control log. |
| [Steam Remote Play Together](https://partner.steamgames.com/doc/features/remoteplay?l=english) | Existing local multiplayer can become online quickly by streaming one host's video and accepting remote controllers. | Useful for private testing or an optional distribution integration if the project later ships on Steam. | The primary architecture: host departure ends the game, all players inherit stream latency/image loss, and browser/native cross-play is lost. |
| [Mario Kart 8 Deluxe rooms](https://en-americas-support.nintendo.com/app/answers/detail/a_id/29175) | Local seat selection, friends rooms, shared-console online players, character selection and track voting form a familiar kart-racing flow. | Make a connection a party containing one or more local seats; retain ready state and track voting. | Separate incompatible mental models for couch and online play, or hiding controller assignment until after joining. |
| [Jackbox room codes](https://support.jackboxgames.com/hc/en-us/articles/15794759479959-How-do-I-join-a-game) | A short code, display name and browser are enough to join a private group. | Code + deep link + QR, no account required for friend rooms. | Four-letter codes without rate limits/expiry at larger scale; use six unambiguous characters and server-side throttling. |

Managed deterministic engines such as Photon Quantum validate the broad model,
but moving this C source port into a Unity-specific simulation envelope would be
more invasive than building the narrow seams it needs. Managed dedicated-game
hosting likewise solves fleet operations, not the ROM-data problem.

## Product model: one Party system, explicit play modes

The core model is an **endpoint** (one running browser/native app) containing
one or more **local seats**. A room maps up to four seats from any endpoints to
four canonical racer slots.

| Mode | Internet | Endpoints | Local seats | First-release status |
|---|---:|---:|---:|---|
| Solo | No | 1 | 1 | Existing; must never regress |
| Couch play | No | 1 | 2–4 | Existing; keep as the fastest path |
| Phone Party | Needed to pair; direct after | 1 display + 1–4 phone controllers | 1–4 | First player-visible Party release |
| Nearby room | Signaling normally; gameplay can stay on LAN | 2–4 | 1–2 initially | After private online is stable |
| Online private room | Yes | 2–4 | 1 initially, then mixed | Initial online product |
| Quick match | Yes | 2–4 | 1 | Unsupported at $0; separately funded product |

“Nearby” must be honest. Native builds can eventually offer true offline LAN
discovery and signaling. Browsers cannot generally advertise/listen as a local
server, so browser nearby play should say **Internet needed to connect; game
traffic stays local when possible**. Offline browser couch play remains fully
available from the installed PWA.

### Scope of the first online release

Include:

- private 2–4-player standard races;
- one local seat per endpoint;
- browser/native cross-play only when the protocol, engine compatibility,
  supported ROM revision and simulation settings match exactly;
- guest identity, room code, share link and QR;
- character choice, track vote, short series, ready state and rematch;
- reconnect during the lobby and between races;
- mid-race deterministic AI takeover after a disconnected-player grace period;
- local input replay and state-hash diagnostics.

Explicitly defer:

- ranked ladders, prizes and anti-cheat claims;
- public text chat and voice, which introduce moderation, privacy and blocking
  work disproportionate to a four-player racer;
- quick-chat/emotes until the private race/recovery UX is stable;
- campaign co-op, progression writes, ghosts and time-trial records from online
  sessions;
- join-in-progress as a racer, mid-race control handback, and spectators;
- battles/challenges until their deterministic rollback matrix is independently
  green;
- cross-ROM-revision play, gameplay-changing mods and magic codes;
- true offline LAN in the browser.

## Player experience

### Entry

Once a ROM is ready, the launcher should present two equal, plain choices:

- **Play here** — solo, ordinary couch play or Phone Party; ordinary local
  controllers work offline.
- **Online room** — create/join a private friend room.

Do not put online configuration in the current presentation-settings block.
Online is a play intent, not a video option. The existing “No server” badge and
copy must become “ROM stays on this device” before online ships. On first online
use, one concise consent screen should name what leaves the device: display
name, app/protocol version, ROM revision identifier, input packets, network
quality and compact state hashes. It should name what never leaves: ROM bytes,
saves, savestates, mods and screenshots.

### Create/join flow

1. **Choose local players first.** “Press a button to join” assigns controllers
   and confirms each seat can steer, accelerate and pause. A missing controller
   is fixed before networking begins.
2. **Create or join.** Create returns a six-character unambiguous code,
   role-specific `/room/` link and QR. Join accepts pasted codes/links without
   forcing an account; `/controller/` links can never join an online room.
3. **Compatibility preflight.** The room checks protocol, build compatibility,
   US/EU revision, 30/25 Hz cadence, gameplay settings and mod digest. Errors
   identify the exact mismatch and the action that fixes it.
4. **Lobby.** Show seats grouped by device, controller readiness, character,
   “ready”, and a calm connection label (`Good`, `Variable`, `Poor`) backed by
   measured RTT/jitter. Keep raw ping one disclosure away.
5. **Vote and start.** Each seat votes; ties use the manifest's seeded choice.
   A three-second cancelable start barrier confirms every endpoint has loaded
   the same track and captured rollback frame zero.

The room has a **leader** for changing options, never a gameplay host. Leadership
passes automatically when that person leaves. Copy should not call this person
“host,” because that sets the wrong reliability expectation.

### During a race

- Render only local seats, at the same quality as couch play. Do not show four
  tiny viewports merely to make simulation layout easy.
- Keep the ordinary HUD. A small network mark appears only for sustained
  degradation; do not make a flickering ping number part of the racing HUD.
- Local input is applied immediately. Remote misprediction should correct
  through rollback without camera cuts, duplicated sound, doubled rumble or
  rewritten saves.
- If a player vanishes, show “Alex is reconnecting” and switch that racer to AI
  at one room-authorized tick after the grace period. In the first release,
  reconnection returns the player to the lobby/next race, not mid-race control.
- If coordination is unavailable but peer input paths are alive, keep the
  current roster and race running. If a roster transition becomes necessary
  and cannot be agreed safely, pause at a tick boundary with a specific
  reconnect message instead of allowing silent divergence.

### Results and recovery

Results are local/casual records. They do not write campaign progression, retail
time-trial records or globally ranked stats. A checksum disagreement invalidates
the result and says “This race lost sync; no local progress was changed.” The
diagnostic export contains build/protocol ids, timings, input-log hashes and the
first divergent tick—not state memory or ROM-derived bytes.

Leaving a room never deletes local saves. A room-service outage never hides the
offline play button. Those two invariants should be UI regression tests.

## Architecture

### Launcher/session boundary

One persistent `SessionRuntime` sits above engine entry/exit in native and above
one wasm instance in the browser. Its shared pure `MdkrSessionCore` owns the
orthogonal UI scene, engine lifecycle, room phase and connectivity states.
Platform views render immutable snapshots and dispatch commands; adapters
perform effects and acknowledge them with a generation. This is executable
shared behavior, not two view models kept “conceptually” in sync.

The browser preserves that boundary with `mdkr-online-tools.wasm`, a standalone
projection module built from the same C session core, room reducer, fake adapter
and view model as native. It deliberately excludes the ROM, renderer, engine,
storage, sockets and provider code; browser JavaScript supplies semantic DOM,
choice widgets and launcher-owned routes only. The publisher-owned
`online-control-config.js` ships disabled, so the launcher does not request this
module or any room endpoint. If a reviewed same-origin release enables room
control, the shell still waits for clean build provenance and a locally
full-SHA-verified supported ROM, then derives the bounded compatibility record
without retaining or transmitting ROM bytes. Gallery and live evidence adapters
correlate all 43 cases and 27 typed actions against the native executable
without creating a second JS reducer. The independent local race-admission bit
remains false until written A3 `GO`.

The engine boundary is deliberately narrow:

```text
launcher/session                    versioned bridge             game/runtime
Party scenes + navigation    ──▶  match manifest             ──▶ deterministic race
room/signaling/transports    ──▶  canonical tick inputs      ──▶ snapshot/resimulate
updates/retries/diagnostics  ◀──  events + result            ◀── authority hashes
```

Local Controls may pause. The online race overlay may not stop one endpoint's
authored clock; room-wide pause is unsupported until it has a deterministic
tick protocol. Browser results/rematch cannot reload or call the program entry
point again. These lifecycle proofs precede production networking.

```text
 browser/native endpoint A                         endpoint B/C/D
 ┌────────────────────────────┐                  ┌─────────────────────┐
 │ 1–2 local seats/controllers│                  │ local seats          │
 │ deterministic fixed tick   │◀── WebRTC ─────▶│ same simulation      │
 │ local rollback ring        │ direct; TURN opt.│ local rollback ring  │
 │ local ROM + saves          │                  │ local ROM + saves    │
 │ local-only presentation    │                  │ local presentation   │
 └──────────────┬─────────────┘                  └──────────┬──────────┘
                │ reliable WebSocket control + gameplay fallback
                ▼                                           ▼
       ┌──────────────────── edge Worker ─────────────────────┐
       │ auth/rate limits · room lookup · short-lived TURN    │
       └──────────────────────────┬────────────────────────────┘
                                  ▼
       ┌──────── one stateful room actor per room ────────────┐
       │ membership · epoch · manifest · ready/start barrier  │
       │ leader lease · control log · checksums · reconnect   │
       │ no ROM · no game simulation · no savestate           │
       └───────────────────────────────────────────────────────┘
```

### Cloud shape

A Cloudflare-first implementation is a strong prototype fit as of this proposal:

- **Worker:** HTTPS API, anonymous signed session tokens, rate limiting, room
  routing, optional funded/BYO TURN credential minting and sanitized telemetry.
- **SQLite-backed Durable Object per room:** strongly ordered lobby/control
  state and Hibernation WebSockets. Persist room epoch, phase, compatibility
  manifest, seats, leader lease and bounded control log. Treat constructor
  reruns and object moves as ordinary recovery paths.
- **Authenticated WebRTC graph:** attempt direct edges, then select a graph of
  diameter two or less for deterministic one-hop forwarding. A longer chain is
  not falsely called connected enough; it needs admitted fallback or refusal.
  Input bandwidth is tiny and inner source/epoch/sequence authentication makes
  forwarding distinct from authority. A funded/BYO profile can add managed
  TURN, and an SFU can replace the adapter without changing the game protocol.
- **Capacity-bounded WebSocket gameplay fallback:** send the same frame packets
  through the room object with recipient-specific application-layer encryption,
  deduplicate at the receiver, meter relay admission, and fail new allocations
  before the free-plan ceiling. Existing direct matches are unaffected.
- **Static game:** keep it independently cacheable/installable, so service
  health cannot regress local play.

Durable Objects are a coordination primitive, not a hard-real-time game host.
Their documented lifecycle permits in-memory state to be discarded and the
constructor to run again; all room transitions therefore need epochs,
idempotency and persisted intent. Hibernation is useful in idle lobbies and
during sparse-control portions of a peer-driven race. Do not persist or route
successful per-tick input on the ordinary path. Clients retain the complete
input log; the room stores only bounded control/health intent and requests a
bounded resend after recovery.

Cloudflare's current Realtime pricing is bandwidth-based and small game inputs
are inexpensive, but provider pricing and product maturity are not architecture
contracts. Keep `RoomService`, `SignalingTransport` and `TurnCredentialProvider`
interfaces narrow, and run a measured provider bake-off before production. A
second provider is not automatic high availability: it only helps after room
state/failover semantics are designed and chaos-tested.

### Guaranteed-zero-cost operating profile

Zero recurring infrastructure cost is possible for a private-room community,
provided **$0 means a hard ceiling with graceful capacity loss**, not unlimited
service and not universal network compatibility.

As of 2026-08-11, Cloudflare's
[Workers Free plan](https://developers.cloudflare.com/workers/platform/pricing/)
includes SQLite-backed Durable Objects, 100,000 Worker requests/day, 100,000
Durable Object requests/day (incoming WebSocket messages receive a 20:1 billing
ratio), 13,000 GB-s/day of Durable Object duration, 100,000 SQLite writes/day
and 5 GB stored data. Free-plan overages fail rather than becoming paid usage.
The existing static site remains on free static hosting.

Use that allowance only for:

- room creation/join, signaling descriptions and compatibility manifests;
- lobby/control transitions and leader/reconnect leases;
- sparse health/checksum disagreement reports, not per-tick success traffic;
- a deliberately small emergency WebSocket-relay pool.

Once peers connect, ordinary input, acknowledgements and periodic matching
checksums travel over their WebRTC graph. The room actor can hibernate between
control events while its WebSockets remain attached. A match that has a healthy
peer graph therefore consumes almost no per-tick infrastructure quota.

Cloudflare Realtime currently includes
[1,000 GB/month](https://developers.cloudflare.com/realtime/sfu/pricing/) before
TURN/SFU egress charges, but that is not a guaranteed-$0 control: Cloudflare's
[budget alerts are informational and do not cap usage](https://developers.cloudflare.com/billing/manage/budget-alerts/).
The zero-cost production account should therefore not enable metered TURN/SFU.
Offer three honest outcomes when direct ICE fails:

1. admit the room to the free WebSocket relay if its daily capacity remains;
2. accept a user/community-provided TURN endpoint with short-lived credentials;
3. say the networks cannot connect directly and preserve local play.

Do not use anonymous public TURN servers or volunteer peers as opaque relays.
They provide no dependable capacity, privacy owner or abuse response. Peer
forwarding inside the same four-player room may be explored, but it makes a
player's uplink part of others' path and must not be advertised as equivalent to
managed TURN.

The free deployment needs an application-side admission threshold below the
provider maximum, a signed remote kill switch cached with the static app, and a
clear “Online capacity is full; local play still works” state. Do not attach a
paid Workers plan or other uncapped usage product to the production account.

### Session manifest

The room signs one immutable manifest before loading the race:

```text
protocol version             room id + epoch
minimum/maximum client build canonical player slots and endpoint/seat mapping
ROM revision id              simulation cadence and gameplay-settings digest
mod/content digest           selected mode, track, rules and series position
RNG seed                     input delay, rollback window and start tick/time
```

For the alpha, require the exact release provenance commit and no gameplay mods.
Later, a deliberately versioned compatibility id may permit builds proven to
produce identical state. Presentation mode, resolution, renderer, FOV,
supersampling, smoothing, language and accessibility choices stay local unless
a test proves one is still accidentally authoritative.

### Input protocol

At each fixed tick, an endpoint samples every local seat into the existing
controller shape: button bitset, signed stick X/Y and presence. It sends input
for tick `T + D`, where `D` is the room's input-delay policy. A small binary
production envelope should include:

```text
protocol/room epoch · endpoint and sequence · newest tick
one fixed input bundle plus transport-level acknowledgement fields
received-tick acknowledgements · last confirmed simulation tick
periodic state-hash version/value · flags
```

Keep packets comfortably below the path MTU. The implemented laboratory bundle
is exactly 64 bytes: version, epoch, newest tick, authenticated-subset slot mask,
one to three implicit consecutive four-slot frames, neutral padding and
CRC-16/CCITT. Frames decode fail-atomically and enter transport oldest-first.
The authenticated peer/slot binding remains outside the payload. The deployed
envelope still needs endpoint sequence, acknowledgements and channel
authentication; do not mistake the laboratory codec for that handshake. Send
each new input on the fast lane and safety lane initially; after measurement,
the safety copy may batch without changing the inner bundle. Duplicate,
reordered and stale-epoch packets are harmless. Reliable lobby messages and
unreliable input messages are separate channels and schemas.

Default prediction is the last complete controller state. Inputs are corrections,
not RPCs (“use item”), so a rollback cannot execute a one-shot action twice.
Start with a 2–3 tick delay on good regional links and tune only from measured
correction/stall distributions. The implemented real-game ring holds 32
snapshots: a correction may replay at most 31 ticks, while the oldest accepted
input has authored age 30 because the preceding clean boundary is also needed.
An aged contiguous-input gap or older late input latches one typed recovery,
cooperatively unwinds the engine and returns to the launcher; it never restores
evicted memory or performs unbounded catch-up. A future product may choose a
smaller admission window from device budgets without changing this safety cap.

### Rollback snapshots and side effects

`MDKR_STATE_HASH=3` is a detector, not a savestate. Add a local-only rollback
snapshot contract:

- copy the authoritative arena ranges and allocator metadata while keeping
  addresses stable within that process;
- explicitly register authoritative globals and sidecars outside the arena;
- include RNG, controller edge state, gameplay clocks, object pools and the
  canonical roster;
- exclude GPU objects, display lists, presentation snapshots, wall clock,
  sockets, filesystem handles and device audio queues;
- never serialize or transmit the snapshot to another endpoint.

Restoring snapshot `T`, replaying confirmed inputs through `N`, and hashing `N`
must exactly equal a clean uninterrupted run. Raw snapshots need not be portable
between wasm/native because they never cross machines.

Rollback resimulation must run in a side-effect-suppressed mode. Save writes,
achievements, filesystem sync, telemetry, rumble and device audio are committed
only once when a tick becomes irreversible. Gameplay audio/visual events receive
stable `(tick, emitter, ordinal)` ids so presentation can suppress duplicates
or reconcile a predicted event. Camera correction is presentation-only; never
snap the camera to a remote correction as if it were kart authority.

### The simulation/viewport separation

This is the first major engine change. Introduce three distinct concepts:

- `canonical_player_count` and canonical racer slots: identical everywhere;
- `local_seat_count` and `local_seat -> canonical_slot`: endpoint-local input;
- `local_viewport_count` and `viewport -> canonical_slot`: presentation only.

Remote input is injected into the appropriate canonical `OSContPad` before the
ordinary `input_update()` edge calculation. The game must simulate the same
human racers and player ordering on every endpoint even when A draws slot 0 full
screen and B draws slot 1 full screen.

Audit every authoritative read of `gNumberOfActivePlayers`,
`cam_get_viewport_layout()`, `get_current_viewport()`, player indices and HUD
loops. The repository has already moved several particles, visibility and HUD
decisions out of render passes; extend that boundary rather than maintaining
network-only forks. A new gate must run one canonical race under 1P, 2P and
headless/no-presentation layouts and require the same v3 stream after excluding
only explicitly presentation-local camera state. If that requires weakening
the hash broadly, stop: the separation has not been achieved.

Menus need not be synchronized initially. The app/browser lobby can select a
manifest and enter a production direct-load seam, then return to the room after
results. Networking the original frontend multiplies rollback surface without
improving the first race.

### Reconnect and late state

Clients retain the manifest and compressed input log for the current series.
On a brief transport reconnect they present the room epoch, seat lease and last
confirmed tick, then receive missing input/control ranges. A still-running local
client can roll forward from its ring.

For a process/tab restart, do not transfer memory snapshots: restart the track
locally and fast-forward the canonical input log without rendering/audio. Measure
whether that reaches the live tick within the reconnect budget. If it cannot,
the first release reconnects the user to the lobby and leaves the racer under AI
control. A spectator/late-join product can be added only after fast-forward is
bounded across every supported track.

## Reliability contract

“Hyper reliable” should mean explicit degradation and measured recovery, not a
claim that networks or providers never fail.

| Failure | Required behavior |
|---|---|
| Room service unavailable before play | Local/solo/couch remain visible and work offline; online says when retry will occur. |
| WebRTC fails or is blocked | Admit the room to the bounded free WebSocket relay when capacity remains; otherwise explain that these networks cannot connect directly. Never risk a bill silently. |
| WebSocket/control reconnects | Peer input path continues the fixed roster; control messages resume by room epoch and sequence. |
| One player disconnects | Grace period, then one room-authorized AI-takeover tick; remaining players and leader continue. |
| Browser tab backgrounds | Treat as a disconnect candidate, never as a host pause. Online mode must qualify continued ticking or disconnect explicitly; local hidden-document suspension remains unchanged. |
| Room actor restarts/deploys | Rebuild from persisted manifest/control state, recover WebSocket attachments where available, ask clients for bounded tail, reject stale epochs. |
| Duplicate/reordered/lost packets | Frame ids, redundant input windows, acknowledgements and dedupe produce the same canonical stream. |
| State hashes diverge | Stop trusting results, capture sanitized first-divergence evidence, never choose a winner by majority in a 1v1. |
| New client version appears mid-room | Existing room remains pinned to its protocol/build policy; no mixed rolling upgrade. |
| Entire provider/control plane fails mid-race | Continue only while the fixed roster and peer inputs require no transition; otherwise freeze at a confirmed tick with a truthful message. |

Suggested beta service objectives, revised after real load tests:

- 99.5% of compatible private-room attempts on the qualified direct-connect
  network profile reach the start barrier; report the broader real-world rate
  separately rather than hiding the no-TURN population;
- 99.9% of started races finish without an infrastructure-caused abort;
- no silent desyncs: 100% of detected hash disagreement invalidates results;
- p95 reconnect under 5 seconds for a 10-second induced link interruption;
- p95 rollback depth no more than 3 ticks on the qualified regional profile,
  p99 no more than 6; zero resimulation beyond the configured bound;
- zero ROM/save/snapshot bytes in backend request and telemetry corpus scans;
- local launcher/play availability independent of backend availability.

Track match start success, transport chosen, RTT/jitter/loss, rollback depth,
prediction corrections, confirmed-tick lag, disconnect reason, reconnect time,
first divergent tick and room-actor recovery. Use rotating anonymous identifiers;
do not collect controller contents beyond aggregate packet statistics unless the
player explicitly exports a local diagnostic.

## Security, privacy and fairness

- Keep TURN/API secrets server-side; mint short-lived, room-scoped credentials.
- Use TLS/DTLS, strict binary length/version validation, per-room rate and size
  budgets, replay-protected epochs and unguessable internal room ids. The short
  display code is only a lookup capability and expires.
- Sanitize display names and prefer a curated quick-chat set. Provide mute,
  block and report before adding public matchmaking.
- Do not reveal peer IP addresses in public matching; use relay-only ICE there.
  Private friends may opt into direct paths after clear disclosure.
- Never upload a ROM, full SHA as a content lookup, save, mod payload, savestate,
  PCM or framebuffer. Send a supported revision enum and compatibility digest.
- Scan server logs/telemetry in CI with the existing asset-free principles.
- Treat all casual results as client-attested. Hash agreement proves clients
  stayed synchronized with one another, not that an altered client played
  honestly. Ranked competition requires a separately reviewed asset-legal
  authority model and is outside this architecture.

## Delivery plan and gates

The implementation authority is the
[multiplayer delivery program](../multiplayer/README.md), with ticket-level
dependencies and acceptance in S10–S13. Its achievement ladder is deliberately
front-loaded:

1. **A0 Session foundation:** launcher boundary, shared state core, browser and
   native race/results/rematch lifecycle, online-safe overlay.
2. **A1 Browser Phone Party:** secure role-specific QR pairing, approval and
   real iOS/Android controller feel for local split-screen.
3. **A2 Native Phone Party:** same page/protocol through a native WebRTC adapter,
   with no local server, certificate or firewall setup.
4. **A3 Rollback laboratory:** restore identity, viewport independence, bounded
   correction and cross-platform strict-determinism evidence.
5. **A4 Private online alpha:** exact compatibility, connected partial graph,
   result/rematch lifecycle, reconnect and honest typed failures.
6. **A5 Zero-cost private beta:** abuse/quota/deploy drills, bounded SLO evidence,
   kill switches, privacy review and runbooks.

No production online race opens before A3 says `GO`. Pure reducer fixtures and
fake-data launcher UX may proceed earlier against S10 seams. If restore identity
or viewport independence cannot close without a broad rewrite/exclusion, stop
and re-scope; do not ship delay lockstep under an excellent-gameplay claim.

For one experienced engineer, private-room production quality is plausibly
6–12 engineer-months including tests and UX; the gated backlog deliberately
uses measured evidence rather than turning that range into sprint promises.

## Required verification matrix

Every multiplayer change should add the failing direction, not just a happy
route:

- ROM-free units: codec round trips, packet limits, sequence wrap, ack windows,
  room reducer/idempotency and fuzz corpus;
- real-ROM local: restore/replay identity, all-racer determinism, race finish,
  post-race teardown, AI takeover and no save writes;
- presentation: 0/1/2 local viewports, GL/WebGPU/browser, smoothing off/on,
  Original/PAL cadence, window resize/fullscreen and state-hash invariance;
- network: loss/jitter/reorder/duplicate/outage profiles with fixed seeds and
  expected rollback bounds;
- lifecycle: background/foreground, reload, suspend/resume, controller hotplug,
  peer exit, leader exit, room restart and deployment version skew;
- privacy: backend capture asserts no 12 MiB payload, save signature, snapshot
  marker, asset path or external ROM request; logs are scanned too;
- accessibility/UX: keyboard and controller complete flows, visible focus,
  reduced motion, status not color-only, readable local split screens, and every
  error naming a recovery action;
- physical acceptance: two controllers on one endpoint plus two remote
  endpoints, restrictive NAT/TURN, poor Wi-Fi and at least one real cross-OS
  room.

## Decisions to revisit after A3/A4 evidence

Defaults are proposed so implementation can progress, but these remain measured
decisions:

- direct/one-hop WebRTC graph versus managed SFU at four endpoints;
- direct ICE allowed by default in private rooms versus relay-only privacy;
- exact input delay/rollback window per 30 and 25 Hz simulations;
- whether fast-forward can support process-restart rejoin inside the target;
- whether two local seats per online endpoint can ship with the first beta;
- whether US 1.1 and EU 1.1 need separate queues or one room-code namespace;
- backend vendor after latency, lifecycle, cost and outage drills.

The non-negotiable decisions are: local play never depends on cloud health; no
ROM-derived payload reaches the service; no player is a hidden gameplay host;
online results do not mutate trusted local progression; and detected divergence
is surfaced rather than papered over.
