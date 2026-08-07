/*
 * camera_obstruction_resolver.h -- pure fixed-step spring-arm policy.
 *
 * This layer owns no world data and has no hidden temporal state.  Callers keep
 * one MdkrCameraObstructionResolverState per camera slot and pass the exact
 * latched projection used for the resulting image.  It deliberately does not
 * know about environment variables, DKR objects, or camera-bank selection.
 */
#ifndef MDKR64_CAMERA_OBSTRUCTION_RESOLVER_H
#define MDKR64_CAMERA_OBSTRUCTION_RESOLVER_H

#include "camera_obstruction.h"
#include "display_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdkrCameraObstructionPolicy {
    /* The shipping policy: projection-derived lens guard plus spring arm. */
    MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN = 0,
    /* Diagnostic compatibility mode: publish the authored eye unchanged. */
    MDKR_CAMERA_OBSTRUCTION_POLICY_LEGACY = 1,
    /* Diagnostic control: sweep the eye centre only, never the near plane. */
    MDKR_CAMERA_OBSTRUCTION_POLICY_CENTER_RAY = 2,
} MdkrCameraObstructionPolicy;

typedef struct MdkrCameraObstructionResolverConfig {
    MdkrCameraObstructionPolicy policy;
    /* Extra world-space separation after a contact. Must be finite and >= 0. */
    float clearance_skin;
    /* Maximum expansion speed in world units per second. Must be finite and > 0. */
    float recovery_speed;
    /* Absolute cap on any one expansion. Must be finite and > 0. */
    float max_recovery_step;
    /*
     * Release hysteresis on retraction, in consecutive authored ticks.
     *
     * Retraction ENGAGES on contact with zero latency and this field cannot
     * delay it. It RELEASES -- that is, the boom is allowed to start expanding
     * again -- only once the anchor->desired path has been reported clear for
     * this many consecutive accepted ticks. A shorter clear run holds the boom
     * at its retracted length instead of expanding into a contact that has not
     * actually gone away.
     *
     * The band this closes is a false negative, not a tuning preference: a
     * swept lens volume grazing a wall reports CLEAR for a few ticks at facet
     * seams and at shallow incidence, and expanding on that report pops the
     * boom toward desired and snaps it back on the next tick. 0 disables the
     * hold and restores release-on-first-clear-tick.
     */
    uint32_t release_hold_ticks;
} MdkrCameraObstructionResolverConfig;

/* Caller-owned state. Zero-initialize or call mdkr_camera_obstruction_resolver_reset. */
typedef struct MdkrCameraObstructionResolverState {
    MdkrCameraVec3 last_safe_eye;
    uint64_t projection_generation;
    /* Consecutive accepted clear ticks since the latched contact. */
    uint32_t clear_run_ticks;
    uint8_t last_safe_valid;
    /* A retraction is engaged and has not served its release hold yet. */
    uint8_t retraction_latched;
} MdkrCameraObstructionResolverState;

/*
 * The resolver never chooses an acceleration structure. The finalizer supplies
 * this immutable, side-effect-free query so track BVHs and later static/dynamic
 * composition remain the single authoritative occlusion route.
 */
typedef MdkrCameraSweepStatus (*MdkrCameraObstructionSweepFn)(
    const void *context,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit);

typedef struct MdkrCameraObstructionQuery {
    MdkrCameraObstructionSweepFn sweep;
    const void *context;
} MdkrCameraObstructionQuery;

/* Adapter for a raw kernel world; ROM-free tests use this, gameplay need not. */
MdkrCameraSweepStatus mdkr_camera_occlusion_world_sweep(
    const void *context,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit);

typedef struct MdkrCameraObstructionResolverInput {
    MdkrCameraObstructionQuery query;
    const MdkrCameraProjection *projection;
    MdkrCameraVec3 pivot;
    /* A caller-provided candidate known safe in the previous authored step. */
    MdkrCameraVec3 start_safe_eye;
    MdkrCameraVec3 desired_eye;
    float fixed_delta_seconds;
    uint32_t mask;
    uint32_t ignored_object_generation;
    uint8_t discontinuity;
} MdkrCameraObstructionResolverInput;

typedef enum MdkrCameraObstructionResolverStatus {
    MDKR_CAMERA_OBSTRUCTION_RESOLVER_CLEAR = 0,
    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED = 1,
    /* A clear path exists, but bounded expansion has not reached desired yet. */
    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RECOVERING = 2,
    /* No new safe pose was accepted; caller must hold/fallback explicitly. */
    MDKR_CAMERA_OBSTRUCTION_RESOLVER_FAILSAFE = 3,
    MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID = 4,
} MdkrCameraObstructionResolverStatus;

typedef struct MdkrCameraObstructionResolverResult {
    MdkrCameraObstructionResolverStatus status;
    MdkrCameraVec3 resolved_eye;
    MdkrCameraLensGuard guard;
    uint32_t blocker_kind;
    uint32_t blocker_stable_id;
    /*
     * Outward contact normal of the blocker that forced the retraction, valid
     * only when blocker_stable_id is set. Reported, never consumed: the
     * resolver's policy does not depend on blocker identity or orientation.
     * MOTION-01 needs it because a static blocker's stable ID is per-triangle,
     * so only the normal can tell one continuous wall from a new surface.
     */
    MdkrCameraVec3 blocker_normal;
    /*
     * Contact point of that same blocker, and its fraction along the
     * anchor->desired boom. Reported, never consumed by the resolver's own
     * policy. A caller that must weigh a blocker (how close is it to the
     * authored eye, how much boom does it cost) cannot recover either from
     * blocker_stable_id, and re-sweeping to find out would double the work.
     */
    MdkrCameraVec3 blocker_point;
    float blocker_fraction;
    uint8_t accepted;
    /* True only when the anchor-to-desired sweep itself was blocked. */
    uint8_t path_was_blocked;
    /*
     * True when the status is RETRACTED because a latched retraction is still
     * serving its release hold, not because this tick's sweep hit anything.
     * Reported so a trace can tell a contact tick from a held one; blocker
     * identity is deliberately left empty on a held tick, because no blocker
     * was measured.
     */
    uint8_t release_held;
    uint8_t used_last_safe;
    uint8_t projection_revalidated;
} MdkrCameraObstructionResolverResult;

void mdkr_camera_obstruction_resolver_reset(
    MdkrCameraObstructionResolverState *state);

/*
 * Resolve one fixed authored camera pose without allocating or mutating world
 * input. Retraction is immediate; a clear boom expands by at most
 * min(recovery_speed * fixed_delta_seconds, max_recovery_step), and only once
 * release_hold_ticks consecutive clear ticks have retired a latched
 * retraction. On INVALID or an overlap with no revalidated fallback, accepted
 * is zero and state is not advanced to an unsafe eye.
 *
 * This is a positional prototype, not generic camera smoothing. Integration
 * must feed a pivot-relative boom-safe anchor; do not damp arbitrary authored
 * translation or look-direction changes through this API.
 */
MdkrCameraObstructionResolverStatus mdkr_camera_obstruction_resolve(
    const MdkrCameraObstructionResolverConfig *config,
    const MdkrCameraObstructionResolverInput *input,
    MdkrCameraObstructionResolverState *state,
    MdkrCameraObstructionResolverResult *out_result);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_CAMERA_OBSTRUCTION_RESOLVER_H */
