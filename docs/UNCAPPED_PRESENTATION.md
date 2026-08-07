# Original gameplay with modern presentation

Status: shipped in 1.0.4 (2026-08-04); the Preview label was removed once
authored UV scroll gained retained endpoints. The qualification measurements
below were taken 2026-08-02.

## Decision

Golden Balloon keeps Diddy Kong Racing's gameplay on the original fixed tick
and uncaps only presentation. `Gameplay.SimulationCadence=original` is the
fidelity setting. `Video.FrameLimit` controls how often the host may present;
`Video.MotionSmoothing=interpolate` controls whether opportunities between game
ticks contain a newly reconstructed image or hold the last authored one.

These are deliberately separate controls:

| Control | Owns | Does not own |
|---|---|---|
| Gameplay cadence: Original | Physics, AI, animation decisions, timers, RNG, input consumption, audio service, saves | Monitor refresh and intermediate images |
| Frame limit: Display/numeric/Uncapped | Host presentation opportunities and event pumping | Game tickets or game speed |
| Motion smoothing: Interpolated | Camera/object/vertex/effect presentation between adjacent authored tasks | Prediction, collision, AI, input, audio, or persistence |
| Pure/Restored/Remastered | Rendering style and fidelity | Simulation or presentation cadence |

At 60 Hz on an NTSC game tick, the visible sequence is task T at alpha 0, a
reconstruction at alpha 1/2, then task T+1 at alpha 0. At 120 Hz, the three
intermediate alphas are 1/4, 1/2, and 3/4. Numeric rates use an exact rational
deadline grid, so 90, 144, 165, 240, PAL 60, and variable display schedules do not
need to divide the game tick evenly. Native Uncapped removes the software
ceiling while interpolation can produce new images; it does not promise that
the display or GPU can consume unlimited unique images. Rates above your
display's refresh need a display connection that can drop an image it has not
shown yet. Where the system does not offer one, they present at your display's
refresh instead, unless Allow Tearing is on. With smoothing Off, held authored
images are serviced at display cadence because an unbounded no-swap loop cannot
add motion and can starve audio. A browser maps Uncapped to its display/rAF
ceiling.

## Why Enhanced cadence is not the uncap

Enhanced is the old one-field compatibility path. It invokes gameplay code
twice as often with half-width updates. That sounds like “60 FPS,” but DKR's
physics and AI are not invariant to that repartitioning.

The Bluey 2 reference lane makes the distinction measurable:

| Result | Original | US 1.1 reference | Enhanced |
|---|---:|---:|---:|
| Bluey finish clock | 3,458 | 3,459 | 3,022 |
| Mean-speed ratio to reference | 1.000646 | 1.000000 | 1.139822 |
| Checkpoint/lap agreement | 97.266881% | reference | 8.671922% |

Enhanced finishes 437 60 Hz fields, or 7.28 seconds, early and runs about 14%
faster over the common comparison window. It reproduces the reported boss
behavior and is a useful sensitivity control, but it is not preservationist
gameplay. The full reference methodology, audio result, and HUD-timestamp
explanation are in [BLUEY2_PARITY.md](BLUEY2_PARITY.md).

The rule: a modern presentation option may create more images; it must never
create more game updates.

## Immutable replay boundary

The earlier interpolation experiment was unsafe because a display-list pointer
did not own its data. By the time a midpoint replay ran, the game could already
be rewriting the arena for the next task. A camera-only override could therefore
combine task-T commands with task-T+1 matrices, vertices, textures, viewports,
or child lists.

The production path now publishes one atomic retained-task transaction:

- the complete graphics arena is copied into a private double-buffered image
  and tagged with the exact authored tick; the default admission ceiling is the
  known 16 MiB arena size, so a normal Interpolated session remains enabled;
- the top-level display list and arena-backed segment bases are rebased into
  that image;
- matrices, vertices, triangles, viewports, texture/TLUT spans, and remastered
  smooth-normal streams observed outside the arena during the real HLE walk are
  copied as explicit dependencies;
- cache, owner, and registry lookup continue to use the original address as
  identity while byte reads use the retained address;
- publication is transactional: an allocation, span, or dependency failure
  leaves the last complete task intact and the presentation opportunity holds;
- unload, restart, renderer recovery, and relaunch invalidate the task token,
  so a task from one arena generation cannot be acquired by another.

For forward motion, DKR has already authored task T+1 in its alternate buffer
before the presentation interval drains. A read-only structural census follows
only display-list flow, segment/billboard state, owner matrices, and vertex
batches. It invokes no backend calls and no game callbacks. That publishes the
true adjacent deformation/effect pair while task T's frozen owner bindings stay
unchanged.

Compatibility checks are intentionally conservative. Object generations,
viewports, model/animation/topology streams, particle identities, and shared
effect lifetimes must agree. A mismatch holds the exact task-T value. The
renderer never extrapolates and never guesses across a cut, respawn, recycled
address, topology change, or missing dependency.

Camera snapshots latch the complete pose, projection recipe, and canonical
view-projection at the display-list projection emission—not later from mutable
camera globals. True midpoints are rebuilt from interpolated camera inputs;
alpha zero and one copy the exact matrices authored by their tasks. Camera
substitution is limited to matrix keys observed by the completed real HLE walk,
so unused build-time allocations cannot be mistaken for retained endpoints.

## Load shedding and latency

Interpolation is optional visual work. WebGPU polls completions without waiting
on the cooperative game/audio thread. It admits at most two submitted frames
and permits only one in-flight frame when starting a replay, reserving the
second slot for the next authored endpoint. If the GPU is behind, the attempted
midpoint is skipped and the last complete image remains visible. Simulation,
audio, and input continue.

This is the right failure mode for both a 240 Hz display and native Uncapped:
achieved visual FPS becomes a capability result, while gameplay speed remains a
contract. The display connection is part of that capability: a rate above the
display's refresh only reaches the screen where the system offers a way to drop
an image it has not shown yet. Where it does not, and Allow Tearing is off, the
picture arrives at the display's refresh.
Interpolation uses adjacent authored states rather than prediction,
so it does not invent speculative input response. Input is still consumed on
the next original game ticket; more presentation opportunities only pump and
queue host edges sooner.

The correctness-first retained arena currently costs two 16 MiB private buffers
and one 16 MiB copy per authored graphics task while interpolation is armed.
Original or smoothing-off play does not arm this retention. The general arena
does not expose a safe high-water mark or a closed pointer graph, so a compact
copy is not presumed correct. `[RETAINED-TASK]` reports copied bytes, the copy
budget, peak arena payload, current/peak resident payload, and budget
rejections. A constrained diagnostic run may set
`MDKR_RETAINED_ARENA_COPY_BUDGET_BYTES` between 1,024 bytes and 16 MiB; a
rejected capture holds the last complete endpoint rather than publishing partial
memory. Anything else — `0`, a smaller number, a larger one, or a non-decimal
string — falls back to the 16 MiB default, because a sub-1,024-byte budget
admits no capture at all and would report zero capture failures while producing
no retained tasks. That is an allocation/load-shedding safety valve, not the
normal presentation policy.
Any future optimization must preserve the complete-ownership and poison gates
before replacing the full copy with a narrower lifetime scheme.

On the local Apple-silicon RelWithDebInfo qualification at 320×240 GL, 600
fixed ticks / 1,200 presentation opportunities measured a 0.291 ms mean retained
publication, 0.215 ms mean HLE replay before presentation, and 4.633 ms mean
complete tick wall. The run copied 9.375 GiB of arena data, completed 597
midpoints, resolved 165,259 private-arena and 6,349 copied-external addresses,
and reported no capture, acquire, future-pair, or replay failure. These figures
are a local cost census, not a universal hardware guarantee; the regression gate
requires publication and replay individually to stay below a complete 60 Hz
presentation interval.

## Evidence contract

The implementation is accepted only if all four layers agree:

1. **Authority:** v3 simulation state, ordered gameplay events, consumed input,
   temporary PCM, audio sample time, and saves are byte-identical across
   presentation rates and smoothing controls.
2. **Endpoint identity:** alpha-zero replay reproduces the exact authored
   semantic vertex stream and backend pixels.
3. **Midpoint sensitivity:** production midpoints differ from both adjacent
   endpoints, while object, deformation, particle geometry, vertex color,
   primitive alpha, shield, and authored-UV-scroll controls each cause an
   independent pixel change.
4. **Lifetime safety:** replay succeeds while the complete live arena is
   poisoned, across unload/restart/reissue boundaries, with zero stale fallback,
   key collision, capture failure, or dependency failure.

The principal gates are:

- `tests/check_presentation_matrix.py`: fixed authority, GL/WebGPU agreement,
  endpoint semantics/pixels, live-arena poison, unique midpoints, and independent
  model/particle/fade/effect/UV-scroll controls;
- `tests/check_arbitrary_presentation_rates.py`: NTSC 30/60/90/120/144/165/240,
  native Uncapped stand-in, PAL 60 and PAL Match Display, and nonblocking
  WebGPU overload behavior;
- `tests/check_presentation_breadth.py`: boss, challenge, vehicle, 1P/4P, and
  NTSC/PAL content breadth;
- `tests/check_presentation_lifecycle.py`: pause-to-unload, race restart, and
  Adventure post-race/lobby/hub retirement boundaries;
- `tests/check_camera_snapshot_coverage.py`: split-screen camera 1, TT spectator
  camera 3, and cinematic bank-4 motion plus byte-exact current/next endpoint
  chains and the unwalked-matrix boundary;
- `tests/check_browser_presentation_rates.py`: real Chromium fixed and irregular
  rAF schedules, authority parity, replay ownership, and surface accounting;
- `tests/test_gfx_retained_task.c`, `tests/test_presentation_packet.c`, and
  `tests/test_video_config.c`: transaction, forward-packet, and public-config
  units.

The local qualification record for this change is summarized in the 1.0.4
changelog entry. Raw ROM-derived frames, traces, PCM, and saves remain local and
are not committed.

## Authored UV scroll

No DKR scroller uses `G_SETTILESIZE`, `gSPTexture` or a texture matrix. Every
one of them rewrites the S10.5 corner UVs inside a `Triangle` array in place,
once per authored tick, and `gSPPolygon` (`G_TRIN`) carries only a pointer to
that array. The drivers are `obj_loop_texscroll` (the level-wide
waterfall/river/lava scroller, matched by texture index), `obj_loop_animator`
(one segment and batch), the generated wave surfaces in `waves.c`, and the
rain, particle and skydome sheets. They differ only in which triangles they
touch and in the modulus they fold back at.

That is why the retained endpoints do not need to know which driver owns a
batch. The retained task already holds tick T's bytes for the array, because
`G_TRIN` copies them as a task dependency; the already-authored next task names
the same array now holding T+1. The difference of the two is the tick's
displacement, whatever produced it, and replay adds `alpha` times that
displacement to the authored corner coordinates. Nothing is written back: the
retained image stays immutable and the live array is only read, so the replay
ownership boundary is untouched and alpha zero is byte-exact.

**Wrapping.** Each driver periodically folds a corner back into range by adding
or subtracting a whole number of texture repeats: `texscroll` uses `width<<8`
and `height<<8` (eight repeats), the animator `width<<7` (four), waves
`width*32` (one), rain `(width<<5)*2`, the skydome `width<<9`, and the particle
sheet a flat 512. Every one of those is a multiple of 32 — one texel — because
they are all whole repeats of a texture that is a whole number of texels wide.
Interpolating the raw difference across a fold would sweep the surface through
its entire texture inside one tick, so the shortest wrap distance is recovered
structurally rather than numerically: every triangle in a batch takes the same
displacement, a batch is at most sixteen triangles, and the un-folded ones state
the displacement directly. Anything larger must then be that displacement offset
by an exact multiple of one texel. A batch that cannot be explained that way
registers nothing and holds.

**Identity.** Endpoints are keyed by the triangle array's original address, and
a batch also has to reproduce its own topology — the four index and flag bytes
of every triangle — between the two ticks. `waves.c` is the one driver that
double-buffers its geometry: it builds into `gWaveTriangles[flip + (k << 1)]`
and flips `flip` every tick, so one surface sits at two alternating addresses.
That correspondence is declared by the game through
`gfx_dkr_note_paired_triangle_buffers` rather than guessed by the renderer, and
both phases of a pair key off the same even-phase address.

**Refusals.** A displacement reaches the screen only when the previous published
tick agreed on the same one for the same key. Authored scroll speed is a level
constant, so a real scroller confirms on its second tick and stays confirmed;
anything else — a batch appearing for the first time, a batch whose bytes are
not wholly retained, a fold that leaves no un-folded triangle to read the
displacement from — holds its authored phase for that tick, which is exactly
the behaviour that shipped in 1.0.4. Refusals are counted as `uvscrollhold` in
`[PRESENT-PACKET]`.

**Cost.** A confirmed site is a 32-byte record: two displacements, a triangle
count and two per-triangle selectors. Only batches that actually moved are
recorded, so ordinary static geometry costs nothing, and the table is capped at
4096 sites. The measured peak over the Jungle Falls and Hot Top Volcano routes
is 11 live sites, 352 bytes a tick. It lives in the presentation packet, not in
the retained task, and `[RETAINED-TASK]` confirms that directly: over the same
3227-tick Jungle Falls capture the arena bytes, arena peak, resident bytes,
resident peak, external dependency count and external bytes are identical
before and after this change, and the only counter that moves is
`externalResolve`, which counts read-only lookups. The added cost against
`MDKR_RETAINED_ARENA_COPY_BUDGET_BYTES` is zero.

**The gate.** `tests/check_presentation_matrix.py`'s
`MDKR_TEST_UV_SCROLL_INTERPOLATION=off` control is the midpoint-sensitivity
arm: on Jungle Falls the interpolated midpoints must differ between the two
arms and the authored endpoints must stay byte-identical, while state, event
and input streams agree. It is non-vacuous by measurement, not just by
argument — run against the binary built immediately before this change, the
same two runs produce zero differing midpoints out of thirty, because the
scrolling phase held at its endpoint value.

## The 50 Hz source on a 60 Hz display

PAL content is the case where Original's own definition works against the
player. The authored tick is two 50 Hz fields, so one image is meant to last
40 ms. A 60 Hz display can only offer 33.3 ms or 50 ms, and the absolute 40 ms
grid plus a blocking present therefore alternates between them in a repeating
3/2/3/2/2-refresh beat. The mean is exactly 25 authored images a second, so
gameplay speed, timers and music pitch are all correct — but no two consecutive
images are shown for the same length of time, and that is what is visible.
NTSC has no equivalent beat: its 33.3 ms tick is exactly two 60 Hz refreshes.

Match Display plus Interpolated is the answer, and it needs nothing new: the
deadline grid is already an exact rational, the accumulator already carries
50 Hz fields in integer units, and `tests/check_arbitrary_presentation_rates.py`
proves both a 60 Hz cap and Match Display against the 25 Hz cadence with
byte-identical authority streams. The engine reports the combination once at
startup so a European player is not left to find the setting by accident.

The beat also reaches audio. The synthesis service is due once per authored
tick, so on PAL its refills inherit the same 33.3/50 ms alternation while each
refill still carries one 40.6 ms block. A latency target of exactly one block —
which is correct while refills and blocks are the same length, as they are on
NTSC — leaves nothing to cover the long half, and the sink runs dry. The queue
controller therefore targets the worst observed refill gap rather than the
block, so an even host is unaffected and a coarse or beating one gets a
proportional cushion.

## Player-facing recommendation

- Keep **Gameplay cadence: Original** for accurate play.
- For acceptance testing of modern motion, use **Match Display** plus
  **Interpolated** in the directly visible Frame Rate & Motion section.
- Use a numeric cap when consistent thermals or power use matter.
- Use native **Uncapped** for measurement or when the renderer/display stack has
  headroom; it still sheds visual work rather than changing gameplay.
- Rates above your display's refresh need a display connection that can drop an
  image it has not shown yet. Where the system does not offer one, they present
  at your display's refresh instead, unless **Allow Tearing** is on.
- Use **Motion smoothing: Off** for authored motion or comparison captures.
- Treat **Enhanced** as an explicit gameplay-changing compatibility option, not
  a quality or FPS upgrade.
