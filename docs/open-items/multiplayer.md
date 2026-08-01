# Open items — Multiplayer

> Part of the split [open-items index](README.md). Every defect this port has
> hit is recorded with its mechanism, the measurement that found it, the fix, and
> the check that would catch a regression. **Nothing is deleted when it is
> fixed** — a closed entry is the only warning the next person gets that the same
> trap exists, and retractions are recorded in place rather than removed.


## P3.6 two-player split-screen — wave "splitscreen" (WORKS)

A two-player split-screen race on Ancient Lake now runs headlessly: two viewports,
two independently controllable racers, per-player HUD, 0 crashes across 10 runs on
each of the two backends. Asserted by `tests/check_race_2p_split.py` +
`tests/input_scripts/race_2p_split.txt`.

**Nothing in the game code was broken.** The port already supported split-screen
end to end — the viewport layout, the two cameras, the per-player HUD presets, the
per-player input dispatch and the AI driver all worked on the first attempt once a
second player could exist. The single blocker was in the **test harness**:

- **Root cause.** `--input-script` applied every entry to controller port 0
  (`script_apply(&s_pads[0], ...)` in `platform/platform_sdl_min.c`). A second
  player joins at PLAYER SELECT only by pressing A/START on a pad that is not yet
  in `gActivePlayersArray` — `charselect_new_player()` (menu.c) scans
  `gMenuButtons[0..3]`, and `gMenuButtons[i]` resolves to pad `i` because
  `input_assign_players()` sets `sPlayerID[i] = i`. So with port 0 as the only
  reachable pad, `gNumberOfActivePlayers` could **never** exceed 1, by
  construction. The game side was already fine: `osContGetReadData`
  (`platform/stubs_dkr.c`) has always filled all `MAXCONTROLLERS` pads from
  `platform_pad_buttons(i)`.
- **Fix.** An optional 1-based `P1..P4` port field on each input-script line
  (default `P1`, so all 13 existing fixtures parse unchanged), dispatched to
  `s_pads[e->port]`. A malformed port field now aborts the process instead of
  running a truncated route.
- **Positive control.** Reverting *only* `&pads[e->port]` → `&pads[0]` makes the
  check fail with `no [PACE2] rows — PLAYER_TWO never published a racer probe`,
  plus `no hud_init: hudPlayers=1 numViewports=2`; the run does not even reach a
  race, because without player 2 the char-select taps land on a different menu
  graph.

**The two-player menu graph is a genuinely different graph** — worth recording,
because copying a one-player route's tap timings cannot work:
`menu_character_select_loop()` branches on `confirmOffset >= gNumberOfActivePlayers`
with `confirmOffset == 1` from the title, so **1 player → CAUTION(28)/GAME_SELECT(19)**
but **2 players → straight to TRACK_SELECT(15)**. Track select then skips
`TRACKMENU_CHOOSE` (the Time-Trial toggle; ≥2 players force
`set_time_trial_enabled(FALSE)`) and adds the CPU-racer-count stage.

**Test-hook extensions**, all no-ops unless a two-player race exists:
- `MDKR_AUTOPILOT=1` needed no change — it already drives every racer with
  `playerIndex != PLAYER_COMPUTER`, so it covers player 2 too.
- The racer probe now publishes `PLAYER_TWO` as well as `PLAYER_ONE`
  (`game/src/racer.c`, inside the existing `NATIVE_PORT` block), emitted on a
  **separate** `[PACE2]` line so the player-1 `[PACE]` format stays
  byte-compatible with `check_race_drive.py` / `check_race_finish_time.py`.
  Confirmed behaviour-neutral: `check_race_drive.py` reproduces its documented
  reference exactly (4381 in-race frames, final cp=28 lap=1, max step 22.4 at
  frame 5696, slowest 240-frame mean 3.36), and `check_race_finish_time.py`
  reproduces course time 4777 / eeprom md5 `a144ba9f…`.
- `hud_init()` (`game/src/game_ui.c`) traces `hudPlayers=`/`numViewports=`, which
  is the layout evidence a check can assert on.

**The viewport sharp edge did not bite.** Split-screen sets two viewports per
frame and `mtx_ortho` asks for a 320-tall viewport on a 240-tall target, so the
P3.3 WebGPU containment clamp was the expected failure. It is fixed and stayed
fixed: WebGPU and GL produce the same image (per-half scores within a few percent,
identical gameplay numbers) and identical physics.

**2026-07-31 detector correction — a zip-pad boost is not a teleport.** The 2P
route now crosses a real pad: P2's per-frame displacement ramps 40.15 → 44.75 and
back down, with 4.0 as the largest frame-to-frame change in step length over the
full route. The fixture still carried an obsolete `MAX_STEP = 40` ceiling even though the
one-player gameplay gate had already established that authored boosts exceed it.
The 2P gate now uses the same shape policy: `MAX_ACCEL = 40` plus the generous
`MAX_STEP = 150` backstop. This continues to reject the historic 1296.8-unit
ASSET_MISC_8 discontinuity without rejecting normal boosted driving.

**Not a bug — the shared centre minimap.** In two-player the minimap straddles the
viewport divider: each player draws their own copy at the same absolute screen
position, clipped to their own half, so each sees half of it. That follows from
DKR's own code and is not a port artefact: `mtx_ortho` writes
`vtrans = (width*2, height*2)`, `vscale = (width*2, width*2)` — values that do
**not** depend on `gActiveCameraID`, so every player's ortho HUD lands in one
full-screen space and only the scissor differs. That is exactly why
`hud_init_element()` compensates per player in HUD element coordinates
(`+108` for texture elements, `+60`/`−48` for the rest) — and
`hud_minimap`'s `gMinimapScreenY = -gMinimapDotOffsetY / 2` has no such per-player
term, i.e. it deliberately centres it. Corroborated by the four-player case below,
where the same code puts one map at the **screen centre** straddling all four
quadrants, again with no per-player offset. **Not verified pixel-wise against the
real ROM** (see below).

**Historical spot check: four-player also worked.** The same port field reaches pads 3 and 4, and a
4-player route loads cleanly (`level_load: ... numPlayers=3`,
`hud_init: hudPlayers=3 numViewports=4`, rc=0) rendering four quadrant viewports
each with its own place indicator, banana count and item box. This is a *spot
check only* at this checkpoint — one route, one dumped frame, no assertion set
and no run matrix. The later Wave 3 resolution below supersedes that limitation.

### Wave 3 resolution — 3P/4P and multiplayer post-race are now measured

`tests/check_race_multiplayer.py` plus the `race_3p_split.txt` and
`race_4p_split.txt` fixtures close the two runtime gaps identified above:

- the existing probe now publishes P1–P4 without changing the historical P1/P2
  formats;
- every human must contribute 3500+ active rows, remain finite and bounded,
  avoid teleports/stalls, make substantial progress, and stay distinct from
  every other racer;
- all four quadrants are scored separately, with a dedicated 3P-minimap contract
  and automatic flat-quadrant rejection controls;
- spaced edge-triggered A taps advance both player counts through
  `MENU_RESULTS` and back to track select, proving teardown and the multiplayer
  results menu rather than stopping at `fin=1`.

Measured Debug/WebGPU transitions are results→track-select 7632→7931 for 3P and
7932→8231 for 4P. The complete check runs both arms from the real frontend and
is registered in `tools/run_checks.py` and the alignment-UBSan runtime matrix.

**2026-07-31 terminal-contract correction.** After the ROM-fidelity work moved
the AI lines, 4P P2 was still actively racing at `cp=33/lap=1` when the other
three finished. This is not a hang: `race_check_finish()` intentionally ends an
ordinary multiplayer race once N-1 racers are done, marks the sole remainder
`raceFinished=1/finishPosition=4`, and advances to results. The old fixture
overfit the earlier line by requiring all four to cross the finish. It now reads
the existing end-of-update `[ORACLE]` state for every human, requires unique
positions 1–4 and `raceFinished=1`, retains `cp>=40/lap>=2` for all normal
finishers, and permits exactly one last-place DNF only after `cp>=30/lap>=1`.

### Left open
- **No oracle comparison.** Every split-screen claim here is internal-consistency
  plus code reading; nothing was scored against ares. A two-player oracle route
  needs the ares harness to inject into controller port 2, which it cannot
  currently do per-port (`docs/ORACLE.md` records the opposite bug — input
  reaching all four ports at once joined four players). The shared centre minimap
  is the specific thing that comparison would settle.
- **Challenge/battle is outside this multiplayer fixture, but no longer open.**
  The closed 3P/4P fixture is an ordinary Ancient Lake race.
  `check_challenge_modes.py` now covers every authored egg/treasure/battle course,
  and `check_taj_challenges.py` covers Taj's distinct two-racer lifecycle.
- ~~**Two-player post-race remains separate.**~~ **RESOLVED (2026-07-29):**
  `check_race_2p_split.py` now runs 9,600 frames through the full race,
  requires `MENU_RESULTS` after the race and the return to
  `MENU_TRACK_SELECT`, mirroring the 3P/4P arms. The fixture's held-A drive
  input was the blocker — multiplayer post-race menu input is edge-triggered,
  exactly as `race_3p_split.txt` documents — and is now a tap plus spaced
  post-race retries. Motion assertions end at each racer's own finish (race
  clock stops), because DKR freezes finished racers for the fade/results
  sequence.
- **Real hardware, two physical pads.** Only scripted pads were exercised. The
  gamepad path already fills `s_pads[i]` per device, so it should work, but it was
  not tested.
- **`osContGetReadData` still reports ports 1–3 as absent.** ~~It sets
  `pad[i].errno = (i == 0) ? 0 : CONT_NO_RESPONSE_ERROR` and `osContInit` still
  returns `bitpattern = 0x01`~~ **RESOLVED (verified 2026-07-29):** the
  controller-status work landed the real platform presence query this note
  asked for. `controller_query()` and `osContGetReadData()`
  (`platform/stubs_dkr.c:658/705`) now derive `bitpattern`, `status`, and
  `errno` per port from `platform_pad_present()`
  (`platform/platform_sdl_min.c:1740`), so ports 1–4 report truthfully and the
  "trap for future work" no longer exists.
