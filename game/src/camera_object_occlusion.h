/**
 * Native-only immutable visual occlusion cache for ObjectModel assets.
 *
 * This deliberately does not share the gameplay collision-object list or any
 * interaction state. A cache is built once from a finalized model and removed
 * before that model's allocation is freed; dynamic object enumeration belongs
 * to CAM-05's later world-list work.
 */
#ifndef CAMERA_OBJECT_OCCLUSION_H
#define CAMERA_OBJECT_OCCLUSION_H

#ifdef NATIVE_PORT

#include <stddef.h>
#include <stdint.h>

#include "camera_obstruction_transform.h"
#include "structs.h"

#define MDKR_CAMERA_OBJECT_OCCLUSION_HARD_MASK 1U
#define MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES 8U
/* threshold_basis: these are per-query aggregate work fences, calibrated from
 * measured demand rather than chosen for roundness. Instrumented 17,000-tick
 * adventure-postrace plus the 5,200/9,000/7,000/3,400/3,600-frame camera routes
 * were run with the fences lifted to 1024/4096/32768 so each query's true
 * demand could be recorded.
 *
 * Node visits: measured maximum 39 of 64 across every route. The transitioning
 * 166-triangle door that saturates the chunk/triangle fences compiles to
 * ceil(166/8) = 21 chunks and 2*21-1 = 41 BVH nodes, so 64 admits a complete
 * traversal of that model with 56% headroom. Unchanged.
 *
 * Retained chunks 16 -> 32 and retained chunk triangles 128 -> 256: on
 * adventure-postrace ticks 3465-3471 the transitioning door demanded up to 17
 * chunks / 134 triangles, exceeding the previous 16/128 fences, so the exact
 * kernel exhausted and the conservative world-AABB fallback published a pose
 * whose AABB contained the resolved eye (penetrated=1 on ticks 3466/3467). The
 * new fences cover that model's absolute worst case -- all 21 chunks / all 166
 * triangles retained -- with 52%/54% headroom, and 88%/91% headroom over the
 * measured 17/134. The two stay coupled at exactly
 * MAX_RETAINED_CHUNKS * CHUNK_TRIANGLES, as before, so a query can never be
 * triangle-fenced before it is chunk-fenced. No storage is sized by either
 * constant (the only fence-sized array is the MAX_QUERY_NODES node stack), so
 * the raise costs 0 bytes; it doubles only the worst-case exact triangle work
 * per query, which no measured route approaches.
 *
 * Stationary tests: measured maximum 9 of 128. Unchanged. */
#define MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES 64U
#define MDKR_CAMERA_OBJECT_OCCLUSION_MAX_RETAINED_CHUNKS 32U
#define MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_TRIANGLES 256U
#define MDKR_CAMERA_OBJECT_OCCLUSION_MAX_STATIONARY_TESTS 128U

typedef struct MdkrCameraObjectOcclusionTelemetry {
    size_t model_cache_count;
    size_t built_model_count;
    size_t rejected_model_count;
    size_t vertex_count;
    size_t hard_triangle_count;
    size_t chunk_count;
    size_t node_count;
    size_t nonblocking_triangle_count;
    size_t visual_no_collision_hard_triangle_count;
    size_t unknown_policy_triangle_count;
    size_t malformed_input_count;
    size_t degenerate_triangle_count;
    size_t bytes;
    size_t query_count;
    size_t missing_cache_query_count;
    size_t invalid_query_count;
} MdkrCameraObjectOcclusionTelemetry;

typedef struct MdkrCameraObjectOcclusionExactWork {
    size_t nodes_visited;
    size_t nodes_rejected;
    size_t chunks_retained;
    size_t triangles_retained;
    /* Distinguishes a healthy bounded-work fence from corrupt/invalid input. */
    uint8_t exhausted;
    MdkrCameraRoundedLensSweepTelemetry kernel;
} MdkrCameraObjectOcclusionExactWork;

typedef struct MdkrCameraObjectOcclusionExactLimits {
    size_t nodes;
    size_t chunks;
    size_t triangles;
    uint64_t stationary_tests;
} MdkrCameraObjectOcclusionExactLimits;

/* Called only after an ObjectModel's native visual arrays are finalized. */
void mdkr_camera_object_occlusion_model_loaded(ObjectModel *model);

/* Called before ObjectModel-owned data is torn down, including failed loads. */
void mdkr_camera_object_occlusion_model_pre_free(ObjectModel *model);

/* Immutable view; NULL means the model has no safely published visual cache. */
const MdkrCameraOcclusionWorld *mdkr_camera_object_occlusion_world_for_model(
    const ObjectModel *model,
    uint32_t *out_generation);

MdkrCameraSweepStatus mdkr_camera_object_occlusion_sweep_model(
    const ObjectModel *model,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit);

/* Allocation-free enclosing-sphere query through the same immutable index.
 * Generation mismatch, corrupt index state, or exhausted work returns INVALID. */
MdkrCameraSweepStatus mdkr_camera_object_occlusion_sweep_model_profiled(
    const ObjectModel *model,
    uint32_t expected_model_generation,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit,
    const MdkrCameraObjectOcclusionExactLimits *limits,
    MdkrCameraObjectOcclusionExactWork *out_work);

/* Allocation-free exact query through the immutable per-model chunk index.
 * Query work beyond the public chunk/triangle fences returns INVALID so the
 * two-phase caller retains its conservative enclosing-sphere hit. */
MdkrCameraSweepStatus mdkr_camera_object_occlusion_rounded_lens_sweep_model_profiled(
    const ObjectModel *model,
    uint32_t expected_model_generation,
    const MdkrCameraObjectTransform *object_transform,
    const MdkrCameraRoundedLensSweepInput *world_input,
    MdkrCameraSweepHit *out_world_hit,
    const MdkrCameraObjectOcclusionExactLimits *limits,
    MdkrCameraObjectOcclusionExactWork *out_work);

void mdkr_camera_object_occlusion_get_telemetry(
    MdkrCameraObjectOcclusionTelemetry *out_telemetry);

#endif /* NATIVE_PORT */

#endif /* CAMERA_OBJECT_OCCLUSION_H */
