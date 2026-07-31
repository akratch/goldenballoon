#include "byteswap.h"
#include "endian_utils.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void expect_u32(const char *name, uint32_t actual, uint32_t expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL %-35s actual=0x%08x expected=0x%08x\n",
                name, (unsigned)actual, (unsigned)expected);
        s_failures++;
    }
}

static void expect_u64(const char *name, uint64_t actual, uint64_t expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL %-35s actual=0x%016llx expected=0x%016llx\n",
                name, (unsigned long long)actual, (unsigned long long)expected);
        s_failures++;
    }
}

static void expect_s32(const char *name, int32_t actual, int32_t expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL %-35s actual=%d expected=%d\n",
                name, (int)actual, (int)expected);
        s_failures++;
    }
}

static int16_t load_host_s16(const void *address) {
    int16_t value;
    memcpy(&value, address, sizeof(value));
    return value;
}

int main(void) {
    static const uint8_t fields[] = {
        0xA5,
        0x12, 0x34,
        0x80, 0x00,
        0x01, 0x02, 0x03, 0x04,
        0x3F, 0x80, 0x00, 0x00,
        0x5A
    };
    static const uint8_t pcm_with_pad[] = {
        0xA5,
        0x00, 0x01,
        0xFF, 0xFE,
        0x80, 0x00,
        0x7F, 0xFF,
        0x5A
    };
    static const uint8_t be_u64_with_pad[] = {
        0xA5,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
        0x5A
    };
    uint8_t output[10];
    uint8_t before[sizeof(output)];
    uint16_t encoded_u16;
    int16_t encoded_s16;
    uint32_t encoded_u32;
    int32_t encoded_s32;
    float encoded_f32;
    float converted_f32;

    /* Every field starts at an intentionally odd address. */
    expect_u32("unaligned BE u16", mdkr_read_be_u16(fields + 1), 0x1234u);
    expect_s32("unaligned BE s16", mdkr_read_be_s16(fields + 3), -32768);
    expect_u32("unaligned BE u32", mdkr_read_be_u32(fields + 5), 0x01020304u);
    expect_s32("unaligned BE s32", mdkr_read_be_s32(fields + 3),
               (int32_t)0x80000102u);
    expect_true("unaligned BE f32",
                fabsf(mdkr_read_be_f32(fields + 9) - 1.0f) < 0.000001f);
    expect_u64("unaligned BE u64", mdkr_read_be_u64(be_u64_with_pad + 1),
               UINT64_C(0x0123456789ABCDEF));
    memset(output, 0xCC, sizeof(output));
    mdkr_write_be_u64(output + 1, UINT64_C(0x0123456789ABCDEF));
    expect_true("unaligned BE u64 write",
                memcmp(output + 1, be_u64_with_pad + 1, 8) == 0 &&
                output[0] == 0xCC && output[9] == 0xCC);

    memcpy(&encoded_u16, fields + 1, sizeof(encoded_u16));
    memcpy(&encoded_s16, fields + 3, sizeof(encoded_s16));
    memcpy(&encoded_u32, fields + 5, sizeof(encoded_u32));
    memcpy(&encoded_s32, fields + 3, sizeof(encoded_s32));
    memcpy(&encoded_f32, fields + 9, sizeof(encoded_f32));
    expect_u32("GE_SWAPPED u16", GE_SWAPPED(encoded_u16), 0x1234u);
    expect_s32("GE_SWAPPED s16", GE_SWAPPED(encoded_s16), -32768);
    expect_u32("GE_SWAPPED u32", GE_SWAPPED(encoded_u32), 0x01020304u);
    expect_s32("GE_SWAPPED s32", GE_SWAPPED(encoded_s32),
               (int32_t)0x80000102u);
    converted_f32 = GE_SWAPPED(encoded_f32);
    expect_true("GE_SWAPPED f32", fabsf(converted_f32 - 1.0f) < 0.000001f);

    GE_SWAP(encoded_u32);
    expect_u32("GE_SWAP in place", encoded_u32, 0x01020304u);
    expect_u32("raw bswap16 remains reversal", bswap16(0x1234u), 0x3412u);
    expect_u32("raw bswap32 remains reversal", bswap32(0x01020304u), 0x04030201u);

    memset(output, 0xCC, sizeof(output));
    expect_true("BE16 PCM conversion succeeds",
                mdkr_copy_be16_to_host(output + 1, 8, pcm_with_pad + 1, 8));
    expect_s32("PCM sample +1", load_host_s16(output + 1), 1);
    expect_s32("PCM sample -2", load_host_s16(output + 3), -2);
    expect_s32("PCM sample min", load_host_s16(output + 5), -32768);
    expect_s32("PCM sample max", load_host_s16(output + 7), 32767);
    expect_true("unaligned destination guards intact",
                output[0] == 0xCC && output[9] == 0xCC);

    memset(output, 0x5A, sizeof(output));
    memcpy(before, output, sizeof(output));
    expect_true("odd PCM length rejected",
                !mdkr_copy_be16_to_host(output, sizeof(output),
                                        pcm_with_pad + 1, 7));
    expect_true("odd rejection is transactional",
                memcmp(output, before, sizeof(output)) == 0);
    expect_true("short destination rejected",
                !mdkr_copy_be16_to_host(output, 6, pcm_with_pad + 1, 8));
    expect_true("null destination rejected",
                !mdkr_copy_be16_to_host(NULL, 8, pcm_with_pad + 1, 8));
    expect_true("null source rejected",
                !mdkr_copy_be16_to_host(output, sizeof(output), NULL, 8));
    expect_true("zero-byte null copy accepted",
                mdkr_copy_be16_to_host(NULL, 0, NULL, 0));

    if (s_failures != 0) {
        fprintf(stderr, "%d endian assertion(s) failed\n", s_failures);
        return EXIT_FAILURE;
    }
    puts("endian-utils: all serialized-read and PCM-copy assertions passed");
    return EXIT_SUCCESS;
}
