# Open items — Gameplay, race and Adventure

> One subsystem of the split [open-items index](README.md), which states how these files are kept.

**Currently open** (per [`README.md`](README.md#still-open)'s open/closed
table): 8 items.

| Item | Where |
|---|---|
| Gameplay cameras can enter terrain and object geometry — the correction ships as an opt-in (`Camera.Obstruction`); default-on was tried and rejected by device acceptance on 2026-08-07, so the defect still stands in default play | [§ OPEN: gameplay cameras can enter terrain and object geometry](#open-gameplay-cameras-can-enter-terrain-and-object-geometry) |
| **(index-level item)** F-18 boss cadence / independent state reference — Original two-field cadence matches retail ROM boss timing; oracle breadth (challenge, multiplayer, progression, audio, renderer-state) remains open. No subsystem section exists for this item; it is tracked at the index level | [§ Still open](README.md#still-open), [`BLUEY2_PARITY.md`](../BLUEY2_PARITY.md) |
| Zip-pad boost magnitude is authored, but no route exercises a real pad crossing and no hardware trace was taken | [§ CLOSED, NOT A DEFECT: the zip-pad boost — wave "zippad"](#closed-not-a-defect-the-zip-pad-boost-is-the-magnitude-dkr-authored--wave-zippad) |
| Independent Ancient Lake route drives a real line, but long-horizon standard-race parity against the ROM remains open | [§ The fidelity payoff, measured — and it is NOT there](#the-fidelity-payoff-measured--and-it-is-not-there) |
| AI stuck-recovery cooldown `unk215` only decays while reversing (believed authored; hardware/ares verification still open) | [§ OPEN, believed authored: AI stuck-recovery cooldown](#open-believed-authored-hardware-unverified-ai-stuck-recovery-cooldown-unk215-only-decays-while-reversing--the-hot-top-volcano-crater-wedge) |
| Campaign completeness (silver coins, later boss rematches, both Wizpig races, credits) is ungated, not unimplemented — disclosed in README/ROADMAP but no automated or recorded manual witness of a full start-to-credits pass | [§ OPEN: campaign completeness](#open-campaign-completeness--silver-coins-later-boss-rematches-both-wizpig-races-and-the-credits-path-are-ungated-not-unimplemented) |
| Taj Time Trial records no best time and stores no ghost, silently — deliberate containment, not itemized outside `taj-playable-mod.md` | [§ OPEN, deliberately deferred: Taj Time Trial](#open-deliberately-deferred-taj-time-trial-records-no-best-time-and-stores-no-ghost) |
| Ghost read/write coverage — corrected 2026-08-07: `check_ghost_matrix` round-trips 46 of the 47 legal (track, vehicle) pairs through a fresh process, so what is left open is the 47th, an asserted autopilot non-producer whose ghost round trip nothing drives | [§ OPEN: ghost coverage](#open-ghost-coverage-is-one-track-vehicle-pair-of-47) |

## OPEN: gameplay cameras can enter terrain and object geometry

The car, hovercraft, plane, loop, fixed, finish, and challenge camera behaviors
author positions directly without a terrain or object obstruction resolver. Late
dialogue and shake offsets can move the eye again after the behavior returns, and
the existing terrain detector only decides whether to draw the flat-colour void
curtain. It does not correct the pose, is level/mode conditional, and does not see
object-model occluders such as doors.

The source investigation, target architecture, CAM-00–CAM-09 work breakdown,
test matrix, rollout controls, and definition of done live in the
[camera obstruction and modern native presentation plan](../architecture/camera-obstruction.md).

**Updated 2026-08-07.** The resolver was briefly what a player who sets nothing
gets, and is not any more: it was made the default and reverted the same day
when device acceptance found the corrected camera too sensitive in play. An
unset `MDKR_CAMERA_OBSTRUCTION` resolves to Observe, and the launcher offers
**Keep the camera out of walls** as the opt-in. On the qualified routes the
described defect is corrected under that opt-in — no penetrated, degraded, or
invalid pose is published — but the paragraph above describes the *authored*
camera, which is what a default install renders.

The item stays open because the exit gates that remain are breadth of evidence,
not closed defects: soft-occluder enrollment and its pixel proof are unbuilt,
and the differential-fuzz, GCC/wasm32 sanitizer-equivalent, WebGPU/browser and
resource-plateau corpora have not been run. An uncovered bank or mode is still
possible. It also stays open because the defect stands in default play again:
correcting it is a setting a player has to find. §10.1 of the plan records what
the default flip rested on and why device acceptance reversed it. A center
ray, fixed-radius clamp, terrain-only spring arm, or void-curtain mask remains
explicitly a partial mitigation rather than the fix.


## OPEN, believed authored (hardware-unverified): AI stuck-recovery cooldown `unk215` only decays while reversing — the Hot Top Volcano crater wedge

> Observed during first-boss route work (`tests/check_first_boss_progression.py`)
> once wave "closedloop"'s ASSET_AI_BEHAVIOUR byte-swap fix made the AI table
> decode correctly and started driving the autopilot racer with it: the fourth
> Dino race's AI kart leaves the track at Hot Top Volcano's crater jump (around
> checkpoint 7, frame ~3025) and lands wedged at (-2139.4, -120.7, 1498.9). Left
> alone, it never recovers — measured sitting there, three wheels grounded, A
> held, for 18,677 frames straight.

### Mechanism

`racer_AI_pathing_inputs()`, `game/src/racer.c:790-803`:

```c
if (racer->unk214 == 0 && racer->velocity < -0.5) {
    racer->unk215 -= updateRate;
    if (racer->unk215 < 0) {
        racer->unk215 = 0;
    }
}

if (racer->velocity > -1.0 && racer->unk214 == 0 && !gRaceStartTimer && D_8011D544 == 0.0f &&
    racer->groundedWheels && racer->unk215 == 0) {
    racer->unk213 += updateRate;

    if (racer->unk213 > 60) {
        racer->unk213 = 0;
        racer->unk214 = 60;
        racer->unk215 = 120;
        /* ... advance to the next AI line (unk1CA) ... */
    }
} else {
    racer->unk214 -= updateRate;
    racer->unk213 = 0;
    if (racer->unk214 < 0) {
        racer->unk214 = 0;
    }
}
```

`unk213` accumulates while the kart is stationary, grounded and otherwise
racing normally; once it exceeds 60 update-rate units the AI kicks into a
60-tick "reverse and pick another line" state (`unk214 = 60`, and `unk1CA`
advances to a different AI node) and arms a 120-tick cooldown (`unk215`)
before this recovery can fire again. The cooldown only decrements when
`unk214 == 0` (i.e. after the reverse window has run its course) **and**
`velocity < -0.5`.

> **Sign correction, from the opponent measurement below.** This section
> previously read `velocity < -0.5` as "driving backwards fast enough".
> It is the opposite: `racer->velocity` is **negative for forward motion** —
> `racer.c` assigns `racer->velocity = -sqrtf(...)`, the boss animation picks
> RUN at `velocity < -2.0`, and the opponent witness measures about **-11** for
> a kart driving a clean lap and **+1.9** while the AI's reverse-out state is
> running. So the branch means "the reverse window has expired **and** the kart
> is back to driving forward at speed". Everything the section concludes is
> unchanged — a kart that cannot move satisfies neither reading — but the
> mechanism is that the cooldown clears itself during ordinary driving, which
> is why a kart that gets moving again recovers within a second or two.

If the kart comes out of that window still wedged against geometry — unable to
reach `velocity < -0.5` no matter what it does — `unk215` never reaches 0
again, so the
`unk213`-driven recovery can never re-arm. Nothing else in `racer.c` reads or
writes `unk215`, so once this happens the kart is stuck for the rest of the
race.

### Why this is believed authored, not a port defect

`racer_AI_pathing_inputs()` and the rest of `game/src/racer.c` carry no
`GLOBAL_ASM` and no `NON_MATCHING` — this is matching decompiled code. No
port-side change touches `unk215`, `unk213`, `unk214`, or the branch
conditions around them; they read exactly as the ROM's own logic. The
ASSET_AI_BEHAVIOUR fix that exposed this (`docs/asset_swap_notes.md`) made the
AI table decode *correctly* for the first time — the wedge is a consequence of
the AI table now driving this racer down a real racing line it previously
never reached (the table was garbage before the swap fix), not a new defect
the byte-swap correction introduced.

### Measured: opponents do not wedge, and here is why

This was the open question that decided the item's classification, and it is now
measured rather than assumed. `tests/check_ai_unstick_opponents.py` races Hot Top
Volcano — the level the wedge was found on — with a full eight-racer field and
reads `unk213`/`unk214`/`unk215`, velocity, grounding and checkpoint for every
**genuine** opponent every tick, through a `MDKR_AI_STUCK_TRACE` witness in
`update_AI_racer()` that refuses any racer index ever seen carrying a real player
index.

> Worth stating, because the reachability claim reads the wrong way from a grep:
> `update_player_racer()`'s `} else { racer_AI_pathing_inputs(...); }` is **not**
> the opponent path. It is inside the `if (playerIndex == PLAYER_COMPUTER)
> update_AI_racer(...) else { ... }` split near the top of the function, so it is
> reached only by a *human* kart the finish/menu hand-off has already relabelled
> `PLAYER_COMPUTER`. Opponents run `update_AI_racer()`, which calls
> `racer_AI_pathing_inputs()` while `racer->unk201 != 0`.

**32 races, 32 distinct boot RNG seeds (`MDKR_RNGSEED=0x<hex>`), 7 opponents
each: 23 stuck-recovery episodes, 23 recoveries, 0 wedges.** Every armed cooldown
reached zero again; none was still armed when a run ended. Episode peaks:

| counter | meaning | observed maximum |
|---|---|---|
| `stall` | consecutive update units with the cooldown armed while `\|velocity\| < 0.5` | 229 |
| `nodecay` | consecutive units armed without the decay condition ever holding | 729 |
| `armed` | total units the cooldown was armed | 1196 |

The 229 is the one that answers the question, because it *is* the wedge — and
then it ends. Opponent 6 left the track at `(-96.1, -987.7, 2357.9)`, all wheels
off a surface, velocity pinned at `0.472` (below the `0.5` the decay branch
needs), cooldown at its full `120` with the reverse window already expired: 208
update units of a kart whose recovery provably could not re-arm. At tick 14790
DKR's own **out-of-bounds respawn** put it back on the track, and the cooldown
decayed `120 → 114 → 84 → 54 → 24 → 0` over six samples of ordinary forward
driving.

That respawn is the opponent's recovery, and it explains the asymmetry this item
started from: the autopiloted human at the crater lands *inside* the world,
grounded, so nothing ever respawns it and `unk215` stays armed for 18,677 frames.
An opponent that reaches the same state is out of bounds and gets picked up.

So on the evidence available today the deadlock is **not reachable for a CPU
opponent on this track**, which is why no player has reported a rival standing
still, and why the item stays "believed authored, hardware-unverified" rather
than becoming a player-visible defect. The gate is registered and fails closed:
if no opponent ever arms the cooldown it reports that the measurement stopped
working, rather than passing quietly.

### The existing tripwire

`platform/mdkr_adventure.c`'s `mdkr_autopilot_unstick()`, gated on the
test-only `MDKR_AUTOPILOT_UNSTICK` env var, zeroes `racer->unk215` for the
autopilot racer once it is *provably* immobile (moved less than 1 unit while
120 update-rate units — 60 frames — elapse, grounded, mid-race, checkpoint
already passed, input not blocked) and then lets the game's own recovery
drive from there. It never touches position, steering, collision, laps or any
verdict — see the function's own header comment in `mdkr_adventure.c` for the
full contract, and its production-behavior statement: "That is upstream
behaviour ... not a port defect, so it is not changed for production."
`tests/check_first_boss_progression.py` sets it only for the campaign arms
(the deliberately-broken collision-grid control does not set it, and must
keep failing to finish); every firing is logged via the `autopilotunstick:`
trace line, and the check asserts (`off_course` in the fourth-race
verification block) that it **never fires outside Hot Top Volcano** — any new
wedge site elsewhere would fail the gate rather than being silently absorbed.

That "only there" property used to be enforced by **one check's assertion, not
by the guard** — and `tests/check_race_multiplayer.py` also sets
`MDKR_AUTOPILOT_UNSTICK=1`, on Ancient Lake, deliberately and documented ("this
keeps a random AI wall wedge from erasing multiplayer coverage") but with no
equivalent of `off_course`, so a wedge there was absorbed rather than reported.
A hard scope in the guard would break that second use, so the hook now takes an
**opt-in** one:

    MDKR_AUTOPILOT_UNSTICK=1        any level (unchanged; multiplayer keeps this)
    MDKR_AUTOPILOT_UNSTICK=L<id>    only while courseId == <id>

`check_first_boss_progression.py` passes `L7`, so a wedge anywhere else on the
campaign route is no longer rescued at all — it fails the gate on its own merits
instead of being quietly absorbed and then reported. The `off_course` assertion
stays, because it is what proves the scope is doing something.

### Open question: hardware verification

Not yet confirmed on real hardware or in a cycle-accurate emulator (ares) that
the retail cart wedges identically at this crater jump once fed the same
(correct) AI table. If a hardware/ares run reproduces the same wedge, this
closes as authored-and-accepted. If it does not, the AI-table correction
changed reachability in a way retail's own AI never encountered, and this
needs an actual fix rather than a test-only workaround.

### Not done, and staying that way for now

No production behavior changes here — `mdkr_autopilot_unstick()` compiles into
every native build (it lives beside the other `MDKR_DRIVE_ROUTE`/`MDKR_OBJDUMP`
test hooks under the same `#ifdef NATIVE_PORT` block in `racer.c`) but is a
runtime no-op unless `MDKR_AUTOPILOT_UNSTICK` is explicitly set, which no
shipping launch path does. An opt-in `--restored`-tier unstick (labeled, never
default in `--pure`) is noted as a possible future enhancement, not
implemented.

## CLOSED, NOT A DEFECT: the zip-pad boost is the magnitude DKR authored — wave "zippad"

The register carried this since wave "closedloop": *a zip pad reaches 44.9 world
units/frame in an eight-racer Tracks race against 23.2 for the same pad in a solo
Time Trial — 3.2× vs 1.67× top speed — mechanism is DKR's own, magnitude never
checked against the ROM.* It is now measured. **Authored. No port defect, no fix.**

### First: neither historical number reproduces, and that is the first finding

The 44.9 came from a pad that `nav_to_time_trial_race.txt` + `MDKR_AUTOPILOT`
happened to drive over. **That route no longer touches a zip pad at all** —
measured with the new `[BOOST]` probe: zero frames of `SURFACE_ZIP_PAD` across the
entire race, no boost ever armed, and `check_race_drive.py` now reports max step
**14.3** where it recorded 44.9. The AI line moved when this same wave's parent
("closedloop") landed the ROM-faithful RNG seed, arctan table and table-based trig.

So the two figures were never a controlled comparison and cannot be made into one:
two different racing lines, at two different points of two different track modes,
with two different entry speeds and gradients, and two different "top speed"
denominators. This is exactly the trap [`tests/README.md`](../../tests/README.md#open-loop-vs-closed-loop--read-this-before-adding-or-editing-a-fixture)
documents — it bit the *measurement* this time rather than a fixture.

### The controlled measurement

`MDKR_ZIPPAD_BOOST=<frame>[:<ticks>]` (`objects.c mdkr_zippad_boost_hook`, no-op
unless set) arms player one, once, in exactly the state `racer.c:5727` arms it in
for `SURFACE_ZIP_PAD` on a car — `boostTimer = normalise_time(45)`,
`boostType = BOOST_LARGE`. Everything downstream is untouched decomp code, so this
measures the shipping boost with a deterministic trigger instead of a chaotic one.
Armed at frame 4000, cadence enhanced / one field, Ancient Lake, default car:

| arm | cruise | boost frames | peak &#124;velocity&#124; | peak step | ×cruise |
|---|---|---|---|---|---|
| **8-racer Tracks** | 12.27 | 45 | **22.357** | 24.31 | 1.98× |
| **solo Time Trial** | 12.67 | 45 | **22.336** | 24.98 | 1.97× |
| control `:15` | 12.27 | 15 | 20.880 | 20.89 | 1.70× |
| control `:120` | 12.23 | 120 | 22.358 | 24.55 | 2.01× |

**The two modes differ by 0.021 velocity units — 0.09%.** There is no racer-count
coupling in the boost. `normalise_time()` (`objects.c:1341`) has no framerate term
either: it is `(timer * 5) / 6` under PAL and the identity otherwise. And under
`--headless-frames` the pacer is `PACE_SYNTH` with a fixed field count
(`platform_sdl_min.c:1414`), so `updateRate` was identical in both arms — logged as
`rate=` in the trace and confirmed, which rules out the frameskip-compensation
suspect the plan listed.

### The boost saturates — which bounds the whole question

The `:120` control is the physically interesting row: holding `boostTimer` **2.7×
longer reaches the same peak, 22.358**. `traction = 2.0f` per update against the
drag term reaches terminal velocity well inside 45 ticks, so a zip pad cannot
produce an unbounded speed however long it is held. Its magnitude is bounded by
the authored physics, not by the timer. Recorded because it is the answer to
"could a boost ever run away here?" — it cannot.

### The reference used: code-level differential, not an ares trace

**No hardware trace was taken, and that is a deliberate, disclosed choice.** The
oracle pipeline in [`docs/ORACLE.md`](../ORACLE.md) needs an *instrumented* ares
built by `tools/prepare_ares_oracle.sh` (pinned commit + patch + compile); the only
ares on this machine is a stock `/Applications/ares.app` with none of the
injection/state-trace hooks, and no capture is committed. Building it was out of
proportion to what the question needed, because the differential is decisive on its
own:

**All 310 boost/velocity statements in `game/src/racer.c` are byte-identical to the
decomp baseline** recorded in `.decomp-baseline` (`3b2dd520`) — compared as an
ordered sequence over `boostTimer`, `boostType`, `BOOST_*`, `SURFACE_ZIP_PAD`,
`normalise_time`, `traction`, `racer->velocity` and `gCurrentRacerMiscAssetPtr`, so
a moved or reordered statement would show. `handle_racer_top_speed`,
`update_car_velocity_ground` and `handle_car_velocity_control` — the functions that
apply the boost — are byte-identical *whole*. The only port deltas anywhere near the
path are `GET_BOOST_TABLE()` (the LP64 `ASSET_MISC_20` accessor), the `MtxF sp60`
stack fix in the *plane* update, and a `D_800DCDA0` index clamp in the AI input
function — none of which is in the car boost path. `objects.c`'s only additions are
the port's own observability hooks.

The handbook §3 priors were checked and are clean: `boostTimer`/`boostType` are
plain `s8` runtime fields (`structs.h:1437`, `:1482`), never memcpy'd from a ROM
record, never reached through a cast or an offset, and `normalise_time(45)` cannot
overflow one; `gCurrentRacerMiscAssetPtr`'s backing tables are already in the
`dkr_misc_normalize_tables()` word-swap list.

### The gate

[`tests/check_boost_magnitude.py`](../../tests/check_boost_magnitude.py), registered
in `tools/run_checks.py` as `boost_magnitude`. It asserts the per-frame
`|racer->velocity|` trace (45 boost frames exactly, monotone ramp, plateau inside
`[21.8, 22.7]`, decay back under 0.75 of the plateau) and the cross-mode peak
difference, and it runs **both** perturbed-constant controls on every invocation —
`:15` trips four assertions, `:120` trips two, and if either passes the check fails
with `POSITIVE CONTROL BROKEN`.

The trace is asserted on velocity rather than on the position step **on purpose**:
the step carries cornering and gradient, so normalised by cruise the two fixtures'
ramps differ by up to 0.17 against a tolerance that would have to be 0.25, while the
velocity traces agree to 0.05. A check calibrated on the step would have been
another line-shaped fixture.

### Honest gaps

* **The plateau assertion stops at frame +34, not at the end of the boost.** The
  solo Time Trial line reaches a corner around +37 and sheds speed while the timer
  is still running (19.34, then 15.75) — a boost guarantees the throttle, not the
  road. Frames +25..+34 are the widest window in which both fixtures are still on
  the boost's own terminal speed. The decay half is covered by the tail assertion
  instead, which is normalised and therefore line-tolerant.
* **A real pad crossing is still not exercised by any check.** What is validated is
  the boost *state* and everything downstream of it, not the surface-detection code
  that arms it (`racer.c:5727`'s `surfaceType == SURFACE_ZIP_PAD` test). A route
  that reliably crosses a pad would close that, and would pair naturally with G2's
  human-line oracle route.
* **No hardware number.** The claim proved is "identical to the decomp, and
  internally consistent", not "identical to the ROM at the instruction level". If
  the instrumented ares is ever built, the `[BOOST]` probe is already the right
  shape to diff against it.

**Deliberately deferred.** The reported 44.9-vs-23.2 asymmetry does not exist:
armed identically, the boost peaks at 22.357 with eight racers and 22.336
solo, 0.09% apart, and saturates at 22.358 even when held 2.7× longer. All 310
boost and velocity statements in `racer.c` are byte-identical to the decomp
baseline, and the check is registered and gating. The residual — that no
committed route crosses a real pad — is a consequence of the fix, not a gap in
it: the AI line over a zip pad is chaotic with respect to any simulation
change, so the RNG and trigonometry corrections above moved every route that
used to cross one. A synthetic arm that sets the exact boost state the pad
sets measures the same quantity with a reproducibility a chaotic route cannot
offer. An ares trace would confirm the decomp matches the ROM at instruction
level; it would not change the magnitude, which is already proven identical
to the source the port is derived from.

## FIXED: three ROM-fidelity divergences, and the fixture class that was blocking them — wave "closedloop"

The "hasmaudit" wave below found three measured divergences from the ROM in
`platform/math_util_native.c` and **deliberately left all three switched off**,
because each one shifts every AI racing line and the fixtures could not survive
that. This wave removes the blocker and then flips them.

### The blocker, stated as a class

`tests/input_scripts/*.txt` are **open-loop input replays**: fixed button presses
at fixed frames. For a menu that is exactly right — nothing between the input and
the `menu_init:` assertion has any state to accumulate. For anything containing a
racer it is a trap: the trajectory is chaotic with respect to *any* change in the
simulation, so the fixture asserts against one line and fails whenever that line
moves, for reasons unrelated to what it tests. It had already forced one
recalibration (`MIN_FINAL_CP` 20 → 15 in P3.5) before this wave.

**The sweep** (CONTRIBUTING.md rule 6). Every `tests/*.py` was enumerated against
the scripts it runs and the closed-loop hooks it sets, and every
`tests/input_scripts/*.txt` against its consumers. 19 checks, 16 scripts. The
classification now lives in [`../tests/README.md`](../../tests/README.md#open-loop-vs-closed-loop--read-this-before-adding-or-editing-a-fixture)
and is summarised here:

| verdict | fixtures |
|---|---|
| already closed-loop (`MDKR_AUTOPILOT` / `MDKR_DRIVE_ROUTE`) | `check_race_finish_time`, `check_race_2p_split`, `check_track_sweep`, `check_vehicle_sweep`, `check_array_bounds_sweep`, `check_collision_untextured`, `check_adventure_race_loop` (driving only — see below) |
| **converted by this wave** | `check_race_drive`, `check_adventure_hub`, `check_save_failsafe` (its Taj case), and the two route-calibrated *assertions* inside `check_collision_gridmask` / `check_boss_win_verdict` |
| deliberately left open-loop, with the reason | the nine `nav_*` menu routes (no racer between input and assertion); `race_drive_long` and `race_drive_time_trial` as used by `check_texture_lineswap` / `check_rom_revision` / `check_determinism`, which compare **pixels between two arms of the same route**, so the route only has to be identical to itself |

The sweep also turned up something it was not looking for: **five of the nine
`nav_*` scripts had no automated consumer at all** — their expected terminal state
lived only in a table in `tests/README.md`, to be checked by a human with `grep`.
`tests/check_nav_fixtures.py` now runs all nine.

### FIXED 1: the boot RNG seed

`gCurrentRNGSeed` / `gPrevRNGSeed` are now `0x5141564D` / `0x5141564D` ('QAVM'),
the values in the `.data` section of `game/src/hasm/ido/math_util.s`, instead of
the invented `0x00051234` / `0`. `MDKR_RNGSEED=legacy` restores the old pair.

### FIXED 2: `gArcTanTable` rounds instead of truncating

491 of the 1025 live entries were one unit low. Rounding reproduces
`EXPORT(gArcTanTable)` exactly (0/1025 differ, FNV `0xe0d93ef8`).
`MDKR_ARCTAN=trunc` restores the truncation.

### FIXED 3: `sins_s16` / `coss_s16` / `sins_2` / `sins_f` / `coss_f` walk the ROM's table

The port evaluated all five with libm. The ROM interpolates a 1025-entry
quarter-turn table. `XLEAF(sins_s16)` (math_util.s:2432) is now transcribed into
`platform/math_util_native.c`, and `gSineTable` is generated — **with no ROM data**
— as `round(sin(i*pi/2/1024) * 0x8000)`, which matches all 1025 `.half` entries.
`sins_f`/`coss_f` route through it exactly as `LEAF(sins_f)` does (`jal sins_s16`,
`cvt.s.w`, `mul.s` by 1/0x10000) rather than calling libm themselves — before this
they did not even agree with `sins_s16`. `MDKR_TRIG=libm` restores the
approximation.

Every `srl` in that routine is transcribed as an unsigned shift, not `>>` on a
signed value: `coss_s16` adds `0x4000` to a sign-extended `s16`, so the argument is
genuinely negative for a whole quadrant, and `srl`-vs-`sra` is precisely the
transcription defect shape the "hasmaudit" wave was hunting.

**Verification.** `tests/check_math_tables.py` re-implements the same assembly walk
in Python, over `gSineTable` as parsed out of the `.s`, and requires the binary's
FNV-1a over **all 65536** `sins_s16` results to match it bit for bit — 0x2412893d
both ways. Two independent readings of the same assembly agreeing across the whole
domain is what makes this more than a table diff. The measured cost of the old
behaviour: libm disagreed on **44168 of 65536 angles (67.4 %)**.

### What the four converted fixtures actually assert now

| fixture | was | is |
|---|---|---|
| `check_race_drive` | replayed `race_drive_long.txt` (throttle held, stick LEFT 25 frames in every 120) and asserted cp ≥ 15 / lap ≥ 1 against that one line | `MDKR_AUTOPILOT=1` over `nav_to_time_trial_race.txt` (the same navigation with the driving inputs dropped); cp ≥ 30 / lap ≥ 2, measured 48/2 |
| `check_race_drive`'s teleport test | absolute `MAX_STEP = 40.0` units/frame | **shape, not speed**: `MAX_ACCEL = 40` on the frame-to-frame *change* in step length (measured 5.8 healthy) plus a generous `MAX_STEP = 150`. The old cap fired on a real 44.9-unit zip-pad boost |
| `check_collision_gridmask` "≥ 2 racers lost the ground for ≥ 60 frames" | a hand-timed 60, from one measurement of one line | `> MAX_FIXED_AIRBORNE`, the ceiling the *fixed arm* is required to stay under, **and** ≥ 2× the same racer's fixed-arm figure. A paired statement about the two arms, so it holds for any line |
| `check_collision_gridmask` / `check_boss_win_verdict` win arm | `MDKR_BOSS_SLOW=1` (`velocity *= 0.15f` on the boss) | `MDKR_BOSS_WIN=1` — writes `racer->finishPosition = 1` at the finish and nothing else |
| `check_adventure_hub` | the script's blind RIGHT-pulse train; cp ≥ 5 / lap ≥ 1 on the hub's AI-node spline | a 44-waypoint `MDKR_DRIVE_ROUTE` tour sampled from that route's own trace, asserting **waypoints actually reached** (≥ 20, measured 26) |
| `check_save_failsafe` case 5 | the same blind train | imports `check_adventure_hub.HUB_TOUR_ROUTE` — one definition of the tour in the tree |
| `check_adventure_race_loop` | three laps, `fin=1`, and motion asserted over the whole race-to-lobby window | `MDKR_FORCE_LAPS=1`, `fin=1` kept as a hard assertion, and motion scoped to **load → finish** |

**Why `MDKR_BOSS_SLOW` had to go, in one measurement.** The human is driven by
DKR's own AI, and the AI paths relative to the field, so crippling the boss moved
the *human's* line too. With the ROM-faithful math it moved it off the Fire
Mountain summit: y = 4649 at frame 8700, then 84 frames with all four wheels off
the ground and the pitch ramping 3317 → 4133, no finish, and
`racer_boss_finish()` never reached **at any budget**. The same route without the
slowdown finishes in every arm measured (y = 4868, cp = 36, `fin=1`). Reaching an
outcome by writing the one field the code branches on beats perturbing the physics
until the branch happens to be taken.

### Proof the positive controls still fail in their broken direction

This is the part that would make the whole wave worthless if it were wrong.

**`check_collision_gridmask`** — `MDKR_GRIDMASK=off` restores the tautology, on the
same binary, and after the conversion it still reproduces the defect in both math
arms:

| arm | truncated | maxCandidates | longest `gw=0` run | peak y | `fin=` |
|---|---|---|---|---|---|
| fixed, ROM-faithful | 0 | 275 | 27 | 4868 | 1 |
| **broken**, ROM-faithful | **73** | **500** (the cap) | **285** | 2119 | **0** |
| fixed, superseded math | 0 | 310 | 27 | 4868 | 1 |
| **broken**, superseded math | **73** | **500** | **301** | 2118 | **0** |

Both racers still lose the ground in the broken arm and neither does in the fixed
arm; the negative control (Ancient Lake, which never saturates) is still
byte-identical across the two arms over 3857 racer rows.

**`check_race_drive`** — `ASSET_MISC_8` was removed from
`dkr_misc_normalize_tables()`'s word-array list (game/src/objects.c), i.e. the
denormal-divisor bug was deliberately reintroduced, and the converted check fails
on six assertions at once: exit code −6; `[FATAL] update_player_racer: non-finite
position (nan, inf, nan) ... lateral=-inf`; 14 in-race frames of the 1000 required;
checkpoint 0 of 30; lap 0 of 2; one dumped frame of four. The autopilot reaches it
*sooner* than the old route did, because the AI steers on the first frame out of
the grid instead of waiting for a scripted pulse at frame 3000.

**`check_boss_win_verdict`** — `--break-invariant` still fails, on all four of its
assertions.

**`check_math_tables`** — the legacy arm is still the divergence: 491/1025 arctan
entries low, libm wrong on 44168/65536 angles, seed `0x00051234`.

### The fidelity payoff, measured — and it is NOT there

> **Historical — superseded by the reference-replay oracle.** This section's
> "no oracle route drives a real racing line" finding predates the
> `oracle_reference_replay.py` work (`d2808f9`, 2026-08-01): `race_state_oracle`
> now replays the ROM's own observed update widths and input states over a real
> Ancient Lake lap. [`docs/ORACLE.md`](../ORACLE.md) is the current, authoritative
> source on oracle route coverage; treat the "what this wave did NOT do" bullet
> below about racing-line coverage as historical rather than current status.

`tools/run_oracle.sh` scores our frames against the real ROM in instrumented ares.
Six routes, before (`MDKR_RNGSEED=legacy MDKR_ARCTAN=trunc MDKR_TRIG=libm`) and
after (the new defaults), same ares captures both times:

| route | before | after | delta |
|---|---|---|---|
| `boot_to_title` | 0.9976 | 0.9976 | 0 |
| `charselect_anim_period` | 0.9568 | 0.9570 | +0.0002 |
| `title_to_audio_options` | 0.9177 | 0.9178 | +0.0001 |
| `title_to_character_select` | 0.9086 | 0.9086 | 0 |
| `title_to_options` | 0.8548 | 0.8547 | −0.0001 |
| `race_karts` | 0.6365 | 0.6361 | −0.0004 |

**Every delta is within run-to-run noise.** That is a real result and it should not
be dressed up: these three corrections make the port compute the ROM's numbers,
but they do not move pixel similarity on any route the oracle can currently
capture. The reason is visible in the table — the oracle's routes are frontend
screens plus one static kart-select shot, and their scores are dominated by
texture decode, layout and text rendering. The one route with a racer in it,
`race_karts`, is a stationary grid shot. Nothing in the current oracle route set
exercises an AI racing line over enough frames for an LSB-level arctan difference
or a different RNG sequence to show up as pixels.

So the justification for these three is not the oracle score; it is that the port
was demonstrably computing different numbers from the ROM at 98 `rand_range()`
sites, 72 `atan2s()`/`arctan2_f()` sites and every trig call in the game, and that
is now checkable to the bit (`tests/check_math_tables.py`). **A route that drives a
real lap and compares against ares would be the measurement that could show a
payoff, and it does not exist yet** — that is the gap this wave leaves behind.

Also worth recording, since it is the opposite of a payoff: `title_to_audio_options`
scores **0.9178**, not the 0.861 the "oraclefix" wave recorded as the weakest menu
route. That improvement is not from this wave (both arms score it identically); it
came in earlier and the number in the older section is stale.

### What this wave did NOT do

- **No oracle route that drives a real racing line.** This is the gap that makes
  the payoff unmeasurable (above). It needs an ares input script that starts a
  race and holds the throttle, plus marks deep enough into the lap for the two
  runs to have diverged if they were going to.
- ~~**The Adventure three-lap finish is recorded, not diagnosed.**~~ Superseded
  by wave `adventurefinish` below: the one-lap bypass is gone, the identity error
  is measured, and both persisted outcomes are gated.
- **`race_drive_long.txt` was not deleted or converted.** Three checks compare
  pixels between two arms of that route, where the trajectory is irrelevant, so
  its open loop is correct. It is no longer driven by `check_race_drive`.
- **`MDKR_BOSS_SLOW` was left in the tree** (`platform/stubs_dkr.c`,
  `game/src/vehicle_tricky.c`) although no check uses it any more. Removing it
  touches boss race-finish code another wave is working in; the two checks that
  used it now `env.pop` it explicitly so an inherited value cannot bring the
  trajectory shift back.
- **The zip-pad boost magnitude was not verified against the ROM** — measured and
  recorded above, and `check_race_drive`'s teleport test was rewritten so that it
  does not depend on the answer either way. **SUPERSEDED 2026-07-31 by [wave
  "zippad"](#closed-not-a-defect-the-zip-pad-boost-is-the-magnitude-dkr-authored--wave-zippad):
  authored, no defect. Note the 44.9 recorded here no longer reproduces — this very
  wave's corrections moved the AI line off that pad — which is why the closing
  measurement had to arm the boost instead of driving over one.**
- **`--expect-fail 9` is no longer used** — level 9 (`obj_loop_snowball`'s NULL
  `animatedObject`) completed a 12000-frame autopilot race with `maxcp=80` in BOTH
  math arms. Checked in both arms specifically to rule out the new RNG sequence
  merely hiding it. The old diagnosis was never reproduced on current source and
  is retracted rather than masked: the release command now requires all 20 tracks
  to pass.

### RETRACTED: the Adventure three-lap race finishes normally — wave "adventurefinish"

The old section below is retained because it captures the evidence that led to
the report. Its conclusion is retracted. The missing measurement it requested—
finish position and stable human identity — settled it.

**Old evidence (superseded interpretation):** on the Adventure hub → Dino
Domain lobby → Ancient Lake route with `MDKR_AUTOPILOT=1`:

| | superseded math | ROM-faithful math |
|---|---|---|
| human's `lap` (`race_check_finish`, `rlap=`) | 3 | 3 |
| human's `lap` (`update_player_racer`, `lap=`) | froze at 2 — the racer had finished | kept going to 3 |
| race clock at the plateau | 5222 | **4771** |
| `racer->raceFinished` (`fin=`) | 1, at frame 12712 | **never**, over 1438 frames of frozen clock |
| return to the lobby (entrance 3) | frame 13492 | frame 13694 |
| checkpoints crossed | 53 | 54 |

Both arms returned normally and neither crashed. The mistake was calling
`(*gRacers)[0]` "the human": `gRacers` is starting-grid order. The trace observed
an AI and then combined that observation with a human-race narrative.

**Resolution:** the probe now follows `gRacersByPort[PLAYER_ONE]`, publishes
`racerIndex`, `playerIndex`, and `finishPosition`, and samples after natural
finish assignment. At the original checkpoint the human completed lap 3 at
frame 12502 / clock 5027 and finished fifth, correctly persisting Ancient Lake
as visited with no race balloon.

The runtime-boundary wave moved the current natural result to first at frame
12186 / clock 4711, demonstrating that a particular natural place was itself an
unstable test oracle. At `424c4d6` the full-length fixture instead runs symmetric
post-finish win/loss controls from isolated fresh saves. Both must report the
same natural place/frame/clock/lap; only then may one request first and the other
non-first. Production Adventure code must independently decode to status 2 /
`(2,1,0,0,0,0)` and status 1 / `(1,0,0,0,0,0)` respectively. Debug, Release,
ASan, and alignment builds pass, as do the Time Trial and boss verdict gates.

No production race/save behavior changed. This closes a false diagnosis and a
real coverage hole.

### Deliberately deferred: Ancient Lake long-horizon parity

Replaying the ROM's observed update widths and input states (`race_state_oracle`'s
`reference_replay` arm, [`docs/ORACLE.md`](../ORACLE.md)) makes checkpoint
clocks 0 through 3 exact and moves the first five-unit separation from clock 18
to clock 767, which classifies the early mismatch as timestep partitioning
rather than a physics divergence. Beyond that point sub-unit floating-point
differences compound in an open loop with no corrective input — the expected
and unavoidable behavior of any non-bit-exact re-implementation driven over
hundreds of seconds. The diagnostic is deliberately kept red and deliberately
excluded from the automated suite: it requires an owned ROM and a locally
built instrumented emulator, and a green threshold would either be arbitrary
or would silently mask a real regression. It is a developer instrument, not a
player-facing property; nothing a player does is affected by where an
open-loop replay diverges at clock 767.

### None of the three is `#ifdef NATIVE_PORT`-gated, and none needs to be

All three live in `platform/math_util_native.c`, which is port-only by
construction — it exists to supply the symbols from a `.s` this build does not
assemble. Nothing under `game/` changed. The corrections make the port *more*
faithful to the ROM, so even if they had been in `game/` they would be
transcription fixes and not host adaptations, exactly as with the grid-mask and
`vec3f_rotate_py` fixes.

## FIXED: a one-shot cutscene replayed for ever, because its latch was undefined behaviour the optimiser deleted — wave "keyshift"

> Reported from the published browser build: *"I collected the Key on the first
> race, and now the key animation is playing after EVERY race."*

Correct, reproduced exactly, and **the cause is not in the save logic at all** — it
is a C undefined-behaviour shift that clang deletes at `-O2`. The native build
defaults to **Debug** and the web build to **Release** (`CMakeLists.txt:6-14`), so
this defect exists only in the build players actually run, which is why every native
check was green.

### Mechanism

`level_load()` (`game/src/game.c`) decides whether to redirect a world-hub load
into the "your key unlocked the Challenge door" cutscene:

```c
if (settings->keys & (1 << var_s0) &&
    !(settings->cutsceneFlags & (CUTSCENE_DINO_DOMAIN_KEY << (var_s0 + 31)))) {
    ...
    settings->cutsceneFlags |= CUTSCENE_DINO_DOMAIN_KEY << (var_s0 + 31);
```

`var_s0` is the world, 1..4. **The shift count is therefore 32..35.** The ROM means
it: MIPS `sllv` takes the count from the low five bits of the register, so `<< 32`
is `<< 0`, and the four worlds land on `0x4000/0x8000/0x10000/0x20000` exactly as
`structs.h` documents. In C a shift count `>=` the operand width is undefined
behaviour, and this is precisely the shape an optimiser sees through: the enclosing
`world > WORLD_CENTRAL_AREA && world < WORLD_FUTURE_FUN_LAND` pins the count to a
range that is *entirely* `>= 32`, so clang concludes the value is poison and folds
it to **zero**.

A mask of zero breaks the latch in both directions at once:

* `!(cutsceneFlags & 0)` is always true → **the gate always fires**;
* `cutsceneFlags |= 0` is a no-op → **nothing is ever recorded**, not in RAM and
  not in the save.

So after the key is collected the cutscene plays on every entry to that world's
hub — i.e. after every race in it — for ever, and a browser reload cannot help
because the latch never reached IndexedDB either.

At `-O0` the identical source is **correct**: clang emits a real register shift and
arm64 (like wasm32, like MIPS) masks the count modulo 32.

### Measured

Same route, same injected save (`keys = 0x02`, Dino Domain key collected), same
us.v80 ROM; the only variable is `CMAKE_BUILD_TYPE`. Trace from the probe now
permanently at the gate:

| build | first entry to the world hub | returning from the race |
|---|---|---|
| Debug `-O0` | `cutsceneFlags=0x0` **mask=0x4000** → fires | `cutsceneFlags=0x44000` mask=0x4000 → **latched, silent** |
| Release `-O2` | `cutsceneFlags=0x0` **mask=0x0** → fires | `cutsceneFlags=0x40000` mask=0x0 → **FIRES AGAIN** |

and the user-visible difference is one extra load of the world hub on the way back,
which *is* the cutscene playing:

```
fixed  : level 12 @10709 -> level 0 @11057                    (one hub load)
broken : level 12 @10709 -> level 12 @11435 -> level 0 @11783  (cutscene, then hub)
```

The saved EEPROM afterwards: `cutsceneFlags = 0x00044000` fixed (bit `0x4000`
present) vs `0x00040000` broken.

**Independent confirmation that this is target-independent, not an arm64 quirk.**
The fold happens in LLVM IR, before any target lowering. Compiling the gate's exact
shape for `--target=wasm32 -O2` — the browser's own target — the whole flag
computation is *deleted*:

```llvm
define hidden void @step(ptr %0) {          ; the raw `<< (world + 31)` form
  %2 = load i8, ptr %0
  %3 = add i8 %2, -1
  %4 = icmp ult i8 %3, 4
  br i1 %4, label %5, label %8
5:  %6 = load i32, ptr @fired
    %7 = add nsw i32 %6, 1
    store i32 %7, ptr @fired                ; the cutscene fires
    br label %8                             ; ... and @flags is never loaded,
8:  ret void                                ;     never tested, never stored
}
```

The masked form keeps `shl nuw nsw i32 16384, %9` and the full load/test/store.

### Fix

`DKR_SHL32(x, n)` in `game/include/macros.h` — `((u32)(x) << ((u32)(n) & 31))`
under `NATIVE_PORT`, and the untouched `((x) << (n))` otherwise, so a matching ROM
build is bit-identical and the port stops depending on UB surviving `-O2`. It
reproduces `sllv` exactly for every count, including counts above 63, so it is a
faithful transcription rather than a guess about intent.

Applied at all four sites (see the sweep below). Defining `MDKR_SHL32_CONTROL`
restores the plain shift and nothing else; that is how the control build is made.

### The class sweep — `-fsanitize=shift-exponent`

The shape is *"a shift whose count the ROM expected the hardware to mask"*, and the
mechanical instrument is UBSan's `shift-exponent`, which reports every executed
shift with a count `>=` the operand width. The prior wave ("tajprogress") had
already predicted this failure mode and proposed exactly this detector; it was not
built then. Built now, over four routes chosen to cross a world hub with a key
collected, Timber's Island at the Taj balloon threshold, a water track and a Tracks
race:

| site | expression | reached on | verdict |
|---|---|---|---|
| `game.c` key gate + latch | `CUTSCENE_DINO_DOMAIN_KEY << (world + 31)` | `adv_key` | **the report.** Folded to 0 at `-O2`. FIXED |
| `game.c` boss-approach latch | `8 << (settings->worldId + 31)` | `adv_key` (every world-hub load) | **also folded to 0 at `-O2`** — the "boss door is now open" cutscene had the same defect, unreported only because it needs 4 world balloons. FIXED |
| `objects.c` Taj offer | `settings->tajFlags \|= 1 << (j + 31)` | `adv_taj` (5 total balloons) | **the whole statement is DELETED at `-O2`, on arm64 and wasm32 alike.** FIXED — see the retraction below |
| `waves.c` `obj_wave_height` | `var_t0 <<= (log->unk2 + 0x1F)` | **all four routes**, i.e. every race | NOT folded, and hottest of the four. clang turns the `if/else` into a `select` and so speculates the shift with a count range of `[31, 286]` that includes the legal 31, which is what stops the fold. Latent, FIXED anyway |

> **Upstream caught up on the `waves.c` row (2026-08-07).** Upstream now spells
> that shift `<< (unk2 - 1)` — the same semantics this wave derived — so the
> `c6695703` sync adopted upstream's expression. `DKR_SHL32` is still wrapped
> around it: `unk2` is `u8`, so a bare shift is still UB for counts >= 32, and
> `(unk2 + 0x1F) & 31 == (unk2 - 1) & 31` for `unk2 > 0`, making the change
> byte-identical. The other three sites remain this port's own fixes.

Both directions of the instrument were run, because a sanitizer sweep reporting
zero is indistinguishable from a dead one:

```
fix in place      : 4 routes, 0 distinct shift-exponent sites
fix reverted      : 6 distinct sites  (game.c:528/532/535, game.c:629,
                    objects.c:1822, waves.c:2321)
```

`waves.c:2321` is reported by **every** route, so it doubles as the sweep's living
sentinel: if a future run reports nothing on a reverted build, the instrument has
died.

### RETRACTION: this is the real root cause of wave "tajprogress"

That wave attributed "a genie test we already completed, replayed on every entry to
Timber's Island" to *two independent `FS.syncfs` coalescers, so one of the two
progress flushes could be lost*, and repaired it at the save-read seam with
`tajFlags |= (tajFlags >> 3) & TAJ_FLAGS_UNLOCKED_A_CHALLENGE`. It also probed
`1 << (j + 31)` directly, measured `0x1/0x2/0x4`, and concluded the site was
*"right on this host — by luck"*.

That probe was run in a **Debug** build. In any optimised build the statement does
not exist: the emitted block calls `set_taj_voice_line()`, `set_taj_status()`,
`set_next_taj_challenge_menu()` and `safe_mark_write_save_file()` and **never
touches `tajFlags`**. So the OFFERED half was never set in the browser at all, and
the save was flushed in the "beaten but never offered" state by design, not by a
lost sync.

The earlier repair is still correct and is deliberately left in place — it is the
belt to this braces, and it is a no-op on every consistent save — but it only
covers *beaten* challenges. A challenge that was offered and **not** beaten (the
player loses, or quits) was still re-offered on every visit, because OFFERED was
never stored, and no save-read repair can reconstruct that. `DKR_SHL32` fixes the
write, which is the part that was actually broken.

The general lesson is recorded as a new shape in `DEVELOPER_HANDBOOK.md` §3: **a probe run at
`-O0` says nothing about a defect whose mechanism is optimisation.**

### The SaveFile writer/reader sweep

`CONTRIBUTING.md` rule 6 in general form for this defect: *a one-time presentation
or unlock whose "already done" state is persisted but is not consulted by the thing
that performs it.* Every field in `SaveFile` (`game/include/save_layout.h`) was
enumerated and its write-set and read-set built by `git grep` plus the `save_data.c`
accessors, looking for rows where the two disagree about which consumers exist.

**Coverage: exhaustive over the whole 512-byte image** — all 11 runtime progress
fields (~90 individual bits/bit-groups), plus the per-slot checksum and the two
`CourseRecords` blocks, so every byte of `SaveBuffer` is accounted for rather than
sampled. **Two live disagreements found (both mine, both fixed), five latent
structural issues, four questions that cannot be answered from source.**

| field | round-trip | verdict |
|---|---|---|
| `cutsceneFlags` (19 live bits) | SOUND — 19 disk fields map onto bits 0..18 with no gap or reorder; 32 bits both sides | **the defect.** No *other* bit is set-but-never-tested or tested-but-never-set |
| `keys` (8 bits) | SOUND, 8 bits both sides | SOUND. The `save_layout.h:182-183` comments swap Sherbet/Snowflake against `enums.h`; every code path treats key bit *n* as world *n*, so it is a stale comment pair, not an index disagreement |
| `tajFlags` (6 bits) | SOUND | **the defect** (write side). Two parallel triples, and the OFFERED write was compiled away |
| `courseFlagsPtr` low half (34 × 2-bit course ladder) | **asymmetric**: decoded as a 3-step ladder, re-encoded as a 3-term count | SOUND today; structurally the `tajFlags` shape. See below |
| `courseFlagsPtr` high half (6 × 16-bit door/balloon/trigger latches) | only the 6 hub indices are serialised | SOUND for hubs. Five writers index *any* level, so every latch outside a hub is WRITTEN-NEVER-READ (measured: `courseFlagsPtr[38] = 0x440003`, two dead trigger bits on a boss arena) |
| `trophies` (5 × 2 bits) | SOUND | SOUND for groups 1-4 (idempotent keep-best, flushed at the write). Group 5 (FFL) is read but no writer was proven — needs level data |
| `bosses` (12 bits) | SOUND | SOUND, and explicitly **not** the `tajFlags` shape: the rematch write is *nested inside* the first-win branch in the same function and frame (`vehicle_tricky.c:376-383`), so "rematch without first win" is unreachable — which is why gates consulting only the rematch half are still sound |
| `balloonsPtr[0..5]` | SOUND | The two real award sites write total and per-world in one statement pair. Three writers move the total alone (`menu.c:8285` free-balloon cheat, `menu.c:4874/4920` slot copy) — ROM-faithful, and bounded: they satisfy only total-gated readers, never per-world ones |
| `ttAmulet`, `wizpigAmulet` | SOUND | SOUND. **Both increments are guarded by persisted state, not a RAM global** — `ttAmulet` by `RACE_CLEARED` on the challenge course, `wizpigAmulet` by the rematch `bosses` bit — and both clamp at 4, so each piece is awarded exactly once |
| `filename` | SOUND, 16 bits both sides | SOUND. `name : 15` in the struct is documentation-only; 3 chars × 5 bits = 15 bits of payload and `sizeof(SaveFile) == 40` either way |
| `SaveConfig` (`unlockedAdv2`, `unlockedDrumstick`, `language`, 20 × TT course, `subtitles`) | SOUND — same 56-bit nibble checksum on both sides | SOUND except **`subtitles`, which is off by one**: the struct names bit 24, the code uses bit 25, because a one-bit "default high" field is missing from the struct |
| `checksum` (16 bits, per slot) | SOUND — `(5 + sum(bytes[2:])) & 0xFFFF` on both sides | Not progress state, but it is the gate in front of all of it. Already covered in both directions by `check_save_failsafe.py` cases 1-3, and this wave's own check builds its images with the same sum |
| `CourseRecords fastLaps` / `courseTimes` (2 × 192 bytes, outside `SaveFile`) | SOUND — own 16-bit sum per block | Out of scope for the one-shot-latch shape (they are best-times, not "already done" flags) and already covered end to end by `check_race_finish_time.py`, which records a time and reads it back after a restart. Listed so the enumeration is exhaustive over all 512 bytes |

**Why a name-based grep would never have found this.** Counted mechanically
(`grep -rlw` per macro, excluding `structs.h`): of the **17** named `CUTSCENE_*`
flag macros, **12 are never used as a `cutsceneFlags` mask anywhere** — all five
`*_BOSS`, all four `*_BOSS_2`, and three of the four `*_KEY`. Their bits are
reachable *only* through the computed `8 << (worldId + 31)` (plus its `var_s0 <<= 5`
sibling for the rematch group) and `<< (world + 31)`. Grepping the flag names finds
a gate that looks unreferenced and a latch that looks unwritten; the bug is
invisible to it, which is why the mechanical instrument had to be a sanitizer over
the *shift*, not a search over the *names*.

Two disk bits have no macro at all: `0x1000` (`sceneWorld5Boss2`), dead; and
`0x40000` (`sceneWorld5Key`), which is live and mislabelled `// Unused` in
`save_layout.h:207` — it is really the "the balloon cutscene may now be skipped"
latch (`objects.c:7050/7059`, the `gRaceEndStage == 4` A-button path).

One false friend, and it is the reason the 12/17 count needs stating carefully:
`CUTSCENE_SHERBET_ISLAND_BOSS`'s single occurrence outside `structs.h`
(`objects.c:9306`) is **not a flag test**. It compares a cutscene *id* against
`(CUTSCENE_SHERBET_ISLAND_BOSS | CUTSCENE_ADVENTURE_TWO)`; the value `0x14` is
right, the vocabulary is borrowed from the wrong enum, and it will mislead the next
reader who greps that name.

### Left alone on purpose, with measured reachability

* **`object_functions.c:3463`** — `0x10000 << balloonID` with **no `>= 0` guard**,
  where the door (`:3599`) and trigger (`:3954`) sites sharing the same 16-entry
  allocator (`objects.c:289`) both have one. `obj_init_goldenballoon` leaves
  `balloonID == -1` when `func_8000CC20` exhausts the pool, and the hub alone has
  7 balloons + 7 doors competing for those 16 slots. A negative shift count is UB.
  Not fixed because adding the guard changes behaviour rather than transcribing it,
  and no route was found that exhausts the pool.
* **The `courseFlagsPtr` ladder/count asymmetry.** `{CLEARED}` reloads as
  `{VISITED}`, and `{VISITED,SILVER}` reloads as `{VISITED,CLEARED}` — a *lost*
  silver unlock plus a *fabricated* clear. Unreachable today only because
  `objects.c:6927/6932` makes SILVER unreachable without CLEARED and
  `objects.c:1394` stamps VISITED at level load. The invariant is real, implicit
  and unasserted.
* **Two negative shifts**: `object_functions.c:610` and `objects.c:2314`, both
  `trophies >> ((worldId - 1) * 2)` in effect, UB when `worldId == 0`. Whether a
  trophy cabinet exists in a `worldId == 0` level is level data, not source; the
  `shift-exponent` sweep did **not** report either site on the four routes, so
  neither is reached by them.
* **`menu.c:14853`** — `for (flag = 3; i < 16; flag <<= 2, i++)` overflows a signed
  `s32` past bit 31. Benign because `trophies` is 16 bits so those iterations can
  never match; the bound should be 5, not 16.
* **`trackmenu_set_records()`** (`menu.c:3188`, called from `:7298` and `:7567`) — a
  third writer of `keys`/`cutsceneFlags`/`trophies`/`bosses`/`courseFlagsPtr` that
  ORs all three save slots into the live `Settings` ("furthest progress of all
  files"). Readers cannot distinguish merged state from slot state. It appears safe
  because `populate_settings_from_save_data` calls `clear_game_progress` first and
  `safe_mark_write_save_file` refuses while in Tracks mode — but `menu.c:7567`
  fires the merge during GAME SELECT, and the three `force_mark_write_save_file`
  callers do not check. Not proven reachable; recorded because it is exactly "the
  write set and the read set disagree about which consumers exist".
* **`menu.c:9602/9610`** read `courseFlagsPtr[gTrackIdForPreview]` with no `-1`
  guard, where a locked track-select cell is `-1` and only the A-press is guarded
  (`:9217`). On a fresh save with the cursor on a locked cell this reads
  `courseFlagsPtr[-1]`, i.e. the tail of the `Settings` struct. Not confirmed
  reachable.
* **`trophies` group 5** (FFL, bits 8-9) is read (`menu.c:14855`, `objects.c:2314`)
  and no writer was proven; it needs `worldId == 5` at `menu.c:12595`. Both
  "all trophies" gates use `& 0xFF` and deliberately ignore it.

### Check

`tests/check_key_cutscene_once.py` — headless and muted, injects a save with the
Dino Domain key collected and nothing shown yet, then drives
`adventure_race_loop.txt` with the same closed-loop `MDKR_DRIVE_ROUTE` steering
`check_adventure_race_loop.py` uses. Six assertions: the route really ran (6808
in-race rows, 62286.9 units of path — coverage, so nothing below can pass
vacuously); the key gate is evaluated exactly twice; **the latch mask is
non-zero**; the first evaluation sees the latch clear and the second sees it set;
exactly one load of the lobby on the return leg; and the latch is present in
`save/eeprom.bin` afterwards.

**It must be run against an optimised build** — `--build build-rel` — because a
Debug build cannot observe the defect at all. That is stated in the file and is the
single most important thing about it.

Proven in both directions against `-DMDKR_EXTRA_C_FLAGS=-DMDKR_SHL32_CONTROL` at
`CMAKE_BUILD_TYPE=Release`, 4 of 6 assertions failing while the two coverage
assertions still pass (so the control fails for the right reason, not because the
route broke):

```
key: the key-cutscene latch mask is 0x0 at evaluation 1, expected 0x4000
key: the cutscene fired again on the return to the world hub -- cutsceneFlags
     0x40000 still lacks 0x4000 after it had already played once
key: 2 loads of levelId 12 on the return leg, expected 1 -- frames [10709, 11435]
key: save slot 0 cutsceneFlags 0x40000 lacks bit 0x4000 after the run
```

## NOT A DEFECT: the boss win cutscene was skipped because the win was already recorded — wave "bossverdict"

> Reported from the published browser build (source `45b28e8`), after the volcano
> fall-through fix landed: *"I won the first boss race and got **no win cutscene, no
> amulet piece**, and was dropped back in front of the boss door **as if it had just
> been unlocked again**."* With the probe line from their own console:
>
> ```
> bossfinish: finishPos=1 playerIndex=0 courseId=38 worldId=1 bosses=0x2
>             courseFlags=0x440003 cutsceneLevel=57 timer=1 raceFinished=1
> ```
>
> **The port is behaving correctly.** That line says the win had ALREADY been
> recorded before this race, so `racer_boss_finish()` took the ROM's own re-race
> path, which by design presents nothing. Reproduced field-for-field from an
> unmodified binary. Asserted from now on by `tests/check_boss_win_verdict.py`.

### What the probe's numbers mean

Every field decoded at the one place it is read (`game/src/vehicle_tricky.c:269`):

| field | value | meaning |
|---|---|---|
| `finishPos=1` | 1st | `racer->finishPosition` of the **player** — `update_tricky` passes `firstRacerObj->racer`, not the boss's |
| `bosses=0x2` | `1 << worldId` for world 1 | **the Dino Domain boss bit is ALREADY SET.** Only two things write `settings->bosses`: this function's win branch and the save unpacker (`save_data.c:401`, 12 packed bits). So a win branch had already run and been saved |
| `courseFlags=0x440003` | `courseFlagsPtr[38]` | bit 0 `RACE_VISITED`, **bit 1 `RACE_CLEARED`**, plus per-level flags in bits 16+ (`0x10000 << n`, here arena triggers 2 and 6) |
| `cutsceneLevel=57` | `i` | the `ASSET_MISC_67` lookup 38 → 57, correct and populated |
| `timer=1` | `gTrickyCutsceneTimer` | the one-shot latch (`vehicle_tricky.c:226`), so this really is the single call for this race |
| `raceFinished=1` | — | the finish was seen |

So **the win was recorded and the presentation was skipped** — not "the win was
lost". Those two readings imply completely different fixes, and it is the first
one.

### The branch that skips it

`game/src/vehicle_tricky.c:360-372`, reached because bit 1 was already set:

```c
if (settings->courseFlagsPtr[settings->courseId] & 2) {     /* RACE_CLEARED */
    if (finishPos == 1) {
        level_transition_begin(4);
        instShowBearBar();
    } else { ... }
    arg1_ret++;
    *sceneTimer = arg1_ret;
    return;                     /* <- no level_properties_push, no save write */
}
```

No `level_properties_push()` means **no cutscene level is ever queued**; no
`safe_mark_write_save_file()` (that call is at `:414`, past the early return); and
`level_transition_begin(4)` with an empty property stack simply returns to the
overworld — which is "dropped back in front of the boss door". The amulet is not
missing either: the amulet cutscene lives in the `worldBit <<= 6` arm at `:381-397`
and belongs to the **second** boss race (level 46, after 8 balloons), not the first.

`game/src/vehicle_tricky.c` contains **no `GLOBAL_ASM` and no `NON_MATCHING`**, so
this is matching decompiled code, i.e. the ROM's own re-race behaviour — not a
transcription defect of the kind the "gridmask"/"hasmaudit" waves found.

### Measured, three runs, one binary

All muted + headless. `MDKR_BOSS_SLOW=1` is what lets the human win at all;
`MDKR_WATCH_COURSEFLAGS=38` reports every change to `courseFlagsPtr[38]` and
`settings->bosses`.

**1. First win (fresh `save/eeprom.bin`), boss 38 —** the invariant holds and the
presentation happens:

```
[BOSSW] frame=3349 courseFlags[38]=0x1      bosses=0x0   <- RACE_VISITED, at level entry
[BOSSW] frame=7019 courseFlags[38]=0x40001  bosses=0x0   <- an arena trigger
bossfinish: finishPos=1 … bosses=0x0 courseFlags=0x40001 timer=1
[BOSSW] frame=8781 courseFlags[38]=0x40003  bosses=0x2   <- the WIN branch, both bits together
level_load: levelId=57 entrance=4 cutscene=4 @frame~9139  <- the win cutscene
level_load: levelId=0 @frame~10917
```

**2. The same win with the boss course held at "already beaten"**
(`MDKR_BOSS_PRECLEARED=38`) — the report, reproduced:

```
bosspreclear: level=38 world=1 courseFlags=0x3 bosses=0x2 @frame~1
bossfinish: finishPos=1 playerIndex=0 courseId=38 worldId=1 bosses=0x2
            courseFlags=0x40003 cutsceneLevel=57 timer=1 raceFinished=1
level_load: levelId=0 @frame~8432        <- straight back to the overworld
```

Every field the branch reads matches the browser line exactly; `courseFlags`
differs only in one *arena trigger* bit (`0x400000` vs `0x040000`, i.e. which
trigger the driving line crossed). **No `level_load: levelId=57` at all** — no
cutscene before the race either, because `level_load()` (`game.c:475`) only
redirects to the cutscene level when `!(courseFlagsPtr[levelId] & 1)`.

**3. A full Adventure loop never touches either field.** Hub → Dino Domain lobby →
Ancient Lake (raced and finished) → lobby → hub, 17000 frames,
`MDKR_WATCH_COURSEFLAGS=38`: **one** `[BOSSW]` line, the frame-1 baseline
`courseFlags[38]=0x0 bosses=0x0`. So nothing on the Adventure path pollutes the
boss course's flags — the leading alternative explanation, and it is dead.

### Why "Adventure vs `MDKR_LOAD_TRACK`" is NOT the difference

The previous wave's open question was the entry path. It is not the variable:

- `set_course_finish_flags()` (`objects.c:6904`), the ordinary first-win path that
  writes `RACE_CLEARED` **and awards a balloon**, is unreachable in a boss race:
  the racer spawner sets `D_8011AD3C = 1` whenever `raceType == RACETYPE_BOSS`
  (`objects.c:1460`, unconditional at level init), and that routes
  `race_check_finish()` into the short middle branch at `objects.c:6784-6790`,
  which sets `raceFinished` and `finishPosition` and nothing else. Corroborated
  two ways: the same `if` also does `gNumRacers = 2`, and the boss race really does
  trace exactly two racers (`[GRND] pi=0 ri=0` and `pi=-1 ri=1`), so that branch
  demonstrably ran; and if a boss win *did* reach the balloon award, a world's
  balloon total could exceed 8 (4 tracks × 2), which the boss door's own
  `balloonsPtr[worldId] == 8` test treats as the maximum. **Reasoned, not driven,
  for the Adventure path** — the only ingredient that could differ there is
  `gIsTimeTrial` (which rewrites `raceType` to `HUBWORLD` two lines earlier), and
  Adventure cannot enable Time Trial.
- The Adventure path differs in `is_in_tracks_mode()`, in `gIsTimeTrial`, in what a
  return transition targets — and, decisively, in **flag persistence**. The
  Tracks-mode `Settings` is rebuilt from the save files by
  `trackmenu_set_records()` (`menu.c:3187`), which ORs every file's
  `courseFlagsPtr` and `bosses` together. Measured: a boss win reached through
  `MDKR_LOAD_TRACK` does **not** come back on the next boot — two consecutive wins
  sharing one `eeprom.bin` both read `bosses=0x0 courseFlags=0x40001` and both load
  cutscene 4. (Which file, if any, that win was written to was not chased; what
  matters is that it does not return.) In Adventure the flags *are* saved and
  restored — `func_800732E8` / `populate_settings_from_save_data` pack and unpack
  `bosses` and the per-level 2-bit course status symmetrically. So **the reported
  save can carry an earlier win and the direct path cannot manufacture the
  symptom**, which is why the reproduction above needs `MDKR_BOSS_PRECLEARED`.

### `func_80017A18` (the `return 0` stub) is ruled out

Raised as a candidate because it is called 8 times on boss track 38. It has
**exactly one caller**, `collision_objectmodel()` (`objects.c:5624`), and its
return value is used for exactly one thing, `collisionData->collidedObj = obj`. It
is a racer-versus-object-model collision test; it is not on any path between
`racer_boss_finish()` and the win cutscene or the amulet, and no value it could
return can set `settings->bosses`. Independently: the win cutscene demonstrably
loads on the first-win arm **with the stub live and those 8 calls happening**.

### How a save ends up with the win recorded but never seen

Measured on the first-win arm: the win branch writes both flags and calls
`safe_mark_write_save_file()` at frame **8781**, and the cutscene only loads at
**9139** — `level_transition_begin(4)` runs ~360 frames in between. **The win is
persisted ~6 seconds before it is shown**, so anything that interrupts those 360
frames loses the presentation permanently while keeping the recorded win. On the
build in that report, `update_tricky` had a ready-made interrupter:
`vehicle_tricky.c:219` calls `level_transition_begin(1)` whenever the player drops
400 units below their peak height — which, before the grid-mask fix, is exactly
what the volcano did to them. That is the most economical account of their earlier
report (*"I finished the race and was awarded first place, and still got the
failure animation, and was returned without having won"*) and of the `bosses=0x2`
in the log above. Faithful to the ROM (the ROM writes the flags then transitions),
so **not fixed**; recorded because it explains the save and because it is the
reason the reported symptom is not reproducible from a fresh save.
**This account is not itself tested — it is the most economical explanation
of the earlier report, not a reproduction of it — and `tests/check_boss_win_verdict.py`
asserts nothing about cutscene 5 never loading on a winning finish, so it would
not catch a regression of the original "1st place recorded as a loss" symptom.**

**What this means for the player:** nothing is broken now. That save has Tricky recorded as
beaten, so re-racing him will keep returning you to the lobby with no cutscene —
correct DKR behaviour. The amulet piece comes from the **rematch** (the second
Tricky race, which needs all 8 Dino Domain balloons), not from the first win. To
see the first-win cutscene again the file has to be started fresh.

### The invariant, and the check

The one property that must hold, and that nothing else in the codebase guarantees:

> When `racer_boss_finish()` reads them on a first visit, `courseFlagsPtr[bossLevel]
> & RACE_CLEARED` and `settings->bosses & (1 << worldId)` are both clear.

It is fragile in a specific way: **both fields are written by index from unrelated
subsystems** — golden balloons (`object_functions.c:3493`), doors (`:3756-3768`),
triggers (`:3972`) all do `courseFlagsPtr[settings->courseId] |= 0x10000 << id`,
`objects.c:1394/6595/6915/6919` write the low bits, and the save unpacker writes
both by level index. One stray index sets `RACE_CLEARED` on a boss course and from
then on **every** first boss win silently loses its cutscene and its amulet, with
no crash and nothing in the log. That is the regression
`tests/check_boss_win_verdict.py` exists to catch:

```
$ MDKR_AUDIO=0 python3 tests/check_boss_win_verdict.py
check_boss_win_verdict: PASS  (first win: bosses=0x0 courseFlags=0x40001 ->
  cutscene 4 @9139; already beaten: bosses=0x2 courseFlags=0x40003 -> no cutscene)

$ MDKR_AUDIO=0 python3 tests/check_boss_win_verdict.py --break-invariant
  - FIRST WIN: racer_boss_finish() read courseFlags=0x40003 on boss 38, which
    already has RACE_CLEARED (0x2) set. …
  - FIRST WIN: racer_boss_finish() read bosses=0x2 with world 1's bit (0x2)
    already set …
  - FIRST WIN: the win cutscene (levelId=57 entrance=4 cutscene=4) never loaded. …
  - FIRST WIN: no 'bossredirect:' line -- level_load() did not treat boss 38 as a
    first visit …
check_boss_win_verdict: FAIL                                        (exit 1)
```

Both arms come from one binary. The `cleared` arm is a **positive control** that
reproduces the report, so the invariant assertions cannot pass vacuously, and
`--break-invariant` runs the regression itself. The check also asserts that
`RACE_CLEARED` and the `bosses` bit are observed **on the same frame** — the win
branch writes them three statements apart, so `RACE_CLEARED` arriving alone is the
signature of a stray writer.

Two hooks, both no-ops unless set, both in `platform/mdkr_adventure.c` with one
call at the frame boundary in `platform/platform_sdl_min.c`:

- `MDKR_WATCH_COURSEFLAGS=<levelId>` — one `[BOSSW]` line per change to
  `courseFlagsPtr[levelId]` / `settings->bosses`. It **polls at the frame boundary
  rather than hooking each writer**, deliberately: that is what makes it catch a
  write through the *wrong* index, which is the bug shape being guarded.
- `MDKR_BOSS_PRECLEARED=<levelId>` — hold that boss course at "already beaten".

### What was NOT done, and what it would take

**An Adventure-path *first* boss win has still never been driven end to end.** The
blocker is measured, not assumed:

- The Dino Domain lobby's boss exit is `dest=38 radius=255 boss=0` at
  `(-771, -4, 1777)` (`MDKR_OBJDUMP=1` on level 12), and it is guarded by two
  `BIGBOSSDOOR` leaves (`doorID=13/14, balloons=4, localBalloons=1`) which open
  only on `balloonsPtr[worldId] >= 4` — i.e. **four won Dino Domain races**.
- Even ignoring the gate, `MDKR_DRIVE_ROUTE="…;12:E38"` cannot get there: steering
  straight at the exit for 15000 frames stalls in a box `x -1293..-871,
  z -430..671`, **closest approach 1210 units**. The lobby's five AI nodes are all
  in the central ring (`z -560..487`), so `MDKR_AUTOPILOT` cannot sample the gate
  chamber either (it covers only `x -1278..-985, z -714..-430`).
- What it would need: hand-sampled waypoints into the northern chamber, plus four
  race wins in an 8-racer field (autopilot finishes, but is not known to *win*),
  with the global-balloon arithmetic worked out (lobby doors need 1/2/3/5 global
  balloons, so ~2 hub balloons must be collected before the 4th race is enterable).

It is no longer on the critical path for this report — `bosses=0x2` can only come
from a prior win branch, and measurement 3 shows the Adventure loop never writes
these flags — but it is the last unexercised leg of the boss flow.

### EVTQ drop type 10, from the same browser session

**`AL_SEQP_VOL_EVT`** (`enum ALMsg` in `game/include/PR/libaudio.h`, 0-based:
… 9 `AL_SEQP_API_EVT`, **10 `AL_SEQP_VOL_EVT`**, 11 `AL_SEQP_LOOP_EVT` …).

**It does not change the verdict, for a structural reason.** Its handler in the
compact sequence player (now `platform/audio_event_queue.c`) assigns `seqp->vol`
and re-applies gain
to the allocated voices — and **posts nothing**. So unlike a dropped
`AL_CSP_NOTEOFF_EVT` (leaks a voice, note never released) or a dropped MIDI event
(lost note), a dropped volume event cannot break a self-perpetuating chain: it
costs exactly one volume change, i.e. music stuck at the previous level until the
next `alCSPSetVol()`. The posters are DKR's `music_fade`/`music_volume_reset`
paths, which re-issue every frame during a fade, so the worst case is a fade that
ends on the wrong level.

Worth noting for the queue-budget item: libultra's own comment at the drop site
names volume events as the canonical overflow case — *"if we filled the evtq with
volume events, then when the seqp is told to play it will handle all the events at
once completely emptying out the queue … this problem must be treated as an out of
resource error and the evtq should be increased"* — so type 10 in the log is
textbook evidence that the queue was under-budgeted in that browser session.
Follow-up now measures every queue in the committed runtime gate; the old
sequence-event types have not recurred on current head, while reproducible
live-sink pressure was isolated to the SFX queue. Any recurrence from either
player is now a release failure; see
[the resolved queue item](audio.md#fixed-browser-audio-event-queue-drops--measured-in-the-real-runtime).

The **cutscene link stays refuted**, now by measurement as well as by
construction: the verdict and its cutscene selection are reproduced
deterministically native, where there are **0** EVTQ drops, and both directions
(cutscene played / cutscene correctly skipped) come out of the flag state alone.

## P3.5 Adventure — the hub is drivable and the trophy series is covered

> **Status:** Parts A (drive the hub), C (enter a race, finish it, and return),
> and D (the four-world trophy championship) are DONE. Part B stands:
> `MENU_RESULTS` is a 2-player screen and a 1-player Adventure race cannot reach
> it; the trophy path correctly reaches `MENU_TROPHY_RACE_RANKINGS` instead.

**Part A is DONE.** FILE SELECT → Adventure → the new-game cutscene → Timber's
Island, with a player-controlled kart, is a reproducible headless route:
`tests/input_scripts/adventure_hub_drive.txt`, asserted by
`tests/check_adventure_hub.py`. 12/12 runs clean.

```
menu_init: menuId=19 @frame~1781      GAME_SELECT
menu_init: menuId=6  @frame~1931      FILE_SELECT
menu_init: menuId=23 @frame~2236      MENU_NEWGAME_CINEMATIC
level_load: levelId=36 cutscene=15 @frame~2236   the intro cutscene
level_load: levelId=0             @frame~5867   ASSET_LEVEL_CENTRALAREAHUB
[PACE] frame=6000  racer x=25.0     y=255.0 z=16.0    clock=132
[PACE] frame=12000 racer x=-1894.1  y=307.1 z=1113.8  clock=6132  cp=23 lap=2
```
6133 in-hub frames, 44441 world units of path, bounding box 5989 × 4069, y in
−35..539, max single-frame step 13.8, sampled frames 1014–2345 distinct colours /
sigma 36.9–57.7.

The route needed **three** fixes, all in the project's §3 bug classes, none of
them reachable from any pre-P3.5 fixture, and two of which aborted with nothing on
stderr.

### Root cause 1 — `GameTextTableStruct.entries[]`: 4-byte ROM words declared `char *`
`set_current_text()` (game/src/game_text.c) DMAs 16 raw bytes of
ASSET_GAME_TEXT_TABLE straight into `(*gGameTextTable)->entries`, then reads
`entries[textID & 1]` and `entries[(textID & 1) + 1]`, masking them with
`0xFF000000` / `0x00FFFFFF`. Those are **32-bit ROM words** (flags | ROM offset),
never pointers — but the decomp declares `char *entries[128]`, which is 8 bytes
per element on LP64. Two independent consequences:

1. **The stride doubles**, so index 1 reads ROM words 2–3 instead of word 1 and
   `size` comes out as garbage. Measured: `size = 65536`, and the follow-up
   `asset_load(ASSET_GAME_TEXT, ...)` then DMA'd 64 KB across live arena objects.
2. **`&entries[32]` moves from byte 128 to byte 256** of the 0x800 allocation, and
   `load_game_text_table()` uses it as the base of two 960-byte message buffers:
   128+960+960 == 0x800 exactly, 256+960+960 overruns by 128 bytes.

`asset_load()` is also a raw ROM DMA that does **not** normalise endianness, and
asset_swap's `ASSET_GAME_TEXT_TABLE` case only covers the wholesale
`asset_table_load()` path (it swaps word 0 for `asset_table_size`). So the four
words are still big-endian when the masks read them. Both are fixed: the field is
`u32 entries[128]` under `NATIVE_PORT` with `_Static_assert`s locking
`sizeof == 0x204` and `offsetof(entries[32]) == 128`, plus an explicit
byteswap of the four words just DMA'd.

**How it presented, and why it took a watchpoint.** The symptom was a SIGSEGV in
`free_3d_model()` → `model->references--` while tearing down the intro cutscene,
~600 frames *after* the corrupting write, on a `ModelInstance` whose `objModel`
read back as `0x8abdca4f24adc8ab`. That value is identical across runs despite
ASLR, i.e. it is *content*, not a stale address — the tell that the memory had
been overwritten with asset bytes rather than freed. A pre-teardown integrity
sweep found no double-frees and no shared `ModelInstance` pointers, so the write
had to be found directly: a hardware watchpoint on the victim's `objModel` caught

```
frame #1: platform_rom_read(romOffset=5240464, dst=0x…1a0590, len=20480)
frame #4: asset_load(assetIndex=7, address=…, assetOffset=1572992, size=65536)
frame #5: set_current_text(textID=50)
frame #6: func_8002125C(...)          <- the cutscene's dialogue entry
```

**Reached by ordinary play twice over**: the intro cutscene's dialogue, and every
hub door — `obj_loop_door()` calls `set_current_text(door->textID & 0xFF)` to show
the balloon requirement.

### Root cause 2 — `ParticleBehaviour`'s trailing pointer breaks the 0xA0 stride
`ColorLoopEntry *colourLoop` is the last field at 0x9C. On LP64 alignment pushes
it to 0xA0 and grows `sizeof` to 0xA8, while the on-disk record stride is 0xA0
(`asset_swap.c PARTICLE_BEHAVIOUR_SIZE`, which was already correct). Every read of
`colourLoop` therefore returned the **next record's** first eight bytes —
measured `0x3f8000003f800000`, i.e. two 1.0f floats — which the code then
dereferenced. Crash in `create_general_particle()` at `colourLoop->numEntries`,
~1280 frames into the hub, from `obj_loop_rangetrigger`.

Fixed as a `dkrptr32` slot with `_Static_assert`s on both `sizeof == 0xA0` and
`offsetof(colourLoop) == 0x9C`, read through a new `PARTICLE_COLOUR_LOOP()`
helper that maps the on-disk `-1` sentinel to `(ColorLoopEntry *)-1` (which the
existing `(s32)x != -1` / `(u32)x != -1U` tests still see correctly) and otherwise
reconstructs the arena pointer.

**This one moved an existing regression baseline — see the note at the end.**

### Root cause 3 — `func_80026E54()` writes two entries per segment into `sp94[10]`
`game/src/tracks.c`, the void/skybox segment sort reached from `void_check()`.
The fill loop does `sp94[j++]` **twice** per iteration, so with the function's own
`arg0 < 10` bound `j` runs to 2*arg0−1 == 17, and `void_generate_primitive()` then
reads `&sp94[sp60[i] * 2]` and its neighbour — also up to index 17. The array is
declared for ten. On the N64 the overflow lands in the frame slack the decomp
still models as `UNUSED s32 pad[7]`; under `-fstack-protector` it overwrites the
canary and aborts in `__stack_chk_fail`. Sized to `2 * 10` under `NATIVE_PORT`
with a `_Static_assert`. Measured trigger: `func_80026E54(arg0=9, …)`, i.e. nine
void segments in view at once, which the hub produces on parts of the loop.
Same shape as the `func_8002F440` shadow-clip fix.

> **Superseded upstream (2026-08-07).** Upstream now declares `f32 sp94[20]`
> itself, so the `NATIVE_PORT` override and its `_Static_assert` were dropped at
> the `c6695703` sync and the code today reads as plain upstream text. The fix
> above is still the reason the size is right; it is simply no longer ours. See
> the sync log in [DECOMP_SYNC.md](../DECOMP_SYNC.md).

### Positive controls (each fix reverted, rebuilt, `check_adventure_hub.py` re-run)
| reverted | failure |
|---|---|
| `GameTextTableStruct.entries` → `char *` | `exit code -6` and `ASSET_LEVEL_CENTRALAREAHUB (levelId=0) never loaded` (dies in the cutscene teardown) |
| `ParticleBehaviour.colourLoop` → real pointer | `exit code -11`, `[CRASH] signal 11`, `only reached hub checkpoint 1`, `only reached hub lap 0` |
| `func_80026E54` `sp94[2*10]` → `sp94[10]` | `exit code -6 (killed by signal 6)` |

### Part B — `MENU_RESULTS` after an Adventure race is unreachable as specified
The P3.5 brief expected `race_finish_adventure()` →
`load_menu_with_level_background(MENU_RESULTS, ASSET_LEVEL_TROPHYRACE, 0)`
(`thread3_main.c:689`). **That is not what `race_finish_adventure()` does, and
`MENU_RESULTS` is gated on two players.** Read paths, not run — but they are
unambiguous:

- `race_finish_adventure()` (`objects.c:6952`) only sets `gRaceEndTimer = 300`
  and `gRaceEndStage = 0`. The work happens in `race_transition_adventure()`,
  which runs the balloon cutscene and ends at `level_transition_begin(2)` — i.e.
  **back to the hub**, never a results menu.
- `thread3_main.c:689` (`LEVEL_CONTEXT_RESULTS`) has exactly one producer:
  `POSTRACE_OPT_8` from `menu_postrace` (`thread3_main.c:494`). And
  `menu.c:11234-11238` reaches `POSTRACE_OPT_8` only via
  `else if (gNumberOfActivePlayers >= 2)`; the 1-player branch below it offers
  TRY AGAIN / SELECT TRACK / SELECT CHARACTER instead, and
  `gTrophyRaceWorldId != 0` takes `POSTRACE_OPT_10` →
  `MENU_TROPHY_RACE_RANKINGS` (menuId 21), not `MENU_RESULTS` (17).
- Which of the two an Adventure finish takes is decided at `objects.c:6895`:
  winning a first-time 1-player Adventure race sets `i = TRUE` and calls
  `race_finish_adventure()`; anything else calls `postrace_start()`.

So **`MENU_RESULTS` (17) is a 2-player / multiplayer results screen**, and a
1-player Adventure race cannot reach it — the same "unreachable by construction"
shape as P3.1's Time-Trial route, and it should be validated by the split-screen
work, not here. `MENU_TROPHY_RACE_ROUND` (20) / `MENU_TROPHY_RACE_RANKINGS` (21)
are the screens a *trophy* race reaches.

~~**Not reached, and stated plainly: no Adventure race was started.**~~ — **now
DONE, see Part C.** Retained for the record: entering a race means driving the
kart through an "exit" object — `racer->exitObj`, whose `level_entry` carries a
`destinationMapId` at +0x08, consumed by `func_8006D968()` after
`racer->transitionTimer` counts 60 frames down (`racer.c:8862`). 18 open-loop hub
routes were tried (6 steering patterns × 12000 frames, plus 12
throttle-release/brake variants tuned around the one point where the route does
brush a door and its fade briefly whites the screen out); none completed a
transition, and `MDKR_AUTOPILOT=1` laps the hub spline (cp 66, lap 6 in ~6100
frames, rc=0) without ever entering a door. Hitting a specific hub entrance needs
closed-loop steering toward a known object position — which is exactly what Part C
adds.

### Part C — the Adventure race loop is DONE (wave "advloop")
The full loop is now a reproducible headless route, asserted by
`tests/check_adventure_race_loop.py` with the fixture
`tests/input_scripts/adventure_race_loop.txt`. **12/12 runs clean and
byte-identical.**

```
level_load: levelId=0  @5867                       Timber's Island (hub)
drive: level=0 step 3: balloon 10 COLLECTED (total=1) @6331
drive: level=0 step 6: exit to 12 TAKEN @6648
level_load: levelId=12 @6648                       Dino Domain lobby
drive: level=12 step 0: exit to 5 TAKEN @6950
level_load: levelId=5  @6950                       ANCIENT LAKE — the race
[PACE] frame=12712 ... clock=5222 cp=53 lap=2 rlap=3 fin=1     3 laps, finished
level_load: levelId=12 @13492 entrance=3           returned to the lobby
drive: level=12 step 1: exit to 0 TAKEN @13840
level_load: levelId=0  @13840 entrance=2           returned to Timber's Island
```
6539 in-race racer rows / 64153 world units of race path / max single-frame step
14.5; 3158 rows and 37433 units driving back in the hub; sampled frames
1121–2990 distinct colours / sigma 32.8–61.6.

**No game defect was found on this path.** Every obstacle was a property of DKR's
own design that the harness had to satisfy, and the whole wave is one new test
hook (`platform/mdkr_adventure.c`, 1 call line in `racer.c`) plus a fixture and a
check. The five things that had to be understood:

1. **Closed-loop steering needs DKR's own formula, and it has a half-turn in it.**
   The first attempt copied `racer_enter_door()` — `arctan2_f(dirX, dirZ) -
   (steerVisualRotation & 0xFFFF)`, negate, `>> 5` — and drove the kart *exactly
   180° away from the target and then straight on for ever*: measured distance to
   the Dino Domain exit rising monotonically 4765 → 6538 while the stick settled
   at 0, which is the stable fixed point of inverted steering. `racer_enter_door`
   aligns against the **exit's own stored facing vector**, which is already
   anti-parallel to the direction of travel through the door. The block that
   steers at a *position* is `racer_AI_pathing_inputs()` (`racer.c:765`) and it
   has `((arctan2_f(xDiff, zDiff)) - 0x8000) & 0xFFFF`: `steerVisualRotation` is a
   half turn out of phase with the geometric heading, because the forward vector
   the physics integrates is `racer->ox1/oz1` = local +z through the object
   matrix. Using the AI's version verbatim fixed it.
2. **Doors need balloons, and the route collects one — but the door is *not* a
   hard gate on the exit.** `obj_loop_door()` opens on
   `courseFlagsPtr[courseId] & (0x10000 << doorID)`, set from
   `*settings->balloonsPtr >= door->balloonCount`. Measured requirements: the
   hub's Dino Domain doors 1, Snowflake Mountain 2, Sherbet Island 10, Dragon
   Forest 16; in the Dino Domain lobby, Ancient Lake 1, Fossil Canyon 2, Jungle
   Falls 3, Hot Top Volcano 5, boss doors 4 (local). Only 4 of the hub's 7
   balloons are collectable: the other 3 have `challengeID != 0`, and
   `obj_init_goldenballoon()` sets `properties.goldenBalloon.action = 1` for those
   until their `tajFlags` bit is set, which gates out
   `obj_loop_goldenballoon()`'s collect branch. The committed route collects
   balloon 10 so the doors genuinely open — verified, the lobby's Ancient Lake
   door reports `open=1` — which is the canonical path. **But an earlier claim
   here that the exit cannot latch in front of a closed door was wrong and is
   retracted:** `obj_loop_exit()`'s half-plane boundary
   (`dot(exit->direction, racer − exit) == 0`) does not coincide with the door
   line, and the activation radii are large (253 hub → Dino Domain, 191 lobby →
   Ancient Lake), so a corner exists on the near side where the test passes. See
   the new open item below.
3. **Taj wedges the kart, and the hook cannot rescue it.** A straight line from
   the hub spawn to the Dino Domain exit drives into Taj (`BHV_PARK_WARDEN`,
   behaviourId 62, at (−197, 255, 193)); `obj_loop_parkwarden()` opens a dialogue
   on contact and calls `disable_racer_input()` every frame it is open, which
   zeroes the racer's velocity inside `update_player_racer`. Measured: frozen at
   (−125.3, 149.6) with `gRacerInputBlocked = 1` for 3000+ frames, 84 units from
   Taj. Unrescuable from this hook by construction — the dialogue advances on
   `input_pressed()` read off the pad, and `update_player_racer` zeroes
   `gCurrentButtonsPressed` whenever `gRacerInputBlocked` is set. The route swings
   wide instead (first waypoint (200, 500)).
4. **The AI-node graph is not a navmesh.** Tempting, and a dead end: the hub has
   21 `BHV_AINODE` objects in **seven disconnected components** — a 9-node loop on
   the central beach plus a 3-node triangle around each wandering object
   (`MDKR_OBJDUMP=2` prints them). No node path exists from the spawn to any door,
   which is also the real reason `MDKR_AUTOPILOT` can only lap the beach: measured
   bounding box x −1633..1761, z −1699..2025 over 6133 frames, closest approach to
   any hub exit 2144 units. Cross-island travel is therefore explicit waypoints,
   sampled from the `[PACE]` trace of the pre-existing open-loop hub fixture —
   i.e. from ground the port has already been *observed* to drive, not guessed.
5. **A held A button silences the whole post-race menu.** `adventure_hub_drive.txt`
   holds `6000 A 6000` as a throttle; copying that into this fixture made the run
   sit on the post-race screen for 4200 frames and never return. Every post-race
   screen is driven by `input_pressed()`, which is **edge-detected**, so while A is
   held no later tap produces an edge. With the hold removed, one single A tap
   clears `POSTRACE_STAGE_BEGIN` and the return happens 350 frames later. The
   drive hook and `MDKR_AUTOPILOT` supply the throttle themselves, so the hold was
   never needed.

**Where the loop is closed by RETURN TO LOBBY, not by a win — stated plainly.**
The player is driven by the same AI as the CPU field, so it finishes mid-pack.
`objects.c:6851` only takes the `race_finish_adventure()` branch — the balloon
cutscene, which returns to the lobby by itself — when
`(*gRacersByPosition)->racer->playerIndex != PLAYER_COMPUTER`, i.e. when the racer
in **first** place is human. That check is legitimate here (it runs from
`race_check_finish()` at `objects.c:3258`, outside the window in which
`update_player_racer` temporarily flips the human to `PLAYER_COMPUTER`), so a
mid-pack finish really does take `postrace_start()`. Measured: with no input the
run freezes on that menu (clock=5222, fin=1, 13000 frames); with blind A taps it
picks option 0 = TRY AGAIN and re-races for ever (`level_load levelId=5` at 6950,
14212, 19882, 25642). The fixture therefore picks option 1, **RETURN TO LOBBY**,
of `[TRY AGAIN, RETURN TO LOBBY, QUIT]` (`menu.c:10570`).

At this historical Part C checkpoint, **`race_finish_adventure()` + the Taj
balloon cutscene remained unvalidated**. That gap is now superseded:
`check_adventure_race_loop.py` closes production win/loss persistence, and
`check_first_boss_progression.py` drives the legal fourth-race and first-boss
campaign chain through result, hub return, and save reload.

The original constraint was that the then-current fixture could not make its
AI-driven player finish first.
`MDKR_FORCE_LAPS=1` does not do it (it shortens the race for the whole field —
measured finish clock 1695, still mid-pack) and `MDKR_FORCE_BOOST` boosts every
racer, so neither gives a relative advantage. A `race_finish_adventure` route
needed either a new hook that handicapped the CPU field or a route that reached
a race the AI reliably won; both were deliberately out of scope here rather
than faked.

Also not validated at that checkpoint, but demonstrably *reachable*, was the
Dino Domain boss warp. The current first-boss gate supersedes this gap. An early
blind route entered `ASSET_LEVEL_TRICKYTOPS1` (levelId 38) from the
lobby twice — `obj_loop_exit()` only disables a `WARP_BOSS_FIRST` exit when
`balloonsPtr[worldId] == 8`, so with 0 local balloons the warp is live. That is a
`RACETYPE_BOSS` load with its own cutscene remap (observed `courseId` 57 while
`level_load` reported 38) and deserves its own route.

### ~~NEW OPEN ITEM (from wave "advloop"): a closed door can be driven past~~ — CLOSED by wave "objcoll"
> **RESOLVED 2026-07-26.** The question this item posed — *"is the ground at that
> corner reachable, or is it walled off by level geometry we are mis-decoding or
> mis-colliding?"* — had a third answer neither branch anticipated: **the door leaf
> itself was intangible**, because `func_80017A18` was a `return 0` stub. There is
> no corner to reach; the door never collided with anything. The analysis below is
> preserved because its *geometry* is still correct and still the reason
> `obj_loop_exit()` cannot be treated as door-gated, but its conclusion ("so the
> open question is collision, not object logic") is now answered. See
> [wave "objcoll"](collision.md#fixed-object-model-collision-never-reported-a-hit-so-locked-doors-were-intangible--wave-objcoll).

**Unverified against hardware — needs an oracle comparison, so it is logged, not
fixed.** With `MDKR_DRIVE_ROUTE="0:200,500:-1004,946:E12;12:E5"` and **no balloon
collected**, the kart reaches the Dino Domain lobby at frame 6582 and **Ancient
Lake at 6884**, with `balloons_total=0` and the relevant doors reporting `open=0`
in `MDKR_OBJDUMP`. Both exits latched in front of a shut door.

Mechanically this is not surprising: `obj_loop_exit()`'s only geometric tests are
a sphere (`sqrtf(dx²+dy²+dz²) < exit->radius`, and `interactObj->distance <
radius`) and a half-plane through the exit's own origin, and neither is tied to
the door leaves. For the hub → Dino Domain exit at (−4015, 383, 2683) with
`dir = (0.773, −0.634)` and radius 253, the plane
`0.773·x − 0.634·z + 4804.2 = 0` runs *west* of the line joining the two door
leaves at (−4105, 2435) and (−3804, 2802), so e.g. (−4150, 2600) satisfies the
plane at a 3D distance of ~195 from the exit — inside the radius, outside the
doorway.

So the open question is **collision, not object logic**: on real hardware, is the
ground at that corner reachable, or is it walled off by level geometry we are
mis-decoding or mis-colliding? DKR does have door-skip glitches, so this may be
faithful. Until it is checked against `tools/run_oracle.sh` neither answer should
be assumed. Note this does **not** affect the committed route, which collects
balloon 10 and goes through opened doors on purpose.

### Regression baselines this wave MOVED (read before trusting old numbers)
Root cause 2 changes what the particle system reads, and `race_drive_long.txt` is
a **blind** open-loop route (throttle held, LEFT pulsed on a fixed period), so its
line is chaotic with respect to any change in the simulation. Measured: the
pre-fix and post-fix builds are **bit-identical until race clock 36** and then
separate.

| | pre-P3.5 | now |
|---|---|---|
| `race_drive_long` final `cp` | 28 | **19** (lap 1 either way) |
| `race_drive_long` max step | 22.4 (crossed a zip pad) | **13.5** (does not) |
| `race_full_3lap_tt` course time | 4777 @ frame 7589 | **4709 @ frame 7495** |
| `race_full_3lap_tt` fastest lap | 1558 | **1515** |
| `save/eeprom.bin` md5 | `a144ba9f…` | **`bfd4de76…`** |

Two check thresholds were recalibrated for this, both with the reason in-file:
`check_race_drive.py` `MIN_FINAL_CP` 20 → 15 (the broken build scores 0, so the
margin that matters is intact), and `check_race_finish_time.py` now matches the
EEPROM record to the traced clock **within 32** instead of exactly. That second
one is a genuine loosening and is called out as such: the traced clock comes from
the `[PACE]` probe inside `update_player_racer` (which stops for a finished
racer) while the record is what `race_finish_time_trial()` sums when it runs, so
the two reads are a few updates apart. They happened to agree exactly before
(both 4777); now the trace says 4709 and byte 338 holds 4683. The window is still
tight enough that a byteswap or a record-layout change fails it.

**Honest gap:** the *propagation path* from a particle colour read into kart
position was not traced. The colour-randomisation block that consumes
`rand_range()` is gated on `randomizationFlags` (offset 0x5C, unaffected by the
stride change), `obj_init_emitter()` already sizes its region with
`sizeof(ParticleEmitter)`, and nothing allocates from
`sizeof(ParticleBehaviour)` — so the obvious candidates are all ruled out. What
*is* established: the pre-fix build dereferenced out-of-bounds bytes on every
colour-looping emitter, so any behaviour resting on that read was undefined, and
the corrected build is the one whose inputs are in-bounds.

## OPEN: campaign completeness — silver coins, later boss rematches, both Wizpig races, and the credits path are ungated, not unimplemented

Disclosed to players at [`README.md`](../../README.md#known-limitations)
("the complete start-to-credits campaign is not automated or claimed
complete") and in [`ROADMAP.md`](../../ROADMAP.md), which calls this "the
largest single piece of deferred work in the project, and the one most likely
to matter to someone playing rather than reading." This item records the
mechanism behind that disclosure: **the code is present and wired, and one
silver-coin-gated rematch is already gated and passing** —
`obj_init_silvercoin`/`obj_loop_silvercoin` and the
`SILVER_COIN_ACTIVE`/`COLLECTED`/`INACTIVE` states (`object_functions.c`),
persistence via `RACE_CLEARED_SILVER_COINS` (`save_data.c:430-431,569-570`),
HUD via `hud_silver_coins` (`game_ui.c`), and the credits sequence in
`menu.c`/`thread3_main.c`/`camera.c` all exist, with a
`tests/input_scripts/credits_via_cheat.txt` fixture reaching credits, and
`tests/check_bluey2_rematch.py` gating the first silver-coin-gated boss
rematch end to end.

What is missing is not the logic — this is the same decompiled retail code
the closed portions of Adventure already run — it is a witness for the *rest*
of the path: silver-coin progression past the first rematch, the later boss
rematches, both Wizpig races, and the credits path reached by actually
playing rather than by cheat. [`docs/RELEASE_CANDIDATE_TEST_GUIDE.md`](../RELEASE_CANDIDATE_TEST_GUIDE.md)
prescribes a manual start-to-credits pass as the intentional acceptance
boundary for this gap, but no CHANGELOG or RELEASE_NOTES entry records that
pass as ever having been completed. So: the campaign is **ungated, not
unimplemented**, and today it is neither automated nor recorded as manually
witnessed.

### MOSTLY CLOSED: `tests/check_campaign_progression.py`

The bulk of the above is now gated. That check does not attempt one continuous
start-to-credits drive — it would run for hours and fail as one opaque blob.
It gates each progression **seam** with a save fixture entering it and an
assertion on what production writes leaving it, with each fixture derived from
what the previous seam is proved to write
([`tests/fixtures/README.md`](../../tests/fixtures/README.md) carries the
bit-level derivation):

- **silver coins** — all eight collected on a real course entered through the
  hub and lobby, `RACE_CLEARED_SILVER_COINS` written once, the two balloon
  counters incremented, and the whole thing read back by a second process;
- **all four boss rematches** — each fought on the EEPROM the previous one
  persisted, so `wizpigAmulet` climbing 1, 2, 3, 4 is written by production four
  times rather than asserted into a fixture; against a first-encounter control
  that must award the first-boss bit and no amulet;
- **Wizpig 1** — four amulet pieces redirecting the next Timber's Island load to
  the Wizpig mouth sequence (three pieces must not), then the race itself won;
- **Wizpig 2** — won, setting and persisting `bosses & 0x20`, the single value
  `menu_credits_init` reads to choose the true ending over "THE END?".

Three things remain manual, each for a measured reason recorded in that fixtures
README rather than papered over: the lobby's boss-rematch **door** driven rather
than retargeted (the kart stalls 1,240 units short of it), the **T.T. amulet**
challenges and the **trophy** championships with the Future Fun Land unlock they
feed (the trophy side separately gated by `check_trophy_series.py`), and the
**credits screen** reached by finishing Wizpig 2 — `ASSET_LEVEL_WIZPIG2ANIM` does
not run itself out headlessly, so the check asserts the bit that selects the
ending rather than the screen that shows it.

One methodological note worth keeping, because it cost a false "unwitnessed"
finding: `game_load_level` traces the level it was **asked** for, and the
Wizpig-face branch rewrites that level afterwards, then pushes and pops the hub
around the cutscene. A redirected hub load is therefore indistinguishable in the
level-load stream from an ordinary one. The `wizpigface:` trace states the
redirect where it happens instead.

## OPEN, deliberately deferred: Taj Time Trial records no best time and stores no ghost

Taj is heavily advertised ([`RELEASE_NOTES.md`](../../RELEASE_NOTES.md),
[`README.md`](../../README.md)) but the deferred scope this causes is not
itemized in either doc, only in
[`docs/architecture/taj-playable-mod.md`](../architecture/taj-playable-mod.md).
Per that doc's "Records and ghosts" section (§352-369) and "Deferred" list
(§535-545): Taj is a virtual character overlaid on the retail roster rather
than a true `CHARACTER_TAJ` slot, and Taj-compatible serialized ghosts or
leaderboard semantics are explicitly deferred. `race_finish_time_trial` skips
the best-time store, the player-ghost swap, and staff-ghost retirement for a
Taj run — the run still plays the ordinary end-of-run announcement (HUD and
audio only, no record side effect), so **a player Time Trialling as Taj gets
no saved ghost and no recorded best time, silently.**

**Deliberately deferred.** Taj is a virtual character overlaid on the retail
roster rather than a true `CHARACTER_TAJ` slot, and that boundary is
deliberate: it keeps the mod architecture from touching retail data
migration, the character-select map asset, or the serialized record format.
The consequence a player can observe is the intended containment, not an
oversight — competitive timing data is quarantined precisely so a Taj run can
never overwrite the original roster's records, staff-ghost progress,
initials, or player ghosts, and so an existing Controller Pak or EEPROM save
written before Taj existed stays exactly as valid afterward. Giving Taj real
ghost and leaderboard semantics means deciding how a non-retail character is
represented in a format retail hardware also reads — a compatibility
question, not a bug — and belongs with the authored vehicle rows and the
fourth carpet physics class in a second-generation asset-pack feature
(`taj-playable-mod.md` §535-545, "Deferred").

## Playability wave — memory safety, saves, and the race finish

### FIXED: stack-buffer-overflow on the Adventure entry path (`fileselect_render`)
`menu.c fileselect_render` declares `char trimmedFilename[4]`, but
`filename_trim()` writes `strlen(input) + 1` bytes and **both** inputs exceed
that: `gSavefileInfo[i].name` is `char[4]` (up to 5 with the terminator) and
`gFilenames[i]` is a menu-text string such as `"GAME A"` (7 with the terminator).
So the FILE SELECT screen overran the buffer by up to 3 bytes **every time it drew
an un-started save slot** — i.e. on the normal Adventure entry path, which is
exactly where a player starts.

On the N64 it lands in unused stack and is harmless; here it trips
`-fstack-protector` or corrupts a neighbouring local depending on layout. Sized to
`[32]` under `NATIVE_PORT` with a `_Static_assert` tying it to
`sizeof(SavefileInfo.name)`; the N64 declaration is untouched. Same class and
remedy as the `func_8002F440` `sp90[6]->[8]` fix.

**Verified with a positive control, because this one does NOT crash normally**
(15/15 clean runs before *and* after): with `[4]` the ASan build reports
`stack-buffer-overflow WRITE ... in fileselect_render menu.c:7769` and aborts
(rc=134); with `[32]` the same route is ASan-clean, rc=0. That asymmetry is the
whole point — a plain crash-count fixture could never have caught it.

### Save persistence — VERIFIED WORKING
Delete `save/eeprom.bin`, run → 512 bytes / 120 non-zero; run again → identical
md5, so the file is read back and not reset. Progress survives a restart.

### The race finish path — partially validated, results screen NOT yet observed
New fixture `tests/input_scripts/race_full_3lap.txt` drives the race toward its
natural end. Findings:
- The open-loop route (hold accelerate + pulse LEFT every 120 frames) holds the
  racing line for **one** lap and then drifts: lap 1 at clock 2776, lap 2 not
  until clock 10557. So a driving script is a flaky way to reach the finish, and
  extending it further is not the answer.
- New test hook **`MDKR_FORCE_LAPS=N`** (`platform/asset_swap.c
  swap_level_header`, no-op unless set, same contract as `MDKR_FORCE_BOOST`)
  rewrites `LevelHeader.laps` once at the load boundary. Done there deliberately:
  `laps` is read from ~12 sites across `racer.c` and `game_ui.c`, so a per-site
  override would mean patching the whole race loop and HUD; rewriting the single
  `s8` at 0x4B keeps every reader consistent by construction, and it is a plain
  byte so it cannot perturb the endianness swap. Verified inert when unset.
**Measured, and the two cases differ — do not conflate them:**

| run | max `racer->lap` | final clock | EEPROM | outcome |
|---|---|---|---|---|
| no hook, 3 laps | 2 | 10883 | unchanged | route drifts; never finishes |
| `MDKR_FORCE_LAPS=2` | 1 | 9883 | unchanged | **driving** limit — lap 2 unfinished |
| `MDKR_FORCE_LAPS=1` | **0** | frozen at 2775 | unchanged | **anomaly, see below** |

- `FORCE_LAPS=2` did **not** reach the finish, but for an uninteresting reason:
  the open-loop route needs until ~clock 10557 to complete lap 2 and the run ended
  at 9883. That is the fixture's driving limitation, not finish logic.
- `FORCE_LAPS=1` is the interesting one. The race clock **freezes** at 2775 (one
  lap's worth) and never resumes across 12000 frames, yet `racer->lap` stays
  **0** the whole time. The clock gate is `header->laps > countLap` while the
  finish gate (`objects.c:6687`) is `curRacer->lap >= currentLevelHeader->laps`,
  so `countLap` evidently reached 1 while `lap` did not — and because the finish
  test reads `lap`, `raceFinished` never fires. The race is left running with a
  stopped clock.

#### RESOLVED by instrumentation: the lap machinery is FINE; the test route is not

Direct instrumentation of `lap` / `countLap` / `raceFinished` / `lap_times[]`
settles it, and **retracts the "finish is broken" reading above**:

```
MDKR_FORCE_LAPS=2:
  f=12000 lap=1 countLap=1 raceFinished=0 hdrLaps=2 lt0=2775 lt1=6109 cp=33
  f=12600 lap=1 countLap=1 raceFinished=0 hdrLaps=2 lt0=2775 lt1=6709 cp=33
  f=13500 lap=1 countLap=1 raceFinished=0 hdrLaps=2 lt0=2775 lt1=7609 cp=35
```

`lap` increments correctly, `countLap` follows it, and **lap 1's time is recorded
and frozen at 2775 exactly as it should be**. Nothing about the lap counting,
lap timing or per-lap recording is broken.

What is actually happening: `cp` oscillates 33 → 34 → 33 → 35 while lap 2's timer
runs on. The kart is **stuck**. Position sampling in that window shows it
wandering (x −3300…−1230, z ≈ −3500…−4500, resting y ≈ −9.9) with
`courseCheckpoint` creeping 32 → 34 over 2500 frames — *not* a respawn loop
(unlike the ASSET_MISC_8 stranding, which teleported ~1300 units in one frame),
just an open-loop route that bounces the kart into a corner and never recovers.

The earlier `laps == 1` "anomaly" is explained by the same thing and is not worth
chasing: the kart simply never crosses the line, so `lap` never leaves 0 and the
clock stops once `lap_times[0]` reaches the one-lap boundary. `laps == 1` is also
not a configuration any real track uses, so it is an artifact of the test hook.

**So the open item is a TEST-HARNESS gap, not a game bug:** we cannot yet drive
three clean laps, so the finish (and the time write that follows it) is
*unvalidated* rather than *broken*. Do not go looking for a finish bug first.

#### DONE: `MDKR_AUTOPILOT=1` — drive the human racer with DKR's own AI
`racer_AI_pathing_inputs()` is what every CPU racer uses and what laps real tracks
in the attract demo. The hook runs it for the human racer (after the input
dispatch in `update_player_racer`, gated on a non-CPU racer so CPU racers are not
driven twice), leaving race logic, physics and finish handling untouched.

| route | lap 1 | lap 2 | outcome |
|---|---|---|---|
| open loop | clock 2776 | never | kart stuck, `cp` creeping 32→34 |
| **autopilot** | **clock 1703** | **clock 3353** | consistent ~1650/lap racing line |

Deterministic (identical across 3 runs), 5×12000-frame runs = 0 crashes, and
inert when unset (`check_race_drive` + `check_determinism` both pass).

#### THE REMAINING QUESTION, now much narrower
With a driver that actually reaches the final lap, the open item is no longer
"can we finish?" but **what happens at the finish handoff**:

- The player racer update **stops at the final-lap crossing**. Under autopilot the
  periodic probe runs to f=8000 and then ceases; lap 3 began at f=6469 and a lap
  takes ~1650 frames, so the stop lands exactly on the third crossing. The same
  signature appeared under `MDKR_FORCE_LAPS=1`.
- That stop is plausibly **correct** — a finished racer is expected to move to a
  different update path (finish cutscene camera, etc.), which is also why
  `raceFinished` was never observed flipping in a probe that lives *inside* the
  update that stops.
- But **no time is written to EEPROM** afterwards, in any run.

So the next investigation is specifically: instrument *outside*
`update_player_racer` (the probe there cannot see past the handoff), confirm
whether `raceFinished` is set, and follow the post-race path to whichever code
should record the lap/course time into `Settings.courseTimesPtr` /
`flapTimesPtr`. `thread3_main.c clear_lap_records()` shows the shape of that
storage, and `ASSET_MISC_23` (now correctly byte-swapped) holds the defaults it
is compared against.

An earlier revision of this file said "`MENU_RESULTS` was never observed" as
though that were the defect. That framing was wrong: `MENU_RESULTS` is loaded only
via `load_menu_with_level_background(MENU_RESULTS, ASSET_LEVEL_TROPHYRACE, 0)`
(`thread3_main.c:689`), i.e. it is the **trophy-race** results screen. A Time
Trial finish is not expected to reach it, so its absence is not evidence of the
bug. The evidence is the frozen clock, `raceFinished` never firing, and the
unchanged EEPROM.

### P3.1 RESOLVED — a Time Trial finish now records a time (wave "finishtime")

**`raceFinished` was never broken, and the record-writing code was never broken.
Two other things were.** Both retractions below are backed by measurements, not
reasoning.

#### Retraction 1: "`raceFinished` never fires"
It always fired. The probe that failed to see it lived *inside*
`update_player_racer`, which stops running for a racer the instant it finishes —
DKR flips a finished racer to `PLAYER_COMPUTER` (`racer.c:4348`) and hands the
kart to the AI for the finish cutscene. A probe fed from `race_check_finish()`
instead — which keeps being called every frame — sees the transition cleanly:

```
frame=7588 ... clock=4777 cp=53 lap=2 rlap=3 fin=0
frame=7589 ... clock=4777 cp=53 lap=2 rlap=3 fin=1     <- raceFinished = TRUE
```

Note `lap=2` vs `rlap=3` on the same line: `lap`/`cp` come from the update that
stops and are permanently one lap stale, `rlap`/`fin` come from
`race_check_finish`. That single stale field is the whole reason the finish looked
dead. The `[PACE]` trace now carries `rlap=` and `fin=` permanently.

#### Root cause 1 (the reason no run had ever saved a time): the route, not the code
DKR writes course/lap records **only** from `race_finish_time_trial()`
(`objects.c:7119`), whose entire body is gated on `if (gIsTimeTrial)`. Every
existing race fixture ends its track-select navigation by confirming **TIME TRIAL
= OFF** — `nav_to_time_trial_race.txt` even says so in its own comments ("confirm
Time-Trial=off"). The fixture names are misleading: "time trial" there means
*Tracks mode*, not Time Trial.

Measured on the old route (`race_full_3lap.txt`, `MDKR_FORCE_LAPS=2`), at the
finish: `raceFinished=1 finishPos=6 gNumFinishedRacers=9 gRaceFinishTriggered=-1
gIsTimeTrial=0 gNumRacers=8`. `race_finish_time_trial()` ran and immediately fell
through its `if (gIsTimeTrial)`, leaving `display_times=0` and
`racers[0].best_times=0x00`. Those two are exactly what the post-race menu tests
before calling `mark_to_write_flap_times()` / `mark_to_write_course_times()`
(`menu.c:11212-11218`), so the EEPROM write was unreachable **by construction**.
`gNumRacers=8` is the tell — a real Time Trial is solo.

So the save path was correct and simply never exercised. New fixture
`tests/input_scripts/race_full_3lap_tt.txt` adds the one missing input: at the
track-select Time-Trial stage the stick must be pushed **DOWN** to move
`gTracksMenuTimeTrialHighlightIndex` 0 → 1 so `set_time_trial_enabled(1)` runs
(`menu.c:9844`). With Time Trial actually on, the same code writes the record
first time:

```
rftt-pre:   vehicleID=0 courseId=5 existingFlap=1642 existingCourse=5154
            bestRacerTime=1558 bestCourseTime=4777
rftt-wrote: flapTime=1558 courseTime=4777 best_times=0x82 display_times=1
write_eeprom_data(flags=0x3) courseId=5 courseTime[0..2]={4777,5154,5154}
```
(1642/5154 are the `ASSET_MISC_23` staff defaults; 0x82 = course record + lap 2
was the fastest lap.) The post-race screens then need **repeated edge-detected A
presses** to walk BEGIN → SHRINK_VIEWPORT → RACE_TIMES → ENTER_INITIALS (3
characters + confirm) → RACE_RECORDS → OPTIONS → END. The old fixture held A for
11000 frames, which yields exactly *one* press.

#### Root cause 2: `MDKR_AUTOPILOT` cannot drive a solo Time Trial
Turning Time Trial on exposed a second blocker. `func_80042D20` — DKR's AI
throttle/behaviour routine, reached from `racer_AI_pathing_inputs()` — counts
racers whose `playerIndex == PLAYER_COMPUTER` into `var_t0` and does
`if (var_t0 == 0) { return; }` (`racer.c:277`) before ever setting `A_BUTTON`. A
solo Time Trial has exactly one racer and it is the human, so the count is always
zero and the AI never applies throttle. Steering *is* still computed (it happens
earlier, in `func_80045C48`), so the symptom is not "nothing happens":

| | TT off (8 racers) | TT on (solo), before the fix |
|---|---|---|
| z over 800 frames | −6413 → −9300 (forward) | −6482 → −6162 (**backwards**) |
| `courseCheckpoint` | 0 → 2 → 4 … | 0 → **−1** |
| `nextCheckpoint` | 2 | **17** (wrapped) |
| after ~1000 frames | lapping | dead stop at x −1790, z −6151 |

The backwards run is the AI's own "I'm stuck" logic (`racer_AI_pathing_inputs`:
velocity ≈ 0 for 60 frames ⇒ 60 frames of reverse). This is faithful N64
behaviour — the real game never asks the AI to drive a solo Time Trial — so the
remedy belongs in the test hook, not the game logic. The hook now presents the
racer as a CPU racer for the duration of the AI call and restores it immediately,
which is **the game's own idiom** (`update_player_racer` does exactly this once
`raceFinished` is set). With that, autopilot laps a solo Time Trial at a
consistent ~1590 clock/lap and finishes 3 laps at clock 4777 / frame 7589.

#### Root cause 3 (found by the new route): stack-buffer-overflow in `timetrial_ghost_read`
A real LP64 defect, and the only genuine *bug* of the three. A Catmull-Rom
segment needs **four** control points: `catmull_rom_interpolation()` reads
`data[index+0..3]` (`objects.c:10503`). The fill loop in `timetrial_ghost_read`
writes four as well — `for (i = 0; i <= ARRAY_COUNT(vectorY); i++)`, i.e.
i = 0..3. But the three arrays are declared `f32 vectorX[3] / vectorY[3] /
vectorZ[3]`, one element short of what the same function reads *and* writes.

On the N64 the fourth store lands in adjacent stack slack (the decomp's
`pad_sp58` local is what remains of it), so the value read back is the one just
written and hardware interpolates the ghost correctly. On LP64 the fourth store
lands on the `-fstack-protector` canary:

```
stop reason = signal SIGABRT
  frame #3: __stack_chk_fail + 96
  frame #4: mdkr64`timetrial_ghost_read(obj=...) at racer.c:8711
  frame #5: mdkr64`obj_loop_timetrialghost at object_functions.c:1061
  frame #7: mdkr64`obj_update at objects.c:3186
```

**Reached by the ordinary play loop, not by anything exotic**, and the ghost in
question is the player's own, not a staff ghost. Exact repro (no pre-existing save
or ghost state — the run starts from a deleted `save/eeprom.bin`; the ghost is
recorded and replayed within the *same process*):

```bash
rm -f save/eeprom.bin
MDKR_AUTOPILOT=1 MDKR_AUDIO=0 ./build/mdkr64 --headless-frames 13000 \
  --input-script tests/input_scripts/race_full_3lap_tt.txt --rom baserom.us.v80.z64
```

The fixture finishes the Time Trial at frame 7589 (which records a player ghost
and swaps it in for playback), its trailing A taps then pick **TRY AGAIN** on the
post-race menu, the level is re-entered at frame ~7970, and the ghost is played
back from there. Locals at the abort, under lldb with the fix reverted:

```
frame #3: __stack_chk_fail + 96
frame #4: timetrial_ghost_read(obj=0x…) at racer.c:8747
frame #5: obj_loop_timetrialghost at object_functions.c:1061
ghostDataIndex = 0      <- the PLAYER's ghost bank, not staff (staff would be 2)
ghostNodeCount = 173    <- real recorded ghost nodes, not a stub
i              = 4      <- the loop wrote 4 elements into f32 [3]
```

So this is "set a record, then hit TRY AGAIN" — the single most likely thing a
player does after setting a record. Without the canary it would be worse than a
crash: a neighbouring local corrupted *and* a garbage fourth control point fed to
the spline, putting the ghost somewhere it never was — another silent one.

Fixed `NATIVE_PORT`-gated: arrays sized to `GHOST_SPLINE_POINTS` (4) **and** the
loop bound switched from `<=` to `<`, so exactly the same four stores happen, now
in bounds. The N64 declaration and loop are untouched. Same class and remedy as the
`fileselect_render` `trimmedFilename[4] → [32]` and `func_8002F440`
`sp90[6] → [8]` fixes above.

> **The fix is two coupled parts — revert them together or not at all.** Changing
> only the size back to `[3]` while leaving the loop at `<` yields a *third* state
> that is memory-safe (three writes into a three-element array) and therefore
> passes every runtime check, but is silently wrong:
> `catmull_rom_interpolation()` still reads `data[3]`, now uninitialised stack.
> The `_Static_assert` exists precisely to catch that half-revert — it makes the
> mis-sized state fail to **compile**. Do not neuter it to get a build. Measured,
> all three states:
>
> | state | size | loop | result |
> |---|---|---|---|
> | fixed | `[4]` | `<` | check PASS, rc=0 |
> | half-revert | `[3]` | `<` | **does not compile** (`_Static_assert` fires) |
> | half-revert, assert neutered | `[3]` | `<` | check PASS — memory-safe, silently wrong |
> | true original pair | `[3]` | `<=` | **`run exited -6` (SIGABRT)**, check FAILS |

#### How it was verified
`tests/check_race_finish_time.py` (new) asserts the whole chain: `rlap` reaches
the level's lap count, `fin` flips 0 → 1, the race clock freezes, the frozen value
lands in a plausible band, that **exact measured** value appears in
`save/eeprom.bin` as a 16-bit big-endian record (so a byteswap or layout
regression in the serialiser fails the check rather than passing on a hardcoded
offset), and a second run reads the record back instead of resetting it. It also
asserts on the **process exit code** — the `timetrial_ghost_read` abort printed
nothing at all to stderr, so a log-scraping check missed it entirely.

It additionally asserts **that the route reaches the ghost path at all**, via new
`ghost=`/`gbank=` `[PACE]` fields fed from `timetrial_ghost_read()`: measured
**5010 ghost-playback frames, bank 0 (the player's own ghost)**. Without that
assertion the check could keep passing while a future route change stopped
re-entering the level after the finish, covering none of that code — which is
exactly the trap the first version of this script fell into by ignoring the exit
code.

- 3× definition-of-done runs from a deleted `save/eeprom.bin`: each reached
  `fin=1` at frame 7589, course time 4777, and produced a 512-byte
  `eeprom.bin` with md5 `a144ba9f8b9e59e8dfddb75d48bf937c` containing 4777
  (`0x12A9`) at byte 338 and the 1558 fastest-lap record at byte 146.
- **Restart persistence**: re-running reports `existingFlap=1558
  existingCourse=4777` read back from EEPROM in a fresh process, and correctly
  does *not* rewrite (a deterministic re-drive cannot beat its own time), so the
  md5 is byte-identical. Beating it on purpose with `MDKR_FORCE_LAPS=2`
  (2-lap course time 3190 < 4777) moves the md5
  `a144ba9f…` → `ab348c9a…` and `write_eeprom_data(flags=0x2)` writes course
  times only — the flap record was not beaten, and the flags reflect that.
- **Positive controls, both fixes, both fail as predicted.** Reverting
  `timetrial_ghost_read` to the true original pair (`[3]` **and** `<=`, see the
  three-state table above): `run exited -6 (killed by signal 6)`. Reverting the
  autopilot CPU presentation: `max rlap=0`, `raceFinished` never TRUE,
  `course time 10189 is not present in save/eeprom.bin`.
- No regressions: `check_race_drive.py` PASS, `check_determinism.py` PASS,
  `race_drive_time_trial` ×10 = 0 crashes, all 9 `nav_*` fixtures ×5 = 0 crashes,
  `race_full_3lap_tt` ×5 = 0 crashes.

**Still open on this path:** the initials the post-race screen stores for a new
record compress to `0x0000` on this route (the A-tap route selects the first
character cell three times). Whether that matches the real ROM for the same
keypresses is unverified — it needs an oracle run of the record screen, and it is
cosmetic (the *time* is stored and read back correctly). Only the Ancient Lake /
car combination has been exercised; other tracks and the hovercraft/plane record
slots are untested, as is a *trophy* race finish, which takes the
`race_finish_adventure` balloon-cutscene branch rather than `postrace_start`.

#### OPEN: ghost coverage is one (track, vehicle) pair of 47

**Corrected 2026-08-07 — the coverage is 46 of 47, not 1 of 47.** The heading is
left as written so the index links in [`README.md`](README.md) keep resolving;
retitling it and the two index rows that point at it is a follow-up that has to
move both files at once.

**The gap as reported.** `timetrial_ghost_read()` — the same function root cause
3 above found a stack-buffer-overflow in — was exercised end to end (write, then
fresh-process read) by exactly one test on one (track, vehicle) pair:
`tests/check_race_finish_time.py` on Ancient Lake / car
([`tests/README.md`](../../tests/README.md)). `check_vehicle_sweep.py` and
`check_track_sweep.py` cover all 47 legal (track, vehicle) combinations for
ordinary racing, but contain **zero** ghost references — they do not drive a
Time Trial finish, so they never reach the ghost write/read path at all. This
is a save-format path: ghost payloads are persisted to the same Controller
Pak / EEPROM records ordinary saves use, and this exact path already shipped
a stack-buffer-overflow that "aborted with nothing at all on stderr" (root
cause 3 above) before it was caught.

**What closed it.** `tests/check_ghost_matrix.py` drives the other 46 pairs,
three runs each in a private `MDKR_SAVE_DIR` because 47 ghosts do not fit in the
pak's 6 slots: measure the frame at which the post-race OPTIONS stage offers SAVE
GHOST, write the ghost from a fresh save directory on an identical re-drive, then
read it back **in a separate process** and assert the same pair, node count,
character and time come out and that playback runs from a player bank. The
written header is located in the pak image by its measured contents rather than a
hardcoded offset, so a layout or byte-order regression in the serialiser fails
here instead of sliding past an offset-keyed probe, and the read run asserts the
pak is byte-identical afterward — an identical re-drive cannot beat its own time
and must not rewrite player data. The constraint this entry set was met: the
serialized ghost layout was not altered to make the sweep pass. Reference run
(us.v80, `build-rel`): 46 pairs round-tripped through a fresh process, 1
documented non-producer, 15.4 min, with three pairs needing the fallback cadence
(19 hovercraft, 30 plane, 33 hovercraft). See
[`tests/README.md`](../../tests/README.md), "Ghost matrix".

**What is still not driven — the 47th pair.** Spaceport Alpha (15) in the car
completes no lap on either simulation cadence: its autopilot racing line
dead-ends at `courseCheckpoint` 10, and DKR records a ghost only for a course
time under 10,800 frames (`objects.c`, `race_finish_time_trial`). The pair is
legal — the ROM's `available_vehicles` mask offers the car — so it is asserted to
behave exactly that way rather than skipped, and the check fails if it ever
starts finishing or if it starts aborting. That first failure is the promotion
trigger. Until then nothing drives that pair's ghost round trip, and no claim
here says otherwise: what is proven for it is that it produces no ghost, not that
its ghost survives a save and a reload.

### Part D — the Adventure trophy championship is DONE (wave "trophyseries")

`tests/check_trophy_series.py` enters the unlocked Dino Domain cabinet through
`obj_loop_trophycab`, then drives the production round → race → post-race →
rankings loop. The test controls only three boundaries: cabinet collision for a
post-quit resume that would otherwise cross a nearby exit, completion after 600
real race frames, and a validated finish-order permutation at rankings init.
It never writes championship points, ranks, trophy bits, cinematics, or EEPROM.

The measured matrix covers all 16 authored rounds:

| world | tracks | final branch |
|---|---|---|
| Dino Domain | 5, 3, 29, 7 | 32-point tie; stable racer order; gold |
| Snowflake Mountain | 8, 4, 10, 30 | second; silver |
| Sherbet Island | 13, 6, 9, 28 | third; bronze |
| Dragon Forest | 19, 18, 20, 31 | fourth; no cinematic and no award |

The gold arm starts with trophy bits zero, observes the final trophy cinematic,
decodes a checksum-valid persisted `0x3`, starts a second process, and sees the
cabinet instantiate state 3 from the reloaded save. A malformed duplicate-racer
order is rejected before touching `starting_position`. The quit arm uncovered a
production bug: rankings accepted cursor movement onto **QUIT TROPHY RACE**, but
`RANKINGS_EXIT` ignored `gMenuOption` and always initialized the next round.
It now returns to the Adventure lobby without an award; a fresh process consumes
that EEPROM, re-enters the cabinet, and starts round zero with zero points.

## Race gameplay — tiny racer models + crawl speed — FIXED (single root cause)
Two user-reported in-race defects turned out to share ONE root cause, plus a
follow-on cascade of latent decomp UB that only became reachable once racers
actually move.

- **ROOT CAUSE — fixed-point trig amplitude was halved.**
  `platform/math_util_native.c`'s WEAK `sins_s16`/`coss_s16`/`sins_2` scaled by
  `32767.0f` (0x7FFF). DKR's fixed-point trig convention is amplitude **0x10000
  (65536)**: the real hand-asm (`src/hasm/ido/math_util.s`) reads a u16 sine table
  and `sll v0,1` (×2) it, so 1.0 → 0x10000, and every caller assumes that scale —
  matrix builders do `sins_s16(a) * (1.0f/0x10000)` (`game/src/hasm/math_util.c`),
  integer callers do `(sins_s16(a) * v) >> 16`. At 0x7FFF every sine/cosine came
  out ≈0.5×, so every rotation matrix's 3×3 was ≈0.5× and products like cos·cos
  were 0.25×. Measured at runtime: `mtxf_from_inverse_transform` with all-zero
  rotation produced `mtx[2][2]=0.25` instead of 1.0.
  - **Tiny models:** object/racer model matrices (`mtxf_from_transform`) rendered
    at ~0.5× rotation-scale on top of the object scale → karts drew ~¼-size. The
    drop-shadow uses a separate `shadowScale` path, so it stayed full-size — the
    tell that world position was correct and only the model matrix was wrong.
  - **Crawl speed:** the racer's velocity is transformed local→world through the
    SAME matrix builders (`func_8004F7F4`: `mtxf_from_inverse_transform` +
    `mtxf_transform_point`), so a forward velocity of −0.73 became world −0.18
    (×0.25). The collision feedback loop then pinned `racer->velocity` to the
    achieved motion, so it locked at a constant ~0.18 world-units/frame with no
    acceleration. Fixed: local −12.0 → world −12.0 (1:1); racer now accelerates
    from standstill to a steady ~12 units/frame top speed.
  - **Fix:** scale by `65536.0f` in the three s16 trig stubs (float `/0x10000` and
    integer `>>16` paths both become exact). `sins_f`/`coss_f` (true [-1,1]) were
    already correct and unchanged.

- **Cascade (latent N64 UB, only reachable once racers drive — all decomp-faithful,
  fixed NATIVE_PORT-gated):**
  1. `racer.c` `func_8005B818`: five spline-control arrays declared `[4]` but the
     fill loop writes `[0..4]` and `cubic_spline_interpolation` reads
     `data[index..index+3]` with index up to 1 → touches `[4]`. Sized to `[5]`
     (host stack canary / SIGABRT otherwise).
  2. `weather.c` `lensflare_render`: passed a bare 24-byte `ObjectTransform` as
     `(Object*)`, and `render_sprite_billboard` reads `obj->animFrame` (Object
     offset 0x18, one slot past). Backed by a full `Object` via a pointer alias so
     `animFrame` is in-bounds (ASan stack-buffer-overflow / intermittent SIGSEGV).
     (The boost renderer `func_800135B8` was already safe — its
     `ObjectTransform_800135B8` is 0x1A with `unk18` exactly at 0x18.)
  3. `racer.c:320` `func_80042D20`: `D_800DCDA0[racer->racePosition]` — the array
     has 8 entries but `racePosition` is 1-indexed (1..8), so 8th place read one
     past the end. Index clamped to `[0,7]` (every trailing entry is 2, so 8th
     place keeps the intended value).

- **Verification:** ASan race run (`build-asan`, 4300 frames) clean; optimized
  `race_drive` 20× headless = 0 crashes; title 300f + character-select + options
  navs stable; frame capture at frame ~2880 shows full-size karts on Ancient Lake with HUD.
  All runs muted + headless (`MDKR_AUDIO=0 --headless-frames`).

- **Note — `math_util_native.c` (recorded here under its former name
  `math_stubs_temp.c`) is temporary bring-up glue.** These trig stubs are
  WEAK sinf()/cosf() approximations, not bit-exact to the ROM sine table. A future
  `hasm_native/math_util.c` should port the real table-interpolated `sins_s16`
  (`src/hasm/ido/math_util.s`) for exact fidelity; until then the amplitude is now
  correct (0x10000) so all matrix/rotation math is right to within sinf() rounding.
