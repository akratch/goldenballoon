# Playable Taj mod — architecture and execution scope

**Status:** integrated locally; automated qualification green, human acceptance pending

**Target:** native port only; the matching N64 build remains unchanged

**Compatibility:** supported retail ROMs, unchanged retail save and asset schemas

## Outcome

The first release should make Taj a secret, human-selectable character who rides
his magic carpet on every course and is intentionally overpowered. It should not
add an eleventh retail character row or turn the challenge-only carpet into a
fourth player vehicle.

Taj is instead a **native virtual character**:

```text
all three Taj challenges ─┐
                         ├─> persistent Taj unlock ─> virtual select entry
ABRACADABRA magic code ──┘                              │
                                                       v
                                      normal car/hover/plane authority
                                                       │
                          ┌────────────────────────────┼───────────────────────┐
                          v                            v                       v
                 Taj/carpet rendering          OP handling module      Taj UI/audio
```

The authoritative racer remains a normal track-selected vehicle with a
known-good retail character row.
A native sidecar records that the human chose Taj, hides the donor vehicle at
render time, synchronizes a carpet and rider presentation to it, and applies
Taj-only tuning after ordinary car, hovercraft, or plane physics. This preserves
checkpoints, laps, collision, items, doors, cameras, split-screen, and the
ten-character ROM data contract.

## Why this is the first-release design

The engine is not data-driven at the character boundary:

- `Character` contains ten IDs in
  [`game/include/enums.h`](../../game/include/enums.h), and `NUM_CHARACTERS` is
  ten in [`game/src/menu.h`](../../game/src/menu.h).
- [`gRacerObjectTable`](../../game/src/objects.c) contains ten car, ten hover,
  and ten plane rows. Race spawn and hub transforms index it as
  `character + vehicle * NUM_CHARACTERS`.
- [`game/src/runtime_contracts.c`](../../game/src/runtime_contracts.c) asserts
  that ten characters times three player vehicles is exactly the 30-row retail
  vehicle-sound table.
- Character-select navigation has four fixed layouts for 8, 9, 9, or 10
  characters in [`game/src/menu.c`](../../game/src/menu.c). Scene actor lists and
  counts are fixed in [`game/src/object_functions.c`](../../game/src/object_functions.c).
- Portraits, HUD sprites, shields, boost data, stats, steer divisors, marker
  height, horn banks, and voice banks contain direct ten-wide lookups.
- Ghosts store a character byte and later feed it back into the ten-wide racer
  table.

Increasing `NUMBER_OF_CHARACTERS` would be a coordinated retail-data migration,
not a character addition. The ROM contains useful Taj NPC textures, models,
animations, and voice clips, but no Taj racer rows for car, hovercraft, or plane.

The existing `VEHICLE_CARPET` path is also the wrong gameplay authority. It is a
special vehicle used by Taj's hub challenge. [`update_carpet`](../../game/src/racer.c)
temporarily selects plane physics and a plane camera, removes normal vehicle
sound, and expects a separately linked Taj object. Selecting it directly would
spread a fourth vehicle through fixed three-vehicle tables and expose challenge
state in ordinary races.

The virtual-character design contains those risks behind `NATIVE_PORT` and does
not alter the retail data model.

## Player experience

### Unlocks

Taj has two routes to the same global unlock:

1. **Earned route:** win the car, hovercraft, and plane Taj challenges in any
   Adventure save. The third first-time win triggers the unlock.
2. **Easter-egg route:** enter `ABRACADABRA` in Magic Codes. This spelling fits
   the 19-character input limit and references Taj's existing Abrakadabra clip.

The unlock is global because character selection happens before an Adventure
save is chosen. Per-save `Settings.tajFlags` still determines the three earned
challenge wins. The natural hook belongs in `mode_end_taj_race()` immediately
after its first-time `tajFlags` update; the complete mask is `0x08 | 0x10 |
0x20`.

On first unlock:

- play a short Taj celebration voice;
- show a localized, project-owned “TAJ HAS JOINED THE RACE!” message;
- enable Taj for the current session; and
- persist the global unlock after the current challenge/save update succeeds.

`ABRACADABRA` is checked in a native custom-code table before the retail-table
loop. It must not be appended to or represented as an index into the encrypted
`ASSET_MISC_MAGIC_CODES` blob. The retail table has 29 entries, while the list
renderer dereferences ROM text offsets for every set cheat bit; treating a spare
bit as row 29 would be an out-of-bounds read.

The custom-code layer exposes a synthetic “CONTROL TAJ” list row without forging
a retail row. It follows the existing unlocked/active split:

- `taj_unlocked` persists globally;
- `taj_enabled` is session state and defaults to the persisted value at boot;
- the synthetic row turns availability on or off without erasing the unlock;
- “clear all codes” disables it for the session but retains the unlocked row;
- deleting an Adventure file does not revoke it; and
- “erase all bonuses” clears it.

### Character select

When enabled, Taj adds the next contiguous **display identity** after the
currently available retail roster: index 8, 9, or 10 for an 8-, 9-, or
10-character save. A presentation-only Park Warden object gives that identity
a real standing, animated Taj in the lineup. `taj_select_layout` is the single
source of truth for both the two physical rows and every cursor direction, so
an invisible navigation-only slot cannot recur. Six-character lower rows use a
uniform safe-area scale; no individual character is hidden or displaced off
screen. This is not a new retail `Character` value. Selection must separate:

- cursor/navigation slot;
- display identity;
- presentation actor, voice, music, name, and portrait; and
- race asset identity.

Taj resolves to `CHARACTER_DIDDY` only when the menu writes retail race slots.
Diddy is the safest donor because every ordinary path supports it; Taj's OP
module supplies the stats, so donor performance is irrelevant. A per-player
native selection mask carries Taj identity through menu transitions and race
setup. Diddy remains independently selectable, and uniqueness checks operate on
display identity rather than donor ID.

Taj is human-only in v1. AI fill continues to choose from the ten retail
characters. The normal same-character code controls whether multiple humans may
choose Taj.

The authored select maps have no Taj actor, so the native presentation layer
composes one from the supported ROM's Park Warden model, preserves the retail
characters' authored transforms except for row spacing, and uses Park Warden
idle/greeting/magic animations for idle, hover, and confirmation. On hover Taj
raises the same P1–P4 placard geometry and textures as the retail cast; only the
placard batches are rendered, never a donor character. On track the layer
creates presentation-only Park Warden and carpet objects after scene load,
claims them before retail behaviour initialization, and uses the authored Park
Warden animation 6 carpet pose. None of these presentation objects runs
interactive `BHV_PARK_WARDEN`, character-select, or challenge-carpet behaviour.

### On-track fantasy

Taj appears on the magic carpet regardless of whether a course normally uses
cars, hovercraft, or planes. The course's authored vehicle remains authoritative;
“everywhere” describes Taj's presentation, not free flight:

- the carpet visually hovers and bobs just above the authoritative car pose;
- the hidden donor retains ground contact, collision, checkpoints, laps, zippers,
  item collection, doors, and out-of-bounds rules;
- normal vehicle camera and recovery logic remain authoritative; and
- the carpet does not cross walls, skip checkpoints, or bypass locked doors.

Initial tuning should feel shamelessly strong but remain steerable:

| Property | Initial target |
|---|---:|
| Acceleration response | 1.50× ordinary car result |
| Sustainable top speed | 1.35× ordinary top-speed result |
| Speed retained through hard steering | at least 85% |
| Drift-release magic dash | 45 ticks |
| Dash cooldown | 120 ticks |
| Ordinary spin/squish/bubble effects | immune |
| Banana loss from ordinary hits | none |

These are named constants in one module and require playtest tuning. The existing
global velocity safety clamp remains in force. Item use and weapon hits against
other racers remain unchanged. Out-of-bounds recovery, course progression, and
finish logic are never bypassed.

The shared Taj tuning helper runs immediately after ordinary car, hovercraft, or
plane updates in both player and finish/AI dispatch paths. Authored loop and
flying-car trigger states retain their movement authority while Taj's attack
immunity remains active. The helper modifies physics results; it does not
continuously overwrite `boostTimer` or fork the vehicle simulation. The dash
owns separate native state and requests standard boost effects only for
presentation.

## Runtime architecture

### Taj identity service

`game/src/taj_mod.c` and `taj_mod.h` are the only owners of Taj state. Their
public surface answers:

- whether Taj is globally unlocked and enabled;
- whether a select slot or controller selected Taj;
- whether a live human racer is Taj;
- which retail character ID to serialize or pass to ROM-indexed code; and
- whether canonical record/ghost writes are allowed.

No call site infers Taj from `CHARACTER_DIDDY`. Identity comes from the sidecar.
No value outside `0..9` may enter retail `Character` fields,
`Settings.racers[]`, racer object tables, ghost data, or other ROM-indexed rows.
Add debug assertions at each boundary.

Sidecar state is keyed by stable controller/player slot during menus and by
`playerIndex` for a live racer. It resets on title return, re-selection, player
departure, level teardown, and object cleanup. It survives menu-to-race and hub
transforms. Do not use `Object_Racer.unk154`; existing AI/challenge systems
already overload that link.

The two identities remain separate after binding: character select and results
query the stable settings slot, while physics, HUD, audio, and carpet ownership
query the live-racer mask. This is required when player two leads a two-player
Adventure race, because retail swaps the two live `playerIndex` values without
reordering `Settings.racers[]` or the results cards.

### Persistence

Create a versioned mod-state service behind a platform-neutral C interface. The
desktop backend may use the same atomic preferences mechanism as
[`platform/app/app_config.cpp`](../../platform/app/app_config.cpp); Web uses its
browser-persistence equivalent. At minimum it stores:

```text
taj_mod_version=1
taj_unlocked=0|1
adventure_migration_complete=0|1
```

The migration tombstone distinguishes a genuinely pre-mod save (which may
import an existing all-three-challenges completion) from an explicit “erase all
bonuses” reset, which must stay reset. Valid Adventure slots are reconciled at
title boot before the first character-select visit.

Do not grow `SaveFile`, `SaveConfig`, or the 512-byte EEPROM image. Although
bits 26–55 are preserved as unknown config flags, assigning one to Taj would
claim undocumented retail state and make external save-tool semantics ambiguous.
A sidecar keeps imported/exported retail saves byte-compatible and establishes a
reusable home for later mods.

Persistence failure is non-fatal but visible: keep Taj active for the session,
log the failed save, and tell the player that the unlock could not be saved. The
write is atomic, and “erase all bonuses” uses the service rather than deleting
files directly.

### Carpet and rider presentation

The donor racer is created normally. Once its object and segment are valid, the
Taj module creates two native-owned presentation companions:

1. a carpet render object using the compatible existing flying-carpet model;
2. a rider render object using the Park Warden/Taj model and controlled seated
   and celebration animations.

They synchronize after physics and before rendering. The donor model, shadow,
and water wake are suppressed only while the full Taj composition is healthy;
collision, particles, item attachment, and racer state remain alive.
Split-screen renders the same companions from each camera, never one copy per
viewport. The binding is retained through the finished-human-to-AI handoff so
the carpet remains present in finish cameras and result transitions.

Creation/destruction is transactional. If either race asset fails validation,
destroy any partial companion and retain the playable donor presentation. Racer
destruction and level teardown own explicit cleanup. Object pointers are never
left for the allocator to discover.

Phase 0 validates these asset candidates at runtime:

- `ASSET_OBJECT_ID_FLYINGCARPET` / `ASSET_OBJECTMODEL_MAGICCARPET`;
- `ASSET_OBJECT_ID_PARKWARDEN` / `ASSET_OBJECTMODEL_PARKWARDEN`; and
- model, animation, attachment, scale, and segment assumptions for a
  presentation-only instance.

The probe may reuse IDs from the user's ROM. It must not commit extracted
textures, model blobs, ROM screenshots, or other copyrighted game assets. New
portrait/badge work outside character select is project-owned with recorded
provenance.

### UI and audio resolvers

HUD, results, selection, flags, horn, and voices go through Taj resolvers before
ten-wide tables. V1 needs:

- the modeled Taj actor and retail player placard in select, plus project-owned
  Taj/carpet identity treatment in HUD, rankings, and results;
- localized Taj name and unlock/code descriptions;
- explicit Taj clip IDs from `sound_ids.h`, not character-ID arithmetic;
- a deliberate horn/loop policy; and
- a generic fallback for any screen not yet Taj-aware.

The audit covers select portraits/music, HUD, results/rankings, challenge/trophy
screens, world flags, shields, magnets, boost effects, and multiplayer layouts.
Assertions turn a missed ten-wide access into a diagnostic instead of corruption.

### Records and ghosts

Taj can play Time Trial, but an OP run is noncanonical:

- do not overwrite best times, staff-ghost progress, initials, or player ghosts;
- do not serialize a Taj-only ID;
- mark the result as modded/non-recordable in native UI and trace output; and
- preserve the previous canonical record unchanged.

Adventure and trophy progress may save normally. This is an intentional secret
character advantage; only competitive timing data is quarantined.

## Expected implementation surface

The design intentionally avoids editing the global character enum or the 30-row
runtime contract. Expected touchpoints are:

| Area | Likely files | Responsibility |
|---|---|---|
| Taj core | new `game/src/taj_mod.c`, `game/src/taj_mod.h` | unlock/session state, display-to-donor mapping, racer sidecars, tuning constants, lifecycle |
| Magic codes/select | `game/src/menu.c`, `game/src/menu.h` | custom-code parser/list row, virtual select entry, confirmation mapping, localized presentation |
| Select presentation | `game/src/taj_select_layout.*`, `game/src/taj_visual.*`, `game/src/object_functions.c` | shared navigation/row layout, standing Taj actor, retail player placard, lifecycle |
| Race lifecycle | `game/src/objects.c` | challenge-completion hook, companion creation/destruction, race/hub transitions |
| Physics/records | `game/src/racer.c` and record/ghost call sites | post-car OP tuning, immunities, modded-time suppression |
| HUD/results/audio | `game/src/game_ui.*`, `game/src/menu.c`, sound call sites | Taj-safe portrait/name/voice/horn resolvers |
| Persistence | new narrow `platform/mod_state.*`, desktop and Web backends | versioned atomic global mod state |
| Tests | `tests/test_*`, `tests/check_*`, input scripts, `tools/run_checks.py` | ROM-free units, end-to-end unlock/race coverage, positive controls |

Any attempted change to `NUMBER_OF_CHARACTERS`, `NUM_CHARACTERS`,
`RUNTIME_VEHICLE_SOUND_ROWS`, `SaveFile`, `SaveConfig`, or the retail magic-code
asset is a design escalation and must stop for review.

## Work packages

### Phase 0 — asset and lifecycle spike (0.5–1.5 days)

- Probe carpet and Park Warden object/model headers.
- Spawn, animate, synchronize, and destroy presentation-only instances on one
  track without changing physics.
- Verify GL, WebGPU, and one split-screen run.
- Validate the physical select actor, placard extraction, and lifecycle.

**Gate:** no gameplay work starts until repeated load/restart/teardown passes
without invalid access and the carpet/rider composition is visually credible.

### Phase 1 — mod state and unlocks (1.5–2.5 days)

- Add versioned platform-neutral mod state and atomic backends.
- Add `ABRACADABRA` and a safe synthetic list row.
- Hook the complete challenge mask and first-unlock feedback.
- Define boot, clear-codes, delete-save, erase-bonuses, and failure behavior.
- Ship ROM-free unit tests and positive controls.

### Phase 2 — virtual select and identity (2–4 days)

- Separate cursor, display, presentation, and race identities.
- Add Taj navigation, feedback, multiplayer rules, and P1–P4 sidecars.
- Canonicalize Taj to Diddy at every retail boundary.
- Add the physical select actor and numbered placard.
- Keep Taj out of AI assignment.

### Phase 3 — carpet composition (3–5 days)

- Create companion lifecycle and base-racer draw suppression.
- Add hover/bob, seated animation, scale, shadows/effects, and recovery states.
- Add Taj portrait, HUD, result, voice, horn, and sound resolution.
- Verify every hub transform and race teardown path.

### Phase 4 — OP handling and record quarantine (2–3 days)

- Apply centralized acceleration, top-speed, turn-retention, dash, and immunity.
- Tune across narrow, wide, icy, and vertical courses.
- Disable canonical Time Trial/ghost writes while retaining completion UI.
- Add deterministic identity/tuning/record-suppression traces.

### Phase 5 — hardening and evidence (3–5 days)

- Run the course/vehicle/player-count matrix below.
- Add sanitizer and assertion coverage for out-of-range retail IDs.
- Verify save imports/exports remain byte-compatible and persistence failures safe.
- Run clean-room/public-surface checks and record new-art provenance.

The core work in all five phases is implemented. The runtime asset probe
validated model IDs 203/208 and authored rider animation 6 on both supported
retail revisions; the fallback remains transactional if either asset contract
fails. “Implemented” is not a claim that every target below has completed human
acceptance. The qualification record distinguishes automated evidence from the
remaining playtest matrix.

## Qualification record and remaining matrix

Every gameplay run follows the repository audio-safety rule: `MDKR_AUDIO=0` and
a bounded `--headless-frames` argument.

### Automated ROM-free evidence

- Exact code match, near-match rejection, max-length input, and no access past 29
  retail rows.
- Persist/restore/clear/failure behavior for versioned mod state, including a
  failed transactional erase that preserves the prior durable unlock.
- Missing-directory atomic file installation and imported-save migration/reset
  semantics.
- Complete challenge mask unlocks once; partial/repeated wins do not.
- Display-to-donor mapping never returns an ID outside `0..9` to retail code.
- Taj resolvers never index ten-wide portrait, sound, stat, shield, or boost rows
  with a virtual ID.
- Time Trial/ghost policy preserves existing records byte for byte.

### Automated ROM-backed evidence

- The public Magic Codes keyboard accepts exact `ABRACADABRA`, returns through
  the frontend, selects Taj, starts a race, restarts it, and restores the global
  unlock after a process restart.
- All six orderings of the three challenge-completion flags unlock once; the
  existing Taj-challenge gate separately exercises the natural challenge paths.
- The 8+Taj, Drumstick+Taj, T.T.+Taj, and complete 10+Taj picker layouts all use
  the shared physical/navigation layout. Every ordinary hover remains ordinary.
- A temporary picker allocation failure recovers. Exhausting the retry budget
  shows an unavailable message and skips Taj instead of accepting an invisible
  character.
- P1, P2, P3, and P4 render the correct numbered placard. P1-owned Taj is raced
  beside Diddy in 2P, P3-owned Taj in 3P, and P4-owned Taj in 4P.
- A real `JOINTVENTURE` route selects Taj as P2, applies the production lead-row
  swap at the safe hub boundary, and proves the next Adventure race binds that
  stable row to live controller port 2 without exposing a virtual character ID.
- Car, hovercraft, and plane dispatch all retain their authored vehicle
  authority while applying the exact Taj identity and tuning bits.
- A real WebGPU race frame contains Taj, the red/gold carpet, and no donor draw;
  native GL picker evidence covers the four roster layouts, PAL, and 21:9, and
  a focused WebGPU picker run covers the complete ten-character roster.
- A real Chrome/WebGPU run proves every base-roster hover, Taj animation and
  confirmation, a durable IDBFS flush, and restoration after a reload without
  re-entering the code.
- Taj's select voice, confirmation voice, horn identity, minimap colour, HUD,
  results identity, and finish binding have exact trace assertions. Two real
  Rankings captures, separated by a stage/menu teardown, also prove the native
  40x40 Taj portrait beside an unchanged Diddy control.
- All 47 legal course/vehicle pairs activate Taj's live identity and matched
  carpet/rider presentation; the paired speed gate measures car, hovercraft,
  and plane within 2% of the stated 1.35x sustained-speed profile.
- Time Trial completion preserves pre-existing canonical record tables and the
  direct ghost-writing policy remains fail closed.
- The focused picker and gameplay journeys pass native and combined ASan/UBSan
  builds. The broader repository release matrix is a separate release gate.

### Human acceptance and unclosed breadth

Before calling the feature release-qualified, play it on real display, audio,
and controller hardware. The manual pass should cover duplicate-Taj selection
with the same-character cheat; join/leave and deselect churn; natural
third-challenge celebration; toggle, Clear Codes, delete-file, failed
persistence, and Erase All Bonuses UI; pause “Select Character,” quit, title,
hub, boss, trophy, and challenge returns; and representative item, water,
collision, fall, recovery, and finish behavior.

The automation covers every legal course/vehicle pair for one Taj player, but
does not claim the full cross-product of all player counts and course outcomes.
It also does not yet provide an end-to-end Controller Pak import transaction;
the shared picker path, both supported ROM assets, and import hook are covered
at narrower boundaries. Those are explicit acceptance items, not implicit
evidence from the implemented architecture.

### Acceptance criteria

1. Both unlock routes persist and obey reset semantics without changing a retail
   save image.
2. Taj is independently selectable without removing Diddy or exposing an
   out-of-range character ID.
3. Every ordinary course renders Taj on the carpet while car authority keeps
   laps, collision, items, and recovery correct.
4. OP values are observable, deterministic, and confined to Taj.
5. Diddy and all ten retail characters remain behaviorally unchanged.
6. Taj never writes a canonical Time Trial record or incompatible ghost.
7. One- through four-player lifecycle tests find no leaked/stale companion.
8. Matching build is unchanged and no extracted ROM asset is committed.

## Deferred

- A true `CHARACTER_TAJ = 10` and 10→11 / 30→33 retail-data migration.
- Authored Taj car, hovercraft, and plane rows.
- A fourth independent carpet physics/camera vehicle.
- AI-controlled Taj or free flight/checkpoint bypass.
- A modified retail character-select map asset.
- Taj-compatible serialized ghosts or leaderboard semantics.

Those may become a second-generation asset-pack feature after the virtual
character proves the mod architecture.
