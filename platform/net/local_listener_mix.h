/* Endpoint-local listener selection for presentation-only audio parameters. */
#ifndef MDKR_LOCAL_LISTENER_MIX_H
#define MDKR_LOCAL_LISTENER_MIX_H

#include <stdbool.h>
#include <stdint.h>

#include "net_roster.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrLocalListenerMix {
    int32_t volume;
    uint8_t pan;
    uint8_t listener_count;
    bool endpoint_local;
} MdkrLocalListenerMix;

/* `candidate_*[slot]` describes how canonical camera `slot` would hear one
 * source. Offline play receives the authored fallback unchanged. Online play
 * selects only frozen endpoint viewports: no views means silent physical
 * output, one view retains spatial pan, and a shared screen centers the mix.
 * This function is pure and never changes the roster or candidate arrays. */
bool mdkr_local_listener_mix_select(
    const MdkrNetRoster *roster,
    const int32_t candidate_volume[MDKR_MATCH_SLOTS],
    const uint8_t candidate_pan[MDKR_MATCH_SLOTS],
    unsigned canonical_candidate_count,
    int32_t fallback_volume,
    uint8_t fallback_pan,
    MdkrLocalListenerMix *out);

#ifdef __cplusplus
}
#endif
#endif
