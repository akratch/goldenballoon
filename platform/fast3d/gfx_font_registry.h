#ifndef MDKR_GFX_FONT_REGISTRY_H
#define MDKR_GFX_FONT_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GFX_FONT_REGISTRY_CAPACITY 64
#define GFX_FONT_REGIONS_PER_ATLAS 96

typedef struct GfxFontAtlasRegion {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} GfxFontAtlasRegion;

typedef struct GfxFontRegistryEntry {
    const uint8_t *source;
    uint32_t references;
    size_t region_count;
    GfxFontAtlasRegion regions[GFX_FONT_REGIONS_PER_ATLAS];
} GfxFontRegistryEntry;

typedef struct GfxFontRegistry {
    GfxFontRegistryEntry entries[GFX_FONT_REGISTRY_CAPACITY];
} GfxFontRegistry;

void gfx_font_registry_init(GfxFontRegistry *registry);

/*
 * Register one loaded font's use of an atlas. Re-registering a shared source
 * increments its reference count and merges unique glyph regions atomically.
 */
bool gfx_font_registry_register(GfxFontRegistry *registry, const void *source,
                                const GfxFontAtlasRegion *regions,
                                size_t region_count);

/* Drop one reference. The entry and its region metadata clear at zero. */
bool gfx_font_registry_unregister(GfxFontRegistry *registry,
                                  const void *source);

const GfxFontRegistryEntry *gfx_font_registry_find(
    const GfxFontRegistry *registry, const void *source);

size_t gfx_font_registry_active_count(const GfxFontRegistry *registry);

#endif
