#ifndef MDKR_GFX_FONT_REGISTRY_H
#define MDKR_GFX_FONT_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GFX_FONT_REGISTRY_CAPACITY 64
/*
 * 96 is not a budget, it is the exact bound. font_atlas_regions() emits at most
 * one region per entry of ASSET_FONTS' 96-glyph table, and the character sets
 * contributed by different texture slots of one font are disjoint, so a source
 * atlas can never accumulate more than 96 (rectangle, character) pairs even
 * with the character-keyed dedupe that keeps aliased cells apart. Measured
 * worst case across all four authored faces is 36.
 */
#define GFX_FONT_REGIONS_PER_ATLAS 96

/*
 * Which authored ASSET_FONTS face an atlas belongs to.
 *
 * Only the two plain faces have an outline replacement. FunFont and BigFont
 * are DKR's own multicolour display lettering and must keep their ROM pixels,
 * so they classify as NONE and continue down the SDF path in Remastered.
 *
 * Classification happens at the font loader, never by texture dimensions.
 */
typedef enum GfxFontFace {
    GFX_FONT_FACE_NONE = 0,
    GFX_FONT_FACE_SMALL,
    GFX_FONT_FACE_SUBTITLE
} GfxFontFace;

typedef struct GfxFontAtlasRegion {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    /* Index into the 96-entry ASSET_FONTS glyph table, i.e. ASCII - 32. The
     * outline path needs to know which character a cell draws; the SDF path
     * ignores it. */
    uint8_t character;
} GfxFontAtlasRegion;

typedef struct GfxFontRegistryEntry {
    const uint8_t *source;
    uint32_t references;
    GfxFontFace face;
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
 *
 * Regions are keyed by rectangle *and* character: one cell legitimately serves
 * two characters (FunFont draws 'a' from the 'A' cell), and the outline path
 * has to be able to ask "which glyph is this" of every region it is given.
 *
 * If two registrations of one source disagree about the face, the entry
 * degrades to GFX_FONT_FACE_NONE -- keeping the ROM pixels is always safe.
 */
bool gfx_font_registry_register(GfxFontRegistry *registry, const void *source,
                                GfxFontFace face,
                                const GfxFontAtlasRegion *regions,
                                size_t region_count);

/* Drop one reference. The entry and its region metadata clear at zero. */
bool gfx_font_registry_unregister(GfxFontRegistry *registry,
                                  const void *source);

const GfxFontRegistryEntry *gfx_font_registry_find(
    const GfxFontRegistry *registry, const void *source);

size_t gfx_font_registry_active_count(const GfxFontRegistry *registry);

#endif
