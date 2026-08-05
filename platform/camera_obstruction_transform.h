/**
 * camera_obstruction_transform.h -- pure object-local camera sweep support.
 *
 * Dynamic object models retain immutable local-space occlusion meshes. This
 * module adapts a world-space camera sweep to one such mesh without accessing
 * object/game state or allocating memory. Its transform is explicitly a
 * right-handed rigid transform with one positive uniform scale; mirroring,
 * non-uniform scale, and shear fail closed instead of distorting a sphere.
 */
#ifndef MDKR64_CAMERA_OBSTRUCTION_TRANSFORM_H
#define MDKR64_CAMERA_OBSTRUCTION_TRANSFORM_H

#include "camera_obstruction.h"

#ifdef __cplusplus
extern "C" {
#endif

/* World-space images of the local unit axes, each multiplied by uniform scale. */
typedef struct MdkrCameraObjectTransform {
    MdkrCameraVec3 translation;
    MdkrCameraVec3 local_x_axis;
    MdkrCameraVec3 local_y_axis;
    MdkrCameraVec3 local_z_axis;
} MdkrCameraObjectTransform;

/**
 * Build a validated positive-scale transform. Angles are radians and compose as
 * world Y yaw, then local X pitch, then local Z roll (Ry * Rx * Rz).
 */
int mdkr_camera_object_transform_from_yaw_pitch_roll(
    MdkrCameraVec3 translation,
    float yaw_radians,
    float pitch_radians,
    float roll_radians,
    float uniform_scale,
    MdkrCameraObjectTransform *out_transform);

/* Validate the public basis contract and return its positive uniform scale. */
int mdkr_camera_object_transform_validate(
    const MdkrCameraObjectTransform *transform,
    float *out_uniform_scale);

int mdkr_camera_object_transform_point_to_local(
    const MdkrCameraObjectTransform *transform,
    MdkrCameraVec3 world_point,
    MdkrCameraVec3 *out_local_point);

int mdkr_camera_object_transform_point_to_world(
    const MdkrCameraObjectTransform *transform,
    MdkrCameraVec3 local_point,
    MdkrCameraVec3 *out_world_point);

/**
 * Convert the exact eye-to-near-plane guard to immutable object-local space.
 *
 * This is only valid for the module's positive uniform, right-handed transform
 * contract.  The eye is translated and rotated into local space, whereas the
 * guard basis vectors are rotated only.  All length-valued guard fields are
 * divided by the uniform scale, so an exact local triangle test is equivalent
 * to testing the uniformly transformed triangle in world space.  In
 * particular, the renderer convention remains right x up == -forward.
 *
 * The helper is pure and allocation-free. It leaves every input untouched and
 * fails closed for invalid transforms, invalid rounded guards, or non-finite
 * conversion results.
 */
int mdkr_camera_rounded_lens_guard_to_object_local(
    const MdkrCameraObjectTransform *transform,
    const MdkrCameraRoundedLensGuard *world_guard,
    MdkrCameraVec3 world_eye,
    MdkrCameraVec3 *out_local_eye,
    MdkrCameraRoundedLensGuard *out_local_guard);

/**
 * Sweep an exact world-space rounded lens against caller-owned object-local
 * geometry. Transform validity, guard conversion, and all finite contracts
 * fail closed. Fraction and metadata are retained from the local sweep; the
 * point, normal, clearance, and penetration depth are returned in world space.
 */
MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_object_local(
    const MdkrCameraOcclusionWorld *local_world,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraRoundedLensSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit);

MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_object_local_profiled(
    const MdkrCameraOcclusionWorld *local_world,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraRoundedLensSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit,
    MdkrCameraRoundedLensSweepTelemetry *out_telemetry);

MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_object_local_profiled_limited(
    const MdkrCameraOcclusionWorld *local_world,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraRoundedLensSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit,
    MdkrCameraRoundedLensSweepTelemetry *out_telemetry,
    uint64_t stationary_test_limit);

/**
 * Sweep a world-space guard against caller-owned object-local indexed geometry.
 * The returned point, normal, clearance, and penetration depth are world-space;
 * fraction and metadata remain identical to the underlying pure kernel result.
 */
MdkrCameraSweepStatus mdkr_camera_sweep_object_local(
    const MdkrCameraOcclusionWorld *local_world,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_CAMERA_OBSTRUCTION_TRANSFORM_H */
