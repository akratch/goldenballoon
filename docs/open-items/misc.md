# Open items — Everything else

> Part of the split [open-items index](README.md). Every defect this port has
> hit is recorded with its mechanism, the measurement that found it, the fix, and
> the check that would catch a regression. **Nothing is deleted when it is
> fixed** — a closed entry is the only warning the next person gets that the same
> trap exists, and retractions are recorded in place rather than removed.


## Integration pending
- [x] Wire `asset_swap_normalize` / `asset_swap_lut` hooks into game/src/asset_loading.c per contract in platform/asset_swap.h. (M2 wired the LUT + asset_table_load hooks; M3 wired the per-caller post-inflate/whole-record hooks: LEVEL_MODELS, OBJECT_MODELS + asset_swap_object_animation, OBJECTS, SPRITES, LEVEL_OBJECT_MAPS, LEVEL_HEADERS, and the ASSET_MISC LevelHeader_70 swap.) Gzip'd sections swap AFTER inflate via gzip_inflate_output sizing.

## NOT A DEFECT, but zero coverage until now: character-select dancers reported static — wave "charselectmotion"

A report surfaced (summarized, not a verbatim quote on file) that the dancing
characters on the PLAYER SELECT screen had stopped animating. Ad-hoc analysis
with `tools/anim_period.py` disproved it: captures were byte-identical across
86 commits, whole-screen motion RMS measured ~14.120, and the dominant
autocorrelation period measured ~20 frames — the dancers were, and always had
been, moving. Related prior art in this tree: `docs/open-items/renderer.md`'s
"headless renders were NOT reproducible" entry (wave "determinism") is what
made this screen's frames comparable run-to-run at all, and
`tests/check_menu_anim_rate.py` already guards the OPPOSITE failure shape (the
T1 bug, dancing 8x too fast) via the same `music_animation_fraction()` /
`obj_loop_char_select()` path (`game/src/object_functions.c`).

**The gap.** Both of those existing gates measure something *about* the
animation but neither one measures "is it still moving at all" as a
regression floor. The disproof above was real analysis, run once, by hand — a
genuine freeze regression in `obj_loop_char_select()` or in the COUNTER-driven
clock it reads would have shipped with zero automated warning.

**Closed by `tests/check_charselect_motion.py`.** Productizes the same
analysis into a permanent gate: a six-window ensemble (the
`check_mip_motion.py` pattern — `WINDOW_FRAMES=24`, `WINDOW_COUNT=6`,
`CAPTURE_COUNT=144`) over whole-screen motion RMS on the character-select
capture, plus a loosely-banded periodicity check. No animation-freeze hook
exists in the engine and none was added for this — the required
broken-direction control is built at the analyzer level instead, by feeding
the scorer the same captured frames with every frame replaced by frame 0
("every frame duplicated from frame 0"); measured motion on that arm is
exactly 0.0 in every window, correctly failing the gate. See
`tests/README.md`'s "Character-select dancer motion" section for the measured
numbers and threshold rationale.
