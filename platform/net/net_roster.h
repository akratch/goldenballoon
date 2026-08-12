/* Canonical match participants separated from endpoint-local seats/views. */
#ifndef MDKR_NET_ROSTER_H
#define MDKR_NET_ROSTER_H

#include <stdbool.h>
#include <stdint.h>

#include "match_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_NET_ROSTER_UNMAPPED 0xffu

typedef struct MdkrNetRoster {
    uint64_t canonical_owner[MDKR_MATCH_SLOTS];
    uint8_t local_to_canonical[MDKR_MATCH_SLOTS];
    uint8_t viewport_to_canonical[MDKR_MATCH_SLOTS];
    uint8_t canonical_player_count;
    uint8_t local_seat_count;
    uint8_t viewport_count;
} MdkrNetRoster;

bool mdkr_net_roster_init(
    MdkrNetRoster *roster, const MdkrMatchManifestV1 *manifest);
bool mdkr_net_roster_configure_local(
    MdkrNetRoster *roster, const uint8_t *canonical_slots,
    unsigned local_seat_count);
bool mdkr_net_roster_set_viewports(
    MdkrNetRoster *roster, const uint8_t *canonical_slots,
    unsigned viewport_count);
bool mdkr_net_roster_is_local(
    const MdkrNetRoster *roster, unsigned canonical_slot);

#ifdef __cplusplus
}
#endif
#endif
