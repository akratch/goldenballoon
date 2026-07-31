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
    "game/src/game.c": (
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
        "case VEHICLE_FLYING_CAR:",
        "racer_sound_plane(obj, buttonsPressed, buttonsHeld, ticksDelta);",
        "case VEHICLE_LOOPDELOOP:",
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
        "mdkr_texture_allocation_size(",
        "mdkr_texture_frame_advance(",
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
        "mdkr_model_load_selection(",
        "mdkr_trophy_state(settings->trophies, settings->worldId,",
        "uintptr_t objectEntryAddress = (uintptr_t) obj->level_entry;",
    ),
    "game/src/save_data.c": (
        "mdkr_extension_bit(extension, &extensionBit)",
    ),
    "game/src/weather.c": (
        "DKR_SHL32(1U, rand_range(0, 32) + 5)",
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
    if racer.count("pathGoalValid = set_position_goal_from_path") != 2:
        failures.append("game/src/racer.c: expected both recovery paths to validate path goals")
    if racer.count("mdkr_normalize_xz(") != 2:
        failures.append("game/src/racer.c: expected both recovery paths to normalize safely")

    audio_sfx = sources["game/src/audiosfx.c"]
    if audio_sfx.count("sndp_group_volume_for_key(keyMap)") != 4:
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
