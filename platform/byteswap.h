/**
 * byteswap.h — Compatibility wrappers for big-endian N64 serialized data.
 *
 * The raw bswap16/bswap32 helpers always reverse bytes. The read_be* and
 * GE_SWAP* conversion APIs instead produce host-native values on either host
 * byte order and tolerate unaligned serialized fields.
 */
#ifndef _PLATFORM_BYTESWAP_H_
#define _PLATFORM_BYTESWAP_H_

#include <stdint.h>

#ifdef NATIVE_PORT

#include "endian_utils.h"

static inline uint32_t bswap32(uint32_t x) {
    return ((x & UINT32_C(0x000000FF)) << 24) |
           ((x & UINT32_C(0x0000FF00)) << 8) |
           ((x & UINT32_C(0x00FF0000)) >> 8) |
           ((x & UINT32_C(0xFF000000)) >> 24);
}

static inline uint16_t bswap16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

/* Read a big-endian 32-bit value from a memory address */
static inline int32_t read_be32(const void *p) {
    return mdkr_read_be_s32(p);
}

/* Read a big-endian 16-bit value from a memory address */
static inline int16_t read_be16(const void *p) {
    return mdkr_read_be_s16(p);
}

/* Read a big-endian 32-bit unsigned value */
static inline uint32_t read_be32u(const void *p) {
    return mdkr_read_be_u32(p);
}

/* ===== Type-safe swap macros (C11 _Generic) =====
 *
 * GE_SWAPPED(x)  — return host-native copy of a big-endian encoded scalar
 * GE_SWAP(x)     — in-place: x = GE_SWAPPED(x)
 *
 * Uses _Generic to dispatch to the correct width automatically.
 * Catches type mismatches at compile time instead of silent truncation. */

#define GE_SWAPPED(x) _Generic((x),                    \
    uint32_t: mdkr_be_encoded_u32_to_host,               \
    int32_t:  mdkr_be_encoded_s32_to_host,               \
    uint16_t: mdkr_be_encoded_u16_to_host,               \
    int16_t:  mdkr_be_encoded_s16_to_host,               \
    float:    mdkr_be_encoded_f32_to_host                 \
)(x)

#define GE_SWAP(x)  ((x) = GE_SWAPPED(x))

#else
/* On N64 (big-endian), no swapping needed */
#define read_be32(p) (*(const int32_t *)(p))
#define read_be16(p) (*(const int16_t *)(p))
#define read_be32u(p) (*(const uint32_t *)(p))
#define GE_SWAPPED(x) (x)
#define GE_SWAP(x)    ((void)0)
#endif

#endif /* _PLATFORM_BYTESWAP_H_ */
