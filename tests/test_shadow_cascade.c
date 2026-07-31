#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gfx_shadow_cascade.h"

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void perspective(float matrix[4][4], float aspect) {
    const float near_plane = 10.0f;
    const float far_plane = 15000.0f;
    const float cotangent = 1.7320508075688772f;
    memset(matrix, 0, 16 * sizeof(float));
    matrix[0][0] = cotangent / aspect;
    matrix[1][1] = cotangent;
    matrix[2][2] =
        -(far_plane + near_plane) / (far_plane - near_plane);
    matrix[2][3] = -1.0f;
    matrix[3][2] =
        -(2.0f * far_plane * near_plane) /
        (far_plane - near_plane);
}

static void prepare_frame(GfxShadowFrame *frame, size_t views) {
    memset(frame, 0, sizeof(*frame));
    frame->valid = true;
    frame->generation = 17;
    frame->stage_generation = 9;
    frame->view_count = views;
    for (size_t index = 0; index < views; index++) {
        GfxShadowView *view = &frame->views[index];
        view->valid = true;
        perspective(
            view->view_projection,
            views == 2 ? 2.0f / 3.0f : 4.0f / 3.0f);
        view->viewport[0] = (float)(index * 160);
        view->viewport[2] = views == 1 ? 320.0f : 160.0f;
        view->viewport[3] = views < 3 ? 240.0f : 120.0f;
        view->bounds_min[0] = -3000.0f;
        view->bounds_min[1] = -500.0f;
        view->bounds_min[2] = -6000.0f;
        view->bounds_max[0] = 3000.0f;
        view->bounds_max[1] = 1800.0f;
        view->bounds_max[2] = 1000.0f;
        view->triangle_count = 1000;
    }
}

static int finite_matrix(const float matrix[4][4]) {
    for (size_t row = 0; row < 4; row++) {
        for (size_t column = 0; column < 4; column++) {
            if (!isfinite(matrix[row][column])) {
                return 0;
            }
        }
    }
    return 1;
}

static void transform_point(
    const float matrix[4][4],
    const float point[3],
    float clip[4]) {
    const float value[4] = {point[0], point[1], point[2], 1.0f};
    for (size_t row = 0; row < 4; row++) {
        clip[row] = 0.0f;
        for (size_t column = 0; column < 4; column++) {
            clip[row] += matrix[row][column] * value[column];
        }
    }
}

int main(void) {
    GfxShadowFrame frame;
    GfxShadowPlan first;
    GfxShadowPlan second;
    GfxShadowBudget budget;
    const float sun[3] = {0.35f, 0.9f, 0.2f};

    budget = gfx_shadow_budget_for_views(1);
    expect(
        budget.resolution == 2048 &&
        budget.cascades_per_view == 2 &&
        budget.map_count == 2 &&
        budget.depth_bytes == 32u * 1024u * 1024u,
        "1P budget is two 2K Depth32 maps");
    budget = gfx_shadow_budget_for_views(2);
    expect(
        budget.resolution == 1024 &&
        budget.cascades_per_view == 2 &&
        budget.map_count == 4 &&
        budget.depth_bytes == 16u * 1024u * 1024u,
        "2P budget is four 1K Depth32 maps");
    budget = gfx_shadow_budget_for_views(4);
    expect(
        budget.resolution == 1024 &&
        budget.cascades_per_view == 1 &&
        budget.map_count == 4 &&
        budget.depth_bytes == 16u * 1024u * 1024u,
        "4P budget is one 1K Depth32 map per view");
    expect(
        gfx_shadow_budget_for_views(0).map_count == 0 &&
        gfx_shadow_budget_for_views(5).map_count == 0,
        "unsupported view counts fail closed");

    prepare_frame(&frame, 1);
    expect(
        gfx_shadow_build_plan(&frame, sun, &first),
        "valid 1P camera builds a plan");
    expect(
        first.valid && first.cascade_count == 2 &&
        first.frame_generation == 17 &&
        first.stage_generation == 9,
        "plan retains immutable frame identity");
    expect(
        first.cascades[0].split_near == 10.0f &&
        first.cascades[0].split_far == 2200.0f &&
        first.cascades[1].split_near == 1800.0f &&
        first.cascades[1].split_far == 7000.0f,
        "1P cascades overlap at the stable transition");
    expect(
        finite_matrix(first.cascades[0].world_to_clip) &&
        finite_matrix(first.cascades[1].world_to_clip) &&
        first.cascades[0].world_units_per_texel > 0.0f &&
        first.cascades[1].world_units_per_texel >
            first.cascades[0].world_units_per_texel,
        "cascade matrices and texel scales are finite and ordered");
    {
        float point[3] = {0.0f, 0.0f, -1000.0f};
        float clip[4];
        transform_point(first.cascades[0].world_to_clip, point, clip);
        expect(
            isfinite(clip[0]) && isfinite(clip[1]) &&
            isfinite(clip[2]) && clip[3] == 1.0f,
            "published matrix follows the backend column-vector contract");
    }
    expect(
        gfx_shadow_build_plan(&frame, sun, &second) &&
        memcmp(&first, &second, sizeof(first)) == 0,
        "identical input produces byte-stable plans");

    prepare_frame(&frame, 2);
    expect(
        gfx_shadow_build_plan(&frame, sun, &first) &&
        first.cascade_count == 4 &&
        first.cascades[0].view_index == 0 &&
        first.cascades[2].view_index == 1 &&
        first.cascades[3].map_index == 3,
        "2P cameras receive independent two-cascade jobs");
    prepare_frame(&frame, 4);
    expect(
        gfx_shadow_build_plan(&frame, sun, &first) &&
        first.cascade_count == 4 &&
        first.cascades[3].view_index == 3 &&
        first.cascades[3].cascade_index == 0 &&
        first.cascades[0].split_far == 5000.0f,
        "4P cameras receive one independent bounded job");

    frame.views[0].view_projection[0][0] = NAN;
    expect(
        !gfx_shadow_build_plan(&frame, sun, &first) && !first.valid,
        "non-finite camera fails atomically");
    prepare_frame(&frame, 1);
    frame.views[0].triangle_count = 0;
    expect(
        !gfx_shadow_build_plan(&frame, sun, &first) && !first.valid,
        "empty caster bounds fail atomically");
    prepare_frame(&frame, 1);
    frame.views[0].bounds_min[1] =
        frame.views[0].bounds_max[1] + 1.0f;
    expect(
        !gfx_shadow_build_plan(&frame, sun, &first) && !first.valid,
        "reversed caster bounds fail atomically");
    prepare_frame(&frame, 1);
    frame.views[0].viewport[2] = 0.0f;
    expect(
        !gfx_shadow_build_plan(&frame, sun, &first) && !first.valid,
        "empty viewport fails atomically");
    prepare_frame(&frame, 1);
    expect(
        !gfx_shadow_build_plan(
            &frame, (const float[3]) {0.0f, 0.0f, 0.0f}, &first) &&
        !first.valid,
        "degenerate sun direction fails atomically");

    if (failures != 0) {
        fprintf(stderr, "%d shadow-cascade test(s) failed\n", failures);
        return 1;
    }
    puts("PASS shadow cascade");
    return 0;
}
