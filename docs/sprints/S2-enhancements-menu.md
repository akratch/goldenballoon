# S2 — Enhancements, quality-of-life, and save states

> **Executing this plan:** work the tasks in order, one `- [ ]` step at a time.
> Tick a step only once its command has been run and its output read. The
> run-it-and-watch-it-fail steps are load-bearing; skipping ahead to the
> implementation forfeits the positive control that makes the gate mean anything.

**Goal:** Give players a real enhancements surface — speedometer, draw distance,
AI difficulty, cosmetics, and save states — behind a registry that makes every
enhancement declare, in code, whether it changes gameplay.

**Architecture:** One registry owns every enhancement: its key, its default, its
category, and — the load-bearing part — its **authority class**. A
`PRESENTATION` enhancement is provably invisible to `check_state_hash`; a
`GAMEPLAY` enhancement is allowed to change the state stream and is excluded
from parity gates by that declaration rather than by a hand-maintained list. The
UI is generated from the registry, so adding an enhancement is one table row plus
its effect, and the gate that proves presentation enhancements are inert is
generated from the same table and therefore cannot fall behind it.

**Tech stack:** C11 for the registry and effects, ImGui (`platform/app/`) for the
UI, existing `platform/video_config.c` schema machinery for persistence, existing
`platform/save_container.c` for the save-state container.

## Global constraints

- Every game or test invocation passes `--headless-frames N` and sets
  `MDKR_AUDIO=0`.
- Game-code changes go behind `#ifdef NATIVE_PORT`, with a `_Static_assert`
  wherever a struct layout or offset assumption is involved.
- New `MdkrVideoKey` entries are **appended**, never inserted.
- Every new `tests/check_*.py` is registered in `tools/run_checks.py`'s `CHECKS`.
  Every new `tests/test_*.c` is registered in `cmake/tests.cmake`.
- Positive controls are mandatory: for every enhancement, the gate must fail with
  the effect reverted.
- No ROM-derived data in any commit.
- Run `python3 tools/check_public_surface.py --staged` before every commit.

---

## User stories

**US-1 — See my speed.** As a player, I turn on a speedometer and see my current
speed during a race, so that I can learn how boosts and surfaces affect me.

**US-2 — See further.** As a player, I raise the draw distance and objects stop
popping in near the horizon, so that the track reads the way a modern game does.

**US-3 — A real challenge.** As a player who has beaten the game, I turn AI
difficulty up and opponents genuinely race harder, so that the game is worth
replaying.

**US-4 — Know what I am changing.** As a player, every enhancement tells me
plainly whether it changes how the game plays or only how it looks, so that I can
keep a purist run pure.

**US-5 — Practise a corner.** As a player, I press `F5` to save a state and `F7`
to load it, so that I can practise one corner fifty times without restarting the
race.

**US-6 — Not lose my real save.** As a player, save states are a separate thing
from my progress file and can never corrupt it, so that experimenting is safe.

**US-7 — Reset cleanly.** As a player, one button returns every enhancement to
its default, so that I can get back to a known state after experimenting.

---

## Milestones and acceptance criteria

### M1 — The registry and the authority contract

**Done when:**
- `platform/enhancement_registry.h/.c` defines every enhancement as a table row:
  key, label, help text, category, authority class (`PRESENTATION` or
  `GAMEPLAY`), type, default, range.
- Each row maps to an appended `MdkrVideoKey` so persistence, INI round-trip,
  env override and the existing settings machinery all work with no new code.
- `tests/check_enhancement_authority.py` iterates the registry programmatically
  and, for **every** row declared `PRESENTATION`, asserts the `[SIMHASH]` v3
  stream is byte-identical with the enhancement on and off. A new presentation
  enhancement is therefore gated the moment it is added, with no test edit.
- The same gate asserts every `GAMEPLAY` row **does** change the stream, so a
  mis-declared row fails in both directions.

### M2 — Presentation enhancements

**Done when:** speedometer, draw distance, LOD bias, and HUD scale all ship,
default to off/authored, and pass the M1 authority gate as `PRESENTATION`.

### M3 — Gameplay enhancements

**Done when:** AI difficulty ships with at least `authored` / `hard` /
`brutal` arms, is declared `GAMEPLAY`, is excluded from parity gates by that
declaration, and a race at `authored` is proven byte-identical to a race with the
enhancement compiled in but unset.

### M4 — Save states — **BLOCKED at the step-1 gate, 2026-08-09**

The container (Task 6) is shipped and gated. **Capture and restore are not
built**, because Task 7's own feasibility gate refuted this plan's premise. The
full measurement is in
[`../open-items/save.md`](../open-items/save.md#open-save-state-capture-is-blocked-by-payload-scope-not-by-pointer-tokens--wave-savestate).

In short: the pointer-token argument **holds** — a raw arena snapshot restores
correctly across six different ASLR bases, byte-identical, with no fix-ups. What
fails is payload *scope*. This plan asserted that `segment_consts.c` and the
`[SIMHASH]` v3 field list were "the existing inventory of authoritative state".
They yield about 20 globals. Measured by diffing the process image's writable
sections across a 600-tick window, the sufficient payload is the arena plus
**1,686 externally-linked globals, ~809 KB**. Arena-only does not diverge — it
segfaults, dereferencing a host global that still describes the post-restore
world.

The task was stopped rather than completed with a list fitted to the 142 symbols
one route happened to touch, which would pass a three-track gate and corrupt on
the first unmeasured route. The unblocking path — a per-TU span registry, 42 of
64 `game/src` TUs — is recorded in the open item.

**Was to be done when:**
- `F5` writes and `F7` reads a state; `Shift+F5`/`Shift+F7` select a slot 1–8.
- A save/load round trip inside a race produces a `[SIMHASH]` v3 stream
  identical to the un-interrupted run from the same tick forward.
- A state file records the ROM revision and the app version and refuses to load
  into a mismatched build with a named reason.
- Save states live in their own directory and are never written through
  `platform/save_container.c`'s progress path; `check_save_failsafe` still
  passes unchanged.
- Loading a corrupt or truncated state fails cleanly and leaves the running game
  untouched.

### M5 — Menu surface

**Done when:** Settings gains an Enhancements section grouped by category, each
row labelled with its authority class, with a "Reset enhancements" action that
restores defaults without touching presentation, audio, or input settings.

---

## File structure

**Create:**

| Path | Responsibility |
|---|---|
| `platform/enhancement_registry.h/.c` | The table, and lookup by key/index. |
| `platform/enh_speedometer.h/.c` | Speed readout: sampling and formatting only. |
| `platform/enh_draw_distance.h/.c` | Draw-distance and LOD-bias policy. |
| `platform/enh_ai_difficulty.h/.c` | AI difficulty scalars and their application point. |
| `platform/save_state.h/.c` | Snapshot container: capture, restore, validate. |
| `tests/test_enhancement_registry.c` | Table integrity: unique keys, valid ranges, every row reachable. |
| `tests/test_save_state_container.c` | Header/versioning/corruption unit test, no ROM. |
| `tests/check_enhancement_authority.py` | The generated authority gate. |
| `tests/check_enh_speedometer.py` | Pixel gate for the readout. |
| `tests/check_enh_draw_distance.py` | Pixel gate for the distance change. |
| `tests/check_enh_ai_difficulty.py` | Behavioural gate for the difficulty arms. |
| `tests/check_save_state_roundtrip.py` | The M4 determinism gate. |

**Modify:** `platform/video_config.h/.c` (appended keys + schema rows),
`platform/app/ui_settings.cpp` (Enhancements section),
`platform/app/app_ui_policy.cpp`, `platform/platform_sdl_min.c` (F5/F7),
`platform/fast3d/gfx_pc_dkr.c` (draw-distance cull point),
`cmake/tests.cmake`, `CMakeLists.txt`, `tools/run_checks.py`.

---

## Task 1: The enhancement registry

**Files:**
- Create: `platform/enhancement_registry.h`, `platform/enhancement_registry.c`,
  `tests/test_enhancement_registry.c`
- Modify: `cmake/tests.cmake`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `MdkrVideoKey` from `platform/video_config.h`.
- Produces: `MdkrEnhancement`, `MdkrEnhAuthority`, `mdkr_enhancement_count()`,
  `mdkr_enhancement_at()`, `mdkr_enhancement_for_key()`. Tasks 2–5 and the
  authority gate all read these.

- [ ] **Step 1: Write the failing test**

`tests/test_enhancement_registry.c`:

```c
#include "enhancement_registry.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void expect(int c, const char *w) {
    if (!c) { printf("FAIL %s\n", w); failures++; } else printf("ok   %s\n", w);
}

static void test_every_row_is_complete(void) {
    for (int i = 0; i < mdkr_enhancement_count(); i++) {
        const MdkrEnhancement *e = mdkr_enhancement_at(i);
        char what[128];
        snprintf(what, sizeof what, "row %d has a label", i);
        expect(e->label && e->label[0], what);
        snprintf(what, sizeof what, "row %d has help text", i);
        expect(e->help && e->help[0], what);
        snprintf(what, sizeof what, "row %d declares an authority class", i);
        expect(e->authority == MDKR_ENH_PRESENTATION ||
               e->authority == MDKR_ENH_GAMEPLAY, what);
    }
}

static void test_keys_are_unique(void) {
    for (int i = 0; i < mdkr_enhancement_count(); i++)
        for (int j = i + 1; j < mdkr_enhancement_count(); j++)
            expect(mdkr_enhancement_at(i)->key != mdkr_enhancement_at(j)->key,
                   "no two enhancements share a config key");
}

static void test_lookup_round_trips(void) {
    for (int i = 0; i < mdkr_enhancement_count(); i++) {
        const MdkrEnhancement *e = mdkr_enhancement_at(i);
        expect(mdkr_enhancement_for_key(e->key) == e,
               "for_key returns the same row at() did");
    }
    expect(mdkr_enhancement_for_key(MDKR_VIDEO_RENDER_SCALE) == NULL,
           "a non-enhancement key returns NULL rather than a neighbour");
}
```

- [ ] **Step 2: Register in `cmake/tests.cmake` and run to verify it fails**

Follow the `mdkr_display_config_test` block pattern. Sources:
`tests/test_enhancement_registry.c`, `platform/enhancement_registry.c`,
`platform/video_config.c`, `platform/config_ini.c`.

Run: `cmake --preset rel && cmake --build build-rel -j8 --target mdkr_enhancement_registry_test`
Expected: FAIL, `enhancement_registry.h: No such file`.

- [ ] **Step 3: Write the header**

```c
/* enhancement_registry.h — every optional player-facing behaviour, in one table.
 *
 * The AUTHORITY CLASS is the point of this module. An enhancement that claims
 * PRESENTATION is asserted, by a gate generated from this table, to leave the
 * authoritative [SIMHASH] v3 stream byte-identical. An enhancement that claims
 * GAMEPLAY is asserted to change it. Neither claim is checked by hand, so the
 * table cannot drift away from what the gates test.
 */
#ifndef MDKR64_ENHANCEMENT_REGISTRY_H
#define MDKR64_ENHANCEMENT_REGISTRY_H

#include "video_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdkrEnhAuthority {
    /* Cannot move authoritative state. Proven, not asserted. */
    MDKR_ENH_PRESENTATION = 0,
    /* Deliberately changes how the game plays. Excluded from parity gates
     * BY THIS DECLARATION, which is why it is not a comment. */
    MDKR_ENH_GAMEPLAY
} MdkrEnhAuthority;

typedef enum MdkrEnhCategory {
    MDKR_ENH_CAT_DISPLAY = 0,
    MDKR_ENH_CAT_DIFFICULTY,
    MDKR_ENH_CAT_COSMETIC
} MdkrEnhCategory;

typedef struct MdkrEnhancement {
    MdkrVideoKey     key;       /* persistence, env override, INI round-trip */
    const char      *label;     /* player-facing, no process vocabulary */
    const char      *help;      /* one sentence, player-facing */
    MdkrEnhAuthority authority;
    MdkrEnhCategory  category;
} MdkrEnhancement;

int                     mdkr_enhancement_count(void);
const MdkrEnhancement  *mdkr_enhancement_at(int index);
/* NULL when `key` is not an enhancement. */
const MdkrEnhancement  *mdkr_enhancement_for_key(MdkrVideoKey key);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ENHANCEMENT_REGISTRY_H */
```

- [ ] **Step 4: Implement with an empty table**, then append the four keys below
  to `MdkrVideoKey` in `platform/video_config.h` and add their schema rows to
  `platform/video_config.c` (both the descriptor table and the default/range
  tables — `tests/test_video_config.c` asserts every key has an entry in each):

```c
    MDKR_ENH_SPEEDOMETER,      /* int 0..2  — off / mph / km/h        */
    MDKR_ENH_DRAW_DISTANCE,    /* int 100..400 — percent of authored  */
    MDKR_ENH_LOD_BIAS,         /* int 0..2  — authored / high / max   */
    MDKR_ENH_AI_DIFFICULTY,    /* string    — authored / hard / brutal */
```

Then populate the registry table with the four rows, the first three
`MDKR_ENH_PRESENTATION`, the last `MDKR_ENH_GAMEPLAY`.

- [ ] **Step 5: Run the unit tests**

```bash
cmake --build build-rel -j8 && \
  ctest --test-dir build-rel -R 'enhancement_registry|video_config' --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add platform/enhancement_registry.h platform/enhancement_registry.c \
        platform/video_config.h platform/video_config.c \
        tests/test_enhancement_registry.c cmake/tests.cmake CMakeLists.txt
python3 tools/check_public_surface.py --staged
git commit -m "feat: declare every enhancement and its authority class in one table"
```

---

## Task 2: The generated authority gate

**Files:**
- Create: `tests/check_enhancement_authority.py`
- Modify: `platform/app/ui_settings.cpp` (extend the existing
  `Settings_dumpSchemaContract()` to also dump the enhancement table),
  `tools/run_checks.py`

This gate is written **before** any enhancement has an effect, so every
enhancement added afterwards is born gated.

- [ ] **Step 1: Expose the table to the harness.** Extend
  `Settings_dumpSchemaContract()` in `platform/app/ui_settings.cpp` to print one
  line per enhancement:

```
[ENHTABLE] key=Enhancements.Speedometer authority=presentation category=display
```

- [ ] **Step 2: Write the gate.** `tests/check_enhancement_authority.py`:

1. Runs the binary once with the existing schema-contract dump flag and parses
   every `[ENHTABLE]` row. It must **fail if zero rows are parsed** — a gate that
   silently iterates an empty list is the classic vacuous pass.
2. For each row, runs two headless races with `MDKR_STATE_HASH=3`: one with the
   key at its default, one with it set to a non-default value the schema permits.
3. `presentation` rows: assert the two `[SIMHASH]` streams are byte-identical.
4. `gameplay` rows: assert they differ.
5. Prints the row count it verified, so a shrinking table is visible in the log.

Read `tests/check_determinism.py` first for the established way this repository
captures and compares a `[SIMHASH]` stream; reuse its helper rather than writing
a second stream parser.

- [ ] **Step 3: Run it — it must pass on the empty-effect table.**

```bash
MDKR_AUDIO=0 python3 tests/check_enhancement_authority.py
```

At this point every enhancement is inert, so all four rows produce identical
streams. The `gameplay` arm for `MDKR_ENH_AI_DIFFICULTY` will therefore **fail** —
which is correct, and is the failing state Task 5 closes. Record that expected
failure in the gate's docstring and mark the row `xfail` with an explicit
`EXPECTED_INERT_UNTIL = "S2 Task 5"` constant, so the gate is honest rather than
disabled.

- [ ] **Step 4: Register in `CHECKS` and commit.**

```python
    Check("enhancement_authority", "check_enhancement_authority.py", "rom",
          "every enhancement's declared authority class matches its measured "
          "effect on the authoritative state stream"),
```

---

## Task 3: Speedometer

**Files:**
- Create: `platform/enh_speedometer.h`, `platform/enh_speedometer.c`,
  `tests/check_enh_speedometer.py`
- Modify: `platform/fast3d/gfx_pc_dkr.c` or the existing HUD draw path,
  `CMakeLists.txt`, `tools/run_checks.py`

- [ ] **Step 1: Write the failing pixel gate.** `tests/check_enh_speedometer.py`
  asserts, using the frame-capture helper `tests/check_native_ui_resolution.py`
  already uses:
  - with `Enhancements.Speedometer=0`, the frame is byte-identical to the
    baseline capture;
  - with `=1`, a bounded rectangle in the expected HUD corner differs;
  - the rest of the frame outside that rectangle is byte-identical, proving the
    readout does not disturb the existing HUD;
  - the displayed number is monotonically non-decreasing over the first second of
    a standing start, read from a `[SPEEDO]` diagnostic line rather than OCR.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Implement.** `mdkr_enh_speedometer_sample()` reads the local
  racer's speed from the same field the HUD already reads for other purposes —
  find it by grepping the existing HUD code rather than reaching into
  `game/src/racer*.c` directly, and do not add a `game/` edit for this.
  Format with the existing SDF text path (`platform/fast3d/gfx_font_sdf.c`) so
  the readout matches the Remastered interface rather than introducing a second
  text renderer.

- [ ] **Step 4: Run the gate. Verify it passes.**

- [ ] **Step 5: Positive control** — force `mdkr_enh_speedometer_sample()` to
  return a constant, rebuild, confirm the monotonicity assertion fails, restore.

- [ ] **Step 6: Run `check_native_ui_resolution.py` and
  `check_enhancement_authority.py`.** Both must pass.

- [ ] **Step 7: Register in `CHECKS` and commit.**

---

## Task 4: Draw distance and LOD bias

**Files:**
- Create: `platform/enh_draw_distance.h`, `platform/enh_draw_distance.c`,
  `tests/check_enh_draw_distance.py`
- Modify: the object cull site in the render path, `CMakeLists.txt`,
  `tools/run_checks.py`

The correctness risk here is specific and worth stating: DKR's draw distance is a
**render-side cull**, and moving it must not move object *updates*. If raising
draw distance causes more objects to tick, the enhancement is `GAMEPLAY`, not
`PRESENTATION`, and Task 2's gate will say so. That gate failing is the signal
that the hook is in the wrong place — do not reclassify the row to make it pass.

- [ ] **Step 1: Write the failing gate.** Assert that at 400% the captured frame
  differs from 100%, that the `[SIMHASH]` v3 stream is identical between them,
  and that the live object count reported per tick is identical between them.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Find the cull site mechanically**, not by reading: build with
  `-DMDKR_TRACE` and run `MDKR_TRACE=1` over a race, or grep the render path for
  the existing distance comparison. Record in the commit body which site you
  chose and how you established it is render-side only.

- [ ] **Step 4: Implement** as a multiplier on the existing threshold, clamped to
  the schema's 100..400 range, read once per frame rather than per object.

- [ ] **Step 5: Run the gate and the authority gate. Both must pass.**

- [ ] **Step 6: Positive control** — set the multiplier to 1.0 unconditionally,
  confirm the "frames differ" assertion fails, restore.

- [ ] **Step 7: Sweep the class.** Draw distance is one of several authored
  distance constants. Grep the render path for every comparison against a
  distance threshold, list them in the commit body, and state for each whether it
  is render-side or update-side. This is the CONTRIBUTING §6 obligation and it is
  not optional.

- [ ] **Step 8: Register in `CHECKS` and commit.**

---

## Task 5: AI difficulty

**Files:**
- Create: `platform/enh_ai_difficulty.h`, `platform/enh_ai_difficulty.c`,
  `tests/check_enh_ai_difficulty.py`
- Modify: the AI update path under `game/src/` behind `#ifdef NATIVE_PORT`,
  `CMakeLists.txt`, `tools/run_checks.py`

This is the sprint's only `game/` edit. Keep it to a single multiplier read at
one site.

- [ ] **Step 1: Write the failing gate.** Over 8 seeded races per arm:
  - `authored` produces a `[SIMHASH]` v3 stream byte-identical to a build
    without the enhancement compiled in at all (the purity assertion);
  - `hard` and `brutal` each finish the race with a strictly better mean
    opponent finish position than `authored`;
  - no arm produces a wedged opponent — reuse the recovery assertion from
    `tests/check_ai_unstick_opponents.py` rather than writing a second one;
  - lap times remain physically plausible: no arm finishes below a floor derived
    from the `authored` best lap.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Implement.** Add the multiplier read at the single AI speed or
  rubber-band site, wrapped:

```c
#ifdef NATIVE_PORT
    speed_target *= mdkr_enh_ai_difficulty_scale();
#endif
```

`mdkr_enh_ai_difficulty_scale()` returns exactly `1.0f` for `authored`, and the
function must be written so that the `authored` path is bit-identical to the
pre-change expression — not "multiplied by 1.0", which is not always a no-op for
floats near the denormal boundary. Return early instead.

- [ ] **Step 4: Run the gate. Verify it passes.**

- [ ] **Step 5: Remove the `EXPECTED_INERT_UNTIL` marker** from
  `tests/check_enhancement_authority.py` and re-run it. The `gameplay` arm must
  now pass on its own terms.

- [ ] **Step 6: Run the gameplay regressions.**

```bash
MDKR_AUDIO=0 python3 tests/check_race_drive.py
MDKR_AUDIO=0 python3 tests/check_race_finish_time.py
MDKR_AUDIO=0 python3 tests/check_ai_unstick_opponents.py
MDKR_AUDIO=0 python3 tests/check_determinism.py
```

- [ ] **Step 7: Register in `CHECKS` and commit.**

---

## Task 6: Save-state container (no capture yet)

**Files:**
- Create: `platform/save_state.h`, `platform/save_state.c`,
  `tests/test_save_state_container.c`
- Modify: `cmake/tests.cmake`, `CMakeLists.txt`

**Interfaces:**
- Produces: `MdkrSaveStateHeader`, `mdkr_save_state_write()`,
  `mdkr_save_state_read()`, `mdkr_save_state_validate()`. Task 7 consumes all
  three.

- [ ] **Step 1: Write the failing test.** No ROM, no window. Assert:
  - a written header round-trips every field;
  - a state whose magic is wrong is refused with a reason containing `format`;
  - a state whose ROM revision differs is refused with a reason naming both
    revisions;
  - a state whose app version differs is refused with a reason naming both
    versions;
  - a state truncated at every byte offset from 1 to `sizeof(header)+16` is
    refused cleanly at each one — loop the offsets, do not spot-check;
  - a state whose payload CRC does not match is refused.

- [ ] **Step 2: Register and run to verify it fails.**

- [ ] **Step 3: Implement the header**

```c
/* save_state.h — practice-oriented state snapshots.
 *
 * DELIBERATELY NOT the progress save. Progress lives in
 * platform/save_container.c and its EEPROM image; a save state is a separate
 * file in a separate directory with a separate format, and no code path lets
 * one become the other. check_save_failsafe must keep passing untouched.
 */
#ifndef MDKR64_SAVE_STATE_H
#define MDKR64_SAVE_STATE_H

#include <stddef.h>
#include <stdint.h>

#define MDKR_SAVE_STATE_MAGIC   0x4D444B53u /* 'MDKS' */
#define MDKR_SAVE_STATE_VERSION 1u

typedef struct MdkrSaveStateHeader {
    uint32_t magic;
    uint32_t format_version;
    char     rom_revision[16];  /* e.g. "us.v80" */
    char     app_version[32];   /* MDKR_APP_VERSION_STR at capture */
    uint64_t tick;              /* authoritative tick at capture */
    uint32_t payload_bytes;
    uint32_t payload_crc32;
} MdkrSaveStateHeader;

/* 0 on success; non-zero with a one-line reason in `err`. */
int mdkr_save_state_write(const char *path, const MdkrSaveStateHeader *header,
                          const void *payload, char *err, size_t err_size);
int mdkr_save_state_read(const char *path, MdkrSaveStateHeader *out_header,
                         void *payload, size_t payload_cap,
                         char *err, size_t err_size);
int mdkr_save_state_validate(const MdkrSaveStateHeader *header,
                             const char *rom_revision, const char *app_version,
                             char *err, size_t err_size);

#endif /* MDKR64_SAVE_STATE_H */
```

- [ ] **Step 4: Run the unit test. Verify it passes.**

- [ ] **Step 5: Run `check_save_failsafe.py`** and confirm it is untouched.

- [ ] **Step 6: Commit.**

---

## Task 7: Save-state capture and restore

**Files:**
- Modify: `platform/save_state.c`, `platform/platform_sdl_min.c`
- Create: `tests/check_save_state_roundtrip.py`
- Modify: `tools/run_checks.py`

**The feasibility argument, and how to check it before building:** this engine
stores intra-structure pointers as 32-bit arena tokens
(`mdkr_arena_token_from_host` in `platform/stubs_dkr.c`, and the pointer-token
decision in `docs/ARCHITECTURE_DECISIONS.md`), which is what makes a raw arena
snapshot restorable at a different host base address. Step 1 proves that claim
before any UI exists. If it fails, stop and escalate — do not add pointer
fix-ups, which would be a new invariant this project has not paid for.

- [ ] **Step 1: Prove restorability first.** Write a throwaway harness that, at a
  fixed tick, memcpy's the arena to a heap buffer, continues 600 ticks recording
  the `[SIMHASH]` v3 stream, then copies the buffer back and re-runs the same 600
  ticks. Assert the two streams are byte-identical. Paste the result. **If the
  streams differ, this task stops here** and the finding goes to
  `docs/open-items/` with the measured divergence tick.

- [ ] **Step 2: Write the failing gate.** `tests/check_save_state_roundtrip.py`:
  - saves at tick T, plays to T+600, loads, plays to T+600 again, and asserts the
    second `[SIMHASH]` v3 segment equals the first;
  - repeats across at least three tracks and two game modes, because a
    mode-specific allocation would otherwise pass on one route;
  - asserts loading a state captured with `Enhancements.AIDifficulty=brutal`
    into a session set to `authored` is **refused**, not silently applied;
  - asserts a corrupt state leaves the running race unchanged — the state hash
    after a failed load equals the hash before it.

- [ ] **Step 3: Run it, verify it fails.**

- [ ] **Step 4: Implement capture and restore** on top of Task 6's container,
  with the payload being the arena span plus the explicitly enumerated globals
  that live outside it. Enumerate those globals from
  `platform/segment_consts.c` and the `[SIMHASH]` v3 field list in
  `platform/sim_hash.c` — those two are the existing inventory of authoritative
  state, and anything they read must be in the payload.

- [ ] **Step 5: Bind the keys** in `platform/platform_sdl_min.c`: `F5` save,
  `F7` load, `Shift+F5`/`Shift+F7` cycle slot, with an on-screen confirmation
  line. Suppress all four while the overlay has focus.

- [ ] **Step 6: Run the gate. Verify it passes.**

- [ ] **Step 7: Positive control** — omit one enumerated global from the payload,
  rebuild, confirm the round-trip gate fails, restore. This is the assertion that
  the payload is complete rather than merely large.

- [ ] **Step 8: Run the full determinism and save suites.**

```bash
MDKR_AUDIO=0 python3 tests/check_determinism.py
MDKR_AUDIO=0 python3 tests/check_state_hash.py
MDKR_AUDIO=0 python3 tests/check_save_failsafe.py
MDKR_AUDIO=0 python3 tests/check_ghost_matrix.py
```

- [ ] **Step 9: Register in `CHECKS` and commit.**

---

## Task 8: The Enhancements menu

**Files:**
- Modify: `platform/app/ui_settings.cpp`, `platform/app/app_ui_policy.cpp`
- Test: extend `tests/test_app_ui_policy.cpp`, `tests/check_app_ui_input.py`

- [ ] **Step 1: Extend `tests/test_app_ui_policy.cpp`** to assert every
  enhancement key is visible in the Enhancements section and that no enhancement
  key leaks into the Presentation or Fidelity sections.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Implement the section** — iterate `mdkr_enhancement_at()`,
  group by `category`, and render each row with the existing `drawKey()` helper.
  Append the authority class to each row's help text as plain player-facing
  wording: `"Changes how the game plays."` or `"Changes only how the game looks."`
  Do not print the enum name.

- [ ] **Step 4: Add "Reset enhancements"** — restores only enhancement keys to
  their schema defaults, leaving presentation, audio and input untouched. Assert
  that scoping in the UI-policy test.

- [ ] **Step 5: Run the UI tests.**

```bash
ctest --test-dir build-rel -R 'app_ui_policy' --output-on-failure
MDKR_AUDIO=0 python3 tests/check_app_ui_input.py
MDKR_AUDIO=0 python3 tests/check_restart_apply.py
```

- [ ] **Step 6: Commit.**

---

## Task 9: Documentation and full-suite verification

- [ ] **Step 1: Document the enhancements** in a new
  `docs/ENHANCEMENTS.md` — one row per enhancement with its default, range,
  authority class, and the gate that proves the class. Add it to the
  `docs/README.md` index.

- [ ] **Step 2: Add a player-facing section to `README.md`** — plain language,
  no gate vocabulary, and an explicit sentence that enhancements marked as
  changing gameplay are off by default.

- [ ] **Step 3: Run the link checker.** `python3 tools/check_markdown_links.py`

- [ ] **Step 4: Run the complete suite, sequentially.**

```bash
rm -f save/eeprom.bin
MDKR_AUDIO=0 python3 tools/run_checks.py
```

- [ ] **Step 5: Paste the summary into the final commit body.**

---

## Self-review

**Spec coverage.** M1 → Tasks 1–2. M2 → Tasks 3–4. M3 → Task 5. M4 → Tasks 6–7.
M5 → Task 8. US-4 → Task 1's authority class plus Task 8 Step 3. US-6 → Task 6's
separate container and the retained `check_save_failsafe`. US-7 → Task 8 Step 4.

**Deliberately out of scope**, to be raised as open items rather than dropped:

- **Cosmetics (kart and character colour).** The registry's `COSMETIC` category
  exists and is empty. Adding colours needs a palette-override path that S1's
  texture pipeline may supersede entirely; building both would be waste.
- **Randomizer.** Order-of-magnitude larger than this sprint and depends on the
  campaign graph being fully enumerable, which S9 touches but does not finish.
- **Browser save states.** The wasm build's memory model needs its own
  feasibility proof equivalent to Task 7 Step 1.

**Type consistency.** `MdkrEnhAuthority` and `MdkrEnhCategory` are defined in
Task 1 and used in Tasks 2 and 8. `mdkr_enh_ai_difficulty_scale()` is introduced
in Task 5 Step 3 and referenced nowhere earlier. `MdkrSaveStateHeader` is defined
in Task 6 and consumed in Task 7. The `EXPECTED_INERT_UNTIL` marker is set in
Task 2 Step 3 and removed in Task 5 Step 5.
