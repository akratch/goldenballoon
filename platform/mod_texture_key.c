/**
 * mod_texture_key.c — content digest for one decoded texture.
 *
 * The digest is a published contract, so the encoding here is spelled out
 * rather than inherited from the host. Two rules keep it identical everywhere:
 *
 *   Fields are fed to the hash one at a time, never as struct bytes.
 *   struct DkrTexCacheKey carries padding, padding is never assigned a value,
 *   and hashing it would make the same texture hash differently between runs,
 *   call sites and compilers. No sanitizer reports that read, so the only
 *   defence is to never make it.
 *
 *   Multi-byte values are written little-endian by shifting, not by copying
 *   their representation, so a big-endian host names textures the same way a
 *   little-endian one does.
 *
 * The payload length is not hashed as a field, and does not need to be: every
 * value ahead of it is fixed width, and SHA-256 already binds the total
 * message length, so a payload can never be confused with a longer or shorter
 * one.
 */
#include "mod_texture_key.h"

#include "sha256.h"

/* First 16 bytes of the SHA-256, as 32 hex characters plus NUL. */
#define MDKR_MOD_TEXTURE_DIGEST_BYTES 16

static void hash_u8(MdkrSha256 *context, uint8_t value) {
    mdkr_sha256_update(context, &value, sizeof(value));
}

static void hash_u16_le(MdkrSha256 *context, uint16_t value) {
    uint8_t bytes[2];

    bytes[0] = (uint8_t) (value & 0xffu);
    bytes[1] = (uint8_t) ((value >> 8) & 0xffu);
    mdkr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u32_le(MdkrSha256 *context, uint32_t value) {
    uint8_t bytes[4];
    int     index;

    for (index = 0; index < 4; index++) {
        bytes[index] = (uint8_t) ((value >> (index * 8)) & 0xffu);
    }
    mdkr_sha256_update(context, bytes, sizeof(bytes));
}

void mdkr_mod_texture_digest(const struct DkrTexCacheKey *key,
                             const uint8_t *texels, size_t texel_bytes,
                             char out_hex[33]) {
    static const char digits[] = "0123456789abcdef";
    MdkrSha256        context;
    uint8_t           digest[MDKR_SHA256_DIGEST_SIZE];
    int               index;

    if (out_hex == NULL) {
        return;
    }
    out_hex[0] = '\0';
    if (key == NULL) {
        return;
    }

    mdkr_sha256_init(&context);
    hash_u32_le(&context, MDKR_MOD_TEXTURE_DIGEST_VERSION);
    hash_u16_le(&context, key->width);
    hash_u16_le(&context, key->height);
    hash_u8(&context, key->fmt);
    hash_u8(&context, key->siz);
    hash_u8(&context, key->palette);
    hash_u32_le(&context, key->palette_hash);
    hash_u32_le(&context, key->palette_fmt);
    /* A caller with no payload to offer names the format alone rather than
     * reading through a null pointer. */
    if (texels != NULL && texel_bytes > 0) {
        mdkr_sha256_update(&context, texels, texel_bytes);
    }
    mdkr_sha256_final(&context, digest);

    for (index = 0; index < MDKR_MOD_TEXTURE_DIGEST_BYTES; index++) {
        out_hex[index * 2] = digits[digest[index] >> 4];
        out_hex[index * 2 + 1] = digits[digest[index] & 0x0fu];
    }
    out_hex[MDKR_MOD_TEXTURE_DIGEST_BYTES * 2] = '\0';
}
