/*
 * Runtime-only, ROM-derived high-resolution font coverage.
 *
 * The caller owns both buffers. This module has no filesystem API and never
 * writes derived atlas data to disk.
 */
#ifndef MDKR_GFX_FONT_SDF_H
#define MDKR_GFX_FONT_SDF_H

#include "gfx_font_registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t gfx_font_sdf_output_bytes(uint32_t width, uint32_t height,
                                 uint32_t upscale);

/*
 * Reconstruct high-resolution RGBA coverage from a decoded ROM atlas.
 *
 * Each glyph region is transformed independently. Distance searches and colour
 * reconstruction are clamped to that region, preventing a tightly packed
 * neighbouring glyph from contributing to the selected glyph's contour.
 * Pixels outside registered regions remain transparent.
 */
bool gfx_font_sdf_upscale_rgba(
    const uint8_t *source, uint32_t width, uint32_t height, uint32_t upscale,
    const GfxFontAtlasRegion *regions, size_t region_count,
    uint8_t *output, size_t output_size);

#endif
