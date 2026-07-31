#include "fast3d/gfx_font_sdf.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void test_contracts(void) {
    uint8_t source[4 * 4 * 4] = { 0 };
    uint8_t output[8 * 8 * 4];
    const GfxFontAtlasRegion region = { 0, 0, 4, 4 };

    expect_true("valid size",
                gfx_font_sdf_output_bytes(4, 4, 2) == sizeof(output));
    expect_true("zero width refused",
                gfx_font_sdf_output_bytes(0, 4, 2) == 0);
    expect_true("one-times is not a remaster",
                gfx_font_sdf_output_bytes(4, 4, 1) == 0);
    expect_true("dimension multiplication overflow refused",
                gfx_font_sdf_output_bytes(UINT32_MAX, 4, 8) == 0);
    expect_true("backend integer dimension overflow refused",
                gfx_font_sdf_output_bytes((uint32_t)INT_MAX, 1, 2) == 0);
    expect_true("short output refused",
                !gfx_font_sdf_upscale_rgba(
                    source, 4, 4, 2, &region, 1,
                    output, sizeof(output) - 1));
    expect_true("regions required",
                !gfx_font_sdf_upscale_rgba(
                    source, 4, 4, 2, NULL, 0, output, sizeof(output)));
}

static void test_contour_colour_and_uncovered_pixels(void) {
    enum { W = 7, H = 5, S = 4, OW = W * S, OH = H * S };
    uint8_t source[W * H * 4];
    uint8_t output[OW * OH * 4];
    const GfxFontAtlasRegion region = { 1, 0, 5, 5 };
    memset(source, 0, sizeof(source));

    for (int y = 1; y <= 3; y++) {
        size_t index = ((size_t)y * W + 3) * 4;
        source[index + 0] = 240;
        source[index + 1] = 180;
        source[index + 2] = 60;
        source[index + 3] = 255;
    }
    expect_true("upscale succeeds",
                gfx_font_sdf_upscale_rgba(
                    source, W, H, S, &region, 1,
                    output, sizeof(output)));

    {
        size_t centre =
            ((size_t)(2 * S + S / 2) * OW +
             (size_t)(3 * S + S / 2)) * 4;
        expect_true("interior remains opaque", output[centre + 3] > 245);
        expect_true("authored RGB remains",
                    output[centre] > 220 &&
                    output[centre + 1] > 160 &&
                    output[centre + 2] > 45);
    }

    {
        int partial = 0;
        for (size_t index = 0; index < (size_t)OW * OH; index++) {
            uint8_t alpha = output[index * 4 + 3];
            if (alpha > 0 && alpha < 255) {
                partial++;
            }
        }
        expect_true("distance contour has partial coverage", partial > 0);
    }
    expect_true("outside registered region stays transparent",
                output[((size_t)2 * S * OW) * 4 + 3] == 0);
}

static void test_adjacent_glyphs_do_not_bleed(void) {
    enum { W = 6, H = 5, S = 4, OW = W * S, OH = H * S };
    uint8_t source[W * H * 4];
    uint8_t output[OW * OH * 4];
    const GfxFontAtlasRegion regions[] = {
        { 0, 0, 3, 5 },
        { 3, 0, 3, 5 },
    };
    memset(source, 0, sizeof(source));

    /* Red stem in the left cell; opaque green block touches the shared edge. */
    for (int y = 1; y <= 3; y++) {
        size_t red = ((size_t)y * W + 1) * 4;
        source[red + 0] = 255;
        source[red + 3] = 255;
        for (int x = 3; x < W; x++) {
            size_t green = ((size_t)y * W + (size_t)x) * 4;
            source[green + 1] = 255;
            source[green + 3] = 255;
        }
    }
    expect_true("adjacent atlas derives",
                gfx_font_sdf_upscale_rgba(
                    source, W, H, S, regions, 2,
                    output, sizeof(output)));

    for (int y = 0; y < OH; y++) {
        for (int x = 0; x < 3 * S; x++) {
            size_t pixel = ((size_t)y * OW + (size_t)x) * 4;
            expect_true("right glyph colour cannot enter left cell",
                        output[pixel + 1] == 0);
        }
    }
}

int main(void) {
    test_contracts();
    test_contour_colour_and_uncovered_pixels();
    test_adjacent_glyphs_do_not_bleed();

    if (s_failures != 0) {
        fprintf(stderr, "%d font SDF test(s) failed\n", s_failures);
        return 1;
    }
    printf("font SDF tests passed\n");
    return 0;
}
