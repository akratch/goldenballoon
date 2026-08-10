# S5 — ROM and region breadth

> **Executing this plan:** work the tasks in order, one `- [ ]` step at a time.
> Tick a step only once its command has been run and its output read. The
> run-it-and-watch-it-fail steps are load-bearing; skipping ahead to the
> implementation forfeits the positive control that makes the gate mean anything.

**Goal:** Grow the supported set beyond US 1.1 and European 1.1 — safely, in the
order `docs/ROM_REVISIONS.md` §6 already establishes — and give players a way to
check their dump before they download anything.

**Architecture:** The supported set is currently two revisions that race
byte-identically, and every other revision is classified from its header CRC pair
and refused by name before engine boot (`platform/rom_id.c`). That gate is the
only reason the unbounded sub-entry indexing below it is not a live hazard. This
sprint therefore does the cheap safety work **first**, publishes a checker that
needs no new engine code at all, and only then opens the gate — one revision at a
time, each with its own oracle route.

**Tech stack:** C11, the existing `platform/rom_id.c` classification table, the
existing `dist/web/rom-id.js` browser mirror, `tools/run_oracle.sh` and
`docs/ORACLE.md` for per-revision fidelity, GitHub Pages for the checker.

## Global constraints

- Every game or test invocation passes `--headless-frames N` and sets
  `MDKR_AUDIO=0`.
- **No ROM-derived data in any commit** — this sprint handles more ROMs than any
  other and is the likeliest place to slip. `tools/check_no_rom.sh` and
  `tools/check_clean_room.sh` fail closed; run both before every commit.
- `platform/rom_id.c` and `dist/web/rom-id.js` are mirrors, and
  `tests/check_rom_revision.py` compares their revision tables row by row. Any
  change to one is a change to both, in the same commit.
- Game-code changes go behind `#ifdef NATIVE_PORT` with a `_Static_assert`
  wherever a layout or offset assumption is involved.
- Do not widen the accepted set in the same commit that adds a bounds check. The
  ordering below is the safety property.
- Run `python3 tools/check_public_surface.py --staged` before every commit.

---

## User stories

**US-1 — Check my dump first.** As a player, I drop my ROM onto a web page and it
tells me the exact revision and whether this port supports it, without uploading
anything, so that I know before I download a build.

**US-2 — Get a straight answer.** As a player with an unsupported revision, the
refusal names my revision, names what is supported, and says what would have to
change, so that I am not left guessing whether my dump is broken.

**US-3 — Play the Japanese release.** As a Japanese-release owner, I get a build
that supports my cartridge, so that I am not required to source a US dump.

**US-4 — Play the 1.0 revisions.** As a US 1.0 or European 1.0 owner, my ROM
works, so that the older printing is not excluded.

**US-5 — Trust the result.** As the maintainer, every newly supported revision
has its own pixel and audio evidence, so that "supported" means the same thing
for all of them.

---

## Milestones and acceptance criteria

### M1 — Bound the sub-entry indices

**Done when:**
- Every sub-entry accessor bounds-checks the index against that section's actual
  entry count, and an out-of-range index aborts loudly with the section name, the
  requested index, and the actual count.
- A mechanical sweep establishes that **every** accessor is covered, not a
  sample; the sweep instrument is committed, not just its result.
- A negative-control test drives a deliberately out-of-range index and asserts
  the loud abort, so the check is proven non-vacuous.
- No behaviour changes on the supported revisions: the full suite's frame and
  state hashes are unchanged.

### M2 — The hosted revision checker

**Done when:**
- A static page accepts a local file, computes its header CRCs and whole-image
  SHA-256 entirely in the browser, and reports the revision and support status
  using the existing `dist/web/rom-id.js`.
- The page uploads nothing; a gate asserts the page contains no network call.
- The page is published alongside the existing browser build and linked from
  `README.md`, and the same `check_rom_revision.py` row-by-row comparison covers
  the copy the page uses.

### M3 — US 1.0 and European 1.0

**Done when:**
- Both revisions are accepted, with their own asset-table offsets, whole-image
  SHA-256s, and asset-table bounds.
- Each has an oracle route producing a recorded fidelity score, and an audio
  comparison, at the same standard the supported pair meets.
- Every game-text index the port requests is proven in range on the 259-entry
  1.0 `GAME_TEXT` section, by exhaustive enumeration rather than by playing.
- The suite runs green with a 1.0 ROM staged in `build/roms/`.

### M4 — The Japanese release

**Done when:**
- A `REGION_JP` build configuration exists and produces a separate binary.
- Font handling, game text, and EEPROM layout are correct for that region, each
  with its own gate.
- A JP oracle route and audio comparison exist.
- The launcher and the checker both direct a JP-ROM owner to the JP build rather
  than refusing without a route.

### M5 — Documentation and honesty pass

**Done when:** `docs/ROM_REVISIONS.md`, `README.md`, `ROADMAP.md` and
`docs/DEFINITION_OF_DONE.md` all agree on exactly which revisions are supported,
what evidence each has, and what remains unverified.

---

## File structure

**Create:**

| Path | Responsibility |
|---|---|
| `platform/asset_subentry.h/.c` | Bounds-checked sub-entry accessors. |
| `tools/sweep_subentry_access.py` | Mechanical sweep for unchecked accessors. |
| `tests/test_asset_subentry.c` | Bounds unit test, no ROM. |
| `tests/check_subentry_bounds.py` | Negative control: out-of-range aborts loudly. |
| `dist/web/rom-check.html` | The revision checker page. |
| `tests/check_rom_checker_page.py` | No-network and correctness gate for the page. |
| `tests/check_rom_text_indices.py` | Exhaustive game-text index range proof. |
| `cmake/region_jp.cmake` | The `REGION_JP` build configuration. |

**Modify:** `platform/rom_id.c`, `platform/rom_validation.c`,
`dist/web/rom-id.js`, `game/src/asset_loading.c` (behind `NATIVE_PORT`),
`tools/run_checks.py`, `cmake/tests.cmake`, `CMakeLists.txt`,
`docs/ROM_REVISIONS.md`, `README.md`, `ROADMAP.md`,
`docs/DEFINITION_OF_DONE.md`.

---

## Task 1: Build the sweep instrument before fixing anything

**Files:**
- Create: `tools/sweep_subentry_access.py`

CONTRIBUTING §6 requires a mechanical sweep, and requires building the instrument
when it does not exist. Do this first: the sweep's output *is* the task list for
Task 2, and doing it afterwards would mean guessing at coverage.

- [ ] **Step 1: Write the sweep.** It must, over the whole tree:
  - find every read of an asset sub-entry — the index-into-section pattern
    `asset_swap.h` documents and `game/src/asset_loading.c` implements;
  - classify each site as bounds-checked or not, by looking for a comparison
    against that section's count dominating the access;
  - print a table of file, line, section, and verdict;
  - exit non-zero when any site is unchecked, so it can become a gate later.

- [ ] **Step 2: Run it and record the baseline.**

```bash
python3 tools/sweep_subentry_access.py
```

Paste the full table into the commit body. This is the coverage claim Task 2 will
be measured against.

- [ ] **Step 3: Commit the instrument and the baseline.**

---

## Task 2: Bounds-check the sub-entry accessors

**Files:**
- Create: `platform/asset_subentry.h`, `platform/asset_subentry.c`,
  `tests/test_asset_subentry.c`, `tests/check_subentry_bounds.py`
- Modify: `game/src/asset_loading.c` (behind `#ifdef NATIVE_PORT`),
  `cmake/tests.cmake`, `CMakeLists.txt`, `tools/run_checks.py`

**The mechanism, stated so it is not lost:** section indices are bounds-checked
today; indices *within* a section are not. This build asks for 1.1 indices. On a
1.0 ROM the `GAME_TEXT` section has 259 entries where 1.1 has 343, so a 1.1-only
string index is an unguarded out-of-bounds read producing no diagnostic. It is
not reachable today only because unsupported revisions are refused before engine
boot. M3 removes that refusal, so this must land first.

- [ ] **Step 1: Write the failing unit test.** `tests/test_asset_subentry.c`, no
  ROM, over a synthetic section descriptor:
  - index 0 of a 1-entry section resolves;
  - index 1 of a 1-entry section aborts;
  - index -1 aborts (pass a signed value through the real signature — if the
    signature is unsigned, assert that a huge unsigned value aborts);
  - a zero-entry section aborts for index 0;
  - the abort message contains the section name, the requested index, and the
    count.

- [ ] **Step 2: Register and run to verify it fails.**

- [ ] **Step 3: Write the accessor**

```c
/* asset_subentry.h — bounds-checked access to one entry inside an asset section.
 *
 * Section indices have always been checked; entry indices inside a section have
 * not. That gap is currently unreachable because unsupported ROM revisions are
 * refused before engine boot, and it becomes reachable the moment the supported
 * set widens. A loud abort is strictly better than the silent out-of-range read
 * it replaces: this codebase's dominant defect shape is the silent one.
 */
#ifndef MDKR64_ASSET_SUBENTRY_H
#define MDKR64_ASSET_SUBENTRY_H

#include <stddef.h>
#include <stdint.h>

typedef struct MdkrAssetSection {
    const char *name;       /* e.g. "GAME_TEXT" */
    const uint8_t *base;
    uint32_t entry_count;
    const uint32_t *offsets; /* entry_count + 1 entries */
} MdkrAssetSection;

/* Returns the entry base, or aborts with a named diagnostic. Never returns
 * NULL: a caller that could handle absence would have checked already. */
const uint8_t *mdkr_asset_subentry(const MdkrAssetSection *section,
                                   uint32_t index, uint32_t *out_size);

#endif /* MDKR64_ASSET_SUBENTRY_H */
```

- [ ] **Step 4: Route every site the sweep found through it**, behind
  `#ifdef NATIVE_PORT` in `game/src/asset_loading.c`. Keep the edits minimal and
  gated so the matching N64 build is untouched.

- [ ] **Step 5: Re-run the sweep and assert zero unchecked sites.**

```bash
python3 tools/sweep_subentry_access.py
```

Expected: exit 0. Paste the table.

- [ ] **Step 6: Write the negative control gate.** `tests/check_subentry_bounds.py`
  drives a deliberately out-of-range index through an env-gated test hook and
  asserts the process aborts with the named diagnostic and a non-zero exit — not
  a segfault, not a silent read.

- [ ] **Step 7: Prove nothing changed on supported revisions.**

```bash
MDKR_AUDIO=0 python3 tests/check_race_drive.py
MDKR_AUDIO=0 python3 tests/check_determinism.py
MDKR_AUDIO=0 python3 tests/check_track_sweep.py
MDKR_AUDIO=0 python3 tests/check_asset_swap_invariants.py
MDKR_AUDIO=0 python3 tests/check_array_bounds_sweep.py
```

All must pass with unchanged hashes.

- [ ] **Step 8: Register both new checks in `CHECKS` and commit.**

---

## Task 3: The hosted revision checker

**Files:**
- Create: `dist/web/rom-check.html`, `tests/check_rom_checker_page.py`
- Modify: `dist/web/rom-id.js` (only if a shared export is needed),
  `.github/workflows/web-demo.yml`, `README.md`, `tools/run_checks.py`

This is the highest value-per-line task in the sprint: the classification logic
already exists in `dist/web/rom-id.js`, and `tests/check_rom_revision.py` already
proves it agrees with `platform/rom_id.c` character for character.

- [ ] **Step 1: Write the failing gate.** `tests/check_rom_checker_page.py`:
  - asserts `dist/web/rom-check.html` contains no `fetch(`, no `XMLHttpRequest`,
    no `WebSocket`, no `<form action`, and no external `src`/`href` host — the
    page must be provably local-only, because asking players to hand over a ROM
    would be indefensible;
  - runs the page's classification logic under `node` over each ROM present in
    `build/roms/` and asserts the verdict string matches the native binary's,
    character for character — the same comparison `check_rom_revision.py` makes;
  - skips, with a printed reason, when `node` is absent.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Write the page.** A single self-contained HTML file: a drop
  target, `FileReader` + `crypto.subtle.digest('SHA-256', …)` for the whole-image
  hash, the existing header-CRC classification from `rom-id.js`, and a result
  panel that names the revision, says supported or not, and — for an unsupported
  one — states plainly what would have to change, mirroring the refusal text the
  native binary produces (US-2).

- [ ] **Step 4: Handle byte order.** `.v64` and `.n64` must be normalised before
  hashing, exactly as `platform/rom_io.c` does, or every byteswapped dump will be
  reported as unknown. `tests/check_rom_revision.py` §3 already establishes that
  both sides normalise; the page inherits that obligation.

- [ ] **Step 5: Run the gate. Verify it passes.**

- [ ] **Step 6: Publish it** alongside the browser build in
  `.github/workflows/web-demo.yml`, and link it from `README.md`'s ROM section.

- [ ] **Step 7: Register in `CHECKS` and commit.**

---

## Task 4: Exhaustive game-text index proof

**Files:**
- Create: `tests/check_rom_text_indices.py`
- Modify: `tools/run_checks.py`

Before accepting a 1.0 ROM, prove the port never asks it for an index it does not
have. Enumerating is cheap; discovering it in play is not.

- [ ] **Step 1: Enumerate every text index the port can request.** Add an
  env-gated instrumentation mode that logs `[TEXTIDX] section=... index=...` on
  every game-text access, then drive it across the existing route corpus — the
  `nav_*` fixtures, `check_track_sweep`, `check_adventure_hub`,
  `check_campaign_progression`, and `check_trophy_series`.

- [ ] **Step 2: Assert the maximum observed index against each revision's count.**
  The gate must fail if any observed index is at or above the 1.0 `GAME_TEXT`
  count of 259, and must print the offending index and the route that produced
  it.

- [ ] **Step 3: Record the coverage honestly.** The corpus is not the whole game.
  State in the gate's docstring which routes were driven and what fraction of the
  text sections they touched, so the claim is "these routes never exceed 259",
  not "the port never exceeds 259".

- [ ] **Step 4: Run it and record the result.**

- [ ] **Step 5: Register in `CHECKS` and commit.**

---

## Task 5: Accept US 1.0 and European 1.0

**Files:**
- Modify: `platform/rom_id.c`, `dist/web/rom-id.js`,
  `platform/rom_validation.c`, `docs/ROM_REVISIONS.md`,
  `tests/check_rom_revision.py`

Do not start this task until Tasks 2 and 4 are green.

- [ ] **Step 1: Extend `tests/check_rom_revision.py` first.** Add the two
  revisions to its expected-accept set, keeping its existing skip-when-absent
  behaviour so the check still passes on a machine with only the supported ROM.
  Run it — it must fail, because the revisions are still refused.

- [ ] **Step 2: Add each revision's asset-table offset** from the decomp's own
  `ver/splat/dkr.*.yaml` — `us.v77 0xECB60`, `pal.v77 0xECBF0` — plus its
  whole-image SHA-256 and asset-table bounds, to `platform/rom_id.c` **and**
  `dist/web/rom-id.js` in the same commit.

- [ ] **Step 3: Run the row-by-row mirror comparison.**

```bash
MDKR_AUDIO=0 python3 tests/check_rom_revision.py
```

- [ ] **Step 4: Run an oracle route per revision.** Follow `docs/ORACLE.md` —
  including its section on the ways the harness has lied, before trusting any
  fidelity score. Record each score in `docs/ROM_REVISIONS.md`.

```bash
bash tools/run_oracle.sh
```

- [ ] **Step 5: Run an audio comparison per revision.**

```bash
python3 tools/compare_audio_reference.py
```

- [ ] **Step 6: Run the full suite with a 1.0 ROM staged** in `build/roms/`, and
  paste the summary.

- [ ] **Step 7: If any evidence is missing, do not ship the revision.** Refuse it
  by name as before and record what is missing in `docs/ROM_REVISIONS.md` §6.
  A half-supported revision is worse than a refused one.

- [ ] **Step 8: Commit.**

---

## Task 6: The Japanese build

**Files:**
- Create: `cmake/region_jp.cmake`
- Modify: `CMakeLists.txt`, `platform/rom_id.c`, `dist/web/rom-id.js`,
  `platform/save_codec.c`, `docs/ROM_REVISIONS.md`, `README.md`

`docs/ROM_REVISIONS.md` §6 establishes the approach: a second build directory
with a per-version asset-offset default, sidestepping 423 compile-time gates.
Runtime dispatch across them buys little for much more work. Do not attempt it.

- [ ] **Step 1: Add the build configuration.** `cmake/region_jp.cmake` defines
  `REGION_JP`, selects the `jpn.v79 0xEE5D0` asset-table offset default, and
  names the output binary distinctly so the two cannot be confused on disk.

- [ ] **Step 2: Verify it compiles before anything else.**

```bash
cmake -S . -B build-jp -DMDKR_REGION=JP -DCMAKE_BUILD_TYPE=Release
cmake --build build-jp -j8
```

- [ ] **Step 3: Gate the EEPROM layout.** JP's save layout differs. Extend
  `tests/check_save_failsafe.py` with a JP arm asserting round-trip through the
  JP layout, and assert that a JP save cannot be read by the NA build and vice
  versa — each must refuse with a named reason rather than reading garbage.

- [ ] **Step 4: Gate the font and text handling** with a JP arm on
  `tests/check_native_ui_resolution.py` and the `nav_*` fixtures.

- [ ] **Step 5: Run an oracle route and an audio comparison** for `jpn.v79`.

- [ ] **Step 6: Route the player.** Both the launcher's refusal text and the
  Task 3 checker page must direct a JP-ROM owner to the JP build rather than
  refusing without a route (US-3).

- [ ] **Step 7: Run the full suite against the JP build** with a JP ROM staged.

- [ ] **Step 8: Commit.**

---

## Task 7: Documentation honesty pass

**Files:**
- Modify: `docs/ROM_REVISIONS.md`, `README.md`, `ROADMAP.md`,
  `docs/DEFINITION_OF_DONE.md`

- [ ] **Step 1: Make the four documents agree.** Each supported revision gets a
  row naming its evidence — oracle score, audio comparison, suite run — and each
  unsupported one names what is missing. Where a revision is supported but an
  evidence type is absent, say so; do not omit the row.

- [ ] **Step 2: Update `README.md`'s ROM section and download table**, in
  player-facing language, including the link to the checker page.

- [ ] **Step 3: Remove the closed items from `ROADMAP.md`'s ROM support
  section** and leave the open ones with their reasons intact.

- [ ] **Step 4: Run the link checker and the guards.**

```bash
python3 tools/check_markdown_links.py
bash tools/check_no_rom.sh
bash tools/check_clean_room.sh
```

- [ ] **Step 5: Commit.**

---

## Self-review

**Spec coverage.** M1 → Tasks 1–2. M2 → Task 3. M3 → Tasks 4–5. M4 → Task 6.
M5 → Task 7. US-2 is served by Task 3 Step 3 and Task 6 Step 6.

**Ordering is a safety property, not a preference.** Task 2 must land before
Task 5, and Task 4 before Task 5, because the accept path is what makes the
unbounded index reachable. If the sprint is cut short, cutting from the end is
safe; reordering is not.

**Deliberately out of scope:**

- **The demo/prototype builds.** Not among the five released revisions
  `docs/ROM_REVISIONS.md` enumerates.
- **Runtime dispatch across the 423 region gates.** Explicitly rejected in
  `docs/ROM_REVISIONS.md` §6 as much more work for little gain.
- **PAL timing changes.** PAL is already modelled as a 50 Hz source clock with
  the authored two-field ticket and is first-class per
  `docs/DEFINITION_OF_DONE.md`; nothing here touches it.

**Type consistency.** `MdkrAssetSection` and `mdkr_asset_subentry()` are defined
in Task 2 and used only there and in the sites Task 2 Step 4 rewrites.
`[TEXTIDX]` is the one new stdout marker, introduced in Task 4 Step 1.
