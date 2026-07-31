/**
 * endian_utils.h — unaligned-safe readers for N64 big-endian serialized data.
 *
 * These helpers describe the byte order of the DATA, not the host. Building the
 * value one byte at a time keeps the result correct on little- and big-endian
 * CPUs and avoids undefined behaviour when a serialized field is unaligned.
 */
#ifndef MDKR_ENDIAN_UTILS_H
#define MDKR_ENDIAN_UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint16_t mdkr_read_be_u16(const void *address) {
    const uint8_t *bytes = (const uint8_t *)address;
    return (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
}

static inline int16_t mdkr_read_be_s16(const void *address) {
    return (int16_t)mdkr_read_be_u16(address);
}

static inline uint32_t mdkr_read_be_u32(const void *address) {
    const uint8_t *bytes = (const uint8_t *)address;
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static inline uint64_t mdkr_read_be_u64(const void *address) {
    const uint8_t *bytes = (const uint8_t *)address;
    return ((uint64_t)bytes[0] << 56) |
           ((uint64_t)bytes[1] << 48) |
           ((uint64_t)bytes[2] << 40) |
           ((uint64_t)bytes[3] << 32) |
           ((uint64_t)bytes[4] << 24) |
           ((uint64_t)bytes[5] << 16) |
           ((uint64_t)bytes[6] << 8) |
           (uint64_t)bytes[7];
}

static inline void mdkr_write_be_u64(void *address, uint64_t value) {
    uint8_t *bytes = (uint8_t *)address;
    bytes[0] = (uint8_t)(value >> 56);
    bytes[1] = (uint8_t)(value >> 48);
    bytes[2] = (uint8_t)(value >> 40);
    bytes[3] = (uint8_t)(value >> 32);
    bytes[4] = (uint8_t)(value >> 24);
    bytes[5] = (uint8_t)(value >> 16);
    bytes[6] = (uint8_t)(value >> 8);
    bytes[7] = (uint8_t)value;
}

static inline int32_t mdkr_read_be_s32(const void *address) {
    return (int32_t)mdkr_read_be_u32(address);
}

static inline float mdkr_read_be_f32(const void *address) {
    uint32_t bits = mdkr_read_be_u32(address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/*
 * Convert a buffer of big-endian signed 16-bit PCM samples into host-native
 * representation. Source and destination may be unaligned. The copy is
 * transactional with respect to argument validation: invalid sizes/pointers
 * return 0 without touching the destination.
 */
static inline int mdkr_copy_be16_to_host(
    void *destination,
    size_t destination_size,
    const void *source,
    size_t source_size
) {
    const uint8_t *src;
    uint8_t *dst;
    size_t offset;

    if (source_size == 0) {
        return 1;
    }
    if (destination == NULL || source == NULL ||
        (source_size & 1u) != 0u || destination_size < source_size) {
        return 0;
    }

    src = (const uint8_t *)source;
    dst = (uint8_t *)destination;
    for (offset = 0; offset < source_size; offset += sizeof(uint16_t)) {
        uint16_t sample = mdkr_read_be_u16(src + offset);
        memcpy(dst + offset, &sample, sizeof(sample));
    }
    return 1;
}

/*
 * A scalar copied directly out of a big-endian image contains encoded bytes in
 * its object representation. Reading the local copy as BE converts it without
 * making any assumption about host byte order. These preserve GE_SWAPPED's
 * value-expression API while making it correct on native big-endian hosts.
 */
static inline uint16_t mdkr_be_encoded_u16_to_host(uint16_t value) {
    return mdkr_read_be_u16(&value);
}

static inline int16_t mdkr_be_encoded_s16_to_host(int16_t value) {
    return mdkr_read_be_s16(&value);
}

static inline uint32_t mdkr_be_encoded_u32_to_host(uint32_t value) {
    return mdkr_read_be_u32(&value);
}

static inline int32_t mdkr_be_encoded_s32_to_host(int32_t value) {
    return mdkr_read_be_s32(&value);
}

static inline float mdkr_be_encoded_f32_to_host(float value) {
    return mdkr_read_be_f32(&value);
}

#endif /* MDKR_ENDIAN_UTILS_H */
