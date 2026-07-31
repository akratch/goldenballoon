#ifndef _OBJECT_LAYOUT_H_
#define _OBJECT_LAYOUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Checked cursor for the native Object allocation assembled in gSpawnObjectHeap.
 *
 * The N64 builder advanced a byte pointer and assumed every native type was
 * four-byte aligned and every pointer occupied four bytes. Neither assumption
 * holds on LP64. Keeping alignment, overflow, and capacity handling in this
 * small ROM-free module makes sizing and placement one operation: callers
 * cannot reserve one layout and then populate another.
 */
typedef enum MdkrObjectLayoutError {
    MDKR_OBJECT_LAYOUT_OK = 0,
    MDKR_OBJECT_LAYOUT_INVALID_ARGUMENT,
    MDKR_OBJECT_LAYOUT_INVALID_ALIGNMENT,
    MDKR_OBJECT_LAYOUT_ARITHMETIC_OVERFLOW,
    MDKR_OBJECT_LAYOUT_CAPACITY_EXCEEDED,
} MdkrObjectLayoutError;

typedef struct MdkrObjectLayout {
    uint8_t *base;
    size_t capacity;
    size_t cursor;
    MdkrObjectLayoutError error;
} MdkrObjectLayout;

void mdkr_object_layout_init(
    MdkrObjectLayout *layout,
    void *base,
    size_t capacity);
void *mdkr_object_layout_append(
    MdkrObjectLayout *layout,
    size_t size,
    size_t alignment);
void *mdkr_object_layout_append_array(
    MdkrObjectLayout *layout,
    size_t count,
    size_t element_size,
    size_t alignment);
bool mdkr_object_layout_finish(
    MdkrObjectLayout *layout,
    size_t final_alignment,
    size_t *size_out);
MdkrObjectLayoutError mdkr_object_layout_error(
    const MdkrObjectLayout *layout);
const char *mdkr_object_layout_error_string(
    MdkrObjectLayoutError error);

/*
 * Bounds-checked view of one variable-length serialized level-object record.
 * Record bodies remain in their map so deletion/compaction keeps pointer
 * identity; this parser validates the byte-stream boundary before any typed
 * view is formed.
 */
enum {
    MDKR_LEVEL_OBJECT_COMMON_SIZE = 8,
};

typedef enum MdkrLevelObjectRecordError {
    MDKR_LEVEL_OBJECT_RECORD_OK = 0,
    MDKR_LEVEL_OBJECT_RECORD_INVALID_ARGUMENT,
    MDKR_LEVEL_OBJECT_RECORD_TRUNCATED_HEADER,
    MDKR_LEVEL_OBJECT_RECORD_SHORT_STRIDE,
    MDKR_LEVEL_OBJECT_RECORD_ODD_STRIDE,
    MDKR_LEVEL_OBJECT_RECORD_TRUNCATED_BODY,
    MDKR_LEVEL_OBJECT_RECORD_BAD_OBJECT_ID,
} MdkrLevelObjectRecordError;

typedef struct MdkrLevelObjectRecordView {
    const uint8_t *bytes;
    size_t size;
    uint16_t object_id;
} MdkrLevelObjectRecordView;

MdkrLevelObjectRecordError mdkr_level_object_record_parse(
    const void *bytes,
    size_t bytes_remaining,
    size_t object_id_count,
    MdkrLevelObjectRecordView *view_out);
const char *mdkr_level_object_record_error_string(
    MdkrLevelObjectRecordError error);

#endif
