/* Frozen launcher-to-engine deterministic race choices; no room/UI types. */
#ifndef MDKR_MATCH_LAUNCH_DESCRIPTOR_H
#define MDKR_MATCH_LAUNCH_DESCRIPTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "match_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION 1u
#define MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES 148u
#define MDKR_MATCH_CHARACTER_COUNT 10u
#define MDKR_MATCH_PLAYER_VEHICLE_COUNT 3u
#define MDKR_MATCH_NO_CHARACTER UINT8_MAX
#define MDKR_MATCH_NO_VEHICLE UINT8_MAX

typedef struct MdkrMatchSeatSelectionV1 {
    uint32_t selection_revision;
    uint8_t character_id;
    uint8_t vehicle_id;
} MdkrMatchSeatSelectionV1;

typedef struct MdkrMatchLaunchDescriptorV1 {
    uint32_t version;
    MdkrMatchManifestV1 manifest;
    MdkrMatchSeatSelectionV1 selections[MDKR_MATCH_SLOTS];
} MdkrMatchLaunchDescriptorV1;

bool mdkr_match_launch_descriptor_validate(
    const MdkrMatchLaunchDescriptorV1 *descriptor);
bool mdkr_match_launch_descriptor_encode(
    const MdkrMatchLaunchDescriptorV1 *descriptor, uint8_t *output,
    size_t capacity);
bool mdkr_match_launch_descriptor_decode(
    const uint8_t *bytes, size_t length,
    MdkrMatchLaunchDescriptorV1 *output);
uint64_t mdkr_match_launch_descriptor_digest(
    const MdkrMatchLaunchDescriptorV1 *descriptor);

#ifdef __cplusplus
}
#endif
#endif
