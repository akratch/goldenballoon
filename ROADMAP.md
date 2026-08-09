# Roadmap

What is **not** done as of v1.0.5, why it was deferred, and what would have to be
true to take it up. Nothing here is promised and nothing here has a date; this
is a statement of scope, so that "not implemented" is never mistaken for
"overlooked".

The rule the rest of this project runs on applies here too: a claim needs a
measurement behind it. Where an item has evidence, it is linked. Where the
honest answer is that something has not been measured, it says so.

**About the issue IDs.** Headings below carry short identifiers inherited from
the audits that raised them, so that a roadmap entry and its original write-up
can be matched up — the same identifiers appear in
[`docs/DEVELOPER_HANDBOOK.md`](docs/DEVELOPER_HANDBOOK.md) and
[`docs/open-items/`](docs/open-items/README.md). The prefixes name the audit
that raised the item: **F-** foundation completeness, **IQ-** image quality,
**WGPU-** WebGPU correctness and portability, **MEM-** allocator and native
memory, **PORT-** portability, **AUDIO-** audio, **GAME-** gameplay, **C-**
correctness. The number is that audit's own item number; it carries no priority
and implies no ordering.

Related reading: [`CHANGELOG.md`](CHANGELOG.md) for what has landed,
[`docs/open-items/`](docs/open-items/README.md) for defects with their mechanism
and evidence, and [`docs/RELEASE_CHECKLIST.md`](docs/RELEASE_CHECKLIST.md) for
the gate a release has to pass.

---

## Correctness and fidelity

### F-18 — independent state reference breadth

**Where it stands.** Two lanes compare the port's own simulation against an
independent reference: a full Ancient Lake lap, and an all-racer Bluey 2 lane
that isolates the reported boss-speed divergence. The Bluey lane settled the
simulation-cadence question — retail and Original finish at ticks 3,459/3,458
with a 1.00065× speed ratio, against 3,022 and 1.13982× for the Enhanced
one-field arm — and `Gameplay.SimulationCadence=original` is the default because
of it. Ancient Lake now also has a fail-closed diagnostic that replays the
real ROM's observed update widths and input states. It makes checkpoint clocks
0–3 exact and moves the first five-unit position separation from clock 18 to
767, classifying the early mismatch as timestep partitioning rather than a
different game-speed policy.

**What remains.** Everything the two lanes do not cover: challenge breadth,
multiplayer, progression and save, audio, renderer state, and the
higher-rate rendering experiments. Ancient Lake still develops
sub-unit floating-point drift into a different long-horizon open-loop line
(39.241% authored and 67.747% reference-replay checkpoint agreement), so it is
retained as a strict red instrument rather than declared equivalent through a
permissive tolerance.

**Condition to take it up.** A reported divergence that one of the uncovered
areas would explain. See
[`docs/open-items/gameplay.md`](docs/open-items/gameplay.md).

### Two oracle questions left open

- **No current route actually crosses a zip pad under measurement.** The old
  44.9-versus-23.2 claim no longer reproduces; when armed identically, the
  boost peaks at 22.357 with eight racers and 22.336 solo (0.09% apart), and a
  source audit finds all 310 boost/velocity statements byte-identical to the
  decomp baseline. A real pad crossing plus ROM trace is still absent.
- **The `race_karts` oracle route scores 0.636** where frontend routes score
  0.855–0.998, and nobody has investigated why. It is the only in-race route, so
  the number is either a real in-race fidelity gap or an artefact of the route.
  Worth time-boxing rather than leaving as a permanently unexplained figure.

Both are recorded with their measurements in
[`docs/open-items/gameplay.md`](docs/open-items/gameplay.md) and
[`docs/ORACLE.md`](docs/ORACLE.md).

### Campaign completeness

**Where it stands.** This entry used to call the late campaign the largest
single piece of deferred work in the project. It is not, and has not been since
the closure campaign of 2026-08-07 —
[`docs/DEFINITION_OF_DONE.md`](docs/DEFINITION_OF_DONE.md) is the current record.
`check_campaign_progression.py` gates each late progression **seam** with a save
fixture entering it and an assertion on what production writes leaving it:
silver-coin collection by the game's own coin objects and its EEPROM round-trip
read back by a second process; all four boss rematches won, each on the EEPROM
the previous one persisted, with `wizpigAmulet` climbing 1, 2, 3, 4; the Wizpig 1
unlock in both directions (four amulet pieces redirect the hub to the mouth
sequence, three do not) followed by Wizpig 1 raced and won; and the Wizpig 2 win
that sets and persists `bosses & 0x20`, the single value `menu_credits_init`
reads to choose the true ending. The trophy championships have their own gate,
`check_trophy_series.py`, across all 16 authored rounds.

**All three residuals are closed** (2026-08-09). Each was a headless-driving
obstacle rather than a missing feature, and each is now driven:

1. **The lobby's boss-rematch door is driven**, not retargeted. The recorded
   stall at (-1295, 685) reproduces; a waypoint east of the wall at (-300, 700)
   takes the exit at frame ~3124. Both arms are kept — the `MDKR_LOAD_TRACK`
   retarget remains the fast path, the driven arm is the completeness proof.
   `tests/route_plan.py` supplies the route memory the drive hook lacked: it
   fails loudly on oscillation, on a stalled closest approach, and on a
   per-waypoint frame budget, instead of retrying forever.
2. **Trophies and the T.T. amulet are chained**, not premised. `trophies` climbs
   0x3→0xf→0x3f→0xff and `ttAmulet` 1→2→3→4 across separate processes on one
   another's EEPROMs, and `check_future_fun_land.py` witnesses the lighthouse
   unlock on the save the chain produced — with the negative arms that a silver,
   or a missing Wizpig 1 bit, does not open it.
3. **The credits screen is reached**, and the recorded obstacle turned out to be
   a symptom. `func_8006C300()` is never non-zero — zero on all 29,929 sampled
   frames, and structurally so, because `game_load_level` zeroes its backing
   global on every load and the only writer is the redirect branch for a repeat
   Wizpig entry. The A presses could never have popped anything. The real cause
   was that all 159 of the level's animation objects sat deactivated: every
   Future Fun Land header is `RACETYPE_HUBWORLD`, so the world-arrival branch
   ran on a cutscene level and overwrote `gCutsceneID`, because the fixture did
   not carry the "arrived in Future Fun Land" flag. That is a fixture defect, not
   a game defect — a player cannot reach Wizpig 2 without entering that hub — so
   the fix is in the fixture and no `game/` behaviour changed.

**What is still not claimed.** There is deliberately no single continuous
start-to-credits run. The campaign check's own reasoning stands: it would take
hours and fail as one opaque blob, and composing witnessed seams is the better
design. Future Fun Land's own internal state also remains outside these gates.

### Camera obstruction correction — ships opt-in; default-on rejected on device

**Where it stands.** The projection-derived lens guard is ported and compiled:
the sweep kernel, resolver, transform adapter, static and dynamic occlusion
caches, target-readability classification, and the fixed-tick runtime that owns
each of the eight authored camera slots
(`game/src/camera_obstruction_runtime.c`, `platform/camera_obstruction*.c`).
Rendering no longer computes a lens; it consumes the projection record the
finalizer latched. The runtime policy comes from `MDKR_CAMERA_OBSTRUCTION`, and
**unset selects Observe** — the authored pose, retained and only measured.
Modern, where every authored slot is resolved and no pinned route publishes a
penetrated, degraded, or invalid pose, is the player-facing opt-in, reached
through the launcher's Camera setting or the variable directly. It was made the
default on 2026-08-07 and reverted the same day: device acceptance found the
corrected camera too sensitive in play, and no instrumented gate had caught it.
`center-ray` and `legacy` remain in the same binary as diagnostic arms and as
the required broken-direction controls, and a misspelled value falls back to the
default rather than silently correcting a camera nobody asked to correct or
silently selecting an unqualified arm. `Camera.Comfort` ships beside it: a
presentation-only reduced-motion opt-in, off by default.

**What remains.** Several CAM-00–CAM-09 exit gates in
[`docs/architecture/camera-obstruction.md`](docs/architecture/camera-obstruction.md)
are still open, and every one of them is *breadth of evidence* rather than a
known defect — differential fuzzing corpora, GCC/wasm32 sanitizer-equivalent
runs, soft-occluder enrollment and its pixel proof, WebGPU/browser and
resource-plateau breadth. Manual motion review is no longer merely open: it ran
on device and rejected the default. §10.1 of that document records which rows
the flip rested on, which it did not wait for, and why passing all of them was
not sufficient — MOTION-01's thresholds need recalibrating against that verdict
before another flip is attempted.
They are now open *behind* the default rather than in front of it, which is
survivable for one reason: the resolver substitutes only at presentation depth,
so no open gate can turn into moved authoritative state, and `Camera.Obstruction
= observe` is a shipped, tested setting rather than a build. The underlying
defect — gameplay cameras entering terrain and object geometry — no longer
stands in default play, and stays open in
[`docs/open-items/gameplay.md`](docs/open-items/gameplay.md) on breadth alone.
A centre ray, fixed-radius clamp, terrain-only spring arm, or void-curtain mask
is still explicitly a partial mitigation rather than the fix.

## Renderer

### WGPU-11 — external and oracle corpus breadth

**Where it stands.** The local closeout is done and is not small: 46 native
menu, course, hub and multiplayer routes execute 249,339,186 strict commands
with zero faults across 29 material IDs and 37 material/pipeline keys; the
offline gate validates all 445 supported-ROM models, 1,146 level segments and
16,943 batches including dormant variants; forced 4 KiB segmentation and a
one-entry shader-index limit prove safe continuation, one same-backend rebuild,
and terminal failure without a renderer switch; and the 113-point fault
registry is fully classified.

**What remains.** Everything that needs hardware or a second implementation:
complete-corpus and minimum-feature runs on browser hardware, the independent
state reference above, and external native platforms. No amount of local work
closes this; it needs machines. See
[`docs/open-items/renderer.md`](docs/open-items/renderer.md).

### IQ-8 — WebGPU 4× MSAA

Deferred, and its value has fallen since it was written: supersampling ships and
is on by default at 2×, which already addresses most of what MSAA was for here.
It stays on the list because MSAA is cheaper than SSAA at equal edge quality, so
there is a real reason to want it on lower-end GPUs — not because the image is
currently unacceptable.

### IQ-11 — texture-pack loader

A **loader only**. If this lands it must extend the ROM-absence guard rather
than sit beside it, and it must ship no content whatsoever: the guard that fails
the build closed if any shipped file contains ROM data is the reason this project
can exist, and a texture-pack path is exactly the kind of feature that erodes it
by accident. No pack format is designed, and none will be until the guard
question is answered first.

### Presentation rate above the authored tick

The unsafe 1.0.1 retained-replay experiment is retired, but the replacement is
now a production feature. Optional **Motion smoothing: Interpolated** owns a
private copy of each complete graphics arena plus the immutable adjacent task,
then produces presentation-only in-between images. It never advances physics,
AI, timers, input, events, audio, or saves. With Motion smoothing Off, extra
presentation opportunities hold the latest authored image instead.

Original remains the recommended/default Frame Limit. Match Display, numeric
caps, and native Uncapped can use the immutable presentation path when
Interpolated is selected; saturation deliberately holds an image rather than
blocking gameplay or audio. Fixed-ticket simulation, tick-indexed input,
independent audio service, bounded GPU queues, suspension rebasing, and
state/event/input/PCM invariance remain the authority boundary. The measured
contracts cover exact endpoints, distinct in-between images, lifecycle/device
loss, UI, cutscenes, split-screen, particles, vehicles, GL/WebGPU, and browser
presentation schedules.

The historical 1.0.1 decision was still correct: a replay that retained only
mutable display-list pointers could observe rewritten viewport, matrix, vertex,
texture, and nested-list storage after task `K+1` began. The present path does
not revive that design; its retained ownership and fail-closed generation rules
are the reason smoothing is now safe to expose. Remaining work is broader
physical-platform and DAC acceptance, not a disabled product feature.

### Shadow and visual leftovers

- A placement-sensitive shadow gate that has produced false results twice
  (medium, worth doing).
- One-frame caster lag and a decal/map inconsistency — both **accepted**, no
  action planned.
- Broader materials and water, and scene-wide atmosphere and VFX, are polish
  that must not gate anything.

## Platforms

### Windows

The portable x64 build is supported since 1.0.1. Hosted CI checks the product,
stock-Windows imports, package shape, ROM-free startup surfaces, and extracted
archive; the release candidate also passed manual WebGPU gameplay, controller,
audio, save, and relaunch acceptance on Windows hardware.

The UTF-8-to-UTF-16 filesystem/process boundary, extended-length path handling,
Unicode-path gate, and reviewed non-elevating/DPI/long-path manifest have landed
in source. Remaining portability work is user-selectable auto/Direct3D-12/
Vulkan adapter policy, richer textual GPU diagnostics, a hosted Windows pass of
the new boundary, and broader physical coverage across Intel, AMD, NVIDIA,
hybrid-GPU, Windows on Arm, and negative VM/RDP configurations.

### Linux

Best-effort. GL and WebGPU both build and the CI matrix includes ROM-free Linux
cells, but physical-GPU runs, Wayland (as opposed to X11), and controller
hotplug/rumble breadth are all unmeasured. This is platform-acceptance breadth
— it needs physical machines, not a source fix — and has no mechanism or
measurement of its own to record, so unlike the numbered audit items above it
does not have a matching entry in [`docs/open-items/`](docs/open-items/README.md).

### Native app shell — shipped

**No longer deferred.** The native ImGui app shell (`platform/app/`) ships: a
first-run launcher with explicit user-selected ROM intake and complete-image
SHA-256 plus asset-table validation, a
settings panel generated from the video/gameplay schema with honest LIVE vs
RESTART presentation, an in-game F1 overlay, F10 FPS readout, and a diagnostics
log view. It builds on macOS, Windows (MinGW cross) and Linux, and the macOS
`.app` now launches it directly — the bash/AppleScript first-run picker shim
that stood in for it is retired.

The F1 overlay now owns a real simulation pause boundary, proven during a live
Time Trial by exact state hash, kart, race-clock, checkpoint, and lap stability.
Return-to-launcher uses the same orderly process replacement on POSIX and the
Windows wide-character runtime. The remaining native semantic-accessibility and
Linux file-picker limitations are recorded in
[`docs/APP_SHELL.md`](docs/APP_SHELL.md).

### Signing and notarization

The macOS engineering path is implemented. Local bundles are ad-hoc signed
after every Mach-O mutation so integrity remains valid; the protected manual
release workflow imports a Developer ID certificate into an ephemeral
keychain, signs inside-out with Hardened Runtime, notarizes/staples the app,
signs and notarizes/staples the DMG, mounts it, and requires Gatekeeper
acceptance. The remaining external action is configuring the protected GitHub
environment and completing the first accepted Apple notary run; see
[`macos/README.md`](macos/README.md). Windows signing remains open.

### Physical device breadth

Mobile touch controls have a real automated gate (a Chromium three-finger chord
that must reach the game's own controller read and return to exact neutral), but
automated is not the same as physical. Rotation, safe areas, toolbar collapse,
gamepad handoff and stuck-input soaks on real iOS and Android hardware are
outstanding.

## ROM support

The supported set is **US 1.1 and European 1.1**, which race byte-identically.
US 1.0, European 1.0 and the Japanese release are identified from their header
CRCs and **refused by name**. Supported images additionally require an exact
normalized whole-image SHA-256 and valid revision-specific asset-table bounds,
so a header-correct damaged or mixed dump cannot cross the launcher or engine
boot boundary.

Expanding that set is future product scope, and
[`docs/ROM_REVISIONS.md`](docs/ROM_REVISIONS.md) §6 states the gates. In order,
cheapest first:

1. **Bound the sub-entry indices.** Section indices are bounds-checked; indices
   *within* a section are not. This build asks for 1.1 indices, and 1.0's
   `GAME_TEXT` has 259 entries where 1.1 has 343 — so a 1.1-only string index on
   a 1.0 ROM is an unguarded out-of-bounds read with no diagnostic. That it
   passes the current fixtures is not evidence; it is precisely this project's
   silent failure class. Adding a bounds check to the sub-entry accessors is
   cheaper than auditing every index and turns a silent read into a loud abort.
   **Deliberately deferred:** the gap is not reachable today. Unsupported
   revisions are classified from their header CRC pair and refused by name
   before engine boot (`platform/rom_id.c:265-302,332-342`), the refusal
   message cites this exact gap as its reason, and no override path exists for
   that verdict — `MDKR_ROM_ALLOW_MODIFIED` only covers a damaged dump of an
   already-supported revision. Adding the check is genuinely cheap and is the
   correct first step whenever the supported set expands; today it is defense
   in depth behind a gate that already fail-closes, not a live hazard.
2. **Build the Japanese release as a separate binary.** It needs `REGION_JP`:
   font handling, game text and EEPROM layout all change, and this build compiles
   `REGION_NA`. A second build directory with a per-version asset-offset default
   sidesteps 423 compile-time gates; runtime dispatch across them buys little for
   much more work.
3. **Oracle routes and audio verification per revision.** There is no pixel
   parity route for any unsupported revision, and audio is unverified for all of
   them.

PAL timing is modelled as a 50 Hz source clock with the same authored two-field
ticket, so its visual cadence remains 25 Hz. Experimental host-pacing policies
are independent of that grid but do not add visual frames in 1.0.1+. Region-
specific calibrated gameplay oracles remain a separate expansion question for
unsupported revisions.

## Project infrastructure

- **Hosted CI is green; branch protection is not yet on.** This entry used to
  say the first hosted run was still owed. It is not: run `31248954626` of the
  `correctness` workflow completed **success** on `main` at
  `080c4c4e6480eef86afe1f964766d27d2e618fda`, 2026-08-08, with all six jobs
  green — policy and source contracts, Linux ROM-free ASan + UBSan, native Linux
  OpenGL-only, native Linux WebGPU, native macOS WebGPU warnings, and linked
  wasm plus browser save custody. Claims in this repository no longer rest on
  local runs alone.

  What is still owed is the consequence: `main` does not yet *require* those
  jobs, so nothing stops a direct push that has not passed them.
  `tools/check_github_branch_protection.py` exists to assert the configuration
  and has not been run green against the live repository. Enabling protection is
  a repository-settings change rather than a source change, which is why it sits
  here rather than in a commit.
- **Mode-coverage stragglers.** Ghost save and load are no longer one of them.
  `check_ghost_matrix.py` drives 46 of the 47 legal (track, vehicle) pairs
  through a record, a save, and a read-back in a **fresh process**, each pair in
  its own save directory; the 47th, Spaceport Alpha in the car, is an asserted
  autopilot non-producer rather than a skip — its racing line dead-ends at
  `courseCheckpoint` 10, and DKR records a ghost only for a course time under
  10,800 frames — and the check fails if that pair ever starts finishing, which
  is how it would be promoted. So the residual is one pair whose round trip
  nothing drives, not 46. Why the breadth was worth buying is in
  [`docs/open-items/gameplay.md`](docs/open-items/gameplay.md#open-ghost-coverage-is-one-track-vehicle-pair-of-47):
  this exact path already shipped a stack overflow that aborted with nothing at
  all on stderr. What is still narrow is magic-code entry, and it is now partly
  covered — `nav_to_magic_codes` submits valid `ARNOLD` and invalid `ARNOLE`
  through the onscreen keyboard, and `check_taj_p2_adventure.py` enters retail
  `JOINTVENTURE` and races the two-player Adventure it unlocks — but only for
  those codes, not across the code set. **Deliberately deferred:** onscreen-keyboard
  entry is proven end to end for an accepted code, a rejected near-miss that
  differs by one character, and the retail code that unlocks two-player
  Adventure — covering the accept path, the reject path, and the
  highest-traffic unlock. The remaining codes share one decrypt, normalize,
  validate and apply path, and that path's actual failure mode was an
  endianness defect in the shared table decode (see
  [`docs/open-items/portability.md`](docs/open-items/portability.md#fixed-tagged-macos-artifact-exposed-a-stale-magic-code-endian-failure)),
  which any single code exercises. Sweeping the full set would multiply
  route time without reaching new code.
- **Collision-candidate headroom is a watch metric, not an action item.** Boss
  levels 41 and 54 peak at 416 of 500 candidates. The cap can no longer be
  stepped over, and the 84-slot margin is unchanged; it is recorded so that a
  future content or collision change that eats into it is noticed. See
  [`docs/open-items/collision.md`](docs/open-items/collision.md).

---

## How to pick something up

Read the relevant [`docs/open-items/`](docs/open-items/README.md) file first —
including the closed entries, which exist precisely to warn you about the trap
you are about to walk into — then
[`CONTRIBUTING.md`](CONTRIBUTING.md) for what a change has to ship with. The
short version: fix the instance, then sweep the class with a mechanical
instrument, and if the instrument does not exist, building it is part of the fix.
