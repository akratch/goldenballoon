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
    /*
     * AUTHORED RATE. Set when the batch is driven by a texscroll object, which
     * publishes its rate directly instead of leaving presentation to recover
     * it by differencing. `rate_*` is the tick's advance and `phase_*` the
     * accumulator residue the authored bytes already owe, both in QUARTER UV
     * units; the interpolated displacement is (phase + rate * alpha) / 4.
     *
     * An authored record needs no second observation, so it does not go
     * through the confirm-or-hold rule below. That rule exists to stop a
     * mis-resolved texture WRAP reaching the screen as a reverse smear, and a
     * rate that was never measured cannot be a mis-resolved wrap — nothing
     * about it is inferred from the bytes. Measured records are unaffected and
     * still require the previous tick to agree.
     */
    bool authored;
    int32_t rate_u;
    int32_t rate_v;
    int32_t phase_u;
    int32_t phase_v;
} GfxPresentationUvScroll;

typedef struct GfxPresentationPacketBinding {
    GfxPresentationMatrixOwner owner;
    int viewport;
    uint32_t ordinal;
    uint8_t key_bytes[GFX_PRESENTATION_PACKET_MAX_KEY_BYTES];
    size_t key_size;
    bool stale;
} GfxPresentationPacketBinding;

/*
 * Why a per-surface-class census row blended or snapped, for the
 * [SMOOTH-VERDICT] report. MDKR_VERDICT_BLEND is itself a reason (the
 * row's own success case), not just "not a failure" -- see
 * gfx_presentation_packet_note_verdict.
 */
typedef enum MdkrVerdictReason {
    MDKR_VERDICT_BLEND = 0,
    MDKR_VERDICT_NO_OWNER,
    MDKR_VERDICT_TICK_MISMATCH,
    MDKR_VERDICT_GENERATION_MISMATCH,
    MDKR_VERDICT_TOPOLOGY_MISMATCH,
    MDKR_VERDICT_DISCONTINUITY,
    MDKR_VERDICT_CAMERA_CUT,
    MDKR_VERDICT_UV_HOLD,
    MDKR_VERDICT_DEPENDENCY_MISS,
    MDKR_VERDICT_PAN_RATE_DEMOTED,
    MDKR_VERDICT_REASON_COUNT,
} MdkrVerdictReason;

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
    /*
     * Renderer-owned world content (weather): batches whose identity token is
     * a fixed renderer-owned slot rather than a spawned Object.
     *
     * `registrations` is the non-vacuity witness -- it says the content was
     * given a presentation identity at all, which is the whole defect class
     * these three count. `hits`/`overrides` say the replay actually resolved a
     * pair for it and that the pair MOVED, i.e. the substitution changed
     * pixels rather than re-drawing tick T. `jump_holds` counts the guard in
     * GfxPresentationMatrixOwner::max_vertex_delta refusing a pair whose
     * displacement is a slot reuse or a volume wrap, not a movement.
     */
    uint64_t renderer_vertex_registrations;
    uint64_t renderer_vertex_hits;
    uint64_t renderer_vertex_overrides;
    uint64_t renderer_vertex_jump_holds;
    uint64_t projected_shadow_deformation_hits;
    uint64_t projected_shadow_deformation_overrides;
    uint64_t particle_color_hits;
    uint64_t particle_color_overrides;
    uint64_t primitive_alpha_hits;
    uint64_t primitive_alpha_overrides;
    /* Magnitude of the substitutions, in 0-255 alpha steps. A stage can
     * override every draw it touches and still be invisible if every step is
     * one byte on a surface whose blend cannot resolve one byte, and an
     * override count says nothing about which of those it is doing. */
    uint64_t primitive_alpha_delta_sum;
    uint64_t primitive_alpha_delta_peak;
    uint64_t projected_shadow_primitive_alpha_hits;
    uint64_t projected_shadow_primitive_alpha_overrides;
    uint64_t particle_primitive_alpha_hits;
    uint64_t particle_primitive_alpha_overrides;
    /* Tick agreement between a replayed recipe and the alpha-zero pose its
     * residual is measured against, counted over EVERY class that carries a
     * residual (root, child, billboard, effect). Mismatches are zero by
     * construction on a correct tree and were 708 of 708 effect overrides on
     * the build that shipped in v1.0.1-v1.0.3. */
    uint64_t owner_tick_checks;
    uint64_t owner_tick_mismatches;
    uint64_t owner_tick_suppressed;
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
    /* Why a known scroller held, split by the confirmation clause that
     * refused it. One aggregate hold count cannot distinguish a batch the
     * census never saw at T-1 (a scroller that just came into view, and will
     * confirm on its next tick) from one whose two published ticks disagree
     * about the displacement (a wrap the resolver could not undo). The first
     * is bounded by how often geometry enters the frustum; the second is not,
     * and only the second is a candidate defect. */
    uint64_t uv_scroll_hold_unpublished;
    uint64_t uv_scroll_hold_ambiguous;
    uint64_t uv_scroll_hold_shape;
    uint64_t uv_scroll_hold_phase;
    /* Confirmations that stood on ONE tick's record: the batch's own
     * cross-triangle agreement was the corroboration (>= 2 triangles whose
     * corners all state one fold-resolved displacement), taken where the
     * two-tick rule would otherwise hold a scroller that just came into
     * view or whose authored rate genuinely varies per tick (eased flows,
     * camera-coupled sheets). Field evidence 2026-08-17: the two-tick rule
     * held 46% of hub WORLD_SCROLL verdicts, every one a surface stepping
     * at 30 Hz against a gliding camera. */
    uint64_t uv_scroll_solo_accepts;
    /* Registrations and confirmations that carried an AUTHORED rate rather
     * than a measured difference. A gate that drives a scroller whose rate is
     * not a whole number of units per tick reads these to prove the authored
     * path is the one it is measuring: with the rate absent, that content
     * cannot confirm at all and holds on every present. */
    uint64_t uv_scroll_authored_registrations;
    uint64_t uv_scroll_authored_confirmations;
    uint64_t uv_scroll_overrides;
    size_t uv_scroll_peak;
    size_t uv_scroll_bytes_peak;
    /*
     * [SMOOTH-VERDICT] per-surface-class census. `verdict_blend[c]` and
     * `verdict_snap[c]` are the row's blend/snap totals for class c
     * (blend + snap == every graded draw of that class); `verdict_reason[c]`
     * is a full breakdown by MdkrVerdictReason, including
     * MDKR_VERDICT_BLEND itself, so "top_reason" for a class is just the
     * argmax over its own row. This translates outcomes clause sites have
     * already decided (uv_scroll_hold_*, the MDKR_REPLAY_HOLD_* classes) --
     * it does not re-run any confirmation or hold logic itself.
     */
    uint32_t verdict_blend[MDKR_SURF_CLASS_COUNT];
    uint32_t verdict_snap[MDKR_SURF_CLASS_COUNT];
    uint32_t verdict_reason[MDKR_SURF_CLASS_COUNT][MDKR_VERDICT_REASON_COUNT];
    /*
     * Topology-key pair comparisons (Task 7). `topology_checks` counts every
     * time a topology-keyed owner asked the published pair whether its two
     * ticks describe the same mesh; `topology_mismatches` counts the answers
     * that were no.
     *
     * The pair is the point. A gate that only asserted mismatches==0 would
     * pass just as well on a build where the question is never asked at all,
     * which is precisely how a topology guard would rot: the wave surfaces
     * change LOD variant only when the camera crosses a distance boundary, so
     * a route can legitimately produce zero mismatches, and only checks>0
     * separates that from a guard that stopped running.
     */
    uint64_t topology_checks;
    uint64_t topology_mismatches;
    /*
     * Task 9: X/Z nearest-slot reorder checks against a regenerated
     * projected-shadow decal's own published pair (see
     * gfx_presentation_packet_deformation_reordered). Kept SEPARATE from
     * topology_checks/topology_mismatches above rather than folding into
     * them: shadows are never topology-keyed (they have no discrete variant
     * a game-side note could name the way a wave tile's LOD grid can), so a
     * route with wave content would otherwise make `shadow_reorder_checks
     * >0` ambiguous between "the shadow guard ran" and "an unrelated wave
     * tile changed variant" -- a distinction the fixture needs.
     */
    uint64_t shadow_reorder_checks;
    uint64_t shadow_reorder_mismatches;
    /*
     * VOID CURTAIN replay census. The curtain (tracks.c void walker) is the
     * one world batch family that TELEPORTS every authored tick: its quads
     * are rebuilt on a line re-anchored to the tick camera (250 units ahead,
     * perpendicular to yaw), so an uninterpolated curtain steps at 30 Hz
     * against a 120 Hz gliding world -- visible exactly as the falls
     * "breaking" during pans and invisible when static. These three say, per
     * replayed curtain batch, whether the deformation stage actually blended
     * it or left the teleport on screen. Batches are recognised by vertex
     * pointer against the registered per-viewport double buffers.
     */
    uint64_t void_replay_batches;
    uint64_t void_replay_interpolated;
    uint64_t void_replay_held;
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
 * Task 9: does a published deformation pair describe the SAME vertex at the
 * SAME index across both ticks, judged by X/Z position alone (Y is the
 * receiver's contact height, not identity)?
 *
 * `gfx_presentation_packet_lookup_deformation` proves the pair by COUNT and
 * BYTE SIZE agreeing -- it cannot see whether index i still names the same
 * authored point it named last tick. A regenerated decal (projected shadow,
 * or any other per-tick-rebuilt mesh sharing this recipe) can hold both
 * counts steady while the walk that built it started from a different
 * triangle, so what used to be vertex 0 is now vertex 3 and vice versa: same
 * topology, reordered authoring. Blending previous[i] with current[i] under
 * that condition mixes two unrelated points and can pull the decal's shape
 * far past any single vertex's real per-tick displacement -- the "explosion"
 * a per-vertex magnitude guard (owner->max_vertex_delta) does not catch,
 * because each individual index's delta can still be small.
 *
 * The test: for every current-tick vertex, is its own previous-tick vertex
 * (index i) at least as close in X/Z as every OTHER previous-tick vertex? If
 * some other index is strictly closer, the index-for-index correspondence
 * the caller is about to blend is not the nearest-slot pairing, i.e. not the
 * identity permutation, and the pair should snap rather than blend.
 *
 * `x_offset`/`z_offset` are byte offsets of two `int16_t` fields within one
 * stride-sized element (Vertex::x, Vertex::z on the DKR side); this file is
 * deliberately game-struct-agnostic, so the caller who knows the layout
 * supplies it. O(count^2) with count bounded by DKR_MAX_VERTICES (32), i.e.
 * at most 1024 comparisons per call.
 */
bool gfx_presentation_packet_deformation_reordered(
    const GfxPresentationDeformationBinding *binding, size_t x_offset,
    size_t z_offset);
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
/* Cumulative hold-clause counters as of THIS call, so a caller of
 * gfx_presentation_packet_lookup_uv_scroll can diff before/after and learn
 * which clause (if any) refused a batch without gfx_pc_dkr.c re-deriving the
 * confirm-or-hold rule itself. Any argument may be NULL. */
void gfx_presentation_packet_get_uv_scroll_hold_stats(
    uint64_t *unpublished, uint64_t *ambiguous, uint64_t *shape,
    uint64_t *phase);
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

/*
 * Void-curtain batch recognition (see the census comment on the stats
 * struct). The walker registers each viewport's two vertex double buffers
 * at level alloc; the replay asks whether a batch's vertex pointer lies in
 * any registered range and reports the blend outcome. reset() is called at
 * the same alloc so ranges from a freed level arena can never false-match.
 */
void gfx_presentation_packet_void_ranges_reset(void);
/* dl_address is the 32-bit vertex-load word (OS_K0_TO_PHYSICAL of the live
 * buffer), not a host pointer — the retained replay resolves into an arena
 * copy, so only the DL word is a shared identity across both walks. */
void gfx_presentation_packet_void_range_note(uint64_t dl_address,
                                             size_t bytes);
int gfx_presentation_packet_void_range_contains(uint64_t dl_address);
void gfx_presentation_packet_note_void_replay(int interpolated);
void gfx_presentation_packet_note_phase_hold(bool effect);
void gfx_presentation_packet_note_deformation_override(void);
void gfx_presentation_packet_note_particle_deformation(bool overridden);
/* One renderer-owned vertex batch resolved a pair; `overridden` says the two
 * endpoints differed, so the substitution moved something. */
void gfx_presentation_packet_note_renderer_vertex(bool overridden);
/* The displacement guard refused a structurally valid pair. */
void gfx_presentation_packet_note_renderer_vertex_jump(void);
void gfx_presentation_packet_note_projected_shadow_deformation(
    bool overridden);
void gfx_presentation_packet_note_deformation_color(bool particle,
                                                    bool overridden);
/* `authored` is the display list's own G_SETPRIMCOLOR alpha, `applied` what
 * the replay substituted for it. Passing both rather than a bool lets the
 * census bound how far the substitution actually moved the byte. */
void gfx_presentation_packet_note_primitive_alpha(bool particle,
                                                  uint8_t authored,
                                                  uint8_t applied);
void gfx_presentation_packet_note_projected_shadow_primitive_alpha(
    bool overridden);
void gfx_presentation_packet_note_effect_override(void);
/* One replayed recipe's residual: did the recipe's capture tick match the tick
 * the alpha-zero pose came from? Counting rather than refusing is deliberate.
 * A refusal here would be a behaviour change wearing an assertion's clothes,
 * and the equality is structural after the anchoring fix -- the counter exists
 * so a future regression is caught by a gate rather than by a player. */
void gfx_presentation_packet_note_owner_tick(bool agreed);
void gfx_presentation_packet_note_owner_tick_suppressed(void);
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

/*
 * Drop registrations belonging to a display list that is about to be authored
 * over. The live side is normally emptied by the freeze at the end of that
 * list's real walk, so on a pass that submits its list this is a no-op. On a
 * pass that does NOT submit -- a frame-timer hold or an elided catch-up render
 * -- the game re-authors the same buffer, and without this the previous
 * attempt's bindings survive into the next freeze carrying the previous tick's
 * recipe. Replay would then measure that recipe's residual against a newer
 * alpha-zero pose, which is the same off-by-one-tick anchoring error the
 * effect stage shipped with (docs/evidence/smoothing-artifact-repro-2026-08.md
 * §2.3), reached by a different route.
 */
void gfx_presentation_packet_discard_live_registrations(void);
void gfx_presentation_packet_invalidate(void);
void gfx_presentation_packet_get_stats(GfxPresentationPacketStats *out);
/*
 * Record one graded [SMOOTH-VERDICT] outcome: `surface_class` blended when
 * `reason == MDKR_VERDICT_BLEND` and snapped (held its captured pose) for
 * every other reason. Out-of-range arguments are ignored rather than
 * clamped, so a caller passing a class/reason this build does not know about
 * cannot silently corrupt an adjacent row.
 */
/* One topology-key pair comparison and its outcome (see topology_checks). */
void gfx_presentation_packet_note_topology(bool agree);
/* One X/Z nearest-slot reorder check against a shadow's own published pair
 * and its outcome (see shadow_reorder_checks). */
void gfx_presentation_packet_note_shadow_reorder(bool reordered);
void gfx_presentation_packet_note_verdict(MdkrSurfaceClass surface_class,
                                          MdkrVerdictReason reason);
/* Zero only the verdict census (verdict_blend/verdict_snap/verdict_reason),
 * called after [SMOOTH-VERDICT] emits its rows so a later flush in the same
 * process starts a fresh aggregation window. Every other stat in
 * GfxPresentationPacketStats stays cumulative for the process lifetime. */
void gfx_presentation_packet_reset_verdict_stats(void);
void gfx_presentation_packet_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GFX_PRESENTATION_PACKET_H */
