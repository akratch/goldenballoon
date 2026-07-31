#ifndef MDKR_SAVE_CODEC_H
#define MDKR_SAVE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_SAVE_IMAGE_SIZE 512u
#define MDKR_SAVE_SLOT_COUNT 3u
#define MDKR_SAVE_SLOT_SIZE 40u
#define MDKR_SAVE_COURSE_COUNT 34u
#define MDKR_SAVE_WORLD_COUNT 6u
#define MDKR_SAVE_RECORD_BLOCK_SIZE 192u
#define MDKR_SAVE_TT_COURSE_COUNT 20u

typedef enum MdkrSaveResult {
    MDKR_SAVE_OK = 0,
    MDKR_SAVE_ERR_ARGUMENT = -1,
    MDKR_SAVE_ERR_SIZE = -2,
    MDKR_SAVE_ERR_PATCH = -3,
    MDKR_SAVE_ERR_CORRUPT_BASE = -4
} MdkrSaveResult;

typedef enum MdkrSaveBlock {
    MDKR_SAVE_BLOCK_SLOT_1 = 0,
    MDKR_SAVE_BLOCK_SLOT_2,
    MDKR_SAVE_BLOCK_SLOT_3,
    MDKR_SAVE_BLOCK_CONFIG,
    MDKR_SAVE_BLOCK_FAST_LAPS,
    MDKR_SAVE_BLOCK_COURSE_TIMES,
    MDKR_SAVE_BLOCK_COUNT
} MdkrSaveBlock;

typedef enum MdkrSaveBlockStatus {
    MDKR_SAVE_BLOCK_EMPTY = 0,
    MDKR_SAVE_BLOCK_VALID,
    MDKR_SAVE_BLOCK_NONCANONICAL,
    MDKR_SAVE_BLOCK_CORRUPT
} MdkrSaveBlockStatus;

typedef struct MdkrSaveSlot {
    uint8_t course_status[MDKR_SAVE_COURSE_COUNT];
    uint8_t taj_flags;
    uint16_t trophies;
    uint16_t bosses;
    uint8_t balloons[MDKR_SAVE_WORLD_COUNT];
    uint8_t tt_amulet;
    uint8_t wizpig_amulet;
    uint16_t world_flags[MDKR_SAVE_WORLD_COUNT];
    uint8_t keys;
    uint32_t cutscene_flags;
    uint16_t filename;
    uint8_t reserved;
} MdkrSaveSlot;

typedef struct MdkrSaveConfig {
    uint8_t adventure_two_unlocked;
    uint8_t drumstick_unlocked;
    uint8_t language;
    uint32_t tt_course_flags;
    uint8_t default_flag;
    uint8_t subtitles;
    uint64_t unknown_flags;
} MdkrSaveConfig;

typedef struct MdkrSaveDocument {
    uint8_t bytes[MDKR_SAVE_IMAGE_SIZE];
    MdkrSaveBlockStatus block_status[MDKR_SAVE_BLOCK_COUNT];
    MdkrSaveSlot slots[MDKR_SAVE_SLOT_COUNT];
    MdkrSaveConfig config;
} MdkrSaveDocument;

typedef struct MdkrSaveReport {
    MdkrSaveBlockStatus block_status[MDKR_SAVE_BLOCK_COUNT];
    uint8_t corrupt_block_count;
    uint8_t noncanonical_block_count;
} MdkrSaveReport;

enum {
    MDKR_SLOT_PATCH_ERASE = UINT32_C(1) << 0,
    MDKR_SLOT_PATCH_CREATE = UINT32_C(1) << 1,
    MDKR_SLOT_PATCH_TAJ_FLAGS = UINT32_C(1) << 2,
    MDKR_SLOT_PATCH_TROPHIES = UINT32_C(1) << 3,
    MDKR_SLOT_PATCH_BOSSES = UINT32_C(1) << 4,
    MDKR_SLOT_PATCH_TT_AMULET = UINT32_C(1) << 5,
    MDKR_SLOT_PATCH_WIZPIG_AMULET = UINT32_C(1) << 6,
    MDKR_SLOT_PATCH_KEYS = UINT32_C(1) << 7,
    MDKR_SLOT_PATCH_CUTSCENES = UINT32_C(1) << 8,
    MDKR_SLOT_PATCH_FILENAME = UINT32_C(1) << 9
};

typedef struct MdkrSaveSlotPatch {
    uint32_t fields;
    uint64_t course_mask;
    uint8_t balloon_mask;
    uint8_t world_flags_mask;
    MdkrSaveSlot values;
} MdkrSaveSlotPatch;

enum {
    MDKR_CONFIG_PATCH_ADVENTURE_TWO = UINT32_C(1) << 0,
    MDKR_CONFIG_PATCH_DRUMSTICK = UINT32_C(1) << 1,
    MDKR_CONFIG_PATCH_LANGUAGE = UINT32_C(1) << 2,
    MDKR_CONFIG_PATCH_TT_COURSES = UINT32_C(1) << 3,
    MDKR_CONFIG_PATCH_DEFAULT_FLAG = UINT32_C(1) << 4,
    MDKR_CONFIG_PATCH_SUBTITLES = UINT32_C(1) << 5
};

typedef struct MdkrSaveConfigPatch {
    uint32_t fields;
    MdkrSaveConfig values;
} MdkrSaveConfigPatch;

typedef struct MdkrSavePatch {
    uint8_t slot_mask;
    MdkrSaveSlotPatch slots[MDKR_SAVE_SLOT_COUNT];
    MdkrSaveConfigPatch config;
    uint8_t reset_record_blocks;
} MdkrSavePatch;

typedef enum MdkrSaveRecoveryAction {
    MDKR_SAVE_RECOVER_RETAIN = 0,
    MDKR_SAVE_RECOVER_RESET
} MdkrSaveRecoveryAction;

typedef struct MdkrRecoveryPlan {
    MdkrSaveRecoveryAction block_action[MDKR_SAVE_BLOCK_COUNT];
} MdkrRecoveryPlan;

MdkrSaveResult mdkr_save_decode(const uint8_t *bytes, size_t byte_count,
                                MdkrSaveDocument *out);
MdkrSaveResult mdkr_save_validate(const MdkrSaveDocument *doc,
                                  MdkrSaveReport *out);
MdkrSaveResult mdkr_save_apply(const MdkrSaveDocument *base,
                               const MdkrSavePatch *patch,
                               MdkrSaveDocument *out);
MdkrSaveResult mdkr_save_encode(const MdkrSaveDocument *doc, uint8_t *bytes,
                                size_t byte_capacity);
MdkrSaveResult mdkr_save_recover(const uint8_t *bytes, size_t byte_count,
                                 const MdkrRecoveryPlan *plan,
                                 MdkrSaveDocument *out);

uint16_t mdkr_save_sum_checksum(const uint8_t *block, size_t block_size);
uint8_t mdkr_save_config_checksum(uint64_t value);
uint64_t mdkr_save_config_decode_value(const uint8_t bytes[8]);
void mdkr_save_config_encode_value(uint64_t value, uint8_t bytes[8]);
void mdkr_save_filename_decode(uint16_t encoded, char output[4]);
MdkrSaveResult mdkr_save_filename_encode(const char *name, uint16_t *out);

/*
 * The EEPROM stores one of four ordered course states, while live Settings use
 * prerequisite bits: visited, cleared, silver-coins-cleared. Encoding accepts
 * only the longest valid prerequisite prefix, so a malformed live bit set can
 * lose impossible progress but can never gain progress during a round trip.
 */
uint8_t mdkr_save_course_flags_to_status(uint32_t flags);
uint32_t mdkr_save_course_status_to_flags(uint8_t status);

#ifdef __cplusplus
}
#endif

#endif
