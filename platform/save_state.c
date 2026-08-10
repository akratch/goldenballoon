/*
 * save_state.c — the save-state container: write it, read it, refuse it.
 *
 * See save_state.h for why this format is kept at arm's length from the
 * progress save. The rule this file follows throughout: nothing the file
 * claims about itself is acted on until it has been checked against
 * something the file does not control — the caller's buffer capacity, or the
 * size the filesystem reports.
 */
#include "save_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The on-disk record is a fixed 72 bytes whatever the compiler does to the
 * struct. If this ever fires, the format changed and format_version owes the
 * world a bump. */
_Static_assert(MDKR_SAVE_STATE_HEADER_BYTES ==
                   4u + 4u + MDKR_SAVE_STATE_ROM_REVISION_MAX +
                       MDKR_SAVE_STATE_APP_VERSION_MAX + 8u + 4u + 4u,
               "on-disk save state header is 72 bytes");

#define OFF_MAGIC          0u
#define OFF_FORMAT_VERSION 4u
#define OFF_ROM_REVISION   8u
#define OFF_APP_VERSION    24u
#define OFF_TICK           56u
#define OFF_PAYLOAD_BYTES  64u
#define OFF_PAYLOAD_CRC32  68u

/* Precisions used when a caller-supplied string goes into a reason line, so a
 * pathological argument cannot crowd the rest of the sentence out of `err`. */
#define ROM_PREC ((int)(MDKR_SAVE_STATE_ROM_REVISION_MAX - 1u))
#define APP_PREC ((int)(MDKR_SAVE_STATE_APP_VERSION_MAX - 1u))

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void
set_error(char *err, size_t err_size, const char *fmt, ...) {
    va_list args;
    if (err == NULL || err_size == 0u) {
        return;
    }
    va_start(args, fmt);
    (void)vsnprintf(err, err_size, fmt, args);
    va_end(args);
}

static void put_le32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void put_le64(uint8_t *bytes, uint64_t value) {
    put_le32(bytes, (uint32_t)(value & 0xFFFFFFFFu));
    put_le32(bytes + 4, (uint32_t)((value >> 32) & 0xFFFFFFFFu));
}

static uint32_t get_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t get_le64(const uint8_t *bytes) {
    return (uint64_t)get_le32(bytes) | ((uint64_t)get_le32(bytes + 4) << 32);
}

/* A fixed-width text field with no terminator anywhere in it turns every
 * later strcmp into a read past the field. Refused, not repaired. */
static int field_is_terminated(const char *field, size_t size) {
    size_t i;
    for (i = 0u; i < size; i++) {
        if (field[i] == '\0') {
            return 1;
        }
    }
    return 0;
}

/* Zero-fills first, so the bytes after the terminator are always zeroes and
 * the same header always produces the same file. */
static void put_field(uint8_t *bytes, size_t size, const char *text) {
    size_t used = strlen(text);
    memset(bytes, 0, size);
    if (used > size) { /* unreachable: callers check termination first */
        used = size;
    }
    memcpy(bytes, text, used);
}

uint32_t mdkr_save_state_crc32(const void *data, size_t size) {
    /* CRC-32/ISO-HDLC, reflected, four bits at a time. Sixteen entries
     * instead of the usual 256: this checks a snapshot for corruption on a
     * key press, so a 1 KiB table would be the only large thing about it. */
    static const uint32_t nibble[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
    };
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;

    if (bytes == NULL || size == 0u) {
        return 0u; /* which is also 0xFFFFFFFF ^ 0xFFFFFFFF */
    }
    for (i = 0u; i < size; i++) {
        crc ^= (uint32_t)bytes[i];
        crc = (crc >> 4) ^ nibble[crc & 0x0Fu];
        crc = (crc >> 4) ^ nibble[crc & 0x0Fu];
    }
    return crc ^ 0xFFFFFFFFu;
}

int mdkr_save_state_validate(const MdkrSaveStateHeader *header,
                             const char *rom_revision, const char *app_version,
                             char *err, size_t err_size) {
    if (header == NULL) {
        set_error(err, err_size, "no save state header supplied");
        return MDKR_SAVE_STATE_ERR_ARGUMENT;
    }

    if (header->magic != MDKR_SAVE_STATE_MAGIC) {
        set_error(err, err_size,
                  "not a Golden Balloon save state: unrecognised format tag "
                  "0x%08lX, expected 0x%08lX",
                  (unsigned long)header->magic,
                  (unsigned long)MDKR_SAVE_STATE_MAGIC);
        return MDKR_SAVE_STATE_ERR_FORMAT;
    }

    if (header->format_version != MDKR_SAVE_STATE_VERSION) {
        set_error(err, err_size,
                  "save state uses format version %lu; this build reads "
                  "format version %lu",
                  (unsigned long)header->format_version,
                  (unsigned long)MDKR_SAVE_STATE_VERSION);
        return MDKR_SAVE_STATE_ERR_VERSION;
    }

    if (!field_is_terminated(header->rom_revision,
                             sizeof(header->rom_revision))) {
        set_error(err, err_size,
                  "save state ROM revision field is not terminated");
        return MDKR_SAVE_STATE_ERR_FORMAT;
    }
    if (!field_is_terminated(header->app_version,
                             sizeof(header->app_version))) {
        set_error(err, err_size,
                  "save state app version field is not terminated");
        return MDKR_SAVE_STATE_ERR_FORMAT;
    }

    if (header->payload_bytes > MDKR_SAVE_STATE_MAX_PAYLOAD) {
        set_error(err, err_size,
                  "save state declares %lu payload bytes, above the %lu-byte "
                  "limit",
                  (unsigned long)header->payload_bytes,
                  (unsigned long)MDKR_SAVE_STATE_MAX_PAYLOAD);
        return MDKR_SAVE_STATE_ERR_SIZE;
    }

    if (rom_revision != NULL &&
        strcmp(header->rom_revision, rom_revision) != 0) {
        set_error(err, err_size,
                  "save state is from ROM revision '%.*s'; this session is "
                  "running '%.*s'",
                  ROM_PREC, header->rom_revision, ROM_PREC, rom_revision);
        return MDKR_SAVE_STATE_ERR_ROM;
    }

    if (app_version != NULL && strcmp(header->app_version, app_version) != 0) {
        set_error(err, err_size,
                  "save state was written by app version '%.*s'; this build "
                  "is version '%.*s'",
                  APP_PREC, header->app_version, APP_PREC, app_version);
        return MDKR_SAVE_STATE_ERR_APP;
    }

    return MDKR_SAVE_STATE_OK;
}

int mdkr_save_state_write(const char *path, const MdkrSaveStateHeader *header,
                          const void *payload, char *err, size_t err_size) {
    uint8_t raw[MDKR_SAVE_STATE_HEADER_BYTES];
    FILE *file;
    size_t payload_bytes;
    uint32_t crc;

    if (path == NULL) {
        set_error(err, err_size, "no save state path supplied");
        return MDKR_SAVE_STATE_ERR_ARGUMENT;
    }
    if (header == NULL) {
        set_error(err, err_size, "no save state header supplied");
        return MDKR_SAVE_STATE_ERR_ARGUMENT;
    }

    /* The writer never corrects a header into this format; a caller holding
     * someone else's header finds out here rather than on disk. */
    if (header->magic != MDKR_SAVE_STATE_MAGIC) {
        set_error(err, err_size,
                  "refusing to write a save state whose format tag is "
                  "0x%08lX, expected 0x%08lX",
                  (unsigned long)header->magic,
                  (unsigned long)MDKR_SAVE_STATE_MAGIC);
        return MDKR_SAVE_STATE_ERR_FORMAT;
    }
    if (header->format_version != MDKR_SAVE_STATE_VERSION) {
        set_error(err, err_size,
                  "refusing to write format version %lu; this build writes "
                  "format version %lu",
                  (unsigned long)header->format_version,
                  (unsigned long)MDKR_SAVE_STATE_VERSION);
        return MDKR_SAVE_STATE_ERR_VERSION;
    }
    if (!field_is_terminated(header->rom_revision,
                             sizeof(header->rom_revision))) {
        set_error(err, err_size, "ROM revision field is not terminated");
        return MDKR_SAVE_STATE_ERR_ARGUMENT;
    }
    if (!field_is_terminated(header->app_version,
                             sizeof(header->app_version))) {
        set_error(err, err_size, "app version field is not terminated");
        return MDKR_SAVE_STATE_ERR_ARGUMENT;
    }
    if (header->payload_bytes > MDKR_SAVE_STATE_MAX_PAYLOAD) {
        set_error(err, err_size,
                  "save state payload of %lu bytes is above the %lu-byte "
                  "limit",
                  (unsigned long)header->payload_bytes,
                  (unsigned long)MDKR_SAVE_STATE_MAX_PAYLOAD);
        return MDKR_SAVE_STATE_ERR_SIZE;
    }
    if (payload == NULL && header->payload_bytes != 0u) {
        set_error(err, err_size,
                  "save state header declares %lu payload bytes but no "
                  "payload was supplied",
                  (unsigned long)header->payload_bytes);
        return MDKR_SAVE_STATE_ERR_ARGUMENT;
    }

    payload_bytes = (size_t)header->payload_bytes;
    /* Computed, never taken on trust, so the checksum in the file always
     * describes the bytes in the file. */
    crc = mdkr_save_state_crc32(payload, payload_bytes);

    memset(raw, 0, sizeof(raw));
    put_le32(raw + OFF_MAGIC, MDKR_SAVE_STATE_MAGIC);
    put_le32(raw + OFF_FORMAT_VERSION, MDKR_SAVE_STATE_VERSION);
    put_field(raw + OFF_ROM_REVISION, MDKR_SAVE_STATE_ROM_REVISION_MAX,
              header->rom_revision);
    put_field(raw + OFF_APP_VERSION, MDKR_SAVE_STATE_APP_VERSION_MAX,
              header->app_version);
    put_le64(raw + OFF_TICK, header->tick);
    put_le32(raw + OFF_PAYLOAD_BYTES, header->payload_bytes);
    put_le32(raw + OFF_PAYLOAD_CRC32, crc);

    file = fopen(path, "wb");
    if (file == NULL) {
        set_error(err, err_size, "cannot open save state '%s' for writing",
                  path);
        return MDKR_SAVE_STATE_ERR_IO;
    }

    if (fwrite(raw, 1u, sizeof(raw), file) != sizeof(raw) ||
        (payload_bytes != 0u &&
         fwrite(payload, 1u, payload_bytes, file) != payload_bytes)) {
        (void)fclose(file);
        (void)remove(path);
        set_error(err, err_size, "cannot write save state '%s'", path);
        return MDKR_SAVE_STATE_ERR_IO;
    }

    if (fclose(file) != 0) {
        /* Buffered data can fail to reach the disk here and nowhere else. A
         * partial state file is worse than none. */
        (void)remove(path);
        set_error(err, err_size, "cannot finish writing save state '%s'",
                  path);
        return MDKR_SAVE_STATE_ERR_IO;
    }

    return MDKR_SAVE_STATE_OK;
}

int mdkr_save_state_read(const char *path, MdkrSaveStateHeader *out_header,
                         void *payload, size_t payload_cap,
                         char *err, size_t err_size) {
    MdkrSaveStateHeader header;
    uint8_t raw[MDKR_SAVE_STATE_HEADER_BYTES];
    FILE *file;
    long measured;
    unsigned long file_bytes;
    unsigned long expected_bytes;
    size_t payload_bytes;
    uint32_t computed;
    int result;

    if (path == NULL) {
        set_error(err, err_size, "no save state path supplied");
        return MDKR_SAVE_STATE_ERR_ARGUMENT;
    }
    if (out_header == NULL) {
        set_error(err, err_size, "no output header supplied");
        return MDKR_SAVE_STATE_ERR_ARGUMENT;
    }
    if (payload == NULL && payload_cap != 0u) {
        set_error(err, err_size,
                  "payload capacity of %lu bytes given with no buffer",
                  (unsigned long)payload_cap);
        return MDKR_SAVE_STATE_ERR_ARGUMENT;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(err, err_size, "cannot open save state '%s'", path);
        return MDKR_SAVE_STATE_ERR_IO;
    }

    /* The size the filesystem reports is the only thing here the file does
     * not get to claim for itself, so everything else is checked against it. */
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        set_error(err, err_size, "cannot measure save state '%s'", path);
        return MDKR_SAVE_STATE_ERR_IO;
    }
    measured = ftell(file);
    if (measured < 0L || fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        set_error(err, err_size, "cannot measure save state '%s'", path);
        return MDKR_SAVE_STATE_ERR_IO;
    }
    file_bytes = (unsigned long)measured;

    if (file_bytes < (unsigned long)MDKR_SAVE_STATE_HEADER_BYTES) {
        (void)fclose(file);
        set_error(err, err_size,
                  "save state is truncated: %lu bytes, need at least %lu for "
                  "the header",
                  file_bytes, (unsigned long)MDKR_SAVE_STATE_HEADER_BYTES);
        return MDKR_SAVE_STATE_ERR_TRUNCATED;
    }

    /* Zeroed before the read, not after: a short read returns below, but if a
     * later edit ever lets one through, the fields it parses are zeroes and
     * fail the format check rather than being uninitialised stack. */
    memset(raw, 0, sizeof(raw));
    if (fread(raw, 1u, sizeof(raw), file) != sizeof(raw)) {
        (void)fclose(file);
        set_error(err, err_size, "save state header is truncated or unreadable");
        return MDKR_SAVE_STATE_ERR_TRUNCATED;
    }

    memset(&header, 0, sizeof(header));
    header.magic = get_le32(raw + OFF_MAGIC);
    header.format_version = get_le32(raw + OFF_FORMAT_VERSION);
    memcpy(header.rom_revision, raw + OFF_ROM_REVISION,
           MDKR_SAVE_STATE_ROM_REVISION_MAX);
    memcpy(header.app_version, raw + OFF_APP_VERSION,
           MDKR_SAVE_STATE_APP_VERSION_MAX);
    header.tick = get_le64(raw + OFF_TICK);
    header.payload_bytes = get_le32(raw + OFF_PAYLOAD_BYTES);
    header.payload_crc32 = get_le32(raw + OFF_PAYLOAD_CRC32);

    /* Format tag, format version, field termination, declared length ceiling.
     * The session comparisons are the caller's to make, with this same
     * function, once it has a header to show. */
    result = mdkr_save_state_validate(&header, NULL, NULL, err, err_size);
    if (result != MDKR_SAVE_STATE_OK) {
        (void)fclose(file);
        return result;
    }

    payload_bytes = (size_t)header.payload_bytes;

    /* Capacity before length, so an oversized state is refused against the
     * caller's real buffer rather than read into it and regretted. */
    if (payload_bytes > payload_cap) {
        (void)fclose(file);
        set_error(err, err_size,
                  "save state payload is %lu bytes and does not fit the "
                  "%lu-byte buffer supplied",
                  (unsigned long)payload_bytes, (unsigned long)payload_cap);
        return MDKR_SAVE_STATE_ERR_CAPACITY;
    }

    /* No overflow: payload_bytes passed the ceiling check above. */
    expected_bytes = (unsigned long)MDKR_SAVE_STATE_HEADER_BYTES +
                     (unsigned long)payload_bytes;
    if (file_bytes != expected_bytes) {
        (void)fclose(file);
        set_error(err, err_size,
                  "save state header declares %lu payload bytes, so the file "
                  "should be %lu bytes, but it holds %lu",
                  (unsigned long)payload_bytes, expected_bytes, file_bytes);
        return (file_bytes < expected_bytes) ? MDKR_SAVE_STATE_ERR_TRUNCATED
                                             : MDKR_SAVE_STATE_ERR_SIZE;
    }

    if (payload_bytes != 0u &&
        fread(payload, 1u, payload_bytes, file) != payload_bytes) {
        (void)fclose(file);
        set_error(err, err_size,
                  "save state payload is truncated or unreadable: %lu bytes "
                  "expected",
                  (unsigned long)payload_bytes);
        return MDKR_SAVE_STATE_ERR_TRUNCATED;
    }

    (void)fclose(file);

    computed = mdkr_save_state_crc32(payload, payload_bytes);
    if (computed != header.payload_crc32) {
        /* The payload is already in the caller's buffer and is not
         * trustworthy. Blank it so ignoring this return value produces
         * obvious nonsense instead of plausible nonsense. */
        if (payload_bytes != 0u) {
            memset(payload, 0, payload_bytes);
        }
        set_error(err, err_size,
                  "save state payload checksum mismatch: file says 0x%08lX, "
                  "payload is 0x%08lX",
                  (unsigned long)header.payload_crc32,
                  (unsigned long)computed);
        return MDKR_SAVE_STATE_ERR_CHECKSUM;
    }

    /* Committed only here, so a refused state can never be mistaken for a
     * loaded one. */
    *out_header = header;
    return MDKR_SAVE_STATE_OK;
}
