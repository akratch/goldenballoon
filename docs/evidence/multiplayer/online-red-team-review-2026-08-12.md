# Online multiplayer overnight red-team review — updated 2026-08-13

Decision boundary: **code review ready; production online race remains STOP**.

This review record covers the locally implemented launcher/service/carrier foundation.
It does not authorize deployment, paid infrastructure, public matchmaking or an
Online Race Start path. A finding is useful even when no exploit is found:
record the exact invariant inspected, evidence used, and any scope that could
not be exercised.

## Workstation safety

Do not run any test runner or any file under `tests/` on the occupied maintainer
desktop, including Python/Node/service tests described as static, pure or
headless. Local work is limited to source inspection, syntax/AST/type checks and
bounded low-priority compilation that does not execute its outputs. On a
dedicated desktop, compiled tests require `--with-compiled-tests`; native game/renderer
roles require `--with-app-tests`; native
GPU/window CTests are absent unless CMake is configured with
`-DMDKR_ENABLE_GPU_TESTS=ON`; real Chromium creation refuses to proceed unless
the runner receives `--with-browser-tests`. Those flags are insufficient by
themselves: the caller must also supply `MDKR_DEDICATED_TEST_DESKTOP=1`, which
the runner and CMake never manufacture. All opt-ins belong on a dedicated test
desktop only.
The application itself now rejects hidden/smoke/autoplay/file-dialog automation
before SDL/Cocoa unless the launch carries both `MDKR_APP_TESTS_ALLOWED=1` and
the independent desktop attestation. Local CTest also excludes every
`app_process` and `browser` label. On macOS, SDL background/no-activation hints
and AppKit accessory policy are installed before `SDL_Init`, rather than after
window creation. The service package also pins Vitest to one worker by default.
Do not remove `MDKR64_HIDDEN`, activate an application, use `open`, or invoke a
test by binary name merely because its CTest label is unknown.

`tools/run_checks.py` also lowers its inherited CPU priority, enters inherited
Darwin background scheduling on macOS, caps nested build
and library concurrency, exports SDL's macOS background/no-activation hints,
and serializes every role that can start the game, a renderer or a browser even
under `--jobs`. Do not work around those controls for an overnight pass on an
occupied desktop.
Self-contained Chrome/Worker checks use the explicit `browser_local` role; the
manifest scans browser-launch markers and refuses to start if a future check is
accidentally assigned to a parallel-safe role.

The following executable review commands belong only on an isolated desktop;
they are not safe commands for the occupied maintainer workstation:

```bash
export MDKR_DEDICATED_TEST_DESKTOP=1
(cd services/party && npm run check)
node tests/test_match_peer_crypto_js.mjs
node tests/test_match_preflight_js.mjs
node tests/test_match_signal_client_js.mjs
python3 tests/check_party_internal_api.py
python3 tests/test_party_usage_reconciliation.py
python3 tests/test_party_beta_ledger.py
python3 tests/check_ci_contract.py
python3 tests/check_app_background_activation.py
env -u MDKR_APP_TESTS_ALLOWED -u MDKR_BROWSER_TESTS_ALLOWED \
  MDKR64_HIDDEN=1 MDKR_AUDIO=0 \
  SDL_MAC_BACKGROUND_APP=1 SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1 \
  ctest --test-dir build --output-on-failure -j1 \
  -LE 'gpu|app_process|browser' \
  -R '^(match_peer_|match_preflight|match_signal_client_js|online_compatibility_identity|online_compatibility_source_contract|online_lobby_(core|view_model|fake_adapter|browser_wasm)|multiplayer_boundaries)'
```

## 2026-08-13 live-state delta

The browser binding now uses ABI v4. A three-state launcher invite projection
distinguishes preparing, usable and leader-replaceable custody. Invite secrets
expire from a conservative receipt-relative deadline (5%, capped at 30 seconds,
early), so a skewed display clock cannot keep a QR alive. Expiry removes the
link, code and QR from launcher custody and the DOM before rendering; projection rollback cannot
resurrect them. The leader gets one **New Invitation** recovery using the current
generation, while guests retain Connection Details and never rotation custody.

The role-specific `/room/` entry no longer stages the raw capability in
`sessionStorage`, which could outlive the handoff when the checked-in release
policy was disabled. It redirects with fragment custody only; the always-loaded
launcher captures exact syntax into closure memory and scrubs the address
before even consulting launcher configuration, policy, model, ROM or network
state. Disabled builds destroy it and show a specific local-first recovery.
Enabled builds may hold it only for the bounded ten-minute invite window while
a ROM is chosen; expiry erases it. Either History API scrub failure triggers a
clean navigation and abandons redemption rather than letting the current task
continue with the secret. The dedicated activation/browser fixture now checks
empty hash, absent storage, zero API I/O under the disabled policy, fail-closed
role/root scrub denial and one exact join under the enabled fixture.

The exact state boundary additionally rejects revision, epoch, leader-generation
and invite-generation regression; equal-revision lobby/control equivocation;
non-monotonic control epochs; and a control tail whose final revision, epoch,
leader, selected track or vehicle mask differs from the enclosing lobby. The
pure test covers clock skew, local-deadline overflow, malformed retained custody,
expiry, regression and equivocation. It now classifies a fully older valid
publication as an authenticated no-op while rejecting mixed regression and
advancement. A delayed rotate response may supply only the exact in-flight
expected/current generation's secret after a newer membership publication, but cannot roll that
membership back, restore an older generation or retain custody after leadership
or phase loss. The real-browser fixture now has publication-driven,
timer-driven and membership-versus-rotation race arms.

Current evidence is deliberately limited to JS/Python syntax, diff/static
contracts and a low-priority compile of
`mdkr_online_lobby_view_model_test` and
`mdkr_online_lobby_browser_wasm_test`. The actual small Emscripten ABI-v4
`mdkr_online_tools` target also links; its rebuild exposed and fixed an
Emscripten CMake generator-expression reference to the absent native harness.
No executable or browser test ran on the occupied workstation. Queue the
following for an isolated desktop:

The final custody follow-up rechecked syntax for every changed JavaScript/ES
module, parsed all 29 changed Python files as AST, checked every changed shell
script with `bash -n`, reran the Party TypeScript no-emit typecheck and passed
`git diff --check`. It launched no product, test runner, browser or emulator.

```bash
MDKR_DEDICATED_TEST_DESKTOP=1 node tests/test_online_room_live_state.mjs
MDKR_DEDICATED_TEST_DESKTOP=1 ctest --test-dir build -j1 --output-on-failure \
  -R 'online_room_live_state_js|online_lobby_view_model|online_lobby_browser_wasm'
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_BROWSER_TESTS_ALLOWED=1 \
  python3 tests/check_browser_online_match_room.py --shell-dir dist/web
```

The broad CTest command above must retain `-LE 'gpu|app_process|browser'` even
on an isolated desktop unless the reviewer is deliberately running a separately
attested UI lane. On an occupied workstation, do not run any command in either
block; inspect sources and syntax only.

## 2026-08-13 Phone Party delta

The browser display and phone now use ten-second, 16 KiB streaming response
boundaries; exact create/redeem/rotate schemas; actual room-object invite
lifetimes; strict same-origin controller URLs; 64 KiB UTF-8 socket caps; exact
monotonic public state; and same-transition fingerprint rejection. Both socket
owners reject stale-generation callbacks, contain synchronous setup/listener/
send failures, stop after five consecutive or short-lived retries and reset
only after 30 stable seconds. Healthy direct DataChannels continue with a
truthful Limited status; without them, the phone neutralizes and offers **Try
now**. QR/canvas failure hides the false image while keeping `/controller/` and
the accessible six-digit code usable.

Controller entry now captures and scrubs the exact query-free fragment before
reading fixture/configuration globals. It abandons redemption through a clean
`/controller/` navigation if the History API refuses the scrub and never writes
the capability to web storage. Embedded/unsupported **Share private link** with
**Copy private link** fallback and
duplicate-tab **Use this tab** paths no longer copy/reload the already-clean
address: they reconstruct the closure-held private URL only for the explicit
clipboard or acknowledged takeover navigation, and the destination immediately scrubs
again. Without a capability, recovery shares exact public `/controller/` and
requires the current code instead of presenting a dead private-link action. The
dedicated controller fixture has authored private/public copy, reclaim and
scrub-denial arms.

The 2026-08-13 follow-up removes force-stealing from that claim: takeover now
broadcasts an explicit neutral/transport-close request, waits, and takes an
ordinary exclusive Web Lock; rejection refuses rather than falling through to
a competing owner. A no-Web-Locks broadcast election uses only a hashed,
15-second local heartbeat. Production Party origin configuration and browser
service/controller URLs are canonical HTTPS and same-origin, with HTTP limited
to loopback fixtures. These new origin/lease negatives are authored and
syntax/type validated but have not been executed on the occupied workstation.
The same pending browser batch now includes phone BFCache clean reload and host
pagehide/BFCache secret, peer and remote-pad cleanup; neither lifecycle may
resurrect a scrubbed invite or publisher.

Dismissal and countdown expiry now erase the Phone Party QR bitmap, code and
controller URL from launcher/DOM custody immediately while retaining approved
leases. Revoke returns the exact committed transition/generation, and reopen
waits for that correlation before rotation; repeated close/reopen is covered by
a stateful browser fixture. Stale revoke completion is scoped to its old room.
PartyRoom socket publication is also post-commit best effort: send, close and
attachment-deserialization failures are contained per peer, so one broken
hibernated socket cannot manufacture an HTTP failure or stop another recipient.
MatchRoom now applies the same containment to every remaining signaling close,
socket setup and attachment update rather than letting a platform exception
escape its hibernated event.
Native host commands now enter the same object-wide input gate as HTTP room
mutations in addition to their local socket tail, closing the budget/directory
await race where one transport could otherwise persist a stale predecessor.
The launcher's **Start local game** action now performs the same immediate
invite erasure and correlated revoke before clicking Play. Controller-room
authority remains `open | closed`; gameplay phase is deliberately launcher-
owned rather than patched into the service or game.
Phone invite rotation now captures request predecessor G and accepts its secret
response only for G+1 while public authority is exactly G or an already-socket-
published G+1. The stateful browser fixture deliberately publishes G+1 before
returning HTTP, while repeated revoke/reopen expects predecessors 1/3/5/7.
Every published generation change first erases any old displayed QR/code/URL,
so a second authenticated host cannot make another tab relabel stale custody.
All Worker JSON and raw proxy request bodies now use a streaming 16/64 KiB
reader with early cancel, strict `Content-Length` and fatal UTF-8; no service
route retains the prior unbounded `request.arrayBuffer()` aggregation.
Every public Phone Party and MatchRoom request now also requires its exact
per-action key set, including exact nested compatibility. Unknown fields cannot
be silently ignored, forwarded to Durable Objects or mistaken for a staged
future capability; authored service negatives cover create, link/code join,
state, rotate, command, redeem and host control without authority mutation.
Every public body, including Match commands, requires `application/json` at the
Worker edge before parsing or proxying; a wrong/missing/JSONP type cannot be
rewritten into an internally trusted JSON request.

The PartyRoom no longer broadcasts a host offer or ICE candidate to every
phone. It deserializes each authenticated controller attachment and sends only
to the exact `to` controller id; controller identity remains injected from its
own attachment. Create/rotate responses now return the committed generation and
actual positive lifetime remaining after room mutation.
It now also reconstructs exact role-specific hello/offer/answer/ICE envelopes,
including exact nested SDP and declared optional bounded candidate fields,
before relay. Null, primitive, array, extra-field, wrong-role and invalid-
generation messages close without fanout. Authenticated native host commands
use exact action shapes and reject ambiguity before cost reservation or room
mutation. Dedicated service fixtures cover unknown signaling, non-object JSON,
identity overwrite and non-mutating extra native-command fields.

The browser host now exposes per-seat **Remove** with a safe-default native
confirmation. Cancel is request-free and restores the invoking action; confirm
focuses the same seat while the service atomically releases only that lease,
publishes the target phone's closed state and closes only its signaling socket
with `seat_reclaimed`. If another host removes the target while confirmation is
open, the stale modal closes and announces the already-neutral result. The phone
distinguishes pending rejection from removal and gives accurate recovery copy.

No behavioral test was executed after this delta. Current evidence is
JavaScript syntax, Python AST, TypeScript `--noEmit`, diff hygiene and compile-
only native targets. Queue at minimum on an isolated desktop:

```bash
(cd services/party && MDKR_DEDICATED_TEST_DESKTOP=1 npm run check)
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_BROWSER_TESTS_ALLOWED=1 \
  python3 tests/check_party_host.py --shell-dir dist/web
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_BROWSER_TESTS_ALLOWED=1 \
  python3 tests/check_controller_page.py --shell-dir dist/web
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_BROWSER_TESTS_ALLOWED=1 \
  python3 tests/check_phone_party_webrtc.py --shell-dir dist/web
```

## Review map

| Boundary | Authoritative implementation | Contract/evidence |
|---|---|---|
| Lobby authority | `platform/online/lobby_core.*`, `services/party/src/match/{protocol,reducer,match-room}.ts` | `online-lobby-protocol-v1.md`, shared 43-command TSV |
| Signaling relay | `services/party/src/match/signaling.ts`, `match-room.ts`, `worker.ts` | `match-signaling-v1.md`, exact relay plus failed-delivery/hibernation service tests |
| Phone Party boundary | `services/party/src/{worker,party-room,types}.ts`, `dist/web/party/party-host.js`, `dist/web/controller/controller.js` | `party-protocol-v1.md`, exact TTL/generation/request/signaling schemas, QR fallback, finite recovery and two-controller non-disclosure fixtures |
| Browser seam | `dist/web/online/match-signal-client.js` | `test_match_signal_client_js.mjs`; deliberately not imported by disabled Online Room |
| Peer topology/forwarding | `platform/net/match_peer_{graph,forward}.*` | `match-peer-carrier-v1.md`, native tests |
| Peer identity/encryption | `platform/net/match_peer_{transcript,crypto}.*`, `dist/web/online/match-peer-crypto.js` | exact native/browser vectors, seal-window-owned nonce sequence, concurrent/provider/exhaustion, authenticated payload type and mutation negatives |
| Preflight | `platform/net/match_preflight.*`, `dist/web/online/match-preflight.js` | `match-preflight-v1.md`, fixed 124-byte vector, canonical graph digest and authenticated-direction-bound three-fragment carrier codec |
| Phrase UX | `platform/online/lobby_{view_model,fake_adapter,browser_wasm}.*`, `platform/app/ui_online_room.cpp`, `dist/web/{index.html,online/online-room.*}` | prior ABI-v3 43-case/27-action normal+ASan pass; current ABI-v4 source/compile contract adds invite recovery; dedicated-desktop executable/rendered evidence pending |
| $0 admission | `services/party/src/party-budget.ts`, `worker.ts` | capacity/observability/reconciliation runbooks and exact schema validators |
| Release gate | `dist/web/online/online-control-config.js`, `online-room.js` | disabled-policy activation tests; Start remains locally gated |
| Desktop-safe validation | `platform/app/app_activation*`, `main_app.cpp`, `app_host.cpp`, `app_window.cpp`, both test runners | final surface capability, Darwin background scheduling, `check_app_background_activation.py`, app/GPU/browser exclusions |

## Local validation snapshot

The final non-window pass on 2026-08-12 produced:

- service TypeScript/QR/Vitest: 10 files, 58 tests passed with one worker;
- pure native/Node carrier, preflight, compatibility, lobby and boundary CTest:
  12/12 passed with all app/GPU/browser labels excluded, `MDKR64_HIDDEN=1`
  and audio disabled;
- the same focused 12/12 lane passed an AddressSanitizer build with
  `halt_on_error=1`; macOS LeakSanitizer is unavailable, so leak detection was
  explicitly disabled and is not claimed;
- provider reconciliation v2 and seven-day ledger v3 adversarial suites: passed;
- public surface (13 tests), game/service isolation, free edge rule, web publish
  stamp, 16/16 internal calls, CI policy, app-background source contract,
  Markdown links and `git diff --check`: passed.

The subsequent phrase-ceremony slice was kept process-only on the occupied
workstation: normal and AddressSanitizer view-model/fake-adapter tests passed
2/2 (LeakSanitizer disabled/unavailable as above); the native static launcher
library and isolated Emscripten ABI-v3 room projection compiled with at most two
background-priority workers; JavaScript/Python syntax, CI policy, desktop
activation source contract and the browser phrase/action/AX source contract
passed. No rendered claim was added: actions 26–27 keyboard/gamepad, native speech,
browser DOM/AX and 200% reflow remain a dedicated-desktop rerun.

The browser projection now also has a browser-free native ABI conformance gate.
It passed normally and under ASan while enumerating all 43 gallery cases, all
10 view kinds, all 18 typed failures and every enabled action 1–27. It drives
each route plus both pending-check → phrase decisions: **Words Match** enters
selection, while **Words Differ** clears the phrase, stops progression and
preserves the room for a fresh secure preflight. It validates timeout
visibility/copy ownership and proves invalid actions, gallery indices and
minimized live projections are fail-atomic. A service-supplied mismatch is
explicitly rejected because only local authenticated-transcript comparison may
originate it. The actual Emscripten ABI-v3 artifact rebuilt successfully and remains below its
128 KiB isolation ceiling. This is stronger model/projection evidence, not a
substitute for the still-open DOM, assistive-technology or reflow run.

No browser automation, ROM run, GPU lane, app executable or native-window test
was launched for this snapshot. Provider assumptions were rechecked against the
official [Durable Objects pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/),
[Durable Objects WebSocket guidance](https://developers.cloudflare.com/durable-objects/best-practices/websockets/)
and [Workers limits](https://developers.cloudflare.com/workers/platform/limits/)
pages dated August 2026.

Those executable results predate the Phone Party delta above and must not be
used as a fresh verdict for it. The 2026-08-13 follow-up deliberately executed
no tests on the occupied desktop.

## Highest-value attack hypotheses

1. **Identity confusion:** attempt cross-room credentials, duplicate/extra
   subprotocols, self-targets, client-supplied sender fields, departed members,
   and a state credential on the signaling route. Confirm rejection occurs
   before room mutation or relay fanout.
2. **Generation races:** interleave two upgrades for one endpoint, old/new close
   callbacks, hibernation and new peer messages. A stale close must never report
   the fresh generation absent; a target generation mismatch must never deliver.
   Also make a state-socket join publication beat the HTTP rotate response: the
   newer member/revision must remain authoritative, the current-generation
   secret may become shareable, and an older/malformed/same-generation-changed
   secret must not.
3. **Replay/order/nonce custody:** attempt to supply or reset a sender sequence,
   reinitialize a native window or issue two browser windows for one live key,
   overlap asynchronous browser
   seals, fail the crypto provider mid-seal, exhaust the counter, switch
   payloads at one sequence, replay an old peer generation after presence
   replacement, splice report fragments from different authenticated peers,
   forge another endpoint inside a completed report, and inject an uncorrelated
   `signal_error` into the browser seam. Those direct reset/duplicate paths now
   refuse; reviewers should still scrutinize lifecycle ownership to ensure
   application code derives exactly one sender object per live directional
   epoch/generation rather than independently deriving the same key twice.
4. **Amplification/exhaustion:** verify one inbound message produces at most one
   target send or one bounded error, never broadcast; enforce 64 KiB UTF-8,
   SDP/ICE metadata, 60/10-second and 256-lifetime limits before unbounded work.
5. **Persistence/privacy:** inspect object storage and operations aggregates for
   SDP, ICE, public keys, credentials, capability/code values, names, IPs and
   arbitrary error text. Only the connection-generation counter may persist.
6. **Cost bypass:** attempt signaling upgrades without the exact 15-unit
   `matchSignalSocket` reservation, wrong operation weight/kind/version, forged
   credentials, repeated replacements and exhausted admission. Health v2 must
   reconstruct all thirteen buckets exactly; control retains its final reserve.
   Provider reconciliation v2 must reject a missing or ≥75% Durable Object
   duration dimension even when request and storage counters remain low.
7. **State-socket regression:** client messages on `/connect` must still close;
   signaling must not mutate reducer state, broadcast payloads to state sockets,
   or turn a service field into Start/admission.
8. **Signaling compromise:** treat the relay as malicious. Substitution may
   prevent connection, but it must not produce a matching human phrase or
   decrypt/forge recipient AEAD input. Preflight reports never traverse it.
9. **Disabled-release I/O:** opening Online Room with the checked-in policy must
   import neither room-model nor signaling modules and perform no `/api/`
   request. Static/service state must not enable the policy.
10. **Local recovery:** every capacity, setup, graph, phrase, channel and room
    failure must preserve one-click local/Phone Party recovery, selections when
    safe, neutral input on custody loss, accurate signaling-vs-peer copy, and
    accessible Cancel/Leave behavior.

11. **Phone Party isolation and poison recovery:** with two approved phones,
    target an offer and ICE at one controller and prove the other receives
    neither. Inject oversized/chunked/invalid UTF-8 HTTP and socket data,
    unknown/impossible room/controller fields, lower transitions and different
    content at the same transition. Flap constructor, listener, send and close
    paths past five attempts; stale callbacks must not mutate, retry must pause,
    direct controls must either remain active truthfully or neutralize, and a
    manual retry must start exactly one fresh bounded sequence. Break QR encode
    and canvas context and confirm code-based pairing remains complete. Rapidly
    dismiss/reopen twice: each revoke/rotate generation must correlate, the old
    QR bitmap/code/URL must disappear before network completion, approved leases
    must persist, and a throwing hibernated recipient must not change the
    committed HTTP verdict or block the healthy peer. Repeat through **Start
    local game** and prove Play occurs only after local invite custody is gone.
    Deliver each rotation's public G+1 before its HTTP secret and then inject a
    newer revoke; accept the former ordering and refuse secret resurrection in
    the latter. Rotate/revoke from a second host and prove the first host clears
    its old QR before merging the public successor. Exercise per-seat removal:
    safe-default cancel must send nothing and restore focus; confirm must close
    only the targeted phone with neutral `seat_reclaimed` copy while the other
    phone stays leased; removal by another host while confirmation is open must
    close the stale dialog and focus the now-available seat.

12. **Phrase ceremony:** after authenticated setup, every display must render
    the same bounded three-compound phrase in a labelled, non-translatable
    region and announce the exact phrase plus mismatch warning. Selection must
    remain unreachable until **Words Match** is explicitly activated.
    **Words Differ** must clear the phrase, stop progression, preserve safe room
    context and require fresh secure setup; service state cannot originate that
    local finding. A mismatch, connection-generation change or rematch epoch requires a fresh
    comparison; no timeout may auto-confirm it. The fake/shared projection and
    fail-atomic model tests implement this ceremony. The live MatchRoom control
    seam does not yet prove carrier-bound phrase values and must not be credited
    as such before A3 `GO` and real endpoint evidence.

## Acceptance invariants

- The service authenticates the actor; JSON never does.
- One current signaling socket exists per member; the object assigns generation.
- Relay delivery is one exact authenticated member and generation.
- Phone Party host signaling is one exact authenticated controller attachment;
  a second controller never receives another phone's offer or ICE candidate.
- Phone Party host/controller publications and HTTP responses are exact,
  byte/time bounded and generation/transition guarded; automatic retry is
  finite and cannot outlive its current socket generation.
- Signaling stores no payload and carries no gameplay or preflight bytes.
- Peer input is direction/generation/epoch/transcript-bound AEAD with replay
  rejection; one direction-bound seal window—not each payload caller—owns its
  monotonic nonce sequence across input and preflight, browser concurrent seals
  refuse, and provider failure does not advance or publish. Open requires the
  complete caller-expected direction, so a valid peer key cannot claim another
  source; one-hop admission separately binds the immediate channel's source id
  and generation before relaying fixed opaque envelopes.
- Preflight fragment state is bound to one authenticated peer direction, and
  the completed report must match that separately supplied endpoint custody.
- The room comparison phrase exposes 30 transcript bits as three speakable
  compounds; Phone Party's separate two-compound approval SAS is unchanged.
- `READY` is consensus evidence, never engine authority or release admission.
- Transcript entries are all validated before sorting; reconnect generations
  reset forwarding replay custody; corrupt preflight counts cannot drive an
  out-of-bounds status walk.
- Preflight reports bind an endpoint-order-independent digest of every directed
  reachability observation, so a signaling layer cannot present incompatible
  direct/one-hop graphs while preserving descriptor/transcript consensus.
- The fixed carrier authenticates input versus preflight-fragment type; three
  sequence-bound fragments carry one report over direct or opaque one-hop
  reliable control without adding any service relay route.
- Signaling close/generation decisions share one object input gate, so stale
  absence cannot follow fresh presence; a broken socket cannot throw through a
  committed state broadcast, and a replacement generation persists only after
  its welcome is delivered and before the predecessor closes; non-object JSON
  roots fail as 400.
- Online room control, signaling and transport stay in launcher/adapters; game
  code sees only a frozen launch descriptor and canonical fixed-tick inputs.
- Literal-zero admission and provider failure never remove local play.
- No test command foregrounds a window unless the reviewer explicitly opted in
  on a dedicated desktop.
- A self-contained Chrome/Worker check cannot masquerade as a parallel-safe
  source check; the manifest rejects browser-launch markers outside serialized
  roles before executing any task.

## Intentional open evidence—not code bugs by itself

- Written A3 rollback `GO` has not been issued.
- Real 2–4 endpoint WebRTC channel binding, NAT/CGNAT/IPv6/enterprise matrices,
  packet chaos and physical phrase UX remain gated.
- Hosted Worker/Durable Object deployment, DNS/secrets, edge rule installation,
  provider export, rollback drill and seven contiguous zero-charge days require
  human provisioning and named operators.
- Four-phone iOS/Android split-screen acceptance, screen-reader review and
  uncoached two-person Online Room study remain human evidence.
- Managed TURN/fallback is intentionally absent from the $0 profile; difficult
  NAT must fail honestly with local recovery rather than consume a hidden paid
  dependency.

These items are findings only if implementation or copy claims they are done,
bypasses their gate, or lacks the documented recovery—not merely because the
external evidence has not yet been provisioned.

## Finding format

For each finding record severity, attacker/precondition, exact file/function,
minimal reproduction, violated invariant, observed blast radius, whether local
play remains available, and the smallest robust remediation. Never include a
real credential, invite, SDP, IP address, ROM path/data or provider secret in
the report. End with an explicit `GO`, `STOP`, or `INCOMPLETE EVIDENCE`; absence
of findings is not automatically `GO` while the intentional evidence above is
open.
