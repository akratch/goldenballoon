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
- The patch is tiny and confined to four ares files + two macOS cmake fixes:
  - `n64/controller/gamepad/gamepad.cpp` — controller read → `mdkr64OracleControllerRead`
  - `ares/node/video/screen.cpp` — each presented frame → `mdkr64OraclePresentedVideoDump`
  - `n64/vi/vi.cpp` — appends the oracle translation unit (input + dump logic)
  - `n64/n64.hpp` — declares the two hooks
- Binary lands at
  `build/ares-oracle/ares/build-oracle/desktop-ui/ares.app/Contents/MacOS/ares`.

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
per band. Measured on `race_state_oracle`: **-0.488 dB** over a 153.21 s aligned
overlap. Like every other oracle output the capture is ROM-derived and
**local-only** — never commit it.

Button masks are the standard N64 SI layout (A=0x8000 … C-Right=0x0001),
identical to the native port's masks, so an injected value lands verbatim.

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
  measured phase changes without hiding them in code.

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

The Ancient Lake state arms are currently expected to return nonzero. The
Original and Enhanced arms complete a lap, but the strict contract exposes a
cadence-sensitive open-loop racing line. `reference_replay` is a diagnostic,
not a product mode: `MDKR_ORACLE_UPDATE_FIELDS` exists only as an environment-
gated test seam and its schedule must be consumed completely. The replay moves
the first five-unit position separation from race clock 18 to 767, and matches
the ROM's checkpoint clocks exactly through checkpoints 0–3 (3, 349, 463,
707), classifying the early difference as timestep partitioning. Sub-unit
floating-point drift still grows into a different open-loop line; that strict
arm remains red rather than hiding it behind a tolerance. Bluey 2 remains the
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

- The ares checkout/build and all captures are **local-only and git-ignored**
  (`build/ares-oracle/`, `*.ppm`). Never commit ROM-derived frames, saves, or
  the ares source.
- ares is pinned to a known-good commit and cloned into ignored space; the
  source is patched in place, never vendored.
