#ifndef CAMERA_OBSTRUCTION_RUNTIME_H
#define CAMERA_OBSTRUCTION_RUNTIME_H

#ifdef NATIVE_PORT
#include "camera.h"
#include "camera_obstruction.h"

typedef enum MdkrCameraIntentFamily {
    MDKR_CAMERA_INTENT_FAMILY_UNKNOWN = 0,
    MDKR_CAMERA_INTENT_FAMILY_CAR,
    MDKR_CAMERA_INTENT_FAMILY_HOVERCRAFT,
    MDKR_CAMERA_INTENT_FAMILY_PLANE,
    MDKR_CAMERA_INTENT_FAMILY_LOOP,
    MDKR_CAMERA_INTENT_FAMILY_FIXED,
    MDKR_CAMERA_INTENT_FAMILY_FINISH_CHALLENGE,
    MDKR_CAMERA_INTENT_FAMILY_FINISH_RACE,
    MDKR_CAMERA_INTENT_FAMILY_TT_SPECTATE,
    MDKR_CAMERA_INTENT_FAMILY_SCRIPTED_CUTSCENE,
    /* Not a family: the table bound for per-family lookup tables. */
    MDKR_CAMERA_INTENT_FAMILY_COUNT,
} MdkrCameraIntentFamily;

/*
 * Native sidecar only. Camera remains ABI-identical; authors provide the exact
 * shot data while they still know its pivot/target rather than reverse-
 * engineering it later from an eye position.
 */
typedef struct MdkrCameraIntent {
    MdkrCameraVec3 desired_eye;
    MdkrCameraVec3 pivot;
    MdkrCameraVec3 target;
    MdkrCameraVec3 forward;
    int camera_id;             /* Physical 0..7 bank slot. */
    int authored_mode;
    MdkrCameraIntentFamily family;
    unsigned char target_valid;
    unsigned char forward_valid;
    unsigned char discontinuity;
} MdkrCameraIntent;

/* Last author wins for a physical slot; finalization consumes it once. */
void camera_obstruction_intent_capture(const MdkrCameraIntent *intent);

/*
 * Presentation reads a resolved native sidecar while gameplay continues to
 * own and consume gCameras unchanged. The scope is deliberately explicit so
 * authority passes before render cannot accidentally observe corrected poses.
 */
void camera_obstruction_presentation_begin(void);
void camera_obstruction_presentation_end(void);
Camera *camera_obstruction_camera_for_slot(s32 physical_slot);
const Camera *camera_obstruction_snapshot_camera_for_slot(s32 physical_slot);
int camera_obstruction_snapshot_discontinuous(s32 physical_slot);
int camera_obstruction_projection_matches_render(
    s32 viewport, s32 physical_slot, uint64_t generation);
int camera_obstruction_racer_opacity_for_viewport(s32 viewport);
#endif

/* Native fixed-tick finalizer. Observe is telemetry-only; correction policies
 * publish exclusively to the presentation sidecar. No policy writes
 * Camera/gCameras or simulation state. */
void camera_obstruction_tick(int update_rate_fields);
void camera_obstruction_runtime_reset(void);
/*
 * Register this runtime as the owner of Camera.Obstruction's deferred apply
 * (video_config.h). Called once during engine startup. Until it is, the key
 * behaves exactly as it always did: resolved from MDKR_CAMERA_OBSTRUCTION at
 * the first tick that asks, with no way to change it under a running engine.
 */
void camera_obstruction_runtime_install_config_apply(void);
/* Emit the optional MDKR_CAMERA_PERF allocation-free timing census. */
void camera_obstruction_perf_summary(void);
/*
 * MOTION-01. Emit the optional allocation-free motion-quality census for the
 * whole run: retract/recovery latencies, resolved-camera velocity/jerk, blocker
 * churn, shoulder flips, emergency dwell, and discontinuity density. Collection
 * is off unless MDKR_CAMERA_TRACE (or MDKR_CAMERA_MOTION) is on, so a release
 * build with tracing off pays nothing.
 */
void camera_obstruction_motion_summary(void);

#endif /* CAMERA_OBSTRUCTION_RUNTIME_H */
