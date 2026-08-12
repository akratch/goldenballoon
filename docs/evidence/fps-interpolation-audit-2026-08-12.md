# Open issues, FPS and interpolation audit — 2026-08-12

## Public issue investigation

GitHub currently has two open player reports. Both now have default-behaviour
fixes and fault-injection coverage; neither issue has been closed ahead of a
release.

### #31 — Magic Codes did not persist

The menu kept unlocked and active codes only in process globals. EEPROM was not
the right owner: several codes deliberately alter how a save is interpreted,
while others are unsafe or meaningless to restore. The fix adds a small,
versioned sidecar with a strict checksum and an explicit allow-list. Ordinary
player modifiers—including `TIMETOLOSE` and `TEENYWEENIES`—restore. Progression
grants, one-shot rewards, credits, lockup/checksum diagnostics and Taj state do
not. Mutually exclusive code groups are validated on both write and restore.

Native writes use the same atomic text-state backend as Taj state. Browser
writes are coalesced by generation: an IDBFS rejection keeps the latest MEMFS
state dirty and an ordinary later flush retries it. Tests cover corruption,
excluded bits, conflict groups, synchronous failure, asynchronous coalescing,
generation changes, a native two-process restart, and a real Chromium IDBFS
rejection followed by a page reload.

### #30 — music and silver-coin audio disappeared

The player described two manifestations of compressed-sequence note loss. The
Bluey rematch's level header limited music to 19 concurrent voices even though
the native logical pool has room for 26; the production route rejected seven
authored note-ons and peaks at 25 voices after the fix. Separately,
`alCSPNew()` allocated the configured jingle voices but failed to copy
`ALSeqpConfig.voiceLimit` into the player. Music masked that constructor bug by
setting its limit immediately. Jingles started at limit zero, admitted one
voice through the API's inclusive comparison, then rejected the rest. A
silver-coin pickup calls the jingle player directly, so the report precisely
matched that fault.

The constructor now installs its configuration, and native music uses its
complete bounded logical pool. The shared 40-voice physical synth budget is
unchanged. On the progression-valid Bluey route, 4,530 note-ons are admitted
with zero unintended rejects or physical steals (`musicPeak=25/26`,
`jinglePeak=2/16`). The general 143.5-second audio capture remains green, and
the ROM-derived ares comparison is +0.840 dB broadband with every frequency
band inside the established tolerance. This changes admission, not master
gain, tempo or sink policy.

## Verdict

The presentation architecture is sound and substantially ahead of a blanket
“lerp every transform” port. Gameplay remains on the ROM's authored 30 Hz NTSC
or 25 Hz PAL clock. Extra display opportunities replay an immutable captured
task between two authoritative endpoints. Every replay decision passes through
one alpha resolver, extrapolation is clamped out, discontinuities fail closed,
and topology-changing content snaps rather than inventing an in-between mesh.

The remaining work is mostly breadth and hardware evidence, not a missing core
algorithm. The clear software wins found in this audit are implemented:

- display-interval classification now uses a trimmed 32-sample window with
  hysteresis, rather than letting one stall classify a fixed panel as VRR;
- a variable-refresh interval distribution declines the constant-grid alpha
  quantizer by default;
- yaw, pitch, roll and FOV cuts hold the composed camera atomically;
- per-class verdict telemetry reports the exact held-frame share;
- just-in-time input sampling is now the default and removes one presentation
  interval of avoidable input wait while interpolation is active;
- the full frontend-to-finish adventure route now rejects uncaptured replay
  dependencies and excessive per-class held-frame shares;
- both shield and magnet shells now have independent pixel-envelope witnesses.

No evidence supports changing the authoritative tick rate, extrapolating, or
enabling hard-pan demotion by default. Those would trade known correctness for
an unmeasured preference.

## External checks used

The implementation was checked against platform and engine guidance, not used
as a substitute for measurements from this codebase:

- Apple says the actual frame interval is `targetTimestamp - timestamp`, warns
  that the requested rate is not guaranteed, and recommends testing gradual and
  sudden refresh changes. That supports measured interval classification rather
  than trusting a nominal panel rate: [Optimizing for ProMotion displays](https://developer.apple.com/documentation/quartzcore/optimizing-iphone-and-ipad-apps-to-support-promotion-displays).
- Apple's macOS display-link API exposes actual and nominal output periods as
  different facts. That is the same distinction this port now makes between the
  reported display ceiling and observed presentation intervals:
  [CVDisplayLink](https://developer.apple.com/documentation/corevideo/cvdisplaylink-k0k).
- The browser animation callback normally follows display refresh and supplies a
  timestamp; code must not advance animation by a fixed amount per callback on
  high-refresh displays: [requestAnimationFrame](https://developer.mozilla.org/en-US/docs/Web/API/Window/requestAnimationFrame).
- Godot's interpolation guidance calls out teleports, indirect/parent motion,
  cameras, and low-tick-rate testing as the places interpolation commonly
  breaks. This tree's generation, camera-cut, external-owner, and low-rate
  negative-control gates cover those same failure classes:
  [Using physics interpolation](https://docs.godotengine.org/en/stable/tutorials/physics/interpolation/using_physics_interpolation.html).

## What the code does now

### Authority and scheduling

`platform/present_sched.c` and `platform/host_frame_driver.c` own exact rational
host time and fixed simulation tickets. A display rate above authored cadence
creates presentation opportunities; it does not create extra gameplay ticks.
The arbitrary-rate gate compares state, ordered gameplay events, consumed input,
and PCM across Original, display-derived, 30/40/60/90/120/144/165/240, uncapped,
NTSC, PAL, GL, and WebGPU arms.

### Replay lifetime

`platform/fast3d/gfx_retained_task.c` captures the complete arena image and
explicit external dependencies transactionally. A failed capture leaves the
last complete task intact. An unresolved dependency refuses the interpolated
walk and holds a completed authored image; replay never reads the live arena the
next tick is writing.

### Ownership and correspondence

Object roots and children use generation-keyed snapshot identity. Billboards
carry both their anchor and local matrix. Weather-owned vertices and external
transforms have explicit renderer identities. Wave deformation uses a per-tile
owner and topology key. Projected shadows detect vertex-slot reorder before the
optional per-vertex path can blend it. UV scroll requires an authored-rate and
identity match. Primitive alpha, particles, effect shells, and deformations use
the same retained tick pair.

The skydome has no spawned-object identity, but its actual defect was narrower:
its tick-T camera translation was being recomposed against an interpolated
view-projection. A camera-locked replay now substitutes the interpolated eye.
The pixel gate recreates the frozen-translation defect as a token-gated negative
control.

### Discontinuities and cameras

Spawns, recycled object addresses, teleports, finish/spectate cameras, topology
changes, and camera cuts do not blend. Camera classification uses the composed
view pose—not an incomplete subset of the underlying camera slot—and treats
yaw, pitch, roll and FOV as one atomic decision. An individual object's
shortest-arc backstop remains as a separate safety net.

### Display timing

`platform/pacing_policy.c` classifies observed intervals from a bounded window.
It trims isolated long/short outliers, requires sustained evidence to enter the
variable state, and stronger sustained evidence to return to fixed. The alpha
quantizer only projects onto a nominal refresh grid when the observed intervals
support that claim. Synthetic, tearing, latest-image, software-deadline, and
unknown-refresh paths already decline the grid for independent reasons.

This matters on ProMotion/VRR: a blocking queue can retire each present without
following the panel's advertised maximum-rate grid. Quantizing alpha to that
fictional grid creates repeated or uneven phases even though the raw measured
phase is correct.

### Input latency

With Interpolated/display resolving to 60 Hz on the same ProMotion Mac, the
capture-to-commit census measured:

| arm | sample p50 | sample p99 |
|---|---:|---:|
| late sample disabled | 16.1 ms | 17.3 ms |
| late sample enabled | 0.1 ms | 0.1 ms |

Original already measured 0.1 ms because its input pump follows the authored
pacing wait. The default therefore removes accidental latency where it exists
without claiming a benefit where it does not. The 60 Hz scripted control must
remain byte-identical with the diagnostic opt-out for state, events, consumed
input, and PCM.

## Evidence from this audit

- Robust pacing classifier unit and mutation cases: pass.
- Real ProMotion pacing quality, including a positive variable-refresh witness:
  pass.
- Four-route camera snapshot coverage, including long adventure and cutscene
  ownership: pass.
- Wave midpoint reconstruction: 95 acted midpoints, zero endpoint locks, pass.
- Smooth verdict production run: WATER_WAVE 153 blend / 0 snap; OBJECT_ROOT
  83 / 7; WORLD_SCROLL 11 / 0. Every owned wave draw blended, with zero
  topology mismatches and zero missing owners.
- Full adventure route: three level lifetimes, 8,486 replay walks, zero
  uncaptured externals/refusals, zero unconsumed camera cuts; held shares were
  WORLD_SCROLL 228, WATER_WAVE 49 and OBJECT_ROOT 36 permille, all below their
  route ceilings.
- Shield and magnet pixel envelopes: 0/90 violations apiece. The magnet arm
  reaches 96 effect overrides with more than 5,300 owner-tick checks and zero
  mismatches;
  its same-route magnet-off reference has zero keyed pixels in the measurement
  region.
- The designed GL motion-quality battery passes all five rows: 12,777,444 seam
  checks with zero violations, no forbidden blend through discontinuities, no
  interval regressions/stalls, and +2.83% replay cost against its 8% ceiling.
  The WebGPU synthetic-clock arm remains diagnostic: it submits 120 Hz work
  without allowing wall time for the GPU queue to retire and therefore records
  optional holds. The real-time WebGPU arm is healthy; this harness distinction
  is recorded in [`docs/open-items/renderer.md`](../open-items/renderer.md), not
  misreported as a product failure.
- Audio and gameplay streams remain authored-cadence controlled; presentation
  changes do not alter their clocks.

The native build is clean and 185 of 187 configured CTests pass. The two
exceptions are `rollback_snapshot` (SIGTRAP) and `rollback_ring` (abort), both
from the pre-existing untracked multiplayer work present before this audit.
Neither test nor its rollback implementation overlaps these changes. This means
the issue/FPS work is qualified, but the shared dirty worktree as a whole cannot
honestly be called all-green until that separate multiplayer work is repaired.

The raw counters remain authoritative. `heldpermille` is derived as
`snap * 1000 / (blend + snap)` and the gate recomputes it, so dashboards cannot
quietly disagree with the underlying verdict counts.

## Remaining gaps, in priority order

### 1. Windows and Linux VRR present timing — hardware evidence required

The interval classifier and grid-decline policy are platform-neutral, but this
Mac cannot prove DXGI/Vulkan/compositor behavior on a FreeSync/G-Sync panel.
Run the pacing-quality realtime arm on Windows and Linux at fixed refresh and
VRR, then correlate its interval state/transitions with PresentMon or equivalent
OS presentation data. Acceptance: fixed mode stays fixed through isolated
stalls; VRR enters variable without flapping; displayed cadence has no repeated
phase pattern; no tearing when disabled.

This is not grounds for another heuristic. If platform presentation timestamps
become available, prefer them over CPU-side swap-return intervals and retain the
current classifier as the fallback.

### 2. Input-to-photon latency — hardware evidence required

The in-process census ends when submit/present returns. It cannot see scanout,
display processing, controller USB/Bluetooth polling, or pixel response. The
software win is large and isolated, but an LED/high-speed-camera or LDAT-style
test is needed for an end-to-end claim. Test Original and Interpolated at 60,
120, display, and uncapped; report distributions, not a best sample.

### 3. Skydome formal identity — low priority, no current defect

The camera-locked substitution is correct for the content that exists and has a
pixel negative control. A formal external-transform owner would make the census
uniform, but would duplicate the camera-relative rule and add lifecycle state.
Do it only if future skydomes gain independent motion, rotation, or multiple
instances. It is not a present release blocker.

### 4. Same-display mode switches — platform limitation

SDL2 reports a window moving to another display, but may not report a refresh
change on the same display. The observed-interval classifier adapts the alpha
decision anyway; `display` pacing naturally follows the blocking queue. What can
remain stale is descriptive nominal-rate telemetry and numeric policy ranking
until the setting is re-applied. SDL3 or native display notifications should be
evaluated during a platform-layer upgrade, not bolted into gameplay code.

### 5. Performance headroom on low-power hardware — device matrix

The replay path has CPU-stage timing gates and bounded retained memory, but a
desktop Mac does not prove 1%/0.1% frame-time behavior on an Ally, Steam Deck,
integrated Linux GPU, or older Windows laptop. Capture present interval,
interpolation cost, replay refusal, GPU backpressure, and thermals for at least
20 minutes per device. A stable lower cap is better than intermittently missing
a higher deadline.

## Deliberately rejected changes

- **60 Hz gameplay simulation by default:** changes physics, AI, timers, RNG
  consumption and audio scheduling. Presentation smoothness does not require it.
- **Extrapolation:** lowers nominal latency by drawing a state the game has not
  authored and produces the worst errors at collisions, cuts and topology
  changes.
- **Blanket interpolation:** content without identity or correspondence must
  hold. A plausible-looking blend between unrelated data is corruption.
- **Default hard-pan demotion:** wave geometry now has correspondence and the VRR
  quantizer no longer assumes a fixed interval. No device evidence shows that
  replacing correct midpoints with stepping during a pan is better. Keep the
  existing arm diagnostic-only.
- **More settings:** ownership, timing classification, camera atomicity and late
  input sampling are correctness policy. They should be good defaults, not
  player-facing repair switches.

## Release bar for this subsystem

1. Unit/contract tests for pacing, snapshots, packets, retained tasks and input
   queue pass.
2. Arbitrary-rate authority streams and PCM are byte-identical.
3. Motion battery, smoothing-stage bisection, wave/effect envelopes, camera
   coverage, shadows, and smooth-verdict gates pass with their negative controls.
4. Realtime pacing passes on one fixed-refresh and one variable-refresh display.
5. Human play covers rapid pans, water, skydome, particles, finish/spectate
   cameras, respawn, shield and magnet, menus, transitions, and live rate changes.
6. Windows/Linux hardware gaps above are recorded honestly until measured; they
   are not converted into unproven defaults.
