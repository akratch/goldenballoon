# Developer handbook

Orientation for anyone working on this port. **Read §3 before debugging
anything**: the recurring 64-bit and endianness bug shapes it lists account for
nearly every hard defect here.

**Golden Balloon** is a native and browser source port of *Diddy Kong Racing*,
built on the community
[DavidSM64 decompilation](https://github.com/DavidSM64/Diddy-Kong-Racing) for
game logic, with an original platform layer under `platform/`. It reuses the
platform spine developed for **mgb64**, a sibling GoldenEye port; findings that
apply to both are sent back through [`MGB64_BACKFLOW.md`](MGB64_BACKFLOW.md).

Related reading:

| For | Read |
|---|---|
| Build and play | [`../README.md`](../README.md) |
| Contributing a change | [`../CONTRIBUTING.md`](../CONTRIBUTING.md) |
| Standing architecture decisions | [`ARCHITECTURE_DECISIONS.md`](ARCHITECTURE_DECISIONS.md) |
| Known and fixed defects | [`open-items/`](open-items/README.md) |
| Deferred work | [`../ROADMAP.md`](../ROADMAP.md) |
| Milestone history | [`STATUS.md`](STATUS.md) |

---

## 1. What exists today

**It is playable.** Native: boots → menus → drives a full lap of Ancient Lake with
working HUD, audio, and boost effects. Browser: the same engine compiled to wasm32,
running on WebGPU in Chrome, rendering title/menus/race correctly.

| Area | State | Demonstrated by |
|---|---|---|
| Native build | macOS arm64. WebGPU is the qualified fail-closed default; GL is available only through explicit diagnostic selection (`MDKR_RENDERER=gl`) pending visual-parity work. Production can replay immutable adjacent tasks for presentation-only interpolation; WebGPU admission never blocks the gameplay/audio frame path and reserves endpoint capacity, while minimized windows elide GPU walks | `check_presentation_matrix.py`: authority, immutable endpoint, and unique-midpoint proof; `check_renderer_backends.py`: route/pixel parity, dense default-WebGPU intro identity, and forced startup-failure rejection; `check_gpu_backpressure.py`: zero WebGPU runtime waits plus the live GL fence ceiling; `check_surface_suspension.py`: GL/WebGPU minimized control with state/event/input/PCM invariance |
| Browser build | wasm32 + WebGPU + Asyncify rAF loop + AudioWorklet + ROM picker + IDBFS saves | `check_browser_runtime`: actual Chromium, 3,600 authored-cadence frames into a race with no sub-two-field update, five changing scenes, live fullscreen/CSS/DPR resize, AudioWorklet PCM including nonzero fixed-mode RAW16 loads, exact ROM/EEPROM reload, erase recovery, and zero-upload network audit; `check_browser_presentation_rates`: display/capped/irregular rAF authority parity, production interpolation, queue holds, and honest uncapped fallback |
| Presentation modes | Pure 4:3 reference; Restored/Remastered widescreen with CPU mip chains, configured anisotropy, and working 2× supersampling by default on GL/WebGPU; the localized in-game screen controls mode, 1×–4× SSAA, aspect/FOV, filtering, effects, and subtitles with truthful live/restart behavior and atomic native/browser persistence; Remastered reconstructs the shared ROM font atlas at 4× without changing metrics | `video_config` + `video_config_runtime` + `mip_chain` + `font_sdf` CTests; `check_video_options` on GL/WebGPU; `check_video_presets`, `check_renderer_backends`, `check_widescreen_proportions`, and real-Chromium `check_browser_runtime` config mutation/reload |
| Restoration correctness | Every sprite reader shares one checked, asset-bounded serialized-layout decoder; shade and fog use the RDP's screen-linear interpolation while texture coordinates remain perspective-correct; moving textures receive complete mip chains | `sprite_layout`, `rdp_interpolation`, and `font_registry` CTests; `check_sprite_layout`, `check_rdp_interpolation`, `check_texture_lineswap`, and `check_mip_motion` |
| Remastered text | Only runtime-registered font glyph regions receive 4× signed-distance reconstruction; region isolation, point/clamp sampling, fixed logical metrics, and registry-aware cache invalidation prevent atlas bleed or stale reuse; Pure and Restored remain exact | `font_sdf` + `font_registry` CTests; `check_font_sdf` on GL/WebGPU; `check_browser_runtime` requires nonzero text-only SDF uploads |
| Widescreen + shadows | Hor+ world, proportion-correct world billboards, safe 4:3 UI, full-bleed transitions, live resize/HiDPI, 21:9 two-player projection, stable projected shadows | display CTest + pixel-level `check_widescreen_proportions` + `check_widescreen_shadow` + `check_shadow_visual_ab` + `check_race_2p_split`; `check_browser_runtime` covers three live wasm resize/HiDPI transitions |
| Native representation safety | Host-aligned object tails, bounded variable object records, and field-oriented sprite/UI renderer APIs | `check_native_layout`: exact MEM-02/MEM-03 alignment and MEM-04 ASan controls, fixed unit, source contract, nine menus, 20 tracks, 47 legal vehicles, Adventure both ways, boss collision, 2P, and the widescreen/FOV balloon fixture under halt-on-error alignment UBSan |
| Allocator safety | Checked native/browser sizes, pool membership, fixed-address splitting and host-width alignment; grow-before-write delayed frees; fail-fast safe allocation | `memory_allocator` CTest in Debug, Release, ASan, and UBSan; complete builds in all four native configurations plus wasm |
| Runtime boundaries | Owner-last level teardown, initialized racer/item state, finite path fallback, bounded sound groups/IDs, and explicit special-vehicle sound rows | `runtime_contracts` CTest + mutation-controlled `check_runtime_safety`; navigation, Adventure, PCM/RAW16, 47-vehicle, array-bounds, native sanitizer, and wasm gates |
| Boot → menus | Every screen navigable | `check_nav_fixtures.py` (all nine routes) |
| Racing | 20 tracks × 3 vehicles = 47 legitimate combinations; 3-lap TT finishes and the time round-trips through EEPROM | `check_track_sweep`, `check_vehicle_sweep`, `check_race_finish_time` |
| Local multiplayer | 2P, 3P, and 4P layouts; direct controller-to-racer binding; independent local racers/HUDs; 3P minimap; results → track-select teardown | `check_2p_human_binding` proves exact P1/P2 input and motion across renderer/rate arms; `check_race_2p_split`; `check_race_multiplayer` scores every racer and quadrant and runs flat-quadrant controls |
| Adventure | Adventure One hub → balloon → door → lobby → race → hub; Adventure Two canonical unlock/save identity plus all 20 mirrored racing lines; every authored challenge/battle course; all three Taj vehicle challenges; all four trophy championships | `check_adventure_hub`, `check_adventure_race_loop`, `check_adventure_two`, `check_challenge_modes`, `check_taj_challenges`, `check_trophy_series` |
| Boss races | Tricky 2 end to end with production object collision; all ten load/drive. The legal first-boss campaign route clears the fourth Dino race, opens the four-balloon door, physically finishes Tricky 1 in win/loss arms, returns to the hub, and reloads exact progression | `check_collision_gridmask`, `check_boss_win_verdict`, `check_first_boss_progression`; the old stock-AI summit miss remains a positive control and route-fidelity note in `OPEN_ITEMS.md`'s objcoll entry |
| Audio | Music + SFX + reverb via the software aspMain mixer; RAW16 instruments are converted from serialized big-endian PCM | `check_audio_output` covers format/energy/timing/reverb; `check_raw16_audio` inventories 25 music + 1 SFX RAW16 wave and compares fixed/exact-legacy PCM in both directions |
| ROM revisions | US 1.1 + EU 1.1 byte-identical payloads supported with authored NTSC/60 and PAL/50 source clocks; the other three named and refused; `.v64`/`.n64` normalised | `check_rom_revision` + `check_simulation_cadence` |
| Camera obstruction | The Modern resolver (`game/src/camera_obstruction_runtime.c` and the occlusion sources beside it) is **opt-in**: unset `MDKR_CAMERA_OBSTRUCTION` selects `observe`, which measures and leaves the authored camera in place, and the launcher's `Camera.Obstruction` setting is what a player turns `modern` on with. That setting is **level-scoped**, not restart-scoped: an in-game change is staged and applied by `camera_obstruction_runtime_apply_config()` at the next level load, immediately before the `camera_obstruction_runtime_reset()` that path has always run — the same reset that retires the outgoing policy's held poses. A frame-boundary apply would be a hard cut of the rendered eye with no path that fades between two policies, and the level boundary is the one moment the game already cuts the camera. `center-ray` and `legacy` remain diagnostic A/B controls, and an unrecognised string falls back to `observe` — never to a correcting arm and never to the known-unsafe one. Substitution happens at presentation depth, so neither policy moves authoritative state | the `check_camera_obstruction_*`, `check_camera_dynamic_*`, `check_camera_projection_fallback_runtime`, `check_camera_emergency_readability_runtime`, and `check_camera_3p_tt_runtime` gates, over the `camera_*` ROM-free CTests (`ctest -R '^camera_'`); design and measured fence sizing in [`architecture/camera-obstruction.md`](architecture/camera-obstruction.md) |
| Oracle | Patched ares runs the real ROM for pixel parity and US 1.1 racer-state comparison (silent by construction) | `race_state_oracle`: Bubbler's authored two-field route passes and the historical one-field arm fails as a positive control; broader strict standard-race parity remains reported separately |

**127 registered check scripts / 141 full-run tasks, each validated in both
directions.** (`tools/run_checks.py --list` prints them; the three
`tests/check_*.py` it does not name directly are CTest companions that the
`rom_free_units` task owns.)
(2026-07-29 additions: `check_shadow_stage_reset.py` and
`check_touch_controls.py`. 2026-07-31 post-1.0 additions:
`check_charselect_motion.py`, `check_shell_dropfile.py`,
`check_boost_magnitude.py`, `check_audio_level_reference.py`,
`check_collision_headroom.py`, and `check_shadow_plausibility.py`. The 2026-08-02
launcher reliability pass adds `check_overlay_pause.py`.)
The manifest also runs the ROM-free display/endian/object-layout CTests;
filename entry, locked-door collision, RAW16 audio, native-layout safety, and
widescreen/shadow safety repeat in their specialized configurations. A check
that cannot fail is not a
check; every one has a recorded positive control. `tools/run_checks.py` fails if
a script is not registered, and `docs/RELEASE_CHECKLIST.md` is the gate.
The 2026-07-26 integrated Release run passed the then-current 33 tasks in 7m40s;
the locked-door gate also passed separately in Debug and under ASan. After the
manifest expanded, the RAW16 task passed separately in Debug, Release, and ASan;
the broad audio, wasm build, and real Chromium runtime also pass. At the
`fa7adcc` billboard checkpoint, the complete Release-led manifest passed all
37/37 tasks in 7m53s, including every ASan/UBSan, linked-wasm, and real-Chromium
arm. At the v0.3 release checkpoint, the then-current manifest passed the **38-task
optimized native/sanitizer stage in 22m17s**, the **29-task Debug primary stage
in 12m31s**, and both wasm-only tasks — including real Chromium — in **1m05s**.
The restoration/remaster source branch subsequently passed the expanded
**45/45 task manifest in 31m52s**. After its integration into main at `0166585`,
all 12 ROM-free CTests passed in Debug, Release, and ASan; the five new focused
behavioural gates passed on their required GL/WebGPU arms; wasm linked cleanly;
and the shipped page passed 3,600 real Chromium/WebGPU frames with nonzero SDF
and mip telemetry, exact save reload, AudioWorklet output, and the zero-upload
network audit.

### Waves 1–3 completion snapshot

The defined waves are **23/23 complete**:

| Wave | Defined scope | State |
|---|---:|---|
| Wave 1 core safety | 6 audit IDs: MEM-11, MEM-12, PORT-01, C-01, C-02, GAME-06 | 6/6 integrated |
| Wave 1 WebGPU lifecycle | WGPU-01..06, WGPU-08, WGPU-10 | 8/8 integrated; the 46-route WGPU-11 census and complete 445-model offline asset walk pass, while independent state reference and non-Apple/minimum-feature runtime validation remain |
| Wave 2 lighting | RL-2, RL-5, CO-1 | 3/3 integrated |
| Wave 3 gameplay envelope | multiplayer, Adventure Two, challenge/battle, first boss, Taj challenges, trophy series | 6/6 integrated |

The default WebGPU+app native build exposes 117 ROM-free CTests: 98 non-GPU and
19 GPU. This wave accounting is not a claim that the entire foundation or
remaster backlog is complete.

### Wave 3 multiplayer checkpoint

The former four-player spot check and untouched three-player path are now one
registered gate. The native probe publishes all four local humans while
retaining the existing P1/P2 trace formats. Real controller-specific menu
routes load Ancient Lake with `numPlayers=2/3`, select the exact 3P/4P HUD and
viewport layouts, and keep every racer finite, moving, distinct, and progressing
through the course.

`check_race_multiplayer.py` scores all four quadrants independently. In 3P the
fourth quadrant is asserted as the intentionally mostly-black minimap, not
mislabelled as a camera; in 4P it is a fourth live scene. The check flat-fills
each quadrant in turn and requires its scorer to reject the corruption.
Edge-triggered post-race taps then prove both arms reach `MENU_RESULTS` and
return to track select. Measured transitions are 7632→7931 for 3P and
7932→8231 for 4P.

### Wave 3 Adventure Two checkpoint

The native save boundary now converts the N64 big-endian global config block
explicitly. Before this correction a checksum-valid Adventure Two unlock was
validated as serialized bytes, then interpreted as a reversed little-endian
`u64`, silently losing the unlock on native and wasm hosts.

Adventure Two also exposed a renderer defect: DKR mirrors its world with a
negative N64 viewport X scale, but modern GL/WebGPU viewports require positive
dimensions. The HLE now publishes the absolute host rectangle and applies the
retained sign to clip-space X, which is algebraically equivalent and leaves the
subsequent safe-4:3 HUD viewport unmirrored.

`check_adventure_two.py` selects the real mode and save, traverses the campaign
hub/lobby route, and drives all 20 race tracks. It requires live mirror witnesses
from stereo, minimap, steering, and the translated viewport; canonical EEPROM
bytes and checksums; and a pixel positive control. Across 14 pre-start samples,
Adventure Two differs from the unflipped Adventure One world by MAD 49.154 but
matches its horizontal reflection at MAD 2.816.

### Wave 3 challenge, boss, Taj, and trophy checkpoints

`check_challenge_modes.py` covers the four authored egg, treasure, and battle
courses in win/loss arms, including production results, TT-amulet progression,
EEPROM, fresh-process reload, and terminal-gate controls. It also carries the
giant character portraits (`BHV_CHARACTER_FLAG`, Fire Mountain and Smokey
Castle) as a paired pixel witness on both production backends: the same arena is
run twice on GL and again on WebGPU, identically except for
`MDKR_SUPPRESS_PORTRAITS`, and the two framebuffers must differ while the
gameplay traces stay identical. The battle arenas author no portraits, and that
zero is pinned rather than skipped. Binding traces alone cannot see a portrait
that submits a draw and paints nothing, which is exactly how the run-time
`DKR_TRIANGLE`/`TexCoords` byte-order defect stayed invisible for a release.

`check_first_boss_progression.py` starts from a checksum-valid legal checkpoint,
wins the fourth Dino Domain race, enters the four-balloon boss door, and
physically finishes Tricky 1. Its win and loss arms return to the hub and reload
the exact save; the unrecovered stock-AI line is retained as the failing control.

`check_taj_challenges.py` covers car, hovercraft, and plane at the live 5/10/18
balloon thresholds, with first win, loss, abort, completed replay, suppressed
completion, rendered evidence, EEPROM, and fresh-process reload.

`check_trophy_series.py` drives all 16 rounds in all four worlds, including
stable ties, gold/silver/bronze/no award, quit/retry, cinematic, EEPROM, and
fresh-process cabinet state. It also fixed the rankings QUIT option being
ignored.

Repo shape: `game/` ~133k lines (vendored decomp + `NATIVE_PORT` patches),
`platform/` ~48k lines (our port layer + mgb64-derived backends).

---

## 2. Architecture (settled — see ARCHITECTURE_DECISIONS.md for the authoritative list)

1. **Cooperative single thread.** No real threads; `osCreateThread` is a no-op, message
   queues are ring buffers, and a blocking `osRecvMesg` on the video queue *is* the frame
   boundary. This is what makes the browser build possible (Asyncify suspends there).
2. **One 16 MB arena** stands in for RDRAM. On wasm32 it is placed above the 256 MB
   N64-segment-token ceiling so arena addresses can't be mistaken for segment tokens.
3. **32-bit pointer slots.** DL-embedded pointers are `dkrptr32` tokens resolved via
   `dkr_lo32_to_ptr()`. DKR's tokens are `(u32)hostptr ^ 0x80000000` (the `OS_K0_TO_PHYSICAL`
   flip). On wasm32 the resolver recovers globals/rodata directly from the token.
4. **ROM-overlaid structs keep N64 layout.** Embedded pointers become 4-byte tokens under
   `NATIVE_PORT`, with `_Static_assert` offset locks. 63 locks in place.
5. **Assets normalised at the load boundary** (big-endian → native), per asset type.
6. **Graphics = F3DDKR HLE** (`platform/fast3d/gfx_pc_dkr.c`, ~new code) over the
   shipped GL and WebGPU backends at the `GfxRenderingAPI` seam. macOS GL
   remains Metal-backed by the OS. A standalone Metal backend (`gfx_metal.mm`)
   was never built by any target here and was removed post-1.0.6; it lives on
   in git history and in the sister **mgb64** project if a Metal backend is
   ever revisited.
7. **Audio** = the first-party clean-room engine (`platform/audio_compat.c` and
   friends), its `Acmd` output executed by the software aspMain mixer
   (`platform/mixer.c`) via macro override. See
   [`architecture/audio.md`](architecture/audio.md).
8. **Frame pacing** drives VI-field retraces from wall-clock so DKR's own frameskip
   compensation works (`updateRate` = true elapsed 60 Hz fields).

---

## 3. The dominant bug class (read this before debugging anything)

Almost every hard bug this project has hit is one of a small set of recurring
shapes. **Ten of them come from running N64 code on a 64-bit host** and are in the
table below; four more, described after it, come from elsewhere (the HLE renderer,
the test suite, and the decomp's hand-assembly transcriptions). Check the table
first:

| Shape | Example found |
|---|---|
| **Allocation sized for 4-byte pointers** | `gLoadedObjectHeaders` alloc'd `len*4` but stores 8-byte host pointers → overran into the adjacent refcount array → loader returned all-zero headers |
| **`sizeof(struct)` ≠ on-disk record stride** | `Object_Boost` is 0x80 on disk, 0x88 on LP64 (trailing pointer fields) → every record past the first mis-parsed |
| **`(u32)`/`(s32)` cast of a pointer, then dereferenced** | `(s32)` sign-extended a reconstructed arena pointer into a wild address (char-select crash); `(u32)` truncation in menu geometry |
| **Big-endian asset never byteswapped** | `ASSET_MISC_8` read `600.0f` as a *denormal* → `-inf` velocity → world stopped drawing |
| **`long` inflates a struct** | `Gsetcolor.color` as `unsigned long` doubled `sizeof(Gfx)` 8→16, so a hardcoded-8 stride overran vertex data (the "red spikes") |
| **Heterogeneous tails/variable records/fake common prefixes inherit N64 alignment or adjacency** | Object tails placed pointer-bearing structs at 4 mod 8; a legal 10-byte map record misaligned the next native union; renderer casts read an animation-frame byte beyond a bare transform |
| **A callee writes N elements through a pointer parameter with no count** | `collision_get_y()` writes one `f32` per intersecting triangle into caller arrays of 8, 8, 9, 10 and 30, with no count parameter — five capacities, one callee. No sanitizer can see it: `array-bounds` needs an indexed array *type* and the callee has only a pointer. The instrument is a bound parameter, and `tools/sweep_bug_shapes.py` enumerates the class |
| **A saturation cap tested with `==` against an insert that can step over it** | `generate_collision_candidates()` tests `j == 500` after the *facet* insert, but the *segment* insert advances `j` untested — enter at 499, write 500, and the test is never true again |
| **One ROM array split into two C objects, then indexed across the boundary** | `waves.c`'s `D_8012A5E8[2]` + `D_8012A600[24]` used as one 26-entry table (two browser crashes); `menu.c`'s `gTrackSelectIDs[4][6]` written as `[5][6]`, where row 4 **is** `gFFLUnlocked` on the N64 — so Future Fun Land read as unlocked on a fresh save and a phantom track-select row loaded an arbitrary level id |
| **A shift whose count the ROM expected the *hardware* to mask** | MIPS `sllv` takes the count from its low 5 bits, so the decomp writes progress flags as `FLAG << (i + 31)` meaning `<< (i - 1)`. In C a count `>= 32` is UB, and the surrounding range checks pin it to *always* `>= 32`, so clang folds it to 0 at `-O2` and deletes the load, the test **and** the store. Both halves of a "show this once" latch vanish at once: the gate always fires and nothing is ever recorded. Four sites; two were live defects (the world-key cutscene replaying after every race, and Taj's OFFERED bit never being written at all) |

Two of these were **silent** (denormals, `-0.0` numerators) and one produced *no crash at
all* — which is why the regression suite now asserts behaviour, not just exit codes.

The last row carries a second lesson that costs a whole wave if you miss it. **The
native build defaults to Debug and the web build to Release** (`CMakeLists.txt:6-14`), so
any defect whose mechanism *is* optimisation exists only in the build players actually run,
and every native check stays green. Worse, wave "tajprogress" probed that exact shift,
measured it correct, and wrote it off as luck — the probe was a Debug build, where it
*is* correct. **A probe run at `-O0` says nothing about a defect whose mechanism is
optimisation.** When a report comes from the browser and reproduces nowhere, build
`-DCMAKE_BUILD_TYPE=Release` natively before assuming the difference is wasm; that is
what turned "not reproducible" into a byte-for-byte A/B here. UBSan
`-fsanitize=shift-exponent` is the mechanical detector for this row, and clang's
`--target=wasm32 -O2 -S -emit-llvm` will show you what the browser really compiles
without an emsdk install.

The last row is the one you cannot reason your way out of: measured with
`tools/compare_data_layout.py --stats`, **432 of the 1001 data pairs that have the same
neighbour on both targets sit at a different distance** on LP64 Mach-O than on wasm32.
Adjacency on the host predicts nothing about the browser. `-Warray-bounds` sees none of
it (every case indexes with a variable) and ASan sees none of the global cases (they are
`__DATA,__common`, which it does not redzone); **UBSan `-fsanitize=array-bounds` sees all
of them, without any crash**, and `tests/check_array_bounds_sweep.py` now runs it over
the fixture set and fails on anything not already triaged. Run it after any `game/src`
change.

There is an **eleventh shape, specific to the HLE renderer**: *an RDP behaviour the assets
were authored to cancel out, which we never reproduce because we have no TMEM.* The
worked example is the odd-row TMEM word swap ("interlaced" textures, `LOADBLOCK` with
`dxt = 0`): ~30 % of every texture DKR uploads is pre-swizzled in ROM to cancel the
RDP's fetch-time exchange, and reading DRAM rows straight through decoded all of them
scrambled — the reported "golden balloon glyph looks corrupted", plus the whole
minimap, every palm frond, and the particle atlases. Silent for 107 commits.
See `docs/OPEN_ITEMS.md` "wave lineswap". Before assuming an HLE shortcut is
equivalent, ask what the *asset pipeline* was compensating for.

And a **twelfth shape, in the tests rather than the port**: *a fixture that replays a
fixed input sequence and then asserts against the one trajectory it happened to
produce.* A racing line is chaotic with respect to any change in the simulation, so
such a fixture fails whenever anything moves — for reasons that have nothing to do
with what it tests — and the pressure is then to loosen its thresholds, which turns
a real check into a decorative one. It had already forced one recalibration
(`MIN_FINAL_CP` 20 → 15) before three ROM-fidelity corrections broke four fixtures
at once. **Steer toward a target each frame and assert on the outcome**
(`MDKR_AUTOPILOT`, `MDKR_DRIVE_ROUTE`), and when a check needs a specific event,
reach it by writing the one field the code branches on rather than by perturbing
the physics until the branch happens to be taken — `MDKR_BOSS_SLOW` scaled the
boss's velocity to 0.15× to make the human win, and because DKR's AI paths relative
to the field that moved the *human* off a cliff. `MDKR_BOSS_WIN` writes
`finishPosition` instead. See `docs/OPEN_ITEMS.md` "wave closedloop" and the
classification table at the top of `tests/README.md`.

### The reason this list is worth keeping

**Every one of these shapes was discovered from a single report and then swept as
a class, and the sweep always found more than the report did.** One racer falling
through one volcano became
an audit of ~90 C bodies transcribing hand-written assembly, which turned up
`vec3f_rotate_py` with pitch and yaw transposed (reached hundreds of times per race)
and a silently skipped collision batch. One browser crash in the wave renderer became
a comparison of 1001 adjacent global pairs across both targets — 43 % of which sit at
different distances — and turned up a five-row write into a four-row array that let
the game **load an arbitrary level**, wrong on native as well as wasm. One scrambled
texture became ~30 % of all texture uploads.

So when you fix something here, the fix is not done until you have answered: what is
the general shape, where else could it occur, and what *mechanical* instrument finds
every instance? Reading only finds what you already suspect; the sweeps that worked
used a sanitizer, a differential against ground truth, a link-map comparison, or a
count over a data table. If the instrument does not exist yet, building it is part of
the fix — that is where `tests/check_array_bounds_sweep.py` and
`tools/compare_data_layout.py` came from. `CONTRIBUTING.md` rule 6 states this as
policy; add any new shape you find to the table above.

---

## 4. Testing (all MUTED + HEADLESS)

**Audio safety is a hard rule.** Always pass `--headless-frames N` for game or
test runs — it returns before the SDL audio device is ever opened
(`platform/audi_port_dkr.c:188`). The controlled native sink exception is
`check_audio_sink_evidence.py`: it explicitly sets `MDKR_TEST_HEADLESS_AUDIO=1`
with `SDL_AUDIODRIVER=dummy` to prove SDL queue acceptance, never physical output.
Recognized `--help`/`-h` now returns before
ROM/window/audio initialization; an unrecognized flag still starts the ordinary
interactive path. `MDKR_AUDIO=0` is belt-and-braces — note `MDKR_AUDIO=off` is a
**no-op**, the code tests for `"0"`. The ares oracle is silent by construction —
`tools/run_oracle.sh` runs it with `SDL_AUDIODRIVER=dummy` (no device is ever
opened) plus `Audio/Mute` + `Volume=0`. It previously passed
`Audio/Driver=None`, which is **not a valid key** in the pinned ares: that made
ares abort before loading the ROM, so the guarantee rested on a setting ares
rejected. See `docs/ORACLE.md`.

```bash
# race stability
for i in $(seq 1 20); do MDKR_AUDIO=0 ./build/mdkr64 --headless-frames 2900 \
  --input-script tests/input_scripts/race_drive_time_trial.txt --rom baserom.us.v80.z64 \
  >/dev/null 2>&1 || echo CRASH; done
# the behavioural check (catches silent world-stops-drawing / -inf)
MDKR_AUDIO=0 python3 tests/check_race_drive.py
# idle-attract soak (exercises the audio sequence player hard)
MDKR_AUDIO=0 ./build/mdkr64 --headless-frames 12000 --rom baserom.us.v80.z64
```
Fixtures: 9 menu-nav scripts + `race_drive_time_trial` + `race_drive_long` (7500 frames,
full lap, real zip-pad boost) + `race_full_3lap` (drive to the race's end; use it with
`MDKR_AUTOPILOT=1`, which drives the human racer with DKR's own AI and holds a
consistent ~1650-clock lap — the open-loop route strands the kart after lap 1). `check_race_drive.py` asserts position sanity, forward
progress (checkpoints/laps), **and** that late frames still render a real scene — calibrated
against the actual broken frame (59 colours/σ 5.9) vs healthy (620–2720/σ 22–47), and
verified to fail on a reverted build.

Visual checks: `--dump-frames DIR` + `sips -s format png`. Never a live window.

**Headless runs are byte-reproducible — and that is load-bearing, not free.** Until
the `osGetCount()` fix they were not: the COUNTER came from the host wall clock, so
music-synced menu animation had a different phase every run (10 runs → 10 distinct
frames, median 18.9 % of pixels), which quietly invalidated *every* frame
comparison including oracle scoring. `tests/check_determinism.py` guards it; run it
after any change to the pacer, the counter, or anything the game reads as time.

---

## 5. Keeping up with the decomp

Upstream reached **100% matching** in July 2026. This port is synced to
`c6695703` (2026-08-07), which is a real commit on upstream `master` — verify
that with `git merge-base --is-ancestor` before recording any baseline, because
two of the last three recorded baselines were private-fork commits. Whatever
this paragraph says, [`DECOMP_SYNC.md`](DECOMP_SYNC.md) and `.decomp-baseline`
are canonical for sync state.
**Never re-copy files over `game/`** — it carries our `NATIVE_PORT` patches. Use
the 3-way merge tooling:

```bash
tools/sync_decomp.sh --dry-run    # what would change
tools/sync_decomp.sh              # apply clean merges, leave conflict markers
```
Baseline commit lives in `.decomp-baseline`. Full process + validation in
**`docs/DECOMP_SYNC.md`** (includes a sync log). Rule for conflicts: take *theirs* for
decomp logic, keep *ours* inside `#ifdef NATIVE_PORT`. A *matching* refactor upstream
should be behaviour-neutral — the racer probe numbers must be byte-identical before/after.

### Reading vendored code: `game/include/decomp_names.h`

A residue of the vendored tree is still named after the address it lives at —
`func_800BDC80`, `D_8011C238`. **`game/include/decomp_names.h`** is the glossary:
one entry per symbol, giving a readable name and, on the same line, the evidence
that earned it.

The entries are `#define`s in the direction `readable_name` → raw symbol, so a
port-owned file can `#include "decomp_names.h"` and *call* the readable name
while the compiler and linker still see the raw one. It works on a definition
too — `platform/audio_compat.c` defines `alSynSetVoiceAuxBus()` and the object
file still exports `func_80065A80`.

**The constraint that shapes all of this: never rename a symbol in vendored
text.** Every raw token in `game/{src,include}` is a line upstream may also
edit, and renaming it converts the next sync's clean merge into a hand-resolved
conflict — permanently. That applies to the port's own `#ifdef NATIVE_PORT`
blocks too: they *live* in vendored files, so their call sites stay raw and use
the header purely as a lookup. Only files the decomp does not have may use the
aliases in code: everything under `platform/`, and the port-authored sources in
`game/src` (`taj_*.c`, `camera_*.c`, `object_layout.c`, `sprite_layout.c`,
`runtime_contracts.c`, `hasm_native/`). The tree carries exactly one deliberate
in-place rename, `camSetProjMtx`, and `DECOMP_SYNC.md` records it as a standing
conflict accepted on purpose.

**Adding a name requires an evidence line.** Cite the thing that proves it — an
existing comment, the `switch` arm the function is dispatched from, the named
field its result is stored into, a check that pins the contents. Mark
role-derived readings with `~` in the comment and put `_UNVERIFIED` in the name
when even the role is a reading. If you cannot cite anything, leave the symbol
out: **a wrong name is worse than `func_`**, because the address at least admits
that nobody knows. The header's closing section lists the symbols this rule has
already refused, and `docs/open-items/misc.md` (wave "decompnames") counts what
is left.

Comments keep citing the raw address even where the code no longer does. The
address is the searchable key into `symbol_addrs.us.v80`, the `.s` files and
upstream's tracker; a readable name is not.

---

## 6. Next work (priority order)

Everything below is *ready* unless marked blocked. The task list in the harness mirrors
this; `docs/OPEN_ITEMS.md` has the evidence for each.

Waves 1–3 are complete. The current work outside those waves is prioritized in
[`../ROADMAP.md`](../ROADMAP.md):

1. ~~the remaining local broad-UB correctness tranche~~ **DONE on the
   available compiler/host matrix**, including pointer-width closure;
2. ~~broad browser stage/resource cycles plus explicit voice/system-heap
   ownership~~ **DONE on available hosts**: native and real-wasm four-load
   routes prove stable heaps, exact voice/state conservation, coherent renderer
   generations, and zero terminal host/backend ownership;
3. the remaining legal Adventure graph and one long natural campaign;
4. Linux Wayland/x86_64/physical-GPU, Windows, alternate-ABI,
   standalone-Metal, and physical-controller release-matrix evidence;
5. WGPU-11's offline/reference-state and external/minimum-feature corpus;
6. broaden the new independent original-game state/racing-line oracle and
   settle authored-physics versus enhanced-60-Hz policy; and
7. broader colour/tonemap, materials, atmosphere, and grading.

F-28/WGPU-09 are closed at this checkpoint: GL consecutive dumps retain the
completed pre-swap composite, and real Chromium enforces 20.0/25.0 ms p95/p99
plus a two-frame async pipeline compile/hold ceiling. The measured one-frame
maxima do not justify enabling the inherited persistent prewarm cache.

The canonical save codec, browser export/import/editor, fail-safe snapshots,
attract behavior, aggregate-save isolation, in-game video/accessibility menu,
and current retail-ROM decision are complete. The frozen product supports US
1.1 and European 1.1; another retail revision is a separately gated future
expansion.

### Lead checkpoint before the next repair wave

Object-model collision is integrated, and main is deliberately paused at a clean,
validated boundary through object collision. The same-binary locked-door A/B
passes in Debug, Release, and ASan; the then-current Release manifest passed all
33 tasks, including the real wasm/WebGPU Chromium run. No boss budget was widened
and no boss check uses the legacy collision arm to obtain a pass.

The subsequent RAW16/endian wave is checkpointed at `a651cdc`: its focused gate
passes in Debug, Release, and ASan, the broad audio gate remains green, wasm
compiles cleanly, and actual Chromium reaches nonzero fixed-mode RAW16 loads.
The expanded 37-task manifest passed completely at the later `fa7adcc`
checkpoint in 7m53s (historical figure — the live task count is tracked in
§6, item 6, "Normalise the `--build` convention").

The widescreen refinement is checkpointed at `fa7adcc`. The original display
policy kept HUD art undistorted but missed regular world billboards: F3DDKR adds
their local vertices after projection, so the fixed 4:3 compensation widened a
collectible balloon by 1.37x at 16:9 and 1.78x at 21:9. The correction now scales
the billboard matrix's output columns for the active viewport and effective lens,
preserving rotated sprites, authored size, FOV behavior, and exact legacy
stretching. A five-arm pixel gate passes Debug GL/WebGPU, Release WebGPU, and
ASan GL; the rebuilt wasm and real Chromium resize/runtime gates also pass.

The 2026-07-26 refinement expands that gate to seven arms: 16:10 and forced 4:3
inside 21:9 join 4:3, 16:9, capped 21:9, changed FOV, and exact legacy stretch.
The current Release GL/WebGPU fixture produces 99×36 HUD / 48×18 world motifs
at ordinary production lenses; the deliberate legacy arm still measures
173×36 / 84×18. A source census now rejects any new direct post-projection
billboard producer outside the audited world/ortho builders; portrait
layout/projection and rotated billboard columns are ROM-free unit cases.

The native representation-boundary wave is checkpointed at `a7cdb95`. One
checked host-layout cursor now owns every object tail; serialized object records
are bounded and behavior-minimum-validated before typed access; and native
sprite/UI renderer entry points accept real transform/frame fields rather than
larger fake object prefixes. Exact legacy controls produce four MEM-02 and two
MEM-03 alignment reports and one MEM-04 ASan over-read. The fixed gate passes
under halt-on-error alignment UBSan across all menus, tracks, legal vehicles,
both Adventure directions, boss/object collision, 2P, and the balloon/FOV pixel
fixture. Debug, Release, ASan, and wasm builds also pass.

The standard complete manifest at `7a7f2f7` passed all **38/38 tasks in
19m51s** (historical figure; see §6, item 6 for the live count), including
linked wasm and real Chromium. Its first invocation had one
anonymous signal-6 exit on the last alignment vehicle row after 46 passes; the
exact level-15 plane route then passed 20/20 repetitions and the whole layout
task passed on a strict rerun. The sweep had hidden child output unless `-v`;
`7a7f2f7` makes every failure print its diagnostic tail and recognizes UBSan
text. No retry or tolerated-failure rule was added. The next standard full
invocation passed 38/38, including that row.

At the 2026-07-26 checkpoint, the audit denominator carried in the earlier
write-up was wrong. The committed
`0ea020f` audit has **35 unique primary IDs**, not 43; the preserved expanded
survey plus the reproduced RAW16 finding has **47**. At this checkpoint **15/35
original IDs (42.9%)** and **19/47 current IDs (40.4%)** are closed. The nineteen
current closures are BUILD-01, MEM-01, MEM-02, MEM-03, MEM-04, MEM-05, MEM-06,
MEM-08, MEM-09, MEM-10, PORT-05, AUDIO-01, AUDIO-02, AUDIO-03, GAME-01,
GAME-03, VIS-01, VIS-02, and WGPU-07. Partial work is not credited as complete
in that historical count.

That 19/47 figure is historical. The current disposition table in the audit is
**36/47 (76.6%)** after the integrated core-safety, WebGPU, multiplayer,
first-boss/trophy, and attract-demo work; it is an audit-register fraction, not
an overall project-completion percentage.

MEM-05 is implemented at `07ea545`. The allocator property gate covers terminal
list nodes and every coalescing direction, the 256→257 delayed-free boundary,
foreign/one-past/null pointers, invalid and overflowing sizes/timers, slot and
byte exhaustion, exact fits at metadata capacity, transactional two-split fixed
allocation, and full-width alignment. Debug, Release, ASan, and UBSan CTests,
their complete native builds, and the wasm link pass.

The runtime-boundary batch is implemented at `05cff1f`. It closes MEM-06,
MEM-08, MEM-09, MEM-10, AUDIO-01, and AUDIO-02: level teardown is owner-last;
plane input and ordinary weapon targets have defined defaults; zero-checkpoint
and zero-distance recovery cannot create NaNs; sound groups and sound IDs use
exclusive domains; and flying-car/loop audio maps explicitly to validated
plane/car rows. The ROM-free unit and mutation-controlled production census
pass in every native configuration. Navigation, full Adventure return, unchanged
PCM/RAW16, all 47 legal vehicle combinations, the array-bounds sweep, and wasm
compilation pass.

GAME-03's reported three-lap Adventure "non-finish" was a harness identity
error: `(*gRacers)[0]` is starting-grid order and the probe had followed an AI.
`gRacersByPort[PLAYER_ONE]` proves that the human naturally completes all three
laps. The old checkpoint's fifth-place result was not a stable oracle: after the
runtime-boundary batch the current fixture naturally wins at frame 12186 /
clock 4711.

`check_adventure_race_loop.py` runs isolated full-lap win and loss arms, requires
the same pre-control natural place/frame/clock/lap, and independently decodes
Ancient Lake's EEPROM slot. Symmetric verdict controls act only after natural
completion: loss persists status 1 / `(1,0,0,0,0,0)` and win persists status 2 /
`(2,1,0,0,0,0)`. Debug, Release, ASan, alignment, and wasm builds pass at
`424c4d6`; Time Trial and boss-verdict checks remain green. With no explicit
test environment variable, production gameplay is unchanged.

### Post-v0.3 visual repair checkpoint — `6ab4dad`

Three user-visible defects are fixed at one reviewed checkpoint:

- Banana sparkle sprite 162 emitted 29 display-list commands into space for 27
  and overwrote its own first vertex; lava-spurt sprite 177 was short by one.
  Sprite construction now computes and enforces the exact aligned layout with
  checked arithmetic, full failure unwinding, and capacity-before-construction.
  A Pure-mode binary A/B differs only on the ten sparkle frames, and the same
  3,300-frame route is clean under ASan.
- The moving dark/bright effect over the lower third was fogged road/sand
  geometry crossing the near plane, not a projected vehicle shadow. Clipped
  vertices now rederive fog from their new `z/w`, and fog is interpolated in
  screen space on GL and WebGPU. A four-arm renderer A/B independently isolated
  both corrections.
- Supersampling could not invent smoother contours from the source-resolution
  ROM font atlas. Remastered now reconstructs only explicitly registered font
  textures at 4× with signed-distance alpha and alpha-weighted colour sampling,
  while retaining logical dimensions, glyph metrics, kerning, colours, and
  safe-4:3 placement. Menu, dialogue, HUD, and filename glyphs share this path;
  Pure and Restored remain legacy-exact. Decorative text baked into unrelated
  artwork is deliberately untouched.

Fresh final-source verification: Debug and ASan builds plus all 8 CTests pass;
the ASan banana route exits cleanly after 3,300 frames; wasm builds with the ROM
absence guard; and a strict rerun of `check_browser_runtime.py` passes 3,600
wasm/WebGPU frames at median 60 fps, live resize/HiDPI, AudioWorklet, exact
ROM/EEPROM reload, recovery controls, and zero-upload audit. One immediately
preceding browser run completed all visual frames but reported an SFX
high-water mark of 280/512; the strict rerun measured 117/512 with zero drops.
No queue budget or gate was changed.

### Restoration/remaster integration — `0166585`

The restoration/remaster branch was reviewed file by file and reconciled with
the newer main line rather than merged blindly. All
five substantive deliverables landed:

- All four sprite consumers now use one exact, overflow-checked serialized
  layout decoder bounded by the source asset size. Sprite 162's sparkle layout
  is 27 → 29 commands and sprite 177's is 66 → 67, with no guessed slack.
- Shade and fog interpolation now match the RDP's screen-linear behavior while
  position, UVs, and the near-clip intersection remain perspective-correct.
  The clipped fog endpoint is still rederived from its new `z/w`.
- Remastered SDF text is driven by runtime-registered glyph rectangles, not a
  whole-atlas heuristic. Each glyph is reconstructed in isolation and sampled
  point/clamp at LOD 0; registration lifecycle changes invalidate the derived
  cache. Pure and Restored stay pixel-identical to their legacy paths.
- Texture uploads expose complete mip-chain telemetry and the moving-camera
  proof measures a roughly 10% reduction in temporal shimmer on both renderers.
- The RL-1 vertex-colour experiment remains an explicit diagnostic, with a
  regression gate proving that DKR's baked colour must remain the ambient base.
  Production defaults retain that base and Chromium asserts zero experimental
  overrides.

Main's stricter upload-size guard, transactional sprite-builder rollback, and
the existing banana/fog evidence were retained during conflict resolution.
The imported documentation was corrected anywhere it contradicted the measured
implementation.

Fresh integrated evidence: Debug, Release, and ASan builds each pass all 12
CTest units. `check_sprite_layout`, `check_rdp_interpolation`,
`check_font_sdf`, `check_mip_motion`, `check_rl1_vertex_colour_ab`, and the
updated texture-lineswap gate pass; the SDF path uploads only actual text on GL
and WebGPU. A 3,300-frame ASan banana/sparkle route and filename-entry route are
clean. Linked wasm passes the ROM/derived-artifact guard, and real Chromium runs
3,600 frames at median 60 fps with 52 SDF uploads, 896 mip chains / 5,083
levels, AudioWorklet output, exact ROM/EEPROM reload, and no network upload.

### 1. An oracle route that drives a real lap — **FIRST LANE DONE; PARITY OPEN**
All three ROM-fidelity divergences were corrected (boot RNG seed, `gArcTanTable`
rounding, `gSineTable` trig) and **the oracle moved by nothing** — six routes, every
delta inside ±0.0004. The reason is the route set: frontend screens plus a *stationary*
kart-select shot. Nothing drives an AI racing line long enough for an LSB arctan
difference or a different RNG sequence to become pixels. So the suite currently cannot
see the payoff of the very fixes it was blocking. Also: `race_karts` scores **0.636**,
thirty points below the menus, by far our worst route and the least investigated.
`race_state_oracle` now drives both the US 1.1 ROM and the native port through a
full Ancient Lake lap and compares intermediate racer state on the race's own
clock. Its strict production arms remain intentionally red: the authored
two-field arm currently measures 39.241% checkpoint/lap agreement and
2,103.419 world-unit position p95; enhanced one-field simulation measures
2.988% and 6,230.618. A local-only `reference_replay` arm now compiles the
real-ROM VI trace into exact observed update widths and input states. That arm
makes the first four checkpoint clocks exact and moves the first five-unit
position separation from clock 18 to 767, proving that timestep partitioning
caused the early mismatch. Sub-unit floating-point drift still compounds into
a different open-loop line, so the diagnostic remains red rather than
manufacturing parity with a permissive tolerance.

The later all-racer Bluey 2 lane closes the cadence choice. Retail and Original
two-field Bluey finish at ticks 3,459 and 3,458 with a 1.00065× mean-speed
ratio; the Enhanced one-field arm finishes at 3,022 with a 1.13982× ratio.
Interactive gameplay now defaults to persisted Original cadence, with the old
one-field simulation retained explicitly as Enhanced compatibility. The
progression/audio regression and release-bound evidence are recorded in
[`BLUEY2_PARITY.md`](BLUEY2_PARITY.md). F-18 remains partial for broader
challenge, multiplayer, progression/save, audio, renderer-state, and
standard-race parity beyond this boss lane.

The target architecture keeps authoritative gameplay on its original cadence
and permits higher-rate presentation only after state-hash and render-purity
gates prove that presentation cannot change simulation. Current implementation
and qualification status are documented in [`../ROADMAP.md`](../ROADMAP.md),
[`open-items/README.md`](open-items/README.md), and the presentation tests.

### 2. Adventure Two — **DONE**
`get_filtered_cheats()` (`menu.c`) forces `CHEAT_MIRRORED_TRACKS` on for every race when
`gIsInAdventureTwo`, and the flag is threaded through `camera.c` (viewport width
negated), `racer.c:4393`, `audio_spatial.c` (stereo field) and five sites in
`game_ui.c`. State lives in the save as `CUTSCENE_ADVENTURE_TWO`. At the cited
checkpoint nothing had run it. `check_adventure_two.py` now closes that gap
through the canonical save block and all 20 mirrored racing lines, with live
world/camera, stereo, minimap, steering, viewport, EEPROM, and pixel witnesses.

### 3. Close the remaining browser-runtime coverage hole — **DONE**

The native half is now closed: `check_renderer_backends.py` compares GL with
WebGPU in a real race and fault-injects WebGPU window startup. That injection
must stop cleanly without changing the cached backend or entering diagnostic
GL; it confirmed and fixed the stale cached-backend defect described here.

`check_browser_runtime.py` now runs the shipped shell and freshly linked wasm in
an isolated real Chromium profile. It selects the external ROM through the actual
file input, drives 3,600 rAF frames into Ancient Lake, requires five live scenes,
measures ~60 Hz cadence and race progress, performs three CSS/DPR resizes, consumes
AudioWorklet PCM, and proves exact ROM/EEPROM restoration and both recovery controls.
Its CDP/server audit permits only local bodyless GET/HEAD requests, so the
bring-your-own-ROM privacy claim is executable. This gate also reproduced the
live-sink SFX event-queue margin defect and now enforces more than 2× headroom.

### 4. Retail-ROM scope — **CLOSED for the current product**
All five revisions boot, navigate and finish a 3-lap TT once pointed at their own asset
LUT. Three are refused for one honest reason: `asset_table_load()` bounds-checks the
*section* index and nothing bounds-checks an index *within* a section, and US 1.1's
`GAME_TEXT` has 343 entries where US 1.0's has 259. A bounds check on the sub-entry
accessors turns a silent OOB read into a loud abort — cheaper than auditing every call
site. Japan additionally needs `REGION_JP` and is best done as a separate binary with
`-DVERSION_jpn_v79`, sidestepping all 423 compile-time gates. See `docs/ROM_REVISIONS.md`.
The current product deliberately supports US 1.1 and European 1.1 only; the
revision gate owns that exact set and named refusal of the other three. The work
above is therefore a future compatibility expansion, not an open foundation
requirement.

### 5. A navigation primitive that can reach an arbitrary point — **DONE for the first-boss route**
The cited checkpoint measured the Adventure-path first boss chamber at a closest
approach of 1210 units. Waypoint chaining plus a narrow checkpoint-36 summit
steering recovery now drives the legal fourth-race → boss-door → Tricky 1 route.
`check_first_boss_progression.py` proves physical win/loss finishes, result
cutscenes, hub return, and exact save reload. The old stock-AI line still misses
the narrow summit finish and is deliberately retained as the check's positive
control, not as an open coverage gap.

### 6. Normalise the `--build` convention — **DONE**
All behavioural scripts now accept a build directory or executable through one
shared resolver. `tools/run_checks.py` owns the special Release, ASan, UBSan, and
wasm/browser shapes plus the ROM-free CTests, runs save-mutating checks sequentially,
and rejects any unregistered `tests/check_*.py`. The manifest covered 31 scripts
and 38 tasks at the cited checkpoint, then 32 scripts and 39 tasks after the
runtime-boundary gate. The current manifest contains **122 scripts and
135 tasks**. `RELEASE_CHECKLIST.md` has one command per native configuration
and routes the wasm artifact through the same runner.

### Smaller, and each has its evidence in `docs/OPEN_ITEMS.md`
- Adventure Two and the legal first Adventure boss progression are closed.
  Tricky 1's old autopilot line still misses the summit after nine legitimate
  collision hits perturb it by 2.5 units; retain it as a route-fidelity oracle
  question and positive control, not as a reason to widen gameplay budgets.
- The user-facing RAW16 bass defect is closed at `a651cdc`. Three
  `alRaw16Pull` sites convert serialized big-endian PCM; the fourth ADPCM load
  remains a byte stream. The gate inventories 25 music and one SFX RAW16 wave
  and runs exact fixed/legacy output in both directions.
- The browser event-queue issue is closed: live Chromium measurement found SFX
  peaks up to 195, the port budget is 512, and the release gate requires every
  queue to remain below half capacity with zero dropped posts.
- The bare-pointer caller arrays are fixed: the four ordinary `collision_get_y()`
  callers share `COLLISION_Y_QUERY_CAPACITY` (16, `game/src/tracks.h:206`) and the
  wave builder keeps its measured 30. The bounded-write high-water counters in
  `platform/stubs_dkr.c` remain the class instrument.
- `--expect-fail 9` was stale and its "known-bad" examples are removed; the generic
  `--expect-fail` diagnostic option remains, and release validation must not use it
  (`docs/RELEASE_CHECKLIST.md`).

---

## 6.9 Optimized broad-UB checkpoint

The old audit's roughly 32 live broad-UBSan locations no longer describe
current `main`. Fresh measurement began with one reachable menu colour pack;
the strict content census then exposed the deeper route-specific set across
water physics, animator/snow setup, boost and door packing, challenge angles,
music/background rendering, and vehicle audio.

All locally reachable sites are now defined without changing intended game
bits. The important negative control was the repair itself: growing
`VehicleSoundData` and converting `U8_ANGLE_TO_U16` as an assumed 8-bit scale
were sanitizer-clean but broke the multiplayer oracle. Exact committed HEAD passed on an isolated branch. The final code keeps the native allocation
unchanged (248 bytes on LP64, original 0xE0 on wasm32), reuses the original
unused 0x98..0x9F region for P3/P4 Doppler history, and preserves the historical
10-bit angle scale with defined multiplication. Both pointer-width sizes are
compile-time asserted.

`check_full_ubsan.py` is registered in the manifest. It builds optimized with
the full `undefined` group, verifies handlers and aborting positive controls,
uses no production allow-list, and covers the 46-route census, every one of the
47 legal track/vehicle combinations, 3P/4P through results, challenge results,
filename entry, and a 7,500-frame race. Debug 24/24 CTests, Release, ASan,
audio, and linked wasm validation pass. The audit still labels F-01 Partial
because the written exit gate additionally names a separate `-O1` and external
toolchain/platform tuple.

The Adventure Two visual oracle was subsequently re-baselined (`862106a`
reworked the capture window to frames 2125–2250 with explicit mode/cadence
arms) and re-verified on 2026-07-29 at post-v0.4 HEAD: track 5 measures
reflected MAD 0.149 against the unflipped control's 64.602, decisively passing
the gate (reflection ≤ 6.0, unflipped ≥ 10.0, ratio > 5×). The historical 31.862
failure described a stale capture window, not a campaign progression failure.

## 7. Feeding mgb64 back

`docs/MGB64_BACKFLOW.md` holds findings for the GoldenEye port, marked CONFIRMED vs
SUSPECTED with file:line. Highlights:
- **CONFIRMED**: mgb64 has the same audio event-queue **spin hazard** and is *more* exposed
  (its `default: break;` makes the wedge guaranteed) — `audio_compat.c:3963`, `snd.c:1569`.
  A guard must return **nonzero** or the spin relocates into the driver loop.
- **CONFIRMED NOT present but load-bearing**: mgb64 already casts the reverb delay taps
  (`audio_compat.c:1780/1794/1853/1854`). Those casts look cosmetic; "cleaning them up"
  would reintroduce an 8-GiB-offset memory corruption.
- **SUSPECTED**: delay-line tail overrun (no slack while the mixer moves 8-byte-rounded
  counts) — `audio_compat.c:2000`.
- Generalised LP64 shapes (§3 above), the sign-extended wild-pointer hazard in the shared
  `gfx_ptr` code, and backend-decoupling notes.

---

## 8. Working practices

### The one rule that earned its place

`CONTRIBUTING.md` rule 6 — **fix the instance, then sweep the class** — was promoted
from habit to policy because it paid every single time it was applied, and it always
found more than the report that triggered it:

| one report | class swept | what fell out |
|---|---|---|
| racers fell through one volcano | ~90 C bodies transcribing hand-written asm | `vec3f_rotate_py` with pitch and yaw transposed, reached hundreds of times per race |
| one browser crash in `waves.c` | 1001 adjacent global pairs, both targets (**43 % sit at different distances**) | a five-row write into a four-row array that let the game load an **arbitrary level** — wrong on native too |
| one scrambled texture | every `LOADBLOCK` with `dxt == 0` | ~30 % of all texture uploads, including the minimaps |
| "the key animation replays" | every variable shift count | a second, unreported instance in the boss-approach cutscene, and the true cause of the Taj bug |
| "audio works" (a *claim*, not a bug) | every claim in `README`/`CHANGELOG` | one outright false row, four with no check behind them |

The sweep must be **mechanical** — a sanitizer, a differential against ground truth, a
link-map comparison, a count over a data table. Reading only finds what you already
suspect. When the instrument does not exist, building it *is* part of the fix: that is
where `tests/check_array_bounds_sweep.py`, `tools/compare_data_layout.py` and
`tools/sweep_bug_shapes.py` came from, and all three now catch new instances
automatically.

### Mistakes already made here, so they are not repeated

- **The Taj replay was misdiagnosed first.** "Two competing IDBFS syncs" was wrong; the
  real cause was `-O2` deleting the flag write. The probe that exonerated the shift had
  been run at `-O0`.
- **The track-select defect was proposed as the cause of the Taj bug and was not.**
  Disproved two ways, including by construction: there is no Taj level id at all.
- **Two oracle figures were quoted into the README from stale records** and had to be
  re-measured; one was wrong at both ends and omitted the in-race route entirely.
- **`--build` cost two false FAILs** in the maintainer's own reporting, an hour apart,
  after being documented as a hazard. Documentation was not enough; see §6.6.
- **The first 38-task native-layout run lost the useful evidence for a transient
  signal-6 exit.** `check_vehicle_sweep.py` captured the child log but printed it
  only with `-v`, and its bad-line detector did not include UBSan. The exact arm
  subsequently passed 20/20, the strict layout rerun passed, and a second standard
  manifest passed 38/38; `7a7f2f7` now always preserves failure tails. If it
  recurs, diagnose the printed site — do not infer a game defect or dismiss it as
  infrastructure from the exit code alone.

The pattern in the first four: a plausible reading, stated before it was
measured. The fifth is the complementary failure: the measurement ran, but the
harness discarded the evidence needed to interpret it. This codebase punishes
both, because so many of its defects are silent.


| Want to… | Read |
|---|---|
| Understand the architecture | `ARCHITECTURE_DECISIONS.md` (decisions 1–9, milestones) |
| See what's broken / deferred | `docs/OPEN_ITEMS.md` |
| Sync new decomp progress | `docs/DECOMP_SYNC.md` + `tools/sync_decomp.sh` |
| Compare against the real ROM | `docs/ORACLE.md` + `tools/run_oracle.sh` |
| Know which ROM revisions work | `docs/ROM_REVISIONS.md` (us.v80 + pal.v80 supported; per-revision taxonomy) |
| Run/verify the game | `tests/README.md` (fixtures, `MDKR_FORCE_BOOST`, trace env vars) |
| Build the browser version | `docs/architecture/web.md` + `README.md` |
| Send findings upstream to GE | `docs/MGB64_BACKFLOW.md` |

**Play it:** build normally and run `./build/mdkr64 --rom /path/to/your.z64`. A
build with audio is the only way to hear regressions; every automated check runs
muted by the hard rule in §4, so nothing in the suite can catch a silent mixer.

**A thirteenth bug shape, added by the "gridmask" wave: `game/`'s `NON_MATCHING` C
bodies are not verified against the assembly they replace.** Upstream's matching
build takes `GLOBAL_ASM(...)` for every handwritten-assembly function, so its
`#ifdef NON_MATCHING` C is documentation that nothing executes. We compile it
(`NON_MATCHING=1`), which makes us the first to run it — and
`compute_grid_overlap_mask()` had a transcribed condition comparing against the
wrong operand, and it is **still present in upstream `3b2dd520`** (the commit this
tree is synced to). It was silent for the
usual reason: the function is a *pre-filter*, so getting it wrong still produced
correct collisions, just too many candidates — until the 500-entry list saturated
and silently threw the ground away. **Before trusting any function under
`game/src/hasm/`, diff the C against the `.s` next to it.**

**That diff has now been done** (wave "hasmaudit", `docs/OPEN_ITEMS.md`). It was
not a one-off: **two more transcription defects** came out of the same class.
`vec3f_rotate_py()` paired each angle with itself instead of pitch with yaw, so a
horizontal direction vector came out vertical (measured: the ROM's `(z,0,0)`
became `(0,−z,0)`) — particles, lens flare, spotlights, sprite placement; and
`generate_collision_candidates()` *skipped* untextured terrain batches where the
ROM collides with them at `SURFACE_DEFAULT`. Both fixed, both with checks.
The audit also covers the **data** side of those `.s` files. Three ROM-fidelity
divergences there — boot RNG seed, arctangent-table rounding, and sine-table
generation — are now fixed and gated. The pixel oracle moved by no measurable
amount because none of its six routes drives a racing line; that missing
instrument remains §6.1. The lesson still holds: a wrong seed is perfectly
deterministic, so `check_determinism.py` alone can never identify it.

A fourteenth shape, from the same wave: **an unpaired function is a finding.**
`func_80017A18` (object-model collision) formerly had its body behind `#ifdef
NON_EQUIVALENT`, so this build linked a weak unconditional-miss provider. Upstream
later matched the authoritative body; it is now integrated with two `DKR_PTR`
adaptations and a locked-door A/B. The old "8 calls" measurement described only
boss track 38; the relevant zero-balloon hub route made 948 calls. Always inspect
the linked provider and measure the user route before describing reachability.

Useful trace env vars: `MDKR_TRACE=1` (menu ids, pacing, racer probe, decoded MISC
tables), `=2` + `MDKR_DL_FRAME=N` (display-list opcode trace, plus per-frame
`nearclip=/dropped=/degen=` counters), `=3` (input reads); `MDKR_DUMP_EVERY=N`,
`MDKR_FORCE_BOOST=frame:len`, `MDKR_AUDIO_DUMP=out.wav MDKR_AUDIO_RMS=1`,
`MDKR_SUPPRESS_PORTRAITS=1` (a `check_challenge_modes.py` test hook, not a
player setting: it skips the `BHV_CHARACTER_FLAG` draw in `render_3d_misc()`
under `NATIVE_PORT` only, so the paired run witnesses the pixels that draw
paints; any value other than empty or `0` suppresses),
`MDKR_RENDERER=gl`,
`MDKR_CAMERA_OBSTRUCTION=observe|modern|center-ray|legacy` (the obstruction
resolver arm; unset is `observe`, the authored camera, and anything unrecognised
falls back to it — a typo may neither silently correct nor select a
known-unsafe arm, and prints a one-shot `camera_obstruction:` line to stderr
naming the value it could not parse. The launcher's `Camera.Obstruction` setting
exports the same variable at engine handoff, and only when nothing already set
it, so this override outranks the launcher — and, because it does, an in-game
change to the setting is refused as `LOCKED` whenever this variable is set,
leaving the diagnostic arm in charge for the whole run) with `MDKR_CAMERA_TRACE=1|2` (`1` prints the per-tick
`camera_obstruction_observe summary` line; `2` adds a per-viewport detail line
and, under `modern`, keeps running the stationary sweep it would otherwise skip)
and `MDKR_CAMERA_PERF=1` (`[CAMERAPERF]` per-section timings),
`MDKR_NEARCLIP=off|w|zw` (A/B the near-plane clip),
`MDKR_LINESWAP=off` (A/B the pre-swizzled-texture un-swizzle),
`MDKR_GRIDMASK=off` (A/B the collision grid-mask Z fix),
`MDKR_ROTPY=legacy` (A/B the `vec3f_rotate_py` pitch/yaw fix),
`MDKR_COLLTEX=legacy` / `MDKR_COLLTEX_FORCE=1` (A/B the untextured-collision fix;
`FORCE` makes the otherwise-unreached case occur),
`MDKR_RNGSEED=legacy` / `MDKR_ARCTAN=trunc` / `MDKR_TRIG=libm` (A/B the three
landed ROM-fidelity corrections),
`MDKR_OBJCOLL=legacy|trace` (reproduce the unconditional-miss behavior or print
every production object-model hit),
`MDKR_SEGMARGIN=<n>` / `MDKR_SEGBOUND=legacy` / `MDKR_COLLCAP=<n>|legacy` (force or
remove the bounds added by wave "boundsweep" — the boundaries are unreached in play,
so these are how its controls get there),
`MDKR_BOSS_SLOW=1` (cripple a boss so the human wins — the only way to reach
`racer_boss_finish()`'s win branch headlessly),
`MDKR_WATCH_COURSEFLAGS=<levelId>` (one `[BOSSW]` line per change to
`courseFlagsPtr[levelId]`/`settings->bosses`, polled at the frame boundary so a write
through the *wrong* index shows up too),
`MDKR_BOSS_PRECLEARED=<levelId>` (hold a boss course at "already beaten" — the state
that makes a boss win present nothing; see `docs/OPEN_ITEMS.md` "wave bossverdict"),
`MDKR_WORLD_SHADOW=off|soft|full` (the `Video.WorldShadows` setting's env name;
`0`/`1` still resolve, so every pre-R2 A/B keeps working),
`MDKR_SHADOW_BIAS=<world units>` / `MDKR_SHADOW_UMBRA=<0..1>` (raw float overrides
for the two tuned shadow constants — the seam that ruled acne out of the R2
light-depth-sign investigation), `MDKR_TEST_SHADOW_BOGUS_CASTER=<world units>`
(displace every static caster's first admission, so the depth map holds geometry
the object has left — `check_shadow_plausibility.py`'s broken direction),
`MDKR_TAJ_SELECT_FAIL_SPAWNS=<n>` / `MDKR_TAJ_SELECT_FAIL_SIGN_SPAWNS=<n>`
(fail the first `n` character-select spawns, read once in
`taj_visual_select_begin()` and clamped to 0..1000; the first fails the composed
rider *and* its placard, the second fails only the numbered placard so a
composed rider survives into the UNAVAILABLE picker state —
`check_taj_visual_lifecycle.py`'s sign-only arm, which is the only one that can
see a lit unselectable Taj left standing in the line-up).

**A second, separate env-var family lives one layer down, in the vendored
platform code.** The trace vars above are `MDKR_*` and belong to game-level
code written for this project. `platform/fast3d/gfx_opengl.c`, `gfx_webgpu.c`,
`platform/mixer.c`, and their headers instead read `GE007_*` diagnostic
overrides — the prefix is a holdover from the author's GoldenEye port
**mgb64**, where this code originated (mgb64 is a GoldenEye 007 port; `GE007`
is that project's own internal naming, not something coined here). It is kept
as-is on purpose, not renamed to `MDKR_*`: these backend files are
shared, actively-converging code between the two projects (see "7. Feeding
mgb64 back" below and `docs/MGB64_BACKFLOW.md`), and renaming the prefix here
would just be cosmetic drift that makes future diffs against mgb64 harder to
read for no behavioral gain. See NOTICE.md
("Vendored platform code shared with mgb64") for the one-paragraph version of
this same rationale.

Do not hand-maintain an inventory of the individual `GE007_*` names here — it
would silently go stale. To enumerate the current set, or to check what
governs a given rendering/audio knob while debugging:

```sh
grep -rn 'GE007_' platform/ | sed -E 's/.*(GE007_[A-Z0-9_]+).*/\1/' | sort -u
```

Each one is read via `getenv`/`port_env_*` at the site that uses it; grep for
the exact name to find its call site and default.

### Diagnostic switches

Found by an env-var audit: each has a live C-side reader and no reference
anywhere in `tests/`, `tools/`, `docs/`, `.github/`, or a `CMakeLists.txt`/
`cmake/` `ENVIRONMENT` line (ctest wiring counts as a reference — several
`MDKR_APP_SMOKE_*` vars that looked orphaned at first turned out to be set
that way and are not listed here). They still work; nothing in the suite
currently exercises them, so treat their behavior as believed-correct until
you've run them once.

- `MDKR_APP_FILEDIALOG_SELFTEST=1` — exercise the native file picker through
  the same live-window activation state the launcher uses, then print the
  selection and ROM verdict and exit.
- `MDKR_APP_OVERLAY_TEST=1` — force the pause overlay open the instant
  `Overlay_install()` runs, for a one-shot headless render proof without
  scripting input.
- `MDKR_APP_UI_INPUT_TRACE=1` — trace the Frame Limit combo's
  hover/focus/activation state to stderr as `[app-ui-test]` rows, frame by
  frame.
- `MDKR_AUDIO_FILTER_TRACE_JSONL=<path>` (+
  `MDKR_AUDIO_FILTER_TRACE_WAVE_BASE=<address>`) — write one JSON row per
  native ADPCM filter decode to `<path>`; the wave-base var narrows the trace
  to one `ALWaveTable`.
- `MDKR_DL_LENIENT=1` — legacy override: forces display-list fault recovery
  (non-strict) even when `MDKR_DL_STRICT=1` is also set.
- `MDKR_HASH_DUMP_TICK=N` (+ `MDKR_HASH_DUMP_UNTIL=M`,
  `MDKR_HASH_DUMP_IDS=1`) — dump one `[HASHOBJ]` row per object at tick `N`
  (or the closed range `[N,M]`); `IDS` adds a companion `[HASHOBJID]` row
  naming each object (behaviour, host address, racer slot) so a divergent row
  can be tied to a concrete actor.
- `MDKR_MAX_HFOV=<deg>|off` — clamp the maximum horizontal FOV (60-175°) the
  aspect/widescreen math can produce; `off` removes the clamp.
- `MDKR_MUSIC_MIDI_TRACE_JSONL=<path>` — write the native MIDI sequence
  player's channel-service trace to `<path>` as JSON lines.
- `MDKR_MUSIC_SOLO_PROGRAMS=<list>` / `MDKR_MUSIC_MUTE_PROGRAMS=<list>` — play
  only, or mute, the listed MIDI program numbers (comma/space-separated).
- `MDKR_OBJMAP_PROBE=1` — log one row per byte-swapped level-object body
  field, naming the raw (pre-swap) value against the value now consumed; the
  seam that found the wrong entries in the endian-swap table.
- `MDKR_RENDER_CENSUS=1` — hash authoritative state immediately before/after
  `render_scene()` and report ticks where rendering mutated it; one summary
  row every 600 ticks and at each change.
- `MDKR_ROM_ALLOW_MODIFIED=1` — accept a supported-revision ROM whose
  SHA-256 doesn't match the reference image (a hand-patched or damaged
  dump); independent of `MDKR_ROM_ANY_REVISION`, which instead accepts an
  unsupported revision.
- `MDKR_TEST_OVERLAY_OPEN_FRAME=N` (+ `MDKR_TEST_OVERLAY_CLOSE_FRAME=M`) —
  schedule the overlay open/closed directly at authoritative tick `N`/`M` via
  `setOpen()`, bypassing the Escape-key event injection
  `MDKR_TEST_OVERLAY_ESCAPE_OPEN_FRAME` uses. `CLOSE_FRAME` alone is
  exercised by `check_overlay_pause.py`; `OPEN_FRAME` is not currently
  exercised by anything but shares its scheduling code, so it is believed
  live rather than confirmed.
- `MDKR_TEST_SETTINGS_TOGGLE=Key=value@tick[,Key=value@tick]...` — change a
  setting mid-run exactly as the in-game overlay would, at the first present
  opportunity at or after `tick`. It fires from `platform_input_pump()`, which
  runs on every present INCLUDING the presentation subloop's, so an edit lands
  at the same hostile moment a real one does rather than at a boundary chosen to
  be convenient. Each entry fires once; a soak is a long list. Every edit prints
  a `[SETTINGS-TOGGLE]` row carrying `applied=0|1`, so a key pinned by the
  environment or the command line is visibly refused rather than silently doing
  nothing. Drives `check_live_toggle_settings.py`; inert unless set.
- `MDKR_WAVES_TRACE=1` — per-render `[WAVES]` row: authoritative
  phase/magnitude plus how much swell actually reached the surface; the seam
  that found the wave generators were inert on levels 19 and 34 (see
  `objects.c`'s `mdkr_objmap_swap_entry_body` note).
