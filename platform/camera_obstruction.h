/**
 * camera_obstruction.h -- native camera-obstruction geometry primitives.
 *
 * This module deliberately knows nothing about DKR objects, collision state, or
 * allocation.  It is the small, ROM-free foundation for the camera occlusion
 * world: callers provide immutable indexed triangles and receive a pure swept
 * lens-guard result.  The initial guard implementation is a conservative sphere;
 * the tagged guard keeps the public query contract ready for a rounded lens
 * guard without changing camera callers later.
 */
#ifndef MDKR64_CAMERA_OBSTRUCTION_H
#define MDKR64_CAMERA_OBSTRUCTION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrCameraVec3 {
    float x;
    float y;
    float z;
} MdkrCameraVec3;

typedef enum MdkrCameraLensGuardKind {
    MDKR_CAMERA_LENS_GUARD_SPHERE = 1,
    /* Reserved for CAM-02's later exact rounded eye-to-near-plane guard. */
    MDKR_CAMERA_LENS_GUARD_ROUNDED_LENS = 2,
} MdkrCameraLensGuardKind;

typedef struct MdkrCameraLensGuard {
    MdkrCameraLensGuardKind kind;
    /* Includes any caller-selected clearance skin. */
    float radius;
} MdkrCameraLensGuard;

/*
 * The exact visual guard is the solid convex hull of the eye and the four
 * near-plane corners, expanded by skin.  It is intentionally a separate
 * narrow-phase representation: existing sphere callers stay byte-for-byte
 * sphere callers until their broad phase can retain all candidate triangles.
 *
 * `forward` points from the eye through the image; `right` and `up` span the
 * near plane.  It follows the renderer's {right, up, back} convention, so the
 * orthonormal basis satisfies right x up == -forward.
 * `broadphase_radius` encloses the rounded guard at the eye and is supplied so
 * a caller can use the current swept-sphere/BVH route as a conservative first
 * pass before calling the per-triangle narrow phase below.
 */
typedef struct MdkrCameraRoundedLensGuard {
    MdkrCameraVec3 forward;
    MdkrCameraVec3 right;
    MdkrCameraVec3 up;
    float near_distance;
    float half_width;
    float half_height;
    float skin;
    float broadphase_radius;
} MdkrCameraRoundedLensGuard;

/* Metadata is kept separate from indexed vertex storage for cache/BVH builders. */
typedef struct MdkrCameraOcclusionTriangle {
    uint32_t stable_id;
    uint32_t mask;
    uint32_t kind;
    uint32_t object_generation;
} MdkrCameraOcclusionTriangle;

typedef struct MdkrCameraOcclusionWorld {
    const MdkrCameraVec3 *vertices;
    size_t vertex_count;
    /* Exactly three indices per triangle, in the same order as triangles. */
    const uint32_t *indices;
    const MdkrCameraOcclusionTriangle *triangles;
    size_t triangle_count;
} MdkrCameraOcclusionWorld;

typedef struct MdkrCameraSweepInput {
    MdkrCameraLensGuard guard;
    MdkrCameraVec3 start_eye;
    MdkrCameraVec3 desired_eye;
    /* Zero selects every triangle; otherwise metadata mask intersection selects. */
    uint32_t mask;
    /* Zero ignores none; nonzero skips matching dynamic object generations. */
    uint32_t ignored_object_generation;
} MdkrCameraSweepInput;

/* Fixed-basis continuous translation input for the exact rounded-lens path. */
typedef struct MdkrCameraRoundedLensSweepInput {
    MdkrCameraRoundedLensGuard guard;
    MdkrCameraVec3 start_eye;
    MdkrCameraVec3 desired_eye;
    /* Zero selects every triangle; otherwise metadata mask intersection selects. */
    uint32_t mask;
    /* Zero ignores none; nonzero skips matching dynamic object generations. */
    uint32_t ignored_object_generation;
} MdkrCameraRoundedLensSweepInput;

typedef enum MdkrCameraSweepFeature {
    MDKR_CAMERA_SWEEP_FEATURE_NONE = 0,
    MDKR_CAMERA_SWEEP_FEATURE_FACE = 1,
    MDKR_CAMERA_SWEEP_FEATURE_EDGE = 2,
    MDKR_CAMERA_SWEEP_FEATURE_VERTEX = 3,
} MdkrCameraSweepFeature;

typedef struct MdkrCameraLensNarrowHit {
    /* Signed triangle-to-solid-lens distance minus skin. */
    float clearance;
    float penetration_depth;
    /* Closest point on the candidate visual triangle and a lens-facing normal. */
    MdkrCameraVec3 point;
    MdkrCameraVec3 normal;
    MdkrCameraSweepFeature feature;
    uint8_t overlapping;
} MdkrCameraLensNarrowHit;

typedef struct MdkrCameraSweepHit {
    /* Earliest center-path fraction in [0, 1]. */
    float fraction;
    /* Signed point-to-triangle distance minus radius; negative means overlap. */
    float clearance;
    float penetration_depth;
    /* Contact point on the triangle and outward/depenetration normal. */
    MdkrCameraVec3 point;
    MdkrCameraVec3 normal;
    uint32_t kind;
    uint32_t stable_id;
    MdkrCameraSweepFeature feature;
    uint8_t started_overlapping;
} MdkrCameraSweepHit;

typedef enum MdkrCameraSweepStatus {
    MDKR_CAMERA_SWEEP_CLEAR = 0,
    MDKR_CAMERA_SWEEP_HIT = 1,
    /* Fail closed: callers should retain/recover a known-safe camera pose. */
    MDKR_CAMERA_SWEEP_INVALID = 2,
} MdkrCameraSweepStatus;

/* Caller-owned, per-call exact-kernel work census. All counters saturate rather
 * than wrap, and the profiled entry point canonicalizes it even on INVALID. */
typedef struct MdkrCameraRoundedLensSweepTelemetry {
    uint64_t triangles_seen;
    uint64_t triangles_filtered;
    uint64_t triangles_aabb_rejected;
    uint64_t triangles_narrowed;
    uint64_t analytic_swept_sat_tests;
    uint64_t analytic_revalidation_misses;
    uint64_t bounded_interval_tests;
    uint64_t bounded_interval_exhaustions;
    uint64_t stationary_tests;
    uint64_t conservative_advance_iterations;
    uint64_t contact_refinement_tests;
    uint64_t interval_fallbacks;
    uint64_t interval_samples;
    uint64_t ambiguous_intervals;
    uint64_t publication_revalidations;
} MdkrCameraRoundedLensSweepTelemetry;

/**
 * Calculate the conservative sphere radius for an eye-to-near-plane lens.
 * vertical_fov_radians is the effective (already capped) vertical FOV.
 */
int mdkr_camera_lens_guard_from_projection(
    float near_distance,
    float vertical_fov_radians,
    float aspect,
    float skin,
    MdkrCameraLensGuard *out_guard);

/*
 * Build an oriented rounded eye-to-near-plane guard from the effective
 * projection. up_hint need only be nonparallel to forward; the routine derives
 * a deterministic renderer-convention orthonormal basis (right x up ==
 * -forward).  This does not alter the legacy sphere guard API.
 */
int mdkr_camera_rounded_lens_guard_from_projection(
    float near_distance,
    float vertical_fov_radians,
    float aspect,
    float skin,
    MdkrCameraVec3 forward,
    MdkrCameraVec3 up_hint,
    MdkrCameraRoundedLensGuard *out_guard);

/*
 * Recompute the enclosing eye-centered sphere in double precision and round it
 * outward. Broad-phase adapters must use this value rather than trusting the
 * stored float radius, so they cannot cull a boundary contact before the exact
 * narrow phase sees it.
 */
int mdkr_camera_rounded_lens_guard_conservative_radius(
    const MdkrCameraRoundedLensGuard *guard,
    double *out_radius);

/*
 * Exact static narrow phase for one already-selected visual triangle.  The
 * returned HIT means the triangle intersects the rounded lens (the pyramid
 * solid expanded by skin); CLEAR publishes the positive closest clearance.
 * It neither allocates nor mutates inputs, and performs no broad-phase or
 * motion sweep.  A future query can use broadphase_radius to retain candidates
 * from the existing conservative swept-sphere pass, then call this at a pose.
 */
MdkrCameraSweepStatus mdkr_camera_rounded_lens_guard_triangle_test(
    const MdkrCameraRoundedLensGuard *guard,
    MdkrCameraVec3 eye,
    MdkrCameraVec3 a,
    MdkrCameraVec3 b,
    MdkrCameraVec3 c,
    MdkrCameraLensNarrowHit *out_hit);

/*
 * Sweep a fixed-orientation rounded lens from start_eye to desired_eye through
 * every selected world triangle. The sphere API remains independent. This
 * routine uses exact lens tests for contact decisions and never narrows only a
 * single sphere winner. Invalid geometry, numeric state, or exhausted
 * conservative-advancement budget fails closed with INVALID.
 */
MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit);

MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_profiled(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit,
    MdkrCameraRoundedLensSweepTelemetry *out_telemetry);

MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_profiled_limited(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit,
    MdkrCameraRoundedLensSweepTelemetry *out_telemetry,
    uint64_t stationary_test_limit);

/* Development oracle for validating the analytic swept-SAT path. It retains
 * the bounded conservative-advancement/sampled implementation and is not a
 * fixed-tick shipping entry point. */
MdkrCameraSweepStatus mdkr_camera_rounded_lens_sweep_reference(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit,
    MdkrCameraRoundedLensSweepTelemetry *out_telemetry);

/**
 * Sweep a conservative sphere through immutable indexed, two-sided triangles.
 * No memory is allocated and no input/world bytes are mutated.  Invalid finite
 * contracts (NaN/Inf, invalid indices, or degenerate triangles) return INVALID
 * rather than quietly omitting an occluder.
 */
MdkrCameraSweepStatus mdkr_camera_sweep(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_CAMERA_OBSTRUCTION_H */
