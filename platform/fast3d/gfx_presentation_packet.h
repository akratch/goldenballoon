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
