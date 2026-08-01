#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "asset_swap.h"

#define CHEAT_COUNT 29u
#define INDEX_BYTES ((1u + CHEAT_COUNT * 3u) * 2u)
#define BLOB_SIZE 384u

static int g_failures;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr);                 \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void write_be16(uint8_t *blob, uint32_t offset, uint16_t value) {
    blob[offset] = (uint8_t)(value >> 8);
    blob[offset + 1u] = (uint8_t)value;
}

static uint16_t read_host16(const uint8_t *blob, uint32_t offset) {
    uint16_t value;
    memcpy(&value, blob + offset, sizeof(value));
    return value;
}

static void put_string(uint8_t *blob, uint16_t offset, const char *text) {
    memcpy(blob + offset, text, strlen(text) + 1u);
}

static void make_decrypted_be_table(uint8_t blob[BLOB_SIZE]) {
    uint32_t i;

    memset(blob, 0, BLOB_SIZE);
    write_be16(blob, 0, CHEAT_COUNT);
    for (i = 0; i < CHEAT_COUNT * 3u; i++) {
        write_be16(blob, 2u + i * 2u, INDEX_BYTES);
    }

    /* Preserve the retail us.v80 offsets involved in the reported symptoms:
     * without a host-endian normalization 29 becomes 7424, while the two
     * default unlocked-code descriptions at 187/220 become 47872/56320. */
    write_be16(blob, 2u + 1u * 2u, 187u);
    write_be16(blob, 2u + 3u * 2u, 209u);
    write_be16(blob, 2u + 4u * 2u, 220u);
    write_be16(blob, 2u + 12u * 2u, 317u); /* cheat 4 code */
    write_be16(blob, 2u + 13u * 2u, 324u); /* cheat 4 description */

    put_string(blob, INDEX_BYTES, "1058732594");
    put_string(blob, 187u, "Control T.T.");
    put_string(blob, 209u, "8649305321");
    put_string(blob, 220u, "Control Drumstick");
    put_string(blob, 317u, "ARNOLD");
    put_string(blob, 324u, "Big Characters");
}

static const char *table_string(const uint8_t *blob, uint32_t entry) {
    return (const char *)blob + read_host16(blob, 2u + entry * 2u);
}

static int find_code(const uint8_t *blob, const char *input) {
    uint16_t count = read_host16(blob, 0);
    uint16_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(table_string(blob, (uint32_t)i * 3u), input) == 0) {
            return i;
        }
    }
    return -1;
}

static int host_is_little_endian(void) {
    const uint16_t value = 1u;
    return *(const uint8_t *)&value == 1u;
}

static void test_reported_endian_regression(void) {
    uint8_t blob[BLOB_SIZE];

    make_decrypted_be_table(blob);
    if (host_is_little_endian()) {
        /* Positive control: these are the exact misreads behind the issue.
         * Do not dereference the bogus offsets; the historical game did and
         * rendered unrelated arena bytes as "8" / blank strings. */
        CHECK(read_host16(blob, 0) == 7424u);
        CHECK(read_host16(blob, 4u) == 47872u);
        CHECK(read_host16(blob, 10u) == 56320u);
    }

    CHECK(asset_swap_misc_magic_codes(blob, sizeof(blob)) == 1);

    CHECK(read_host16(blob, 0) == CHEAT_COUNT);
    CHECK(read_host16(blob, 4u) == 187u);
    CHECK(read_host16(blob, 10u) == 220u);
    CHECK(strcmp(table_string(blob, 1u), "Control T.T.") == 0);
    CHECK(strcmp(table_string(blob, 4u), "Control Drumstick") == 0);
    CHECK(find_code(blob, "ARNOLD") == 4);
    CHECK(strcmp(table_string(blob, 13u), "Big Characters") == 0);
    CHECK(find_code(blob, "NOTACODE") == -1);
}

static void test_damaged_offset_fails_closed(void) {
    uint8_t blob[BLOB_SIZE];
    uint8_t original[BLOB_SIZE];

    make_decrypted_be_table(blob);
    /* A non-first entry used to escape the self-describing first-offset check.
     * Pin it outside the payload and require an all-or-nothing rollback. */
    write_be16(blob, 2u + 13u * 2u, BLOB_SIZE);
    memcpy(original, blob, sizeof(blob));

    CHECK(asset_swap_misc_magic_codes(blob, sizeof(blob)) == 0);
    CHECK(memcmp(blob, original, sizeof(blob)) == 0);
}

static void test_unterminated_string_fails_closed(void) {
    uint8_t blob[BLOB_SIZE];
    uint8_t original[BLOB_SIZE];
    uint32_t i;

    make_decrypted_be_table(blob);
    write_be16(blob, 2u + 13u * 2u, BLOB_SIZE - 4u);
    for (i = BLOB_SIZE - 4u; i < BLOB_SIZE; i++) {
        blob[i] = 'X';
    }
    memcpy(original, blob, sizeof(blob));

    CHECK(asset_swap_misc_magic_codes(blob, sizeof(blob)) == 0);
    CHECK(memcmp(blob, original, sizeof(blob)) == 0);
}

int main(void) {
    test_reported_endian_regression();
    test_damaged_offset_fails_closed();
    test_unterminated_string_fails_closed();

    if (g_failures != 0) {
        fprintf(stderr, "magic-code tests: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("magic-code tests: PASS");
    return 0;
}
