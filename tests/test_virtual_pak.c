#include "virtual_pak.h"
#include "sha256.h"

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

static void name_for(
    unsigned index, uint8_t name[MDKR_VPAK_NAME_SIZE],
    uint8_t ext[MDKR_VPAK_EXT_SIZE]) {
    memset(name, 0, MDKR_VPAK_NAME_SIZE);
    memset(ext, 0, MDKR_VPAK_EXT_SIZE);
    name[0] = (uint8_t)('A' + index);
    ext[0] = (uint8_t)('0' + index % 10);
}

static void resign(uint8_t image[MDKR_VPAK_IMAGE_SIZE]) {
    MdkrSha256 sha;
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    memset(image + 32, 0, MDKR_SHA256_DIGEST_SIZE);
    mdkr_sha256_init(&sha);
    mdkr_sha256_update(&sha, image, MDKR_VPAK_IMAGE_SIZE);
    mdkr_sha256_final(&sha, digest);
    memcpy(image + 32, digest, sizeof(digest));
}

static void test_round_trip_and_ranges(void) {
    MdkrVirtualPak pak;
    MdkrVirtualPak decoded;
    MdkrVirtualPakFile state;
    uint8_t image[MDKR_VPAK_IMAGE_SIZE];
    uint8_t name[MDKR_VPAK_NAME_SIZE];
    uint8_t ext[MDKR_VPAK_EXT_SIZE];
    uint8_t written[512];
    uint8_t readback[512];
    int file_number = -1;
    size_t i;

    mdkr_virtual_pak_init(&pak);
    CHECK(mdkr_virtual_pak_free_bytes(&pak) == MDKR_VPAK_CAPACITY);
    CHECK(mdkr_virtual_pak_encode(&pak, image, sizeof(image)) == MDKR_VPAK_OK);
    memset(&decoded, 0xA5, sizeof(decoded));
    CHECK(mdkr_virtual_pak_decode(image, sizeof(image), &decoded) ==
          MDKR_VPAK_OK);
    CHECK(decoded.payload_used == 0);

    name_for(0, name, ext);
    CHECK(mdkr_virtual_pak_allocate(
              &pak, 0x3459, 0x4E445945, name, ext, 0x6700, &file_number) ==
          MDKR_VPAK_OK);
    CHECK(file_number == 0);
    CHECK(pak.files[0].size == 0x6700);
    CHECK(mdkr_virtual_pak_free_bytes(&pak) ==
          MDKR_VPAK_CAPACITY - 0x6700);
    for (i = 0; i < sizeof(written); i++) written[i] = (uint8_t)(i * 37u);
    CHECK(mdkr_virtual_pak_write(
              &pak, file_number, 0x6700 - sizeof(written), written,
              sizeof(written)) == MDKR_VPAK_OK);
    memset(readback, 0, sizeof(readback));
    CHECK(mdkr_virtual_pak_read(
              &pak, file_number, 0x6700 - sizeof(readback), readback,
              sizeof(readback)) == MDKR_VPAK_OK);
    CHECK(memcmp(readback, written, sizeof(written)) == 0);
    memcpy(readback, written, sizeof(readback));
    CHECK(mdkr_virtual_pak_read(
              &pak, file_number, 0x6700 - sizeof(readback) + 1, readback,
              sizeof(readback)) == MDKR_VPAK_ERR_RANGE);
    CHECK(memcmp(readback, written, sizeof(readback)) == 0);
    CHECK(mdkr_virtual_pak_file_state(&pak, file_number, &state) ==
          MDKR_VPAK_OK);
    CHECK(state.size == 0x6700);
}

static void test_delete_reuse_and_capacity(void) {
    MdkrVirtualPak pak;
    MdkrVirtualPak decoded;
    uint8_t image[MDKR_VPAK_IMAGE_SIZE];
    uint8_t names[MDKR_VPAK_MAX_FILES][MDKR_VPAK_NAME_SIZE];
    uint8_t exts[MDKR_VPAK_MAX_FILES][MDKR_VPAK_EXT_SIZE];
    int file_number;
    int i;

    mdkr_virtual_pak_init(&pak);
    for (i = 0; i < MDKR_VPAK_MAX_FILES; i++) {
        name_for((unsigned)i, names[i], exts[i]);
        CHECK(mdkr_virtual_pak_allocate(
                  &pak, 1, (uint32_t)i, names[i], exts[i], 1,
                  &file_number) == MDKR_VPAK_OK);
        CHECK(file_number == i);
    }
    name_for(16, names[0], exts[0]);
    CHECK(mdkr_virtual_pak_allocate(
              &pak, 1, 16, names[0], exts[0], 1, NULL) ==
          MDKR_VPAK_ERR_DIR_FULL);

    name_for(5, names[5], exts[5]);
    CHECK(mdkr_virtual_pak_delete(
              &pak, 1, 5, names[5], exts[5]) == MDKR_VPAK_OK);
    name_for(16, names[0], exts[0]);
    CHECK(mdkr_virtual_pak_allocate(
              &pak, 1, 16, names[0], exts[0], 513, &file_number) ==
          MDKR_VPAK_OK);
    CHECK(file_number == 5);
    CHECK(pak.files[file_number].size == 768);
    CHECK(mdkr_virtual_pak_encode(&pak, image, sizeof(image)) == MDKR_VPAK_OK);
    CHECK(mdkr_virtual_pak_decode(image, sizeof(image), &decoded) ==
          MDKR_VPAK_OK);
    CHECK(decoded.payload_used == pak.payload_used);
    CHECK(mdkr_virtual_pak_used_files(&decoded) == MDKR_VPAK_MAX_FILES);

    mdkr_virtual_pak_init(&pak);
    name_for(0, names[0], exts[0]);
    CHECK(mdkr_virtual_pak_allocate(
              &pak, 1, 0, names[0], exts[0], MDKR_VPAK_CAPACITY, NULL) ==
          MDKR_VPAK_OK);
    name_for(1, names[1], exts[1]);
    CHECK(mdkr_virtual_pak_allocate(
              &pak, 1, 1, names[1], exts[1], 1, NULL) ==
          MDKR_VPAK_ERR_DATA_FULL);
}

static void test_corrupt_images_are_atomic(void) {
    MdkrVirtualPak pak;
    MdkrVirtualPak output;
    MdkrVirtualPak before;
    uint8_t image[MDKR_VPAK_IMAGE_SIZE];
    uint8_t name[MDKR_VPAK_NAME_SIZE];
    uint8_t ext[MDKR_VPAK_EXT_SIZE];

    mdkr_virtual_pak_init(&pak);
    name_for(0, name, ext);
    CHECK(mdkr_virtual_pak_allocate(
              &pak, 1, 2, name, ext, 256, NULL) == MDKR_VPAK_OK);
    CHECK(mdkr_virtual_pak_encode(&pak, image, sizeof(image)) == MDKR_VPAK_OK);
    memset(&output, 0xCC, sizeof(output));
    before = output;

    image[MDKR_VPAK_IMAGE_SIZE - 1] ^= 0x80;
    CHECK(mdkr_virtual_pak_decode(image, sizeof(image), &output) ==
          MDKR_VPAK_ERR_DIGEST);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    image[MDKR_VPAK_IMAGE_SIZE - 1] ^= 0x80;

    /* Authenticate a structurally invalid extent. The decoder must validate
     * the directory after integrity, not treat SHA-256 as a bounds proof. */
    image[64 + 12] = 0x00;
    image[64 + 13] = 0x00;
    image[64 + 14] = 0x80;
    image[64 + 15] = 0x00;
    resign(image);
    CHECK(mdkr_virtual_pak_decode(image, sizeof(image), &output) ==
          MDKR_VPAK_ERR_FORMAT);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    CHECK(mdkr_virtual_pak_decode(
              image, sizeof(image) - 1, &output) == MDKR_VPAK_ERR_FORMAT);
}

int main(void) {
    test_round_trip_and_ranges();
    test_delete_reuse_and_capacity();
    test_corrupt_images_are_atomic();
    if (failures != 0) {
        fprintf(stderr, "virtual_pak: %d failure(s)\n", failures);
        return 1;
    }
    puts("virtual_pak: PASS");
    return 0;
}
