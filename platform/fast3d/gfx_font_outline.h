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
 * `source` is the decoded RGBA8 atlas at its logical N64 dimensions. The face
 * is drawn with shared metrics -- one size, one baseline and one width for the
 * whole face -- rather than by fitting each glyph to its own ROM ink box:
 *
 *   - scale comes from the ROM's cap height (or x-height) mapped onto the same
 *     metric of the outline face, and the baseline from the ink bottoms of the
 *     alphanumerics that stand on it. Both are anchored to known characters, so
 *     every atlas of one face solves to the same numbers even though DKR splits
 *     a face across textures and each is redrawn on its own;
 *   - one width factor for the face follows the ROM's ink density, so the line
 *     keeps the ROM's rhythm under advances that never move, without any
 *     per-letter stretching;
 *   - each glyph is then drawn at its own natural proportions, centred on the
 *     midpoint of the ROM ink it replaces, so the leftover space is divided
 *     evenly between its two side bearings;
 *   - the result is clamped to the cell, so a glyph is never clipped;
 *   - colour is the region's own alpha-weighted mean, so whatever tint the
 *     ROM glyph carried is preserved and the combiner behaves as before.
 *
 * Any glyph that cannot be rendered falls back to a nearest-neighbour blit of
 * the ROM cell, so a missing outline degrades to today's output rather than to
 * a hole; an atlas that yields no metrics at all is declined, and the renderer
 * keeps the ROM pixels. Pixels outside registered regions stay transparent.
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
