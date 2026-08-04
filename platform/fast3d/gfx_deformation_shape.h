/* Checked command-stream shape arithmetic shared by packet capture and its
 * ROM-free regression. `size_limit` is normally SIZE_MAX; making it explicit
 * lets the test exercise wasm32's overflow branch on a 64-bit host. */
#ifndef MDKR_GFX_DEFORMATION_SHAPE_H
#define MDKR_GFX_DEFORMATION_SHAPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool gfx_deformation_shape_matches(uint32_t count,
                                                 uint32_t stride,
                                                 size_t byte_size,
                                                 size_t size_limit) {
    if (count == 0u || stride == 0u || byte_size > size_limit ||
        (size_t)count > size_limit / (size_t)stride) {
        return false;
    }
    return (size_t)count * (size_t)stride == byte_size;
}

#endif /* MDKR_GFX_DEFORMATION_SHAPE_H */
