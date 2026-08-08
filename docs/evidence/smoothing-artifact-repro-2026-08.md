# What the 120 Hz artifact actually is — named classes and their witnesses

Date: 2026-08-08
Branch: `codex/v1.0.3-reliability-ux` (worktree `presentation-gold-standard`)
Host: Apple M3 Max, macOS arm64, AppleClang release build (`build-rel`)
ROM: US v80 Rev 1, validated by the runtime banner
Instruments: the frame-dump seam (`--dump-frames` + `MDKR_DUMP_FROM`,
`platform_dump_frame`, backend readback before the swap) driven at fixed
headless schedules, and `tests/check_smoothing_stage_bisection.py`'s ranking,
already published as `docs/evidence/smoothing-stage-attribution-2026-08-08.md`.

The owner rejected motion smoothing on a **120 Hz display** — "lots of
artifacting or other issues with the way it looked, making it preferable to
disable" — without naming a track or vehicle. At 120 Hz the subloop draws
**three interpolated presents per authored tick**, so three of every four
images a player sees are reconstructions and only one is a walk the game
actually authored.

This note names the artifact classes that reconstruction can produce on that
grid, gives each one a measured witness or says it is unwitnessed, and then
spends most of its length on the one that turned out to be a real defect: the
**shield/magnet effect stage places its shell one whole authored tick ahead of
the racer it belongs to, on every interpolated frame**. That is not a subtle
sub-pixel complaint. The shell leaves the kart, moves 32 px up-track on a
640x480 frame, and the kart's own body then occludes the half of it that should
be drawn in front — on 75% of presented frames at 120 Hz and 50% at 60 Hz.

## Method

One route for every arm: level 29 Jungle Falls, `nav_to_time_trial_race.txt`
under autopilot, 3,230 authored ticks, GL backend, a 320x240 window whose
backend dump is 640x480. `MDKR_FORCE_SHIELD` brackets the sampled window so the
effect stage is live on every frame measured. The window is authored ticks
3200–3230, after the countdown clears around tick 3120 — before it the racers
are screen-static and every model stage reads as innocent for the wrong reason.

`MDKR_FORCE_SHIELD` is expressed in **present** indices, not ticks. The first
60 Hz arm silently ran with no shield at all (`effectreg=0`) because the same
literal window fell off the end of a run that presents half as often. The arms
below use `12680:600` at 120 Hz and `6340:300` at 60 Hz, which are the same
authored ticks, and the corrected 60 Hz arm registers the same 476 effect
recipes as the 120 Hz one.

| Arm | Rate | `Video.MotionSmoothing` | Stages | Frames dumped |
|---|---|---|---|---|
| `a120-allon` | 120 | interpolate | all | 120 (ticks 3200–3230) |
| `a120-effoff` | 120 | interpolate | `MDKR_TEST_EFFECT_INTERPOLATION=off` | 120 |
| `a120-alloff` | 120 | interpolate | all seven off — camera only | 120 |
| `a120-off` | 120 | off | — | 120 |
| `a60-allon` | 60 | interpolate | all | 60 (same ticks) |
| `a60-off` | 60 | off | — | 60 |

Two supporting measurements are used throughout. **Step profile** is the mean
absolute difference between consecutive presented frames, grouped by the alpha
slot the step lands on; uniform motion makes the four steps inside a tick
equal. **Shield track** segments the additive green shell by colour
(`G > R+45 && G > B+45 && G > 110`) inside the player box — x∈[200,520],
y∈[250,460] of the 640x480 dump, which excludes the HUD band and the rival
shields on the left — and reports its coverage and centroid per present.

## 1. The artifact classes at 120 Hz

| # | Class | Mechanism | Measured witness | Disposition |
|---|---|---|---|---|
| C1 | **Effect stage leads the racer by one authored tick** | `mdkr_camera_replay_effect_world` measures the shield's base-root residual against the alpha-zero snapshot while taking the residual's own endpoint from the alpha-one recipe. The two do not cancel; what is left is a full tick of racer travel added to the shell's world position. | Shield centroid sits **32.1 / 32.7 / 33.3 px** from its own tick's authored pose at alpha 1/4, 2/4, 3/4 — flat, not a ramp — and its coverage drops from 6,960 px to 3,563 px on every interpolated frame. §2. | **OPEN — defect. Write-up in §2, no fix here.** |
| C2 | Camera blends across a cut | Post-race spectate handover, the 3P T.T. spectator cut and camera-mode reframes move the eye outright, all inside the 2,000-unit teleport threshold and inside one camera slot, so nothing in the captured pose said "cut". | Not re-measured here. `0c6cb33` / `48d79d6` / `969bd2e`: 12 blended cuts before, 0 after, pinned by `tests/check_camera_snapshot_coverage.py`'s cut classifier (`6bffbc2`). | **Addressed on this branch.** |
| C3 | Interpolated angle smears the long way round | A rotation advancing more than half a turn per tick interpolates backwards; one near a quarter turn smears visibly. | Not firing on this route. The shield's yaw is `gShieldSineTime * 0x800` and `gShieldSineTime` advances by `updateRate` (`objects.c:1084`), i.e. 11.25° per field and 22.5° per authored tick — far under the `0x4000` snap. `effectphasehold=0` and `effectmiss=0` on both rate arms. The magnet's phase rate is not established here. | **Addressed (`2dafae2`); not this route's artifact.** |
| C4 | Replay reads storage it does not own | An interpolated walk resolving a non-arena dependency with no retained copy read the live pointer, by which time task K+1 had already been authored into it — a corruption of arbitrary shape. | `uncapturedext=0`, `uncapturedrefusals=0`, `staletenants=0` on all six arms here, and on all eleven bisection arms over 114,957 interpolated replays. | **Addressed (`26526db`).** |
| C5 | Extra frames in flight at a high refresh | A swap chain deeper than one frame adds latency and lets the pacer's phase estimate drift from what the display shows. | Not measured here — every arm is GL and headless. | **Addressed for WebGPU (`e9fa33a`); unwitnessed on this route.** |
| C6 | Effect shell drifts off the camera when the stage is *off* | With the effect matrix held, the retained MVP carries the tick-T camera while the camera itself interpolates, so the shell falls back and swells. | `a120-effoff`: centroid **7.1 / 15.3 / 23.6 px** from the authored pose and coverage **7,516 / 8,225 / 9,344 px** across the alpha grid — a clean ramp. | Reachable only through the test opt-out. This is the artifact C1's stage exists to remove, and it is the contrast arm for §2. |
| C7 | Residual tick-boundary step | Content no stage covers advances only at the authored tick, so the step into the authored endpoint is larger than the steps inside it. | Camera-only arm: step into the endpoint **12.240** against an interior mean of **10.358** (+18%). Effect-off arm: **11.300** against **9.758** (+16%). At 60 Hz the two steps are **15.643 / 15.431** (1.4% apart) — the grid has no interior to be asymmetric about. | **OPEN — inherent to partial coverage; magnitude now measured.** |
| C8 | UV-scroll phase holds | Scroll batches that cannot be identity-matched hold their authored phase while the surface around them advances. | `uvscrollhold/uvscrollhit` = **10,248/70,389 at 120 Hz and 3,416/23,463 at 60 Hz — 14.56% in both**. Rate-independent. From the attribution note: 0.4038 ceiling share at ~2.5 per changed byte, i.e. broad and faint. | **OPEN — not diagnosed here.** |
| C9 | Primitive-alpha overrides that reach no pixel | 119,129 interpolated alpha substitutions changed nothing on this window. | Carried from the attribution note; unexplained there and here. | **OPEN — a question, not a witnessed artifact.** |

C1 is the only class in this table that is both a live production defect and
large enough to match an unprompted "lots of artifacting". Everything else is
either already addressed on this branch, faint, or rate-independent.

## 2. The effect stage: legitimate motion, or an artifact?

The attribution note ranked `effect` first among the genuine peer stages —
0.5547 of the ceiling by area at a mean magnitude of ~44 per changed byte — and
read that as "a hard displacement of a small region, which is what a
two-lifetime recipe reconstruction around each racer should look like". The
area and magnitude are indeed what a shield shell displacement looks like. The
question that ranking could not answer is whether the displacement is the
*right* one.

### 2.1 The change is coherent, so it is not flicker

Differencing `a120-allon` against `a120-effoff` over the 90 intermediates:

- **Endpoints are exact.** All 30 authored frames are byte-identical between
  the arms. The stage never escapes the replay.
- **One blob, not scatter.** The largest connected component of changed pixels
  holds **81–84%** of them, and components of 8 px or more hold **99.9%**.
- **The blob persists.** Intersection-over-union of the changed mask between
  consecutive intermediates is **0.85–0.88**.
- **It is a displacement, not a brightness shift.** Sign purity —
  `|Σδ| / Σ|δ|` over the changed bytes — is **0.001–0.074**, i.e. the positive
  and negative deltas very nearly cancel, which is what a bright object moving
  against a background produces and what a fade or a flicker does not.

So the stage is not producing scattered flicker. It is moving one coherent
object. That was the question the brief asked, and the answer is that the shape
of the change is innocent.

### 2.2 The change is coherent and still wrong

The structure metrics above are computed against the *other arm*, which is why
they cannot see the defect: both arms put the shell somewhere, and a wrong
somewhere is just as coherent as a right one. Tracking the shell itself against
**its own tick's authored pose** is what separates them.

Thirty ticks, `a120-allon`:

| Alpha | Shell coverage (px) | Centroid (x, y) | Distance from the same tick's authored pose |
|---|---|---|---|
| 0 (authored) | 6,959.6 | (133.80, 70.76) | 0 |
| 1/4 | 3,563.1 | (150.44, 45.11) | **32.08 px** ± 5.65 |
| 2/4 | 3,553.9 | (151.11, 45.02) | **32.71 px** ± 5.86 |
| 3/4 | 3,562.5 | (151.70, 44.94) | **33.28 px** ± 6.04 |

The same measurement on `a120-effoff`, where the effect matrix is held:

| Alpha | Shell coverage (px) | Centroid (x, y) | Distance from the authored pose |
|---|---|---|---|
| 1/4 | 7,516.0 | (129.93, 76.40) | 7.12 px ± 1.35 |
| 2/4 | 8,224.6 | (125.49, 82.89) | 15.26 px ± 2.70 |
| 3/4 | 9,343.9 | (121.02, 89.63) | 23.64 px ± 3.92 |

**Read the two tables against each other.** The held arm ramps — 7.1, 15.3,
23.6 — in the ratio 1 : 2.14 : 3.32, which is what a quantity that drifts
across a tick does. The production arm is **flat**: 32.08, 32.71, 33.28, a 3.7%
spread across a fourfold change in alpha, and it is already at full magnitude
at the first interpolated frame.

An interpolated quantity cannot be flat in alpha. A constant offset can. The
production shell is not being interpolated to the wrong place — it is being
placed at a fixed displacement from where it belongs, and the interpolation
riding on top of that offset contributes the 1.2 px of drift between alpha 1/4
and alpha 3/4.

Coverage says the same thing twice over. The shell holds 6,960 px when the game
draws it and **3,563 / 3,554 / 3,563 px** when the replay does — flat again,
and **48.8% smaller**. The loss is not scale: it is occlusion. The offset is
directed up-track, so the shell is pushed away from the chase camera and behind
the kart's own depth, and the kart then hides the half of the shell that should
be drawn in front of it. Frame-by-frame inspection of ticks 3200–3201 shows
exactly that — at the authored frame the green shell washes over the kart body,
and at every interpolated frame the kart is drawn clean and unoccluded with the
shell as a halo behind and above it.

### 2.3 The mechanism

`mdkr_camera_replay_object_world` carries the rule in a comment
(`game/src/camera.c:345-350`):

> The retained display list was authored from the PREVIOUS snapshot: DKR
> submits one list while it builds the next. The residual must therefore be
> measured against alpha zero, not the newer snapshot. Measuring it against
> alpha one extrapolates every moving object backward at alpha zero instead of
> reproducing the retained list's authored endpoint.

`mdkr_camera_replay_object_transform` (`camera.c:419`) implements it. It
resolves the owner's pose twice — once at **numerator 0** as `authored`, once
at the replay alpha as `target` — and carries the residual across:

```c
out->x_position =
    target.position[0] + (owner->source_position[0] - authored.position[0]);
```

The residual cancels only when `owner->source_position` is the transform
captured at the **same** tick that `authored` resolves to. For the object and
child classes it is: the owner comes out of the retained display list, which
was authored at tick T, and `authored` is the alpha-zero snapshot, which is
tick T.

The effect path breaks that pairing. `dkr_replay_effect_world`
(`platform/fast3d/gfx_pc_dkr.c:5105`) looks the two-lifetime recipe up as a
`{T, T+1}` pair — `presentation_snapshot_replay_target_tick` returns
`current->authored_tick`, which the same function proves is
`authored_task_tick + 1`, so `retained.previous_bytes` is the tick-T recipe
(the one embedded in the list being replayed) and `retained.current_bytes` is
tick T+1. `mdkr_camera_replay_effect_world` then lerps the shell's own local
recipe across that pair correctly, and hands the **wrong endpoint** to the base
transform (`game/src/camera.c:539`):

```c
    !mdkr_camera_replay_object_transform(
        current, numerator, denominator, &base)) {
```

`current` is the tick-T+1 capture, so `owner->source_position` is the racer
root at **T+1** while `authored` inside the helper is still resolved at
numerator 0, i.e. **T**. The residual no longer cancels; it evaluates to

```
base(alpha) = pose(alpha) + (pose(T+1) - pose(T))
```

— the interpolated racer pose plus one entire tick of racer travel. At alpha
just above zero the shell sits where the racer will be at T+1 while the kart is
still drawn at T. The offset is the same at every alpha, which is precisely the
flatness the pixels report, and it scales with speed, which is why it is
invisible while the racers are stationary and obvious under power.

Nothing else in the reconstruction is wrong. The local recipe is faithful:
`mdkr_camera_effect_world_from_transforms` (`camera.c:466`) reproduces
`mtx_shear_push`'s expression order term for term, including the detail that
`mtx_shear_push` folds the object scale into the shear (`camera.c:2702`,
`shear *= arg2_scale`) *before* `effectOwner.effect_shear` is captured from it
(`camera.c:2786`), so the captured shear and the captured scale stay consistent
with the rows they each multiply. The shell's own yaw, shear, scale and local
offset all interpolate through the correct `{T, T+1}` endpoints. It is only the
racer root the shell is anchored to that is off by a tick.

### 2.4 Why every existing gate is green

`check_presentation_matrix.py`'s effect control asserts that the stage's output
*changes intermediate frames* — 30/30 on its route — and
`check_smoothing_stage_bisection.py` asserts that it changes them *by more than
the other stages*. Both are true of a stage that puts the shell a tick ahead.
Neither arm compares the reconstructed pose against the pose the object should
have had, and the authoritative hash streams cannot see it because the defect
is presentation-only by construction. The `effecthit == effectoverride == 708`
identity says every lookup succeeded and every one substituted; there is no
refusal, no phase hold and no collision to notice
(`effectphasehold=0`, `effectmiss=0`, `effectcollision=0`).

### 2.5 Proposed fix, for the next task

Pass `previous` rather than `current` to the base transform at
`game/src/camera.c:539`. `previous` is the tick-T capture — the same lifetime
as the retained display list being walked and as the alpha-zero snapshot
`mdkr_camera_replay_object_transform` measures its residual against — so the
residual cancels and `base` becomes the interpolated racer pose with no
constant term. The guards above the call already prove `previous` and `current`
share address, generation, secondary address and secondary generation, and
`mdkr_camera_replay_object_transform` re-checks `owner->valid`, so the swap
needs no new validation.

The accompanying gate should assert the property this note measured rather than
the one the current controls measure: **the effect stage's contribution must
ramp with alpha.** A leave-one-out difference that is flat across the alpha grid
is a constant displacement whatever its magnitude, and that is a cheap,
content-independent invariant — the tables in §2.2 separate 1 : 2.14 : 3.32
from 1 : 1.02 : 1.04 without knowing anything about shields.

## 3. The honest 60-versus-120 Hz comparison

The reason the owner is on a 120 Hz display matters, and the first thing to say
is what the scheduler numbers do **not** show.

| Arm | presents | interp | real endpoints | stale | elided | catchup | skips | blocked |
|---|---|---|---|---|---|---|---|---|
| `a120-allon` | 12,920 | 9,657 | 3,227 | **36** | 0 | 0 | 0 | 0 |
| `a60-allon` | 6,460 | 3,219 | 3,227 | **14** | 0 | 0 | 0 | 0 |
| `a120-off` | 12,920 | 0 | 3,227 | 9,693 | 0 | 0 | 0 | 0 |
| `a60-off` | 6,460 | 0 | 3,227 | 3,233 | 0 | 0 | 0 | 0 |

Stale holds are **0.37% of interpolated presents at 120 Hz and 0.44% at 60 Hz**
— slightly *lower* at the higher rate, and negligible at both. Nothing is
elided, no catch-up debt accrues, no advance blocks, and the interpolated count
is exactly three per tick at 120 Hz and one per tick at 60 Hz as the grid
requires. The per-stage refusal rates are rate-independent too: `uvscrollhold`
is 14.56% of lookups at both rates to four figures, and the effect stage refuses
nothing at either.

**So "worse at 120 Hz" is not a scheduling story. It is an exposure story.**

The defect in §2 costs one wrong image per authored tick regardless of rate.
What changes is how long that wrong image is on screen:

| | Correct (authored) presents | Wrong (reconstructed) presents | Duty cycle of the artifact |
|---|---|---|---|
| 60 Hz | 3,227 | 3,219 | **50%** |
| 120 Hz | 3,227 | 9,657 | **75%** |

The per-frame magnitude is identical — `a60-allon` puts the shell **32.71 px**
from the authored pose at coverage **3,553.9 px**, matching the 120 Hz alpha-2/4
row to three figures, because it is the same alpha and the same constant offset.
At 60 Hz the shell alternates correct/wrong every frame; at 120 Hz it shows one
correct frame and then holds the wrong pose for three. Both are a 30 Hz strobe,
but at 120 Hz the wrong pose owns three quarters of the light the display emits
instead of half, and the correct pose is a single-frame flash rather than an
equal partner.

The step profile shows the same thing from the other side. Mean absolute
difference between consecutive presents, grouped by the alpha slot the step
lands on:

| Arm | → alpha 0 | → 1/4 | → 2/4 | → 3/4 |
|---|---|---|---|---|
| `a120-allon` | 10.810 | 10.509 | **8.658** | **8.626** |
| `a120-effoff` | 11.300 | 9.586 | 9.766 | 9.921 |
| `a120-alloff` | 12.240 | 10.102 | 10.368 | 10.605 |
| `a120-off` | 20.642 | 0 | 0 | 0 |

| Arm | → alpha 0 | → 1/2 |
|---|---|---|
| `a60-allon` | 15.643 | 15.431 |
| `a60-off` | 20.642 | 0 |

Production at 120 Hz is the only row with a **two-big-two-small** shape: the
step that jumps the shell out of place and the step that snaps it back are both
large, and the two steps in between — during which the shell is nearly static —
are 12% smaller than the arm with the effect stage disabled. That signature has
no 60 Hz counterpart at all, because a two-point grid has no interior to be
asymmetric about, and it is why the same defect reads as a texture of wrongness
at 120 Hz and as a simple alternation at 60.

The smoothing-off rows are the honest baseline for what the owner reverted to:
at 120 Hz, `a120-off` repeats each authored image three times, byte for byte —
9,693 stale presents, zero motion on three of every four frames. That is a 30 Hz
image cadence on a 120 Hz panel. It is not smooth, and the owner still preferred
it, which is the size of the problem C1 represents.

## Explicitly open

- **One route, one vehicle, one window.** Jungle Falls, autopilot, authored
  ticks 3200–3230. The brief also named Greenwood Village boost sections;
  they are not measured here. C1's mechanism is content-independent and scales
  with racer speed, so it should reproduce anywhere a shield or magnet is up
  and the racer is moving, but this note does not assert that.
- **The magnet path is unmeasured.** It shares `mtx_shear_push`, the same
  recipe capture and the same `mdkr_camera_replay_effect_world`, so the same
  defect should apply, but no magnet arm was run.
- **Headless and GL only.** Every arm here runs a deterministic fixed-ticket
  schedule with no display attached. Nothing in this note can reproduce or
  exclude a real-display pacing artifact — tearing, phase drift against a
  physical 120 Hz vsync, or the alpha-grid quantization
  `present_sched_alpha_projected` applies when a genuine refresh quantum is
  known. The owner's complaint may contain such a component, and this note
  neither confirms nor rules it out.
- **C1's magnitude is measured in screen pixels on this route, not in world
  units.** The stated mechanism predicts the offset equals one tick of racer
  travel; the pixel measurement is consistent with that and does not
  independently confirm the world-space magnitude.
- **C5, C8 and C9 remain unwitnessed or undiagnosed here**, as marked in §1.
