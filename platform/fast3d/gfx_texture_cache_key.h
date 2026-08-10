#ifndef MDKR64_GFX_TEXTURE_CACHE_KEY_H
#define MDKR64_GFX_TEXTURE_CACHE_KEY_H

#include <stdbool.h>
#include <stdint.h>

/* Complete identity of a decoded/uploaded native frontend texture. */
struct DkrTexCacheKey {
    const uint8_t *addr;        /* source allocation identity */
    uint32_t source_line_bytes; /* decoded row addressing */
    uint32_t source_size_bytes; /* loaded span and inferred height */
    uint32_t palette_hash;      /* CI decode input */
    uint32_t palette_fmt;       /* TLUT type applied to that input */
    uint16_t width, height;     /* logical tile dimensions */
    uint8_t fmt, siz, palette;  /* RDP decode format */
    bool line_swapped;          /* LOADBLOCK dxt row layout */
    bool font_remastered;       /* source atlas versus derived SDF */
    bool font_outline;          /* source atlas versus outline redraw */
    bool mipmaps;               /* single level versus complete chain */
    bool cutout;                /* plain versus coverage-preserving mips */
    uint32_t override_generation; /* bumped when pack overrides toggle */
};

static inline bool dkr_texcache_key_equal(
    const struct DkrTexCacheKey *left,
    const struct DkrTexCacheKey *right) {
    return left->addr == right->addr &&
        left->source_line_bytes == right->source_line_bytes &&
        left->source_size_bytes == right->source_size_bytes &&
        left->palette_hash == right->palette_hash &&
        left->palette_fmt == right->palette_fmt &&
        left->width == right->width && left->height == right->height &&
        left->fmt == right->fmt && left->siz == right->siz &&
        left->palette == right->palette &&
        left->line_swapped == right->line_swapped &&
        left->font_remastered == right->font_remastered &&
        left->font_outline == right->font_outline &&
        left->mipmaps == right->mipmaps && left->cutout == right->cutout &&
        left->override_generation == right->override_generation;
}

#endif /* MDKR64_GFX_TEXTURE_CACHE_KEY_H */
