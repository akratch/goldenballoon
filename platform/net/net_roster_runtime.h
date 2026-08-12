/* Engine-lifetime publication of a launcher-owned, validated network roster. */
#ifndef MDKR_NET_ROSTER_RUNTIME_H
#define MDKR_NET_ROSTER_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "net_roster.h"
#include "match_launch_descriptor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install is intentionally copy-based: the engine never borrows launcher
 * memory and cannot mutate matchmaking/session state. Install before engine
 * threads start; clear only after they have joined. */
bool mdkr_net_roster_runtime_install(
    const MdkrMatchManifestV1 *manifest, const MdkrNetRoster *roster);
/* Player-visible online path. The validated descriptor is copied with the
 * roster; launcher memory is never borrowed by game code. */
bool mdkr_net_roster_runtime_install_launch(
    const MdkrMatchLaunchDescriptorV1 *launch,
    const MdkrNetRoster *roster);
void mdkr_net_roster_runtime_clear(void);
bool mdkr_net_roster_runtime_active(void);
const MdkrNetRoster *mdkr_net_roster_runtime_get(void);
const MdkrMatchManifestV1 *mdkr_net_roster_runtime_manifest(void);
const MdkrMatchLaunchDescriptorV1 *
mdkr_net_roster_runtime_launch_descriptor(void);
const MdkrMatchSeatSelectionV1 *mdkr_net_roster_runtime_selection(
    unsigned canonical_slot);

/* Authoritative gameplay calls this with its ordinary local-play value. The
 * fallback preserves retail/local behavior when no online roster is active. */
uint8_t mdkr_net_roster_runtime_canonical_player_count(uint8_t fallback);
uint8_t mdkr_net_roster_runtime_viewport_count(uint8_t fallback);
bool mdkr_net_roster_runtime_local_to_canonical(
    unsigned local_seat, uint8_t *canonical_slot);
bool mdkr_net_roster_runtime_viewport_to_canonical(
    unsigned viewport, uint8_t *canonical_slot);
bool mdkr_net_roster_runtime_canonical_to_local(
    unsigned canonical_slot, uint8_t *local_seat);

#ifdef __cplusplus
}
#endif
#endif
