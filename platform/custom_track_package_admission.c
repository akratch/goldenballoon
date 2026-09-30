#include "custom_track_package_admission.h"
#include "runtime_contracts.h"
#include "custom_track_model_payload.h"
#include "waves.h"
#include "object_layout.h"
#include "custom_track_decode.h"
#include <limits.h>
#include <string.h>

/* Peak requested pool bytes: persistent data plus concurrently live setup
 * scratch. Texture storage and allocator metadata are separate budgets. */
static int wave_peak_fits(const MdkrCustomTrackPayload *header,
    const MdkrTrackWaveGrid *grid, unsigned players, size_t limit) {
    size_t base;
    if (!mdkr_track_header_wave_render_valid(header->bytes, header->size, players) ||
        !mdkr_track_header_wave_storage(header->bytes, header->size, players, limit, &base)) return 0;
    const unsigned char *bytes = header->bytes;
    const uint64_t subdivisions = (players == 2 ? 4 : bytes[0x56]) *
        (bytes[0x71] ? 2u : 1u);
    const uint64_t side = subdivisions + 1, cells = (uint64_t)grid->columns * grid->rows;
    const uint64_t total = base + cells * 12 +
        (uint64_t)grid->segments * (sizeof(LevelModel_Alternate) + side * side + 4) +
        (subdivisions * grid->columns + 1) * 8 +
        32 * sizeof(WaveGen) + 32 * sizeof(Object *) + side * 4;
    /* Also bounds every individual signed engine allocation request. */
    return total <= INT32_MAX && total <= SIZE_MAX && total <= limit;
}

typedef struct PulseAdmission {
    const MdkrTrackPackage *package;
    const MdkrTrackPackageLayout *layout;
    const MdkrTrackMaterialAdmission *materials;
    uint32_t retail_count;
} PulseAdmission;

static int pulse_texture_valid(void *context, uint32_t id) {
    const PulseAdmission *admission = context;
    const MdkrTrackMaterialAdmission *materials = admission->materials;
    if (!materials) return 0;
    MdkrCustomTrackPayload payload = {0};
    if (id < admission->retail_count) {
        if (!materials->read_retail ||
            !materials->read_retail(materials->context, id, &payload)) return 0;
    } else {
        const MdkrTrackManifest *manifest = mdkr_track_package_manifest(admission->package);
        for (size_t i = 0; i < manifest->count; ++i) {
            if (manifest->adds[i].section == MDKR_CUSTOM_TRACK_TEXTURES &&
                admission->layout->addition_ids[i] == id) {
                if (!mdkr_track_package_payload(admission->package, i, &payload)) return 0;
                break;
            }
        }
    }
    MdkrTrackTextureInfo info;
    return mdkr_track_texture_pulse_payload(payload.bytes, payload.size,
        materials->scratch, materials->scratch_capacity, &info) == MDKR_TRACK_TEXTURE_OK;
}

static int model_header_valid(const void *model, size_t decoded,
    const MdkrCustomTrackPayload *header, uint32_t retail_2d_count,
    uint32_t retail_3d_count, size_t wave_storage_limit, PulseAdmission *pulse) {
    uint32_t waves = 0;
    if (!mdkr_track_model_wave_count(model, decoded, &waves) ||
        !mdkr_track_model_lighting_links_valid(model, decoded, header->bytes, header->size) ||
        !mdkr_track_model_pulse_textures_valid(model, decoded, pulse_texture_valid, pulse)) return 0;
    if (waves) {
        MdkrTrackWaveGrid grid;
        if (!mdkr_track_model_wave_grid(model, decoded, &grid) ||
            !mdkr_track_header_wave_texture_valid(header->bytes, header->size,
                retail_2d_count, retail_3d_count) ||
            !wave_peak_fits(header, &grid, 1, wave_storage_limit) ||
            !wave_peak_fits(header, &grid, 2, wave_storage_limit)) return 0;
    }
    return 1;
}

int mdkr_track_package_retained_model_valid(const MdkrTrackPackage *package,
    const uint32_t *retail, const uint32_t *current, const uint32_t *limits,
    uint32_t retail_2d_count, const MdkrTrackMaterialAdmission *materials,
    MdkrTrackModelRead read_model, void *context,
    void *scratch, size_t scratch_capacity, size_t engine_arena_bytes, size_t wave_storage_limit) {
    MdkrTrackPackageLayout layout;
    if (!read_model || engine_arena_bytes > INT32_MAX ||
        !mdkr_track_package_layout(package, retail, current, limits, &layout) ||
        layout.header.model != -1) return 0;
    const MdkrTrackManifest *manifest = mdkr_track_package_manifest(package);
    for (size_t i = 0; i < manifest->count; ++i) {
        if (manifest->adds[i].section != MDKR_CUSTOM_TRACK_HEADERS) continue;
        MdkrCustomTrackPayload header = {0}, model = {0};
        if (!mdkr_track_package_payload(package, i, &header) ||
            !mdkr_track_header_scalars_valid(header.bytes, header.size)) return 0;
        const unsigned char *bytes = header.bytes;
        const uint32_t id = (uint32_t)bytes[0x34] << 8 | bytes[0x35];
        if (id > INT16_MAX || id >= retail[MDKR_CUSTOM_TRACK_MODELS] ||
            !read_model(context, id, &model) || model.size > engine_arena_bytes) return 0;
        size_t decoded = 0, collision = 0;
        if (!mdkr_custom_track_decode(model.bytes, model.size, scratch, scratch_capacity, &decoded) ||
            decoded > ((engine_arena_bytes - model.size) & ~(size_t)15) ||
            !mdkr_track_model_collision_budget(scratch, decoded, &collision) ||
            collision > engine_arena_bytes ||
            !mdkr_track_model_relocate_textures(scratch, decoded, NULL, 0,
                retail[MDKR_CUSTOM_TRACK_TEXTURES], retail[MDKR_CUSTOM_TRACK_TEXTURES])) return 0;
        PulseAdmission pulse = {package, &layout, materials, retail[MDKR_CUSTOM_TRACK_TEXTURES]};
        return model_header_valid(scratch, decoded, &header, retail_2d_count,
            retail[MDKR_CUSTOM_TRACK_TEXTURES], wave_storage_limit, &pulse);
    }
    return 0;
}

int mdkr_track_package_prepare_model(const MdkrTrackPackage *package,
    const uint32_t *retail, const uint32_t *current, const uint32_t *limits,
    uint32_t retail_2d_count, const MdkrTrackMaterialAdmission *materials,
    void *scratch, size_t scratch_capacity, void *output, size_t capacity,
    size_t engine_arena_bytes, size_t wave_storage_limit, size_t *written) {
    if (!written) return 0;
    *written = 0;
    MdkrTrackPackageLayout layout;
    if (!mdkr_track_package_layout(package, retail, current, limits, &layout)) return 0;
    const MdkrTrackManifest *manifest = mdkr_track_package_manifest(package);
    for (size_t i = 0; i < manifest->count; ++i) {
        if (manifest->adds[i].section != MDKR_CUSTOM_TRACK_MODELS) continue;
        MdkrCustomTrackPayload payload;
        if (!mdkr_track_package_payload(package, i, &payload)) return 0;
        size_t encoded = 0, decoded = 0;
        if (!mdkr_track_model_repack(payload.bytes, payload.size,
            scratch, scratch_capacity, output, capacity, layout.texture_ids,
            layout.texture_count, retail[MDKR_CUSTOM_TRACK_TEXTURES],
            layout.counts[MDKR_CUSTOM_TRACK_TEXTURES], engine_arena_bytes, &encoded, &decoded)) return 0;
        MdkrCustomTrackPayload header = {0};
        for (size_t j = 0; j < manifest->count; ++j)
            if (manifest->adds[j].section == MDKR_CUSTOM_TRACK_HEADERS &&
                !mdkr_track_package_payload(package, j, &header)) return 0;
        PulseAdmission pulse = {package, &layout, materials, retail[MDKR_CUSTOM_TRACK_TEXTURES]};
        if (!model_header_valid(scratch, decoded, &header, retail_2d_count,
                retail[MDKR_CUSTOM_TRACK_TEXTURES], wave_storage_limit, &pulse)) return 0;
        *written = encoded;
        return 1;
    }
    return 0;
}

int mdkr_track_package_prepare_header(const MdkrTrackPackage *package,
    const uint32_t *retail, const uint32_t *current, const uint32_t *limits,
    uint32_t retail_misc_count, uint32_t retail_2d_count,
    MdkrTrackMiscRead read_misc, void *misc_context,
    void *output, size_t capacity, size_t *written) {
    if (!written) return 0;
    *written = 0;
    MdkrTrackPackageLayout layout;
    if (!output || !mdkr_track_package_layout(package, retail, current, limits, &layout)) return 0;
    const MdkrTrackManifest *manifest = mdkr_track_package_manifest(package);
    for (size_t i = 0; i < manifest->count; ++i) {
        if (manifest->adds[i].section != MDKR_CUSTOM_TRACK_HEADERS) continue;
        MdkrCustomTrackPayload payload;
        unsigned char header[MDKR_TRACK_HEADER_BYTES];
        if (!mdkr_track_package_payload(package, i, &payload) ||
            payload.size < sizeof header || payload.size > capacity) return 0;
        if (!mdkr_track_header_scalars_valid(payload.bytes, payload.size) ||
            !mdkr_track_header_misc_valid(payload.bytes, payload.size, retail_misc_count)) return 0;
        if (!mdkr_track_header_sky_texture_valid(payload.bytes, payload.size,
                retail_2d_count, retail[MDKR_CUSTOM_TRACK_TEXTURES])) return 0;
        memcpy(header, payload.bytes, sizeof header);
        if (!mdkr_track_header_relocate(header, sizeof header, &layout.header)) return 0;
        for (unsigned field = 0; field < 8; ++field) {
            const unsigned char *p = header + (field < 7 ? 0x74 + field * 4 : 0xAC);
            const uint32_t id = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                                (uint32_t)p[2] << 8 | p[3];
            if (id == UINT32_MAX) continue;
            MdkrCustomTrackPayload record = {0};
            if (!read_misc || !read_misc(misc_context, id, &record) ||
                !(field < 7 ? mdkr_track_colour_cycle_valid(record.bytes, record.size)
                            : mdkr_track_pulse_cycle_valid(record.bytes, record.size))) return 0;
        }
        /* All refusals precede output writes, including retained references
         * that are invalid despite a perfectly valid addition layout. */
        memcpy(output, payload.bytes, payload.size);
        memcpy(output, header, sizeof header);
        *written = payload.size;
        return 1;
    }
    return 0;
}

int mdkr_track_package_layout(const MdkrTrackPackage *package,
    const uint32_t *retail, const uint32_t *current, const uint32_t *limits,
    MdkrTrackPackageLayout *out) {
    const MdkrTrackManifest *manifest = mdkr_track_package_manifest(package);
    if (!manifest || !retail || !current || !limits || !out) return 0;
    MdkrTrackPackageLayout result = {0};
    result.header.model = result.header.collectables = result.header.structure = -1;
    for (size_t i = 0; i < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++i) {
        if (retail[i] > current[i] || current[i] > limits[i]) return 0;
        result.counts[i] = current[i];
    }
    for (size_t i = 0; i < manifest->count; ++i) {
        const MdkrTrackAddition *addition = &manifest->adds[i];
        const unsigned section = (unsigned)addition->section;
        if (section >= MDKR_CUSTOM_TRACK_SECTION_COUNT ||
            result.counts[section] >= limits[section]) return 0;
        const uint32_t id = result.counts[section]++;
        result.addition_ids[i] = id;
        if (section == MDKR_CUSTOM_TRACK_MODELS) {
            if (id > INT16_MAX) return 0;
            result.header.model = (int32_t)id;
        } else if (section == MDKR_CUSTOM_TRACK_OBJECT_MAPS) {
            if (id > INT16_MAX) return 0;
            if (addition->slot == MDKR_TRACK_MAP_STRUCTURE)
                result.header.structure = (int32_t)id;
            else if (addition->slot == MDKR_TRACK_MAP_COLLECTABLES)
                result.header.collectables = (int32_t)id;
            else return 0;
        } else if (section == MDKR_CUSTOM_TRACK_TEXTURES) {
            if (id >= 0x8000u || result.texture_count == MDKR_TRACK_MODEL_TEXTURES_MAX) return 0;
            result.texture_ids[result.texture_count++] = id;
        }
    }
    /* The relocation helpers also consume these full inventory counts. */
    if (result.counts[MDKR_CUSTOM_TRACK_MODELS] > 0x8000u ||
        result.counts[MDKR_CUSTOM_TRACK_OBJECT_MAPS] > 0x8000u ||
        result.counts[MDKR_CUSTOM_TRACK_TEXTURES] > 0x8000u) return 0;
    result.header.model_count = result.counts[MDKR_CUSTOM_TRACK_MODELS];
    result.header.object_map_count = result.counts[MDKR_CUSTOM_TRACK_OBJECT_MAPS];
    result.header.retail_model_count = retail[MDKR_CUSTOM_TRACK_MODELS];
    result.header.retail_object_map_count = retail[MDKR_CUSTOM_TRACK_OBJECT_MAPS];
    *out = result;
    return 1;
}

int mdkr_track_package_course_layout(const MdkrTrackPackage *package,
    const uint32_t *retail, const uint32_t *current, const uint32_t *limits,
    MdkrTrackCourseLayout *out) {
    MdkrTrackCourseLayout result = {0};
    if (!out || !mdkr_track_package_layout(package, retail, current, limits, &result.assets)) return 0;
    if (retail[MDKR_CUSTOM_TRACK_HEADERS] != retail[MDKR_CUSTOM_TRACK_NAMES] ||
        current[MDKR_CUSTOM_TRACK_HEADERS] != current[MDKR_CUSTOM_TRACK_NAMES] ||
        result.assets.counts[MDKR_CUSTOM_TRACK_HEADERS] == current[MDKR_CUSTOM_TRACK_HEADERS]) return 0;
    result.course_id = current[MDKR_CUSTOM_TRACK_HEADERS];
    result.synthesize_name = result.assets.counts[MDKR_CUSTOM_TRACK_NAMES] == current[MDKR_CUSTOM_TRACK_NAMES];
    if (result.synthesize_name) {
        if (result.assets.counts[MDKR_CUSTOM_TRACK_NAMES] >= limits[MDKR_CUSTOM_TRACK_NAMES]) return 0;
        ++result.assets.counts[MDKR_CUSTOM_TRACK_NAMES];
    }
    if (result.assets.counts[MDKR_CUSTOM_TRACK_NAMES] != result.assets.counts[MDKR_CUSTOM_TRACK_HEADERS]) return 0;
    *out = result;
    return 1;
}

int mdkr_track_package_names_valid(const MdkrTrackPackage *package) {
    const MdkrTrackManifest *manifest = mdkr_track_package_manifest(package);
    if (!manifest) return 0;
    for (size_t i = 0; i < manifest->count; ++i) {
        if (manifest->adds[i].section != MDKR_CUSTOM_TRACK_NAMES) continue;
        MdkrCustomTrackPayload payload;
        if (!mdkr_track_package_payload(package, i, &payload)) return 0;
        for (unsigned language = 0; language < 4; ++language)
            if (!mdkr_level_name_bounded(payload.bytes, payload.size, language)) return 0;
    }
    return 1;
}

MdkrTrackNameSource mdkr_track_package_prepare_name(const MdkrTrackPackage *package,
    void *output, size_t capacity, size_t *written) {
    if (!written) return MDKR_TRACK_NAME_REFUSED;
    *written = 0;
    const MdkrTrackManifest *manifest = mdkr_track_package_manifest(package);
    if (!manifest || !output) return MDKR_TRACK_NAME_REFUSED;
    for (size_t i = 0; i < manifest->count; ++i) {
        if (manifest->adds[i].section != MDKR_CUSTOM_TRACK_NAMES) continue;
        MdkrCustomTrackPayload payload;
        if (!mdkr_track_package_payload(package, i, &payload) ||
            payload.size > capacity || !mdkr_track_package_names_valid(package))
            return MDKR_TRACK_NAME_REFUSED;
        memcpy(output, payload.bytes, payload.size);
        *written = payload.size;
        return MDKR_TRACK_NAME_PAYLOAD;
    }
    const char *name = manifest->name;
    MdkrTrackNameSource source = MDKR_TRACK_NAME_METADATA;
    int printable = name[0] != 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 0x20 || *p > 0x7E) printable = 0;
    if (!printable) { name = manifest->id; source = MDKR_TRACK_NAME_IDENTIFIER; }
    const size_t length = strlen(name) + 1;
    if (length > capacity / 4) return MDKR_TRACK_NAME_REFUSED;
    for (unsigned language = 0; language < 4; ++language)
        memcpy((unsigned char *)output + language * length, name, length);
    *written = 4 * length;
    return source;
}

static int object_map_admit(const MdkrCustomTrackPayload *payload,
    void *scratch, size_t scratch_capacity, size_t object_id_count,
    size_t decoded_limit, MdkrTrackObjectMapCheck check, void *context,
    int require_behavior, MdkrTrackObjectMapBudget *result) {
    size_t decoded = 0, records = 0;
    if (result->decoded_bytes > decoded_limit) return 0;
    const size_t remaining = decoded_limit - result->decoded_bytes;
    const size_t capacity = scratch_capacity < remaining ? scratch_capacity : remaining;
    if (!mdkr_custom_track_decode(payload->bytes, payload->size, scratch, capacity, &decoded) ||
        decoded > remaining ||
        !mdkr_level_object_map_validate(scratch, decoded, object_id_count, &records) ||
        records > SIZE_MAX - result->records) return 0;
    if (require_behavior) {
        const unsigned char *bytes = scratch;
        const uint32_t entry_bytes = (uint32_t)bytes[0] << 24 |
            (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
        if (!check || !check(context, bytes + 16, entry_bytes)) return 0;
    }
    ++result->entries;
    result->records += records;
    result->decoded_bytes += decoded;
    return 1;
}

int mdkr_track_course_object_maps_admit(const void *header, size_t header_size,
    uint32_t map_count, MdkrTrackObjectMapRead read_map, void *read_context,
    void *scratch, size_t scratch_capacity, size_t object_id_count,
    size_t decoded_limit, MdkrTrackObjectMapCheck check, void *check_context,
    MdkrTrackObjectMapBudget *out) {
    if (!header || header_size < MDKR_TRACK_HEADER_BYTES || !read_map || !check || !out)
        return 0;
    const unsigned char *bytes = header;
    const uint32_t ids[] = {(uint32_t)bytes[0xBA] << 8 | bytes[0xBB],
        (uint32_t)bytes[0x36] << 8 | bytes[0x37]};
    for (unsigned i = 0; i < 2; ++i)
        if (ids[i] > INT16_MAX || ids[i] >= map_count) return 0;
    MdkrTrackObjectMapBudget result = {0};
    for (unsigned i = 0; i < 2; ++i) {
        MdkrCustomTrackPayload payload = {0};
        if (!read_map(read_context, ids[i], &payload) ||
            !object_map_admit(&payload, scratch, scratch_capacity, object_id_count,
                decoded_limit, check, check_context, 1, &result)) return 0;
    }
    *out = result;
    return 1;
}

typedef struct CourseMapAdmission {
    const MdkrTrackPackage *package;
    const MdkrTrackPackageLayout *layout;
    uint32_t retail_count;
    MdkrTrackObjectMapRead read_retail;
    void *context;
} CourseMapAdmission;

static int course_map_payload(void *context, uint32_t id, MdkrCustomTrackPayload *out) {
    const CourseMapAdmission *admission = context;
    if (id < admission->retail_count)
        return admission->read_retail && admission->read_retail(admission->context, id, out);
    const MdkrTrackManifest *manifest = mdkr_track_package_manifest(admission->package);
    for (size_t i = 0; i < manifest->count; ++i)
        if (manifest->adds[i].section == MDKR_CUSTOM_TRACK_OBJECT_MAPS &&
            admission->layout->addition_ids[i] == id)
            return mdkr_track_package_payload(admission->package, i, out);
    return 0;
}

int mdkr_track_package_course_maps_admit(const MdkrTrackPackage *package,
    const uint32_t *retail, const uint32_t *current, const uint32_t *limits,
    MdkrTrackObjectMapRead read_retail, void *read_context,
    void *scratch, size_t scratch_capacity, size_t object_id_count,
    size_t decoded_limit, MdkrTrackObjectMapCheck check, void *check_context,
    MdkrTrackObjectMapBudget *out) {
    MdkrTrackCourseLayout layout;
    if (!out || !check || !mdkr_track_package_course_layout(package, retail, current, limits, &layout))
        return 0;
    const MdkrTrackManifest *manifest = mdkr_track_package_manifest(package);
    for (size_t i = 0; i < manifest->count; ++i) {
        if (manifest->adds[i].section != MDKR_CUSTOM_TRACK_HEADERS) continue;
        MdkrCustomTrackPayload payload;
        unsigned char header[MDKR_TRACK_HEADER_BYTES];
        if (!mdkr_track_package_payload(package, i, &payload) || payload.size < sizeof header) return 0;
        memcpy(header, payload.bytes, sizeof header);
        if (!mdkr_track_header_relocate(header, sizeof header, &layout.assets.header)) return 0;
        CourseMapAdmission admission = {package, &layout.assets,
            retail[MDKR_CUSTOM_TRACK_OBJECT_MAPS], read_retail, read_context};
        return mdkr_track_course_object_maps_admit(header, sizeof header,
            layout.assets.counts[MDKR_CUSTOM_TRACK_OBJECT_MAPS], course_map_payload, &admission,
            scratch, scratch_capacity, object_id_count, decoded_limit, check, check_context, out);
    }
    return 0;
}

static int object_maps_check(const MdkrTrackPackage *package,
    void *scratch, size_t scratch_capacity, size_t object_id_count,
    size_t decoded_limit, MdkrTrackObjectMapCheck check, void *context,
    int require_behavior, MdkrTrackObjectMapBudget *out) {
    const MdkrTrackManifest *manifest = mdkr_track_package_manifest(package);
    if (!manifest || !out) return 0;
    MdkrTrackObjectMapBudget result = {0};
    for (size_t i = 0; i < manifest->count; ++i) {
        if (manifest->adds[i].section != MDKR_CUSTOM_TRACK_OBJECT_MAPS) continue;
        MdkrCustomTrackPayload payload;
        if (!mdkr_track_package_payload(package, i, &payload) ||
            !object_map_admit(&payload, scratch, scratch_capacity, object_id_count,
                decoded_limit, check, context, require_behavior, &result)) return 0;
    }
    *out = result;
    return 1;
}

int mdkr_track_package_object_maps_valid(const MdkrTrackPackage *package,
    void *scratch, size_t scratch_capacity, size_t object_id_count,
    size_t decoded_limit, MdkrTrackObjectMapBudget *out) {
    return object_maps_check(package, scratch, scratch_capacity, object_id_count,
        decoded_limit, NULL, NULL, 0, out);
}

int mdkr_track_package_object_maps_admit(const MdkrTrackPackage *package,
    void *scratch, size_t scratch_capacity, size_t object_id_count,
    size_t decoded_limit, MdkrTrackObjectMapCheck check, void *context,
    MdkrTrackObjectMapBudget *out) {
    return object_maps_check(package, scratch, scratch_capacity, object_id_count,
        decoded_limit, check, context, 1, out);
}

int mdkr_track_package_texture_budget(const MdkrTrackPackage *package,
    void *scratch, size_t scratch_capacity, size_t runtime_limit,
    MdkrTrackTextureBudget *out) {
    const MdkrTrackManifest *manifest = mdkr_track_package_manifest(package);
    if (!manifest || !out) return 0;
    MdkrTrackTextureBudget result = {0, 0};
    for (size_t i = 0; i < manifest->count; ++i) {
        if (manifest->adds[i].section != MDKR_CUSTOM_TRACK_TEXTURES) continue;
        MdkrCustomTrackPayload payload;
        MdkrTrackTextureInfo info;
        if (!mdkr_track_package_payload(package, i, &payload) ||
            mdkr_track_texture_authored_payload(payload.bytes, payload.size,
                scratch, scratch_capacity, &info) != MDKR_TRACK_TEXTURE_OK ||
            info.runtime_allocation_bytes > runtime_limit - result.runtime_bytes)
            return 0;
        result.runtime_bytes += info.runtime_allocation_bytes;
        ++result.entries;
    }
    *out = result;
    return 1;
}
