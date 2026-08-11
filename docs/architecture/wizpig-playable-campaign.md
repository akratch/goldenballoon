# Playable Wizpig — campaign plan

**Status:** superseded by the combined Wizpig/Terry implementation campaign in
[`playable-wizpig-terry-campaign.md`](playable-wizpig-terry-campaign.md). This
document preserves the deeper original Wizpig investigation.

**Target:** native port only. Matching N64 builds, retail saves, and retail
asset schemas remain unchanged.

**Working branch:** `codex/wizpig-playable-scope`

## Decision summary

Implement Wizpig as a second native **virtual character**, alongside Taj. Do
not add `CHARACTER_WIZPIG`, increase `NUMBER_OF_CHARACTERS`, or put an eleventh
ID through any retail table or serialized record.

- Gameplay authority: Krunch's normal car, hovercraft, or plane racer row.
- Balance: Krunch's stock weight, handling, reactions, hit states, banana loss,
  and boost behavior, with a measured 1.04x acceleration response and 1.04x
  sustainable speed. No Taj immunity, drift dash, or turn-retention floor.
- Car/hover presentation: keep the authoritative Krunch vehicle and its
  attachments, omit only the Krunch driver batches, and compose the seated
  Wizpig rider from the rocket model over it.
- Plane presentation: hide the complete donor and compose the complete
  Wizpig-and-rocket model. Plane physics, camera, collision, checkpoints, and
  recovery remain authoritative.
- Unlock: beating Wizpig 2 in any Adventure save unlocks him globally. Add the
  synthetic native magic code `WIZPIGPOWER` as the shortcut and a separate
  `CONTROL WIZPIG` row in the code list.
- Competitive policy: Wizpig Time Trial runs finish normally but do not update
  canonical times, initials, staff-ghost retirement, or player ghosts.

The code string is a product label, not an architectural dependency; it can be
changed before implementation without changing this plan.

## Why this must extend Taj's virtual-character architecture

The retail boundary is still ten characters and thirty ordinary vehicle rows:

- `Character` ends at ten IDs and the player-vehicle enum remains car, hover,
  and plane.
- `gRacerObjectTable` is indexed as `character + vehicle * NUM_CHARACTERS`.
- Weight, handling, sound, portraits, flags, boost geometry, and ghost paths
  contain direct ten-wide lookups.
- Character select currently adds Taj only as a display index. It resolves the
  selected racer to an in-range Diddy donor before writing a retail slot.

Wizpig's boss vehicles are not reusable as gameplay authority. `VEHICLE_WIZPIG`
and `VEHICLE_ROCKET` run boss state machines, manipulate the race-start timer,
clear attacks, play boss outcomes, and assume boss-race camera/finish behavior.
Making a human racer use those IDs would be a new vehicle implementation and
would violate the request for ordinary heavy-racer behavior.

The virtual design instead keeps all race rules on a proven retail racer and
changes identity, tuning, and presentation only behind `NATIVE_PORT`.

## Asset findings

The US 1.1 and PAL 1.1 supported revisions share this asset payload. The ROM
corpus parser confirms:

| Asset | Model ID | Geometry | Use |
|---|---:|---:|---|
| Empty plane | 200 | 111 vertices / 80 triangles / 11 batches | Evidence that vehicle-only art exists; not needed for the rocket path |
| Empty car | 201 | 75 / 58 / 9 | Useful reference/fallback prototype, but has no spawnable object header |
| Empty hovercraft | 202 | 87 / 67 / 9 | Spawnable through `ASSET_OBJECT_ID_EMPTYHOVER` |
| Wizpig | 213 | 740 / 561 / 58 | Standing select actor |
| Pig Rocketeer | 214 | 887 / 654 / 68 | Cutscene behavior; not the preferred race object |
| Wizpig rocket | 215 | 887 / 654 / 68 | Seated rider and complete flying presentation |

The combined rocket model's batches separate cleanly by authored textures. The
rocket uses the rocket-environment, red-stripe, and lightning textures; the
remaining batches are Wizpig's cape, skin, robe, face, hair, and hooves. Phase
0 must turn that observation into an exact immutable schema probe before any
batch is filtered.

The empty car and empty plane models have no ordinary object headers. Building
synthetic headers or replacing cached model instances would add lifecycle and
reference-count risk for no gameplay benefit. Prefer filtering the one known
Krunch donor's combined mesh while retaining its vehicle attachments.

Krunch's car and hovercraft each have six distance models. Models 0–4 retain
separable driver and vehicle batches. The farthest model is too collapsed for
reliable composition, so the Wizpig-only draw path should cap its **draw-local**
donor LOD at 4. Do not write the authoritative `obj->modelIndex`; it is part of
the simulation hash. The cap must reuse the existing draw-local LOD seam.

## Target architecture

### 1. Generalized mod-racer identity

Refactor the one-character Taj sidecar into a small roster service with an
explicit identity enum:

```c
typedef enum ModRacerIdentity {
    MOD_RACER_RETAIL = 0,
    MOD_RACER_TAJ,
    MOD_RACER_WIZPIG,
} ModRacerIdentity;
```

It owns, in one place:

- unlocked and session-enabled bits per virtual character;
- selected identity per stable Settings/results slot;
- live identity per racer `playerIndex` after binding;
- display-index-to-identity resolution;
- donor mapping (`Taj -> Diddy`, `Wizpig -> Krunch`);
- result/HUD/audio identity resolution; and
- the modded-run latch used by record and ghost writes.

Every retail boundary receives only `0..9`. No caller may infer Wizpig from
Krunch, just as no caller may infer Taj from Diddy. A normal Krunch must remain
independently selectable beside Wizpig.

Keep stable selection identity separate from live racer identity. The existing
P1/P2 Adventure lead-row swap changes live player bindings without reordering
every Settings/results consumer. The generalized API must preserve the exact
Taj behavior for both identities.

### 2. Persistence and unlock migration

Keep the existing atomic file path (`taj_mod_state.ini`) for compatibility, but
upgrade its schema to version 2 and accept version 1 as input:

```text
mod_roster_version=2
taj_unlocked=0|1
taj_migration_complete=0|1
wizpig_unlocked=0|1
wizpig_migration_complete=0|1
```

Version-1 parsing must preserve both Taj fields exactly, initialize Wizpig to
locked/unmigrated, and write version 2 only through the existing atomic
transaction. Do not rename or delete the old file during this campaign.

The Wizpig earned condition is `settings->bosses & 0x20`, the bit written by a
Wizpig 2 win. Reconcile it when any valid cart or Controller Pak Adventure save
is decoded, as Taj already does for completed challenges. The migration
tombstone is required: after Erase All Bonuses, scanning the same completed
save must not immediately unlock Wizpig again.

The persistence transaction must remain shared across both characters. One
generation owns one candidate snapshot; a second action while Web/IDBFS is busy
queues a retry and is not reported as a failed write. Stale success/failure
callbacks may not settle a newer generation.

Unlock flow:

1. A first Wizpig 2 win sets the retail boss bit through the existing path.
2. The roster service observes the completed bit and enables Wizpig for the
   session.
3. It atomically persists the new global state and queues one announcement.
4. A store failure leaves the session unlock usable and exposes Retry.
5. A later valid-save reconciliation repairs an interrupted first unlock.

`WIZPIGPOWER` must be checked in the native custom-code layer before the 29-row
retail table walk. It adds a synthetic code-list row; it must never forge a bit
or index into `ASSET_MISC_MAGIC_CODES`.

### 3. Character select and roster layout

Raise only the native runtime capacity from 11 to 12. Leave
`NUM_CHARACTERS == 10`.

Generalize `taj_select_layout` into the one source of truth for every enabled
virtual slot, actor transform, and cursor edge. The layouts must cover all
retail unlock combinations and these virtual combinations: Taj only, Wizpig
only, and both.

When both are enabled, keep Taj on the lower secret row and move Wizpig to the
upper row only when appending both to the lower row would create a seventh
slot. This keeps the supported geometry at 4–6 actors per row:

- default retail roster + both: 5 top / 5 bottom;
- one retail secret + both: 5 or 6 top / 5 or 6 bottom;
- complete retail roster + both: 6 top / 6 bottom.

Add a physical Wizpig actor from `ASSET_OBJECT_ID_WIZPIG`, a separately owned
P1–P4 placard, explicit Wizpig voices, neutral select music, and independent
hover/confirm state. Taj and Wizpig allocation/recovery must be independent:
one unavailable actor must make only its own slot unselectable.

The visual and navigation table are one transaction. Never accept an invisible
virtual slot, and never leave a visible actor after its sign fails. Preserve
the same-character cheat semantics and keep both virtual characters human-only.

### 4. On-track car and hovercraft composition

For a healthy Wizpig composition:

1. Keep the Krunch racer, vehicle mesh, wheels/fan, collision, shadow, wake,
   items, and sounds authoritative.
2. At `render_mesh`, skip only the schema-proven Krunch **driver** batches for
   the current car/hover model.
3. Draw the seated Wizpig portion of model 215 and skip only its schema-proven
   rocket batches.
4. Synchronize the rider to the authoritative racer, including car bob,
   y/x/z rotation offsets, squash/stretch, opacity, recovery, and finish-camera
   handoff.
5. Suppress the rider's duplicate shadow. Keep the ordinary car/hover shadow
   and its normal effect anchors.

The composed rider must reflect stock hits because Wizpig is not Taj: spin,
squish, bubble, tumble, and banana-loss behavior remain live. Extract a shared
pure presentation transform from the current donor-only tumble/bob/stretch
logic so the rider receives the same draw transform without becoming a racer or
mutating authoritative state.

If the rider, model probe, batch fingerprint, or LOD selection is invalid,
atomically stop filtering and draw the complete Krunch donor. A complete donor
fallback is preferable to an invisible racer or a driverless vehicle.

### 5. Flying composition

For plane authority and authored plane-state variants (`VEHICLE_PLANE`, loop,
and flying-car transitions):

- hide the complete donor only after model 215 is validated and live;
- draw the full seated Wizpig-and-rocket object;
- keep plane physics and the plane camera untouched;
- anchor shield, magnet, boost, and finish presentation to the rocket;
- use the rocket's one correctly scaled shadow; and
- add presentation-only exhaust without running `update_rocket()` or spawning
  an authoritative boss racer.

Choose flying presentation from the track-selected player vehicle, not from a
one-tick transient alone, so loop/flying-car states cannot make the model swap
between Krunch and the rocket.

The player must remain ordinary plane authority even on Wizpig 2. The CPU boss
continues to use `VEHICLE_ROCKET`; the player merely looks like the same
character. No boss voice table, start boost, hazard, or boss-finish logic may
run for the player.

### 6. Balanced heavy-racer tuning

Use Krunch as the donor and apply a narrow post-vehicle helper:

| Property | Target |
|---|---:|
| Positive acceleration gain | 1.04x Krunch |
| Sustainable forward speed | 1.04x Krunch, per vehicle |
| Weight | stock Krunch |
| Handling / steering loss | stock Krunch |
| Hit states and banana loss | stock |
| Drift dash / immunity | none |

The helper runs after ordinary car, hovercraft, and plane updates in both human
and finish/AI dispatch paths. It must not fork a vehicle solver, overwrite
`boostTimer`, change vertical velocity, or bypass collision/recovery.

As with the corrected Taj cap, the 1.04x value is a cruising target. While a
stock boost is active, raise the cap to at least the stock result but do not
stack the Wizpig multiplier on the boost. Measure actual route percentiles;
constants alone are not evidence of 4% in three different drag models.

### 7. UI, audio, and records

Add identity-based resolvers rather than more donor checks:

- project-owned 40x40 Wizpig portrait for HUD, Rankings, results, and the giant
  Fire Mountain/Smokey Castle flags;
- distinct name/label, select confirmation, positive/negative voices, horn,
  and minimap color;
- Krunch's ordinary engine loops by design;
- no donor name, portrait, voice, horn, or map color on any screen; and
- normal finish announcements even when persistent record writes are skipped.

Generalize Taj's Time Trial quarantine to any non-retail identity. Guard only
the canonical writes and staff/player ghost mutations, not the enclosing result
or announcement block.

## Taj regression-prevention register

The Wizpig campaign is not complete unless each historical Taj failure class
has an explicit gate:

| Taj rollout failure class | Required Wizpig/generalized protection |
|---|---|
| Test bootstrap polluted persisted unlock/selection state | Environment hooks are read-only shadows at every accessor; genuine unlock still writes and announces |
| Busy Web persistence was reported as failure; migration advanced before bytes landed | One owner per generation, queued retry, migration advances only on accepted durable state |
| Virtual display identity leaked into retail rows | Boundary assertions and donor resolver; no value outside `0..9` reaches tables, settings, or ghosts |
| P2 Adventure lead swap lost identity | Stable settings identity and live racer identity tested separately before/after the production swap |
| Navigation exposed an invisible Taj | One layout drives cursor and actors; unavailable presentation makes only that slot untargetable |
| Failed sign left a visible but unselectable actor | Actor/sign lifecycle is transactional with symmetric teardown |
| Oversized/duplicate select and race shadows | Scale from header/object ratio; one shadow per composed racer; explicit 1P and split-screen gates |
| Partial companion loss hid the donor or left an orphan | Filtering/suppression is conditional on complete healthy composition and revoked before deferred free |
| Companion narrowed a shared model cache and froze Taj's ceremony | Presentation spawn lease never calls behavior-scoped `model_anim_offset`; cache-width gate covers Wizpig boss/credits consumers |
| Presentation objects entered the simulation hash/shared caches | All composed actors, exhaust, and descendants are excluded from authoritative object count/hash and never mutate shared model material state |
| Extra companions could exceed `gObjPtrList` | Capacity refusal remains pre-store; four-player maximum allocation and fallback are measured |
| Taj speed cap erased zip-pad boosts | Paired boosted/non-boosted Krunch/Wizpig speed gate across car, hover, and plane |
| Time Trial guard skipped the whole finish block | Gate record bytes and ghost state separately from voice/results progression |
| Finished-human CPU handoff dropped identity | Per-racer selected latch survives `raceFinished` handoff and is cleared at teardown |
| Diddy appeared in HUD/results/challenge portraits | Positive Wizpig portrait witnesses plus ordinary Krunch negative controls in every portrait surface |
| Shield/magnet/shadow stayed on hidden donor | Vehicle-dependent visible anchor resolver, tested in all three vehicle classes |
| A test passed with no actor/portrait pixels | Absolute pixel floors, suppressed-presentation A/B controls, and gameplay-trace identity |

## Work packages

### Phase 0 — asset, pose, and batch-filter spike (1–2 days)

- Add a read-only/model-probe harness for models 43–48, 85–90, 213, and 215.
- Fingerprint every accepted batch boundary, texture family, model count, and
  supported revision. Fail closed on any mismatch.
- Render three isolated prototypes: seated rider on Krunch car, seated rider on
  Krunch hovercraft, and full rocket over plane authority.
- Prove draw-local LOD 4 fallback, one/split-screen shadows, stock tumble/squish,
  and transactional donor fallback.

**Gate:** no identity or persistence work starts until both renderers produce
credible captures and forced probe/allocation failures show a complete Krunch.

### Phase 1 — generalized roster state without behavior change (2–3 days)

- Introduce `ModRacerIdentity` and port Taj call sites to it.
- Convert selected/live masks to identity arrays or disjoint checked masks.
- Upgrade persistence to schema v2 with exact v1 migration.
- Generalize record quarantine, P2 lead swap, test hooks, async generations,
  and reset semantics.
- Run every existing Taj unit and end-to-end gate unchanged in outcome.

**Gate:** the refactor is Taj-equivalent before Wizpig can be enabled.

### Phase 2 — Wizpig unlock and character select (2–4 days)

- Hook first Wizpig 2 completion and valid-save import reconciliation.
- Add `WIZPIGPOWER`, the synthetic code-list row, announcements, retry text,
  clear-codes, delete-file, and erase-bonuses behavior.
- Generalize the 8/9/10 retail layouts to 9–12 display identities.
- Add physical Wizpig actor/sign, voices, multiplayer ownership, and
  unavailable-state recovery.

### Phase 3 — race presentation (3–5 days)

- Land car/hover driver-batch filtering and rider composition.
- Land full rocket presentation for plane/loop/flying-car states.
- Add shared bob/tumble/stretch transforms, shadows, visible effect anchors,
  rocket exhaust, finish handoff, teardown, and determinism exclusions.
- Exercise Wizpig-vs-Wizpig boss races explicitly.

### Phase 4 — tuning and complete identity surfaces (2–3 days)

- Apply and measure the 1.04x Krunch profile across all three vehicles.
- Add portraits, labels, minimap color, horn, positive/negative voices, and
  explicit donor fallbacks.
- Quarantine Time Trial writes while preserving finish UI/audio.

### Phase 5 — hardening and release evidence (3–5 days)

- Run the complete route matrix, sanitizers, optimized builds, both renderers,
  browser persistence, PAL, 21:9, and resource plateau checks.
- Mutation-test the asset probes, identity bounds, record quarantine, and
  presentation A/B gates.
- Record the new portrait's project-owned provenance and update public docs.

Estimated focused implementation: **10–17 engineering days**, followed by
human controller/display acceptance.

## Verification matrix

### ROM-free tests

- v1 state parses and serializes to v2 without losing Taj.
- All unlock/enable/clear/erase/failure/retry combinations for both characters.
- Exact and near-miss custom codes; no access after retail row 28.
- Every retail-unlock x virtual-enable roster layout, position, and navigation
  edge, including 12-display-character full roster.
- Display identity maps to the correct donor and never outside `0..9`.
- P1/P2 stable/live swaps for Taj, Wizpig, ordinary Diddy, and ordinary Krunch.
- Pure 1.04x gain/cap functions, boost exemption, invalid-racer rejection, and
  stock attack/banana policy.
- Non-retail identity forbids canonical record/ghost writes.

### ROM-backed native/browser tests

- Earned unlock after a real Wizpig 2 win; no unlock after Wizpig 1, a loss, or
  an existing three-piece/progress control.
- Exact code entry, code-list toggles, clear codes, process restart, failed
  desktop write, rejected IDBFS commit, retry, and Erase All Bonuses.
- Character select at 8/9/10 retail racers with Taj only, Wizpig only, and both;
  P1–P4 signs; duplicate selection cheat; actor/sign allocation failure.
- Car, hover, and plane traces prove Krunch authority, Wizpig identity, expected
  visual class, and no boss vehicle dispatch.
- All 47 legal course/vehicle pairs, plus both Wizpig boss races, challenges,
  trophies, hubs, pause-select-character, restart, quit, and finish transitions.
- Stock attacks visibly spin/squish/bubble Wizpig and drop bananas; Taj remains
  unchanged and immune.
- Paired Krunch/Wizpig measurements land at 1.04x ± 0.02 for sustainable speed
  and acceleration, while boost peaks match stock behavior.
- HUD, Rankings, results, minimap, collection-wall portraits, horn, voices,
  shield/magnet anchors, shadows, and rocket exhaust each have a positive
  witness and an ordinary-Krunch negative control.
- Time Trial result appears and announces; pre-existing record/ghost bytes are
  unchanged.
- Native GL, native WebGPU, Chrome/WebGPU, PAL, 4:3, 16:9, and 21:9 captures
  meet absolute visibility floors and suppressed-presentation A/B controls.
- ASan/UBSan and optimized builds; repeated level churn and four-player
  allocation hold a stable resource plateau.
- Simulation hashes are identical across every presentation-only fault flag;
  ordinary Taj and retail control streams retain their expected baselines.

### Human acceptance

- Wizpig reads as seated in both kart and hovercraft, never standing through the
  body or hovering above the seat.
- Every plane course consistently shows the rocket, including loops, recovery,
  finish cameras, and split screen.
- He feels like Krunch with a small edge, not like Taj: impacts matter, turns
  cost speed, and 4% is noticeable only over a sustained comparison.
- No donor Krunch body, portrait, voice, or label leaks when composition is
  healthy.
- Taj's select, carpet, unlock, persistence, portraits, boost behavior, and
  challenge celebration remain unchanged.

## Expected implementation surface

Prefer generic names during Phase 1 rather than adding a parallel set of
Wizpig-only global masks. Likely touchpoints:

- `game/src/taj_mod.*` -> generalized roster identity service;
- `platform/taj_mod_state*` -> schema-v2 compatible state backend;
- `game/src/taj_select_layout.*`, `menu.c`, `object_functions.c` -> 12-slot
  layout and two physical virtual actors;
- `game/src/taj_visual.*`, `objects.c`, `tracks.c`, `platform/sim_hash.c` ->
  generalized presentation ownership plus Wizpig mesh filtering/rocket path;
- `game/src/taj_physics.*`, `racer.c` -> identity-aware tuning and record guard;
- `game_ui.c` and `menu.c` -> portrait/name/minimap/results resolvers;
- `save_data.c`, `vehicle_tricky.c` or the centralized boss-completion seam ->
  completed-save and first-win reconciliation;
- CMake/CTest registration, focused C units, native journeys, browser journeys,
  and shared route sweeps.

Any change to `NUMBER_OF_CHARACTERS`, `NUM_CHARACTERS`, the 30-row vehicle
sound contract, `SaveFile`, `SaveConfig`, the EEPROM image, or the retail magic
code blob is a campaign stop and requires a separate design review.

## Definition of done

1. Wizpig unlocks globally through Wizpig 2 or the custom code and survives
   relaunch/reset semantics without changing retail save bytes.
2. Taj, Wizpig, Krunch, and Diddy are independently selectable; no virtual ID
   reaches retail data.
3. Wizpig sits in a kart or hovercraft and uses his rocket on every flying
   course while ordinary vehicle authority remains intact.
4. His measured performance is Krunch +4% and he retains stock vulnerabilities.
5. Modded Time Trial records and ghosts cannot contaminate canonical data.
6. Every Taj regression class in the register has a positive control and Taj's
   existing suite stays green.
7. One- through four-player lifecycle, renderer, browser, sanitizer, and
   resource gates find no stale object, cache mutation, or hidden-donor state.
8. Matching builds are unchanged and no extracted ROM asset is committed.
