# What the 120 Hz artifact actually is — named classes and their witnesses

Date: 2026-08-08
Branch: `worktree-presentation-gold-standard`
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
sub-pixel complaint. The shell leaves the kart and moves **51 px** up-track on a
640x480 frame — against the 5.25 px the shell travels in a whole authored tick —
and the kart's own body then occludes the half of it that should be drawn in
front, on 75% of presented frames at 120 Hz and 50% at 60 Hz.

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
equal.

**Shell track** segments the additive green shell by colour
(`G > R+45 && G > B+45 && G > 110`), restricts the mask to the window
y∈[150,480), x∈[120,600) — whose top edge clears the HUD band and the rival
shells — takes the **largest connected component**, and reports that
component's area, centroid and bounding box. **All coordinates in this note are
full-frame pixels of the 640x480 dump, origin top-left**, not window-relative.

The window is a measurement hazard, because the defect under investigation
moves the shell, and a shell that leaves the crop would produce a smaller area
and a biased centroid that could be mistaken for the defect's own signature. So
containment is asserted rather than assumed: the shell component's bounding box
is checked against all four window edges on every frame of every arm, and
**touches none of them on any of the 300 frames measured**. An earlier draft of
this note used a tighter window (y∈[250,460), x∈[200,520)) whose top edge the
displaced shell did cross; every figure in §2.2 is from the containing window,
and the tight-window figures it replaces were biased low. The script and the
full per-frame rows are in the appendix.

## 1. The artifact classes at 120 Hz

| # | Class | Mechanism | Measured witness | Disposition |
|---|---|---|---|---|
| C1 | **Effect stage leads the racer by one authored tick** | `mdkr_camera_replay_effect_world` measures the shield's base-root residual against the alpha-zero snapshot while taking the residual's own endpoint from the alpha-one recipe. The two do not cancel; what is left is a full tick of racer travel added to the shell's world position. | Shell centroid sits **50.8 / 51.1 / 50.8 px** from its own tick's authored pose at alpha 1/4, 2/4, 3/4 — flat, not a ramp — against an endpoint-to-endpoint envelope of 5.25 px mean / 18.42 px max, and its area drops from 6,767 px to ~2,340 px on every interpolated frame. §2. | **CLOSED 2026-08-08 — see §4.** Write-up in §2. |
| C2 | Camera blends across a cut | Post-race spectate handover, the 3P T.T. spectator cut and camera-mode reframes move the eye outright, all inside the 2,000-unit teleport threshold and inside one camera slot, so nothing in the captured pose said "cut". | Not re-measured here. `0c6cb33` / `48d79d6` / `969bd2e`: 12 blended cuts before, 0 after, pinned by `tests/check_camera_snapshot_coverage.py`'s cut classifier (`6bffbc2`). | **Addressed on this branch.** |
| C3 | Interpolated angle smears the long way round | A rotation advancing more than half a turn per tick interpolates backwards; one near a quarter turn smears visibly. | Not firing on this route. The shield's yaw is `gShieldSineTime * 0x800` and `gShieldSineTime` advances by `updateRate` (`objects.c:1084`), i.e. 11.25° per field and 22.5° per authored tick — far under the `0x4000` snap. `effectphasehold=0` and `effectmiss=0` on both rate arms. The magnet's phase rate is not established here. | **Addressed (`2dafae2`); not this route's artifact.** |
| C4 | Replay reads storage it does not own | An interpolated walk resolving a non-arena dependency with no retained copy read the live pointer, by which time task K+1 had already been authored into it — a corruption of arbitrary shape. | `uncapturedext=0`, `uncapturedrefusals=0`, `staletenants=0` on all six arms here, and on all eleven bisection arms over 114,957 interpolated replays. | **Addressed (`26526db`).** |
| C5 | Extra frames in flight at a high refresh | A swap chain deeper than one frame adds latency and lets the pacer's phase estimate drift from what the display shows. | Not measured here — every arm is GL and headless. | **Addressed for WebGPU (`e9fa33a`); unwitnessed on this route.** |
| C6 | Effect shell drifts off the camera when the stage is *off* | With the effect matrix held, the retained MVP carries the tick-T camera while the camera itself interpolates, so the shell falls back and swells. | `a120-effoff`: centroid **7.3 / 16.2 / 24.5 px** from the authored pose and area **7,193 / 8,093 / 9,319 px** across the alpha grid — a clean ramp, and one that still breaks the 5.25 px envelope. | Reachable only through the test opt-out. This is the artifact C1's stage exists to remove, and it is the contrast arm for §2. |
| C7 | **Residual tick-boundary step** | Content no stage covers advances only at the authored tick, so the step into the authored endpoint is larger than the steps inside it — and until 2026-08-08 C1's displaced shell was snapping back on that same step. | **Shipped 1.0.3 production** (`a120-allon`, §3): **10.810** into the endpoint against an interior mean of **9.264** — **+16.68%**. After §4's anchoring fix: **9.187** against **8.856**, **+3.73%** — a factor of 4.5. What remains is significant only in the HUD (+10.00%, t=12.58, 29/29 ticks); the other 81% of the frame reads +1.49% with a 95% CI of −0.30%..+3.29%. §5.1. | **CLOSED — mostly C1, now benign; authored 2D HUD is the measured remainder.** |
| C8 | **UV-scroll phase holds** | Scroll batches that cannot be identity-matched hold their authored phase while the surface around them advances. | `uvscrollhold/uvscrollhit` = **10,248/70,389 at both rates — 14.56%**, because confirm-or-hold is decided once per tick and repeated at every alpha, so the ratio is rate-invariant by construction. Split by clause: **6,702 no {T-1} record, 3,546 oscillating displacement, 0 ambiguous, 0 shape**. Route A holds only before the race (in-race: 90 lookups, 90 confirms); route B holds **through gameplay at 12.76%** of its sampled window. §5.2. | **CLOSED — benign; the fail-closed refusal the wrap rule exists to make. One named improvement left open (per-axis confirmation).** |
| C9 | **Primitive-alpha overrides that reach no pixel** | 119,129 interpolated alpha substitutions changed nothing on this window. | The two numbers describe different presents: **48** of the 119,129 overrides fall inside the 30-tick pixel window (mean substitution 3.75 of 255, no particle, no projected shadow), and the other 119,081 are in scenes it never samples. Route B fires 178,038 and is pixel-gated. Stage cost under **7.0 us per replay** of 546 us. §5.3. | **CLOSED — benign; the zero was the sampling window, not the stage.** |

C1 is the only class in this table that is both a live production defect and
large enough to match an unprompted "lots of artifacting". Everything else is
either already addressed on this branch, faint, or rate-independent. With C7,
C8 and C9 dispositioned in §5, C5 — a real display's pacing — is the only row
this note leaves unwitnessed.

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

| Alpha | Shell area (px) | Centroid (x, y) | Distance from the same tick's authored pose |
|---|---|---|---|
| 0 (authored) | 6,766.8 ± 1,304.9 | (333.76, 319.64) | 0 |
| 1/4 | 2,358.9 ± 377.4 | (368.08, 292.87) | **50.79 px** ± 3.35 |
| 2/4 | 2,366.4 ± 376.5 | (368.29, 292.75) | **51.08 px** ± 3.47 |
| 3/4 | 2,295.2 ± 441.1 | (373.70, 293.02) | **50.82 px** ± 3.60 |

The same measurement on `a120-effoff`, where the effect matrix is held:

| Alpha | Shell area (px) | Centroid (x, y) | Distance from the authored pose |
|---|---|---|---|
| 1/4 | 7,193.3 ± 1,586.3 | (329.87, 324.59) | 7.32 px ± 1.92 |
| 2/4 | 8,092.6 ± 1,208.7 | (325.49, 332.89) | 16.19 px ± 4.68 |
| 3/4 | 9,318.7 ± 1,018.7 | (321.02, 339.63) | 24.53 px ± 5.52 |

And the scale everything above has to be read against — how far the shell moves
between two *authored* images, i.e. the entire budget one tick of interpolation
has to spend:

**Endpoint-to-endpoint envelope: mean 5.25 px, sd 5.16, max 18.42 px** over the
29 adjacent authored pairs.

**Read the three together.** The held arm ramps — 7.3, 16.2, 24.5 — in the
ratio 1 : 2.21 : 3.35, which is what a quantity drifting across a tick does.
The production arm is **flat**: 50.79, 51.08, 50.82, a **0.6% spread across a
fourfold change in alpha**, already at full magnitude on the first interpolated
frame. And both are far outside the 5.25 px the shell actually travels in a
whole authored tick — the production arm by a factor of **9.7 on the mean and
2.8 on the worst single tick**.

An interpolated quantity cannot be flat in alpha. A constant offset can. The
production shell is not being interpolated to the wrong place; it is placed at a
fixed displacement from where it belongs, and the interpolation riding on top of
that offset contributes the 0.3 px of variation across the whole alpha grid.

Area says the same thing twice over. The shell holds 6,767 px when the game
draws it and **2,359 / 2,366 / 2,295 px** when the replay does — flat again, and
**65.3% smaller**. Two mechanisms contribute and this measurement does not
separate them: the offset is directed up-track, so the shell both recedes from
the chase camera (smaller) and falls behind the kart's own depth (occluded).
Frame inspection of ticks 3200–3201 settles the depth question if not the split
— at the authored frame the green shell washes over the kart body, and at every
interpolated frame the kart is drawn clean and unoccluded with the shell as a
halo behind and above it. Clipping is *not* a contributor: the shell component's
bounding box touches no window edge on any of the 300 frames measured.

**A corroboration, and a coincidence that was not one.** Extrapolating the
held arm's ramp to alpha 1 — 7.32/0.25, 16.19/0.50, 24.53/0.75 give slopes of
29.3, 32.4, 32.7 — puts a shell held for a whole tick about **32.7 px** from the
authored pose. That is the screen motion of a world-static point over one tick,
so it independently confirms that "one tick of travel" is a displacement of this
order, which is what the mechanism in §2.3 predicts. It is *not* equal to the
production arm's 50.8 px and should not be presented as a match: the two are
different projections of one tick of travel — a static point receding from the
camera in one case, a point pushed forward along the track from near the camera
axis in the other. The tight-window draft of this note reported 32.1 px for the
production offset, which did coincide with the extrapolation; that agreement was
an artifact of the crop, not evidence.

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

**Position is not the only field carried.** The same helper applies the same
uncancelled residual to rotation and scale:

```c
out->rotation.y_rotation = (s16)(
    target.rotation_y + (s16)(owner->source_rotation[0] - authored.rotation_y));
...
out->scale = target.scale * (owner->source_scale / authored.scale);
```

so the shell is also *oriented* to the racer's next-tick heading. Scale does not
reach the output — `mdkr_camera_effect_world_from_transforms` consumes the base
transform's rotation and position only — but the rotation does, and on a corner
it adds a yaw error to the translation error rather than merely displacing the
shell rigidly. This note measures the combined result in screen space and does
not separate the two contributions.

**This shipped.** The path is production on `main` and has been since
`d2808f9` (Golden Balloon 1.0.1); it is present in v1.0.1, v1.0.2 and v1.0.3.
Motion smoothing is off by default, so it reaches only players who turned it on
— which is exactly the population the owner sampled.

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

### 2.6 What the accompanying gate must assert — and what it must not

An earlier draft of this section proposed "the effect stage's contribution must
ramp with alpha". **That gate is wrong in both directions and must not be
built.**

- It **passes the defective build.** The leave-one-out contribution — the
  per-frame distance between the production and effect-off shell centroids — is
  **57.02 ± 4.57, 65.50 ± 5.62, 72.98 ± 8.47 px** at alpha 1/4, 2/4, 3/4 on the
  build measured here. It ramps, monotonically, on every arm-pair. It ramps
  because the *contrast* arm ramps (§2.2), not because the production arm does,
  and a difference between the two inherits that.
- It **fails a correct build.** Under a chase camera the shell is very nearly
  camera-static: the whole authored tick moves it 5.25 px on average. A
  correctly anchored shell is therefore near-flat *and* near-zero, which is what
  a "must ramp" gate would reject. The defect reads flat for the opposite
  reason — a large constant — and no gate keyed on flatness alone can tell a
  large constant from a small correct one.

The discriminating property is displacement **from the shell's own tick's
authored pose**, which has two things a correct reconstruction must satisfy and
this build violates:

- **(a) Bounded by the interpolation envelope.** An interpolated present lies
  between its two authored endpoints, so its displacement from the tick-T pose
  can never exceed the tick-T-to-tick-T+1 displacement. Measured: envelope mean
  **5.25 px**, max **18.42 px** over 29 pairs; production displacement
  **50.79 / 51.08 / 50.82 px**. Fails by 9.7x on the mean and 2.8x against the
  single worst authored tick in the window — a margin no tolerance choice has to
  adjudicate.
- **(b) Converges to zero as alpha → 0.** The alpha-zero contract is byte-exact
  reproduction of the retained list's authored endpoint, so displacement must
  fall toward zero at the bottom of the grid. Measured: 50.79 px at alpha 1/4,
  the *smallest* of the three and statistically indistinguishable from the
  largest. This is the condition that names the defect rather than merely
  detecting it, because a constant offset is the only way to fail it while
  passing every endpoint check.

**Condition (a) alone** also rejects the effect-off arm — 24.53 px at alpha 3/4
against the same 5.25 px envelope, over the 18.42 px worst-tick bound — which is
correct and deliberate: C6 is an artifact too, and a gate that only rejected the
production defect would go green the day someone "fixed" it by disabling the
stage. Condition (b) does **not** reject that arm and is not expected to: its
displacement ramps 7.32 → 16.19 → 24.53 px and so converges toward zero at
alpha → 0 exactly as (b) requires. The two conditions catch different failures —
(a) bounds how far a reconstruction may stray, (b) pins where it must start —
and only (a) is load-bearing against a held matrix.

**The primary gate candidate is not pixels at all.** The defect is an exact
lifetime mismatch, so it has an exact witness: carry the capture tick on the
recipe endpoints and assert, on every effect override, that the endpoint handed
to `mdkr_camera_replay_object_transform` was captured at the same tick the
helper resolves `authored` at (numerator 0). With the fix that equality holds by
construction and costs one comparison and one counter in `[PRESENT-PACKET]`;
without it, it is non-zero on every single override — 708 of 708 on this route.
An exact structural assertion is worth more than any pixel threshold here, and
it generalises to the object and child classes, which obey the same rule today
but have no gate saying so. The pixel conditions (a) and (b) are the
content-level backstop that would catch a future stage getting the same pairing
wrong in a different way.

**Neither condition has been run against a fixed build.** This is a diagnosis
task and no production code was changed; (a) and (b) are stated as a design
whose pass criterion follows from what a correct reconstruction implies, and
verified only in the direction that the defective and effect-off builds both
fail them by large measured margins.

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

The per-frame magnitude is identical, and not merely to within a metric: every
one of `a60-allon`'s 60 dumped frames is **byte-identical** to the `a120-allon`
frame at the same alpha (60 of 60 matched, 0 differed). The 60 Hz arm puts the
shell **51.08 px ± 3.47** from the authored pose at area **2,366.4 px**, the
120 Hz alpha-2/4 row exactly, because it *is* that image. Changing the
presentation rate changes which alphas get sampled and nothing about what any of
them looks like.

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
- **C7's rank ordering is unresolved.** The camera-only and effect-off arms both
  show the step into the authored endpoint ~16–18% larger than the interior
  steps, but no arm attributes that residual to particular content. It is a
  measured magnitude with no named cause.
  *(Superseded 2026-08-08 — see §5.1. The production arm in the same table
  shows +16.68%, most of which was C1; after the fix production reads +3.73%
  and the significant remainder is the 2D HUD.)*
- **Neither proposed gate in §2.6 has been run against a fixed build**, because
  this is a diagnosis task and no production code was changed. They are verified
  only in the direction of rejecting the two builds measured here.
  *(Superseded 2026-08-08 — see §4.)*
- **C5, C8 and C9 remain unwitnessed or undiagnosed here**, as marked in §1.
  *(C8 and C9 superseded 2026-08-08 — see §5.2 and §5.3. C5 stands.)*

## 4. Fixed — measured 2026-08-08

C1 is closed. `mdkr_camera_replay_effect_world` now hands `previous` — the
tick-T capture — to the base transform, so the residual cancels and `base` is
the interpolated racer pose with no constant term. Both conditions of §2.6 have
now been run against a fixed build, in both directions, and the exact witness
§2.6 asked for exists and is gated.

### 4.1 The pixels

Same route, same window, same measurement, `MDKR_PRESENT_RATE=120`,
`MDKR_FORCE_SHIELD=12680:600`, 30 authored ticks and 90 interpolated presents.
The gate re-derives its own envelope from the run rather than carrying this
note's literals, which is why the envelope row differs slightly from §2.2's
(5.34 / 19.45 against 5.25 / 18.42): the standing gate takes the largest
connected component with its own tie-breaks. Both are the same quantity
measured twice, and the verdict does not turn on the difference.

| Alpha | Before — displacement | After | Before — shell area | After |
|---|---|---|---|---|
| 1/4 | **50.77 px** ± 3.57 | **2.19 px** ± 4.17 | 2,359 px | 6,784 px |
| 2/4 | **51.09 px** ± 3.69 | **3.85 px** ± 4.83 | 2,366 px | 6,792 px |
| 3/4 | **50.84 px** ± 3.72 | **4.45 px** ± 4.75 | 2,295 px | 6,811 px |

Envelope on this run: **mean 5.34 px, max 19.45 px** over 29 authored pairs.

Read the "after" column the way §2.2 asked the "before" column to be read. The
flat 50.8 px series is gone; what replaces it is a **ramp** — 2.19 → 3.85 →
4.45 — whose whole extent sits **inside the 5.34 px the shell travels in one
authored tick**. The worst single interpolated present is 18.68 px against a
19.45 px worst authored step, so condition (a) holds even before its 2 px
segmentation tolerance is applied. And the shell's area comes back from a flat
~2,340 px to ~6,795 px, against 6,767 px when the game draws it: the recession
and the kart-body occlusion that cost 65.3% of it were both consequences of the
displacement, and both are gone.

Condition (b) reports `not-discriminating` on the fixed build, which is the
outcome §2.6 predicted and asked for: a correctly anchored shell under a chase
camera is near-flat *and* near-zero, so ordering near-zero noise would decide
nothing. It fired, correctly, on the defective build — 50.77 against 50.84, a
ratio of 0.9987 where a genuine ramp reads about a third.

### 4.2 The exact witness

§2.6's primary candidate is now production. `GfxPresentationMatrixOwner` carries
the authored tick its recipe was copied out at
(`presentation_task_authoring_tick`), and every replayed recipe compares that
stamp against the tick the alpha-zero pose resolves to, counting into
`[PRESENT-PACKET]` as `ownertickcheck` / `ownertickmismatch`. It covers **all
four recipe classes** — object root, articulated child, billboard anchor and
effect — not only the one the defect was found in.

| Build | ownertickcheck | ownertickmismatch |
|---|---|---|
| Both defects present, route A | 288,873 | **717** |
| Anchoring defect only (§4.3 fixed), route A | 288,864 | **708** |
| Fixed, route A | 288,864 | **0** |
| Fixed, all 11 bisection arms | 2,404,110 | **0** |

The middle row isolates C1: **708 of 708 effect overrides**, exactly the
`effecthit == effectoverride == 708` identity §2.4 records — every single
override on the route, as predicted. The remaining 9 of the first row's 717 are
the separate instance in §4.3.

### 4.3 A second instance the witness found by itself

The other **9** were not the shield. They are `GFX_PRESENTATION_MATRIX_BILLBOARD`
recipes stamped one tick behind the pose they were measured against, at two
tick boundaries (1111→1112 and 1269→1270) during a pre-race transition.

Cause: the presentation packet's *live* registration list is emptied by the
freeze at the end of a display list's real walk. A pass that holds its frame
(`gDrawFrameTimer`) or elides its catch-up render never submits its list, so it
is never walked and never freezes — and the game then authors over the same
buffer. Without intervention the abandoned attempt's bindings survive into the
next freeze still carrying the older tick, and replay measures their residual
against a newer alpha-zero pose. Same wrong answer as C1, reached by a
different route, in a class §2.6 expected to be clean.

`presentation_task_authoring_begin` now discards live registrations when a new
authoring lifetime opens, which is a no-op on any pass that submitted. The nine
disappear, and `ownertickcheck` falls by exactly nine (288,873 → 288,864) —
those bindings are no longer replayed at all, which is correct: they describe a
list nothing ever walked.

This is the whole argument for preferring a structural witness to a pixel
threshold. Nine frames in a transition would never have been found by a shell
tracker, and no pixel gate would have been pointed at them.

### 4.4 Standing gates

- `tests/check_effect_shell_envelope.py` (new, registered in
  `tools/run_checks.py`) implements §2.6 conditions (a) and (b) on this route.
  Run against the pre-fix build it fails **90 of 90** interpolated presents;
  against the fixed build it passes with the worst present at 18.68 px inside a
  21.45 px bound. It also asserts the structural witness is non-vacuous —
  `ownertickcheck` present *and* non-zero — because a recipe that loses its
  stamp stops being counted rather than being counted as a mismatch, and a zero
  over zero would read green while asserting nothing.
- `tests/check_smoothing_stage_bisection.py` asserts `ownertickmismatch=0` over
  a non-zero `ownertickcheck` on every production arm of both routes, with the
  same presence-before-value stat contract its `uncapturedext` assertion uses,
  and prints `[TICK-ANCHORING]`.

### 4.5 What did not change

The authoritative streams. `[SIMHASH]`/`[EVENTHASH]`/`[INPUTHASH]` over the
3,230-tick route are byte-identical between the pre-fix and fixed binaries, at
`MotionSmoothing=interpolate` + `PRESENT_RATE=120` and at the original-pace
default, 9,690 rows each. `check_presentation_matrix.py` passes unchanged,
including arm C's forced-shield control (30/60 intermediate frames differ, all
alpha-zero frames reproduce the authored endpoint) — that control pinned "the
stage changes intermediate frames", which is as true of a correctly anchored
shell as of a displaced one, so it needed no re-pointing.

### 4.6 Still open after the fix

- **The magnet path is still unmeasured as such.** It shares `mtx_shear_push`,
  the same capture and the same replay function, and the tick-agreement witness
  now covers it structurally wherever it runs — but no arm in this note or in
  the gate forces a magnet rather than a shield.
- **Greenwood Village is still unrun**, as are non-GL backends and any real
  display. C1's closure does not speak to C5, C7, C8 or C9, and nothing here
  can confirm or exclude a real-display pacing component of the owner's
  complaint. *(C7, C8 and C9 are dispositioned in §5 — 2026-08-08.)*

## 5. C7, C8 and C9 — dispositions, measured 2026-08-08

All three are **benign** — but C7's headline number was real production
behaviour, and the reason it is benign now is that §4's anchoring fix removed
most of it. Every figure below is from the fixed build (`6ac92f7` plus the
census commits this section documents), same route, same window, same 640x480
GL dumps, with the pre-fix comparisons taken from §3's own table.

### 5.1 C7 — the tick-boundary step was real production behaviour, and the C1 fix shrank it 4.5x

**First, a correction this note owes itself.** §1's C7 row cited the effect-off
and camera-only arms, and an earlier draft of this section read that as the
whole story — as though the boundary step were an artifact of arms that hold
what production interpolates. It is not. §3's step table measured **shipped
production** too, and that row is the one that matters:

| Arm (pre-fix, §3) | → alpha 0 | → 1/4 | → 2/4 | → 3/4 | interior mean | boundary excess (pooled) |
|---|---|---|---|---|---|---|
| **`a120-allon` — production, v1.0.1-v1.0.3** | **10.810** | 10.509 | 8.658 | 8.626 | 9.264 | **+16.68%** |
| `a120-effoff` | 11.300 | 9.586 | 9.766 | 9.921 | 9.758 | +15.81% |
| `a120-alloff` | 12.240 | 10.102 | 10.368 | 10.605 | 10.358 | +18.17% |

A player on 1.0.3 with smoothing on was seeing a **+16.7%** step into every
authored endpoint. The three arms agree within a couple of points because they
were elevated for two different reasons at once — the contrast arms because
they hold what production interpolates, production because of C1 — and that
coincidence is what made the earlier misreading easy.

**The C1 anchoring fix is what closed it.** Production on the fixed build:

| Arm | → alpha 0 | → 1/4 | → 2/4 | → 3/4 | boundary excess (pooled) |
|---|---|---|---|---|---|
| **`a120-allon` (fixed)** | **9.187** | 8.838 | 8.881 | 8.850 | **+3.73%** |
| `a120-alloff` | 12.240 | 10.102 | 10.368 | 10.605 | +18.17% |
| `a120-uvoff` | 9.203 | 8.835 | 8.877 | 8.846 | +3.90% |
| `a60-allon` (fixed) | 13.912 | 13.548 | — | — | +2.69% |

**+16.68% to +3.73% — a factor of 4.5**, with no change to the HUD, the
content, the route or the grid. The mechanism is the one §3 already named
without following through: the pre-fix table's *two-big-two-small* shape is the
displaced shell jumping out of place on the first interpolated present (10.509)
and snapping back at the endpoint (10.810), with the two interior steps small
(8.658, 8.626) because the shell was nearly static in between. The boundary
step was carrying the snap-back. Anchor the shell to its own tick and the four
steps flatten to 9.187 / 8.838 / 8.881 / 8.850.

**What is left, and how much of it is real.** Pooled means hide the per-tick
pairing, so the residual is measured tick by tick: each authored boundary step
against **its own tick's** three interior steps, n = 29 complete ticks.

| Region | Share of frame | Boundary | Interior | Excess | t | 95% CI | ticks positive |
|---|---|---|---|---|---|---|---|
| Whole frame | 100% | 9.187 | 8.925 | **+2.93%** | 3.85 | +1.44% .. +4.42% | 25/29 |
| HUD boxes (text + radar) | 18.81% | 8.806 | 8.006 | **+10.00%** | 12.58 | +8.44% .. +11.56% | **29/29** |
| Everything else | 81.19% | 9.275 | 9.139 | **+1.49%** | 1.63 | **-0.30% .. +3.29%** | 20/29 |

**The reference this has to be read against is zero.** On a uniform alpha grid
every step is the same fraction of a tick, so content every stage covers moves
the same distance on each of the four steps and its boundary excess is 0. The
same non-HUD region with every stage switched off reads **+18.58%** (t = 23.29,
29/29 ticks) — that is what uncovered content looks like at this sample size.

Against those two poles: **the HUD excess is unambiguous and the rest is not
distinguishable from zero.** The non-HUD 95% confidence interval includes zero
and only 20 of 29 ticks are positive.

**The HUD component is stage-independent, which is the proof it is authored.**
The same two boxes read **+10.31%** in the camera-only arm against production's
**+10.00%**. Turning off all seven interpolation stages moves the HUD's
boundary excess by 0.3 points — because no stage was ever touching it. The
two boxes separately, and the route's scroller for contrast (full-frame
640x480 boxes; these three rows are **pooled** means, so they sit slightly
above the paired figures above and are not directly comparable to them):

| Region | Box | Share of frame | Boundary | Interior | Excess |
|---|---|---|---|---|---|
| HUD text — position / lap / bananas / time | y 24-104, x 40-600 | 14.58% | 9.297 | 8.247 | +12.73% |
| HUD radar — minimap disc and blip | y 330-430, x 470-600 | 4.23% | 7.113 | 6.653 | +6.92% |
| Waterfall sheet (the route's UV scroller) | y 0-300, x 280-470 | 18.55% | 10.051 | 9.709 | +3.52% |

The pixel-level version says the same thing: pixels whose mean boundary step
exceeds 4 while their mean interior step stays under 0.5 — content that moves
*only* at the authored tick — number **514 of 307,200 (0.167% of the frame)**,
inside a bounding box of y 66-369, x 504-579. That is the TIME digits and the
radar blip, and nothing else. UV scroll contributes nothing measurable: the
`a120-uvoff` arm moves the boundary step by 0.016.

**The non-HUD remainder is diffuse, and named as open.** Tiling the non-HUD
frame at 32x32: **173 of 300 tiles carry positive excess and 127 carry
negative**, the top five tiles carry only 27.9%, and the positive mass (78,146)
exceeds the net (48,363) by 62% — most of it cancels. The largest cluster
(y 256-383, x 288-383) is the player kart and its forced shield. That is
consistent with a small real contribution from per-tick discrete content
(texture-frame animation, sprite spawn and despawn) sitting under sampling
noise, and this note does **not** claim to have attributed it: 47% of a
residual that is itself statistically indistinguishable from zero is recorded
as open in §5.4, not explained.

**The other two candidate causes, against what the data actually supports.**

*(b) A half-frame phase error — excluded, with the census read correctly.* The
pacing census differences the **monotone phase** (whole ticks plus sub-tick
alpha, in ppm of a tick) between consecutive **displayed** presents. Held
presents are not displayed and contribute no sample: `presents=12920`,
`displayed=12884`, and 12,920 − 12,884 = **36**, the run's stale-hold count
exactly. Over the 12,883 intervals:

```
[PRESENTPERF-HIST] series=alpha-delta n=12883 min=250000 p50=253952
p95=253952 p99=253952 max=1250000 mean=250698 regressions=0 over=0 stalls=0
```

**No interval is ever shorter than exactly one quarter tick, and at least 99%
are exactly that** — `min` is 250,000 and p50, p95 and p99 all sit in the
250,000-254,095 bin. A half-frame phase error puts displayed intervals at
values that are not multiples of a quarter tick, or moves p50 off the minimum;
neither happens, so the first interpolated present is on the exact rational
grid the subloop claims.

The tail to `max=1250000` is **not** a counter-example, and the earlier draft's
"every present advances exactly one quarter tick" was too strong: a held
present's quarter tick of advance is not lost, it lands in the *following*
interval. So the legal values above the minimum are 500,000 (one hold),
750,000 (two), and 1,250,000 (four consecutive holds) — all exact multiples of
the grid step. The arithmetic closes: the 36 holds carry 36 x 250,000 =
**9,000,000 ppm** of deferred advance, and the measured total excess over the
minimum is (250,698 − 250,000) x 12,883 = **8,992,334 ppm**. Nine ticks against
nine ticks.

`stalls=0` and `stale=36` are not in conflict either: `stalls` counts two
**displayed** presents landing on an identical phase — a frame the player could
not tell from its predecessor — and a stale hold is never displayed, so it is
outside that census by construction. What this census cannot speak to is the 36
held presents' own phase, because it does not sample them.

*(c) A hold burst at the alpha transitions — excluded in the measured window.*
None of the 119 steps in the sampled window is zero, so no present there
repeated its predecessor. Run-wide the holds are 36 of 12,920 presents (0.28%),
and (b) accounts for all of them.

**Disposition: benign.** The measured, significant part of the residual is the
2D HUD — screen-space texrects whose content is discrete (digits, a blip
position) and which have no world-space pose pair to interpolate. The
whole-frame effect is a **2.9% step-size ripple**, against the **16.7%** the
same route showed in shipped 1.0.3. **No fix**; C1's fix was the fix.

### 5.2 C8 — every UV-scroll hold is a refusal the contract exists to make

`uvscrollhold` now attributes itself to the clause that refused. Route A,
3,230 authored ticks at 120 Hz:

| Clause | Count | Share of holds |
|---|---|---|
| `uvscrollholdunpub` — no `{T-1}` record for this batch | 6,702 | 65.4% |
| `uvscrollholdphase` — the two published ticks disagree | 3,546 | 34.6% |
| `uvscrollholdambig` — poisoned key | **0** | — |
| `uvscrollholdshape` — triangle count moved | **0** | — |
| total, against 70,389 confirmations | 10,248 | 12.71% of 80,637 lookups |

Route B (battle challenge, 4,200 ticks) splits the same way:
21,930 / 4,698 / 0 / 0, over 248,727 lookups — 10.71%.

**The rate independence is arithmetic, not evidence of load.** Confirm-or-hold
is decided once per (authored tick, batch) from the published table, and every
alpha inside that tick repeats the same decision. Both the numerator and the
denominator therefore scale with presents-per-tick, and the ratio is the same
at 60 and 120 Hz **by construction**. It was never a load signal.

**Where the holds are on route A: before the race.** 18 of the 19 batch keys
the replay ever looks up hold at least once, and **the last hold on this route
is at authored tick 3122** — before the countdown clears. In the window every
other measurement in this note uses, ticks 3200-3230, there are **90 UV-scroll
lookups and 90 confirmations: zero holds**. The only scroller live during the
race, key `0xc8620a650` (ticks 2988-3229), confirms **726 of 726**. Route A's
14.56% is a menu-and-lobby number.

**Where the holds are on route B: throughout, including gameplay.** This does
not generalise from route A, and route B is the counter-case. Its holds run to
authored tick 4198, and in the last 30 ticks — the window its own bisection arm
dumps — there are **3,432 lookups, 2,994 confirmations and 438 holds: 12.76%,
all `unpublished`, no phase holds**. Per 500-tick band the hold rate on route B
sits between 5.2% and 11.8% from tick 500 to the end. A battle challenge is
gameplay, so **UV-scroll holds do reach the screen during play on some
content** — at roughly one lookup in eight, each costing one authored tick of
one surface's texture phase.

**The two clauses, named.** Route A's 3,546 phase holds are four triangle
batches in a pre-race scene (ticks 269-567) whose per-tick U displacement
genuinely oscillates — successive published ticks report du of -98, +98, +25, -97 S10.5
units on the same batch, sign included. There is no constant tick displacement
for the replay to scale by alpha, and interpolating one of those readings would
sweep the surface the wrong way for a quarter of a tick. Refusing is the
contract working exactly as `gfx_pc_dkr.c`'s wrap rule describes. The 6,702
unpublished holds are dominated by one static 4-triangle sheet
(`0x104fe0594`: 5,697 holds against **11,415 confirmations on the same key**)
and a 1-triangle companion (723 against 1,359) — batches that publish
intermittently rather than every tick, so a third of their own lookups find no
adjacent pair to confirm against. Each such hold self-clears on the batch's
next published tick.

**Route B adds a second phase-hold shape, and a named limitation.** Six of its
ten phase-holding keys are 12-triangle batches whose displacement alternates
between `du=0, dv=2` and `du=4, dv=2` on adjacent ticks (576 lookups at each
reading). That is a sub-unit U scroll rate quantised to whole S10.5 units: the
surface really does advance 4 units every other tick, so **no two consecutive
ticks can ever agree** and the batch phase-holds on every present it is drawn
in. Its V axis, meanwhile, is a constant 2 per tick and could be interpolated —
the confirmation is per record, not per axis, so a disagreement on U refuses V
with it. A per-axis confirmation is the obvious improvement and it is
deliberately **not** made here: it widens a fail-closed contract at the end of
a hardening task, it needs its own red-then-green witness, and the axis it
would newly interpolate is the one the wrap rule protects. Recorded in §5.4.

**Disposition: benign, fail-closed by design. No fix.** A hold is one authored
tick of one surface's texture phase, and the alternative — interpolating a
displacement no second observation corroborates — is the wrap artifact the
confirmation rule was built to prevent. What was missing was not a fix but the
attribution, and the unit coverage: `gfx_presentation_packet_capture_uv_scroll`
and `_lookup_uv_scroll` had **no unit caller at all** before this section (the
architecture note recorded exactly that gap), and now have one per clause.

### 5.3 C9 — the primitive-alpha zero was the window, not the stage

Route A's 119,129 overrides and the bisection's zero pixel difference are both
true and they do not describe the same presents. The census now carries the
substitutions' magnitude, and the run can be differenced against a 3,200-tick
arm to isolate the 30 ticks the pixels were measured over:

| | whole route (3,230 ticks) | sampled window (ticks 3200-3230) |
|---|---|---|
| `primalphahit` | 654,816 | 9,021 |
| `primalphaoverride` | 119,129 | **48** |
| `primalphadeltasum` | 1,839,152 | 180 |
| mean substitution | 15.4 of 255 | 3.75 of 255 |
| `particleprimalphaoverride` | 101,782 | **0** |
| `projectedshadowprimalphaoverride` | 8,055 | **0** |

**99.96% of the overrides are outside the window the pixels were counted in**,
and 85% of them are particle fades. Inside it the stage moves the alpha byte 48
times by 3.75 steps on average, on no particle and no projected shadow — which
is why the leave-one-out arm is byte-identical on all 120 frames (0 endpoints
and 0 intermediates differ, total absolute difference 0).

**Its contribution window is elsewhere, and is already gated.** Route B (battle
challenge, level 26) applies 178,038 overrides of which 151,084 are particle,
and `check_presentation_matrix.py`'s arm C primitive-alpha control asserts a
pixel difference there against the alpha-hold arm: on its own shorter 60 Hz
window it reports 50,372 changed draws from 97,849 compatible pairs and 35 of
50 intermediate backend frames moved. That gate fails if interpolated fades
stop reaching the backend.

**The cost, bounded.** `[PRESENTPERF] section=replay` over 9,681 replays:
**545,996 ns** mean with the stage on, **538,970 ns** with it off — a
difference of **7,026 ns**, so the stage costs **about 7.0 microseconds per
replay** (under 7.1), 1.3% of the replay section and 0.08% of a 120 Hz
presentation interval. One sample per arm, so read it as a bound rather than a
measured delta.

**Disposition: benign. No fix.** The correction is to the claim, not the code:
§1's C9 row said "119,129 interpolated alpha substitutions changed nothing on
this window", and the accurate statement is that 48 of them were in that window
and the other 119,081 were never sampled.

### 5.4 What C7/C8/C9 leave open

- **C7's non-HUD residual is not attributed.** It is +1.49% with a 95%
  confidence interval of -0.30% .. +3.29% over 29 ticks, so it may be nothing;
  but it is 47% of the whole-frame excess and 173 of 300 screen tiles carry
  some of it. A longer sample would say whether it is real, and if it is, the
  candidates are per-tick discrete content no stage can cover by construction
  (texture-frame animation, sprite spawn and despawn). This note does not
  claim to have identified it.
- **The 36 held presents' own phase is unmeasured.** The alpha-delta census
  samples displayed frames only, so it can prove the displayed grid is exact
  and cannot say anything about the phase a held present would have carried.
- **C7 is one route, one window, one HUD layout.** Jungle Falls under a chase
  camera. A split-screen or boss layout redistributes the HUD's 52% share and
  nothing here says how.
- **UV-scroll holds reach gameplay on route B and the per-axis limitation
  stands.** 12.76% of lookups hold in its sampled window, and its
  alternating-`du` batches phase-hold on every present because confirmation is
  per record rather than per axis. Splitting the confirmation by axis would let
  the constant V component glide; it is not done here (see §5.2) and remains
  the one open improvement any of these three classes suggests.
- **C5 is still unwitnessed**, as §4.6 records: nothing here runs against a
  real display, so a physical-vsync component of the owner's complaint is
  neither confirmed nor excluded.

### 5.6 Amendment (2026-08-09): C8's phase holds were a defect after all

§5.2 dispositioned every UV-scroll hold as benign and left per-axis
confirmation as the one open improvement. The phase-hold half of that
disposition was **wrong**, and the reason is arithmetic that neither §5.2 nor
its route A measurement could see.

`obj_loop_texscroll` advances through a two-bit accumulator, so an authored
rate that is not a multiple of four **alternates its emitted whole-unit step
by one on every tick, permanently**. Two adjacent ticks can never agree, so
the confirmation rule refuses on every present the batch is drawn in. Jungle
Falls could not show this: its waterfall is authored at 32 quarter units a
tick (16 whole units at `updateRate` 2, exactly constant), which is why it
confirms 726 of 726 — and both gates that exercised UV scroll ran Jungle Falls
only. §5.2's claim that "authored scroll speed is a level constant, so a real
scroller confirms on its second tick" is true of the speed and false of the
bytes.

**The affected content, enumerated at runtime.** Every level loaded through
`MDKR_LOAD_TRACK` and dumped at `obj_loop_texscroll`: an *odd* authored rate
is the affected predicate (at `updateRate` 2 it makes the per-tick advance
2 mod 4). Levels 0, 3, 8, 9, 13, 15, 17, 19, 20, 28, 35, 36 and 42 carry at
least one; levels 7, 29 (Jungle Falls), 30, 31, 38 and 46 are multiples of
four throughout and were never affected.

**The fix is the authored rate, not a weaker rule.** The driver publishes its
rate and accumulator residue; the replay computes `(phase + rate*alpha)/4`.
The wrap rule is untouched for every measured scroller, and per-axis
confirmation — §5.4's open improvement — was not needed and was not made.

**Measured, level 19 (two fractional scrollers, authored V rates 127 and 85),
3,400 authored ticks at 120 Hz.** The red arm is the same binary with
`MDKR_TEST_UV_SCROLL_AUTHORED_RATE=off`, and it reproduces the pre-fix build
exactly:

| | authored rate off (= pre-fix) | authored rate on |
|---|---|---|
| `uvscrollholdphase` | 6,417 | **4,170** |
| `uvscrollauthoredconfirm` | 0 | 2,247 |
| `uvscrollholdunpub` | 6,246 | 6,246 |
| `uvscrollholdambig` / `holdshape` | 0 / 0 | 0 / 0 |

The 2,247 recovered lookups equal the authored confirmations exactly, and the
authored batches now hold **zero** times — `ambiguous` and `shape` are the only
clauses an authored record can reach, and both are zero. The residual 4,170
phase holds are the pre-race scene §5.2 already attributed (the same 3,546-plus
population route A carries), which is a genuine oscillating displacement and
still refuses, correctly. Jungle Falls is unchanged: 12.71% before and after,
because its rate was never fractional.

The gate is `check_smoothing_stage_bisection.py` route C, printed as
`[UV-SCROLL-AUTHORED]`; it fails on a tree without the fix because the green
arm registers no authored rates at all.

### 5.5 What did not change, and how that was checked

The three census commits add counters and change no branch. The witness is the
same one §4.5 uses, re-run against the pre- and post-census binaries on route A
(3,230 authored ticks, `MDKR_PRESENT_RATE=120`,
`MDKR_PRESENT_SMOOTHING=interpolate`, `MDKR_FORCE_SHIELD=12680:600`, GL,
`--window-size 320x240`, autopilot on `nav_to_time_trial_race.txt`):

- `[SIMHASH]`/`[EVENTHASH]`/`[INPUTHASH]`: **9,690 rows, byte-identical**;
- the 120 dumped presents (`MDKR_DUMP_FROM=12800`): **120 of 120 frames
  byte-identical**.

The magnitude census is itself gated rather than trusted. Mutating
`primitive_alpha_delta_sum += delta` to `+= 0u` and the peak comparison to a
constant, then running the same 900-tick arm:

| Build | `primalphaoverride` | `primalphadeltasum` | `primalphadeltapeak` | Gate |
|---|---|---|---|---|
| mutated | 34,780 | **0** | **0** | **FAIL** — "the magnitude census is accumulating nothing" |
| as committed | 34,780 | 544,082 | 191 | PASS (mean 15.64) |

`check_smoothing_stage_bisection.py` asserts that identity — an override is a
changed alpha byte, so the sum cannot fall below the count and the peak cannot
be zero — on both routes' production arm, and prints
`[PRIM-ALPHA-MAGNITUDE]`. The UV-scroll clause counters are held to their
aggregate by the same gate and print `[UV-SCROLL-HOLDS]`.

Standing gates, all green on the committed tree: `check_presentation_matrix.py`,
`check_smoothing_stage_bisection.py`, `check_effect_shell_envelope.py`, and the
`presentation_packet` unit (which now carries the UV-scroll contract's first
unit coverage).

## Appendix A — the shell-track measurement, reproducible

This is a one-off analysis, not a standing gate, so it lives here rather than in
`tests/`. It reads the PPM frame dumps the arms in §Method produce and needs
only numpy.

```python
import numpy as np

Y0, Y1, X0, X1 = 150, 480, 120, 600   # full-frame window; HUD band excluded

def components(mask):
    """8-connected component sizes and bounding boxes, largest first."""
    h, w = mask.shape
    seen = np.zeros((h, w), bool)
    sizes, boxes = [], []
    for y0, x0 in zip(*np.nonzero(mask)):
        if seen[y0, x0]:
            continue
        stack, size = [(y0, x0)], 0
        seen[y0, x0] = True
        miny = maxy = y0
        minx = maxx = x0
        while stack:
            y, x = stack.pop()
            size += 1
            miny, maxy = min(miny, y), max(maxy, y)
            minx, maxx = min(minx, x), max(maxx, x)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = y + dy, x + dx
                    if (0 <= ny < h and 0 <= nx < w
                            and mask[ny, nx] and not seen[ny, nx]):
                        seen[ny, nx] = True
                        stack.append((ny, nx))
        sizes.append(size)
        boxes.append((miny, minx, maxy, maxx))
    order = np.argsort(sizes)[::-1]
    return [sizes[i] for i in order], [boxes[i] for i in order]

def shell(img):
    """(area, cx, cy, bbox) of the additive green shell, FULL-FRAME pixels."""
    r, g, b = img[..., 0], img[..., 1], img[..., 2]
    m = (g > r + 45) & (g > b + 45) & (g > 110)
    w = np.zeros_like(m)
    w[Y0:Y1, X0:X1] = m[Y0:Y1, X0:X1]
    if w.sum() < 200:
        return None
    sizes, boxes = components(w)
    y0, x0, y1, x1 = boxes[0]
    lab = np.zeros_like(w)
    lab[y0:y1 + 1, x0:x1 + 1] = w[y0:y1 + 1, x0:x1 + 1]
    ys, xs = np.nonzero(lab)
    # CONTAINMENT: this must never touch a window edge, or the area and the
    # centroid are both measuring the crop instead of the shell.
    assert y0 > Y0 and x0 > X0 and y1 < Y1 - 1 and x1 < X1 - 1
    return sizes[0], float(xs.mean()), float(ys.mean()), (y0, x0, y1, x1)
```

`disp_from_own_tick_authored` is the euclidean distance from the centroid of the
authored present that opens the same tick (the frame at `frame - alpha`). The
endpoint-to-endpoint envelope is the same distance between consecutive authored
presents.

## Appendix B — raw per-frame rows

240 rows: 120 presents each for `a120-allon` and `a120-effoff`, authored ticks
3200–3230 at `MDKR_PRESENT_RATE=120`. `a60-allon`'s 60 rows are omitted because
every one of its frames is byte-identical to the `a120-allon` frame at the same
alpha (§3).

```csv
frame,arm,alpha,area,cx,cy,disp_from_own_tick_authored
12800,a120-allon,0,8530,321.07,318.06,0.00
12801,a120-allon,1,2789,366.77,298.80,49.60
12802,a120-allon,2,2836,367.67,299.11,50.30
12803,a120-allon,3,2818,368.29,298.81,50.99
12804,a120-allon,0,7700,327.53,318.94,0.00
12805,a120-allon,1,2687,368.94,297.96,46.42
12806,a120-allon,2,2639,369.09,297.52,46.75
12807,a120-allon,3,2524,368.94,296.72,46.99
12808,a120-allon,0,7500,324.69,319.32,0.00
12809,a120-allon,1,2620,370.89,295.60,51.93
12810,a120-allon,2,2692,371.86,295.30,52.93
12811,a120-allon,3,2821,371.87,294.71,53.21
12812,a120-allon,0,7714,330.33,319.40,0.00
12813,a120-allon,1,2814,371.13,293.27,48.45
12814,a120-allon,2,2857,370.88,292.92,48.43
12815,a120-allon,3,2958,371.45,293.10,48.81
12816,a120-allon,0,7466,331.37,321.07,0.00
12817,a120-allon,1,3069,373.40,293.34,50.36
12818,a120-allon,2,3106,374.22,292.85,51.31
12819,a120-allon,3,3131,374.91,292.77,51.93
12820,a120-allon,0,6946,335.88,319.93,0.00
12821,a120-allon,1,2745,374.99,291.81,48.17
12822,a120-allon,2,2679,376.17,292.01,49.02
12823,a120-allon,3,2579,377.48,292.28,49.95
12824,a120-allon,0,6616,333.56,322.94,0.00
12825,a120-allon,1,2360,379.00,292.45,54.72
12826,a120-allon,2,2301,379.80,292.66,55.28
12827,a120-allon,3,2297,379.76,292.73,55.20
12828,a120-allon,0,6318,336.85,324.74,0.00
12829,a120-allon,1,2427,379.57,294.54,52.32
12830,a120-allon,2,2388,380.53,296.29,52.14
12831,a120-allon,3,2369,380.98,297.88,51.66
12832,a120-allon,0,6190,336.31,328.44,0.00
12833,a120-allon,1,2372,380.92,297.51,54.29
12834,a120-allon,2,2308,381.36,297.27,54.79
12835,a120-allon,3,2257,382.22,297.93,55.13
12836,a120-allon,0,5716,336.03,328.52,0.00
12837,a120-allon,1,2192,382.44,300.04,54.45
12838,a120-allon,2,2100,382.64,301.11,54.07
12839,a120-allon,3,1670,385.48,309.40,53.02
12840,a120-allon,0,5516,334.52,329.28,0.00
12841,a120-allon,1,1864,384.67,300.69,57.72
12842,a120-allon,2,1672,384.79,298.59,58.89
12843,a120-allon,3,1785,386.09,301.34,58.65
12844,a120-allon,0,5453,335.49,326.20,0.00
12845,a120-allon,1,1845,386.32,302.51,56.09
12846,a120-allon,2,1987,386.76,302.11,56.65
12847,a120-allon,3,2091,386.82,300.97,57.20
12848,a120-allon,0,5488,340.06,324.83,0.00
12849,a120-allon,1,2179,385.07,298.97,51.90
12850,a120-allon,2,2323,384.95,298.11,52.24
12851,a120-allon,3,2243,384.55,296.58,52.70
12852,a120-allon,0,5784,337.38,322.42,0.00
12853,a120-allon,1,2025,384.69,298.62,52.96
12854,a120-allon,2,2027,385.58,298.92,53.62
12855,a120-allon,3,1463,382.55,292.97,53.92
12856,a120-allon,0,5903,338.40,322.70,0.00
12857,a120-allon,1,2027,385.45,298.30,53.00
12858,a120-allon,2,2021,386.01,298.32,53.48
12859,a120-allon,3,2029,386.23,298.46,53.63
12860,a120-allon,0,3266,336.45,304.38,0.00
12861,a120-allon,1,1914,384.57,298.86,48.44
12862,a120-allon,2,2073,385.76,299.78,49.52
12863,a120-allon,3,2154,386.56,299.97,50.30
12864,a120-allon,0,5751,342.55,320.67,0.00
12865,a120-allon,1,2152,384.68,299.08,47.35
12866,a120-allon,2,2106,385.17,298.89,47.87
12867,a120-allon,3,2169,385.71,298.49,48.53
12868,a120-allon,0,3374,340.67,302.62,0.00
12869,a120-allon,1,2050,383.60,296.33,43.39
12870,a120-allon,2,2036,383.88,295.74,43.76
12871,a120-allon,3,1333,382.06,283.02,45.80
12872,a120-allon,0,6474,341.30,319.34,0.00
12873,a120-allon,1,1391,380.67,281.27,54.77
12874,a120-allon,2,1478,379.96,280.76,54.61
12875,a120-allon,3,1582,378.94,280.74,53.91
12876,a120-allon,0,7258,341.18,317.49,0.00
12877,a120-allon,1,2491,378.84,289.64,46.83
12878,a120-allon,2,2532,377.79,289.45,46.11
12879,a120-allon,3,2641,378.01,289.85,46.05
12880,a120-allon,0,7335,340.49,319.82,0.00
12881,a120-allon,1,2814,378.14,290.20,47.90
12882,a120-allon,2,2922,378.95,289.71,48.85
12883,a120-allon,3,3105,379.24,289.50,49.20
12884,a120-allon,0,7563,341.02,316.06,0.00
12885,a120-allon,1,3074,376.94,288.11,45.51
12886,a120-allon,2,3030,376.16,287.06,45.56
12887,a120-allon,3,2628,374.11,282.52,47.12
12888,a120-allon,0,7575,335.86,317.17,0.00
12889,a120-allon,1,2733,375.01,286.15,49.94
12890,a120-allon,2,2660,374.99,285.97,50.05
12891,a120-allon,3,2561,375.56,285.55,50.75
12892,a120-allon,0,7491,334.44,319.16,0.00
12893,a120-allon,1,2269,374.79,284.35,53.29
12894,a120-allon,2,2321,373.50,284.47,52.25
12895,a120-allon,3,2258,371.72,283.15,51.83
12896,a120-allon,0,7855,333.48,319.77,0.00
12897,a120-allon,1,2421,370.63,287.60,49.14
12898,a120-allon,2,2347,370.90,289.15,48.36
12899,a120-allon,3,2367,370.37,290.58,47.05
12900,a120-allon,0,8154,330.64,321.16,0.00
12901,a120-allon,1,2381,368.97,290.70,48.96
12902,a120-allon,2,2377,368.48,289.96,49.04
12903,a120-allon,3,2271,368.01,289.22,49.16
12904,a120-allon,0,8334,327.38,319.11,0.00
12905,a120-allon,1,2233,366.82,291.38,48.21
12906,a120-allon,2,2262,366.93,292.79,47.50
12907,a120-allon,3,2213,366.87,294.15,46.72
12908,a120-allon,0,8071,323.72,318.59,0.00
12909,a120-allon,1,2294,283.38,286.81,51.35
12910,a120-allon,2,2231,282.81,284.74,53.09
12911,a120-allon,3,1968,365.27,293.45,48.56
12912,a120-allon,0,7661,322.01,314.49,0.00
12913,a120-allon,1,2098,280.09,280.48,53.98
12914,a120-allon,2,2144,279.86,280.20,54.33
12915,a120-allon,3,2257,280.45,280.15,53.91
12916,a120-allon,0,8002,322.16,312.57,0.00
12917,a120-allon,1,2438,280.92,280.74,52.09
12918,a120-allon,2,2537,281.30,280.87,51.71
12919,a120-allon,3,2313,360.36,293.56,42.67
12800,a120-effoff,0,8530,321.07,318.06,0.00
12801,a120-effoff,1,9217,317.80,323.32,6.19
12802,a120-effoff,2,10105,314.37,330.16,13.83
12803,a120-effoff,3,11323,310.44,336.60,21.37
12804,a120-effoff,0,7700,327.53,318.94,0.00
12805,a120-effoff,1,8374,324.24,324.00,6.03
12806,a120-effoff,2,9166,319.51,330.78,14.30
12807,a120-effoff,3,10409,315.57,338.08,22.57
12808,a120-effoff,0,7500,324.69,319.32,0.00
12809,a120-effoff,1,8167,320.94,324.74,6.59
12810,a120-effoff,2,9139,316.38,331.85,15.03
12811,a120-effoff,3,10210,311.83,338.70,23.27
12812,a120-effoff,0,7714,330.33,319.40,0.00
12813,a120-effoff,1,8031,326.63,325.26,6.93
12814,a120-effoff,2,8991,321.70,332.25,15.48
12815,a120-effoff,3,10227,316.27,339.07,24.18
12816,a120-effoff,0,7466,331.37,321.07,0.00
12817,a120-effoff,1,7915,326.81,327.22,7.66
12818,a120-effoff,2,8678,321.41,334.19,16.46
12819,a120-effoff,3,10144,317.26,340.91,24.35
12820,a120-effoff,0,6946,335.88,319.93,0.00
12821,a120-effoff,1,7537,329.60,325.86,8.64
12822,a120-effoff,2,8089,323.57,332.71,17.75
12823,a120-effoff,3,9131,318.26,339.40,26.26
12824,a120-effoff,0,6616,333.56,322.94,0.00
12825,a120-effoff,1,7119,328.50,329.02,7.91
12826,a120-effoff,2,7745,321.96,335.75,17.28
12827,a120-effoff,3,8701,314.60,342.65,27.35
12828,a120-effoff,0,6318,336.85,324.74,0.00
12829,a120-effoff,1,6857,331.66,330.98,8.12
12830,a120-effoff,2,7567,324.86,338.40,18.17
12831,a120-effoff,3,8845,316.87,345.22,28.61
12832,a120-effoff,0,6190,336.31,328.44,0.00
12833,a120-effoff,1,6639,330.85,334.92,8.47
12834,a120-effoff,2,7405,324.16,341.95,18.17
12835,a120-effoff,3,8962,316.98,348.81,28.08
12836,a120-effoff,0,5716,336.03,328.52,0.00
12837,a120-effoff,1,5904,329.19,334.45,9.05
12838,a120-effoff,2,7393,322.51,341.64,18.84
12839,a120-effoff,3,8600,314.46,348.29,29.26
12840,a120-effoff,0,5516,334.52,329.28,0.00
12841,a120-effoff,1,5559,327.86,335.49,9.12
12842,a120-effoff,2,6532,320.66,342.07,18.86
12843,a120-effoff,3,8243,313.96,348.64,28.24
12844,a120-effoff,0,5453,335.49,326.20,0.00
12845,a120-effoff,1,5944,329.44,332.26,8.56
12846,a120-effoff,2,6794,322.52,338.54,17.90
12847,a120-effoff,3,7736,314.81,345.41,28.23
12848,a120-effoff,0,5488,340.06,324.83,0.00
12849,a120-effoff,1,6073,334.54,331.20,8.43
12850,a120-effoff,2,5896,328.31,338.11,17.73
12851,a120-effoff,3,7277,320.14,345.26,28.53
12852,a120-effoff,0,5784,337.38,322.42,0.00
12853,a120-effoff,1,6107,331.29,328.89,8.89
12854,a120-effoff,2,6443,324.59,335.87,18.55
12855,a120-effoff,3,7446,317.40,343.02,28.69
12856,a120-effoff,0,5903,338.40,322.70,0.00
12857,a120-effoff,1,6115,331.33,328.31,9.02
12858,a120-effoff,2,6644,323.64,334.66,19.00
12859,a120-effoff,3,7867,319.14,341.44,26.88
12860,a120-effoff,0,3266,336.45,304.38,0.00
12861,a120-effoff,1,3579,328.49,309.00,9.21
12862,a120-effoff,2,6636,324.01,331.72,30.04
12863,a120-effoff,3,8559,317.85,338.53,38.88
12864,a120-effoff,0,5751,342.55,320.67,0.00
12865,a120-effoff,1,3456,337.01,307.18,14.59
12866,a120-effoff,2,6904,330.21,332.64,17.19
12867,a120-effoff,3,8294,324.32,339.50,26.20
12868,a120-effoff,0,3374,340.67,302.62,0.00
12869,a120-effoff,1,3656,335.96,306.75,6.26
12870,a120-effoff,2,5555,329.87,331.84,31.15
12871,a120-effoff,3,8598,324.04,338.63,39.66
12872,a120-effoff,0,6474,341.30,319.34,0.00
12873,a120-effoff,1,7120,336.71,324.31,6.76
12874,a120-effoff,2,7809,330.91,330.87,15.52
12875,a120-effoff,3,8919,326.66,337.76,23.53
12876,a120-effoff,0,7258,341.18,317.49,0.00
12877,a120-effoff,1,7580,337.83,323.14,6.57
12878,a120-effoff,2,8218,334.21,329.82,14.17
12879,a120-effoff,3,9522,329.11,336.50,22.52
12880,a120-effoff,0,7335,340.49,319.82,0.00
12881,a120-effoff,1,8032,336.72,325.19,6.56
12882,a120-effoff,2,8706,332.46,331.13,13.88
12883,a120-effoff,3,9831,329.05,337.85,21.35
12884,a120-effoff,0,7563,341.02,316.06,0.00
12885,a120-effoff,1,8251,337.69,321.34,6.24
12886,a120-effoff,2,8876,333.68,327.28,13.41
12887,a120-effoff,3,9699,329.47,333.35,20.80
12888,a120-effoff,0,7575,335.86,317.17,0.00
12889,a120-effoff,1,8193,333.94,322.10,5.29
12890,a120-effoff,2,8809,331.16,327.84,11.66
12891,a120-effoff,3,9495,327.68,334.42,19.09
12892,a120-effoff,0,7491,334.44,319.16,0.00
12893,a120-effoff,1,7975,332.96,324.56,5.60
12894,a120-effoff,2,8800,331.08,330.42,11.74
12895,a120-effoff,3,9925,328.79,336.90,18.61
12896,a120-effoff,0,7855,333.48,319.77,0.00
12897,a120-effoff,1,8595,331.83,325.60,6.06
12898,a120-effoff,2,9180,330.60,331.21,11.79
12899,a120-effoff,3,10131,329.14,337.98,18.71
12900,a120-effoff,0,8154,330.64,321.16,0.00
12901,a120-effoff,1,8823,330.02,326.54,5.42
12902,a120-effoff,2,9220,328.61,332.82,11.84
12903,a120-effoff,3,10459,327.55,339.61,18.71
12904,a120-effoff,0,8334,327.38,319.11,0.00
12905,a120-effoff,1,9171,327.09,324.53,5.42
12906,a120-effoff,2,9722,326.42,330.63,11.56
12907,a120-effoff,3,10628,326.49,337.45,18.36
12908,a120-effoff,0,8071,323.72,318.59,0.00
12909,a120-effoff,1,8919,323.83,324.25,5.66
12910,a120-effoff,2,9384,323.90,330.26,11.67
12911,a120-effoff,3,10378,323.46,336.95,18.36
12912,a120-effoff,0,7661,322.01,314.49,0.00
12913,a120-effoff,1,8345,322.43,320.00,5.53
12914,a120-effoff,2,9105,322.81,326.11,11.65
12915,a120-effoff,3,9916,323.13,332.52,18.06
12916,a120-effoff,0,8002,322.16,312.57,0.00
12917,a120-effoff,1,8546,323.03,317.35,4.86
12918,a120-effoff,2,9268,324.55,323.30,10.99
12919,a120-effoff,3,10086,325.87,329.46,17.30
```
