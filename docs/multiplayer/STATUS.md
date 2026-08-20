# Multiplayer operational status

Last reconciled: **2026-08-13**. This is the live execution ledger; sprint files
remain the acceptance specifications. A ticket is not `DONE` because its code
exists—it is done only when its named negative control and release evidence pass.

## Decision summary

| Achievement | State | What is true now | Exit work |
|---|---|---|---|
| A0 Session foundation | EVIDENCE REVIEW | Shared state core/bridge, persistent native runtime and one-module browser rematch all have automated lifecycle proofs. | Human screen-reader/keyboard/gamepad pass and final foundation decision. |
| A1 Browser Phone Party | IMPLEMENTED / EVIDENCE REFRESH | Secure QR/code pairing, explicit mixed-source seat assignment, launcher and pause-safe in-game setup, direct WebRTC, optional haptics and wasm P1–P4 handoff have prior real-browser evidence. Direct input survives signaling loss; signaling recovery rotates the connection epoch and rebinds the same seat. Host and phone now add exact 16 KiB/ten-second responses, actual receipt-relative invite TTL, exact monotonic socket state with equivocation rejection, five-attempt generation-guarded recovery, manual phone Retry, QR-to-`/controller/`+code fallback and server-side one-controller signaling delivery. The relay rebuilds exact role-specific SDP/ICE/hello envelopes, injects phone identity from socket custody and exact-validates native host mutations before reserve or authority. Per-seat removal is launcher-owned, confirmation-first, cancel-safe and terminates only the target lease/socket with neutral recovery copy. The previously authored Worker/browser negatives are now **executed and green** (2026-08-13): QR-failure, actual-TTL/rotation-generation, malformed/equivocated state, finite-reconnect request/media-type, signaling-envelope, native-command, two-controller targeted delivery, removal isolation, and the controller fragment-scrub/abandon-on-scrub-failure/embedded-copy/duplicate-tab/pagehide-freeze-BFCache arms. Executing them found one real product defect: duplicate-tab takeover replaced only the fragment, so it never re-entered the document and left the capability in the address bar; fixed, with the two instruments that could not have caught it (fragment-only navigation creating no document; a focus sample racing a queued `close` event) repaired without changing an assertion. Service-restart and abuse evidence is automated against the real local Worker: a restart with live leases rebinds both phones to their own seats and rotates the epoch while refusing pre-restart generations; a 112-way concurrent abuse flood during a live session buys zero admission units and moves no foreign seat; the literal-zero kill switch refuses new pairing while established control keeps flowing; and exhaustion refuses with a typed, `no-store`, secret-free enum tied to the sentence the phone shows. | Four physical phones in a complete race, physical iOS/Android acceptance and rollout review. |
| A2 Native Phone Party | DONE LOCAL / EVIDENCE REVIEW | The persistent launcher owns a pinned static libdatachannel/Mbed TLS adapter, ephemeral P-256 identity, exact browser-compatible phrase and ECC-Q QR. One TLS-verified originless WSS bootstrap creates the ordinary PartyRoom; authenticated reconnect and generation-tagged direct state/control DataChannels feed the shared bounded codec/router. Both channel closes, bounded control ping timeout, callbacks, overflow, packet timeout, removal and teardown fail neutral; terminal peers rebuild without surrendering the seat and stale answer/ICE generations are ignored. Launcher and in-game controller management cover responsive QR/code sharing, phrase approval, explicit replacement seats, connection status, invite rotation/revocation, confirmed removal/close and local-play recovery. Release packages carry hash-verified dependency notices. A clean MinGW GCC 16.1 Release cross-build passes with stock Windows DLLs only. Packaged qualification now runs in every release lane (2026-08-13): a GPU-, network- and ROM-free bring-up smoke drives the real host in the Linux, macOS and Windows lanes — including the Windows lane, which has no qualifying adapter — proving the bootstrap reaches exactly the configured secure origin and fails closed otherwise, the invite is an HTTPS URL carrying its capability in the fragment, the fallback code is six digits, and the QR encodes that URL at ECC-Q against a cross-language oracle taken from the browser encoder. A companion static gate inspects the packaged artifact for the real WSS/WebRTC transport rather than the stub and for the compiled service origin, and each lane threads the artifact's own version into the smoke. Minimize/DPI/rematch, firewall-negative and binary-size evidence are closed in automation: a live session keeps leases, seats, invite and input routing across a 30-second minimize, the 1/2/1 DPI re-layout and three engine loan round trips; refused connect, refused WSS upgrade and silent mid-session drop all fail neutral with the shipped copy, never self-retry and release seats rather than leave stale input, with the service half proven against a real Worker whose listener is killed and then rebound as a blackhole; and seven release-lane artifacts are recorded against generous tripwire ceilings. | Two physical phone routes, sleep/wake, real network change, accessibility and signing/notarization. Production origin/service provisioning is prepared to one login, one command and one verify script ([`DEPLOY_PHONE_PARTY.md`](DEPLOY_PHONE_PARTY.md)) but is not claimed: no account, domain or secret is provisioned. The Linux, Windows and notarized-macOS size baselines stay unrecorded until a machine that builds them runs `--record`. |
| A3 Rollback laboratory | IN PROGRESS | Manifest, snapshot/event/driver, canonical roster, endpoint presentation and launcher transport gates pass. Four isolated real launcher/engine processes share one manifest while using slot-0, slot-1, two-local and no-render envelopes; their authority/input/event projections converge, mapped pixels prove the intended cameras/HUD/divider, and physical audio follows only local listeners. Transport uses authenticated out-of-band slot ownership, a CRC-protected three-frame bundle and a shared 32-snapshot/31-replay-tick bound. The ring has a fail-atomic 16 MiB product cap; the largest current standard-track row is 16,272,416 bytes. LAN, regional-good, regional-variable and poor profiles converge; two-second outage and adversarial profiles preserve an exact pre-fault prefix then unwind into typed launcher recovery at the retained-history boundary. Three regional-variable online epochs/two rematches retain launcher identity with fresh epochs/manifests and zero stale, unauthorized or conflicting ingress. ASan found and now guards complete audio-player, particle and menu/background teardown between arena loans, plus structural seat-count rejection before caller-buffer access. All 20 standard tracks pass delayed correction/exact second replay with an observed human car; a separate ROM-derived matrix passes all 47 legal standard-track/player-vehicle pairings. All 15 balloon type/level configurations use their real inventory branches inside a corrected window and exactly replay 14 concrete weapon IDs, including projectiles with pinned shared assets and rollback-owned attachments. Whale Bay passes a 10,180-tick soak with its instantiated player vehicle observed as hovercraft. Native bounded histograms separately measure capture, restore, resimulated game ticks and authored gameplay frames; the current breadth run peaks at 1.30/0.97/0.27/0.82 ms p99 with no 16.67 ms deadline miss. A real 4P correction, three full standard races/two deep-rematch replays, deterministic AI takeover, journaled sound/rumble and durable-write firewalls pass. Real Chromium/Wasm now also passes tick-300 correction/exact replay, persistence/reload, fullscreen/resize, AudioWorklet and WebGPU recovery with a 391,277-byte snapshot and 1.01 ms resimulation p99. The engine revalidates the manifest track, standard-race catalog type, legal-vehicle mask and authored regional cadence against the loaded ROM before tick one; a mismatched manifest exits cooperatively with clean teardown. | Qualify real audio/rumble devices and future achievement/diagnostic sinks; close PAL, desktop-OS and physical-device performance matrices. Battles/challenges remain explicitly unsupported for online v1 and must continue to fail admission. A deployed authenticated carrier and state resync remain post-`GO`; online race admission stays disabled. The five carrier-review findings (G-1 verification-phrase key commitment, G-2 seal-window key binding, G-3 budget-before-validation, G-4 unbounded generation map, G-5 vacuous hibernation grep) are all **CLOSED** as of 2026-08-13 with evidence recorded in [`OPERATIONAL_BACKLOG.md`](OPERATIONAL_BACKLOG.md#required-before-the-written-a3-go--carrier-review-findings-2026-08-13); the remaining `GO` conditions are human/device evidence, not these findings. |
| A4 Private online alpha | FOUNDATION / GATED | The launcher-owned lobby reducer passes compatibility, seat, character/vehicle selection, vote, barrier, reconnect, leader and retry/atomicity tests. A fixed checksum-protected launch descriptor freezes the manifest and canonical per-seat selections; V3 copy-owns it through the persistent launcher and engine runtime, then direct-loads its track and applies its racer choices. Four endpoint processes converge on identical match identity while retaining different local controller/view maps. Native/browser now compile the same ABI-v4 shared projection with 43 responsive room cases and 27 actions, including a prominent bounded three-compound phrase, accessible mismatch warning, explicit **Words Match** and **Words Differ** decisions, fresh secure retry, rematch comparison and a leader-only expired-invite replacement state. A native browser-free conformance gate drives all 43 cases, 10 kinds, 18 failures and 27 actions normally and under ASan; service projection cannot synthesize the local mismatch. A separate pure presenter used by the shipped browser renderer consumes all 43 authoritative C projections in Node and proves action/selection order, timeout priority, count grammar, local recovery substitution, immutable output, malformed-model refusal and phrase/mismatch announcements. Its exhaustive live-action policy admits effects only from the current enabled projection, refuses stale/unrendered routes before I/O, keeps gallery mutation evidence-only and prevents production Check Setup from simulating the still-gated carrier. The prior 25-action native/browser rendered evidence remains valid for unchanged routes; the expanded phrase/27-action/invite-expiry rendered pass is deliberately pending execution. The browser MatchRoom control adapter passes create/join/share/rotate/select/Ready/reconnect/recovery, including a two-clean-profile journey over the real local Worker, but it is not authenticated-carrier phrase evidence. Its hardened state boundary now rejects publication regression/equivocation and control-log/state disagreement; receipt-relative early expiry destroys stale invite secrets without trusting display clock synchronization. A publisher-owned policy ships disabled; an enabled same-origin release still requires clean build provenance and a locally SHA-verified supported ROM before the model loads. Local MatchRoom persists authenticated bounded room control and recovers every phase without exposing replay internals. | Rerun native/browser gallery, input, speech, invite-expiry and AX gates; then run the uncoached two-person and human screen-reader/device evidence. Authenticated live phrase binding and peer/race integration remain blocked until the selected carrier and written A3 `GO`; production MatchRoom deployment is not claimed. |
| A5 Zero-cost private beta | FOUNDATION / GATED | Conservative weighted admission/reserves cover Phone Party and MatchRoom HTTP, socket lifetimes and every authenticated native host mutation; repeated rotation cannot escape accounting. Both retain purpose-HMACed short codes rather than raw values. The real local Worker passes a 64-way exhausted-admission flood plus persisted process restart while preserving established control/close traffic, secure static routes, typed accessible local recovery and a literal-zero kill switch. Protected capacity/refusal/retention telemetry and fixed no-extra-write operation buckets reconstruct admitted units, flag legacy traffic and remain unchanged by forged/refused floods. Static deploy skew and versioned Worker/object boundaries pass locally; a checked Free-plan edge policy isolates `/api/` from static play. Strict privacy-safe provider-v2 reconciliation and v3 seven-day ledger contracts include Durable Object duration as well as requests/storage, and reject billing, drift, discontinuity, incomplete tracking, failed local play, open incidents or a fixed synthetic MatchRoom/Phone Party success-latency miss without reflecting input. The controlled collector uses fresh profiles, production HTTP/WebSocket/WebRTC paths, exclusive aggregate output and exact operated sample counts; its one-attempt real-Worker smoke passes create/join, direct input during signaling loss, same-lease epoch rebind and input after recovery, then correctly remains `STOP`. | Run the 20-attempt collector only after hosted provisioning and extend distributed/network chaos; then hosted edge installation, real provider/version/raw-request reconciliation, named runbook owners, privacy review and seven real contiguous zero-currency days. |

There is deliberately no overall `GO` yet. Local play remains the safe product;
online race admission stays disabled until A3 passes.

The 2026-08-13 browser live-state hardening landed after the last executable
MatchRoom journey. Its source, syntax, focused native compile and actual
Emscripten ABI-v4 link evidence is current;
the new pure Node boundary gate and browser create/join/rotate/expiry/stale/leave
journeys are queued for the next full-suite batch and are not represented as
a fresh pass above.

Role-link capability custody is now fragment-only across one same-origin
redirect. The launcher scrubs before configuration access, never uses web
storage, drops authority immediately in disabled releases, bounds enabled
pre-ROM closure custody to ten minutes, and abandons redemption if either
History API scrub fails. The scrub-failure browser negatives are authored and
join that same evidence batch.

That evidence boundary no longer applies to the 2026-08-13 Phone Party
hardening. The QR-failure, actual-TTL/rotation-generation,
malformed/equivocated state, finite reconnect exact request/media-type and
signaling-envelope, native-command, two-controller targeted-delivery and removal
fixtures have now been **executed and pass**, together with the full service
suite and the native party targets. Each was mutation-tested against the
product source before being trusted: dropping the addressed-controller check,
the receipt-relative TTL clamp, the JSON media-type guard, atomic seat release
or the invite-generation check each makes its named fixture fail.

The controller entry now also scrubs its exact query-free fragment before
configuration access, abandons redemption if scrubbing fails, and repairs the
embedded-copy and duplicate-tab flows so they reconstruct the closure-held
private URL only for the explicit gesture/new scrubbed navigation. The current
hardening additionally requires canonical HTTPS service/controller origins
(loopback HTTP only), makes private/public share copy truthful, and replaces
fail-open/force-stolen tab locks with an acknowledged neutral-and-close takeover
plus a hashed expiring broadcast/storage fallback. Phone and host pagehide,
freeze and BFCache restoration now fail neutral and cannot revive a scrubbed
invite or dead control surface. The source and browser arms have now been
executed. Doing so exposed a real defect in exactly the flow this hardening
exists to protect: duplicate-tab takeover called `location.replace` on the
reconstructed invite URL while the current URL was the same page with its
fragment already scrubbed, so it was a same-document navigation — no reload, no
entry code, no scrub, and the capability left sitting in the address bar. Fixed
by comparing `location.href` after the replace, which distinguishes the two
cases because only a same-document navigation updates it synchronously.

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
| 7 | Parallel launcher UX | Shared phrase ceremony is implemented. Refresh native/browser rendered evidence, then bind the proven launcher model to authenticated peer setup without moving invite, compatibility, connection or recovery state into the game; run first-time friend and human assistive-technology evidence. | A4 UX ready | Live adapter preserves the 43-case/27-action fixture contract, derives the displayed phrase and local mismatch from the authenticated epoch transcript, and passes two-person task study plus screen-reader/device review. |
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
| 2 | All-state/failure gallery | AUTOMATED COMPLETE / EVIDENCE REVIEW | All 43 cases cover the complete fake journey, 18 typed failures and six elapsed timeout outcomes with one clear recovery and Play Here preserved. |
| 3 | Native keyboard/gamepad/speech routes | IMPLEMENTED / RENDER EVIDENCE REFRESH | Prior 25-action keyboard/gamepad and 42-case speech evidence passes. Actions 26–27 plus phrase announcement and assertive mismatch recovery are model/source/compiler validated; rerun the expanded desktop gate. Human screen-reader/device review remains. |
| 4 | Browser Online Room binding | IMPLEMENTED / EXECUTABLE + RENDER EVIDENCE REFRESH | Native browser-ABI conformance previously passed all 43 cases, 10 kinds, 18 failures and 27 actions normally and under ASan. The current ABI-v4/browser hardening adds an exact immutable MatchRoom state/identity boundary, private/unknown-field refusal, monotonic/equal-revision publication checks, control-tail/final-state correlation, receipt-relative early invite expiry, leader-only replacement, phase-correlated 21-field projection, fail-atomic public-state rollback without expired-secret resurrection and terminal guest leave after credential revocation. Its exhaustive pure Node gate is authored and syntax-validated; the two focused C targets compile cleanly. Execute them in the dedicated batch. Prior rendered shared-C evidence correlates 42 cases and 25 routes; expanded phrase/27-route/invite-expiry/AX/reflow and the hardened live journey need a rerun. This remains pre-race control evidence, not carrier-bound phrase evidence. |
| 5 | First-time friend usability | AUTOMATED FLOW COMPLETE / NEXT HUMAN UX | Two isolated clean Chrome profiles over the real local Worker pass create/share, wrong-role recovery, `/room/` join, socket outage/retry, distinct selection, Ready/backtrack/re-Ready, accessibility and leave with bounded automation timings. Run the uncoached human create/share/join/ready/cancel/rematch study and close P0/P1 observations. Race admission remains gated. |
| 6 | Phone Party physical release evidence | PARALLEL HUMAN / A1 | Four-phone mixed-source race; iOS/Android scan, chords, rotation, background, loss, haptics and accessibility. Capabilities are erased/revoked and input fails neutral. |
| 7 | Remaining rollback breadth/platform proof | PARALLEL ENGINE / A3 candidate | Item families, wasm/PAL/Windows/Linux/macOS timing, real audio/rumble and persistent lifecycle pass with bounded memory/time and forbidden side effects at zero. |
| 8 | Written A3 decision | HARD GATE | Publish `GO` only with the fingerprinted matrix. `STOP` names the failed boundary. Until GO, no live adapter or service response can admit an online race. |
| 9 | MatchRoom persistence adapter | DONE LOCAL / O1 | Pure TypeScript reducer and SQLite Durable Object complete create/link-or-code join/race/rematch/close locally. Every phase reconstructs exactly; capabilities, codes and credentials are purpose-hashed/scoped; control tail and retention are bounded; hibernated sockets and expiry alarms pass. Live launcher/race admission remains gated. |
| 10 | Authenticated direct/one-hop carrier | FOUNDATION LOCAL / AFTER A3 GO FOR LIVE BINDING | Bounded topology, deterministic direct/one-hop route selection, opaque forwarding admission, direction-specific HKDF and recipient AES-GCM envelopes pass native/browser vectors and negatives. Shared handshake/phrase UI is implemented; real 2–4 endpoint WebRTC, carrier-to-UI phrase binding, partial-NAT and signaling-outage evidence remain gated. |
| 11 | Live preflight/barrier/results binding | PURE CONSENSUS FOUNDATION / AFTER 9–10 FOR LIVE BINDING | The bounded launcher gate now agrees on one descriptor SHA-256 and peer-key transcript, exact epoch/generations, locally verified supported ROM, human phrase confirmation and an admissible channel graph. Typed staged recovery and readiness withdrawal pass; it owns no socket, clock, UI or engine authority. Live effects plug below the proven launcher model only after A3 `GO`; Cancel cannot resurrect load and results/rematch preserve the room. |
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
| LC-09 | IMPLEMENTED / EVIDENCE REFRESH | The launcher renders keyboard/gamepad/touch/phone sources, host approval carries an explicit seat, replacement is labeled, and a reconnecting phone remains neutral without local takeover. Each approved browser phone also has an exact-seat safe-default removal confirmation; cancel sends nothing, confirm focuses the released seat, and another-host removal closes stale confirmation. Target-only terminal state/socket evidence is authored. A physical mixed race and dedicated execution remain. |
| LC-10 | EVIDENCE REVIEW | Bounded queues, 250 ms neutral, lifecycle release, direct-channel failure and reconnect epochs pass automation. Healthy direct channels remain active during signaling-only loss; physical background/network-loss matrix remains. |
| LC-11 | EVIDENCE REVIEW | Bounded optional phone vibration now crosses the reliable channel with capability negotiation and local opt-out; real-device feel and rollback journal confirmation remain. |
| LC-12, LC-13R | PROPOSED P1 | Build only after direct-path acceptance shows fallback value; requires application-layer AEAD and a separate hard reserve. |
| LC-13D | EVIDENCE REVIEW | Two-browser direct proof, independent wasm P1–P4 queues and the local in-game pause/revoke/preserve path pass; add four physical phones in one race plus service-restart, abuse/load and device evidence. |
| LC-14 | DONE LOCAL / EVIDENCE REVIEW | Pinned media-disabled libdatachannel/Mbed TLS transport, authenticated one-WSS service bridge, exact SAS/QR vectors, generation-tagged signaling, bounded callback/packet queues, direct-channel close recovery, control ping watchdog, launcher/overlay lifecycle and release notices compile and pass locally. Physical native/browser recovery remains in LC-16. |
| LC-16 | HUMAN / PROVISIONING | Run the packaged Windows/macOS/Linux and physical-device lifecycle/accessibility matrix; archive firewall-negative, signing/notarization and binary-size evidence before A2 release. |
| LC-15 | READY | Physical browser acceptance, docs and staged A1 rollout. |
| LC-17 | PROPOSED P1 | Remembering, floating stick and tilt are earned enhancements after A1; never expand the initial critical path. |
| LAN-LOCAL (Phase 3) | DONE LOCAL / DEVICE REVIEW | Zero-internet local play: the launcher embeds the controller-page server and an in-process room, phones scan a QR to a plain-`http://<lan-ip>/controller/` origin, redeem over `/party-ws`, and pair over real libdatachannel DTLS with the pure-JS SAS v2 twin. `check_party_lan_e2e.py` drives the whole triangle with no Worker; the Host allowlist refuses DNS-rebinding, an upgraded socket is anonymous until it redeems, and the six-digit fallback throttles on one shared bucket. The card is gated on a reachable LAN address plus staged controller assets and fails closed to an honest reason otherwise; an origin-less build shows the local card but never a cloud card. Trust model in [`../security/multiplayer-threat-model.md`](../security/multiplayer-threat-model.md#lan-mode-local-play-no-internet). Remaining: the real-phone device items in [`../RC_PHONE_PARTY_CHECKLIST.md`](../RC_PHONE_PARTY_CHECKLIST.md). |

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
| ON-00 | EVIDENCE REVIEW | `platform/online/lobby_core.*` and protocol v1 implement the pure launcher reducer. Native C and service TypeScript consume one checked-in 43-command valid/invalid trace and require equivalent results plus canonical phase, epoch, leader, member and seat state after every command. The trace covers all 16 command kinds, supported-ROM compatibility, cancellation, barriers, retry/conflict/CAS precedence, reconnect, leader custody, leave and close. Native and browser also share an exact clean-provenance build/gameplay compatibility vector; dirty, malformed and unsupported inputs fail atomically. |
| ON-00S | EVIDENCE REVIEW | A–E are complete: the reducer enforces per-seat ownership, unique characters, player vehicles, selection revisions, Ready invalidation and a ROM-derived legal mask; `MdkrMatchLaunchDescriptorV1` freezes canonical selections plus manifest into 148 checksummed bytes; V3 copy-owns it through `SessionRuntime` and the engine runtime. A real-ROM launch directly applies the descriptor track/characters/vehicles, and four endpoint processes converge on identical identity with distinct local mappings. Byte, lobby/manifest, envelope, epoch, source-mutation and loaded-ROM mismatch controls reject atomically. The mask remains capability metadata, never a player choice. |
| ON-03 | IN PROGRESS P0 / EXECUTABLE + RENDER EVIDENCE REFRESH | ON-03A/B/C1–C5 automation, ON-03D and the browser MatchRoom control binding are implemented. Native/browser compile the same 43-case ABI-v4 projection with 27 actions. A browser-free native ABI gate previously drove the ABI-v3 43 cases, 10 kinds, 18 failures and 27 actions normally/under ASan; the shipped pure presenter consumes all projections and exhaustively classifies live actions. The current ABI-v4 delta exact-validates/deep-freezes public MatchRoom state plus stable launcher identity; rejects private/unknown fields, impossible phase/seat state, publication regression/equivocation and control-tail/final-authority disagreement; treats fully older HTTP state as a no-op while admitting only the exact in-flight expected/current rotate secret across a newer socket publication; binds invitations to same-origin generation custody; expires secrets early from receipt-relative time without replay extension; exposes leader-only replacement; correlates service phase to the 21-field projection; rolls public state back across false/thrown presentation without resurrecting expired secrets; treats stale-revision actions as unapplied; and makes accepted guest Leave terminal after credential revocation. Its exhaustive Node and browser expiry/race gates are authored and syntax-validated, and focused native targets compile, but execution is queued. Production Check Setup and carrier/race-dependent routes remain visibly locked; phrase comparison and mismatch stay local-only. Prior 25-action rendered keyboard/gamepad/browser evidence remains, while expanded 27-action phrase/invite render/AX and hardened live create/join/rotate/expiry/leave journeys require a rerun. The publisher policy still ships disabled and activation still requires same-origin clean provenance plus a locally verified supported ROM. **NEXT:** execute the pure gate and the browser/render evidence, then uncoached two-person and human screen-reader/device review; authenticated live phrase binding follows the selected carrier. No production online-race action before A3 `GO`. |
| ON-01 | DONE LOCAL / EVIDENCE REVIEW | `services/party/src/match/` adds the bounded protocol/reducer plus a v3 SQLite `MatchRoom`: authenticated actor injection, QR/deep-link and isolated rate-limited code joins, leader-only generation-checked invite rotation, complete two-endpoint race/rematch, 64-result control tail, every-phase reconstruction, hibernated socket recovery, exact retry/conflict/CAS controls, a shared 43-command C/TypeScript parity trace, minimized public projection, corrupt-history rejection, alarm deletion and raw-code storage negatives pass in the 53-test local service suite. A separate 15-unit, lifetime-bounded signaling socket injects sender identity, assigns/replaces endpoint generations, revokes queued work from superseded senders and targets only exact current members; spoof, replay, stale/rolled-back generation, oversized UTF-8/binary, hibernation, no-payload-persistence and protocol negatives pass without weakening the state-only socket. Replacement close custody is serialized with generation assignment, so a stale close cannot announce absence after fresh presence; welcome snapshots also canonicalize overlapping replacements to one highest open generation per endpoint. A source inventory prevents standard sockets, timers, event listeners or outbound WebSockets from disabling idle hibernation. The shared JSON boundary rejects non-object protocol roots as bounded 400s. The parity work aligned stale-revision precedence and restricts service compatibility to the same supported US/PAL ROM enum as native. Complete DO transitions are input-gated across await points; simultaneous Match commands produce one success and one exact stale-revision response, while competing Phone Party approvals can lease a seat only once. A 24,576-command seeded reducer campaign proves deterministic mirror execution, valid bounded state, atomic rejection, exact duplicate receipts and reconstruction parity. Production deployment is not claimed and admission remains false. |
| ON-02 | FOUNDATION LOCAL / LIVE CHANNEL BINDING GATED | `match_peer_graph` admits only mutual 2–4 endpoint graphs of diameter at most two, prefers direct and selects the lowest-id one-hop route with exact epoch/generation checks. `match_peer_forward` separately binds the immediate channel's authenticated source id/generation, authorizes only the named intermediate and deduplicates opaque ciphertext. Ephemeral P-256 identities plus a canonical room/build/ROM/epoch/sorted-peer transcript yield one cross-platform verification phrase. Pinned Mbed TLS and browser WebCrypto share exact transcript, direction/generation-bound HKDF-SHA-256 and AES-256-GCM vectors; a direction-owned monotonic seal window removes caller-selected nonces and rejects concurrent browser seals. Opening requires the full caller-expected direction, so every envelope byte, valid-key forged source, replay, wrong recipient and stale generation fail before output. The isolated Match signaling relay contains broken state/signal sockets and makes replacement generation authoritative only after welcome delivery plus persistence; the dormant same-origin browser adapter passes local auth/generation/hibernation/limit tests without importing under the disabled policy. Shared phrase UI is implemented; binding its value to real 2–4 endpoint browser/native channels remains after written A3 `GO`. No Start/admission path changed. Carrier review findings G-1 and G-2 are closed (2026-08-13). The phrase now uses a two-round key commitment, so an active MITM can no longer grind both sides: the old birthday search found a collision in 19,231 keypairs per side (0.15 s), while the committed round yields 0 successes in 3,000,000 simulated sessions. Derivation mints the seal window itself, in C through a caller-owned keyring and in the browser through an input-keyed registry, so a repeated derive shares one sequence space instead of restarting it. The transcript domain is `-v2` and the envelope version byte is `2`; a v1 envelope fails closed before decryption. Evidence in [`OPERATIONAL_BACKLOG.md`](OPERATIONAL_BACKLOG.md#required-before-the-written-a3-go--carrier-review-findings-2026-08-13) and [`../ref/match-peer-carrier-v1.md`](../ref/match-peer-carrier-v1.md). |
| ON-04 | FOUNDATION LOCAL / LIVE BINDING GATED | `match_preflight` copies one immutable descriptor SHA-256, peer-key transcript and an order-independent digest of the exact directed epoch/generation graph, then requires an authenticated monotonic report from every endpoint. Native/browser share an exact fixed 124-byte `MPF1` report vector and a three-fragment sequence-bound codec that fits the carrier's 64-byte payload; reassembly and final report attribution bind to one separately authenticated peer direction, while the AEAD header authenticates input versus preflight type and keeps one-hop forwarders opaque. Descriptor/transcript/graph disagreement—including signaling route equivocation—unusable route, missing peer, unverified ROM, unconfirmed phrase and unready channels produce deterministic typed recovery; exact retries are idempotent, changed retries conflict, readiness may withdraw, and invalid state fails closed. `READY` is evidence only and cannot start the engine. The shared UI now makes phrase confirmation explicit and repeats it per epoch; live carrier value binding, rendered desktop refresh and human phrase evidence remain after A3 `GO`. |
| ON-05–06 | GATED BY A3 GO | Live loading barriers and recovery integrate only with proven rollback and the qualified carrier. |
| ON-07 | PROPOSED P1 | Encrypted hard-capped fallback is optional and must never consume control reserve. |
| ON-08–11 | PARTIAL LOCAL / GATED BY ALPHA | Admission red-team proves mixed flood refusal, weighted HTTP/socket control reserve, literal-zero stop, protected identifier-free capacity snapshot, bounded refusal writes and finite aggregate retention. Two-release Chromium proves static waiting-worker custody and atomic offline activation. All 16 Worker/object calls now use v1 while four objects retain legacy readers and reject unknown versions pre-storage. Seeded reducer chaos and socket-size attacks pass locally. Provider reconciliation v2 fail-closes on the separate Durable Object duration ceiling; the v3 ledger freezes that dimension with aggregate-only synthetic experience samples and stop thresholds. Its controlled production-path collector passes a deliberately non-qualifying real-Worker smoke including signaling loss and recovery. Hosted provider/version rollback, the real 20-attempt run, distributed/network chaos and beta evidence follow a working alpha. |
| ON-12 | PROPOSED P2 | Mixed couch+online/nearby mode follows A5 and must not complicate the golden path. |

## Security, privacy and $0 controls already enforced

- Controller invite security is 128 random secret bits in a URL fragment; the
  full payload also carries a 128-bit room id. The fragment is erased before
  any request. Each redeem creates a distinct credential; invite rotation
  invalidates the old secret and code.
- Dismissing the in-game setup sheet revokes the displayed invite without
  dropping approved leases. Reopening rotates to a fresh QR/code; a stale
  queued dialog-close event cannot tear down the new room. QR pixels, code and
  URL are erased before revoke I/O, the exact committed generation is applied
  before reopen rotation, and natural expiry uses the same erasure path.
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
- The companion secret-gated health view exposes only thirteen fixed reservation
  buckets plus a legacy counter. It derives weighted units without another
  write, requires exact versioned operation/weight pairs and reports complete,
  partial or invalid tracking. The real restart/flood test requires exact
  20/57 unit reconstruction when the Match signaling canary is included and an
  empty complete literal-zero view. Its additive exact schema is version 2.
- The exact seven-day beta ledger validator re-runs each provider/capacity
  reconciliation and health-weight invariant, requires contiguous UTC days,
  build/deployment digests, successful service-blocked local play, zero open
  incidents and daily/final `GO`. Fixture validation is explicitly not hosted
  evidence.
- “$0” is an admission guarantee, not unlimited availability: the product may
  refuse new pairing/online sessions and difficult NAT paths. Offline
  keyboard/gamepad/touch local play always remains available.

## Local play (no internet): what it protects and trusts

Phase 3 adds local play: a phone-controller room that runs entirely on one LAN
with no cloud Worker, no account and no internet. The launcher embeds the
controller-page server and an in-process room; phones scan a QR to a plain-`http`
origin on this machine's LAN address. The full trust analysis is in
[`../security/multiplayer-threat-model.md`](../security/multiplayer-threat-model.md#lan-mode-local-play-no-internet);
in short:

- **Protected, unchanged from cloud mode.** Pad input and rumble ride WebRTC
  data channels that are DTLS-encrypted regardless of the plain-`http` page, so
  the gameplay channel is encrypted end to end. The pairing phrase (SAS v2)
  commits to both DTLS fingerprints, so a matching phrase means the two people
  verified the pad channel was not swapped — pad-channel MITM is detectable
  exactly as in cloud mode. Fail-closed: the page trusts only its serving origin;
  `/party-ws` upgrades only for a Host in the machine's own allowlist (DNS
  rebinding refused); a socket is anonymous until it redeems; the six-digit code
  throttles on one shared bucket; an origin-less build shows the local card but
  never a cloud card.
- **Not protected.** The integrity of the controller *page* itself. Plain `http`
  cannot authenticate the page's bytes, so a hostile device already on your LAN
  could serve a fake page. Local play trusts your own network — the trust level
  of your printer's or router's `http` admin page. This is a deliberate trade for
  zero-internet play, not a defended boundary.

Proven end to end by `check_party_lan_e2e.py` (native host + embedded server +
in-process room + real headless-Chromium controller over real DTLS, no Worker).
The real-phone acceptance items are in
[`../RC_PHONE_PARTY_CHECKLIST.md`](../RC_PHONE_PARTY_CHECKLIST.md).

## Repeatable evidence commands

Automation windows render hidden or ordered behind the desktop by design (set
`MDKR_TEST_VISIBLE_HEADLESS=1` to watch one), and the release gate is the
complete suite wherever it runs. From the repository root:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
nice -n 15 cmake --build build --parallel 2
env MDKR64_HIDDEN=1 MDKR_AUDIO=0 \
  SDL_MAC_BACKGROUND_APP=1 SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1 \
  CTEST_PARALLEL_LEVEL=1 OMP_NUM_THREADS=1 RAYON_NUM_THREADS=1 \
  VECLIB_MAXIMUM_THREADS=1 \
  ctest --test-dir build --output-on-failure -j1 \
    -LE 'gpu|app_process|browser'
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
python3 tests/check_party_production_config.py
python3 tests/check_party_service_chaos.py
python3 tests/check_party_firewall_negative.py --shell-dir dist/web
python3 tests/check_binary_size_evidence.py
python3 tests/check_party_binary_surface.py --self-test
python3 tools/verify_party_deploy.py --self-test
tools/deploy_party.sh --dry-run
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

What has changed is the cost of that provisioning, not its status. The
production Worker environment, the deploy script and a verifier that pairs a
synthetic phone against a deployed origin are checked in, and the whole path is
exercisable today against a local Worker. Provisioning is now one login, one
command and one verify script — see
[`DEPLOY_PHONE_PARTY.md`](DEPLOY_PHONE_PARTY.md). The tracked configuration
still points at the unresolvable `party.example.invalid`, so an unconfigured
checkout is not deployable, and both the config gate and the deploy script
refuse a placeholder that survived substitution. No account, domain, DNS record
or secret exists, and nothing in this ledger claims otherwise.

The human remainder for Phone Party is now short enough to enumerate; it is
[`docs/RC_PHONE_PARTY_CHECKLIST.md`](../RC_PHONE_PARTY_CHECKLIST.md).
