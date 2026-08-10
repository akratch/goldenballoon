/*
 * test_save_state_container.c — the save-state file format, and every way it
 * is allowed to be refused.
 *
 * A save state is NOT the progress save. This test deliberately links only
 * platform/save_state.c: if it ever needs save_container.h or save_codec.h to
 * build, the two formats have grown into each other and that is the bug.
 *
 * The offsets asserted below are the on-disk format contract, hardcoded on
 * purpose. A state file written on one host must be readable on another, so
 * the layout is pinned here rather than derived from the struct — deriving it
 * from sizeof/offsetof would let padding or a field reorder silently
 * redefine the format and still pass.
 */
#include "save_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* Reports the reason too, because a refusal with an unhelpful reason is a
 * defect this module exists to prevent. */
#define CHECK_REASON(condition, err)                                           \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s (reason was \"%s\")\n",   \
                    __FILE__, __LINE__, #condition, (err));                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* On-disk field offsets. See the file comment: these are the contract. */
#define OFF_MAGIC          0u
#define OFF_FORMAT_VERSION 4u
#define OFF_ROM_REVISION   8u
#define OFF_APP_VERSION    24u
#define OFF_TICK           56u
#define OFF_PAYLOAD_BYTES  64u
#define OFF_PAYLOAD_CRC32  68u
#define OFF_PAYLOAD        72u

#define PAYLOAD_BYTES 64u
#define ERR_SIZE      256u
#define SCRATCH_BYTES 4096u

static uint8_t g_payload[PAYLOAD_BYTES];
static char g_dir[256];

static int contains(const char *haystack, const char *needle) {
    return haystack != NULL && strstr(haystack, needle) != NULL;
}

static void temp_path(char *out, size_t out_size, const char *leaf) {
    snprintf(out, out_size, "%s/%s", g_dir, leaf);
}

/* Reads a whole file. Returns byte count, or (size_t)-1 if it does not fit. */
static size_t slurp(const char *path, uint8_t *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    size_t got;
    int extra;
    if (f == NULL) {
        return (size_t)-1;
    }
    got = fread(out, 1u, cap, f);
    extra = fgetc(f);
    fclose(f);
    if (extra != EOF) {
        return (size_t)-1;
    }
    return got;
}

static int spill(const char *path, const uint8_t *bytes, size_t count) {
    FILE *f = fopen(path, "wb");
    size_t wrote;
    if (f == NULL) {
        return -1;
    }
    wrote = (count == 0u) ? 0u : fwrite(bytes, 1u, count, f);
    if (fclose(f) != 0 || wrote != count) {
        return -1;
    }
    return 0;
}

static void put_le32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t get_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static MdkrSaveStateHeader fixture_header(void) {
    MdkrSaveStateHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = MDKR_SAVE_STATE_MAGIC;
    header.format_version = MDKR_SAVE_STATE_VERSION;
    snprintf(header.rom_revision, sizeof(header.rom_revision), "us.v80");
    snprintf(header.app_version, sizeof(header.app_version), "1.0.6");
    header.tick = 0x0123456789ABCDEFULL;
    header.payload_bytes = PAYLOAD_BYTES;
    header.payload_crc32 = mdkr_save_state_crc32(g_payload, PAYLOAD_BYTES);
    return header;
}

/* Writes the fixture state and returns its raw bytes, so tests can corrupt a
 * genuine file rather than a hand-assembled approximation of one. */
static size_t write_fixture(const char *path, uint8_t *raw, size_t raw_cap) {
    MdkrSaveStateHeader header = fixture_header();
    char err[ERR_SIZE];
    err[0] = '\0';
    CHECK_REASON(mdkr_save_state_write(path, &header, g_payload, err,
                                       sizeof(err)) == 0,
                 err);
    return slurp(path, raw, raw_cap);
}

/* ---------------------------------------------------------------- 1 */

static void test_round_trip(void) {
    char path[512];
    MdkrSaveStateHeader written = fixture_header();
    MdkrSaveStateHeader read_back;
    uint8_t buffer[SCRATCH_BYTES];
    char err[ERR_SIZE];

    temp_path(path, sizeof(path), "round_trip.mdkrstate");
    err[0] = '\0';
    CHECK_REASON(
        mdkr_save_state_write(path, &written, g_payload, err, sizeof(err)) == 0,
        err);

    memset(&read_back, 0x5A, sizeof(read_back));
    memset(buffer, 0xA5, sizeof(buffer));
    err[0] = '\0';
    CHECK_REASON(mdkr_save_state_read(path, &read_back, buffer, sizeof(buffer),
                                      err, sizeof(err)) == 0,
                 err);

    CHECK(read_back.magic == written.magic);
    CHECK(read_back.format_version == written.format_version);
    CHECK(strcmp(read_back.rom_revision, written.rom_revision) == 0);
    CHECK(strcmp(read_back.app_version, written.app_version) == 0);
    CHECK(read_back.tick == written.tick);
    CHECK(read_back.payload_bytes == written.payload_bytes);
    CHECK(read_back.payload_crc32 == written.payload_crc32);
    CHECK(memcmp(buffer, g_payload, PAYLOAD_BYTES) == 0);
    /* Nothing beyond the declared payload may be written. */
    CHECK(buffer[PAYLOAD_BYTES] == 0xA5);
    CHECK(buffer[sizeof(buffer) - 1u] == 0xA5);

    /* An exactly-sized buffer is legal; off-by-one capacity checks show up
     * here rather than in a fuzzer. */
    memset(buffer, 0xA5, sizeof(buffer));
    err[0] = '\0';
    CHECK_REASON(mdkr_save_state_read(path, &read_back, buffer, PAYLOAD_BYTES,
                                      err, sizeof(err)) == 0,
                 err);
    CHECK(memcmp(buffer, g_payload, PAYLOAD_BYTES) == 0);

    /* The header alone validates against a matching session. */
    err[0] = '\0';
    CHECK_REASON(mdkr_save_state_validate(&read_back, "us.v80", "1.0.6", err,
                                          sizeof(err)) == 0,
                 err);
    /* ...and with no session strings to compare against, which is how the UI
     * inspects a state before it knows what to compare it to. */
    err[0] = '\0';
    CHECK_REASON(
        mdkr_save_state_validate(&read_back, NULL, NULL, err, sizeof(err)) == 0,
        err);

    /* An empty payload is a legal state, not a truncated one. */
    {
        MdkrSaveStateHeader empty = fixture_header();
        empty.payload_bytes = 0u;
        empty.payload_crc32 = mdkr_save_state_crc32(NULL, 0u);
        temp_path(path, sizeof(path), "empty.mdkrstate");
        err[0] = '\0';
        CHECK_REASON(
            mdkr_save_state_write(path, &empty, NULL, err, sizeof(err)) == 0,
            err);
        err[0] = '\0';
        CHECK_REASON(mdkr_save_state_read(path, &read_back, buffer,
                                          sizeof(buffer), err,
                                          sizeof(err)) == 0,
                     err);
        CHECK(read_back.payload_bytes == 0u);
        remove(path);
    }

    temp_path(path, sizeof(path), "round_trip.mdkrstate");
    remove(path);
}

/* ---------------------------------------------------------------- 2 */

static void test_wrong_magic(void) {
    char path[512];
    uint8_t raw[SCRATCH_BYTES];
    uint8_t buffer[SCRATCH_BYTES];
    MdkrSaveStateHeader header;
    MdkrSaveStateHeader untouched;
    char err[ERR_SIZE];
    size_t size;

    temp_path(path, sizeof(path), "bad_magic.mdkrstate");
    size = write_fixture(path, raw, sizeof(raw));
    CHECK(size == OFF_PAYLOAD + PAYLOAD_BYTES);
    CHECK(get_le32(raw + OFF_MAGIC) == MDKR_SAVE_STATE_MAGIC);

    put_le32(raw + OFF_MAGIC, 0x4D444B45u); /* 'MDKE' — a neighbour's tag */
    CHECK(spill(path, raw, size) == 0);

    memset(&header, 0x5A, sizeof(header));
    untouched = header;
    memset(buffer, 0xA5, sizeof(buffer));
    err[0] = '\0';
    CHECK(mdkr_save_state_read(path, &header, buffer, sizeof(buffer), err,
                               sizeof(err)) != 0);
    CHECK_REASON(contains(err, "format"), err);
    CHECK(memcmp(&header, &untouched, sizeof(header)) == 0);
    CHECK(buffer[0] == 0xA5);

    /* validate() refuses the same thing without a file in hand. */
    header = fixture_header();
    header.magic = 0x4D444B45u;
    err[0] = '\0';
    CHECK(mdkr_save_state_validate(&header, "us.v80", "1.0.6", err,
                                   sizeof(err)) != 0);
    CHECK_REASON(contains(err, "format"), err);

    remove(path);
}

/* ---------------------------------------------------------------- 3 */

static void test_rom_revision_mismatch(void) {
    MdkrSaveStateHeader header = fixture_header();
    char err[ERR_SIZE];

    err[0] = '\0';
    CHECK(mdkr_save_state_validate(&header, "eu.v81", "1.0.6", err,
                                   sizeof(err)) != 0);
    CHECK_REASON(contains(err, "us.v80"), err); /* what the state holds */
    CHECK_REASON(contains(err, "eu.v81"), err); /* what is running */

    /* Same revision, different case, is still a different revision: the
     * comparison is exact, because ROM identity is not a display string. */
    err[0] = '\0';
    CHECK(mdkr_save_state_validate(&header, "US.V80", "1.0.6", err,
                                   sizeof(err)) != 0);
    CHECK_REASON(contains(err, "US.V80"), err);
}

/* ---------------------------------------------------------------- 4 */

static void test_app_version_mismatch(void) {
    MdkrSaveStateHeader header = fixture_header();
    char err[ERR_SIZE];

    err[0] = '\0';
    CHECK(mdkr_save_state_validate(&header, "us.v80", "1.0.7", err,
                                   sizeof(err)) != 0);
    CHECK_REASON(contains(err, "1.0.6"), err);
    CHECK_REASON(contains(err, "1.0.7"), err);

    /* A state that differs in both is still refused with one line; the ROM
     * mismatch is the one reported, because it is the one that matters. */
    err[0] = '\0';
    CHECK(mdkr_save_state_validate(&header, "eu.v81", "1.0.7", err,
                                   sizeof(err)) != 0);
    CHECK_REASON(contains(err, "us.v80"), err);
    CHECK(strchr(err, '\n') == NULL);
}

/* ---------------------------------------------------------------- 5 */

static unsigned test_truncation_at_every_offset(void) {
    char path[512];
    char source[512];
    uint8_t raw[SCRATCH_BYTES];
    uint8_t buffer[SCRATCH_BYTES];
    char err[ERR_SIZE];
    size_t size;
    size_t header_bytes;
    size_t limit;
    size_t n;
    unsigned iterations = 0u;

    temp_path(source, sizeof(source), "truncation_source.mdkrstate");
    size = write_fixture(source, raw, sizeof(raw));
    CHECK(size == OFF_PAYLOAD + PAYLOAD_BYTES);

    header_bytes = sizeof(MdkrSaveStateHeader);
    if (MDKR_SAVE_STATE_HEADER_BYTES > header_bytes) {
        header_bytes = MDKR_SAVE_STATE_HEADER_BYTES;
    }
    limit = header_bytes + 16u;
    /* Every offset in the loop must be a genuine truncation of a longer
     * file, otherwise the last iterations quietly test a valid read. */
    CHECK(limit < size);

    temp_path(path, sizeof(path), "truncated.mdkrstate");

    /* A zero-byte file is the degenerate case of the same bug. */
    CHECK(spill(path, raw, 0u) == 0);
    err[0] = '\0';
    {
        MdkrSaveStateHeader header;
        MdkrSaveStateHeader untouched;
        memset(&header, 0x5A, sizeof(header));
        untouched = header;
        memset(buffer, 0xA5, sizeof(buffer));
        CHECK(mdkr_save_state_read(path, &header, buffer, sizeof(buffer), err,
                                   sizeof(err)) != 0);
        CHECK(err[0] != '\0');
        CHECK(memcmp(&header, &untouched, sizeof(header)) == 0);
    }

    for (n = 1u; n <= limit; n++) {
        MdkrSaveStateHeader header;
        MdkrSaveStateHeader untouched;
        size_t written;

        CHECK(spill(path, raw, n) == 0);
        written = slurp(path, buffer, sizeof(buffer));
        if (written != n) {
            fprintf(stderr,
                    "truncation offset %lu: file holds %lu bytes, not %lu\n",
                    (unsigned long)n, (unsigned long)written,
                    (unsigned long)n);
            failures++;
            continue;
        }

        memset(&header, 0x5A, sizeof(header));
        untouched = header;
        memset(buffer, 0xA5, sizeof(buffer));
        err[0] = '\0';

        if (mdkr_save_state_read(path, &header, buffer, sizeof(buffer), err,
                                 sizeof(err)) == 0) {
            fprintf(stderr, "truncation offset %lu was accepted\n",
                    (unsigned long)n);
            failures++;
        }
        if (err[0] == '\0') {
            fprintf(stderr, "truncation offset %lu refused with no reason\n",
                    (unsigned long)n);
            failures++;
        }
        if (strchr(err, '\n') != NULL) {
            fprintf(stderr, "truncation offset %lu reason is not one line\n",
                    (unsigned long)n);
            failures++;
        }
        if (memcmp(&header, &untouched, sizeof(header)) != 0) {
            fprintf(stderr, "truncation offset %lu wrote to the out header\n",
                    (unsigned long)n);
            failures++;
        }
        if (buffer[0] != 0xA5) {
            fprintf(stderr, "truncation offset %lu wrote to the payload\n",
                    (unsigned long)n);
            failures++;
        }
        iterations++;
    }

    remove(path);
    remove(source);
    return iterations;
}

/* ---------------------------------------------------------------- 6 */

static void test_payload_crc_mismatch(void) {
    char path[512];
    uint8_t raw[SCRATCH_BYTES];
    uint8_t buffer[SCRATCH_BYTES];
    MdkrSaveStateHeader header;
    char err[ERR_SIZE];
    size_t size;

    temp_path(path, sizeof(path), "bad_crc.mdkrstate");
    size = write_fixture(path, raw, sizeof(raw));
    CHECK(size == OFF_PAYLOAD + PAYLOAD_BYTES);

    /* One flipped payload bit, the shape actual bit rot takes. */
    raw[OFF_PAYLOAD + 7u] ^= 0x01u;
    CHECK(spill(path, raw, size) == 0);
    memset(buffer, 0xA5, sizeof(buffer));
    err[0] = '\0';
    CHECK(mdkr_save_state_read(path, &header, buffer, sizeof(buffer), err,
                               sizeof(err)) != 0);
    CHECK_REASON(contains(err, "checksum"), err);
    raw[OFF_PAYLOAD + 7u] ^= 0x01u;

    /* A corrupted checksum field is refused just as firmly as a corrupted
     * payload — the reader must not decide the payload is authoritative. */
    put_le32(raw + OFF_PAYLOAD_CRC32, get_le32(raw + OFF_PAYLOAD_CRC32) ^ 0x1u);
    CHECK(spill(path, raw, size) == 0);
    err[0] = '\0';
    CHECK(mdkr_save_state_read(path, &header, buffer, sizeof(buffer), err,
                               sizeof(err)) != 0);
    CHECK_REASON(contains(err, "checksum"), err);

    remove(path);
}

/* ---------------------------------------------------------------- 7 */

static void test_payload_larger_than_buffer(void) {
    char path[512];
    uint8_t big_payload[256];
    uint8_t raw[SCRATCH_BYTES];
    uint8_t buffer[SCRATCH_BYTES];
    MdkrSaveStateHeader header;
    MdkrSaveStateHeader oversized;
    MdkrSaveStateHeader untouched;
    char err[ERR_SIZE];
    size_t size;
    size_t i;

    for (i = 0u; i < sizeof(big_payload); i++) {
        big_payload[i] = (uint8_t)(i * 7u + 3u);
    }

    /* (a) A truthful header whose payload is simply bigger than the buffer
     * the caller offered. Refused, and the buffer is untouched. */
    temp_path(path, sizeof(path), "oversize.mdkrstate");
    oversized = fixture_header();
    oversized.payload_bytes = (uint32_t)sizeof(big_payload);
    oversized.payload_crc32 =
        mdkr_save_state_crc32(big_payload, sizeof(big_payload));
    err[0] = '\0';
    CHECK_REASON(mdkr_save_state_write(path, &oversized, big_payload, err,
                                       sizeof(err)) == 0,
                 err);

    memset(&header, 0x5A, sizeof(header));
    untouched = header;
    memset(buffer, 0xA5, sizeof(buffer));
    err[0] = '\0';
    CHECK(mdkr_save_state_read(path, &header, buffer, sizeof(big_payload) - 1u,
                               err, sizeof(err)) != 0);
    CHECK(err[0] != '\0');
    CHECK(memcmp(&header, &untouched, sizeof(header)) == 0);
    for (i = 0u; i < sizeof(buffer); i++) {
        if (buffer[i] != 0xA5) {
            fprintf(stderr, "oversize read touched payload byte %lu\n",
                    (unsigned long)i);
            failures++;
            break;
        }
    }
    /* One more byte of capacity and the same file reads cleanly, which is
     * what proves the refusal above was the capacity rule and not an
     * unrelated rejection. */
    err[0] = '\0';
    CHECK_REASON(mdkr_save_state_read(path, &header, buffer,
                                      sizeof(big_payload), err,
                                      sizeof(err)) == 0,
                 err);
    CHECK(memcmp(buffer, big_payload, sizeof(big_payload)) == 0);
    remove(path);

    /* (b) The lying header: a declared length the file cannot back. A reader
     * that trusts payload_bytes and freads it into a buffer sized from the
     * same field is the classic silent read primitive. */
    temp_path(path, sizeof(path), "lying_header.mdkrstate");
    size = write_fixture(path, raw, sizeof(raw));
    CHECK(size == OFF_PAYLOAD + PAYLOAD_BYTES);

    /* Declared length fits the caller's buffer but exceeds the file, so the
     * capacity rule cannot be what catches this one. */
    put_le32(raw + OFF_PAYLOAD_BYTES, 4000u);
    CHECK(spill(path, raw, size) == 0);
    memset(&header, 0x5A, sizeof(header));
    untouched = header;
    memset(buffer, 0xA5, sizeof(buffer));
    err[0] = '\0';
    CHECK(mdkr_save_state_read(path, &header, buffer, sizeof(buffer), err,
                               sizeof(err)) != 0);
    CHECK(err[0] != '\0');
    CHECK(memcmp(&header, &untouched, sizeof(header)) == 0);
    for (i = 0u; i < sizeof(buffer); i++) {
        if (buffer[i] != 0xA5) {
            fprintf(stderr, "lying header touched payload byte %lu\n",
                    (unsigned long)i);
            failures++;
            break;
        }
    }

    /* And the arithmetic-overflow shape of the same lie. */
    put_le32(raw + OFF_PAYLOAD_BYTES, 0xFFFFFFFFu);
    CHECK(spill(path, raw, size) == 0);
    memset(buffer, 0xA5, sizeof(buffer));
    err[0] = '\0';
    CHECK(mdkr_save_state_read(path, &header, buffer, sizeof(buffer), err,
                               sizeof(err)) != 0);
    CHECK(err[0] != '\0');
    CHECK(buffer[0] == 0xA5);

    /* Trailing bytes after a truthful payload are a lie in the other
     * direction and are refused too. */
    put_le32(raw + OFF_PAYLOAD_BYTES, PAYLOAD_BYTES);
    raw[size] = 0xEE;
    CHECK(spill(path, raw, size + 1u) == 0);
    err[0] = '\0';
    CHECK(mdkr_save_state_read(path, &header, buffer, sizeof(buffer), err,
                               sizeof(err)) != 0);
    CHECK(err[0] != '\0');

    remove(path);

    /* The writer refuses to produce a state it cannot describe. */
    err[0] = '\0';
    oversized = fixture_header();
    oversized.payload_bytes = 16u;
    temp_path(path, sizeof(path), "null_payload.mdkrstate");
    CHECK(mdkr_save_state_write(path, &oversized, NULL, err, sizeof(err)) != 0);
    CHECK(err[0] != '\0');
    remove(path);
}

/* ---------------------------------------------------------------- 8 */

static void test_format_version_checked(void) {
    char path[512];
    uint8_t raw[SCRATCH_BYTES];
    uint8_t buffer[SCRATCH_BYTES];
    MdkrSaveStateHeader header;
    MdkrSaveStateHeader untouched;
    char err[ERR_SIZE];
    size_t size;

    temp_path(path, sizeof(path), "future_version.mdkrstate");
    size = write_fixture(path, raw, sizeof(raw));
    CHECK(size == OFF_PAYLOAD + PAYLOAD_BYTES);
    CHECK(get_le32(raw + OFF_FORMAT_VERSION) == MDKR_SAVE_STATE_VERSION);

    put_le32(raw + OFF_FORMAT_VERSION, 7u);
    CHECK(spill(path, raw, size) == 0);

    memset(&header, 0x5A, sizeof(header));
    untouched = header;
    memset(buffer, 0xA5, sizeof(buffer));
    err[0] = '\0';
    CHECK(mdkr_save_state_read(path, &header, buffer, sizeof(buffer), err,
                               sizeof(err)) != 0);
    CHECK_REASON(contains(err, "version 7"), err);
    CHECK_REASON(contains(err, "version 1"), err);
    CHECK(memcmp(&header, &untouched, sizeof(header)) == 0);

    /* Same refusal from the header alone. */
    header = fixture_header();
    header.format_version = 7u;
    err[0] = '\0';
    CHECK(mdkr_save_state_validate(&header, "us.v80", "1.0.6", err,
                                   sizeof(err)) != 0);
    CHECK_REASON(contains(err, "version 7"), err);
    CHECK_REASON(contains(err, "version 1"), err);

    /* An older format is refused for the same reason: this build reads one
     * version, and a migration is a decision, not a default. */
    header.format_version = 0u;
    err[0] = '\0';
    CHECK(mdkr_save_state_validate(&header, "us.v80", "1.0.6", err,
                                   sizeof(err)) != 0);
    CHECK(err[0] != '\0');

    remove(path);
}

/* --------------------------------------------------------------- misc */

static void test_crc32_is_standard(void) {
    static const char check[] = "123456789";
    /* CRC-32/ISO-HDLC. Pinned so a state file stays portable between hosts
     * and between this implementation and any future replacement. */
    CHECK(mdkr_save_state_crc32(check, sizeof(check) - 1u) == 0xCBF43926u);
    CHECK(mdkr_save_state_crc32(NULL, 0u) == 0u);
    CHECK(mdkr_save_state_crc32("", 0u) == 0u);
}

static void test_argument_guards(void) {
    MdkrSaveStateHeader header = fixture_header();
    uint8_t buffer[64];
    char err[ERR_SIZE];
    char path[512];

    temp_path(path, sizeof(path), "guards.mdkrstate");

    err[0] = '\0';
    CHECK(mdkr_save_state_write(NULL, &header, g_payload, err, sizeof(err)) !=
          0);
    CHECK(err[0] != '\0');
    err[0] = '\0';
    CHECK(mdkr_save_state_write(path, NULL, g_payload, err, sizeof(err)) != 0);
    CHECK(err[0] != '\0');
    err[0] = '\0';
    CHECK(mdkr_save_state_read(path, &header, buffer, sizeof(buffer), err,
                               sizeof(err)) != 0);
    CHECK_REASON(contains(err, "guards.mdkrstate"), err);
    err[0] = '\0';
    CHECK(mdkr_save_state_validate(NULL, "us.v80", "1.0.6", err, sizeof(err)) !=
          0);
    CHECK(err[0] != '\0');

    /* The writer will not stamp this format's file with another format's
     * tag, which is the half of the separation a reader cannot enforce. */
    header.magic = 0x4D444B53u ^ 0xFFu;
    err[0] = '\0';
    CHECK(mdkr_save_state_write(path, &header, g_payload, err, sizeof(err)) !=
          0);
    CHECK_REASON(contains(err, "format"), err);
    header = fixture_header();
    header.format_version = 2u;
    err[0] = '\0';
    CHECK(mdkr_save_state_write(path, &header, g_payload, err, sizeof(err)) !=
          0);
    CHECK_REASON(contains(err, "version 2"), err);
    header = fixture_header();

    /* A NULL/zero err buffer must not be a crash, only a lost explanation. */
    header.magic = 0u;
    CHECK(mdkr_save_state_validate(&header, "us.v80", "1.0.6", NULL, 0u) != 0);
    CHECK(mdkr_save_state_validate(&header, "us.v80", "1.0.6", err, 0u) != 0);

    /* A one-byte err buffer gets a NUL, not a stray byte. */
    {
        char tiny[2];
        tiny[0] = 'X';
        tiny[1] = 'X';
        CHECK(mdkr_save_state_validate(&header, "us.v80", "1.0.6", tiny, 1u) !=
              0);
        CHECK(tiny[0] == '\0');
        CHECK(tiny[1] == 'X');
    }
}

/* An unterminated fixed-width field is how a hostile state file turns
 * rom_revision into an out-of-bounds string read at the first strcmp. */
static void test_unterminated_fields_refused(void) {
    char path[512];
    uint8_t raw[SCRATCH_BYTES];
    uint8_t buffer[SCRATCH_BYTES];
    MdkrSaveStateHeader header;
    char err[ERR_SIZE];
    size_t size;
    size_t i;

    temp_path(path, sizeof(path), "unterminated.mdkrstate");
    size = write_fixture(path, raw, sizeof(raw));
    CHECK(size == OFF_PAYLOAD + PAYLOAD_BYTES);

    for (i = 0u; i < 16u; i++) {
        raw[OFF_ROM_REVISION + i] = (uint8_t)'z';
    }
    CHECK(spill(path, raw, size) == 0);
    err[0] = '\0';
    CHECK(mdkr_save_state_read(path, &header, buffer, sizeof(buffer), err,
                               sizeof(err)) != 0);
    CHECK(err[0] != '\0');

    remove(path);
}

/* ---------------------------------------------------------------- main */

typedef void (*TestFn)(void);

static int run(const char *name, TestFn fn, unsigned index) {
    int before = failures;
    fn();
    if (failures == before) {
        printf("ok %u - %s\n", index, name);
        return 0;
    }
    printf("FAIL %u - %s (%d checks failed)\n", index, name, failures - before);
    return 1;
}

int main(void) {
    char template_dir[] = "/tmp/mdkr_save_state_test_XXXXXX";
    const char *made;
    unsigned i;
    unsigned truncation_iterations;
    int before;

    for (i = 0u; i < PAYLOAD_BYTES; i++) {
        g_payload[i] = (uint8_t)(i * 31u + 11u);
    }

    made = mkdtemp(template_dir);
    if (made == NULL) {
        fprintf(stderr, "could not create a temporary directory under /tmp\n");
        return 1;
    }
    snprintf(g_dir, sizeof(g_dir), "%s", made);

    run("header round-trips every field", test_round_trip, 1u);
    run("wrong magic is refused, reason says 'format'", test_wrong_magic, 2u);
    run("ROM revision mismatch names both revisions",
        test_rom_revision_mismatch, 3u);
    run("app version mismatch names both versions", test_app_version_mismatch,
        4u);

    before = failures;
    truncation_iterations = test_truncation_at_every_offset();
    if (failures == before) {
        printf("ok 5 - truncation refused cleanly at every offset (%u offsets "
               "plus the empty file)\n",
               truncation_iterations);
    } else {
        printf("FAIL 5 - truncation sweep (%u offsets, %d checks failed)\n",
               truncation_iterations, failures - before);
    }

    run("payload CRC mismatch is refused", test_payload_crc_mismatch, 6u);
    run("payload larger than the buffer, and lying headers, are refused",
        test_payload_larger_than_buffer, 7u);
    run("format version is checked, reason names both versions",
        test_format_version_checked, 8u);

    before = failures;
    test_crc32_is_standard();
    test_argument_guards();
    test_unterminated_fields_refused();
    printf("%s extra - CRC-32 pinned, argument guards, unterminated fields\n",
           (failures == before) ? "ok" : "FAIL");

    if (rmdir(g_dir) != 0) {
        fprintf(stderr, "temporary directory %s was not left empty\n", g_dir);
        failures++;
    }

    if (failures != 0) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall save-state container checks passed\n");
    return 0;
}
