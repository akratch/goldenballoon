# Regression fixtures

`ctest --test-dir build-rel -R '^sprite_model_mesh$' --output-on-failure`
checks the recovered static-replacement mesh decoder and placement layer using
synthetic geometry only. It covers malformed records, ownership refusal,
supported sprite identities, transforms and trunk attachment. It does not prove
pack loading, renderer integration, visual parity or GPU resource lifetime.

The adjacent `sprite_models` CTest generates synthetic directory and ZIP packs
to check loader atlas sharing, priority isolation, incomplete/malformed family
fallback, toggles, and repeated reload/shutdown. It does not exercise GPU cache
retirement or constitute real-pack visual acceptance.

`python3 tests/check_sprite_models_gpu.py --build build-rel --rom /path/to/ROM`
creates a disposable synthetic palm pack and requires actual model draws. It
also executes eleven one-shot GPU allocation failures (one native recovery)
and eleven persistent failures (bounded clean exit), followed by a healthy run.
Both modes also run with replacements disabled and compare complete 3,400-tick
simulation-hash streams. `--smoke-only` runs just the healthy and disabled pair.
The healthy synthetic run also requires one shared atlas upload, reuse by the
remaining mesh entries, and exact logical geometry payload bytes. These are
cache payload counters, not physical VRAM measurements. Visual placement,
presentation replay equivalence and real replacement artwork need separate checks.

Follow the maintainer's current local validation authorization in `AGENTS.md`.
The maintainer authorized this workstation for behavioral validation on
2026-09-06, including native, GPU, browser, ROM, and compiled tests. That
standing authorization permits the existing test-environment flags without
another confirmation each turn. Other workstations need their own authorization;
hidden-window settings alone do not establish it.

Input scripts for `--input-script` headless verification of menu navigation.
Run e.g.:
```
MDKR_TRACE=1 ./build/mdkr64 --headless-frames 1700 \
  --input-script tests/input_scripts/nav_to_character_select.txt \
  --rom baserom.us.v80.z64 2>&1 | grep menu_init
```
Expected: `menuId=3` (character select). The options script yields `menuId=12`.
Both prove real input reaches the game and menus advance/diverge on navigation.

## Character release acceptance receipt

`character_release_evidence` runs
`tests/test_character_release_evidence.py`. It keeps the public
`mdkr-character-release-acceptance/1` schema and verifier in lockstep, proves
that incomplete human/device observations fail closed, and exercises exact
artifact plus provenance hashing. The release operator creates the deliberately
red template with `tools/check_character_release_evidence.py --write-template`
and validates it against the packaged artifact directory only after all three
platforms and low/mid/high physical-device tiers have been observed. The
receipt stores normalized descriptions and digests only; ROMs, character
assets, captures, paths, and operator identity remain private.

`modern_character_draw_store` protects the renderer's retained custom-character
commands. It proves current and previous skin palettes are copied immutably into
demand-driven exact-size storage, rigid and invalid draws behave safely, released
assets and overtaken generations fail closed, shutdown frees every palette, and
renderer restart cannot revive a stale token. This preserves the 256-joint and
2,048-command ceilings without permanently reserving the worst-case 68 MB bone
palette in every build.

**Always run muted and headless** — `MDKR_AUDIO=0` plus `--headless-frames N`.
Omitting `--headless-frames` opens a window *and* the SDL audio device.

## PNG allocation and layout contracts

`png_write_layout` also exercises the first-party PNG decoder allocation
helpers: empty request refusal, retained ownership after refused resize, small
positive allocation/growth/shrink, and ordinary RGB/RGBA PNG roundtrips through
the production implementation. The texture-store probe delegates to the same
helpers. These additions compile but still need behavioral and sanitizer
execution on the current candidate; they do not clear the broader advisory
review. See [decoder boundaries](../docs/security/image-decoder-boundaries.md).

`stb_conversion_ownership` covers the decoder's local conversion-failure
amendment. Small ordinary pixel buffers exercise both bit-depth conversions
for 1–4 components, successful sample values and destination-allocation refusal.
An ordinary encoded 8-bit 2x2 RGBA PNG exercises the product's 8-bit RGBA entry
point and optional 16-bit output API, with deterministic allocation refusal and
optional flip on/off. Allocation counters must return
to zero; assertions are not disabled by optimized builds. The fixture compiles
the actual amended header with the production allocation policy plus counters,
not a replacement converter. Only the vendored PNG-only header's unused-helper
warning is locally exempted; first-party warnings remain errors. Runtime,
sanitizer and old-code-control execution are pending. No malformed image or
external reproducer is used, and this does not clear the advisory review.
The narrowing helper uses ordinary 16-bit buffers directly; encoded 16-bit PNG
and caller-admission coverage are separate, not implied by this fixture.

## Native unit build portability

`run_checks_artifact_routing` covers selected native drivers/game executables
and early dependency admission for the seven local Worker consumers. Missing
Wrangler must fail before stage inspection or subprocesses; embedded-LAN must
not acquire a Worker dependency. `party_worker_startup_reporting` covers
readiness ownership, launch refusal, unexpected failure/interruption cleanup,
and bounded metadata without raw output. `party_worker_reporting` covers the
two-person/chaos aggregate error boundaries and whole-log error scanning across
chunk boundaries. Process/browser/network boundaries are mocked in these
fixtures. They are registered but remain unexecuted; none replaces real local
service or multiplayer qualification.

`online_teardown_tracker` covers the shared native room shutdown drain. The
warning deadline must not authorize detached workers outliving their tracker or
network globals. Empty/already-complete and deliberately held two-worker cases
check warning delivery outside the lock, completion before return and repeated
drain behavior. The fixture uses ordinary threads and condition variables, not
networking, sleeps or app startup. CTest's 30-second fixture timeout detects a
deadlocked regression; it is not a claimed product shutdown deadline. Strict
C++17 syntax passes; behavioral execution remains pending. The fixture also
uses the production retirement helper for background destruction, injected
thread-launch refusal/inline cleanup and recovery, and sixteen completed
retirements reaped while another destructor is deliberately held. These are
unexecuted assertions, not evidence of actual transport responsiveness.
Polling coverage additionally requires held workers to return not-ready without
waiting, independent completed-handle reclamation and idempotent ready results.
Storage allocation refusal is handled before launch by source construction;
allocation-failure injection and real transport cancellation/closing UI still
need separate qualification.

`app_cleanup_completion` covers the dependency-free asynchronous cleanup observer:
no startup before owner retirement, exactly one startup across pending polls,
successful and exceptional futures, throwing startup, and explicit rejection of
invalid/deferred futures. A deferred callback must never execute on the polling
thread. Completion can mean failure; callers must inspect `failed()` before
claiming successful cleanup. The fixture uses promises, not RTC or networking,
and does not qualify actual library shutdown. Behavioral execution remains pending.

`datachannel_cleanup_worker` compiles the same helper used by the reviewed RTC
final-token amendment. It covers refusal of either reserved thread, detach
refusal, cancellation before arming, destruction under an initialization mutex,
cleanup that joins the final owner's thread, and exception/TLS completion before
the result. Both workers are reserved before initialization; the publisher joins
the cleanup worker before settling the ordinary promise, without a late
thread-exit registration. The fixture does not execute RTC or prove rollback of
partially initialized RTC subsystems. Strict syntax and optimized/ASan+UBSan compilation
pass; its assertions and actual library startup/shutdown remain unexecuted.

`datachannel_startup_stages` exercises the production polling/registry ownership
helpers used by the dependency amendment. Allocation and worker-start refusal
must leave empty stopped polling ownership; a successful retry must own exactly
one worker. Registry allocation precedes C startup, and publication follows its
return. These fixtures do not inject failures into the actual RTC subsystems or
establish recovery from internal C startup failures.

`rtc_initialization_transaction` combines the real state and reserved-worker
helpers with subsystem counters. It covers seven partial-start failure points,
pending rollback, original exception preservation, successful retry, and cleanup
failure at each of Pool, Poll, SCTP and Sockets. Deferred/invalid completion,
duplicate initialization, omitted retirement and repeated failed retirement
must never admit another epoch or discard ownership. Real token/preload weak-owner
reuse, dependency fault injection and Windows socket behavior still require
separate qualification. These added assertions remain unexecuted.
Stage-call counters distinguish zero started pool workers from no acquired pool;
the sockets-only failure also proves no unacquired pool cleanup is requested.
Pending admission checks use the same thread with the owner mutex held; they do
not establish responsive concurrent API calls while real RTC cleanup holds that
mutex through subsystem retirement.

`datachannel_work_admission` uses the actual `MdkrScheduledWork` record and
production priority-queue container/comparator to extract large callables with
allocation/copy refusal, unchanged deadline ordering and owner destruction after
the scheduler lock. Its shared insertion helper uses real deque allocator
refusal, measurement/move refusal and successful retry to check size/amount
consistency. CMake binds the helper through the independent dependency patch;
the optimized fixture compiles but has not executed. It does not cover allocating
submission, Processor continuation, off-thread teardown or receive counters
(RTC-ALLOC-01a/c/d/e).

`datachannel_prepared_work` uses the production prepared-node FIFO and typed
scope guard for move-only owners, preparation refusal/retry, allocation-free
accepted publication/extraction/continuation, standard/non-standard task throws,
settled-pending owner release and iterative queue destruction. It is a helper
fixture, not a substitute for actual scheduler concurrency.
`datachannel_prepared_dispatch` separately links the patched vendor ThreadPool
and Processor. Its seven groups cover timer/ready order in both directions,
accepted continuation under allocation refusal and task exceptions, bounded
admission wakeup, final-owner join, active-worker timed cancellation outside locks
and repeated idle-pool joins. Publication assumes a live epoch and the inspected
strong-owner Processor callers; the clear case does not prove arbitrary
post-join publishers safe. Both CTests are registered (helper 30 seconds, actual
dispatcher 60 seconds). Final optimized, ASan/UBSan and Windows cross-builds
compile/link the game plus both prepared fixtures, the existing work fixture
and Workshop model after correcting the new vendor fixture's private include
paths. Generated registration is present for all four fixtures in each profile;
all new assertions remain unexecuted. These builds include settled Workshop
mapping/detail focus, reinspection and invalid-height guidance. Windows retains
its historical cloud origin, not final partyless artifact provenance.
The refusal seam intercepts ordinary throwing `new`/`new[]`, not `malloc`,
over-aligned allocation or exception-runtime allocation. Explicit `bad_alloc`
throws cover exception continuation, not actual body-allocation refusal. The
bounded-admission case's 100 ms no-return window is a black-box observation,
not deterministic condition-wait entry proof.
Generic allocating `noexcept` admission, SCTP refusal ownership, mandatory
transport retirement and user task-body allocation remain separate requirements.

`datachannel_transport_retirement` links the actual serial teardown Processor
with the production reservation helper. Ten groups cover preparation and shared
control-block refusal, weak-controlblock/epoch lifetime, one-shot stop, throwing
stop, post-ownership initialization unwind, start/stop ordering and serialized
blocking deletion with continued pool progress. Its ordinary-new interception
does not cover C allocators or real ICE/SCTP initialization failure.

`datachannel_transport_edges` separately exercises the actual vendor Transport
base implementation and production chain-retirement helper. Eleven groups cover
unstarted losers, occupied-edge rejection, constructor unwind, same-owner and
replacement registration, permanent sealing before deferred stop, external
callback replacement, reserved and foreign-owner cascades, callback removal
reentry and reentrant destruction of a retired callback capture. The concurrent
removal case is bounded interleaving stress, not a
deterministic old-code deadlock control. It initializes RTC global services but
opens no peer/app/ROM session. This does not exercise the private peer/WebSocket
publication CAS or qualify actual network/platform shutdown. Both fixtures are
registered with 60-second limits; assertions remain unexecuted.
Optimized, ASan/UBSan and Windows cross-builds compile/link the game, both new
fixtures, prepared dispatcher and Workshop model including the latest field-focus
interruption correction. Generated registration is present in all three profiles.
Windows retains its historical cloud origin, not final partyless artifact
provenance; existing BasisU GCC warnings remain. No new tests or apps executed.

`party_open_transaction` is a production-source contract for connection setup,
callback registration, publication and failed-open retirement ordering. It is
not a runtime WebSocket fault-injection fixture or rendered recovery result.
The source assertions remain unexecuted; constructor/registration/open failures,
callback interleavings and actual reconnect/shutdown require behavioral evidence.

`party_callback_identity` compiles the same predicates used at Phone Party's
locked commits. Its deterministic capture/change/commit model rejects a retired
socket, replaced peer, stale initializing placeholder and revoked retry/ping
decision, while allowing completed direct peers to survive signaling reconnect.
Always-on expectations remain enabled in optimized builds. This is not the actual
TransportState or RTC callback scheduler; source bindings in
`party_open_transaction` tie predicates to production publication/retirement,
but neither fixture substitutes for real concurrent callback acceptance. The
new assertions are unexecuted.

`party_peer_setup_retry` uses the production bounded early-setup policy: three
lifecycle setup failures, 300ms/600ms backoff, no duplicate-hello bypass, stale
generation/revision completion refusal and preserved unanswered-offer budget.
Exhaustion cannot authorize a fourth fresh unanswered offer. Production may
begin a new unanswered-offer episode after a previously authenticated peer fails;
that does not replenish its lifecycle setup-failure counter. Source bindings in
`party_open_transaction` require reservation before allocation, current-token
failure/success commits, due retry checks and pending exhaustion reporting.
The optimized fixture compiles; its assertions and real callback/allocation
recovery journeys remain unexecuted.

The existing `match_signal_client` fixture now includes a one-shot refused
socket-worker start through the actual `connect()` exception boundary. It asserts
Idle rollback, the ordinary refusal code, empty events, repeated refusal without
a diagnostic destination, then a real same-client loopback welcome and repeated
close. This coverage is added, not executed; it does not simulate all allocation
failures in the running socket worker or establish Windows socket ownership.
An added actual-client close-event refusal seam verifies that mandatory stopping
and worker transfer precede optional reporting, with joining still required.
This assertion is compiled, not executed.

The same fixture now includes factory refusal/retry, three transactional send
admission stages with unchanged sequence/correlation on refusal, and worker
publication plus terminal-event/drain allocation refusal. It drives the actual
client, not a second signaling implementation. `match_signal_admission_source`
has six production bindings for ordering, rollback, worker unwind, nonallocating
terminal fallback, storage-before-consumption drain semantics and local secret
buffer guards. The actual fixture also uses `scoped_string_wipe.h` to assert
synthetic-buffer erasure during early unwind and idempotent explicit erasure.
Production reserves final capacity before copying secrets and guards the local
offer/request/comparison buffers with `mbedtls_platform_zeroize`; this does not
promise to erase previously released growth copies or all process memory.
Final integrated optimized, ASan/UBSan and Windows cross-compilation passes;
all new behavioral/source assertions
remain unexecuted, and do not qualify caller-wide allocation or backpressure.

`network_lifetime` exercises the production shared lease and Winsock reference
policy with fake OS calls: allocation-before-start, refused-start/version retry,
last-owner cleanup failure and sticky refusal, copied/moved ownership without
allocation, result-before-release ordering, concurrent independent copies,
repeated sessions and non-Windows no-op behavior. `network_lifetime_source`
has eight source methods binding the policy to actual OS calls, resolver member
order, socket close order, LAN lookup/listener/retained aliases, mandatory timeout
admission, callback-capture retirement and launcher failure reporting. These are
added, unexecuted assertions, not Windows OS injection or a LAN lifecycle run.
The separate `lan_party_server` fixture now adds six actual-server groups for
listener admission refusal/restart, accepted allocation/registry/thread refusal,
upgrade allocation failure, consumer exceptions, callback stop requests and
callback-capture destruction. Accepted timeout/required socket-option refusal
is covered before worker admission; the worker marker remains active while
callback captures are explicitly released.
The source candidate stages fd ownership, preserves registry join ownership,
uses a sealed common finalizer and lets only the launcher finish joins. Strict
source/fixture syntax passed; these new loopback assertions remain unexecuted.
Exceptional destructor join failure deliberately retains state and records
failed cleanup; neither the fixture nor lease model establishes successful
retirement on that exceptional branch. Final optimized, ASan/UBSan and Windows
cross-builds compile/link the game and all five network/peer/queue/signal/LAN
fixtures after the latest timeout/capture changes. This is not Windows execution
or final-package proof; that cross-build retains a historical cloud origin rather
than final partyless provenance. Existing dependency compiler warnings remain.

`native_party_notice_pins` exercises recipe-to-notice consistency: matching
multiline pins, independently or consistently stale notices, ambiguous/missing
recipe definitions, missing consumers and false prefix matches. Its pure helper
is shared with the real third-party notice gate, which additionally pins the
combined manifest and each amendment's source bytes and checks all desktop
notice-hash validators. The fixture is registered but unexecuted; source checks
do not prove that a packaged artifact actually contains the required notices.

`async_work_budget` covers move-only work permits, fixed capacity, refused
acquisitions, destruction/reassignment and scheduling-failure release, permit
state surviving its budget object's lifetime, and 24 contending workers sharing
eight slots. A second translation unit checks that the inline resolver accessor
shares one eight-slot process budget instead of creating a pool per source file.
Eight bounds outstanding first-party DNS work, not active connections or players.
Held workers retain capacity through their owned lookup/result cleanup even
after a caller abandons the result.
The fixture uses no resolver or network; it cannot prove OS lookup cancellation
or bound libdatachannel/libjuice's independently owned work. Its behavioral
assertions remain unexecuted; the 30-second timeout is a fixture deadlock bound,
not a product shutdown promise.

`online_resolver_budget` binds both production clients to that shared budget:
admission precedes lookup allocation/start, cancellation and deadlines apply to
capacity waits, worker captures own their strings and task, and a task-held
permit survives through address-result and synchronization cleanup. It also
checks that a failed detach retains and joins the handle. This source contract
launches no resolver; concurrency and actual shutdown behavior require separate
executed qualification. `online_lobby_takeover_source` additionally requires
zero outstanding first-party resolver work before global RTC cleanup begins.

`app_launch_hold` compiles the actual launcher sampler against deterministic SDL
functions, without linking SDL or opening devices. It covers same-controller
shoulder pairing, late attachment, index/identity races, detachment and balanced
borrow release. `ai_difficulty_value` checks the shared pure gameplay/UI
interpretation across all canonical case variants and legacy fallbacks;
`ai_difficulty_ui` binds it to independent current/desired display, speech,
selection and unchanged source locks. These fixtures still require execution.

`match_live_transport` includes silent TLS-handshake and HTTP-response close
cases plus a short connect-deadline case. They use ordinary loopback sockets
with finite server holds, not a public service or certificate bypass. They
qualify socket cancellation only when executed; no OS DNS cancellation or
libdatachannel global-cleanup verdict follows. Unknown case selectors now fail
instead of reporting success after running no cases.
Its `sigpipe` case additionally exercises the production send helper against an
ordinarily disconnected local peer in an isolated child with default SIGPIPE
handling. This prevents incidental process-wide signal configuration from hiding
the regression; platforms without per-send suppression explicitly mark this case
not applicable. Compilation is not proof that the assertion passed.

`match_transport_tls_io` is a source contract for the HTTP transport's
nonblocking TLS BIO, cancellation/deadline checks, retryable polling, bounded
readiness waits, SIGPIPE-safe plaintext/TLS sends, and retained certificate/hostname
verification. It launches no
endpoint and cannot substitute for the loopback TLS behavioral cases above.

`online_room_takeover_policy` compiles static assertions against the native
launcher's shared ownership/view policy. An owned session with an unavailable
view must retain takeover; only an actual release or known entry view permits
the normal shell. `check_online_lobby_takeover.py` also checks production wiring
and deferred leave ordering before its existing rendered scenarios. The new
policy assertions pass compilation, not an actual window/transport test. A
failed view before/after service, actionable exit, registry cleanup and a fresh
room after recovery still need behavioral qualification; a successful static
assertion must not be reported as that broader result.

The same fixture also implements `IMdkrOnlineAdapter` and calls the production
`OnlineRoom_readViewKind` boundary: null/uninitialized admission, exact view
forwarding, failure before/after service, read-only observation, ownership
release and fresh-entry recovery. These are runtime assertions, unlike the
truth-table static assertions above, and have not yet executed. The stub owns
no transport; its destruction counter does not qualify production registry or
worker teardown. A rendered invalid-view/Leave Room fixture is still required.
The target links the existing ROM-free fake-adapter/view/lobby/session sources:
GCC can emit a virtual-call candidate for the interface header's inline fake
seam even though the fixture owns its own deterministic adapter. This resolves
the Windows link dependency without a substitute function or optimization waiver.
The takeover source contract also pins end-of-frame barriers after invite and
stranded-room rebuild attempts and live timeout/primary/secondary/cancel
actions. Actual successful/refused retries and focus/scope checks remain part
of native acceptance; the source guard is not a rendered result.

`online_join_code_input` compiles the same `OnlineRoom_drawJoinCodeInput`
helper used by the native beta launcher against ImGui's CPU-only core. It
exercises typed/pasted digit filtering and leading zeroes, middle click/arrow
editing, deletion, selection replacement, undo/redo, visible text/caret/selection
geometry and narrow-field horizontal scrolling at two widget scales. It uses
a private in-memory clipboard; no platform/render backend, SDL, GPU, ROM or
native window is initialized. The test is still executable validation and
does not create an exception to an execution-tool refusal. It is registered
and compiles, but its new assertions have not run. Real launcher fonts/themes,
OS clipboard/input, controller entry and packaged 200% layouts still require
the acceptance guide's native-online journey.

The Windows validation build includes every registered unit target, not just
the game. `ghost_bank` includes the Windows process-ID declaration explicitly;
the SDL-stubbed `app_window` unit owns its console entry point and does not link
an SDL application startup shim. The non-MSVC `void_pairs` target links the
production `tracks.c` walker. MinGW additionally requires supported
link-time optimization to discard unrelated engine references before linking;
configuration fails if that capability is unavailable rather than omitting the
test. GNU link-time work for this target is limited to one compiler worker.
`ainode_route`, `engine_guards` and `save_records` link production engine files
the same way, through `mdkr_add_engine_unit_test` in `cmake/tests.cmake`.

The CI contract also pins Windows portable-settings read-back to a literal,
whole-line match against the fresh process's read log. Its controls cover the
old bracket-as-regex mistake, a weakened partial-line match, and accidentally
checking the write log instead. Native Windows execution remains necessary to
prove persistence; these source-contract checks are not a runtime substitute.

## Complete suite runner and `--build` contract

### Retaining pacing diagnosis evidence

For an authorized diagnostic invocation of `check_pacing_quality.py`, add
`--keep-evidence` to retain its uniquely created private temporary directory.
The checker prints that location before launching any arm. Each attempt keeps
`process.log` and labelled `process.json` before parsing or checking the result,
including nonzero exits, startup failures and partial timeout output. The
directory also contains the isolated saves/configs used by the run; treat the
whole directory as private, potentially ROM-derived evidence and never commit
or publish it. No ROM file or environment dump is copied into evidence.

Without the flag, the temporary directory is removed as before. The flag changes
retention only: it neither authorizes app execution nor changes pacing policy,
retry counts, quality thresholds or baseline exemptions. Parser checks pass;
the new evidence-file and mocked-process regression assertions still need
execution. Use this retention mode on the next approved investigation of the
43-re-anchor companion failure, whose original full trace was not retained.

Every behavioural script accepts the same `--build` value: either a directory
(`--build build-rel`) or the executable inside it
(`--build build-rel/mdkr64`). Use the runner for a complete pass:

```bash
python3 tools/run_checks.py --jobs 6 \
  --build build-rel --release-build build-rel --asan-build build-asan \
  --wasm build-web/mdkr64_web.wasm
```

The runner and `tools/ci/ci_local.sh` require the caller's human-attested
`MDKR_DEDICATED_TEST_DESKTOP=1` before execution. The runner's `--list` is a
non-executing inventory exception. Hidden windows and background scheduling
are defense in depth. The release gate is the complete suite on an authorized
test desktop; GPU and timing-sensitive tasks remain serialized.

The manifest registers the `tests/check_*.py` scripts as parallel tasks, apart
from a set of CTest companions that `rom_free_units` owns and runs through the
ctest task (`check_controller_settings_persistence.py`, `check_host_input_focus.py`,
`check_launcher_tabs.py`, and `check_overlay_input_handoff.py` among them), so
every registered check script still runs exactly once in a complete pass. That task also
runs the ROM-free display/endian/magic-code/object-layout/allocator/
runtime-contract, sprite-layout, RDP-interpolation, font-registry/SDF, and RL-1
CTests, while filename entry, locked-door collision, RAW16 audio, native-layout
safety, and widescreen/shadow safety run in their specialized
primary/Release/ASan/alignment configurations. Startup fails if a new check
script is not
registered, or if a `tests/test_*.py` has no CMake `add_test()` to carry it into
the ctest task. Each task receives its own temporary save directory, exported as
both `MDKR_SAVE_DIR` and `MDKR_TEST_SAVE_DIR`, so parallel tasks cannot overwrite
one another's EEPROM fixtures or the repository's playable `save/`. A
standalone check still defaults to `save/` and restores the EEPROM images it
found there. Inherited `MDKR*` hooks are cleared except for those two,
`MDKR_TEXCACHE_VERIFY`, and the caller's `MDKR_DEDICATED_TEST_DESKTOP`
attestation. `MDKR_VIDEO_CONFIG_PATH` is pointed at the null device,
and audio is forced off. `--only NAME`, `--role ROLE`, `--primary-only`,
`--skip-instrumented`, and `--skip-wasm` are iteration/configuration tools; a
default run is the complete gate.

Headless frame numbers are presentation-loop iterations, not authoritative VI
fields. Fixtures calibrated before authored cadence became the default must set
both `MDKR_SIMULATION_CADENCE=enhanced` and `MDKR_SYNTH_FIELDS=1` in their clean
environment when their scripts, captures, or thresholds intentionally retain
that historical one-field timeline. Shipping-cadence gates instead select
`original`/two fields explicitly (or test the fresh default), and the cadence
gate verifies that distinction. Never repair a fixture mismatch by weakening
its gameplay or pixel threshold without first classifying its time base.

As a historical reference point, the v0.3 release gate passed the **38-task
optimized native/sanitizer stage in 22m17s**, the **29-task Debug primary stage
in 12m31s**, and both wasm-only tasks — including the linked layout check and
real Chromium — in **1m05s** (figures predate the current suite and are kept only
to illustrate the stage shape). The
vehicle sweep always emits a failed child's diagnostic tail and recognizes
UBSan text; there is no retry or tolerated-failure path in the registered gate.
The ROM-free CI-contract task fails closed if push/PR triggers,
Linux GL/WebGPU, macOS WebGPU warnings, sanitizers, linked wasm, browser save
custody, immutable action pins, or either ROM guard disappears.
The address-domain source task separately rejects every raw native
pointer-to-32-bit narrowing outside `address_domains.h` and
`dkr_native_ptr.h`; direct conversions remain compiler errors.

`camera_obstruction_authority` is a ROM-free source-level CTest for the camera
occlusion seam. Until the resolver is integrated it permits no tick calls; once
present, it requires exactly one `camera_obstruction_tick` call in each fixed
tick route (`mode_game` and `update_menu_scene`), after the TT camera author and
before sort/LOD/visibility. It also rejects resolved/display/projection camera
APIs from the simulation-owned sort, LOD, and visibility passes: those consumers
must retain their legacy logical basis builders or an explicitly named logical
basis/view replacement.

`water_effect_tick_contract` protects the simulation-owned water texture clock
extracted from rendering for rollback. Because `gObjPtrList` mixes full objects
with particle-prefix storage, it requires the production ordering used by
`shadow_update`: reject particles, check the header's water-effect group, then
load and dereference the optional `WaterEffect`. In-memory positive controls
remove the particle gate and move the dereference early so the check cannot
silently pass after either four-player crash regression returns.

`durable_rollback_firewall` is the ROM-free storage-side contract. It proves
both EEPROM entry points refuse an online/resimulation progression write before
candidate mutation, and proves the shared Controller Pak store refuses it
before directory resolution, encoding, temporary-file creation, replacement or
browser sync. One positive control removes each storage-family gate.

`rumble_rollback_adapter` protects the first production reversible-effect sink.
The rollback journal must install its cancellation callback; only vanished
rumble may reach it, the decoded controller index must be bounded, and the host
call must stop rather than start vibration. Corrected authoritative rumble state
is serviced normally after replay, so this callback never edits `RumbleData`.

`rollback_audio` is the ROM-free state-machine proof for versioned SFX custody.
It covers immediate prediction preview, duplicate suppression, corrected-only
defer/preview, vanished cancellation, confirmation retirement, invalid request
rejection and last-value command coalescing.

`session_bridge` includes the vendor-neutral MatchTransport drain contract.
Local samples are indexed by frozen endpoint seat order and may replace only
the endpoint's canonical slots; transport-owned remote slots remain byte-exact,
local confirmation is additive, and malformed/count-mismatched merges leave the
bridge untouched. `match_transport` consumes that seam from the launcher side:
authenticated peer masks cannot overwrite local seats, exact epochs and bounded
tick windows fail closed, remote repeat-last prediction carries no false
confirmation, late corrections expose the first dirty tick, and a rejected
drain leaves both history and bridge unchanged. `match_input_runtime` is the
copy-out C ABI installed only for the bounded engine lifetime. `session_runtime`
proves the adapter is created at READY, maps physical local-seat order into the
frozen canonical roster, copies retained replay ticks by exact epoch, and is
retired/replaced with the engine epoch. The four-process convergence gate now
feeds all scripted remote ports through authenticated launcher ingress and
withholds one real A-edge for four ticks; the production snapshot path rewinds
five ticks and converges again. These gates prove the adapter and engine
consumption boundary, not a deployed carrier or identity handshake.

`audio_rollback_adapter` protects the production wiring. Table, direct and
spatial starts must trace before binding and must not reach their raw allocator
when rollback consumes them; spatial identity includes position. Voice
cancellation checks its monotonic allocation generation before any stop, and
volume/pan/pitch/priority/stop commands flush only after event reconciliation.
The same gate requires spatial-source pool/topology state in the real authority
registry. Its in-memory mutations break each of those properties independently.

`postrace_rollback_contract` keeps the results transition inside the rollback
domain. It requires the post-race viewport and both scene-load latches in the
authority registry, permits an already-authored post-race tick to replay, and
retains fail-closed admission for recursion, pause, textbox and scene-load
boundaries. Removing a latch, reintroducing the post-race ban, or weakening a
scene-load guard fails its mutation controls.

`camera_obstruction_resolver` is the ROM-free policy layer above the immutable
geometry sweep. It exercises immediate skin-backed retraction, bounded fixed-step
recovery, last-safe revalidation after projection changes, discontinuity reset,
invalid-world fail-safe behavior, deterministic repeatability, and the diagnostic
center-ray near-plane false-clear control. It intentionally owns no DKR state or
environment parsing; the fixed-tick integration supplies those inputs.
Release-hold cases include repeated zero-time projection revalidation at every
point before release: the eye and clear-tick counter stay held, positive-time
resume serves exactly one tick, and contact still retracts immediately at zero
time. The zero-duration hold remains the no-hysteresis control.

`camera_object_bvh` is a ROM-free white-box test of the production immutable
object-model index. It compares indexed sphere and exact rounded-lens results
byte-for-byte with full-world queries under identity and 64 rotated/scaled
inputs, exercises node/chunk/triangle/stationary budget exhaustion, and injects
NaN bounds, invalid leaf tags, duplicate children, corrupt chunk coverage,
shrunken parents, stale generations, and same-address generation reuse. Every
fault must return `INVALID` (or be rejected by the pre-publication validator),
never an ordinary clear. This test intentionally includes the production cache
translation unit so it cannot drift into a second test-only BVH implementation.

`camera_dynamic_temporal` exercises the production moving-object presentation
envelope against 4,097 renderer-transform samples per case. It covers endpoint
identity, translation/rotation/scale, shortest-angle wrap, high coordinates, 128
deterministic randomized poses, invalid transforms, and the moving non-door
camera-chord cut broad phase. No dense intermediate object AABB may escape the
published envelope.

`camera_dynamic_publication` executes the invalid-until-proven dynamic census
state machine. It proves that a failed census has no current query source, the
first recovered census advertises a discontinuity with no previous transform,
all hard objects cut on the failed image, and interpolation resumes only after
two consecutive complete publications.

`camera_dynamic_precedence` pins the dynamic narrow-phase winner ordering
against the production comparator itself, one key at a time and in both
directions: time outside the public tie window, then spawn generation, model
generation, source triangle, hit stable ID, and authoritative list index. It
includes the production translation unit so the ordering under test is the one
the sweeps call, rather than a second copy of the rule.

`camera_dynamic_temporal` and `camera_lens_pose` link `game/src/hasm/math_util.c`
so the renderer matrix recipe they compare against is the production one. Both
carry an absolute golden arm, so a sign error in `mtxf_from_transform` fails them
instead of moving both sides of a differential together.

`camera_target_visibility` pins the current-pose readability classification. A
thin local skin may be excluded, a remote blocker is `HIDDEN`, a focus that
remains in a thick slab is `EMBEDDED` and not visible, and source/numeric failure
is `INVALID`. These outcomes cannot collapse into a convenient clear result.

`camera_obstruction_observe` protects the camera authority boundary and rollout
gate. The two fixed-tick calls sample all eight authored slots, publish the
dynamic occluder list, latch selected viewport projections, and resolve into a
native presentation sidecar. They never write `Camera`/`gCameras`, selection,
sort/LOD/visibility, audio, or other authoritative state. `render_scene` and the
interpolation snapshot walker consume the sidecar only inside presentation.
Correction is opt-in: unset `MDKR_CAMERA_OBSTRUCTION` selects `observe` — the
authored camera — and the launcher's **Camera** setting exports `modern` onto
the same variable when a player opts in. Set `center-ray` for the deliberately
incomplete diagnostic control, or `legacy` for original direct placement; an
unrecognised value falls back to the default, `observe`. `MDKR_CAMERA_COMFORT=reduced`
turns on the presentation-only reduced-motion filter over the corrected camera;
unset and unrecognised values are `authored`. Set
`MDKR_CAMERA_TRACE=1` for summaries or `=2` for per-slot desired/effective,
projection/guard, mapping, intent age/pivot/target, and hit diagnostics; this is
intentionally separate from `MDKR_TRACE`. The sidecar receives last-author-wins
intent records at the post-dialogue/shake racer seam, T.T.'s final spectate pose,
and scripted cutscene transform writes. The finalizer consumes one fresh intent
per selected physical slot; only an unchanged, already-consumed paused pose may
reuse stale intent. Render projection generation and every presentation camera
read are checked/routed through the same scoped sidecar.

`check_camera_obstruction_runtime.py` is the ROM-backed, same-binary Ancient Lake
release witness. It runs Legacy, intentionally incomplete Center-ray, and Modern
for 5,200 deterministic frames. Legacy and Center-ray must positively reproduce
lens penetration; Modern must actually correct poses while publishing zero
penetrated resolved lenses, degraded sources, invalid slots, duplicate solves,
projection mismatches, missing caches, invalid transforms, or capacity failures.

```bash
python3 tests/check_camera_obstruction_runtime.py --build build-rel \
  --rom baserom.us.v80.z64
```

`check_camera_obstruction_display_matrix.py` runs 24 Modern geometric arms: 4:3
low/high, 16:9, 21:9, 32:9, and portrait crossed with authored, minimum 20-degree,
maximum 140-degree capped, and maximum 140-degree uncapped FOV policy. At every
FOV it requires equal-aspect resolved-camera traces to match byte-for-byte,
catching any attempt to derive world framing from resolution, render scale, or
rounded internal render targets.

The remaining registered camera runtime gates are deliberately orthogonal:

- `check_camera_dynamic_obstruction_runtime.py` requires every dynamic-instance
  blocker attribution to coincide with an applied correction and rejects every
  hard-object publication omission.
- `check_camera_projection_fallback_runtime.py` injects a latch failure and allows
  restoration only for a freshly revalidated exact camera/projection pair.
- `check_camera_probe_invalid_recovery.py` makes one exact static probe of one
  tick unprovable through the `MDKR_TEST_CAMERA_EXACT_INVALID_TICK` seam and
  proves the degradation is scoped: the tick keeps its telemetry mark yet still
  publishes a proven, target-visible pose, and no other tick is touched.
- `check_camera_obstruction_lifecycle_runtime.py` drives quit, race restart, and
  hub/race reloads; each level generation needs a reset witness and fresh tick-1
  solve. Its Adventure arm also proves an open/moving door is published safely.
- `check_camera_emergency_readability_runtime.py` disables alternate shoulders in
  a test arm and requires real corrected gameplay rows to exercise the 96..254
  per-viewport racer opacity envelope without compromising clearance or target
  visibility. Emergency elevated/azimuthal endpoints are counted separately.
- `check_camera_3p_tt_runtime.py` validates the 3P T.T. fourth viewport/camera 3
  directly, avoiding an unrelated inherited autopilot-progress limitation.
- `check_camera_motion_quality.py` captures the MOTION-01 motion metrics
  (retract latency, recovery, jerk, shoulder flips, emergency dwell, published
  cuts, blocker churn) on the lake, hub, and 3P T.T. spectate routes under
  Modern. Chatter and shoulder-flip invariants gate hard; the analog
  distributions print as the labelled baseline for threshold-setting.
  Level-1 re-engagement events retain release/gap, per-slot contact history and
  recovery state in each arm's optional `--baseline-out` JSON. Console context
  is bounded to 16 events per arm; missing event rows from older binaries are
  disclosed without weakening the aggregate failure. The ROM-free
  `camera_motion_reporting` CTest entry (`test_camera_motion_reporting.py`)
  checks extraction, bounded reporting and failed authored/high-rate arm
  isolation without launching a game. These additions still need execution on
  the current candidate; they do not resolve Ancient Lake's hard motion failure.
- `check_camera_obstruction_performance_runtime.py` runs one optimized binary in
  Observe, Modern, and Modern + reduced motion over a long 4P route, requiring
  at least 5,000 active four-viewport fixed ticks (more than 83 seconds at 60 Hz).
  It gates finalizer/query percentiles, tail counts, query fan, static cache
  bytes/build time, exact dynamic-sidecar allocation bytes, and byte-identical
  v3 authority state across all three — which is `Camera.Comfort`'s
  presentation-only proof as well as the camera's.
  `MDKR_CAMERA_PERF=1` is allocation-free and reads an
  intra-thread performance clock; it never uses the synthetic VI/rAF clock.

The development-only held-shot selection trial has separate qualification.
`--held-shot-trial` on the motion, lifecycle and performance runners explicitly
enables it; normal invocations do not inherit the trial switch. Motion JSON
records which policy was measured, and the performance runner leaves Observe
unchanged for its authority comparison. A trial pass does not qualify the normal
policy or authorize changing the user-facing camera default. Actual rendered
quality still requires the architecture plan's visual/device acceptance.

`check_camera_held_shot_faults.py` is registered in Release and sanitizer roles.
It uses private save/config directories and injects projection and exact-query
failure into a known active held-shot episode, requiring safe publication and
fresh history after the missing projection. Its ROM-free reporting CTest
rejects missing injections and unsafe or incomplete evidence.
`check_camera_chain_evidence.py LOG` audits unpublished-chain continuity and
reported framing/return invariants in a private trace; it is not an independent
geometry proof. Its parser and negative controls run through CTest, as does
the actual interpolation long-arc containment control. Keep trace/capture files
outside public source and release archives.

The three source-role camera cache tasks are static audits of the immutable
occlusion caches the runtime queries, and hold no ROM:

- `check_camera_track_occlusion_cache.py` pins the static visual-triangle cache
  built from a finalized level model: it is copied once in segment/batch/face
  asset order, never mutated, and released with the level, so a camera boom
  spanning arbitrary segments cannot read gameplay state or a stale allocation.
- `check_camera_object_occlusion_cache.py` pins the object-model cache's
  ownership and generation lifecycle: the cache owns its copies, its registry
  key is the model allocation, and the entry is erased before that address can
  be reused by a later loaded object.
- `check_camera_dynamic_occlusion.py` pins the fixed-tick dynamic hard-occluder
  publication: identity assignment, the precedence order used to break ties
  between competing hits, and the purity of the census with respect to
  authoritative state.
The ROM-model task independently inflates all 445 supported object/level models
and validates every renderer-consumed geometry/visibility region, batch
sentinel, texture reference, and triangle-local vertex index, including dormant
variants no route instantiates.

`check_renderer_backends.py` additionally writes every GL present from frame 15
through 229 and rejects an isolated black buffer between completed images. This
owns the F-28 consecutive-capture contract.

### App-shell drag-and-drop ROM acquisition — `tests/check_shell_dropfile.py`

```bash
python3 tests/check_shell_dropfile.py --build build \
  --rom baserom.us.v80.z64
```

Q2: the NSOpenPanel and typed-path ROM-acquisition arms call `RomPanel_setRom()`
directly and are unit-tested (`tests/test_app_shell.cpp`); the drag-and-drop
arm is entered by an `SDL_DROPFILE` handler and had no coverage of its own.
`MDKR_APP_SMOKE_DROP=<path>` (inside the existing `MDKR_APP_SMOKE_FRAMES`
launcher smoke) queues that event for `AppHost::pumpAndShouldQuit()` after
SDL's platform translation boundary, where it uses the exact live-event
handler and ownership contract. It is not passed through `SDL_PushEvent`:
reserved platform events cannot be portably round-tripped through SDL2-on-SDL3
compatibility layers. The gate drives the handler both directions: a supported
ROM must be accepted (same path back, `valid=1`, the picker's own verdict
message, and persisted to prefs), and a non-ROM file must be refused gracefully
(`valid=0`, an explanatory message, exit 0, nothing persisted) — no crash
either way. Every run points `MDKR_APP_PREFS_DIR` (a test-only override
alongside `MDKR_SAVE_DIR`) at a private temporary directory, so this is the one
check that can reach a successful `AppConfig::save()` without ever touching the
real machine-shared `SDL_GetPrefPath("mdkr64","mdkr64")` prefs file.

### Skip the launcher — `tests/check_launcher_skip.py`

```bash
python3 tests/check_launcher_skip.py --build build \
  --rom baserom.us.v80.z64
```

Issue #60. `Launcher.SkipWhenReady` (off by default) trades the launcher for a
faster start; the gate spends five of its seven arms on the way back, because
that is the part whose failure strands a player. `main()` takes one decision
before the first frame and logs it in full (`[app] skip-launcher setting=…
shift=… shoulderL=… shoulderR=… holdOpensLauncher=… armed=…`); when it arms,
`Launcher::draw()` calls the same `RomPanel_requestPlayValidation()` the Play
button calls, so the mandatory final ROM check still runs and only its verdict
publishes a boot. `MDKR_APP_SMOKE_SKIP_LAUNCHER=1` adds no behaviour — it keeps
the headless launcher smoke rendering until a Play action arrives or a deadline
passes (hashing a 32 MiB image outlasts four frames) and prints one parseable
report.

The hold is sampled **every launcher frame until the boot dispatches**, not
once at startup, and the first sighting disarms the skip one-way. SDL folds
keyboard state from events, so a Shift already down before the window existed
is invisible to a single sample taken at window creation — on macOS the first
thing SDL learns about that key is its release. Re-sampling makes the window a
player has to aim at "while the launcher is on screen", which is at least as
long as hashing the ROM takes, rather than one unhittable instant.

Arms: the setting on and nothing held **boots**, carrying the remembered ROM;
Shift held, or both shoulders held, opens the launcher instead; the shipped
default (setting absent) does not boot, which is the positive control proving
arm 1's boot came from the setting; **one shoulder alone still boots**, the
positive control proving the two hold arms were stopped by the hold policy and
not by the mere presence of `MDKR_APP_TEST_LAUNCH_HOLD` (which injects the raw
hold, never the decision — `@<sample>` makes it appear only from that sample
onward — so `AppUi_launcherHoldOpensLauncher()` runs for real in every arm); a
hold that appears only from launcher frame 3 **arms and then disarms without
dispatching**, which is the arm a one-shot sample fails and the reason the
per-frame sampling exists; a remembered file that is not a ROM arms but never
dispatches; an inherited `MDKR_APP_BOOT_RECOVERY` message arms but never
dispatches — that one is the infinite-relaunch guard, since a failed boot
relaunches the app carrying exactly that message; and a **teardown** arm holds
a real controller (attached by the virtual-gamepad smoke contract) through a
launch that never dispatches, then requires every release carrying handles to
have happened while `SDL_INIT_GAMECONTROLLER` was still up. `main()` owns
`AppHost` by value and calls `host.shutdown()` — reaching `SDL_Quit()` — before
its scope ends, so `~Launcher`'s release backstop runs after SDL freed the
devices. That is asserted as an ordering rather than by a sanitizer on purpose:
**ASan does not catch it on SDL 2.x**, because `SDL_GameControllerClose`
validates its argument against an internal list `SDL_Quit` has already emptied
and drops a stale handle without dereferencing it. Confirmed by building it and
looking, not assumed. The pure policy, including
every readiness field, is unit-tested in `tests/test_app_ui_policy.cpp`.

`user_paths` is the ROM/SDL-window-free packaged-data contract. It supplies a
deterministic preference provider to a synthetic `.app`, verifies that video
config and the complete known save set migrate outside the bundle, checks
Resources and legacy CWD fixtures remain byte-identical, proves existing
destinations and explicit `MDKR_VIDEO_CONFIG_PATH`/`MDKR_SAVE_DIR` overrides
win, and exercises the fail-closed preference-discovery path.

### Cooperative final shutdown — `tests/check_final_shutdown.py`

```bash
python3 tests/check_final_shutdown.py --build build \
  --rom baserom.us.v80.z64
```

The gate runs finite GL and WebGPU sessions in isolated save directories. It
requires exactly one terminal marker in the order headless boundary → audio →
backend → frontend → SDL, with zero live backend/frontend objects. Positive
controls reject nonzero ownership and reordered teardown. Run it against
Debug, Release, and ASan after changing exit, renderer, SDL, or audio
ownership.

### Repeated browser resource ownership — `tests/check_browser_resource_plateau.py`

```bash
python3 tests/check_browser_resource_plateau.py \
  --engine-dir build-web --shell-dir dist/web \
  --rom baserom.us.v80.z64
```

The gate runs four real race loads through production pause-menu restarts in an
isolated Chromium profile. It requires stable warmed game/audio heaps, exact
physical/music/jingle/SFX voice-state conservation, coherent non-growing
WebGPU generations, stable pointer-registry ownership, and zero terminal
frontend/backend/host/AudioWorklet ownership. Its synthetic controls prove that
missing generation evidence and a false voice-conservation row are rejected.

The ordinary `browser_runtime` task also owns the temporary authored-cadence
containment budget: exactly one NTSC/60 source-clock initialization, no
sub-two-field update, 24–36 FPS median, 40.0 ms p95, 45.0 ms p99, and at most
two render frames for either async pipeline completion or a consecutive
last-complete-frame hold. It reports the raw maximum and frame number without
using that scheduler/menu-transition
outlier as a pipeline proxy.

`check_webgpu_fault_matrix.py` cross-checks the public 133-point registry
against the executable runtime matrices. All 103 native-required points must be
named by `check_webgpu_recovery.py` or `check_app_adopted_pacing.py`; all three
browser-required points (one browser-only and two with intentionally different
native/browser policies) must be named by `check_browser_runtime.py`; and none
of the 29 dormant inherited routes may receive runtime-coverage credit.

Wave 2 adds the ROM-free `level_lighting` CTest and
`check_remaster_lighting.py`. The latter runs Ancient Lake and Fire Mountain
through both shipped backends, requires a normalized and restrained
header-derived rig, proves smooth normals reach racer and character fragments,
compares production against the baked control, and requires Pure/Restored to
remain byte-identical when the diagnostic seam toggles.

## World-depth visual gates

The world-depth epic adds the first three checks below; the fidelity and
presentation waves extended the same list, which ends with its browser
production coverage:

- `check_world_fx_capture.py` proves the typed capture instrument is neutral
  and depends on the exact matrix registry;
- `check_world_fx_matrix.py` measures bounded, backend-identical ownership over
  bright/dark/snow/water/hub/boss and 2P/4P content, including exact map count
  and memory policy;
- `check_world_shadows.py` proves visible map darkening, actor projected-decal
  release, exact gameplay-state equivalence, and byte-identical fallback under
  forced optional-resource loss — and repeats the whole off/on pair per backend
  in **shipping configuration** (`MDKR_TRACE` absent), requiring the same pixel
  handoff and a byte-identical frame;
- `check_state_hash.py` anchors the fidelity architecture's Phase 1
  instrument: two identical runs must produce byte-identical per-tick
  `[SIMHASH]` streams (`MDKR_STATE_HASH=3`; versioned 64-bit hash over the
  authoritative runtime contract), a different window size AND the
  other renderer backend must not change one bit of authoritative state
  (the first machine-checked slice of the presentation-invariance
  invariant), and the `MDKR_RNGSEED=legacy` positive control must diverge
  at tick 0. Three process-determinism arms re-run levels 41, 37 and 11 and
  require each to agree with itself. The `sim_sched` CTest covers the
  exact-integer fixed-step accumulator (hour-long zero-drift NTSC/PAL,
  catch-up budgets, stall rebase, rational alpha).
  **Field-set versions.** The gate runs **v3**. v1 and v2 remain selectable
  byte-for-byte for archived comparisons; v2 first widened the object/particle
  integrator and exposed ten process-nondeterministic levels that v1 missed.
  v3 adds globals and progression, behavior properties, interactions, racer
  physics/AI/controller state, model animation cadence, and the former
  render-owned distance/opacity/LOD fields. It excludes host pointers and
  presentation-only caches. Nine independent controls use
  `MDKR_TEST_HASH_PERTURB=<family>:<tick>` to flip one covered byte only while
  hashing and require exactly one changed row; the archived v2/v1 x-rotation
  control remains. The exact field table and exclusions live beside the
  implementation in `platform/sim_hash.c`.

  **Current production policy:** Original gameplay cadence remains authoritative.
  Non-Original Frame Limit values schedule additional presentation/input-pump
  opportunities. With MotionSmoothing=off they hold the last authored image;
  with MotionSmoothing=interpolate they may submit a complete immutable
  in-between image. Presentation cannot issue a simulation ticket, consume an
  input ticket, advance audio, or mutate the v3 authority stream.
- `check_sim_hash_artifact.py` covers the on-disk artifact form of that same
  `[SIMHASH]` stream and the tool that compares two of them. With
  `MDKR_STATE_HASH_FILE=<path>` set beside `MDKR_STATE_HASH`, the engine mirrors
  every stdout `[SIMHASH]` line into the file byte-for-byte and in order; the
  parity arm proves the file equals the run's own stdout stream exactly, so the
  artifact is a faithful second sink that changes neither what is hashed nor the
  simulation (an unwritable path is reported to stderr and the sink dropped,
  never fatal). The comparator, `tools/online/compare_sim_hash_artifacts.py`,
  exits 0 only when two artifacts are non-empty, the same length and byte-equal
  modulo line endings on every tick line (so a macOS LF and a Windows CRLF for
  the same tick are not a divergence), and otherwise names the first divergent
  tick or the
  truncation point; it fails closed on a missing file, an empty file, an
  unparseable line and a length mismatch, each with its own message. These are
  the pieces a cross-OS / cross-region determinism comparison stands on.
- `check_render_purity.py` is the fidelity spec's §12.2.1 gate. Skipping half
  of all scene renders (`MDKR_TEST_SKIP_RENDER=odd`) must leave the raw v3
  `[SIMHASH]` stream byte-identical. There is no test-only state subtraction:
  HUD and ordinary-object texture dice execute in fixed authority with
  cadence-compatible RNG ownership; opacity and racer LOD are draw-local;
  animation cadence, visibility, distance/order, light phase, and the
  course-arrow timer advance once per fixed tick. An explicit
  `MDKR_TEST_RENDER_IMPURITY=1` gameplay-RNG write must make normal and skipped
  schedules diverge, proving sensitivity. The gate's first runs root-caused the
  skydome camera-follow write and two 1-ULP add/subtract restore pairs on racers.
  Phase 3 Wave B added three internal arms on the same binary. All require
  `MDKR_INTERNAL_TEST_TOKEN=mdkr64-presentation-replay-v1`; without that exact
  token every replay flag is inert. **C** (`MDKR_TEST_REPLAY_WALK=1`)
  re-walks every tick's captured display list a second time through the HLE,
  with the frozen shadow-matrix registry restored and the same
  view-projection, and requires the `[SIMHASH]` stream to stay byte-identical
  to arm A's; **D** dumps frames from that same replay and requires them
  byte-identical to a non-replay run, catching a near-but-not-exact redraw
  arm C alone cannot see (root cause of its first failures: the replay was
  lighting the scene with a shadow map one tick ahead of the geometry casting
  it, since `gfx_end_frame` swaps the caster read index before the replay
  runs); **E** (`MDKR_TEST_REPLAY_WALK=recompose`) forces every gameplay
  matrix through the `world x view_projection` recomposition slice 2's camera
  interpolation depends on, camera still unchanged, and requires state and
  pixels untouched — its first run found 5178 of 35119 matrices did not
  recompose back to their own display list, the path camera-only
  interpolation now verifies per matrix before trusting it, falling back to
  the list's own matrix when the decomposition does not hold. These remain
  versioned broken-direction controls; production uses the same retained HLE
  replay machinery only after the complete task/dependency transaction and
  adjacent-state publication succeed.
- `check_dl_high_water.py` covers the authoring side of the same overflow.
  `gDisplayLists[]` is one allocation whose Gfx region is immediately followed
  by `gMatrixHeap[]`, so writing past `gCurrNumF3dCmdsPerPlayer` commands does
  not fault — it overwrites matrices, and the walkers above then parse matrix
  words as commands. Nothing measured that margin before. `gfxtask_run_xbus`
  now compares the authored length against the row the buffer was allocated to
  at the one point both are known, reports every new high as
  `[TRACE] dl_high_water: bytes=.. limit=..`, and aborts with
  `[FATAL] display list overflowed its heap row (bytes=.. limit=.. commands=..)`
  past it. Submission is also the only consistent sample: the row and the write
  cursor change together, in `alloc_displaylist_heap`. The gate runs a retail 1P
  time trial twice. Unmodified it must emit the witness, report a limit that is
  a real `gNumF3dCmdsPerPlayer[]` row, and stay under it — measured 12,496 of
  36,000 bytes. With `MDKR_TEST_DL_HIGH_WATER_LIMIT=64` the same route must exit
  nonzero having printed the `[FATAL]` line and a witness for the length that
  tripped it, which is the control that the assertion is fail-closed rather than
  a print. `MDKR_TEST_UNDERSIZED_DL_HEAP` authors its overflow on purpose, so
  that route reports through the witness and runs on into the walkers instead of
  aborting; `check_fast3d_dl_hardening.py` stays green and its injected arm now
  measures 74,088 bytes against a 36,000-byte row. The four-player soak evidence
  lives with the rest of AP-19 below.
- `check_fast3d_dl_hardening.py` covers the renderer's side of the 4P party-hub
  crash settled under AddressSanitizer.
  The authoring defect (a four-viewport party sized against the retail 1P
  display-list budget) is fixed at its cause; this gate proves the two walkers
  survive such a stream instead of parsing the bytes after the buffer as
  commands. `MDKR_TEST_UNDERSIZED_DL_HEAP=1` restores the 1P sizing so the
  overflowing stream is authored on purpose: the run must still reach hub,
  lobby and race, and both the overlay prepass and the interpreter must report
  that they stopped at an opcode the interpreter does not implement. The same
  party route without the injector, and a retail 1P time-trial route under
  `MDKR_DL_CENSUS=1`, must report zero `[DL]` lines and zero censused faults —
  the refusals never fire on properly authored content, which is also the
  control on the opcode set itself. Two positive controls bound the injection:
  the misauthored run's peak submitted display list must exceed the 4500-Gfx
  1P budget it was held to, and the well-authored run's must exceed it too, so
  the arms differ only in the heap the game wrote into. The pure rules both
  walkers share (`platform/fast3d/gfx_dkr_dl_guards.h`) are unit-tested by
  ctest `fast3d_dl_guards`, which carries its own control: the rule it replaced
  — `room >= need` against `dkr_arena_room`'s SIZE_MAX "extent unknown" answer
  — admitted every non-arena pointer at every read length. The opcode set is
  one X-macro list, `DKR_DL_IMPLEMENTED_OPCODES`, that both the predicate and
  the interpreter's dispatch gate read, so the two walkers cannot come to
  disagree about which opcodes are commands; `check_webgpu_content_census.py`
  binds that list to real content (46 routes under `MDKR_DL_STRICT=1`, zero
  faults).
- `fast3d_dl_hardening_asan` runs the same script with `--injected-only`
  against the ASan build. The release arm proves the walkers stop and the route
  survives; it cannot prove what the reads were. A command fetched one past the
  end of an 80-byte global, and a fault printer quoting the words of an address
  the walk had just refused, both land in mapped memory in a release build and
  report success — measured: with the printer fix reverted the release arm
  still passes and this lane aborts under
  `ASAN_OPTIONS=abort_on_error=1`. Same relationship as
  `live_toggle_settings_asan` and `widescreen_shadow_asan` to their release
  arms.
- `check_camera_snapshot_coverage.py` closes the non-sequential camera-ID
  boundary with real content. A two-player race must capture/interpolate camera
  1 in the lower half, and the production 3P HUD toggle must replace the minimap
  with the time-trial spectator on camera 3 even though `gNumCameras` remains
  three. A real new-game cinematic must also retain and interpolate authored
  cutscene-bank camera 4 after the lifecycle flag is cleared. The split-screen
  viewport crops must contain live intermediate pixels while their frozen
  controls contain none; the WebGPU cinematic must provide its own midpoint
  witness. A semantic VP observer additionally requires every alpha-zero
  override to equal the retained task's captured authored VP byte-for-byte and
  carries each alpha-one target forward to prove it becomes the following
  task's exact alpha-zero VP. A one-bit mutation control must be rejected for
  every observed endpoint. Production latches the complete recipe and canonical
  VP at display-list projection emission; only matrix keys consumed by the real
  HLE walk may receive a replay substitution. The shadow-frame unit preserves
  an unwalked build-only matrix while overriding its walked sibling, and the
  snapshot unit separately proves a camera-bank switch is a cut, never a
  cross-bank blend. Snapshot overflow must remain zero and fixed-ticket/present
  accounting must be exact. The split-screen arms use diagnostic GL for fast,
  synchronous PPM reads; the cutscene arm uses the shipping WebGPU backend.
  Its long adventure arm continues from the frontend through hub and lobby
  doors, a race and the post-finish spectator camera. Across those level
  lifetimes it additionally requires zero uncaptured replay dependencies, zero
  replay refusals caused by them, zero unconsumed camera-cut notes, and bounded
  held-frame shares for world scroll, water/waves and object roots. Raw
  blend/snap counts are used to recompute each reported share before the bound
  is trusted.
- `check_hud_render_authority.py` drives the race-start and wrong-way HUD state
  machines through 1P, 2P, and 4P routes. Normal presentation and a schedule
  that skips every odd scene draw must produce byte-identical raw v3 state,
  ordered event, and HUD-action streams. The gate also requires the single
  READY/GO cue, the correct 1P/2P music-start or 4P music-mute branch, and a
  forced wrong-way nag timer that completes. This protects the fixed-tick owner
  from silently drifting back into HUD rendering.
- `check_widescreen_hud_scope.py` is a regex-over-source gate (the
  `check_ci_contract.py` house style) that keeps the opt-in widescreen HUD
  offset (`Video.WidescreenHUD`, off by default) confined to the HUD. It
  asserts that every glyph of the race banana counter shares the lap counter's
  right-edge anchor in `hud_widescreen_anchor()` -- so the "x" glyph cannot
  drift out of its group into the lap counter -- and that the standard centered
  ortho is restored at the HUD -> dialogue-box boundary in `thread3_main.c`,
  gated on `hud_widescreen_enabled()`, so the dialogue boxes (including Taj's)
  keep their authored centered 4:3 placement while the widescreen-HUD-off
  default path emits nothing new (#50). It further asserts that the restore is
  confined to `gGameMode == GAMEMODE_INGAME` -- the only mode that emits the
  expanded WIDE_HUD ortho -- so the intro/menu/lockup dialogue path is left
  untouched. A self-test replays all three assertions against the pre-fix source
  to prove they fail red. Since the issue-#51 batch it additionally requires
  every `HudTypes` element to carry an explicit per-mode row in the
  `sHudWidescreenAnchor` table (totality against a golden snapshot) and
  censuses every draw-side `gHudOffsetX` consumer against the
  `hud_slide_draw_offset()` owner list, with red self-tests per assertion.
- `check_widescreen_hud_layers.py` is the pixel companion the scope gate
  cannot be: the first check that actually enables `Video.WidescreenHUD` and
  measures rendered frames. Its arms pin the issue-#51 fixes -- time-trial
  lap rows laid out disjoint and ordered (labels no longer overprint the
  times), the TAJ MAGIC identity label centered, the battle-mode HUD strip
  at its authored center, and zero HUD pixels parked in the right expanded
  gutter during the race-start pre-slide hold -- plus a 4:3 rail where the
  widescreen-ON frame must stay byte-identical to OFF. Detector self-tests
  run against synthetic rasters on every invocation.
- `check_split_screen_backdrop.py` pins issue #61's split-screen sky. Two or
  more viewports never reach `skydome_render()`; they get
  `trackbg_render_gradient()`, one flat quad whose authored `+/-200` by
  `+/-150` extent is exactly a 4:3, 60-degree-vertical-FOV frustum at its
  261-unit depth (`261 * tan(30 deg) = 150.7`, times `4/3` = `200.9`). Under
  the port's Hor+ widescreen projection the frustum is wider than the quad, so
  the sides of the sky kept the clear colour -- black on every level whose
  `voidColour` is `0,0,0`, which is the reported "black lines on each side of
  the skybox ... e.g. Fossil Canyon". The check drives the two-player fixture
  on Fossil Canyon (bright orange-to-yellow backdrop over a black clear) and
  asserts both the derived extent published by `[TRACE] bg_backdrop:` -- 4:3
  must reproduce the ROM's exact `200`/`150`, 16:9 must be `267` -- and the
  pixels: the worst-frame pure-black fraction of each viewport's sky band
  (rows 3%..10% of its own half, clearing the split separator). Measured
  without the widening: viewport 0 12.7%, viewport 1 10.7%; with it 0.2% and
  4.3% (viewport 1's floor is a real shadowed canyon wall). Detector
  self-tests run on synthetic rasters first.
- `check_split_screen_pause_resolution.py` covers issue #61's pause-owner
  distinction. On both WebGPU and GL it pauses the same real two-player
  Ancient Lake race with P1 or P2, in Restored and Remastered. Different panel
  colours prove the requested owner; the shared RESTART RACE glyph contour
  must agree across owners. Each owner's output-resolution UI must gain at
  least 15% edge detail over its own disabled-UI control. Substituting the
  original-bitmap Restored capture for Remastered P2 must fail the font
  comparison, so equal resolution cannot conceal the wrong font path.
  Both racers' simulation streams remain identical across all arms; overlay
  telemetry rejects late world draws and failed output passes. Run with
  `--renderer webgpu` or `--renderer gl` for a subset, or omit the option for
  both. `--keep-frames DIR` retains local-only captures and logs. Acceptance
  only covers the hardware and renderer actually run, not the entire #61
  report or untested Windows drivers.
- `check_split_screen_void_coverage.py` drives the real two-player Walrus
  Cove loop/tunnel route on GL and WebGPU in Restored, Remastered, and Pure.
  Separate P1/P2 witnesses compare production pixels to both the authored
  foreground curtain and a no-curtain control: valid scenery must reappear,
  but coverage over actual holes must remain. The same verdict must reject
  the authored, deleted-curtain, and early-but-depth-writing controls. Pure
  retains byte-identical authored frames; SIMHASH v3 and both racer streams
  must agree within each preset. ROM header metadata selects fixed US 1.1
  witnesses (P1 frame 3061, P2 frame 3100) or European 1.1 witnesses (P1 frame
  2980, P2 frame 3058), and every arm verifies the engine's revision identity.
  US uses ten captures through frame 3412; Europe uses twelve through 3409.
  All byte orders supported by the engine select the same regional plan.
  `--renderer` and `--mode` select diagnostic subsets; the registered serial
  GPU task runs every group. `--keep-frames DIR` retains private ROM-derived
  evidence. This gate requires a human-attested dedicated test desktop and
  does not establish acceptance on hardware where it has not been run.
- `test_void_render_policy.c` is the ROM-free `void_render_policy` CTest
  companion. It covers preset defaults, invalid overrides, both required
  override gates, and which policies draw before scenery. It does not certify
  rendered pixels and requires dedicated-desktop authorization to execute.
- `check_widescreen_minimap_alignment.py` pins the second half of issue #57:
  under the widescreen HUD, billboard-mode ortho sprites bypassed the WIDE_HUD
  matrix's horizontal compression and rendered 4/3 wider than authored, so the
  minimap image's texels slid sideways around its (correct) anchor and the
  driver marker sat ~100 px off the hub island. Its hub arm measures Taj's map
  dot against the island's gate icon, its race arm measures the player arrow
  against the checkered finish tile and the track-loop width (the draw path is
  shared), each ON arm must prove the wide layout engaged and match the OFF
  arm's [PACE] row at the capture frame, and a 4:3 rail requires the ON frame
  byte-identical to OFF. Detector self-tests run against synthetic rasters on
  every invocation.
- `check_save_options_scroll_band.py` drives a seeded save to the Save
  Options screen, scrolls between Game Paks, and asserts no screen-wide
  band appears (issue #52): a glyph landing exactly at the left screen edge
  makes the game emit a corner-swapped rectangle that real hardware's
  span-invalid rule refuses, and the port now refuses identically
  (`[RECT-SKIP]`). The check also requires the skip counter to be non-zero
  (the guard is exercised, not vacuous) and known-good UI pixels to remain
  (an over-skipping regression fails).
- `check_track_preview_time_steady.py` drives Tracks mode to the Ancient Lake
  setup screen and holds there while the preview flyby banks, asserting the
  BEST TIME digit sprites never rock with the camera (issue #59): each
  course-record glyph's trimmed shear spread must stay under 0.8 native
  pixels across the whole flyby. Non-vacuity witnesses require the digits to
  be found in the dumps and the flyby background to keep moving, so a route
  drift or a frozen preview fails instead of passing an empty assertion.
- `check_track_exit_storage.py` drives the Hot Top Volcano destination-(-1)
  out-of-bounds exit through the `MDKR_FORCE_EXIT_LATCH` seam and asserts the
  game reaches the hub instead of the pre-1.5.2 dead-end menu stall
  (`menu_init menuId=8` with no further activity), and that the trophy-storage
  globals survive the transition (issue #55). The dead-end used to return an
  uninitialized menu result; the port now returns the ares-measured us.v80
  value, so the speedrun trick works while the black-screen hang is gone.
- `check_save_write_notice.py` drives a durable save into a read-only save
  directory and asserts the game both logs the failure and surfaces exactly
  one player-facing notice (issue #54): a save-write failure retries via the
  write-relocation fallback and then announces "progress could not be saved"
  rather than discarding the error silently.
- `check_gamecontrollerdb.py` (the `gamecontrollerdb_lint` CTest companion)
  parses the whole shipped `gamecontrollerdb.txt`, requires every `(GUID,
  platform)` pair to be unique, and pins the six curated Nintendo Switch
  Online N64 HIDAPI entries present + behaviorally equivalent to the
  DirectInput entry with no BACK binding (issue #55), so the added lines are
  inert for every other controller.
- `check_live_toggle_settings.py` gates the same settings being CHANGED
  mid-run. `Video.FrameLimit`, `Video.MotionSmoothing` and
  `Video.AllowTearing` apply at the host-frame boundary and
  `Camera.Obstruction` at the next level load, driven headlessly by
  `MDKR_TEST_SETTINGS_TOGGLE=Key=value@tick`, which fires from
  `platform_input_pump()` so an edit lands at the same point inside the frame
  the in-game overlay's would. Every arm asserts the change reached the pacer
  and the swapchain (`[SETTINGS-APPLY]`, `[PRESENT-POLICY] event=live-apply`,
  a re-emitted `[PRESENT-MODE]`), that the v3 state, event and input streams
  stay byte-identical to an untoggled baseline, and that nothing reached
  retired replay state. The **pace** arm drives the settings panel's
  Presentation pace quick choice through the same runtime call the radio button
  makes (`Presentation.Pace=original|smooth@tick`) and owns the property no
  other arm can see: the quick choice writes both pacing keys in ONE
  transaction, so four `[SETTINGS-APPLY]` rows must arrive on exactly TWO
  boundary applies. Two sequential setters would re-latch twice, and one frame
  boundary between them would run with half the change applied. The **camera** arm asserts the LEVEL deferral in the
  direction that fails silently: the census must still report `gate=OBSERVE`
  on every tick between the edit and the level load. The **soak** arm flips
  motion smoothing 24 times across a level load — the stale walk-entry hazard
  the deferred apply exists to close — and runs again on the ASan lane, where
  a stale segment base would be a read of freed level memory rather than a
  plausible image.
- `check_presentation_matrix.py` is the fidelity spec's §12.2.2 gate:
  presentation rate must never move the authoritative tick. All stream
  comparisons use the same **v3** authority contract as state-hash and render
  purity. **Arm A** proves the promoted host-frame driver issues one exact
  two-field ticket per scheduled game pass, with no debt or update-rate
  violation on the original schedule. **Arm B** requires both the per-tick
  `[SIMHASH]` state stream, ordered `[EVENTHASH]` gameplay-event stream, and
  `[INPUTHASH]` consumed-pad stream to be byte-identical across
  `MDKR_PRESENT_RATE` unset, `=30`, and `=60` (`=30` has one presentation
  opportunity per tick and no midpoint; `=60` presents twice per tick).
  **Arm C** is the smoothness
  witness: with `MDKR_PRESENT_RATE=60` every interpolated midpoint must differ
  from both retained endpoint frames. Production exposes the completed real
  walk at alpha zero, then re-walks its immutable private task with camera,
  object, deformation, particle, fade, and effect state rebuilt at the exact
  rational alpha. The
  positive control, `MDKR_PRESENT_SMOOTHING=off`, presents at the same rate
  but must repeat the tick's own image, so every intermediate frame is
  byte-identical to its neighbours — without that control arm C could not
  tell interpolation from any other source of frame-to-frame difference. Arm B
  repeats the unset and `=60` authority endpoints on WebGPU and compares state,
  events, consumed input, PCM, fixed-ticket accounting, and live object replay
  with the GL control. Arm C's long backend-pixel sensitivity battery remains
  GL-specific; the renderer parity/backpressure gates own WebGPU pixels. Arm B
  also requires nonzero generation-keyed root and composed-child ownership,
  exact owner-census reconciliation, and nonzero object matrix rebuilds. Arm C
  adds an object-specific pixel control:
  `MDKR_TEST_OBJECT_INTERPOLATION=off` keeps the identical interpolated camera
  but disables object rebuilding, and the intermediate backend frames must
  change against the fully smoothed arm. The same gate requires a bounded
  retained packet to publish without allocation failure, observes nonzero
  billboard-matrix and world-anchor registrations/overrides, and bounds the
  stale-key tail. Build-time pointers are refreshed with the exact bytes seen
  by the real HLE walk. A delayed alpha-zero GL control injects matrix and
  billboard-anchor rewrites and must reproduce the frozen bytes with matching
  semantic hashes and zero live-pointer fallback. A separate real-time WebGPU
  arm must execute production midpoints from the poisoned-live private task,
  reconcile every queue admission outcome, and perform no runtime GPU wait.
  Synthetic WebGPU overload remains a distinct load-shedding assertion: it may
  hold optional images, but it may not slow or mutate authority.

  The packet also retains tick-stamped model/particle vertex batches under
  generation/model/animation/topology/root-stream keys. A read-only structural
  census follows the already-authored alternate-buffer task without backend or
  game callbacks, publishing the true forward ``{T,T+1}`` deformation/effect
  stream while the frozen task-T owner bindings remain unchanged. Arm B
  requires nonzero future publications and zero capture failures. Arm C proves
  deformation, point-trail XYZ/RGBA, primitive-alpha fades, and shield/effect
  recipes produce independent pixel differences against their exact hold
  controls. Ambiguous stable keys, phase gaps, unsafe stale fallbacks, and
  authoritative-stream differences fail closed. Its
  first run diverged from tick 1345 on two real defects, both about phase
  rather than magnitude: the synthetic-pacing COUNTER was being advanced once
  per present instead of once per tick (its monotonic clamp fabricates a tick
  whenever read without the clock moving), and the authoritative tick index
  was bumped before the interpolated presents drained, so the last input pump
  before a tick was applying the next tick's scripted input one tick early.
  `check_arbitrary_presentation_rates.py` extends the same contract beyond the
  old integer-field grid. Over 600 fixed ticks it requires exact rational
  presentation totals for NTSC `30`, `40`, `60`, `90`, `120`, `144`, `165`, `240`, and a
  deterministic 1000 Hz uncapped stand-in, plus PAL `60` at exactly 2.4
  opportunities per 25 Hz tick. `display-margin` is checked against the
  `display` arm that ran on the same monitor in the same session: it must
  resolve to exactly three Hz below whatever refresh that arm resolved to, and
  produce the exact rational count for it. Every arm must keep v3 state, ordered events,
  consumed input, temporary PCM, audio time, and fixed two-field update counts
  byte-identical to its region's original arm; replay/packet failures and
  deformation/effect-key collisions remain zero. A second forced-WebGPU matrix
  runs Enhanced one-field simulation at original policy, `30`, `45`, `60`,
  `120`, and uncapped. It requires identical state/event/input/PCM streams and
  the exact 300-quantum audio schedule in every arm; a one-quantum timing
  perturbation proves the PCM comparator can detect a real mismatch without
  changing gameplay authority.
- `check_smoothing_stage_bisection.py` is the attribution instrument, not a
  pass/fail question about any one stage. It renders the same route at
  `MDKR_PRESENT_RATE=120` — three interpolated presents per authoritative tick
  — once per stage configuration (all-on, all-off, and leave-one-out for each
  of the seven `MDKR_TEST_*_INTERPOLATION` opt-outs) and ranks the stages by
  frame difference against the all-on arm, so a 120 Hz artifact hunt knows
  which seam to open first. Two routes, because none fires all seven: Jungle
  Falls in-race with shields forced ranks six, and the battle challenge ranks
  the point-trail particle stage that the first route registers but never
  interpolates. Two guards keep a zero readable. Each leave-one-out arm must
  drive its stage's `[PRESENT-PACKET]` reach counter to exactly zero, so a
  mistyped env cannot masquerade as a stage that ran and changed nothing; and
  every stage gets an explicit `fires` / `reached-never-interpolated` /
  `absent` disposition on both routes, with anything but `fires` failing the
  gate for a stage it ranks. Every arm must keep the v3 state, event and input
  streams byte-identical and every authored endpoint frame exact. The same
  gate carries the uncaptured-external evidence: all eleven production arms
  must resolve zero — with the stat field's presence required, not defaulted —
  and the token-gated `MDKR_TEST_UNCAPTURED_EXTERNAL` seam must force the
  interpolated walks to refuse into held authored images rather than read
  live memory. Results in
  `docs/evidence/smoothing-stage-attribution-2026-08-08.md`.
- `check_effect_shell_envelope.py` runs independent shield and magnet pixel
  arms by default and holds each effect shell inside the distance a single
  authored tick can move it. The shield uses its full connected silhouette;
  the level-three magnet's lightning is disconnected, so its arm isolates a
  stable lower-shell region around player one and rejects any centroid that
  approaches the region boundary. A same-route magnet-off reference must have
  zero keyed pixels in that region, proving the measured shape is the effect
  rather than green track art. On the diagnosis route each arm
  measures, at every interpolated present, how far the shell sits from the
  authored pose of the tick its own recipe was copied from, and requires that
  displacement to stay inside the tick's own travel envelope rather than
  merely being small. That is the shape the shipped 1.0.1-1.0.3 defect
  violated: the shell rode a whole tick ahead of its racer at a flat ~51 px on
  a 640x480 frame, which no endpoint check, hash stream or does-this-stage-
  change-pixels control could see, because a wrong pose is as coherent as a
  right one. Each arm independently requires stage overrides and owner-tick
  checks, zero owner mismatches, a non-vacuous pixel area, and no envelope
  violation, so one effect cannot satisfy the other's coverage.
- `check_wave_midpoint_envelope.py` holds the water and lava surfaces to the
  midpoint-sensitivity contract: an image reconstructed between two authored
  ticks has to differ from BOTH of them where the reconstruction lives.
  Replayed content passes every endpoint check ever written, because
  reproducing an endpoint is exactly what it does, so the region under
  measurement is derived rather than cropped — the route runs twice, once
  normally and once with the wave topology guard forced to refuse every pair,
  and the pixels that differ between the two runs at a given present are the
  wave stage's own footprint at that present. The gate first proves the
  control changes nothing else (every authored frame byte-identical between
  the runs), then that the stage reaches the screen at all (a floor on how
  many midpoints the two runs disagree about — which is also what catches a
  topology key that has gone constant), and only then takes the midpoint
  verdict. Whale Bay, not Jungle Falls: an entire Jungle Falls race produced
  zero presents at which wave interpolation moved a pixel.
- `check_smooth_verdict.py` proves the `[SMOOTH-VERDICT]` per-surface-class
  census — the translation of existing replay clause outcomes into one row
  per graded `MdkrSurfaceClass` — is real rather than vacuous, on one Jungle
  Falls scripted race. It requires at least one row for WATER_WAVE,
  OBJECT_ROOT, and WORLD_SCROLL; that every row's blend+snap equals its own
  total; that WATER_WAVE carries the wave stage's real presentation
  ownership (blend at least once, zero NO_OWNER); and, via
  `--topology-negative-control`, that the topology-key guard is actually
  reached (a positive check count) and actually refuses a forced-mismatched
  pair. The default arm terminates on authoritative ticks and permits bounded
  topology-mismatch snaps: skipped draws can retain an older list across a real
  grid-LOD boundary, and snapping is the guard's required answer. Every such
  mismatch must be accounted inside WATER_WAVE's snap count. `--pan-demote`
  and `--sky-midpoint` are optional token-gated arms
  for the pan-rate demotion clause and the skydome midpoint-sensitivity
  witness, respectively; neither runs by default.
  The pan-demotion arm uses the presentation matrix's Jungle Falls window and
  a token-gated forced presentation-yaw input. WATER_WAVE is its deterministic
  positive witness and OBJECT_ROOT its non-demotable control. A confirmed
  WORLD_SCROLL draw must also demote, but UV-scroll confirmation itself is
  host-schedule-sensitive, so a run containing only honest UV_HOLD rows does
  not make this otherwise deterministic gate probabilistic again.
  A companion run supplies the forced-yaw variable without the internal token
  and requires the seam to remain dormant.
- `check_motion_quality_battery.py` states six things Motion smoothing must
  never do, one row each, in the words a player would use: spin a kart the
  long way round (R1), smear a spawn or respawn across the screen (R2), blend
  through a camera cut (R3), let an effect shell escape its racer (R4), run
  the presentation clock backwards or thump on the authored tick boundary
  (R5), and drop a registration role it had been carrying (R6). Each row
  carries a committed, token-gated negative control that reintroduces exactly
  that defect, so a green row is a measurement rather than an absence. Every
  arm additionally keeps the v3 state, event and input streams byte-identical
  with smoothing on and off, so no row can be bought with gameplay divergence.
- `check_presentation_shadows.py` is the focused Ancient Lake visual regression
  for terrain-projected kart decals. It keeps the 3,300-tick v3 state, event,
  and input streams plus every authored endpoint exact, then compares the
  production rigid lateral decal translation against a token-gated historical
  vertex-lerp control. The control must reproduce severe area/residual pulses;
  production must stay below the fixed visual thresholds while preserving the
  receiver-authored Y coordinate and mesh identity.
  `check_presentation_breadth.py` applies that v3 comparison to 17 NTSC/PAL
  content arms spanning every boss/challenge class, car/hovercraft/plane, and
  1P/4P, while also bounding snapshot, replay, matrix-recomposition, retained-
  packet publication, deformation-key collision health, adjacent forward
  capture, rewritten-dependency safety, and the battle arm's point-trail
  registrations. Ordinary production must expose real alpha-zero endpoints,
  perform nonzero immutable midpoint walks, resolve both private-arena and
  copied-external dependencies, and perform zero delayed endpoint redraws.
  `check_presentation_lifecycle.py` covers the teardown half the content matrix
  cannot see: a 2P pause-to-Track-Select path with no following `level_load`, a
  production pause-menu race restart, and the full Adventure loss/post-race/
  lobby/hub return. Original and 60 Hz arms must keep v3 state, ordered events,
  consumed input, and temporary PCM byte-identical while retained walks cross
  every arena retirement/reissue with zero snapshot, packet, freeze, restore,
  or key-collision failure. A one-row hash perturbation proves the comparator's
  broken direction. The dangerous 2P no-`level_load` teardown repeats under the
  linked ASan artifact as its own manifest task, so a retained-pointer lifetime
  regression cannot depend on allocator luck to crash.
- `check_fixed_tick_schedules.py` drives the application with deterministic
  two-, three-, four-, five-, and six-field host opportunities plus periodic
  suspension rebases. Every arm must complete 1,800 exact two-field game
  passes with byte-identical v3 state, ordered gameplay events, consumed input,
  and temporary PCM. It requires real multi-ticket catch-up, intermediate render elision,
  rebase counters, zero ticket/input-queue debt, and zero `updateRate`
  violations. A one-field diagnostic must diverge and be counted; independent
  trace-only perturbations must change exactly one event/input row and zero
  state rows; an independent one-quantum audio timing control must change PCM
  without changing state/events/input. A focused WebGPU arm then runs the real
  two-controller join route through its first race ticks with and without
  five-field debt. Its state, ordered-event, and all-port consumed-input streams
  must remain byte-identical, catch-up/elision must be live, and player 2 must
  contribute real press/release edges. The ROM-free `host_frame_driver` CTest
  independently injects rational
  30/50/60/90/120/144/165/240 Hz, 59.94-like, irregular, burst, rebase, PAL, and
  uncapped-like schedules with exact alpha and long-run drift assertions. The
  `audio_service_clock` proves grouped/split host-time equivalence, catch-up
  ordering, suspension debt retirement, and one audio quantum per two source
  fields even under enhanced one-field game cadence. The shared
  `audio_queue_controller` CTest proves bounded, aligned production across
  simulated 30/60/120/144/240/1000 Hz service schedules, counter wrap and
  stalls. `audio_sink_contract` then opens SDL queue mode with silence and
  requires exact format, active drain, bounded backlog, pause and clear; CI
  uses the dummy driver, while an optional physical-device invocation exercises
  the same path without ROM audio. `audio_volume` independently proves exact
  unity and mute, perceptual gain, bounded ramps, queue-overflow rejection, and
  reconnect crossfade state without SDL or a ROM. The `input_tick_queue`
  CTest covers between-tick tap stretching, independent
  buttons/ports, latest-analog policy, disconnect/reconnect, catch-up ticket
  targeting, target reordering, and overflow-to-neutral behavior.
- `check_shadow_stage_reset.py` proves the static caster cache resets at level
  load in SHIPPING builds: identical terminal `[WORLD-FX] static=` census with
  and without `MDKR_TRACE`, plus a `MDKR_TEST_SHADOW_STAGE_RESET_SKIP=1`
  control whose census must grow (the historical defect was a reset reachable
  only through the diagnostic path, which every other shadow gate accidentally
  enabled). **That accident is now closed at the source**: every gate in the
  shadow family carries a shipping-configuration arm with `MDKR_TRACE` unset —
  `check_world_shadows.py` and `check_shadow_plausibility.py` and
  `check_shadow_visual_ab.py` and `check_presentation_shadows.py` all require
  byte-identical frames against their traced arm, and
  `check_widescreen_shadow.py` (which dumps no frames) requires an identical
  `[SHADOW]` decal mode, heap capacities, overflow-drop and non-decal census.
  A diagnostic variable that reaches the frame buffer now fails the gate
  whatever its mechanism, instead of needing to have been predicted;
- `check_shadow_plausibility.py` asserts the property four separate shadow
  defects have now violated: **every rendered shadow maps to a caster that is
  really there**. Two halves, because the class has two. *Provenance* reads the
  `[WORLD-FX]` census and requires `staleCasters=0` (the tenancy counter — the
  stage cache dedups by raw arena `Triangle` address, so a recycled or
  rewritten-in-place address would otherwise keep casting whatever it held
  first, from wherever that was), `implausible=0`, `allocFails=0`, and a valid
  `[SHADOW-PLAN]` at the exact budget tier the fixture's view count should
  produce. *Attribution* differences a shadow-on/shadow-off frame pair and
  requires the darkened footprint to exist, to stay one-sided, and not to
  swallow the frame — the shape an inverted or pancaked depth axis makes.
  It runs three differently authored worlds at 1P plus the `race_2p_split` and
  `race_4p_split` fixtures, so all three planner tiers (2048px/2 cascades,
  1024px/2, 1024px/1) are covered. Broken direction:
  `MDKR_TEST_SHADOW_BOGUS_CASTER=<world units>` displaces the first admission of
  every static caster, so the depth map holds geometry the object has left —
  that arm must fail, and does (1945 stale admissions at +900 units);
- `check_touch_controls.py` gates the mobile touch layer in real Chromium:
  a desktop arm (overlay hidden, launcher live — the capability-listener path
  must be non-fatal), a persisted-"shown" revival arm (fails on the
  pre-`dc5f83b` shell), a CDP three-finger chord that must reach
  `osContGetReadData P1` with A+R plus a decisive stick and return to exact
  neutral, and a press+release completed between rAF callbacks that must still
  produce one `[INPUTHASH]` press then release through the bounded JS queue; and
- `check_browser_runtime.py` requires the production WebGPU handoff to reach
  complete maps without resource failure or latching while retaining the
  existing cadence, resize, persistence, audio, and fault budgets.

The lighting gate explicitly sets `MDKR_WORLD_SHADOW=0` because its A/B owns
only RL-5 smooth sun and grade. The shadow gate owns the independent production
feature. Keeping these variables isolated prevents one effect from hiding an
inert or overbroad result in the other.

## Restoration/remaster visual gates

The current visual sprint adds seven self-contained gates. Run them directly
during renderer iteration; the complete manifest still remains the release bar.

```bash
python3 tests/check_sprite_layout.py --build build --rom baserom.us.v80.z64
python3 tests/check_intro_shrub_sprite.py --build build --rom baserom.us.v80.z64
python3 tests/check_rdp_interpolation.py --build build --rom baserom.us.v80.z64
python3 tests/check_texture_edge_classification.py --build build --rom baserom.us.v80.z64
python3 tests/check_font_sdf.py --build build --rom baserom.us.v80.z64
python3 tests/check_font_outline.py --build build --rom baserom.us.v80.z64
python3 tests/check_mip_motion.py --build build --rom baserom.us.v80.z64
python3 tests/check_rl1_vertex_colour_ab.py --build build --rom baserom.us.v80.z64
```

`check_sprite_layout` combines unit boundary cases with a byte-order-aware census
of every supported sprite record. It also pins the multi-run census: of 600
frames across 193 sprites, exactly two exceed five tiles (sprite 108 frame 0, the
intro shrubs; sprite 178 frame 6, a burst frame no swept route loads), so that
pair is the whole blast radius of an append-base regression.
`tests/check_intro_shrub_sprite.py` covers the
other half of the same subsystem at the RSP boundary: a sprite wider than five
tiles is emitted as several `G_VTX_APPEND` runs that share one base, so it
renders the authored intro and scores three fixed regions of one deterministic
frame to prove every tile lands in its own quad rather than over the first one.
`check_rdp_interpolation` compares corrected
and exact-legacy gradient arms on both backends and requires timing identity.
`check_texture_edge_classification` does the same for the cutout-versus-blend
render-mode decision: DKR forked `G_RM_AA_ZB_XLU_LINE_MOD` to clear
`ALPHA_CVG_SEL` while keeping `CVG_X_ALPHA`, and reading only `CVG_X_ALPHA` turned
that authored alpha blend into a hard 0.19 alpha test. The gate pins the
character-select frame where the difference lands and bounds it from both sides,
so an inert change and an over-broad one both fail.
`check_font_sdf` runs mode/backend/control arms: Pure and Restored must be
byte-identical, while Remastered must upload derived fields and change only
known text regions. `check_font_outline` is its counterpart for the outline
redraw of the two plain lettering faces: it drives Restored and Remastered on
both backends, requires the outline path to upload atlases, and requires every
changed pixel to lie inside the text rectangle -- a redraw that leaked outside
its glyph cells would move layout, which is the one thing the feature may not
do. Pure stays pixel-identical even with `MDKR_HIRES_TEXT=1` forced, because
Pure is denied structurally rather than by preset precedence. `clippedTexels`
must be zero across all 94 authored glyphs; the control that reverts the
per-cell fit reports 32 clipped texels on real glyph data, so the assertion is
not vacuous. `check_mip_motion` measures temporal second-difference energy
over a fixed 24-frame moving Everfrost Peak window with anisotropy pinned to one.
`check_rl1_vertex_colour_ab` records the three RL-1 arms on Ancient Lake and
Fire Mountain and enforces the measured decision to retain baked colour as the
ambient base. Each gate has a failure direction; none is a screenshot-only
approval.

### Character-select dancer motion — `tests/check_charselect_motion.py`

```bash
MDKR_AUDIO=0 python3 tests/check_charselect_motion.py --build build --rom baserom.us.v80.z64 -v
```

Productizes the ad-hoc analysis that disproved a "dancers static" report
(byte-identical captures across 86 commits, whole-screen motion RMS ~14.1,
dominant period ~19-20 frames via `tools/anim_period.py`) into a permanent
gate — until this existed, a real regression in `obj_loop_char_select()` or
`music_animation_fraction()` would have shipped undetected. It reuses
`check_mip_motion.py`'s six-window ensemble shape (`WINDOW_FRAMES=24`,
`WINDOW_COUNT=6`, `CAPTURE_COUNT=144`): mean whole-screen motion RMS over the
144-frame character-select capture must clear a floor and at least 5/6
windows must individually clear a per-window floor, with a loosely-banded
periodicity assertion (peak autocorrelation lag in [14, 26] frames, r >= 0.3)
alongside it. That band also rejects the historical 8x-too-fast oscillation.
Whole-screen RMS was chosen over the legacy cropped dancer analysis because it
reproduces the original ad-hoc numbers directly and was measured to still catch
a freeze localized to just the character models (butterflies alone left
animating drops per-window motion from a real 5.76-6.90 to 4.24-4.65, cleanly
below the chosen floor). The older rate-only script was retired when its fixed
capture window no longer reached the settled character-select screen; this
gate subsumes both of its useful assertions without weakening either one.

No env-gated animation-freeze hook exists in the engine, and the plan
explicitly prefers an analyzer-level control over adding one for this gate.
The required broken-direction controls are built without any engine change.
The frozen arm replaces every captured grid with frame 0; measured motion
collapses to exactly 0.0 in every window and must fail both motion floors. The
rate arm loops five phases sampled across one healthy cycle, recreating the
historical roughly five-frame oscillation while preserving visible movement;
it must fail the [14, 26]-frame period band. The check's own PASS depends on
both controls being rejected by the same scorer used for the real capture.

### Output-resolution HUD/text — `tests/check_native_ui_resolution.py`

```bash
python3 tests/check_native_ui_resolution.py --build build \
  --rom baserom.us.v80.z64 -v
```

This gate runs GL and WebGPU in Remastered mode at 2x scene resolution, once
with the production output-resolution UI path and once with
`MDKR_UI_NATIVE_RES=0`. It requires identical normalized gameplay state, more
than 500 active output-pass frames, zero world-after-overlay draws, zero pass
startup failures, and no output pass in the disabled control. At frame 3000 it
requires every changed pixel to remain inside the top HUD/minimap regions,
requires center and bottom-left world crops to be byte-identical, and measures
at least 1.15x top-HUD edge energy. Current GL/WebGPU values are 1.270x/1.296x.

The multiplayer gates extend the same ordering contract to 2P, 3P, and 4P:
every required viewport/quadrant must remain live while output UI is active,
with zero late-world draws and begin failures. Each run uses an isolated
`MDKR_SAVE_DIR`; no multiplayer check reads or writes the player's save.

`check_network_viewport_invariance.py` is the rollback authority companion.
It locks AI, world lifetime, particle and vehicle-authoring functions to the
shared canonical-player query and verifies launcher roster install → blocking
engine boot → clear ordering. Its built-in mutations reintroduce a camera
layout read into AI, remove the canonical fish-lifetime query and omit roster
retirement; all three must fail. `net_roster` separately covers slot-0,
slot-1, 2-local and no-render mappings, copy ownership, mapping accessors and
invalid remote viewport admission. These are source/unit foundations; the
real process boundary is owned by `check_online_process_convergence.py`. That
ROM-backed gate launches four isolated production app/engine processes through
the launcher-owned online envelope (slot 0, slot 1, 2-local and no-render),
requires one canonical manifest digest, byte-identical 3,600-row v3/input
streams and identical canonical transition/world/result event projections, and
proves a remote viewport fails before boot. The no-render arm executes zero
world passes and must omit local feedback; sound/music/rumble are intentionally
not match-authority fields. Nonempty view maps are consumed with separate local
output-rectangle and canonical-camera identities. Each mapped world command
uses copy-owned viewport bytes, preventing a later canonical transition from
rewriting the pointer before the graphics task consumes it. A mapped pixel
witness requires slot 0 and slot 1 to differ full-screen, rejects uniform
right/bottom drawable gutters, and requires the 0+2 couch endpoint to contain
two distinct live halves plus a dark production divider. The global minimap
must report the frozen local layout (one-view or couch split), and the zero-view
verifier must emit neither mapped world passes nor global HUD/divider chrome.
Every mapped output must also emit a non-vacuous HUD shadow-reflow witness:
canonical animation deltas are rebased onto the one- or two-view preset without
mutating rollback-owned `HudData`. The same processes require exact endpoint
listener modes: one-view `spatial`, two-view `shared-center`, and no-view
`silent`, while voice-lifetime evaluation reports all four canonical cameras.
The `local_listener_mix` unit pins max-local volume, one-listener pan,
shared-screen centering, verifier silence and malformed-map rejection. The gate
also proves launcher transport convergence: every scripted remote port crosses
authenticated ingress and one delayed edge forces a five-tick rewind before
streams rejoin. The baseline also runs a hostile launch whose frozen manifest
names track 6 while the ROM loads track 5. Protocol v1 admits standard races
only; the engine must reject that mismatch before tick one, exit nonzero without
an abort, and complete owner-last host teardown. `match_manifest`, `net_roster`
and `rollback_game_runtime` separately pin atomic manifest+roster publication,
standard-rules-only validation, track/race-type/regional-cadence matching, and
wrong-value controls. `tests/check_net_roster_owner_guard.py` is a standalone
source proof (host `cc`, no ROM, no CMake target) that a local-Play boot cannot
inherit an online session's process-global roster and stall on network input
that never arrives -- ownership is explicit and the guard is idempotent. Passing
`--profile` repeats all four isolated endpoints under
LAN, regional-good, regional-variable, poor, two-second-outage or adversarial
packet/clock schedules. Ordinary through poor must complete and converge;
outage/adversarial must preserve the exact pre-fault prefix, detect the first
irrecoverable gap at the 31-tick replay boundary, unwind cleanly and enter the
launcher's typed connection-lost scene. Profile-specific counters must be
nonzero, while invalid/stale/unauthorized/conflicting/out-of-window transport
counters and atomic-drain rejection stay zero. The same retirement record must
contain a nonempty capture histogram with p50 ≤ p95 ≤ p99, no 20.48 ms
histogram overflow, and zero capture/restore samples beyond the explicit
8.33 ms p99 and 16.67 ms tail thresholds. A restore histogram is required to
be either internally ordered and nonempty or exactly zero when no correction
occurred. `rollback_ring` also requires fail-atomic rejection when the selected
slot count would exceed the explicit 16 MiB authority-memory cap; the current
32-slot 4P real-game ring is 16,007,200 bytes.

The same four-process gate takes a `--regions pal|cross` arm (with `--pal-rom`
naming the European ROM): the two accepted ROM payloads are byte-identical, so a
PAL endpoint racing the online 30 Hz manifest under the launcher-armed NTSC
source identity must produce authority, input and canonical-event streams
byte-identical to a US endpoint. `--regions pal` runs four PAL endpoints;
`--regions cross` mixes US and PAL across both single-seat mask shapes and the
couch endpoint. Every online epoch witnesses the armed NTSC identity while its
true region stays observable, and no offline clock is latched inside it.
`check_online_region_reentry.py` proves the identity is epoch-scoped: one
process runs an online PAL epoch (NTSC identity armed) then an offline PAL epoch
that re-latches the authentic 50 Hz source clock with the override cleared.
`check_online_engine_boot.py --rom pal --authored-hz 30` asserts the converged
PAL race hash EQUALS the pinned US golden (`--expect-hash`, imported from the
direct-boot gate), i.e. bit-identity with a US epoch. The default
`tools/run_online_checks.py` sweep schedules the US arm of
`check_online_process_convergence.py` (no PAL ROM needed — it guards the
US-only defect class, e.g. a no-render endpoint diverging from its rendering
peers); `tools/run_online_checks.py --pal-rom <pal.v80>` joins all four of
these cross-region lanes to the serial schedule after the default sweep;
without `--pal-rom` the sweep is otherwise unchanged.

`--ai-takeover-slot N --ai-takeover-tick T` overlays one immutable
room-authorized disconnect decision on that same four-process proof. All
endpoints must converge while the chosen slot may be locally owned on one and
remote on the others. Transport reports one activation per endpoint, ignores
post-cutoff peer input only where that slot is remote, authors confirmed neutral
history after the grace interval, and never enters gap recovery. The game keeps
canonical human identity for cameras/HUD/results while routing the vehicle
solver through AI; `test_ai_takeover_adapter.py` mutates the exact-tick replay
mask, temporary CPU dispatch/identity restore, no-double-owner rejection and
no-handback telemetry. The default cutoff is rollback tick 240/process tick
2660, where the checked-in fixture authors an A edge for every player; the gate
requires the selected slot to be present-neutral on that exact row, so the
takeover proof cannot pass by replacing an already-idle sample.

`check_online_character_map.py`, `check_online_racer_lod_animation.py`, and
`check_online_trackselect.py` are further online-takeover lanes owned by
`tools/run_online_checks.py`. The character-map lane pins the lobby-catalog-id to
engine `Character` mapping so a joiner never mis-spawns (the Tiptup-as-Conker /
T.T.-as-Diddy defect). The racer-LOD lane guards racer level-of-detail and
animation across its offline, online-biased, and online idle-host fall-behind
arms (the two-Mac beta-4 T-pose regression). The trackselect lane covers the
online track-selection surface.

`check_rollback_track_matrix.py` runs the same production delayed-correction
and exact-second-replay proof on all 20 standard tracks using a human-driven
car. It asserts the vehicle actually dispatched by the game rather than trusting
the launcher request. It requires 180 authored race
ticks, 189 captures, three restores, a bounded 32-slot ring, ordered nonzero
capture/restore histograms, clean host teardown and zero timing/journal/I/O
overflow per track. `--tracks 5,8` is the focused iteration form; the release
row uses the complete corpus. `check_rollback_vehicle_matrix.py` independently
decodes each track's legal player-vehicle mask from the ROM, exercises all 47
standard-track/player-vehicle rows through the explicit Time Trial route, and
requires the observed in-game vehicle to match each requested car, hovercraft or
plane. `check_rollback_soak.py` then holds Whale Bay's wave/behaviour-heavy
hovercraft route for 10,180 authored ticks with the current ring and the same
exact correction; it also requires an observed hovercraft dispatch. All three
scripts print artifact-sized snapshot, ring and four-part p99 summaries
(`capture/restore/resimulated-game-tick/authored-gameplay-frame`) rather than
relying on historical small-ring prose. Their bounded histograms require p99 at
or below 8.33 ms, no overflow beyond 20.48 ms and no sample beyond the hard
16.67 ms authored-tick deadline. Authored gameplay-frame time ends at the
rollback boundary; renderer/GPU presentation latency is intentionally a
separate platform gate.

`check_rollback_item_matrix.py` adds the item dimension that track and vehicle
breadth cannot infer. It grants one real pre-snapshot inventory charge, then
drives the ordinary Z-release path inside a four-tick delayed correction for all
15 balloon type/level configurations (14 concrete weapon IDs; missile L1 and L3
both intentionally map to the standard rocket). The result must consume the
charge and observe the real boost, shield, magnet feedback, trap spawn or
projectile spawn before an exact second replay. Match-critical item headers,
models, sprites, attachments, textures and explosions are leased before tick
zero; dynamic model instances live in a dedicated snapshotted model subpool,
attachment objects live in the snapshotted object pool, and shared model
reference counters are registered separately so discarded replays cannot leak
references. A suppressed-release mutation must
fail, while an invalid balloon index and an armed non-correction mode must fail
closed. Every row also enforces the 16 MiB ring, timing, journal, forbidden-I/O
and clean-teardown contracts.

`check_rollback_weapon_refcount.py` guards the refcount-erasure class the item
matrix cannot see: it fires the probe missile inside a delayed correction on
Ancient Lake, where the projectile OUTLIVES the four-tick window, so its
snapshot-covered `ObjectModel.references` ++ must survive the correction and
be decremented exactly once by the later LIVE despawn. Resimulation freezes
sprite/texture refcounts (host state outside the snapshot), but the pinned
item models' counters are registered rollback authority: restore reverts them
and the replayed re-application keeps them exact. Freezing those too erased
the ++ of any weapon spawn a correction rewound past, walking the pinned model
to zero and a mid-race free under live draws. The lane asserts the corrected
fire is observed, the exact second replay stays byte-identical, and the
engine's one-sided conservation validator (references never below lease holds
plus live holders, checked every authored boundary) and the mid-race
free-refusal tripwire both stay silent.

`online_lobby_core` is the socket-free launcher room reducer. Native C and the
service TypeScript reducer both consume
`tests/fixtures/online_lobby_reducer_v1.tsv`: one 117-row lifecycle fixture
that asserts the result/error and canonical lobby state after every valid or
invalid transition. Its tests cover
membership, exact compatibility, seat ownership, unique per-seat character and
vehicle selections, selection revisions, Ready invalidation, voting, barriers,
reconnect, leader custody, CAS and retry idempotency, plus the session-config
and tournament columns (mode, configured track, cup, race_index, trophy
points, last placements). `match_launch_descriptor`
then freezes the Loading lobby into a fixed 148-byte manifest+selection record.
It requires stable canonical seat order, a neutral unused tail, legal vehicles,
unique characters and exact lobby/manifest epoch/track/mask/count agreement;
an exhaustive one-byte mutation loop proves decode/checksum failure is atomic.
`session_bridge` and `session_runtime` carry that record in the V3 production
envelope, copy-own it across the persistent launcher/engine loan and reject
wrong epochs, duplicate characters and non-neutral envelope bytes without
partial mutation. V2 remains an explicit manifest-only laboratory path and
cannot expose a launch descriptor. `net_roster` then copy-owns the validated
descriptor for the engine lifetime. `check_match_launch_direct_load.py` proves a
real ROM direct-loads its descriptor track and applies all four canonical
characters/vehicles without `MDKR_LOAD_TRACK`;
`check_online_process_convergence.py --launch-v3` proves four endpoint processes
retain identical descriptor identity while their local controller, viewport,
HUD and listener maps differ safely.

`online_compatibility_identity` and `check_browser_online_activation.py` share
one exact vector for the domain-separated build/gameplay digests derived from
clean release semver and source commit. Native rejects dirty/malformed
provenance and unsupported ROM revisions atomically; browser activation also
requires same-origin policy and a locally SHA-verified supported ROM before any
room request. CTest registers the browser check's `--source-only` arm as
`online_compatibility_source_contract`, so the ordinary non-GPU lane catches
native/browser domain and 32-byte version-bound drift without opening Chrome.

`match_peer_graph` freezes the 2–4 endpoint connectivity policy: mutual edges,
direct-first deterministic one-hop selection, diameter-three refusal and exact
epoch/connection generations. `match_peer_forward` admits only the current
named intermediate receiving from the separately authenticated source endpoint
and connection generation, then applies a bounded replay window without
decrypting or changing ciphertext.
`match_peer_crypto` and `match_peer_crypto_js` lock pinned Mbed TLS and browser
WebCrypto to ephemeral P-256 identities and one direction/generation-bound
HKDF-SHA-256 and AES-256-GCM vector. `match_peer_transcript` shares the exact
room/build/ROM/epoch/sorted-key digest and verification phrase with the browser.
The crypto gates mutate every 132-byte envelope position and require
the complete caller-expected source-to-recipient direction, then authentication,
before plaintext or replay state changes. A valid peer key cannot authenticate
a header claiming another source endpoint or generation. Callers cannot choose
the GCM sequence: a direction-bound seal window allocates it monotonically.
Each of the peer connection's three channels — state, control and authority —
is a LANE with its own derived key, so both gates seal on all three and
require distinct key records, that a seal advances only its own lane's window,
and that every other lane's key refuses the envelope as `WRONG_LANE`. Dropping
the lane from the HKDF info collapses the three derivations onto one keyring
slot and fails that first assertion. The gates cover network reordering, stale
receive-window rejection, corrupt/exhausted sender state, provider failure and
two concurrent browser seals. Native used/exhausted window initialization and a
second browser window for one derived key object refuse; browser state is
read-only. The wire contract is documented in
`docs/ref/match-peer-carrier-v1.md`.

`match_preflight` and `match_preflight_js` share one exact fixed 136-byte `MPF2`
report vector and cover the pure launcher consensus gate over the frozen
launch-descriptor SHA-256, peer-key transcript, order-independent canonical
directed-graph SHA-256, exact epoch/generations, local supported-ROM
verification, phrase confirmation and ready channel graph. It
requires monotonic authenticated endpoint reports; progress, mismatch priority,
readiness withdrawal, duplicate/conflict/stale handling, malformed wire bytes,
all digest-byte disagreements, graph reorder equivalence and reachability
equivocation, disconnected routes, corrupt state and fail-atomic outputs are
executable. A second cross-platform contract carries the report as three
sequence-bound 64-byte payload-type-1 fragments, binds every piece and the
completed report to one carrier-authenticated peer direction, and covers
reorder, exact duplicate, conflict, stale replacement, final-padding,
cross-source splicing and forged-attribution negatives. `READY`
has no engine or network authority.
The two halves also pin the appended twelve-byte route-quality record: the
score/band consistency rule, the refusal of a band that disagrees with its own
score, and the two-way version refusal (an `MPF1` report is named
`legacy_mpf1` rather than downgraded into, and the frozen `MPF1` header rule
refuses an `MPF2` report).
The UX, wire and security contract is documented in
`docs/ref/match-preflight-v1.md`.

`match_route_quality` covers the pre-flight route measurement itself: every rung
of the score ladder at its own boundary and one unit past it, the named bands
over the 1-10 score, degenerate records (a rate above 100% rejects without
mutating the caller's record; a route that lost everything still floors at one),
the entry-timing widen from measured p95 RTT against the manifest floor and its
four-tick cap, the fixed 64-byte probe payload codec, and the caller-clocked
measurement phase replaying both lanes for 6 s with a 1 s drain, and the
queue-drain term (the carrier's own count of inbound pump-drain drops from its
bounded queues, which only the caller can see) reaching the record and its
rung. Its cut arms pin what a race start does to a window still in flight: the
trailing probes younger than the cut margin are dropped rather than scored as
loss, an echo genuinely lost before the cut still is, and a cut window emits no
further probe. A second arm holds the margin to the route — a 240 ms carrier
that lost nothing must measure no loss when the cut lands, where a fixed 100 ms
margin would charge it a third of a second of healthy probes — and a third pins
both adoption floors at their boundaries: 29 answered samples are refused, 30
over a two-second span are adopted, and samples that clear the count but not the
span are refused on span alone. Positive controls, both real mutations: return
`0` from `mdkr_match_route_measure_cut` before it scans and the cut assertions
fail; drop the sample floor to `1` in `mdkr_match_route_measure_adoptable` and
the 29-sample refusal fails. Its impairment lane injects 8% loss on the unreliable bundle lane
through `net_impairment` and requires the `rough` band; the positive control
for that lane is a real mutation of the production ladder — set
`route_steady_floor` in `platform/net/match_preflight.c` to `1`, so every score
bands steady, and "8% injected loss scores in the rough band" fails.

`online_live_route` (`mdkr_online_live_adapter_test --route`) runs the whole
thing over two real loopback DTLS meshes, in three arms. Both endpoints exchange
the record in their `MPF2` reports and must agree on the band, and a LAN-shaped
route must leave the agreed lead alone. A route with a round trip a single
authored tick cannot absorb must widen BOTH endpoints' operative lead above the
manifest floor; returning the floor unconditionally from `setUpRace` fails
exactly those two delay assertions while convergence still passes. The third arm
gives ONE endpoint a slower measured route
(`mdkr_online_live_adapter_test_set_route_measurement`), so the two race with
DIFFERENT operative leads over the same descriptor and must still fold the
identical canonical state hash — the determinism claim the widen rests on.
The fourth arm presses Start into a window that is still open (D-N5): the race
must run on the manifest floor with the room chip reading
"Checking connection…", and the cut window must still be adopted on both
endpoints and still cross the wire as the round's second attestation. Two
positive controls, each failing a different half: make the projection fall back
to an empty chip while the measurement runs and only the chip assertion fails;
restore the old `raceReady_` early return in `serviceRouteMeasurement` and only
the settle/exchange assertions fail. The fifth arm runs TWO races through one
live room on the lifecycle rig: race 1 starts into an open window and both
endpoints adopt a CUT record, the rematch boundary retires it, and race 2 runs
on a FULL one whose chip is a measured round trip rather than the checking copy.
Its positive control is `rearmRouteReportForNextRace` holding a cut record like
a full one — race 2 then still reports a cut record and never measures again.
Which record survives which boundary is pinned at the unit level too, in
`online_live_adapter_beta`
(`mdkr_online_live_adapter_test_race_latch_reset_keeps_route(cut_short)`), whose
control is calling `resetRouteMeasurement()` from `resetRaceLatches` again.
`online_live_adapter_beta` pins that a rekey or a re-verify mid-measurement
restarts the whole route window rather than settling on samples sealed under
retired keys; clearing only the publication latch (the pre-fix shape) fails it.

`online_live_repair` (`mdkr_online_live_adapter_test --repair`) pins explicit
input-gap repair on the reliable unordered authority channel over the same two
real loopback DTLS meshes. The driver suppresses twenty-five consecutive bundle
sends in one direction — far past the carrier's three ticks of redundancy — so
the run of authored ticks it costs can never be covered by a later bundle. That
run must be closed by repair alone and both endpoints must fold the identical
canonical state hash: a repaired input is the same input. That hash is a fold of
the CONFIRMED CANONICAL INPUT timeline read back through
`race_inputs_for_tick`, not engine or simulation state — this lane is ROM-free
and proves the two endpoints commit the same inputs, which the rollback and
SIMHASH lanes then turn into the same simulation. Two positive
controls. Turning repair off with
`mdkr_online_live_adapter_race_set_repair(adapter, false)` and replaying the
identical burst must reach typed `INPUT_GAP` rollback exhaustion and never
converge. And the burst must not age out a valid authority packet: more than
one replay window of state envelopes crosses the mesh while repair answers are
in flight, and every answer sent must be received. `match_peer_transport` pins
that second property at the transport boundary with its own control — pointing
the authority lane's replay window at the state lane's drops the delayed repair
outright. The impairment matrix (`--matrix`) carries the same pairing: its
two-second-outage profile is a loss run far past the bundle's redundancy, so it
now converges with repair (and must show repair restoring ticks) and runs a
second time on the identical seed with repair off, where it must still latch
`INPUT_GAP` at the 31-tick replay boundary.

Two further arms cover the request policy itself. The author's drain is frozen
for the first half of a burst, so the requester's first ask names ticks the
author has not committed and cannot answer, and its sends stay suppressed for
the second half, so the ticks it then commits never arrive as bundles: only a
re-ask can close that run. With the shipped eight-authored-tick re-ask window
the race converges; with the window pushed past the whole race — the single-shot
latch it replaced — the stranded gap ages into `INPUT_GAP`. `match_input_repair`
pins the responder's side: one honest request's worth of answers is always
charged successfully, a flood past the per-tick budget is refused however many
different runs it names, and the budget refills only when the responder's own
authored tick advances.

`check_rollback_authority_wrapper.py` is the suite-facing entry for the frozen
mutable-authority census and its omitted-state positive control.

`online_lobby_view_model` is the socket- and UI-toolkit-free ON-03A projection
over those actual session/lobby snapshots. Every entry, room, preflight,
selection, loading, countdown, racing, results and recovery view emits bounded
title/explanation/status copy, typed primary/secondary/Cancel controls, live
announcement priority and any timeout outcome. Its exhaustive failure table
keeps raw provider/transport terms out of player copy, maps unknown reasons to
one actionable fallback, preserves **Play Here**, rejects mismatched snapshots
fail-atomically, and proves room data cannot expose **Start Race** before the
local release gate (which also requires leader, 2+ members and everyone Ready).
It also pins the one route chip in all three of its states: a measured record
reads "~45 ms · steady", a measurement still running reads
"Checking connection…" (Start is never held for it, so a player reaches the chip
before it settles), and neither reads as nothing at all. Its positive control is
a real mutation — make the projection return an empty chip while the
measurement runs and the checking assertion fails on its own.

`check_app_background_activation.py` (registered as
`app_background_activation_contract`) and `app_window` enforce desktop-safe
native automation. `MDKR64_HIDDEN=1` creates a background GL/WebGPU surface,
sets SDL's background/no-activation hints and AppKit's accessory policy before
`SDL_Init`, skips macOS application/key-window activation and every window raise, ignores
OS fullscreen effects while preserving the settings transaction, and prevents
persisted fullscreen from affecting creation. Automation windows render hidden
or ordered behind the desktop by design (set `MDKR_TEST_VISIBLE_HEADLESS=1` to
watch one), and the release gate is the complete suite wherever it runs.
Use `ctest -LE 'gpu|app_process|browser'` for a suite
that cannot launch the production app or create any native launcher surface;
the `gpu` lane remains real
render/input evidence but must not steal focus or switch Spaces.
`online_lobby_fake_adapter` is the deterministic ON-03B effect layer beneath
the real views. It uses the actual session and room reducers for create/join,
invite, preflight, selections, vote, Ready, loading, racing, results and a
second round. Revisioned UI requests make exact double actions idempotent and
reject conflicting/stale/route-confused mutations; versioned callback tokens
make load cancellation terminal. Its vertical fixture cancels one epoch, races
2 more with fresh identity, preserves both members and proves a stale engine
result cannot finish the current race. It also exercises a typed $0-capacity
refusal whose recovery remains **Play Here**.

The ON-03C native surface starts with four fixture-paired CTests:
`app_online_room_smoke`/`app_online_room_content` render the 1280×720 launcher,
while `app_online_room_minimum_smoke`/`app_online_room_minimum_content` render
640×480 at 200% UI scale. The minimum producer sends a real SDL touch drag
through ImGui to the active panel's scroll owner and captures the below-fold
primary action. Both consumers reject stale/missing producers, blank or
offscreen content, lost contrast and absent action color. The ordinary Settings
handheld gate uses the same active-panel scroll seam, preventing Online Room
support from weakening the existing input proof. Run the focused contract with:

```bash
env MDKR64_HIDDEN=1 MDKR_AUDIO=0 \
  SDL_MAC_BACKGROUND_APP=1 SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1 \
  ctest --test-dir build --output-on-failure \
  -R 'app_online_room_(smoke|content|minimum_smoke|minimum_content|gallery|accessibility|actions)'
```

That focused command is GPU/UI evidence; a suite that must not touch those
lanes retains `-LE 'gpu|app_process|browser'` instead.

The pure `online_lobby_browser_wasm` test compiles the Emscripten-facing C ABI
as an ordinary native process. It enumerates all 43 authoritative cases, all 10
view kinds, 18 failures and 27 enabled actions; drives every action; proves
timeout ownership and both explicit phrase decisions; and verifies invalid
gallery/live projections are fail-atomic. This is ROM-, renderer-, provider-,
SDL- and browser-free, but it is still a compiled executable and is therefore
queued—not run—under the maintainer workstation's current no-executable-test
policy.

`online_room_presenter_js` asks that same executable for a bounded JSON dump of
all 43 projections, then runs the exact pure presenter loaded by the published
browser shell in Node. It independently checks all 27 action routes, all 18
failure values, action/selection order, timeout priority, singular/plural count
copy, **Play Here**/**Choose ROM** live recovery, immutable output, malformed
model rejection and the complete phrase/mismatch announcement. This strengthens
browser semantic evidence without claiming DOM layout, actual accessibility-tree
behavior or input dispatch; those remain in the Chromium gate.
The same test enumerates the live policy for all 27 action ids: a route is
admitted only when it is currently rendered and enabled, production setup cannot
borrow the trusted fixture's simulated preflight, and carrier/race-dependent
routes remain explicit locked outcomes. The runtime consumes this policy before
performing any launcher effect or request; production callers also cannot select
gallery fixtures.

`online_room_live_state_js` owns the browser's other pure boundary: the exact
public MatchRoom snapshot and its launcher-only identity envelope. It rejects
unknown/root/private fields, partial or changed credentials, incompatible
builds, malformed U64s, impossible member/seat/selection/phase state, broken
control-tail continuity or final-state disagreement, regressing/equivocating
publications, cross-origin or malformed invitations and stale invite generations.
Accepted input is deep-copied into an immutable canonical state. Invite secrets
use a receipt-relative local deadline with a 5%/30-second early-expiry margin,
so display clock skew cannot keep a QR alive; expiry destroys the secret before
rendering and projects an explicit **New Invitation** recovery for the leader.
Valid fully older HTTP publications are no-ops, mixed advancement/regression is
equivocation, and a rotate secret crossing a newer socket publication must match
the exact in-flight expected/current generation and current leader custody.
Replay cannot extend local expiry; terminal close may revoke the generation's
service deadline.
The exact 21-field C ABI v4 projection, owner/ready/loaded masks, three-state
invite custody and room/service phase correlation are checked without DOM,
network, Wasm or a game process. The browser transaction restores prior state
on projection/render failure but never resurrects an expired secret. A successful
guest leave is terminal because the service revokes that credential in the same
commit; it is not followed by a predictably unauthorized refresh. This gate is
registered but must be executed with the deferred executable-validation batch
while the maintainer's workstation is occupied.

The executable also publishes a versioned 43-case gallery built only through
fake-adapter commands/callbacks/reducers. `app_online_room_gallery` renders each
case in an isolated profile and requires all 43 captures to be non-flat and
byte-distinct. `app_online_room_accessibility` walks every case with actual SDL
keyboard navigation and requires its title plus every visible action to be
spoken; recovery titles must be assertive. `app_online_room_actions` discovers
all 27 action ids from the executable and activates one valid rendered instance
through both keyboard and a virtual gamepad. Reducer tests separately prove the
confirmed Leave Race effect ordering and that preflight retry cannot create a
replacement room. `check_browser_online_room.py` qualifies the browser's
zero-I/O release gate, launcher-owned ROM/Phone Party handoffs, Escape focus
restoration and 320×568/200% reflow. `check_browser_online_room_gallery.py`
then loads the isolated sub-128-KiB shared-C projection (never the ROM, game engine
or a provider), compares all 43 browser cases with the native executable,
activates all 27 typed routes through focused native keyboard controls, and
proves touch, recovery accessibility and every-case portrait/landscape 200%
reflow (down to 320×568). These
are product-input gates; none enables production race admission.

`check_browser_online_activation.py` qualifies the publisher-to-launcher seam
without a hosted service. The shipped static policy is disabled and leaves the
projection unloaded. When the fixture supplies an enabled same-origin policy,
a clean 40-hex source provenance and a locally verified supported ROM, the
launcher derives a versioned build id plus gameplay-contract digest and loads
the model exactly once. US 1.1 maps to revision 1/30 Hz and PAL 1.1 to revision
2/25 Hz; dirty provenance and cross-origin service targets fail closed. This
activation performs no `/api/` request and still cannot enable race admission.

`check_browser_online_match_room.py` enables the otherwise-inert live adapter
with a MatchRoom-shaped seam. It proves link/code create and join, invite QR,
generation rotation, publication-driven and timer-driven expiry, immediate old
secret removal, membership/rotation and leadership-loss races, leader-only
replacement, missing-QR fallback, local selection/Ready, generation-guarded
state-socket reconnect, synchronous adapter-construction failure recovery,
five-attempt socket-flap exhaustion, explicit Retry,
accepted host-close without a doomed state refresh, corrupt snapshot recovery,
secret-fragment erasure before request and accessible
action names. Unknown service fields and superseded callbacks remain powerless:
the exact boundary rejects them, while the shared C projection receives the
locally false admission policy and never exposes Start Race.

`tests/check_browser_online_two_person.py` starts the real local Worker with
static assets and fresh Durable Objects, then drives two isolated Chrome
profiles through create, QR/code/link sharing, `/room/` fragment handoff,
join, socket outage/recovery, different racer choices, Ready convergence,
selection backtracking, accessible actions and independent leave. It records
automation timings, proves wrong `/controller/` links recover without mutating
the room, keeps local recovery one action away, and requires Start to remain
absent. The run also guards two cross-client bugs: share APIs have a bounded
room-code fallback, and Retry must restore both authenticated state and its
subscription. This is an automated journey—not the outstanding uncoached human
usability approval.

`tests/check_party_capacity.py` runs two fresh real-Worker environments. The
first exactly consumes a small admission budget with one MatchRoom and one
Phone Party, sends a 64-request mixed creation flood, and requires every new
create/join to return the public `{error: "service_budget_safe"}` response with
`no-store`. The real Wrangler process then restarts on the same persistent
state; the refusal latch, MatchRoom and Phone Party must reconstruct without
reopening admission. A 64-request forged bearer flood across both control APIs
must reject without consuming one control unit or touching a room. Existing
authenticated state, mutation, MatchRoom close
and Phone Party close must still pass from the separate control reserve;
closing does not
refund the daily stop line. Static launcher, `/controller/`, and `/room/`
routes retain their CSP/security headers throughout. A real browser must render
the assertive **Online Rooms Are Full Right Now** recovery with accessible
**Choose ROM** and **Try Again** actions. Missing/wrong operations secrets must
deny the aggregate snapshot. The HTTP controls consume 10 units, including the
post-restart reconstruction read; the fixture
then opens both authenticated socket shapes and requires the protected
snapshot to expose exactly 20 admission units, 42 control units (including the
socket reservations), the refusal latch and no identity, capability or network
canary. Its companion health view exposes only fixed reservation buckets,
reconstructs those units exactly with complete tracking across restart, and
keeps forged/refused floods at zero observations. The second environment proves literal
`MAX_ADMISSIONS_PER_DAY=0` refuses the first admission and reports `closed`
while static/local paths remain available and its health aggregate remains
empty/complete.

`tests/check_online_wire_schema_parity.py` extracts the match wire vocabulary
from `services/party/src/match/protocol.ts`, `match-room.ts` and `signaling.ts`
-- the public state projection and its nested lobby/member/seat/control-step key
sets, the command types and request body, the lobby phases, the `MatchError`
and typed error tokens, the socket close code/reason pairs, both signaling
envelopes and the `/api/ops/health` reservation buckets -- and compares each one
against the key set `dist/web/online/*.js` actually reads or writes. It fails
when either side gains, loses or renames a member the other does not follow,
when a response envelope no longer matches a combination `validWireKeys()`
admits, when the browser issues a command the schema does not define, when a
4000-class close reason is not in `terminalLiveCloseCode`, or when the capacity
gate's exact `/api/ops/health` pin diverges from `BUDGET_OPERATIONS`. The
members the browser deliberately does not speak -- the six v2 lobby fields,
top-level `iceServers`, the `schemaVersion` 1 pin (ruling R36, still open) and
the thirteen commands no browser path issues -- are listed with their reason,
and a listed member that stops disagreeing fails the gate too, so the gap
cannot change size in either direction unrecorded. Fifteen positive controls run
on every invocation: each feeds a synthetic one-line drift (renamed lobby field,
added or removed key, moved `schemaVersion`, renamed close reason, extra budget
operation, renamed command) through the same comparison and requires the
matching failure, and a blanked source must fail closed rather than find zero
keys and pass. Source-only: no ROM, build, network or service.

`tests/check_party_internal_api.py` freezes the 17-call Worker→Durable Object
census and requires every call to use the internal v1 envelope. It also requires
PartyBudget, PartyRoom, PartyCodeDirectory and MatchRoom to reject an unknown
version before URL/body parsing or storage access. Service tests exercise all
four rejection boundaries; existing direct-object tests retain the legacy old-
Worker arm, while the full Worker tests exercise the versioned new-Worker arm.

`tests/check_party_edge_policy.py` validates the checked-in Free-plan zone
rate-limit payload: exactly one terminating `/api/` rule, IP/colo counting,
30 requests per 10 seconds, 10-second mitigation, no deployment identity or
secret material, and no match for launcher/controller/room/static routes. It
also freezes the browser mapping from a provider-generated HTML `429` to the
typed capacity recovery. Actual zone installation and Security Events evidence
remain human/provisioned work.

`tests/check_party_production_config.py` validates the production Worker
environment in `services/party/wrangler.jsonc`: an `env.production` carrying the
custom-domain route, the four Durable Object bindings, the v1/v2/v3 migrations,
`workers_dev`/observability off, and a route host that matches `PARTY_ORIGIN`.
It requires only `/api/*` to run the Worker so `/controller/`, `/party/` and
`/room/` stay static, and it fails closed while the tracked config still carries
the unresolvable `party.example.invalid` placeholder.

`tests/check_party_binary_surface.py` inspects a packaged desktop artifact (or,
with `--self-test`, a synthetic fixture) for the real WSS/WebRTC Phone Party
transport rather than the stub and for the compiled service origin, so a build
that dropped the transport or shipped an empty origin fails before release.

`tests/check_party_cmake_origin_gate.py` pins the configure-time
canonical-origin gate on `MDKR_PARTY_ORIGIN`: it replays the gate's own
regexes, extracted from `CMakeLists.txt`, over an accept/reject matrix
(path, query, fragment, userinfo, trailing slash, uppercase, malformed host
labels and ports refused; bare host and host:port accepted) and requires the
runtime twin `mdkr_party_canonical_https_origin` to stay wired in the native
host. Its self-test proves the matrix still rejects a weakened gate.

`tests/check_release_party_origin.py` requires every workflow that packages
*and* uploads a distributable to gate on `vars.MDKR_PARTY_ORIGIN` (or the
deliberate `MDKR_ALLOW_PARTYLESS_RELEASE=1` escape hatch) and to thread the
resolved origin into every CMake configure it runs; without it a release lane
can silently ship a launcher whose Phone Party surface is compiled out.
`--self-test` proves each assertion still rejects the lane it names.

`tests/check_release_local_only_surface.py` freezes the two release boundaries:
native desktop workflows explicitly opt into the online beta while keeping the
bare preview switch off and every cloud surface origin-gated, while the Pages
publisher removes every cloud route and runtime.
The adjacent `check_browser_local_only_release.py` gate then exercises that
stripped `publish-web` artifact in Chromium inside the Pages workflow; it is
workflow-owned because the general suite does not produce that release stage.

`tests/check_party_service_chaos.py` drives the real local Worker headlessly
through four release-depth scenarios: a restart with live phone leases that
rebinds each lease to its own seat and rotates the epoch while refusing
pre-restart generations; a concurrent abuse flood during a live session that
buys zero admission and moves no foreign seat; the literal-zero kill switch,
which refuses new pairing while established control coasts; and quota exhaustion
seen by a paired phone as a typed, `no-store`, identity-free refusal while its
seat coasts.

`tests/check_party_firewall_negative.py` simulates the blocked-socket condition
against the real Worker and the native host: a killed listener and a rebound
blackhole both fail neutral, the established room announces its bounded reconnect
and stops, a blocked room-open and a refused code entry land in the shipped
recovery copy verbatim, and local play stays reachable throughout.

`tests/check_binary_size_evidence.py` records the packaged release-lane artifact
sizes against generous checked-in ceilings in `tests/binary_size_baseline.json`,
skips artifacts a given machine did not build, and fails strictly when a named
lane exceeds its ceiling. Re-baseline deliberately with `--record`.

`tests/test_party_usage_reconciliation.py` adversarially validates the offline
operator gate that compares the exact privacy-safe capacity snapshot with a
normalized provider aggregate. It rejects unknown fields, arithmetic drift,
date skew, refusal latches, paid plan/billing/overages, nonzero charge,
unreviewed higher limits, 75% stop lines and absent provider activity. Its CLI
emits only bounded aggregate pass output or a stable stop code and proves a
secret-canary value cannot be reflected. This is contract evidence; a hosted
provider export and seven-day zero-currency ledger remain operational evidence.

`tests/test_party_beta_ledger.py` validates the machine contract for exactly
seven contiguous hosted beta days. Every day reuses the strict provider
reconciliation and independently verifies the fixed health-bucket weights,
complete legacy-free tracking, local-play availability, closed incident state,
source/deployment digests and daily `GO`. Schema injection, date/window skew,
charge, incomplete tracking, arithmetic drift, boolean confusion, oversized
input and secret reflection all stop. Its fixture pass proves only the validator;
it never counts as hosted seven-day evidence.

`tests/check_web_publish_stamp.py` stages the public shell exactly as the
publisher does and requires one identical build token on every root launcher,
Phone Party, controller and `/room/` JavaScript/CSS reference. It also removes
the room document in a negative fixture and requires publication to fail
closed, preventing a release from mixing protocol/client generations through
ordinary browser caches. Each document also receives one explicit build stamp
for service-worker cache admission.

`tests/check_browser_publish_skew.py` stages two immutable releases and drives
their real service-worker lifecycle in a clean Chromium profile. It proves the
old worker remains active while the new worker waits, both caches are isolated,
and an old worker that has fetched new HTML will not overwrite its offline
document. With the server forcibly unavailable during that window, navigation
recovers one complete old build. Closing the old document then activates the
new worker and deletes only the stale cache—no `skipWaiting`, mixed assets or
mid-session takeover.

The adjacent browser-party evidence is split by responsibility:
`check_controller_page.py` owns the engine-free phone surface and neutral
lifecycle, `check_party_host.py` owns launcher QR/code approval and seat custody,
and `check_phone_party_webrtc.py` owns authenticated signaling plus direct state
and control channels. The host gate also forces QR encoding failure and requires
the canvas to disappear while `/controller/` plus the accessible six-digit code
remain truthful. The service fixture connects two approved controller sockets
and proves a host offer reaches only its addressed attachment. That peer gate
requires a no-churn handshake, same-peer
ICE restart, bounded ping/pong, fail-neutral watchdog expiry, exact-neutral
reliable-channel failure, one fresh generation and working input after each
recovery. `check_party_experience_canary_smoke.py` runs the
operated journey against a fresh real local Worker: direct input continues while
phone signaling is forcibly unavailable, then the same seat rotates its epoch,
rebinds and accepts fresh input after signaling returns. The one-attempt smoke
must remain `STOP`; it cannot counterfeit hosted evidence.

`tests/check_party_native_e2e.py` is the only gate that runs the native half
for real: the packaged host model with its production libdatachannel transport
(the `mdkr_native_party_e2e_driver` binary, which the check owns — it is
deliberately not a ctest), a live `wrangler dev --local` Party Worker, and the
shipped `/controller/` page in headless Chromium. The transport speaks plain
`ws://` to the loopback Worker only under `MDKR_INTERNAL_TEST_TOKEN`; the
bringup gate separately proves loopback HTTP stays refused without it. Its
golden path requires the pending controller — which the real Worker reports
with `"seat": null` — to be visible to the driver before approval, the
driver's ECDH pairing phrase to match the page's rendered phrase, and fifty
non-neutral pad packets to cross the real ingress after Connected. Its second
scenario swallows the first `webrtc_offer` frame on the phone and requires the
signaling retry deadline to reach Connected inside sixty seconds; its third
SIGSTOPs the driver for three seconds mid-stream and requires a Connected
controller with fresh input within five seconds of resuming, which drives the
bounded transport queue and the host's custody self-heal through the true
stack.

`tests/check_online_live_transport_e2e.py` joins two production live-transport
drivers through the real service protocol and proves creator/joiner convergence,
bounded recovery, and fail-neutral teardown without relying on launcher UI or
an in-process transport stub.

`tests/check_party_lan_e2e.py` is the no-internet crown gate: it runs the same
`mdkr_native_party_e2e_driver` under `--lan`, where the embedded
`MdkrLanPartyServer` plus the in-process `MdkrLanPartyRoom` ARE the signaling
backend and no `wrangler` Worker exists anywhere in the run. A headless Chromium
loads `/controller/#<capability>` from that embedded server over plain `http`
and pairs with the pure-JS SAS fallback. Because a loopback origin is a secure
context that would expose `crypto.subtle`, the lane serves the page from a real
non-loopback private LAN IPv4 (falling back to `127.0.0.1` with a boot hook only
when the host has no LAN address) and asserts, in the live page, that
`isSecureContext === false` and `crypto.subtle === undefined` so the phrase can
only have come from `MDKRPartySas`'s pure-JS SHA-256 + P-256 twin. Its golden
path requires that page phrase to equal the native mbedtls phrase (the crown
cross-proof) and non-neutral packets to cross the real ingress; further
scenarios refuse a wrong six-digit code before pairing on the right one, trip the
room's shared code bucket with thirteen rapid wrong codes, and require the phone
to show `host_closed` when the host closes the room.

`tests/check_online_live_transport_e2e.py` is the live-transport capstone: it
races TWO
`mdkr_online_live_transport_e2e_driver` native processes (one creator, one
joiner-by-code) against a live local MatchRoom Worker (`wrangler dev --local`
with real Durable Objects) over the entire production online path -- real HTTP
create/join/code/command over the MatchRoom lobby routes and the authenticated
`/connect` state WebSocket, the match-signal client, the peer mesh
negotiating real WebRTC DataChannels (libdatachannel DTLS), the live
adapter (selections, the auto-confirmed transcript verification phrase,
preflight consensus, and descriptor install through the retail-identity
clamp), and the per-tick transport feed that seals local input into 3-frame
bundles, fans them out on the mesh, and drains authored ticks through
`mdkr_match_transport_receive`. Each process races at least 1800 confirmed
authored ticks; the golden path requires both verification phrases to match,
both descriptors installed, and the two end-of-race FNV state hashes to be
byte-identical. DEV lane, not release-required: the drivers speak plain
`ws://`/`http://` to the loopback Worker only under the shared
`MDKR_INTERNAL_TEST_TOKEN`, the same loopback-transport token the party e2e and
signal-client tests use. **Scope class:** component (real-transport
integration) -- it exercises the REAL production transport stack (a live local
MatchRoom Worker with real Durable Objects, real WebRTC DataChannels over DTLS,
and the match-signal/mesh/live-adapter/per-tick-feed client path) end-to-end
between two native driver processes with NO input or fault seams; but it drives
test-driver processes against a loopback Worker under the internal test token,
not the shipping app, so it is a DEV integration lane, not the shipping-app
acceptance route (that is `check_online_native_flow_cloud.py`).

`check_online_engine_boot_direct.py` (run-checks registered in
`tools/run_online_checks.py`) boots the VISIBLE engine into a
live loopback online race (`MDKR_APP_TEST_ONLINE_LIVE`, two real adapters over
the in-process hub, real libdatachannel DTLS) with NO menu-nav input script:
the direct-boot seam must reach the race purely from the installed manifest,
converge byte-for-byte with the peer and tear down cleanly. Its default
invocation is the historical Ancient Lake lane (vote track 5, Car, mask 0x7)
and stays byte-identical. `--track <id>` re-aims the same gate at any of the
20 standard race tracks: the loopback leader fixes the track with
SET_CONFIG_TRACK (env seam `MDKR_APP_TEST_ONLINE_TRACK` in
`platform/app/online_live_wiring.cpp`; a configured session skips the
track-vote step in the view exactly like retail, so the config alone drives
the manifest) and the wiring derives that track's raw ROM vehicle mask and
default vehicle. `--mask 0x<mm>` additionally pins the
exact START_RACE mask the wiring froze. The narrow-mask lane is
`--track 8 --mask 0x2`: Whale Bay is hovercraft-only with a non-Car default,
so the previously zero-coverage admission arm (manifest mask ==
`leveltable_vehicle_usable(track)` with mask != 0x07) plus per-seat vehicle
legality (descriptor validation and BEGIN_LOADING both check the chosen
vehicle bit against the mask) must hold end-to-end for the boot to happen at
all. **Scope class:** seam-injection (config/input seams on an in-process
loopback) -- the VISIBLE engine boots a live loopback race (real in-process
hub, real libdatachannel DTLS) driven purely by config seams
(`MDKR_APP_TEST_ONLINE_LIVE`, `--track`/`--mask`) with no menu-nav input; it is
NOT the shipping cloud route (that is `check_online_native_flow_cloud.py`).
Not CTest-registered -- like the other `MDKR_APP_TEST_ONLINE_LIVE` engine boots
it needs the local US 1.1 ROM -- so it can also run standalone with `--build
<dir> --rom <path>`.

`check_online_session_boot.py` (run-checks registered in
`tools/run_online_checks.py`) is
the gate for the SEPARATED online boot path. Where the direct
lane proves the race is reached with no menu-nav script, this proves it is now
reached THROUGH a separate `GAMEMODE_ONLINE_SESSION` that holds its own state
and never runs the offline `GAMEMODE_MENU` / `gCurrentMenuId` path. It stands up
the same in-process live loopback session (real libdatachannel DTLS, roster +
launch descriptor installed from the vote) and boots the engine with NO input
script, but also sets `MDKR_TEST_ONLINE_SESSION_SCRIPT=<hold>` -- an engine-side
seam (`game/src/online/online_session.c`, beta + env gated, inert otherwise)
that installs the REAL `party_link` forward feed and publishes a scripted
snapshot each `LOBBY_WAIT` tick: `MDKR_ONLINE_LOBBY` for `<hold>` ticks then
`MDKR_ONLINE_LOADING`. The gate asserts the session mode was entered
(`[online-session] begin`), that it genuinely idled in `LOBBY_WAIT` reading the
scripted snapshot (`>= hold` ticks with `haveSnap=1 snapPhase=1`), that it then
handed off to the race (`[online-session] phase=RACE`) with `gGameMode=2`
(`GAMEMODE_ONLINE_SESSION`) and `gCurrentMenuId=0` (the offline `MENU_BOOT` was
never loaded), and that the race still boots and converges byte-for-byte through
the same `ENGINE-ONLINE-LIVE` witness the direct lane checks. **Scope class:**
seam-injection (config + input seams on an in-process loopback) -- the same
in-process live loopback session as the direct lane, with the engine-side
`MDKR_TEST_ONLINE_SESSION_SCRIPT` seam publishing a scripted LOBBY_WAIT snapshot
to prove the separated `GAMEMODE_ONLINE_SESSION` boot; it is NOT the shipping
cloud route (that is `check_online_native_flow_cloud.py`). Default
`--build build-beta` (the beta engine carries the seam).

`check_online_charselect.py` (run-checks registered in
`tools/run_online_checks.py`) is the
gate for the native online CHARACTER SELECT screen. Where the
session-boot lane proves LOBBY_WAIT hands off to the race, this proves the first
player-facing SCREEN of the separated path. It stands up the same in-process live
loopback session (real libdatachannel DTLS, roster + launch descriptor from the
vote) but sets `MDKR_TEST_ONLINE_CHARSELECT=1`. That engine-side seam
(`game/src/online/online_charselect.c`, beta + env gated, inert otherwise) stands
in for the launcher: it installs the REAL `party_link` forward feed, publishes a
scripted 2-seat `LOBBY` room (local seat 0; a scripted remote `RIVAL` who has
already locked Bumper and readied), then each CHARSELECT tick acts as a minimal
reducer -- it polls the screen's published intent, converges the local seat, and
flips the lobby to `LOADING` once both seats are ready. The screen (which
re-implements the presentation with the game's OWN portraits + font instead of
calling the offline charselect `_loop`) walks its cursor to Pipsy, confirms and
readies. The gate asserts the screen was entered and its portrait assets loaded
(`[online-charselect] enter`), the remote seat rendered from the snapshot (a
render row with the remote's char + name), the published intent reached the target
character + the SAME default vehicle `menu_online_versus_race_setup` applies +
ready, the local seat converged and rendered, the phase advanced on the scripted
host-start (`[online-charselect] advance`), the assets were freed
(`[online-charselect] exit`), and the session handed off to the race WITHOUT the
offline menu (`gGameMode=2 gCurrentMenuId=0`), which still converges byte-for-byte
through the `ENGINE-ONLINE-LIVE` witness. **Scope class:** seam-injection
(config + input seams on an in-process loopback) -- the same live loopback
session with the engine-side `MDKR_TEST_ONLINE_CHARSELECT` seam standing in for
the launcher (scripted 2-seat LOBBY + minimal reducer) to prove the native
character-select screen; it is NOT the shipping cloud route (that is
`check_online_native_flow_cloud.py`). Default `--build build-beta`.

`check_online_lobby_start.py` (run-checks registered in
`tools/run_online_checks.py`) is the
KEYSTONE gate: it proves the NATIVE online screens own RACE 1. Where the
charselect/trackselect/session-boot lanes boot the engine race-READY (the
descriptor already built), this stands up the two loopback adapters STOPPED at
`SELECTING` (no descriptor, `party_link` installed) via
`OnlineRoom_makeTestLobbyStartRoom` and boots the visible (host) engine
DESCRIPTOR-LESS (`MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START=1`). The engine's native
CHARSELECT/TRACKSELECT scripted input (`MDKR_TEST_ONLINE_LOBBY_START=1` -- input
only; it does NOT install the self-contained charselect/trackselect feed seams)
drives the host's selection/ready/`SET_CONFIG_TRACK`/START through the REAL reverse
feed into the adapter, building the descriptor live at `BEGIN_LOADING`. The gate
asserts: the session BEGAN descriptor-less (`[online-session] begin: lobby-start`)
and the descriptor-FIRST begin did NOT fire; CHARSELECT then TRACKSELECT fronted;
the race-1 readiness gate DEFERRED the boot (`race-1 boot deferred ... -> LOBBY_WAIT
re-wait`) until the launcher built + armed the descriptor
(`[online-lobby-start] race-1 armed`); then race 1 booted EXACTLY ONCE and ONLY
THEN (`phase=RACE ... [race=1]`) on the host-selected track 5 (`[online-boot]
direct race: track=5`, honored, no divergence, no admission reject), with
`gGameMode=2 gCurrentMenuId=0` throughout and the engine entering the online
rollback race (`loadedTrack=5`). This is the descriptor-less-begin + race-1
readiness-gate proof (never a NULL/stale descriptor deref). **Scope class:**
seam-injection (input seams on an in-process loopback) -- two loopback adapters
stopped at SELECTING (descriptor-less) with the input-only
`MDKR_TEST_ONLINE_LOBBY_START` native-screen seam driving
selection/ready/START through the real reverse feed to build the descriptor
live; it is NOT the shipping cloud route (that is
`check_online_native_flow_cloud.py`). Default
`--build build-beta`.

`check_online_lobby_unconfigured.py` (run-checks registered in
`tools/run_online_checks.py`) is the PRODUCTION
ROOM SHAPE regression gate for the two-peer cloud CHARSELECT wedge. The
lobby-start lane's room carries a track-5 pre-config whose only purpose is to
"unlock READY at SELECTING"; the REAL production pairing creates a FRESH
single-race room with NO configured track, and on that shape the SELECTING view
gates READY behind a track VOTE the native screens never cast -- so the first
real two-process cloud run wedged at CHARSELECT forever (seat character
converged, `seat.ready` never latched, stale-retry churn). This lane re-runs the
same descriptor-less native flow with `MDKR_APP_TEST_ONLINE_UNCONFIGURED=1` (the
room builder SKIPS the pre-config, witnessed) and
`MDKR_APP_TEST_ONLINE_ROOM_LATENCY=3` (the loopback room channel gains 3 pumps
of latency per leg with the State broadcast one pump SLOWER than a
CommandResult -- the real transport's ordering, which starves a blind stale
retry of the fresh revision). Asserts the full CHARSELECT -> TRACKSELECT ->
VEHICLESELECT -> deferred/armed/booted chain with the native TRACKSELECT's
`SET_CONFIG_TRACK=3` as the room's FIRST-EVER config and race 1 booting on
track 3. The unit-level twin is `mdkr_online_live_adapter_test --latency`
(tests/test_online_live_adapter.cpp, `online_live_adapter` ctest): two REAL
live adapters + per-endpoint replicas of the reverse-feed pump over a
latency/ordering room double, asserting bounded both-ready convergence, ready
STAYING latched, no redundant accepted-SET churn, and the host lock + START
reaching LOADING. **Scope class:** seam-injection (config + fault + input
injection) -- it reproduces the PRODUCTION room SHAPE (no pre-config) on an
in-process loopback via the `MDKR_APP_TEST_ONLINE_UNCONFIGURED` config seam and the
`MDKR_APP_TEST_ONLINE_ROOM_LATENCY` fault seam, driven by scripted native-screen
input; it is NOT the shipping two-process cloud route (that is
`check_online_native_flow_cloud.py`). Default `--build build-beta`.

`tools/online/check_online_native_flow_cloud.py` (MANUAL network lane -- NOT in
`run_online_checks.py`'s default sweep; needs live Wi-Fi + the deployed
party.goldenballoon.net service; run standalone, never two cloud runs concurrent)
is the two-process real-cloud PRODUCTION-PATH acceptance instrument. Where `check_online_lobby_
start.py` proves the descriptor-less native path on an IN-PROCESS loopback with a
pre-configured room, this launches TWO SEPARATE `mdkr64` app processes over the
REAL cloud and exercises the full flow with the ONLY automation being (a) pairing
bootstrap -- `MDKR_APP_TEST_ONLINE_AUTOPAIR=create|join` (+ `_JOIN_CODE`) drives the
interactive launcher's create/join chooser, join code, "Check Setup" secure
handshake and Words-Match confirm (`ui_online_room.cpp` `autopairService`), then
goes HANDS-OFF -- and (b) injected PAD INPUT: the input-only native-screen seams
(`MDKR_TEST_ONLINE_LOBBY_START` for CHARSELECT/VEHICLE/TRACKSELECT, per-role
`MDKR_TEST_ONLINE_CHARSELECT_PICK` so the two seats never collide,
`MDKR_TEST_ONLINE_RESULTS_CHOOSER` for the more-races chooser) plus the
deterministic race-input fixture (`MDKR_APP_TEST_ONLINE_SYNTH_RACE_INPUT`) standing
in for the two absent human controllers. Everything after pairing is the PRODUCTION
code path choosing its own route. Runs with VISIBLE windows (the interactive
launcher only builds frames with a real drawable). The seven assertions: (a) the
`[online-room-ready]` latch/publish/boot-enter fire on BOTH -- takeover self-fired;
(b) the native session enters CHARSELECT descriptor-less (no
`source=launch-descriptor`); (c) selections sync through the reducer; (d) race 1
converges; (e) the chooser round-trips a converged race 2; (f) both end FINISHED --
the host's FINISH first dispatches the EXISTING leader-only REMATCH wrap
(`lobby_core.c`: RESULTS -> LOBBY + a fresh series), so the transition is
reducer-observable and the real joiner's chooser mirror exits to its OWN champion
ceremony from its latched final standings (no new reducer command, no Worker
change); (g) the takeover re-fires on BOTH endpoints AND a second descriptor-less
native session reaches CHARSELECT -- the automatic FINISHED re-take into the
freshly wrapped fresh-series tournament room (the panel's re-arm observer completes
immediately: the wrap's RESULTS-out-and-back was the rising edge). `--through`: `c`
stops after the CHARSELECT -> TRACKSELECT advance; `e` stops after (a)-(e) (a
faster intermediate stop -- needs `--tournament CUP`, since a single race never
auto-finals); `full` (all seven assertions) is the GREEN bar. Convergence on this
descriptor-less path is DUAL-witnessed per race: the reducer-agreed finish order
(both processes commit the SAME placements per race_index) AND the cross-process
STATE-HASH fold -- each endpoint's resident coordinator folds the same
firstTick-anchored window of CONFIRMED canonical input frames into an FNV state
hash (`[online-resident-live] race fold ...`, log-only, session-gated) and the
capstone requires the identical span + identical hash on both processes, so
identical finish orders can no longer mask divergent simulations. Default
`--build build-beta`. The tournament-final joiner strand
this lane originally caught (the host's FINISH was a purely local leave that sent
NO reducer command, so neither of the joiner mirror's exits -- room-left-RESULTS /
vanished-host -- could ever fire) is fixed by the FINISH wrap above; the local
red-first pins live in `check_online_lobby_tournament.py` (the host's wrap must
land before the ceremony) and `check_online_results_chooser.py` (the final mirror
exits to its own ceremony, never re-selection). The SAME strand class existed for
a SINGLE-RACE room's FINISH (a mandatory pre-beta extension): the host
now commits the same REMATCH wrap there (phase-only in single-race), leaves via
the race-winner ceremony into FINISHED, and the joiner's mirror KEEPS THE FOLLOW
on the observed wrap (FINISH and RACE AGAIN are reducer-indistinguishable in
single-race; the follow and the host's automatic FINISHED re-take re-converge both
peers in CHARSELECT of the wrapped room). Its local red-first pins live in
`check_online_single_race_replay.py` (the `finish` scenario: wrap committed +
converged on the REAL loopback reducer, reason=FINISHED never LEFT) and
`check_online_results_chooser.py` (`single-finish` / `single-joiner`).
**Scope class:** production-path -- the real two-process cloud route exercised end-to-end, the ONLY automation being the pairing bootstrap and injected pad input standing in for the two absent human controllers; everything after pairing is the production code path choosing its own route. This is the one production-path acceptance lane the other online lanes' seam-injection scenarios stand in for.

`check_online_lobby_tournament.py` (run-checks registered in
`tools/run_online_checks.py`) is the KEYSTONE gate: it COMPOSES the lobby-start boot with the
resident multi-race coordinator so a descriptor-less session runs a FULL
tournament IN ONE engine process. Same descriptor-less begin as the lobby-start
lane, but `MDKR_APP_TEST_ONLINE_MODE=tournament` + `MDKR_APP_TEST_ONLINE_CUP=1`
(Snowflake cup: rounds 13/6/9/28, all Car-legal) so the room is pre-configured a
tournament (READY-unlock; the native TRACKSELECT enters in tournament mode via
`MDKR_TEST_ONLINE_LOBBY_TOURNAMENT=1` and rides the cup), and
`MDKR_TEST_ONLINE_RESULTS_HOST_PRESS=1` drives the host's per-round advance. After
race 1 the lobby-start coordinator HANDS OFF to the resident coordinator
(`composed: handed off to resident coordinator`), so races 2..4 re-cycle in-process
via the SAME mid-residency PUBLISH_RESULTS + frame-stepped `OnlineRoom_residentAdvanceStep`.
Finality is FEED-derived (`MDKR_ONLINE_SESSION_CUP_ROUNDS`): the lane
asserts EXACTLY 4 `[online-boot] direct race:` boots on the cup-1 schedule
[13,6,9,28] in one process, native RESULTS each round (`haveResults=1`), feed-isFinal
0 for races 1-3 and 1 at race 4 ONLY, points accruing by trophy weight to 34,30,
`race_index` 0->3 via the reverse-feed REMATCH on fresh epochs [2,3,4], and
`gGameMode=2 gCurrentMenuId=0` throughout with no admission reject / watchdog trip.
It also runs two WEDGE sub-tests proving the deferred safety findings fire cleanly
(never hang): (W1) `MDKR_APP_TEST_ONLINE_LOBBY_WEDGE=descriptor` -- the descriptor/
match-input never arms, so the engine's WALL-CLOCK WATCHDOG
(`descless wait TIMEOUT`) fires + exits clean; (W2) `...=cancel` -- the leader
CANCEL_LOADINGs while a boot is pending, so the session UNWINDS (`lobby-start
UNWIND`) + re-fronts CHARSELECT + recovers (race 1 still boots). A source-scan pins
`MDKR_ONLINE_SESSION_CUP_ROUNDS == MDKR_ONLINE_CUP_ROUNDS`. **Scope
class:** seam-injection (input + fault injection) -- the full descriptor-less
tournament runs on an in-process loopback driven by scripted native-screen input +
host-press seams (with the wedge/watchdog fault seams for the safety sub-tests); it
is NOT the shipping cloud route (that is `check_online_native_flow_cloud.py`). It
reaches its final-standings FINISH by the host committing the native chooser's
FINISH option (`[online-results] chooser: committed option=FINISH -> LEAVE`).
Default `--build build-beta`.

`check_online_final_replay.py` (run-checks registered in
`tools/run_online_checks.py`) is the RULED two-real-peer FINAL-REPLAY proof: at
the tournament FINAL standings the host commits NEW TOURNAMENT instead of FINISH
(`MDKR_TEST_ONLINE_RESULTS_CHOOSER=3,5`), which dispatches the SAME leader-only
REMATCH wrap (fresh series, race_index 3 -> 0) -- the host then CONTINUES
in-session (results -> TRACKSELECT, re-locks the cup, STARTs) while a real second
peer's road is the ceremony -> FINISHED -> automatic re-take into the freshly
wrapped room. The ruling holds because both roads re-converge through REDUCER
AUTHORITY, and this lane proves the re-convergence on the loopback 2-endpoint
rig: `MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT=1` drives the peer adapter as the
stand-in remote process (re-readying itself in the wrapped room exactly as the
re-taken joiner's fresh session does), the launcher's coordinator takes the
OBSERVE-ONLY re-cycle on the observed final wrap (`tournament final wrap: room
left RESULTS -> LOBBY (fresh series...)` -- never the auto-driving mid-cup
advance, which would race the host's own re-selection), the match-input re-arms
on the FRESH epoch 5, and race 5 boots as the fresh series' round 1
(race_index=0) -- then the full second cup races ([13,6,9,28] x 2, results
race_index 0..3 twice, 8/8 cross-endpoint state-hash folds converged=1) and its
FINISH ends cleanly (wrap -> CEREMONY -> single FINISHED). RED at the pre-fix
build: the coordinator's tournament re-cycle trigger was `race_index >
rs->raceIndex`, which the final wrap (3 -> 0) never satisfies -- the coordinator
parked in Results forever and the continuing host's race 5 never booted. **Scope
class:** seam-injection (input seams on an in-process loopback); the shipping
cloud route's FINISH leg is `check_online_native_flow_cloud.py`. Default
`--build build-beta`.

`check_online_session_end.py` (run-checks registered in
`tools/run_online_checks.py`) is the
gate for the engine->launcher FINISH/RETURN handshake (the session
end-reason channel). After a native online session ends the engine notes WHY on
the party_link channel (`mdkr_party_link_note_session_end`) + requests the platform
exit; the launcher's `runOnlineLobbyStart{Live,Engine}Session` TAKES that reason
(`mdkr_party_link_take_session_end`) right after the boot returns and BEFORE
`OnlineRoom_clearPartyLink()`, logging `[online-session-end] reason=...`. The lane
proves each reason end-to-end on the loopback lobby-start rig: FINISHED as the
HOST ("A: FINISH" on the final standings -> exit 0), FINISHED as a JOINER
(the non-host FOLLOWS the host out of the final standings once the
snapshot phase leaves RESULTS -- otherwise it parked until window-close; proven via
`MDKR_TEST_ONLINE_RESULTS_JOINER_FINISH`, terminal-only so the visible endpoint
still drives rounds 1..N-1 as host), LEFT via a genuine CHARSELECT
browse-B backout (`MDKR_TEST_ONLINE_CHARSELECT_BACKOUT`; the scripted lanes'
tick-3 browse-B still STAYs), LEFT via a pre-START remote-vacated seat
(`MDKR_TEST_ONLINE_REMOTE_VACATE`; debounced), LEFT via a mid-tournament
leader cancel (a clean return replacing the earlier re-front-into-error),
and ERROR via the wall-clock watchdog (nonzero exit). **Scope class:**
seam-injection (input + fault injection) -- each session-end reason is stood up on
the in-process loopback lobby-start rig via input/fault seams (the host FINISH via
the native chooser, a charselect backout, a remote-vacate, a mid-tournament cancel,
and a wall-clock wedge); it is NOT the shipping cloud route (that is
`check_online_native_flow_cloud.py`). Default `--build build-beta`.

`check_online_tournament.py` (run-checks registered in
`tools/run_online_checks.py`)
drives a FULL 4-race Dino Domain cup (mode
tournament, cup 0: tracks 5, 3, 29, 7) through ONE loopback room
(`MDKR_APP_TEST_ONLINE_MODE=tournament` + `MDKR_APP_TEST_ONLINE_CUP=0`). The
loopback branch boots the visible engine exactly once per process, so the
proof splits honestly and the PASS line says which is which: race 1 is
ENGINE-proven -- the visible engine boots cup round 1 (track 5) off the live
transport, converges (`[ENGINE-ONLINE-LIVE] converged=1`), races to the
finish, ends its session ~2.5 s later (`[online-postrace]`), records real
placements (`[online-results]`) and the launcher reports them accepted=1 --
while races 2-4 are TRANSPORT-proven through the same live adapters (the
`test_online_live_adapter.cpp` lifecycle-rig approach, run by the wiring's
tournament continuation): the leader's REMATCH advances race_index, each
round's frozen manifest must carry the cup schedule's track and raw ROM mask
on a fresh epoch with byte-identical descriptors on both endpoints, at least
30 authored ticks seal/drain hash-equal across the mesh, and the leader
publishes fixed 0/1 placements through the launcher's exact report seam.
Round 4 (Hot Top Volcano, mask 0x6) is a narrow-mask, no-Car round inside the
cup. Final standings must equal the authentic 9/7/5/3/1 trophy accrual --
36/28 when the visible endpoint also wins race 1, and otherwise exactly what
the real race-1 placements plus three 9/7 rounds produce. Expect roughly 2-4
minutes wall time (the engine race runs near real time to the finish; the
default `--ticks 9000` is only a stall bound because the engine exits itself
at the postrace witness). Both of these engine gates are timing-sensitive:
run them one at a time, never concurrently with builds. Neither is
CTest-registered -- like the other `MDKR_APP_TEST_ONLINE_LIVE` engine boots
they need the local US 1.1 ROM -- so they can also run standalone with
`--build <dir> --rom <path>`. **Scope class:** seam-injection (config + input
seams on an in-process loopback) -- a full 4-race cup on one loopback room via
config seams (`MDKR_APP_TEST_ONLINE_MODE=tournament`,
`MDKR_APP_TEST_ONLINE_CUP`), race 1 engine-proven on the visible engine and
races 2-4 transport-proven through the live adapters; it is NOT the shipping
cloud route (that is `check_online_native_flow_cloud.py`).

`check_online_room_ready_rearm.py` (run-checks registered in
`tools/run_online_checks.py`; not CTest-registered) is the gate for the safe
2nd-tournament room-ready
RE-ARM state machine. The native takeover latch
(`OnlineRoom_pollRoomReadyTransition`) is one-shot per adapter, so before this
fix a SECOND tournament in the SAME session fell back to the per-race ImGui path
instead of the native takeover; the re-arm is reason-aware and FINISHED-gated (a
FINISHED native return ARMS `OnlineRoom_armRoomReadyRearm`; LEFT/ERROR/NONE do
NOT; the panel's per-frame `OnlineRoom_observeRoomReadyRearm` completes a pending
arm IMMEDIATELY -- the tournament-final FINISH wrap already took the room out of
the takeover window at RESULTS and back to a fresh-series SELECTING during the
session, which IS the rising edge -- so the next poll re-takes native exactly once
per FINISHED return; a room transition alone never re-fires). The full
interactive two-tournament loop needs a live cloud adapter + a human, so this
lane drives the loopback tournament room through the wiring edges DIRECTLY via the
`MDKR_APP_TEST_ONLINE_ROOM_READY_REARM_PROBE` seam and asserts every sub-flag of
the probe verdict: `totalFires=3` (exactly one native takeover per tournament +
the reset coda's fresh adapter -- no re-boot loop), `t1Once` (tournament #1 fired
once), `leftNoRearm` (a LEFT/ERROR return never re-fires even with the condition
still TRUE), `wrapAloneNoRefire` (the RESULTS-park -> FINISH-wrap round trip alone
re-fires nothing), `finishedRetakeOnce`/`routed2` (the FINISHED arm completes and
re-takes native exactly once, route=lobby-start), `noRetakeWithoutFinished` (a
later condition cycle without a FINISHED return never re-fires), and
`resetDropsPending` (a fresh adapter drops a stale pending arm). Like
the sibling engine lanes it needs the local US 1.1 ROM; it can also run
standalone with `--build <dir> --rom <path>`. **Scope class:** component
(direct state-machine probe) -- it drives the loopback tournament room's re-arm
edges DIRECTLY via the `MDKR_APP_TEST_ONLINE_ROOM_READY_REARM_PROBE` seam and
asserts the probe verdict's sub-flags, rather than running a full session flow;
it is NOT the shipping cloud route (that is
`check_online_native_flow_cloud.py`).

`check_online_peer_loss.py` (standalone lane, not run-checks / CTest
registered) is the process-level peer-loss gate. It reuses the live-transport
`mdkr_online_live_transport_e2e_driver` but drives it against small in-process
FAKE servers (no wrangler, no ROM), so it is fast and deterministic. Two arms:
a survivor whose opponent's `/connect` transport is SEVERED mid-session, and one
whose opponent NEVER joins a valid one-seat room -- each must reach the room but
install NO race descriptor (`[E2E] installed` never prints), report NO converged
finish (`[E2E] result=ok` never prints), and tear down within its budget without
wedging. That is the transport-layer shadow of the engine's OPPONENT_LEFT /
OPPONENT_NEVER_STARTED routing; the engine reason-mapping (peer-loss -> failure,
the no-demotion rule) and the recovery-card copy are pinned separately at the
unit level by the `online_live_adapter_beta` and `online_lobby_view_model`
CTests. It does NOT boot the visible engine, so the literal engine witnesses
(`[online-live] peer lost mid-race`, `[START] ... aborting to the room`) and the
`race_abort` control message are out of its reach -- those need the
wrangler-backed two-process `MDKR_APP_TEST_ONLINE_LIVE_CLOUD` path. Run it on a
quiet machine, standalone, with `--build build-beta`. **Scope class:** component
(transport-layer, fake servers) -- it reuses the live-transport driver against
small in-process FAKE signaling servers (no wrangler, no ROM) to prove
peer-loss teardown at the transport layer; it does NOT boot the visible engine,
and it is NOT the shipping cloud route (that is
`check_online_native_flow_cloud.py`).

`match_peer_liveness` (A7) is the pure soft-fail/hard-fail policy the live
status line runs during racing, isolated from the WebRTC transport so a
scripted miss/hit sequence is exercised as plain arithmetic. Pre-A7 the
status line only ever said "Direct Connection" until the mesh's own
control-ping ladder declared the peer lost outright, so a network wobble that
recovered on its own carried no warning at all, and one that did not read as
a sudden, unexplained loss. The gate drives a previously-good peer through a
first missed probe (stays Transient, a soft-fail rather than an announced
loss), a second miss (still Transient), recovery mid-streak, and the Nth
consecutive miss (Unreachable, saturating rather than overflowing on further
misses). N is derived from the mesh's own ladder
(`kMdkrMatchControlPingTimeoutMs / kMdkrMatchControlPingIntervalMs` = 3,
pinned by a `static_assert` in `match_live_adapter.cpp`), so the presenter's
escalation lands at the same wall-clock boundary `peerLost(PingTimeout)` does
and never earlier -- that loss detection itself is unchanged. Positive
control: a small reference implementation of the pre-A7 immediate-escalation
shape (any miss -> Unreachable at once) is applied to the exact same
first-miss observation and lands Unreachable where the fixed policy's
assertion requires Transient, so the test provably discriminates the two
shapes rather than passing regardless. The retained "last known measurement"
the brief calls for needs no bookkeeping of its own here: the transport's
peer.rttMs is untouched by a miss (only an answered pong updates it), so it
is already the last measurement for free -- the tracker only needs to know
THAT a peer was previously good, not what its RTT was.

`match_peer_transport`'s `authorityLaneKeepsItsOwnReplayWindow` and
`authorityWrongPayloadTypeIsTypedLoss` pin the third channel at the transport
boundary: a repair answer sealed before seventy state envelopes and delivered
after them is opened rather than retired as a replay of them, an envelope
sealed for another lane and replayed onto the authority channel is a counted
drop and never a delivered repair, and an envelope authenticated under the
peer's own authority key carrying a payload type that channel does not serve
is the typed `ControlChannelViolation`. Positive control: pointing the
authority lane's replay window at the state lane's makes the delayed repair
disappear and only that arm goes red. Two more arms cover the protocol-version
edge: an offerer that carries only the two older channels comes up, opens them
and never reaches ready, so at the answerer's setup deadline it must resolve as
the named `ChannelSetMismatch` (recorded in the failure ring by that name, with
the missing label logged) rather than as the ICE-never-completed
`ConnectTimeout`; the same offerer carrying the full set reaches
`PeerChannelsReady` and is never lost.

The pure policy alone does not prove `LiveAdapter` actually calls it, so two
more gates cover the wiring. `match_peer_transport`'s
`controlPingMissCountsBeforeTimeout` drives a silent peer through a real fake-
clock ping ladder and asserts `linkStats()`'s `consecutivePingMisses` reads 1
after exactly one missed interval, with the peer still `channelsReady` and
no `PeerLost` yet, then that the mesh's own hard verdict still fires
correctly past the full timeout. `online_live_adapter`'s
`test_soft_fail_liveness_precedes_hard_fail_status` runs a real two-endpoint
race over the loopback mesh, silences one peer, and requires the RACING
status line to soften to "Connection hiccup — retrying" on the first missed
interval (never "Connection lost" ahead of it, never ending the race), before
the mesh's real `PingTimeout` eventually ends it for real. Positive control:
commenting out `updateLiveness()`'s call site in `pumpMesh()` (verified by
hand) leaves every other CTest green and only this arm goes red, since it is
the only one that ever reads the status string mid-race before a loss.
`online_lobby_view_model` pins the player-facing copy at its single source of
truth, `lobby_view_model.c`'s RACING status switch -- "Connection hiccup —
retrying" (`MDKR_CONNECTIVITY_DEGRADED`) and "Connection lost"
(`MDKR_CONNECTIVITY_LOST`), alongside the pre-existing "Direct Connection"
for a healthy peer -- so the pure module itself carries no player-copy
lookup of its own to drift out of sync with it.

`check_online_lobby_drop.py` (registered) pins the LOBBY-AUTHORITATIVE drop:
the room knows a member left the moment its socket closes and broadcasts
presence=false, so the survivor no longer waits out a transport ladder to
learn it. Same in-process kill signature as the transport-loss lane below
(`MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_TICK`), opposite regime. Arm 1 (the
shipped default) asserts the departed endpoint's seats are finalised at an
agreed tick ahead of the authored head (`[MESH] room departure ... finalised
at tick=T result=0`), the loss is typed as the room's own `PeerDeparted`
(parsed from the header, never hard-coded), the attribution is still
OPPONENT_LEFT, the mid-race latch reaches the card within FOUR authored ticks
of the presence drop (two for the peer-silence grace below, two to card), and
the clean LEFT return holds with no watchdog and no leaks. Arm 2 is the
positive control: the identical run with `MDKR_ONLINE_LOBBY_DROP=0` must NOT
resolve inside those four ticks, must finalise no seat, and must fall back to
`PingTimeout` inside the named ping bound -- so arm 1's number is attributable
to the room's verdict rather than to a fast machine.

Arms B and C pin the PEER-SILENCE GRACE the verdict now waits out. Arm B is
the room-service wobble: `MDKR_APP_TEST_ONLINE_ROOM_DEPARTURE_AT_TICK` drops
only the peer's loopback presence and leaves the peer pumping, sealing and
ponging, so the room says a member left while that member is plainly still
racing. The survivor must hold the verdict (`[MESH] room departure ... HELD`,
on a nonzero count of authenticated packets), finalise no seat, and carry on
until the peer is really severed 90 ticks later -- at which point the ping
ladder ends it, as it does whenever the room says nothing. Arm C is arm B's
positive control: the identical wobble with `MDKR_ONLINE_LOBBY_DROP_GRACE=0`
is the pre-grace code, and it finalises the seat and cards inside those four
ticks -- the false "opponent left" the grace removes.

The determinism half is adjudicated exactly, and
separately, by `test_match_transport.c`: a finalised transport and a reference
transport whose departed peer simply sends neutral input from the same tick
commit byte-identical canonical frames for 30 ticks, with a run that never
finalises diverging at the first tick past T as the in-test positive control.
The canonical frame stream is the simulation's only input, so identical frames
under an identical seed are an identical run. The mesh half --
edge-triggered departure, silent retirement, the proposal reaching every
survivor but the departed one, and a drop naming a non-roster endpoint as a
control-channel violation -- is pinned by `test_match_peer_transport.cpp`; the
decision's gates, the lowest-surviving-id proposer rule and the grace's own
decision table by `test_online_live_adapter_beta.cpp`; and what counts as an
authenticated packet -- an envelope that opened under the peer's own lane key,
never garbage and never the control channel's plaintext JSON -- again by
`test_match_peer_transport.cpp`. **Scope class:** engine flow (loopback
transport).

`check_online_midrace_transport_loss.py` (registered) pins prompt, truthful
mid-race peer loss ON THE TRANSPORT ITSELF -- the real-cloud kill signature: a
SIGKILLed opponent's signal socket dies (the service broadcasts
presence=false) while its mesh goes silent with channels nominally up. The
lane's sever seam (`MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_TICK`) reproduces that
in-process on the descriptor-less lobby-start tournament session: it freezes
the in-process peer's pump and drops its loopback signal presence, refusing
NOTHING -- detection must come from the transport's own liveness ladders. The
room-authoritative drop is switched OFF here (`MDKR_ONLINE_LOBBY_DROP=0`):
these ladders are what remains when the room says nothing at all (arm 3's
lingering presence, a service slow to drop a member, the Worker itself down),
and leaving the fast path on would resolve the same presence drop first. The
severed race runs at the authored 30 Hz (the seam arms `paceAdvanceHz`),
because detection is wall-clock (ping interval + stale bound =
`kMdkrMatchMidRaceLossPingBoundMs`, parsed from the header, never a magic
number) while the unthrottled headless drain finishes a race in ~2 real
seconds. Asserts: typed `[MESH] peer LOST` reason=PingTimeout within the named
bound of the sever (tick-denominated), the truthful OPPONENT_LEFT mapping (the
no-demotion rule), the EXISTING mid-race latch ending the race (no ghost
race), the crash-fix clean LEFT return (exit 0, zero leaks, no abort), and
that the watchdog path (`round advance TIMEOUT` / `descless wait TIMEOUT`)
never fires. The laggy-but-alive false-positive guard is pinned at the mesh
layer by `test_match_peer_transport.cpp`'s
`presenceBlipWithHealthyChannelsIsNotPeerLoss`. **Scope class:** engine flow
(loopback transport) -- the real-transport confirmation is
`check_online_native_flow_cloud.py --drop mid-race --drop-method kill`.

`check_online_pause_overlay.py` (registered) pins the two-machine PAUSE crash
fix: pressing START in a live ONLINE race must never stop -- or crash -- the
networked sim. Pre-fix, mode_game's retail pause branch consumed the CANONICAL
input of every seat, so any START edge (local or remote, live or replayed by a
rollback correction) engaged gIsPaused inside the online sim, and a paused sim
made `mdkr_game_resimulate_tick` refuse every later correction replay
(`game-tick completion rejected ... paused=1`) which
`reconcile_network_inputs` escalated to abort() -- the non-pausing beta
machine died with a crash dialog at the pause tick. Four arms on the
in-process two-adapter loopback race, using the synthetic-pad button script
(`MDKR_APP_TEST_ONLINE_SYNTH_PAD_SLOT`/`_SCRIPT`) to drive REAL canonical
START/D-pad/A edges and `MDKR_APP_TEST_ONLINE_LIVE_PREDICT` to sustain the
WAN-shaped correction storm: [storm] a remote START inside the storm (the
exact beta crash shape) reconciles and the race converges to the budget;
[live] an on-time remote START fires the deterministic suppression witness
and opens NO local overlay; [leave] a local START opens the non-blocking
CONTINUE/LEAVE overlay, resumes, reopens, and LEAVE RACE takes the clean
note-LEFT return (exit 0, zero leaks) while corrections keep reconciling with
the overlay open (the sim provably never stops); [belt] with the legacy
engage re-allowed (`MDKR_APP_TEST_ONLINE_ALLOW_RETAIL_PAUSE`, test-only), the
exact pre-fix abort trigger fires and is routed as `online correction replay
refused (sim-state; recoverable)` -> clean LEFT -- never abort. **Scope
class:** engine flow (loopback transport).

`tests/check_lan_controller_assets.py` keeps the local-play controller asset set
identical across the three places that must never disagree: the C++
`kControllerAssets` manifest in `lan_party_launch.cpp` that the embedded server
serves, the `tools/lan_controller_assets.txt` list the packaging scripts stage
from, and the tracked `dist/web` tree those files are copied out of, so a
packaged build cannot ship a controller page that loads broken.

The controller-page gate additionally covers exact 43-character fragment
scrubbing, embedded private share/copy recovery, capability-free public-page
share/copy plus code guidance, acknowledged duplicate-tab private navigation,
the no-Web-Locks hashed broadcast/storage lease and pagehide cleanup, actual
reload for protocol mismatch, BFCache clean reload and fail-closed History API
denial. The host gate also authors pagehide secret/pad cleanup plus persisted
room-authority recovery. The service
security gate separately covers canonical HTTPS and loopback-only origin
configuration. These latest arms are
authored and syntax-valid but are deliberately unexecuted on the occupied
workstation; they require the dedicated browser desktop.
`check_persistent_browser_session.py` separately proves
two complete engine loans return through one wasm module without surrendering
launcher or mounted-storage ownership.

The controller gate also drives the shared analog surface through Arrow/WASD,
requires full cardinal range, radius-clamped diagonals, synchronized spoken
direction and exact neutral on release. `touch_surface.test.cjs` separately
proves blur and page lifecycle neutralization without a browser timing race.

The controller/host browser gates also freeze destructive-action UX: Leave and
End open labelled confirmations with the non-destructive choice focused, cancel
mutates neither lease nor room, and confirmation produces exact neutral state
before truthful **Controller disconnected** recovery. The shared document
contract requires inline code errors, stable dynamic invite-form metadata,
semantic auxiliary headings, contained modal overscroll, visible focus, and a
safe-area-aware skip target on the private-room handoff. Static document/source
contracts additionally require bounded 16 KiB/ten-second HTTP reads, exact
host/controller publications, same-transition equivocation rejection, stale
socket-generation guards, five-attempt reconnect and the phone's native **Try
now** recovery action.

The Party service also contains a separate launcher MatchRoom authority in
`services/party/src/match/`. Its pure reducer and real local SQLite Durable
Object tests cover link/code create and join, the complete two-endpoint
selection/vote/load/race/results/rematch path, authenticated actor injection,
every-phase reconstruction, hibernated WebSocket recovery, exact and conflicting
replay, simultaneous revision conflict, a shared native/service fingerprint
vector, leader-only generation-checked invite rotation, expiry deletion,
canonical non-Join commands, a literal-zero admission
kill switch and durable-storage absence of raw invite/code/credential values.
Client state also omits internal receipts, command high-water marks and
fingerprints. Run all service gates
with `(cd services/party && npm run check)`; `npm run typecheck` validates the
non-executing type boundary alone. These tests do not switch on production
online admission. Worker integration also races two Match commands from one
revision and two Phone Party approvals for one seat. Complete transitions are
input-gated across storage awaits: the Match race has exactly one success and
one `stale_revision`, while the seat race has exactly one lease and leaves the
other phone pending. Repeated parallel test processes retain those outcomes.

`match-signaling.test.ts` and `match-signaling-worker.test.ts` keep Match peer
setup separate from the read-only state subscription. They prove exact bounded
hello/SDP/ICE/end schemas, UTF-8 burst/lifetime caps, room-bound credentials,
object-injected sender identity, exact-target generations, stale/unavailable
recovery, hibernation, duplicate-socket replacement and originless native
protocol negotiation. `test_match_signal_client_js.mjs` checks the dormant
same-origin launcher adapter, including secret-free URLs, welcome custody,
selected-subprotocol custody, outgoing sequencing and fail-closed stale server
generations. `hibernation-contract.test.ts` inventories all four Durable Object
sources and rejects standard WebSocket acceptance, event listeners, timers or
outbound WebSockets that would consume idle duration; both socket objects must
retain the hibernation API and serialized attachments. None of these
tests enable Start or send gameplay/preflight data through the service.

`test_party_experience_canary.py` pins the zero-analytics operated canary:
HTTPS-only origin shape, fixed 20-attempt lanes, nearest-rank p95 thresholds,
aggregate-only output and refusal to overwrite evidence. The production-path
runner is `tools/run_party_experience_canary.py`; its explicit loopback
development mode can smoke real Worker/WebSocket/WebRTC behavior but cannot
produce a ledger-qualified day. `test_party_beta_ledger.py` then requires the
exact v1 experience aggregate inside every day of the v3 ledger, including
provider-v2 Durable Object duration, across seven contiguous zero-charge days
and adversarially rejects downgrade, under-sampling, success or latency drift.

`net_local_input` is the ROM-free input ownership gate beneath that later
integration. It merges local-seat-indexed samples into a canonical frame using
the frozen roster, returns the exact authored-slot mask, and preserves remote
slots byte-for-byte. Slot-1, two-local-seat and no-local verifier cases pass;
out-of-range analog, non-neutral absent samples, count mismatches and malformed
mapping tails must fail atomically without touching the caller's output.
`match_input_packet` pins the smallest carrier byte boundary: a fixed 24-byte
big-endian `MINP` v1 payload carries epoch, tick, canonical slot and pad sample,
requires zero reserved bytes and a valid checksum, and leaves the destination
unchanged on every malformed byte. `match_input_bundle` pins the loss-resilient
profile carrier: exactly 64 bytes, one to three implicit consecutive frames for
all four slots, CRC-16/CCITT, zero-neutral unused fields and fail-atomic decode.
Its exhaustive one-byte mutation loop rejects every corrupt byte. Peer
ownership is deliberately absent from both codecs; the launcher supplies an
authenticated slot mask, and a decoded bundle may only narrow that mask.

`match_input_repair` pins the gap-repair codec that answers a loss burst the
bundle's three-tick redundancy has outlived: one 64-byte `MR` v1 message is
either a request naming `{epoch, first_tick, count}` or an answer carrying one
canonical slot's contiguous run of up to twelve pad samples. A request may not
exceed the rollback budget (`MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS`), may not name
a slot and may not carry samples; an answer's cells past `count` must be zero
on the wire, so two encodings of one run are byte-identical. Carried samples
are held to the bundle carrier's own shape (present, sticks within +/-80). The
exhaustive one-byte mutation loop rejects every corrupt byte, and an unknown
version or kind is refused rather than parsed on its byte layout. Its answer
fixture builder asserts its storage bound; rejection cases change metadata
only after constructing bounded sample storage, so the test harness does not
violate the condition it is checking in the codec.

`net_impairment` and `net_clock` own deterministic hostile-environment
schedules. The named LAN/regional/poor/outage/adversarial packet profiles cover
latency, jitter, loss, duplication, reorder, corruption, a cadence-derived
two-second outage and bounded deliveries per destination/tick. Clock profiles
independently vary endpoint offset/drift, long frames, skipped scheduler
opportunities and sleep/wake; only `tick_offered` increments authored work, so
host recovery can never disguise a changed simulation timestep. Same-profile,
same-seed schedules are byte-identical and forced controls prove each branch is
observable.

`net_failure_ring` owns the allocation-free in-process forensics ring: 2048
fixed-width typed records (rollback save/load, desync, recovery, queue
pressure/overflow, frame commit, input predicted, late input discarded, peer
loss, lifecycle boundary, progress watchdog, stall begin/ongoing/end with a per-peer RTT/jitter/byte snapshot, session failure).
The unit test pins wrap retiring the oldest records, the typed field
round-trips, the redaction filter that refuses any code source outside
`[A-Za-z0-9_-]` whole (so no URL, host:port pair, ICE candidate line or spaced
SAS phrase can reach a dump), stall/watchdog emission, and oldest-first dump
ordering. Recording reads no clock of its own: the simulation side stamps its
authored tick and the transport side the host clock it already samples.

The unit test also pins where a dump lands. With `MDKR_STATE_HASH_FILE` set it
sits beside that artifact as `<artifact>.netfail`; with none set -- every
shipped build -- it falls back to `mdkr64-online-failure.txt` in the directory
the app shell keeps `mdkr64.log` in, resolved by `mdkr_user_log_directory()`
and handed to the ring once at live-adapter construction. The test links the
real `platform/user_paths.c` with a link-time preference provider, so that
fallback is proven end to end without SDL.

`net_failure_ring_impairment` is the lane over it, and it drives the PRODUCTION
path only. Its scenario is the live adapter's own `--forensics` test
(`test_forensics_dump_on_midrace_peer_loss`): two real adapters over a real
loopback DTLS mesh with every mesh transmission gated by a seeded
`net_impairment` two-second-outage carrier, then the opponent goes silent until
the survivor's control-ping ladder resolves the typed `PeerLost`. The test
writes no ring record and calls no dump -- every record comes from
`platform/net` and `platform/online`, and the dump is the one the adapter's own
peer-loss handler writes. `check_net_failure_ring_impairment.py` (registered as
a CTest, given both arms) requires that dump to name the loss reason by name
and to carry the last stall record with a per-peer snapshot showing real mesh
traffic, and refuses any code field that escaped redaction. Its positive
control is the identical adapter, mesh and transport built with
`MDKR_NET_FAILURE_RING_DISABLED`: it runs the same race to the same peer loss
and must FAIL those assertions, so deleting the mesh recorder or the adapter's
dump cannot leave the lane green.

`check_persistent_app_session.py` is the fast native ownership proof (three
five-tick epochs). `check_persistent_rollback_rematch.py` is its ROM-backed
release counterpart: the same launcher loans one engine three times for full
8,200-frame 4P races, runs the deep tick-4,800 rollback in every epoch, draws
the launcher between loans, and requires results plus clean host teardown each
time. `check_online_profile_rematch.py` keeps that launcher alive for three
regional-variable online epochs and requires fresh epochs/manifests,
non-vacuous loss/duplicate/reorder/clock faults, zero stale or unauthorized
transport ingress, no recovery, stable launcher identity and three clean arena
teardowns. The ASan arm is load-bearing: it previously found stale audio-player
list roots, particle-pool pointers and track-menu background roots on later
loans, plus a seat-count mismatch that dereferenced caller storage before
structural rejection. Engine teardown now retires the menu/background roots
before arena release, and the session bridge rejects the count before reading
samples. The source mutation contract in `test_audio_rollback_adapter.py` now
also requires the audio roots to be retired before a replacement arena can be
allocated. The typed app boot seam
accepts either an authored-tick limit or a presentation-frame limit, never
both; multiplayer menu fixtures use frames.

`check_runtime_safety.py` is the production-seam companion to the
`runtime_contracts` CTest. The unit exhausts audio group/sound/vehicle row
domains, finite X/Z normalization boundaries, save-derived model selection,
16-slot course flags, trophy worlds, Pak extensions, exact texture/CI capacity,
custom-FX asset spans, MIPS `cvt.w.d`/low-half behavior, and signed 16.16 audio
reconstruction. Each boundary has valid lower/upper cases and invalid cases on
both sides. The source census locks owner-last level teardown, both racer
recovery callers, initialized plane/item state, exclusive sound bounds, and
both special-vehicle mappings. It deletes every required fragment in memory and
must reject each mutation, so its source assertions have executable failure
directions.

The guards added in 1.8 are not pinned as source text. Where a guard can be
exercised without a ROM it has a behavioural unit, which this gate also runs:
`ainode_route` (the AI-node rebuild's duplicate-ID, stale-link and cost guards,
the route search's stale-link checks, and the route search against the original
algorithm), `engine_guards` (`weather_reset` refuses a negative weather type
before tearing anything down; `clear_lap_records` clears appended courses
instead of reading defaults past the retail asset) and `save_records` (all four
progress and record codecs use the retail save inventory). The rest are token
checks of the essential guard inside the function that needs it, in the code
the native build compiles: whitespace, line breaks, comments and braces do not
matter, nothing is counted, and an `ordered` guard must come before the read it
protects. The controls delete each guard's tokens, reverse each order and plant
each forbidden form (every one must fail), and rewrite every guarded file with a
comment and a line break between all tokens (every guard must still pass).
Deleting the empty-batch skip in `tracks.c`, caching a failed object-header
load, reading an object-map reference before resolving it, or sizing the
redirected level-header load from the unchecked span each turns the gate red;
rewording a guard's comment or bracing its body does not.

## Two ways a gate lies — read this before adding a check or a diagnostic

Gates fail in two distinct ways here, and only one of them is loud.

**Shape brittleness** is a check that greps for the *form* of the code rather
than the property it cares about. `check_overlay_input_handoff.py` reads
`platform_input_pump`'s body for `SDL_PollEvent`; a refactor moving that into a
helper turned it red with behaviour untouched. Loud, annoying, cheap. The
counter-intuitive part: making such a check **more precise makes it more
brittle, not safer**. Pinning "exactly these two guard terms" breaks the moment
a third surface appears. Loosen *what the text must look like*, keep *what the
code must do* — or better, assert structurally: inline the callee before
searching, allow an open-ended term list, count events rather than matching
prose.

**Assertion vacuity** is a check that cannot fail for the reason it claims. It
is quiet, and it is far worse, because it converts "unchecked" into "checked and
fine". Four found in one night:

- `g_frameLimitRectValid` was a sticky latch, so a per-frame "the widget is on
  screen" assertion was never evaluated against a visible widget.
- `check_a11y_shell`'s in-game overlay arm walks whatever it reaches — three
  passing runs covered 14, 8 and 6 rows. The real assertion is "more than zero".
- "a header declaring more than the cache holds is refused" **still passed with
  the guard it tested disabled**; only counting decoder entries and allocator
  high-water could distinguish refused-before-decoding from refused-after.
- `check_bluey2_rematch` asserted the very defect it existed to catch.

The test is simple: **ask what would have to break for this to go red.** If the
answer is not the property named, the assertion is decorative. Prove it by
mutation — and when a mutation leaves one assertion green while others fail,
that green one is worthless; say which survived rather than accepting the
aggregate red.

**A diagnostic can lie too, and confidently.** The off-screen helper added to
the settings panel announced `frame-limit combo is scrolled out of the panel
(rect y=614..614)` about a combo sitting visibly at y=596..633 — `BeginCombo()`
returning true has already begun the popup window, and ImGui's `Begin()`
reassigns last-item data to that window's title bar. A diagnostic exists to make
the next failure cheap to read; one that points at a fault which is not there is
worse than the silence it replaced. Same shape as a `grep -q` inside a pipeline
under `set -o pipefail`, which SIGPIPEs its producer and reports **"ok" exactly
when it finds a violation**. Verify a new diagnostic against a known-good case
before trusting it, and never use `grep -q` in a pipeline under `pipefail` —
drain with `grep .` or capture to a variable first.

## Open loop vs closed loop — read this before adding or editing a fixture

An **open-loop** fixture replays fixed button presses at fixed frames and asserts
against the one trajectory those presses happened to produce. That is fine for a
menu, where nothing between the input and the assertion has any state to
accumulate. It is a trap for anything with a racer in it: the line is chaotic with
respect to *any* change in the simulation, so a physics or RNG change re-strands
the kart and the check fails for a reason unrelated to what it tests. Three
separate recalibrations had already been forced that way before the "closedloop"
wave, and the ROM-faithful RNG seed / arctan table / table-based trig broke four
fixtures at once.

A **closed-loop** fixture steers toward something each frame and asserts on the
*outcome*. Two mechanisms exist, both no-ops unless set:

- `MDKR_AUTOPILOT=1` — hand the human racer to `racer_AI_pathing_inputs()`, DKR's
  own driver. Use this for anything on a race track.
- `MDKR_DRIVE_ROUTE=...` — steer at named level objects and waypoints
  (`platform/mdkr_adventure.c`). Use this in the Adventure hub and world lobbies,
  where the AI-node graph is seven disconnected components and the autopilot can
  only lap the beach.

Outcomes to reach rather than replay, when a check needs a specific event:
`MDKR_LOAD_TRACK`, `MDKR_FORCE_LAPS`, `MDKR_BOSS_WIN`, `MDKR_BOSS_PRECLEARED`,
`MDKR_ADVENTURE_WIN`, `MDKR_COLLTEX_FORCE`. Writing the one verdict field after
the natural prerequisite beats perturbing the physics until the branch happens
to be taken — `MDKR_BOSS_WIN` replaced `MDKR_BOSS_SLOW` for exactly that reason
(see the grid-mask section below).

### Every fixture, classified

| script | frames | driven by | loop |
|---|---|---|---|
| `nav_to_options` | 1700 | `check_nav_fixtures.py` → `menu_init: menuId=12` | open, **by design** — menus only |
| `nav_to_audio_options` | 1900 | `check_nav_fixtures.py` → `menuId=13` | open, by design |
| `nav_to_save_options` | 1950 | `check_nav_fixtures.py` → `menuId=14` | open, by design |
| `nav_to_magic_codes` | 2050 | `check_nav_fixtures.py`, `check_array_bounds_sweep.py` → `menuId=10`; the nav gate also submits valid `ARNOLD` and invalid `ARNOLE` through the onscreen keyboard | open, by design |
| `nav_to_character_select` | 1600 | `check_nav_fixtures.py`, `check_charselect_motion.py`, `check_determinism.py`, `check_array_bounds_sweep.py` → `menuId=3` | open, by design |
| `nav_to_game_select` | 2000 | `check_nav_fixtures.py` → `menuId=19` | open, by design |
| `nav_to_file_select_adventure` | 2100 | `check_nav_fixtures.py`, `check_array_bounds_sweep.py` → `menuId=6` | open, by design |
| `nav_to_track_select` | 2300 | `check_nav_fixtures.py`, `check_array_bounds_sweep.py` → `menuId=15` | open, by design |
| `nav_charselect_after_track_select` | 2901 | `check_framed_world_views.py` (aperture-leak arm) → character select reached *through* Track Select | open, by design — menus only |
| `nav_charselect_late` | 2901 | `check_framed_world_views.py` (aperture-leak arm) → the same screen, same age, no Track Select visit; the pair's recovered horizontal lens ratio must be 1.0 | open, by design |
| `nav_to_time_trial_race` | 2900 / 3500 / 4000 / 7500 | `check_nav_fixtures.py` → `level_load levelId=5`; **`check_widescreen_shadow.py`**; **`check_race_drive.py`**; **`check_shadow_plausibility.py`** (+ `MDKR_LOAD_TRACK` to retarget the 1P worlds) | open to the grid, then **closed** (`MDKR_AUTOPILOT`) |
| `race_drive_long` | 3900–4300 | `check_texture_lineswap.py`, `check_rom_revision.py`, `check_determinism.py` | open, **by design** — these three compare *pixels between two arms of the same route*, so the route only has to be identical to itself |
| `race_drive_time_trial` | 1500 | `check_determinism.py` | open, by design — same reason |
| `race_full_3lap` | 12000 | `check_array_bounds_sweep.py` | **closed** (`MDKR_AUTOPILOT`) |
| `race_full_3lap_tt` | 6500–13000 | `check_race_finish_time.py`, `check_collision_gridmask.py`, `check_boss_win_verdict.py`, `check_collision_untextured.py`, `check_track_sweep.py`, `check_vehicle_sweep.py`, `check_array_bounds_sweep.py`, `check_collision_headroom.py` | **closed** (`MDKR_AUTOPILOT`, + `MDKR_LOAD_TRACK` to retarget) |
| `race_2p_split` | 3500 / 9600 | `check_race_2p_split.py`, `check_shadow_plausibility.py` (2-view shadow budget tier) | **closed** — autopilot drives *both* humans, through results→track-select |
| `race_3p_split` / `race_4p_split` | 3500 / 9600 | `check_race_multiplayer.py`; `race_4p_split` also `check_shadow_plausibility.py` (4-view tier) | **closed** — every human racer, all four quadrants, 3P minimap, and results→track-select |
| `adventure_hub_drive` | 2300 / 6500 / 12000 | `check_filename_entry.py`, `check_widescreen_proportions.py`, `check_adventure_hub.py`, `check_save_failsafe.py`, `check_texture_lineswap.py` | **closed** (`MDKR_DRIVE_ROUTE` island tour; filename check stops at the character grid) |
| `adventure_resume_race` / `adventure_two_resume_race` | 5200 | `check_adventure_two.py` | **closed** — canonical unlock/save identity, all 20 mirrored racing lines, viewport, stereo, minimap, steering, and pixel reflection control |
| `adventure_race_loop` | 7000 / 17000 | `check_door_glyphs.py`, `check_adventure_race_loop.py` | **closed** (`MDKR_DRIVE_ROUTE` + `MDKR_AUTOPILOT`) |

A run counts as a failure on a non-zero exit, a `[CRASH]` backtrace, a `[FATAL]`
abort, or a missing assert line.

The build itself is also a call-ABI gate. Clang/Emscripten rejects every implicit
function declaration, and wasm-ld warnings are fatal. This specifically prevents
the historical `f32 log(f32)` versus libc `double log(double)` collision and the
implicit-`int` bounds-probe calls from producing a runnable-looking wasm module.
A web clean build may emit only Binaryen's
`warning: no output file specified, not emitting output`, caused by the required
`--emit-symbol-map` print operation.

The nine `nav_*` routes are asserted by `tests/check_nav_fixtures.py`; before the
"closedloop" wave their expected terminal state lived only in this table and five
of them had no automated consumer at all.

## In-game video options — `tests/check_video_options.py`

```bash
python3 tests/check_video_options.py --build build
MDKR_RENDERER=gl python3 tests/check_video_options.py --build build
```

The focused fixture enters native-port menu 29, changes every player-facing
video/accessibility control, returns to Options, and runs four isolated arms:
successful atomic save plus fresh-process reload; environment-owned lock with no
override baking; an unwritable temporary path where every video transaction
fails closed; and malformed internal browser-launch arguments that return 2
without changing an existing file. It also requires the live renderer to move
to 3× supersampling. Every arm uses a private working directory and
`MDKR_SAVE_DIR`; it cannot read or delete a player's real EEPROM or config.

The full `check_browser_runtime.py` injects the same fixture into the actual wasm
build, persists through IDBFS, creates a fresh document, and requires the
16:10/FOV-50/3× state to return. Save-only wipe and ROM forget controls must
retain `/save/mdkr64.ini`.

## Portable settings placement — `tests/check_portable_paths.py`

```bash
python3 tests/check_portable_paths.py --build build
```

A `portable.txt` marker beside the executable must relocate the settings write
next to the game, and its absence must leave the home-directory path in use
(issue #33, the non-ASCII home path). Both arms drive the real binary against
isolated directories; a control arm proves the CWD-relative fallback stays
untouched. The check needs no ROM and accepts `--rom` only for the suite's
common invocation shape.

## Original Audio Options persistence — `tests/check_audio_options_persistence.py`

```bash
python3 tests/check_audio_options_persistence.py \
  --build build --rom baserom.us.v80.z64
```

This enters the retail Audio Options screen, changes both original sliders, and
proves the native settings file commits before the menu transition. A second,
isolated arm uses a missing config parent to force the real atomic writer to
fail. The menu must stay open, render the save warning, accept an A-button
retry, and leave only after B explicitly chooses session-only levels. Neither
arm can touch the player's normal settings or save directory.

## Native renderer routing and fail-closed selection — `tests/check_renderer_backends.py`

```bash
python3 tests/check_renderer_backends.py --build build-rel -v
```

The GL and WebGPU arms independently replay `nav_to_time_trial_race.txt` through
frame 3200 in isolated save directories. They must identify and initialize the
requested backend, emit identical menu/level transition traces, enter playable
Ancient Lake, and dump the same frame set. On the measured baseline, six
non-flat scene pairs have per-frame RGB mean absolute difference at most 1.694
(mean 0.872); the gate allows 4.0 per frame and 2.0 on average. It permits one
one-sided sample at a transition because GL reads the current back buffer while
WebGPU's asynchronous readback can retain the last presented scene.

Those sparse whole-frame metrics are a coarse backend-plumbing check, not GL
visual qualification: dense intro capture exposed localized GL corruption they
did not reject. A separate no-selector arm captures frames 640 and 652 at the
known bad interval and requires them to resolve to WebGPU and match an explicit
WebGPU run byte for byte. This protects the release default while GL remains a
diagnostic backend.

The startup negative arm forces WebGPU window creation to fail and requires the
engine's deliberate `EXIT_FAILURE` path with no crash marker and no GL
initialization. GL remains testable only when the run explicitly sets
`MDKR_RENDERER=gl`; there is no automatic renderer switch.

## Native GPU production and surface suspension

```bash
python3 tests/check_gpu_backpressure.py \
  --build build-rel --rom baserom.us.v80.z64
python3 tests/check_pacing_quality.py \
  --build build-rel --rom baserom.us.v80.z64
python3 tests/check_app_adopted_pacing.py \
  --build build-rel --rom baserom.us.v80.z64
python3 tests/check_overlay_pause.py \
  --build build-rel --rom baserom.us.v80.z64
python3 tests/check_overlay_pause_cutscene.py \
  --build build-rel --rom baserom.us.v80.z64
python3 tests/check_surface_suspension.py \
  --build build-rel --rom baserom.us.v80.z64
```

`check_gpu_backpressure.py` runs real native GL and WebGPU at the synthetic
uncapped opportunity rate with visible swapchains. Production WebGPU submits
only authored tick images, nonblocking-polls completion callbacks, and must
record zero runtime wait calls/nanoseconds; an orderly shutdown may drain any
remaining work. GL interval-0 fences keep their two-frame ceiling and must
exercise the fence-wait path. Every submission retires by shutdown with no
failure or abandoned completion. The gate also requires effective
immediate/interval-0 diagnostics, zero leaked child resources, and a measured
achieved submission rate.
An additional no-selector arm requires the production native default to resolve
to WebGPU, preventing a backend-policy edit from bypassing the qualified path.

`check_pacing_quality.py` measures how EVENLY images arrive rather than how many
of them do. The engine publishes three distributions at shutdown under
`MDKR_PRESENT_PERF=1` — wall interval between present opportunities, wall
interval between frames that actually reached the screen, and interpolation
phase advanced between displayed frames — as fixed-bucket
`[PRESENTPERF-HIST]` histograms carrying p50/p95/p99/max/variance, alongside a
`[PRESENTPERF-LATENCY]` present-queue-depth estimate (mean/max GPU frames in
flight at present, times the refresh period) that quantifies the cost of a FIFO
present mode.

Failure reports retain each paced companion attempt's own distributions,
displayed-frame count, slot quantum, stalls, phase regressions, re-anchor count,
and retry/classifier notes. A strict-arm baseline is not evidence about a
different failing companion arm. The ROM-free `pacing_quality_reporting` CTest
checks that attribution and keeps a failed slot budget red; it also verifies
that companion evidence cannot populate the strict-baseline success list.
Both realtime retry loops call the same mandatory integrity check before any
no-display/unthrottled exemption: realtime identity, present-mode evidence,
no tearing, no live-sink underruns and no backward phase movement. The contract
covers missing counters and each broken invariant, and checks the integration
order in both loops. These additional cases await execution; quality thresholds
and retry budgets are unchanged.

Synthetic arms cover every presentation policy — including the battery-friendly
`40` cap and `display-margin` — crossed with both smoothing
settings and assert only structural identities: the census must count the same
presents the scheduler did, every present after the first must contribute
exactly one interval sample and every displayed frame exactly one phase sample,
the interpolation phase must never run backwards or fail to advance, and with
smoothing off no displayed frame may advance the phase by less than a whole tick
(there are no interpolated images to display). Synthetic pacing does not sleep,
so those arms deliberately assert no wall-clock number.

The realtime arm is a genuinely paced, compositor-visible run
(`MDKR_PACE_REALTIME=1`) on the display policy with smoothing. It gates
no-tearing-mode and `underruns=0`, and REPORTS the displayed-interval and
phase-variance tails as the labelled quality baseline. Those are reported rather
than gated because a threshold chosen before a baseline exists is a guess.

Two session conditions make those numbers meaningless without anything being
wrong in the code, and both are detected and named explicitly rather than
reported as a generic failure: a session with no window server refuses every
drawable, and a session that accepts presents without vsync-blocking them lets
the display policy free-run (it installs no software limiter and relies on FIFO
to pace it). In both cases the no-tearing and no-underrun assertions still run —
neither can be excused by the environment — and only the baseline is withheld.
The arm is genuinely bimodal on a developer machine: the same command can
vsync-throttle on one run and free-run on the next, so read the printed baseline
or its absence rather than assuming one was produced. `--skip-realtime` runs the
synthetic arms alone.

`underruns` in `[AUDIO-SINK]` counts the sink observed fully drained after it
had been fed at least once, which is the audible starvation a gate can require
to be zero; it excludes the boot prime, where the queue is legitimately empty
because nothing has been enqueued yet. `floorbreaches` is the softer
"would not have survived another refill gap this long" signal, reported for
trend. Headless runs open no audio device, so the deterministic coverage for
both counters is `tests/test_audio_queue_controller.c`.
`check_app_adopted_pacing.py` enters through the real ImGui launcher and makes
the engine adopt the launcher's context/device. Numeric 240 Hz and Uncapped
must complete on the WebGPU default and explicit GL path with fully drained GPU
work and no completion failures. This is the regression for the null GL entry-point crash
which affected both of those launcher choices.

`check_overlay_pause.py` enters through that same production app-owned WebGPU
window, navigates the ordinary title-to-Time-Trial route, and opens the ImGui
overlay only after the kart and race clock are live. The complete v3
authoritative-state hash plus position, clock, checkpoint, and lap telemetry
must remain exactly fixed for the whole open interval and advance again after
close. Its PCM arm also requires the independent overlay pause mix, reduced but
live music, suppressed race effects, bounded sample edges, and restoration of
the latest underlying authored mix on resume. This prevents an input-capturing
overlay from masquerading as a pause menu while gameplay or its dominant
feedback continues behind it. Its handoff arm opens the overlay through a real
dispatched key event and closes it from the render callback, which is the shape
the on-screen Resume button has under a mouse click, Enter, or gamepad nav.
Capture must be claimed on the open and given back on the close: issue #20 was a
suppression latch that only ever moved when the capture state changed across a
dispatched event, so Resume left the pad muted for the rest of the session while
F1, Escape and B/Circle were fine. `check_overlay_input_handoff.py` is the
ROM-free companion that pins the same contract in source — one shared release
routine, reconciled against the overlay's live answer both before and across
dispatch — because the defect was the absence of a call, which no run of the
paths that still worked could reveal.

`check_overlay_pause_cutscene.py` covers the other half of that pause.
`check_overlay_pause.py` proves the simulation freezes; this proves the PICTURE
and every object that must publish it survive the freeze. It opens the overlay
during the title screen's scripted camera, the new-game intro animation, the
title attract race, and an Ancient Lake start-line flyover. Every frame presented
while it is open must keep roughly the boundary frame's colour count, stay within
a few mean levels of that frame, and start moving again after the close; every
process must also exit cleanly. The original menu-camera arms went to a single
flat colour — each level's own background fill — because `update_menu_scene`
keeps running `obj_update(0)` to re-arm its camera and an animation-path follower
divided by zero. The attract arm covers the same required zero-rate object walk
with eight racers: their vehicle solvers also divide by the update rate, and the
pre-fix Greenwood Village run aborted with a NaN racer at the first paused tick.
The race-start arm covers the complementary in-game path: its camera-bank pulse
must be retained when an app overlay opened from presentation reaches the next
input boundary. Before the fix, frame 2680 jumped 73.166 mean levels from the
held flyover to the ordinary behind-kart camera. The colour-count assertion
alone would pass on a scene that kept animating behind the overlay, so the hold
assertion runs beside it. Frame capture from an app-shell run comes from
`MDKR_APP_AUTOPLAY_DUMP_FRAMES`, which forwards `--dump-frames` through the
synthesized engine argv.

`check_surface_suspension.py` compares equal-tick control and minimized arms on
both native backends. The minimized interval must stop real display-list walks
and replay, perform no more than two presentation boundaries, and rebase once
on resume. All 30 v3 state, ordered-event and consumed-input rows plus the
temporary PCM digest must remain byte-identical to the visible control.

## WebGPU lifecycle and recovery — `tests/check_webgpu_recovery.py`

```bash
python3 tests/check_webgpu_recovery.py \
  --build build --rom baserom.us.v80.z64
```

This integration matrix runs 97 cases: 96 injected failures across instance, surface, adapter,
device, queue, and configure bring-up; every surface-status repair class;
featureless depth clipping; both native device-loss outcomes; and all
Pure/Remastered scene-target, shader, texture, draw, post, resolve, mip, capture,
and readback constructors reached by DKR, plus one un-injected baseline. The
last 21 arms install a generated custom character into a private catalog and
drive the direct Workshop preview context, the only route that reaches the
skinned renderer and its optional occlusion-evidence pass. The baseline first
shows that route accepting every custom draw and publishing visibility
evidence; each of the 20 injections then proves the failure stays local — the
custom draw is refused or the evidence is withheld while the frame still
completes. These 21 runs of 180 frames each add several minutes to this
GPU-serial lane.
Required resources may rebuild the
WebGPU device once, then must terminate cleanly without switching renderers;
mip and diagnostic failures must stay local. Every
case uses a private save directory and rejects driver validation errors,
assertions, aborts, and uncaptured device errors. The ROM-free
`webgpu_lifecycle` and `webgpu_artifacts` CTests exhaust the pure
transition/usage policy and explicit OS/CPU/ABI artifact selector.

`tests/check_webgpu_fault_matrix.py` is the ROM/GPU-free companion. It requires
every public fault name to be wired to a real backend site and classifies every
site by product route, dialect, and failure policy. It deliberately identifies
the inherited MGB64 minimap, modern-mesh, and prewarm paths — and GE007's
uncalled mid-draw readback probes — as dormant in DKR; those names are not
counted as runtime coverage. The custom-character skinned points are the
opposite case: shipped-conditional, reachable only with an installed and
selected Workshop package, and therefore covered by real arms rather than
excused.

## Real browser runtime — `tests/check_browser_runtime.py`

```bash
python3 tests/check_browser_runtime.py \
  --engine-dir build-web --shell-dir dist/web \
  --rom baserom.us.v80.z64 --camera-obstruction modern
```

The registered task passes `--camera-obstruction modern`; omitting the flag
skips that arm entirely. It sets the first browser document's camera policy and
`MDKR_CAMERA_TRACE=1`, then requires at least `--frames` minus 100 telemetry
rows, the requested gate on every one of them, zero duplicate solves,
projection mismatches, penetrated or invalid or degraded resolved lenses and
hidden targets, nonzero applied corrections on the Modern arm, and one dynamic
publication row per camera row with zero missing caches, missing identities,
uncategorized hits, invalid transforms, and capacity failures.

This is a runtime gate, not a wasm-file inspection. Using only the Python standard
library and the Chrome DevTools Protocol, it serves the committed shell with the
freshly linked JS/wasm, launches an isolated Chromium profile, and chooses the
external ROM through the real file input. The ROM is never copied into the served
tree.

The first document must run 3,600 rAF-paced frames through the title and menus
into Ancient Lake. Five screenshots must be non-flat and changing; original-mode
cadence must remain 24–36 fps with at least 80% `updateRate == 2` and no
sub-two-field updates; the racer must advance;
the AudioWorklet must consume PCM; its first active block must report nonzero
fixed-mode RAW16 loads/bytes; and the C renderer must observe 1260×540 DPR-2,
640×480 DPR-1, then 1260×540 DPR-2 backing stores after live CSS resizes. All
music, jingle, and SFX event queues are measured, may not drop a post, and may not
consume more than half their budgets. The production run forces an
attachment-only surface to exercise the render-blit present fallback, drops one
overlay pass, and requires shader/attribute/varying telemetry to remain within
the granted device limits with zero table overflow or pipeline failure.

After flushing IDBFS, the same profile reloads without another picker action. The
exact 512-byte EEPROM hash and 12 MiB ROM must exist before `main()`. Erasing
progress must retain the ROM; forgetting the ROM must empty both stores. CDP and
the local server audit every request: only local GET/HEAD requests with no body
are allowed, and the ROM filename may not enter any URL.

The gate self-validates the important failure directions: flat-black visual data,
a synthetic ROM POST, a mismatched EEPROM hash, and a forced one-entry SFX queue
must all be rejected or detected. The test bridge is injected in memory before
page JavaScript; it is inert for ordinary visitors and accepts no fixture from the
URL.

`check_browser_presentation_rates.py` is the independent browser pacing gate.
It runs the actual wasm/WebGPU engine in isolated Chromium profiles with exact
rational rAF timestamps while still yielding through the real browser event
loop. Original, display-at-144, capped-60-on-144, irregular display, and a
shared native `uncapped` config must all complete 60 fixed authored ticks with
byte-identical v3 state, ordered events, consumed input, and temporary PCM.
Display 144 must issue exactly 288 presents; capped 60 exactly 120. The browser
launcher must omit unbounded presentation and disclose the rAF ceiling, while
the shared `uncapped` value must report requested `uncapped`, effective
`display`/FIFO, and its `raf-ceiling` reason.

A final stored-ROM reload forces engine-level WebGPU adapter failure. The canvas
must be hidden, the launcher and independent recovery controls restored, and an
actionable graphics error remain stable instead of a black frame.

The third arm sets `MDKR_TEST_WEBGPU_WINDOW_FAIL=1`. It must log the injected
window failure, change the cached selection to GL before `gfx_init`, and produce
a non-flat GL menu frame. The comparator's positive control pairs the late race
scene with black; measured MAD is 92.9, far beyond the 4.0 threshold. Debug and
Release both pass.

## Browser save custody — `tests/check_browser_save_ui.py`

```bash
python3 tests/check_browser_save_ui.py \
  --engine-dir build-web --shell-dir dist/web
```

This is the fast ROM-free companion to the full browser runtime. It removes
`navigator.gpu` before page code runs, never selects a ROM, and rejects any load
of `mdkr64_web.js`/`.wasm`. The launcher must still expose enabled save controls
and instantiate only the small shared-codec save-tools module.

The check drives raw and portable export, safe untrusted metadata preview, eight
malformed/oversized inputs, all six IDBFS transaction fault points, byte-exact
rollback, corrupt-original forensic export, corrupt-block recovery, block merge,
one-field edit containment, destructive cancellation, complete wipe, real
file-input import, and reload persistence. Chromium's accessibility tree,
dialog focus/return, keyboard drop target, live status regions, and every local
network request are asserted. The publish workflow runs this gate independently
of the long game/WebGPU check.

## New-save filename C-string check — `tests/check_filename_entry.py`

```bash
python3 tests/check_filename_entry.py --build build-asan -v
```

This is the shortest first-session route through FILE SELECT and the new-save
character grid. It runs in a fresh temporary working directory, so it neither
depends on nor mutates `save/eeprom.bin`. The check requires `menuId=6`, a zero
exit, and no sanitizer or fatal text through frame 2300.

The historical one-byte `gCurFilenameCharBeingDrawn` binary is retained only as
an external positive-control artifact during validation. Running the same harness
with `--expect-asan` requires a non-zero exit, an AddressSanitizer report, the
symbol name, and the `font.c` scanner site; it aborts at frame 2052. The fixed
ASan, Debug, and Release builds all pass. This both-direction requirement prevents
the route from going green merely because it stopped reaching filename entry.

## Native representation boundaries — `tests/check_native_layout.py`

```bash
python3 tests/check_native_layout.py \
  --rom baserom.us.v80.z64 --asan-build build-asan -v
```

This is the dedicated gate for three host-ABI defects: heterogeneous object tails
that inherited N64 alignment/pointer sizes, variably aligned serialized object
records exposed as naturally aligned native unions, and small render records cast
to larger scene objects for prefix access.

It first builds a halt-on-error alignment-UBSan target and the ROM-free layout
unit. Linked sanitizer handlers are checked explicitly. Three exact historical
controls must then fail in their known directions: the MEM-02 tail emits four
alignment reports, the MEM-03 HUD-record view emits two, and the MEM-04 bare
transform aborts under ASan when the fake object prefix reads its animation
frame. The fixed unit and a source contract that rejects native fake renderer
casts must remain clean.

The full arm drives all nine menus, all 20 tracks, every one of the 47 legal
track/vehicle combinations, Adventure hub and hub→race→hub routes, boss/object
collision, 21:9 two-player, and `check_widescreen_proportions.py` under that
alignment build. The last route pixel-measures the HUD and world balloons across
4:3, 16:10, 16:9, 21:9, forced 4:3, and changed FOV, and still requires exact legacy stretching to
be rejected. Use `--quick` only during iteration; the registered release task is
the complete arm.

## Input-script format

```
<frame> <TOKEN>[+<TOKEN>...] [holdFrames] [P1|P2|P3|P4]
```
Buttons `A B Z START L R CUP CDOWN CLEFT CRIGHT`; directional tokens
`UP DOWN LEFT RIGHT` drive the **analog stick** as well as the matching D-pad bit
(DKR menus read the stick). `holdFrames` defaults to 3. The trailing field is the
**controller port**, 1-based, default `P1` — so every pre-existing script parses
unchanged. A malformed port field is a hard error: the binary prints
`[input-script] bad port field ...` and exits **1** rather than run a truncated
route (a partially-loaded script would otherwise exit 0 and "pass" while covering
nothing).

## In-race drive check — `tests/check_race_drive.py` (RUN THIS AFTER ANY GAMEPLAY CHANGE)

Everything above passes as long as the process exits 0 with no `[CRASH]`/`[FATAL]`.
That is not enough for gameplay: when the player racer ends up somewhere it should
never be, the segment lookup stops matching, `traverse_segments_bsp_tree()` returns no
visible segments, and the screen becomes a **flat fog-brown field with only the HUD and
minimap on it** — while the race clock keeps counting and the run still exits 0. A
whole class of asset/physics bugs is invisible to a survival-only fixture (that is how
the `ASSET_MISC_8` denormal-divisor bug survived the full matrix; see
`docs/OPEN_ITEMS.md`).

```
python3 tests/check_race_drive.py -v        # ~40 s, muted + headless
```

It drives `nav_to_time_trial_race.txt` with **`MDKR_AUTOPILOT=1`** for 7500 frames —
closed loop, since the "closedloop" wave — and asserts three independent things:

1. **Position sanity** — no `inf`/`nan`, `y` inside a generous track band, and no
   teleport. A teleport is identified by its *shape*: the per-frame change in step
   length (`MAX_ACCEL = 40`, measured 5.8 on a healthy run) plus a generous absolute
   ceiling (`MAX_STEP = 150`). The old absolute 40 units/frame cap does not survive a
   route that actually takes a zip pad in an eight-racer race — measured 44.9
   units/frame sustained over 55 frames, on grass, all four wheels down, ramping
   smoothly, i.e. `BOOST_LARGE` doing what a boost does.
2. **Forward progress** — the traced `cp=` (`courseCheckpoint`, checkpoints crossed
   since the race start, never reset per lap) must reach ≥ 30, `lap=` must reach ≥ 2,
   and the kart must never average < 1.5 units/frame over 240 frames. Position alone
   cannot tell "driving the track" from "ping-ponging between a respawn point and a
   wall".
3. **The scene still draws** — sampled frames (via `MDKR_DUMP_EVERY`) must contain a
   real rendered scene, measured as distinct 5-bit-quantized colours + luma sigma over
   a centre crop that excludes the HUD band.

Healthy reference: 4381 in-race frames, final `cp=48 lap=2`, max step 44.9 at frame
3909, max step-to-step acceleration 5.8, slowest 240-frame mean speed 10.35, sampled
frames 1115–3356 distinct colours / sigma 21–46 (the sampled set shifts a little
run to run because the frame-dump stride and the AI line are independent).

⚠️ **Why it is no longer an input replay.** It used to run `race_drive_long.txt` —
throttle held, stick LEFT for 25 frames in every 120 — and assert against that one
line. That line is chaotic with respect to any change in the simulation: P3.5's
`ParticleBehaviour` stride fix moved it once (forcing `MIN_FINAL_CP` 20 → 15), and the
ROM-faithful RNG seed / arctan table / table-based trig moved it again, to cp 14, lap 0
and a 240-frame window averaging 0.11 units/frame with the kart nosed into a wall.
Nothing about the port was broken either time. `racer_AI_pathing_inputs()` steers
toward the next AI node every frame, so it corrects after a perturbation instead of
accumulating it. The autopilot overrides the stick **after** update_player_racer's
normal input dispatch, so every line of physics downstream — including
`func_80050A28()`, where the `ASSET_MISC_8` divisor bug fired — runs unchanged.

**Verified in the broken direction.** With `ASSET_MISC_8` removed from
`dkr_misc_normalize_tables()`'s word-array list, i.e. the denormal-divisor bug
deliberately reintroduced, this check fails on six assertions at once: exit code −6,
`[FATAL] update_player_racer: non-finite position (nan, inf, nan) … lateral=-inf`,
14 in-race frames of 1000, checkpoint 0 of 30, lap 0 of 2, one dumped frame of four.

`race_drive_long.txt` is still in the tree and still open-loop, because
`check_texture_lineswap.py`, `check_rom_revision.py` and `check_determinism.py` use it
to compare **pixels between two arms of the same route** — for that, the route only has
to be identical to itself.

⚠️ **The 44.9 figure quoted above is historical and no longer reproduces.** This route
crossed a zip pad in 2026-07; it does not any more (measured: zero frames of surface
`SURFACE_ZIP_PAD` across the whole race, and this check now reports max step 14.3).
The AI line moved when the wave "closedloop" corrections landed — the same reason this
file keeps warning against calibrating on a line. `MAX_STEP`/`MAX_ACCEL` are unaffected,
because they were deliberately set so the check does not depend on whether a pad is
crossed. The boost magnitude itself is now measured by `check_boost_magnitude.py`
below, which does not rely on any route reaching a pad.

## Zip-pad boost magnitude — `tests/check_boost_magnitude.py` (RUN THIS AFTER ANY CHANGE TO RACER VELOCITY, `boostTimer`/`boostType`, OR `normalise_time()`)

```bash
python3 tests/check_boost_magnitude.py -v      # ~3 min, muted + headless, four runs
```

Closes the long-standing register question "is the zip-pad boost the magnitude DKR
authored, or a port defect?" (`docs/open-items/gameplay.md`, wave "zippad"). Answer:
**authored.**

It arms the boost rather than driving over a pad, for the reason in the ⚠️ above — no
committed route can be relied on to keep crossing one particular pad, so a check
calibrated on one would be measuring the AI, not the boost.
`MDKR_ZIPPAD_BOOST=<tick>[:<ticks>]` (`objects.c mdkr_zippad_boost_hook`, no-op unless
set) arms **player one only, once**, in exactly the state the decomp arms it in for
`SURFACE_ZIP_PAD` on a car: `boostTimer = normalise_time(ticks)` (default 45, the
authored constant), `boostType = BOOST_LARGE`. Everything downstream is untouched
decomp code, so what is measured is the shipping boost with a deterministic trigger.
`MDKR_BOOST_TRACE=1` emits the per-update `[BOOST]` row the check parses (boost state,
`racer->velocity`, world position, wheel surface).

Four runs, all sequential:

| arm | fixture | `MDKR_ZIPPAD_BOOST` | cruise | boost frames | peak &#124;velocity&#124; |
|---|---|---|---|---|---|
| 8-racer Tracks | `nav_to_time_trial_race.txt` | `4000` | 12.27 | 45 | **22.357** |
| solo Time Trial | `race_full_3lap_tt.txt` | `4000` | 12.67 | 45 | **22.336** |
| control | `nav_to_time_trial_race.txt` | `4000:15` | 12.27 | 15 | 20.880 |
| control | `nav_to_time_trial_race.txt` | `4000:120` | 12.23 | 120 | 22.358 |

The two baseline arms differ by **0.021 velocity units — 0.09%** with the boost armed
identically, which is the assertion that answers the register: the mechanism has no
racer-count coupling, and `normalise_time()` has no framerate term either (it is a PAL
5/6 rescale of the constant and nothing else).

**The trace is asserted on `|racer->velocity|`, not on the position step.** The step is
what the other motion checks use, but it carries cornering and gradient — i.e. the
racing line — into the measurement: normalised by cruise, the two fixtures' ramps
differ by up to 0.17 where the tolerance would have to be 0.25. The velocity trace does
not; the two agree to 0.05 across the plateau. Asserted: 45 boost frames exactly, a
cruising entry state, a monotone ramp, a plateau inside `[21.8, 22.7]` over frames
+25..+34, peak inside `[21.5, 23.0]`, and decay back under 0.75 of the plateau by
frames +55..+88.

**Verified in the broken direction, both ways, on every run.** The `<ticks>` field is
the perturbed boost constant, and both controls must fail or the check reports
`POSITIVE CONTROL BROKEN`:

* `:15` trips **four** assertions — duration, peak, ramp monotonicity (the velocity
  turns over at frame +16), and the plateau (12.98–14.57 against an envelope of
  21.8–22.7).
* `:120` trips **two** — duration, and the tail: the speed is still 0.96 of the plateau
  where the authored boost has decayed to 0.56. This is the arm that proves the
  per-frame trace assertion is load-bearing, because it does **not** change the peak.

That last point is the physically interesting result: **the boost saturates.** Holding
`boostTimer` 2.7x longer reaches the same 22.358, because `traction = 2.0f` per update
against the drag term reaches terminal velocity well inside 45 ticks. A zip pad cannot
produce an unbounded speed however long it is held — its magnitude is bounded by the
authored physics, not by the timer.

## Presentation-mode check — `tests/check_video_presets.py`

```bash
python3 tests/check_video_presets.py --build build \
  --rom baserom.us.v80.z64 -v
```

Runs the `nav_to_time_trial_race` route once per presentation mode and requires
all three normalized `[PACE]` streams to be identical (3400 rows). Same
reasoning as the widescreen check: the modes are allowed to change how the frame
looks, never what the race does.

It also asserts that **Pure reproduces the authored 4:3 framing** — on a
2560×1440 drawable Pure must pillarbox (`presentation=1920x1440+320`, aspect
1.33333) while Restored fills (`2560x1440+0`, 1.77778). That assertion is
deliberate protection against a plausible-looking "simplification": setting
`Video.Widescreen=0` does *not* produce a 4:3 image, it engages the
pre-widescreen path that stretches 4:3 across the window. Pure pins
`Video.Aspect=4:3` instead.

The precedence ladder (`default < preset < env < --video-set`) is checked
through `--video-list`, which exits before ROM load and so costs nothing. Note
that passing a mode flag explicitly counts as a *preset* application even when
it resolves to the same value the default held; `[default]` appears only when no
mode flag is given.

The positive control is an `MDKR_RNGSEED=legacy` arm, which perturbs the boot
seeds and therefore the simulation: its stream **must** differ from baseline. It
currently diverges at row 3157 on a sub-unit position delta. Without that arm, a
comparator that had silently stopped finding rows would read as a pass.

The gate strips every inherited `MDKR_*` variable before running, so a developer
with `MDKR_RENDER_SCALE` exported cannot accidentally turn every arm into the
same env-precedence override.

An odd-size Pure framebuffer arm also checks the real captured side gutters.
Every pixel within each gutter must be uniform and both sides must match; the
gate deliberately accepts the game's authored clear color instead of assuming
that unused presentation space is black.

## Widescreen + shadow-depth check — `tests/check_widescreen_shadow.py`

```bash
python3 tests/check_widescreen_shadow.py --build build \
  --rom baserom.us.v80.z64 -v
```

This runs the same closed-loop race at 4:3, 16:9 and 21:9 and requires the
normalized `[PACE]` streams to match exactly. That is stronger than a screenshot:
DKR's object renderer advances animation, can consume gameplay RNG, and feeds the
racer's on-screen simulation timer, so an over-eager widescreen cull change can
alter the race while still looking plausible.

The same harness asserts that every production shadow draw group requests decal
depth, normal heap high-water marks stay inside all three physical allocations,
and a tiny `MDKR_SHADOW_CAPS=2,8,16` fault-injection run drops whole meshes
without a crash. Its final `MDKR_SHADOW_DECAL=0` arm deliberately restores the
old non-decal route and must be detected, proving the check cannot pass merely
because the fixture rendered no shadows. Run it against an ASan build as well as
the GL and WebGPU builds.

A sixth arm repeats the production 4:3 case in **shipping configuration**
(`MDKR_TRACE` unset). `[PACE]` cannot exist there, so what it asserts is the
strongest untraced subset: the `[SHADOW]` decal mode, all three heap capacities,
the overflow-drop count and the non-decal group count must be identical to the
traced arm, and the arm must carry no `[PACE]` rows at all. This gate dumps no
frames, so the pixel half of the same question is carried by
`check_world_shadows.py`, `check_shadow_visual_ab.py`,
`check_shadow_plausibility.py` and `check_presentation_shadows.py`.

Each arm must exit successfully and emit exactly one clean completion for the
requested frame count. Traced arms must contain every `[PACE]` frame in order,
exactly once. A timeout or incomplete trace still fails the gate, but unequal
partial trace lengths are reported as an **unavailable comparison**, not a
simulation divergence. Missing shadow and depth summaries are reported
independently, and failure context includes the untraced shipping arm. The
180-second per-arm timeout and 4,000-frame default route are unchanged.

Use `--keep-evidence` when diagnosing a failure to retain full per-arm output
and process metadata (command, timeout status, elapsed time, requested/completed
frames) in a fresh private temporary directory. These logs may contain
ROM-derived state: do not commit or distribute them. The normal mode retains no
new artifacts; both modes continue to use isolated disposable saves.

`test_widescreen_shadow_reporting.py` exercises reporting with synthetic output
and mocked process boundaries: completed controls, actual stream differences,
partial/time-limited runs, missing or reordered trace rows, completion markers,
missing summaries, sanitizer diagnostics, launch failures, private evidence,
and all six failure contexts. It launches no game. The reporting correction
does **not** fix or explain an underlying application timeout; complete real
renderer/sanitizer arms remain required for qualification.

For the multiplayer projection path, `check_race_2p_split.py` also accepts
`--window-size WIDTHxHEIGHT`; use `--window-size 1260x540` to exercise both
top/bottom viewports at 21:9 while retaining its independent liveness check for
each half.

### Widescreen/FOV asset proportions — `tests/check_widescreen_proportions.py`

```bash
python3 tests/check_widescreen_proportions.py --build build \
  --rom baserom.us.v80.z64 --renderer webgpu -v
```

This dependency-free pixel gate drives Timber's Island through two deterministic
approach samples of balloon 10: frame 6300 keeps the SAFE_2D HUD glyph clear of
the rainbow behind it, and frame 6410 brings the world-space F3DDKR billboard
close enough for a meaningful changed-FOV measurement. It segments the saturated
blue zigzag inside each balloon and runs seven isolated arms at equal height:
4:3, 16:10, 16:9, 21:9 with the default 104° cap, forced 4:3 centered inside a
21:9 drawable, 16:9 at 75° FOV with the cap off, and exact 21:9 legacy stretching.

Measured on the current Release GL and native WebGPU paths:

| arm | HUD motif | world motif | required interpretation |
|---|---:|---:|---|
| 4:3 | 99×36 | 48×18 | reference |
| 16:10 | 99×36 | 48×18 | identical authored size |
| 16:9 | 99×36 | 48×18 | identical authored size |
| 21:9, cap 104° | 99×36 | 50×19 | 1.053× lens scale; shape retained |
| 21:9, forced 4:3 | 99×36 | 48×18 | centered presentation; authored size |
| 16:9, FOV 75° | 99×36 | 36×14 | 0.752× lens scale; shape retained |
| 21:9 legacy | 173×36 | 84×18 | known-bad 1.75× horizontal stretch |

Every arm must reach the same normalized frame-6410 racer state and collect the
same balloon at frame 6476. Production motif aspect must remain within 8% of the
4:3 reference, size must track the analytic focal scale, and the legacy arm must
fall well outside that threshold. The legacy arm is the positive control: if the
detector cannot distinguish the former distortion, the check fails. It passes
Debug GL, Release WebGPU, and ASan GL.

The same check performs a source census of the unusual post-projection
billboard mode. Exactly two audited producers may enable it, both in `camera.c`
(world and ortho); the world builder must call the shared aspect/FOV correction;
and the F3DDKR decoder must retain one local-offset path. A new direct producer
fails the gate until its transform is classified and covered. Current world
callers — collectibles, weapons/bubbles, boost effects, particles, rain splashes,
lens flare, and the magnet reticle — all converge on that builder. Vehicle-part
sprites use the ordinary isotropic perspective matrix; HUD/menu sprites and
rectangles use SAFE_2D.

### Cinematic and framed live views — `tests/check_framed_world_views.py`

```bash
python3 tests/check_framed_world_views.py --build build \
  --rom baserom.us.v80.z64 -v
```

This distinguishes unframed cinematics from wooden-frame apertures. Opening
logos, animated credits, the unframed Track Select setup page, and initial
post-race footage must fill the horizontal presentation; Track Select's browser
and zoom phases plus later single-player post-race footage must keep live
geometry inside their wooden frames. The reverse Track Select transition must
restore containment before its frame appears. Decorative backgrounds must fill
the side regions without black gutters. The gate captures 4:3, 16:9, and 21:9
output on WebGPU and repeats the ultrawide witnesses on OpenGL.

Three additional WebGPU witnesses run Original simulation cadence at 60 Hz
with interpolation enabled and capture the in-between present crossing Track
Select setup, its reverse transition, and the first framed post-race phase.
Safe-aperture/presentation policy changes are discrete: the snapshot must use
the new camera recipe whole, never blend a safe aperture into a widescreen
projection. Animated viewport coordinates continue interpolating once the
policy is stable.

Each arm is paired with `MDKR_TEST_FRAMED_WORLD_UNSAFE=1`. The renderer fault
must be inert for presentation scenes and all 4:3 captures, while framed scenes
must reproduce visible side-band bleed at wider ratios. Frontend routes and
normalized gameplay traces must stay unchanged between the two arms.

One further arm, `--scene charselect-aperture-leak`, asserts that an aperture
does not outlive the screen that asked for it. Track Select's safe 4:3 region is
per-viewport state that the projection latch reads every tick, while the
unframed render path states the presentation region to the renderer; if the
region survives Track Select the two disagree and every later screen draws a 4:3
lens across the whole presentation rectangle. The arm captures character select
twice at 16:9 — once reached through Track Select, once reached directly on the
same frame — and recovers the horizontal scale that best maps one onto the
other. Character select animates, so the comparison is a per-column brightness
profile rather than a pixel identity. The recovered ratio must be 1.0; a
synthetic 4:3 stretch of the control must be rejected, which is what keeps the
detector honest. Measured before the fix, the leaked route recovered 0.750.

`--scene widescreen-hud-banner` covers issue #67: the finish banner still on
screen when the post-race view shrinks into the wooden frame must not be cut
off when Video.WidescreenHUD is on. It captures the 1ST banner (Tracks), the
FINISH banner (Time Trial, `race_full_3lap_tt.txt`) and the smaller non-first
banner (Adventure) three times each: Widescreen HUD off, on, and on with
`MDKR_TEST_WIDE_HUD_EXPANDED_SCISSOR=1`. That renderer fault restores the
shipped mapping, which read the inherited frame clip from the screen's left
edge and cut the banners at x = drawable height. The capture with the option on
must be identical to the one with it off, inside and outside the frame opening.
The fault capture must lose banner pixels right of x = drawable height and
nothing left of it, so the arm cannot pass on a frame without a banner or with
the option silently off. The non-first banner ends left of that edge at 16:9,
so it runs at 21:9 only. Measured before the fix, every arm failed with its
first missing column at x = 1080 on a 1080-row drawable, or at the banner's own
left edge where the whole banner sat right of it at 21:9.

`check_shadow_visual_ab.py` is the slower pixel-level companion:

```bash
python3 tests/check_shadow_visual_ab.py --build build \
  --rom baserom.us.v80.z64 --renderer webgpu -v
```

It compares 300 consecutive moving-camera frames against the deliberate
`MDKR_SHADOW_DECAL=0` regression. The gameplay streams must remain identical,
some (but not all) frames must differ, and every affected production pixel must
be darker: decal bias may restore missing shadow coverage, but may not perturb
geometry or the rest of the frame. The PPM comparison is dependency-free and
needs no checked-in golden image.

A third arm runs the production decal route in **shipping configuration**
(`MDKR_TRACE` unset) and requires every dumped frame to be byte-identical to the
traced arm's. The comparison is deliberately on pixels rather than on the
`[SHADOW]`/`[DEPTH]` totals: this route is wall-clock paced (it pins neither
`MDKR_SYNTH_FIELDS` nor the cadence, because its texture-filtering invariant
needs `--pure`), so those cumulative counters move run to run — measured 2371 /
2510 / 2590 presents across three arms of one build — while frames keyed to
simulated frame numbers do not.

## Idle / attract-demo soak (no input script)

Not every crash needs input. Letting the title screen idle starts the **attract
demo** at ~frame 5100, which loads real race levels back to back — a path no
`nav_*` fixture reaches, and the only thing that caught the object sub-pool
exhaustion documented in `docs/OPEN_ITEMS.md`. Soak it with:
```
MDKR_AUDIO=0 MDKR_TRACE=1 ./build/mdkr64 --headless-frames 12000 \
  --rom baserom.us.v80.z64 2>&1 | grep -E 'level_load|CRASH|FATAL'
```
Expect level loads at ~14, ~173, ~1134 (frontend), then a new demo level roughly
every 1500 frames, and no `[CRASH]`/`[FATAL]`. The run must reach frame 12000 and
exit **rc=0**; the demo cycles back to the frontend (levelId=23) at ~frame 8132.
This soak is the *only* thing that exercises the audio event-queue saturation and
the boost-graphics draw — both of which wedged or crashed here before, and neither
of which any `nav_*`/`race_*` fixture reaches. Treat a non-zero rc, a stalled
frame counter, or an `[EVTQ] post DROPPED` line as a failure.

`MDKR_EVTQ_STATS=1` adds per-audio-queue high-water-mark reporting
(`[EVTQ] qN(ptr) new peak P of S`) — that is how the music queue was sized. It
walks the alloc list on every post, so use it only for measurement runs.

**`MDKR_AUDIO=off` is a no-op** — `platform/audi_port_dkr.c` tests
`disable[0] == '0'`, so only `MDKR_AUDIO=0` skips the device. `--headless-frames`
is what actually guarantees silence (it disables the SDL audio device
unconditionally); `MDKR_AUDIO=0` is belt-and-braces.

### Autopilot — `MDKR_AUTOPILOT=1` (drive with DKR's own AI)

The open-loop input route holds the racing line for one lap and then strands the
kart, so it cannot be used to reach a race finish. `MDKR_AUTOPILOT=1` instead
drives the **human** racer with `racer_AI_pathing_inputs()` — the same driver every
CPU racer uses and the one that laps real tracks in the attract demo. Race logic,
physics and finish handling are untouched, so what it exercises is the real code
path.

```bash
MDKR_AUTOPILOT=1 MDKR_AUDIO=0 ./build/mdkr64 --headless-frames 12000 \
  --input-script tests/input_scripts/race_full_3lap.txt --rom baserom.us.v80.z64
```

Measured on Ancient Lake (deterministic — identical across runs): lap 1 at clock
1703, lap 2 at 3353, i.e. a consistent ~1650/lap racing line, versus 2776 for lap
1 and a stuck kart on lap 2 open-loop. It is a **no-op unless set**.

It drives **every** racer whose `playerIndex != PLAYER_COMPUTER`, so on a
two-player route it covers *both* human racers with no extra machinery.
## Track coverage sweep — `tests/check_track_sweep.py` (RUN THIS AFTER ANY ASSET/RACE CHANGE)

Everything else in this suite validates **one track in one vehicle** (Ancient
Lake, car). There are 20 playable tracks and three player vehicles, and this
port's recurring failure mode is a per-track asset that decodes wrong and fails
*silently* — `ASSET_MISC_20` (boost), `ASSET_MISC_8` (steer divisor) and the
per-triangle collision facets were all that shape.

```bash
python3 tests/check_track_sweep.py                        # all 20 tracks
python3 tests/check_track_sweep.py --tracks 5,9 --frames 4200
python3 tests/check_track_sweep.py --vehicle 1            # force hovercraft
python3 tests/check_track_sweep.py --expect-fail N        # diagnostic only: tolerate a named failure
```

It uses two hooks, both **no-ops unless set**:
- `MDKR_LOAD_TRACK=<levelId>[:<vehicle>]` (`game/src/game.c`) retargets the *race*
  load of one proven route, so 20 tracks need one fixture instead of twenty. It
  rewrites only the load matching `gTrackIdToLoad` — DKR's own "track to race"
  global — so menu backgrounds and the track-select preview are untouched.
- `MDKR_AUTOPILOT=1` drives with DKR's own AI, so no per-track input tuning.

Both sweeps pin Original presentation/cadence with `MDKR_SYNTH_FIELDS=2`,
producing one authored gameplay ticket per headless host opportunity. They
therefore measure content and physics coverage, not a developer's persisted
frame limit, compiler, sanitizer, renderer, or compositor speed; the separate
pacing gates retain responsibility for real-clock behavior.

Per track it asserts: exit code 0, the level actually loaded as a race, finite
position samples, real forward progress (`courseCheckpoint` climbing), and no
`[FATAL]`/sanitizer/assert output.

**Historical first run:** level 9 alone died while updating a snowball object.
That diagnosis no longer reproduces: level 9 completes a 12000-frame autopilot
race with `maxcp=80` in both the old- and ROM-faithful-math arms. The finding is
therefore retracted on current source, not treated as a known failure. Release
validation must run without `--expect-fail`; otherwise a new level-9 regression
would be silently accepted.

⚠️ **With no `--vehicle`, this sweep drives all 20 tracks in the CAR** — it does
*not* use each track's own default vehicle, and its help text used to claim it did.
`MDKR_LOAD_TRACK` rewrites the level id inside `level_load()`, long after the menus
picked the vehicle from `leveltable_vehicle_default(gTrackIdForPreview)`, and the
previewed track on this route is always Ancient Lake — a car track. So this sweep
also races a car on the three hovercraft-only tracks (Whale Bay 8, Pirate Lagoon 4,
Boulder Canyon 19), which the real track-select menu would never offer. Use
`check_vehicle_sweep.py` below for per-vehicle coverage.

## Vehicle coverage sweep — `tests/check_vehicle_sweep.py` (RUN THIS AFTER ANY RACER/PHYSICS CHANGE)

Every race this port had ever validated was a **car** race, on every track, for the
reason in the warning above. DKR has three player vehicles with genuinely separate
physics modules — `update_player_racer()`'s `switch (vehicleID)` dispatches to
`func_8004F7F4` (car), `func_80046524` (hovercraft) and `func_80049794` (plane) —
and several tracks cannot be raced in a car at all.

```bash
python3 tests/check_vehicle_sweep.py                 # the full legitimate matrix (47), ~30 min
python3 tests/check_vehicle_sweep.py --matrix        # print the matrix and exit
python3 tests/check_vehicle_sweep.py --vehicles 1,2  # hovercraft + plane rows only
python3 tests/check_vehicle_sweep.py --tracks 6,32   # the two loop-de-loop tracks
```

**It sweeps 47 combinations, not 60.** `LevelHeader.available_vehicles` (ROM offset
0x4E) is the bitmask the track-select menu offers and `LevelHeader.vehicle` (0x4D)
is its default; `level_global_init()` packs them into
`gGlobalLevelTable[i].vehicles` as `(available << 4) | (default & 0xF)`, which
`leveltable_vehicle_usable()`/`leveltable_vehicle_default()` read back. Forcing a
plane onto a hovercraft-only track is a meaningless input, not a bug, so the sweep
is exactly the mask: car on 16 tracks, hovercraft on all 20, plane on 11.

The mask is decoded **from the ROM**, independently of any port code (like
`tools/dump_misc_asset.py`), and then cross-checked against the game's own reading,
published by `level_global_init()` as
`[TRACE] level_vehicles: id=N default=D avail=0xM`. Without that cross-check the
sweep would only be agreeing with its own assumption.

Per combination it asserts exit code 0, that the level loaded as a race **with the
requested vehicle**, that the racer was really updated by that vehicle module, all
position samples finite, forward progress (`courseCheckpoint`) plus real
displacement, no `[FATAL]`/sanitizer output — and two things a survival check cannot
see:

- **A plane must use the vertical axis.** `y` spread ≥ 60 world units. A plane that
  loads, "drives", and sits at one altitude is a defect; the pitch axis is an input
  path neither other vehicle touches. Measured plane rows span 331–1235; the
  flattest car row is 210 and the flattest row of any kind is 76 (Whale Bay,
  hovercraft). `MDKR_AUTOPILOT` does drive pitch — `func_80045C48` derives
  `gCurrentStickY` from the spline's vertical derivative.
- **Per track, the vehicles must trace DIFFERENT paths.** The position stream is
  hashed per row; two vehicles on one track colliding means the override did
  nothing. Together with `[PVEH]` (below) this is what makes "we really raced a
  hovercraft" falsifiable rather than assumed.

`[PVEH] frame=N player=P vehicleID=V` is a new greppable line (emitted on change, so
one line per race) reporting the `vehicleID` `update_player_racer()` actually
dispatches on. It is deliberately **not** part of `[PACE]`, whose format stays
byte-compatible with every existing check. `MDKR_LOAD_TRACK=<level>:<vehicle>` only
rewrites the *argument* to `level_load()`; whether the racer object ends up with
that vehicle is a separate question, and a silently-ignored override would have
driven a car through all 47 rows and passed.

**`VEHICLE_LOOPDELOOP` (4) is permitted mid-race and reported as `+loopdeloop`.**
That is not slack: DKR tracks place a vehicle-change trigger object
(`object_functions.c`, the `racer->vehicleID = trigger->vehicleID` branch) around
corkscrew sections, which swaps the racer into the loop physics module
(`func_8004CC20`) and restores `vehicleIDPrev` at the exit trigger. Walrus Cove (6)
in both car and hovercraft, and DarkMoon Caverns (32) in hovercraft, do exactly
this — which is also the only coverage that module has. The *first* dispatch must
still equal the requested vehicle, so the falsifiability above is intact.

**The first plane race ever run in this port aborted**: `f32 sp60[4]` in
`func_80049794` holding the 4x4 `MtxF` that `mtxf_from_transform()` writes — a
48-byte stack overflow, `__stack_chk_fail`, **exit 134 with nothing on stderr**.
Same shape as the `timetrial_ghost_read` and `fileselect_render` overflows
(HANDOFF.md §3). See the comment at that declaration in `racer.c`. Positive
control: restore `f32 sp60[4]` and every plane row fails with `exited 134`.

Two tracks narrow the mask further, but only at 2+ players (`menu.c`, `VERSION >=
VERSION_79`): Spaceport Alpha (15) drops the hovercraft, Frosty Village (28) drops
the plane. This is a one-player sweep, so both are swept in all three vehicles;
`--two-player-mask` applies the 2P narrowing instead.

## Two-player split-screen — `race_2p_split.txt` + `tests/check_race_2p_split.py`

```bash
MDKR_AUDIO=0 python3 tests/check_race_2p_split.py -v                  # ~2.5 min, WebGPU
MDKR_AUDIO=0 python3 tests/check_race_2p_split.py -v --renderer gl    # and again on GL
MDKR_AUDIO=0 python3 tests/check_race_2p_split.py -v --cadence enhanced
```

**Every other fixture is a one-player route, and a one-player build passes all of
them.** This is the only route that reaches `gNumberOfActivePlayers == 2`.

Two things about it are easy to get wrong:

1. **A second player can only join on their own pad.** `charselect_new_player()`
   scans `gMenuButtons[0..3]` for a pad that is not yet in `gActivePlayersArray`,
   so *no* amount of input on port 0 can produce a second player. That is why the
   input-script format grew the `P1..P4` port field (see above). Positive control:
   ignore the port field in `script_apply()` and the check fails with
   `no [PACE2] rows — PLAYER_TWO never published a racer probe`.
2. **The two-player menu graph is a different graph.** In
   `menu_character_select_loop()` the branch is
   `confirmOffset >= gNumberOfActivePlayers` with `confirmOffset == 1` when
   entering char select from the title, so one player goes to
   CAUTION(28)/GAME_SELECT(19) and two players go **straight to
   TRACK_SELECT(15)**. Track select then skips `TRACKMENU_CHOOSE` (the Time-Trial
   toggle — multiplayer forces `set_time_trial_enabled(FALSE)`) and adds the
   "how many CPU racers" stage. Do not copy the one-player tap timings.

The default invocation gates the shipping `SimulationCadence=original` path;
`tools/run_checks.py` also registers a separate `race_2p_split_enhanced` arm for
the opt-in historical one-field mode. The long route deliberately enables the
native autopilot hook so it can finish without physical controllers; its
cadence-specific path measurements are an AI/end-to-end smoke contract, not
evidence about human input routing. Direct human binding belongs to the focused
gate below.

The current fixed-one-field Enhanced reference is 4,719 shared racing frames.
The in-racer pace probes end at P1 checkpoint 53/lap 2 and P2 checkpoint 39/lap
2; max step is 23.3/44.7, max step-to-step change is 1.6/4.0, and the slowest
240-frame mean is 10.70/1.82. P2's slow window is the chaotic AI hook's authored
lane, not evidence of controller starvation. The post-update oracle supplies the
end-to-end result: P1 crosses on lap 3 and takes position 1; the production
N-1-finished rule classifies the still-racing P2 last on lap 2 at position 2.
The check requires those exact finish roles, distinct positions, results at
frame 7632, and track select at 7931. In-memory missing-P2, swapped-position,
and invalid-P2 controls prove the terminal classifier fails in each broken
direction. `check_2p_human_binding.py`, with autopilot off, separately proves
the real P1/P2 controller paths and first-sector response.

Each arm asserts the **exit code** plus the output-overlay ordering contract and
eight independent things: the
`hud_init: hudPlayers=1 numViewports=2` layout line, a two-player
`level_load: ... numPlayers=1`, the existence of the `[PACE2]` player-2 probe,
per-player motion sanity (finite position, y band, no discontinuous teleport,
checkpoint/lap progress, no stall) for **both** players, that the two racers stay
apart in world space (so "the same racer traced twice" cannot pass), the top
and bottom halves of each sampled in-race frame scored **separately**, so a live
viewport cannot mask a dead one, and (since 2026-07-29) the full **2P post-race
classification and flow**: the terminal oracle records P1 first/P2 classified
last, `MENU_RESULTS` loads, and results returns to `MENU_TRACK_SELECT` —
mirroring `check_race_multiplayer.py`'s 3P/4P arms. The
fixture taps A instead of holding it (post-race menu input is edge-triggered),
and motion is judged only while each racer's own race clock advances, because
DKR freezes finished racers for the fade/results sequence.

As in `check_race_drive.py`, teleport detection judges the *shape* of the motion:
`MAX_ACCEL = 40` limits the frame-to-frame change in step length, with a generous
`MAX_STEP = 150` absolute backstop. The former 40-unit absolute ceiling was stale:
on 2026-07-31 player 2 took a real zip pad and ramped smoothly through 40.15,
40.73, ... 44.75 units/frame before ramping down, while the whole run's largest
step-to-step change was only 4.0. The known ASSET_MISC_8 break instead moved
1296.8 units in one frame, so the shape test retains ample separation in the
broken direction.

The fixture's track-preview transition also covers the queued display-list
pointer lifetime repaired by the later all-content census. The original report
called it an optional/unterminated child; the measured command was a valid
`G_DL` to `dRspInit` whose registry entry had been cleared before its queued
task executed. One-generation registry grace fixes the token/segment collision,
and `MDKR_DL_STRICT=1` now passes this route with zero faults.

The deterministic 9,600-frame contract was re-measured on forced WebGPU on
2026-08-08 with the retail vehicle-audio RNG restored (`c6fbd94`). cp, the
slowest-window mean speed, and both-players-traced frame count are all pure
functions of the AI racing line, which moves under any legitimate physics/RNG/
collision change — a per-cadence pin within a percent or two of one recorded
run turns a healthy build into a false "not driving the track" / "stalled" /
"lost player" failure (this happened once already: a `ParticleBehaviour`
stride fix forced `check_race_drive.py`'s own `MIN_FINAL_CP` 20 -> 15). Both
cadences now use `check_race_drive.py`'s calibration norm instead — a floor
set well below the measurement, wide enough to separate "drove multiple laps"
/ "not wedged" / "raced for most of the run" from "went nowhere":
checkpoint `>=25/18` (observed 53/39 original, 53/40 enhanced), `min_both_frames`
`>=1300` original / `>=2900` enhanced (observed 2180 / 4852, ~40% headroom
each), and slowest 240-frame mean `>=12.0/6.0` original / `>=5.0/0.6` enhanced
(observed 25.39/14.59 and 10.78/1.52). `min_final_lap>=2` is unchanged and is
the check's real "both players progressed" gate — it is a monotone lap
counter, not a trajectory-sensitive statistic, so it keeps its tight, honest
floor. Both cadences retain the shared finite-position, track-band, separation,
`MAX_STEP=150`, and `MAX_ACCEL=40` protections. Neither cadence may reach
results without oracle-confirmed `raceFinished=1`, distinct positions P1=1 and
P2=2, P1 lap `>=3`, and P2 lap `>=2`; the three terminal broken-direction
controls must also reject. These autopilot thresholds intentionally say nothing
about physical controller binding.

Positive control for the render half of the check:
```bash
MDKR_AUDIO=0 python3 tests/check_race_2p_split.py --keep-frames /tmp/2p
python3 tests/check_race_2p_split.py --blank-half-control /tmp/2p
```
which flat-fills one half of each real frame and requires the per-half metric to
**reject** it (measured: 1 colour / sigma 0.0). Without that, "both halves scored
fine" is unfalsifiable.

`[PACE2]` is a separate greppable line from `[PACE]`, so the player-1 `[PACE]`
format stays byte-compatible with `check_race_drive.py` and
`check_race_finish_time.py`.

**Per-player in-race input is separately verified by
`check_2p_human_binding.py`.** Autopilot is off. Neutral, P1-only, P2-only, and
both-active scenarios run at Enhanced fixed-one-field cadence across GL/WebGPU
and 60/120 Hz presentation. The gate checks the exact held, pressed, released,
deadzone-adjusted stick, player, racer, port, and update-delta values at the
production racer dispatch boundary. It also requires byte-identical v3 state,
consumed input, binding, and per-clock trajectories across all four
renderer/rate arms. In the measured P2-only arm, P2 reaches checkpoint 5 over a
6,296.6-unit path while P1 remains exactly stationary; the converse P1 arm and
the both-active arm also progress. Drop-P2, swapped-port, and neutral-as-active
controls prove that the gate rejects starvation, cross-wiring, and false motion.

## Three-/four-player split-screen and results — `tests/check_race_multiplayer.py`

```bash
python3 tests/check_race_multiplayer.py --build build -v
python3 tests/check_race_multiplayer.py --build build --players 3
python3 tests/check_race_multiplayer.py --build build --players 4
```

This is not a wider survival-only version of the 2P check. Each arm navigates
the real per-controller frontend, loads Ancient Lake with the exact encoded
player count, and proves all of the following:

1. `hudPlayers=2/3` and `numViewports=3/4` select the intended production
   layout.
2. `[PACE]`, `[PACE2]`, `[PACE3]`, and (for 4P) `[PACE4]` come from distinct
   human racers. Each stream must remain finite, stay in the track's vertical
   band, avoid one-frame teleports and long stalls, and make substantial
   checkpoint/lap progress. Every non-last-place racer must reach lap 3
   (`[PACE] lap=2` before its terminal update).
3. Every racer pair has a nontrivial median world-space separation, so copying
   one valid probe into several slots cannot pass.
4. Every scene quadrant is scored independently. In 3P, the intentionally
   mostly-black fourth quadrant has a separate minimap contract requiring the
   track, racer arrows, and start line instead of pretending it is a fourth
   camera.
5. The race advances through multiplayer `MENU_RESULTS` (menu 17), then the
   default Select Track action returns to `MENU_TRACK_SELECT` (menu 15). The
   input fixtures use spaced A edges; the former long A hold could never
   advance this edge-triggered flow.
6. End-of-update `[ORACLE]` rows prove every human becomes `raceFinished=1` and
   receives each finish position exactly once. DKR intentionally terminates an
   ordinary multiplayer race after N-1 racers finish and classifies the sole
   remainder last; that one DNF may stop at `cp>=30/lap>=1`, while every other
   racer retains the stricter `cp>=40/lap>=2` requirement.
7. The same visual scorer must reject each quadrant after it is flat-filled.
   This positive control runs automatically in both arms.

Measured Debug/WebGPU reference: 3P loads at frame 2361, reaches results at 7632,
and returns to track select at 7931; 4P does so at 2441, 7932, and 8231. On the
2026-07-31 fidelity line, 4P P1/P3/P4 finish normally and P2 is the authored
last-place classification at `cp=33/lap=1`; its end-of-update state is
`fin=1/fpos=4`, and it continues production finish-camera driving through
`cp=34`. The 4P P2 pace probe retains 5,225 active rows, reaches `cp=33/lap=1`,
and has a slowest 240-frame mean of 1.28; the tightened gate requires 4,500
rows, `cp>=32`, and mean speed >=1.0. All other human streams remain well above
those floors. Scene quadrants measure 1003–2603 quantized colours and
sigma 24.9–58.7; the 3P minimap measures 85–126 colours, sigma 33.1–33.9, and
5.6–5.8% nonblack coverage.

The fixture validates scripted local pads and the production renderer/HUD/menu
paths. It does not claim a pixel oracle against retail hardware or physical
controller hotplug behavior. Battle/challenge modes are outside this ordinary
race fixture, but their authored one-human/three-AI outcomes are now closed by
`check_challenge_modes.py`; Taj's distinct two-racer path is closed by
`check_taj_challenges.py`.

## Determinism check — `tests/check_determinism.py` (RUN THIS AFTER ANY PORT-LAYER CHANGE)

Every frame comparison in this project — the ares oracle scoring, GL-vs-WebGPU
parity, native-vs-wasm, and every "dump a PNG and look at it" verification —
assumes a headless run is **reproducible**. Until the `osGetCount()` fix it was
not, and nothing caught it: a non-reproducible renderer still exits 0 on every
fixture.

```bash
python3 tests/check_determinism.py                    # 3 fixtures x GL/WebGPU x 2 runs
python3 tests/check_determinism.py --runs 3 --every 200 -v
python3 tests/check_determinism.py --renderer webgpu  # focused backend rerun
```
It strips all `MDKR*`/`GE007_*` caller settings, explicitly runs both native
backends, and requires every dumped frame to be byte-identical across N runs.
Measured before the fix, `nav_to_character_select` frame 1450 over 10
runs: **10 distinct images, 45/45 pairs differing, median 18.9 % of pixels**
(minimum 4.7 %). Verified to FAIL on a build with the fix reverted.

**If you touch the pacer, the counter, or anything the game reads as time, run
this.** The failure mode is silent: no crash, no assertion, every fixture green,
and every visual comparison quietly meaningless.

## Previous-release parity — `tests/check_previous_release_parity.py`

Every other determinism gate compares a build with itself, so none of them can
see a release change how the retail game plays. This one runs a build of the
previous published release (`--reference-build`, v1.7.0) and this build through
the same retail routes and requires their `[SIMHASH]` v3 streams to be
byte-identical for 6,600 ticks, at least 3,000 of them racing: Ancient Lake and
Whale Bay (two worlds, car and hovercraft), Tricky's boss race, and all four
challenge arenas (Fire Mountain has no AI nodes; the other three are where the
AI plans routes through the node graph), on the US ROM and on the PAL ROM when
`--roms` holds one. Each arm has its own save directory, preferences and working directory
(no content packs) and runs at Original cadence. A red route prints the first
divergent tick with both rows; bisect it over `v1.7.0..HEAD` and decide whether
the change is intended. A `MDKR_RNGSEED=legacy` arm must diverge, the reference
must be a clean checkout of the tag configured like this build, and the old
build writes a Time Trial record and Controller Pak ghost that this build must
decode, re-encode and play back. Without `--reference-build` the gate exits 77
and `tools/run_checks.py` reports it as SKIPPED, never as passed and never as a
complete suite; a release run (`--require-fresh` or `--require-shipping-sdl`)
fails on the skip and names the missing flag. Pure helpers
are unit-tested by `test_previous_release_parity.py`; the stream capture and
comparator are shared with the cross-architecture gate in `simhash_streams.py`.

```bash
python3 tests/check_previous_release_parity.py --build build-rel \
  --reference-build ../mdkr64-v170-ref/build-rel --rom baserom.us.v80.z64 \
  --roms /path/to/rom-revisions --jobs 4
```

## Challenge-arena route search — `ainode_route` (ctest)

The CPU racers in Smokey Castle, Darkwater Beach and Icicle Pyramid choose
their next AI node with `func_8001CD28`. The original search keeps no closed
set: a node that left the frontier is added again when a neighbour is expanded,
which is how it finds a target on the racer's own node and turns back through
the node it came from. 1.8 briefly searched each node once, and those arenas
played differently from 1.7.0 and the N64 (the previous-release gate diverged at
Smokey Castle tick 3115, Darkwater Beach 3133 and Icicle Pyramid 3781). The
ROM-free `ainode_route` unit links the production `objects.c` and runs the
v1.7.0 body, copied verbatim, beside it over named graphs (the in-binary
self-test's triangle with the target on the start node, a ring with the target
behind the racer, tags on the start node, a dead-end chain) and 360 random
graphs, about 17,500 searches: the next node and the direction written must
agree every time, and the run must include searches that found their target,
emptied their frontier and reached the budget. The native search stops after
`MDKR_AI_ROUTE_POP_BUDGET` pops (128 x 128), which only a search the original
would never finish reaches: run over every start, previous node and node or tag
target of the three arenas, the original always finished within 234 pops. The
same unit pins the AI-node rebuild guards by behaviour (duplicate IDs, stale and
out-of-domain links, over-budget costs) and the search's stale-link checks.

## Time Trial record bytes — `save_records` (ctest)

Each 192-byte Time Trial record block holds a checksum and one (time, initials)
pair per retail (course, vehicle): 190 bytes. The two bytes after the last pair,
and the Controller Pak times file past its two blocks, are not the codec's, and
the writers used to encode into a fresh memory-pool block, so a save carried
whatever the pool held there (and summed it into the checksum): 1.7.0 wrote
`0a 00`/`c3 00` on one route and `ff 00`/`4c ff` on another. The writers now
zero their buffers first. The ROM-free `save_records` unit links the production
`save_data.c` with a pool that hands out blocks full of `0x00`, `0xA5` or
`0x5A`, and requires the EEPROM record blocks and the Controller Pak times file
to be identical across the three and zero past the codec's bytes. A block
written the old way (residue in the tail, checksum over it) must still load with
every record, and every progress and record codec must size itself from the
retail save inventory, never the live level table: appended courses cannot
change the encoded bytes or be written by a decode (mutating any one of the
four codecs to the live table turns it red). The 1.7.0 save itself stays pinned
by `save_v170_fixture` and `save_v170_compat`.

## Saves written by 1.7.0 — `tests/check_save_v170_compat.py`

`tests/fixtures/save_v170/` holds the EEPROM the published 1.7.0 build wrote
after one Adventure door and one race from a new game, as annotated hex and as
1.7.0's own `.mdkr-save` export, plus 1.7.0's `mdkr-save inspect` report of it
(its README records how they were made and why they carry no ROM-derived
bytes). The ROM-free ctests `save_v170_fixture` and `save_v170_inspect` hold
`platform/save_codec.c`, `platform/save_container.c` and this tree's
`mdkr-save` to it: the same facts, byte-identical field re-encode, and 1.7.0's
container reproduced byte for byte. This check does the same through the game's
own codec in `game/src/save_data.c`, which is what a player's save actually
goes through: `MDKR_SAVE_COMPAT_FIXTURE` hands it the image after level-table
init, and every fact 1.7.0 reported must come back, with Ancient Lake cleared at
save index 3 and the slot re-encoded byte for byte. A copy with the slot shifted
one bit (checksum resealed) must be rejected, which is what proves the
comparison can see a layout change; a checksum-valid settings word must
round-trip and a wrong one must be refused. It runs in the native and ASan
lanes.

## Authored state/RNG compatibility — `tests/check_authored_rng_compat.py`

This gate is intentionally narrower and stricter than the general determinism
suite: it records the raw all-racer stream of the `race_state_oracle` route,
one arm per cadence. The **original** arm is the ROM-compatibility pin — the
authored two-field cadence, 4,800 frames, 27,840 rows; the **enhanced** arm
(added 2026-09-03, below) is the route's own enhanced arm at 9,500 frames and
54,880 rows. Each arm's rows — positions, velocities, race progress, logical
delta, and the shared authored RNG — must have the raw SHA-256 pinned for that
arm in `tests/check_authored_rng_compat.py`; the digests live only there, with
their rebaseline history, so this page cannot drift out of step with them. Run
one arm with `--arm original` / `--arm enhanced`, both by default.

Each arm also pins **how many times each generator was stepped** — the
authoritative and presentation draw counts, read from the run's own
`[RNGDRAWS]` line. That is not a restatement of the digest, and the reason is
worth stating plainly:

> `rand_range()` is not injective. From the boot seed `0x5141564D` it enters a
> cycle of **period 20** after 11 draws, and `gCurrentRNGSeed` thereafter only
> ever takes 20 values. Two builds whose authoritative draw counts differ by a
> multiple of 20 record byte-identical streams. Measured on this tree: burning
> 20 extra authoritative draws leaves **both** arms' digests exactly as pinned
> and every other stream oracle in the tree green.

So a digest that did not move is not evidence that a stream did not move — it
is evidence about the draw-count delta modulo 20. The counts have no such blind
spot; they move one for one, and they are what actually holds the
cadence-conditional redirects, because a draw count is precisely what those
change.

Four controls keep the gate from passing for the wrong reason:

1. it flips one RNG bit in the first row of each arm and fails unless its own
   validator rejects that mutation;
2. it re-reads `frames`, `cadence` and `synth_fields` for each arm out of
   `tools/oracle_routes/race_state_oracle.json` and fails when the pin and the
   route disagree, so a digest cannot quietly come to describe a different
   route — with an in-process control that feeds it a deliberately wrong
   `synth_fields` and requires the rejection;
3. it drives `MDKR_TEST_AUTH_RNG_BURN=20` — one full turn of the cycle — and
   requires the digest to be **unchanged** and the draw-count golden to
   **fail**. If that control ever stops failing, the counts have stopped being
   an independent observable;
4. it fails when the two arms produce the *same* digest, which would mean one
   of them did not take the cadence it asked for.

### The enhanced arm is the first pin of that stream

Until this arm landed, every gate that recorded an RNG stream recorded the
original cadence: this oracle, `check_weather_rng_order.py`'s
`EXPECTED_ORIGINAL_SHA256`, and `check_state_hash.py`, which sets no cadence and
takes the original default. The second compatibility target named at
`platform/math_util_native.c` — "the pre-FPS native gameplay stream at opt-in
enhanced cadence" — was therefore held by nothing, and any change to it would
have moved silently. `docs/ref/presentation-rng-census.md` names that gap as the
reason it declined to redirect its eight latent presentation-output callers.

The enhanced digest was expected to move **exactly once**: in the commit that
redirects those eight callers through `cadence_compat_rand_range()`. That
happened on 2026-09-03 (`64bf3d28` → `c2ac09ae`), and the superseded
pre-redirect value is kept named in the file so a bisect that lands on it says
so. From here a change to the enhanced digest is a regression.

The original-cadence digest did not move, and that is worth being precise
about: it *could not have*. `cadence_compat_rand_range()` branches on
`platform_sim_tick_fields()`, a per-run constant, so at two fields every
redirected site calls `rand_range()` down the identical path. The original arm
is a guard against writing a redirect unconditionally by mistake, not evidence
that a redirected value is presentation-only — that classification rests on
reading each consumer, and is recorded in the census.

The reference is clean commit
`64936e36b4c9ef7ecdce5beb93cd662d4318548d`. This deliberately replaces the
pre-vehicle-audio hash. The old port skipped `racer_sound_car()` for ordinary
cars, so it also skipped the retail routine's shared `rand_range()` draws and
froze the wrong world/RNG stream.

That authority decision was not inferred from the changed hash. The pinned,
instrumented ares ran the owned US 1.1 ROM through the same regular-car route
and trapped the real `rand_range` entry at `0x8006FB8C` only when its return
address was inside retail `racer_sound_car` (`0x80005254..0x80005D08`). It
recorded 4,801–4,802 player-zero, vehicle-zero calls (the presented-frame exit
can admit one terminal first roll): 2,926–2,927 from the first jitter roll and
1,875 from the conditional second roll, all over `[0,10]`. Two independent runs
had the same normalized first-3,500-call SHA-256
`9fd7cb9aebc163b00f9c8e4bfd292f90b684b4d46415ab5e0ef594c8bfb2d16e`.
This command regenerates and validates that witness; the CSV is ROM-derived and
must never be committed:

```bash
tools/prepare_ares_oracle.sh
tools/run_oracle.sh race_state_oracle --skip-native \
  --vehicle-rng-trace /tmp/mdkr-ares-car-rng.csv \
  --rom /path/to/owned-us-v11.z64
```

The native digest was independently reproduced from the detached accepted
commit with this method (use an empty path for the worktree and your own
supported ROM):

```bash
set -o pipefail
git worktree add --detach /tmp/mdkr-authored-reference \
  64936e36b4c9ef7ecdce5beb93cd662d4318548d
cmake -S /tmp/mdkr-authored-reference \
  -B /tmp/mdkr-authored-reference/build-reference \
  -DCMAKE_BUILD_TYPE=Release -DMDKR_WEBGPU_BACKEND=OFF -DBUILD_TESTING=OFF
cmake --build /tmp/mdkr-authored-reference/build-reference -j
cd /tmp/mdkr-authored-reference
env -i PATH="$PATH" LC_ALL=C MDKR_AUDIO=0 \
  MDKR_SIMULATION_CADENCE=original MDKR_SYNTH_FIELDS=2 MDKR_TRACE=1 \
  MDKR_ORACLE_STATE=1 MDKR_RENDERER=gl \
  MDKR_SAVE_DIR=/tmp/mdkr-authored-reference/reference-save \
  ./build-reference/mdkr64 --headless-frames 4800 \
  --input-script <(python3 tools/dkr_oracle_route.py native-script \
    race_state_oracle --arm original) --rom /path/to/owned-us-v11.z64 2>&1 | \
  python3 -c "import hashlib,sys; r=[x for x in sys.stdin.buffer if b'[ORACLE]' in x]; print(len(r), hashlib.sha256(b''.join(r)).hexdigest())"
cd -
git worktree remove --force /tmp/mdkr-authored-reference
```

That command independently reproduced exactly 27,840 rows and the pinned
digest on 2026-08-03. The raw trace is ROM-derived and must never be committed.
Current native route compilation emits positive input entries one ticket early:
the fixed-ticket host publishes script entry N to the simulation sample traced
at N+1, while route JSON frames name the intended authored present/tick phase.
This phase conversion preserves the exact accepted digest above; removing it
produces 27,832 rows and is the gate's required timing-regression direction.

## Audio output — `tests/check_audio_output.py` (RUN THIS AFTER ANY CHANGE UNDER `game/src/audio*`, `platform/audio_*.c`, `platform/mixer*` OR `platform/audi_port_dkr.c`)

The only check that asserts on **sound**. Until it landed, `README.md` and
`CHANGELOG.md` both listed audio as working and nothing in `tests/` looked at it —
every fixture runs muted by the hard rule above, so the claim rested on somebody
having listened to it.

**It opens no audio device.** `--headless-frames` returns before SDL audio is touched
and `MDKR_AUDIO=0` is set on top; synthesis still runs headlessly, and
`MDKR_AUDIO_DUMP` taps the PCM to a file while `MDKR_AUDIO_RMS=1` prints the mixer's
own RMS / peak / main-bus-clip / fx-guard accounting plus a per-service music trace. The
captures are **ROM-derived**: they go to a temp dir that is deleted unless
`--keep-audio` is passed, and a `.wav` must never be committed (`.gitignore` covers
`*.wav`).

```bash
MDKR_AUDIO=0 python3 tests/check_audio_output.py            # ~12 s, 2 runs x 4300 frames
MDKR_AUDIO=0 python3 tests/check_audio_output.py --control dry-vs-dry   # must exit 1
MDKR_AUDIO=0 python3 tests/check_audio_output.py --control boot-only    # must exit 1
```

Seven assertions over `race_drive_time_trial.txt` at 4300 frames, which crosses boot
silence, the attract intro, the title, character select, track select and a level load
into a race — six music sequences at five different tempos, with engine SFX on top:

| # | asserted | measured |
|---|---|---|
| 1 | 22050 Hz / 2 ch / 16-bit, and 736 sample-frames per pump == 2 NTSC fields | 33.379 ms vs 33.367 ms |
| 2 | not silent, and silent where it should be | whole rms 6580, windows 2372-8908, pre-boot rms 65 with 99.61 % exact zeros |
| 3 | not saturated | rail 0.00174 %, main-bus clip 0.01084 %, worst overshoot 1.29 dBFS |
| 4 | both channels alive, not a mono duplicate | L 6594 / R 6567, rms(L-R) 2864, corr +0.9053 |
| 5 | the music changes across transitions | 8-band spectral L1 0.525-1.145 over seven adjacent pairs |
| 6 | the beat grid sits at the tempo the sequencer programmed | argmax 0.9981x-1.0041x of the live quarter note on three windows; +-5 % detunings score negative |
| 7 | the reverb path contributes | 5.10 % of the dry mix is bit-exactly zero; the wet mix carries rms 100.9 there; fx-guard 0 |

Assertion 6 is the one that separates "the synthesiser is running" from "the
synthesiser is running at the wrong rate" — the class of bug behind the "crazy fast
dance" report. Its reference is `alCSPGetTempo()`, read out of the sequence player
each frame, **not** `music_tempo()`: `music_tempo()` reports only the BPM `audio.c`
last programmed from `gSeqSoundTable`, and it goes stale on any sequence carrying its
own `AL_MIDI_META_TEMPO` events. The attract intro (sequence 35) walks 130 -> 140 ->
120 BPM while `music_tempo()` still says 110.

**Note what is deliberately NOT asserted: peak below full scale.** The peak IS full
scale on healthy audio (DKR's mix touches the rail; the RSP mixer saturates on
hardware too), so that assertion would fail on a correct build. What separates healthy
from destroyed is the *fraction* at the rail and the pre-clamp overshoot, which is
what assertion 3 measures.

The check is proven in both directions. Five signal-level controls run on every
invocation — the analysis is re-applied to corrupted copies of the real capture
(zeroed, `R := L`, x6 gain, one block repeated, resampled by 1.12x) and the check
**fails if any of them passes**. The two `--control` arms above are engine-level.

Not covered, and needing a device or a human ear: that anything is audible at all
(no device is opened); timbre, tuning and instrument assignment (a wrong bank gives a
healthy RMS and a healthy beat grid); whether the 0.011 % of clamped samples is
audible; and realtime pacing, since headless synthesis runs on a fixed cadence so
underruns and DAC drift are invisible here.

## Final-lap music — `tests/check_final_lap_music.py`

```bash
python3 tests/check_final_lap_music.py --build build \
  --rom baserom.us.v80.z64
```

This finishes a real three-lap Time Trial with the closed-loop racer and proves
that the final-lap request crosses the complete native audio path. It requires
lap three, the same sequence changing from 126 to 141 BPM, a matching live
sequencer change from 476160 to about 425472 microseconds per quarter note, and
a beat grid at the new tempo in the captured PCM. The WAV is ROM-derived and
exists only in the test's temporary directory; no audio device is opened.

This deliberately checks both `music_tempo()` and `alCSPGetTempo()`. Updating
only the former makes the UI/debug value say 141 BPM while the sequencer keeps
playing at 126 BPM. Its broken-direction control assigns the old live tempo to
the final-lap PCM window and requires the beat-grid oracle to reject it.

## Accepted SDL sink evidence — `tests/check_audio_sink_evidence.py`

This short real-ROM engine route is the native bridge between deterministic
pre-sink PCM and the SDL application queue. It uses the test-only
`MDKR_TEST_HEADLESS_AUDIO=1` opt-in together with `SDL_AUDIODRIVER=dummy`, then
sets `MDKR_AUDIO_SINK_DUMP` and requires a valid, nonempty 22050 Hz stereo s16 WAV
whose payload and telemetry exactly match the blocks the native sink accepted
into its output ring.
The route must have zero rejected, dropped, or recovery-repaired blocks. It also runs
a control proving ordinary headless mode remains sinkless when the opt-in is absent.

```bash
python3 tests/check_audio_sink_evidence.py
python3 tests/check_audio_sink_evidence.py --control-no-opt-in
```

The dummy driver proves queue acceptance only. It does not qualify the device mixer,
DAC, speakers, hotplug, latency, or audible output; those remain manual physical
release-matrix work.

The deterministic capture now follows the same source-time contract as the fixed
simulation clock: original NTSC advances two source fields per game pass and emits
one 736-sample audio quantum, while enhanced one-field simulation services audio
only every second pass. `check_fixed_tick_schedules.py` additionally requires the
PCM to remain byte-identical under 3–6-field catch-up and suspension rebases;
`check_arbitrary_presentation_rates.py` requires the same identity across native
30–240 Hz, uncapped-like, and PAL-60 presentation schedules, plus Enhanced
forced-WebGPU original/30/45/60/120/uncapped schedules. A live device is still
required to qualify SDL or AudioWorklet underrun, backlog, and DAC-drift
behavior.

## Shipped callback/ring sink wiring — `tests/check_audio_resilience.py`

The `audio_resilience` CTest proves the *mechanism* — the lock-free SPSC ring in
`platform/audio_ring.c`, its underflow-concealing crossfade, and a latency
controller driven by a measured drain rather than an elapsed-time estimate — with
no ROM and no device. It cannot see whether the engine still uses any of it. A
revert to `SDL_QueueAudio`, or a deleted
`mdkr_audio_queue_controller_set_device_period()` or
`mdkr_audio_queue_controller_note_drain()` call, leaves every C unit green and
ships broken audio. `tests/check_audio_resilience.py` closes that gap by running
the real binary against the dummy driver and asserting on the four seams that
only exist when the wiring is intact: the device-open banner must report
`mode=callback` with a power-of-two ring of at least 8192 frames; the
`[AUDIO-RING]` shutdown summary must agree with that banner's period and
capacity, must never report an overflow or an evicted frame, and must never see a
callback ask for more than one period; the `[AUDIO-SINK]` controller row must
carry the *negotiated* `deviceperiod=`; and its `stalls=` must be zero, because
the stall guard exists only to bound an estimated drain and a headless run — which
free-runs far ahead of real time — trips it whenever the estimate is back. Frame
conservation ties it together: `pushed=` on the ring line must equal `samples=`
on the `[AUDIO-SERVICE]` line, so the ring is carrying exactly what the
synthesiser made and nothing else.

Underruns and concealments are deliberately *not* asserted here. Headless runs
outpace real time by a wide margin, so the ring stays deep and starvation never
occurs; a zero-underrun assertion would pass for the wrong reason. Starvation and
its crossfade stay with the deterministic CTest. The run needs both
`MDKR_TEST_HEADLESS_AUDIO=1` and `SDL_AUDIODRIVER=dummy` (the opt-in is refused
without the dummy driver) plus `MDKR_AUDIO_SERVICE_TRACE=1` for the controller and
service rows. A control arm re-runs with `MDKR_AUDIO=0`, which keeps synthesis but
opens no host device, and requires the banner and the ring summary to be absent —
proof that the detectors are reading the sink and would notice its removal.

```bash
python3 tests/check_audio_resilience.py --build build-rel --rom baserom.us.v80.z64
python3 tests/check_audio_resilience.py --no-controls
```

## Absolute output level — `tests/check_audio_level_reference.py` (RUN THIS ALONGSIDE `check_audio_output.py`, SAME TRIGGER PATHS)

`check_audio_output.py` is **scale-blind by construction**: it asserts a *floor* on
RMS and a *ceiling* on saturation, and its tempo, stereo and spectral-change
assertions are ratios and correlations that a flat gain leaves exactly unchanged.
`check_raw16_audio.py` compares two arms of the same build. `[EVTQ]` telemetry counts
events; the resource-plateau `voicePeak` law counts voices. So a **systematic loudness
bias** — one wrong shift in a gain stage, an extra `/2`, a master trim added "to stop
the clipping" — passed the entire suite. This check is the one that would not.

**It opens no audio device**, on the same terms as `check_audio_output.py`; the
capture is ROM-derived and deleted unless `--keep-audio` is given.

```bash
MDKR_AUDIO=0 python3 tests/check_audio_level_reference.py               # ~42 s
MDKR_AUDIO=0 python3 tests/check_audio_level_reference.py --control gain+3  # must exit 1
MDKR_AUDIO=0 python3 tests/check_audio_level_reference.py --control gain-3  # must exit 1
MDKR_AUDIO=0 python3 tests/check_audio_level_reference.py \
    --reference build/ares-oracle/console.raw     # opt-in console lane
```

Eight assertions over the same `race_drive_time_trial.txt` 4300-frame route, so a
level move lands in both bodies of evidence:

| # | asserted | measured | tolerance |
|---|---|---|---|
| 1 | 22050 Hz / 2 ch / 16-bit and the exact sample-frame count | 3 164 800 frames = 143.53 s | +-8192 frames |
| 2 | whole-capture RMS | 7457.9 = **-12.857 dBFS** | +-1.0 dB |
| 3 | per-channel RMS | L -12.857 / R -12.856 dBFS | +-1.2 dB |
| 4 | crest factor (peak - RMS) | **12.857 dB** | +-1.0 dB |
| 5 | saturation, WAV side and engine side | rails 0.07248 %, main-bus clip 0.07233 %, worst pre-clamp 35951 = +0.81 dBFS | ceilings |
| 6 | true peak, 4x oversampled | L **+1.002** / R **+1.478 dBFS** | +-2.0 dB |
| 7 | absolute per-band RMS, 8 bands | -17.949 / -19.357 / -21.587 / -21.877 / -22.934 / -24.898 / -27.279 / -32.869 dBFS | +-2.5 dB |
| 8 | per-slice RMS, fifteen 10 s slices | -21.790 … -10.362 dBFS | +-2.0 dB |

Assertion 4 is the one that keeps working when tolerances drift. The program already
touches full scale, so a build that got **louder** cannot raise its peak — it closes
the crest instead. Under the `+3 dB` engine control the sample peak is bit-identical
at 32768 while the crest falls to 10.058 dB.

Assertion 6 exists because sample peak is pinned at the rail and therefore carries no
information; the inter-sample overshoot is what a real reconstruction filter and every
downstream resampler actually produce, and it is measured, not assumed.

**Both directions, and both are engine-level, not analysis-level.**
`MDKR_AUDIO_TEST_GAIN_DB` scales the synthesised PCM inside `dkr_audio_service_tick()` —
before the engine's own RMS accounting, before the dump, before the sink — and
**refuses to act whenever a host output device is open**, so it is file-domain only
and can never make sound. With the variable unset the capture is byte-identical to a
build compiled without the seam (verified by `cmp`).

| arm | whole RMS | crest | rails | verdict |
|---|---|---|---|---|
| reference | -12.857 dBFS | 12.857 dB | 0.07248 % | PASS |
| `--control gain+3` | -10.058 dBFS (+2.799) | 10.058 dB | 1.21170 % | **FAIL, 28 assertions, exit 1** |
| `--control gain-3` | -15.857 dBFS (-3.000) | 12.857 dB | 0.00000 % | **FAIL, 28 assertions, exit 1** |

Four signal-level controls also run on every invocation (+-3.0 and +-1.5 dB applied to
the real capture in memory); the check **fails if any of them passes**. The +-1.5 dB
pair is there so the band cannot quietly become a rubber ruler that only catches gross
errors.

**The console lane (`--reference`) is opt-in and cannot be committed.** It compares
against the real ROM's own synthesiser output, captured from the audio-interface DMA
stream inside the instrumented ares of [`docs/ORACLE.md`](../docs/ORACLE.md)
(`MDKR64_ARES_AUDIO_DUMP`). When it is given, the port arm is re-run on the *same*
oracle route the console capture used, then envelope-aligned and compared. Measured on
`race_state_oracle`: **port/console +0.016 dB** over a 153.16 s aligned overlap. Full
numbers and the method's limits are in
[`docs/open-items/audio.md`](../docs/open-items/audio.md).

## Cave SFX reverb bus — `tests/check_cave_reverb_bus.py` (RUN THIS AFTER ANY AUX-BUS, `alSynNew`, `alFxNew`, OR SFX FX-ROUTING CHANGE)

DKR runs **two** reverb buses (`game/src/audio.c`): bus 0 = `AL_FX_CUSTOM` (the
ROM music reverb, which every CSP voice keeps) and bus 1 = `AL_FX_BIGROOM` (the
fixed libaudio big-room reverb every SFX voice is re-parented onto for the
cave/tunnel echo). Every other audio gate drives the outdoor Ancient Lake
oracle, which carries **no reverb lines**, so SFX wet ~= 0 there and the bus SFX
land on is invisible — which is why issue #49 (Treasure Caves echo turning into
attenuation) survived the whole suite.

This is that missing oracle. It loads Treasure Caves directly
(`MDKR_LOAD_TRACK=30`, the retarget hook `check_track_sweep.py` uses) and drives
it with DKR's own AI (`MDKR_AUTOPILOT=1`) so the racer moves under the cave
ceiling where `audspat_calculate_echo` ramps the SFX reverb send up. The
clean-room synth writes a per-voice reverb-routing line to
`MDKR_AUDIO_VOICE_TRACE_JSONL` each time a voice's FX mix is set: the aux bus its
envmixer is currently a source of, the wet send (fxMix 0..127), and how many aux
buses the synth built.

```bash
MDKR_AUDIO=0 python3 tests/check_cave_reverb_bus.py --build build \
    --rom baserom.us.v80.z64
```

It asserts (1) the synth built **two** aux buses (`max_aux == 2`), (2) Treasure
Caves loaded as a race and the racer made forward progress (anti-vacuity for
"mid-cave"), and (3) at least a margin of `fxmix>0` SFX voices reached bus 1.
Before the fix the port built one aux bus, `max_aux == 1`, and every `fxmix>0`
line — including the cave SFX — is on bus 0, so assertions 1 and 3 both fail;
that is the RED state the fix turns GREEN. The trace still carries the bus-0
music wet lines, so a build that lost reverb entirely fails the anti-vacuity
rather than passing.

## Music voices stay off the big-room bus — `tests/check_music_bus_isolation.py` (RUN THIS AFTER ANY AUX-BUS, VOICE-ALLOC, OR CSP NOTE-ON ROUTING CHANGE)

The companion to `check_cave_reverb_bus.py`. That gate proves SFX *reach* bus 1;
this one proves music *never does*. Physical voices are a shared pool
(`alSynAllocVoice`, `platform/audio_synth.c`), and the allocator sets only the
logical voice's `fxBus` — it never moves the physical voice's aux bus. SFX
re-parent their physical voice to bus 1 (`AL_FX_BIGROOM`) and never move it back,
so a slot an SFX used stays pinned to the big-room reverb. Stock DKR
(`csplayer.c`) re-parents every music voice to bus 0 (`AL_FX_CUSTOM`) on note-on,
right after a successful `alSynAllocVoice`; the port dropped that step, and
`maxAuxBusses == 1` masked the leak until issue #49 built the real second bus.
When a music note then reuses an SFX-vacated voice, its reverb send follows the
voice onto bus 1 and the music leaks into the SFX echo.

This is the missing music-side oracle. It loads Treasure Caves
(`MDKR_LOAD_TRACK=30`) and drives it with DKR's own AI (`MDKR_AUTOPILOT=1`) — a
race that plays music over dense engine SFX, so the 40-voice pool is recycled
between the two owners and music note-ons routinely land on slots an SFX just
vacated on bus 1. The clean-room synth writes a per-voice routing line to
`MDKR_AUDIO_VOICE_TRACE_JSONL`: an `"fxmix"` line whenever the SFX player sets a
voice's FX mix (as `check_cave_reverb_bus.py` reads) and a new `"music"` line at
every CSP note-on start (`alSynStartVoiceParams`, a path the SFX player never
uses), each carrying the aux bus the voice is a source of and how many buses the
synth built.

```bash
MDKR_AUDIO=0 python3 tests/check_music_bus_isolation.py --build build \
    --rom baserom.us.v80.z64
```

It asserts (1) the synth built **two** aux buses (`max_aux == 2`), (2) Treasure
Caves loaded as a race with forward progress, (3) SFX actually reached bus 1 and
(4) music actually played — both anti-vacuity, since the leak needs SFX-parked
voices *and* music note-ons — and (5) the oracle: **zero** music note-ons are on
bus 1. Before the fix a Treasure Caves drive puts most music note-ons (measured
5209 of 6252) on bus 1 once SFX have moved pool voices there, so assertion 5
fails — the RED state. Restoring the stock per-note re-parent to bus 0 turns it
GREEN (every music line on bus 0).

## RAW16 byte order and timbre — `tests/check_raw16_audio.py` (RUN THIS AFTER ANY RAW16, MIXER LOAD, ENDIAN-HELPER, OR AUDIO-BANK CHANGE)

`check_audio_output.py` passed on the broken build: a byte-reversed instrument can
still have healthy RMS, stereo, tempo, clipping, and reverb. This focused gate
checks the format boundary that the broad signal test cannot hear.

```bash
MDKR_AUDIO=0 python3 tests/check_raw16_audio.py --build build
MDKR_AUDIO=0 python3 tests/check_raw16_audio.py --build build-rel
MDKR_AUDIO=0 python3 tests/check_raw16_audio.py --build build-asan
```

It has three independent assertions:

1. Source classification requires exactly the three `alRaw16Pull()` loads to use
   the converted path and leaves `_decodeChunk()`'s one ADPCM byte-stream load
   ordinary.
2. A bounds-checked independent parser inventories the US/PAL v80 banks:
   233 unique music waves (208 ADPCM + **25 RAW16**) and 444 unique SFX waves
   (443 ADPCM + **1 RAW16**). The principal 18,432-byte bass sample is 27.34x
   rougher under the old byte interpretation.
3. One executable runs production `MDKR_RAW16=fixed` and exact
   `MDKR_RAW16=legacy`. Both arms must issue **9,441 loads / 676,240 bytes**,
   remain bit-identical before the first RAW16 output block, then diverge
   strongly afterward. Measured: first difference at sample-frame 1,499,298,
   17.68% changed post-boundary samples, difference RMS 1,280.5, and an **8.51x**
   worst-block legacy roughness ratio.

The legacy arm is load-bearing: if conversion disappears, both outputs become
identical and the check fails. Temporary PCM is ROM-derived and deleted unless
`--keep-audio` is explicitly passed; never commit it.

`tests/test_endian_utils.c` is the ROM-free companion. CTest covers aligned and
misaligned big-endian integer/f32 readers, `GE_SWAP*`, literal byte reversal,
PCM conversion, invalid arguments, and no-write-on-error behavior. A real
big-endian runtime has not been run.

Actual browser reachability is separate: `check_browser_runtime.py` requires the
AudioWorklet route to report `mode=fixed` with nonzero RAW16 loads and bytes.

## Vehicle engine audio — `tests/check_vehicle_audio.py` (RUN THIS AFTER VEHICLE SOUND, ASSET_AUDIO, OR ENGINE-PITCH CHANGES)

The broad PCM gate cannot prove which loop is playing or whether its pitch
tracks throttle and speed. This focused gate independently decodes the selected
`VehicleSoundAsset` row from the retail ROM, drives production car, hovercraft,
and plane routes, and reads the opt-in `MDKR_VEHICLE_AUDIO_TRACE` witness after
the sound handles have been updated.

```bash
MDKR_AUDIO=0 python3 tests/check_vehicle_audio.py --build build
```

It requires each runtime sound ID to equal the ROM's big-endian value, changing
base pitch, and an active main engine handle. The car arm additionally requires
sustained driving intensity, throttle-pitch ramp, and both sides of the
idle/main crossfade. The original defect
fails five independent assertions: sound ID 115 became 29440, the main handle
never opened, and intensity, throttle pitch, and base pitch remained frozen.
`vehicle_audio_contract` is its millisecond, ROM-free CTest companion: it checks
the complete 0x4C mixed-field swap map, byte-field preservation, and
transactional rejection of short records. `runtime_contracts` exhaustively
checks the shared vehicle-model mapping used by both initialization and update.

## Line-particle ribbon orientation — `tests/check_line_particle_orientation.py`

A line particle is a ribbon laid along the emitter's path, spread either side of
it by a half-width offset on **one** of the parent's local axes. Which axis is
authored per particle descriptor (`lineOrientation`: 0 → local X, 1 → Y, 2 → Z),
and a plane's wing contrail is authored 0 — flat across the wings. This check
flies Hot Top Volcano in a plane under `MDKR_AUTOPILOT`, reads the ribbon basis
out of `MDKR_LINE_PARTICLE_TRACE=1`'s `[LINEPART]` rows, and requires every
emitted ribbon to spread along local X.

```bash
MDKR_AUDIO=0 python3 tests/check_line_particle_orientation.py -v   # ~1 min, muted + headless
```

It asserts the **geometry** (the local offset vector actually used to build the
vertices), not the orientation number that selected it, so an orientation value
that stopped reaching the vertex maths cannot pass. The world-space vector is
reported but never asserted — it is the local vector after the plane's own
pitch/roll, which is exactly why the local one is the roll-independent
statement. Two guards keep it honest: an in-process positive control replays the
measured pre-fix ribbon (orientation 1, local `(0, 2, 0)`) and must see it
rejected, and a non-vacuity floor requires at least 100 ribbons, so a route that
stopped emitting trails fails instead of passing silently.

The defect it closes is a bit-order one, not a byte-order one, and byte-swapping
cannot reach it: `lineOrientation` is a 6-bit C bitfield in the descriptor's
`0x0A` halfword, which the N64's compiler packs from the most significant bit
down and a little-endian host packs from the least significant bit up. Read
LSB-first, the four padding bits of the stock value `0x0001` became orientation
1, and every wing contrail in the game was built as a vertical sheet — a
hairline seen edge-on from the chase camera instead of a broad flat band. Read
the derivation next to the `PARTICLE_DESC_*` accessors in `game/src/particles.h`
before adding any new field to a ROM-sourced struct.

## Texture "interlace" check — `tests/check_texture_lineswap.py` (RUN THIS AFTER ANY TEXTURE-DECODE CHANGE)

About **30 %** of every texture DKR uploads is *pre-swizzled*: its odd rows have the
two halves of each 64-bit word exchanged in ROM, because it is loaded with one of
libultra's `...S` macros (`LOADBLOCK` with `dxt = 0`) and the RDP's texture fetch
would have undone the exchange. The HLE has no TMEM, so it has to undo the
exchange itself
(`unswap_row()` in `platform/fast3d/gfx_pc_dkr.c`). Getting it wrong is **silent**:
nothing crashes, every fixture stays green, and the image is "mostly right, just
kind of scrambled" — the minimap becomes a field of dashes, sprites get comb edges.

```bash
MDKR_AUDIO=0 python3 tests/check_texture_lineswap.py -v      # ~2 min, muted + headless
```

It renders `race_drive_long` (3900 frames) **twice from the same binary**, with
`--pure` explicit in both arms — normally and with `MDKR_LINESWAP=off`, which
reproduces the pre-fix decode byte-for-byte — and measures the artifact itself:
at texel-row spacing `step = width/320`,
`parity = mean|I(y)−I(y+step)| / mean|I(y)−I(y+2·step)|`. A clean image scores
below 1, a swizzled one above. Scored over the changed-pixel mask and over the
minimap ROI. The fixed ROI has known texel-row spacing and retains the absolute
broken-arm threshold. The dynamic mask also contains minified 3D surfaces where
one framebuffer row is not one source texel row, so it requires a material
changed-pixel count, clean-arm ceiling, and improvement ratio instead. Measured:
changed mask `off` 1.01–1.58, normal 0.61–0.73 (1.52–2.60x); minimap ROI `off`
1.46–1.58, normal 0.62–0.69 (2.12–2.50x).

Because the broken arm is re-rendered every run, the check **cannot pass
vacuously** — a build that stops un-swizzling makes both arms identical, and the
changed-pixel count, fixed-ROI absolute threshold, and improvement assertions
fail. Verified in both directions
(see docs/OPEN_ITEMS.md "wave lineswap" for the pasted output).

`MDKR_LINESWAP=off` is also the way to A/B the decoder by eye, in the same style as
`MDKR_NEARCLIP=off`. The `[TEX] lineSwappedUploads=N` line printed at headless exit
is how you confirm a route reaches the path at all (0 = the route loads no
pre-swizzled texture, so any assertion on it would be vacuous).

The ROM-free `texture_cache_identity` CTest protects the decoder/cache boundary.
It requires the cache key to include the resolved source-row pitch and loaded
span as well as palette, font derivation, mipmap, and cutout policy, and requires
the lookup and insertion paths to use that complete key. This prevents a
same-address tile reinterpretation or first-use mip policy from silently binding
pixels uploaded for a different material.

## Locked doors block — `tests/check_door_blocks.py` (RUN THIS AFTER ANY CHANGE TO OBJECT-MODEL COLLISION OR `obj_loop_door` / `obj_loop_exit`)

```bash
MDKR_AUDIO=0 python3 tests/check_door_blocks.py -v            # ~6 min, two arms
```

Asserts that a hub door the player has not earned is **physically solid**. Before
wave "objcoll" it was not: `func_80017A18()` — the per-facet object-model collision
test, and the only non-NULL writer of `collisionData->collidedObj` in the game —
linked to a `return 0` stub, so every collision-meshed object was intangible. With
zero balloons the kart drove through a shut door and `obj_loop_exit()` (which has
no door check, by design) warped it into the Dino Domain lobby at frame ~6731 and
Ancient Lake at ~7017.

The route approaches the two hub door leaves on their center line before aiming
at the exit. This is load-bearing: the older diagonal advloop reproducer can
latch the exit from a near-side corner without touching the door mesh at all.

**Why it has two arms.** The failure mode is *silence* — you drive through, nothing
crashes, nothing is logged — so running only the fixed build proves nothing
(CONTRIBUTING.md rule 2). `MDKR_OBJCOLL=legacy` restores the stub's `return 0`, so
one binary drives both sides, the same trick as `MDKR_COLLTEX=legacy`:

| arm | expected |
|---|---|
| default | reaches Timber's Island (levelId 0), **not** 12 or 5; `[OBJCOLL]` hits > 0 |
| `MDKR_OBJCOLL=legacy` | reaches 12 **and** 5; `[OBJCOLL]` hits == 0 |

The fixed arm asserts a negative, which only means something beside the legacy
arm's positive. It also asserts the hub *is* reached and that hits are non-zero, so
"blocked" cannot be satisfied by a route that broke early or never touched a door.

`[OBJCOLL] objectmodel_collision_hits=N` at headless exit is the reachability
number: about 1742 on this centered-door route, **9** on boss track 38, 0 on
tracks 5/32/15. Object
collision is overwhelmingly a hub-world phenomenon, which is why the pre-fix
measurement (taken only on race tracks) made the gap look 100x smaller than it was.

## Object-mesh wedge recovery — `tests/check_object_wedge.py`

```bash
MDKR_AUDIO=0 python3 tests/check_object_wedge.py -v
```

Pins the complementary failure mode: a racer that begins a tick already inside
a one-sided object mesh must be ejected rather than remaining embedded. The
production arm requires embedded-point recovery, post-impact escape and bounded
pitch levelling. A same-binary `MDKR_OBJCOLL=norecover` positive control seeds
the identical contact and must remain embedded, so the production arm cannot
pass merely because the fixture stopped reaching the door. The versioned test
capability is inert in normal play.

## Door numerals stay per door — `tests/check_door_glyphs.py`

```bash
python3 tests/check_door_glyphs.py -v
python3 tests/check_door_glyphs.py --rate 240 -v
python3 tests/check_door_glyphs.py --rate uncapped -v
```

The four ordinary Dino Domain race doors share one cached `ObjectModel` but require
different numbers of balloons: Ancient Lake 1, Fossil Canyon 2, Jungle Falls 3,
and Hot Top Volcano 5. Golden Balloon 1.0.1 moved `obj_door_number()` out of the
per-object display-list build and into a view-dependent fixed-tick traversal.
That function writes `TriangleBatchInfo.texOffset` on the shared model, so the
last admitted door supplied its digit to every visible door. A deterministic
WebGPU/Original capture rendered all three front signs as 5 at frame 6820 and as
2 at frame 6880 even though the four `Object_Door.balloonCount` values remained
1/2/3/5.

This gate enters the real lobby on a fresh Adventure save and records the texture
offset selected at the material/display-list boundary. Both GL and WebGPU must
submit all four differently numbered doors in one frame, keep every authored
door/count pair stable, and choose `balloonCount * 4` independently for each
one-digit atlas entry. The trace also exposes the cached batch's shared stored
offset. At least one stored/required mismatch is mandatory: that counterfactual
proves the test would detect the old shared-state path instead of merely
confirming that the route reached the lobby. In Original presentation mode the gate also
captures frame 6820 from both native renderers and compares the three door faces.
This final-pixel check catches the separate OpenGL sampler-cache defect where a
new texture object could skip wrap/filter setup and repeat its digit across the
wood. It rejects blank scene output, requires three visually distinct central
glyphs, and synthesizes blank, common-glyph, and repeated-sampler fail-red
controls from the live capture. The fixed GL capture agrees with WebGPU within
normal rasterization noise.

The ROM-free `object_material_ownership` CTest protects the corresponding
source boundary. It rejects any native exposure of the legacy mutating door
API, requires door and racer material selection to remain draw-local, and
requires OpenGL's sampler memoization to include texture-object identity. This
keeps the failure class closed even in lanes that cannot run the gameplay ROM.

## Collision grid mask / boss race — `tests/check_collision_gridmask.py` (RUN THIS AFTER ANY COLLISION OR BOSS-FLOW CHANGE)

**The only check that races a BOSS level.** The 20 ids in `check_track_sweep.py`
come from `ASSET_MISC_MAIN_TRACKS_IDS`; the ten boss levels live in a separate ROM
table, `ASSET_MISC_BOSS_TRACKS_IDS` (`tools/dump_misc_asset.py` sub-asset 30) =
**38, 46, 40, 53, 1, 52, 41, 54, 37, 55**, so no earlier sweep touched them.

```bash
MDKR_AUDIO=0 python3 tests/check_collision_gridmask.py -v      # ~4 min
MDKR_AUDIO=0 python3 tests/check_collision_gridmask.py --skip-control --skip-win
```

`compute_grid_overlap_mask()`'s Z-row test compared against a value the clamping
above had already forced, making the Z half of the collision pre-filter a no-op.
That is **silent**: an over-permissive pre-filter still gives correct collisions,
only far too many candidates — until the 500-entry `gCollisionCandidates` list
saturates and `generate_collision_candidates()` **truncates**, at which point the
facets a racer is standing on can be in the discarded tail and the racer falls out
of the level. On Tricky's volcano spiral (levels 38 and 46) it did: the original
level-38 measurement saw the boss lose the ground for 301 frames and the player
for 103, and **the first boss race could not be finished at all**. Full write-up
in `docs/OPEN_ITEMS.md` "wave gridmask".

That sentence records the historical pre-fix/gridmask fixture. The current legal
campaign route is closed separately by `check_first_boss_progression.py`, whose
checkpoint-36 steering recovery physically reaches the narrow finish in both
win and loss arms. The unrecovered stock-AI miss remains its required failing
control.

Four runs from one binary. `MDKR_GRIDMASK=off` restores the pre-fix tautology, so
the broken arm is re-produced on every run and must *exhibit* the defect before the
fixed arm is credited — the check cannot pass vacuously, and it fails closed if
either probe disappears. Reference:

| arm | maxCandidates | truncated | longest ground loss | peak y | `fin=` |
|---|---|---|---|---|---|
| `MDKR_GRIDMASK=off` | 500 (cap) | 36 | 51 (boss) / 2 (human) | 7 | 0 |
| fixed | 270 | 0 | 18 (boss) / 27 (human) | 4868 | 1 |

The integrated fixture is level 46: restoring object-model collision moved the
level-38 autopilot line 2.5 units and it no longer reaches the summit, while level
46 still reproduces the broken grid mask and finishes with production collision.
A racer counts as falling only when its ground-loss run exceeds the **fixed arm's
own ceiling** (`MAX_FIXED_AIRBORNE`) and at least doubles that same racer's
fixed-arm run. Requiring both racers encoded one trajectory; saturation,
truncation, a paired fall, and broken-only failure to finish carry the defect.

Plus a **byte-identity negative control** on Ancient Lake: where nothing truncates
the two arms' 3857 `[PACE]` racer rows must be identical, so the fix provably does
not perturb anything it should not (measured identical on all 30 tracks), and a
**win arm** via `MDKR_BOSS_WIN=1`.

Two new trace probes and two new hooks, all no-ops unless set:

- `[COLL] maxCandidates=N truncated=N cap=500` at headless exit — the mechanism.
- `[GRND] frame=N pi=P ri=R gw=K surf=a,b,c,d xrot=X zrot=Z` per racer per frame.
  **Per racer is load-bearing**: the decisive observation was that the AI boss falls
  through even when the player does not, so a player-only measurement can miss it.
  `xrot`/`zrot` are the tilt — during the fall the roll freezes at 75 and never
  recovers, which is what "it tilts and never tilts back" was.
- `MDKR_GRIDMASK=off|legacy` — A/B the fix (same contract as `MDKR_NEARCLIP=off`).
- `MDKR_BOSS_WIN=1` — write `racer->finishPosition = 1` once the race has finished,
  so the **human wins** with its racing line untouched (platform/mdkr_adventure.c).
  It replaced `MDKR_BOSS_SLOW=1`, which scaled the boss's velocity to 0.15×: because
  DKR's AI paths relative to the field, that moved the *human's* line too, and with
  the ROM-faithful math it moved it off the Fire Mountain summit — 84 frames with all
  four wheels off the ground, no finish, and `racer_boss_finish()` unreachable at any
  budget. Reach an outcome by writing the field the code branches on, not by
  perturbing the physics until the branch happens to be taken.
  A boss race has two racers and `MDKR_AUTOPILOT` drives the human with the boss's
  own AI, so the human always comes second and `racer_boss_finish()`'s
  `finishPosition == 1` arm was unreachable from any headless route — the same
  "unreachable by construction" shape as the Time-Trial record write. With it, the
  boss verdict is asserted **both ways**: `finishPos=2` → cutscene 5 (lose),
  `finishPos=1` → cutscene 4 (win). That is what makes "I came first and it told me
  I lost" falsifiable.

It also asserts the **boss cutscene flow**: on a fresh `save/eeprom.bin` the first
entry must select cutscene 3 (the challenge — Tricky's *"Now I challenge you to a
RACE!"*), not 5 (lose), and no cutscene-5 load may precede the race load. The check
deletes `save/eeprom.bin` before each run and after the last; with a save present
the boss entry is a *revisit*, which plays no challenge cutscene at all and would
make those assertions vacuous.

⚠️ Do **not** add an upper bound on the broken arm's peak y. One was written and
removed: it encodes where the AI line happens to fall off, and any change that
shifts the racing line (e.g. a longer pre-race cutscene) makes the broken arm reach
the summit first and fail the bound while still fully exhibiting the defect. See the
note in `docs/OPEN_ITEMS.md`.

## Collision candidate headroom — `tests/check_collision_headroom.py` (RUN THIS AFTER ANY CHANGE TO `generate_collision_candidates` OR LEVEL GEOMETRY)

The `j >= cap` pre-check guard added at both
insert sites in `generate_collision_candidates()` (wave "boundsweep",
`docs/open-items/collision.md`) stops the out-of-bounds write, but a saturated
500-entry `gCollisionCandidates` list still silently **drops** every candidate
past the cap — the same mechanism that dropped racers through Tricky's volcano
before wave "gridmask". Boss levels 41 and 54 measure 416 of 500, only 84 slots
of margin. This item is instrument-and-gate only: it does **not** raise the
cap — the layout/perf effects of a larger allocation are unmeasured.

```bash
MDKR_AUDIO=0 python3 tests/check_collision_headroom.py -v      # ~3 min
```

Three things, in order:

1. **Guard presence and ORDER** — a static source check that the
   `j >= mdkr_coll_cap(MAX_COLLISION_CANDIDATES)) { goto out; }` text is still
   present at both insert sites in `game/src/hasm/collision.c` (the segment
   insert and the facet insert), *and* that each guard sits above the store it
   guards. If wave "boundsweep"'s fix ever regresses to the ROM's bare
   `j == MAX_COLLISION_CANDIDATES` equality test, this fails before anything
   else runs. Presence alone is not the property: the facet insert once carried
   its pre-check *below* its own store, which lets the segment insert hand it
   `j == cap` and puts one element at index `cap`.
2. **Per-level sweep** — one `MDKR_AUTOPILOT` run per level, all ten boss
   levels (`ASSET_MISC_BOSS_TRACKS_IDS` = 38, 46, 40, 53, 1, 52, 41, 54, 37, 55)
   at 13000 frames plus Ancient Lake (track 5, an ordinary race) at 6500,
   reading the `[COLL] maxCandidates=N truncated=N cap=N` line every route
   already emits (`platform/platform_sdl_min.c`). Fails if `truncated != 0`
   (saturation must never happen in normal play) or if a level's
   `maxCandidates` exceeds a frozen baseline ceiling (measured peak + 16 slots
   of slack) — the regression tripwire for headroom quietly shrinking.
3. **Positive control** — `MDKR_COLLCAP=150` on boss level 41's own route (same
   track, script and frame budget as its row in the sweep, cap alone lowered
   from 500 to 150, well under its natural 416 peak). Asserts the guard still
   holds under a forced boundary (`maxCandidates` never exceeds the lowered
   cap — the fix from step 1 is doing its job, not merely present as text) and
   that the run truncates, then evaluates the SAME "truncated must be 0" rule
   step 2 applies against this forced arm and requires it to report a
   failure — proof the sweep's assertion is not vacuous.
4. **Allocation-lowering control** — `MDKR_COLLALLOC=1 MDKR_COLLCAP=150` on the
   same route. `MDKR_COLLCAP` alone moves the boundary *inside* a full-size
   500-entry allocation, so a store at index `cap` is a real element of a real
   array and nothing reports it — which is why the boundary control ran green
   while the facet insert's guard sat below its store. `MDKR_COLLALLOC=1` sizes
   the allocation to the effective cap plus one canary element and arms the
   canary at index `cap`: the first slot a missing or mis-ordered guard writes,
   and one no correct path can reach. `[COLL] canary=` must be 0 (`-1` means
   unarmed, which is itself a failure in this arm). Verified to discriminate:
   `canary=1` on a build with the facet guard moved back below its store.

Measured baseline (this binary, `MDKR_AUTOPILOT`, one run per level):

| track | frames | peak candidates | truncated | cap | margin |
|---|---|---|---|---|---|
| 38 (Tricky 1) | 13000 | 270 | 0 | 500 | 230 |
| 46 (Tricky 2) | 13000 | 270 | 0 | 500 | 230 |
| 40 | 13000 | 55 | 0 | 500 | 445 |
| 53 | 13000 | 55 | 0 | 500 | 445 |
| 1 | 13000 | 152 | 0 | 500 | 348 |
| 52 | 13000 | 152 | 0 | 500 | 348 |
| **41** | 13000 | **416** | 0 | 500 | **84** |
| **54** | 13000 | **416** | 0 | 500 | **84** |
| 37 | 13000 | 149 | 0 | 500 | 351 |
| 55 | 13000 | 92 | 0 | 500 | 408 |
| 5 (Ancient Lake, ordinary race) | 6500 | 30 | 0 | 500 | 470 |

Levels 41 and 54 remain the tightest margins in the game, unchanged from wave
"boundsweep"'s original measurement — this check exists so that stops being true
silently. `[COLPEAK] candidates new peak N of M` (`platform/stubs_dkr.c`
`mdkr_coll_candidates()`, `MDKR_TRACE`-gated, same pattern as `[EVTQ]`'s
per-queue peak telemetry in `platform/audio_event_queue.c`) prints each time a
run's high-water mark advances, for tracing *when* in a route the peak moves;
the sweep above reads the unconditional `[COLL]` summary line instead, which
needs no `MDKR_TRACE` and is what every other collision check already parses.

## One-shot cutscene latch — `tests/check_key_cutscene_once.py` (RUN THIS AFTER ANY CHANGE TO `cutsceneFlags`, `level_load()` OR `DKR_SHL32`)

```bash
MDKR_AUDIO=0 python3 tests/check_key_cutscene_once.py --build build-rel -v   # ~4 min
```

**This one needs an OPTIMISED build and will not detect its own defect without
one.** Configure it once:

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j8
```

Reported from the browser as *"I collected the Key on the first race, and now the
key animation is playing after EVERY race."* The world-key cutscene's "already
shown" latch is `CUTSCENE_DINO_DOMAIN_KEY << (world + 31)` — a shift count of 32..35,
which is what MIPS `sllv` masking means and what C calls undefined behaviour. clang
folds it to 0 at `-O2` and deletes the load, the test and the store, so the gate
always fires and nothing is ever recorded. **The native build defaults to Debug and
the web build to Release**, so the same source is correct natively and broken in the
browser. See `docs/OPEN_ITEMS.md` wave "keyshift".

The check injects a save with the Dino Domain key collected and drives
`adventure_race_loop.txt` with `check_adventure_race_loop.py`'s closed-loop route.
It asserts the route really ran (6808 in-race rows, 62286.9 units — so nothing else
can pass vacuously), that the key gate is evaluated on both entries to the world
hub, that the latch mask is non-zero, that the second evaluation sees the latch
**set**, that the return leg contains exactly one lobby load rather than two, and
that the latch is present in `save/eeprom.bin` afterwards.

To exercise the control, build the reverted form and assert it fails:

```bash
cmake -S . -B build-relctl -DCMAKE_BUILD_TYPE=Release \
      -DMDKR_EXTRA_C_FLAGS=-DMDKR_SHL32_CONTROL && cmake --build build-relctl -j8
python3 tests/check_key_cutscene_once.py --build build-relctl --expect-fail   # must PASS
```

Measured: 4 of the 6 assertions fail on that build while both coverage assertions
still pass. The same build at `-DCMAKE_BUILD_TYPE=Debug` **passes**, which is the
whole point of the wave.

The class detector is separate and mechanical — UBSan `-fsanitize=shift-exponent`
over routes that cross a world hub, Timber's Island at the Taj balloon threshold, a
water track and a Tracks race. With the fix in place it reports 0 sites; reverted it
reports 6 shift-exponent sites (in `game.c`, `objects.c`, and `waves.c`).
The `waves.c` site is hit by every route, so it is the sentinel that proves the
instrument is alive.

## Boss win verdict — `tests/check_boss_win_verdict.py` (RUN THIS AFTER ANY CHANGE THAT WRITES `courseFlagsPtr` OR `settings->bosses`)

```bash
MDKR_AUDIO=0 python3 tests/check_boss_win_verdict.py -v              # ~3 min
MDKR_AUDIO=0 python3 tests/check_boss_win_verdict.py --break-invariant   # must FAIL
```

`racer_boss_finish()` (`game/src/vehicle_tricky.c`) decides between "play the
win cutscene, award the amulet, write the save" and "just transition back" on
**one bit** — `courseFlagsPtr[courseId] & RACE_CLEARED`. The already-cleared arm
pushes nothing onto the level-property stack, so *no cutscene level is ever
queued*, nothing is awarded and the save is not written; the player is returned to
the world lobby standing at the boss door. That is the ROM's own re-race path and
it is **indistinguishable from a broken first win**.

RACE_CLEARED on a boss course has exactly one legitimate writer: that same
function's win branch, which sets it together with `settings->bosses |= 1 <<
worldId`. Both fields are otherwise written *by index* from unrelated subsystems
(golden balloons, doors and triggers all do `courseFlagsPtr[<index>] |= ...`; the
save packs `bosses` as 12 loose bits), so **one stray index silently kills every
first boss win** — no crash, no log. This check pins that invariant:

| arm | `bosses` / `courseFlags` at the verdict | outcome |
|---|---|---|
| `first` (fresh save) | `0x0` / `0x40001` | cutscene 4 loads @9139, cutscene 5 never loads |
| `cleared` (`MDKR_BOSS_PRECLEARED=38`) | `0x2` / `0x40003` | **no cutscene at all**, overworld @8432 |
| `lose` (`MDKR_BOSS_WIN` unset) | `0x0` / `0x40001` | cutscene **5** loads — the failure animation |

The `cleared` arm is the **positive control**: it reproduces the reported
*"I won the first boss race, got no win cutscene, no amulet, and was dropped back
in front of the boss door"* from an unmodified binary, which is what makes the
`first` arm's invariant assertions falsifiable rather than decorative.
`--break-invariant` runs the regression itself — it pre-clears the boss course in
the `first` arm too — and the check must then fail.

**The other half of the report.** The player wrote *"I was awarded first place,
still got the failure animation"*. Those are two separate wrong presentations —
cutscene 4 missing, and cutscene 5 appearing — and only the first used to be
asserted, so a build that pushed **both** on the same finish passed. The `first`
arm now also requires that cutscene 5 (`level_properties_push(i, 5, -1, 5)`, the
`finishPos != 1` limb) never loads. The `lose` arm is that assertion's
**non-vacuity control**: with `MDKR_BOSS_WIN` unset the autopilot finishes where
it naturally does and DKR takes its own losing limb, so the harness is shown
producing the exact `level_load: levelId=57 … entrance=5 … cutscene=5` line the
negative assertion reads. Without it, "cutscene 5 never loaded" would be a claim
about a log line nothing had ever been seen to emit.

Two hooks, both no-ops unless set (`platform/mdkr_adventure.c`):

- `MDKR_WATCH_COURSEFLAGS=<levelId>` — one `[BOSSW] frame=N courseFlags[L]=0x… (was
  0x…) bosses=0x… (was 0x…) courseId=… worldId=…` line per observed **change**.
  It **polls at the frame boundary** rather than hooking each writer, deliberately:
  that is what makes it catch a write through a *wrong* index. Low bits are the
  race flags (1 visited, 2 cleared, 4 silver coins), bits 16+ are per-level
  balloon/door/trigger flags.
- `MDKR_BOSS_PRECLEARED=<levelId>` — hold that boss course at "already beaten"
  (`RACE_VISITED|RACE_CLEARED` plus the world's `bosses` bit), i.e. the state a
  save carries once the boss has been beaten once. Re-applied every frame because
  FILE SELECT reallocates `Settings` and `clear_game_progress()` would wipe it.

## CPU-opponent stuck recovery — `tests/check_ai_unstick_opponents.py`

```bash
python3 tests/check_ai_unstick_opponents.py --build build -v      # ~2 min, 12 races
python3 tests/check_ai_unstick_opponents.py --self-test           # reducer control, instant
```

`racer_AI_pathing_inputs()` arms a 120-unit cooldown (`unk215`) after every AI
stuck-recovery, and that cooldown decays on exactly one condition —
`unk214 == 0 && racer->velocity < -0.5`, i.e. the reverse window has expired
**and** the kart is moving. Nothing else in `racer.c` reads or writes `unk215`,
so a kart that comes out of the reverse window still unable to move can never
clear the cooldown and can never re-arm the recovery. Measured on the
autopiloted **human** at Hot Top Volcano's crater: 18,677 motionless frames
(`docs/open-items/gameplay.md`). Whether the same wedge is reachable for a
genuine **opponent** decides that item's classification, because
`mdkr_autopilot_unstick()` is reachable only from the human-autopilot limb — a
real CPU racer has no recovery at all.

This gate measures it. `MDKR_AI_STUCK_TRACE=<stride>` (`game/src/racer.c`, inert
unless set) installs an observation-only witness in `update_AI_racer()` — the
limb genuine opponents actually take; `update_player_racer()`'s
`racer_AI_pathing_inputs()` call is inside the *non*-computer branch and is
reached only by a human kart the finish hand-off has relabelled — and it refuses
any racer index ever seen carrying a real player index. It maintains two
per-opponent counters in update-rate units: `stall` (cooldown armed while
`|velocity| < 0.5`, the wedge signature) and `nodecay` (cooldown armed while the
decay branch's own condition is false, which legitimately covers the reverse
window), plus `arm`/`clear` events bracketing every episode.

12 races on Hot Top Volcano, each with a distinct boot RNG seed
(`MDKR_RNGSEED=0x<hex>`, `platform/math_util_native.c` — nothing re-seeds at
boot, so each value is an independent random stream for the whole run; measured
divergence is total within one lap). The check **fails closed**: zero cooldown
episodes across all races means the witness stopped reporting, not that the
game got better.

| window | value | measured legitimate maximum |
|---|---|---|
| `WEDGE_WINDOW` (`stall`) | 900 | 229 update units |
| `NODECAY_WINDOW` (`nodecay`) | 2400 | 729 update units |

Both come from a 32-race, 32-seed measurement pass that produced 23 legitimate
stuck-then-recover episodes on genuine opponents — 23 `arm`, 23 `clear`, none
left armed. The 229 is the shape worth knowing: opponent 6 left the track at
`(-96.1, -987.7, 2357.9)`, all wheels off a surface, velocity pinned at `0.472`
(below the 0.5 the decay branch needs), cooldown at 120 with the reverse window
already expired — the wedge exactly — for 208 update units, and then DKR's own
**out-of-bounds respawn** put it back on the track and the cooldown decayed
`120 → 114 → 84 → 54 → 24 → 0` in six samples of ordinary driving. That respawn
is the opponent's recovery, and it is why the same track wedges the autopilot
(which lands *inside* the world, grounded, so respawn never fires) and not the
field. A genuine deadlock is unbounded, so 4× the legitimate maximum separates
the two without being calibrated on any racing line.

`--self-test` feeds synthetic telemetry in the engine's exact row shape through
the same reducer and requires the wedge to be reported and the measured
229-unit episode not to be. The engine-side witness is proven live by the
episode census a passing run prints.

**If this check ever fails, do not change gameplay to make it pass.**
`racer_AI_pathing_inputs()` is matching decompiled code with no `GLOBAL_ASM` and
no `NON_MATCHING`; preservation-port-versus-fix is an owner decision and the open
item exists to hold it. Report the telemetry.

This gate deliberately does **not** export `MDKR_TRACE` — that variable arms
engine behaviour as well as printing (see the shadow-gate note under
`check_world_shadows.py`), so a measurement taken with it set describes a
different program.

## Bluey 2 rematch — `tests/check_bluey2_rematch.py`

```bash
python3 tests/check_bluey2_rematch.py --build build --rom /path/to/owned-us-v11.z64
```

This is the standing regression for the reported faster Bluey rematch. It
builds a checksum-valid checkpoint with Bluey 1 and the four Snowflake silver
races complete, marks Bluey 2 visited, resumes the file, and traverses the
production hub/lobby route before retargeting the final race load to course 52.
No first-visit cutscene redirect is allowed. Bluey must naturally finish first
at checkpoint 24/lap 2, and the human's production boss verdict must be a loss.

Original two-field simulation is the product contract. Enhanced one-field
simulation is the fail-red control: on the reference build Original finishes at
3,518 logic ticks while Enhanced finishes at 3,019 with 1.1696x mean object
speed. The check requires a materially earlier/faster Enhanced result, then
substitutes the Original result for that control and verifies that its own
sensitivity assertions reject the pair.

The Original arm also dumps headless PCM to its temporary directory. It pins
the authored sequence-30 stop and sequence-57 race-cue start (3.204 seconds
apart, with the race cue 0.968 seconds after GO) while requiring every 250 ms
window around GO to remain active. This distinguishes the original game's cue
transition from an audio-device or mixer dropout. The same arm traces every
compressed-sequence note admission for the music and jingle players. It rejects
any voice-limit loss or physical-voice steal and reports each player's measured
peak occupancy, which directly covers the dense Bluey passage and multi-note
silver-coin jingles from issue #30. Full release/Ares measurements and the
visible-timer explanation are in
[`docs/BLUEY2_PARITY.md`](../docs/BLUEY2_PARITY.md).

## First boss campaign progression — `tests/check_first_boss_progression.py`

```bash
MDKR_AUDIO=0 python3 tests/check_first_boss_progression.py -v
MDKR_AUDIO=0 python3 tests/check_first_boss_progression.py --break-invariant  # must FAIL
```

This is the legal Adventure route the isolated boss verdict check does not cover.
It starts from a checksum-valid checkpoint with three Dino races cleared, three
local balloons, and two actual Timber's Island balloon bits. It then drives the
production chain `Timber's Island -> Dino lobby -> Hot Top Volcano -> Dino lobby
-> Tricky 1`, requiring the fourth race's three laps, the four-local-balloon boss
door, and first-visit challenge channel 3.

Production object collision legitimately nudges Tricky 1's stock human AI line
2.5 units thousands of frames before the summit. The line reaches checkpoint 37
but misses the narrow finish extent. `MDKR_BOSS_ROUTE=1` leaves that whole line
alone through checkpoint 35, then supplies steering only toward a point beyond
the finish. It does not move the kart or write lap, `raceFinished`, verdict, boss
or save state.

The check runs three arms:

| arm | required result |
|---|---|
| win | physical finish first; only then `MDKR_BOSS_WIN` changes place 2 -> 1 with `raceFinished=1`; cutscene 4 |
| loss | the identical recovered route naturally finishes second; cutscene 5 |
| broken-route control | `MDKR_BOSS_WIN=1`, recovery absent: checkpoint 37, no `bossfinish`, no force, no win cutscene |

Both campaign arms return to the Dino hub, write checksum-valid EEPROM, and are
loaded in a second process. Win persists Hot Top and Tricky cleared plus Dino's
boss bit; loss persists Hot Top cleared and Tricky visited only. Both retain
zero TT/Wizpig amulet pieces: the first world boss awards a balloon and boss
clear, while the amulet piece belongs to the second boss.

## Campaign progression — `tests/check_campaign_progression.py`

```bash
MDKR_AUDIO=0 python3 tests/check_campaign_progression.py -v          # ~11 min
MDKR_AUDIO=0 python3 tests/check_campaign_progression.py --quick     # ~50 s
MDKR_AUDIO=0 python3 tests/check_campaign_progression.py --quick --break-invariant  # must FAIL
```

`--break-invariant` drives seam A on the stock AI line instead. Measured, that
line takes 6 of the 8 coins, so nothing is written and six assertions go red —
which is what keeps the silver-coin arm from being satisfied by a race that
merely happened.

This is where the campaign stops being ungated. It picks up from the state the
first-boss check leaves behind and gates each later progression **seam** with a
save fixture entering it and an assertion on what production writes leaving it,
so the fixtures compose into a witnessed campaign. Which courses belong to which
world is read out of the ROM's own level headers, never from the level names —
they disagree, and a fixture that guesses silently exercises nothing.

| Seam | What it drives | What it must write |
|---|---|---|
| A | Ancient Lake entered through the real hub and lobby, all eight silver coins collected by the game's own coin objects | `RACE_CLEARED_SILVER_COINS` written once, status 2 -> 3 in EEPROM, `balloons` (6,4) -> (7,5), reloaded by a second process, no amulet |
| B | all four worlds' second boss races, won, each on the EEPROM the previous one persisted | the world's `1 << (world + 6)` bit, `wizpigAmulet` +1 exactly (1, 2, 3, 4 across the chain), one amulet cutscene, boss course cleared, and no carried bit dropped |
| B control | the same race and the same win from a save whose first boss was never beaten | the FIRST-boss bit, **no** rematch bit, **no** amulet, no amulet cutscene |
| B driven | the Dino Domain rematch entered by driving the human through the lobby's own boss door, not by `MDKR_LOAD_TRACK` | the same state as the retarget arm, plus `WARP_BOSS_REMATCH` live at eight world balloons with `WARP_BOSS_FIRST` disabled, and the lobby's exit actually taken |
| C | the carried four-piece save loaded into Timber's Island, then Wizpig 1 raced | the hub redirected to `WIZPIGMOUTHSEQUENCE` with `CUTSCENE_WIZPIG_FACE` latched and persisted; then Wizpig 1's central-area boss bit and cleared course |
| C control | the same hub load one rematch earlier, at three pieces | **no** redirect and **no** `CUTSCENE_WIZPIG_FACE` |
| T | all four trophy championships, chained on one another's saves through `check_trophy_series.py`'s own driver | `trophies` 0x3 → 0xf → 0x3f → 0xff, each step matching production's own `trophyaward:` line, and nothing else in the save moved |
| T.T. | the four challenge courses won in four separate processes on one another's saves | `ttAmulet` 1, 2, 3, 4, one `TTAMULETSEQUENCE` each, each course cleared |
| T.T. control | the FIRST challenge replayed on the save it produced | **no** piece and **no** cutscene — the award is gated on the course not already being cleared |
| E | Wizpig 2, won, and the post-win cutscene stack driven to the credits | `bosses & 0x20` live and persisted, `MENU_CREDITS` reached, and `menu_credits_init` choosing "TO BE CONTINUED …" + `SEQUENCE_CRESCENT_ISLAND` with `gViewingCreditsFromCheat` zero |

Seams B, C, T, T.T. and E run on **carried** saves: `Slot.from_save()` reads a
real persisted EEPROM back and the next seam runs on it, overriding only the
fields a later gate needs. So `wizpigAmulet == 4`, `trophies == 0xFF` and
`ttAmulet == 4` are each something production wrote four times, not something a
fixture asserted.

The driven seam-B arm's lobby approach is watched by
[`tests/route_plan.py`](route_plan.py) against the run's own `[PACE]` position
stream, so a route that stops closing reports the waypoint, the closest approach
and whether it oscillated, was held off, or ran out of budget — instead of
looping to the frame limit and reporting only that the boss never loaded. It is
not a slow arm: measured, the lobby is entered at frame ~2947 and the rematch at
~3124.

Seam C's redirect is invisible in the level-load stream and needs the
`wizpigface:` trace: `game_load_level` logs the level it was *asked* for, then
the redirect branch pushes the hub, swaps in the mouth sequence,
and pops the hub back — so a redirected hub load and an ordinary one print the
same two `levelId=0` lines. Reading those as "the cutscene never fired" is the
mistake this seam was first reported with.

Seam A needs `MDKR_SILVER_ROUTE=1`. DKR places silver coins off the racing line
on purpose, so `MDKR_AUTOPILOT`'s stock AI sweeps up only 5–6 of 8 (measured on
Ancient Lake over three laps) and the `silverCoinCount >= 8` write can never
happen. Like `MDKR_BOSS_ROUTE`, the hook supplies **steering only**, and only
toward a coin within 1,400 units so the kart keeps making checkpoint progress; it
never touches `silverCoinCount` or the coin objects, so a coin still counts only
when `obj_loop_silvercoin` sees its own `distance < 80`.

The contrast arm runs `credits_via_cheat.txt` and confirms it reaches
`MENU_CREDITS` from a save file that was never started — which is why arriving at
credits is not by itself evidence about the campaign.

**What this deliberately does not prove** is in
[`tests/fixtures/README.md`](fixtures/README.md), which also keeps the measured
obstacle behind every route here. What is left is Future Fun Land's own state —
its four cleared races, its balloons, the four world keys and the world-arrival
cutscene flag are constructed, because this check does not drive to that world.
The door into it is gated separately, below.

## Controller hotplug — `tests/check_input_hotplug.py`

```bash
MDKR_AUDIO=0 python3 tests/check_input_hotplug.py --build build
```

Drives real device add and remove through `SDL_JoystickAttachVirtual`, so it
needs no hardware. Eight independently scored assertions over two arms, watching
`[PAD-CHANNEL]` rows emitted from the same three accessors `osContGetReadData`
reads — so the trace cannot agree with SDL while disagreeing with the game.

**It found a real defect on its first run.** A pad connected *before launch* was
bound to two channels: `SDL_Init` queues a `CONTROLLERDEVICEADDED` for each
existing device, `platform_input_init()` enumerated while that event was still
queued, and `SDL_GameControllerOpen()` is reference-counted — so the second open
stored the same pointer in the next free slot. The game saw a controller in P2
that nobody was holding, the next real pad went to P3, and P2's rumble buzzed
P1's pad. See [`open-items/multiplayer.md`](../docs/open-items/multiplayer.md).

The **boot arm is the point**: mid-run hotplug produces one ADDED event and no
enumeration, so every existing gate missed this. Tick `0` of the test schedule
fires from inside `platform_input_init()` *before* its startup enumeration,
which is the only way to reproduce a pad that was already plugged in.

Negative control: the fix gated behind a runtime flag makes the gate exit 1 with
exactly four failures. Three of those are downstream of the one root cause — the
disconnect fail-safe and the padless-rumble guard were already correct, which
the overflow arm's independent passes show.

## Shell self-voicing — `tests/check_a11y_shell.py`

```bash
MDKR_AUDIO=0 python3 tests/check_a11y_shell.py --build build
```

Walks `Tab` and the arrows through the launcher, every settings section and the
in-game overlay, and requires **every focusable control** to have produced a
`[SPEAK]` utterance carrying its name and current value. The expected set is
enumerated from the app's own schema dump, not from a list here, so a setting
added and never voiced fails this gate with no test edit.

**It found three real defects on its first run**, which is the argument for
enumerating rather than listing:

1. `App.UpdateCheck` and `Tools.Enabled` had **no control at all** — both carry
   the Interface category, but that section was hand-written and drew only two
   rows, so they were reachable by ini and env only. `test_app_ui_policy.cpp`
   asserted they were *visible*, which is a statement about policy, not about
   anything actually being drawn.
2. The three settings with the longest help said **nothing whatsoever** —
   `mdkr_a11y_focus()` correctly refuses over-long text whole rather than
   truncating mid-word, and those paragraphs exceed the buffer. Fixed at the
   emission point by keeping as many whole sentences as fit.
3. The ImGui SDL2 backend drops key events carrying an unrecognised window id.

The positive control has two arms: removing the emission entirely fails naming
all 52 controls, and skipping a single key fails naming that one — so the gate
isolates rather than only detecting all-or-nothing.

The Online Room panel is resolved per BUILD rather than assumed OFF: it reads
`MDKR_ENABLE_ONLINE_BETA` and `MDKR_ENABLE_ONLINE_ROOM_PREVIEW` out of the
build's own `CMakeCache.txt` (`harness_utils.cmake_cache_bool`, mirroring
`tools/run_checks.py`'s helper of the same name) and asserts accordingly: with
either flag ON, opening the panel must announce "Online Room" like any other
panel; with both OFF, the launcher must refuse the request and announce the
Play home it lands on instead. Checking only the PREVIEW cache entry used to
be wrong for a `--allow-online-beta` release bundle
(`macos/Scripts/build_app_bundle.sh`): `MDKR_ENABLE_ONLINE_BETA=ON` forces the
panel in through a non-cache `set()` in `CMakeLists.txt`, so its own
CMakeCache.txt keeps reporting `MDKR_ENABLE_ONLINE_ROOM_PREVIEW=OFF` even
though the panel is compiled in — the gate demanded a refusal from a build
that could not produce one. A self-test (a stub `CMakeCache.txt` reporting
each configuration, including the BETA-on/PREVIEW-off shipping case) runs at
the top of every invocation as the positive control for that detection.

## Race announcements — `tests/check_a11y_race.py`

```bash
MDKR_AUDIO=0 python3 tests/check_a11y_race.py --build build --rom baserom.us.v80.z64
```

Drives a full autopiloted race and requires the `[SPEAK]` stream to carry the
five things a player who cannot see the screen needs in order to follow one: the
start, at least one change of position, the laps, the final lap, and the result.
Lap calls must match `Lap N of M` — a bare "Lap 2" does not say how much race is
left — and the finish must be spoken at `critical` priority, because it is the
one line no later utterance may cut off.

Three arms, each answering a question the others cannot:

| Arm | Asks | Passes when |
|---|---|---|
| full race | is a race followable by ear? | all five moments spoken, laps named `Lap N of M`, the result `critical` |
| purity | did listening change the race? | `[SIMHASH]` v3 rows byte-identical with announcements on and off — 5400 rows on the current route |
| categories | does each switch own its own category? | silencing `race_event`, `race_lap` or `race_position` leaves the other two intact |

The purity arm is the authority proof: announcements are
`MDKR_ENH_PRESENTATION`, so the byte-identical hash stream is what makes that
classification a measured fact rather than an intention.

**The coalescing assertion is the one with teeth.** Position changes in a
mid-pack scrap land about three authoritative ticks apart, and a stream that
spoke all of them would cut every line off with the next one — audible as noise,
not as information. `MDKR_A11Y_RACE_POSITION_MIN_TICKS` (60 ticks,
`platform/a11y_race.h`) debounces them, and the gate measures the tightest gap
between consecutive `cat=race_position` utterances against the `[PACE]` clock,
failing under 50 frames. Asserting 50 rather than 60 leaves the tick-to-frame
mapping a frame or two of slack at the edges while staying an order of magnitude
above what an uncoalesced stream produces. The arm refuses to score itself
vacuous: fewer than two timestamped position calls is a failure naming that
cause, so "the race had no overtaking" and "the spacing was never tested" cannot
be mistaken for a pass.

The route runs under `MDKR_SYNTH_FIELDS=1` for determinism. That is sound here
and would not be for a pacing measurement — the synthetic pacer's presentation
and GPU counters are meaningless, but authoritative ticks, the hash stream and
utterance ordering are not.

## Future Fun Land unlock — `tests/check_future_fun_land.py`

```bash
MDKR_AUDIO=0 python3 tests/check_future_fun_land.py -v               # ~4 min
```

Four gold trophies plus a beaten Wizpig 1 open the lighthouse rocket
(`begin_lighthouse_rocket_cutscene`, `game/src/thread3_main.c`), which is the only
way into the last world. The trophies are **produced**, not stated: the check
imports `check_trophy_series.py`'s championship driver and chains four of them.

Three arms, because a refusal writes nothing at all — no bit, no level load, no
trace — so a single arm would pass with the whole unlock deleted:

| Arm | Save | Must happen |
|---|---|---|
| unlock | four production golds, Wizpig 1 beaten | `ASSET_LEVEL_ROCKETSEQUENCE` once, `ASSET_LEVEL_FUTUREFUNLANDHUB` reached, `CUTSCENE_LIGHTHOUSE_ROCKET` persisted |
| one short | worlds 1–3 gold, world 4 driven to second place (a production **silver**) | the signpost triggered and **nothing** else |
| no Wizpig 1 | the four-gold save with `bosses & 1` cleared | the signpost triggered and **nothing** else |

Every arm asserts the `rocketsign: trigger` line, which is emitted at the
honk/collision and *before* the gate is evaluated. That is what separates "the
route never reached the signpost" from "the signpost read the save and said no",
and it is the only thing that makes the two negative arms mean anything.

## Hand-asm transcription checks (RUN THESE AFTER ANY CHANGE UNDER `game/src/hasm*`)

Three checks guard the bug class described in `HANDOFF.md` §3's sixth shape: **C
bodies that transcribe hand-written assembly, which only this port compiles.**
Upstream's matching build links the real `.s`, so it never executes that C and its
tests can never catch an error in it. Before trusting anything under
`game/src/hasm/`, diff the C against the `.s` next to it. Full findings:
`docs/OPEN_ITEMS.md` wave "hasmaudit".

```bash
MDKR_AUDIO=0 python3 tests/check_math_rotpy.py -v            # ~20 s
MDKR_AUDIO=0 python3 tests/check_math_tables.py -v           # ~5 s, no race
MDKR_AUDIO=0 python3 tests/check_collision_untextured.py -v  # ~2 min
```

**`check_math_rotpy.py`** — `vec3f_rotate_py()` must pair pitch with yaw. The
upstream body transposed them in the x and y results, turning a horizontal
direction vertical (`pitch 0, yaw 90°` → the ROM's `(z,0,0)` became `(0,−z,0)`).
Its strongest assertion needs no golden numbers and no disassembly: `vec3f_rotate`
applied to `(0,0,z)` with zero roll **is** `vec3f_rotate_py` by definition, so the
two must agree — and they only do with the fix. `MDKR_ROTPY=legacy` restores the
transposition from the same binary.

> The fix is **behaviour-neutral for the racer simulation** (0 of 359 `[PACE]` rows
> change), because every call site feeds particles, lights or sprites rather than
> physics. So do **not** try to assert this one with a `[PACE]` diff — it would pass
> vacuously. That is why the probe counts calls and hashes every result instead.

**`check_collision_untextured.py`** — an untextured terrain batch
(`textureIndex == 255`) is collidable with `SURFACE_DEFAULT`; the upstream body
`continue`d and dropped it. **The case is not reached by any shipped level** (0
batches across all 20 main and all 10 boss tracks), so the two normal arms are
asserted *bit-identical* — that is the safety statement and the tripwire. Because
unreached arms cannot distinguish a working fix from no fix,
`MDKR_COLLTEX_FORCE=1` treats every batch as untextured; forced+legacy then
collapses the candidate list to **1** and drops the racer to **y ≈ −568**, while
forced+fixed holds 26 candidates at y 29.

**`check_math_tables.py`** — the `.s` **data** section this port substitutes for.
`platform/math_util_native.c` regenerates `gArcTanTable` and supplies the RNG seeds
because `game/src/hasm/ido/math_util.s` is not assembled. Two divergences are
**still live by default and deliberately deferred**, and this check exists so they
cannot rot and so nobody deletes the opt-in switches without reading why:

- `gArcTanTable` truncates where the ROM rounds — 491 of 1025 entries one unit low.
  `MDKR_ARCTAN=round` reproduces the ROM's table **exactly** (0/1025), which is
  what the check asserts, by hashing the table the binary built against the
  `.half` directives parsed out of the `.s` at run time. **No ROM bytes are
  committed.**
- `gCurrentRNGSeed`/`gPrevRNGSeed` are `0x00051234`/`0`; the ROM boots
  `0x5141564D` for both. `MDKR_RNGSEED=rom` selects the ROM's.

> **Why they are deferred, and what to do when you land them.** Both are reached
> and material (80 and 110 of 359 `[PACE]` rows respectively), so turning either on
> shifts every AI racing line. `check_race_drive`'s open-loop route then strands
> the kart short of a lap, and `check_collision_gridmask`'s **positive control
> stops reproducing its defect**. Re-cut both fixtures in the same change and
> invert the two "still the known divergence" assertions in
> `check_math_tables.py` — which is exactly what its failure message tells you.

## Wave-visibility table layout — `tests/check_wave_visible_table.py` (RUN THIS AFTER ANY `waves.c` OR LINK-FLAG CHANGE)

The only check here that inspects a **built artifact** rather than running the game.
It needs no ROM and starts no process, so it is trivially audio-safe.

```bash
python3 tests/check_wave_visible_table.py                                  # build-web/ or dist/web/
python3 tests/check_wave_visible_table.py --wasm build-web-base/mdkr64_web.wasm -v
```

`waves.c` treats `D_8012A5E8[2]` and `D_8012A600[24]` as **one 26-entry table** —
`waves_visibility()` sentinel-clears all 26 slots and then appends visible wave
blocks through `D_8012A5E8[var_a3]` with `var_a3` measured up to **25**, indexing the
first array straight on into the second. As two separate C objects that only works if
the linker places them adjacently. Mach-O/arm64 does (gap 0). **wasm-ld does not**:
it 16-byte-aligns the 288-byte `D_8012A600`, leaving an **8-byte hole** after the
24-byte `D_8012A5E8`, so slots 2..25 land below the cleared sentinels, the
`blockID != -1` walk runs off the table, and `func_800B92F4()`'s
`D_800E3178[D_8012A5E8[k].unk8]` loads through a garbage index. That is the
`memory access out of bounds` a player hit in the browser — full write-up in
[`../docs/open-items/web.md`](../docs/open-items/web.md#fixed-browser-memory-access-out-of-bounds-in-the-wave-renderer--wave-wavetable).

The check asserts the *shape*, not addresses (those move every link): the 26
sentinel-clearing stores the reset loop emits must form a single arithmetic run with
stride `sizeof(unk8012A5E8)` == 12 and no gap. It **fails closed** — if it cannot
locate those stores it fails rather than passing vacuously, because "found nothing"
cannot tell a fixed build from one whose codegen moved.

Verified in both directions on real artifacts:

| artifact | result |
|---|---|
| fix applied | `run len=26 232680..232980` → **PASS** |
| fix reverted | `run len=2 232688..232700` \| **GAP 8** \| `run len=24 232720..232996` → **FAIL** |
| the wasm behind the player's **first** crash (`63c5d32`, md5 `348a9d80…`) | → **FAIL** |
| the wasm behind the player's **second** crash (`9c643a0`, md5 `a08bbcb0…`) | → **FAIL** |

Both player crashes were the same defect at two different instructions: the first read
past the table (`D_800E3178[D_8012A5E8[k].unk8]`), the second had the runaway walk's own
`unk8` **store** land on `gWaveBlockIDs[8]`, after which `waves_render()` indexed
`gWaveModel` past its array. One check covers both.

### Reproducing the browser layout on native (how the second crash was pinned)

Native is immune to this defect, so the only way to exercise it natively is to *build
the browser's layout*. Replace the union with the shipped module's exact relative
placement and toggle only the hole:

```c
static struct {                 /* offsets measured in the shipped wasm */
    unk8012A5E8   head[2];      /* +0    &D_8012A5E8    */
    unsigned char hole[8];      /* +24   wasm-ld padding -- REMOVE for the control */
    unk8012A5E8   tail[24];     /* +32   &D_8012A600    */
    unsigned char between[272]; /* +320                 */
    s16           blockIDs[512];/* +592  &gWaveBlockIDs */
} g;
```

With the hole: walk runs to k=1199, its store at k=50 hits `gWaveBlockIDs[8]`, and
tracks 8/10/30 die with Bus error / SIGSEGV / SIGSEGV. Without it: k ≤ 22, all clean.
This works because **the web build is bit-reproducible** — a local pre-fix build is
byte-identical to the shipped `a08bbcb0…`, so the offsets above are the real ones.

**Native cannot catch this defect at all** — the gap is 0 there by luck, so every
native fixture passes either way, and ASan does not redzone the
`__DATA,__common` symbols involved. UBSan `-fsanitize=array-bounds` *does* flag the
out-of-bounds indices (in `waves_alloc` and `waves_init`) without any crash; that is the
native tell.

## Out-of-bounds index sweep — `tests/check_array_bounds_sweep.py` (RUN THIS AFTER ANY `game/src` CHANGE)

The generalisation of the check above, and the detector for the whole split-array
class: a UBSan `-fsanitize=array-bounds,pointer-overflow` build driven over 8
representative routes (~1.5 min), failing on **any** out-of-bounds index that is not
in its committed allow-list. It builds `build-ubsan/` itself; `--no-build` reuses it.

```bash
MDKR_AUDIO=0 python3 tests/check_array_bounds_sweep.py -v
```

Why it exists rather than a table of symbol pairs: the class is *an index leaving its
array*, and a pair table only knows the pairs somebody already thought of. It found
two live defects (`gTrackSelectIDs`/`gFFLUnlocked` and `gScreenViewports[4]`) that no
crash and no native fixture had ever revealed — see docs/OPEN_ITEMS.md wave
"splitsweep".

Every allow-list entry carries the reason it is not a finding, and entries are keyed
on **(file, type, source snippet)** rather than line number, so moving code does not
re-baseline the list but changing the offending line does.

It **fails closed** four ways, because "no reports" cannot distinguish clean from not
measuring: the binary must actually import `__ubsan_handle_out_of_bounds` (the
sanitizer flag has to survive CMake's flag ordering — note that `-Wno-everything`
lands *after* `CMAKE_C_FLAGS`, which is why the flags go through
`-DMDKR_EXTRA_C_FLAGS`), every route must exit 0, the report set must be non-empty,
and `REQUIRED_SENTINELS` (allow-listed idioms on paths every route crosses) must all
still be reported.

When it fails, triage before allow-listing. The question is *not* "does it crash
here" — the wave-table defect never crashed natively. It is: does the index leave its
array; what object does it land on **on LP64 and on wasm32**
(`tools/compare_data_layout.py` answers that); and does anything depend on it landing
there (`gFFLUnlocked` did). If the adjacency is load-bearing, back both names onto one
object with `_Static_assert`s, as `game/src/waves.c` and `game/src/menu.c` do.

Known blind spot: an overrun written through a **bare pointer** rather than an indexed
array is invisible to this check and to ASan. `get_inside_segment_count_xz()` is the
live example.

## ROM revision + byte order — `tests/check_rom_revision.py` (RUN THIS AFTER ANY ROM-LOADER CHANGE)

Which of the five released DKR revisions did the user hand us, and does the browser
shell agree with the engine about it? The old gate was size + magic + "the internal
title contains 'diddy'", which **all five revisions pass** — so a European or
Japanese cart was accepted and then SIGSEGV'd before the first frame, reading its
asset lookup table from the us.v80 offset. Two of the five are now supported
(us.v80, pal.v80); the rest are refused by name.

```bash
python3 tests/check_rom_revision.py           # skips any revision not present
python3 tests/check_rom_revision.py --roms build/roms -v
```

Asserts: each present revision is classified and named; unsupported ones are
refused **cleanly** (exit 1, no crash trace, no renderer bring-up); a non-DKR N64
ROM is called out as such; synthesised `.v64`/`.n64` copies race byte-identically
to the `.z64`; `platform/rom_id.c` and `dist/web/rom-id.js` produce identical
revision tables and identical verdicts (the JS is run under node); and pal.v80 —
whose whole claim to support is a byte-identical asset payload — renders and drives
byte-identically to us.v80.

It synthesises its byte-swapped and non-DKR inputs at runtime under a temp dir and
deletes them, and **skips cleanly** for any revision that is absent, so it passes on
a machine with only `baserom.us.v80.z64`. Verified in both directions; the pasted
output of both positive controls, and the two flaws the controls exposed in the
check itself, are in [../docs/ROM_REVISIONS.md](../docs/ROM_REVISIONS.md) — which is
also the per-revision failure taxonomy.

## Visual checks

Never open a window to look at something — dump and convert:
```
MDKR_AUDIO=0 MDKR_DUMP_FROM=1240 ./build/mdkr64 --headless-frames 1252 \
  --dump-frames /tmp/shot --rom baserom.us.v80.z64
sips -s format png /tmp/shot/frame_1250.ppm --out /tmp/shot/shot.png
```
`MDKR_DUMP_FROM` keeps a late-scene capture from writing thousands of early PPMs.
`MDKR_DUMP_EVERY=N` adds a stride on top of it, so sampling ~10 frames across a
6000-frame drive costs one pass and 10 PPMs instead of thousands (that is what
`check_race_drive.py` uses). Frame ~1250 is the Timber's Island frontend
(sand/palms/character); frame ~2880 of `race_drive_time_trial` is the Ancient Lake
start line (full-size karts + HUD).

### Boost / exhaust graphics — `MDKR_FORCE_BOOST=<tick>[:<len>]`

`MDKR_FORCE_BOOST` gives every racer, on every authored tick in `[tick, tick+len)`
(default `len` 90), exactly the state a `SURFACE_ZIP_PAD` gives it
(`boostTimer = normalise_time(45)`, `boostType = BOOST_LARGE`), so a dumped frame
is guaranteed to contain rendered boost flames. It is a no-op unless the variable
is set (`objects.c dkr_force_boost_hook`).

```
MDKR_AUDIO=0 MDKR_DUMP_FROM=2810 MDKR_FORCE_BOOST=2830:120 ./build/mdkr64 \
  --headless-frames 2960 --dump-frames /tmp/boost \
  --input-script tests/input_scripts/race_drive_time_trial.txt \
  --rom baserom.us.v80.z64
sips -s format png /tmp/boost/frame_2910.ppm --out /tmp/boost/boost.png
```
Expect a bright, character-tinted exhaust plume out the back of every kart
(player white/yellow, Banjo green-yellow, Krunch magenta) at frames 2850..2950,
and none at 2820. This is the regression check for the `ASSET_MISC_20` boost-table
decode (`objects.c dkr_boost_table`); if that decode breaks again,
`render_sprite_billboard` aborts with `[FATAL] ... NULL sprite`.

Why a hook and not a fixture: originally, no deterministic input script reached a zip
pad (the port's driving physics stranded the racer), and in the attract demo only
racers the camera is *not* following ever boost, so their boost objects are correctly
culled before the draw. The stranding is fixed (`docs/OPEN_ITEMS.md`) and
`race_drive_long.txt` now takes a genuine zip pad on camera at ~frame 5650 — dump
around there for an unforced boost. `MDKR_FORCE_BOOST` is kept because it puts a boost
on *every* racer at a chosen frame, which is a stricter check of the table decode.

## Full-race / finish path — `race_full_3lap.txt` + `MDKR_FORCE_LAPS`

`race_drive_long.txt` proves the racer drives and completes ONE lap.
`race_full_3lap.txt` drives toward the race's natural end. Note its limit: the
open-loop route (hold accelerate, pulse LEFT for 25 frames every 120) holds the
racing line for one lap and then drifts — measured lap 1 at clock 2776, lap 2 not
until 10557 — so do **not** rely on it to reach the finish.

To exercise the finish deterministically, shorten the race instead:
```bash
MDKR_FORCE_LAPS=1 MDKR_AUDIO=0 ./build/mdkr64 --headless-frames 8000 \
  --input-script tests/input_scripts/race_full_3lap.txt --rom baserom.us.v80.z64
```
`MDKR_FORCE_LAPS=N` rewrites `LevelHeader.laps` once at the load boundary
(`platform/asset_swap.c`), so every one of the ~12 readers across `racer.c` and
`game_ui.c` sees a consistent value. It is a **no-op unless set**.

Note `MENU_RESULTS` (menuId 17) is the **trophy-race** results screen; a Time Trial
finish is not expected to reach it, so its absence is not a defect.

## Adventure — `adventure_hub_drive.txt` + `tests/check_adventure_hub.py`

The only fixture that leaves the frontend on the **Adventure** side. It walks
FILE SELECT → new-game name entry → the intro cutscene → Timber's Island, then
drives the hub with a real player-input throttle/steer pattern.

```bash
python3 tests/check_adventure_hub.py -v        # ~40 s, muted + headless
```

Reference (deterministic): `menu_init` 19 @1781, 6 @1931, 23 @2236;
`level_load: levelId=36 cutscene=15` @2236 (the intro cutscene) then
`level_load: levelId=0` @**5867** (`ASSET_LEVEL_CENTRALAREAHUB`); 6133 in-hub
frames, path length 44441 world units, bounding box 5989 × 4069, y −35..539,
final `cp=23 lap=2`, max single-frame step 13.8, sampled frames 1014–2345
distinct colours / sigma 36.9–57.7.

The cutscene budget is not padding: it ends when its terminating animation entry
(actor 23, `animationStartDelay` 3630) counts down one tick per game update, and
it is **not** skippable — `cinematic_start()` is called with both skip masks 0.
So the hub cannot be reached before frame ~5867; do not shorten the budget.

The check allows **one** sampled frame to score below the render thresholds: the
route brushes a hub door, and DKR's own door fade whites the screen out for ~30
frames (measured 209 colours / sigma 7.9 around frame 6600).

This route is what caught three latent LP64 defects, all reachable by ordinary
play and none reachable from any other fixture — the `GameTextTableStruct.entries`
stride (which corrupted the arena via a 64 KB `asset_load`), the
`ParticleBehaviour` 0xA0 record stride, and the `func_80026E54` `sp94[10]` stack
overflow. Full write-up in `docs/OPEN_ITEMS.md` "P3.5". `MDKR_AUTOPILOT=1` also
drives the hub (it laps the hub spline: cp 66 / lap 6 in ~6100 frames).

**`MENU_RESULTS` (menuId 17) is NOT reachable from a 1-player Adventure race** —
`menu.c` gates it on `gNumberOfActivePlayers >= 2`, and a won 1-player Adventure
race goes through `race_transition_adventure()` back to the hub instead. See
`docs/OPEN_ITEMS.md` "P3.5 Part B" before spending time on it.

## Adventure Two — `tests/check_adventure_two.py`

```bash
python3 tests/check_adventure_two.py                    # all 20 race tracks
python3 tests/check_adventure_two.py --tracks 5,19 -v  # focused iteration
```

This is a real mode/save route, not `CHEAT_MIRRORED_TRACKS` injected into a
Tracks race. Every arm installs a checksum-valid N64-format EEPROM, selects
Adventure Two at GAME SELECT, resumes a slot carrying
`CUTSCENE_ADVENTURE_TWO`, drives through Timber's Island and Dino Domain, and
enters a race. `MDKR_LOAD_TRACK` retargets that final load so the same campaign
path drives all 20 main tracks.

The check requires finite racer state and checkpoint progress on every track,
the exact Adventure Two bit in the rewritten save, and the canonical
big-endian global unlock block. Trace witnesses prove that stereo panning,
minimap placement, human steering, and the negative N64 viewport path all
execute their mirror operations.

The pixel control compares Adventure One and Two before AI difficulty can
diverge. It crops out the deliberately fixed safe-4:3 HUD and horizontally
reflects the Adventure One world. Measured over 14 samples: same-orientation
MAD **49.154**, reflected MAD **2.816**. Reverting the clip-X translation makes
the two modes byte-identical and the positive control fails.

## Adventure race loop — `adventure_race_loop.txt` + `tests/check_adventure_race_loop.py`

The fixture above drives the hub; **this** one enters a race from it and comes
back. It is the first route that runs `obj_loop_exit()` → `racer->exitObj` →
`racer_enter_door()` → `func_8006D968()`, i.e. DKR's actual level-to-level
transition machinery.

```bash
python3 tests/check_adventure_race_loop.py -v   # ~55 s, muted + headless
```

Current reference:

```
level_load: levelId=0  @5867                       Timber's Island
level_load: levelId=12 @6635                       Dino Domain lobby
level_load: levelId=5  @6936                       Ancient Lake
[PACE] frame=12186 ... clock=4711 rlap=3 fin=1 fpos=1 ridx=0
level_load: levelId=12 @13624 entrance=3           win return to lobby
level_load: levelId=0  @13976 entrance=2           back on Timber's Island
```

The primary arm records 6685 race-level rows, 62600 world units through the
natural finish, and max driving step 20.4; 3022 rows / 35551 units prove the kart
is alive after returning to the hub. The scene sampler spans the race, win
transition, lobby, and hub.

**Steering comes from a test hook, not from the script.** Hand-timed stick input
cannot hit a hub door — 18 open-loop routes failed, and `MDKR_AUTOPILOT=1` laps
the hub's AI-node loop for ever (its closest approach to *any* hub exit is 2144
units). `MDKR_DRIVE_ROUTE` (`platform/mdkr_adventure.c`) steers closed-loop at
named objects instead:

```bash
MDKR_OBJDUMP=1     # per level: every exit / door / balloon / dialogue NPC, with
                   # the fields that decide whether the player may pass (=2 adds
                   # the AI-node graph). This is how the route was authored.
MDKR_DRIVE_ROUTE="0:200,500:B10:E12;12:E5:E0"
                   # per level, ordered steps: E<destMapId> = drive at the exit to
                   # that level, B<id> = collect that golden balloon, x,z = a
                   # waypoint. Step progress persists per level, so the lobby
                   # drives INTO the race on the way in and back OUT on the way
                   # home. Unlisted levels are untouched, so MDKR_AUTOPILOT drives
                   # the race itself.
MDKR_DRIVE_WPR=<units>      waypoint radius (default 220)
MDKR_DRIVE_VERBOSE=1        per-30-frame progress trace
```

Five things about this route are worth knowing before changing it:

- **The route collects a golden balloon so the doors really open.** The Dino
  Domain doors want 1 (`obj_loop_door`: `*settings->balloonsPtr >=
  door->balloonCount`); with balloon 10 banked the lobby's Ancient Lake door
  reports `open=1`. Only 4 of the hub's 7 balloons are collectable — the other 3
  are Taj-challenge balloons, inert until their `tajFlags` bit is set.
  Production object-model collision makes a closed door a hard gate. The
  same-binary `check_door_blocks.py` control proves that the exact legacy
  collision path still drives through, so this fixture must continue to collect
  its balloon rather than relying on the old defect.
- **Do not bump Taj or T.T.** They open a dialogue on contact and then call
  `disable_racer_input()` every frame, which zeroes the kart's velocity; the
  dialogue is advanced by `input_pressed()` off the pad, which the drive hook
  cannot write. Measured wedge: frozen at (−125.3, 149.6) for 3000+ frames, 84
  units from Taj.
- **Never hold A in an Adventure fixture.** Every post-race screen is driven by
  `input_pressed()`, which is edge-detected: while A is held there are no further
  edges and every later tap is silently swallowed. This is why this fixture has
  no `A <long hold>` line even though `adventure_hub_drive.txt` does.
- **Starting-grid slot zero is not "the player."** `gRacers` preserves starting
  order. The old finish probe sampled `(*gRacers)[0]`, followed an AI, and
  produced the false GAME-03 report that the human reached lap three without
  finishing. The gate now follows `gRacersByPort[PLAYER_ONE]` and asserts its
  `racerIndex`.
- **Natural finishing place is observed, not asserted.** The old checkpoint
  finished fifth; the current runtime-boundary build naturally finishes first.
  `MDKR_ADVENTURE_WIN=1` and `MDKR_ADVENTURE_LOSS=1` act only after production
  marks the same natural finish, then request first and non-first verdicts.
  Both arms must report the same pre-control place/frame/clock/lap and preserve
  identical lap, clock, position, and physics.

Each arm runs in its own temporary working directory and captures its new
`save/eeprom.bin` before cleanup. The checker derives Ancient Lake's save ordinal
from the ROM, validates the slot checksum, and decodes course and balloon bits
itself. Loss must be status 1 / `(1,0,0,0,0,0)`; win must be status 2 /
`(2,1,0,0,0,0)`. Shared repository save state is never read or removed.

## Post-race door fling — `tests/check_postrace_door_fling.py`

Issue #41: every way out of an Adventure race funnels through one return load
(`levelId=12 entrance=3 cutscene=100`) that places the kart in the race door's
alcove and drives it back out while the door — opened by the kart's own
proximity — is still rising. `collision_objectmodel()`'s authored frame pairing
(previous-tick inverse, current-tick forward) is what lets moving meshes carry
riders; for a rising gate it welded the door's vertical step onto every
laterally blocked racer point and the integrator amplified that into a launch
(measured y 5 → 231 in 28 ticks, pinned at the alcove ceiling). The fix
evaluates rising sliding doors in one consistent current-pose frame with
world-history origins, and re-primes a spawned object's collision matrices
from its final pose so the first post-load tick reads and writes one frame.

```bash
python3 tests/check_postrace_door_fling.py -v   # three arms, muted + headless
```

Two fixed arms drive the quit paths the race-loop check does not: a pause-menu
`RETURN TO LOBBY` mid-lap (`adventure_quit_midrace.txt`) and the same quit
during the starting camera pan (`adventure_quit_startpan.txt`). Both must show
the real return load, a bounded return altitude, no single-frame teleport, and
a completed descent to the alcove floor. The third arm re-runs the mid-race
quit with `MDKR_DOORCARRY=legacy`, which restores the carry-frame pairing for
rising doors from the same binary; the launch must reappear, otherwise the
fixed arms' altitude bound is not being tested. The win/loss return paths stay
covered by `check_adventure_race_loop.py`'s own return-altitude assertion.

The gate is registered as `postrace_door_fling` in `tools/run_checks.py`.

## Recording a time — `race_full_3lap_tt.txt` + `tests/check_race_finish_time.py`

**`race_full_3lap.txt` can never save a time, and neither can any `*_time_trial`
fixture.** Their names are misleading: they select *Tracks* mode and then confirm
**TIME TRIAL = OFF**. DKR writes course/lap records only from
`race_finish_time_trial()`, whose whole body is gated on `if (gIsTimeTrial)`, so on
those routes the record write is unreachable by construction (measured at the
finish: `gIsTimeTrial=0`, `gNumRacers=8`, `display_times=0`, `best_times=0x00`).

`race_full_3lap_tt.txt` is the route that does record a time. It differs by one
input — a stick **DOWN** at the track-select Time-Trial stage, which flips
`gTracksMenuTimeTrialHighlightIndex` 0 → 1 — plus repeated A taps after the finish,
because the post-race screens are edge-detected and need a fresh press per stage
(BEGIN → SHRINK_VIEWPORT → RACE_TIMES → ENTER_INITIALS → RACE_RECORDS → OPTIONS →
END). Use it with `MDKR_AUTOPILOT=1`; a solo Time Trial has 1 racer, so no throttle
input is wanted.

```bash
python3 tests/check_race_finish_time.py -v        # ~90 s, muted + headless
```

It asserts the full chain, not just survival: `rlap=` reaches the level's lap
count, `fin=` flips 0 → 1, the race clock freezes, the frozen value is plausible,
that **exact** value is present in `save/eeprom.bin` as a 16-bit big-endian
record, a second run reads it back rather than resetting it, **and the process exit
code is 0**. That last one matters: the `timetrial_ghost_read` stack overflow it
caught aborted with nothing at all on stderr.

It also asserts that the route **reaches the ghost-playback path**
(`ghost=` ≥ 100 frames; measured 5010, `gbank=0` = the player's own ghost). The
fixture's trailing A taps pick "TRY AGAIN" after the finish, which re-enters the
level and plays back the ghost recorded on the previous lap set — the only thing
that exercises `timetrial_ghost_read()`. Asserting the coverage means a future
route change that stopped re-entering the level fails loudly instead of quietly
covering nothing.

⚠️ If you positive-control the `timetrial_ghost_read` fix, revert **both** coupled
parts — the array size `[4]` → `[3]` *and* the loop bound `<` → `<=`. Reverting
only the size leaves the loop writing three elements: memory-safe, so no runtime
check can catch it, but silently wrong. The `_Static_assert` makes that state fail
to compile; don't neuter it. See the three-state table in `docs/OPEN_ITEMS.md`.

`rlap=`/`fin=`/`fpos=` are `[PACE]` fields fed from the post-assignment seam in
`race_check_finish()`. The older `lap=`/`cp=` come from inside
`update_player_racer`, which stops running for a racer the moment it finishes —
so they are permanently one lap stale at the finish and cannot see
`raceFinished`. Assert on the race-check fields for anything finish-related;
`ridx=` proves the stable controller-mapped racer is being observed rather than
a starting-grid slot. `ghost=`/`gbank=` are fed from
`timetrial_ghost_read()` and count interpolated ghost frames (bank 0/1 = the
player's own ghost, 2 = staff).

Reference (deterministic, Ancient Lake, car): `level_load` frame ~2641, finish at
traced clock **4709** / frame **7495**, fastest lap **1515**; `save/eeprom.bin` md5
`bfd4de76775de4b5c5d972a97eeb6ed7`, course record **4683** at byte 338.

⚠️ **These numbers moved in P3.5** (they were clock 4777 / frame 7589, fastest lap
1558, md5 `a144ba9f…`) for the same reason `race_drive_long`'s did — the
`ParticleBehaviour` on-disk-stride fix changed the trajectory of a route driven by
`MDKR_AUTOPILOT`. The check now matches the EEPROM record to the traced clock
**within 32** rather than exactly: the traced value comes from the `[PACE]` probe
inside `update_player_racer` (which stops for a finished racer) while the record is
what `race_finish_time_trial()` sums when it runs, so the two reads sit a few
updates apart. They coincided before and no longer do. See `docs/OPEN_ITEMS.md`
"P3.5".

## Ghost matrix — `tests/check_ghost_matrix.py`

`check_race_finish_time.py` above exercises `timetrial_ghost_read()` on **one** of
the 47 legal (track, vehicle) pairs — Ancient Lake in the car. That one row is how
the 3-vs-4 control-point overflow was found, and it was found by accident. This
check covers the other 46.

The pair matters, not just the track: a Controller Pak ghost slot is keyed on
**both** ids, so `func_80074B34()` / `func_80075000()` (`game/src/save_data.c`)
match on `levelId` *and* `vehicleId`. Ancient Lake in a car round-tripping says
nothing about Ancient Lake in a plane.

```bash
python3 tests/check_ghost_matrix.py               # all 47 pairs, ~15 min
python3 tests/check_ghost_matrix.py --subset      # every track and vehicle once
python3 tests/check_ghost_matrix.py --pairs 5:0,15:2
python3 tests/check_ghost_matrix.py --matrix      # print the pairs and exit
```

Reference run (us.v80, `build-rel`): **46 pairs round-tripped a ghost through a
fresh process, 1 documented non-producer, 15.4 min** — measured alongside other
ROM work on the same machine, so treat it as an upper bound rather than a floor.
Three pairs needed the fallback cadence (19 hovercraft, 30 plane, 33 hovercraft).

Three runs per pair, each in its own `MDKR_SAVE_DIR` — 47 ghosts do not fit in the
pak's 6 slots (`DKR_GHOST_SLOT_COUNT`), and per-pair isolation is what keeps them
from contending:

1. **measure** — drive to the finish and read the frame the post-race OPTIONS
   stage opens offering SAVE GHOST. A three-lap finish lands anywhere between
   ~7370 and ~12470 frames depending on the pair, so a fixed script cannot aim at
   a menu row across 47 rows; `[TTGHOST] event=options` makes the aim a
   measurement. Its trailing taps pick TRY AGAIN, which re-enters the level and
   plays back the just-recorded ghost — the in-process half of the coverage, and
   the original overflow route.
2. **write** — fresh save dir, identical re-drive, menu steered onto SAVE GHOST
   (the only route to `timetrial_save_player_ghost()`). Asserts
   `[TTGHOST] event=save … status=0` with a non-empty node count and that
   `controller-pak-1.mdp` appeared.
3. **read** — a **fresh process** on that save dir. `[TTGHOST] event=load` must
   report the same pair and the same `nodes`/`character`/`time` that were
   written, and playback must run from a player bank (0/1, not the staff ghost).

`nodes`/`character`/`time` are the whole of `GhostHeader` apart from its checksum,
so matching all three is the serialised record surviving a process boundary in
every field the format carries. On top of the trace comparison the write run
locates that header **in the pak image itself**, by its measured contents rather
than a hardcoded offset — the same discipline `check_race_finish_time.py` uses for
the EEPROM course record, and for the same reason: a layout or byte-order
regression in the ghost serialiser moves those bytes and fails here, where an
offset-keyed probe would keep passing. The read run then asserts the pak comes out
**byte-identical**, because an identical re-drive cannot beat the stored time and
must not rewrite player data.

The pak image holds `GhostHeader`/`GhostNode` exactly as the machine laid them
out, which is what a real Controller Pak holds too (raw N64-order structs); on a
little-endian host the same design yields little-endian fields, so the check
builds its search pattern from `sys.byteorder`. It asserts "the struct
round-tripped verbatim", not "the port picked an endianness".

The save format is frozen — this check proves the **current** layout round-trips;
it must never be relaxed to accommodate a layout change.

Every run asserts the **exit code** and the presence of the markers a healthy run
emits, never merely the absence of a crash marker: the defect class this exists
for (`__stack_chk_fail`) prints nothing at all to stderr.

`--re-entry-pair` (default `5:0`) additionally drives write → reload → read →
re-enter → read again, because the original overflow only appeared on a *repeat*
read within one process.

### Pacing decides which pairs can be lapped at all

Whether `MDKR_AUTOPILOT` completes a lap is **pacing-dependent, in both
directions** — neither simulation cadence laps the whole matrix:

| pair | Original (`SYNTH_FIELDS=2`) | Enhanced (`SYNTH_FIELDS=1`) |
| --- | --- | --- |
| track 4, hovercraft | finishes frame 5614 | stalls at `rlap` 1, `cp` 25 |
| track 33, car | finishes frame 6837 | stalls at `rlap` 1, `cp` 52 |
| track 19, hovercraft | stalls at `rlap` 0 | finishes |
| track 30, plane | stalls at `rlap` 0 | finishes |
| track 33, hovercraft | stalls at `rlap` 0 | finishes |

All measured over a 40000-frame budget. Track 33 appears on **both** sides, which
is the point: this splits per *pair*, not per track. Track 4 offers **only** the
hovercraft, so pinning Enhanced would leave a whole track unlappable; pinning
Original loses three more pairs. So each pair is driven on the **first cadence
that gets it round**
(`CADENCE_ARMS`, Original first — the pacing `check_vehicle_sweep.py` sweeps this
same matrix under), and any pair that needed the fallback is tagged
`[enhanced cadence]` in the output rather than quietly absorbed.

A run that *dies* is never allowed to fall through to the next cadence — that would
let a crash read as "that pacing just could not lap it".

This is a pacing-dependent AI/driving divergence, not a ghost defect. It is
recorded here because it decides which pairs this check can cover, and because it
is invisible to `check_vehicle_sweep.py`, which asserts forward progress
(`maxcp >= 3`) rather than lap completion.

⚠️ One legal pair is a documented **non-producer** and is asserted to stay that way
rather than skipped: Spaceport Alpha (15) **in the car**. Its autopilot racing line
dead-ends at `courseCheckpoint` 10 and completes no lap in 40000 frames on *either*
cadence, and DKR only records a ghost for a course time under 10800 frames
(`race_finish_time_trial()`). The pair is legal in the ROM's `available_vehicles`
mask and is driveable by a human; it is the AI line that does not get a car round
that track. If it ever finishes, the check fails and tells you to promote it out of
`NO_GHOST_PAIRS`.

## Ghost bank capacity — `tests/check_ghost_bank_capacity.py`

The ghost matrix above gives every pair its own `MDKR_SAVE_DIR` because the
authored DKRACING-GHOSTS note holds exactly six ghost slots
(`DKR_GHOST_SLOT_COUNT`, `game/src/save_data.c`) — the original ROM's own file
format, and on the port exactly the "CONTROLLER PAK FULL after six ghosts"
defect of issue #46. `platform/ghost_bank.c` lifts that ceiling without
touching the format: before a pair is loaded or saved, the least-recently-used
window pair is swapped out to a per-pair bank file under `<save>/ghost-bank/`
and the requested pair's banked record is swapped back in, verbatim. This
check is the matrix's counterpart for that machinery: it drives **more pairs
than the window holds against ONE shared save directory** — the contention the
matrix deliberately avoids.

```bash
python3 tests/check_ghost_bank_capacity.py             # 8 pairs, one save dir
python3 tests/check_ghost_bank_capacity.py --pairs 10
python3 tests/check_ghost_bank_capacity.py --list      # candidate pool
```

Driving mechanics (navigation prefix, cadence arms, options-frame measurement,
generated tap scripts, exit-code-first verdicts) are imported from
`check_ghost_matrix.py` rather than re-derived, so the two checks cannot drift.
Each pair measures its SAVE GHOST frame on a private directory (the
deterministic re-drive can never beat its own stored time), then all writes
and reads run **sequentially against the one shared directory**. Asserted:

1. Every save reports status 0, and **no** `[TTGHOST]` row of any run reports
   status 6 (`CONTROLLER_PAK_NO_ROOM_FOR_GHOSTS`) — the defect fails loud on
   first occurrence.
2. Every pair reloads in a fresh process with the exact
   `nodes`/`character`/`time` it saved; with more pairs than slots that is
   only possible through bank restoration.
3. Byte identity through eviction: the serialised record captured from the pak
   image after each save must reappear **verbatim** after that pair's read
   run. Pairs whose record was absent beforehand were genuinely evicted; at
   least pairs-minus-six such round trips must occur, and each must be traced
   by the bank itself (`[GHOSTBANK] event=restore`), so whatever restored the
   bytes provably was the bank.
4. The pak container keeps its authored 32192-byte real-hardware-faithful
   size: extra pairs live in the bank, never in a grown pak.

The authored per-slot ghost erase (`func_800753D8`), whole-note deletion and
reformat still stick — the bank reconciles its index against the live window,
drops bank copies the game deleted, and fails toward deletion when its own
sidecar is lost: sweeps read the bank directory itself, so records neither
the window nor the index can vouch for are quarantined to `.bad.N` (never
loaded again, still recoverable by hand) rather than left to resurrect — and
a wipe after note deletion/reformat quarantines rather than unlinks, so a
pak-side I/O accident cannot destroy the library. `tests/test_ghost_bank.c`
covers those arms at the unit level, along with LRU order, swap bookkeeping,
the first-run migration split of a pre-bank note, and refused-write no-ops;
the swap's crash-ordering argument lives in the code comment at
`ghost_bank.c`'s evict site, not in a unit that interrupts the stores.

## Taj vehicle challenges — `tests/check_taj_challenges.py`

This is the end-to-end gate for all three dynamically created Timber's Island
vehicle challenges. It covers car, hovercraft, and plane at their live ROM
thresholds (5/10/18 balloons), with first win, loss, abort, completed replay,
and a disabled-completion positive control for each vehicle.

First-time arms exercise the real threshold offer, Taj dialogue and
transformation. Replay enters at the production acceptance seam because a
completed challenge is not auto-offered. Every arm checks two-racer movement,
production finishing places and result dialogue, exact `tajFlags`, EEPROM
checksum, and a second-process reload; first-win arms also score a rendered
frame.

```bash
python3 tests/check_taj_challenges.py --quick -v  # five car arms
python3 tests/check_taj_challenges.py -v          # all 15 arms
```

The fixture driver contributes only carpet progress/hold events or the same
quit request used by the pause menu. It never writes the human finish, place,
result menu, progress flags, or save; the assertions below define the exact
boundary.

## Playable Taj mod — `tests/check_taj_playable.py`

This is the ROM-backed end-to-end gate for the native virtual Taj character. It
enters `ABRACADABRA` through the real Magic Codes keyboard, selects the next
contiguous virtual display identity, races and restarts with the carpet
composition, then reboots
against the persisted global sidecar. Separate two-, three-, and four-player
arms prove Taj belongs to the selecting controller and viewport; the two-player
arm also proves ordinary Diddy can coexist even though both resolve to retail
donor row 9. The car/hovercraft/plane arms prove the selected identity reaches
every ordinary vehicle dispatch. An imported-save arm reconciles existing
`tajFlags` before the first select visit. Exact audio traces cover the Taj
highlight, confirmation, and horn paths. A real WebGPU race capture is joined
to the carpet/rider composition and donor-suppression traces, then checked for
Taj's purple robe and the red/gold carpet. Instrumented witnesses additionally
require the ROM-authored carpet clock to publish changing vertex hashes through
both render buffers and require the playable composition to preserve the retail
7:15 carpet-to-Park-Warden scale ratio. The rendered lower-centre silhouette
rejects the former oversized 1:1 donor scale. A negative-control run freezes
the mesh and restores that 1:1 scale; the same witness validator must reject
both defects. The two-player arm also A/Bs production against the exact former
split-screen filter and requires the carpet-only grounding shadow to darken the
captured raster. The Time Trial arm first establishes
canonical records, then reaches Taj's normal finish hook, requires the finish
presentation binding to remain live, and proves those existing record tables
are byte-semantically unchanged.

```bash
MDKR_AUDIO=0 python3 tests/check_taj_playable.py --build build \
  --rom baserom.us.v80.z64 -v
```

The check is also registered as `taj_playable` in `tools/run_checks.py`. It is
intentionally not a CTest because it needs a caller-supplied supported ROM; the
Taj state and tuning seams remain covered by the ROM-free `taj_mod` and
`taj_physics` CTests; `taj_mod_state_file` separately proves that an atomic
sidecar store creates a missing save directory and leaves no temporary file.

`tests/check_taj_results_portrait.py` carries the same identity through a real
two-player race into Rankings. It captures Taj beside an ordinary Diddy,
requires the project-owned Taj card to match the retail portrait's 40x40
contract, and checks its purple turban, blue elephant face/trunk, and gold
detail against the unchanged Diddy negative control. The old tan portrait
fails the face checks; a pixel control removes the gold detail and must fail
that specific assertion. `--keep-frames DIR` retains captures and the log for
visual review. The route then returns to Track Select,
starts the race again, and repeats the capture after a full stage/menu teardown;
this protects both the portrait identity and its native display-list/texture
lifetime.

Both Taj portrait gates default to the shipping WebGPU backend; `--renderer gl`
retains the diagnostic comparison. They require the requested backend's runtime
witness rather than accepting a silent renderer fallback.

`tests/check_taj_hud_portrait.py` proves the same card in the retail P2
Adventure HUD slot. It selects Taj as P2, reaches the real hub without changing
lead state, and joins the exact HUD identity trace to visible purple, blue,
elephant face, and jewel pixels at the authored portrait anchor.

`tests/check_taj_p2_adventure.py` covers the lead-player seam ordinary Tracks
multiplayer cannot reach. It enters `JOINTVENTURE` through the retail Magic
Codes keyboard, selects visible Taj as P2, and uses the explicitly test-only
`MDKR_TAJ_P2_LEAD_HANDOFF=1` at the safe hub boundary to invoke the production
`swap_lead_player()` transaction. The following real two-player Adventure race
must bind swapped settings slot 0 to live controller port 2, preserve both
viewports, and keep every observed ROM-facing character ID in the retail 0..9
range. `MDKR_TAJ_P2_LEAD_TRACE=1` is observation-only.

`tests/check_taj_speed_profile.py` measures all three vehicles against paired
ordinary controls and holds sustained speed to 1.35x +/-2%. The registered Taj
arm of `tests/check_vehicle_sweep.py` runs every one of the ROM's 47 legal
track/vehicle pairs and requires live Taj identity, carpet/rider presentation,
shield anchoring, and a deterministic car dash witness.

`tests/check_taj_visual_lifecycle.py` injects loss of each picker and race
companion only after a complete pair exists. The surviving half must be cleaned
up atomically, donor rendering must recover during the gap, and a bounded fresh
generation must recompose without stale ownership.

`tests/check_taj_character_select.py` is the rendered roster gate the gameplay
journey cannot replace. It constructs the base-eight, Drumstick-only, T.T.-only,
and complete-ten retail layouts, requires Taj at contiguous indices 8/9/9/10,
navigates a real controller route into each slot, and checks captured pixels and
instrumented model probes for the standing Park Warden/Taj actor, unchanged
ordinary-character hover across every available retail slot, animated Taj
hover, the structurally verified placard-only asset batch, safe-area occupancy
of the surrounding cast, and final Taj identity. Focused captures prove the
authored P2, P3, and P4 placards. A rendered negative control restores the full
Park Warden shadow that originally remained after Taj was scaled for the picker;
the oracle requires production to retain a small grounding shadow while
rejecting the control's large dark footprint. Focused PAL and 21:9 runs qualify
the supported second ROM revision and widescreen safe area; every sampling box
is stated in ROM-logical pixels and mapped through the presentation the
renderer actually composed, because a forced aspect centre-fits the authored
320x240 envelope inside the drawable (21:9 in a 1280x960 capture puts it at
731x549+274+206) rather than stretching it over the frame. Reading those boxes
at raw pixel coordinates samples the letterbox bar, which is how an oracle can
report a placard that is plainly on screen as absent. One injected
allocation failure must
recover, while an exhausted retry budget must visibly fail closed and make the
invisible slot unselectable.
`taj_select_layout` is the ROM-free companion CTest: the same row definition
drives both actor placement and every directional cursor candidate. The picker
gate defaults to OpenGL and accepts `--renderer webgpu`; the release runner
qualifies the complete roster on both shipped renderer paths and adds a focused
21:9 arm.

`tests/check_browser_taj_character_select.py` closes the platform gap through
the shipped shell and a real Chrome/WebGPU process. Starting from a fresh
browser profile, it enters `ABRACADABRA` through the public keyboard route and
joins the runtime model/animation probes to four browser screenshots. It fails
if Taj is not visible before hover, the numbered P1 placard is absent, the pose
does not animate, final identity mapping is wrong, or the former all-caps badge
has replaced the physical actor again. It then flushes IDBFS, reloads the same
isolated profile without entering the code, and requires the physical Taj actor
and placard to return from durable state. The gate is registered as
`browser_taj_character_select` in `tools/run_checks.py`.

`tests/check_browser_taj_persistence.py` rejects the first real
`Module.__mdkrPersist({reason: "taj-mod"})` promise after the C side has replaced
its MEMFS sidecar. It requires the visible C failure path, exact retained bytes,
a successful ordinary retry flush, the matching IndexedDB record, and a fresh
document that restores Taj without re-entering `ABRACADABRA`.

`tests/check_browser_magic_codes_persistence.py` applies the same real-browser
durability fault to the general Magic Codes sidecar: the first IDBFS commit is
rejected, exact accepted/active bytes remain in memory, an ordinary retry
persists them, and a fresh document restores the enabled state without entering
the code again.

## Complete bonus roster — `tests/check_bonus_character_select.py`

This focused real-ROM smoke gate arms the version-3 sidecar and a completed
retail save, then requires the contiguous 13-racer picker. It proves that Taj,
Wizpig, and Terry each compose an independent physical actor/placard pair,
checks the exact roster mapping, and navigates a real controller route from the
retail cast through Wizpig to Terry. It also confirms/deselects Wizpig and
confirms Terry, requiring each actor's unoccupied dance, hovered standing pose,
and confirmation flourish. A 4:3 capture must be non-empty and use the full
presentation surface. Failed runs preserve their isolated evidence directory;
successful temporary runs clean it up.

```bash
MDKR_AUDIO=0 python3 tests/check_bonus_character_select.py \
  --build build-wizpig --rom baserom.us.v80.z64 -v
```

The check complements, rather than replaces, the deeper Taj picker and
playability journeys. It specifically protects the shared expandable-roster
layout and the two new ROM-authored presentation assets. It is registered as
`bonus_character_select` in `tools/run_checks.py`.

## Independent custom-character browser — `tests/check_custom_character_roster.py`

This real WebGPU gate generates a tiny license-clean GLB and portrait, packages
and transactionally installs them into a temporary isolated character catalog,
then enters the actual custom-racer browser through its controller route. It
requires the runtime catalog trace, opaque modal coverage, centered portrait
pixels, a clean exit, and no fatal/sanitizer marker. It never reads or changes
the player's normal character library.

```bash
python3 tests/check_custom_character_roster.py \
  --build build-character-tests --rom baserom.us.v80.z64
```

The gate is registered as `custom_character_roster` in `tools/run_checks.py`
and is serialized with the other native GPU/pixel checks. Its generated package
uses a Greek display name and Cyrillic short name, and the runtime trace must
prove both entered the ROM-independent native path. The dedicated pure C glyph
test covers deterministic RGBA output, kerning/fitting, transparent-edge
hygiene, supported LTR scripts, explicit shaping/missing/invalid fallbacks, and
shutdown/reinitialization. The roster companion also proves the shared retail-
font projection: printable ASCII survives exactly;
common Latin diacritics, ligatures, attached combining marks, typographic
punctuation, and full-width ASCII fold predictably; each remaining valid
unsupported codepoint becomes one fallback cell; malformed UTF-8 and
unterminated input fail boundedly; controls become spaces; and output truncation
is explicit. The rendered Portrait Studio gate requires the same shared engine
path to report display/short mode, reason, folded, and fallback counts before
Build, including an emoji case that must not masquerade as native text.

## Custom-character identity surfaces — `tests/check_custom_character_identity_surfaces.py`

This real WebGPU gate generates a CC0 package with a Bumper gameplay donor,
four unmistakable portrait quadrants, authored display/short/narration/sort
names, and a distinctive minimap colour. It carries the launcher's engine
handoff contract through the production character-select transaction and a
complete Ancient Lake Time Trial. Runtime witnesses must retain Bumper as
gameplay authority while resolving the package's minimap colour and 40x40 card
for the exact player. Pixel checks then require the authored marker colour in
the real race minimap region and all four portrait quadrants on the real
race-times page.

```bash
python3 tests/check_custom_character_identity_surfaces.py \
  --build build-character-tests --rom baserom.us.v80.z64
```

The gate is registered as `custom_character_identity_surfaces` and serialized
with every native GPU/pixel check. Its temporary package, catalog, save, input
script, and captures never touch the player's normal library.

## Collection-arena custom portrait — `tests/check_custom_character_flag_portrait.py`

This real-ROM WebGPU gate generates and installs a CC0 package with a Bumper
donor and an unmistakable package-owned 40x40 card, then enters Fire Mountain's
actual egg challenge. The lazy `BHV_CHARACTER_FLAG` binding for player one must
name the exact package card rather than Bumper's retail portrait. Two otherwise
identical runs differ only by the production portrait-draw suppression control;
matched framebuffer samples must differ by a full portrait-shaped region. This
isolates real painted flag pixels from the custom model, HUD, course, and race
simulation. The installed package inventory is hashed before and after both
arms.

```bash
python3 tests/check_custom_character_flag_portrait.py \
  --build build-character-tests --rom baserom.us.v80.z64
```

The gate is registered as `custom_character_flag_portrait` and GPU-serialized
in `tools/run_checks.py`. Its source, catalog, save, logs, and captures are
temporary unless `--evidence-dir` is supplied.

## Raw source Workshop intake — `tests/check_character_raw_intake_ui.py`

This ROM-free WebGPU gate first creates and drops a canonical `.mdkrsource`
artifact with a self-contained GLB, exact original-source and conversion-
settings bindings, external adapter identity claim, and exact license bytes.
It requires mutation-free review, explicit unsigned-adapter disclosure,
separate acceptance, exact reviewed-digest revalidation, collision-free
exclusive GLB/license/provenance extraction, rights prefill without approval,
and the ordinary resumable draft/inspection handoff. A second process
keyboard/speech-walks that review at 200% in a 640x480 window and proves review
alone extracts nothing. Hostile unit fixtures reject unknown/duplicate/symlink
members, duplicate JSON keys, digest changes, review races, and every sibling
destination collision without partial output. The reference adapter packer is
also proved deterministic and explicitly does not report an authenticated
converter.

The same ROM-free WebGPU gate then drops a nested authoring ZIP containing one
generated license-clean DAE, requires bounded conversion to an exclusively
created self-contained GLB, confirms the missing embedded-license disclosure,
and verifies the ZIP stays byte-identical and no package/cache is published. It
also keyboard-walks the explicit converted-destination workflow. Before
conversion, it drops uppercase FBX, OBJ, Blender, JSON glTF, USDZ and native
DCC fixtures and requires each to route to its format-specific GLB export lane
instead of ROM/package validation. One lane is keyboard/speech walked through
the copyable checklist; every lane must leave the source byte-identical and
create no draft, candidate, output, or installed state. The gate then
drops two distinct generated, license-clean, self-contained GLBs onto one real
launcher profile. It requires Workshop routing,
bounded model inspection, two independently authenticated source drafts, durable
selection, an independently identified same-source branch, source-fingerprint-
bound fallback/seat/head mapping restoration, explicit switching and exact
deletion, durable close/resume navigation that does not monopolize installed
editing, legacy singleton migration, and rendered captures without publishing a
package candidate or runtime cache. A fresh
process and every switch require reinspection, so stale model bytes never inherit
an in-memory fingerprint. A checksum-corrupt inventory is rendered read-only and
must remain byte-for-byte untouched. Hostile unit fixtures prove per-member and
aggregate ZIP expansion-ratio rejection before reads and unsupported-compression
rejection. The rendered lifecycle supplies an otherwise complete draft with a
malformed SPDX expression, requires the exact inline parser refusal, and proves
that neither a review candidate nor an installed cache appears. The ROM-free
probe corpus also mutates buffer lengths, view bounds/stride/targets, accessor
types/counts/offsets/sparse flags, POSITION metadata and floats, indices,
animation inputs/outputs, inverse binds, and shared/image views. Every case must
return an actionable policy error without an exception. Scene cycles, affine
shear, hostile material/name/texture fields, misleading required extensions,
bad PNG checksums, and compressed pixels inconsistent with IHDR dimensions are
also covered, alongside zero normals, invalid tangent handedness, unusable
weights, non-finite exponent syntax, and oversized JSON integers; direct
compiler controls independently recheck storage and input types. A many-error
fixture proves diagnostics stop at 256 exact items plus a
suppression count. The rendered intake drops a zero-count accessor, requires its exact
manager diagnostic, and proves that no candidate or cache appears. The gate
also drives reviewed installation,
requires exact completed-draft/candidate cleanup while preserving a sibling
same-ID source draft, then proves the installed one-LOD Performance workspace
opens the sole candidate without changing source or installed bytes and refuses
to guess after a second same-ID draft is added. It hashes both external GLBs
and the license before and after the lifecycle, keyboard-walks the spoken draft
controls, and renders at 640x480 with 200% UI scale and touch scrolling.

```bash
python3 tests/check_character_raw_intake_ui.py \
  --build build-character-tests
```

The rendered pass also keyboard-walks and speaks the optional LOD profile,
exclusive-create destination, and “Create LOD copy and continue” action; native
geometry generation and four-level compilation are independently exercised by
`character_lod_builder`.

The gate is registered as the `app_character_raw_intake` CTest and the
`character_raw_intake_ui` run-check; it needs neither a ROM nor a community
model.

The underlying data-only format and extraction lifecycle are independently
registered as the `character_source_adapter` Python CTest and the native
`character_adapter_output_index` CTest. The public frozen contract and
reference pack/inspect commands are documented in
`docs/architecture/character-source-adapter-contract.md`.

## Character-work shutdown lifecycle — `tests/check_character_quit_lifecycle_ui.py`

This ROM-free rendered gate drops a generated, license-clean GLB into the real
launcher and deliberately holds the real background manager long enough to
request window shutdown while work is active. It requires a visible,
cancellable finishing card, disables Play/import, services the result after
navigation, and permits Quit only after no worker result remains unpublished.
It also proves that the external GLB stays byte-identical and no private
launcher result file is stranded in the isolated catalog. The native
`character_async_job` test independently proves single-operation exclusion,
result publication, exception transfer without `std::terminate`, and the final
destructor join.

```bash
python3 tests/check_character_quit_lifecycle_ui.py \
  --build build-character-tests
```

The rendered gate is registered as `app_character_quit_lifecycle`; it needs no
ROM, network, or community model.

## Workshop tool history — `tests/check_character_workshop_history_ui.py`

This WebGPU gate generates and installs an isolated CC0 fixture, then opens the
exact saved Overview, Package, Identity, Rig & Motion, Gameplay, Offset Studio,
Performance, and Test tabs. The 200% Overview route requires each unfinished
readiness area and the header next step to announce the workspace they open,
a real Workshop operation to leave an acknowledgeable message in the recent-
status stack, and the Package route requires the vehicle echo to name Offset
Studio; no tab may render another tab's undo/redo controls. The
`character-workshop-ux` line reports only what it computes -- layout, the
tab-bar popup flag read from the helper the tab bar uses, and the depth the
status stack keeps -- plus four route names for the un-gated P1/P4/P5 polish
items. Static Workshop copy no speech walk can report -- the recent-status
separator, the focused-route undo chords, the plain-language fit status, the
keyboard/controller expectation, and the player-to-controller-port line -- is
pinned against ui_settings.cpp, literally for the player-facing strings and by
whitespace-insensitive pattern for the two structural ones. Positive control:
renaming any of them, dropping a readiness button, or lowering the status
depth fails the gate.
It requires source-digest-bound history controls for Identity, Profile,
Rig, Fit/review, Performance assembly, and Test setup, and verifies that merely
rendering every route leaves the installed source and cache byte-for-byte
unchanged. The 200% keyboard/speech Gameplay route additionally requires all
ten project-owned, colour-independent donor-profile metric badges with no
retail portrait art. The distinct 200% Offset Studio route requires the linked-
ROM exact-preview, disabled-package preview, measured vertical/facing/contact
starting points and honest package-anchor reset contracts plus the
front/side/top placement and contact planes, context yaw, vehicle-only copy
boundary, and Fit undo contract to render. The contact proposal must expose its
exact-state sample counts and XYZ deltas, remain inside the existing safety
envelope, require explicit acceptance, and invalidate stale evidence for a new
exact test. The 200% keyboard/speech Performance route requires
all four named targets, the exact authored-LOD control, runtime-equivalent
source/local-bias selection, merged distance intervals, session-only distance
scrubbing, selected-LOD structural accounting, transition quality warnings,
keyboard/speech access to the plot, and independent Performance history to
render. The Test route also requires all 13 semantic inspection choices,
the safe midpoint phase default, neutral camera/light defaults, exclusive-
capture disclosure, and the explicit session-only/no-performance-evidence
contract to render. `character_edit_history` separately proves discrete edits,
continuous-gesture coalescing, deferred commit, divergent redo invalidation,
and bounded eviction.

```bash
python3 tests/check_character_workshop_history_ui.py \
  --build build-character-tests
```

The rendered gate is registered as the `app_character_workshop_history` CTest
and `character_workshop_history_ui` run-check. It needs neither a ROM nor a
community model. Passing `--rom` additionally requires the rendered donor cards
to transition from their honest no-ROM treatment to exact ROM-derived metric
bars and acceleration signatures; this uses only the game's ordinary verified
base ROM.

## Named-draft transfer — `tests/check_character_draft_transfer_ui.py`

This ROM-free two-catalog gate drives the real Package workspace to save and
exclusively export an exact-source `.mdkrdrafts` bundle, then reviews and imports
it in an independently installed copy of the same generated CC0 character. It
requires the transfer to strip local portrait paths, omit model/package/ROM
bytes, retain exact portrait/editor state, refuse overwrite, expose source and
add/duplicate/collision outcomes before consent, and remain mutation-free at
review. The recipient review is rendered at 200% in the compact 640x480 layout;
a keyboard/speech traversal must reach the compatibility summary before the
rights-gated action. Confirmed import must re-read the reviewed file and add all
new snapshots atomically without resuming, building, activating, assigning, or
changing installed character bytes. A second review must be idempotent, and a
same-id package with a different source digest must fail compatibility without
publishing a hidden draft.

The pure `character_draft_transfer` test independently proves complete-payload
integrity detection, bounded parsing, privacy normalization, exact-base
selection, fail-atomic capacity refusal, deterministic non-overwriting id
renaming, and semantic duplicate suppression.

```bash
python3 tests/check_character_draft_transfer_ui.py \
  --build build-character-tests
```

The rendered gate is registered as `app_character_draft_transfer`; neither arm
requires a ROM, a network, or a community asset.

## Portrait Studio authoring — `tests/check_character_portrait_studio_ui.py`

This ROM-free gate installs a generated CC0 package into an isolated catalog,
generates a non-square local RGBA PNG, and opens the real Identity workspace. It
requires strict source loading, digest/dimension disclosure, square crop,
premultiplied-area or crisp reduction, edge-connected matte removal, background
frames, a non-destructive target-space subject mask with pointer and numeric
coordinate routes, bounded framing/mask undo/redo, deterministic styling,
quality reporting, and an exact-byte-derived native/light/dark/grayscale/
protanopia/deuteranopia/tritanopia readability proof, plus six exact-output
clean/classic/bold/crisp/dithered/soft comparison variants and
selection and palette-replacement surfaces to render at 200% UI scale in the
compact launcher. Each variant preserves framing and cleanup, changes only the
draft style recipe, and remains installed-byte-pure. A keyboard-only
speech walk must announce the consequential controls, and both arms must leave
installed source/cache bytes unchanged. The same arm collects every non-ASCII
codepoint written in a `platform/app` string literal and requires the live
atlas to draw all of them: the app never consults a host font, so a codepoint
outside the embedded subset is a tofu box on every machine. The scan decodes
the literal rather than reading its source characters, so a codepoint written
as `\u2192`, `\U00002192` or as the UTF-8 byte run `\xE2\x86\x92` counts the
same as a pasted arrow -- the byte form is already used in the tree for the
bullet in `ui_overlay.cpp`. It is deliberately naive about context: a literal
on a commented-out line still counts, because one extra probe is cheaper than
a tokenizer whose bugs would be silent passes. Positive control: an arrow put
back into any launcher string in any of those four spellings reports
`missing=2192` and fails the gate. Pure `character_portrait_studio` and
`character_draft_snapshot` tests prove CRC and APNG refusal, mutation-free decode
failure, image operations, bounded provenance recipes, deterministic output,
analysis, opaque background compositing and deterministic screening transforms,
preset invariants, selection semantics, v1-v7 migration, and v8
source-record/mask/top-camera round trips independently of rendering. The shared
`character_edit_history` and rendered history gates retain the existing
source-bound, byte-capped undo/redo contract around those draft fields.

```bash
python3 tests/check_character_portrait_studio_ui.py \
  --build build-character-tests
```

The gate is registered as `app_character_portrait_studio` and
`character_portrait_studio_ui`; it needs neither a ROM nor a community model.

## Exact Character Workshop contexts — `tests/check_custom_character_workshop_preview.py`

This gate generates and transactionally installs a license-clean package with
a Bumper donor into a temporary isolated catalog. It directly starts the real
character-select scene plus baseline Ancient Lake car, Whale Bay hovercraft,
and Windmill Plains plane races without an input script. It also directly
qualifies the dense and alternate routes for every vehicle: Greenwood Village
and Snowball Valley for car, Crescent Island and Hot Top Volcano for
hovercraft, and Spaceport Alpha and Everfrost Peak for plane. The matrix
includes one-, three-, and four-player layouts and
requires the package's non-Diddy donor, one shared WebGPU asset upload, nonzero
modern draws and triangles, zero refused draws, nonempty captures, and real
four-player viewport dividers. Four-player rendering must materially multiply
the one-player character work. Negative arms require invalid context/player
values, a missing package assignment, and an unsupported vehicle to fail closed
with the precise refusal. One positive arm holds `select.idle` at phase
`250/1000` in the exact car scene and requires nonzero runtime held-pose ticks
with zero fallback ticks. A paired authored-only
`race.finish_win` arm requires every inspected tick to report source fallback,
proving the result distinguishes an unavailable semantic from exact phase
control.
Additional negative arms reject an unknown semantic, an out-of-range phase,
and a semantic/phase pair with one member missing. A further positive arm holds
`select.idle` at phase `500/1000`, applies a 180-degree yaw/15-degree pitch
absolute racer-relative fitted-bounds orbit and bright character-only light,
suppresses the scripted camera bank, requires nonzero warmed camera/light
application counts plus 12 consecutive eligible rendered frames, and proves
both exclusive output-sized PNG products. The gameplay product is canonical RGB
and contains the composed world, vehicle, character, and HUD. The model-only
product is canonical RGBA from the isolated WebGPU replay; it contains a
nonempty centered character over real transparency, excludes scene matte
colors, and has zero RGB in every zero-alpha pixel. The gate decodes every PNG
filter and requires the generated character's contiguous material component to
be large, unclipped, and inside a central safe frame. Negative visual arms reject unpaired fields, out-of-range
yaw, unknown lighting, a select-camera orbit, visual fields or capture in live
mode, uppercase capture suffixes, and an existing destination; the last must
remain byte-identical.

Paired model-only arms also drive exact +90-degree top and -90-degree underside
pole cameras against a closed, skinned tetrahedral fixture. Both must retain a
stable yaw-defined screen orientation and a nonempty centered 3D subject; this
prevents an edge-on planar fixture from being mistaken for a camera failure or
an oblique 89-degree approximation from passing as a true top view.

Every valid arm also enables the production presentation census. It must begin
only after the 120-authored-tick warm-up and emit a bounded structured result
with at least 40 post-warm-up intervals and nonzero modern-character
replacements. The headless arm must identify its synthetic pacing so those fast
harness intervals can never be presented as a player/device performance result.
It must also report a stable WebGPU backend, adapter, driver, vendor/device ID,
physical output size and actual scene-render size. With RenderScale 1, output
and render dimensions must agree across every arm and with the captured PPM;
the gate intentionally accepts the host's real HiDPI drawable rather than
mistaking logical window pixels for the comparison environment. Every valid
arm must also publish a normalized target-frame forward direction, an ordered
calibrated vertical volume, and a ground/seat anchor at automatic zero from the
actual replacement transform. The select volume may not penetrate the roster
floor beyond the five-millimetre numerical tolerance.
The one-player and four-player model captures must replay the same complete
player-0/view-0 primitive set. Each retains one exact target-to-clip witness,
aspect-fits that selected viewport into the full output without distortion, and
publishes bounded fixed-point projections for eight calibrated bounds corners,
the fitted anchor, and the forward endpoint. The gate installs a second
reviewed-humanoid package and drives a real car arm through automatic contact
solving. The current result-v23 contract must publish all four bounded
contact witnesses; the gate independently recomputes the left-hand
target-to-endpoint distance from quantized coordinates and rejects partial,
detached, or fabricated select-context evidence. Authored-clips-only arms must
publish an explicit zero mask when no automatic solve owns the pose.

The bounded semantic-review arms require the exact three-state selection-room
battery and all eleven race semantics on each of the car's open, dense, and
alternate-environment courses. Every sample waits for the engine-owned settled
pose witness plus 60 complete replacement draws, publishes framing, contact,
retained-vehicle, and visibility evidence, and then returns automatically.
Unknown scenes and vehicle-only scenes requested for character select fail
closed. ROM-free rendered and pure-policy tests independently pin the launcher's
one-action progression, resume, full-refresh, retry, stop, and approval rules.

Every valid arm must additionally publish a completed asynchronous opaque-depth
witness. The renderer replays player 0's exact current primitive set into an
isolated depth target and against the final scene depth over the same 8 x 8
screen grid. The gate verifies exact boolean masks, popcounts, scene-subset
semantics, and material draw classification without claiming pixel precision.
A successful all-zero query remains valid failing visual evidence: it
distinguishes a fully clipped, back-facing, or alpha-rejected subject from an
unavailable GPU readback. Alpha-blended materials are structurally valid but
unqualified because they do not have one portable opaque-depth meaning.
Two fault-injection arms own the optional-allocation boundary. A one-shot
isolated-depth failure must be released, retried, and end with qualified exact
visibility. A persistent diagnostic-pipeline failure must make exactly the
Workshop's three bounded attempts, publish explicit unavailable visibility, and
still complete the playable session without a device or renderer fatal error.
The renderer creates those query, buffer, texture/view, and diagnostic-pipeline
resources inside asynchronous validation and out-of-memory scopes and encodes
the replay only after both callbacks succeed.

Every arm must also publish the versioned GPU timing contract. One arm
force-disables it and must remain explicitly unsupported with no values. A
device without timestamp queries must do the same naturally. A supporting
device must return exact scene-pass timestamps with monotonic nonzero
percentiles and at least two-thirds usable post-warm-up frames. At most the six
readback slots may remain pending; usable, pending, and ring-full counts may
never exceed the wall sample, and every remaining scene frame must be covered
by the reported invalid-readback count. A device without in-pass timing must
balance that partition exactly. Character-draw values are required only when the device exposes
native in-pass timestamps and are forbidden otherwise; wall cadence is never
accepted as a substitute.

The pose arm is intentionally not performance qualification. The Workshop
keeps it as session fit evidence and displays replacement/contact observations,
but the production publish boundary refuses to write it into the durable 4x4
latest/baseline matrix. Live game-driven animation remains the only evidence
eligible for timing comparison.

```bash
python3 tests/check_custom_character_workshop_preview.py \
  --build build-character-tests --rom baserom.us.v80.z64
```

The gate is registered as `custom_character_workshop_preview` and GPU-serialized
in `tools/run_checks.py`. The ROM-free `app_lifecycle` CTest separately proves
that its scoped launcher environment handoff restores both absent variables and
exact caller-supplied values after repeated writes.

## Durable Character Workshop test evidence — `tests/check_character_test_evidence_ui.py`

This ROM-free rendered gate installs a generated CC0 animated package and
exercises the Test workspace's durable 4-context by 4-player-layout matrix. It
publishes a qualified Car 4P result through the production result boundary,
checks exact source/fit/LOD/presentation fingerprints plus timing, device and
physical-dimension fields on disk, and checks the v3 signed target-frame bounds,
ground/seat anchor, normalized facing, and four hand/foot chain witnesses from
that replacement draw. It
restarts the launcher, requires the same fit diagnosis to remain available, and
pins the result as an explicit comparison baseline. A 200% compact keyboard
speech walk must announce the qualified cell and both baseline controls. A
separate 200% virtual-controller D-pad walk renders transition mode and must
reach all four responsive player-layout choices, both review-mode choices, both
named semantic endpoints and numeric phases, camera values, character light,
and every enabled inspection action through SDL and the production ImGui input
backend. Capture controls must stay outside that focus graph while transition
review disables deterministic stills.

The second half clears only that baseline while preserving latest evidence,
then clears the package's complete local evidence inventory. Finally it corrupts
the authenticated store and requires the Workshop to become read-only without
rewriting one byte. Every arm hashes the installed package before and after so
test bookkeeping cannot mutate character source or compiled cache bytes. The
generated package has multiple authored LODs; one rendered arm publishes a
valid result, changes only the persisted local LOD policy, and requires the
result to become visibly stale. The older fake-fit-digest arm remains as an
independent rejection case. The
empty-inventory arm also publishes a fully rendered held-pose inspection
through the production result boundary and requires its session result to stay
available for fit review while the durable evidence file remains byte-exact.
Its paired fallback arm proves that unavailable semantic motion is reported but
cannot unlock fit approval, while still leaving durable evidence byte-exact.
The rendered keyboard/speech arm publishes a successful transparent model-only
camera/light/capture inspection into the session report tray, decodes and draws
its bounded digest-bound thumbnail, announces descriptive preview text, and
requires both render-product choices, portrait handoff, removal, report path,
exclusive export, and list-clear controls to remain reachable at 200% scale.
Another rendered arm publishes two distinct bounded RGB scene fixtures as a
digest-bound registered donor/custom pair, opens the actual report-tray blend
control, and requires its exact-grid accessibility trace. The pure report unit
and linked-ROM gate separately prove refusal on witness drift and production
renderer agreement, so this UI smoke does not manufacture renderer authority.
The complete over-budget arm also accepts the explicit test-only sharing token,
exports a real schema-v1 device profile through the production UI, parses all
16 honest warning rows, and proves the output retains host/GPU/driver/build,
resolution, cost, wall, and optional GPU timing while excluding package ID,
display name, character directory, and every raw source/fit/presentation
digest. Production users must separately check the visible identity disclosure;
the exporter never uploads or overwrites a file.
The pure `character_visual_report` test independently proves embedded PNG/JSON
output, capture-time digest binding, same-size/same-product file-replacement
refusal, rebinding refusal, HTML/script escaping, source-path privacy, PNG
completeness/dimension/product checks, schema-v5 scene/model-alpha plus
custom/retail-donor subject identity and fixed
projection metadata, integer-space registered SVG overlay, checkerboard
transparency presentation, and overwrite refusal;
`character_preview_cache` proves the Offset Studio no-filename path derives
distinct bounded two-slot custom-scene, model-alpha, and retail-donor products
per package/context, preserves the last published product until its replacement
binds, preserves unrelated files, cleans only
regular non-link owned files, rejects cross-package ownership and hostile links,
and leaves the renderer destination absent for exclusive PNG creation;
`workshop_preview_runtime` proves visual bounds, donor-reference enable/reset
and qualified-batch counting, failure-without-mutation, and measurement epochs.
The linked-ROM gate separately requires the donor route to preserve the
package's selected donor, produce zero modern replacements and nonzero qualified
donor-character batches, capture a nonempty composed RGB frame, and auto-return
after 12 stable frames. It then captures the custom half at the same held pose
and camera, requiring identical fitted anchor/bounds/forward/head and camera
bounds/viewport/head witnesses plus the same RGB pixel grid; this exercises
both renderer halves of the launcher's registered overlay contract.
The pure `character_test_evidence_store` unit separately covers canonical parsing,
whole-inventory and row authentication, strict unsigned/signed numeric and UTF-8
bounds, renderer-fit/contact-witness invariants, exact key replacement,
authenticated v1 through v8 loading and next-write v9 migration, the
64-package/2048-record limits, transaction
failures, honest baseline comparability, and complete-matrix device-profile
summary/export refusal for partial, stale, mixed-device, invalid-cost,
wrong-suffix, and existing-destination inputs. The durable filename intentionally
remains `character_test_evidence-v1.tsv`: its authenticated header selects the
schema, allowing existing v1 stores to be discovered and migrated rather than
silently orphaned.

```bash
python3 tests/check_character_test_evidence_ui.py \
  --build build-character-tests
```

The rendered gate is registered as `app_character_test_evidence` in CTest and
`character_test_evidence_ui` in `tools/run_checks.py`; it requires no ROM.

## In-window file intake backends

`file_browser_ui` drives the real ImGui browser with synthetic gamepad events
at 100% and 200% scale. It checks initial focus, folder entry, literal `##`
filenames, file selection, Escape cancellation and finite draw vertices.
Accessibility speech labels are observed through a stub; it does not validate
device speech, GPU pixels, the primary launcher action or Linux packaging.

`rom_browser`, `pack_installer`, `pack_installer_path` and `pack_install_job`
are ROM-free CTest targets. Browser coverage includes case-insensitive
filtering, directory ordering, navigation and failed directory recovery.
Installer coverage includes manifest/Rice ZIP discovery, invalid ZIP refusal,
Unicode filenames, exclusive publication under races, move/cancellation
cleanup and dangling destination symlink refusal. A non-pack ZIP or folder is
refused before anything is staged, with only the intake cancellation check
reached. A ZIP inside a folder the process may enter but not list still
installs; a mods folder that is a link works; abandoned staging is reclaimed
while a live installation's is not; each refusal message names its cause.
Cancellation coverage includes a request before staging and after the first
256 KiB copy chunk, with no publication or surviving partial staging.
Folder cases cover nested content, existing and concurrently-created empty
destinations, self-containing mods paths, cancellation, links inside the pack
and links planted in staging. `pack_installer_path` runs the same fixture
against the pathname installer Windows ships; descriptor-identity cases are
compiled only into `pack_installer`. `pack_install_job` drives the launcher's
install job frame by frame. These are host-specific publication tests: macOS
success does not qualify the Linux exclusive-rename syscall or Windows
MoveFileExW.

```bash
cmake --build build-rel --target mdkr_rom_browser_test mdkr_pack_installer_test \
    mdkr_pack_installer_path_test mdkr_pack_install_job_test
ctest --test-dir build-rel -R '^(rom_browser|pack_install.*)$' --output-on-failure
```

`mdkr_pack_installer_test` optionally accepts a local ZIP path as its sole
argument. It copies and validates that fixture in disposable temporary storage,
checks the published length, and cleans up without modifying the original or
live mods directory. Backend success does not establish rendered controller
navigation, pack rendering/performance or Linux/Deck package readiness.

## Playable Wizpig race voice — `tests/check_wizpig_race_voice.py`

Runs the real Hot Top Volcano plane route with playable Wizpig, isolated saves
and muted device output (synthesis remains active). Independently pinned retail
sound IDs must own both positive zipper reactions and negative reactions; the
gate rejects missing event coverage, donor cues, consecutive repeated cues and
sanitizer/fatal diagnostics. It checks dispatch identity, not perceived sound
quality, retail boss encounter voices or the boss-footstep report.

```bash
python3 tests/check_wizpig_race_voice.py --build build-rel \
  --rom baserom.us.v80.z64
```

Registered as `wizpig_race_voice` in `tools/run_checks.py`. A bounded 8,000-frame
run uses a 240-second process timeout. Logs and saves stay in a temporary
evidence directory (printed on completion), or `--evidence-dir`; do not include
private ROM-derived evidence in source/release archives.

## Bonus results portraits — `tests/check_bonus_results_portraits.py`

This real-ROM gate completes a time-trial post-race flow once as Wizpig and
once as Terry. Each arm requires a project-owned 40x40 card at the results
portrait seam, captures the real race-times page, and checks the card's visible
identity palette (Wizpig's red/gold boss portrait; Terry's green pterodactyl
portrait and yellow beak). This prevents either virtual racer from silently
falling back to Krunch anywhere the shared results portrait lookup is used.

```bash
python3 tests/check_bonus_results_portraits.py --build build-wizpig \
  --rom baserom.us.v80.z64
```

The gate is registered as `bonus_results_portraits` in
`tools/run_checks.py`.

## Bonus portrait packs — `tests/check_bonus_portrait_pack.py`

This real-ROM gate proves the three generated bonus-racer portraits are
replaceable through the ordinary Content Packs texture path, and that their
published digests do not drift. Nine runs of the same post-race flow
`check_bonus_results_portraits.py` uses: a no-pack baseline with
`MDKR_MOD_TEXTURE_DUMP` on, one pack arm per racer overriding that racer's
pinned digest, the identical pack switched off by its own `pack.ini`, and the
baseline and Taj pack arms again on WebGPU — the renderer that ships, so a
digest published from GL alone would be a name most players never produce.
Two further arms use the pre-1.7 Taj filename on GL and WebGPU, proving that
retouching the generated card does not silently invalidate existing packs.
The ROM-free `mod_texture_store` test pins current-name precedence, disabling
aliases with the pack toggle, and isolation from unrelated texture digests.
It also requires bounded PNG header admission before pixel decoding, rejects
unreadable headers without entering the decoder, and verifies that returned
dimensions agree with admission. The dimension-mismatch controls change only
the test wrapper's metadata after decoding an ordinary small PNG; a matching
image must still load with its exact pixels.
Its last case is about the author dump's `<digest>.txt` record and the
`<digest>.texels` companion beside it, field by field: those are parsed by
`tools/ricepack/`, so a field that is absent, misnamed or truncated becomes a
confident wrong CRC offline rather than a visible failure. The fixture's source
span is deliberately padded — a 24-byte pitch over 16-byte rows — so a dump that
wrote a tightly packed span, or reported the packed pitch, fails here instead of
passing on an unpadded fixture and mis-hashing every real tile. The record is
versioned (`dump_format`), and a record with no version line is version 1, which
is how a corpus dumped before the field existed stays readable.
The four first-party PNG callers also carry compile-time bounds for 16-bit
intermediate storage, including callers that request 8-bit output. Raising
their dimension/cache limits beyond those bounds must fail compilation until
the decoder-size contract is reviewed; this is independent of test execution.
Every arm proves it got the backend it asked for, so a silent adapter fallback
cannot pass as WebGPU evidence. Frames are not compared across backends: two
rasterizers need not agree byte-for-byte and nothing here claims they do, while
the digest is fixed before either backend sees the texels.

The pack image is the synthetic magenta-corner-on-green quadrant PNG imported
from `check_mod_texture_override.py`, at 64x64 — not a solid colour, so the
arms witness texture addressing rather than only presence, and not derived from
any game art. Under a pack the card region is entirely the pack's two colours,
green-dominant three to one; beside the card, zero pack pixels; with the pack
switched off, the frame is byte-identical to the no-pack baseline.

The Taj pack arm additionally runs with the dump on — a pack installed *and*
`MDKR_MOD_TEXTURE_DUMP` set is the one combination that reaches the dump's
encode path for an override, and no other arm or gate produces it. The written
PNG must be 64x64 and pixel-identical to the pack's own image. It pins a fixed
defect: the renderer passed the pack's 64x64 buffer with the tile's logical
40x40 size, so stb encoded 40x40 at a 160-byte stride out of a 256-byte-stride
image — in bounds, no crash, no sanitizer report, just a sheared picture handed
to the author who asked what the game drew.

The digest assertion is the point of the file. A ROM texture's digest is frozen
because the ROM is; these three are a function of `game/src/menu.c`, so
retouching the artwork renames them and silently breaks every pack in the wild.
The baseline arm reads the dumped corpus and fails — naming the digest the art
now produces — when the pinned name is no longer the one a pack author would
find. A one-channel change to a single pixel of Taj's card is enough to trip it;
nothing else in the suite notices.

```bash
python3 tests/check_bonus_portrait_pack.py --build build \
  --rom baserom.us.v80.z64
```

The published digests also appear in [`docs/MODDING.md`](../docs/MODDING.md);
the two must be changed together. The gate is registered as
`bonus_portrait_pack` in `tools/run_checks.py`.

## Terry flight audio — `tests/check_terry_flight_audio.py`

This real-ROM gate runs Terry on the same course in a plane and kart. The plane
must emit the retail Terry/Smokey sound 547 on the fly animation's exact 28→32
subframe crossing, maintain the authored 48-tick cadence without drops, and
keep both continuous engine and idle handles stopped. It also proves Terry uses
Pipsy's light physics-stat row with the 1.03x performance contract and that the
plane donor's ordinary/boosted wing-line emitter bits are filtered. The kart is
the negative control: it must emit no flight-only audio or particle witness and
retain Krunch's ordinary engine loop. This protects the presentation-only policy
from muting item, collision, surface, or ground-vehicle paths.

```bash
python3 tests/check_terry_flight_audio.py --build build-wizpig \
  --rom baserom.us.v80.z64
```

The gate is registered as `terry_flight_audio` in `tools/run_checks.py`.

## Taj entrance theme -- `tests/check_taj_theme.py`

This real-ROM gate starts a new Adventure, summons Taj by honking near him, and reads
the native MIDI event trace for entrance sequence 32. All 13 channels that
contain authored notes must sound, with no channel-disabled rejection inherited
from the hub's dynamic music mix.

```bash
python3 tests/check_taj_theme.py --build build --rom baserom.us.v80.z64
```

The gate is registered as `taj_theme` in `tools/run_checks.py`.

## Adventure trophy series — `tests/check_trophy_series.py`

`check_trophy_adventure_two.py --build BUILD --rom ROM` separately selects
Adventure Two through its menu and exercises Future Fun Land's championship.
It checks all four mirrored race loads, production gold award and cabinet
display, plus checksum-valid EEPROM trophy bits and Adventure Two identity.
The fixture uses the shared championship completion/order seams; it is not a
manual four-race drive or a fresh-process Future Fun Land cabinet reload.

`check_trophy_ffl_reload.py --build BUILD --rom ROM` covers that fresh-process
reload in both Adventure modes. Its starting checkpoint supplies other-world
golds, campaign prerequisites and already-seen Taj offers, but no FFL trophy.
Production awards FFL gold, then a second process receives the exact EEPROM
bytes without rewriting fields and follows the lighthouse route to the cabinet.
It checks the race schedule/mode, award transition, both saved checksums and
campaign identity, hub load and cabinet display. Controls reject unearned
trophy bits and missing load/display witnesses. As with the series gate,
championship timing and finish ordering use existing test seams.
The cabinet DISPLAY witness is emitted only after successful trophy object
creation. It is not a pixel-level visibility or occlusion check.

```bash
python3 tests/check_trophy_series.py -v
```

This starts from a checksum-valid checkpoint with the Dino Domain cabinet
legitimately unlocked, enters it through production collision/dialogue code, and
drives all four championship rounds. Additional arms select the other four
world schedules at the same production entry boundary. Assertions cover all 20
tracks, per-round point accumulation, a stable 32-point tie, gold/silver/bronze
and no-award finals, the trophy cinematic, the rankings QUIT path, post-quit
retry, checksum-valid EEPROM, and a fresh-process cabinet display from reload.

The Future Funland arm added for issue #63 requires a 36-point gold across
Spacedust Alley, Darkmoon Caverns, Star City and Spaceport Alpha, persisting
`0x300` in the existing ten-bit trophy field. Its world selector changes only
the series world at the existing entry boundary; it is not evidence of driving
to Future Funland's own cabinet. The existing fresh-process display check is
for Dino Domain. Future Funland cabinet/Tracks display after restart, existing
saved trophies, Adventure Two and no-downgrade behavior remain required release
acceptance. The extended arm is pending execution.

The ROM-free `runtime_contracts` unit exhausts all 1,024 ten-bit trophy values
across the five championship worlds and retains invalid-world/null-output
rejection. This replaces an incorrect expectation that world 5 was invalid;
it does not change the four-mainland-trophy rocket unlock requirement.
The same unit checks medal merging as per-world maximum (bronze plus silver
must not become gold), including all rank pairs, neighbors, identity and gold
dominance. `save_codec` additionally round-trips every ten-bit trophy value
while preserving the other save blocks. `runtime_safety` checks that the
production Tracks merge and five-world status counter retain these helpers,
with its existing removed-fragment controls. These additions await execution.

`MDKR_TROPHY_COMPLETE_AFTER` advances lap state only after a real race has run;
`race_check_finish()` still creates the finish order. `MDKR_TROPHY_ORDER` accepts
only a full permutation and runs at rankings init; malformed input is rejected.
Neither control writes points, ranks, trophy bits, cinematics, or save data.

## Save-file fail-safe — `tests/check_save_failsafe.py` (RUN THIS AFTER ANY SAVE/EEPROM CHANGE)

`save/eeprom.bin` is the one piece of **persistent untrusted input** in the port.
On the web build it lives in IndexedDB, so whatever is in it comes back on every
reload — which makes "a bad save puts the game somewhere the player cannot leave"
a permanent trap rather than a one-off.

```bash
MDKR_AUDIO=0 python3 tests/check_save_failsafe.py -v     # ~30 s, muted + headless
```

Four cases, each starting from a wiped `save/` (and wiping it again afterwards — a
leftover save sends FILE SELECT down the resume path and breaks
`check_adventure_hub.py`):

| case | image | asserts |
|---|---|---|
| 1 | 100 of 512 bytes (a torn store) | title screen at ~1134; only the three boot levels loaded; `save/eeprom.bin.bad` byte-identical to the input; `eeprom.bin` back to 512 bytes |
| 2 | 512 random bytes | as above |
| 3 | `wizpigAmulet=7` in slot 0 (checksum **valid**), real started slots in 1–2 | exit 0, no `[CRASH]`, FILE SELECT reached, slot 0 erased, **slots 1–2 untouched** |
| 4 | no save file at all | clean boot and **no** `.bad`/`.tmp` — this is what keeps 1–2 from passing vacuously |
| 5 | `tajFlags = 0x08` (car challenge **beaten but never offered**) + 5 balloons | the kart still drives Timber's Island: path >= 25000 units and <= 45 % of post-hub rows stationary |

Case 3 is the one that matters most: the slot checksum is a plain 16-bit byte sum,
so corrupt bytes satisfy it by chance, and `wizpigAmulet` is 3 bits on disk while
the amulet has four pieces. Values 5 and 7 reach `objects.c`'s
`BHV_DYNAMIC_LIGHT_OBJECT_2` spawner as an unclamped `modelIndex` and SIGSEGV in
the racer collision path ~750 frames into Timber's Island. 4 of 40 randomly
corrupted checksum-valid images hit it before the fix; 0 of 40 after.

Case 5 is the one actually hit in play. `tajFlags` holds "Taj has OFFERED this
challenge" (0x01/0x02/0x04) and "you have BEATEN it" (0x08/0x10/0x20) as two
triples, written minutes apart by two different functions with two different save
flushes. The offer gate (in `objects.c`) reads only the OFFERED half and the
auto-offer dialogue never asks, so "beaten but never offered" replays a finished
challenge on every hub entry — and Taj's dialogue holds `disable_racer_input()`
down, so the player just stops. It is measured by DRIVING, not by exit code:
healthy 37767.6 units / 24 % stationary, wedged 14439.4 / 76 %.

⚠️ Three separate fixes are under this check — `eeprom_load()`'s validation in
`platform/stubs_dkr.c`, and the amulet rejection plus the BEATEN-implies-OFFERED
repair in `game/src/save_data.c`. Revert them **one at a time** when
positive-controlling: they fail different cases (1–2, 3, and 5), and reverting more
than one at once hides which you broke. Measured failures for each are tabulated in
`docs/OPEN_ITEMS.md` "savefailsafe".

**The symptom as first reported — "boot drops me straight into a race" — was NOT
reproduced, and cannot be caused by a save file.** (The report was later corrected to
"a genie test we already completed" after normal menu navigation, which is case 5
and does reproduce; the boot-path disproof below is what rules the boot path out.) `menu_file_select_loop()`
calls `init_racer_headers()` (which zeroes `settings->courseId`) three lines before
the resume returns, so `get_track_id_to_load()` always yields levelId 0, and
`courseId` is not in the save format at all. 98 boots across every corruption shape
reach MENU_TITLE at frame 1134. If you see a race load early in an *idle* boot, it
is the ordinary title attract demo (`levelId=18` @~5132, `levelId=28` @~6632,
`numPlayers=0 cutscene=100`), which exits on START.

## 100%-save entry — `tests/check_save_100_entry.py`

```bash
python3 tests/check_save_100_entry.py --build build-rel   # ~2 min
```

Owns one defect class the fail-safe check above structurally cannot see: a save
that decodes, inspects and *renders* correctly but that FILE SELECT refuses to
enter. The 100%-complete file shipped beside the RC did exactly that — pressing
A on the slot played a buzzer and nothing else.

The cause was not packing. `tools/mdkr_save_make100.py` set every defined
cutscene bit, `CUTSCENE_ADVENTURE_TWO` among them, while leaving the global
"Adventure Two unlocked" config bit clear. `fileselect_input_root()` in
`game/src/menu.c` (case 0) silently refuses a slot whose `isAdventure2` marker
disagrees with the GAME SELECT option in effect, and GAME SELECT never offers
the Adventure Two option unless the config bit is set — so the slot was
unreachable from either option.

| arm | image | asserts |
|---|---|---|
| good | `mdkr_save_make100.py` output, driven through GAME SELECT's Adventure Two option | a `level_load` past FILE SELECT — the slot is actually entered, not merely drawn |
| broken (positive control) | same cutscene flags, global AT2 config bit cleared | FILE SELECT is reached and entry is then refused, reproducing the shipped defect |

The positive control is the point: `mdkr-save inspect` and a headless boot both
pass on the broken image, because each only decodes the slot and neither drives
the input gate. Only the entry route separates the two.

## Sprint ROM-free units — eleven new CTests

```bash
ctest --test-dir build --output-on-failure \
  -R '^(mod_manifest|mod_registry|mod_source_zip|mod_texture_key|dev_command|enhancement_registry|a11y_model|save_state_container|update_check|gpu_diagnostics|adapter_policy)$'
```

Eleven CTest units added by the [`docs/sprints/`](../docs/sprints/README.md)
work. All are ROM-free and window-free, all run in milliseconds, and
`rom_free_units` carries them into every lane. They are registered in
`cmake/tests.cmake`; `tools/run_checks.py` does not name them individually
because its manifest cross-check covers `tests/test_*.py`, and these are C.

| Unit | What it owns | The assertion that would otherwise rot |
|---|---|---|
| `mod_manifest` | One `pack.ini`, parsed and validated | Over-long name/author/version is **rejected**, not truncated |
| `mod_registry` | Pack discovery, load order, path resolution | Traversal rejected against a bait file that genuinely exists; equal-priority tie-break pinned to an explicit ASCII case fold, so load order cannot shift with locale |
| `mod_source_zip` | One read interface over directory and zip packs | Shared lexical path validation with existing traversal bait; leaf/ancestor symlink refusal, root identity after replacement and repeated enumeration; POSIX nonblocking special-file rejection. `mod_source_zip_path` runs it against the pathname reader Windows ships, where the root follows its current name instead |
| `mod_texture_key` | The published texture digest | An exact pinned value, plus a `0xaa`-filled union proving padding bytes are not hashed |
| `dev_command` | Console command parsing | `set` refuses any key outside the schema, so a console cannot become an arbitrary-write primitive |
| `enhancement_registry` | The enhancement table and its authority classes | The table and `mdkr_video_key_is_enhancement()` describe exactly the same set |
| `a11y_model` | The accessibility utterance stream | Coalescing is per category, interruption is per priority, and each CRITICAL case is paired with an identical NORMAL case so the exemption is the only difference |
| `save_state_container` | Save-state file format and validation | Truncation refused at all 88 offsets; a header declaring a payload the file does not contain is refused in both directions |
| `update_check` | Version comparison and the once-a-day interval | `1.10.0` beats `1.9.0` — numeric, not lexicographic; a release supersedes the nightly it came from, and a nightly never supersedes a release |
| `gpu_diagnostics` | The `[GPUINFO]` adapter record | Every candidate carries a non-empty reason, and an adapter name containing control characters or quotes cannot forge a line in the block |
| `adapter_policy` | Choosing an adapter and backend | A preference matching nothing falls back and names the string that missed, rather than failing to start; UNKNOWN class ranks between discrete and integrated in both directions |

Each of these was mutation-checked when it landed — the implementation was
deliberately broken and the suite confirmed to fail — because several of them
guard a silent failure rather than a crash, and a check that passes both with
and without the fix is not a check.

Directory-containment follow-up: five new POSIX assertions failed against the
old reader before its fix. Windows reads folder packs by pathname and refuses a
symbolic link, junction or mount point at any depth; `mod_source_zip_path`
runs that reader on POSIX. On Windows a directory link fixture falls back to a
junction when symbolic links need Developer Mode; a file link that cannot be
created prints a SKIP line rather than passing silently. `fs_utf8` checks the
pathname primitives both arms share (entry kinds, listing, exclusive rename,
directory hold) on every host. The Windows executables are built by
`tools/mingw_cross_check.sh --intake-tests`; they have not yet run on
Windows, and the older mutation statement above does not qualify them.

## Draw distance and model detail — `tests/check_enh_draw_distance.py`

```bash
MDKR_AUDIO=0 python3 tests/check_enh_draw_distance.py --build build
```

The assertion that matters is **not** "the frame changed" — it is that the
**live object count per tick is identical** between 100% and 400%. That is what
separates a render cull from an update cull, and it is checked *before* the
hash comparison so a divergence is named rather than merely detected: *"changing
how far the game draws changed which objects exist. The hook is in the wrong
place."*

The sprint warned this could go wrong, and it nearly did. Under `NATIVE_PORT`,
`check_if_in_draw_range()` has **two callers, both authoritative** — they write
`obj->opacity`, which `[SIMHASH]` v3 hashes — and **none in the render path**:
`render_level_geometry_and_objects()` replays the tick's per-viewport decision
out of the route cache instead. Widening that threshold anywhere inside the
function would have been an update-side edit. The setting therefore goes through
a separate read-only predicate that writes nothing and only *adds* draws.

`MDKR_DRAWDIST_TRACE=1` tallies drawn objects per frame, split authored versus
extended. The gate also requires the **authored** count to be identical on every
frame, proving the setting extends rather than re-decides.

Issue #47 added two more arms to the same file. `split100`/`split400` run the
100%/400% pair on `race_2p_split.txt` instead of Time Trial: the only
multiplayer draw-distance reduction is a 0.5x halving that applies solely to
the four-player layout, so this pair proves the setting also reaches the draw
in two-player split-screen, which takes no such reduction. `dd1600` is a third
value on the original Time Trial pair, proving the schema's "Maximum" notch
(1600%, up from 400%) reaches `scale=16.0` at the cull rather than being
silently rejected or re-clamped to the old ceiling.

The LOD arms run on a 4-player split, not Time Trial: a Time Trial has one
racer already at model 0, so the bias has nothing to hold. The census counts
choices the bias actually *changed* after clamping, so a route that stops
exercising the ladder fails rather than passing on an incidentally different
frame.

## Pack music — `tests/check_mod_music_override.py`

```bash
MDKR_AUDIO=0 python3 tests/check_mod_music_override.py --build build
```

Seven headless captures, no audio device, and **no ROM audio anywhere** — the
pack's WAV is a 440 Hz sine the test synthesises, exactly 440 cycles in one
second at 22050 Hz so the loop seam is continuous.

The design constraint the gate exists to hold: the replacement is
**presentation-only**. The sequence player is not skipped — it still posts its
play event and the CSP runs unchanged; the single state change is the player's
volume field, the same one the music slider writes. Skipping instead of muting
would sound identical and break determinism silently, so assertion 5 requires
the `[SIMHASH]` v3 rows *and* the sequence trace to be identical across all
three arms.

Assertion 2 measures a **residual**, not a resemblance: with effects volume at
zero the bus holds nothing but the replacement, so the pack's own synthesised
waveform must fit the capture to 0.006%. Arm C installs the same pack as a
`.zip` with no directory present and requires byte-identical output — which is
what proves the registry now discovers archives.

Arm G is the music half of the **Custom content** setting. It installs the same
pack with `Content.PacksEnabled=0` at launcher rank — where a player's saved
setting sits — and requires the capture to be byte-identical to arm A's: the
pack is found and reported active, and the replacement is never claimed. The
setting used to reach the texture layer alone, so a checkbox named "Custom
content" quietly left the music replaced. What arm G deliberately does *not*
claim is a mid-track cut: the switch is read at `mdkr_mod_music_begin()`,
because muting the sequence player is a one-way redirection and stopping a
replacement partway would leave silence where the game's own music should be.
Its positive control removes that read; arm 7 fails with `with
Content.PacksEnabled=0 the engine still claimed the replacement`.

The positive control stubs the begin hook to return 0; assertions 1–4 all fail.
Assertion 6 keeps passing under that stub, honestly, because it then compares
two equally unmodded arms — recorded in the docstring rather than papered over.

## Pack textures and the live switch — `tests/check_mod_texture_override.py`

```bash
MDKR_AUDIO=0 python3 tests/check_mod_texture_override.py --build build
```

Four headless 380-frame boots at 320×240 on GL, each in its own throwaway
directory. The first runs with `MDKR_MOD_TEXTURE_DUMP` and dumps the
digest-named corpus a pack author starts from; the pack the other runs install
is built from that corpus, so the digest contract is exercised end to end
rather than pinned to a constant that could rot. Every one of those digests is
overridden with a single 4×4 magenta PNG the check encodes itself — overriding
*everything* is what lets whole-frame hashes answer "did any pack pixel
survive" at any frame, and the size mismatch exercises the same UV
normalisation a higher-resolution pack takes.

The gate this file owns is the **live** one. `Content.PacksEnabled` is declared
`SCOPE_LIVE`, and until `mdkr_video_config_publish()` carried it to
`mdkr_mod_texture_set_enabled()` / `mdkr_mod_music_set_enabled()` that
declaration was a promise nothing kept: the key was read once at boot, so the
settings checkbox took effect next launch while `Tab` moved the identical lever
immediately. The fourth run drives
`MDKR_TEST_SETTINGS_TOGGLE=Content.PacksEnabled=0@150,Content.PacksEnabled=1@320`,
which fires `mdkr_video_config_runtime_set()` — the exact call the settings
panel makes, not a rewritten ini and a relaunch — and frames are sampled at
120, 240 and 360, one per state and each ≥30 frames clear of a transition.

**Byte-identical is the assertion that matters.** Frame 240, after the setting
was switched off at runtime, must equal the *no-pack baseline* exactly. Merely
"different" would also be produced by a store that stopped answering while the
texture cache kept serving pack pixels it had already uploaded; only an exact
match proves `override_generation` retired those entries and re-uploaded the
ROM's texels. Frame 360 must equal the pack run again, which is also the
observation that **off→on needs no restart**: the registry is scanned and the
store bound unconditionally by `platform_content_packs_init()`, so the setting
only decides whether the store answers.

Assertion 5 requires the `[SIMHASH]` v3 stream to be byte-identical across all
three comparison arms. `Content.PacksEnabled` is content class and toggling it
mid-run must not move one bit of authoritative state.

There is **no `Tab` arm**, and the docstring says so rather than implying
coverage: `Tab` is an SDL keyboard event, a headless run has no keyboard, and
the input-script fixture format carries controller tokens only. Tab and the
setting call the same lever; the rule between them — an explicit settings
change wins and sets the lever to the setting's value, ending any momentary
comparison — lives as a comment at the publication point and is asserted in
both directions by the ROM-free `video_config_runtime` unit test.

Music differs deliberately and the header says why: the switch is read at
`mdkr_mod_music_begin()`, so it is honoured from the next piece of music
onwards. Muting the sequence player is a one-way redirection, so cutting a
replacement off mid-track would leave silence where the game's own music
should be.

The positive control comments out the two receiver calls in
`mdkr_video_config_publish()`, restoring the original defect exactly — the key
still reports LIVE and the seam still logs `applied=1`. Assertion 3 fails with
`frame 240: … the frame is not the no-pack baseline …; it is still the pack's
image`, and assertion 4 fails for the mirrored reason. Assertions 1, 2 and 5
keep passing, honestly, because a setting that does nothing moves no state
either.

## Opponent skill — `tests/check_enh_ai_difficulty.py`

```bash
MDKR_AUDIO=0 python3 tests/check_enh_ai_difficulty.py --build build
```

The only gameplay-class enhancement, so it is the only one required to *move*
the state stream. Six races per arm on Ancient Lake, and the route is chosen
deliberately: the autopiloted player **wins** there at `authored`, so mean
opponent finish has headroom. On Hot Top Volcano the same fixture finishes the
player last, pinning the metric at 4.0 and making the assertion unfalsifiable —
recorded in the docstring so nobody "simplifies" the route later.

The purity arm is the strong form: the gate builds a binary with the
enhancement **compiled out entirely** (`MDKR_ENH_AI_DIFFICULTY_OMIT`) and
requires 12,000 `[SIMHASH]` v3 rows byte-identical to the `authored` arm across
two seeds. `authored` returns by an early return before any float reaches an
ALU — never a multiply by 1.0f.

The wedge assertion is imported from `check_ai_unstick_opponents.py`, not
restated, and the lap-time floor is derived from the authored best lap and the
scale the binary itself reports rather than hard-coded.

## Free camera — `tests/check_tool_freecam.py`

```bash
MDKR_AUDIO=0 python3 tests/check_tool_freecam.py --build build
```

Detach at a tick, move, re-attach, and require **both** that the `[SIMHASH]` v3
stream is byte-identical to an un-detached run and that the frame after
re-attaching is byte-identical too — with the frames *during* detachment
differing, so the gate is not vacuous.

**This gate has already earned itself.** The free camera substitutes the latched
projection record, which is presentation-scoped and unhashed — but the globals
`cam_rebuild_native_projection()` derives from it are not, and the *next* tick's
visibility pass rebuilds its cull planes from them without refreshing. That
visibility answer gates AI RNG. The first run diverged at tick 2232, 232 ticks
after detaching at 2000. The fix went in the tool, not the gate: a closing hook
re-derives the authored lens through `cam_set_fov()` — the game's own rebuild,
not a saved copy.

Its positive control is three variants of "re-attach by restoring a saved record
instead of ceasing to substitute". Two of them **pass**, which is itself
informative: the port's own `camera_obstruction_projection_matches_render()`
handshake rejects a stale generation and falls back to the authored matrix, so a
naive re-apply is caught by existing machinery. The third — live identity with a
stale lens spliced in, which is what a re-apply would have to do to get past
validation — fails on exactly the intended assertion.

## Crash report — `tests/check_crash_screen.py`

```bash
MDKR_AUDIO=0 python3 tests/check_crash_screen.py --build build
```

The crash screen is **strictly additive**. Every harness here greps
`[CRASH]`/`[FATAL]`, so the gate's first job is proving those markers still
arrive **first and unchanged**, by line index, in both a SIGABRT and a SIGSEGV
arm. It also asserts the exit disposition is the exact value the pre-change
binary produced (`-6` and `-11`), so CI classification cannot shift underneath
the suite.

Two arms rather than one, because a single one would be vacuous: the abort path
fires during asset init with `tick=0` and `track=-1` frozen, so a hard-coded
constant would pass it. The gate therefore requires the two arms to **disagree**
on tick and track, requires `track-name` to be consistent with `track` in both,
and requires `renderer=` and `version=` to follow the real environment.

The report deliberately never spells a fault `SIGABRT` or `SIGSEGV` — those
strings are in `harness_utils.ABORT_MARKERS`, and the gate fails if any of them
appears inside the report line. `MDKR_NO_CRASH_HANDLER=1` suppresses the whole
surface, which the ~9 harnesses that defer to ASan rely on.

## Speed readout — `tests/check_enh_speedometer.py`

```bash
MDKR_AUDIO=0 python3 tests/check_enh_speedometer.py --build build
```

Four arms — baseline, `=0`, `=1` (mph), `=2` (kph) — capturing the same race
frame. `=0` is byte-identical to baseline. `=1` changes pixels **only inside the
readout box**, and the check walks every row outside that box asserting no
differing byte: the weapon panel ends at 88% height and the box starts at 90%,
so "outside the box" is the rest of the interface and the whole world.

The value assertion is not merely monotonic. A frozen sampler is
non-decreasing, so the window must also **start at rest and actually climb** —
the positive control (`return 6.0f`) fails on both halves of that.

Assertion 6 is the one that makes the authority claim real for this row: all
four arms run under `MDKR_STATE_HASH=3` on a route that *reaches a race with the
readout on screen*, and all 3,200 rows must be identical.
`check_enhancement_authority.py`'s own fixture never leaves the menus, so it
cannot observe this — it names this file in `EFFECT_GATES` instead.

## Developer-tool purity — `tests/check_dev_tools_purity.py`

```bash
MDKR_AUDIO=0 python3 tests/check_dev_tools_purity.py --build build
```

Registering a tool in `platform/app/dev_tools.cpp` is a **claim**: that opening
its window cannot move authoritative state. This gate enumerates the table from
the running binary's `[TOOLTABLE]` dump and tests that claim for every entry, so
a tool added later is born gated with no edit here.

For each tool it runs two headless races under `MDKR_STATE_HASH=3` — one with
`Tools.Enabled=0`, one with the tool forced open — and requires the `[SIMHASH]`
streams to be byte-identical. Frame count and race outcome are compared too, so
a tool that merely slows the frame loop enough to shift pacing is also caught.

**Parsing zero rows is a failure.** Every tool is an observer with no body until
its own task lands, so an empty table would otherwise sail through.

## Custom-track loader integration — `tests/check_custom_track_loader.py`

An additional negative arm uses `MDKR_LEVEL_NAMES_SELFTEST=unterminated`, fills
only the first loaded name entry with nonzero bytes, and invokes the production
selector. Its exact unterminated-name diagnostic is required; an unrelated
abort, sanitizer failure or the fixture's escaped-guard abort cannot pass.

The enabled arm additionally sets `MDKR_LEVEL_NAMES_SELFTEST=1` and requires
a positive-count `[LEVEL_NAMES]` marker for all four localized strings in every
retail name entry. Each lookup stays within its entry, including the selected
string's terminator. This is raw corpus compatibility, not a visual translation
review or acceptance of authored names.

The wrapper also boots with `MDKR_LEVEL_HEADER_SHORT_SELFTEST=1`, corrupting
only the consumer's private header-table copy to give entry zero no bytes.
It requires a nonzero exit and the named header-span refusal before decoding.
This negative arm validates the production boot boundary; initial and redirected
level-load call sites are additionally pinned by the runtime safety census.

Related save boundary: `check_custom_save_inventory.py --build build-rel --rom
<private-rom>` (also registered as `custom_save_inventory` and
`custom_save_inventory_asan`) runs the real progression/time codecs using the
ROM's retail metadata, then eight synthetic extra courses and one extra world.
It requires byte-identical retail encoding and decode/re-encode, guarded 40-byte
progression/384-byte time buffers, and empty custom records after retail restore.
The opt-in `MDKR_CUSTOM_SAVE_SELFTEST=1` fixture uses separate in-memory settings,
restores global metadata and codec cursor state, and performs no save-file I/O.
An ordinary boot must not emit its marker. This is not a manifest import,
appended-header startup, custom sidecar, or playable-course test.

Run with `--build BUILD --rom PRIVATE_ROM_PATH`, or select
`--only custom_track_loader` in `tools/run_checks.py`. Repeat against the
sanitizer build. The test isolates saves/settings and runs ordinary boot plus
the opt-in `MDKR_CUSTOM_TRACK_LOADER_SELFTEST=1` fixture. It requires one success
marker only in the enabled arm and a clean process exit in both arms.

All five sections use the active ROM's bounded, normalized tables with synthetic
appended bytes. Checks cover logical sizes, independent caller-owned table
copies, mixed retail/custom reads, invalid-read destination preservation and
retail restoration after detachment. Actual ROM bytes stay in memory, not in
fixtures or output. The enabled arm also requires successful bounded decoding
of all nonempty retail object-map and level-model compressed entries, with
separate positive corpus counts; synthetic-only or older loader fixtures do
not satisfy this gate. `dma_destination` separately links the real PI DMA
entry points (`platform/pi_dma.c`) with a recording ROM read and requires every
DMA to land at the caller's pointer: stack, static and NULL destinations, low
pointers (the test arena's first, an inner and its last low-32 address, two
outside that window, and `UINT32_MAX`), and full-width pointers whose low 32
bits are arena addresses. The arena helpers are defined as `stubs_dkr.c`
defines them, with the arena's high half away from every test pointer. Both
earlier reconstructions fail it: 1.7.0's unconditional rebuild from the low 32
bits, and the later helper that widened a low pointer only when the rebuilt
address fell inside the arena. On 32-bit hosts a pointer is its own low half,
so a rebuild cannot move it there; only the pass-through and completion
message are tested.

This does not validate package admission, authored model formats, importer/UI,
live reload or playable custom tracks. The fixture detaches before game asset
consumers initialize; production session ownership remains a separate gate.

## Asset sub-entry bounds — `tests/check_subentry_bounds.py`

```bash
MDKR_AUDIO=0 python3 tests/check_subentry_bounds.py --build build
```

The negative control for `mdkr_asset_subentry()`. Section indices were always
bounds-checked; indices *within* a section were not, and the one accessor that
did compare — `get_misc_asset()` — returned the **section base silently** when
the index was out of range, converting an out-of-bounds read into a confidently
wrong one. That is the failure this gate exists to keep closed.

Five arms. A positive control with no hook must exit 0 with no diagnostic, so
the check cannot pass merely because everything aborts. Two synthetic indices
(1 and `UINT32_MAX`) go through `MDKR_SUBENTRY_TEST_INDEX`, and two live ones
(`100000` and `-1`) go through `MDKR_SUBENTRY_TEST_MISC_INDEX` into the real
`get_misc_asset()` against the real loaded section. Each aborting arm asserts
`returncode == -SIGABRT` **specifically** — a segfault would pass a
merely-non-zero test — and regex-checks that the reported count is a plausible
ROM count rather than the index echoed back.

The companion `asset_subentry` CTest covers the accessor itself with no ROM:
each aborting case runs in a forked child and asserts both the signal and the
message text.

## Hosted ROM checker — `tests/check_rom_checker_page.py`

```bash
MDKR_AUDIO=0 python3 tests/check_rom_checker_page.py --build build
```

`dist/web/rom-check.html` asks a player to hand a 12 MB cartridge dump to a web
page, and this is what makes that a reasonable thing to ask.

**Nothing leaves the machine.** The page's text must contain no `fetch(`, no
`XMLHttpRequest`, no `WebSocket`, no `<form action`, no `sendBeacon`, no
`EventSource`, no `importScripts` and no `@import`; separately, every
URL-bearing attribute and every CSS `url()` in the file is *parsed* and required
to be same-directory relative, which is the general guarantee the literal list
only names mechanisms for. Every local file it does reference must ship beside
it.

**It answers what the engine answers.** The page loads `dist/web/rom-id.js`
rather than copying it — asserted with `check_rom_revision.py`'s own row parser,
so "carries its own revision table" means exactly what that check means by it.
Its verdict logic sits in one DOM-free block between two markers, which this
gate lifts out, loads under `node` beside `rom-id.js`, and runs over every
cartridge dump under `build/roms/`. The sentence it produces must equal the
`[ROM]` line the native binary prints for the same file, character for character
— both sides are handed the same display name, so the whole sentence is compared
and not a suffix of it. A synthesised `.v64` and `.n64` must identify as the
same release and hash to the same whole-image SHA-256 as the `.z64`; without
normalisation before hashing, every byteswapped dump would read as damaged.

`node` absent skips the two runtime assertions with a printed reason; the text
assertions always run.

Verified non-vacuous in three directions: a `fetch(` inserted into the page
fails the first assertion, a suffix appended to the verdict sentence fails the
comparison on all five dumps, and hashing before normalisation instead of after
fails the byte-order assertion on both `.v64` and `.n64`.

## Game-text index census — `tests/check_rom_text_indices.py`

```bash
MDKR_AUDIO=0 python3 tests/check_rom_text_indices.py --build build
MDKR_AUDIO=0 python3 tests/check_rom_text_indices.py --only nav,hub   # subset
```

Before a US 1.0 or European 1.0 ROM can be accepted, the port has to be shown
not to ask it for a text entry it does not have: `us.v77`'s `GAME_TEXT` holds
259 entries where `us.v80` holds 343 (`docs/ROM_REVISIONS.md` §5).

`set_current_text()` range-tests the id it is *given* and then adds the language
offset (`+85` German, `+170` French, `+255` Japanese) **afterwards**, so the
resolved index is never re-tested. That resolved index is what this gate bounds.
`game/src/game_text.c` gained one `NATIVE_PORT` census site at exactly that
point, emitting `[TEXTIDX] section=GAME_TEXT index=… requested=… language=…
count=…`. It is off unless `MDKR_TEXTIDX` is set; `MDKR_TEXTIDX=1` writes to
stderr, and any other value is a **file path** the census is appended to.

The path form is what lets the gate drive the real route gates unmodified — the
nine `nav_*` fixtures, the 20-track sweep, the hub tour, the campaign seams and
the trophy series — by pointing each at a shim named `mdkr64` that execs the real
binary with its own census file. Those gates parse the engine's stderr, so a
second stream interleaved into it would change what they read. Nothing in them
is modified, so the routes driven here cannot drift from the ones the suite runs.

Fails if any observed index reaches 259, printing the index and the route; fails
if any route group launched no engine at all, or if no index was observed
anywhere, because an enumeration over nothing proves nothing. The **coverage is
printed rather than implied** — distinct ids reached, the fraction of the
section they are, and the languages seen — and the docstring states plainly that
the claim is "these routes never resolve an index at or above 259", not "the port
never does". One of the three text-bearing asset sections is instrumented;
`MENU_TEXT` and `LEVEL_NAMES` are not.

**Recorded baseline** (us.v80, 60 engine runs across the five route groups):
maximum resolved index **81**, on `adventure_resume_race.txt @9000 frames
track=54`; 26 distinct ids of the 340 the ROM addresses (7.6%) over 40 accesses;
every access in English. The `nav_*` fixtures and all 20 tracks of the sweep
contribute **zero** — `load_game_text_table()` runs on level entry and a race
triggers no dialogue, so the whole census comes from the hub, the campaign seams
and the trophy cabinet. The printed projection notes that the same ids under
French (+170) reach 251, still in range, and under Japanese (+255) would reach
336, which is why a JP build needs its own enumeration and not this one.

Verified non-vacuous by lowering the ceiling below an observed index: the gate
then failed naming index 50 and the hub route that produced it.

## Enhancement authority — `tests/check_enhancement_authority.py`

```bash
MDKR_AUDIO=0 python3 tests/check_enhancement_authority.py --build build
```

Every enhancement in `platform/enhancement_registry.c` declares an authority
class. This gate tests that claim **in both directions**: a `presentation` row
must leave the `[SIMHASH]` v3 stream byte-identical when flipped to its probe
value, and a `gameplay` row must change it. One direction alone would let a
gameplay-changing setting be mislabelled cosmetic, or a setting that does
nothing be labelled as though it did.

The row list and each row's probe value are parsed from `[ENHTABLE]` lines the
**running binary** emits under `MDKR_ENH_DUMP_TABLE=1`, never from a list in the
test — a second list drifts, and a drifted one still prints PASS while silently
covering one fewer setting. `tests/test_enhancement_registry.c` fails a row that
declares no probe value, so adding an enhancement forces you to say how to
exercise it.

**Parsing zero rows is a failure**, checked before anything else. With every
effect currently unimplemented that is the likeliest way this gate could go
vacuous, and a mutant that drops every parsed row exits 1 on it.

`EXPECTED_INERT` names the rows whose effect is not built yet, each with the
task that closes it. The gate **fails if a listed row starts moving the
stream** — an expectation that has silently become wrong is worse than none.

## Checks whose detail lives with their source

The gates below are registered in `tools/run_checks.py` and run by the complete
suite. Their own module docstrings carry the full assertion list; what follows is
the invocation and what each one owns, so the manifest and this file agree.
`check_ci_contract.py` asserts that agreement: every registered `tests/check_*.py`
must be named here.

### Source-region simulation cadence — `tests/check_simulation_cadence.py`

```bash
python3 tests/check_simulation_cadence.py --build build \
  --rom baserom.us.v80.z64 --roms build/roms
```

Containment gate for NTSC and PAL source clocks, the original/enhanced pacing
mechanism, and the explicit oracle policy. Needs the ROM-revision directory
because the region clause covers both releases, and the PAL release is not the
default `--rom`.

### Field-level byte-swap audit — `tests/check_asset_swap_invariants.py`

```bash
python3 tests/check_asset_swap_invariants.py --rom baserom.us.v80.z64
```

Closes the byte-swap misread class rather than chasing instances: spec-derived
ROM field invariants with byte-reversed positive controls, plus raw
`asset_load()` swap coverage. The defining property of the class is that
deterministically wrong input data keeps every self-consistency oracle green —
BHV_WAVE_GENERATOR's `u16` fields were misread from bring-up until v0.8.

### Retail model corpus — `tests/check_rom_model_corpus.py`

```bash
python3 tests/check_rom_model_corpus.py --rom baserom.us.v80.z64
```

Walks every object and level model asset, its batches, sentinels, and render
states without relying on a reachable game route. Complements the live WebGPU
census: that one answers "what did our routes emit", this one answers "what did
those routes leave unvisited".

### Strict display-list and material census — `tests/check_webgpu_content_census.py`

```bash
python3 tests/check_webgpu_content_census.py --build build \
  --rom baserom.us.v80.z64
```

Runs all nine authored menu routes, every main race, every boss, all four
challenges, the Adventure intro/hub, and the 3P/4P layouts through the real
WebGPU backend under `MDKR_DL_STRICT=1`. Each process must report zero
display-list faults, every material identity and dynamic pipeline key, no shader
guard or pipeline failure, and at least 4x shader-index and 2x per-material
pipeline/bind-group headroom. It also runs the texture-cache content audit
(`MDKR_TEXCACHE_VERIFY=1`) and requires zero stale cache hits, which is the route
class where the historical key-aliasing bug lived.

### Repeated stage/ownership plateau — `tests/check_resource_plateau.py`

```bash
python3 tests/check_resource_plateau.py --build build \
  --rom baserom.us.v80.z64
```

Loads the same race four times in one process through the original Try Again
path and requires CPU pool, audio, GPU, and pointer-registry generations to reach
a plateau. The first generations may warm persistent caches; the later ones may
not grow.

Pass `--texture-pack /path/to/pack.zip` to copy a real pack into the run's
isolated preferences/mods directory. This arm requires nonzero rendered
replacement-texture evidence, not merely successful indexing. Use
`--artifacts-dir /private/path` to retain logs and the copied pack privately.
The counters cover engine pools, renderer ownership, and retained decoded
texture pixels/lookup-table bytes. The final three equivalent texture-cache
samples must match and decoded pixels must remain within the reported budget.
They do not measure process RSS, registry/ZIP memory, or frame-time spikes and
cannot alone prove that total process memory has plateaued.

For a longer cache-warmup check, use `--frames 75000`; generated retry pulses
remain within the native loader's 512-entry capacity. A short run may legitimately
fail the strict cache plateau requirement while still discovering new textures.
Keep that result and compare it with an actually repeated longer route.

`python3 tests/check_input_script_capacity.py --binary build/mdkr64 --rom
baserom.us.v80.z64` checks that exactly 512 entries are accepted and 513 fail
instead of silently truncating. It creates a hidden native window and uses
isolated temporary preferences; the ROM remains local.

### Rolling attract demo — `tests/check_attract_demo.py`

```bash
python3 tests/check_attract_demo.py --build build --rom baserom.us.v80.z64
```

Title-demo vehicle and path selection, long-soak stability, and input teardown.
GAME-08 was the source-labelled retail defect this owns: `load_level_for_menu()`
forced `VEHICLE_PLANE` for every menu level, so the demo's AI racers consumed the
plane-node family while individual frames still looked plausible.

### Launcher screenshot contract — `tests/check_app_capture.py`

```bash
python3 tests/check_app_capture.py --build build --self-test
```

Launcher screenshot dimensions, contrast, palette, and draw bounds. `--self-test`
additionally requires the broken-direction mutation controls to fail; the runner
always passes it, so the controls are release evidence rather than an optional
extra.

### Real launcher widget input — `tests/check_app_ui_input.py`

```bash
python3 tests/check_app_ui_input.py --build build --rom baserom.us.v80.z64
```

Drives the real ImGui widgets through SDL keyboard and gamepad input in a process
with private app preferences, video config, and save roots. Proves first-run
selection, cross-process reload, gamepad parity, visible save failure with
unchanged desired state, and recovery once the configuration path becomes
writable.

It also presses the Presentation pace radio button and requires ONE press to
persist BOTH pacing keys — `FrameLimit=display` with
`MotionSmoothing=interpolate` for Smooth, and the authored pair for Original —
then reads them back in a fresh process. Configs the quick choice has no name
for (a numeric cap, `40`, `display-margin`) must read back as `custom` rather
than being shown as one of the two named pairs.

### Launcher panel scrolling — `tests/check_app_scroll.py`

The ROM-free `app_scroll_environment` CTest checks this harness's launch boundary:
missing, empty and relative save paths must refuse to launch even if an inherited
save path exists. An explicit temporary fixture retains its save/preferences/
video paths while unrelated inherited engine controls are removed.

```bash
python3 tests/check_app_scroll.py --build build --rom baserom.us.v80.z64
```

Drives the two ways a player scrolls a launcher page, both through the production
`AppHost::processEvent` -> `ImGui_ImplSDL2_ProcessEvent` adapter, and asserts the
panel's `ScrollY` actually advances:

* A **MacBook-trackpad-shaped mouse wheel** — an `SDL_MOUSEWHEEL` whose integer
  `y` is zero and whose motion is carried entirely by the fractional `preciseY`
  field (`MDKR_APP_SMOKE_WHEEL_SCROLL`). A wheel bridge that reads the rounded
  integer field scrolls nothing on a trackpad; this run stays RED until the
  bridge reads `preciseY`.
* The **macOS natural-scroll direction** — the same wheel marked
  `SDL_MOUSEWHEEL_FLIPPED` (`MDKR_APP_SMOKE_WHEEL_FLIPPED`). The input retains
  the same downward delta; macOS already applied the user's preference, so
  negating it again would incorrectly reverse the gesture.
* A **touchscreen drag** (`MDKR_APP_SMOKE_TOUCH_SCROLL`) on non-interactive
  content, which pans the same panel through the custom touch gesture.

Each sub-run owns private app preferences, video config (`MDKR_VIDEO_CONFIG_PATH`),
and save roots, so a repo-root `mdkr64.ini` can never reach it.

### Phone panel focus — `tests/check_phone_panel_focus.py`

Runs the production SDL keyboard and gamepad accessibility walks at 640x480
and requires the focused-item announcement for Start Local Play. Use `--size`
to exercise additional window sizes. The private, asset-free fixture relaxes
ROM readiness only; it does not open a room or certify connected-room or
ready-ROM navigation. This guards the compact-window regression where an
off-screen flattened child prevented directional navigation to local play.

### Per-viewport route isolation — `tests/check_viewport_route_isolation.py`

```bash
python3 tests/check_viewport_route_isolation.py --build build-rel \
  --rom baserom.us.v80.z64
```

2P/4P object-route isolation with a last-viewport fade control. The fixture
leaves player one's racer opaque in every early viewport and faded only in the
final one; the production arm must consume the retained object x viewport route,
and the broken-direction arm substitutes the final viewport's route everywhere,
reproducing MP-001 exactly.

### Presentation RNG split — `tests/check_presentation_rng_split.py`

```bash
python3 tests/check_presentation_rng_split.py --build build \
  --rom baserom.us.v80.z64
```

The port carries two random streams. `rand_range()` steps
`gCurrentRNGSeed`/`gPrevRNGSeed`, the retail pair registered in the rollback
snapshot (`rollback_game_authority.c:121-122`) and covered by the `[SIMHASH]`
hash. `presentation_rand_range()` steps a platform-private word that is
deliberately outside that registry. This gate asserts the split's whole point:
presentation randomness must not advance the authoritative pair.

`MDKR_RNG_SPLIT_TRACE=1` emits one `[RNGSPLIT]` row per *presented* frame. Be
precise about what the route is: at `MDKR_SIMULATION_CADENCE=enhanced` with
`MDKR_SYNTH_FIELDS=1` the simulation ticks on **every** presented frame and the
`[SIMHASH]` hash changes on every one of them, so these are *not* frames that
present without advancing the simulation. They are authoritative ticks whose
settled menu-idle logic draws no random numbers, while texture animation keeps
drawing from the presentation stream — two streams running side by side over the
same frames, one required to stand still. At the shipping two-field cadence
`cadence_compat_rand_range()` routes every HUD roll back onto the authoritative
stream for byte-exact ROM ordering and the presentation stream never advances at
all, so the assertion would hold vacuously; the lane pins the cadence where the
split is live.

Both halves are asserted, because either one alone passes for the wrong reason.
Over the settled window the seeds must be byte-identical, the presentation draw
counter must strictly increase by at least 100 across at least 16 distinct
seeds, and the authoritative seed must have moved *before* the window — that
last one is what keeps a trace printing a constant from passing. Arm B is the
positive control: `MDKR_TEST_RENDER_IMPURITY=1`, the existing render-purity
seam, performs one authoritative RNG write inside every render and must break
the pin while leaving the presentation draw counts identical. Arm C requires two
runs to be byte-identical, because the presentation stream is seeded from a
constant, not host state, and the pixel-comparison lanes depend on that.

The census behind the redirect decisions — every `rand_range()` caller in
`game/src`, its verdict, and the measurements that decide it — is
`docs/ref/presentation-rng-census.md`. As of 2026-09-03 it carries 32
cadence-conditional sites, not 24: the eight it had listed as latent
presentation output were redirected once the enhanced-cadence arm of
`check_authored_rng_compat.py` existed to measure what that spends.

### Weather RNG order — `tests/check_weather_rng_order.py`

```bash
python3 tests/check_weather_rng_order.py --build build-rel \
  --rom baserom.us.v80.z64
```

Wizpig 1 (level 37) is the only normal level route that exercises rain. The gate
records the raw authoritative state hash plus the seed immediately around every
splash placement roll and lightning timer reset, and requires presentation
invariance. Only digests and row counts are stored; no ROM-derived state ships.

### Weather presentation identity — `tests/check_weather_presentation_identity.py`

```bash
python3 tests/check_weather_presentation_identity.py --build build-rel \
  --rom baserom.us.v80.z64
```

Snow (level 13) and rain (level 37) under `MDKR_PRESENT_RATE=120` +
`MDKR_PRESENT_SMOOTHING=interpolate`, against the same route with smoothing off.

A presentation owner is only minted for a real spawned `Object` in
`gObjPtrList`. Precipitation is not one: the flakes come out of
`gSnowVertexData` and the rain sheet out of `gRainVertices`, so before
`weather_register_vertex_batch()` existed neither carried an identity and an
interpolated present replayed their tick-T bytes unchanged — held for two to
four presents, then jumped. Nothing detected that. The authoritative hashes do
not move (registration is not authoritative), the pixel controls pass (a frozen
flake is a coherent image), and the aggregate packet counters were already large
from particles and model deformation.

So the gate reads the counters that name *this* content:
`renderervertexreg` must be nonzero (the batches have an identity at all),
`renderervertexoverride` must be nonzero (an interpolated present really did
substitute a moved vertex, not merely resolve a pair whose endpoints agree), and
on the snow route `renderervertexjumphold` must be nonzero — the volume-wrap
guard has to be able to fire, or `max_vertex_delta` is a comment. Both routes'
`[SIMHASH]` streams must equal the smoothing-off run's, which is the "identity
must not perturb the authoritative stream" contract.

It also reads `[SNAPSHOT] externalpeak`/`externalcaptures`, which cover the
other half of the same defect: the rain splashes and the lens flare pieces. Their
world position barely moves (a splash never moves at all), so no vertex counter
can see them — what was wrong is that they were billboarded against a camera
that interpolates while they themselves were drawn from a static array and a
function-local the snapshot walk could not discover, so they swam against the
world and snapped back every tick. They are now registered with the snapshot's
renderer-owned transform registry, at their spawn and retirement sites so a
recycled splash slot gets a fresh generation. `overflows` must stay zero (the
registry must never push a capture past `PRESENTATION_SNAPSHOT_MAX_OBJECTS`) and
`discontblend` must stay zero (nothing blended across a recycled slot).

Measured: snow 167,800 registered batches / 476,187 substitutions / 3,816 guard
holds and 5 renderer-owned transforms (the lens flare); rain 2,072 / 1,278 / 0
and 8 renderer-owned transforms (the splash slots).

### Address-domain narrowing — `tests/check_address_domains.py`

```bash
python3 tests/check_address_domains.py
```

Source gate keeping native pointer-to-32-bit crossings inside named domain
boundary helpers. The compiler rejects direct pointer/integer casts; this owns
the two-cast spelling such as `(u32)(uintptr_t)pointer`, which suppresses the
diagnostic while still discarding the high half. Matching-only `!NATIVE_PORT`
branches are excluded.

### Player-facing prose — `tests/check_player_prose.py`

```bash
python3 tests/check_player_prose.py
```

Source gate banning AI-slop vocabulary (*seamlessly, leverage, ensure,
robust, "enhance your experience", comprehensive, "authored presentation
states", "safe frame boundary", "durable storage", "compatibility mode", and
the same pattern extended) from what a player or a reviewer actually reads:
string literals in `platform/app/*.cpp`/`*.h` (the launcher UI), plus
`README.md` and `RELEASE_NOTES.md`. It tokenizes C/C++ source so only string
literal contents are scanned, never comments or code — engineering prose
legitimately uses this vocabulary precisely, and a gate that flagged it would
be disabled within a week. Positive control: a seeded `"seamlessly"` string
must be rejected; a same-text code comment must not be.

### Cadence mode gating — `tests/check_cadence_gating.py`

```bash
python3 tests/check_cadence_gating.py
```

Source gate rejecting `updateRate == 1`, `updateRate == 2`, `updateRate != 1`,
`updateRate != 2` (either operand order) anywhere under `game/src` or
`platform`. Code that behaves differently under the Enhanced simulation
cadence must gate on the launch-time `platform_sim_cadence_is_enhanced()`
(`platform/platform_os.h`, `platform/platform_sdl_min.c`), never on the
per-tick `updateRate`: under Original a lagging tick legitimately arrives with
`updateRate` 3 or more, and the authored code must handle that
byte-identically, so a mode test keyed on `updateRate`'s value would change
Original's own output on exactly the frames the determinism contract cares
about most. `mdkr_boss_cadence_clamp` (`game/src/racer.c`) and
`menu_stick_delay_amount` (`game/src/menu.c`) are the two correct examples;
ordinary arithmetic uses of `updateRate` as a scale factor are unaffected.
Positive control: a seeded `updateRate == 2` mode test must be rejected.

### Adventure Party boundaries — `tests/check_adventure_party_boundaries.py`

```bash
python3 tests/check_adventure_party_boundaries.py
python3 tests/check_adventure_party_boundaries.py --self-test
```

The AP-01 static source-boundary gate for Adventure Party (see
`docs/architecture/adventure-party.md`). Its central invariant is that a party
session never sets the retail two-player-adventure globals, so every retail
exact-two / lead-swap branch stays inert without an edit. The gate:

1. Requires every use of `is_in_two_player_adventure`, `race_is_adventure_2P`,
   `swap_lead_player`, `input_swap_id`, `gIsInTwoPlayerAdventure`,
   `gTwoPlayerAdvRace`, or an exact player-count comparison of
   `gNumberOfActivePlayers` / `gNumActivePlayers` against an integer literal
   under `game/src` to be declared, with a classification and a treatment
   (`bypass` / `adapter-seam` / `inert-by-globals` / `unrelated`), in
   `tests/adventure_party_boundary_inventory.json`. A new undeclared branch
   fails closed; a stale entry a later ticket removed also fails. Entries are
   keyed by file plus the comment-stripped, whitespace-normalized source line
   (counted), so they survive line-number drift.
2. Rejects any write to `gIsInTwoPlayerAdventure` / `gTwoPlayerAdvRace` or set
   of `CHEAT_TWO_PLAYER_ADVENTURE` from adventure-party code (`platform/
   adventure_party/**` or a `#ifdef NATIVE_PORT` adapter region naming an
   `adventure_party` identifier nearby).
3. Rejects `mdkr_authoritative_player_count` referenced from
   `platform/adventure_party/**` (the online-authority boundary).
4. Rejects a new ambiguous `g...Party...Count` global under `game/src` or
   `platform/`.
5. Rejects `adventure_party` identifiers inside the test-hook file
   `platform/mdkr_adventure.c`/`.h` or Phone Party infrastructure
   `platform/party/**`.

ROM-free, no build. `platform/adventure_party/` does not exist yet; a missing
directory contributes no violations. `--self-test` feeds every rule a synthetic
in-memory violation (never touching the repo on disk) and asserts the rule
fires while a matching clean control stays silent.

### Adventure Party admission — `tests/check_adventure_party_admission.py`

```bash
python3 tests/check_adventure_party_admission.py            # ~2-3 min, muted + headless
python3 tests/check_adventure_party_admission.py -v
```

The AP-06 focused route: the FIRST game-source integration of Adventure Party
(`docs/architecture/adventure-party.md`). It drives two, three, and four
controllers through Character Select with `Enhancements.AdventureParty` on and
proves each reaches the ordinary Adventure route the retail game denies them —
`CHARACTER_SELECT(3) -> GAME_SELECT(19) -> Adventure -> FILE_SELECT(6) ->`
campaign load — and that a session is formed at file entry (`aparty_session`
FORMING then ACTIVE_LOBBY with the joined seat/character roster). Reaching Game
Select with three or four seats *is* the party path: retail stops a 3/4-player
Adventure selection at `TRACK_SELECT(15)` because the JOINTVENTURE offset admits
at most two, so the run also asserts no JOINTVENTURE magic code was submitted.
With the enhancement off, three/four players route to Tracks exactly as stock and
no `aparty_` line appears; one player is unchanged in both arms and never forms a
party.

The gate also proves two file-authority facts. **R26 new-game refusal:** a party
cannot begin a NEW campaign yet (the new-game shared-scene envelope is AP-11), so
a party host confirming an UN-STARTED file is fail-closed refused
(`aparty_file_refused: reason=newgame`, the cursor stays, no session, no campaign
load) instead of silently collapsing to a 1P new game; the started fixture file
then confirms and forms the session normally. **FIX 1 copy/erase host-only
authority:** `fileselect_input_copy` / `fileselect_input_erase` skip player-two
aggregation in a party exactly as `fileselect_input_root` does — a source-level
guard-presence assertion (`check_copy_erase_guard`), because that guard is
token-identical to the ROOT guard the ON arm already exercises behaviourally, and
a headless copy/erase confirm drive with a null-effect oracle would need
gFileConfirm observability the build does not emit.

Three positive controls run inside the gate and mutate the route/env or the
captured output, never the sources: the OFF-arm 3-player run (the behaviour a
clamped-to-two admission would produce) must FAIL the ON-arm assertions, an ON-arm
run with its `aparty_session` lines stripped must FAIL them too, and the R26
refuse-arm output with its `aparty_file_refused` line stripped must FAIL the
refusal arm.

Save fixture: the check writes its own EEPROM image — a started, checksum-valid
Adventure One save in slot 0, built with the `harness_utils` bit-stream encoders
(the same slot shape `check_adventure_two.py` resumes on its Adventure One arm,
minus its progress) — so the host's single FILE_SELECT confirm resumes an
existing file (slots 1 and 2 are empty, which the R26 refuse arm confirms first).
The new-game shared-scene envelope is AP-11, out of scope here. No developer save
is read or written; every run uses a private temporary directory.

### Adventure Party hub roster — `tests/check_adventure_party_hub.py`

```bash
python3 tests/check_adventure_party_hub.py                 # ~4-5 min, muted + headless
python3 tests/check_adventure_party_hub.py --players 3 -v
```

The AP-08 focused route: with a party session active (admission proven by
`check_adventure_party_admission.py`), an Adventure LOBBY load spawns the WHOLE
party atomically. For 2, 3 and 4 players it proves, from the running binary:
N human racers with the joined seat→character identity (`aparty_roster` + the
`[PACE]`/`[PACEn]` probes); N viewports in the existing
`VIEWPORT_LAYOUT_<N>_PLAYERS` composition — the 3P arm keeps the fourth-quadrant
minimap — with per-quadrant pixel evidence; a per-viewport hub HUD (`hud_init`
reports N viewports); a stable per-seat input binding (seat *i* reads only
controller port *i*, no swap) demonstrated by driving each pad in isolation and
asserting only that racer moves; no fail-closed spawn abort; and a stable roster
generation. With the enhancement off, three/four controllers route to Tracks and
no `aparty_` line appears.

Two positive controls run inside the gate: a flat-field control flattens each
viewport region of a real hub frame and requires the same scorer to reject it
(a blank viewport must not pass), and a swapped-binding control re-attributes
each driving round to a rotated seat and requires the motion analysis to fail
(so it genuinely discriminates the binding). Note the hub viewport-liveness
floor is deliberately looser than `check_race_multiplayer.py`'s — a lobby is not
a race track, so a free-driving racer may briefly face a wall — but it still sits
far above the single-colour flat fill the positive control produces.

Save fixture: the started Adventure One slot-0 save from
`check_adventure_party_admission.py` (imported), resumed by the host's FILE_SELECT
confirm. No developer save is read or written.

Key measurement finding: AP-08 overrides the racer/viewport count *inside*
`track_setup_racers` (not `gNumberOfActivePlayers`), so the `level_load`
`numPlayers` field stays 0 even for a party — the roster-expansion signal is the
`aparty_layout`/`aparty_roster`/`aparty_binding` traces and `hud_init`, not
`numPlayers`. `check_enhancement_authority.py`'s 3P profile and
`check_adventure_party_admission.py` were updated to assert `aparty_layout`
accordingly (was: the pre-AP-08 "hub still 1P" tripwire).

### Adventure Party lobby interactions — `tests/check_adventure_party_transition.py`

```bash
python3 tests/check_adventure_party_transition.py            # ~3-4 min, muted + headless
python3 tests/check_adventure_party_transition.py -v
```

The AP-09/10 focused route: with a party in the central hub, every shared lobby
interaction resolves to ONE authority, proven from the running binary's `aparty_`
traces (the AP-05 schema):

- **Doors/exits** — any participant may trigger, but the pure reducer
  (`adventure_party_arbitrate_transition`) latches exactly ONE whole-party
  transition per level generation. A *single* door (host only) drives one
  authored load and re-forms the WHOLE party at the destination lobby
  (`aparty_transition` once + destination `aparty_roster`/`aparty_layout`); two
  racers into the door in the same window still yield one transition — the lowest
  seat wins, the loser's door is rejected (`aparty_interaction action=2
  verdict=-1`), and there is no second load. Proven at 2P and 3P.
- **Golden balloons** — two non-host seats race a shared hub balloon and it is
  collected exactly once (`aparty_interaction action=3 verdict=0`), by a non-host
  seat; the per-course collected flag makes the double-collect a mechanical
  impossibility (first toucher wins).
- **Pause** — a non-host `Start` opens the ONE shared pause (`aparty_interaction
  action=1` with a non-host seat) and the host owns the resume decision
  (`action=1 verdict=0` from seat 0). One `gIsPaused` / one pause menu, never
  per-viewport.
- **Disconnect** — dropping a bound pad during the lobby (the `MDKR_AP_DROP_PAD`
  injector; the input-script presence mask is whole-route and cannot) forces the
  shared pause (`aparty_interaction seat=<dropped> action=1 verdict=-3`); the
  pure `adventure_party_disconnect_should_pause` decision it rests on is unit-
  tested ROM-free in `test_adventure_party_policy`.

Racers are steered by the `MDKR_AP_SEAT_ROUTE` test injector (each named seat
follows its own waypoint route to a door `E<dest>` or balloon `B<id>`), the only
way to send exactly one seat, or different seats, at a door — the shared
`MDKR_DRIVE_ROUTE` drives every human at one target. Two positive controls run
inside the gate: the single-door output must FAIL the conflicting assertions (a
single door has no losing-door rejection), and stripping the `aparty_transition`
lines must FAIL the single-door assertions (no latched transition). The off arm
(doors behave stock) is covered by `check_adventure_hub.py`.

Save fixture: the started Adventure One slot-0 save from
`check_adventure_party_admission.py` (imported), resumed by the host's FILE_SELECT
confirm. No developer save is read or written.

R16 two-hop: the gate also drives central hub → world lobby → **back** (the
shared `MDKR_DRIVE_ROUTE`, level-scoped, which per-seat routes cannot span) and
asserts two lobby→lobby `aparty_transition` latches in two consecutive
generations with the full 3-seat roster re-formed at BOTH destinations. This is
the R16 fix (Task 9): `ADVENTURE_PARTY_EVENT_LOBBY_TRANSITION` takes a
lobby→lobby door through the one `enter_level` generation bump, which clears the
first hop's terminal latch so the second door can latch — the release the
arbiter latch otherwise never gets within a session (the Task 8 single-hop
limitation is resolved).

### Adventure Party default race loop — `tests/check_adventure_party_race_loop.py`

```bash
python3 tests/check_adventure_party_race_loop.py            # ~8 min, muted + headless
python3 tests/check_adventure_party_race_loop.py -v
```

The AP-12 / R16 focused route: with a party resumed into the central hub, the
whole party crosses **hub → world lobby** (a lobby→lobby door, the R16
`LOBBY_TRANSITION` bump) and then **lobby → race**, each as one arbitrated
whole-party transition, and a DEFAULT balloon race then runs with ALL N humans
plus CPUs filling to a SIX-racer total. Proven from the running binary's traces:

- **Six-racer field** — the `racefield:` adapter diagnostic reports
  `humans=N cpus=6-N total=6 viewports=N` for N in {2,3,4} (2P=4 CPUs, 3P=3,
  4P=2); the field rule (a humans-only field would auto-award a co-op win) is a
  direct assertion. The race entry publishes the full N-seat `aparty_roster`, the
  `aparty_layout viewports=N`, and N `aparty_binding seat=i port=i` — the party
  entered the race whole, per-seat binding held.
- **Winner independence** — a **host win**, a **non-host-human win**, and a
  **CPU win** each returns the SAME party to the lobby: the returned lobby
  publishes the identical roster / layout / per-seat binding, the session
  generation (sgen) is unchanged, and the level generation (lgen) advanced
  (`RACE_START` then `RACE_RESULT_COMMITTED`). No lead swap, no roster swap — all
  the retail lead-swap paths are gated on `is_in_two_player_adventure()`, which a
  party never sets. Winners are forced with the `MDKR_AP_RACE_WINNER` test hook
  (by stable `racerIndex`), because an AI-driven CPU field never naturally lets a
  chosen human win — the host finishes first and the non-host humans are
  force-finished last; the hook permutes finish positions after the port-1 human
  has genuinely finished (it changes the verdict, not the drive).
- **Retry** — postrace TRY AGAIN reloads the SAME race (two race loads with no
  lobby load between them, the party six-racer field re-fielded both times) and
  the session stays `ACTIVE_RACE` (exactly one `ACTIVE_RACE` entry — no
  `RACE_RESULT_COMMITTED` between the loads).
- **Quit-to-lobby** — a mid-race host pause → RETURN TO LOBBY
  (`PAUSE_QUIT_LOBBY`, driven by the `MDKR_TEST_PAUSE_QUIT` hook) returns the
  party to the lobby (`RACE_RESULT_COMMITTED`), roster/layout/binding intact, no
  double-load.

Driving: the shared `MDKR_DRIVE_ROUTE` steers every human at the hub/lobby doors
(the party travels as one; the arbiter latches a single whole-party transition);
in the race, unrouted, `MDKR_AUTOPILOT` drives all humans while the CPUs fill the
field; `MDKR_FORCE_LAPS=1` keeps each arm short. Two positive controls run inside
the gate: stripping `aparty_roster` from a return output must FAIL the
party-returned-intact assertion, and replaying a 2P output through the 4P field
assertions must FAIL (a 2P field is humans=2/cpus=4, not humans=4/cpus=2). The 4P
arm is field-rule only (four viewports render ~4× slower; the winner/return
matrix is the 3P arms). The off arm — the 1P adventure race loop unchanged — is
`check_adventure_race_loop.py`.

Award semantics are OUT OF SCOPE here (AP-13 owns team-condition → exact-once
commit): the gate asserts no save/award bytes. For the record, the interim retail
finish code writes a first-clear reward (RACE_CLEARED + a world balloon) iff the
first-place racer (`gRacersByPosition[0]`) is a human and `settings->gNumRacers ==
1` — so a host OR non-host human win writes it and a CPU win writes nothing; AP-13
reconciles this.

Save fixture: the started Adventure One slot-0 save from
`check_adventure_party_admission.py` (imported), resumed by the host's FILE_SELECT
confirm. No developer save is read or written.

### Adventure Party AP-18 campaign qualification — `tests/check_adventure_party_campaign.py`

```bash
python3 tests/check_adventure_party_campaign.py --build build --rom baserom.us.v80.z64
python3 tests/check_adventure_party_campaign.py --self-test
```

AP-18 turns the campaign-support claim into an exhaustive, ROM-derived
manifest. `tests/data/adventure_party_campaign_manifest.json` must account for
each of the six playable Adventure lobbies and all thirty-four save-eligible
courses exactly once. The checker independently decodes the level-header world
and race type, level enum name, save order, default vehicle, and available
vehicle mask, and compares those facts to every manifest row. The manifest also
records the actual party-facing vehicle set, including the retail multiplayer
narrowing (no hovercraft on Spaceport Alpha; no plane on Frosty Village). It
requires the complete central-hub outbound/return transition set, all entry-door classes,
and the Adventure One/Two, default, silver, challenge, boss, trophy, Taj and two
deliberate fail-closed campaign branches to have an explicit party policy and
witness. Removing one course is an in-process mutation control and must fail.

The full arm then replaces the interim AP test-hook entry where retail geometry
is headless-drivable. A legitimately boss-beaten save crosses the central hub,
Dino Domain lobby and the **real Ancient Lake door**; the race's own retail
predicate reports a silver race and finishes with eight team coins. A first-boss
checkpoint crosses the real Hot Top door, wins the fourth race, returns to the
lobby, then crosses the **real Tricky door**. That one route is the multi-hop
`0 -> 12 -> 7 -> 12 -> 38 -> 12` breadth witness: it must suspend host-solo,
finish as player index zero, restore an exact three-seat roster, and restore the
same vehicle recorded on the pre-suspend lobby load at retail boss-return
entrance 5. Stripping a silver transition and mutating only that restored
vehicle must each fail their oracle.

The conflicting-transition arm uses two genuinely different Dino race doors,
E5 (Ancient Lake) and E3 (Fossil Canyon). The checker requires that exact pair
in the per-seat route contract. With both routes active, exactly one of those
destinations may latch/load and the other interaction must be rejected by the
terminal transition latch. The rejection oracle is scoped after the Dino lobby
load so an unrelated same-E12 hub collision cannot satisfy it; stripping that
rejection is a positive control and must fail.

No `MDKR_LOAD_TRACK`, `MDKR_SILVER_FORCE`, `MDKR_CHALLENGE_FORCE`, or
`MDKR_TROPHY_FORCE` is allowed in this gate. Challenge and trophy policy remain
covered by AP-15/AP-16, but their door geometry is not mislabeled as a real-door
pass: the manifest records the exact headless limit. With the ROM-authored Dino
key bit (`1 << keyID == 2`) set, E11 direct steering stalls at a measured minimum
708.3 units from the exit (70-unit key-door open radius), behind the authored
wall. The real trophy cabinet NPC is at `(933,-6,-2172)` in Dino Domain; two
legitimate boss-beaten-save approaches either wedge at the interior wall or let
another independently mobile party racer hit a real boss/overworld exit first.
These are navigation-driver limits, not waived gameplay assertions; the
manifest keeps them visible until a pathfinding-capable headless route can
replace the two remaining envelope retargets.

### Adventure Party AP-19 resource/performance — `tests/check_adventure_party_performance.py`

```bash
python3 tests/check_adventure_party_performance.py --build build --rom baserom.us.v80.z64
python3 tests/check_adventure_party_performance.py --self-test
python3 tests/check_adventure_party_performance_soaks.py --build build --rom baserom.us.v80.z64
python3 tests/check_adventure_party_performance_soaks.py --self-test
```

The release invocation is one serialized four-camera process at the qualification
window (640x480) and performs **twenty** genuine Dino Domain
world-lobby → Ancient Lake → world-lobby cycles. The route does not reload or
re-form the party between cycles: each race is a production one-lap replay field,
each postrace return is selected from the live menu, and the session generation
stays one while every race/return pair advances the level generation exactly twice.
Each side of every cycle must republish exactly four identity controller bindings,
four viewports, and four HUDs.
The checked-in script contains sparse course-camera advance edges; each run
projects those edges only through its requested cycle horizon. After the twentieth
ordered door step there is no input edge at the return entrance, so the final
lobby can flush its resource generation without accidentally starting cycle 21.

The private save is the standard started Adventure-One fixture with only Ancient
Lake's two-bit status pre-cleared at its ROM-derived save ordinal. That makes all
twenty race loads ordinary no-award replays; otherwise the first natural human
win schedules a one-player course cinematic before a later entry, contaminating
the equivalence set. Progression itself remains AP-13's independently checked
scope.

Budgets are data, not constants hidden in the checker:
`tests/data/adventure_party_performance_budgets.json` freezes the retail four-player
display-list capacity at 11,000 Gfx commands and the qualification ceiling at
10,750, preserving at least 250 commands of headroom. The capacity is the actual
four-player allocation (`gNumF3dCmdsPerPlayer[2..3]`), not an inferred limit.
The measured worst case is 10,507 on a legitimate 4P Dino-E0 return to the
central hub: 493 commands below hard capacity and 243 below the qualification
ceiling. The gate reads every
`gfxtask` submitted in the four-camera central hub, world lobby, and race spans;
the ordinary initial-entry 4P hub measurement is 9,670. `maximum_high_water_bytes`
restates the same ceiling in the units the engine reports, and `load_budgets`
pins it to `qualification_max_commands * 8` so the two cannot drift apart. That
row is checked against the engine's own `[TRACE] dl_high_water` witness rather
than the host census, which covers the whole process instead of the selected
four-camera spans and reads the length against the row actually installed: the
twenty-cycle arm measures 77,128 of 86,000 bytes against an 88,000-byte
four-player row, and the five-rebuild Taj arm 73,752. A run that emits no
witness at all fails, so the counter cannot quietly stop. It also requires at least 1 MiB main-
pool free and a 512 KiB largest free block, bounded texture/registry occupancy,
zero ambiguous/full registry inserts, and coherent fixed audio/controller pools.
The last five equivalent race and lobby generations must have identical main-pool
and audio-heap ownership. Texture/shader and pointer-registry counters may perform
their documented one-generation retirement transfer, but the terminal two may not
establish a new high or retain a rising three-generation suffix, and every count
remains under its formal ceiling. A process that
merely survives, or one whose individually plausible counters grow per generation,
fails.

The same check compiles `tests/data/adventure_party_performance_churn.c` against
the production reducer and executes 20,000 complete FORM → race → optional
host-solo suspension/restore → QUIT → DESTROY lifetimes. Session and level
generations must land exactly at 20,000 and 70,000, while every dissolved session
has zero roster, suspended roster, latch, and token state. Parser controls inject
generation growth, a swapped controller binding, a retained churn failure, and a
display-list over-budget relationship, an over-budget and a missing
display-list high-water witness; each must be rejected. `--self-test` runs
only those controls. `--development-cycles 1..19` is explicitly non-qualifying
and is never used by the manifest.

### Adventure Party AP-19 soak companion — `tests/check_adventure_party_performance_soaks.py`

The serialized companion closes AP-19's other two literal repeated-operation
criteria in real game processes. Its 4P Taj pair runs the same 640x480 scene
with one and five CAR/HOVERCRAFT rebuilds. Every rebuild must publish exactly
four live racers, the exact roster/bindings and four-view layout. Both arms then
cross real E12 and E0 doors so queued renderer/registry retirement settles at a
normalized central-hub endpoint; main/audio ownership, renderer-live and
registry-live must match exactly. The measured five-transform endpoint is
`mainLive=589`, `mainUsed=3831696`, `mainFree=12907120`,
`mainLargest=12656336`, `rendererLive=0`, `registryLive=30`; its DL high-water
is 9269/10750 commands.

The boss arm uses AP-17's legal unbeaten checkpoint and five natural defeats,
so no save mutation or first-win presentation makes the generations unequal.
Each completed host-solo run must restore all four seats with `match=1`, advance
the level generation exactly once, and begin one terminal flush suspension that
publishes the fifth restore's renderer ownership. The last five boss and restore
generations must plateau under the same formal memory/renderer/registry budgets;
the measured DL high-water is 4283. A pre-cleared boss is deliberately not used:
retail rematches self-reload and would never exercise the restore adapter.
Mutation controls reject an extra Taj racer, retained final memory, boss growth,
and a swapped restored controller.

### Adventure Party exact-once progression — `tests/check_adventure_party_progress.py`

```bash
python3 tests/check_adventure_party_progress.py            # ~10 min, muted + headless
python3 tests/check_adventure_party_progress.py -v
```

The AP-13 gate: a co-op default-race win writes EXACTLY ONE existing retail
campaign commit, and nothing else does. Proven from the running binary + the
persisted EEPROM:

- **Byte-equivalence, per winner seat.** A party win with seat 0/1/2/3 forced
  first (2P/3P/4P, by stable `racerIndex` — `apracewinner`) persists a 40-byte
  slot **byte-identical** to a 1P win of Ancient Lake driven from the same started
  save. The tolerated-difference whitelist is **EMPTY**: the save slot holds only
  campaign progression (no character/start-order rows — those live in the runtime
  `Settings.racers[]`, never serialised), so a party win writes the same bytes as
  a 1P win. The commit is decided by the PARTY team condition + an exact-once
  token (`aparty_award op=issue…result=1` then `op=consume…result=0`), NOT the
  retail arm — which for a party would fire by accident on `settings->gNumRacers
  == 1` and could neither enforce exact-once nor survive the finish/door hand-off
  relabelling the winner `PLAYER_COMPUTER`.
- **Exact-once.** Each fresh win emits exactly one token issue+consume(ok) pair
  and exactly one `RACE_CLEARED` write (`[BOSSW]`). After the win auto-returns,
  the shared route **re-enters the now-cleared course**; the permit fails closed
  on the `RACE_CLEARED` bit, so the second race mints no token and writes no flag
  — the save still shows one clear and balloons `(2,1)`.
- **No mutation on a non-win.** A CPU-first loss (`MDKR_AP_RACE_WINNER=cpu`) and a
  mid-race quit-to-lobby persist status VISITED only, write no `RACE_CLEARED`, and
  consume no token.
- **Round-trip.** The party-progressed save loads + resumes with the enhancement
  OFF: a 1P process reads Ancient Lake back as CLEARED (`[BOSSW]`) and its
  checksum survives.
- **Positive controls (output/replay only).** A CPU-loss output replayed through
  the win assertions must FAIL; a win output with its token consume + flag write
  duplicated must FAIL the exactly-once assertion.

Keys and hub balloons are exact-once + collector-agnostic *structurally* and are
NOT re-plumbed (see the report): `obj_loop_worldkey` commits `settings->keys` for
ANY live human (`playerIndex != PLAYER_COMPUTER` — every party seat qualifies),
the bit is idempotent, `free_object` retires the key, and `obj_init_worldkey`
deletes an already-collected key on re-entry. A read-only `worldkey:` trace
(mirroring `silvercoin:`) and an `MDKR_OBJDUMP` `WORLDKEY` line make the collector
observable.

Save fixture: the same started Adventure One slot-0 save; every run is in a
private temp dir. Off arm — 1P awards unchanged — is `check_adventure_race_loop.py`
and `check_campaign_progression.py`.

### Adventure Party host-solo challenge — `tests/check_adventure_party_challenges.py`

```bash
python3 tests/check_adventure_party_challenges.py         # ~15 min, muted + headless
python3 tests/check_adventure_party_challenges.py -v
```

The AP-15 gate: a party entering a four-racer special challenge SUSPENDS to a
retail host-solo challenge and is RESTORED to the exact party on return. Proven
from the running binary + the persisted EEPROM (3P, a 2P arm, and a 1P reference):

- **Suspension envelope.** The challenge loads host-solo — `aparty_session
  state=SOLO_ACTIVITY` with the party roster captured as the suspended facts, the
  retail FOUR-racer field (`[CHALLENGE] racers=4`), and NO party race field (no
  `racefield:`).
- **Exact-once win.** A host win commits the retail amulet EXACTLY once
  (`courseFlags RACE_CLEARED` + `ttAmulet++`) through the unchanged challenge-finish
  path, gated by one `aparty_award` challenge token issue+consume
  (`kind=COMPLETION_CHALLENGE`); the persisted 40-byte slot is BYTE-IDENTICAL to a
  **1P** win of the same challenge resumed from the same started save (empty
  whitelist, the house compare from `check_adventure_party_progress.py`).
- **No commit on defeat.** `MDKR_CHALLENGE_OUTCOME=loss` mints no token, leaves
  `ttAmulet` unchanged, and still restores the party.
- **Restore + re-entry.** Every return runs `RESTORING_PARTY` and
  `aparty_restore match=1`; the challenge re-enters (a 2nd `SOLO_ACTIVITY`).
- **Positive controls (output only):** a restore-with-different-roster replay must
  FAIL restore; stripping the suspension traces must FAIL suspend.

Entry: the world-lobby challenge/key door is unreachable by a headless AI line, so
the party drives a reachable race door and that load is retargeted to the challenge
(`MDKR_LOAD_TRACK`) — the same arrival-adapter seam (`ACTIVE_LOBBY` →
`RACETYPE_CHALLENGE` load → `SOLO_START` → restore). `MDKR_CHALLENGE_OUTCOME` drives
the production challenge to its win/loss. Off arm — 1P challenges unchanged — is
`check_challenge_modes.py` / `check_campaign_progression.py`.

### Adventure Party host-solo boss — `tests/check_adventure_party_boss_restore.py`

```bash
python3 tests/check_adventure_party_boss_restore.py       # ~15 min, muted + headless
python3 tests/check_adventure_party_boss_restore.py -v
```

The AP-17 gate: a 3P party entering a boss SUSPENDS to a retail host-solo boss and
is RESTORED on return. Proven from the running binary + the persisted EEPROM:

- **Suspension envelope.** `aparty_session state=SOLO_ACTIVITY` with the suspended
  roster; no party race field (`racefield:` absent); the host (`playerIndex 0`) is
  the sole human at the boss finish.
- **Exact-once first win.** A first win commits `settings->bosses |= worldBit`
  (save delta `bosses 0x0 -> 0x2`, boss course CLEARED) through the unchanged
  `racer_boss_finish` path, witnessed by one `aparty_award` boss token issue+consume
  (`kind=COMPLETION_BOSS`); the win cutscene (level 57) presents.
- **No award on defeat.** A loss sets no boss bit and consumes no token, and still
  restores the party.
- **Rematch.** Re-entering the beaten boss re-suspends host-solo (a 2nd
  `SOLO_ACTIVITY`) and refuses a new token (`aparty_award op=issue result=0`).
- **Restore.** `aparty_restore match=1` on the returns.
- **Positive controls (output only):** a return-with-one-racer replay must FAIL
  restore; a doubled boss-award replay must FAIL exactly-once.

Entry is by the same reachable-race-door retarget as the challenge gate (the boss
door is behind geometry no headless line paths through, and the production
balloon-race route that reaches it crashes a party's spatial audio — a pre-existing
AP-12 defect, see `check_adventure_party_boss_restore.py`'s docstring and the task
report). The boss-door warp and `racer_boss_finish` are retail, 1P-anchored by
`check_first_boss_progression.py` / `check_boss_win_verdict.py`.

### Adventure Party team-shared silver coins — `tests/check_adventure_party_silver.py`

```bash
python3 tests/check_adventure_party_silver.py            # ~20 min, muted + headless
python3 tests/check_adventure_party_silver.py -v
```

The AP-14 gate: in a party silver-coin race the coins are TEAM-SHARED — any human
collects, all viewports see it vanish, one team counter, eight team coins plus any
human first wins, awarding the retail silver clear exactly once. Proven from the
running binary + the persisted EEPROM (3P central scene, a 2P arm, a 1P reference):

- **Team-shared collection, any human.** The `silvercoin` trace shows a NON-HOST
  seat (`playerIndex>0`) collecting a coin, every coin retired for ALL viewports
  (`invis=0x600` = both engine invisibility bits, so viewports 0/1 directly and
  2/3 by the `(viewport & 1)` alias), and the ONE team tally incrementing `1..8`
  each exactly once (no double-collect).
- **HUD parity.** Every party viewport draws the same one team total
  (`silverhud viewport=N teamCoins=..`, all N reaching 8).
- **Exact-once win.** Eight team coins + a human first commits
  `RACE_CLEARED_SILVER_COINS` once through the unchanged `set_course_finish_flags`
  path — reading the TEAM total, not the leading racer's own `silverCoinCount`
  (`silvercoinfinish teamCoins=8 coins=0`) — gated by one `aparty_award` SILVER
  token (`activity=3`) issue+consume; the persisted 40-byte slot is BYTE-IDENTICAL
  to a **1P** silver win of the same course (empty whitelist, the house compare).
- **No award on a CPU-first finish.** With eight team coins banked but no human
  first (no `MDKR_ADVENTURE_WIN` rotation), no token is minted and no silver flag
  written.
- **Replay.** Re-entering after the silver clear is no longer a silver race
  (`RACE_CLEARED_SILVER_COINS` -> `gIsSilverCoinRace` FALSE): no coins, no second
  award, no crash — driven directly by the win arm's two Ancient Lake loads.
- **Positive controls (output only):** a seven-team-coin output must FAIL the win
  assertions; a coin-trace-stripped output must FAIL them too.

Entry: a headless party cannot drive a boss-beaten world lobby's silver-coin door
(the door only appears with the boss beaten + course cleared, and that lobby
repositions the party far from it — measured across 15 attempts). So, per the R20
retarget precedent, the party drives the PROVEN started-save route into Ancient
Lake (which `check_adventure_party_progress.py` navigates and wins) and
`MDKR_SILVER_FORCE=5` flips THAT course to silver at its own load only — the world
lobby stays the reachable boss-not-beaten started-save lobby, and the coins,
counter and award are the game's own party path. Adventure Two: **NOT RUN**
(AP-16 owns the A2 matrix). Off arm — 1P silver unchanged — is
`check_campaign_progression.py` (seam A).

### Adventure Party trophy series — `tests/check_adventure_party_trophy.py`

```
python3 tests/check_adventure_party_trophy.py            # ~20 min, muted + headless
python3 tests/check_adventure_party_trophy.py -v
python3 tests/check_adventure_party_trophy.py --players 3
```

AP-16 Part A (the last v1 activity ticket). **Measurement first:** the retail
Adventure trophy race fields EIGHT racers (`track_setup_racers` defaults
`gNumRacers` to 8, a 1P trophy keeps it, and the rankings width is 8). Part A
measured that this eight-racer field renders four viewports within the per-frame
DL/matrix/vertex budgets — the 4P DL high-water is **6354 Gfx**, far under the
**11000** 4P budget (and under the always-present 4-viewport hub's 9670) — so the
trophy row is a **SPLIT** (N humans + 8-N CPUs to the retail eight-racer total, N
viewports), never a shrunken/improvised field. The gate proves, for a 4P (or
`--players 3`) party: entry to the real trophy series, every round is the retail
8-racer split (`racefield total=8`), all four production rounds run with
accumulating standings (`trophyround points0` climbs, four `trophyrankings`), and
a gold championship writes the Dino trophy exactly once via **one
`COMPLETION_TROPHY` token** (`aparty_award` issue+consume, activity=8 kind=3)
beside the upgrade-only retail write, persisting a save slot **byte-identical to a
1P gold** (the whole 40-byte progression slot, empty whitelist — which subsumes a
no-balloon / no-`RACE_CLEARED`-write assertion for the trophy rounds); the series is
one `ACTIVE_RACE` span returning to the same lobby. A 1P-ref
awards the same gold with **no** party token. Two positive controls fire
(duplicate-consume, stripped-field). **Route reachability (R20/R24 precedent):** a
headless party cannot drive the world-lobby cabinet (a boss-beaten fixture wedges
on a central-hub Taj-summon `SHARED_DIALOGUE`; the boss-beaten lobby repositions
the party), so the gate keeps the reachable boss-not-beaten lobby (the AP-13
progress route) and `MDKR_TROPHY_FORCE_ENTER` forces the cabinet's own
`begin_trophy_race_teleport()` there — the least-fake entry; the cabinet's
precondition-gated collision/dialogue is covered 1P by `check_trophy_series.py`.

### Adventure Party — Adventure Two matrix — `tests/check_adventure_party_adventure_two.py`

```
python3 tests/check_adventure_party_adventure_two.py     # ~30 min, muted + headless
python3 tests/check_adventure_party_adventure_two.py -v
```

AP-16 Part C. Adventure Two is a **required matrix arm, not a separate
implementation**: an A2-flagged party fixture (`CUTSCENE_ADVENTURE_TWO` set,
Adventure Two picked at GAME SELECT via a DOWN) drives the SAME party policy with
**no new game-side branch** (the boundary scanner is unchanged; the party
admission, hub-formation, race-field, silver and award adapters are all
A1/A2-agnostic — the A2-specific mirroring / A2 coin init is orthogonal retail code
keyed on `is_in_adventure_two`). Three arms: (1) 3P admission → hub on A2 (the party
forms a 3-seat session and resumes the A2 save); (2) a default race win is exact-once
(`aparty_award` + one `RACE_CLEARED`) and its save slot is **byte-identical** to a 1P
A2 win of the same mirrored course; (3) a silver scene banks ONE team tally over the
**A2 coin object set** (`obj_init_silvercoin_adv2`) — any human collect, `invis=0x600`
for all viewports, one SILVER award — the AP-14 machinery unchanged. The load-bearing
no-new-adapters witness is `racefield: level=5 humans=3 cpus=3 total=6 viewports=3`
with `adventure_mode: level=5 adventureTwo=1 mirrored=1` (the identical six-racer
party split on the mirrored course). Positive controls fire. If A2 had needed any new
game-side branch this would be BLOCKED (it did not).

### Adventure Party Taj transaction — `tests/check_adventure_party_taj.py`

Separate selection-music check: `python3 tests/check_taj_selection_music.py
--build build-rel --rom baserom.us.v80.z64` requires actual loaded sequence
27 followed by 26 and nonzero-volume MIDI notes in both intervals. Empty,
missing-load and zero-volume evidence must fail. This muted native GPU check
does not establish audible quality or complete authored instrumentation.
By default all six scenarios run with fresh isolated preferences: confirmation,
reselection (27→26→27→26), title exit (27→7), and multiplayer join/leave
(27→26→27→26, with exact controller ownership checks), and custom selection
(27→26, with a generated package and an actual Diddy-donor selection witness),
plus custom-to-Taj return (27→26→27→26, requiring custom identity cleanup).
The custom fixture uses the existing test-only synthetic GLB validator, not a
change to the shipped importer. Select one with
`--scenario confirm|reselect|title|multiplayer|custom|custom-return`. Each loaded interval must
emit nonzero-volume MIDI notes. These scenarios run across all four retail
unlock layouts (base, Drumstick, T.T., complete), for twenty-four independent arms.
Use `--layout base|drumstick|tt|complete` to restrict a diagnostic run.
Complete authored instrumentation and audible quality remain separate acceptance work.

Includes 2P/3P hovercraft and 2P plane changes after a non-host summon. At
dialogue exit, every live seat must match its roster character, target vehicle,
full opacity and non-hidden flags. Synthetic missing/transparent-seat witnesses
must fail. These diagnostics complement, rather than replace, retained visual
captures; they caught the party-wide fade-out/focus-only fade-in asymmetry.

```bash
python3 tests/check_adventure_party_taj.py            # ~5-6 min, muted + headless
python3 tests/check_adventure_party_taj.py -v
```

The AP-11 gate: Taj's vehicle transform rebuilds the WHOLE party transactionally
instead of the retail single racer, and the shared dialogue is a proper
SHARED_DIALOGUE envelope. Read from the running binary's `aparty_` traces plus the
`aparty_transform` diagnostic:

The default matrix includes both car-to-hovercraft and car-to-plane in 2P and
3P, with per-seat post-transform input identity and opaque/unhidden state.
`--texture-pack /path/to/pack.zip` repeats those party arms and challenge refusal
with the ZIP copied into private preferences; every party arm must demonstrate
actual replacement-texture use. Run once without this option for the isolated
no-pack baseline. The retail off-arm remains an isolated no-pack control.
These traces prove state restoration, not pixel-level visual correctness or
physical-controller acceptance; actual rendered inspection is separate.

- **Non-host summon, host choice.** Seat 1 drives into Taj (`MDKR_AP_SEAT_ROUTE`);
  any occupied seat may trigger the interaction (`aparty_interaction seat=1
  action=2 verdict=0` then `aparty_session state=SHARED_DIALOGUE`), and the host
  owns the menu regardless of who summoned (`taj_menu_loop` reads
  `input_pressed(PLAYER_ONE)`; `aparty_interaction seat=0 action=0 verdict=0`).
- **Whole-party transform.** Retail `transform_player_vehicle` rebuilds the roster
  as exactly ONE racer; the party path rebuilds ALL N with the new shared vehicle
  and the same seat->character identities. The oracle is `aparty_transform
  path=party n=N live=N` — `live` is the post-rebuild `gNumRacers`, so a collapse
  reads `live=1`. Exactly one transform is emitted (rebuilt once), the roster
  (`aparty_roster`, characters unchanged) and split layout (`aparty_layout
  viewports=N`) are republished, and the `[RACERINPUT]` witness confirms
  `port == player == racer` for every seat afterward (binding survives the
  transform).
- **R10 latch lifecycle.** The dialogue latch RELEASES on completion
  (`SHARED_DIALOGUE -> ACTIVE_LOBBY` at the SAME level generation — the transform
  is within-lobby, never a reload), and no transition fires between the latch and
  the release (retail freezes racer input during the scene, and the arbiter
  rejects any door while the session is not `ACTIVE_LOBBY`). Doors working FROM
  `ACTIVE_LOBBY` is proven by `check_adventure_party_transition`.
- **R27 challenge refusal.** The in-hub Taj CHALLENGE rows are not classified for
  a party yet (a challenge would spawn a lone Taj-vs-host race and strand the other
  seats), so a host CHALLENGE-row selection during the party `SHARED_DIALOGUE` is
  fail-closed refused (`aparty_taj_challenge: ... refused=1`) instead of starting a
  race. Run with `MDKR_TAJ_PROBE=1` so `[TAJ] phase=accept`
  (`init_racer_for_challenge`) would appear IF a challenge race started — it must
  NOT; no extra racer spawns; the party is intact; and a vehicle TRANSFORM still
  works afterward (the same whole-party oracle, `live==3`). The refusal keys on a
  live party SESSION, never on player count — the fixture unlocks a challenge
  (`TAJ_FLAGS_CAR_CHAL_UNLOCKED`) so the row is reachable, and vehicle-transform
  rows are unaffected.
- **Positive controls.** Rewriting the transform line to `live=1` (a collapsed
  roster) must FAIL the whole-party assertion; stripping the `SHARED_DIALOGUE`
  lines must FAIL the latch assertion; stripping the `aparty_taj_challenge` line
  must FAIL the R27 refusal arm; and injecting a `[TAJ] phase=accept` line must
  FAIL the R27 no-race oracle.
- **Off arm.** `check_taj_p2_adventure.py` is run and quoted (retail JOINTVENTURE
  2P Taj, the lead-swap machinery a party never engages, is untouched — and it
  reaches challenges normally, which the R27 refusal must never block: the refusal
  keys on a party session, not on player count).

Only the summoner drives: a second moving racer near a lobby door raises a door
textbox that blocks `npc_dialogue_loop` (and diverges shell-vs-headless), so the
deterministic route keeps exactly one mover and the menu is host input. Save
fixture: the started Adventure One slot-0 save; every run is in a private temp dir.

### Adventure Party with custom characters — `tests/check_adventure_party_custom_characters.py`

```bash
python3 tests/check_adventure_party_custom_characters.py \
  --build build --rom baserom.us.v80.z64
```

Cross-feature release gate for D1, D2, D6 and D7. It builds and installs four
license-clean generated packages matching the exact retail donors selected by
the 2P/3P party fixtures. A real 3P admission must publish the ordinary donor-id
party roster while every seat renders its own package HUD identity. Taj's
whole-party vehicle rebuild and a host-solo boss return must publish the same
package-to-seat relationships at their live-racer seams. Finally, a 2P party
win with customs must write a slot byte-identical to the same party win without
custom presentation. Package ids remain outside the reducer and EEPROM; the
`aparty_custom_identity` trace joins presentation to a live donor racer only for
diagnostics. All arms require real WebGPU replacement draws, and donor mismatch
plus missing transformed-seat positive controls must fail.

### Harness isolation — `tests/check_harness_isolation.py`

```bash
python3 tests/check_harness_isolation.py
```

Source gate that keeps the taj-theme false-failure class extinct. A check that
sets `MDKR_SAVE_DIR` but not `MDKR_VIDEO_CONFIG_PATH` inherits a repo-root
`mdkr64.ini`; with display-paced motion smoothing configured there, the
engine's `--headless-frames` budget counts presents instead of authored ticks
and scripted drives fall short of their goal (the mechanism documented in
`check_door_blocks.py`). The gate fails the moment a check acquires a
save-dir assignment without a video-config pin (or `save_env`), with two
documented exemptions (`check_ghost_bank_capacity.py`, `check_shell_dropfile.py`)
and a `--self-test` proving the scanner is not vacuous.

### Save-directory hermeticity — `tests/test_check_save_dir_hermeticity.py`

```bash
python3 tests/test_check_save_dir_hermeticity.py        # ctest: check_save_dir_hermeticity
python3 tests/test_check_save_dir_hermeticity.py --list  # the offender list, not a verdict
```

ROM-free source gate for the sibling defect class the 1.6.0 fixture wave
uncovered. A check that builds a "hermetic" engine environment by dropping
every inherited `MDKR*` variable also drops the `MDKR_SAVE_DIR` that
`tools/run_checks.py` exports per task. Since issue #54 a non-packaged build no
longer resolves an unpinned save to `$CWD/save`; it resolves it to the shared
per-user directory, so an adventure-in-progress EEPROM sitting there — left by
a developer playing, or by an earlier suite task — re-routes the boot flow of
every such check and reds it for a reason that has nothing to do with the code
under test ("level never loaded … no `[PVEH]`", "0 flap cues", "trophies 0x0",
"capture-window positions=0").

The gate parses every `tests/check_*.py` and follows each environment value
from where it is built to where it is handed to a process, so a pin has to be
on the environment that actually launches — a pin elsewhere in the same
function does not satisfy it. It recognises the scrub in every shape the
corpus uses: an `os.environ` comprehension that filters `MDKR` keys, a
`pop("MDKR_SAVE_DIR")` or `del env["MDKR_SAVE_DIR"]`, an environment written
out inline at the launch that inherits nothing, and a factory function whose
scrubbed return value the caller launches with. Repairs it accepts:
`harness_utils.save_env(env, run_dir)` (preferred — it pins the video config in
the same breath, which `check_harness_isolation.py` requires), an explicit
`env["MDKR_SAVE_DIR"] = …`, `MDKR_SAVE_DIR=…` passed into the call that builds
the environment, or simply inheriting `os.environ` unscrubbed. Controls: nine
synthetic fixtures ship with the gate — five offending shapes it must reject
and four pinned shapes it must accept — and they run ahead of the corpus scan
on every invocation, so a scanner that stopped matching fails loudly instead of
passing an empty sweep. One documented exemption,
`check_portable_paths.py`, whose subject *is* unpinned save resolution.

### Delta inventory — `tests/check_delta_inventory.py`

```bash
python3 tests/check_delta_inventory.py
```

Source gate for the M0 classification inventory (`docs/CAMPAIGN_FAITHFUL_
ENHANCED.md`). `//!@Delta` marks a per-tick constant authored for the 30 Hz
tick that does not scale with the Enhanced 60 Hz simulation cadence -- the
defect class behind issue #26 (boss racers unbeatable at 60 Hz) and the
measured 2x menu auto-repeat. Marking a site is only half the job: M1's fix
shape depends on which of four kinds of per-tick operation it is, so the gate
requires every marker to carry one right there in the comment --
`//!@Delta CONTINUOUS`, `DISCRETE`, `THRESHOLD`, or `INERT`, optionally
followed by `: <rationale>` -- and fails a bare, unclassified `//!@Delta`.
It is ROM-free and holds no opinion on whether a classification is correct,
only that one is present. 66 markers are classified today: 43 CONTINUOUS
(scale by `updateRateF * 0.5` or `k ** (updateRate / 2)`), 10 THRESHOLD
(scale the compared bound, leave the counter alone -- the landed example is
`menu_stick_delay_amount()` in `game/src/menu.c`), 13 INERT (equilibrium-
governed or already scaled on the same path, each with its proof in the
comment), and 0 DISCRETE (this per-tick-constant family did not happen to
produce a pure gated-event site; `check_cadence_gating.py` covers the
discrete-gating half of the M3 split separately). One line
(in `game/src/racer.c`) mentions the marker family in prose rather than
being a marker itself and is excluded by exact line content, so a real new
marker landing on that exact line cannot be silently swallowed. Positive
control: a seeded bare `//!@Delta` must be rejected; a negative control
proves a properly classified marker is accepted.

### Test assertions armed — `tests/check_test_assertions_armed_wrapper.py`

```bash
python3 tools/check_test_assertions_armed.py --self-test
```

Structural gate for `tools/check_test_assertions_armed.py` (the suite runs it
through the wrapper). CMakeLists.txt defaults `CMAKE_BUILD_TYPE` to Release
when unset, and Release adds `-DNDEBUG`, which turns every `assert()` in
`<assert.h>`/`<cassert>` into `((void)0)` -- the call still compiles, the
check it guards silently never runs, and the test still reports PASS. 22
test files shipped exactly this way once, fixed with a per-file `#undef
NDEBUG` before the assert.h/cassert include; a sister project repeated the
mistake with an allowlist that fell out of date as files were added. This
gate has no allowlist: it scans every `tests/*.c` and `tests/*.cpp` TU that
calls `assert(` and requires it to be either TU-armed (`#undef NDEBUG`
precedes its own assert.h/cassert include) or target-armed (the CMake
target it is compiled into, found by searching `cmake/tests.cmake` and
`CMakeLists.txt` add_executable source lists, carries a `-UNDEBUG` compile
option -- `NDEBUG=0` is explicitly rejected, since `<assert.h>` only tests
whether NDEBUG is `#defined`). `--self-test` drives both arming predicates
in both directions -- an undef-free TU, a too-late `#undef`, an assert(
mentioned only in a comment, a CMake target with and without `-UNDEBUG`, and
an end-to-end scan over a synthetic offender -- before it scans the real
tree, so a predicate that stopped firing fails loudly instead of passing an
empty sweep.

### CI contract — `tests/check_ci_contract.py`

```bash
python3 tests/check_ci_contract.py
```

Fails closed when the workflows lose required coverage: push/PR/manual policy,
the native matrix, the sanitizer lane, linked wasm and save custody, ROM guards,
release provenance, and the immutable action pins. It carries its own negative
fixtures — every guard is re-run against a deliberately mutated copy of the
source it inspects and must reject it. It also derives the release version from
`CMakeLists.txt`'s `MDKR_VERSION` and requires this file's task list to match
`tools/run_checks.py`'s manifest.

The GPU-routing contract also compares the lobby-takeover CTest success count
with the check's actual active-view inventory. Stale counts, inventory drift,
and removal of that success predicate are rejected by mutation controls; the
application check still asserts offline-shell suppression for every active arm
and the opposite behavior at the entry chooser.

The exact text it pins lives beside it in `tests/ci_contract_manifest.py`: one
`Pin` per literal a named file must, or must never, contain, and one `Control`
per deliberate break of a pin. Reword a workflow step, a refusal message or an
artifact filename and you edit the pin and its control in the manifest; the
check itself only holds what it computes — version agreement, the three-way
frame-limit help comparison, occurrence counts, step ordering, the structural
regexes over CMake and the packagers' archive manifests, the no-write output
guards, and this file's coverage sweep.

### Optimized full-UBSan route gate — `tests/check_full_ubsan.py`

```bash
python3 tests/check_full_ubsan.py --rom baserom.us.v80.z64
```

Builds its own `-fsanitize=undefined,float-cast-overflow` tree, verifies the
required handlers actually linked, runs its positive controls, then drives the
broad content and stateful routes. It holds an exclusive lock on its build
directory: two concurrent runs would interleave a reconfigure with a compile and
fail for reasons unrelated to the code under test.

### Post-engine process replacement — `tests/check_restart_apply.py`

```bash
python3 tests/check_restart_apply.py --build build-rel --rom baserom.us.v80.z64
```

Drives both of the production app's real post-engine transitions from a private
extracted-layout fixture on a deep Unicode path whose package directory contains
a space, exactly as a player's install does — no test seam replaces the
executable or bypasses `main_app.cpp`. For Restart & Apply, successful
replacement is one arm; the post-replacement boot failure and the
handoff-staging failure are the other two, and each must clear the one-shot
controls, return to the visible launcher recovery, and leave the copied ROM and
settings intact. Return to Launcher stages no ROM handoff and must arrive as a
launcher invocation rather than an argument-bearing automation invocation, which
`arg_triage` sends straight to the windowless engine — to the player that reads
as the application closing. The gate asserts the exact invocation each process
in the exec chain received, and the replacement must still find the durable ROM
selection.

### Staged-web provenance fixtures — `tests/check_release_ready_web_provenance.py`

```bash
python3 tests/check_release_ready_web_provenance.py
```

Deterministic fixtures for `tools/ci/check_web_build_provenance.py`, the guard
the release-readiness script runs over a staged `dist/web`. Each case writes a
synthetic `build-info.json` and pins the exact exit code and message: the clean
exact candidate must pass, while a dirty tree, a source commit that is not HEAD,
a missing version, and a wrong version must each be rejected with the reason
named. A provenance guard that stopped inspecting anything fails here instead of
accepting every candidate.

### Custom-character binary fuzzer — `tests/fuzz_modern_character_asset.cpp`

Use an authorized test environment, including for the corpus generator below.
The KTX2/BasisU alignment finding has a local decoder amendment and a
valid-texture regression covering all four output formats across host buffer
alignments. Release qualification still requires sanitizer fuzzing and exact
artifact checks; prior runs do not certify a changed decoder. Keep discovery
artifacts private and discuss findings at defensive engineering level.

```bash
python3 tests/generate_modern_character_fuzz_corpus.py
MDKR_BASISU_LOCAL_CACHE=/path/to/pinned-basisu-mirror \
  cmake -S . -B build-fuzz -DMDKR_ENABLE_FUZZERS=ON \
    -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
    -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
nice -n 15 cmake --build build-fuzz \
  --target mdkr_modern_character_asset_fuzzer --parallel 2
CHARACTER_FUZZ_ROOT="$(mktemp -d /tmp/mdkr-character-fuzz.XXXXXX)"
cp -R tests/fuzz_corpus/modern_character_asset "$CHARACTER_FUZZ_ROOT/corpus"
nice -n 15 ./build-fuzz/mdkr_modern_character_asset_fuzzer \
  -max_total_time=600 -artifact_prefix="$CHARACTER_FUZZ_ROOT/" \
  "$CHARACTER_FUZZ_ROOT/corpus"
```

ASan+UBSan libFuzzer coverage crosses all three custom-character binary trust
boundaries in one process: every input reaches the exact shipped MDKC memory
loader, the PNG-only/no-stdio `stb_image` build, and the exact Basis Universal
KTX2 inspect/transcode bridge. Valid containers also feed authenticated embedded
PNG/KTX2 payloads and the portrait PNG through their matching decoder. Decode
allocation is capped at 16 MiB per input; production limits remain stricter and
unchanged. The deterministic corpus contains valid and truncated generated PNG,
valid ETC1S and UASTC/Zstandard textures, a valid generated character,
checksum-valid malformed/truncated controls, and the minimised reproducer for
the wrapping-index out-of-memory below. Its generator uses only the
license-clean test fixtures and must reproduce the tracked bytes exactly. Run
against a copied corpus when discoveries should remain local; libFuzzer may add
minimized inputs to a writable corpus directory.

The BasisU target is built with `fuzzer-no-link` instrumentation and the exact
`stb_image_impl.c` translation unit is compiled into this dedicated target, so
coverage enters both third-party decoders instead of stopping at their
first-party bridges. `MDKR_ENABLE_FUZZERS` remains OFF by default and does not
add fuzz code or sanitizer flags to ordinary release builds.

### KTX2 bridge bounds — `tests/test_modern_character_ktx2.cpp`

```bash
ctest --test-dir build -R modern_character_ktx2
```

Drives `mdkr_ktx2_inspect` and `mdkr_ktx2_transcode` over two license-clean
textures embedded in the test — 8x8 ETC1S/sRGB and 8x8 UASTC/linear Zstandard —
pinning the reported dimensions, mip count, colour space and the exact
allocation size for every target format, and requiring a truncated file to be
refused.

Five hostile arms patch one header field into the valid UASTC texture and
require both entry points to refuse it, naming the bound that refused it. Four
cover the KTX2 index's offset/length pairs — key/value, data-format,
supercompression global data, and a level's own byte range — each with a pair
whose sum wraps its width, which the pinned transcoder reads as a small region
and then walks or sizes from the length the pair actually holds. The key/value
arm is the minimised libFuzzer out-of-memory reproducer, seeded as
`tests/fuzz_corpus/modern_character_asset/wrapping-key-value-length.ktx2`. The
fifth arm declares a level uncompressed size far beyond what its mip spans,
which the Zstandard path allocated whole before reading the file. See
`docs/open-items/misc.md`, wave "ktx2index". With either bound removed the
matching arms fail: one by crashing on the walk, one refused for the wrong
reason after an out-of-bounds read, and three by accepting the file.

### Online wire-parser fuzzers — `tests/fuzz_match_signal_wire.cpp`, `tests/fuzz_online_live_wire.cpp`

```bash
cmake -S . -B build-fuzz -DMDKR_ENABLE_FUZZERS=ON \
  -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
  -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
cmake --build build-fuzz --target mdkr_match_signal_wire_fuzzer \
  --target mdkr_online_live_wire_fuzzer -j6
./build-fuzz/mdkr_match_signal_wire_fuzzer -max_total_time=300 \
  tests/fuzz_corpus/match_signal_wire
./build-fuzz/mdkr_online_live_wire_fuzzer -max_total_time=300 \
  tests/fuzz_corpus/online_live_wire
```

Coverage-guided libFuzzer (ASan+UBSan) over the two hand-rolled wire-parser
families in the native online stack: the match-signal client's RFC 6455
server-frame extractor + server-message validation state machine, and the
MatchRoom transport's HTTP/1.1 response parse, /connect WS frame decoder and
lobby/ice-server JSON mapping. Each target drives a shipped `*_fuzz_wire`
seam (compiled unconditionally, stripped from production links), so the
corpus exercises the exact production parsers — no copies, no sockets, no
threads. `MDKR_ENABLE_FUZZERS` is OFF by default and purely additive: a
normal configure never creates the targets. Apple's toolchain clang ships no
libFuzzer runtime; use Homebrew LLVM as above. Seed corpora are derived from
the suites' wire-level test vectors (valid welcome/hello/ICE frames,
fragmentation, masked/RSV violations, oversize declarations, a full 201
create response, chunked state, 4000-class closes).

## Rice packs — `rice_crc_parity`, `ricepack_import`, `rice_crc`

```bash
ctest --test-dir build --output-on-failure \
    -R '^(rice_crc_parity|ricepack_import|rice_crc)$'
```

A Rice/GLideN64 high-resolution pack now loads at runtime, as downloaded: the
renderer computes the pack's own texture identity while it draws and asks the
registry for a replacement. `tools/ricepack/` still exists and still converts a
pack into a digest-keyed content pack, but nothing requires it any more — these
three gates cover the runtime path and the tool respectively. All three are
ROM-free, run in under a tenth of a second, and build every fixture they need —
a handful of four-pixel PNGs, plus deliberately dishonest headers for the cap
tests.

| Unit | What it owns | The assertion that would otherwise rot |
|---|---|---|
| `rice_crc_parity` | `platform/rice_crc.c`, the C port the renderer calls | Nine vectors generated from `tools/ricepack/rice_crc.py` at its default variant, covering every RDP size code, an odd width, a pitch wider than a row, and the smallest geometry the key is defined over; plus five refusals (an undefined `siz`, a zero width, a pitch narrower than a row, a span too short for the walk, and a row shorter than one accumulator read). If the two implementations drift, shipped packs stop resolving while the offline importer keeps working — a split neither side can see alone, and each refusal is a *silently wrong key* rather than a crash if its guard goes |
| `ricepack_import` | The filename allowlist, the `_all`-over-split precedence, the orphan-half rules, the caps and the manifest | The junk files fall out of the *pattern* — the allowlist is asserted to contain no `Thumbs`/`DS_Store` literal, so a special case cannot creep in; the same pack decides identically with its `_all` sorting before or after the halves it shadows; a re-run is byte-identical; and `complete` coverage is false whenever any input was refused |
| `rice_crc` | The Python side of the same arithmetic, used by the measurement tool below | The CRC-32 primitive is checked against `zlib.crc32` rather than a remembered constant; row padding is proven to be outside the hash; and the four accumulation variants are proven distinct, so a hit-rate sweep across them measures something |

What no unit here can show is that the arithmetic is the arithmetic Rice uses:
these gates prove the C and the Python agree with each other and with their own
vectors. That question was settled by measurement against a real pack, and the
discriminator is below.

The measurement that settles it is a real-ROM experiment, not a test, and runs
as one command:

```bash
python3 tools/ricepack/measure_crc_variants.py \
    --build build --rom baserom.us.v80.z64 \
    --pack /path/to/rice-pack --out ~/dkr-crc-experiment
```

It drives four routes with `MDKR_MOD_TEXTURE_DUMP` set, computes all four
candidate CRCs over every dumped `<digest>.texels` span, and prints how many of
the pack's mappable keys each variant matched. A key counts only when the CRC
*and* the fmt/siz agree, so a wrong variant is expected to score exactly zero —
which is what makes a single-digit result readable as noise and a three-digit
one readable as the answer. All-zero across the four means the input is wrong
before the arithmetic is; the skip counts printed above the table say which.
`--out` must be outside the repository and is refused otherwise, twice.

## Release-guard failure controls

Pack-install cancellation is also checked by `pack_installer`: callbacks stop
a completed copy before validation or after validation but before publication.
Neither case may create the destination. `pack_install_job` drives the
launcher's install job the way a frame does: a copy that finishes in the same
frame as a Cancel click or Quit is never published, a copy refused at
publication leaves the winning pack untouched, leaving the launcher never
publishes, and each discarded copy is deleted in the background. Where
`Launcher::draw` calls the service is not pinned by a gate; this is not an
end-to-end pending-job mouse/gamepad cancellation witness, which remains
separate from the app-shell, quit and lobby-takeover smoke tests.

`python3 tests/test_source_custody.py` checks the release dependency inventory
in disposable repositories. Untracked and staged-but-uncommitted source inputs
must both fail: `git archive HEAD` contains neither. Missing working-tree inputs
also fail. Source areas are game/platform/tests/tools/cmake/.github/services/lib
plus the root CMake recipe, including service manifests, lockfiles and vendored
library inputs; ignored build output and private planning docs are not swept into
the publication inventory. `python3 tools/ci/check_source_custody.py --json`
produces a review list without staging, committing or deleting anything. The
check runs from the release-readiness guard and its controls are the
`source_custody` CTest. It checks path custody, not modified-file content
equivalence, transitive build dependencies, licensing or clean-archive build
success. Those remain separate release requirements. Never bulk-add private
artifacts simply to make the inventory pass.

`python3 tests/test_clean_room_guard.py` exercises the public Bash entry point
in disposable Git repositories, without ROMs or windows. It checks clean input,
failed Git inventory/read commands, malformed metadata, truncated blob streams,
all three four-byte header signatures (including tiny synthetic files), deleted
historical texture-dump names (including merge-only additions), forced pack
additions, missing ignore rules,
oversized untracked files and shell-shaped filenames. The test is also registered
as `clean_room_guard` in CTest. Scratch repositories are temporary; Git identity
and configuration are isolated from the maintainer's settings.

The implementation uses NUL-delimited Git paths and checked subprocesses, not
filename interpolation into shell commands. Blob contents are streamed in
64 KiB chunks through one `git cat-file --batch` process. Git/protocol/read
errors fail closed; Python-owned temporary stderr storage replaces predictable
scratch filenames. Blob-size warnings remain provenance-review prompts rather
than automatic failures. These heuristics are not a complete license audit or
proof that arbitrary content is non-derived.
