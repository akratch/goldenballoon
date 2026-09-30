#include "custom_track_model.h"
#include "custom_track_header.h"

static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static uint32_t be16(const unsigned char *p) {
    return (uint32_t)p[0] << 8 | p[1];
}

static int32_t signed_be16(const unsigned char *p) {
    const uint32_t value = be16(p);
    return value >= 0x8000u ? (int32_t)value - 0x10000 : (int32_t)value;
}

int mdkr_track_model_lighting_links_valid(const void *model, size_t size,
    const void *header, size_t header_size) {
    if (!mdkr_track_model_validate_indices(model, size)) return 0;
    const unsigned char *bytes = model, *h = header;
    for (uint32_t i = 0; i < be16(bytes + 0x1A); ++i) {
        const unsigned char *segment = bytes + be32(bytes + 4) + i * 0x44;
        const unsigned char *batches = bytes + be32(segment + 0x0C);
        for (uint32_t j = 0; j < be16(segment + 0x20); ++j) {
            const uint32_t flags = be32(batches + j * 12 + 8);
            const uint32_t colour = (flags >> 28) & 7u;
            const int pulse = (flags & 0x00040000u) != 0;
            if (!colour && !pulse) continue;
            if (!h || header_size < MDKR_TRACK_HEADER_BYTES) return 0;
            if (colour && be32(h + 0x74 + 4 * (colour - 1)) == UINT32_MAX) return 0;
            if (pulse && (be32(h + 0xAC) == UINT32_MAX || batches[j * 12] == 0xFF)) return 0;
        }
    }
    return 1;
}

int mdkr_track_model_pulse_textures_valid(const void *model, size_t size,
    MdkrTrackPulseTextureCheck check, void *context) {
    if (!mdkr_track_model_validate_indices(model, size)) return 0;
    const unsigned char *bytes = model;
    unsigned char selected[MDKR_TRACK_MODEL_TEXTURES_MAX] = {0};
    for (uint32_t i = 0; i < be16(bytes + 0x1A); ++i) {
        const unsigned char *segment = bytes + be32(bytes + 4) + i * 0x44;
        const unsigned char *batches = bytes + be32(segment + 0x0C);
        for (uint32_t j = 0; j < be16(segment + 0x20); ++j) {
            const unsigned char *batch = batches + j * 12;
            if (!(be32(batch + 8) & 0x00040000u)) continue;
            /* Track rendering skips hidden and empty batches before the
             * specialized upload. Their texture layout is not consumed. */
            if ((be32(batch + 8) & 0x00000100u) ||
                be16(batch + 2) == be16(batch + 14) ||
                be16(batch + 4) == be16(batch + 16)) continue;
            if (batch[0] == 0xFF || !check) return 0;
            selected[batch[0]] = 1;
        }
    }
    for (uint32_t i = 0; i < be16(bytes + 0x18); ++i)
        if (selected[i] && !check(context, be32(bytes + be32(bytes) + i * 8))) return 0;
    return 1;
}

int mdkr_track_model_wave_grid(const void *model, size_t size, MdkrTrackWaveGrid *out) {
    uint32_t waves = 0;
    if (!out || !mdkr_track_model_validate_indices(model, size) ||
        !mdkr_track_model_wave_count(model, size, &waves)) return 0;
    if (!waves) { *out = (MdkrTrackWaveGrid){0}; return 1; }
    const unsigned char *bytes = model;
    const uint32_t segments = be16(bytes + 0x1A), offset = be32(bytes + 4);
    int selected = 0, basis_valid = 0;
    const unsigned char *basis = NULL;
    uint32_t colour_slot = 0;
    for (uint32_t i = 0; i < segments; ++i) {
        const unsigned char *segment = bytes + offset + i * 0x44;
        const unsigned char *batches = bytes + be32(segment + 0x0C);
        for (uint32_t j = 0; j < be16(segment + 0x20); ++j) {
            const unsigned char *batch = batches + j * 12;
            const uint32_t flags = be32(batch + 8);
            /* Grid setup reads the height vertex of every such batch, even
             * in unflagged segments and after the selected basis batch. */
            if ((flags & 0x00402000u) == 0x00402000u &&
                be16(batch + 2) >= be16(segment + 0x1C)) return 0;
            /* RENDER_UNK_1000000 | RENDER_WATER, excluding RENDER_HIDDEN. */
            if (selected || (flags & 0x01002100u) != 0x01002000u) continue;
            selected = 1;
            colour_slot = (flags >> 28) & 7u;
            const unsigned char *box = bytes + be32(bytes + 8) + i * 12;
            basis = box;
            basis_valid = batch[0] != 0xFF &&
                signed_be16(box + 6) > signed_be16(box) &&
                signed_be16(box + 10) > signed_be16(box + 4);
        }
    }
    if (!basis_valid) return 0;
    const unsigned char *boxes = bytes + be32(bytes + 8);
    int32_t min_x = signed_be16(boxes), min_z = signed_be16(boxes + 4);
    int32_t max_x = signed_be16(boxes + 6), max_z = signed_be16(boxes + 10);
    for (uint32_t i = 0; i < segments; ++i) {
        const unsigned char *box = boxes + i * 12;
        int32_t x1 = signed_be16(box), z1 = signed_be16(box + 4);
        int32_t x2 = signed_be16(box + 6), z2 = signed_be16(box + 10);
        if (x2 < x1 || z2 < z1) return 0;
        if (x1 < min_x) min_x = x1;
        if (z1 < min_z) min_z = z1;
        if (x2 > max_x) max_x = x2;
        if (z2 > max_z) max_z = z2;
    }
    const int32_t width = signed_be16(basis + 6) - signed_be16(basis);
    const int32_t depth = signed_be16(basis + 10) - signed_be16(basis + 4);
    int32_t origin_x = signed_be16(basis), origin_z = signed_be16(basis + 4);
    /* Closed form of native repeated subtraction. Signed BE16 coordinates
     * keep these differences and products comfortably inside int32. */
    origin_x -= ((origin_x - min_x + width - 1) / width) * width;
    origin_z -= ((origin_z - min_z + depth - 1) / depth) * depth;
    const int32_t columns = (max_x - origin_x) / width + 1;
    const int32_t rows = (max_z - origin_z) / depth + 1;
    for (uint32_t i = 0; i < segments; ++i) {
        const unsigned char *box = boxes + i * 12;
        const int32_t x = (signed_be16(box) - origin_x + 8) / width;
        const int32_t z = (signed_be16(box + 4) - origin_z + 8) / depth;
        const int32_t segment_x = x * width + origin_x;
        const int32_t segment_z = z * depth + origin_z;
        /* The runtime stores these aligned origins in s16 fields. Valid
         * authored s16 bounds do not imply that their aligned origins fit. */
        if (segment_x < INT16_MIN || segment_x > INT16_MAX ||
            segment_z < INT16_MIN || segment_z > INT16_MAX) return 0;
        /* Runtime reads all segment row masks, not just hasWaves segments.
         * Columns above 31 alias under DKR_SHL32's masked shift. */
        if (x >= columns || z >= rows || x >= 32 || z >= 64) return 0;
    }
    *out = (MdkrTrackWaveGrid){segments, (uint32_t)columns, (uint32_t)rows, colour_slot};
    return 1;
}

int mdkr_track_model_wave_basis_valid(const void *model, size_t size) {
    MdkrTrackWaveGrid grid;
    return mdkr_track_model_wave_grid(model, size, &grid);
}

static int span(uint32_t offset, uint32_t count, uint32_t stride,
                uint32_t extent, uint32_t alignment) {
    if (!count) return 1;
    return offset >= 0x4C && offset % alignment == 0 && offset <= extent &&
           count <= (extent - offset) / stride;
}

int mdkr_track_model_validate_spans(const void *model, size_t size) {
    if (!model || size < 0x4C) return 0;
    const unsigned char *bytes = model;
    const uint32_t extent = be32(bytes + 0x48);
    const uint32_t textures = be16(bytes + 0x18), segments = be16(bytes + 0x1A);
    const uint32_t segment_offset = be32(bytes + 4);
    if (extent < 0x4C || extent > size || textures > 255 ||
        !segments || segments > 128 ||
        !span(be32(bytes), textures, 8, extent, 4) ||
        !span(segment_offset, segments, 0x44, extent, 4) ||
        !span(be32(bytes + 8), segments, 12, extent, 2) ||
        !span(be32(bytes + 0x14), segments - 1, 8, extent, 2)) return 0;
    for (uint32_t i = 0; i < segments; ++i) {
        const unsigned char *s = bytes + segment_offset + i * 0x44;
        const uint32_t vertices = be16(s + 0x1C), triangles = be16(s + 0x1E);
        const uint32_t batches = be16(s + 0x20);
        if (vertices > 0x7FFF || triangles > 0x7FFF || batches > 0x7FFF ||
            !span(be32(s), vertices, 10, extent, 2) ||
            !span(be32(s + 4), triangles, 16, extent, 2) ||
            !span(be32(s + 0x0C), batches + 1, 12, extent, 4) ||
            !span(be32(s + 0x14), triangles, 8, extent, 2)) return 0;
    }
    return 1;
}

int mdkr_track_model_wave_count(const void *model, size_t size, uint32_t *count) {
    if (!count || !mdkr_track_model_validate_spans(model, size)) return 0;
    const unsigned char *bytes = model;
    const uint32_t segments = be16(bytes + 0x1A), offset = be32(bytes + 4);
    uint32_t waves = 0;
    for (uint32_t i = 0; i < segments; ++i)
        if (bytes[offset + i * 0x44 + 0x2B] != 0) ++waves;
    *count = waves;
    return 1;
}

int mdkr_track_model_validate_indices(const void *model, size_t size) {
    if (!mdkr_track_model_validate_spans(model, size)) return 0;
    const unsigned char *bytes = model;
    const uint32_t textures = be16(bytes + 0x18), segments = be16(bytes + 0x1A);
    for (uint32_t i = 0; i < segments; ++i) {
        const unsigned char *s = bytes + be32(bytes + 4) + i * 0x44;
        const uint32_t vertices = be16(s + 0x1C), triangles = be16(s + 0x1E);
        const uint32_t count = be16(s + 0x20);
        const unsigned char *batches = bytes + be32(s + 0x0C);
        if (s[0x40] > count) return 0;
        for (uint32_t j = 0; j <= count; ++j) {
            const unsigned char *b = batches + j * 12;
            const uint32_t vertex = be16(b + 2), face = be16(b + 4);
            if (vertex > vertices || face > triangles) return 0;
            if (j == count) break; /* Sentinel has offsets, not material policy. */
            const uint32_t next_vertex = be16(b + 14), next_face = be16(b + 16);
            if (next_vertex < vertex || next_vertex > vertices ||
                next_face < face || next_face > triangles ||
                (b[0] != 0xFF && b[0] >= textures)) return 0;
            for (uint32_t f = face; f < next_face; ++f) {
                const unsigned char *t = bytes + be32(s + 4) + f * 16;
                for (unsigned corner = 1; corner <= 3; ++corner)
                    if (t[corner] >= next_vertex - vertex) return 0;
            }
        }
    }
    return 1;
}

int mdkr_track_model_validate_draw_batches(const void *model, size_t size) {
    if (!mdkr_track_model_validate_indices(model, size)) return 0;
    const unsigned char *bytes = model;
    const uint32_t segments = be16(bytes + 0x1A);
    for (uint32_t i = 0; i < segments; ++i) {
        const unsigned char *segment = bytes + be32(bytes + 4) + i * 0x44;
        const uint32_t count = be16(segment + 0x20);
        const unsigned char *batches = bytes + be32(segment + 0x0C);
        for (uint32_t j = 0; j < count; ++j) {
            const unsigned char *batch = batches + j * 12;
            const uint32_t vertices = be16(batch + 14) - be16(batch + 2);
            const uint32_t faces = be16(batch + 16) - be16(batch + 4);
            if (faces && (!vertices || vertices > 32 || faces > 16)) return 0;
        }
    }
    return 1;
}

int mdkr_track_model_validate_visibility(const void *model, size_t size) {
    if (!mdkr_track_model_validate_indices(model, size)) return 0;
    const unsigned char *bytes = model;
    const uint32_t count = be16(bytes + 0x1A), extent = be32(bytes + 0x48);
    const uint32_t pvs = be32(bytes + 0x10), row_bytes = (count + 7) / 8;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *s = bytes + be32(bytes + 4) + i * 0x44;
        const uint32_t row = be16(s + 0x28);
        if (row == 0xFFFF) continue; /* Renderer explicitly disables PVS. */
        if (row > 0x7FFF || pvs > extent || row > extent - pvs ||
            !span(pvs + row, row_bytes, 1, extent, 1)) return 0;
    }
    if (count == 1) return 1;
    struct Visit { uint32_t node, low, high; } stack[128];
    unsigned char seen[127] = {0};
    size_t pending = 1, visited = 0;
    stack[0] = (struct Visit){0, 0, count - 1};
    while (pending) {
        const struct Visit v = stack[--pending];
        if (v.node >= count - 1 || seen[v.node] || v.low >= v.high) return 0;
        seen[v.node] = 1; ++visited;
        const unsigned char *node = bytes + be32(bytes + 0x14) + v.node * 8;
        const uint32_t split = node[5];
        if (node[4] > 2 || split <= v.low || split > v.high) return 0;
        for (unsigned side = 0; side < 2; ++side) {
            const uint32_t child = be16(node + side * 2);
            const uint32_t low = side ? split : v.low;
            const uint32_t high = side ? v.high : split - 1;
            if (child == 0xFFFF) {
                if (low != high) return 0;
            } else {
                if (pending >= 128) return 0;
                stack[pending++] = (struct Visit){child, low, high};
            }
        }
    }
    return visited == count - 1;
}

typedef struct ModelRegion { uint32_t begin, end; int pvs; } ModelRegion;
static int add_region(ModelRegion *regions, size_t *count, uint32_t begin,
                      uint32_t bytes, int pvs) {
    if (!bytes) return 1;
    if (*count >= 645 || bytes > UINT32_MAX - begin) return 0;
    const uint32_t end = begin + bytes;
    for (size_t i = 0; i < *count; ++i)
        if (begin < regions[i].end && regions[i].begin < end &&
            !(pvs && regions[i].pvs)) return 0;
    regions[(*count)++] = (ModelRegion){begin, end, pvs};
    return 1;
}

int mdkr_track_model_validate_regions(const void *model, size_t size) {
    if (!mdkr_track_model_validate_visibility(model, size)) return 0;
    const unsigned char *bytes = model;
    const uint32_t segments = be16(bytes + 0x1A);
    ModelRegion regions[645]; /* Five root regions + five per segment. */
    size_t count = 0;
    if (!add_region(regions, &count, 0, 0x4C, 0) ||
        !add_region(regions, &count, be32(bytes), be16(bytes + 0x18) * 8, 0) ||
        !add_region(regions, &count, be32(bytes + 4), segments * 0x44, 0) ||
        !add_region(regions, &count, be32(bytes + 8), segments * 12, 0) ||
        !add_region(regions, &count, be32(bytes + 0x14), (segments - 1) * 8, 0)) return 0;
    for (uint32_t i = 0; i < segments; ++i) {
        const unsigned char *s = bytes + be32(bytes + 4) + i * 0x44;
        if (!add_region(regions, &count, be32(s), be16(s + 0x1C) * 10, 0) ||
            !add_region(regions, &count, be32(s + 4), be16(s + 0x1E) * 16, 0) ||
            !add_region(regions, &count, be32(s + 0x0C), (be16(s + 0x20) + 1) * 12, 0) ||
            !add_region(regions, &count, be32(s + 0x14), be16(s + 0x1E) * 8, 0)) return 0;
        const uint32_t row = be16(s + 0x28);
        if (row != 0xFFFF && !add_region(regions, &count,
                be32(bytes + 0x10) + row, (segments + 7) / 8, 1)) return 0;
    }
    return 1;
}

int mdkr_track_model_validate_collision(const void *model, size_t size) {
    if (!mdkr_track_model_validate_regions(model, size)) return 0;
    const unsigned char *bytes = model;
    for (uint32_t i = 0; i < be16(bytes + 0x1A); ++i) {
        const unsigned char *s = bytes + be32(bytes + 4) + i * 0x44;
        const unsigned char *batches = bytes + be32(s + 0x0C);
        const uint32_t triangles = be32(s + 4), facets = be32(s + 0x14);
        uint32_t planes = 0;
        for (uint32_t b = 0; b < be16(s + 0x20); ++b) {
            const unsigned char *batch = batches + b * 12;
            for (uint32_t f = be16(batch + 4); f < be16(batch + 16); ++f) {
                if (bytes[triangles + f * 16] & 0x80) continue;
                if (be16(bytes + facets + f * 8) != planes++) return 0;
            }
        }
        for (uint32_t b = 0; b < be16(s + 0x20); ++b) {
            const unsigned char *batch = batches + b * 12;
            if (be32(batch + 8) & (1u << 9)) continue; /* RENDER_NO_COLLISION */
            for (uint32_t f = be16(batch + 4); f < be16(batch + 16); ++f) {
                if (bytes[triangles + f * 16] & 0x80) continue;
                for (unsigned edge = 0; edge < 3; ++edge)
                    if (be16(bytes + facets + f * 8 + 2 + edge * 2) >= planes) return 0;
            }
        }
    }
    return 1;
}

int mdkr_track_model_collision_budget(const void *model, size_t size, size_t *required) {
    if (!required || !mdkr_track_model_validate_collision(model, size) ||
        !mdkr_track_model_validate_draw_batches(model, size)) return 0;
    const unsigned char *bytes = model;
    uint64_t cursor = be32(bytes + 0x48);
    const uint32_t segments = be16(bytes + 0x1A);
    for (uint32_t i = 0; i < segments; ++i) {
        const unsigned char *s = bytes + be32(bytes + 4) + i * 0x44;
        const uint32_t triangles = be16(s + 0x1E), batches = be16(s + 0x20);
        cursor = (cursor + triangles * UINT64_C(2) + 15) & ~UINT64_C(15);
        cursor += triangles * UINT64_C(64);
        cursor = (cursor + batches * UINT64_C(2) + 15) & ~UINT64_C(15);
    }
    if (cursor > SIZE_MAX) return 0;
    *required = (size_t)cursor;
    return 1;
}

int mdkr_track_model_relocate_textures(void *model, size_t size,
    const uint32_t *texture_ids, size_t texture_count,
    uint32_t retail_count, uint32_t final_count) {
    enum { HEADER_SIZE = 0x4C, TEXTURE_STRIDE = 8 };
    uint32_t mapped[MDKR_TRACK_MODEL_TEXTURES_MAX];
    uint32_t resolved[MDKR_TRACK_MODEL_TEXTURES_MAX];
    if (!model || size < HEADER_SIZE ||
        texture_count > MDKR_TRACK_MODEL_TEXTURES_MAX ||
        (texture_count && !texture_ids) || retail_count > final_count ||
        final_count > 0x8000u) return 0;
    /* Snapshot caller mappings before mutation, including an aliased input. */
    for (size_t i = 0; i < texture_count; ++i) {
        mapped[i] = texture_ids[i];
        if (mapped[i] < retail_count || mapped[i] >= final_count) return 0;
    }
    unsigned char *bytes = model;
    const uint32_t extent = be32(bytes + 0x48);
    const uint32_t table = be32(bytes);
    const uint32_t count = (uint32_t)bytes[0x18] << 8 | bytes[0x19];
    if (extent < HEADER_SIZE || extent > size ||
        count > MDKR_TRACK_MODEL_TEXTURES_MAX) return 0;
    if (count == 0) return 1;
    if (table < HEADER_SIZE || table % 4 || table > extent ||
        count > (extent - table) / TEXTURE_STRIDE) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = be32(bytes + table + i * TEXTURE_STRIDE);
        if (id >= MDKR_TRACK_TEXTURE_LOCAL_BASE &&
            id - MDKR_TRACK_TEXTURE_LOCAL_BASE < MDKR_TRACK_MODEL_TEXTURES_MAX) {
            const uint32_t ordinal = id - MDKR_TRACK_TEXTURE_LOCAL_BASE;
            if (ordinal >= texture_count) return 0;
            resolved[i] = mapped[ordinal];
        } else {
            if (id >= retail_count) return 0;
            resolved[i] = id;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char *entry = bytes + table + i * TEXTURE_STRIDE;
        entry[0] = (unsigned char)(resolved[i] >> 24);
        entry[1] = (unsigned char)(resolved[i] >> 16);
        entry[2] = (unsigned char)(resolved[i] >> 8);
        entry[3] = (unsigned char)resolved[i];
    }
    return 1;
}
