# 1.7.0 release work inventory and parallel ownership

> Historical 1.7 inventory, not the current 1.8 qualification verdict. For the
> active `release/1.8-integration` work, consult the private 1.8 integration
> record, the
> [custom-track parity scope](DKR_R_CUSTOM_TRACK_PARITY_REVIEW.md), and current
> status at the top of the [release checklist](RELEASE_CHECKLIST.md).
> Historical execution deferrals below do not revoke the maintainer's current
> local-validation authorization. Outstanding acceptance requirements still
> need evidence against the final candidate.

Evidence cut: 2026-09-06, `int-1.7.0`, base `c657fa62` plus the local trophy,
pacing-integrity, Workshop readiness and native join-editor corrections.
This is a coordination inventory, not a
release approval. [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) remains the
command-by-command gate authority; [the acceptance guide](RELEASE_CANDIDATE_TEST_GUIDE.md)
defines the observed product journeys. Do not reduce either to this summary.

## Assessment

The project has substantial implemented capability and historical validation,
but is still in defect resolution and final qualification, not simply waiting
for publication. We cannot give a defensible completion percentage or promise
that the next human acceptance pass will be clean. Current source still has
unresolved camera/pacing gates, advisory disposition and unexecuted changes.
The 271-task old-source diagnostic suite has finished with 36 failed tasks in
626m50s (process exit 1). It is not a current candidate pass; triage and a full
corrected run are still required. Its immutable source and private evidence
remain preserved, not restarted or rewritten.
The [36-task failure ledger](open-items/diagnostic-suite-failures.md) separates
confirmed routing/dependency failures from measured gameplay failures and
unresolved startup/timeout causes; none is waived merely for being old-source.

## Entire remaining release scope

| Workstream | Remaining work and completion evidence |
|---|---|
| Execution and baseline | This checkpoint uses source/parser checks and bounded compilation under the latest conversation-supplied instructions; behavioral qualification remains deferred. Earlier runner-control refusals are historical evidence, not a current hardware diagnosis. Preserve the completed old diagnostic run and triage its 36 failed tasks without editing its source/artifacts. Integrate fixes into a clean, immutable candidate with exact version, commit and feature profile. |
| Trophy/progression defects | Qualify #63's five-world award/display correction, per-world best-medal Tracks merge, and bounded status counter. Run new units, old-code controls, all five championships, save/reload, no-downgrade, real cabinet/Tracks, Adventure Two and cross-revision/package checks. See [domain audit](open-items/trophy-domain-audit.md). |
| Camera | Resolve Ancient Lake's early correction re-engagement without waiving the hard gate. Reconcile proportional recovery behavior with the architecture's timing/cadence requirements; validate zero-time hold correction, shoulder classification, all motion routes and the manual comfort review. |
| Pacing | Diagnose and fix the measured companion re-anchor failure; use that arm's own evidence. Execute reporting/integrity regressions and controls. Pass the full visible realtime gate without a missing-baseline exemption; retain phase, audio and tearing requirements. |
| Renderer / public #61 | Requalify void/sky/pause fixes on current source and both supported revisions. Separately reproduce or resolve the Windows/NVIDIA purple symptom. Run GL/WebGPU visual, split-screen, UI-resolution, shadows, weather, materials and performance gates; one symptom/backend does not close the issue. |
| Dependency/security | Retain the verified BasisU alignment amendment and optional KTX2 feature contract. Complete current-pin advisory applicability/disposition, including the unresolved general-loader review. Execute later PNG admission regressions and controls, structural/package gates, and final-candidate ASan/UBSan decoder fuzzing. Do not infer universal decoder safety from one clean campaign. |
| Full automated validation | Fresh Debug, optimized and ASan/UBSan builds; complete unrestricted Release runner with shipping SDL and exact selected native/Release/ASan/wasm artifacts; Debug primary behavioral suite. Include runner/CI routing, Windows read-back, save, input, audio, campaign, rollback, character and browser gates. A subset or compile cannot replace this. |
| Independent gameplay evidence | Requalify the real-ROM Bluey 2 cadence oracle and standing boss gate. Preserve Ancient Lake's separate diagnostic classification. Record differential evidence when gameplay authority/math changes; do not turn a diagnostic into a blanket parity claim. |
| Character Workshop | Complete all exact-renderer, identity, intake, history/draft, portrait, lifecycle and evidence tasks. Observe first-use/edit/test/install/update/recovery/export/remove journeys, all vehicle/select contexts, 1P–4P, mixed-direction/fallback names, offline behavior, modalities and physical low/mid/high GPU profiles. Validate the completed receipt against the final four desktop artifacts and provenance bytes. |
| Other public issues / content scope | #62: physical-controller choice/persistence in both settings surfaces. #60: Windows packaged skip/escape/recovery paths. #58: distinguish qualified Taj portrait from unimplemented Dixie/Tiny/NDS content; obtain an explicit scope decision. Recheck bonus racers, ghost/record identity and persistent safe Magic Codes. Never close an issue on partial local evidence. |
| Multiplayer | Qualify the actual shipping native Online Room beta profile and local Adventure Party through the acceptance guide: race/rematch, controller custody, network failures, progression and teardown. Execute failed-view/leave/registry-cleanup regression coverage for the invalid-view/session-ownership candidate in the [native quality plan](multiplayer/NATIVE_MULTIPLAYER_QUALITY_PLAN.md); its compile-time policy checks are not a rendered recovery pass. The requested premium online program means native online multiplayer, not Phone Party; its journey acceptance targets and capability slices remain explicit there. Keep published web local-only. If cloud Phone Party is selected, separately satisfy its deployment/privacy/operations/admission gates; otherwise retain explicit partyless provenance and surface absence. LAN phone behavior is separate. |
| Web | Fresh wasm/JS/symbol-map provenance, size and no-ROM gates; real browser gameplay, save custody/import/export/offline recovery, no-WebGPU save access and published local-only surface. Debugging a native build does not qualify wasm layout or browser behavior. |
| Desktop packages | Rebuild all four exact artifacts: Windows ZIP, macOS unsigned/ad-hoc DMG, Linux AppImage and tarball. Verify manifests, pins/notices, importer/validator, hashes and provenance. macOS uses pinned standalone SDL, deployment target and mounted-bundle/DMG checks; Windows needs real WebGPU/input/audio/save/portable-Unicode/relaunch acceptance; Linux needs actual outer AppImage plus tarball runtime on physical Linux. Cross-compiles and extracted-payload parity are insufficient. |
| Human acceptance | Complete every section of the guide on required platforms, including controls/audio, 1P–4P, camera, all three #61 symptoms, #63, bonus characters, Workshop and multiplayer. Play a clean campaign through credits with silver coins, rematches, both Wizpig encounters and save boundaries. Record exact hashes, devices, first failure and unobserved cells. |
| Legal, documentation and final assembly | Rerun tree/history clean-room and actual-artifact no-ROM/privacy gates, review exceptional blobs, align README/changelog/versions/notices with measured shipping scope, and assemble immutable artifacts with matching sidecars. Resolve stale public-vs-candidate capability wording rather than advertising unfinished work. |
| Maintainer-controlled release | Obtain explicit approval before push/tag/draft release/upload/publication/deployment/issue mutation. Bind the version tag and every artifact to the qualified commit. Only then perform approved web publication and post-release download/boot/save/no-upload checks. Optional Developer ID signing is a separate choice, not a hidden prerequisite for the documented unsigned macOS artifact. |

The candidate's current intended profile is native online beta enabled, bare
development preview disabled, KTX2 enabled, cloud origin empty; the published
browser remains local-only. A change to this profile requires an explicit
decision and corresponding qualification, not silently skipping its tests.

The current RTC class sweep is tracked separately in the
[initialization/commit-boundary audit](open-items/native-rtc-initialization-audit.md)
and [work-admission audit](open-items/native-rtc-work-admission-audit.md).
The first-party signaling worker-start candidate now restores retryable Idle
state on launch refusal; its new actual-client regression includes same-instance
loopback recovery but remains unexecuted. Optional close-event reporting now also
cannot strand the joining worker. Bounded candidates implement accepted-callable
extraction without copying and queue accounting after successful insertion;
their helper/patch compile but assertions remain unexecuted. The newer additive
prepared-dispatch candidate makes accepted Processor continuation nonallocating,
with live-epoch ownership, timer ordering and accepted-lane join accounting.
Its helper and actual-vendor fixtures are registered but unexecuted; integrated
optimized, ASan/UBSan and Windows cross-builds compile/link the game plus both
prepared fixtures, existing work fixture and Workshop model after correcting
the new fixture's private include paths. Final source includes Workshop
reinspection and invalid-height guidance; no new assertions executed. The
Windows profile retains its historical cloud origin, not final partyless
artifact provenance. Generic allocating `noexcept` admission,
SCTP refusal ownership, complete off-thread teardown and receive/destructor admission
(RTC-ALLOC-01a and remaining -01c/d/e) still require coordinated implementation. Do not
treat the compiled initialization transaction as coverage of those later paths.
The next integrated candidate reserves per-transport serial stop/deletion and
binds lower receive/retirement authority to the exact upper owner. Independent
review corrected shared-lower loser cleanup, callback lock inversion and
retired-capture destruction under a pending lock. Optimized, ASan/UBSan and Windows
cross-builds compile/link the game, two new actual-vendor fixtures, prepared
dispatcher and Workshop model;
new assertions remain unexecuted. This advances -01d, not generic allocating
admission, stop/destructor body safety or complete RTC qualification. Workshop
raw field navigation also relinquishes queued focus on deletion/edit shortcuts;
its actual input/layout acceptance remains required.
These three-profile builds include the latest field-focus interruption fix.
The Windows profile retains its historical cloud origin, not final partyless
artifact provenance. Existing BasisU GCC warnings remain; no tests/apps executed.
The subsequent signal factory/outbound/reporting candidate adds atomic queue/
correlation admission, best-effort ordinary factory refusal, a nonallocating
terminal fallback, loss-preserving drain preparation and scoped local credential
erasure on unwind. Final integrated optimized, ASan/UBSan and Windows cross-builds
compile/link the game plus the five affected network/peer/queue/signal/LAN
fixtures, including the latest LAN follow-ups. New assertions remain unexecuted;
this is not runtime recovery or package proof. The Windows profile still has its
historical nonempty cloud origin, not final partyless artifact provenance.
The [Windows network-lifetime audit](open-items/windows-network-lifetime-audit.md)
records historical unchecked acquisition and cold LAN enumeration defects, now
addressed by checked shared-lease candidates. Socket/alias/resolver owners retain
their own prerequisite; result cleanup precedes lease/permit retirement, and
native cleanup failure contributes to the terminal no-relaunch verdict. Windows
OS-fault and packaged acceptance remain open. First-party peer-setup refusal now
has bounded autonomous retries with stale-lifecycle and unanswered-offer budget
checks; real fault/recovery journeys remain unexecuted. LAN post-accept/handler
admission now has a separate syntax-checked candidate for staged fd/registry/
thread ownership, sealed finalization, checked mandatory timeouts and callback
stop/capture-retirement ownership. Its six
actual-server regression groups remain unexecuted; exceptional retained join
ownership is a reported cleanup failure, not completed retirement.

The decoder allocator candidate rejects empty allocations without changing
positive-size budgets; it preserves ownership on refused resize. A later local
`stb_image.h` amendment releases consumed input on failed bit-depth conversion
and stops subsequent postprocessing. Original and amended hashes are separately
recorded in the notices; the writer is unchanged. The earlier allocator candidate
passed native optimized and ASan/UBSan compilation. New conversion ownership
assertions, old-code controls, final-artifact validation and the broader
general-loader advisory disposition remain open. See the
[complete PNG caller inventory](security/image-decoder-boundaries.md).
The conversion-amended game and affected PNG/store/portrait/ownership targets
now compile in native optimized, ASan/UBSan and Windows cross-build profiles;
no new behavioral or sanitizer-runtime pass is claimed.
The web engine now compiles the same local decoder amendment; generated JS and
wasm pass non-executing syntax/structural validation. This does not qualify the
published shell, browser runtime, saves or final clean-candidate artifacts.

## Parallel Character Workshop agent: first handoff complete

It is appropriate to parallelize; it is not appropriate to declare the entire
pipeline security-cleared. The original KTX2 alignment defect has a candidate
fix and sanitizer/fuzz evidence. Later PNG boundary changes and exact-artifact
advisory/receipt qualification remain open. Do not weaken these to get a green
Workshop result.

For independent checkouts, use a separate worktree and build/save/evidence directories from
an agreed committed checkpoint. The main worktree currently has uncommitted
trophy/pacing work; do not copy or reset that delta. The completed old
diagnostic checkout and artifacts must remain untouched. Separate worktrees
prevent source collisions but do not remove contention in GPU/performance
measurements: coordinate those jobs rather than overlapping their measurement
windows. Follow the latest applicable validation instructions; an independent
checkout does not change them.

Suggested assignment:

> Improve the existing Custom Character Workshop through observed, reversible
> workflows: first-use guidance, navigation, actionable errors, rig/pose/contact
> editing, portrait workflow, undo/redo and draft recovery, install/update/remove,
> and keyboard/controller/accessibility alternatives. Follow the existing
> Workshop specification and character spike-tail acceptance plan; retain the
> complete intended pipeline rather than replacing it with a smaller importer.
> Add regressions for each change and retain exact-renderer evidence provenance.
> Do not change donor physics, package trust or safety verdicts. Report any
> needed boundary/schema change for coordinated ownership first. No publication,
> deployment, real-user data mutation or private-artifact commits.

The maintainer subsequently authorized a managed second agent. The
`workshop_quality` agent completed its first tranche in the shared release
worktree under named file/function ownership, not an independent checkout.
It owns the new
[14-finding Workshop audit](open-items/character-workshop-quality-audit.md), Workshop model helpers/tests,
and the readiness table/cards plus inspector/library readiness summaries in
`ui_settings.cpp`. It must coordinate shared builds, stage/commit nothing,
and leave the release integrator's delta untouched. Its first tranche improves
responsive readiness presentation and makes accepted performance exceptions
explicit without altering approval or activation semantics. Model assertions,
strict C++17 syntax and native/model-test compilation are complete; execution,
rendered layouts and controller journeys remain unqualified. The audit covers
13 story rows. Its second tranche implements the Identity-to-named-draft
handoff, explicit cancellation and package/source-bound return after successful
save/resume. Model/source regressions and native compilation are complete;
actual focus/layout/navigation validation is pending. Intake guidance, keyboard
dependency, portrait hierarchy and cross-device evidence remain gaps. Shared
build ownership has returned to the release integrator.

A subsequent independent integration review caught and corrected a real
deferred-tab race in that handoff: the old visible ImGui tab was cancelling a
newly queued navigation request before its target appeared. The model now
distinguishes requested and observed tabs, retaining the request through stale
or absent observations and acknowledging the actual target before returning to
normal manual selection. Forward/reverse and manual-navigation regressions
were added. Strict syntax and optimized plus ASan/UBSan native/model compilation pass; actual
input/layout regression execution remains required. The review found no
additional issue in the inspected failed-save return gates, package/source
matching or evaluated exception counts; that is a scoped source assessment,
not proof of every Workshop story or a decoder security clearance.

The third Workshop tranche adds a guided raw-intake checklist: six navigable
groups describe all 13 existing Build prerequisites, and post-edit missing
inputs appear beside Build. Completed sections remain available to revisit;
source/draft-bound jumps do not inspect, accept, build or install anything.
The original Build expression, autosave and importer/decoder authority remain
unchanged. An exhaustive 8,192-combination model regression and production-source
parity assertions were added, not executed. Syntax/parser checks and the
integrated optimized and ASan/UBSan native/model builds pass. Narrow/200%, deferred scrolling,
focus, interrupted/resumed intake and exact-file failure journeys still need
behavioral qualification. WQ-06 is implemented, not acceptance-complete; the
keyboard-entry and portrait-publication hierarchy work remains open.

The fourth tranche implements WQ-08's portrait publication hierarchy. Framing,
style preview, canvas and named-draft destinations are described separately;
the compatibility PNG route remains available behind an accessible disclosure.
Both direct install actions name their installed-revision effect and warn about
editor reload and preserving other unpublished work before proceeding. Shared
minimap colour stays outside that collapsed section, and Package navigation
does not publish or restore. Existing revision workers, arguments and admission
are unchanged. Source contracts and the acceptance guide cover the distinction;
rendered clarity, installation/failure/recovery and input/device acceptance
remain unqualified. Integrated optimized and ASan/UBSan native game and portrait
target builds pass. WQ-08's presentation is implemented, not behaviorally cleared.

File ownership:

| Agent | Owns | Coordination rule |
|---|---|---|
| Workshop agent | `platform/app/character_workshop_model.*`, Workshop-specific sections of `platform/app/ui_settings.cpp`, their focused tests, Workshop UX documentation | `ui_settings.cpp` is shared and large: claim named functions/sections, not the whole settings subsystem. No concurrent edits to those sections. |
| Release/security integrator | `platform/modern_character_ktx2.*`, `platform/modern_character_asset.*`, texture/PNG decode and writer boundaries, `cmake/character_basisu.cmake`, decoder amendments/pins, package manifests and sanitizer/fuzz gates | No pin update, feature disable/enable, admission relaxation or payload-schema change by the Workshop agent without handoff. |
| Joint review before edits | `tools/character_source_adapter.py` and importer/validator tooling, package/draft schemas, `character_test_evidence_store.*`, release-receipt schema/verifier, donor/runtime registry/renderer interfaces, `CMakeLists.txt`, shared CI/runner files | These cross untrusted-input, persistence, evidence or integration boundaries; file ownership must be transferred explicitly for overlapping changes. Never relabel unobserved/failed evidence as approved. |

Done means tested journeys on the integrated candidate, source/license and
last-known-good preservation, no hidden synchronous work or mouse-only steps,
and the actual receipt/physical-device acceptance—not just more controls or
a locally green isolated test. W5 custom-physics profiles remain explicitly
future scope in the current specification; do not enable them incidentally.
The release integrator separately owns the native multiplayer audit and
`ui_online_room.cpp`/`online_live_wiring.cpp`; the Workshop agent does not edit
those files. The new multiplayer product target is tracked in
[the native multiplayer quality plan](multiplayer/NATIVE_MULTIPLAYER_QUALITY_PLAN.md).

The native teardown trace additionally found an unsafe timeout-detach fallback:
retiring workers could outlive static tracker/network state. The candidate now
always joins through shared drain logic, reporting a slow close after ten
seconds. Optimized and ASan/UBSan game/fixture compilation pass; assertions and
real shutdown behavior remain unexecuted. This is not a bounded-responsive-exit
claim. Follow-up source changes move retirement record allocation before launch,
retain ownership through thread construction, roll back to inline cleanup on
scheduling failure, and reap completed handles independently of stalled workers.
Launch-refusal/recovery and repeated-session assertions are added but unexecuted;
storage-allocation fault injection remains pending. Actual transport
cancellation/progress and the separate failed-view/registry recovery fixture
remain explicit work in that plan. Normal Quit now has a closing-only native
surface and polls background retirement after the cancellable Workshop phase
completes, suppressing new room and engine actions. Delayed status has no invented
deadline. Resource-exhaustion fallback and renderer-failure exits still join
synchronously for lifetime safety; real transport and rendered acceptance remain
pending.
Windows game/fixture cross-compilation also passes; it is not a runtime verdict.

The original immutable diagnostic run subsequently failed `widescreen_shadow_asan`:
all six arms timed out (124), without final shadow reports; partial trace lengths
also differed. This is incomplete qualification, not proof of a shadow memory
fault or a deterministic simulation mismatch. Preserve the failure, investigate
completion/timeout handling, and obtain complete current-candidate evidence.
No timeout or behavioral gate has been relaxed. That old-source run separately
passed campaign progression, ROM revision and array-bounds checks; none qualifies
the current unstaged integration.

The reporting follow-up now distinguishes timed-out/incomplete arms from actual
simulation differences, requires exact completion and ordered traces, preserves
optional private per-arm logs/metadata, and fixes missing-summary/context failure
reporting. Thirteen mock regressions are added, not executed. The original
timeout's cause and a complete successful run remain open.

The zero-cost issue sweep additionally corrects #60's cross-controller shoulder
aggregation and stale initial-device snapshot; its actual sampler has a new
deterministic SDL-boundary fixture. It also corrects #62's effective-value
display mismatch for mixed-case/unknown legacy configuration through a shared
non-latching interpretation used by the existing gameplay latch and each UI
snapshot. Settings bytes, restart staging and source locks are retained. See
[the follow-up report](open-items/release-issue-followup.md) for source findings
and remaining acceptance, separate from historical widget/launcher passes.

## Current zero-cost integration checkpoint

Current zero-cost integration checkpoint: Workshop library/assignment controls
use stable package identities and wrapped labels; secondary-chain selections
and editable tree titles no longer derive identity from authored display text.
Raw mapping choices retain fixed-height clipping and expose literal names and
full focused speech. Additional selector containment and real novice/input
acceptance remain open. No package admission or donor authority was relaxed.

Normal Quit now services returned preview publication even while minimized,
settles existing Workshop work on exceptional exits, and polls a progress-only
online close. The HTTP socket path now checks cancellation/deadlines in each
nonblocking TLS BIO call, avoids retrying fatal TLS timeouts, and suppresses
Linux SIGPIPE for both plaintext and TLS sends. New ordinary-loopback and
closed-peer assertions remain unexecuted. All-owner RTC cleanup integration now
retracts phone aliases, retires room and phone ownership, and gates a once-only
cleanup future on both groups finishing. Exceptional completion is a failure,
not success. Terminal-path and real-transport qualification, plus the separate
OS-cancellable DNS lifecycle, remain open; see the native quality plan. First-party
room/signaling lookups now share an eight-job admission budget retained through
actual result cleanup, including abandoned callers. Global RTC cleanup waits for
these jobs too. Result RAII, startup-failure handling and new capacity/recovery
fixtures are implemented, not behaviorally qualified; library-internal DNS and a
hard exit-time guarantee remain separate unresolved work.
The shared terminal helper now covers all 59 post-launcher host-shutdown sites,
with failure propagation before relaunch and an explicit fix for a diagnostic
cloud-session error-path leak. The integrated optimized and ASan/UBSan native
game and affected fixtures compile; the new assertions and actual terminal
journeys have not executed. This is not final artifact qualification.

Runner triage also corrects three native-browser end-to-end gates that ignored
the selected native build: explicit build/shell routing and driver preflight
now replace fallback to a different `build-rel`. Windows driver suffix handling
is aligned with native artifacts. Mock regressions await execution; the current
command control rejected even a non-executing `--list` inventory attempt, so
parser checks remain the available verification here.

The next runner tranche admits the expected lockfile-installed Wrangler
entrypoint for exactly seven local Worker consumers before later preflight I/O;
embedded-LAN remains independent. Shared startup now retains the untransferred
process through readiness and attempts cleanup on expected failure, unexpected
exception and interruption. Optional diagnostics cannot mask the primary failure.
Two-person and chaos aggregate failures retain fixed categories/source locations
and private bounded metadata without raw service/browser values. Chaos still
scans its whole log for errors using bounded memory. New mocked fixtures are
registered, not executed; the historical canary/chaos startup causes remain unknown.

The resolver-integrated native game, transport fixture and two-translation-unit
budget fixture compile in optimized and ASan/UBSan profiles. New tests remain
unexecuted. Static browser triage rules out the obvious served/build JS/wasm
copy mismatch in the old diagnostic snapshot: selected failing trace tails stop
between second graphics-task dispatch and the subsequent display-list entry,
bracketing backend frame-start/setup rather than proving a later pacing failure.
Engine-wait RAF counters are not independent compositor evidence. No scheduler
rewrite or KTX2 profile change follows from this observation; bounded phase
diagnostics and current-artifact reproduction remain the next browser work.
Those bounded diagnostics are now implemented behind explicit test opt-in:
renderer/engine phase history, an independent RAF heartbeat, and additional
timeout-only diagnostic snapshots. Normal timing qualification defaults off,
native phase markers compile away, and no renderer or pacing decision is changed.
The wasm engine and generated/source JS syntax checks pass; new Node/Python
fixtures remain unexecuted. Actual startup reproduction and root-cause correction
remain outstanding. A related first-Quit correction also makes the resolver-work
observer nonallocating, so an unused or failed-to-initialize pool cannot throw
merely while testing the global-cleanup barrier.

The dependency-level shutdown sweep then found a thread-launch exception in
libdatachannel's noexcept final-token destructor. A tracked amendment reserves
cleanup and joining-publisher workers before initialization; final destruction
only signals work. Strict syntax and optimized/ASan+UBSan game/fixture compilation pass;
assertions remain unexecuted. The separate upstream partial-initialization
rollback defect now has a transaction candidate: a failed subsystem start
publishes pending selective retirement rather than the previous ready future.
The [native lifecycle follow-up](
multiplayer/NATIVE_MULTIPLAYER_QUALITY_PLAN.md#dependency-cleanup-reservation-and-partial-initialization-follow-up)
tracks both the bounded correction and the remaining failure-ownership work.
The [RTC initialization audit](open-items/native-rtc-initialization-audit.md)
enumerates the four confirmed defects and their implemented candidates, stage-specific
rollback contracts and required execution. Optimized/ASan+UBSan game/stage/transaction
compilation passes, not behavioral qualification. Queued-work noexcept allocation
failure and C-internal startup exits remain separate open hardening work.
The connection review additionally found in-flight callback commits that need
generation/shutdown checks at publication, not only callback entry. That source
candidate is implemented, including delayed retry/ping progress rechecks and
bounded early setup recovery; real interleaving validation remains open.
The ASan tree now consumes this worktree's amended dependency source, not the
other worktree's unamended override; that other source tree was not modified.
The source-form manifest and all desktop notice-hash validators also now match
the actual dependency graph and local amendments. Full license text is preserved.
New recipe/notice consistency regressions are registered but remain unexecuted;
source hashes and compilation do not qualify distributed packages.

The latest Workshop selector tranche also separates literal authored names from
native widget identity and paints full literal closed-combo previews. It
preserves existing IDs, selection rules and fixed-height clipping. Source/parser
checks do not establish popup layering, focused/disabled appearance, Unicode
containment or complete keyboard/controller journeys; those remain acceptance.

Integrated optimized and ASan/UBSan native game plus affected fixtures compile;
Windows game/fixture cross-compilation also passes. Python source parses and
whitespace checks pass, and new CTest registrations were inspected without
execution. New assertions, actual UI/network behavior, final immutable builds
and hardware/package acceptance remain unqualified. No new expense, deployment,
publication or GitHub issue mutation was introduced.

## DKR-R comparison: capability is not qualification

Read-only upstream snapshot: `ThatGuyMcd/DKR-R` main
`b0156864562100ce3453ce4b72ba15c8e3125376`, latest listed release
[Version1.0.4, 2026-09-03](https://github.com/ThatGuyMcd/DKR-R/releases/tag/Version1.0.4).
This compares documentation and our inspected source/evidence, not a
side-by-side benchmark or independent certification of their claims. No
competitor implementation was copied.

| Capability | DKR-R documented offering | Golden Balloon assessment / remaining comparison |
|---|---|---|
| Presentation | Accurate/Modern, widescreen, FOV, unlocked interpolation | Implemented counterpart; camera/pacing/current-package quality still open |
| Scene quality | Distance, LOD, anisotropy | Implemented settings; direct quality/performance comparison unmeasured |
| Settings shell | Controller launcher and live overlay | Implemented; final controller acceptance remains |
| Local input | Four assignments, profiles, keyboard ownership, mapping wizard | Four-player input/remapping exists; no proven wizard/assignment feature-for-feature parity |
| Motion control | Two-axis gyro and Steam Deck SDL3 sensor path | No corresponding native gyro implementation identified in inspected first-party input/config source; a real gap |
| Vehicle controls | Per-vehicle inversion | Not identified in inspected settings; parity unestablished |
| Rumble/Paks | Concurrent rumble and four virtual Paks | Implemented virtual Paks and rumble profiles; not a unique advantage, final device checks remain |
| Audio controls | Five mix categories and EQ | Master/music/effects are exposed; separate vehicle/ambience/EQ counterparts not identified |
| HUD and CRT | Placement/size options, CRT masks | Native-resolution UI/scaling exists; equivalent user HUD placement/size and CRT controls not established |
| Saves/codes | Import/export/backups, builder, launch codes | Broad counterpart exists; new trophy fixes require regression qualification |
| Texture packs | RT64/Rice hot switching | Our local texture/music packs differ; do not claim format/ecosystem interchangeability |
| Diagnostics | Overlay, private reports/logs/dumps | We have extensive diagnostics; equivalent end-user support-export UX not established |
| Online | Public two-player offering; detailed docs also describe 2–4 peers | Our two-player native beta is not an automatic lead; private four-process rollback evidence is not shipped four-player availability |
| Adventure | Advertised 2P Adventure, 1.0.4 boss fixes | Local Adventure Party supports broader candidate work; final campaign/party acceptance needed |
| ROMs | US 1.0 and US 1.1 | US 1.1 and EU 1.1: PAL is an advantage, US 1.0 a gap; neither set contains the other |
| Platforms | Windows/Linux releases, separately linked macOS fork | First-party Apple-silicon and browser work are meaningful differentiators; current final-platform qualification remains |
| Characters | No equivalent complete builder identified in reviewed product docs | Bonus racers and executable Workshop are potential differentiators; decoder, UX/device receipt and distribution evidence must close |

Sources: [upstream README](https://github.com/ThatGuyMcd/DKR-R/blob/b0156864562100ce3453ce4b72ba15c8e3125376/README.md),
[controller documentation](https://github.com/ThatGuyMcd/DKR-R/blob/b0156864562100ce3453ce4b72ba15c8e3125376/docs/CONTROLLERS.md),
[online documentation](https://github.com/ThatGuyMcd/DKR-R/blob/b0156864562100ce3453ce4b72ba15c8e3125376/docs/ONLINE_MULTIPLAYER.md).
Their online README/release wording and detailed 2–4-player document differ;
do not choose the weaker claim simply to declare superiority.
Our comparison uses `README.md`, `platform/video_config.c`, input/controller
and virtual-Pak code/docs, Workshop implementation/specification, and the
release checklist. Missing keyword matches alone do not prove absence;
unestablished rows require an explicit follow-up feature audit.

**Conclusion:** we have meaningful differentiation, not evidence that we
exceed every DKR-R capability. Closing release defects is distinct from adding
gyro, richer audio controls, HUD/CRT options, controller wizard parity, US 1.0
support, or texture-ecosystem compatibility. These are explicit competitive
backlog candidates requiring scope and acceptance decisions, not reasons to
quietly expand this release or promise superiority today.
