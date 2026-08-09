# S8 — Accessibility: speech output and a real accessibility surface

> **Executing this plan:** work the tasks in order, one `- [ ]` step at a time.
> Tick a step only once its command has been run and its output read. The
> run-it-and-watch-it-fail steps are load-bearing; skipping ahead to the
> implementation forfeits the positive control that makes the gate mean anything.

**Goal:** Let a blind or low-vision player set the game up and race it — the app
shell speaks what is focused, the race speaks what is happening, and every
existing accessibility option is gathered into one place a player can find.

**Architecture:** ImGui draws to a canvas and exposes no native accessibility
tree, so a screen reader has nothing to read. The realistic path — and the one
Ship of Harkinian took — is **self-voicing**: the app maintains its own semantic
model of what is focused and speaks it through the platform's speech API. This
sprint builds that model as a separate, testable layer that emits text, then
attaches a platform speech backend to it. The model is gated by asserting the
emitted text, which needs no audio device and works in CI.

**Tech stack:** C11 for the model, `AVSpeechSynthesizer` on macOS, SAPI 5 /
`ISpVoice` on Windows, `speech-dispatcher` via `dlopen` on Linux, the Web Speech
API in the browser build.

## Global constraints

- Every game or test invocation passes `--headless-frames N` and sets
  `MDKR_AUDIO=0`. Speech must be routed so that `MDKR_AUDIO=0` **also** silences
  it — a test run that starts talking is the same failure the audio rule exists
  to prevent.
- The semantic model emits text and is tested on that text. Do not gate on
  audible output.
- Speech must never block a frame. A synthesiser call on the render thread turns
  a slow voice engine into a stutter.
- New `MdkrVideoKey` entries are appended, never inserted.
- Do not claim screen-reader compatibility in `README.md` until Task 6's route
  has been executed with a real screen reader. Self-voicing is not the same
  claim, and conflating them would be the exact kind of unearned assertion this
  project does not make.
- Run `python3 tools/check_public_surface.py --staged` before every commit.

---

## User stories

**US-1 — Set the game up.** As a blind player, every control I focus in the
launcher and settings is spoken with its name, current value, and what it does,
so that I can configure the game without sight.

**US-2 — Know where I am.** As a blind player, moving between sections announces
the section, so that I can build a mental model of the menu.

**US-3 — Race.** As a blind player, my position, lap, and race events are spoken
as they happen, so that I can follow a race.

**US-4 — Control the voice.** As a player using speech, I set its rate and
volume and turn categories on and off, so that it is useful rather than
overwhelming.

**US-5 — Find the options.** As a player with any access need, one Accessibility
section holds reduced motion, contrast, UI scale, and speech, so that I am not
hunting through four panels.

**US-6 — Not hear it by accident.** As a player who does not use speech, it is
off, silent, and costs nothing.

---

## Milestones and acceptance criteria

### M1 — The semantic model

**Done when:**
- `platform/a11y_model.h/.c` accepts focus, value, section and event
  announcements and produces a single ordered text stream with a priority and a
  category on each utterance.
- Interruption rules are defined and tested: a focus change cancels a pending
  focus utterance but not an in-flight race-critical one.
- `tests/test_a11y_model.c` covers ordering, coalescing, interruption, and the
  category filter, with no audio and no window.
- Every utterance is emitted to stdout as `[SPEAK] cat=… pri=… text=…` when
  `MDKR_A11Y_TRACE=1`, which is how every later gate asserts behaviour.

### M2 — Shell self-voicing

**Done when:** every focusable control in the launcher, settings, and overlay
produces an utterance naming the control, its value, and its help; section
changes announce the section; and `tests/check_a11y_shell.py` walks the entire UI
by keyboard and asserts an utterance for every reachable control, failing if any
control is silent.

### M3 — Speech backends

**Done when:** macOS, Windows and Linux each speak the model's stream off the
render thread, with a queue bounded so a slow engine drops old utterances rather
than growing without limit; the browser build uses the Web Speech API; and a
platform with no engine degrades to text-only with a stated reason rather than
failing.

### M4 — In-race announcements

**Done when:** position changes, lap changes, final lap, race start and finish,
and item pickup are announced; each category is individually toggleable; and the
announcements are proven presentation-only by a `[SIMHASH]` v3 comparison.

### M5 — The Accessibility section

**Done when:** one settings section gathers speech, reduced motion
(`Camera.Comfort`), contrast, and UI scale, and every option in it is itself
reachable and spoken.

### M6 — Verified with a real screen reader

**Done when:** the route in `docs/ACCESSIBILITY.md` has been executed with
VoiceOver and with NVDA, results recorded, and `README.md`'s known-limitations
line updated to state exactly what is and is not supported.

---

## File structure

**Create:**

| Path | Responsibility |
|---|---|
| `platform/a11y_model.h/.c` | Utterance queue, priority, coalescing, filtering. |
| `platform/a11y_speech.h` | Backend interface. |
| `platform/a11y_speech_mac.mm` | AVSpeechSynthesizer backend. |
| `platform/a11y_speech_win.cpp` | SAPI backend. |
| `platform/a11y_speech_linux.c` | speech-dispatcher backend, `dlopen`'d. |
| `platform/a11y_speech_web.c` | Web Speech API bridge. |
| `platform/a11y_speech_null.c` | Text-only fallback. |
| `platform/a11y_race.h/.c` | Race event → utterance mapping. |
| `tests/test_a11y_model.c` | Model unit test. |
| `tests/check_a11y_shell.py` | Every-control-is-spoken gate. |
| `tests/check_a11y_race.py` | Race announcement + purity gate. |
| `docs/ACCESSIBILITY.md` | What is supported, and the verification route. |

**Modify:** `platform/video_config.h/.c`, `platform/app/ui_settings.cpp`,
`platform/app/ui_launcher.cpp`, `platform/app/ui_overlay.cpp`,
`platform/app/ui_common.cpp`, `cmake/tests.cmake`, `CMakeLists.txt`,
`tools/run_checks.py`, `README.md`, `docs/APP_SHELL.md`.

---

## Task 1: The semantic model

**Files:**
- Create: `platform/a11y_model.h`, `platform/a11y_model.c`,
  `tests/test_a11y_model.c`
- Modify: `cmake/tests.cmake`, `CMakeLists.txt`

**Interfaces:**
- Produces: `MdkrA11yCategory`, `MdkrA11yPriority`, `mdkr_a11y_announce()`,
  `mdkr_a11y_focus()`, `mdkr_a11y_next()`, `mdkr_a11y_set_category_enabled()`.
  Tasks 2–4 all emit through these.

- [ ] **Step 1: Write the failing test.** `tests/test_a11y_model.c`:
  - two focus announcements in a row yield only the second — a player tabbing
    quickly must not hear a backlog;
  - a race-critical announcement in flight is **not** cancelled by a focus
    change;
  - a disabled category produces no utterance;
  - the queue is bounded: pushing 1,000 utterances leaves a bounded count and
    drops the **oldest**, not the newest;
  - an utterance longer than the buffer is rejected rather than truncated
    mid-word;
  - `mdkr_a11y_next()` on an empty queue returns 0 and does not write to `out`.

- [ ] **Step 2: Register and run to verify it fails.**

- [ ] **Step 3: Write the header**

```c
/* a11y_model.h — what the app would say, independent of any voice.
 *
 * ImGui exposes no accessibility tree, so this layer IS the accessibility tree:
 * the app tells it what is focused and what happened, and it produces an ordered
 * text stream. Keeping it free of any speech API is what lets the whole
 * behaviour be gated on text, in CI, with no audio device.
 */
#ifndef MDKR64_A11Y_MODEL_H
#define MDKR64_A11Y_MODEL_H

#include <stdbool.h>
#include <stddef.h>

#define MDKR_A11Y_TEXT_MAX 512

typedef enum MdkrA11yCategory {
    MDKR_A11Y_CAT_FOCUS = 0,   /* the control under the cursor */
    MDKR_A11Y_CAT_SECTION,     /* moving between panels */
    MDKR_A11Y_CAT_STATUS,      /* settings applied, errors */
    MDKR_A11Y_CAT_RACE_POSITION,
    MDKR_A11Y_CAT_RACE_LAP,
    MDKR_A11Y_CAT_RACE_EVENT,
    MDKR_A11Y_CAT_COUNT
} MdkrA11yCategory;

typedef enum MdkrA11yPriority {
    MDKR_A11Y_PRI_LOW = 0,     /* superseded by anything newer in its category */
    MDKR_A11Y_PRI_NORMAL,
    MDKR_A11Y_PRI_CRITICAL     /* never cancelled once in flight */
} MdkrA11yPriority;

void mdkr_a11y_init(void);
void mdkr_a11y_announce(MdkrA11yCategory category, MdkrA11yPriority priority,
                        const char *text);
/* Convenience for the commonest case: name, value, help -> one focus utterance. */
void mdkr_a11y_focus(const char *name, const char *value, const char *help);

/* Pops the next utterance. Returns 1 and fills `out`, or 0 when empty. */
int  mdkr_a11y_next(char *out, size_t out_size, MdkrA11yCategory *out_category);

void mdkr_a11y_set_category_enabled(MdkrA11yCategory category, bool enabled);
bool mdkr_a11y_category_enabled(MdkrA11yCategory category);
/* Emits `[SPEAK] cat=... pri=... text=...` when MDKR_A11Y_TRACE=1. */
void mdkr_a11y_set_trace(bool on);

#endif /* MDKR64_A11Y_MODEL_H */
```

- [ ] **Step 4: Implement.** A fixed-capacity ring, coalescing per category for
  `LOW`/`NORMAL`, an in-flight flag that `CRITICAL` sets, and the trace emission.
  No allocation, no threads — the backends in Task 3 own the threading.

- [ ] **Step 5: Run the test. Verify it passes.**

- [ ] **Step 6: Commit.**

---

## Task 2: Shell self-voicing

**Files:**
- Create: `tests/check_a11y_shell.py`
- Modify: `platform/app/ui_common.cpp`, `platform/app/ui_settings.cpp`,
  `platform/app/ui_launcher.cpp`, `platform/app/ui_overlay.cpp`,
  `platform/video_config.h/.c`, `tools/run_checks.py`

- [ ] **Step 1: Append the keys and their schema rows.**

```c
    MDKR_A11Y_SPEECH,          /* int 0..1  */
    MDKR_A11Y_SPEECH_RATE,     /* int 50..300, percent of normal */
    MDKR_A11Y_SPEECH_VOLUME,   /* int 0..100 */
    MDKR_A11Y_SPEECH_RACE,     /* int 0..1, race announcements */
```

```c
    [MDKR_A11Y_SPEECH] = {
        "Accessibility.Speech", "MDKR_A11Y_SPEECH",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 1.0f,
        "Speak menus",
        "Read out the control you are on, its setting, and what it does.",
        MDKR_VIDEO_CAT_INTERFACE
    },
```

Add the remaining three rows in the same shape, plus the default/range/text table
entries each key needs.

- [ ] **Step 2: Write the failing gate.** `tests/check_a11y_shell.py`, with
  `MDKR_A11Y_TRACE=1` and `MDKR_AUDIO=0`:
  - drives `Tab` and arrow keys through the launcher, every settings section, and
    the in-game overlay, using the input-injection route
    `tests/check_app_ui_input.py` already establishes;
  - collects every `[SPEAK]` line;
  - asserts **every** focusable control produced at least one utterance —
    enumerate the controls from the schema dump
    (`Settings_dumpSchemaContract()`), so a newly added setting that nobody
    voiced fails this gate automatically;
  - asserts each focus utterance contains the control's name and its current
    value;
  - asserts moving between sections produced a `cat=section` utterance;
  - fails if zero `[SPEAK]` lines were collected.

- [ ] **Step 3: Run it, verify it fails.**

- [ ] **Step 4: Emit from one place.** Add the `mdkr_a11y_focus()` call inside
  the shared row helper in `platform/app/ui_common.cpp` and the `drawKey()` path
  in `ui_settings.cpp`, not at each call site. Per-site emission is how a control
  ends up silent.

- [ ] **Step 5: Run the gate. Verify it passes.**

- [ ] **Step 6: Positive control** — remove the emission from the shared helper,
  rebuild, confirm the gate fails naming the silent controls, restore.

- [ ] **Step 7: Register in `CHECKS` and commit.**

---

## Task 3: Speech backends

**Files:**
- Create: `platform/a11y_speech.h`, `platform/a11y_speech_mac.mm`,
  `platform/a11y_speech_win.cpp`, `platform/a11y_speech_linux.c`,
  `platform/a11y_speech_web.c`, `platform/a11y_speech_null.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Define the backend interface** — `mdkr_a11y_speech_init()`,
  `_speak(text)`, `_stop()`, `_shutdown()`, `_available()`. One backend compiles
  per platform, exactly as `platform/app/file_dialog_*.{cpp,mm}` already selects
  per platform; follow that pattern rather than inventing a second one.

- [ ] **Step 2: Drain off the render thread.** A worker pulls from
  `mdkr_a11y_next()` and calls `_speak()`. The model is not thread-safe by
  design, so guard the pop with the same single-owner discipline the rest of the
  port uses: only the worker pops, only the UI pushes, and the ring's indices are
  the sole shared state.

- [ ] **Step 3: Honour `MDKR_AUDIO=0`.** `_init()` must refuse and report
  text-only when audio is disabled. Verify by running an existing check with
  `MDKR_A11Y_SPEECH=1` and confirming silence.

- [ ] **Step 4: Load Linux speech-dispatcher with `dlopen`**, so the tarball
  still runs on a system without it, reporting text-only when absent.

- [ ] **Step 5: Bound the queue in the backend too.** If the engine is slower
  than the utterance rate, drop oldest. A player who tabs through twenty settings
  should hear the twentieth, not the first.

- [ ] **Step 6: Verify each backend on the platforms available**, and record
  which were verified and which were not in `docs/ACCESSIBILITY.md`. Do not mark
  an unverified backend as working.

- [ ] **Step 7: Commit.**

---

## Task 4: Race announcements

**Files:**
- Create: `platform/a11y_race.h`, `platform/a11y_race.c`,
  `tests/check_a11y_race.py`
- Modify: `CMakeLists.txt`, `tools/run_checks.py`

- [ ] **Step 1: Write the failing gate.** `tests/check_a11y_race.py`:
  - drives a full race with `MDKR_A11Y_TRACE=1` and asserts utterances for race
    start, each position change, each lap, final lap, and finish;
  - asserts position announcements are **coalesced** — a race with rapid
    position swapping must not produce an unbroken stream, so assert a minimum
    interval between consecutive `cat=race_position` utterances;
  - asserts the `[SIMHASH]` v3 stream is byte-identical with announcements on and
    off — announcements are presentation-only;
  - asserts each category toggle silences only its own category.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Source the events from the existing trace.**
  `platform/gameplay_event_trace.c` already observes race events. Map from it
  rather than adding new observation points in the race code.

- [ ] **Step 4: Run the gate and the race regressions.**

```bash
MDKR_AUDIO=0 python3 tests/check_a11y_race.py
MDKR_AUDIO=0 python3 tests/check_race_drive.py
MDKR_AUDIO=0 python3 tests/check_determinism.py
```

- [ ] **Step 5: Register in `CHECKS` and commit.**

---

## Task 5: The Accessibility section

**Files:** `platform/app/ui_settings.cpp`, `platform/app/app_ui_policy.cpp`,
`tests/test_app_ui_policy.cpp`

- [ ] **Step 1: Extend `tests/test_app_ui_policy.cpp`** to assert the
  Accessibility section contains the four speech keys plus `Camera.Comfort`, the
  contrast key, and the UI scale key — and that those three are no longer
  duplicated in their old sections.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Implement the section**, reusing `drawSettingsSectionHeader()`
  and `drawKey()`.

- [ ] **Step 4: Make the section itself reachable and spoken** — run
  `tests/check_a11y_shell.py`, which will now cover the new controls
  automatically because it enumerates from the schema.

- [ ] **Step 5: Run the UI suites and commit.**

```bash
ctest --test-dir build-rel -R 'app_ui_policy' --output-on-failure
MDKR_AUDIO=0 python3 tests/check_app_ui_input.py
MDKR_AUDIO=0 python3 tests/check_a11y_shell.py
```

---

## Task 6: Verification with real screen readers, and the honesty pass

**Files:** `docs/ACCESSIBILITY.md`, `README.md`, `docs/APP_SHELL.md`,
`docs/DEFINITION_OF_DONE.md`

- [ ] **Step 1: Write `docs/ACCESSIBILITY.md`** — what is supported, what is
  self-voiced versus what a native screen reader can read, the exact verification
  route for VoiceOver and NVDA, and the known gaps.

- [ ] **Step 2: Execute the VoiceOver route** on macOS: launch, navigate to
  settings, change a value, start a race, finish it. Record what worked and what
  did not, verbatim.

- [ ] **Step 3: Execute the NVDA route** on Windows, the same way.

- [ ] **Step 4: Update the claim in `README.md` precisely.** If self-voicing
  works but the native accessibility tree is still absent, say exactly that. The
  current line — "does not yet speak to screen readers, so it is not advertised
  as screen-reader compatible" — should become a specific statement, not a
  broader claim.

- [ ] **Step 5: Update `docs/APP_SHELL.md`'s semantic-accessibility limitation**
  and add a row to `docs/DEFINITION_OF_DONE.md`.

- [ ] **Step 6: Run the link checker and the complete suite.**

```bash
python3 tools/check_markdown_links.py
rm -f save/eeprom.bin
MDKR_AUDIO=0 python3 tools/run_checks.py
```

- [ ] **Step 7: Commit.**

---

## Self-review

**Spec coverage.** M1 → Task 1. M2 → Task 2. M3 → Task 3. M4 → Task 4.
M5 → Task 5. M6 → Task 6. US-6 → the default-off key in Task 2 Step 1 and the
`MDKR_AUDIO=0` refusal in Task 3 Step 3.

**Deliberately out of scope:**

- **A native accessibility tree** (NSAccessibility / UI Automation / AT-SPI).
  It would let a player use their own screen reader rather than the app's voice,
  which is better — but it means projecting an immediate-mode canvas into a
  retained tree, which is a sprint of its own. The model built in Task 1 is the
  right substrate for it, and doing self-voicing first means the semantic data
  already exists when that sprint starts.
- **Colour-blind palettes and remappable HUD contrast beyond the existing
  contrast option.** Worth doing; needs its own evidence.
- **Braille display output.** Follows the native tree, not self-voicing.

**Type consistency.** `MdkrA11yCategory` and `MdkrA11yPriority` are defined in
Task 1 and used in Tasks 2–4. `mdkr_a11y_next()` is popped only by the Task 3
worker. `[SPEAK]` is the one new stdout marker, introduced in Task 1 and asserted
by the gates in Tasks 2 and 4.
