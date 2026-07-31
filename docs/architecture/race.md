# Getting into a controllable race

> **This work has landed.** Written ahead of the M6 wave, kept for the
> deterministic menu route and the verified `MENU_ID` mapping, which are still
> accurate and still used by the fixtures. Read the goal and validation
> sections as a record of what was achieved.

The goal was to drive the menus into an actual race with a human-controlled
racer that responds to input (accelerates, steers), rendering the track — the
"playable proof of work". It is done, and the route below is the one the
navigation fixtures still take.

## What already works (leverage it)
- The title screen's 3D background IS a live attract-mode race demo (Ancient Lake,
  AI-driven) — so the race scene renderer, track geometry, racer models, and camera
  substantially work already, WITHOUT input. This de-risks M6 massively: the engine
  runs a full race; we mainly need to enter one under human control and fix the
  remaining LP64/asset bugs on the human-race code path.
- `level_load` (game/src/game.c:354) is the real in-game loader; it already has the
  NATIVE_PORT asset_swap_normalize(ASSET_LEVEL_HEADERS) wired (M4).

## Menu routes to gameplay (pick the simplest deterministic one)
From MENU_CHARACTER_SELECT (reached: tests/input_scripts/nav_to_character_select.txt →
menuId=3):
1. **Time Trial / Tracks mode** (menu.c:~7480, `gIsInTracksMode = TRUE`): char select →
   init_racer_headers() → MENU_TRACK_SELECT → pick a track → single-racer race. SIMPLEST
   isolated race, no adventure state. RECOMMENDED first target.
2. **Adventure**: char select → MENU_FILE_SELECT → adventure → CENTRALAREAHUB (Timber's
   Island) drivable hub. More game state (save file), but a rich "drive around" proof.

Map the exact button sequence for route 1 by reading the MENU_CHARACTER_SELECT and
MENU_TRACK_SELECT handlers (menu.c) and scripting it via --input-script (remember the
two-edge / edge-detect input rules and input-ignore windows learned in M4 — a held
button is one edge; taps need release between them). Extend tests/input_scripts/ with
`nav_to_time_trial_race.txt` and assert via MDKR_TRACE=1 menu_init that you reach
TRACK_SELECT then the in-race state.

## Expected work (fix at true cause, NATIVE_PORT-gated)
- **More ROM-overlaid-struct / LP64 bugs on the race path**: racer objects, track
  collision (LevelModelSegment collisionFacets/Planes already dkrptr32'd — verify),
  the object map (gObjectMap), waypoints/paths, HUD. Same classes already fixed in
  M2–M4; apply the dkrptr32 + dkr_lo32_to_ptr + asset_swap patterns. Use the
  SIGSEGV backtrace handler (main_pc.c) + MDKR_TRACE.
- **Racer control**: confirm input drives the player racer (racer.c — throttle/steer
  read from the pad via the same osContGetReadData path M4 wired). The human racer is
  player 0; AI fills the rest.
- **Camera**: the race camera (camera.c) — the demo uses it already; verify human-race
  camera follows the player.
- **Collision**: generate_collision_candidates / collision.c (has NON_MATCHING C
  paths) — needed so the racer doesn't fall through the track.
- **Countdown/HUD**: game_ui.c HUD elements (needs fonts from M3c).

## Validation (the "validated win") — achieved
- Scripted: `--input-script nav_to_time_trial_race.txt` reaches the in-race state
  (MDKR_TRACE shows level_load of a real track + race start), dumps frames showing the
  track from the racer's camera. Then a follow-on script pressing ACCEL (A) + steer
  and dumped frames showing the racer MOVING through the world (compare position/scene
  across frames — the camera/track scrolls).
- Interactive: `./build/mdkr64`, human plays: pick Time Trial, pick a track, drive.
- `--headless-frames 300` (title) still exits 0. No new warnings. Record a short
  clip/frame-sequence as the proof-of-work artifact.

## Verified MENU_ID enum → trace-id mapping (game/src/menu.h)
Confirmed against live MDKR_TRACE=1 menu_init output:
`0=TITLE, 1=LOGOS, 3=CHARACTER_SELECT, 5=TRACK_SELECT_ADVENTURE, 6=FILE_SELECT,
10=MAGIC_CODES, 12=OPTIONS, 15=TRACK_SELECT, 19=GAME_SELECT, 20=TROPHY_RACE_ROUND,
26=BOOT`. So the Time-Trial target state is menu_init(menuId=15) then a real
level_load. Char-select confirm branches (menu.c:~7480): tracks path → menuId=15,
adventure path → menuId=6. Use these ids as scripted-nav assertions.

## Notes
- Keep it deterministic and reproducible (scripts committed as fixtures).
- If audio (M5) isn't done, race runs silent — fine for the playable-render proof.
- Watch for race-only asset types not yet swap-covered (check asset_swap coverage vs
  what a track load touches: waypoints, weather, particles-in-level).
