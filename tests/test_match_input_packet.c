/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/match_input_packet.h"

int main(void) {
    MdkrMatchInputPacket source = {
        .match_epoch = UINT32_C(0x01020304),
        .tick = UINT32_C(0xa1b2c3d4),
        .canonical_slot = 2u,
        .buttons = UINT16_C(0x9041), .stick_x = -101, .stick_y = 99,
    };
    MdkrMatchInputPacket decoded;
    uint8_t bytes[MDKR_MATCH_INPUT_PACKET_BYTES];
    uint8_t mutated[MDKR_MATCH_INPUT_PACKET_BYTES];
    assert(mdkr_match_input_packet_encode(&source, bytes, sizeof(bytes)));
    memset(&decoded, 0xa5, sizeof(decoded));
    assert(mdkr_match_input_packet_decode(bytes, sizeof(bytes), &decoded));
    assert(memcmp(&source, &decoded, sizeof(source)) == 0);
    assert(bytes[8] == 1u && bytes[9] == 2u && bytes[10] == 3u &&
           bytes[11] == 4u);
    for (unsigned index = 0u; index < sizeof(bytes); index++) {
        MdkrMatchInputPacket untouched;
        memset(&untouched, 0xa5, sizeof(untouched));
        memcpy(mutated, bytes, sizeof(mutated));
        mutated[index] ^= 0x40u;
        assert(!mdkr_match_input_packet_decode(
            mutated, sizeof(mutated), &untouched));
        for (unsigned byte = 0u; byte < sizeof(untouched); byte++) {
            assert(((const uint8_t *)&untouched)[byte] == 0xa5u);
        }
    }
    source.canonical_slot = 4u;
    assert(!mdkr_match_input_packet_encode(&source, bytes, sizeof(bytes)));
    assert(!mdkr_match_input_packet_decode(bytes, sizeof(bytes) - 1u, &decoded));
    puts("test_match_input_packet: PASS");
    return 0;
}
