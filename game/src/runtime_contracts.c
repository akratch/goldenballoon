#include "runtime_contracts.h"

#include "enums.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

_Static_assert(NUMBER_OF_CHARACTERS * NUMBER_OF_PLAYER_VEHICLES ==
                   MDKR_VEHICLE_SOUND_ROW_COUNT,
               "vehicle sound row count must cover every base character/vehicle pair");
_Static_assert((long long) MDKR_AI_ROUTE_POP_BUDGET * MDKR_AI_MAP_LINK_MAX < INT32_MAX,
               "route search budget must leave every map-coordinate path cost in s32");

s32 mdkr_ainode_can_append(s32 nodeId, s32 count, s32 occupied) {
    return nodeId >= 0 && nodeId < MDKR_AI_NODE_COUNT &&
        count >= 0 && count < MDKR_AI_NODE_COUNT && !occupied;
}

s32 mdkr_ainode_distance_cost(f32 dx, f32 dy, f32 dz) {
    const f32 distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!isfinite(distance) || distance > INT32_MAX / MDKR_AI_NODE_COUNT) return -1;
    return (s32)distance;
}

s32 mdkr_audio_group_valid(s32 groupId) {
    return groupId >= 0 && groupId < MDKR_SOUND_GROUP_COUNT;
}

s32 mdkr_sound_id_valid(u32 soundId, u32 soundCount) {
    return soundId < soundCount;
}

s32 mdkr_vehicle_sound_model(s32 vehicleId) {
    switch (vehicleId) {
        case VEHICLE_CAR:
        case VEHICLE_LOOPDELOOP:
            return VEHICLE_CAR;
        case VEHICLE_HOVERCRAFT:
            return VEHICLE_HOVERCRAFT;
        case VEHICLE_PLANE:
        case VEHICLE_FLYING_CAR:
            return VEHICLE_PLANE;
        default:
            return -1;
    }
}

s32 mdkr_vehicle_sound_row(s32 characterId, s32 vehicleId, s32 *assetRow, s32 *soundVehicleId) {
    s32 mappedVehicle;

    if (assetRow == NULL || soundVehicleId == NULL ||
        characterId < 0 || characterId >= NUMBER_OF_CHARACTERS) {
        return FALSE;
    }

    mappedVehicle = mdkr_vehicle_sound_model(vehicleId);
    if (mappedVehicle < 0) {
        return FALSE;
    }

    *soundVehicleId = mappedVehicle;
    *assetRow = mappedVehicle * NUMBER_OF_CHARACTERS + characterId;
    return *assetRow >= 0 && *assetRow < MDKR_VEHICLE_SOUND_ROW_COUNT;
}

s32 mdkr_asset_rows_fit(s32 startOffset, s32 endOffset, s32 rowCount, s32 rowSize) {
    s32 span;

    if (startOffset < 0 || endOffset < startOffset || rowCount < 0 || rowSize <= 0) {
        return FALSE;
    }
    span = endOffset - startOffset;
    return rowCount <= span / rowSize;
}

s32 mdkr_normalize_xz(f32 x, f32 z, f32 *normalizedX, f32 *normalizedZ) {
    f32 magnitudeSquared;
    f32 inverseMagnitude;

    if (normalizedX == NULL || normalizedZ == NULL) {
        return FALSE;
    }
    *normalizedX = 0.0f;
    *normalizedZ = 0.0f;
    if (!isfinite(x) || !isfinite(z)) {
        return FALSE;
    }
    magnitudeSquared = x * x + z * z;
    if (!isfinite(magnitudeSquared) || magnitudeSquared <= 1.0e-12f) {
        return FALSE;
    }
    inverseMagnitude = 1.0f / sqrtf(magnitudeSquared);
    *normalizedX = x * inverseMagnitude;
    *normalizedZ = z * inverseMagnitude;
    if (!isfinite(*normalizedX) || !isfinite(*normalizedZ)) {
        *normalizedX = 0.0f;
        *normalizedZ = 0.0f;
        return FALSE;
    }
    return TRUE;
}

s32 mdkr_model_index_resolve(s32 requested, s32 modelCount, s32 *resolved) {
    if (resolved == NULL || modelCount <= 0) {
        return FALSE;
    }
    *resolved = (requested >= 0 && requested < modelCount) ? requested : 0;
    return TRUE;
}

s32 mdkr_object_map_index_resolve(s32 requested, s32 mapCount, s32 *resolved) {
    if (resolved == NULL || requested < 0 || mapCount <= 0) {
        return FALSE;
    }
    *resolved = requested < mapCount ? requested : 0;
    return TRUE;
}

s32 mdkr_postrace_background_index(s32 worldId) {
    return worldId >= WORLD_DINO_DOMAIN && worldId <= WORLD_FUTURE_FUN_LAND
        ? worldId - WORLD_DINO_DOMAIN : 0;
}

s32 mdkr_weather_preset_index(s32 requested, s32 lastPreset) {
    if (requested < 0 || lastPreset < 0) return -1;
    return requested > lastPreset ? lastPreset : requested;
}

u32 mdkr_light_fraction(u32 scale, s32 elapsed, s32 duration) {
    if (elapsed <= 0 || duration <= 0) return 0;
    if (elapsed >= duration) return scale;
    return (u32)(((u64)scale * (u32)elapsed) / (u32)duration);
}

s32 mdkr_light_cycle_time(s32 elapsed, s32 delta, s32 duration) {
    if (elapsed < 0 || delta < 0 || duration <= 0) return -1;
    return (s32)(((u64)(u32)elapsed + (u32)delta) % (u32)duration);
}

const char *mdkr_level_name_bounded(const char *bytes, size_t size, unsigned languageIndex) {
    if (!bytes || languageIndex > 3) return NULL;
    for (unsigned i = 0; i <= languageIndex; ++i) {
        const char *end = memchr(bytes, 0, size);
        if (!end) return NULL;
        if (i == languageIndex) return bytes;
        size_t consumed = (size_t)(end - bytes) + 1;
        bytes += consumed;
        size -= consumed;
    }
    return NULL;
}

s32 mdkr_model_load_selection(s32 requested, s32 modelCount, s32 *resolved,
                              s32 *loadCount) {
    if (resolved == NULL || loadCount == NULL ||
        !mdkr_model_index_resolve(requested, modelCount, resolved)) {
        return FALSE;
    }
    *loadCount = *resolved + 1;
    return TRUE;
}

s32 mdkr_course_flag(s32 index, u32 *flag) {
    if (flag == NULL || index < 0 || index >= 16) {
        return FALSE;
    }
    *flag = UINT32_C(0x10000) << (u32) index;
    return TRUE;
}

s32 mdkr_trophy_state(u32 trophies, s32 worldId, u32 *state) {
    /* Five two-bit fields are persisted, including Future Funland (bits 8..9).
     * The four mainland trophies unlock it; they are not the storage bound. */
    if (state == NULL || worldId < WORLD_DINO_DOMAIN ||
        worldId > WORLD_FUTURE_FUN_LAND) {
        return FALSE;
    }
    *state = (trophies >> ((u32) (worldId - 1) * 2U)) & 3U;
    return TRUE;
}

u32 mdkr_trophy_records_merge(u32 left, u32 right) {
    u32 merged = 0;
    for (s32 world = WORLD_DINO_DOMAIN; world <= WORLD_FUTURE_FUN_LAND; world++) {
        u32 shift = (u32) (world - WORLD_DINO_DOMAIN) * 2U;
        u32 leftState = (left >> shift) & 3U;
        u32 rightState = (right >> shift) & 3U;
        merged |= (leftState > rightState ? leftState : rightState) << shift;
    }
    return merged;
}

s32 mdkr_extension_bit(char extension, u32 *bit) {
    u32 index;

    if (bit == NULL || extension < 'A' || extension > 'Z') {
        return FALSE;
    }
    index = (u32) (extension - 'A');
    *bit = UINT32_C(1) << index;
    return TRUE;
}

static s32 checked_size_add(size_t left, size_t right, size_t *result) {
    if (result == NULL || right > SIZE_MAX - left) {
        return FALSE;
    }
    *result = left + right;
    return TRUE;
}

static s32 checked_size_mul(size_t left, size_t right, size_t *result) {
    if (result == NULL || (left != 0 && right > SIZE_MAX / left)) {
        return FALSE;
    }
    *result = left * right;
    return TRUE;
}

s32 mdkr_texture_pulse_layout(unsigned width, unsigned height, unsigned format,
    MdkrPulseTextureLayout *out) {
    if (!out || format != 1 || width < 4 || width > 64 || height < 2 || height > 64 ||
        (width & (width - 1)) || (height & (height - 1)) || width * height > 2048)
        return 0;
    MdkrPulseTextureLayout result = {width, height / 2, 0, 0, (size_t)width * height};
    for (unsigned n = width; n > 1; n >>= 1) ++result.maskS;
    for (unsigned n = height / 2; n > 1; n >>= 1) ++result.maskT;
    *out = result;
    return 1;
}

s32 mdkr_texture_allocation_size(size_t dataBytes, size_t frameCount,
                                 size_t textureCommandBytes,
                                 size_t paletteCommandBytes,
                                 size_t *allocationBytes) {
    size_t perFrame;
    size_t commandBytes;
    size_t total;

    if (allocationBytes == NULL || dataBytes == 0 || frameCount == 0 ||
        textureCommandBytes == 0 ||
        !checked_size_add(textureCommandBytes, paletteCommandBytes, &perFrame) ||
        !checked_size_mul(frameCount, perFrame, &commandBytes) ||
        !checked_size_add(dataBytes, 15, &total) ||
        !checked_size_add(total, commandBytes, &total) ||
        total > INT_MAX) {
        return FALSE;
    }
    *allocationBytes = total;
    return TRUE;
}

s32 mdkr_texture_frame_advance(size_t decodedBytes, size_t cursorOffset,
                               size_t headerBytes, size_t frameBytes,
                               s32 finalFrame, size_t *nextOffset) {
    size_t spanLimit;

    if (nextOffset == NULL || headerBytes == 0 ||
        cursorOffset > decodedBytes ||
        headerBytes > decodedBytes - cursorOffset ||
        decodedBytes > SIZE_MAX - 15) {
        return FALSE;
    }
    if (frameBytes == 0) {
        if (!finalFrame) {
            return FALSE;
        }
        *nextOffset = cursorOffset;
        return TRUE;
    }
    spanLimit = (decodedBytes + 15) & ~(size_t) 15;
    if (frameBytes < headerBytes || cursorOffset > spanLimit ||
        frameBytes > spanLimit - cursorOffset) {
        return FALSE;
    }
    *nextOffset = cursorOffset + frameBytes;
    return TRUE;
}

s32 mdkr_palette_reservation(size_t usedBytes, size_t paletteBytes,
                             size_t capacityBytes, size_t *paletteOffset) {
    if (paletteOffset == NULL || paletteBytes == 0 ||
        usedBytes > capacityBytes || paletteBytes > capacityBytes - usedBytes) {
        return FALSE;
    }
    *paletteOffset = usedBytes;
    return TRUE;
}

s32 mdkr_audio_fx_span(const u32 *table, size_t tableBytes,
                       size_t audioSectionBytes, s32 *offset, s32 *size) {
    u32 start;
    u32 end;

    if (table == NULL || offset == NULL || size == NULL ||
        tableBytes < 10 * sizeof(*table)) {
        return FALSE;
    }
    start = table[8];
    end = table[9];
    if (start >= end || end > audioSectionBytes ||
        start > INT_MAX || end - start > INT_MAX) {
        return FALSE;
    }
    *offset = (s32) start;
    *size = (s32) (end - start);
    return TRUE;
}

/*
 * IDO emits `cvt.w.d` with the FCSR rounding mode temporarily set toward zero.
 * MIPS returns the signed indefinite value (INT32_MIN) for NaN, infinity, or a
 * result outside the signed-word range. Keep that exact word-level contract.
 */
s32 mdkr_mips_trunc_w_d(f64 value) {
    if (!isfinite(value) || value < (f64) INT32_MIN ||
        value >= (f64) INT32_MAX + 1.0) {
        return INT32_MIN;
    }
    return (s32) value;
}

/*
 * `cvt.w.s` under the N64 boot FCSR's round-to-nearest-even mode. Implement
 * the tie rule explicitly instead of inheriting a mutable host fenv. Invalid
 * conversions return MIPS' signed-word indefinite value.
 */
s32 mdkr_mips_round_w_s(f32 value) {
    f32 lowerFloat;
    f32 fraction;
    s32 lower;

    if (!isfinite(value) || value < (f32) INT32_MIN ||
        value >= 2147483648.0f) {
        return INT32_MIN;
    }
    lowerFloat = floorf(value);
    lower = (s32) lowerFloat;
    fraction = value - lowerFloat;
    if (fraction > 0.5f ||
        (fraction == 0.5f && (((u32) lower & 1u) != 0))) {
        lower++;
    }
    return lower;
}

u32 mdkr_mips_atan_index(u32 numerator, u32 denominator) {
    uint64_t scaled;
    u32 index;

    if (denominator == 0) {
        return 0;
    }
    scaled = (uint64_t) numerator << 10;
    index = (u32) (scaled / denominator);
    return index <= 1024u ? index : 1024u;
}

static s16 signed_low_half(s32 value) {
    u16 bits = (u16) (u32) value;
    s16 result;

    memcpy(&result, &bits, sizeof(result));
    return result;
}

s16 mdkr_mips_f64_to_s16(f64 value) {
    return signed_low_half(mdkr_mips_trunc_w_d(value));
}

u16 mdkr_mips_f64_to_u16(f64 value) {
    return (u16) (u32) mdkr_mips_trunc_w_d(value);
}

f32 mdkr_audio_rate_value(s16 rateMajor, u16 rateMinor) {
    u32 bits = ((u32) (u16) rateMajor << 16) | (u32) rateMinor;
    s32 fixed;

    memcpy(&fixed, &bits, sizeof(fixed));
    return (f32) fixed / 65536.0f;
}
