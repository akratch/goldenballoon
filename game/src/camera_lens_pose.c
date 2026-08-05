#include "camera.h"

#ifdef NATIVE_PORT

#include "math_util.h"

#include <math.h>
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
    pose.up = camera_lens_pose_vec3(matrix[1][0], matrix[1][1], matrix[1][2]);
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

#endif /* NATIVE_PORT */
