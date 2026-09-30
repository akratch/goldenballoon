#include "custom_track_section.h"
#include <stdlib.h>

#ifdef MDKR_CUSTOM_TRACK_TEST_ALLOCATOR
/* Compile-time fixture seam; absent from production builds. */
void *mdkr_custom_track_test_malloc(size_t size);
void *mdkr_custom_track_test_calloc(size_t count, size_t size);
void mdkr_custom_track_test_free(void *memory);
#define malloc mdkr_custom_track_test_malloc
#define calloc mdkr_custom_track_test_calloc
#define free mdkr_custom_track_test_free
#endif

struct MdkrCustomTrackSection {
    uint32_t *table;
    unsigned char *payload;
    size_t words, retail_words;
    uint32_t retail_end, payload_bytes;
};

struct MdkrCustomTrackSession {
    MdkrCustomTrackSection *sections[MDKR_CUSTOM_TRACK_SECTION_COUNT];
};

size_t mdkr_custom_track_section_retail_count(const MdkrCustomTrackSection *section) {
    return section ? section->retail_words - 2 : 0;
}

void mdkr_custom_track_session_destroy(MdkrCustomTrackSession *session) {
    if (!session) return;
    for (size_t i = 0; i < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++i)
        mdkr_custom_track_section_destroy(session->sections[i]);
    free(session);
}

const MdkrCustomTrackSection *mdkr_custom_track_session_section(
    const MdkrCustomTrackSession *session, MdkrCustomTrackSectionKind kind) {
    if (!session || (unsigned)kind >= MDKR_CUSTOM_TRACK_SECTION_COUNT) return NULL;
    return session->sections[kind];
}

MdkrCustomTrackSession *mdkr_custom_track_session_create(
    const MdkrCustomTrackSectionInput *inputs, size_t count,
    size_t max_payload_bytes, size_t max_table_words) {
    unsigned char seen[MDKR_CUSTOM_TRACK_SECTION_COUNT] = {0};
    if (!inputs || !count || count > MDKR_CUSTOM_TRACK_SECTION_COUNT) return NULL;
    for (size_t i = 0; i < count; ++i) {
        unsigned kind = (unsigned)inputs[i].kind;
        if (kind >= MDKR_CUSTOM_TRACK_SECTION_COUNT || seen[kind]) return NULL;
        seen[kind] = 1;
    }
    MdkrCustomTrackSession *session = calloc(1, sizeof(*session));
    if (!session) return NULL;
    for (size_t i = 0; i < count; ++i) {
        const MdkrCustomTrackSectionInput *input = &inputs[i];
        MdkrCustomTrackSection *section = mdkr_custom_track_section_create(
            input->retail, input->retail_words, input->entries, input->entry_count,
            max_payload_bytes, max_table_words);
        if (!section) {
            mdkr_custom_track_session_destroy(session);
            return NULL;
        }
        session->sections[input->kind] = section;
        max_payload_bytes -= section->payload_bytes;
        max_table_words -= section->words;
    }
    return session;
}

void mdkr_custom_track_section_destroy(MdkrCustomTrackSection *section) {
    if (!section) return;
    free(section->table);
    free(section->payload);
    free(section);
}

MdkrCustomTrackSection *mdkr_custom_track_section_create(
    const uint32_t *retail, size_t words,
    const MdkrCustomTrackPayload *entries, size_t count,
    size_t max_payload_bytes, size_t max_table_words) {
    size_t i, total = 0, position = 0;
    uint32_t *sizes;
    MdkrCustomTrackSection *section;
    if (!retail || words < 2 || (count && !entries) ||
        words > SIZE_MAX / sizeof(uint32_t) ||
        count > SIZE_MAX / sizeof(uint32_t) - words ||
        words + count > max_table_words) return NULL;
    for (i = 0; i < count; ++i) {
        if (!entries[i].bytes || !entries[i].size ||
            entries[i].size > max_payload_bytes - total ||
            entries[i].size > (size_t)INT32_MAX - total) return NULL;
        total += entries[i].size;
    }
    sizes = count ? malloc(count * sizeof(*sizes)) : NULL;
    if (count && !sizes) return NULL;
    for (i = 0; i < count; ++i) sizes[i] = entries[i].size;
    section = calloc(1, sizeof(*section));
    if (!section) { free(sizes); return NULL; }
    section->words = words + count;
    section->retail_words = words;
    section->table = malloc(section->words * sizeof(*section->table));
    if (!section->table || !mdkr_custom_track_extend_table(
            retail, words, sizes, count, section->table, section->words)) {
        free(sizes);
        mdkr_custom_track_section_destroy(section);
        return NULL;
    }
    free(sizes);
    section->payload = total ? malloc(total) : NULL;
    if (total && !section->payload) {
        mdkr_custom_track_section_destroy(section);
        return NULL;
    }
    for (i = 0; i < count; ++i) {
        memcpy(section->payload + position, entries[i].bytes, entries[i].size);
        position += entries[i].size;
    }
    section->retail_end = retail[words - 2];
    section->payload_bytes = (uint32_t)total;
    return section;
}

uint32_t mdkr_custom_track_section_size(const MdkrCustomTrackSection *section) {
    return section ? section->retail_end + section->payload_bytes : 0;
}

int mdkr_custom_track_section_matches_retail(const MdkrCustomTrackSection *section,
                                            const uint32_t *table, size_t words) {
    return section && table && words == section->retail_words && words >= 2 &&
           table[words - 1] == UINT32_MAX &&
           memcmp(section->table, table, (words - 1) * sizeof(*table)) == 0;
}

int mdkr_custom_track_section_load(const MdkrCustomTrackSection *section,
    int32_t offset, int32_t size, void *destination, size_t capacity,
    MdkrCustomTrackRetailRead reader, void *context) {
    MdkrCustomTrackReadPlan plan;
    if (!section || !destination || size <= 0 || (size_t)size > capacity ||
        !mdkr_custom_track_plan_read(section->retail_end, section->payload_bytes,
                                    offset, size, &plan) ||
        (plan.retail_bytes && !reader)) return 0;
    if (plan.retail_bytes && !reader(context, plan.retail_offset, destination,
                                    plan.retail_bytes)) return 0;
    if (plan.custom_bytes)
        memcpy((unsigned char *)destination + plan.retail_bytes,
               section->payload + plan.custom_offset, plan.custom_bytes);
    return 1;
}

const uint32_t *mdkr_custom_track_section_table(const MdkrCustomTrackSection *section,
                                               size_t *words) {
    if (!section || !words) return NULL;
    *words = section->words;
    return section->table;
}

MdkrCustomTrackSpan mdkr_custom_track_section_read(
    const MdkrCustomTrackSection *section, int32_t offset, int32_t size,
    void *destination, size_t capacity) {
    uint32_t relative;
    MdkrCustomTrackSpan span;
    if (!section || !destination || size <= 0 || (size_t)size > capacity)
        return MDKR_CUSTOM_TRACK_SPAN_INVALID;
    span = mdkr_custom_track_classify_span(section->retail_end, section->payload_bytes,
                                          offset, size, &relative);
    if (span == MDKR_CUSTOM_TRACK_SPAN_CUSTOM)
        memcpy(destination, section->payload + relative, (size_t)size);
    return span;
}
