#include "custom_track_table.h"
#include "custom_track_section.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef MDKR_CUSTOM_TRACK_TEST_ALLOCATOR
static int allocation_budget = -1;
static size_t live_allocations;
void *mdkr_custom_track_test_malloc(size_t size) {
    if (allocation_budget == 0) return NULL;
    if (allocation_budget > 0) --allocation_budget;
    void *memory = malloc(size);
    if (memory) ++live_allocations;
    return memory;
}
void *mdkr_custom_track_test_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    void *memory = mdkr_custom_track_test_malloc(count * size);
    if (memory) memset(memory, 0, count * size);
    return memory;
}
void mdkr_custom_track_test_free(void *memory) {
    if (memory) --live_allocations;
    free(memory);
}
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "table:%d: %s\n", __LINE__, #x); return 1; } } while (0)

typedef struct RetailFixture { unsigned calls; int fail; } RetailFixture;
static int read_retail(void *context, uint32_t offset, void *destination, uint32_t size) {
    RetailFixture *fixture = context;
    ++fixture->calls;
    if (offset > 40 || size > 40 - offset) abort();
    /* A failing reader deliberately dirties its prefix to exercise the stated
     * partial-write contract; no custom suffix may be copied after failure. */
    for (uint32_t i = 0; i < size; ++i)
        ((unsigned char *)destination)[i] = (unsigned char)(offset + i);
    return !fixture->fail;
}

int main(void) {
    /* Exhaust small address spaces, checking every covered byte against an
     * independent source-selection oracle (including an empty retail prefix). */
    for (uint32_t retail_bytes = 0; retail_bytes <= 8; ++retail_bytes) {
        for (uint32_t custom_bytes = 0; custom_bytes <= 8; ++custom_bytes) {
            for (int32_t offset = -1; offset <= 18; ++offset) {
                for (int32_t length = -1; length <= 18; ++length) {
                    MdkrCustomTrackReadPlan plan = {99, 98, 97, 96};
                    const MdkrCustomTrackReadPlan before_plan = plan;
                    int valid = offset >= 0 && length > 0 &&
                        offset + length <= (int32_t)(retail_bytes + custom_bytes);
                    CHECK(mdkr_custom_track_plan_read(retail_bytes, custom_bytes,
                                                      offset, length, &plan) == valid);
                    if (!valid) {
                        CHECK(memcmp(&plan, &before_plan, sizeof(plan)) == 0);
                        continue;
                    }
                    CHECK(plan.retail_bytes + plan.custom_bytes == (uint32_t)length);
                    for (uint32_t i = 0; i < (uint32_t)length; ++i) {
                        uint32_t logical = (uint32_t)offset + i;
                        if (i < plan.retail_bytes) {
                            CHECK(logical < retail_bytes);
                            CHECK(plan.retail_offset + i == logical);
                        } else {
                            CHECK(logical >= retail_bytes);
                            CHECK(plan.custom_offset + i - plan.retail_bytes == logical - retail_bytes);
                        }
                    }
                }
            }
        }
    }
    MdkrCustomTrackReadPlan limit_plan;
    CHECK(mdkr_custom_track_plan_read(INT32_MAX - 1, 1, INT32_MAX - 2, 2, &limit_plan));
    CHECK(limit_plan.retail_bytes == 1 && limit_plan.custom_bytes == 1);
    CHECK(!mdkr_custom_track_plan_read(INT32_MAX, 1, 0, 1, &limit_plan));
    CHECK(!mdkr_custom_track_plan_read(0, 1, 0, 1, NULL));
    puts("custom track split read plan: PASS");
    const uint32_t padded[] = {0, 12, 40, UINT32_MAX, 0xABCD1234, 0};
    size_t measured = 999;
    CHECK(mdkr_custom_track_measure_table(padded, 6, 48, &measured));
    CHECK(measured == 4);
    measured = 999;
    CHECK(!mdkr_custom_track_measure_table(padded, 3, 48, &measured));
    CHECK(!mdkr_custom_track_measure_table(padded, 6, 39, &measured));
    CHECK(!mdkr_custom_track_measure_table(padded + 3, 3, 48, &measured));
    const uint32_t unordered[] = {0, 40, 12, UINT32_MAX};
    CHECK(!mdkr_custom_track_measure_table(unordered, 4, 48, &measured));
    CHECK(!mdkr_custom_track_measure_table(NULL, 6, 48, &measured));
    CHECK(!mdkr_custom_track_measure_table(padded, SIZE_MAX, 48, &measured));
    CHECK(measured == 999);
    const uint32_t retail[] = {0, 12, 40, UINT32_MAX};
    const uint32_t sizes[] = {8, 16};
    const uint32_t expected[] = {0, 12, 40, 48, 64, UINT32_MAX};
    uint32_t out[6], before[6];
    CHECK(mdkr_custom_track_extend_table(retail, 4, sizes, 2, out, 6));
    CHECK(memcmp(out, expected, sizeof(out)) == 0);
    CHECK(out[2] - out[1] == 28); /* Last retail payload remains intact. */
    CHECK(mdkr_custom_track_extend_table(retail, 4, NULL, 0, out, 6));
    CHECK(memcmp(out, retail, sizeof(retail)) == 0);
    memset(out, 0xA5, sizeof(out));
    memcpy(before, out, sizeof(out));
    CHECK(!mdkr_custom_track_extend_table(retail, 4, sizes, 2, out, 5));
    CHECK(memcmp(out, before, sizeof(out)) == 0);
    const uint32_t overflow[] = {8, INT32_MAX};
    CHECK(!mdkr_custom_track_extend_table(retail, 4, overflow, 2, out, 6));
    CHECK(memcmp(out, before, sizeof(out)) == 0);
    const uint32_t backwards[] = {0, 40, 12, UINT32_MAX};
    CHECK(!mdkr_custom_track_extend_table(backwards, 4, sizes, 2, out, 6));
    const uint32_t zero[] = {0};
    CHECK(!mdkr_custom_track_extend_table(retail, 4, zero, 1, out, 6));
    CHECK(!mdkr_custom_track_extend_table(retail, 3, sizes, 2, out, 6));
    CHECK(!mdkr_custom_track_extend_table(retail, 4, sizes, SIZE_MAX, out, 6));
    CHECK(memcmp(out, before, sizeof(out)) == 0);
    puts("custom track table: PASS");
    uint32_t relative = 999;
    CHECK(mdkr_custom_track_classify_span(40, 24, 12, 28, &relative) == MDKR_CUSTOM_TRACK_SPAN_RETAIL);
    CHECK(relative == 999);
    CHECK(mdkr_custom_track_classify_span(40, 24, 39, 2, &relative) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    CHECK(relative == 999);
    CHECK(mdkr_custom_track_classify_span(40, 24, 40, 24, &relative) == MDKR_CUSTOM_TRACK_SPAN_CUSTOM);
    CHECK(relative == 0);
    CHECK(mdkr_custom_track_classify_span(40, 24, 63, 1, &relative) == MDKR_CUSTOM_TRACK_SPAN_CUSTOM);
    CHECK(relative == 23);
    CHECK(mdkr_custom_track_classify_span(40, 24, 63, 2, &relative) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    CHECK(mdkr_custom_track_classify_span(40, 24, 64, 1, &relative) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    CHECK(mdkr_custom_track_classify_span(40, 0, 40, 1, &relative) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    CHECK(mdkr_custom_track_classify_span(40, 24, -1, 1, &relative) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    CHECK(mdkr_custom_track_classify_span(40, 24, 40, -1, &relative) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    CHECK(mdkr_custom_track_classify_span(40, 24, 40, 0, &relative) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    CHECK(mdkr_custom_track_classify_span(INT32_MAX, 1, 0, 1, &relative) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    CHECK(mdkr_custom_track_classify_span(40, 24, 40, 1, NULL) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    CHECK(relative == 23);
    puts("custom track span: PASS");
    unsigned char first[] = {1, 2, 3}, second[] = {4, 5};
    MdkrCustomTrackPayload entries[] = {{first, sizeof(first)}, {second, sizeof(second)}};
    CHECK(!mdkr_custom_track_section_create(retail, 4, entries, 2, 4, 6));
    CHECK(!mdkr_custom_track_section_create(retail, 4, entries, 2, 5, 5));
    MdkrCustomTrackSection *section = mdkr_custom_track_section_create(retail, 4, entries, 2, 5, 6);
    CHECK(section != NULL);
    CHECK(mdkr_custom_track_section_matches_retail(section, retail, 4));
    const uint32_t wrong_prefix[] = {1, 12, 40, UINT32_MAX};
    const uint32_t wrong_count[] = {0, 12, 40, 43, UINT32_MAX};
    CHECK(!mdkr_custom_track_section_matches_retail(section, wrong_prefix, 4));
    CHECK(!mdkr_custom_track_section_matches_retail(section, wrong_count, 5));
    CHECK(!mdkr_custom_track_section_matches_retail(section, retail, 3));
    CHECK(!mdkr_custom_track_section_matches_retail(section, NULL, 4));
    CHECK(!mdkr_custom_track_section_matches_retail(NULL, retail, 4));
    CHECK(mdkr_custom_track_section_size(section) == 45);
    CHECK(mdkr_custom_track_section_size(NULL) == 0);
    memset(first, 99, sizeof(first));
    memset(second, 99, sizeof(second));
    unsigned char bytes[5] = {0};
    const unsigned char expected_bytes[] = {1, 2, 3, 4, 5};
    CHECK(mdkr_custom_track_section_read(section, 40, 5, bytes, sizeof(bytes)) == MDKR_CUSTOM_TRACK_SPAN_CUSTOM);
    CHECK(memcmp(bytes, expected_bytes, sizeof(bytes)) == 0);
    RetailFixture fixture = {0, 0};
    unsigned char joined[7];
    const unsigned char expected_joined[] = {38, 39, 1, 2, 3, 4, 5};
    CHECK(mdkr_custom_track_section_load(section, 38, 7, joined, sizeof(joined), read_retail, &fixture));
    CHECK(fixture.calls == 1 && memcmp(joined, expected_joined, sizeof(joined)) == 0);
    memset(joined, 0xA5, sizeof(joined));
    fixture.fail = 1;
    CHECK(!mdkr_custom_track_section_load(section, 38, 7, joined, sizeof(joined), read_retail, &fixture));
    CHECK(fixture.calls == 2 && joined[0] == 38 && joined[1] == 39);
    for (size_t i = 2; i < sizeof(joined); ++i) CHECK(joined[i] == 0xA5);
    CHECK(!mdkr_custom_track_section_load(section, 38, 7, joined, 6, read_retail, &fixture));
    CHECK(!mdkr_custom_track_section_load(section, 39, 7, joined, 7, read_retail, &fixture));
    CHECK(!mdkr_custom_track_section_load(section, 38, 7, joined, 7, NULL, NULL));
    CHECK(fixture.calls == 2);
    for (size_t i = 2; i < sizeof(joined); ++i) CHECK(joined[i] == 0xA5);
    CHECK(mdkr_custom_track_section_load(section, 40, 5, joined, 7, NULL, NULL));
    CHECK(memcmp(joined, expected_bytes, 5) == 0);
    fixture.fail = 0;
    CHECK(mdkr_custom_track_section_load(section, 38, 2, joined, 7, read_retail, &fixture));
    CHECK(fixture.calls == 3 && joined[0] == 38 && joined[1] == 39);
    CHECK(mdkr_custom_track_section_read(section, 39, 2, bytes, sizeof(bytes)) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    CHECK(mdkr_custom_track_section_read(section, 0, 5, bytes, sizeof(bytes)) == MDKR_CUSTOM_TRACK_SPAN_RETAIL);
    CHECK(mdkr_custom_track_section_read(section, 40, 5, bytes, 4) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    CHECK(memcmp(bytes, expected_bytes, sizeof(bytes)) == 0);
    size_t words = 0;
    const uint32_t *table = mdkr_custom_track_section_table(section, &words);
    CHECK(table && words == 6 && table[2] == 40 && table[3] == 43 && table[4] == 45 && table[5] == UINT32_MAX);
#ifdef MDKR_CUSTOM_TRACK_TEST_ALLOCATOR
    const size_t retained = live_allocations;
    CHECK(retained == 3); /* Section, immutable table, payload arena. */
    for (int budget = 0; budget <= 4; ++budget) {
        allocation_budget = budget;
        MdkrCustomTrackSection *candidate = mdkr_custom_track_section_create(
            retail, 4, entries, 2, 5, 6);
        allocation_budget = -1;
        CHECK((candidate != NULL) == (budget == 4));
        mdkr_custom_track_section_destroy(candidate);
        CHECK(live_allocations == retained);
        CHECK(mdkr_custom_track_section_read(section, 40, 5, bytes, sizeof(bytes)) == MDKR_CUSTOM_TRACK_SPAN_CUSTOM);
        CHECK(memcmp(bytes, expected_bytes, sizeof(bytes)) == 0);
    }
#endif
    mdkr_custom_track_section_destroy(section);
    mdkr_custom_track_section_destroy(NULL);
#ifdef MDKR_CUSTOM_TRACK_TEST_ALLOCATOR
    CHECK(live_allocations == 0);
#endif
    puts("custom track owned section: PASS");
    MdkrCustomTrackSectionInput inputs[] = {
        {MDKR_CUSTOM_TRACK_HEADERS, retail, 4, entries, 2},
        {MDKR_CUSTOM_TRACK_MODELS, retail, 4, entries, 2}
    };
    CHECK(!mdkr_custom_track_session_create(NULL, 2, 10, 12));
    CHECK(!mdkr_custom_track_session_create(inputs, 0, 10, 12));
    CHECK(!mdkr_custom_track_session_create(inputs, MDKR_CUSTOM_TRACK_SECTION_COUNT + 1, 10, 12));
    CHECK(!mdkr_custom_track_session_create(inputs, 2, 9, 12));
    CHECK(!mdkr_custom_track_session_create(inputs, 2, 10, 11));
    inputs[1].kind = inputs[0].kind;
    CHECK(!mdkr_custom_track_session_create(inputs, 2, 10, 12));
    inputs[1].kind = (MdkrCustomTrackSectionKind)-1;
    CHECK(!mdkr_custom_track_session_create(inputs, 2, 10, 12));
    inputs[1].kind = MDKR_CUSTOM_TRACK_SECTION_COUNT;
    CHECK(!mdkr_custom_track_session_create(inputs, 2, 10, 12));
    inputs[1].kind = MDKR_CUSTOM_TRACK_MODELS;
    inputs[1].retail = backwards;
    CHECK(!mdkr_custom_track_session_create(inputs, 2, 10, 12));
    inputs[1].retail = retail;
#ifdef MDKR_CUSTOM_TRACK_TEST_ALLOCATOR
    CHECK(live_allocations == 0);
#endif
    MdkrCustomTrackSession *session = mdkr_custom_track_session_create(inputs, 2, 10, 12);
    CHECK(session);
    const MdkrCustomTrackSection *headers = mdkr_custom_track_session_section(session, MDKR_CUSTOM_TRACK_HEADERS);
    const MdkrCustomTrackSection *models = mdkr_custom_track_session_section(session, MDKR_CUSTOM_TRACK_MODELS);
    CHECK(headers && models && headers != models);
    CHECK(!mdkr_custom_track_session_section(session, MDKR_CUSTOM_TRACK_TEXTURES));
    CHECK(!mdkr_custom_track_session_section(session, (MdkrCustomTrackSectionKind)-1));
    CHECK(!mdkr_custom_track_session_section(session, MDKR_CUSTOM_TRACK_SECTION_COUNT));
    CHECK(!mdkr_custom_track_session_section(NULL, MDKR_CUSTOM_TRACK_HEADERS));
    memset(first, 42, sizeof(first));
    memset(second, 42, sizeof(second));
    const unsigned char session_bytes[] = {99, 99, 99, 99, 99};
    CHECK(mdkr_custom_track_section_read(models, 40, 5, bytes, sizeof(bytes)) == MDKR_CUSTOM_TRACK_SPAN_CUSTOM);
    CHECK(memcmp(bytes, session_bytes, sizeof(bytes)) == 0);
#ifdef MDKR_CUSTOM_TRACK_TEST_ALLOCATOR
    CHECK(live_allocations == 7); /* Session plus two independently owned sections. */
    for (int budget = 0; budget <= 9; ++budget) {
        allocation_budget = budget;
        MdkrCustomTrackSession *candidate = mdkr_custom_track_session_create(inputs, 2, 10, 12);
        allocation_budget = -1;
        CHECK((candidate != NULL) == (budget == 9));
        mdkr_custom_track_session_destroy(candidate);
        CHECK(live_allocations == 7);
        CHECK(mdkr_custom_track_section_read(headers, 40, 5, bytes, sizeof(bytes)) == MDKR_CUSTOM_TRACK_SPAN_CUSTOM);
        CHECK(memcmp(bytes, session_bytes, sizeof(bytes)) == 0);
    }
#endif
    mdkr_custom_track_session_destroy(session);
    mdkr_custom_track_session_destroy(NULL);
#ifdef MDKR_CUSTOM_TRACK_TEST_ALLOCATOR
    CHECK(live_allocations == 0);
#endif
    puts("custom track atomic session: PASS");
    const uint32_t largest_retail[] = {0, INT32_MAX - 5, UINT32_MAX};
    section = mdkr_custom_track_section_create(largest_retail, 3, entries, 2, 5, 5);
    CHECK(section && mdkr_custom_track_section_size(section) == INT32_MAX);
    CHECK(mdkr_custom_track_section_read(section, INT32_MAX - 1, 1, bytes, sizeof(bytes)) == MDKR_CUSTOM_TRACK_SPAN_CUSTOM);
    CHECK(bytes[0] == 42);
    CHECK(mdkr_custom_track_section_read(section, INT32_MAX, 1, bytes, sizeof(bytes)) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    mdkr_custom_track_section_destroy(section);
    section = mdkr_custom_track_section_create(retail, 4, NULL, 0, 0, 4);
    CHECK(section && mdkr_custom_track_section_size(section) == 40);
    CHECK(mdkr_custom_track_section_read(section, 39, 1, bytes, sizeof(bytes)) == MDKR_CUSTOM_TRACK_SPAN_RETAIL);
    CHECK(mdkr_custom_track_section_read(section, 40, 1, bytes, sizeof(bytes)) == MDKR_CUSTOM_TRACK_SPAN_INVALID);
    mdkr_custom_track_section_destroy(section);
#ifdef MDKR_CUSTOM_TRACK_TEST_ALLOCATOR
    CHECK(live_allocations == 0);
#endif
    puts("custom track logical size: PASS");
    return 0;
}
