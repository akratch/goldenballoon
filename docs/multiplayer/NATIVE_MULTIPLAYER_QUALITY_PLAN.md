# Native multiplayer quality and capability program

Owner direction: 2026-09-06. Online means **online multiplayer**, centered on
the native launcher and integrated game experience, not Phone Party or web UI.
The objective is a materially better complete multiplayer product than DKR-R,
not merely another working invite screen or an expanded feature list.

This program adds an explicit improvement backlog; it does not declare the
existing release gates complete, enable gated capabilities, authorize paid
infrastructure or permit deployment. Release requirements are listed in the [release checklist](../RELEASE_CHECKLIST.md).
The larger capabilities below remain future work until implemented and tested.

## Evidence-backed current assessment

- The actual native beta wiring in `platform/app/online_live_wiring.cpp`
  composes the real room, signaling and match adapter; it restricts the
  production shape to two endpoints, one local seat each, retail identities,
  and STUN-only transport. Four-process laboratory rollback evidence does not
  prove that this shipping route supports four online racers.
- `platform/app/ui_online_room.cpp` already has host/join, invite/expiry,
  verification, roster, connection details, selecting/loading/countdown,
  results/rematch and recovery cards. Audit and improve this real route; do not
  build a separate polished demo that bypasses its authority/state machine.
- The existing operational ledger records unresolved membership/recovery and
  route-measurement decisions, including multi-peer disagreement. Those require
  protocol evidence, not changes to reassuring copy or timeout constants alone.
- DKR-R's current public release is 1.0.4. Its README advertises two-player
  online; its detailed online document describes 2–4 peers. Treat that as
  documentation ambiguity, not permission to claim a player-count lead.
  [Release](https://github.com/ThatGuyMcd/DKR-R/releases/tag/Version1.0.4),
  [online documentation](https://github.com/ThatGuyMcd/DKR-R/blob/b0156864562100ce3453ce4b72ba15c8e3125376/docs/ONLINE_MULTIPLAYER.md).

## Dependency cleanup reservation and partial-initialization follow-up

Source review of the pinned libdatachannel 0.24.5 found thread construction and
detach in the implicitly noexcept final-token destructor. A launch refusal can
terminate the process before the app's cleanup-future observer can catch it.
Inline fallback is unsafe: final-token destruction can hold the initialization
mutex or run on a pool thread that cleanup must join.

The reserved-worker base amendment uses `reserved_cleanup_worker.h` through the
tracked MPL `libdatachannel-cleanup-worker.patch`. Two workers and job/future
state are reserved before `doInit()`: one performs cleanup, the other joins it
and then publishes the result. The final-token destructor only signals the
preallocated decision. Launch/detach refusal cancels and joins owned workers;
an unarmed helper cancels its reserved job without calling RTC under the
initialization mutex. The later initialization amendment described below arms
before acquisition so constructor failure instead retires partially owned stages
after releasing that mutex. The publisher touches only job-owned bookkeeping after
publication. This is not a promise that every OS thread has exited by readiness.

An earlier draft using late `set_*_at_thread_exit` publication was rejected in
source review because registration may allocate during shutdown. The integrated
candidate does not use that mechanism. It preserves cleanup exceptions and
waits for cleanup-thread TLS destruction through the real join. The focused
fixture exercises the actual helper, while the takeover source contract binds
the dependency patch's initialization/future/arm order. Configure-time reverse
patch verification also rejects incomplete source-directory overrides without
modifying another worktree's source. Strict C++17 syntax and the optimized and
ASan/UBSan game/fixture builds pass; behavioral/failure-injection and real RTC
runs remain open.

**Initialization follow-up, implemented but unqualified:** the old `doInit()`
marked ready before all starts succeeded. A new source transaction publishes its
retirement future before acquisition, commits ready only on complete success,
and selectively retires owned stages after constructor unwinding releases the
initialization mutex. PollService's strong start guarantee and SCTP's front-loaded
registry allocation support that ownership contract. New epochs are refused
until retirement completes successfully; existing live tokens retain their epoch.
Phone Party construction/registration errors now enter its ordinary recovery path.
The optimized and ASan/UBSan game and shared helper fixtures compile; actual subsystem fault
injection, global recovery and user journeys remain unqualified. Separate noexcept
queued-work allocation failures and internal C startup exits remain open. No hard
DNS/exit deadline or final-package clearance is claimed.
The [work-admission audit](../open-items/native-rtc-work-admission-audit.md)
maps the related dequeue, continuation, teardown, receive and accounting defects.
Its bounded patch/shared-helper candidate now removes callable copying during
dequeue and commits queue accounting only after successful insertion. Optimized
compilation passes; assertions remain unexecuted. RTC-ALLOC-01a/c/d/e remain open.
The dependent prepared-dispatch candidate now implements nonallocating accepted
Processor continuation, typed progress guards, timer/ready ordering and join
accounting under the actual live-epoch/strong-owner contract. Shared-helper and
actual patched-vendor fixtures are registered, not executed. Final optimized,
ASan/UBSan and Windows cross-builds compile/link the game plus four affected
fixtures after a fixture include-path correction, including settled Workshop
reinspection and invalid-height guidance. Windows retains its historical cloud
origin, not final partyless artifact provenance. Exact composed source pins and source-form notice
gates bind this candidate. Generic allocating `noexcept`, SCTP admission refusal
and complete terminal retirement remain open; this is partial -01c progress,
not complete queued-work safety.
The later per-resource retirement candidate now reserves serial stop/deletion
before construction, gates start/stop, preserves exact lower-edge ownership and
seals that edge before queued retirement. Review corrected callback/pending lock
inversion and deferred retired-capture destruction outside the pending lock.
Optimized, ASan/UBSan and Windows cross-builds compile/link the game and both new
actual-vendor fixtures, alongside the prepared dispatcher and Workshop model.
Windows retains its historical cloud origin, not final partyless artifact
provenance; BasisU GCC warnings remain. The 21 new fixture groups have not
executed. Generic admission, SCTP refusal, stop/destructor bodies and real
network/platform journeys remain open in the same work-admission audit; this
does not expand the shipping multiplayer player count or establish a shutdown
deadline.
The separate first-party signaling thread-start candidate restores Idle and
returns the existing refusal instead of leaving a workerless Connecting client.
Its same-client refusal/retry regression compiles in optimized and ASan/UBSan
profiles but has not executed. Subsequent source candidates contain ordinary
factory/worker exceptions, make outbound/correlation admission transactional and
retain terminal failure across reporting/drain refusal. Local credential buffers
now have capacity-before-copy and scoped erasure on unwind. Final optimized,
ASan/UBSan and Windows cross-builds compile/link the game plus five affected
network/peer/queue/signal/LAN fixtures, including the final timeout and callback
capture-retirement follow-ups. New assertions remain unexecuted. The Windows
profile retains a historical cloud origin, not final partyless provenance;
dependency compiler warnings remain. Real fault/recovery and caller-wide
allocation/backpressure qualification remain open. Optional close-event allocation can no
longer prevent mandatory stopping, worker joining and socket-lease release;
the actual-client refusal assertion is compiled, not executed.
The callback-identity tranche now protects generation-scoped state/event commits,
unfinished peer setup and delayed retry/ping verdicts. Completed direct controller
peers retain their intended lifetime across signaling reconnects. Shared-predicate
and source-binding fixtures cover the intended ownership decisions, not actual
RTC scheduling. Early peer construction refusal now has a bounded autonomous
setup-retry candidate: reserve before allocation, retain lifecycle budgets and
offer counts, reject stale completion/retry tokens, and keep exhaustion reporting
pending on allocation refusal. Optimized compilation passes; real failure/retry
and exhaustion UX remain unexecuted. General event delivery under allocation
failure or queue saturation remains a separate requirement.
The [Windows ownership candidate](../open-items/windows-network-lifetime-audit.md)
uses checked shared leases for sockets, LAN enumeration/listeners/aliases and
abandoned resolver work, with cleanup failure included in the terminal verdict.
OS failure injection, cold Windows phone hosting and package acceptance remain
open. LAN post-accept/handler admission now has its own syntax-checked candidate:
staged fd/registry/thread ownership, sealed common finalization, nonblocking
listener, allocation-free reaping, checked mandatory timeouts and callback-only
stop with marker lifetime through capture retirement. Six actual
server regression groups remain unexecuted; exceptional retained join ownership
records failed cleanup. The lease helper alone does not establish these paths.
The [initialization failure audit](../open-items/native-rtc-initialization-audit.md)
records the four confirmed findings, shipped-subsystem ownership matrix,
Phone Party constructor-error gap, transaction proposal and required evidence.

## First source findings

Findings are source observations, not rendered-device verdicts.

- `platform/app/ui_online_room.cpp:977` — P1 source correction implemented:
  join-code editing previously hid native text/caret and painted a grouped
  overlay with different hit-test positions. The real field now uses native
  `InputTextWithHint` text, caret, selection and scrolling; grouping stays on
  the read-only host invite. Six-digit admission, paste filtering, leading
  zeroes and post-edit spoken guidance are unchanged. Native optimized build
  passed. The production helper in `online_join_code_input.h` is now shared by
  the launcher and a registered `online_join_code_input` regression fixture.
  Its CPU-only ImGui core receives real input events and checks text, caret
  and selection draw geometry, middle edits, undo/redo, filtered/oversized paste
  and narrow scrolling at two widget scales. Strict syntax, native optimized
  and ASan/UBSan compilation pass; the standalone widget target also
  cross-compiles for Windows. The new assertions have not executed. This is not
  platform input, theme/font, gamepad, screen-reader or packaged visual proof.
  Cover leading zeroes, middle edits,
  selection replacement, pasted separators, 200%/narrow layout and gamepad
  keyboard entry; capture the actual native widget, not only a staged card.
- `platform/app/online_live_wiring.cpp:4` — corrected stale comment claiming
  the native beta never ships. Release profiles enable it; documentation must
  distinguish development default, shipping beta and local-only web. No runtime
  gate changed.
- `platform/app/online_live_wiring.cpp:14` — capability boundary, not a defect:
  two endpoints/one seat and no relay. Multi-peer or mixed couch/online support
  needs rekey, membership, authority, launch and disconnect qualification before
  exposing more seats. Removing a limit is not an implementation.
- `docs/multiplayer/OPERATIONAL_BACKLOG.md:320` — unresolved recovery and route
  quality decisions must be reconciled with current production source and tests.
  Preserve exact healthy-signaling-loss vs real-peer-loss distinctions.
- `platform/app/ui_online_room.cpp:2347` — P1 recovery correction implemented,
  behavioral qualification pending: a failed view composition previously drew
  advice to leave without an action; its zero view-kind sentinel also disabled
  takeover while the adapter remained owned. The shared
  `online_room_takeover_policy.h` now keeps takeover for an owned adapter with
  an unavailable view, and the unavailable panel requests the existing deferred
  Leave Room teardown without dispatching through the rejected view. An owned
  but uninitialized adapter gets the same recovery panel, not another chooser.
  The genuinely idle and known-entry chooser states retain the normal shell.
  The registered `online_room_takeover_policy` fixture has compile-time
  assertions for ownership, missing views, all active kinds and release; these
  assertions pass compilation. Optimized and ASan/UBSan native game/policy
  builds and the Windows game/policy cross-build pass; upstream dependency
  warnings remain recorded, not suppressed or cleared by the build. The
  existing takeover checker adds structural
  bindings to the production policy, failed-view early return and deferred
  teardown ordering. Qualification must still exercise a failed view at frame
  start and after service, actual leave/registry cleanup, focus and next-room
  recovery. No invalid-view runtime reproduction or rendered pass is claimed.
  The fixture now additionally implements the real `IMdkrOnlineAdapter`
  interface and exercises the shared production `OnlineRoom_readViewKind`
  boundary, including read-only querying and failures before/after service.
  Those runtime assertions compile but have not run; a stub's release counter
  does not prove real mesh, registry or worker teardown.
- `platform/app/ui_online_room.cpp:2405` — related stale-frame correction
  implemented, behavioral qualification pending: `drawBetaRoom` previously
  retained the original view/lobby snapshot after a successful invite-card
  adapter rebuild; its null guard detected refusal but not replacement. Invite
  and stranded-room cards now report frame invalidation after closing their UI
  scopes, regardless of whether reconstruction succeeds. Selecting propagates
  that result; the live room also ends the frame after timeout, primary,
  secondary or cancel actions. The next frame recomposes from current state,
  without relying on an adapter-address comparison. The takeover checker pins
  these barriers structurally. Successful/refused rebuilds, same-frame controls,
  re-entry, focused action feedback and balanced rendered scopes still need
  behavioral coverage. No user-visible runtime reproduction is claimed.
- `platform/app/ui_online_room.cpp` (`drawBetaPhraseDecision`) — the related
  review also found in-draw dispatch on the word-comparison screen, followed by
  controls and details from its previous view. The candidate now gathers at
  most one enabled decision, closes its UI scopes, dispatches once and ends
  the caller's frame. The real phrase model currently enables both decisions;
  honoring its enabled flags is defensive consistency, not evidence of a
  verification bypass. Source contracts cover dispatch ordering, single-decision
  gating and the live caller's refresh boundary. Protocol and phrase verification
  semantics are unchanged. Integrated optimized and ASan/UBSan native builds
  and Python 3.10 AST parsing pass; new source assertions have not run.
  Confirm/mismatch/cancel, rejected dispatch, disabled
  fixture controls and next-frame feedback still need behavioral qualification.
  The side-by-side decision layout also remains subject to narrow/200% and
  controller-focus observation; source review does not certify its containment.

The first UI review used the Web Interface Guidelines' transferable interaction
principles: visible focus/edit state and actionable errors. Native ImGui does
not acquire browser ARIA semantics; app speech is not a screen-reader tree.
The full native user-story/code-path audit below remains work, not an implied
pass for files that have only been inventoried.

### Shutdown lifecycle finding

`platform/app/ui_online_room.cpp` (`OnlineRoom_shutdownForAppExit`) — P1
source-confirmed lifetime gap: after its 10-second wait, the old code detached
unfinished retirement workers and returned toward static destruction. Those
workers still accessed the static tracker and could still depend on network
globals. The candidate uses shared `online_teardown_tracker.h` drain logic:
warn after the diagnostic deadline, but join every worker before returning.
No detached-worker or process-global lifetime clearance is inferred from elapsed
time. The new `online_teardown_tracker` fixture exercises empty/completed drains,
two deliberately held ordinary workers, the timeout warning, actual completion
before return and a repeated drain. It uses synchronization rather than sleeps,
no network or app, and remains an executable test requiring authorized execution.
The takeover source contract binds shutdown to this shared implementation.
Strict C++17 syntax, Python 3.10 parsing, optimized and ASan/UBSan native
game/fixture compilation pass. The registered assertions have not executed.
The Windows game and tracker fixture also cross-compile; this does not qualify
Windows exit behavior, and existing BasisU GCC warnings remain recorded.

This correction is not bounded, responsive shutdown for a permanently stalled
transport. The existing panel-owned adapter is also destroyed synchronously at
app exit, so the old implementation never established a total exit-time bound.
Remaining work includes cooperative cancellation/close deadlines through actual
transport ownership, visible closing progress, quit during room retirement,
normal and delayed completion, and safe host/global teardown ordering.

The follow-up source audit confirmed two scheduling failure paths: thread
construction could strand the live count, and growing the handle vector after
launch could destroy an unrecorded joinable thread. The shared retirement
candidate stores a stable record before launching and retains adapter ownership
until launch succeeds. Scheduling refusal rolls back bookkeeping and destroys
the adapter synchronously; this preserves lifetime, not UI responsiveness under
resource exhaustion. Individual completed handles are joined/reaped before a
later retirement even if a different transport remains stalled. New fixture
assertions cover thread-launch refusal, exact-once inline cleanup, subsequent
recovery and sixteen completed retirements alongside one held destructor.
Storage-allocation refusal still needs fault-injection evidence; all new runtime
assertions remain unexecuted. Strict C++17 syntax and Python AST parsing pass;
optimized, ASan/UBSan and Windows game/fixture builds also pass for this follow-up.
The next candidate integrates normal Quit with a progress-only native surface:
the cancellable Workshop phase completes first, then both engine registries
retract and the adapter retires. Each event-loop iteration polls and reaps only
completed workers; the window keeps pumping while a normal transport closes.
The closing surface cannot start jobs, rooms or races; the panel also refuses
reinitialization. Same-frame Quit suppresses both engine handoffs before polling.
After ten seconds the copy explicitly reports delayed cleanup without inventing
a percentage or deadline. App-spoken section status changes with it. Actual
transport cancellation is still required to bound a permanently stalled close;
allocation/launch refusal retains synchronous cleanup, and renderer-failure exits
retain the final ordered join. Polling assertions and source bindings are added;
rendered, real-transport and Workshop/quit interaction acceptance remains open.

Independent review also found a pending-preview edge: a window-close event
could precede `Launcher::draw` publishing the returned preview, while no worker
was yet busy. Publication now lives in non-rendering launcher servicing before
quit/idle checks, including zero-drawable windows; quit readiness also retains
the pending-result guard. Exceptional exits settle existing Workshop results
without drawing before global owners are destroyed. Source bindings are added;
preview-return/close, minimize and failed-renderer execution remains required.

The transport trace found that HTTP restored blocking sockets after TCP connect
and entered TLS handshake without bounded cancellation. Follow-up review caught
two deficiencies in the initial amendment: TLS timeout errors are fatal under
the pinned library contract, and one SSL operation can perform several socket
calls without returning to an outer deadline check. The current candidate uses
nonblocking BIO callbacks that check cancellation/deadlines before every socket
call, returns WANT_READ/WRITE for ordinary retry, preserves pending TLS write
arguments, and forbids context reuse after fatal I/O. Readiness waits respect
the remaining budget; close notification is best-effort/nonblocking. Three
ordinary silent-loopback cases cover TLS cancellation, HTTP response cancellation
and connect deadline expiry. Successful trusted TLS, fragmentation, backpressure
and all new assertions remain unqualified. Certificate verification, production
budgets and endpoint requirements remain unchanged.

The related transport sweep additionally found that Linux room writes could
raise SIGPIPE on an ordinary disconnected peer. Both plaintext and TLS now use
the same per-send suppression already used by the signaling client. An isolated
child regression restores the default signal disposition so incidental library
configuration cannot conceal the old failure. Source contracts cover both send
routes and the TLS I/O rules. These assertions are written, not executed.

This is not complete network shutdown qualification. Existing detached DNS
helpers capture copied names and shared-owned result state; inspection did not
establish the original tracker lifetime bug there. They still lack OS lookup
cancellation. The new shared first-party budget limits actual outstanding lookup
work to eight across both room and signaling clients, including abandoned
callers. This is not a connection or player limit. Capacity waits respect the
existing cancellation/deadline; task permits survive address-result and
synchronization cleanup. Address chains now use RAII ownership, copied inputs
are prepared before worker launch, and allocation/startup failures return an
ordinary resolution failure. An already-abandoned worker skips its OS lookup.
Detach failure retains the joinable handle and joins as an exceptional fallback;
that fallback is lifetime-safe, not guaranteed responsive. Pinned libdatachannel
closes transports asynchronously, and libjuice can join an uncancellable DNS
lookup. The all-owner cleanup integration now retires both the panel room and
the launcher's phone host/transport, including owners behind an invisible phone
surface. Borrowed launcher/overlay aliases are cleared before background
retirement; the host retains its transport through destruction. Only after both
retirement groups have reaped AND first-party resolver work has released its
results does the launcher begin/poll `rtc::Cleanup()`. This prevents its socket
library teardown from racing a still-owned first-party OS lookup.
The no-RTC build returns an explicitly ready future instead. The shared
`AppCleanupCompletion` helper gates startup, starts once, polls without executing
deferred work, and distinguishes exceptional/invalid/deferred completion from
success. Its promise-based assertions are added but unexecuted. Terminal-path
integration and exact-package behavior still require qualification. Do not
claim a hard exit deadline without addressing the resolver dependency.

The integration now routes all 59 post-launcher terminal host-shutdown calls
through a shared helper: Workshop completion, alias retraction, network cleanup,
pad release, then host shutdown. The two startup failures before launcher
construction remain direct. Normal Quit polls in every build profile; smoke,
autoplay and normal/restart exits propagate cleanup failure and refuse relaunch.
The terminal-path sweep also fixed an independent diagnostic cloud-session leak
on invalid pacing input. Repeated finished cleanup does not revisit process
globals after host/log shutdown. An exceptional RTC future still does not prove
library teardown succeeded; it is logged as a failure, never a safe-close claim.

Integrated native optimized and ASan/UBSan-instrumented game, transport and
completion-fixture builds pass, including the latest Workshop selector changes.
The first optimized attempt caught a macOS signal-macro portability error in the
new fixture; it was corrected and both profiles rebuilt successfully. The
dependency-free no-RTC factory passes strict C++17 syntax. CMake registers the
completion fixture, TLS source contract and a source-only takeover contract.
Python 3.10 AST and whitespace checks pass. No new assertions, sanitizer runtime,
app/network scenario or final Windows/Linux package behavior has executed.

The subsequent resolver tranche adds a two-translation-unit budget fixture for
capacity, contention, moves, failure unwinding and owner-independent lifetime.
Production transport fixtures additionally exercise capacity-wait cancellation,
deadline expiry without starting a lookup, next-room recovery and a permit held
after caller abandonment. Both-client structural contracts bind admission and
result ownership to production; the launcher contract includes resolver retirement.
These assertions are added but unexecuted. No OS cancellation or complete DNS
shutdown deadline is claimed, and library-internal resolver work is separately
owned by RTC rather than this first-party budget.
The resolver-integrated native optimized and ASan/UBSan game, transport and
budget-fixture builds pass. This is compilation/linking evidence only: saturation,
abandonment, recovery, fault-injection and shutdown assertions remain unexecuted.
Independent review additionally caught a first-Quit allocation gap: asking the
lazy resolver pool for its count could allocate before any DNS had run, outside
the cleanup observer's exception boundary. Shutdown now uses a nonallocating
published-pool observation. Publication follows successful construction and is
retracted before destruction; observing zero is a cleanup barrier only after
all producer owners retire. Pre-initialization and cross-translation-unit
observation/lifetime assertions are added, not executed.

Do not restore detachment or force process termination as a
substitute for that lifecycle work. The owned-invalid-view rendered/registry
cleanup fixture remains independently required.

## Complete player journey and required evidence

| Journey | Quality/fix requirements | Completion evidence |
|---|---|---|
| Discover multiplayer | One obvious native entry; explain supported modes and requirements before work is lost; local play always available | First-use, no-ROM, offline, unavailable-service, incompatible-build observations |
| Create and invite | Useful progress/cancel, readable code, copy/share confirmation, privacy and expiry, no leaked capability in logs | Real host journey plus cancellation, expiry/rotation and redaction controls |
| Join | Effortless code entry/paste, visible caret/selection, actionable inline errors, first-error focus | Actual keyboard/controller entry, leading zeroes, corrections, invalid/expired/full rooms, narrow/200% |
| Verify and admit | Clear peer identity and safety-word comparison; no accidental approval or coercive timeout | Both displays, mismatched words, retry and stale-approval controls |
| Lobby ownership | Clear host/guest roles, seats and controllers; meaningful readiness; all peers see canonical selections | Real multi-process roster, duplicate input ownership, reconnect, stale state and host-rule changes |
| Choose content/rules | Fluent character/vehicle/track selection; expose only compatible choices; understandable custom-content restrictions | Complete supported selection matrix, mismatches, disabled choices and controller navigation |
| Connection check | Show latency/jitter/loss and a comprehensible route assessment; do not label unmeasured routes good | Controlled LAN/WAN-like profiles, failed/direct-impossible routes, cancellation and telemetry correctness |
| Ready/load/countdown | Visible peer progress and clear waits; responsive UI/audio; bounded cancellation/recovery | Slow peer, different load times, readiness revocation, disconnect during every barrier |
| Race | Low local input latency, smooth presentation and equal gameplay authority; unobtrusive useful status | Deterministic convergence plus measured input-to-display latency, pacing, rollback and packet profiles on real devices |
| In-game interface | Native-consistent settings, player list, input reassignment and safe leave flow; never steal authority from gameplay | Both peers, every pause owner, controller loss, focus return, reduced motion and 200% |
| Results/rematch | Legible results, distinct rematch/new-track/leave choices, retained group and settings, no boot/menu trap | Repeated race/results/rematch epochs, split decisions, stale results and disconnect at each transition |
| Recovery | Differentiate service wobble, peer loss, temporary impairment and terminal failure; preserve valid sessions and neutralize stale input | Delayed/lost/out-of-order delivery, suspend/resume, wobble-then-quit, multi-peer agreement and no duplicate progression |
| Return later | Remember safe preferences without reviving expired authority; clean teardown and a quick next-session path | Quit/relaunch, interrupted shutdown, offline start, privacy/redaction and fresh invitation |
| Inclusive polish | Cohesive spacing/typography, responsive layout, visible focus, meaningful status, optional restrained motion, full non-pointer routes | Uncoached players plus mouse/keyboard/controller, narrow/200%, contrast, reduced-motion and honest assistive-tech matrix |

For every state/action, map UI -> shared view model -> adapter command ->
authenticated room/transport event -> immutable launch/engine state -> visible
result. Record allowed actions, disabled reason, persistence, cancellation,
timeout and recovery ownership. Fix duplicate state machines or dead-end
handoffs instead of decorating them. Use the existing native UI/state contracts
and add real-path negative controls wherever the audit finds a gap.

## What the requested "WOW" experience must demonstrate

This is a native product acceptance brief, not a separate web interface or a
promise that a visual refresh alone will achieve it. Build on the real launcher
and game routes; presentation must never invent readiness or connection quality.

| Experience | Observable acceptance target |
|---|---|
| Friends reach the first race without coaching | Host and guest can discover online play, invite, correct a code, verify, choose racers and start using the interface's own guidance. Measure completion rate, time, backtracks and assistance; retain failed attempts. |
| One continuous session | Launcher, native selection, loading, racing, results and rematch preserve group identity and input ownership. No redundant setup, conflicting instructions or unexplained return to a menu. |
| Premium visual and interaction craft | Consistent type, spacing, player identity, focus and feedback across launcher and game. At narrow and 200% layouts, essential actions and progress remain readable and reachable. Motion is interruptible and respects the user's motion preference. |
| Controller-first, not controller-tolerant | Complete host/join/edit/verify/choose/race/rematch/leave using supported controller input; disconnect and reconnect a controller without losing the room or trapping focus. Verify real code entry, not just button traversal. |
| Connection confidence | Explain what is happening, whose action is required and how to recover. Measured latency/jitter/loss can inform status; a working signaling connection alone must not be called a healthy race route. |
| Graceful interruption | Cancel during setup, service loss, peer loss, suspend/resume and shutdown have explicit outcomes. No stale input, duplicate progression, misleading success or abandoned live session behind local play. |
| Delighted repeat players | Repeated races and new-track choices retain the group and safe preferences. Reinvite, reconnect and richer social features count only once their actual shipping routes are implemented and qualified. |

Before a comparative usability study, record the same task script, machines,
controllers, network profiles and measurement definitions for both products.
Choose numeric success/latency budgets before collecting results; do not select
them afterward to manufacture a lead. A current-source functional pass and
uncoached human experience review are separate requirements.

Delivery slices retain the full ambition while making completion reviewable:

1. **Release foundation:** fix current correctness/security/recovery defects and
   qualify the shipping two-player flow, including real native editing and
   controller paths. This is not the claimed competitive end state.
2. **Native experience:** complete the 14 journeys above, unify launcher/game
   presentation, and validate the end-to-end session with uncoached players.
3. **Broader play:** four-player/mixed-seat, cross-platform, relay and reconnect
   work with protocol, determinism, ownership and operations evidence before
   exposure in the UI.
4. **Distinctive features:** progression sessions, playlists, custom appearance,
   spectating/replays and social capabilities with explicit privacy, fairness
   and operating decisions. Keep these visible; do not advertise them as shipped.

## Capability expansion to evaluate and build deliberately

These are explicit modern multiplayer workstreams, not assertions that they
already exist or that all belong in the next release. The maintainer's broad
direction does not select an infrastructure budget or authorize publication.

1. Four online racers and mixed local/remote seats, with unanimous membership,
   full rekey/custody, stable identities and real 3P/4P end-to-end coverage.
2. Cross-platform and cross-architecture play only after deterministic
   protocol/build/ROM compatibility and Windows/macOS/Linux matrix evidence.
3. Relay fallback for difficult networks: selected service, explicit privacy,
   abuse/admission controls, capacity/cost policy and approved provisioning.
   Do not silently remove the current STUN-only/zero-cost safeguards.
4. Private friends/recent-party/reinvite workflows without exposing capabilities
   or requiring accounts gratuitously; decide account/presence requirements.
5. Better reconnect/rejoin and host migration where game authority permits;
   explicit state transfer, agreement, expiration and failure behavior.
6. Shared Adventure/trophies/challenges and tournament/playlist sessions with
   clear progression ownership, boss/solo exceptions, save custody and fairness.
7. Rich rules and custom visual identities with compatible peers, exact content
   identity and rights-aware distribution. Separate visuals from negotiated
   gameplay changes; no hidden donor/physics mismatch.
8. Spectating, replay/ghost sharing and post-race insights with bounded storage,
   immutable race identity, privacy and no effect on race authority.
9. Optional social communication, public discovery/matchmaking, skill/ranking
   systems: explicit moderation/privacy/abuse and operating requirements first.
   These change the current invite-only v1 contract and need a documented
   product decision, not accidental activation.
10. Controller excellence, native per-vehicle/gyro options, configurable HUD,
    premium audio/status feedback and accessible focus/input recovery across
    the launcher and game, with physical-device evidence.

## Delivery order and bar for claiming superiority

First close current release correctness/security blockers and the source-backed
join/edit/recovery defects. In parallel, audit the complete real native journey
and prototype the coherent visual language within production components. Then
deliver capability slices with full happy/negative/recovery/device evidence.
Do not replace the requested ambition with a smaller two-player demo; keep the
larger backlog explicit while controlling each release's advertised scope.

A credible comparison requires the same machines/controllers/network profiles,
time-to-first-race, success/recovery rates, input latency, pacing, repeated
rematches, readable failure handling and uncoached usability. Feature count,
screenshots, laboratory four-process tests and code volume do not establish
superiority. No head-to-head performance/UX verdict has yet been measured.
