# Campaign fixtures — the derivation chain

`tests/check_campaign_progression.py` gates the Adventure campaign one **seam**
at a time: a save state that enters the seam, and an assertion on what the game
writes leaving it. That only adds up to a witnessed campaign if each fixture
contains nothing the previous seam has not been shown to produce. This file is
that argument, field by field.

The fixtures are **built in code, not stored as binaries** — `derive_seam_a`,
`derive_seam_b` and `derive_seam_e` in the check. A committed `.bin` would be a
number nobody can audit; a derivation function that names the seam each field
comes from can be read against this document and against the game source. The
one committed artefact is
[`tests/input_scripts/wizpig_to_credits.txt`](../input_scripts/wizpig_to_credits.txt),
which is a controller script, not save state.

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
production wrote four times. Seam C runs on the end of that chain, seam E on the
end of seam C. Each carried step also asserts that no bit the incoming save held
was dropped, so “carried” is checked, not assumed.

The only constructed inputs left are fixture A, the seam B negative control
(deliberately not a legitimate state), and the four premise fields listed under
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

## Fixture E — seam C’s save, plus what Wizpig 2 additionally wants

Entered by seam E. The campaign half is carried out of seam C untouched: the four
rematch bits and `wizpigAmulet == 4` from seam B’s wins, Wizpig 1’s cleared
course and `bosses` bit 0 from seam C’s.

Four fields are **overridden**, and they are the only things in the whole chain
this check asserts without having watched them. They are Wizpig 2’s other gate
(`game/src/game.c:755`, and the T.T. door at
`game/src/object_functions.c:4116`): `trophies == 0xFF`, `ttAmulet == 4` with the
four world keys, Future Fun Land’s four races cleared, and at least 47 total
balloons. See the residual list below.

## Residual manual acceptance

These are the parts of the campaign this check does not witness. Each is a
specific headless obstacle, not a judgement call, and each is a step a manual
acceptance pass still has to perform.

**1. The lobby’s boss-rematch door, driven.** Seam B enters the rematch by
`MDKR_LOAD_TRACK` retarget, the same way `check_bluey2_rematch.py` does. The
route cannot instead drive through the lobby door: measured in the Dino Domain
lobby with all door bits open, the kart stalls at (-1295, 685), 1,240 units short
of the boss exit at (-777, 1812), and the drive hook’s reverse-and-retry
oscillates there indefinitely. The exit is reachable from the spawn the player
returns to *after a race*, which is why `check_first_boss_progression`’s route
reaches the first boss only as `12:E7:E38`. What is unwitnessed is therefore the
**approach**, not the gate: the gate itself is `balloonsPtr[worldId] == 8`, and
seam A proves silver clears are what produce that number.

**2. The T.T. amulet and the trophy championships.** `ttAmulet` is written by the
four T.T. challenge levels (`game/src/objects.c:9256-9268`) and `trophies` by the
trophy-race rankings screen. The trophy side already has a gate —
[`check_trophy_series.py`](../check_trophy_series.py) drives all four
championships and their EEPROM persistence — but neither is chained into this
file, so fixture E states both as premises. The Future Fun Land unlock they feed
(`trophies & 0xFF == 0xFF` plus Wizpig 1, `game/src/thread3_main.c:1895-1899`) is
unwitnessed for the same reason.

**3. The credits screen from a won Wizpig 2.** Seam E proves the campaign sets
and persists `bosses & 0x20`, which is the single value `menu_credits_init`
(`game/src/menu.c:15188-15201`) reads to choose “TO BE CONTINUED …” and
`SEQUENCE_CRESCENT_ISLAND` over “THE END?”. It does not reach the screen. After
the win the game pushes a stack of cutscene levels and pops them on either an A
press with `func_8006C300()` non-zero or the scene ending itself
(`game/src/thread3_main.c:894-919`); measured, the run reaches
`ASSET_LEVEL_WIZPIG2ANIM` (level 62) and stays there for 25,000 further frames
with A tapped every 200 frames, so the animation does not run itself out
headlessly. `tests/input_scripts/wizpig_to_credits.txt` is the script that
supplies those taps and is kept for the manual pass and for whoever closes this.

The contrast arm is still meaningful: the WHODIDTHIS route reaches
`MENU_CREDITS` from a save that was never started, which is why reaching credits
is not by itself evidence about the campaign — and is why seam E asserts on the
bit rather than on the screen.
