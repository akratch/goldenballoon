#include "custom_track_prepared.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifdef MDKR_TRACK_PACKAGE_TEST_ALLOCATOR
void *mdkr_track_test_malloc(size_t size);
void *mdkr_track_test_calloc(size_t count, size_t size);
void mdkr_track_test_free(void *pointer);
#define malloc mdkr_track_test_malloc
#define calloc mdkr_track_test_calloc
#define free mdkr_track_test_free
#endif

struct MdkrPreparedCourse {
    MdkrTrackCourseLayout layout;
    unsigned char digest[32];
    char id[96];
    uint32_t retail_counts[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    size_t count, allocated_payload_bytes;
    MdkrPreparedCourseEntry entries[MDKR_TRACK_ADDS_MAX + 1];
};

void mdkr_track_prepared_destroy(MdkrPreparedCourse *course) {
    if (!course) return;
    for (size_t i = 0; i < course->count; ++i) free((void *)course->entries[i].payload.bytes);
    free(course);
}

const MdkrTrackCourseLayout *mdkr_track_prepared_layout(const MdkrPreparedCourse *course) {
    return course ? &course->layout : NULL;
}

size_t mdkr_track_prepared_entry_count(const MdkrPreparedCourse *course) {
    return course ? course->count : 0;
}

int mdkr_track_prepared_entry(const MdkrPreparedCourse *course, size_t index,
    MdkrPreparedCourseEntry *out) {
    if (!course || !out || index >= course->count) return 0;
    *out = course->entries[index];
    return 1;
}

int mdkr_track_prepared_digest(const MdkrPreparedCourse *course, unsigned char out[32]) {
    if (!course || !out) return 0;
    memcpy(out, course->digest, sizeof course->digest);
    return 1;
}

MdkrPreparedCourse *mdkr_track_prepare_course(const MdkrTrackPackage *package,
    const MdkrTrackPreparation *p) {
    const MdkrTrackManifest *manifest = mdkr_track_package_manifest(package);
    if (!manifest || !p || !p->check_map || !p->owned_payload_limit ||
        !p->engine_arena_bytes || p->engine_arena_bytes > INT32_MAX) return NULL;
    MdkrPreparedCourse *course = calloc(1, sizeof *course);
    if (!course) return NULL;
    memcpy(course->id, manifest->id, sizeof course->id);
    memcpy(course->retail_counts, p->retail, sizeof course->retail_counts);
    if (!mdkr_track_package_course_layout(package, p->retail, p->current, p->limits, &course->layout) ||
        !mdkr_track_package_digest(package, course->digest)) goto fail;
    MdkrTrackTextureBudget texture_budget;
    MdkrTrackObjectMapBudget map_budget;
    if (!mdkr_track_package_texture_budget(package, p->materials.scratch,
            p->materials.scratch_capacity, p->texture_runtime_limit, &texture_budget) ||
        !mdkr_track_package_course_maps_admit(package, p->retail, p->current, p->limits,
            p->read_map, p->read_context, p->scratch, p->scratch_capacity,
            p->object_id_count, p->decoded_map_limit, p->check_map, p->check_context, &map_budget)) goto fail;
    if (course->layout.assets.header.model == -1 &&
        !mdkr_track_package_retained_model_valid(package, p->retail, p->current, p->limits,
            p->retail_2d_count, &p->materials, p->read_model, p->read_context,
            p->scratch, p->scratch_capacity, p->engine_arena_bytes, p->wave_storage_limit)) goto fail;
    const size_t count = manifest->count + (course->layout.synthesize_name != 0);
    for (size_t i = 0; i < count; ++i) {
        const int synthesized = i == manifest->count;
        const MdkrCustomTrackSectionKind section = synthesized ? MDKR_CUSTOM_TRACK_NAMES : manifest->adds[i].section;
        MdkrCustomTrackPayload source = {0};
        if (!synthesized && !mdkr_track_package_payload(package, i, &source)) goto fail;
        size_t capacity = synthesized ? 4 * sizeof manifest->name : source.size;
        if (section == MDKR_CUSTOM_TRACK_MODELS) capacity = p->engine_arena_bytes;
        if (!capacity || capacity > UINT32_MAX ||
            capacity > p->owned_payload_limit - course->allocated_payload_bytes) goto fail;
        void *bytes = malloc(capacity);
        if (!bytes) goto fail;
        MdkrPreparedCourseEntry *entry = &course->entries[course->count++];
        *entry = (MdkrPreparedCourseEntry){section, synthesized ? course->layout.course_id :
            course->layout.assets.addition_ids[i], {bytes, 0}};
        course->allocated_payload_bytes += capacity;
        size_t written = 0;
        if (section == MDKR_CUSTOM_TRACK_HEADERS) {
            if (!mdkr_track_package_prepare_header(package, p->retail, p->current, p->limits,
                p->retail_misc_count, p->retail_2d_count, p->read_misc, p->read_context,
                bytes, capacity, &written)) goto fail;
        } else if (section == MDKR_CUSTOM_TRACK_MODELS) {
            if (!mdkr_track_package_prepare_model(package, p->retail, p->current, p->limits,
                p->retail_2d_count, &p->materials, p->scratch, p->scratch_capacity,
                bytes, capacity, p->engine_arena_bytes, p->wave_storage_limit, &written)) goto fail;
        } else if (section == MDKR_CUSTOM_TRACK_NAMES) {
            if (!mdkr_track_package_prepare_name(package, bytes, capacity, &written)) goto fail;
        } else {
            memcpy(bytes, source.bytes, source.size);
            written = source.size;
        }
        if (!written || written > capacity || written > UINT32_MAX) goto fail;
        entry->payload.size = (uint32_t)written;
    }
    return course;
fail:
    mdkr_track_prepared_destroy(course);
    return NULL;
}

MdkrCustomTrackSession *mdkr_track_prepared_session_create(
    const MdkrPreparedCourse *const *courses, size_t count,
    const MdkrTrackRetailTable retail[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    size_t max_payload_bytes, size_t max_table_words) {
    if (!courses || !count || !retail || count > max_table_words) return NULL;
    uint32_t cursor[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    size_t words = 0, entries = 0, payload_bytes = 0;
    for (unsigned kind = 0; kind < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++kind) {
        if (!retail[kind].words || retail[kind].count < 2 ||
            retail[kind].count - 2 > UINT32_MAX ||
            retail[kind].count > max_table_words - words) return NULL;
        words += retail[kind].count;
        cursor[kind] = (uint32_t)(retail[kind].count - 2);
    }
    for (size_t i = 0; i < count; ++i) {
        const MdkrPreparedCourse *course = courses[i];
        if (!course || course->count > max_table_words - words ||
            course->layout.course_id != cursor[MDKR_CUSTOM_TRACK_HEADERS]) return NULL;
        for (size_t previous = 0; previous < i; ++previous)
            if (!strcmp(course->id, courses[previous]->id) ||
                !memcmp(course->digest, courses[previous]->digest, sizeof course->digest)) return NULL;
        for (unsigned kind = 0; kind < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++kind)
            if (course->retail_counts[kind] != retail[kind].count - 2) return NULL;
        for (size_t j = 0; j < course->count; ++j) {
            const MdkrPreparedCourseEntry *entry = &course->entries[j];
            unsigned kind = (unsigned)entry->section;
            if (kind >= MDKR_CUSTOM_TRACK_SECTION_COUNT || entry->id != cursor[kind] ||
                cursor[kind] == UINT32_MAX || entry->payload.size > max_payload_bytes - payload_bytes)
                return NULL;
            ++cursor[kind];
            payload_bytes += entry->payload.size;
        }
        for (unsigned kind = 0; kind < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++kind)
            if (course->layout.assets.counts[kind] != cursor[kind]) return NULL;
        words += course->count;
        entries += course->count;
    }
    if (entries > SIZE_MAX / sizeof(MdkrCustomTrackPayload)) return NULL;
    MdkrCustomTrackPayload *payloads = malloc(entries * sizeof *payloads);
    if (!payloads) return NULL;
    MdkrCustomTrackSectionInput inputs[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    size_t position = 0;
    for (unsigned kind = 0; kind < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++kind) {
        const size_t begin = position;
        for (size_t i = 0; i < count; ++i)
            for (size_t j = 0; j < courses[i]->count; ++j)
                if ((unsigned)courses[i]->entries[j].section == kind)
                    payloads[position++] = courses[i]->entries[j].payload;
        inputs[kind] = (MdkrCustomTrackSectionInput){(MdkrCustomTrackSectionKind)kind,
            retail[kind].words, retail[kind].count, payloads + begin, position - begin};
    }
    MdkrCustomTrackSession *session = mdkr_custom_track_session_create(inputs,
        MDKR_CUSTOM_TRACK_SECTION_COUNT, max_payload_bytes, max_table_words);
    free(payloads);
    return session;
}
