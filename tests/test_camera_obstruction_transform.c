#include "camera_obstruction_transform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define TEST_EPSILON 0.0005f

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void expect_near(const char *name, float actual, float expected, float tolerance) {
    if (!isfinite(actual) || fabsf(actual - expected) > tolerance) {
        fprintf(stderr, "FAIL %-36s actual=%f expected=%f (+/-%f)\n",
                name, actual, expected, tolerance);
        s_failures++;
    }
}

static void expect_vec_near(const char *name, MdkrCameraVec3 actual, MdkrCameraVec3 expected, float tolerance) {
    char label[96];

    snprintf(label, sizeof(label), "%s x", name);
    expect_near(label, actual.x, expected.x, tolerance);
    snprintf(label, sizeof(label), "%s y", name);
    expect_near(label, actual.y, expected.y, tolerance);
    snprintf(label, sizeof(label), "%s z", name);
    expect_near(label, actual.z, expected.z, tolerance);
}

static MdkrCameraSweepInput input_for(MdkrCameraVec3 start, MdkrCameraVec3 desired, float radius) {
    MdkrCameraSweepInput input;
    memset(&input, 0, sizeof(input));
    input.guard.kind = MDKR_CAMERA_LENS_GUARD_SPHERE;
    input.guard.radius = radius;
    input.start_eye = start;
    input.desired_eye = desired;
    return input;
}

static MdkrCameraOcclusionWorld local_wall_world(void) {
    static const MdkrCameraVec3 vertices[] = {
        { 5.0f, -5.0f, -5.0f }, { 5.0f, 5.0f, -5.0f }, { 5.0f, 0.0f, 5.0f },
    };
    static const uint32_t indices[] = { 0U, 1U, 2U };
    static const MdkrCameraOcclusionTriangle triangles[] = { { 71U, 1U, 19U, 77U } };
    MdkrCameraOcclusionWorld world = { vertices, 3U, indices, triangles, 1U };
    return world;
}

static MdkrCameraVec3 to_world(
    const MdkrCameraObjectTransform *transform,
    MdkrCameraVec3 local) {
    MdkrCameraVec3 world;
    expect_true("local point transforms to world", mdkr_camera_object_transform_point_to_world(
        transform, local, &world));
    return world;
}

static MdkrCameraVec3 vector_to_world(
    const MdkrCameraObjectTransform *transform,
    MdkrCameraVec3 local) {
    MdkrCameraVec3 world = to_world(transform, local);

    world.x -= transform->translation.x;
    world.y -= transform->translation.y;
    world.z -= transform->translation.z;
    return world;
}

static MdkrCameraVec3 normalize(MdkrCameraVec3 value) {
    const float length = sqrtf(value.x * value.x + value.y * value.y + value.z * value.z);

    expect_true("normal has nonzero length", isfinite(length) && length > 0.0f);
    value.x /= length;
    value.y /= length;
    value.z /= length;
    return value;
}

static void expect_exact_sweep_physical_equivalent(
    const char *name,
    MdkrCameraSweepStatus actual_status,
    const MdkrCameraSweepHit *actual,
    MdkrCameraSweepStatus expected_status,
    const MdkrCameraSweepHit *expected,
    float tolerance) {
    expect_true(name, actual_status == expected_status);
    if (actual_status != MDKR_CAMERA_SWEEP_HIT || expected_status != MDKR_CAMERA_SWEEP_HIT) {
        return;
    }
    expect_near("exact local fraction", actual->fraction, expected->fraction, tolerance);
    expect_near("exact local clearance", actual->clearance, expected->clearance, tolerance);
    expect_near("exact local penetration", actual->penetration_depth, expected->penetration_depth,
                tolerance);
    expect_vec_near("exact local normal", actual->normal, expected->normal, tolerance);
    expect_true("exact local metadata", actual->stable_id == expected->stable_id &&
                actual->kind == expected->kind && actual->feature == expected->feature &&
                actual->started_overlapping == expected->started_overlapping);
}

static void expect_point_on_triangle(
    const char *name,
    MdkrCameraVec3 point,
    MdkrCameraVec3 a,
    MdkrCameraVec3 b,
    MdkrCameraVec3 c,
    float tolerance) {
    const MdkrCameraVec3 ab = { b.x - a.x, b.y - a.y, b.z - a.z };
    const MdkrCameraVec3 ac = { c.x - a.x, c.y - a.y, c.z - a.z };
    const MdkrCameraVec3 ap = { point.x - a.x, point.y - a.y, point.z - a.z };
    const float dot_ab_ab = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    const float dot_ab_ac = ab.x * ac.x + ab.y * ac.y + ab.z * ac.z;
    const float dot_ac_ac = ac.x * ac.x + ac.y * ac.y + ac.z * ac.z;
    const float dot_ap_ab = ap.x * ab.x + ap.y * ab.y + ap.z * ab.z;
    const float dot_ap_ac = ap.x * ac.x + ap.y * ac.y + ap.z * ac.z;
    const float denominator = dot_ab_ab * dot_ac_ac - dot_ab_ac * dot_ab_ac;
    const MdkrCameraVec3 normal = {
        ab.y * ac.z - ab.z * ac.y,
        ab.z * ac.x - ab.x * ac.z,
        ab.x * ac.y - ab.y * ac.x,
    };
    const float normal_length = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    const float plane_distance = fabsf(ap.x * normal.x + ap.y * normal.y + ap.z * normal.z) /
                                 normal_length;
    const float u = (dot_ac_ac * dot_ap_ab - dot_ab_ac * dot_ap_ac) / denominator;
    const float v = (dot_ab_ab * dot_ap_ac - dot_ab_ac * dot_ap_ab) / denominator;

    expect_true("point triangle normal valid", isfinite(normal_length) && normal_length > 0.0f &&
                isfinite(denominator) && fabsf(denominator) > 0.0f);
    expect_true(name, isfinite(plane_distance) && plane_distance <= tolerance &&
                isfinite(u) && isfinite(v) && u >= -tolerance && v >= -tolerance &&
                u + v <= 1.0f + tolerance);
}

static void test_rounded_lens_local_continuous_sweep(void) {
    const MdkrCameraVec3 local_vertices[] = {
        { 9.0f, 0.0f, 0.0f }, { 9.0f, 3.0f, 0.0f }, { 9.0f, 0.0f, 3.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 313U, 4U, 29U, 88U } };
    MdkrCameraOcclusionWorld local_world = { local_vertices, 3U, indices, triangles, 1U };
    MdkrCameraVec3 world_vertices[3];
    MdkrCameraOcclusionWorld world;
    MdkrCameraObjectTransform transform;
    MdkrCameraRoundedLensSweepInput world_input;
    MdkrCameraRoundedLensSweepInput local_input;
    MdkrCameraRoundedLensSweepInput input_before;
    MdkrCameraObjectTransform transform_before;
    MdkrCameraOcclusionWorld world_before;
    MdkrCameraSweepHit direct_hit;
    MdkrCameraSweepHit core_local_hit;
    MdkrCameraSweepHit local_hit;
    MdkrCameraSweepHit profiled_hit;
    MdkrCameraRoundedLensSweepTelemetry profiled_work;
    MdkrCameraSweepStatus direct_status;
    MdkrCameraSweepStatus core_local_status;
    MdkrCameraSweepStatus local_status;
    MdkrCameraVec3 world_forward;
    MdkrCameraVec3 world_up;
    size_t index;

    expect_true("construct exact local transform", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3){ 70.0f, -45.0f, 125.0f }, 0.8f, -0.25f, 0.45f, 2.5f, &transform));
    world_forward = vector_to_world(&transform, (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f });
    world_up = vector_to_world(&transform, (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f });
    expect_true("construct exact world guard", mdkr_camera_rounded_lens_guard_from_projection(
        5.0f, 3.14159265358979323846f / 3.0f, 1.0f, 0.5f, world_forward, world_up,
        &world_input.guard));
    world_input.start_eye = to_world(&transform, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f });
    world_input.desired_eye = to_world(&transform, (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    world_input.mask = 4U;
    world_input.ignored_object_generation = 0U;
    for (index = 0U; index < 3U; index++) {
        world_vertices[index] = to_world(&transform, local_vertices[index]);
    }
    world = (MdkrCameraOcclusionWorld){ world_vertices, 3U, indices, triangles, 1U };
    input_before = world_input;
    transform_before = transform;
    world_before = world;

    expect_true("exact adapter converts local input", mdkr_camera_rounded_lens_guard_to_object_local(
        &transform, &world_input.guard, world_input.start_eye, &local_input.start_eye,
        &local_input.guard));
    expect_true("exact adapter converts desired eye", mdkr_camera_object_transform_point_to_local(
        &transform, world_input.desired_eye, &local_input.desired_eye));
    local_input.mask = world_input.mask;
    local_input.ignored_object_generation = world_input.ignored_object_generation;
    core_local_status = mdkr_camera_rounded_lens_sweep(&local_world, &local_input, &core_local_hit);
    direct_status = mdkr_camera_rounded_lens_sweep(&world, &world_input, &direct_hit);
    local_status = mdkr_camera_rounded_lens_sweep_object_local(
        &local_world, &transform, &world_input, &local_hit);
    expect_true("exact profiled local status",
                mdkr_camera_rounded_lens_sweep_object_local_profiled(
                    &local_world, &transform, &world_input, &profiled_hit,
                    &profiled_work) == local_status);
    expect_true("exact profiled local hit bytes",
                memcmp(&profiled_hit, &local_hit, sizeof(local_hit)) == 0);
    expect_true("exact profiled local work",
                profiled_work.triangles_seen == 1U &&
                profiled_work.triangles_narrowed == 1U &&
                profiled_work.stationary_tests > 0U);
    expect_exact_sweep_physical_equivalent("exact rotated local sweep matches world", local_status,
                                           &local_hit, direct_status, &direct_hit, 0.004f);
    expect_true("exact mid corridor hits", local_status == MDKR_CAMERA_SWEEP_HIT &&
                local_hit.stable_id == 313U && local_hit.kind == 29U &&
                local_hit.fraction > 0.5f && local_hit.fraction < 1.0f &&
                !local_hit.started_overlapping);
    expect_true("exact adapter preserves local metadata", local_status == core_local_status &&
                local_hit.stable_id == core_local_hit.stable_id &&
                local_hit.kind == core_local_hit.kind &&
                local_hit.feature == core_local_hit.feature &&
                local_hit.started_overlapping == core_local_hit.started_overlapping);
    expect_vec_near("exact adapter point maps core local witness", local_hit.point,
                    to_world(&transform, core_local_hit.point), 0.004f);
    expect_vec_near("exact adapter normal maps core local witness", local_hit.normal,
                    normalize(vector_to_world(&transform, core_local_hit.normal)), 0.004f);
    expect_near("exact adapter clearance scales core local witness", local_hit.clearance,
                core_local_hit.clearance * 2.5f, 0.004f);
    expect_near("exact adapter penetration scales core local witness", local_hit.penetration_depth,
                core_local_hit.penetration_depth * 2.5f, 0.004f);
    expect_point_on_triangle("exact direct witness is on world triangle", direct_hit.point,
                             world_vertices[0], world_vertices[1], world_vertices[2], 0.004f);
    expect_point_on_triangle("exact adapter witness is on world triangle", local_hit.point,
                             world_vertices[0], world_vertices[1], world_vertices[2], 0.004f);
    expect_true("exact local input purity", memcmp(
        &world_input, &input_before, sizeof(world_input)) == 0);
    expect_true("exact local transform purity", memcmp(
        &transform, &transform_before, sizeof(transform)) == 0);
    expect_true("exact local world purity", memcmp(&world, &world_before, sizeof(world)) == 0);

    world_input.start_eye = to_world(&transform, (MdkrCameraVec3){ 7.0f, 0.0f, 0.0f });
    direct_status = mdkr_camera_rounded_lens_sweep(&world, &world_input, &direct_hit);
    local_status = mdkr_camera_rounded_lens_sweep_object_local(
        &local_world, &transform, &world_input, &local_hit);
    expect_exact_sweep_physical_equivalent("exact local initial overlap matches world", local_status,
                                           &local_hit, direct_status, &direct_hit, 0.004f);
    expect_point_on_triangle("exact direct initial witness is on world triangle", direct_hit.point,
                             world_vertices[0], world_vertices[1], world_vertices[2], 0.004f);
    expect_point_on_triangle("exact adapter initial witness is on world triangle", local_hit.point,
                             world_vertices[0], world_vertices[1], world_vertices[2], 0.004f);
    expect_true("exact local initial overlap", local_status == MDKR_CAMERA_SWEEP_HIT &&
                local_hit.fraction == 0.0f && local_hit.started_overlapping);
}

static void test_rounded_lens_local_continuous_invalid(void) {
    MdkrCameraOcclusionWorld world = local_wall_world();
    MdkrCameraObjectTransform transform;
    MdkrCameraRoundedLensSweepInput input;
    MdkrCameraSweepHit hit;

    expect_true("construct exact invalid transform", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f, 2.0f, &transform));
    expect_true("construct exact invalid guard", mdkr_camera_rounded_lens_guard_from_projection(
        4.0f, 1.0f, 1.0f, 0.1f, (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f }, &input.guard));
    input.start_eye = (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f };
    input.desired_eye = (MdkrCameraVec3){ 20.0f, 0.0f, 0.0f };
    input.mask = 0U;
    input.ignored_object_generation = 0U;
    transform.local_x_axis.x = -transform.local_x_axis.x;
    expect_true("exact local mirror rejected", mdkr_camera_rounded_lens_sweep_object_local(
        &world, &transform, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_true("exact local mirror clears output", hit.fraction == 0.0f && hit.stable_id == 0U);
    expect_true("construct exact invalid input transform", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f, 2.0f, &transform));
    input.desired_eye.x = NAN;
    expect_true("exact local nonfinite input rejected", mdkr_camera_rounded_lens_sweep_object_local(
        &world, &transform, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_true("exact local nonfinite clears output", hit.fraction == 0.0f && hit.stable_id == 0U);
}

static void test_rounded_lens_local_contract(void) {
    MdkrCameraObjectTransform transform;
    MdkrCameraRoundedLensGuard world_guard;
    MdkrCameraRoundedLensGuard local_guard;
    MdkrCameraRoundedLensGuard world_guard_before;
    MdkrCameraObjectTransform transform_before;
    MdkrCameraVec3 world_eye;
    MdkrCameraVec3 local_eye;
    MdkrCameraVec3 local_eye_before;
    MdkrCameraVec3 local_a;
    MdkrCameraVec3 local_b;
    MdkrCameraVec3 local_c;
    MdkrCameraVec3 local_center;
    MdkrCameraVec3 world_a;
    MdkrCameraVec3 world_b;
    MdkrCameraVec3 world_c;
    MdkrCameraVec3 world_eye_before;
    MdkrCameraVec3 local_a_before;
    MdkrCameraVec3 local_b_before;
    MdkrCameraVec3 local_c_before;
    MdkrCameraLensNarrowHit local_hit;
    MdkrCameraLensNarrowHit world_hit;
    MdkrCameraSweepStatus local_status;
    MdkrCameraSweepStatus world_status;
    MdkrCameraVec3 handed;

    expect_true("construct rotation scale transform", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3){ 120.0f, -55.0f, 30.0f }, 0.6f, -0.35f, 0.2f, 2.75f, &transform));
    /* A deliberately world-oriented guard verifies all three axes are remapped. */
    expect_true("construct world rounded lens", mdkr_camera_rounded_lens_guard_from_projection(
        11.0f, 1.05f, 1.6f, 0.35f, (MdkrCameraVec3){ 0.3f, 0.4f, 0.85f },
        (MdkrCameraVec3){ -0.2f, 0.9f, 0.1f }, &world_guard));
    world_eye = to_world(&transform, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f });
    world_guard_before = world_guard;
    transform_before = transform;
    local_eye = (MdkrCameraVec3){ -1000.0f, -1000.0f, -1000.0f };
    local_eye_before = local_eye;
    expect_true("rounded lens maps to object local", mdkr_camera_rounded_lens_guard_to_object_local(
        &transform, &world_guard, world_eye, &local_eye, &local_guard));
    expect_vec_near("rounded lens local eye", local_eye, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f },
                    TEST_EPSILON);
    expect_near("rounded lens local near", local_guard.near_distance, 4.0f, TEST_EPSILON);
    expect_near("rounded lens local half width", local_guard.half_width,
                world_guard.half_width / 2.75f, TEST_EPSILON);
    expect_near("rounded lens local half height", local_guard.half_height,
                world_guard.half_height / 2.75f, TEST_EPSILON);
    expect_near("rounded lens local skin", local_guard.skin, world_guard.skin / 2.75f,
                TEST_EPSILON);
    expect_near("rounded lens local broadphase", local_guard.broadphase_radius,
                world_guard.broadphase_radius / 2.75f, TEST_EPSILON);
    handed = (MdkrCameraVec3){
        local_guard.right.y * local_guard.up.z - local_guard.right.z * local_guard.up.y,
        local_guard.right.z * local_guard.up.x - local_guard.right.x * local_guard.up.z,
        local_guard.right.x * local_guard.up.y - local_guard.right.y * local_guard.up.x,
    };
    expect_vec_near("local renderer handedness", handed,
                    (MdkrCameraVec3){ -local_guard.forward.x, -local_guard.forward.y,
                                       -local_guard.forward.z }, TEST_EPSILON);
    expect_true("rounded lens transform input purity", memcmp(
        &transform, &transform_before, sizeof(transform)) == 0);
    expect_true("rounded lens guard input purity", memcmp(
        &world_guard, &world_guard_before, sizeof(world_guard)) == 0);
    expect_true("rounded lens local output overwritten", memcmp(
        &local_eye, &local_eye_before, sizeof(local_eye)) != 0);

    /* A near-plane triangle exercises the exact narrow path under rotation and scale. */
    local_center = (MdkrCameraVec3){
        local_guard.forward.x * (local_guard.near_distance + local_guard.skin * 0.5f),
        local_guard.forward.y * (local_guard.near_distance + local_guard.skin * 0.5f),
        local_guard.forward.z * (local_guard.near_distance + local_guard.skin * 0.5f),
    };
    local_a = (MdkrCameraVec3){
        local_center.x - local_guard.right.x * local_guard.half_width * 0.2f -
            local_guard.up.x * local_guard.half_height * 0.2f,
        local_center.y - local_guard.right.y * local_guard.half_width * 0.2f -
            local_guard.up.y * local_guard.half_height * 0.2f,
        local_center.z - local_guard.right.z * local_guard.half_width * 0.2f -
            local_guard.up.z * local_guard.half_height * 0.2f,
    };
    local_b = (MdkrCameraVec3){
        local_center.x + local_guard.right.x * local_guard.half_width * 0.2f -
            local_guard.up.x * local_guard.half_height * 0.2f,
        local_center.y + local_guard.right.y * local_guard.half_width * 0.2f -
            local_guard.up.y * local_guard.half_height * 0.2f,
        local_center.z + local_guard.right.z * local_guard.half_width * 0.2f -
            local_guard.up.z * local_guard.half_height * 0.2f,
    };
    local_c = (MdkrCameraVec3){
        local_center.x + local_guard.up.x * local_guard.half_height * 0.2f,
        local_center.y + local_guard.up.y * local_guard.half_height * 0.2f,
        local_center.z + local_guard.up.z * local_guard.half_height * 0.2f,
    };
    world_a = to_world(&transform, local_a);
    world_b = to_world(&transform, local_b);
    world_c = to_world(&transform, local_c);
    world_eye_before = world_eye;
    local_a_before = local_a;
    local_b_before = local_b;
    local_c_before = local_c;
    local_status = mdkr_camera_rounded_lens_guard_triangle_test(
        &local_guard, local_eye, local_a, local_b, local_c, &local_hit);
    world_status = mdkr_camera_rounded_lens_guard_triangle_test(
        &world_guard, world_eye, world_a, world_b, world_c, &world_hit);
    expect_true("rounded lens local world status matches", local_status == world_status);
    expect_true("rounded lens local narrow hit", local_status == MDKR_CAMERA_SWEEP_HIT);
    expect_true("rounded lens local overlap matches", local_hit.overlapping == world_hit.overlapping);
    expect_true("rounded lens local feature matches", local_hit.feature == world_hit.feature);
    expect_near("rounded lens clearance scales", world_hit.clearance,
                local_hit.clearance * 2.75f, 0.002f);
    expect_near("rounded lens penetration scales", world_hit.penetration_depth,
                local_hit.penetration_depth * 2.75f, 0.002f);
    expect_vec_near("rounded lens point maps to world", to_world(&transform, local_hit.point),
                    world_hit.point, 0.002f);
    expect_vec_near("rounded lens normal maps to world", normalize(vector_to_world(
        &transform, local_hit.normal)), world_hit.normal, 0.002f);
    expect_true("rounded lens eye input purity", memcmp(
        &world_eye, &world_eye_before, sizeof(world_eye)) == 0);
    expect_true("rounded lens triangle a input purity", memcmp(
        &local_a, &local_a_before, sizeof(local_a)) == 0);
    expect_true("rounded lens triangle b input purity", memcmp(
        &local_b, &local_b_before, sizeof(local_b)) == 0);
    expect_true("rounded lens triangle c input purity", memcmp(
        &local_c, &local_c_before, sizeof(local_c)) == 0);
}

static void test_rounded_lens_local_invalid_contracts(void) {
    MdkrCameraObjectTransform transform;
    MdkrCameraRoundedLensGuard guard;
    MdkrCameraRoundedLensGuard output_guard;
    MdkrCameraVec3 output_eye;

    expect_true("construct local contract baseline", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3){ 1.0f, 2.0f, 3.0f }, 0.1f, 0.2f, 0.3f, 2.0f, &transform));
    expect_true("construct local contract guard", mdkr_camera_rounded_lens_guard_from_projection(
        5.0f, 1.0f, 1.5f, 0.1f, (MdkrCameraVec3){ 1.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 0.0f, 0.0f, 1.0f }, &guard));
    transform.local_x_axis.x = -transform.local_x_axis.x;
    transform.local_x_axis.y = -transform.local_x_axis.y;
    transform.local_x_axis.z = -transform.local_x_axis.z;
    expect_true("rounded lens mirror rejected", !mdkr_camera_rounded_lens_guard_to_object_local(
        &transform, &guard, (MdkrCameraVec3){ 1.0f, 2.0f, 3.0f }, &output_eye, &output_guard));
    expect_true("construct nonuniform contract", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3){ 1.0f, 2.0f, 3.0f }, 0.1f, 0.2f, 0.3f, 2.0f, &transform));
    transform.local_y_axis.x *= 1.1f;
    transform.local_y_axis.y *= 1.1f;
    transform.local_y_axis.z *= 1.1f;
    expect_true("rounded lens nonuniform rejected", !mdkr_camera_rounded_lens_guard_to_object_local(
        &transform, &guard, (MdkrCameraVec3){ 1.0f, 2.0f, 3.0f }, &output_eye, &output_guard));
    expect_true("construct shear contract", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3){ 1.0f, 2.0f, 3.0f }, 0.1f, 0.2f, 0.3f, 2.0f, &transform));
    transform.local_y_axis.x += transform.local_x_axis.x * 0.1f;
    transform.local_y_axis.y += transform.local_x_axis.y * 0.1f;
    transform.local_y_axis.z += transform.local_x_axis.z * 0.1f;
    expect_true("rounded lens shear rejected", !mdkr_camera_rounded_lens_guard_to_object_local(
        &transform, &guard, (MdkrCameraVec3){ 1.0f, 2.0f, 3.0f }, &output_eye, &output_guard));
    expect_true("construct nonfinite contract", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3){ 1.0f, 2.0f, 3.0f }, 0.1f, 0.2f, 0.3f, 2.0f, &transform));
    transform.translation.x = INFINITY;
    expect_true("rounded lens nonfinite transform rejected", !mdkr_camera_rounded_lens_guard_to_object_local(
        &transform, &guard, (MdkrCameraVec3){ 1.0f, 2.0f, 3.0f }, &output_eye, &output_guard));
    expect_true("construct invalid guard baseline", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3){ 1.0f, 2.0f, 3.0f }, 0.1f, 0.2f, 0.3f, 2.0f, &transform));
    expect_true("rounded lens nonfinite eye rejected", !mdkr_camera_rounded_lens_guard_to_object_local(
        &transform, &guard, (MdkrCameraVec3){ NAN, 2.0f, 3.0f }, &output_eye, &output_guard));
    guard.forward.x = NAN;
    expect_true("rounded lens nonfinite guard rejected", !mdkr_camera_rounded_lens_guard_to_object_local(
        &transform, &guard, (MdkrCameraVec3){ 1.0f, 2.0f, 3.0f }, &output_eye, &output_guard));
    guard.forward.x = 1.0f;
    guard.right.x = 0.25f;
    expect_true("rounded lens invalid handed basis rejected", !mdkr_camera_rounded_lens_guard_to_object_local(
        &transform, &guard, (MdkrCameraVec3){ 1.0f, 2.0f, 3.0f }, &output_eye, &output_guard));
}

static void test_round_trip_and_transformed_sweep(void) {
    MdkrCameraOcclusionWorld world = local_wall_world();
    MdkrCameraObjectTransform transform;
    MdkrCameraVec3 local_point = { 2.0f, -3.0f, 4.0f };
    MdkrCameraVec3 world_point;
    MdkrCameraVec3 recovered_point;
    MdkrCameraVec3 expected_normal;
    MdkrCameraSweepInput input;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepHit repeat_hit;
    MdkrCameraSweepStatus status;
    float scale;

    expect_true("construct yaw pitch roll transform",
                mdkr_camera_object_transform_from_yaw_pitch_roll(
                    (MdkrCameraVec3){ 50.0f, -30.0f, 70.0f }, 0.7f, -0.3f, 0.5f, 2.5f, &transform));
    expect_true("validate transform", mdkr_camera_object_transform_validate(&transform, &scale));
    expect_near("uniform scale", scale, 2.5f, TEST_EPSILON);
    world_point = to_world(&transform, local_point);
    expect_true("world point transforms to local", mdkr_camera_object_transform_point_to_local(
        &transform, world_point, &recovered_point));
    expect_vec_near("local world local round trip", recovered_point, local_point, TEST_EPSILON);

    input = input_for(to_world(&transform, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f }),
                      to_world(&transform, (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f }), 2.5f);
    status = mdkr_camera_sweep_object_local(&world, &transform, &input, &hit);
    expect_true("yaw pitch roll sweep hits", status == MDKR_CAMERA_SWEEP_HIT);
    expect_near("transformed fraction", hit.fraction, 0.4f, TEST_EPSILON);
    expect_true("stable id survives transformed sweep", hit.stable_id == 71U && hit.kind == 19U);
    expect_vec_near("transformed hit point", hit.point,
                    to_world(&transform, (MdkrCameraVec3){ 5.0f, 0.0f, 0.0f }), TEST_EPSILON);
    expected_normal = to_world(&transform, (MdkrCameraVec3){ -1.0f, 0.0f, 0.0f });
    expected_normal.x -= transform.translation.x;
    expected_normal.y -= transform.translation.y;
    expected_normal.z -= transform.translation.z;
    expected_normal.x /= 2.5f;
    expected_normal.y /= 2.5f;
    expected_normal.z /= 2.5f;
    expect_vec_near("transformed hit normal", hit.normal, expected_normal, TEST_EPSILON);
    expect_near("world clearance", hit.clearance, 0.0f, TEST_EPSILON);
    status = mdkr_camera_sweep_object_local(&world, &transform, &input, &repeat_hit);
    expect_true("transformed deterministic status", status == MDKR_CAMERA_SWEEP_HIT);
    expect_true("transformed deterministic bytes", memcmp(&hit, &repeat_hit, sizeof(hit)) == 0);
}

static void test_overlap_masks_and_ignored_generation(void) {
    MdkrCameraOcclusionWorld world = local_wall_world();
    MdkrCameraObjectTransform transform;
    MdkrCameraSweepInput input;
    MdkrCameraSweepHit hit;

    expect_true("construct translation transform", mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3){ 20.0f, 30.0f, -40.0f }, 0.0f, 0.0f, 0.0f, 3.0f, &transform));
    input = input_for(to_world(&transform, (MdkrCameraVec3){ 4.5f, 0.0f, 0.0f }),
                      to_world(&transform, (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f }), 3.0f);
    expect_true("transformed overlap hits", mdkr_camera_sweep_object_local(
        &world, &transform, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("transformed overlap marked", hit.started_overlapping);
    expect_near("transformed penetration", hit.penetration_depth, 1.5f, TEST_EPSILON);
    expect_near("transformed overlap clearance", hit.clearance, -1.5f, TEST_EPSILON);
    expect_vec_near("transformed depenetration normal", hit.normal,
                    (MdkrCameraVec3){ -1.0f, 0.0f, 0.0f }, TEST_EPSILON);

    input.ignored_object_generation = 77U;
    expect_true("ignored generation is preserved", mdkr_camera_sweep_object_local(
        &world, &transform, &input, &hit) == MDKR_CAMERA_SWEEP_CLEAR);
    input.ignored_object_generation = 0U;
    input.mask = 2U;
    expect_true("mask is preserved", mdkr_camera_sweep_object_local(
        &world, &transform, &input, &hit) == MDKR_CAMERA_SWEEP_CLEAR);
    input.mask = 1U;
    expect_true("matching mask is preserved", mdkr_camera_sweep_object_local(
        &world, &transform, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
}

static void test_invalid_transforms_fail_closed(void) {
    MdkrCameraOcclusionWorld world = local_wall_world();
    MdkrCameraObjectTransform transform;
    MdkrCameraSweepInput input = input_for(
        (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f }, (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f }, 1.0f);
    MdkrCameraSweepHit hit;

    memset(&hit, 0xA5, sizeof(hit));

    expect_true("negative scale constructor rejected", !mdkr_camera_object_transform_from_yaw_pitch_roll(
        (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f, -1.0f, &transform));
    transform = (MdkrCameraObjectTransform){
        { 0.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
    };
    expect_true("negative handedness rejected", mdkr_camera_sweep_object_local(
        &world, &transform, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_true("invalid transform clears output", hit.fraction == 0.0f && hit.stable_id == 0U);
    transform = (MdkrCameraObjectTransform){
        { 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
    };
    expect_true("nonuniform scale rejected", mdkr_camera_sweep_object_local(
        &world, &transform, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    transform = (MdkrCameraObjectTransform){
        { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.1f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
    };
    expect_true("shear rejected", mdkr_camera_sweep_object_local(
        &world, &transform, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    transform = (MdkrCameraObjectTransform){
        { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
    };
    expect_true("singular basis rejected", mdkr_camera_sweep_object_local(
        &world, &transform, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    transform = (MdkrCameraObjectTransform){
        { NAN, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
    };
    expect_true("NaN transform rejected", mdkr_camera_sweep_object_local(
        &world, &transform, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
}

int main(void) {
    test_rounded_lens_local_continuous_sweep();
    test_rounded_lens_local_continuous_invalid();
    test_rounded_lens_local_contract();
    test_rounded_lens_local_invalid_contracts();
    test_round_trip_and_transformed_sweep();
    test_overlap_masks_and_ignored_generation();
    test_invalid_transforms_fail_closed();

    if (s_failures != 0) {
        fprintf(stderr, "%d camera-obstruction-transform assertion(s) failed\n", s_failures);
        return EXIT_FAILURE;
    }
    puts("camera-obstruction-transform: all assertions passed");
    return EXIT_SUCCESS;
}
