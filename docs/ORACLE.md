# mdkr64 visual and state oracle (ares vs native)

A confirmation / parity harness. It runs the **real DKR ROM** inside a locally
patched [ares](https://ares-emu.net/) N64 emulator, captures reference frames
along a scripted input route, runs the **same logical route** on our native port
(`mdkr64`), and pixel-compares the two so we can find fidelity gaps and prove
correctness.

Everything the oracle produces is **ROM-derived and local-only**. The patched
ares checkout, its build, and all captured frames live under
`build/ares-oracle/` (git-ignored). Do not commit any of it.

Modeled on mgb64's ROM oracle (`tools/prepare_ares_movement_oracle_build.sh`,
`rom_oracle_route.py`, `movement_oracle_capture.sh`) but deliberately focused.
The visual lane has no RDP/game-memory trace: input and frame dumps are keyed on
the emulator's presented-frame counter. The US 1.1 state-only lane adds one
compact, version-locked racer record from emulated RDRAM.
`tools/compare_oracle_state.py` enforces the independent-runner contract, while
`tests/check_bluey2_rematch.py` is the progression-valid native regression for
the passing boss lane.

---

## Pieces

| File | Role |
|------|------|
| `tools/prepare_ares_oracle.sh` | clone + patch + build a local instrumented ares |
| `tools/dkr_oracle_route.py`    | compile ONE route JSON → native input-script + ares injection route + capture marks |
| `tools/oracle_reference_replay.py` | compile a real-ROM state trace into a fail-closed native update/input replay |
| `tools/oracle_routes/*.json`   | route specs (schema `mdkr64.oracle.route.v1`) |
| `tools/run_oracle.sh`          | run a route on BOTH runners, then compare |
| `tools/compare_frames.py`      | align frames at marks, score similarity, emit report + montage |
| `tools/compare_oracle_state.py` | join native and original racer state on the race clock |
| `tools/oracle_screens.py`      | list the stable "screen segments" in a capture — the calibration measurement |

Route JSON event numbers name the intended authored native present/tick phase.
The native fixed-ticket boundary publishes a script entry at N to the simulation
sample traced at N+1, so `dkr_oracle_route.py native-script` emits every positive
event one ticket early. The ares script remains at the authored event number.
This explicit conversion keeps previously calibrated native/ares routes aligned;
removing it changes the exact `race_state_oracle` stream from 27,840 to 27,832
rows and is rejected rather than rebaselined.

The one-ticket lead clamps at zero, and that clamp is the only place the mapping
is not injective: an authored frame 0 and an authored frame 1 both emit frame 0,
and `script_apply()` then merges their buttons on one tick instead of pressing
them in sequence. No shipped route authors an event that early — every one
starts well after the boot logos — so give any sub-two-frame event its own
representable tick before authoring it.

---

## Three things that made this harness lie (read before trusting a score)

Every one of these presented as a *rendering* fidelity gap and was not.

1. **`Audio/Driver=None` does not exist** in the pinned ares (only `Device`,
   `Frequency`, `Latency`, `Blocking`, `Dynamic`, `Mute`, `Volume`, `Balance` are
   bound; `audio.driver` defaults to `"SDL"`). An unknown `--setting` key makes
   ares print `Invalid setting` and **return before loading the ROM**, so runs
   produced **zero frames**. Silence now comes from `SDL_AUDIODRIVER=dummy` (SDL
   opens no device at all), with `Audio/Mute` + `Volume=0` as a second layer.
2. **Input was injected into all four controller ports.** `Gamepad::read()` runs
   for every connected controller and the hook was port-blind, so four players
   *joined* at PLAYER SELECT. That changes the menu graph — `charselect_confirm`
   only takes the CAUTION branch when `gNumberOfActivePlayers == 1` — so the real
   ROM skipped CAUTION and ran ahead into track select while our port showed
   CAUTION, scoring ~50% for two correctly-rendered but *different screens*. The
   hook is now port-1 only.
3. **A single global sync delta cannot align a multi-tap route.** Each menu
   transition takes longer on real hardware: a level load there is a genuine
   multi-frame stall (DMA + decompression) that ares presents frames through,
   while this port loads from host memory almost instantly. Measured on
   `title_to_options`: our black transition spans ~20 frames, the real ROM's ~40.
   The error **accumulates** per tap (observed best-match offsets grew +93 → +113).

**So: never read a single similarity number in isolation.** Read the aligned
score, the offset it needed, and the montage.

---

## 1. Build the instrumented ares (once)

```
tools/prepare_ares_oracle.sh          # clone + patch + build (Release)
tools/prepare_ares_oracle.sh --force  # nuke + rebuild the local checkout
tools/prepare_ares_oracle.sh --no-build   # clone + patch only
```

- Clones ares at the pinned commit `91b112279…` into `build/ares-oracle/ares`
  (git-ignored). Override the source with `ARES_REPO_URL=<url-or-local-path>`.
- The patch is tiny and confined to five ares files + two macOS cmake fixes:
  - `n64/controller/gamepad/gamepad.cpp` — controller read → `mdkr64OracleControllerRead`
  - `ares/node/video/screen.cpp` — each presented frame → `mdkr64OraclePresentedVideoDump`
  - `n64/vi/vi.cpp` — appends the oracle translation unit (input, frame dump,
    state trace, audio dump)
  - `n64/cpu/cpu.cpp` — two per-instruction hooks at `CPU::instruction()`:
    `mdkr64OracleMaybeForceTrack` and `mdkr64OracleTraceVehicleRng`
  - `n64/n64.hpp` — declares the two `vi.cpp` hooks
- Binary lands at
  `build/ares-oracle/ares/build-oracle/desktop-ui/ares.app/Contents/MacOS/ares`.
- `MDKR64_ARES_ORACLE_DIR` (or `--work-dir`) moves that whole workspace —
  `prepare_ares_oracle.sh` clones and builds under it, and `run_oracle.sh`
  writes every route's captures under it. **It does not move the default ares
  binary path.** `run_oracle.sh`'s `ARES_BIN_DEFAULT` is the literal
  repo-relative `build/ares-oracle/...` path above and is not derived from the
  workspace, so a build placed in an overridden directory (a scratchpad, say)
  fails with `ares binary not executable` until you pass `--ares-bin` or set
  `ARES_BIN`. `prepare_ares_oracle.sh` prints the exact `--ares-bin` invocation
  when it finishes.

The ares hooks read these env vars (set for you by `run_oracle.sh`):

| Env | Meaning |
|-----|---------|
| `MDKR64_ARES_INPUT_SCRIPT` | injection route file (`start len buttonsHex stickX stickY` per line) |
| `MDKR64_ARES_DUMP_DIR`     | write `frame_<N>.ppm` (P6) here |
| `MDKR64_ARES_DUMP_EVERY`   | dump every N presented frames (filmstrip) |
| `MDKR64_ARES_DUMP_START`   | first frame eligible to dump |
| `MDKR64_ARES_DUMP_MARKS`   | comma-separated exact frames to always dump |
| `MDKR64_ARES_EXIT_AFTER_FRAMES` | self-exit (`_Exit(0)`) after N presented frames |
| `MDKR64_ARES_STATE_TRACE` | write the local-only US 1.1 numeric state CSV |
| `MDKR64_ARES_AUDIO_DUMP` | write the ROM's own audio-interface PCM (raw LE s16 stereo) |
| `MDKR64_ARES_VEHICLE_RNG_TRACE` | write the local-only US 1.1 retail car-audio RNG call witness |
| `MDKR64_ARES_FORCE_TRACK` | `source:target[:N]` level ids; rewrite the one-player `level_load` argument so a route can reach a track the menus cannot script into (`bluey2_state_oracle` sets `5:52`). With `N`, only the Nth matching load is rewritten (route key `forced_track_occurrence`), so a route can race the source track first. `level_load` has the same address in US 1.1 and PAL 1.1 |
| `MDKR64_ARES_RDRAM_DUMP` | `FRAME[,FRAME...]:PREFIX`; write all 8 MiB of RDRAM to `PREFIX_<frame>.bin` at each listed presented frame, through the cache-coherent CPU debug path (see the RDRAM snapshot lane below) |
| `MDKR64_ARES_POKE_RACER_POS` | `start:end:x:y:z[:trophyWorld]` — hold the human racer's object position for a presented-frame window (issue #55 exit-latch lane) |
| `MDKR64_ARES_MENU_RESULT_TRACE` | write the local-only US 1.1 per-return `menu_loop` value witness (issue #55 uninitialized-return lane) |

### The audio lane

`MDKR64_ARES_AUDIO_DUMP` taps `AI::sample()`, where `data` is the word the ROM
itself DMA'd to the audio interface. Taking it **there** and not at
`stream->frame()` is the whole point: it is upstream of ares' float conversion,
upstream of the analogue-hold decay ares applies on the idle branch, and upstream
of every host resampler and output driver. What lands in the file is the real
ROM's synthesiser output at full s16 scale, and it is deterministic for a given
input route.

Only DMA-active samples are written — the idle decay is an output-stage artefact,
not something the ROM produced, and including it would bias the level measurement
the lane exists for. The rate is whatever the ROM programmed into `AI_DACRATE`
(DKR asks for 22050 Hz; the VI divider makes it 22047), so it is written to a
`<path>.rate` sidecar rather than assumed by the reader.

Consumed by `tests/check_audio_level_reference.py --reference`, which re-runs the
port on the same oracle route and reports the port/console RMS ratio whole and
per band. Refreshed after the vehicle-audio correction on
`race_state_oracle`: **+0.016 dB** over a 153.16 s aligned
overlap. Like every other oracle output the capture is ROM-derived and
**local-only** — never commit it.

The repeatable capture command is:

```bash
tools/run_oracle.sh race_state_oracle --skip-native \
  --audio-dump /tmp/mdkr64-console.raw
```

The runner keeps the host silent, validates the raw stereo frame boundary and
rate sidecar, and prints the capture's duration and SHA-256 before it can be
used as the `--reference` input. Like `--vehicle-rng-trace`, the option is
accepted only on `race_state_oracle`: the lane *replaces* that route's state
scoring rather than adding to it, so asking for it on any other route is a
usage error instead of a silently unscored comparison.

### The vehicle-audio RNG authority lane

`MDKR64_ARES_VEHICLE_RNG_TRACE` is a CPU-level US 1.1 witness. It records only
entries to the retail `rand_range` address whose return address lies inside
`racer_sound_car`, then reads the shared seed and current racer through the
emulated CPU's debug/cache path. This distinguishes an authored shared-RNG call
from a port-side cosmetic choice without trusting downstream particles,
triangles, or race outcomes.

`tools/run_oracle.sh race_state_oracle --vehicle-rng-trace PATH` runs this lane
and validates the local CSV with `tools/validate_ares_vehicle_rng.py`. The
validator pins the route's exact call-site census and SHA-256 and includes a
direct RNG-transition control. The trace contains runtime-derived data and stays
local; only its schema and digest belong in the repository.

Restoring those retail calls also advances the shared seed before downstream
weather work. The weather-enabled Original-cadence oracle was therefore
deliberately refreshed to 813 trace rows and SHA-256
`fe180923803148e9ea3e5be6f913e9daa6831c4f5d9f12fe345f7d0bf3fea5a9`.
That refresh is accepted only with byte-identical state/weather streams under
skipped rendering and 30/60 Hz presentation, plus the existing wrong-order and
one-byte negative controls; it is not a relaxed threshold.

Button masks are the standard N64 SI layout (A=0x8000 … C-Right=0x0001),
identical to the native port's masks, so an injected value lands verbatim.

### The RDRAM snapshot lane

`MDKR64_ARES_RDRAM_DUMP` writes the console's whole RDRAM at chosen presented
frames, so memory a draw sampled can be read offline. It answers questions no
frame capture can, such as which bytes a texture load fetched past the end of
its texture. Run it through the runner on a route that declares
`"rdram_capture": true`:

```bash
mkdir -p /tmp/mdkr64-rdram   # the lane writes files; it does not create directories
tools/run_oracle.sh spaceport_pulse_ring --skip-native \
  --rom baserom.us.v80.z64 --rdram-dump 3800:/tmp/mdkr64-rdram/us_tracks
```

The runner refuses `--rdram-dump` on any other route, and an rdram route
without it. It requires the run to complete, each `PREFIX_<frame>.bin` to be
exactly 8 MiB and reported written, and a declared forced load to have
happened. With `forced_track_occurrence` N above 1, any route also needs the
ares log to show every earlier load left as is (`match k of N`): an ares built
before the occurrence field forces the first race instead, and
`tools/prepare_ares_oracle.sh` re-applies its `cpu.cpp` hooks on every refresh
so an in-place rebuild picks the field up. Snapshots are ROM-derived and local-only: keep them out of the
repository and out of artifacts.

**Spaceport Alpha's pulsing floor ring (2026-10-01).** Three routes reach the
race, each with `forced_track` 15:

- `spaceport_pulse_ring`: the Tracks race of Ancient Lake, retargeted.
- `spaceport_pulse_ring_tt`: the same with Time Trial on.
- `spaceport_pulse_ring_second_race`: Ancient Lake is raced first, quit
  through PAUSE -> SELECT TRACK, and selected again; only that second load is
  retargeted (`forced_track_occurrence` 2).

Five snapshots were taken: US 1.1 on all three routes, and PAL 1.1 (Europe
Rev 1) on the first and third. All five show the same layout after texture
211's 1024 texel bytes:

| bytes after the ring's texels | contents | the same in all five snapshots |
|---|---|---|
| 0-55 | the ring's own Gfx load list | yes, except its texel address (US `0x002d1390`, PAL `0x002dc820`) |
| 56-127 | 72 bytes left over from loading | yes |
| 128-159 | texture 880's header (32x64 RGBA16, 4128 bytes) | yes, except its command-list pointer |
| 160-4255 | texture 880's texels (CRC-32 `f5df41bf`) | yes |

Retail loads two 2048-byte images from the ring's texel base, so the console
draws the ring from bytes 0-4095 of that memory. The native renderer reproduces
it from an owned 4096-byte image: the ring's texels, a 160-byte zero fill, then
texture 880's first 2912 texel bytes. Both textures are found in the texture
cache by asset ID and checked by shape and content CRC (`pulse_material`
covers the checks and their failures). Against every snapshot, over the ring's
UV area, the second image matches the console texel for texel and the first
matches it on 95.8% of the area. The 4.2% that differs is the fill band, where
the console's address-dependent command words, load leftovers and texture
880's 32-byte header are not reproduced. That band is visible as a narrow
strip at the edge of the dive shaft for about three frames per lap. The
wrapped plan of every other smaller texture (`mdkr_texture_pulse_upload`)
matched the console on 51.2% of the first image and none of the second.

### The track-exit poke and menu-result lanes (issue #55)

Hot Top Volcano's destination −1 `BHV_EXIT` sits ~114 units below the
drivable surface, so no input route can trip `obj_loop_exit`'s latch on the
real ROM — the only human route is an out-of-bounds clip. Two paired lanes
make the authored aftermath measurable anyway:

- **`MDKR64_ARES_POKE_RACER_POS="start:end:x:y:z[:trophyWorld]"`** holds the
  human racer's object position at `(x, y, z)` for the presented-frame window
  and zeroes its velocity, so the retail latch, `racer_enter_door`, the fade,
  and the destination routing all run authored code. Writes go through
  `CPU::writeDebug` — cache-aware — because writing backing RDRAM can be
  undone by a later dirty D-cache writeback (the same hazard the vehicle-RNG
  lane documents on its read side). The optional `trophyWorld` seeds
  `gTrophyRaceWorldId` (and `gTrophyRaceRound=2`) once at window start so the
  status heartbeat (printed to stderr every 30 presented frames from window
  start) can witness whether retail preserves them across the transition.
  Mind the half-plane: the latch requires `dot(dir, racer) + rotationDiff <
  0`, so take the exit's `dir`/`rotDiff` from a native `MDKR_OBJDUMP` row and
  pick the poke point on the negative side (for HTV's exit at
  (−1517, 158, −2354) with dir (0, 1), that means z < −2354).
- **`MDKR64_ARES_MENU_RESULT_TRACE=<path>`** records the exact value the
  retail US 1.1 `menu_loop` returns to `mode_menu`, per call, by watching the
  CPU step from inside `menu_loop`'s address range directly into
  `mode_menu`'s (symbol ranges from `docs/ref/symbols/symbol_addrs.us.v80.txt`;
  callees return into `menu_loop`, interrupts land in neither range, so that
  boundary is precisely the return, and v0 is the routed value). This is the
  only way to read the value for `MENU_UNUSED_8`, whose `ret` the C leaves
  uninitialized — authored UB with no source-level answer. Rows are kept for
  every menuId 8 return plus each menuId change for context.

Measured 2026-08-24 (route `race_state_oracle` inputs, `MDKR64_ARES_FORCE_TRACK=5:7`,
poke window 4300–4420 at (−1517, 158, −2400)): the menu 8 return is
`v0 = 0x8006CA60`, `mode_menu` consumes it through the `MENU_RESULT_FLAGS_200`
arm (`& 0x7F` → level 96), `level_load`'s own out-of-range guard substitutes
the central hub, and the seeded trophy globals survive to the end of the run.
`game/src/menu.c` pins that measured value at the `AVOID_UB` default for
`MENU_UNUSED_8`; `tests/check_track_exit_storage.py` holds the native port to
the same end-to-end outcome. Like every oracle output, the traces are
ROM-derived and local-only.

### ares must be allowed to run unfocused

ares defaults to **pausing the emulation core when its window loses focus**.
`run_oracle.sh` passes `--setting Input/Defocus=Allow` so the core keeps running
headlessly in the background. (Without it, a background launch silently produces
zero frames — the window never becomes key, so the core never un-pauses.)

---

## 2. Route spec (schema `mdkr64.oracle.route.v1`)

One JSON drives both runners. Example (`tools/oracle_routes/title_to_options.json`):

```json
{
  "schema": "mdkr64.oracle.route.v1",
  "name": "title_to_options",
  "sync": { "event": "title", "native_frame": 1134, "ares_frame": 1340 },
  "native": { "frames": 1520, "dump_every": 20, "dump_start": 1100 },
  "ares":   { "frames": 1720, "dump_every": 20, "dump_start": 1300 },
  "marks": [
    { "name": "title",     "logical": 0 },
    { "name": "menu_shown", "logical": 146 }
  ],
  "events": [
    { "frame": 1250, "buttons": ["start"], "hold": 4 },
    { "frame": 1310, "buttons": ["down"],  "hold": 4 }
  ]
}
```

- `events[].frame` is the **authoritative native present-frame** (so a route
  reproduces the proven `tests/input_scripts/` fixtures verbatim). Buttons:
  `a b z start l r up down left right cup cdown cleft cright` (directional
  buttons also drive the analog stick, matching the native token behavior).
- **`sync`** is the timing bridge. The two emulators reach a given screen at
  different frame numbers, so ares event/mark frames are derived from the native
  ones by `delta = sync.ares_frame - sync.native_frame`:
  `ares_frame = native_frame + delta`.
- **`marks`** are capture/compare points, given as `logical` offsets from the
  sync event and resolved per-runner.
- **`ares_extra`** (optional, on any event or mark) shifts *only* the ares side,
  on top of the sync delta. It exists because of limitation 3 above: transition
  lengths differ structurally between runners, so a multi-tap route needs
  per-event calibration to keep both runners on the same screen. Calibrate it by
  measurement, not by eye:
  ```
  tools/oracle_screens.py build/ares-oracle/<route>/ares/frames
  tools/oracle_screens.py build/ares-oracle/<route>/native/frames
  ```
  Each tool prints the frame at which each screen becomes visible; the difference
  between the two runners at a given screen is the `ares_extra` that screen's
  press needs. Keep the value in the route so the calibration stays reviewable
  data rather than a hidden constant.
- **Do not put a mark at `logical` 0** on a menu route: that lands on the
  level-load transition in *both* runners, giving two black frames that score
  hist=100% / block=0% for a meaningless 50%. `compare_frames.py` now detects
  near-constant frames, prints `SKIPPED (blank frame: …)`, and excludes them from
  the overall score.
- A state-only route sets `compare_frames: false` and `state_trace: true`.
  `native_synth_fields` selects its deterministic field step, while
  `native_event_divisor` scales the native presentation-frame script to that
  cadence. `ares_phase_offsets` entries require a written `basis`; they capture
  measured phase changes without hiding them in code. `forced_track` (with
  `forced_track_source`, default level id 5) is what `run_oracle.sh` turns into
  `MDKR64_ARES_FORCE_TRACK` for a track the menus cannot script into.

Inspect a route:
```
python3 tools/dkr_oracle_route.py list
python3 tools/dkr_oracle_route.py native-script title_to_options
python3 tools/dkr_oracle_route.py ares-script   title_to_options
python3 tools/dkr_oracle_route.py mark-pairs     title_to_options
python3 tools/dkr_oracle_route.py validate       title_to_options
```

---

## 3. Run a route

```
tools/run_oracle.sh boot_to_title
tools/run_oracle.sh title_to_options --ares-timeout 280
tools/run_oracle.sh race_state_oracle  # reference + default Original arm
tools/run_oracle.sh race_state_oracle --native-arm enhanced --skip-ares
# Second stage: reuse the freshly captured real-ROM trace and replay its exact
# observed update widths/input states in the native diagnostic lane.
tools/run_oracle.sh race_state_oracle --native-arm reference_replay --skip-ares
tools/run_oracle.sh boot_to_title --ares-bin /path/to/ares   # explicit binary
```

Outputs land under `build/ares-oracle/<route>/`:
```
native/frames/   native PPMs (pruned to marks + a coarse cadence)
ares/frames/     ares PPMs (real ROM reference)
compare/report_<route>.json
compare/montage_<route>.png
```

For `race_state_oracle`, no PPMs are created. The additional outputs are:

```text
ares_state.csv
native_original.log
native_enhanced.log
native_reference_replay.log
compare/state_report_race_state_oracle_original.json
compare/state_report_race_state_oracle_enhanced.json
compare/state_report_race_state_oracle_reference_replay.json
native_saves/   # isolated from the player's normal save directory
ares_saves/
```

The Ancient Lake state arms are classified `"state_classification":
"diagnostic"` (2026-08-25): `compare_oracle_state.py` records threshold
shortfalls as observations, headlines the divergence-onset clock, and exits 0,
while instrument-integrity failures (vacuous rng, missing probes, short
traces) still fail closed. The thresholds themselves are unchanged — they
define what an observation is, so a before/after pair of reports remains the
differential evidence for any gameplay-math change. The Original and Enhanced
arms complete a lap, but the strict contract exposes a
cadence-sensitive open-loop racing line. `reference_replay` is a diagnostic,
not a product mode: `MDKR_ORACLE_UPDATE_FIELDS` exists only as an environment-
gated test seam and its schedule must be consumed completely. The replay moves
the first five-unit position separation from race clock 18 to 767, and matches
the ROM's checkpoint clocks exactly through checkpoints 0–3 (3, 349, 463,
707), classifying the early difference as timestep partitioning. Sub-unit
floating-point drift still grows into a different open-loop line — the
expected behavior of any non-bit-exact re-implementation replayed with no
corrective input, which is why the open-loop agreement percentage is
deprecated as a figure of merit: it is a chaos-sensitive phase metric, and
the divergence-onset clock is the signal. Bluey 2 remains the
passing gameplay-cadence oracle; see [`BLUEY2_PARITY.md`](BLUEY2_PARITY.md).

ares samples RDRAM on VI observations while the CPU can begin the next update
under an unchanged framebuffer. State normalization therefore retains the
first observation per framebuffer serial, then the last stable observation per
race clock. Retaining the last observation per serial admits in-flight hybrid
state (the Ancient Lake control caught a transient 0.05-unit Y write).

The native port dumps **every** frame (1280×960 HiDPI), so `run_oracle.sh`
prunes to just the mark frames (±3) + `native.dump_every` cadence after the run
— an unpruned route is multiple GB. Use `--keep-all-native` to disable pruning.

---

## 4. Read the report

`compare_frames.py` downscales both runners to a common 256×192 luma raster and
scores each mark with two deliberately coarse metrics so minor filtering /
sub-pixel differences don't dominate:

- **hist** — 32-bin global luma histogram intersection
- **block** — normalized correlation of a 16×12 grid of block-mean luma
  (structural: "same stuff in the same place")

`similarity = 0.5·hist + 0.5·block`. The **montage** PNG has one row per mark:
`native | ares (real ROM) | abs luma diff`, labeled with the score and the
actual frames used. Read it directly to confirm the scene matches.

Each mark reports **two** scores:

- **exact** — the derived ares frame. Sensitive to transition-length drift.
- **aligned** — the best-matching ares frame within `--align-window` (default
  ±120), with the **offset** it needed. Judge fidelity on this one, and treat a
  large offset as its own finding (drift, or a mis-calibrated `ares_extra`).

The aligned score is the more flattering of the two *by construction*, so never
quote it without the offset. A high aligned score with a large offset means "the
right screen, at the wrong time"; a low aligned score is a genuine render gap.

---

## Sync anchors and the boot-path divergence

The real ROM and the native port do **not** boot identically:

- Both render the identical static **"DIDDY KONG RACING" title logo** early
  (native ~f40–140, ares ~f80–220). This is the cleanest sync/compare anchor —
  `boot_to_title` uses it and scores **~99.8%** similarity.
- After the logo the paths diverge: the real ROM plays the full attract intro
  (N64 logo + Diddy flyby → Rareware logo → island intro flyby), while the
  native port blanks the N64-logo intro and shows only the Rare copyright text,
  reaching the interactive island/title menu at ~f1134 while ares is still
  mid-intro at ~f1340.

Because of that divergence, `boot_to_title` (logo sync) is the solid,
pixel-aligned validation. The menu-navigation routes (`title_to_options`,
`title_to_character_select`, `race_entry`) drive the same logical inputs into
both runners and capture what renders at each mark, but their island-title sync
(native 1134 / ares 1340) is **best-effort**: the real ROM's longer attract
intro means multi-tap frame-precise menu alignment is not guaranteed in v1.
Refining it (e.g. an intro-skip START injection to reach the ares menu
deterministically) is future work.

---

## Limitations (v1)

- **Best-effort menu alignment** across runners (see above). `boot_to_title` is
  the validated, pixel-aligned case.
- **The island/3D scenes are slow in ares on first run** — parallel-RDP
  (MoltenVK) JIT-compiles shaders (`[WARN] Stalled compile`), so reaching the
  island can take a while cold. The per-route ares workspace persists its
  settings/saves to warm the cache; give `--ares-timeout` headroom.
- **macOS-focused.** The prepare script's cmake fixes target macOS/CLT builds
  (mirrors mgb64). Other platforms will need the equivalent driver flags.
- The state reader is deliberately US 1.1-specific. Its symbol addresses and
  32-bit structure offsets must be re-derived before another ROM revision can
  claim state-oracle coverage.
- The current state lane covers one standard race. Challenge, boss,
  multiplayer, progression/save, and renderer-state contracts remain.
- The audio lane measures **level**, not identity. Envelope alignment over a
  multi-tap menu route is coarse (measured correlation +0.76 on
  `race_state_oracle`), so per-band ratios carry alignment noise and a
  sample-accurate PCM comparison is not available from it.

## Hygiene

### Boss audio comparisons

`boss_stomp_oracle` and `wizpig_stomp_oracle` are explicitly audio-only routes.
They retarget the Tracks-menu race load from Ancient Lake to Tricky 1 (38) or
Wizpig 1 (37). They do not provide a gameplay-state or screenshot score.
Capture with `tools/run_oracle.sh ROUTE --skip-native --audio-dump NEW_PATH`
and the usual `--rom` / `--ares-bin` arguments. Both the raw capture path and
its `.rate` sidecar must be new. Confirm the matching forced-load marker in
the retained ares log before using the capture as a boss reference.
The runner also requires successful completion at the requested frame limit
and an exact match between logged audio sample-frame count and raw PCM length;
a truncated or enlarged capture cannot pass on a completion message alone.

Compare using `tests/check_audio_level_reference.py --build BUILD --rom ROM
--reference RAW_PATH --reference-route ROUTE --reference-window 65:140`.
The native arm verifies the requested boss race loaded; the window reports
aligned RMS and exact full-scale sample occupancy. Those are aggregate
measurements, not proof of individual stomp balance or subjective audibility.
Keep the raw audio, WAV files, and emulator artifacts private. ROM-free schema,
runner refusal and native route-identity controls live in
`tests/test_oracle_audio_routes.py` (CTest `oracle_audio_routes`).

### Private artifacts

- The ares checkout/build and all captures are **local-only and git-ignored**
  (`build/ares-oracle/`, `*.ppm`). Never commit ROM-derived frames, saves, or
  the ares source.
- ares is pinned to a known-good commit and cloned into ignored space; the
  source is patched in place, never vendored.
