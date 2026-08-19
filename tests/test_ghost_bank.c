/**
 * Unit test for platform/ghost_bank.{c,h} -- the per-(level, vehicle) Time
 * Trial ghost bank behind issue #46's "controller pak full after six ghosts".
 *
 * Three layers are exercised:
 *
 *   1. The pure GHSS window operations, against note images built here the
 *      exact way game/src/save_data.c's authored writers build them
 *      (func_80074EB8 creation, func_80075000 empty-slot append), so the
 *      module is proven against real authored bytes rather than its own
 *      output.
 *   2. The pure bank-record and index codecs: round-trip, digest rejection,
 *      bounds, and LRU victim selection.
 *   3. mdkr_ghost_bank_select() end to end against an in-memory fake of the
 *      five save_data.c pak helpers plus a real temporary bank directory:
 *      migration split, LRU eviction, byte-identical restore of an evicted
 *      pair, erase/note-deletion reconciliation, and failure no-ops.
 *
 * The fake pak stubs deliberately carry the real save_data.h signatures so a
 * drift between this test's model and the game's helpers is a compile error.
 */
#include "ghost_bank.h"

#include "save_data.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* ======================================================================== *
 *  Authored-shape note builder (mirrors save_data.c's writers)
 * ======================================================================== */

#define NOTE_BYTES MDKR_GHOST_WINDOW_BYTES
#define SLOT_BYTES MDKR_GHOST_WINDOW_SLOT_BYTES
#define DIR_BYTES MDKR_GHOST_WINDOW_DIRECTORY_BYTES

static void put16(uint8_t *at, int16_t value) {
    memcpy(at, &value, sizeof(value));
}

static int16_t get16(const uint8_t *at) {
    int16_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static void dir_set(uint8_t *note, int slot, int level, int vehicle,
                    int offset) {
    uint8_t *entry = note + 4 + (size_t)slot * 4;
    entry[0] = (uint8_t)level;
    entry[1] = (uint8_t)vehicle;
    put16(entry + 2, (int16_t)offset);
}

/* The checksum the authored writer computes in func_80074AA8: byte sum over
 * the record from its third byte. */
static int16_t authored_checksum(const uint8_t *record, size_t length) {
    uint16_t sum = 0;
    size_t i;
    for (i = 2; i < length; i++) {
        sum = (uint16_t)(sum + record[i]);
    }
    return (int16_t)sum;
}

/* Deterministic per-pair record bytes: GhostHeader + nodeCount nodes. */
static size_t make_record(uint8_t *record, int level, int vehicle,
                          int character, int16_t time, int16_t nodeCount) {
    size_t length = 8 + (size_t)nodeCount * 12;
    size_t i;
    memset(record, 0, SLOT_BYTES);
    record[2] = (uint8_t)character;
    record[3] = 0;
    put16(record + 4, time);
    put16(record + 6, nodeCount);
    for (i = 8; i < length; i++) {
        record[i] = (uint8_t)(level * 31 + vehicle * 7 + (int)i);
    }
    put16(record, authored_checksum(record, length));
    return length;
}

/* func_80074EB8: the very first save creates the whole 0x6700 note. */
static void note_create(uint8_t *note, int level, int vehicle, int character,
                        int16_t time, int16_t nodeCount) {
    int i;
    memset(note, 0, NOTE_BYTES);
    /* Store the GHSS signature the way the machine does: a host s32, exactly
     * as save_data.c's `ghost->signature = GHSS` lays it out in the pak. */
    {
        int32_t ghss = 0x47485353;
        memcpy(note, &ghss, sizeof(ghss));
    }
    dir_set(note, 0, level, vehicle, DIR_BYTES);
    for (i = 1; i < 7; i++) {
        dir_set(note, i, 0xFF, 0, DIR_BYTES + SLOT_BYTES);
    }
    make_record(note + DIR_BYTES, level, vehicle, character, time, nodeCount);
}

/* func_80075000's empty-slot arm: append into the first free slot. */
static int note_append(uint8_t *note, int level, int vehicle, int character,
                       int16_t time, int16_t nodeCount) {
    int slot = -1;
    int i;
    for (i = 0; i < 6; i++) {
        if (note[4 + i * 4] == 0xFF) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    dir_set(note, slot, level, vehicle, DIR_BYTES + slot * SLOT_BYTES);
    for (i = slot + 1; i < 7; i++) {
        dir_set(note, i, 0xFF, 0, DIR_BYTES + (slot + 1) * SLOT_BYTES);
    }
    make_record(note + DIR_BYTES + (size_t)slot * SLOT_BYTES, level, vehicle,
                character, time, nodeCount);
    return slot;
}

/* ======================================================================== *
 *  1. Pure window operations
 * ======================================================================== */

static void test_window_operations(void) {
    uint8_t note[NOTE_BYTES];
    uint8_t extracted[SLOT_BYTES];
    uint8_t original[SLOT_BYTES];
    size_t length = 0;
    int level = -1;
    int vehicle = -1;
    int i;

    note_create(note, 5, 0, 3, 5000, 200);
    CHECK(mdkr_ghost_window_validate(note, NOTE_BYTES) == MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_window_occupied(note, NOTE_BYTES) == 1);
    CHECK(mdkr_ghost_window_find(note, NOTE_BYTES, 5, 0) == 0);
    CHECK(mdkr_ghost_window_find(note, NOTE_BYTES, 99, 0) == -1);

    for (i = 1; i < 6; i++) {
        CHECK(note_append(note, 6 + i, i % 3, i, (int16_t)(5000 + i),
                          (int16_t)(100 + i)) == i);
    }
    CHECK(mdkr_ghost_window_validate(note, NOTE_BYTES) == MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_window_occupied(note, NOTE_BYTES) == 6);
    CHECK(mdkr_ghost_window_find(note, NOTE_BYTES, 8, 2) == 2);
    CHECK(mdkr_ghost_window_pair_at(note, NOTE_BYTES, 2, &level, &vehicle) ==
          MDKR_GHOST_BANK_OK);
    CHECK(level == 8 && vehicle == 2);

    /* Extraction returns the whole authored extent verbatim. */
    memcpy(original, note + DIR_BYTES + 2 * SLOT_BYTES, SLOT_BYTES);
    CHECK(mdkr_ghost_window_extract(note, NOTE_BYTES, 2, extracted,
                                    sizeof(extracted), &length) ==
          MDKR_GHOST_BANK_OK);
    CHECK(length == SLOT_BYTES);
    CHECK(memcmp(extracted, original, SLOT_BYTES) == 0);

    /* Removing a middle slot compacts to the authored prefix shape and does
     * not disturb any surviving record's bytes. */
    memcpy(original, note + DIR_BYTES + 4 * SLOT_BYTES, SLOT_BYTES);
    CHECK(mdkr_ghost_window_remove(note, NOTE_BYTES, 2) ==
          MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_window_validate(note, NOTE_BYTES) == MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_window_occupied(note, NOTE_BYTES) == 5);
    CHECK(mdkr_ghost_window_find(note, NOTE_BYTES, 8, 2) == -1);
    CHECK(mdkr_ghost_window_find(note, NOTE_BYTES, 10, 1) == 3);
    CHECK(mdkr_ghost_window_extract(note, NOTE_BYTES, 3, extracted,
                                    sizeof(extracted), &length) ==
          MDKR_GHOST_BANK_OK);
    CHECK(length == SLOT_BYTES);
    CHECK(memcmp(extracted, original, SLOT_BYTES) == 0);
    /* The freed tail entry is canonically empty. */
    CHECK(note[4 + 5 * 4] == 0xFF);
    CHECK(get16(note + 4 + 5 * 4 + 2) == (int16_t)(DIR_BYTES + 5 * SLOT_BYTES));
    CHECK(get16(note + 4 + 6 * 4 + 2) == (int16_t)(DIR_BYTES + 5 * SLOT_BYTES));

    /* Re-inserting the extracted pair restores the identical bytes. */
    memcpy(original, extracted, SLOT_BYTES);
    CHECK(mdkr_ghost_window_extract(note, NOTE_BYTES, 3, extracted,
                                    sizeof(extracted), &length) ==
          MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_window_remove(note, NOTE_BYTES, 3) ==
          MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_window_occupied(note, NOTE_BYTES) == 4);
    CHECK(mdkr_ghost_window_insert(note, NOTE_BYTES, 10, 1, extracted,
                                   length) == MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_window_find(note, NOTE_BYTES, 10, 1) == 4);
    CHECK(mdkr_ghost_window_extract(note, NOTE_BYTES, 4, extracted,
                                    sizeof(extracted), &length) ==
          MDKR_GHOST_BANK_OK);
    CHECK(memcmp(extracted, original, SLOT_BYTES) == 0);

    /* Fill back to six, then the seventh insert must report a full window. */
    CHECK(mdkr_ghost_window_insert(note, NOTE_BYTES, 8, 2, original,
                                   SLOT_BYTES) == MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_window_occupied(note, NOTE_BYTES) == 6);
    CHECK(mdkr_ghost_window_insert(note, NOTE_BYTES, 30, 0, original,
                                   SLOT_BYTES) == MDKR_GHOST_BANK_ERR_FULL);
    /* A pair may not be inserted twice. */
    CHECK(mdkr_ghost_window_remove(note, NOTE_BYTES, 5) ==
          MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_window_insert(note, NOTE_BYTES, 5, 0, original,
                                   SLOT_BYTES) != MDKR_GHOST_BANK_OK);

    /* Bounds and corruption. */
    CHECK(mdkr_ghost_window_remove(note, NOTE_BYTES, 9) !=
          MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_window_remove(note, NOTE_BYTES, 5) !=
          MDKR_GHOST_BANK_OK); /* slot 5 already empty */
    note[0] ^= 0x40;
    CHECK(mdkr_ghost_window_validate(note, NOTE_BYTES) ==
          MDKR_GHOST_BANK_ERR_FORMAT);
    note[0] ^= 0x40;
    CHECK(mdkr_ghost_window_validate(note, 0x80) ==
          MDKR_GHOST_BANK_ERR_FORMAT);
}

/* ======================================================================== *
 *  2. Record and index codecs, LRU victim
 * ======================================================================== */

static void test_record_codec(void) {
    uint8_t payload[SLOT_BYTES];
    uint8_t decoded[SLOT_BYTES];
    uint8_t image[MDKR_GHOST_BANK_RECORD_IMAGE_MAX];
    size_t image_size = 0;
    size_t length = 0;
    int level = -1;
    int vehicle = -1;
    size_t i;

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 131u + 7u);
    }
    CHECK(mdkr_ghost_bank_record_encode(12, 1, payload, SLOT_BYTES, image,
                                        sizeof(image), &image_size) ==
          MDKR_GHOST_BANK_OK);
    CHECK(image_size == MDKR_GHOST_BANK_RECORD_HEADER_BYTES + SLOT_BYTES);
    CHECK(mdkr_ghost_bank_record_decode(image, image_size, &level, &vehicle,
                                        decoded, sizeof(decoded), &length) ==
          MDKR_GHOST_BANK_OK);
    CHECK(level == 12 && vehicle == 1);
    CHECK(length == SLOT_BYTES);
    CHECK(memcmp(decoded, payload, SLOT_BYTES) == 0);

    /* One flipped payload bit must fail the digest. */
    image[image_size - 1] ^= 0x01;
    CHECK(mdkr_ghost_bank_record_decode(image, image_size, &level, &vehicle,
                                        decoded, sizeof(decoded), &length) ==
          MDKR_GHOST_BANK_ERR_DIGEST);
    image[image_size - 1] ^= 0x01;

    /* Wrong magic and truncated images are format errors. */
    image[0] ^= 0x20;
    CHECK(mdkr_ghost_bank_record_decode(image, image_size, &level, &vehicle,
                                        decoded, sizeof(decoded), &length) ==
          MDKR_GHOST_BANK_ERR_FORMAT);
    image[0] ^= 0x20;
    CHECK(mdkr_ghost_bank_record_decode(image, image_size - 1, &level,
                                        &vehicle, decoded, sizeof(decoded),
                                        &length) != MDKR_GHOST_BANK_OK);

    /* Payload length bounds: a record is at least a GhostHeader and at most
     * one slot extent. */
    CHECK(mdkr_ghost_bank_record_encode(12, 1, payload, 7, image,
                                        sizeof(image), &image_size) !=
          MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_bank_record_encode(12, 1, payload, SLOT_BYTES + 1, image,
                                        sizeof(image), &image_size) !=
          MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_bank_record_encode(0xFF, 1, payload, 64, image,
                                        sizeof(image), &image_size) !=
          MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_bank_record_encode(12, 3, payload, 64, image,
                                        sizeof(image), &image_size) !=
          MDKR_GHOST_BANK_OK);
}

static void test_index_codec_and_lru(void) {
    MdkrGhostBankIndex index;
    MdkrGhostBankIndex decoded;
    uint8_t image[MDKR_GHOST_BANK_INDEX_IMAGE_MAX];
    uint8_t note[NOTE_BYTES];
    size_t image_size = 0;
    int i;

    mdkr_ghost_bank_index_init(&index);
    CHECK(index.count == 0);
    CHECK(mdkr_ghost_bank_index_touch(&index, 5, 0, 1) == MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_bank_index_touch(&index, 7, 1, 1) == MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_bank_index_touch(&index, 9, 2, 0) == MDKR_GHOST_BANK_OK);
    CHECK(index.count == 3);
    CHECK(mdkr_ghost_bank_index_find(&index, 7, 1) >= 0);
    CHECK(mdkr_ghost_bank_index_find(&index, 7, 2) == -1);

    /* Ticks are strictly monotonic; re-touching refreshes recency. */
    {
        int five = mdkr_ghost_bank_index_find(&index, 5, 0);
        int seven = mdkr_ghost_bank_index_find(&index, 7, 1);
        CHECK(index.entries[five].tick < index.entries[seven].tick);
        CHECK(mdkr_ghost_bank_index_touch(&index, 5, 0, 1) ==
              MDKR_GHOST_BANK_OK);
        CHECK(index.entries[five].tick > index.entries[seven].tick);
    }

    CHECK(mdkr_ghost_bank_index_encode(&index, image, sizeof(image),
                                       &image_size) == MDKR_GHOST_BANK_OK);
    memset(&decoded, 0xA5, sizeof(decoded));
    CHECK(mdkr_ghost_bank_index_decode(image, image_size, &decoded) ==
          MDKR_GHOST_BANK_OK);
    CHECK(decoded.count == index.count);
    CHECK(decoded.tick_counter == index.tick_counter);
    for (i = 0; i < (int)index.count; i++) {
        CHECK(decoded.entries[i].level == index.entries[i].level);
        CHECK(decoded.entries[i].vehicle == index.entries[i].vehicle);
        CHECK(decoded.entries[i].in_window == index.entries[i].in_window);
        CHECK(decoded.entries[i].tick == index.entries[i].tick);
    }
    image[image_size - 1] ^= 0x80;
    CHECK(mdkr_ghost_bank_index_decode(image, image_size, &decoded) ==
          MDKR_GHOST_BANK_ERR_DIGEST);
    image[image_size - 1] ^= 0x80;

    CHECK(mdkr_ghost_bank_index_remove(&index, 7, 1) == MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_bank_index_find(&index, 7, 1) == -1);
    CHECK(index.count == 2);

    /* Victim selection: oldest tick among the window's occupied pairs wins;
     * a pair the index has never seen counts as oldest of all. */
    note_create(note, 1, 0, 0, 100, 10);
    note_append(note, 2, 0, 0, 100, 10);
    note_append(note, 3, 0, 0, 100, 10);
    mdkr_ghost_bank_index_init(&index);
    CHECK(mdkr_ghost_bank_index_touch(&index, 2, 0, 1) == MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_bank_index_touch(&index, 1, 0, 1) == MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_bank_index_touch(&index, 3, 0, 1) == MDKR_GHOST_BANK_OK);
    /* All known: slot 1 (pair 2:0) is oldest. */
    CHECK(mdkr_ghost_bank_pick_victim(note, NOTE_BYTES, &index) == 1);
    /* Pair 1:0 unknown to the index: it becomes the victim. */
    CHECK(mdkr_ghost_bank_index_remove(&index, 1, 0) == MDKR_GHOST_BANK_OK);
    CHECK(mdkr_ghost_bank_pick_victim(note, NOTE_BYTES, &index) == 0);
    /* An unusable window yields no victim. */
    CHECK(mdkr_ghost_bank_pick_victim(note, 0x40, &index) == -1);
}

/* ======================================================================== *
 *  3. mdkr_ghost_bank_select() against a fake pak + real bank directory
 * ======================================================================== */

static uint8_t s_note[NOTE_BYTES];
static int s_noteExists;
static int s_noteSize = NOTE_BYTES;
static SIDeviceStatus s_deviceStatus = CONTROLLER_PAK_GOOD;
static int s_noteWrites;
static int s_failNoteWrite;
static char s_root[1024];

SIDeviceStatus get_si_device_status(s32 controllerIndex) {
    (void)controllerIndex;
    return s_deviceStatus;
}

s32 start_reading_controller_data(s32 controllerIndex) {
    (void)controllerIndex;
    return 0;
}

SIDeviceStatus get_file_number(s32 controllerIndex, char *fileName,
                               char *fileExt, s32 *fileNumber) {
    (void)controllerIndex;
    (void)fileExt;
    if (strcmp(fileName, "DKRACING-GHOSTS") != 0) {
        return CONTROLLER_PAK_CHANGED;
    }
    if (!s_noteExists) {
        if (fileNumber != NULL) {
            *fileNumber = -1;
        }
        return CONTROLLER_PAK_CHANGED;
    }
    if (fileNumber != NULL) {
        *fileNumber = 0;
    }
    return CONTROLLER_PAK_GOOD;
}

SIDeviceStatus get_file_size(s32 controllerIndex, s32 fileNum, s32 *fileSize) {
    (void)controllerIndex;
    if (fileNum != 0 || !s_noteExists) {
        return CONTROLLER_PAK_BAD_DATA;
    }
    *fileSize = s_noteSize;
    return CONTROLLER_PAK_GOOD;
}

SIDeviceStatus read_data_from_controller_pak(s32 controllerIndex, s32 fileNum,
                                             u8 *data, s32 dataLength) {
    (void)controllerIndex;
    if (fileNum != 0 || !s_noteExists || dataLength < 0 ||
        dataLength > s_noteSize) {
        return CONTROLLER_PAK_BAD_DATA;
    }
    memcpy(data, s_note, (size_t)dataLength);
    return CONTROLLER_PAK_GOOD;
}

SIDeviceStatus write_controller_pak_file(s32 controllerIndex, s32 fileNumber,
                                         char *fileName, char *fileExt,
                                         u8 *dataToWrite, s32 fileSize) {
    (void)controllerIndex;
    (void)fileExt;
    if (s_failNoteWrite) {
        return CONTROLLER_PAK_NOT_FOUND;
    }
    if (strcmp(fileName, "DKRACING-GHOSTS") != 0 || fileSize != s_noteSize ||
        (fileNumber != -1 && fileNumber != 0) ||
        (fileNumber == -1 && s_noteExists) ||
        (fileNumber == 0 && !s_noteExists)) {
        return CONTROLLER_PAK_BAD_DATA;
    }
    memcpy(s_note, dataToWrite, (size_t)fileSize);
    s_noteExists = 1;
    s_noteWrites++;
    return CONTROLLER_PAK_GOOD;
}

bool mdkr_rollback_game_runtime_host_io_allowed(bool progression_write) {
    (void)progression_write;
    return true;
}

int mdkr_user_save_directory(char *output, size_t output_size) {
    /* select() is pointed at the temporary root via
     * mdkr_ghost_bank_set_root(); reaching this default resolution in the
     * test would mean the seam regressed. */
    (void)output;
    (void)output_size;
    return 0;
}

int mdkr_trace_enabled(void) {
    return 0;
}

void mdkr_trace(const char *fmt, ...) {
    (void)fmt;
}

static int bank_file_exists(int level, int vehicle) {
    char path[1200];
    FILE *file;
    snprintf(path, sizeof(path), "%s/controller-1-ghost-%d-%d.mdg", s_root,
             level, vehicle);
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

static int bank_file_payload(int level, int vehicle, uint8_t *payload,
                             size_t capacity, size_t *length) {
    char path[1200];
    uint8_t image[MDKR_GHOST_BANK_RECORD_IMAGE_MAX + 1];
    size_t got;
    int stored_level = -1;
    int stored_vehicle = -1;
    FILE *file;
    snprintf(path, sizeof(path), "%s/controller-1-ghost-%d-%d.mdg", s_root,
             level, vehicle);
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    got = fread(image, 1, sizeof(image), file);
    fclose(file);
    if (mdkr_ghost_bank_record_decode(image, got, &stored_level,
                                      &stored_vehicle, payload, capacity,
                                      length) != MDKR_GHOST_BANK_OK) {
        return 0;
    }
    return stored_level == level && stored_vehicle == vehicle;
}

static void reset_fake_pak(void) {
    memset(s_note, 0, sizeof(s_note));
    s_noteExists = 0;
    s_noteWrites = 0;
    s_failNoteWrite = 0;
    s_deviceStatus = CONTROLLER_PAK_GOOD;
}

static void fill_six_pairs(void) {
    int i;
    note_create(s_note, 1, 0, 0, 5001, 101);
    for (i = 1; i < 6; i++) {
        note_append(s_note, 1 + i, i % 3, i, (int16_t)(5001 + i),
                    (int16_t)(101 + i));
    }
    s_noteExists = 1;
}

static void test_select_migration_and_noop(void) {
    int i;
    reset_fake_pak();
    mdkr_ghost_bank_reset();
    fill_six_pairs();

    /* Selecting a pair already in the window is a strict pak no-op, and the
     * first contact splits every resident pair into its own bank file. */
    CHECK(mdkr_ghost_bank_select(0, 3, 2) == 0);
    CHECK(s_noteWrites == 0);
    for (i = 0; i < 6; i++) {
        CHECK(bank_file_exists(1 + i, i % 3));
    }
}

static void test_select_eviction_and_restore(void) {
    uint8_t original[SLOT_BYTES];
    uint8_t banked[SLOT_BYTES];
    uint8_t restored[SLOT_BYTES];
    size_t original_length = 0;
    size_t banked_length = 0;
    size_t restored_length = 0;
    int slot;

    reset_fake_pak();
    mdkr_ghost_bank_reset();
    fill_six_pairs();

    /* Make pair (1, 0) the least recently used and (2, 1) the most. */
    CHECK(mdkr_ghost_bank_select(0, 1, 0) == 0);
    CHECK(mdkr_ghost_bank_select(0, 3, 2) == 0);
    CHECK(mdkr_ghost_bank_select(0, 4, 0) == 0);
    CHECK(mdkr_ghost_bank_select(0, 5, 1) == 0);
    CHECK(mdkr_ghost_bank_select(0, 6, 2) == 0);
    CHECK(mdkr_ghost_bank_select(0, 2, 1) == 0);
    CHECK(s_noteWrites == 0);

    slot = mdkr_ghost_window_find(s_note, NOTE_BYTES, 1, 0);
    CHECK(slot >= 0);
    CHECK(mdkr_ghost_window_extract(s_note, NOTE_BYTES, slot, original,
                                    sizeof(original), &original_length) ==
          MDKR_GHOST_BANK_OK);

    /* A seventh pair with no banked ghost: the LRU pair is evicted to its
     * bank file and the freed slot is left empty for the authored save. */
    CHECK(mdkr_ghost_bank_select(0, 30, 1) == 0);
    CHECK(s_noteWrites == 1);
    CHECK(mdkr_ghost_window_find(s_note, NOTE_BYTES, 1, 0) == -1);
    CHECK(mdkr_ghost_window_find(s_note, NOTE_BYTES, 30, 1) == -1);
    CHECK(mdkr_ghost_window_occupied(s_note, NOTE_BYTES) == 5);
    CHECK(mdkr_ghost_window_validate(s_note, NOTE_BYTES) ==
          MDKR_GHOST_BANK_OK);
    CHECK(bank_file_payload(1, 0, banked, sizeof(banked), &banked_length));
    CHECK(banked_length == original_length);
    CHECK(memcmp(banked, original, banked_length) == 0);

    /* The authored save would now find an empty slot; simulate it. */
    CHECK(note_append(s_note, 30, 1, 7, 4321, 150) >= 0);

    /* Re-selecting the evicted pair restores its record byte-identically,
     * evicting the current LRU (3, 2) in turn. */
    CHECK(mdkr_ghost_bank_select(0, 1, 0) == 0);
    CHECK(s_noteWrites == 2);
    CHECK(mdkr_ghost_window_find(s_note, NOTE_BYTES, 3, 2) == -1);
    slot = mdkr_ghost_window_find(s_note, NOTE_BYTES, 1, 0);
    CHECK(slot >= 0);
    CHECK(mdkr_ghost_window_extract(s_note, NOTE_BYTES, slot, restored,
                                    sizeof(restored), &restored_length) ==
          MDKR_GHOST_BANK_OK);
    CHECK(restored_length == original_length);
    CHECK(memcmp(restored, original, restored_length) == 0);
    CHECK(mdkr_ghost_window_occupied(s_note, NOTE_BYTES) == 6);
    CHECK(mdkr_ghost_window_validate(s_note, NOTE_BYTES) ==
          MDKR_GHOST_BANK_OK);
    /* The pair saved through the empty slot survived both swaps. */
    CHECK(mdkr_ghost_window_find(s_note, NOTE_BYTES, 30, 1) >= 0);
}

static void test_select_erase_reconciliation(void) {
    int slot;

    reset_fake_pak();
    mdkr_ghost_bank_reset();
    fill_six_pairs();
    CHECK(mdkr_ghost_bank_select(0, 1, 0) == 0); /* migration split */
    CHECK(bank_file_exists(4, 0));

    /* The player erases (4, 0) through the authored pak menu
     * (func_800753D8): remove it from the fake note the same compacting way.
     */
    slot = mdkr_ghost_window_find(s_note, NOTE_BYTES, 4, 0);
    CHECK(slot >= 0);
    CHECK(mdkr_ghost_window_remove(s_note, NOTE_BYTES, slot) ==
          MDKR_GHOST_BANK_OK);

    /* The next select notices the erase and drops the bank copy instead of
     * resurrecting it. */
    CHECK(mdkr_ghost_bank_select(0, 2, 1) == 0);
    CHECK(!bank_file_exists(4, 0));
    /* And selecting the erased pair afterwards does not bring it back. */
    CHECK(mdkr_ghost_bank_select(0, 4, 0) == 0);
    CHECK(mdkr_ghost_window_find(s_note, NOTE_BYTES, 4, 0) == -1);

    /* Whole-note deletion (pak reformat / note delete) wipes the library. */
    CHECK(bank_file_exists(1, 0));
    s_noteExists = 0;
    CHECK(mdkr_ghost_bank_select(0, 2, 1) == 0);
    CHECK(!bank_file_exists(1, 0));
    CHECK(!bank_file_exists(2, 1));
    CHECK(!bank_file_exists(3, 2));
}

static void test_select_failure_no_ops(void) {
    uint8_t before[NOTE_BYTES];

    reset_fake_pak();
    mdkr_ghost_bank_reset();
    fill_six_pairs();
    CHECK(mdkr_ghost_bank_select(0, 1, 0) == 0);
    memcpy(before, s_note, NOTE_BYTES);

    /* A refused pak write leaves the window untouched. */
    s_failNoteWrite = 1;
    CHECK(mdkr_ghost_bank_select(0, 40, 2) != 0);
    CHECK(memcmp(before, s_note, NOTE_BYTES) == 0);
    s_failNoteWrite = 0;

    /* Recovery: the same request succeeds once the device does. */
    CHECK(mdkr_ghost_bank_select(0, 40, 2) == 0);
    CHECK(s_noteWrites == 1);

    /* No pak at all: a polite refusal, no writes. */
    reset_fake_pak();
    mdkr_ghost_bank_reset();
    s_deviceStatus = CONTROLLER_PAK_RUMBLE_PAK_FOUND;
    CHECK(mdkr_ghost_bank_select(0, 1, 0) != 0);
    CHECK(s_noteWrites == 0);

    /* Illegal arguments are rejected before any device traffic. */
    s_deviceStatus = CONTROLLER_PAK_GOOD;
    CHECK(mdkr_ghost_bank_select(-1, 1, 0) != 0);
    CHECK(mdkr_ghost_bank_select(0, -1, 0) != 0);
    CHECK(mdkr_ghost_bank_select(0, 0xFF, 0) != 0);
    CHECK(mdkr_ghost_bank_select(0, 1, 3) != 0);
}

static void remove_tree(const char *root) {
    /* Only the flat bank directory layout is created here. */
    char path[1400];
    int level;
    int vehicle;
    snprintf(path, sizeof(path), "%s/controller-1-index.mdgi", root);
    remove(path);
    for (level = 0; level < 64; level++) {
        for (vehicle = 0; vehicle < 3; vehicle++) {
            snprintf(path, sizeof(path), "%s/controller-1-ghost-%d-%d.mdg",
                     root, level, vehicle);
            remove(path);
        }
    }
#if defined(_WIN32)
    _rmdir(root);
#else
    rmdir(root);
#endif
}

int main(void) {
    snprintf(s_root, sizeof(s_root), "mdkr64-ghost-bank-test-%ld",
             (long)
#if defined(_WIN32)
             _getpid()
#else
             getpid()
#endif
    );
#if defined(_WIN32)
    _mkdir(s_root);
#else
    mkdir(s_root, 0700);
#endif
    mdkr_ghost_bank_set_root(s_root);

    test_window_operations();
    test_record_codec();
    test_index_codec_and_lru();
    test_select_migration_and_noop();
    test_select_eviction_and_restore();
    test_select_erase_reconciliation();
    test_select_failure_no_ops();

    mdkr_ghost_bank_set_root(NULL);
    remove_tree(s_root);
    if (failures != 0) {
        fprintf(stderr, "ghost_bank: %d failure(s)\n", failures);
        return 1;
    }
    puts("ghost_bank: PASS");
    return 0;
}
