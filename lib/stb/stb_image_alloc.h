/* First-party allocation admission for the pinned PNG decoder, not upstream
 * stb code. Positive requests retain the system allocator's behavior unless
 * the calling thread set a decode bound. */
#ifndef MDKR_STB_IMAGE_ALLOC_H
#define MDKR_STB_IMAGE_ALLOC_H

#include <stdint.h>
#include <stdlib.h>

/* Per-thread ceiling on any single decoder allocation; 0 means none. The
 * decoder's zlib stage grows its output until the stream ends, so without a
 * bound a few megabytes of crafted IDAT can demand gigabytes, whatever the
 * header declares. Defined by the translation unit that instantiates
 * stb_image (lib/stb/stb_image_impl.c, or a test's stand-in). Returns the
 * previous bound so a caller can restore it. */
#ifdef __cplusplus
extern "C" {
#endif
size_t mdkr_stbi_set_allocation_limit(size_t bytes);
size_t mdkr_stbi_allocation_limit(void);
#ifdef __cplusplus
}
#endif

/* The largest single allocation decoding a PNG with this header can
 * legitimately make: the inflated scanlines (doubled, because the zlib buffer
 * grows by doubling from the decoder's own estimate, which interlaced images
 * exceed), the converted output, and the concatenated IDAT bytes (bounded by
 * the file, doubled the same way), plus a margin for palettes and row
 * buffers. channels is what stbi_info() reports; is16 what
 * stbi_is_16_bit_from_memory() reports. Zero when the header is unusable. */
static inline size_t mdkr_stbi_png_allocation_bound(int width, int height,
                                                    int channels, int is16,
                                                    int req_comp,
                                                    size_t file_size) {
    uint64_t depth, row, scanlines, output, idat, bound;
    if (width <= 0 || height <= 0 || channels <= 0 || channels > 4 ||
        req_comp < 0 || req_comp > 4) {
        return 0u;
    }
    depth = is16 ? 2u : 1u;
    row = 1u + (uint64_t)width * (uint64_t)channels * depth;
    scanlines = 2u * row * (uint64_t)height;
    output = (uint64_t)width * (uint64_t)height * depth *
             (uint64_t)(req_comp > channels ? req_comp : channels);
    idat = 2u * (uint64_t)file_size;
    bound = scanlines;
    if (output > bound) bound = output;
    if (idat > bound) bound = idat;
    bound += 64u * 1024u;
#if SIZE_MAX < UINT64_MAX
    if (bound > (uint64_t)SIZE_MAX) return 0u;
#endif
    return (size_t)bound;
}

static inline void *mdkr_stbi_malloc(size_t size) {
    const size_t limit = mdkr_stbi_allocation_limit();
    return size != 0u && (limit == 0u || size <= limit) ? malloc(size) : NULL;
}

static inline void *mdkr_stbi_realloc(void *pointer, size_t size) {
    /* A refused resize must retain the original allocation for the decoder's
     * normal failure cleanup. Do not delegate realloc(pointer, 0), whose
     * ownership behavior varies between C runtimes. */
    const size_t limit = mdkr_stbi_allocation_limit();
    return size != 0u && (limit == 0u || size <= limit) ? realloc(pointer, size) : NULL;
}

/* Storage for the per-thread bound. Expand exactly once, in the translation
 * unit that instantiates stb_image. */
#if defined(_MSC_VER)
#define MDKR_STBI_THREAD_LOCAL __declspec(thread)
#else
#define MDKR_STBI_THREAD_LOCAL _Thread_local
#endif
#define MDKR_STBI_ALLOCATION_LIMIT_STORAGE                                   \
    static MDKR_STBI_THREAD_LOCAL size_t mdkr_stbi_thread_limit;            \
    size_t mdkr_stbi_set_allocation_limit(size_t bytes) {                   \
        const size_t previous = mdkr_stbi_thread_limit;                     \
        mdkr_stbi_thread_limit = bytes;                                     \
        return previous;                                                    \
    }                                                                       \
    size_t mdkr_stbi_allocation_limit(void) { return mdkr_stbi_thread_limit; }

#endif
