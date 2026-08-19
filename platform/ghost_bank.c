/**
 * ghost_bank.c -- per-(level, vehicle) Time Trial ghost bank (issue #46).
 *
 * See ghost_bank.h for the design contract. The file is organized in the
 * same layers the header declares:
 *
 *   1. Pure GHSS window operations over the DKRACING-GHOSTS note bytes.
 *   2. Pure bank-record and index-sidecar codecs (magic/version/SHA-256,
 *      after platform/virtual_pak.c).
 *   3. The host layer: bank directory paths, atomic stores, quarantine, and
 *      mdkr_ghost_bank_select(), the single entry point the two NATIVE_PORT
 *      hooks in game/src/objects.c call.
 *
 * Everything here is best-effort by doctrine: any failure returns the pak,
 * the bank library, and the authored code path to exactly the state they
 * were in, so the authored error contract (including the real PAK FULL
 * dialog for genuine device failures) is never masked, only the artificial
 * six-pair ceiling is.
 */
#include "ghost_bank.h"

#include "fs_utf8.h"
#include "mdkr_trace.h"
#include "sha256.h"
#include "user_paths.h"
#include "rollback/rollback_game_runtime.h"

/* The authored pak helpers this module drives, with their real signatures.
 * save_data.c stays byte-untouched; these are its public exports. */
#include "PR/os_cont.h" /* MAXCONTROLLERS */
#include "save_data.h"

#include <stdio.h>
#include <string.h>

#define GHOST_BANK_PATH_MAX 1200
#define GHOST_WINDOW_MAX_BYTES 0x8000 /* directory offsets are s16 */
#define GHOST_WINDOW_SIGNATURE 0x47485353 /* 'GHSS', save_data.h */
#define GHOST_DIRECTORY_ENTRIES (MDKR_GHOST_WINDOW_SLOTS + 1)
#define GHOST_RECORD_MIN_BYTES 8 /* sizeof(GhostHeader) */

/* ======================================================================== *
 *  1. Pure GHSS window operations
 * ======================================================================== *
 * The window bytes are the note exactly as the native game persists it: raw
 * host structs (save_data.c writes an unk80075000 through the PFS boundary
 * verbatim), so every s16/s32 below is read and written in host order via
 * memcpy, never by shifting bytes.
 */

static int16_t window_read_s16(const uint8_t *at) {
    int16_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static void window_write_s16(uint8_t *at, int16_t value) {
    memcpy(at, &value, sizeof(value));
}

static int32_t window_read_s32(const uint8_t *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static const uint8_t *window_entry(const uint8_t *window, int slot) {
    return window + 4 + (size_t)slot * 4;
}

static uint8_t *window_entry_mut(uint8_t *window, int slot) {
    return window + 4 + (size_t)slot * 4;
}

static int pair_fields_valid(int level, int vehicle) {
    return level >= 0 && level < 0xFF && vehicle >= 0 && vehicle <= 2;
}

/* The same bounds ghost_directory_is_valid() enforces in save_data.c. */
MdkrGhostBankResult mdkr_ghost_window_validate(
    const uint8_t *window, size_t size) {
    int previous = MDKR_GHOST_WINDOW_DIRECTORY_BYTES;
    int i;
    if (window == NULL) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    if (size < MDKR_GHOST_WINDOW_DIRECTORY_BYTES ||
        size > INT16_MAX ||
        window_read_s32(window) != GHOST_WINDOW_SIGNATURE) {
        return MDKR_GHOST_BANK_ERR_FORMAT;
    }
    for (i = 0; i < GHOST_DIRECTORY_ENTRIES; i++) {
        const uint8_t *entry = window_entry(window, i);
        int offset = window_read_s16(entry + 2);
        if (offset < MDKR_GHOST_WINDOW_DIRECTORY_BYTES ||
            offset < previous || (size_t)offset > size) {
            return MDKR_GHOST_BANK_ERR_FORMAT;
        }
        if (i < MDKR_GHOST_WINDOW_SLOTS && entry[0] != 0xFF) {
            int next = window_read_s16(window_entry(window, i + 1) + 2);
            if (entry[1] > 2 || next < offset ||
                next - offset < GHOST_RECORD_MIN_BYTES) {
                return MDKR_GHOST_BANK_ERR_FORMAT;
            }
        }
        previous = offset;
    }
    return MDKR_GHOST_BANK_OK;
}

int mdkr_ghost_window_find(
    const uint8_t *window, size_t size, int level, int vehicle) {
    int i;
    if (mdkr_ghost_window_validate(window, size) != MDKR_GHOST_BANK_OK) {
        return -1;
    }
    for (i = 0; i < MDKR_GHOST_WINDOW_SLOTS; i++) {
        const uint8_t *entry = window_entry(window, i);
        if (entry[0] != 0xFF && entry[0] == (uint8_t)level &&
            entry[1] == (uint8_t)vehicle) {
            return i;
        }
    }
    return -1;
}

int mdkr_ghost_window_occupied(const uint8_t *window, size_t size) {
    int count = 0;
    int i;
    if (mdkr_ghost_window_validate(window, size) != MDKR_GHOST_BANK_OK) {
        return -1;
    }
    for (i = 0; i < MDKR_GHOST_WINDOW_SLOTS; i++) {
        if (window_entry(window, i)[0] != 0xFF) {
            count++;
        }
    }
    return count;
}

MdkrGhostBankResult mdkr_ghost_window_pair_at(
    const uint8_t *window, size_t size, int slot,
    int *level, int *vehicle) {
    const uint8_t *entry;
    MdkrGhostBankResult result = mdkr_ghost_window_validate(window, size);
    if (result != MDKR_GHOST_BANK_OK) {
        return result;
    }
    if (slot < 0 || slot >= MDKR_GHOST_WINDOW_SLOTS) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    entry = window_entry(window, slot);
    if (entry[0] == 0xFF) {
        return MDKR_GHOST_BANK_ERR_NOT_FOUND;
    }
    if (level != NULL) {
        *level = entry[0];
    }
    if (vehicle != NULL) {
        *vehicle = entry[1];
    }
    return MDKR_GHOST_BANK_OK;
}

MdkrGhostBankResult mdkr_ghost_window_extract(
    const uint8_t *window, size_t size, int slot,
    uint8_t *payload, size_t capacity, size_t *length) {
    const uint8_t *entry;
    int offset;
    int next;
    MdkrGhostBankResult result = mdkr_ghost_window_validate(window, size);
    if (result != MDKR_GHOST_BANK_OK) {
        return result;
    }
    if (slot < 0 || slot >= MDKR_GHOST_WINDOW_SLOTS || payload == NULL ||
        length == NULL) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    entry = window_entry(window, slot);
    if (entry[0] == 0xFF) {
        return MDKR_GHOST_BANK_ERR_NOT_FOUND;
    }
    offset = window_read_s16(entry + 2);
    next = window_read_s16(window_entry(window, slot + 1) + 2);
    if ((size_t)(next - offset) > capacity) {
        return MDKR_GHOST_BANK_ERR_RANGE;
    }
    memcpy(payload, window + offset, (size_t)(next - offset));
    *length = (size_t)(next - offset);
    return MDKR_GHOST_BANK_OK;
}

/* Rebuild the window in the canonical shape the authored writers produce:
 * occupied slots as a compact prefix in the order given, every empty entry
 * carrying {0xFF, 0, end}, the payload area packed, and the slack zeroed
 * (func_80075000 and func_800753D8 both bzero their rewrite buffer). The
 * record byte ranges passed in must not alias `window`; callers stage them
 * in `scratch`. */
typedef struct {
    uint8_t level;
    uint8_t vehicle;
    const uint8_t *bytes;
    size_t length;
} GhostWindowRecord;

static MdkrGhostBankResult window_rebuild(
    uint8_t *window, size_t size,
    const GhostWindowRecord *records, int count) {
    size_t offset = MDKR_GHOST_WINDOW_DIRECTORY_BYTES;
    int i;
    if (count < 0 || count > MDKR_GHOST_WINDOW_SLOTS) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    for (i = 0; i < count; i++) {
        offset += records[i].length;
    }
    if (offset > size) {
        return MDKR_GHOST_BANK_ERR_FULL;
    }
    memset(window, 0, size);
    {
        int32_t signature = GHOST_WINDOW_SIGNATURE;
        memcpy(window, &signature, sizeof(signature));
    }
    offset = MDKR_GHOST_WINDOW_DIRECTORY_BYTES;
    for (i = 0; i < count; i++) {
        uint8_t *entry = window_entry_mut(window, i);
        entry[0] = records[i].level;
        entry[1] = records[i].vehicle;
        window_write_s16(entry + 2, (int16_t)offset);
        memcpy(window + offset, records[i].bytes, records[i].length);
        offset += records[i].length;
    }
    for (i = count; i < GHOST_DIRECTORY_ENTRIES; i++) {
        uint8_t *entry = window_entry_mut(window, i);
        entry[0] = 0xFF;
        entry[1] = 0;
        window_write_s16(entry + 2, (int16_t)offset);
    }
    return MDKR_GHOST_BANK_OK;
}

/* Stage every occupied record's extent into `scratch` and list them in slot
 * order. Returns the record count, or -1 on an invalid window. */
static int window_stage_records(
    const uint8_t *window, size_t size,
    uint8_t *scratch, size_t scratch_capacity,
    GhostWindowRecord *records) {
    size_t used = 0;
    int count = 0;
    int i;
    if (mdkr_ghost_window_validate(window, size) != MDKR_GHOST_BANK_OK) {
        return -1;
    }
    for (i = 0; i < MDKR_GHOST_WINDOW_SLOTS; i++) {
        const uint8_t *entry = window_entry(window, i);
        int offset;
        int next;
        if (entry[0] == 0xFF) {
            continue;
        }
        offset = window_read_s16(entry + 2);
        next = window_read_s16(window_entry(window, i + 1) + 2);
        if (used + (size_t)(next - offset) > scratch_capacity) {
            return -1;
        }
        memcpy(scratch + used, window + offset, (size_t)(next - offset));
        records[count].level = entry[0];
        records[count].vehicle = entry[1];
        records[count].bytes = scratch + used;
        records[count].length = (size_t)(next - offset);
        used += (size_t)(next - offset);
        count++;
    }
    return count;
}

MdkrGhostBankResult mdkr_ghost_window_remove(
    uint8_t *window, size_t size, int slot) {
    uint8_t scratch[GHOST_WINDOW_MAX_BYTES];
    GhostWindowRecord records[MDKR_GHOST_WINDOW_SLOTS];
    int count;
    int victim_level;
    int victim_vehicle;
    int kept = 0;
    int i;
    MdkrGhostBankResult result =
        mdkr_ghost_window_pair_at(window, size, slot, &victim_level,
                                  &victim_vehicle);
    if (result != MDKR_GHOST_BANK_OK) {
        return result;
    }
    count = window_stage_records(window, size, scratch, sizeof(scratch),
                                 records);
    if (count < 0) {
        return MDKR_GHOST_BANK_ERR_FORMAT;
    }
    for (i = 0; i < count; i++) {
        if (records[i].level == (uint8_t)victim_level &&
            records[i].vehicle == (uint8_t)victim_vehicle) {
            continue;
        }
        records[kept++] = records[i];
    }
    return window_rebuild(window, size, records, kept);
}

MdkrGhostBankResult mdkr_ghost_window_insert(
    uint8_t *window, size_t size, int level, int vehicle,
    const uint8_t *payload, size_t length) {
    uint8_t scratch[GHOST_WINDOW_MAX_BYTES];
    GhostWindowRecord records[MDKR_GHOST_WINDOW_SLOTS];
    int count;
    MdkrGhostBankResult result = mdkr_ghost_window_validate(window, size);
    if (result != MDKR_GHOST_BANK_OK) {
        return result;
    }
    if (!pair_fields_valid(level, vehicle) || payload == NULL ||
        length < GHOST_RECORD_MIN_BYTES ||
        length > MDKR_GHOST_RECORD_MAX_BYTES) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    if (mdkr_ghost_window_find(window, size, level, vehicle) != -1) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    count = window_stage_records(window, size, scratch, sizeof(scratch),
                                 records);
    if (count < 0) {
        return MDKR_GHOST_BANK_ERR_FORMAT;
    }
    if (count >= MDKR_GHOST_WINDOW_SLOTS) {
        return MDKR_GHOST_BANK_ERR_FULL;
    }
    records[count].level = (uint8_t)level;
    records[count].vehicle = (uint8_t)vehicle;
    records[count].bytes = payload;
    records[count].length = length;
    return window_rebuild(window, size, records, count + 1);
}

/* ======================================================================== *
 *  2. Bank record and index codecs
 * ======================================================================== */

static const uint8_t s_record_magic[8] = {'M', 'D', 'K', 'R',
                                          'G', 'B', 'R', '1'};
static const uint8_t s_index_magic[8] = {'M', 'D', 'K', 'R',
                                         'G', 'B', 'X', '1'};
#define GHOST_BANK_FORMAT_VERSION 1u
#define GHOST_BANK_DIGEST_OFFSET 32u

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_be64(const uint8_t *p) {
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static void write_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void write_be64(uint8_t *p, uint64_t value) {
    write_be32(p, (uint32_t)(value >> 32));
    write_be32(p + 4, (uint32_t)value);
}

/* Digest of the whole image with its digest field held at zero, exactly
 * virtual_pak.c's image_digest scheme. */
static void bank_image_digest(
    const uint8_t *image, size_t image_size,
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE]) {
    static const uint8_t zero_digest[MDKR_SHA256_DIGEST_SIZE] = {0};
    MdkrSha256 context;
    mdkr_sha256_init(&context);
    mdkr_sha256_update(&context, image, GHOST_BANK_DIGEST_OFFSET);
    mdkr_sha256_update(&context, zero_digest, sizeof(zero_digest));
    mdkr_sha256_update(
        &context,
        image + GHOST_BANK_DIGEST_OFFSET + MDKR_SHA256_DIGEST_SIZE,
        image_size - GHOST_BANK_DIGEST_OFFSET - MDKR_SHA256_DIGEST_SIZE);
    mdkr_sha256_final(&context, digest);
}

MdkrGhostBankResult mdkr_ghost_bank_record_encode(
    int level, int vehicle, const uint8_t *payload, size_t length,
    uint8_t *image, size_t capacity, size_t *image_size) {
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    size_t total;
    if (payload == NULL || image == NULL || image_size == NULL ||
        !pair_fields_valid(level, vehicle) ||
        length < GHOST_RECORD_MIN_BYTES ||
        length > MDKR_GHOST_RECORD_MAX_BYTES) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    total = MDKR_GHOST_BANK_RECORD_HEADER_BYTES + length;
    if (capacity < total) {
        return MDKR_GHOST_BANK_ERR_RANGE;
    }
    memset(image, 0, MDKR_GHOST_BANK_RECORD_HEADER_BYTES);
    memcpy(image, s_record_magic, sizeof(s_record_magic));
    write_be32(image + 8, GHOST_BANK_FORMAT_VERSION);
    write_be32(image + 12, (uint32_t)total);
    write_be32(image + 16, (uint32_t)level);
    write_be32(image + 20, (uint32_t)vehicle);
    write_be32(image + 24, (uint32_t)length);
    memcpy(image + MDKR_GHOST_BANK_RECORD_HEADER_BYTES, payload, length);
    bank_image_digest(image, total, digest);
    memcpy(image + GHOST_BANK_DIGEST_OFFSET, digest, sizeof(digest));
    *image_size = total;
    return MDKR_GHOST_BANK_OK;
}

MdkrGhostBankResult mdkr_ghost_bank_record_decode(
    const uint8_t *image, size_t image_size, int *level, int *vehicle,
    uint8_t *payload, size_t capacity, size_t *length) {
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    uint32_t stored_level;
    uint32_t stored_vehicle;
    uint32_t stored_length;
    if (image == NULL || payload == NULL || length == NULL) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    if (image_size <
            MDKR_GHOST_BANK_RECORD_HEADER_BYTES + GHOST_RECORD_MIN_BYTES ||
        image_size > MDKR_GHOST_BANK_RECORD_IMAGE_MAX ||
        memcmp(image, s_record_magic, sizeof(s_record_magic)) != 0 ||
        read_be32(image + 8) != GHOST_BANK_FORMAT_VERSION ||
        read_be32(image + 12) != image_size) {
        return MDKR_GHOST_BANK_ERR_FORMAT;
    }
    bank_image_digest(image, image_size, digest);
    if (memcmp(digest, image + GHOST_BANK_DIGEST_OFFSET, sizeof(digest)) !=
        0) {
        return MDKR_GHOST_BANK_ERR_DIGEST;
    }
    stored_level = read_be32(image + 16);
    stored_vehicle = read_be32(image + 20);
    stored_length = read_be32(image + 24);
    if (!pair_fields_valid((int)stored_level, (int)stored_vehicle) ||
        stored_length !=
            image_size - MDKR_GHOST_BANK_RECORD_HEADER_BYTES ||
        read_be32(image + 28) != 0) {
        return MDKR_GHOST_BANK_ERR_FORMAT;
    }
    if (capacity < stored_length) {
        return MDKR_GHOST_BANK_ERR_RANGE;
    }
    memcpy(payload, image + MDKR_GHOST_BANK_RECORD_HEADER_BYTES,
           stored_length);
    if (level != NULL) {
        *level = (int)stored_level;
    }
    if (vehicle != NULL) {
        *vehicle = (int)stored_vehicle;
    }
    *length = stored_length;
    return MDKR_GHOST_BANK_OK;
}

void mdkr_ghost_bank_index_init(MdkrGhostBankIndex *index) {
    if (index != NULL) {
        memset(index, 0, sizeof(*index));
        index->tick_counter = 1;
    }
}

MdkrGhostBankResult mdkr_ghost_bank_index_encode(
    const MdkrGhostBankIndex *index, uint8_t *image, size_t capacity,
    size_t *image_size) {
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    size_t total;
    uint32_t i;
    if (index == NULL || image == NULL || image_size == NULL ||
        index->count > MDKR_GHOST_BANK_INDEX_MAX_ENTRIES) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    total = MDKR_GHOST_BANK_INDEX_HEADER_BYTES +
            (size_t)index->count * MDKR_GHOST_BANK_INDEX_ENTRY_BYTES;
    if (capacity < total) {
        return MDKR_GHOST_BANK_ERR_RANGE;
    }
    memset(image, 0, total);
    memcpy(image, s_index_magic, sizeof(s_index_magic));
    write_be32(image + 8, GHOST_BANK_FORMAT_VERSION);
    write_be32(image + 12, (uint32_t)total);
    write_be64(image + 16, index->tick_counter);
    write_be32(image + 24, index->count);
    for (i = 0; i < index->count; i++) {
        uint8_t *entry = image + MDKR_GHOST_BANK_INDEX_HEADER_BYTES +
                         (size_t)i * MDKR_GHOST_BANK_INDEX_ENTRY_BYTES;
        entry[0] = index->entries[i].level;
        entry[1] = index->entries[i].vehicle;
        entry[2] = index->entries[i].in_window ? 1 : 0;
        write_be64(entry + 4, index->entries[i].tick);
    }
    bank_image_digest(image, total, digest);
    memcpy(image + GHOST_BANK_DIGEST_OFFSET, digest, sizeof(digest));
    *image_size = total;
    return MDKR_GHOST_BANK_OK;
}

MdkrGhostBankResult mdkr_ghost_bank_index_decode(
    const uint8_t *image, size_t image_size, MdkrGhostBankIndex *index) {
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    MdkrGhostBankIndex candidate;
    uint32_t count;
    uint32_t i;
    if (image == NULL || index == NULL) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    if (image_size < MDKR_GHOST_BANK_INDEX_HEADER_BYTES ||
        image_size > MDKR_GHOST_BANK_INDEX_IMAGE_MAX ||
        memcmp(image, s_index_magic, sizeof(s_index_magic)) != 0 ||
        read_be32(image + 8) != GHOST_BANK_FORMAT_VERSION ||
        read_be32(image + 12) != image_size) {
        return MDKR_GHOST_BANK_ERR_FORMAT;
    }
    bank_image_digest(image, image_size, digest);
    if (memcmp(digest, image + GHOST_BANK_DIGEST_OFFSET, sizeof(digest)) !=
        0) {
        return MDKR_GHOST_BANK_ERR_DIGEST;
    }
    count = read_be32(image + 24);
    if (count > MDKR_GHOST_BANK_INDEX_MAX_ENTRIES ||
        image_size != MDKR_GHOST_BANK_INDEX_HEADER_BYTES +
                          (size_t)count * MDKR_GHOST_BANK_INDEX_ENTRY_BYTES) {
        return MDKR_GHOST_BANK_ERR_FORMAT;
    }
    mdkr_ghost_bank_index_init(&candidate);
    candidate.tick_counter = read_be64(image + 16);
    candidate.count = count;
    for (i = 0; i < count; i++) {
        const uint8_t *entry = image + MDKR_GHOST_BANK_INDEX_HEADER_BYTES +
                               (size_t)i * MDKR_GHOST_BANK_INDEX_ENTRY_BYTES;
        uint32_t j;
        if (!pair_fields_valid(entry[0], entry[1]) || entry[2] > 1 ||
            entry[3] != 0 || read_be32(entry + 12) != 0) {
            return MDKR_GHOST_BANK_ERR_FORMAT;
        }
        for (j = 0; j < i; j++) {
            if (candidate.entries[j].level == entry[0] &&
                candidate.entries[j].vehicle == entry[1]) {
                return MDKR_GHOST_BANK_ERR_FORMAT;
            }
        }
        candidate.entries[i].level = entry[0];
        candidate.entries[i].vehicle = entry[1];
        candidate.entries[i].in_window = entry[2];
        candidate.entries[i].tick = read_be64(entry + 4);
        if (candidate.entries[i].tick >= candidate.tick_counter) {
            return MDKR_GHOST_BANK_ERR_FORMAT;
        }
    }
    *index = candidate;
    return MDKR_GHOST_BANK_OK;
}

int mdkr_ghost_bank_index_find(
    const MdkrGhostBankIndex *index, int level, int vehicle) {
    uint32_t i;
    if (index == NULL) {
        return -1;
    }
    for (i = 0; i < index->count &&
                i < MDKR_GHOST_BANK_INDEX_MAX_ENTRIES; i++) {
        if (index->entries[i].level == (uint8_t)level &&
            index->entries[i].vehicle == (uint8_t)vehicle) {
            return (int)i;
        }
    }
    return -1;
}

MdkrGhostBankResult mdkr_ghost_bank_index_touch(
    MdkrGhostBankIndex *index, int level, int vehicle, int in_window) {
    int at;
    if (index == NULL || !pair_fields_valid(level, vehicle)) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    at = mdkr_ghost_bank_index_find(index, level, vehicle);
    if (at < 0) {
        if (index->count >= MDKR_GHOST_BANK_INDEX_MAX_ENTRIES) {
            return MDKR_GHOST_BANK_ERR_FULL;
        }
        at = (int)index->count++;
        index->entries[at].level = (uint8_t)level;
        index->entries[at].vehicle = (uint8_t)vehicle;
    }
    index->entries[at].in_window = in_window ? 1 : 0;
    index->entries[at].tick = index->tick_counter++;
    return MDKR_GHOST_BANK_OK;
}

MdkrGhostBankResult mdkr_ghost_bank_index_set_in_window(
    MdkrGhostBankIndex *index, int level, int vehicle, int in_window) {
    int at;
    if (index == NULL) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    at = mdkr_ghost_bank_index_find(index, level, vehicle);
    if (at < 0) {
        return MDKR_GHOST_BANK_ERR_NOT_FOUND;
    }
    index->entries[at].in_window = in_window ? 1 : 0;
    return MDKR_GHOST_BANK_OK;
}

MdkrGhostBankResult mdkr_ghost_bank_index_remove(
    MdkrGhostBankIndex *index, int level, int vehicle) {
    int at;
    if (index == NULL) {
        return MDKR_GHOST_BANK_ERR_ARGUMENT;
    }
    at = mdkr_ghost_bank_index_find(index, level, vehicle);
    if (at < 0) {
        return MDKR_GHOST_BANK_ERR_NOT_FOUND;
    }
    memmove(&index->entries[at], &index->entries[at + 1],
            (size_t)(index->count - (uint32_t)at - 1) *
                sizeof(index->entries[0]));
    index->count--;
    memset(&index->entries[index->count], 0, sizeof(index->entries[0]));
    return MDKR_GHOST_BANK_OK;
}

int mdkr_ghost_bank_pick_victim(
    const uint8_t *window, size_t size, const MdkrGhostBankIndex *index) {
    uint64_t best_tick = 0;
    int best_slot = -1;
    int i;
    if (index == NULL ||
        mdkr_ghost_window_validate(window, size) != MDKR_GHOST_BANK_OK) {
        return -1;
    }
    for (i = 0; i < MDKR_GHOST_WINDOW_SLOTS; i++) {
        const uint8_t *entry = window_entry(window, i);
        uint64_t tick = 0;
        int at;
        if (entry[0] == 0xFF) {
            continue;
        }
        at = mdkr_ghost_bank_index_find(index, entry[0], entry[1]);
        if (at >= 0) {
            tick = index->entries[at].tick;
        }
        if (best_slot < 0 || tick < best_tick) {
            best_slot = i;
            best_tick = tick;
        }
    }
    return best_slot;
}

/* ======================================================================== *
 *  3. Host layer
 * ======================================================================== */

#ifdef __EMSCRIPTEN__
/* Defined in platform/stubs_dkr.c (EM_ASYNC_JS): commits MEMFS to IDBFS. */
extern int mdkr_persist_save_async(int kind);
#endif

static char s_bank_root[GHOST_BANK_PATH_MAX];
static int s_bank_root_state; /* 0 unresolved, 1 ready, -1 failed */
static char s_bank_root_override[GHOST_BANK_PATH_MAX];
static int s_bank_root_override_set;

void mdkr_ghost_bank_set_root(const char *directory) {
    if (directory == NULL) {
        s_bank_root_override_set = 0;
    } else {
        snprintf(s_bank_root_override, sizeof(s_bank_root_override), "%s",
                 directory);
        s_bank_root_override_set = 1;
    }
    s_bank_root_state = 0;
}

void mdkr_ghost_bank_reset(void) {
    s_bank_root_state = 0;
}

static int ensure_directory(const char *path) {
    int exists = 0;
    int is_directory = 0;
    if (mdkr_path_query_utf8(path, &exists, NULL, &is_directory) == 0 &&
        exists) {
        return is_directory;
    }
    if (mdkr_mkdir_utf8(path) == 0) {
        return 1;
    }
    exists = 0;
    is_directory = 0;
    return mdkr_path_query_utf8(path, &exists, NULL, &is_directory) == 0 &&
           exists && is_directory;
}

static int bank_root(char *out, size_t out_size) {
    if (s_bank_root_state == 0) {
        if (s_bank_root_override_set) {
            snprintf(s_bank_root, sizeof(s_bank_root), "%s",
                     s_bank_root_override);
            s_bank_root_state = 1;
        } else {
            char save_dir[GHOST_BANK_PATH_MAX];
            int written;
            if (!mdkr_user_save_directory(save_dir, sizeof(save_dir)) ||
                !ensure_directory(save_dir)) {
                s_bank_root_state = -1;
            } else {
                written = snprintf(s_bank_root, sizeof(s_bank_root),
                                   "%s/ghost-bank", save_dir);
                if (written < 0 ||
                    (size_t)written >= sizeof(s_bank_root) ||
                    !ensure_directory(s_bank_root)) {
                    s_bank_root_state = -1;
                } else {
                    s_bank_root_state = 1;
                }
            }
            if (s_bank_root_state < 0) {
                fprintf(stderr,
                        "[GHOSTBANK] ghost bank directory is unavailable\n");
            }
        }
    }
    if (s_bank_root_state < 0) {
        return 0;
    }
    return snprintf(out, out_size, "%s", s_bank_root) < (int)out_size;
}

static int record_path(char *out, size_t out_size, int controllerIndex,
                       int level, int vehicle) {
    char root[GHOST_BANK_PATH_MAX];
    int written;
    if (!bank_root(root, sizeof(root))) {
        return 0;
    }
    written = snprintf(out, out_size, "%s/controller-%d-ghost-%d-%d.mdg",
                       root, controllerIndex + 1, level, vehicle);
    return written >= 0 && (size_t)written < out_size;
}

static int index_path(char *out, size_t out_size, int controllerIndex) {
    char root[GHOST_BANK_PATH_MAX];
    int written;
    if (!bank_root(root, sizeof(root))) {
        return 0;
    }
    written = snprintf(out, out_size, "%s/controller-%d-index.mdgi", root,
                       controllerIndex + 1);
    return written >= 0 && (size_t)written < out_size;
}

/* Copy-on-write store: temp file, flush to stable storage, atomic replace,
 * then directory-entry flush -- virtual_pak_store's transaction. */
static int store_image(const char *path, const uint8_t *image, size_t size) {
    char temporary[GHOST_BANK_PATH_MAX + 8];
    FILE *file;
    int failed = 0;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) < 0) {
        return 0;
    }
    file = mdkr_fopen_utf8(temporary, "wb");
    if (file == NULL) {
        fprintf(stderr, "[GHOSTBANK] could not open %s\n", temporary);
        return 0;
    }
    if (fwrite(image, 1, size, file) != size || fflush(file) != 0) {
        failed = 1;
    }
    if (!failed && mdkr_file_sync(file) != 0) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (!failed && mdkr_move_utf8(temporary, path, 1, 1) != 0) {
        failed = 1;
    }
    if (failed) {
        (void)mdkr_remove_utf8(temporary);
        fprintf(stderr, "[GHOSTBANK] durable write of %s failed\n", path);
        return 0;
    }
    (void)mdkr_parent_directory_sync_utf8(path);
#ifdef __EMSCRIPTEN__
    (void)mdkr_persist_save_async(1);
#endif
    return 1;
}

/* Read a whole image. Returns 1 with *size set, 0 when the file does not
 * exist (or cannot be opened), -1 when it exists but overflows `capacity`. */
static int load_image(const char *path, uint8_t *image, size_t capacity,
                      size_t *size) {
    FILE *file = mdkr_fopen_utf8(path, "rb");
    size_t got;
    int trailing;
    if (file == NULL) {
        return 0;
    }
    got = fread(image, 1, capacity, file);
    trailing = fgetc(file);
    if (ferror(file) != 0 || fclose(file) != 0 || trailing != EOF) {
        return -1;
    }
    *size = got;
    return 1;
}

/* Rename a file that failed validation out of the way, virtual_pak style. */
static void quarantine(const char *path) {
    char bad_path[GHOST_BANK_PATH_MAX + 16];
    int suffix;
    for (suffix = 1; suffix <= 99; suffix++) {
        int exists = 1;
        if (snprintf(bad_path, sizeof(bad_path), "%s.bad.%d", path,
                     suffix) < 0) {
            return;
        }
        if (mdkr_path_query_utf8(bad_path, &exists, NULL, NULL) == 0 &&
            exists) {
            continue;
        }
        if (mdkr_move_utf8(path, bad_path, 0, 1) == 0) {
            fprintf(stderr, "[GHOSTBANK] quarantined corrupt file as %s\n",
                    bad_path);
        }
        return;
    }
    fprintf(stderr,
            "[GHOSTBANK] corrupt file retained; quarantine slots are full\n");
}

/* Load the controller's index sidecar; absent or corrupt (after quarantine)
 * yields an empty index. Returns 0 only when the bank root is unusable. */
static int index_load(int controllerIndex, MdkrGhostBankIndex *index) {
    uint8_t image[MDKR_GHOST_BANK_INDEX_IMAGE_MAX];
    char path[GHOST_BANK_PATH_MAX];
    size_t size = 0;
    int got;
    mdkr_ghost_bank_index_init(index);
    if (!index_path(path, sizeof(path), controllerIndex)) {
        return 0;
    }
    got = load_image(path, image, sizeof(image), &size);
    if (got <= 0) {
        if (got < 0) {
            quarantine(path);
        }
        return 1;
    }
    if (mdkr_ghost_bank_index_decode(image, size, index) !=
        MDKR_GHOST_BANK_OK) {
        quarantine(path);
        mdkr_ghost_bank_index_init(index);
    }
    return 1;
}

static int index_store(int controllerIndex,
                       const MdkrGhostBankIndex *index) {
    uint8_t image[MDKR_GHOST_BANK_INDEX_IMAGE_MAX];
    char path[GHOST_BANK_PATH_MAX];
    size_t size = 0;
    if (!index_path(path, sizeof(path), controllerIndex) ||
        mdkr_ghost_bank_index_encode(index, image, sizeof(image), &size) !=
            MDKR_GHOST_BANK_OK) {
        return 0;
    }
    return store_image(path, image, size);
}

static int record_exists(int controllerIndex, int level, int vehicle) {
    char path[GHOST_BANK_PATH_MAX];
    int exists = 0;
    if (!record_path(path, sizeof(path), controllerIndex, level, vehicle)) {
        return 0;
    }
    return mdkr_path_query_utf8(path, &exists, NULL, NULL) == 0 && exists;
}

static int record_store(int controllerIndex, int level, int vehicle,
                        const uint8_t *payload, size_t length) {
    uint8_t image[MDKR_GHOST_BANK_RECORD_IMAGE_MAX];
    char path[GHOST_BANK_PATH_MAX];
    size_t size = 0;
    if (!record_path(path, sizeof(path), controllerIndex, level, vehicle) ||
        mdkr_ghost_bank_record_encode(level, vehicle, payload, length, image,
                                      sizeof(image), &size) !=
            MDKR_GHOST_BANK_OK) {
        return 0;
    }
    return store_image(path, image, size);
}

/* Returns 1 with the pair's banked extent, 0 when no (valid) bank copy
 * exists; a corrupt file is quarantined and reported absent. */
static int record_load(int controllerIndex, int level, int vehicle,
                       uint8_t *payload, size_t capacity, size_t *length) {
    uint8_t image[MDKR_GHOST_BANK_RECORD_IMAGE_MAX];
    char path[GHOST_BANK_PATH_MAX];
    size_t size = 0;
    int stored_level = -1;
    int stored_vehicle = -1;
    int got;
    if (!record_path(path, sizeof(path), controllerIndex, level, vehicle)) {
        return 0;
    }
    got = load_image(path, image, sizeof(image), &size);
    if (got <= 0) {
        if (got < 0) {
            quarantine(path);
        }
        return 0;
    }
    if (mdkr_ghost_bank_record_decode(image, size, &stored_level,
                                      &stored_vehicle, payload, capacity,
                                      length) != MDKR_GHOST_BANK_OK ||
        stored_level != level || stored_vehicle != vehicle) {
        quarantine(path);
        return 0;
    }
    return 1;
}

static void record_delete(int controllerIndex, int level, int vehicle) {
    char path[GHOST_BANK_PATH_MAX];
    if (record_path(path, sizeof(path), controllerIndex, level, vehicle)) {
        (void)mdkr_remove_utf8(path);
    }
}

static void index_delete(int controllerIndex) {
    char path[GHOST_BANK_PATH_MAX];
    if (index_path(path, sizeof(path), controllerIndex)) {
        (void)mdkr_remove_utf8(path);
    }
}

/* The whole DKRACING-GHOSTS note is gone: a pak reformat or a note deletion
 * through the pak menu. Authored intent is "every ghost on this pak is
 * gone", and the bank must not outvote it, so the library goes too. The
 * sidecar is deleted last so an interrupted wipe resumes. */
static void wipe_library(int controllerIndex,
                         const MdkrGhostBankIndex *index) {
    uint32_t i;
    for (i = 0; i < index->count; i++) {
        record_delete(controllerIndex, index->entries[i].level,
                      index->entries[i].vehicle);
    }
    index_delete(controllerIndex);
    MDKR_TRACE("[GHOSTBANK] event=wipe controller=%d entries=%u",
               controllerIndex, (unsigned)index->count);
}

/* Reconcile the index with the window the game actually has:
 *   - a pair the index says was in the window but is not there any more was
 *     erased through the authored pak menu (func_800753D8), so its bank copy
 *     is dropped rather than resurrected later;
 *   - a resident pair the index has never seen is split out to its own bank
 *     file (the first-run migration of a pre-bank DKRACING-GHOSTS, and the
 *     pickup of pairs the authored save path added through an empty slot);
 *   - a resident pair the index thought was banked is promoted (the
 *     window-write side of a swap landed but the follow-up index write did
 *     not).
 * Only the index (and, for migration, missing bank files) is written here;
 * the pak is never touched. Returns 1 when the index changed. */
static int reconcile(int controllerIndex, const uint8_t *window, size_t size,
                     MdkrGhostBankIndex *index, int io_allowed) {
    uint8_t payload[MDKR_GHOST_RECORD_MAX_BYTES];
    int changed = 0;
    int i;
    for (i = (int)index->count - 1; i >= 0; i--) {
        MdkrGhostBankIndexEntry entry = index->entries[i];
        if (!entry.in_window) {
            continue;
        }
        if (mdkr_ghost_window_find(window, size, entry.level,
                                   entry.vehicle) >= 0) {
            continue;
        }
        if (!io_allowed) {
            continue;
        }
        record_delete(controllerIndex, entry.level, entry.vehicle);
        (void)mdkr_ghost_bank_index_remove(index, entry.level, entry.vehicle);
        changed = 1;
        MDKR_TRACE("[GHOSTBANK] event=erase controller=%d level=%d vehicle=%d",
                   controllerIndex, entry.level, entry.vehicle);
    }
    for (i = 0; i < MDKR_GHOST_WINDOW_SLOTS; i++) {
        int level = -1;
        int vehicle = -1;
        int at;
        if (mdkr_ghost_window_pair_at(window, size, i, &level, &vehicle) !=
            MDKR_GHOST_BANK_OK) {
            continue;
        }
        at = mdkr_ghost_bank_index_find(index, level, vehicle);
        if (at >= 0) {
            if (!index->entries[at].in_window) {
                (void)mdkr_ghost_bank_index_set_in_window(index, level,
                                                          vehicle, 1);
                changed = 1;
            }
            continue;
        }
        if (!io_allowed) {
            continue;
        }
        if (!record_exists(controllerIndex, level, vehicle)) {
            size_t length = 0;
            if (mdkr_ghost_window_extract(window, size, i, payload,
                                          sizeof(payload), &length) ==
                    MDKR_GHOST_BANK_OK &&
                record_store(controllerIndex, level, vehicle, payload,
                             length)) {
                MDKR_TRACE(
                    "[GHOSTBANK] event=migrate controller=%d level=%d "
                    "vehicle=%d bytes=%u",
                    controllerIndex, level, vehicle, (unsigned)length);
            } else {
                /* Leave the pair out of the index: a later select retries
                 * the split instead of recording a lie. */
                continue;
            }
        }
        if (mdkr_ghost_bank_index_touch(index, level, vehicle, 1) ==
            MDKR_GHOST_BANK_OK) {
            changed = 1;
        }
    }
    return changed;
}

int mdkr_ghost_bank_select(int controllerIndex, int levelId, int vehicleId) {
    uint8_t window[GHOST_WINDOW_MAX_BYTES];
    uint8_t banked[MDKR_GHOST_RECORD_MAX_BYTES];
    MdkrGhostBankIndex index;
    SIDeviceStatus status;
    size_t banked_length = 0;
    int have_banked = 0;
    int window_dirty = 0;
    int index_dirty = 0;
    int io_allowed;
    s32 file_number = -1;
    s32 file_size = 0;
    int slot;

    if (controllerIndex < 0 || controllerIndex >= MAXCONTROLLERS ||
        !pair_fields_valid(levelId, vehicleId)) {
        return -1;
    }

    /* The authored read bracket, exactly as func_80074B34 opens and closes
     * one: device status first, note lookup and read inside, then hand the
     * SI back. Any status this module does not understand is left for the
     * authored path to report through its own dialog wiring. */
    status = get_si_device_status(controllerIndex);
    if (status != CONTROLLER_PAK_GOOD) {
        start_reading_controller_data(controllerIndex);
        return -1;
    }
    status = get_file_number(controllerIndex, "DKRACING-GHOSTS", "",
                             &file_number);
    if (status == CONTROLLER_PAK_GOOD) {
        if (get_file_size(controllerIndex, file_number, &file_size) !=
                CONTROLLER_PAK_GOOD ||
            file_size < MDKR_GHOST_WINDOW_DIRECTORY_BYTES ||
            file_size > INT16_MAX ||
            read_data_from_controller_pak(controllerIndex, file_number,
                                          window, file_size) !=
                CONTROLLER_PAK_GOOD) {
            start_reading_controller_data(controllerIndex);
            return -1;
        }
    } else if (status != CONTROLLER_PAK_CHANGED) {
        start_reading_controller_data(controllerIndex);
        return -1;
    }
    start_reading_controller_data(controllerIndex);

    {
        char root[GHOST_BANK_PATH_MAX];
        if (!bank_root(root, sizeof(root))) {
            return -1;
        }
    }
    /* An online rollback timeline owns no durable progression; refuse every
     * bank/index/pak mutation in that state, same as virtual_pak_store. */
    io_allowed = mdkr_rollback_game_runtime_host_io_allowed(true);

    if (!index_load(controllerIndex, &index)) {
        return -1;
    }

    if (status == CONTROLLER_PAK_CHANGED) {
        /* No DKRACING-GHOSTS note. */
        if (index.count > 0 && io_allowed) {
            wipe_library(controllerIndex, &index);
        }
        /* An absent note never blocks a pair: the authored save creates the
         * whole file with the pair in slot zero. */
        return 0;
    }

    if (mdkr_ghost_window_validate(window, (size_t)file_size) !=
        MDKR_GHOST_BANK_OK) {
        /* Leave a malformed note exactly as found: the authored path owns
         * the CONTROLLER_PAK_BAD_DATA report and recovery. */
        return -1;
    }

    index_dirty = reconcile(controllerIndex, window, (size_t)file_size,
                            &index, io_allowed);

    slot = mdkr_ghost_window_find(window, (size_t)file_size, levelId,
                                  vehicleId);
    if (slot >= 0) {
        if (mdkr_ghost_bank_index_touch(&index, levelId, vehicleId, 1) ==
            MDKR_GHOST_BANK_OK) {
            index_dirty = 1;
        }
        if (index_dirty && io_allowed) {
            (void)index_store(controllerIndex, &index);
        }
        return 0;
    }

    have_banked = record_load(controllerIndex, levelId, vehicleId, banked,
                              sizeof(banked), &banked_length);

    if (mdkr_ghost_window_occupied(window, (size_t)file_size) >=
        MDKR_GHOST_WINDOW_SLOTS) {
        uint8_t victim_payload[MDKR_GHOST_RECORD_MAX_BYTES];
        size_t victim_length = 0;
        int victim_level = -1;
        int victim_vehicle = -1;
        int victim = mdkr_ghost_bank_pick_victim(window, (size_t)file_size,
                                                 &index);
        if (!io_allowed || victim < 0 ||
            mdkr_ghost_window_pair_at(window, (size_t)file_size, victim,
                                      &victim_level, &victim_vehicle) !=
                MDKR_GHOST_BANK_OK ||
            mdkr_ghost_window_extract(window, (size_t)file_size, victim,
                                      victim_payload, sizeof(victim_payload),
                                      &victim_length) != MDKR_GHOST_BANK_OK) {
            return -1;
        }
        /* Bank the victim, then demote it in the index BEFORE the pak
         * write: if the process dies between the two stores, the pair is
         * still in the window and the reconcile promotes it back; the
         * reverse order would make the next reconcile read the half-done
         * swap as a player erase and delete the only copy. */
        if (!record_store(controllerIndex, victim_level, victim_vehicle,
                          victim_payload, victim_length)) {
            return -1;
        }
        /* Demotion is bookkeeping, not use: keep the victim's LRU tick. */
        if (mdkr_ghost_bank_index_set_in_window(&index, victim_level,
                                                victim_vehicle, 0) !=
            MDKR_GHOST_BANK_OK) {
            (void)mdkr_ghost_bank_index_touch(&index, victim_level,
                                              victim_vehicle, 0);
        }
        if (!index_store(controllerIndex, &index)) {
            return -1;
        }
        index_dirty = 0;
        if (mdkr_ghost_window_remove(window, (size_t)file_size, victim) !=
            MDKR_GHOST_BANK_OK) {
            return -1;
        }
        window_dirty = 1;
        MDKR_TRACE("[GHOSTBANK] event=evict controller=%d level=%d vehicle=%d "
                   "bytes=%u",
                   controllerIndex, victim_level, victim_vehicle,
                   (unsigned)victim_length);
    }

    if (have_banked) {
        if (mdkr_ghost_window_insert(window, (size_t)file_size, levelId,
                                     vehicleId, banked, banked_length) !=
            MDKR_GHOST_BANK_OK) {
            return -1;
        }
        window_dirty = 1;
    }

    if (window_dirty) {
        if (!io_allowed ||
            write_controller_pak_file(controllerIndex, file_number,
                                      "DKRACING-GHOSTS", "", window,
                                      file_size) != CONTROLLER_PAK_GOOD) {
            return -1;
        }
        if (have_banked) {
            MDKR_TRACE(
                "[GHOSTBANK] event=restore controller=%d level=%d vehicle=%d "
                "bytes=%u",
                controllerIndex, levelId, vehicleId,
                (unsigned)banked_length);
        }
    }

    if (have_banked &&
        mdkr_ghost_bank_index_touch(&index, levelId, vehicleId, 1) ==
            MDKR_GHOST_BANK_OK) {
        index_dirty = 1;
    }
    if ((index_dirty || window_dirty) && io_allowed) {
        (void)index_store(controllerIndex, &index);
    }
    return 0;
}
