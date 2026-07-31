#include "save_tools_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                                \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static void test_summary_and_edits(void) {
    uint8_t image[MDKR_SAVE_IMAGE_SIZE] = { 0 };
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    uint8_t next[MDKR_SAVE_IMAGE_SIZE];
    MdkrSaveDocument document;
    char *summary;
    size_t summary_size;
    size_t required;

    CHECK(mdkr_save_summary_json(image, sizeof(image), NULL, 0, &summary_size,
                                 &required) == MDKR_SAVE_TOOL_OK);
    CHECK(required == summary_size + 1);
    summary = (char *) malloc(required);
    CHECK(summary != NULL);
    if (summary != NULL) {
        CHECK(mdkr_save_summary_json(image, sizeof(image), summary, required,
                                     NULL, NULL) == MDKR_SAVE_TOOL_OK);
        CHECK(strstr(summary, "\"blocks\":[\"empty\",\"empty\"") != NULL);
        CHECK(strstr(summary, "\"sha256\":\"") != NULL);
        free(summary);
    }

    CHECK(mdkr_save_edit_slot_state(image, 0, 1, 0, edited) ==
          MDKR_SAVE_TOOL_OK);
    CHECK(mdkr_save_edit_slot_name(edited, 0, "DKR", next) ==
          MDKR_SAVE_TOOL_OK);
    memcpy(edited, next, sizeof(edited));
    CHECK(mdkr_save_edit_balloon(edited, 0, 0, 47, next) ==
          MDKR_SAVE_TOOL_OK);
    memcpy(edited, next, sizeof(edited));
    CHECK(mdkr_save_edit_slot_field(edited, 0, MDKR_SAVE_SLOT_FIELD_TT_AMULET,
                                    4, next) == MDKR_SAVE_TOOL_OK);
    memcpy(edited, next, sizeof(edited));
    CHECK(mdkr_save_edit_course(edited, 0, 33, 3, next) ==
          MDKR_SAVE_TOOL_OK);
    memcpy(edited, next, sizeof(edited));
    CHECK(mdkr_save_decode(edited, sizeof(edited), &document) == MDKR_SAVE_OK);
    CHECK(document.block_status[0] == MDKR_SAVE_BLOCK_VALID);
    CHECK(document.slots[0].filename != 0);
    CHECK(document.slots[0].balloons[0] == 47);
    CHECK(document.slots[0].tt_amulet == 4);
    CHECK(document.slots[0].course_status[33] == 3);
    {
        char name[4];
        mdkr_save_filename_decode(document.slots[0].filename, name);
        CHECK(strcmp(name, "DKR") == 0);
    }

    CHECK(mdkr_save_edit_config(
              edited, MDKR_SAVE_CONFIG_FIELD_ADVENTURE_TWO, 1, next) ==
          MDKR_SAVE_TOOL_OK);
    memcpy(edited, next, sizeof(edited));
    CHECK(mdkr_save_edit_config(edited, MDKR_SAVE_CONFIG_FIELD_TT_COURSES,
                                0xFFFFF, next) == MDKR_SAVE_TOOL_OK);
    CHECK(mdkr_save_decode(next, sizeof(next), &document) == MDKR_SAVE_OK);
    CHECK(document.config.adventure_two_unlocked == 1);
    CHECK(document.config.tt_course_flags == 0xFFFFF);
}

static void test_rejections_are_atomic(void) {
    uint8_t image[MDKR_SAVE_IMAGE_SIZE] = { 0 };
    uint8_t output[MDKR_SAVE_IMAGE_SIZE];
    uint8_t before[MDKR_SAVE_IMAGE_SIZE];
    char small[8];
    char small_before[8];

    memset(output, 0xA7, sizeof(output));
    memcpy(before, output, sizeof(output));
    CHECK(mdkr_save_edit_slot_state(image, 3, 1, 0, output) ==
          MDKR_SAVE_TOOL_ERR_FIELD);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(mdkr_save_edit_slot_name(image, 0, "bad", output) ==
          MDKR_SAVE_TOOL_ERR_FIELD);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(mdkr_save_edit_slot_field(image, 0, MDKR_SAVE_SLOT_FIELD_TAJ_FLAGS,
                                    0x08, output) ==
          MDKR_SAVE_TOOL_ERR_FIELD);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(mdkr_save_edit_slot_field(image, 0, MDKR_SAVE_SLOT_FIELD_TT_AMULET,
                                    5, output) ==
          MDKR_SAVE_TOOL_ERR_FIELD);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(mdkr_save_edit_config(image, MDKR_SAVE_CONFIG_FIELD_LANGUAGE, 4,
                                output) == MDKR_SAVE_TOOL_ERR_FIELD);
    CHECK(memcmp(output, before, sizeof(output)) == 0);

    memset(small, 0x4D, sizeof(small));
    memcpy(small_before, small, sizeof(small));
    CHECK(mdkr_save_summary_json(image, sizeof(image), small, sizeof(small),
                                 NULL, NULL) ==
          MDKR_SAVE_TOOL_ERR_CAPACITY);
    CHECK(memcmp(small, small_before, sizeof(small)) == 0);
}

static void test_recovery(void) {
    uint8_t image[MDKR_SAVE_IMAGE_SIZE] = { 0 };
    uint8_t recovered[MDKR_SAVE_IMAGE_SIZE];
    MdkrSaveDocument document;
    image[40] = 1;
    CHECK(mdkr_save_recover_blocks(image, 0, recovered) ==
          MDKR_SAVE_TOOL_ERR_CORRUPT);
    CHECK(mdkr_save_recover_blocks(
              image, 1u << MDKR_SAVE_BLOCK_SLOT_2, recovered) ==
          MDKR_SAVE_TOOL_OK);
    CHECK(mdkr_save_decode(recovered, sizeof(recovered), &document) ==
          MDKR_SAVE_OK);
    CHECK(document.block_status[MDKR_SAVE_BLOCK_SLOT_2] ==
          MDKR_SAVE_BLOCK_EMPTY);
    CHECK(memcmp(recovered, image, MDKR_SAVE_SLOT_SIZE) == 0);
}

static void test_block_copy_merge(void) {
    uint8_t destination[MDKR_SAVE_IMAGE_SIZE] = { 0 };
    uint8_t source[MDKR_SAVE_IMAGE_SIZE] = { 0 };
    uint8_t source_slot[MDKR_SAVE_IMAGE_SIZE];
    uint8_t source_config[MDKR_SAVE_IMAGE_SIZE];
    uint8_t merged[MDKR_SAVE_IMAGE_SIZE];
    uint8_t sentinel[MDKR_SAVE_IMAGE_SIZE];
    MdkrSaveDocument document;

    CHECK(mdkr_save_edit_slot_state(source, 1, 1, 0, source_slot) ==
          MDKR_SAVE_TOOL_OK);
    CHECK(mdkr_save_edit_slot_name(source_slot, 1, "TWO", source) ==
          MDKR_SAVE_TOOL_OK);
    CHECK(mdkr_save_edit_config(
              source, MDKR_SAVE_CONFIG_FIELD_ADVENTURE_TWO, 1,
              source_config) == MDKR_SAVE_TOOL_OK);
    memcpy(source, source_config, sizeof(source));

    memset(destination, 0xA5, MDKR_SAVE_SLOT_SIZE);
    destination[0] = 0;
    destination[1] = 0;
    CHECK(mdkr_save_copy_blocks(
              destination, source,
              (1u << MDKR_SAVE_BLOCK_SLOT_2) |
                  (1u << MDKR_SAVE_BLOCK_CONFIG),
              merged) == MDKR_SAVE_TOOL_OK);
    CHECK(memcmp(merged, destination, MDKR_SAVE_SLOT_SIZE) == 0);
    CHECK(memcmp(merged + MDKR_SAVE_SLOT_SIZE,
                 source + MDKR_SAVE_SLOT_SIZE,
                 MDKR_SAVE_SLOT_SIZE) == 0);
    CHECK(memcmp(merged + 3 * MDKR_SAVE_SLOT_SIZE,
                 source + 3 * MDKR_SAVE_SLOT_SIZE, 8) == 0);
    CHECK(memcmp(merged + 128, destination + 128,
                 MDKR_SAVE_IMAGE_SIZE - 128) == 0);
    CHECK(mdkr_save_decode(merged, sizeof(merged), &document) == MDKR_SAVE_OK);
    CHECK(document.block_status[MDKR_SAVE_BLOCK_SLOT_2] ==
          MDKR_SAVE_BLOCK_VALID);
    CHECK(document.config.adventure_two_unlocked == 1);

    source[MDKR_SAVE_SLOT_SIZE + 5] ^= 1;
    memset(merged, 0x6C, sizeof(merged));
    memcpy(sentinel, merged, sizeof(sentinel));
    CHECK(mdkr_save_copy_blocks(
              destination, source, 1u << MDKR_SAVE_BLOCK_SLOT_2, merged) ==
          MDKR_SAVE_TOOL_ERR_CORRUPT);
    CHECK(memcmp(merged, sentinel, sizeof(merged)) == 0);
    CHECK(mdkr_save_copy_blocks(destination, source, 0, merged) ==
          MDKR_SAVE_TOOL_ERR_FIELD);
    CHECK(memcmp(merged, sentinel, sizeof(merged)) == 0);
    CHECK(mdkr_save_copy_blocks(
              destination, source, 1u << MDKR_SAVE_BLOCK_COUNT, merged) ==
          MDKR_SAVE_TOOL_ERR_FIELD);
    CHECK(memcmp(merged, sentinel, sizeof(merged)) == 0);
}

int main(void) {
    test_summary_and_edits();
    test_rejections_are_atomic();
    test_recovery();
    test_block_copy_merge();
    if (failures != 0) {
        fprintf(stderr, "save tools core: %d failure(s)\n", failures);
        return 1;
    }
    puts("save tools core: all tests passed");
    return 0;
}
