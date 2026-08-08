#include "gfx_font_registry.h"

#include <limits.h>
#include <string.h>

static bool region_equal(const GfxFontAtlasRegion *left,
                         const GfxFontAtlasRegion *right) {
    return left->x == right->x && left->y == right->y &&
           left->width == right->width && left->height == right->height &&
           left->character == right->character;
}

static bool region_valid(const GfxFontAtlasRegion *region) {
    return region->width != 0 && region->height != 0;
}

static bool entry_contains_region(const GfxFontRegistryEntry *entry,
                                  const GfxFontAtlasRegion *region) {
    for (size_t index = 0; index < entry->region_count; index++) {
        if (region_equal(&entry->regions[index], region)) {
            return true;
        }
    }
    return false;
}

static bool earlier_input_contains_region(const GfxFontAtlasRegion *regions,
                                          size_t end,
                                          const GfxFontAtlasRegion *region) {
    for (size_t index = 0; index < end; index++) {
        if (region_valid(&regions[index]) &&
            region_equal(&regions[index], region)) {
            return true;
        }
    }
    return false;
}

void gfx_font_registry_init(GfxFontRegistry *registry) {
    if (registry != NULL) {
        memset(registry, 0, sizeof(*registry));
    }
}

const GfxFontRegistryEntry *gfx_font_registry_find(
    const GfxFontRegistry *registry, const void *source) {
    const uint8_t *bytes = (const uint8_t *)source;

    if (registry == NULL || bytes == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < GFX_FONT_REGISTRY_CAPACITY; index++) {
        const GfxFontRegistryEntry *entry = &registry->entries[index];
        if (entry->source == bytes && entry->references != 0) {
            return entry;
        }
    }
    return NULL;
}

bool gfx_font_registry_register(GfxFontRegistry *registry, const void *source,
                                GfxFontFace face,
                                const GfxFontAtlasRegion *regions,
                                size_t region_count) {
    const uint8_t *bytes = (const uint8_t *)source;
    GfxFontRegistryEntry *entry = NULL;
    size_t additions = 0;

    if (registry == NULL || bytes == NULL ||
        (region_count != 0 && regions == NULL)) {
        return false;
    }
    for (size_t index = 0; index < GFX_FONT_REGISTRY_CAPACITY; index++) {
        if (registry->entries[index].source == bytes &&
            registry->entries[index].references != 0) {
            entry = &registry->entries[index];
            break;
        }
    }
    if (entry == NULL) {
        for (size_t index = 0; index < GFX_FONT_REGISTRY_CAPACITY; index++) {
            if (registry->entries[index].references == 0) {
                entry = &registry->entries[index];
                break;
            }
        }
    }
    if (entry == NULL || entry->references == UINT32_MAX) {
        return false;
    }

    for (size_t index = 0; index < region_count; index++) {
        if (region_valid(&regions[index]) &&
            !entry_contains_region(entry, &regions[index]) &&
            !earlier_input_contains_region(regions, index, &regions[index])) {
            additions++;
        }
    }
    if (additions > GFX_FONT_REGIONS_PER_ATLAS - entry->region_count) {
        return false;
    }

    if (entry->references == 0) {
        entry->source = bytes;
        entry->region_count = 0;
        entry->face = face;
    } else if (entry->face != face) {
        /* Two fonts disagree about what this atlas is. Neither claim can be
         * trusted, so fall back to the ROM pixels for the shared source. */
        entry->face = GFX_FONT_FACE_NONE;
    }
    for (size_t index = 0; index < region_count; index++) {
        if (region_valid(&regions[index]) &&
            !entry_contains_region(entry, &regions[index])) {
            entry->regions[entry->region_count++] = regions[index];
        }
    }
    entry->references++;
    return true;
}

bool gfx_font_registry_unregister(GfxFontRegistry *registry,
                                  const void *source) {
    const GfxFontRegistryEntry *found =
        gfx_font_registry_find(registry, source);
    GfxFontRegistryEntry *entry;

    if (found == NULL) {
        return false;
    }
    entry = &registry->entries[found - registry->entries];
    entry->references--;
    if (entry->references == 0) {
        memset(entry, 0, sizeof(*entry));
    }
    return true;
}

size_t gfx_font_registry_active_count(const GfxFontRegistry *registry) {
    size_t count = 0;

    if (registry == NULL) {
        return 0;
    }
    for (size_t index = 0; index < GFX_FONT_REGISTRY_CAPACITY; index++) {
        if (registry->entries[index].references != 0) {
            count++;
        }
    }
    return count;
}
