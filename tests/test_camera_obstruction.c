#include "camera_obstruction.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define TEST_EPSILON 0.0002f
#define METAMORPHIC_CASES 4096U
#define ROUNDED_ORACLE_CASES 256U

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void expect_near(const char *name, float actual, float expected, float tolerance) {
    if (!isfinite(actual) || fabsf(actual - expected) > tolerance) {
        fprintf(stderr, "FAIL %-38s actual=%f expected=%f (+/-%f)\n",
                name, actual, expected, tolerance);
        s_failures++;
    }
}

static MdkrCameraSweepInput sweep_input(
    MdkrCameraVec3 start,
    MdkrCameraVec3 desired,
    float radius) {
    MdkrCameraSweepInput input;
    memset(&input, 0, sizeof(input));
    input.guard.kind = MDKR_CAMERA_LENS_GUARD_SPHERE;
    input.guard.radius = radius;
    input.start_eye = start;
    input.desired_eye = desired;
    return input;
}

static MdkrCameraOcclusionWorld world_for(
    const MdkrCameraVec3 *vertices,
    size_t vertex_count,
    const uint32_t *indices,
    const MdkrCameraOcclusionTriangle *triangles,
    size_t triangle_count) {
    MdkrCameraOcclusionWorld world;
    world.vertices = vertices;
    world.vertex_count = vertex_count;
    world.indices = indices;
    world.triangles = triangles;
    world.triangle_count = triangle_count;
    return world;
}

static uint32_t fuzz_next(uint32_t *state) {
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static float fuzz_range(uint32_t *state, float minimum, float maximum) {
    const float unit = (float)(fuzz_next(state) >> 8) * (1.0f / 16777215.0f);

    return minimum + (maximum - minimum) * unit;
}

static int finite_hit(const MdkrCameraSweepHit *hit) {
    return isfinite(hit->fraction) && hit->fraction >= 0.0f && hit->fraction <= 1.0f &&
           isfinite(hit->clearance) && isfinite(hit->penetration_depth) &&
           isfinite(hit->point.x) && isfinite(hit->point.y) && isfinite(hit->point.z) &&
           isfinite(hit->normal.x) && isfinite(hit->normal.y) && isfinite(hit->normal.z);
}

static void expect_equivalent_transform(
    const char *name,
    MdkrCameraSweepStatus baseline_status,
    const MdkrCameraSweepHit *baseline,
    MdkrCameraSweepStatus transformed_status,
    const MdkrCameraSweepHit *transformed,
    float scale,
    MdkrCameraVec3 translation) {
    char label[96];

    snprintf(label, sizeof(label), "%s status", name);
    expect_true(label, baseline_status == transformed_status);
    if (baseline_status != MDKR_CAMERA_SWEEP_HIT || transformed_status != MDKR_CAMERA_SWEEP_HIT) {
        return;
    }
    snprintf(label, sizeof(label), "%s fraction", name);
    expect_near(label, transformed->fraction, baseline->fraction, 0.001f);
    snprintf(label, sizeof(label), "%s stable id", name);
    expect_true(label, transformed->stable_id == baseline->stable_id);
    snprintf(label, sizeof(label), "%s kind", name);
    expect_true(label, transformed->kind == baseline->kind);
    snprintf(label, sizeof(label), "%s feature", name);
    expect_true(label, transformed->feature == baseline->feature);
    snprintf(label, sizeof(label), "%s overlap", name);
    expect_true(label, transformed->started_overlapping == baseline->started_overlapping);
    snprintf(label, sizeof(label), "%s point x", name);
    expect_near(label, transformed->point.x, baseline->point.x * scale + translation.x, 0.003f * fmaxf(1.0f, fabsf(transformed->point.x)));
    snprintf(label, sizeof(label), "%s point y", name);
    expect_near(label, transformed->point.y, baseline->point.y * scale + translation.y, 0.003f * fmaxf(1.0f, fabsf(transformed->point.y)));
    snprintf(label, sizeof(label), "%s point z", name);
    expect_near(label, transformed->point.z, baseline->point.z * scale + translation.z, 0.003f * fmaxf(1.0f, fabsf(transformed->point.z)));
    snprintf(label, sizeof(label), "%s normal x", name);
    expect_near(label, transformed->normal.x, baseline->normal.x, 0.001f);
    snprintf(label, sizeof(label), "%s normal y", name);
    expect_near(label, transformed->normal.y, baseline->normal.y, 0.001f);
    snprintf(label, sizeof(label), "%s normal z", name);
    expect_near(label, transformed->normal.z, baseline->normal.z, 0.001f);
    snprintf(label, sizeof(label), "%s clearance", name);
    expect_near(label, transformed->clearance, baseline->clearance * scale, 0.003f * fmaxf(1.0f, fabsf(transformed->clearance)));
    snprintf(label, sizeof(label), "%s penetration", name);
    expect_near(label, transformed->penetration_depth, baseline->penetration_depth * scale, 0.003f * fmaxf(1.0f, fabsf(transformed->penetration_depth)));
}

static void test_no_hit_and_endpoint_touch(void) {
    MdkrCameraVec3 vertices[] = {
        { 20.0f, -10.0f, -10.0f }, { 20.0f, 10.0f, -10.0f }, { 20.0f, 0.0f, 10.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 7U, 1U, 41U, 0U } };
    MdkrCameraOcclusionWorld world = world_for(vertices, 3U, indices, triangles, 1U);
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus status;

    status = mdkr_camera_sweep(&world, &(MdkrCameraSweepInput){
        .guard = { MDKR_CAMERA_LENS_GUARD_SPHERE, 1.0f },
        .start_eye = { 0.0f, 0.0f, 0.0f }, .desired_eye = { 10.0f, 0.0f, 0.0f },
    }, &hit);
    expect_true("clear path status", status == MDKR_CAMERA_SWEEP_CLEAR);
    expect_near("clear path fraction", hit.fraction, 1.0f, TEST_EPSILON);

    vertices[0].x = 11.0f;
    vertices[1].x = 11.0f;
    vertices[2].x = 11.0f;
    status = mdkr_camera_sweep(&world, &(MdkrCameraSweepInput){
        .guard = { MDKR_CAMERA_LENS_GUARD_SPHERE, 1.0f },
        .start_eye = { 0.0f, 0.0f, 0.0f }, .desired_eye = { 10.0f, 0.0f, 0.0f },
    }, &hit);
    expect_true("endpoint touch status", status == MDKR_CAMERA_SWEEP_HIT);
    expect_near("endpoint touch fraction", hit.fraction, 1.0f, TEST_EPSILON);
}

static void test_face_edge_vertex_and_two_sided(void) {
    const MdkrCameraVec3 face_vertices[] = {
        { 5.0f, -5.0f, -5.0f }, { 5.0f, 5.0f, -5.0f }, { 5.0f, 0.0f, 5.0f },
    };
    const MdkrCameraVec3 edge_vertices[] = {
        { 5.0f, -1.0f, -1.0f }, { 5.0f, 1.0f, -1.0f }, { 5.0f, 1.0f, 1.0f },
    };
    const MdkrCameraVec3 vertex_vertices[] = {
        { 5.0f, 1.0f, 1.0f }, { 5.0f, 3.0f, 1.0f }, { 5.0f, 1.0f, 3.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 12U, 1U, 9U, 0U } };
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus status;
    MdkrCameraOcclusionWorld world = world_for(face_vertices, 3U, indices, triangles, 1U);
    MdkrCameraSweepInput input;

    input = sweep_input((MdkrCameraVec3){ 0, 0, 0 }, (MdkrCameraVec3){ 10, 0, 0 }, 1.0f);
    status = mdkr_camera_sweep(&world, &input, &hit);
    expect_true("face hit", status == MDKR_CAMERA_SWEEP_HIT);
    expect_true("face feature", hit.feature == MDKR_CAMERA_SWEEP_FEATURE_FACE);
    expect_near("face fraction", hit.fraction, 0.4f, TEST_EPSILON);

    world = world_for(edge_vertices, 3U, indices, triangles, 1U);
    input = sweep_input((MdkrCameraVec3){ 0, 2, 0 }, (MdkrCameraVec3){ 10, 2, 0 }, 1.0f);
    status = mdkr_camera_sweep(&world, &input, &hit);
    expect_true("edge hit", status == MDKR_CAMERA_SWEEP_HIT);
    expect_true("edge feature", hit.feature == MDKR_CAMERA_SWEEP_FEATURE_EDGE);
    expect_near("edge fraction", hit.fraction, 0.5f, TEST_EPSILON);

    world = world_for(vertex_vertices, 3U, indices, triangles, 1U);
    input = sweep_input((MdkrCameraVec3){ 0, 0, 0 }, (MdkrCameraVec3){ 10, 0, 0 }, 1.5f);
    status = mdkr_camera_sweep(&world, &input, &hit);
    expect_true("vertex hit", status == MDKR_CAMERA_SWEEP_HIT);
    expect_true("vertex feature", hit.feature == MDKR_CAMERA_SWEEP_FEATURE_VERTEX);

    world = world_for(face_vertices, 3U, indices, triangles, 1U);
    input = sweep_input((MdkrCameraVec3){ 10, 0, 0 }, (MdkrCameraVec3){ 0, 0, 0 }, 1.0f);
    status = mdkr_camera_sweep(&world, &input, &hit);
    expect_true("paper-thin backface is two-sided", status == MDKR_CAMERA_SWEEP_HIT);
    expect_near("paper-thin backface fraction", hit.fraction, 0.4f, TEST_EPSILON);
}

static void test_overlap_earliest_tie_and_radius(void) {
    const MdkrCameraVec3 vertices[] = {
        { 5, -5, -5 }, { 5, 5, -5 }, { 5, 0, 5 },
        { 7, -5, -5 }, { 7, 5, -5 }, { 7, 0, 5 },
        { 5, -4, -4 }, { 5, 4, -4 }, { 5, 0, 4 },
    };
    const uint32_t indices[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
    const MdkrCameraOcclusionTriangle triangles[] = {
        { 30U, 1U, 1U, 0U }, { 20U, 1U, 2U, 0U }, { 5U, 1U, 3U, 0U },
    };
    MdkrCameraOcclusionWorld world = world_for(vertices, 9U, indices, triangles, 3U);
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus status;
    MdkrCameraSweepInput input = sweep_input((MdkrCameraVec3){ 0, 0, 0 },
                                               (MdkrCameraVec3){ 10, 0, 0 }, 1.0f);
    const uint32_t permuted_indices[] = { 6, 7, 8, 3, 4, 5, 0, 1, 2 };
    const MdkrCameraOcclusionTriangle permuted_triangles[] = {
        { 5U, 1U, 3U, 0U }, { 20U, 1U, 2U, 0U }, { 30U, 1U, 1U, 0U },
    };
    float small_fraction;
    float large_fraction;

    world = world_for(vertices, 9U, indices, triangles, 2U);
    status = mdkr_camera_sweep(&world, &input, &hit);
    expect_true("earliest time beats lower later stable id",
                status == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 30U);
    expect_near("earliest time fraction", hit.fraction, 0.4f, TEST_EPSILON);

    world = world_for(vertices, 9U, indices, triangles, 3U);
    status = mdkr_camera_sweep(&world, &input, &hit);
    expect_true("equal time returns stable lowest id", status == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 5U);
    expect_near("earliest wall wins", hit.fraction, 0.4f, TEST_EPSILON);
    world = world_for(vertices, 9U, permuted_indices, permuted_triangles, 3U);
    status = mdkr_camera_sweep(&world, &input, &hit);
    expect_true("permuted triangles preserve stable tie", status == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 5U);
    world = world_for(vertices, 9U, indices, triangles, 3U);

    input.guard.radius = 0.5f;
    status = mdkr_camera_sweep(&world, &input, &hit);
    small_fraction = hit.fraction;
    input.guard.radius = 1.5f;
    status = mdkr_camera_sweep(&world, &input, &hit);
    large_fraction = hit.fraction;
    expect_true("larger guard cannot increase safe boom", status == MDKR_CAMERA_SWEEP_HIT &&
                large_fraction <= small_fraction);

    input = sweep_input((MdkrCameraVec3){ 4.5f, 0, 0 }, (MdkrCameraVec3){ 10, 0, 0 }, 1.0f);
    status = mdkr_camera_sweep(&world, &input, &hit);
    expect_true("start overlap", status == MDKR_CAMERA_SWEEP_HIT && hit.started_overlapping);
    expect_near("overlap fraction", hit.fraction, 0.0f, TEST_EPSILON);
    expect_near("overlap depth", hit.penetration_depth, 0.5f, TEST_EPSILON);
    expect_near("depenetration normal", hit.normal.x, -1.0f, TEST_EPSILON);
}

static void test_pinned_concave_tunnel_and_thin_pillar(void) {
    const uint32_t indices[] = { 0U, 1U, 2U, 3U, 4U, 5U };
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus status;

    {
        /* Two perpendicular faces meet ahead of a diagonal camera chord. Both
         * contacts have the same time; immutable stable ID must decide. */
        const MdkrCameraVec3 vertices[] = {
            { 5.0f, -20.0f, -20.0f }, { 5.0f, 20.0f, -20.0f },
            { 5.0f, 0.0f, 20.0f },
            { -20.0f, -20.0f, 5.0f }, { 20.0f, -20.0f, 5.0f },
            { 0.0f, 20.0f, 5.0f },
        };
        const MdkrCameraOcclusionTriangle triangles[] = {
            { 20U, 1U, 1U, 0U }, { 10U, 1U, 1U, 0U },
        };
        const MdkrCameraOcclusionWorld world = world_for(
            vertices, 6U, indices, triangles, 2U);
        const MdkrCameraSweepInput input = sweep_input(
            (MdkrCameraVec3) { 0.0f, 0.0f, 0.0f },
            (MdkrCameraVec3) { 10.0f, 0.0f, 10.0f }, 1.0f);

        status = mdkr_camera_sweep(&world, &input, &hit);
        expect_true("pinned concave corner hit",
                    status == MDKR_CAMERA_SWEEP_HIT);
        expect_true("pinned concave stable tie",
                    hit.stable_id == 10U);
        expect_near("pinned concave fraction", hit.fraction, 0.4f, 0.001f);
    }

    {
        /* A centered boom is clear for a compact guard but cannot fit a lens
         * wider than the tunnel half-height. */
        const MdkrCameraVec3 vertices[] = {
            { -20.0f, 2.5f, -20.0f }, { 40.0f, 2.5f, -20.0f },
            { 10.0f, 2.5f, 40.0f },
            { -20.0f, -2.5f, -20.0f }, { 10.0f, -2.5f, 40.0f },
            { 40.0f, -2.5f, -20.0f },
        };
        const MdkrCameraOcclusionTriangle triangles[] = {
            { 100U, 1U, 1U, 0U }, { 101U, 1U, 1U, 0U },
        };
        const MdkrCameraOcclusionWorld world = world_for(
            vertices, 6U, indices, triangles, 2U);
        MdkrCameraSweepInput input = sweep_input(
            (MdkrCameraVec3) { 0.0f, 0.0f, 0.0f },
            (MdkrCameraVec3) { 20.0f, 0.0f, 0.0f }, 1.0f);

        expect_true("pinned tunnel compact guard clear",
                    mdkr_camera_sweep(&world, &input, &hit) ==
                    MDKR_CAMERA_SWEEP_CLEAR);
        input.guard.radius = 3.0f;
        status = mdkr_camera_sweep(&world, &input, &hit);
        expect_true("pinned tunnel oversized guard blocked",
                    status == MDKR_CAMERA_SWEEP_HIT && hit.started_overlapping);
    }

    {
        /* A point chord passes above a thin pillar, while a finite guard clips
         * its edge. This is the small-feature counterpart to the wall witness. */
        const MdkrCameraVec3 vertices[] = {
            { 10.0f, -0.2f, -0.2f }, { 10.0f, 0.2f, -0.2f },
            { 10.0f, 0.2f, 0.2f },
            { 10.0f, -0.2f, -0.2f }, { 10.0f, 0.2f, 0.2f },
            { 10.0f, -0.2f, 0.2f },
        };
        const MdkrCameraOcclusionTriangle triangles[] = {
            { 200U, 1U, 1U, 0U }, { 201U, 1U, 1U, 0U },
        };
        const MdkrCameraOcclusionWorld world = world_for(
            vertices, 6U, indices, triangles, 2U);
        MdkrCameraSweepInput input = sweep_input(
            (MdkrCameraVec3) { 0.0f, 0.3f, 0.0f },
            (MdkrCameraVec3) { 20.0f, 0.3f, 0.0f }, 0.0f);

        expect_true("pinned thin-pillar point control clear",
                    mdkr_camera_sweep(&world, &input, &hit) ==
                    MDKR_CAMERA_SWEEP_CLEAR);
        input.guard.radius = 0.15f;
        expect_true("pinned thin-pillar finite guard hit",
                    mdkr_camera_sweep(&world, &input, &hit) ==
                    MDKR_CAMERA_SWEEP_HIT);
    }
}

static void test_invalid_input_and_purity(void) {
    MdkrCameraVec3 valid_vertices[] = {
        { 5, -5, -5 }, { 5, 5, -5 }, { 5, 0, 5 },
    };
    MdkrCameraVec3 nan_vertices[] = {
        { NAN, -5, -5 }, { 5, 5, -5 }, { 5, 0, 5 },
    };
    uint32_t valid_indices[] = { 0, 1, 2 };
    const uint32_t degenerate_indices[] = { 0, 0, 1 };
    MdkrCameraOcclusionTriangle triangles[] = { { 1U, 1U, 1U, 0U } };
    MdkrCameraOcclusionWorld world = world_for(valid_vertices, 3U, valid_indices, triangles, 1U);
    MdkrCameraSweepInput input = sweep_input((MdkrCameraVec3){ 0, 0, 0 },
                                               (MdkrCameraVec3){ 10, 0, 0 }, 1.0f);
    MdkrCameraSweepHit hit;
    MdkrCameraOcclusionWorld before_world;
    MdkrCameraSweepInput before_input;
    MdkrCameraVec3 before_vertices[3];
    uint32_t before_indices[3];
    MdkrCameraOcclusionTriangle before_triangles[1];

    before_world = world;
    before_input = input;
    memcpy(before_vertices, valid_vertices, sizeof(before_vertices));
    memcpy(before_indices, valid_indices, sizeof(before_indices));
    memcpy(before_triangles, triangles, sizeof(before_triangles));
    expect_true("pure query hit", mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("world structure unchanged", memcmp(&world, &before_world, sizeof(world)) == 0);
    expect_true("input unchanged", memcmp(&input, &before_input, sizeof(input)) == 0);
    expect_true("vertices unchanged", memcmp(valid_vertices, before_vertices, sizeof(before_vertices)) == 0);
    expect_true("indices unchanged", memcmp(valid_indices, before_indices, sizeof(before_indices)) == 0);
    expect_true("metadata unchanged", memcmp(triangles, before_triangles, sizeof(before_triangles)) == 0);

    world.indices = degenerate_indices;
    expect_true("degenerate triangle rejected", mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    world = world_for(nan_vertices, 3U, valid_indices, triangles, 1U);
    expect_true("NaN vertex rejected", mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    input.start_eye.x = NAN;
    expect_true("NaN input rejected", mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    world = world_for(valid_vertices, 3U, valid_indices, triangles, 1U);
    input.start_eye.x = 0.0f;
    input.guard.radius = INFINITY;
    expect_true("infinite guard rejected", mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    input.guard.radius = 1.0f;
    world.triangle_count = SIZE_MAX;
    expect_true("overflowing triangle count rejected before access",
                mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
}

static void test_translation_scale_and_projection_guard(void) {
    const MdkrCameraVec3 base_vertices[] = {
        { 5, -5, -5 }, { 5, 5, -5 }, { 5, 0, 5 },
    };
    const MdkrCameraVec3 shifted_vertices[] = {
        { 105, 45, -30 }, { 105, 55, -30 }, { 105, 50, -20 },
    };
    const MdkrCameraVec3 scaled_vertices[] = {
        { 15, -15, -15 }, { 15, 15, -15 }, { 15, 0, 15 },
    };
    const uint32_t indices[] = { 0, 1, 2 };
    const MdkrCameraOcclusionTriangle triangles[] = { { 11U, 1U, 1U, 0U } };
    MdkrCameraOcclusionWorld world;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepInput input;
    float base_fraction;
    MdkrCameraLensGuard guard;

    world = world_for(base_vertices, 3U, indices, triangles, 1U);
    input = sweep_input((MdkrCameraVec3){ 0, 0, 0 }, (MdkrCameraVec3){ 10, 0, 0 }, 1.0f);
    expect_true("base sweep", mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    base_fraction = hit.fraction;

    world = world_for(shifted_vertices, 3U, indices, triangles, 1U);
    input = sweep_input((MdkrCameraVec3){ 100, 50, -25 }, (MdkrCameraVec3){ 110, 50, -25 }, 1.0f);
    expect_true("translated sweep", mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_near("translation preserves fraction", hit.fraction, base_fraction, TEST_EPSILON);

    world = world_for(scaled_vertices, 3U, indices, triangles, 1U);
    input = sweep_input((MdkrCameraVec3){ 0, 0, 0 }, (MdkrCameraVec3){ 30, 0, 0 }, 3.0f);
    expect_true("scaled sweep", mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_near("uniform scale preserves fraction", hit.fraction, base_fraction, TEST_EPSILON);

    expect_true("4:3 stock lens guard", mdkr_camera_lens_guard_from_projection(
        10.0f, 3.14159265358979323846f / 3.0f, 4.0f / 3.0f, 0.0f, &guard));
    expect_near("4:3 broadphase radius", guard.radius, 13.8778f, 0.001f);
    expect_true("invalid lens guard rejected", !mdkr_camera_lens_guard_from_projection(
        10.0f, 0.0f, 4.0f / 3.0f, 0.0f, &guard));
}

static void test_near_plane_guard_beats_center_ray(void) {
    const MdkrCameraVec3 vertices[] = {
        { 50, 10, -4 }, { 50, 14, -4 }, { 50, 12, 4 },
    };
    const uint32_t indices[] = { 0, 1, 2 };
    const MdkrCameraOcclusionTriangle triangles[] = { { 99U, 1U, 1U, 0U } };
    MdkrCameraOcclusionWorld world = world_for(vertices, 3U, indices, triangles, 1U);
    MdkrCameraLensGuard guard;
    MdkrCameraSweepInput input;
    MdkrCameraSweepHit hit;

    expect_true("projection guard creates", mdkr_camera_lens_guard_from_projection(
        10.0f, 3.14159265358979323846f / 3.0f, 4.0f / 3.0f, 0.0f, &guard));
    input = sweep_input((MdkrCameraVec3){ 0, 0, 0 }, (MdkrCameraVec3){ 100, 0, 0 }, 0.0f);
    expect_true("broken center ray misses near-plane occluder",
                mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_CLEAR);
    input.guard = guard;
    expect_true("projection guard catches near-plane occluder",
                mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("guard result is finite", isfinite(hit.fraction) && hit.fraction >= 0.0f && hit.fraction <= 1.0f);
}

static void test_oriented_rounded_lens_narrow_phase(void) {
    MdkrCameraRoundedLensGuard guard;
    MdkrCameraRoundedLensGuard thin_skin_guard;
    MdkrCameraRoundedLensGuard rotated_guard;
    MdkrCameraLensNarrowHit hit;
    MdkrCameraSweepStatus status;
    const MdkrCameraVec3 eye = { 0.0f, 0.0f, 0.0f };
    const MdkrCameraVec3 side_vertices[] = {
        { 5.0f, 8.0f, -0.25f }, { 5.0f, 8.0f, 0.25f }, { 5.0f, 8.5f, 0.0f },
    };
    const uint32_t side_indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle side_triangles[] = { { 501U, 1U, 1U, 0U } };
    MdkrCameraOcclusionWorld side_world;
    MdkrCameraSweepHit sphere_hit;
    double conservative_radius;

    expect_true("rounded lens creates", mdkr_camera_rounded_lens_guard_from_projection(
        10.0f, 3.14159265358979323846f / 3.0f, 4.0f / 3.0f, 0.5f,
        (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f }, (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f }, &guard));
    expect_near("rounded lens half width", guard.half_width, 7.6980f, 0.001f);
    expect_near("rounded lens half height", guard.half_height, 5.7735f, 0.001f);
    expect_near("rounded lens enclosing radius", guard.broadphase_radius, 14.3778f, 0.001f);
    expect_near("rounded lens forward x", guard.forward.x, 1.0f, TEST_EPSILON);
    expect_near("rounded lens renderer right y", guard.right.y, -1.0f, TEST_EPSILON);
    expect_near("rounded lens up z", guard.up.z, 1.0f, TEST_EPSILON);
    expect_true("rounded lens conservative radius recomputes",
                mdkr_camera_rounded_lens_guard_conservative_radius(
                    &guard, &conservative_radius));
    expect_true("rounded lens conservative radius is outward",
                conservative_radius >= (double)guard.broadphase_radius);
    expect_true("rounded lens conservative radius rejects null guard",
                !mdkr_camera_rounded_lens_guard_conservative_radius(
                    NULL, &conservative_radius));
    expect_true("rounded lens conservative radius rejects null output",
                !mdkr_camera_rounded_lens_guard_conservative_radius(&guard, NULL));

    /* This lies inside the broad enclosing sphere, but outside the true pyramid. */
    side_world = world_for(side_vertices, 3U, side_indices, side_triangles, 1U);
    status = mdkr_camera_sweep(&side_world, &(MdkrCameraSweepInput){
        .guard = { MDKR_CAMERA_LENS_GUARD_SPHERE, guard.broadphase_radius },
        .start_eye = eye, .desired_eye = eye,
    }, &sphere_hit);
    expect_true("enclosing sphere has side false positive", status == MDKR_CAMERA_SWEEP_HIT &&
                sphere_hit.started_overlapping);
    status = mdkr_camera_rounded_lens_guard_triangle_test(
        &guard, eye, (MdkrCameraVec3){ 5.0f, 8.0f, -0.25f },
        (MdkrCameraVec3){ 5.0f, 8.0f, 0.25f },
        (MdkrCameraVec3){ 5.0f, 8.5f, 0.0f }, &hit);
    expect_true("rounded lens rejects sphere-only side false positive", status == MDKR_CAMERA_SWEEP_CLEAR);
    expect_true("rounded lens publishes positive side clearance", hit.clearance > 0.0f);
    expect_true("rounded lens clear hit is finite", isfinite(hit.clearance) &&
                isfinite(hit.point.x) && isfinite(hit.normal.x));

    status = mdkr_camera_rounded_lens_guard_triangle_test(
        &guard, eye, (MdkrCameraVec3){ 10.25f, -1.0f, -1.0f },
        (MdkrCameraVec3){ 10.25f, 1.0f, -1.0f },
        (MdkrCameraVec3){ 10.25f, 0.0f, 1.0f }, &hit);
    expect_true("rounded lens skin catches near plane", status == MDKR_CAMERA_SWEEP_HIT && hit.overlapping);
    expect_near("rounded lens skin penetration", hit.penetration_depth, 0.25f, 0.002f);
    expect_near("rounded lens near plane normal", hit.normal.x, -1.0f, TEST_EPSILON);

    expect_true("thin rounded lens creates", mdkr_camera_rounded_lens_guard_from_projection(
        10.0f, 3.14159265358979323846f / 3.0f, 4.0f / 3.0f, 0.1f,
        (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f }, (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f },
        &thin_skin_guard));
    status = mdkr_camera_rounded_lens_guard_triangle_test(
        &thin_skin_guard, eye, (MdkrCameraVec3){ 10.25f, -1.0f, -1.0f },
        (MdkrCameraVec3){ 10.25f, 1.0f, -1.0f },
        (MdkrCameraVec3){ 10.25f, 0.0f, 1.0f }, &hit);
    expect_true("thin skin clears same near plane", status == MDKR_CAMERA_SWEEP_CLEAR);
    expect_near("thin skin exact clearance", hit.clearance, 0.15f, 0.002f);

    expect_true("rotated rounded lens creates", mdkr_camera_rounded_lens_guard_from_projection(
        10.0f, 3.14159265358979323846f / 3.0f, 4.0f / 3.0f, 0.0f,
        (MdkrCameraVec3){ 0.0f, 1.0f, 0.0f }, (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f },
        &rotated_guard));
    status = mdkr_camera_rounded_lens_guard_triangle_test(
        &rotated_guard, eye, (MdkrCameraVec3){ -1.0f, 10.0f, -1.0f },
        (MdkrCameraVec3){ 1.0f, 10.0f, -1.0f },
        (MdkrCameraVec3){ 0.0f, 10.0f, 1.0f }, &hit);
    expect_true("rotated near plane is protected", status == MDKR_CAMERA_SWEEP_HIT);
    expect_near("rotated near plane normal", hit.normal.y, -1.0f, TEST_EPSILON);
}

static void test_oriented_rounded_lens_invalid_and_pure(void) {
    MdkrCameraRoundedLensGuard guard;
    MdkrCameraRoundedLensGuard before_guard;
    MdkrCameraLensNarrowHit hit;
    MdkrCameraVec3 eye = { 0.0f, 0.0f, 0.0f };
    MdkrCameraVec3 a = { 10.0f, -1.0f, -1.0f };
    MdkrCameraVec3 b = { 10.0f, 1.0f, -1.0f };
    MdkrCameraVec3 c = { 10.0f, 0.0f, 1.0f };
    MdkrCameraVec3 before_a;
    MdkrCameraVec3 before_b;
    MdkrCameraVec3 before_c;

    expect_true("parallel rounded lens basis rejected", !mdkr_camera_rounded_lens_guard_from_projection(
        10.0f, 1.0f, 1.0f, 0.0f, (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f }, &guard));
    expect_true("valid rounded lens for purity", mdkr_camera_rounded_lens_guard_from_projection(
        10.0f, 1.0f, 1.0f, 0.0f, (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f }, &guard));
    before_guard = guard;
    before_a = a;
    before_b = b;
    before_c = c;
    expect_true("rounded lens pure test hits", mdkr_camera_rounded_lens_guard_triangle_test(
        &guard, eye, a, b, c, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("rounded lens guard unchanged", memcmp(&guard, &before_guard, sizeof(guard)) == 0);
    expect_true("rounded lens triangle a unchanged", memcmp(&a, &before_a, sizeof(a)) == 0);
    expect_true("rounded lens triangle b unchanged", memcmp(&b, &before_b, sizeof(b)) == 0);
    expect_true("rounded lens triangle c unchanged", memcmp(&c, &before_c, sizeof(c)) == 0);

    guard.right.x = 0.25f;
    expect_true("corrupt rounded basis rejected", mdkr_camera_rounded_lens_guard_triangle_test(
        &guard, eye, a, b, c, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    guard = before_guard;
    expect_true("degenerate narrow triangle rejected", mdkr_camera_rounded_lens_guard_triangle_test(
        &guard, eye, a, a, c, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_true("NaN narrow eye rejected", mdkr_camera_rounded_lens_guard_triangle_test(
        &guard, (MdkrCameraVec3){ NAN, 0.0f, 0.0f }, a, b, c, &hit) ==
        MDKR_CAMERA_SWEEP_INVALID);
}

static void test_oriented_rounded_lens_continuous_sweep(void) {
    const MdkrCameraVec3 vertices[] = {
        /* Broad sphere overlaps this at t=0; the real pyramid never does. */
        { 5.0f, 8.0f, -0.25f }, { 5.0f, 8.0f, 0.25f }, { 5.0f, 8.5f, 0.0f },
        /* This face is touched only while the lens passes through the corridor. */
        { 15.0f, -20.0f, -20.0f }, { 15.0f, 20.0f, -20.0f }, { 15.0f, 0.0f, 20.0f },
        /* The final near plane touches this only at the requested endpoint. */
        { 30.0f, -20.0f, -20.0f }, { 30.0f, 20.0f, -20.0f }, { 30.0f, 0.0f, 20.0f },
        /* A parallel edge meets the rounded near-plane edge, not its face. */
        { 15.0f, 6.0f, -20.0f }, { 15.0f, 6.0f, 20.0f }, { 15.0f, 7.0f, 0.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U };
    const MdkrCameraOcclusionTriangle triangles[] = {
        { 1U, 1U, 9U, 0U }, { 99U, 2U, 7U, 44U }, { 55U, 4U, 8U, 0U },
    };
    const uint32_t edge_indices[] = { 9U, 10U, 11U };
    const MdkrCameraOcclusionTriangle edge_triangles[] = { { 77U, 8U, 6U, 0U } };
    const uint32_t tie_indices[] = { 3U, 4U, 5U, 3U, 4U, 5U, 3U, 4U, 5U };
    const MdkrCameraOcclusionTriangle tie_triangles[] = {
        { 8U, 1U, 9U, 0U }, { 7U, 1U, 9U, 0U }, { 7U, 1U, 3U, 0U },
    };
    MdkrCameraOcclusionWorld world = world_for(vertices, 12U, indices, triangles, 3U);
    MdkrCameraOcclusionWorld edge_world = world_for(
        vertices, 12U, edge_indices, edge_triangles, 1U);
    MdkrCameraOcclusionWorld tie_world = world_for(
        vertices, 12U, tie_indices, tie_triangles, 3U);
    MdkrCameraRoundedLensSweepInput input;
    MdkrCameraRoundedLensGuard guard;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepHit repeat_hit;
    MdkrCameraSweepHit reference_hit;
    MdkrCameraSweepStatus status;
    MdkrCameraSweepStatus reference_status;
    MdkrCameraRoundedLensSweepTelemetry telemetry;
    MdkrCameraRoundedLensSweepTelemetry reference_telemetry;
    MdkrCameraLensNarrowHit narrow_hit;
    MdkrCameraRoundedLensSweepInput before_input;
    MdkrCameraOcclusionWorld before_world;
    MdkrCameraVec3 before_vertices[12];
    uint32_t before_indices[12];
    MdkrCameraOcclusionTriangle before_triangles[3];

    expect_true("continuous rounded lens creates", mdkr_camera_rounded_lens_guard_from_projection(
        10.0f, 3.14159265358979323846f / 3.0f, 1.0f, 0.5f,
        (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f }, (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f },
        &guard));
    input = (MdkrCameraRoundedLensSweepInput){
        .guard = guard,
        .start_eye = { 0.0f, 0.0f, 0.0f },
        .desired_eye = { 20.0f, 0.0f, 0.0f },
    };
    before_input = input;
    before_world = world;
    memcpy(before_vertices, vertices, sizeof(before_vertices));
    memcpy(before_indices, indices, sizeof(before_indices));
    memcpy(before_triangles, triangles, sizeof(before_triangles));

    expect_true("mid corridor start is clear", mdkr_camera_rounded_lens_guard_triangle_test(
        &guard, input.start_eye, vertices[3], vertices[4], vertices[5], &narrow_hit) ==
        MDKR_CAMERA_SWEEP_CLEAR);
    expect_true("mid corridor endpoint is clear", mdkr_camera_rounded_lens_guard_triangle_test(
        &guard, input.desired_eye, vertices[3], vertices[4], vertices[5], &narrow_hit) ==
        MDKR_CAMERA_SWEEP_CLEAR);
    expect_true("sphere takes side false positive first", mdkr_camera_sweep(
        &world, &(MdkrCameraSweepInput){
            .guard = { MDKR_CAMERA_LENS_GUARD_SPHERE, guard.broadphase_radius },
            .start_eye = input.start_eye, .desired_eye = input.desired_eye,
        }, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 1U);
    status = mdkr_camera_rounded_lens_sweep(&world, &input, &hit);
    reference_status = mdkr_camera_rounded_lens_sweep_reference(
        &world, &input, &reference_hit, &reference_telemetry);
    expect_true("continuous lens passes side false positive to mid blocker",
                status == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 99U && hit.kind == 7U);
    expect_near("continuous mid blocker fraction", hit.fraction, 0.225f, 0.002f);
    expect_true("continuous hit does not begin overlapping", !hit.started_overlapping);
    expect_near("continuous hit has zero contact clearance", hit.clearance, 0.0f, TEST_EPSILON);
    expect_true("continuous face contact classified", hit.feature == MDKR_CAMERA_SWEEP_FEATURE_FACE);
    expect_true("analytic and reference status agree", status == reference_status);
    expect_true("analytic and reference blocker agree",
                hit.stable_id == reference_hit.stable_id && hit.kind == reference_hit.kind &&
                hit.feature == reference_hit.feature);
    expect_near("analytic and reference fraction agree",
                hit.fraction, reference_hit.fraction, 0.002f);
    expect_true("reference keeps analytic path disabled",
                reference_telemetry.analytic_swept_sat_tests == 0U);
    expect_true("profiled continuous status",
                mdkr_camera_rounded_lens_sweep_profiled(
                    &world, &input, &repeat_hit, &telemetry) == status);
    expect_true("profiled continuous hit bytes", memcmp(&repeat_hit, &hit, sizeof(hit)) == 0);
    expect_true("profiled triangle accounting",
                telemetry.triangles_seen == 3U &&
                telemetry.triangles_filtered + telemetry.triangles_aabb_rejected +
                    telemetry.triangles_narrowed == telemetry.triangles_seen);
    expect_true("profiled exact work visible",
                telemetry.triangles_narrowed != 0U && telemetry.stationary_tests != 0U &&
                telemetry.analytic_swept_sat_tests == telemetry.triangles_narrowed &&
                telemetry.analytic_revalidation_misses == 0U &&
                telemetry.publication_revalidations == 1U);
    expect_true("continuous repeated status", mdkr_camera_rounded_lens_sweep(
        &world, &input, &repeat_hit) == status);
    expect_near("continuous repeated fraction", repeat_hit.fraction, hit.fraction, TEST_EPSILON);
    expect_true("continuous repeated hit bytes", memcmp(&repeat_hit, &hit, sizeof(hit)) == 0);
    expect_true("continuous ties use stable id then feature then index",
                mdkr_camera_rounded_lens_sweep(&tie_world, &input, &repeat_hit) ==
                MDKR_CAMERA_SWEEP_HIT && repeat_hit.stable_id == 7U && repeat_hit.kind == 9U);
    expect_true("continuous input unchanged", memcmp(&input, &before_input, sizeof(input)) == 0);
    expect_true("continuous world unchanged", memcmp(&world, &before_world, sizeof(world)) == 0);
    expect_true("continuous vertices unchanged", memcmp(vertices, before_vertices, sizeof(before_vertices)) == 0);
    expect_true("continuous indices unchanged", memcmp(indices, before_indices, sizeof(before_indices)) == 0);
    expect_true("continuous metadata unchanged", memcmp(triangles, before_triangles, sizeof(before_triangles)) == 0);

    input.mask = 1U;
    expect_true("continuous masked false positive clears", mdkr_camera_rounded_lens_sweep(
        &world, &input, &hit) == MDKR_CAMERA_SWEEP_CLEAR);
    input.mask = 2U;
    expect_true("continuous matching mask hits", mdkr_camera_rounded_lens_sweep(
        &world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 99U);
    input.mask = 0U;
    input.ignored_object_generation = 44U;
    input.mask = 2U;
    expect_true("continuous ignored generation clears", mdkr_camera_rounded_lens_sweep(
        &world, &input, &hit) == MDKR_CAMERA_SWEEP_CLEAR);

    input.ignored_object_generation = 0U;
    input.mask = 0U;
    expect_true("rounded skin edge static contact", mdkr_camera_rounded_lens_guard_triangle_test(
        &guard, (MdkrCameraVec3){ 5.0f, 0.0f, 0.0f }, vertices[9], vertices[10], vertices[11],
        &narrow_hit) == MDKR_CAMERA_SWEEP_HIT && narrow_hit.feature == MDKR_CAMERA_SWEEP_FEATURE_EDGE);
    input.start_eye = (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f };
    input.desired_eye = (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f };
    status = mdkr_camera_rounded_lens_sweep(&edge_world, &input, &hit);
    expect_true("continuous rounded skin edge hits", status == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 77U);
    expect_true("continuous rounded skin edge classified", hit.feature == MDKR_CAMERA_SWEEP_FEATURE_EDGE);

    input.start_eye = (MdkrCameraVec3){ 5.0f, 0.0f, 0.0f };
    input.desired_eye = (MdkrCameraVec3){ 20.0f, 0.0f, 0.0f };
    input.mask = 2U;
    expect_true("continuous initial rounded overlap", mdkr_camera_rounded_lens_sweep(
        &world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.started_overlapping);
    expect_near("continuous initial rounded overlap fraction", hit.fraction, 0.0f, TEST_EPSILON);
    expect_true("continuous initial rounded penetration", hit.penetration_depth > 0.0f);

    input.guard.skin = 0.0f;
    input.guard.broadphase_radius -= 0.5f;
    input.start_eye = (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f };
    input.desired_eye = (MdkrCameraVec3){ 20.0f, 0.0f, 0.0f };
    input.mask = 4U;
    expect_true("continuous endpoint touch hits", mdkr_camera_rounded_lens_sweep(
        &world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 55U);
    expect_near("continuous endpoint fraction", hit.fraction, 1.0f, TEST_EPSILON);
    expect_true("continuous endpoint touch is not overlap", !hit.started_overlapping);
}

static void test_oriented_rounded_lens_continuous_invalid(void) {
    const MdkrCameraVec3 vertices[] = {
        { 15.0f, -3.0f, -3.0f }, { 15.0f, 3.0f, -3.0f }, { 15.0f, 0.0f, 3.0f },
    };
    uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 9U, 1U, 1U, 0U } };
    MdkrCameraOcclusionWorld world = world_for(vertices, 3U, indices, triangles, 1U);
    MdkrCameraRoundedLensSweepInput input;
    MdkrCameraSweepHit hit;
    MdkrCameraRoundedLensSweepTelemetry telemetry;

    expect_true("continuous invalid lens creates", mdkr_camera_rounded_lens_guard_from_projection(
        10.0f, 1.0f, 1.0f, 0.0f, (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f }, &input.guard));
    input.start_eye = (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f };
    input.desired_eye = (MdkrCameraVec3){ 20.0f, 0.0f, 0.0f };
    input.mask = 0U;
    input.ignored_object_generation = 0U;
    memset(&telemetry, 0xA5, sizeof(telemetry));
    expect_true("profiled null input rejected", mdkr_camera_rounded_lens_sweep_profiled(
        &world, NULL, &hit, &telemetry) == MDKR_CAMERA_SWEEP_INVALID);
    expect_true("profiled invalid telemetry canonical",
                telemetry.triangles_seen == 0U && telemetry.stationary_tests == 0U &&
                telemetry.interval_fallbacks == 0U);
    indices[2] = 3U;
    expect_true("continuous invalid index rejected", mdkr_camera_rounded_lens_sweep(
        &world, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    indices[2] = 2U;
    world.triangle_count = SIZE_MAX;
    expect_true("continuous overflowing count rejected", mdkr_camera_rounded_lens_sweep(
        &world, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    world.triangle_count = 1U;
    input.guard.right.x = 0.25f;
    expect_true("continuous corrupt guard rejected", mdkr_camera_rounded_lens_sweep(
        &world, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    input.guard.right.x = 0.0f;
    input.guard.broadphase_radius *= 0.999f;
    expect_true("continuous undersized broad radius rejected", mdkr_camera_rounded_lens_sweep(
        &world, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
}

static void test_oriented_rounded_lens_tangent_and_long_clear(void) {
    const MdkrCameraVec3 tangent_vertices[] = {
        { 15.0f, 6.2735028f, -20.0f }, { 15.0f, 6.2735028f, 20.0f },
        { 15.0f, 7.2735028f, 0.0f },
    };
    const MdkrCameraVec3 long_clear_vertices[] = {
        { 500000.0f, 100.0f, -10.0f }, { 500000.0f, 100.0f, 10.0f },
        { 500000.0f, 101.0f, 0.0f },
    };
    const MdkrCameraVec3 slicing_vertices[] = {
        { 5.0f, -100.0f, -100.0f }, { 5.0f, 100.0f, -100.0f },
        { 5.0f, 0.0f, 100.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle tangent_triangles[] = { { 71U, 1U, 4U, 0U } };
    const MdkrCameraOcclusionTriangle long_clear_triangles[] = { { 72U, 1U, 4U, 0U } };
    const MdkrCameraOcclusionTriangle slicing_triangles[] = { { 73U, 1U, 4U, 0U } };
    MdkrCameraOcclusionWorld tangent_world = world_for(
        tangent_vertices, 3U, indices, tangent_triangles, 1U);
    MdkrCameraOcclusionWorld long_clear_world = world_for(
        long_clear_vertices, 3U, indices, long_clear_triangles, 1U);
    MdkrCameraOcclusionWorld slicing_world = world_for(
        slicing_vertices, 3U, indices, slicing_triangles, 1U);
    MdkrCameraRoundedLensSweepInput input;
    MdkrCameraRoundedLensSweepInput zero_skin_input;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepHit reference_hit;
    MdkrCameraSweepHit limited_hit;
    MdkrCameraRoundedLensSweepTelemetry work;
    MdkrCameraRoundedLensSweepTelemetry reference_work;
    MdkrCameraRoundedLensSweepTelemetry limited_work;

    expect_true("tangent rounded lens creates", mdkr_camera_rounded_lens_guard_from_projection(
        10.0f, 3.14159265358979323846f / 3.0f, 1.0f, 0.5f,
        (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f }, (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f },
        &input.guard));
    input.start_eye = (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f };
    input.desired_eye = (MdkrCameraVec3){ 20.0f, 0.0f, 0.0f };
    input.mask = 0U;
    input.ignored_object_generation = 0U;
    expect_true("rounded tangent static contact", mdkr_camera_rounded_lens_guard_triangle_test(
        &input.guard, (MdkrCameraVec3){ 5.0f, 0.0f, 0.0f }, tangent_vertices[0],
        tangent_vertices[1], tangent_vertices[2], &(MdkrCameraLensNarrowHit){ 0 }) ==
        MDKR_CAMERA_SWEEP_HIT);
    expect_true("rounded tangent sweep does not stall", mdkr_camera_rounded_lens_sweep_profiled(
        &tangent_world, &input, &hit, &work) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 71U);
    expect_true("rounded tangent contact is conservative", hit.fraction <= 0.25f + 0.002f);
    expect_true("rounded tangent uses bounded production path",
                work.analytic_revalidation_misses == 1U &&
                work.bounded_interval_tests > 0U &&
                work.bounded_interval_tests <= 96U &&
                work.bounded_interval_exhaustions == 0U &&
                work.interval_fallbacks == 0U && work.interval_samples == 0U &&
                work.stationary_tests <= 114U);
    expect_true("rounded tangent sampled oracle remains available",
                mdkr_camera_rounded_lens_sweep_reference(
                    &tangent_world, &input, &reference_hit, &reference_work) ==
                    MDKR_CAMERA_SWEEP_HIT && reference_work.analytic_swept_sat_tests == 0U &&
                    reference_work.interval_fallbacks > 0U &&
                    reference_work.interval_samples > 0U);
    memset(&limited_hit, 0xA5, sizeof(limited_hit));
    expect_true("rounded tangent work fence fails closed",
                mdkr_camera_rounded_lens_sweep_profiled_limited(
                    &tangent_world, &input, &limited_hit, &limited_work, 1U) ==
                    MDKR_CAMERA_SWEEP_INVALID && limited_work.stationary_tests == 1U &&
                    limited_hit.stable_id == 0U && limited_hit.fraction == 0.0f);
    memset(&limited_hit, 0xA5, sizeof(limited_hit));
    expect_true("rounded work fence cannot bypass telemetry accounting",
                mdkr_camera_rounded_lens_sweep_profiled_limited(
                    &tangent_world, &input, &limited_hit, NULL, 1U) ==
                    MDKR_CAMERA_SWEEP_INVALID && limited_hit.stable_id == 0U &&
                    limited_hit.fraction == 0.0f);

    expect_true("oversized slicing triangle has no contained vertices but intersects",
                mdkr_camera_rounded_lens_guard_triangle_test(
                    &input.guard, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f }, slicing_vertices[0],
                    slicing_vertices[1], slicing_vertices[2], &(MdkrCameraLensNarrowHit){ 0 }) ==
                MDKR_CAMERA_SWEEP_HIT);
    input.start_eye = (MdkrCameraVec3){ -20.0f, 0.0f, 0.0f };
    input.desired_eye = (MdkrCameraVec3){ 20.0f, 0.0f, 0.0f };
    expect_true("oversized slicing triangle continuous hit", mdkr_camera_rounded_lens_sweep(
        &slicing_world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 73U);
    zero_skin_input = input;
    zero_skin_input.guard.skin = 0.0f;
    zero_skin_input.guard.broadphase_radius -= 0.5f;
    zero_skin_input.start_eye = (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f };
    zero_skin_input.desired_eye = (MdkrCameraVec3){ 20.0f, 0.0f, 0.0f };
    expect_true("zero-skin initial slicing contact is overlap", mdkr_camera_rounded_lens_sweep(
        &slicing_world, &zero_skin_input, &hit) == MDKR_CAMERA_SWEEP_HIT &&
        hit.started_overlapping && hit.penetration_depth == 0.0f);

    input.start_eye = (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f };
    input.desired_eye = (MdkrCameraVec3){ 1000000.0f, 0.0f, 0.0f };
    expect_true("long clear rounded sweep does not quantization-stall",
                mdkr_camera_rounded_lens_sweep(&long_clear_world, &input, &hit) ==
                MDKR_CAMERA_SWEEP_CLEAR);
}

static void test_oriented_rounded_lens_high_coordinate_diagonal(void) {
    const MdkrCameraVec3 vertices[] = {
        { 100000512.0f, 100000480.0f, -100000032.0f },
        { 100000512.0f, 100000544.0f, -100000032.0f },
        { 100000512.0f, 100000512.0f, -99999968.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 88U, 1U, 5U, 0U } };
    MdkrCameraOcclusionWorld world = world_for(vertices, 3U, indices, triangles, 1U);
    MdkrCameraRoundedLensSweepInput input;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepHit repeat_hit;

    expect_true("high coordinate diagonal guard creates", mdkr_camera_rounded_lens_guard_from_projection(
        10.0f, 1.0f, 1.0f, 0.25f, (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f }, &input.guard));
    input.start_eye = (MdkrCameraVec3){ 100000000.0f, 100000000.0f, -100000000.0f };
    input.desired_eye = (MdkrCameraVec3){ 100001000.0f, 100001000.0f, -100000000.0f };
    input.mask = 0U;
    input.ignored_object_generation = 0U;
    expect_true("high coordinate diagonal thin blocker hits", mdkr_camera_rounded_lens_sweep(
        &world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 88U);
    expect_true("high coordinate diagonal repeat hits", mdkr_camera_rounded_lens_sweep(
        &world, &input, &repeat_hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("high coordinate diagonal hit bytes", memcmp(&hit, &repeat_hit, sizeof(hit)) == 0);
}

static void test_seeded_rounded_analytic_against_reference(void) {
    uint32_t state = 0x51A7C0DEU;
    unsigned int case_index;
    unsigned int hits = 0U;
    unsigned int clears = 0U;
    uint64_t analytic_misses = 0U;
    uint64_t max_stationary_tests = 0U;

    for (case_index = 0U; case_index < ROUNDED_ORACLE_CASES; case_index++) {
        MdkrCameraVec3 vertices[3];
        const uint32_t indices[] = { 0U, 1U, 2U };
        const MdkrCameraOcclusionTriangle triangles[] = {
            { 9000U + case_index, 1U, 17U, 0U },
        };
        MdkrCameraOcclusionWorld world;
        MdkrCameraRoundedLensSweepInput input;
        MdkrCameraSweepHit analytic_hit;
        MdkrCameraSweepHit reference_hit;
        MdkrCameraRoundedLensSweepTelemetry analytic_work;
        MdkrCameraRoundedLensSweepTelemetry reference_work;
        MdkrCameraSweepStatus analytic_status;
        MdkrCameraSweepStatus reference_status;
        const float plane_x = fuzz_range(&state, -25.0f, 25.0f);
        const float path_y = fuzz_range(&state, -20.0f, 20.0f);
        const float path_z = fuzz_range(&state, -20.0f, 20.0f);
        const float center_y = path_y + ((case_index % 3U) == 0U ?
            fuzz_range(&state, -3.0f, 3.0f) : fuzz_range(&state, 35.0f, 70.0f));
        const float center_z = path_z + ((case_index % 3U) == 0U ?
            fuzz_range(&state, -3.0f, 3.0f) : fuzz_range(&state, -70.0f, 70.0f));
        const float half_y = fuzz_range(&state, 4.0f, 30.0f);
        const float half_z = fuzz_range(&state, 4.0f, 30.0f);
        char label[96];

        vertices[0] = (MdkrCameraVec3){ plane_x, center_y - half_y, center_z - half_z };
        vertices[1] = (MdkrCameraVec3){ plane_x, center_y + half_y, center_z - half_z };
        vertices[2] = (MdkrCameraVec3){ plane_x, center_y, center_z + half_z };
        world = world_for(vertices, 3U, indices, triangles, 1U);
        expect_true("seeded rounded guard creates",
                    mdkr_camera_rounded_lens_guard_from_projection(
                        fuzz_range(&state, 2.0f, 14.0f),
                        fuzz_range(&state, 0.35f, 2.2f),
                        fuzz_range(&state, 0.5f, 3.5f),
                        fuzz_range(&state, 0.0f, 2.0f),
                        (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f },
                        (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f }, &input.guard));
        input.start_eye = (MdkrCameraVec3){ -60.0f, path_y, path_z };
        input.desired_eye = (MdkrCameraVec3){ 60.0f, path_y, path_z };
        input.mask = 0U;
        input.ignored_object_generation = 0U;
        analytic_status = mdkr_camera_rounded_lens_sweep_profiled(
            &world, &input, &analytic_hit, &analytic_work);
        reference_status = mdkr_camera_rounded_lens_sweep_reference(
            &world, &input, &reference_hit, &reference_work);
        snprintf(label, sizeof(label), "rounded oracle case %u status", case_index);
        expect_true(label, analytic_status == reference_status);
        snprintf(label, sizeof(label), "rounded oracle case %u bounded analytic", case_index);
        if (analytic_work.interval_fallbacks != 0U) {
            fprintf(stderr,
                    "rounded oracle diagnostic case=%u plane=%.9g path=(%.9g,%.9g) "
                    "center=(%.9g,%.9g) half=(%.9g,%.9g) near=%.9g width=%.9g "
                    "height=%.9g skin=%.9g miss=%llu fallback=%llu\n",
                    case_index, plane_x, path_y, path_z, center_y, center_z,
                    half_y, half_z, input.guard.near_distance,
                    input.guard.half_width, input.guard.half_height,
                    input.guard.skin,
                    (unsigned long long)analytic_work.analytic_revalidation_misses,
                    (unsigned long long)analytic_work.interval_fallbacks);
        }
        expect_true(label, analytic_work.analytic_swept_sat_tests <= 1U &&
                           analytic_work.bounded_interval_tests <= 96U &&
                           analytic_work.bounded_interval_exhaustions == 0U &&
                           analytic_work.interval_fallbacks == 0U &&
                           analytic_work.interval_samples == 0U &&
                           analytic_work.stationary_tests <= 114U);
        analytic_misses += analytic_work.analytic_revalidation_misses;
        if (analytic_work.stationary_tests > max_stationary_tests) {
            max_stationary_tests = analytic_work.stationary_tests;
        }
        if (analytic_status == MDKR_CAMERA_SWEEP_HIT) {
            snprintf(label, sizeof(label), "rounded oracle case %u blocker", case_index);
            expect_true(label, analytic_hit.stable_id == reference_hit.stable_id &&
                               analytic_hit.kind == reference_hit.kind);
            snprintf(label, sizeof(label), "rounded oracle case %u fraction", case_index);
            expect_near(label, analytic_hit.fraction, reference_hit.fraction, 0.003f);
            hits++;
        } else if (analytic_status == MDKR_CAMERA_SWEEP_CLEAR) {
            clears++;
        }
    }
    expect_true("rounded oracle corpus covers hits", hits >= 40U);
    expect_true("rounded oracle corpus covers clears", clears >= 80U);
    expect_true("rounded oracle analytic misses stay rare", analytic_misses <= 8U);
    expect_true("rounded oracle stationary work bounded", max_stationary_tests <= 114U);
}

/*
 * A reproducible, ROM-free metamorphic corpus. Coordinates stay in [-100, 100]
 * before transforms and [-200000, 200000] after them: deliberately far from float
 * overflow while still spanning several orders of magnitude in the ad-hoc tests
 * below. Every generated triangle has a substantial area by construction.
 */
static void test_seeded_metamorphic_sweeps(void) {
    uint32_t state = 0xC0FFEE11U;
    unsigned int case_index;
    unsigned int hits = 0U;
    unsigned int clears = 0U;

    for (case_index = 0U; case_index < METAMORPHIC_CASES; case_index++) {
        MdkrCameraVec3 vertices[3];
        MdkrCameraVec3 translated_vertices[3];
        MdkrCameraVec3 scaled_vertices[3];
        const uint32_t indices[] = { 0U, 1U, 2U };
        const MdkrCameraOcclusionTriangle triangles[] = {
            { 1000U + case_index, 1U, 50U + (case_index % 7U), 0U },
        };
        MdkrCameraOcclusionWorld world;
        MdkrCameraOcclusionWorld translated_world;
        MdkrCameraOcclusionWorld scaled_world;
        MdkrCameraSweepInput input;
        MdkrCameraSweepInput translated_input;
        MdkrCameraSweepInput scaled_input;
        MdkrCameraSweepInput smaller_input;
        MdkrCameraSweepInput larger_input;
        MdkrCameraSweepHit hit;
        MdkrCameraSweepHit repeat_hit;
        MdkrCameraSweepHit translated_hit;
        MdkrCameraSweepHit scaled_hit;
        MdkrCameraSweepHit smaller_hit;
        MdkrCameraSweepHit larger_hit;
        MdkrCameraSweepStatus status;
        MdkrCameraSweepStatus repeat_status;
        MdkrCameraSweepStatus translated_status;
        MdkrCameraSweepStatus scaled_status;
        MdkrCameraSweepStatus smaller_status;
        MdkrCameraSweepStatus larger_status;
        const float plane_x = fuzz_range(&state, -35.0f, 35.0f);
        const float center_y = fuzz_range(&state, -30.0f, 30.0f);
        const float center_z = fuzz_range(&state, -30.0f, 30.0f);
        const float half_y = fuzz_range(&state, 8.0f, 24.0f);
        const float half_z = fuzz_range(&state, 8.0f, 24.0f);
        const float radius = fuzz_range(&state, 0.25f, 4.0f);
        const float scale = (case_index & 1U) != 0U ? 0.125f : 16.0f;
        const MdkrCameraVec3 translation = {
            fuzz_range(&state, -10000.0f, 10000.0f),
            fuzz_range(&state, -10000.0f, 10000.0f),
            fuzz_range(&state, -10000.0f, 10000.0f),
        };
        const float path_y = (case_index & 1U) == 0U ? center_y : center_y + half_y + 20.0f;
        const float path_z = (case_index & 1U) == 0U ? center_z : center_z + half_z + 20.0f;
        unsigned int vertex_index;
        char label[96];

        vertices[0] = (MdkrCameraVec3){ plane_x, center_y - half_y, center_z - half_z };
        vertices[1] = (MdkrCameraVec3){ plane_x, center_y + half_y, center_z - half_z };
        vertices[2] = (MdkrCameraVec3){ plane_x, center_y, center_z + half_z };
        input = sweep_input((MdkrCameraVec3){ -100.0f, path_y, path_z },
                            (MdkrCameraVec3){ 100.0f, path_y, path_z }, radius);
        world = world_for(vertices, 3U, indices, triangles, 1U);
        status = mdkr_camera_sweep(&world, &input, &hit);
        repeat_status = mdkr_camera_sweep(&world, &input, &repeat_hit);
        snprintf(label, sizeof(label), "seeded case %u deterministic status", case_index);
        expect_true(label, status == repeat_status);
        snprintf(label, sizeof(label), "seeded case %u deterministic bytes", case_index);
        expect_true(label, memcmp(&hit, &repeat_hit, sizeof(hit)) == 0);
        if (status == MDKR_CAMERA_SWEEP_HIT) {
            const float normal_length = sqrtf(hit.normal.x * hit.normal.x + hit.normal.y * hit.normal.y + hit.normal.z * hit.normal.z);
            snprintf(label, sizeof(label), "seeded case %u finite bounded hit", case_index);
            expect_true(label, finite_hit(&hit));
            snprintf(label, sizeof(label), "seeded case %u unit normal", case_index);
            expect_near(label, normal_length, 1.0f, 0.002f);
            hits++;
        } else {
            snprintf(label, sizeof(label), "seeded case %u is valid", case_index);
            expect_true(label, status == MDKR_CAMERA_SWEEP_CLEAR);
            clears++;
        }

        for (vertex_index = 0U; vertex_index < 3U; vertex_index++) {
            translated_vertices[vertex_index] = (MdkrCameraVec3){
                vertices[vertex_index].x + translation.x,
                vertices[vertex_index].y + translation.y,
                vertices[vertex_index].z + translation.z,
            };
            scaled_vertices[vertex_index] = (MdkrCameraVec3){
                vertices[vertex_index].x * scale,
                vertices[vertex_index].y * scale,
                vertices[vertex_index].z * scale,
            };
        }
        translated_input = input;
        translated_input.start_eye.x += translation.x;
        translated_input.start_eye.y += translation.y;
        translated_input.start_eye.z += translation.z;
        translated_input.desired_eye.x += translation.x;
        translated_input.desired_eye.y += translation.y;
        translated_input.desired_eye.z += translation.z;
        scaled_input = input;
        scaled_input.guard.radius *= scale;
        scaled_input.start_eye.x *= scale;
        scaled_input.start_eye.y *= scale;
        scaled_input.start_eye.z *= scale;
        scaled_input.desired_eye.x *= scale;
        scaled_input.desired_eye.y *= scale;
        scaled_input.desired_eye.z *= scale;
        translated_world = world_for(translated_vertices, 3U, indices, triangles, 1U);
        scaled_world = world_for(scaled_vertices, 3U, indices, triangles, 1U);
        translated_status = mdkr_camera_sweep(&translated_world, &translated_input, &translated_hit);
        scaled_status = mdkr_camera_sweep(&scaled_world, &scaled_input, &scaled_hit);
        snprintf(label, sizeof(label), "seeded case %u translation", case_index);
        expect_equivalent_transform(label, status, &hit, translated_status, &translated_hit, 1.0f, translation);
        snprintf(label, sizeof(label), "seeded case %u scale", case_index);
        expect_equivalent_transform(label, status, &hit, scaled_status, &scaled_hit, scale, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f });

        smaller_input = sweep_input(input.start_eye, input.desired_eye, radius * 0.5f);
        larger_input = sweep_input(input.start_eye, input.desired_eye, radius * 1.5f);
        smaller_status = mdkr_camera_sweep(&world, &smaller_input, &smaller_hit);
        larger_status = mdkr_camera_sweep(&world, &larger_input, &larger_hit);
        snprintf(label, sizeof(label), "seeded case %u radius validity", case_index);
        expect_true(label, smaller_status != MDKR_CAMERA_SWEEP_INVALID && larger_status != MDKR_CAMERA_SWEEP_INVALID);
        if (smaller_status == MDKR_CAMERA_SWEEP_HIT) {
            snprintf(label, sizeof(label), "seeded case %u radius hit monotonic", case_index);
            expect_true(label, larger_status == MDKR_CAMERA_SWEEP_HIT);
            if (larger_status == MDKR_CAMERA_SWEEP_HIT) {
                snprintf(label, sizeof(label), "seeded case %u radius fraction monotonic", case_index);
                expect_true(label, larger_hit.fraction <= smaller_hit.fraction + 0.001f);
            }
        }
    }
    expect_true("seeded corpus exercises hits", hits > METAMORPHIC_CASES / 3U);
    expect_true("seeded corpus exercises clears", clears > METAMORPHIC_CASES / 3U);
}

static void test_adversarial_coordinate_scales(void) {
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 321U, 1U, 1U, 0U } };
    const MdkrCameraVec3 small_vertices[] = {
        { 0.0010000f, -0.0000100f, -0.0000100f },
        { 0.0010000f, 0.0000100f, -0.0000100f },
        { 0.0010000f, 0.0000000f, 0.0000100f },
    };
    const MdkrCameraVec3 large_vertices[] = {
        { 1.0e20f, -1.0e16f, -1.0e16f },
        { 1.0e20f, 1.0e16f, -1.0e16f },
        { 1.0e20f, 0.0f, 1.0e16f },
    };
    MdkrCameraSweepHit hit;
    MdkrCameraOcclusionWorld world;
    MdkrCameraSweepInput input;

    world = world_for(small_vertices, 3U, indices, triangles, 1U);
    input = sweep_input((MdkrCameraVec3){ 0.0009000f, 0.0f, 0.0f },
                        (MdkrCameraVec3){ 0.0011000f, 0.0f, 0.0f }, 0.0000010f);
    expect_true("small finite coordinates hit", mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("small finite coordinates bounded", finite_hit(&hit));

    world = world_for(large_vertices, 3U, indices, triangles, 1U);
    input = sweep_input((MdkrCameraVec3){ 1.0e20f - 1.0e16f, 0.0f, 0.0f },
                        (MdkrCameraVec3){ 1.0e20f + 1.0e16f, 0.0f, 0.0f }, 1.0e15f);
    expect_true("large finite coordinates hit", mdkr_camera_sweep(&world, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("large finite coordinates bounded", finite_hit(&hit));
    expect_near("large finite coordinates fraction", hit.fraction, 0.45f, 0.02f);
}

int main(void) {
    test_no_hit_and_endpoint_touch();
    test_face_edge_vertex_and_two_sided();
    test_overlap_earliest_tie_and_radius();
    test_pinned_concave_tunnel_and_thin_pillar();
    test_invalid_input_and_purity();
    test_translation_scale_and_projection_guard();
    test_near_plane_guard_beats_center_ray();
    test_oriented_rounded_lens_narrow_phase();
    test_oriented_rounded_lens_invalid_and_pure();
    test_oriented_rounded_lens_continuous_sweep();
    test_oriented_rounded_lens_continuous_invalid();
    test_oriented_rounded_lens_tangent_and_long_clear();
    test_oriented_rounded_lens_high_coordinate_diagonal();
    test_seeded_rounded_analytic_against_reference();
    test_seeded_metamorphic_sweeps();
    test_adversarial_coordinate_scales();

    if (s_failures != 0) {
        fprintf(stderr, "%d camera-obstruction assertion(s) failed\n", s_failures);
        return EXIT_FAILURE;
    }
    puts("camera-obstruction: all geometry assertions passed");
    return EXIT_SUCCESS;
}
