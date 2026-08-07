/**
 * decomp_names.h — readable aliases for the decomp's remaining raw symbols.
 *
 * PORT-OWNED FILE. It does not exist upstream and a sync never touches it; the
 * "why it lives here" section below shows why that is structural, not luck.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS
 * ---------------------------------------------------------------------------
 * `game/{src,include}` is vendored from the DKR decomp and kept in step with
 * upstream by a 3-way merge (`tools/sync_decomp.sh`, `docs/DECOMP_SYNC.md`).
 * Upstream has named most of the tree, but a residue of symbols is still
 * spelled by its ROM address — `func_800BDC80`, `D_8011C238`. Those are
 * addresses, not names: you cannot read a call site and tell what it does.
 *
 * The obvious fix — renaming them in place — is the one thing this tree must
 * not do. Every raw token in vendored text is a line upstream may also edit,
 * and renaming it turns the next sync's clean merge into a hand-resolved
 * conflict, permanently, for as long as the port tracks upstream. The port has
 * exactly one deliberate in-place rename (`camSetProjMtx`), and DECOMP_SYNC.md
 * records it as a standing conflict accepted on purpose.
 *
 * So the names live HERE instead, as aliases that expand back to the raw
 * symbol:
 *
 *     #define shadow_clip_polygon_to_footprint  func_8002FF6C
 *
 * Port-owned code that includes this header calls
 * `shadow_clip_polygon_to_footprint(...)`, and the compiler still sees — and
 * the linker still emits — `func_8002FF6C`. The vendored text stays
 * byte-identical to what the sync produced. The alias adds no symbol, changes
 * no signature, and cannot change behaviour. It even works on a definition:
 * `platform/audio_compat.c` can define `void alSynSetVoiceAuxBus(...)` and the
 * object file still exports `func_80065A80` for the vendored callers. This is
 * the same technique, in the same direction, that `platform/sim_hash.c`
 * already uses for the decomp's `unkNN` struct members.
 *
 * ---------------------------------------------------------------------------
 * IT IS ALSO A GLOSSARY, AND MOSTLY THAT
 * ---------------------------------------------------------------------------
 * Most references to these symbols sit inside vendored files — including the
 * port's own `#ifdef NATIVE_PORT` blocks, which live in vendored text and are
 * therefore exactly as merge-sensitive as the code around them. Those call
 * sites stay raw ON PURPOSE. For them this header is a lookup table: grep the
 * address here and the entry tells you what the thing is and what proves it.
 *
 * Migrating a raw call site to an alias is allowed only in a file the decomp
 * does not have: everything under `platform`, and the port-authored files in
 * `game/src` (`taj_*.c`, `camera_*.c`, `object_layout.c`, `sprite_layout.c`,
 * `runtime_contracts.c`, and the `hasm_native` sources). Never add the include,
 * or an alias, to a vendored file.
 *
 * Comments that cite a raw address keep citing it. The address is the
 * searchable key tying a comment to `symbol_addrs.us.v80`, to the `.s` files
 * and to upstream's tracker; a comment that says only
 * `shadow_clip_polygon_to_footprint` cannot be looked up anywhere else.
 *
 * ---------------------------------------------------------------------------
 * WHY IT LIVES HERE, AND WHY A SYNC CANNOT TOUCH IT
 * ---------------------------------------------------------------------------
 * `tools/sync_decomp.sh` builds its work list by running, INSIDE THE DECOMP
 * CHECKOUT, `git diff --name-only BASELINE -- src include libultra`. Only paths
 * that exist upstream can appear in it, so a file upstream does not have is
 * never read, merged or written. In the hypothetical where upstream one day
 * adds its own `include/decomp_names.h`, `git show BASELINE:include/…` finds no
 * blob and the script SKIPs the file with "(no baseline blob)" rather than
 * overwriting it — it fails safe, not silently.
 *
 * `game/include` is on the compiler's include path (CMakeLists.txt) and is NOT
 * globbed for compilation — only the `.c` files directly under `game/src`,
 * `game/src/hasm` and `game/src/hasm_native` are — so a header here adds no
 * translation unit.
 * `game/include/dkr_native_ptr.h` is the existing precedent for a port-owned
 * header in the vendored include directory.
 *
 * ---------------------------------------------------------------------------
 * HOW TO ADD A NAME
 * ---------------------------------------------------------------------------
 * 1. One entry: the alias, the raw symbol, and a comment stating THE EVIDENCE
 *    — the existing comment that says it, the caller that proves it, the
 *    dispatch arm it sits in, the named field its result is stored into, the
 *    check that pins its contents. "It looks like X" is not evidence and does
 *    not go in this file.
 * 2. Match the defining file's convention: functions are snake_case, globals
 *    are gCamelCase, and the audio driver files keep libultra's `alXxx`
 *    camelCase. Adopt upstream's spelling wherever upstream has one.
 * 3. Record confidence honestly:
 *      plain name        the evidence settles it
 *      `~` in comment    role-derived: the name describes the role the code
 *                        demonstrably plays, not an established fact
 *      `_UNVERIFIED`     in the NAME, when even the role is a reading
 *    A WRONG NAME IS WORSE THAN `func_`. If you cannot cite something, leave
 *    the symbol out. The remaining residue has a home: `docs/open-items/misc.md`
 *    (wave "decompnames"). Archaeology is a separate job from this file.
 * 4. Never define an alias whose expansion is used as anything other than the
 *    raw global it stands for.
 */
#ifndef MDKR_DECOMP_NAMES_H
#define MDKR_DECOMP_NAMES_H

/* ===========================================================================
 * game/src/tracks.c — view-frustum culling
 * ========================================================================= */

/* Named in prose by its own caller: scene_visibility_prepare_viewport's comment
 * calls it "the cull planes (func_8002A31C -- which keeps sFaithfulCullPlanes
 * for object admission…)". */
#define track_compute_cull_planes            func_8002A31C
/* Constant camera-space triangle corners, read ONLY by the cull-plane
 * construction in track_compute_cull_planes / mdkr_build_cull_plane. NOT
 * collision data, as the residue audit supposed — collision lives in
 * LevelModelSegment::collisionPlanes, built by track_init_collision. */
#define gCullPlaneBaseTris                   D_800DC8AC
/* The three derived plane equations. Written only by track_compute_cull_planes,
 * read only by block_visible's bounding-box test; its NATIVE_PORT neighbour
 * comment contrasts it with sFaithfulCullPlanes. Also NOT collision data. */
#define gGeometryCullPlanes                  D_8011D0F8

/* ===========================================================================
 * game/src/tracks.c — object shadow generation
 * ========================================================================= */

/* Fan-triangulates the accumulated shadow polygons straight into the NAMED
 * outputs gCurrShadowVerts / gCurrShadowTris / gCurrShadowHeapData. */
#define shadow_build_mesh_from_polygons      func_8002F440
/* Sutherland-Hodgman clip of one polygon against the shadow footprint. The
 * decomp's own comments above it pin the shapes ("arg0 is always 3", "arg2 is
 * always 4") and the body is the textbook inside/outside edge loop. */
#define shadow_clip_polygon_to_footprint     func_8002FF6C
/* ~ the water arm of shadow_generate, taken when the segment has waves.
 * Existing comment above it: "Handles water shadow generation?". */
#define shadow_build_wave_polygons           func_8002EEEC
/* ~ area-weighted blend across the shadow's lighting groups; the result is
 * stored into obj->shading->unk0. Existing comment: "Transition points between
 * different lighting levels, used by certain objects". */
#define shadow_compute_lighting_blend        func_8002FA64

/* Welded source-geometry vertices for the shadow polygons, deduplicated by
 * (x,z). Every write is a `D_8011B120[D_8011B118++]` pair and every bound check
 * is against ARRAY_COUNT(D_8011B120). */
#define gShadowVertexCache                   D_8011B120
#define gShadowVertexCacheCount              D_8011B118
/* Per-clip-edge cache of the vertices clipping creates, four 32-entry slices.
 * The port's own _Static_assert and comment already spell the layout out:
 * "index = plane*32 + slot". */
#define gShadowClipVertexCache               D_8011B330
#define gShadowClipVertCounts                D_8011B320
/* The per-object clipped-polygon list the two builders fill and the mesh and
 * lighting passes consume; unk0 / unk2[] / unkA are read as vertex count,
 * vertex indices and lighting group. */
#define gShadowPolygons                      D_8011C238
#define gShadowPolygonCount                  D_8011C230
/* waves_get_shadow_tile_triangles' two outputs for one wave tile grid — the
 * triangle corners, and the matching plane equations written at the running
 * offset. Its 20-line NATIVE_PORT comment names all three and their capacities;
 * tracks.c then takes `&D_8011C8B8[D_8011D0B8 + n]` as a source-plane pointer
 * exactly the way the ground path takes `&segment->collisionPlanes[…]`. */
#define gShadowWaveTileTris                  D_8011C3B8
#define gShadowWaveTilePlanes                D_8011C8B8
#define gShadowWaveTilePlaneOffset           D_8011D0B8

/* ===========================================================================
 * game/src/tracks.c — level segments and water queries
 * ========================================================================= */

/* ~ per-segment water-height init: the segment->unk38 it computes is read back
 * as the static water height by get_level_segment_waves' fallback arm. */
#define block_init_water_height              func_8002C71C
/* Builds the per-triangle 8x8 XZ overlap bitmask in segment->unk10[]. The query
 * side is the `DKR_PTR(s16, seg->unk10)[faceNum] & mask` fast-reject in
 * get_inside_segment_count_xz and the collision-candidate walk. */
#define block_build_triangle_grid_masks      func_8002C954
/* ~ depth-sorts the void/skybox segments. tests/check_adventure_hub.py records
 * it as "(void/skybox segment sorting)" — its sp94 overrun is one of the three
 * bugs that check exists for. */
#define sort_void_segments                   func_80026E54

/* get_level_segment_waves' result cache. tracks.h's NATIVE_PORT block comment
 * already names the triple ("storage, gTrackWaves ordering, count"), and
 * wave_query_cache_save/restore copy exactly these two into WaveQueryCache. */
#define gTrackWaveProps                      D_8011D128
#define gTrackWaveCount                      D_8011D308

/* ===========================================================================
 * game/src/waves.c
 * ========================================================================= */

/* The two arms of waves_render's `if (gWaveController.xlu)` branch. Only the
 * XLU arm runs the alpha ramp against the shore-depth table; the opaque arm
 * hardcodes 0xFF,0xFF,0xFF,0xFF. */
#define waves_update_vertices_xlu            func_800B92F4    /* ~ named for the branch it is */
#define waves_update_vertices_opaque         func_800B97A8    /* ~ likewise */
/* Both call sites store the return and the out-param straight into named
 * fields: `D_8011D128[n].waveHeight = …(…, &wave->rot)` in tracks.c, and the
 * racer buoyancy read the port's Phase-2b comment documents. */
#define waves_get_height_and_normal          func_800BB2F4
/* Existing comment on the line above its definition: "determines current
 * bounding box, batch and texture" — it sets gWaveBoundingBox* / gWaveBatch /
 * gWaveTexture from the first RENDER_WATER batch it finds. */
#define waves_find_water_batch               func_800BBE08
/* ~ computes gWaveTileCountX/Z and gWaveTileGridCount and carves the combined
 * gWaveModel / gWaveGenList / gWaveGenObjs allocation; it is the function the
 * NATIVE_PORT 64-bit carving fix in waves.c rewrites. */
#define waves_build_tile_grid                func_800BBF78
/* ~ walks a shadow's tile grid emitting two triangles per cell plus their plane
 * equations; the 20-line NATIVE_PORT comment above it describes exactly that,
 * and platform/stubs_dkr.c calls the pair "triangle fill" and "height fill". */
#define waves_get_shadow_tile_triangles      func_800BDC80
/* ~ advances each live generator's gWaveGenList[i].unk1A swell phase by
 * (unk1C * updateRate) >> 4. The port's Phase-2b comment names it as the
 * per-tick phase advance, and it belongs to the wavegen_* family. */
#define wavegen_advance_phase                func_800BFE98

/* The 26-slot visible-wave-block table. NATIVE_PORT already backs both halves
 * with one `WaveVisibleTable` union plus two _Static_asserts, because waves.c
 * indexes the first array straight on into the second; that split is what
 * crashed the browser build (tests/check_wave_visible_table.py). */
#define gWaveVisibleBlocks                   D_8012A5E8    /* the table, slots 0..25 */
#define gWaveVisibleBlocksTail               D_8012A600    /* the same table's slots 2..25 */
/* ~ per-grid-vertex 0..255 byte built at level load from collision_get_y: how
 * clear the water surface is of geometry beneath it. Consumers gate on `< 0x7F`
 * to fade alpha and damp wave height — i.e. shorelines. */
#define gWaveShoreDepthTable                 D_800E3178
/* ~ per-tile packed vertex-buffer slot ids. Its own comment says "indexed by
 * gWaveModel.unkC", and waves.h says that field "indexes D_800E30D4". */
#define gWaveTileSlotIDs                     D_800E30D4
/* ~ per-tile bucket of up to 8 wave-generator indices, 0xFF-terminated. Its own
 * comment says "tracks an index into gWaveGenList". */
#define gWaveTileGenBuckets                  D_800E3184
/* ~ the two flat-quad triangles per flip state, used when a tile has no HQ
 * sub-block; selected as `&D_800E3090[gWaveVertexFlip << 1]`, the same
 * flip-indexing convention as gWaveVertices / gWaveTriangles. */
#define gWaveFlatQuadTriangles               D_800E3090

/* ===========================================================================
 * game/src/racer.c — per-vehicle update
 * ========================================================================= */

/* The four arms of `switch (racer->vehicleID)`; the case labels ARE the
 * evidence, and the same dispatch appears twice with the same mapping. */
#define racer_update_car                     func_8004F7F4    /* VEHICLE_CAR; + "Car vehicle logic." */
#define racer_update_loopdeloop              func_8004CC20    /* VEHICLE_LOOPDELOOP; + "Handles loop de loops" */
#define racer_update_hovercraft              func_80046524    /* VEHICLE_HOVERCRAFT */
#define racer_update_plane                   func_80049794    /* VEHICLE_PLANE; + "Plane physics, largest function in DKR." */
/* The grounded half of the car velocity update: `if (racer->groundedWheels > 0)
 * func_80050A28(…) else update_car_velocity_offground(…)`. objects.c's own
 * NATIVE_PORT comment restates its body line for line. */
#define update_car_velocity_grounded         func_80050A28
/* ~ racer/ground collision resolution. Existing comment hedges ("Related to
 * ground collision?"), but save_data.c's NATIVE_PORT comment independently
 * calls it "the racer collision path (func_80054FD0, reached from
 * update_player_racer)". */
#define racer_update_ground_collision        func_80054FD0
/* Existing comment: "Used for magnet and homing rockets to get the distance to
 * the nearest racer."; the weapon code assigns its result to `intendedTarget`. */
#define racer_find_nearest_racer             func_8005698C

/* ===========================================================================
 * game/src/racer.c — AI
 * ========================================================================= */

/* Named outright by a port comment in the same file: "DKR's AI
 * throttle/behaviour routine (func_80042D20) hits its `if (var_t0 == 0)
 * return;` guard". */
#define racer_ai_throttle_behaviour          func_80042D20
/* ~ the `default:` arm of racer_AI_pathing_inputs' `switch (raceType)`, beside
 * the already-named racer_ai_challenge and racer_ai_eggs; the same port comment
 * says "Steering is still computed (it happens earlier in func_80045C48)". */
#define racer_ai_race                        func_80045C48
/* ~ the two racing-line selector tables. tests/check_authored_rng_compat.py
 * names them RACING_LINE_TABLE / RACING_LINE_NEIGHBOUR and pins BOTH their
 * contents and their adjacency: racePosition is 1-indexed, so 8th place reads
 * one past the first table and lands on the second's [0] == 1. That overrun is
 * authored behaviour, asserted rather than fixed. The second table has its own
 * independent use, indexed by the number of CPU racers ahead. */
#define gRacerAIRacingLineByPosition         D_800DCDA0
#define gRacerAIRacingLineByRivalsAhead      D_800DCDA8

/* ===========================================================================
 * game/src/objects.c — object registry, collision, render
 * ========================================================================= */

/* Named by a load-bearing comment in the same file: "SIMULATION: sphere
 * collision func_80016748 (…, which writes racer velocity and fires rumble)". */
#define obj_sphere_collision_push            func_80016748
/* The per-facet object-model collision test. tests/check_door_blocks.py opens
 * with "func_80017A18() is the per-facet object-model collision test", and its
 * 27-line NATIVE_PORT header records the same. Upstream has since matched it
 * with named parameters. */
#define object_model_test_collisions         func_80017A18
/* ~ per-tick update of obj->animatedObject (cutscene props, door openers,
 * Wizpig's ship, the char-select actor). platform/sim_hash.c records the
 * result field as `~ func_8001F460 result, tested == 0`, and object_functions.c
 * gates obj_door_open on `1 - func_8001F460(…)`. */
#define obj_update_animated_object           func_8001F460
/* The 16-slot Object* id registry (D_8011AE08) and its three operations, each
 * with the decomp's own one-line comment: "Reset all values … to NULL", "Set
 * the object value for the given index if it's not already set", "Set the next
 * available value …, and return it's index value. -1 if it's not set." */
#define obj_id_slots_reset                   func_8000CBC0
#define obj_id_slot_set                      func_8000CBF0
#define obj_id_slot_alloc                    func_8000CC20
/* Existing comment: "Renders the boost graphics." */
#define obj_render_boost                     func_800135B8
/* ~ Catmull-Rom walk of an NPC along its AI-node path (ainode_get /
 * ainode_find_next / move_object), returning the distance moved, which every
 * caller feeds straight into animFrameF. Existing comment: "Updated
 * Object_NPC". */
#define npc_move_along_ainode_path           func_8001C6C4
/* Returns bossRaceID + 1 in a boss race and 0 otherwise, so every caller just
 * tests it for nonzero. `D_8011AD24[1]` is assigned from
 * `levelHeader->bossRaceID` in racer.c — but the +1 bias means the value is not
 * the id, and nothing in the tree pins what callers do with the magnitude. */
#define get_boss_race_id_UNVERIFIED          func_80023568

/* ~ persists the vehicle id assigned by track_setup_racers across calls; read
 * at entry to seed gPrevTimeTrialVehicle, overwritten at the end. */
#define gLastSetupVehicle                    D_8011ADC5
/* ~ holds the shadow texture init_object_shadow just loaded so spawn_object's
 * three failure paths can tex_free() it. */
#define gPendingShadowTexture                D_8011AE50
/* ~ dead spillover storage. The NATIVE_PORT comment above them states the
 * layout: one 0x30-byte ShadeProperties object is split across gWorldShading
 * plus these two adjacent symbols by a legacy pointer cast. Neither is read or
 * written anywhere else in the tree. */
#define gWorldShadingSpillA                  D_8011AF34
#define gWorldShadingSpillB                  D_8011AF38

/* ===========================================================================
 * game/src/object_models.c
 * ========================================================================= */

/* The entire body is three clamps: obj->modelIndex into [0, numberOfModelIds),
 * obj->animationID into [0, numberOfAnimations), obj->animFrame into the
 * animation's length. taj_visual.c calls it after every animFrame advance. */
#define obj_clamp_model_animation            func_80061C0C

/* ===========================================================================
 * game/src/textures_sprites.c
 * ========================================================================= */

/* Resets a LevelHeader_70 colour cycle to its start: unk4/unk8/unkC cleared,
 * rgba restored from rgba2, unkC re-summed from the per-entry durations. It
 * sits immediately above update_colour_cycle, whose doc comment records the
 * official name updateColourCycle — this is that cycle's reset half. */
#define reset_colour_cycle                   func_8007F1E8

/* ===========================================================================
 * game/src/save_data.c — the EEPROM slot bit stream
 * ========================================================================= */

/* The MSB-first bit-stream cursor the reader and writer share: a byte pointer,
 * the byte in hand, and the walking mask. All three are set together at every
 * stream open, and the mask's seed is what distinguishes the two directions
 * (0 forces the reader to fetch; 128 primes the writer). */
#define gSaveBitCursor                       D_801241EC
#define gSaveBitByte                         D_801241F0
#define gSaveBitMask                         D_801241F4
/* Reads n bits MSB-first and returns them. The decomp's comment says "arg0 is
 * the number of bits we care about", and tests/harness_utils.py decodes the
 * slot "exactly as func_80072C54 reads it". */
#define save_read_bits                       func_80072C54
/* Writes n bits MSB-first, mirroring every read call site 1:1. NOTE the decomp
 * comment above it ("arg1 is the bit being looked for") is WRONG: the body
 * stores into *gSaveBitCursor, it does not search. */
#define save_write_bits                      func_80072E28
/* tests/check_campaign_progression.py derives every slot bit offset from "the
 * write order in game/src/save_data.c func_800732E8"; it opens the stream in
 * write mode (mask seeded to 128). */
#define save_write_slot                      func_800732E8

/* The Controller Pak ghost pair. tests/check_ghost_matrix.py quotes the shared
 * match key — `levelId == …unk0 && vehicleId == …unk1` — and racer.c's port
 * comments call func_80075000 "the write half of the pair-keyed round trip the
 * load probe closes". Each is the sole worker behind timetrial_load_player_ghost
 * / timetrial_write_player_ghost. */
#define read_ghost_from_controller_pak       func_80074B34
#define write_ghost_to_controller_pak        func_80075000
/* ~ enumerates the pak's ghost directory into four parallel caller arrays; the
 * decomp's own parameter names are levelIDs / vehicleIDs / characterIDs /
 * checksumIDs, and the port's added guard rejects a capacity below
 * DKR_GHOST_SLOT_COUNT. */
#define list_ghosts_on_controller_pak        func_800756D4

/* ===========================================================================
 * game/src/menu.c
 * ========================================================================= */

/* Allocates and initialises the UI's wooden panels — its own comment says so,
 * and its first act is to call the already-named menu_button_free(). */
#define menu_button_alloc                    func_8007FFEC
/* ~ the Time Trial voice handle for the track-select cursor: sound_play(…,
 * &D_80126848) on scrolling out of the TT row, sndp_stop on scrolling back in,
 * cleared on menu entry beside the already-named gTrackTTSoundMask. */
#define gTrackSelectTTVoiceSound             D_80126848
/* The two words that the track-select fill loop's fifth row lands on. menu.c's
 * NATIVE_PORT "LAYOUT (split-array class)" block derives the addresses
 * (&gTrackSelectIDs[4][0] + 4 and + 8) and its TrackSelectIDsSplit union
 * already names these exact bytes unused1C / unused20. Never read under their
 * own names. */
#define gTrackSelectRow4Pad1C                D_8012691C
#define gTrackSelectRow4Pad20                D_80126920

/* ===========================================================================
 * game/src/game_ui.c — HUD
 * ========================================================================= */

/* ~ per-(player count, player) HUD extra-info mode, cycled 0..3 by C-Down in
 * multiplayer, the mirror of the already-named single-player
 * gShowCourseDirections. Its three nonzero values gate the stopwatch, the
 * banana counter and the lap count respectively. */
#define gHudInfoDisplayMode                  D_800E2794
/* ~ the two Time Trial voice countdowns and the once-played widening flag; each
 * is seeded, counted down, and consumed by exactly one sound_play arm, in code
 * duplicated identically across the vanilla and authored-tick paths. */
#define gHudTTBoostVoiceTimer                D_80126D4C   /* seeded 60, gates SOUND_VOICE_TT_OH_NO */
#define gHudTTGhostVoiceTimer                D_80126D50   /* seeded 120..1200, gates the ghost-proximity line */
#define gHudTTOhNoVoiceVariety               D_80126D64   /* widens hud_rand_range(0, …+2) after the first play */
/* ~ the level's ASSET_MISC_58 "pulsating light" colour-cycle record: fetched by
 * get_misc_asset, normalised by asset_swap_misc_lightdata, reset by
 * reset_colour_cycle, advanced by update_colour_cycle, and read directly for
 * the HUD text colour. platform/asset_swap.h documents the same asset. */
#define gHudColourCycleData                  D_80127194

/* ===========================================================================
 * game/src/camera.c, video.c, thread3_main.c
 * ========================================================================= */

/* The flat backing store for the MtxF *gModelMatrixF[6] stack. camera.c's own
 * comment says "gModelMatrixF is [6] but cam_init() only fills [0..4] —
 * D_80120DA0 only has storage for 5 matrices". */
#define gModelMatrixFStorage                 D_80120DA0
/* ~ the ceiling gVideoDeltaTime may grow to: seeded to LOGIC_15FPS by
 * fb_init_vi, tested as `… && D_801262E4 >= observedUpdateRate` by the
 * delta-time adaptation, and re-set per level from
 * gCurrentLevelHeader->unk4[numberOfPlayers]. */
#define gVideoDeltaTimeCeiling               D_801262E4
/* ~ unpacks an Exit object's level_entry into gLevelSettings and arms the
 * transition flag. platform/mdkr_adventure.c: "racer_enter_door() then drives
 * the last stretch and calls func_8006D968() on the exit's level_entry". */
#define set_level_transition_from_exit_entry func_8006D968

/* ===========================================================================
 * Audio — the port supplies these bodies (platform/audio_compat.c), so the
 * alias names a definition, not just a call. libultra camelCase, matching the
 * alSynXxx and alSeqXxx siblings beside them.
 * ========================================================================= */

/* ~ re-parents a pooled physical voice's envmixer onto aux bus `bus`, via
 * alAuxBusParam AL_FILTER_UNK11 (remove) then AL_FILTER_ADD_SOURCE (add). The
 * port's own doc comment on the definition says exactly that, and the signature
 * shape matches alSynSetPriority(ALSynth *, voice, s16). */
#define alSynSetVoiceAuxBus                  func_80065A80
/* ~ posts the AL_MIDI_UNK_5F control change whose handler sweeps every channel,
 * zeroing stored fxmix below `vel` while driving live voices to it. The port
 * comment above the definition calls it "the global FX-send floor described in
 * the AL_MIDI_UNK_5F handler". */
#define alSeqSetFxSendFloor                  func_80063A90

/* ===========================================================================
 * Anti-piracy sentinel (platform/segment_consts.c supplies the value)
 * ========================================================================= */

/* camera.c's own extern comment: "Used as a symbol for anti-piracy checks in
 * the game." Retail reads ROM domain 0xB0000578; the port hard-codes 0x8965 so
 * the `& 0xFFFF == 0x8965` test passes. */
#define gAntiPiracySentinel                  D_B0000578

/* ---------------------------------------------------------------------------
 * DELIBERATELY NOT NAMED
 * ---------------------------------------------------------------------------
 *   func_80079760  declared in PR/sched.h between osScGetInterruptQ and
 *                  osScRemoveClient, stubbed as a no-op by the port, called
 *                  from nowhere in the tree. Position in a header is not
 *                  evidence of behaviour.
 *   func_800113BC  the decomp's own comment is "Unused function, purpose
 *                  currently unknown."
 *   func_80014B50  "Only used in the unused function func_800149C0."
 *   func_80084854  "Probably soundoption_render" — the comment hedges, so a
 *                  name here would launder a guess into a fact.
 *   func_80018CE0  "Handles MidiFadePoint, MidiFade, and MidiSetChannel
 *                  objects?" — same reason.
 *   func_80052988  "Anims related to the car I think" — same reason.
 *   func_80073588  its own comment hedges on what arg2 selects ("either lap
 *                  times or course initials?"), which is the only thing that
 *                  would distinguish the two candidate names.
 * The rest of the residue is counted and tracked in docs/open-items/misc.md,
 * wave "decompnames".
 */

#endif /* MDKR_DECOMP_NAMES_H */
