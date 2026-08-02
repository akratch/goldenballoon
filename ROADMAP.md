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

Production motion smoothing and retained display-list replay are disabled for
1.0.1+. Release-candidate testing proved that a replay delayed until after task
`K+1` begins can observe mutable viewport, matrix, vertex, texture, and nested
display-list storage after the game has started rewriting it. Partial packet
freezing and generation keys do not establish immutable ownership of that full
dependency graph.

Original therefore remains the recommended/default Frame Limit. Match Display,
numeric, and Uncapped settings remain Experimental host-pacing/input-pump
policies: they do not add visual frames, never swap a duplicate image, and leave
the authored cadence at approximately 30 Hz NTSC or 25 Hz PAL. Fixed-ticket
simulation, tick-indexed input, independent audio service, bounded GPU queues,
suspension rebasing, and state/event/input/PCM invariance are still useful
landed prerequisites.

Revisiting higher-rate visuals requires immutable ownership of every dependency
for a real forward `{T,T+1}` pair, including nested display lists and texture
state; exact endpoint and midpoint pixel oracles across UI, cutscenes,
split-screen, particles, and every vehicle part; device-loss/lifecycle coverage;
and broad physical-platform/DAC qualification. The former interpolation work
and its broken-direction controls remain historical diagnostic infrastructure,
not an active release feature.

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

The portable x64 build is supported since 1.0.1. Hosted CI checks the product,
stock-Windows imports, package shape, ROM-free startup surfaces, and extracted
archive; the release candidate also passed manual WebGPU gameplay, controller,
audio, save, and relaunch acceptance on Windows hardware.

The post-1.0.2 compatibility backlog is explicit: add user-selectable
auto/Direct3D-12/Vulkan adapter policy and textual GPU diagnostics; preserve
adapter/device request failure messages; centralize UTF-8-to-UTF-16 filesystem
and process operations; add Unicode-path tests and a reviewed application
manifest; then broaden physical coverage across Intel, AMD, NVIDIA, hybrid-GPU,
Windows on Arm, and negative VM/RDP configurations. Until the wide-path work
lands, keep the extracted app and ROM in short, ASCII-only paths.

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
ticket, so its visual cadence remains 25 Hz. Experimental host-pacing policies
are independent of that grid but do not add visual frames in 1.0.1+. Region-
specific calibrated gameplay oracles remain a separate expansion question for
unsupported revisions.

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
