# A3 rollback evidence

Decision: **OPEN — automated native laboratory passes; no online-race `GO`.**

| Field | Evidence |
|---|---|
| Authority/manifest | 1,618 reviewed file-scope declarations + injected omission/local-scope control; manifest byte/reserved/id mutation |
| Input/snapshot/events/driver | ROM-free CTest units |
| Endpoint laboratory | Four endpoint seeded convergence; disabled control diverges. The same four isolated launcher/engine processes pass LAN, regional-good, regional-variable and poor profiles through the production transport seam. Two-second-outage and adversarial profiles preserve an exact pre-fault prefix, detect the first unrecoverable input gap at the 31-tick replay boundary, unwind the engine cleanly and enter the launcher's typed connection-lost recovery scene. Schedules cover latency, jitter, loss, duplicate, reorder, corruption, outage, bounded delivery, offset/drift, long frames, skipped host opportunities and sleep/wake without altering authored `dt`; counters are required nonzero and same-seed state is byte-identical. |
| Canonical/local roster | Canonical manifest plus independent local-seat/view maps; copy-owned atomic manifest+roster engine publication, no-render unit, exact-epoch/rematch rejection and source mutation controls pass. Before tick one, the engine compares the loaded ROM's track id, catalog race type and authored regional cadence with the frozen standard-race manifest. A deliberate track mismatch exits nonzero through cooperative host teardown—no abort, partial ring or gameplay tick. `net_local_input` atomically maps local-seat samples into owned canonical slots while preserving every remote slot and rejecting malformed/ambiguous input. Launcher-owned `MdkrMatchTransport` consumes that seam through an engine-lifetime copy-out provider: authenticated remote ownership, additive confirmation, exact correction ticks, atomic local drain and typed recovery. A shared 32-snapshot ring permits at most 31 replayed ticks and accepts an input at authored age 30; older late input or an aged contiguous-input gap fails closed into launcher recovery instead of reading evicted history. The lower-level fixed 24-byte big-endian `MINP` v1 codec remains covered. Profile traffic uses a fixed 64-byte `MB` v1 bundle containing one to three consecutive all-slot frames, CRC-16 and byte-neutral unused fields, so the latest three frames survive ordinary loss without trusting a payload ownership mask. Every single-byte mutation is rejected and decode is fail-atomic. Authenticated ownership remains out-of-band and the decoded slot mask may only narrow it. A real A-edge withheld for four ticks triggers a five-tick replay and convergence. A deployed carrier/authentication handshake remains open. |
| Real engine range registry/restore | IN PROGRESS; atomic object/list/settings/global/camera/particle/wave/behaviour/transition/input registration restores heap, scalar and auxiliary mutations, preserves allocator next-address identity and runs explicit presentation rebuild hooks. The production ring is uniformly 32 slots. Production input history withholds P1 ticks 117–120, authors predictions, reports dirty tick 117 on late delivery, restores tick 116, requires corrected non-input authority to diverge, and reproduces corrected tick 120 byte-for-byte on a second replay on all 20 standard tracks with an observed human car, all 47 ROM-legal standard-track/player-vehicle pairings and all 15 balloon type/level configurations. Snapshots span 297,441–305,729 bytes across 141–153 registered ranges. Native WebGPU 2P and 4P race-start routes pass the same delayed-correction proof. A complete autopiloted 4P race exactly replays ticks 4,797–4,800, reaches four results and the results-scene load, and passes item-spawn churn. Deferred race types are explicit admission negatives. |
| Side effects / persistence | IN PROGRESS; real sound, rumble and save emissions enter the bounded journal without replacing the accessibility observer. Corrected-only preview, vanished cancellation, confirmation retirement, resimulation I/O denial and online progression-write rejection pass units. Production SFX starts are bound to stable event ids and versioned `SoundHandle` generations: replay duplicates are suppressed, corrected-only starts are deferred, vanished voices cancel without ABA reuse, and final volume/pan/pitch/priority/stop commands coalesce and flush after reconciliation. Spatial source pool/topology state rewinds with authored ticks while the physical audio queue remains host-only. A native 4P correction exercised 19 starts, 4 suppressed duplicates, 4 deferred starts and 40/160 applied/coalesced commands with zero adapter rejects or overflows. Vanished predicted rumble stops the bounded physical controller directly. EEPROM/IDBFS and every Controller Pak mutation are firewalled at their lowest shared durable boundaries. Physical audio/rumble qualification and achievement/diagnostic-export firewalls remain OPEN. |
| Standard tracks/vehicles/finish/items/10k ticks | AUTOMATED COMPLETE / PLATFORM EVIDENCE OPEN; all 20 standard tracks pass delayed correction/exact second replay with an observed human car, an independent ROM-mask enumeration passes all 47 legal standard-track/player-vehicle rows, and all 15 balloon type/level configurations pass their real item branch with mutation/bounds/mode controls. Whale Bay completes 10,180 authored ticks with the instantiated player vehicle observed as hovercraft using the current 32-slot ring: 10,189 captures, three restores, a 303,761-byte snapshot/9,720,352-byte ring and zero timing/effect/I/O overflow. A full 4P standard race passes a deep-race replay with 2 item spawns, 224 checkpoint events and 4 results. |
| Real viewport/camera/HUD independence | AUTOMATED PASS; authoritative player-count reads across race init/input/AI/world/items/particles/finish/records share the canonical roster seam. Four isolated production launcher/engine processes enter via one manifest but distinct slot-0/slot-1/2-local/no-render envelopes and retain identical 3,600-row authority/input streams and canonical event projections. Presentation separates output rectangles from canonical cameras, proves lens equivalence before drawing and stores immutable viewport bytes. Single-seat cameras fill the drawable; slots 0+2 form a live top/bottom split with a 96.9% dark divider. The minimap uses local layout. Every per-player HUD element is rendered from a stack shadow that rebases canonical animation deltas onto the endpoint's one- or two-view preset, leaving rollback-owned `HudData` untouched; process witnesses prove every local output was actually moved or scaled. Spatial voice lifetime is forced back to all four canonical cameras even after local rendering changes the active output layout. Final volume/pan is then selected from only frozen local listeners: one view stays spatial, couch audio uses the loudest local listener and centers, and zero views are physically silent without stopping canonical voices. Source mutations detect loss of canonical audio count, HUD shadow redirect, camera mapping, lens guard, viewport storage, local minimap or no-render suppression. Physical listening quality and broader standard-track/rematch/platform matrices remain OPEN. |
| Native/wasm, NTSC/PAL, 2P/4P | PARTIAL; native WebGPU 2P and 4P delayed-correction routes pass. The 4P route also completes its 9,600-frame race/results visual oracle and its 2,700-frame race-start route under AddressSanitizer. The linked wasm product builds with the rollback/network sources; browser runtime parity and PAL evidence remain OPEN. |
| Snapshot bytes and p99 CPU | IN PROGRESS; rollback timing uses allocation-free bounded 10 µs histograms and reports p50/p95/p99/max independently for capture, restore, each resimulated game tick and each authored gameplay frame. The 20-track/47-legal-vehicle/15-item gates require exactly eight replay samples per correction, non-vacuous ordered frame samples, p99 at or below 8.33 ms, zero histogram overflow above 20.48 ms and zero samples beyond the hard 16.67 ms authored-tick deadline. Four-process network profiles enforce the same distributions while exercising real correction bursts. Across current native breadth, the largest full ring is 9,783,328 bytes and maximum capture/restore/resimulation/authored-frame p99 is 1.30/0.97/0.27/0.82 ms. The Whale Bay 10,180-tick observed-hovercraft soak records 0.98/0.90/0.15/0.48 ms p99. Allocation fails atomically above the 12 MiB product cap. Cross-platform/browser and renderer/GPU presentation-time matrices remain OPEN. |
| Session/results/two rematches | PARTIAL; `check_persistent_rollback_rematch.py` completes three 8,200-frame 4P standard races/two rematches in one native launcher process. Each epoch reaches the tick-4,800 deep replay and results (`result=7` including replay rows), preserves one session id, returns through a launcher draw and tears host state down cleanly. `check_online_profile_rematch.py` additionally runs three 2,800-tick regional-variable online epochs through the same persistent launcher, requiring new exact epochs/manifests, non-vacuous carrier faults, zero stale/unauthorized/conflicting/out-of-window ingress, no recovery and three clean teardowns. Its ASan run exposed and now protects retirement of arena-backed sound-player roots and every particle pool before the next engine loan. Production online result exchange and persistent browser full-race parity remain OPEN. |
| Disconnect takeover | AUTOMATED PASS; the launcher accepts one already-authorized future `(epoch, slot, tick)` control decision, rejects stale/past/conflicting controls, treats duplicates idempotently and forbids handback. Grace ticks retain ordinary predicted/received input; activation and later history are confirmed neutral while the independent replay-stable mask temporarily routes the vehicle solver through existing AI and restores its canonical human identity before camera/HUD/results consumers. Peer input at/after the cutoff is ignored as taken-over rather than becoming a second owner. The four-process gate converges when the same slot is remote on three endpoints and local on one, with one activation each and no aged-gap recovery. Its default cutoff consumes and replaces a real authored A edge, rather than an already-idle sample. Product reconnect timers and delivery through the future room control log remain A4. |
| Reviewer/date/decision | OPEN |

The delayed-input evidence command intentionally omits `MDKR_AUTOPILOT` so the
late canonical P1 throttle/steering reaches the human racer:

```sh
MDKR_AUDIO=0 MDKR_ROLLBACK_LAB=1 \
MDKR_ROLLBACK_LAB_ROUNDTRIP=1 MDKR_ROLLBACK_LAB_DELAYED_INPUT=1 \
MDKR_LOAD_TRACK=5 \
MDKR_TEST_SCRIPT_ONLY_INPUT=1 ./build/mdkr64 --rom '<verified-us-v80-rom>' \
  --headless-ticks 2800 \
  --input-script tests/input_scripts/nav_to_time_trial_race.txt \
  --window-size 320x240
```

The real-process topology baseline is repeatable with:

```sh
python3 tests/check_online_process_convergence.py \
  --build build --rom baserom.us.v80.z64
```

It uses the launcher-owned exact-epoch `MdkrSessionLaunchV2` path, not a direct
engine roster/input-provider hooks. Its four temporary processes isolate preferences and saves,
verify one manifest digest, compare every authority/input row and the canonical
projection of transition/world/result events, require clean teardown, and run a
fail-closed remote-viewport negative. Sound/music/rumble are excluded from that
event projection because they are endpoint-local feedback; the no-render arm
must actually omit at least one such event while executing zero world passes.
The nonempty arms also capture the mapped scene: slot 0 and slot 1 must differ
full-screen and must not leave a uniform right/bottom clear-colour gutter,
while the 0+2 couch endpoint must have two distinct halves and a
real divider. They additionally require one draw-only HUD reflow witness per
mapped output and exact `spatial`/`shared-center`/`silent` listener-policy
witnesses. Every remote scripted port crosses authenticated launcher ingress;
the slot-1 endpoint also withholds canonical slot 0's A-edge for four ticks and
must converge after the resulting five-tick production rewind/replay. This is a
deterministic carrier fault seam. Named profiles use the three-frame bundle;
neither seam is presented as a deployed WebRTC carrier or authentication
handshake.

For local-player breadth, use presentation frames rather than simulation ticks
because the menu scripts are authored against visible-frame cadence. Replace
the input script above with `race_2p_split.txt` or `race_4p_split.txt`, remove
`MDKR_LOAD_TRACK`, set `MDKR_PRESENT_RATE=original`, and run
`--headless-frames 2700`. Both routes pass delayed correction and exact second
replay. The four-player route initially exposed an unsafe authoritative-water
walk over the mixed object/particle list; particle, header-group and allocation
guards now follow renderer order, with a ROM-free source contract and an ASan
route protecting the fix.

The same 4P route now covers the production audio adapter. Its registered
spatial-source, shared-model lifetime and post-race boundary raises that route
to 141 ranges, a 297,441-byte snapshot and a fixed 9,518,112-byte 32-slot ring. The corrected replay suppresses four
duplicate physical starts, defers four corrected-only starts, and reduces 160
replay-time SFX parameter/priority/stop writes to 40 final commands applied
only after journal reconciliation. Voice cancellation is guarded by a
monotonic allocation generation, so a short one-shot cannot end, reuse its
fixed-pool address, and cause rollback to stop the replacement sound.

The 8,200-frame arm requalified the additive boundary at tick 4,800. It exposed
three missing post-race/scene-load latches and the laboratory's obsolete ban on
replaying an already-authored post-race tick. Those latches now restore with
race authority; post-race replay is admitted while recursive replay, pause,
textbox and scene-load boundaries remain fail-closed. The route exactly
replayed tick 4,800 in post-race, tore the lab down at tick 5,196, applied 20
coalesced final SFX commands, and retained zero overflow or forbidden I/O.

`MDKR_ROLLBACK_LAB_TARGET_TICK` selects a bounded authored replay target while
retaining tick 120 as the default. The complete 4P standard-race breadth arm
uses autopilot, `MDKR_ROLLBACK_LAB_RESIM=1`, target tick 4800 and 8,200
presentation frames. It exactly replays ticks 4,797–4,800, then tears the race
laboratory down as the results scene loads. The additive audio/post-race
requalification reached teardown at tick 5,196, retired 1,514 side-effect rows,
and retained zero overflows or forbidden I/O. Its raw breadth counters include
the deliberately replayed post-race rows, so the earlier non-replay census
remains the item/result-count baseline rather than being silently compared to
those larger diagnostic totals.
Autopilot deliberately makes this an exact-replay/lifecycle arm, not the
changed-input negative control owned by the race-start routes.
