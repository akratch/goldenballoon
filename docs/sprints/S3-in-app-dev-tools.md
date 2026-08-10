# S3 — In-app developer tools

> **Executing this plan:** work the tasks in order, one `- [ ]` step at a time.
> Tick a step only once its command has been run and its output read. The
> run-it-and-watch-it-fail steps are load-bearing; skipping ahead to the
> implementation forfeits the positive control that makes the gate mean anything.

**Goal:** Make the instrumentation this project already has reachable from inside
the running game — a command console, a free camera, collision and object
viewers, a performance window, and a crash screen — so that bug reports arrive
with evidence and contributors can see what they are changing.

**Architecture:** This sprint adds almost no new measurement. It adds a *surface*
over instruments that already exist: `[SIMHASH]` streams, `MDKR_TRACE`,
`gameplay_event_trace.c`, the collision candidate lists, the presentation
snapshot, and the diagnostics log. Every tool is a read-only ImGui window
registered in one table, gated behind a single `Tools.Enabled` key that is off in
release builds unless explicitly turned on, and — critically — every tool is
required to prove it cannot move authoritative state.

**Tech stack:** C11 for the data providers, C++/ImGui in `platform/app/` for the
windows, the existing `platform/app/diag_log.cpp` for output capture.

## Global constraints

- Every game or test invocation passes `--headless-frames N` and sets
  `MDKR_AUDIO=0`.
- **No tool may write authoritative state.** Every tool ships with an arm in
  `tests/check_dev_tools_purity.py` proving the `[SIMHASH]` v3 stream is
  byte-identical with the tool open and closed. A tool that cannot pass that is
  not merged.
- No change under `game/`. These are observers.
- New `MdkrVideoKey` entries are appended, never inserted.
- Every new `tests/check_*.py` is registered in `tools/run_checks.py`'s `CHECKS`;
  every new `tests/test_*.c` in `cmake/tests.cmake`.
- Run `python3 tools/check_public_surface.py --staged` before every commit.

---

## User stories

**US-1 — Report a bug with evidence.** As a player hitting a glitch, I press a
key, get a window naming the track, tick, camera pose and last events, and copy
it to the clipboard, so that my report is actionable.

**US-2 — Look at the problem.** As a contributor, I detach the camera and fly
around a frozen scene, so that I can see what the authored camera is hiding.

**US-3 — See the collision the game sees.** As a contributor investigating a
fall-through, I overlay the collision mesh and candidate list on the rendered
scene, so that I can tell a geometry bug from a physics bug.

**US-4 — Find what is slow.** As a contributor, I open a performance window
showing per-phase frame cost, so that I can attribute a stutter before guessing.

**US-5 — Run a command.** As a contributor, I open a console and warp to a track,
set a variable, or dump state, so that reproducing a bug does not require a new
input script every time.

**US-6 — Get something useful from a crash.** As a player, when the game dies I
see a screen naming the fault, the tick, the track and where the log went,
instead of a window vanishing.

**US-7 — Not pay for tools I do not use.** As a player, the tools are off, cost
nothing, and cannot alter my race.

---

## Milestones and acceptance criteria

### M1 — Tool host, registry, and the purity gate

**Done when:**
- `platform/app/dev_tools.h/.cpp` owns a table of tools: id, title, default
  hotkey, and a draw callback.
- `Tools.Enabled` (appended `MdkrVideoKey`) gates the whole surface; default off.
- `tests/check_dev_tools_purity.py` enumerates the table from a
  `[TOOLTABLE]` dump and asserts, **for every registered tool**, that opening it
  leaves the `[SIMHASH]` v3 stream byte-identical. It fails if zero tools parse.
- The gate is in `CHECKS` before any tool exists, so every later tool is born
  gated.

### M2 — Crash screen

**Done when:** an abort, a fatal, or a signal produces a full-screen panel naming
the fault kind, the authoritative tick, the track id, the renderer, the app
version and the log path, with a copy-to-clipboard action; the existing
`[CRASH]`/`[FATAL]` stdout markers are unchanged so every harness that greps for
them still works.

### M3 — Diagnostics window and console

**Done when:** a live diagnostics window shows tick, track, camera pose, frame
pacing and the last N events; a console accepts at least `help`, `warp <track>`,
`set <key> <value>`, `get <key>`, `hash`, and `dump events`; unknown commands
report the nearest match; and `set` refuses any key not in the video/enhancement
schema rather than writing arbitrary memory.

### M4 — Free camera

**Done when:** the camera detaches on a key, flies with the gamepad or WASD,
returns to the authored pose exactly on release, and — proven by the M1 gate —
does not move authoritative state while detached.

### M5 — Collision and object viewers

**Done when:** the collision viewer overlays the mesh the physics actually
queries (not a re-derivation), the object viewer lists live objects with
behaviour id, position and flags, and both are proven inert by the M1 gate.

### M6 — Performance window

**Done when:** per-phase timings for simulation, display-list build, GPU submit
and present are shown as a rolling histogram sourced from the counters
`check_pacing_quality` already reads, with a note that synthetic-field runs are
not meaningful.

---

## File structure

**Create:**

| Path | Responsibility |
|---|---|
| `platform/app/dev_tools.h/.cpp` | Tool table, hotkeys, host window, `[TOOLTABLE]` dump. |
| `platform/app/tool_console.h/.cpp` | Command parsing, history, completion. |
| `platform/app/tool_diagnostics.h/.cpp` | Live state readout. |
| `platform/app/tool_freecam.h/.cpp` | Detached camera pose and input. |
| `platform/app/tool_collision.h/.cpp` | Collision overlay draw. |
| `platform/app/tool_objects.h/.cpp` | Live object list. |
| `platform/app/tool_performance.h/.cpp` | Phase-timing histogram. |
| `platform/app/crash_screen.h/.cpp` | Fatal presentation. |
| `platform/dev_command.h/.c` | Pure command parser: tokenise, resolve, validate. |
| `tests/test_dev_command.c` | Parser unit test, no window. |
| `tests/check_dev_tools_purity.py` | The generated inertness gate. |
| `tests/check_crash_screen.py` | Fault-path gate. |
| `tests/check_tool_freecam.py` | Detach/restore gate. |

**Modify:** `platform/video_config.h/.c`, `platform/platform_sdl_min.c`,
`platform/app/app_host.cpp`, `platform/app/ui_overlay.cpp`,
`cmake/tests.cmake`, `CMakeLists.txt`, `tools/run_checks.py`.

---

## Task 1: Tool host, table, and purity gate

**Files:**
- Create: `platform/app/dev_tools.h`, `platform/app/dev_tools.cpp`,
  `tests/check_dev_tools_purity.py`
- Modify: `platform/video_config.h`, `platform/video_config.c`,
  `platform/app/app_host.cpp`, `CMakeLists.txt`, `tools/run_checks.py`

**Interfaces:**
- Produces: `MdkrDevTool`, `DevTools_register()`, `DevTools_draw()`,
  `DevTools_dumpTable()`, `DevTools_isOpen(id)`, `DevTools_setOpen(id, bool)`.
  Every later task registers through these.

- [ ] **Step 1: Append the key** to `MdkrVideoKey` and add its schema rows:

```c
    MDKR_TOOLS_ENABLED,
```

```c
    [MDKR_TOOLS_ENABLED] = {
        "Tools.Enabled", "MDKR_TOOLS",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 1.0f,
        "Developer tools",
        "Show the diagnostic windows. Off by default; they never change play.",
        MDKR_VIDEO_CAT_INTERFACE
    },
```

Add the matching default/range/text table entries — `tests/test_video_config.c`
asserts every key has one.

- [ ] **Step 2: Write the header**

```cpp
/* dev_tools.h — the registry every in-game diagnostic window goes through.
 *
 * A tool is an OBSERVER. Registration is also a claim: that opening this window
 * cannot move authoritative state. check_dev_tools_purity.py enumerates this
 * table and tests that claim for every entry, so the claim cannot rot.
 */
#ifndef MDKR64_DEV_TOOLS_H
#define MDKR64_DEV_TOOLS_H

#include <stdbool.h>

typedef enum MdkrDevToolId {
    MDKR_TOOL_DIAGNOSTICS = 0,
    MDKR_TOOL_CONSOLE,
    MDKR_TOOL_FREECAM,
    MDKR_TOOL_COLLISION,
    MDKR_TOOL_OBJECTS,
    MDKR_TOOL_PERFORMANCE,
    MDKR_TOOL_COUNT
} MdkrDevToolId;

typedef void (*MdkrDevToolDraw)(bool *open);

typedef struct MdkrDevTool {
    MdkrDevToolId    id;
    const char      *title;   /* window title, player-readable */
    const char      *hotkey;  /* display string, e.g. "F3" */
    MdkrDevToolDraw  draw;    /* NULL until its task lands */
} MdkrDevTool;

void DevTools_register(MdkrDevToolId id, MdkrDevToolDraw draw);
void DevTools_draw(void);            /* called once per frame from app_host */
bool DevTools_isOpen(MdkrDevToolId id);
void DevTools_setOpen(MdkrDevToolId id, bool open);
/* Prints one `[TOOLTABLE] id=... title=... hotkey=...` line per tool. */
void DevTools_dumpTable(void);

#endif /* MDKR64_DEV_TOOLS_H */
```

- [ ] **Step 3: Implement the host** — a static table with all six ids present
  and `draw == NULL`, a `DevTools_draw()` that skips null entries, hotkey
  dispatch, and the dump. Wire `DevTools_draw()` into `platform/app/app_host.cpp`
  after the existing overlay draw, and add a `--dump-tool-table` argument path
  next to the existing schema-contract dump.

- [ ] **Step 4: Write the purity gate.** `tests/check_dev_tools_purity.py`:

1. Runs `--dump-tool-table` and parses `[TOOLTABLE]` rows; **fails on zero rows**.
2. For each row, runs two headless races with `MDKR_STATE_HASH=3`: one with
   `Tools.Enabled=0`, one with `Tools.Enabled=1` and that tool forced open via an
   env override.
3. Asserts the two `[SIMHASH]` streams are byte-identical.
4. Asserts the frame count and race outcome match, so a tool that merely slows
   the frame loop enough to change pacing is also caught.
5. Prints the number of tools verified.

- [ ] **Step 5: Run the gate. It must pass with all six tools inert.**

```bash
MDKR_AUDIO=0 python3 tests/check_dev_tools_purity.py
```

- [ ] **Step 6: Register in `CHECKS` and commit.**

```python
    Check("dev_tools_purity", "check_dev_tools_purity.py", "rom",
          "every registered developer tool leaves the authoritative state "
          "stream byte-identical"),
```

---

## Task 2: Crash screen

**Files:**
- Create: `platform/app/crash_screen.h`, `platform/app/crash_screen.cpp`,
  `tests/check_crash_screen.py`
- Modify: `platform/app/diag_log.cpp`, `platform/app/app_host.cpp`,
  `tools/run_checks.py`

**The constraint that matters:** the existing `[CRASH]` and `[FATAL]` stdout
markers are what every harness in `tests/` greps for. Changing, reordering, or
buffering them breaks the suite silently. The crash screen is **additive** and
runs only after those markers have been written and flushed.

- [ ] **Step 1: Write the failing gate.** `tests/check_crash_screen.py`:
  - runs the binary with a deliberate fault injected through an existing
    fault-injection env if one exists, or a new `MDKR_FAULT_TEST=abort` path
    added for this purpose and compiled in every configuration;
  - asserts the `[FATAL]` marker still appears on stdout, first, unchanged;
  - asserts a `[CRASHSCREEN]` line follows naming the fault kind, the tick, the
    track id, the renderer and the log path;
  - asserts the process exit code is unchanged from today's behaviour, so CI
    classification does not shift;
  - asserts that with `--headless-frames` the screen does **not** attempt to
    present, since there is no window.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Implement.** Install the handler after the existing fatal path
  writes and flushes its marker. Collect the fields from state you already have:
  the tick from the simulation scheduler, the track from the level globals, the
  renderer from `MDKR_RENDERER` resolution, the version from `AppVersion()`, the
  log path from `platform/app/diag_log.cpp`. Present with ImGui only when a
  window exists; otherwise print and exit.

- [ ] **Step 4: Run the gate. Verify it passes.**

- [ ] **Step 5: Run three harnesses that grep the markers** to prove nothing
  moved:

```bash
MDKR_AUDIO=0 python3 tests/check_runtime_safety.py
MDKR_AUDIO=0 python3 tests/check_race_drive.py
MDKR_AUDIO=0 python3 tests/check_array_bounds_sweep.py
```

- [ ] **Step 6: Register in `CHECKS` and commit.**

---

## Task 3: Command parser (pure, no UI)

**Files:**
- Create: `platform/dev_command.h`, `platform/dev_command.c`,
  `tests/test_dev_command.c`
- Modify: `cmake/tests.cmake`, `CMakeLists.txt`

**Interfaces:**
- Produces: `MdkrDevCommandResult`, `mdkr_dev_command_parse()`,
  `mdkr_dev_command_suggest()`. Task 4 renders the results.

- [ ] **Step 1: Write the failing test.** Assert:
  - `help` parses to the help verb with zero arguments;
  - `warp 12` parses with an integer argument, `warp abc` is rejected naming the
    argument, `warp` alone is rejected naming the missing argument;
  - `set Video.RenderScale 2.0` parses and resolves to a real `MdkrVideoKey`;
  - `set Not.A.Key 1` is **rejected**, and the rejection names the key — this is
    the assertion that keeps the console from becoming an arbitrary-write
    primitive;
  - `warpp 12` suggests `warp`;
  - a 4 KiB input line is rejected rather than overflowing;
  - an input containing a NUL byte mid-line is rejected.

- [ ] **Step 2: Register and run to verify it fails.**

- [ ] **Step 3: Implement.** Tokenise on whitespace with quoted-string support,
  match the verb against a static table, validate arity and argument types, and
  resolve `set`/`get` keys through the existing `video_config` name lookup. Never
  accept a key that is not in the schema.

- [ ] **Step 4: Run the test. Verify it passes.**

- [ ] **Step 5: Commit.**

---

## Task 4: Console and diagnostics windows

**Files:**
- Create: `platform/app/tool_console.h/.cpp`, `platform/app/tool_diagnostics.h/.cpp`
- Modify: `platform/app/dev_tools.cpp`, `CMakeLists.txt`

- [ ] **Step 1: Register both draw callbacks** and re-run the M1 purity gate.
  It must still pass — with `set` present, this is a real test, not a formality.

- [ ] **Step 2: Implement the diagnostics window** — tick, track id and name,
  camera position and pose, frame limit and measured present rate, the last 32
  entries from `platform/gameplay_event_trace.c`, and a copy-to-clipboard action
  producing a plain-text block suitable for pasting into an issue (US-1).

- [ ] **Step 3: Implement the console** on Task 3's parser, with scrollback,
  history, and tab completion from `mdkr_dev_command_suggest()`. Route output
  through `platform/app/diag_log.cpp` so console output also lands in the log
  file.

- [ ] **Step 4: Guard `warp`.** A warp changes authoritative state by design, so
  it must be refused while the purity gate's env marker is set, and it must print
  a one-line warning that the current run is no longer comparable to a clean one.

- [ ] **Step 5: Run the purity gate and the UI input gate.**

```bash
MDKR_AUDIO=0 python3 tests/check_dev_tools_purity.py
MDKR_AUDIO=0 python3 tests/check_app_ui_input.py
```

- [ ] **Step 6: Commit.**

---

## Task 5: Free camera

**Files:**
- Create: `platform/app/tool_freecam.h/.cpp`, `tests/check_tool_freecam.py`
- Modify: `platform/app/dev_tools.cpp`, `tools/run_checks.py`

**Where this must hook:** the camera obstruction work already established that
rendering consumes a projection record the finalizer latched rather than
computing its own lens (`game/src/camera_obstruction_runtime.c`,
`platform/camera_obstruction*.c`). The free camera substitutes **that record**,
at presentation depth, for exactly the same reason the obstruction resolver is
allowed to: substituting there cannot become moved authoritative state. Do not
hook the authored camera slots.

- [ ] **Step 1: Write the failing gate.** `tests/check_tool_freecam.py`:
  - detaches at tick T, moves the camera 500 units, re-attaches at T+300;
  - asserts the `[SIMHASH]` v3 stream over T..T+300 is byte-identical to an
    un-detached run;
  - asserts the frame at T+300 after re-attaching is **byte-identical** to the
    un-detached run's frame at T+300, proving the authored pose is restored
    exactly and not approximately;
  - asserts the frames during detachment differ, so the gate is not vacuous.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Implement.** Substitute the projection record while detached;
  restore by simply ceasing to substitute — do not save and re-apply a pose,
  which would introduce a rounding path.

- [ ] **Step 4: Run the gate and the camera suites.**

```bash
MDKR_AUDIO=0 python3 tests/check_tool_freecam.py
MDKR_AUDIO=0 python3 tests/check_camera_obstruction_runtime.py
MDKR_AUDIO=0 python3 tests/check_camera_obstruction_display_matrix.py
MDKR_AUDIO=0 python3 tests/check_dev_tools_purity.py
```

- [ ] **Step 5: Positive control** — make re-attachment restore a saved pose
  instead of ceasing substitution, confirm the byte-identical frame assertion
  fails, then restore the correct implementation.

- [ ] **Step 6: Register in `CHECKS` and commit.**

---

## Task 6: Collision and object viewers

**Files:**
- Create: `platform/app/tool_collision.h/.cpp`, `platform/app/tool_objects.h/.cpp`
- Modify: `platform/app/dev_tools.cpp`, `CMakeLists.txt`

- [ ] **Step 1: Source the collision overlay from the query itself.** Draw the
  triangles the physics query returns, not a second traversal of the level data.
  A viewer that re-derives geometry can agree with itself while disagreeing with
  the game, which is the failure mode that makes viewers worse than useless.
  `tests/check_collision_gridmask.py` and `platform/camera_obstruction_query.c`
  show where the candidate list is available.

- [ ] **Step 2: Show the candidate count and the cap.** Boss levels 41 and 54
  peak at 416 of 500 candidates; that headroom is a tracked watch metric in
  `docs/open-items/collision.md`. Surfacing it live turns a watch metric into
  something a contributor notices before it bites.

- [ ] **Step 3: Implement the object viewer** — behaviour id, position, flags,
  and particle status for each live entry, sourced from the same list
  `platform/sim_hash.c` v3 walks, so the viewer and the hash can never disagree
  about what is live.

- [ ] **Step 4: Run the purity gate and the collision suites.**

```bash
MDKR_AUDIO=0 python3 tests/check_dev_tools_purity.py
MDKR_AUDIO=0 python3 tests/check_collision_gridmask.py
MDKR_AUDIO=0 python3 tests/check_collision_untextured.py
MDKR_AUDIO=0 python3 tests/check_door_blocks.py
```

- [ ] **Step 5: Commit.**

---

## Task 7: Performance window

**Files:**
- Create: `platform/app/tool_performance.h/.cpp`
- Modify: `platform/app/dev_tools.cpp`

- [ ] **Step 1: Source the counters `check_pacing_quality.py` already reads.**
  Do not add new timing instrumentation; the M2/M3 pacing work already produced
  per-phase histograms.

- [ ] **Step 2: Label the synthetic-field caveat in the window itself.**
  Presentation and GPU counters are meaningless under `MDKR_SYNTH_FIELDS`; the
  window must say so on screen when that variable is set, because reading those
  numbers under synthetic fields has already misled measurement work on this
  project once.

- [ ] **Step 3: Run the purity gate and `check_pacing_quality.py`.**

- [ ] **Step 4: Commit.**

---

## Task 8: Documentation and full-suite verification

- [ ] **Step 1: Write `docs/DEV_TOOLS.md`** — one section per tool: what it
  shows, where the data comes from, the hotkey, and the gate that proves it
  inert. Add it to the `docs/README.md` index under "Subsystem guides".

- [ ] **Step 2: Add the bug-report route to `CONTRIBUTING.md`** — replace
  "include the command line, ROM version, renderer and crash output" with
  "open the diagnostics window and paste its copy block", keeping the existing
  requirements as the fallback for a crash before the window exists.

- [ ] **Step 3: Run the link checker.** `python3 tools/check_markdown_links.py`

- [ ] **Step 4: Run the complete suite, sequentially.**

```bash
rm -f save/eeprom.bin
MDKR_AUDIO=0 python3 tools/run_checks.py
```

- [ ] **Step 5: Paste the summary into the final commit body.**

---

## Self-review

**Spec coverage.** M1 → Task 1. M2 → Task 2. M3 → Tasks 3–4. M4 → Task 5.
M5 → Task 6. M6 → Task 7. US-1 → Task 4 Step 2. US-7 → Task 1's default-off key
plus the purity gate.

**Deliberately out of scope:**

- **A display-list viewer.** Valuable, but it needs a capture format for the
  F3DDKR command stream that does not exist yet; that is its own sprint.
- **A save editor.** `docs/SAVE_MANAGEMENT.md` and `tools/mdkr_save_cli.c`
  already cover this from outside the game, and an in-game editor that can write
  the progress file conflicts with S2's rule that save states never touch it.
- **A hook debugger.** There is no scripting or hook system to debug until S1's
  successor adds one.

**Type consistency.** `MdkrDevToolId` has six members and Task 1 Step 3 requires
all six present in the table from the start, so Tasks 4–7 register into existing
slots rather than extending the enum. `DevTools_dumpTable()` emits the
`[TOOLTABLE]` rows Task 1 Step 4's gate parses.
