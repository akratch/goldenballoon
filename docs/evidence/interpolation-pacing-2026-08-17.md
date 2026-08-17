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

## DEEP DIVE VERDICT (2026-08-17, investigation paused at user request)

**The dominant remaining defect is not in this renderer.** Frame-delivery
accounting across all five instrumented play sessions: 23-55% of fully
rendered, submitted frames were REFUSED a drawable at the present boundary
(`[WGPU-BACKPRESSURE] unavailable` = 23,094/47,459; 10,196/18,474;
2,037/13,373; 6,451/21,300; 2,879/12,528) and silently shown as repeats.
Classification is proven three ways: (1) elimination on independent
counters — discipline-layer unavailable=0 (no Timeout/Outdated while
presentable), [PRESENT-MODE] rows = 1 + user resizes (no Outdated/Lost),
no 120-consecutive-timeout fatal despite bursts (only Occluded resets the
recovery counter); (2) disassembly of the vendored wgpu-native v29.0.1.1:
`Surface::acquire_texture` checks `NSWindow.occlusionState` BEFORE
`nextDrawable` and returns Occluded (0x30001) without acquiring when
NSWindowOcclusionStateVisible is clear; (3) the upstream regression family
is documented: wgpu-native #590, wgpu #9430 / #9410 / #10087 (introduced
by wgpu PR #9141 in v29) — including terminal-launched apps never
receiving the visible bit. Every instrumented session was terminal-
launched. The app is occlusion-blind (sdl2-compat has no occlusion flag)
and treats Occluded as benign-transient, so each storm frame silently
drops a rendered image: dead-center whole-frame strobing, worst while
eye-tracking a pan, invisible to offscreen dumps, and untouched by all
seven renderer-layer fixes (each of which was real and remains landed).

Resume protocol (info per minute; record unavailable= for each):
1. Launch via Finder/.app instead of terminal, same hub pan (~2 min).
   unavailable~0 + clean falls => confirmed; fix = patch/pin wgpu-native
   occlusion gate + app-side occlusion cross-check + status-split
   telemetry in wgpu_report_backpressure.
2. Accessibility "Dim Flashing Lights" check (30 s).
3. MDKR_RENDERER=gl A/B (2 min) — clean GL indicts the WebGPU present.
4. Fixed 60 Hz display mode A/B (2 min) — VRR amplifier check.
5. External non-miniLED display (2 min) — panel-layer check.
6. F9 bracket while flashing; dumps clean + eyes flashing locks in the
   present-layer classification.

Secondary (real, lower priority, from the authored-walk audit): the void
curtain still has three port-amplified caps that can drop waterfall
content on authored frames under the widened lens — gVoidPrimLimit=45
saturation (the 44-plane sorter fix RAISED demand into this ceiling),
D_8011D47C is s8 (index truncation above 128 entries => garbage quads /
slivers), and the whole-curtain bail at 175 entries. Instrument
per-tick {D_8011D49E, gVoidPrimCount saturation, sorter-abort hits} in
one pan bracket, then raise limits in lockstep (prim limit ~180, s16
indices, clamp-not-bail at 175).

## FOURTH WAVE (2026-08-17, session6 forensics): shim the bit, trust the dumps

Session6 (one hour live, defense from merge c83b24c active, F9 bracket
frames 442751-443398 while the falls flashed) settled three questions at
once.

**1. The pump-and-retry defense is necessary but not sufficient.** The
ledger read submitted=440553 presented=436629 unavailable=3924
occludedvisible=3924 occlretries=15699 occlrecovered=3: refusals fell from
23-55% of frames to 0.89%, but every surviving refusal exhausted all four
retries — the stale occlusionState bit persists far longer than the 1.2 ms
an in-frame retry can afford, and only 3 of 3,924 events recovered. A
notification-race defense cannot close a multi-millisecond stale window.
Fix landed: an in-process method swizzle on -[NSWindow occlusionState]
(platform_macos_install_occlusion_shim, platform_sdl_min.c) that ORs
NSWindowOcclusionStateVisible into every answer the present library reads.
Every window in this process is a game surface; "always presentable" is
the correct policy, and MDKR_NO_OCCLUSION_SHIM=1 restores the stock getter
for A/B. Smoke proof: 1793/1793 presents, unavailable=0, occlretries=0 —
the refusal class is gone at the source, not raced.

**2. The F9 capture rig was polluting its own brackets.** [PRESENTSCHED]
presents-per-tick was 4.00 for 110,700 straight ticks and dropped to ~2.15
exactly at CAPTURE-START, recovering to 3.88 at CAPTURE-STOP: the
synchronous readback+PPM write halved the present rate (120 -> ~64 fps)
for exactly the window being captured. Worse, the rate deficit exposed a
real slot-projector gap: the first replay of a tick was PINNED to one
quantum, so a ~2-present tick drew endpoint/quarter pairs (alpha 0.25)
where the pose pair had really advanced half a tick — the bracket's
E,1/4,E,1/4 cadence was the rig plus this pin, not the live defect. Both
fixed: the PPM write moved to a bounded async writer thread (lossless
blocking policy for --dump-frames fixtures, drop-and-count for F9; 119.5
fps sustained WITH every-frame dumps in the smoke run), and the slot
projector's first-replay branch now measures elapsed phase since the
previous tick's last replay (still increment-based/offset-invariant) and
anchors to the quantized measured phase when more than two healthy slots
elapsed (mdkr_present_slot_phase, unit-tested both directions). [CAPTURE]
rows now carry dt_us so a polluted bracket can never masquerade again.

**3. The rendered content stream is PROVEN clean.** A single-frame
anomaly scan over all 647 bracket PPMs (frame N vs both neighbors vs
neighbor-skip diff) found ZERO content flashes — no vanishing falls, no
one-frame breaks, at any threshold. The void-curtain capacity fix held.
Everything upstream of the present boundary is now measured healthy:
4.00 presents/tick, alpha p50=p95 ideal, content stream anomaly-free.
Whatever remained on the glass was injected at or after present — which
is exactly where the 3,924 unrescued refusals live.

Also landed for localization-forever: rate-limited [WGPU-REFUSAL] rows
(first 64 verbatim then every 64th, with frame + status + presentable bit
+ t_ns) and a [WGPU-BACKPRESSURE-PERIODIC] cumulative ledger every 4096
submissions, distinct tag so expect_one parsers never collide. A latent
link break (g_frameCounter in the capture-pose probe vs the standalone
presentation-snapshot unit test) surfaced during this rebuild and was
fixed in the test harness — the prior battery had exercised a stale
binary.

Verification: 194/194 units, check_pacing_quality PASS (mode=slot,
variance 122 ppm, transitions 0), check_arbitrary_presentation_rates PASS
(byte identity), check_weather_presentation_identity PASS,
check_gpu_backpressure PASS, live smoke unavailable=0.

## FIFTH WAVE (2026-08-17, four-agent independent review): the amplifier was ours

Per the owner's direction, four independent Fable agents reviewed the
accumulated evidence — an agnostic pixel-level diagnostician, a 1 Hz code
tracer, a WebGPU/Metal specialist, and an adversarial reviewer of the whole
chain. They converged:

**Unified percept model.** The player has been reporting ONE perceptual
signature — a ~1/second hitch of the high-contrast falls during smooth-
pursuit panning — produced by DIFFERENT generators across eras: the
occlusion refusal storm (sessions 1-6, killed by the shim), then session7's
measured 999.90 +/- 0.06 ms metronomic acquire stall. A doubled frame
during pursuit displaces the image ~2x the pan step on the retina; the
falls (texture stddev 66, brightest object, ~30%/frame scroll
decorrelation) are the worst-case stimulus in the scene. Static camera =
no pursuit = invisible. Frame dumps are clean BY CONSTRUCTION — it is a
timeline defect. This is why every content-side fix "changed nothing".

**The amplifier: WGPU_SURFACE_MAX_FRAME_LATENCY 1 == a two-drawable Metal
pool.** wgpu-hal v29 maps latency N to CAMetalLayer.maximumDrawableCount
N+1 with allowsNextDrawableTimeout disabled; at 1 that is the legal
minimum: one drawable on glass (held longer by WindowServer for a
composited window), one in flight, ZERO slack — any ~1 ms system beat
costs a full +8.3 ms inside a blocking nextDrawable. The libultraship
precedent cited for the pin does not hold on Metal (its macOS backend
runs a default THREE-deep pool via SDL_Renderer; gfx_metal.cpp:698).
FIXED: latency 2 in gfx_webgpu.c AND app_host.cpp (the depth is a
property of the configuration and must ride every configure);
check_app_adopted_pacing expectations updated with the measured
rationale.

**Session7's generator was probably the measurement rig itself.** The
session was launched with --dump-frames and no MDKR_DUMP_FROM filter:
every present of the whole session was synchronously read back and
written (53,215 PPMs, ~196 GB at ~440 MB/s) — a constant-rate dirty-page
torrent whose kernel flush cadence is metronomic. Launch protocol fixed:
user sessions get MDKR_DUMP_FROM=999999999 so dumps exist ONLY inside F9
brackets. The no-dump control run is the decisive next measurement.

**Third occlusion-storm site found and fixed.** check_app_adopted_pacing
was failing 100%-refused ("active display session required" — a masking
message): the LAUNCHER presents through wgpu before engine init, so the
shim never installed for adopted-window/probe-.app paths. The shim is now
installed at common SDL init, at launcher init
(platform_present_occlusion_shim_install), and in the engine window
branch (idempotent). Gate now PASSES.

**Exonerated by source audit:** SDL3/sdl2-compat pump (nothing ~1000 ms
gated; session7 provably had no controller open), wgpu-native timers
(binary links no CoreVideo, no timer machinery), mdkr64's own loops (no
1 Hz logic), EDR (surface is SDR BGRA8Unorm), aliasing-as-primary (static
would shimmer too — though see below).

**Remaining ranked follow-ups if any flash survives the pool fix + clean
launch:** (1) stall-recovery re-spacing — after a >1-slot acquire gap the
policy currently DROPS the 3/4-alpha present (alpha runs 1.0->0.25->0.5->
1.0; p99 alpha-delta 503808 ppm, ~530/session), turning each stall into a
1/2x->2x velocity wobble; re-space or slew instead. (2) Falls-material
mip/aniso: measured 2.5x pan-vs-static motion-compensated temporal
residual on the falls (6.5 vs 2.7; rock 1.2) — the continuous
scintillation component; enable the gated g_pcMipmaps/aniso path for
track materials and re-measure. (3) CADisplayLink timestamp rate
reference for the PLL (slew was ~10 ms/s against the ~119.83 Hz panel;
vblank-derived timestamps respect the no-compositor-noise doctrine).
(4) Panel-side discriminators: external display / 240 fps slow-mo.
Analysis artifacts: /tmp/interp-evidence/f9/{png,*.py}.

## SIXTH WAVE (2026-08-17, AFK directive): the falls defect itself — an
## uninterpolated, camera-anchored void curtain

With delivery proven perfect (session8: 90,111/90,112 presents, zero
refusals, ~119.25 Hz) and the player still reporting zero change, the
cadence class was falsified as the percept for the third and final time.
A new always-on census (voidbatches/voidinterp/voidheld in
[PRESENT-PACKET], batches recognized by DL-word ranges registered at the
void arena alloc) then measured the true defect directly:

**voidinterp = 0 of voidbatches = 1,745 across a full hub tour.** The
void curtain — the waterfall backdrop strips — had NEVER been
interpolated. It is rebuilt every authored tick on a line anchored to
that tick's camera (250 units ahead, perpendicular to yaw;
tracks.c D_8011D4A0..B0), so it TELEPORTS in world space at 30 Hz while
the interpolated camera glides at 120 Hz around it. Static camera: no
teleport, clean. Panning: the strips shear/snap against the falls every
tick — the exact "waterfalls flash/break while panning" report, invisible
to every present/pacing/content instrument previously deployed (dumps
contain each authored frame correctly; the defect is BETWEEN frames).

Diagnosis chain, each step measured:
1. ownervalid=0 on 100% of curtain batches (VOID-DIAG rows): the curtain
   never had a deformation owner — it draws in the track DL with no
   matrix binding, so no pairing was ever attempted.
2. Fix rail: the WEATHER pattern (weather.c weather_register_vertex_batch)
   — renderer-owned per-batch identity registration, exactly the rail
   built for double-buffered renderer-lifetime content (rain's comment
   even names the flip problem). Curtain flushes changed to one batch per
   QUAD (NATIVE_PORT, display-list-only), each registered under a stable
   identity with a 900-unit pair guard.
3. Iteration 1 (identity = per-viewport quad INDEX): 82% blend on the
   gentle 44-waypoint tour, but the sort-order index churns exactly
   during pans — a falls-circling route measured 25%, then 17% with a
   plane-pair key (adjacency churns with the height sort too).
4. The categorical killer found via deformcollision=1,174: one plane
   opens a quad in EVERY band its span crosses (func_80026E54 runs per
   band), so any per-plane key collides with itself within a tick and
   the packet layer's collision-poison defense refused pairs for exactly
   the widest, most visible sheets.
5. Final identity: world-stable PLANE key (hash of the sliced world
   triangle segment+face, or segment bounding box, carried through the
   insertion sort in a NATIVE_PORT-only struct field, set at push via
   sVoidPendingKey) mixed with the plane's per-tick QUAD ORDINAL
   (open-addressed per-walk counter map). Same-tick keys are unique by
   construction; band churn degrades to same-sheet adjacent-segment
   morphs bounded by the guard.

**Result on the harshest falls-circling route: voidinterp = 2,217 of
2,697 (82%, was 0%), deformcollision 1,174 -> 20, jump-holds 81 (guard
correctly refusing genuine content swaps).** Residual holds are strips
entering at the view boundary (first tick has no pair — correct) and
band-count churn (renders as one authored step, the retail behavior).

Gates: 194/194 units, byte identity
(check_arbitrary_presentation_rates), weather identity, pacing quality,
adventure hub — all PASS. The identity plumbing is presentation-only:
no simulation state is read or written by any of it.

Diagnostics kept: [VOID-RANGE] rows at level alloc, voidbatches/
voidinterp/voidheld in [PRESENT-PACKET], MDKR_VOID_DIAG=1 for per-batch
rows ([VOID-DIAG] at the walk tail, [VOID-PART] verdict/result in the
particle replay arm, [JUMP-HOLD] with per-axis deltas).
MDKR_TEST_PARTICLE_INTERPOLATION=off A/Bs the curtain (and weather)
blending for attribution.
