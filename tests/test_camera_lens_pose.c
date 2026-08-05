#include "camera.h"
#include "math_util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int sFailures;

#define EPSILON 0.0002f
#define DKR_ANGLE_TO_RAD (6.28318530717958647692f / 65536.0f)

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        sFailures++;
    }
}

static void expect_near(const char *name, float actual, float expected) {
    if (!isfinite(actual) || fabsf(actual - expected) > EPSILON) {
        fprintf(stderr, "FAIL %s actual=%f expected=%f\n", name, actual, expected);
        sFailures++;
    }
}

static void expect_vec(const char *name, Vec3f actual, Vec3f expected) {
    char label[64];

    snprintf(label, sizeof(label), "%s.x", name);
    expect_near(label, actual.x, expected.x);
    snprintf(label, sizeof(label), "%s.y", name);
    expect_near(label, actual.y, expected.y);
    snprintf(label, sizeof(label), "%s.z", name);
    expect_near(label, actual.z, expected.z);
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

/* Test-local renderer oracle: this is mtxf_from_transform's native C recipe.
 * The production helper must obtain its directions from this same matrix path. */
void mtxf_from_transform(MtxF *mtx, ObjectTransform *trans) {
    const f32 ys = sins_s16(trans->rotation.y_rotation) * (1.0f / 65536.0f);
    const f32 yc = coss_s16(trans->rotation.y_rotation) * (1.0f / 65536.0f);
    const f32 xs = sins_s16(trans->rotation.x_rotation) * (1.0f / 65536.0f);
    const f32 xc = coss_s16(trans->rotation.x_rotation) * (1.0f / 65536.0f);
    const f32 zs = sins_s16(trans->rotation.z_rotation) * (1.0f / 65536.0f);
    const f32 zc = coss_s16(trans->rotation.z_rotation) * (1.0f / 65536.0f);

    (*mtx)[0][0] = xs * ys * zs + zc * yc;
    (*mtx)[0][1] = zs * xc;
    (*mtx)[0][2] = xs * yc * zs - zc * ys;
    (*mtx)[1][0] = xs * ys * zc - zs * yc;
    (*mtx)[1][1] = zc * xc;
    (*mtx)[1][2] = xs * yc * zc + zs * ys;
    (*mtx)[2][0] = xc * ys;
    (*mtx)[2][1] = -xs;
    (*mtx)[2][2] = xc * yc;
    (*mtx)[3][0] = trans->x_position;
    (*mtx)[3][1] = trans->y_position;
    (*mtx)[3][2] = trans->z_position;
    (*mtx)[3][3] = 1.0f;
}

static Vec3f vec3(float x, float y, float z) {
    Vec3f value;

    value.x = x;
    value.y = y;
    value.z = z;
    return value;
}

static Vec3f normalize(Vec3f value) {
    const float length = sqrtf(value.x * value.x + value.y * value.y + value.z * value.z);
    return vec3(value.x / length, value.y / length, value.z / length);
}

static Vec3f cross(Vec3f left, Vec3f right) {
    return vec3(
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x);
}

static float dot(Vec3f left, Vec3f right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

static MdkrCameraLensPose renderer_reference(const Camera *camera, int apply_shake) {
    ObjectTransform transform;
    MtxF matrix;
    MdkrCameraLensPose pose;

    memset(&transform, 0, sizeof(transform));
    transform.scale = 1.0f;
    transform.rotation.y_rotation = (s16)(-0x8000 - camera->trans.rotation.y_rotation);
    transform.rotation.x_rotation = (s16)(-(camera->trans.rotation.x_rotation + camera->pitch));
    transform.rotation.z_rotation = (s16)-camera->trans.rotation.z_rotation;
    transform.x_position = camera->trans.x_position;
    transform.y_position = camera->trans.y_position +
        (apply_shake ? camera->shakeMagnitude : 0.0f);
    transform.z_position = camera->trans.z_position;
    mtxf_from_transform(&matrix, &transform);
    pose.eye = vec3(transform.x_position, transform.y_position, transform.z_position);
    pose.right = normalize(vec3(matrix[0][0], matrix[0][1], matrix[0][2]));
    pose.forward = normalize(vec3(-matrix[2][0], -matrix[2][1], -matrix[2][2]));
    pose.up = normalize(cross(pose.right, pose.forward));
    pose.right = normalize(cross(pose.forward, pose.up));
    return pose;
}

static void test_zero_golden_and_shake(void) {
    Camera camera;
    MdkrCameraLensPose pose;

    memset(&camera, 0, sizeof(camera));
    camera.trans.x_position = 12.0f;
    camera.trans.y_position = 34.0f;
    camera.trans.z_position = 56.0f;
    camera.shakeMagnitude = 7.0f;

    expect_true("zero pose builds", cam_lens_pose_from_camera_snapshot(&camera, FALSE, &pose));
    expect_vec("zero eye", pose.eye, vec3(12.0f, 34.0f, 56.0f));
    expect_vec("zero right", pose.right, vec3(-1.0f, 0.0f, 0.0f));
    expect_vec("zero up", pose.up, vec3(0.0f, 1.0f, 0.0f));
    expect_vec("zero forward", pose.forward, vec3(0.0f, 0.0f, 1.0f));

    expect_true("shake pose builds", cam_lens_pose_from_camera_snapshot(&camera, TRUE, &pose));
    expect_vec("shake eye", pose.eye, vec3(12.0f, 41.0f, 56.0f));
}

static void test_renderer_recipe_and_handedness(void) {
    static const s16 angles[][3] = {
        { 0x1000, 0x2000, -0x0800 },
        { -0x3000, 0x5000, 0x1800 },
        { 0x5A00, -0x2200, -0x6000 },
    };
    size_t index;

    for (index = 0; index < sizeof(angles) / sizeof(angles[0]); index++) {
        Camera camera;
        MdkrCameraLensPose actual;
        MdkrCameraLensPose expected;
        char label[64];

        memset(&camera, 0, sizeof(camera));
        camera.trans.x_position = 100.0f + (float)index;
        camera.trans.y_position = -20.0f + (float)index;
        camera.trans.z_position = 40.0f - (float)index;
        camera.trans.rotation.x_rotation = angles[index][0];
        camera.trans.rotation.y_rotation = angles[index][1];
        camera.trans.rotation.z_rotation = angles[index][2];
        camera.pitch = (s16)(0x0400 * (s16)(index + 1));
        camera.shakeMagnitude = 2.5f;

        expected = renderer_reference(&camera, TRUE);
        snprintf(label, sizeof(label), "recipe %zu builds", index);
        expect_true(label, cam_lens_pose_from_camera_snapshot(&camera, TRUE, &actual));
        snprintf(label, sizeof(label), "recipe %zu eye", index);
        expect_vec(label, actual.eye, expected.eye);
        snprintf(label, sizeof(label), "recipe %zu right", index);
        expect_vec(label, actual.right, expected.right);
        snprintf(label, sizeof(label), "recipe %zu up", index);
        expect_vec(label, actual.up, expected.up);
        snprintf(label, sizeof(label), "recipe %zu forward", index);
        expect_vec(label, actual.forward, expected.forward);
        snprintf(label, sizeof(label), "recipe %zu R.U", index);
        expect_near(label, dot(actual.right, actual.up), 0.0f);
        snprintf(label, sizeof(label), "recipe %zu R.F", index);
        expect_near(label, dot(actual.right, actual.forward), 0.0f);
        snprintf(label, sizeof(label), "recipe %zu U.F", index);
        expect_near(label, dot(actual.up, actual.forward), 0.0f);
        snprintf(label, sizeof(label), "recipe %zu handedness", index);
        expect_vec(label, cross(actual.right, actual.up),
                   vec3(-actual.forward.x, -actual.forward.y, -actual.forward.z));
    }
}

static void test_invalid_input(void) {
    Camera camera;
    MdkrCameraLensPose pose;
    MdkrCameraLensPose sentinel;

    memset(&camera, 0, sizeof(camera));
    memset(&pose, 0xA5, sizeof(pose));
    sentinel = pose;
    camera.trans.x_position = INFINITY;
    expect_true("infinite position rejected",
                !cam_lens_pose_from_camera_snapshot(&camera, FALSE, &pose));
    expect_true("failure leaves output untouched", memcmp(&pose, &sentinel, sizeof(pose)) == 0);

    memset(&camera, 0, sizeof(camera));
    camera.shakeMagnitude = NAN;
    expect_true("unused invalid shake accepted",
                cam_lens_pose_from_camera_snapshot(&camera, FALSE, &pose));
    expect_true("applied invalid shake rejected",
                !cam_lens_pose_from_camera_snapshot(&camera, TRUE, &pose));
    expect_true("null camera rejected", !cam_lens_pose_from_camera_snapshot(NULL, FALSE, &pose));
    expect_true("null output rejected", !cam_lens_pose_from_camera_snapshot(&camera, FALSE, NULL));
}

int main(void) {
    test_zero_golden_and_shake();
    test_renderer_recipe_and_handedness();
    test_invalid_input();
    if (sFailures != 0) {
        fprintf(stderr, "test_camera_lens_pose: %d failure(s)\n", sFailures);
        return 1;
    }
    puts("test_camera_lens_pose: PASS");
    return 0;
}
