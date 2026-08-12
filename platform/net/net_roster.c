#include "net_roster.h"

#include <string.h>

static bool unique_slots(const uint8_t *slots, unsigned count, unsigned limit) {
    bool seen[MDKR_MATCH_SLOTS] = {false, false, false, false};
    unsigned index;
    if (count > MDKR_MATCH_SLOTS || (count != 0u && slots == NULL)) return false;
    for (index = 0u; index < count; index++) {
        if (slots[index] >= limit || seen[slots[index]]) return false;
        seen[slots[index]] = true;
    }
    return true;
}

bool mdkr_net_roster_init(
    MdkrNetRoster *roster, const MdkrMatchManifestV1 *manifest) {
    unsigned left;
    unsigned right;
    if (roster == NULL || !mdkr_match_manifest_validate(manifest)) return false;
    for (left = 0u; left < manifest->slot_count; left++) {
        if (manifest->slot_owner[left] == 0u) return false;
        for (right = left + 1u; right < manifest->slot_count; right++) {
            if (manifest->slot_owner[left] == manifest->slot_owner[right]) return false;
        }
    }
    memset(roster, 0, sizeof(*roster));
    memset(roster->local_to_canonical, MDKR_NET_ROSTER_UNMAPPED,
           sizeof(roster->local_to_canonical));
    memset(roster->viewport_to_canonical, MDKR_NET_ROSTER_UNMAPPED,
           sizeof(roster->viewport_to_canonical));
    memcpy(roster->canonical_owner, manifest->slot_owner,
           sizeof(roster->canonical_owner));
    roster->canonical_player_count = manifest->slot_count;
    return true;
}

bool mdkr_net_roster_configure_local(
    MdkrNetRoster *roster, const uint8_t *canonical_slots,
    unsigned local_seat_count) {
    if (roster == NULL || !unique_slots(canonical_slots, local_seat_count,
                                        roster->canonical_player_count)) return false;
    memset(roster->local_to_canonical, MDKR_NET_ROSTER_UNMAPPED,
           sizeof(roster->local_to_canonical));
    if (local_seat_count != 0u) {
        memcpy(roster->local_to_canonical, canonical_slots, local_seat_count);
    }
    roster->local_seat_count = (uint8_t)local_seat_count;
    return true;
}

bool mdkr_net_roster_set_viewports(
    MdkrNetRoster *roster, const uint8_t *canonical_slots,
    unsigned viewport_count) {
    unsigned index;
    if (roster == NULL || !unique_slots(canonical_slots, viewport_count,
                                        roster->canonical_player_count)) return false;
    for (index = 0u; index < viewport_count; index++) {
        if (!mdkr_net_roster_is_local(roster, canonical_slots[index])) return false;
    }
    memset(roster->viewport_to_canonical, MDKR_NET_ROSTER_UNMAPPED,
           sizeof(roster->viewport_to_canonical));
    if (viewport_count != 0u) {
        memcpy(roster->viewport_to_canonical, canonical_slots, viewport_count);
    }
    roster->viewport_count = (uint8_t)viewport_count;
    return true;
}

bool mdkr_net_roster_is_local(
    const MdkrNetRoster *roster, unsigned canonical_slot) {
    unsigned index;
    if (roster == NULL || canonical_slot >= roster->canonical_player_count) return false;
    for (index = 0u; index < roster->local_seat_count; index++) {
        if (roster->local_to_canonical[index] == canonical_slot) return true;
    }
    return false;
}
