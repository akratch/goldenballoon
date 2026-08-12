/* Pure adapter from a consensus lobby snapshot to the engine descriptor. */
#ifndef MDKR_MATCH_LAUNCH_BUILDER_H
#define MDKR_MATCH_LAUNCH_BUILDER_H

#include "lobby_core.h"
#include "net/match_launch_descriptor.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mdkr_match_launch_descriptor_from_lobby(
    const MdkrOnlineLobby *lobby, const MdkrMatchManifestV1 *manifest,
    MdkrMatchLaunchDescriptorV1 *output);

#ifdef __cplusplus
}
#endif
#endif
