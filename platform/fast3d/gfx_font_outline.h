/*
 * Runtime-only high-resolution text from embedded outline faces.
 *
 * Two of DKR's four authored fonts are plain lettering with no house style:
 * SmallFont (an 11px IA8 grotesque carrying menus, HUD and times) and
 * SubtitleFont (a 14px chunky display face). Those two are replaced with real
 * outline faces we ship ourselves. FunFont and BigFont are DKR's multicolour
 * display lettering and are never touched here.
 *
 * The replacement is layout-neutral by construction. render_text_string()
 * takes every metric -- advance, per-glyph offset, cell size, source UV -- from
 * FontData, never from the texture, so substituting the pixels inside a glyph
 * cell cannot move any text. This module only ever writes inside the cells the
 * registry handed it.
 *
 * The caller owns both buffers. This module has no filesystem API and never
 * writes derived atlas data to disk.
 */
#ifndef MDKR_GFX_FONT_OUTLINE_H
#define MDKR_GFX_FONT_OUTLINE_H

#include "gfx_font_registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Owned by video_config.c (Video.HighResolutionText); 0 in Pure, 1 in Restored
 * and Remastered by preset. */
extern int g_pcHiresText;

/* Output is RGBA8 at (width * upscale) x (height * upscale). Matches the SDF
 * path's buffer shape so the two share the renderer's scratch allocation. */
size_t gfx_font_outline_output_bytes(uint32_t width, uint32_t height,
                                     uint32_t upscale);

/* True when an embedded face backs this classification. */
bool gfx_font_outline_available(GfxFontFace face);

/*
 * Redraw a decoded ROM font atlas from the embedded outline face.
 *
 * `source` is the decoded RGBA8 atlas at its logical N64 dimensions. For each
 * registered region the glyph is fitted to that region's ROM ink box:
 *
 *   - vertical fit is exact, which derives baseline, x-height, cap height and
 *     descender placement from the ROM data itself, per glyph, with no global
 *     metrics to get wrong;
 *   - horizontal fit follows the ROM ink width so the line keeps the ROM's
 *     rhythm, but the stretch is bounded (see WIDTH_STRETCH_LIMIT): at a 7px
 *     cap height the ROM cannot draw a stem thinner than about 2px, so 'l',
 *     'i', 'I' and '1' have ink boxes twice as wide as any outline face's
 *     natural stem, and matching those exactly would produce fat verticals;
 *   - the result is clamped to the cell, so a glyph is never clipped;
 *   - colour is the region's own alpha-weighted mean, so whatever tint the
 *     ROM glyph carried is preserved and the combiner behaves as before.
 *
 * Any glyph that cannot be rendered falls back to a nearest-neighbour blit of
 * the ROM cell, so a missing outline degrades to today's output rather than to
 * a hole. Pixels outside registered regions stay transparent.
 */
bool gfx_font_outline_render_rgba(
    GfxFontFace face, const uint8_t *source, uint32_t width, uint32_t height,
    uint32_t upscale, const GfxFontAtlasRegion *regions, size_t region_count,
    uint8_t *output, size_t output_size);

/*
 * Coverage texels the per-cell backstop had to discard, cumulative.
 *
 * The fit is supposed to keep every glyph inside its own cell, so this must
 * stay zero: anything counted here is a letter that was silently trimmed.
 * Exposed so a gate can assert the arithmetic rather than trust it.
 */
extern uint32_t gfx_font_outline_clipped_texels;

/* Release the glyph scratch buffer. Called from the renderer's teardown. */
void gfx_font_outline_shutdown(void);

#endif
