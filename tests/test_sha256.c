#include <stdio.h>
#include <string.h>

#include "sha256.h"

/* Two-block and three-block FIPS 180-2 vectors: the message length crosses the
 * 64-byte block boundary and the length padding lands in its own block. */
static const char two_block[] =
    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
static const char three_block[] =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
    "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
static const char two_block_hex[] =
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
static const char three_block_hex[] =
    "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1";
static const char million_a_hex[] =
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

static void hex_of(const uint8_t digest[MDKR_SHA256_DIGEST_SIZE],
                   char out[MDKR_SHA256_HEX_SIZE]) {
    size_t i;
    for (i = 0; i < MDKR_SHA256_DIGEST_SIZE; i++) {
        snprintf(out + i * 2u, 3u, "%02x", digest[i]);
    }
}

static int check(const char *input, const char *expected) {
    char actual[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(input, strlen(input), actual);
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "sha256(%s): got %s, expected %s\n", input, actual, expected);
        return 1;
    }
    return 0;
}

/* Uneven incremental updates must agree with the one-shot digest: chunk
 * boundaries never align with the 64-byte compression block. */
static int check_incremental(const char *input, const char *expected) {
    MdkrSha256 context;
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    char actual[MDKR_SHA256_HEX_SIZE];
    size_t length = strlen(input);
    size_t offset = 0;
    size_t chunk = 1;
    mdkr_sha256_init(&context);
    while (offset < length) {
        size_t take = length - offset < chunk ? length - offset : chunk;
        mdkr_sha256_update(&context, input + offset, take);
        offset += take;
        chunk = chunk * 2u + 1u;
    }
    mdkr_sha256_final(&context, digest);
    hex_of(digest, actual);
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "incremental sha256: got %s, expected %s\n", actual,
                expected);
        return 1;
    }
    return 0;
}

static int check_repeated_million(void) {
    MdkrSha256 context;
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    char actual[MDKR_SHA256_HEX_SIZE];
    char block[1000];
    unsigned i;
    memset(block, 'a', sizeof(block));
    mdkr_sha256_init(&context);
    for (i = 0; i < 1000u; i++) {
        mdkr_sha256_update(&context, block, sizeof(block));
    }
    mdkr_sha256_final(&context, digest);
    hex_of(digest, actual);
    if (strcmp(actual, million_a_hex) != 0) {
        fprintf(stderr, "sha256(1e6 x 'a'): got %s, expected %s\n", actual,
                million_a_hex);
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += check("", "e3b0c44298fc1c149afbf4c8996fb924"
                          "27ae41e4649b934ca495991b7852b855");
    failures += check("abc", "ba7816bf8f01cfea414140de5dae2223"
                             "b00361a396177a9cb410ff61f20015ad");
    failures += check(two_block, two_block_hex);
    failures += check(three_block, three_block_hex);
    failures += check_incremental("", "e3b0c44298fc1c149afbf4c8996fb924"
                                      "27ae41e4649b934ca495991b7852b855");
    failures += check_incremental(two_block, two_block_hex);
    failures += check_incremental(three_block, three_block_hex);
    failures += check_repeated_million();
    return failures != 0;
}
