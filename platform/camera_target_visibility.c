#include "camera_target_visibility.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static MdkrCameraVec3 mdkr_camera_target_visibility_lerp(
    MdkrCameraVec3 start,
    MdkrCameraVec3 end,
    float fraction) {
    return (MdkrCameraVec3) {
        start.x + (end.x - start.x) * fraction,
        start.y + (end.y - start.y) * fraction,
        start.z + (end.z - start.z) * fraction,
    };
}

static float mdkr_camera_target_visibility_distance(
    MdkrCameraVec3 left,
    MdkrCameraVec3 right) {
    const double x = (double)right.x - left.x;
    const double y = (double)right.y - left.y;
    const double z = (double)right.z - left.z;
    const double squared = x * x + y * y + z * z;

    if (!isfinite(squared) || squared < 0.0) {
        return NAN;
    }
    return (float)sqrt(squared);
}

static int mdkr_camera_target_visibility_finite(MdkrCameraVec3 value) {
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

MdkrCameraTargetVisibilityStatus mdkr_camera_target_visibility_query(
    const MdkrCameraObstructionCombinedQuery *query,
    MdkrCameraVec3 target,
    MdkrCameraVec3 eye,
    uint32_t mask,
    MdkrCameraSweepHit *out_hit) {
    static const float target_exclusion_steps[] = {
        1.0f, 2.0f, 4.0f, 8.0f, 16.0f,
    };
    MdkrCameraSweepInput sightline = {
        .guard = { MDKR_CAMERA_LENS_GUARD_SPHERE, 0.25f },
        .start_eye = target,
        .desired_eye = eye,
        .mask = mask,
    };
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus status;
    size_t step_index;

    if (out_hit != NULL) memset(out_hit, 0, sizeof(*out_hit));
    if (query == NULL || mask == 0U ||
        !mdkr_camera_target_visibility_finite(target) ||
        !mdkr_camera_target_visibility_finite(eye)) {
        return MDKR_CAMERA_TARGET_VISIBILITY_INVALID;
    }
    status = mdkr_camera_obstruction_combined_sweep(query, &sightline, &hit);
    if (status == MDKR_CAMERA_SWEEP_CLEAR) {
        return MDKR_CAMERA_TARGET_VISIBILITY_VISIBLE;
    }
    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        return MDKR_CAMERA_TARGET_VISIBILITY_INVALID;
    }
    if (!hit.started_overlapping) {
        if (out_hit != NULL) *out_hit = hit;
        return MDKR_CAMERA_TARGET_VISIBILITY_HIDDEN;
    }

    {
        const float distance = mdkr_camera_target_visibility_distance(target, eye);

        if (!isfinite(distance)) {
            return MDKR_CAMERA_TARGET_VISIBILITY_INVALID;
        }
        if (distance > 1.0f) {
            for (step_index = 0U;
                 step_index < sizeof(target_exclusion_steps) /
                     sizeof(target_exclusion_steps[0]);
                 step_index++) {
                const float step = target_exclusion_steps[step_index];

                if (step >= distance) break;
                sightline.start_eye = mdkr_camera_target_visibility_lerp(
                    target, eye, step / distance);
                status = mdkr_camera_obstruction_combined_sweep(
                    query, &sightline, &hit);
                if (status == MDKR_CAMERA_SWEEP_CLEAR) {
                    return MDKR_CAMERA_TARGET_VISIBILITY_VISIBLE;
                }
                if (status == MDKR_CAMERA_SWEEP_INVALID) {
                    return MDKR_CAMERA_TARGET_VISIBILITY_INVALID;
                }
                if (!hit.started_overlapping) {
                    if (out_hit != NULL) *out_hit = hit;
                    return MDKR_CAMERA_TARGET_VISIBILITY_HIDDEN;
                }
            }
        }
    }
    if (out_hit != NULL) *out_hit = hit;
    return MDKR_CAMERA_TARGET_VISIBILITY_EMBEDDED;
}
