# Closed-loop presentation pacing and slot-projected alpha, 2026-08-17

## Reported symptoms

After the 2026-08-16 pacing change (open-loop display deadline + second
in-flight admission slot), a 120 Hz ProMotion MacBook showed NEW jitter in
moving characters while driving, and the Timber's Island waterfall shimmer
was not fixed. This investigation re-derived the whole mechanism from first
principles, measured it on the affected machine, and replaced the open-loop
design with a closed-loop one. It supersedes the "Fix" section of
[`waterfall-interpolation-2026-08-16.md`](waterfall-interpolation-2026-08-16.md);
that document's content isolation (the texscroll registry does real work,
mipmap/anisotropy hypotheses rejected) still stands.

## Root causes

Ranked, each with the measurement that proved it. "Ideal" at a 120 Hz
display against the 30 Hz authored tick is an alpha advance of exactly
0.25 tick (250,000 ppm) per displayed frame, every frame.

**RC1a — the interpolation alpha was stamped from the wrong clock.** In the
player-representative free mode, the drawn alpha is the CPU-wake-time
accumulator. Measured on the affected machine (30 s, track 5, autopilot,
foreground WebGPU, 2026-08-17 morning, healthy session, 2026-08-16 build):
alpha-delta p50 253,952 / p95 385,024 / p99 458,752 ppm, min 0 with 15
stalls, max 1,000,000 ppm (a full tick in one frame). Consecutive motion
steps varying 0.5x-1.5x of true velocity IS the character jitter, and a
continuously scrolling high-contrast texture (the waterfalls) is the most
sensitive possible detector of the same modulation. The FIFO queue displays
frames evenly; the CONTENT sampled into them was not evenly spaced — the
"Elusive Frame Timing" failure shape.

**RC1b — two unsynchronized pacers in series.** The 2026-08-16 change added
an open-loop absolute deadline at the reported integer refresh (120.000000
Hz on CLOCK_MONOTONIC, no feedback of any kind) in front of the FIFO
acquire, which already blocks (wgpu Metal: `CAMetalLayer.nextDrawable`
inside `wgpuSurfaceGetCurrentTexture`, `desiredMaximumFrameLatency=1`).
The panel does not run at the reported integer rate: the disciplined clock
(below) measured this panel's real retirement near 249,654 ppm/frame
(~119.83 Hz), ~1,400 ppm slower than the open-loop grid — one whole-slot
overrun roughly every 6 s. Under `MDKR_PRESENT_QUANTUM_STRICT=1` the
mismatch was directly visible as 726/3,568 (20%) failed acquires
(`unavailable`) while the panel had adaptively stepped down — every one a
rendered frame the player saw as a repeat.

**RC1c — the VRR-honesty classifier was blinded.** The deadline change also
routed the classifier's input through the deadline's own sleeps, so it
measured the pacer's sleep quality rather than the display; `[ALPHA-QUANTUM]`
flapped `mode` 3 times in 30 s (alternating grid-snapped and free alpha
regimes mid-run).

**RC2 — the authored endpoint presents off-slot.** In subloop mode no pacing
runs between the tick's wake and the endpoint present, so every 4th frame
leaves tickCompute (the game pass ~5-6 ms, including the real display-list
walk) after its slot. A fixed-refresh FIFO re-times it; a VRR panel shows a
30 Hz ripple. The alpha census also pins the endpoint's phase at 0/1 while
its true residual is r in [0, 0.25), which fattens the alpha-delta tails on
either side of every boundary.

**RC3 — the second in-flight admission slot re-created a rejected hazard.**
Letting replays fill both slots removes the authored endpoint's reservation;
the repo's own 2026-08 experiment table records that failure shape
(endpointSkips 515, "authored frames starve worse than the defect being
fixed"). It also bought nothing once pacing was fixed (queue high-water 1).

**RC4 — content residuals (separate from pacing).** (a) The authored
texscroll endpoint-vs-interior mismatch is at most 3/4 of an S10.5 unit =
~0.023 texel — mathematically real, visually negligible; the waterfall
shimmer was NOT UV math. (b) The large translucent hub water sheet's ~2x
endpoint step is RESOLVED AS AUTHORED CONTENT, not a defect: the hub ocean
(all 24 `RENDER_WATER` segments, level-texture 25 / asset 1156) animates by
a 26-frame texture flipbook advancing one frame every 2 authored ticks
(15 Hz, `track_tex_anim` → `tex_animate_texture`), the same cadence the N64
shows. Frame swaps are tick-quantized by authorship, land only on endpoint
frames by construction, and the interpolation design deliberately excludes
flipbooks from the registry ("a flipbook can never be mistaken for a
scroll"). Everything else about the hub water IS owned: wave-tile matrices,
wave vertex XYZ/RGBA, and the wave grid's own UV drift all interpolate; the
flat far sheet's geometry is static. Any future endpoint-step gate over hub
water crops must model out the 15 Hz flipbook component. (c) Render-only
residuals (kart bob/spin, `camera.c` C7 census) still step at 30 Hz by
design; documented follow-up.

**RC5 — gate blind spots.** The variance bound (3e10 ppm^2) admits a 0.17
tick per-frame sigma; the replay-shed census omitted `stale` from its
denominator; failed acquires were not gated at all; and CI's strict arm
pins the legacy grid, so the player configuration (free/slot) had no
distribution gate.

## The fix (fix/interpolation-pacing)

One closed loop, one phase authority:

1. **Deadline discipline** (`pacing_policy.c`): every present feeds
   `mdkr_present_deadline_feedback(block_ns, unavailable, now)` — how long
   the surface acquire blocked, measured around `wgpuSurfaceGetCurrentTexture`.
   A phase controller (creep 25 us/frame early, clamped push later when the
   block exceeds 1 ms) locks the grid onto real retirement for mismatches
   inside 1,200 ppm with the exact integer-rational grid intact; a rate
   controller (EMA of display-bound completion intervals, 8-sample
   confirmation) re-rates the grid onto the measured period for real slews
   (ProMotion buckets), snapping back to nominal within 600 ppm. Unit tests
   cover the 119.88-under-120 lock, the 120-to-96 Hz re-lock, backoff,
   clamping, and inertness before initialization.
2. **Slot-projected alpha** (`mdkr_present_slot_phase`, wired in
   `present_sched_alpha_projected` behind `platform_present_slot_alpha_active`):
   the drawn phase advances by exactly one display quantum per replay;
   the measured wake phase is consulted only to detect a genuinely missed
   slot (deviation beyond 3/4 quantum re-anchors, counted). The tick/display
   beat's extra slot becomes one soft repeat just below the boundary
   (content-identical to the incoming endpoint). `[ALPHA-QUANTUM]` reports
   `mode=slot`; `[PRESENTSCHED-SUMMARY]` gains `slotsnaps`/`slotanchors`.
   `MDKR_PRESENT_QUANTUM_STRICT=1` still pins the legacy quantize path so
   its arm measures what it always measured.
3. **Endpoint lead + slot gate**: the wake that will make the tick due runs
   `MDKR_PRESENT_ENDPOINT_LEAD_US` (default 3,000) early when the
   accumulator allows; `platform_present_endpoint_gate()` sleeps the
   remainder so the authored endpoint present leaves ON its slot, and
   records how often the game pass overran the lead (`leads`,
   `leadmissmeanus`). Known limitation: the game pass averages ~5-6 ms on
   this machine, so the 3 ms lead partially covers it; sizing the lead from
   its own telemetry is a follow-up once ROG Ally numbers exist.
4. **Reservation restored**: `wgpu_backpressure_limit_before_frame` again
   reserves one slot for the authored endpoint; `platform_present_replay_queue_ahead`
   removed.
5. **Wake precision**: the POSIX `pace_sleep_until` now mirrors the Windows
   shape — coarse sleep to 1 ms short, then 100 us micro-sleeps — because
   Darwin coalesces bare nanosleep wakes by whole milliseconds.
6. **Gates** (`check_pacing_quality.py`): shed-census denominator includes
   `stale`; a presenting session must have `unavailable=0`; the disciplined
   (player-config) arm is now held to grid-relative uniformity bounds
   (alpha-delta p95 <= 1.5x quantum, max <= 2.5x quantum, stalls and
   re-anchors within the beat budget), and `mode=slot` is asserted whenever
   `[PRESENT-DISCIPLINE] active=1`.

Everything above is scoped to discipline mode (native WebGPU + realtime +
`FrameLimit=display` + non-tearing blocking FIFO). Byte-identity proof:
`check_arbitrary_presentation_rates.py` passes unchanged, as do
`check_gpu_backpressure.py`, `check_surface_suspension.py`, and the
pacing/sim_sched/presentation unit suites.

## Measurements

All 30 s, track 5, autopilot, WebGPU display+interpolate, 320x240,
M3 ProMotion MacBook, live foreground session. Ideal alpha step at 120 Hz
over the 30 Hz tick is a constant 250,000 ppm; the census bins at 4,096 ppm,
so 253,952 is the upper edge of the bin CONTAINING the ideal step.

| Metric | 2026-08-16 build, free | 2026-08-16 build, strict | This fix, slot |
|---|---:|---:|---:|
| alpha-delta p50 | 253,952 | 253,952 | 253,952 |
| alpha-delta p95 | 385,024 | 503,808 | **253,952** |
| alpha-delta p99 | 458,752 | 503,808 | **253,952** |
| alpha-delta variance (ppm^2) | 4.1e9 | 10.4e9 | **5.0e8** |
| stalls | 15 | 0 | 0 |
| regressions | 0 | 0 | 0 |
| acquire failures (`unavailable`) | 0 | 726 (20%) | 2 |
| displayed p95 / p99 (us) | 9,400 / 11,000 | 10,000 / 12,800 | **9,700 / 10,600** |
| endpoint slot miss (mean) | unpaced (+1-3 ms) | unpaced | **455 us** (891/900 ticks led) |
| mode | free (flapped 3x) | grid (forced) | slot (stable) |

The post-fix alpha distribution is a single histogram bin through p99 —
uniform motion to within census resolution. The residual max=1e6 is one
~29 ms host scheduler stall in 30 s (present in every arm of every build);
its content jump is the correct response to genuinely lost time.

Two implementation defects were found and fixed during live validation,
each with the measurement that exposed it:
- The slot projector originally compared measured phase against a
  zero-based absolute slot sequence; the per-tick constant offset
  (endpoint lead + tick/slot residual) swept through the snap window with
  the beat and re-anchored every replay of the affected ticks (637/2,500
  in one 30 s run, matching the 3-per-tick x beat-fraction prediction).
  The comparison is now increment-based (one displayed frame must advance
  the measured phase by ~one quantum), which is offset-invariant by
  construction; re-anchors fell inside the beat budget immediately.
- `check_pacing_quality.py`'s realtime verdict: two consecutive full-suite
  PASSes on the live session, including the strict M3-baseline arm and the
  new slot arm (grid-relative bounds, honest-arm retry policy mirroring
  the strict arm's bimodality rationale, and an `unavailable` budget of
  presented/1000 — the defect it guards against measured 200x that).

## Acceptance for release (ROG Ally 120 Hz VRR, plus this machine foreground)

- `[PRESENT-DISCIPLINE]`: `unavailable=0`, blockp50 in [400, 2,600] us
  (locked equilibrium), period within 2,000 ppm of the panel's real rate.
- alpha-delta: p95 <= 1.5x quantum, max <= 2.5x quantum, stalls and
  slotanchors within the beat budget (`check_slot_quality`).
- displayed-interval p95 <= 1.2x quantum; zero endpoint admission skips;
  replay shed < 5% with the stale-inclusive denominator.
- Visual pass at the hub spawn and driving past all three falls
  (issues #35/#36 per the 2026-08 issue batch).

## Second wave (2026-08-17, after visual playtest): content coverage

The playtest falsified "waterfalls fixed": cadence was measurably uniform,
yet the falls still flickered and cutscene characters smeared. Both are the
signature of CONTENT that declines interpolation stepping at 30 Hz against
a gliding 120 Hz camera. The per-class verdict census
(`MDKR_SMOOTH_VERDICT=1` + `MDKR_PRESENT_SCHED_TRACE=1`) attributed it on
the real hub route:

- **WORLD_SCROLL held 46% of its verdicts (`heldpermille=456`), all
  `UV_HOLD`.** Two mechanisms: (a) the measured path's two-tick confirm
  rule can NEVER pass for a scroller whose rate varies per tick (measured
  du sequences -83, -72, -60 on the hub's eased/camera-coupled sheets —
  every such surface held forever); (b) every scroller's first visible
  tick held (`holdunpub`), a constant churn while driving.
- **The authored texscroll registry never engages in the hub at all**
  (`uvauth_reg=0`, now a permanent `[SNAPSHOT]` census): the hub does not
  run `obj_loop_texscroll`, so the 08-16 premise that the authored
  {rate, phase} path served the falls was wrong there. It serves race
  tracks that have `BHV_TEXTURE_SCROLL` objects.
- The falls' main vertical scroll (constant dv=128/tick) did confirm and
  blend — the flicker came from the held companion layers around it.

**Fix:** `gfx_presentation_packet_lookup_uv_scroll` now accepts a
single-tick measured record when the batch itself corroborates it — two or
more triangles whose corners all state one fold-resolved displacement (the
mis-resolved-wrap failure the two-tick rule guarded against cannot move
independent triangles by the same wrong amount). One-triangle batches keep
the two-tick rule. The record's du describes exactly the [T, T+1] interval
being replayed, so this is also the CORRECT value for variable-rate
scrollers, not a compromise. Hub route result: heldpermille 456 → 27,
`uvscrollsolo=74,147` accepts, gates all green (194/194 unit,
byte-identity, pacing).

Character/cutscene note: OBJECT_ROOT blends 99.8%; the deformation
contract correctly lerps within one animation and snaps on id changes.
The cutscene backdrop layers were part of the held-scroll population above.
If smear persists in cutscenes after this fix, the next suspects are the
`deformincompatible` population (~2.6/tick, needs per-owner attribution)
and sample-and-hold persistence at the panel's adaptive ~91-99 Hz.

## Third wave (2026-08-17): menu-shell scenes captured zero cameras

The second playtest narrowed the residual to cutscene/attract characters
(Taj's greeting entrance, title-screen racer demos) and "falls terrible
only while moving". Frame dumps made the mechanism unambiguous: in the Taj
bridge shot, consecutive 120 Hz frames measured 0.33/0.33/0.33/6.2 — three
near-identical interiors then a 19x endpoint jump. UV scrolls kept gliding
(which is why static falls looked fine) while the camera and every object
stepped at 30 Hz; a scene whose framing steps takes the falls with it,
hence "fine static, terrible moving". Scheduler telemetry agreed:
`interp=6872` replays but `interpviews=5712` — 1,160 replays drew with no
interpolated camera view at all.

Root cause: `capture_cameras` (presentation_snapshot_walk.c) captured
cameras under `GAMEMODE_MENU` only for `RACETYPE_CUTSCENE_1/2` levels. The
title attract demos are live AI races loaded by the menu shell as
`RACETYPE_DEFAULT` (player count 0, not the cutscene sentinel), and the
Taj greeting cinematic falls in the same class — those scenes captured
ZERO cameras every tick, so `mdkr_camera_interpolated_view_projections`
had nothing to substitute and the fail-closed design correctly rendered
authored frames only. Fix: capture for EVERY loaded level scene under the
menu shell; the menu-borrow hazard the old gate guarded against still
cannot pass, because a borrowing menu never latches an authored camera
record and `authored_cameras_copy()` returns zero entries for it.

Frame-dump proof, same shot, same window: flat ~1.96 per-frame advance
after the fix (no periodicity). Gates: camera snapshot coverage,
presentation lifecycle, byte-identity, pacing quality, 194/194 unit — all
PASS.

Also established this wave: the wave-grid topology snap on 5x5 window
shifts is CORRECT (buffer slots are reused by different world tiles, so
cross-shift vertex lerp would smear two tiles together); cutscene-vehicle
wheel/prop parts advance `modelIndex` per tick (authored flipbook class);
and computer racers' vertex animation is authored at 15 Hz.

## Remaining work

- Visual re-test: falls while driving, Taj entrance, title attract racers.
- Per-owner attribution of the residual `deformincompatible` (~2.6/tick)
  population if character smear persists anywhere after this wave.
- ROG Ally 120 Hz VRR qualification with the same acceptance numbers.
- Endpoint lead auto-sizing from `leadmissmeanus` telemetry; the game-pass
  cost (~5-6 ms, dominated by the real walk) also bounds how good any lead
  can be at 120 Hz — pipelining the walk is the structural fix if the Ally
  measurements demand it.
- C7 render-only residual stepping (kart bob/spin) — design follow-up.
- (RC4b closed 2026-08-17: the hub water sheet's step is the authored 15 Hz
  ocean flipbook, not an ownership gap. If sub-tick smoothing of flipbooks
  is ever wanted, the contract would be phase-aligned frame selection or a
  two-frame cross-fade — both diverge from authored appearance and are
  deliberately NOT planned.)
