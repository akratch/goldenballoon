# Open items — Everything else

> One subsystem of the split [open-items index](README.md), which states how these files are kept.

**Currently open** (per [`README.md`](README.md#still-open)'s open/closed
table): the decomp-symbol residue below, which is a standing hazard rather than
a defect and closes only as archaeology retires it. Every other entry here is
closed or resolved; nothing is deleted, so the file is also the append-only
historical record.


## Integration pending

**Nothing is pending here any more.** The single item below is done; the section
keeps its name and its anchor because both are linked from the index.

- [x] Wire `asset_swap_normalize` / `asset_swap_lut` hooks into game/src/asset_loading.c per contract in platform/asset_swap.h. (M2 wired the LUT + asset_table_load hooks; M3 wired the per-caller post-inflate/whole-record hooks: LEVEL_MODELS, OBJECT_MODELS + asset_swap_object_animation, OBJECTS, SPRITES, LEVEL_OBJECT_MAPS, LEVEL_HEADERS, and the ASSET_MISC LevelHeader_70 swap.) Gzip'd sections swap AFTER inflate via gzip_inflate_output sizing.

## NOT A DEFECT, but a standing readability hazard: 446 vendored symbols are still spelled as ROM addresses — wave "decompnames"

`game/{src,include}` is vendored from the decomp, and a residue of its symbols is
still named after the address it lives at: `func_800BDC80`, `D_8011C238`. Those
are not names. A reader who lands on a call site learns nothing from them, and
the cost compounds — the 2026-08-07 sync's conflict resolutions, the array-bounds
sweep, and the wave-visibility crash post-mortem all had to re-derive the same
meanings from scratch because nothing had written them down.

**Measured 2026-08-07** by scanning every `.c`/`.h` under `game/` (excluding
`decomp_names.h` itself, which only mentions symbols defined elsewhere):
**154 distinct `func_` and 292 distinct `D_` symbols across 2550 reference
sites.** Concentration by file:

| file | `func_` | `D_` | sites |
|---|---:|---:|---:|
| `game/src/objects.c` | 60 | 51 | 518 |
| `game/src/tracks.c` | 23 | 45 | 524 |
| `game/src/racer.c` | 26 | 22 | 210 |
| `game/src/menu.c` | 17 | 39 | 219 |
| `game/src/waves.c` | 12 | 22 | 297 |
| `game/src/object_functions.c` | 15 | 7 | 77 |
| `game/src/save_data.c` | 12 | 4 | 134 |
| `game/src/thread3_main.c` | 6 | 14 | 47 |
| `game/src/game_ui.c` | 4 | 15 | 106 |
| `game/src/particles.c` | 0 | 21 | 27 |
| `game/src/game_text.c` | 1 | 10 | 67 |
| `game/src/fade_transition.c` | 0 | 12 | 42 |
| all other headers (`objects.h` 44, `tracks.h` 19, `racer.h` 16, `save_data.h` 11, `menu.h` 10, …) | 132 | 6 | 140 |
| all other `.c` (`camera.c`, `object_models.c`, `audiosfx.c`, the `vehicle_*.c` set, …) | 24 | 35 | 142 |

The per-file columns count symbols *distinct within that file*, so they overlap
across files and do not sum to the totals above — a declaration in `objects.h`
and its definition in `objects.c` are one symbol counted twice.

**What was done (2026-08-07).** 84 of them — 45 functions and 39 data symbols —
now have readable aliases in [`game/include/decomp_names.h`](../../game/include/decomp_names.h),
each with a one-line statement of the evidence that earned it. That covers 63 of
the 64 symbols port code actually exercises, plus 21 more the decomp had already
commented well enough to name. Port-authored files use the readable names; the
vendored text is byte-identical, because a rename in vendored text is permanent
merge-conflict surface (see [`DECOMP_SYNC.md`](../DECOMP_SYNC.md)).

**The rule for what remains: never blind-rename.** The residue is not a
find-and-replace backlog. Every name in the alias header cites something —
an existing comment, a dispatch arm, a named field the result is stored into, a
check that pins the contents. The seven symbols this pass refused are listed at
the bottom of the header with the reason each was refused. In six of the seven
it is the decomp's *own* note that disqualifies the symbol: it hedges
("Probably…", "…I think", "seems to be … ?"), says the purpose is unknown, or
says only that the function is dead. A hedge is not evidence, and a plausible
name laundered out of one is worse than the address — the address at least tells
you that you do not know. The same trap already bit this tree twice in the other
direction — the residue audit that opened this wave asserted `D_800DC8AC` and
`D_8011D0F8` were collision data, and both turned out to be camera frustum
culling data, unrelated to collision. Two symbols, two wrong readings, from an
audit that never opened the bodies.

**Where the next pass starts.** `objects.c` and `tracks.c` hold 40 % of the
residue between them, and `objects.h`/`tracks.h`/`racer.h` carry another 110
function declarations with no bodies to read. Those need the ROM's own evidence —
`symbol_addrs.us.v80`, the `.s` files, upstream's tracker — not another reading
of the C. Anything named that way goes in the same header, in the same format,
with the same evidence line.


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

## OPEN: the settings panel's scripted gates depend on a hand-written list of section names — wave "gaterects"

`Settings_draw()` collapses the sections above the category loop for whichever
scripted gate is armed, so a queued click lands on the widget rather than on
whatever the panel had scrolled under it. That works, and it is the wrong layer.

`scriptedGateArmed` is a hand-written disjunction of three env vars, and the two
sections above the loop each carry their own flag. Add a fourth gate, or a third
section, and the trap re-arms silently -- which is exactly how it fired the first
time: the Accessibility section arrived `DefaultOpen`, pushed the Frame limit
combo about 660pt below an 800pt panel, and the failure surfaced as
`frame-limit requested=240 actual=original`, a persistence verdict for a layout
fault. Diagnosing that cost hours.

The root cause is one line, twice. `g_frameLimitRectValid` and
`g_uiScaleRectValid` are sticky latches: set true the first time the widget is
submitted and never cleared, so the harness happily reads a rect captured on
some earlier frame and clicks geometry the panel is currently clipping. Setting
them from `ImGui::IsItemVisible()` each frame instead makes an off-screen target
fail as "not on screen", which is what it is, and demotes the section-collapse
block from a correctness dependency to a convenience.

Not done here because it changes the gate whose diagnosis this entry describes,
and it wants its own mutation pass on a quiet machine rather than a confident
edit at the end of a long session. The off-screen diagnostics added alongside
the collapse block already name the fault when it recurs, so the next occurrence
costs a line of output rather than an afternoon.
