/* Fixed, endian-stable carrier payload for one authenticated match input. */
#ifndef MDKR_MATCH_INPUT_PACKET_H
#define MDKR_MATCH_INPUT_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_INPUT_PACKET_VERSION 1u
#define MDKR_MATCH_INPUT_PACKET_BYTES 24u

typedef struct MdkrMatchInputPacket {
    uint32_t match_epoch;
    uint32_t tick;
    uint8_t canonical_slot;
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
} MdkrMatchInputPacket;

bool mdkr_match_input_packet_encode(
    const MdkrMatchInputPacket *packet, uint8_t *bytes, size_t length);
bool mdkr_match_input_packet_decode(
    const uint8_t *bytes, size_t length, MdkrMatchInputPacket *packet);

#ifdef __cplusplus
}
#endif
#endif
