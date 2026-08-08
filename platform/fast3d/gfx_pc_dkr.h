/**
 * gfx_pc_dkr.h — F3DDKR display-list interpreter (HLE) public API.
 *
 * This is the DKR-specific front-end of the fast3d renderer for mdkr64. It
 * decodes Diddy Kong Racing's custom F3DDKR microcode command stream and drives
 * the shared GfxRenderingAPI backends (OpenGL / Metal) that are reused verbatim
 * from mgb64. It is the structural sibling of mgb64's gfx_pc.h, but DKR's GBI is
 * a different command set (10-byte vertices with per-triangle UVs, G_TRIN
 * polygon batches, G_DMADL sub-lists, a 3-slot MVP matrix system and
 * billboarding), so the interpreter core is bespoke.
 *
 * Derived-from attribution (see platform/fast3d/PROVENANCE.md): texture decode,
 * color-combiner→shader translation, VBO packing and RDP state handling are
 * ported from Emill/n64-fast3d-engine via mgb64's gfx_pc.c. The F3DDKR command
 * semantics are from game/include/f3ddkr.h and the DKR decomp call sites.
 */
#ifndef GFX_PC_DKR_H
#define GFX_PC_DKR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h> /* Gfx, Mtx */
#include "gfx_font_registry.h"
#include "gfx_shadow_frame.h" /* GfxShadowReplayViewProjection */

#ifdef __cplusplus
extern "C" {
#endif

struct GfxRenderingAPI;

/* Mirrors mgb64's gfx_pc.h struct layout exactly so the vendored backends
 * (gfx_opengl.c / gfx_metal.mm), which include "../gfx_pc.h", can share this
 * declaration. If a platform/gfx_pc.h shim is needed for those TUs it should
 * simply re-declare these two symbols (or include this header). */
struct GfxDimensions {
    uint32_t width, height;
    float aspect_ratio;
};

extern struct GfxDimensions gfx_current_dimensions;
/*
 * OUTPUT resolution — the drawable/surface, BEFORE Video.RenderScale.
 * gfx_current_dimensions is the RENDER resolution (output x scale). Backends
 * configure their swapchain and size their readback target from this one; the
 * resolve is the single place a frame returns from render space to output
 * space.
 */
extern struct GfxDimensions gfx_output_dimensions;

/**
 * Initialize the interpreter against a rendering backend vtable. The OS-shim
 * agent passes &gfx_opengl_api (or the active backend). Calls rapi->init().
 */
bool gfx_init(struct GfxRenderingAPI *rapi);
/* Release frontend caches and the active backend before its window/surface. */
void gfx_shutdown(void);
/* Drop every frontend-held backend handle after device replacement. */
void gfx_reset_renderer_caches(void);
/* Initialize and publish a replacement backend after a live backend switch. */
bool gfx_rebind_renderer(struct GfxRenderingAPI *rapi);

/** Update the drawable dimensions (window/framebuffer pixels). Safe to call
 *  every frame; defaults to 320x240 until set. The backends read
 *  gfx_current_dimensions to size their scene targets. */
void gfx_set_dimensions(uint32_t width, uint32_t height);

/**
 * Publish the AUTHORED logical framebuffer the display list is composed in.
 *
 * This is the game's own surface, not the host window: 320x240 on NTSC/MPAL and
 * 320x264 on PAL, where video.c raises every mode by PAL_HEIGHT_DIFFERENCE and
 * the menus lay themselves out over the taller field. Viewport, scissor,
 * texrect and fill-rectangle coordinates are mapped onto the presentation
 * region against this size, so a PAL surface that is announced as 240 rows
 * scales its 2D art by 264/240 and clips the bottom of the screen. Defaults to
 * 320x240; video_init() publishes the real surface once the framebuffers exist.
 */
void gfx_dkr_set_logical_surface(uint32_t width, uint32_t height);

/**
 * Return the exact pixel extent of the completed frame available to capture.
 *
 * This is OUTPUT space, after supersample resolve. It may temporarily differ
 * from gfx_output_dimensions while a backend debounces a live resize, so frame
 * dump/readback callers must use this query rather than sampling SDL again.
 */
bool gfx_get_capture_dimensions(uint32_t *width, uint32_t *height);

/** Begin a frame transaction for one immutable game-authored task. False means
 * no display list may be submitted. authored_tick was latched when the game
 * began building task.data_ptr; it is never reconstructed from host time. */
bool gfx_start_frame(uint64_t authored_tick);

/** Interpret and render a complete F3DDKR display list. */
void gfx_run(Gfx *dl);

/** mgb64-compatible alias for gfx_run. */
void gfx_run_dl(Gfx *dl);

/** End a frame: flushes and presents (backend end_frame + finish_render). */
void gfx_end_frame(void);

/**
 * Emit the process-lifetime F3DDKR command census when MDKR_DL_CENSUS=1.
 *
 * The report is observational and safe before renderer initialization. It is
 * consumed by the all-content WebGPU gate to prove which opcodes and recursion
 * depths the corpus actually exercised, rather than treating a successful
 * frame run as implicit display-list coverage.
 */
void gfx_dkr_report_dl_census(void);

/** True once the active backend has entered an unrecoverable runtime state. */
bool gfx_renderer_failed(void);

/**
 * Drop every cached GPU texture whose source texels overlap [base, base+size).
 *
 * The texture cache is keyed on the SOURCE ADDRESS, so an entry outlives the
 * game freeing that memory: mempool then hands the same bytes to a different
 * asset and, if the new asset has the same dimensions and format, the lookup
 * HITS and binds the previous asset's image. The game's allocator therefore has
 * to tell the renderer when arena bytes are recycled — called from
 * mempool_slot_clear() (game/src/memory.c) under NATIVE_PORT.
 *
 * Set MDKR_TEXCACHE_VERIFY=1 to hash every bind's source and count hits whose
 * content changed since upload (gfx_dkr_texcache_stale_hits); it must stay 0.
 */
void gfx_dkr_texcache_invalidate_range(const void *base, uint32_t size);

/**
 * Mark a fully initialized level as the start of a renderer-resource
 * generation. When tracing is enabled, the previous generation is emitted with
 * texture/shader ownership deltas so repeated stage loads can prove a plateau.
 */
void gfx_dkr_resource_generation_begin(
    int32_t level, int32_t players, int32_t cutscene);

/** Retire prior-stage pointer tokens after its final graphics task is drained. */
void gfx_dkr_stage_resources_released(void);

/*
 * Declare a ping-pong pair of triangle buffers.
 *
 * Some authored geometry is double-buffered by the game itself: waves.c builds
 * the water/lava surface into gWaveTriangles[flip + ...] and flips `flip` every
 * authored tick, so the SAME surface is at one address on tick T and a different
 * one on tick T+1. Retained presentation replay keys UV-scroll endpoints by
 * address, and an address that alternates carries no identity across ticks.
 *
 * This is the game stating the correspondence rather than the renderer guessing
 * it: `base` is buffer 0, `stride` is the byte distance to the next buffer, and
 * `count` is how many buffers the allocation holds. Buffer 2i and 2i+1 are the
 * two phases of one surface. Registration is presentation-only -- nothing here
 * is read on the authoritative path, and an unregistered buffer simply keeps the
 * previous hold behaviour.
 */
void gfx_dkr_note_paired_triangle_buffers(const void *base, size_t stride,
                                          unsigned count);
void gfx_dkr_forget_paired_triangle_buffers(const void *base);

/*
 * Identify decoded texture payloads owned by ASSET_FONTS, with the glyph cells
 * that may be sampled from each atlas and which authored face they belong to.
 *
 * `face` is a GfxFontFace, passed as int so game code need not include the
 * registry header. Pure never enters any derivation path; Restored and
 * Remastered redraw the two plain faces from outline fonts, and Remastered
 * additionally derives SDF contours for the stylized ones.
 */
bool gfx_dkr_font_texture_register(
    const void *source, int face, const GfxFontAtlasRegion *regions,
    size_t region_count);
bool gfx_dkr_font_texture_unregister(const void *source);

/** Count of cache hits that returned a texture uploaded from different bytes.
 *  Only maintained when MDKR_TEXCACHE_VERIFY=1. */
extern uint32_t gfx_dkr_texcache_stale_hits;

/** Frontend-owned backend texture handles in the current renderer epoch. */
extern uint64_t gfx_dkr_texture_ids_created;
extern uint64_t gfx_dkr_texture_ids_deleted;
extern uint32_t gfx_dkr_texture_ids_live;
extern uint32_t gfx_dkr_texture_ids_high_water;
extern uint64_t gfx_dkr_shader_programs_created;

/**
 * Count of texture uploads whose source was a pre-swizzled ("interlaced") image
 * — a LOADBLOCK issued with dxt == 0, i.e. one of libultra's `...S` macros.
 * Those assets need their odd rows un-word-swapped before decode; see the block
 * comment above unswap_row() in gfx_pc_dkr.c. Always maintained (it is one
 * increment per cache miss) so a regression check can confirm the route it
 * drives actually reaches the path it is asserting on.
 */
extern uint32_t gfx_dkr_texload_line_swapped;

/** Runtime font-derivation telemetry used by the mode/backend/browser gates. */
extern uint32_t gfx_dkr_font_sdf_uploads;
/** Atlases redrawn from an embedded outline face (Restored and Remastered). */
extern uint32_t gfx_dkr_font_outline_uploads;
extern uint32_t gfx_dkr_font_registry_failures;

/**
 * Number of texture-cache fills that reached the backend with a complete mip
 * chain, and the sum of levels in those chains. The moving-camera mip gate uses
 * these counters to distinguish a real enabled arm from a visually plausible
 * single-level fallback.
 */
extern uint32_t gfx_dkr_mipmapped_uploads;
extern uint64_t gfx_dkr_mip_levels_uploaded;

/** RL-1 diagnostic telemetry. The default/production arm is always "baked". */
extern uint64_t gfx_dkr_rl1_triangles;
const char *gfx_dkr_rl1_active_arm_name(void);

/** Remastered smooth-normal lighting coverage and controlled-failure telemetry. */
extern uint64_t gfx_dkr_remaster_racer_triangles;
extern uint64_t gfx_dkr_remaster_character_triangles;
extern uint64_t gfx_dkr_remaster_missing_normal_batches;
/** Receiver draws that consumed a vertex with no derived world position
 *  (samples the shadow map at the origin). Must stay zero in production. */
extern uint64_t gfx_dkr_shadow_receiver_invalid_world;
const char *gfx_dkr_rl5_active_arm_name(void);

/**
 * Triangle counts observed at the final F3DDKR emission boundary. Both counters
 * require G_ZBUFFER + Z_CMP; `gfx_dkr_decal_triangles` additionally requires
 * ZMODE_DEC. The backend receives this same state immediately before the
 * buffered triangles are submitted, so a ROM-backed check can prove that the
 * shadow fix survived display-list decoding rather than merely inspecting the
 * source render-mode flags.
 */
extern uint64_t gfx_dkr_depth_compared_triangles;
extern uint64_t gfx_dkr_decal_triangles;

/* ---- presentation replay (Phase 3 Wave B, design §2) --------------------- */

/**
 * Re-walk the most recently walked display list to produce another image from
 * the tick that built it, with the frozen matrix registry restored and,
 * optionally, an interpolated camera view-projection substituted per viewport.
 * Returns false when nothing has been walked yet or the freeze is unavailable.
 *
 * This never re-enters game/src: the list is already built (design R3).
 */
bool gfx_dkr_replay_walk(
    const GfxShadowReplayViewProjection *overrides, size_t override_count);

/* Alpha-0 replay of the authored image with object substitution active — the
 * delayed-endpoint negative control's entry point. Endpoint walks are exempt
 * from the uncaptured-external refusal because their contract is byte-exact
 * reproduction of an image the authoritative walk already drew; declaring that
 * intent HERE, rather than inferring it from numerator == 0 inside the walk,
 * is what keeps residual obligation 2's ownership proof independent of the
 * pacer's ticket accounting. */
bool gfx_dkr_replay_walk_endpoint(
    const GfxShadowReplayViewProjection *overrides, size_t override_count);

/* The fixed-clock alpha additionally rebuilds generation-keyed object roots
 * from the immutable presentation snapshot pair. This is always a strictly
 * interior image and always eligible to refuse. */
bool gfx_dkr_replay_walk_interpolated(
    const GfxShadowReplayViewProjection *overrides, size_t override_count,
    uint64_t numerator, uint64_t denominator);

/* Read-only dependency census of the complete alternate-buffer task that
 * follows the task being replayed. Publishes true forward animated-vertex,
 * particle, fade, and effect data for {T,T+1}; it never submits a draw. */
bool gfx_dkr_capture_future_deformations(const Gfx *begin, const Gfx *end,
                                         uint64_t authored_tick);

/** True while a replay walk is in progress — the guard every "must not happen
 *  twice per tick" side effect in the walk is gated on. */
bool gfx_dkr_replay_pass_active(void);

/** The per-frame interpreter reset, without the backend start_frame or the
 *  caster capture_begin (see the definition). */
void gfx_dkr_reset_interpreter_state(void);

/**
 * Replay telemetry.
 *  walks/real_walks — replay walks vs real gfx_run walks (graphics tasks). DKR
 *      does not submit a task every frame, so real_walks, not the tick count,
 *      is what walks must equal.
 *  matrix_hits    — G_MTX commands recomposed from a decomposition that proved
 *                   faithful against the display list.
 *  matrix_rejects — commands whose decomposition did NOT reproduce the list and
 *                   therefore kept the list's own matrix.
 *  matrix_misses  — commands with no registry entry at all.
 */
/**
 * Retire the held display list. A stage boundary frees the memory it points
 * into, and a replay of a freed list walks garbage.
 */
void gfx_dkr_replay_invalidate(void);

/**
 * Real gfx_run walks (graphics tasks) since process start.
 *
 * The presentation subloop MUST consult this before replaying. DKR skips the
 * graphics task on some passes (gDrawFrameTimer, thread3_main.c), and on such a
 * pass gSPTaskNum does NOT flip — so the game rebuilds its display list into the
 * very buffer that was last walked. Replaying then walks a half-written list.
 * That is a segfault, not a glitch, and it is what this counter exists to
 * prevent: replay only when a NEW list was walked this pass.
 */
uint64_t gfx_dkr_real_walk_count(void);
/* Authored simulation tick stamped onto the most recent real graphics task. */
uint64_t gfx_dkr_last_walked_authored_tick(void);

/**
 * Registry hits refused because the Mtx address had been rewritten since
 * registration -- a dead tenant (see dkr_shadow_lookup_live in gfx_pc_dkr.c).
 *
 * Non-zero is NORMAL and expected: three slot-2 pushers in camera.c reuse
 * matrix arena addresses without registering, and refusing their predecessors'
 * worlds is exactly what this guard is for. The number is published so a gate
 * can prove the guard is still FIRING -- a sudden zero would mean either the
 * pushers stopped reusing addresses or the tenancy check stopped working, and
 * the second of those silently restores the wrong-caster defect.
 */
uint64_t gfx_dkr_shadow_stale_tenant_count(void);

/* dkr_replay_object_holds split by cause. Only `moving` -- a continuous
 * object that moved between the two authoritative ticks and still held -- is a
 * defect signal; the rest are holds that are correct. */
void gfx_dkr_replay_get_object_hold_classes(
    uint64_t *no_pair, uint64_t *discontinuous, uint64_t *still,
    uint64_t *moving);

void gfx_dkr_replay_get_stats(
    uint64_t *walks, uint64_t *matrix_hits, uint64_t *matrix_misses,
    uint64_t *matrix_rejects, uint64_t *real_walks);
void gfx_dkr_replay_get_object_stats(uint64_t *hits, uint64_t *holds);

/**
 * Uncaptured-external fail-closed evidence.
 *
 * `externals` counts non-arena pointers an INTERPOLATED walk resolved without a
 * retained copy; `refusals` counts the walks those aborted. Both are expected
 * to be zero on a correct tree — every shipped handler that reads a non-arena
 * dependency also captures it — so a non-zero reading is the signature of a
 * handler that reads mutable memory the replay does not own. That is the exact
 * hazard `PRES-001` describes and the one the live-arena poison gate cannot
 * observe, because its poison covers the arena only.
 */
void gfx_dkr_replay_get_uncaptured_stats(uint64_t *externals,
                                         uint64_t *refusals,
                                         uint64_t *zero_alpha_interior);
void gfx_dkr_replay_get_billboard_stats(
    uint64_t *matrix_hits, uint64_t *matrix_holds,
    uint64_t *vertex_hits, uint64_t *vertex_holds);

/**
 * Recomposition-tolerance evidence (Phase 3 Wave B, §12.4).
 *
 * `matrix_hits` above counts every accepted matrix; these split the accepted
 * set's tolerant tail out of it and report the magnitudes on both sides of the
 * threshold, so the separation margin between benign float re-association and a
 * genuine mis-association is measured per run rather than assumed.
 */
void gfx_dkr_replay_get_reject_stats(
    uint64_t *tolerant, uint64_t *tolerant_worst, uint64_t *reject_least,
    bool *reject_least_valid);

#ifdef __cplusplus
}
#endif

#endif /* GFX_PC_DKR_H */
