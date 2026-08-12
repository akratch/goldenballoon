# Rollback authority inventory v1

Status: **foundation implemented; engine registration and restore breadth are
not yet a release `GO`.** This document is the review boundary for S12 RB-00.
The executable contracts live in `platform/rollback`, `platform/net`, their
ROM-free tests, and `tools/check_rollback_authority.py`.

## The rule

Only deterministic state that can affect a later race tick belongs in a local
rollback snapshot. A snapshot is in-process recovery data: it is never sent to
a peer, written to disk, or restored in another process. Registration freezes
before tick zero and restoration is legal only at the fixed-tick boundary while
presentation work is idle.

Every candidate is assigned exactly one class:

| Class | Treatment |
|---|---|
| Snapshot | Copy its stable, writable range and restore it byte-for-byte. |
| Derive | Rebuild after restore with a registered deterministic hook. |
| Presentation-local | Exclude; no authoritative reader may depend on it. |
| Host handle | Exclude and reject if a range containing it is registered. |
| Forbidden side effect | Journal it; execute only when confirmed, or prohibit it for online progression. |

## Frozen match inputs

`MdkrMatchManifestV1` binds protocol and match epoch, a 16-byte build identity,
ROM revision, cadence, 32-byte gameplay/content digest, four stable slot-owner
ids, track, rule and vehicle settings, input delay, and the RNG seed. Its
112-byte network-order representation has strict validation and an end checksum.
The manifest digest is embedded in every snapshot; a mismatch fails atomically.

## v3 authority families

The existing `MDKR_STATE_HASH=3` stream is the detector and field census, not a
savestate. Its source-level inventory in `platform/sim_hash.c` covers:

| Family | Rollback treatment |
|---|---|
| Gameplay RNG; mode, race/load/countdown/pause and level timers | Snapshot scalar block. |
| Settings progression, records, flags and balloon counts | Snapshot for deterministic reads; online progression writes remain forbidden. |
| Object pool/list membership and allocation metadata | Snapshot arenas plus allocator/list metadata; preserve addresses. |
| Object/particle transforms, velocities, animation and interaction state | Snapshot pool ranges. |
| Behavior-selected property arms and racer physics/route/lap/item/AI state | Snapshot pool ranges. |
| Model-instance authoritative animation scalars | Snapshot stable model-instance storage. |
| Controller edge state and canonical four-slot samples | Snapshot edge calculator; input history itself remains outside and drives resimulation. |
| Race phase, finish order and deterministic results | Snapshot until confirmed; publish results once. |

Before the engine integration may claim `GO`, concrete address/size rows for
each arena and scalar block must be registered in one engine-owned function and
the restore matrix must prove all families. Registering one monolithic process
range or relying on the v3 hash alone is not acceptable.

### Exact range registration now implemented

`mdkr_rollback_game_authority_register()` now builds one atomic registry after
a gameplay level is fully loaded: 129 ranges on the two current non-wave
evidence tracks and 141 on Whale Bay, where optional wave and object-behaviour
allocations are live. It includes the `POOL_OBJECT` descriptor and exact backing
allocation; stable object, particle, collision, checkpoint, camera, racer, AI,
object-header and map allocations; the complete Settings allocation; the
explicit RNG, object-list, race, checkpoint, map and mode scalars enumerated by
the current v3 audit; all eight camera records; five concrete particle buffers
and their count/full flags; wave phase/model allocations and wave-control
scalars when present; unique POOL_MAIN behaviour allocations discovered in
stable object order; transition timers, phase, geometry pointers and a pinned
transition workspace; and canonical four-port controller samples plus
edge-calculator state needed to replay a completed authored tick.
Standard-item headers and immutable visual assets are leased before registry
freeze. Their dynamic model instances and attachment objects allocate from the
snapshotted object sub-pool; each unique shared model's small mutable reference
counter is registered separately. Discarding a projectile replay therefore
restores both pointer identity and lifetime accounting without treating shared
geometry, textures or the whole main arena as authority. The lease count is
launcher/runtime bookkeeping: it is initialized before tick zero, unchanged
during a match and cleared only after the rollback ring is retired.
It also registers presentation-snapshot and camera-obstruction invalidation as
post-restore rebuild hooks.

The object sub-pool range contains its slot topology and object/particle bytes,
so restoring it also restores deterministic allocation reuse without copying
the main arena. The ROM-free identity control changes bytes and allocation
topology, restores, and requires the next allocation to reuse the same address.
A second direct registry test captures the complete synthetic registry, mutates
heap, auxiliary behaviour, camera, particle, wave, transition and global
authority, restores them exactly, executes both rebuild hooks, and proves a
missing allocation leaves the registry empty.

Generic live allocations are registered only through allocator resolution;
callers cannot supply a guessed extent. Batch registration validates every
range, tag, overflow and overlap before mutating the registry.
`MDKR_ROLLBACK_LAB=1` wires that registry to the real gameplay level-ready
boundary, freezes it before tick zero, preallocates 32 ring slots and captures
tick zero. A requested lab aborts level startup if any required allocation,
freeze, ring allocation or capture fails; level teardown destroys the ring
before engine memory is released. Every allocator-owned range is re-resolved in
the main pool at each tick boundary. This catches a freed, resized or replaced
authority block without falsely rejecting unrelated HUD/texture cache churn;
this allocator never moves a live block. Each boundary also scans every live
object's POOL_MAIN behaviour pointer and rejects any newly reachable allocation
that is absent from the frozen registry. Dynamic authority growth therefore
fails at its first boundary with object/behaviour evidence instead of waiting
for a later desync.

The first ROM-backed negative run correctly stopped on 22 post-freeze main-pool
assignments. Allocation-origin and stack evidence classified one as a barn-door
transition started by the anim-camera simulation path and 21 as first-use HUD
assets for lap count, bananas, timer and race position. Transition state was
therefore added to authority and its maximum-sized workspace is pinned before
tick zero; the HUD assets remain presentation-local. The corrected US 1.1
breadth matrix reaches 2,800 process ticks cleanly on all 20 standard tracks
using an observed human-driven car. A separate ROM-mask-derived matrix exercises
all 47 legal standard-track/player-vehicle pairings and verifies the dispatched
car, hovercraft or plane. Every row corrects ticks 117–120 and
repeats the corrected snapshot byte-for-byte. Native snapshots now span
500,225–508,513 bytes across 143–155 ranges, for a maximum 16,272,416-byte
ring. The increase is intentional: every mutable `ModelInstance` now lives in
a dedicated snapshotted subpool, so legal free/reuse cycles restore atomically
instead of becoming per-allocation lifetime promises.
The shared production limit is 32 snapshots: at most 31 ticks may be replayed,
and an input may be
at most 30 authored ticks old because correction also needs the boundary before
the first dirty tick. The lab now captures
every real tick boundary, and `MDKR_ROLLBACK_LAB_ROUNDTRIP=1` restored the first
real boundary, ran both rebuild hooks and continued to the same clean 2,800-tick
exit. `MDKR_ROLLBACK_LAB_RESIM=1` additionally retained the exact tick-120
snapshot, restored tick 116, replayed ticks 117–120 through the real game loop
with their captured canonical inputs and no display-list authoring, then proved
the complete reconstructed snapshot identical. With autopilot disabled,
`MDKR_ROLLBACK_LAB_MUTATION_CONTROL=1` first replayed those ticks with changed
P1 throttle/steering and required the authority snapshot to diverge, then
restored tick 116 again and passed the original-input identity proof. Its first
negative run found a real
render dependency: the racer's animated water texture clock advanced inside
shadow generation. That clock now advances in the once-per-tick authoritative
render bridge, while menu scenes preserve their authored render cadence.

The first Jungle Falls breadth run exposed an omitted sprite-particle buffer;
all five particle buffer families and their count/full scalars are now explicit
authority. The first Whale Bay run then exposed viewport-derived object sorting
through an incomplete camera capture, followed by buoy height driven by
POOL_MAIN wave/log state. Capturing all eight cameras, the wave subsystem and
unique behaviour allocations removed both failures. These were fail-closed
negative results and subsystem-level fixes, not track-id exceptions.

The delayed-input control now uses the production `MdkrNetInputHistory`: it
withholds P1 packets for ticks 117–120, authors repeat-last predictions,
submits four late validated packets, requires dirty tick 117, restores tick 116
and replays the corrected canonical samples. Corrected tick 120 must change
non-input authority relative to prediction; a second corrected replay must
then reproduce the complete corrected snapshot exactly on all three tracks.

Launcher ingress uses the same bound. `MdkrMatchTransport` tracks each remote
slot's contiguous confirmed-through frontier; an aged gap or older late input
latches one typed recovery record instead of asking the engine to restore an
evicted boundary. The launcher then cooperatively unwinds the engine and enters
the connection-lost recovery scene. A fixed 24-byte single-input codec remains
the minimal format. Named impairment profiles use a fixed 64-byte bundle with
one to three consecutive four-slot frames and CRC-16/CCITT, sent oldest-first
after decode so ordinary loss can be repaired without repeatedly invalidating
the predicted suffix. Codec payloads do not grant ownership: an authenticated
peer mask is out-of-band and the decoded slot mask may only narrow it.

The live sound/rumble/save emission sites now feed the bounded rollback journal
through an independent gameplay-event observer, so accessibility keeps its own
observer. Confirmed rows are retired to keep long matches bounded. The actual
EEPROM/IDBFS boundary rejects progression writes throughout an online rollback
timeline, and all host I/O is denied during resimulation; focused tests exercise
both policies. Controller Pak allocation, deletion, data writes and reformatting
now converge on the same policy before path creation, encoding, temporary-file
creation, atomic replace or browser sync; candidate Pak state is committed only
after that boundary succeeds. Reversible audio/rumble rows are identified and
reconciled in the journal. Production SFX starts bind the journal id to a
host-only request and a monotonic voice generation: duplicate replay starts
are suppressed, corrected-only starts preview after reconciliation, and a
vanished prediction cannot stop a newer voice that recycled the same
fixed-pool address. Final volume, pan, pitch, priority and stop commands
coalesce during resimulation and flush only after the corrected event set is
known. Deterministic spatial-source pool/topology state is registered; device
queues and voice allocation remain host-only. Rumble now has a production cancellation adapter:
when correction removes a previewed event, its bounded controller index stops
the host motor directly without editing restored `RumbleData`; corrected rumble
continues through the ordinary post-resimulation host service pass. The adapter
is source-guarded against an accidental motor start. Physical audio/rumble
device qualification remains open. Native 2P and 4P race-start routes now pass the same late-input correction
and exact second replay. The first 4P route exposed a mixed-list safety defect
in the extracted authoritative water-texture clock: particle storage was being
read as a full `Object`. The loop now follows `shadow_update` ordering by
rejecting particles, checking the header water group, and only then reading the
optional `WaterEffect`; a source-order contract, a complete 9,600-frame 4P
race/results oracle, and a 2,700-frame ASan route protect the repair. Other race
Other race modes remain outside online v1; device and cross-platform matrices
remain open and keep A3 gated.
Viewport/HUD/listener invariance and persistent native rematch lifecycle now
have separate real-process gates.

The remaining standard-item breadth is now closed by a 15-row matrix: every
balloon type and level enters its real inventory-use branch inside ticks
147–150 of a delayed correction. The ROM maps those configurations to 14
concrete weapon IDs because missile L1 and L3 intentionally share the standard
rocket. Boost, shield, magnet feedback, trap spawn and projectile spawn all
pass exact second replay. A suppressed-release mutation fails observation, and
invalid-index plus wrong-mode controls fail closed. The first projectile rows
exposed unsnapshotted model instances, shared-cache first use, main-pool glow
attachments and reference-count debt; the ownership and residency rules above
are the subsystem fixes and the matrix permanently guards them.

The laboratory replay target is now a strict cached unsigned setting rather
than a compile-time-only test location. `MDKR_ROLLBACK_LAB_TARGET_TICK` defaults
to 120, rejects values smaller than the four-tick window or outside `uint32_t`,
and is printed at admission. This placed an exact replay at tick 4,800 of the
autopiloted native 4P Ancient Lake route. The race laboratory survived 5,491
authored ticks through four results and teardown into the results scene, with 2
item spawns, 224 checkpoints, 1,083 retired side-effect rows and zero duplicate,
overflow or forbidden-I/O rows. This closes one standard-race finish/item
lifecycle arm; it does not substitute for other modes, rematches or platforms.

On the Apple M3 Max WebGPU headless evidence host, each track made 189 captures
and three restores. Capture mean/max was 727,104/908,541 ns on Ancient Lake,
729,251/937,459 ns on Jungle Falls and 739,218/915,000 ns on Whale Bay; restore
mean/max was 770,541/849,625 ns, 747,597/846,917 ns and 784,583/865,584 ns
respectively. Each run journaled real game sound/rumble/save emissions without
overflow and exited cleanly. This is
encouraging but is not the required p99
matrix: release evidence still needs long native/wasm, NTSC/PAL and 1P/2P/4P
samples on representative low-end hardware.

The runtime now records capture, restore, each resimulated game tick and each
authored gameplay frame in separate bounded, allocation-free 10 µs histograms.
Runtime summaries publish p50/p95/p99/max, terminal-bin overflow and counts
beyond 8,333,333 ns and 16,666,667 ns. The ring's 2,048 bins cover 20.48 ms:
p99 must remain at or below 8.33 ms, histogram overflow is forbidden and no
sample may cross the hard 16.67 ms authored-tick deadline. A small statistical
tail between p99 and the deadline is therefore represented honestly rather than
mislabelled as overflow. The clock and histograms remain host diagnostics
outside authority, and a unit injects a pathological 20.48 ms+ sample to prove
all negative counters. Current native breadth peaks at 1.30/0.97/0.27/0.82 ms
p99 for capture/restore/resimulation/authored frame. The real Chromium/Wasm
gate now passes a tick-300 delayed correction and exact replay with a 391,277-
byte snapshot and 1.01 ms resimulation p99 while sustaining the contained
authored 30 Hz loop. PAL, additional desktop OSes, low-end devices and
renderer/GPU presentation-time qualification remain open.

The current Whale Bay/hovercraft configuration completed a 10,180-authored-tick
soak with the 32-slot ring. Its 10,189 captures and three delayed-control
restores report 0.19/0.18/0.05/0.16 ms capture/restore/resimulation/authored-
frame p99, a 506,545-byte snapshot and a
16,209,440-byte ring. Gameplay effects are non-vacuous with zero timing,
journal or forbidden-I/O overflow. This closes the current native worst-track
duration row; wasm/platform p99 remains.

## Explicit exclusions

| State | Class | Reason and owner |
|---|---|---|
| GPU resources, display lists, renderer caches, swapchain and window objects | Host handle / presentation-local | Backend owns lifetime; restore must invalidate/rebuild presentation snapshots. |
| SDL objects, sockets, WebRTC channels, threads, mutexes and file handles | Host handle | Platform/launcher owns them; range registration rejects host-handle flags. |
| Wall clock, physical audio queue/voice allocation and presentation interpolation history | Presentation-local | Never an input to the authored tick; reseed or reconcile after correction. Spatial-source scheduling/topology is separately registered because gameplay creates and removes it. |
| Particle colour/brightness, shading and racer light flags | Presentation-local | Existing v3 audit identifies their presentation-only readers and measured exclusions. |
| Raw ROM backing bytes | Immutable/excluded | Manifest binds revision; bytes are never copied into each snapshot. |
| Save, EEPROM/IDBFS and Controller Pak sync | Forbidden side effect | Real save emissions are confirmed-only journal rows. The lowest EEPROM and shared Pak-store boundaries reject online rollback progression before filesystem/IDBFS work; Pak callers publish candidate memory only after durable success. Achievements and durable diagnostics still need an equivalent product boundary if/when present. |
| Audio/rumble/particles caused by predicted gameplay events | Reversible event | Sound and rumble emissions have stable journal ids. Audio uses versioned handle custody, deferred corrected previews and a coalesced post-replay command buffer; vanished rumble reaches a production host-only motor stop and corrected state returns through normal service. Physical-device acceptance remains open. |
| Playable-Taj sidecars currently excluded by v3 | Open audit item | Existing v3 rationale proves downstream detection, but restore identity must independently classify/register or deterministically rebuild them before `GO`. |

## Source-change gate

The checker fingerprints mutable, zero-indented declarations in `game/src`, the
repository's file-scope declaration convention, without line numbers. Its
lexical pass removes comments and string/character literals before matching, so
function-local declarations no longer pollute the authority baseline. Any
addition, removal, type/extent change, or initializer change fails CI and
requires this inventory and the reviewed fingerprint to move together.
`--self-test` injects a synthetic omitted authoritative scalar, verifies that an
indented function-local decoy is excluded, and proves the gate fails closed.

This census cannot decide semantic authority; reviewers still trace every new
reader and writer. It prevents a declaration from silently bypassing that
decision. Local automatic variables and heap fields remain covered by the v3
family audit and the engine range registry.

The reviewed baseline is now 1,618 declarations. Its four-declaration delta is
classified: `sMdkrMutationObserver` is a diagnostic-only allocator callback
that simulation never reads, and `sTransitionWorkspacePinned` is a stable
allocation-policy latch whose pointed-to workspace and gameplay-readable state
are registered. `sRollbackResimulating` is an execution-mode latch used only to
reject recursion and suppress presentation authoring while the same simulation
path replays; it is not read to choose gameplay results.
`sRollbackSoundGeneration` is a host-audio ABA token incremented only when a
physical voice-state slot is allocated; simulation never reads it, and the
physical voice pool remains outside authority. The observer context/origin and
workspace-capacity declarations are likewise
non-authoritative host diagnostics but do not match this checker's declaration
grammar.

## Release gates still owed

- Qualify audio and rumble on real devices; extend the durable
  firewall to achievements and diagnostic export if/when those sinks exist.
- Extend the proven real-engine registry and rebuild hooks to every remaining
  authority family found by finish, item, multiplayer and lifecycle matrices.
- Preserve the passing three-track/particle and historical 10,180-tick proof;
  requalify the soak at 32 slots, add remaining standard tracks/items and
  require an omitted-byte control to diverge. Deferred race types must remain
  rejected by manifest/ROM admission before tick one.
- Preserve canonical roster invariance across one-, two- and no-render layouts
  and the three-epoch impaired carrier-rematch/ASan gate.
- Preserve the 16 MiB fail-atomic ring cap and native capture/restore/
  resimulation/authored-frame p99 gates; extend them to the required
  browser/PAL/OS/device matrix while retaining at least half an authored tick
  for simulation at the selected rollback window.
- Run native/wasm and NTSC/PAL cross-process convergence before opening online
  races. Until every row passes, private-room UI may prototype but cannot start
  an online race.
