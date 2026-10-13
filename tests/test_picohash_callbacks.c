/* Known-answer hash/HMAC vectors also exercise reset and exact generic
 * callback signatures under Clang's function sanitizer. */
#include "picohash.h"
#include <stdio.h>

struct Vector { void (*init)(picohash_ctx_t *); const char *hash, *hmac, *long_hmac; };
static const struct Vector vectors[] = {
    {picohash_init_md5, "900150983cd24fb0d6963f7d28e17f72", "d2fe98063f876b03193afb49b4979591", "3f089f35f9784d3a29ac4deff0131d7b"},
    {picohash_init_sha1, "a9993e364706816aba3e25717850c26c9cd0d89d", "4fd0b215276ef12f2b3e4c8ecac2811498b656fc", "26030f686a1d565e024767e62c960a6434855442"},
    {picohash_init_sha224, "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7", "f524670b7e34f31467de0aa96593861cf65117d414fb2d86158d760e", "239098ddfc7c4670f73e5e946863d2888d2e92ee022e30501bbebca4"},
    {picohash_init_sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "9c196e32dc0175f86f4b1cb89289d6619de6bee699e4c378e68309ed97a1a6ab", "b58b2b694fdba0dd76da3ebe99174f728d327560f36ece224e90867972479922"},
};
static int matches(const unsigned char *digest, size_t len, const char *hex) {
    static const char digits[] = "0123456789abcdef";
    if (strlen(hex) != len * 2) return 0;
    for (size_t i = 0; i < len; ++i)
        if (hex[i*2] != digits[digest[i] >> 4] || hex[i*2+1] != digits[digest[i] & 15]) return 0;
    return 1;
}
int main(void) {
    unsigned char digest[PICOHASH_MAX_DIGEST_LENGTH];
    unsigned char long_key[100]; memset(long_key, 'k', sizeof(long_key));
    for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
        for (int mode = 0; mode < 3; ++mode) {
            picohash_ctx_t ctx;
            const char *expected;
            if (mode == 0) { vectors[i].init(&ctx); expected = vectors[i].hash; }
            else if (mode == 1) { picohash_init_hmac(&ctx, vectors[i].init, "key", 3); expected = vectors[i].hmac; }
            else { picohash_init_hmac(&ctx, vectors[i].init, long_key, sizeof(long_key)); expected = vectors[i].long_hmac; }
            for (int repeat = 0; repeat < 2; ++repeat) {
                picohash_update(&ctx, "a", 1); picohash_update(&ctx, "bc", 2);
                picohash_final(&ctx, digest);
                if (!matches(digest, ctx.digest_length, expected)) {
                    fprintf(stderr, "hash callback vector %zu mode %d repeat %d failed\n", i, mode, repeat); return 1;
                }
                picohash_reset(&ctx);
            }
        }
    }
    puts("picohash callbacks: all hash/HMAC/reset vectors passed");
    return 0;
}
