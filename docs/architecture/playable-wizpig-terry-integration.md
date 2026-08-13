# Playable Wizpig and Terry — integration guide

## Integration coordinates

- Source branch: `codex/wizpig-playable-scope`
- Branch point: `74b968936d90010c71557cbd58f405934a2923a3`
- Implementation commit: `1235879` (`feat: add playable Wizpig and Terry`)
- Documentation commit: the commit containing this file
- Scope: native-port-only virtual racers; matching N64 builds and retail
  serialized formats remain unchanged

The implementation is self-contained in `1235879`. The preferred integration
is to merge the branch so this guide and the implementation travel together.
Cherry-picking `1235879` is also supported when the lead needs a code-only
review first.

```bash
git cherry-pick 1235879
```

If main has moved through any of the core files listed below, resolve conflicts
semantically. Do not accept all of either side for `objects.c`, `menu.c`,
`taj_mod.c`, or `taj_physics.c`; each contains both inherited Taj safety rules
and the generalized three-racer identity path.

## Shipped product contract

Wizpig and Terry are virtual identities carried beside safe retail donor rows.
No new value enters `Character`, `NUMBER_OF_CHARACTERS`, ten-wide retail asset
tables, EEPROM character fields, ghost headers, or matching-build code.

| Identity | Presentation authority | Physics contract | Plane presentation |
|---|---|---|---|
| Taj | existing Diddy/carpet integration | existing OP Taj profile | existing carpet |
| Wizpig | Krunch vehicle plus schema-filtered Wizpig companion | Krunch weight/handling/hits; 1.04x positive unboosted gain and sustainable cruise | complete Wizpig rocket over hidden donor plane |
| Terry | Krunch vehicle plus schema-filtered Terry companion | Pipsy weight/handling/response; 1.03x positive unboosted gain and sustainable cruise | complete animated Terryboss pterodactyl over hidden donor plane |

The presentation donor and physics-stat donor are deliberately separate for
Terry. Changing `TERRY_MOD_DONOR_CHARACTER` to Pipsy would swap the vehicle
model family and invalidate the proven Krunch driver-batch composition. Terry
therefore retains Krunch for vehicle geometry while
`mod_racer_physics_stat_character()` selects Pipsy's retail stat row.

All virtual runs remain noncanonical for Time Trial records and ghosts. Wizpig
and Terry retain normal attacks, banana loss, collision, recovery, items, and
boost behavior. Neither can fall through into Taj's immunity, dash, or turn
retention.

## User-visible behavior

### Unlock and roster

- Native character-select capacity expands from 10 to 13 without changing the
  retail enum.
- Wizpig unlocks from Wizpig 2 progress or `WIZPIGPOWER`.
- Terry unlocks from the Dino Domain/Tricky rematch or `TERRYFLY`.
- The sidecar persistence schema is version 3 and migrates exact v1/v2 files.
- `CONTROL WIZPIG` and `CONTROL TERRY` independently enable or disable their
  roster entries.
- The picker owns independent actor/placard pairs and semantic dance, hover,
  confirmation, and standing poses for all three virtual racers.

### Vehicle presentation

- Kart and hovercraft keep the authoritative Krunch vehicle, attachment,
  collision, camera, and effect anchors. Only schema-verified driver batches
  are hidden after the companion is healthy.
- Wizpig is scaled to occupy the vehicle similarly to Krunch and never carries
  the rocket shell on ground vehicles.
- Terry uses a compact perched pose on kart/hover and his complete fly cycle on
  plane tracks.
- Plane mode hides the donor plane WHOLE, so no batch index of it is consumed
  and there is no carve a wrong geometry fingerprint could misapply. The plane
  therefore asserts donor IDENTITY (`modelIds` really are `KREMPLANE_0..5`) and
  companion health, but no per-LOD vertex/triangle/batch table -- unlike the
  kart and hovercraft, whose driver-batch carve does require the full
  fingerprint. Allocation, identity or companion failure draws the complete
  donor instead of a partial composition.
- Each composition keeps exactly one shadow. Presentation companions are
  excluded from authoritative racer transforms and simulation hashing.

### UI identity parity

- Taj, Wizpig, and Terry each own a project-authored 40x40 native-port card.
- The shared identity resolver covers Rankings, post-race results, portrait-
  bearing HUDs, and challenge character flags.
- Names, picker actors, placards, menu poses, race labels, and minimap colors
  are identity-owned; Krunch does not leak into Wizpig or Terry UI.
- The cards are generated from code-native pixel primitives. No extracted ROM
  portrait or generated capture is committed.

### Terry flight presentation

- Continuous plane engine and idle handles are stopped only for Terry in plane
  mode.
- The authored boss flap cue (`SOUND_UNK_223`, sound 547) fires once on the
  fly-cycle 28-to-32 subframe crossing, including catch-up ticks.
- Donor plane wing-line emitters 3/4 and boosted emitters 7/8 are filtered at
  the vehicle-particle spawn boundary only for Terry in plane mode.
- Particle filtering does not mutate racer state and does not affect surface,
  item, impact, kart, or hovercraft particles. Existing trails fade normally.
- Terry intentionally has no borrowed race voice bank. Suppression is preferred
  to leaking Krunch grunts or Smokey dialogue.

## Architecture and file ownership

| Area | Primary files | Merge-critical responsibility |
|---|---|---|
| Virtual identity and unlocks | `game/src/taj_mod.[ch]` | Three-identity roster, stable/live binding, donor resolution, unlock policy, record quarantine |
| Persistence | `platform/taj_mod_state.[ch]`, `game/src/save_data.c` | Exact v1/v2 migration, v3 serialization, async generation/failure rules, progress reconciliation |
| Picker layout | `game/src/taj_select_layout.[ch]`, `game/src/menu.c` | 10–13 slot layout, navigation, actor state, unlock rows, donor-free results lookup |
| Shared presentation | `game/src/bonus_character_visual.[ch]` | Scale/offset helpers, Krunch driver-batch schema, shadow scaling |
| Wizpig presentation | `game/src/wizpig_visual.[ch]` | Companion leases, schema validation, pose sync, LOD/batch filtering, lifecycle |
| Terry presentation | `game/src/terry_visual.[ch]` | Companion leases, perched/fly animation, flap audio, picker poses, lifecycle |
| Render integration | `game/src/objects.c`, `game/src/object_functions.c`, `platform/sim_hash.c` | Presentation-actor classification, transform bypass, draw/shadow/LOD hooks, hash exclusion |
| Physics | `game/src/taj_physics.[ch]`, `game/src/racer.c` | Identity-specific post-solver tuning, Pipsy stat-row selection, boost non-stacking |
| Audio and particles | `game/src/audio_vehicle.c`, `game/src/particles.c` | Terry plane-engine suppression, flap ownership, wing-trail filtering |
| HUD/UI | `game/src/game_ui.c`, `game/src/menu.[ch]` | Names, cards, challenge flags, HUD cards, results cards, minimap colors |
| Lifecycle/transition guards | `game/src/tracks.c`, `game/src/vehicle_tricky.c`, `game/src/taj_visual.c` | Teardown, boss unlock hook, inherited Taj-safe transitions |
| Regression inventory | `tests/`, `tools/run_checks.py` | ROM-free contracts and ROM-backed picker, portrait, audio, particle, and Taj regressions |

The full rationale and acceptance matrix live in
`docs/architecture/playable-wizpig-terry-campaign.md`. The deeper original
Wizpig investigation remains in `docs/architecture/wizpig-playable-campaign.md`.

## Merge-critical invariants

1. Keep all virtual IDs out of retail arrays and serialized character fields.
2. Preserve stable Settings-slot identity separately from live racer identity;
   two-player Adventure swaps those authorities at different times.
3. Presentation actors have `BHV_RACER` headers for asset loading but are not
   initialized authoritative racers. The `bonus_visual_is_presentation_actor()`
   exclusions in `objects.c` prevent zeroed racer tails from flattening models,
   corrupting LOD, or leaking temporary transforms.
4. Never hide donor geometry before the companion and every resident donor LOD
   pass their schema checks. Failure must show a complete donor. "Exact" means
   the full geometry fingerprint wherever specific batch indices are carved out
   (kart and hovercraft donors, and Wizpig's rocket-only rider batches); where
   an object is hidden whole, identity alone is what has to hold.
5. Do not mutate shared `ObjectModel`, animation, material, or model-index data.
6. Terry's Pipsy stat-row lookup must not change his Krunch presentation donor.
7. Wizpig/Terry performance multipliers apply only to positive, unboosted
   authority and sustainable cruise; stock boost magnitudes remain unchanged.
8. Terry wing-trail suppression uses the local emitter mask in
   `update_vehicle_particles()`. Do not clear authoritative emitter state.
9. Preserve Taj's legacy trace witnesses alongside generic roster traces; the
   older Taj regression suite consumes both.
9b. Presentation actors advance their animation CLOCK only. `obj_animate_tick()`
   owns the deformation and the published `curVertData` for every animated
   object; a module that also calls `obj_animate()` runs the deformation twice
   against one frame, leaving both halves of the double buffer identical so
   animated-vertex interpolation has nothing to blend. The `bonus_animation`
   trace reports `distinct=` for exactly this.
9c. Every `animationID` a module selects must be inside the model's
   `numberOfAnimations`; `obj_clamp_model_animation()` silently substitutes a
   different clip otherwise, so the bound belongs in the schema check.
10. All presentation companions remain excluded from canonical simulation hash
    state and from retail save/ghost formats.

## Conflict hotspots

### `game/src/objects.c`

`1235879` also reflowed the whole file to `.clang-format`, which buried 128
lines of real hooks inside a 3716-line diff. That reflow has since been reverted
on this branch: `git diff v1.2.1 -- game/src/objects.c` is now 18 hunks, all
semantic. Review or transplant by the function names below.

Note that `git diff -w --ignore-blank-lines` gives **zero** reduction against
the reflowed revision — `-w` normalises whitespace within a line and cannot see
through re-wrapping. Statement-level canonicalisation is what separates the two.

The complete hook inventory, in file order:

1. `wizpig_visual.h` / `terry_visual.h` includes.
2. `bonus_visual_is_presentation_actor()` — the shared exclusion predicate, with
   a `#else` arm so it is a compile-time `FALSE` on matching builds.
3. `bonus_visual_trace_transform_bypass()` — one-shot per-identity witness.
4. `free_all_objects()` — companion reset before the destruction walk.
5. `free_object()` and `obj_destroy()` — free/destroy lifecycle.
6. `obj_update()` — companion tick, after the authoritative racer loop.
7. `obj_authoritative_texture_tick()` — presentation-actor exclusion.
8. `obj_visibility_tick()` — presentation-actor exclusion.
9. `render_3d_model()` — **boss head-matrix push**. Bonus actors are claimed as
   `BHV_NONE`, so `racerObj` is NULL and retail's secondary head matrix is
   skipped; without this `else if` arm the `vertOverride` batches fold through
   the body matrix and the model visibly collapses. This is the hook whose
   omission the 13-racer picker test detects.
10. `render_object()` — donor draw suppression and placard-owner filtering.
11. `racer_model_index_for_view()` — donor LOD cap, gated on `allowLodBias` so
    it applies at the draw seam only. It must never reach the authoritative
    `obj->modelIndex`, which is part of the v3 simulation hash.
12. `obj_lod_tick()` — presentation-actor exclusion.
13. `set_temp_model_transforms()` — presentation-actor exclusion, plus the
    transform-bypass arm that keeps a neutral scale instead of reading a zeroed
    `stretch_height`.
14. `unset_temp_model_transforms()` — matching restore exclusion.
15. `render_mesh()` — placard batch filtering, schema-verified donor and rider
    render-batch filtering, and the bounded placard texture index.
16. `run_object_init_func()` — spawned-object claims before racer init.

After resolution, the 13-racer picker test is the fastest positive detector for
the former flattened/collapsed model defect.

### `game/src/menu.c` and `game/src/taj_mod.c`

These files generalize Taj-only state to a three-identity roster. Preserve both
the generic `mod_racer_*` API and compatibility wrappers/traces used by Taj
tests. Taking main wholesale can silently restore donor portraits or lose
Wizpig/Terry persistence; taking this branch wholesale can discard unrelated
menu work that landed after the branch point.

### `game/src/taj_physics.c` and `game/src/racer.c`

Preserve the pre/post vehicle seams, the finished-human identity latch, Taj's
boost-cap exemption, Wizpig's 1.04 contract, Terry's 1.03 contract, and Terry's
Pipsy stat-row lookup in both human and AI stat-loading paths.

## Qualification completed on the source branch

The following passed against the implementation commit and supported US Rev 1
ROM:

```bash
cmake --build build-wizpig --target mdkr64 -j8
ctest --test-dir build-wizpig -R '^taj_physics$' --output-on-failure
python3 tests/check_bonus_character_select.py --build build-wizpig \
  --rom baserom.us.v80.z64
python3 tests/check_bonus_results_portraits.py --build build-wizpig \
  --rom baserom.us.v80.z64
python3 tests/check_terry_flight_audio.py --build build-wizpig \
  --rom baserom.us.v80.z64
python3 tests/check_taj_results_portrait.py --build build-wizpig \
  --rom baserom.us.v80.z64
python3 tests/check_taj_hud_portrait.py --build build-wizpig \
  --rom baserom.us.v80.z64
git diff --check
python3 -m py_compile tests/check_bonus_results_portraits.py \
  tests/check_terry_flight_audio.py
```

Observed ROM-backed results:

- 13 racers composed; Taj, Wizpig, and Terry actor/placard pairs and pose states
  passed.
- Distinct retail-sized Wizpig and Terry cards rendered in the real post-race
  flow.
- Terry produced 24 authored flap cues at the exact 48-tick cadence, 19
  zero-handle engine witnesses, a runtime Pipsy-row/1.03 profile witness, and
  exact plane-wing-emitter suppression; the kart retained 36 engine samples.
- The inherited Taj Rankings and two-player Adventure HUD portrait gates passed
  after the shared portrait resolver was generalized.

The complete 112-test native CTest run reported 111 passes. The only red was
`audio_ring_threaded`, which failed its four overflow/flood timing assertions
and failed the same way when rerun alone.

That red is resolved. It was a host-timing flake in the test itself, fixed on
main by `0c3cf96` ("test: make audio ring overflow stress deterministic"), which
this branch predated. v1.2.1 has since been merged in, so the gate is green
here; the branch never touched the audio ring implementation.

The commit hook also passed the clean-room scan: no ROM, extracted bulk asset,
capture, local machine path, or content-pack payload is tracked.

## Required post-merge checks

At minimum, repeat the build and six targeted commands above on the integrated
main tree. Then perform one manual pass with all three bonuses enabled:

1. Visit Player Select and observe unoccupied dance, hover/standing, and
   confirmation poses for Wizpig and Terry.
2. Race each new character in kart, hovercraft, and plane modes.
3. Confirm there is no Krunch body/skull leak, duplicate shadow, collapsed
   transform, or donor portrait.
4. Confirm Wizpig fills the vehicle similarly to Krunch and remains a subtly
   enhanced heavy racer.
5. Confirm Terry feels like a nimble Pipsy-class racer, has flap audio but no
   plane engine, and emits no wing contrails.
6. Finish a race and inspect the dedicated result card, name, and minimap color.
7. Run a Time Trial and verify the race completes but canonical record/ghost
   bytes remain unchanged.

For release breadth, follow with GL and WebGPU, multiplayer, browser WebGPU,
PAL, ASan/UBSan, resource plateau, and canonical save/ghost controls listed in
the implementation campaign.

## Deliberate non-goals and caveats

- Terry is the distinct boss pterodactyl asset, not the ambient level
  pterodactyl and not Smokey's dragon. It was not an original retail player
  model; the native player integration is authored by this project.
- Terry has no authored player voice bank. The implementation does not invent
  one or borrow another character's dialogue.
- Matching N64 builds remain retail. The bonus roster is native-only by design.
- Generated visual captures and the base ROM are qualification inputs only and
  must not be committed.
