#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <emscripten/emscripten.h>

#include "save_codec.h"
#include "save_container.h"
#include "save_tools_core.h"
#include "virtual_pak.h"

static uint8_t s_current[MDKR_SAVE_IMAGE_SIZE];
static uint8_t s_baseline[MDKR_SAVE_IMAGE_SIZE];
static MdkrSaveContainerMetadata s_metadata;
static MdkrSaveInputFormat s_input_format;
static int s_loaded;

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_validate_pak(
    const uint8_t *input, size_t input_size) {
    MdkrVirtualPak candidate;
    return (int)mdkr_virtual_pak_decode(input, input_size, &candidate);
}

static int require_loaded(void) {
    return s_loaded ? 0 : 1;
}

static int commit_edit(MdkrSaveToolResult result,
                       const uint8_t changed[MDKR_SAVE_IMAGE_SIZE]) {
    if (result != MDKR_SAVE_TOOL_OK) return (int) result;
    memcpy(s_current, changed, sizeof(s_current));
    return 0;
}

static int copy_metadata(char *output, size_t capacity, const char *value) {
    int count;
    if (value == NULL) value = "";
    count = snprintf(output, capacity, "%s", value);
    return count >= 0 && (size_t) count < capacity;
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_load(const uint8_t *input, size_t input_size) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    MdkrSaveContainerMetadata metadata;
    MdkrSaveInputFormat input_format;
    MdkrSaveContainerResult result =
        mdkr_save_container_decode(input, input_size, payload, &input_format,
                                   &metadata);
    MdkrSaveDocument document;
    if (result != MDKR_SAVE_CONTAINER_OK) {
        return 100 - (int) result;
    }
    if (mdkr_save_decode(payload, sizeof(payload), &document) != MDKR_SAVE_OK) {
        return 200;
    }
    memcpy(s_current, payload, sizeof(s_current));
    memcpy(s_baseline, payload, sizeof(s_baseline));
    s_metadata = metadata;
    s_input_format = input_format;
    s_loaded = 1;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void mdkr_save_tools_blank(void) {
    memset(s_current, 0, sizeof(s_current));
    memcpy(s_baseline, s_current, sizeof(s_baseline));
    memset(&s_metadata, 0, sizeof(s_metadata));
    s_input_format = MDKR_SAVE_INPUT_RAW;
    s_loaded = 1;
}

EMSCRIPTEN_KEEPALIVE
unsigned mdkr_save_tools_input_format(void) {
    return s_loaded ? (unsigned) s_input_format : UINT32_MAX;
}

EMSCRIPTEN_KEEPALIVE
size_t mdkr_save_tools_metadata(unsigned field, char *output,
                                size_t output_capacity) {
    const char *value;
    size_t required;
    if (!s_loaded) return 0;
    switch (field) {
        case 0: value = s_metadata.created_at; break;
        case 1: value = s_metadata.app_version; break;
        case 2: value = s_metadata.source; break;
        default: return 0;
    }
    required = strlen(value) + 1;
    if (output == NULL) return required;
    if (output_capacity < required) return 0;
    memcpy(output, value, required);
    return required;
}

EMSCRIPTEN_KEEPALIVE
size_t mdkr_save_tools_summary_size(void) {
    size_t required = 0;
    if (require_loaded() != 0 ||
        mdkr_save_summary_json(s_current, sizeof(s_current), NULL, 0, NULL,
                               &required) != MDKR_SAVE_TOOL_OK) {
        return 0;
    }
    return required;
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_summary(char *output, size_t output_capacity) {
    if (require_loaded() != 0) return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    return (int) mdkr_save_summary_json(
        s_current, sizeof(s_current), output, output_capacity, NULL, NULL);
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_copy_raw(uint8_t *output, size_t output_capacity) {
    if (require_loaded() != 0 || output == NULL) {
        return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    }
    if (output_capacity < sizeof(s_current)) {
        return MDKR_SAVE_TOOL_ERR_CAPACITY;
    }
    memcpy(output, s_current, sizeof(s_current));
    return 0;
}

static int make_metadata(MdkrSaveContainerMetadata *metadata,
                         const char *created_at, const char *app_version,
                         const char *source) {
    memset(metadata, 0, sizeof(*metadata));
    return copy_metadata(metadata->created_at, sizeof(metadata->created_at),
                         created_at) &&
           copy_metadata(metadata->app_version, sizeof(metadata->app_version),
                         app_version) &&
           copy_metadata(metadata->source, sizeof(metadata->source), source);
}

EMSCRIPTEN_KEEPALIVE
size_t mdkr_save_tools_container_size(const char *created_at,
                                      const char *app_version,
                                      const char *source) {
    MdkrSaveContainerMetadata metadata;
    size_t required = 0;
    if (require_loaded() != 0 ||
        !make_metadata(&metadata, created_at, app_version, source) ||
        mdkr_save_container_encode(s_current, &metadata, NULL, 0, NULL,
                                   &required) != MDKR_SAVE_CONTAINER_OK) {
        return 0;
    }
    return required;
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_export_container(
    const char *created_at, const char *app_version, const char *source,
    char *output, size_t output_capacity, size_t *output_size) {
    MdkrSaveContainerMetadata metadata;
    if (require_loaded() != 0 ||
        !make_metadata(&metadata, created_at, app_version, source)) {
        return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    }
    return (int) mdkr_save_container_encode(
        s_current, &metadata, output, output_capacity, output_size, NULL);
}

EMSCRIPTEN_KEEPALIVE
unsigned mdkr_save_tools_corrupt_mask(void) {
    MdkrSaveDocument document;
    unsigned mask = 0;
    unsigned i;
    if (require_loaded() != 0 ||
        mdkr_save_decode(s_current, sizeof(s_current), &document) !=
            MDKR_SAVE_OK) {
        return UINT32_MAX;
    }
    for (i = 0; i < MDKR_SAVE_BLOCK_COUNT; i++) {
        if (document.block_status[i] == MDKR_SAVE_BLOCK_CORRUPT) {
            mask |= 1u << i;
        }
    }
    return mask;
}

EMSCRIPTEN_KEEPALIVE
unsigned mdkr_save_tools_diff_count(void) {
    unsigned count = 0;
    unsigned i;
    if (require_loaded() != 0) return 0;
    for (i = 0; i < MDKR_SAVE_IMAGE_SIZE; i++) {
        if (s_current[i] != s_baseline[i]) count++;
    }
    return count;
}

EMSCRIPTEN_KEEPALIVE
void mdkr_save_tools_accept_baseline(void) {
    if (s_loaded) memcpy(s_baseline, s_current, sizeof(s_baseline));
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_recover(unsigned block_mask) {
    uint8_t changed[MDKR_SAVE_IMAGE_SIZE];
    if (require_loaded() != 0) return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    return commit_edit(
        mdkr_save_recover_blocks(s_current, block_mask, changed), changed);
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_copy_blocks(const uint8_t *source, size_t source_size,
                                unsigned block_mask) {
    uint8_t changed[MDKR_SAVE_IMAGE_SIZE];
    if (require_loaded() != 0 || source == NULL ||
        source_size != MDKR_SAVE_IMAGE_SIZE) {
        return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    }
    return commit_edit(
        mdkr_save_copy_blocks(s_current, source, block_mask, changed), changed);
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_slot_state(unsigned slot, int create, int erase) {
    uint8_t changed[MDKR_SAVE_IMAGE_SIZE];
    if (require_loaded() != 0) return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    return commit_edit(
        mdkr_save_edit_slot_state(s_current, slot, create, erase, changed),
        changed);
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_slot_name(unsigned slot, const char *name) {
    uint8_t changed[MDKR_SAVE_IMAGE_SIZE];
    if (require_loaded() != 0) return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    return commit_edit(
        mdkr_save_edit_slot_name(s_current, slot, name, changed), changed);
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_slot_field(unsigned slot, unsigned field, uint32_t value) {
    uint8_t changed[MDKR_SAVE_IMAGE_SIZE];
    if (require_loaded() != 0) return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    return commit_edit(
        mdkr_save_edit_slot_field(s_current, slot, (MdkrSaveSlotField) field,
                                  value, changed),
        changed);
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_course(unsigned slot, unsigned course, unsigned status) {
    uint8_t changed[MDKR_SAVE_IMAGE_SIZE];
    if (require_loaded() != 0) return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    return commit_edit(
        mdkr_save_edit_course(s_current, slot, course, status, changed),
        changed);
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_balloon(unsigned slot, unsigned world, unsigned count) {
    uint8_t changed[MDKR_SAVE_IMAGE_SIZE];
    if (require_loaded() != 0) return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    return commit_edit(
        mdkr_save_edit_balloon(s_current, slot, world, count, changed),
        changed);
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_world_flags(unsigned slot, unsigned world, unsigned flags) {
    uint8_t changed[MDKR_SAVE_IMAGE_SIZE];
    if (require_loaded() != 0) return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    return commit_edit(
        mdkr_save_edit_world_flags(s_current, slot, world, flags, changed),
        changed);
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_config(unsigned field, uint32_t value) {
    uint8_t changed[MDKR_SAVE_IMAGE_SIZE];
    if (require_loaded() != 0) return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    return commit_edit(
        mdkr_save_edit_config(s_current, (MdkrSaveConfigField) field, value,
                              changed),
        changed);
}

EMSCRIPTEN_KEEPALIVE
int mdkr_save_tools_reset_records(unsigned record_mask) {
    uint8_t changed[MDKR_SAVE_IMAGE_SIZE];
    if (require_loaded() != 0) return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    return commit_edit(
        mdkr_save_reset_records(s_current, record_mask, changed), changed);
}

int main(void) {
    return 0;
}
