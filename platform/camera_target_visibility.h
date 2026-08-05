#ifndef MDKR_CAMERA_TARGET_VISIBILITY_H
#define MDKR_CAMERA_TARGET_VISIBILITY_H

#include "camera_obstruction_query.h"

typedef enum MdkrCameraTargetVisibilityStatus {
    MDKR_CAMERA_TARGET_VISIBILITY_INVALID = 0,
    MDKR_CAMERA_TARGET_VISIBILITY_VISIBLE,
    MDKR_CAMERA_TARGET_VISIBILITY_HIDDEN,
    MDKR_CAMERA_TARGET_VISIBILITY_EMBEDDED,
} MdkrCameraTargetVisibilityStatus;

/* Thin current-pose composition query. A focus that remains initially
 * overlapping after bounded local skin exclusion is EMBEDDED, never visible. */
MdkrCameraTargetVisibilityStatus mdkr_camera_target_visibility_query(
    const MdkrCameraObstructionCombinedQuery *query,
    MdkrCameraVec3 target,
    MdkrCameraVec3 eye,
    uint32_t mask,
    MdkrCameraSweepHit *out_hit);

#endif /* MDKR_CAMERA_TARGET_VISIBILITY_H */
