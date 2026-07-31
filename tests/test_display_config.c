#include "display_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int s_failures;

static void expect_near(const char *name, float actual, float expected, float tolerance) {
    if (!isfinite(actual) || fabsf(actual - expected) > tolerance) {
        fprintf(stderr, "FAIL %-34s actual=%f expected=%f (+/-%f)\n",
                name, actual, expected, tolerance);
        s_failures++;
    }
}

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void expect_projection_isotropic(
    const char *name,
    const MdkrProjection *projection,
    float physical_width,
    float physical_height) {
    const float radians_per_half_degree = 3.14159265358979323846f / 360.0f;
    const float pixels_per_x =
        physical_width / (2.0f * tanf(projection->horizontal_fov * radians_per_half_degree));
    const float pixels_per_y =
        physical_height / (2.0f * tanf(projection->vertical_fov * radians_per_half_degree));

    expect_near(name, pixels_per_x / pixels_per_y, 1.0f, 0.0001f);
}

int main(void) {
    MdkrBillboardCorrection billboard;
    MdkrDisplayLayout layout;
    MdkrProjection projection;
    float rotated_billboard[4][4] = {
        { 0.6f, 0.8f, 0.0f, 0.0f },
        { -0.8f, 0.8f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f, 1.0f },
    };

    layout = mdkr_display_calculate_layout(1920, 1080, 1, 0.0f);
    expect_near("auto presentation width", layout.presentation.width, 1920.0f, 0.01f);
    expect_near("auto presentation height", layout.presentation.height, 1080.0f, 0.01f);
    expect_near("auto presentation aspect", layout.presentation_aspect, 16.0f / 9.0f, 0.0001f);
    expect_near("safe width", layout.safe.width, 1440.0f, 0.01f);
    expect_near("safe height", layout.safe.height, 1080.0f, 0.01f);
    expect_near("safe centered x", layout.safe.x, 240.0f, 0.01f);
    expect_near("cover width", layout.fullbleed.width, 1920.0f, 0.01f);
    expect_near("cover height", layout.fullbleed.height, 1440.0f, 0.01f);
    expect_near("cover centered y", layout.fullbleed.y, -180.0f, 0.01f);
    expect_near("safe UI uses a uniform scale",
                layout.safe.width / 320.0f, layout.safe.height / 240.0f, 0.0001f);
    expect_near("fullbleed uses a uniform scale",
                layout.fullbleed.width / 320.0f, layout.fullbleed.height / 240.0f, 0.0001f);

    layout = mdkr_display_calculate_layout(1920, 1080, 1, 4.0f / 3.0f);
    expect_near("forced 4:3 width", layout.presentation.width, 1440.0f, 0.01f);
    expect_near("forced 4:3 x", layout.presentation.x, 240.0f, 0.01f);
    expect_near("forced 4:3 aspect", layout.presentation_aspect, 4.0f / 3.0f, 0.0001f);
    expect_near("forced 4:3 safe matches presentation width",
                layout.safe.width, layout.presentation.width, 0.01f);
    expect_near("forced 4:3 safe matches presentation x",
                layout.safe.x, layout.presentation.x, 0.01f);

    layout = mdkr_display_calculate_layout(1080, 1920, 1, 0.0f);
    expect_near("portrait presentation aspect", layout.presentation_aspect, 9.0f / 16.0f, 0.0001f);
    expect_near("portrait safe width", layout.safe.width, 1080.0f, 0.01f);
    expect_near("portrait safe height", layout.safe.height, 810.0f, 0.01f);
    expect_near("portrait safe centered y", layout.safe.y, 555.0f, 0.01f);
    expect_near("portrait safe UI uses a uniform scale",
                layout.safe.width / 320.0f, layout.safe.height / 240.0f, 0.0001f);

    layout = mdkr_display_calculate_layout(1920, 1080, 0, 0.0f);
    expect_true("legacy flag", layout.legacy_stretch);
    expect_near("legacy safe stretches full width", layout.safe.width, 1920.0f, 0.01f);
    expect_near("legacy fullbleed stretches width", layout.fullbleed.width, 1920.0f, 0.01f);
    expect_near("legacy fullbleed stretches height", layout.fullbleed.height, 1080.0f, 0.01f);
    expect_near("legacy reports authored aspect", layout.presentation_aspect, 4.0f / 3.0f, 0.0001f);

    projection = mdkr_display_calculate_projection(
        60.0f, 320.0f, 240.0f, 16.0f / 9.0f, 1, 0, 0.0f, 120.0f);
    expect_near("16:9 projection aspect", projection.aspect, 16.0f / 9.0f, 0.0001f);
    expect_near("16:9 preserves vertical FOV", projection.vertical_fov, 60.0f, 0.001f);
    expect_near("16:9 Hor+ horizontal FOV", projection.horizontal_fov, 91.4928f, 0.01f);
    expect_true("16:9 does not hit cap", !projection.horizontal_fov_capped);
    expect_projection_isotropic(
        "16:9 projection preserves proportions", &projection, 1920.0f, 1080.0f);
    billboard = mdkr_display_calculate_billboard_correction(
        60.0f, projection.vertical_fov, 4.0f / 3.0f, projection.aspect, 1);
    expect_near("16:9 billboard horizontal correction", billboard.clip_x, 0.75f, 0.0001f);
    expect_near("16:9 billboard authored size", billboard.clip_y, 1.0f, 0.0001f);
    expect_near("16:9 billboard pixels are square",
                1920.0f * billboard.clip_x,
                1080.0f * (4.0f / 3.0f) * billboard.clip_y, 0.01f);
    mdkr_display_apply_billboard_correction(rotated_billboard, billboard);
    expect_near("rotated billboard X column keeps pixel size",
                1920.0f * rotated_billboard[0][0], 1440.0f * 0.6f, 0.01f);
    expect_near("rotated billboard X off-diagonal keeps pixel size",
                1920.0f * rotated_billboard[1][0], 1440.0f * -0.8f, 0.01f);
    expect_near("rotated billboard Y column keeps pixel size",
                1080.0f * rotated_billboard[0][1], 1080.0f * 0.8f, 0.01f);
    expect_near("rotated billboard Y off-diagonal keeps pixel size",
                1080.0f * rotated_billboard[1][1], 1080.0f * 0.8f, 0.01f);

    projection = mdkr_display_calculate_projection(
        60.0f, 320.0f, 240.0f, 16.0f / 10.0f, 1, 0, 0.0f, 120.0f);
    expect_near("16:10 preserves vertical FOV", projection.vertical_fov, 60.0f, 0.001f);
    expect_projection_isotropic(
        "16:10 projection preserves proportions", &projection, 1728.0f, 1080.0f);

    projection = mdkr_display_calculate_projection(
        60.0f, 320.0f, 240.0f, 9.0f / 16.0f, 1, 0, 0.0f, 120.0f);
    expect_near("portrait preserves vertical FOV", projection.vertical_fov, 60.0f, 0.001f);
    expect_projection_isotropic(
        "portrait projection preserves proportions", &projection, 1080.0f, 1920.0f);

    projection = mdkr_display_calculate_projection(
        60.0f, 160.0f, 120.0f, 16.0f / 9.0f, 1, 0, 0.0f, 120.0f);
    expect_near("quadrant retains host aspect", projection.aspect, 16.0f / 9.0f, 0.0001f);
    expect_projection_isotropic(
        "quadrant projection preserves proportions", &projection, 960.0f, 540.0f);

    projection = mdkr_display_calculate_projection(
        60.0f, 320.0f, 120.0f, 16.0f / 9.0f, 1, 0, 0.0f, 120.0f);
    expect_near("true half-height aspect", projection.aspect, 32.0f / 9.0f, 0.0001f);
    expect_near("half-height horizontal cap", projection.horizontal_fov, 120.0f, 0.001f);
    expect_true("half-height cap engaged", projection.horizontal_fov_capped);
    expect_true("cap preserves shape by reducing vfov", projection.vertical_fov < 60.0f);
    expect_projection_isotropic(
        "half-height cap preserves proportions", &projection, 1920.0f, 540.0f);

    projection = mdkr_display_calculate_projection(
        60.0f, 320.0f, 240.0f, 21.0f / 9.0f, 1, 0, 0.0f, 120.0f);
    expect_near("21:9 horizontal FOV", projection.horizontal_fov, 106.8264f, 0.02f);
    expect_true("21:9 default lens below cap", !projection.horizontal_fov_capped);

    projection = mdkr_display_calculate_projection(
        60.0f, 320.0f, 240.0f, 21.0f / 9.0f, 1, 0, 0.0f,
        MDKR_SIMULATION_SAFE_MAX_HFOV);
    expect_near("21:9 simulation-safe cap", projection.horizontal_fov,
                MDKR_SIMULATION_SAFE_MAX_HFOV, 0.001f);
    expect_true("21:9 safe cap engaged", projection.horizontal_fov_capped);
    expect_true("safe cap preserves shape", projection.vertical_fov < 60.0f);
    expect_projection_isotropic(
        "21:9 cap preserves proportions", &projection, 2520.0f, 1080.0f);
    billboard = mdkr_display_calculate_billboard_correction(
        60.0f, projection.vertical_fov, 4.0f / 3.0f, projection.aspect, 1);
    expect_true("21:9 cap enlarges billboards with world", billboard.clip_y > 1.0f);
    expect_near("21:9 capped billboard pixels are square",
                2520.0f * billboard.clip_x,
                1080.0f * (4.0f / 3.0f) * billboard.clip_y, 0.01f);

    projection = mdkr_display_calculate_projection(
        60.0f, 320.0f, 240.0f, 16.0f / 9.0f, 1, 1, 75.0f, 140.0f);
    expect_near("gameplay FOV override", projection.vertical_fov, 75.0f, 0.001f);
    expect_projection_isotropic(
        "gameplay FOV preserves proportions", &projection, 1920.0f, 1080.0f);
    billboard = mdkr_display_calculate_billboard_correction(
        60.0f, projection.vertical_fov, 4.0f / 3.0f, projection.aspect, 1);
    expect_true("wider gameplay FOV shrinks billboards with world", billboard.clip_y < 1.0f);
    expect_near("gameplay-FOV billboard pixels are square",
                1920.0f * billboard.clip_x,
                1080.0f * (4.0f / 3.0f) * billboard.clip_y, 0.01f);

    projection = mdkr_display_calculate_projection(
        40.0f, 320.0f, 240.0f, 4.0f / 3.0f, 1, 1, 75.0f, 140.0f);
    expect_near("gameplay FOV preserves authored zoom",
                tanf(projection.vertical_fov * (3.14159265358979323846f / 360.0f)) /
                    tanf(40.0f * (3.14159265358979323846f / 360.0f)),
                tanf(75.0f * (3.14159265358979323846f / 360.0f)) /
                    tanf(60.0f * (3.14159265358979323846f / 360.0f)),
                0.0001f);

    projection = mdkr_display_calculate_projection(
        60.0f, 320.0f, 120.0f, 16.0f / 9.0f, 0, 0, 0.0f, 120.0f);
    expect_near("legacy projection stays 4:3", projection.aspect, 4.0f / 3.0f, 0.0001f);
    expect_near("legacy projection ignores cap", projection.vertical_fov, 60.0f, 0.001f);
    billboard = mdkr_display_calculate_billboard_correction(
        60.0f, projection.vertical_fov, 4.0f / 3.0f, projection.aspect, 0);
    expect_near("legacy billboard keeps X stretch", billboard.clip_x, 1.0f, 0.0001f);
    expect_near("legacy billboard keeps Y scale", billboard.clip_y, 1.0f, 0.0001f);

    if (s_failures != 0) {
        fprintf(stderr, "%d display-config assertion(s) failed\n", s_failures);
        return EXIT_FAILURE;
    }
    puts("display-config: all layout and projection assertions passed");
    return EXIT_SUCCESS;
}
