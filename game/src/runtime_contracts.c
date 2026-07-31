#include "runtime_contracts.h"

#include "enums.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

_Static_assert(NUMBER_OF_CHARACTERS * NUMBER_OF_PLAYER_VEHICLES ==
                   MDKR_VEHICLE_SOUND_ROW_COUNT,
               "vehicle sound row count must cover every base character/vehicle pair");

s32 mdkr_audio_group_valid(s32 groupId) {
    return groupId >= 0 && groupId < MDKR_SOUND_GROUP_COUNT;
}

s32 mdkr_sound_id_valid(u32 soundId, u32 soundCount) {
    return soundId < soundCount;
}

s32 mdkr_vehicle_sound_row(s32 characterId, s32 vehicleId, s32 *assetRow, s32 *soundVehicleId) {
    s32 mappedVehicle;

    if (assetRow == NULL || soundVehicleId == NULL ||
        characterId < 0 || characterId >= NUMBER_OF_CHARACTERS) {
        return FALSE;
    }

    switch (vehicleId) {
        case VEHICLE_CAR:
        case VEHICLE_HOVERCRAFT:
        case VEHICLE_PLANE:
            mappedVehicle = vehicleId;
            break;
        case VEHICLE_FLYING_CAR:
            /* The flying-car/carpet path uses the plane update and sound model. */
            mappedVehicle = VEHICLE_PLANE;
            break;
        case VEHICLE_LOOPDELOOP:
            /* The loop challenge uses the car update and sound model. */
            mappedVehicle = VEHICLE_CAR;
            break;
        default:
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
    if (state == NULL || worldId < 1 || worldId > 4) {
        return FALSE;
    }
    *state = (trophies >> ((u32) (worldId - 1) * 2U)) & 3U;
    return TRUE;
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
