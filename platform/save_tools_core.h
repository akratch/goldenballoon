#ifndef MDKR_SAVE_TOOLS_CORE_H
#define MDKR_SAVE_TOOLS_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "save_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdkrSaveToolResult {
    MDKR_SAVE_TOOL_OK = 0,
    MDKR_SAVE_TOOL_ERR_ARGUMENT = -1,
    MDKR_SAVE_TOOL_ERR_CODEC = -2,
    MDKR_SAVE_TOOL_ERR_CAPACITY = -3,
    MDKR_SAVE_TOOL_ERR_FIELD = -4,
    MDKR_SAVE_TOOL_ERR_CORRUPT = -5
} MdkrSaveToolResult;

typedef enum MdkrSaveSlotField {
    MDKR_SAVE_SLOT_FIELD_TAJ_FLAGS = 0,
    MDKR_SAVE_SLOT_FIELD_TROPHIES,
    MDKR_SAVE_SLOT_FIELD_BOSSES,
    MDKR_SAVE_SLOT_FIELD_TT_AMULET,
    MDKR_SAVE_SLOT_FIELD_WIZPIG_AMULET,
    MDKR_SAVE_SLOT_FIELD_KEYS,
    MDKR_SAVE_SLOT_FIELD_CUTSCENES
} MdkrSaveSlotField;

typedef enum MdkrSaveConfigField {
    MDKR_SAVE_CONFIG_FIELD_ADVENTURE_TWO = 0,
    MDKR_SAVE_CONFIG_FIELD_DRUMSTICK,
    MDKR_SAVE_CONFIG_FIELD_LANGUAGE,
    MDKR_SAVE_CONFIG_FIELD_TT_COURSES,
    MDKR_SAVE_CONFIG_FIELD_DEFAULT,
    MDKR_SAVE_CONFIG_FIELD_SUBTITLES
} MdkrSaveConfigField;

MdkrSaveToolResult mdkr_save_summary_json(
    const uint8_t *bytes, size_t byte_count, char *output,
    size_t output_capacity, size_t *output_size, size_t *required_capacity);

MdkrSaveToolResult mdkr_save_edit_slot_state(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot, int create,
    int erase, uint8_t output[MDKR_SAVE_IMAGE_SIZE]);
MdkrSaveToolResult mdkr_save_edit_slot_name(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot, const char *name,
    uint8_t output[MDKR_SAVE_IMAGE_SIZE]);
MdkrSaveToolResult mdkr_save_edit_slot_field(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot,
    MdkrSaveSlotField field, uint32_t value,
    uint8_t output[MDKR_SAVE_IMAGE_SIZE]);
MdkrSaveToolResult mdkr_save_edit_course(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot, unsigned course,
    unsigned status, uint8_t output[MDKR_SAVE_IMAGE_SIZE]);
MdkrSaveToolResult mdkr_save_edit_balloon(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot, unsigned world,
    unsigned count, uint8_t output[MDKR_SAVE_IMAGE_SIZE]);
MdkrSaveToolResult mdkr_save_edit_world_flags(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot, unsigned world,
    unsigned flags, uint8_t output[MDKR_SAVE_IMAGE_SIZE]);
MdkrSaveToolResult mdkr_save_edit_config(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], MdkrSaveConfigField field,
    uint32_t value, uint8_t output[MDKR_SAVE_IMAGE_SIZE]);
MdkrSaveToolResult mdkr_save_reset_records(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned record_mask,
    uint8_t output[MDKR_SAVE_IMAGE_SIZE]);
MdkrSaveToolResult mdkr_save_recover_blocks(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned block_mask,
    uint8_t output[MDKR_SAVE_IMAGE_SIZE]);
MdkrSaveToolResult mdkr_save_copy_blocks(
    const uint8_t destination[MDKR_SAVE_IMAGE_SIZE],
    const uint8_t source[MDKR_SAVE_IMAGE_SIZE], unsigned block_mask,
    uint8_t output[MDKR_SAVE_IMAGE_SIZE]);

const char *mdkr_save_tool_result_string(MdkrSaveToolResult result);

#ifdef __cplusplus
}
#endif

#endif
