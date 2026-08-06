# Open items — Saves and progression

> One subsystem of the split [open-items index](README.md), which states how these files are kept.

**Currently open** (per [`README.md`](README.md#still-open)'s open/closed
table): none — every entry below is closed or resolved; kept as the
append-only historical record.


## FIXED: save progression and persistence isolation

The live game represents course progress as prerequisite bits:
`VISITED`, `CLEARED`, then `SILVER`. EEPROM stores the four ordered states
`0..3`. The old writer counted independent bits, so impossible states changed
meaning on reload: `CLEARED` alone became `VISITED`, while
`VISITED|SILVER` became `VISITED|CLEARED`.

`mdkr_save_course_flags_to_status()` now persists only the longest valid
prerequisite prefix; `mdkr_save_course_status_to_flags()` owns the reverse
mapping. The ROM-free codec test exhausts all four serialized states and all
eight live low-bit combinations. Legal states are exact, while impossible
states can only lose unsupported bits and can never gain a progression bit.
The engine uses those same helpers at its EEPROM boundary.

The compiler-layout declaration now names the actual config bits: the
default-high bit is 24 and subtitles is 25. Static assertions lock the slot and
config block sizes, while the byte codec remains the serialization authority.

Finally, save-write custody is explicit. Adventure gameplay schedules the live
`Settings` source only through the tracks-mode guard. Forced cartridge copies
source the selected `gSavefileData[]` object; Controller Pak imports decode into
the dedicated fourth scratch object before scheduling it. The SI boundary
consumes that typed source exactly once. Browsing the aggregate Tracks records
can still populate live menu state, but that object has no forced persistence
route.

Evidence: 21/21 CTest; native build; the complete save fail-safe matrix
(torn/garbage/autosave/poisoned Adventure/Taj); linked wasm and ROM-absence
guard; and the ROM/GPU/engine-independent browser save-custody gate, including
all six transaction failures, hostile input, native interchange, recovery,
merge, edit, wipe/import/reload, accessibility, and zero-upload checks.

## Save-file fail-safe and browser save recovery — wave "savefailsafe"

> Prompted by a report from the published browser build: *"every time I boot now, I
> get dropped into the first Genie island race"*, after extended Adventure/boss play
> and two earlier engine crashes.
>
> **The symptom was later corrected in a follow-up report** — *"It doesn't boot direct into a
> race; after navigating all screens, it boots into a genie test, which we already
> completed"* — and the corrected symptom **does** reproduce: see Defect 4. The
> literal first reading (a save sending the BOOT path into a race) is still
> disproved below, and that disproof is kept because it is what rules the boot path
> out. Four save-borne defects in all, all fixed, all asserted by
> `tests/check_save_failsafe.py`.

### NOT REPRODUCED (the literal first reading): a save cannot send BOOT into a race
*Scope: this rules out the boot path — logo/title/menus — only. The corrected
symptom is a Taj challenge replayed after normal menu navigation, which is
Defect 4 and does reproduce.*

Stated plainly because the retraction matters more than the fix. **No content of
`save/eeprom.bin` can make the boot enter a race.** Two independent lines:

1. **By construction.** The Adventure resume path is
   `menu_file_select_loop()` → return `gNumberOfActivePlayers` → `mode_menu()`'s
   `menuLoopResult > 0` branch → `load_next_ingame_level(n, -1, …)` →
   `gPlayableMapId = get_track_id_to_load()`. `get_track_id_to_load()`
   (`menu.c:13654`) returns `settings->courseId` for a non-new game — but
   `menu_file_select_loop()` calls **`init_racer_headers()`** three lines before it
   returns, and that sets `gSettingsPtr->courseId = 0`
   (`thread3_main.c:1153`). So the resume target is *always* levelId 0,
   ASSET_LEVEL_CENTRALAREAHUB. `courseId` is also **not in the save file at all** —
   `SaveFile` (`save_layout.h`) has no level field, and neither
   `populate_settings_from_save_data()` nor `func_800732E8()` touches it.
2. **By measurement.** 98 headless boots on us.v80 across: absent; all-0x00;
   all-0xFF; all-0xAA; 512 random bytes; a random-but-checksum-valid image; a
   fully-maxed valid image; a real save written by the Adventure route; and 11
   truncation lengths (0, 1, 8, 39, 40, 100, 119, 120, 200, 320, 511). **Every one
   reaches `menu_init menuId=0` (MENU_TITLE) at frame 1134**, having loaded only
   levelId 39 (OPTIONSBACKGROUND, behind MENU_BOOT), 21 (FRONTEND, behind
   MENU_LOGOS) and 23 (TITLESCREENSEQUENCE, the title's own backdrop) — identical
   to the no-save boot. The race-looking loads a healthy idle boot *does* make are
   the ordinary **attract demos**, `levelId=18` (Greenwood Village) at frame ~5132
   and `levelId=28` (Frosty Village) at ~6632, `numPlayers=0 cutscene=100`,
   loaded via `load_level_for_menu()` from `menu_title_screen_loop()` on the
   title-demo timer. Those are camera-only demos and exit on START.

Also checked and cleared:

- **Does a bad checksum fail SAFE or into the resume path? SAFE.**
  `populate_settings_from_save_data()` calls `clear_game_progress()` (which sets
  `newGame = TRUE`) *before* it validates, and clears `newGame` only inside the
  `var_a0 == 0` branch. `read_save_file()` then calls `erase_save_file()`, which
  rewrites the slot to 0xFF. Worth knowing: that means **one bad byte silently
  destroys that save slot on the next boot** — faithful to the ROM, and the reason
  the quarantine below exists.
- **Is a test hook live in the shipped web build?** Not the ones this report
  needed ruled out — but the answer is no longer "the page writes exactly one env
  var". Emscripten seeds `getEnvStrings()` with
  `{USER, LOGNAME, PATH, PWD, HOME, LANG, _}` plus `Module.ENV`, and
  `grep -o "MDKR_[A-Z_0-9]*" dist/web/mdkr64_web.js` returns **nothing** — the
  loader never mentions one. `dist/web/mdkr64-shell.js`'s `preRun` now sets
  `MDKR_TRACE` (from `?trace=`), `MDKR_OBJCOLL`, `MDKR_LOAD_TRACK`, and any
  `MDKR_[A-Z0-9_]+` key carried by a `testConfig.env` object, each value truncated
  to 256 bytes. `MDKR_DRIVE_ROUTE`, `MDKR_AUTOPILOT` and `MDKR_FORCE_LAPS` remain
  unreachable from the page, and `MDKR_TRACE` only enables logging. **Re-read the
  `preRun` block before using this to rule anything out — the list has grown
  once already.**
- **Adventure-2 mismatch at FILE SELECT is not a strand.** A save with
  `cutsceneFlags & CUTSCENE_ADVENTURE_TWO` cannot be entered from normal
  Adventure: `fileselect_input_root()` (`menu.c:7857`) plays
  `SOUND_HORN_DRUMSTICK` and `break`s. An A-tap-only fixture therefore sits there
  for ever, which looked like a wedge — but B still returns to GAME SELECT and
  ERASE still works, so it is a *fixture* artifact, not a trap. Recorded because
  it will look like a hang again the next time a fixture only presses A.

### Defect 1 — the save image was accepted at any length (platform)
`eeprom_load()` (`platform/stubs_dkr.c`) did `fread(s_eeprom, 1, 512, f)` and
ignored the count, so a short file was interpreted with its missing tail read as
zeros. A short file is not hypothetical: the old `eeprom_store()` opened with
`fopen(path, "wb")`, which **truncates before anything is written**, so a crash
inside the store left 0..511 bytes on disk — and on the web build the file comes
back out of IndexedDB, where a killed tab or an interrupted `FS.syncfs` can do the
same. Two changes:

- **Atomic store.** Write `save/eeprom.bin.tmp`, then `rename()` it over
  `save/eeprom.bin`. Atomic on POSIX and a single node swap in emscripten's FS, so
  the real file is never *observed* short; an interrupted store leaves the previous
  good image plus a stray `.tmp`.
- **Validating load.** The image is a fixed table of independently-checksummed
  blocks (`save_layout.h`: 3×`SaveFile`(40) | `SaveConfig`(8) | `CourseRecords`(192)×2).
  Require exactly 512 bytes, and require every block to be erased (all 0xFF), blank
  (all 0x00) or checksum-valid. Anything else is copied verbatim to
  **`save/eeprom.bin.bad`** and replaced in memory with the state the game itself
  defines for empty: 0xFF for a slot (`erase_save_file()`'s own marker), 0x00 for
  config/records (which then fail their own checksums and are ignored).

The checksum rules mirror `save_data.c` exactly (`populate_settings_from_save_data`
/ `func_80073588`: big-endian u16 at [0..1] == `(5 + Σ bytes[2..n)) & 0xFFFF`;
`read_eeprom_settings`: byte 0 == `5 +` the nibble sum of the low 56 bits). This is
**behaviour-neutral for every image DKR can produce** — a valid block is passed
through untouched, and a blanked block is one the game already rejected on its own
checksum. Verified: a valid image produces no `[SAVE]` line and no `.bad` file, and
`check_race_finish_time` still records and reads back its course time.

### Defect 2 — a checksum-VALID slot with an impossible field SIGSEGVs, every boot
The slot checksum is a plain 16-bit byte sum, which corrupt bytes satisfy by
chance. Fuzzing 40 randomly-corrupted images through the Adventure resume route:
**4 of 40 crashed with SIGSEGV**, all four in
`func_80054FD0` ← `func_8004F7F4` ← `update_player_racer` ← `obj_update`. A
leave-one-out over the decoded fields put every one of them on a single field:

```
drop wizpig     : 0        <- only this one removes the crash
drop <any other>: -11
```

and a per-value sweep (others zeroed) isolates it exactly:

| `wizpigAmulet` | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| exit code | 0 | 0 | 0 | 0 | 0 | **−11** | 0 | **−11** |

`settings->wizpigAmulet` is **3 bits on disk** (0..7) but the amulet has **four**
pieces — `game.c:521` tests `wizpigAmulet >= 4`, `vehicle_tricky.c:366` uses
`wizpigAmulet - 1` as a 0..3 cutscene index — so 5..7 cannot come from play.
`objects.c:2308` (`BHV_DYNAMIC_LIGHT_OBJECT_2`) then does

```c
i = settings->wizpigAmulet;
assetCount = i + 1;
curObj->modelIndex = settings->wizpigAmulet;
```

with **no clamp against the object's `numberOfModelIds`**, so the spawner loads
out-of-range model ids on Timber's Island and the racer collision path dereferences
the result ~750 frames later. (6 survives only because its overrun happens to land
on benign bytes — the field is unbounded, not "mostly fine".) Every other field at
its maximum representable value survives the same route: `tajFlags` 0x3F,
`trophies` 0x3FF, `bosses` 0xFFF, `keys` 0xFF, balloons 127×6, door flags
0xFFFF×6, all courses cleared, `filename` 0xFFFF, `cutsceneFlags` 0xFFFFFFFF.

**Why this is the worst shape for the demo:** the save lives in IndexedDB, so it is
crash → reload → same save → crash, for ever, and until this wave the page had no
control that clears saved progress.

**Fixed** in `populate_settings_from_save_data()` under `NATIVE_PORT`: a slot with
`ttAmulet > 4 || wizpigAmulet > 4` is rejected with the function's own idiom for
"this is not a save" — `clear_game_progress()`, leaving `newGame = TRUE` — so
`read_save_file()` erases the slot and FILE SELECT offers an unstarted file. Scope
is deliberately narrow (the one field the fuzz proved fatal). ~~**The deeper fix
belongs with the object code: clamp `modelIndex`/`assetCount` against
`numberOfModelIds` in `objects.c`.** Left open here on purpose — `objects.c` was
being edited concurrently by the boss-race wave.~~ **DONE in wave "core-safety":**
the character, trophy, amulet and rocket-signpost selectors in `spawn_object()`
all go through one final-use resolver, `mdkr_model_load_selection()`, which
validates the save-derived value against that object header's own
`numberOfModelIds` and selects nothing when it fails. The save-side rejection here
is kept as the narrower belt.

Post-fix the same 40-image fuzz is **0/40 anomalous**, and `wizpigAmulet` 0..7 all
exit 0.

### Defect 3 — two independent `FS.syncfs` coalescers in one page (web)
`FS.syncfs` is not reentrant. The engine's post-EEPROM-write `EM_ASM`
(`stubs_dkr.c`) guarded on `Module.__mdkrSyncing`; the shell's `persist()` guarded
on its own module-local `syncing`. **Neither knew about the other**, so the shell's
5-second timer could start a sync while an EEPROM-write sync was in flight. The
shell now installs its coalescer as `Module.__mdkrPersist` and the engine calls it,
so the whole page shares one in-flight sync and one error path (the save banner).
The engine keeps a local fallback for when it runs without the shell.

### Browser recovery path — there was none, now there is
`dist/web/index.html` gains a **"Stored data" section (step 04)** with an
**Erase saved progress** button (`#clear-save`, `.btn-danger`, `confirm()`-gated,
status in `#save-status`), separate from the ROM control and cross-referenced from
the "Saves persist" bullet in step 02. It talks to **IndexedDB directly** rather
than through the engine — deliberately, because the state that needs erasing is the
state the engine crashes on, and a recovery path that needs a working engine is no
recovery path. IDBFS names its database after the mount point (`/rom`, `/save`) and
keeps files in one `FILE_DATA` store; clearing that store in a plain readwrite
transaction works even while the engine holds a connection, which
`deleteDatabase()` does not (it blocks).

Two pre-existing shell defects were found while wiring it and are fixed in the same
pass, because the new control is meaningless next to a dead one:

- **"Forget stored ROM" was unreachable *and* did nothing.** Nothing ever set
  `$("forget").hidden = false`, and the handler instantiated a *second* engine
  module, never mounted IDBFS on it, and unlinked a path that did not exist there —
  so it downloaded the whole engine and deleted nothing.
- **A persisted ROM could not be used.** `#play` was only enabled by picking a
  file, so "the ROM persists across reloads" was true of the storage and false of
  the UI: a returning visitor had to re-pick the ROM every time. `probeStoredRom()`
  now counts the `/rom` store at startup and enables Play / reveals Forget.

### Defect 4 — a Taj challenge you have already beaten is replayed, and wedges you
**This is the defect behind the report, once the symptom was corrected.** The
first description in the report was "boots into a race"; the correction was: *"It doesn't
boot direct into a race — after navigating all screens, it boots into a genie test,
which we already completed."* So: normal menu navigation, Adventure, and then a
**Taj challenge** that was already finished.

`tajFlags` (6 bits in `SaveFile`) holds two parallel triples:

| | car | hover | plane |
|---|---|---|---|
| **OFFERED** (Taj has proposed it) | `0x01` | `0x02` | `0x04` |
| **BEATEN** (finished first) | `0x08` | `0x10` | `0x20` |

They are written at two different moments by two different functions:
`track_setup_racers()` (`objects.c:1807`) sets OFFERED when Taj teleports over,
and `mode_end_taj_race()` (`objects.c:10744`) sets BEATEN on a first-place finish.
Each calls `safe_mark_write_save_file()` separately, and on the web each is
followed by its own IDBFS sync.

**The offer gate consults only the OFFERED half** (`objects.c:1794`:
`!(tajFlags & TAJ_FLAGS_CAR_CHAL_UNLOCKED) && balloonsPtr[0] >= miscAsset16[0]`),
and **the auto-offer dialogue never asks**: `DIALOGUEPAGE_TAJ_CHALLENGE_CAR` falls
straight through to `DIALOGUEPAGE_TAJ_4`, which returns
`(gNextTajChallengeMenu - 1) | 0x40` and starts the race. So a save reading
"beaten but never offered" replays a finished challenge on **every** entry to
Timber's Island — and because `obj_loop_parkwarden()` calls
`disable_racer_input()` every frame its dialogue is open (and that dialogue cannot
be advanced from the pad — see P3.5 Part C), the player is simply stopped.

Measured on us.v80, `adventure_hub_drive.txt`, 12000 frames, 9909 post-hub
`[PACE]` rows, with `balloonsPtr[0] = 5` (= `ASSET_MISC_16[0]`, the car threshold):

| `tajFlags` | path driven | rows stationary |
|---|---|---|
| `0x00` (nothing) | 37767.6 | 2406 (24 %) |
| `0x09` (offered + beaten — consistent) | 37767.6 | 2406 (24 %) |
| **`0x08` (beaten, never offered)** | **14439.4** | **7525 (76 %)** |

**Fixed** at the save-read seam, in `populate_settings_from_save_data()` under
`NATIVE_PORT`, one line: `tajFlags |= (tajFlags >> 3) & TAJ_FLAGS_UNLOCKED_A_CHALLENGE`.
The implication holds in one direction only, which is what makes it a repair
rather than a guess — a challenge cannot be beaten without having been offered —
so it can only ever restore a bit play must already have set. It never sets a
BEATEN bit, it is idempotent, and it is a **no-op on every consistent save**
(verified for `0x00/0x01/0x03/0x07/0x09/0x1B/0x1F/0x3F`). The write-side cause is
Defect 3 above (two independent `FS.syncfs` coalescers, so one of the two progress
flushes could be lost); this is the belt to that braces.

#### What was measured and ruled OUT along the way
- **Taj completion IS persisted and IS read back.** `tajFlags` round-trips
  correctly: injected `0x01/0x03/0x07/0x09/0x1B/0x1F/0x3F` all reappear in
  `save/eeprom.bin` after a hub visit. "Never written" was the cheapest
  explanation and it is wrong.
- **The write goes to the right slot.** With the started file in **slot 1** and a
  RIGHT press at FILE SELECT, the progress landed in slot 1; slots 0 and 2 stayed
  erased. `get_save_file_index()` is `gSaveFileIndex`, and its two resets
  (`menu_title_screen_init`, `menu_file_select_init`) both precede the pick.
- **`1 << (j + 31)` was right on this host — by luck.** `track_setup_racers`
  (`objects.c`) relied on MIPS masking the shift count to 5 bits. Probed directly:
  j=1/2/3 yield `0x1/0x2/0x4`, i.e. arm64's register shift masks identically. It
  is now `DKR_SHL32` — see the resolved item below.
- **The `ttAmulet`/`wizpigAmulet > 4` rejection added in this wave is NOT a false
  positive.** Both writers clamp: `objects.c:6597` (`i = ttAmulet + 1; if (i > 4)
  i = 4;`) and `vehicle_tricky.c:387` (`worldBit++; if (worldBit >= 5) worldBit =
  4;`), so play cannot produce 5..7. Measured: a save with `tajFlags = 0x3F`,
  `ttAmulet = 4`, `wizpigAmulet = 4` and 18 balloons survives untouched
  (`valid=True`, `erased=False`, `tajFlags` still `0x3f`). It cannot erase real
  progress.
- **It is NOT the `gTrackSelectIDs[4][6]` / `gFFLUnlocked` arbitrary-level load**
  (wave "splitsweep"). Two independent reasons, both checked:
  1. **Measured.** Re-run on top of that fix, the Taj wedge reproduces with
     byte-identical numbers (14439.4 / 76 % reverted, 37767.6 / 24 % fixed). The
     repro route never visits TRACK SELECT at all — TITLE → CHARACTER_SELECT →
     CAUTION → GAME_SELECT → **Adventure** → FILE_SELECT → resume → hub.
  2. **By construction.** There is no Taj level id: `grep ASSET_LEVEL.*taj`
     over `asset_enums.h` returns nothing, because a challenge runs *in place* on
     `ASSET_LEVEL_CENTRALAREAHUB` with `levelHeader->race_type` switched and
     restored by `mode_end_taj_race()`. An arbitrary level id can therefore only
     ever produce a random *race track*, never "a genie test". And the Taj offer
     block is gated `if (is_in_tracks_mode() == FALSE)`, so Tracks mode cannot
     reach it either. Both defects are real; they are different defects, and this
     is the one that matches the corrected symptom.

### ~~NEW OPEN ITEM: five sites depend on MIPS masking the shift count~~ — CLOSED by wave "keyshift"
`1 << (j + 31)` is how the decomp renders code that relied on MIPS's `sllv`
masking the shift amount to 5 bits, i.e. `1 << ((j + 31) & 31)`. In C a shift of
32 or more is **undefined behaviour**. Five sites:

| site | expression as this wave found it |
|---|---|
| `objects.c` `track_setup_racers` | `settings->tajFlags \|= 1 << (j + 31)` |
| `game.c` `level_load` | `!(cutsceneFlags & (CUTSCENE_DINO_DOMAIN_KEY << (var_s0 + 31)))` |
| `game.c` `level_load` | `cutsceneFlags \|= CUTSCENE_DINO_DOMAIN_KEY << (var_s0 + 31)` |
| `game.c` `level_load` | `var_s0 = 8 << (settings->worldId + 31)` |
| `waves.c` `obj_wave_height` | `var_t0 <<= (log->unk2 + 0x1F)` |

All four progress-flag sites were probed as **currently correct** on arm64
(measured `0x1/0x2/0x4` for j=1/2/3), because arm64's variable shift also masks
modulo 32 and clang emitted a register shift rather than folding. That was luck,
not portability — and worse than latent: wave "keyshift" then showed that at `-O2`
clang folds the statement away outright, and the port's *web* build is Release, so
this was the live, reported "key cutscene replays after every race" defect.

**All five sites (plus one more the shift-syntax sweep found) now go through
`DKR_SHL32(x, n)`** in `game/include/macros.h`, which reproduces `sllv` for every
count; `MDKR_SHL32_CONTROL` selects the reverted form for
`tests/check_key_cutscene_once.py`'s broken arm. The suggested detector shipped
with it: `-fsanitize=shift-exponent` is in `tests/check_array_bounds_sweep.py`'s
instrumented build, with a required `__ubsan_handle_shift_out_of_bounds` import so
an empty report cannot be confused with a dropped flag. Full write-up:
[collision.md class 3](collision.md#class-3--a-variable-shift-count-that-can-reach-32).

### Check
`tests/check_save_failsafe.py` — five cases, headless and muted, each starting
from a known EEPROM state. Under `tools/run_checks.py` it writes into a
run-scoped temporary save directory (`MDKR_TEST_SAVE_DIR`, exported to the engine
as `MDKR_SAVE_DIR`); a standalone run defaults to the repository's `save/` and
restores every EEPROM artifact it found, however it exits. A leftover save would
otherwise send FILE SELECT down the resume path and break
`check_adventure_hub.py`. Paths below are relative to whichever save directory is
in effect:

1. **torn** (100 of 512 bytes) → title screen at 1134, nothing but the three boot
   levels loaded, `eeprom.bin.bad` byte-identical to the input, `eeprom.bin` back to
   512 bytes.
2. **garbage** (512 random bytes) → same.
3. **poison** — `wizpigAmulet = 7` in slot 0, *valid* started slots with a complete
   amulet (4) in slots 1 and 2 → exit 0, no `[CRASH]`, FILE SELECT reached (proving
   the file was read), slot 0 erased, **slots 1 and 2 byte-for-byte untouched**.
   That last assertion is what keeps the case honest: an absent save cannot satisfy
   it, and neither can a fix that rejects the whole image instead of the one bad
   slot.
4. **control, no save at all** → clean boot, and **no** `.bad` and no `.tmp` file,
   which is what stops cases 1–2 passing vacuously.
5. **taj** — `tajFlags = 0x08` (car challenge beaten, never offered) with 5
   balloons, driven for 12000 frames → the kart must still cover ≥ 25000 units of
   Timber's Island and be stationary for ≤ 45 % of the post-hub rows. Measured by
   *driving*, because the damage is a held-down input, not a crash: healthy
   37767.6 / 24 %, wedged 14439.4 / 76 %. It also asserts levelId 0 loaded, so a
   route change that stopped resuming the file fails loudly instead of covering
   nothing.

Proven in both directions by running the controls, not by reasoning:

| reverted | failures |
|---|---|
| `eeprom_load()` validation (`platform/stubs_dkr.c`) | `torn: save/eeprom.bin.bad was not created`; `torn: save/eeprom.bin is 100 bytes after the run, expected 512`; `garbage: save/eeprom.bin.bad was not created` |
| amulet rejection (`game/src/save_data.c`) | `poison: run produced [CRASH]/[FATAL]`; `poison: run exited -11 (killed by signal 11)`; `poison: slot 0 was not erased (first bytes 002c00000000)` |
| BEATEN-implies-OFFERED repair (`game/src/save_data.c`) | `taj: the kart only drove 14439.4 units (need >= 25000.0)`; `taj: the kart was stationary for 76% of the run (limit 45%)` |

`check_determinism`, `check_race_drive`, `check_race_finish_time` (course time
4777 on the current route), `check_adventure_hub` and `check_adventure_race_loop`
all still pass.
