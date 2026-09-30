#!/usr/bin/env python3
"""Guard the production seams for the runtime-safety repair batch.

The ROM-free unit executable exercises the shared boundary helpers.  This
source census complements it by proving that the real teardown, racer, audio,
texture, token-arithmetic, shift, and model-selection paths still call those
helpers and retain their initialization/order contracts. Each required fragment
is removed in-memory as a positive control so a stale or accidentally weakened
census cannot silently pass.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from harness_utils import resolve_binary


REPO = Path(__file__).resolve().parent.parent

REQUIRED = {
    "game/src/waves.c": (
        "dkrptr32 colourToken = header->unk74[colourID - 1];",
        "colourToken == (dkrptr32)-1 ? NULL : DKR_PTR(LevelHeader_70, colourToken)",
    ),
    "game/src/thread3_main.c": (
        "level_count_for_save(&retailLevels, &retailWorlds);",
        "if (j >= retailLevels) {",
        "settings->flapInitialsPtr[i][j] = 0;",
        "settings->courseInitialsPtr[i][j] = 0;",
    ),
    "game/src/game.c": (
        "(s32)gCurrentLevelHeader->weatherVelX * 256",
        "(s32)gCurrentLevelHeader->weatherVelY * 256",
        "(s32)gCurrentLevelHeader->weatherVelZ * 256",
        "i < gNumberOfLevelHeaders || i <= 0",
        "sLevelNameSizes = (u32 *)(gLevelNames + i);",
        "mdkr_level_name_bounded(gLevelNames[levelId], sLevelNameSizes[levelId], language)",
        "(void)level_header_size_checked(gTempAssetTable, gNumberOfLevelHeaders, i);",
        "size = level_header_size_checked(gTempAssetTable, gNumberOfLevelHeaders, levelId);",
        "table[index + 1] > asset_table_size(ASSET_LEVEL_HEADERS)",
        "sRetailLevelCount = asset_retail_entry_count(ASSET_LEVEL_HEADERS_TABLE, gNumberOfLevelHeaders);",
        "if (i < sRetailLevelCount && sRetailWorldCount <= gCurrentLevelHeader->world) {",
        "*outLevelCount = sRetailLevelCount;",
        "*outWorldCount = sRetailWorldCount;",
        "LevelHeader *levelHeader = gCurrentLevelHeader;",
        "weatherEnabled = levelHeader->weatherEnable > 0;",
        "skyTexture = DKR_PTR(TextureHeader, levelHeader->unkA4);",
        "free_track();",
        "weather_free();",
        "tex_free(skyTexture);",
        "mempool_free(levelHeader);",
        "gCurrentLevelHeader = NULL;",
    ),
    "game/src/tracks.c": (
        "if (numberVertices <= 0 || numberTriangles <= 0) continue;",
        "if (pulseData == NULL || texture == NULL) batchFlags &= ~RENDER_PULSING_LIGHTS;",
        "color = pulseData->outColorValue;",
        "dkrptr32 colourToken = gCurrentLevelHeader2->unk74[levelHeaderIndex - 1];",
        "LevelHeader_70 *lh70 = colourToken == (dkrptr32)-1 ? NULL :",
        "if (lh70 != NULL) {",
        "gCurrentLevelModel = NULL;",
        "gCurrentLevelHeader2 = NULL;",
        "u8 *collisionCursor;",
        "segment->unk10 = DKR_TOK(collisionCursor);",
        "segment->collisionPlanes = DKR_TOK(collisionCursor);",
        "segment->unk34 = DKR_TOK(collisionCursor);",
        "collisionMaxBytes",
    ),
    "game/src/tracks.h": (
        "(ptr) = DKR_TOK((u8 *)(mdl) + (u32)(ptr))",
    ),
    "game/src/racer.c": (
        "s8 spA1 = FALSE;",
        "Object *intendedTarget = NULL;",
        "pathGoalValid = set_position_goal_from_path",
        "mdkr_normalize_xz(spB8, spB4, &spB8, &spB4)",
        "mdkr_normalize_xz(xVel, zVel, &xVel, &zVel)",
        "*x = obj->trans.x_position;",
        "*y = obj->trans.y_position;",
        "*z = obj->trans.z_position;",
        "return FALSE;",
    ),
    "game/src/audio.c": (
        "audConfig.numGroups = MDKR_SOUND_GROUP_COUNT;",
        "mdkr_sound_id_valid(soundId, gSoundCount)",
        "mdkr_sound_id_valid(soundID, gSoundCount)",
    ),
    "game/src/audiosfx.c": (
        "sndp_group_volume_for_key(keyMap)",
        "gSoundGroupVolume == NULL || gSoundGroupCount == 0",
    ),
    "game/src/audio_vehicle.c": (
        "mdkr_vehicle_sound_row(characterId, vehicleId, &assetRow, &soundVehicleId)",
        "mdkr_asset_rows_fit(table[ASSET_AUDIO_7], table[ASSET_AUDIO_8]",
        "asset_swap_vehicle_sound(asset, sizeof(VehicleSoundAsset))",
        "mdkr_vehicle_sound_model(gSoundRacerObj->vehicleIDPrev)",
        "case VEHICLE_CAR:",
        "racer_sound_plane(obj, buttonsPressed, buttonsHeld, ticksDelta);",
        "racer_sound_car(obj, buttonsPressed, buttonsHeld, ticksDelta);",
    ),
    "game/src/audiomgr.c": (
        "mdkr_audio_fx_span(assetAudioTable,",
        "c->fxType[0] = AL_FX_NONE;",
        "mempool_free(assetAudioTable);",
        "mempool_free(asset8);",
    ),
    # game/libultra/src/audio/{env,load}.c entries removed: the SGI-derived
    # synthesizer was deleted at the clean-room audio swap, and the safety
    # contracts those two entries asserted died with the code they guarded.
    # The clean-room engine's equivalent boundary (synthInternals.h param
    # round-trips) is covered by check_address_domains' allowance instead.
    "game/src/textures_sprites.c": (
        "mdkr_light_cycle_time(arg0->unk8, updateRate, arg0->unkC)",
        "mdkr_light_cycle_time(data->time, timeDelta, data->totalTime)",
        "mdkr_light_fraction(65536u, arg0->unk8, cur->unk18->unk0)",
        "mdkr_light_fraction(data->frames[nextFrameIndex].value,",
        "mdkr_texture_allocation_size(",
        "mdkr_texture_frame_advance(",
        "mdkr_texture_pulse_layout(texture_list->width, texture_list->height,",
        "tblock = mblock + layout.halfBytes / sizeof(*mblock);",
        "mdkr_palette_reservation(",
        "gNumberOfLoadedTextures >= MAX_NUM_TEXTURES",
        "gTextureCache[ASSETCACHE_ID(slotIndex)] = id;",
        "if (cacheExtended) {\n        gNumberOfLoadedTextures++;",
    ),
    "game/src/object_functions.c": (
        "mdkr_trophy_state(settings->trophies, settings->worldId,",
        "mdkr_course_flag(levelEntry->goldenBalloon.balloonID, &flag)",
        "mdkr_course_flag(door->doorID, &doorIDFlag)",
        "mdkr_course_flag(triggerEntry->index, &flags)",
        "mdkr_model_index_resolve(",
    ),
    "game/src/objects.c": (
        "if (!mdkr_ainode_can_append(index2, nodeCount, (*gAINodes)[index2] != NULL)) {",
        "s32 cost = mdkr_ainode_distance_cost(diffX, diffY, diffZ);",
        "target == NULL || target != aiNode->nodeObj[i] ||",
        "target->level_entry->aiNode.nodeID != temp2 ||",
        "if (!mdkr_object_map_index_resolve(mapID, i, &mapID)) {",
        "s32 translationTableBytes;\n    for (i = 0; i < MDKR_OBJ_BHV_CACHE_N; ++i) sObjectBehaviorCache[i] = -3;",
        "if (!mdkr_objmap_validate_bodies(entries, totalSize)) return FALSE;",
        "if (hdr == NULL) {\n        /* A transient load failure must not poison future admissions. */\n        return -1;\n    }",
        "mdkr_model_load_selection(",
        "mdkr_trophy_state(settings->trophies, settings->worldId,",
        "uintptr_t objectEntryAddress = (uintptr_t) obj->level_entry;",
    ),
    "game/src/save_data.c": (
        "mdkr_extension_bit(extension, &extensionBit)",
        "level_count_for_save(&levelCount, &worldCount);",
    ),
    "game/src/menu.c": (
        "worldID = mdkr_postrace_background_index(header->world);",
        "headerWorldTex = mdkr_postrace_background_index(header->world);",
        "settings->trophies = mdkr_trophy_records_merge(",
        "mdkr_trophy_state(settings->trophies,",
        "i = WORLD_DINO_DOMAIN; i <= WORLD_FUTURE_FUN_LAND; i++)",
        "mdkr_trophy_state(settings->trophies, i, &trophyState)",
    ),
    "game/src/weather.c": (
        "weatherType = mdkr_weather_preset_index(weatherType, WEATHER_RAIN);",
        "if (weatherType < 0) return;",
        "DKR_SHL32(1U, rand_range(0, 32) + 5)",
    ),
    "platform/platform_sdl_min.c": (
        "static int8_t s_rumbleSupported[DKR_MAXPADS] = { -1, -1, -1, -1 };",
        "if (s_rumbleSupported[port] >= 0) return s_rumbleSupported[port];",
        "SDL_GameControllerHasRumble(s_gc[port]) == SDL_TRUE",
        "zero-strength compatibility probe exactly once per opened controller",
        "s_rumbleSupported[i] = -1;",
    ),
}

FORBIDDEN = {
    "game/src/audio.c": (
        "if (soundId > gSoundCount)",
        "if (soundID > gSoundCount)",
        "audConfig.numGroups = 1;",
    ),
    "game/src/racer.c": (
        "s8 spA1;\n",
        "Object *intendedTarget;\n",
    ),
    "game/src/object_functions.c": (
        "0x10000 << levelEntry->goldenBalloon.balloonID",
        "0x10000 << door->doorID",
        "0x10000 << triggerEntry->index",
    ),
    "game/src/save_data.c": (
        "1 << (fileExtensions[fileNum][0] + (BLANK_EXT_CHAR - 1))",
    ),
    "platform/platform_sdl_min.c": (
        "return SDL_GameControllerRumble(s_gc[port], 0, 0, 0) == 0;",
    ),
}


def load_sources() -> dict[str, str]:
    return {
        relative: (REPO / relative).read_text(encoding="utf-8", errors="replace")
        for relative in REQUIRED
    }


def validate(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for relative, fragments in REQUIRED.items():
        text = sources[relative]
        for fragment in fragments:
            if fragment not in text:
                failures.append(f"{relative}: missing contract fragment {fragment!r}")
    for relative, fragments in FORBIDDEN.items():
        text = sources[relative]
        for fragment in fragments:
            if fragment in text:
                failures.append(f"{relative}: obsolete unsafe fragment remains {fragment!r}")

    game = sources["game/src/game.c"]
    order = (
        "LevelHeader *levelHeader = gCurrentLevelHeader;",
        "free_track();",
        "weather_free();",
        "tex_free(skyTexture);",
        "mempool_free(levelHeader);",
    )
    positions = [game.find(fragment) for fragment in order]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        failures.append("game/src/game.c: level teardown ownership order changed")

    racer = sources["game/src/racer.c"]
    nodes = sources["game/src/objects.c"].split("void ainode_update(void)", 1)[-1]
    node_order = [nodes.find(fragment) for fragment in (
        "if (!mdkr_ainode_can_append(index2, nodeCount, (*gAINodes)[index2] != NULL)) {",
        "continue;", "(*gAINodes)[index2] = obj;", "nodeIDs[nodeCount] =",
        "elevations[nodeCount] =", "nodeCount++;",
    )]
    if any(position < 0 for position in node_order) or node_order != sorted(node_order):
        failures.append("objects.c: AI-node duplicate/capacity guard must precede rebuild writes")
    weather = sources["game/src/weather.c"].split("void weather_reset(", 1)[-1]
    weather_order = [weather.find(fragment) for fragment in (
        "weatherType = mdkr_weather_preset_index(weatherType, WEATHER_RAIN);",
        "if (weatherType < 0) return;", "weather_free();", "gWeatherPresets[weatherType]",
    )]
    if any(position < 0 for position in weather_order) or weather_order != sorted(weather_order):
        failures.append("weather.c: validate weather type before teardown and preset access")
    if game.count("size = level_header_size_checked(gTempAssetTable, gNumberOfLevelHeaders, levelId);") != 2:
        failures.append("game.c: initial and redirected header loads must both check spans")
    if sources["game/src/save_data.c"].count("level_count_for_save(&levelCount, &worldCount);") != 4:
        failures.append("save_data.c: all four retail codecs must use the fixed inventory")
    if racer.count("pathGoalValid = set_position_goal_from_path") != 2:
        failures.append("game/src/racer.c: expected both recovery paths to validate path goals")
    if racer.count("mdkr_normalize_xz(") != 2:
        failures.append("game/src/racer.c: expected both recovery paths to normalize safely")

    audio_sfx = sources["game/src/audiosfx.c"]
    if audio_sfx.count("sndp_group_volume_for_key(keyMap)") != 5:
        failures.append("game/src/audiosfx.c: not every group-volume read uses the bounded helper")
    if audio_sfx.count("groupID >= gSoundGroupCount") != 2:
        failures.append("game/src/audiosfx.c: group getter/setter bounds checks changed")
    return failures


def run_positive_controls(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for relative, fragments in REQUIRED.items():
        for fragment in fragments:
            mutated = dict(sources)
            mutated[relative] = mutated[relative].replace(fragment, "")
            if not validate(mutated):
                failures.append(
                    f"positive control did not reject removal of {relative}: {fragment!r}"
                )
    mutated = dict(sources)
    mutated["game/src/audiosfx.c"] = mutated["game/src/audiosfx.c"].replace(
        "groupID >= gSoundGroupCount", "", 1
    )
    if not validate(mutated):
        failures.append("positive control did not reject a missing group API bounds check")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True, help="native build or mdkr64 binary")
    parser.add_argument("--rom", help="accepted for the suite's common invocation shape")
    args = parser.parse_args()

    sources = load_sources()
    failures = validate(sources)
    failures.extend(run_positive_controls(sources))

    binary = Path(resolve_binary(args.build))
    unit = binary.parent / "mdkr_runtime_contracts_test"
    if not unit.is_file():
        failures.append(f"missing runtime-contract unit executable: {unit}")
    else:
        result = subprocess.run(
            [str(unit)],
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if result.returncode != 0:
            failures.append(
                f"runtime-contract unit executable failed ({result.returncode}):\n"
                f"{result.stdout}"
            )

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("runtime safety production census and positive controls passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
