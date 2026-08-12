#include "match_launch_builder.h"

#include <string.h>

_Static_assert(MDKR_ONLINE_CHARACTER_COUNT == MDKR_MATCH_CHARACTER_COUNT,
               "lobby and launch character catalogs must agree");
_Static_assert(
    MDKR_ONLINE_PLAYER_VEHICLE_COUNT == MDKR_MATCH_PLAYER_VEHICLE_COUNT,
    "lobby and launch vehicle catalogs must agree");

bool mdkr_match_launch_descriptor_from_lobby(
    const MdkrOnlineLobby *lobby, const MdkrMatchManifestV1 *manifest,
    MdkrMatchLaunchDescriptorV1 *output) {
    MdkrMatchLaunchDescriptorV1 next;
    unsigned source;
    unsigned target = 0u;
    if (lobby == NULL || manifest == NULL || output == NULL ||
        !mdkr_online_lobby_valid(lobby) ||
        lobby->phase != MDKR_ONLINE_LOADING ||
        lobby->match_epoch != manifest->match_epoch ||
        lobby->selected_track != manifest->track_id ||
        lobby->selected_vehicle_mask != manifest->vehicle_mask ||
        lobby->seat_count != manifest->slot_count) return false;
    memset(&next, 0, sizeof(next));
    next.version = MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION;
    next.manifest = *manifest;
    for (source = 0u; source < MDKR_ONLINE_MAX_SEATS; source++) {
        const MdkrOnlineSeat *seat = &lobby->seats[source];
        if (!seat->occupied) continue;
        if (target >= MDKR_MATCH_SLOTS) return false;
        next.selections[target].selection_revision = seat->selection_revision;
        next.selections[target].character_id = seat->character_id;
        next.selections[target].vehicle_id = seat->vehicle_id;
        target++;
    }
    for (; target < MDKR_MATCH_SLOTS; target++) {
        next.selections[target].character_id = MDKR_MATCH_NO_CHARACTER;
        next.selections[target].vehicle_id = MDKR_MATCH_NO_VEHICLE;
    }
    if (!mdkr_match_launch_descriptor_validate(&next)) return false;
    *output = next;
    return true;
}
