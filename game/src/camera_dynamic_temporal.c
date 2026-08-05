#include "camera_dynamic_temporal.h"

#ifdef NATIVE_PORT

#include <float.h>
#include <math.h>
#include <stdint.h>

#include "camera_obstruction_transform.h"
#include "math_util.h"

enum { MDKR_CAMERA_DYNAMIC_TEMPORAL_SAMPLES = 16 };

static int mdkr_camera_dynamic_temporal_finite_vec3(MdkrCameraVec3 value) {
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static int mdkr_camera_dynamic_local_bounds_valid(
    const MdkrCameraDynamicAabb *bounds) {
    return bounds != NULL &&
        mdkr_camera_dynamic_temporal_finite_vec3(bounds->minimum) &&
        mdkr_camera_dynamic_temporal_finite_vec3(bounds->maximum) &&
        bounds->minimum.x <= bounds->maximum.x &&
        bounds->minimum.y <= bounds->maximum.y &&
        bounds->minimum.z <= bounds->maximum.z;
}

int mdkr_camera_dynamic_world_aabb(
    const MdkrCameraDynamicAabb *local_bounds,
    const MdkrCameraObjectTransform *transform,
    MdkrCameraDynamicAabb *out_bounds) {
    MdkrCameraDynamicAabb bounds;
    int corner;

    if (!mdkr_camera_dynamic_local_bounds_valid(local_bounds) ||
        transform == NULL || out_bounds == NULL) {
        return 0;
    }
    for (corner = 0; corner < 8; corner++) {
        const MdkrCameraVec3 local = {
            (corner & 1) ? local_bounds->maximum.x : local_bounds->minimum.x,
            (corner & 2) ? local_bounds->maximum.y : local_bounds->minimum.y,
            (corner & 4) ? local_bounds->maximum.z : local_bounds->minimum.z,
        };
        MdkrCameraVec3 world;

        if (!mdkr_camera_object_transform_point_to_world(transform, local, &world)) {
            return 0;
        }
        if (corner == 0) {
            bounds.minimum = world;
            bounds.maximum = world;
        } else {
            if (world.x < bounds.minimum.x) bounds.minimum.x = world.x;
            if (world.y < bounds.minimum.y) bounds.minimum.y = world.y;
            if (world.z < bounds.minimum.z) bounds.minimum.z = world.z;
            if (world.x > bounds.maximum.x) bounds.maximum.x = world.x;
            if (world.y > bounds.maximum.y) bounds.maximum.y = world.y;
            if (world.z > bounds.maximum.z) bounds.maximum.z = world.z;
        }
    }
    *out_bounds = bounds;
    return 1;
}

int mdkr_camera_dynamic_swept_aabb_intersects(
    const MdkrCameraDynamicAabb *bounds,
    MdkrCameraVec3 start_eye,
    MdkrCameraVec3 desired_eye,
    double radius) {
    const double start[3] = { start_eye.x, start_eye.y, start_eye.z };
    const double end[3] = { desired_eye.x, desired_eye.y, desired_eye.z };
    double minimum[3];
    double maximum[3];
    double first = 0.0;
    double last = 1.0;
    int axis;

    if (!mdkr_camera_dynamic_local_bounds_valid(bounds) ||
        !mdkr_camera_dynamic_temporal_finite_vec3(start_eye) ||
        !mdkr_camera_dynamic_temporal_finite_vec3(desired_eye) ||
        !isfinite(radius) || radius < 0.0) {
        return 0;
    }
    minimum[0] = (double)bounds->minimum.x - radius;
    minimum[1] = (double)bounds->minimum.y - radius;
    minimum[2] = (double)bounds->minimum.z - radius;
    maximum[0] = (double)bounds->maximum.x + radius;
    maximum[1] = (double)bounds->maximum.y + radius;
    maximum[2] = (double)bounds->maximum.z + radius;
    for (axis = 0; axis < 3; axis++) {
        const double delta = end[axis] - start[axis];

        if (delta == 0.0) {
            if (start[axis] < minimum[axis] || start[axis] > maximum[axis]) {
                return 0;
            }
        } else {
            double axis_first = (minimum[axis] - start[axis]) / delta;
            double axis_last = (maximum[axis] - start[axis]) / delta;

            if (axis_first > axis_last) {
                const double temporary = axis_first;
                axis_first = axis_last;
                axis_last = temporary;
            }
            if (axis_first > first) first = axis_first;
            if (axis_last < last) last = axis_last;
            if (first > last) return 0;
        }
    }
    return last >= 0.0 && first <= 1.0;
}

static int mdkr_camera_dynamic_transform_from_pose(
    const ObjectTransform *pose,
    MdkrCameraObjectTransform *out_transform) {
    MtxF matrix;
    float validated_scale;

    if (pose == NULL || out_transform == NULL) {
        return 0;
    }
    mtxf_from_transform(&matrix, (ObjectTransform *)pose);
    out_transform->translation = (MdkrCameraVec3) {
        matrix[3][0], matrix[3][1], matrix[3][2],
    };
    out_transform->local_x_axis = (MdkrCameraVec3) {
        matrix[0][0], matrix[0][1], matrix[0][2],
    };
    out_transform->local_y_axis = (MdkrCameraVec3) {
        matrix[1][0], matrix[1][1], matrix[1][2],
    };
    out_transform->local_z_axis = (MdkrCameraVec3) {
        matrix[2][0], matrix[2][1], matrix[2][2],
    };
    return mdkr_camera_object_transform_validate(
        out_transform, &validated_scale);
}

static ObjectTransform mdkr_camera_dynamic_interpolate_pose(
    const ObjectTransform *previous,
    const ObjectTransform *current,
    double alpha) {
    ObjectTransform interpolated = *previous;
    int16_t delta;

    interpolated.x_position = (float)(
        (double)previous->x_position +
        ((double)current->x_position - previous->x_position) * alpha);
    interpolated.y_position = (float)(
        (double)previous->y_position +
        ((double)current->y_position - previous->y_position) * alpha);
    interpolated.z_position = (float)(
        (double)previous->z_position +
        ((double)current->z_position - previous->z_position) * alpha);
    interpolated.scale = (float)(
        (double)previous->scale +
        ((double)current->scale - previous->scale) * alpha);
    delta = (int16_t)((uint16_t)current->rotation.x_rotation -
                      (uint16_t)previous->rotation.x_rotation);
    interpolated.rotation.x_rotation = (int16_t)(uint16_t)(
        (uint16_t)previous->rotation.x_rotation +
        (uint16_t)(int16_t)((double)delta * alpha));
    delta = (int16_t)((uint16_t)current->rotation.y_rotation -
                      (uint16_t)previous->rotation.y_rotation);
    interpolated.rotation.y_rotation = (int16_t)(uint16_t)(
        (uint16_t)previous->rotation.y_rotation +
        (uint16_t)(int16_t)((double)delta * alpha));
    delta = (int16_t)((uint16_t)current->rotation.z_rotation -
                      (uint16_t)previous->rotation.z_rotation);
    interpolated.rotation.z_rotation = (int16_t)(uint16_t)(
        (uint16_t)previous->rotation.z_rotation +
        (uint16_t)(int16_t)((double)delta * alpha));
    return interpolated;
}

int mdkr_camera_dynamic_temporal_bounds(
    const MdkrCameraDynamicAabb *local_bounds,
    const ObjectTransform *previous,
    const ObjectTransform *current,
    MdkrCameraDynamicAabb *out_bounds) {
    const double angle_to_radians = 6.28318530717958647692 / 65536.0;
    double local_radius_squared = 0.0;
    double local_radius;
    double translation_distance;
    double angular_distance;
    double maximum_scale;
    double scale_distance;
    double padding;
    int sample;
    int corner;

    if (!mdkr_camera_dynamic_local_bounds_valid(local_bounds) ||
        previous == NULL || current == NULL || out_bounds == NULL) {
        return 0;
    }
    for (corner = 0; corner < 8; corner++) {
        const double x = (corner & 1) ?
            local_bounds->maximum.x : local_bounds->minimum.x;
        const double y = (corner & 2) ?
            local_bounds->maximum.y : local_bounds->minimum.y;
        const double z = (corner & 4) ?
            local_bounds->maximum.z : local_bounds->minimum.z;
        const double length_squared = x * x + y * y + z * z;

        if (!isfinite(length_squared)) return 0;
        if (length_squared > local_radius_squared) {
            local_radius_squared = length_squared;
        }
    }
    local_radius = sqrt(local_radius_squared);
    maximum_scale = fmax(fabs((double)previous->scale),
                         fabs((double)current->scale));
    scale_distance = fabs((double)current->scale - previous->scale);
    translation_distance = sqrt(
        ((double)current->x_position - previous->x_position) *
            ((double)current->x_position - previous->x_position) +
        ((double)current->y_position - previous->y_position) *
            ((double)current->y_position - previous->y_position) +
        ((double)current->z_position - previous->z_position) *
            ((double)current->z_position - previous->z_position));
    angular_distance = angle_to_radians * (
        fabs((double)(int16_t)((uint16_t)current->rotation.x_rotation -
                               (uint16_t)previous->rotation.x_rotation)) +
        fabs((double)(int16_t)((uint16_t)current->rotation.y_rotation -
                               (uint16_t)previous->rotation.y_rotation)) +
        fabs((double)(int16_t)((uint16_t)current->rotation.z_rotation -
                               (uint16_t)previous->rotation.z_rotation)));
    if (!isfinite(local_radius) || !isfinite(maximum_scale) ||
        !isfinite(scale_distance) || !isfinite(translation_distance) ||
        !isfinite(angular_distance) || maximum_scale <= 0.0) {
        return 0;
    }
    /* Each exact renderer sample is inflated by a Lipschitz bound for the
     * unsampled subinterval. The small table term covers fixed-angle sine/cos
     * quantization as well as outward float rounding. */
    padding = (translation_distance + local_radius *
        (scale_distance + maximum_scale * angular_distance)) /
        MDKR_CAMERA_DYNAMIC_TEMPORAL_SAMPLES +
        local_radius * maximum_scale * 0.001 + 0.01;
    if (!isfinite(padding) || padding > FLT_MAX) return 0;

    for (sample = 0; sample <= MDKR_CAMERA_DYNAMIC_TEMPORAL_SAMPLES; sample++) {
        const double alpha =
            (double)sample / MDKR_CAMERA_DYNAMIC_TEMPORAL_SAMPLES;
        const ObjectTransform interpolated = mdkr_camera_dynamic_interpolate_pose(
            previous, current, alpha);
        MdkrCameraObjectTransform sampled_transform;
        MdkrCameraDynamicAabb sampled_bounds;

        if (!mdkr_camera_dynamic_transform_from_pose(
                &interpolated, &sampled_transform) ||
            !mdkr_camera_dynamic_world_aabb(
                local_bounds, &sampled_transform, &sampled_bounds)) {
            return 0;
        }
        if (sample == 0) {
            *out_bounds = sampled_bounds;
        } else {
            if (sampled_bounds.minimum.x < out_bounds->minimum.x)
                out_bounds->minimum.x = sampled_bounds.minimum.x;
            if (sampled_bounds.minimum.y < out_bounds->minimum.y)
                out_bounds->minimum.y = sampled_bounds.minimum.y;
            if (sampled_bounds.minimum.z < out_bounds->minimum.z)
                out_bounds->minimum.z = sampled_bounds.minimum.z;
            if (sampled_bounds.maximum.x > out_bounds->maximum.x)
                out_bounds->maximum.x = sampled_bounds.maximum.x;
            if (sampled_bounds.maximum.y > out_bounds->maximum.y)
                out_bounds->maximum.y = sampled_bounds.maximum.y;
            if (sampled_bounds.maximum.z > out_bounds->maximum.z)
                out_bounds->maximum.z = sampled_bounds.maximum.z;
        }
    }
    out_bounds->minimum.x = (float)((double)out_bounds->minimum.x - padding);
    out_bounds->minimum.y = (float)((double)out_bounds->minimum.y - padding);
    out_bounds->minimum.z = (float)((double)out_bounds->minimum.z - padding);
    out_bounds->maximum.x = (float)((double)out_bounds->maximum.x + padding);
    out_bounds->maximum.y = (float)((double)out_bounds->maximum.y + padding);
    out_bounds->maximum.z = (float)((double)out_bounds->maximum.z + padding);
    return mdkr_camera_dynamic_temporal_finite_vec3(out_bounds->minimum) &&
        mdkr_camera_dynamic_temporal_finite_vec3(out_bounds->maximum);
}

#endif /* NATIVE_PORT */
