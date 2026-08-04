# Changelog

Release history for Golden Balloon. The detailed test inventory is in
[tests/README.md](tests/README.md); deferred work and known gaps are tracked in
[docs/open-items/](docs/open-items/README.md).

From 1.0.0 onward this project follows semantic versioning for the platform
layer's public seams (config keys, environment variables, command-line flags and
save formats). Everything below 1.0.0 predates that commitment.

## [1.0.4] — 2026-08-04

No ROM or other game data is included.

### Recommended settings

- **Renderer:** WebGPU
- **Presentation:** Restored
- **Frame limit:** Original
- **Motion smoothing:** Off
- **Gameplay cadence:** Original

This is the qualified path and the default for a fresh configuration. It keeps
the original game speed and art direction while adding widescreen output,
supersampling, mipmaps, and modern filtering. Existing preferences are
preserved when upgrading.

For smoother experimental motion, use **Match Display** with **Motion
smoothing: Interpolated**. Interpolation creates presentation-only images
between game ticks; it does not run physics, AI, timers, input, audio, or saves
more often. **Enhanced** cadence is not an FPS option: it changes gameplay and
runs the measured Bluey 2 route about 14% faster. Remastered remains a visual
preview, and OpenGL remains a diagnostic backend.

### Added

- Added Taj as an unlockable playable racer. Enter `ABRACADABRA` or complete
  all three Taj challenges to unlock him. He has a normal character-select
  slot, portrait, voice, horn, HUD and results identity, custom handling, and a
  scaled animated magic carpet across all three vehicle classes. His unlock
  persists through relaunches and imported saves, works in multiplayer, and
  keeps modded Time Trial records and ghosts separate from the original roster.
- Added safe presentation-only interpolation for cameras, racers, supported
  objects and deformations, particles, fades, and projected shadows. The new
  **Motion smoothing** control chooses between authored-frame holds and
  interpolated in-between images.
- Added persistent master, music, and sound-effects controls to native
  Settings, synchronized with the original in-game Audio Options menu.
- Added Windows borderless fullscreen with **F11** and **Alt+Enter**, complete
  controller remapping, and Off/Light/Balanced/Strong rumble choices.

### Changed

- Reorganized native Settings around the choices players use most. Restored is
  clearly marked as the recommended presentation, frame-rate and motion
  controls are visible together, and the gameplay-changing cadence setting is
  kept separate. The launcher remains usable at compact sizes and high UI
  scales. Larger controls and direct touch scrolling improve use on handheld
  PCs and touchscreens.
- Kept Original as the default gameplay cadence. Enhanced is now labelled as a
  compatibility mode that changes game speed.
- Made WebGPU presentation nonblocking. If optional interpolated work cannot be
  admitted, the renderer holds a complete image instead of delaying gameplay
  or audio.
- Moved complete-ROM validation to a cancellable launcher worker so selecting a
  ROM no longer freezes the interface.
- Made app preference writes transactional and durable. Windows paths now use
  UTF-16 and extended-length handling for ROMs, saves, configuration,
  diagnostics, captures, and relaunches. The Windows manifest enables both
  long paths and the UTF-8 active code page.
- Pinned and hash-verified both Linux AppImage build inputs.

### Fixed

#### Gameplay and audio

- Restored speed- and throttle-responsive engine audio for cars, hovercraft,
  and planes without consuming the gameplay RNG stream.
- Fixed final-lap music on native builds: the live sequencer now applies the
  faster tempo instead of changing only the reported BPM.
- Fixed a Windows startup timing fault that could leave audio behind the video
  until character select. Held-frame presentation at higher refresh rates is
  now bounded so it cannot busy-spin and starve audio service.
- Bounded the native PCM backlog, checked sink failures, and crossfaded recovery
  after a rejected or dropped buffer. Volume changes are ramped to avoid clicks,
  active loops respond immediately, and persistence failures remain visible
  with a retry path.
- Made F1 a real pause boundary. Simulation and the race clock stop, race
  effects are suppressed, and the quieter authored music mix continues until
  play resumes.

#### Presentation

- Fixed projected kart and character shadows flickering or snapping between
  interpolated frames.
- Fixed widescreen framed-world views. Track-select previews and framed results
  footage stay inside their wooden apertures; the surrounding decorative art
  reaches the display edges without exposing off-canvas carousel cards or
  controls.
- Restored full Hor+ presentation where there is no physical frame: opening
  logos, animated credits, Track Select setup, and initial post-race footage.
- Treated transitions between contained and full-width camera policies as cuts,
  preventing a one-frame interpolation blend between incompatible projections.
- Removed the large character-picker shadow behind Taj and corrected the scale
  and animation of his magic carpet.

#### Launcher, input, and recovery

- Added **Restart & Apply** for settings that require a relaunch. A staging or
  engine-start failure now returns to a usable launcher with diagnostics instead
  of closing the app.
- Routed Escape through the overlay's back and quit-confirmation flow instead
  of immediately exiting during play.
- Fixed whole-interface flicker while dragging the native UI-scale slider by
  applying the new scale after the interaction ends.
- Cleared controller buttons and axes when the native window loses focus, fixed
  compact high-scale overlay layout, and added retry when a verified replacement
  ROM is playable but its preference cannot be saved.

#### Compatibility and browser

- Restored German in the European 1.1 language selector. US 1.1 remains
  English/French, matching that revision's menu.
- Improved browser ROM and save replacement, storage-failure recovery, focus,
  skip navigation, and heading structure. A retained ROM can be persisted with
  **Retry browser storage**, and blocked fullscreen requests now show a visible
  result.

### Known limitations

- Interpolated motion remains a preview. UV-scrolled level surfaces such as
  waterfalls, water, and lava still advance on authored game ticks and may
  shimmer or step during camera motion. Motion smoothing Off is unaffected.
- WebGPU with Restored presentation is the qualified visual path. OpenGL and
  Remastered have not been promoted for 1.0.4.
- Linux still uses drag and drop or a typed ROM path instead of a native file
  picker. Native screen-reader semantics are also outside this release.
- The macOS artifact is ad-hoc integrity sealed but not Developer ID signed or
  notarized, so first launch may require **Open Anyway** in Privacy & Security.

## [1.0.3] — 2026-08-02

### Fixed

- Completed the native texture-cache identity with source pitch/span, palette,
  format, dimensions, row layout, SDF derivation, mip, and cutout policy; cache
  slot reuse now invalidates sampler memos before replacing the uploaded image.
- Cleared WebGPU redundant-bind trackers before cached bind-group and pipeline
  release, preventing opaque handle-address reuse from skipping a required bind.
- Kept Remastered grading and tonemapping on world pixels at every render scale:
  output HUD/text now enter the terminal overlay at explicit 1× and at a
  hardware budget clamp to 1×, matching the already-correct supersampled path.
- Unified the texture-edge shader and mip-coverage alpha boundary across GL,
  WebGPU, Metal, and the CPU mip reducer.

### Changed

- Made Restored unambiguously the default in native config, CLI help, launcher,
  browser, README, and tests. Remastered is opt-in and work in progress. The
  inert future TexturePack key remains parseable but is no longer advertised.

### Testing

- Made the cached-model material ownership rule fail closed in native builds:
  the legacy door mutator is no longer part of the native API or binary, the
  per-door resolver is const-correct, and a ROM-free CTest guards draw-local
  door/racer selection plus OpenGL sampler texture identity. The existing real
  Adventure oracle remains the final GL/WebGPU gameplay and pixel gate.
- Made the odd-row texture corruption oracle hermetic and backend-complete; added
  exact cache-key units, WebGPU cache-ownership guards, and 1×/2× opaque-HUD
  finish A/B coverage.

## [1.0.2] — 2026-08-02

### Fixed

- **Dino Domain door numerals no longer change with the camera.** The four race
  doors carry independent 1/2/3/5 requirements but reuse one cached model.
  Golden Balloon 1.0.1 selected each digit during a view-dependent fixed-tick
  traversal by mutating that shared model, so the last visible door could put
  its numeral on every sign. Display-list construction now resolves the atlas
  offset directly from the door being submitted without changing shared model
  state. The same audit found and fixed an older OpenGL sampler-cache omission
  that could repeat the selected numeral across the door face. A real Adventure
  route now proves the authored requirements, per-door material bindings, and
  final GL/WebGPU door pixels. Blank frames, a common glyph on every sign, and
  repeated sampler output are generated as fail-red controls during the gate.
- **Windows save migration and tooling preserve exact bytes.** Exclusive legacy
  preference/save migration and save-utility staging now open binary files with
  native Windows semantics, avoiding CRT newline translation. Portable
  environment, temporary-directory, and shell fixtures let the hosted Windows
  lane compile and execute the complete ROM-free release suite.

### Documentation

- Clarified that Windows WebGPU automatically selects Vulkan or Direct3D 12,
  documented the 1.0.1 ASCII-path workaround, and recorded explicit backend
  selection, wide-character filesystem APIs, and a reviewed application
  manifest for a future portability release.
- Added an exact-hash 1.0.1 human acceptance guide and consolidated the stale
  pre-character-select animation-rate script into the stronger settled-screen
  motion/rate gate, with explicit frozen and five-frame-loop controls.

## [1.0.1] — 2026-08-01

### Changed

- **1.0.1 keeps visual presentation at the authored rate.** Original remains
  the recommended/default Frame Limit. Match Display, numeric, and Uncapped are
  **Experimental — Under Construction** host-pacing/input/event-pump policies
  only; they do not increase unique visual FPS and never swap a duplicate image.
  Their benefit may be negligible, while higher settings can use more CPU.
  Production motion smoothing and delayed
  display-list replay are disabled because a walk after task `K+1` begins can
  observe rewritten viewport, matrix, vertex, texture, and nested display-list
  dependencies. The primary US 1.1 build therefore remains at its authored
  roughly 30 unique visual FPS under every exposed policy. The detailed
  interpolation entries later in this 1.0.1 section are retained as a
  historical development record, not as active 1.0.1 behavior.

- **WebGPU remains the native default and the browser renderer.** A
  controlled M3 Max run of the earlier experimental replay build at 1920×1080,
  `RenderScale=1`, 60 interpolated presents per 30 Hz authored tick schedule,
  and 300 synthetic headless ticks completed in 1.15 s on GL versus 9.01 s on
  WebGPU. GL sustained 698.9 submissions/s with a 2.617 ms mean tick wall;
  WebGPU sustained 103.1 submissions/s with a 17.377 ms mean tick wall. A
  five-second sample attributed the WebGPU gap to native presentation rather
  than game code: the main thread spent about 41% of samples retiring
  wgpu-native queue work and 27% waiting for `CAMetalLayer` drawables, while
  snapshot/freeze/interpolation work remained in the low-microsecond range.
  Those interpolated-present measurements describe the historical experiment,
  not the authored-frame-only 1.0.1 release path.
  Raising the experimental WebGPU queue ceiling from two to three did not
  improve throughput. Production 1.0.1 subsequently removed gameplay-time
  queue drains entirely because it submits only authored tick images; the
  browser/internal replay diagnostic retains the two-frame ceiling. Throughput
  alone was not sufficient to make GL the default: dense capture of the opening sequence
  exposed large localized sky/terrain texture corruption which sparse
  whole-frame difference sampling had missed. The same corruption reproduces at the
  `v1.0.0` source checkpoint, ruling out the fixed-tick/FPS work as its cause.
  With no selector, the app now chooses WebGPU; `MDKR_RENDERER=gl` remains an
  explicit diagnostic path while GL parity work continues. A new targeted intro
  guard requires the two no-selector frames which exposed the corruption to
  match explicit WebGPU byte for byte.

- **The native launcher is less monolithic and its UX now fails safe.** Panel
  routing, dropped-ROM intake, navigation, boot actions, and overlay
  confirmation flows are named, typed helpers instead of one large draw path.
  ROM replacement is transactional; invalid or unsaved candidates preserve the
  active ROM. Text, enum, checkbox, and deferred slider edits retain attempted
  values after persistence failure, expose a visible error, and can commit
  through **Retry** after write access returns in the same process. Escape and
  controller B share the popup → confirmation → Settings → overlay back stack.
  The launcher supports a 640×480 compact top-navigation layout, persists a
  0.75×–2.00× UI scale without cumulative style drift, and rebuilds its font
  atlas across standard/HiDPI transitions before frame construction. Keyboard
  and gamepad navigation, visible focus, scalable contrast, and restrained
  motion are in scope; a native VoiceOver/UI-Automation semantic tree is not.

- **Portable release identity and Windows packaging are fail closed.** Linux
  and Windows validation compile the validated workflow version and require the
  executable to report it before packaging. Linux additionally content-validates
  built and extracted AppHost frames through software WebGPU and GL before
  upload. The Windows archive prepared for manual native GPU/gameplay acceptance
  exposes one `GoldenBalloon.exe`; a shared import-table gate rejects SDL and
  MinGW runtime DLL dependencies, and the package contains no DLLs.

### Fixed

- **Windows WebGPU no longer stalls gameplay and audio behind synchronous GPU
  drains.** A post-1.0.0 queue bound waited for the entire native queue after
  every second submission, before the completed tick could refill audio. The
  production path now nonblocking-polls completion callbacks and submits only
  newly authored images; explicit blocking remains outside runtime gameplay.
  This restores 1.0.0 startup audio and character-select timing. The portable
  candidate passed manual WebGPU gameplay, controller, audio, save, and relaunch
  acceptance on Windows hardware.

- **Returning from the in-game overlay to the macOS launcher no longer depends
  on the process's original working directory.** The shell resolves its absolute
  executable path before entering the app bundle's Resources directory and uses
  that path for the overlay re-exec.

- **Numeric and Uncapped frame limits no longer crash the adopted macOS GL
  path.** The launcher creates the GL context with the system declarations, so
  the engine must initialize GLAD after adopting that context. FIFO presentation
  happened not to call the missing sync entry points; interval-zero pacing did
  and jumped through a null `glFinish`/fence pointer. Adopted contexts now require
  a successful `SDL_GL_MakeCurrent` and GLAD load before gameplay begins. A new
  real-launcher gate completes 240 and Uncapped policy handoffs on both default
  WebGPU and explicit GL, requiring every GPU submission to drain.

### Added

- **Magic-code tables now validate completely and fail closed.** The reported
  `"Sorry, the code was incorrect"` response plus `8`/blank enabled rows was
  reproduced on a pre-fix binary: the decrypted table's big-endian count and
  offsets were read as native values (`29 -> 7424`, `187 -> 47872`,
  `220 -> 56320`). Current source and the `v1.0.0` tag already contain the
  original decrypt-then-normalize repair, so the reported DMG was not built
  reproducibly from that tag. The normalizer now validates every string offset
  and terminator transactionally, aborts loading instead of exposing a partial
  table, and has a ROM-free unit test that pins the exact reported misreads,
  `ARNOLD -> BIG CHARACTERS`, invalid offsets, and rollback. The macOS release
  lane's commit/version/checksum provenance prevents another stale
  local binary from being presented as a tagged artifact.

- **Fail-closed macOS bundle integrity with optional Developer ID signing.** A
  report from an M4 Mac mini on macOS 26.2 distinguished Finder's “damaged”
  rejection from the
  expected unidentified-developer warning. The bundle builder was rewriting
  SDL2's Mach-O load path after the linker's ad-hoc signature, then packaging
  the invalid code envelope. Homebrew's current `sdl2` also resolves to
  `sdl2-compat`, a shim which loads an unbundled SDL3 dynamically and therefore
  escaped ordinary `otool` dependency checks; the reported crash log shows both
  the bundled shim and `/opt/homebrew` SDL3 in one process. Release packaging
  now builds SHA-pinned upstream SDL2 2.32.10 for arm64/macOS 13 and rejects
  sdl2-compat, SDL3, Homebrew load paths, undeclared architectures/targets, and
  unresolved nested dependencies. The local path now clears inherited xattrs,
  ad-hoc signs nested SDL2 before sealing the app, and requires strict nested
  signature/resource/load-path verification. The protected manual release
  workflow imports a password-protected Developer ID certificate into an
  ephemeral keychain, uses an App Store Connect API key for `notarytool`, signs
  nested code explicitly before the app with Hardened Runtime, requires an
  Accepted notarization plus staple/Gatekeeper validation, then signs,
  notarizes, staples, mounts, and re-verifies the DMG. Speculative library-
  validation and unsigned-executable-memory entitlements are removed. This
  patch's public macOS artifact is intentionally unsigned: macOS may show the
  ordinary unidentified-developer warning, but bundle verification must pass
  and Finder must not report that the app is damaged. The credentialed signing
  and notarization lane remains ready for a later release.

- **A ROM-free native audio-sink qualification seam.** The game's
  queue-occupancy controller is now a shared pure module with deterministic
  30/60/120/144/240/1000 Hz, counter-wrap, alignment, capacity and stall
  contracts. A second CTest opens SDL queue mode with silence, requires exact
  22050 Hz stereo-s16 output, observes real drain, and verifies bounded
  backlog plus pause/clear behavior under SDL's dummy driver. The same binary
  passed a five-second physical CoreAudio run with 476 controller calls, 214
  observed drains, zero queue failures/stall guards, and a 1,193-frame queue
  high-water. A 1,000-tick before/after headless capture remained byte-exact
  (`bf2c44b9...97b7f1`). Hidden hardware-buffer underruns, speaker output and
  DAC drift still require the cross-platform physical release matrix.

- **Historical replay lifecycle qualification covered destructive edges.** The
  experimental path compared Original and higher-rate host schedules through a
  2P pause-to-Track-Select teardown, race restart, and Adventure return while
  checking state, events, input, PCM, packet lifetime, and arena retirement.
  This remains useful diagnostic infrastructure, but retained walks are not
  enabled in the production 1.0.1 renderer.

- **Exact arbitrary-rate host pacing, independent of the authored simulation.**
  `Video.FrameLimit` accepts `original`, `display`, bounded integer policies
  from 30 through 1000, and `uncapped`; the app exposes curated choices while
  retaining Original as the recommended, proven default. Numeric policies use
  absolute rational monotonic deadlines. Sub-field host time feeds the fixed-
  ticket driver and independent audio clock, and native/browser schedule gates
  require byte-identical state, ordered events, consumed input, and PCM.
  Production 1.0.1 frame admission submits only a newly authored task, so these
  policies change host/input-pump opportunities without adding visual frames or
  duplicate swaps. Native WebGPU polls completion without a runtime drain, GL
  retains its fence bound, minimized windows stop GPU walks, and resume rebases
  rather than accumulating catch-up.

- **Historical experiment — generation-keyed core-object interpolation.** The
  following describes the retained-replay prototype and its diagnostic seams;
  it is not active in the production 1.0.1 renderer. Presentation replay binds
  registered 3D root matrices to the exact `(pool address, spawn generation)`
  that built them and reconstructs roots, racer heads and vehicle-part child
  matrices from the immutable previous/current snapshot pair. Render-only
  tumble/bob/scale residuals are carried without reading or writing a live
  object. Recycled addresses, spawns, teleports and missing history fail closed
  to the tick pose. The presentation matrix gate requires nonzero object
  rebuilds, reconciles every owned/unowned registration, and disables object
  interpolation while retaining the same interpolated camera to prove the path
  changes backend pixels. A bounded retained packet now also owns billboard-
  local matrices and world-space anchor vertices through the last present of a
  tick; sprite objects and sprite particles therefore interpolate position,
  scale and camera-facing roll without trusting rewritten arena addresses.
  Parent-relative attachment anchors follow their already-interpolated parent
  exactly once. A tick-stamped previous/current packet now also retains model
  vertex batches and blends their XYZ deformation plus shade RGBA across the
  exact same object generation, model, animation, topology and root stream.
  Spawn,
  teleport, transition, skipped-tick and duplicate-key cases hold the authored
  pose. The rate gate proves 315,974 changed batches over its route and a
  deformation-only control changes 20/20 sampled intermediate race frames
  while camera/root/billboard smoothing stays enabled. Direct world-space
  point/line particle meshes now use the same exact retained endpoints under a
  generation/kind/topology/tick-adjacency contract; line topology changes hold,
  repeated multi-viewport submissions collapse only when byte-identical, and
  conflicting keys fail closed. Particle opacity capture now decodes the
  unsigned 8.8 value stored in the signed field instead of clamping its upper
  half to zero. The battle witness blends 21,994 changed point-trail XYZ/RGBA
  batches; independent geometry and color controls each change 50/50
  intermediate backend frames without changing the authoritative hash.
  Sprite/model/line-particle primitive alpha preserves draw-local modifiers
  while replacing current object opacity with the interpolated value; it
  applies 54,859 changed draws and its isolated control changes 33/50 frames.
  Point trails remain single-scaled because their opacity is already retained
  in vertex alpha. Shield and magnet shear
  matrices now retain a semantic two-lifetime recipe (racer generation plus
  shared effect-object generation) and reconstruct both the interpolated racer
  root and continuous local rotation/scale/shear without lerping matrix cells.
  Ambiguous effect keys fail closed; the forced-shield control applies 5,676
  overrides and changes 30/30 intermediate backend frames with byte-identical
  authoritative state. Smoothed presentation now also exposes the retained
  previous-tick endpoint before its intermediate frames. The old order exposed
  the new authoritative image first and then drew a previous-to-current
  midpoint, visibly reversing motion every other present. Final qualification
  showed that delayed replay after task `K+1` begins can still read rewritten
  viewport, matrix, vertex, texture, and nested display-list dependencies. The
  release path therefore disables motion smoothing and retained replay
  completely; a future implementation requires immutable ownership of every
  dependency through an exact forward `{T,T+1}` pair.

## [1.0.0] — 2026-07-30

### Release-night additions (folded at tag time)

The following landed between the 1.0.0 cut and the tag, and ships in 1.0.0.

### Added

- **Native app shell (`platform/app/`).** Launching the app with no arguments
  now opens a real launcher instead of failing to find a ROM. You hand it a ROM
  by dragging the file onto the window, by the **Choose ROM File...** button
  (your system's own file picker), or by pasting a path; the choice is validated
  against the same five-revision table the CLI and the browser shell use
  (`platform/rom_id.c`), so a European or Japanese cart is refused **by name**
  rather than accepted and crashed on. A validated ROM is remembered.
- **Nothing on your disk is searched, ever.** The launcher does not scan folders
  and calls no directory-listing API, so it can never raise a macOS permission
  prompt for Documents, Downloads or Desktop. The identified revision is shown
  prominently before Play, with a **Change ROM...** button once one is loaded.
- **Settings panel generated from the schema.** Every key in
  `mdkr_video_schema` renders itself, grouped by a new `category` field, with its
  own documentation string. LIVE-scope keys apply immediately; RESTART-scope keys
  are labelled, saved, and shown as "running now X → Y on the next launch"
  instead of pretending to take effect. Keys pinned by an environment variable or
  the command line are disabled and say which layer owns them.
- **In-game overlay (F1) and FPS readout (F10).** The overlay renders through
  whichever backend is active (WebGPU or GL) and swallows input while open, on
  both the open and close edge, so the toggle key never leaks to the game. It
  does **not** pause the simulation and does not claim to — see Known limitations.
- **Diagnostics view.** `stdout`/`stderr` are teed into an in-app console and a
  rotating log file, with a one-click copy for bug reports.
- **A headless shell gate.** `ctest -R app_` covers the argv triage, the ROM
  validator, a schema round-trip, and a render smoke that fails when its captured
  frame is missing — so it cannot pass without producing pixels.

### Changed

- **The macOS `.app` launches the shell directly.** The bash/AppleScript
  first-run picker shim is retired, along with the second copy of the ROM
  revision table it carried. One binary, one revision table. Any invocation
  *with* arguments still runs the unchanged engine path, so every existing script
  behaves exactly as before.
- **Branded GUI surfaces use the product name "Golden Balloon".** `mdkr64`
  remains the internal name (repository, build target, technical filenames,
  environment prefix), while application and game-window titles, the macOS
  bundle display name, and the DMG volume/filename use the public brand.
- Windows builds now link `libstdc++` statically. Without it the shell's C++ would
  have added a `libstdc++-6.dll` import, which the import-table allowlist
  correctly rejects.

### Fixed

- **Windows crash diagnostics could be lost.** The log tee redirects `stderr`
  through a pipe drained by a reader thread that dies with the process, so a
  crash-time backtrace could be swallowed or block on a full pipe — exactly when
  it matters. The crash handler now writes to the pre-tee console and log
  descriptors directly, bypassing the pipe.
- **Metal-backed WebGPU windows reported half resolution under the shell.** The
  drawable-size query keyed off a Metal view the engine owns, which is null when
  the shell owns the window; it now tests the window's own flag, so the renderer
  is no longer fed a half-resolution drawable on Retina displays.

### Known limitations

- The overlay does not pause the simulation. It holds the controls so the kart
  coasts, and says the race is still running. A real pause needs a game-code seam
  that does not exist yet.
- "Return to Launcher" re-execs the process and is POSIX-only; Windows offers
  "Quit to Desktop" instead of mislabelling a silent quit.
- The browser build is unaffected: the shell is native-only and the web build
  keeps its existing JS shell.

**The public launch.** The first release published as an open repository.

The engine is the one 0.8.0 shipped — this release adds no gameplay, renderer or
audio behaviour. What it adds is everything a project needs before it can be
handed to people who did not write it: provenance that has been classified file
by file, a documentation tree organised around what a reader is trying to do
rather than the order in which it was written, an honest statement of what is
*not* done, and repository standards that are enforced rather than described.

### Added

- **[`ROADMAP.md`](ROADMAP.md)** — the deferred work, each item with the reason
  it was deferred and the condition under which it would be taken up: WGPU-11
  external and oracle corpus breadth, IQ-8 WebGPU MSAA, the IQ-11 texture-pack
  loader and the ROM-absence-guard question it must answer first, F-18
  independent state reference breadth, ROM revision expansion, Windows and Linux
  platform maturity, signing and notarization, and campaign completeness.
- **[`RELEASE_NOTES.md`](RELEASE_NOTES.md)** — player-facing notes for this
  release: what the port is, what it needs from you, which platforms are
  supported and which are experimental, and the known limitations.
- **[`docs/open-items/`](docs/open-items/README.md)** — the 422 KB defect record
  split by subsystem into ten files plus an index. Every entry is preserved
  verbatim, closed ones included: a closed entry is the only warning the next
  person gets that the same trap exists.
- **Repository standards that are enforced:** `.editorconfig`, `.clang-format`,
  pre-commit and pre-push hooks, issue and pull-request templates, a shell-syntax
  check, `tools/check_markdown_links.py`, which validates every local Markdown
  link and heading anchor, and a public-surface guard for tracked paths, text,
  commit identities, commit messages, and annotated tags.

### Changed

- **The documentation tree is organised for a reader, not a historian.**
  `STATUS.md`, `HANDOFF.md` and `PLAN.md` move under `docs/` as `STATUS.md`,
  `DEVELOPER_HANDBOOK.md` and `ARCHITECTURE_DECISIONS.md`; `docs/README.md` is
  rewritten as a map keyed on what someone is trying to do; and every public
  document reads as the documentation of a maintained project rather than a log
  of how it was produced.
- **The source repository is itself the public boundary.** It keeps ordinary
  Git history; temporary plans, handoffs, transcripts, and personal paths stay
  outside tracked files, while durable decisions are written into maintained
  architecture, status, or open-item documents.

### Removed

- **Eleven SGI-legend-bearing files that nothing referenced.** Unreferenced
  `PR/` headers, `libultra` `gu` sources and the scheduler, deleted rather than
  shipped: 2,041 lines that the build never used and whose provenance would have
  had to be explained forever.

## [0.8.0] — 2026-07-30

The fidelity wave: fixed-authority simulation with decoupled interpolated
presentation (proven non-authoritative across all content), process
determinism on all 66 levels under a widened v2 state-hash oracle, the
two-camera mixing fix, and the S-tier web shell polish.

### Added

- **Presentation-rate invariance is now proven across content, not one route
  (spec §12.3).** `tests/check_presentation_breadth.py` runs the per-tick
  `[SIMHASH]` comparison with the presentation subloop engaged over a
  §12.3-shaped content set — every challenge and battle type, three boss
  courses, car/hovercraft/plane races, four-player split screen, and PAL — and
  records the four presentation-side quantities that can degrade without moving
  an authoritative bit: the replay's matrix-recomposition reject ratio, registry
  freeze/restore failures, snapshot capture overflows, and interpolation
  continuity. Behind it, a one-time sweep of levels 0–65 found **63 of 63
  eligible levels byte-identical**, with zero overflows and zero registry
  failures; three levels were excluded for an unrelated defect hunt. So: **all
  66 levels are process deterministic, and the presentation-invariance sweep
  covered 63 of 63 eligible ones.** Four-player split screen
  substitutes 2.49 view-projections per interpolated present, which is the first
  machine-checked evidence for the design doc's multi-viewport claim (R2) —
  every gate run before this was one player.
- **A present-path cost census, `MDKR_PRESENT_PERF=1`.** Six `[PRESENTPERF]`
  sections time the snapshot publish walk, the matrix registry freeze, the
  interpolated view-projection build, the replay walk, and each present.
  Measured at 1080p on an M3 Max: 60 Hz presentation sustains at 59.8
  presents/second with the authoritative tick held at 29.9, using **23.4% of
  the tick budget — a 4.3× margin**. The machinery Phase 3 added costs 9.3 µs
  per tick, 0.2% of the added cost; the other 99.8% is the display-list re-walk
  and the second present, both of which inherently have to happen. No
  optimisation was applied because the measurement does not support one. A
  companion `[PRESENTREJECT]` census buckets rejected recompositions by
  magnitude, which is what showed that 99.96% of them are sub-millimetre float
  re-association rather than a wrong decomposition.

### Fixed

- **Shadow casters no longer inherit a dead matrix's world (`Mtx` addresses are
  not stable identities).** The host-side shadow matrix registry keys entries by
  `Mtx` pointer but copies the world/view-projection by value, and the game
  legitimately rebuilds a *different* matrix in the same arena memory later in
  the same frame. Three slot-2 pushers in `camera.c` — `render_sprite_billboard`
  (×2) and `render_ortho_triangle_image` — do that without registering, so the
  registry kept answering with the previous tenant's world. Because that world
  is also the shadow-caster input, and because the vehicle-part branch
  deliberately does not enable billboard mode, **car wheels, propellers and fans
  were casting shadows from track geometry up to 3,107 world units away** — on
  the real walk, with no interpolation involved. Fixed from both sides: the
  registered `Mtx` image is now compared against the live bytes at lookup and a
  mismatch is a miss rather than a lie, and the vehicle-part push registers its
  own tenancy so that geometry keeps a *correct* shadow instead of merely losing
  a wrong one. The two genuinely screen-facing pushers upload a matrix with no
  view-projection composed into it, so no `(world, view_projection)` pair
  describes them and they are deliberately left for the guard to refuse.
  Presentation-side, the same defect was the entire hard-reject population: 14 →
  **0** on level 40, with `mtxrejectleast` going from 10,453,373 LSBs to −1
  (nothing left to reject) and the worst mismatch falling from 203,622,112 LSBs
  to 63. `[SIMHASH]` v2 is byte-identical to `2d697f6` on levels 5, 17, 20, 21
  and 40.
- **The recomposition rejects were previously mis-diagnosed, and the record is
  corrected.** They had been attributed to `mtx_head_push` composing its list
  matrix against `gCurrentMVPMatrixF` while registering a world from the model
  stack. That fix was implemented and measured to change nothing — 4,271 head
  pushes on level 40's route, **0** disagreements, byte-identical reject output —
  because the only caller invokes `mtx_head_push` on the statement immediately
  after `mtx_cam_push`, so the seam cannot open. It was reverted rather than
  left in the hottest matrix path. Registrations now carry a site tag, which
  attributes 13 of level 40's 14 rejects to `mtx_cam_push` and only 1 to the head
  push — the attribution-by-elimination that produced the wrong theory is no
  longer possible.

### Changed

- **`check_presentation_breadth.py`'s tolerance bounds were strengthened.** Hard
  rejects are now asserted `== 0` rather than `<= 64`, because the population
  they bounded is extinct by design rather than merely small. The old positive
  control (*rejects must be non-zero, or the tolerance swallowed the genuine
  population*) cannot be stated on an arm that rejects nothing, so it moves to a
  new `tenancy-control` arm that disables the guard with `MDKR_SHADOW_TENANCY=0`
  on the same binary and requires the population to come **back**, still
  rejected and still enormous — measured 8 rejects, least 9,543,536 LSBs (146
  world units), 149,118× above the worst tolerated. The arm additionally asserts
  the guard actually fires, so a tenancy check that silently became a no-op fails
  there instead of passing everything. `MIN_GENUINE_REJECT_LSB` and
  `MAX_TOLERATED_LSB` are unchanged; nothing was loosened.

### Changed

- **`Video.FrameLimit=60` was evaluated for promotion to the native default and
  refused.** The default stays `original`. Breadth and performance both came
  back clean, and neither is sufficient: four of the presentation-rate matrix's
  seven schedules (120/144/VRR/uncapped) still cannot run at all; `60` does not
  divide PAL's 50 Hz field clock, so a `60` default would silently mean
  `original` on every European release; core-object interpolation — the second
  half of the spec's own Phase 3 checklist item — does not exist yet, and the
  design doc admits camera-only only on condition it is not shipped as the final
  behaviour; the live/interactive and input-edge-queue slices that are sequenced
  before an interactive rate rise have not landed. The 5–22% camera-mixing
  population measured during development was subsequently eliminated by the
  stale-tenancy and tolerance fixes above. One mechanical trap remains: flipping
  the default string alone would be *inert*, because the config runtime only
  pushes the key when it did not come from the default.

- **Phase 3 fidelity architecture: camera-only interpolated presentation
  (headless/offscreen, off by default).** A presentation subloop can now
  present faster than the simulation ticks, re-walking a tick's own display
  list through the renderer's HLE layer with the gameplay camera's
  view-projection interpolated from a snapshot pair — never by calling
  `render_scene`/`mode_game_render` again, which would double-advance
  render-tree-embedded state. `Video.FrameLimit` (`original`\|`60`) and
  `Video.MotionSmoothing` (`off`\|`interpolate`) are new, precedence-aware
  config keys mapped onto the `MDKR_PRESENT_RATE`/`MDKR_PRESENT_SMOOTHING`
  seams; both require a restart (the present pacer and the replay's frame-state
  capture each resolve their value once, at the first present) and neither is
  touched by the Pure/Restored/Remastered presets. Every object still holds its
  authoritative pose
  through an interpolated frame — camera-only is deliberately the narrower,
  first slice, proven headless/offscreen on native builds only (not yet
  live/interactive, not yet browser, not yet object interpolation). Three
  real defects were root-caused building this, not tuned around: a display
  list read after the game had already begun overwriting it, a matrix
  registry silently emptied by its own restore path, and two clock-phase
  bugs (a synthetic clock advancing per present instead of per tick, and
  the tick counter bumping before the interpolated presents that still owed
  it input).

### Fixed

- **Racers that were translucent stopped being simulated as "on screen".**
  The render-purity migration's visibility tick reproduced one of
  `render_level_geometry_and_objects`' three object loops — the opaque pass —
  so a time-trial ghost, or any racer mid-fade after a hit, lost the "was
  drawn" timer that gates its AI steering, its balloon-upgrade dice and its
  particle emitters. It now reproduces the union of the two loops a racer can
  reach. The HUD tick also moved to the head of the tick block, because it
  writes the toggle that the object sort's camera basis and the visibility
  frusta read, and it now restores the active camera like every other tick
  does; and the presentation accumulators moved ahead of the weather tick,
  restoring the order the renderer rolled the skydome and the weather dice in.
- **A quit to the title screen could replay a freed display list.** With the
  presentation subloop armed, the interpolated present re-walks the last
  display list through a bare pointer that only a level *load* retired — and
  quit-to-title never loads a level. The retirement moved to the three
  teardown seams that can free what the pointer refers to.
- **A present with nothing new to draw swapped an undefined back buffer**
  instead of holding the frame it was supposed to repeat (visible as flicker at
  `FrameLimit=60` with `MotionSmoothing=off`, on the GL path). Such presents no
  longer swap; pacing is unchanged, so `FrameLimit=60` with smoothing off
  honestly updates the screen at tick rate rather than pretending otherwise.
- **Correction to `49b1840`.** That commit attributed a Taj challenge failure
  to asymmetric guards around the carBob exact-bits restore. The guards are in
  fact identical and unnested, so no such path exists: the failure was entirely
  the loop-course assertion the same commit also fixed. The pairing guard is
  kept as defence (and extended to the tumble pair, whose brackets are wider),
  and the Taj gate gained the progress floor that max-displacement alone does
  not provide. See the census's "Corrections to the census itself".

## [0.7.2] — 2026-07-29

### Changed

- **The web launcher now defaults to Restored.** Remastered remains one click
  away (labeled work-in-progress); players with a stored choice keep it.

## [0.7.1] — 2026-07-29

### Fixed

- **Viewport can no longer strand short on iPhone.** Sizing prefers the
  visual viewport, every lifecycle edge (launch, rotation, fullscreen, tab
  return, stage reveal) arms a bounded settle watchdog, and in-flight touch
  zones re-measure on re-layout — the standalone "blue bar until rotated"
  launch state heals itself within a frame of the viewport settling.
- **iOS audio latency.** The ringer-switch workaround uses the sanctioned
  Audio Session API where available instead of a persistently looping media
  element (whose media-pipeline buffers were the "huge delay"), the worklet
  ring is capped at 0.4 s as a hard latency ceiling, and suspensions can no
  longer build an audible backlog.
- **Final-review corrections:** portrait Pause taps no longer hit the
  throttle pad beneath; the Look zone no longer steals the top of Drift;
  the save-tools module rides the cache-busting stamp; accessibility
  activations fire the full chord.

## [0.7] — 2026-07-29

### Fixed

- **Stuck throttle on iPhone.** Releasing Go could leave acceleration
  latched: release delivery depended on pointer capture that Safari
  declines, so a thumb that slid off its starting zone lifted into the
  canvas unseen. All touch controls (pad, stick, pause) now track moves and
  releases at the window level in the capture phase; the registered gate
  gains an off-pad-lift regression arm.
- **No audio on iOS Safari.** Two Apple-specific behaviors: an AudioContext
  created at the engine's 22050 Hz renders silence on iOS (the worklet now
  creates at the hardware rate on Apple touch devices and resamples), and
  Web Audio obeys the ring/silent switch until a silent media element flips
  the session to playback during the first touch (unmute technique).
- **Pause chip relocated** from top-center (covering the banana counter) to
  a mostly-transparent icon at bottom-center — view-safe and one thumb-hop
  from either hand.

## [0.6] — 2026-07-29

### Added

- **Two-thumb slide-to-chord touch controls.** The right cluster is now a
  throttle pad: touching any zone holds accelerate, and sliding the same
  thumb onto Drift, Item, or Look chords the modifier without ever lifting
  off the throttle — drifting and firing no longer cost acceleration. Brake
  sits on a deliberate diagonal and is the only off-throttle zone. Zone
  retention across gaps means a mid-corner slide can never stall the kart;
  a haptic tick marks each zone change; second-finger taps and Pause still
  work as before. The registered gate now proves the slide contract
  end-to-end into the game's own input read.
- **Visible build identity and real cache busting.** The launcher footer
  shows the exact build (`build <commit> · <time>`), and the publisher
  stamps every asset URL with the source commit so a stale Safari cache can
  never mix shell versions — the stamp propagates from the page through the
  shell to the wasm engine.
- **iPhone install path.** A web-app manifest (fullscreen, landscape) plus
  Apple metas make Share → Add to Home Screen launch the game chromeless;
  the launcher explains this exactly where iPhone Safari's missing
  Fullscreen API would otherwise leave a gap.

### Fixed

- **iPhone rendering no longer squeezed behind Safari's toolbar.** The game
  canvas sizes with dynamic viewport units, so the visible area is the real
  visible area.

## [0.5.1] — 2026-07-29

### Fixed

- **Fullscreen button reachable on phones.** The touch overlay's action
  cluster covered the fullscreen button's corner, so taps landed on Go;
  with the touch UI active the button now sits top-right above the overlay.
  On iPhone Safari, which has no element Fullscreen API, the button is
  hidden instead of failing; prefixed WebKit engines get a working fallback.

## [0.5] — 2026-07-29

### Added

- **Adaptive analog touch controls for mobile play.** Phone and tablet browsers
  automatically receive a safe-area-aware steering stick plus simultaneous
  Go, Brake, Drift, Item, Look, and Pause controls. The overlay supports
  multi-touch chords, yields to a connected gamepad unless explicitly retained,
  can be hidden at any time, and releases every held input on interruption.
  Portrait remains playable and gives a non-blocking landscape suggestion.
- **Two-player post-race coverage.** `check_race_2p_split.py` now drives the
  full 2P race through MENU_RESULTS and back to track select, closing the last
  uncovered multiplayer results transition (3P/4P were already gated).

### Fixed

- **Remastered shadow playthrough repair.** A v0.4 playthrough reported
  spurious shadows from invisible geometry and a generally heavy shadow look.
  Four root causes were repaired:
  - the cascade planner's caster depth range now covers the whole stage-static
    caster cache, so track geometry the game had CPU-culled no longer gets
    depth-clamped onto the light near plane as a full-strength phantom shadow
    (GL) or silently clipped (WebGPU) — the backends now agree;
  - worlds without an authored directional cue (weather drift or sky scroll)
    no longer invent a pseudo-random sun heading from a level-identity hash;
    they share one canonical key-light azimuth while keeping their sky-derived
    elevation;
  - billboard-sprite actors (bananas, balloons, most pickups) keep their
    authored projected blob: they are excluded from the caster feed by design,
    so suppressing their decal left them floating with no grounding at all;
  - the shadow umbra was lifted from 0.48 to 0.62 — DKR's baked vertex colour
    already carries authored occlusion, and the deeper factor double-darkened
    bright arcade art into heavy splotches.
- **Touch overlay preference persistence.** A stored explicit "shown" choice
  now revives the overlay after reload even when the touch media queries no
  longer match, and a live capability change (keyboard detached, pointer turns
  coarse) wires the overlay without a reload.
- **Touch shell robustness.** The capability listener could blank the whole
  launcher on engines without `MediaQueryList.addEventListener`; touch wiring
  is now feature-detected and non-fatal, mid-race browser zoom gestures stay
  suppressed while the overlay is hidden, and the Look button meets the 44 px
  touch-target floor.
- **Pre-release shadow deep review (wave "shadowdeep").** A four-lane review
  before the next release found and fixed the remaining world-shadow defects:
  - shipping builds never reset the shadow static caster cache at level load
    (the reset was reachable only through the diagnostic trace path every
    shadow gate happened to enable), so previous levels kept casting into the
    next one; a new registered gate holds shipping and traced builds to an
    identical caster census against a suppressed-reset control;
  - the runtime void curtain froze into the cache as a permanent phantom wall,
    wave-generated lava/water cast opaque shadows, and the ROM's authored
    no-shadow batch flag was ignored — a DL-build-time caster-exclusion seam
    closes all three;
  - shadow bias is now authored in world units and normalized per plan, the
    far shadow boundary fades instead of terminating on a hard line, the
    WebGPU shadow pipeline depth-clamps like GL, translucent actors keep
    their projected decal, cached static geometry no longer costs one draw
    call per triangle, and the caster bounds are seeded and
    plausibility-clamped with telemetry.

## [0.4] — 2026-07-28

### Added

- **Grounded Remastered world lighting.** Racers, characters, track structures,
  and suitable opaque objects now cast stable soft cascaded shadows on OpenGL,
  WebGPU, and the browser. Every 1P–4P camera owns a bounded map; projected
  shadows remain an immediate fallback if an optional resource is unavailable.
- **Restrained world-specific colour finish.** Playable worlds receive one
  explicit linear-light filmic shoulder plus bounded runtime-derived
  saturation, contrast, and tint. HUD, text, minimaps, menus, and transitions
  remain outside the treatment; Pure and Restored are unchanged.
- **In-game Video Options.** The localized Options menu now exposes
  Pure/Restored/Remastered presentation, 1×–4× supersampling, aspect ratio,
  authored-relative FOV, filtering, Remaster effects, and subtitles. Live
  controls apply immediately; cache-owned changes are saved with an explicit
  restart notice.
- **Output-resolution authored 2D.** The supersampled world now resolves before
  HUD, minimap, menu, and text composition, improving measured authored-2D edge
  quality without moving safe-4:3 layouts or changing the world beneath them.
- **Local save custody and recovery.** The launcher can download/import the
  exact EEPROM or a portable checksummed backup, edit reviewed progression
  fields, merge independently checksummed blocks, and restore three automatic
  recovery generations. All processing remains local and every replacement is
  staged, persisted, reloaded, and byte-verified before becoming live.
- **Virtual Controller Paks and ghost backup.** Four durable Paks preserve the
  original quota/error contract and Time Trial ghost path. The browser can
  transactionally export and import the complete Pak set as a versioned
  `.mdkr-paks` bundle.
- **Complete local-player and campaign envelope.** Two-, three-, and four-player
  layouts, Adventure Two, every authored challenge/battle course, the legal
  first-boss route, all three Taj vehicle challenges, and all four trophy
  championships now have production-driven completion and persistence evidence.
- **Truthful controller status and haptics.** Ports 1–4 report real presence,
  disconnects cancel active effects, and SDL/browser vibration is used when the
  connected controller exposes it.

### Fixed

- **Boss and campaign simulation use the authored regional cadence.** The
  historical one-field port made Bubbler finish 12.6% early. Gameplay now
  defaults to complete two-field updates at 30 Hz NTSC or 25 Hz PAL while
  rendering remains inseparable; the old one-field behavior is an explicit
  `enhanced` compatibility setting independent of visual presets. A
  clock-injected policy suite and all-racer US 1.1 oracle cover the fallback
  until fixed simulation and high-rate presentation are separated.
- **Fullscreen no longer freezes the browser renderer.** Fullscreen transitions
  are serialized, canvas resize has one transactional owner, and oversized
  supersampling is proportionally constrained to a render-pixel budget while
  the output surface remains at native resolution. A real Chromium gate now
  requires live frames through entry and exit.
- **Browser shadow activation is atomic.** Heavy receiver pipelines prewarm in
  the background while the ordinary world and projected shadows remain intact;
  invalid or failed optional resources cannot create missing geometry, a
  shadowless gap, or a fatal page. The WebGPU comparison shader now uses the
  explicit-level operation required inside nonuniform control flow.
- **Video preferences now persist transactionally and independently of campaign
  progress.** Native writes use atomic durable replacement; browser settings
  use the existing IDBFS durability queue and survive **Erase saved progress**.
  Startup overrides remain visibly locked and are never baked into the user's
  file. Invalid launcher values and storage failures leave the prior setting
  unchanged.
- **Adventure Two now preserves its real save identity and mirrored world.**
  Native/wasm save decoding no longer reverses the unlock bit, and negative N64
  viewport scale is translated into a valid mirrored modern viewport without
  mirroring the safe-4:3 HUD.
- **WebGPU recovery and capacity fail closed.** Surface/device loss can rebuild
  WebGPU once; a second failure stops cleanly without switching to the
  unqualified GL diagnostic backend. Browser failures retain save access and
  actionable UI. Per-frame vertex data grows through bounded
  segments instead of a fixed ceiling, and every shipped creation fault has a
  classified outcome.
- **Stage changes and final shutdown release their real owners.** Queued display
  lists retain one generation of pointer-registry grace, stale textures expire,
  repeated race loads plateau, and normal exit/reload leaves no renderer,
  AudioWorklet, ROM, arena, or delayed-free ownership behind.
- **Linux/GCC portability defects are repaired.** Native audio, shading, wave
  tails, scheduler messages, UI initialization, physical-pixel sizing, and
  several LP64 object boundaries now use their actual C owners. Ubuntu
  24.04/GCC builds without diagnostics and both Mesa Vulkan/WebGPU and OpenGL
  complete real render/present/shutdown runs.
- **Finished multiplayer racers no longer over-read lap-time HUD state.** The
  lap banner and race-time paths validate the current lap against the physical
  five-slot record, and header-driven HUD loops cannot exceed that owner.

## [0.3.2] — 2026-07-27

### Added

- **Sharper Remastered text without shipping derived assets.** Font glyph cells
  are registered at runtime and reconstructed as isolated 4× signed-distance
  fields, preserving the game's metrics, kerning, colour, and safe-4:3 layout.
  Pure and Restored retain the original atlas path exactly.
- **RDP-correct shade and fog gradients.** Shade and fog now interpolate in
  screen space while position and texture coordinates remain
  perspective-correct. Newly near-clipped vertices derive fog from their new
  clip-space position.
- **Measured moving-texture mipmaps.** Both renderers now expose complete
  mip-chain telemetry, and a moving Everfrost Peak fixture demonstrates roughly
  10% lower temporal shimmer.

### Fixed

- **Collecting a banana no longer draws a bright vertical streak.** Every sprite
  consumer now shares one overflow-checked, asset-bounded layout decoder. It
  reserves the exact command and vertex regions needed by the banana sparkle
  and lava-spurt sprites and unwinds failed construction transactionally.
- **The subtle dark/bright flash over the lower screen is gone.** It was
  near-clipped road and sand fog, not a vehicle shadow; the corrected clip and
  interpolation rules keep it stable on OpenGL and WebGPU.
- **Remastered text no longer bleeds between atlas cells or reuses stale
  reconstructions.** Each registered glyph is derived and sampled in isolation,
  and font-lifetime changes invalidate the derived cache.

## [0.3] — 2026-07-26

### Added

- **Three explicit presentation modes.** `--pure` provides pillarboxed,
  undistorted 4:3 at the authored FOV; `--restored` adds modern-fidelity
  filtering without changing the art direction; `--remastered` is the default
  home for later look-changing work. Settings resolve predictably from ini,
  preset, environment, and CLI layers, and `--video-list` reports their sources.
- **Real supersampling on OpenGL and WebGPU.** Restored and Remastered now render
  at 2× by default, with 1×–4× selectable in the browser. WebGPU uses a true
  N×N box resolve into an output-sized texture, keeping screenshots and fidelity
  readback at the window resolution.
- **CPU-built mip chains and configured anisotropic filtering.** NPOT textures
  no longer depend on driver mip generation, alpha-cutout coverage is preserved,
  2D draws stay pinned to level 0, and both shipped renderers consume the same
  generated chains.
- **Browser presentation controls.** The ROM-launch screen now exposes
  Pure/Restored/Remastered and supersampling choices, remembers them locally,
  and supports linkable `?mode=` and `?scale=` overrides.

### Fixed

- **Supersampled WebGPU frames no longer oscillate between output and render
  size.** Resize debouncing now compares committed output dimensions and derives
  the render target from them. GL/WebGPU same-frame difference returned from
  33.948 to 1.163 without loosening any comparison budget.
- **Supersampling auxiliary paths now use output dimensions consistently.**
  The F1 overlay targets the swapchain at its real size, backend debug dumps
  write exactly the output-sized buffer they allocated, and a failed resolve
  holds the prior complete frame instead of attempting an invalid scaled copy.
- **Display diagnostics now distinguish window and render resolution.**
  `[DISPLAY] output=… render=… scale=…` makes live resize and supersampling
  behavior unambiguous.

## [0.2] — 2026-07-26

### Fixed

- **Six reachable runtime boundaries are now deterministic and bounded.** Level
  teardown releases weather and signed-sentinel sky textures before its header;
  plane trick input, ordinary weapon targets, and zero-checkpoint recovery no
  longer consume indeterminate values or divide by zero. Audio now allocates all
  five live groups, rejects sound/group exclusive upper bounds, validates the
  complete 30-row vehicle-sound span, and maps flying-car and loop-de-loop
  explicitly to plane and car audio. ROM-free exhaustive domain tests and a
  mutation-controlled production census accompany the fixes; normal race PCM
  remains unchanged.
- **The native/browser allocator now enforces its boundary contracts.** Terminal
  frees no longer form sentinel pointers, invalid or overflowing sizes are
  rejected before alignment, foreign addresses cannot alias the main pool, and
  fixed-address allocation preflights both possible list splits transactionally.
  "Safe" allocation fails at the allocator boundary with a useful diagnostic.
  The delayed-free queue grows before entry 257 while preserving the full
  two-tick lifetime. A ROM-free property gate covers list/coalescing shapes,
  queue capacity, invalid pointers/sizes/timers, byte and slot exhaustion, and
  host-width alignment in Debug, Release, ASan, and UBSan.
- **The three-lap Adventure "non-finish" was a test identity error, and the
  missing win/loss progression coverage is now closed.** The old probe treated
  starting-grid slot zero as the human and actually followed an AI. It now uses
  the stable controller-port mapping and publishes finish position plus racer
  identity after natural finish assignment. The old checkpoint finished fifth,
  while the current runtime-boundary build naturally wins — proof that finishing
  place was not a stable oracle. Symmetric post-finish win/loss controls now
  require the same natural run and exact independently decoded visited/cleared
  and balloon outcomes across Debug, Release, ASan, and alignment builds.
- **Native object and renderer layouts no longer depend on N64 alignment or
  adjacent bytes.** One checked cursor now sizes and places every optional
  pointer-bearing object tail with host `sizeof`/alignment and overflow checks;
  variable object-map records are bounded and behavior-minimum-validated before
  typed access; and small HUD/menu/weather/particle/effect records pass their real
  transform/frame fields instead of being cast to larger objects. The same sweep
  fixed eight-byte map boost records reading a private extension byte from the
  next record.
- **World-space balloons and other billboards no longer stretch on wide
  viewports.** F3DDKR projects each sprite anchor, then adds its local vertices
  directly in clip space; the original 4:3 compensation therefore made the
  collectible-balloon motif 1.37x wider at 16:9 and 1.78x wider at 21:9. The
  native path now corrects the billboard matrix's X/Y output columns for the
  active viewport and effective lens, preserving rotated sprites, authored size,
  gameplay-FOV scaling, the 104° cap, and exact legacy stretching. The refinement
  gate now also covers 16:10 and forced 4:3 inside a 21:9 drawable, plus
  portrait/rotated-transform unit cases, and rejects any new direct
  post-projection billboard producer outside the audited world/ortho builders.
- **RAW16 instruments no longer decode as byte-reversed noise.** The three
  uncompressed synthesis loads now convert serialized big-endian signed PCM to
  host representation; the fourth, ADPCM load remains byte-exact. A same-binary
  `fixed`/exact-legacy gate inventories 25 music and one SFX RAW16 wave, requires
  an identical pre-boundary PCM prefix, and measures the old interpretation at
  27.34x the principal bass sample's roughness. Debug, Release, ASan, wasm
  compilation, the broad audio gate, and actual Chromium reachability pass.
- **Big-endian asset readers no longer assume a little-endian host.** Serialized
  16-/32-bit integer and floating values are built from their bytes, preserving
  unaligned safety and correct semantics on either host order. ROM-free unit
  cases cover misalignment, scalar conversion, raw reversal, PCM conversion,
  invalid arguments, and transactional failure behavior.
- **The browser no longer loses audio events under live-sink pacing.** The
  committed Chromium gate reproduced the 150-entry SFX queue reaching its limit.
  Measurement without the truncating drop reached 195 entries; the native-port
  budget is now 512, and the gate rejects any run that consumes over half of it.
  Drop diagnostics identify queue, event type, peak, capacity, and total count.
- **Browser persistence callbacks now describe the completed IDBFS write.**
  Callers that arrive during a sync wait through the final coalesced sync, and
  mount, ROM-write, and initial-load errors are no longer silently discarded.
- **Browser test controls and `?trace=` now reach `getenv()`.** Emscripten's
  `ENV` runtime object is explicitly exported; previously the shell wrote an
  absent property and the documented trace control silently did nothing.
- **Terrain-projected shadows no longer flicker as the camera moves.** Shadow
  geometry now reaches the renderer as a decal and therefore uses the existing
  depth-bias path. A 300-frame moving-camera A/B restores 324 dark pixels on GL
  and 276 on WebGPU, with identical gameplay state and zero brightened
  components.
- **The fixed-cap shadow mesh builder now fails transactionally.** Descriptor,
  triangle, and vertex capacity are checked before a mesh is committed; overflow
  rolls every cursor back, terminators and empty meshes are explicit, and an
  ASan fault-injection run safely drops 26,084 complete meshes.
- **Locked doors were intangible, so you could drive into worlds you had not
  earned.** `func_80017A18()` — the per-facet object-model collision test, and the
  only non-NULL writer of `collisionData->collidedObj` in the game — linked to a
  `return 0` stub, because `objects.c` guarded the body with `#ifdef
  NON_EQUIVALENT`. Every collision-meshed object was therefore intangible, and
  `obj_loop_exit()` (which has no door check, by design) warped the player through.
  With zero balloons the kart reached the Dino Domain lobby at frame 6589 and
  Ancient Lake at 6890. Upstream has since matched the body (decomp `9da89ecb`);
  adopting it needed two `DKR_PTR` lines here. This also revives five interaction
  sites that could never fire — including the door's own *"you need N balloons"*
  textbox. Verified by `tests/check_door_blocks.py`, which drives both arms from
  one binary via `MDKR_OBJCOLL=legacy`; identical results in Debug and Release.
  The old Tricky-1 autopilot route regresses as a consequence: nine legitimate
  hits move it 2.5 units and it misses the summit much later. The two boss gates
  now use Tricky 2, which exercises the same grid-mask/verdict contracts and
  finishes with production object collision; Tricky 1 remains an explicit
  route/oracle gap rather than a widened budget.

- **The new-save filename grid read beyond a global on every rendered glyph.**
  The decomp declared a ROM-sized four-byte character buffer as a one-byte scalar
  and passed it to the font renderer as a C string. ASan reached the screen through
  ordinary first-session navigation and aborted at frame 2052. The declaration now
  matches every retail symbol map and retains an explicit terminator.
- **Two wasm function-signature conflicts are eliminated and cannot silently
  return.** The vehicle-audio approximation named `log` no longer collides with
  libc's different `log` ABI, and bounds probes now share one declared interface
  instead of relying on implicit `int`. Clean builds also exposed and repaired
  three more undeclared calls and a browser-only dead GL fallback branch.
- **A failed native WebGPU window no longer initializes the WebGPU renderer over
  an OpenGL window.** The fallback now updates the single cached backend choice
  before `main_pc.c` selects its renderer vtable. A fault-injected failure renders
  through GL in both Debug and Release.

### Added

- **A dependency-free real-browser release gate.**
  `tests/check_browser_runtime.py` serves the committed shell plus the freshly
  linked wasm, selects the external ROM through the real file input, and drives
  3,600 paced WebGPU frames into Ancient Lake. It asserts changing screenshots,
  ~60 Hz cadence, race progress, three live resize/HiDPI transitions, AudioWorklet
  PCM, exact ROM/EEPROM restoration, both recovery buttons, clean wasm exit, and
  a zero-upload network audit in one isolated Chromium profile. Synthetic visual,
  persistence, network, and event-drop controls prove the failure directions.
- **Universal Hor+ widescreen with an explicit display policy.** World,
  contained 4:3 UI, and full-bleed transition spaces now share one renderer
  mapping; authored camera zooms are preserved, gameplay FOV is configurable,
  horizontal FOV is safely capped by default, and exact legacy stretching
  remains available. Native and browser targets respond to live resize and
  HiDPI dimensions, and two-player projection is derived from each real
  viewport rather than its scissor.
- **Both-direction widescreen, asset-proportion, and projected-shadow gates.**
  A ROM-free CTest covers layout/projection/billboard math. A seven-arm pixel
  check measures the same HUD and world balloon at 4:3, 16:10, 16:9, capped
  21:9, forced 4:3, and 75° FOV, then requires exact legacy stretching to fail
  the production threshold.
  Real-game checks also require exact simulation equivalence, safe forced
  overflow under ASan, final decal depth state, moving-camera pixel evidence on
  GL/WebGPU, and two live 21:9 player viewports.
- **A full native representation-boundary gate.** Exact legacy controls must
  emit four MEM-02 and two MEM-03 alignment diagnostics, while a bare-transform
  MEM-04 prefix read must abort under ASan. The fixed unit and source contract
  pass before halt-on-error alignment UBSan runs all menus, 20 tracks, all 47
  legal vehicles, both Adventure directions, boss/object collision, 2P, and the
  seven-arm widescreen/FOV balloon fixture.
- **A both-direction filename-entry safety gate.**
  `tests/check_filename_entry.py` isolates save state, proves the new-save screen
  was reached, rejects sanitizer/fatal output, and can require the exact historical
  ASan failure for positive-control validation. The fixed ASan, Debug, Release, and
  wasm builds were all checked.
- **Hard build gates for C and wasm call ABIs.** Implicit function declarations
  are errors in every Clang/Emscripten translation unit, and wasm-ld warnings are
  fatal. The Asyncify add-list now contains only the three Release symbols that
  actually survive optimization, so useful linker diagnostics are no longer
  buried under seven intentional missing-symbol warnings.
- **A native renderer parity and fallback gate.**
  `tests/check_renderer_backends.py` drives GL and WebGPU through an identical
  menu route into a race, compares real sampled scenes within measured pixel
  budgets, rejects a race-vs-black positive control, and forces the previously
  broken WebGPU-window-to-GL branch.
- **One fail-closed suite runner and one `--build` contract.** All behavioural
  scripts accept either a build directory or an executable.
  `tools/run_checks.py` registers all 32 check scripts plus the ROM-free CTests,
  rejects omissions, routes Release/ASan/UBSan/wasm checks to their correct
  artifacts, and runs all 39 current tasks sequentially so save-mutating fixtures cannot
  race. The v0.2 release gate passed the 37-task optimized native/sanitizer
  stage in 21m09s, the 28-task Debug primary stage in 11m43s, and both
  wasm-only tasks — including real Chromium — in 1m05s.

## [0.1] — 2026-07-25

First tagged release. Native (macOS, Linux) and browser (WebGPU) builds, both
bring-your-own-ROM. Claims below are limited to what the headless suite
demonstrates: 18 behavioural checks, a 20-track sweep, 47 (track, vehicle)
combinations, and two fail-closed clean-room gates.

### Fixed

- **A completed Taj challenge replayed forever, and wedged the player.** `tajFlags` is
  two parallel triples — OFFERED and BEATEN — written minutes apart, and the offer gate
  consults only the OFFERED half while the auto-offer dialogue never asks. A save in the
  "beaten but never offered" state re-entered a finished challenge on *every* visit to
  Timber's Island, with racer input disabled by a dialogue the pad cannot advance.
  Measured over 9909 post-hub frames: 37767 units driven when consistent, **14439 units
  and 76 % stationary** when not. Repaired in one direction only — a challenge cannot be
  beaten without having been offered — so it can only restore a bit that play must
  already have set.
- **Both racers fell through the volcano climb in the first boss race.** One tautological
  comparison in `compute_grid_overlap_mask()` (`z2 >= bbox_z1`, already guaranteed by the
  clamp above it) meant the Z half of the collision pre-filter never filtered; the
  500-entry candidate list saturated and truncated, discarding the ground. Truncations
  73 → 0, boss airborne 301 frames → 16, and the race became completable at all. **A new
  bug shape:** the C body is a transcription of hand-written assembly that upstream never
  compiles (it links the real `.s` via `GLOBAL_ASM`), so no amount of upstream matching
  progress could ever have surfaced it.
- **`vec3f_rotate_py` had pitch and yaw transposed** — a horizontal direction came out
  vertical. Reached 250×/1500 boot frames and up to 6761×/race: particles, lens flare,
  spotlights, sprite placement. Found by diffing every hand-asm C body against its `.s`.
- **A five-row write into a four-row array let the game load an arbitrary level.**
  `trackmenu_init()` fills five rows into `gTrackSelectIDs[4][6]`; on the N64 row 4 *is*
  `gFFLUnlocked`, which has no other writer. Our linkers separate them, so it was never
  written — the cursor could reach the Future Fun Land row before it was unlocked and
  pressing A loaded a level id read from an unrelated, per-frame-rewritten object. Wrong
  on **both** targets, not just wasm.
- **Two out-of-bounds walks crashed the browser build.** `waves.c` treats two separate C
  objects as one 26-entry table; wasm-ld leaves an 8-byte hole where Mach-O leaves none,
  so the walk desynchronised from its sentinels permanently and its own store landed
  inside `gWaveBlockIDs`. Both crashes a player reported are this defect; the check fails
  against both of the exact binaries that produced them.
- **The character-select dance ran ~8× too fast.** `osGetCount()` could return the same
  value twice in one frame — by construction headlessly, and because browsers clamp
  `performance.now()` to ~100 µs — and `music_animation_fraction()` cannot survive a zero
  delta: the `else` branch's `0u - 1` moved the beat phase 22.4 ms *backwards*, seven
  times per frame. Measured against the real ROM: 4.7 frames per cycle versus 36–38.
- **A checksum-valid save could crash every boot**, which in a browser is a permanent
  loop; and torn saves were adopted. `wizpigAmulet` is 3 bits on disk for a four-piece
  amulet and was used unclamped as a model index. The store is now atomic and the load
  demands exactly 512 valid bytes, quarantining anything else instead of trusting it.
- **Two dead paths in the browser shell:** "Forget stored ROM" was never shown *and* did
  nothing, and Play was never enabled for a persisted ROM — so "your ROM persists across
  reloads" was true of the storage and false of the UI.
- **"Interlaced" (pre-swizzled) textures decoded scrambled.** About **30 %** of every
  texture the game uploads is loaded with one of libultra's `...S` macros — a
  `LOADBLOCK` with `dxt = 0`, meaning the asset's odd rows are already word-swapped
  in ROM so the RDP's fetch-time TMEM exchange cancels out. Having no TMEM, the HLE
  read DRAM rows straight through and decoded all of them with every odd row's texel
  groups transposed. Reported as the golden-balloon HUD glyph "looking corrupted" in
  the browser build; it reproduced identically in the native build on **both**
  backends, because the fault was in the shared F3DDKR HLE and not in any backend.
  Also affected: the in-race and hub minimaps (a field of disconnected dashes → a
  continuous track outline), every palm frond and plant, the frontend plane's
  propeller disc, and the IA8 particle/smoke atlases. Silent — nothing crashed and
  no check asserted on it. Locked down by `tests/check_texture_lineswap.py`, which
  renders the route both ways (`MDKR_LINESWAP=off` reproduces the pre-fix decode
  byte-for-byte) and so cannot pass vacuously. See docs/OPEN_ITEMS.md "wave
  lineswap".

### Added

- **Erase saved progress** in the launcher, separate from the ROM control, clearing
  IndexedDB directly rather than through the engine — because the state needing erasure
  is the state the engine crashes on.
- **A symbol map shipped from the same link as the wasm**, plus
  `tools/web/symbolize_crash.py`, so a browser stack trace of bare code offsets resolves
  to function names. Both after-the-fact rebuild tricks were tried first and produced
  *false* names; that is recorded so nobody repeats them.
- **`tests/check_array_bounds_sweep.py`** — a UBSan sweep over 8 routes that fails on any
  out-of-bounds index not in an allow-list whose entries each say why they are not a
  finding. Deliberately not a table of known pairs: a pair table only knows the pairs
  somebody already thought of.
- **`tools/anim_period.py`**, **`tools/compare_data_layout.py`**, and eight new
  behavioural checks. Every fix above ships one, and each was validated in both
  directions.

### Investigated and closed without a fix

- **"Winning the boss gave no cutscene and no amulet"** is correct behaviour. The
  the playtest log settles it: `bosses=0x2` means the world-1 boss bit was already set, so
  the race was a *re-race*, which correctly returns to the lobby; the amulet belongs to
  the rematch. How the save got there is the satisfying part — the win is written and
  saved 358 frames before it is *shown*, and the pre-fix fall-warp fired in exactly that
  window. One defect, two reports.
- **"Every boot drops me into a race"** could not be reproduced from any save content: 98
  boots across absent, zeroed, 0xFF, random, checksum-valid-random, fully-maxed and 11
  truncation lengths all reached the title screen.
- **No test hook is reachable in the shipped web build** — proven, not asserted:
  `grep -o "MDKR_[A-Z_0-9]*"` over the loader returns zero matches.


### Working

- **Boot to menus.** Every menu screen is navigable. Scored **93.7–95.1 %** against
  the real ROM running in a patched ares, via the visual oracle
  ([docs/ORACLE.md](docs/ORACLE.md)).
- **Racing.** All **20 tracks** load and drive, in **all three vehicles** — 47
  (track, vehicle) combinations, every one the ROM's own `available_vehicles`
  bitmask permits. A full 3-lap Time Trial can be finished, and the resulting time
  is written to EEPROM and read back after a restart.
- **Two-player split-screen.** Two viewports, two independently controllable racers,
  per-player HUD, on both renderers. 0 crashes across 10 runs per backend.
- **Audio.** Music, SFX and reverb, produced by the decompiled N64 synthesiser
  driving a software `aspMain` mixer — not sampled audio.
- **Browser build (WebGPU).** Boots, runs at 60 fps, renders title, menus and race.
  The ROM is selected in-page, read client-side, and never uploaded. Saves persist
  across reloads via IDBFS.
- **Renderers.** WebGPU by default with an OpenGL fallback natively
  (`MDKR_RENDERER=webgpu|gl`); WebGPU only in the browser.
- **Determinism.** Headless renders are reproducible frame-for-frame, which is what
  makes the regression suite meaningful.
- **Adventure mode.** The loop closes: hub → collect a balloon → through the door →
  lobby → race → three laps → back to the hub, verified 12/12 byte-identical.
- **Boss races.** Tricky's second arena runs end to end: challenge cutscene →
  race → win/lose cutscene → return. All ten boss levels load and drive; the
  first Tricky arena still needs a collision-robust automated route.
- **ROM revisions.** US 1.1 and EU 1.1 are supported and race identically; US 1.0,
  EU 1.0 and Japan are identified from their header CRCs and refused **by name**
  rather than failing mysteriously. `.v64`/`.n64` byte orders are normalised on
  load, on both the native and browser paths.

### Known broken

- **Adventure trophy results screen is not reachable.**
- **An Adventure-path *first* boss win has never been driven end to end** — the
  lobby's boss gate needs four world balloons and the gate chamber is not reachable
  by the route driver (measured closest approach 1210 units), while the lobby's five
  AI nodes all sit in the central ring. The verdict logic itself is asserted both
  ways by `tests/check_boss_win_verdict.py`.
- **The browser event queue is under-budgeted** — `[EVTQ] post DROPPED` at boot.
  Non-fatal (an existing `csplayer.c` guard covers the self-perpetuating events) and
  it does not reproduce headlessly: 0 drops in 3000 native frames.
- **Three ROM-fidelity divergences are deferred, not fixed:** the boot RNG seed,
  `gArcTanTable` generated by truncation instead of rounding (491 of 1025 entries),
  and `sins`/`coss` using libm instead of the ROM's table lerp. All three are reached
  and material; each shifts every AI racing line, so flipping them invalidates the
  route-calibrated fixtures — including one positive control, which stops reproducing
  its own defect. Opt-in via `MDKR_RNGSEED=rom` and `MDKR_ARCTAN=round`, and pinned
  by `tests/check_math_tables.py` so they cannot rot.
- **Not started:** challenge modes, 3–4 player.

### Engineering notes

The dominant defect class in this port was 64-bit (LP64) pointer and struct-layout
divergence from the original 32-bit big-endian target, plus endianness of
ROM-resident data. Notable root causes fixed in this release:

- LP64 pointer truncation — a systematic sweep of truncate-then-dereference sites,
  which had been presenting as unrelated intermittent crashes.
- ROM-overlaid structs converted to 4-byte `dkrptr32` token slots, with 43+
  `_Static_assert` offset locks so layout drift fails at compile time.
- Halved fixed-point trig amplitude — the single root cause behind both tiny racer
  models and crawl speed.
- An LP64 delay-line tap in the audio reverb path that overran the arena.
- Near-plane clipping absent from the F3DDKR HLE.
- Texture cache binding a freed asset after arena reuse (TRACK_SELECT preview: 61 %
  → 94 %).
- Big-endian 16-bit texel/TLUT decode, and `ASSET_MISC` table swizzles.
- Three wasm32-specific pointer-recovery defects in the browser build.

Several of these were completely silent — a big-endian float read back as a
denormal, a `-0.0` numerator. The bug taxonomy is in
[docs/DEVELOPER_HANDBOOK.md](docs/DEVELOPER_HANDBOOK.md); every write-up with its evidence is in
[docs/open-items/](docs/open-items/README.md).

### Legal and release posture

- MIT license covering first-party work only, plus `NOTICE.md` provenance and
  `DISCLAIMER.md`.
- `tools/check_no_rom.sh` — fail-closed structural scan of shipped artifacts for N64
  ROM header magic in all three byte orders.
- `tools/check_clean_room.sh` — fail-closed proof that no ROM or ROM-derived data is
  in the working tree *or anywhere in git history*.
- Publication is `workflow_dispatch`-only; it never fires on push, tag or schedule.

### Repository hygiene (this release)

- Removed four GoldenEye-only translation units that had been vendored into
  `platform/` but could never compile here (they referenced headers absent from this
  repo): `port_trace.c`, `config_pc.c`, `frame_clamp.c`, `audio_ring.c`.
- `.gitignore` now covers ROMs in all three N64 byte orders (previously only
  `.z64`), plus audio dumps, stray EEPROM saves, and emscripten sourcemaps.
- Documentation reorganised: per-milestone specs consolidated under
  `docs/architecture/`, indexed by [docs/README.md](docs/README.md);
  `docs/OPEN_ITEMS.md` gained a subsystem table of contents.
- Added `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SECURITY.md`, and
  `docs/RELEASE_CHECKLIST.md`.
