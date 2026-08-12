#include "net_roster_runtime.h"

#include <string.h>

static MdkrNetRoster sRoster;
static MdkrMatchManifestV1 sManifest;
static MdkrMatchLaunchDescriptorV1 sLaunch;
static bool sActive;
static bool sHaveLaunch;

static bool roster_valid(const MdkrNetRoster *roster) {
    unsigned index;
    if (roster == NULL || roster->canonical_player_count < 2u ||
        roster->canonical_player_count > MDKR_MATCH_SLOTS ||
        roster->local_seat_count > roster->canonical_player_count ||
        roster->viewport_count > roster->local_seat_count) {
        return false;
    }
    for (index = 0u; index < roster->canonical_player_count; index++) {
        unsigned other;
        if (roster->canonical_owner[index] == 0u) return false;
        for (other = 0u; other < index; other++) {
            if (roster->canonical_owner[index] ==
                roster->canonical_owner[other]) return false;
        }
    }
    for (index = roster->canonical_player_count;
         index < MDKR_MATCH_SLOTS; index++) {
        if (roster->canonical_owner[index] != 0u) return false;
    }
    for (index = 0u; index < roster->local_seat_count; index++) {
        unsigned other;
        if (roster->local_to_canonical[index] >=
            roster->canonical_player_count) return false;
        for (other = 0u; other < index; other++) {
            if (roster->local_to_canonical[index] ==
                roster->local_to_canonical[other]) return false;
        }
    }
    for (index = roster->local_seat_count; index < MDKR_MATCH_SLOTS; index++) {
        if (roster->local_to_canonical[index] !=
            MDKR_NET_ROSTER_UNMAPPED) return false;
    }
    for (index = 0u; index < roster->viewport_count; index++) {
        if (!mdkr_net_roster_is_local(
                roster, roster->viewport_to_canonical[index])) return false;
    }
    for (index = roster->viewport_count; index < MDKR_MATCH_SLOTS; index++) {
        if (roster->viewport_to_canonical[index] !=
            MDKR_NET_ROSTER_UNMAPPED) return false;
    }
    return true;
}

bool mdkr_net_roster_runtime_install(
    const MdkrMatchManifestV1 *manifest, const MdkrNetRoster *roster) {
    unsigned slot;
    if (sActive || !mdkr_match_manifest_validate(manifest) ||
        !roster_valid(roster) ||
        manifest->slot_count != roster->canonical_player_count) return false;
    for (slot = 0u; slot < MDKR_MATCH_SLOTS; slot++) {
        if (manifest->slot_owner[slot] != roster->canonical_owner[slot]) {
            return false;
        }
    }
    sManifest = *manifest;
    memset(&sLaunch, 0, sizeof(sLaunch));
    sRoster = *roster;
    sActive = true;
    sHaveLaunch = false;
    return true;
}

bool mdkr_net_roster_runtime_install_launch(
    const MdkrMatchLaunchDescriptorV1 *launch,
    const MdkrNetRoster *roster) {
    if (!mdkr_match_launch_descriptor_validate(launch) ||
        !mdkr_net_roster_runtime_install(
            launch != NULL ? &launch->manifest : NULL, roster)) {
        return false;
    }
    sLaunch = *launch;
    sHaveLaunch = true;
    return true;
}

void mdkr_net_roster_runtime_clear(void) {
    memset(&sManifest, 0, sizeof(sManifest));
    memset(&sLaunch, 0, sizeof(sLaunch));
    memset(&sRoster, 0, sizeof(sRoster));
    sActive = false;
    sHaveLaunch = false;
}

bool mdkr_net_roster_runtime_active(void) {
    return sActive;
}

const MdkrNetRoster *mdkr_net_roster_runtime_get(void) {
    return sActive ? &sRoster : NULL;
}

const MdkrMatchManifestV1 *mdkr_net_roster_runtime_manifest(void) {
    return sActive ? &sManifest : NULL;
}

const MdkrMatchLaunchDescriptorV1 *
mdkr_net_roster_runtime_launch_descriptor(void) {
    return sActive && sHaveLaunch ? &sLaunch : NULL;
}

const MdkrMatchSeatSelectionV1 *mdkr_net_roster_runtime_selection(
    unsigned canonical_slot) {
    if (!sActive || !sHaveLaunch ||
        canonical_slot >= sManifest.slot_count) return NULL;
    return &sLaunch.selections[canonical_slot];
}

uint8_t mdkr_net_roster_runtime_canonical_player_count(uint8_t fallback) {
    return sActive ? sRoster.canonical_player_count : fallback;
}

uint8_t mdkr_net_roster_runtime_viewport_count(uint8_t fallback) {
    return sActive ? sRoster.viewport_count : fallback;
}

bool mdkr_net_roster_runtime_local_to_canonical(
    unsigned local_seat, uint8_t *canonical_slot) {
    if (!sActive || canonical_slot == NULL ||
        local_seat >= sRoster.local_seat_count) return false;
    *canonical_slot = sRoster.local_to_canonical[local_seat];
    return true;
}

bool mdkr_net_roster_runtime_viewport_to_canonical(
    unsigned viewport, uint8_t *canonical_slot) {
    if (!sActive || canonical_slot == NULL ||
        viewport >= sRoster.viewport_count) return false;
    *canonical_slot = sRoster.viewport_to_canonical[viewport];
    return true;
}

bool mdkr_net_roster_runtime_canonical_to_local(
    unsigned canonical_slot, uint8_t *local_seat) {
    unsigned index;
    if (!sActive || local_seat == NULL ||
        canonical_slot >= sRoster.canonical_player_count) return false;
    for (index = 0u; index < sRoster.local_seat_count; index++) {
        if (sRoster.local_to_canonical[index] == canonical_slot) {
            *local_seat = (uint8_t)index;
            return true;
        }
    }
    return false;
}
