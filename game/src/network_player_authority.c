#include "network_player_authority.h"

#ifdef NATIVE_PORT
#include "net/net_roster_runtime.h"

s32 mdkr_authoritative_player_count(s32 localPlayerCount) {
    if (localPlayerCount < 1 || localPlayerCount > 4) localPlayerCount = 1;
    return (s32) mdkr_net_roster_runtime_canonical_player_count(
        (u8) localPlayerCount);
}
#endif
