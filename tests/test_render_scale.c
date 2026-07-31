#include <math.h>
#include <stdio.h>

#include "gfx_render_scale.h"

float g_pcRenderScale = 1.0f;

static int failures;

static void expect_dimensions(
    unsigned int output_width,
    unsigned int output_height,
    unsigned int max_dimension,
    unsigned int expected_width,
    unsigned int expected_height,
    const char *message) {
    unsigned int width = 0;
    unsigned int height = 0;
    gfx_render_scaled_dimensions(
        output_width, output_height, max_dimension, &width, &height);
    if (width != expected_width || height != expected_height) {
        fprintf(
            stderr,
            "FAIL: %s (got %ux%u, expected %ux%u)\n",
            message,
            width,
            height,
            expected_width,
            expected_height);
        failures++;
    }
}

int main(void) {
    g_pcRenderScale = 2.0f;
    expect_dimensions(
        960, 540, 8192, 1920, 1080,
        "ordinary 2x output is unchanged");

    expect_dimensions(
        2560, 1440, 8192, 3840, 2160,
        "fullscreen 2x is proportionally capped at the 4K render budget");

    g_pcRenderScale = 4.0f;
    expect_dimensions(
        960, 540, 8192, 3840, 2160,
        "4x remains available when it fits the render budget");

    g_pcRenderScale = 3.0f;
    expect_dimensions(
        1000, 500, 2000, 2000, 1000,
        "device edge cap preserves aspect");

    g_pcRenderScale = NAN;
    expect_dimensions(
        1280, 720, 8192, 1280, 720,
        "NaN falls back to 1x");

    g_pcRenderScale = 0.25f;
    expect_dimensions(
        5000, 3000, 8192, 5000, 3000,
        "the budget never undersamples native output");

    if (failures != 0) {
        fprintf(stderr, "%d render-scale test(s) failed\n", failures);
        return 1;
    }
    puts("render-scale tests passed");
    return 0;
}
