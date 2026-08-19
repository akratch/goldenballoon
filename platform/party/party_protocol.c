#include "party_protocol.h"

#include <string.h>

static uint16_t read_u16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t read_u32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static void write_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

size_t mdkr_party_pad_encoded_size(uint8_t edge_count) {
    if (edge_count > MDKR_PARTY_PAD_MAX_EDGES) return 0u;
    return MDKR_PARTY_PAD_FIXED_BYTES +
        (size_t)edge_count * MDKR_PARTY_PAD_EDGE_BYTES;
}

uint16_t mdkr_party_crc16(const uint8_t *bytes, size_t length) {
    uint16_t crc = 0xffffu;
    size_t index;
    unsigned bit;
    if (bytes == NULL && length != 0u) return 0u;
    for (index = 0; index < length; index++) {
        crc ^= (uint16_t)bytes[index] << 8u;
        for (bit = 0; bit < 8u; bit++) {
            crc = (crc & 0x8000u) != 0u
                ? (uint16_t)((crc << 1u) ^ 0x1021u)
                : (uint16_t)(crc << 1u);
        }
    }
    return crc;
}

static MdkrPartyDecodeResult validate_sample(
    uint16_t buttons, int8_t stick_x, int8_t stick_y) {
    if ((buttons & (uint16_t)~MDKR_PARTY_PAD_LEGAL_BUTTONS) != 0u) {
        return MDKR_PARTY_DECODE_BUTTONS;
    }
    if (stick_x < -80 || stick_x > 80 || stick_y < -80 || stick_y > 80) {
        return MDKR_PARTY_DECODE_STICK;
    }
    return MDKR_PARTY_DECODE_OK;
}

static MdkrPartyDecodeResult validate_packet(const MdkrPartyPadPacket *packet) {
    uint8_t index;
    MdkrPartyDecodeResult result;
    if (packet == NULL) return MDKR_PARTY_DECODE_NULL;
    if ((packet->flags & (uint8_t)~MDKR_PARTY_PAD_FLAG_MASK) != 0u) {
        return MDKR_PARTY_DECODE_FLAGS;
    }
    if (packet->edge_count > MDKR_PARTY_PAD_MAX_EDGES) {
        return MDKR_PARTY_DECODE_EDGE_COUNT;
    }
    if (((packet->flags & MDKR_PARTY_PAD_FLAG_HAS_EDGES) != 0u) !=
        (packet->edge_count != 0u)) {
        return MDKR_PARTY_DECODE_FLAGS;
    }
    result = validate_sample(packet->buttons, packet->stick_x, packet->stick_y);
    if (result != MDKR_PARTY_DECODE_OK) return result;
    if ((packet->flags & MDKR_PARTY_PAD_FLAG_NEUTRAL) != 0u &&
        (packet->buttons != 0u || packet->stick_x != 0 || packet->stick_y != 0)) {
        return MDKR_PARTY_DECODE_NEUTRAL;
    }
    if ((packet->flags & MDKR_PARTY_PAD_FLAG_PRESENT) == 0u &&
        ((packet->flags & MDKR_PARTY_PAD_FLAG_NEUTRAL) == 0u ||
         packet->buttons != 0u || packet->stick_x != 0 || packet->stick_y != 0)) {
        return MDKR_PARTY_DECODE_NEUTRAL;
    }
    for (index = 0u; index < packet->edge_count; index++) {
        const MdkrPartyPadEdge *edge = &packet->edges[index];
        if (edge->sequence_delta == 0u || edge->sequence_delta > 127u ||
            (index != 0u && edge->sequence_delta >=
             packet->edges[index - 1u].sequence_delta)) {
            return MDKR_PARTY_DECODE_EDGE_SEQUENCE;
        }
        result = validate_sample(edge->buttons, edge->stick_x, edge->stick_y);
        if (result != MDKR_PARTY_DECODE_OK) return result;
    }
    return MDKR_PARTY_DECODE_OK;
}

bool mdkr_party_pad_encode(
    const MdkrPartyPadPacket *packet, uint8_t *output, size_t capacity,
    size_t *out_length) {
    const size_t length = packet != NULL
        ? mdkr_party_pad_encoded_size(packet->edge_count) : 0u;
    size_t offset;
    uint8_t index;
    if (out_length != NULL) *out_length = 0u;
    if (output == NULL || length == 0u || capacity < length ||
        validate_packet(packet) != MDKR_PARTY_DECODE_OK) return false;
    output[0] = MDKR_PARTY_PAD_MAGIC_0;
    output[1] = MDKR_PARTY_PAD_MAGIC_1;
    output[2] = MDKR_PARTY_PAD_VERSION;
    output[3] = MDKR_PARTY_PAD_TYPE_STATE;
    output[4] = packet->flags;
    write_u32(output + 5u, packet->connection_sequence);
    write_u32(output + 9u, packet->sample_sequence);
    write_u32(output + 13u, packet->sender_time_ms);
    write_u16(output + 17u, packet->buttons);
    output[19] = (uint8_t)packet->stick_x;
    output[20] = (uint8_t)packet->stick_y;
    output[21] = packet->edge_count;
    offset = 22u;
    for (index = 0u; index < packet->edge_count; index++) {
        const MdkrPartyPadEdge *edge = &packet->edges[index];
        output[offset++] = edge->sequence_delta;
        write_u16(output + offset, edge->buttons);
        offset += 2u;
        output[offset++] = (uint8_t)edge->stick_x;
        output[offset++] = (uint8_t)edge->stick_y;
    }
    write_u16(output + offset, mdkr_party_crc16(output, offset));
    if (out_length != NULL) *out_length = length;
    return true;
}

MdkrPartyDecodeResult mdkr_party_pad_decode(
    const uint8_t *bytes, size_t length, MdkrPartyPadPacket *out_packet) {
    MdkrPartyPadPacket decoded;
    MdkrPartyDecodeResult result;
    size_t offset;
    uint8_t index;
    if (bytes == NULL || out_packet == NULL) return MDKR_PARTY_DECODE_NULL;
    if (length < MDKR_PARTY_PAD_FIXED_BYTES ||
        length > MDKR_PARTY_PAD_MAX_BYTES) return MDKR_PARTY_DECODE_LENGTH;
    if (bytes[0] != MDKR_PARTY_PAD_MAGIC_0 ||
        bytes[1] != MDKR_PARTY_PAD_MAGIC_1) return MDKR_PARTY_DECODE_MAGIC;
    if (bytes[2] != MDKR_PARTY_PAD_VERSION) return MDKR_PARTY_DECODE_VERSION;
    if (bytes[3] != MDKR_PARTY_PAD_TYPE_STATE) return MDKR_PARTY_DECODE_TYPE;
    memset(&decoded, 0, sizeof(decoded));
    decoded.flags = bytes[4];
    decoded.connection_sequence = read_u32(bytes + 5u);
    decoded.sample_sequence = read_u32(bytes + 9u);
    decoded.sender_time_ms = read_u32(bytes + 13u);
    decoded.buttons = read_u16(bytes + 17u);
    decoded.stick_x = (int8_t)bytes[19];
    decoded.stick_y = (int8_t)bytes[20];
    decoded.edge_count = bytes[21];
    if (decoded.edge_count > MDKR_PARTY_PAD_MAX_EDGES ||
        mdkr_party_pad_encoded_size(decoded.edge_count) != length) {
        return MDKR_PARTY_DECODE_EDGE_COUNT;
    }
    offset = 22u;
    for (index = 0u; index < decoded.edge_count; index++) {
        MdkrPartyPadEdge *edge = &decoded.edges[index];
        edge->sequence_delta = bytes[offset++];
        edge->buttons = read_u16(bytes + offset);
        offset += 2u;
        edge->stick_x = (int8_t)bytes[offset++];
        edge->stick_y = (int8_t)bytes[offset++];
    }
    if (read_u16(bytes + offset) != mdkr_party_crc16(bytes, offset)) {
        return MDKR_PARTY_DECODE_CHECKSUM;
    }
    result = validate_packet(&decoded);
    if (result != MDKR_PARTY_DECODE_OK) return result;
    *out_packet = decoded;
    return MDKR_PARTY_DECODE_OK;
}

const char *mdkr_party_decode_result_name(MdkrPartyDecodeResult result) {
    static const char *const names[] = {
        "ok", "null", "length", "magic", "version", "type", "flags",
        "edge-count", "edge-sequence", "buttons", "stick", "neutral",
        "checksum"
    };
    if ((unsigned)result >= sizeof(names) / sizeof(names[0])) return "unknown";
    return names[result];
}
