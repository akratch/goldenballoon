# Playable Wizpig and Terry — implementation campaign

**Status:** implemented and locally qualified on the isolated
`codex/wizpig-playable-scope` branch.

This is the combined release plan. The deeper Wizpig investigation remains in
[`wizpig-playable-campaign.md`](wizpig-playable-campaign.md); Terry's verified
asset research remains isolated in the user-provided Terry research worktree.

## Product contract

Wizpig and Terry are native-port bonus racers. Neither changes the retail ten
character enum, thirty-row vehicle tables, EEPROM layout, ghost format, or
matching N64 builds.

| Racer | Donor authority | Kart / hover | Plane-class track | Performance |
|---|---|---|---|---|
| Wizpig | Krunch | Krunch vehicle with driver-only batches replaced by seated Wizpig | Full Wizpig rocket over hidden donor plane | Krunch weight/handling/hits; +4% unboosted acceleration and sustainable speed |
| Terry | Krunch presentation; Pipsy physics stats | Krunch vehicle with driver-only batches replaced by a scaled, perched Terry | Full animated Terry pterodactyl over hidden donor plane | Pipsy weight/handling/response; +3% unboosted acceleration and sustainable speed |

Both use normal item, collision, checkpoint, recovery, camera, finish, and
multiplayer systems. Wizpig never enters `update_wizpig()`/`update_rocket()`;
Terry never enters `update_smokey()`. Those are boss policy controllers, not
general racer physics.

Terry's “native flight” means the real `Terryboss` model and its authored fly
animation are the complete visible racer on plane tracks while ordinary plane
authority remains underneath. Kart/hover tracks always retain a physical
vehicle.

## Identity and compatibility boundary

`ModRacerIdentity` travels beside a safe retail donor:

```c
MOD_RACER_RETAIL
MOD_RACER_TAJ
MOD_RACER_WIZPIG
MOD_RACER_TERRY
```

The roster service owns unlock, session-enable, Settings-slot selection, live
racer binding, donor resolution, test identity, announcements, record policy,
and async persistence. Donor identity is never inferred: ordinary Krunch and
both Krunch-backed bonus racers remain independent choices.

The native character-select capacity is 13. The full roster is six actors on
the upper row and seven on the lower row, with a draw-only 0.82 scale on the
seven-actor row. Navigation comes from the same layout object as actor
placement. The retail `Character` and `NUMBER_OF_CHARACTERS` stay unchanged.

## Persistence and unlocks

The existing atomic sidecar path is retained and upgraded to exact schema v3:

```text
mod_roster_version=3
taj_unlocked=0|1
taj_migration_complete=0|1
wizpig_unlocked=0|1
wizpig_migration_complete=0|1
terry_unlocked=0|1
terry_migration_complete=0|1
```

Exact v1 and v2 documents migrate in memory; all new writes emit v3. A shared
generation-based transaction serializes simultaneous unlocks, ignores stale
browser callbacks, and restores a failed erase. Migration tombstones ensure
Erase All Bonuses is not immediately undone by rereading a completed save.

- Wizpig unlock: defeat Wizpig 2 (`settings->bosses & 0x20`) or enter
  `WIZPIGPOWER`.
- Terry unlock: defeat the Dino Domain/Tricky rematch
  (`settings->bosses & 0x80`) or enter `TERRYFLY`.
- Code list: independent `CONTROL WIZPIG` and `CONTROL TERRY` rows.
- Import: EEPROM and Controller Pak save scans reconcile both boss bits.

## Presentation transactions

Each bonus racer owns at most one presentation companion per live player. A
companion spawn is claimed before its original boss initializer executes,
stripped of behavior and interaction, and excluded from the authoritative
simulation hash.

Composition is fail-visible and transactional:

1. Spawn the presentation actor with the full entry size required by its real
   `BHV_RACER` header.
2. Verify the exact supported model schema and donor model-ID sequence.
3. Verify every currently resident donor LOD; tolerate normal lazy loading of
   other LOD slots.
4. Only then hide the donor driver or whole donor plane.
5. If allocation, schema, or lifecycle health fails, show the complete donor.

No shared `ObjectModel`, material, animation table, or authoritative
`modelIndex` is mutated. Car/hover LOD is capped at 4 only at draw resolution,
because Krunch's sixth model collapses driver and vehicle geometry.

Wizpig uses model 215: rocket-only batches are omitted in a kart/hover and the
complete model is shown in flight. Terry uses the distinct 205-vertex,
166-triangle, 19-batch `Terryboss` model with compact walk/perched and fly
animation states. The ambient pterodactyl and Smokey dragon assets are not
substitutes.

Wizpig's ground companion target is 0.30 (the header ratio produces the final
runtime scale). This was selected from same-camera A/B captures against the
earlier 0.22 composition: 0.30 occupies the kart like Krunch while all physics,
collision, camera, and effect anchors remain on the unchanged donor.

Player Select has an explicit semantic pose state machine instead of a fixed
animation. An unoccupied slot uses the actor's locomotion cycle as the retail
“dance”; hover resets to the authored idle/standing pose and displays the
controller placard; a newly confirmed Wizpig uses jump and Terry uses fly for a
bounded 60-tick flourish, then returns to the hover pose. Every animation change
resets its frame, preventing accidental interpolation between incompatible
skeleton poses. Terry's placard has a small actor-local vertical correction so
his head stays readable. Results screens intentionally keep the live racer
seated: those overlays are drawn over the still-authoritative race world and
are not a separate character-stage menu.

Taj, Wizpig, and Terry each own a project-authored 40x40 native portrait card.
The identity resolver shares those cards across Rankings/results, portrait-
bearing HUDs, and challenge character flags; no virtual racer falls through to
its donor portrait. Playable minimap markers are identity-owned as well: Taj
keeps retail magenta, Wizpig is crimson, and Terry is teal.

### Presentation-transform defect and permanent guard

The first integrated build exposed a subtle header/runtime-identity mismatch.
Both boss companions retain retail `BHV_RACER` object headers so their model
and animation assets load, but the presentation claim deliberately changes
their live behavior to `BHV_NONE` before `obj_init_racer()`. The generic render
prepass keyed only on the header, interpreted the zeroed object tail as an
initialized `Object_Racer`, and copied its zero `stretch_height` into
`gObjectModelScaleY`. That flattened every animated vertex on the Y axis in
both Player Select and gameplay.

Presentation companions are now excluded as a class from racer visibility,
LOD, texture, temporary-transform, and transform-restore paths. They keep the
neutral model Y scale and use their own animation publication. The ROM-backed
13-racer picker gate requires an explicit transform-bypass witness for both
Wizpig and Terry, so reintroducing the header-only classification fails before
visual review. Matching/N64 builds resolve the classifier to false and retain
their original behavior.

One shadow is retained per composed racer. Kart/hover keeps the donor vehicle
shadow; plane mode keeps the visible companion shadow. Presentation companions
are explicitly admitted to split-screen shadow generation while remaining
non-authoritative.

## Gameplay and audio policy

Wizpig receives a post-solver 1.04 multiplier only for positive, unboosted
acceleration gains and the sustainable cruise target. It never stacks with a
zip pad or blue boost. Attacks, spin, squish, bubble, bananas, steering loss,
vertical velocity, and collision remain stock Krunch.

Terry keeps Krunch as the presentation donor so the schema-qualified kart,
hover, plane, shadow, audio, and driver filtering remain stable, but reads
Pipsy's retail weight, handling, and response rows. A narrow post-solver 1.03
multiplier applies only to positive unboosted acceleration and sustainable
cruise, never to boosts. He retains ordinary attacks, banana loss, collision,
and recovery, and cannot fall through into Taj's immunity, dash, or turn
retention.

Terry has no authored voice bank. Race voice requests are suppressed for Terry
instead of borrowing Krunch grunts or Smokey dialogue. Kart and hover engines,
items, impacts, ambience, and UI cues remain normal. In plane mode only, the
continuous donor engine and idle handles are stopped at the audio-player seam;
physics still updates the donor's sound state and no global sound asset is
mutated. The visible fly animation emits `SOUND_UNK_223` on the exact 28→32
subframe crossing used by `update_smokey()`, through the same spatial playback
function and ROM-authored level/range/pitch data. The resulting cycle is one cue
every 48 simulation ticks. Catch-up crossing logic prevents a slow present from
dropping the cue. Wizpig uses his existing short vocal clips in character select
and ordinary Krunch vehicle audio.

The donor plane's paired normal and boosted wing-line emitters are filtered at
the vehicle-particle spawn boundary only for Terry in plane mode. The filter is
presentation-only: it leaves racer state, surface effects, items, impacts, and
all kart/hover emitters untouched, while existing line particles fade through
the retail emitter shutdown path.

## Competitive and save policy

Any virtual identity latches the run noncanonical before vehicle update. Time
Trials still race, finish, show results, and announce outcomes, but cannot
write canonical lap/course records, retire staff ghosts, or create player
ghosts. The identity latch survives the finished-human CPU handoff and clears
only at race teardown.

## Regression gates inherited from Taj

- no virtual display index enters a ten-wide retail table;
- stable Settings identity remains separate from live P1/P2 Adventure binding;
- unavailable actor/sign pairs make only their own slot unselectable;
- actor/sign teardown is symmetric and cannot orphan a visible object;
- companion loss revokes donor filtering before deferred free;
- no presentation object enters simulation hashes or shared asset mutation;
- async unlocks coalesce without losing the last roster snapshot;
- failed erase restores prior durable and enabled state;
- boost behavior is not clipped or multiplied;
- record guards do not skip finish/results state machines;
- split-screen companions retain exactly one appropriate shadow;
- test bootstraps resolve the same donor path as genuine selections without
  persisting fake unlocks.

## Execution and acceptance matrix

The lists below define the complete merge/release campaign. The implementation
is intentionally structured so every remaining renderer, sanitizer, and
platform lane exercises the same identity and presentation policies rather
than a second character-specific path.

### A. ROM-free contracts

- exact v1/v2/v3 parse, v3 serialize, malformed/trailing-input rejection;
- unlock, enable, disable, erase, failure, retry, stale callback, and three-way
  concurrent unlock behavior;
- donor bounds and stable/live identity remapping for all three bonus racers;
- every 8/9/10-retail plus 1/2/3-virtual layout, including connected 13-slot
  navigation and safe-area scaling;
- Wizpig +4% gain/cap and boost non-stacking;
- Terry Pipsy stat-row selection, +3% gain/cap, boost non-stacking, stock
  attack behavior, and virtual-run record quarantine;
- invalid player/racer indices fail closed.

### B. ROM-backed automation

- earned Wizpig and Terry unlocks, loss/near-bit controls, import migration,
  process restart, code-list toggles, and Erase All Bonuses;
- physical select actors and P1–P4 placards with all three virtual slots live,
  including dance, hover/stand, and confirmation pose transitions;
- Wizpig and Terry car/hover/plane compositions with trace-confirmed Krunch
  authority and no boss vehicle dispatch;
- loop/flying-car transition stability, restart, pause-select-character, quit,
  finish camera, and finished-racer handoff;
- hit, squish, bubble, banana, shield, magnet, boost, shadow, and effect-anchor
  witnesses for kart, hover, and flight;
- one through four local players, duplicate-character cheat, mixed virtual and
  retail racers, and object-allocation failure;
- canonical Time Trial bytes and ghosts unchanged after both virtual racers;
- GL and WebGPU, browser WebGPU, PAL, 4:3/widescreen, resource plateau, ASan,
  UBSan, and optimized builds.

### C. Visual/human acceptance

- Wizpig is recognizably seated at retail-racer scale, with no Krunch body or
  rocket shell leaking into kart/hover mode;
- Wizpig's complete rocket is readable at normal plane-camera distance;
- Terry is recognizably perched in a kart/hover without filling the camera;
- Terry's complete silhouette, flap animation, banking, and single shadow are
  readable on every plane track;
- Terry flight has no continuous engine or plane wing contrails and each wing
  cycle has one level-matched authored flap cue; kart/hover retain engines;
- neither racer clips the camera or changes collision/recovery envelopes;
- Wizpig feels like a heavy racer with a subtle edge, while Terry feels like a
  nimble small racer with a similarly restrained edge;
- ordinary Krunch and Taj remain unchanged.

## Campaign close criteria

The work is releasable only after every deterministic unit/contract test is
green, the complete native suite has no reproducible failure, all six primary
vehicle captures meet the human acceptance above, canonical record controls
remain byte-identical, and no matching-build or retail serialization boundary
changed. Extracted ROM assets and generated captures remain build evidence and
are not committed.

## Worktree qualification evidence

The implementation pass completed these gates against the current worktree:

- all 112 native CTests, including the expanded roster, persistence, physics,
  layout, hash, audio, and object-lifecycle contracts;
- the original complete playable-Taj ROM journey, including car, hover, plane,
  multiplayer, persistence, audio identity, and Time Trial quarantine;
- the original rendered Taj picker across all four retail roster shapes;
- the new real-ROM 13-racer picker gate, with physical actor/sign composition
  and controller navigation plus all three pose states for Wizpig and Terry;
- the real-ROM Terry audio gate, with 24 consecutive cues at the authored
  48-tick cadence, 19 sampled zero-handle engine witnesses, a runtime Pipsy
  stat-row/+3% witness, exact wing-trail suppression, and a 36-sample
  kart-engine negative control;
- distinct real post-race 40x40 Wizpig and Terry cards, with runtime ownership
  traces and identity-palette pixel checks;
- real-ROM car, hover, and plane captures for both new racers, with traces
  confirming Krunch authority, successful presentation composition, and the
  intended per-mode scale; and
- `git diff --check` plus Python bytecode validation of every modified ROM gate.

The six captures were reviewed at ordinary chase-camera distance. Wizpig is
seated inside the kart and hovercraft envelope and uses the complete rocket in
flight. Terry uses the compact authored walk pose as a perched vehicle rider
and the complete authored fly cycle in plane mode. The 13-racer picker was
reviewed at 4:3 after its final scale and orientation tuning. These generated
ROM-derived images are temporary evidence and are not repository assets.

Before merge, run the broader platform lanes listed above (WebGPU/browser,
PAL, sanitizers, optimized builds, and the exhaustive track/vehicle and
multiplayer matrices) in the release environment. A failure in those lanes is
a release blocker, not permission to weaken the compatibility boundaries in
this document.
