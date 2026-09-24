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

#ifndef MDKR_CAMERA_PRODUCTION_MATH
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
#endif


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

static void test_shake_eye_chord(void) {
    Camera before = {0}, after = {0}, sample;
    MdkrCameraLensPose first, last, actual;
    before.trans.x_position = -16.0f;
    before.trans.y_position = 32.0f;
    before.trans.z_position = 64.0f;
    before.shakeMagnitude = -8.0f;
    after.trans.x_position = 48.0f;
    after.trans.y_position = 16.0f;
    after.trans.z_position = -32.0f;
    after.shakeMagnitude = 12.0f;
    for (int enabled = 0; enabled <= 1; enabled++) {
        expect_true("chord start builds",
                    cam_lens_pose_from_camera_snapshot(&before, enabled, &first));
        expect_true("chord end builds",
                    cam_lens_pose_from_camera_snapshot(&after, enabled, &last));
        for (int step = 0; step <= 8; step++) {
            const float t = step / 8.0f;
            sample = before;
            sample.trans.x_position += t * (after.trans.x_position - before.trans.x_position);
            sample.trans.y_position += t * (after.trans.y_position - before.trans.y_position);
            sample.trans.z_position += t * (after.trans.z_position - before.trans.z_position);
            sample.shakeMagnitude += t * (after.shakeMagnitude - before.shakeMagnitude);
            // Rotation changes the lens basis, not the world-space eye chord.
            sample.trans.rotation.y_rotation = (s16)(step * 1024);
            expect_true("chord sample builds",
                        cam_lens_pose_from_camera_snapshot(&sample, enabled, &actual));
            expect_vec("shake eye follows swept chord", actual.eye,
                       vec3(first.eye.x + t * (last.eye.x - first.eye.x),
                            first.eye.y + t * (last.eye.y - first.eye.y),
                            first.eye.z + t * (last.eye.z - first.eye.z)));
        }
    }
}

/* A transition guard must consume the composed view-pitch arc used by
 * presentation, not independently interpolate the two authored components. */
static void test_composed_pitch_equivalence(void) {
    static const u16 angles[] = {0, 1, 0x1000, 0x7fff, 0x8000, 0xffff};
    for (unsigned x = 0; x < sizeof(angles) / sizeof(angles[0]); ++x) {
        for (unsigned p = 0; p < sizeof(angles) / sizeof(angles[0]); ++p) {
            Camera split = {0}, composed;
            MdkrCameraLensPose a, b;
            split.trans.rotation.x_rotation = (s16)angles[x];
            split.pitch = (s16)angles[p];
            split.trans.rotation.y_rotation = (s16)0x9123;
            split.trans.rotation.z_rotation = 0x1234;
            composed = split;
            composed.trans.rotation.x_rotation =
                (s16)(u16)(angles[x] + angles[p]);
            composed.pitch = 0;
            expect_true("split pitch builds",
                        cam_lens_pose_from_camera_snapshot(&split, 0, &a));
            expect_true("composed pitch builds",
                        cam_lens_pose_from_camera_snapshot(&composed, 0, &b));
            expect_vec("composed pitch forward", a.forward, b.forward);
            expect_vec("composed pitch right", a.right, b.right);
            expect_vec("composed pitch up", a.up, b.up);
        }
    }
}

static void test_rotation_envelope(void) {
    Camera before = {0}, after, sample;
    before.trans.rotation.y_rotation = (s16)0xfffd;
    before.trans.rotation.x_rotation = 0x7fff;
    before.pitch = 0x7fff;
    before.trans.rotation.z_rotation = 2;
    after = before;
    after.trans.rotation.y_rotation = 4;
    after.trans.rotation.x_rotation = (s16)0x8002;
    after.pitch = (s16)0x8002;
    after.trans.rotation.z_rotation = -2;
    float bound = -1;
    expect_true("rotation product budget rejects without writes",
        !cam_lens_rotation_envelope(&before, &after, 10, 8, 6, 279, &bound) && bound == -1);
    expect_true("rotation product exact budget accepted",
        cam_lens_rotation_envelope(&before, &after, 10, 8, 6, 280, &bound));
    expect_true("small rotation bound is tighter than enclosing sphere",
                bound > 0 && bound < 0.1f);
    MdkrCameraLensPose base, current;
    expect_true("envelope reference", cam_lens_pose_from_camera_snapshot(&before, 0, &base));
    for (int step = 0; step <= 97; ++step) {
        sample = before;
        sample.trans.rotation.y_rotation = (s16)(u16)(0xfffd + 7 * step / 97);
        sample.trans.rotation.x_rotation = (s16)(u16)(0xfffe + 6 * step / 97);
        sample.pitch = 0;
        sample.trans.rotation.z_rotation = (s16)(2 - 4 * step / 97);
        expect_true("envelope sampled renderer basis",
                    cam_lens_pose_from_camera_snapshot(&sample, 0, &current));
        for (int h = -1; h <= 1; h += 2)
        for (int v = -1; v <= 1; v += 2) {
            const double a[3] = {
                10.0*base.forward.x + h*8.0*base.right.x + v*6.0*base.up.x,
                10.0*base.forward.y + h*8.0*base.right.y + v*6.0*base.up.y,
                10.0*base.forward.z + h*8.0*base.right.z + v*6.0*base.up.z};
            const double b[3] = {
                10.0*current.forward.x + h*8.0*current.right.x + v*6.0*current.up.x,
                10.0*current.forward.y + h*8.0*current.right.y + v*6.0*current.up.y,
                10.0*current.forward.z + h*8.0*current.right.z + v*6.0*current.up.z};
            double distance = sqrt((a[0]-b[0])*(a[0]-b[0]) +
                                   (a[1]-b[1])*(a[1]-b[1]) +
                                   (a[2]-b[2])*(a[2]-b[2]));
            expect_true("sampled corner within envelope", distance <= bound);
        }
    }
    bound = -1;
    expect_true("invalid dimensions preserve output",
        !cam_lens_rotation_envelope(&before, &after, NAN, 8, 6, 280, &bound) && bound == -1);
    expect_true("null output rejected",
        !cam_lens_rotation_envelope(&before, &after, 10, 8, 6, 280, NULL));
    after.trans.rotation.y_rotation = (s16)(u16)(0xfffd + 0x8000);
    expect_true("half-turn oversized domain rejected",
        !cam_lens_rotation_envelope(&before, &after, 10, 8, 6, 4096, &bound) && bound == -1);
}

int main(void) {
    test_zero_golden_and_shake();
    test_renderer_recipe_and_handedness();
    test_invalid_input();
    test_shake_eye_chord();
    test_composed_pitch_equivalence();
    test_rotation_envelope();
    if (sFailures != 0) {
        fprintf(stderr, "test_camera_lens_pose: %d failure(s)\n", sFailures);
        return 1;
    }
    puts("test_camera_lens_pose: PASS");
    return 0;
}
