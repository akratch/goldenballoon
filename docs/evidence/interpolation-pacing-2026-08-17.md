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
endpoint step is an ownership gap, tracked separately. (c) Render-only
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
M3 ProMotion MacBook.

| Metric | 2026-08-16 build, free (healthy session) | 2026-08-16 build, strict (healthy) | This fix, slot (DEGRADED session) |
|---|---:|---:|---:|
| alpha-delta p50 | 253,952 | 253,952 | 249,856-253,952 |
| alpha-delta p95 | 385,024 | 503,808 | 503,808 |
| alpha-delta max | 1,000,000 | 1,000,000 | 1,000,000 |
| stalls | 15 | 0 | 0 |
| regressions | 0 | 0 | 0 |
| acquire failures (`unavailable`) | 0 | 726 (20%) | 0-2 |
| displayed p95 (us) | 9,400 | 10,000 | 10,350 |
| mode | free (flapped 3x) | grid (forced) | slot |

**The DEGRADED caveat is load-bearing.** Every post-fix capture so far ran
against a locked/just-woken session: the same session free-ran the GL
control at 860 presents/s (no compositor vsync at all), which is the
documented `explain_unthrottled_presentation` environment. The residual
2-quantum p95 tail in the post-fix column is dominated by whole-slot losses
from background scheduler throttling, not by the alpha pipeline (stalls 0,
p50 exactly on grid, zero overruns, slot re-anchors near the armed-lead
count before the snap-window fix and near the beat budget after). The
healthy-session A/B (this table's last column re-measured, plus the full
`check_pacing_quality.py` battery) is captured automatically by
`/tmp/interp-evidence/session_watch.sh` the next time a real display session
exists, and this file must be updated with those numbers before release.

## Acceptance for release (ROG Ally 120 Hz VRR, plus this machine foreground)

- `[PRESENT-DISCIPLINE]`: `unavailable=0`, blockp50 in [400, 2,600] us
  (locked equilibrium), period within 2,000 ppm of the panel's real rate.
- alpha-delta: p95 <= 1.5x quantum, max <= 2.5x quantum, stalls and
  slotanchors within the beat budget (`check_slot_quality`).
- displayed-interval p95 <= 1.2x quantum; zero endpoint admission skips;
  replay shed < 5% with the stale-inclusive denominator.
- Visual pass at the hub spawn and driving past all three falls
  (issues #35/#36 per the 2026-08 issue batch).

## Remaining work

- Healthy-session re-measurement on this machine (automatic, see above).
- ROG Ally 120 Hz VRR qualification with the same acceptance numbers.
- Endpoint lead auto-sizing from `leadmissmeanus` telemetry; the game-pass
  cost (~5-6 ms, dominated by the real walk) also bounds how good any lead
  can be at 120 Hz — pipelining the walk is the structural fix if the Ally
  measurements demand it.
- Hub translucent water sheet ownership (RC4b) — separate investigation.
- C7 render-only residual stepping (kart bob/spin) — design follow-up.
