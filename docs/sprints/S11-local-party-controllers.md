# S11 — Local Party: phones as zero-install split-screen controllers

> **Browser and native direct paths implemented locally; release evidence incomplete.** Secure
> QR/code pairing, approval, WebRTC and wasm pad handoff pass automated gates.
> Same-peer ICE restart, reliable-channel fail-neutral recovery and continued
> direct input through signaling loss/recovery are automated locally.
> Mixed-source assignment, in-game setup, four independent wasm ports and the
> persistent native launcher adapter pass; four-phone physical-race and
> packaged Windows/macOS/Linux device acceptance remain open in
> the [operational ledger](../multiplayer/STATUS.md).
> This is the first player-visible multiplayer delivery slice.
> It is intentionally independent of rollback and online races: one browser
> display runs today's ordinary local 2–4 player game while phones supply virtual
> controller ports.

**Goal:** A player opens Golden Balloon on a laptop/TV, chooses **Add phone
controllers**, and shows a QR. Friends scan it with the phone's ordinary camera,
open a small HTTPS controller page without installing an app or providing a ROM,
receive a seat, and play split-screen with responsive analog steering. Physical
gamepads, keyboard, the display's existing touch overlay and phones can be mixed
without two sources owning one port.

**Release promise:** “Scan. Pick up your phone. Race.” **Internet is required to
pair a new phone**; keyboard, physical gamepads and display touch remain the
fully offline local path. The controller page never
loads WebGPU, wasm, ROM code or game assets. Once paired, ordinary play uses a
direct WebRTC DataChannel; the free room service is signaling/control only.

**Depends on:** S10 SF-01/04/09 contracts. Production SessionRuntime/service
integration requires S10 foundation `GO`. **Unblocks:** S12's canonical
pad/input protocol and S13's room/signaling service.

## Execution rules

- Work tickets in dependency order. A ticket is `Done` only after its named
  negative control has been observed failing and the fixed arm passes.
- Every engine/game invocation uses a bounded `--headless-frames N`; automated
  runs set `MDKR_AUDIO=0`.
- Render Party state through S10 `MdkrSessionCore`. DOM/ImGui views dispatch
  commands and never own a second room/seat state machine.
- Deploy the dependency-minimal controller document and its API on one dedicated
  Party origin. Its HTML, CSS, JavaScript and icons are same-origin; no CDN
  scripts, trackers, fonts, analytics or third-party embeds.
- No ROM, save, snapshot, PCM, framebuffer or game asset may enter a controller
  request, cache or log. Extend the existing public/asset-free gates.
- Network input is untrusted. Bounds-check before mutation; unknown versions,
  types, ports, lengths and sequence windows fail closed.
- Hidden, disconnected, expired or overflowed controllers become neutral. A
  stuck accelerator is always worse than a dropped input.
- QR scanning's golden path is the phone's system camera. Do not depend on the
  experimental, non-Baseline `BarcodeDetector` API. An in-page scanner is a
  later enhancement, never a prerequisite.
- Prefer the existing Pointer Events + pointer-capture design and extract it;
  do not maintain two subtly different touch engines. The standard explicitly
  provides `touch-action` and pointer capture for this class of control.
- Visual press response begins on pointer-down and never waits for the network.
  Analog motion is 1:1 and unanimated. Sheets are interruptible; reduced motion,
  transparency and increased-contrast variants are first-class fixtures.
- Add third-party code only when a maintained platform API is insufficient;
  pin it, license it in `THIRD_PARTY.md`/`NOTICE.md`, and ship it locally.

## Product contract

### Display journey

1. A valid ROM exposes **Play here** — “Solo, couch, or use phones” — separately
   from **Online room**. Local never waits for a service probe.
2. **Play here** shows four controller tiles. **Add phones** opens a two-minute
   join sheet with a high-contrast QR, six-digit fallback code, short
   privacy sentence and visible Close/Extend actions.
3. A phone appears as **Waiting for approval** with the same 20-bit
   two-compound-word pairing
   phrase on phone and display. The display player approves and assigns a free
   seat; QR possession alone cannot inject input.
4. Each occupied tile shows controller number/color, source (`Keyboard`,
   controller name, `This screen`, or `Phone`), connection state and a live
   **Press Go** test. Name entry is optional and never blocks readiness.
5. **Start local game** becomes available once at least one seat is ready.
   Additional phones may join from the F1/Controls overlay at a safe game/menu
   boundary.

### Phone journey

| State | Required UI |
|---|---|
| Opening | Golden Balloon wordmark, “Joining controller room…”, cancel link |
| Unsupported/embedded browser | Explain the direct-connection requirement; share/copy the private link when held, otherwise share/copy `/controller/` and require the current code in Safari/Chrome |
| Update required | Required/current controller protocol and **Refresh controller** |
| Awaiting approval | Pairing phrase, “The display must approve this phone”; optional device name is secondary |
| Assigned | Large controller number/color, one-step **Press Go** input test, **Use controller** |
| Controller | Analog stick; Accelerate, Brake, Drift, Item, Look and Start; connection mark; seat label; settings |
| Duplicate tab | “This controller is already open in another tab”; leave/reclaim actions, never two active publishers |
| Reconnecting | Controls neutralized, visible retry progress, leave button; no stale interactive appearance |
| Rejected/expired/full | Exact reason and **Enter another code**; never an infinite spinner |

Controller settings are local to that phone: left/right-handed layout, button
scale, stick size/dead zone, labels, haptics and keep-screen-awake. Portrait and
landscape are both supported; orientation is recommended, not forcibly locked.
Safe-area insets, 44 CSS-pixel preferred action targets, high contrast and
reduced motion are release requirements. Real-hand acceptance must prove the
Accelerate+Steer+Drift, Accelerate+Steer+Item and Brake+Steer chords. Minimum
target size alone cannot approve the layout.

Press feedback is immediate and causal: highlight on pointer-down, commit an
ordinary button edge on the corresponding pointer transition, and allow
cancel-by-drag-away with a forgiving re-entry region. The analog stick stays
under the captured pointer without smoothing or decorative spring motion.
Launcher sheets use restrained, critically damped motion from their trigger and
remain reversible while moving; controller input itself is never animated.

### Pairing and QR contract

- QR payload: `https://<static-origin>/controller/#<base64url-capability>`.
  The payload combines a 128-bit room id and 128 random secret bits in the URL
  fragment, so it is absent from
  the initial HTTP request and referrer logs. Controller JavaScript copies it
  into closure memory and immediately removes the fragment with
  `history.replaceState` before configuration access, browser probes or network
  work. The route is exact and query-free; a failed scrub clean-navigates to
  `/controller/` and abandons redemption. Embedded-browser **Copy private link** and duplicate-tab
  reclaim reconstruct the private URL only for the explicit user gesture and
  the destination page scrubs it again; no capability enters web storage.
  Production entry and minted links require one canonical HTTPS origin (HTTP is
  loopback-only). Duplicate takeover first broadcasts neutral/close to the old
  tab and takes its exclusive lock without force-stealing; Web Lock rejection
  fails closed. A no-Web-Locks fallback stores only a 96-bit capability digest
  prefix, random tab id and 15-second heartbeat expiry. The new pending phone
  still requires visible host removal/replacement and approval.
- The controller posts the capability over HTTPS to redeem a short-lived,
  controller-specific pending credential. Friends can scan the current invite
  during the same two-minute window, but every phone receives an independent
  credential and requires explicit host approval. The room stores only keyed
  digests, not bearer values.
- Invite expires after two minutes, when the sheet closes, or when the room
  ends. **Extend two minutes** rotates the capability and QR. A room can have at
  most eight pending devices and four approved controllers.
- Render a standard black-on-white QR with four-module quiet zone, no logo or
  styling, and error correction Q or H. Show the fallback code as text.
- Vendor the TypeScript/JavaScript and C++ ports of
  [Nayuki's MIT QR generator](https://github.com/nayuki/QR-Code-generator) for
  generation only. A pinned test-only decoder proves the rendered symbol
  round-trips; it is not shipped to phones.
- Serve the static controller from the dedicated Party origin with response
  headers: deny-by-default CSP, `frame-ancestors 'none'`, no-referrer, nosniff,
  restrictive Permissions Policy and HSTS on the custom domain. Static fetch
  directives remain `'self'`; API/WebSocket connections are same-origin. Test
  that static routes do not invoke metered Worker code. Credential responses are
  `Cache-Control: no-store`.

## Technical architecture

```text
 phone controller page                         launcher-owned SessionRuntime
 ┌────────────────────────┐                   ┌──────────────────────────┐
 │ shared TouchSurface    │ -- pad-state --> │ ControllerHost           │
 │ state + edge history   │    WebRTC         │ RemotePadBridge[4]       │
 │ lifecycle neutralizer  │ <-- control ----- │ PadRouter -> port 0..3   │
 │ optional vibration     │                   │ SessionCore + Party UI   │
 └───────────┬────────────┘                   └────────────┬─────────────┘
             │ HTTPS/WebSocket signaling                  │ ordinary game pads
             └──────────── free PartyRoom DO ─────────────┘
```

Two WebRTC channels have distinct semantics:

- `mdkr-pad-state-v1`: unordered, `maxRetransmits: 0`; compact current-state
  packet plus recent transitions. New state supersedes queued old state.
- `mdkr-pad-control-v1`: reliable/ordered; hello, capabilities, seat lease,
  ping/pong, input-test result, rumble request, graceful leave and errors.

The controller sends immediately on a state transition and repeats the newest
state every 50 ms while active. The host expires a source to neutral after 250
ms without a valid packet. If `bufferedAmount` crosses the small-state ceiling,
drop stale state frames and keep only the newest; never build latency in the
browser's send queue. WebRTC exposes `bufferedAmount` specifically for this
backpressure decision.

### Binary pad-state v1

All integers are network byte order; exact sizes are compile/test locked.

| Field | Type | Rule |
|---|---:|---|
| magic | `u8 × 2` | fixed ASCII `GB` |
| version/type | `u8 × 2` | v1, STATE |
| flags | `u8` | PRESENT, NEUTRAL, HAS_EDGES only |
| connection sequence | `u32` | monotonic; new connection gets a new epoch |
| sample sequence | `u32` | monotonic modulo-safe comparison |
| sender monotonic time | `u32` | milliseconds modulo 2^32; diagnostics only |
| buttons | `u16` | only legal N64 pad bits survive decode |
| stick X/Y | `i8 × 2` | clamp to authored `[-80, 80]` |
| edge count | `u8` | 0–8 |
| edge records | bounded | delta sequence + complete buttons/X/Y state |
| checksum | `u16` | detects malformed/corrupt app payload, not security |

The DataChannel is already DTLS protected. Room/connection identity is bound at
channel setup and is not repeated in every state packet. The reliable control
hello proves the pad protocol and connection epoch before state is accepted.

### Pad ownership

Introduce one source router instead of OR-ing inputs together:

```c
typedef enum MdkrPadSourceKind {
    MDKR_PAD_NONE, MDKR_PAD_KEYBOARD, MDKR_PAD_SDL,
    MDKR_PAD_LOCAL_TOUCH, MDKR_PAD_REMOTE_PHONE, MDKR_PAD_SCRIPT
} MdkrPadSourceKind;

bool mdkr_pad_router_claim(int port, MdkrPadSourceKind kind, uint64_t owner);
void mdkr_pad_router_release(MdkrPadSourceKind kind, uint64_t owner);
bool mdkr_pad_router_sample(int port, MdkrPadSample *out);
```

One port has one owner. Claims are host-approved and generation checked. Release
publishes neutral before making the port free. Preserve current defaults:
keyboard may own P1, physical pads take lowest unclaimed ports, scripted tests
claim their explicit ports, and the display touch overlay may own P1 only when
selected. A physical-device reconnect never silently evicts a phone.

## Milestones and release gates

| Milestone | Shippable outcome | Exit gate |
|---|---|---|
| M0 Pad ownership | Every local source has explicit port custody | Existing keyboard/gamepad/script/touch gates unchanged; collision control rejected |
| M1 Controller surface | Standalone no-network controller works against a mock host | Multi-touch/edge/lifecycle unit + real mobile layout capture |
| M2 QR pairing | Scan → approve → assigned seat through free signaling | QR decode, expiry/replay/rate-limit controls, no secret in request URL |
| M3 Browser host | Up to four phones drive ordinary local split-screen | 1P–4P scripted browser race; 20 ms tap preserved; stale pad neutralized |
| M4 Product finish | Mixed sources, reconnect, haptics, overlay add/remove | Physical iOS/Android/display matrix and latency budget |
| M5 Native host | Native launcher can host the same phones | Same protocol vectors and two native OS device routes |

M0–M4 close program achievement A1 and may release as the browser-host proof.
M5 is the immediately following A2 achievement—not an indefinite enhancement—and
may proceed while S12 rollback work uses the already frozen pad contracts.

## Backlog

| ID | Pri | Size | Depends | Deliverable |
|---|---|---:|---|---|
| LC-00 | P0 | S | S10 SF-01,09 | Frozen v1 contracts, fixtures and threat model |
| LC-01 | P0 | M | LC-00 | Pure C pad router and source custody |
| LC-02 | P0 | M | LC-00 | Binary remote-pad codec/history/timeout |
| LC-03 | P0 | M | — | Extracted reusable TouchSurface |
| LC-04 | P0 | L | LC-03 | Static controller page, interaction patterns and mock transport |
| LC-05 | P0 | L | LC-00 | Free Worker/Room DO pairing and signaling |
| LC-06 | P0 | M | LC-05 | Host party sheet, QR, approval and seats |
| LC-07 | P0 | L | LC-02,04,05 | WebRTC state/control channels and reconnect |
| LC-08 | P0 | L | LC-01,02,07 | Browser host → wasm four-port bridge |
| LC-09 | P0 | M | LC-06,08 | Mixed-source readiness and input test UX |
| LC-10 | P0 | M | LC-07,08 | Lifecycle neutralization and congestion safety |
| LC-11 | P1 | S | LC-07,08 | Game rumble → optional phone vibration |
| LC-12 | P1 | L | LC-05,07 | Encrypted, hard-capped WebSocket gameplay fallback |
| LC-13D | P0 | L | LC-01..10 | Direct-path browser E2E, privacy, mutation and load gates |
| LC-13R | P1 | M | LC-12 | Relay-only E2E, encryption and capacity gates |
| LC-14 | P0 | XL | LC-01,02,05,07, S10 | Native WebRTC host adapter |
| LC-15 | P0 | M | LC-13D | Browser device acceptance, docs and staged A1 rollout |
| LC-16 | P0 | M | LC-14 | Native platform acceptance and staged A2 rollout |
| LC-17 | P1 | M | LC-13D | Remembered controller, floating stick and optional tilt |

### LC-00 — Freeze contracts before implementation

**Create:** `platform/party/party_protocol.h`, `docs/ref/party-protocol-v1.md`,
`tests/fixtures/party/*.bin`, `services/party/README.md`.

- Write golden valid/invalid packets in C and TypeScript; both decoders consume
  the same byte fixtures.
- Define room phases, capability expiry, approval, seat lease, reconnect epoch,
  close reasons and allowed transitions as data tables.
- Threat-model QR screenshot reuse, guessed codes, pending-device flood, stale
  packet replay, forged ports, oversized SDP, logging leakage and controller
  disappearance while holding buttons.
- Pin privacy retention: room metadata expires within 24 hours; active
  credentials at room close; no input packet logging.

**Negative control:** flip every length/version/type field in the corpus and
require both decoders to reject without partial output mutation.

### LC-01 — One pad source router

**Create:** `platform/pad_router.h/.c`, `tests/test_pad_router.c`.
**Modify:** `platform/platform_sdl_min.c`, `platform/platform_os.h`,
`cmake/tests.cmake`.

- Move keyboard, SDL, local touch and scripted sampling behind `MdkrPadSource`.
- Claim/release ports atomically; owner generation prevents stale releases.
- Publish neutral on release, overflow and failed sample.
- Keep `platform_pad_present/buttons/stick/rumble` as the stable game-facing
  interface so `platform/stubs_dkr.c` does not learn about remote phones.
- Prove current input traces byte-identical for every existing source arm.

**Negative control:** let two sources claim P1; the test must fail on ownership,
not merely observe combined buttons later.

### LC-02 — Remote pad core

**Create:** `platform/party/remote_pad.h/.c`,
`platform/party/party_protocol.c`, `tests/test_remote_pad.c`,
`tests/test_party_protocol.c`.

- Pure codec: no sockets, browser, SDL, allocation or wall clock.
- Bounded eight-transition reorder/dedupe window with modulo-safe sequences.
- Preserve a press+release occurring between authored ticks exactly once.
- Timeout accepts an injected monotonic time and emits one neutral transition.
- Track malformed, duplicate, stale, overflow and timeout counters.

**Negative controls:** remove release-edge history, disable timeout and accept a
stale connection epoch; each arm must fail a separate assertion.

### LC-03 — Extract the existing touch engine

**Create:** `dist/web/input/touch-surface.js`,
`dist/web/input/touch-surface.css`, `tests/web/touch-surface.test.js`.
**Modify:** `dist/web/mdkr64-shell.js`, `dist/web/index.html`,
`dist/web/style.css`, `tests/check_touch_controls.py`.

- Extract pointer capture, analog geometry, multi-button sliding, edge queue,
  safe release and layout policy without changing the current overlay behavior.
- Dependency-inject the publisher and clock; no global `Module` dependency.
- Keep the 128-edge overflow-to-neutral behavior and all blur/pagehide/
  visibility/fullscreen guards.
- Add handedness and size as pure layout inputs; existing display overlay keeps
  its current defaults.

**Gate:** the existing browser touch route remains byte-identical in consumed
input; the component test drives two simultaneous pointers and cancellation.

### LC-04 — Static controller page

**Create:** controller source under `dist/web/controller/`, the dedicated Party
static deployment and `tests/check_controller_page.py`.

- First paint needs no wasm/WebGPU/ROM probe. Budget: under 100 KiB compressed
  total for critical HTML+CSS+JS. Do not add a service worker merely to advertise
  installability; repeat-use installation is an optional post-success prompt.
- Implement every state in the phone journey and a mock-host mode available
  only under the browser test flag.
- Persist only comfort settings and optional device display name locally. A
  session-scoped tab arbiter permits one active publisher per controller lease.
- Press visuals update on pointer-down. Pointer capture preserves 1:1 stick
  tracking and simultaneous action chords; no debounce/animation touches the
  published pad value.
- Prevent browser navigation/zoom gestures only inside the controller surface
  with explicit `touch-action`/overscroll policy; ordinary settings/help remain
  scrollable and zoomable.
- Request Screen Wake Lock after **Use controller**; expose its state, release on
  leave, and reacquire after `visibilitychange` when allowed. Failure is a quiet
  capability fallback.
- `pagehide`, hidden, offline and channel loss synchronously publish neutral
  before UI changes where the browser permits.

**Gate:** no request URL names wasm, ROM, save or game assets; response-header,
semantic/focus, 320×568 through tablet, 200% text, reduced motion/transparency,
increased contrast and real-hand three-chord routes pass.

### LC-05 — Free pairing/signaling service

**Create:** `services/party/{package.json,package-lock.json,wrangler.jsonc}`,
`services/party/src/{worker.ts,party-room.ts,types.ts}`, and service tests.

- Worker routes only create/redeem/connect; one SQLite Durable Object per party.
- Hibernatable WebSockets; no `setInterval`; persist phase/epoch/approved seats,
  not SDP history or inputs.
- Store keyed hashes of invite/credentials. Constant-time comparison, explicit
  expiry, strict origin and body-size allowlists.
- Per-room pending/join/SDP limits and monotonically increasing transition ids.
- Reserve daily request/duration/write budgets separately for control, pairing
  and optional relay. Pairing refuses before consuming the control-plane floor.
- Local Miniflare/workerd test path requires no cloud account.

**Negative controls:** replay an invite after rotation, exceed pending capacity,
send a 1 MiB SDP and restart the object between approval and connection.

### LC-06 — Party sheet and QR

**Create:** `dist/web/party/party-host.js`, `party-host.css`, vendored QR module.
**Modify:** launcher and overlay surfaces, `THIRD_PARTY.md`, `NOTICE.md`.

- One state reducer owns sheet state; DOM rendering has no network side effects.
- QR is derived only from the active invite and is blanked on close/expiry.
- Host approval binds controller id to a free router port. Reject has a reason.
- Pairing phrase is a short-authentication string derived from the host/phone
  key-exchange transcript and is identical on both screens. Approval binds that
  verified transcript, credential and seat generation; the signaling service
  cannot silently substitute keys without changing the phrase.
- Add a screen-reader text equivalent for QR/code/seat state.
- Controller tiles retain number/color/source mapping across close/reopen and
  show input activity without exposing raw button bytes.
- Connection Doctor copy distinguishes service outage, direct failure, guest
  Wi-Fi isolation, VPN interference, relay-full and expired invite.

**Gate:** pinned decoder reads the rendered QR at 1×/2× DPR and light/dark app
themes; a corrupted finder pattern fails the positive control.

### LC-07 — WebRTC controller transport

**Modify:** `dist/web/party/party-host.js`, `dist/web/controller/controller.js`;
add shared signaling/browser tests.

- The approved host creates a generation-tagged offer and both named channels;
  the controller answers through signaling. Use direct ICE in the zero-cost
  profile and request no media permissions. Stale offers, answers and ICE from
  an earlier peer generation are ignored.
- Open the two specified channels; validate protocol labels and control hello
  before accepting state.
- Event-driven state sends plus 50 ms repetition; reliable control ping every
  few seconds, never a per-frame room-service heartbeat.
- ICE restart first reuses the existing approved peer. A terminal peer/channel
  failure creates one fresh peer generation inside the same seat lease; a new
  connection epoch publishes neutral and invalidates all old state packets.
- Export structured close reason and local statistics without IP/SDP logging.

**Gate:** loopback plus real Chromium peers; forced dropped/reordered state,
successful control ping/pong, liveness-watchdog expiry, control-channel close,
same-peer ICE restart and signaling loss/recovery after connection.

### LC-08 — Browser host bridge into four real ports

**Create:** `platform/party/remote_pad_web.c` and JS bridge tests.
**Modify:** `platform/platform_sdl_min.c`, browser source lists.

- JS queues decoded transitions per approved port; one `EM_JS` drain feeds the
  pure remote-pad core during the existing input pump.
- The game consumes remote samples only through `platform_pad_*` and the normal
  `osContGetReadData` edge calculation.
- No Asyncify re-entry from a WebRTC callback; callbacks mutate JS-owned queues,
  and C drains at its ordinary boundary, matching current touch design.
- Browser host sends consumed-input acknowledgement for latency measurement.

**Gate:** four browser peers join P1–P4, drive the real 4P route, and reproduce
the existing viewport/HUD/post-race assertions.

### LC-09 — Mixed controllers and readiness

- Implement explicit source/seat rows and host-controlled reassignment.
- Support keyboard + three phones, physical pad + phones, display touch + phones,
  and four phones. Never infer readiness from connection alone; each source
  completes an input test.
- Reassignment is allowed only at menu/pause-safe boundaries and publishes
  neutral on both ports during the transaction.
- Controller names are escaped, length bounded and never rendered as HTML.

**Gate:** every pairwise source combination plus disconnect/reconnect; color is
not the only status signal.

### LC-10 — Lifecycle and congestion safety

- Centralize `neutralize(reason)` on both endpoints. Cover pointer cancel, blur,
  hidden, pagehide, offline, freeze, channel close, lease expiry, timeout and
  host rejection.
- Drop stale state at backpressure threshold; reliable control has a separate
  small bounded queue and closes on overflow.
- Reconnect preserves approved identity but not held state; the player must
  touch controls again.
- Measure phone-to-consumed-tick latency through synchronized ping samples.
- Compare phone glass-to-display response against a wired controller with a
  high-speed-camera/photodiode method on physical devices. Network timestamps
  diagnose components but cannot substitute for end-to-end feel.

**Release target:** on qualified local Wi-Fi, p95 press-to-consume ≤50 ms and
p99 ≤80 ms over 10 minutes; zero held input more than 250 ms after any injected
loss lifecycle event. Report glass-to-display delta and jitter against the wired
baseline; freeze its gate only after physical calibration rather than inventing
a planning threshold.

### LC-11 — Optional haptics

- Route Rumble Pak start/stop from `platform_pad_rumble()` to the owning remote
  source over reliable control, generation checked.
- Phone advertises vibration capability and user preference. The web Vibration
  API is limited across browsers, so unsupported is a normal no-op.
- Clamp duration/rate; always send stop on release/leave/hidden.

**Negative control:** delayed rumble for an old seat generation must not vibrate
the reassigned phone.

### LC-12 — Free WebSocket fallback

- Reuse pad-state packets through the room only after direct ICE times out.
- Complete the S10 reviewed ephemeral key exchange, verified by the pairing
  phrase, and wrap every relayed packet in sequence/epoch/seat-bound
  authenticated encryption. The Durable Object receives no gameplay key and
  cannot inspect controller input.
- A dedicated relay-budget actor grants short leases below the daily free-plan
  threshold; it sees admissions, never each input packet.
- Limit concurrent fallback rooms, controller rate, duration and message size.
  Refuse before provider exhaustion with a local-play recovery message.
- Direct rooms and already-paired local play do not depend on relay admission.
- This is not an A1 release dependency. “Direct connection unavailable” with
  actionable recovery is acceptable before the optional fallback is qualified.

### LC-13D — Direct-path end-to-end and abuse gates

**Create:** `tests/check_phone_controllers.py`,
`tests/check_party_privacy.py`, `tests/check_party_service.py` and register them.

Matrix: 1–4 phones; mixed sources; portrait/landscape; 30/25 Hz; GL/WebGPU
browser host; 20/60/120 Hz phone display; loss/reorder; service restart; invite
replay; XSS names; hidden controller; host refresh; room full; quota full.

Privacy capture fails on ROM-size bodies, save/snapshot signatures, SDP/IP in
logs, third-party requests or a capability in a GET URL. Mutation arms must
prove timeout, ownership, QR decode, edge history and room epoch assertions.

### LC-13R — Relay-only gates

- Force direct ICE failure and prove encrypted controller play within the
  admitted rate/duration/message budget.
- Capture the Durable Object boundary and prove it sees ciphertext/routing
  metadata but no decodable pad state.
- Exhaust the relay reserve while control/pairing reserves remain usable.
- Mutation arms remove AEAD authentication, reuse a nonce and let relay consume
  the control floor; each must fail independently.

### LC-14 — Native display host

- **Implemented locally:** `MdkrPartyTransport` uses libdatachannel 0.24.3 at an
  exact commit with Mbed TLS 3.6.7, media disabled, four exact transitive
  submodule pins and a dated/hash-pinned Mozilla CA extract. CMake verifies
  every mutable boundary; feature-off builds retain a no-network stub. A
  tracked, hash-pinned MPL source patch keeps VerifiedTlsTransport active on
  Windows only for the explicit-CA Mbed TLS build, closing upstream's
  backend-agnostic Windows fail-open branch. A clean MinGW GCC 16.1 Release
  cross-build and stock-Windows-DLL import audit pass; packaged execution and
  physical lifecycle evidence remain LC-16 gates.
- One originless, versioned WSS upgrade reserves room and socket capacity before
  room creation, returns a one-use bootstrap over that socket, then becomes the
  ordinary authenticated signaling socket. Reconnect binds a 256-bit
  room/role credential. Browser-origin attempts cannot use the native route.
- Ephemeral P-256 ECDH derives the exact shared 20-bit two-word phrase used by
  the browser. The deterministic cross-language vector, invalid-point negative,
  ECC-Q QR module hash, protocol state and bounded queue tests are release gates.
- The persistent launcher and in-game overlay own room/invite/seat state across
  engine loans and rematches. The engine receives only approved bounded packets
  through `MdkrRemotePad` and `PadRouter`; transport callbacks never mutate it.
- State is unordered/unreliable and control reliable/ordered. Disconnect,
  timeout, stale epoch, packet overflow, explicit removal and room teardown all
  publish or preserve neutral before releasing custody.
- No local listener, HTTP server, self-signed certificate or inbound firewall
  port exists. Exact dependency/source-form notices ship in every native
  package. Binary size and physical platform evidence remain LC-16 work.

This is a separate A2 release gate because library/platform surface is
material. It is P0 for the flagship feature, but it does not block S12 once the
pad/session contracts are frozen.

### LC-15 — Device acceptance and rollout

- Required: current iOS Safari camera→controller, Android Chrome, qualified
  desktop Chrome/Edge host, two physical phones simultaneously, poor Wi-Fi,
  phone lock/unlock, notification interruption and 20-minute battery/thermal run.
- Canary behind `party_controller` feature flag; local controls remain default.
- Publish privacy text, troubleshooting, supported-browser matrix and “internet
  is needed to pair; direct local play continues after pairing when possible.”
- Rollback switch hides the party CTA and closes new rooms without altering the
  cached controller/local-game paths.

### LC-16 — Native acceptance and rollout

- Windows, macOS and Linux launcher/overlay run the same approve/assign/remove
  journey and shared state fixtures.
- Required routes: two simultaneous phones, sleep/wake, network change, app
  minimize, display DPI change and Return to Lobby/Rematch under S10 lifecycle.
- Package/license/binary-size review and platform signing/notarization paths
  include the selected WebRTC dependency.
- Native failure never opens a firewall port, starts a local HTTP server or asks
  the player to trust a certificate.

### LC-17 — Earned enhancements

After A1 evidence, add only enhancements justified by observed friction:

- session-approved remembered device key for faster repeat approval, never a
  room-spanning seat grant;
- floating-stick layout that anchors beneath the first thumb while preserving
  edge-safe action geometry;
- optional tilt steering behind a just-in-time permission and calibration step,
  always paired with conventional touch steering;
- post-success **Add to Home Screen** guidance where the platform supports it.

Each enhancement has an off path, capability fallback and reduced-motion/
accessibility equivalent. Do not delay first-race success to advertise it.

## Definition of done

- A new player can scan and steer without instructions beyond the display.
- Four remote phones complete a real split-screen race and results flow.
- A 20 ms tap between authored ticks is consumed once; no lifecycle path leaves
  input held; mixed source ownership is deterministic.
- The controller page is small, accessible, asset-free and independent of
  WebGPU/ROM support.
- Direct play survives signaling loss after pairing. Optional encrypted relay
  fails closed before cost and is not required to close A1.
- Physical iOS/Android evidence closes A1; physical native platform evidence
  closes A2. Scripted peers alone close neither.
