#include "local_listener_mix.h"

#include <limits.h>

static bool roster_viewports_valid(const MdkrNetRoster *roster) {
    bool seen[MDKR_MATCH_SLOTS] = {false, false, false, false};
    unsigned index;
    if (roster == NULL ||
        roster->canonical_player_count == 0u ||
        roster->canonical_player_count > MDKR_MATCH_SLOTS ||
        roster->viewport_count > roster->canonical_player_count) {
        return false;
    }
    for (index = 0u; index < roster->viewport_count; index++) {
        const uint8_t slot = roster->viewport_to_canonical[index];
        if (slot >= roster->canonical_player_count || seen[slot]) return false;
        seen[slot] = true;
    }
    return true;
}

bool mdkr_local_listener_mix_select(
    const MdkrNetRoster *roster,
    const int32_t candidate_volume[MDKR_MATCH_SLOTS],
    const uint8_t candidate_pan[MDKR_MATCH_SLOTS],
    unsigned canonical_candidate_count,
    int32_t fallback_volume,
    uint8_t fallback_pan,
    MdkrLocalListenerMix *out) {
    unsigned index;
    int32_t best_volume = INT32_MIN;
    uint8_t best_pan = 64u;
    if (out == NULL || canonical_candidate_count > MDKR_MATCH_SLOTS ||
        fallback_volume < 0 || fallback_pan > 128u) return false;

    out->volume = fallback_volume;
    out->pan = fallback_pan;
    out->listener_count = 0u;
    out->endpoint_local = false;
    if (roster == NULL) return true;
    if (!roster_viewports_valid(roster) ||
        canonical_candidate_count < roster->canonical_player_count ||
        candidate_volume == NULL || candidate_pan == NULL) return false;

    out->endpoint_local = true;
    out->listener_count = roster->viewport_count;
    if (roster->viewport_count == 0u) {
        out->volume = 0;
        out->pan = 64u;
        return true;
    }
    for (index = 0u; index < roster->viewport_count; index++) {
        const uint8_t slot = roster->viewport_to_canonical[index];
        if (candidate_volume[slot] < 0 || candidate_pan[slot] > 128u) {
            return false;
        }
        if (candidate_volume[slot] > best_volume) {
            best_volume = candidate_volume[slot];
            best_pan = candidate_pan[slot];
        }
    }
    out->volume = best_volume;
    out->pan = roster->viewport_count == 1u ? best_pan : 64u;
    return true;
}
