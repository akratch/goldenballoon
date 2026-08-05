#include "save_codec.h"

#include <stdio.h>
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

static int all_value(const uint8_t *bytes, size_t size, uint8_t value) {
    size_t i;
    for (i = 0; i < size; i++) {
        if (bytes[i] != value) {
            return 0;
        }
    }
    return 1;
}

static MdkrSaveDocument blank_document(void) {
    uint8_t bytes[MDKR_SAVE_IMAGE_SIZE] = { 0 };
    MdkrSaveDocument document;
    memset(&document, 0xA5, sizeof(document));
    CHECK(mdkr_save_decode(bytes, sizeof(bytes), &document) == MDKR_SAVE_OK);
    return document;
}

static void test_exact_size_and_empty(void) {
    uint8_t bytes[MDKR_SAVE_IMAGE_SIZE + 1] = { 0 };
    MdkrSaveDocument document;
    MdkrSaveDocument before;
    unsigned i;

    memset(&document, 0xCC, sizeof(document));
    before = document;
    CHECK(mdkr_save_decode(bytes, MDKR_SAVE_IMAGE_SIZE - 1, &document) ==
          MDKR_SAVE_ERR_SIZE);
    CHECK(memcmp(&document, &before, sizeof(document)) == 0);
    CHECK(mdkr_save_decode(bytes, MDKR_SAVE_IMAGE_SIZE + 1, &document) ==
          MDKR_SAVE_ERR_SIZE);
    CHECK(memcmp(&document, &before, sizeof(document)) == 0);
    CHECK(mdkr_save_decode(NULL, MDKR_SAVE_IMAGE_SIZE, &document) ==
          MDKR_SAVE_ERR_ARGUMENT);

    CHECK(mdkr_save_decode(bytes, MDKR_SAVE_IMAGE_SIZE, &document) ==
          MDKR_SAVE_OK);
    for (i = 0; i < MDKR_SAVE_BLOCK_COUNT; i++) {
        CHECK(document.block_status[i] == MDKR_SAVE_BLOCK_EMPTY);
    }
    memset(bytes, 0xFF, MDKR_SAVE_IMAGE_SIZE);
    CHECK(mdkr_save_decode(bytes, MDKR_SAVE_IMAGE_SIZE, &document) ==
          MDKR_SAVE_OK);
    for (i = 0; i < MDKR_SAVE_BLOCK_COUNT; i++) {
        CHECK(document.block_status[i] == MDKR_SAVE_BLOCK_EMPTY);
    }
}

static void test_slot_patch_and_preservation(void) {
    MdkrSaveDocument base = blank_document();
    MdkrSaveDocument changed;
    MdkrSaveDocument changed_again;
    MdkrSavePatch patch;
    uint8_t encoded[MDKR_SAVE_IMAGE_SIZE];
    uint8_t before[MDKR_SAVE_IMAGE_SIZE];
    uint16_t checksum;

    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = 1;
    patch.slots[0].fields =
        MDKR_SLOT_PATCH_CREATE | MDKR_SLOT_PATCH_TAJ_FLAGS |
        MDKR_SLOT_PATCH_TROPHIES | MDKR_SLOT_PATCH_BOSSES |
        MDKR_SLOT_PATCH_TT_AMULET | MDKR_SLOT_PATCH_WIZPIG_AMULET |
        MDKR_SLOT_PATCH_KEYS | MDKR_SLOT_PATCH_CUTSCENES |
        MDKR_SLOT_PATCH_FILENAME;
    patch.slots[0].course_mask = (UINT64_C(1) << 0) | (UINT64_C(1) << 33);
    patch.slots[0].balloon_mask = UINT8_C(0x3F);
    patch.slots[0].world_flags_mask = UINT8_C(0x21);
    patch.slots[0].values.course_status[0] = 3;
    patch.slots[0].values.course_status[33] = 2;
    patch.slots[0].values.taj_flags = 0x09;
    patch.slots[0].values.trophies = 0x155;
    patch.slots[0].values.bosses = 0xA55;
    patch.slots[0].values.balloons[0] = 47;
    patch.slots[0].values.balloons[1] = 8;
    patch.slots[0].values.balloons[2] = 9;
    patch.slots[0].values.balloons[3] = 10;
    patch.slots[0].values.balloons[4] = 11;
    patch.slots[0].values.balloons[5] = 9;
    patch.slots[0].values.tt_amulet = 4;
    patch.slots[0].values.wizpig_amulet = 3;
    patch.slots[0].values.world_flags[0] = 0x1234;
    patch.slots[0].values.world_flags[5] = 0xCAFE;
    patch.slots[0].values.keys = 0x1E;
    patch.slots[0].values.cutscene_flags = UINT32_C(0x89ABCDEF);
    patch.slots[0].values.filename = 0x4567;

    CHECK(mdkr_save_apply(&base, &patch, &changed) == MDKR_SAVE_OK);
    CHECK(changed.block_status[0] == MDKR_SAVE_BLOCK_VALID);
    CHECK(changed.slots[0].course_status[0] == 3);
    CHECK(changed.slots[0].course_status[33] == 2);
    CHECK(changed.slots[0].taj_flags == 0x09);
    CHECK(changed.slots[0].trophies == 0x155);
    CHECK(changed.slots[0].bosses == 0xA55);
    CHECK(changed.slots[0].balloons[0] == 47);
    CHECK(changed.slots[0].tt_amulet == 4);
    CHECK(changed.slots[0].wizpig_amulet == 3);
    CHECK(changed.slots[0].world_flags[5] == 0xCAFE);
    CHECK(changed.slots[0].keys == 0x1E);
    CHECK(changed.slots[0].cutscene_flags == UINT32_C(0x89ABCDEF));
    CHECK(changed.slots[0].filename == 0x4567);
    checksum = (uint16_t) (((uint16_t) changed.bytes[0] << 8) |
                           changed.bytes[1]);
    CHECK(checksum ==
          mdkr_save_sum_checksum(changed.bytes, MDKR_SAVE_SLOT_SIZE));
    CHECK(all_value(changed.bytes + MDKR_SAVE_SLOT_SIZE,
                    MDKR_SAVE_IMAGE_SIZE - MDKR_SAVE_SLOT_SIZE, 0));

    CHECK(mdkr_save_encode(&changed, encoded, sizeof(encoded)) == MDKR_SAVE_OK);
    CHECK(memcmp(encoded, changed.bytes, sizeof(encoded)) == 0);

    changed.bytes[39] = 0xA5;
    checksum = mdkr_save_sum_checksum(changed.bytes, MDKR_SAVE_SLOT_SIZE);
    changed.bytes[0] = (uint8_t) (checksum >> 8);
    changed.bytes[1] = (uint8_t) checksum;
    CHECK(mdkr_save_decode(changed.bytes, sizeof(changed.bytes), &changed) ==
          MDKR_SAVE_OK);
    memcpy(before, changed.bytes, sizeof(before));
    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = 1;
    patch.slots[0].balloon_mask = 1;
    patch.slots[0].values.balloons[0] = 48;
    CHECK(mdkr_save_apply(&changed, &patch, &changed_again) == MDKR_SAVE_OK);
    CHECK(changed_again.bytes[39] == 0xA5);
    CHECK(changed_again.slots[0].balloons[0] == 48);
    CHECK(memcmp(changed_again.bytes + MDKR_SAVE_SLOT_SIZE,
                 before + MDKR_SAVE_SLOT_SIZE,
                 MDKR_SAVE_IMAGE_SIZE - MDKR_SAVE_SLOT_SIZE) == 0);
}

static void test_course_status_contract(void) {
    static const uint8_t expected_status[8] = {
        0, /* none */
        1, /* visited */
        0, /* cleared without visited: reject impossible progress */
        2, /* visited + cleared */
        0, /* silver without prerequisites */
        1, /* visited + silver: retain only the valid prefix */
        0, /* cleared + silver without visited */
        3  /* complete canonical chain */
    };
    unsigned flags;
    unsigned status;

    for (status = 0; status < 4; status++) {
        uint32_t live = mdkr_save_course_status_to_flags((uint8_t) status);
        CHECK(mdkr_save_course_flags_to_status(live) == status);
    }
    CHECK(mdkr_save_course_status_to_flags(4) == 0);
    CHECK(mdkr_save_course_status_to_flags(UINT8_MAX) == 0);
    for (flags = 0; flags < 8; flags++) {
        uint8_t encoded = mdkr_save_course_flags_to_status(flags);
        uint32_t decoded = mdkr_save_course_status_to_flags(encoded);
        CHECK(encoded == expected_status[flags]);
        CHECK((decoded & ~flags) == 0);
    }
}

static void test_noncanonical_and_invalid_slot(void) {
    MdkrSaveDocument base = blank_document();
    MdkrSaveDocument changed;
    MdkrSaveDocument invalid;
    MdkrSavePatch patch;
    uint16_t checksum;

    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = 1;
    patch.slots[0].fields =
        MDKR_SLOT_PATCH_CREATE | MDKR_SLOT_PATCH_TAJ_FLAGS;
    patch.slots[0].values.taj_flags = 0x08;
    CHECK(mdkr_save_apply(&base, &patch, &changed) == MDKR_SAVE_OK);
    CHECK(changed.block_status[0] == MDKR_SAVE_BLOCK_NONCANONICAL);

    changed.bytes[19] |= 0x38;
    checksum = mdkr_save_sum_checksum(changed.bytes, MDKR_SAVE_SLOT_SIZE);
    changed.bytes[0] = (uint8_t) (checksum >> 8);
    changed.bytes[1] = (uint8_t) checksum;
    CHECK(mdkr_save_decode(changed.bytes, sizeof(changed.bytes), &invalid) ==
          MDKR_SAVE_OK);
    CHECK(invalid.block_status[0] == MDKR_SAVE_BLOCK_CORRUPT);

    changed.bytes[7] ^= 1;
    CHECK(mdkr_save_decode(changed.bytes, sizeof(changed.bytes), &invalid) ==
          MDKR_SAVE_OK);
    CHECK(invalid.block_status[0] == MDKR_SAVE_BLOCK_CORRUPT);
}

static void test_config_canonical_byte_order(void) {
    MdkrSaveDocument base = blank_document();
    MdkrSaveDocument changed;
    MdkrSavePatch patch;
    uint64_t value;
    uint64_t native_value;

    memset(&patch, 0, sizeof(patch));
    patch.config.fields =
        MDKR_CONFIG_PATCH_ADVENTURE_TWO | MDKR_CONFIG_PATCH_DRUMSTICK |
        MDKR_CONFIG_PATCH_LANGUAGE | MDKR_CONFIG_PATCH_TT_COURSES |
        MDKR_CONFIG_PATCH_DEFAULT_FLAG | MDKR_CONFIG_PATCH_SUBTITLES;
    patch.config.values.adventure_two_unlocked = 1;
    patch.config.values.drumstick_unlocked = 1;
    patch.config.values.language = 2;
    patch.config.values.tt_course_flags = 0xABCDE;
    patch.config.values.default_flag = 1;
    patch.config.values.subtitles = 1;
    CHECK(mdkr_save_apply(&base, &patch, &changed) == MDKR_SAVE_OK);
    CHECK(changed.block_status[MDKR_SAVE_BLOCK_CONFIG] ==
          MDKR_SAVE_BLOCK_VALID);
    CHECK(changed.config.tt_course_flags == 0xABCDE);
    value = mdkr_save_config_decode_value(changed.bytes + 120);
    CHECK((value & 1) == 1);
    CHECK(((value >> 2) & 3) == 2);
    CHECK(((value >> 4) & 0xFFFFF) == 0xABCDE);
    CHECK((uint8_t) (value >> 56) == mdkr_save_config_checksum(value));

    memcpy(&native_value, changed.bytes + 120, sizeof(native_value));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    CHECK(native_value != value);
#endif
}

static void test_atomic_failure_and_recovery(void) {
    MdkrSaveDocument base = blank_document();
    MdkrSaveDocument corrupt;
    MdkrSaveDocument output;
    MdkrSaveDocument sentinel;
    MdkrSavePatch patch;
    MdkrRecoveryPlan plan;

    base.bytes[40] = 1;
    CHECK(mdkr_save_decode(base.bytes, sizeof(base.bytes), &corrupt) ==
          MDKR_SAVE_OK);
    CHECK(corrupt.block_status[1] == MDKR_SAVE_BLOCK_CORRUPT);

    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = 2;
    patch.slots[1].fields = MDKR_SLOT_PATCH_KEYS;
    patch.slots[1].values.keys = 1;
    memset(&output, 0xA6, sizeof(output));
    sentinel = output;
    CHECK(mdkr_save_apply(&corrupt, &patch, &output) ==
          MDKR_SAVE_ERR_CORRUPT_BASE);
    CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);

    memset(&plan, 0, sizeof(plan));
    CHECK(mdkr_save_recover(corrupt.bytes, sizeof(corrupt.bytes), &plan,
                            &output) == MDKR_SAVE_ERR_CORRUPT_BASE);
    plan.block_action[MDKR_SAVE_BLOCK_SLOT_2] = MDKR_SAVE_RECOVER_RESET;
    CHECK(mdkr_save_recover(corrupt.bytes, sizeof(corrupt.bytes), &plan,
                            &output) == MDKR_SAVE_OK);
    CHECK(output.block_status[1] == MDKR_SAVE_BLOCK_EMPTY);
    CHECK(all_value(output.bytes + MDKR_SAVE_SLOT_SIZE,
                    MDKR_SAVE_SLOT_SIZE, 0xFF));
    CHECK(memcmp(output.bytes, corrupt.bytes, MDKR_SAVE_SLOT_SIZE) == 0);
}

static void test_record_reset(void) {
    MdkrSaveDocument base = blank_document();
    MdkrSaveDocument changed;
    MdkrSavePatch patch;

    memset(&patch, 0, sizeof(patch));
    patch.reset_record_blocks = 3;
    CHECK(mdkr_save_apply(&base, &patch, &changed) == MDKR_SAVE_OK);
    CHECK(changed.block_status[MDKR_SAVE_BLOCK_FAST_LAPS] ==
          MDKR_SAVE_BLOCK_VALID);
    CHECK(changed.block_status[MDKR_SAVE_BLOCK_COURSE_TIMES] ==
          MDKR_SAVE_BLOCK_VALID);
    CHECK(changed.bytes[128] == 0 && changed.bytes[129] == 5);
    CHECK(changed.bytes[320] == 0 && changed.bytes[321] == 5);
}

static uint32_t test_random_state = UINT32_C(0xC001D00D);

static uint32_t test_random(void) {
    uint32_t value = test_random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    test_random_state = value;
    return value;
}

/* Block starts inside the 512-byte image. Slots occupy [0, 120). */
#define TEST_CONFIG_OFFSET 120u
#define TEST_FAST_LAPS_OFFSET 128u
#define TEST_COURSE_TIMES_OFFSET 320u

static void store_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t) (value >> 8);
    bytes[1] = (uint8_t) value;
}

/* Random bytes are corrupt in every block, so half the corpus gets its stored
 * checksums recomputed: those images reach slot, config and record decoding
 * instead of stopping at the first checksum comparison. */
static void repair_block_checksums(uint8_t bytes[MDKR_SAVE_IMAGE_SIZE]) {
    unsigned i;
    uint64_t config;
    for (i = 0; i < MDKR_SAVE_SLOT_COUNT; i++) {
        uint8_t *slot = bytes + i * MDKR_SAVE_SLOT_SIZE;
        store_be16(slot, mdkr_save_sum_checksum(slot, MDKR_SAVE_SLOT_SIZE));
    }
    config = mdkr_save_config_decode_value(bytes + TEST_CONFIG_OFFSET) &
             UINT64_C(0x00FFFFFFFFFFFFFF);
    config |= (uint64_t) mdkr_save_config_checksum(config) << 56;
    mdkr_save_config_encode_value(config, bytes + TEST_CONFIG_OFFSET);
    store_be16(bytes + TEST_FAST_LAPS_OFFSET,
               mdkr_save_sum_checksum(bytes + TEST_FAST_LAPS_OFFSET,
                                      MDKR_SAVE_RECORD_BLOCK_SIZE));
    store_be16(bytes + TEST_COURSE_TIMES_OFFSET,
               mdkr_save_sum_checksum(bytes + TEST_COURSE_TIMES_OFFSET,
                                      MDKR_SAVE_RECORD_BLOCK_SIZE));
}

static void test_arbitrary_image_noop_property(void) {
    uint8_t bytes[MDKR_SAVE_IMAGE_SIZE];
    uint8_t encoded[MDKR_SAVE_IMAGE_SIZE];
    MdkrSaveDocument document;
    MdkrSaveDocument changed;
    MdkrSaveReport report;
    MdkrSavePatch patch;
    unsigned iteration;
    unsigned valid_slots = 0;
    unsigned noncanonical_slots = 0;
    unsigned corrupt_slots = 0;
    unsigned valid_config = 0;
    unsigned valid_records = 0;
    unsigned patched = 0;
    for (iteration = 0; iteration < 10000; iteration++) {
        unsigned corrupt = 0;
        unsigned noncanonical = 0;
        size_t i;
        for (i = 0; i < sizeof(bytes); i++) {
            bytes[i] = (uint8_t) test_random();
        }
        if ((iteration & 1u) == 0u) {
            repair_block_checksums(bytes);
        }

        /* Only the exact image size is decodable, whatever the content. */
        if ((iteration % 64u) == 0u) {
            memset(&changed, 0xD7, sizeof(changed));
            document = changed;
            CHECK(mdkr_save_decode(bytes, sizeof(bytes) - 1u, &document) ==
                  MDKR_SAVE_ERR_SIZE);
            CHECK(memcmp(&document, &changed, sizeof(document)) == 0);
        }

        CHECK(mdkr_save_decode(bytes, sizeof(bytes), &document) ==
              MDKR_SAVE_OK);
        CHECK(mdkr_save_encode(&document, encoded, sizeof(encoded)) ==
              MDKR_SAVE_OK);
        CHECK(memcmp(bytes, encoded, sizeof(bytes)) == 0);

        CHECK(mdkr_save_validate(&document, &report) == MDKR_SAVE_OK);
        for (i = 0; i < MDKR_SAVE_BLOCK_COUNT; i++) {
            CHECK(report.block_status[i] == document.block_status[i]);
            if (document.block_status[i] == MDKR_SAVE_BLOCK_CORRUPT) {
                corrupt++;
            } else if (document.block_status[i] ==
                       MDKR_SAVE_BLOCK_NONCANONICAL) {
                noncanonical++;
            }
        }
        CHECK(report.corrupt_block_count == corrupt);
        CHECK(report.noncanonical_block_count == noncanonical);

        for (i = 0; i < MDKR_SAVE_SLOT_COUNT; i++) {
            if (document.block_status[i] == MDKR_SAVE_BLOCK_VALID) {
                valid_slots++;
            } else if (document.block_status[i] ==
                       MDKR_SAVE_BLOCK_NONCANONICAL) {
                noncanonical_slots++;
            } else if (document.block_status[i] == MDKR_SAVE_BLOCK_CORRUPT) {
                corrupt_slots++;
            }
        }
        if (document.block_status[MDKR_SAVE_BLOCK_CONFIG] ==
            MDKR_SAVE_BLOCK_VALID) {
            valid_config++;
        }
        if (document.block_status[MDKR_SAVE_BLOCK_FAST_LAPS] ==
                MDKR_SAVE_BLOCK_VALID &&
            document.block_status[MDKR_SAVE_BLOCK_COURSE_TIMES] ==
                MDKR_SAVE_BLOCK_VALID) {
            valid_records++;
        }

        if (document.block_status[0] == MDKR_SAVE_BLOCK_CORRUPT) {
            continue;
        }
        memset(&patch, 0, sizeof(patch));
        patch.slot_mask = 1;
        patch.slots[0].fields = MDKR_SLOT_PATCH_KEYS;
        patch.slots[0].values.keys = (uint8_t) (test_random() & 0x1Fu);
        CHECK(mdkr_save_apply(&document, &patch, &changed) == MDKR_SAVE_OK);
        CHECK(changed.slots[0].keys == patch.slots[0].values.keys);
        CHECK(memcmp(changed.bytes + MDKR_SAVE_SLOT_SIZE,
                     document.bytes + MDKR_SAVE_SLOT_SIZE,
                     MDKR_SAVE_IMAGE_SIZE - MDKR_SAVE_SLOT_SIZE) == 0);
        patched++;
    }
    CHECK(valid_slots > 0);
    CHECK(noncanonical_slots > 0);
    CHECK(corrupt_slots > 0);
    CHECK(valid_config > 0);
    CHECK(valid_records > 0);
    CHECK(patched > 0);
}

int main(void) {
    test_course_status_contract();
    test_exact_size_and_empty();
    test_slot_patch_and_preservation();
    test_noncanonical_and_invalid_slot();
    test_config_canonical_byte_order();
    test_atomic_failure_and_recovery();
    test_record_reset();
    test_arbitrary_image_noop_property();
    if (failures != 0) {
        fprintf(stderr, "save codec: %d failure(s)\n", failures);
        return 1;
    }
    puts("save codec: all tests passed");
    return 0;
}
