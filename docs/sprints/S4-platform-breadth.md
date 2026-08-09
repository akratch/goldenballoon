# S4 — Platform breadth

> **Executing this plan:** work the tasks in order, one `- [ ]` step at a time.
> Tick a step only once its command has been run and its output read. The
> run-it-and-watch-it-fail steps are load-bearing; skipping ahead to the
> implementation forfeits the positive control that makes the gate mean anything.

**Goal:** Close the four platform gaps that separate this port from the Harbour
Masters distribution model — an Intel macOS slice, a user-selectable Windows
graphics adapter with real diagnostics, a native Linux file picker, and measured
Wayland and controller-hotplug behaviour.

**Architecture:** No engine changes. Every task here is a packaging, backend
selection, or platform-boundary concern, and each one is separately shippable.
The one shared piece of new machinery is a **GPU diagnostics record** — a
structured description of the adapter, backend, and why it was chosen — because
three of the four tasks currently fail in the same way: something does not work
and the user has no way to say what they have.

**Tech stack:** CMake, GitHub Actions, SDL2, the existing WebGPU (Dawn) and
OpenGL backends, macOS `lipo`/`codesign`/`notarytool`, GTK/portal file dialogs on
Linux.

## Global constraints

- Every game or test invocation passes `--headless-frames N` and sets
  `MDKR_AUDIO=0`.
- Do not add a rendering backend. The Metal backend was deliberately removed and
  recorded as removed in `docs/DEFINITION_OF_DONE.md`; reintroducing one is out
  of scope and would need its own decision.
- Windows filesystem and process work goes through `platform/fs_utf8.h`, which
  already owns the UTF-8↔UTF-16 boundary. Do not add a second conversion.
- Anything claimed as passing on hardware this project does not have must be
  labelled as unverified, in the same commit, in
  `docs/DEFINITION_OF_DONE.md`'s hardware list. Do not quietly promote a code
  review to a machine check.
- Run `python3 tools/check_public_surface.py --staged` before every commit.

---

## User stories

**US-1 — Play on an Intel Mac.** As a player with a 2019 MacBook Pro, I download
one DMG and it runs, so that I am not told to build from source.

**US-2 — Pick my GPU.** As a Windows player with a hybrid-graphics laptop, I
choose which adapter and backend the game uses, so that it stops launching on the
integrated chip.

**US-3 — Understand a graphics failure.** As a Windows or Linux player whose game
will not start, I get a message naming my adapter, driver version, the backends
tried, and why each was rejected, so that my bug report is actionable.

**US-4 — Choose a ROM on Linux.** As a Linux player, I click "Choose ROM File"
and get a real file picker, so that I do not have to paste an absolute path.

**US-5 — Play on Wayland.** As a Linux player on a Wayland session, the window,
fullscreen, and input all behave, so that I am not pushed back to X11.

**US-6 — Plug in a pad mid-race.** As a player, I connect or disconnect a
controller during play and the game adapts without dropping input or crashing, so
that a flat battery is not a lost race.

---

## Milestones and acceptance criteria

### M1 — GPU diagnostics record

**Done when:** a single structured record describes the selected backend, the
adapter name, vendor and device id, the driver version where available, and an
ordered list of every backend considered with the reason each was accepted or
rejected; it is printed on startup at a `[GPUINFO]` marker, shown in the
diagnostics UI, and included in the crash screen if S3 has landed.

### M2 — macOS universal binary

**Done when:**
- `cmake --preset rel` on an Apple-silicon host produces an `arm64` binary as
  today, and the release path produces a `universal` binary containing both
  `arm64` and `x86_64` slices.
- `lipo -archs` on the shipped executable and on every bundled dylib lists both
  slices; a gate asserts it rather than a human checking.
- Ad-hoc signing after every Mach-O mutation still holds — `codesign --verify
  --deep --strict` passes on the assembled bundle.
- The existing notarization workflow is unchanged except for the wider binary.

### M3 — Windows adapter policy

**Done when:**
- `Video.GraphicsAdapter` (appended key) accepts `auto`, `high-performance`,
  `low-power`, or an adapter substring; `Video.GraphicsBackend` accepts `auto`,
  `d3d12`, `vulkan`, `gl`.
- Selection failures fall back in a defined order and say so in `[GPUINFO]`
  rather than failing to start.
- A hosted-CI arm asserts the selection logic over a table of synthetic adapter
  lists, so the policy is tested without needing the hardware.
- `tools/check_windows_imports.sh` still passes; no new import is added to the
  stock-Windows surface.

### M4 — Linux native file picker

**Done when:** the launcher's "Choose ROM File" opens an `xdg-desktop-portal`
file chooser when the portal is available and a GTK dialog otherwise, falls back
to today's drag-and-drop/paste path when neither is, and the fallback is
announced in the UI rather than silently presented as the only option.

### M5 — Wayland and hotplug acceptance

**Done when:** a documented acceptance route covering window creation,
fullscreen, DPI scaling, cursor capture, and controller connect/disconnect
mid-race has been executed on a Wayland session and an X11 session, with results
recorded; and `check_input_hotplug.py` gates the controller half automatically on
any platform.

---

## File structure

**Create:**

| Path | Responsibility |
|---|---|
| `platform/gpu_diagnostics.h/.c` | The adapter/backend record and its formatting. |
| `platform/adapter_policy.h/.c` | Pure selection policy over a candidate list. |
| `platform/file_dialog_linux.c` | Portal and GTK file chooser. |
| `tests/test_adapter_policy.c` | Selection policy unit test, no GPU. |
| `tests/check_gpu_diagnostics.py` | `[GPUINFO]` presence and completeness gate. |
| `tests/check_input_hotplug.py` | Controller connect/disconnect gate. |
| `tests/check_macos_universal.py` | `lipo` slice gate (skips off macOS). |
| `docs/PLATFORM_ACCEPTANCE.md` | The manual Wayland/X11/hardware routes. |

**Modify:** `platform/video_config.h/.c`, `platform/host_window.c`,
`platform/fast3d/gfx_webgpu.c`, `platform/app/file_dialog.h`,
`platform/app/file_dialog_stub.cpp`, `platform/app/ui_launcher.cpp`,
`platform/platform_sdl_min.c`, `CMakeLists.txt`, `cmake/tests.cmake`,
`macos/` packaging scripts, `.github/workflows/macos-release.yml`,
`.github/workflows/release.yml`, `tools/run_checks.py`,
`docs/DEFINITION_OF_DONE.md`, `README.md`.

---

## Task 1: GPU diagnostics record

**Files:**
- Create: `platform/gpu_diagnostics.h`, `platform/gpu_diagnostics.c`,
  `tests/check_gpu_diagnostics.py`
- Modify: `platform/fast3d/gfx_webgpu.c`, `platform/fast3d/gfx_opengl.c`,
  `CMakeLists.txt`, `tools/run_checks.py`

**Interfaces:**
- Produces: `MdkrGpuInfo`, `mdkr_gpu_info_reset()`, `mdkr_gpu_info_note_candidate()`,
  `mdkr_gpu_info_select()`, `mdkr_gpu_info_print()`. Tasks 2 and 3 both record
  through these.

- [ ] **Step 1: Write the failing gate.** `tests/check_gpu_diagnostics.py`
  asserts that a headless run on any platform emits exactly one `[GPUINFO]`
  block, that it names the selected backend, that it lists at least one
  candidate, that every listed candidate carries a non-empty reason string, and
  that the block appears **before** the first rendered frame so a startup failure
  still produces it.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Write the header**

```c
/* gpu_diagnostics.h — what adapter we chose, and why we rejected the others.
 *
 * Three of this project's open platform items are the same item: something did
 * not work on hardware the maintainers do not have, and the report could not
 * say what the hardware was. This record exists so that the next such report
 * arrives complete.
 */
#ifndef MDKR64_GPU_DIAGNOSTICS_H
#define MDKR64_GPU_DIAGNOSTICS_H

#include <stddef.h>
#include <stdint.h>

#define MDKR_GPU_MAX_CANDIDATES 8
#define MDKR_GPU_STR_MAX        192

typedef struct MdkrGpuCandidate {
    char backend[32];       /* "webgpu-d3d12", "webgpu-vulkan", "gl" */
    char adapter[MDKR_GPU_STR_MAX];
    char driver[MDKR_GPU_STR_MAX];
    uint32_t vendor_id, device_id;
    int  accepted;          /* 1 selected, 0 rejected */
    char reason[MDKR_GPU_STR_MAX]; /* never empty */
} MdkrGpuCandidate;

typedef struct MdkrGpuInfo {
    MdkrGpuCandidate candidates[MDKR_GPU_MAX_CANDIDATES];
    int              count;
    int              selected;  /* index, or -1 */
} MdkrGpuInfo;

void mdkr_gpu_info_reset(void);
void mdkr_gpu_info_note_candidate(const MdkrGpuCandidate *candidate);
void mdkr_gpu_info_select(int index, const char *reason);
/* Emits the `[GPUINFO]` block. Safe to call before a window exists. */
void mdkr_gpu_info_print(void);
const MdkrGpuInfo *mdkr_gpu_info_get(void);

#endif /* MDKR64_GPU_DIAGNOSTICS_H */
```

- [ ] **Step 4: Record candidates from both backends.** In
  `platform/fast3d/gfx_webgpu.c`, note each adapter Dawn enumerates with its
  properties; in `platform/fast3d/gfx_opengl.c`, note the GL renderer and version
  strings. Call `mdkr_gpu_info_print()` immediately after selection, before the
  first frame.

- [ ] **Step 5: Run the gate. Verify it passes.**

- [ ] **Step 6: Register in `CHECKS` and commit.**

---

## Task 2: macOS universal binary

**Files:**
- Create: `tests/check_macos_universal.py`
- Modify: `CMakeLists.txt`, the `macos/` packaging scripts,
  `.github/workflows/macos-release.yml`, `tools/run_checks.py`, `README.md`

- [ ] **Step 1: Write the failing gate.** `tests/check_macos_universal.py`:
  - skips with a clear message when not on macOS or when no release bundle is
    present, and **reports the skip** rather than passing silently;
  - runs `lipo -archs` on the bundle executable and asserts both `x86_64` and
    `arm64`;
  - walks every `.dylib` and `.framework` binary inside the bundle and asserts
    the same, because a universal executable with a single-slice dependency
    fails at launch on the other architecture with no useful message;
  - runs `codesign --verify --deep --strict` and asserts success.

- [ ] **Step 2: Run it against the current build, verify it fails** on the
  missing `x86_64` slice.

- [ ] **Step 3: Set `CMAKE_OSX_ARCHITECTURES`** for the release preset only.
  Leave the `dev` preset host-native — doubling every developer build to chase a
  packaging property would be a bad trade.

- [ ] **Step 4: Resolve the dependency slices.** The WebGPU runtime is fetched
  and hash-verified at configure time (see `cmake/webgpu.cmake`). Either fetch
  both slices and `lipo -create` them, or fetch a universal artifact if upstream
  publishes one. Whichever path is chosen, the hash verification must still
  fail closed — record both hashes, do not relax the check.

- [ ] **Step 5: Re-sign after every mutation.** `lipo -create` rewrites the
  Mach-O and invalidates any existing signature. The ad-hoc signing step must run
  after the last mutation; confirm ordering by running the gate.

- [ ] **Step 6: Run the gate. Verify it passes.**

- [ ] **Step 7: Run the packaged-app smoke path** the release workflow already
  uses, so that a universal bundle is proven to launch and not merely to contain
  two slices.

- [ ] **Step 8: Update `README.md`'s download table** to name one macOS file
  covering both architectures, and update
  `.github/workflows/macos-release.yml`'s artifact name accordingly.

- [ ] **Step 9: Register in `CHECKS` and commit.**

---

## Task 3: Adapter and backend selection policy

**Files:**
- Create: `platform/adapter_policy.h`, `platform/adapter_policy.c`,
  `tests/test_adapter_policy.c`
- Modify: `platform/video_config.h/.c`, `platform/fast3d/gfx_webgpu.c`,
  `cmake/tests.cmake`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `MdkrGpuCandidate` (Task 1).
- Produces: `mdkr_adapter_policy_choose()`. `gfx_webgpu.c` is its only caller.

- [ ] **Step 1: Append the keys and their schema rows.**

```c
    MDKR_VIDEO_GRAPHICS_ADAPTER,
    MDKR_VIDEO_GRAPHICS_BACKEND,
```

```c
    [MDKR_VIDEO_GRAPHICS_ADAPTER] = {
        "Video.GraphicsAdapter", "MDKR_GPU_ADAPTER",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 0.0f,
        "Graphics adapter",
        "auto picks for you. high-performance or low-power choose a class. "
        "Any other text matches part of an adapter name.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_VIDEO_GRAPHICS_BACKEND] = {
        "Video.GraphicsBackend", "MDKR_GPU_BACKEND",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 0.0f,
        "Graphics backend",
        "auto is recommended. Choose one only to work around a driver problem.",
        MDKR_VIDEO_CAT_FIDELITY
    },
```

Add the matching default/range/text table entries.

- [ ] **Step 2: Write the failing test.** `tests/test_adapter_policy.c` is a pure
  table test over synthetic candidate lists — no GPU required, so it runs
  everywhere including hosted CI. Assert:
  - `auto` on a hybrid list picks discrete over integrated;
  - `low-power` picks integrated even when discrete is present;
  - a substring match picks the named adapter regardless of class;
  - a substring matching nothing falls back to `auto` **and returns a reason
    saying so**, rather than failing to start;
  - an empty candidate list returns a defined failure, not index `-1` used as an
    index;
  - backend preference is applied before adapter class, and the resulting order
    is asserted explicitly for a three-backend list.

- [ ] **Step 3: Register and run to verify it fails.**

- [ ] **Step 4: Implement.** Pure function: candidates in, index and reason out.
  No Dawn types in the signature, so the test needs no graphics headers.

- [ ] **Step 5: Call it from `gfx_webgpu.c`** and record the outcome through
  `mdkr_gpu_info_select()`.

- [ ] **Step 6: Run the unit test, the GPU diagnostics gate, and the WebGPU
  suites.**

```bash
ctest --test-dir build-rel -R 'adapter_policy' --output-on-failure
MDKR_AUDIO=0 python3 tests/check_gpu_diagnostics.py
MDKR_AUDIO=0 python3 tests/check_determinism.py
```

- [ ] **Step 7: Verify the Windows import surface is unchanged.**

```bash
bash tools/check_windows_imports.sh
bash tools/mingw_cross_check.sh
```

- [ ] **Step 8: Commit.**

---

## Task 4: Linux native file picker

**Files:**
- Create: `platform/app/file_dialog_linux.c`
- Modify: `platform/app/file_dialog.h`, `platform/app/file_dialog_stub.cpp`,
  `platform/app/ui_launcher.cpp`, `CMakeLists.txt`, `README.md`

- [ ] **Step 1: Extend the existing gate.** `tests/check_shell_dropfile.py`
  already covers the drag-and-drop path. Add an assertion that the launcher
  reports which intake methods are available on this platform via a
  `[ROMINTAKE]` line, and that on Linux it names either `portal`, `gtk`, or
  `paste-only`. A headless run cannot open a dialog, so the gate tests the
  *advertised capability*, not the dialog itself.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Implement the portal path first** — `org.freedesktop.portal.
  FileChooser` over D-Bus, which works on both Wayland and X11 and needs no GTK
  link. Load libdbus dynamically with `dlopen` so the binary still runs on a
  system without it; a hard link would break the "SDL2 is bundled, run anywhere"
  property the Linux tarball currently has.

- [ ] **Step 4: Implement the GTK fallback** the same way — `dlopen` GTK3, use it
  if present, otherwise report `paste-only`.

- [ ] **Step 5: Make the fallback visible.** When neither is available, the
  launcher must say so in the UI, not present paste as if it were the only design.

- [ ] **Step 6: Run the gate and the launcher tests.**

```bash
MDKR_AUDIO=0 python3 tests/check_shell_dropfile.py
MDKR_AUDIO=0 python3 tests/check_app_ui_input.py
```

- [ ] **Step 7: Remove the "Linux does not yet have a native Choose ROM File
  dialog" line from `README.md`'s known limitations** — and only then. Do not
  update the README ahead of the gate.

- [ ] **Step 8: Commit.**

---

## Task 5: Controller hotplug gate

**Files:**
- Create: `tests/check_input_hotplug.py`
- Modify: `platform/platform_sdl_min.c`, `tools/run_checks.py`

`platform/platform_sdl_min.c:1408` already opens every joystick at startup
(`for (int i = 0; i < SDL_NumJoysticks(); i++) gc_try_open(i)`). What is
unmeasured is what happens *after* startup.

- [ ] **Step 1: Write the failing gate.** Using SDL's virtual joystick API so no
  physical hardware is needed, assert:
  - a pad added mid-race is opened and its input reaches
    `osContGetReadData` on the correct channel;
  - a pad removed mid-race leaves its channel reporting exact neutral, not the
    last-held value — a stuck input after a disconnect is the failure mode that
    actually loses races;
  - removing player 1's pad does not renumber players 2–4;
  - re-adding a pad returns it to its original channel where the channel is free;
  - a pad added beyond channel 4 is ignored without error;
  - rumble on a disconnected channel is a no-op, not a crash.

- [ ] **Step 2: Run it, verify which assertions fail.** Record them; some may
  already pass, and the ones that do are still worth gating.

- [ ] **Step 3: Fix each failing assertion** in `platform/platform_sdl_min.c`,
  handling `SDL_CONTROLLERDEVICEADDED` and `SDL_CONTROLLERDEVICEREMOVED`.

- [ ] **Step 4: Run the gate and the input suites.**

```bash
MDKR_AUDIO=0 python3 tests/check_input_hotplug.py
MDKR_AUDIO=0 python3 tests/check_race_2p_split.py
MDKR_AUDIO=0 python3 tests/check_determinism.py
```

- [ ] **Step 5: Register in `CHECKS` and commit.**

---

## Task 6: Platform acceptance route

**Files:**
- Create: `docs/PLATFORM_ACCEPTANCE.md`
- Modify: `docs/DEFINITION_OF_DONE.md`, `docs/README.md`, `ROADMAP.md`

The Wayland, physical-GPU, and hybrid-graphics items cannot be closed by code.
What *can* be built is the route, so that when hardware is available the pass is
a checklist rather than an improvisation.

- [ ] **Step 1: Write `docs/PLATFORM_ACCEPTANCE.md`** with one numbered route per
  environment — Wayland, X11, Windows hybrid-graphics laptop, Windows on Arm,
  Intel Mac, Apple-silicon Mac — each listing the exact steps, the exact expected
  observation, and a result field. Include the `[GPUINFO]` block capture as a
  required artifact for every route, so a failed route arrives with its adapter
  information already attached.

- [ ] **Step 2: Update the hardware list in
  `docs/DEFINITION_OF_DONE.md`** to point at the new routes, and mark which have
  been executed. Executed means executed — do not mark a route from a code
  review.

- [ ] **Step 3: Update `ROADMAP.md`'s Platforms section** to reference the routes
  and to remove any item this sprint closed.

- [ ] **Step 4: Run the link checker.** `python3 tools/check_markdown_links.py`

- [ ] **Step 5: Commit.**

---

## Task 7: Full-suite verification

- [ ] **Step 1: Run the complete suite, sequentially.**

```bash
rm -f save/eeprom.bin
MDKR_AUDIO=0 python3 tools/run_checks.py
```

- [ ] **Step 2: Run the cross-compile checks.**

```bash
bash tools/mingw_cross_check.sh
bash tools/check_windows_imports.sh
```

- [ ] **Step 3: Paste every summary into the final commit body.**

---

## Self-review

**Spec coverage.** M1 → Task 1. M2 → Task 2. M3 → Task 3. M4 → Task 4.
M5 → Tasks 5–6. US-3 is served by Task 1 and consumed by Tasks 3 and 6.

**Deliberately out of scope:**

- **Nintendo Switch and Wii U.** Large permanent maintenance surface, and the
  Harbour Masters ports get theirs from libultraship, which this project does not
  use. Not planned.
- **Windows code signing.** It belongs with the release pipeline; it is
  [S6 Task 5](S6-release-engineering.md#task-5-windows-code-signing).
- **A native Metal backend.** Removed deliberately; reinstating it is a decision,
  not a task.
- **32-bit builds.** No demand identified, and the WebGPU runtime's 32-bit story
  is unestablished.

**Type consistency.** `MdkrGpuCandidate` is defined in Task 1 and consumed by
Task 3's `mdkr_adapter_policy_choose()`. `mdkr_gpu_info_select()` is called from
Task 1 Step 4 and Task 3 Step 5. `[GPUINFO]`, `[ROMINTAKE]` are the two new
stdout markers, introduced in Tasks 1 and 4 respectively.
