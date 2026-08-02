#include "fast3d/gfx_texture_cache_key.h"

#include <stdio.h>

static int failures;

static void expect_distinct(
    const char *field,
    const struct DkrTexCacheKey *left,
    const struct DkrTexCacheKey *right) {
    if (dkr_texcache_key_equal(left, right)) {
        fprintf(stderr, "FAIL texture key ignored %s\n", field);
        failures++;
    }
}

int main(void) {
    static const uint8_t source_a;
    static const uint8_t source_b;
    const struct DkrTexCacheKey base = {
        .addr = &source_a,
        .source_line_bytes = 32,
        .source_size_bytes = 256,
        .palette_hash = 0x12345678u,
        .width = 16,
        .height = 8,
        .fmt = 1,
        .siz = 2,
        .palette = 3,
        .line_swapped = false,
        .font_remastered = false,
        .mipmaps = false,
        .cutout = false,
    };
    struct DkrTexCacheKey changed;

    if (!dkr_texcache_key_equal(&base, &base)) {
        fprintf(stderr, "FAIL identical texture key did not match\n");
        failures++;
    }

#define CHECK_FIELD(field_, value_)            \
    do {                                        \
        changed = base;                         \
        changed.field_ = (value_);              \
        expect_distinct(#field_, &base, &changed); \
    } while (0)

    CHECK_FIELD(addr, &source_b);
    CHECK_FIELD(source_line_bytes, 64);
    CHECK_FIELD(source_size_bytes, 512);
    CHECK_FIELD(palette_hash, 0x87654321u);
    CHECK_FIELD(width, 17);
    CHECK_FIELD(height, 9);
    CHECK_FIELD(fmt, 2);
    CHECK_FIELD(siz, 3);
    CHECK_FIELD(palette, 4);
    CHECK_FIELD(line_swapped, true);
    CHECK_FIELD(font_remastered, true);
    CHECK_FIELD(mipmaps, true);
    CHECK_FIELD(cutout, true);

#undef CHECK_FIELD

    if (failures != 0) {
        return 1;
    }
    puts("all texture cache key tests passed");
    return 0;
}
