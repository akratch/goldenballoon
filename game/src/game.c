#include "game.h"

#include "asset_enums.h"
#include "asset_loading.h"
#ifdef NATIVE_PORT
#include "asset_swap.h"
#include "mdkr_trace.h"
#include "fast3d/gfx_pc_dkr.h"
#include "fast3d/gfx_level_lighting.h"
#include "presentation_snapshot.h"
#endif
#include "audio.h"
#include "audio_spatial.h"
#include "audiosfx.h"
#include "camera.h"
#include "common.h"
#include "joypad.h"
#include "lights.h"
#include "macros.h"
#include "memory.h"
#include "menu.h"
#include "objects.h"
#include "racer.h"
#include "rcp_dkr.h"
#include "save_data.h"
#include "set_rsp_segment.h"
#include "structs.h"
#include "textures_sprites.h"
#include "thread3_main.h"
#include "tracks.h"
#include "types.h"
#include "video.h"
#include "weather.h"

/************ .data ************/

char *gTempLevelNames = NULL;
s8 gCurrentDefaultVehicle = -1;
u8 gTwoPlayerAdvRace = FALSE;
s32 gIsInRace = 0;

// Updated automatically from calc_func_checksums.py
s32 gViewportFuncChecksum = ViewportFuncChecksum;
s32 gViewportFuncLength = 0x154;
s16 gLevelPropertyStackPos = 0;
s16 D_800DD32C = 0;
s8 D_800DD330 = 0;

/*******************************/

/************ .bss ************/

s32 *gTempAssetTable;
s32 gMapId;
LevelHeader *gCurrentLevelHeader;
char **gLevelNames;
s32 gNumberOfLevelHeaders;
s32 gNumberOfWorlds;
s8 *D_80121178;
LevelGlobalData *gGlobalLevelTable;
s32 gRaceTypeCountTable[16];
AIBehaviourTable *gAIBehaviourTable;
s16 gLevelPropertyStack[5 * 4]; // Stores level info for cutscenes. 5 sets of four properties.

/******************************/

#ifdef NATIVE_PORT
static void mdkr_publish_level_lighting(const LevelHeader *header) {
    GfxLevelLightingInput input = {0};
    const GfxLevelLightingRig *rig;

    if (header == NULL) {
        gfx_level_lighting_reset();
        return;
    }
    input.world_id = header->world;
    input.geometry_id = header->geometry;
    input.skybox_id = header->skybox;
    input.fog_near = header->fogNear;
    input.fog_far = header->fogFar;
    input.fog_rgb[0] = (u8)header->fogR;
    input.fog_rgb[1] = (u8)header->fogG;
    input.fog_rgb[2] = (u8)header->fogB;
    input.background_rgb[0] = header->bgColorRed;
    input.background_rgb[1] = header->bgColorGreen;
    input.background_rgb[2] = header->bgColorBlue;
    input.sky_top_rgb[0] = header->BGColourTopR;
    input.sky_top_rgb[1] = header->BGColourTopG;
    input.sky_top_rgb[2] = header->BGColourTopB;
    input.sky_bottom_rgb[0] = header->BGColourBottomR;
    input.sky_bottom_rgb[1] = header->BGColourBottomG;
    input.sky_bottom_rgb[2] = header->BGColourBottomB;
    input.weather_velocity[0] = header->weatherVelX;
    input.weather_velocity[1] = header->weatherVelY;
    input.weather_velocity[2] = header->weatherVelZ;
    input.weather_intensity = header->weatherIntensity;
    input.sky_scroll[0] = header->unkA2;
    input.sky_scroll[1] = header->unkA3;

    for (int i = 0; i < GFX_LEVEL_LIGHT_MAX_CYCLES; i++) {
        if ((s32)header->unk74[i] == -1) {
            continue;
        }
        LevelHeader_70 *cycle =
            DKR_PTR(LevelHeader_70, header->unk74[i]);
        if (cycle == NULL) {
            continue;
        }
        input.cycles[i].valid = true;
        input.cycles[i].current_rgba[0] = cycle->rgba.r;
        input.cycles[i].current_rgba[1] = cycle->rgba.g;
        input.cycles[i].current_rgba[2] = cycle->rgba.b;
        input.cycles[i].current_rgba[3] = cycle->rgba.a;
        input.cycles[i].authored_rgba[0] = cycle->rgba2.r;
        input.cycles[i].authored_rgba[1] = cycle->rgba2.g;
        input.cycles[i].authored_rgba[2] = cycle->rgba2.b;
        input.cycles[i].authored_rgba[3] = cycle->rgba2.a;
    }

    if (!gfx_level_lighting_publish(&input)) {
        mdkr_trace("level_light: level=%d invalid", (int)gMapId);
        return;
    }
    rig = gfx_level_lighting_current();
    mdkr_trace(
        "level_light: level=%d world=%d geometry=%d "
        "dir=%.4f,%.4f,%.4f colour=%.4f,%.4f,%.4f "
        "strength=%.4f source=0x%x",
        (int)gMapId, rig->world_id, rig->geometry_id,
        (double)rig->direction_world[0],
        (double)rig->direction_world[1],
        (double)rig->direction_world[2],
        (double)rig->colour_linear[0],
        (double)rig->colour_linear[1],
        (double)rig->colour_linear[2],
        (double)rig->strength, (unsigned)rig->source_mask);
}
#endif

/**
 * Allocates memory for gGlobalLevelTable, then populates it with relevant data from every level header.
 * The level headers are streamed from ROM.
 * Additionally loads other globally accessed information, like level names, then runs a checksum compare, for good
 * measure.
 */
void level_global_init(void) {
    s32 i;
    s32 size;
    UNUSED s32 pad;
    s32 checksumCount;
    u8 *header;
    s32 j;
    header = mempool_alloc_safe(sizeof(LevelHeader), COLOUR_TAG_YELLOW);
    gTempAssetTable = (s32 *) asset_table_load(ASSET_LEVEL_HEADERS_TABLE);
    i = 0;
    while (i < 16) {
        gRaceTypeCountTable[i++] = 0;
    }
    gNumberOfLevelHeaders = 0;
    while (gTempAssetTable[gNumberOfLevelHeaders] != -1) {
        gNumberOfLevelHeaders++;
    }
    gNumberOfLevelHeaders--;
    gGlobalLevelTable = mempool_alloc_safe(gNumberOfLevelHeaders * sizeof(LevelGlobalData), COLOUR_TAG_YELLOW);
    gCurrentLevelHeader = (LevelHeader *) header;
    gNumberOfWorlds = -1;
    for (i = 0; i < gNumberOfLevelHeaders; i++) {
        asset_load(ASSET_LEVEL_HEADERS, (uintptr_t)gCurrentLevelHeader, gTempAssetTable[i], sizeof(LevelHeader));
#ifdef NATIVE_PORT
        asset_swap_normalize(ASSET_LEVEL_HEADERS, gCurrentLevelHeader, sizeof(LevelHeader));
#endif
        if (gNumberOfWorlds < gCurrentLevelHeader->world) {
            gNumberOfWorlds = gCurrentLevelHeader->world;
        }
        if ((gCurrentLevelHeader->race_type >= 0) && (gCurrentLevelHeader->race_type < 16)) {
            gRaceTypeCountTable[gCurrentLevelHeader->race_type]++;
        }
        gGlobalLevelTable[i].world = gCurrentLevelHeader->world;
        gGlobalLevelTable[i].raceType = gCurrentLevelHeader->race_type;
        gGlobalLevelTable[i].vehicles = ((u16) gCurrentLevelHeader->available_vehicles) << 4;
        gGlobalLevelTable[i].vehicles |= gCurrentLevelHeader->vehicle & 0xF;
        gGlobalLevelTable[i].unk3 = 1;
        gGlobalLevelTable[i].unk4 = gCurrentLevelHeader->unkB0;
    }
#ifdef NATIVE_PORT
    /*
     * TEST HOOK (trace only) -- publish the game's own reading of every level's
     * default and available vehicles, exactly as leveltable_vehicle_default() /
     * leveltable_vehicle_usable() will decode it out of gGlobalLevelTable.vehicles.
     *
     * Why: tests/check_vehicle_sweep.py has to know which (track, vehicle) pairs are
     * legitimate before it can sweep them, and it derives that INDEPENDENTLY from the
     * ROM's LevelHeader bytes. Printing what the game believes turns "the sweep agrees
     * with its own assumption" into a real cross-check -- if the header decode, the
     * <<4 packing or the asset offsets ever drift, the two disagree and the sweep
     * fails loudly instead of quietly sweeping the wrong matrix. Costs one line per
     * level, once, and only under MDKR_TRACE.
     */
    {
        if (mdkr_trace_enabled()) {
            for (j = 0; j < gNumberOfLevelHeaders; j++) {
                mdkr_trace("level_vehicles: id=%d default=%d avail=0x%x type=%d world=%d",
                           (int) j,
                           (int) (gGlobalLevelTable[j].vehicles & 0xF),
                           (int) ((gGlobalLevelTable[j].vehicles >> 4) & 0xF),
                           (int) gGlobalLevelTable[j].raceType,
                           (int) gGlobalLevelTable[j].world);
            }
        }
    }
#endif
    gNumberOfWorlds++;
    D_80121178 = mempool_alloc_safe(gNumberOfWorlds, COLOUR_TAG_YELLOW);
    for (i = 0; i < gNumberOfWorlds; i++) {
        D_80121178[i] = -1;
    }
    for (i = 0; i < gNumberOfLevelHeaders; i++) {
        if (gGlobalLevelTable[i].raceType == 5) {
            D_80121178[gGlobalLevelTable[i].world] = i;
        }
    }
    mempool_free(gTempAssetTable);
    mempool_free(header);
    gTempAssetTable = (s32 *) asset_table_load(ASSET_LEVEL_NAMES_TABLE);
    for (i = 0; gTempAssetTable[i] != (-1); i++) {}
    i--;
    size = gTempAssetTable[i] - gTempAssetTable[0];
    /* LP64: gLevelNames is char **, so the block must be sized by the real
     * pointer width. sizeof(void *) == sizeof(s32) on N64, so the ROM-side
     * allocation size is unchanged. */
    gLevelNames = mempool_alloc_safe(i * sizeof(void *), COLOUR_TAG_YELLOW);
    gTempLevelNames = mempool_alloc_safe(size, COLOUR_TAG_YELLOW);
    asset_load(ASSET_LEVEL_NAMES, (uintptr_t)gTempLevelNames, 0, size);
    for (size = 0; size < i; size++) {
        gLevelNames[size] = (char *) &gTempLevelNames[gTempAssetTable[size]];
    }
    mempool_free(gTempAssetTable);
    // Antipiracy measure
#ifdef ANTI_TAMPER
    checksumCount = 0;
    for (j = 0; j < gViewportFuncLength; j++) {
        checksumCount += ((u8 *) (&viewport_rsp_set))[j];
    }
    if (checksumCount != gViewportFuncChecksum) {
        drm_disable_input();
    }
#endif
}

UNUSED s16 func_8006ABB4(s32 levelID) {
    if (levelID < 0) {
        return 0xE10;
    }
    if (levelID >= gNumberOfLevelHeaders) {
        return 0xE10;
    }
    return gGlobalLevelTable[levelID].unk4;
}

/**
 * Iterates through the level property table and attempts to find a level ID that matches the properties you want.
 * Iterates Forwards.
 */
UNUSED s32 search_level_properties_forwards(s32 levelID, s8 raceType, s8 worldID) {
    if (levelID < 0) {
        levelID = 0;
    } else {
        levelID++;
    }
    if (raceType != RACETYPE_CHALLENGE) {
        if (worldID == -1) {
            for (; levelID < gNumberOfLevelHeaders; levelID++) {
                if (raceType == gGlobalLevelTable[levelID].raceType) {
                    return levelID;
                }
            }
        } else if (raceType == -1) {
            for (; levelID < gNumberOfLevelHeaders; levelID++) {
                if (worldID == gGlobalLevelTable[levelID].world) {
                    return levelID;
                }
            }
        } else {
            for (; levelID < gNumberOfLevelHeaders; levelID++) {
                if ((raceType == gGlobalLevelTable[levelID].raceType) &&
                    (worldID == gGlobalLevelTable[levelID].world)) {
                    return levelID;
                }
            }
        }
    } else {
        if (worldID == -1) {
            for (; levelID < gNumberOfLevelHeaders; levelID++) {
                if (gGlobalLevelTable[levelID].raceType & RACETYPE_CHALLENGE) {
                    return levelID;
                }
            }
        } else {
            for (; levelID < gNumberOfLevelHeaders; levelID++) {
                if ((gGlobalLevelTable[levelID].raceType & RACETYPE_CHALLENGE) &&
                    (worldID == gGlobalLevelTable[levelID].world)) {
                    return levelID;
                }
            }
        }
    }
    return -1;
}

/**
 * Iterates through the level property table and attempts to find a level ID that matches the properties you want.
 * Iterates Backwards.
 */
UNUSED s32 search_level_properties_backwards(s32 levelID, s8 raceType, s8 worldID) {
    if (levelID >= gNumberOfLevelHeaders) {
        levelID = gNumberOfLevelHeaders;
    }
    levelID--;
    if (raceType != RACETYPE_CHALLENGE) {
        if (worldID == -1) {
            for (; levelID >= 0; levelID--) {
                if (raceType == gGlobalLevelTable[levelID].raceType) {
                    return levelID;
                }
            }
        } else if (raceType == -1) {
            for (; levelID >= 0; levelID--) {
                if (worldID == gGlobalLevelTable[levelID].world) {
                    return levelID;
                }
            }
        } else {
            for (; levelID >= 0; levelID--) {
                if ((raceType == gGlobalLevelTable[levelID].raceType) &&
                    (worldID == gGlobalLevelTable[levelID].world)) {
                    return levelID;
                }
            }
        }
    } else {
        if (worldID == -1) {
            for (; levelID >= 0; levelID--) {
                if (gGlobalLevelTable[levelID].raceType & RACETYPE_CHALLENGE) {
                    return levelID;
                }
            }
        } else {
            for (; levelID >= 0; levelID--) {
                if ((gGlobalLevelTable[levelID].raceType & RACETYPE_CHALLENGE) &&
                    (worldID == gGlobalLevelTable[levelID].world)) {
                    return levelID;
                }
            }
        }
    }
    return -1;
}

/**
 * Return the number of tracks that aren't challenge maps.
 */
UNUSED s32 leveltable_non_challenge_count(s8 raceType) {
    if (raceType >= RACETYPE_DEFAULT && raceType < 16) {
        return gRaceTypeCountTable[raceType];
    }
    return 0;
}

/**
 * Returns the number of levels that belong to one hub world.
 */
UNUSED s32 leveltable_world_level_count(s8 worldID) {
    s32 out, i;
    out = 0;
    for (i = 0; i < gNumberOfLevelHeaders; i++) {
        if (worldID == gGlobalLevelTable[i].world) {
            out++;
        }
    }
    return out;
}

/**
 * Returns the default vehicle from the set map ID.
 */
Vehicle leveltable_vehicle_default(s32 mapId) {
    if (mapId > 0 && mapId < gNumberOfLevelHeaders) {
        return gGlobalLevelTable[mapId].vehicles & 0xF;
    }
    return VEHICLE_CAR;
}

/**
 * Returns the available vehicles from the set map ID.
 */
s32 leveltable_vehicle_usable(s32 mapId) {
    if (mapId > 0 && mapId < gNumberOfLevelHeaders) {
        s32 temp = gGlobalLevelTable[mapId].vehicles;
        if (temp != 0) {
            return (temp >> 4) & 0xF;
        }
    }
    return (1 << VEHICLE_CAR);
}

/**
 * Returns the race type from the set map ID.
 */
s8 leveltable_type(s32 mapId) {
    if (mapId >= 0 && mapId < gNumberOfLevelHeaders) {
        return gGlobalLevelTable[mapId].raceType;
    }
    return -1;
}

/**
 * Returns the world ID from the set map ID.
 */
s8 leveltable_world(s32 mapId) {
    if (mapId >= 0 && mapId < gNumberOfLevelHeaders) {
        return gGlobalLevelTable[mapId].world;
    }
    return 0;
}

/**
 * Returns the ID of the current hub world. Example: Dino Domain.
 */
s32 level_world_id(s32 worldId) {
    s8 *hubAreaIds;

    if (worldId < 0 || worldId >= gNumberOfWorlds) {
        worldId = 0;
    }
    hubAreaIds = (s8 *) get_misc_asset(ASSET_MISC_HUB_AREA_IDS);

    return hubAreaIds[worldId];
}

/**
 * Writes the level and hub count to the two arguments passed through.
 */
void level_count(s32 *outLevelCount, s32 *outWorldCount) {
    *outLevelCount = gNumberOfLevelHeaders;
    *outWorldCount = gNumberOfWorlds;
}

/**
 * Returns true if the current event is a regular race or a boss race.
 * Returns false if it's a menu, challenge or hubworld.
 */
s32 level_is_race(void) {
    return gIsInRace;
}

/**
 * Loads and sets up the level header, then loads and sets of the level geometry.
 * Sets weather, fog and active cutscenes where applicable.
 * Official Name: levelInit
 */
void level_load(s32 levelId, s32 numberOfPlayers, s32 entranceId, Vehicle vehicleId, s32 cutsceneId) {
    s8 *someAsset;
    s32 i;
#ifdef NATIVE_PORT
    /*
     * TEST HOOK -- MDKR_LOAD_TRACK=<levelId>[:<vehicle>] races a DIFFERENT track
     * than the one the menu route selected. No-op unless set (same contract as
     * MDKR_FORCE_BOOST / MDKR_FORCE_LAPS / MDKR_AUTOPILOT).
     *
     * Why: everything validated so far is ONE track in ONE vehicle (Ancient Lake,
     * car). There are 20 playable tracks (ASSET_MISC_MAIN_TRACKS_IDS) and three
     * player vehicles, and this port's recurring failure mode is a per-track asset
     * that decodes wrong and fails SILENTLY -- the boost table, the ASSET_MISC_8
     * steer divisor and the collision facets were all that shape. Reaching each
     * track through the menus would need 20 hand-tuned input scripts; this lets one
     * proven route (tests/input_scripts/race_full_3lap_tt.txt) be retargeted, so a
     * sweep is a loop instead of 20 fixtures.
     *
     * Targeting: rewrite only the load DKR itself considers "the track to race" --
     * gTrackIdToLoad is the game's own global for that (menu.c sets it from
     * gTrackIdForPreview). Matching against it picks the race load and leaves every
     * menu-background load alone, as well as the track-select PREVIEW, which passes
     * a non-zero cutsceneId. Reading that global needs no change to menu.c.
     */
    {
        extern int mdkr_force_track(int *vehicleOut);
        extern s32 gTrackIdToLoad; /* menu.c: the track DKR is about to race */
        int forcedVehicle = -1;
        int forcedTrack = mdkr_force_track(&forcedVehicle);
        if (forcedTrack >= 0 && numberOfPlayers >= 0 && cutsceneId == 0 && levelId == gTrackIdToLoad) {
            levelId = forcedTrack;
            if (forcedVehicle >= 0) {
                vehicleId = (Vehicle) forcedVehicle;
            }
        }
    }
    {
        extern int g_frameCounter;
        if (mdkr_trace_enabled()) {
            mdkr_trace("level_load: levelId=%d numPlayers=%d entrance=%d vehicle=%d cutscene=%d @frame~%d",
                       (int) levelId, (int) numberOfPlayers, (int) entranceId, (int) vehicleId, (int) cutsceneId,
                       g_frameCounter);
        }
    }
#endif
    s32 size;
    s32 var_s0;
    s32 wizpig;
    s32 numPlayers;
    s32 prevLevelID;
    Settings *settings;
    s32 offset;

    rumble_kill();
    if (cutsceneId == -1) {
        cutsceneId = CUTSCENE_NONE;
    }
    if (numberOfPlayers == ZERO_PLAYERS) {
        numPlayers = 1;
        numberOfPlayers = ONE_PLAYER;
    } else {
        numPlayers = 0;
    }

    if (numberOfPlayers == ONE_PLAYER) {
        sndp_set_active_sound_limit(8);
    } else if (numberOfPlayers == TWO_PLAYERS) {
        sndp_set_active_sound_limit(12);
    } else {
        sndp_set_active_sound_limit(16);
    }
    settings = get_settings();
    gTempAssetTable = (s32 *) asset_table_load(ASSET_LEVEL_HEADERS_TABLE);

    for (i = 0; gTempAssetTable[i] != -1; i++) {}
    i--;
    /* levelId indexes gTempAssetTable[levelId] and [levelId + 1] below, so both
     * ends of the range have to hold, not just the upper one. */
    if (levelId < 0 || levelId >= i) {
        stubbed_printf("LOADLEVEL Error: Level out of range\n");
        levelId = ASSET_LEVEL_CENTRALAREAHUB;
    }

    offset = gTempAssetTable[levelId];
    size = gTempAssetTable[levelId + 1] - offset;
    gCurrentLevelHeader = (LevelHeader *) mempool_alloc_safe(size, COLOUR_TAG_YELLOW);
    asset_load(ASSET_LEVEL_HEADERS, (uintptr_t)gCurrentLevelHeader, offset, size);
#ifdef NATIVE_PORT
    asset_swap_normalize(ASSET_LEVEL_HEADERS, gCurrentLevelHeader, (u32) size);
#endif
    D_800DD330 = 0;
    prevLevelID = levelId;
    if (gCurrentLevelHeader->race_type == RACETYPE_DEFAULT) {
        level_properties_reset();
    }
    if (level_properties_get() == 0 && D_800DD32C == 0) {
        if (gCurrentLevelHeader->race_type == RACETYPE_BOSS) {
            var_s0 = settings->courseFlagsPtr[levelId];
            wizpig = FALSE;
            if (gCurrentLevelHeader->world == WORLD_CENTRAL_AREA ||
                gCurrentLevelHeader->world == WORLD_FUTURE_FUN_LAND) {
                wizpig = TRUE;
            }
            if (!(var_s0 & 1) || wizpig) {
                level_properties_push(levelId, entranceId, vehicleId, cutsceneId);
                if (settings->bosses & (1 << settings->worldId)) {
                    cutsceneId = CUTSCENE_ID_UNK_7;
                } else {
                    cutsceneId = CUTSCENE_ID_UNK_3;
                }
                if (wizpig) {
                    cutsceneId = 0;
                    if (var_s0 & 1) {
                        D_800DD330 = 2;
                    }
                }
                someAsset = (s8 *) get_misc_asset(ASSET_MISC_67);
#ifdef NATIVE_PORT
                /* ASSET_MISC_67 is an unterminated array of (bossLevelId,
                 * cutsceneLevelId) byte pairs; a levelId that is absent from it
                 * walks past the sub-asset. Its own byte length is the only
                 * bound, and a miss must leave levelId as the boss level. */
                {
                    s32 bossTableSize = get_misc_asset_size(ASSET_MISC_67);

                    for (var_s0 = 0; var_s0 + 1 < bossTableSize && levelId != someAsset[var_s0]; var_s0 += 2) {}
                    if (var_s0 + 1 < bossTableSize) {
                        levelId = someAsset[var_s0 + 1];
                    }
                }
#else
                for (var_s0 = 0; levelId != someAsset[var_s0]; var_s0 += 2) {}
                levelId = someAsset[var_s0 + 1];
#endif
                entranceId = cutsceneId;
#ifdef NATIVE_PORT
                {
                    mdkr_trace("bossredirect: boss=%d -> cutsceneLevel=%d cutsceneId=%d bosses=0x%x courseFlags=0x%x "
                               "worldId=%d",
                               (int) prevLevelID, (int) levelId, (int) cutsceneId, (unsigned) settings->bosses,
                               (unsigned) settings->courseFlagsPtr[prevLevelID], (int) settings->worldId);
                }
#endif
                if (cutsceneId == CUTSCENE_NONE) {
                    stubbed_printf("BossLev problem\n");
                }
            }
        }
        if (gCurrentLevelHeader->race_type == RACETYPE_HUBWORLD) {
            if (gCurrentLevelHeader->world > WORLD_CENTRAL_AREA && gCurrentLevelHeader->world < WORLD_FUTURE_FUN_LAND) {
                var_s0 = gCurrentLevelHeader->world;
#ifdef NATIVE_PORT
                /* DKR_SHL32: the ROM writes this as `<< (var_s0 + 31)` and relies on
                 * MIPS masking the shift count to 5 bits. In C that count is >= 32,
                 * i.e. UB, and clang folds it to 0 at -O2 (the browser build is
                 * Release) -- which makes the `&` test always true and the `|=` a
                 * no-op, so the cutscene replays after every race and is never
                 * recorded. See macros.h and docs/OPEN_ITEMS.md wave "keyshift". */
                {
                    /* Gated: mdkr_trace() is an unconditional fflush'd stderr write and
                     * this point is reached on every world-hub load. */
                    if (mdkr_trace_enabled()) {
                        mdkr_trace("keycutscene: world=%d keys=0x%x cutsceneFlags=0x%x mask=0x%x",
                                   (int) var_s0, (unsigned) settings->keys,
                                   (unsigned) settings->cutsceneFlags,
                                   (unsigned) DKR_SHL32(CUTSCENE_DINO_DOMAIN_KEY, var_s0 + 31));
                    }
                }
#endif
                if (settings->keys & (1 << var_s0) &&
                    !(settings->cutsceneFlags & DKR_SHL32(CUTSCENE_DINO_DOMAIN_KEY, var_s0 + 31))) {
                    // Trigger World Key unlocking Challenge Door cutscene.
                    level_properties_push(levelId, entranceId, vehicleId, cutsceneId);
                    settings->cutsceneFlags |= DKR_SHL32(CUTSCENE_DINO_DOMAIN_KEY, var_s0 + 31);
                    someAsset = (s8 *) get_misc_asset(ASSET_MISC_68);
                    levelId = someAsset[var_s0 - 1];
                    entranceId = 0;
                    cutsceneId = CUTSCENE_ID_UNK_5;
                }
            }
        }
        if (gCurrentLevelHeader->race_type == RACETYPE_HUBWORLD && gCurrentLevelHeader->world == WORLD_CENTRAL_AREA &&
            !(settings->cutsceneFlags & CUTSCENE_WIZPIG_FACE) && settings->wizpigAmulet >= 4) {
            // Trigger wizpig face cutscene
            level_properties_push(levelId, entranceId, vehicleId, cutsceneId);
            entranceId = 0;
            cutsceneId = CUTSCENE_NONE;
            settings->cutsceneFlags |= CUTSCENE_WIZPIG_FACE;
            levelId = ((s8 *) get_misc_asset(ASSET_MISC_68))[4];
#ifdef NATIVE_PORT
            /* NATIVE_PORT, read-only: this is the Wizpig 1 unlock -- four amulet
             * pieces turn the next Timber's Island load into the Wizpig mouth
             * sequence, which is what opens the central boss.
             *
             * It has to be reported HERE because the `level_load:` trace above
             * has already printed the level that was *asked* for, and this branch
             * rewrites it afterwards. Reading that trace alone, the redirect is
             * invisible: the hub is requested, silently replaced by the mouth
             * sequence, and then popped back off the level-properties stack, so
             * the log shows two ordinary hub loads and no sign of the unlock.
             * (That misreading is exactly why this seam was first reported as
             * "did not fire".) One line per save file, since the flag it sets is
             * the branch's own guard. */
            mdkr_trace("wizpigface: hub=%d -> cutsceneLevel=%d wizpigAmulet=%d cutsceneFlags=0x%x",
                       (int) prevLevelID, (int) levelId, (int) settings->wizpigAmulet,
                       (unsigned) settings->cutsceneFlags);
#endif
        }
    }
    D_800DD32C = 0;
    if (prevLevelID != levelId) {
        mempool_free(gCurrentLevelHeader);
        offset = gTempAssetTable[levelId];
        size = gTempAssetTable[levelId + 1] - offset;
        gCurrentLevelHeader = mempool_alloc_safe(size, COLOUR_TAG_YELLOW);
        asset_load(ASSET_LEVEL_HEADERS, (uintptr_t)gCurrentLevelHeader, offset, size);
#ifdef NATIVE_PORT
        asset_swap_normalize(ASSET_LEVEL_HEADERS, gCurrentLevelHeader, (u32) size);
#endif
    }
    mempool_free(gTempAssetTable);
    aitable_init((s8 *) &gCurrentLevelHeader->AILevelTable);
    func_8000CBC0();
    gMapId = levelId;
    for (var_s0 = 0; var_s0 < 7; var_s0++) {
        if ((s32) gCurrentLevelHeader->unk74[var_s0] != -1) {
#ifdef NATIVE_PORT
            /* Capture the sub-asset index before the slot is overwritten with the
             * resolved pointer: the swap below needs its byte length to bound the
             * entry walk. */
            s32 miscIndex = (s32) gCurrentLevelHeader->unk74[var_s0];
#endif
            gCurrentLevelHeader->unk74[var_s0] =
                DKR_TOK(get_misc_asset((s32) gCurrentLevelHeader->unk74[var_s0]));
#ifdef NATIVE_PORT
            /* MISC section is punted at load (heterogeneous); this LevelHeader_70
             * blob is still big-endian — normalize it before func_8007F1E8 reads
             * its count/entry fields. Deduped against per-level re-swap. */
            asset_swap_misc_lightdata(DKR_PTR(void, gCurrentLevelHeader->unk74[var_s0]),
                                      (u32) get_misc_asset_size(miscIndex));
#endif
            func_8007F1E8(DKR_PTR(LevelHeader_70, gCurrentLevelHeader->unk74[var_s0]));
        }
    }
#ifdef NATIVE_PORT
    /*
     * Publish only after every LevelHeader_70 token has been resolved,
     * endian-normalized and initialized. The rig is stable for the level: live
     * colour-cycle animation remains authored material animation and does not
     * swing the sun every frame.
     */
    mdkr_publish_level_lighting(gCurrentLevelHeader);
#endif

    if (cutsceneId == CUTSCENE_ID_UNK_64) {
        if (get_trophy_race_world_id() != 0) {
            if (gCurrentLevelHeader->race_type == RACETYPE_DEFAULT) {
                cutsceneId = CUTSCENE_NONE;
            }
        } else if (is_in_tracks_mode() == 1) {
            if (gCurrentLevelHeader->race_type == RACETYPE_DEFAULT) {
                cutsceneId = CUTSCENE_NONE;
            }
        }
    }
    if (gCurrentLevelHeader->race_type == RACETYPE_DEFAULT || gCurrentLevelHeader->race_type == RACETYPE_BOSS) {
        gIsInRace = TRUE;
    } else {
        gIsInRace = FALSE;
    }
    if (numPlayers && gCurrentLevelHeader->race_type != RACETYPE_CUTSCENE_2) {
        gCurrentLevelHeader->race_type = RACETYPE_CUTSCENE_1;
    }
    music_voicelimit_set(gCurrentLevelHeader->voiceLimit);
    music_volume_reset();
    lights_init(32);
    var_s0 = VEHICLE_CAR;
    if (vehicleId >= VEHICLE_CAR && vehicleId < NUMBER_OF_PLAYER_VEHICLES) {
        var_s0 = gCurrentLevelHeader->unk4F[vehicleId];
    }
    set_taj_challenge_type(var_s0);
    var_s0 = settings->worldId;
    if (gCurrentLevelHeader->world != -1) {
        settings->worldId = gCurrentLevelHeader->world;
    }
    settings->courseId = levelId;
#ifdef NATIVE_PORT
    if (mdkr_trace_enabled() && gIsInRace) {
        mdkr_trace("adventure_mode: level=%d adventureTwo=%d mirrored=%d saveFlags=0x%x",
                   (int) levelId, (int) is_in_adventure_two(),
                   (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) != 0,
                   (unsigned) settings->cutsceneFlags);
    }
#endif
    if (var_s0 == WORLD_CENTRAL_AREA && settings->worldId > 0) {
        gCurrentDefaultVehicle = get_level_default_vehicle();
    }
    if (settings->worldId == WORLD_CENTRAL_AREA && var_s0 > 0 && gCurrentDefaultVehicle != -1) {
        vehicleId = gCurrentDefaultVehicle;
    }
    set_vehicle_id_for_menu(vehicleId);
    if (gCurrentLevelHeader->race_type == RACETYPE_HUBWORLD) {
        if (settings->worldId - 1 >= 0) {
            /* DKR_SHL32 (NATIVE_PORT): same MIPS `sllv` masking the key-cutscene gate
             * above depends on -- worldId is 1..5 here, so the ROM's `8 << (worldId +
             * 31)` means CUTSCENE_DINO_DOMAIN_BOSS << (worldId - 1). Left as a plain
             * C shift it is UB and clang folds it to 0 at -O2, which makes this
             * "boss door is now open" cutscene replay on every entry to the world hub
             * and never record itself. Measured, same run as the key cutscene. */
            var_s0 = DKR_SHL32(8, settings->worldId + 31);
#ifdef NATIVE_PORT
            {
                if (mdkr_trace_enabled()) {   /* every world-hub load; see above */
                    mdkr_trace("bosscutscene: worldId=%d balloons=%d mask=0x%x cutsceneFlags=0x%x",
                               (int) settings->worldId, (int) settings->balloonsPtr[settings->worldId],
                               (unsigned) var_s0, (unsigned) settings->cutsceneFlags);
                }
            }
#endif
            if (settings->worldId == 5) {
                if (settings->balloonsPtr[0] >= 47) {
                    if (settings->ttAmulet >= 4) {
                        if ((settings->cutsceneFlags & var_s0) == 0) {
                            settings->cutsceneFlags |= var_s0;
                            cutsceneId = CUTSCENE_ID_UNK_5;
                        }
                    }
                }
            } else {
                if (settings->balloonsPtr[settings->worldId] >= 4) {
                    if (!(settings->cutsceneFlags & var_s0)) {
                        settings->cutsceneFlags |= var_s0;
                        cutsceneId = CUTSCENE_ID_UNK_5;
                    }
                }
                var_s0 <<= 5;
                if (settings->balloonsPtr[settings->worldId] >= 8) {
                    if (!(settings->cutsceneFlags & var_s0)) {
                        settings->cutsceneFlags |= var_s0;
                        cutsceneId = CUTSCENE_ID_UNK_5;
                    }
                }
            }
        }
    }

    var_s0 = settings->courseFlagsPtr[levelId]; // Redundant
    if (numberOfPlayers != ONE_PLAYER && gCurrentLevelHeader->race_type == RACETYPE_DEFAULT) {
        cutsceneId = CUTSCENE_ID_UNK_64;
    }
    if ((gCurrentLevelHeader->race_type == RACETYPE_DEFAULT || gCurrentLevelHeader->race_type & RACETYPE_CHALLENGE) &&
        is_in_two_player_adventure()) {
        gTwoPlayerAdvRace = TRUE;
        cutsceneId = CUTSCENE_ID_UNK_64;
    } else {
        gTwoPlayerAdvRace = FALSE;
    }
    if (gCurrentLevelHeader->race_type == RACETYPE_DEFAULT && numPlayers == 0 && is_time_trial_enabled()) {
        cutsceneId = CUTSCENE_ID_UNK_64;
    }
    cutscene_id_set(cutsceneId);
    init_track(gCurrentLevelHeader->geometry, gCurrentLevelHeader->skybox, numberOfPlayers, vehicleId, entranceId,
               gCurrentLevelHeader->collectables, gCurrentLevelHeader->unkBA);
    if (gCurrentLevelHeader->fogNear == 0 && gCurrentLevelHeader->fogFar == 0 && gCurrentLevelHeader->fogR == 0 &&
        gCurrentLevelHeader->fogG == 0 && gCurrentLevelHeader->fogB == 0) {
        for (var_s0 = 0; var_s0 < 4; var_s0++) {
            reset_fog(var_s0);
        }
    } else {
        for (var_s0 = 0; var_s0 < 4; var_s0++) {
            set_fog(var_s0, gCurrentLevelHeader->fogNear, gCurrentLevelHeader->fogFar, (u8) gCurrentLevelHeader->fogR,
                    gCurrentLevelHeader->fogG, gCurrentLevelHeader->fogB);
        }
    }
    settings = get_settings();
    if (gCurrentLevelHeader->world != -1) {
        settings->worldId = gCurrentLevelHeader->world;
    }
    settings->courseId = levelId;
    if (gCurrentLevelHeader->weatherEnable > 0) {
        weather_reset(gCurrentLevelHeader->weatherType, gCurrentLevelHeader->weatherEnable,
                      gCurrentLevelHeader->weatherVelX << 8, gCurrentLevelHeader->weatherVelY << 8,
                      gCurrentLevelHeader->weatherVelZ << 8, gCurrentLevelHeader->weatherIntensity * 257,
                      gCurrentLevelHeader->weatherOpacity * 257);
        weather_clip_planes(-1, -512);
    }
    if (gCurrentLevelHeader->skyDome == -1) {
        gCurrentLevelHeader->unkA4 = DKR_TOK(load_texture((s32) gCurrentLevelHeader->unkA4));
        gCurrentLevelHeader->unkA8 = 0;
        gCurrentLevelHeader->unkAA = 0;
    }
    if ((s32) gCurrentLevelHeader->pulseLightData != -1) {
#ifdef NATIVE_PORT
        /* Capture the misc index before it is overwritten with the resolved
         * token — the swapper needs the sub-asset's byte length to bound its
         * frame walk. */
        s32 pulseMiscIndex = (s32) gCurrentLevelHeader->pulseLightData;
#endif
        gCurrentLevelHeader->pulseLightData =
            DKR_TOK(get_misc_asset((s32) gCurrentLevelHeader->pulseLightData));
#ifdef NATIVE_PORT
        /* Same punt as the LevelHeader_70 loop above: ASSET_MISC is not
         * normalized at load, so this PulsatingLightData record is still
         * big-endian, and every one of its fields is multi-byte. Left as-is,
         * init_pulsating_light_data() reads numberFrames byte-reversed (us.v80
         * misc sub-asset 64, used by Spaceport Alpha and Star City: 4 -> 1024)
         * and accumulates totalTime over 1020 entries past the end of a
         * 28-byte blob, so outColorValue — the pulsing-light PrimColor for
         * RENDER_PULSING_LIGHTS batches — comes from out-of-bounds memory.
         * Deduped by blob pointer against per-level re-swap. */
        asset_swap_misc_pulsating(DKR_PTR(void, gCurrentLevelHeader->pulseLightData),
                                  (u32) get_misc_asset_size(pulseMiscIndex));
#endif
        init_pulsating_light_data(DKR_PTR(PulsatingLightData, gCurrentLevelHeader->pulseLightData));
    }
    cam_set_fov(gCurrentLevelHeader->cameraFOV);
    bgdraw_primcolour(gCurrentLevelHeader->bgColorRed, gCurrentLevelHeader->bgColorGreen,
                      gCurrentLevelHeader->bgColorBlue);
    video_delta_reset();
    func_8007AB24(gCurrentLevelHeader->unk4[numberOfPlayers]);
#ifdef NATIVE_PORT
    /* The renderer's stage boundary must fire on EVERY level load, not only
     * under diagnostics: the shadow static caster cache and stage-generation
     * counters are reset here. This call used to sit inside the trace gate
     * below, so shipping builds (no MDKR_TRACE / MDKR_RESOURCE_STATS) carried
     * every previous level's static casters into the next level and recycled
     * arena addresses false-dedup'ed the new level's geometry out of the
     * shadow map. check_shadow_stage_reset.py holds both arms equal. */
    gfx_dkr_resource_generation_begin(levelId, numberOfPlayers, cutsceneId);
    /* Spec §5: "Level transitions reset snapshot history so interpolation
     * never crosses two unrelated scenes." Same stage boundary as the
     * renderer's resource generation, and for the same reason — the object
     * pool is about to be torn down and every recycled address is a
     * coincidence. No-op unless MDKR_PRESENT_SNAPSHOT is set. */
    presentation_snapshot_stage_reset();
    if (mdkr_resource_trace_enabled()) {
        MdkrMemoryPoolStats memoryStats;
        u32 audioUsed;
        u32 audioCapacity;
        u32 audioAllocations;
        MdkrAudioVoiceStats voiceStats;
        u32 voicePeakPhysical;
        u32 voicePeakMusic;
        if (mdkr_mempool_stats(POOL_MAIN, &memoryStats)) {
            mdkr_audio_heap_stats(
                &audioUsed, &audioCapacity, &audioAllocations);
            mdkr_audio_voice_stats(&voiceStats);
            /* voicePeak is the high-water of live physical/music voice
             * ownership over the generation that just ended, read-and-cleared
             * here so each row's window is exactly one generation. The
             * instantaneous voicePhys/voiceMusic fields beside it are a
             * boundary sample and CANNOT show music ownership: the teardown
             * above ran music_stop() -> alCSPStop(), which in the clean-room
             * engine releases every music voice synchronously (the SGI one
             * posted AL_SEQP_STOPPING_EVT and unwound later, which is why the
             * boundary sample used to catch them). */
            mdkr_audio_voice_peaks_take(&voicePeakPhysical, &voicePeakMusic);
            mdkr_trace(
                "resource_state: level=%d players=%d cutscene=%d "
                "mainLive=%d mainUsed=%d mainFree=%d mainLargest=%d "
                "audioUsed=%u audioCapacity=%u audioAllocs=%u "
                "voicePhys=%u/%u/%u voiceMusic=%u/%u "
                "voiceJingle=%u/%u sfxStates=%u/%u voicePeak=%u/%u "
                "voiceValid=%u",
                (int)levelId, (int)numberOfPlayers, (int)cutsceneId,
                (int)memoryStats.liveSlots, (int)memoryStats.usedBytes,
                (int)memoryStats.freeBytes,
                (int)memoryStats.largestFreeBytes, (unsigned)audioUsed,
                (unsigned)audioCapacity, (unsigned)audioAllocations,
                (unsigned)voiceStats.physicalAllocated,
                (unsigned)voiceStats.physicalFree,
                (unsigned)voiceStats.physicalLame,
                (unsigned)voiceStats.musicAllocated,
                (unsigned)voiceStats.musicFree,
                (unsigned)voiceStats.jingleAllocated,
                (unsigned)voiceStats.jingleFree,
                (unsigned)voiceStats.sfxAllocated,
                (unsigned)voiceStats.sfxFree,
                (unsigned)voicePeakPhysical, (unsigned)voicePeakMusic,
                (unsigned)voiceStats.valid);
        }
    }
#endif
}

/**
 * If the level's music ID is nonzero, set the current background music.
 */
void level_music_start(f32 tempo) {
    if (gCurrentLevelHeader->music != SEQUENCE_NONE) {
        music_channel_reset_all();
        music_play(gCurrentLevelHeader->music);
        music_tempo_set_relative(tempo);
        music_dynamic_set(gCurrentLevelHeader->instruments);
    }
}

/**
 * Return the current map ID.
 */
s32 level_id(void) {
    return gMapId;
}

/**
 * Return the race type ID of the current level.
 * Official name: levelGetType
 */
u8 level_type(void) {
    return gCurrentLevelHeader->race_type;
}

/**
 * Return the header data of the current level.
 * Official Name: levelGetLevel
 */
LevelHeader *level_header(void) {
    return gCurrentLevelHeader;
}

/**
 * Returns the amount of level headers there are in the game.
 */
UNUSED u8 level_header_count(void) {
    return gNumberOfLevelHeaders - 1;
}

/**
 * Returns the name of the level from the passed ID
 */
char *level_name(s32 levelId) {
    char *levelName;
    u8 numberOfNullPointers = 0;

    if (levelId < 0 || levelId >= gNumberOfLevelHeaders) {
        return NULL;
    }

    levelName = gLevelNames[levelId];
    switch (get_language()) {
        case LANGUAGE_GERMAN:
            while (numberOfNullPointers < 1) {
                if (*(levelName++) == 0) {
                    numberOfNullPointers++;
                }
            }
            break;
        case LANGUAGE_FRENCH:
            while (numberOfNullPointers < 2) {
                if (*(levelName++) == 0) {
                    numberOfNullPointers++;
                }
            }
            break;
        case LANGUAGE_JAPANESE:
            while (numberOfNullPointers < 3) {
                if (*(levelName++) == 0) {
                    numberOfNullPointers++;
                }
            }
            break;
    }
    return levelName;
}

/**
 * Call multiple functions to stop and free audio, then free track, weather and wave data.
 */
void level_free(void) {
    LevelHeader *levelHeader = gCurrentLevelHeader;
    TextureHeader *skyTexture = NULL;
    s32 weatherEnabled = FALSE;

    if (levelHeader != NULL) {
        weatherEnabled = levelHeader->weatherEnable > 0;
        if (levelHeader->skyDome == -1) {
            skyTexture = DKR_PTR(TextureHeader, levelHeader->unkA4);
        }
    }
    aitable_free();
    bgdraw_primcolour(0, 0, 0);
    sndp_stop_all_looped();
    music_stop();
    music_jingle_stop();
    music_channel_reset_all();
    lights_free();
    free_track();
    audspat_reset();
    sound_volume_change(VOLUME_NORMAL);
    if (weatherEnabled) {
        weather_free();
    }
    if (skyTexture != NULL) {
        tex_free(skyTexture);
    }
    if (levelHeader != NULL) {
        mempool_free(levelHeader);
        gCurrentLevelHeader = NULL;
    }
#ifdef NATIVE_PORT
    /*
     * unload_level_game() drains the old graphics task before reaching here.
     * Display lists are rebuilt and re-register their live host pointers on the
     * next frame, so retaining prior-stage token mappings only preserves stale
     * addresses and tombstones.
     */
    gfx_dkr_stage_resources_released();
#endif
}

/**
 * Set the skill level of the AI.
 * Apply offsets based on game mode.
 */
void aitable_init(s8 *aiLevelTable) {
    s32 temp;
    UNUSED s32 temp2;
    s16 tableIndexCount;
    s8 aiLevel;
    Settings *settings;

    aiLevel = 0;
    if (is_in_tracks_mode() == FALSE) {
        settings = get_settings();
        temp = settings->courseFlagsPtr[settings->courseId];
        if (temp & 2) {
            aiLevel = 1;
        }
        if (temp & 4) {
            aiLevel = 2;
        }
    } else {
        aiLevel = 3;
    }
    if (get_trophy_race_world_id()) {
        aiLevel = 4;
    }
    if (is_in_adventure_two()) {
        aiLevel += 5;
    }
    aiLevel = aiLevelTable[aiLevel];
    if (get_filtered_cheats() & CHEAT_ULTIMATE_AI) {
        aiLevel = 9;
    }
    if (get_game_mode() == GAMEMODE_MENU) {
        aiLevel = 5;
    }
    gTempAssetTable = (s32 *) asset_table_load(ASSET_AI_BEHAVIOUR_TABLE);
    tableIndexCount = 0;
    while (-1 != (s32) gTempAssetTable[tableIndexCount]) {
        tableIndexCount++;
    }
    tableIndexCount--;
    if (aiLevel >= tableIndexCount) {
        stubbed_printf("AITABLE Error: Table out of range\n");
        aiLevel = 0;
    }
    temp2 = gTempAssetTable[aiLevel];
    temp = gTempAssetTable[aiLevel + 1] - temp2;
    gAIBehaviourTable = mempool_alloc_safe(temp, COLOUR_TAG_YELLOW);
    asset_load(ASSET_AI_BEHAVIOUR, (uintptr_t)gAIBehaviourTable, temp2, temp);
#ifdef NATIVE_PORT
    /* asset_load() is the raw ROM DMA path — only asset_table_load() runs the
     * normalize hook, and ASSET_AI_BEHAVIOUR is never loaded that way, so this
     * record is still big-endian. AIBehaviourTable's two leading f32 (racer.h)
     * are the AI difficulty ramp: read byte-reversed they decode to denormals
     * (~1e-41) for every AI level, collapsing the whole ramp to ~0. In
     * update_ai_racer() (racer.c) they feed
     *     sqrtf((v * 0.025 + 0.561) / 0.004)
     * which never NaNs, so the defect is silent — every AI racer behaves as if
     * aiLevel 6 (the 0.0 entry) regardless of the real difficulty. us.v80
     * levels 0..9 decode to -6.0,-5.0,-4.0,-3.0,-1.5,-0.5,0.0,2.0,5.0,7.0.
     * (The s8 percentages[4][4] that follow are byte data and were unaffected.) */
    asset_swap_normalize(ASSET_AI_BEHAVIOUR, gAIBehaviourTable, (u32) temp);
#endif
    mempool_free(gTempAssetTable);
}

/**
 * Frees the AI behaviour table from memory.
 */
void aitable_free(void) {
    mempool_free(gAIBehaviourTable);
}

/**
 * Return the behaviour value table for AI racers.
 */
AIBehaviourTable *aitable_get(void) {
    return gAIBehaviourTable;
}

/**
 * Return whether it is a standard race with two players in adventure mode.
 */
s8 race_is_adventure_2P(void) {
    return gTwoPlayerAdvRace;
}

/**
 * Pushes the current level data onto a stack.
 * Used for preserving certain properties when viewing cutscenes, where this information would otherwise be lost.
 */
void level_properties_push(s32 levelId, s32 entranceId, Vehicle vehicleId, s32 cutsceneId) {
#ifdef NATIVE_PORT
    /* The stack holds five 4-entry sets; a push beyond that writes past
     * gLevelPropertyStack and into the adjacent globals. Cutscene nesting never
     * reaches five in normal play, so dropping the push keeps the stack
     * consistent with the matching pop. */
    if (gLevelPropertyStackPos + 4 > (s32) ARRAY_COUNT(gLevelPropertyStack)) {
        return;
    }
#endif
    gLevelPropertyStack[gLevelPropertyStackPos++] = levelId;
    gLevelPropertyStack[gLevelPropertyStackPos++] = entranceId;
    gLevelPropertyStack[gLevelPropertyStackPos++] = vehicleId;
    gLevelPropertyStack[gLevelPropertyStackPos++] = cutsceneId;
}

/**
 * Reads the level data from the stack, then pops it.
 * Used after cutscenes to properly restore the previous level status.
 */
void level_properties_pop(s32 *levelId, s32 *entranceId, s32 *vehicleId, s32 *cutsceneId) {
    s32 tempVehicleID;

    gLevelPropertyStackPos--;
    *cutsceneId = gLevelPropertyStack[gLevelPropertyStackPos--];
    tempVehicleID = gLevelPropertyStack[gLevelPropertyStackPos--];
    *entranceId = gLevelPropertyStack[gLevelPropertyStackPos--];
    *levelId = gLevelPropertyStack[gLevelPropertyStackPos];

    if (tempVehicleID != -1) {
        *vehicleId = tempVehicleID;
    }

    D_800DD32C = 1;
}

/**
 * Resets the position in the level propert stack, effectively clearing it.
 */
void level_properties_reset(void) {
    gLevelPropertyStackPos = 0;
}

/**
 * Returns the position of the level property stack.
 * Should always return a multiple of 4.
 */
s16 level_properties_get(void) {
    return gLevelPropertyStackPos;
}

s32 func_8006C300(void) {
    if (D_800DD330 >= 2) {
        D_800DD330 = 1;
        return 0;
    } else {
        return D_800DD330;
    }
}
