#include "sha256.h"

#include <string.h>

static const uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotate_right(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

static uint32_t read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void write_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void transform(MdkrSha256 *ctx, const uint8_t block[64]) {
    uint32_t words[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned i;

    for (i = 0; i < 16; i++) {
        words[i] = read_be32(block + i * 4u);
    }
    for (; i < 64; i++) {
        uint32_t s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^
                      (words[i - 15] >> 3);
        uint32_t s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^
                      (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];
    for (i = 0; i < 64; i++) {
        uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t t1 = h + sum1 + choose + kRoundConstants[i] + words[i];
        uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void mdkr_sha256_init(MdkrSha256 *ctx) {
    static const uint32_t initialState[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    memcpy(ctx->state, initialState, sizeof(initialState));
    ctx->totalBytes = 0;
    ctx->blockSize = 0;
}

void mdkr_sha256_update(MdkrSha256 *ctx, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    ctx->totalBytes += size;
    while (size > 0) {
        size_t available = sizeof(ctx->block) - ctx->blockSize;
        size_t take = size < available ? size : available;
        memcpy(ctx->block + ctx->blockSize, bytes, take);
        ctx->blockSize += take;
        bytes += take;
        size -= take;
        if (ctx->blockSize == sizeof(ctx->block)) {
            transform(ctx, ctx->block);
            ctx->blockSize = 0;
        }
    }
}

void mdkr_sha256_final(MdkrSha256 *ctx, uint8_t digest[MDKR_SHA256_DIGEST_SIZE]) {
    uint64_t totalBits = ctx->totalBytes * 8u;
    unsigned i;

    ctx->block[ctx->blockSize++] = 0x80;
    if (ctx->blockSize > 56) {
        memset(ctx->block + ctx->blockSize, 0, sizeof(ctx->block) - ctx->blockSize);
        transform(ctx, ctx->block);
        ctx->blockSize = 0;
    }
    memset(ctx->block + ctx->blockSize, 0, 56 - ctx->blockSize);
    for (i = 0; i < 8; i++) {
        ctx->block[63 - i] = (uint8_t)(totalBits >> (i * 8u));
    }
    transform(ctx, ctx->block);
    for (i = 0; i < 8; i++) {
        write_be32(digest + i * 4u, ctx->state[i]);
    }
    memset(ctx, 0, sizeof(*ctx));
}

void mdkr_sha256_hex(const void *data, size_t size, char hex[MDKR_SHA256_HEX_SIZE]) {
    static const char digits[] = "0123456789abcdef";
    MdkrSha256 ctx;
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    unsigned i;

    mdkr_sha256_init(&ctx);
    mdkr_sha256_update(&ctx, data, size);
    mdkr_sha256_final(&ctx, digest);
    for (i = 0; i < MDKR_SHA256_DIGEST_SIZE; i++) {
        hex[i * 2u] = digits[digest[i] >> 4];
        hex[i * 2u + 1u] = digits[digest[i] & 0x0fu];
    }
    hex[MDKR_SHA256_HEX_SIZE - 1] = '\0';
}
