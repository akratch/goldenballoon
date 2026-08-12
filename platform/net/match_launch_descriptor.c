#include "match_launch_descriptor.h"

#include <string.h>

static void put32(uint8_t *output, uint32_t value) {
    output[0] = (uint8_t)(value >> 24u);
    output[1] = (uint8_t)(value >> 16u);
    output[2] = (uint8_t)(value >> 8u);
    output[3] = (uint8_t)value;
}

static uint32_t get32(const uint8_t *input) {
    return (uint32_t)input[0] << 24u | (uint32_t)input[1] << 16u |
           (uint32_t)input[2] << 8u | (uint32_t)input[3];
}

static uint32_t checksum(const uint8_t *bytes, size_t size) {
    uint32_t hash = UINT32_C(2166136261);
    size_t index;
    for (index = 0u; index < size; index++) {
        hash ^= bytes[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

bool mdkr_match_launch_descriptor_validate(
    const MdkrMatchLaunchDescriptorV1 *descriptor) {
    unsigned slot;
    if (descriptor == NULL ||
        descriptor->version != MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION ||
        !mdkr_match_manifest_validate(&descriptor->manifest)) return false;
    for (slot = 0u; slot < descriptor->manifest.slot_count; slot++) {
        const MdkrMatchSeatSelectionV1 *selection =
            &descriptor->selections[slot];
        unsigned other;
        if (selection->selection_revision == 0u ||
            selection->character_id >= MDKR_MATCH_CHARACTER_COUNT ||
            selection->vehicle_id >= MDKR_MATCH_PLAYER_VEHICLE_COUNT ||
            (descriptor->manifest.vehicle_mask &
             (uint8_t)(1u << selection->vehicle_id)) == 0u) return false;
        for (other = 0u; other < slot; other++) {
            if (descriptor->selections[other].character_id ==
                selection->character_id) return false;
        }
    }
    for (; slot < MDKR_MATCH_SLOTS; slot++) {
        const MdkrMatchSeatSelectionV1 *selection =
            &descriptor->selections[slot];
        if (selection->selection_revision != 0u ||
            selection->character_id != MDKR_MATCH_NO_CHARACTER ||
            selection->vehicle_id != MDKR_MATCH_NO_VEHICLE) return false;
    }
    return true;
}

bool mdkr_match_launch_descriptor_encode(
    const MdkrMatchLaunchDescriptorV1 *descriptor, uint8_t *output,
    size_t capacity) {
    unsigned slot;
    if (!mdkr_match_launch_descriptor_validate(descriptor) || output == NULL ||
        capacity < MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES) return false;
    memset(output, 0, MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES);
    output[0] = 'M'; output[1] = 'L'; output[2] = 'D'; output[3] = '1';
    output[4] = MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION;
    if (!mdkr_match_manifest_encode(
            &descriptor->manifest, output + 8u,
            MDKR_MATCH_MANIFEST_BYTES)) return false;
    for (slot = 0u; slot < MDKR_MATCH_SLOTS; slot++) {
        uint8_t *selection = output + 120u + slot * 6u;
        put32(selection, descriptor->selections[slot].selection_revision);
        selection[4] = descriptor->selections[slot].character_id;
        selection[5] = descriptor->selections[slot].vehicle_id;
    }
    put32(output + 144u, checksum(output, 144u));
    return true;
}

bool mdkr_match_launch_descriptor_decode(
    const uint8_t *bytes, size_t length,
    MdkrMatchLaunchDescriptorV1 *output) {
    MdkrMatchLaunchDescriptorV1 next;
    unsigned slot;
    if (bytes == NULL || output == NULL ||
        length != MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES ||
        bytes[0] != 'M' || bytes[1] != 'L' || bytes[2] != 'D' ||
        bytes[3] != '1' ||
        bytes[4] != MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION ||
        bytes[5] != 0u || bytes[6] != 0u || bytes[7] != 0u ||
        get32(bytes + 144u) != checksum(bytes, 144u)) return false;
    memset(&next, 0, sizeof(next));
    next.version = bytes[4];
    if (!mdkr_match_manifest_decode(
            bytes + 8u, MDKR_MATCH_MANIFEST_BYTES, &next.manifest))
        return false;
    for (slot = 0u; slot < MDKR_MATCH_SLOTS; slot++) {
        const uint8_t *selection = bytes + 120u + slot * 6u;
        next.selections[slot].selection_revision = get32(selection);
        next.selections[slot].character_id = selection[4];
        next.selections[slot].vehicle_id = selection[5];
    }
    if (!mdkr_match_launch_descriptor_validate(&next)) return false;
    *output = next;
    return true;
}

uint64_t mdkr_match_launch_descriptor_digest(
    const MdkrMatchLaunchDescriptorV1 *descriptor) {
    uint8_t bytes[MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES];
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    if (!mdkr_match_launch_descriptor_encode(
            descriptor, bytes, sizeof(bytes))) return 0u;
    for (index = 0u; index < sizeof(bytes); index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}
