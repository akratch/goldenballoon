#!/usr/bin/env python3
"""No NEW out-of-bounds array index -- and no new instance of three bug SHAPES
that no runtime instrument in this tree can see -- may appear in game/src.

Three phases, and the second and third exist because the first cannot see them:

  1. RUNTIME. Build with UBSan array-bounds + pointer-overflow + shift-exponent,
     drive 8 routes, fail on any report not in ALLOWED.
  2. STATIC (tools/sweep_bug_shapes.py). Enumerate three shapes UBSan is blind
     to, and fail on any TRIAGE-severity finding without a reason in
     SHAPE_TRIAGE, or on any growth in the informational population.
  3. CONTROLS. The bounds phase 2 is about are UNREACHED in normal play --
     minimum measured slack 4, 24, 4, 56, 291 slots, and 84 of 500 candidates --
     so phases 1 and 2 alone would pass on a build with every bound deleted.
     Phase 3 forces each boundary from the same binary, requires the guarded and
     unguarded arms to differ, and separately checks that every bound really is
     the caller's own capacity (a bound can be passed WRONG with no symptom).

Why this check exists
---------------------
The worst bug class this port has hit is *the decomp splits one ROM array into two
adjacent C objects, and the code indexes across the boundary*. It is invisible on
the host we develop on, because whether the two objects end up adjacent is a
linker decision and the two linkers disagree: measured on this tree, **432 of the
1001 data pairs that have the same neighbour on both targets sit at a different
distance** on LP64 Mach-O than on wasm32. So "it works natively" carries no
information at all, and a native crash-free suite carries none either — the wave
table (docs/OPEN_ITEMS.md "wavetable") crashed two real players in the browser
while every native fixture passed.

The one instrument that saw it *without a crash, on the host, before it shipped*
was UBSan `-fsanitize=array-bounds`. It had been reporting
`waves.c:535 index 2 out of bounds for 'unk8012A5E8[2]'` all along and nobody was
looking. This check is "somebody is looking": it builds an instrumented binary,
drives the fixtures, and fails on any out-of-bounds index that is not already
triaged in ALLOWED below.

It catches the *cause* (an index leaving its array) rather than one symptom, so it
generalises to the whole class, on any future toolchain — which is what
tests/check_wave_visible_table.py cannot do, since that one pattern-matches the
codegen of a single table.

What is allow-listed, and why that is not a whitewash
----------------------------------------------------
Every entry below is a decomp idiom that was measured and found harmless: a
*flattened 2-D index* (`arr[0][i]` walking past the inner array but staying inside
the same object), or the documented LP64 `s32`-as-a-pointer idiom. Each has to
name what it is. Anything else is a finding.

Entries are keyed on (file, type, source snippet), NOT on a line number, so that
editing a file does not spuriously re-baseline the whole list. Move the code and
the entry still matches; *change* the offending line and it does not, which is
exactly when a human should look again.

Fails closed
------------
"No reports" cannot distinguish a clean build from a build that is not measuring:

  * the instrumented binary must actually import `__ubsan_handle_out_of_bounds`
    (i.e. the sanitizer flag survived the CMake flag ordering — note that
    `-Wno-everything` in CMakeLists.txt is applied *after* CMAKE_C_FLAGS, which is
    why the sweep flags go through -DMDKR_EXTRA_C_FLAGS);
  * every run must exit 0;
  * REQUIRED_SENTINELS must all still be reported. They are allow-listed idioms on
    paths every route crosses, so if they go quiet the instrument is broken, not
    the game.

Verified in both directions, on real builds
-------------------------------------------
    fixes applied          -> 4 sites, every one allow-listed              PASS
    gTrackSelectIDsTable union reverted (game/src/menu.c)
                           -> + menu.c:8502 "index 4 out of bounds for
                              type 's16[4][6]'"                            FAIL
    gScreenViewports[5] reverted (game/src/camera.c)
                           -> + camera.c:1097 "index 4 out of bounds for
                              type 'ScreenViewport[4]'"                    FAIL
    -- phase 1, shift-exponent, before the source fix:
    `& 31` removed at game.c's `8 << (worldId + 31)`
                           -> + game.c "shift exponent 32 is too large"
                              on route boss38                              FAIL
    `& 31` removed at waves.c's `var_t0 <<= (unk2 + 0x1F)`
                           -> + waves.c "shift exponent 32 is too large"
                              on route nav_to_track_select                 FAIL
    -- phase 3, the boundary controls (these are the ones that matter, because
       every bound in phase 2 is unreached and would otherwise be untested):
    MDKR_SEGMARGIN forced, bound in place
                           -> exits 0, xzClamped > 0                       PASS
    MDKR_SEGMARGIN forced, MDKR_SEGBOUND=legacy
                           -> the run dies                                 (control)
    MDKR_COLLCAP=32, guards in place
                           -> maxCandidates == 32, never above             PASS
    MDKR_COLLCAP=legacy    -> maxCandidates far above 32                   (control)

The allow-list survived the reported lines moving 7378 -> 7383 and 2994 -> 3001
between two runs of this sweep, which is the snippet key doing its job.

MUTED + HEADLESS: every run passes --headless-frames (which returns before the SDL
audio device is opened) and MDKR_AUDIO=0.

Usage:
    tests/check_array_bounds_sweep.py
    tests/check_array_bounds_sweep.py --no-build      # reuse build-ubsan as-is
    tests/check_array_bounds_sweep.py -v
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

from harness_utils import exclusive_build_dir

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = "build-ubsan"

# ===========================================================================
# PHASE 2 -- the STATIC class sweep (tools/sweep_bug_shapes.py)
# ===========================================================================
# CONTRIBUTING.md rule 6: fix the instance, then sweep the class. Three shapes
# no runtime instrument in this tree can see are enumerated statically, and
# every TRIAGE-severity finding must appear below with the reason it is not a
# defect. Keys are (class, file, key) -- function + parameter, or function +
# comparison -- so moving code does not re-baseline the list but renaming or
# re-parameterising the offending function does.
#
# INFO-severity findings are counted, not enumerated: the count may fall freely
# but may not RISE above the ceiling recorded in SHAPE_INFO_MAX, so a newly
# introduced instance fails even though the harmless population is not
# hand-maintained.
SHAPE_TRIAGE = {
    # ---------------------------------------------------------------------
    # bare-pointer: a callee writes N elements through a pointer parameter,
    # where N is not derivable from the parameters. All 27 instances in
    # game/src, every one of them triaged.
    # ---------------------------------------------------------------------
    # -- FIXED in this commit: a maxOut parameter added, ARRAY_COUNT at every
    #    call site. Kept listed because the shape is still "writes through a
    #    pointer in a loop" -- the enumerator cannot see the bound, only a human
    #    can, so the entry IS the record that a human did.
    ("bare-pointer", "game/src/camera.c",
     "mdkr_camera_interpolated_view_projections:out"):
        "BOUNDED BY CONSTRUCTION: the loop is `for (viewport = 0; viewport < "
        "capacity; viewport++)` and every write is out[viewport] — the bound "
        "parameter IS the loop bound, so no count can exceed it. The sole "
        "caller (stubs_dkr.c presentation subloop) passes ARRAY_COUNT of a "
        "local GfxShadowReplayViewProjection views[4], matching "
        "GFX_SHADOW_MAX_VIEWS. NULL out returns 0.",
    ("bare-pointer", "game/src/tracks.c", "get_inside_segment_count_xz:arg2"):
        "BOUNDED (this commit): maxOut added; caller passes ARRAY_COUNT"
        "(segmentsInside[8]). Peak 4 of 8, min slack 4, 0 calls at the bound.",
    ("bare-pointer", "game/src/tracks.c", "get_inside_segment_count_xyz:arg0"):
        "BOUNDED (this commit): maxOut added; caller passes ARRAY_COUNT(inSegs[28]). "
        "Peak 4 of 28, 0 calls at the bound.",
    ("bare-pointer", "game/src/tracks.c", "collision_get_y:yOut"):
        "BOUNDED (this commit): maxOut added at all five call sites (arrays of "
        "16, 16, 16, 16, 30). Peak count 7; ordinary callers have at least 11 "
        "slots of measured slack and 0 calls at the bound. The count is per "
        "TRIANGLE, not per segment, so it is the widest-open member of the class.",
    ("bare-pointer", "game/src/waves.c", "func_800BDC80:arg1"):
        "BOUNDED (this commit): maxOut added; caller passes min(ARRAY_COUNT"
        "(D_8011C3B8), room left in D_8011C8B8). Peak 8 of 64.",
    ("bare-pointer", "game/src/waves.c", "func_800BDC80:arg2"):
        "BOUNDED (this commit): same call, second output pointer, same bound.",
    # -- func_80017A18: NEW to the enumerator in wave "objcoll", not new to the
    #    tree. Its body was `#ifdef NON_EQUIVALENT` and never compiled, so the
    #    sweep could not see it; adopting upstream's matched body (decomp
    #    9da89ecb) made four output pointers visible at once. All four are
    #    bounded by the SAME literal, which is the strongest answer this class
    #    admits: `arg1` is a compile-time constant at every call site.
    #
    #      racer.c:6875  collision_objectmodel(racerObj, 4, ...)  -> arg1 == 4
    #      racer.c:7088  collision_objectmodel(obj,      1, ...)  -> arg1 == 1
    #
    #    and those are the only two callers in the tree. Inside
    #    collision_objectmodel the buffers passed on as arg6/arg7/arg8 are its
    #    own f32[4] locals (sp100/spF0/spE0); argA is the caller's `surface`
    #    (racer.c sp58[4], or a single s8 at the arg1 == 1 site). Capacity 4,
    #    count 4 -- at the bound with zero slack, but the count is a literal, so
    #    it cannot be exceeded without editing a call site.
    #
    #    WATCH: if a third caller ever passes a non-literal arg1, all four of
    #    these become live and collision_objectmodel's own f32[4] locals overflow
    #    first -- before func_80017A18 is even entered.
    #    The 100%-match sync adopted upstream's named parameters for this
    #    function (arg6/arg7/arg8/argA became targetPointsX/Y/Z/surfaces); the
    #    bodies were verified byte-identical in behaviour, so the reasoning
    #    below carries over unchanged under the new keys.
    ("bare-pointer", "game/src/objects.c", "func_80017A18:targetPointsX"):
        "BOUNDED BY LITERAL: writes targetPointsX[i] for i < arg1; arg1 is 4 "
        "or 1 at the only two call sites, into collision_objectmodel's f32[4] "
        "locals.",
    ("bare-pointer", "game/src/objects.c", "func_80017A18:targetPointsY"):
        "BOUNDED BY LITERAL: same loop, same count, same f32[4] capacity.",
    ("bare-pointer", "game/src/objects.c", "func_80017A18:targetPointsZ"):
        "BOUNDED BY LITERAL: same loop, same count, same f32[4] capacity.",
    ("bare-pointer", "game/src/objects.c", "func_80017A18:surfaces"):
        "BOUNDED BY LITERAL: writes surfaces[i] = 0 for i < arg1; capacity is "
        "racer.c's s8 sp58[4] at the arg1 == 4 site and a single s8 at the "
        "arg1 == 1 site. At the bound, zero slack, not exceedable.",
    #    (arg2 is deliberately NOT listed: it is only ever `arg2[0]++`, a single
    #    counter cell, so the enumerator does not classify it as this shape and a
    #    triage entry for it would itself be flagged as matching nothing.)
    # -- bounded by an explicit count parameter: not this shape at all. -----
    ("bare-pointer", "game/src/hasm/collision.c", "resolve_collisions:surface"):
        "BOUNDED BY PARAMETER: `surface++` happens once per `while (numEntries > 0)` "
        "iteration and numEntries is a parameter (callers pass 1 or 4 against a "
        "4-element array). The count is the caller's, which is what this class is "
        "missing everywhere else.",
    ("bare-pointer", "game/src/video.c", "fb_memcpy:dest"):
        "BOUNDED BY PARAMETER: `for (i = 0; i < len; i++) *dest++ = *src++;`.",
    ("bare-pointer", "game/src/save_data.c", "get_controller_pak_file_list:fileSizes"):
        "BOUNDED BY PARAMETER: maxNumOfFilesToGet caps the fill loop.",
    ("bare-pointer", "game/src/save_data.c", "get_controller_pak_file_list:fileTypes"):
        "BOUNDED BY PARAMETER: same loop, same cap.",
    ("bare-pointer", "game/src/save_data.c", "font_codes_to_string:outString"):
        "BOUNDED BY PARAMETER: stringLength. Also save_data.c, owned by another "
        "wave -- recorded, not touched.",
    ("bare-pointer", "game/src/save_data.c", "string_to_font_codes:outString"):
        "BOUNDED BY PARAMETER: stringLength. save_data.c, not touched.",
    ("bare-pointer", "game/src/menu.c", "filename_decompress:output"):
        "BOUNDED BY PARAMETER: writes output[0..length]; every caller passes "
        "MAX_INITIALS_LENGTH. NOTE it writes output[length] as the terminator, so "
        "the buffer must be length+1 -- true at every call site today.",
    # -- unreachable: no caller exists. ------------------------------------
    ("bare-pointer", "game/src/audio.c", "music_get_fx_mix_all_channels:channelFXMix"):
        "UNREACHABLE: marked UNUSED and has zero callers in the tree. Genuinely "
        "unbounded (one u8 per gMusicPlayer->maxChannels) if it ever gains one.",
    # -- bounded by the allocator that sized the array. ---------------------
    ("bare-pointer", "game/src/camera_dynamic_occlusion.c",
     "mdkr_camera_dynamic_index_clear:buckets"):
        "BOUNDED BY CONSTRUCTION: the loop bound is sIndexBucketCount, which the "
        "same allocator sets to the calloc element count of all three bucket "
        "arrays it can be handed. Every call site passes one of those file "
        "statics and is guarded by the sIndexBucketCount == 0 early return.",
    ("bare-pointer", "game/src/camera_object_occlusion.c",
     "mdkr_camera_object_occlusion_sort_chunk_range:order"):
        "BOUNDED BY PARAMETER: writes only order[position] for "
        "begin <= position < end; end is the bound, and the recursion narrows "
        "[begin, end) monotonically from the root call's chunk_count, which is "
        "the array's fill count.",
    # -- the C-string contract: bounded by the input's NUL. -----------------
    ("bare-pointer", "game/src/font.c", "parse_string_with_number:output"):
        "STRING CONTRACT: copies until the input's NUL, so the output buffer must "
        "be at least strlen(input)+1 (plus digit expansion for `~`). Reported "
        "TWICE because `#if REGION != REGION_JP` is not decidable by the sweep's "
        "arm selector, which deliberately keeps both arms rather than lose "
        "coverage; only the non-JP arm is compiled here.",
    ("bare-pointer", "game/src/font.c", "fontConvertString:outString"):
        "STRING CONTRACT: NUL-terminated copy with in-place code translation, "
        "output sized by the caller from the same literal.",
    ("bare-pointer", "game/src/menu.c", "filename_trim:output"):
        "ALREADY HANDLED: this exact overrun was found and fixed in an earlier "
        "wave -- see the NATIVE_PORT comment at menu.c's trimmedFilename "
        "declaration, 'the N64 declares this [4], but filename_trim() writes "
        "strlen(input)+1 bytes'. The callee is still unbounded; the callers were "
        "resized. menu.c is not this wave's file.",
    ("bare-pointer", "game/src/tracks.c", "func_8002FF6C:arg1"):
        "ALREADY HANDLED: the same earlier wave sized func_8002F440's sp90/sp80 to "
        "the real clip maximum of 8 (N64 declares 6), with a NATIVE_PORT comment. "
        "Frustum clipping of a triangle is bounded at 7 output vertices by "
        "geometry, so the caller-side fix is a complete one.",
    ("bare-pointer", "game/src/tracks.c", "func_80026E54:arg1"):
        "ALREADY HANDLED: the Adventure-hub wave sized this caller's sp94 to "
        "2*10 with a _Static_assert after measuring arg0=9; see the NATIVE_PORT "
        "comment in the function. `arg1` itself is a sort scratch of arg0 bytes "
        "supplied by the caller, bounded by the same arg0 < 10.",
    # -- menu/save paths this wave does not own. ---------------------------
    ("bare-pointer", "game/src/menu.c", "savemenu_blank_save_destination:file"):
        "RECORDED, NOT TOUCHED (menu.c): writes file[*fileIndex] once per new-game "
        "slot, at most NUMBER_OF_SAVE_FILES, into a destination array the caller "
        "also fills from other sources through the same running index. Bounded by "
        "the slot count, but the capacity is not passed. No measured overrun.",
    ("bare-pointer", "game/src/save_data.c", "func_800756D4:levelIDs"):
        "RECORDED, NOT TOUCHED (save_data.c is another wave's file): controller-pak "
        "ghost enumeration, four parallel output arrays, count from pak directory "
        "contents. The port has no controller pak, so it is unreachable here.",
    ("bare-pointer", "game/src/save_data.c", "func_800756D4:vehicleIDs"):
        "RECORDED, NOT TOUCHED: same call, second output array.",
    ("bare-pointer", "game/src/save_data.c", "func_800756D4:characterIDs"):
        "RECORDED, NOT TOUCHED: same call, third output array.",
    ("bare-pointer", "game/src/save_data.c", "func_800756D4:checksumIDs"):
        "RECORDED, NOT TOUCHED: same call, fourth output array.",

    # ---------------------------------------------------------------------
    # equality-cap: `i == CAP` guarding a write indexed by i, where a second
    # increment can advance i past CAP.
    # ---------------------------------------------------------------------
    # These HUD_* values are element identifiers, not capacities. The detector
    # conservatively treats all-caps constants as possible caps, but this block
    # is a set of per-element scale exceptions inside
    # `for (i = 0; i < HUD_ELEMENT_COUNT; i++)`. Every entry[i] write is guarded
    # by that loop bound; the comparisons neither stop nor advance the loop.
    ("equality-cap", "game/src/game_ui.c",
     "hud_init_element:i!=HUD_RACE_POSITION"):
        "NOT A CAPACITY: HUD element ID used by a scale-exception predicate; "
        "the enclosing i < HUD_ELEMENT_COUNT loop bounds every entry[i] write.",
    ("equality-cap", "game/src/game_ui.c",
     "hud_init_element:i!=HUD_RACE_POSITION_END"):
        "NOT A CAPACITY: same bounded HUD element-exception predicate.",
    ("equality-cap", "game/src/game_ui.c",
     "hud_init_element:i!=HUD_WEAPON_DISPLAY"):
        "NOT A CAPACITY: same bounded HUD element-exception predicate.",
    ("equality-cap", "game/src/game_ui.c",
     "hud_init_element:i!=HUD_PRO_AM_LOGO"):
        "NOT A CAPACITY: same bounded HUD element-exception predicate.",
    ("equality-cap", "game/src/game_ui.c",
     "hud_init_element:i!=HUD_CHALLENGE_FINISH_POS_1"):
        "NOT A CAPACITY: same bounded HUD element-exception predicate.",
    ("equality-cap", "game/src/game_ui.c",
     "hud_init_element:i!=HUD_CHALLENGE_FINISH_POS_2"):
        "NOT A CAPACITY: same bounded HUD element-exception predicate.",
    ("equality-cap", "game/src/game_ui.c",
     "hud_init_element:i!=HUD_LAP_COUNT_LABEL"):
        "NOT A CAPACITY: same bounded HUD element-exception predicate.",
    ("equality-cap", "game/src/game_ui.c",
     "hud_init_element:i!=HUD_CHALLENGE_PORTRAIT"):
        "NOT A CAPACITY: same bounded HUD element-exception predicate.",
    ("equality-cap", "game/src/game_ui.c",
     "hud_init_element:i!=HUD_EGG_CHALLENGE_ICON"):
        "NOT A CAPACITY: same bounded HUD element-exception predicate.",
    ("equality-cap", "game/src/game_ui.c",
     "hud_init_element:i==HUD_BANANA_COUNT_SPARKLE"):
        "NOT A CAPACITY: same bounded HUD element-exception predicate.",
    ("equality-cap", "game/src/game_ui.c",
     "hud_init_element:i==HUD_BANANA_COUNT_NUMBER_2"):
        "NOT A CAPACITY: same bounded HUD element-exception predicate.",
    ("equality-cap", "game/src/hasm/collision.c",
     "generate_collision_candidates:j==MAX_COLLISION_CANDIDATES"):
        "GUARDED (this commit): the ROM's equality test is kept exactly, and a "
        "`j >= cap` pre-check was added at BOTH inserts, so j can no longer reach "
        "the facet insert already at the cap. Peak 416 of 500, 0 truncations.",
    ("equality-cap", "game/src/hasm/collision.c",
     "generate_collision_candidates:counter==10"):
        "SAFE: `counter` has ONE increment, immediately after the only write, and "
        "starts at 0, so it lands on 10 exactly and the test always fires. This is "
        "the same function as the defect above and the contrast is the point -- one "
        "increment cannot step over an equality test, two can.",
    ("equality-cap", "game/src/audio_spatial.c",
     "audspat_point_create:gNumAudioPoints==MAX_AUDIO_POINTS"):
        "SAFE: the test is a PRE-check that returns early, and the single increment "
        "is on the path past it, so the counter cannot exceed MAX_AUDIO_POINTS.",
    ("equality-cap", "game/src/waves.c", "waves_visibility:var_v1!=ARRAY_COUNT"):
        "SAFE, BUT COUPLED -- and now asserted. The reset loop strides FOUR and "
        "stops on `!=`, so it terminates only because ARRAY_COUNT(D_8012A600) is 24. "
        "That is WAVE_VISIBLE_SLOTS - 2, a ROM fact one correction away from moving; "
        "at 25 or 27 the loop writes past the table, in the exact data neighbourhood "
        "of the two browser crashes in docs/OPEN_ITEMS.md 'wavetable'. A "
        "_Static_assert on the divisibility was added in this commit.",
    ("equality-cap", "game/src/objects.c", "func_8001F460:var_t0!=D_8011AE78"):
        "SAFE: linear-search idiom. The preceding loop is `for (var_t0 = 0; var_t0 < "
        "D_8011AE78 && ...; var_t0++) {}`, so var_t0 lands on D_8011AE78 exactly and "
        "`!=` means 'found'. The comparand is a count, not a capacity.",
    ("equality-cap", "game/src/objects.c", "func_80021600:j!=D_8011AE78"):
        "SAFE: the same linear-search idiom, same bound, same reasoning.",
    ("equality-cap", "game/src/objects.c", "func_8001F460:var_s2!=0x7F"):
        "SAFE: 0x7F is a SENTINEL value in the animation-node stream, not a "
        "capacity -- the writes indexed by var_s2 are bounded by their own loops.",

    # ---------------------------------------------------------------------
    # shift-count: a shift whose count can reach the width of the type. All
    # five MIPS-mask idioms in game/src; the other 141 shifts by a variable
    # are INFO and are covered at runtime by -fsanitize=shift-exponent.
    # ---------------------------------------------------------------------
    # All five are FIXED, by wave "keyshift" (DKR_SHL32 in game/include/macros.h),
    # not here. They stay listed because the shape is still present in the source
    # and this list is what records that a human read each one. Do not re-derive
    # the reasoning: read that section. It measured what this wave only suspected
    # -- clang folds these to zero at -O2, so they were LIVE, not latent.
    ("shift-count", "game/src/objects.c", "track_setup_racers:MIPS-MASK-IDIOM"):
        "FIXED by wave \"keyshift\" (DKR_SHL32). j is 1..3 and the masked results "
        "0x1/0x2/0x4 are exactly the three TAJ_FLAGS_*_UNLOCKED bits the gate above "
        "reads. At -O2 the whole statement was DELETED, on arm64 and wasm32 alike.",
    ("shift-count", "game/src/game.c", "level_load:MIPS-MASK-IDIOM"):
        "FIXED by wave \"keyshift\" (DKR_SHL32), three sites in level_load(). With "
        "world 1..4 the key gate/latch yields the four CUTSCENE_*_KEY constants in "
        "world order -- corroborated by the `if` excluding Future Fun Land, which "
        "has no key constant -- and `8 << (worldId + 31)` with worldId 1..5 yields "
        "the five CUTSCENE_*_BOSS constants. The first two ARE the reported "
        "'key animation plays after every race'.",
    # obj_wave_height's shift needs no entry any more: the 100%-match sync
    # moved the -1 inside DKR_SHL32's masked operand, so the added-constant
    # idiom is gone and the site classifies as INFO var-count. The keyshift
    # reasoning it used to carry lives in docs/open-items/portability.md.
}

# Ceilings for the INFO population, per class. May fall; may not rise.
SHAPE_INFO_MAX = {
    "bare-pointer": 0,
    # 29 pre-1.0.5 + 3 attributed additions, each verified bounded:
    # camera_object_occlusion.c sweep/rounded-lens `stack_count != 0` (x2) are
    # the node-stack pop underflow guards; every two-slot push is fenced by
    # `stack_count + 2 > limits->nodes` immediately beforehand, which also
    # reports exhaustion. object_functions.c
    # `obj_char_select_batch_texture_index:numCursors == 0` is an early-out on
    # an empty cursor list filled by the MAXCONTROLLERS loop into
    # playerSelectIndices[MAXCONTROLLERS]; it caps nothing.
    "equality-cap": 32,
    "shift-count": 141,
}

# Only array-bounds is load-bearing for this class. pointer-overflow is kept
# because it is the second-cheapest reader of the LP64 pointer-truncation class and
# costs nothing to collect; object-size needs optimisation to do anything and the
# sweep build is -O0, so it is left off rather than pretended to be covered.
# shift-exponent reads a DIFFERENT class that array-bounds is blind to: the decomp
# renders MIPS `sllv` (which masks the shift count to 5 bits) as `1 << (j + 31)`,
# which is UB in C. It is currently correct on arm64 only because arm64 masks
# identically AND clang emitted a register shift instead of folding -- luck, not
# portability. See docs/OPEN_ITEMS.md "five sites depend on MIPS masking the shift
# count".
SANITIZERS = "array-bounds,pointer-overflow,shift-exponent"

# (file, type-or-"ptr", source snippet prefix) -> why it is not a finding.
# The snippet is whitespace-normalised and compared as a prefix, 48 chars.
ALLOWED = {
    # --- flattened 2-D index: the whole object is walked through row 0. ---------
    # `u8 gCharacterVolumes[10][2]` is one 20-byte run and the code addresses it as
    # `[0][n*2+1]`. The largest flat index observed anywhere in the sweep is 19,
    # i.e. the LAST element, so the access never leaves the object on any target.
    # ISO-C UB, but memory-safe, layout-independent and identical to the N64.
    ("game/src/menu.c", "u8[2]",
     "music_channel_off(gCharacterVolumes[0][D_801263B"):
        "flattened gCharacterVolumes[10][2] walk, max flat index 19 of 20.",
    ("game/src/menu.c", "u8[2]",
     "music_channel_on(gCharacterVolumes[0][gMenuSelec"):
        "flattened gCharacterVolumes[10][2] walk.",
    ("game/src/menu.c", "u8[2]",
     "music_channel_fade_set(gCharacterVolumes[0][gMen"):
        "flattened gCharacterVolumes[10][2] walk.",
    ("game/src/menu.c", "u8[2]",
     "music_channel_fade_set(gCharacterVolumes[0][D_80"):
        "flattened gCharacterVolumes[10][2] walk.",
    ("game/src/racer.c", "s8[2]",
     "tempVel = (f32) D_800DCDB0[0][racer->miscAnimCou"):
        "flattened D_800DCDB0[16][2] (== 32 bytes) walk indexed by "
        "`miscAnimCounter & 0x1F`, i.e. 0..31 — exactly the whole object.",
}

# Production diagnostics may remain triaged in ALLOWED while they are repaired,
# but none is required to stay broken. The test-only executable in
# tests/fixtures/ubsan_positive_control.c proves that each sanitizer class is
# live even after the game becomes completely clean.
REQUIRED_SENTINELS = []

# (name, extra env, frames, input script or None). Chosen for breadth per second:
# the menu graph (which is where the track-select grid is built), a full Tracks-mode
# race, a boss race, and the attract demo.
ROUTES = [
    ("nav_to_track_select", {}, 2300, "nav_to_track_select.txt"),
    ("nav_to_character_select", {}, 1600, "nav_to_character_select.txt"),
    ("nav_to_magic_codes", {}, 2000, "nav_to_magic_codes.txt"),
    ("nav_to_file_select_adventure", {}, 2100, "nav_to_file_select_adventure.txt"),
    ("race_tt", {"MDKR_AUTOPILOT": "1"}, 5200, "race_full_3lap_tt.txt"),
    ("race_wavetrack19", {"MDKR_LOAD_TRACK": "19", "MDKR_AUTOPILOT": "1"}, 4000,
     "race_full_3lap_tt.txt"),
    ("boss38", {"MDKR_LOAD_TRACK": "38", "MDKR_AUTOPILOT": "1"}, 4000,
     "race_full_3lap.txt"),
    ("attract", {}, 3000, None),
]

REPORT_RE = re.compile(
    r"^(?P<file>[^:]+):(?P<line>\d+):\d+: runtime error: "
    r"(?:index (?P<idx>-?\d+) out of bounds for type '(?P<type>[^']+)'"
    r"|(?P<ptr>pointer index expression|applying non-zero offset)"
    r"|(?P<shift>shift exponent) -?\d+ is too large)")


def sh(cmd, log, cwd=ROOT, env=None):
    with open(log, "wb") as fh:
        return subprocess.call(cmd, cwd=cwd, stdout=fh, stderr=subprocess.STDOUT,
                               env=env)


def build(verbose):
    d = os.path.join(ROOT, BUILD_DIR)
    log = os.path.join(tempfile.gettempdir(), "mdkr_ubsan_cfg.log")
    rc = sh(["cmake", "-S", ROOT, "-B", d, "-DCMAKE_BUILD_TYPE=Debug",
             "-DMDKR_EXTRA_C_FLAGS=-fsanitize=%s -fno-omit-frame-pointer" % SANITIZERS,
             "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=%s" % SANITIZERS], log)
    if rc:
        print("  FAIL: cmake configure of %s failed (see %s)" % (BUILD_DIR, log))
        return False
    log = os.path.join(tempfile.gettempdir(), "mdkr_ubsan_build.log")
    rc = sh(["cmake", "--build", d, "-j%d" % (os.cpu_count() or 4)], log)
    if rc:
        print("  FAIL: build of %s failed (see %s)" % (BUILD_DIR, log))
        return False
    if verbose:
        print("  built %s/mdkr64" % BUILD_DIR)
    return True


# One imported handler per sanitizer that this check draws conclusions from. If a
# flag is silently dropped (CMake flag ordering has done this before) the runs go
# quiet and a PASS would be a lie -- so the absence of a handler is a FAILURE, not
# an "all clear". shift-exponent is listed because a *source* fix at every reached
# site legitimately empties its report list, and then nothing else would prove the
# instrument is still armed.
REQUIRED_HANDLERS = [
    ("__ubsan_handle_out_of_bounds", "array-bounds"),
    ("__ubsan_handle_pointer_overflow", "pointer-overflow"),
    ("__ubsan_handle_shift_out_of_bounds", "shift-exponent"),
]


def instrumented(binary):
    """Every sanitizer flag really did survive into the link. Returns [missing]."""
    out = subprocess.run(["nm", "-u", binary], capture_output=True,
                         text=True).stdout
    return [(sym, flag) for sym, flag in REQUIRED_HANDLERS if sym not in out]


def sanitizer_positive_controls(verbose):
    """Compile and execute one deliberate test-only defect per sanitizer."""
    cache_path = os.path.join(ROOT, BUILD_DIR, "CMakeCache.txt")
    compiler = None
    try:
        with open(cache_path, errors="replace") as cache:
            for line in cache:
                match = re.match(r"CMAKE_C_COMPILER(?::FILEPATH)?=(.+)", line)
                if match:
                    compiler = match.group(1).strip()
                    break
    except OSError:
        pass
    if not compiler or not os.path.exists(compiler):
        print("  FAIL: cannot resolve CMAKE_C_COMPILER from %s" % cache_path)
        return False

    source = os.path.join(
        ROOT, "tests", "fixtures", "ubsan_positive_control.c")
    expected = {
        "array": ("out of bounds",),
        "pointer": ("pointer index expression", "applying non-zero offset"),
        "shift": ("shift exponent",),
    }
    ok = True
    with tempfile.TemporaryDirectory(prefix="mdkr_ubsan_control_") as temp:
        binary = os.path.join(temp, "ubsan_positive_control")
        compile_proc = subprocess.run(
            [
                compiler, source, "-O0", "-g",
                "-fsanitize=%s" % SANITIZERS,
                "-fno-sanitize-recover=all", "-o", binary,
            ],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True,
        )
        if compile_proc.returncode != 0:
            print("  FAIL: UBSan positive-control compile exited %d"
                  % compile_proc.returncode)
            if verbose:
                print(compile_proc.stdout)
            return False
        for name, needles in expected.items():
            proc = subprocess.run(
                [binary, name], cwd=ROOT, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True,
                env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1"),
            )
            witnessed = any(value in proc.stdout for value in needles)
            if proc.returncode == 0 or not witnessed:
                print("  FAIL: %s UBSan positive control exited %d; "
                      "expected a diagnostic containing %s"
                      % (name, proc.returncode, " or ".join(needles)))
                if verbose:
                    print(proc.stdout)
                ok = False
            elif verbose:
                print("  control ubsan-%-7s exit=%d diagnostic witnessed"
                      % (name, proc.returncode))
    return ok


def snippet(path, line):
    try:
        with open(os.path.join(ROOT, path), errors="replace") as fh:
            for n, text in enumerate(fh, 1):
                if n == line:
                    return re.sub(r"\s+", " ", text).strip()[:48]
    except OSError:
        pass
    return ""


def collect(binary, rom, logdir, verbose):
    """Run every route; return ({key: (line, snippet, route)}, ok)."""
    found, ok = {}, True
    for name, extra, frames, script in ROUTES:
        env = dict(os.environ)
        env["MDKR_AUDIO"] = "0"
        env["MDKR_PRESENT_RATE"] = "original"
        env["MDKR_SIMULATION_CADENCE"] = "original"
        env["MDKR_SYNTH_FIELDS"] = "2"
        env["UBSAN_OPTIONS"] = "halt_on_error=0:log_path=%s/%s" % (logdir, name)
        env.update(extra)
        cmd = [binary, "--headless-frames", str(frames), "--rom", rom]
        if script:
            cmd += ["--input-script", os.path.join("tests/input_scripts", script)]
        log = os.path.join(logdir, "%s.stdout" % name)
        rc = sh(cmd, log, env=env)
        nrep = 0
        for path in glob.glob("%s/%s.*" % (logdir, name)):
            if path.endswith(".stdout"):
                continue
            for text in open(path, errors="replace"):
                m = REPORT_RE.match(text.strip())
                if not m:
                    continue
                nrep += 1
                rel = os.path.relpath(m.group("file"), ROOT) \
                    if m.group("file").startswith(ROOT) else m.group("file")
                kind = m.group("type") or ("shift" if m.group("shift") else "ptr")
                line = int(m.group("line"))
                key = (rel, kind, snippet(rel, line))
                found.setdefault(key, (line, m.group(0), name))
        if rc != 0:
            print("  FAIL: route %s exited %d (see %s)" % (name, rc, log))
            ok = False
        if verbose:
            print("  %-30s exit=%d reports=%d" % (name, rc, nrep))
    return found, ok


def shape_sweep(verbose):
    """PHASE 2: every TRIAGE finding is triaged; the INFO population has not grown."""
    tool = os.path.join(ROOT, "tools", "sweep_bug_shapes.py")
    # The enumerator's own SELFTEST pins the known instances of each shape. Run
    # it first: a parser regression that stopped finding them would otherwise
    # read here as "nothing new was added".
    self_proc = subprocess.run([sys.executable, tool, "--selftest"],
                               cwd=ROOT, capture_output=True, text=True)
    if self_proc.returncode != 0:
        print("  FAIL: tools/sweep_bug_shapes.py --selftest exited %d"
              % self_proc.returncode)
        for line in (self_proc.stdout + self_proc.stderr).strip().split("\n"):
            if line:
                print("    %s" % line)
        return False
    proc = subprocess.run([sys.executable, tool], cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        print("  FAIL: tools/sweep_bug_shapes.py exited %d" % proc.returncode)
        return False
    if proc.stderr.strip():
        # PARSE SHORTFALL means the enumerator lost track of function boundaries
        # in some file, so "no findings there" would be a lie. Fail closed.
        for line in proc.stderr.strip().split("\n"):
            print("  FAIL: %s" % line)
        return False

    triage, info, ok = [], {}, True
    for line in proc.stdout.strip().split("\n"):
        if not line:
            continue
        cls, rel, ln, key, sev, txt = line.split("|", 5)
        if sev == "TRIAGE":
            triage.append((cls, rel, int(ln), key, txt))
        else:
            info[cls] = info.get(cls, 0) + 1

    if not triage:
        print("  FAIL: the static sweep produced no TRIAGE findings at all. The "
              "triage list below is")
        print("        not empty, so this means the enumerator stopped measuring.")
        return False

    new = [t for t in triage if (t[0], t[1], t[3]) not in SHAPE_TRIAGE]
    if new:
        ok = False
        print("  FAIL: %d static finding(s) with no triage entry:" % len(new))
        for cls, rel, ln, key, txt in new:
            print("    [%s] %s:%d  %s" % (cls, rel, ln, key))
            print("        %s" % txt)
        print()
        print("        Each needs a reason in SHAPE_TRIAGE. The question is not "
              "'does it crash' --")
        print("        none of these three shapes crashes on the host. It is: what "
              "sets the count,")
        print("        what is the capacity, and can the counter pass the test "
              "without equalling it?")

    for cls, ceiling in sorted(SHAPE_INFO_MAX.items()):
        n = info.get(cls, 0)
        if n > ceiling:
            ok = False
            print("  FAIL: %s has %d informational finding(s), ceiling %d. Something "
                  "new was added" % (cls, n, ceiling))
            print("        in this class. Run tools/sweep_bug_shapes.py %s and look "
                  "at what is new." % cls)
        elif verbose:
            print("  %-14s %d triaged, %d informational (ceiling %d)"
                  % (cls, sum(1 for t in triage if t[0] == cls), n, ceiling))

    stale = [k for k in SHAPE_TRIAGE
             if not any((t[0], t[1], t[3]) == k for t in triage)]
    if stale:
        ok = False
        print("  FAIL: %d triage entr(ies) match nothing any more -- the code moved "
              "out from under" % len(stale))
        print("        them, so their reasoning is unverified. Re-triage or delete:")
        for k in sorted(stale):
            print("    %s %s %s" % k)
    return ok


# (name, env, frames, script, predicate(out) -> None or "why it failed").
# PHASE 3: the bounds this commit added are UNREACHED in normal play, so a check
# that only ran normal play would prove nothing about them. Each control forces
# the boundary from the same binary and requires the two arms to differ.
def _seg_probe(out):
    m = re.search(r"\[SEGS\] xzMax=(\d+)/(\d+) slack=(-?\d+) clamped=(\d+)", out)
    return (int(m.group(1)), int(m.group(2)), int(m.group(4))) if m else None


# label -> the set of capacities that site's callers are allowed to pass. These
# are the ARRAY_COUNTs at the call sites, and the probe reports the smallest one
# actually handed to the callee at run time -- so a call site that loses its
# ARRAY_COUNT, or passes the wrong array's, shows up as a number outside the set
# instead of as silence. `None` means "any positive value": func_800BDC80's second
# output is written at a RUNNING offset into D_8011C8B8[128], so its capacity is
# legitimately different on every call.
EXPECTED_BOUNDS = [("xz", {8}), ("xyz", {28}), ("colY", {16, 30}),
                   ("shTri", None), ("shHgt", {300})]


def _bounds_seen(out):
    """label -> (smallest capacity seen, smallest per-call slack seen)."""
    seen = {}
    for label, _want in EXPECTED_BOUNDS:
        m = re.search(r"%sMax=\d+/(\d+) slack=(-?\d+)" % label, out)
        if m:
            seen[label] = (int(m.group(1)), int(m.group(2)))
    return seen


def _coll_probe(out):
    m = re.search(r"\[COLL\] maxCandidates=(\d+) truncated=(\d+) cap=(\d+)", out)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None


def controls(binary, rom, logdir, verbose):
    """Force each boundary and require the guarded arm to hold it."""
    ok = True

    def run(name, env_extra, frames, script):
        env = dict(os.environ)
        env["MDKR_AUDIO"] = "0"
        env["MDKR_PRESENT_RATE"] = "original"
        env["MDKR_SIMULATION_CADENCE"] = "original"
        env["MDKR_SYNTH_FIELDS"] = "2"
        env.update(env_extra)
        cmd = [binary, "--headless-frames", str(frames), "--rom", rom]
        if script:
            cmd += ["--input-script", os.path.join("tests/input_scripts", script)]
        try:
            p = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=900)
            return p.returncode, p.stdout.decode("utf-8", "replace")
        except subprocess.TimeoutExpired as e:
            return "timeout", (e.stdout or b"").decode("utf-8", "replace")

    # --- control 1: the segment list, forced to overflow -------------------
    # MDKR_SEGMARGIN widens the 4-unit acceptance margin so every segment
    # matches the query point, which is the REAL mechanism (many overlapping
    # segments) rather than a faked index. Bounded: writes stop at 8 and the
    # caller's own `>= 8` test returns 0 waves, so the run completes. Legacy:
    # one s32 per segment goes into segmentsInside[8] until the frame is gone.
    # Track 19 is a wave track: measured xzMax 2 there, i.e. the callee is
    # really entered. nav_to_track_select measures xzMax=0 and was silently
    # proving nothing -- the control's own reachability assertion caught it.
    frames, script = 3000, "race_full_3lap_tt.txt"
    seg_env = {"MDKR_SEGMARGIN": "100000", "MDKR_LOAD_TRACK": "19",
               "MDKR_AUTOPILOT": "1"}
    rc_b, out_b = run("segbound-fixed", seg_env, frames, script)
    probe_b = _seg_probe(out_b)
    if rc_b != 0 or probe_b is None:
        print("  FAIL: control segbound-fixed exited %s with %s probe"
              % (rc_b, "no" if probe_b is None else "a"))
        ok = False
    elif probe_b[2] == 0:
        print("  FAIL: control segbound-fixed never reached the bound (xzClamped=0) "
              "-- MDKR_SEGMARGIN")
        print("        did not force the overflow, so the arms below prove nothing.")
        ok = False
    elif verbose:
        print("  control segbound-fixed   exit=0 xzMax=%d/%d clamped=%d"
              % probe_b)

    rc_l, out_l = run("segbound-legacy", dict(seg_env, MDKR_SEGBOUND="legacy"),
                      frames, script)
    if rc_l == 0:
        print("  FAIL: control segbound-legacy exited 0. With the bound removed and "
              "the margin forced,")
        print("        get_inside_segment_count_xz() writes one s32 per segment into "
              "an 8-element stack")
        print("        array -- it MUST fail. A clean exit means the control is not "
              "reproducing, so the")
        print("        bound is untested.")
        ok = False
    elif verbose:
        print("  control segbound-legacy  exit=%s (expected non-zero)" % rc_l)

    # --- control 1b: every bound really is the caller's own capacity -------
    # A full race route reaches all five sites. Run it unforced so the bounds are
    # the real ones.
    # Two routes, because no single one reaches all five sites: a water track
    # exercises the wave-shadow writer and collision_get_y's biggest caller, a
    # boss track exercises the rest. Requiring the UNION to cover them is what
    # stops a site quietly going unmeasured.
    merged = {}
    for nm, env_r, fr, sc in (("bounds-water", {"MDKR_LOAD_TRACK": "19"}, 6500,
                               "race_full_3lap_tt.txt"),
                              ("bounds-boss", {"MDKR_LOAD_TRACK": "40"}, 6500,
                               "race_full_3lap.txt")):
        rc_p, out_p = run(nm, dict(env_r, MDKR_AUTOPILOT="1"), fr, sc)
        if rc_p != 0:
            print("  FAIL: control %s exited %s" % (nm, rc_p))
            ok = False
        for label, (cap, slack) in _bounds_seen(out_p).items():
            if cap <= 0:
                continue
            prev = merged.get(label)
            merged[label] = (cap, slack) if prev is None else \
                (min(prev[0], cap), min(prev[1], slack))
    for label, want in EXPECTED_BOUNDS:
        got = merged.get(label)
        if got is None:
            print("  FAIL: %s was not reached by either control route, so its bound "
                  "is unmeasured" % label)
            print("        and the guard on it is untested. Pick a route that "
                  "reaches it.")
            ok = False
        elif want is not None and got[0] not in want:
            print("  FAIL: %s was handed a capacity of %d; the call sites pass %s. "
                  "A caller is passing" % (label, got[0], sorted(want)))
            print("        the wrong array's size -- the write is bounded, but not "
                  "to the array it writes into.")
            ok = False
        elif got[1] <= 0:
            print("  FAIL: %s came within %d of its array end in NORMAL play. That "
                  "is no longer latent." % (label, got[1]))
            ok = False
        elif verbose:
            print("  control bounds  %-6s capacity=%-4d min slack=%d"
                  % (label, got[0], got[1]))

    # --- control 2: the candidate cap, forced to the boundary --------------
    # Lower the cap far below the measured 416 peak so the boundary is crossed
    # thousands of times. maxCandidates is the write INDEX high-water mark, so
    # maxCandidates > cap means the cap was stepped over.
    env_cap = {"MDKR_LOAD_TRACK": "41", "MDKR_AUTOPILOT": "1"}
    rc_b, out_b = run("collcap-fixed", dict(env_cap, MDKR_COLLCAP="32"), 3000,
                      "race_full_3lap.txt")
    pb = _coll_probe(out_b)
    if rc_b != 0 or pb is None:
        print("  FAIL: control collcap-fixed exited %s with %s [COLL] probe"
              % (rc_b, "no" if pb is None else "a"))
        ok = False
    elif pb[1] == 0:
        print("  FAIL: control collcap-fixed never saturated (truncated=0) at cap=32, "
              "so neither arm")
        print("        reaches the boundary the guard exists for.")
        ok = False
    elif pb[0] > pb[2]:
        print("  FAIL: control collcap-fixed wrote index %d past its cap of %d -- "
              "the guard did NOT hold." % (pb[0], pb[2]))
        ok = False
    elif verbose:
        print("  control collcap-fixed    exit=0 maxCandidates=%d cap=%d truncated=%d"
              % (pb[0], pb[2], pb[1]))

    rc_l, out_l = run("collcap-legacy", dict(env_cap, MDKR_COLLCAP="legacy"), 3000,
                      "race_full_3lap.txt")
    pl = _coll_probe(out_l)
    if pl is None:
        print("  FAIL: control collcap-legacy produced no [COLL] probe")
        ok = False
    elif pl[0] <= 32:
        print("  FAIL: control collcap-legacy peaked at %d, which is within the 32 "
              "the fixed arm held." % pl[0])
        print("        The two arms are then indistinguishable and the guard is "
              "untested.")
        ok = False
    elif verbose:
        print("  control collcap-legacy   maxCandidates=%d (unguarded, vs 32 held)"
              % pl[0])
    return ok


def main():
    # build-ubsan is shared state this check reconfigures and rebuilds. Two
    # concurrent runs would interleave a reconfigure with a compile and fail for
    # reasons unrelated to the code under test.
    try:
        with exclusive_build_dir(os.path.join(ROOT, BUILD_DIR)):
            return run_check()
    except RuntimeError as exc:
        print("check_array_bounds_sweep: %s" % exc)
        return 2


def run_check():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--no-build", action="store_true",
                    help="reuse %s as it stands" % BUILD_DIR)
    ap.add_argument("--no-controls", action="store_true",
                    help="skip phase 3 (the boundary controls). For iterating "
                         "only -- the bounds are unreached in normal play, so a "
                         "run without them does not test them.")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    print("check_array_bounds_sweep: UBSan -fsanitize=%s over %d routes"
          % (SANITIZERS, len(ROUTES)))
    if not os.path.exists(os.path.join(ROOT, args.rom)):
        sys.exit("check_array_bounds_sweep: no ROM at %s" % args.rom)

    binary = os.path.join(ROOT, BUILD_DIR, "mdkr64")
    if not args.no_build:
        if not build(args.verbose):
            print("check_array_bounds_sweep: FAIL")
            return 1
    if not os.path.exists(binary):
        print("  FAIL: %s does not exist (drop --no-build)" % binary)
        print("check_array_bounds_sweep: FAIL")
        return 1
    missing = instrumented(binary)
    if missing:
        for sym, flag in missing:
            print("  FAIL: %s does not import %s, so -fsanitize=%s is NOT active."
                  % (binary, sym, flag))
        print("        Nothing this check reports would mean anything. Failing "
              "rather than passing vacuously.")
        print("check_array_bounds_sweep: FAIL")
        return 1
    if not sanitizer_positive_controls(args.verbose):
        print("check_array_bounds_sweep: FAIL")
        return 1

    logdir = tempfile.mkdtemp(prefix="mdkr_ubsweep_")
    try:
        found, ok = collect(binary, args.rom, logdir, args.verbose)
    finally:
        shutil.rmtree(logdir, ignore_errors=True)

    seen_kinds = {(f, k) for (f, k, _s) in found}
    for sentinel in REQUIRED_SENTINELS:
        if sentinel not in seen_kinds:
            print("  FAIL: sentinel %s:%s was not reported. It is an allow-listed "
                  "idiom on a path" % sentinel)
            print("        every route crosses, so its absence means the instrument "
                  "is dead, not that")
            print("        the code is clean. Failing closed.")
            ok = False

    new = sorted(k for k in found if k not in ALLOWED)
    if new:
        ok = False
        print("  FAIL: %d out-of-bounds index site(s) not in the allow-list:" % len(new))
        for key in new:
            line, text, route = found[key]
            print("    %s:%d  (%s)   first seen on route: %s"
                  % (key[0], line, key[1], route))
            print("        %s" % text.split(": runtime error: ")[-1])
            print("        source: %s" % (key[2] or "<could not read>"))
        print()
        print("        Triage each one before allow-listing it. The question that "
              "matters is NOT")
        print("        'does it crash here' — the wave-table defect never crashed "
              "natively. It is:")
        print("          1. does the index leave its own array?")
        print("          2. if so, what object does it land on -- on LP64 AND on "
              "wasm32?")
        print("             (tools/compare_data_layout.py answers that)")
        print("          3. does anything depend on it landing there? "
              "(gFFLUnlocked did)")
        print("        If the adjacency is load-bearing, back both names onto ONE "
              "object with")
        print("        _Static_asserts, as game/src/waves.c and game/src/menu.c do.")

    if args.verbose:
        print("  allow-listed sites reported this run:")
        for key in sorted(k for k in found if k in ALLOWED):
            print("    %-24s %-16s line %d" % (key[0], key[1], found[key][0]))

    # PHASE 2 -- static enumeration of the three shapes no runtime tool can see.
    print("  phase 2: static class sweep (tools/sweep_bug_shapes.py)")
    if not shape_sweep(args.verbose):
        ok = False

    # PHASE 3 -- force the boundaries the added bounds exist for. Skipped only
    # with --no-controls, which is for iterating on phases 1-2, never for a
    # suite run.
    if args.no_controls:
        print("  phase 3: SKIPPED (--no-controls). The bounds are UNREACHED in "
              "normal play, so")
        print("           phases 1-2 alone do not test them.")
    else:
        print("  phase 3: boundary controls (each forces a bound that normal play "
              "never reaches)")
        logdir2 = tempfile.mkdtemp(prefix="mdkr_ubctl_")
        try:
            if not controls(binary, args.rom, logdir2, args.verbose):
                ok = False
        finally:
            shutil.rmtree(logdir2, ignore_errors=True)

    print("check_array_bounds_sweep: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
