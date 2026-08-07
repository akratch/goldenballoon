/* Native-only published dynamic-object camera-occlusion list. */
#ifndef MDKR_CAMERA_DYNAMIC_OCCLUSION_H
#define MDKR_CAMERA_DYNAMIC_OCCLUSION_H

#ifdef NATIVE_PORT

#include <stddef.h>
#include <stdint.h>

#include "camera_obstruction_transform.h"

typedef struct Object Object;

#define MDKR_CAMERA_DYNAMIC_OCCLUSION_MAX_QUERY_INSTANCES 4U
/* Internal query selector outside the hard-triangle mask namespace. Static
 * sources still see the ordinary hard bit and therefore remain unchanged. */
#define MDKR_CAMERA_DYNAMIC_OCCLUSION_QUERY_CURRENT_POSE UINT32_C(0x80000000)

typedef struct MdkrCameraDynamicOcclusionTelemetry {
    size_t capacity;
    size_t allocation_bytes;
    size_t published_instance_count;
    size_t peak_instance_count;
    size_t identity_count;
    size_t model_bounds_count;
    size_t tick_count;
    size_t sweep_count;
    size_t broadphase_candidate_count;
    size_t narrowphase_candidate_count;
    size_t hard_solid_instance_count;
    size_t hard_door_instance_count;
    size_t transitioning_door_instance_count;
    size_t current_transitioning_door_count;
    size_t temporal_moved_instance_count;
    size_t temporal_proxy_hit_count;
    size_t excluded_racer_count;
    size_t excluded_particle_count;
    size_t excluded_non_model_count;
    size_t excluded_non_solid_count;
    size_t missing_cache_count;
    size_t missing_identity_count;
    size_t invalid_transform_count;
    size_t uncategorized_model_count;
    size_t capacity_failure_count;
    size_t invalid_sweep_count;
    size_t sphere_sweep_count;
    size_t sphere_hit_count;
    size_t sphere_broadphase_instance_count;
    size_t sphere_node_visited_count;
    size_t sphere_node_rejected_count;
    size_t sphere_chunk_retained_count;
    size_t sphere_chunk_triangle_count;
    size_t sphere_invalid_sweep_count;
    size_t sphere_conservative_fallback_count;
    size_t sphere_max_instances_per_sweep;
    size_t sphere_max_nodes_visited_per_sweep;
    size_t sphere_max_chunks_retained_per_sweep;
    size_t sphere_max_chunk_triangles_per_sweep;
    size_t exact_sweep_count;
    size_t exact_hit_count;
    size_t exact_broadphase_instance_count;
    size_t exact_model_triangle_count;
    size_t exact_node_visited_count;
    size_t exact_node_rejected_count;
    size_t exact_chunk_retained_count;
    size_t exact_chunk_triangle_count;
    size_t exact_triangle_aabb_rejected_count;
    size_t exact_triangle_narrowed_count;
    size_t exact_stationary_test_count;
    size_t exact_invalid_sweep_count;
    /* Exact counterpart of sphere_conservative_fallback_count: per-instance
     * recoveries where the bounded exact model kernel ran out of published
     * work budget and this source degraded to the conservative world AABB
     * instead of discarding the whole sweep. */
    size_t exact_conservative_fallback_count;
    size_t exact_max_instances_per_sweep;
    size_t exact_max_model_triangles_per_sweep;
    size_t exact_max_single_model_triangles;
    size_t exact_max_narrowed_triangles_per_sweep;
    size_t exact_max_stationary_tests_per_sweep;
    size_t exact_max_nodes_visited_per_sweep;
    size_t exact_max_chunks_retained_per_sweep;
    size_t exact_max_chunk_triangles_per_sweep;
} MdkrCameraDynamicOcclusionTelemetry;

/* Safe load-boundary lifecycle. `prepare` may allocate; tick and sweep never do. */
int mdkr_camera_dynamic_occlusion_prepare(size_t object_capacity);
void mdkr_camera_dynamic_occlusion_shutdown(void);
void mdkr_camera_dynamic_occlusion_reset(void);

/* Object-pool lifecycle hooks. Generation is never an Object* address alone. */
void mdkr_camera_dynamic_occlusion_note_spawn(const Object *object);
void mdkr_camera_dynamic_occlusion_note_free(const Object *object);

/*
 * Publish a snapshot after obj_update, before any presentation-only sort/draw.
 * It reads the authoritative Object list in list-index order and never calls
 * gameplay collision or changes Object state.
 */
void mdkr_camera_dynamic_occlusion_tick(void);

typedef struct MdkrCameraDynamicOcclusionHit {
    MdkrCameraSweepHit hit;
    uint64_t object_spawn_generation;
    uint32_t model_generation;
    /* Immutable model-cache triangle ID retained after hit.stable_id is
     * replaced by the global dynamic-instance namespace ID. */
    uint32_t source_triangle_stable_id;
    uint32_t authoritative_list_index;
} MdkrCameraDynamicOcclusionHit;

/* Side-effect-free query over the last successfully published fixed-tick list. */
MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_sweep_detailed(
    const MdkrCameraSweepInput *input,
    MdkrCameraDynamicOcclusionHit *out_hit);
MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_sweep(
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit);

/* Exact fixed-basis rounded-lens counterpart. Candidates are retained with
 * the core's recomputed conservative radius, then every retained immutable
 * object mesh is swept through the object-local exact rounded-lens kernel. */
MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_rounded_lens_sweep_detailed(
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraDynamicOcclusionHit *out_hit);
MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_rounded_lens_sweep(
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit);
void mdkr_camera_dynamic_occlusion_get_telemetry(
    MdkrCameraDynamicOcclusionTelemetry *out_telemetry);

/*
 * Published per-instance footprint, for callers that must tell a small prop
 * from a wall-sized solid. Side-effect free and valid only for the current
 * fixed tick's published list; returns 0 for an unknown ID, a failed
 * publication, or an instance published as a temporal (swept) proxy.
 */
typedef struct MdkrCameraDynamicOcclusionFootprint {
    /* Largest world-AABB half-extent of this instance's current pose. */
    float max_half_extent;
    MdkrCameraVec3 center;
    /* Value a caller passes as MdkrCameraSweepInput::ignored_object_generation
     * to exclude exactly this instance from both dynamic phases. */
    uint32_t object_generation;
} MdkrCameraDynamicOcclusionFootprint;

int mdkr_camera_dynamic_occlusion_instance_footprint(
    uint32_t stable_instance_id,
    MdkrCameraDynamicOcclusionFootprint *out_footprint);

/* Moving non-door hard solids render at the current fixed pose instead of
 * interpolating through an endpoint-only collision snapshot. Doors retain
 * interpolation because their conservative temporal proxy is queryable. */
int mdkr_camera_dynamic_occlusion_object_discontinuous(const Object *object);

/* A moving non-door solid is rendered at its current fixed pose instead of
 * being presentation-interpolated. Camera interpolation must likewise cut
 * when its swept lens could meet one of those objects. */
int mdkr_camera_dynamic_occlusion_discontinuous_sweep_candidate(
    const MdkrCameraSweepInput *input);

#endif /* NATIVE_PORT */

#endif /* MDKR_CAMERA_DYNAMIC_OCCLUSION_H */
