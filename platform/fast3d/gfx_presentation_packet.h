/*
 * Immutable retained dependencies for presentation replay.
 *
 * The display list itself contains bare pointers to transient matrices and
 * vertices. Object-root matrices are already copied by gfx_shadow_frame, but
 * billboard-local matrices and anchor vertices intentionally do not belong to
 * the shadow/world registry. Animated model and direct world-space particle
 * vertices additionally change every authored tick. This packet retains
 * billboard ownership/key bytes through the last present and keeps a separate
 * bounded, tick-stamped previous/current copy of compatible vertex batches.
 */
#ifndef GFX_PRESENTATION_PACKET_H
#define GFX_PRESENTATION_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx_shadow_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_PRESENTATION_PACKET_MAX_KEY_BYTES 64u
#define GFX_PRESENTATION_DEFORM_MAX_BYTES 512u

/* One gSPPolygon batch is at most 16 triangles (the G_TRIN count field is four
 * bits, biased by one), so a per-triangle "this corner set moved" selector fits
 * in a uint16_t and the whole UV-scroll record stays 32 bytes. */
#define GFX_PRESENTATION_UV_SCROLL_MAX_TRIANGLES 16u
#define GFX_PRESENTATION_UV_SCROLL_MAX_SITES 4096u

/*
 * One authored tick's UV-scroll displacement for a single triangle batch.
 *
 * DKR does not scroll with G_SETTILESIZE, gSPTexture or a texture matrix. Every
 * authored scroller — the BHV_TEXTURE_SCROLL waterfall/river/lava driver, the
 * BHV_ANIMATOR single-batch driver, the generated wave surfaces, and the
 * rain/particle/skydome sheets — rewrites the S10.5 corner UVs inside a
 * Triangle array in place, once per authored tick, and the display list carries
 * only a POINTER to that array. So the retained task's copy of those bytes is a
 * {T} endpoint, and the already-authored next task names the same array — or,
 * for a game-declared ping-pong surface, its partner — holding {T+1}.
 *
 * `du`/`dv` are that tick's displacement in S10.5 texel units (32 == one
 * texel), already wrap-resolved: see gfx_pc_dkr.c's capture for the wrap rule
 * and the moduli each driver uses.
 */
typedef struct GfxPresentationUvScroll {
    int32_t du;
    int32_t dv;
    uint32_t triangle_count;
    uint16_t moved_u;   /* bit i: triangle i's U advanced by du this tick */
    uint16_t moved_v;
} GfxPresentationUvScroll;

typedef struct GfxPresentationPacketBinding {
    GfxPresentationMatrixOwner owner;
    int viewport;
    uint32_t ordinal;
    uint8_t key_bytes[GFX_PRESENTATION_PACKET_MAX_KEY_BYTES];
    size_t key_size;
    bool stale;
} GfxPresentationPacketBinding;

typedef struct GfxPresentationPacketStats {
    uint64_t matrix_registrations;
    uint64_t vertex_registrations;
    uint64_t particle_vertex_registrations;
    uint64_t projected_shadow_vertex_registrations;
    uint64_t freezes;
    uint64_t freeze_failures;
    uint64_t matrix_hits;
    uint64_t matrix_misses;
    uint64_t vertex_hits;
    uint64_t vertex_misses;
    uint64_t stale_keys;
    uint64_t stale_matrix_holds;
    uint64_t stale_vertex_holds;
    uint64_t unsafe_stale_fallbacks;
    uint64_t forced_matrix_rewrites;
    uint64_t forced_vertex_rewrites;
    uint64_t dependency_endpoint_checks;
    uint64_t dependency_endpoint_mismatches;
    uint64_t dependency_expected_hash;
    uint64_t dependency_actual_hash;
    size_t frozen_matrices;
    size_t frozen_vertices;
    size_t matrix_peak;
    size_t vertex_peak;
    uint64_t deformation_registrations;
    uint64_t deformation_hits;
    uint64_t deformation_holds;
    uint64_t deformation_phase_holds;
    uint64_t deformation_overrides;
    uint64_t deformation_misses;
    uint64_t deformation_incompatible;
    uint64_t deformation_collisions;
    uint64_t deformation_color_hits;
    uint64_t deformation_color_overrides;
    uint64_t particle_deformation_hits;
    uint64_t particle_deformation_overrides;
    uint64_t projected_shadow_deformation_hits;
    uint64_t projected_shadow_deformation_overrides;
    uint64_t particle_color_hits;
    uint64_t particle_color_overrides;
    uint64_t primitive_alpha_hits;
    uint64_t primitive_alpha_overrides;
    uint64_t projected_shadow_primitive_alpha_hits;
    uint64_t projected_shadow_primitive_alpha_overrides;
    uint64_t particle_primitive_alpha_hits;
    uint64_t particle_primitive_alpha_overrides;
    uint64_t effect_registrations;
    uint64_t effect_hits;
    uint64_t effect_overrides;
    uint64_t effect_phase_holds;
    uint64_t effect_misses;
    uint64_t effect_collisions;
    uint64_t future_captures;
    uint64_t future_failures;
    uint64_t endpoint_vertex_checks;
    uint64_t endpoint_vertex_mismatches;
    uint64_t endpoint_expected_hash;
    uint64_t endpoint_actual_hash;
    size_t deformation_peak;
    uint64_t uv_scroll_registrations;
    uint64_t uv_scroll_confirmations;
    uint64_t uv_scroll_rejects;
    uint64_t uv_scroll_collisions;
    uint64_t uv_scroll_hits;
    uint64_t uv_scroll_holds;
    uint64_t uv_scroll_overrides;
    size_t uv_scroll_peak;
    size_t uv_scroll_bytes_peak;
} GfxPresentationPacketStats;

typedef struct GfxPresentationDeformationBinding {
    GfxPresentationMatrixOwner owner;
    int viewport;
    uint32_t ordinal;
    uint32_t count;
    uint32_t stride;
    size_t byte_size;
    const uint8_t *previous_bytes;
    const uint8_t *current_bytes;
} GfxPresentationDeformationBinding;

bool gfx_presentation_packet_register_matrix(
    const void *key, size_t key_size, int viewport,
    const GfxPresentationMatrixOwner *owner);
bool gfx_presentation_packet_register_vertex(
    const void *key, size_t key_size, int viewport,
    const GfxPresentationMatrixOwner *owner);
/* Particle model buffers outlive the present subloop, so identity is enough to
 * find their retained owner. Exact vertex bytes are still copied separately by
 * capture_deformation; replay never reads a past tick from this pointer. */
bool gfx_presentation_packet_register_vertex_identity(
    const void *key, int viewport,
    const GfxPresentationMatrixOwner *owner);
/* Terrain-projected shadow meshes are direct world-space vertex batches whose
 * heap addresses can alternate between authored ticks. `key` is still the live
 * address, because that is what the walk has in hand and it is unambiguous
 * WITHIN a tick; `ordinal` is stamped onto the registered entry so the
 * deformation binding below can be keyed by (owner, viewport, ordinal), which
 * is the part that survives the address changing. */
bool gfx_presentation_packet_register_projected_shadow_vertex(
    const void *key, int viewport, uint32_t ordinal,
    const GfxPresentationMatrixOwner *owner);
/* Replace a build-time dependency image with the exact bytes consumed by the
 * real display-list walk. Arena addresses may be rewritten after registration;
 * replay must retain what the backend actually saw, not that earlier tenant. */
bool gfx_presentation_packet_note_walked_matrix(
    const void *key, const void *bytes, size_t size);
bool gfx_presentation_packet_note_walked_vertex(
    const void *key, const void *bytes, size_t size);
void gfx_presentation_packet_note_stale_hold(bool matrix, bool safe);
void gfx_presentation_packet_note_dependency_endpoint(
    const void *expected, const void *actual, size_t size);

/* Begins HLE capture for the real display list of one authoritative tick.
 * Billboard registrations happen earlier during list build and are separate. */
void gfx_presentation_packet_capture_begin(uint64_t tick);
/* Discard a partially scanned task without disturbing the last complete
 * previous/current publication. */
void gfx_presentation_packet_capture_abort(void);
bool gfx_presentation_packet_capture_deformation(
    const GfxPresentationMatrixOwner *owner, int viewport, uint32_t ordinal,
    const void *bytes, size_t byte_size, uint32_t count, uint32_t stride);
bool gfx_presentation_packet_lookup_deformation(
    const GfxPresentationMatrixOwner *owner, int viewport, uint32_t ordinal,
    uint64_t current_tick, uint32_t count, uint32_t stride,
    GfxPresentationDeformationBinding *out);
/* Resolve the exact retained bytes authored for this task token. This is
 * the safe replay fallback when the packet cannot prove a previous/current
 * interpolation pair: callers hold these bytes instead of reading the arena
 * pointer after the next game tick may have reused it. */
bool gfx_presentation_packet_lookup_deformation_hold(
    const GfxPresentationMatrixOwner *owner, int viewport, uint32_t ordinal,
    uint64_t authored_tick, uint32_t count, uint32_t stride,
    GfxPresentationDeformationBinding *out);
/*
 * Stage one triangle batch's {T -> T+1} UV displacement, keyed by the batch's
 * ORIGINAL address. Level triangle arrays are level-lifetime allocations, so
 * the address plus the triangle count plus the batch's own topology (checked by
 * the caller) is a stable identity within a stage; gfx_presentation_packet_
 * invalidate() clears the table at every stage boundary.
 *
 * Two batches cannot share an address, so a repeat observation of the same key
 * within one capture is a collision: the key is poisoned rather than letting
 * the later batch decide the earlier one's phase.
 */
bool gfx_presentation_packet_capture_uv_scroll(
    const void *key, const GfxPresentationUvScroll *scroll);
/*
 * Resolve the displacement to interpolate for one batch at `target_tick`.
 *
 * Succeeds only when the published table is exactly that tick AND the previous
 * published tick agreed on the same displacement for the same key and count.
 * Authored scroll speed is constant for a level, so a genuine scroller confirms
 * on its second tick and stays confirmed; a one-off misread — the sole residual
 * risk in the wrap rule — cannot reach the screen, it holds for that tick.
 */
bool gfx_presentation_packet_lookup_uv_scroll(
    const void *key, uint64_t target_tick, uint32_t triangle_count,
    GfxPresentationUvScroll *out);
/*
 * Publish the staged UV-scroll table under its own tick stamp.
 *
 * This is deliberately NOT part of publish_deformation. Deformation streams are
 * staged by both the authoritative walk (task T) and the forward census (task
 * T+1), so their previous/current pair alternates correctly. UV-scroll
 * displacements are a difference between two tasks and only the census can
 * compute one, so riding that rotation would interleave every census table with
 * an empty one from the real walk and nothing would ever confirm.
 *
 * A zero tick discards the staged table without publishing.
 */
void gfx_presentation_packet_publish_uv_scroll(uint64_t tick);
void gfx_presentation_packet_note_uv_scroll_override(void);

/* Publish only the staged deformation/effect stream. Used by the read-only
 * census of the already-authored next task; matrix/vertex owner bindings for
 * the task being replayed remain frozen and untouched. */
bool gfx_presentation_packet_publish_deformation(void);
void gfx_presentation_packet_note_future_capture(bool success);
void gfx_presentation_packet_note_deformation_incompatible(void);
void gfx_presentation_packet_note_phase_hold(bool effect);
void gfx_presentation_packet_note_deformation_override(void);
void gfx_presentation_packet_note_particle_deformation(bool overridden);
void gfx_presentation_packet_note_projected_shadow_deformation(
    bool overridden);
void gfx_presentation_packet_note_deformation_color(bool particle,
                                                    bool overridden);
void gfx_presentation_packet_note_primitive_alpha(bool particle,
                                                  bool overridden);
void gfx_presentation_packet_note_projected_shadow_primitive_alpha(
    bool overridden);
void gfx_presentation_packet_note_effect_override(void);
void gfx_presentation_packet_note_endpoint_semantic(
    const void *expected, const void *actual, size_t size);

/* Atomic live -> frozen publication. The live side is reset whether the copy
 * succeeds or fails; a partial packet is never made visible. */
void gfx_presentation_packet_freeze(void);
bool gfx_presentation_packet_frozen(void);

bool gfx_presentation_packet_lookup_matrix(
    const void *key, GfxPresentationPacketBinding *out);
bool gfx_presentation_packet_lookup_vertex(
    const void *key, GfxPresentationPacketBinding *out);
/* Replay can read bytes from a private retained arena while ownership stays
 * keyed to the address used by the real walk. These forms keep identity and
 * observed bytes explicit so stale-tenancy checks never reread live game
 * memory. */
bool gfx_presentation_packet_lookup_matrix_observed(
    const void *key, const void *observed,
    GfxPresentationPacketBinding *out);
bool gfx_presentation_packet_lookup_vertex_observed(
    const void *key, const void *observed,
    GfxPresentationPacketBinding *out);
bool gfx_presentation_packet_lookup_live_vertex(
    const void *key, GfxPresentationPacketBinding *out);
bool gfx_presentation_packet_has_live_vertex(const void *key);
bool gfx_presentation_packet_has_frozen_vertex(const void *key);

void gfx_presentation_packet_invalidate(void);
void gfx_presentation_packet_get_stats(GfxPresentationPacketStats *out);
void gfx_presentation_packet_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GFX_PRESENTATION_PACKET_H */
