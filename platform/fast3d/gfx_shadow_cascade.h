/*
 * gfx_shadow_cascade.h — backend-neutral stable directional-shadow planning.
 *
 * The frontend turns one immutable GfxShadowFrame into at most four map jobs.
 * Backends own textures/passes; this module owns the camera-correct split,
 * light-space fit, and texel-stable matrices consumed by GL and WebGPU.
 * The source-maintained Metal backend can adopt the same contract once its
 * legacy single-map path is replaced and runtime validation is available.
 */
#ifndef GFX_SHADOW_CASCADE_H
#define GFX_SHADOW_CASCADE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx_shadow_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_SHADOW_MAX_CASCADES 2
#define GFX_SHADOW_MAX_MAPS 4

typedef struct GfxShadowBudget {
    uint32_t resolution;
    uint8_t cascades_per_view;
    uint8_t map_count;
    uint16_t reserved;
    size_t depth_bytes;
} GfxShadowBudget;

typedef struct GfxShadowCascade {
    bool valid;
    uint8_t view_index;
    uint8_t cascade_index;
    uint8_t map_index;
    uint8_t reserved;
    float split_near;
    float split_far;
    float world_units_per_texel;
    float light_bounds_min[3];
    float light_bounds_max[3];
    /* Conventional column-vector matrix: clip = world_to_clip * world. */
    float world_to_clip[4][4];
} GfxShadowCascade;

typedef struct GfxShadowPlan {
    bool valid;
    uint64_t frame_generation;
    uint64_t stage_generation;
    GfxShadowBudget budget;
    size_t view_count;
    size_t cascade_count;
    GfxShadowCascade cascades[GFX_SHADOW_MAX_MAPS];
} GfxShadowPlan;

GfxShadowBudget gfx_shadow_budget_for_views(size_t view_count);

/*
 * sun_direction_world points from the surface toward the directional light.
 * The plan is all-or-nothing: invalid input clears out and returns false.
 */
bool gfx_shadow_build_plan(
    const GfxShadowFrame *frame,
    const float sun_direction_world[3],
    GfxShadowPlan *out);

#ifdef __cplusplus
}
#endif

#endif /* GFX_SHADOW_CASCADE_H */
