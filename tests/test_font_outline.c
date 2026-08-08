/*
 * Contract tests for the outline glyph path.
 *
 * The whole design rests on one property: the replacement may change the
 * pixels inside a glyph cell and nothing else. Every test here is a way of
 * checking that property, plus the fit rules that keep the result legible.
 */
#include "fast3d/gfx_font_outline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_pcHiresText = 1;

static int s_failures;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

#define ATLAS_W 64u
#define ATLAS_H 16u
#define UPSCALE 4u

/* Two cells side by side with a one-texel gutter, plus an empty third cell. */
static const GfxFontAtlasRegion kRegions[] = {
    {  2, 2, 8, 11, 'H' - 32 },
    { 11, 2, 7, 11, 'o' - 32 },
    { 20, 2, 6, 11, ' ' - 32 },
};
#define REGION_COUNT (sizeof(kRegions) / sizeof(kRegions[0]))

/* Paint a solid block of ink inside each non-empty cell, inset by one texel so
 * the cell has a margin the way a real glyph does. */
static void build_source(uint8_t *src, uint8_t alpha, uint8_t r, uint8_t g,
                         uint8_t b) {
    memset(src, 0, (size_t)ATLAS_W * ATLAS_H * 4u);
    for (size_t i = 0; i < REGION_COUNT - 1; i++) {
        const GfxFontAtlasRegion *rg = &kRegions[i];
        for (uint32_t y = 1; y + 1 < rg->height; y++) {
            for (uint32_t x = 1; x + 1 < rg->width; x++) {
                uint8_t *px = &src[(((size_t)rg->y + y) * ATLAS_W +
                                    rg->x + x) * 4u];
                px[0] = r;
                px[1] = g;
                px[2] = b;
                px[3] = alpha;
            }
        }
    }
}

static uint8_t *alloc_output(size_t *size) {
    *size = gfx_font_outline_output_bytes(ATLAS_W, ATLAS_H, UPSCALE);
    return (uint8_t *)malloc(*size);
}

static void test_faces_available(void) {
    expect_true("small face embedded",
                gfx_font_outline_available(GFX_FONT_FACE_SMALL));
    expect_true("subtitle face embedded",
                gfx_font_outline_available(GFX_FONT_FACE_SUBTITLE));
    expect_true("stylized faces have no replacement",
                !gfx_font_outline_available(GFX_FONT_FACE_NONE));
}

static void test_output_bytes_overflow(void) {
    expect_true("zero dimension rejected",
                gfx_font_outline_output_bytes(0, 8, 4) == 0);
    expect_true("zero upscale rejected",
                gfx_font_outline_output_bytes(8, 8, 0) == 0);
    expect_true("size is w*h*upscale^2*4",
                gfx_font_outline_output_bytes(8, 8, 4) == 8u * 8u * 16u * 4u);
    expect_true("overflow rejected",
                gfx_font_outline_output_bytes(0xFFFFu, 0xFFFFu, 0xFFFFu) == 0);
}

static void test_rejects_undersized_output(void) {
    uint8_t *src = (uint8_t *)malloc((size_t)ATLAS_W * ATLAS_H * 4u);
    size_t size;
    uint8_t *out = alloc_output(&size);

    build_source(src, 255, 255, 255, 255);
    expect_true("short buffer rejected",
                !gfx_font_outline_render_rgba(
                    GFX_FONT_FACE_SMALL, src, ATLAS_W, ATLAS_H, UPSCALE,
                    kRegions, REGION_COUNT, out, size - 1));
    expect_true("null source rejected",
                !gfx_font_outline_render_rgba(
                    GFX_FONT_FACE_SMALL, NULL, ATLAS_W, ATLAS_H, UPSCALE,
                    kRegions, REGION_COUNT, out, size));
    expect_true("unclassified face rejected",
                !gfx_font_outline_render_rgba(
                    GFX_FONT_FACE_NONE, src, ATLAS_W, ATLAS_H, UPSCALE,
                    kRegions, REGION_COUNT, out, size));
    free(src);
    free(out);
}

/*
 * The load-bearing one. A texrect samples exactly one cell, so a texel written
 * outside a registered cell is a texel that can bleed into a neighbouring
 * glyph. Nothing outside the registered rectangles may be touched.
 */
static void test_writes_stay_inside_registered_cells(void) {
    uint8_t *src = (uint8_t *)malloc((size_t)ATLAS_W * ATLAS_H * 4u);
    size_t size;
    uint8_t *out = alloc_output(&size);
    uint32_t out_w = ATLAS_W * UPSCALE;
    uint32_t out_h = ATLAS_H * UPSCALE;
    int stray = 0;

    build_source(src, 255, 255, 255, 255);
    expect_true("render succeeds",
                gfx_font_outline_render_rgba(
                    GFX_FONT_FACE_SMALL, src, ATLAS_W, ATLAS_H, UPSCALE,
                    kRegions, REGION_COUNT, out, size));

    for (uint32_t y = 0; y < out_h; y++) {
        for (uint32_t x = 0; x < out_w; x++) {
            int inside = 0;
            for (size_t i = 0; i < REGION_COUNT; i++) {
                const GfxFontAtlasRegion *rg = &kRegions[i];
                if (x >= (uint32_t)rg->x * UPSCALE &&
                    x < ((uint32_t)rg->x + rg->width) * UPSCALE &&
                    y >= (uint32_t)rg->y * UPSCALE &&
                    y < ((uint32_t)rg->y + rg->height) * UPSCALE) {
                    inside = 1;
                    break;
                }
            }
            if (!inside && out[((size_t)y * out_w + x) * 4u + 3u] != 0) {
                stray++;
            }
        }
    }
    expect_true("no ink outside registered cells", stray == 0);
    free(src);
    free(out);
}

/* An empty cell is a space. It must stay empty rather than acquire a glyph. */
static void test_empty_cell_stays_empty(void) {
    uint8_t *src = (uint8_t *)malloc((size_t)ATLAS_W * ATLAS_H * 4u);
    size_t size;
    uint8_t *out = alloc_output(&size);
    uint32_t out_w = ATLAS_W * UPSCALE;
    const GfxFontAtlasRegion *space = &kRegions[2];
    int ink = 0;

    build_source(src, 255, 255, 255, 255);
    (void)gfx_font_outline_render_rgba(
        GFX_FONT_FACE_SMALL, src, ATLAS_W, ATLAS_H, UPSCALE, kRegions,
        REGION_COUNT, out, size);

    for (uint32_t y = 0; y < (uint32_t)space->height * UPSCALE; y++) {
        for (uint32_t x = 0; x < (uint32_t)space->width * UPSCALE; x++) {
            uint32_t dx = (uint32_t)space->x * UPSCALE + x;
            uint32_t dy = (uint32_t)space->y * UPSCALE + y;
            if (out[((size_t)dy * out_w + dx) * 4u + 3u] != 0) {
                ink++;
            }
        }
    }
    expect_true("empty cell produces no glyph", ink == 0);
    free(src);
    free(out);
}

/* Each populated cell must actually receive coverage: a silently blank glyph
 * would be worse than the source atlas. */
static void test_populated_cells_receive_ink(void) {
    uint8_t *src = (uint8_t *)malloc((size_t)ATLAS_W * ATLAS_H * 4u);
    size_t size;
    uint8_t *out = alloc_output(&size);
    uint32_t out_w = ATLAS_W * UPSCALE;

    build_source(src, 255, 255, 255, 255);
    (void)gfx_font_outline_render_rgba(
        GFX_FONT_FACE_SMALL, src, ATLAS_W, ATLAS_H, UPSCALE, kRegions,
        REGION_COUNT, out, size);

    for (size_t i = 0; i < REGION_COUNT - 1; i++) {
        const GfxFontAtlasRegion *rg = &kRegions[i];
        int ink = 0;
        for (uint32_t y = 0; y < (uint32_t)rg->height * UPSCALE; y++) {
            for (uint32_t x = 0; x < (uint32_t)rg->width * UPSCALE; x++) {
                uint32_t dx = (uint32_t)rg->x * UPSCALE + x;
                uint32_t dy = (uint32_t)rg->y * UPSCALE + y;
                if (out[((size_t)dy * out_w + dx) * 4u + 3u] >= 128) {
                    ink++;
                }
            }
        }
        expect_true("cell received coverage", ink > 0);
    }
    free(src);
    free(out);
}

/* Whatever tint the ROM glyph carried is preserved, so the combiner keeps
 * behaving as it did. */
static void test_colour_follows_the_source(void) {
    uint8_t *src = (uint8_t *)malloc((size_t)ATLAS_W * ATLAS_H * 4u);
    size_t size;
    uint8_t *out = alloc_output(&size);
    uint32_t out_w = ATLAS_W * UPSCALE;
    int wrong = 0;
    int seen = 0;

    build_source(src, 255, 200, 100, 50);
    (void)gfx_font_outline_render_rgba(
        GFX_FONT_FACE_SMALL, src, ATLAS_W, ATLAS_H, UPSCALE, kRegions,
        REGION_COUNT, out, size);

    for (uint32_t y = 0; y < ATLAS_H * UPSCALE; y++) {
        for (uint32_t x = 0; x < out_w; x++) {
            const uint8_t *px = &out[((size_t)y * out_w + x) * 4u];
            if (px[3] == 0) {
                continue;
            }
            seen++;
            if (px[0] != 200 || px[1] != 100 || px[2] != 50) {
                wrong++;
            }
        }
    }
    expect_true("some ink produced", seen > 0);
    expect_true("source tint preserved", wrong == 0);
    free(src);
    free(out);
}

/* The same inputs must produce the same atlas every time, or a texture-cache
 * refill would silently change the screen. */
static void test_render_is_deterministic(void) {
    uint8_t *src = (uint8_t *)malloc((size_t)ATLAS_W * ATLAS_H * 4u);
    size_t size;
    uint8_t *a = alloc_output(&size);
    uint8_t *b = alloc_output(&size);

    build_source(src, 255, 255, 255, 255);
    (void)gfx_font_outline_render_rgba(
        GFX_FONT_FACE_SMALL, src, ATLAS_W, ATLAS_H, UPSCALE, kRegions,
        REGION_COUNT, a, size);
    (void)gfx_font_outline_render_rgba(
        GFX_FONT_FACE_SMALL, src, ATLAS_W, ATLAS_H, UPSCALE, kRegions,
        REGION_COUNT, b, size);
    expect_true("identical inputs give identical output",
                memcmp(a, b, size) == 0);
    free(src);
    free(a);
    free(b);
}

/*
 * The load-bearing non-vacuity control.
 *
 * Every other assertion in this file is also satisfied by the nearest-neighbour
 * ROM fallback, which runs whenever render_glyph declines. If the rasteriser
 * ever went silently dead -- a regenerated header without a cmap, a shifted
 * character index -- gfx_font_outline_render_rgba would blit the ROM cell for
 * every region, still return true, and the whole suite would stay green while
 * the feature shipped as a 4x point-magnification of the source atlas.
 *
 * Two properties separate a real rasterisation from that fallback:
 *   - the source here is a hard-edged block, so a nearest-neighbour copy can
 *     only ever contain alpha 0 or 255, while a rasterised outline has
 *     antialiased edges and therefore intermediate coverage;
 *   - the output must differ from the nearest-neighbour upscale of the source.
 */
static void test_output_is_a_real_rasterisation(void) {
    uint8_t *src = (uint8_t *)malloc((size_t)ATLAS_W * ATLAS_H * 4u);
    size_t size;
    uint8_t *out = alloc_output(&size);
    uint32_t out_w = ATLAS_W * UPSCALE;
    int partial = 0;
    int differs = 0;

    build_source(src, 255, 255, 255, 255);
    expect_true("render succeeds",
                gfx_font_outline_render_rgba(
                    GFX_FONT_FACE_SMALL, src, ATLAS_W, ATLAS_H, UPSCALE,
                    kRegions, REGION_COUNT, out, size));

    for (size_t i = 0; i < REGION_COUNT - 1; i++) {
        const GfxFontAtlasRegion *rg = &kRegions[i];
        for (uint32_t y = 0; y < (uint32_t)rg->height * UPSCALE; y++) {
            for (uint32_t x = 0; x < (uint32_t)rg->width * UPSCALE; x++) {
                uint32_t dx = (uint32_t)rg->x * UPSCALE + x;
                uint32_t dy = (uint32_t)rg->y * UPSCALE + y;
                uint32_t sx = (uint32_t)rg->x + x / UPSCALE;
                uint32_t sy = (uint32_t)rg->y + y / UPSCALE;
                uint8_t got = out[((size_t)dy * out_w + dx) * 4u + 3u];
                uint8_t nearest =
                    src[((size_t)sy * ATLAS_W + sx) * 4u + 3u];
                if (got != 0 && got != 255) {
                    partial++;
                }
                if (got != nearest) {
                    differs++;
                }
            }
        }
    }
    expect_true("output carries antialiased coverage, so a glyph was rasterised",
                partial > 0);
    expect_true("output differs from a nearest-neighbour copy of the source",
                differs > 0);
    free(src);
    free(out);
}

/*
 * A wide glyph in a cell whose ROM ink is inset must be fitted to the width
 * that actually remains from the ink's left edge, not to the whole cell. If it
 * is fitted to the whole cell the write loop's cell guard trims the overhang
 * and the glyph is silently clipped, which the "never clip" rule forbids.
 *
 * The check: rendered ink may not start left of the ROM ink's left edge, and
 * may not reach the cell's right edge when the ROM ink was inset from it by
 * more than the fit is allowed to expand.
 */
static void test_inset_ink_is_not_clipped(void) {
    uint32_t atlas_w = 40u;
    uint32_t atlas_h = 16u;
    uint8_t *src = (uint8_t *)calloc((size_t)atlas_w * atlas_h * 4u, 1);
    /* A wide cell whose ink sits well inside it on the left. */
    const GfxFontAtlasRegion region = { 2, 2, 14, 12, 'W' - 32 };
    const uint32_t ink_x0 = 5;
    size_t size = gfx_font_outline_output_bytes(atlas_w, atlas_h, UPSCALE);
    uint8_t *out = (uint8_t *)malloc(size);
    uint32_t out_w = atlas_w * UPSCALE;
    int min_x = (int)out_w;
    int max_x = -1;

    gfx_font_outline_clipped_texels = 0;

    for (uint32_t y = 1; y + 1 < region.height; y++) {
        for (uint32_t x = ink_x0; x + 1 < region.width; x++) {
            uint8_t *px = &src[(((size_t)region.y + y) * atlas_w +
                                region.x + x) * 4u];
            px[0] = px[1] = px[2] = px[3] = 255;
        }
    }
    expect_true("inset render succeeds",
                gfx_font_outline_render_rgba(
                    GFX_FONT_FACE_SMALL, src, atlas_w, atlas_h, UPSCALE,
                    &region, 1, out, size));

    for (uint32_t y = 0; y < atlas_h * UPSCALE; y++) {
        for (uint32_t x = 0; x < out_w; x++) {
            if (out[((size_t)y * out_w + x) * 4u + 3u] == 0) {
                continue;
            }
            if ((int)x < min_x) {
                min_x = (int)x;
            }
            if ((int)x > max_x) {
                max_x = (int)x;
            }
        }
    }
    expect_true("inset cell produced ink", max_x >= 0);
    expect_true("ink does not start left of the ROM ink",
                max_x < 0 || min_x >= (int)(((uint32_t)region.x + ink_x0) *
                                            UPSCALE));
    /*
     * Asserting the ink stays inside the cell would only re-certify the write
     * loop's backstop, which holds even when it is busy trimming a glyph. The
     * falsifiable claim is that the backstop had nothing to trim: revert
     * cell_limit to the whole cell width and this counter goes positive while
     * every bounds assertion still passes.
     */
    expect_true("the cell backstop discarded no coverage",
                gfx_font_outline_clipped_texels == 0);
    free(src);
    free(out);
}

int main(void) {
    test_faces_available();
    test_output_bytes_overflow();
    test_rejects_undersized_output();
    test_writes_stay_inside_registered_cells();
    test_output_is_a_real_rasterisation();
    test_inset_ink_is_not_clipped();
    test_empty_cell_stays_empty();
    test_populated_cells_receive_ink();
    test_colour_follows_the_source();
    test_render_is_deterministic();
    gfx_font_outline_shutdown();

    expect_true("no glyph was clipped anywhere in the suite",
                gfx_font_outline_clipped_texels == 0);

    if (s_failures != 0) {
        fprintf(stderr, "%d font outline test(s) failed\n", s_failures);
        return 1;
    }
    printf("font outline tests passed\n");
    return 0;
}
