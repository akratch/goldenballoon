/*
 * gfx_shadow_frame.h — capture-once shadow input shared by the DKR frontend
 * and every renderer backend.
 *
 * DKR's render traversal mutates animation, RNG, and some gameplay state. A
 * shadow implementation must therefore consume geometry observed during the
 * ordinary display-list walk; it may never ask the game to render a second
 * time. The frontend commits one immutable frame, and the next backend
 * start_frame consumes it before the write side is reset.
 */
#ifndef GFX_SHADOW_FRAME_H
#define GFX_SHADOW_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_SHADOW_MAX_VIEWS 4
#define GFX_SHADOW_MATRIX_DIM 4

typedef enum GfxShadowAlphaMode {
    GFX_SHADOW_ALPHA_OPAQUE = 0,
    GFX_SHADOW_ALPHA_MASKED = 1,
} GfxShadowAlphaMode;

typedef enum GfxShadowMobility {
    GFX_SHADOW_MOBILITY_STATIC = 0,
    GFX_SHADOW_MOBILITY_DYNAMIC = 1,
} GfxShadowMobility;

typedef struct GfxShadowVertex {
    float position[3];
    float uv[2];
} GfxShadowVertex;

typedef struct GfxShadowMaterial {
    uint32_t texture_id;
    GfxShadowAlphaMode alpha_mode;
    bool two_sided;
} GfxShadowMaterial;

typedef struct GfxShadowRange {
    size_t first_vertex;
    size_t vertex_count;
    uint32_t texture_id;
    uint8_t view_index;
    uint8_t alpha_mode;
    uint8_t two_sided;
    uint8_t reserved;
} GfxShadowRange;

typedef struct GfxShadowView {
    bool valid;
    float view_projection[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM];
    float viewport[4];
    float bounds_min[3];
    float bounds_max[3];
    size_t triangle_count;
} GfxShadowView;

typedef struct GfxShadowFrame {
    uint64_t generation;
    uint64_t stage_generation;
    bool valid;
    bool failed;
    const GfxShadowVertex *static_vertices;
    size_t static_vertex_count;
    const GfxShadowRange *static_ranges;
    size_t static_range_count;
    GfxShadowVertex *vertices;
    size_t vertex_count;
    size_t vertex_capacity;
    GfxShadowRange *ranges;
    size_t range_count;
    size_t range_capacity;
    GfxShadowView views[GFX_SHADOW_MAX_VIEWS];
    size_t view_count;
} GfxShadowFrame;

typedef struct GfxShadowMatrixBinding {
    float world[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM];
    float view_projection[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM];
    GfxShadowMobility mobility;
    /*
     * Set by gfx_shadow_replay_restore when this entry's view_projection was
     * actually replaced with an interpolated camera. It tells the G_MTX handler
     * whether recomposing is WORTH it: for an unreplaced entry the display
     * list already holds the exact matrix, and recomposing could only lose
     * precision to it (see gfx_pc_dkr.c's G_MTX replay branch).
     */
    bool vp_overridden;
    /*
     * The view-projection AS CAPTURED, never overridden. The replay recomposes
     * with this first and compares the result against the display list's own
     * bytes: that comparison is what proves this particular entry's
     * (world, view_projection) decomposition is faithful, and therefore that
     * substituting a different camera into it is sound. Not every registration
     * site is exact — mtx_head_rotation builds its list matrix as
     * head x (parentWorld x VP) while it must register a world of
     * (head x parentWorld) — so the replay verifies rather than assumes.
     */
    float captured_view_projection[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM];
    /* The registration context camera.c published (see the entry struct in the
     * .c): which viewport emitted this matrix, and whether the GAMEPLAY
     * view-projection was in force when it did. Only gameplay entries are ever
     * given an interpolated camera. */
    bool gameplay_vp;
    int viewport;
    /*
     * WHICH camera.c site registered this entry (GFX_SHADOW_SITE_*).
     *
     * A reject can name its magnitude and its world without naming the code
     * that produced it, and that gap is exactly how the recomposition rejects
     * were once attributed to mtx_head_push by elimination rather than by
     * measurement. Tagging at the registration site is the only place that
     * knows, and it costs one int per entry.
     */
    int site;
    /* The Mtx bytes AT THE KEY as they stood at registration time. The registry
     * copies matrices by value but keys by pointer, so if these no longer match
     * the live bytes the address was rewritten by a later, non-registering
     * display-list command and the stored world no longer describes it. */
    int32_t key_bytes[16];
    /* Whether key_bytes was captured at all (see gfx_shadow_matrix_set_site).
     * A binding without it cannot be tenancy-checked and must not be trusted
     * by a consumer that needs to know the key still holds this matrix. */
    bool key_bytes_valid;
} GfxShadowMatrixBinding;

/* Registration sites, in camera.c order. */
#define GFX_SHADOW_SITE_UNKNOWN     0
#define GFX_SHADOW_SITE_PERSPECTIVE 1
#define GFX_SHADOW_SITE_WORLD_ORIGIN 2
#define GFX_SHADOW_SITE_SHEAR_PUSH  3
#define GFX_SHADOW_SITE_CAM_PUSH    4
#define GFX_SHADOW_SITE_HEAD_PUSH   5
#define GFX_SHADOW_SITE_SPRITE_PART 6

typedef struct GfxWorldFxStats {
    uint64_t frames_begun;
    uint64_t frames_committed;
    uint64_t frames_failed;
    uint64_t triangles_captured;
    uint64_t opaque_triangles;
    uint64_t masked_triangles;
    uint64_t static_cache_hits;
    uint64_t static_cache_misses;
    uint64_t matrix_registrations;
    uint64_t matrix_lookup_hits;
    uint64_t matrix_lookup_misses;
    uint64_t allocation_failures;
    /* Finite but not world-plausible vertices (|coord| beyond the stage
     * limit): rejected before they can poison the stage caster AABB. */
    uint64_t implausible_triangles;
    /* Triangle batches dropped by the DL-build-time caster exclusion seam. */
    uint64_t excluded_triangles;
    size_t current_views;
    size_t current_triangles;
    size_t current_static_triangles;
    size_t current_dynamic_triangles;
    size_t current_ranges;
    size_t peak_vertices;
    size_t peak_ranges;
    size_t matrix_entries;
    size_t matrix_peak;
} GfxWorldFxStats;

/*
 * Matrix bindings are registered while the game builds its one display list.
 * The key is the exact host Mtx pointer embedded in the matching G_MTX command.
 * Registration copies both matrices; no game pointer survives the call.
 */
bool gfx_shadow_matrix_register(
    const void *key,
    const float world[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM],
    const float view_projection[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM],
    GfxShadowMobility mobility);
bool gfx_shadow_matrix_lookup(
    const void *key,
    GfxShadowMatrixBinding *out);
void gfx_shadow_matrix_registry_reset(void);
void gfx_shadow_stage_begin(uint64_t stage_generation);

/*
 * ---- presentation replay (Phase 3 Wave B, design §2) ---------------------
 *
 * Registration happens only while the game BUILDS a display list, and
 * gfx_end_frame() resets the registry as soon as that list's one real walk has
 * consumed it. A second walk of the same list therefore finds nothing. These
 * entry points let the renderer take the copy at reset time and put it back
 * before a replay, optionally substituting an interpolated camera
 * view-projection for the gameplay camera's own entries only.
 */
typedef struct GfxShadowReplayViewProjection {
    bool valid;
    float view_projection[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM];
} GfxShadowReplayViewProjection;

/* Published by camera.c immediately before each registration: which viewport
 * emitted it, and whether the gameplay camera's VP was the one in force (see
 * the entry comment in the .c — gViewProjMatrixF is not one matrix a frame). */
void gfx_shadow_matrix_set_context(int viewport, bool gameplay_vp);
/* The site the NEXT gfx_shadow_matrix_register call is attributed to. */
void gfx_shadow_matrix_set_site(int site);

/* Copy the live registry + projected-range/excluded-caster marks. Fails whole:
 * a partial freeze would replay a partial scene. */
void gfx_shadow_replay_freeze(void);
bool gfx_shadow_replay_frozen(void);
size_t gfx_shadow_replay_frozen_matrix_count(void);
/* `overrides` is indexed by viewport; NULL replays the captured VP verbatim. */
bool gfx_shadow_replay_restore(
    const GfxShadowReplayViewProjection *overrides, size_t override_count);
/* Put back the live registry/marks the restore displaced. A replay runs after
 * the game has already registered the NEXT tick's matrices; throwing those away
 * would leave the following freeze empty. */
bool gfx_shadow_replay_release(void);
/* `failures` counts freezes that could not complete; `restore_failures` counts
 * restore/release unwinds (an allocation failure after the live registry had
 * already been displaced). Both must be zero on a healthy run. */
void gfx_shadow_replay_get_stats(
    uint64_t *freezes, uint64_t *restores, uint64_t *failures,
    uint64_t *restore_failures);
/* Caster capture must not run twice over one tick's display list. */
void gfx_shadow_capture_suppress(bool suppressed);

/*
 * Backend start_frame consumes gfx_shadow_frame_previous(). The frontend then
 * begins and commits the next frame around the ordinary display-list walk.
 */
void gfx_shadow_capture_begin(void);
int gfx_shadow_capture_view(
    const float viewport[4],
    const float view_projection[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM]);
bool gfx_shadow_capture_triangle(
    int view_index,
    const void *source_key,
    GfxShadowMobility mobility,
    const float positions[9],
    const float uv[6],
    const GfxShadowMaterial *material);
void gfx_shadow_capture_commit(void);
const GfxShadowFrame *gfx_shadow_frame_previous(void);
int gfx_shadow_previous_view_index(const float viewport[4]);
bool gfx_shadow_projected_range_mark(const void *source);
bool gfx_shadow_projected_range_contains(const void *source);
void gfx_shadow_projected_ranges_reset(void);

/*
 * Caster exclusion, marked at DL-build time on the exact triangle-batch
 * pointer (the same keying as the projected-range seam above) and consumed
 * during the HLE walk. Used for geometry that must never enter the shadow
 * map: the runtime void curtain (camera-relative, rewritten in place every
 * frame — the address-keyed static cache would freeze its first frames as a
 * permanent phantom caster), wave-generated water/lava surfaces, and level
 * batches the ROM authors flagged RENDER_NO_SHADOW. Reset once per frame
 * alongside the projected ranges.
 */
bool gfx_shadow_caster_exclude_mark(const void *source);
bool gfx_shadow_caster_excluded(const void *source);

bool gfx_world_fx_trace_enabled(void);
void gfx_world_fx_get_stats(GfxWorldFxStats *out);
void gfx_shadow_frame_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GFX_SHADOW_FRAME_H */
