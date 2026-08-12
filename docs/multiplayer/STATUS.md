# Multiplayer operational status

Last reconciled: **2026-08-12**. This is the live execution ledger; sprint files
remain the acceptance specifications. A ticket is not `DONE` because its code
exists—it is done only when its named negative control and release evidence pass.

## Decision summary

| Achievement | State | What is true now | Exit work |
|---|---|---|---|
| A0 Session foundation | EVIDENCE REVIEW | Shared state core/bridge, persistent native runtime and one-module browser rematch all have automated lifecycle proofs. | Human screen-reader/keyboard/gamepad pass and final foundation decision. |
| A1 Browser Phone Party | EVIDENCE REVIEW | Secure QR/code pairing, explicit mixed-source seat assignment, launcher and pause-safe in-game setup, direct WebRTC, optional haptics and wasm P1–P4 handoff pass real-browser automation. Direct input survives signaling loss; signaling recovery rotates the connection epoch and rebinds the same seat. Same-peer ICE restart and reliable-channel failure both fail neutral and recover without surrendering the approved lease. | Four physical phones in a complete race, physical iOS/Android acceptance, service-restart/abuse evidence and rollout review. |
| A2 Native Phone Party | DONE LOCAL / EVIDENCE REVIEW | The persistent launcher owns a pinned static libdatachannel/Mbed TLS adapter, ephemeral P-256 identity, exact browser-compatible phrase and ECC-Q QR. One TLS-verified originless WSS bootstrap creates the ordinary PartyRoom; authenticated reconnect and direct state/control DataChannels feed the shared bounded codec/router while callbacks, overflow, timeout, removal and teardown fail neutral. Launcher and in-game controller management cover responsive QR/code sharing, phrase approval, explicit replacement seats, connection status, invite rotation/revocation, confirmed removal/close and local-play recovery. Release packages carry hash-verified dependency notices. A clean MinGW GCC 16.1 Release cross-build passes with stock Windows DLLs only. | Execute and qualify packaged Windows/macOS/Linux builds, two physical phone routes, sleep/wake/network change/minimize/DPI/rematch, firewall-negative observation, accessibility, signing/notarization and binary-size evidence. Production origin/service provisioning is not claimed. |
| A3 Rollback laboratory | IN PROGRESS | Manifest, snapshot/event/driver, canonical roster, endpoint presentation and launcher transport gates pass. Four isolated real launcher/engine processes share one manifest while using slot-0, slot-1, two-local and no-render envelopes; their authority/input/event projections converge, mapped pixels prove the intended cameras/HUD/divider, and physical audio follows only local listeners. Transport uses authenticated out-of-band slot ownership, a CRC-protected three-frame bundle and a shared 32-snapshot/31-replay-tick bound. The ring has a fail-atomic 16 MiB product cap; the largest current standard-track row is 16,272,416 bytes. LAN, regional-good, regional-variable and poor profiles converge; two-second outage and adversarial profiles preserve an exact pre-fault prefix then unwind into typed launcher recovery at the retained-history boundary. Three regional-variable online epochs/two rematches retain launcher identity with fresh epochs/manifests and zero stale, unauthorized or conflicting ingress. ASan found and now guards complete audio-player, particle and menu/background teardown between arena loans, plus structural seat-count rejection before caller-buffer access. All 20 standard tracks pass delayed correction/exact second replay with an observed human car; a separate ROM-derived matrix passes all 47 legal standard-track/player-vehicle pairings. All 15 balloon type/level configurations use their real inventory branches inside a corrected window and exactly replay 14 concrete weapon IDs, including projectiles with pinned shared assets and rollback-owned attachments. Whale Bay passes a 10,180-tick soak with its instantiated player vehicle observed as hovercraft. Native bounded histograms separately measure capture, restore, resimulated game ticks and authored gameplay frames; the current breadth run peaks at 1.30/0.97/0.27/0.82 ms p99 with no 16.67 ms deadline miss. A real 4P correction, three full standard races/two deep-rematch replays, deterministic AI takeover, journaled sound/rumble and durable-write firewalls pass. Real Chromium/Wasm now also passes tick-300 correction/exact replay, persistence/reload, fullscreen/resize, AudioWorklet and WebGPU recovery with a 391,277-byte snapshot and 1.01 ms resimulation p99. The engine revalidates the manifest track, standard-race catalog type, legal-vehicle mask and authored regional cadence against the loaded ROM before tick one; a mismatched manifest exits cooperatively with clean teardown. | Qualify real audio/rumble devices and future achievement/diagnostic sinks; close PAL, desktop-OS and physical-device performance matrices. Battles/challenges remain explicitly unsupported for online v1 and must continue to fail admission. A deployed authenticated carrier and state resync remain post-`GO`; online race admission stays disabled. |
| A4 Private online alpha | FOUNDATION / GATED | The launcher-owned lobby reducer passes compatibility, seat, character/vehicle selection, vote, barrier, reconnect, leader and retry/atomicity tests. A fixed checksum-protected launch descriptor freezes the manifest and canonical per-seat selections; V3 copy-owns it through the persistent launcher and engine runtime, then direct-loads its track and applies its racer choices. Four endpoint processes converge on identical match identity while retaining different local controller/view maps. Native publishes 42 responsive room views and all 25 actions through keyboard/gamepad. Browser C4 loads the same C projection in an isolated 35 KiB evidence module and passes all 42 native-correlated views, all 25 keyboard routes, touch, accessibility and portrait/landscape 200% reflow. The browser MatchRoom control adapter passes create/join/share/rotate/select/Ready/reconnect/recovery, including a two-clean-profile journey over the real local Worker. A publisher-owned policy ships disabled; an enabled same-origin release still requires clean build provenance and a locally SHA-verified supported ROM before the model loads. Local MatchRoom persists authenticated bounded room control and recovers every phase without exposing replay internals. | Run the uncoached two-person study and human screen-reader/device evidence. Native live binding and peer/race integration remain blocked until the selected carrier and written A3 `GO`; production MatchRoom deployment is not claimed. |
| A5 Zero-cost private beta | FOUNDATION / GATED | Conservative weighted admission/reserves cover Phone Party and MatchRoom HTTP, socket lifetimes and every authenticated native host mutation; repeated rotation cannot escape accounting. Both retain purpose-HMACed short codes rather than raw values. The real local Worker passes a 64-way exhausted-admission flood plus persisted process restart while preserving established control/close traffic, secure static routes, typed accessible local recovery and a literal-zero kill switch. Protected capacity/refusal/retention telemetry and fixed no-extra-write operation buckets reconstruct admitted units, flag legacy traffic and remain unchanged by forged/refused floods. Static deploy skew and versioned Worker/object boundaries pass locally; a checked Free-plan edge policy isolates `/api/` from static play. Strict privacy-safe reconciliation and v2 seven-day ledger contracts reject billing, drift, discontinuity, incomplete tracking, failed local play, open incidents or a fixed synthetic MatchRoom/Phone Party success-latency miss without reflecting input. The controlled collector uses fresh profiles, production HTTP/WebSocket/WebRTC paths, exclusive aggregate output and exact operated sample counts; its one-attempt real-Worker smoke passes create/join, direct input during signaling loss, same-lease epoch rebind and input after recovery, then correctly remains `STOP`. | Run the 20-attempt collector only after hosted provisioning and extend distributed/network chaos; then hosted edge installation, real provider/version/raw-request reconciliation, named runbook owners, privacy review and seven real contiguous zero-currency days. |

There is deliberately no overall `GO` yet. Local play remains the safe product;
online race admission stays disabled until A3 passes.

## Operational execution order

This is a dependency graph, not one serial queue. Work may advance in the
automatable lane while a human/device/provisioning gate waits, but no release
achievement may skip its gate. Keep one implementation item and one evidence
item active per lane; finish the lowest dependency-ready P0 before opening
polish in that same lane.

| Order | Lane | Work packet | Achievement earned | Required evidence |
|---:|---|---|---|---|
| 1 | Automated local | Finish browser mixed-seat model, source-labelled setup, safe in-game controller management, four-seat wasm routing and confirmed optional rumble. **Implementation and collision/fail-neutral mutations pass.** | A1 implementation complete | Existing browser automation stays green; every source has visible ownership, test input and recovery states. |
| 2 | Human/device | Run four-phone race plus iOS Safari/Android Chrome scan, hand/chord, rotation, background, network-loss and accessibility matrix. This blocks A1 release, not independent A3 work. | A1 release | Device/build/browser fingerprints, captures and signed privacy/cost acceptance. |
| 3 | Automated rollback | Close endpoint presentation: local projection geometry, HUD policy, spatial-listener mix and no-render behavior must use the frozen local view map without changing authority. **COMPLETE (automated): canonical camera/output mapping, fail-closed lens equivalence, immutable viewport bytes, pixel coverage, endpoint-local minimap placement, draw-only per-player HUD reflow, canonical audio voice lifetime, local listener volume/pan policy and zero-view silence/chrome suppression pass. Physical-device listening remains in lane 6.** | A3 R2 presentation complete | Slot-0, slot-1, 2-local and no-render authority remains identical; screenshot/audio witnesses and mutations prove each local policy. |
| 4 | Automated rollback | Drive the launcher-owned MatchTransport adapter through real isolated processes using named packet and clock profiles, confirmation and two rematches. **COMPLETE (automated): four endpoints pass LAN/regional/poor convergence; outage/adversarial enter bounded typed recovery; three impaired epochs prove exact retirement.** | A3 network path complete | Non-vacuous counters, exact pre-fault prefixes, safe transport telemetry, clean engine unwind and exact-epoch launcher recovery/rematch. |
| 5 | Automated rollback | Extend in-scope standard-track/item/vehicle and 2P/4P lifecycle coverage; measure snapshot, resimulation and authored-frame p50/p95/p99 budgets. Keep deferred battles/challenges fail-closed at admission. **All 20 tracks with a human car, all 47 ROM-legal track/player-vehicle pairings and all 15 balloon type/level configurations pass exact correction/replay; the item matrix covers 14 concrete weapon IDs and includes mutation, bounds and mode negatives. Whale Bay passes a 10,180-tick observed-hovercraft soak. Deterministic disconnect takeover, a deep post-race replay, three full native races/two rematches and the real Chromium/Wasm runtime pass. Separate capture/restore/resimulation/authored-frame histograms, a 16 MiB ring cap, statistical 8.33 ms p99 and hard 16.67 ms tail counters are implemented and required by the gates.** | A3 candidate | Omitted-byte, forbidden-I/O and remaining PAL/OS/device budget negatives pass. |
| 6 | Human/platform | Qualify real audio/rumble and native/wasm NTSC/PAL on macOS, Windows and Linux; publish written `GO` or `STOP`. | A3 decision | Fingerprinted matrix and evidence review; no online-race capability exists before `GO`. |
| 7 | Parallel launcher UX | Automated native/browser fake-adapter coverage is complete. Bind the proven launcher model to MatchRoom without moving invite, compatibility, connection or recovery state into the game; run first-time friend and human assistive-technology evidence. | A4 UX ready | Live adapter preserves the 42-state/25-action fixture contract; two-person task study and screen-reader/device review pass. |
| 8 | Post-A3 online | Keep the locally complete MatchRoom recovery seam gated; after A3 `GO`, add authenticated direct transport, live barriers, bounded metrics, runbooks and chaos/load gates. | A4 then A5 | Seven-day no-billing-method quota ledger and rollback drill; local/Phone Party remain available under every service failure. |

## Golden-path execution train

This is the operational queue. “Parallel” means a separate lane with its own
one-ticket WIP limit; it does not permit skipping an exit gate. Each packet
ends in a player-visible demonstration and stores its command output, fixture
version, platform/browser fingerprint and reviewer decision under
`docs/evidence/multiplayer/`.

| Seq | Work packet | State / achievement | Required exit and baked-in guard |
|---:|---|---|---|
| 0 | Keep the feature-off/local baseline green | CONTINUOUS | Default Online Room makes no request and exposes no Start; Play Here, cached local play and Phone Party remain usable during synthetic DNS/service/quota failure. Source boundary continues to reject service/network concepts from `game/src`. |
| 1 | Native Online Room entry + fake journey | EVIDENCE REVIEW / UX foundation | Wide and compact render captures, touch-scroll witness, sanitized view model and stale callback/idempotency controls pass. Default release gate is local-only and false. |
| 2 | All-state/failure gallery | AUTOMATED COMPLETE / EVIDENCE REVIEW | All 42 states cover the complete fake journey, 17 typed failures and six elapsed timeout outcomes with one clear recovery and Play Here preserved. |
| 3 | Native keyboard/gamepad/speech routes | AUTOMATED COMPLETE / EVIDENCE REVIEW | All 25 actions pass keyboard and virtual-gamepad activation; every view's spoken title/action set passes. Human screen-reader/device review remains. |
| 4 | Browser Online Room binding | AUTOMATED CONTROL COMPLETE / EVIDENCE REVIEW | The shared-C gallery correlates all 42 views and 25 routes; an explicit MatchRoom seam now passes create/join/share/rotate/select/ready/reconnect/recovery. Touch, recovery AX and portrait/landscape 200% reflow remain green. |
| 5 | First-time friend usability | AUTOMATED FLOW COMPLETE / NEXT HUMAN UX | Two isolated clean Chrome profiles over the real local Worker pass create/share, wrong-role recovery, `/room/` join, socket outage/retry, distinct selection, Ready/backtrack/re-Ready, accessibility and leave with bounded automation timings. Run the uncoached human create/share/join/ready/cancel/rematch study and close P0/P1 observations. Race admission remains gated. |
| 6 | Phone Party physical release evidence | PARALLEL HUMAN / A1 | Four-phone mixed-source race; iOS/Android scan, chords, rotation, background, loss, haptics and accessibility. Capabilities are erased/revoked and input fails neutral. |
| 7 | Remaining rollback breadth/platform proof | PARALLEL ENGINE / A3 candidate | Item families, wasm/PAL/Windows/Linux/macOS timing, real audio/rumble and persistent lifecycle pass with bounded memory/time and forbidden side effects at zero. |
| 8 | Written A3 decision | HARD GATE | Publish `GO` only with the fingerprinted matrix. `STOP` names the failed boundary. Until GO, no live adapter or service response can admit an online race. |
| 9 | MatchRoom persistence adapter | DONE LOCAL / O1 | Pure TypeScript reducer and SQLite Durable Object complete create/link-or-code join/race/rematch/close locally. Every phase reconstructs exactly; capabilities, codes and credentials are purpose-hashed/scoped; control tail and retention are bounded; hibernated sockets and expiry alarms pass. Live launcher/race admission remains gated. |
| 10 | Authenticated direct/one-hop carrier | AFTER 9 / O2 | 2–4 endpoints, partial graph diameter ≤2, malicious forwarding, stale epoch, reconnect and signaling-outage gates. Pairwise keys prevent source impersonation; SDP/IP never enters logs. |
| 11 | Live preflight/barrier/results binding | AFTER 9–10 / O1–O2 vertical slice | Live effects plug below the already-proven launcher model; exact build/ROM/settings/route checks freeze one descriptor, Cancel cannot resurrect load, results/rematch preserve the room. |
| 12 | Invited alpha | AFTER 11 / A4 | Device/network/usability matrix, Connection Doctor, update flow, privacy consent and red-team report pass. Rollback drill disables new rooms without affecting local play. |
| 13 | Optional encrypted relay admission | VALUE-GATED P1 | Build only if direct alpha evidence justifies it. Per-recipient ciphertext, separate reserve, hard daily admission and kill switch prove capacity exhaustion refuses new leases before cost. |
| 14 | Zero-cost operated beta | AFTER 12 (and 13 only if enabled) / A5 | No billing method/paid plan, seven-day quota ledger, SLO/error budget, deploy skew, chaos/load, abuse and privacy erasure evidence plus rehearsed runbooks. Local play succeeds throughout. |

The shortest path to something excellent is packets 1–5 plus the parallel A3
decision—not service code. That order makes the complete experience cheap to
change, validates language and recovery before distributed state exists, and
preserves the launcher as the sole owner of matchmaking and Party UX.

### Current automated closure queue

Execute these in order unless a failing gate creates a lower-level repair:

1. **COMPLETE (automated): deterministic, room-authorized AI takeover uses an
   immutable future tick, confirmed neutral history, replay-stable masks and no
   handback; four processes converge for a seat that is local on one endpoint
   and remote on the others.** Product reconnect timers/copy remain A4 work.
2. **TRACK/VEHICLE/ITEM MATRIX COMPLETE:** retain the 20-standard-track
   human-car, 47-ROM-legal-player-vehicle, 15 balloon type/level and
   observed-hovercraft Whale Bay soak rows.
   Keep named-profile and persistent-rematch rows as prerequisites.
   Online v1 does not admit battle/challenge/boss/hub/cutscene modes: the frozen
   manifest and ROM catalog must reject those before authored tick one.
3. **NATIVE AUTOMATION COMPLETE:** retain separate snapshot, replay-tick and
   authored-gameplay-frame p50/p95/p99 gates plus the 16 MiB fail-atomic cap.
   Preserve the passing Wasm row and extend the same evidence to PAL/OS/device rows; renderer/GPU presentation
   latency remains a platform qualification, not a mislabeled simulation frame.
4. Complete native/wasm NTSC/PAL and OS/device qualification, including real
   listening and rumble. Provisioning/device waits do not stop automated work.
5. Publish the A3 `GO` or `STOP`. Only `GO` unlocks a production online-race
   action; carrier authentication and state resync are subsequent A4 work.

## Ticket ledger

### S10 — session foundation

| Tickets | State | Evidence / next action |
|---|---|---|
| SF-00–02, SF-04 | DONE | Architecture, fixtures, `SessionCore` and bridge unit gates. The bridge wraps the rollback/net canonical manifest in endpoint-local seat/view metadata, so distinct seats and a no-render verifier retain one byte-identical match identity. Native runtime admission requires an exact epoch, and engine publication is copied rather than pointer-borrowed. |
| SF-05 | EVIDENCE REVIEW | `check_persistent_app_session.py` proves three native engine epochs/two rematches in one launcher runtime, with launcher draws between loans and clean host teardown after every return. |
| SF-06 | EVIDENCE REVIEW | `check_persistent_browser_session.py` proves one wasm module across repeat play. |
| SF-03, SF-07–10 | IN PROGRESS | Browser patterns, pause policy, adapters, threat/data/cost rules and taxonomy exist; reconcile native UI and final fixtures. |
| SF-11–12 | READY | Run human/device/accessibility acceptance, mutations and publish the A0 decision. |

### S11 — Phone Party

| Tickets | State | Evidence / next action |
|---|---|---|
| LC-00–08 | EVIDENCE REVIEW | C/JS codec vectors, router/history tests, controller/host real-browser gates, QR decode, service tests and direct WebRTC handoff pass. The transport gate now covers same-peer ICE restart, control-channel fail-neutral/fresh-generation recovery and continued input across a real-Worker signaling outage and epoch rebind. Destructive phone/host leave paths require safe-default confirmations; cancel-before-mutation, neutral confirmed leave, 320×568/200% layout, modal overscroll, dynamic form semantics and notch-safe room handoff pass. |
| LC-09 | EVIDENCE REVIEW | The launcher renders keyboard/gamepad/touch/phone sources, host approval carries an explicit seat, replacement is labeled, and a reconnecting phone remains neutral without local takeover. A physical mixed race remains. |
| LC-10 | EVIDENCE REVIEW | Bounded queues, 250 ms neutral, lifecycle release, direct-channel failure and reconnect epochs pass automation. Healthy direct channels remain active during signaling-only loss; physical background/network-loss matrix remains. |
| LC-11 | EVIDENCE REVIEW | Bounded optional phone vibration now crosses the reliable channel with capability negotiation and local opt-out; real-device feel and rollback journal confirmation remain. |
| LC-12, LC-13R | PROPOSED P1 | Build only after direct-path acceptance shows fallback value; requires application-layer AEAD and a separate hard reserve. |
| LC-13D | EVIDENCE REVIEW | Two-browser direct proof, independent wasm P1–P4 queues and the local in-game pause/revoke/preserve path pass; add four physical phones in one race plus service-restart, abuse/load and device evidence. |
| LC-14 | DONE LOCAL / EVIDENCE REVIEW | Pinned media-disabled libdatachannel/Mbed TLS transport, authenticated one-WSS service bridge, exact SAS/QR vectors, bounded callback/packet queues, direct DataChannels, launcher/overlay lifecycle and release notices pass locally. |
| LC-16 | HUMAN / PROVISIONING | Run the packaged Windows/macOS/Linux and physical-device lifecycle/accessibility matrix; archive firewall-negative, signing/notarization and binary-size evidence before A2 release. |
| LC-15 | READY | Physical browser acceptance, docs and staged A1 rollout. |
| LC-17 | PROPOSED P1 | Remembering, floating stick and tilt are earned enhancements after A1; never expand the initial critical path. |

### S12 — rollback core

| Tickets | State | Evidence / next action |
|---|---|---|
| RB-00–01 | EVIDENCE REVIEW | Frozen declaration census/self-test, manifest mutation test, bounded canonical input/prediction tests. |
| RB-02–05 | IN PROGRESS | Snapshot/ring/event/driver code compiles into native and wasm products. Atomic registration covers object/list/settings/global/camera/particle/wave/behaviour/transition/input foundations, a dedicated dynamic ModelInstance subpool, post-race/scene-load latches and spatial-audio source topology; direct restore tests prove heap/scalar/auxiliary/transition/audio-source identity, allocator reuse and rebuild hooks. The shared production budget is 32 snapshots, at most 31 replayed ticks and input age 30. All 20 standard tracks pass delayed correction/exact second replay with an observed human car, and the independently decoded ROM matrix passes all 47 legal standard-track/player-vehicle rows; native snapshots span 500,225–508,513 bytes (maximum 16,272,416-byte ring) and ring allocation fails atomically above 16 MiB. A separate 15-row item matrix covers every balloon type/level and 14 concrete weapon IDs through the real inventory path; it exposed and now guards projectile asset residency, rollback-owned model/attachment allocation and snapshotted shared-model reference lifetime. Suppressed-release, invalid-index and wrong-mode controls fail closed. Native breadth p99 peaks at 1.30 ms capture, 0.97 ms restore, 0.27 ms resimulated game tick and 0.82 ms authored gameplay frame, with zero hard 16.67 ms misses. Whale Bay completes a 10,180-authored-tick observed-hovercraft soak at 0.19/0.18/0.05/0.16 ms p99 with a 506,545-byte snapshot. The audio/lifecycle-qualified native 4P route uses 143 ranges and a 500,225-byte snapshot. Its delayed-input arm exactly repeats corrected authority, suppresses duplicate starts, defers corrected starts and coalesces replay-time SFX commands after reconciliation; its deep arm replays tick 4,800 post-race. The real Chromium/Wasm product passes tick-300 correction/exact replay with a 391,277-byte snapshot and 1.01 ms resimulation p99. Zero adapter rejection/overflow or forbidden I/O is required. PAL, additional desktop OS and physical-device p99 budgets remain; unsupported race types are an admission-negative, not accidental scope. |
| RB-06 | EVIDENCE REVIEW | Canonical/local roster, independent local-seat/view maps and copy-owned manifest+roster engine runtime pass. Launcher-owned `MdkrMatchTransport` enforces authenticated out-of-band ownership, additive confirmation, exact corrections, atomic drains and exact epochs. The 24-byte single-input codec and 64-byte three-frame bundle are fixed, CRC-protected and fail-atomic. Four isolated processes prove slot-0, slot-1, 2-local and no-render authority plus viewport/HUD/audio policy. The loaded ROM must match manifest track, standard-race type and regional cadence before tick one; the real-process mismatch control exits nonzero without aborting and tears the host down cleanly. Safe transport telemetry is required at retirement; remote viewport admission fails before boot. Physical listener quality and a deployed carrier/authentication handshake remain outside this automated row. |
| RB-07–08 | EVIDENCE REVIEW | LAN, regional-good, regional-variable and poor named profiles converge across four isolated real processes. Two-second outage and adversarial profiles exercise loss/reorder/corruption/clock pause, preserve the exact pre-fault prefix and transition once into launcher-owned connection-lost recovery after the 31-tick retained-history bound. Every profile requires non-vacuous counters; invalid, stale, unauthorized, conflicting and rejected drains remain zero. Production state resync is intentionally not claimed. |
| RB-09–12 | IN PROGRESS | Native production shell completes three full 4P standard races/two deep rematches. A separate three-epoch regional-variable online carrier run proves fresh epoch/manifest state, stable launcher identity, zero stale/recovery counters and clean teardown; ASan exposed and now guards audio-player, particle and complete menu/background arena-root retirement, plus structural input-count rejection before caller-buffer access. Deterministic disconnect takeover now has an exact future-tick launcher control, remote/local seat parity, confirmed neutral history, replay-stable AI mask, canonical identity preservation and no-double-owner/no-handback gates; four real processes converge. The browser runtime parity gate passes. Production reconnect timing/control-log delivery, online result exchange, PAL/desktop-OS qualification and the physical-device matrix remain. |
| RB-13 | GATED | Publish `GO` only if every real-game gate and measured budget passes; otherwise publish `STOP` with the failing boundary. |

### S13 — private online and operations

| Tickets | State | Evidence / next action |
|---|---|---|
| ON-00 | EVIDENCE REVIEW | `platform/online/lobby_core.*` and protocol v1 implement the pure launcher reducer; lifecycle, compatibility, capacity, authorization, CAS, retry, reconnect and leader-custody controls pass. |
| ON-00S | EVIDENCE REVIEW | A–E are complete: the reducer enforces per-seat ownership, unique characters, player vehicles, selection revisions, Ready invalidation and a ROM-derived legal mask; `MdkrMatchLaunchDescriptorV1` freezes canonical selections plus manifest into 148 checksummed bytes; V3 copy-owns it through `SessionRuntime` and the engine runtime. A real-ROM launch directly applies the descriptor track/characters/vehicles, and four endpoint processes converge on identical identity with distinct local mappings. Byte, lobby/manifest, envelope, epoch, source-mutation and loaded-ROM mismatch controls reject atomically. The mask remains capability metadata, never a player choice. |
| ON-03 | IN PROGRESS P0 / EVIDENCE REVIEW | ON-03A/B/C1–C5 automation, ON-03D and the browser MatchRoom control binding are implemented. Native publishes 42 reducer-built cases covering 10 view kinds, all 17 typed failures and six elapsed timeout outcomes; all 25 actions pass keyboard/gamepad. The publisher policy ships disabled, so opening Online Room performs no room I/O and the 35 KiB shared-C module remains unloaded. The enabled launcher seam requires same-origin policy, clean source provenance and a locally SHA-verified US/PAL ROM; activation is idempotent and performs no `/api/` request. It validates minimized MatchRoom snapshots and proves fragment/code create/join, accessible QR sharing with bounded fallback, generation rotation, selection, Ready, socket Retry and corrupt-state recovery. Two clean profiles converge through the real local Worker, including wrong-role recovery and Ready backtrack. Dirty builds, cross-origin targets and service data—including an adversarial admission flag—cannot expose Start. **NEXT:** uncoached two-person and human screen-reader/device evidence; native live binding follows the selected carrier. No production online-race action before A3 `GO`. |
| ON-01 | DONE LOCAL / EVIDENCE REVIEW | `services/party/src/match/` adds the bounded protocol/reducer plus a v3 SQLite `MatchRoom`: authenticated actor injection, QR/deep-link and isolated rate-limited code joins, leader-only generation-checked invite rotation, complete two-endpoint race/rematch, 64-result control tail, every-phase reconstruction, hibernated socket recovery, exact retry/conflict/CAS controls, canonical C/TypeScript fingerprint vector, minimized public projection, corrupt-history rejection, alarm deletion and raw-code storage negatives pass in the 41-test local service suite. Complete DO transitions are input-gated across await points; simultaneous Match commands produce one success and one exact stale-revision response, while competing Phone Party approvals can lease a seat only once. State-only sockets reject ordinary client commands and byte-bound oversized UTF-8/binary frames before closing. The shared allocation-free UTF-8 scanner also protects Phone Party signaling. A 24,576-command seeded reducer campaign proves deterministic mirror execution, valid bounded state, atomic rejection, exact duplicate receipts and reconstruction parity. Production deployment is not claimed and admission remains false. |
| ON-02, ON-04–06 | GATED BY A3 GO | Direct graph, compatibility, barriers and recovery integrate only with proven rollback. |
| ON-07 | PROPOSED P1 | Encrypted hard-capped fallback is optional and must never consume control reserve. |
| ON-08–11 | PARTIAL LOCAL / GATED BY ALPHA | Admission red-team proves mixed flood refusal, weighted HTTP/socket control reserve, literal-zero stop, protected identifier-free capacity snapshot, bounded refusal writes and finite aggregate retention. Two-release Chromium proves static waiting-worker custody and atomic offline activation. All 15 Worker/object calls now use v1 while four objects retain legacy readers and reject unknown versions pre-storage. Seeded reducer chaos and socket-size attacks pass locally. The v2 ledger freezes aggregate-only synthetic experience samples and stop thresholds; its controlled production-path collector passes a deliberately non-qualifying real-Worker smoke including signaling loss and recovery. Hosted provider/version rollback, the real 20-attempt run, distributed/network chaos and beta evidence follow a working alpha. |
| ON-12 | PROPOSED P2 | Mixed couch+online/nearby mode follows A5 and must not complicate the golden path. |

## Security, privacy and $0 controls already enforced

- Controller invite security is 128 random secret bits in a URL fragment; the
  full payload also carries a 128-bit room id. The fragment is erased before
  any request. Each redeem creates a distinct credential; invite rotation
  invalidates the old secret and code.
- Dismissing the in-game setup sheet revokes the displayed invite without
  dropping approved leases. Reopening rotates to a fresh QR/code; a stale
  queued dialog-close event cannot tear down the new room.
- P-256 ephemeral ECDH derives a 20-bit two-compound-word comparison phrase on the two
  endpoints. Approval binds the authenticated controller identity and a seat
  lease; controller signaling cannot spoof another controller id.
- Strict same-origin, body/type/length/rate/transition limits, keyed credential
  digests, no-store responses, restrictive CSP/Permissions Policy and bounded
  hibernatable signaling are present from the first service slice.
- Gameplay pad state is direct DTLS DataChannel traffic and is never stored or
  relayed by the Party service. ROM/save/snapshot/framebuffer/audio data are
  absent from controller resources and requests.
- Pairing admission stops conservatively below free request ceilings and keeps
  a control/close reserve. Expired codes and pseudonymous rate buckets are
  alarm-purged. The deploy profile has no billing dependency and `workers_dev`
  is disabled.
- Host, controller and match endpoint credentials bind a 128-bit nonce to their
  exact room/role with a 128-bit HMAC while keeping the existing 43-character
  wire shape. Forged/cross-room control rejects before budget/object access; a
  real 64-request forged flood leaves the control reserve at zero.
- The operations capacity snapshot uses an independent secret, exact
  aggregate-only schema, `no-store` responses and daily shards deleted after 32
  days. Missing configuration is hidden; unauthorized reads do not touch the
  budget object; refusal floods persist only the first latch per category.
- The checked-in Free-plan WAF payload spends the one available rate-limit rule
  only on `/api/`, blocks one IP above 30 requests/10 seconds before Worker
  handling, and leaves every static/local recovery path outside. Provider HTML
  `429` maps to typed capacity UX. Zone installation and distributed low-rate
  exhaustion evidence remain human/provisioned.
- The offline $0 reconciliation gate accepts only exact aggregate schemas,
  validates internal capacity arithmetic, refuses billing/overages/nonzero
  charge and stops at 75% on either side. Its adversarial CLI test proves
  unknown private values never reach diagnostics. A real hosted provider export
  and seven-day currency ledger remain provisioning evidence.
- The companion secret-gated health view exposes only twelve fixed reservation
  buckets plus a legacy counter. It derives weighted units without another
  write, requires exact versioned operation/weight pairs and reports complete,
  partial or invalid tracking. The real restart/flood test requires exact
  20/42 unit reconstruction and an empty complete literal-zero view.
- The exact seven-day beta ledger validator re-runs each provider/capacity
  reconciliation and health-weight invariant, requires contiguous UTC days,
  build/deployment digests, successful service-blocked local play, zero open
  incidents and daily/final `GO`. Fixture validation is explicitly not hosted
  evidence.
- “$0” is an admission guarantee, not unlimited availability: the product may
  refuse new pairing/online sessions and difficult NAT paths. Offline
  keyboard/gamepad/touch local play always remains available.

## Repeatable evidence commands

From the repository root:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
python3 tools/check_rollback_authority.py --self-test
python3 tests/check_controller_page.py
python3 tests/check_party_host.py
python3 tests/check_phone_party_webrtc.py
python3 tests/check_online_room_gallery.py --build build
python3 tests/check_online_room_a11y.py --build build
python3 tests/check_online_room_actions.py --build build
python3 tests/check_browser_online_room.py --shell-dir dist/web
python3 tests/check_browser_online_activation.py --shell-dir dist/web
python3 tests/check_browser_online_room_gallery.py --build build --shell-dir dist/web
python3 tests/check_browser_online_match_room.py --shell-dir dist/web
python3 tests/check_browser_online_two_person.py --shell-dir dist/web
python3 tests/check_party_capacity.py --shell-dir dist/web
python3 tests/check_party_internal_api.py
python3 tests/check_party_edge_policy.py
python3 tests/test_party_usage_reconciliation.py
python3 tests/test_party_experience_canary.py
python3 tests/test_party_beta_ledger.py
python3 tests/check_web_publish_stamp.py
python3 tests/check_browser_publish_skew.py --shell-dir dist/web
python3 tests/check_touch_controls.py --engine-dir build-web --shell-dir dist/web
python3 tests/check_match_launch_direct_load.py --build build --rom baserom.us.v80.z64
python3 tests/check_online_process_convergence.py --build build --rom baserom.us.v80.z64
python3 tests/check_online_process_convergence.py --build build --rom baserom.us.v80.z64 --launch-v3
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 python3 tests/check_persistent_rollback_rematch.py --build build-asan --rom baserom.us.v80.z64 --timeout 600
python3 tests/check_rollback_track_matrix.py --build build --rom baserom.us.v80.z64
python3 tests/check_rollback_item_matrix.py --build build --rom baserom.us.v80.z64
python3 tests/check_rollback_vehicle_matrix.py --build build --rom baserom.us.v80.z64
python3 tests/check_rollback_soak.py --build build --rom baserom.us.v80.z64
(cd services/party && npm run check)
```

Deployment remains intentionally absent from this ledger until a named owner
provisions the production Party origin/secret and completes the deploy,
capacity, privacy and rollback runbooks. A local dry run is evidence of build
integrity, not evidence that production exists.
