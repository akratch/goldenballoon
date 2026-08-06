# Open items — Everything else

> One subsystem of the split [open-items index](README.md), which states how these files are kept.


## Integration pending

**Nothing is pending here any more.** The single item below is done; the section
keeps its name and its anchor because both are linked from the index.

- [x] Wire `asset_swap_normalize` / `asset_swap_lut` hooks into game/src/asset_loading.c per contract in platform/asset_swap.h. (M2 wired the LUT + asset_table_load hooks; M3 wired the per-caller post-inflate/whole-record hooks: LEVEL_MODELS, OBJECT_MODELS + asset_swap_object_animation, OBJECTS, SPRITES, LEVEL_OBJECT_MAPS, LEVEL_HEADERS, and the ASSET_MISC LevelHeader_70 swap.) Gzip'd sections swap AFTER inflate via gzip_inflate_output sizing.

## NOT A DEFECT, but zero coverage until now: character-select dancers reported static — wave "charselectmotion"

A report surfaced (summarized, not a verbatim quote on file) that the dancing
characters on the PLAYER SELECT screen had stopped animating. Ad-hoc analysis
with `tools/anim_period.py` disproved it: captures were byte-identical across
86 commits, whole-screen motion RMS measured ~14.120, and the dominant
autocorrelation period measured ~20 frames — the dancers were, and always had
been, moving. Related prior art in this tree: `docs/open-items/renderer.md`'s
"headless renders were NOT reproducible" entry (wave "determinism") is what
made this screen's frames comparable run-to-run at all. The older rate-only
check sampled a hand-tuned window and caught the T1 bug (dancing 8x too fast)
through the same `music_animation_fraction()` / `obj_loop_char_select()` path,
but did not prove the dancers were moving at all. Its capture window later
became stale as frontend timing settled, so it was retired rather than
re-baselined.

**Closed by `tests/check_charselect_motion.py`.** Productizes the same
analysis into a permanent gate: a six-window ensemble (the
`check_mip_motion.py` pattern — `WINDOW_FRAMES=24`, `WINDOW_COUNT=6`,
`CAPTURE_COUNT=144`) over whole-screen motion RMS on the character-select
capture, plus a loosely-banded periodicity check that rejects the original
too-fast cycle as well as an implausibly slow one. No animation-freeze hook
exists in the engine and none was added for this — both broken-direction
controls are built at the analyzer level. The frozen arm replaces every frame
with frame 0 and must fail the motion floors; the fast arm loops five phases
sampled across one healthy cycle and must fail the bounded-period assertion.
See
`tests/README.md`'s "Character-select dancer motion" section for the measured
numbers and threshold rationale.
