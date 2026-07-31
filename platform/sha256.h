#ifndef MDKR_SHA256_H
#define MDKR_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define MDKR_SHA256_DIGEST_SIZE 32
#define MDKR_SHA256_HEX_SIZE 65

typedef struct {
    uint32_t state[8];
    uint64_t totalBytes;
    uint8_t block[64];
    size_t blockSize;
} MdkrSha256;

void mdkr_sha256_init(MdkrSha256 *ctx);
void mdkr_sha256_update(MdkrSha256 *ctx, const void *data, size_t size);
void mdkr_sha256_final(MdkrSha256 *ctx, uint8_t digest[MDKR_SHA256_DIGEST_SIZE]);
void mdkr_sha256_hex(const void *data, size_t size, char hex[MDKR_SHA256_HEX_SIZE]);

#endif
