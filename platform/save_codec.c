#include "save_codec.h"

#include <string.h>

enum {
    SLOT_CHECKSUM_BITS = 16,
    SLOT_COURSE_BITS = 68,
    SLOT_TAJ_BITS = 6,
    SLOT_TROPHY_BITS = 10,
    SLOT_BOSS_BITS = 12,
    SLOT_BALLOON_BITS = 7,
    SLOT_AMULET_BITS = 3,
    SLOT_WORLD_FLAGS_BITS = 16,
    SLOT_KEYS_BITS = 8,
    SLOT_CUTSCENE_BITS = 32,
    SLOT_FILENAME_BITS = 16,
    SLOT_RESERVED_BITS = 8,
    CONFIG_OFFSET = 120,
    FAST_LAPS_OFFSET = 128,
    COURSE_TIMES_OFFSET = 320
};

typedef struct BitCursor {
    uint8_t *bytes;
    size_t bit;
} BitCursor;

static int bytes_are(const uint8_t *bytes, size_t count, uint8_t value) {
    size_t i;
    for (i = 0; i < count; i++) {
        if (bytes[i] != value) {
            return 0;
        }
    }
    return 1;
}

static uint32_t bit_read(const uint8_t *bytes, size_t *bit, unsigned width) {
    uint32_t value = 0;
    unsigned i;
    for (i = 0; i < width; i++, (*bit)++) {
        value = (value << 1) |
                ((bytes[*bit >> 3] >> (7u - (*bit & 7u))) & 1u);
    }
    return value;
}

static void bit_write(BitCursor *cursor, unsigned width, uint32_t value) {
    unsigned i;
    for (i = 0; i < width; i++, cursor->bit++) {
        const uint8_t mask =
            (uint8_t) (UINT8_C(1) << (7u - (cursor->bit & 7u)));
        const unsigned source_bit = width - i - 1u;
        if ((value >> source_bit) & 1u) {
            cursor->bytes[cursor->bit >> 3] |= mask;
        } else {
            cursor->bytes[cursor->bit >> 3] &= (uint8_t) ~mask;
        }
    }
}

static uint16_t load_be16(const uint8_t *bytes) {
    return (uint16_t) (((uint16_t) bytes[0] << 8) | bytes[1]);
}

static void store_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t) (value >> 8);
    bytes[1] = (uint8_t) value;
}

uint16_t mdkr_save_sum_checksum(const uint8_t *block, size_t block_size) {
    uint32_t sum = 5;
    size_t i;
    if (block == NULL || block_size < 2) {
        return 0;
    }
    for (i = 2; i < block_size; i++) {
        sum += block[i];
    }
    return (uint16_t) sum;
}

uint8_t mdkr_save_config_checksum(uint64_t value) {
    unsigned sum = 5;
    unsigned i;
    value &= UINT64_C(0x00FFFFFFFFFFFFFF);
    for (i = 0; i < 14; i++) {
        sum += (unsigned) ((value >> (i * 4u)) & UINT64_C(0xF));
    }
    return (uint8_t) sum;
}

uint8_t mdkr_save_course_flags_to_status(uint32_t flags) {
    if ((flags & UINT32_C(1)) == 0) {
        return 0;
    }
    if ((flags & UINT32_C(2)) == 0) {
        return 1;
    }
    if ((flags & UINT32_C(4)) == 0) {
        return 2;
    }
    return 3;
}

uint32_t mdkr_save_course_status_to_flags(uint8_t status) {
    static const uint8_t flags[4] = {0, 1, 3, 7};
    return status < 4u ? flags[status] : 0;
}

uint64_t mdkr_save_config_decode_value(const uint8_t bytes[8]) {
    uint64_t value = 0;
    unsigned i;
    if (bytes == NULL) {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

void mdkr_save_config_encode_value(uint64_t value, uint8_t bytes[8]) {
    int i;
    if (bytes == NULL) {
        return;
    }
    for (i = 7; i >= 0; i--) {
        bytes[i] = (uint8_t) value;
        value >>= 8;
    }
}

static void decode_slot(const uint8_t *bytes, MdkrSaveSlot *slot) {
    size_t bit = SLOT_CHECKSUM_BITS;
    unsigned i;
    memset(slot, 0, sizeof(*slot));
    for (i = 0; i < MDKR_SAVE_COURSE_COUNT; i++) {
        slot->course_status[i] = (uint8_t) bit_read(bytes, &bit, 2);
    }
    slot->taj_flags = (uint8_t) bit_read(bytes, &bit, SLOT_TAJ_BITS);
    slot->trophies = (uint16_t) bit_read(bytes, &bit, SLOT_TROPHY_BITS);
    slot->bosses = (uint16_t) bit_read(bytes, &bit, SLOT_BOSS_BITS);
    for (i = 0; i < MDKR_SAVE_WORLD_COUNT; i++) {
        slot->balloons[i] =
            (uint8_t) bit_read(bytes, &bit, SLOT_BALLOON_BITS);
    }
    slot->tt_amulet = (uint8_t) bit_read(bytes, &bit, SLOT_AMULET_BITS);
    slot->wizpig_amulet =
        (uint8_t) bit_read(bytes, &bit, SLOT_AMULET_BITS);
    for (i = 0; i < MDKR_SAVE_WORLD_COUNT; i++) {
        slot->world_flags[i] =
            (uint16_t) bit_read(bytes, &bit, SLOT_WORLD_FLAGS_BITS);
    }
    slot->keys = (uint8_t) bit_read(bytes, &bit, SLOT_KEYS_BITS);
    slot->cutscene_flags =
        bit_read(bytes, &bit, SLOT_CUTSCENE_BITS);
    slot->filename =
        (uint16_t) bit_read(bytes, &bit, SLOT_FILENAME_BITS);
    slot->reserved = (uint8_t) bit_read(bytes, &bit, SLOT_RESERVED_BITS);
}

static MdkrSaveBlockStatus slot_status(const uint8_t *bytes,
                                       const MdkrSaveSlot *slot) {
    if (bytes_are(bytes, MDKR_SAVE_SLOT_SIZE, UINT8_C(0xFF)) ||
        bytes_are(bytes, MDKR_SAVE_SLOT_SIZE, 0)) {
        return MDKR_SAVE_BLOCK_EMPTY;
    }
    if (load_be16(bytes) !=
        mdkr_save_sum_checksum(bytes, MDKR_SAVE_SLOT_SIZE)) {
        return MDKR_SAVE_BLOCK_CORRUPT;
    }
    if (slot->tt_amulet > 4 || slot->wizpig_amulet > 4) {
        return MDKR_SAVE_BLOCK_CORRUPT;
    }
    if (slot->filename > 0x7FFF ||
        ((slot->taj_flags & UINT8_C(0x38)) != 0 &&
         ((slot->taj_flags >> 3) & UINT8_C(0x07) &
          (uint8_t) ~slot->taj_flags) != 0)) {
        return MDKR_SAVE_BLOCK_NONCANONICAL;
    }
    return MDKR_SAVE_BLOCK_VALID;
}

static void decode_config(const uint8_t *bytes, MdkrSaveConfig *config) {
    uint64_t value = mdkr_save_config_decode_value(bytes);
    uint64_t payload = value & UINT64_C(0x00FFFFFFFFFFFFFF);
    memset(config, 0, sizeof(*config));
    config->adventure_two_unlocked = (uint8_t) (payload & 1u);
    config->drumstick_unlocked = (uint8_t) ((payload >> 1) & 1u);
    config->language = (uint8_t) ((payload >> 2) & 3u);
    config->tt_course_flags = (uint32_t) ((payload >> 4) & 0xFFFFFu);
    config->default_flag = (uint8_t) ((payload >> 24) & 1u);
    config->subtitles = (uint8_t) ((payload >> 25) & 1u);
    config->unknown_flags = payload & UINT64_C(0x00FFFFFFFC000000);
}

static MdkrSaveBlockStatus config_status(const uint8_t *bytes) {
    uint64_t value;
    if (bytes_are(bytes, 8, UINT8_C(0xFF)) || bytes_are(bytes, 8, 0)) {
        return MDKR_SAVE_BLOCK_EMPTY;
    }
    value = mdkr_save_config_decode_value(bytes);
    if ((uint8_t) (value >> 56) != mdkr_save_config_checksum(value)) {
        return MDKR_SAVE_BLOCK_CORRUPT;
    }
    return MDKR_SAVE_BLOCK_VALID;
}

static MdkrSaveBlockStatus record_status(const uint8_t *bytes) {
    if (bytes_are(bytes, MDKR_SAVE_RECORD_BLOCK_SIZE, UINT8_C(0xFF)) ||
        bytes_are(bytes, MDKR_SAVE_RECORD_BLOCK_SIZE, 0)) {
        return MDKR_SAVE_BLOCK_EMPTY;
    }
    return load_be16(bytes) ==
                   mdkr_save_sum_checksum(bytes, MDKR_SAVE_RECORD_BLOCK_SIZE)
               ? MDKR_SAVE_BLOCK_VALID
               : MDKR_SAVE_BLOCK_CORRUPT;
}

MdkrSaveResult mdkr_save_decode(const uint8_t *bytes, size_t byte_count,
                                MdkrSaveDocument *out) {
    uint8_t source[MDKR_SAVE_IMAGE_SIZE];
    unsigned i;
    if (bytes == NULL || out == NULL) {
        return MDKR_SAVE_ERR_ARGUMENT;
    }
    if (byte_count != MDKR_SAVE_IMAGE_SIZE) {
        return MDKR_SAVE_ERR_SIZE;
    }

    /* Permit an in-place refresh (`mdkr_save_decode(doc.bytes, ..., &doc)`).
     * Snapshot first because clearing the decoded model would otherwise erase
     * an input buffer embedded in that same document. */
    memcpy(source, bytes, sizeof(source));
    memset(out, 0, sizeof(*out));
    memcpy(out->bytes, source, sizeof(source));
    for (i = 0; i < MDKR_SAVE_SLOT_COUNT; i++) {
        const uint8_t *slot_bytes = source + i * MDKR_SAVE_SLOT_SIZE;
        decode_slot(slot_bytes, &out->slots[i]);
        out->block_status[i] = slot_status(slot_bytes, &out->slots[i]);
    }
    decode_config(source + CONFIG_OFFSET, &out->config);
    out->block_status[MDKR_SAVE_BLOCK_CONFIG] =
        config_status(source + CONFIG_OFFSET);
    out->block_status[MDKR_SAVE_BLOCK_FAST_LAPS] =
        record_status(source + FAST_LAPS_OFFSET);
    out->block_status[MDKR_SAVE_BLOCK_COURSE_TIMES] =
        record_status(source + COURSE_TIMES_OFFSET);
    return MDKR_SAVE_OK;
}

MdkrSaveResult mdkr_save_validate(const MdkrSaveDocument *doc,
                                  MdkrSaveReport *out) {
    unsigned i;
    if (doc == NULL || out == NULL) {
        return MDKR_SAVE_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    for (i = 0; i < MDKR_SAVE_BLOCK_COUNT; i++) {
        out->block_status[i] = doc->block_status[i];
        if (doc->block_status[i] == MDKR_SAVE_BLOCK_CORRUPT) {
            out->corrupt_block_count++;
        } else if (doc->block_status[i] == MDKR_SAVE_BLOCK_NONCANONICAL) {
            out->noncanonical_block_count++;
        }
    }
    return MDKR_SAVE_OK;
}

static int slot_patch_is_valid(const MdkrSaveSlotPatch *patch) {
    unsigned i;
    if ((patch->fields & MDKR_SLOT_PATCH_ERASE) != 0) {
        return patch->fields == MDKR_SLOT_PATCH_ERASE &&
               patch->course_mask == 0 && patch->balloon_mask == 0 &&
               patch->world_flags_mask == 0;
    }
    if ((patch->fields & MDKR_SLOT_PATCH_CREATE) != 0 &&
        (patch->fields & MDKR_SLOT_PATCH_ERASE) != 0) {
        return 0;
    }
    if ((patch->course_mask >> MDKR_SAVE_COURSE_COUNT) != 0 ||
        (patch->balloon_mask >> MDKR_SAVE_WORLD_COUNT) != 0 ||
        (patch->world_flags_mask >> MDKR_SAVE_WORLD_COUNT) != 0 ||
        patch->values.taj_flags > 0x3F ||
        patch->values.trophies > 0x3FF ||
        patch->values.bosses > 0xFFF ||
        patch->values.tt_amulet > 4 ||
        patch->values.wizpig_amulet > 4 ||
        ((patch->fields & MDKR_SLOT_PATCH_FILENAME) != 0 &&
         patch->values.filename > 0x7FFF)) {
        return 0;
    }
    for (i = 0; i < MDKR_SAVE_COURSE_COUNT; i++) {
        if (((patch->course_mask >> i) & 1u) &&
            patch->values.course_status[i] > 3) {
            return 0;
        }
    }
    for (i = 0; i < MDKR_SAVE_WORLD_COUNT; i++) {
        if (((patch->balloon_mask >> i) & 1u) &&
            patch->values.balloons[i] > 0x7F) {
            return 0;
        }
    }
    return 1;
}

void mdkr_save_filename_decode(uint16_t encoded, char output[4]) {
    static const char characters[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ.?    ";
    int i;
    if (output == NULL) {
        return;
    }
    output[3] = '\0';
    for (i = 2; i >= 0; i--) {
        output[i] = characters[encoded & 31u];
        encoded >>= 5;
    }
}

MdkrSaveResult mdkr_save_filename_encode(const char *name, uint16_t *out) {
    static const char characters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.? ";
    uint16_t encoded = 0;
    unsigned i;
    if (name == NULL || out == NULL) {
        return MDKR_SAVE_ERR_ARGUMENT;
    }
    for (i = 0; i < 3; i++) {
        const char *found;
        char value = name[i];
        if (value == '\0') {
            value = ' ';
        }
        found = strchr(characters, value);
        if (found == NULL) {
            return MDKR_SAVE_ERR_PATCH;
        }
        encoded = (uint16_t) (encoded * 32u +
                              (uint16_t) (found - characters));
        if (name[i] == '\0') {
            while (++i < 3) {
                encoded = (uint16_t) (encoded * 32u + 28u);
            }
            break;
        }
    }
    if (name[0] != '\0' && name[1] != '\0' && name[2] != '\0' &&
        name[3] != '\0') {
        return MDKR_SAVE_ERR_PATCH;
    }
    *out = encoded;
    return MDKR_SAVE_OK;
}

static void write_slot(uint8_t *bytes, const MdkrSaveSlot *slot) {
    BitCursor cursor;
    unsigned i;
    cursor.bytes = bytes;
    cursor.bit = SLOT_CHECKSUM_BITS;
    for (i = 0; i < MDKR_SAVE_COURSE_COUNT; i++) {
        bit_write(&cursor, 2, slot->course_status[i]);
    }
    bit_write(&cursor, SLOT_TAJ_BITS, slot->taj_flags);
    bit_write(&cursor, SLOT_TROPHY_BITS, slot->trophies);
    bit_write(&cursor, SLOT_BOSS_BITS, slot->bosses);
    for (i = 0; i < MDKR_SAVE_WORLD_COUNT; i++) {
        bit_write(&cursor, SLOT_BALLOON_BITS, slot->balloons[i]);
    }
    bit_write(&cursor, SLOT_AMULET_BITS, slot->tt_amulet);
    bit_write(&cursor, SLOT_AMULET_BITS, slot->wizpig_amulet);
    for (i = 0; i < MDKR_SAVE_WORLD_COUNT; i++) {
        bit_write(&cursor, SLOT_WORLD_FLAGS_BITS, slot->world_flags[i]);
    }
    bit_write(&cursor, SLOT_KEYS_BITS, slot->keys);
    bit_write(&cursor, SLOT_CUTSCENE_BITS, slot->cutscene_flags);
    bit_write(&cursor, SLOT_FILENAME_BITS, slot->filename);
    bit_write(&cursor, SLOT_RESERVED_BITS, slot->reserved);
    store_be16(bytes, mdkr_save_sum_checksum(bytes, MDKR_SAVE_SLOT_SIZE));
}

static void apply_slot_patch(MdkrSaveSlot *slot,
                             const MdkrSaveSlotPatch *patch) {
    unsigned i;
    if (patch->fields & MDKR_SLOT_PATCH_CREATE) {
        memset(slot, 0, sizeof(*slot));
    }
    for (i = 0; i < MDKR_SAVE_COURSE_COUNT; i++) {
        if ((patch->course_mask >> i) & 1u) {
            slot->course_status[i] = patch->values.course_status[i];
        }
    }
    for (i = 0; i < MDKR_SAVE_WORLD_COUNT; i++) {
        if ((patch->balloon_mask >> i) & 1u) {
            slot->balloons[i] = patch->values.balloons[i];
        }
        if ((patch->world_flags_mask >> i) & 1u) {
            slot->world_flags[i] = patch->values.world_flags[i];
        }
    }
    if (patch->fields & MDKR_SLOT_PATCH_TAJ_FLAGS) {
        slot->taj_flags = patch->values.taj_flags;
    }
    if (patch->fields & MDKR_SLOT_PATCH_TROPHIES) {
        slot->trophies = patch->values.trophies;
    }
    if (patch->fields & MDKR_SLOT_PATCH_BOSSES) {
        slot->bosses = patch->values.bosses;
    }
    if (patch->fields & MDKR_SLOT_PATCH_TT_AMULET) {
        slot->tt_amulet = patch->values.tt_amulet;
    }
    if (patch->fields & MDKR_SLOT_PATCH_WIZPIG_AMULET) {
        slot->wizpig_amulet = patch->values.wizpig_amulet;
    }
    if (patch->fields & MDKR_SLOT_PATCH_KEYS) {
        slot->keys = patch->values.keys;
    }
    if (patch->fields & MDKR_SLOT_PATCH_CUTSCENES) {
        slot->cutscene_flags = patch->values.cutscene_flags;
    }
    if (patch->fields & MDKR_SLOT_PATCH_FILENAME) {
        slot->filename = patch->values.filename;
    }
}

static int config_patch_is_valid(const MdkrSaveConfigPatch *patch) {
    return patch->values.adventure_two_unlocked <= 1 &&
           patch->values.drumstick_unlocked <= 1 &&
           patch->values.language <= 3 &&
           patch->values.tt_course_flags <= 0xFFFFF &&
           patch->values.default_flag <= 1 &&
           patch->values.subtitles <= 1;
}

static void write_config(uint8_t *bytes, const MdkrSaveConfig *config) {
    uint64_t value = config->unknown_flags &
                     UINT64_C(0x00FFFFFFFC000000);
    value |= (uint64_t) config->adventure_two_unlocked;
    value |= (uint64_t) config->drumstick_unlocked << 1;
    value |= (uint64_t) config->language << 2;
    value |= (uint64_t) config->tt_course_flags << 4;
    value |= (uint64_t) config->default_flag << 24;
    value |= (uint64_t) config->subtitles << 25;
    value |= (uint64_t) mdkr_save_config_checksum(value) << 56;
    mdkr_save_config_encode_value(value, bytes);
}

static void apply_config_patch(MdkrSaveConfig *config,
                               const MdkrSaveConfigPatch *patch) {
    if (patch->fields & MDKR_CONFIG_PATCH_ADVENTURE_TWO) {
        config->adventure_two_unlocked =
            patch->values.adventure_two_unlocked;
    }
    if (patch->fields & MDKR_CONFIG_PATCH_DRUMSTICK) {
        config->drumstick_unlocked = patch->values.drumstick_unlocked;
    }
    if (patch->fields & MDKR_CONFIG_PATCH_LANGUAGE) {
        config->language = patch->values.language;
    }
    if (patch->fields & MDKR_CONFIG_PATCH_TT_COURSES) {
        config->tt_course_flags = patch->values.tt_course_flags;
    }
    if (patch->fields & MDKR_CONFIG_PATCH_DEFAULT_FLAG) {
        config->default_flag = patch->values.default_flag;
    }
    if (patch->fields & MDKR_CONFIG_PATCH_SUBTITLES) {
        config->subtitles = patch->values.subtitles;
    }
}

static void reset_record_block(uint8_t *bytes) {
    memset(bytes, 0, MDKR_SAVE_RECORD_BLOCK_SIZE);
    store_be16(bytes, 5);
}

MdkrSaveResult mdkr_save_apply(const MdkrSaveDocument *base,
                               const MdkrSavePatch *patch,
                               MdkrSaveDocument *out) {
    uint8_t bytes[MDKR_SAVE_IMAGE_SIZE];
    unsigned i;
    if (base == NULL || patch == NULL || out == NULL) {
        return MDKR_SAVE_ERR_ARGUMENT;
    }
    if ((patch->slot_mask >> MDKR_SAVE_SLOT_COUNT) != 0 ||
        (patch->reset_record_blocks & ~UINT8_C(0x03)) != 0 ||
        !config_patch_is_valid(&patch->config)) {
        return MDKR_SAVE_ERR_PATCH;
    }
    memcpy(bytes, base->bytes, sizeof(bytes));
    for (i = 0; i < MDKR_SAVE_SLOT_COUNT; i++) {
        MdkrSaveSlot slot;
        MdkrSaveSlotPatch const *slot_patch;
        uint8_t *slot_bytes;
        if (((patch->slot_mask >> i) & 1u) == 0) {
            continue;
        }
        slot_patch = &patch->slots[i];
        if (!slot_patch_is_valid(slot_patch)) {
            return MDKR_SAVE_ERR_PATCH;
        }
        slot_bytes = bytes + i * MDKR_SAVE_SLOT_SIZE;
        if (slot_patch->fields & MDKR_SLOT_PATCH_ERASE) {
            memset(slot_bytes, UINT8_C(0xFF), MDKR_SAVE_SLOT_SIZE);
            continue;
        }
        if (base->block_status[i] == MDKR_SAVE_BLOCK_CORRUPT &&
            (slot_patch->fields & MDKR_SLOT_PATCH_CREATE) == 0) {
            return MDKR_SAVE_ERR_CORRUPT_BASE;
        }
        slot = base->slots[i];
        apply_slot_patch(&slot, slot_patch);
        write_slot(slot_bytes, &slot);
    }
    if (patch->config.fields != 0) {
        MdkrSaveConfig config = base->config;
        if (base->block_status[MDKR_SAVE_BLOCK_CONFIG] ==
            MDKR_SAVE_BLOCK_CORRUPT) {
            return MDKR_SAVE_ERR_CORRUPT_BASE;
        }
        if (base->block_status[MDKR_SAVE_BLOCK_CONFIG] ==
            MDKR_SAVE_BLOCK_EMPTY) {
            memset(&config, 0, sizeof(config));
            config.default_flag = 1;
            config.subtitles = 1;
        }
        apply_config_patch(&config, &patch->config);
        write_config(bytes + CONFIG_OFFSET, &config);
    }
    if (patch->reset_record_blocks & 1u) {
        reset_record_block(bytes + FAST_LAPS_OFFSET);
    }
    if (patch->reset_record_blocks & 2u) {
        reset_record_block(bytes + COURSE_TIMES_OFFSET);
    }
    return mdkr_save_decode(bytes, sizeof(bytes), out);
}

MdkrSaveResult mdkr_save_encode(const MdkrSaveDocument *doc, uint8_t *bytes,
                                size_t byte_capacity) {
    if (doc == NULL || bytes == NULL) {
        return MDKR_SAVE_ERR_ARGUMENT;
    }
    if (byte_capacity < MDKR_SAVE_IMAGE_SIZE) {
        return MDKR_SAVE_ERR_SIZE;
    }
    memcpy(bytes, doc->bytes, MDKR_SAVE_IMAGE_SIZE);
    return MDKR_SAVE_OK;
}

MdkrSaveResult mdkr_save_recover(const uint8_t *bytes, size_t byte_count,
                                 const MdkrRecoveryPlan *plan,
                                 MdkrSaveDocument *out) {
    MdkrSaveDocument base;
    MdkrSavePatch patch;
    unsigned i;
    MdkrSaveResult result;
    if (bytes == NULL || plan == NULL || out == NULL) {
        return MDKR_SAVE_ERR_ARGUMENT;
    }
    result = mdkr_save_decode(bytes, byte_count, &base);
    if (result != MDKR_SAVE_OK) {
        return result;
    }
    memset(&patch, 0, sizeof(patch));
    for (i = 0; i < MDKR_SAVE_SLOT_COUNT; i++) {
        if (plan->block_action[i] == MDKR_SAVE_RECOVER_RESET) {
            patch.slot_mask |= (uint8_t) (1u << i);
            patch.slots[i].fields = MDKR_SLOT_PATCH_ERASE;
        } else if (base.block_status[i] == MDKR_SAVE_BLOCK_CORRUPT) {
            return MDKR_SAVE_ERR_CORRUPT_BASE;
        }
    }
    if (plan->block_action[MDKR_SAVE_BLOCK_CONFIG] ==
        MDKR_SAVE_RECOVER_RESET) {
        MdkrSaveConfig config;
        memset(&config, 0, sizeof(config));
        config.default_flag = 1;
        config.subtitles = 1;
        write_config(base.bytes + CONFIG_OFFSET, &config);
        decode_config(base.bytes + CONFIG_OFFSET, &base.config);
        base.block_status[MDKR_SAVE_BLOCK_CONFIG] = MDKR_SAVE_BLOCK_VALID;
    } else if (base.block_status[MDKR_SAVE_BLOCK_CONFIG] ==
               MDKR_SAVE_BLOCK_CORRUPT) {
        return MDKR_SAVE_ERR_CORRUPT_BASE;
    }
    if (plan->block_action[MDKR_SAVE_BLOCK_FAST_LAPS] ==
        MDKR_SAVE_RECOVER_RESET) {
        patch.reset_record_blocks |= 1;
    } else if (base.block_status[MDKR_SAVE_BLOCK_FAST_LAPS] ==
               MDKR_SAVE_BLOCK_CORRUPT) {
        return MDKR_SAVE_ERR_CORRUPT_BASE;
    }
    if (plan->block_action[MDKR_SAVE_BLOCK_COURSE_TIMES] ==
        MDKR_SAVE_RECOVER_RESET) {
        patch.reset_record_blocks |= 2;
    } else if (base.block_status[MDKR_SAVE_BLOCK_COURSE_TIMES] ==
               MDKR_SAVE_BLOCK_CORRUPT) {
        return MDKR_SAVE_ERR_CORRUPT_BASE;
    }
    return mdkr_save_apply(&base, &patch, out);
}
