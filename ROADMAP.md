# Roadmap

What is **not** done at v1.0.0, why it was deferred, and what would have to be
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
independent reference: a full Ancient Lake lap, and an all-racer Bubbler lane
that isolates the reported boss-speed divergence. The Bubbler lane settled the
simulation-cadence question — retail and authored finish at ticks 3,459/3,458
with a 1.00047× speed ratio, against 3,022 and 1.13965× for the historical
one-field arm — and `Gameplay.SimulationCadence=original` is the default because
of it. Ancient Lake now also has a fail-closed diagnostic that replays the
real ROM's observed update widths and input states. It makes checkpoint clocks
0–3 exact and moves the first five-unit position separation from clock 18 to
767, classifying the early mismatch as timestep partitioning rather than a
different game-speed policy.

**What remains.** Everything the two lanes do not cover: challenge breadth,
multiplayer, progression and save, audio, renderer state, and the
fixed-update/interpolated-render architecture. Ancient Lake still develops
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

The Adventure loop is closed end to end — hub, balloons, lobby, three-lap race,
trophies, the legal first boss, both Adventure One and Two — but the campaign is
not finished in the sense a player means it. Silver-coin challenges, the world
1–4 boss rematches, both Wizpig races, the credits sequence, and the remainder of
the legal door/key graph are not driven by any gate. This is the largest single
piece of deferred work in the project, and the one most likely to matter to
someone playing rather than reading.

The exit condition is a single deterministic start→credits gate, not a
collection of partial routes.

## Renderer

### WGPU-11 — external and oracle corpus breadth

**Where it stands.** The local closeout is done and is not small: 46 native
menu, course, hub and multiplayer routes execute 249,339,186 strict commands
with zero faults across 29 material IDs and 37 material/pipeline keys; the
offline gate validates all 445 supported-ROM models, 1,146 level segments and
16,943 batches including dormant variants; forced 4 KiB segmentation and a
one-entry shader-index limit prove safe continuation and live GL fallback; and
the 105-point fault registry is fully classified.

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

Camera plus generation-keyed core-object and billboard interpolation exists and is
proven headless/offscreen on native builds. Root transforms, racer heads and
vehicle-part child matrices are rebuilt from immutable snapshot data. A bounded
retained packet owns billboard-local matrices and world-space anchors, covering
sprite objects and sprite particles while refusing rewritten transient keys;
it also retains adjacent animated-model vertex batches and blends XYZ plus
shade RGBA only when generation, model, animation, topology, root stream and
tick adjacency agree.
Point/line particle meshes use retained world-space endpoints under the same
generation/topology/adjacency rules, with byte-identical multi-viewport repeats
collapsed safely. Particle opacity respects its unsigned 8.8 representation;
sprite/model/line-particle primitive alpha is scaled from the authoritative
object fade while point trails retain their alpha exactly once in vertex RGBA.
Shield/magnet shear matrices retain a semantic recipe keyed
by both the racer and shared effect-object generations, then rebuild continuous
local rotation, scale and shear around the interpolated racer root.
Recycled pool addresses, spawns, teleports, transitions, ambiguous keys and
missing history hold the tick pose. Pixel controls independently prove root,
model-deformation, point-trail geometry, retained RGBA, primitive-alpha fade,
and shield-effect paths change backend output with identical authoritative
state.
The smoothed path is explicitly one authoritative tick late: both native
backends retain the real walk off-surface, replay the previous endpoint at
alpha zero, and then advance monotonically toward the current endpoint. This
prevents the former current/midpoint/current ordering from reversing motion.
`Video.FrameLimit=60`
was evaluated for promotion to the native
default in v0.8.0 and **refused**, with the argument recorded rather than
implied. The clock and product blockers in that historical decision have since
changed: exact native numeric caps through 1000, display policy, true no-sleep
uncapped policy, and PAL 60 now run off the source-field grid; tick-indexed input
and independently clocked audio service have also landed. Browser display-rate
parity now shares the fixed clock: one opportunity per rAF, numeric throttles,
irregular timestamp proof, and explicit uncapped-to-display fallback. The
Native WebGPU completion accounting and GL interval-0 fences now cap CPU-ahead
GPU work at two frames; minimized windows elide GPU walks and resume through a
fresh scheduler history. A dedicated lifecycle gate now also proves exact
state/event/input/PCM identity through 2P pause-to-menu teardown, pause-menu
race restart, and the full Adventure post-race/lobby/hub return while retained
display-list history is freed and reissued. A shared ROM-free sink controller
now passes simulated 30–1000 Hz schedules and SDL's exact queue/drain/pause/
clear contract; a five-second physical CoreAudio/silence witness also passes.
The default still stays `original` for the first opt-in release because the
remaining nested presentation classes and full physical platform/DAC matrix
remain. The former 5–22%
camera-mixing population was subsequently eliminated by stale-tenancy and
geometric-tolerance fixes; the current 17-arm breadth gate reports zero hard
matrix rejects and keeps the historical worst/best levels as stress controls.

What would have to land before revisiting the default: remaining nested classes
and remaining render-visible scalar coverage; audible/loopback audio and DAC-drift
qualification across the physical platform matrix; and the rest of that
matrix. Core 3D
roots/composed children, compatible model/particle deformation and shade RGBA,
object/particle opacity, shield/magnet
shear recipes, the fixed-ticket authority clock, bounded tick-input
queues, catch-up render elision, independently due audio service, and ordered
state/event/input/PCM proofs are now landed prerequisites, not remaining
blockers.

### Shadow and visual leftovers

- A dead `SunShadowRes` control and a misleading GL message (small, worth doing).
- A placement-sensitive shadow gate that has produced false results twice
  (medium, worth doing).
- One-frame caster lag and a decal/map inconsistency — both **accepted**, no
  action planned.
- Broader materials and water, and scene-wide atmosphere and VFX, are polish
  that must not gate anything.

## Platforms

### Windows

**Experimental.** The code is portable and the platform layer has no macOS-only
assumptions left in it, but there is no Windows CI cell, no Win32 surface bridge
exercised by any gate, and no measured run on Windows hardware. Anyone building
there today is the first person to do so. Treat a Windows build as a
contribution opportunity, not a supported configuration.

### Linux

Best-effort. GL and WebGPU both build and the CI matrix includes ROM-free Linux
cells, but physical-GPU runs, Wayland (as opposed to X11), and controller
hotplug/rumble breadth are all unmeasured. The Wayland/Win32 surface-bridge work
is tracked as F-03; see
[`docs/open-items/web.md`](docs/open-items/web.md) and
[`docs/open-items/renderer.md`](docs/open-items/renderer.md).

### Native app shell — shipped

**No longer deferred.** The native ImGui app shell (`platform/app/`) ships: a
first-run launcher with ROM discovery and per-revision CRC validation, a
settings panel generated from the video/gameplay schema with honest LIVE vs
RESTART presentation, an in-game F1 overlay, F10 FPS readout, and a diagnostics
log view. It builds on macOS, Windows (MinGW cross) and Linux, and the macOS
`.app` now launches it directly — the bash/AppleScript first-run picker shim
that stood in for it is retired.

Two honest limitations remain, both recorded in
[`docs/APP_SHELL.md`](docs/APP_SHELL.md):

- The overlay does **not** pause the simulation. It swallows input so the kart
  coasts, and the footer says the race is still running. A real pause needs a
  seam in game code that does not exist yet.
- Return-to-launcher re-execs the process and is therefore POSIX-only; on
  Windows the overlay offers "Quit to Desktop" instead of mislabelling a
  silent quit.

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
CRCs and **refused by name** — not silently, and not by a boolean that could be
flipped.

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
2. **Build the Japanese release as a separate binary.** It needs `REGION_JP`:
   font handling, game text and EEPROM layout all change, and this build compiles
   `REGION_NA`. A second build directory with a per-version asset-offset default
   sidesteps 423 compile-time gates; runtime dispatch across them buys little for
   much more work.
3. **Oracle routes and audio verification per revision.** There is no pixel
   parity route for any unsupported revision, and audio is unverified for all of
   them.

PAL timing is modelled as a 50 Hz source clock with the same authored two-field
ticket, so original cadence is 25 Hz and arbitrary presentation rates remain
independent of that grid. Region-specific calibrated gameplay oracles remain a
separate expansion question for unsupported revisions.

## Project infrastructure

- **Hosted CI has never run green.** The workflow matrix is written and the
  checks are real, but the first hosted run is still owed, and repository branch
  protection depends on it. Until then, every claim in this repository rests on
  local runs.
- **Mode-coverage stragglers.** Ghost save and load are gated for one
  (track, vehicle) pair rather than across the set, and there is no gate that
  actually enters a magic code and observes its effect.
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
