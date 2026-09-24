#ifndef MDKR_CUSTOM_TRACK_TABLE_H
#define MDKR_CUSTOM_TRACK_TABLE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum MdkrCustomTrackSpan {
    MDKR_CUSTOM_TRACK_SPAN_INVALID = 0,
    MDKR_CUSTOM_TRACK_SPAN_RETAIL,
    MDKR_CUSTOM_TRACK_SPAN_CUSTOM
} MdkrCustomTrackSpan;

typedef struct MdkrCustomTrackReadPlan {
    uint32_t retail_offset, retail_bytes;
    uint32_t custom_offset, custom_bytes;
} MdkrCustomTrackReadPlan;

/* Plan a nonempty logical read, including one crossing the append boundary.
 * Execute the retail read first and check its result before copying the custom
 * suffix. A failed ROM read may have modified its destination prefix: callers
 * needing atomic destination updates must stage the complete result privately.
 * Planning itself performs no I/O and leaves output untouched on failure. */
static inline int mdkr_custom_track_plan_read(
    uint32_t retail_end, uint32_t appended_bytes, int32_t offset, int32_t size,
    MdkrCustomTrackReadPlan *output) {
    MdkrCustomTrackReadPlan plan = {0, 0, 0, 0};
    uint32_t start, length, total;
    if (!output || retail_end > INT32_MAX ||
        appended_bytes > (uint32_t)INT32_MAX - retail_end ||
        offset < 0 || size <= 0) return 0;
    start = (uint32_t)offset;
    length = (uint32_t)size;
    total = retail_end + appended_bytes;
    if (start > total || length > total - start) return 0;
    if (start < retail_end) {
        plan.retail_offset = start;
        plan.retail_bytes = retail_end - start;
        if (plan.retail_bytes > length) plan.retail_bytes = length;
    } else {
        plan.custom_offset = start - retail_end;
    }
    plan.custom_bytes = length - plan.retail_bytes;
    *output = plan;
    return 1;
}

/* Classify a nonempty load in a section with one contiguous appended arena.
 * INVALID is not permission to retry ROM DMA. Relative offset is written only
 * for CUSTOM. A caller handling zero-byte no-ops must do so before this check.
 * Individual entry/format validation and arena lifetime belong to the session. */
static inline MdkrCustomTrackSpan mdkr_custom_track_classify_span(
    uint32_t retail_end, uint32_t appended_bytes, int32_t offset, int32_t size,
    uint32_t *relative_offset) {
    uint32_t start, length, relative;
    if (retail_end > INT32_MAX || appended_bytes > (uint32_t)INT32_MAX - retail_end ||
        offset < 0 || size <= 0) return MDKR_CUSTOM_TRACK_SPAN_INVALID;
    start = (uint32_t)offset;
    length = (uint32_t)size;
    if (start < retail_end) {
        return length <= retail_end - start ? MDKR_CUSTOM_TRACK_SPAN_RETAIL
                                           : MDKR_CUSTOM_TRACK_SPAN_INVALID;
    }
    relative = start - retail_end;
    if (!relative_offset || relative >= appended_bytes || length > appended_bytes - relative)
        return MDKR_CUSTOM_TRACK_SPAN_INVALID;
    *relative_offset = relative;
    return MDKR_CUSTOM_TRACK_SPAN_CUSTOM;
}

/* Measure a normalized ROM table within its allocated word count. Retail
 * sections may contain alignment padding after the first sentinel. The final
 * offset may precede padding in the data section too. Never scan unboundedly
 * or substitute the physical section size for the last entry's end offset.
 * Outputs remain untouched on failure. */
static inline int mdkr_custom_track_measure_table(
    const uint32_t *table, size_t available_words, uint32_t data_bytes,
    size_t *table_words) {
    uint32_t previous = 0;
    if (!table || !table_words || available_words < 2 ||
        available_words > SIZE_MAX / sizeof(uint32_t) || data_bytes > INT32_MAX)
        return 0;
    for (size_t i = 0; i < available_words; ++i) {
        uint32_t value = table[i];
        if (value == UINT32_MAX) {
            if (i == 0) return 0;
            *table_words = i + 1;
            return 1;
        }
        if (value > data_bytes || (i && value < previous)) return 0;
        previous = value;
    }
    return 0;
}

/* Host-endian offset-table extension for an immutable custom-track session.
 * retail includes its end offset AND UINT32_MAX sentinel; sizes describes
 * nonempty appended payloads. All buffers must be disjoint. No allocation or
 * ROM access. Failure leaves output untouched. This is not package admission
 * or a loader hook: payload validation and session ownership are separate. */
static inline int mdkr_custom_track_extend_table(
    const uint32_t *retail, size_t retail_words,
    const uint32_t *sizes, size_t count,
    uint32_t *output, size_t capacity) {
    uint32_t end;
    size_t i;
    if (!retail || !output || retail_words < 2 || (count && !sizes) ||
        retail_words > SIZE_MAX / sizeof(uint32_t) ||
        count > SIZE_MAX / sizeof(uint32_t) - retail_words ||
        capacity < retail_words + count || retail[retail_words - 1] != UINT32_MAX)
        return 0;
    for (i = 0; i + 1 < retail_words; ++i) {
        if (retail[i] > INT32_MAX || (i && retail[i] < retail[i - 1])) return 0;
    }
    end = retail[retail_words - 2];
    for (i = 0; i < count; ++i) {
        if (!sizes[i] || sizes[i] > (uint32_t)INT32_MAX - end) return 0;
        end += sizes[i];
    }
    memcpy(output, retail, (retail_words - 1) * sizeof(uint32_t));
    end = retail[retail_words - 2];
    for (i = 0; i < count; ++i) {
        end += sizes[i];
        output[retail_words - 1 + i] = end;
    }
    output[retail_words - 1 + count] = UINT32_MAX;
    return 1;
}
#endif
