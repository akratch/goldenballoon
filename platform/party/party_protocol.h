/* Phone Party pad-state wire contract v1. Pure codec; no allocation or I/O. */
#ifndef MDKR_PARTY_PROTOCOL_H
#define MDKR_PARTY_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_PARTY_PAD_MAGIC_0 ((uint8_t)'G')
#define MDKR_PARTY_PAD_MAGIC_1 ((uint8_t)'B')
#define MDKR_PARTY_PAD_VERSION 1u
#define MDKR_PARTY_PAD_TYPE_STATE 1u
#define MDKR_PARTY_PAD_MAX_EDGES 8u
#define MDKR_PARTY_PAD_FIXED_BYTES 24u
#define MDKR_PARTY_PAD_EDGE_BYTES 5u
#define MDKR_PARTY_PAD_MAX_BYTES \
    (MDKR_PARTY_PAD_FIXED_BYTES + \
     MDKR_PARTY_PAD_MAX_EDGES * MDKR_PARTY_PAD_EDGE_BYTES)

#define MDKR_PARTY_PAD_FLAG_PRESENT 0x01u
#define MDKR_PARTY_PAD_FLAG_NEUTRAL 0x02u
#define MDKR_PARTY_PAD_FLAG_HAS_EDGES 0x04u
#define MDKR_PARTY_PAD_FLAG_MASK 0x07u
#define MDKR_PARTY_PAD_LEGAL_BUTTONS 0xff3fu

typedef enum MdkrPartyDecodeResult {
    MDKR_PARTY_DECODE_OK = 0,
    MDKR_PARTY_DECODE_NULL,
    MDKR_PARTY_DECODE_LENGTH,
    MDKR_PARTY_DECODE_MAGIC,
    MDKR_PARTY_DECODE_VERSION,
    MDKR_PARTY_DECODE_TYPE,
    MDKR_PARTY_DECODE_FLAGS,
    MDKR_PARTY_DECODE_EDGE_COUNT,
    MDKR_PARTY_DECODE_EDGE_SEQUENCE,
    MDKR_PARTY_DECODE_BUTTONS,
    MDKR_PARTY_DECODE_STICK,
    MDKR_PARTY_DECODE_NEUTRAL,
    MDKR_PARTY_DECODE_CHECKSUM
} MdkrPartyDecodeResult;

typedef struct MdkrPartyPadEdge {
    /* 1 means sample_sequence-1. Deltas are unique and ordered oldest first,
     * therefore strictly decreasing on the wire (for example 4, 2, 1). */
    uint8_t sequence_delta;
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
} MdkrPartyPadEdge;

typedef struct MdkrPartyPadPacket {
    uint8_t flags;
    uint32_t connection_sequence;
    uint32_t sample_sequence;
    uint32_t sender_time_ms;
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
    uint8_t edge_count;
    MdkrPartyPadEdge edges[MDKR_PARTY_PAD_MAX_EDGES];
} MdkrPartyPadPacket;

size_t mdkr_party_pad_encoded_size(uint8_t edge_count);
uint16_t mdkr_party_crc16(const uint8_t *bytes, size_t length);

bool mdkr_party_pad_encode(
    const MdkrPartyPadPacket *packet, uint8_t *output, size_t capacity,
    size_t *out_length);

/* Failure is atomic: `out_packet` is byte-for-byte unchanged on rejection. */
MdkrPartyDecodeResult mdkr_party_pad_decode(
    const uint8_t *bytes, size_t length, MdkrPartyPadPacket *out_packet);

const char *mdkr_party_decode_result_name(MdkrPartyDecodeResult result);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_PARTY_PROTOCOL_H */
