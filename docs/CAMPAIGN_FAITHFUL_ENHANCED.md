# Campaign: faithful Enhanced cadence

**Goal.** Enhanced runs the game at 60 Hz with the gameplay a player would get
on Original. Original itself does not move a single instruction. Alongside it:
code structure and repo hygiene enforced by gates rather than by intention, and
no AI-slop prose anywhere a player or a reviewer can see it.

This is the plan of record. It is written to be falsifiable: every milestone
has a gate that fails when the milestone is not met, and a positive control
that proves the gate is not vacuous.

---

## 0. What is provably impossible, stated up front

"Enhanced with zero deviation from Original" is not reachable, for three
independent reasons, each sufficient alone:

1. **RNG consumption is per-tick.** Twice the ticks is twice the draws.
   `EVENTHASH` and `SIMHASH` diverge at tick zero, not gradually — the same
   mechanism that makes `check_state_hash`'s legacy-RNG control diverge
   immediately.
2. **The integrator is nonlinear.** `velocity -= velSquare * traction` applied
   twice at half strength is not the same as once at full strength. No choice
   of scaling constant fixes this; it is a property of the arithmetic.
3. **The authored state is fixed-point.** `s16` rotations, the 2-bit UV
   accumulators, `x_rotation += angle >> 3`. Half-steps quantise differently
   and the residue accumulates with a sign rather than cancelling.

Chasing bit-identity would therefore produce a gate that can only be satisfied
by lying. The contract below is the honest one, and it is stricter where it can
be and explicit where it cannot.

**THE CONTRACT.** At authored 30 Hz tick boundaries, under Enhanced:

| stream | requirement |
|---|---|
| `EVENTHASH` | **bit-identical** to Original |
| `INPUTHASH` | **bit-identical** to Original |
| `PCM` | **bit-identical** to Original |
| `SIMHASH` | divergence bounded by a published tolerance over a full race |

Original's own streams stay bit-identical to what they are today, always. That
is not a target; it is a precondition on every commit in this campaign.

---

## 1. The architecture: split the tick

A DKR tick welds together two things, and only one of them resists
subdivision.

* **Discrete** — RNG draws, checkpoint crossings, item logic, AI decisions,
  timers, audio cue triggers, lap and finish events. These are events, not
  integrals. Subdividing them is meaningless, and it is what breaks the
  streams.
* **Continuous** — position and velocity integration, camera solve, animation
  phase, suspension and pitch. These are integrals over time. Subdividing them
  is what they are for.

Enhanced runs **discrete logic only on authored boundaries, in authored
order**, and **continuous integration at 60 Hz with correct dt**. That is what
makes the first three rows of the contract achievable at all.

Issue #26 is the evidence for this split. The boss was not merely moving
faster — it was *deciding* twice as often, and the wheel-0 traction hole turned
that into an unbounded runaway.

---

## 2. Milestones

### M0 — Classify every per-tick site
The ~59 `//!@Delta` markers (49 `racer.c`, 4 `object_functions.c`, 6 `menu.c`)
plus the boss start-timer constants in `vehicle_*.c` are the inventory. Each
gets one of four labels: CONTINUOUS (scale by dt), DISCRETE (gate to authored
boundary), THRESHOLD (scale the bound, as the menu auto-repeat fix does), or
INERT (equilibrium-governed or already scaled — proven, not assumed).

**Gate:** `check_delta_inventory.py` — every `//!@Delta` in the tree carries a
classification comment; an unclassified one fails. **Positive control:** adding
a bare `//!@Delta` fails the gate.

### M1 — Delta-correctness sweep
Apply the M0 classification. Additive terms take `× updateRateF * 0.5`; decays
become `k^(updateRate/2)`; thresholds scale. Removes the 2× acceleration
transients and the measured 3–8% AI lap delta.

**Gate:** extend `check_bluey2_rematch` and add an ordinary-racer arm asserting
Enhanced/Original lap-time parity within tolerance on a non-boss track.
**Positive control:** `MDKR_BOSS_CADENCE_COMPAT=0` still reproduces the runaway.

### M2 — Wheel-0 traction hole
`update_car_velocity_ground()` samples drag from `wheel_surfaces[0]` alone,
while the human path `func_80050A28()` averages all contacting wheels. Under
Enhanced only, use the averaging form when wheel 0 has lifted. This makes the
#26 clamp a backstop rather than the mechanism.

**Gate:** boss peak `|velocity|` at or under the authored governor with the
clamp *disabled* — i.e. the physics is right without the safety net.

### M3 — The discrete/continuous split
Gate discrete call sites to authored boundaries. Large but mechanical once M0
exists.

**Gate:** `EVENTHASH`/`INPUTHASH`/`PCM` bit-identical between cadences on three
routes; `SIMHASH` divergence within the published bound.

### M4 — Input latency, measured before it is optimised
The tick quantum is the second-order term; the pipeline may dominate.
Instrument input→photon and publish the budget split before deciding whether
M3 earns its cost for latency specifically.

**Gate:** a recorded budget with a method others can re-run.

---

## 3. Code quality gates

These are lessons this repo has already paid for. Each becomes a gate so the
lesson does not have to be re-learned.

1. **Cadence gating keys on launch-time cadence, never on `updateRate`.** Under
   Original a lag tick legitimately arrives with `updateRate` 3 or more, and
   the authored code must handle it byte-identically. A grep gate rejects
   `updateRate == 1` / `updateRate == 2` used as a mode test.
2. **Every behavioural fix ships a positive control that fails without it.**
   `check_bluey2_rematch` sat green for months while asserting the defect it
   was written to catch. A gate with no failing control is a gate with no
   evidence.
3. **No `git add -A`.** This campaign already produced one commit whose message
   described three renderer fixes while the commit also contained an unreviewed
   launcher redesign and a gameplay change. Stage explicit paths.
4. **A root cause is not closed until its class is swept.** #25 and #27 were the
   same function failing to save borrowed state; sweeping that class found two
   more defects nobody had reported.
5. **A measurement is not believed until its subject is confirmed.**
   `MDKR_LOAD_TRACK` only binds once the route reaches track select; a short run
   silently races a default level. Confirm via `[TRACE] level_light: level=N`.
   This cost three wrong measurements in one day.

---

## 4. Prose quality

**The rule:** anything a player or a reviewer reads should sound like a game
developer wrote it — short, concrete, specific. No process vocabulary in
player-facing text. No sentence whose purpose is to sound thorough.

Banned in player-facing strings: *seamlessly, leverage, ensure, robust, enhance
your experience, comprehensive, authored presentation states, safe frame
boundary, durable storage, compatibility mode*. Release notes are for players:
no validation or process words.

**Structural finding to fix first:** player-facing strings are pinned by
contract tests across `CMakeLists.txt`, `macos/Scripts/verify_unsigned_release.sh`
and `tests/ci_contract_manifest.py`, so improving one sentence is a five-file
change. That is *why* the slop survived. The pinning should assert the claims
that matter (no false product claims) rather than exact prose.

**Gate:** `check_player_prose.py` — banned vocabulary in player-facing strings
and release notes fails. **Positive control:** a seeded "seamlessly" fails.
