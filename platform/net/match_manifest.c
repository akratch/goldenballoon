#include "match_manifest.h"

#include <string.h>

static void put16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8); out[1] = (uint8_t)value;
}
static void put32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24); out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8); out[3] = (uint8_t)value;
}
static void put64(uint8_t *out, uint64_t value) {
    unsigned index;
    for (index = 0u; index < 8u; index++) out[index] = (uint8_t)(value >> (56u - index * 8u));
}
static uint16_t get16(const uint8_t *in) { return (uint16_t)((uint16_t)in[0] << 8 | in[1]); }
static uint32_t get32(const uint8_t *in) {
    return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 |
           (uint32_t)in[2] << 8 | in[3];
}
static uint64_t get64(const uint8_t *in) {
    uint64_t value = 0u; unsigned index;
    for (index = 0u; index < 8u; index++) value = value << 8 | in[index];
    return value;
}
static uint32_t checksum(const uint8_t *bytes, size_t size) {
    uint32_t hash = UINT32_C(2166136261); size_t index;
    for (index = 0u; index < size; index++) { hash ^= bytes[index]; hash *= UINT32_C(16777619); }
    return hash;
}

static bool any_nonzero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0u;
    size_t index;
    for (index = 0u; index < size; index++) combined |= bytes[index];
    return combined != 0u;
}

bool mdkr_match_manifest_validate(const MdkrMatchManifestV1 *manifest) {
    unsigned slot;
    if (manifest == NULL || manifest->match_epoch == 0u ||
        manifest->protocol_version == 0u || manifest->track_id > 65u ||
        (manifest->rom_revision != MDKR_ROM_US_11 &&
         manifest->rom_revision != MDKR_ROM_EU_11) ||
        (manifest->cadence_hz != 25u && manifest->cadence_hz != 30u) ||
        manifest->slot_count < 2u || manifest->slot_count > MDKR_MATCH_SLOTS ||
        manifest->rules != MDKR_MATCH_RULES_STANDARD_RACE ||
        manifest->vehicle_mask == 0u ||
        (manifest->vehicle_mask & ~7u) != 0u || manifest->input_delay > 8u ||
        !any_nonzero(manifest->build_id, sizeof(manifest->build_id)) ||
        !any_nonzero(manifest->gameplay_digest,
                     sizeof(manifest->gameplay_digest))) return false;
    for (slot = 0u; slot < manifest->slot_count; slot++) {
        unsigned other;
        if (manifest->slot_owner[slot] == 0u) return false;
        for (other = 0u; other < slot; other++) {
            if (manifest->slot_owner[slot] == manifest->slot_owner[other]) return false;
        }
    }
    for (; slot < MDKR_MATCH_SLOTS; slot++) {
        if (manifest->slot_owner[slot] != 0u) return false;
    }
    return true;
}

bool mdkr_match_manifest_accepts_loaded_race(
    const MdkrMatchManifestV1 *manifest, uint16_t loaded_track_id,
    uint8_t loaded_race_type, uint8_t loaded_vehicle_mask,
    uint8_t authored_cadence_hz) {
    return mdkr_match_manifest_validate(manifest) &&
        manifest->track_id == loaded_track_id &&
        loaded_race_type == MDKR_MATCH_RACE_TYPE_STANDARD &&
        manifest->vehicle_mask == loaded_vehicle_mask &&
        manifest->cadence_hz == authored_cadence_hz;
}

bool mdkr_match_manifest_encode(
    const MdkrMatchManifestV1 *manifest, uint8_t *output, size_t capacity) {
    unsigned slot;
    if (!mdkr_match_manifest_validate(manifest) || output == NULL ||
        capacity < MDKR_MATCH_MANIFEST_BYTES) return false;
    memset(output, 0, MDKR_MATCH_MANIFEST_BYTES);
    output[0] = 'G'; output[1] = 'B'; output[2] = 'M'; output[3] = 'F';
    output[4] = MDKR_MATCH_MANIFEST_VERSION;
    output[5] = manifest->rom_revision; output[6] = manifest->cadence_hz;
    output[7] = manifest->slot_count;
    put32(output + 8, manifest->match_epoch);
    put32(output + 12, manifest->protocol_version);
    memcpy(output + 16, manifest->build_id, 16u);
    memcpy(output + 32, manifest->gameplay_digest, 32u);
    for (slot = 0u; slot < MDKR_MATCH_SLOTS; slot++) put64(output + 64u + slot * 8u,
                                                            manifest->slot_owner[slot]);
    put64(output + 96, manifest->rng_seed);
    put16(output + 104, manifest->track_id);
    output[106] = manifest->rules;
    output[107] = manifest->vehicle_mask;
    output[108] = manifest->input_delay;
    put16(output + 110, (uint16_t)(checksum(output, 110u) & 0xffffu));
    return true;
}

bool mdkr_match_manifest_decode(
    const uint8_t *bytes, size_t length, MdkrMatchManifestV1 *output) {
    MdkrMatchManifestV1 value;
    unsigned slot;
    if (bytes == NULL || output == NULL || length != MDKR_MATCH_MANIFEST_BYTES ||
        bytes[0] != 'G' || bytes[1] != 'B' || bytes[2] != 'M' || bytes[3] != 'F' ||
        bytes[4] != MDKR_MATCH_MANIFEST_VERSION ||
        bytes[109] != 0u ||
        get16(bytes + 110) != (uint16_t)(checksum(bytes, 110u) & 0xffffu)) return false;
    memset(&value, 0, sizeof(value));
    value.rom_revision = bytes[5]; value.cadence_hz = bytes[6];
    value.slot_count = bytes[7]; value.match_epoch = get32(bytes + 8);
    value.protocol_version = get32(bytes + 12);
    memcpy(value.build_id, bytes + 16, 16u);
    memcpy(value.gameplay_digest, bytes + 32, 32u);
    for (slot = 0u; slot < MDKR_MATCH_SLOTS; slot++) value.slot_owner[slot] =
        get64(bytes + 64u + slot * 8u);
    value.rng_seed = get64(bytes + 96);
    value.track_id = get16(bytes + 104); value.rules = bytes[106];
    value.vehicle_mask = bytes[107]; value.input_delay = bytes[108];
    if (!mdkr_match_manifest_validate(&value)) return false;
    *output = value;
    return true;
}

uint64_t mdkr_match_manifest_digest(const MdkrMatchManifestV1 *manifest) {
    uint8_t bytes[MDKR_MATCH_MANIFEST_BYTES];
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    if (!mdkr_match_manifest_encode(manifest, bytes, sizeof(bytes))) return 0u;
    for (index = 0u; index < sizeof(bytes); index++) {
        hash ^= bytes[index]; hash *= UINT64_C(1099511628211);
    }
    return hash;
}
