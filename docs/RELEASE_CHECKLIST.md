# Release checklist

Run this before every public release. It is ordered so the cheapest gate that can
stop a release runs first. Two of these steps are the ones that keep the project in
the clear legally — they are scripts, not judgement calls, and they fail closed.

Nothing here is optional, and nothing here should be reasoned around. If a gate
fails, the release stops.

## 0. Prerequisites

You supply your own legally-owned ROM. It is never committed.

```bash
ln -s /path/to/your/baserom.us.v80.z64 baserom.us.v80.z64
```

> **Audio safety — hard rule.** Every engine invocation below passes
> `--headless-frames N`, which returns before the audio device is ever opened, and
> sets `MDKR_AUDIO=0`. Never run the binary without `--headless-frames`, **not even
> `--help`**. Note that `MDKR_AUDIO=off` is a **no-op** — the check is
> `disable[0] == '0'`, so only the digit `0` disables. See
> [`../CONTRIBUTING.md`](../CONTRIBUTING.md).

## 1. Clean-room verification

The claim in [`../DISCLAIMER.md`](../DISCLAIMER.md) and [`../NOTICE.md`](../NOTICE.md)
is that no ROM or ROM-derived data is in this repository — *"not in the working tree
and not in git history"*. A ROM committed and then deleted in a later commit is
still in every clone forever, so a working-tree check alone does not prove this.

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

```bash
cmake -S . -B build     && cmake --build build     -j8   # Debug, the default
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release \
                        && cmake --build build-rel -j8   # what the browser ships
cmake -S . -B build-asan \
  -DCMAKE_C_FLAGS="-fsanitize=address -g -O1 -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
                        && cmake --build build-asan -j8  # first-session safety gate
```

Expected: `[100%] Built target mdkr64`, no new warnings, from all three.

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
`tests/check_*.py` is absent from its manifest.

```bash
python3 tools/run_checks.py \
  --build build-rel --release-build build-rel --asan-build build-asan \
  --skip-wasm
```

`check_key_cutscene_once.py` is the one check that is **build-type-sensitive by
design**. The runner verifies that its dedicated binary has an optimized
`CMAKE_BUILD_TYPE`; it also verifies that the filename-entry sanitizer binary
actually imports ASan. The ROM-free CTest task includes `display_config`,
`endian_utils`, `object_layout`, and `memory_allocator`; the RAW16 gate repeats against primary,
Release, and ASan artifacts. The specialized native-layout gate verifies linked
ASan/alignment handlers and exact legacy controls, then runs the complete
menu/track/vehicle/Adventure/boss/2P/widescreen matrix under halt-on-error
alignment UBSan. The primary suite's seven-arm `check_widescreen_proportions.py`
pixel-measures both SAFE_2D and world-space golden balloons at 4:3, 16:9,
21:9, and changed FOV, with exact legacy stretching as its failing control.
`check_native_ui_resolution.py` runs GL/WebGPU production and disabled-control
arms, requires the HUD/minimap-only pixel delta and measured edge gain, and
rejects any world-after-overlay draw or pass-start failure. The 2P/3P/4P gates
extend that ordering assertion to every viewport layout.
`check_video_options.py` runs both native backends and requires every in-game
control, atomic fresh-process reload, override locking/no-bake behavior,
unwritable-storage rollback, and malformed-launcher no-rewrite behavior. The
real-browser gate repeats the menu mutation and IDBFS reload in wasm.
`--skip-wasm` is used here only because the web artifact is built and both
structurally and dynamically checked in section 4.

## 3. Behavioural regression suite

Run every primary behavioural check against Debug as the second configuration.
The specialized Release/ASan/UBSan arms already ran in section 2b.

```bash
python3 tools/run_checks.py --build build --primary-only --skip-wasm
```

See [`../tests/README.md`](../tests/README.md) for every assertion, positive
control, and frame budget. The optimized and Debug invocations are both required:
the former catches optimizer-created defects, while the latter is the everyday
developer configuration and can expose different layout/timing failures.

Do not add `--expect-fail 9`: the historical level-9 failure no longer reproduces
and both math arms complete that track. Keeping the stale exception would allow a
new regression on exactly that track to pass the release gate.

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

```bash
tools/web/build_web.sh          # builds, stages dist/web, runs the guard itself
tools/check_no_rom.sh dist/web
python3 tools/run_checks.py \
  --only wave_visible_table,browser_save_ui,browser_resource_plateau,touch_controls,browser_runtime \
  --wasm build-web/mdkr64_web.wasm \
  --rom baserom.us.v80.z64
```

Expected: `check_no_rom: PASS — N artifact(s) scanned, no ROM data present.` and
all four runner tasks PASS.

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
upload or name the ROM. This is the reproducible browser-runtime evidence;
the post-release check below remains a human packaging/hosting check.
It also requires exactly one authored NTSC realtime pace initialization, no
update or wall-field count below two, a 24–36 FPS median complete-loop cadence,
and post-startup cadence no worse than 40.0 ms p95 / 45.0 ms p99. These are
temporary containment budgets while authoritative update and presentation
remain inseparable. It also fails if an
async pipeline takes more than two render frames, or if incomplete-pipeline
presentation holds the last complete image for more than two consecutive
frames. The raw maximum and its frame number remain visible in the PASS line.

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

## 5. Publication

Publishing is `workflow_dispatch`-only, by deliberate maintainer decision — it never
fires on push, tag or schedule. The workflow re-runs the size budget, ROM-absence
guard, browser save-custody gate, and tracked-ROM check as their own red steps,
so the release does not depend on a script the build could have skipped.

Before dispatching:

- [ ] `CHANGELOG.md` updated, and accurate — no capability claimed that the suite
      above does not demonstrate.
- [ ] `README.md`'s status table still matches measured reality.
- [ ] `LICENSE`, `NOTICE.md`, `DISCLAIMER.md` still describe what is actually in the
      tree. In particular, if a directory changed provenance, `NOTICE.md` must say so.
- [ ] Version tag agrees with `CHANGELOG.md`.

## 6. Post-release spot check

Load the published page in a WebGPU browser, select a ROM, and confirm it boots and
renders. Confirm in devtools that no request carries ROM bytes — the ROM is read
client-side and must never be uploaded.

Download a save backup, erase stored progress, import the downloaded file, and
confirm the preview is correct. In a browser/profile without WebGPU, confirm that
the same save controls remain available while **Play** is blocked.
