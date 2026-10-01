/* Published-pose shoulder census; never consumed by the camera resolver. */
#ifndef MDKR64_CAMERA_MOTION_SHOULDER_H
#define MDKR64_CAMERA_MOTION_SHOULDER_H

#include "camera_obstruction.h"

#include <math.h>

/* The existing lateral side-label deadband, not an added movement tolerance. */
#define MDKR_CAMERA_MOTION_SHOULDER_EPSILON 1.0f

typedef struct MdkrCameraShoulderHistory {
    MdkrCameraVec3 eye;
    MdkrCameraVec3 pivot;
    MdkrCameraVec3 previous_eye;
    MdkrCameraVec3 previous_pivot;
    int side;
    int previous_valid;
    int moved;
} MdkrCameraShoulderHistory;

/*
 * MOTION-01's phase classification for one published tick: an active
 * correction constrains the camera. A retraction (contact or release hold)
 * and emergency framing are blocked; an alternate shot -- a fan shot or a held
 * shot -- is blocked while it holds and is the clear phase while it RECOVERS,
 * because recovery, on every path, is the correction letting go.
 */
static inline int mdkr_camera_motion_blocked(int retracted, int emergency,
                                             int alternate, int recovering) {
    return retracted || emergency || (alternate && !recovering);
}

/* A shoulder is the side one correction holds against one obstruction, so the
 * census tracks it only for a valid alternate shot in the blocked phase. */
static inline int mdkr_camera_shoulder_tracked(int alternate, int pose_valid,
                                               int blocked) {
    return alternate && pose_valid && blocked;
}

typedef enum MdkrCameraShoulderChange {
    MDKR_CAMERA_SHOULDER_UNCHANGED = 0,
    MDKR_CAMERA_SHOULDER_FLIP,
    MDKR_CAMERA_SHOULDER_BASIS_CROSSING,
} MdkrCameraShoulderChange;

/* Half an ULP bounds rounding of a published float coordinate. Use the wider
 * adjacent spacing at powers of two, and the finite neighbor at FLT_MAX. */
static inline double mdkr_camera_shoulder_rounding_bound(float value) {
    const float above = nextafterf(value, INFINITY);
    const float below = nextafterf(value, -INFINITY);
    const double upper = isfinite(above) ? fabs((double)above - value) : 0.0;
    const double lower = isfinite(below) ? fabs((double)value - below) : 0.0;
    return 0.5 * fmax(upper, lower);
}

static inline int mdkr_camera_shoulder_axis_held(
    float eye, float pivot, float previous_eye, float previous_pivot) {
    const double displacement = ((double)eye - pivot) -
        ((double)previous_eye - previous_pivot);
    const double rounding = mdkr_camera_shoulder_rounding_bound(eye) +
        mdkr_camera_shoulder_rounding_bound(pivot) +
        mdkr_camera_shoulder_rounding_bound(previous_eye) +
        mdkr_camera_shoulder_rounding_bound(previous_pivot);
    return fabs(displacement) <= rounding;
}

/* side is the published eye's signed lateral coordinate after the existing
 * deadband. Retain the last nonzero sample THROUGH that deadband: comparing
 * only adjacent ticks would miss a real, slow shoulder switch.
 *
 * A side-label change with an unchanged pivot-relative eye is rotation of the
 * authored reference axis, not camera motion. Check quantization EACH tick so
 * the repeated float translations across deadband ticks can round differently.
 * Also cap cumulative drift below the side deadband: a very slow real switch
 * must not disappear into an indefinitely accumulated rounding allowance.
 * Keep these crossings separately visible. Invalid/inactive samples retire
 * this history. */
static inline MdkrCameraShoulderChange mdkr_camera_shoulder_sample(
    MdkrCameraShoulderHistory *history, MdkrCameraVec3 eye,
    MdkrCameraVec3 pivot, int side, int active) {
    MdkrCameraShoulderChange change = MDKR_CAMERA_SHOULDER_UNCHANGED;

    if (!active || !isfinite(eye.x) || !isfinite(eye.y) || !isfinite(eye.z) ||
        !isfinite(pivot.x) || !isfinite(pivot.y) || !isfinite(pivot.z)) {
        history->side = 0;
        history->previous_valid = 0;
        history->moved = 0;
        return change;
    }
    if (history->previous_valid &&
        !(mdkr_camera_shoulder_axis_held(eye.x, pivot.x,
              history->previous_eye.x, history->previous_pivot.x) &&
          mdkr_camera_shoulder_axis_held(eye.y, pivot.y,
              history->previous_eye.y, history->previous_pivot.y) &&
          mdkr_camera_shoulder_axis_held(eye.z, pivot.z,
              history->previous_eye.z, history->previous_pivot.z))) {
        history->moved = 1;
    }
    history->previous_eye = eye;
    history->previous_pivot = pivot;
    history->previous_valid = 1;
    if (side == 0) {
        return change;
    }
    if (history->side != 0 && side != history->side) {
        const double dx = ((double)eye.x - pivot.x) -
            ((double)history->eye.x - history->pivot.x);
        const double dy = ((double)eye.y - pivot.y) -
            ((double)history->eye.y - history->pivot.y);
        const double dz = ((double)eye.z - pivot.z) -
            ((double)history->eye.z - history->pivot.z);
        const int held = !history->moved &&
            dx * dx + dy * dy + dz * dz <
                MDKR_CAMERA_MOTION_SHOULDER_EPSILON *
                    MDKR_CAMERA_MOTION_SHOULDER_EPSILON;
        change = held ? MDKR_CAMERA_SHOULDER_BASIS_CROSSING :
                        MDKR_CAMERA_SHOULDER_FLIP;
    }
    history->eye = eye;
    history->pivot = pivot;
    history->side = side;
    history->moved = 0;
    return change;
}

#endif
