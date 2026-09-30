/*
 * A save written by the published 1.7.0 build must still mean the same thing
 * to this tree's save tooling, byte for byte.
 *
 * tests/fixtures/save_v170/ holds the EEPROM the v1.7.0 binary wrote after one
 * Adventure door and one race from a new game, twice over: as annotated hex
 * (eeprom.hex) and as the .mdkr-save container 1.7.0's own `mdkr-save export`
 * produced from it (eeprom.mdkr-save). README.md there records how both were
 * made. This test holds platform/save_codec.c and platform/save_container.c to
 * them:
 *
 *   1. The decode reports the progress 1.7.0 reported for the same bytes
 *      (inspect-1.7.0.json is 1.7.0's `mdkr-save inspect` of them). A layout
 *      change that moves or resizes a field in BOTH the reader and the writer
 *      still round-trips its own output, so only these facts can see it.
 *   2. Re-encoding the decoded fields reproduces the image byte for byte, both
 *      through a no-op slot patch and through a patch that rewrites every slot
 *      field with its decoded value.
 *   3. 1.7.0's container decodes to the same payload and metadata, and encoding
 *      that payload and metadata reproduces 1.7.0's file byte for byte.
 */
#include "save_codec.h"
#include "save_container.h"

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

enum { MAX_FILE = 64 * 1024 };

static char *read_file(const char *directory, const char *name, size_t *size_out) {
    char path[4096];
    FILE *file;
    char *bytes;
    size_t size;
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int) sizeof(path)) {
        return NULL;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "cannot open fixture %s\n", path);
        return NULL;
    }
    bytes = (char *) malloc(MAX_FILE + 1);
    size = bytes != NULL ? fread(bytes, 1, MAX_FILE + 1, file) : 0;
    fclose(file);
    if (bytes == NULL || size > MAX_FILE) {
        free(bytes);
        return NULL;
    }
    bytes[size] = '\0';
    *size_out = size;
    return bytes;
}

/* eeprom.hex: a `size N` line, then `OFFSET: BYTES` rows. Every byte must
 * appear exactly once, so a truncated or hand-edited fixture fails here
 * instead of decoding as zeros. */
static int parse_hex_fixture(const char *text, uint8_t *out, size_t capacity) {
    unsigned char seen[MDKR_SAVE_IMAGE_SIZE];
    size_t declared = 0, filled = 0, i;
    const char *line = text;
    memset(seen, 0, sizeof(seen));
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t length = end != NULL ? (size_t) (end - line) : strlen(line);
        char row[256];
        if (length >= sizeof(row)) return 0;
        memcpy(row, line, length);
        row[length] = '\0';
        line += length + (end != NULL ? 1 : 0);
        if (row[0] == '\0' || row[0] == '#') continue;
        if (strncmp(row, "size ", 5) == 0) {
            if (declared != 0) return 0;
            declared = strtoul(row + 5, NULL, 10);
            if (declared != capacity || declared > sizeof(seen)) return 0;
            continue;
        }
        if (declared == 0) return 0;
        {
            char *cursor;
            unsigned long offset = strtoul(row, &cursor, 16);
            if (cursor == row || *cursor != ':') return 0;
            cursor++;
            for (;;) {
                char *next;
                unsigned long value;
                while (*cursor == ' ') cursor++;
                if (*cursor == '\0') break;
                value = strtoul(cursor, &next, 16);
                if (next != cursor + 2 || value > 0xFF || offset >= declared ||
                    seen[offset]) {
                    return 0;
                }
                out[offset] = (uint8_t) value;
                seen[offset] = 1;
                offset++;
                filled++;
                cursor = next;
            }
        }
    }
    for (i = 0; i < declared; i++) {
        if (!seen[i]) return 0;
    }
    return declared != 0 && filled == declared;
}

static int load_eeprom(const char *directory, uint8_t image[MDKR_SAVE_IMAGE_SIZE]) {
    size_t size = 0;
    char *text = read_file(directory, "eeprom.hex", &size);
    int ok = text != NULL && parse_hex_fixture(text, image, MDKR_SAVE_IMAGE_SIZE);
    free(text);
    CHECK(ok);
    return ok;
}

/* The facts 1.7.0's `mdkr-save inspect` printed for these bytes
 * (inspect-1.7.0.json). Course index 3 is Ancient Lake in the ROM's save
 * ordering; tests/check_save_v170_compat.py proves that mapping against the
 * game's own level table. */
static void test_decode_reports_the_1_7_0_facts(const uint8_t image[MDKR_SAVE_IMAGE_SIZE]) {
    static const uint8_t balloons[MDKR_SAVE_WORLD_COUNT] = {2, 1, 0, 0, 0, 0};
    static const uint16_t world_flags[MDKR_SAVE_WORLD_COUNT] = {1027, 54, 0, 0, 0, 0};
    MdkrSaveDocument document;
    MdkrSaveReport report;
    const MdkrSaveSlot *slot = &document.slots[0];
    char name[4];
    unsigned i;

    CHECK(mdkr_save_decode(image, MDKR_SAVE_IMAGE_SIZE, &document) == MDKR_SAVE_OK);
    CHECK(document.block_status[MDKR_SAVE_BLOCK_SLOT_1] == MDKR_SAVE_BLOCK_VALID);
    CHECK(document.block_status[MDKR_SAVE_BLOCK_SLOT_2] == MDKR_SAVE_BLOCK_EMPTY);
    CHECK(document.block_status[MDKR_SAVE_BLOCK_SLOT_3] == MDKR_SAVE_BLOCK_EMPTY);
    CHECK(document.block_status[MDKR_SAVE_BLOCK_CONFIG] == MDKR_SAVE_BLOCK_EMPTY);
    CHECK(document.block_status[MDKR_SAVE_BLOCK_FAST_LAPS] == MDKR_SAVE_BLOCK_EMPTY);
    CHECK(document.block_status[MDKR_SAVE_BLOCK_COURSE_TIMES] == MDKR_SAVE_BLOCK_EMPTY);
    CHECK(mdkr_save_validate(&document, &report) == MDKR_SAVE_OK);
    CHECK(report.corrupt_block_count == 0 && report.noncanonical_block_count == 0);

    mdkr_save_filename_decode(slot->filename, name);
    CHECK(strcmp(name, "AAA") == 0);
    CHECK(slot->taj_flags == 0);
    CHECK(slot->trophies == 0);
    CHECK(slot->bosses == 0);
    CHECK(slot->tt_amulet == 0);
    CHECK(slot->wizpig_amulet == 0);
    CHECK(slot->keys == 0);
    CHECK(slot->cutscene_flags == 262144u);
    CHECK(slot->reserved == 0);
    for (i = 0; i < MDKR_SAVE_COURSE_COUNT; i++) {
        CHECK(slot->course_status[i] == (i == 3 ? 2 : 0));
    }
    for (i = 0; i < MDKR_SAVE_WORLD_COUNT; i++) {
        CHECK(slot->balloons[i] == balloons[i]);
        CHECK(slot->world_flags[i] == world_flags[i]);
    }
}

static void test_fields_reencode_byte_identically(const uint8_t image[MDKR_SAVE_IMAGE_SIZE]) {
    MdkrSaveDocument document;
    MdkrSaveDocument rewritten;
    MdkrSavePatch patch;
    uint8_t encoded[MDKR_SAVE_IMAGE_SIZE];

    CHECK(mdkr_save_decode(image, MDKR_SAVE_IMAGE_SIZE, &document) == MDKR_SAVE_OK);
    CHECK(mdkr_save_encode(&document, encoded, sizeof(encoded)) == MDKR_SAVE_OK);
    CHECK(memcmp(encoded, image, sizeof(encoded)) == 0);

    /* A selected slot with no field changes is re-serialised from its decoded
     * fields by write_slot(), checksum included. */
    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = 1;
    CHECK(mdkr_save_apply(&document, &patch, &rewritten) == MDKR_SAVE_OK);
    CHECK(memcmp(rewritten.bytes, image, MDKR_SAVE_IMAGE_SIZE) == 0);

    /* Every slot field written back with the value it decoded to. */
    memset(&patch, 0, sizeof(patch));
    patch.slot_mask = 1;
    patch.slots[0].fields = MDKR_SLOT_PATCH_TAJ_FLAGS | MDKR_SLOT_PATCH_TROPHIES |
                            MDKR_SLOT_PATCH_BOSSES | MDKR_SLOT_PATCH_TT_AMULET |
                            MDKR_SLOT_PATCH_WIZPIG_AMULET | MDKR_SLOT_PATCH_KEYS |
                            MDKR_SLOT_PATCH_CUTSCENES | MDKR_SLOT_PATCH_FILENAME;
    patch.slots[0].course_mask = (UINT64_C(1) << MDKR_SAVE_COURSE_COUNT) - 1u;
    patch.slots[0].balloon_mask = (uint8_t) ((1u << MDKR_SAVE_WORLD_COUNT) - 1u);
    patch.slots[0].world_flags_mask = (uint8_t) ((1u << MDKR_SAVE_WORLD_COUNT) - 1u);
    patch.slots[0].values = document.slots[0];
    CHECK(mdkr_save_apply(&document, &patch, &rewritten) == MDKR_SAVE_OK);
    CHECK(memcmp(rewritten.bytes, image, MDKR_SAVE_IMAGE_SIZE) == 0);
    CHECK(rewritten.block_status[MDKR_SAVE_BLOCK_SLOT_1] == MDKR_SAVE_BLOCK_VALID);
}

static void test_1_7_0_container_round_trips(const char *directory,
                                             const uint8_t image[MDKR_SAVE_IMAGE_SIZE]) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    MdkrSaveInputFormat format = MDKR_SAVE_INPUT_RAW;
    MdkrSaveContainerMetadata metadata;
    size_t size = 0, encoded_size = 0, required = 0;
    char *container = read_file(directory, "eeprom.mdkr-save", &size);
    char *encoded;

    CHECK(container != NULL);
    if (container == NULL) return;
    memset(payload, 0xA5, sizeof(payload));
    CHECK(mdkr_save_container_decode((const uint8_t *) container, size, payload, &format,
                                     &metadata) == MDKR_SAVE_CONTAINER_OK);
    CHECK(format == MDKR_SAVE_INPUT_CONTAINER);
    CHECK(memcmp(payload, image, sizeof(payload)) == 0);
    CHECK(strcmp(metadata.app_version, "1.7.0") == 0);
    CHECK(strcmp(metadata.source, "native") == 0);
    CHECK(strcmp(metadata.created_at, "2026-09-30T18:07:44Z") == 0);

    CHECK(mdkr_save_container_encode(payload, &metadata, NULL, 0, &encoded_size,
                                     &required) == MDKR_SAVE_CONTAINER_OK);
    encoded = (char *) malloc(required);
    CHECK(encoded != NULL);
    if (encoded != NULL) {
        CHECK(mdkr_save_container_encode(payload, &metadata, encoded, required,
                                         &encoded_size, NULL) == MDKR_SAVE_CONTAINER_OK);
        CHECK(encoded_size == size);
        CHECK(encoded_size == size && memcmp(encoded, container, size) == 0);
    }

    /* The raw image is accepted as itself by the same entry point. */
    memset(payload, 0xA5, sizeof(payload));
    CHECK(mdkr_save_container_decode(image, MDKR_SAVE_IMAGE_SIZE, payload, &format, NULL) ==
          MDKR_SAVE_CONTAINER_OK);
    CHECK(format == MDKR_SAVE_INPUT_RAW);
    CHECK(memcmp(payload, image, sizeof(payload)) == 0);
    free(encoded);
    free(container);
}

int main(int argc, char **argv) {
    uint8_t image[MDKR_SAVE_IMAGE_SIZE];
    if (argc != 2) {
        fprintf(stderr, "usage: %s tests/fixtures/save_v170\n", argv[0]);
        return 2;
    }
    memset(image, 0, sizeof(image));
    if (load_eeprom(argv[1], image)) {
        test_decode_reports_the_1_7_0_facts(image);
        test_fields_reencode_byte_identically(image);
        test_1_7_0_container_round_trips(argv[1], image);
    }
    if (failures != 0) {
        fprintf(stderr, "save v1.7.0 fixture: %d failure(s)\n", failures);
        return 1;
    }
    puts("save v1.7.0 fixture: decode facts, field re-encode and container re-encode match 1.7.0");
    return 0;
}
