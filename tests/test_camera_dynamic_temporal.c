#include "camera_dynamic_temporal.h"
#include "math_util.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DKR_ANGLE_TO_RAD (6.28318530717958647692f / 65536.0f)
#define DENSE_SAMPLES 4096

static int sFailures;
static uint32_t sRandomState = UINT32_C(0xC04D00F1);

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        sFailures++;
    }
}

static s32 test_sine(s16 angle) {
    const u16 turn = (u16)angle;

    if (turn == 0U || turn == 0x8000U) return 0;
    if (turn == 0x4000U) return 0x10000;
    if (turn == 0xC000U) return -0x10000;
    return (s32)lroundf(sinf((float)turn * DKR_ANGLE_TO_RAD) * 65536.0f);
}

s32 sins_s16(s16 angle) {
    return test_sine(angle);
}

s32 coss_s16(s16 angle) {
    return test_sine((s16)(angle + 0x4000));
}

/* Test-local renderer oracle: byte-for-byte C recipe used by the production
 * native transform, including Z->X->Y order and fixed-angle quantization. */
void mtxf_from_transform(MtxF *mtx, ObjectTransform *trans) {
    const f32 ys = sins_s16(trans->rotation.y_rotation) * (1.0f / 65536.0f);
    const f32 yc = coss_s16(trans->rotation.y_rotation) * (1.0f / 65536.0f);
    const f32 xs = sins_s16(trans->rotation.x_rotation) * (1.0f / 65536.0f);
    const f32 xc = coss_s16(trans->rotation.x_rotation) * (1.0f / 65536.0f);
    const f32 zs = sins_s16(trans->rotation.z_rotation) * (1.0f / 65536.0f);
    const f32 zc = coss_s16(trans->rotation.z_rotation) * (1.0f / 65536.0f);
    const f32 scale = trans->scale;

    memset(mtx, 0, sizeof(*mtx));
    (*mtx)[0][0] = (xs * ys * zs + zc * yc) * scale;
    (*mtx)[0][1] = (zs * xc) * scale;
    (*mtx)[0][2] = (xs * yc * zs - zc * ys) * scale;
    (*mtx)[1][0] = (xs * ys * zc - zs * yc) * scale;
    (*mtx)[1][1] = (zc * xc) * scale;
    (*mtx)[1][2] = (xs * yc * zc + zs * ys) * scale;
    (*mtx)[2][0] = (xc * ys) * scale;
    (*mtx)[2][1] = -xs * scale;
    (*mtx)[2][2] = (xc * yc) * scale;
    (*mtx)[3][0] = trans->x_position;
    (*mtx)[3][1] = trans->y_position;
    (*mtx)[3][2] = trans->z_position;
    (*mtx)[3][3] = 1.0f;
}

static uint32_t next_random(void) {
    sRandomState = sRandomState * UINT32_C(1664525) + UINT32_C(1013904223);
    return sRandomState;
}

static float random_float(float minimum, float maximum) {
    const double unit = (double)next_random() / UINT32_MAX;
    return (float)((double)minimum + ((double)maximum - minimum) * unit);
}

static ObjectTransform interpolate_pose(
    const ObjectTransform *previous,
    const ObjectTransform *current,
    double alpha) {
    ObjectTransform pose = *previous;
    int16_t delta;

    pose.x_position = (float)((double)previous->x_position +
        ((double)current->x_position - previous->x_position) * alpha);
    pose.y_position = (float)((double)previous->y_position +
        ((double)current->y_position - previous->y_position) * alpha);
    pose.z_position = (float)((double)previous->z_position +
        ((double)current->z_position - previous->z_position) * alpha);
    pose.scale = (float)((double)previous->scale +
        ((double)current->scale - previous->scale) * alpha);
    delta = (int16_t)((uint16_t)current->rotation.x_rotation -
                      (uint16_t)previous->rotation.x_rotation);
    pose.rotation.x_rotation = (int16_t)(uint16_t)(
        (uint16_t)previous->rotation.x_rotation +
        (uint16_t)(int16_t)((double)delta * alpha));
    delta = (int16_t)((uint16_t)current->rotation.y_rotation -
                      (uint16_t)previous->rotation.y_rotation);
    pose.rotation.y_rotation = (int16_t)(uint16_t)(
        (uint16_t)previous->rotation.y_rotation +
        (uint16_t)(int16_t)((double)delta * alpha));
    delta = (int16_t)((uint16_t)current->rotation.z_rotation -
                      (uint16_t)previous->rotation.z_rotation);
    pose.rotation.z_rotation = (int16_t)(uint16_t)(
        (uint16_t)previous->rotation.z_rotation +
        (uint16_t)(int16_t)((double)delta * alpha));
    return pose;
}

static int bounds_for_pose(
    const MdkrCameraDynamicAabb *local,
    ObjectTransform pose,
    MdkrCameraDynamicAabb *out_bounds) {
    MtxF matrix;
    MdkrCameraObjectTransform transform;

    mtxf_from_transform(&matrix, &pose);
    transform.translation = (MdkrCameraVec3) {
        matrix[3][0], matrix[3][1], matrix[3][2],
    };
    transform.local_x_axis = (MdkrCameraVec3) {
        matrix[0][0], matrix[0][1], matrix[0][2],
    };
    transform.local_y_axis = (MdkrCameraVec3) {
        matrix[1][0], matrix[1][1], matrix[1][2],
    };
    transform.local_z_axis = (MdkrCameraVec3) {
        matrix[2][0], matrix[2][1], matrix[2][2],
    };
    return mdkr_camera_dynamic_world_aabb(local, &transform, out_bounds);
}

static int contains_bounds(
    const MdkrCameraDynamicAabb *envelope,
    const MdkrCameraDynamicAabb *sample) {
    const float tolerance = 0.001f;

    return sample->minimum.x >= envelope->minimum.x - tolerance &&
        sample->minimum.y >= envelope->minimum.y - tolerance &&
        sample->minimum.z >= envelope->minimum.z - tolerance &&
        sample->maximum.x <= envelope->maximum.x + tolerance &&
        sample->maximum.y <= envelope->maximum.y + tolerance &&
        sample->maximum.z <= envelope->maximum.z + tolerance;
}

static void check_dense_case(
    const char *name,
    const MdkrCameraDynamicAabb *local,
    const ObjectTransform *previous,
    const ObjectTransform *current) {
    MdkrCameraDynamicAabb envelope;
    int sample;

    if (!mdkr_camera_dynamic_temporal_bounds(
            local, previous, current, &envelope)) {
        fprintf(stderr, "FAIL %s envelope rejected\n", name);
        sFailures++;
        return;
    }
    for (sample = 0; sample <= DENSE_SAMPLES; sample++) {
        const double alpha = (double)sample / DENSE_SAMPLES;
        const ObjectTransform pose = interpolate_pose(previous, current, alpha);
        MdkrCameraDynamicAabb sampled_bounds;

        if (!bounds_for_pose(local, pose, &sampled_bounds) ||
            !contains_bounds(&envelope, &sampled_bounds)) {
            fprintf(stderr, "FAIL %s dense sample %d/%d escaped\n",
                    name, sample, DENSE_SAMPLES);
            sFailures++;
            return;
        }
    }
}

static ObjectTransform make_pose(
    float x, float y, float z, float scale,
    int16_t rx, int16_t ry, int16_t rz) {
    ObjectTransform pose;

    memset(&pose, 0, sizeof(pose));
    pose.x_position = x;
    pose.y_position = y;
    pose.z_position = z;
    pose.scale = scale;
    pose.rotation.x_rotation = rx;
    pose.rotation.y_rotation = ry;
    pose.rotation.z_rotation = rz;
    return pose;
}

int main(void) {
    MdkrCameraDynamicAabb local = {
        { -47.0f, -19.0f, -83.0f },
        { 61.0f, 37.0f, 29.0f },
    };
    ObjectTransform previous;
    ObjectTransform current;
    MdkrCameraDynamicAabb invalid_output;
    int case_index;

    previous = make_pose(0.0f, 0.0f, 0.0f, 1.0f, 0, 0, 0);
    current = previous;
    check_dense_case("endpoint equality", &local, &previous, &current);

    current = make_pose(1200.0f, -450.0f, 880.0f, 2.75f,
                        0x3800, -0x4100, 0x2700);
    check_dense_case("translation rotation scale", &local, &previous, &current);

    previous = make_pose(10000000.0f, -8000000.0f, 6000000.0f, 0.25f,
                         0x7FF0, -0x7FF0, 0x7FE0);
    current = make_pose(10000400.0f, -7999700.0f, 5999800.0f, 4.0f,
                        -0x7FF0, 0x7FF0, -0x7FE0);
    check_dense_case("high-coordinate shortest wrap", &local, &previous, &current);

    for (case_index = 0; case_index < 128; case_index++) {
        char name[64];

        local.minimum = (MdkrCameraVec3) {
            -random_float(0.1f, 250.0f),
            -random_float(0.1f, 250.0f),
            -random_float(0.1f, 250.0f),
        };
        local.maximum = (MdkrCameraVec3) {
            random_float(0.1f, 250.0f),
            random_float(0.1f, 250.0f),
            random_float(0.1f, 250.0f),
        };
        previous = make_pose(
            random_float(-1000000.0f, 1000000.0f),
            random_float(-1000000.0f, 1000000.0f),
            random_float(-1000000.0f, 1000000.0f),
            random_float(0.05f, 8.0f),
            (int16_t)next_random(), (int16_t)next_random(),
            (int16_t)next_random());
        current = make_pose(
            previous.x_position + random_float(-5000.0f, 5000.0f),
            previous.y_position + random_float(-5000.0f, 5000.0f),
            previous.z_position + random_float(-5000.0f, 5000.0f),
            random_float(0.05f, 8.0f),
            (int16_t)next_random(), (int16_t)next_random(),
            (int16_t)next_random());
        snprintf(name, sizeof(name), "seeded case %d", case_index);
        check_dense_case(name, &local, &previous, &current);
    }

    local = (MdkrCameraDynamicAabb) {
        { -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f },
    };
    previous = make_pose(0.0f, 0.0f, 0.0f, 1.0f, 0, 0, 0);
    current = previous;
    current.x_position = NAN;
    expect_true("nonfinite pose rejected",
                !mdkr_camera_dynamic_temporal_bounds(
                    &local, &previous, &current, &invalid_output));
    current = previous;
    current.scale = 0.0f;
    expect_true("zero scale rejected",
                !mdkr_camera_dynamic_temporal_bounds(
                    &local, &previous, &current, &invalid_output));
    local.minimum.x = 2.0f;
    expect_true("reversed local bounds rejected",
                !mdkr_camera_dynamic_temporal_bounds(
                    &local, &previous, &previous, &invalid_output));

    local = (MdkrCameraDynamicAabb) {
        { -1.0f, -4.0f, -4.0f }, { 1.0f, 4.0f, 4.0f },
    };
    expect_true("non-door chord crosses current solid",
                mdkr_camera_dynamic_swept_aabb_intersects(
                    &local, (MdkrCameraVec3) { -10.0f, 0.0f, 0.0f },
                    (MdkrCameraVec3) { 10.0f, 0.0f, 0.0f }, 0.5));
    expect_true("non-door chord guard catches edge",
                mdkr_camera_dynamic_swept_aabb_intersects(
                    &local, (MdkrCameraVec3) { -10.0f, 4.4f, 0.0f },
                    (MdkrCameraVec3) { 10.0f, 4.4f, 0.0f }, 0.5));
    expect_true("non-door parallel chord remains clear",
                !mdkr_camera_dynamic_swept_aabb_intersects(
                    &local, (MdkrCameraVec3) { -10.0f, 5.0f, 0.0f },
                    (MdkrCameraVec3) { 10.0f, 5.0f, 0.0f }, 0.5));
    expect_true("non-door initial overlap cuts",
                mdkr_camera_dynamic_swept_aabb_intersects(
                    &local, (MdkrCameraVec3) { 0.0f, 0.0f, 0.0f },
                    (MdkrCameraVec3) { 10.0f, 0.0f, 0.0f }, 0.5));
    expect_true("non-door invalid radius does not claim geometry",
                !mdkr_camera_dynamic_swept_aabb_intersects(
                    &local, (MdkrCameraVec3) { -10.0f, 0.0f, 0.0f },
                    (MdkrCameraVec3) { 10.0f, 0.0f, 0.0f }, NAN));

    if (sFailures != 0) {
        fprintf(stderr, "camera-dynamic-temporal: %d failure(s)\n", sFailures);
        return 1;
    }
    printf("camera-dynamic-temporal: dense renderer envelope oracle passed\n");
    return 0;
}
