#include "custom_track_header.h"
#include <limits.h>

static uint32_t read_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int mdkr_track_header_scalars_valid(const void *header, size_t size) {
    if (!header || size < MDKR_TRACK_HEADER_BYTES) return 0;
    const uint8_t *bytes = header;
    if ((bytes[0] > 6 && bytes[0] != 0xFF) || bytes[0x4D] > 2 ||
        bytes[0x4E] > 7 || bytes[0x9C] > 90 ||
        (read_be32(bytes + 8) & 0x7F800000u) == 0x7F800000u) return 0;
    switch (bytes[0x4C]) {
        case 0: case 1: case 3: case 5: case 6: case 7: case 8:
        case 64: case 65: case 66: return 1;
        default: return 0;
    }
}

int mdkr_track_header_sky_texture_valid(const void *header, size_t size,
    uint32_t retail_2d_count, uint32_t retail_3d_count) {
    if (!header || size < MDKR_TRACK_HEADER_BYTES ||
        retail_2d_count > 0x8000u || retail_3d_count > 0x8000u) return 0;
    const uint8_t *bytes = header;
    if (bytes[0x49] != 0xFF) return 1;
    const uint32_t id = read_be32(bytes + 0xA4) & 0xFFFFu;
    return (id & 0x7FFFu) < (id & 0x8000u ? retail_3d_count : retail_2d_count);
}

int mdkr_track_header_wave_texture_valid(const void *header, size_t size,
    uint32_t retail_2d_count, uint32_t retail_3d_count) {
    if (!header || size < MDKR_TRACK_HEADER_BYTES ||
        retail_2d_count > 0x8000u || retail_3d_count > 0x8000u) return 0;
    const uint8_t *bytes = header;
    const uint32_t id = (uint32_t)bytes[0x68] << 8 | bytes[0x69];
    return (id & 0x7FFFu) < (id & 0x8000u ? retail_3d_count : retail_2d_count);
}

int mdkr_track_header_wave_render_valid(const void *header, size_t size, unsigned players) {
    if (!header || size < MDKR_TRACK_HEADER_BYTES || (players != 1 && players != 2)) return 0;
    const uint8_t *bytes = header;
    const unsigned subdivisions = players == 2 ? 4 : bytes[0x56];
    return subdivisions > 0 && 2 * subdivisions <= 16 && 2 * (subdivisions + 1) <= 32;
}

int mdkr_track_header_wave_storage(const void *header, size_t size, unsigned players,
    size_t limit, size_t *required) {
    if (!header || size < MDKR_TRACK_HEADER_BYTES || !required ||
        (players != 1 && players != 2)) return 0;
    const uint8_t *bytes = header;
    const uint32_t seed = ((uint32_t)bytes[0x60] << 8 | bytes[0x61]) & ~1u;
    const uint32_t subdivisions = players == 2 ? 4 : bytes[0x56];
    const uint32_t tiles = bytes[0x57], buffers = players == 2 ? 4 : 2;
    /* func_800BC6C8 writes sp34[subdivisions] in a 128-element array. */
    if (!subdivisions || subdivisions >= 128 || !tiles || !seed || seed > INT16_MAX) return 0;
    const uint64_t side = subdivisions + 1;
    const uint64_t total = (uint64_t)seed * 4 + (uint64_t)tiles * tiles * 4 +
        side * side * (4 + 4 * 9 + 250 * buffers) +
        (uint64_t)subdivisions * subdivisions * 32 * buffers;
    if (total > SIZE_MAX || total > limit) return 0;
    *required = (size_t)total;
    return 1;
}

int mdkr_track_colour_cycle_valid(const void *data, size_t size) {
    const uint8_t *bytes = data;
    if (!bytes || size < 24) return 0;
    const uint32_t frames = read_be32(bytes);
    /* The consumer reads colours at 20 + 8*i and durations at 24 + 8*i.
     * Its last duration needs four bytes, not a full trailing eight-byte
     * pseudo-record (retail misc 58 deliberately ends after that word). */
    if (frames > INT32_MAX || frames > (size - 20) / 8) return 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t duration = read_be32(bytes + 24 + (size_t)i * 8);
        if ((!duration && frames > 1) || duration > (uint32_t)INT32_MAX - total) return 0;
        total += duration;
    }
    return 1;
}

int mdkr_track_pulse_cycle_valid(const void *data, size_t size) {
    const uint8_t *bytes = data;
    if (!bytes || size < 12) return 0;
    const uint32_t frames = (uint32_t)bytes[0] << 8 | bytes[1];
    if (!frames || frames > (size - 12) / 4) return 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const uint8_t *frame = bytes + 12 + (size_t)i * 4;
        const uint32_t duration = (uint32_t)frame[2] << 8 | frame[3];
        if ((!duration && frames > 1) || duration > UINT16_MAX - total) return 0;
        total += duration;
    }
    return 1;
}

int mdkr_track_header_misc_valid(const void *header, size_t size, uint32_t retail_misc_count) {
    if (!header || size < MDKR_TRACK_HEADER_BYTES || retail_misc_count > INT32_MAX) return 0;
    const uint8_t *bytes = header;
    for (unsigned i = 0; i < 8; ++i) {
        const uint8_t *field = bytes + (i < 7 ? 0x74 + 4 * i : 0xAC);
        const uint32_t id = read_be32(field);
        if (id != UINT32_MAX && id >= retail_misc_count) return 0;
    }
    return 1;
}

static int resolve(const uint8_t *field, int32_t replacement, uint32_t count, uint32_t retail_count,
                   uint16_t *resolved) {
    if (replacement < -1 || retail_count > count) return 0;
    uint32_t id = replacement == -1 ?
        ((uint32_t)field[0] << 8) | field[1] : (uint32_t)replacement;
    if (id > INT16_MAX || id >= (replacement == -1 ? retail_count : count)) return 0;
    *resolved = (uint16_t)id;
    return 1;
}

int mdkr_track_header_relocate(void *header, size_t size,
                             const MdkrTrackHeaderRelocation *relocation) {
    if (!header || !relocation || size < MDKR_TRACK_HEADER_BYTES) return 0;
    uint8_t *bytes = header;
    /* Consume all caller data before writing, including when input aliases. */
    MdkrTrackHeaderRelocation plan = *relocation;
    uint16_t model, collectables, structure;
    if (!resolve(bytes + 0x34, plan.model, plan.model_count, plan.retail_model_count, &model) ||
        !resolve(bytes + 0x36, plan.collectables, plan.object_map_count, plan.retail_object_map_count, &collectables) ||
        !resolve(bytes + 0xBA, plan.structure, plan.object_map_count, plan.retail_object_map_count, &structure)) return 0;
    bytes[0x34] = (uint8_t)(model >> 8); bytes[0x35] = (uint8_t)model;
    bytes[0x36] = (uint8_t)(collectables >> 8); bytes[0x37] = (uint8_t)collectables;
    bytes[0xBA] = (uint8_t)(structure >> 8); bytes[0xBB] = (uint8_t)structure;
    if (bytes[0] == 6) bytes[0] = 1;
    return 1;
}
