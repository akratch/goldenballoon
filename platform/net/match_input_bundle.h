/* Loss-resilient fixed carrier bundle: three canonical input frames. */
#ifndef MDKR_MATCH_INPUT_BUNDLE_H
#define MDKR_MATCH_INPUT_BUNDLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "session/session_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_INPUT_BUNDLE_VERSION 1u
#define MDKR_MATCH_INPUT_BUNDLE_FRAMES 3u
#define MDKR_MATCH_INPUT_BUNDLE_BYTES 64u

typedef struct MdkrMatchInputBundle {
    uint32_t match_epoch;
    uint32_t newest_tick;
    uint8_t frame_count;
    uint8_t slot_mask;
    /* Frame zero is newest; each following frame is one authored tick older. */
    MdkrPadSample frames[MDKR_MATCH_INPUT_BUNDLE_FRAMES]
                        [MDKR_SESSION_MAX_PLAYERS];
} MdkrMatchInputBundle;

bool mdkr_match_input_bundle_encode(
    const MdkrMatchInputBundle *bundle, uint8_t *bytes, size_t length);
bool mdkr_match_input_bundle_decode(
    const uint8_t *bytes, size_t length, MdkrMatchInputBundle *bundle);

#ifdef __cplusplus
}
#endif
#endif /* MDKR_MATCH_INPUT_BUNDLE_H */
