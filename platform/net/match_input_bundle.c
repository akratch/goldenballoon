#include "match_input_bundle.h"

#include <string.h>

static const uint8_t kMagic[2] = {'M', 'B'};

static void store_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void store_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static uint16_t load_u16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static uint32_t load_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static uint16_t crc16_ccitt(const uint8_t *bytes, size_t length) {
    uint16_t value = UINT16_C(0xffff);
    size_t index;
    for (index = 0u; index < length; index++) {
        unsigned bit;
        value ^= (uint16_t)bytes[index] << 8;
        for (bit = 0u; bit < 8u; bit++) {
            value = (uint16_t)(value << 1) ^
                (uint16_t)(UINT16_C(0x1021) &
                           (uint16_t)-(int16_t)(value >> 15));
        }
    }
    return value;
}

static bool sample_valid(const MdkrPadSample *sample, bool owned) {
    if (!owned) {
        return sample->buttons == 0u && sample->stick_x == 0 &&
            sample->stick_y == 0 && sample->present == 0u;
    }
    return sample->present == 1u && sample->stick_x >= -80 &&
        sample->stick_x <= 80 && sample->stick_y >= -80 &&
        sample->stick_y <= 80;
}

bool mdkr_match_input_bundle_encode(
    const MdkrMatchInputBundle *bundle, uint8_t *bytes, size_t length) {
    uint8_t encoded[MDKR_MATCH_INPUT_BUNDLE_BYTES];
    unsigned frame;
    unsigned slot;
    if (bundle == NULL || bytes == NULL ||
        length != MDKR_MATCH_INPUT_BUNDLE_BYTES ||
        bundle->match_epoch == 0u || bundle->frame_count == 0u ||
        bundle->frame_count > MDKR_MATCH_INPUT_BUNDLE_FRAMES ||
        bundle->slot_mask == 0u || (bundle->slot_mask & 0xf0u) != 0u ||
        bundle->newest_tick + 1u < bundle->frame_count) return false;
    memset(encoded, 0, sizeof(encoded));
    memcpy(encoded, kMagic, sizeof(kMagic));
    encoded[2] = MDKR_MATCH_INPUT_BUNDLE_VERSION;
    encoded[3] = bundle->frame_count;
    encoded[4] = bundle->slot_mask;
    store_u32(&encoded[6], bundle->match_epoch);
    store_u32(&encoded[10], bundle->newest_tick);
    for (frame = 0u; frame < MDKR_MATCH_INPUT_BUNDLE_FRAMES; frame++) {
        for (slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; slot++) {
            const bool included = frame < bundle->frame_count &&
                (bundle->slot_mask & (uint8_t)(1u << slot)) != 0u;
            const MdkrPadSample *sample = &bundle->frames[frame][slot];
            const size_t offset = 14u + (frame * 4u + slot) * 4u;
            if (!sample_valid(sample, included)) return false;
            store_u16(&encoded[offset], sample->buttons);
            encoded[offset + 2u] = (uint8_t)sample->stick_x;
            encoded[offset + 3u] = (uint8_t)sample->stick_y;
        }
    }
    store_u16(&encoded[62], crc16_ccitt(encoded, 62u));
    memcpy(bytes, encoded, sizeof(encoded));
    return true;
}

bool mdkr_match_input_bundle_decode(
    const uint8_t *bytes, size_t length, MdkrMatchInputBundle *bundle) {
    MdkrMatchInputBundle decoded;
    unsigned frame;
    unsigned slot;
    if (bytes == NULL || bundle == NULL ||
        length != MDKR_MATCH_INPUT_BUNDLE_BYTES ||
        memcmp(bytes, kMagic, sizeof(kMagic)) != 0 ||
        bytes[2] != MDKR_MATCH_INPUT_BUNDLE_VERSION || bytes[5] != 0u ||
        bytes[3] == 0u || bytes[3] > MDKR_MATCH_INPUT_BUNDLE_FRAMES ||
        bytes[4] == 0u || (bytes[4] & 0xf0u) != 0u ||
        load_u16(&bytes[62]) != crc16_ccitt(bytes, 62u)) return false;
    memset(&decoded, 0, sizeof(decoded));
    decoded.frame_count = bytes[3];
    decoded.slot_mask = bytes[4];
    decoded.match_epoch = load_u32(&bytes[6]);
    decoded.newest_tick = load_u32(&bytes[10]);
    if (decoded.match_epoch == 0u ||
        decoded.newest_tick + 1u < decoded.frame_count) return false;
    for (frame = 0u; frame < MDKR_MATCH_INPUT_BUNDLE_FRAMES; frame++) {
        for (slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; slot++) {
            const bool included = frame < decoded.frame_count &&
                (decoded.slot_mask & (uint8_t)(1u << slot)) != 0u;
            MdkrPadSample *sample = &decoded.frames[frame][slot];
            const size_t offset = 14u + (frame * 4u + slot) * 4u;
            sample->buttons = load_u16(&bytes[offset]);
            sample->stick_x = (int8_t)bytes[offset + 2u];
            sample->stick_y = (int8_t)bytes[offset + 3u];
            sample->present = included ? 1u : 0u;
            if (!sample_valid(sample, included)) return false;
        }
    }
    *bundle = decoded;
    return true;
}
