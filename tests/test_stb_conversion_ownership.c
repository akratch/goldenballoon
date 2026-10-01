/* Ordinary pixel ownership and deterministic allocator failure, not malformed
 * image input. Compile the same pinned/amended PNG implementation with a
 * counting allocator; production admission and supported formats are unchanged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image_alloc.h"

/* This fixture instantiates stb_image itself, so it owns the decode bound. */
MDKR_STBI_ALLOCATION_LIMIT_STORAGE

static size_t allocation_attempts;
static size_t refused_attempt;
static size_t live_allocations;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL conversion ownership: %s\n", message);
        exit(1);
    }
}

static int refuse_allocation(void) {
    ++allocation_attempts;
    return refused_attempt != 0u && allocation_attempts == refused_attempt;
}

static void *tracked_malloc(size_t size) {
    void *result;
    if (refuse_allocation()) return NULL;
    result = mdkr_stbi_malloc(size);
    if (result != NULL) ++live_allocations;
    return result;
}

static void *tracked_realloc(void *pointer, size_t size) {
    void *result;
    const int new_allocation = pointer == NULL;
    if (refuse_allocation()) return NULL;
    result = mdkr_stbi_realloc(pointer, size);
    if (result != NULL && new_allocation) ++live_allocations;
    return result;
}

static void tracked_free(void *pointer) {
    if (pointer != NULL) {
        expect(live_allocations > 0u, "free has tracked ownership");
        --live_allocations;
    }
    free(pointer);
}

#define STBI_MALLOC(size) tracked_malloc((size_t)(size))
#define STBI_REALLOC(pointer, size) tracked_realloc((pointer), (size_t)(size))
#define STBI_FREE(pointer) tracked_free(pointer)
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
/* PNG-only compilation leaves two upstream arithmetic helpers unused. Limit
 * this warning exception to the vendored implementation, not test/helpers. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* Writer allocation is deliberately independent, as in production. It only
 * creates a small ordinary image for the public loading-path failure checks. */
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static void reset_tracking(size_t fail_at) {
    expect(live_allocations == 0u, "previous operation released all storage");
    allocation_attempts = 0u;
    refused_attempt = fail_at;
}

static void test_conversions(int channels, int refuse) {
    const int samples = 3 * 2 * channels;
    stbi_uc *small;
    stbi_us *wide;
    int index;

    reset_tracking(0u);
    small = (stbi_uc *)tracked_malloc((size_t)samples);
    expect(small != NULL, "ordinary source allocation succeeds");
    for (index = 0; index < samples; ++index) small[index] = (stbi_uc)(index * 9);
    if (refuse) refused_attempt = allocation_attempts + 1u;
    wide = stbi__convert_8_to_16(small, 3, 2, channels);
    if (refuse) {
        expect(wide == NULL, "failed widening reports failure");
    } else {
        expect(wide != NULL && live_allocations == 1u,
               "successful widening transfers ownership");
        for (index = 0; index < samples; ++index) {
            expect(wide[index] == (stbi_us)((index * 9) * 257),
                   "ordinary widening preserves expected samples");
        }
        stbi_image_free(wide);
    }
    expect(live_allocations == 0u, "widening releases its consumed source");

    reset_tracking(0u);
    wide = (stbi_us *)tracked_malloc((size_t)samples * sizeof(*wide));
    expect(wide != NULL, "ordinary wide source allocation succeeds");
    for (index = 0; index < samples; ++index) wide[index] = (stbi_us)(index * 1234);
    if (refuse) refused_attempt = allocation_attempts + 1u;
    small = stbi__convert_16_to_8(wide, 3, 2, channels);
    if (refuse) {
        expect(small == NULL, "failed narrowing reports failure");
    } else {
        expect(small != NULL && live_allocations == 1u,
               "successful narrowing transfers ownership");
        for (index = 0; index < samples; ++index) {
            expect(small[index] == (stbi_uc)((index * 1234) >> 8),
                   "ordinary narrowing preserves expected samples");
        }
        stbi_image_free(small);
    }
    expect(live_allocations == 0u, "narrowing releases its consumed source");
}

typedef struct EncodedPng {
    unsigned char bytes[4096];
    size_t size;
} EncodedPng;

static void collect_png(void *context, void *data, int size) {
    EncodedPng *png = (EncodedPng *)context;
    expect(size >= 0 && (size_t)size <= sizeof(png->bytes) - png->size,
           "ordinary encoded image fits fixed storage");
    memcpy(png->bytes + png->size, data, (size_t)size);
    png->size += (size_t)size;
}

static void *load_png(const EncodedPng *png, int wide_output,
                      int *width, int *height, int *channels) {
    if (wide_output) {
        return stbi_load_16_from_memory(png->bytes, (int)png->size,
                                        width, height, channels, 4);
    }
    // This is the 8-bit RGBA entry point used by the four product callers.
    return stbi_load_from_memory(png->bytes, (int)png->size,
                                 width, height, channels, 4);
}

static void test_public_load_failure_cleanup(int wide_output, int flip) {
    const unsigned char pixels[16] = {
        0, 20, 40, 255, 60, 80, 100, 255,
        120, 140, 160, 255, 180, 200, 220, 255,
    };
    EncodedPng png = {{0}, 0u};
    void *loaded;
    size_t attempts, attempt;
    int width = 0, height = 0, channels = 0, index;
    expect(stbi_write_png_to_func(collect_png, &png, 2, 2, 4, pixels, 8),
           "ordinary PNG fixture encodes");
    reset_tracking(0u);
    /* Flip is not enabled by the product. Exercise both variants so a later
     * caller cannot accidentally postprocess a failed conversion result. */
    stbi_set_flip_vertically_on_load(flip);
    loaded = load_png(&png, wide_output, &width, &height, &channels);
    expect(loaded != NULL && width == 2 && height == 2 && channels == 4,
           "ordinary public load succeeds");
    for (index = 0; index < 16; ++index) {
        const int source = flip ? ((1 - index / 8) * 8 + index % 8) : index;
        const unsigned actual = wide_output
            ? ((const stbi_us *)loaded)[index]
            : ((const stbi_uc *)loaded)[index];
        const unsigned expected = (unsigned)pixels[source] *
            (wide_output ? 257u : 1u);
        expect(actual == expected,
               "ordinary public conversion retains pixels and flip behavior");
    }
    attempts = allocation_attempts;
    stbi_image_free(loaded);
    expect(attempts > 0u && live_allocations == 0u, "baseline has no retained storage");
    for (attempt = 1u; attempt <= attempts; ++attempt) {
        reset_tracking(attempt);
        loaded = load_png(&png, wide_output, &width, &height, &channels);
        expect(loaded == NULL, "refused required allocation reports failure");
        expect(allocation_attempts >= attempt, "requested refusal was reached");
        expect(live_allocations == 0u, "failed public load releases all storage");
    }
    stbi_set_flip_vertically_on_load(0);
    reset_tracking(0u);
}

int main(void) {
    int channels;
    for (channels = 1; channels <= 4; ++channels) {
        test_conversions(channels, 0);
        test_conversions(channels, 1);
    }
    test_public_load_failure_cleanup(0, 0);
    test_public_load_failure_cleanup(0, 1);
    test_public_load_failure_cleanup(1, 0);
    test_public_load_failure_cleanup(1, 1);
    puts("stb conversion ownership: PASS");
    return 0;
}
