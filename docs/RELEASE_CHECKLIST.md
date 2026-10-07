# Release checklist

Run this before every public release. It is ordered so the cheapest gate that can
stop a release runs first. Two of these steps are the ones that keep the project in
the clear legally — they are scripts, not judgement calls, and they fail closed.

Nothing here is optional, and nothing here should be reasoned around. If a gate
fails, the release stops.

## 0. Prerequisites

You supply your own legally-owned ROM. It is never committed.

Set `MDKR_DEDICATED_TEST_DESKTOP=1` when running the complete suite. The runner
requires this explicit test selector; `--list` only prints its inventory. Use
muted or hidden execution where the test supports it. Foreground presentation
measurements in §3a require a visible window.

When running from the private assembly checkout, refresh the separate public
source branch first:

```bash
git fetch public main
```

`tools/ci/check_release_ready.sh` audits `public/main`'s complete reachable
content history in that checkout, while continuing to audit the exact candidate
tree at `HEAD`. In a normal clone of the public source repository, both checks
use `HEAD`. New outgoing commits are additionally checked one by one by the
pre-push and hosted CI gates, including content committed and later deleted.

```bash
ln -s /path/to/your/baserom.us.v80.z64 baserom.us.v80.z64
```

> **Audio safety — hard rule.** Every gameplay-capable engine invocation below
> passes `--headless-frames N`, which returns before the audio device is ever
> opened, and sets `MDKR_AUDIO=0`. The controlled native `audio_sink_evidence` gate
> is the sink exception: it explicitly uses `MDKR_TEST_HEADLESS_AUDIO=1` with
> SDL's dummy driver, proving queue acceptance rather than physical output. The only
> other permitted exceptions are the proven
> early-exit process surfaces `--help`, `--version`, and `--video-list`; all three
> return before ROM, window, and audio initialization. Do not generalize that
> exception to another option. Note that `MDKR_AUDIO=off` is a **no-op** — the
> check is `disable[0] == '0'`, so only the digit `0` disables. See
> [`../CONTRIBUTING.md`](../CONTRIBUTING.md).

## 1. Clean-room verification

The claim in [`../DISCLAIMER.md`](../DISCLAIMER.md) and [`../NOTICE.md`](../NOTICE.md)
is that no ROM or bulk ROM-derived assets are in this repository or its history.
A ROM committed and then deleted in a later commit is still in every clone
forever, so a working-tree check alone does not prove this. The release-readiness
guard separately pins the narrow deleted-screenshot exception recorded in
`NOTICE.md` to an exact historical path list and rejects any new media path.

```bash
tools/check_clean_room.sh
```

Expected: `check_clean_room: PASS`. It checks, and fails closed on:

| # | Check |
|---|---|
| 1 | No `.z64`/`.n64`/`.v64` is tracked |
| 2 | No ROM-extension path was ever added in any commit on any ref |
| 3 | No blob anywhere in history carries an N64 ROM header at offset 0, in any of the three byte orders |
| 4 | No blob in history is implausibly large for source (4 MiB backstop for ROM-derived bulk under an innocent name) |
| 5 | No emulator source is vendored — the visual oracle patches ares out-of-tree, under git-ignored paths |
| 6 | `.gitignore` still covers ROMs in all three byte orders, saves, and captures |

Step 4 reports rather than fails: brand art and generated lookup tables legitimately
exceed the threshold. **Confirm each reported blob is first-party or documented in
`NOTICE.md`** — do not wave it through.

Also confirm by eye that no oracle capture escaped: captures are `*.ppm` under
`build/ares-oracle/`, and `MDKR_AUDIO_DUMP` writes a RIFF/WAVE file of synthesised
game audio, which is ROM-derived output.

```bash
git status --porcelain --ignored | grep -iE '\.(ppm|wav|raw|z64|n64|v64)$' || echo "no stray captures"
```

## 2. Build clean — **both configurations**

Use a clean candidate checkout and fresh build directories for qualification;
do not overwrite artifacts or build trees belonging to an active run. The
native profile below enables the shipping online beta and KTX2 decoder, while
registering the GPU tests needed later in this checklist. The bare development
preview stays disabled; the beta composes its required panel internally.
The empty cloud origin matches the current deliberately partyless candidate.
If a cloud-enabled candidate is approved, use its exact recorded origin in all
three native builds and their packaged artifacts. Keep the web's local-only
profile separate. On macOS, first select the pinned shipping SDL2 prefix via
`PKG_CONFIG_PATH`, as described in section 2b.

```bash
release_native_args=(
  -DBUILD_TESTING=ON
  -DMDKR_ENABLE_GPU_TESTS=ON
  -DMDKR_ENABLE_ONLINE_BETA=ON
  -DMDKR_ENABLE_ONLINE_ROOM_PREVIEW=OFF
  -DMDKR_CHARACTER_KTX2=ON
  -DMDKR_PARTY_ORIGIN=
)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug "${release_native_args[@]}" &&
  nice -n 15 cmake --build build --parallel 2
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release "${release_native_args[@]}" &&
  nice -n 15 cmake --build build-rel --parallel 2
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug "${release_native_args[@]}" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" &&
  nice -n 15 cmake --build build-asan --parallel 2
```

Expected: `[100%] Built target mdkr64`, no new warnings, from all three.
Check the generated compile and link commands: both C and C++ must carry
ASan/UBSan in the sanitizer build, including the custom-character decoder.
Compilation alone does not qualify any behavioral or sanitizer gate.

The web link additionally treats every wasm-ld warning as fatal, and all
Clang/Emscripten translation units reject implicit function declarations. One
toolchain diagnostic remains expected when `--emit-symbol-map` asks Binaryen
`wasm-opt --print-function-map` to print rather than write a file:
`warning: no output file specified, not emitting output`. It originates in the
required same-link crash-symbol-map step, not in project code; any other web
warning fails this gate.

**Why both, every time.** `CMakeLists.txt` defaults native to **Debug** and the web
build to **Release**. So the configuration players actually run in the browser is
the one the native suite never exercised — and that is not hypothetical: two
player-reported bugs (the world-key cutscene replaying after every race, and Taj's
OFFERED flag never being written) were *created by the optimiser* and were invisible at
`-O0`. A progress flag written as `FLAG << (i + 31)` is undefined in C, always folds to
zero at `-O2`, and clang deletes the load, the test and the store together, so both
halves of a "show this once" latch vanish at once. Every native check stayed green
throughout.

A probe run at `-O0` says nothing about a defect whose mechanism *is* optimisation.
When a report comes from the browser and reproduces nowhere, build Release natively
**before** assuming the difference is wasm.

## 2b. Run the complete native suite against the optimised build

Every behavioural check now accepts the same `--build` contract: either a build
directory or its `mdkr64` executable. The runner knows which checks require the
selected, Release, ASan, self-built UBSan, or wasm artifact; it also fails if a new
`tests/check_*.py` is absent from its manifest, or if a `tests/test_*.py` has no
CMake `add_test()` to carry it into the ctest task.

Build the web artifact first (section 4's `tools/web/build_web.sh`), then run the
suite **once, with no subsetting flags**. Every
`--only`/`--role`/`--primary-only`/`--skip-*`
restriction makes the runner label its verdict `SUBSET n/N`; the release run must
print `complete suite, N/N tasks`, which is the only form that counts as a full
run.

```bash
tools/web/build_web.sh          # builds, stages dist/web, runs the guard itself
python3 tools/run_checks.py --jobs 6 --require-shipping-sdl --require-fresh \
  --build build-rel --release-build build-rel --asan-build build-asan \
  --wasm build-web/mdkr64_web.wasm --roms /path/to/rom-revisions \
  --reference-build /path/to/previous-release/build-rel
```

`--reference-build` is a build of the previous published release: a clean
`git worktree add --detach <dir> v1.7.0`, configured with the same `MDKR_*`
options and SDL as `build-rel` and built with `--target mdkr64`.
`previous_release_parity` requires both builds to produce identical `[SIMHASH]`
streams on the retail AI, boss and challenge-arena routes (US and PAL), and has
the old build write a Time Trial record and ghost that this build must read
back. Without the flag that task is SKIPPED, and because this command passes
`--require-fresh`/`--require-shipping-sdl` the runner then fails the run and
names `--reference-build`. A run without those flags may end `PASS WITH SKIPS`,
labelled `INCOMPLETE` rather than `complete suite`; neither is release evidence.
A diverging route is a finding: bisect it over `v1.7.0..HEAD`, then document
the change as intended or fix it.

`--roms` must point at a directory holding BOTH source revisions, US and PAL
v80. Its default, `build/roms`, is normally empty, and the runner's help calls
the flag optional because most single-revision gates are happy without it -- but
a release run is not most gates. Omitting it fails twelve tasks outright, the
first being `simulation_cadence`, which refuses with "no supported PAL v80 ROM
found ... the release cadence gate requires both source clocks". Those twelve
failures look like a broken candidate and are a missing flag.

`--require-fresh` refuses an artifact older than a source file it is built from.
Without it the runner prints `STALE ARTIFACT ... Rebuild before trusting this
run` and then runs anyway, which is not a warning a ten-hour release run can act
on: the 2026-09-08 qualification of `b6e8ff40` carried a `build-asan` binary two
days older than `platform/mod_texture_store.h`, so the fourteen tasks that take
the ASan role reported on a tree the candidate no longer was. A release run has
no reason to accept that, and the flag existed before this was written down.

`--require-shipping-sdl` makes preflight refuse a binary linked against
Homebrew's `sdl2-compat` shim. The releases bundle the pinned upstream SDL2
(`macos/Scripts/build_release_sdl2.sh`; configure with `PKG_CONFIG_PATH`
pointing at its `install/lib/pkgconfig`), and the two libraries differ in
unfocused-joystick delivery: the 1.6.0 qualification once went green under the
shim on a lane that is red under the shipping library. Every run also prints
`run_checks: sdl flavor: …` so a plain developer run still records which one it
measured.

`--jobs` pools the CPU-bound work — the `source` checks and the native/rom/
release/asan checks whose verdict is a deterministic function of the ROM and
inputs (audio, save, state-hash, input, rollback, numeric camera geometry,
progression). Three classes stay serial and run after the pool drains:
build-tree- or port-sharing roles (`ctest`, the instrumented/layout compiles,
the wasm module, and every browser lane — `SERIAL_ROLES`); the render/GPU
checks whose verdict reads rendered pixels or drives a GPU surface, which flake
under parallel GPU contention (`GPU_SERIAL_NAMES`); and the wall-clock/host-load
measurement gates (`SERIAL_NAMES`). Those three sets in the runner are the
authority. `validate_manifest()` fail-closes if a pooled engine check ever
grows a pixel/frame capture without being listed as serial, so a new render
gate cannot silently rejoin the pool. The pool is additionally capped to what
physical RAM can hold (~2 GiB/worker) so a high `--jobs` cannot OOM the host.
Each pooled task gets its own scratch save directory, so the historical
"several checks share `save/eeprom.bin`" constraint no longer forces the whole
manifest through one lane. A `--jobs` run is still the complete suite: the
verdict line stays
`complete suite, N/N tasks`. If any pooled task fails in a way that smells
like host contention rather than a code defect, re-run that task alone
(`--only <name>`) before treating it as a regression — the same discipline
the GPU-labelled ctests already require.

The complete suite’s `rom_free_units` task runs all registered CTests without a label exclusion;
GPU-labelled native-window tests are registered only when the build enables
`MDKR_ENABLE_GPU_TESTS`. Register and run that lane explicitly:

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release \
  -DMDKR_ENABLE_GPU_TESTS=ON
env MDKR64_HIDDEN=1 MDKR_AUDIO=0 \
  SDL_MAC_BACKGROUND_APP=1 SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1 \
  ctest --test-dir build-rel -L gpu --output-on-failure --no-tests=error -j1
```

`MDKR64_HIDDEN=1` and SDL’s `SDL_MAC_BACKGROUND_APP=1` and
`SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1` request unobtrusive windows where
supported. Local CI’s optional ordinary CTest lane excludes
`gpu|app_process|browser`; the complete suite runner does not.

`check_key_cutscene_once.py` is the one check that is **build-type-sensitive by
design**. The runner verifies that its dedicated binary has an optimized
`CMAKE_BUILD_TYPE`; it also verifies that the filename-entry sanitizer binary
actually imports ASan. The ROM-free CTest task includes the display/runtime/
layout/scheduler units, the deterministic audio queue controller, and SDL's
silent queue-mode sink contract. When explicitly selected, native launcher GPU
tests share one CTest resource lock. A release evidence record must state that
this lane ran. A run that excludes it does not establish GPU coverage.

The RAW16 gate repeats against primary, Release, and ASan artifacts. The
specialized native-layout gate verifies linked
ASan/alignment handlers and exact legacy controls, then runs the complete
menu/track/vehicle/Adventure/boss/2P/widescreen matrix under halt-on-error
alignment UBSan. The primary suite's seven-arm `check_widescreen_proportions.py`
pixel-measures SAFE_2D and world-space golden balloons at two deterministic
approach frames across 4:3, 16:9, 21:9, and changed FOV, with exact legacy
stretching as its failing control.

`check_native_ui_resolution.py` runs GL/WebGPU production and disabled-control
arms, requires the HUD/minimap-only pixel delta and measured edge gain, and
rejects any world-after-overlay draw or pass-start failure. The 2P/3P/4P gates
extend that ordering assertion to every viewport layout.

`check_door_glyphs.py` drives a fresh Adventure save into Dino Domain on GL and
WebGPU, proves the four 1/2/3/5 race doors share one cached model, and requires
the material-bound texture offset to remain per door while the camera moves.
Its shared-offset counterfactual must disagree, so the gate detects the 1.0.1
camera-dependent numeral regression rather than passing on route reachability.
In Original presentation mode it also rejects blank output, requires visibly
distinct 2/1/3 glyphs, and compares final door pixels from GL and WebGPU. This
catches per-texture sampler state being skipped when OpenGL changes texture
objects, which otherwise stamps repeated numerals across the wood. Synthetic
blank, common-glyph, and repeated-sampler controls must all fail the pixel gate.

`check_video_options.py` runs both native backends and requires every in-game
control, atomic fresh-process reload, override locking/no-bake behavior,
unwritable-storage rollback, and malformed-launcher no-rewrite behavior. The
real-browser gate repeats the menu mutation and IDBFS reload in wasm.
`check_audio_options_persistence.py` separately requires the original Audio
Options sliders to commit before exit, then injects an unwritable destination
and proves the visible retry/session-only decision path.

## 3. Behavioural regression suite

Run every primary behavioural check against Debug as the second configuration.
The specialized Release/ASan/UBSan arms already ran in section 2b.

```bash
python3 tools/run_checks.py \
  --build build --primary-only --skip-wasm
```

See [`../tests/README.md`](../tests/README.md) for every assertion, positive
control, and frame budget. The optimized and Debug invocations are both required:
the former catches optimizer-created defects, while the latter is the everyday
developer configuration and can expose different layout/timing failures.

Do not add `--expect-fail 9`: the historical level-9 failure no longer reproduces
and both math arms complete that track. Keeping the stale exception would allow a
new regression on exactly that track to pass the release gate.

## 3a. Realtime pacing measurement — prepared machine, human present

The suite runs `pacing_quality` with `--allow-no-baseline`: its automation
windows render occluded by doctrine, and an occluded (or session-less) window
cannot measure displayed intervals — macOS throttles its presents. The
realtime arms therefore downgrade to loud notes in-suite, and the measurement
this section owns has to happen once per release on a machine that can
actually present:

1. Log in, unlock, and stay at the machine for the run. The window comes to
   the front and takes the keyboard. Leave the desktop clear and keep hands
   off while it runs.
2. ```bash
   MDKR_TEST_VISIBLE_HEADLESS=1 MDKR_TEST_FOREGROUND=1 \
     python3 tests/check_pacing_quality.py \
     --build build-rel/mdkr64 --rom baserom.us.v80.z64
   ```
   `--build` is the built binary. `--rom` is the US v80 ROM file, not the
   folder that contains it. Do not pass `--allow-no-baseline`.
3. Require the full PASS — no downgraded "quality not measured" note.
   This is the only coverage the alpha-grid projection has anywhere. An
   unlocked never-block fails here. The suite's copy of this gate passes
   `--allow-no-baseline` and must not be used as this measurement.

   A 60 Hz panel has two phase slots against the 30 Hz tick (the midpoint
   and the endpoint). The strict arm forces the legacy quantizer, which
   returns the measured phase when rounding would cross the tick, so that
   arm's p95 may sit in the end-of-tick bin rather than on a grid point.
   The median must still be the midpoint. A 120 Hz four-slot grid still
   requires both percentiles on a grid point. Native WebGPU's disciplined
   shipping path uses the companion slot arm regardless of the interval
   classifier's fixed/variable state; the classifier controls the legacy
   path. The companion arm is the quality bar and does not get the
   end-of-tick exception. The command above is still a full PASS, and it
   still omits `--allow-no-baseline`.

## 3b. Independent real-ROM gameplay oracle

The in-process deterministic suite proves that presentation choices do not alter
the port's own state. This separate local-only gate compares authored gameplay
against the US 1.1 ROM running in the pinned, instrumented ares build:

```bash
tools/prepare_ares_oracle.sh
tools/run_oracle.sh bluey2_state_oracle \
  --native-bin build-rel/mdkr64 --native-arm original
```

Expected: `compare_oracle_state: PASS`. Both runners must finish, reach the same
lap and checkpoint, retain at least 95% checkpoint/lap agreement, and keep
position p95 within 200 world units. Do not use the Enhanced one-field arm as
the reference. Since the boss-cadence governor of 2026-08-09 (issue #26) it no
longer reproduces the historical boss-speed error on defaults —
`check_bluey2_rematch.py` now requires the Enhanced arm to finish inside the
Original band, and the pre-governor runaway is reproduced only under
`MDKR_BOSS_CADENCE_COMPAT=0`.

Run `python3 tests/check_bluey2_rematch.py --build build-rel --rom <owned ROM>`
as the progression-valid, audio-bearing standing gate. The measured release
closeout and timer-sampling explanation are in
[`BLUEY2_PARITY.md`](BLUEY2_PARITY.md).

The broader Ancient Lake `race_state_oracle` is a divergence-onset diagnostic
(`state_classification: diagnostic` since 2026-08-25) for longstanding
open-loop floating-point drift, as documented in
[`ORACLE.md`](ORACLE.md); it reports threshold observations and exits clean
rather than presenting as a failing parity gate, and it is not a substitute
for this passing cadence gate.
For a change which intentionally touches gameplay math or authority ordering,
record before/after Ancient Lake reports as differential evidence — its
unchanged strict thresholds define what an observation is, so do not loosen
them.

### Menu navigation fixtures

The nine `nav_*` fixtures, three repetitions each, on a **clean EEPROM** (a recorded
time changes the menu route):

```bash
release_smoke_save="$(mktemp -d)"
MDKR_AUDIO=0 MDKR_TRACE=1 MDKR_SAVE_DIR="$release_smoke_save" \
  ./build/mdkr64 --headless-frames 1700 \
  --input-script tests/input_scripts/nav_to_options.txt \
  --rom baserom.us.v80.z64 2>&1 | grep 'menu_init: menuId=12'
```

Per-fixture frame budgets and expected assertion lines are tabulated in
[`../tests/README.md`](../tests/README.md). A run fails on a non-zero exit, a
`[CRASH]` backtrace, a `[FATAL]` abort, or a missing assertion line.

> **Do not assert with `printf ... | grep -q` under `set -o pipefail`.** `grep -q`
> closes the pipe on its first match, the upstream `printf` takes SIGPIPE (141), and
> `pipefail` reports the *successful* match as a pipeline failure — inverting every
> assertion in the harness. Match with bash `case "$out" in *"$want"*)` instead.
> This cost real debugging time; it looks like a total regression.

## 4. Web build and the artifact ROM-absence gate

Section 2b's single full invocation already ran every web task. This section is
the artifact gate plus, for iteration only, a way to re-run just the web lane
without naming its members:

```bash
tools/check_no_rom.sh dist/web
# iteration only -- the release evidence is section 2b's complete-suite run
python3 tools/run_checks.py \
  --role wasm,browser,browser_save,browser_local \
  --wasm build-web/mdkr64_web.wasm \
  --rom baserom.us.v80.z64
```

Expected: `check_no_rom: PASS — N artifact(s) scanned, no ROM data present.` and
every selected runner task PASS. `--role` selects the web lane out of
`tools/run_checks.py`'s own manifest, so this list cannot drift from the tasks
that actually exist — the Taj character-select and persistence gates joined the
`browser` role and are picked up without editing this document. The runner
prints the count it selected and labels the run `SUBSET`.

`check_wave_visible_table.py` is the one check that can only be run on the **web**
artifact: it catches a linker layout split that the native target is immune to by
luck, and which crashed a player in the browser. Do not skip it because the native
suite is green — the native suite passes either way. See
[`../tests/README.md`](../tests/README.md#wave-visibility-table-layout--testscheckwavevisibletablepy-run-this-after-any-wavesc-or-link-flag-change).

`check_browser_runtime.py` then runs that artifact through the committed shell in
an isolated real Chromium profile. It must reach a race for 3,600 paced frames,
render five changing scenes, survive three live CSS/DPR resize transitions, feed
the AudioWorklet, report nonzero fixed-mode RAW16 loads in its first active
block, maintain measured event-queue headroom, restore the exact ROM and EEPROM
after reload, exercise both erase controls, and observe no request that could
upload or name the ROM. The manifest also passes `--camera-obstruction modern`,
so the first document must publish Modern camera telemetry on nearly every
opportunity with nonzero applied corrections and zero penetrated, invalid,
degraded, or capacity-failed results. This is the reproducible browser-runtime
evidence; the post-release check below remains a human packaging/hosting check.
It also requires exactly one authored NTSC realtime pace initialization, no
update or wall-field count below two, a 24–36 FPS median complete-loop cadence,
and post-startup cadence no worse than 40.0 ms p95 / 45.0 ms p99. These are
the default Original/authored-motion baseline. It also fails if an async
pipeline takes more than two
authored render frames. An incomplete pipeline must skip the host opportunity
until a new authored image is ready; the release path may not replay or swap the
last image as a duplicate. The raw maximum and its frame number remain visible
in the PASS line.

`check_browser_presentation_rates.py` independently exercises display, numeric
caps, irregular display schedules, and the browser's documented uncapped
fallback. It must preserve the same fixed-authority state, gameplay-event,
consumed-input, and PCM hashes across those host schedules. Interpolated arms
must perform real immutable replay, publish true forward task data, resolve
private-arena and copied-external dependencies, and account for every submitted
or nonblocking-held surface opportunity without a runtime GPU wait.

`check_browser_resource_plateau.py` separately performs four real wasm race
loads through production pause-menu restarts. It must prove stable warmed
game/audio ownership, exact voice/state conservation, coherent non-growing
WebGPU generations, and zero terminal host/frontend/backend/AudioWorklet
ownership.

`check_browser_save_ui.py` is the fast player-data custody gate. It deliberately
removes WebGPU, never selects a ROM, and proves that the save module remains
independent of the engine. It checks exact raw/container export, hostile and
oversized input, all injected IDBFS transaction failures, safe metadata preview,
corrupt-block recovery, block merge, one-field edit containment, keyboard/screen
reader semantics, complete wipe→real-file import→reload, and zero uploads. The
Pages workflow runs this gate again before an artifact can be deployed.

This is **the** legal gate on a shipped build. It is structural, not a string
search: header magic at offset 0 in all three N64 byte orders, plus the big-endian
magic sequence anywhere in each file (which catches a ROM baked into the wasm). The
engine legitimately contains ROM validation strings, so a name search would
false-positive; see the comments in `tools/check_no_rom.sh`.

The guard refuses to pass vacuously — an empty target directory is a failure, not a
pass. Confirm the file list it prints is what you expect to publish, and that
`mdkr64_web.js` / `mdkr64_web.wasm` are present in `dist/web` but **not** tracked in
git.

## 4b. Custom Character Workshop release evidence

Section 2b's unrestricted suite is also the automated release gate for the
Custom Character Workshop. Its log must contain passing roster, live identity,
collection-flag, raw-intake, history, draft-transfer, Portrait Studio,
test-evidence, and exact-renderer-preview tasks. A focused rerun is useful while
fixing a failure, but its `SUBSET` verdict never replaces the complete-suite
record.

Before publishing any build that exposes the Workshop:

- [ ] Run **Custom Character Workshop acceptance** in
      [`RELEASE_CANDIDATE_TEST_GUIDE.md`](RELEASE_CANDIDATE_TEST_GUIDE.md) on
      the exact packaged candidate, not a source-tree executable.
- [ ] Complete its mouse, keyboard, controller, touch, app-spoken-guidance, 200%
      scale, narrow-layout, reduced-motion, and colour-vision matrix. Do not
      record spoken guidance as screen-reader compatibility. Record
      `not available` honestly where a platform lacks a modality; do not turn
      an unobserved cell into a pass.
- [ ] Exercise clean install, offline relaunch, disable, rebuild/update,
      last-known-good recovery, portable export/review, and permanent removal.
      The user's original model and license must remain outside application
      custody and unchanged.
- [ ] Capture select plus car, hovercraft, and plane evidence for floor,
      facing, seat, contacts, pose transitions, occlusion, and the complete
      1P-4P cost matrix. Retain privacy-bounded profiles from representative
      low-, mid-, and high-tier physical WebGPU devices. Record every
      amber/error row; a local performance exception is evidence, not a
      manufactured pass.
- [ ] Verify both a native mixed-direction name and a missing-glyph fallback,
      including their keyboard/spoken-guidance descriptions and exact live
      roster pixels.
- [ ] Verify the package contains the HarfBuzz and SheenBidi notices alongside
      the importer, validator, BasisU, and meshoptimizer notices. Keep the
      character model, ROM, saves, screenshots, and device-profile exports out
      of the public release archive.
- [ ] At the candidate cut, review the exact pinned Basis Universal/KTX-Software
      commit and vendored `stb_image` commit against their upstream security
      advisories and the CVE database. Record the query date, immutable pins,
      findings and disposition in private release evidence; do not substitute
      an unpinned update or claim that an offline build performed this review.
- [ ] Run the ASan+UBSan custom-character MDKC/KTX2 fuzzer from
      `tests/README.md` against a copy of its deterministic corpus. Preserve and
      regress any crash before release; a time-bounded clean run supplements,
      but never replaces, the structural unit and package gates.
      **Local decoder qualification passes:** the alignment amendment passes
      its sanitizer regression and 4,004,077 fuzz executions over 601 seconds.
      Final-artifact qualification remains; prior unit passes alone do not
      clear this gate for another build.
- [ ] Validate the completed privacy-bounded receipt against the exact artifact
      and provenance bytes. A template is intentionally red until every
      required observation is replaced:

      ```bash
      python3 tools/check_character_release_evidence.py \
        character-acceptance.json --artifact-dir /path/to/candidate-artifacts
      ```

Record the candidate commit and artifact hash, package/source digest, platform,
GPU/driver, output/render size, input/accessibility modalities, completed
contexts, evidence-report hash, and first failed step. Screenshots and model
captures may contain user-licensed or ROM-derived pixels: retain them as private
acceptance evidence unless their redistribution rights were reviewed
separately. The receipt schema is
[`ref/mdkr-character-release-acceptance-v1.schema.json`](ref/mdkr-character-release-acceptance-v1.schema.json);
archive the validator's receipt SHA-256 with the release decision.

## 5. Desktop packaging and publication

Desktop workflow version inputs are filename components, so public releases use
bare semantic versions such as `1.7.0`, never `v1.7.0`. The `v` prefix belongs
only to the Git tag. For version 1.7.0, the portable workflow must produce:

- `Golden-Balloon-1.7.0-linux-x86_64.AppImage`
- `Golden-Balloon-1.7.0-linux-x86_64.tar.gz`

Each portable artifact must have adjacent `.sha256` and `.provenance.json`
sidecars. The checksum file contains exactly the artifact SHA-256 and basename,
so it remains usable after all three files are downloaded into one directory.

Automatic Windows publication is intentionally disabled for this release because
`windows-latest` does not guarantee a qualifying D3D12/Vulkan adapter or GL 3.3
context. Its headless `--help`/`--video-list` execution is not accepted as a
rendered launcher or gameplay gate. The workflow still builds, unit-tests,
import-checks, packages, extracts, and launches `GoldenBalloon.exe` from an
unrelated CWD.

The exact-manifest `Golden-Balloon-1.7.0-windows-x64.zip` may be attached only
after manual acceptance on Windows hardware proves the extracted package can:

1. open the real launcher through default WebGPU;
2. load the supported ROM and complete the release gameplay checklist;
3. receive controller input and produce audio;
4. save and reload settings plus EEPROM data; and
5. exit and relaunch cleanly.

Record the tester, Windows version, GPU, archive SHA-256, and outcome with the
release. Publish the exact checksum-verified archive and provenance sidecars:
its `.sha256` and `.provenance.json` files
that were accepted; never substitute or rebuild it afterward. Explicit GL is a
diagnostic follow-up, not a prerequisite for endorsing the WebGPU-default
Windows artifact. This is a manual native GPU acceptance boundary, not an
automated GPU-qualification claim.

Use `version=dev` only for disposable test artifacts, never for a public
release; `release_tag` must then be empty. A semantic-version build requires the
exact `v<version>` tag, and that tag must resolve to the workflow's source
commit. The tagged dispatch belongs to the exact assembly sequence below; do
not dispatch it from a moving branch or before the candidate tag exists. The
workflow must reject every other input shape, compile that exact
value into both validation binaries, and compare each binary's `--version`
output before packaging. The Windows zip's exact payload remains
the `GoldenBalloon/` directory containing `GoldenBalloon.exe`, `LICENSE`,
`README.md`, `RUN_ME.txt`, and `gamecontrollerdb.txt`; no DLL or unlisted entry
is permitted. The Linux
tarball's exact payload is `Golden-Balloon.AppDir` with the launcher, desktop
metadata, icon, license, README, controller database, internal `mdkr64`
executable, and exactly one SDL2 runtime. Each packager verifies the archive it
actually wrote.

The Linux job must use Xvfb plus Mesa's pinned lavapipe ICD/llvmpipe software
stack to render and content-validate both default-WebGPU and explicit-GL
launcher captures before packaging. It must then extract the tarball, resolve
its bundled SDL2, change to an unrelated CWD, launch through `AppRun`, and repeat
both capture/content gates. The ROM-free CTests and asset-free verifier must
pass in the same job before the Linux artifacts are uploaded. If any part of
that job fails or is unavailable, publish no Linux artifact and do not attach a
locally produced replacement under the canonical release filenames.

Before creating the release tag, put the exact behaviorally qualified candidate
on public `main`, check it out cleanly, and run the external repository gate:

```bash
tools/manual/check_github_launch_ready.sh --repo akratch/goldenballoon
```

Expected: `GitHub public readiness passed`. Do not tag or dispatch a release
while it reports `[FAIL]`. This is the authoritative gate for public repository
settings and surfaces that source CI cannot prove, including branch protection,
Actions SHA-pin enforcement and retention, security endpoints, stale public
commit references, workflow artifacts, protected release/Pages environments,
and release-asset sidecars. `macos-release` must require an environment reviewer
and permit only branch `main` plus tag `v*`; `github-pages` must permit only tag
`v*`. Review every
warning explicitly even though warnings alone do not fail the command.
Hosted workflow paths are compared in full. The exact GitHub-managed
`dynamic/dependabot/dependabot-updates` path is accepted only while the source
tree tracks `.github/dependabot.yml`; every other untracked or moved workflow
still fails closed.
GitHub's immutable closed-PR refs may sit outside rewritten `main` only when
their exact ref and full SHA appear in
`tools/public_retained_ref_allowlist.tsv`; every listed commit's complete
reachable history has passed the current public-surface policy. A new or moved
PR ref, any mutable branch/tag ref, and stale workflow runs still fail closed.
For canonical desktop artifacts, the gate uses GitHub's authoritative asset
SHA-256, downloads only the small adjacent sidecars, and rejects checksum byte
mismatches, dirty or wrong-commit provenance, wrong 1.7 Phone Party/signing
modes, and orphan sidecars. For every 1.7+ release it also requires both Linux
formats, Windows, at least one recognised macOS DMG, and one Phone Party mode
across every platform sidecar.
Sidecar filenames alone are not release evidence.

### macOS 1.7.0 — unsigned/ad-hoc release artifact

The public 1.7.0 macOS artifact intentionally skips Developer ID signing and
notarization. “Unsigned” in its filename means there is no trusted signing
identity: the app must still have a valid inside-out ad-hoc integrity seal. The
only expected first-launch interruption is macOS's unidentified-developer
warning; a “damaged” warning is always a release failure.

The exact public files are:

- `Golden-Balloon-1.7.0-macos-arm64-unsigned.dmg`
- `Golden-Balloon-1.7.0-macos-arm64-unsigned.dmg.sha256`
- `Golden-Balloon-1.7.0-macos-arm64-unsigned.dmg.provenance.json`

The provenance sidecar must name that exact DMG, the exact 40-character source
commit, version `1.7.0`, platform `macos`, the DMG SHA-256, and
`macos_signing: ad-hoc-unsigned`. It must also record `phone_party: partyless`
or `phone_party: cloud-enabled`, exactly matching the release workflow's gated
origin decision; an origin-less artifact must never claim cloud capability.

Before producing the candidate:

- [ ] The source tree and index are clean.
- [ ] `CMakeLists.txt`, `macos/Resources/Info.plist`, the app's `--version`
      output, and the release notes all agree on `1.7.0`. Both desktop producer
      workflows independently reject a release-version input that differs from
      the checked-out `CMakeLists.txt` authority.
- [ ] Linux, Windows, and macOS release workflows explicitly enable
      `MDKR_ENABLE_ONLINE_BETA`; the resulting launcher exposes **Online Room**,
      while the published browser payload remains local-only.
- [ ] The release commit is the intended `v1.7.0` tag commit. A test artifact
      may omit `release_tag`; an artifact may be published only with
      `release_tag=v1.7.0` resolving to the workflow's exact source commit.
- [ ] The pinned standalone SDL2 build is used for arm64/macOS 13. Homebrew
      `sdl2-compat`, SDL3, Homebrew load paths, mixed architectures, and a
      deployment target newer than 13.0 are release blockers.

Build and validate a non-publishing candidate through the protected workflow:

```bash
gh workflow run macos-release.yml --repo akratch/goldenballoon --ref main \
  -f version=1.7.0 \
  -f trusted_signing=false
```

Record the resulting run as `MACOS_TEST_RUN_ID`; require its source SHA to be
the same clean public-main candidate before accepting any result:

```bash
MACOS_TEST_RUN_ID="replace-with-exact-run-id"
gh run watch "$MACOS_TEST_RUN_ID" --repo akratch/goldenballoon --exit-status
test "$(gh run view "$MACOS_TEST_RUN_ID" --repo akratch/goldenballoon \
  --json headSha --jq .headSha)" = "$(git rev-parse HEAD)"
```

The package job must complete all of these checks before its artifact is
accepted:

- [ ] Build SHA-pinned standalone SDL2 2.32.10 for arm64/macOS 13.
- [ ] Build `Golden Balloon.app` with `--strict-deployment-target`, embed version
      `1.7.0` and the exact source commit, bundle SDL2, then seal nested code
      before the outer app.
- [ ] Run `verify_asset_free.sh`, `verify_gatekeeper_bundle.sh`, and
      `verify_unsigned_release.sh`. The last check must prove the ad-hoc seal,
      version/commit identity, bundled SDL2 license, WebGPU default launch,
      launcher pixel output, and no SDL3 or Homebrew runtime load.
- [ ] Create the exact `-unsigned.dmg`. `create_dmg.sh` must pass `hdiutil
      verify`, mount the finished image read-only, and revalidate the packaged
      app from that mount. Then `verify_unsigned_dmg.sh` must mount it read-only
      again and pass the full LaunchServices/WebGPU smoke against the packaged
      app itself.
- [ ] Stamp `ad-hoc-unsigned` provenance and emit the matching `.sha256`
      sidecar. Its record must use the DMG basename, not a build-directory
      prefix, so `shasum -a 256 -c FILE.dmg.sha256` works after both files are
      downloaded into the same directory.

For a local reconstruction of those same build and verification steps, use the
commands in [`../macos/README.md`](../macos/README.md). Do not replace its
pinned SDL2 prefix with a machine-local Homebrew package.

### Exact tag, draft release, and artifact assembly

Do not start this sequence until the entire behavioral matrix, the unsigned
macOS test artifact above, and the pre-tag public GitHub readiness gate have
passed against one clean commit. Keep the GitHub Release as a draft until every
required platform artifact below is present and verified. The established
release tags are signed annotated tags; preserve that boundary:

```bash
RELEASE_SHA="$(git rev-parse HEAD)"
test "$(git rev-parse public/main)" = "$RELEASE_SHA"
git tag -s v1.7.0 "$RELEASE_SHA" -m "Golden Balloon 1.7.0"
git tag -v v1.7.0
git push public refs/tags/v1.7.0
test "$(gh api repos/akratch/goldenballoon/commits/v1.7.0 --jq .sha)" = "$RELEASE_SHA"

gh release create v1.7.0 --repo akratch/goldenballoon \
  --verify-tag --draft \
  --title "Golden Balloon 1.7.0" \
  --notes-file RELEASE_NOTES.md
```

Dispatch the qualified portable workflow from that tag:

```bash
gh workflow run release.yml --repo akratch/goldenballoon --ref v1.7.0 \
  -f version=1.7.0 \
  -f release_tag=v1.7.0
```

Record the run ID shown by GitHub Actions as `PORTABLE_RUN_ID`; never use an
unqualified "latest artifact" download. Require the run to succeed at the
release commit, then download its Linux and Windows artifacts into separate
empty directories and re-run the shared provenance verifier. Each invocation
names every required primary artifact explicitly, so a valid sidecar for only
half of a promised platform set cannot make the download pass:

```bash
PORTABLE_RUN_ID="replace-with-exact-run-id"
PHONE_PARTY_MODE=partyless # or cloud-enabled: match the workflow's gated output
RELEASE_STAGING="$(mktemp -d "${TMPDIR:-/tmp}/golden-balloon-1.7.0.XXXXXX")"

gh run watch "$PORTABLE_RUN_ID" --repo akratch/goldenballoon --exit-status
test "$(gh run view "$PORTABLE_RUN_ID" --repo akratch/goldenballoon \
  --json headSha --jq .headSha)" = "$RELEASE_SHA"
gh run download "$PORTABLE_RUN_ID" --repo akratch/goldenballoon \
  --name Golden-Balloon-1.7.0-linux-x86_64 --dir "$RELEASE_STAGING/linux"
gh run download "$PORTABLE_RUN_ID" --repo akratch/goldenballoon \
  --name Golden-Balloon-1.7.0-windows-x64 --dir "$RELEASE_STAGING/windows"

tools/release/verify_provenance.sh \
  --dist "$RELEASE_STAGING/linux" --version 1.7.0 --commit "$RELEASE_SHA" \
  --require-asset Golden-Balloon-1.7.0-linux-x86_64.AppImage \
  --require-asset Golden-Balloon-1.7.0-linux-x86_64.tar.gz \
  --require platform=linux --require "phone_party=$PHONE_PARTY_MODE"
tools/release/verify_provenance.sh \
  --dist "$RELEASE_STAGING/windows" --version 1.7.0 --commit "$RELEASE_SHA" \
  --require-asset Golden-Balloon-1.7.0-windows-x64.zip \
  --require platform=windows --require "phone_party=$PHONE_PARTY_MODE"

test "$(gh release view v1.7.0 --repo akratch/goldenballoon \
  --json isDraft --jq .isDraft)" = true
LINUX_UPLOAD_PATHS=(
  "$RELEASE_STAGING/linux/Golden-Balloon-1.7.0-linux-x86_64.AppImage"
  "$RELEASE_STAGING/linux/Golden-Balloon-1.7.0-linux-x86_64.AppImage.sha256"
  "$RELEASE_STAGING/linux/Golden-Balloon-1.7.0-linux-x86_64.AppImage.provenance.json"
  "$RELEASE_STAGING/linux/Golden-Balloon-1.7.0-linux-x86_64.tar.gz"
  "$RELEASE_STAGING/linux/Golden-Balloon-1.7.0-linux-x86_64.tar.gz.sha256"
  "$RELEASE_STAGING/linux/Golden-Balloon-1.7.0-linux-x86_64.tar.gz.provenance.json"
)
RELEASE_ID="$(gh api repos/akratch/goldenballoon/releases/tags/v1.7.0 --jq .id)"
EXISTING_ASSETS="$(gh api --paginate \
  "repos/akratch/goldenballoon/releases/$RELEASE_ID/assets?per_page=100" \
  --jq '.[].name')"
for upload_path in "${LINUX_UPLOAD_PATHS[@]}"; do
  upload_name="$(basename "$upload_path")"
  if [[ ! -f "$upload_path" ]]; then
    echo "required local release file is missing; refusing upload: $upload_path" >&2
    exit 1
  fi
  if printf '%s\n' "$EXISTING_ASSETS" | grep -Fqx "$upload_name"; then
    echo "release asset already exists; refusing upload: $upload_name" >&2
    exit 1
  fi
done
gh release upload v1.7.0 --repo akratch/goldenballoon \
  "${LINUX_UPLOAD_PATHS[@]}"
```

Do not use `gh release upload --clobber`: duplicate names are a stop condition,
not permission to replace qualified bytes. Preserve the downloaded Windows
directory unchanged through the real-hardware acceptance above. Only after it
passes, upload that exact zip and its two sidecars:

```bash
test "$(gh release view v1.7.0 --repo akratch/goldenballoon \
  --json isDraft --jq .isDraft)" = true
WINDOWS_UPLOAD_PATHS=(
  "$RELEASE_STAGING/windows/Golden-Balloon-1.7.0-windows-x64.zip"
  "$RELEASE_STAGING/windows/Golden-Balloon-1.7.0-windows-x64.zip.sha256"
  "$RELEASE_STAGING/windows/Golden-Balloon-1.7.0-windows-x64.zip.provenance.json"
)
RELEASE_ID="$(gh api repos/akratch/goldenballoon/releases/tags/v1.7.0 --jq .id)"
EXISTING_ASSETS="$(gh api --paginate \
  "repos/akratch/goldenballoon/releases/$RELEASE_ID/assets?per_page=100" \
  --jq '.[].name')"
for upload_path in "${WINDOWS_UPLOAD_PATHS[@]}"; do
  upload_name="$(basename "$upload_path")"
  if [[ ! -f "$upload_path" ]]; then
    echo "required local release file is missing; refusing upload: $upload_path" >&2
    exit 1
  fi
  if printf '%s\n' "$EXISTING_ASSETS" | grep -Fqx "$upload_name"; then
    echo "release asset already exists; refusing upload: $upload_name" >&2
    exit 1
  fi
done
gh release upload v1.7.0 --repo akratch/goldenballoon \
  "${WINDOWS_UPLOAD_PATHS[@]}"
```

Publish the unsigned macOS artifact into the existing draft by dispatching the
same tagged source commit with its binding enabled:

```bash
gh workflow run macos-release.yml --repo akratch/goldenballoon --ref v1.7.0 \
  -f version=1.7.0 \
  -f trusted_signing=false \
  -f release_tag=v1.7.0
```

The publish job must independently re-check the tag/commit binding, checksum,
exact artifact name, provenance fields, and provenance digest before uploading
to the existing draft `v1.7.0` GitHub Release. It preflights all three target
names before the first upload and must reject an absent, already-published,
prerelease, or colliding target rather than exposing a partial release.

Before publishing the draft, compare its complete asset inventory with the
twelve exact artifact/sidecar names, then re-run the public GitHub gate so the
new release metadata and sidecar contents are checked too:

```bash
printf '%s\n' \
  Golden-Balloon-1.7.0-linux-x86_64.AppImage \
  Golden-Balloon-1.7.0-linux-x86_64.AppImage.sha256 \
  Golden-Balloon-1.7.0-linux-x86_64.AppImage.provenance.json \
  Golden-Balloon-1.7.0-linux-x86_64.tar.gz \
  Golden-Balloon-1.7.0-linux-x86_64.tar.gz.sha256 \
  Golden-Balloon-1.7.0-linux-x86_64.tar.gz.provenance.json \
  Golden-Balloon-1.7.0-windows-x64.zip \
  Golden-Balloon-1.7.0-windows-x64.zip.sha256 \
  Golden-Balloon-1.7.0-windows-x64.zip.provenance.json \
  Golden-Balloon-1.7.0-macos-arm64-unsigned.dmg \
  Golden-Balloon-1.7.0-macos-arm64-unsigned.dmg.sha256 \
  Golden-Balloon-1.7.0-macos-arm64-unsigned.dmg.provenance.json \
  | LC_ALL=C sort > "$RELEASE_STAGING/expected-assets.txt"
gh release view v1.7.0 --repo akratch/goldenballoon \
  --json assets,isDraft --jq '.assets[].name' \
  | LC_ALL=C sort > "$RELEASE_STAGING/actual-assets.txt"
diff -u "$RELEASE_STAGING/expected-assets.txt" "$RELEASE_STAGING/actual-assets.txt"
test "$(gh release view v1.7.0 --repo akratch/goldenballoon \
  --json isDraft --jq .isDraft)" = true
tools/manual/check_github_launch_ready.sh --repo akratch/goldenballoon
gh release edit v1.7.0 --repo akratch/goldenballoon --draft=false --latest
```

Any mismatch leaves the release in draft. Do not delete, replace, or rebuild an
artifact to make the list pass; diagnose the exact failed producer or acceptance
record and repeat that lane from the unchanged tag.

### Optional trusted macOS artifact

The credentialed path is not part of the unsigned 1.7.0 release. If it is used
later, its exact artifact name is
`Golden-Balloon-1.7.0-macos-arm64-signed-notarized.dmg`, with matching
`.sha256` and `.provenance.json` sidecars and
`macos_signing: developer-id-notarized`. Dispatch with
`trusted_signing=true`; the workflow must Developer ID-sign with Hardened
Runtime, notarize and staple the app, sign and notarize the DMG, require
Gatekeeper acceptance, and still enforce `release_tag=v1.7.0` against the exact
workflow commit before publication. There is no release-approved skip-notary
path.

- [ ] After the final Developer ID signatures and stapling, launch the exact
      signed app through LaunchServices and require WebGPU-default startup,
      four successful surface presents, a pixel capture, canonical clean
      AppHost telemetry, and no Homebrew/SDL3 runtime loads. Static Gatekeeper
      acceptance and the pre-sign unsigned smoke do not satisfy this gate.

## 6. Web publication

Publishing is `workflow_dispatch`-only, by deliberate maintainer decision — it never
fires on push, tag or schedule. The workflow re-runs the size budget, ROM-absence
guard, browser save-custody gate, and tracked-ROM check as their own red steps,
so the release does not depend on a script the build could have skipped.
It also refuses every branch, raw commit, moving `main`, and stale tag: dispatch
the exact project-version tag only, after that tag is bound to the qualified
candidate:

```bash
gh workflow run web-demo.yml --ref v1.7.0
```

The tag-binding step must report `web deployment bound to v1.7.0 at <SHA>`,
where `<SHA>` is the same qualified commit recorded by the desktop artifacts.
The next gate must confirm that `v1.7.0` is already the latest published,
non-prerelease GitHub Release; a pushed tag, draft, prerelease, or older
published tag cannot update or roll back the production site. The deploy job
repeats both state and latest-tag checks immediately before calling Pages, so a
release withdrawn or superseded while the build runs also fails closed.

Before dispatching:

- [ ] `CHANGELOG.md` updated, and accurate — no capability claimed that the suite
      above does not demonstrate.
- [ ] `README.md`'s status table still matches measured reality.
- [ ] `LICENSE`, `NOTICE.md`, `DISCLAIMER.md` still describe what is actually in the
      tree. In particular, if a directory changed provenance, `NOTICE.md` must say so.
- [ ] Version tag agrees with `CHANGELOG.md`.

## 7. Post-release spot checks

For the concise human route, use
[`RELEASE_CANDIDATE_TEST_GUIDE.md`](RELEASE_CANDIDATE_TEST_GUIDE.md). Record the
candidate's exact source commit and hashes in the acceptance result; the
artifact sidecars remain the source of truth. The steps below are the
policy-level detail behind that guide.

Load the published page in a WebGPU browser, select a ROM, and confirm it boots and
renders. Confirm in devtools that no request carries ROM bytes — the ROM is read
client-side and must never be uploaded.

Download a save backup, erase stored progress, import the downloaded file, and
confirm the preview is correct. In a browser/profile without WebGPU, confirm that
the same save controls remain available while **Play** is blocked.

For macOS, download the published DMG onto a machine without the build tree or
Homebrew SDL libraries, verify its `.sha256`, mount it, copy `Golden Balloon.app` to
`/Applications`, and launch it without renderer overrides. Confirm the ROM-free
launcher renders through WebGPU, first launch produces at most the expected
unidentified-developer warning, and Finder never reports the app as damaged.
