#include "camera.h"

#ifdef NATIVE_PORT

#include "math_util.h"
#include "presentation_snapshot.h"

#include <math.h>
#include <float.h>
#include <string.h>

static int camera_lens_pose_finite(Vec3f value) {
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static float camera_lens_pose_length(Vec3f value) {
    const double squared = (double)value.x * value.x +
                           (double)value.y * value.y +
                           (double)value.z * value.z;

    if (!isfinite(squared) || squared <= 0.0) {
        return 0.0f;
    }
    return (float)sqrt(squared);
}

static int camera_lens_pose_normalize(Vec3f *value) {
    const float length = value != NULL ? camera_lens_pose_length(*value) : 0.0f;

    if (!isfinite(length) || length <= 0.0f) {
        return FALSE;
    }
    value->x /= length;
    value->y /= length;
    value->z /= length;
    return camera_lens_pose_finite(*value);
}

static Vec3f camera_lens_pose_vec3(float x, float y, float z) {
    Vec3f value;

    value.x = x;
    value.y = y;
    value.z = z;
    return value;
}

static Vec3f camera_lens_pose_cross(Vec3f left, Vec3f right) {
    return camera_lens_pose_vec3(
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x);
}

int cam_lens_pose_from_camera_snapshot(
    const Camera *camera, s32 apply_shake, MdkrCameraLensPose *out) {
    ObjectTransform world_from_camera;
    MtxF matrix;
    MdkrCameraLensPose pose;

    if (camera == NULL || out == NULL ||
        !isfinite(camera->trans.x_position) ||
        !isfinite(camera->trans.y_position) ||
        !isfinite(camera->trans.z_position) ||
        (apply_shake && !isfinite(camera->shakeMagnitude))) {
        return FALSE;
    }

    memset(&world_from_camera, 0, sizeof(world_from_camera));
    world_from_camera.scale = 1.0f;
    /* This is the inverse-view half of cam_build_view_basis(), rather than a
     * hand-authored yaw convention. The local -Z axis is visual forward. */
    world_from_camera.rotation.y_rotation =
        (s16)(-0x8000 - camera->trans.rotation.y_rotation);
    world_from_camera.rotation.x_rotation =
        (s16)(-(camera->trans.rotation.x_rotation + camera->pitch));
    world_from_camera.rotation.z_rotation = (s16)-camera->trans.rotation.z_rotation;
    world_from_camera.x_position = camera->trans.x_position;
    world_from_camera.y_position = camera->trans.y_position;
    if (apply_shake) {
        world_from_camera.y_position += camera->shakeMagnitude;
    }
    world_from_camera.z_position = camera->trans.z_position;

    mtxf_from_transform(&matrix, &world_from_camera);
    pose.eye = camera_lens_pose_vec3(
        world_from_camera.x_position,
        world_from_camera.y_position,
        world_from_camera.z_position);
    /* mtxf_from_transform() stores world images of local axes in rows. */
    pose.right = camera_lens_pose_vec3(matrix[0][0], matrix[0][1], matrix[0][2]);
    pose.forward = camera_lens_pose_vec3(-matrix[2][0], -matrix[2][1], -matrix[2][2]);

    /* The native sine table is fixed-point, so normalize the intended
     * orthonormal axes after taking their directions from renderer math.
     * Rebuild up from right/forward to preserve R x U == -F exactly enough for
     * a geometric lens guard while retaining roll. */
    if (!camera_lens_pose_finite(pose.eye) ||
        !camera_lens_pose_normalize(&pose.right) ||
        !camera_lens_pose_normalize(&pose.forward)) {
        return FALSE;
    }
    pose.up = camera_lens_pose_cross(pose.right, pose.forward);
    if (!camera_lens_pose_normalize(&pose.up)) {
        return FALSE;
    }
    /* Remove any fixed-point orthogonality residue from right as well. */
    pose.right = camera_lens_pose_cross(pose.forward, pose.up);
    if (!camera_lens_pose_normalize(&pose.right)) {
        return FALSE;
    }

    *out = pose;
    return TRUE;
}

static s16 camera_envelope_angle(u16 start, u16 end, int delta, unsigned step, int snapped) {
    if (snapped) return (s16)(step ? end : start);
    return (s16)(u16)(start + (delta < 0 ? -(int)step : (int)step));
}

int cam_lens_rotation_envelope(const Camera *before, const Camera *after,
    float near_distance, float half_width, float half_height,
    unsigned max_states, float *out) {
    if (!before || !after || !out || !max_states ||
        !isfinite(near_distance) || near_distance <= 0 ||
        !isfinite(half_width) || half_width <= 0 ||
        !isfinite(half_height) || half_height <= 0) return FALSE;
    const u16 start[3] = {
        (u16)before->trans.rotation.y_rotation,
        (u16)((u16)before->trans.rotation.x_rotation + (u16)before->pitch),
        (u16)before->trans.rotation.z_rotation
    };
    const u16 end[3] = {
        (u16)after->trans.rotation.y_rotation,
        (u16)((u16)after->trans.rotation.x_rotation + (u16)after->pitch),
        (u16)after->trans.rotation.z_rotation
    };
    int delta[3];
    int snapped[3];
    unsigned counts[3], states = 1;
    for (unsigned axis = 0; axis < 3; ++axis) {
        delta[axis] = (s16)(u16)(end[axis] - start[axis]);
        unsigned magnitude = (unsigned)(delta[axis] < 0 ? -delta[axis] : delta[axis]);
        /* Match presentation_lerp_angle: strictly past a quarter turn snaps.
         * Include both endpoints for alpha=0/denominator=0 and positive alpha.
         * The Cartesian product remains conservative across unsnapped axes. */
        snapped[axis] = magnitude > PRESENTATION_SNAPSHOT_ROTATION_SNAP;
        counts[axis] = snapped[axis] ? 2 : magnitude + 1;
        if (counts[axis] > max_states / states) return FALSE;
        states *= counts[axis];
    }
    Camera sample = {0};
    sample.trans.rotation.y_rotation = (s16)start[0];
    sample.trans.rotation.x_rotation = (s16)start[1];
    sample.trans.rotation.z_rotation = (s16)start[2];
    MdkrCameraLensPose reference, pose;
    if (!cam_lens_pose_from_camera_snapshot(&sample, FALSE, &reference)) return FALSE;
    double largest_squared = 0;
    for (unsigned y = 0; y < counts[0]; ++y)
    for (unsigned p = 0; p < counts[1]; ++p)
    for (unsigned r = 0; r < counts[2]; ++r) {
        sample.trans.rotation.y_rotation = camera_envelope_angle(start[0], end[0], delta[0], y, snapped[0]);
        sample.trans.rotation.x_rotation = camera_envelope_angle(start[1], end[1], delta[1], p, snapped[1]);
        sample.trans.rotation.z_rotation = camera_envelope_angle(start[2], end[2], delta[2], r, snapped[2]);
        if (!cam_lens_pose_from_camera_snapshot(&sample, FALSE, &pose)) return FALSE;
        for (int horizontal = -1; horizontal <= 1; horizontal += 2)
        for (int vertical = -1; vertical <= 1; vertical += 2) {
#define CORNER_DELTA(component) \
            (((double)pose.forward.component - reference.forward.component) * near_distance + \
             ((double)pose.right.component - reference.right.component) * half_width * horizontal + \
             ((double)pose.up.component - reference.up.component) * half_height * vertical)
            double dx = CORNER_DELTA(x), dy = CORNER_DELTA(y), dz = CORNER_DELTA(z);
#undef CORNER_DELTA
            double squared = dx * dx + dy * dy + dz * dz;
            if (!isfinite(squared)) return FALSE;
            if (squared > largest_squared) largest_squared = squared;
        }
    }
    /* Basis components are the actual normalized float outputs above. Reserve
     * extra float arithmetic slack for downstream corner construction; always
     * round the final bound outward, not toward a potentially smaller float. */
    double bound = sqrt(largest_squared) + 32.0 * FLT_EPSILON *
        ((double)near_distance + half_width + half_height);
    if (!isfinite(bound) || bound >= FLT_MAX) return FALSE;
    float result = (float)bound;
    if ((double)result < bound) result = nextafterf(result, INFINITY);
    if (!isfinite(result)) return FALSE;
    *out = result;
    return TRUE;
}

#endif /* NATIVE_PORT */
