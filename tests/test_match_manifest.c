/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/match_manifest.h"

static uint16_t checksum16(const uint8_t *bytes, size_t size) {
    uint32_t hash = UINT32_C(2166136261);
    for (size_t index = 0u; index < size; index++) {
        hash ^= bytes[index]; hash *= UINT32_C(16777619);
    }
    return (uint16_t)hash;
}

int main(void) {
    MdkrMatchManifestV1 manifest;
    MdkrMatchManifestV1 decoded;
    uint8_t bytes[MDKR_MATCH_MANIFEST_BYTES];
    size_t index;
    memset(&manifest, 0, sizeof(manifest));
    manifest.match_epoch = 3u;
    manifest.protocol_version = 1u;
    manifest.build_id[0] = 0x42u;
    manifest.gameplay_digest[31] = 0x99u;
    manifest.slot_owner[0] = 11u;
    manifest.slot_owner[1] = 22u;
    manifest.rng_seed = UINT64_C(0x0123456789abcdef);
    manifest.track_id = 5u;
    manifest.rom_revision = MDKR_ROM_US_11;
    manifest.cadence_hz = 30u;
    manifest.slot_count = 2u;
    manifest.rules = 1u;
    manifest.vehicle_mask = 7u;
    manifest.input_delay = 2u;
    assert(mdkr_match_manifest_validate(&manifest));
    assert(mdkr_match_manifest_accepts_loaded_race(
        &manifest, 5u, MDKR_MATCH_RACE_TYPE_STANDARD, 7u, 30u));
    assert(!mdkr_match_manifest_accepts_loaded_race(
        &manifest, 6u, MDKR_MATCH_RACE_TYPE_STANDARD, 7u, 30u));
    assert(!mdkr_match_manifest_accepts_loaded_race(
        &manifest, 5u, 64u, 7u, 30u));
    assert(!mdkr_match_manifest_accepts_loaded_race(
        &manifest, 5u, MDKR_MATCH_RACE_TYPE_STANDARD, 7u, 25u));
    assert(!mdkr_match_manifest_accepts_loaded_race(
        &manifest, 5u, MDKR_MATCH_RACE_TYPE_STANDARD, 3u, 30u));
    assert(mdkr_match_manifest_encode(&manifest, bytes, sizeof(bytes)));
    memset(&decoded, 0xa5, sizeof(decoded));
    assert(mdkr_match_manifest_decode(bytes, sizeof(bytes), &decoded));
    assert(memcmp(&manifest, &decoded, sizeof(manifest)) == 0);
    assert(mdkr_match_manifest_digest(&manifest) != 0u);
    for (index = 0u; index < sizeof(bytes); index++) {
        uint8_t mutated[MDKR_MATCH_MANIFEST_BYTES];
        MdkrMatchManifestV1 untouched;
        memset(&untouched, 0x5a, sizeof(untouched));
        memcpy(mutated, bytes, sizeof(mutated));
        mutated[index] ^= 1u;
        assert(!mdkr_match_manifest_decode(mutated, sizeof(mutated), &untouched));
        for (size_t byte = 0u; byte < sizeof(untouched); byte++) {
            assert(((uint8_t *)&untouched)[byte] == 0x5au);
        }
    }
    /* A sender cannot smuggle a future field by merely recomputing checksum. */
    bytes[109] = 1u;
    { const uint16_t sum = checksum16(bytes, 110u);
      bytes[110] = (uint8_t)(sum >> 8); bytes[111] = (uint8_t)sum; }
    assert(!mdkr_match_manifest_decode(bytes, sizeof(bytes), &decoded));
    manifest.build_id[0] = 0u;
    assert(!mdkr_match_manifest_validate(&manifest));
    manifest.build_id[0] = 0x42u;
    manifest.rules = 0u;
    assert(!mdkr_match_manifest_validate(&manifest));
    manifest.rules = MDKR_MATCH_RULES_STANDARD_RACE;
    manifest.slot_owner[1] = manifest.slot_owner[0];
    assert(!mdkr_match_manifest_validate(&manifest));
    puts("test_match_manifest: PASS");
    return 0;
}
