# Device acceptance — motion smoothing, re-test after the 2026-08-07 rejection

*Candidate build: this branch (`codex/v1.0.3-reliability-ux`), cross-built from
the release tree. Every line below traces to a shipped change or a named,
still-open gap. Motion smoothing stays opt-in until this session returns a
verdict — a device pass is necessary before the default can move; the
instrumented evidence this branch produced is not a substitute for it, by
standing owner policy recorded at the 2026-08-07 rejection.*

## 1. What changed since the rejection

On 2026-08-07 the owner played Motion smoothing on a 120 Hz display and
rejected it: "lots of artifacting or other issues with the way it looked,
making it preferable to disable" — no track or vehicle named. The diagnosis in
[`docs/evidence/smoothing-artifact-repro-2026-08.md`](evidence/smoothing-artifact-repro-2026-08.md)
found nine candidate artifact classes at 120 Hz (C1–C9), reproduced the one
large enough to match an unprompted complaint, and closed or dispositioned all
nine. A perceptual gate now stands over the mechanism gates so a regression in
any of these has to break a test, not just look wrong on a TV.

| Class | What it was | Fix | Witness |
|---|---|---|---|
| **C1 — effect shell rides one tick ahead** | The effect shell's position residual was measured against the wrong snapshot, adding a full authored tick of racer travel to its world position. Measured on the shield, on the diagnosis route: the shell sat **50.8 px** from its own tick's authored pose — flat across the alpha grid, not a ramp — against a 5.25 px per-tick travel budget, and lost 65% of its area to recession and kart-body occlusion on every interpolated frame. This is the shipped defect: v1.0.1–v1.0.3 all carried it. The shield is the measured instance; the magnet effect shell goes through the same `mdkr_camera_replay_effect_world` path and the evidence doc states twice that it was never itself run — the fix should apply to it by shared code, unwitnessed. | `mdkr_camera_replay_effect_world` now hands the tick-T capture to the base transform so the residual cancels (`b3186a9`, "anchor the effect shell to its own tick"). A second instance — billboard recipes stamped one tick behind at abandoned-pass boundaries — was found by the same structural witness and closed the same way (`presentation_task_authoring_begin` discards live registrations on a new authoring lifetime). | `97cea1d` gates the pixel envelope (`tests/check_effect_shell_envelope.py`) **on the shield only**; the structural witness `ownertickmismatch` (which does cover both) goes from 717 to 0 across 2,404,110 checks on all eleven bisection arms. §4 of the evidence doc, and "Explicitly open" / §4.6: "the magnet path is unmeasured as such... no arm in this note or in the gate forces a magnet rather than a shield." |
| **C2/C3-adjacent — camera blends across a cut** (3 classes: post-race spectate handover, 3P T.T. spectator cut, camera-mode reframe) | Each cut moves the eye outright but stayed inside the teleport threshold and camera slot, so nothing told the replay it was a cut — the interpolator blended across it instead of snapping. | `0c6cb33`, `48d79d6`, `969bd2e` — one fix per cut class, each classified and never blended. | `tests/check_camera_snapshot_coverage.py`'s cut classifier (`6bffbc2`): 12 blended cuts before, 0 after. |
| **C3 — rotation smears past a quarter turn** | An interpolated angle advancing more than a quarter turn per tick would smear the long way round instead of snapping. | `2dafae2`, "snap interpolated angles past a quarter turn per tick." | Not the diagnosis route's artifact (the shield's phase rate stays under the snap there; the magnet's phase rate is not established), but closed and gated for any content that does cross it. |
| **C4 — replay reads storage it does not own** | 18 non-arena port static display lists (`dRdpInit`, `dRspInit`, the render-settings table, dialogue/transition lists) plus 4 already-owned segment-token resolutions were read live mid-replay with no retained copy — by the time an interpolated walk ran, the next authored tick could already have overwritten them. | The real walk now copies the 18 static lists (`dkr_capture_nonarena_list`); the retain path short-circuits the 4 already-owned resolutions before the dependency lookup; and the replay fails closed on anything still uncaptured — held authored image instead of a live read (`26526db`, "refuse interpolated replay over an uncaptured external"). | `uncapturedext=0` on all eleven production arms, 114,957 interpolated replays; the forced-miss control refuses 1,779 of 1,791 walks and turns them into held frames, proving the refusal path isn't vacuous. §§4.4, "uncaptured-external fail-closed arm" in [`smoothing-stage-attribution-2026-08-08.md`](evidence/smoothing-stage-attribution-2026-08-08.md). |
| **C5 — WebGPU swap chain one frame deeper than needed** | `desiredMaximumFrameLatency` was unpinned, adding latency and letting the pacer's phase estimate drift from what the display shows. | Pinned to the backend minimum (1) at both configure sites — the engine's own and the app shell's adopted window (`e9fa33a`, "pin the WebGPU swap chain to one frame in flight"). | `[PRESENT-MODE]` reports `frameLatency=1`; `[SURFACE-CONFIG] owner=app-shell` reports `frameLatency=1` for the adopted window; four negative controls (missing row, depth-2 shell, depth-2 engine) confirm the assertion bites. **Real-display pacing feel and end-to-end throughput are unmeasured — see §3.** |
| **C7 — tick-boundary step** | Content no interpolation stage covers only advances at the authored tick, so the step into the authored endpoint reads larger than the interior steps. Shipped 1.0.3 production measured +16.68%, most of it C1 riding on the same boundary. | No separate fix — C1's anchoring fix removed most of the step. | After the fix: +3.73%, a 4.5x reduction. What remains is significant only in the 2D HUD (+10.00%, 29/29 ticks); the other 81% of the frame is not statistically distinguishable from zero (95% CI −0.30%..+3.29%). Dispositioned benign; §5.1 of the evidence doc. |
| **C8 — UV-scroll phase holds** | Scroll batches that can't be identity-matched hold their authored phase for a tick rather than advance, so a waterfall could tick in place while the camera glides. | No fix — this is the fail-closed refusal the identity-match contract exists to make, not a defect. | 14.56% hold rate is rate-invariant by construction (decided once per tick, repeated at every alpha); in-race the diagnosis route holds only before the race starts. Dispositioned benign; §5.2. |
| **C9 — primitive-alpha overrides that reach no pixel** | 119,129 interpolated alpha substitutions changed nothing on the sampled window. | No fix — the zero is the sampling window, not the stage; the substitutions land in scenes the window never reads. | Stage cost under 7.0 us per replay of 546 us. Dispositioned benign; §5.3. |
| **Perceptual regression gate** | The mechanism gates above each watch one stage; nothing stated the player-facing rules directly. | New `motion_quality_battery` check, six rows (R1–R6) in player vocabulary: a kart may not spin the wrong way round, a respawn may not smear, a camera cut may not blend, time may not run backwards, the authored tick may not thump. | `tests/check_motion_quality_battery.py`, wired into `tools/run_checks.py` (`e7e5817` adds the battery, `7774849` witnesses R5's dump window). Runs on every full-suite pass from here on. |

## 2. The protocol

**Build and config.** This branch's build, on the 120 Hz display the
rejection happened on.

- `Video.FrameLimit = display` (Match Display)
- `Video.MotionSmoothing = interpolate`
- Presentation pace = Smooth is the one-click equivalent of the two rows
  above; either path is fine as long as the config the session lands on has
  both set this way.
- Confirm the display is actually running 120 Hz before starting — the OS
  display settings, not an assumption from the monitor's max spec.

**Routes.**

1. **Diagnosis route (required): Jungle Falls, level 29, 3-lap, with shield
   pickups.** This is the exact route the artifact repro used, and the one
   most likely to reproduce anything still present. Pick up and hold at least
   one shield per lap, at speed, so the effect shell is live on screen for
   sustained stretches — not just a single tap-and-release. **Also pick up and
   hold a magnet at least once, on this route or your own play** — the fix
   only measured the shield; the magnet shell goes through the identical code
   path but has never itself been watched, and this session is the first
   chance for a human eye to close that gap (see §3).
2. **Your own free choice of track(s) and vehicle(s).** Whatever you'd
   naturally play. This branch's diagnosis only drove one route under
   autopilot; nothing here rules out a different one showing something new.

**What to look for, mapped to what's now fixed:**

- **Shield shell riding ahead of the kart, or a washed-out halo behind/above
  it instead of over it** — this was C1, the shipped defect, measured and
  fixed on the shield. Should be gone: the shell should sit on the kart, not
  detached up-track, through pickup, hold and release. **Magnet shell: same
  check, but this is the first time anyone has looked** — the magnet takes
  the same fix through the same code path, unmeasured until now; report what
  you see either way.
- **The camera popping or snapping unnaturally at a scripted cut** — post-race
  spectate handover, the 3P time-trial spectator cut, any camera-mode
  reframe. These now hard-cut instead of blending; if you see a pop, that is
  the fixed behavior, not a regression. What you should NOT see is a smeared,
  swimmy blend across one of those transitions.
- **A kart, or anything else, visibly spinning or turning the "wrong way
  round"** on a fast rotation (post-collision spins, tight hairpins at speed)
  — this was the quarter-turn smear class. Should be gone.
- **Any general "artifacting or other issues"** — re-test against the exact
  wording of the 2026-08-07 verdict. That verdict named no specific defect;
  it named a feeling. The specific mechanisms above are what this branch
  found and fixed, but the bar for this session is the original bar: does the
  picture still look wrong in a way that would make you turn smoothing back
  off, regardless of whether it matches a named class.

## 3. Open items this session can uniquely settle

The headless/GL harness that produced §1's evidence cannot witness these —
they need a real display, and several need an un-occluded foreground window
that this branch's host could not obtain (every command-launched window on
that host reports occluded to the Metal backend). This session is the first
chance to close them:

- **The magnet shell, unmeasured.** C1's fix is shared code between the
  shield and the magnet, but only the shield was ever run through the
  diagnosis or the pixel gate — the evidence doc says so twice ("the magnet
  path is unmeasured", "no arm... forces a magnet rather than a shield").
  Picking up a magnet during this session and watching whether its shell
  stays anchored is a direct, human-eyes close of that gap; nothing headless
  has done it.
- **C5's real-display pacing feel.** The swap-chain depth pin is landed and
  gated structurally (`frameLatency=1` on both configure sites), but no run
  here has measured whether it feels right, or costs anything, on an actual
  120 Hz panel — only that the config takes.
- **The FPS-overlay arm of `tests/check_app_adopted_pacing.py`.** Every other
  arm of that gate passes, including both frame-latency witnesses. This one
  arm has never run to completion anywhere in this branch's history because
  it needs an un-occluded foreground window (see
  [`docs/evidence/present-perf-baseline-2026-08-08.md`](evidence/present-perf-baseline-2026-08-08.md)).
  If it's convenient, running the launcher's FPS-only WebGPU pass in the
  foreground during this session would close a gap nothing else can.
- **The WebGPU depth-1 pin's end-to-end present throughput.** The pin's
  latency benefit is argued from the API docs ("CPU and GPU cannot run in
  parallel" at depth 1), not measured — every run so far presented zero
  frames. Whether it costs anything visible on a GPU-bound scene at 120 Hz is
  exactly the kind of thing a hands-on session, not a headless one, can tell.
- **The un-occluded `[SURFACE-CONFIG]`/`[PRESENT-MODE]` `frameLatency=1`
  rows, on a live present.** They're witnessed at configure time; nobody has
  watched them hold across resize, present-mode re-rank, or a surface
  recovery on a real window in front of a real user.

None of these are reasons to expect a different verdict on the artifact
question — they're separate, narrower gaps that this branch's evidence
explicitly could not close from a headless host.

## 4. Decision rule

**ACCEPT** — the picture, on the diagnosis route and your own choice of
content, no longer shows what the 2026-08-07 verdict described. Then: flipping
the default is a separate, small change (Presentation pace already has a
"Smooth" quick choice; making it the default is one config-default edit plus
player-facing copy) — **listed here as the next step, not executed by this
protocol or this session.**

**REJECT** — record the verdict verbatim, the same way 2026-08-07's was
recorded, including the route and content that showed it. That verbatim
verdict opens a new Phase 0 diagnosis scoped to the named artifact, the same
shape as the one that produced §1 — a repro doc, a witness, and either a fix
or a disposition, before the next device-acceptance attempt.
