#include "object_layout.h"

#include <limits.h>

static bool mdkr_is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static void mdkr_object_layout_fail(
    MdkrObjectLayout *layout,
    MdkrObjectLayoutError error) {
    if (layout != NULL && layout->error == MDKR_OBJECT_LAYOUT_OK) {
        layout->error = error;
    }
}

void mdkr_object_layout_init(
    MdkrObjectLayout *layout,
    void *base,
    size_t capacity) {
    uintptr_t base_address;

    if (layout == NULL) {
        return;
    }

    layout->base = (uint8_t *) base;
    layout->capacity = capacity;
    layout->cursor = 0;
    layout->error = MDKR_OBJECT_LAYOUT_OK;

    if (base == NULL || capacity == 0) {
        layout->error = MDKR_OBJECT_LAYOUT_INVALID_ARGUMENT;
        return;
    }

    base_address = (uintptr_t) base;
    if (capacity - 1 > UINTPTR_MAX - base_address) {
        layout->error = MDKR_OBJECT_LAYOUT_ARITHMETIC_OVERFLOW;
    }
}

void *mdkr_object_layout_append(
    MdkrObjectLayout *layout,
    size_t size,
    size_t alignment) {
    uintptr_t address;
    size_t padding;
    size_t aligned_cursor;

    if (layout == NULL) {
        return NULL;
    }
    if (layout->error != MDKR_OBJECT_LAYOUT_OK) {
        return NULL;
    }
    if (!mdkr_is_power_of_two(alignment)) {
        mdkr_object_layout_fail(
            layout, MDKR_OBJECT_LAYOUT_INVALID_ALIGNMENT);
        return NULL;
    }
    if (layout->cursor > layout->capacity) {
        mdkr_object_layout_fail(
            layout, MDKR_OBJECT_LAYOUT_CAPACITY_EXCEEDED);
        return NULL;
    }
    if (layout->cursor > UINTPTR_MAX - (uintptr_t) layout->base) {
        mdkr_object_layout_fail(
            layout, MDKR_OBJECT_LAYOUT_ARITHMETIC_OVERFLOW);
        return NULL;
    }

    address = (uintptr_t) layout->base + layout->cursor;
    padding = (alignment - (address & (alignment - 1))) &
              (alignment - 1);
    if (padding > layout->capacity - layout->cursor) {
        mdkr_object_layout_fail(
            layout, MDKR_OBJECT_LAYOUT_CAPACITY_EXCEEDED);
        return NULL;
    }

    aligned_cursor = layout->cursor + padding;
    if (size > layout->capacity - aligned_cursor) {
        mdkr_object_layout_fail(
            layout, MDKR_OBJECT_LAYOUT_CAPACITY_EXCEEDED);
        return NULL;
    }

    layout->cursor = aligned_cursor + size;
    return layout->base + aligned_cursor;
}

void *mdkr_object_layout_append_array(
    MdkrObjectLayout *layout,
    size_t count,
    size_t element_size,
    size_t alignment) {
    size_t size;

    if (layout == NULL) {
        return NULL;
    }
    if (element_size != 0 && count > SIZE_MAX / element_size) {
        mdkr_object_layout_fail(
            layout, MDKR_OBJECT_LAYOUT_ARITHMETIC_OVERFLOW);
        return NULL;
    }

    size = count * element_size;
    return mdkr_object_layout_append(layout, size, alignment);
}

bool mdkr_object_layout_finish(
    MdkrObjectLayout *layout,
    size_t final_alignment,
    size_t *size_out) {
    void *end;

    if (layout == NULL || size_out == NULL) {
        mdkr_object_layout_fail(
            layout, MDKR_OBJECT_LAYOUT_INVALID_ARGUMENT);
        return false;
    }

    end = mdkr_object_layout_append(layout, 0, final_alignment);
    if (end == NULL || layout->error != MDKR_OBJECT_LAYOUT_OK) {
        return false;
    }

    /*
     * append(size=0) aligns the returned address but intentionally does not
     * advance past it. Convert that address back to the checked byte count.
     */
    layout->cursor = (size_t) ((uint8_t *) end - layout->base);
    *size_out = layout->cursor;
    return true;
}

MdkrObjectLayoutError mdkr_object_layout_error(
    const MdkrObjectLayout *layout) {
    if (layout == NULL) {
        return MDKR_OBJECT_LAYOUT_INVALID_ARGUMENT;
    }
    return layout->error;
}

const char *mdkr_object_layout_error_string(
    MdkrObjectLayoutError error) {
    switch (error) {
        case MDKR_OBJECT_LAYOUT_OK:
            return "ok";
        case MDKR_OBJECT_LAYOUT_INVALID_ARGUMENT:
            return "invalid argument";
        case MDKR_OBJECT_LAYOUT_INVALID_ALIGNMENT:
            return "invalid alignment";
        case MDKR_OBJECT_LAYOUT_ARITHMETIC_OVERFLOW:
            return "arithmetic overflow";
        case MDKR_OBJECT_LAYOUT_CAPACITY_EXCEEDED:
            return "capacity exceeded";
    }
    return "unknown error";
}

MdkrLevelObjectRecordError mdkr_level_object_record_parse(
    const void *bytes,
    size_t bytes_remaining,
    size_t object_id_count,
    MdkrLevelObjectRecordView *view_out) {
    const uint8_t *record = (const uint8_t *) bytes;
    size_t stride;
    uint16_t object_id;

    if (record == NULL || view_out == NULL || object_id_count == 0) {
        return MDKR_LEVEL_OBJECT_RECORD_INVALID_ARGUMENT;
    }
    if (bytes_remaining < MDKR_LEVEL_OBJECT_COMMON_SIZE) {
        return MDKR_LEVEL_OBJECT_RECORD_TRUNCATED_HEADER;
    }

    stride = record[1] & 0x3Fu;
    if (stride < MDKR_LEVEL_OBJECT_COMMON_SIZE) {
        return MDKR_LEVEL_OBJECT_RECORD_SHORT_STRIDE;
    }
    if ((stride & 1u) != 0) {
        return MDKR_LEVEL_OBJECT_RECORD_ODD_STRIDE;
    }
    if (stride > bytes_remaining) {
        return MDKR_LEVEL_OBJECT_RECORD_TRUNCATED_BODY;
    }

    object_id = (uint16_t) record[0] |
                (uint16_t) ((record[1] & 0x80u) << 1);
    if ((size_t) object_id >= object_id_count) {
        return MDKR_LEVEL_OBJECT_RECORD_BAD_OBJECT_ID;
    }

    view_out->bytes = record;
    view_out->size = stride;
    view_out->object_id = object_id;
    return MDKR_LEVEL_OBJECT_RECORD_OK;
}

const char *mdkr_level_object_record_error_string(
    MdkrLevelObjectRecordError error) {
    switch (error) {
        case MDKR_LEVEL_OBJECT_RECORD_OK:
            return "ok";
        case MDKR_LEVEL_OBJECT_RECORD_INVALID_ARGUMENT:
            return "invalid argument";
        case MDKR_LEVEL_OBJECT_RECORD_TRUNCATED_HEADER:
            return "truncated common header";
        case MDKR_LEVEL_OBJECT_RECORD_SHORT_STRIDE:
            return "stride smaller than common header";
        case MDKR_LEVEL_OBJECT_RECORD_ODD_STRIDE:
            return "odd stride violates two-byte record alignment";
        case MDKR_LEVEL_OBJECT_RECORD_TRUNCATED_BODY:
            return "record body exceeds remaining bytes";
        case MDKR_LEVEL_OBJECT_RECORD_BAD_OBJECT_ID:
            return "object id exceeds translation table";
    }
    return "unknown error";
}
