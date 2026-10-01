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
     * this many consecutive accepted positive-time ticks. A shorter clear run
     * holds the boom at its retracted length instead of expanding into a
     * contact that has not actually gone away.
     *
     * The band this closes is a false negative, not a tuning preference: a
     * swept lens volume grazing a wall reports CLEAR for a few ticks at facet
     * seams and at shallow incidence, and expanding on that report pops the
     * boom toward desired and snaps it back on the next tick. 0 disables the
     * hold and restores release-on-first-clear-tick.
     */
    uint32_t release_hold_ticks;
    /*
     * Further consecutive clear ticks a served hold may wait while the caller
     * still reports the corridor as near contact (see
     * MdkrCameraObstructionResolverInput::corridor_near_contact). A boom that
     * stays clear but keeps grazing the surface it retracted from has not
     * moved away from it, and releasing there lets the next facet of the same
     * wall re-engage the correction a few ticks later. The extension is a
     * bound, not a second hold: a corridor that is clear by the caller's
     * margin releases at release_hold_ticks as before, and one that keeps
     * grazing still releases after release_hold_ticks plus this many ticks.
     * 0 disables it.
     */
    uint32_t release_near_contact_extension_ticks;
    /* Optional expansion cap as a fraction of remaining error, [0, 1].
     * Zero preserves constant-speed recovery. Positive values ease arrival;
     * numerical settling may finish within four float epsilons at world scale.
     * This never limits collision retraction or advances zero-time recovery. */
    float max_recovery_fraction;
} MdkrCameraObstructionResolverConfig;

/* Caller-owned state. Zero-initialize or call mdkr_camera_obstruction_resolver_reset. */
typedef struct MdkrCameraObstructionResolverState {
    MdkrCameraVec3 last_safe_eye;
    uint64_t projection_generation;
    /* Consecutive accepted positive-time clear ticks since the latched contact. */
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
    /* Zero revalidates geometry without advancing recovery or its release hold. */
    float fixed_delta_seconds;
    uint32_t mask;
    uint32_t ignored_object_generation;
    uint8_t discontinuity;
    /*
     * The caller measured this tick's clear anchor->desired corridor within
     * its release margin of geometry: clear, but still grazing. Read only
     * while a served release hold is deciding whether to let go; see
     * release_near_contact_extension_ticks.
     */
    uint8_t corridor_near_contact;
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
 * One authored tick of the release hysteresis described on
 * release_hold_ticks above. mdkr_camera_obstruction_resolve() spends its hold
 * through this function, and so must any caller that recovers a latched
 * correction on a path of its own, so that no sibling path can release on the
 * first clear tick the hold exists to distrust.
 *
 * contact: this tick's corridor sweep reported a blocker. Contact latches the
 * retraction and restarts the clear run at once, at any dt; the hold never
 * delays engagement. Otherwise a latched retraction counts this tick only when
 * fixed_delta_seconds is positive (zero-time revalidation proves geometry, not
 * elapsed authored time) and releases once the run reaches
 * release_hold_ticks -- unless near_contact reports the clear corridor still
 * grazing geometry, in which case it waits until the run also reaches
 * release_hold_ticks + near_contact_extension_ticks.
 *
 * Returns 1 when the correction may recover this tick, 0 while it must hold.
 * Null state pointers return 0 and change nothing.
 */
int mdkr_camera_release_hold_step(uint8_t *retraction_latched,
                                  uint32_t *clear_run_ticks,
                                  int contact,
                                  int near_contact,
                                  float fixed_delta_seconds,
                                  uint32_t release_hold_ticks,
                                  uint32_t near_contact_extension_ticks);

/*
 * max_recovery_fraction for one fixed step. `fraction_per_reference` is the
 * share of the remaining error recovery may close per `reference_seconds` of
 * authored time; the result is the per-step cap that closes the same share per
 * second at any step length, 1 - (1 - fraction)^(step / reference). It equals
 * the input when the step is the reference, is 0 for a zero (paused) step, and
 * two half steps leave exactly the remaining error one full step does. A
 * nonfinite input or reference returns NAN, which the resolver rejects.
 */
float mdkr_camera_recovery_fraction_for_step(float fraction_per_reference,
                                             float reference_seconds,
                                             float fixed_delta_seconds);

/* Transport an eye's correction relative to the authored horizontal boom.
 * Preserves lateral/longitudinal/vertical correction as the target or authored
 * eye moves, before recovery shrinks it. This is geometry, not clearance:
 * callers must validate the complete resulting transition. Degenerate booms,
 * nonfinite inputs or unrepresentable output fail without changing out_eye. */
int mdkr_camera_recovery_transport(MdkrCameraVec3 previous_eye,
    MdkrCameraVec3 previous_pivot, MdkrCameraVec3 previous_desired,
    MdkrCameraVec3 pivot, MdkrCameraVec3 desired, MdkrCameraVec3 *out_eye);

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
