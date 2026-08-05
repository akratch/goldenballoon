#ifndef MDKR_CAMERA_DYNAMIC_TEMPORAL_H
#define MDKR_CAMERA_DYNAMIC_TEMPORAL_H

#ifdef NATIVE_PORT

#include "camera_obstruction_transform.h"
#include "structs.h"

typedef struct MdkrCameraDynamicAabb {
    MdkrCameraVec3 minimum;
    MdkrCameraVec3 maximum;
} MdkrCameraDynamicAabb;

/* Transform a model-local AABB through an already validated renderer transform. */
int mdkr_camera_dynamic_world_aabb(
    const MdkrCameraDynamicAabb *local_bounds,
    const MdkrCameraObjectTransform *transform,
    MdkrCameraDynamicAabb *out_bounds);

/* Conservative fixed-tick presentation envelope. Poses are sampled through
 * mtxf_from_transform() and inflated by an analytic motion/numeric bound. */
int mdkr_camera_dynamic_temporal_bounds(
    const MdkrCameraDynamicAabb *local_bounds,
    const ObjectTransform *previous,
    const ObjectTransform *current,
    MdkrCameraDynamicAabb *out_bounds);

/* Conservative camera-chord broad phase used to cut interpolation for moving
 * non-door solids whose fractional transform is otherwise unrepresented. */
int mdkr_camera_dynamic_swept_aabb_intersects(
    const MdkrCameraDynamicAabb *bounds,
    MdkrCameraVec3 start_eye,
    MdkrCameraVec3 desired_eye,
    double radius);

#endif /* NATIVE_PORT */

#endif /* MDKR_CAMERA_DYNAMIC_TEMPORAL_H */
