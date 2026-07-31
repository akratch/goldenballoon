# Phase 3 — Complete the play loop

> **Historical phase plan.** Statements below that 3P, challenge/battle,
> first-boss, Taj, or trophy paths were untouched describe their original
> checkpoints. The defined Waves 1–3 are now 23/23 complete, and the manifest
> has since grown to 69 scripts / 76 tasks. Current status lives in
> [`DEVELOPER_HANDBOOK.md`](DEVELOPER_HANDBOOK.md) §1/§6.
>
> **For any defect named below, [`open-items/`](open-items/README.md) is
> canonical** — it carries the mechanism, the measurement, the fix and the
> check, and it is maintained. This document is kept for the *scope* decisions:
> what the phase set out to close, in what order, and why.

**End state.** A player starts Adventure or Time Trial, drives a full race,
finishes it, sees the result, and their time/progress is saved — with the menu
screens matching the real ROM. That is the bar: not "the engine runs" but "someone
can sit down and play a race to completion and their record persists".

Status entering this phase: boot → every menu → a full lap of Ancient Lake with
HUD and audio all work; saves round-trip correctly; the game can drive itself
(`MDKR_AUTOPILOT=1`) at a consistent ~1650-clock lap. Menu screens score 92–95 %
against the real ROM (two at 79–84 %, one at 61 %).

---

## P3.1 — Finishing a race must record a time  **[DONE — wave "finishtime"]**

A 3-lap Time Trial now finishes and its course/lap time is written to EEPROM and
read back after a restart. Verified by `tests/check_race_finish_time.py`
(course time 4709, `save/eeprom.bin` md5 the recorded md5), 3× from a deleted save,
with positive controls for both fixes. Full write-up in `docs/OPEN_ITEMS.md`
("P3.1 RESOLVED").

**The finish path was never broken.** Three things were, and only one was a bug:

1. **The route.** DKR records times only inside `race_finish_time_trial()`'s
   `if (gIsTimeTrial)` block, and every existing fixture confirms **TIME TRIAL =
   OFF** — so the write was unreachable by construction. New fixture
   `race_full_3lap_tt.txt` adds the missing stick-DOWN at track select, plus the
   repeated A taps the edge-detected post-race screens need.
2. **`MDKR_AUTOPILOT` could not drive a solo Time Trial.** DKR's AI throttle
   routine returns early when the field has no CPU racers, so the kart got
   steering but no throttle and the AI's own stuck-recovery drove it *backwards*.
   Fixed in the test hook using the game's own idiom.
3. **A genuine LP64 bug:** `timetrial_ghost_read` declares three `f32 [3]` arrays
   but writes and reads four Catmull-Rom control points, so finishing a Time Trial
   and then racing again (post-race "TRY AGAIN", with a player ghost now saved)
   aborted in `__stack_chk_fail`. Sized to 4 under `NATIVE_PORT`.

**`raceFinished` always fired** — the earlier probe simply lived inside
`update_player_racer`, which stops for a finished racer. The `[PACE]` trace now
carries `rlap=`/`fin=` fed from `race_check_finish()`; assert on those.

**Note:** `MENU_RESULTS` is the *trophy-race* results screen
(`thread3_main.c:689`); a Time Trial finish is not expected to reach it. Do not
treat its absence as the bug.

**Left open:** the stored initials compress to `0x0000` on the A-tap route
(cosmetic, needs an oracle run to confirm against the real ROM); only Ancient
Lake / car is exercised; a *trophy* race finish takes the
`race_finish_adventure` → `MENU_RESULTS` branch and is still unvalidated.

## P3.2 — TRACK_SELECT preview window  **[DONE — wave "texalias"]**

**61.0 % → 93.7 %** (block 52.4 → 96.3). Bonus: `game_select` 87.6 → 94.0 %,
`caution` 92.0 → 95.1 %; `char_select` byte-identical (a clean control).

**It was never the camera.** The root cause was systemic: **the HLE texture cache
aliased freed arena memory.** `dkr_bind_tile()` keys entries on the source address
(+ fmt/siz/size/palette) and nothing invalidated them when the game *freed* that
memory, so once mempool handed the same arena bytes to a different asset the next
lookup **hit** and bound the previous asset's texture. Silent by construction — no
crash, no missing draw, correct geometry, wrong image. TRACK SELECT merely made it
visible, because `func_8008F618()` draws the background as 30-px bands alternating
a world's two textures, so every other Dino Domain band came out Snowflake
Mountain blue.

Fixed with `gfx_dkr_texcache_invalidate_range()` called from
`mempool_slot_clear()` — the single point where arena bytes become reusable —
before coalescing rewrites `slot->size`. `MDKR_TEXCACHE_VERIFY=1` keeps a
permanent content check. Cost ~25 µs/frame (< 0.2 % of a frame).

Verified: 0 stale hits across all 9 menu fixtures + `race_drive_time_trial`;
positive control (disable the one call) fires 20 stale hits from frame 1490.

**Retracted:** the preview is *not* static and this was *not* camera placement.
Both runners run the same animated flythrough. The earlier "cameras are parked,
score flat at ~60 % across 260 frames" reading was an artifact — with half the
screen structurally wrong, no phase could score better than ~60 %. The
`objectIdToSpawn` / `BHV_CAMERA_ANIMATION` lead was a red herring.

## P3.3 — MAGIC_CODES and FILE_SELECT  **[DONE — wave "p33-text"]**

Both were **renderer** defects, not menu layout (`game/src/menu.c` untouched).

1. **Every dialogue-box background was invisible.** A `FILL_RECTANGLE` outside
   `G_CYC_FILL`/`COPY` rasterizes through the combiner, where with no texel and no
   shade only the `d` term matters (`(a−b)*c+d` degenerates to `d`).
   `render_dialogue_box` relies on exactly that (`G_CC_ENVIRONMENT` +
   `gDPSetEnvColor`), but the HLE read `prim_color` unconditionally. Traced:
   `rgb_d=5` (ENVIRONMENT), `env=00000080`, `prim=00000000` — fully transparent.
2. **WebGPU viewport clamping squashed all ortho geometry 0.75×.** `mtx_ortho`
   deliberately requests a 320-tall viewport on a 240-tall target; GL/Metal keep
   the transform and clip, but WebGPU validates containment so the half-height was
   trimmed 160 → 120. Screen-space TEXRECT text was unaffected, which is why it
   presented as a text-vs-panel mismatch.

Final numbers with all Phase 3 fixes merged (default WebGPU backend):
`MAGIC_CODES` **94.8 %** (block 98.1), `FILE_SELECT` **94.6 %** (block 96.8),
`OPTIONS` 94.4 %, `SAVE_OPTIONS` 94.9 %, `AUDIO_OPTIONS` 94.0 %,
`CHARACTER_SELECT` 95.0 %, `game_select` 94.0 %, `track_select` 93.7 %.

Fix 2 also removed the black bars that capped `track_select` at 70.4 % on WebGPU,
which P3.2 had flagged as out of scope — the two waves resolved each other.

## P3.4 — Oracle per-route calibration, and push the static screens over target

Per-route `ares_extra` calibration (the field exists; calibrate it from measured
screen arrivals via `tools/oracle_screens.py`), then close the remaining gap on
the screens already at 92–95 % to the stated target of hist > 0.95 / block > 0.98.

## P3.6 — Two-player split-screen  **[DONE — wave "splitscreen"]**

A two-player split-screen race on Ancient Lake runs headlessly: two viewports,
two independently controllable racers, per-player HUD, **0 crashes across 10 runs
on each backend** (default WebGPU and `MDKR_RENDERER=gl`), 3039 frames with both
players traced and driving. New fixture `tests/input_scripts/race_2p_split.txt`
and check `tests/check_race_2p_split.py`. Full write-up in `docs/OPEN_ITEMS.md`
("P3.6 two-player split-screen").

**Nothing in the game code was broken.** The port already supported split-screen
end to end — viewport layout, two cameras, per-player HUD presets, per-player
input dispatch, AI driver. Exactly one thing blocked it, and it was in the test
harness:

- **`--input-script` could only reach controller port 0.** A second player joins
  at PLAYER SELECT only by pressing A on a pad not already in
  `gActivePlayersArray` (`charselect_new_player()`), so `gNumberOfActivePlayers`
  could never exceed 1 *by construction*. Fixed with an optional `P1..P4` port
  field per script line (default `P1`; all 13 existing fixtures parse unchanged).
  Positive control: revert `&pads[e->port]` → `&pads[0]` and the check fails with
  `no [PACE2] rows — PLAYER_TWO never published a racer probe`.

**The two-player menu graph is a different graph**, as anticipated:
`confirmOffset >= gNumberOfActivePlayers` sends 1 player to
CAUTION(28)/GAME_SELECT(19) and 2 players **straight to TRACK_SELECT(15)**; track
select then skips the Time-Trial toggle and adds a CPU-racer-count stage.

**The viewport sharp edge did not bite.** Two viewports per frame plus
`mtx_ortho`'s deliberately over-sized ortho viewport was the expected P3.3
regression; WebGPU and GL render the same image and identical physics.

**Bonus:** 4-player loads and renders four quadrant viewports too
(`hud_init: hudPlayers=3 numViewports=4`, rc=0) — but that is a one-route spot
check, not a validated path.

**Left open:** no oracle comparison (a two-player ares route needs per-port input
injection the harness lacks); only Ancient Lake / car / 2 players is validated —
3-player and the challenge/battle race types are untouched; the two-player
post-race flow past `fin=1` is unvalidated. The shared centre minimap (each player
sees part of one map straddling the split) follows from DKR's own
camera-independent `mtx_ortho` and reproduces identically in the 4-player layout,
so it is believed faithful — but it is **not** verified pixel-wise.

## P3.5 / P3.7 — Later in the phase

Adventure hub drivable (Timber's Island), trophy races; then ship the web build
(reclaim the 256 MB arena-alignment cost, controls UI, verify save persistence
across reloads, static hosting with a WebGPU feature-detect fallback).

---

## Rules that apply to every item

1. **AUDIO SAFETY IS A HARD RULE.** Always pass `--headless-frames N` for game
   or test runs; it returns before the SDL audio device is opened. Recognized
   `--help`/`-h` now returns before ROM/window/audio initialization, but an
   unrecognized flag still falls through to an interactive launch. Also set
   `MDKR_AUDIO=0`. Note `MDKR_AUDIO=off` is a **no-op** — the code tests for
   `"0"`.
2. **Root cause before fix.** This codebase punishes guessing: see the bug
   taxonomy in `DEVELOPER_HANDBOOK.md` §3. Four of this project's hardest bugs were silent
   (a denormal, a `-0.0` numerator) and produced no crash at all.
3. **Positive controls are mandatory for anything that "doesn't crash".** A fix
   with no failing case proves nothing — revert it and watch the check fail.
4. **Game-code changes go behind `#ifdef NATIVE_PORT`** so the matching N64 build
   is untouched, with a `_Static_assert` where a layout assumption is involved.
5. **Never claim a pass you did not run.** Paste the command and its output.
