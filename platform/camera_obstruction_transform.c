#include "camera_obstruction_transform.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define MDKR_CAMERA_TRANSFORM_RELATIVE_TOLERANCE 1.0e-4
#define MDKR_CAMERA_ROUNDED_LENS_AXIS_TOLERANCE 1.0e-4

typedef struct MdkrCameraTransformDVec3 {
    double x;
    double y;
    double z;
} MdkrCameraTransformDVec3;

static MdkrCameraTransformDVec3 mdkr_camera_transform_dvec3(MdkrCameraVec3 value) {
    MdkrCameraTransformDVec3 result = { value.x, value.y, value.z };
    return result;
}

static int mdkr_camera_transform_finite_vec3(MdkrCameraVec3 value) {
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static MdkrCameraTransformDVec3 mdkr_camera_transform_sub(
    MdkrCameraTransformDVec3 a,
    MdkrCameraTransformDVec3 b) {
    MdkrCameraTransformDVec3 result = { a.x - b.x, a.y - b.y, a.z - b.z };
    return result;
}

static MdkrCameraTransformDVec3 mdkr_camera_transform_add(
    MdkrCameraTransformDVec3 a,
    MdkrCameraTransformDVec3 b) {
    MdkrCameraTransformDVec3 result = { a.x + b.x, a.y + b.y, a.z + b.z };
    return result;
}

static MdkrCameraTransformDVec3 mdkr_camera_transform_scale(
    MdkrCameraTransformDVec3 value,
    double scale) {
    MdkrCameraTransformDVec3 result = { value.x * scale, value.y * scale, value.z * scale };
    return result;
}

static double mdkr_camera_transform_dot(
    MdkrCameraTransformDVec3 a,
    MdkrCameraTransformDVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static MdkrCameraTransformDVec3 mdkr_camera_transform_cross(
    MdkrCameraTransformDVec3 a,
    MdkrCameraTransformDVec3 b) {
    MdkrCameraTransformDVec3 result = {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
    return result;
}

static int mdkr_camera_transform_to_float(
    MdkrCameraTransformDVec3 value,
    MdkrCameraVec3 *out_value) {
    if (!isfinite(value.x) || !isfinite(value.y) || !isfinite(value.z) ||
        fabs(value.x) > FLT_MAX || fabs(value.y) > FLT_MAX || fabs(value.z) > FLT_MAX) {
        return 0;
    }
    out_value->x = (float)value.x;
    out_value->y = (float)value.y;
    out_value->z = (float)value.z;
    return 1;
}

static int mdkr_camera_rounded_lens_guard_valid(
    const MdkrCameraRoundedLensGuard *guard) {
    MdkrCameraTransformDVec3 forward;
    MdkrCameraTransformDVec3 right;
    MdkrCameraTransformDVec3 up;
    MdkrCameraTransformDVec3 handed;
    double expected_radius;

    if (guard == NULL || !mdkr_camera_transform_finite_vec3(guard->forward) ||
        !mdkr_camera_transform_finite_vec3(guard->right) ||
        !mdkr_camera_transform_finite_vec3(guard->up) ||
        !isfinite(guard->near_distance) || !isfinite(guard->half_width) ||
        !isfinite(guard->half_height) || !isfinite(guard->skin) ||
        !isfinite(guard->broadphase_radius) || guard->near_distance <= 0.0f ||
        guard->half_width <= 0.0f || guard->half_height <= 0.0f || guard->skin < 0.0f ||
        guard->broadphase_radius < 0.0f) {
        return 0;
    }
    forward = mdkr_camera_transform_dvec3(guard->forward);
    right = mdkr_camera_transform_dvec3(guard->right);
    up = mdkr_camera_transform_dvec3(guard->up);
    handed = mdkr_camera_transform_cross(right, up);
    if (fabs(mdkr_camera_transform_dot(forward, forward) - 1.0) >
            MDKR_CAMERA_ROUNDED_LENS_AXIS_TOLERANCE ||
        fabs(mdkr_camera_transform_dot(right, right) - 1.0) >
            MDKR_CAMERA_ROUNDED_LENS_AXIS_TOLERANCE ||
        fabs(mdkr_camera_transform_dot(up, up) - 1.0) >
            MDKR_CAMERA_ROUNDED_LENS_AXIS_TOLERANCE ||
        fabs(mdkr_camera_transform_dot(forward, right)) >
            MDKR_CAMERA_ROUNDED_LENS_AXIS_TOLERANCE ||
        fabs(mdkr_camera_transform_dot(forward, up)) >
            MDKR_CAMERA_ROUNDED_LENS_AXIS_TOLERANCE ||
        fabs(mdkr_camera_transform_dot(right, up)) >
            MDKR_CAMERA_ROUNDED_LENS_AXIS_TOLERANCE ||
        mdkr_camera_transform_dot(handed, forward) >
            -1.0 + MDKR_CAMERA_ROUNDED_LENS_AXIS_TOLERANCE) {
        return 0;
    }
    expected_radius = sqrt((double)guard->near_distance * guard->near_distance +
                           (double)guard->half_width * guard->half_width +
                           (double)guard->half_height * guard->half_height) + guard->skin;
    return isfinite(expected_radius) && expected_radius <= FLT_MAX &&
           fabs(expected_radius - guard->broadphase_radius) <=
               MDKR_CAMERA_ROUNDED_LENS_AXIS_TOLERANCE * fmax(1.0, expected_radius);
}

static int mdkr_camera_transform_validate_internal(
    const MdkrCameraObjectTransform *transform,
    double *out_scale) {
    MdkrCameraTransformDVec3 x;
    MdkrCameraTransformDVec3 y;
    MdkrCameraTransformDVec3 z;
    double x_length_squared;
    double y_length_squared;
    double z_length_squared;
    double x_length;
    double y_length;
    double z_length;
    double determinant;
    double tolerance;

    if (transform == NULL || !mdkr_camera_transform_finite_vec3(transform->translation) ||
        !mdkr_camera_transform_finite_vec3(transform->local_x_axis) ||
        !mdkr_camera_transform_finite_vec3(transform->local_y_axis) ||
        !mdkr_camera_transform_finite_vec3(transform->local_z_axis)) {
        return 0;
    }
    x = mdkr_camera_transform_dvec3(transform->local_x_axis);
    y = mdkr_camera_transform_dvec3(transform->local_y_axis);
    z = mdkr_camera_transform_dvec3(transform->local_z_axis);
    x_length_squared = mdkr_camera_transform_dot(x, x);
    y_length_squared = mdkr_camera_transform_dot(y, y);
    z_length_squared = mdkr_camera_transform_dot(z, z);
    if (!isfinite(x_length_squared) || !isfinite(y_length_squared) || !isfinite(z_length_squared) ||
        x_length_squared <= DBL_MIN || y_length_squared <= DBL_MIN || z_length_squared <= DBL_MIN) {
        return 0;
    }
    x_length = sqrt(x_length_squared);
    y_length = sqrt(y_length_squared);
    z_length = sqrt(z_length_squared);
    tolerance = MDKR_CAMERA_TRANSFORM_RELATIVE_TOLERANCE * fmax(x_length, fmax(y_length, z_length));
    if (fabs(x_length - y_length) > tolerance || fabs(x_length - z_length) > tolerance ||
        fabs(mdkr_camera_transform_dot(x, y)) > tolerance * x_length ||
        fabs(mdkr_camera_transform_dot(x, z)) > tolerance * x_length ||
        fabs(mdkr_camera_transform_dot(y, z)) > tolerance * y_length) {
        return 0;
    }
    determinant = mdkr_camera_transform_dot(x, mdkr_camera_transform_cross(y, z));
    if (!isfinite(determinant) || determinant <= 0.0 ||
        fabs(determinant - x_length * y_length * z_length) >
            MDKR_CAMERA_TRANSFORM_RELATIVE_TOLERANCE * x_length * y_length * z_length) {
        return 0;
    }
    *out_scale = (x_length + y_length + z_length) / 3.0;
    return isfinite(*out_scale) && *out_scale > 0.0;
}

int mdkr_camera_object_transform_validate(
    const MdkrCameraObjectTransform *transform,
    float *out_uniform_scale) {
    double scale;

    if (out_uniform_scale == NULL || !mdkr_camera_transform_validate_internal(transform, &scale) ||
        scale > FLT_MAX) {
        return 0;
    }
    *out_uniform_scale = (float)scale;
    return 1;
}

int mdkr_camera_object_transform_from_yaw_pitch_roll(
    MdkrCameraVec3 translation,
    float yaw_radians,
    float pitch_radians,
    float roll_radians,
    float uniform_scale,
    MdkrCameraObjectTransform *out_transform) {
    const double yaw = yaw_radians;
    const double pitch = pitch_radians;
    const double roll = roll_radians;
    const double cy = cos(yaw);
    const double sy = sin(yaw);
    const double cp = cos(pitch);
    const double sp = sin(pitch);
    const double cr = cos(roll);
    const double sr = sin(roll);
    const double scale = uniform_scale;
    MdkrCameraObjectTransform transform;

    if (out_transform == NULL || !mdkr_camera_transform_finite_vec3(translation) ||
        !isfinite(yaw) || !isfinite(pitch) || !isfinite(roll) || !isfinite(scale) || scale <= 0.0) {
        return 0;
    }
    transform.translation = translation;
    transform.local_x_axis = (MdkrCameraVec3){
        (float)(scale * (cy * cr + sy * sp * sr)),
        (float)(scale * (cp * sr)),
        (float)(scale * (-sy * cr + cy * sp * sr)),
    };
    transform.local_y_axis = (MdkrCameraVec3){
        (float)(scale * (-cy * sr + sy * sp * cr)),
        (float)(scale * (cp * cr)),
        (float)(scale * (sy * sr + cy * sp * cr)),
    };
    transform.local_z_axis = (MdkrCameraVec3){
        (float)(scale * (sy * cp)),
        (float)(scale * (-sp)),
        (float)(scale * (cy * cp)),
    };
    if (!mdkr_camera_object_transform_validate(&transform, &uniform_scale)) {
        return 0;
    }
    *out_transform = transform;
    return 1;
}

int mdkr_camera_object_transform_point_to_local(
    const MdkrCameraObjectTransform *transform,
    MdkrCameraVec3 world_point,
    MdkrCameraVec3 *out_local_point) {
    double scale;
    MdkrCameraTransformDVec3 relative;
    MdkrCameraTransformDVec3 local;
    MdkrCameraTransformDVec3 x;
    MdkrCameraTransformDVec3 y;
    MdkrCameraTransformDVec3 z;

    if (out_local_point == NULL || !mdkr_camera_transform_finite_vec3(world_point) ||
        !mdkr_camera_transform_validate_internal(transform, &scale)) {
        return 0;
    }
    relative = mdkr_camera_transform_sub(
        mdkr_camera_transform_dvec3(world_point), mdkr_camera_transform_dvec3(transform->translation));
    x = mdkr_camera_transform_dvec3(transform->local_x_axis);
    y = mdkr_camera_transform_dvec3(transform->local_y_axis);
    z = mdkr_camera_transform_dvec3(transform->local_z_axis);
    local = (MdkrCameraTransformDVec3){
        mdkr_camera_transform_dot(relative, x) / (scale * scale),
        mdkr_camera_transform_dot(relative, y) / (scale * scale),
        mdkr_camera_transform_dot(relative, z) / (scale * scale),
    };
    return mdkr_camera_transform_to_float(local, out_local_point);
}

int mdkr_camera_object_transform_point_to_world(
    const MdkrCameraObjectTransform *transform,
    MdkrCameraVec3 local_point,
    MdkrCameraVec3 *out_world_point) {
    double scale;
    MdkrCameraTransformDVec3 world;

    if (out_world_point == NULL || !mdkr_camera_transform_finite_vec3(local_point) ||
        !mdkr_camera_transform_validate_internal(transform, &scale)) {
        return 0;
    }
    world = mdkr_camera_transform_dvec3(transform->translation);
    world = mdkr_camera_transform_add(world, mdkr_camera_transform_scale(
        mdkr_camera_transform_dvec3(transform->local_x_axis), local_point.x));
    world = mdkr_camera_transform_add(world, mdkr_camera_transform_scale(
        mdkr_camera_transform_dvec3(transform->local_y_axis), local_point.y));
    world = mdkr_camera_transform_add(world, mdkr_camera_transform_scale(
        mdkr_camera_transform_dvec3(transform->local_z_axis), local_point.z));
    return mdkr_camera_transform_to_float(world, out_world_point);
}

static int mdkr_camera_object_transform_vector_to_local(
    const MdkrCameraObjectTransform *transform,
    double scale,
    MdkrCameraVec3 world_vector,
    MdkrCameraVec3 *out_local_vector) {
    MdkrCameraTransformDVec3 vector;
    MdkrCameraTransformDVec3 local;
    MdkrCameraTransformDVec3 x;
    MdkrCameraTransformDVec3 y;
    MdkrCameraTransformDVec3 z;

    if (out_local_vector == NULL || !mdkr_camera_transform_finite_vec3(world_vector) ||
        !isfinite(scale) || scale <= 0.0) {
        return 0;
    }
    vector = mdkr_camera_transform_dvec3(world_vector);
    x = mdkr_camera_transform_dvec3(transform->local_x_axis);
    y = mdkr_camera_transform_dvec3(transform->local_y_axis);
    z = mdkr_camera_transform_dvec3(transform->local_z_axis);
    local = (MdkrCameraTransformDVec3){
        mdkr_camera_transform_dot(vector, x) / scale,
        mdkr_camera_transform_dot(vector, y) / scale,
        mdkr_camera_transform_dot(vector, z) / scale,
    };
    return mdkr_camera_transform_to_float(local, out_local_vector);
}

static int mdkr_camera_object_transform_normal_to_world(
    const MdkrCameraObjectTransform *transform,
    MdkrCameraVec3 local_normal,
    MdkrCameraVec3 *out_world_normal);

int mdkr_camera_rounded_lens_guard_to_object_local(
    const MdkrCameraObjectTransform *transform,
    const MdkrCameraRoundedLensGuard *world_guard,
    MdkrCameraVec3 world_eye,
    MdkrCameraVec3 *out_local_eye,
    MdkrCameraRoundedLensGuard *out_local_guard) {
    double scale;
    MdkrCameraRoundedLensGuard local_guard;
    MdkrCameraVec3 local_eye;

    if (out_local_eye == NULL || out_local_guard == NULL ||
        !mdkr_camera_transform_finite_vec3(world_eye) ||
        !mdkr_camera_rounded_lens_guard_valid(world_guard) ||
        !mdkr_camera_transform_validate_internal(transform, &scale)) {
        return 0;
    }
    if (!mdkr_camera_object_transform_point_to_local(transform, world_eye, &local_eye) ||
        !mdkr_camera_object_transform_vector_to_local(
            transform, scale, world_guard->forward, &local_guard.forward) ||
        !mdkr_camera_object_transform_vector_to_local(
            transform, scale, world_guard->right, &local_guard.right) ||
        !mdkr_camera_object_transform_vector_to_local(
            transform, scale, world_guard->up, &local_guard.up)) {
        return 0;
    }
    local_guard.near_distance = world_guard->near_distance / (float)scale;
    local_guard.half_width = world_guard->half_width / (float)scale;
    local_guard.half_height = world_guard->half_height / (float)scale;
    local_guard.skin = world_guard->skin / (float)scale;
    local_guard.broadphase_radius = world_guard->broadphase_radius / (float)scale;
    if (!mdkr_camera_rounded_lens_guard_valid(&local_guard)) {
        return 0;
    }
    *out_local_eye = local_eye;
    *out_local_guard = local_guard;
    return 1;
}

MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_object_local(
    const MdkrCameraOcclusionWorld *local_world,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraRoundedLensSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit) {
    return mdkr_camera_rounded_lens_sweep_object_local_profiled(
        local_world, object_transform, world_input, out_world_hit, NULL);
}

MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_object_local_profiled(
    const MdkrCameraOcclusionWorld *local_world,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraRoundedLensSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit,
    MdkrCameraRoundedLensSweepTelemetry *out_telemetry) {
    MdkrCameraRoundedLensSweepTelemetry local_telemetry;
    return mdkr_camera_rounded_lens_sweep_object_local_profiled_limited(
        local_world, object_transform, world_input, out_world_hit,
        out_telemetry != NULL ? out_telemetry : &local_telemetry,
        UINT64_MAX);
}

MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_object_local_profiled_limited(
    const MdkrCameraOcclusionWorld *local_world,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraRoundedLensSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit,
    MdkrCameraRoundedLensSweepTelemetry *out_telemetry,
    uint64_t stationary_test_limit) {
    float scale;
    MdkrCameraRoundedLensSweepInput local_input;
    MdkrCameraSweepHit local_hit;
    MdkrCameraSweepStatus status;

    if (out_telemetry != NULL) {
        memset(out_telemetry, 0, sizeof(*out_telemetry));
    }
    if (out_world_hit == NULL) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    memset(out_world_hit, 0, sizeof(*out_world_hit));
    if (world_input == NULL ||
        !mdkr_camera_object_transform_validate(object_transform, &scale) ||
        !mdkr_camera_rounded_lens_guard_to_object_local(
            object_transform, &world_input->guard, world_input->start_eye,
            &local_input.start_eye, &local_input.guard) ||
        !mdkr_camera_object_transform_point_to_local(
            object_transform, world_input->desired_eye, &local_input.desired_eye)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    local_input.mask = world_input->mask;
    local_input.ignored_object_generation = world_input->ignored_object_generation;
    status = mdkr_camera_rounded_lens_sweep_profiled_limited(
        local_world, &local_input, &local_hit, out_telemetry,
        stationary_test_limit);
    if (status != MDKR_CAMERA_SWEEP_HIT) {
        if (status == MDKR_CAMERA_SWEEP_CLEAR) {
            *out_world_hit = local_hit;
        }
        return status;
    }
    *out_world_hit = local_hit;
    if (!mdkr_camera_object_transform_point_to_world(
            object_transform, local_hit.point, &out_world_hit->point) ||
        !mdkr_camera_object_transform_normal_to_world(
            object_transform, local_hit.normal, &out_world_hit->normal)) {
        memset(out_world_hit, 0, sizeof(*out_world_hit));
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    out_world_hit->clearance *= scale;
    out_world_hit->penetration_depth *= scale;
    if (!isfinite(out_world_hit->clearance) || !isfinite(out_world_hit->penetration_depth)) {
        memset(out_world_hit, 0, sizeof(*out_world_hit));
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    return MDKR_CAMERA_SWEEP_HIT;
}

static int mdkr_camera_object_transform_normal_to_world(
    const MdkrCameraObjectTransform *transform,
    MdkrCameraVec3 local_normal,
    MdkrCameraVec3 *out_world_normal) {
    MdkrCameraVec3 world_tip;
    MdkrCameraTransformDVec3 world_normal;
    double length_squared;

    if (!mdkr_camera_object_transform_point_to_world(transform, local_normal, &world_tip)) {
        return 0;
    }
    world_normal = mdkr_camera_transform_sub(
        mdkr_camera_transform_dvec3(world_tip), mdkr_camera_transform_dvec3(transform->translation));
    length_squared = mdkr_camera_transform_dot(world_normal, world_normal);
    if (!isfinite(length_squared) || length_squared <= DBL_MIN) {
        return 0;
    }
    return mdkr_camera_transform_to_float(
        mdkr_camera_transform_scale(world_normal, 1.0 / sqrt(length_squared)), out_world_normal);
}

MdkrCameraSweepStatus mdkr_camera_sweep_object_local(
    const MdkrCameraOcclusionWorld *local_world,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit) {
    float scale;
    MdkrCameraSweepInput local_input;
    MdkrCameraSweepHit local_hit;
    MdkrCameraSweepStatus status;

    if (out_world_hit == NULL) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    memset(out_world_hit, 0, sizeof(*out_world_hit));
    if (world_input == NULL ||
        !mdkr_camera_object_transform_validate(object_transform, &scale) ||
        !isfinite(world_input->guard.radius) || world_input->guard.radius < 0.0f) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    local_input = *world_input;
    local_input.guard.radius /= scale;
    if (!isfinite(local_input.guard.radius) ||
        !mdkr_camera_object_transform_point_to_local(
            object_transform, world_input->start_eye, &local_input.start_eye) ||
        !mdkr_camera_object_transform_point_to_local(
            object_transform, world_input->desired_eye, &local_input.desired_eye)) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    status = mdkr_camera_sweep(local_world, &local_input, &local_hit);
    if (status != MDKR_CAMERA_SWEEP_HIT) {
        if (status == MDKR_CAMERA_SWEEP_CLEAR) {
            *out_world_hit = local_hit;
        }
        return status;
    }
    *out_world_hit = local_hit;
    if (!mdkr_camera_object_transform_point_to_world(
            object_transform, local_hit.point, &out_world_hit->point) ||
        !mdkr_camera_object_transform_normal_to_world(
            object_transform, local_hit.normal, &out_world_hit->normal)) {
        memset(out_world_hit, 0, sizeof(*out_world_hit));
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    out_world_hit->clearance *= scale;
    out_world_hit->penetration_depth *= scale;
    if (!isfinite(out_world_hit->clearance) || !isfinite(out_world_hit->penetration_depth)) {
        memset(out_world_hit, 0, sizeof(*out_world_hit));
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    return MDKR_CAMERA_SWEEP_HIT;
}
