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
    # -- The online/session campaign's wire encoders (2026-08-14, first
    #    full-suite traversal; task 187 had never run anywhere).
    ("bare-pointer", "platform/net/match_manifest.c", "put64:out"):
        "BOUNDED BY THE ENCODER'S GATE: fixed eight-byte big-endian store; "
        "both call sites are literal offsets inside "
        "mdkr_match_manifest_encode (64+slot*8 with slot < 4, and 96), "
        "whose entry check refuses capacity < MDKR_MATCH_MANIFEST_BYTES "
        "(112). Highest byte touched is 103.",
    ("bare-pointer", "platform/net/match_preflight.c", "put64:output"):
        "BOUNDED BY LOCALS: one call writes encoded[8] (exactly the store's "
        "width), the other next[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES] at "
        "+20..27 with the later memcpys reaching +92+32 and the function "
        "gated on capacity < sizeof(next).",
    ("bare-pointer", "platform/pacing_policy.c",
     "present_interval_sort:values"):
        "BOUNDED BY CONSTRUCTION: insertion sort; every write index is "
        "position <= i < count. The sole caller sorts a local "
        "ordered[MDKR_PRESENT_INTERVAL_WINDOW] filled by memcpy of "
        "sizeof(ordered) and passes that same constant as count.",
    ("bare-pointer", "platform/party/party_protocol.c",
     "mdkr_party_pad_encode:output"):
        "BOUNDED BY THE ENCODER'S GATE: entry check refuses capacity < "
        "mdkr_party_pad_encoded_size(edge_count) and validate_packet bounds "
        "edge_count; every write offset stays below that encoded size by "
        "construction of the same arithmetic.",
    ("bare-pointer", "game/src/camera.c",
     "mdkr_camera_interpolated_view_projections:out"):
        "BOUNDED BY CONSTRUCTION: the loop is `for (viewport = 0; viewport < "
        "capacity; viewport++)` and every write is out[viewport] — the bound "
        "parameter IS the loop bound, so no count can exceed it. The sole "
        "caller (stubs_dkr.c presentation subloop) passes ARRAY_COUNT of a "
        "local GfxShadowReplayViewProjection views[4], matching "
        "GFX_SHADOW_MAX_VIEWS. NULL out returns 0.",
    ("bare-pointer", "game/src/video_mode_table.c",
     "mdkr_video_apply_pal_height_raise:modes"):
        "BOUNDED BY THE CALLER'S OWN LAST INDEX: the loop is `for (i = 0; i <= "
        "lastIndex; i++)` writing modes[i], and the sole game caller (video.c "
        "video_init) passes NUM_RESOLUTION_MODES == sizeof(gVideoModeResolutions)"
        "/sizeof(VideoModeResolution) - 1 -- the array's own last valid index, "
        "derived from the array itself -- so modes[lastIndex] is the final of the "
        "8 elements, never past it. Byte-identical to the retail `for (i = 0; i "
        "<= NUM_RESOLUTION_MODES; i++)` PAL height loop it replaced; the only "
        "added behaviour is the *raised once-flag that makes the raise idempotent "
        "across in-process launcher epochs. NULL modes returns early.",
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
        "BOUNDED BY THE CALLERS, not by the parameter: stringLength bounds the "
        "loop but the function also writes a terminator at outString[stringLength], "
        "so a destination needs the field width PLUS one. Both callers supply it -- "
        "get_controller_pak_file_list() carves 0x12/6-byte slices, and "
        "copy_controller_pak_data()'s stack buffers are PFS_FILE_NAME_LEN + 1 / "
        "PFS_FILE_EXT_LEN + 1 under NATIVE_PORT.",
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
    # -- the online-screen all-caps BIGFONT copy helpers. Present since the
    #    online room-preview bring-up (both exist at the crossplay campaign base
    #    b992e538; the sweep was never run there because array_bounds is a
    #    rom-role/full-suite check, so these two sat untriaged until the land5
    #    first-ever full-suite run surfaced them). Read end to end 2026-09-01;
    #    each is self-bounded by its cap parameter, not by caller discipline.
    ("bare-pointer", "game/src/online/online_trackselect.c",
     "trackselect_upper:dst"):
        "BOUNDED BY PARAMETER: the copy loop condition is "
        "`src[i] != '\\0' && i + 1u < cap`, so body writes dst[i] stop at "
        "i <= cap-2 and the trailing `dst[i] = '\\0'` lands at cap-1 at worst. "
        "Self-bounded against `cap` regardless of caller; all three call sites "
        "pass sizeof() of a local char buffer (nm[]) with a level_name() source. "
        "NULL src is replaced with \"?\" before the loop.",
    ("bare-pointer", "game/src/online/online_vehicleselect.c",
     "vehicleselect_upper:dst"):
        "BOUNDED BY PARAMETER: byte-identical idiom to trackselect_upper above -- "
        "`src[i] != '\\0' && i + 1u < cap` caps body writes at cap-2 and the "
        "terminator at cap-1. The sole caller passes sizeof(nameBuf) with a "
        "level_name() source. NULL src is replaced with \"?\".",
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
    # -- The Adventure Party co-op and Character Workshop / custom-character
    #    campaigns (2026-09-03, int-1.7.0 line, first sweep since v1.6.0
    #    merged both). Eleven output pointers arrived at once; every one was
    #    read end to end, and each is bounded by something the callee can see.
    ("bare-pointer", "game/src/objects.c",
     "adventure_party_hub_formation:spawnX"):
        "BOUNDED BY THE PLANNER'S CONTRACT: the only writes are "
        "spawn*[seat] with seat = plan.seats[j].seat, and "
        "adventure_party_plan_formation fills 1..participant_count-1 over "
        "seat_count == participant_count-1 slots "
        "(platform/adventure_party/adventure_party_spawn.h), with "
        "participant_count refused outside 2..4 by the check immediately "
        "above the plan call. The sole caller's arrays are s32 spawnX[8] "
        "(objects.c:3889), so the highest index written is 3 of 8.",
    ("bare-pointer", "game/src/objects.c",
     "adventure_party_hub_formation:spawnY"):
        "BOUNDED BY THE PLANNER'S CONTRACT: same loop, same seat index, same "
        "8-slot caller array.",
    ("bare-pointer", "game/src/objects.c",
     "adventure_party_hub_formation:spawnZ"):
        "BOUNDED BY THE PLANNER'S CONTRACT: same loop, same seat index, same "
        "8-slot caller array.",
    ("bare-pointer", "game/src/objects.c",
     "adventure_party_hub_formation:spawnAngle"):
        "BOUNDED BY THE PLANNER'S CONTRACT: same loop, same seat index, same "
        "8-slot caller array.",
    ("bare-pointer", "game/src/menu.c",
     "charselect_custom_fit_text:output"):
        "BOUNDED BY PARAMETER: outputSize. Every write is inside it -- "
        "output[0] after the outputSize == 0 refusal; the shortening loop "
        "only ever writes output[--used] with used starting at "
        "projection.output_bytes, which "
        "mdkr_modern_character_text_project caps at outputSize - 1; and the "
        "ellipsis memcpy of 4 bytes is fenced by `used + 3u < outputSize`.",
    ("bare-pointer", "platform/modern_character_text.c",
     "mdkr_modern_character_text_project:output"):
        "BOUNDED BY PARAMETER: output_capacity. Every byte goes through "
        "emit(), whose sole write is guarded by `*used + 1u < "
        "output_capacity` and which otherwise only sets the truncated flag, "
        "so used <= output_capacity - 1 and the closing output[used] = 0 is "
        "the last in-range byte. NULL output or zero capacity returns 0.",
    ("bare-pointer", "platform/fast3d/gfx_character_text.c",
     "assign_context_faces:faces"):
        "BOUNDED BY PARAMETER: writes faces[index] for index < count in each "
        "of its three passes, and count IS the parameter. Both call sites "
        "(build_layout) pass a local uint8_t "
        "faces[GFX_CHARACTER_TEXT_MAX_CODEPOINTS + 1u] with a count that the "
        "codepoint decoder already fenced at "
        "`count >= GFX_CHARACTER_TEXT_MAX_CODEPOINTS` -> fallback.",
    ("bare-pointer", "platform/fast3d/gfx_character_text.c",
     "assign_context_scripts:scripts"):
        "BOUNDED BY PARAMETER: same shape, same two call sites, same "
        "capacity -- hb_script_t scripts[GFX_CHARACTER_TEXT_MAX_CODEPOINTS + "
        "1u] under the same codepoint cap.",
    ("bare-pointer", "platform/modern_character_gameplay_profile.c",
     "read_float_array:output"):
        "BOUNDED BY PARAMETER: `for (element = 0; element < count; "
        "element++) output[element] = ...`, and the function additionally "
        "refuses when the ROM subasset holds fewer than count * 4 bytes, so "
        "it cannot read past its source either. All three call sites pass an "
        "array and its own element count "
        "(MDKR_DONOR_GAMEPLAY_PROFILE_COUNT, MDKR_DONOR_ACCELERATION_SAMPLES).",
    ("bare-pointer", "platform/modern_character_install.c",
     "json_escape:output"):
        "BOUNDED BY PARAMETER: capacity. Each of the three emit arms is "
        "preceded by its own `write + N >= capacity` refusal for exactly the "
        "N bytes it is about to store (2 for the escaped pair, 6 for the "
        "\\u00xx form, 1 for a literal byte), which leaves at least one "
        "byte for the closing output[write] = 0.",
    ("bare-pointer", "platform/modern_character_render.c",
     "mdkr_modern_render_resolve_draw:palette_scratch"):
        "BOUNDED BY PARAMETER, in MATRICES not floats: the entry check "
        "refuses `palette_matrices < retained->bone_count` whenever "
        "bone_count is nonzero, and the loop writes "
        "palette_scratch[component] for component < bone_count * 16, i.e. 16 "
        "floats per matrix. The game caller (gfx_pc_dkr.c) passes a local "
        "float interpolated_bones[MDKR_MODERN_DRAW_STORE_MAX_BONES * 16u] "
        "with palette_matrices = MDKR_MODERN_DRAW_STORE_MAX_BONES, so the "
        "two units agree; the draw store separately refuses any draw whose "
        "bone_count exceeds that same maximum.",

    # ---------------------------------------------------------------------
    # equality-cap: `i == CAP` guarding a write indexed by i, where a second
    # increment can advance i past CAP.
    # ---------------------------------------------------------------------
    # -- The rollback span builders (2026-08-14, first full-suite traversal).
    #    Both enumerate file-scope state into a caller-sized
    #    spans[SPAN_COUNT] through an unchecked spans[count++] macro and
    #    return `count == SPAN_COUNT` at the end. The equality is a
    #    POST-verification, not a bound: an ADD line added without bumping
    #    the constant overflows the array first and fails the check second.
    #    What keeps that survivable today: the constant and the ADD list
    #    live in the same file, both counts were hand-verified equal at
    #    triage time (34/34 and 8/8), and the rollback span gates execute
    #    the builders every run, so a mismatch cannot land silently. The
    #    honest fix -- a capacity parameter checked per write -- is noted
    #    for the next edit of either file.
    ("equality-cap", "game/src/fade_transition.c",
     "transition_rollback_state_spans:count==MDKR_TRANSITION_ROLLBACK_STATE_SPAN_COUNT"):
        "POST-VERIFIED, NOT BOUNDED: unchecked spans[count++] enumeration "
        "over spans[34]; the trailing equality converts a mismatched ADD "
        "list into a gate failure after the writes. 34 ADD lines == the "
        "constant, verified 2026-08-14.",
    ("equality-cap", "game/src/joypad.c",
     "input_rollback_state_spans:count==MDKR_INPUT_ROLLBACK_STATE_SPAN_COUNT"):
        "POST-VERIFIED, NOT BOUNDED: same builder shape over spans[8]; 8 ADD "
        "lines == the constant, verified 2026-08-14.",
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

    # =====================================================================
    # platform/ -- NEW to the enumerator, not new to the tree.
    # tools/sweep_bug_shapes.py scanned only game/src, so the port's own C had
    # never been swept for any of these three shapes. That scope gap is what let
    # mdkr_adventure.c keep an unbounded `0x10000 << doorID` next to a
    # correctly-bounded twin in the same file. SCAN_DIRS now covers platform/;
    # every TRIAGE finding it produced is below, each read end to end with its
    # call sites.
    # =====================================================================
    ("bare-pointer", "platform/audi_port_dkr.c", "audio_apply_test_gain:buf"):
        "BOUNDED BY CALLER CONTRACT: the loop is `i < frameSamples * "
        "DKR_AUDIO_CHANNELS`. Both call sites take frameSamples from "
        "dkr_choose_frame_samples()/dkr_catchup_frame_samples(), which clamp to "
        "amAudioGetMaxSamples() == PORT_MAX_FRAME_SAMPLES before the buffer is "
        "used, and amAudioSynthFrame allocates each output at "
        "PORT_MAX_FRAME_SAMPLES * DKR_AUDIO_CHANNELS s16. The clamp lives in the "
        "shared controller both callers route through.",
    ("bare-pointer", "platform/audio_ring.c", "mdkr_audio_ring_pull:interleaved"):
        "BOUNDED BY PARAMETER: `for (i = 0; i < frames; i++)` writing "
        "interleaved[i*2] / [i*2+1] -- the bound parameter IS the loop bound. "
        "The SDL callback computes frames = len / DKR_AUDIO_BYTES_PER_FRAME from "
        "the size of the very buffer it passes.",
    ("bare-pointer", "platform/audio_volume.c",
     "mdkr_audio_gain_ramp_apply_s16:samples"):
        "BOUNDED BY PARAMETER: nested `frame < frames` / `channel < channels`, "
        "max index frames*channels-1. Both call sites pass the same buf / "
        "frameSamples pair already bounded above, with channels == "
        "DKR_AUDIO_CHANNELS matching the buffer's interleaving.",
    ("bare-pointer", "platform/audio_volume.c",
     "mdkr_audio_crossfade_from_s16:samples"):
        "BOUNDED BY PARAMETER: count = min(frames, fade_frames), so writes never "
        "exceed frames*channels-1 whatever fade_frames is. The sole caller "
        "forwards frameCount = size / DKR_AUDIO_BYTES_PER_FRAME for the buffer of "
        "that same size. `from` is a fixed int16_t[2] indexed by channel < 2.",
    ("bare-pointer", "platform/config_ini.c", "config_ini_trim:s"):
        "NOT THIS SHAPE: nothing is written past the input's own NUL. Every bound "
        "comes from strlen() of the string itself, and both the memmove and the "
        "trailing-whitespace NULs stay inside live content. It needs only a valid "
        "C string, which all four call sites (a local line[] buffer) provide.",
    ("bare-pointer", "platform/fast3d/gfx_font_sdf.c", "derive_region:distance"):
        "BOUNDED BY CONSTRUCTION: writes distance[y*width+x] for y<height, "
        "x<width of the CLIPPED region. The sole caller runs a pre-pass over "
        "every region with the same clip_region(), takes the max "
        "width*height, and allocates `distance` at exactly that before the real "
        "pass -- so every later call's clipped area is <= the allocation.",
    ("bare-pointer", "platform/fast3d/gfx_mipgen.c", "preserve_alpha_coverage:rgba"):
        "BOUNDED BY CONSTRUCTION: `i < width*height` writing rgba[i*4+3]. The "
        "sole caller (gfx_mip_build_cutout) passes out->level[l] with "
        "out->width[l]/height[l], and that level was allocated at exactly "
        "cw*ch*4 with width/height set from the same locals.",
    ("bare-pointer", "platform/fast3d/gfx_pc_dkr.c", "dkr_clip_near:out"):
        "BOUNDED BY LITERAL, and now also self-bounded. The clipping path guards "
        "every write with `count < DKR_CLIP_MAX_VERTS`; the MDKR_NEARCLIP=off "
        "debug arm copied `n` elements with no clamp of its own and was safe only "
        "because the one call site passes the literal 3 into a "
        "LoadedVertex[DKR_CLIP_MAX_VERTS]. That arm now clamps n to "
        "DKR_CLIP_MAX_VERTS as well, so both arms carry the array's bound.",
    ("bare-pointer", "platform/fast3d/gfx_shadow_frame.c",
     "append_triangle:vertex_count"):
        "BOUNDED BY CONSTRUCTION: required_vertices = *vertex_count + 3 (through "
        "checked_add_size, which rejects overflow), then grow_array() must "
        "succeed to at least that capacity or the append is abandoned, BEFORE the "
        "three writes; *vertex_count is advanced only after they complete. Count "
        "and capacity are one invariant maintained by this function.",
    ("bare-pointer", "platform/fs_utf8.c",
     "mdkr_windows_quote_argument_utf8:output"):
        "BOUNDED BY PARAMETER: every byte goes through MDKR_QUOTE_EMIT, which "
        "tests `written + 1 >= capacity` and returns -1 instead of writing. "
        "Self-enforced against its own capacity argument.",
    ("bare-pointer", "platform/presentation_snapshot.c",
     "presentation_snapshot_authored_cameras_copy:out"):
        "BOUNDED BY PARAMETER: `if (count >= capacity) return 0;` immediately "
        "precedes every out[count] write, so no caller can push it past capacity. "
        "The real caller and the tests pass ARRAY_COUNT/sizeof of a real array.",
    ("bare-pointer", "platform/rom_id.c", "dkr_rom_supported_list:buf"):
        "BOUNDED BY PARAMETER: all writes are snprintf(buf + used, bufLen - used, "
        "...) with the return value checked before `used` advances, plus an "
        "explicit buf[bufLen-1] = 0. Both call sites pass sizeof(buffer).",
    ("bare-pointer", "platform/rom_id.c", "dkr_rom_normalize_byte_order:data"):
        "BOUNDED BY PARAMETER: both swap loops are `i + 1 < size` / `i + 3 < "
        "size`, so the highest index is size-1. The sole caller only reaches it "
        "after an exact size == DKR_ROM_SIZE_BYTES check on the real buffer.",
    # --- content packs, GPU diagnostics and adapter policy -----------------
    # Four writes through a caller-supplied buffer, all four self-bounded by the
    # size parameter travelling with it rather than by caller discipline. The
    # question this class asks is "can the counter pass the test without
    # equalling it"; for each of these the answer is recorded below.
    ("bare-pointer", "platform/mod_registry.c", "path_join:out"):
        "BOUNDED BY PARAMETER, AND REFUSES RATHER THAN TRUNCATES: "
        "`base_length + 1 + leaf_length + 1 > out_size` returns 0 before any "
        "write, so the three writes that follow (memcpy base, the '/' at "
        "out[base_length], memcpy leaf and the NUL) each land inside a span "
        "already proven to fit. Truncation is deliberately not a fallback -- a "
        "truncated path names a DIFFERENT file, and probing a different file is "
        "worse than not finding one. The counter cannot pass the test without "
        "equalling it because the test is an inequality on the total, evaluated "
        "once, before the first byte. Overflow of that sum needs two strings "
        "summing past SIZE_MAX; both come from a bounded MDKR_MOD_PATH_MAX "
        "buffer or a listed directory entry.",
    # -- 1.8 pack-texture work: four writes through a caller's buffer, each
    #    read with every call site. None is a defect; the reasons differ.
    ("bare-pointer", "platform/mod_source.c", "path_join:out"):
        "BOUNDED BY PARAMETER, byte-identical body to mod_registry.c's "
        "path_join above: `base_length + 1 + leaf_length + 1 > out_size` "
        "returns 0 before the first write. Back since 8e123446 (the "
        "MDKR_PACK_BY_PATH pathname arm Windows compiles; the sweep sees both "
        "arms of the non-decidable #if). All three callers pass sizeof of a "
        "local char[MDKR_MOD_PATH_MAX]: directory_open_file's full and "
        "directory, directory_enumerate's here.",
    ("bare-pointer", "platform/mod_registry.c", "digest_name_fold:folded"):
        "BOUNDED BY LITERAL: the function returns 0 unless strlen(rel) == "
        "MDKR_MOD_DIGEST_NAME_CHARS (45), then writes folded[0..44] and the "
        "NUL at folded[45]. Both non-NULL callers (name_index_add, "
        "name_slots_for, through name_key) pass a local "
        "char folded[MDKR_MOD_DIGEST_NAME_CHARS + 1]; is_digest_texture_name "
        "passes NULL, which skips every write.",
    ("bare-pointer", "platform/mod_texture_store.c", "slot_candidates:out"):
        "BOUNDED BY PARAMETER: the loop condition `pack >= 0 && count < max` "
        "is tested before every out[count] write and count has one increment, "
        "after the write, so count <= max. The loop also runs at most "
        "MDKR_MOD_MAX_PACKS (64) times. All four callers (slot_begin) pass "
        "candidates[k], a TexCandidate[MDKR_MOD_MAX_PACKS] row, with max = "
        "MDKR_MOD_MAX_PACKS.",
    ("bare-pointer", "platform/custom_track_model.c", "add_region:regions"):
        "BOUNDED BY CONSTANT, PRE-CHECKED: `*count >= MODEL_REGION_CAPACITY` "
        "returns 0 before the single regions[(*count)++] write, so the "
        "highest index written is MODEL_REGION_CAPACITY - 1. The sole caller, "
        "mdkr_track_model_validate_regions, passes a local "
        "ModelRegion regions[MODEL_REGION_CAPACITY]: one enum constant sizes "
        "the array and the guard, so they cannot drift apart. It is "
        "5 + 5 * MODEL_SEGMENT_MAX (645), the greatest demand: 5 root regions "
        "+ 5 per segment with segments <= MODEL_SEGMENT_MAX (validate_spans, "
        "which runs first through validate_visibility/validate_indices).",
    ("bare-pointer", "platform/gpu_diagnostics.c", "copy_field:destination"):
        "BOUNDED BY PARAMETER: the copy loop's own condition is "
        "`index + 1u < size`, so index stops at size-2 and the unconditional "
        "`destination[index] = '\\0'` after it lands at size-1 at worst. It is "
        "bounded by the DESTINATION size alone and never reads the source "
        "further than that, which is the point: an adapter name filled to the "
        "brim by a vendor runtime without a terminator is not walked off the "
        "end. Both sides of every call are arrays of the same declared size.",
    ("bare-pointer", "platform/adapter_policy.c", "reason_append:reason"):
        "BOUNDED BY PARAMETER at all three writes. The separator pair is "
        "guarded by `length + 3u > size` returning early, which reserves room "
        "for ';', ' ' and the NUL together rather than one at a time. The "
        "clause loop's condition is `length + 1u < size`, so length stops at "
        "size-2 and the trailing `reason[length] = '\\0'` lands at size-1 at "
        "worst. `length` itself comes from field_length(reason, size), which "
        "cannot report past size. Written out rather than snprintf'd so no "
        "clause can be lost to a format the compiler cannot bound.",
    ("bare-pointer", "platform/mod_music.c", "resample_into:out"):
        "BOUNDED BY CONSTRUCTION: `out` is the malloc immediately above the "
        "only call site (decode_track), sized target_frames * s_channels * "
        "sizeof(int16_t), and the out_frames argument IS target_frames. Both "
        "loops write out[out_channels * frame + channel] for frame < "
        "out_frames and channel < out_channels, so the highest element written "
        "is out_channels * out_frames - 1: an exact fit with no slack, and "
        "none is needed, because the count and the capacity are the same two "
        "expressions. The product cannot overflow either -- target_frames is "
        "rejected against MDKR_MOD_MUSIC_BYTES_MAX / (sizeof(int16_t) * "
        "s_channels) before the allocation. The sweep flags it only because "
        "`out_frames` does not match its bound-ish parameter-name heuristic; "
        "the bound is present and is the parameter.",
    ("bare-pointer", "platform/mod_music.c", "mdkr_mod_music_mix:out"):
        "BOUNDED BY A CONSTANT SHARED WITH THE CALLER, which is the part worth "
        "checking rather than assuming. The loop writes frame * s_channels + "
        "channel for frame < frames and channel < s_channels, so `frames` "
        "alone does not state the capacity -- `out` needs frames * s_channels "
        "elements. Both call sites are in platform/audi_port_dkr.c and pass "
        "the buffer amAudioSynthFrame(n) returned together with the same n; "
        "that buffer holds n * DKR_AUDIO_CHANNELS int16 samples, which the "
        "next line confirms by handing the identical (buf, n, "
        "DKR_AUDIO_CHANNELS) to mdkr_audio_gain_ramp_apply_s16. So the write "
        "fits exactly when s_channels == DKR_AUDIO_CHANNELS, and s_channels "
        "has exactly one writer: mdkr_mod_music_init, called once, from that "
        "same translation unit, with DKR_AUDIO_CHANNELS. A second initialiser "
        "with a different channel count is the single change that would break "
        "this, which is why it is recorded here rather than left to a reader "
        "to re-derive.",
    ("bare-pointer", "platform/bounded_json.h", "json_parse_string:output"):
        "BOUNDED BY PARAMETER: every write path tests output_capacity first -- "
        "the single-byte path, append_utf8's `count > capacity - *length`, and "
        "the terminating NUL. Self-bounded regardless of caller correctness; all "
        "schema call sites pass their actual field capacity. Every C header is "
        "in the source sweep (tools/sweep_bug_shapes.py, CXX_HEADER).",
    ("bare-pointer", "platform/mod_registry.c", "mdkr_mod_registry_name_hits:out"):
        "BOUNDED BY PARAMETER: max <= 0 returns before any write, hits.count "
        "is clamped to max before the copy loop, and the loop writes "
        "out[0 .. hits.count - 1]. hits.all is MDKR_MOD_MAX_PACKS long and "
        "name_hits_visit stops at that count. The one caller "
        "(mod_texture_store.c slot_candidates) passes "
        "hits[MDKR_MOD_MAX_PACKS] with max = MDKR_MOD_MAX_PACKS. (Added by "
        "889b857f after the 2683fbdf measurement; the gate was red on it.)",
    ("bare-pointer", "platform/custom_track_table.h", "mdkr_custom_track_extend_table:output"):
        "BOUNDED BY PARAMETER: refused unless capacity >= retail_words + count "
        "(after overflow checks on both terms). The writes are the memcpy of "
        "retail_words - 1 words, output[retail_words - 1 + i] for i < count, and "
        "the terminator at retail_words - 1 + count, so the highest index is "
        "capacity - 1 at most. Its one caller (custom_track_section.c) passes "
        "a table malloc'd at exactly that many words. Swept since headers were "
        "brought into scope.",
    ("bare-pointer", "platform/modern_character_sort.h", "mdkr_modern_character_order_primitive_rows:output_rows"):
        "COUNT-SIZED OUTPUT: output_rows receives each index below `count` "
        "exactly once (opaque/masked rows, then blend rows at opaque_count + "
        "transparent_count, the two totalling count), and the insertion sort "
        "only permutes inside that range, so every write is below count. Its "
        "one caller (modern_character_runtime.c) passes assembly_count, which "
        "is refused at MDKR_MODERN_CHARACTER_MAX_PRIMITIVES before each "
        "increment, against ordered_rows[MDKR_MODERN_CHARACTER_MAX_PRIMITIVES]. "
        "Swept since headers were brought into scope.",
    ("bare-pointer", "platform/stubs_dkr.c", "controller_query:data"):
        "BOUNDED BY LITERAL: `i < MAXCONTROLLERS` writing data[i], and the only "
        "backing store is joypad.c's OSContStatus gControllerStatus"
        "[MAXCONTROLLERS] -- loop bound and declaration are the same macro.",
    ("bare-pointer", "platform/stubs_dkr.c", "osContGetReadData:pad"):
        "BOUNDED BY LITERAL: same shape -- `i < MAXCONTROLLERS` writing pad[i], "
        "sole caller passes joypad.c's gControllerCurrData[MAXCONTROLLERS].",
    # -- the ghost-bank window/directory codecs (1.5.2). --------------------
    ("bare-pointer", "platform/ghost_bank.c", "window_stage_records:records"):
        "BOUNDED BY CONSTRUCTION: the loop is `for (i = 0; i < "
        "MDKR_GHOST_WINDOW_SLOTS; i++)` and `count` advances at most once per "
        "iteration (occupied slots only -- empty slots `continue`), so count <= "
        "MDKR_GHOST_WINDOW_SLOTS (6) and the highest records[count] write index "
        "is 5. Both callers (mdkr_ghost_window_remove, mdkr_ghost_window_insert) "
        "pass a local GhostWindowRecord records[MDKR_GHOST_WINDOW_SLOTS]. The "
        "bound-ish size/scratch_capacity params guard the PARALLEL scratch copy "
        "(`used + len > scratch_capacity` returns -1), NOT the records array -- "
        "its bound is the loop's own slot count.",
    ("bare-pointer", "platform/ghost_bank.c", "collect_records:pairs"):
        "BOUNDED BY PARAMETER: the loop condition `count < capacity && index < "
        "names.count` (the collected listing, fs_utf8.h) is a pre-check "
        "evaluated before every pairs[count] write, and count has a single "
        "increment on the write path "
        "-- so it cannot pass `capacity` without equalling it. Both callers "
        "(wipe_library, reconcile's on_disk sweep) pass GHOST_BANK_SWEEP_MAX "
        "(1024) with a matching local GhostBankSweepPair[GHOST_BANK_SWEEP_MAX].",
    ("shift-count", "platform/fast3d/gfx_pc_dkr.c", "dkr_generate_cc:MIPS-MASK-IDIOM"):
        "NOT UB: the enumerator's added-constant heuristic assumes a 32-bit "
        "operand, and `cc_id` is a uint64_t parameter. The loop is `i < 2 && (i "
        "== 0 || is_2cyc)`, so i is 0 or 1 and the shift counts are 25 and 53 -- "
        "both under 64. The narrowing into a uint32_t afterwards is ordinary "
        "truncation, not a shift-exponent problem.",
}

# Named review of new INFO sites; exact occurrence counts prevent an extra
# site under the same function/key from silently spending an old allowance.
# These do not increase the anonymous historical population ceilings.
SHAPE_TRIAGE.update({
    ("equality-cap", "game/src/asset_loading.c", "asset_custom_track_env_selftest:i==MDKR_CUSTOM_TRACK_MODELS"):
        "SECTION DISPATCH, not a saturation cap: each i loop starts at zero, "
        "uses i < MDKR_CUSTOM_TRACK_SECTION_COUNT (5), and increments once per "
        "iteration. Every i-indexed local array has that exact section count. "
        "Other increments found in the function belong to separate loops or "
        "decodedCount, not an additional update of this loop's i.",
    ("equality-cap", "game/src/asset_loading.c", "asset_custom_track_env_selftest:i==MDKR_CUSTOM_TRACK_OBJECT_MAPS"):
        "SECTION DISPATCH: same bounded five-section loops as the MODELS case. "
        "The map-only validation and diagnostics do not alter i. The inner "
        "entry loop is independently bounded by the measured table word count.",
    ("equality-cap", "game/src/asset_loading.c", "asset_custom_track_env_selftest:i==MDKR_CUSTOM_TRACK_HEADERS"):
        "SECTION DISPATCH: same function and loop as the MODELS case. The "
        "four increments the enumerator counts are the `++i` of four separate "
        "`for (unsigned i = 0; i < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++i)` "
        "loops, each declaring its own i; the header branch iterates `entry` "
        "and `field`, never i. Its five i-indexed writes (physicalSizes, "
        "tables, ends, payloads, inputs) all target [SECTION_COUNT] locals.",
    ("equality-cap", "game/src/asset_loading.c", "asset_custom_track_env_selftest:i==MDKR_CUSTOM_TRACK_TEXTURES"):
        "SECTION DISPATCH: same loop and reasoning as the HEADERS case. The "
        "texture branch iterates `entry` against words[i] and changes no i.",
})

SHAPE_INFO_REVIEW = {
    ("equality-cap", "game/src/camera.c", "sprite_model_build_bark_vertices:sSpriteModelBarkVertexCount!=0"):
        (2, "Nonempty-cache early return and final success predicate, not caps. "
         "Each write follows count >= MAX refusal and capacity growth; failed "
         "realloc exits before writing. One increment follows the three xyz "
         "writes into capacity * 3 floats; capacity never exceeds MAX=100000."),
}

# Ceilings for the unclassified INFO population, per class. May fall; may not rise.
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
    # +7 from extending SCAN_DIRS to platform/: the informational population in
    # the port's own C, none of which is a saturation cap on a shared counter
    # (they are `x == 0` early-outs and single-writer loop indices).
    #
    # 39 -> 45. Six entries, all in the one function the outline-font work
    # added, gfx_font_outline.c solve_face_fit(). Both counters are bounded by
    # the array they index and cannot reach it:
    #   * `sample_count` fills GlyphSample samples[GFX_FONT_REGIONS_PER_ATLAS]
    #     from `for (index = 0; index < limit; index++)` with a single
    #     `sample_count++` per iteration, where `limit` is explicitly
    #     `min(region_count, GFX_FONT_REGIONS_PER_ATLAS)`. The loop bound IS
    #     the capacity, and iterations may `continue` without writing, so the
    #     count is <= limit <= capacity.
    #   * `estimate_count` fills float estimates[GFX_FONT_REGIONS_PER_ATLAS]
    #     across four passes, each reset to 0 and each a
    #     `for (index = 0; index < sample_count; index++)` loop pushing at most
    #     one element per iteration, so it is <= sample_count <= capacity.
    # The `== 0` / `!= 0` comparisons the enumerator sees are empty-set
    # early-outs before a median is taken, not saturation caps on a shared
    # counter.
    #
    # 45 -> 48, RE-MEASURED 2026-08-14 on the first full-suite traversal in
    # the project's history (tools/sweep_bug_shapes.py; the qualification
    # pass that finally reached task 187). The growth is the multiplayer-
    # foundation campaign (ba28eae): rollback_engine_registry.c contributes
    # two registry-full `count ==` saturation guards over fixed-size span
    # tables, and one further site rides the campaign's growth of the
    # objects.c/menu.c populations. Measured, not summed, per the 2026-08-09
    # merge-hazard note below.
    #
    # 48 -> 49, RE-MEASURED 2026-09-01 on the land5 first-ever full-suite run at
    # the crossplay tip. The ceiling was stale from before the crossplay campaign
    # base: measured 49 at b992e538 already (base == tip, so the crossplay
    # campaign added ZERO equality-cap findings), and check_array_bounds_sweep.py
    # is unchanged in-range. The one finding over the old ceiling is in the
    # non-capacity-comparand INFO class (comparand is a small literal or a count,
    # not an array size -- e.g. `i != 2`, `stack_count != 0`, `gModelMatrixStackPos
    # == 0`), which is why the enumerator files it INFO not TRIAGE; runtime
    # array-bounds UBSan over the 8 routes reports no overflow. Measured with
    # tools/sweep_bug_shapes.py, not summed.
    #
    # 49 -> 50, RE-MEASURED 2026-09-02 for the pre-flight route measurement
    # (task N5). The one added entry is match_preflight.c
    # `mdkr_match_route_measure_finish:answered != 0`, and it is an empty-set
    # early-out, not a saturation cap: `answered` counts answered probes into
    # `uint16_t sorted[MDKR_MATCH_ROUTE_MAX_PROBES]` from a
    # `for (index = 0; index < sent; index++)` loop with a single `answered++`
    # per iteration, where `sent` is `next_sequence - 1` and `_due` refuses to
    # emit past MDKR_MATCH_ROUTE_MAX_PROBES, so `answered <= sent <= capacity`.
    # The guarded read is `sorted[rank - 1]` with
    # `rank = ceil(0.95 * answered)`, which is in [1, answered] for every
    # nonzero `answered`; the comparison exists so the percentile is not taken
    # over an empty set.
    #
    # 50 -> 60, RE-MEASURED 2026-09-03 on the int-1.7.0 line (fix/m3-reds-3),
    # the first sweep since v1.6.0 merged the Adventure Party co-op code, the
    # Character Workshop / custom-character pipeline and the netplay changes.
    # Eleven keys are new against the v1.6.0 population and one of them
    # (match_preflight.c mdkr_match_route_measure_finish:answered != 0) is
    # already named in the 49 -> 50 note above, so the ceiling moves by ten.
    # Every one was read; not one is a saturation cap on a shared counter, and
    # the growth is entirely the two new subsystems arriving in the sweep's
    # field of view:
    #   * 3 in fast3d/gfx_character_text.c, the Workshop text shaper --
    #     `segment_count != 0` is the reverse-order pop guard on a run split
    #     whose segments each consume at least one codepoint from a buffer the
    #     caller already fenced with `count >= GFX_CHARACTER_TEXT_MAX_CODEPOINTS`;
    #     `index != 0` is a "there is no previous glyph" test in the contextual
    #     face pass; `count == 0` is the empty-input early-out under that same
    #     fence.
    #   * 2 in modern_character_install.c's coordinated removal -- the
    #     `candidate_count == 0` empty-set early-out and the
    #     `candidate_count != 0` guard in front of a `candidates[count - 1u]`
    #     read. The array is realloc-grown and its capacity test is a SEPARATE
    #     site (`candidate_count == candidate_capacity`), so neither of these
    #     two is what bounds the fill.
    #   * 2 in modern_character_runtime.c -- `index == 0` is "is this player
    #     one", used to fall back to MDKR_CUSTOM_CHARACTER in the per-player
    #     env scan; `assembly_count == 0` is an empty-set early-out AFTER an
    #     explicit `assembly_count >= MDKR_MODERN_CHARACTER_MAX_PRIMITIVES`
    #     fence on the fill.
    #   * 1 in modern_character_surface_intersection.c -- `stack_size != 0` is
    #     the BVH node-stack pop underflow guard, with every two-slot push
    #     fenced by `stack_size + 2u > MDKR_SURFACE_BVH_STACK` immediately
    #     beforehand. Byte for byte the shape already triaged above for
    #     camera_object_occlusion.c.
    #   * 1 in fast3d/gfx_webgpu.c -- `required_feature_count != 0` decides
    #     whether to attach a requiredFeatures array at all. The fill is six
    #     conditional pushes into a `WGPUFeatureName required_features[6]`:
    #     at the bound with zero slack, never past it.
    #   * 1 in net/match_preflight.c -- `answered == 0` in route_measure_p95,
    #     the percentile-over-an-empty-set guard, factored out of the finish()
    #     site the 49 -> 50 note already reads. Same `answered <= sent <=
    #     MDKR_MATCH_ROUTE_MAX_PROBES` bound.
    # Measured with tools/sweep_bug_shapes.py, not summed.
    "equality-cap": 60,
    # +116 from platform/. Overwhelmingly `1u << port` / `1u << slot` bit masks
    # over small fixed domains and `value >> (i * 8)` byte extractions -- the
    # var-count flavour the enumerator reports without an added constant. The
    # ceiling's job is to fail on GROWTH, so it is set to the measured
    # population, not padded.
    #
    # 257 -> 262. Five entries were added since the ceiling was measured at
    # aaf7486; each was read and each is bounded by construction, so the
    # population grew without the class changing:
    #   * object_functions.c obj_loop_door / obj_loop_scenery -- both shift
    #     through DKR_SHL32, which is the port's masking idiom and exists
    #     precisely to make a wide count defined.
    #   * gfx_pc_dkr.c dkr_check_alpha_compare (x2) and dkr_textlut_fmt -- the
    #     count is the G_MDSFT_* #define, a compile-time constant; the
    #     enumerator reports var-count for anything that is not a numeric
    #     literal in the expression.
    #   * gfx_pc_dkr.c dkr_dp_load_tile (x2) -- `shift` is assigned 0, 1 or 2
    #     by a switch over G_IM_SIZ_* immediately above the use.
    #   * presentation_snapshot.c camera-cut journal (x2) -- `1u << camera_id`
    #     into a uint32_t, guarded at both sites by
    #     `camera_id < PRESENTATION_SNAPSHOT_MAX_CAMERAS` (8).
    # (Nine appear as additions against aaf7486; four older entries moved or
    # were rewritten, so the net is five.)
    #
    # 262 -> 265. Three sites from the presentation work, each read and each
    # bounded by an explicit guard rather than by convention:
    #   * gfx_pc_dkr.c dkr_capture_uv_scroll_endpoints -- `moved |= 1u << i`
    #     over `i < num_tris`, and the function returns early unless
    #     `num_tris <= GFX_PRESENTATION_UV_SCROLL_MAX_TRIANGLES` (16). The
    #     accumulator is a uint16_t, so the domain and the type match exactly.
    #   * presentation_snapshot.c presentation_snapshot_capture_camera (x2) --
    #     `1u << viewport` where `viewport = write->camera_count` immediately
    #     after `if (write->camera_count >= PRESENTATION_SNAPSHOT_MAX_VIEWPORTS)
    #     return`.
    #   * presentation_snapshot.c presentation_snapshot_capture_commit -- the
    #     unconsumed-note sweep shifts by a loop index bounded by
    #     `write->camera_count`, which the guard above caps; the sibling
    #     `1ull << entry->camera_id` is fenced by an explicit
    #     `camera_id >= 0 && camera_id < PRESENTATION_SNAPSHOT_MAX_CAMERAS`.
    #
    # 2026-08-09: raised 257 -> 261, and the four are NOT one batch of work.
    # Measured on origin/main at 080c4c4 the population is already 260 -- three
    # above the recorded ceiling -- so this gate was failing on main before the
    # content-pack work began, and had been for some time. It went unnoticed
    # because array_bounds_sweep is a `rom`-role check: hosted CI cannot run it
    # (no ROM on a runner), so only a local full-suite run sees it, and a subset
    # run does not. That is the same shape as the last gate this project lost
    # track of. The three are not identified here because identifying them means
    # bisecting the class population across the commits since the ceiling was
    # last set, which is worth doing and is not this change.
    #
    # The fourth is from this work and is named: platform/mod_texture_key.c:46,
    # `bytes[index] = (uint8_t) ((value >> (index * 8)) & 0xffu)` -- the
    # little-endian field encoder in the published texture digest. index runs
    # 0..3 over a 4-byte local, so the shift count is 0, 8, 16 or 24 and the
    # value is a u32; it cannot reach the width.
    #
    # MERGE HAZARD, recorded 2026-08-09. The release line (`land/rc2`) moved
    # these same two constants independently and by different amounts:
    # equality-cap 39 -> 45 and shift-count 262 -> 265 there, for
    # gfx_font_outline.c's solve_face_fit counters and three presentation shift
    # sites. Both branches therefore touch these lines and will conflict
    # textually.
    #
    # RESOLVING IT BY SUMMING THE NUMBERS IS WRONG. The tool reports the
    # informational population *after* excluding the enumerated TRIAGE entries,
    # and the two branches enumerate different sites -- so the combined ceiling
    # is not 261 + (265 - 262), and it is not the larger of the two either. Take
    # both triage blocks (that part is genuinely additive), then RE-MEASURE the
    # combined population on the merged tree and set each ceiling to what the
    # tool actually reports. The ceiling is a measured population, never a
    # proof, and a summed guess would silently re-open exactly the growth this
    # gate exists to catch.
    # 2026-08-10, merge of land/rc2 and worktree-hm-parity-sprints: both
    # narratives above are true of their own branches. Per the merge-hazard
    # note the ceilings were RE-MEASURED on the merged tree with
    # tools/sweep_bug_shapes.py -- not summed, not maxed. shift-count came
    # back 266: the release line's 265 plus exactly one site, and it is the
    # one parity's narrative names and reads (mod_texture_key.c:46, the
    # little-endian digest encoder, index 0..3 over a u32). equality-cap
    # came back 45, coincidentally equal to the release line's number.
    #
    # 266 -> 269. Three sites from the standing finish/spectator-camera
    # exclusion, each read and bounded before the shift:
    #   * presentation_snapshot_set_camera_excluded (x2) returns unless
    #     viewport_index is in [0, PRESENTATION_SNAPSHOT_MAX_VIEWPORTS), then
    #     uses that checked value for the set/clear bit.
    #   * presentation_snapshot_capture_camera reads the exclusion bit using
    #     viewport = write->camera_count immediately after the existing
    #     camera_count >= PRESENTATION_SNAPSHOT_MAX_VIEWPORTS failure guard.
    # These are the same four-viewport domain as the neighboring camera-cut
    # journal sites documented above; none can approach the 32-bit width.
    #
    # 269 -> 367, RE-MEASURED 2026-08-14 on the first full-suite traversal
    # (task 187 had never executed on any machine before this pass). The +98
    # is the online/session campaign's arrival in the sweep's field of view:
    # match_preflight.c (22), match_transport.c (10), party_protocol.c (10),
    # session_bridge.c (9), match_manifest.c, match_signal, sha256.c and the
    # rollback registry -- overwhelmingly `1u << slot` masks over the
    # four-slot roster domain and `value >> (i * 8u)` byte extraction in the
    # wire encoders, the same var-count flavour the +116 note above already
    # describes for platform/. The remainder is campaign growth inside
    # already-counted files (gfx_pc_dkr.c effect-recipe paths, menu.c,
    # object_functions.c). Ceiling set to the measured population, not a
    # guess, per the standing rule.
    #
    # 367 -> 368, 2026-08-25 (release-1.5.2, issue #52). One new var-count
    # shift, in platform/fast3d/gfx_pc_dkr.c: the inverted-rectangle
    # span-invalid rule (dkr_dp_fill_rectangle / dkr_dp_texture_rectangle)
    # reads the cycle type via `rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)`
    # to gate the fixed-point FILL/COPY `+1 << 2` coordinate adjustment before
    # deciding whether an inverted rect draws. G_MDSFT_CYCLETYPE == 20
    # (game/include/PR/gbi.h), so it is a compile-time-constant `3U << 20`
    # into a u32 -- flagged var-count only because the count is a macro, not a
    # numeric literal, and covered at runtime by -fsanitize=shift-exponent.
    # Measured 368 with tools/sweep_bug_shapes.py, not summed.
    #
    # 368 -> 386, RE-MEASURED 2026-09-01 on the land5 first-ever full-suite run.
    # Like the equality-cap ceiling above, this was stale from BEFORE the
    # crossplay campaign: measured 386 at the campaign base b992e538 already
    # (base == tip; the crossplay campaign and its FP-contraction/trig work added
    # ZERO shift-count findings, and this check is unchanged in-range). The +18
    # over the old ceiling accumulated between the release-1.5.2 measure and the
    # campaign base (native-finish-line/HUD/track work) and went unseen because
    # array_bounds is a rom-role check that only runs in a full local suite. All
    # 386 are the var-count flavour (bit masks `1u << slot`, byte extractions
    # `value >> (i * 8u)`) -- none is the added-constant MIPS-mask idiom (those
    # are the shift-count TRIAGE entries), and every one is covered at runtime by
    # -fsanitize=shift-exponent, which reported nothing across the 8 routes.
    # Measured with tools/sweep_bug_shapes.py, not summed.
    #
    # 386 -> 388, RE-MEASURED 2026-09-02 for the pre-flight route measurement
    # (task N5). Both added entries are match_preflight.c's new big-endian
    # 16-bit helpers, `put16` (`value >> 8u`) and `get16` (`input[0] << 8u`) --
    # the same var-count byte-extraction flavour as the put32/get32/put64/get64
    # siblings already counted in this population, on a uint16_t with a
    # literal count of 8.
    #
    # 542 -> 546, RE-MEASURED 2026-09-10. Four new entries, none removed, all in
    # one function: the joint-membership bitmap added to
    # mdkr_modern_character_asset_joint_parent_node
    # (platform/modern_character_asset.c) when that walk stopped re-scanning
    # every joint at every level of the parent chain.
    #     is_joint[possible.node >> 3u] |= (unsigned char)(1u << (possible.node & 7u));
    #     if ((is_joint[(uint32_t)parent >> 3u] & (1u << ((uint32_t)parent & 7u))) != 0u)
    # Both shift COUNTS are `& 7u`, so they take 0..7 on an unsigned int and
    # cannot approach its width -- the undefined case this class exists to find
    # is unreachable by construction rather than by argument. Both byte INDICES
    # are `>> 3u` of a value already refused above unless it is below
    # nodes->count, and the function returns 0 when nodes->count exceeds
    # MDKR_MODERN_NODES_MAX (16384), so the index stays under 2048 -- exactly
    # sizeof(is_joint). Ceiling moves by four.
    #
    # 539 -> 542, RE-MEASURED 2026-09-08 after the 1.7.0 candidate delta was
    # committed. The ceiling had been measured on a tree where that work was
    # still uncommitted, so committing it put the population over a ceiling
    # that had never seen it. Diffing the class against c657fa62 by content
    # rather than by line number gives exactly three new entries and none
    # removed, all in one function, mdkr_trophy_records_merge
    # (game/src/runtime_contracts.c) from the issue #63 per-world best-medal
    # merge:
    #     u32 leftState  = (left  >> shift) & 3U;
    #     u32 rightState = (right >> shift) & 3U;
    #     merged |= (leftState > rightState ? leftState : rightState) << shift;
    # `shift` is loop-local: (world - WORLD_DINO_DOMAIN) * 2 over
    # WORLD_DINO_DOMAIN..WORLD_FUTURE_FUN_LAND, which enums.h fixes at 1..5, so
    # it takes exactly {0, 2, 4, 6, 8} on a u32. The count cannot reach 32 and
    # the shift cannot be undefined; these are the ordinary two-bit-per-world
    # pack/unpack this class already counts elsewhere. Ceiling moves by three.
    #
    # 388 -> 539, RE-MEASURED 2026-09-03 on the int-1.7.0 line
    # (fix/m3-reds-3), the first sweep since v1.6.0 merged the Adventure Party
    # co-op code, the Character Workshop / custom-character pipeline
    # (game/src/menu.c, thread3_main.c, objects.c, game_ui.c and the
    # platform/modern_character_*.c family) and the netplay changes. 154
    # instances are new against the v1.6.0 population and one left it; two of
    # the new ones (match_preflight.c put16/get16) are already named in the
    # 386 -> 388 note above, so the ceiling moves by 151. Every one of the 154
    # was read. One was a DEFECT and is fixed (below) -- its shift is still
    # present and still counted, only its ordering changed. The population
    # falls into four groups, none of which can reach the width:
    #
    #   * 53 are CONSTANT counts that the enumerator files as var-count only
    #     because its LITERAL regex is digits-only and these literals carry a
    #     `u` suffix: big-endian byte packing and extraction (`<< 24u`,
    #     `<< 16u`, `>> 8u`), hex-nibble emission (`>> 4u`), UTF-8
    #     continuation accumulation (`<< 6u`), popcount drains (`>>= 1u`),
    #     the packed 16.16 texture-dimension unpack (`>> 16u`) and one
    #     `>> 32u` of a uint64_t premultiplied-alpha accumulator in the
    #     portrait resampler. Widest count 32, and its value is 64-bit.
    #
    #   * 95 are single-bit masks over a small FIXED domain, every one either
    #     a loop index over that domain's own constant or range-checked before
    #     the shift: seats (ADVENTURE_PARTY_MAX_SEATS 4, and participant_count
    #     validated 2..4 before `(1u << participant_count) - 1u`), controller
    #     ports (MAXCONTROLLERS 4), player slots
    #     (MDKR_MODERN_CHARACTER_PLAYERS 4), vehicle contexts
    #     (MDKR_CHARACTER_CONTEXT_COUNT 4), humanoid rig roles
    #     (MDKR_MODERN_HUMANOID_ROLE_COUNT 16), vehicle contacts
    #     (MDKR_MODERN_CHARACTER_CONTACTS 4), preview landmarks (3), preview
    #     joints (16), motion-review samples (11), LOD levels (4), occluder
    #     classes (3), net input slots (MDKR_NET_INPUT_SLOTS), bounding-box
    #     corner axes (3), and worlds (`settings->worldId` fenced
    #     `>= 0 && < 16` at its own site). Widest of the family is
    #     modern_character_runtime.c's LOD state bit,
    #     `context * MDKR_MODERN_CHARACTER_VIEWS + view` with context < 4 and
    #     view < 8, i.e. 0..31 into a uint32_t -- at the width boundary with
    #     zero slack, both factors checked at the function's entry; objects.c
    #     `1u << (player * 8 + reason)` is the same 0..31 shape with
    #     player < 4 and reason < 8 checked immediately above it. The two
    #     `while ((bit >> slot) != 1u) slot++` scans in the rig-role mapping
    #     terminate at slot <= 15 because `bit` is a single set bit produced
    #     by `1u << index` with index < 16, and both are guarded on
    #     `bit != 0`. The compiled-asset fields in this group
    #     (`attachment.context`, `constraint.role`) are bounded by the .mdkc
    #     validator, not locally: validate_references() refuses the package
    #     unless every attachment context is < MDKR_CHARACTER_CONTEXT_COUNT,
    #     and it runs inside load_file() before any consumer sees the asset.
    #
    #   * 3 are the 8x8 visibility-tile masks in the skinned-character
    #     occlusion path (gfx_webgpu.c): `UINT64_C(1) << tile` with
    #     tile < WGPU_SKINNED_VISIBILITY_TILES (GRID 8 squared == 64), so
    #     0..63 into a uint64_t. At the boundary, defined; the drains beside
    #     them are the `>>= 1u` popcounts already counted in the first group.
    #
    #   * 3 are byte-index shifts `(byte * 8u)` / `(index * 8u)` over exactly
    #     eight bytes into a 64-bit value -- the install archive's
    #     little-endian size field (mz_uint64), the catalog revision folded
    #     from source_sha256, and render.c's digest_id. Count 0..56.
    #
    # THE DEFECT, fixed in this commit: modern_character_runtime.c
    # mdkr_modern_character_request_surface_diagnostics() computed
    # `const uint32_t bit = 1u << (unsigned)context;` in its declaration list,
    # i.e. BEFORE the `context < MDKR_CHARACTER_CONTEXT_CAR || context >
    # MDKR_CHARACTER_CONTEXT_PLANE` guard that exists precisely because
    # `context` is a caller's value. A declaration initialiser runs first, so
    # a negative or wide context shifted by a count at or past 32 on the very
    # path the function refuses. The initialisation now happens after the
    # guard. The record of the reading is here rather than in SHAPE_TRIAGE
    # because a triage entry for an INFO-severity key would match nothing and
    # fail as stale.
    #
    # Swept the class, not just the instance (CONTRIBUTING.md rule 6): every
    # other `<type> x = 1u << ...` initialiser in game/ and platform/ was read
    # (presentation_snapshot.c:964, audio_sequence.c:73,
    # camera_dynamic_occlusion.c:137, platform_sdl_min.c:1945,
    # gfx_webgpu.c:12211, thread3_main.c:811, modern_character_asset.c:1253,
    # modern_character_runtime.c:2506, save_tools_core.c:453). Each is a loop
    # index over its own domain constant or already past its guard; 1614 was
    # the only one whose guard stood behind it.
    # Measured with tools/sweep_bug_shapes.py, not summed.
    #
    # 546 -> 560, RE-MEASURED 2026-10-01 on release/1.8-integration (2683fbdf).
    # Diffed against 2d015340 (where 546 was measured) by (file, key, text),
    # not by line: exactly 14 new, none removed. (v1.7.0 itself measured 547:
    # rice_crc.c's `(long)width << siz` -- count fenced to 0..3, but the
    # result could overflow Windows' 32-bit long -- which ead14d37 replaced
    # with a uint64_t table multiply, so it is gone and not counted here.)
    # Every one was read; none can shift by a negative count or by >= the
    # width of its promoted left operand:
    #   * mod_registry.c (5) and mod_texture_store.c (5), the pack name index:
    #     `UINT64_C(1) << index/pack` over pack indices. reg->count is written
    #     only by mdkr_mod_registry_init's `reg->count++`, which is refused at
    #     `reg->count >= MDKR_MOD_MAX_PACKS` (64), so every loop
    #     (`index < reg->count`, `index < s_registry->count && index <
    #     MDKR_MOD_MAX_PACKS`, `pack = MDKR_MOD_MAX_PACKS - 1 .. 0`) yields
    #     0..63 on a uint64_t. name_index_add's `bit` initialiser has no guard
    #     to run ahead of: its sole caller passes scan.pack, that same 0..63
    #     init-loop index. registry_live_packs is
    #     `count >= 64 ? ~0 : (UINT64_C(1) << count) - 1u` and both callers
    #     return first when count <= 0, so the shift sees 1..63.
    #   * custom_track_model_payload.c mdkr_track_model_repack -- `decoded >>
    #     (8 * i)`, i < 4, into a size_t (32 bits on wasm32): counts 0..24.
    #   * gfx_pc_dkr.c dkr_draw_sprite_model -- `3u << G_MDSFT_CYCLETYPE`, the
    #     compile-time 20 (PR/gbi.h); var-count only because it is a macro.
    #   * sprite_models.c mdkr_sprite_models_note_pose -- `1u << (bucket &
    #     31u)`: count 0..31 on an unsigned, and the word index bucket >> 5
    #     with bucket = ... & 255u is 0..7 inside uint32_t phases[8].
    #   * save_inventory_selftest.c save_compat_fixture -- `1 << v` in
    #     `for (v = 0; v < 3; ++v)`: an int shifted by 0..2.
    #
    # 560 -> 561: mod_registry.c name_hit_rank (889b857f), measured after
    # 2683fbdf so the gate was red on it -- `UINT64_C(1) << pack` with pack
    # starting at MDKR_MOD_MAX_PACKS - 1 (63, static_assert <= 64) and only
    # decrementing to 0: a uint64_t shifted by 0..63.
    # 561 -> 566 when the sweep took in every C header (it had read only .c
    # files plus bounded_json.h by name). All five are in
    # platform/modern_character_lod.h and shift 1u (32 bits) by an LOD index
    # held below MDKR_MODERN_CHARACTER_LOD_LEVELS (4): resolve_lod's
    # `maximum` starts at 3 and only decrements, `selected` is refused at >= 4
    # on entry and then clamped to 0..maximum, and both hysteretic selectors
    # test `previous_lod >= MDKR_MODERN_CHARACTER_LOD_LEVELS` before their
    # `1u << previous_lod` in the same || chain. Counts 0..3.
    "shift-count": 566,
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

    triage, info, reviewed, ok = [], {}, {}, True
    for line in proc.stdout.strip().split("\n"):
        if not line:
            continue
        cls, rel, ln, key, sev, txt = line.split("|", 5)
        if sev == "TRIAGE":
            triage.append((cls, rel, int(ln), key, txt))
        elif (cls, rel, key) in SHAPE_INFO_REVIEW:
            identity = (cls, rel, key)
            reviewed[identity] = reviewed.get(identity, 0) + 1
        else:
            info[cls] = info.get(cls, 0) + 1

    for identity, (expected, reason) in SHAPE_INFO_REVIEW.items():
        if reviewed.get(identity, 0) != expected or not reason:
            ok = False
            print("  FAIL: named INFO review %s expected %d occurrence(s), found %d" %
                  (identity, expected, reviewed.get(identity, 0)))

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
        # Each control forces a boundary from a clean new game, then reads a probe
        # that only appears once the forced race is actually running. These runs
        # inherit MDKR_SAVE_DIR from the process env, which the phase-1 census
        # routes above (and any prior task) write into; a race/adventure EEPROM
        # left there re-routes the boot/menu flow so the forced race never
        # launches and the boundary reads xzClamped=0 / maxCandidates=0 / no
        # probe -- the guard then goes untested. A non-packaged build no longer
        # isolates saves to $CWD/save (issue #54), so pin each control to its own
        # fresh save dir and video config to keep the route deterministic.
        save = os.path.join(logdir, "save-" + name)
        os.makedirs(save, exist_ok=True)
        env["MDKR_SAVE_DIR"] = save
        env["MDKR_VIDEO_CONFIG_PATH"] = os.path.join(save, "video.ini")
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
