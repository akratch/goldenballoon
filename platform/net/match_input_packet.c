#include "match_input_packet.h"

#include <string.h>

static const uint8_t kMagic[4] = {'M', 'I', 'N', 'P'};

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

bool mdkr_match_input_packet_encode(
    const MdkrMatchInputPacket *packet, uint8_t *bytes, size_t length) {
    uint8_t encoded[MDKR_MATCH_INPUT_PACKET_BYTES];
    if (packet == NULL || bytes == NULL ||
        length != MDKR_MATCH_INPUT_PACKET_BYTES ||
        packet->match_epoch == 0u ||
        packet->canonical_slot >= 4u) return false;
    memset(encoded, 0, sizeof(encoded));
    memcpy(encoded, kMagic, sizeof(kMagic));
    encoded[4] = MDKR_MATCH_INPUT_PACKET_VERSION;
    store_u32(&encoded[8], packet->match_epoch);
    store_u32(&encoded[12], packet->tick);
    encoded[16] = packet->canonical_slot;
    encoded[17] = 1u;
    store_u16(&encoded[18], packet->buttons);
    encoded[20] = (uint8_t)packet->stick_x;
    encoded[21] = (uint8_t)packet->stick_y;
    store_u16(&encoded[22], crc16_ccitt(encoded, 22u));
    memcpy(bytes, encoded, sizeof(encoded));
    return true;
}

bool mdkr_match_input_packet_decode(
    const uint8_t *bytes, size_t length, MdkrMatchInputPacket *packet) {
    MdkrMatchInputPacket decoded;
    if (bytes == NULL || packet == NULL ||
        length != MDKR_MATCH_INPUT_PACKET_BYTES ||
        memcmp(bytes, kMagic, sizeof(kMagic)) != 0 ||
        bytes[4] != MDKR_MATCH_INPUT_PACKET_VERSION ||
        bytes[5] != 0u || bytes[6] != 0u || bytes[7] != 0u ||
        bytes[17] != 1u ||
        load_u16(&bytes[22]) != crc16_ccitt(bytes, 22u)) return false;
    memset(&decoded, 0, sizeof(decoded));
    decoded.match_epoch = load_u32(&bytes[8]);
    decoded.tick = load_u32(&bytes[12]);
    decoded.canonical_slot = bytes[16];
    decoded.buttons = load_u16(&bytes[18]);
    decoded.stick_x = (int8_t)bytes[20];
    decoded.stick_y = (int8_t)bytes[21];
    if (decoded.match_epoch == 0u ||
        decoded.canonical_slot >= 4u) return false;
    *packet = decoded;
    return true;
}
