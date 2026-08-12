#ifndef MDKR_MATCH_MANIFEST_H
#define MDKR_MATCH_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_MANIFEST_VERSION 1u
#define MDKR_MATCH_MANIFEST_BYTES 112u
#define MDKR_MATCH_SLOTS 4u
#define MDKR_MATCH_RULES_STANDARD_RACE 1u
#define MDKR_MATCH_RACE_TYPE_STANDARD 0u

typedef enum MdkrRomRevisionId {
    MDKR_ROM_US_11 = 1,
    MDKR_ROM_EU_11 = 2
} MdkrRomRevisionId;

typedef struct MdkrMatchManifestV1 {
    uint32_t match_epoch;
    uint32_t protocol_version;
    uint8_t build_id[16];
    uint8_t gameplay_digest[32];
    uint64_t slot_owner[MDKR_MATCH_SLOTS];
    uint64_t rng_seed;
    uint16_t track_id;
    uint8_t rom_revision;
    uint8_t cadence_hz;
    uint8_t slot_count;
    uint8_t rules;
    uint8_t vehicle_mask;
    uint8_t input_delay;
} MdkrMatchManifestV1;

bool mdkr_match_manifest_validate(const MdkrMatchManifestV1 *manifest);
/* Final engine-side admission check against ROM-derived level metadata. A room
 * manifest is only a claim until the loaded ROM proves that the selected id is
 * the same authored-cadence standard race supported by protocol v1. */
bool mdkr_match_manifest_accepts_loaded_race(
    const MdkrMatchManifestV1 *manifest, uint16_t loaded_track_id,
    uint8_t loaded_race_type, uint8_t loaded_vehicle_mask,
    uint8_t authored_cadence_hz);
bool mdkr_match_manifest_encode(
    const MdkrMatchManifestV1 *manifest, uint8_t *output, size_t capacity);
bool mdkr_match_manifest_decode(
    const uint8_t *bytes, size_t length, MdkrMatchManifestV1 *output);
uint64_t mdkr_match_manifest_digest(const MdkrMatchManifestV1 *manifest);

#ifdef __cplusplus
}
#endif
#endif
