# Timber's Island waterfall interpolation, 2026-08-16

> **SUPERSEDED (2026-08-17).** The "Root cause" and "Fix" sections below are
> incomplete: the open-loop display-rate deadline and the second in-flight
> admission slot introduced new defects (character jitter from wake-clock
> alpha on a drifting open-loop grid; a latent endpoint-starvation hazard)
> and did not fix the shimmer. See
> [`interpolation-pacing-2026-08-17.md`](interpolation-pacing-2026-08-17.md)
> for the full mechanism and the closed-loop replacement. The content
> isolation in this file (texscroll registry does real work; mipmap and
> anisotropy hypotheses rejected; the residual water-sheet and fractional-
> phase items) remains valid.

## Reported symptom

On the Windows release build, enabling motion smoothing on a 120 Hz VRR ROG
Ally made the Timber's Island waterfalls shimmer or flicker. A ProMotion
MacBook could reproduce the backend cadence defect once the test used a real
foreground WebGPU surface rather than synthetic GL pacing.

## Content isolation

`tests/input_scripts/adventure_hub_drive.txt` is required to reach level 0.
`MDKR_LOAD_TRACK=0` is not an override, so earlier level-retarget captures that
used it were Ancient Lake and are not evidence about the hub.

At the stationary hub spawn, 120 consecutive GL captures at a 120 Hz
presentation rate showed even advancement across the four interpolation phases:

| Waterfall crop | Largest / smallest phase step |
|---|---:|
| left | 1.019 |
| centre | 1.015 |
| right | 1.028 |

Turning authored-rate UV interpolation off produced a 37.9% endpoint excess on
the small upper cascade in the later driving view. With it on, the excess was
2.4%. The retained `{rate, phase}` texscroll path is therefore doing real work
and the visible cascade is not missing a UV owner.

Mipmaps off/on and 16x anisotropy changed the three stationary waterfall crops
by less than 0.5% in temporal second-difference energy. The hub does not use the
per-batch texture animator for these falls. Those two hypotheses were rejected.

## Reference-port comparison

HarbourMasters ports do not infer scrolling solely from mutable vertex arrays.
Shipwright emits `Gfx_TexScrollEx` / `Gfx_TwoTexScrollEx` commands containing
the current and next tile coordinates; libultraship evaluates those endpoints
at the render frame's interpolation fraction. Shipwright also records explicit
matrix epochs and advances a render-time interpolation scalar independently of
the game update.

mdkr64's authored texscroll registry is the equivalent content contract: the
game publishes the fixed-point scroll rate and accumulator phase, then the
retained display-list replay evaluates it at rational presentation alpha. The
important difference found here was below that layer: many correctly evaluated
images never reached WebGPU admission.

Reference sources:

- Shipwright `z_rcp.c`: <https://github.com/HarbourMasters/Shipwright/blob/develop/soh/src/code/z_rcp.c>
- Shipwright render loop: <https://github.com/HarbourMasters/Shipwright/blob/develop/soh/soh/OTRGlobals.cpp>
- Shipwright matrix interpolation: <https://github.com/HarbourMasters/Shipwright/blob/develop/soh/soh/frame_interpolation.cpp>
- libultraship command evaluator: <https://github.com/kenix3/libultraship/blob/port-maintenance/src/fast/interpreter.cpp>

## Root cause

`Video.FrameLimit=display` assumed the native FIFO presentation call paced the
CPU at one opportunity per refresh. `wgpuSurfacePresent` queues and returns.
After every successful frame the engine immediately attempted another replay;
the one-frame replay admission boundary rejected it while the previous image
was still retiring. Only that rejection armed the display-rate shed floor. The
result was a repeated burst/hold pattern rather than a 120 Hz opportunity clock.

Released behavior on the full saved-game route, 120 Hz foreground WebGPU:

| Metric | Released |
|---|---:|
| presentation opportunities | 15,967 |
| surface updates | 7,109 (92.7 Hz) |
| replay admission skips | 8,833 / 13,646 (64.7%) |
| endpoint admission skips | 0 |
| displayed interval p50 / p95 | 9.2 / 18.4 ms |
| interpolation alpha delta p50 / p95 | 0.283 / 0.627 tick |

This is sufficient to make a continuously scrolling texture shimmer even when
each image's UV phase is mathematically correct.

## Fix

Native WebGPU `display` mode now spaces every opportunity with the existing
absolute rational deadline at the reported refresh. On a paced, non-tearing
FIFO policy, a replay may use the second in-flight admission slot instead of
reserving it unnecessarily; synthetic, uncapped and tearing paths retain the
old endpoint reservation. The observed queue-depth high-water remains one.

Fixed behavior on the same full route:

| Metric | Fixed |
|---|---:|
| presentation opportunities | 9,177 |
| surface updates | 9,154 (119.4 Hz) |
| replay admission skips | 0 |
| endpoint admission skips | 0 |
| displayed interval p50 / p95 / p99 | 8.3 / 10.55 / 11.55 ms |
| interpolation alpha delta p50 / p95 / p99 | 0.238 / 0.352 / 0.373 tick |
| queue-depth mean / max | 1 / 1 frame |

`tests/check_pacing_quality.py` now rejects a paced run that sheds more than 5%
of its interpolation replay attempts. `check_gpu_backpressure.py` still proves
the synthetic overload path reserves endpoint capacity and never blocks the
runtime thread.

## Remaining interpolation work

This closes the reported waterfall mechanism, not every presentation class.
The large translucent water sheet visible later in the hub still has about a
2x endpoint step and is not affected by the authored texscroll stage; it needs
its wave/deformation ownership traced separately. Authored fractional texscroll
phase is also applied only to interior replays today, while real endpoints keep
the authored triangle bytes. A future endpoint-unification change should render
all phases through one explicit continuous-scroll contract, with pixel tests
that deliberately use a fractional rate.

The release candidate still needs the same foreground telemetry run on the ROG
Ally. Required acceptance is zero endpoint skips, replay skips below 5%, no
alpha regressions, and a visual pass at 120 Hz VRR while stationary at the hub
spawn and while driving past all three falls.
