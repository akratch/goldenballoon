# Release checklist

Run this before every public release. It is ordered so the cheapest gate that can
stop a release runs first. Two of these steps are the ones that keep the project in
the clear legally — they are scripts, not judgement calls, and they fail closed.

Nothing here is optional, and nothing here should be reasoned around. If a gate
fails, the release stops.

## 0. Prerequisites

You supply your own legally-owned ROM. It is never committed.

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
actually imports ASan. The ROM-free CTest task includes the display/runtime/
layout/scheduler units, the deterministic audio queue controller, and SDL's
silent queue-mode sink contract. When the selected build contains native
launcher GPU tests, the runner includes them under their shared CTest resource
lock; the complete local matrix never silently excludes them. The RAW16 gate
repeats against primary, Release, and ASan artifacts. The specialized
native-layout gate verifies linked
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
the reference: that is the intentional positive control which reproduces the
historical boss-speed error.

Run `python3 tests/check_bluey2_rematch.py --build build-rel --rom <owned ROM>`
as the progression-valid, audio-bearing standing gate. The measured release
closeout and timer-sampling explanation are in
[`BLUEY2_PARITY.md`](BLUEY2_PARITY.md).

The broader Ancient Lake `race_state_oracle` remains a deliberately red
diagnostic for longstanding open-loop floating-point drift, as documented in
[`ORACLE.md`](ORACLE.md); it is not a substitute for this passing cadence gate.
For a change which intentionally touches gameplay math or authority ordering,
record before/after Ancient Lake reports as differential evidence instead of
loosening its strict real-ROM thresholds.

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
  --only wave_visible_table,browser_save_ui,browser_resource_plateau,touch_controls,browser_runtime,browser_presentation_rates,browser_taj_character_select,browser_taj_persistence \
  --wasm build-web/mdkr64_web.wasm \
  --rom baserom.us.v80.z64
```

Expected: `check_no_rom: PASS — N artifact(s) scanned, no ROM data present.` and
all eight runner tasks PASS.

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
the default Original/authored-motion baseline. It also fails if an async pipeline takes more than two
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

## 5. Desktop packaging and publication

Desktop workflow version inputs are filename components, so public releases use
bare semantic versions such as `1.0.4`, never `v1.0.4`. The `v` prefix belongs
only to the Git tag. For version 1.0.4, the portable workflow must produce:

- `Golden-Balloon-1.0.4-linux-x86_64.AppImage`
- `Golden-Balloon-1.0.4-linux-x86_64.tar.gz`

Automatic Windows publication is intentionally disabled for this patch because
`windows-latest` does not guarantee a qualifying D3D12/Vulkan adapter or GL 3.3
context. Its headless `--help`/`--video-list` execution is not accepted as a
rendered launcher or gameplay gate. The workflow still builds, unit-tests,
import-checks, packages, extracts, and launches `GoldenBalloon.exe` from an
unrelated CWD.

The exact-manifest `Golden-Balloon-1.0.4-windows-x64.zip` may be attached only
after manual acceptance on Windows hardware proves the extracted package can:

1. open the real launcher through default WebGPU;
2. load the supported ROM and complete the release gameplay checklist;
3. receive controller input and produce audio;
4. save and reload settings plus EEPROM data; and
5. exit and relaunch cleanly.

Record the tester, Windows version, GPU, archive SHA-256, and outcome with the
release. Publish the exact checksum-verified archive and provenance sidecars
that were accepted; never substitute or rebuild it afterward. Explicit GL is a
diagnostic follow-up, not a prerequisite for endorsing the WebGPU-default
Windows artifact. This is a manual native GPU acceptance boundary, not an
automated GPU-qualification claim.

Dispatch it with:

```bash
gh workflow run release.yml --ref v1.0.4 \
  -f version=1.0.4 \
  -f release_tag=v1.0.4
```

Use `version=dev` only for disposable test artifacts, never for a public
release; `release_tag` must then be empty. A semantic-version build requires the
exact `v<version>` tag, and that tag must resolve to the workflow's source
commit. The workflow must reject every other input shape, compile that exact
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

### macOS 1.0.4 — unsigned/ad-hoc patch artifact

The public 1.0.4 macOS artifact intentionally skips Developer ID signing and
notarization. “Unsigned” in its filename means there is no trusted signing
identity: the app must still have a valid inside-out ad-hoc integrity seal. The
only expected first-launch interruption is macOS's unidentified-developer
warning; a “damaged” warning is always a release failure.

The exact public files are:

- `Golden-Balloon-1.0.4-macos-arm64-unsigned.dmg`
- `Golden-Balloon-1.0.4-macos-arm64-unsigned.dmg.sha256`
- `Golden-Balloon-1.0.4-macos-arm64-unsigned.dmg.provenance.json`

The provenance sidecar must name that exact DMG, the exact 40-character source
commit, version `1.0.4`, platform `macos`, the DMG SHA-256, and
`macos_signing: ad-hoc-unsigned`.

Before producing the candidate:

- [ ] The source tree and index are clean.
- [ ] `CMakeLists.txt`, `macos/Resources/Info.plist`, the app's `--version`
      output, and the release notes all agree on `1.0.4`.
- [ ] The release commit is the intended `v1.0.4` tag commit. A test artifact
      may omit `release_tag`; an artifact may be published only with
      `release_tag=v1.0.4` resolving to the workflow's exact source commit.
- [ ] The pinned standalone SDL2 build is used for arm64/macOS 13. Homebrew
      `sdl2-compat`, SDL3, Homebrew load paths, mixed architectures, and a
      deployment target newer than 13.0 are release blockers.

Build and validate a non-publishing candidate through the protected workflow:

```bash
gh workflow run macos-release.yml \
  -f version=1.0.4 \
  -f trusted_signing=false
```

The package job must complete all of these checks before its artifact is
accepted:

- [ ] Build SHA-pinned standalone SDL2 2.32.10 for arm64/macOS 13.
- [ ] Build `mdkr64.app` with `--strict-deployment-target`, embed version
      `1.0.4` and the exact source commit, bundle SDL2, then seal nested code
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

After the test artifact passes and `v1.0.4` exists on the exact candidate
commit, publish by dispatching the same source commit with the binding enabled:

```bash
gh workflow run macos-release.yml --ref v1.0.4 \
  -f version=1.0.4 \
  -f trusted_signing=false \
  -f release_tag=v1.0.4
```

The publish job must independently re-check the tag/commit binding, checksum,
exact artifact name, provenance fields, and provenance digest before uploading
to the existing `v1.0.4` GitHub Release.

### Optional trusted macOS artifact

The credentialed path is not part of the unsigned 1.0.4 release. If it is used
later, its exact artifact name is
`Golden-Balloon-1.0.4-macos-arm64-signed-notarized.dmg`, with matching
`.sha256` and `.provenance.json` sidecars and
`macos_signing: developer-id-notarized`. Dispatch with
`trusted_signing=true`; the workflow must Developer ID-sign with Hardened
Runtime, notarize and staple the app, sign and notarize the DMG, require
Gatekeeper acceptance, and still enforce `release_tag=v1.0.4` against the exact
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
Homebrew SDL libraries, verify its `.sha256`, mount it, copy `mdkr64.app` to
`/Applications`, and launch it without renderer overrides. Confirm the ROM-free
launcher renders through WebGPU, first launch produces at most the expected
unidentified-developer warning, and Finder never reports the app as damaged.
