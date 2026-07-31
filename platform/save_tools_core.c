#include "save_tools_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sha256.h"

typedef struct SummaryWriter {
    char *output;
    size_t capacity;
    size_t length;
    int failed;
} SummaryWriter;

static const char *block_status_name(MdkrSaveBlockStatus status) {
    switch (status) {
        case MDKR_SAVE_BLOCK_EMPTY: return "empty";
        case MDKR_SAVE_BLOCK_VALID: return "valid";
        case MDKR_SAVE_BLOCK_NONCANONICAL: return "noncanonical";
        case MDKR_SAVE_BLOCK_CORRUPT: return "corrupt";
        default: return "unknown";
    }
}

static void summary_bytes(SummaryWriter *writer, const char *bytes,
                          size_t count) {
    if (writer->failed) return;
    if (writer->output != NULL) {
        if (writer->length > writer->capacity ||
            count > writer->capacity - writer->length) {
            writer->failed = 1;
            return;
        }
        memcpy(writer->output + writer->length, bytes, count);
    }
    writer->length += count;
}

static void summary_text(SummaryWriter *writer, const char *text) {
    summary_bytes(writer, text, strlen(text));
}

static void summary_format(SummaryWriter *writer, const char *format, ...) {
    char buffer[512];
    va_list arguments;
    int count;
    va_start(arguments, format);
    count = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (count < 0 || (size_t) count >= sizeof(buffer)) {
        writer->failed = 1;
        return;
    }
    summary_bytes(writer, buffer, (size_t) count);
}

static unsigned record_count(const uint8_t *bytes) {
    unsigned count = 0;
    unsigned i;
    /* Two checksum bytes + two original padding bytes, then 47
     * (time, initials) big-endian u16 pairs. */
    for (i = 0; i < 47; i++) {
        const uint8_t *entry = bytes + 4 + i * 4u;
        if (entry[0] != 0 || entry[1] != 0) {
            count++;
        }
    }
    return count;
}

static void render_summary(SummaryWriter *writer,
                           const MdkrSaveDocument *document) {
    char digest[MDKR_SHA256_HEX_SIZE];
    unsigned i;
    mdkr_sha256_hex(document->bytes, sizeof(document->bytes), digest);
    summary_text(writer, "{\"format\":\"dkr-eeprom-4k-be-v1\",\"sha256\":\"");
    summary_text(writer, digest);
    summary_text(writer, "\",\"blocks\":[");
    for (i = 0; i < MDKR_SAVE_BLOCK_COUNT; i++) {
        if (i != 0) summary_text(writer, ",");
        summary_text(writer, "\"");
        summary_text(writer, block_status_name(document->block_status[i]));
        summary_text(writer, "\"");
    }
    summary_text(writer, "],\"slots\":[");
    for (i = 0; i < MDKR_SAVE_SLOT_COUNT; i++) {
        MdkrSaveSlot empty_slot;
        const MdkrSaveSlot *slot;
        char name[4];
        unsigned j;
        if (i != 0) summary_text(writer, ",");
        memset(&empty_slot, 0, sizeof(empty_slot));
        slot = document->block_status[i] == MDKR_SAVE_BLOCK_EMPTY
                   ? &empty_slot
                   : &document->slots[i];
        if (document->block_status[i] == MDKR_SAVE_BLOCK_EMPTY) {
            name[0] = '\0';
        } else {
            mdkr_save_filename_decode(slot->filename, name);
        }
        summary_text(writer, "{\"status\":\"");
        summary_text(writer, block_status_name(document->block_status[i]));
        summary_format(writer,
                       "\",\"name\":\"%.3s\",\"tajFlags\":%u,"
                       "\"trophies\":%u,\"bosses\":%u,\"ttAmulet\":%u,"
                       "\"wizpigAmulet\":%u,\"keys\":%u,\"cutscenes\":%u,"
                       "\"courses\":[",
                       name, slot->taj_flags, slot->trophies, slot->bosses,
                       slot->tt_amulet, slot->wizpig_amulet, slot->keys,
                       slot->cutscene_flags);
        for (j = 0; j < MDKR_SAVE_COURSE_COUNT; j++) {
            if (j != 0) summary_text(writer, ",");
            summary_format(writer, "%u", slot->course_status[j]);
        }
        summary_text(writer, "],\"balloons\":[");
        for (j = 0; j < MDKR_SAVE_WORLD_COUNT; j++) {
            if (j != 0) summary_text(writer, ",");
            summary_format(writer, "%u", slot->balloons[j]);
        }
        summary_text(writer, "],\"worldFlags\":[");
        for (j = 0; j < MDKR_SAVE_WORLD_COUNT; j++) {
            if (j != 0) summary_text(writer, ",");
            summary_format(writer, "%u", slot->world_flags[j]);
        }
        summary_text(writer, "]}");
    }
    {
        MdkrSaveConfig empty_config;
        const MdkrSaveConfig *config;
        memset(&empty_config, 0, sizeof(empty_config));
        config =
            document->block_status[MDKR_SAVE_BLOCK_CONFIG] ==
                    MDKR_SAVE_BLOCK_EMPTY
                ? &empty_config
                : &document->config;
        summary_format(
        writer,
        "],\"config\":{\"status\":\"%s\",\"adventureTwo\":%u,"
        "\"drumstick\":%u,\"language\":%u,\"ttCourses\":%u,"
        "\"defaultFlag\":%u,\"subtitles\":%u},",
        block_status_name(document->block_status[MDKR_SAVE_BLOCK_CONFIG]),
        config->adventure_two_unlocked, config->drumstick_unlocked,
        config->language, config->tt_course_flags, config->default_flag,
        config->subtitles);
    }
    summary_format(
        writer,
        "\"records\":{\"fastLaps\":{\"status\":\"%s\",\"count\":%u},"
        "\"courseTimes\":{\"status\":\"%s\",\"count\":%u}}}",
        block_status_name(document->block_status[MDKR_SAVE_BLOCK_FAST_LAPS]),
        document->block_status[MDKR_SAVE_BLOCK_FAST_LAPS] ==
                MDKR_SAVE_BLOCK_VALID
            ? record_count(document->bytes + 128)
            : 0,
        block_status_name(document->block_status[MDKR_SAVE_BLOCK_COURSE_TIMES]),
        document->block_status[MDKR_SAVE_BLOCK_COURSE_TIMES] ==
                MDKR_SAVE_BLOCK_VALID
            ? record_count(document->bytes + 320)
            : 0);
}

MdkrSaveToolResult mdkr_save_summary_json(
    const uint8_t *bytes, size_t byte_count, char *output,
    size_t output_capacity, size_t *output_size, size_t *required_capacity) {
    MdkrSaveDocument document;
    SummaryWriter writer;
    size_t required;
    if (bytes == NULL) {
        return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    }
    if (mdkr_save_decode(bytes, byte_count, &document) != MDKR_SAVE_OK) {
        return MDKR_SAVE_TOOL_ERR_CODEC;
    }
    writer.output = NULL;
    writer.capacity = 0;
    writer.length = 0;
    writer.failed = 0;
    render_summary(&writer, &document);
    if (writer.failed) return MDKR_SAVE_TOOL_ERR_CODEC;
    required = writer.length + 1;
    if (output_size != NULL) *output_size = writer.length;
    if (required_capacity != NULL) *required_capacity = required;
    if (output == NULL) return MDKR_SAVE_TOOL_OK;
    if (output_capacity < required) {
        return MDKR_SAVE_TOOL_ERR_CAPACITY;
    }
    writer.output = output;
    writer.capacity = output_capacity - 1;
    writer.length = 0;
    writer.failed = 0;
    render_summary(&writer, &document);
    if (writer.failed) return MDKR_SAVE_TOOL_ERR_CAPACITY;
    output[writer.length] = '\0';
    return MDKR_SAVE_TOOL_OK;
}

static MdkrSaveToolResult apply_patch(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], const MdkrSavePatch *patch,
    uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrSaveDocument document;
    MdkrSaveDocument changed;
    MdkrSaveResult result;
    if (base == NULL || patch == NULL || output == NULL) {
        return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    }
    result = mdkr_save_decode(base, MDKR_SAVE_IMAGE_SIZE, &document);
    if (result != MDKR_SAVE_OK) return MDKR_SAVE_TOOL_ERR_CODEC;
    result = mdkr_save_apply(&document, patch, &changed);
    if (result == MDKR_SAVE_ERR_CORRUPT_BASE) {
        return MDKR_SAVE_TOOL_ERR_CORRUPT;
    }
    if (result != MDKR_SAVE_OK) return MDKR_SAVE_TOOL_ERR_FIELD;
    memcpy(output, changed.bytes, MDKR_SAVE_IMAGE_SIZE);
    return MDKR_SAVE_TOOL_OK;
}

static int valid_slot(unsigned slot) {
    return slot < MDKR_SAVE_SLOT_COUNT;
}

MdkrSaveToolResult mdkr_save_edit_slot_state(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot, int create,
    int erase, uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrSavePatch patch;
    if (!valid_slot(slot) || (create != 0 && erase != 0) ||
        (create == 0 && erase == 0)) {
        return MDKR_SAVE_TOOL_ERR_FIELD;
    }
    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = (uint8_t) (1u << slot);
    patch.slots[slot].fields =
        erase ? MDKR_SLOT_PATCH_ERASE : MDKR_SLOT_PATCH_CREATE;
    return apply_patch(base, &patch, output);
}

MdkrSaveToolResult mdkr_save_edit_slot_name(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot, const char *name,
    uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrSavePatch patch;
    uint16_t encoded;
    if (!valid_slot(slot) ||
        mdkr_save_filename_encode(name, &encoded) != MDKR_SAVE_OK) {
        return MDKR_SAVE_TOOL_ERR_FIELD;
    }
    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = (uint8_t) (1u << slot);
    patch.slots[slot].fields = MDKR_SLOT_PATCH_FILENAME;
    patch.slots[slot].values.filename = encoded;
    return apply_patch(base, &patch, output);
}

MdkrSaveToolResult mdkr_save_edit_slot_field(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot,
    MdkrSaveSlotField field, uint32_t value,
    uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrSavePatch patch;
    if (!valid_slot(slot)) return MDKR_SAVE_TOOL_ERR_FIELD;
    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = (uint8_t) (1u << slot);
    switch (field) {
        case MDKR_SAVE_SLOT_FIELD_TAJ_FLAGS:
            if (value > 0x3F ||
                ((value >> 3) & 0x07u & ~value) != 0) {
                return MDKR_SAVE_TOOL_ERR_FIELD;
            }
            patch.slots[slot].fields = MDKR_SLOT_PATCH_TAJ_FLAGS;
            patch.slots[slot].values.taj_flags = (uint8_t) value;
            break;
        case MDKR_SAVE_SLOT_FIELD_TROPHIES:
            if (value > 0x3FF) return MDKR_SAVE_TOOL_ERR_FIELD;
            patch.slots[slot].fields = MDKR_SLOT_PATCH_TROPHIES;
            patch.slots[slot].values.trophies = (uint16_t) value;
            break;
        case MDKR_SAVE_SLOT_FIELD_BOSSES:
            if (value > 0xFFF) return MDKR_SAVE_TOOL_ERR_FIELD;
            patch.slots[slot].fields = MDKR_SLOT_PATCH_BOSSES;
            patch.slots[slot].values.bosses = (uint16_t) value;
            break;
        case MDKR_SAVE_SLOT_FIELD_TT_AMULET:
            if (value > 4) return MDKR_SAVE_TOOL_ERR_FIELD;
            patch.slots[slot].fields = MDKR_SLOT_PATCH_TT_AMULET;
            patch.slots[slot].values.tt_amulet = (uint8_t) value;
            break;
        case MDKR_SAVE_SLOT_FIELD_WIZPIG_AMULET:
            if (value > 4) return MDKR_SAVE_TOOL_ERR_FIELD;
            patch.slots[slot].fields = MDKR_SLOT_PATCH_WIZPIG_AMULET;
            patch.slots[slot].values.wizpig_amulet = (uint8_t) value;
            break;
        case MDKR_SAVE_SLOT_FIELD_KEYS:
            if (value > 0xFF) return MDKR_SAVE_TOOL_ERR_FIELD;
            patch.slots[slot].fields = MDKR_SLOT_PATCH_KEYS;
            patch.slots[slot].values.keys = (uint8_t) value;
            break;
        case MDKR_SAVE_SLOT_FIELD_CUTSCENES:
            patch.slots[slot].fields = MDKR_SLOT_PATCH_CUTSCENES;
            patch.slots[slot].values.cutscene_flags = value;
            break;
        default: return MDKR_SAVE_TOOL_ERR_FIELD;
    }
    return apply_patch(base, &patch, output);
}

MdkrSaveToolResult mdkr_save_edit_course(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot, unsigned course,
    unsigned status, uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrSavePatch patch;
    if (!valid_slot(slot) || course >= MDKR_SAVE_COURSE_COUNT || status > 3) {
        return MDKR_SAVE_TOOL_ERR_FIELD;
    }
    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = (uint8_t) (1u << slot);
    patch.slots[slot].course_mask = UINT64_C(1) << course;
    patch.slots[slot].values.course_status[course] = (uint8_t) status;
    return apply_patch(base, &patch, output);
}

MdkrSaveToolResult mdkr_save_edit_balloon(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot, unsigned world,
    unsigned count, uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrSavePatch patch;
    if (!valid_slot(slot) || world >= MDKR_SAVE_WORLD_COUNT || count > 127) {
        return MDKR_SAVE_TOOL_ERR_FIELD;
    }
    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = (uint8_t) (1u << slot);
    patch.slots[slot].balloon_mask = (uint8_t) (1u << world);
    patch.slots[slot].values.balloons[world] = (uint8_t) count;
    return apply_patch(base, &patch, output);
}

MdkrSaveToolResult mdkr_save_edit_world_flags(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned slot, unsigned world,
    unsigned flags, uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrSavePatch patch;
    if (!valid_slot(slot) || world >= MDKR_SAVE_WORLD_COUNT || flags > 0xFFFF) {
        return MDKR_SAVE_TOOL_ERR_FIELD;
    }
    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = (uint8_t) (1u << slot);
    patch.slots[slot].world_flags_mask = (uint8_t) (1u << world);
    patch.slots[slot].values.world_flags[world] = (uint16_t) flags;
    return apply_patch(base, &patch, output);
}

MdkrSaveToolResult mdkr_save_edit_config(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], MdkrSaveConfigField field,
    uint32_t value, uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrSavePatch patch;
    memset(&patch, 0, sizeof(patch));
    switch (field) {
        case MDKR_SAVE_CONFIG_FIELD_ADVENTURE_TWO:
            if (value > 1) return MDKR_SAVE_TOOL_ERR_FIELD;
            patch.config.fields = MDKR_CONFIG_PATCH_ADVENTURE_TWO;
            patch.config.values.adventure_two_unlocked = (uint8_t) value;
            break;
        case MDKR_SAVE_CONFIG_FIELD_DRUMSTICK:
            if (value > 1) return MDKR_SAVE_TOOL_ERR_FIELD;
            patch.config.fields = MDKR_CONFIG_PATCH_DRUMSTICK;
            patch.config.values.drumstick_unlocked = (uint8_t) value;
            break;
        case MDKR_SAVE_CONFIG_FIELD_LANGUAGE:
            if (value > 3) return MDKR_SAVE_TOOL_ERR_FIELD;
            patch.config.fields = MDKR_CONFIG_PATCH_LANGUAGE;
            patch.config.values.language = (uint8_t) value;
            break;
        case MDKR_SAVE_CONFIG_FIELD_TT_COURSES:
            if (value > 0xFFFFF) return MDKR_SAVE_TOOL_ERR_FIELD;
            patch.config.fields = MDKR_CONFIG_PATCH_TT_COURSES;
            patch.config.values.tt_course_flags = value;
            break;
        case MDKR_SAVE_CONFIG_FIELD_DEFAULT:
            if (value > 1) return MDKR_SAVE_TOOL_ERR_FIELD;
            patch.config.fields = MDKR_CONFIG_PATCH_DEFAULT_FLAG;
            patch.config.values.default_flag = (uint8_t) value;
            break;
        case MDKR_SAVE_CONFIG_FIELD_SUBTITLES:
            if (value > 1) return MDKR_SAVE_TOOL_ERR_FIELD;
            patch.config.fields = MDKR_CONFIG_PATCH_SUBTITLES;
            patch.config.values.subtitles = (uint8_t) value;
            break;
        default: return MDKR_SAVE_TOOL_ERR_FIELD;
    }
    return apply_patch(base, &patch, output);
}

MdkrSaveToolResult mdkr_save_reset_records(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned record_mask,
    uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrSavePatch patch;
    if ((record_mask & ~3u) != 0 || record_mask == 0) {
        return MDKR_SAVE_TOOL_ERR_FIELD;
    }
    memset(&patch, 0, sizeof(patch));
    patch.reset_record_blocks = (uint8_t) record_mask;
    return apply_patch(base, &patch, output);
}

MdkrSaveToolResult mdkr_save_recover_blocks(
    const uint8_t base[MDKR_SAVE_IMAGE_SIZE], unsigned block_mask,
    uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrRecoveryPlan plan;
    MdkrSaveDocument recovered;
    MdkrSaveResult result;
    unsigned i;
    if (base == NULL || output == NULL ||
        (block_mask >> MDKR_SAVE_BLOCK_COUNT) != 0) {
        return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    }
    memset(&plan, 0, sizeof(plan));
    for (i = 0; i < MDKR_SAVE_BLOCK_COUNT; i++) {
        if ((block_mask >> i) & 1u) {
            plan.block_action[i] = MDKR_SAVE_RECOVER_RESET;
        }
    }
    result = mdkr_save_recover(base, MDKR_SAVE_IMAGE_SIZE, &plan, &recovered);
    if (result == MDKR_SAVE_ERR_CORRUPT_BASE) {
        return MDKR_SAVE_TOOL_ERR_CORRUPT;
    }
    if (result != MDKR_SAVE_OK) return MDKR_SAVE_TOOL_ERR_CODEC;
    memcpy(output, recovered.bytes, MDKR_SAVE_IMAGE_SIZE);
    return MDKR_SAVE_TOOL_OK;
}

typedef struct SaveBlockRange {
    uint16_t offset;
    uint16_t size;
} SaveBlockRange;

static const SaveBlockRange s_block_ranges[MDKR_SAVE_BLOCK_COUNT] = {
    [MDKR_SAVE_BLOCK_SLOT_1] = { 0, MDKR_SAVE_SLOT_SIZE },
    [MDKR_SAVE_BLOCK_SLOT_2] = { MDKR_SAVE_SLOT_SIZE, MDKR_SAVE_SLOT_SIZE },
    [MDKR_SAVE_BLOCK_SLOT_3] = { 2 * MDKR_SAVE_SLOT_SIZE, MDKR_SAVE_SLOT_SIZE },
    [MDKR_SAVE_BLOCK_CONFIG] = { 3 * MDKR_SAVE_SLOT_SIZE, 8 },
    [MDKR_SAVE_BLOCK_FAST_LAPS] = {
        3 * MDKR_SAVE_SLOT_SIZE + 8, MDKR_SAVE_RECORD_BLOCK_SIZE
    },
    [MDKR_SAVE_BLOCK_COURSE_TIMES] = {
        3 * MDKR_SAVE_SLOT_SIZE + 8 + MDKR_SAVE_RECORD_BLOCK_SIZE,
        MDKR_SAVE_RECORD_BLOCK_SIZE
    },
};

MdkrSaveToolResult mdkr_save_copy_blocks(
    const uint8_t destination[MDKR_SAVE_IMAGE_SIZE],
    const uint8_t source[MDKR_SAVE_IMAGE_SIZE], unsigned block_mask,
    uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrSaveDocument destination_document;
    MdkrSaveDocument source_document;
    MdkrSaveDocument merged_document;
    uint8_t merged[MDKR_SAVE_IMAGE_SIZE];
    unsigned block;
    const unsigned valid_mask = (1u << MDKR_SAVE_BLOCK_COUNT) - 1u;

    if (destination == NULL || source == NULL || output == NULL) {
        return MDKR_SAVE_TOOL_ERR_ARGUMENT;
    }
    if ((block_mask & ~valid_mask) != 0 || block_mask == 0) {
        return MDKR_SAVE_TOOL_ERR_FIELD;
    }
    if (mdkr_save_decode(destination, MDKR_SAVE_IMAGE_SIZE,
                         &destination_document) != MDKR_SAVE_OK ||
        mdkr_save_decode(source, MDKR_SAVE_IMAGE_SIZE,
                         &source_document) != MDKR_SAVE_OK) {
        return MDKR_SAVE_TOOL_ERR_CODEC;
    }

    /*
     * A merge copies complete, independently checksummed EEPROM blocks. It
     * never "repairs" the source implicitly: corrupt source blocks must first
     * go through the explicit recovery preview. Corrupt unselected blocks in
     * either image are preserved verbatim, which is important for forensic
     * recovery of a different slot.
     */
    for (block = 0; block < MDKR_SAVE_BLOCK_COUNT; block++) {
        if ((block_mask & (1u << block)) == 0) {
            continue;
        }
        if (source_document.block_status[block] == MDKR_SAVE_BLOCK_CORRUPT) {
            return MDKR_SAVE_TOOL_ERR_CORRUPT;
        }
    }

    /*
     * Populate from the destination only after every validation above has
     * succeeded. This keeps output byte-for-byte untouched on every failure,
     * including output aliasing either input.
     */
    memcpy(merged, destination, sizeof(merged));
    for (block = 0; block < MDKR_SAVE_BLOCK_COUNT; block++) {
        const SaveBlockRange *range;
        if ((block_mask & (1u << block)) == 0) {
            continue;
        }
        range = &s_block_ranges[block];
        memcpy(merged + range->offset, source + range->offset, range->size);
    }
    if (mdkr_save_decode(merged, sizeof(merged), &merged_document) !=
        MDKR_SAVE_OK) {
        return MDKR_SAVE_TOOL_ERR_CODEC;
    }
    memcpy(output, merged, sizeof(merged));
    return MDKR_SAVE_TOOL_OK;
}

const char *mdkr_save_tool_result_string(MdkrSaveToolResult result) {
    switch (result) {
        case MDKR_SAVE_TOOL_OK: return "ok";
        case MDKR_SAVE_TOOL_ERR_ARGUMENT: return "invalid argument";
        case MDKR_SAVE_TOOL_ERR_CODEC: return "save codec rejected the image";
        case MDKR_SAVE_TOOL_ERR_CAPACITY: return "output buffer too small";
        case MDKR_SAVE_TOOL_ERR_FIELD: return "invalid edit";
        case MDKR_SAVE_TOOL_ERR_CORRUPT:
            return "the affected block must be reset before editing";
        default: return "unknown save-tool error";
    }
}
