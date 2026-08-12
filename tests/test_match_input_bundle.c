/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/match_input_bundle.h"

static MdkrPadSample sample(unsigned frame, unsigned slot) {
    const MdkrPadSample value = {
        (uint16_t)(0x1000u + frame * 0x100u + slot),
        (int8_t)(-20 + (int)frame + (int)slot),
        (int8_t)(30 - (int)frame - (int)slot), 1u};
    return value;
}

int main(void) {
    MdkrMatchInputBundle source;
    MdkrMatchInputBundle decoded;
    uint8_t bytes[MDKR_MATCH_INPUT_BUNDLE_BYTES];
    uint8_t mutated[MDKR_MATCH_INPUT_BUNDLE_BYTES];
    memset(&source, 0, sizeof(source));
    source.match_epoch = UINT32_C(0x01020304);
    source.newest_tick = 99u;
    source.frame_count = 3u;
    source.slot_mask = 0x0bu;
    for (unsigned frame = 0u; frame < source.frame_count; frame++) {
        for (unsigned slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; slot++) {
            if ((source.slot_mask & (1u << slot)) != 0u) {
                source.frames[frame][slot] = sample(frame, slot);
            }
        }
    }
    assert(mdkr_match_input_bundle_encode(&source, bytes, sizeof(bytes)));
    memset(&decoded, 0xa5, sizeof(decoded));
    assert(mdkr_match_input_bundle_decode(bytes, sizeof(bytes), &decoded));
    assert(memcmp(&source, &decoded, sizeof(source)) == 0);
    assert(bytes[6] == 1u && bytes[7] == 2u &&
           bytes[8] == 3u && bytes[9] == 4u);
    for (unsigned index = 0u; index < sizeof(bytes); index++) {
        MdkrMatchInputBundle untouched;
        memset(&untouched, 0xa5, sizeof(untouched));
        memcpy(mutated, bytes, sizeof(mutated));
        mutated[index] ^= 0x40u;
        assert(!mdkr_match_input_bundle_decode(
            mutated, sizeof(mutated), &untouched));
        for (unsigned byte = 0u; byte < sizeof(untouched); byte++) {
            assert(((const uint8_t *)&untouched)[byte] == 0xa5u);
        }
    }
    source.frames[0][2] = sample(0u, 2u);
    assert(!mdkr_match_input_bundle_encode(&source, bytes, sizeof(bytes)));
    source.frames[0][2] = (MdkrPadSample){0};
    source.frames[0][1].stick_x = 81;
    assert(!mdkr_match_input_bundle_encode(&source, bytes, sizeof(bytes)));
    assert(!mdkr_match_input_bundle_decode(bytes, sizeof(bytes) - 1u, &decoded));
    puts("test_match_input_bundle: PASS");
    return 0;
}
