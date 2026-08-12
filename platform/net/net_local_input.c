#include "net_local_input.h"

#include <string.h>

static bool sample_valid(const MdkrInputSample *sample) {
    if (sample == NULL || sample->stick_x < -80 || sample->stick_x > 80 ||
        sample->stick_y < -80 || sample->stick_y > 80) {
        return false;
    }
    /* Absence has one wire meaning. Reject covert/stale input in a supposedly
     * absent seat rather than depending on every later consumer to mask it. */
    return sample->present ||
        (sample->buttons == 0u && sample->stick_x == 0 && sample->stick_y == 0);
}

static bool roster_mapping_valid(const MdkrNetRoster *roster) {
    bool seen[MDKR_MATCH_SLOTS] = {false, false, false, false};
    unsigned seat;
    if (roster == NULL || roster->canonical_player_count < 2u ||
        roster->canonical_player_count > MDKR_MATCH_SLOTS ||
        roster->local_seat_count > roster->canonical_player_count) {
        return false;
    }
    for (seat = 0u; seat < roster->local_seat_count; seat++) {
        const uint8_t slot = roster->local_to_canonical[seat];
        if (slot >= roster->canonical_player_count || seen[slot]) return false;
        seen[slot] = true;
    }
    for (; seat < MDKR_MATCH_SLOTS; seat++) {
        if (roster->local_to_canonical[seat] != MDKR_NET_ROSTER_UNMAPPED) {
            return false;
        }
    }
    return true;
}

bool mdkr_net_local_input_merge(
    const MdkrNetRoster *roster,
    const MdkrInputSample *local_samples,
    unsigned local_sample_count,
    const MdkrInputSample canonical_base[MDKR_MATCH_SLOTS],
    MdkrCanonicalInputFrame *out) {
    MdkrCanonicalInputFrame next;
    unsigned slot;
    unsigned seat;
    if (out == NULL || canonical_base == NULL ||
        !roster_mapping_valid(roster) ||
        local_sample_count != roster->local_seat_count ||
        (local_sample_count != 0u && local_samples == NULL)) {
        return false;
    }
    memset(&next, 0, sizeof(next));
    for (slot = 0u; slot < MDKR_MATCH_SLOTS; slot++) {
        if (!sample_valid(&canonical_base[slot])) return false;
        next.slots[slot] = canonical_base[slot];
    }
    for (seat = 0u; seat < local_sample_count; seat++) {
        const uint8_t canonical = roster->local_to_canonical[seat];
        if (!sample_valid(&local_samples[seat])) return false;
        next.slots[canonical] = local_samples[seat];
        next.locally_authored_mask |= (uint8_t)(1u << canonical);
    }
    *out = next;
    return true;
}
