# Campaign fixtures — the derivation chain

`tests/check_campaign_progression.py` gates the Adventure campaign one **seam**
at a time: a save state that enters the seam, and an assertion on what the game
writes leaving it. That only adds up to a witnessed campaign if each fixture
contains nothing the previous seam has not been shown to produce. This file is
that argument, field by field.

The fixtures are **built in code, not stored as binaries** — `derive_seam_a`,
`derive_seam_b`, `derive_all_worlds_silver` and `derive_seam_e` in the check. A
committed `.bin` would be a number nobody can audit; a derivation function that
names the seam each field comes from can be read against this document and
against the game source. The committed artefacts are controller scripts, not
save state:
[`wizpig_to_credits.txt`](../input_scripts/wizpig_to_credits.txt),
[`lobby_rematch_door.txt`](../input_scripts/lobby_rematch_door.txt) and
[`tt_challenge_win.txt`](../input_scripts/tt_challenge_win.txt).

Bit offsets are not restated here; they are constants at the top of the check,
taken from the write order in `game/src/save_data.c` (`func_800732E8`, and the
mirrored read at `:394`).

## Where the topology comes from

Nothing below hardcodes which courses belong to which world. `world_topology()`
reads each level header's own `world` and `race_type` bytes out of the ROM and
groups the save-eligible courses from that. This is deliberate: the retail level
**names do not track the world enum** — Whale Bay, Pirate Lagoon, Crescent
Island and Treasure Caves are world 2, not the "Snowflake Mountain" their
neighbours in the enum suggest. A fixture that marks the wrong four courses
silver-cleared still loads, still passes a checksum, and silently exercises
nothing. Reading the header removes that failure mode.

(For the same reason, note that `check_bluey2_rematch.py`'s `SNOWFLAKE_RACES =
(8, 4, 10, 30)` are world 2 courses while Bluey 2 is world 3. That check reaches
its boss by `MDKR_LOAD_TRACK` retarget and never evaluates the lobby's silver
gate, so the mislabelled four are inert there — but they are not a pattern to
copy, and this check does not.)

## Fixture A — “just beat the world’s first boss”

Entered by seam A. Every field is something
[`check_first_boss_progression.py`](../check_first_boss_progression.py) asserts
on the EEPROM its win arm leaves behind:

| Field | Value | Proved by |
| --- | --- | --- |
| four world courses | status 2 (`RACE_CLEARED`) | its `FIRST_THREE` assertions plus `HOT_TOP_VOLCANO` cleared |
| world’s first boss course | status 2 | `TRICKY_ONE` cleared on the win arm |
| `bosses` | `1 << world` | `expected_bosses = DINO_WORLD_BIT` |
| `balloons` | `(6, 4, 0, 0, 0, 0)` | `balloon counts ... want (6,4,0,0,0,0)` |
| world 0 flags | the two hub balloon bits | `checkpoint hub balloon flags were lost` |
| `cutsceneFlags` | boss-door scene | `Dino boss-door cutscene flag was not persisted` |
| `ttAmulet`, `wizpigAmulet` | 0 | `first boss incorrectly awarded an amulet` |
| everything else | 0 | not written by that run |

The eight-balloon sibling of the boss-door cutscene bit is also set, so the
lobby does not replay a scene at four balloons that the player has already seen.

This is exactly the moment silver coins unlock: `gIsSilverCoinRace`
(`game/src/objects.c:2103`) needs the course already `RACE_CLEARED` **and**
`1 << worldId` present in `settings->bosses`.

## The chain is carried, not rebuilt

Fixtures A and B below are constructed. Everything after the first rematch is
**carried**: `Slot.from_save()` reads a real persisted EEPROM back into the same
shape, and the next seam runs on it with only the fields a later gate needs
overridden, each override named at its call site.

So the four seam B arms are not four independent runs from four synthetic saves —
each is fought on the EEPROM the previous one persisted. `wizpigAmulet` climbing
1 → 2 → 3 → 4 (and `bosses` 0x9e → 0x19e → 0x39e → 0x79e) is therefore something
production wrote four times. Seam C runs on the end of that chain, seam T (four
trophy championships) on the end of seam C, seam T.T. (four challenge wins) on
the end of seam T, and seam E on the end of that. Each carried step also asserts
that no bit the incoming save held was dropped, so “carried” is checked, not
assumed — and seams T and T.T. additionally assert that they changed *nothing*
outside their own field, so a trophy race cannot quietly move the campaign on.

The only constructed inputs left are fixture A, the two negative controls
(deliberately not legitimate states), and Future Fun Land's own state under
fixture E.

## Fixture B — “that world is silver-coin complete”

Entered by seam B. It is fixture A with seam A applied four times. Seam A proves
a silver clear does exactly three things, and fixture B applies exactly those:

- the course’s 2-bit status moves 2 → 3
  (`set_course_finish_flags`, `game/src/objects.c:9652-9654`);
- `balloons[world]` and `balloons[0]` each gain one
  (`game/src/objects.c:9631-9634`, taken only when that function returned true).

So four silver clears give status 3 on all four courses and balloons
(6+4, 4+4) = **(10, 8)**. Eight world balloons is the number `obj_loop_exit`
(`game/src/object_functions.c:2718-2724`) tests to disable the lobby’s
`WARP_BOSS_FIRST` warp and enable `WARP_BOSS_REMATCH` — the silver coins are
what turn the boss door into a rematch door.

The world’s lobby flags are set to “all doors opened”: those bits are set by the
lobby’s own doors the first time the player has the balloons to pass them, and a
player who has raced all four courses eight times has passed all four.

**The negative control uses a state that is not legitimate, on purpose.**
`first_boss_beaten=False` removes the first-boss bit and the boss course’s
cleared status while keeping the identical race and the identical forced win.
Production then reads the race as a first encounter and awards the first-boss
bit and no amulet. Without that arm, “the rematch awarded an amulet” would be
consistent with “any boss win awards an amulet”.

## Fixture C — the end of the seam B chain, unmodified

Entered by seam C. No overrides at all: it is the EEPROM the fourth rematch
persisted. Four rematch wins give the four `1 << (world + 6)` bits and
`wizpigAmulet == 4`, which is the whole of Wizpig 1’s gate
(`game/src/game.c:642-643`). The three-piece negative control is the same chain
one link earlier, which is why it needs no construction either.

### Why this seam was first reported as not firing

`game_load_level`’s `level_load:` trace (`game/src/game.c:500`) prints the level
that was **asked** for. The Wizpig-face branch 140 lines later pushes the hub
onto the level-properties stack, rewrites `levelId` to
`ASSET_MISC_68[4]` (= 42, `WIZPIGMOUTHSEQUENCE`), plays it, and pops the hub back.
Both loads therefore print `levelId=0`, and a redirected hub load is
byte-identical in the log to an ordinary one — the first pass over this read the
two hub loads as “the cutscene did not fire” when they are in fact its signature.

The save disagreed all along: `CUTSCENE_WIZPIG_FACE` (0x2000) has exactly one
writer, `game/src/game.c:648`, inside that branch, and it was set. The
`wizpigface:` trace was added so the redirect is stated where it happens rather
than inferred, and seam C asserts both it and the persisted flag.

## Fixture C.5 — the trophy and T.T.-amulet chain

Entered by seams T and T.T., and neither is a fixture in the constructed sense:
both run on the EEPROM the previous seam persisted.

**Seam T** imports `check_trophy_series.py`'s own championship driver
(`drive_gold_championship`, which is that file's `run_case` with the winning
finish order and a caller-supplied save) and runs it four times, world 1 → 4,
each on the last one's output. `trophies` therefore climbs
0x3 → 0xf → 0x3f → 0xff, two bits per world, and each step is checked against
production's own `trophyaward:` line — the world, rank 0, 36 points (four
first places at 9 a round, the same scale that gives that check's 32 for a
1st/2nd/1st/2nd tie), the incoming value it started from and the value it wrote.
`check_trophy_series.py`'s own gate is unchanged by the extraction; its `main` is
now a thin caller of `run_trophy_series`.

Two things the chained form needs that the standalone gate does not, both
measured rather than assumed:

- **The cabinet is entered by that file's own controlled collision**
  (`MDKR_TROPHY_COLLIDE`, its `retry` mode), not by bumping it. Driving up to the
  cabinet from a *carried* save is a knife edge: the lobby corridor to it runs
  past the Hot Top Volcano door, whose capture test
  `0.383x + 0.924z + 1094.6 < 0` the kart passes within a unit of. Measured, it
  reaches (-298, -1062) — where that expression is **-0.8** — and is warped into
  Hot Top Volcano instead. The kart still has to reach the lobby, and the cabinet
  still evaluates its own gate (eight world balloons **and** that world's rematch
  bit) before it opens; driving the last few hundred units to it is what
  `check_trophy_series.py`'s own arms already gate.
- **Two hub-side fields are pinned back between championships.** A championship
  drives Timber's Island on the way in, and the hub writes `taj`
  (`TAJ_CAR_OFFERED`) and its own course flags (measured 0x4400 → 0x67b3) doing
  so. Carried, the next championship's hub drive stalls before the lobby and
  awards nothing — measured, worlds 2, 3 and 4 all left `trophies` at world 1's
  0x3. `carry_trophy_save` restores exactly those two and touches nothing else,
  and each championship's `trophyaward:` line is checked to have started from
  the carried `trophies`, so the chain is verified rather than assumed.

**Seam T.T.** drives the four challenge courses — 11 Fire Mountain (eggs),
25 Smokey Castle (bananas), 26 Darkwater Beach and 27 Icicle Pyramid (battle) —
in four separate processes on one another's saves, and `ttAmulet` climbs
1 → 2 → 3 → 4. The award is `game/src/objects.c:9358-9367`, taken only when the
human wins and the course is **not already** `RACE_CLEARED`.

**The negative control is the second half of that sentence**, and it is run at
*one* piece rather than at four on purpose: `ttAmulet` is clamped
(`if (i > 4) i = 4`), so a replay at four would leave the counter unchanged even
if the gate were gone. Replaying the FIRST challenge on the save that challenge
itself produced must therefore award nothing, play no
`ASSET_LEVEL_TTAMULETSEQUENCE`, and leave `ttAmulet` at 1.

## Fixture E — the chain’s save, plus Future Fun Land

Entered by seam E. Everything Wizpig 2 checks about the *campaign* is carried:
the four rematch bits and `wizpigAmulet == 4` from seam B, Wizpig 1's cleared
course and `bosses` bit 0 from seam C, `trophies == 0xFF` from seam T and
`ttAmulet == 4` from seam T.T. The last two were premises until S9; seam E now
asserts they arrived rather than writing them.

What is still constructed is **Future Fun Land itself**, the world Wizpig 2 lives
in, because this check does not drive there. Five fields:

| Field | Value | Why it is not derived |
| --- | --- | --- |
| Future Fun Land’s four races | status 2 | reaching Wizpig 2 means having raced them |
| `balloons[5]` | 4 | one per FFL race |
| `balloons[0]` | ≥ 47 | the number the T.T. door counts (`game/src/object_functions.c:4159`) |
| `keys` | 0xF | the four world keys |
| `cutsceneFlags` | `CUTSCENE_DINO_DOMAIN_BOSS << 4` = 0x80 | the “arrived in Future Fun Land” scene |

The lighthouse unlock that opens that world is not a premise: it is gated by
[`check_future_fun_land.py`](../check_future_fun_land.py), on trophies four
production championships wrote.

### The arrival flag is load-bearing, and that was measured

The last row is not cosmetic and is the reason residual 3 below is closed. Every
Future Fun Land level header is `RACETYPE_HUBWORLD`, so `game_load_level`’s
world-arrival branch (`game/src/game.c:762-802`) runs on the **post-race cutscene
levels** too. With the flag clear it takes its `cutsceneId = CUTSCENE_ID_UNK_5`
arm, which overwrites the cutscene channel the level-properties stack pushed.
`func_8001E4C4()` then deactivates every animation object whose channel is not 5
and `func_8001E93C()` builds an empty node list, so the scene has no animation
left to run.

Measured on `ASSET_LEVEL_WIZPIG2ANIM`: `gCutsceneID` 5 against a pushed channel
of 1, all **159** of the level’s animation objects moved below
`gObjectListStart`, the node list `D_8011AE78` at **0**, and the animation update
never entered again for the rest of the run. With the flag set, the same route
reaches `MENU_CREDITS` at frame ~12,050.

A player cannot reach Wizpig 2 without having arrived in Future Fun Land once, so
carrying the flag is what makes the fixture *legitimate*. A fixture without it is
a state the campaign cannot produce, and the stall it caused was the fixture’s,
not the game’s.

## Residual manual acceptance

The three residuals this section used to list — the lobby rematch door driven,
the trophy/T.T.-amulet chain, and the credits screen from a won Wizpig 2 — are
**closed**. What each one turned out to be is kept below, because each is the
reason a route or a fixture is shaped the way it is, and a route whose reason has
been deleted is a route the next person will “simplify”.

### 1. The lobby’s boss-rematch door — closed, driven

Seam B still enters three of the four rematches by `MDKR_LOAD_TRACK` retarget,
which is the fast path. It additionally enters the Dino Domain rematch by
**driving the human through the lobby’s own boss door**, and that arm asserts the
identical post-seam state.

The recorded obstacle reproduces exactly at this commit. Driving straight at the
exit (`MDKR_DRIVE_ROUTE="…;12:E46"`) the kart leaves the lobby spawn at
(-919, -343), heads north, is stopped at about (-911, 482) and then slides
north-west along the wall to **(-1295, 685)** — 1,240 units short of the boss
exit at (-777, 1812) — where the drive hook’s reverse-and-retry oscillates for
the rest of the run. So the geometry, not the gate, was the problem: a wall runs
WNW across the lobby’s northern approach and the direct line runs into it.

The fix is **one waypoint**, found by bisecting the space between the stall and
the exit:

| Intermediate waypoint | Result |
| --- | --- |
| none (straight at the exit) | stalls at (-1295, 685), 1,240 units short |
| (-600, 1300) | stalls, 689 units short — still west of the wall |
| (-300, 700) | reached at frame ~3021; the exit taken at ~3124 |
| (200, 900) | reached — the opening is east of the blockage |

`(-300, 700)` is east of the wall’s end; from there the run to the door is clear,
passing (-377, 1308) on the way. The kart’s last sampled position before the exit
latches is 28 units from it. Total cost from the frontend: the rematch race is
entered at frame ~3124, the amulet sequence plays at ~7061 and the lobby is back
at ~7441, so the driven arm is **not** materially slower than the retarget arm.

The approach is watched by [`tests/route_plan.py`](../route_plan.py) rather than
merely timed out: if it ever stalls again the failure names the waypoint, the
closest approach and which of oscillation / held off / budget it was, at the
frame it happened. That matters here specifically because the original symptom
was an indefinite loop that reported nothing.

The gate itself was never in doubt and is now asserted directly: the driven arm
requires the lobby’s `bosswarp:` verdict to show `WARP_BOSS_REMATCH` live at
eight world balloons and `WARP_BOSS_FIRST` disabled, so the door the route drove
through is the rematch door and not its first-encounter twin 35 units away.

The note this section used to carry — that the exit is reachable from the
post-race spawn, which is why `check_first_boss_progression` reaches the first
boss as `12:E7:E38` — is still true, but it is not the cheapest route. Entering a
race and coming back costs the Adventure track-select screen
(`MENU_TRACK_SELECT_ADVENTURE`, which waits for an A press: measured, 10,150
frames of a run spent on it) and the post-race options menu. The waypoint avoids
both.

### 2. The T.T. amulet, the trophies and Future Fun Land — closed, chained

Both are now produced state. See “Fixture C.5” above for seams T and T.T. and
their negative control, and
[`check_future_fun_land.py`](../check_future_fun_land.py) for the lighthouse
unlock (`trophies & 0xFF == 0xFF` plus `bosses & 1`,
`game/src/thread3_main.c:1925-1938`), which is gated on trophies four production
championships wrote, with a “one silver instead of a gold” arm and a “no Wizpig
1” arm.

Two things that check needs and that are worth not re-deriving:

- The unlock has exactly one trigger, the rocket signpost at
  **(3892, -149, 2226)** on Timber’s Island, reached by extending
  `check_adventure_hub.py`’s tour north-east.
- A refusal writes **nothing** — no bit, no level load, no trace — so “the route
  never got there” and “the signpost said no” are indistinguishable without the
  `rocketsign: trigger` line, which is emitted before the gate is evaluated. Both
  negative arms assert the trigger fired.

Entering a world’s trophy cabinet needs that world’s **rematch** bit as well as
its eight balloons (`obj_loop_trophycab`: `(1 << (worldId + 6)) & bosses`), which
is why the trophy chain runs on a save that already holds them. A checkpoint with
only the first-boss bits produced `trophies=0x0` through three championships.

### 3. The credits screen from a won Wizpig 2 — closed, reached

Seam E now drives the post-win cutscene stack all the way to `MENU_CREDITS` and
asserts the branch `menu_credits_init` took: `"TO BE CONTINUED …"`,
`SEQUENCE_CRESCENT_ISLAND`, and `gViewingCreditsFromCheat` zero. The WHODIDTHIS
contrast arm still reaches the same screen from a save that was never started and
must still produce `"THE END"`, which is what keeps “reached credits” from being
evidence about the campaign on its own.

**The recorded obstacle was the wrong one, and the measurement that says so is
worth keeping.** The pop at `game/src/thread3_main.c:894-919` has two arms: an A
press with `func_8006C300()` non-zero, or the scene ending. Instrumented over
29,929 frames of a won Wizpig 2 run:

- `func_8006C300()` was **zero on every single frame**, and zero on all 24,596
  frames after `ASSET_LEVEL_WIZPIG2ANIM` loaded. It is not merely unlucky
  timing: `game_load_level` zeroes `D_800DD330` on *every* load and the only
  place that sets it is the redirect branch for a **repeat** Wizpig boss entry,
  so during any post-race cutscene the A-press arm is structurally unreachable.
  The 113 A presses the script delivered could never have popped anything, and
  the tapping cadence was never the variable.
- `func_800214C4()` — the scene-ending signal — fired exactly once in the whole
  run, popping the Wizpig 2 boss *intro*, and never again.

The real cause is the fixture, and it is written up under “Fixture E” above: the
missing Future Fun Land arrival flag made `game_load_level` overwrite the
cutscene channel, which deactivated all 159 of the animation objects that would
have raised that signal. `tests/input_scripts/wizpig_to_credits.txt` is still the
script seam E uses; its taps are now documented as harmless rather than
load-bearing.

## What is still not witnessed

- **Future Fun Land’s own state.** Fixture E constructs the five fields in the
  table above. The campaign check does not drive to Future Fun Land, race its
  four courses or collect its balloons; `check_future_fun_land.py` proves the
  door into it opens, not what is behind it.
- **One continuous start-to-credits run.** Deliberate, and unchanged: it would
  run for hours and fail as one opaque blob. The chain of witnessed seams is the
  design.
