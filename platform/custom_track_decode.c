#include "custom_track_decode.h"
#include "miniz.h"
#include <stdint.h>

int mdkr_custom_track_decode(const void *input, size_t input_size,
                             void *output, size_t capacity, size_t *written) {
    const unsigned char *bytes = input;
    if (!written) return 0;
    *written = 0;
    if (!bytes || !output || input_size <= 5) return 0;
    uint32_t expected = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    if (!expected || expected > INT32_MAX || expected > capacity) return 0;
    size_t actual = tinfl_decompress_mem_to_mem(output, expected, bytes + 5,
                                               input_size - 5, 0);
    if (actual != expected) return 0;
    *written = actual;
    return 1;
}
